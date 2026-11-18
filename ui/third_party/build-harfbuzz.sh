#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Offline OpenType-only build using upstream's harfbuzz.cc amalgamation.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
case ${1:-} in
    host) cxx=${CXX:-g++}; ar=${AR:-ar} ;;
    mipsel)
        cross=${CROSS:-$root/toolchain/build/mipsel-n2008-cxx-musl-cross/bin/mipsel-linux-musl-}
        cxx="${cross}g++"; ar="${cross}ar" ;;
    *) echo "usage: $0 host|mipsel" >&2; exit 2 ;;
esac
out=${HB_OUT:-$here/out/$1}
src="$here/harfbuzz-14.6.0"
ft=${FT_SRC:-$here/freetype-2.13.3}
mkdir -p "$out"
if [ -f "$here/harfbuzz-14.6.0.tar.xz" ]; then
    echo 'd07a007327277708a2a73ae437887cdbaf282937f6d03ca5467723e9099af586  harfbuzz-14.6.0.tar.xz' |
        (cd "$here" && sha256sum -c -)
elif [ -f "$src/src/harfbuzz.cc" ]; then
    # Public source tree: no archive, only the pruned tree, whose files SHA256SUMS pins one by one.
    grep '  harfbuzz-14\.6\.0/' "$here/SHA256SUMS" | (cd "$here" && sha256sum -c --quiet -)
else
    echo "$0: need harfbuzz-14.6.0.tar.xz or the harfbuzz-14.6.0 tree in $here" >&2
    exit 1
fi
# Keep all OpenType script shapers, Unicode data and thread safety. FT supplies
# font metrics; omit AAT, legacy fallback, OT font callbacks and non-shaping APIs.
# Do not use HB_TINY/HB_LEAN: they also remove hints/variation and other features.
"$cxx" -Os -std=c++11 -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -ffp-contract=off -fno-ident \
    "-ffile-prefix-map=$root/=diskos/" -DNDEBUG -DHAVE_FREETYPE -DHAVE_PTHREAD -pthread \
    -DHAVE_FT_GET_TRANSFORM -DHAVE_FT_GET_VAR_BLEND_COORDINATES -DHAVE_FT_DONE_MM_VAR \
    -DHB_MINI -DHB_NO_OT_FONT -DHB_NO_BUFFER_SERIALIZE -DHB_NO_BUFFER_VERIFY \
    -DHB_NO_DRAW -DHB_NO_COLOR -DHB_NO_MATH -DHB_NO_META \
    -DHB_NO_STYLE -DHB_NO_SUBSET -DHB_NO_OT_FETCH \
    -I"$ft/include" -I"$src/src" -c "$src/src/harfbuzz.cc" -o "$out/harfbuzz.o"
"$ar" crD "$out/libharfbuzz.a" "$out/harfbuzz.o"
stat -c '%n %s bytes' "$out/libharfbuzz.a"
