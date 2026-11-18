#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Static MIPS helpers, fixed production paths and no new dependencies.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CROSS=${CROSS:-mipsel-linux-musl-}
OUT_DIR=${OUT_DIR:-"$HERE/../payload"}
for part in names udhcpc; do
    "${CROSS}gcc" -Os -static -Wall -Wextra -Werror -Wl,--build-id=none \
        -ffile-prefix-map="$HERE/../src/names/"= \
        -o "$OUT_DIR/diskos-$part" "$HERE/../src/names/diskos_$part.c"
    "${CROSS}strip" "$OUT_DIR/diskos-$part"
    chmod 755 "$OUT_DIR/diskos-$part"
    sha256sum "$OUT_DIR/diskos-$part"
done
