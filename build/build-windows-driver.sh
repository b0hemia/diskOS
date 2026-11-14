#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Offline, static x64 helper; takes only a new absolute output path at build time.
set -euo pipefail
[[ $# == 1 && $1 == /* && ! -e $1 && ! -L $1 ]] || {
    echo 'Usage: bash build/build-windows-driver.sh /absolute/new/bind_winusb.exe' >&2; exit 1;
}
[[ $(uname -s) == MSYS* && $(uname -m) == x86_64 ]] || {
    echo 'Requires MSYS2 x86_64 bash' >&2; exit 1;
}
export PATH=/ucrt64/bin:/usr/bin:/bin LC_ALL=C
unset CFLAGS CPPFLAGS LDFLAGS LIBRARY_PATH CPATH C_INCLUDE_PATH GCC_EXEC_PREFIX COMPILER_PATH
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
[[ $(/ucrt64/bin/gcc -dumpmachine) == x86_64-w64-mingw32 ]]
out=$1
work=$(mktemp -d "${out%/*}/.driver-build.XXXXXXXX")
# Retain source/resource artifacts; no package, catalog or certificate is created.
cd "$root/src/windows-driver"
/ucrt64/bin/windres -I. bind_winusb.rc -o "$work/manifest.o"
/ucrt64/bin/gcc -std=c99 -O2 -Wall -Wextra -Werror -nostdlib -fno-builtin \
    -fno-stack-protector "-ffile-prefix-map=$root=." -Wl,--entry=mainCRTStartup \
    bind_winusb.c "$work/manifest.o" -o "$work/bind_winusb.exe" -lkernel32 -lgcc
/ucrt64/bin/strip "$work/bind_winusb.exe"
# Without CRT startup, only Kernel32 can load before the System32 policy is set.
# Fail here as well as in the bundle recipe, including standalone helper builds.
imports=$(/ucrt64/bin/objdump -p "$work/bind_winusb.exe" | awk '/DLL Name:/ {print tolower($3)}')
[[ $imports == kernel32.dll ]] || { echo 'Helper must import only kernel32.dll' >&2; exit 1; }
/ucrt64/bin/objdump -p "$work/bind_winusb.exe" | \
    awk '/Delay Import Directory/ { n++; if ($3 !~ /^0+$/ || $4 !~ /^0+$/) bad=1 }
         END { exit (n == 1 && !bad) ? 0 : 1 }'
cp "$work/bind_winusb.exe" "$out"
echo "Built $out; retained workspace: $work"
