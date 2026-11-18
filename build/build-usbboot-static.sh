#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# build-usbboot-static.sh - build a portable, fully-static `usbboot` for Linux.
#
# The distro's libusb-1.0.a is built WITH udev, so linking against it drags in
# libudev + libcap dynamically - which defeats portability (missing/old libudev
# on other distros). This rebuilds libusb with --disable-udev --enable-static so
# usbboot links fully static: no libusb.so, no libudev, no glibc-version pin.
# udev is only needed for hotplug events; usbboot does a one-shot vid/pid open,
# which uses libusb's sysfs backend and works without udev.
#
# Output: installer/vendor/linux-x86_64/usbboot  (statically linked, stripped).
# Run on an x86_64 Linux host with build-essential + apt source access.
set -euo pipefail
cd "$(dirname "$0")/.."                       # installer/
ROOT=$(pwd)
SRC="$ROOT/src/usbboot/usbboot.c"
OUT="$ROOT/vendor/linux-x86_64/usbboot"
[ -f "$SRC" ] || { echo "ERROR: $SRC missing" >&2; exit 2; }

verify_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    printf '%s  %s\n' "$1" "$2" | sha256sum -c -
  elif command -v shasum >/dev/null 2>&1; then
    printf '%s  %s\n' "$1" "$2" | shasum -a 256 -c -
  else
    echo "ERROR: sha256sum or shasum is required to verify source" >&2
    return 1
  fi
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

echo "==> fetching libusb source"
if ! command -v apt-get >/dev/null 2>&1 || ! apt-get source libusb-1.0 >/dev/null 2>&1; then
  LIBUSB_VER="1.0.27"
  LIBUSB_URL="https://github.com/libusb/libusb/releases/download/v${LIBUSB_VER}/libusb-${LIBUSB_VER}.tar.bz2"
  LIBUSB_TAR="$WORK/libusb-${LIBUSB_VER}.tar.bz2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$LIBUSB_URL" -o "$LIBUSB_TAR"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$LIBUSB_TAR" "$LIBUSB_URL"
  else
    echo "ERROR: failed to fetch libusb source (apt-get source, curl, or wget required)" >&2
    exit 3
  fi
  verify_sha256 ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575 "$LIBUSB_TAR" || exit 3
  tar -xjf "$LIBUSB_TAR"
fi
cd libusb-1.0*/

echo "==> configuring libusb (static, no udev)"
./configure --disable-udev --enable-static --disable-shared \
  --disable-examples-build --disable-tests-build CFLAGS="-O2" >/dev/null
echo "==> building libusb"
make -j"$(nproc)" >/dev/null
LIBA=$(find "$PWD" -name libusb-1.0.a | head -1)
[ -f "$LIBA" ] || { echo "ERROR: libusb-1.0.a not produced" >&2; exit 4; }
INC=$(dirname "$LIBA")/../.. ; INC="$PWD/libusb"

echo "==> linking usbboot (fully static)"
gcc -O2 -std=c99 -I"$INC" -static -o usbboot "$SRC" "$LIBA" -lpthread
strip usbboot
file usbboot | grep -q "statically linked" || { echo "ERROR: not static" >&2; exit 5; }

install -m 0755 usbboot "$OUT"
echo "==> installed: $OUT"
echo "    sha256: $(sha256sum "$OUT" | cut -d' ' -f1)"
echo "    $(file -b "$OUT")"
echo "NOTE: confirm a mask-ROM GET_CPU_INFO handshake on a real device before shipping."
