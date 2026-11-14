#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 diskOS contributors
# Build the read-only boot launcher payload/diskos-launch (static 32-bit little-endian MIPS).
#   CROSS=/path/to/mipsel-linux-musl-  build/build-launcher.sh
# The shipped binary uses the compiled-in device paths; never pass -D overrides for a release build.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../src/launcher/diskos_launch.c"
OUT="$HERE/../payload/diskos-launch"
CROSS=${CROSS:-mipsel-linux-musl-}
"${CROSS}gcc" -Os -static -Wall -Wextra -Werror -ffile-prefix-map="$HERE/../src/launcher/"= -o "$OUT" "$SRC"
"${CROSS}strip" "$OUT" || true
chmod 755 "$OUT"
sha256sum "$OUT"
