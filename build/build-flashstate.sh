#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build with the current mipsel-n2008-musl-cross toolchain's bin/mipsel-linux-musl- prefix.
# Release builds use fixed device paths, with no -D overrides.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CROSS=${CROSS:-mipsel-linux-musl-}
OUT="$HERE/../payload/diskos-flashstate"
"${CROSS}gcc" -Os -static -Wall -Wextra -Werror -Wl,--build-id=none \
    -ffile-prefix-map="$HERE/../src/flashstate/"= \
    -o "$OUT" "$HERE/../src/flashstate/diskos_flashstate.c"
"${CROSS}strip" "$OUT"
chmod 755 "$OUT"
sha256sum "$OUT"
