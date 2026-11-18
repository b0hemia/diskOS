# SPDX-License-Identifier: GPL-3.0-or-later
"""Local fallbacks when the download cannot be used: Downloads scan and browser page.

Nothing here touches the network, and nothing runs on import. The scanner never
modifies, moves or deletes the user's file: a verified match is copied into the
owned cache (under FiiO's own filename) by firmware_catalogue.store(), which
re-verifies the copied bytes before publishing. The browser is opened only when
open_download_page() is called, i.e. on an explicit button press.
"""
import os
import re
import stat
import sys
import webbrowser
from typing import NamedTuple

from . import errors, firmware_catalogue

# FiiO's own download thread; it lists every supported version.
OFFICIAL_PAGE = 'https://forum.fiio.com/note/showNoteContent.do?id=202601311712087234434'
_PARTIAL_SUFFIXES = ('.part', '.crdownload', '.download', '.opdownload', '.tmp', '.partial', '.!ut')


class LocalError(errors.DiskOSError):
    code = 'E175'


class _Cancelled(Exception):
    pass


class ScanResult(NamedTuple):
    """kind: "found" (`path` = verified cache copy, `source` = the user's untouched file),
    "none", "no_folder", "denied" (both recoverable: `detail` says why) or "cancelled"."""
    kind: str
    path: str = None
    source: str = None
    detail: str = None


class BrowserResult(NamedTuple):
    """ok False: no browser could be opened; show `url` (selectable) to the user."""
    ok: bool
    url: str
    error: str = None


def _unquote(value):
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        return re.sub(r'\\(.)', r'\1', value[1:-1])
    return value


def _xdg_download(environ, home, read=None):
    config = environ.get('XDG_CONFIG_HOME')
    if not config or not os.path.isabs(config):
        config = os.path.join(home, '.config')
    try:
        if read is None:
            with open(os.path.join(config, 'user-dirs.dirs'), encoding='utf-8',
                      errors='surrogateescape') as stream:
                text = stream.read()
        else:
            text = read(os.path.join(config, 'user-dirs.dirs'))
    except OSError:
        return None
    found = None
    for line in text.splitlines():
        match = re.match(r'\s*XDG_DOWNLOAD_DIR\s*=\s*(.*)$', line)
        if match:
            found = _unquote(match.group(1))
    if not found:
        return None
    if found == '$HOME' or found.startswith('$HOME/'):
        found = home + found[5:]
    if not os.path.isabs(found) or os.path.normpath(found) == os.path.normpath(home):
        return None  # xdg-user-dirs: a directory equal to $HOME means "unset"
    return os.path.normpath(found)


def _windows_known_folder():
    """The (possibly redirected) Downloads known folder, or None."""
    import ctypes
    from ctypes import wintypes
    guid = ctypes.c_byte * 16
    # FOLDERID_Downloads {374DE290-123F-4565-9164-39C4925E467B}
    folder = guid(*bytes.fromhex('90e24d37' '3f12' '6545' '9164' '39c4925e467b'))
    out = ctypes.c_wchar_p()
    shell = ctypes.windll.shell32
    shell.SHGetKnownFolderPath.argtypes = [ctypes.c_void_p, wintypes.DWORD, wintypes.HANDLE,
                                           ctypes.POINTER(ctypes.c_wchar_p)]
    if shell.SHGetKnownFolderPath(ctypes.byref(folder), 0, None, ctypes.byref(out)) != 0:
        return None
    try:
        return out.value
    finally:
        ctypes.windll.ole32.CoTaskMemFree(out)


def downloads_dir(platform=None, environ=None, home=None, known_folder=_windows_known_folder,
                  read=None):
    """Path of the user's Downloads folder (it may not exist; the scanner reports that).

    Windows: the known folder (follows a redirected Downloads), else %USERPROFILE%\\Downloads.
    macOS: ~/Downloads. Linux: XDG_DOWNLOAD_DIR from user-dirs.dirs, else ~/Downloads."""
    platform = sys.platform if platform is None else platform
    environ = os.environ if environ is None else environ
    home = home or os.path.expanduser('~')
    if platform == 'win32':
        try:
            path = known_folder()
        except Exception:
            path = None
        if path and os.path.isabs(path):
            return path
        return os.path.join(environ.get('USERPROFILE') or home, 'Downloads')
    if platform != 'darwin':
        path = _xdg_download(environ, home, read)
        if path:
            return path
    return os.path.join(home, 'Downloads')


def _signature(st):
    return st.st_size, st.st_mtime_ns, st.st_ino, st.st_dev


def _inspect(path, pin, entry_stat, cancel, output=None):
    """True if `path` hashes to the pin and did not change meanwhile; False otherwise."""
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0) \
        | getattr(os, 'O_NONBLOCK', 0)
    fd = os.open(path, flags)
    with os.fdopen(fd, 'rb') as stream:
        opened = os.fstat(stream.fileno())
        if not stat.S_ISREG(opened.st_mode) or _signature(opened) != _signature(entry_stat):
            return False
        try:
            firmware_catalogue._verify_stream(stream, pin, output, check=cancel)
        except ValueError:
            return False
        after = os.fstat(stream.fileno())
    return _signature(after) == _signature(entry_stat) == _signature(os.lstat(path))

def _copy_candidate(path, version, pin, st, run_lock, check, catalogue):
    check()
    partial = catalogue.new_partial(version, run_lock)
    try:
        with open(partial, 'wb') as output:
            if not _inspect(path, pin, st, check, output):
                return None
            output.flush()
            os.fsync(output.fileno())
        return catalogue.promote(partial, version, run_lock, check=check)
    finally:
        if os.path.exists(partial):
            os.unlink(partial)


def scan_downloads(version, directory=None, run_lock=None, cancel=None,
                   catalogue=firmware_catalogue, locate=downloads_dir):
    """Look in Downloads (nonrecursive) for the official ZIP of `version`, whatever it is named.

    `cancel()` returns true to stop; it is polled per file and per chunk. Only regular non-symlink files of the pinned size are hashed; browser partial files and
    files whose size/mtime/inode change during inspection are skipped. A missing or unreadable
    folder is a recoverable result. On a match the file is copied into the cache and the copy is
    verified; the user's file is never changed."""
    if version not in catalogue.CATALOGUE:
        raise LocalError('No supported firmware version %r' % (version,))
    pin = catalogue.CATALOGUE[version]

    def check():
        if cancel is not None and cancel():
            raise _Cancelled()

    try:
        check()
        directory = directory or locate()
        if not os.path.isdir(directory):
            return ScanResult('no_folder', detail='No Downloads folder at %s' % directory)
        with os.scandir(directory) as iterator:
            entries = []
            for entry in iterator:
                check()
                entries.append(entry)
    except _Cancelled:
        return ScanResult('cancelled')
    except PermissionError as exc:
        return ScanResult('denied', detail='Cannot read Downloads: %s' % exc.strerror)
    except OSError as exc:
        return ScanResult('no_folder', detail='Cannot open Downloads: %s' % exc.strerror)
    try:
        for entry in entries:
            check()
            if entry.name.lower().endswith(_PARTIAL_SUFFIXES):
                continue
            try:
                st = os.lstat(entry.path)
                if not stat.S_ISREG(st.st_mode) or st.st_size != pin.size:
                    continue
                cache = _copy_candidate(entry.path, version, pin, st,
                                        run_lock, check, catalogue)
                if cache is None:
                    continue
            except (OSError, ValueError):
                continue  # unreadable, vanished or corrupt copy: not a usable candidate
            return ScanResult('found', path=cache, source=entry.path)
    except _Cancelled:
        return ScanResult('cancelled')
    return ScanResult('none', detail='No verified %s firmware found in Downloads' % pin.name)


def open_download_page(version, opener=webbrowser.open, catalogue=firmware_catalogue):
    """Open FiiO's official download page. Call only from an explicit user action.

    Never raises for a browser problem: returns BrowserResult(False, url, why) so the UI can show
    the selectable URL."""
    if version not in catalogue.CATALOGUE:
        raise LocalError('No supported firmware version %r' % (version,))
    try:
        if opener(OFFICIAL_PAGE, new=2):
            return BrowserResult(True, OFFICIAL_PAGE)
        return BrowserResult(False, OFFICIAL_PAGE, 'no browser could be opened')
    except Exception as exc:
        return BrowserResult(False, OFFICIAL_PAGE, 'could not open a browser: %s' % exc)
