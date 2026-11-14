# Vendored native tools - portability requirements

The installer bundles native binaries under `vendor/<os>-<arch>/` and calls them
via subprocess. For a distributable build these MUST be portable across the user's
machines - not just copies of the build host's dynamically-linked binaries.

| tool | why bundled | portability requirement |
|---|---|---|
| `usbboot` | Ingenic mask-ROM USB loader (the flasher) | Build **static** against libusb compiled `--disable-udev` (drops libudev + libcap; libusb falls back to sysfs enumeration). `usbboot` has **no direct udev symbols**; libusb uses the sysfs backend when udev is disabled. Prefer full-static (musl) or, at minimum, `$ORIGIN/lib` rpath + a bundled `libusb-1.0.so` in `vendor/<tag>/lib/`. Do **not** ship arbitrary host glibc `.so`s or rely on `LD_LIBRARY_PATH` as the primary strategy. |
| `mksquashfs`, `unsquashfs` | build/extract the rootfs image | Ship **static** squashfs-tools (with lzo support - the stock image uses `-comp lzo`). |
| `my_write6_dram.bin` | the DRAM NAND writer run on-device | Host-architecture independent MIPS blob (runs on the device). Copy as-is; the installer only runs this exact build (SHA-256 pinned in `flasher.py`). |
| `disc_spl_lpddr3.bin` | X2000 SPL | Same - device blob, copy as-is. |

## Status

- Linux release tools must be **fully static**. Build `usbboot` with
  `build/build-usbboot-pinned.sh`, which produces a candidate and manifest outside
  `vendor/`; copy both into `vendor/linux-x86_64/` together. Build `mksquashfs`
  and `unsquashfs` with `build/build-squashfs-static.sh`; it may produce dynamic
  binaries for local host use when static libraries are missing.
  `build/stage-release.sh` refuses any dynamic Linux host tool. The shipped tools are static
  against glibc (built with `gcc -static`), so no runtime `.so` is needed; note the
  static glibc/liblzo2/libusb licensing in `../NOTICE.md` and the corresponding source
  in `../corresponding-source/`. `my_write6_dram.bin` and `disc_spl_lpddr3.bin` are
  device blobs, copied as-is.
- `vendor/macos-*/` is produced on a Mac by **`vendor/setup-macos.sh`** (compiles
  `usbboot` from `src/usbboot/usbboot.c` against Homebrew libusb, gathers
  lzo-capable squashfs-tools, copies the device blobs, and runs `dylibbundler` so
  each tool has its non-system libraries under `vendor/macos-<arch>/lib/` - no Homebrew at
  runtime). `usbboot.c` uses libusb with host-specific branches, so
  it builds on macOS as-is. Then `build/build-macos.sh` packages the app.

## Windows x86_64 (build-only, Windows 10+)

Use MSYS2 bash with these packages already installed: MSYS `gcc gcc-libs
zlib-devel make patch tar gzip bzip2 diffutils binutils coreutils`,
and UCRT64 `mingw-w64-ucrt-x86_64-gcc`, `mingw-w64-ucrt-x86_64-pkgconf`.
The GCC package supplies the CRT/headers and depends on UCRT64 binutils.
From the installer directory run:

```sh
bash build/build-windows-tools.sh
```

No downloads or package installation occur. The recipe builds the pinned
squashfs-tools 4.6.1 archive with `corresponding-source/msys2-squashfs.patch`
(MSYS POSIX branches and little-endian constants), and usbboot from `src/`
against the pinned libusb 1.0.27 archive using UCRT64 static linking.
It writes `vendor/windows-x86_64/{mksquashfs.exe,unsquashfs.exe,usbboot.exe,
bind_winusb.exe,msys-2.0.dll}`, copies `my_write6_dram.bin` and
`disc_spl_lpddr3.bin` byte for byte from tracked `flash/` inputs, and writes
`MANIFEST.txt` with native input/output SHA256 hashes,
source versions and compiler/runtime package versions. Reproduce with the
recorded toolchain and runtime versions. The recipe fixes the build time,
strips binaries and uses fresh source workspaces.

The generated files and retained `vendor/.windows-build.*` and
`vendor/windows-x86_64/.driver-build.*` workspaces are ignored. Packaging uses
explicit file allowlists. Retain the printed manifest SHA256 outside the checkout;
flash-enabled `build/build-windows.ps1` requires it via `-NativeManifestSha256`
and verifies every native input/output hash and both blob copies. Follow
`windows-qualification.md` for the clean-clone runsheet.
An existing destination is refused. Keep the bundle outside the installed MSYS2
root, with no `etc/fstab`; production uses the isolated runtime's `/cygdrive`
prefix. Do not ship UCRT API-set DLLs: Windows 10+ supplies them (Microsoft Learn,
[Universal CRT deployment](https://learn.microsoft.com/en-us/cpp/windows/universal-crt-deployment)).
Only usbboot gets its twelve recorded UCRT import exemptions. Windows flashing
is disabled by default; use `build/build-windows.ps1 -EnableWindowsFlash`
to embed the capability, and follow the qualification runsheet for release packaging.
Redistribution also requires the MSYS2 package sources/notices described in
`../NOTICE.md`.

## Pinned macOS arm64 build inputs

`vendor/setup-macos.sh` requires Homebrew squashfs 4.7.5, libusb 1.0.30,
lzo 2.10, lz4 1.10.0, xz 5.8.4 and zstd 1.5.7_1 (Homebrew revision 1 of
upstream zstd 1.5.7). It fails if a different version is installed.
`dylibbundler` relocates exactly five non-system dylibs:
`libusb-1.0.0.dylib`, `liblzo2.2.dylib`, `liblz4.1.10.0.dylib`,
`liblzma.5.dylib`, and `libzstd.1.5.7.dylib`. `build/stage-release.sh`
rejects any extra or missing library. Source archives, the squashfs patch,
formula snapshots, hashes, licenses and LGPL libusb relink instructions are
in `corresponding-source/macos-arm64/README.md` and `licenses/README.md`.
The Linux release uses static tools and has no `vendor/linux-x86_64/lib/` directory.

## macOS: Intel + Apple Silicon coverage

`usbboot.c` and pyusb are portable; only the *build* is per-arch. Options:

1. **Native per-arch (most robust):** run `build/build-macos.sh` on an Intel Mac
   and on an Apple Silicon Mac to produce two binaries. Recommended.
2. **Single file via Rosetta:** build **x86_64** only (on an Intel Mac, or with an
   x86_64 Homebrew under `arch -x86_64`). The x86_64 app runs natively on Intel and
   under Rosetta 2 on M-series (macOS offers to install Rosetta on first run).
   One file covers both, at a small speed cost - fine for a flasher.
3. **Universal2 (advanced):** build both arches, `lipo` the vendor tools into fat
   binaries, and set `target_arch='universal2'` in the spec with a universal2
   Python. One native file for both, most work.

## Building portable usbboot (sketch)

1. Get the Ingenic `usbboot` source (the X2000 burn tool).
2. Build libusb static: `./configure --disable-udev --enable-static --disable-shared`
   to produce `libusb-1.0.a`.
3. Link `usbboot` against the static libusb; confirm with `ldd` that libudev/libcap
   are gone and only libc (or nothing, if fully static) remains.
4. Verify it still enters mask-ROM and flashes on a real unit before shipping.

## Durable flash identity helper

`build/build-flashstate.sh` builds `payload/diskos-flashstate` from
`src/flashstate/diskos_flashstate.c` (GPL-3.0-or-later). Set `CROSS` to the current
`mipsel-n2008-musl-cross/bin/mipsel-linux-musl-` prefix. It uses static musl,
strips symbols and omits the linker build ID; two builds with the same toolchain
must have identical SHA256 hashes. No host paths or build time are embedded.
Release builds must use the compiled-in `/etc` and `/usr/data` directories.

`diskos-flashstate check` returns 0 for matching IDs, 1 for a missing writable
record, 2 for a different valid writable ID, 3 for a malformed ordinary writable
record, 4 for storage or unexpected path-type errors, and 5 for a missing or
malformed RO record. Both records require exactly 32 lowercase hex digits and LF.
`complete` returns 0 only after checked temporary-file write, file fsync, close,
rename, directory fsync and read-back; errors return 4 (or 5 for invalid RO).
Both commands return 64 on invalid usage. Missing/inaccessible data roots and
symlinks are errors. Only `complete` replaces the managed writable ID and its
ordinary `.diskos-flash-id.tmp`; reset policy and prior data barriers belong to S97.
The helper is installed and checked via `_BOOT_DECISION_FILES`, independently
of the unchanged UI-only `/etc/diskos_manifest`.
