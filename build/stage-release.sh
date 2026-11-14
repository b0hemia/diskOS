#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# stage-release.sh - assemble the release tarball(s) into build/release/ and generate SHA256SUMS.
#
# The release ships as SOURCE (run with your own Python via ./install.sh) plus the native flash
# tools for the target platform - NOT a bundled onefile binary. Each tarball is self-contained:
# extract it, run ./install.sh once, then ./diskos-installer.
#
# Usage:
#   build/stage-release.sh [--artifact NAME ...] [TAG ...]
#   Windows requires --windows-zip PATH, --windows-sha256 HASH and --windows-source-commit COMMIT
#   from the qualification record for that exact ZIP.
# --artifact names existing build/release files in the COMPLETE release list.
# Needs a git checkout with the release committed (git archive of DISKOS_REF, default HEAD).
# Linux usbboot: build/build-usbboot-pinned.sh; macOS: vendor/setup-macos.sh.
set -euo pipefail
cd "$(dirname "$0")/.."                        # installer/
ROOT=$(pwd)
REL="$ROOT/build/release"
mkdir -p "$REL"

fail() { echo "FAIL: $*" >&2; exit 1; }
artifacts=(); tags=(); windows_zip=; windows_sha=; windows_commit=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --artifact|--windows-zip|--windows-sha256|--windows-source-commit)
      [ "$#" -ge 2 ] || fail "missing value for $1"
      case "$1" in
        --artifact) artifacts+=("$2");;
        --windows-zip) windows_zip=$2;;
        --windows-sha256) windows_sha=$2;;
        --windows-source-commit) windows_commit=$2;;
      esac
      shift 2;;
    --*) fail "unknown option: $1";;
    *) tags+=("$1"); shift;;
  esac
done
for name in "${artifacts[@]}"; do
  [[ "$name" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ && "$name" != SHA256SUMS && "$name" != SHA256SUMS.new ]] || fail "bad artifact name: $name"
  [ -f "$REL/$name" ] && [ ! -L "$REL/$name" ] || fail "missing regular artifact: $name"
done
for tag in "${tags[@]}"; do
  case "$tag" in linux-x86_64|linux-arm64|macos-x86_64|macos-arm64|windows-x86_64) ;; *) fail "unsupported tag: $tag";; esac
done
if [[ " ${tags[*]} " == *" windows-x86_64 "* ]]; then
  [[ "$windows_commit" =~ ^[0-9a-f]{40}$ ]] || fail "recorded qualified Windows source commit is required (full SHA)"
  [[ "$windows_sha" =~ ^[0-9a-f]{64}$ ]] || fail "recorded qualified Windows SHA256 is required"
  [ -f "$windows_zip" ] && [ ! -L "$windows_zip" ] || fail "qualified Windows ZIP is required"
  windows_zip=$(realpath "$windows_zip")
  [ "$(sha256sum "$windows_zip" | cut -d ' ' -f 1)" = "$windows_sha" ] || fail "qualified ZIP hash mismatch"
elif [ -n "$windows_zip$windows_sha$windows_commit" ]; then
  fail "Windows ZIP options require windows-x86_64"
fi
set -- "${tags[@]}"
# default to the host platform tag if none given
if [ "$#" -eq 0 ]; then
  _os=$(uname -s | tr '[:upper:]' '[:lower:]'); case "$_os" in darwin) _os=macos;; esac
  _arch=$(uname -m); case "$_arch" in amd64) _arch=x86_64;; aarch64|arm64) _arch=arm64;; esac
  set -- "${_os}-${_arch}"
fi

# The tarball is built from `git archive` of REF (default HEAD), never from the working tree, so untracked
# local files (scratch tools, unused assets, signing keys) cannot leak in. Commit what belongs in the release
# first; set DISKOS_REF=<tag|commit> to stage something other than HEAD.
REF=${DISKOS_REF:-HEAD}
REV=$(git -C "$ROOT" rev-parse --verify --quiet "$REF^{commit}") || { echo "bad ref: $REF" >&2; exit 1; }
[ "$(git -C "$ROOT" rev-parse --show-toplevel)" = "$ROOT" ] || { echo "installer/ must be the git root" >&2; exit 1; }
[ -z "$windows_zip" ] || [ "$REV" = "$windows_commit" ] || fail "archive revision differs from qualified Windows source commit: $REV != $windows_commit"

# Paths that must NEVER appear in a tarball, even if someone commits them by mistake (checked after extract).
FORBIDDEN=(signing '*.key' '*.squashfs' '*.sqfs' 'diskos_dev*.bin' 'diskos_public*.bin' '*_recovery.bin'
  payload/diskos-prefstat ui/tools/diskos_prefstat.c _retired_unlicensed flash/qual_lpddr3.sh flash/scan_check.sh
  flash/my_write5_scan_dram.bin)
# Unused local screenshots: only these docs/assets images may ever ship if untracked ones appear.
BLOCK_ASSETS=(audiobook-np.png books.png chapters.png quicksettings.png)

# Host-side tools taken from vendor/<TAG>/ (untracked/ignored build outputs): an EXPLICIT allowlist only.
REQ_VENDOR=(usbboot mksquashfs unsquashfs my_write6_dram.bin disc_spl_lpddr3.bin)
# Payload that MUST exist in the archive (committed at REF); nothing else from the working tree is added.
REQ_PAYLOAD=(S97diskos_names diskos-names diskos-udhcpc diskos-network-names-v1 mq_ui S96diskos_select S97diskos_install S99usbserial diskos-debug.sh dropbearmulti diskos-rmguard diskos-selected diskos-bootprobe diskos-artdec diskos-launch diskos-flashstate diskos-verify.sh diskos-root.pub.pem)
MACOS_DYLIBS=(libusb-1.0.0.dylib liblzo2.2.dylib liblz4.1.10.0.dylib liblzma.5.dylib libzstd.1.5.7.dylib)
# Other files that MUST be in the archive (writer + its build inputs, launcher source, verifier support).
REQ_ARCHIVE=(flash/my_write6_dram.bin flash/my_write6.c flash/build_nand.sh flash/dram_head.bin flash/dram_head.S flash/dram_head.ld flash/dram.ld
  flash/sha256_min.h flash/disc_spl_lpddr3.bin src/launcher/diskos_launch.c build/build-launcher.sh
  src/flashstate/diskos_flashstate.c build/build-flashstate.sh
  diskos_installer/basegate.py diskos_installer/assets/wordmark.png diskos_installer/assets/wordmark@2x.png
  diskos_installer/assets/display.png diskos_installer/assets/display@1.5x.png diskos_installer/assets/display@2x.png
  diskos_installer/assets/display.json diskos_installer/assets/disc.png diskos_installer/assets/disc@1.5x.png
  diskos_installer/assets/disc@2x.png
  diskos_installer/assets/LICENSE-archivo-OFL.txt diskos_installer/assets/LICENSE-michroma-OFL.txt ui/tools/diskos_artdec.c ui/tools/stb_image.h vendor/setup-macos.sh)

planned=(RELEASE_NOTES.md)
for TAG in "$@"; do planned+=("diskos-installer-$TAG.tar.gz"); done
[ -z "$windows_zip" ] || planned+=(diskos-installer-windows-x86_64.zip)
check_inventory() {
python3 - "$REL" "$1" "${planned[@]}" "${artifacts[@]}" <<'PYSUM'
import re, sys
from pathlib import Path
root = Path(sys.argv[1])
names = set(sys.argv[3:])
entries = {p.name: p for p in root.iterdir()}
extra = entries.keys() - names - {'SHA256SUMS'}
if extra:
    raise SystemExit('Unexpected release entries; pass the complete --artifact list: ' + ', '.join(sorted(extra)))
for name, path in entries.items():
    if path.is_symlink() or not path.is_file():
        raise SystemExit('Release entry is not a regular file: ' + name)
if sys.argv[2] == 'final' and names - entries.keys():
    raise SystemExit('Missing release artifacts: ' + ', '.join(sorted(names - entries.keys())))
previous = root / 'SHA256SUMS'
if previous.exists():
    for line in previous.read_text(encoding='ascii').splitlines():
        match = re.fullmatch(r'[0-9a-f]{64}  ([A-Za-z0-9][A-Za-z0-9._-]*)', line)
        if not match or match[1] not in names:
            raise SystemExit('Previous artifact omitted; pass the complete --artifact list: ' + line)
PYSUM
}
check_inventory preflight
# Refuse every requested platform's device copies before writing any artifact.
python3 - "$ROOT" "$REV" "$@" <<'PYBLOBS'
import subprocess
import sys
from pathlib import Path
root = Path(sys.argv[1])
sys.path.insert(0, str(root / 'build'))
from verify_git_blobs import BlobGuard, DEVICE
try:
    BlobGuard(root, sys.argv[2]).verify(
        (root / 'vendor' / tag / name, 'flash/' + name)
        for tag in sys.argv[3:] for name in DEVICE)
except (OSError, ValueError, subprocess.CalledProcessError) as exc:
    raise SystemExit('Git blob verification failed: ' + str(exc))
PYBLOBS
made=()
for TAG in "$@"; do
  echo "== staging $TAG (from $REF) =="
  miss=()
  required_vendor=("${REQ_VENDOR[@]}")
  if [[ "$TAG" == linux-* ]]; then
    required_vendor+=(usbboot.manifest)
  fi
  if [ "$TAG" = windows-x86_64 ]; then
    required_vendor=(usbboot.exe mksquashfs.exe unsquashfs.exe bind_winusb.exe msys-2.0.dll
      my_write6_dram.bin disc_spl_lpddr3.bin MANIFEST.txt)
  fi
  for f in "${required_vendor[@]}"; do [ -f "vendor/$TAG/$f" ] || miss+=("vendor/$TAG/$f"); done
  if [ "${#miss[@]}" -gt 0 ]; then
    echo "  FAIL $TAG - missing: ${miss[*]}" >&2
    echo "  (build the native tools for $TAG first; every requested platform must stage, or nothing is published)" >&2
    exit 1
  fi

  if [[ "$TAG" == linux-* ]]; then
    if [ -e "vendor/$TAG/lib" ] || [ -L "vendor/$TAG/lib" ]; then
      echo "  FAIL $TAG - static Linux tools must not have vendor/$TAG/lib" >&2
      exit 1
    fi
    command -v file >/dev/null 2>&1 || { echo "  FAIL $TAG - file is required to check native tools" >&2; exit 1; }
    for f in usbboot mksquashfs unsquashfs; do
      path="vendor/$TAG/$f"
      kind=$(file -b "$path")
      if [ -L "$path" ] || [[ "$kind" != *"statically linked"* ]]; then
        echo "  FAIL $TAG - $path is not a regular statically linked binary: $kind" >&2
        exit 1
      fi
    done
  fi
  if [[ "$TAG" == macos-* ]]; then
    if [ ! -d "vendor/$TAG/lib" ] || [ -L "vendor/$TAG/lib" ]; then
      echo "  FAIL $TAG - vendor/$TAG/lib must be a directory" >&2
      exit 1
    fi
  fi

  stage=$(mktemp -d)
  dest="$stage/diskos-installer"
  mkdir -p "$dest"
  git -c core.autocrlf=false -c core.eol=lf -C "$ROOT" archive --format=tar "$REV" | tar -x -C "$dest"

  # required files must be committed at REF (an untracked file is NOT in the archive)
  miss=()
  for f in "${REQ_ARCHIVE[@]}"; do [ -f "$dest/$f" ] || miss+=("$f"); done
  for f in "${REQ_PAYLOAD[@]}";  do [ -f "$dest/payload/$f" ] || miss+=("payload/$f"); done
  if [ "${#miss[@]}" -gt 0 ]; then
    echo "  FAIL - required files missing from git archive of $REF (commit them first): ${miss[*]}" >&2
    rm -rf "$stage"; exit 1
  fi
  key="$dest/payload/diskos-root.pub.pem"
  if [ -L "$key" ] || grep -q 'PRIVATE KEY' "$key" ||
     [ "$(head -n 1 "$key")" != '-----BEGIN PUBLIC KEY-----' ] ||
     [ "$(tail -n 1 "$key")" != '-----END PUBLIC KEY-----' ] ||
     ! openssl pkey -pubin -in "$key" -text -noout 2>/dev/null | grep -Eq 'ASN1 OID: prime256v1|NIST CURVE: P-256'; then
    echo "  FAIL - archived payload/diskos-root.pub.pem must be a P-256 PUBLIC KEY PEM" >&2
    rm -rf "$stage"; exit 1
  fi
  # the committed payload must be the same bytes as the working-tree build the tests ran against
  for f in "${REQ_PAYLOAD[@]}"; do
    if [ -f "payload/$f" ] && ! cmp -s "payload/$f" "$dest/payload/$f"; then
      echo "  FAIL - payload/$f differs between working tree and $REF (commit or revert it)" >&2
      rm -rf "$stage"; exit 1
    fi
  done

  # only this TAG's allowlisted vendor tools; drop any other vendor content from the archive
  find "$dest/vendor" -mindepth 1 -maxdepth 1 ! -name setup-macos.sh -exec rm -rf {} +
  mkdir -p "$dest/vendor/$TAG"
  for f in "${required_vendor[@]}"; do cp -a "vendor/$TAG/$f" "$dest/vendor/$TAG/$f"; done
  python3 "$ROOT/build/verify_git_blobs.py" "$ROOT" "$dest" "$TAG" --ref "$REV"
  if [[ "$TAG" == linux-* ]]; then
    # Use the current validator, but compare inputs only with the archive export.
    python3 "$ROOT/build/usbboot_manifest.py" check "$dest" "$dest/vendor/$TAG" "$TAG"
  fi
  if [[ "$TAG" == macos-* ]]; then
    mkdir -p "$dest/vendor/$TAG/lib"
    while IFS= read -r -d '' f; do
      name=${f##*/}
      allowed=0
      for expected in "${MACOS_DYLIBS[@]}"; do [ "$name" = "$expected" ] && allowed=1; done
      if [ "$allowed" -ne 1 ] || [ ! -f "$f" ] || [ -L "$f" ]; then
        echo "  FAIL $TAG - unexpected or non-regular library: $f" >&2
        rm -rf "$stage"; exit 1
      fi
    done < <(find "vendor/$TAG/lib" -mindepth 1 -maxdepth 1 -print0)
    for name in "${MACOS_DYLIBS[@]}"; do
      f="vendor/$TAG/lib/$name"
      if [ ! -f "$f" ] || [ -L "$f" ]; then
        echo "  FAIL $TAG - missing regular library: $f" >&2
        rm -rf "$stage"; exit 1
      fi
      cp "$f" "$dest/vendor/$TAG/lib/$name"
    done
    # Check staged Mach-O load commands, including rpaths. Linux hosts have no otool.
    for f in "$dest/vendor/$TAG/usbboot" "$dest/vendor/$TAG/mksquashfs" \
             "$dest/vendor/$TAG/unsquashfs" "$dest/vendor/$TAG/lib/"*.dylib; do
      if ! file -b "$f" | grep -q 'Mach-O'; then
        echo "  FAIL $TAG - staged native file is not Mach-O: $f" >&2
        rm -rf "$stage"; exit 1
      fi
      if command -v otool >/dev/null 2>&1; then
        refs=$(otool -l "$f") || { echo "  FAIL $TAG - otool failed: $f" >&2; rm -rf "$stage"; exit 1; }
      else
        refs=$(strings "$f") || { echo "  FAIL $TAG - strings failed: $f" >&2; rm -rf "$stage"; exit 1; }
      fi
      if grep -Eq '/opt/homebrew|/usr/local|Cellar' <<< "$refs"; then
        echo "  FAIL $TAG - staged Mach-O references Homebrew or a local library path: $f" >&2
        rm -rf "$stage"; exit 1
      fi
    done
  fi

  if [ "$TAG" = windows-x86_64 ]; then
    python3 - "$dest" "$windows_zip" "${REQ_PAYLOAD[@]}" <<'PYWIN'
import hashlib, re, sys, zipfile
from pathlib import Path
root = Path(sys.argv[1])
vendor = root / 'vendor/windows-x86_64'
text = (vendor / 'MANIFEST.txt').read_text(encoding='ascii')
def records(section):
    body = text.split(section + ':\n', 1)[1].split('\n\n', 1)[0]
    result = {}
    for line in body.splitlines():
        match = re.fullmatch(r'([0-9a-f]{64}) [ *]([^\\\r\n]+)', line)
        if not match:
            raise ValueError('Malformed manifest record')
        sha, name = match.groups()
        if name.startswith('/') or ':' in name or any(p in ('', '.', '..') for p in name.split('/')) or name in result:
            raise ValueError('Unsafe/duplicate manifest path')
        path = root / name if section.startswith('Input') else vendor / name
        if path.is_symlink() or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != sha:
            raise ValueError('Manifest hash mismatch: ' + name)
        result[name] = sha
    return result
inputs = records('Input SHA256 (paths relative to installer)')
outputs = records('Output SHA256')
required = {'src/usbboot/usbboot.c', 'build/build-windows-tools.sh', 'build/build-windows-driver.sh',
    'src/windows-driver/bind_winusb.c', 'src/windows-driver/bind_winusb.manifest',
    'src/windows-driver/bind_winusb.rc', 'corresponding-source/msys2-squashfs.patch',
    'corresponding-source/squashfs-tools_4.6.1.orig.tar.gz',
    'corresponding-source/lzo2_2.10.orig.tar.gz', 'corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2'}
if not required <= inputs.keys() or set(outputs) != {'usbboot.exe', 'mksquashfs.exe', 'unsquashfs.exe', 'bind_winusb.exe', 'msys-2.0.dll'}:
    raise ValueError('Incomplete Windows manifest inventory')
with zipfile.ZipFile(sys.argv[2]) as package:
    prefix = 'diskos-installer-windows-x86_64/'
    paths = [root / n for n in inputs] + list(vendor.iterdir()) + [root / 'payload' / n for n in sys.argv[3:]]
    for path in paths:
        relative = path.relative_to(root).as_posix()
        if path.is_symlink() or package.read(prefix + '_internal/' + relative) != path.read_bytes():
            raise ValueError('Qualified ZIP differs from source/native inputs: ' + relative)
    if package.read(prefix + 'RECOVERY.txt') != (root / 'RECOVERY.txt').read_bytes():
        raise ValueError('Qualified recovery text differs from archived source')
PYWIN
  fi

  # forbidden-path check on the final tree
  bad=()
  for pat in "${FORBIDDEN[@]}"; do
    while IFS= read -r m; do [ -n "$m" ] && bad+=("$m"); done < <(
      case "$pat" in */*) [ -e "$dest/$pat" ] && echo "$pat";; *) find "$dest" -name "$pat" -print | sed "s|^$dest/||";; esac)
  done
  # any PEM other than the required PUBLIC OTA root key, and any file holding private-key material
  while IFS= read -r m; do bad+=("${m#$dest/}"); done < <(find "$dest" -name '*.pem' ! -path "$dest/payload/diskos-root.pub.pem")
  while IFS= read -r m; do bad+=("${m#$dest/}"); done < <(grep -rIlE -e '^-----BEGIN [A-Z ]*PRIVATE KEY-----'  "$dest" 2>/dev/null || true)
  for a in "${BLOCK_ASSETS[@]}"; do [ -e "$dest/docs/assets/$a" ] && bad+=("docs/assets/$a"); done
  if [ "${#bad[@]}" -gt 0 ]; then
    echo "  FAIL - forbidden files in the staged tree: ${bad[*]}" >&2
    rm -rf "$stage"; exit 1
  fi

  out="$REL/diskos-installer-$TAG.tar.gz"
  tar -czf "$out" -C "$stage" diskos-installer
  rm -rf "$stage"
  echo "  -> $out"
  made+=("diskos-installer-$TAG.tar.gz")
done

[ "${#made[@]}" -gt 0 ] || { echo "nothing staged." >&2; exit 1; }

if [ -n "$windows_zip" ]; then
  zip_name=diskos-installer-windows-x86_64.zip
  if [ "$windows_zip" != "$REL/$zip_name" ]; then cp "$windows_zip" "$REL/$zip_name"; fi
  [ "$(sha256sum "$REL/$zip_name" | cut -d ' ' -f 1)" = "$windows_sha" ] || fail "staged ZIP hash mismatch"
  made+=("$zip_name")
fi

NOTES="$REL/RELEASE_NOTES.md"
cat > "$NOTES" <<'EOF'
# diskOS installer - release

Installer for diskOS on the FiiO Snowsky Disc. Runs from source with your own Python (GUI + CLI). It
builds a diskOS image *from your own official FiiO firmware zip* (FiiO's rootfs is never
redistributed), flashes over mask-ROM with a bad-block-aware verify-every-block writer, and keeps a
checksum-verified stock image so **restore/remove is one action**.

## What's in the tarball
Source + the native flash tools for the platform. **No bundled Python** - you run it with your own.

## Setup
```
tar -xzf diskos-installer-<platform>.tar.gz
cd diskos-installer
./install.sh          # builds a local .venv, installs pyusb + pycryptodome
./diskos-installer doctor
```
Needs Python 3.8+. The GUI additionally needs Tk. Release bundles include the native flash and
squashfs tools; the macOS bundle includes a private libusb. On Linux, device detection needs the
system libusb-1.0 package. `install.sh` checks for missing components.

## Install / restore / remove
See the bundled `README.md`. Requires putting the device in mask-ROM (power off, hold Vol-Down, plug
USB). ~20 min; normally recoverable via mask-ROM, but not guaranteed.

## Honest status
Enthusiast flasher. Linux flashing is tested on real hardware; see the release notes for the test
status of each firmware version. macOS: image builds work, and device flashing has been verified by a
user on Apple Silicon (Intel is expected to work but is not confirmed); see the README.
Not affiliated with FiiO.
EOF
echo "  -> $NOTES"
made+=(RELEASE_NOTES.md)
check_inventory final
echo "== generating complete SHA256SUMS =="
complete=()
while IFS= read -r name; do complete+=("$name"); done < <(printf '%s\n' "${made[@]}" "${artifacts[@]}" | LC_ALL=C sort -u)
( cd "$REL" && sha256sum "${complete[@]}" > SHA256SUMS.new && mv SHA256SUMS.new SHA256SUMS )
cat "$REL/SHA256SUMS"
echo "Done. Release artifacts are the explicit files listed in SHA256SUMS."
