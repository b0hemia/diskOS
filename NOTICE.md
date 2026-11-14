# diskOS installer - third-party components & licenses

diskOS is free software under the GNU GPL v3.0 or later (see `LICENSE`); third-party components keep
their own licences, as listed below.

## Licence history: releases up to 1.2.2 (MIT)

diskOS releases up to and including 1.2.2 were published with the installer and tools under the MIT
licence (the text is kept in `licenses/MIT-diskos-pre-1.2.3.txt`). From 1.2.3 everything the project
wrote is GPL-3.0-or-later. Portions contributed by outside authors under MIT remain available under
the MIT notice in that text:

- commit `44ad3ce` (Chthome Luii; Arch, CachyOS and Fedora support): `README.md`,
  `build/build-squashfs-static.sh`, `build/build-usbboot-static.sh`, `diskos_installer/__main__.py`,
  `diskos_installer/bundle.py`, `diskos_installer/platform_probe.py`, `install.sh` and
  `udev/70-diskos-maskrom.rules`;
- commit `ede6698` (Pierre Nel): `diskos_installer/imagebuild.py`.

## Source for the diskOS code

The corresponding source for the diskOS code in a release (installer, tools, UI) is this repository at
that release's tag. Source for the bundled third-party GPL/LGPL programs is described below.

The diskOS installer ships the third-party programs and libraries below. Each is invoked as a
separate program or loaded as a library; this document provides the required attribution and points
to the corresponding source that ships alongside the binaries. The full verbatim license texts are in
[`licenses/`](licenses/); this file is the component-to-license index.

## Bundled native programs (called as subprocesses - mere aggregation)

| Component | Purpose | License | Source |
|---|---|---|---|
| **usbboot** | Ingenic X2000 mask-ROM USB loader (the flasher front-end) | **GPL-2.0-or-later** - (c) 2021 Aidan MacDonald, (c) 2015 Amaury Pouly | included: `src/usbboot/` (built from this) |
| **mksquashfs / unsquashfs** | build/extract the rootfs image | **GPL-2.0-or-later** | Linux: 4.6.1 in `corresponding-source/linux-squashfs-musl/`; macOS arm64: 4.7.5 in `corresponding-source/macos-arm64/` |
| **dropbearmulti** (Dropbear SSH **2022.83**) | embedded in the diskOS image; started by the opt-in Debug Mode for SSH-over-WiFi | **MIT-style** ((c) 2002-2020 Matt Johnston; components under MIT/BSD/public-domain) - text in `licenses/LICENSE-dropbear.txt` | https://matt.ucc.asn.au/dropbear/dropbear.html (v2022.83; **predates the CVE-2023-48795 "Terrapin" Strict-KEX mitigation**. Use Debug Mode only for short sessions on a trusted network and turn it off when finished.) |

`usbboot` and squashfs-tools are **GPL-2.0-or-later**, so anyone redistributing them must make their
corresponding source available and include the GPL-2.0 license text. (`dropbearmulti` is MIT-style -
no source-availability obligation, only its license text as noted above.) **For Linux and macOS, the complete
corresponding source ships in-tree and in every release tarball**, so it travels from the same place
as the binaries (GPL-2.0 section 3(a), plus the "same place" provision in section 3's final paragraph):
`usbboot`'s own source is `src/usbboot/usbboot.c`;
**Linux squashfs-tools 4.6.1** source is `corresponding-source/linux-squashfs-musl/squashfs-tools-4.6.1.tar.gz`. The macOS arm64 4.7.5 source, Homebrew patch, dependency sources and formulas are in `corresponding-source/macos-arm64/`. The Linux tools are rebuilt by
`build/build-usbboot-pinned.sh` / `build/build-squashfs-static.sh`.

**Static linking (Linux):** the prebuilt Linux `usbboot`, `mksquashfs`, and `unsquashfs` are shipped
**fully statically linked**. They therefore statically incorporate:
- **upstream libusb-1.0.27 (LGPL-2.1-or-later)** - in `usbboot`, from the shipped
  `.orig` archive; the Debian patch is not applied
- the **GNU C Library (glibc, LGPL-2.1-or-later)** - `2.39-0ubuntu8.8`, in `usbboot`
- **LZO 2.10 (GPL-2.0-or-later)**, **zlib 1.3.2**, **liblzma (xz 5.8.4)** (permissive) and the **musl 1.2.5**
  C library (MIT, `licenses/MIT-musl.txt`) - in `mksquashfs`/`unsquashfs`, all built from the archives in
  `corresponding-source/linux-squashfs-musl/`

The Linux x86-64 usbboot release recipe builds offline in a frozen rootless
Podman image (`--network=none --pull=never`). It asserts exactly
`2.39-0ubuntu8.8` for libc6, libc6-dev, libc-bin and libc-dev-bin. Its manifest
ties source, archives, recipe, image ID, epoch, compiler and flags to the
executable hash. The link map inventories the runtime archives actually used.
If libgcc/libgcc_eh or GCC startup code is incorporated, its terms are
GPL-3.0-or-later with GCC Runtime Library Exception 3.1. Complete texts:
`licenses/GPL-3.0.txt` and `licenses/windows/ucrt64-libgcc-COPYING.RUNTIME.txt`
(the exception text is platform independent). GCC runtime copyright is held
by the Free Software Foundation; the exact image's GCC package copyright
notice is retained as `gcc-runtime-copyright.txt` with the build evidence.
Confirm the incorporated members and notices against the candidate link map.

Because liblzo2 is GPL, the resulting `mksquashfs`/`unsquashfs` binaries are effectively GPL-2.0. The
corresponding source for the statically-linked **GPL/LGPL** libraries is provided so their section 3/section 6
obligations are met:
- **LZO** (`corresponding-source/linux-squashfs-musl/lzo-2.10.tar.gz`, also `corresponding-source/lzo2_2.10*`)
  and **libusb-1.0** (`corresponding-source/libusb-1.0_1.0.27*`) ship in-tree.
- **glibc** (statically linked into `usbboot`, so it travels inside the binary): the **complete
  corresponding source** is vendored in `corresponding-source/glibc_2.39*` (the exact version the shipped
  binary was built against). Because it is statically linked it does not qualify for the "system library" exception, so
  its source ships alongside the binaries like the others.

**Relink path (satisfies LGPL-2.1 section 6 for libusb *and* glibc):** we ship the **complete source** of
`usbboot` (`src/usbboot/usbboot.c`) and squashfs-tools (`corresponding-source/linux-squashfs-musl/`)
plus the exact Linux build recipes (`build/build-usbboot-pinned.sh`, `build/build-squashfs-static.sh`). A
user can therefore rebuild and relink these tools against a **modified** libusb, liblzo2, **or glibc**
of their choosing - the freedom section 6 exists to protect. See
[`corresponding-source/README.md`](corresponding-source/README.md).

## Windows x86_64 bundle

The native tools and build record below are accompanied by the two device blobs
in `vendor/windows-x86_64/`:

| Shipped file | Licence | Source / build input |
|---|---|---|
| `mksquashfs.exe` | GPL-2.0-or-later; static components below also apply | `corresponding-source/squashfs-tools_4.6.1.orig.tar.gz`, `corresponding-source/msys2-squashfs.patch` |
| `unsquashfs.exe` | GPL-2.0-or-later; static components below also apply | Same squashfs source and patch |
| `usbboot.exe` | GPL-2.0-or-later; static components below also apply | `src/usbboot/usbboot.c` |
| `msys-2.0.dll` | LGPL-3.0-or-later with the Cygwin linking exception (`winsup/CYGWIN_LICENSE` at tag cygwin-3.6.10; package metadata says GPL-3.0-or-later); some files BSD-like per their own notices | Release asset `msys2-runtime-3.6.10-6.src.tar.zst` and its `.sig`; `licenses/windows/msys2-runtime-CYGWIN_LICENSE.txt`, `-COPYING.LIB.txt` (LGPL-3.0), `-COPYING.txt` (GPL-3.0) |
| `bind_winusb.exe` | GPL-3.0-or-later project code; UCRT64 static CRT and libgcc notices below apply | `src/windows-driver/`; `build/build-windows-driver.sh` |
| `MANIFEST.txt` | GPL-3.0-or-later project build record | `build/build-windows-tools.sh`; versions and input/output SHA256 |

| Statically included component | In files | Licence | Source / notice |
|---|---|---|---|
| LZO 2.10 | Both squashfs EXEs | GPL-2.0-or-later | `corresponding-source/lzo2_2.10.orig.tar.gz`; `licenses/windows/lzo-COPYING.txt` |
| zlib 1.3.2-1 | Both squashfs EXEs | zlib | MSYS `/usr/lib/libz.a`; source not vendored (attribution-only); `licenses/windows/zlib-LICENSE.txt` |
| MSYS libgcc, gcc 15.3.0-1 | Both squashfs EXEs | GPL-3.0-or-later WITH GCC-exception-3.1 | MSYS GCC static libraries; source not vendored; `licenses/windows/msys-libgcc-RUNTIME.LIBRARY.EXCEPTION.txt`, `licenses/GPL-3.0.txt` |
| libusb 1.0.27 | usbboot | LGPL-2.1-or-later | `corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2`; `licenses/windows/libusb-COPYING.txt` |
| mingw-w64 CRT 14.0.0.r426.g4564ee4b5-1 | usbboot and bind_winusb | ZPL-2.1 and the notices in its licence files | UCRT64 static CRT; source not vendored; `licenses/windows/mingw-w64-crt-COPYING*.txt` |
| winpthreads 14.0.0.r426.g4564ee4b5-1 | usbboot | MIT AND BSD-3-Clause | UCRT64 static library; source not vendored; `licenses/windows/winpthreads-COPYING.txt` |
| UCRT64 libgcc, GCC 16.2.0-4 | usbboot and bind_winusb | GPL-3.0-or-later WITH GCC-exception-3.1 | UCRT64 GCC static library; source not vendored; `licenses/windows/ucrt64-libgcc-COPYING.RUNTIME.txt`, `licenses/GPL-3.0.txt` |

Build with `build/build-windows-tools.sh`: vendored LZO is built with MSYS GCC,
zlib uses MSYS `libz.a`, and libgcc uses `-static-libgcc`. Only `msys-2.0.dll`
is copied; its package must be exactly 3.6.10-6. Its source release asset is
128879527 bytes, SHA256
`1d06b1482199ca8adaea770c4b596ec3adb2749345ef81a5d111cb792f0f1643`.
Fetch it outside this checkout with `build/fetch-windows-sources.sh OUTPUT_DIR`,
and attach the archive and `.sig` beside the release binaries. The helper checks
the archive hash and requires a valid signature from a fully trusted MSYS2 key
in `MSYS2_GNUPGHOME`. See `build/windows-qualification.md` for the offline
verification and runtime rebuild procedure. A downloaded `.sig` alone is not authentication.

The runtime also links newlib libc/libm and contains per-file BSD and other
permissive notices. The complete upstream inventory is in
[`msys2-runtime-COPYING.NEWLIB.txt`](licenses/windows/msys2-runtime-COPYING.NEWLIB.txt);
complete per-file notices, including runtime headers and MSYS path-conversion
CC0 terms, are in
[`msys2-runtime-BSD-NOTICES.txt`](licenses/windows/msys2-runtime-BSD-NOTICES.txt).
`licenses/windows/README.md` records their pinned-source provenance.

No UCRT API-set DLL is bundled. To rebuild/relink usbboot with modified libusb,
substitute its archive and update the recipe hash, then rerun it. The same route
permits rebuilding squashfs with modified LZO. `licenses/windows/README.md`
records where each licence text was taken from. Sources for the statically
included GCC runtime, mingw-w64 CRT and winpthreads are not vendored: their
licences (GCC Runtime Library Exception 3.1, ZPL-2.1, MIT and BSD-3-Clause) are relied on for
that; this is a reading of those licence texts, not legal advice. The licence
texts govern their components.

**Python runtime + libraries (Linux/macOS source releases).** The installer runs **from
source** with **your own Python**. The four Python dependencies (`pyusb`, `pycryptodome`, `certifi`, `platformdirs`) are fetched
from PyPI by *your* `pip` into a local `.venv` (created by `./install.sh`); CPython, `tkinter`, and
their transitive native libraries (OpenSSL, Tk/Tcl, the X11 stack, freetype, fontconfig, zlib, etc.)
all come from **your system's Python** - this project ships none of them, so their redistribution
notices are not our obligation. Their license texts (for reference) are still listed in
[`licenses/README.md`](licenses/README.md).

> If you instead build a self-contained PyInstaller onefile yourself (`build/build.sh`), *that*
> binary embeds CPython and its collected native libraries, which you redistribute - see
> [`licenses/THIRD_PARTY_BUNDLED.md`](licenses/THIRD_PARTY_BUNDLED.md) for the enumeration and
> obligations. The Linux/macOS source release distributes source + native flash tools.
> A packaged Windows release does redistribute its Python runtime and libraries;
> its packaging inventory must include their notices and licences as well.

## Bundled device blobs

| Component | Purpose | Notes |
|---|---|---|
| **my_write6_dram.bin** | the bad-block-aware DRAM NAND writer run on-device (checks the Disc's firmware before writing) | diskOS project code - source: `flash/my_write6.c`, built by `flash/build_nand.sh` |
| **disc_spl_lpddr3.bin** | Ingenic X2000 USB stage-1 SPL (DRAM init) | **GPL-2.0** - built from source: Ingenic-community/uboot-xburst `1060b516` + our LPDDR3 patches. Corresponding source in `spl-src/`; see `SPL_SOURCE.md`. The binary also includes a roughly 332-byte DDR parameter block captured from the factory configuration. Its vendor-origin provenance remains an unresolved compliance item, as disclosed in `SPL_SOURCE.md`. (No vendor/USBCloner binary is redistributed.) |

## Python dependencies (Linux/macOS source releases)

`./install.sh` installs these from PyPI into a local `.venv`; **CPython** comes from your own system
Python. This project does not ship any of them, so their license texts are listed here only for
reference. (**PyInstaller** is used *only* if you build the optional onefile yourself; it is not
involved in the source release.)

| Component | License |
|---|---|
| **pyusb** (USB device detection) | BSD-3-Clause; `licenses/BSD-3-Clause-pyusb.txt` matches the locked 1.3.1 wheel byte for byte |
| **pycryptodome** (AES-decrypt of FiiO OTA chunks) | BSD-2-Clause + public-domain (Unlicense), with CC0 SipHash in native arithmetic. Windows wheel text: `licenses/PyCryptodome-3.23.0-Windows-LICENSE.rst`, with runtime-module notices and the wheel's OCB patent statement in `licenses/PyCryptodome-3.23.0-Windows-NOTICES.txt`. Native-source comments, including BLAKE2, Keccak, LibTomCrypt and SipHash: `licenses/PyCryptodome-3.23.0-Native-NOTICES.txt`; complete bundled LibTomCrypt dedication: `licenses/PyCryptodome-3.23.0-libtom-LICENSE.txt`; SipHash dedication: `licenses/CC0-1.0-SipHash.txt`. The existing `licenses/pycryptodome-LICENSE.txt` has the wheel's wording with LF instead of CRLF |
| **certifi 2026.2.25** (CA bundle for verified HTTPS) | MPL-2.0; wheel bundle notice: `licenses/MPL-2.0-certifi.txt`; complete licence: `licenses/MPL-2.0.txt`. Unmodified package source is available from PyPI for certifi 2026.2.25. |
| **platformdirs 4.2.2** (platform directory discovery) | MIT; `licenses/MIT-platformdirs.txt` preserves the wheel LICENSE |
| **CPython** (system Python for source runs; 3.12.10 x64 in the Windows dependency inventory) | PSF-2.0 and historical Python terms; `licenses/PSF-Python.txt` plus the complete Windows distribution notice `licenses/CPython-3.12.10-Windows-LICENSE.txt` and exact-install incorporated-software section in `licenses/CPython-3.12.10-Incorporated-Software.txt` |
| **PyInstaller** runtime bootloader (Windows package and self-built onefile) | GPL-2.0 **with a bootloader exception** permitting distribution of the packaged app under any license |

## Frozen Windows Python runtime

The CPython 3.12.10 x64 dependency inventory lists these native libraries;
versions come from DLL metadata. The Windows spec includes their notices.
See `build/WINDOWS_FROZEN_DEPENDENCIES.md` for the component mapping.

| Component | Licence / notice |
|---|---|
| OpenSSL 3.0.16 (`libcrypto-3.dll`, used by `_hashlib.pyd`) | Apache-2.0; Copyright 1998-2025 The OpenSSL Authors. Full text: `licenses/Apache-2.0-OpenSSL.txt`. No `libssl` or `_ssl` binary in the inventory. |
| libffi (`libffi-8.dll`) | MIT-style libffi terms; Copyright (c) 1996-2022 Anthony Green, Red Hat, Inc and others. Complete exact-install notice in `licenses/CPython-3.12.10-Windows-LICENSE.txt`, lines 366-386. |
| zlib 1.3.1 (`zlib1.dll`) | zlib; Copyright (C) 1995-2022 Jean-loup Gailly & Mark Adler in DLL metadata. Complete upstream 1.3.1 text in `licenses/zlib-1.3.1-LICENSE-README.txt`, copied verbatim from the local 1.3.1 development package's README (1995-2024 copyright). `licenses/windows/zlib-LICENSE.txt` remains for MSYS static zlib 1.3.2. The Windows CPython notice contains no zlib licence. |
| Tcl/Tk 8.6.15 | Tcl/Tk permissive terms and incorporated third-party terms. CPython's Tcl base text (Windows notice lines 566-605) matches the supplied Tcl source after newline normalization. `licenses/Tk-8.6.15-license.terms.txt` is the exact install's Tk text, the copy of `_internal/_tk_data/license.terms` and equal to supplied Tk source after newline normalization. Tcl notices: `licenses/Tcl-8.6.15-regex-NOTICE.txt`, `licenses/Tcl-8.6.15-libtommath-LICENSE.txt`, `licenses/Tcl-8.6.15-Bison-NOTICE.txt`, `licenses/Tcl-8.6.15-zlib-LICENSE.txt`. Tk's X11 header grants, GIF/giftoppm credit, ttk and other source copyrights and runtime script headers are preserved in `licenses/Tk-8.6.15-Incorporated-NOTICES.txt`. New Windows builds omit unused `_tk_data/images/` and `mkpsenc.tcl`, including logo GIFs, Adobe EPS procsets and glyph data; their absence is required by the inventory. Startup-required `icons.tcl` stays with `licenses/CC-BY-SA-3.0-Tango.txt`. See the inventory for notice retention and Tk exclusions. |
| Microsoft VCRUNTIME140 and VCRUNTIME140_1, 14.42.34438.0 | These two DLLs are Microsoft Distributable Code redistributed as part of the CPython 3.12.10 runtime. CPython's "Additional Conditions for this Windows binary build" apply and are shipped verbatim in `licenses/CPython-3.12.10-Windows-LICENSE.txt`, lines 283-320. Microsoft's terms govern; see the [Visual Studio 2022 redistribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution), "Visual C++ Runtime Files". The Microsoft components remain under Microsoft's terms and are not covered by this project's GPL licence. Redistributors must satisfy Microsoft's conditions; the "licensed Visual Studio user" condition is a separate Microsoft-terms question. |
| Dynamic libusb 1.0.27 | LGPL-2.1-or-later; `licenses/windows/libusb-COPYING.txt` matches the 1.0.27 source archive's COPYING. Windows builds explicitly pin the official VS2022/MS64 DLL in `build/windows-libusb.json` and ship `corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2` with its upstream signature. The pinned DLL SHA256 is `d4e5db4fad8bef7201dc5a4a71ca997e6dccc9da25e8a988d4be63b13a02208d`. The source archive SHA256 `ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575` is byte-identical to upstream libusb-1.0.27.tar.bz2; its shipped signature uses signing fingerprint `9C7EA94939C69C4FBC3DBFA8AA0639079EFB61B9`. Preserve the pinned inputs and notices when packaging. See `build/WINDOWS_FROZEN_DEPENDENCIES.md`. |
| MSYS2 runtime 3.6.10 (vendor and `_internal` root copies) | Same complete Cygwin/LGPL-3.0, newlib and BSD/per-file notices and source sidecars as the Windows tool bundle above. |

The warning, error, information and question images embedded in Tk's
`_tk_data/icons.tcl` are credited by upstream to the
[Tango Desktop project](http://tango.freedesktop.org/Tango_Desktop_Project).
The script credits Copyright (c) 2009 Pat Thoyts; its complete opening notice
is retained verbatim in `licenses/Tk-8.6.15-Incorporated-NOTICES.txt`.
Its declared [CC BY-SA 3.0 licence](https://creativecommons.org/licenses/by-sa/3.0/)
is supplied in `licenses/CC-BY-SA-3.0-Tango.txt`. These icons remain under that
licence, separate from this project's licences. Upstream embeds the icons as
PNG data in the script. Collected icons.tcl SHA256
`1264940e62b9a37967925418e9d0dc0befd369e8c181b9bab3d1607e3cc14b85`
matches Tk 8.6.15 library/icons.tcl after LF-to-CRLF conversion. The script
and embedded icons are unmodified, its complete Tk attribution header is
retained. Preserve the notices, pinned libusb inputs and Tk exclusions when packaging.

`licenses/CPython-3.12.10-Windows-LICENSE.txt` is the complete, unmodified
702-line notice from the CPython 3.12.10 x64 installation. `licenses/PSF-Python.txt` is retained alongside it: the base
terms match the Windows notice's opening after newline normalization, but
contain no bundled-library notices or Microsoft redistribution terms.
The Windows notice includes separate Tcl-style and Tk-style texts before its
later Tcl/Tk pointer. The entire incorporated-software section from the exact
installation's Doc-license.html is also shipped, preserving upstream wording.
It includes libmpdec/Stefan Krah, dtoa/David M. Gay, SipHash/Marek Majkowski,
Mersenne Twister, cfuhash, sockets and asyncio, plus a conservative superset of
notices for modules absent from the native inventory. Conversion and component
mapping are documented in `build/WINDOWS_FROZEN_DEPENDENCIES.md` and
`licenses/README.md`.

## Bundled shared libraries (macOS build)

| Component | License |
|---|---|
| **libusb-1.0.0.dylib** (libusb 1.0.30) | LGPL-2.1-or-later; `licenses/LGPL-2.1-libusb.txt` |
| **liblzo2.2.dylib** (lzo 2.10) | GPL-2.0-or-later; `licenses/GPL-2.0.txt` |
| **liblz4.1.10.0.dylib** (lz4 1.10.0) | BSD-2-Clause; `licenses/BSD-2-Clause-lz4.txt` |
| **liblzma.5.dylib** (xz 5.8.4) | 0BSD for liblzma; XZ source includes GPL-2.0-or-later parts; `licenses/0BSD-xz.txt`, `licenses/XZ-COPYING.txt` |
| **libzstd.1.5.7.dylib** (zstd 1.5.7) | BSD-3-Clause or GPL-2.0-only for the library; `licenses/BSD-3-Clause-zstd.txt`, `licenses/GPL-2.0-zstd.txt` |


The exact macOS arm64 upstream archives, the squashfs Homebrew backport, and formula build
recipes are in `corresponding-source/macos-arm64/` (see its `README.md`). `vendor/setup-macos.sh`
builds `usbboot` against Homebrew libusb, copies Homebrew squashfs tools, and uses
`dylibbundler` to copy and relocate these five libraries. LGPL-2.1 section 6 users can
rebuild and relink `usbboot` against a modified libusb using the included source and recipe.

## Website

The website in `site/` is deployed separately from the installer. Its HTML, CSS,
JavaScript, and diskOS-owned screenshot images are GPL-3.0-or-later under `LICENSE`,
Copyright (c) 2026 diskOS contributors. This grant covers only rights held by diskOS
contributors; third-party artwork and trademarks remain with their respective owners.

The ten website fonts retain their upstream licences: nine are OFL-1.1 and
Special Elite is Apache-2.0. Their copyright notices and licence texts are retained
in `site/fonts/`; see [the font index](licenses/README.md#website-fonts).

## diskOS's own code

- **The Python installer, build scripts, and docs** in this repo are licensed **GPL-3.0-or-later** -
  `Copyright (c) 2026 diskOS contributors`. See `LICENSE`.
  The installer GUI is part of this tooling. Its display assets in
  `diskos_installer/assets/` use Archivo for titles in `display*.png` and the
  wordmark in `wordmark*.png`, and Michroma for section kickers in `display*.png`.
  Both fonts are SIL OFL 1.1; their full notices are
  `diskos_installer/assets/LICENSE-archivo-OFL.txt` and
  `diskos_installer/assets/LICENSE-michroma-OFL.txt`. Lettering is pre-rendered;
  dynamic text uses the system font and no fonts are installed on the computer.
  The `disc*.png` illustration contains no font lettering.
- **`payload/diskos-launch`** is GPL-3.0-or-later project tooling; source: `src/launcher/diskos_launch.c`,
  built by `build/build-launcher.sh`.
- **`payload/diskos-bootprobe`** is GPL-3.0-or-later UI helper code; source:
  `ui/tools/diskos_bootprobe.c`, built with the UI tools.
- **The diskOS UI source** (`ui/`, built to `payload/mq_ui`, static-musl MIPS) is licensed
  **GPL-3.0-or-later** (`ui/COPYING`, `licenses/GPL-3.0.txt`; (c) diskOS contributors) - deliberately
  the same licence as the installer and tools since 1.2.3. It bundles LVGL (MIT), SQLite (public
  domain), jsmn (MIT), md5 (public domain), and Montserrat / Source Han Sans / Font Awesome /
  IBM Plex Mono (as `diskos_mono`) / Doto (as `diskos_dot`) and the theme fonts Inter, DSEG7 Classic, JetBrains Mono,
  Space Grotesk, IBM Plex Sans, Nunito, Courier Prime and Rubik Glitch (as `diskos_<theme>_*`),
  plus Archivo (as `diskos_brand_disk_32` / `diskos_brand_os_32` for the boot-splash wordmark)
  (all OFL-1.1);
  see `ui/README.md` and `ui/licenses/`.

  **`diskos-artdec`** (`payload/diskos-artdec`, source `ui/tools/diskos_artdec.c`) is a separate small helper
  binary shipped in the diskOS image at `/opt/diskos/bin/diskos-artdec`. It extracts and decodes album
  artwork for the UI. It is diskOS's own code (**GPL-3.0-or-later**) and statically embeds the unmodified
  `stb_image.h` v2.30 by Sean Barrett (dual licence, MIT or public domain, your choice; notice and licence
  text at the end of `ui/tools/stb_image.h`, pinned in `ui/tools/stb_image.VERSION`) and the static musl C
  runtime (MIT). stb_image is NOT part of `mq_ui`.

  **`diskos-audiodec`** (source `ui/tools/diskos_audiodec.c`, new in 1.2.2) is a small helper that decodes audio
  for Cast and AirPlay sending. It is diskOS's own code (SPDX `GPL-3.0-or-later` in the helper sources since 1.2.3; releases up to 1.2.2 shipped them under MIT, see
  `ui/tools/AUDIO_STOCK_LICENSE`) and is embedded inside the `mq_ui` binary, which is GPL-3.0-or-later; `mq_ui`
  writes it out at run time. It links **dynamically** against the FFmpeg libraries (`libavformat`, `libavcodec`,
  `libavutil`) that are already installed on the Disc in FiiO's firmware. Only FFmpeg's public headers (4.2.2,
  checked by SHA-256 in `ui/third_party/build-stock-audiodec.sh`) are used to compile it.
  FFmpeg's public headers are LGPL-2.1-or-later; the constants, layouts and small
  macros used here fall within LGPL-2.1 section 5. The licence text is supplied in
  `licenses/LGPL-2.1.txt`. No FFmpeg implementation source files are compiled into
  the helper, and no FiiO shared libraries are redistributed.

  The built `mq_ui` binary statically incorporates these third-party components, whose licenses
  apply to the shipped binary (and whose source ships in `ui/` / `ui/lvgl/`):

  | Embedded in `mq_ui` | License | Text / source |
  |---|---|---|
  | **musl libc** (static C runtime) | MIT | `licenses/MIT-musl.txt`; source: https://musl.libc.org/ |
  | **FreeType 2.13.3** (TrueType renderer) | FreeType License (FTL) | `ui/third_party/freetype-2.13.3/docs/FTL.TXT` |
  | **mbedTLS 3.6.7** (TLS and crypto for Cast and AirPlay; new in 1.2.2) | Apache-2.0 (upstream is Apache-2.0 OR GPL-2.0-or-later; used under Apache-2.0) | `licenses/Apache-2.0-mbedTLS.txt`; source: `ui/third_party/mbedtls-3.6.7/` |
  | **Monocypher 4.0.2** (Ed25519 / X25519 / ChaCha20-Poly1305 for AirPlay pairing; new in 1.2.2) | BSD-2-Clause OR CC0-1.0 | `licenses/BSD-2-Clause-Monocypher.txt` (includes the CC0 text); source: `ui/third_party/monocypher-4.0.2/` |
  | **libplist 2.6.0** (Apple property lists, AirPlay; new in 1.2.2) | LGPL-2.1-or-later (see below); `jsmn.c` MIT, `time64.c` MIT | `licenses/LGPL-2.1-libplist.txt`, `licenses/MIT-libplist-jsmn.txt` (Copyright (c) 2010 Serge A. Zaitsev), `licenses/MIT-libplist-time64.txt`; source: `ui/third_party/libplist-2.6.0/` |
  | **SpeexDSP 1.2.1** (`resample.c` only, sample-rate conversion; new in 1.2.2) | BSD-3-Clause | `licenses/BSD-3-Clause-SpeexDSP.txt` and `licenses/BSD-3-Clause-SpeexDSP-resampler.txt` (Jean-Marc Valin and Thorvald Natvig); source: `ui/third_party/speexdsp-1.2.1/` |
  | **dr_libs** (`dr_flac.h`, `dr_mp3.h`, `dr_wav.h`, David Reid; audio decoding; new in 1.2.2) | Public domain (Unlicense) OR MIT-0 | `licenses/Unlicense-or-MIT-0-dr_libs.txt`; source: `ui/third_party/dr_libs-dfe8377631000664666519fdb83da193fd8037f4/` |
  | **nanopb 0.4.9.1** (Protocol Buffers runtime for Cast; new in 1.2.2) | zlib | `licenses/Zlib-nanopb.txt`; source: `ui/third_party/nanopb-0.4.9.1/` |
  | **Cast channel schema** (`cast_channel.proto` from Google's openscreen project, compiled with nanopb into `cast_channel.pb.c`; new in 1.2.2) | BSD-3-Clause (Copyright 2019 The Chromium Authors for the schema; Copyright 2018 The Chromium Authors in the upstream licence) | `licenses/BSD-3-Clause-openscreen.txt`; source: `ui/third_party/cast/` |
  | **LVGL** (UI toolkit) | MIT | `licenses/MIT-LVGL.txt` |
  | **LodePNG 20230410** (Cast artwork PNG encoder; Copyright (c) 2005-2023 Lode Vandevenne) | zlib | `licenses/Zlib-LodePNG.txt`; source: `ui/lvgl/src/libs/lodepng/` |
  | **stb_image_write.h v1.16** (AirPlay artwork JPEG encoder; Copyright (c) 2017 Sean Barrett; new in 1.2.2) | MIT OR Unlicense | `licenses/MIT-or-Unlicense-stb_image_write.txt`; source: `ui/third_party/stb-1ee679ca/` |
  | **gifdec** (GIF decoder, via LVGL) | Public domain upstream; LVGL changes MIT | `licenses/MIT-LVGL.txt`; upstream dedication: https://github.com/lecram/gifdec |
  | **JSMN** (JSON parser) | MIT | `licenses/MIT-JSMN.txt` |
  | **SQLite** (amalgamation) | Public domain | https://sqlite.org/copyright.html |
  | **MD5** (Peslyak/Solar Designer) | Public domain | `ui/md5.c` |
  | **QR Code generator** (Nayuki `qrcodegen`) | MIT | `licenses/MIT-qrcodegen.txt` |
  | **TJpgDec** (tiny JPEG decoder, via LVGL) | BSD-style (ChaN) | `licenses/LICENSE-TJpgDec.txt` |
  | **Montserrat** font (via LVGL built-in fonts) | SIL OFL 1.1 | `licenses/OFL-1.1-Montserrat.txt` |
  | **Source Han Sans SC** (CJK fallback font, subset) | SIL OFL 1.1 | `licenses/OFL-1.1-SourceHanSans.txt` |
  | **IBM Plex Mono** (converted as `diskos_mono`; Reserved Font Name "Plex") | SIL OFL 1.1 | `licenses/OFL-1.1-IBMPlexMono.txt` |
  | **Doto** (converted as `diskos_dot`) | SIL OFL 1.1 | `licenses/OFL-1.1-Doto.txt` |
  | **Inter** (converted as `diskos_hifi_ui`) | SIL OFL 1.1 | `licenses/OFL-1.1-Inter.txt` |
  | **Archivo** (converted as `diskos_brand_disk_32` / `diskos_brand_os_32`, boot-splash wordmark) | SIL OFL 1.1 | `licenses/OFL-1.1-Archivo.txt` |
  | **DSEG7 Classic** (converted as `diskos_hifi_seg`; Reserved Font Name "DSEG") | SIL OFL 1.1 | `licenses/OFL-1.1-DSEG.txt` |
  | **JetBrains Mono** (converted as `diskos_term`) | SIL OFL 1.1 | `licenses/OFL-1.1-JetBrainsMono.txt` |
  | **Space Grotesk** (converted as `diskos_bauh`) | SIL OFL 1.1 | `licenses/OFL-1.1-SpaceGrotesk.txt` |
  | **IBM Plex Sans** (converted as `diskos_blue_sans`; Reserved Font Name "Plex") | SIL OFL 1.1 | `licenses/OFL-1.1-IBMPlexSans.txt` |
  | **Nunito** (converted as `diskos_stone`) | SIL OFL 1.1 | `licenses/OFL-1.1-Nunito.txt` |
  | **Courier Prime** (converted as `diskos_zine`) | SIL OFL 1.1 | `licenses/OFL-1.1-CourierPrime.txt` |
  | **Rubik Glitch** (converted as `diskos_zine_glitch`) | SIL OFL 1.1 | `licenses/OFL-1.1-RubikGlitch.txt` |
  | **Noto Sans** and, new in 1.2.2, **Noto Sans Hebrew** (converted as `font_intl_14` to `font_intl_20`; Latin-extended, Greek, Cyrillic and Hebrew glyphs) | SIL OFL 1.1 | `licenses/OFL-1.1-NotoSans.txt` |
  | **Font Awesome Free** glyphs (via LVGL `LV_SYMBOL_*`) | OFL 1.1 (fonts) + CC-BY 4.0 (icons) | `licenses/OFL-1.1-FontAwesome.txt`, `licenses/CC-BY-4.0.txt` |

**libplist (LGPL-2.1-or-later) is statically linked into `mq_ui`.** The `mq_ui` binary as a whole is distributed
under GPL-3.0-or-later. libplist and its use remain covered by LGPL-2.1-or-later;
the licence text is `licenses/LGPL-2.1-libplist.txt`. We use LGPL-2.1 section 6(a):
the corresponding libplist source, including libcnary, and the complete UI source,
configurations and build scripts are supplied so recipients can modify libplist
and rebuild and relink `mq_ui`. Modification for personal use and reverse
engineering to debug such modifications are permitted. The unmodified library
source is in `ui/third_party/libplist-2.6.0/`; its recipe is
`ui/third_party/build-libplist.sh`.
Upstream pins, URLs and the list of pruned files are in `ui/third_party/VENDOR.json`.

The UI is based in part on the work of the FreeType Team. Portions of this
software are copyright (C) 2024 The FreeType Project (https://freetype.org/).
All rights reserved. FreeType is used under the FTL, compatible with GPL-3.0;
the full text is in `licenses/FTL-FreeType.txt` (a verbatim copy of
`ui/third_party/freetype-2.13.3/docs/FTL.TXT`). FreeType is new in diskOS 1.2.1.
The vendored FreeType files are unmodified. Only the include tree and the base,
truetype, sfnt and smooth source modules are retained, along with the upstream
README and licence files; unused modules, build systems, tests and binary
artifacts are omitted. The separate `ftconfig` files select the minimal modules.
The retained `include/dlg/dlg.h` and `include/dlg/output.h` headers are
Copyright (c) 2019 nyorain (https://github.com/nyorain/dlg), distributed under
the Boost Software License, Version 1.0; the complete text is in
`licenses/BSL-1.0-dlg.txt`. These headers have their own licence and are not
used by the current build, which leaves `FT_DEBUG_LOGGING` disabled.

## JPEG artwork encoder

stb_image_write.h v1.16, Copyright (c) 2017 Sean Barrett, is used under
the MIT licence (alternatively available under the Unlicense).
Pinned source: nothings/stb@1ee679ca2ef753a528db5ba6801e1067b40481b8.
The unchanged header is retained in `ui/third_party/stb-1ee679ca`; the
complete terms are in `licenses/MIT-or-Unlicense-stb_image_write.txt`.

Source Han Sans SC remains covered by the shipped OFL-1.1 notice, including the
regenerated CJK supplement. The runtime TTC is read from the stock firmware and
is not distributed here.

## NOT distributed by this installer

- **FiiO's stock rootfs** - you supply your own official firmware; the installer only reads it locally
  and never redistributes it.
- **ffmpeg libraries** - diskOS does not redistribute FiiO's FFmpeg shared libraries. Album artwork is decoded by the bundled
  `diskos-artdec` helper (see above; `ui/art.c`). The `diskos-audiodec` helper (see above) links dynamically
  against the FFmpeg libraries that FiiO's stock firmware already has on the Disc, and is built using only
  FFmpeg's public headers; the installer does **not** ship those libraries.

---

### License and source notes
- Full license texts shipped in [`licenses/`](licenses/) (GPL-2.0, LGPL-2.1, BSD, PSF, LVGL-MIT,
  dropbear, Font Awesome OFL/CC-BY) - see [`licenses/README.md`](licenses/README.md).
- diskOS is free software under the **GNU GPL v3.0 or later** (`LICENSE`): the installer, the tools
  and the UI in `ui/` (`ui/COPYING`, `licenses/GPL-3.0.txt`). Third-party components keep their own
  licences, as listed in this file. The UI ships with its embedded
  LVGL/SQLite/jsmn/Font Awesome/Montserrat/Source Han Sans/IBM Plex Mono/Doto/theme-font notices shipped.
- Upstream versions and corresponding source are shipped in-tree for the GPL/LGPL native
  binaries: Linux squashfs-tools 4.6.1 and libusb 1.0.27; macOS arm64 squashfs-tools 4.7.5 and libusb 1.0.30; liblzo2 2.10, the SPL, and usbboot. Source is in [`corresponding-source/`](corresponding-source/) and [`spl-src/`](spl-src/).
  The SPL includes the unresolved DDR parameter block described in [`SPL_SOURCE.md`](SPL_SOURCE.md).

## macOS frozen app

The frozen macOS app includes framework CPython and Tcl/Tk, PyUSB, PyCryptodome,
certifi, platformdirs, and native flash tools with private libraries.
The selected runtime's complete notices and component/version mapping ship in
`licenses/macos/`; native-tool sources, patches and formulas ship under
`corresponding-source/macos-arm64/`. `src/usbboot/` and the build recipes allow
rebuilding and relinking the native tools against modified libusb. Keep those
files with the app when redistributing. See `build/MACOS_FROZEN_DEPENDENCIES.md`
for the required matching-runtime notice/source additions. The freezer refuses
a package missing these files. Ad-hoc signing adds no redistribution restriction.

HarfBuzz 14.6.0 is bundled under the Old MIT licence; see
`licenses/HarfBuzz-COPYING` and the retained COPYING files in its subtrees.
SheenBidi 3.0.0 is bundled under Apache-2.0; see
`licenses/SheenBidi-Apache-2.0.txt`. The unmodified canary fonts in
`tools/hbcanary/fonts/` remain under OFL-1.1, with their five licence texts
alongside the fonts. These separate font assets retain their original names
and licences. The Unicode 17.0.0 bidi conformance data in `tools/hbcanary/bidi/`
(test fixtures, not shipped to the device) is under the Unicode License v3;
see `licenses/Unicode-License-v3.txt`. See `licenses/README.md` for the
component mapping.
