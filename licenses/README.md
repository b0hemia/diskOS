# Bundled license texts

This directory carries the full license texts for the third-party components the diskOS installer
bundles. See `../NOTICE.md` for the component->license mapping and the static-linking obligations.

| Component (bundled) | License | Full text |
|---|---|---|
| Unicode 17.0.0 bidi conformance data (`tools/hbcanary/bidi/`, test fixtures only) | Unicode-3.0 (Unicode License v3) | [`Unicode-License-v3.txt`](Unicode-License-v3.txt) (verbatim from https://www.unicode.org/license.txt) |
| HarfBuzz 14.6.0 | Old MIT (library); MIT, OFL-1.1, Apache-2.0 and public-domain corpus terms in retained subtrees | [`HarfBuzz-COPYING`](HarfBuzz-COPYING); additional notices under `../ui/third_party/harfbuzz-14.6.0/` |
| SheenBidi 3.0.0 | Apache-2.0 | [`SheenBidi-Apache-2.0.txt`](SheenBidi-Apache-2.0.txt) |
| Noto Sans Gurmukhi / Gurmukhi UI (canary and embedded faces) | OFL-1.1 | [`OFL-Gurmukhi.txt`](OFL-Gurmukhi.txt) |
| Noto Sans Devanagari (canary and embedded faces) | OFL-1.1 | [`OFL-Devanagari.txt`](OFL-Devanagari.txt) |
| Noto Naskh Arabic (canary and embedded face) | OFL-1.1 | [`OFL-NotoNaskhArabic.txt`](OFL-NotoNaskhArabic.txt) |
| Noto Sans Hebrew (canary and embedded face) | OFL-1.1 | [`OFL-NotoSansHebrew.txt`](OFL-NotoSansHebrew.txt) |
| Lato Regular (HarfBuzz canary) | OFL-1.1 | [`OFL-Lato.txt`](OFL-Lato.txt) |
| usbboot | GPL-2.0-or-later | [`GPL-2.0.txt`](GPL-2.0.txt) |
| squashfs-tools 4.6.1 (Linux), 4.7.5 (macOS arm64) | GPL-2.0-or-later | [`GPL-2.0.txt`](GPL-2.0.txt) |
| liblzo2 (static in mksquashfs/unsquashfs) | GPL-2.0-or-later | [`GPL-2.0.txt`](GPL-2.0.txt) |
| libusb-1.0.27 (static in usbboot) | LGPL-2.1-or-later | [`LGPL-2.1.txt`](LGPL-2.1.txt) |
| libusb 1.0.30 (macOS arm64 dylib) | LGPL-2.1-or-later | [`LGPL-2.1-libusb.txt`](LGPL-2.1-libusb.txt), copied from its source tarball |
| lzo 2.10 (macOS arm64 dylib) | GPL-2.0-or-later | [`GPL-2.0.txt`](GPL-2.0.txt) |
| lz4 1.10.0 library (macOS arm64 dylib) | BSD-2-Clause | [`BSD-2-Clause-lz4.txt`](BSD-2-Clause-lz4.txt), [`LZ4-LICENSE.txt`](LZ4-LICENSE.txt), copied from the source tarball |
| xz 5.8.4 liblzma (macOS arm64 dylib) | 0BSD; source also contains GPL-2.0-or-later parts | [`0BSD-xz.txt`](0BSD-xz.txt), [`XZ-COPYING.txt`](XZ-COPYING.txt), [`GPL-2.0.txt`](GPL-2.0.txt) |
| zstd 1.5.7 library (macOS arm64 dylib) | BSD-3-Clause or GPL-2.0-only; source also has MIT/BSD-2-Clause parts | [`BSD-3-Clause-zstd.txt`](BSD-3-Clause-zstd.txt), [`GPL-2.0-zstd.txt`](GPL-2.0-zstd.txt); full source in `../corresponding-source/macos-arm64/` |
| glibc (static in Linux usbboot) | LGPL-2.1-or-later | [`LGPL-2.1.txt`](LGPL-2.1.txt); corresponding source in `../corresponding-source/glibc_2.39*` (see `../NOTICE.md` for the relink path) |
| PyInstaller bootloader (Windows package and self-built onefile) | GPL-2.0 + bootloader exception | [`PyInstaller-COPYING.txt`](PyInstaller-COPYING.txt), [`GPL-2.0.txt`](GPL-2.0.txt) + exception note in `../NOTICE.md` |
| zlib 1.3.2 / liblzma (xz 5.8.4) (static in Linux squashfs-tools) | zlib / 0BSD | notices accompany the sources in `../corresponding-source/linux-squashfs-musl/`; see also [`0BSD-xz.txt`](0BSD-xz.txt) and [`XZ-COPYING.txt`](XZ-COPYING.txt) |
| pyusb | BSD-3-Clause | [`BSD-3-Clause-pyusb.txt`](BSD-3-Clause-pyusb.txt) (verbatim from pyusb 1.3.1) |
| pycryptodome | BSD-2-Clause + public-domain; CC0 SipHash | [`pycryptodome-LICENSE.txt`](pycryptodome-LICENSE.txt) retains the upstream wording with LF newlines; [`PyCryptodome-3.23.0-Windows-LICENSE.rst`](PyCryptodome-3.23.0-Windows-LICENSE.rst) is the exact locked Windows wheel's CRLF copy. [`PyCryptodome-3.23.0-Windows-NOTICES.txt`](PyCryptodome-3.23.0-Windows-NOTICES.txt) preserves runtime-module notices and the wheel's OCB patent statement. Native source comments: [`PyCryptodome-3.23.0-Native-NOTICES.txt`](PyCryptodome-3.23.0-Native-NOTICES.txt); LibTomCrypt dedication: [`PyCryptodome-3.23.0-libtom-LICENSE.txt`](PyCryptodome-3.23.0-libtom-LICENSE.txt); complete SipHash dedication: [`CC0-1.0-SipHash.txt`](CC0-1.0-SipHash.txt) |
| certifi 2026.2.25 | MPL-2.0 | [`MPL-2.0-certifi.txt`](MPL-2.0-certifi.txt), verbatim wheel bundle notice; complete licence: [`MPL-2.0.txt`](MPL-2.0.txt) |
| platformdirs 4.2.2 | MIT | [`MIT-platformdirs.txt`](MIT-platformdirs.txt), verbatim wheel LICENSE |
| CPython (3.12.10 x64 in the Windows dependency inventory) | PSF-2.0 and historical Python terms; incorporated libraries have separate terms | [`CPython-3.12.10-Windows-LICENSE.txt`](CPython-3.12.10-Windows-LICENSE.txt), complete exact-install Windows notice; [`PSF-Python.txt`](PSF-Python.txt) retained alongside as the platform-neutral base terms. [`CPython-3.12.10-Incorporated-Software.txt`](CPython-3.12.10-Incorporated-Software.txt) is the entire incorporated-software section from exact-install Doc-license.html, including libmpdec, dtoa, SipHash, Mersenne Twister, cfuhash, sockets and asyncio |
| OpenSSL 3.0.16 (`libcrypto-3.dll` in Windows) | Apache-2.0 | [`Apache-2.0-OpenSSL.txt`](Apache-2.0-OpenSSL.txt), verbatim from `/usr/share/common-licenses/Apache-2.0`; Copyright 1998-2025 The OpenSSL Authors per DLL metadata |
| Dynamic Windows libusb 1.0.27 (official VS2022/MS64 DLL) | LGPL-2.1-or-later | [`windows/libusb-COPYING.txt`](windows/libusb-COPYING.txt), byte-identical to the local 1.0.27 source archive's COPYING. DLL and source hashes: [`../build/windows-libusb.json`](../build/windows-libusb.json). The source and its upstream signature ship alongside the DLL. Its signature uses signing fingerprint `9C7EA94939C69C4FBC3DBFA8AA0639079EFB61B9`. Preserve the pinned DLL/source/signature and notices, as recorded in [`../build/WINDOWS_FROZEN_DEPENDENCIES.md`](../build/WINDOWS_FROZEN_DEPENDENCIES.md) |
| Windows zlib 1.3.1 DLL | zlib | [`zlib-1.3.1-LICENSE-README.txt`](zlib-1.3.1-LICENSE-README.txt), complete upstream 1.3.1 README with licence; copied verbatim from the installed development package. [`windows/zlib-LICENSE.txt`](windows/zlib-LICENSE.txt) remains for MSYS static zlib 1.3.2 |
| Windows libffi (`libffi-8.dll`) | MIT-style libffi | [`CPython-3.12.10-Windows-LICENSE.txt`](CPython-3.12.10-Windows-LICENSE.txt), lines 366-386, includes the exact-install notice: Copyright (c) 1996-2022 Anthony Green, Red Hat, Inc and others |
| Windows Tcl/Tk 8.6.15 | Tcl-Tk permissive and incorporated terms; Tango script declares CC BY-SA 3.0 | CPython's Tcl base notice matches supplied upstream Tcl 8.6.15 after newline normalization. [`Tk-8.6.15-license.terms.txt`](Tk-8.6.15-license.terms.txt) is the exact-install Tk text. Tcl incorporated texts: [`Tcl-8.6.15-regex-NOTICE.txt`](Tcl-8.6.15-regex-NOTICE.txt), [`Tcl-8.6.15-libtommath-LICENSE.txt`](Tcl-8.6.15-libtommath-LICENSE.txt), [`Tcl-8.6.15-Bison-NOTICE.txt`](Tcl-8.6.15-Bison-NOTICE.txt), [`Tcl-8.6.15-zlib-LICENSE.txt`](Tcl-8.6.15-zlib-LICENSE.txt). Tk native and runtime script headers: [`Tk-8.6.15-Incorporated-NOTICES.txt`](Tk-8.6.15-Incorporated-NOTICES.txt). Startup-required icons.tcl keeps [`CC-BY-SA-3.0-Tango.txt`](CC-BY-SA-3.0-Tango.txt). Windows builds exclude images/ and mkpsenc.tcl, including Adobe EPS procsets and glyph data |
| Microsoft VC runtime 14.42.34438.0 | Microsoft Distributable Code redistributed with CPython 3.12.10 | CPython's "Additional Conditions for this Windows binary build" are shipped verbatim at Windows notice lines 283-320. Microsoft's terms govern; [Visual Studio 2022 redistribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution), "Visual C++ Runtime Files". Microsoft components remain under Microsoft's terms, outside this project's licences. Redistributors must satisfy Microsoft's conditions; the "licensed Visual Studio user" condition is a separate Microsoft-terms question. |
| MSYS2 runtime 3.6.10 (both packaged copies) | LGPL-3.0-or-later with Cygwin linking exception; newlib/BSD/per-file terms | Full five-file notice set indexed in [`windows/README.md`](windows/README.md) |
| Archivo (installer GUI: pre-rendered titles in `diskos_installer/assets/display*.png` and lettering in `wordmark*.png`) | OFL-1.1 (font) | [`LICENSE-archivo-OFL.txt`](../diskos_installer/assets/LICENSE-archivo-OFL.txt) |
| Michroma (installer GUI: pre-rendered section kickers in `diskos_installer/assets/display*.png`) | OFL-1.1 (font) | [`LICENSE-michroma-OFL.txt`](../diskos_installer/assets/LICENSE-michroma-OFL.txt) |
| LVGL (diskOS UI) | MIT | [`MIT-LVGL.txt`](MIT-LVGL.txt) (verbatim from `../ui/lvgl/LICENCE.txt`) |
| FreeType 2.13.3 (diskOS UI, new in 1.2.1) | FreeType License (FTL) | [`FTL-FreeType.txt`](FTL-FreeType.txt) (verbatim from `../ui/third_party/freetype-2.13.3/docs/FTL.TXT`) |
| dlg headers retained in FreeType's include tree, Copyright (c) 2019 nyorain | BSL-1.0 | [`BSL-1.0-dlg.txt`](BSL-1.0-dlg.txt) (standard complete BSL-1.0 text, byte-identical to the copy GCC ships as `libphobos/src/LICENSE_1_0.txt`) |
| mbedTLS 3.6.7 (diskOS UI Cast/AirPlay TLS, new in 1.2.2) | Apache-2.0 (upstream: Apache-2.0 OR GPL-2.0-or-later) | [`Apache-2.0-mbedTLS.txt`](Apache-2.0-mbedTLS.txt) (verbatim from `../ui/third_party/mbedtls-3.6.7/LICENSE`, contains both texts) |
| Monocypher 4.0.2 (diskOS UI AirPlay pairing, new in 1.2.2) | BSD-2-Clause OR CC0-1.0 | [`BSD-2-Clause-Monocypher.txt`](BSD-2-Clause-Monocypher.txt) (verbatim from `LICENCE.md`) |
| libplist 2.6.0 (static in `mq_ui`, new in 1.2.2) | LGPL-2.1-or-later | [`LGPL-2.1-libplist.txt`](LGPL-2.1-libplist.txt) (verbatim `COPYING.LESSER`; differs from `LGPL-2.1.txt` only in whitespace); see `../NOTICE.md` |
| libplist `time64.c`, Copyright (c) 2007-2010 Michael G Schwern | MIT | [`MIT-libplist-time64.txt`](MIT-libplist-time64.txt) (lines 1-27 of `time64.c`) |
| stb_image_write.h v1.16 (diskOS UI cover JPEG for AirPlay, new in 1.2.2), Copyright (c) 2017 Sean Barrett | MIT OR Unlicense | [`MIT-or-Unlicense-stb_image_write.txt`](MIT-or-Unlicense-stb_image_write.txt) (verbatim licence comment of `../ui/third_party/stb-1ee679ca/stb_image_write.h`) |
| libplist `jsmn.c`, Copyright (c) 2010 Serge A. Zaitsev | MIT | [`MIT-libplist-jsmn.txt`](MIT-libplist-jsmn.txt) (lines 1-24 of libplist's `src/jsmn.c`) |
| SpeexDSP 1.2.1 (`resample.c`, diskOS UI, new in 1.2.2) | BSD-3-Clause | [`BSD-3-Clause-SpeexDSP.txt`](BSD-3-Clause-SpeexDSP.txt) (verbatim `COPYING`) + [`BSD-3-Clause-SpeexDSP-resampler.txt`](BSD-3-Clause-SpeexDSP-resampler.txt) (lines 1-32 of `libspeexdsp/resample.c`: Jean-Marc Valin and Thorvald Natvig) |
| dr_libs (`dr_flac.h`, `dr_mp3.h`, `dr_wav.h`, diskOS UI, new in 1.2.2) | Unlicense OR MIT-0 | [`Unlicense-or-MIT-0-dr_libs.txt`](Unlicense-or-MIT-0-dr_libs.txt) (verbatim `LICENSE`) |
| nanopb 0.4.9.1 (diskOS UI Cast, new in 1.2.2) | zlib | [`Zlib-nanopb.txt`](Zlib-nanopb.txt) (verbatim `LICENSE.txt`) |
| Cast channel schema from openscreen (diskOS UI, new in 1.2.2) | BSD-3-Clause | [`BSD-3-Clause-openscreen.txt`](BSD-3-Clause-openscreen.txt) (verbatim `cast/openscreen-LICENSE`) |
| Noto Sans, Noto Sans Hebrew (Hebrew new in 1.2.2; `font_intl_14` to `font_intl_20` in `mq_ui`) | OFL-1.1 | [`OFL-1.1-NotoSans.txt`](OFL-1.1-NotoSans.txt) |
| JSMN (diskOS UI JSON parser) | MIT | [`MIT-JSMN.txt`](MIT-JSMN.txt) (verbatim from `../ui/jsmn.h`, (c) 2010 Serge Zaitsev) |
| Font Awesome Free glyphs (via LVGL `LV_SYMBOL_*`) | OFL-1.1 (fonts) + CC-BY-4.0 (icons) | [`OFL-1.1-FontAwesome.txt`](OFL-1.1-FontAwesome.txt) (Fonticons' own LICENSE.txt: full OFL-1.1 + MIT; also covers the diskOS Beam glyph U+F8D0, OFL-1.1) + [`CC-BY-4.0.txt`](CC-BY-4.0.txt) |
| musl libc (static in diskOS UI `mq_ui` and Linux `mksquashfs`/`unsquashfs`) | MIT | [`MIT-musl.txt`](MIT-musl.txt) |
| LodePNG 20230410 (Cast artwork PNG encoder in `mq_ui`, via LVGL) | zlib | [`Zlib-LodePNG.txt`](Zlib-LodePNG.txt) (lines 1-24 of `lodepng.c`) |
| gifdec (GIF decoder in `mq_ui`, via LVGL) | Public domain upstream; LVGL changes MIT | [`MIT-LVGL.txt`](MIT-LVGL.txt); upstream dedication: https://github.com/lecram/gifdec |
| SQLite (amalgamation in `mq_ui`) | Public domain | https://sqlite.org/copyright.html (no license text required) |
| QR Code generator, Nayuki (via LVGL, in `mq_ui`) | MIT | [`MIT-qrcodegen.txt`](MIT-qrcodegen.txt) (verbatim from the LVGL source header) |
| TJpgDec, ChaN (via LVGL, in `mq_ui`) | BSD-style | [`LICENSE-TJpgDec.txt`](LICENSE-TJpgDec.txt) (verbatim from the LVGL source header) |
| Montserrat font (via LVGL built-in fonts, in `mq_ui`) | OFL-1.1 | [`OFL-1.1-Montserrat.txt`](OFL-1.1-Montserrat.txt) |
| Source Han Sans SC (CJK fallback font in `mq_ui`) | OFL-1.1 | [`OFL-1.1-SourceHanSans.txt`](OFL-1.1-SourceHanSans.txt) |
| IBM Plex Mono (converted as `diskos_mono`, "something" theme, in `mq_ui`) | OFL-1.1 (Reserved Font Name "Plex") | [`OFL-1.1-IBMPlexMono.txt`](OFL-1.1-IBMPlexMono.txt) |
| Doto (converted as `diskos_dot`, "something" theme, in `mq_ui`) | OFL-1.1 | [`OFL-1.1-Doto.txt`](OFL-1.1-Doto.txt) |
| Inter (converted as `diskos_hifi_ui`, hi-fi theme text, in `mq_ui`) | OFL-1.1 | [`OFL-1.1-Inter.txt`](OFL-1.1-Inter.txt) |
| Archivo (converted as `diskos_brand_disk_32` / `diskos_brand_os_32`, the diskOS wordmark on the boot splash, in `mq_ui`) | OFL-1.1 | [`OFL-1.1-Archivo.txt`](OFL-1.1-Archivo.txt) |
| DSEG7 Classic (converted as `diskos_hifi_seg`, hi-fi theme clock and time counters, in `mq_ui`) | OFL-1.1 (Reserved Font Name "DSEG") | [`OFL-1.1-DSEG.txt`](OFL-1.1-DSEG.txt) |
| JetBrains Mono (converted as `diskos_term`, terminal theme text and clock, in `mq_ui`) | OFL-1.1 | [`OFL-1.1-JetBrainsMono.txt`](OFL-1.1-JetBrainsMono.txt) |
| Space Grotesk (converted as `diskos_bauh`, bauhaus theme text, titles and clock, in `mq_ui`) | OFL-1.1 | [`OFL-1.1-SpaceGrotesk.txt`](OFL-1.1-SpaceGrotesk.txt) |
| IBM Plex Sans (converted as `diskos_blue_sans`, blueprint theme text, in `mq_ui`) | OFL-1.1 (Reserved Font Name "Plex") | [`OFL-1.1-IBMPlexSans.txt`](OFL-1.1-IBMPlexSans.txt) |
| Nunito (converted as `diskos_stone`, stone theme text and clock, in `mq_ui`) | OFL-1.1 | [`OFL-1.1-Nunito.txt`](OFL-1.1-Nunito.txt) |
| Courier Prime (converted as `diskos_zine`, zine theme text and clock, in `mq_ui`) | OFL-1.1 | [`OFL-1.1-CourierPrime.txt`](OFL-1.1-CourierPrime.txt) |
| Rubik Glitch (converted as `diskos_zine_glitch`, zine theme titles, in `mq_ui`) | OFL-1.1 | [`OFL-1.1-RubikGlitch.txt`](OFL-1.1-RubikGlitch.txt) |
| MD5, Peslyak/"Solar Designer" (in `mq_ui`) | Public domain | in `../ui/md5.c`; no license text required |
| dropbearmulti (Dropbear SSH 2022.83, Debug Mode) | MIT-style | [`LICENSE-dropbear.txt`](LICENSE-dropbear.txt) (verbatim upstream) |
| disc_spl_lpddr3.bin (X2000 stage-1 SPL) | GPL-2.0 | [`GPL-2.0.txt`](GPL-2.0.txt); corresponding source in `../spl-src/` (see `../SPL_SOURCE.md`) |

All texts above are included **verbatim from the actual upstream sources** (not hand-written), with
their real copyright lines. **Copyleft texts (GPL-2.0, LGPL-2.1)** carry the redistribution
obligations (corresponding source + relink path; see `../NOTICE.md`). The macOS arm64 source archives, formula snapshots and build note are in
`../corresponding-source/macos-arm64/`. Linux zlib and liblzma notices travel with
the corresponding source.

## Windows libusb source correspondence

The pinned official libusb 1.0.27 VS2022/MS64 DLL has SHA256
`d4e5db4fad8bef7201dc5a4a71ca997e6dccc9da25e8a988d4be63b13a02208d`.
The shipped libusb-1.0_1.0.27.orig.tar.bz2 source archive has SHA256
`ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575`
and is byte-identical to upstream libusb-1.0.27.tar.bz2. Its upstream signature
uses signing fingerprint `9C7EA94939C69C4FBC3DBFA8AA0639079EFB61B9`;
the signing key is supplied in libusb's Debian 1.0.27-1 source package.
Preserve the pinned DLL, source, signature and notices when packaging.

## Windows Python notice provenance and remaining checks

`PSF-Python.txt` matches the local CPython 3.12 base LICENSE byte for byte:
13936 bytes, SHA256
`3b2f81fe21d181c499c59a256c8e1968455d6689d269aa85373bfb6af41da3bf`.
Its base terms are the prefix of the exact-install Windows notice after
CRLF-to-LF normalization. Keep it alongside that notice for the existing
platform-neutral Python reference; it does not replace the Windows appendices.
`Apache-2.0-OpenSSL.txt` is a complete licence text, 11358 bytes, SHA256
`cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`.
The existing `bundled/libcrypto.copyright.txt` is a Debian attribution file
with a link to Apache-2.0, not the complete Apache licence.

`CPython-3.12.10-Windows-LICENSE.txt` is copied byte for byte from
`LICENSE.txt` from the exact CPython 3.12.10 x64
installation. It is 36874 bytes,
702 lines, SHA256
`e502c6b880ff58d614901495a9009c136539cd0b1e2a2abb8fc00b934c203419`.
It includes Microsoft conditions (283-320), bzip2 (326-362), libffi (366-386),
Apache-2.0 (389-564) and separate Tcl/Tk-style texts (566-605 and 607-646).
The Tcl/Tk pointer at 693-696 is additional to those texts. No zlib licence
text appears in this file, so its upstream 1.3.1 notice is provided separately.

`Tk-8.6.15-license.terms.txt` is copied byte for byte from the supplied
`tcl_tk8.6_license.terms` from that CPython 3.12.10 x64 installation: 2307 bytes,
SHA256 `0d1e4405f6273f091732764ed89b57066be63ce64869be6c71ea337dc4f2f9b5`.
This is the `_internal/_tk_data/license.terms` hash. Its text matches
Windows LICENSE lines 607-646 after newline normalization. The earlier text
omits Apple Inc. and has a different DFARS clause; the two are not identical.
The supplied Tcl 8.6.15 source's license.terms matches the earlier
CPython Tcl section after newline normalization; no separate base copy is
required. Additional Tcl component terms are preserved below. Supplied
Tk 8.6.15 source matches the exact-install base text after newline normalization.
Windows builds exclude unused images/ and mkpsenc.tcl and retain icons.tcl
with its complete attribution header and Tango notice.

`zlib-1.3.1-LICENSE-README.txt` is the complete upstream README decompressed
from `/usr/share/doc/zlib1g-dev/README.gz` without edits: 5317 bytes, SHA256
`d106dcdb2a0b3087cb3a2380b9f00a0bbadc7eb835cd80408f94a2d78d69727d`.
The installed development package version is
`1:1.3.dfsg+really1.3.1-1ubuntu3.1`; its header and README identify upstream
1.3.1 and Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler.
The DLL metadata lists Copyright (C) 1995-2022.

The Windows spec explicitly includes these notices. The inventory requires
canonical notice bytes for the Python DLL, 12 stdlib extensions, runtime DLLs
and any Crypto/**/*.pyd. See `../build/WINDOWS_FROZEN_DEPENDENCIES.md` for the
component mapping and packaging requirements.

## Incorporated and locked-wheel text extraction

`CPython-3.12.10-Incorporated-Software.txt` is the entire "Licenses and
Acknowledgements for Incorporated Software" section of the exact-install
`Doc-license.html` from CPython 3.12.10 x64 (SHA256
`4633449165a1a6f16f5e2497458ecbf94f1f9a20f08606553ee5d3d948c1a12a`).
Conversion uses Python's stdlib HTMLParser with convert_charrefs=True; select
the section with id licenses-and-acknowledgements-for-incorporated-software
and all nested sections. Extract visible h2, h3, p and pre text in document
order, discarding only the heading permalink anchor (class headerlink).
Decode HTML entities; preserve pre text exactly after universal newline
decoding, removing trailing LF separators only. Collapse rendered prose
whitespace, join blocks with two LF characters and append one final LF.
Write UTF-8, preserving upstream spelling, punctuation and copyright wording;
no ASCII rewrite of upstream text. There are 67 blocks, 35861 bytes, SHA256
`345c1a8708efb0ea43e34dc83a8b0bd05dce82decb4f5d8de8dee4932f7367d6`.
This is a conservative notice superset; it includes expat/audioop and other
code absent from the native inventory. It does not imply that all
listed software ships. XZ/bzip2 notices remain separate.

The Tcl notice inputs are from the Tcl 8.6.15 source tree. libtommath/LICENSE and
compat/zlib/LICENSE are copied as whole byte streams. The regex notice is
generic/regcomp.c's complete first comment through its closing */ plus LF;
the Bison notice is generic/tclDate.c's unchanged byte prefix before the
"C LALR(1) parser skeleton" comment. Both include the original comment syntax.
They are attribution sidecars in addition to base Tcl terms. Binary regex
credit, public-domain libtommath notices and zlib documentation credit are
not required by their respective grants; keeping the texts makes the
incorporated terms readable. Bison's larger-work exception is preserved
alongside the already shipped GPL-3.0 text.

The exact locked wheel members are pyusb-1.3.1.dist-info/LICENSE and
pycryptodome-3.23.0.dist-info/LICENSE.rst. Read them with zipfile, without
installing/importing wheel code. PyUSB's existing file is byte-identical.
PyCryptodome's exact CRLF member is copied to the Windows LICENSE.rst;
the LF file is retained. The Windows NOTICES.txt concatenates the 16
distinct opening comment blocks containing Copyright from sorted Crypto/*.py
wheel paths (recursively, excluding Crypto/SelfTest/), followed by the complete
opening triple-quoted docstring from Crypto/Cipher/_mode_ocb.py plus CRLF.
Every extracted block is byte-preserved, including its comment markers,
author notices, legacy notices and the wheel's historical OCB patent statement.
Its SHA256 is `8346eef7cc9f8a11bf8cfec9cb5fef6f5ea862274100dab1385ce3fd754f0131`.
The selected wheel contains no separate libtom or Keccak licence file and no
C source; the matching-version sdist supplies native terms below.
The historical wheel statement does not establish current patent status or a grant.

## Tk and PyCryptodome native source notice extraction

The Tk tarball SHA256 is
`550969f35379f952b3020f3ab7b9dd5bfd11c1ef7c9b7c6a75f5c49aca793fec`;
The local PyCryptodome 3.23.0 PyPI sdist
SHA256 is `447700a657182d60338bab09fdb27518f8856aecd80ae4c6bdddb67ff5da44ef`.

`Tk-8.6.15-Incorporated-NOTICES.txt` preserves 221 upstream blocks.
Select the 142 C objects in win/makefile.vc's XLIBOBJS/TKOBJS/TTK_OBJS and
cross-check the identical win/Makefile.in lists; resolve explicit object-to-source
rules (tkMain2 uses tkMain.c). Add all headers under generic/, win/, xlib/ and
compat/, and win/rc/*.rc as a conservative superset. For each path in sorted
order, select complete C comments containing copyright, public domain, permission,
licence/license or LibTomCrypt (case-insensitive), keeping distinct byte streams.
Append distinct opening comment/blank-line prefixes from sorted library/**/*.tcl
outside demos/ when they contain copyright or licence/license. In particular,
the complete icons.tcl header preserves the Tango attribution and CC BY-SA 3.0
link. Append the complete library/images/README logo permission as the final
block. Concatenate each block unchanged followed by one LF. This includes base
copyrights, optional/unused headers and resource scripts, not a claim that every
listed file compiled or shipped. Size 108173 bytes; SHA256
`98519bd25a1db194e2d1a9df1703a955b8574b45c96b01dcc309808c2bcda589`.

## Collected Tk resource notice copies and limits

`CC-BY-SA-3.0-Tango.txt` is the complete standalone CC-BY-SA-3.0 stanza
from `/usr/share/doc/libglib2.0-data/copyright:721-1082`, copied byte for
byte from the heading through the Creative Commons Notice, excluding the
following blank separator before the next License stanza. Size 22735 bytes,
SHA256 `40b2aac4aee68f59f6f6dc9aeb1beec64c6abef4a925ca04c1767db85a554eda`.
The SPDX reference, indentation and DEP-5 paragraph markers are preserved;
no licence wording is rewritten. This is a locally available legal-code copy,
not online authentication or evidence of later Tango relicensing. Tk's
icons.tcl declares this licence; its original header and both URLs remain
in Tk-8.6.15-Incorporated-NOTICES.txt and NOTICE.md adds explicit attribution.
Sections 4(a)-(c) require a licence copy/URI, retained notices, applicable
adaptation terms and supplied attribution information. icons.tcl SHA256
`1264940e62b9a37967925418e9d0dc0befd369e8c181b9bab3d1607e3cc14b85`
matches Tk 8.6.15 library/icons.tcl after LF-to-CRLF conversion. The script
and embedded icons are unmodified; its complete header retains the Tango
project credit, Copyright (c) 2009 Pat Thoyts, licence URI and source URI.
The complete CC BY-SA 3.0 text is shipped alongside the unmodified script.

New Windows packages must exclude `_tk_data/mkpsenc.tcl` and the whole
`_tk_data/images/` subtree. This excludes Adobe glyph data and EPS procsets,
plus unused logo GIFs. The shipped Tk incorporated notice superset is retained
byte for byte, including its optional/unused source and logo notices.

The Tango notice remains allowlisted, hash-pinned and required when icons.tcl
is collected. Tk 8.6.15 sources that script unconditionally even on Windows
(library/tk.tcl:502-517); native message boxes do not remove this dependency.
Preserve the script/header and notices, and exclude unused Tk data when
packaging. The dialog procedure is in `../build/windows-qualification.md`.

## PyCryptodome native source notice extraction

`PyCryptodome-3.23.0-Native-NOTICES.txt` uses the same C-comment selection and
concatenation. Parse setup.py's 42 Extension declarations using ast and
literal_eval only. All names equal the exact wheel's 42 native names. Follow
literal quoted/angle includes recursively, trying the including file's directory,
src/ and src/libtom/ in order, including both conditional architecture branches.
The resulting 90-file closure has 26 distinct selected comments, 29901 bytes,
SHA256 `45d0abd51a4ff1c47d1e27d8fe020bb3c075027f2b10f4b5f6e0f6ab024a17ce`.
Preserve original syntax, spelling and punctuation; no rewritten grants.

`PyCryptodome-3.23.0-libtom-LICENSE.txt` is the complete sdist member
Doc/LEGAL/copy/LICENSE.libtom: 82 bytes, SHA256
`10c41e1aaf84bd3546d1ff7ef89454817744fd2c18cdd80a137e802095448016`.
`CC0-1.0-SipHash.txt` is copied whole from /usr/share/common-licenses/CC0-1.0,
7048 bytes, SHA256
`a2010f343487d3f7618affe54f789f5487602331c0a8d03f49e9a7c547cf0499`.
SipHash's header asks for that dedication; it is supplied alongside its preserved
copyright/dedication header. BSD binary notices are required; public-domain
LibTomCrypt/Keccak/Rijndael comments are retained for clarity. The spec ships these sidecars; the inventory requires the Tk
sidecar for tk86t.dll/_tkinter.pyd and all three crypto sidecars for any
Crypto/**/*.pyd.

## diskOS's own license
diskOS is free software under the **GNU GPL v3.0 or later** (`../LICENSE`); third-party components keep
their own licences (see [`../NOTICE.md`](../NOTICE.md)). The **Python installer, build scripts, and docs**
are licensed **GPL-3.0-or-later** - `Copyright (c) 2026 diskOS contributors`. Releases up to 1.2.2 published
them under MIT; that text is kept as [`MIT-diskos-pre-1.2.3.txt`](MIT-diskos-pre-1.2.3.txt). The one
MIT exception is the stock-audio helper (`../ui/audio_stock_wire.h`, `../ui/tools/audio_stock_start.S`,
`../ui/tools/diskos_audiodec.c`, `../ui/third_party/build-stock-audiodec.sh`, see
`../ui/tools/AUDIO_STOCK_LICENSE`): it loads the Disc's stock FFmpeg, which FiiO configured with
`--enable-nonfree`, so it stays MIT on purpose. The installer GUI is GPL tooling; the
Archivo and Michroma font notices for its pre-rendered lettering are linked above
and ship beside `display*.png`, `wordmark*.png` and `disc*.png` in
`../diskos_installer/assets/`. The disc illustration contains no font lettering.
No fonts are installed on the computer. The **diskOS UI source** (`ui/`, built to
`payload/mq_ui`) is licensed **GPL-3.0-or-later** (`GPL-3.0.txt`, `../ui/COPYING`; (c) diskOS
contributors), the same licence as the tooling since 1.2.3. The UI is based in part on the work of the FreeType Team.
Its embedded third-party notices - LVGL (MIT), SQLite (public domain), JSMN (MIT), md5 (public domain), and
Montserrat / Source Han Sans / Font Awesome / IBM Plex Mono / Doto and the theme fonts (Inter, DSEG7 Classic, JetBrains Mono, Space Grotesk, IBM Plex Sans, Nunito, Courier Prime, Rubik Glitch, Archivo) (OFL-1.1) - ship in this directory and in
`../ui/licenses/`.

## Website

The HTML, CSS, JavaScript, and diskOS-owned screenshot images in `../site/` are
GPL-3.0-or-later under [`../LICENSE`](../LICENSE), Copyright (c) 2026 diskOS contributors.
This grant covers only rights held by diskOS contributors; third-party artwork and
trademarks remain with their respective owners.

### Website fonts

Font files are redistributed unchanged from the website source. The following
texts ship beside them in `../site/fonts/`; their upstream copyrights are retained.

| Font file | Licence | Text in `../site/fonts/` |
|---|---|---|
| `archivo-100-900.woff2` | OFL-1.1 | `LICENSE-archivo.txt` |
| `archivoblack-400.woff2` | OFL-1.1 | `LICENSE-archivoblack.txt` |
| `barlowcond-300.woff2` | OFL-1.1 | `LICENSE-barlowcond.txt` |
| `doto-700.woff2` | OFL-1.1 | `LICENSE-doto.txt` |
| `hanken-300-700.woff2` | OFL-1.1 | `LICENSE-hanken.txt` |
| `jetbrains-500-700.woff2` | OFL-1.1 | `LICENSE-jetbrains.txt` |
| `michroma-400.woff2` | OFL-1.1 | `LICENSE-michroma.txt` |
| `nunito-800.woff2` | OFL-1.1 | `LICENSE-nunito.txt` |
| `sharetech-400.woff2` | OFL-1.1 | `LICENSE-sharetech.txt` |
| `specialelite-400.woff2` | Apache-2.0 | `LICENSE-specialelite.txt` |

Special Elite's embedded copyright notice reads: Copyright (c) 2010 by Brian J.
Bonislawsky DBA Astigmatic (AOETI). All rights reserved. Available under the Apache
2.0 licence. The font retains that notice in its name table; the full Apache-2.0
text is shipped alongside it. These font licences do not change the website's GPL grant.
