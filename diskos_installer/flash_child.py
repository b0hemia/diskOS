# SPDX-License-Identifier: GPL-3.0-or-later
"""Private, stdin-gated Windows tool entry; never parses installer commands."""
import json
import ctypes
import os
import subprocess
import sys
import re

CHILD_MODE = "--_diskos-flash-child"
# Private wrapper result: validation ended before the native invocation.
VALIDATION_REFUSED = 65


def validate_command(cmd):
    """An explicit usbboot argv only: no shell, code, PATH lookup or empty args."""
    if (not isinstance(cmd, list) or not 2 <= len(cmd) <= 128
            or any(not isinstance(arg, str) or not arg or "\x00" in arg
                   or "\r" in arg or "\n" in arg for arg in cmd)
            or not os.path.isabs(cmd[0])
            or os.path.basename(cmd[0]).lower() != "usbboot.exe"):
        raise ValueError("expected an absolute usbboot.exe path and string arguments")
    flags = {"-v", "--help"}
    values = {"--cpu", "--stage1", "--wait", "--addr", "--download",
              "--start1", "--length", "--upload"}
    i = 1
    while i < len(cmd):
        option = cmd[i]
        if option in flags:
            i += 1
        elif option in values and i + 1 < len(cmd):
            value = cmd[i + 1]
            if option == "--cpu":
                valid = value == "x2000"
            elif option in {"--stage1", "--download", "--upload"}:
                valid = os.path.isabs(value)
            else:
                try:
                    valid = 0 <= int(value, 16 if value.lower().startswith("0x") else 10) <= 0xffffffff
                except ValueError:
                    valid = False
            if not valid:
                raise ValueError("invalid usbboot option value")
            i += 2
        else:
            raise ValueError("unknown or incomplete usbboot option")
    return cmd


class JobLimits(ctypes.Structure):
    class Basic(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                    ("flags", ctypes.c_uint32), ("min_ws", ctypes.c_size_t),
                    ("max_ws", ctypes.c_size_t), ("active", ctypes.c_uint32),
                    ("affinity", ctypes.c_size_t), ("priority", ctypes.c_uint32),
                    ("scheduling", ctypes.c_uint32)]
    _fields_ = [("basic", Basic), ("io", ctypes.c_uint64 * 6),
                ("memory", ctypes.c_size_t * 4)]


def windows_api():
    from ctypes import wintypes as w
    from diskos_installer import bundle
    api = bundle._windows_api()
    signatures = {
        "SetHandleInformation": ([w.HANDLE, w.DWORD, w.DWORD], w.BOOL),
        "ResumeThread": ([w.HANDLE], w.DWORD),
        "GetCurrentProcess": ([], w.HANDLE),
        "GetStdHandle": ([w.DWORD], w.HANDLE),
        "GetNamedPipeServerProcessId": ([w.HANDLE, ctypes.POINTER(w.ULONG)], w.BOOL),
        "OpenProcess": ([w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
        "QueryFullProcessImageNameW": ([w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)], w.BOOL),
        "IsProcessInJob": ([w.HANDLE, w.HANDLE, ctypes.POINTER(w.BOOL)], w.BOOL),
        "QueryInformationJobObject": ([w.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                       w.DWORD, ctypes.c_void_p], w.BOOL),
    }
    for method, (args, result) in signatures.items():
        fn = getattr(api, method)
        fn.argtypes, fn.restype = args, result
    return api


def launch_argv(nonce):
    if getattr(sys, "frozen", False):
        return [sys.executable, CHILD_MODE, nonce]
    return [sys.executable, "-I", os.path.abspath(__file__), CHILD_MODE, nonce]


def verify_supervision(parent_pid, api=None):
    """Query current membership without opening or keeping a job handle.

    Bind the private pipe to the original installer process, including when a
    onefile launcher has spawned its own Python process inside the job.
    """
    from ctypes import wintypes as w
    api = api or windows_api()
    member, limits = w.BOOL(), JobLimits()
    if (not api.IsProcessInJob(api.GetCurrentProcess(), None, ctypes.byref(member))
            or not member.value
            or not api.QueryInformationJobObject(None, 9, ctypes.byref(limits),
                                                 ctypes.sizeof(limits), None)):
        raise OSError("child is not in a verified parent job")
    flags = limits.basic.flags
    if not flags & 0x2000 or flags & (0x800 | 0x1000):
        raise OSError("parent job must kill on close and forbid breakaway")
    server = w.ULONG()
    if (not api.GetNamedPipeServerProcessId(api.GetStdHandle(-10), ctypes.byref(server))
            or server.value != parent_pid):
        raise OSError("private gate is not the parent pipe")
    parent = api.OpenProcess(0x1000 | 0x100000, False, parent_pid)
    if not parent:
        raise OSError("parent is unavailable")
    try:
        image, own_image = ctypes.create_unicode_buffer(32768), ctypes.create_unicode_buffer(32768)
        length, own_length = w.DWORD(32768), w.DWORD(32768)
        if (api.WaitForSingleObject(parent, 0) != 258
                or not api.QueryFullProcessImageNameW(parent, 0, image, ctypes.byref(length))
                or not api.QueryFullProcessImageNameW(api.GetCurrentProcess(), 0, own_image,
                                                     ctypes.byref(own_length))
                or os.path.normcase(os.path.realpath(image.value)) !=
                   os.path.normcase(os.path.realpath(own_image.value))):
            raise OSError("private gate has no live installer parent")
    finally:
        api.CloseHandle(parent)


def _long_path(path):
    """Expand an existing Windows spelling without resolving reparse points."""
    from ctypes import wintypes
    api = ctypes.WinDLL("kernel32", use_last_error=True).GetLongPathNameW
    api.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
    api.restype = wintypes.DWORD
    size = api(path, None, 0)
    if not size:
        raise OSError("GetLongPathNameW sizing failed")
    # Retry once if the required size grew between the query and the copy.
    for _ in range(2):
        output = ctypes.create_unicode_buffer(size)
        copied = api(path, output, size)
        if not copied:
            raise OSError("GetLongPathNameW failed")
        if copied < size:
            if not output.value or not os.path.isabs(output.value):
                raise OSError("GetLongPathNameW returned an invalid path")
            return output.value
        size = copied
    raise OSError("GetLongPathNameW size did not stabilize")


def _checked_spelling(path):
    """Inspect original components first; return a normalized expanded spelling."""
    import stat
    stage = "component inspection"
    try:
        current = path
        while True:
            st = os.lstat(current)
            if stat.S_ISLNK(st.st_mode) or getattr(st, "st_file_attributes", 0) & 0x400:
                raise ValueError("symlinks and reparse points are refused")
            above = os.path.dirname(current)
            if above == current:
                break
            current = above
        stage = "long-name expansion"
        expanded = _long_path(path) if os.name == "nt" else path
        stage = "final-path comparison"
        normalized = os.path.normcase(expanded)
        if os.path.normcase(os.path.realpath(expanded, strict=True)) != normalized:
            raise ValueError("canonical path differs from expanded path")
        return normalized
    except (OSError, ValueError) as exc:
        raise ValueError(f"{path!r}: {stage}: {exc}") from exc


def checked_path(path, root=None):
    """Validate aliases and containment, retaining the authorized argv spelling."""
    path = os.path.abspath(path)
    expanded = _checked_spelling(path)
    if root is not None:
        expanded_root = _checked_spelling(os.path.abspath(root))
        try:
            if os.path.commonpath([expanded, expanded_root]) != expanded_root:
                raise ValueError("path is outside the bundle")
        except ValueError as exc:
            raise ValueError(f"{path!r}: bundle containment: {exc}") from exc
    return path


def bundled_tool():
    from diskos_installer import bundle
    root = checked_path(bundle.resource_root())
    vendor = checked_path(bundle.vendor_dir(), root)
    tool = checked_path(os.path.join(vendor, "usbboot.exe"), root)
    checked_path(os.path.join(vendor, "MANIFEST.txt"), root)
    checked_path(os.path.join(root, "src", "usbboot", "usbboot.c"), root)
    with os.scandir(vendor) as entries:
        for entry in entries:
            if entry.name.lower().endswith(".dll"):
                checked_path(entry.path, root)
    bundle.verify_windows_flash_bundle()
    return tool, bundle.native_env(tool)  # existing recursive PE/DLL closure


def authorized_command(request):
    """Admit the normal flash recipe only, with all existing gates repeated."""
    from diskos_installer import basegate, bundle, flasher
    cmd = validate_command(request["cmd"])
    tool, env = bundled_tool()
    # Never execute a caller's path, even when it happens to have the right name.
    if cmd[0] != tool:
        raise ValueError("caller executable is not the bundled usbboot")
    target = request["target"]
    if target not in basegate.CATALOGUE or len(cmd) != 34:
        raise ValueError("not a supported flash operation")
    image, plan, poison, dbg = cmd[15], cmd[19], cmd[23], cmd[33]
    root = os.path.dirname(plan)
    for path in (image, plan, poison):
        checked_path(path)
    checked_path(root)
    if [plan, poison, dbg] != [os.path.join(root, name) for name in ("plan.bin", "poison.bin", "dbg.bin")]:
        raise ValueError("invalid flash workspace")
    if os.path.lexists(dbg):
        raise ValueError("readback must be a new file")
    writer = checked_path(bundle.native(flasher.WRITER_NAME), bundle.resource_root())
    spl = checked_path(bundle.native("disc_spl_lpddr3.bin"), bundle.resource_root())
    if flasher._writer_descriptor(writer) != (flasher.IMG_SIZE // (128 * 1024), basegate.BLOB_SCHEMA):
        raise ValueError("writer identity or capacity is invalid")
    expected = flasher._flash_command(tool, spl, writer, image, plan, poison, dbg)
    if cmd != expected:
        raise ValueError("arguments differ from the authorized flash recipe")
    with open(plan, "rb") as f:
        if f.read() != basegate.build_plan(target, image):
            raise ValueError("plan does not bind this image and firmware base")
    with open(poison, "rb") as f:
        if f.read() != b"\xee" * 128:
            raise ValueError("invalid readback poison")
    with flasher.windows_dev_flash(request["dev_requested"]):
        flasher.preflight(image)
    return expected, env


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if (sys.platform != "win32" or len(argv) != 2 or argv[0] != CHILD_MODE
            or not re.fullmatch(r"[0-9a-f]{64}", argv[1])):
        print("Windows flash child: arguments refused", file=sys.stderr, flush=True)
        return 2
    try:
        if sys.stdin.buffer.read(1) != b"G":
            raise ValueError("assignment gate closed")
        raw = sys.stdin.buffer.read(65537)
        if len(raw) > 65536:
            raise ValueError("oversized parent capability")
        request = json.loads(raw)
        if (not isinstance(request, dict) or set(request) !=
                {"nonce", "parent", "dev_requested", "cmd", "target"}
                or request["nonce"] != argv[1] or type(request["parent"]) is not int
                or request["parent"] <= 0 or type(request["dev_requested"]) is not bool):
            raise ValueError("invalid parent capability")
        verify_supervision(request["parent"])
        cmd, env = authorized_command(request)
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f"Windows flash child: refused: {exc}", file=sys.stderr, flush=True)
        return VALIDATION_REFUSED
    except Exception as exc:
        # Coded bundle/preflight errors must also fail closed in private dispatch.
        print(f"Windows flash child: validation failed: {exc}", file=sys.stderr, flush=True)
        return VALIDATION_REFUSED

    # No error after this boundary may claim a validation refusal.
    try:
        print("Windows flash child: assignment gate passed", flush=True)
        rc = subprocess.call(cmd, env=env, stdin=subprocess.DEVNULL, shell=False)
        return 1 if rc == VALIDATION_REFUSED else rc
    except Exception as exc:
        print(f"Windows flash child: native invocation failed: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    # Isolated source launch: import only this installer's package, never cwd/PATH.
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    sys.exit(main())
