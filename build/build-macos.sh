#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Offline release freeze. Build native tools separately with vendor/setup-macos.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Darwin ] || { echo 'Run this on macOS.' >&2; exit 2; }
[ "$#" = 3 ] || { echo 'Usage: bash build/build-macos.sh /absolute/framework/python3 /absolute/inputs.json SHA256' >&2; exit 2; }
PY="$1"
export DISKOS_MACOS_INPUTS="$2" DISKOS_MACOS_INPUTS_SHA256="$3"
export PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1
unset PYTHONPATH PYTHONHOME
"$PY" -B -I build/macos_inputs.py "$PWD" "$2" "$3"
ARCH=$(uname -m)
case "$ARCH" in arm64|x86_64) ;; *) exit 2;; esac
# Every build has a fresh output directory. Retain failed runs for diagnosis.
mkdir -p build/dist
RUN=$(mktemp -d "$PWD/build/dist/macos-${ARCH}.XXXXXX")
"$PY" -B -I -m PyInstaller --noconfirm --distpath "$RUN/dist" --workpath "$RUN/work" build/diskos-installer.spec
APP="$RUN/dist/diskOS Installer.app"
EXE="$APP/Contents/MacOS/diskos-installer"
"$PY" -B -I build/macos_links.py "$APP" "$EXE" --arch "$ARCH" --rewrite
"$PY" -B -I build/macos_links.py "$APP" "$EXE" --arch "$ARCH"
# Sign all nested Mach-O files before sealing the outer app; failure is fatal.
"$PY" -B -I - "$APP" <<'PY'
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path('build').resolve()))
from macos_links import macho
app = Path(sys.argv[1])
files = sorted({p.resolve() for p in app.rglob('*') if p.is_file() and macho(p)},
               key=lambda p: len(p.parts), reverse=True)
for path in files:
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(path)], check=True)
subprocess.run(['/usr/bin/codesign', '--force', '--deep', '--sign', '-', str(app)], check=True)
subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], check=True)
PY
"$EXE" doctor
"$PY" -B -I build/macos_release.py "$APP" "$RUN" "$2"
echo "Built macOS $ARCH app and archive in $RUN"
