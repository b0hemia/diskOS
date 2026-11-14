#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# diskOS installer - one-time setup.
#
# Creates a local Python virtual environment (.venv) next to this script and installs the two
# pip dependencies (pyusb, pycryptodome) into it. After this you run the tool with ./diskos-installer.
#
# What this does NOT install (they come from your system, not from us):
#   - Python 3 itself            (install via your OS package manager if missing)
#   - Tk/tkinter                 (only needed for the GRAPHICAL installer; CLI works without it)
#   - libusb-1.0                 (needed if no bundled copy is present)
# The script checks for these and tells you exactly what to install if any are missing.
set -eu

HERE=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Do NOT run this as root. The installer keeps per-user state and a saved recovery image under your
# home directory; running setup as root would create them root-owned and in the wrong place.
if [ "$(id -u)" = "0" ]; then
    echo "error: don't run install.sh as root (no sudo). Run it as your normal user;" >&2
    echo "       only the one-time udev rule needs root (see README, 'USB permissions')." >&2
    exit 1
fi

# Scratch file for capturing venv-creation errors: created private + removed on exit, never a
# fixed /tmp path (a predictable name in a world-writable dir invites a symlink attack).
ERRTMP=$(mktemp "${TMPDIR:-/tmp}/diskos_venv.XXXXXX") || { echo "error: mktemp failed" >&2; exit 1; }
trap 'rm -f "$ERRTMP"' EXIT INT TERM

# ---- locate a usable Python 3 -------------------------------------------------
PY=""
if [ "$(uname -s)" = Darwin ]; then
    . "$HERE/build/macos-python.sh"
    PY=$(select_macos_python) || exit 1
else
for c in python3 python; do
    if command -v "$c" >/dev/null 2>&1; then
        if "$c" -c 'import sys; sys.exit(0 if sys.version_info[:2] >= (3, 8) else 1)' 2>/dev/null; then
            PY=$(command -v "$c"); break
        fi
    fi
done
fi
if [ -z "$PY" ]; then
    echo "error: Python 3.8+ not found." >&2
    echo "  Arch/CachyOS:  sudo pacman -S python" >&2
    echo "  Debian/Ubuntu: sudo apt install python3 python3-venv" >&2
    echo "  Fedora:        sudo dnf install python3" >&2
    echo "  macOS:         brew install python3   (or install from python.org)" >&2
    exit 1
fi
echo "==> using $("$PY" --version 2>&1) at $PY"

# ---- create the venv ----------------------------------------------------------
if [ ! -x "$HERE/.venv/bin/python" ] && [ ! -x "$HERE/.venv/Scripts/python.exe" ]; then
    echo "==> creating virtual environment in .venv/"
    if ! "$PY" -m venv "$HERE/.venv" 2>"$ERRTMP"; then
        echo "error: could not create the virtual environment:" >&2
        sed 's/^/  /' "$ERRTMP" >&2 2>/dev/null || true
        echo "  Debian/Ubuntu often needs: sudo apt install python3-venv" >&2
        echo "  Arch/CachyOS: python includes venv in core package (reinstall: sudo pacman -S python)" >&2
        exit 1
    fi
else
    echo "==> reusing existing .venv/"
fi
VENV_PY="$HERE/.venv/bin/python"
[ -x "$VENV_PY" ] || VENV_PY="$HERE/.venv/Scripts/python.exe"   # windows layout

# A stale venv can still point at Apple's old Tk even after selecting a new Python.
if [ "$(uname -s)" = Darwin ] && ! macos_python_ok "$VENV_PY"; then
    echo 'error: existing .venv uses missing or old Tk. Rename .venv to .venv-old, then re-run ./install.sh.' >&2
    exit 1
fi

# ---- install pinned dependencies ---------------------------------------------
echo "==> installing dependencies (pyusb, pycryptodome, certifi, platformdirs)"
"$VENV_PY" -m pip install --upgrade pip >/dev/null 2>&1 || true
"$VENV_PY" -m pip install -r "$HERE/requirements.txt"

# ---- capability checks (warn, do not fail) -----------------------------------
echo "==> checking optional system components"

# Tk (GUI only). A venv inherits the base interpreter's tkinter, so this reflects the system Python.
if "$VENV_PY" -c 'import tkinter' 2>/dev/null; then
    echo "  ok: tkinter present - the graphical installer will work"
else
    echo "  note: tkinter NOT found - the GRAPHICAL installer will not run (the CLI still works)."
    echo "        To enable the GUI, install Tk for this Python, then re-run this script:"
    echo "          Arch/CachyOS:  sudo pacman -S tk"
    echo "          Debian/Ubuntu: sudo apt install python3-tk"
    echo "          Fedora:        sudo dnf install python3-tkinter"
    echo "          macOS:         brew install python-tk@$("$PY" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
    echo "                         (or use python.org's Python, which bundles Tk)"
fi

# libusb-1.0 backend for pyusb (needed to see the device in mask-ROM mode).
TAG=$("$VENV_PY" -c 'from diskos_installer import platform_probe; print(platform_probe.host_tag())' 2>/dev/null || true)
BUNDLED_LIBUSB=0
for f in "$HERE/vendor/$TAG/lib/"libusb-1.0*.dylib "$HERE/vendor/$TAG/lib/"libusb-1.0.so*; do
    [ -f "$f" ] && BUNDLED_LIBUSB=1
done
if [ "$BUNDLED_LIBUSB" -eq 1 ]; then
    echo "  ok: bundled libusb-1.0 present - device detection will use it"
elif "$VENV_PY" -c 'import ctypes.util,sys; sys.exit(0 if ctypes.util.find_library("usb-1.0") else 1)' 2>/dev/null; then
    echo "  ok: libusb-1.0 present - device detection will work"
else
    echo "  note: libusb-1.0 NOT found - the tool cannot detect the device until you install it:"
    echo "          Arch/CachyOS:  sudo pacman -S libusb"
    echo "          Debian/Ubuntu: sudo apt install libusb-1.0-0"
    echo "          Fedora:        sudo dnf install libusb1"
    echo "          macOS:         brew install libusb"
fi

# squashfs-tools (needed to unpack and build firmware images)
if [ -n "$TAG" ] && [ -x "$HERE/vendor/$TAG/mksquashfs" ] && [ -x "$HERE/vendor/$TAG/unsquashfs" ]; then
    echo "  ok: bundled squashfs-tools present"
elif command -v mksquashfs >/dev/null 2>&1 && command -v unsquashfs >/dev/null 2>&1; then
    echo "  ok: system squashfs-tools present on PATH"
else
    echo "  note: squashfs-tools NOT found - needed to unpack and build firmware images:"
    echo "          Arch/CachyOS:  sudo pacman -S squashfs-tools"
    echo "          Debian/Ubuntu: sudo apt install squashfs-tools"
    echo "          Fedora:        sudo dnf install squashfs-tools"
    echo "          macOS:         brew install squashfs"
fi

# usbboot (custom Ingenic mask-ROM uploader)
if [ -n "$TAG" ] && [ -x "$HERE/vendor/$TAG/usbboot" ]; then
    echo "  ok: bundled usbboot present"
else
    echo "  note: usbboot not built yet (needed to flash in mask-ROM mode)."
    case "$TAG" in
        macos-*) echo "        Build it once with: bash vendor/setup-macos.sh" ;;
        linux-*) echo "        Build it once with: bash build/build-usbboot-static.sh" ;;
        *) echo "        See build/NATIVE_TOOLS.md for supported host builds." ;;
    esac
fi

echo
echo "Setup complete. Run the installer with:"
echo "    ./diskos-installer doctor      # check host + tools + device"
echo "    ./diskos-installer gui         # graphical installer (needs tkinter)"
echo "    ./diskos-installer --help      # all commands"
