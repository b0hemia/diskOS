#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Offline candidate build. Never installs into vendor/.
set -eu
export LC_ALL=C TZ=UTC
umask 022
fail() { echo "FAIL: $*" >&2; exit 1; }
image_file= out= epoch=
while [ "$#" -gt 0 ]; do
    [ "$#" -ge 2 ] || fail 'expected --image-id FILE --out DIR --epoch N'
    case "$1" in
        --image-id) image_file=$2;; --out) out=$2;; --epoch) epoch=$2;;
        *) fail "unknown argument: $1";;
    esac
    shift 2
done
[ -n "$image_file" ] && [ -n "$out" ] || fail 'image file and output directory required'
case "$epoch" in ''|*[!0-9]*) fail 'epoch must be a nonnegative integer';; esac
[ "$(id -u)" != 0 ] || fail 'run as an ordinary user with rootless Podman'
image=$(cat "$image_file")
printf '%s\n' "$image" | grep -Eq '^(sha256:)?[0-9a-f]{64}$' || fail 'image ID must be a full digest'
image="sha256:${image#sha256:}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ ! -e "$out" ] && [ ! -L "$out" ] || fail 'refusing to overwrite output directory'
mkdir -- "$out"
out=$(CDPATH= cd -- "$out" && pwd)
printf '%s\n' "$image" > "$out/image.id"
printf '%s\n' "$epoch" > "$out/source-date-epoch.txt"
podman image inspect --format '{{.Id}}' "$image" > "$out/image-inspect.id"
inspected=$(cat "$out/image-inspect.id")
[ "sha256:${inspected#sha256:}" = "$image" ] || fail 'image ID mismatch'
podman run --interactive --pull=never --network=none --read-only --userns=keep-id --user "$(id -u):$(id -g)" \
    --cap-drop=ALL --security-opt=no-new-privileges \
    -e SOURCE_DATE_EPOCH="$epoch" -e LC_ALL=C -e TZ=UTC -e HOME=/out/tmp -e TMPDIR=/out/tmp \
    -v "$root:/src:ro" -v "$out:/out:rw" --entrypoint /bin/sh "$image" -es \
    > "$out/build.log" 2>&1 <<'BUILD'
umask 022
test "$(dpkg --print-architecture)" = amd64
for p in libc6 libc6-dev libc-bin libc-dev-bin; do
    test "$(dpkg-query -W -f='${Version}' "$p")" = 2.39-0ubuntu8.8
done
cd /src
printf '%s  %s\n' ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575 \
    corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2 | sha256sum -c -
sha256sum src/usbboot/usbboot.c build/build-usbboot-pinned.sh build/prepare-usbboot-env.sh \
    build/usbboot_manifest.py corresponding-source/glibc_2.39* \
    corresponding-source/libusb-1.0_1.0.27* > /out/input-sha256.txt
dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\t${source:Package}\t${source:Version}\n' \
    > /out/packages.tsv
gcc-13 -v > /out/compiler.txt 2>&1
gcc-13 -dumpfullversion > /out/compiler-version.txt
cp /usr/share/doc/gcc-13-base/copyright /out/gcc-runtime-copyright.txt
mkdir /out/work /out/tmp
tar -xjf corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2 -C /out/work
cd /out/work/libusb-1.0.27
CFLAGS='-O2 -march=x86-64 -mtune=generic -ffile-prefix-map=/src=. -ffile-prefix-map=/out=.'
export CFLAGS
printf '%s\n' "$CFLAGS" > /out/libusb-cflags.txt
./configure --disable-udev --enable-static --disable-shared \
    --disable-examples-build --disable-tests-build CC=gcc-13 > /out/configure.log 2>&1
make -j2 > /out/make.log 2>&1
cp config.log /out/config.log
printf '%s\n' "$CFLAGS -std=c99 -Ilibusb -static -no-pie -Wl,-Map,/out/usbboot.map -lpthread" \
    > /out/link-flags.txt
gcc-13 $CFLAGS -std=c99 -Ilibusb -static -no-pie -Wl,-Map,/out/usbboot.map \
    -o /out/usbboot.unstripped /src/src/usbboot/usbboot.c libusb/.libs/libusb-1.0.a -lpthread \
    > /out/link.log 2>&1
cp /out/usbboot.unstripped /out/usbboot
strip /out/usbboot
file /out/usbboot > /out/file.txt
readelf -hW /out/usbboot > /out/elf-header.txt
readelf -lW /out/usbboot > /out/program-headers.txt
readelf -dW /out/usbboot > /out/dynamic.txt
if grep -q INTERP /out/program-headers.txt || grep -q NEEDED /out/dynamic.txt; then
    echo 'FAIL: usbboot has dynamic runtime dependencies' >&2
    exit 1
fi
rc=0
/out/usbboot --help > /out/help.txt 2>&1 || rc=$?
test "$rc" = 1
grep -q 'Usage: usbboot' /out/help.txt
cd /out
sha256sum usbboot > output-sha256.txt
BUILD
# Python is required on the host only; the build image needs no Python package.
python3 "$root/build/usbboot_manifest.py" create "$root" "$out"
printf 'Candidate and evidence: %s\n' "$out"
