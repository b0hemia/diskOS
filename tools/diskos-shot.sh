#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# diskos-shot.sh - capture the device screen to a PNG over SSH.
#
# The panel is 360x360, 32bpp, BGRA byte order, and mounted rotated 180 degrees, so we read the raw
# framebuffer (/dev/fb0), reinterpret it as BGRA, and rotate 180. One frame is exactly 518400 bytes
# (360 * 360 * 4). A capture during a live UI redraw can show mild tearing; re-shoot if needed.
#
# Usage:
#   DISKOS_IP=<device-ip> DISKOS_PW=<debug-mode-password> ./diskos-shot.sh [out.png]
#
# Get the IP and password from Debug Mode (Settings > System).
# Requires: sshpass, ssh, python3 with Pillow (pip install Pillow). Default output: diskos-shot.png
set -eu

IP="${DISKOS_IP:?set DISKOS_IP to the device IP (shown in Debug Mode)}"
export SSHPASS="${DISKOS_PW:?set DISKOS_PW to the Debug Mode SSH password}"
OUT="${1:-diskos-shot.png}"
RAW="$(mktemp)"
trap 'rm -f "$RAW"' EXIT

# password via SSHPASS (sshpass -e), not the command line
sshpass -e ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 "root@$IP" \
  'dd if=/dev/fb0 bs=518400 count=1 2>/dev/null' > "$RAW"

# portable byte count (stat -c is GNU-only); require a full frame
BYTES="$(wc -c < "$RAW" | tr -d ' ')"
[ "$BYTES" -ge 518400 ] || { echo "capture failed (short read: $BYTES bytes)" >&2; exit 1; }

python3 - "$RAW" "$OUT" <<'PY'
import sys
from PIL import Image
raw = open(sys.argv[1], "rb").read()[:518400]
img = Image.frombuffer("RGBA", (360, 360), raw, "raw", "BGRA").convert("RGB").rotate(180)
img.save(sys.argv[2])
print(sys.argv[2])
PY
