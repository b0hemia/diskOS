#!/bin/bash
# Build Linux x86_64 squashfs tools with a private, source-pinned musl runtime.
# Ubuntu prerequisites (already installed): gcc, make, binutils, curl, tar, xz,
# file, sha256sum, Python 3. No root, distro development libraries or containers.
# Usage: bash build/build-squashfs-static.sh NEW_CANDIDATE_DIR
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# Pins match corresponding-source/linux-squashfs-musl/SHA256SUMS (musl, zlib and xz are also
# checked against their upstream signatures there). Change a version only together with that folder.
MUSL_SHA256=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
SQUASHFS_SHA256=9c4974e07c61547dae14af4ed1f358b7d04618ae194e54d6be72ee126f0d2f53
LZO_SHA256=c0f892943208266f9b6543b3ae308fab6284c5c90e627931446fb49b4221a072
ZLIB_SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
XZ_SHA256=4ce24038fd4221e0d13bc1a2de7a4db56e90b92b3bf75321f6c14be73f65de4b

for pin in "$MUSL_SHA256" "$SQUASHFS_SHA256" "$LZO_SHA256" "$ZLIB_SHA256" "$XZ_SHA256"; do
  if [[ ! "$pin" =~ ^[0-9a-f]{64}$ ]]; then
    echo "ERROR: unverified source SHA256: $pin; see corresponding-source/README.md" >&2
    exit 3
  fi
done
if [[ $# != 1 || $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
  echo "Usage (Linux x86_64): $0 NEW_CANDIDATE_DIR" >&2
  exit 2
fi
# mkdir intentionally refuses an existing output directory; never overwrite vendor/.
mkdir -- "$1"
OUTDIR=$(cd "$1" && pwd)
mkdir "$OUTDIR/sources"
WORK=$(mktemp -d "$OUTDIR/work.XXXXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
PREFIX="$WORK/prefix"
JOBS=${JOBS:-$(nproc)}
export LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-0}
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH LD_LIBRARY_PATH
unset CFLAGS CPPFLAGS LDFLAGS CC CONFIG_SITE
export CONFIG_SITE=/dev/null

fetch() {
  local name=$1 url=$2 sha=$3
  curl --fail --location --proto '=https' --tlsv1.2 "$url" -o "$OUTDIR/sources/$name"
  printf '%s  %s\n' "$sha" "$name" >> "$OUTDIR/sources/SHA256SUMS"
  (cd "$OUTDIR/sources"; printf '%s  %s\n' "$sha" "$name" | sha256sum -c -)
  tar -xf "$OUTDIR/sources/$name" -C "$WORK"
}
fetch musl-1.2.5.tar.gz https://musl.libc.org/releases/musl-1.2.5.tar.gz "$MUSL_SHA256"
fetch squashfs-tools-4.6.1.tar.gz https://github.com/plougher/squashfs-tools/archive/refs/tags/4.6.1.tar.gz "$SQUASHFS_SHA256"
fetch lzo-2.10.tar.gz https://www.oberhumer.com/opensource/lzo/download/lzo-2.10.tar.gz "$LZO_SHA256"
fetch zlib-1.3.2.tar.gz https://zlib.net/fossils/zlib-1.3.2.tar.gz "$ZLIB_SHA256"
fetch xz-5.8.4.tar.xz https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.xz "$XZ_SHA256"

# Build musl's documented gcc wrapper locally, pinning libc rather than depending
# on whatever version of Ubuntu musl-tools happens to be installed.
(cd "$WORK/musl-1.2.5"
 ./configure --prefix="$PREFIX" --syslibdir="$PREFIX/lib" --disable-shared
 make -j"$JOBS"
 make install)
export CC="$PREFIX/bin/musl-gcc"
export CFLAGS="-O2 -I$PREFIX/include"
export CPPFLAGS="-I$PREFIX/include"
export LDFLAGS="-static -L$PREFIX/lib"
(cd "$WORK/lzo-2.10"
 ./configure --prefix="$PREFIX" --enable-static --disable-shared
 make -j"$JOBS"
 make install)
(cd "$WORK/zlib-1.3.2"
 ./configure --prefix="$PREFIX" --static
 make -j"$JOBS"
 make install)
(cd "$WORK/xz-5.8.4"
 ./configure --prefix="$PREFIX" --enable-static --disable-shared --disable-nls \
   --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo \
   --disable-lzma-links --disable-scripts --disable-doc
 make -j"$JOBS"
 make install)

cd "$WORK/squashfs-tools-4.6.1/squashfs-tools"
# Absolute archives prevent accidentally linking Ubuntu's glibc-built libraries.
make -j"$JOBS" CC="$CC" GZIP_SUPPORT=1 LZO_SUPPORT=1 XZ_SUPPORT=1 \
  LZ4_SUPPORT=0 ZSTD_SUPPORT=0 LZMA_SUPPORT=0 LZMA_XZ_SUPPORT=0 \
  EXTRA_CFLAGS="-I$PREFIX/include" LDFLAGS="-static -L$PREFIX/lib" \
  LIBS="$PREFIX/lib/liblzo2.a $PREFIX/lib/libz.a $PREFIX/lib/liblzma.a -lpthread -lm" \
  mksquashfs unsquashfs
strip mksquashfs unsquashfs
for binary in mksquashfs unsquashfs; do
  file "$binary" | tee -a "$OUTDIR/file.txt" | grep -q 'statically linked' || {
    echo "ERROR: $binary not static" >&2; exit 5;
  }
done
# Run the installer's exact probe, with the summary enabled (no -quiet NSS workaround).
PYTHONPATH="$ROOT" python3 - "$PWD/mksquashfs" "$PWD/unsquashfs" <<'PY'
import sys
from diskos_installer.imagebuild import _probe_squashfs_tools
_probe_squashfs_tools(*sys.argv[1:])
PY
for binary in mksquashfs unsquashfs; do
  install -m 0755 "$binary" "$OUTDIR/$binary"
done
(cd "$OUTDIR"; sha256sum mksquashfs unsquashfs > SHA256SUMS; wc -c mksquashfs unsquashfs > sizes.txt)
echo "==> Static musl candidates and source inputs retained in $OUTDIR; vendor/ unchanged"
