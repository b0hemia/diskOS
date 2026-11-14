# SPDX-License-Identifier: GPL-3.0-or-later
"""Windowed entry: establish streams before importing the installer."""
from contextlib import ExitStack
import ctypes
import errno
import io
import os
import stat
import sys
import threading
import traceback

CHILD_MODE = "--_diskos-flash-child"
FETCH_MODE = "--_diskos-fetch-helper"  # private firmware-acquisition helper; never the flash entry
LOG_FOLDER = "diskos-installer-logs"
LOG_MARKER = ".diskos-gui-logs"
LOG_MAGIC = b"diskOS GUI diagnostics v1\n"
LOG_SLOTS = 4
LOG_LIMIT = 64 * 1024
LOG_NAMES = tuple(f"gui-{n}.log" for n in range(LOG_SLOTS))


class _LogBusy(OSError):
    def __init__(self):
        super().__init__("diskOS Installer is already open. Close it and retry.")


def _lock_log(fd):
    import fcntl
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as exc:
        if exc.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
            raise _LogBusy() from exc
        raise


def _child_stream(number, reading):
    """Own a non-inheritable duplicate, never the parent's inherited handle."""
    import _winapi as win
    import msvcrt
    handle = win.GetStdHandle(number)
    if handle in (None, 0, -1, ctypes.c_void_p(-1).value):
        raise OSError("missing inherited child standard handle")
    current = win.GetCurrentProcess()
    copy = win.DuplicateHandle(current, handle, current, 0, False, win.DUPLICATE_SAME_ACCESS)
    try:
        fd = msvcrt.open_osfhandle(copy, os.O_BINARY | (os.O_RDONLY if reading else os.O_WRONLY))
    except BaseException:
        win.CloseHandle(copy)
        raise
    try:
        os.set_inheritable(fd, False)
        raw = os.fdopen(fd, "rb" if reading else "wb")
    except BaseException:
        os.close(fd)
        raise
    try:
        return io.TextIOWrapper(raw, encoding="utf-8", errors="backslashreplace",
                                line_buffering=not reading)
    except BaseException:
        raw.close()
        raise


def _check_node(info, directory):
    if (stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400
            or not (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode))):
        raise OSError("redirected or unexpected log object")
    if not directory and info.st_nlink != 1:
        raise OSError("hard-linked log object")


def _mountpoint(path, info, parent, mounts):
    return info.st_dev != parent.st_dev or path in mounts or os.path.ismount(path)


class _WindowsIO:
    """Pin names without delete sharing; inspect reparse points on the handle."""
    def __init__(self):
        from ctypes import wintypes as w
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "CreateFileW": ([w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                             w.DWORD, w.DWORD, w.HANDLE], w.HANDLE),
            "GetFileInformationByHandle": ([w.HANDLE, ctypes.c_void_p], w.BOOL),
            "GetFileInformationByHandleEx": ([w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD], w.BOOL),
            "SetFileInformationByHandle": ([w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD], w.BOOL),
            "CloseHandle": ([w.HANDLE], w.BOOL),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.api, name)
            fn.argtypes, fn.restype = args, result
        # BY_HANDLE_FILE_INFORMATION consists of thirteen 32-bit words.
        self.info_type = ctypes.c_uint32 * 13

    def open(self, path, directory=False, create=False, removing=False, expected=None):
        access = 0x80 if directory else 0xC0000000
        if removing:
            access |= 0x10000  # DELETE, for handle-based cleanup
        handle = self.api.CreateFileW(path, access, 3 if directory else 1, None,
                                      1 if create else 3, 0x02200000, None)
        if handle in (None, ctypes.c_void_p(-1).value):
            error = ctypes.get_last_error()
            if not directory and error == 32:  # ERROR_SHARING_VIOLATION
                raise _LogBusy()
            raise ctypes.WinError(error)
        try:
            info = self.info_type()
            if not self.api.GetFileInformationByHandle(handle, ctypes.byref(info)):
                raise ctypes.WinError(ctypes.get_last_error())
            if info[0] & 0x400 or bool(info[0] & 0x10) != directory:
                raise OSError("redirected or unexpected log handle")
            if not directory and info[10] != 1:
                raise OSError("hard-linked log handle")
            if expected is not None:
                identity = (info[11] << 32) | info[12]
                if expected.st_ino > 0xFFFFFFFFFFFFFFFF:
                    # Newer CPython exposes the full FILE_ID_128 on Windows.
                    full = (ctypes.c_ubyte * 24)()
                    if not self.api.GetFileInformationByHandleEx(handle, 18, ctypes.byref(full), 24):
                        raise ctypes.WinError(ctypes.get_last_error())
                    identity = int.from_bytes(bytes(full)[8:], "little")
                if identity != expected.st_ino:
                    raise OSError("log object changed while opening")
            return handle
        except BaseException:
            self.api.CloseHandle(handle)
            raise

    def delete(self, handle):
        flag = ctypes.c_ubyte(1)  # FILE_DISPOSITION_INFO.DeleteFile
        if not self.api.SetFileInformationByHandle(handle, 4, ctypes.byref(flag), 1):
            raise ctypes.WinError(ctypes.get_last_error())


class _LogDirectory:
    """No recursive operations. POSIX uses pinned dir_fd; Windows pins ancestors."""
    def __init__(self, create=False, removing=False):
        self.stack = ExitStack()
        try:
            base = os.environ.get("LOCALAPPDATA")
            if not base or not os.path.isabs(base) or ".." in base.replace("\\", "/").split("/"):
                raise OSError("LOCALAPPDATA must name an absolute per-user directory")
            self.path = os.path.join(os.path.normpath(base), LOG_FOLDER)
            self.win = _WindowsIO() if os.name == "nt" else None
            if self.win:
                drive, tail = os.path.splitdrive(self.path)
                if len(drive) != 2 or drive[1] != ":":
                    raise OSError("log directory must be on a local drive")
                current = drive + os.sep
                self.stack.callback(self.win.api.CloseHandle, self.win.open(current, directory=True))
                for component in tail.strip(os.sep).split(os.sep):
                    current = os.path.join(current, component)
                    try:
                        info = os.lstat(current)
                    except FileNotFoundError:
                        if not create:
                            raise
                        try:
                            os.mkdir(current)
                        except FileExistsError:
                            pass
                        info = os.lstat(current)
                    _check_node(info, True)
                    handle = self.win.open(current, directory=True,
                                           removing=removing and current == self.path, expected=info)
                    self.stack.callback(self.win.api.CloseHandle, handle)
                self.directory = handle
            else:
                # Linux bind mounts can keep st_dev unchanged. Read their actual
                # mount table too; other POSIX hosts use ismount and device IDs.
                mounts = set()
                if sys.platform.startswith("linux"):
                    with open("/proc/self/mountinfo", encoding="ascii") as table:
                        for line in table:
                            value = line.split()[4]
                            for escaped, plain in (("\\040", " "), ("\\011", "\t"),
                                                   ("\\012", "\n"), ("\\134", "\\")):
                                value = value.replace(escaped, plain)
                            mounts.add(value)
                self.mounts = mounts
                flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
                parent = os.open(os.sep, flags)
                self.stack.callback(os.close, parent)
                current = os.sep
                for component in self.path.strip(os.sep).split(os.sep):
                    current = os.path.join(current, component)
                    if create:
                        try:
                            os.mkdir(component, 0o700, dir_fd=parent)
                        except FileExistsError:
                            pass
                    before = os.stat(component, dir_fd=parent, follow_symlinks=False)
                    _check_node(before, True)
                    fd = os.open(component, flags, dir_fd=parent)
                    self.stack.callback(os.close, fd)
                    after = os.fstat(fd)
                    if ((before.st_dev, before.st_ino) != (after.st_dev, after.st_ino)
                            or _mountpoint(current, after, os.fstat(parent), mounts)):
                        raise OSError("log directory changed or is a mount point")
                    self.parent, parent = parent, fd
                self.directory = parent
                info = os.fstat(parent)
                if info.st_uid != os.getuid() or info.st_mode & 0o022:
                    raise OSError("log directory is not private to this user")
                # The directory lock survives marker unlink during Remove, so
                # a simultaneous launch cannot adopt the half-removed folder.
                _lock_log(parent)
            self.removing = removing
        except BaseException:
            self.stack.close()
            raise

    def names(self):
        return set(os.listdir(self.path if self.win else self.directory))

    def open_file(self, name, create=False):
        path = os.path.join(self.path, name)
        before = None
        try:
            before = os.lstat(path) if self.win else os.stat(
                name, dir_fd=self.directory, follow_symlinks=False)
            _check_node(before, False)
        except FileNotFoundError:
            if not create:
                raise
        if self.win:
            import msvcrt
            handle = self.win.open(path, create=before is None, removing=self.removing, expected=before)
            try:
                fd = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY)
            except BaseException:
                self.win.api.CloseHandle(handle)
                raise
        else:
            flags = os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
            if before is None:
                flags |= os.O_CREAT | os.O_EXCL
            fd = os.open(name, flags, 0o600, dir_fd=self.directory)
        try:
            after = os.fstat(fd)
            _check_node(after, False)
            if before and (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino):
                raise OSError("log file changed while opening")
            if not self.win and (after.st_uid != os.getuid() or after.st_mode & 0o077):
                raise OSError("log file is not private to this user")
            if not self.win and _mountpoint(path, after, os.fstat(self.directory), self.mounts):
                raise OSError("log file is a mount point")
            return self.stack.enter_context(os.fdopen(fd, "r+b", buffering=0))
        except BaseException:
            os.close(fd)
            raise

    def owned_files(self, create=False):
        names = self.names()
        if names - {LOG_MARKER, *LOG_NAMES}:
            raise OSError("unrecognized files in log directory; refusing to adopt or remove")
        marker = self.open_file(LOG_MARKER, create=create and not names)
        if not names:
            marker.write(LOG_MAGIC)
            marker.seek(0)
        if not self.win:
            _lock_log(marker.fileno())
        if marker.read(len(LOG_MAGIC) + 1) != LOG_MAGIC:
            raise OSError("unrecognized log ownership marker")
        files = {name: self.open_file(name) for name in LOG_NAMES if name in names}
        if any(os.fstat(file.fileno()).st_size > LOG_LIMIT for file in files.values()):
            raise OSError("oversized log file; refusing to adopt")
        return marker, files

    def close(self):
        self.stack.close()


class _DiagnosticLog(io.TextIOBase):
    """Drop raw output. Only explicit bootstrap events reach the bounded file."""
    _diskos_log = True

    def __init__(self, directory, files, slot, history=None):
        self.directory, self.files, self.slot = directory, files, slot
        self.lock = threading.RLock()
        self._reset(history)

    def _reset(self, history=None):
        # Newest chunk is always slot 0. Copy through the pinned handles;
        # timestamp ties cannot cause the same history to be discarded twice.
        names = [name for name in LOG_NAMES if name in self.files]
        migrating = history is not None
        if history is None:
            history = []
            for name in names[:-1]:
                file = self.files[name]
                file.seek(0)
                history.append(file.read(LOG_LIMIT))
        def copy_chunk(name, data):
            file = self.files[name]
            file.seek(0)
            remaining = memoryview(data)
            while remaining:
                written = file.write(remaining)
                if written is None or written <= 0:
                    raise OSError("incomplete diagnostic log copy")
                remaining = remaining[written:]
            file.truncate(len(data))

        # Migration can put the newest source in a slot we overwrite first.
        # Keep a complete copy before shifting anything; write it again last
        # so its timestamp still sorts ahead of the older history.
        if migrating and history:
            copy_chunk(names[1], history[0])
        for name, data in reversed(list(zip(names[1:], history))):
            copy_chunk(name, data)
        self.file = self.files[LOG_NAMES[self.slot]]
        self.file.seek(0)
        self.file.truncate(0)

    def write(self, value):
        return len(value)  # No exception text, device output, or user paths.

    def flush(self):
        pass  # Binary writes are unbuffered.

    def event(self, value):
        with self.lock:
            if self.closed:
                return
            data = (value + "\n").encode("ascii")
            if len(data) > LOG_LIMIT:
                raise ValueError("diagnostic event too large")
            if self.file.tell() + len(data) > LOG_LIMIT:
                self._reset()
            self.file.write(data)

    def close(self):
        with self.lock:
            if not self.closed:
                super().close()
                self.directory.close()


def _open_log():
    # A sibling of state: bootstrap must not mark fresh state as foreign.
    directory = _LogDirectory(create=True)
    try:
        _, files = directory.owned_files(create=True)
        # Preserve timestamp order from older installers. Ties use the fixed
        # newest-first slot order maintained by this implementation.
        ordered = sorted(files, key=lambda name: (
            -os.fstat(files[name].fileno()).st_mtime_ns, LOG_NAMES.index(name)))
        history = []
        for name in ordered[:LOG_SLOTS - 1]:
            files[name].seek(0)
            history.append(files[name].read(LOG_LIMIT))
        # Open the whole ring before writing; hold every identity until close.
        for name in LOG_NAMES:
            if name not in files:
                files[name] = directory.open_file(name, create=True)
        log = _DiagnosticLog(directory, files, 0, history)
        return log, os.path.join(directory.path, LOG_NAMES[0])
    except BaseException:
        directory.close()
        raise


def remove_logs():
    """Close bootstrap handles, then delete only validated, installer-owned logs."""
    for stream in (sys.stdout, sys.stderr):
        if getattr(stream, "_diskos_log", False):
            stream.close()
    # Only the Windows windowed bootstrap uses LOCALAPPDATA. Native POSIX
    # frontends never create this sibling directory.
    if os.name != "nt":
        return
    try:
        directory = _LogDirectory(removing=True)
    except FileNotFoundError:
        return
    try:
        marker, files = directory.owned_files()
        import msvcrt
        for file in [*files.values(), marker]:
            directory.win.delete(msvcrt.get_osfhandle(file.fileno()))
            file.close()
        directory.win.delete(directory.directory)
    finally:
        directory.close()


def _show_error(message):
    # Native dialog still works when Python/Tk imports or Tk startup fail.
    from ctypes import wintypes as w
    dialog = ctypes.WinDLL("user32", use_last_error=True).MessageBoxW
    dialog.argtypes = [w.HWND, w.LPCWSTR, w.LPCWSTR, w.UINT]
    dialog.restype = ctypes.c_int
    dialog(None, message, "diskOS Installer", 0x10010)


def _run_installer(argv):
    from diskos_installer.__main__ import main
    return main(argv)


def _run_child(argv):
    from diskos_installer.flash_child import main
    return main(argv)


def _run_fetch_helper(argv):
    from diskos_installer.firmware_fetch import helper_main
    return helper_main(argv)


def _failure_event(log, exc):
    # Exception messages, source lines, filenames and locals are private. Keep
    # only known exception kinds, installer error codes and bootstrap line IDs.
    kinds = {"ImportError", "ModuleNotFoundError", "OSError", "PermissionError",
             "RuntimeError", "ValueError", "TypeError", "SystemExit"}
    kind = type(exc).__name__
    log.event("failure " + (kind if kind in kinds else "Exception"))
    code = getattr(exc, "code", None)
    if isinstance(code, str) and len(code) == 4 and code[0] in "EF" and code[1:].isascii() and code[1:].isdigit():
        log.event("error " + code)
    for frame, line in traceback.walk_tb(exc.__traceback__):
        # Only this fixed, known module's line numbers; no arbitrary names.
        if frame.f_code.co_filename == __file__:
            log.event(f"bootstrap line {line}")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    child = bool(argv and argv[0] == CHILD_MODE)
    fetch = bool(argv and argv[0] == FETCH_MODE)
    saved = sys.stdin, sys.stdout, sys.stderr
    log_path = "(could not create a user log)"
    try:
        with ExitStack() as stack:
            try:
                if fetch:  # headless helper: own pipes, no log, no GUI, no recursion
                    if os.name == "nt":
                        streams = [stack.enter_context(_child_stream(n, n == -10)) for n in (-10, -11, -12)]
                        sys.stdin, sys.stdout, sys.stderr = streams
                    return _run_fetch_helper(argv)
                if child:
                    streams = [stack.enter_context(_child_stream(n, n == -10)) for n in (-10, -11, -12)]
                    sys.stdin, sys.stdout, sys.stderr = streams
                    return _run_child(argv)
                log, log_path = _open_log()
                stack.enter_context(log)
                sys.stdout = sys.stderr = log
                sys.stdin = stack.enter_context(open(os.devnull, "r", encoding="utf-8"))
                log.event("diskOS Installer GUI startup")
                result = _run_installer(argv)
                if result in (None, 0):
                    log.event("installer returned success")
                    return 0
                log.event("installer returned failure")
                reason = "Installer returned a failure status."
            except (Exception, SystemExit) as exc:
                if isinstance(exc, SystemExit) and exc.code in (None, 0):
                    return 0
                if not child and isinstance(exc, _LogBusy):
                    _show_error(str(exc))
                    return 1
                reason = str(exc) or type(exc).__name__
                if (child or fetch) and sys.stderr is not None:
                    try:
                        traceback.print_exc(file=sys.stderr)
                        sys.stderr.flush()
                    except Exception:
                        pass
                elif isinstance(sys.stderr, _DiagnosticLog):
                    try:
                        _failure_event(sys.stderr, exc)
                    except Exception:
                        pass  # Diagnostics must not prevent the startup dialog.
            if child or fetch:
                return 1  # Missing pipes never fall back to a GUI or a null gate.
            _show_error(f"Could not start diskOS Installer:\n{reason}\n\nDetails: {log_path}\n"
                        "Use diskos-installer-cli.exe doctor for diagnostics.")
            return 1
    finally:
        sys.stdin, sys.stdout, sys.stderr = saved


if __name__ == "__main__":
    sys.exit(main())
