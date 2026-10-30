# Corresponding source for the shipped GPL/LGPL native binaries

The installer ships prebuilt native tools under `vendor/<os>-<arch>/`. Some of them are, or
statically link, **GPL-2.0** / **LGPL-2.1** code. To satisfy those licenses this directory carries the
**complete corresponding source** for the Linux upstream versions those binaries were built from - so
the source travels **from the same place** as the binaries (GPL-2.0 section 3(a) + the "same place" clause
in section 3's final paragraph; LGPL-2.1 section 6(a)/(d)).

Items named `<name>_<version>*` are full Debian source packages (`.orig` upstream tarball + `.debian`
packaging + `.dsc`); extract with `dpkg-source -x <name>.dsc` (or unpack the `.orig` tarball directly).
Items under `linux-squashfs-musl/` are the exact upstream release archives.

| Shipped binary | Component here | Version | License | Source files |
|---|---|---|---|---|
| Linux `mksquashfs` / `unsquashfs` | **squashfs-tools** | 4.6.1 | GPL-2.0 | `linux-squashfs-musl/squashfs-tools-4.6.1.tar.gz` |
| (statically linked into the above) | **LZO** | 2.10 | GPL-2.0-or-later | `linux-squashfs-musl/lzo-2.10.tar.gz` |
| (static C runtime of the above) | **musl** | 1.2.5 | MIT | `linux-squashfs-musl/musl-1.2.5.tar.gz` |
| Windows `mksquashfs.exe` / `unsquashfs.exe` | squashfs-tools + LZO | 4.6.1 / 2.10 | GPL-2.0 | `squashfs-tools_4.6.1*`, `lzo2_2.10*` (see below) |
| Linux `usbboot` | **upstream libusb-1.0** (statically linked) | 1.0.27 | LGPL-2.1-or-later | `libusb-1.0_1.0.27*` |
| Linux `usbboot` (static C runtime) | **glibc** | 2.39-0ubuntu8.8 | LGPL-2.1-or-later | `glibc_2.39*` |
| `usbboot` | its own C source | - | GPL-2.0-or-later | `../src/usbboot/usbboot.c` |
| `disc_spl_lpddr3.bin` | stage-1 SPL (u-boot-xburst) | see `../SPL_SOURCE.md` | GPL-2.0 | `../spl-src/` |

Also statically linked into the Linux `mksquashfs`/`unsquashfs`: **zlib 1.3.2** and **liblzma (xz 5.8.4)** -
both permissive (zlib / 0BSD), needing attribution only (see `../NOTICE.md`); their source archives are
in `linux-squashfs-musl/` as well.

## Rebuilding the binaries from this source

The Linux release build recipes are in `../build/`:
- `build/build-squashfs-static.sh` - static `mksquashfs`/`unsquashfs` (gzip+lzo+xz), with an LZO
  round-trip self-test, built against musl (see "Linux squashfs tools" below).
- `build/build-usbboot-pinned.sh` - offline, pinned Linux x86-64 `usbboot`, using
  upstream libusb 1.0.27 `.orig` with `--disable-udev --enable-static --disable-shared`.
  The Debian patch is not applied. The complete Debian source package is retained.

The pinned usbboot recipe uses only the local archives here and asserts all four glibc packages
(`libc6`, `libc6-dev`, `libc-bin`, `libc-dev-bin`) are exactly `2.39-0ubuntu8.8`.
The older `build-usbboot-static.sh` uses the host runtime and is not the release
recipe. Source modifications and private relinking are described below.

## Linux squashfs tools (musl)

The Linux `mksquashfs` and `unsquashfs` are statically linked against **musl 1.2.5**, not glibc. A
statically linked glibc still loads the host's name-service modules for user and group lookups, and on
some distributions (for example Fedora 44) that crashed `mksquashfs` while it printed its summary.

`build/build-squashfs-static.sh NEW_DIR` downloads the five upstream archives listed in
`linux-squashfs-musl/SHA256SUMS`, checks each SHA256 before extracting, builds musl (its `musl-gcc`
wrapper), LZO, zlib and liblzma as private static libraries, links `mksquashfs`/`unsquashfs` against
them, checks that both are statically linked, and runs the installer's LZO pack/extract probe. It writes
only to `NEW_DIR` (never to `vendor/`). No patches are applied. Host GCC and binutils are build
prerequisites; the build is not claimed to be byte reproducible.

`linux-squashfs-musl/` holds exactly those archives (`SHA256SUMS`), plus the upstream signatures
for musl (`.asc`), zlib (`.asc`) and xz (`.sig`). `lzo-2.10.tar.gz` is byte-identical to
`lzo2_2.10.orig.tar.gz` above.

If the bundled pair ever fails the installer's capability check on Linux, the installer retries once
with a complete system `mksquashfs`/`unsquashfs` pair from `PATH` (never a mix of bundled and system
tools), uses that pair for the rest of the run only if it passes the same check, and logs that it did
so. Otherwise the original error stands.

## Pinned Linux x86-64 usbboot build

Run from the installer root as an ordinary user with rootless Podman.
Preparation needs network access, curl, gpgv, the host ubuntu-keyring package
and the host CA bundle (/etc/ssl/certs/ca-certificates.crt):

```sh
sh build/prepare-usbboot-env.sh --snapshot "$SNAPSHOT" \
  --base-url "$UBUNTU_BASE_RELEASE_URL" --rootfs "$ROOTFS_FILENAME" --out "$ENV_DIR"
sh build/build-usbboot-pinned.sh --image-id "$ENV_DIR/build-image.id" \
  --out "$CANDIDATE_DIR" --epoch "$SOURCE_EPOCH"
```

Every output directory must be new. Select a fixed Ubuntu Base 24.04 amd64
release tarball and a snapshot timestamp (`YYYYMMDDTHHMMSSZ`) containing the
exact glibc revision in noble-updates/noble-security. Preparation authenticates
SHA256SUMS.gpg with the pinned Ubuntu CD signing key
`843938DF228D22F7B3742BC0D94AA3F0EFE21092`, verifies the rootfs hash, then uses
only that signed apt snapshot. Signed snapshot indexes may be expired; their
expiry check is disabled, their signature checks remain on. The snapshot host
redirects apt's HTTP requests to HTTPS and the bare rootfs has no CA store, so the
host CA bundle is mounted read-only for transport only; apt signature and package
hash checks stay mandatory. The rootfs download uses HTTPS. Choose a rootfs
release older than the snapshot, so apt only moves packages forward to the
snapshot versions (1.2.1: ubuntu-base-24.04.4 with snapshot 20260815T000000Z).

Preparation retains downloaded `.deb` hashes, dpkg records, apt indexes/logs,
the image ID and an OCI archive/hash. It pins inputs and package selection;
byte-identical image archives are not promised. Keep the frozen image for reuse.
The build runs with `--pull=never --network=none`, `LC_ALL=C`, `TZ=UTC`, a fixed
recorded `SOURCE_DATE_EPOCH`, and file prefix maps. It produces a stripped static
candidate, manifest, input hashes, compiler output, package records, link map,
logs and a `--help` smoke result. See `../build/USBBOOT_MANIFEST.md` for the format.
Build twice with the same inputs/image/epoch in different fresh directories and
compare candidate hashes before qualification. The recipe never writes vendor/.

The macOS arm64 tools use separate Homebrew inputs: squashfs-tools 4.7.5,
libusb 1.0.30, lzo 2.10, lz4 1.10.0, xz 5.8.4 and zstd 1.5.7.
Their archives, squashfs patch, formula snapshots and relink instructions
are in [`macos-arm64/README.md`](macos-arm64/README.md).

## The LGPL-2.1 relink path for libusb and glibc (usbboot)

The complete application source provides the LGPL section 6 rebuild/relink path.
For a private modified libusb, unpack the `.orig`, modify it, configure with
`--disable-udev --enable-static --disable-shared`, then `make`. From that build
directory, using your chosen runtime/compiler:

```sh
gcc-13 -O2 -std=c99 -Ilibusb -static -no-pie -o usbboot \
  "$INSTALLER_SOURCE/src/usbboot/usbboot.c" libusb/.libs/libusb-1.0.a -lpthread
```

For modified glibc, extract `glibc_2.39-0ubuntu8.8.dsc` with `dpkg-source -x`,
modify that tree and build its packages in a separate Noble build environment
with glibc's build dependencies (`dpkg-buildpackage -b -uc -us`). Install your
matching rebuilt libc6, libc6-dev, libc-bin and libc-dev-bin packages in a
private build image. Rebuild libusb there, then relink the application with the
command above so its startup objects, headers and static libc come from your
modified runtime. Retain glibc's upstream INSTALL and Debian packaging rules.
Alternatively build glibc out of tree following INSTALL and use a compiler
sysroot containing that installation plus matching startup objects and headers.
For modified libraries, copy the release recipe and remove/update its release
hash and version assertions in your private copy. Release manifest enforcement
does not restrict private modifications; such builds are not release candidates.

## Windows release sources

The shipped-file and static-component inventory is in
[`../NOTICE.md`](../NOTICE.md#windows-x86_64-bundle); attribution status and
verbatim text provenance are in `../licenses/windows/README.md`.
`../build/build-windows-tools.sh` uses the squashfs 4.6.1, LZO 2.10 and libusb
1.0.27 archives here, `msys2-squashfs.patch`, and `../src/usbboot/usbboot.c`.
LZO and libusb are rebuilt statically; zlib and libgcc use MSYS static libraries.
To relink with modified libusb or LZO, substitute its archive and update the
recipe SHA256. Package versions and input/output hashes are in MANIFEST.txt.

Only `msys-2.0.dll` is copied. Its matching source is a separate release asset,
not a git archive: `msys2-runtime-3.6.10-6.src.tar.zst` (128879527 bytes), SHA256
`1d06b1482199ca8adaea770c4b596ec3adb2749345ef81a5d111cb792f0f1643`, plus `.sig`.
From the installer root run `bash build/fetch-windows-sources.sh OUTPUT_DIR`.
Default: a new `/tmp/diskos-windows-sources.*` directory outside the checkout.
Use `--verify OUTPUT_DIR` to recheck an existing archive offline. Signature
presence is checked; authenticate it separately with trusted MSYS2 keys.
Attach both assets beside the release binaries. The build refuses a runtime
package other than 3.6.10-6.

Windows static zlib is attribution-only as in the inventory above
(`../licenses/windows/zlib-LICENSE.txt`). MSYS/UCRT64 GCC runtime, CRT and
winpthreads sources are not vendored; their notices and the reasoning are in
`../NOTICE.md` and `../licenses/windows/README.md`. Linux glibc sources do not
cover Windows runtime components.
