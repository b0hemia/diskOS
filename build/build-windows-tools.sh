#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Offline Windows x86_64 native tools. Retain the workspace for build evidence.
set -euo pipefail
trap 'echo "FAIL: build-windows-tools.sh line $LINENO" >&2' ERR
[[ $# == 0 ]] || { echo 'Usage: bash build/build-windows-tools.sh' >&2; exit 1; }
[[ $(uname -s) == MSYS* ]] || { echo 'Requires MSYS2 bash' >&2; exit 1; }
[[ $(uname -m) == x86_64 ]] || { echo 'Only Windows x86_64 is qualified for this recipe' >&2; exit 1; }
export PATH=/ucrt64/bin:/usr/bin:/bin LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=1791417600
unset MAKEFLAGS MFLAGS CFLAGS CPPFLAGS LDFLAGS LIBRARY_PATH CPATH PKG_CONFIG_PATH
unset C_INCLUDE_PATH GCC_EXEC_PREFIX COMPILER_PATH
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir="$root/corresponding-source"
out="$root/vendor/windows-x86_64"
[[ ! -e $out && ! -L $out ]] || { echo 'Refusing to replace an existing Windows bundle' >&2; exit 1; }
for tool in make patch tar gzip bzip2 sha256sum od cygpath pacman; do
    command -v "$tool" >/dev/null
done
for tool in /usr/bin/gcc /usr/bin/ar /usr/bin/ranlib /usr/bin/strip /ucrt64/bin/gcc /ucrt64/bin/ar \
            /ucrt64/bin/ranlib /ucrt64/bin/windres /ucrt64/bin/strip \
            /ucrt64/bin/objdump /ucrt64/bin/pkg-config; do
    [[ -x $tool ]] || { echo "Missing build tool: $tool" >&2; exit 1; }
done
[[ $(/ucrt64/bin/gcc -dumpmachine) == x86_64-w64-mingw32 ]]
# Match the release source asset exactly; refuse upgraded or older runtime inputs.
check_runtime_version() {
    [[ $1 == 'msys2-runtime 3.6.10-6' ]] || {
        echo 'Requires msys2-runtime 3.6.10-6 (pinned corresponding source)' >&2
        return 1
    }
}
runtime_package=$(pacman -Q msys2-runtime)
check_runtime_version "$runtime_package"
[[ $(pacman -Qqo /usr/bin/msys-2.0.dll) == msys2-runtime ]]
dlls=(msys-2.0.dll)
[[ -f /usr/lib/libz.a ]]
for dll in "${dlls[@]}"; do [[ -f /usr/bin/$dll ]]; done
verify() { printf '%s  %s\n' "$1" "$source_dir/$2" | sha256sum --check --strict -; }
verify 94201754b36121a9f022a190c75f718441df15402df32c2b520ca331a107511c squashfs-tools_4.6.1.orig.tar.gz
verify c0f892943208266f9b6543b3ae308fab6284c5c90e627931446fb49b4221a072 lzo2_2.10.orig.tar.gz
verify ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575 libusb-1.0_1.0.27.orig.tar.bz2
# All generated files, including configure's temporary files, stay under vendor/.
mkdir -p "$root/vendor"
work=$(mktemp -d "$root/vendor/.windows-build.XXXXXXXX")
export TMPDIR="$work" TMP="$work" TEMP="$work"
stage="$work/bundle"
mkdir "$stage"
# Device inputs are tracked under flash/; publish byte-for-byte copies with the tools.
cp "$root/flash/my_write6_dram.bin" "$root/flash/disc_spl_lpddr3.bin" "$stage/"
export CFLAGS="-O2 -ffile-prefix-map=$work=."
# Use the same vendored upstream LZO source as Linux, not an MSYS package DLL.
tar -xzf "$source_dir/lzo2_2.10.orig.tar.gz" -C "$work"
cd "$work/lzo-2.10"
./configure --prefix="$work/lzo-static" --enable-static --disable-shared \
    CC=/usr/bin/gcc AR=/usr/bin/ar RANLIB=/usr/bin/ranlib
make -j2
make install
tar -xzf "$source_dir/squashfs-tools_4.6.1.orig.tar.gz" -C "$work"
cd "$work/squashfs-tools-4.6.1"
patch --batch --forward --fuzz=0 -p1 < "$source_dir/msys2-squashfs.patch"
cd squashfs-tools
cat > "$work/endian-check.c" <<'CHECK'
#include "endian_compat.h"
#if !defined(__BYTE_ORDER) || !defined(__BIG_ENDIAN) || !defined(__LITTLE_ENDIAN)
#error Missing endian constants
#endif
#if __BIG_ENDIAN == __LITTLE_ENDIAN || __BYTE_ORDER != __LITTLE_ENDIAN
#error Requires distinct endian constants and a little-endian build
#endif
int main(void) { return 0; }
CHECK
/usr/bin/gcc -I. "$work/endian-check.c" -o "$work/endian-check.exe"
"$work/endian-check.exe"
make -j2 CC=/usr/bin/gcc EXTRA_CFLAGS="'-ffile-prefix-map=$work=.' '-I$work/lzo-static/include'" \
    EXTRA_LDFLAGS=-static-libgcc LIBS="-lpthread -lm '$work/lzo-static/lib/liblzo2.a' /usr/lib/libz.a" GZIP_SUPPORT=1 LZO_SUPPORT=1 XZ_SUPPORT=0 \
    mksquashfs unsquashfs
cp mksquashfs.exe unsquashfs.exe "$stage/"
/usr/bin/strip "$stage/mksquashfs.exe" "$stage/unsquashfs.exe"
for dll in "${dlls[@]}"; do cp "/usr/bin/$dll" "$stage/"; done
# Build the pinned libusb, rather than incorporating an unrecorded package library.
tar -xjf "$source_dir/libusb-1.0_1.0.27.orig.tar.bz2" -C "$work"
cd "$work/libusb-1.0.27"
./configure --host=x86_64-w64-mingw32 --prefix="$work/libusb-static" \
    --enable-static --disable-shared --disable-examples-build --disable-tests-build \
    CC=/ucrt64/bin/gcc AR=/ucrt64/bin/ar RANLIB=/ucrt64/bin/ranlib RC=/ucrt64/bin/windres
make -j2
make install
export PKG_CONFIG_LIBDIR="$work/libusb-static/lib/pkgconfig"
# pkg-config output deliberately splits compiler/linker flags into shell words.
# shellcheck disable=SC2046
/ucrt64/bin/gcc -O2 "-ffile-prefix-map=$root=." -std=c99 -static \
    -I"$work/libusb-static/include/libusb-1.0" -L"$work/libusb-static/lib" \
    -o "$stage/usbboot.exe" "$root/src/usbboot/usbboot.c" \
    $(/ucrt64/bin/pkg-config --static --libs-only-l --libs-only-other libusb-1.0)
/ucrt64/bin/strip "$stage/usbboot.exe"
bash "$root/build/build-windows-driver.sh" "$stage/bind_winusb.exe"
# objdump -p text on stdin: exactly one Delay Import Directory entry, RVA and size 0.
# The runtime verifier refuses delay imports, so the build must never publish one.
delay_free() {
    awk '/Delay Import Directory/ { n++; if ($3 !~ /^0+$/ || $4 !~ /^0+$/) bad = 1 }
         END { exit (n == 1 && !bad) ? 0 : 1 }'
}
# Per-file policy: squashfs has only MSYS + KERNEL32; runtime adds ntdll.
import_allowed() {
    local file=$1 name=$2 part
    case "$file:$name" in
        mksquashfs.exe:kernel32.dll|unsquashfs.exe:kernel32.dll|usbboot.exe:kernel32.dll|bind_winusb.exe:kernel32.dll|msys-2.0.dll:kernel32.dll|msys-2.0.dll:ntdll.dll) return 0 ;;
        mksquashfs.exe:msys-2.0.dll|unsquashfs.exe:msys-2.0.dll) return 0 ;;
        usbboot.exe:api-ms-win-crt-*-l1-1-0.dll)
            part=${name#api-ms-win-crt-}; part=${part%-l1-1-0.dll}
            case "$part" in
                convert|environment|filesystem|heap|locale|math|private|runtime|stdio|string|time|utility) return 0 ;;
            esac ;;
    esac
    echo "Unexpected import: $name in $file" >&2
    return 1
}
for file in "$stage/"*.exe "$stage/"*.dll; do
    /ucrt64/bin/objdump -p "$file" | delay_free || { echo "Delay imports present or unreadable: $file" >&2; exit 1; }
    imports=$(/ucrt64/bin/objdump -p "$file" | awk '/DLL Name:/ {print tolower($3)}')
    [[ -n $imports ]] || { echo "No imports found: $file" >&2; exit 1; }
    while IFS= read -r name; do
        import_allowed "${file##*/}" "$name"
    done <<< "$imports"
done
# The staged MSYS runtime is adjacent and has no etc/fstab. Use default drive paths.
drive_path() {
    local win drive
    win=$(cygpath -am -- "$1"); drive=${win:0:1}
    printf '/cygdrive/%s/%s' "${drive,,}" "${win:3}"
}
# A tool started from this MSYS shell would inherit the shell's mount table, so
# launch the probes through cmd, a native parent, as the installer does.
# Disable MSYS argument conversion: the /cygdrive arguments must arrive verbatim.
native() { local exe; exe=$(cygpath -w "$1"); MSYS2_ARG_CONV_EXCL='*' cmd /c "$exe" "${@:2}"; }
mkdir "$work/probe-source"
printf 'Windows bundle LZO endian probe\n' > "$work/probe-source/probe"
native "$stage/mksquashfs.exe" "$(drive_path "$work/probe-source")" "$(drive_path "$work/probe.sq")" \
    -comp lzo -b 131072 -processors 2 -mem 256M -noappend -no-progress -quiet -exit-on-error
magic=$(od -An -tx1 -N4 "$work/probe.sq")
[[ ${magic//[[:space:]]/} == 68737173 ]]
native "$stage/unsquashfs.exe" -processors 2 -no-progress -strict-errors \
    -pf "$(drive_path "$work/probe.pf")" "$(drive_path "$work/probe.sq")"
[[ -s "$work/probe.pf" ]]
# usage() always exits 1 (src/usbboot/usbboot.c); require exactly that and its text.
help_rc=0; help_out=$("$stage/usbboot.exe" --help 2>&1) || help_rc=$?
[[ $help_rc == 1 && $help_out == *"Usage: usbboot"* ]]
cd "$stage"
{
    printf 'squashfs-tools=4.6.1\nlzo=2.10\nlibusb=1.0.27\nSOURCE_DATE_EPOCH=%s\n' "$SOURCE_DATE_EPOCH"
    # Package versions identify the exact compilers and runtime DLL inputs.
    pacman -Q bash coreutils make patch tar gzip bzip2 diffutils binutils \
        gcc msys2-runtime zlib zlib-devel gcc-libs \
        mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-binutils \
        mingw-w64-ucrt-x86_64-crt mingw-w64-ucrt-x86_64-winpthreads mingw-w64-ucrt-x86_64-headers mingw-w64-ucrt-x86_64-pkgconf
    printf 'runtime-package=%s\n' "$runtime_package"
    printf '\nInput SHA256 (paths relative to installer):\n'
    (cd "$root" && sha256sum corresponding-source/squashfs-tools_4.6.1.orig.tar.gz \
        corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2 corresponding-source/lzo2_2.10.orig.tar.gz \
        corresponding-source/msys2-squashfs.patch \
        src/usbboot/usbboot.c build/build-windows-tools.sh \
        src/windows-driver/bind_winusb.c src/windows-driver/bind_winusb.manifest \
        src/windows-driver/bind_winusb.rc build/build-windows-driver.sh)
    printf '\nOutput SHA256:\n'
    sha256sum mksquashfs.exe unsquashfs.exe usbboot.exe bind_winusb.exe "${dlls[@]}"
} > MANIFEST.txt
# Publish the complete local result only after the build and probes succeeded.
mv -T "$stage" "$out"
echo "PASS: Windows bundle in vendor/windows-x86_64; retained workspace: $work"
# Retain this digest outside the checkout and supply it to the enabled freeze.
sha256sum "$out/MANIFEST.txt"
