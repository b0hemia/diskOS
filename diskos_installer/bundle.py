# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Locate the bundled native binaries and data files.

Layout (in the source tree AND inside the PyInstaller bundle):
    <root>/vendor/<os>-<arch>/   usbboot, mksquashfs, unsquashfs,
                                 my_write6_dram.bin, disc_spl_lpddr3.bin,
                                 lib/ (bundled .so/.dylib for usbboot)
    <root>/payload/              mq_ui, S97diskos_install, S99usbserial,
                                 diskos_manifest templates, etc.

When frozen by PyInstaller, data is unpacked under sys._MEIPASS; in the source
tree it sits next to this package. `resource_root()` resolves both.

A source checkout does NOT include the large native tools (they are .gitignored);
only the small device .bin blobs are tracked. For the STANDARD squashfs tools
(mksquashfs/unsquashfs, which are ordinary squashfs-tools binaries), when no
vendored copy is present we fall back to a system copy on PATH so a plain checkout
works without building. usbboot (a custom Ingenic mask-ROM uploader with no system
equivalent) and the device .bin blobs never fall back."""

import os
import shutil
import stat
import ctypes
import struct
import sys

from . import platform_probe

# Standard host tools that also ship as an ordinary system package (squashfs-tools).
# ONLY these may fall back to a PATH copy when the vendored binary is absent.
_SYSTEM_FALLBACK = frozenset({"mksquashfs", "unsquashfs"})


def resource_root():
    """Directory that contains vendor/ and payload/.

    - Frozen (PyInstaller): sys._MEIPASS.
    - Source tree: the installer/ dir (parent of this package)."""
    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        return sys._MEIPASS
    # this file is installer/diskos_installer/bundle.py -> installer/
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def vendor_dir():
    return os.path.join(resource_root(), "vendor", platform_probe.host_tag())


def payload_dir():
    return os.path.join(resource_root(), "payload")


def is_vendored(path):
    """True if `path` lives inside this host's vendor/ dir (a bundled tool), as
    opposed to a system copy resolved from PATH."""
    if not path:
        return False
    try:
        vd = os.path.abspath(vendor_dir())
        return os.path.commonpath([os.path.abspath(path), vd]) == vd
    except ValueError:
        return False   # different drive/root (Windows) -> not vendored


def native_build_script(kind="squashfs"):
    """Host-appropriate script that (re)builds the native tools, for error guidance."""
    o, _ = platform_probe.host()
    if o == "macos":
        return "vendor/setup-macos.sh"
    if o == "windows":
        return "build/build-windows-tools.sh"
    return "build/build-usbboot-static.sh" if kind == "usbboot" else "build/build-squashfs-static.sh"


def verify_windows_flash_bundle():
    """Match the build recipe's recorded source and output hashes before any USB probe.
    This is stale-build detection, not an authenticity signature.
    """
    _verify_windows_manifest(("src/usbboot/usbboot.c",), ("usbboot.exe",))


def verify_windows_driver_bundle():
    """Verify the isolated helper against the recorded source/resource/output hashes."""
    _verify_windows_manifest(("src/windows-driver/bind_winusb.c", "src/windows-driver/bind_winusb.rc",
                              "src/windows-driver/bind_winusb.manifest", "build/build-windows-driver.sh"),
                             ("bind_winusb.exe",))
    from .errors import FlashError
    try:
        _verify_windows_dlls(os.path.join(vendor_dir(), "bind_winusb.exe"))
    except (OSError, ValueError) as e:
        raise FlashError(f"Windows binding helper loader policy is unverified: {e}", code="E117",
                         action="rebuild with build/build-windows-tools.sh in MSYS2") from e


def _verify_windows_manifest(sources, binaries):
    import hashlib
    from pathlib import Path
    from .errors import FlashError
    try:
        root, vendor = Path(resource_root()), Path(vendor_dir())
        text = (vendor / "MANIFEST.txt").read_text(encoding="ascii")
        inputs, outputs = text.split("\nOutput SHA256:\n")
        inputs = inputs.split("\nInput SHA256 (paths relative to installer):\n")[1]
        import re
        # sha256sum lines: "<hash>  <name>" (text) or "<hash> *<name>" (binary; MSYS2 writes this form).
        line_re = re.compile(r"^([0-9a-f]{64}) [ *](.+)$")
        def check(section, name, path):
            matches = [m.group(1) for m in map(line_re.match, section.splitlines())
                       if m and m.group(2) == name]
            if len(matches) != 1 or matches[0] != hashlib.sha256(path.read_bytes()).hexdigest():
                raise ValueError(f"missing, ambiguous or stale hash for {name}")
        for name in sources:
            check(inputs, name, root / name)
        for name in binaries:
            check(outputs, name, vendor / name)
    except (OSError, ValueError, IndexError) as e:
        raise FlashError(f"Windows native tool bundle is unverified or stale: {e}", code="E117",
                         action="retain the old bundle outside vendor/windows-x86_64, then "
                                "rebuild with build/build-windows-tools.sh in MSYS2; "
                                "repeat check A before installing") from e


def _ensure_exec(path):
    try:
        st = os.stat(path)
        os.chmod(path, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except OSError:
        pass
    return path


def _which_abs(name):
    """Absolute path to `name` on PATH, ignoring empty and relative PATH entries
    (a relative dir on PATH would be an injection risk for a flash-critical tool).
    Returns None unless it resolves to an absolute, executable regular file."""
    raw = os.environ.get("PATH", os.defpath)
    safe = os.pathsep.join(d for d in raw.split(os.pathsep) if d and os.path.isabs(d))
    p = shutil.which(name, path=safe)
    if not p:
        return None
    p = os.path.abspath(p)
    return p if os.path.isfile(p) and os.access(p, os.X_OK) else None


def native(name, required=True):
    """Absolute path to a native tool for this host.

    A vendored copy under vendor/<tag>/ always wins. For the standard squashfs tools
    ONLY, when no vendored copy is present (a source checkout) we fall back to a system
    copy on PATH so the installer works without the large bundled binaries.

    A vendored copy that EXISTS but is broken (bad perms, wrong arch, missing libs) is
    never masked by the fallback: it is returned as-is and fails its later execution /
    capability check loudly. Only a genuinely ABSENT vendored entry triggers the fallback
    - a dangling symlink counts as a broken install (lexists), not as absent.

    Raises PreflightError (E102) if required and unresolved. System copies are returned
    as an absolute path and are NOT chmod'd (we never modify a system binary)."""
    if platform_probe.host()[0] == "macos":
        from .macos_runtime import prepare_tools
        prepare_tools(vendor_dir())
    windows = platform_probe.host()[0] == "windows"
    filename = name + ".exe" if windows and not name.endswith((".bin", ".exe")) else name
    p = os.path.join(vendor_dir(), filename)
    try:
        os.lstat(p)                          # present (even as a dangling/broken symlink) -> vendored
        return p if windows else _ensure_exec(p)               # dangling symlink still returned -> fails loudly downstream
    except FileNotFoundError:
        pass                                 # genuinely absent -> may fall back below
    except OSError:
        # the vendored path exists but is inaccessible (e.g. an unsearchable vendor dir): do NOT
        # mask a broken install with a system copy - return it so it fails loudly downstream.
        return p if windows else _ensure_exec(p)
    if name in _SYSTEM_FALLBACK and not windows:
        sysp = _which_abs(name)
        if sysp:
            return sysp                       # system tool: absolute, reused as-is, not chmod'd
    if not required:
        return None
    from .errors import PreflightError
    tag = platform_probe.host_tag()
    if windows:
        raise PreflightError(f"bundled file '{filename}' not found for {tag}", code="E102",
                             action="provide the matching Windows tools and adjacent runtime DLLs; "
                                    "Windows flashing is not supported in this slice")
    if name in _SYSTEM_FALLBACK:
        raise PreflightError(
            f"tool '{name}' not found in vendor/{tag} or PATH", code="E102",
            action="install squashfs-tools with LZO support (Arch/CachyOS: "
                   "sudo pacman -S squashfs-tools, Debian/Ubuntu: "
                   "sudo apt install squashfs-tools, Fedora: sudo dnf install squashfs-tools, "
                   "macOS: brew install squashfs), "
                   f"or build the bundled tools with {native_build_script('squashfs')} from the diskOS source")
    if name == "usbboot":
        raise PreflightError(
            f"bundled tool 'usbboot' not found for {tag}", code="E102",
            action=f"build the custom Ingenic uploader with {native_build_script('usbboot')} "
                   "from the diskOS source (it has no system-package equivalent)")
    raise PreflightError(
        f"bundled device file '{name}' not found for {tag}", code="E102",
        action="restore this file from the matching diskOS source or release; "
               "see build/NATIVE_TOOLS.md")


def data(name, required=True):
    """Absolute path to a bundled data/payload file."""
    p = os.path.join(payload_dir(), name)
    if os.path.exists(p):
        return p
    if required:
        from .errors import PreflightError
        raise PreflightError(f"bundled payload '{name}' not found", code="E102",
                             action="the install is incomplete - re-download the installer")
    return None


def native_env(tool_path=None):
    """Environment for running a native tool.

    For a BUNDLED tool, put its private vendor/<tag>/lib on the loader path so a bundled
    libusb is found without touching the system. For a SYSTEM tool resolved from PATH,
    return a clean environment: never point a system binary at our vendored libs, and undo
    any PyInstaller loader override (restore *_ORIG) so it loads its own system libraries.

    `tool_path` is the resolved binary path; None means "bundled" (back-compat)."""
    env = dict(os.environ)
    o, _ = platform_probe.host()
    if o == "windows":
        if tool_path is None:
            raise ValueError("Windows native environment requires an explicit executable")
        _verify_windows_dlls(tool_path)
        # Private runtime DLLs MUST be beside the executable. PATH is searched after
        # System32, so putting a lib/ directory on PATH alone cannot protect them.
        env["PATH"] = os.path.dirname(os.path.abspath(tool_path))
        return env
    var = "DYLD_LIBRARY_PATH" if o == "macos" else "LD_LIBRARY_PATH"
    if tool_path is None or is_vendored(tool_path):
        libdir = os.path.join(vendor_dir(), "lib")
        if os.path.isdir(libdir):
            env[var] = libdir + (os.pathsep + env[var] if env.get(var) else "")
        return env
    # System tool: strip our / PyInstaller loader overrides so it uses its own libs.
    for v in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        orig = env.pop(v + "_ORIG", None)    # PyInstaller stashes the pre-launch value here
        if orig is not None:
            env[v] = orig
        elif getattr(sys, "frozen", False):
            env.pop(v, None)                 # frozen w/o _ORIG -> don't leak _MEIPASS to a system tool
    return env


def msys_path(path):
    """Map local absolute drives for the adjacent MSYS runtime without fstab."""
    path = os.fspath(path)
    if platform_probe.host()[0] != "windows":
        return path
    import ntpath
    if not isinstance(path, str) or any(c in path for c in "\r\n\0"):
        raise ValueError("MSYS path must be an absolute local drive path")
    drive, rest = ntpath.splitdrive(path)
    if (len(drive) != 2 or drive[0] not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
            or drive[1] != ":" or not rest.startswith(("\\", "/"))):
        raise ValueError("MSYS path must be an absolute local drive path")
    return "/cygdrive/" + drive[0].lower() + rest.replace("\\", "/")


def _windows_api():
    """Typed Win32 boundary, shared by the lock, job and sleep services."""
    from ctypes import wintypes as w
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "CreateMutexExW": ([ctypes.c_void_p, w.LPCWSTR, w.DWORD, w.DWORD], w.HANDLE),
        "WaitForSingleObject": ([w.HANDLE, w.DWORD], w.DWORD),
        "ReleaseMutex": ([w.HANDLE], w.BOOL), "CloseHandle": ([w.HANDLE], w.BOOL),
        "CreateJobObjectW": ([ctypes.c_void_p, w.LPCWSTR], w.HANDLE),
        "SetInformationJobObject": ([w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD], w.BOOL),
        "AssignProcessToJobObject": ([w.HANDLE, w.HANDLE], w.BOOL),
        "TerminateJobObject": ([w.HANDLE, w.UINT], w.BOOL),
        "SetThreadExecutionState": ([w.DWORD], w.DWORD),
    }
    for name, (args, result) in signatures.items():
        fn = getattr(api, name)
        fn.argtypes, fn.restype = args, result
    return api

# External dependencies observed in the built Windows squashfs tools and their
# adjacent MSYS runtime. Add names only with import evidence for shipped tools.
_WINDOWS_OS_DLLS = frozenset({"kernel32.dll", "ntdll.dll"})
# Static UCRT64 usbboot import evidence: exactly these Windows 10+ OS contracts
# (filesystem: _lock_file/_unlock_file, pulled in by the MinGW stdio runtime; seen 2026-10-09).
# The binding helper has no CRT imports or CRT startup.
_WINDOWS_USBBOOT_UCRT = frozenset(
    "api-ms-win-crt-" + part + "-l1-1-0.dll" for part in (
        "convert", "environment", "filesystem", "heap", "locale", "math", "private",
        "runtime", "stdio", "string", "time", "utility"))

# Explicit System32 loads by the binding helper; never allowed as static imports.
_WINDOWS_BIND_OS = frozenset({"setupapi.dll", "cfgmgr32.dll", "advapi32.dll", "newdev.dll"})
_WINDOWS_BIND_IMPORTS = frozenset({"kernel32.dll"})


def _verify_windows_dlls(tool_path):
    """Verify the PE import closure before launch; private DLLs live beside the EXE.
    Only closed OS names (plus per-tool system imports and UCRT contracts) resolve outside it.
    Reject adjacent allowlisted names, even when not imported. Inspect all other
    imported DLLs recursively beside the EXE, including unknown API-set names.
    Non-empty delay-import directories are unsupported and fail closed.
    """
    folder = os.path.dirname(os.path.abspath(tool_path))
    adjacent = {}
    with os.scandir(folder) as entries:
        for entry in entries:
            name = entry.name.lower()
            if name.endswith(".dll"):
                if name in _WINDOWS_OS_DLLS | _WINDOWS_USBBOOT_UCRT | _WINDOWS_BIND_OS:
                    raise ValueError("adjacent DLL shadows an OS name: " + name)
                if name in adjacent:
                    raise ValueError("ambiguous adjacent DLL name: " + name)
                adjacent[name] = entry.path
    seen = set()
    def check(path):
        key = os.path.basename(path).lower()
        binding_helper = key == "bind_winusb.exe" and os.path.abspath(path) == os.path.abspath(tool_path)
        if key in seen:
            return
        seen.add(key)
        with open(path, "rb") as f:
            data = f.read()
        try:
            pe = struct.unpack_from("<I", data, 60)[0]
            if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
                raise ValueError("not a PE executable")
            count, size = struct.unpack_from("<H12xH", data, pe + 6)
            opt = pe + 24
            magic = struct.unpack_from("<H", data, opt)[0]
            if magic not in (0x10b, 0x20b):
                raise ValueError("unknown PE format")
            if binding_helper and (magic != 0x20b or struct.unpack_from("<H", data, pe + 4)[0] != 0x8664):
                raise ValueError("binding helper must be native x64")
            sections = opt + size
            def offset(rva):
                for i in range(count):
                    length, addr, rawsize, raw = struct.unpack_from("<4I", data, sections + i * 40 + 8)
                    if addr <= rva < addr + max(length, rawsize):
                        return raw + rva - addr
                raise ValueError("unmapped PE import")
            directory = opt + (112 if magic == 0x20b else 96)
            # Delay-loaded dependencies cannot be verified by this slice.
            if any(struct.unpack_from("<II", data, directory + 13 * 8)):
                raise ValueError("delay imports are unsupported")
            rva = struct.unpack_from("<I", data, directory + 8)[0]
            if not rva:
                if binding_helper:
                    raise ValueError("binding helper imports must be exactly kernel32.dll")
                return
            pos = offset(rva)
            imports = set()
            while any(struct.unpack_from("<5I", data, pos)):
                start = offset(struct.unpack_from("<I", data, pos + 12)[0])
                name = data[start:data.index(b"\0", start)].decode("ascii").lower()
                imports.add(name)
                if name != os.path.basename(name) or "/" in name or "\\" in name:
                    raise ValueError("invalid DLL name")
                allowed = _WINDOWS_OS_DLLS
                if key == "usbboot.exe" and os.path.abspath(path) == os.path.abspath(tool_path):
                    allowed = allowed | _WINDOWS_USBBOOT_UCRT
                if binding_helper:
                    allowed = _WINDOWS_BIND_IMPORTS
                    if name not in allowed:
                        raise ValueError("unexpected binding helper import: " + name)
                if name not in allowed:
                    if name not in adjacent:
                        raise FileNotFoundError(os.path.join(folder, name))
                    check(adjacent[name])
                pos += 20
            if binding_helper and imports != _WINDOWS_BIND_IMPORTS:
                raise ValueError("binding helper imports must be exactly kernel32.dll")
        except (struct.error, IndexError, UnicodeError) as e:
            raise ValueError("invalid PE imports: " + path) from e
    check(tool_path)
