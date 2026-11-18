#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/vendor-build-common.sh"
src="$here/libplist-2.6.0"
for file in "$src"/src/*.c "$src"/libcnary/*.c; do
    name=${file##*/}
    "$cc" -Os -std=gnu99 -ffunction-sections -fdata-sections -fno-ident \
        "-ffile-prefix-map=$here/=third_party/" -DHAVE_CONFIG_H -I"$here/plistconfig" \
        -I"$src/src" -I"$src/include" -I"$src/libcnary/include" \
        -c "$file" -o "$work/${name%.c}.o"
done
"$ar" crD "$work/libplist.a" "$work/"*.o
mv "$work/libplist.a" "$out/libplist.a"
stat -c '%n %s bytes' "$out/libplist.a"
