#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Offline core-only build of the pinned upstream unity translation unit.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case ${1:-} in
    host) cc=${CC:-gcc}; ar=${AR:-ar} ;;
    mipsel) cc="${CROSS:?set CROSS}gcc"; ar="${CROSS}ar" ;;
    *) echo "usage: $0 host|mipsel" >&2; exit 2 ;;
esac
out=${SB_OUT:?set SB_OUT to a build output directory}
mkdir -p "$out/src"
if [ -f "$here/SheenBidi-3.0.0.tar.gz" ]; then
    echo '86c56014034739ba39a24c23eb00323b0bf6f737354f665786015fca842af786  SheenBidi-3.0.0.tar.gz' |
        (cd "$here" && sha256sum -c -)
    tar -xzf "$here/SheenBidi-3.0.0.tar.gz" -C "$out/src" --strip-components=1
elif [ -f "$here/SheenBidi-3.0.0/Source/SheenBidi.c" ]; then
    # Public source tree: no archive, only the pruned tree, whose files SHA256SUMS pins one by one.
    grep '  SheenBidi-3\.0\.0/' "$here/SHA256SUMS" | (cd "$here" && sha256sum -c --quiet -)
    cp -R "$here/SheenBidi-3.0.0/." "$out/src/"
else
    echo "$0: need SheenBidi-3.0.0.tar.gz or the SheenBidi-3.0.0 tree in $here" >&2
    exit 1
fi
"$cc" -Os -std=c11 -ffunction-sections -fdata-sections -fno-ident \
    -DSB_CONFIG_UNITY -DSB_CONFIG_DISABLE_SCRATCH_MEMORY -DNDEBUG \
    "-ffile-prefix-map=$out/=sheenbidi/" \
    -I"$out/src/Headers" -I"$out/src/Source" \
    -c "$out/src/Source/SheenBidi.c" -o "$out/sheenbidi.o"
"$ar" crD "$out/libsheenbidi.a" "$out/sheenbidi.o"
