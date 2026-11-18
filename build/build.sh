#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# build.sh - produce the standalone `diskos-installer` executable for THIS host
# (Linux or macOS). PyInstaller can't cross-build, so run this on each target OS.
#
# Prereqs: python3. Everything else is created in a local venv; nothing is
# installed system-wide.
#
# Before building, populate vendor/<os>-<arch>/ with the native tools for this
# host (see build/NATIVE_TOOLS.md): usbboot, mksquashfs, unsquashfs,
# my_write6_dram.bin, disc_spl_lpddr3.bin (+ lib/ for any bundled .so/.dylib).
set -euo pipefail
cd "$(dirname "$0")/.."          # installer/

VENV=build/venv
[ -d "$VENV" ] || python3 -m venv "$VENV"
"$VENV/bin/pip" install --quiet --upgrade pip pyinstaller -r requirements.txt

# sanity: the vendor dir for this host must exist
tag=$("$VENV/bin/python" - <<'PY'
import platform
s=platform.system().lower(); o={"darwin":"macos","linux":"linux"}.get(s,s)
m=platform.machine().lower()
a={"x86_64":"x86_64","amd64":"x86_64","arm64":"arm64","aarch64":"arm64"}.get(m,m)
print(f"{o}-{a}")
PY
)
[ -d "vendor/$tag" ] || { echo "ERROR: vendor/$tag missing - add native tools for this host first." >&2; exit 2; }

"$VENV/bin/pyinstaller" --clean --noconfirm \
  --distpath build/dist --workpath build/work \
  build/diskos-installer.spec

echo
echo "Built: build/dist/diskos-installer  ($(du -h build/dist/diskos-installer | cut -f1))"
echo "Smoke test: build/dist/diskos-installer doctor"
