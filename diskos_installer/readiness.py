# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Read-only 'ready check' for the GUI checklist.

probe() reads the host once (it can block on USB, so the GUI runs it off the UI thread);
evaluate() turns a probe plus the user's choices into checklist rows. Nothing here writes,
opens a USB device, or raises on a missing item: it only warns. flasher.preflight
stays the authoritative gate right before the flash."""

import os

from . import bundle, flasher, platform_probe
from .errors import PreflightError

# row states: True = ok, False = needs attention, None = not known yet / checked later
STEPS = ("Power the Disc OFF.",
         "Hold Volume Down.",
         "Plug in USB. The screen stays black; that is expected.")


def _tools():
    names = ["usbboot", "mksquashfs", "unsquashfs", flasher.WRITER_NAME, "disc_spl_lpddr3.bin"]
    missing = []
    windows = platform_probe.host()[0] == "windows"
    for name in names:
        filename = name + ".exe" if windows and not name.endswith((".bin", ".exe")) else name
        path = os.path.join(bundle.vendor_dir(), filename)
        if os.path.lexists(path):
            if not os.path.isfile(path):
                missing.append(name)
        elif not (not windows and name in ("mksquashfs", "unsquashfs")
                  and bundle._which_abs(name)):
            missing.append(name)
    return missing


def _linux_access(nodes):
    """Permission hint only: never open usbfs. preflight proves access later."""
    if not nodes or any(not os.path.exists(n) for n in nodes):
        return "unknown", "device changed while checking"
    kwargs = {"effective_ids": True} if os.access in os.supports_effective_ids else {}
    for node in nodes:
        if not os.access(node, os.R_OK | os.W_OK, **kwargs):
            return "denied", node
    return "ok", ", ".join(nodes)


def probe(inventory=None):
    """Windows passes the driver inventory it already read (a dict, or an error string);
    elsewhere the mask-ROM devices are counted here. Returns a plain dict."""
    out = {"os": platform_probe.host()[0], "count": None, "access": None, "inv": None,
           "inv_error": None, "missing_tools": []}
    if out["os"] == "windows":
        if isinstance(inventory, dict):
            out["inv"], out["count"] = inventory, inventory.get("count")
        else:
            out["inv_error"] = inventory or "Windows USB state has not been read yet"
    elif out["os"] == "linux":
        nodes = platform_probe.maskrom_nodes()
        out["count"] = len(nodes) if nodes is not None else -1
        if out["count"] == 1:
            out["access"] = _linux_access(nodes)
    else:
        out["count"] = platform_probe.maskrom_count()  # enumeration only; no open or claim
        if out["count"] == 1:
            out["access"] = ("unknown", "checked by preflight")
    try:
        out["missing_tools"] = _tools()
    except (OSError, PreflightError):
        out["missing_tools"] = ["bundled tools"]
    return out


def _row(key, label, ok, text, fix="", action=None, help=False):
    return {"key": key, "label": label, "ok": ok, "text": text, "fix": fix, "action": action, "help": help}


def _device_row(p):
    n, windows = p["count"], p["os"] == "windows"
    if windows and p["inv_error"]:
        return _row("device", "Disc in flash mode", None, "Not checked yet")
    state = (p["inv"] or {}).get("state") if windows else None
    if p["count"] is None:
        return _row("device", "Disc in flash mode", None, "Checking...")
    if n == -1:
        return _row("device", "Disc in flash mode", False, "USB cannot be listed on this computer",
                    "Run 'diskos-installer doctor' for details.")
    if n == 0:
        text = ("Only a disconnected Disc is recorded" if state == "phantom"
                else "No Disc in flash mode yet")
        return _row("device", "Disc in flash mode", False, text, "", help=True)
    if n > 1:
        return _row("device", "Disc in flash mode", False, "%d devices in flash mode - need exactly 1" % n,
                    "Unplug the other Ingenic devices.")
    return _row("device", "Disc in flash mode", True, "1 Disc found in flash mode")


def _access_row(p):
    if p["os"] == "windows":
        label = "USB driver"
        if p["inv_error"] and p["inv"] is None:
            ok = None if p["inv_error"].startswith("Windows USB state") else False
            return _row("access", label, ok, "Not checked yet" if ok is None else p["inv_error"])
        state = p["inv"]["state"]
        if state == "winusb":
            return _row("access", label, True, "Windows USB driver (WinUSB) is ready")
        if state == "driver":
            return _row("access", label, False, "Windows USB driver not set up yet",
                        "Windows asks for administrator approval once.", "driver")
        return _row("access", label, None, "Checked once the Disc is found")
    label = "USB access"
    if p["count"] != 1:
        return _row("access", label, None, "Checked once the Disc is found")
    state = (p["access"] or ("unknown", ""))[0]
    if state == "ok":
        return _row("access", label, True, "USB permissions look ready; access is checked before flashing")
    if state == "denied":
        return _row("access", label, False, "This user cannot open the Disc over USB",
                    "Fix: sudo cp udev/70-diskos-maskrom.rules /etc/udev/rules.d/ && sudo udevadm control "
                    "--reload-rules && sudo udevadm trigger, then replug the Disc (see docs/INSTALL.md).")
    return _row("access", label, None, "Checked when the flash starts")


def _firmware_row(mode, firmware, have_stock, fw=None):
    if fw is not None:      # the GUI's acquisition state: (verified, text, fix)
        return _row("firmware", "Firmware", fw[0], fw[1], fw[2])
    label = "Firmware chosen"
    if mode == "restore" and have_stock:
        return _row("firmware", label, True, "A saved stock image is available")
    fw = (firmware or "").strip()
    if not fw:
        return _row("firmware", label, False, "No firmware chosen yet", "Choose it below.")
    return _row("firmware", label, True, os.path.basename(fw))


def _tools_row(p):
    if p["missing_tools"]:
        return _row("tools", "Installer tools", False, "Installer tools missing: " + ", ".join(p["missing_tools"]),
                    "Download the installer again; a file is missing from this copy.")
    return _row("tools", "Installer tools", True, "All bundled tools found")


def evaluate(p, mode="install", firmware="", have_stock=False, fw=None):
    """Checklist rows for the given probe. Rows warn only; none of them blocks Install."""
    return [_device_row(p), _access_row(p), _firmware_row(mode, firmware, have_stock, fw), _tools_row(p)]


def needs_flash_mode_help(rows):
    """True when no Disc is in flash mode (zero found), so the 3-step help should show."""
    return any(r["help"] for r in rows)
