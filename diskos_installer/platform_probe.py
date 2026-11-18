# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Host OS/arch detection and Snowsky-Disc device presence checks.

Device USB identities we care about:
  - Ingenic X2000 mask-ROM (flashing mode):  VID:PID 0a108:eaef  (a108:eaef)
  - normal running device (not used for flashing) is a different id.

We enumerate USB via pyusb (bundled) so we don't depend on `lsusb` being present.
"""

import platform
import struct
import sys

MASKROM_VID = 0x0a108 & 0xFFFF  # printed as a108 by lsusb; VID field is 0xa108
MASKROM_PID = 0xeaef
# lsusb shows "a108:eaef"; libusb reports idVendor=0xa108 idProduct=0xeaef
MASKROM_VID = 0xa108


def host():
    """Return (os_key, arch_key) e.g. ('linux','x86_64') / ('macos','arm64')."""
    sysname = platform.system().lower()
    if sysname == "darwin":
        os_key = "macos"
    elif sysname == "linux":
        os_key = "linux"
    else:
        os_key = sysname
    machine = platform.machine().lower()
    arch = {
        "x86_64": "x86_64", "amd64": "x86_64",
        "arm64": "arm64", "aarch64": "arm64",
    }.get(machine, machine)
    return os_key, arch


def windows_native_runtime():
    """Native x64 Windows CPython; shell choice does not qualify Python."""
    return (sys.platform == "win32" and platform.system() == "Windows"
            and host() == ("windows", "x86_64") and struct.calcsize("P") == 8
            and platform.python_implementation() == "CPython")


def windows_dev_runtime():
    """Development opt-ins apply only to the native source runtime."""
    return windows_native_runtime() and not getattr(sys, "frozen", False)


def host_tag():
    o, a = host()
    return f"{o}-{a}"


def is_supported():
    o, _ = host()
    return o in ("linux", "macos")


def host_status():
    from .flasher import flash_eligible
    if flash_eligible():
        return "flashing enabled" if host()[0] == "windows" else "supported"
    if host()[0] == "windows":
        return "host services ready, flashing not yet supported"
    return "supported" if is_supported() else "UNSUPPORTED"


def distro_info():
    """Return dict of /etc/os-release fields on Linux, or empty dict."""
    if platform.system().lower() != "linux":
        return {}
    if hasattr(platform, "freedesktop_os_release"):
        try:
            return platform.freedesktop_os_release()
        except OSError:
            pass
    try:
        data = {}
        with open("/etc/os-release", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    data[k] = v.strip('"\'')
        return data
    except Exception:
        return {}


def distro_name():
    """Human-readable OS or distribution name, e.g. 'CachyOS Linux', 'Arch Linux', 'Ubuntu 24.04', 'macOS'."""
    sysname = platform.system().lower()
    if sysname == "darwin":
        ver = platform.mac_ver()[0]
        return f"macOS {ver}".strip() if ver else "macOS"
    if sysname == "linux":
        info = distro_info()
        return info.get("PRETTY_NAME") or info.get("NAME") or "Linux"
    return platform.system()


LIBUSB_LOADED = None


def _bundled_libusb_backend():
    """A pyusb libusb1 backend pointed at OUR bundled libusb, so USB enumeration
    works in the frozen app even when the system has no libusb. Returns a backend
    or None (caller falls back to pyusb's default search)."""
    global LIBUSB_LOADED
    if host()[0] == "macos":
        import os
        import usb.backend.libusb1 as libusb1
        from . import bundle
        from .macos_runtime import prepare_tools
        prepare_tools(bundle.vendor_dir())
        path = os.path.join(bundle.vendor_dir(), "lib", "libusb-1.0.0.dylib")
        # A frozen release must never silently fall back to a host library.
        if not os.path.isfile(path):
            if getattr(sys, "frozen", False):
                raise RuntimeError('Missing bundled libusb: ' + path)
            import ctypes.util
            path = ctypes.util.find_library('usb-1.0')
            if not path:
                for prefix in ('/opt/homebrew/lib', '/usr/local/lib'):
                    candidate = os.path.join(prefix, 'libusb-1.0.dylib')
                    if os.path.isfile(candidate):
                        path = candidate
                        break
        backend = libusb1.get_backend(find_library=lambda _name: path) if path else None
        if backend is None:
            raise RuntimeError('Cannot load macOS libusb: ' + str(path))
        LIBUSB_LOADED = str(getattr(getattr(backend, 'lib', None), '_name', path))
        return backend
    try:
        import glob
        import os
        import usb.backend.libusb1 as libusb1
        from . import bundle
        libdir = os.path.join(bundle.vendor_dir(), "lib")
        # macOS dylib or Linux .so, whatever we bundled
        for pat in ("libusb-1.0*.dylib", "libusb-1.0.so*", "libusb-1.0*"):
            hits = sorted(glob.glob(os.path.join(libdir, pat)))
            if hits:
                return libusb1.get_backend(find_library=lambda _n, _p=hits[0]: _p)
    except Exception:
        pass
    return None


def _maskrom_count_pyusb():
    """Count devices in Ingenic mask-ROM mode via pyusb. Returns int or None if
    pyusb/backend is unavailable."""
    try:
        import usb.core  # pyusb (bundled)
    except Exception:
        return None
    try:
        backend = _bundled_libusb_backend()   # prefer our bundled libusb
        devs = list(usb.core.find(find_all=True, idVendor=MASKROM_VID,
                                  idProduct=MASKROM_PID, backend=backend))
        return len(devs)
    except Exception:
        return None


def _maskrom_count_lsusb():
    """Fallback: parse `lsusb` if present (Linux)."""
    import shutil
    import subprocess
    if not shutil.which("lsusb"):
        return None
    try:
        out = subprocess.run(["lsusb"], capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return None
    return sum(1 for ln in out.splitlines() if "a108:eaef" in ln.lower())


def maskrom_nodes(sys_root="/sys/bus/usb/devices", dev_root="/dev/bus/usb"):
    """Linux: the /dev/bus/usb/BBB/DDD node of every device in mask-ROM mode, found through sysfs (no libusb).
    Returns a list (possibly empty), or None when sysfs is not there (not Linux)."""
    import os
    if not os.path.isdir(sys_root):
        return None
    nodes = []
    for d in sorted(os.listdir(sys_root)):
        base = os.path.join(sys_root, d)
        try:
            with open(os.path.join(base, "idVendor")) as f:
                vid = int(f.read().strip(), 16)
            with open(os.path.join(base, "idProduct")) as f:
                pid = int(f.read().strip(), 16)
            if (vid, pid) != (MASKROM_VID, MASKROM_PID):
                continue
            with open(os.path.join(base, "busnum")) as f:
                bus = int(f.read().strip())
            with open(os.path.join(base, "devnum")) as f:
                dev = int(f.read().strip())
        except (OSError, ValueError):
            continue
        nodes.append(os.path.join(dev_root, "%03d" % bus, "%03d" % dev))
    return nodes


USB_ACCESS_FIX = ("install the udev rule once: `sudo cp udev/70-diskos-maskrom.rules /etc/udev/rules.d/ && "
                  "sudo udevadm control --reload-rules && sudo udevadm trigger`, then unplug and replug the device. "
                  "Without a desktop login session (e.g. over SSH), add GROUP=\"<group>\" to that rule "
                  "using a group available on your system (for example plugdev on Debian or uucp on Arch), "
                  "add your user to that group, then log out and back in. "
                  "Do not run the installer with sudo.")


def maskrom_access(sys_root="/sys/bus/usb/devices", dev_root="/dev/bus/usb"):
    """Can this user OPEN the mask-ROM device, the way usbboot will? Seeing it (maskrom_count) is not enough: on
    Linux the node is root-only until the udev rule grants access (GitHub #10: counted, then usbboot failed E301).
    Returns ("ok"|"denied"|"unknown", detail). "unknown" = not Linux, no device, or an unexpected error."""
    if host()[0] == "windows":
        from .windows_usb import inventory, require_target
        from .errors import PreflightError
        try:
            row = require_target(inventory())
        except PreflightError as e:
            return "denied", str(e)
        except OSError as e:
            return "unknown", str(e)
        return "unknown", "WinUSB bound; run native diagnostic to prove access: " + row["instance_id"]
    import errno
    import os
    nodes = maskrom_nodes(sys_root, dev_root)
    if nodes is None:
        return "unknown", "not checked on this system"
    if not nodes:
        return "unknown", "no device in mask-ROM mode"
    for node in nodes:
        try:
            fd = os.open(node, os.O_RDWR)
        except OSError as e:
            if e.errno in (errno.EACCES, errno.EPERM):
                return "denied", node
            return "unknown", "%s: %s" % (node, e.strerror)
        os.close(fd)
    return "ok", ", ".join(nodes)


def maskrom_count():
    """How many devices are currently in mask-ROM mode. Prefers pyusb, falls back
    to lsusb, returns -1 if neither is available (caller should warn)."""
    if host()[0] == "windows":
        from .windows_usb import inventory
        try:
            return inventory()["count"]
        except OSError:
            return -1
    n = _maskrom_count_pyusb()
    if n is not None:
        return n
    n = _maskrom_count_lsusb()
    if n is not None:
        return n
    return -1
