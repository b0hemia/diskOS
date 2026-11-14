# Windows frozen dependency inventory and build wiring

The Windows x64 package uses a shared onedir runtime for the GUI and CLI.
This inventory describes CPython 3.12.10 and the locked build inputs, native
components, licence notices and packaging requirements. The native-data
inventory reference is installer commit `f0a021a`, ZIP SHA256
`4b2687f0322c366dee772cb53d60011f1ea6229acbad28a711cad4f227048b3a`.

## Lock evidence and remaining work

`windows-build.lock` includes the runtime pins from `../requirements.txt`
and the PyInstaller build closure. PyInstaller is pinned to 6.16.0 with
altgraph, packaging, pefile, hooks-contrib, pywin32-ctypes and setuptools.
Every dependency needs an exact version and Windows/universal wheel hash;
there is no floating fallback.

Evidence: SHA256 of decoded cached simple-index JSON (no host paths):

| Project | Index SHA256 |
|---|---|
| pyinstaller | bb268a50bd9726fb2ab2bbacd6c7b171daa72e2a9e16cc81d8a98e3724608472 |
| pyinstaller-hooks-contrib | 37f359f8d78f7b55d17a10e79700fe90a42814166fbf315b6be9c4c953244796 |
| altgraph | b5188e67497c83625cc35496c7b92472d550e17e39935d95f3330f240fe6c4cdc |
| packaging | 2669250025b2b2b09ae5c3577e2d5cb80447fc4a5d2142f4352ae1b179f674a3 |
| setuptools | 72886f1df0fcae7280b67c9318c1b6a0d771600b8445c7da7b3a0890869b18e9 |
| pyusb | 57edc64b06d9dd8e738094f13dfd7a4b0a9b802cfd4beb404a372f71df823140 |
| pycryptodome | 0243b3c0d06544c033317edc4e56cf749327eda85a9e03c09334d4f89cc8192a |

No decoded simple-index JSON snapshot was recorded for certifi or platformdirs.
Their downloaded wheel hashes are recorded separately below and in `windows-build.lock`.

| Wheel | Wheel SHA256 |
|---|---|
| certifi-2026.2.25-py3-none-any.whl | 027692e4402ad994f1c42e52a4997a9763c646b73e4096e4d5d6db8af1d6f0fa |
| platformdirs-4.2.2-py3-none-any.whl | 2d7a1657e36a80ea911db832a8a6ece5ee53d8de21edd5cc5879af6530b1bfee |

## Frozen notices checklist

| Expected component | Evidence/text to ship and verify |
|---|---|
| PyInstaller 6.16.0 bootloader | `licenses/PyInstaller-COPYING.txt` contains the licensing terms and Bootloader Exception. Source: https://github.com/pyinstaller/pyinstaller/blob/v6.16.0/COPYING. |
| CPython DLL, stdlib, extension modules | Complete CPython 3.12.10 Windows LICENSE plus `licenses/CPython-3.12.10-Incorporated-Software.txt` from Doc-license.html. |
| Tcl/Tk DLLs and resource scripts | Tcl/Tk 8.6.15 base and incorporated notices. Exclude unused images/ and mkpsenc.tcl. Keep startup-required icons.tcl with its attribution header and complete CC BY-SA 3.0 text. |
| pyusb 1.3.1 | `licenses/BSD-3-Clause-pyusb.txt` is the locked wheel LICENSE. |
| pycryptodome 3.23.0, including Crypto native modules | Exact CRLF wheel LICENSE, runtime-module notices, native-source notices, LibTomCrypt dedication and CC0 SipHash dedication. |
| certifi 2026.2.25 (cacert.pem is shipped) | `licenses/MPL-2.0-certifi.txt` preserves the wheel's bundle notice; `licenses/MPL-2.0.txt` contains the complete MPL 2.0 text. |
| platformdirs 4.2.2 | `licenses/MIT-platformdirs.txt` (MIT; byte-identical to the 4.2.2 wheel LICENSE). |
| Remaining Python/vendor runtime DLLs | OpenSSL 3.0.16, zlib 1.3.1, VC runtime 14.42.34438.0 and libusb 1.0.27; libffi's exact-install notice is in the Windows LICENSE. See the component table below. |
| Native helpers and MSYS2 runtime | Preserve `licenses/windows/`, `NOTICE.md` and source/relink/signature sidecars byte for byte. |
| FreeType 2.13.3 in the UI (new in 1.2.1) | `licenses/FTL-FreeType.txt`, copied verbatim from `ui/third_party/freetype-2.13.3/docs/FTL.TXT`; FreeType Team attribution in `NOTICE.md`. |
| dlg headers retained in the UI sources | Copyright (c) 2019 nyorain; complete `licenses/BSL-1.0-dlg.txt` and attribution in `NOTICE.md`. The UI leaves `FT_DEBUG_LOGGING` disabled. |

The `licenses/PyInstaller-COPYING.txt` SHA256 is
`dcf75fdb959db1e3b41c0f8505069d2ece781b5ec6b3d0a4d30975cfc6580245`.
It is the licence text, not a fetch-note placeholder. The Windows spec retains
its placeholder rejection gate and explicitly includes the FTL and BSL texts.

Audit Analysis for build tools unexpectedly collected into the runtime.
Ship this inventory, the lock, notices, source references and metadata
as readable sidecars. Record interpreter/seed-pip versions, wheel hashes, source
revision, native manifest hash and freeze options without machine paths or clocks.

## Current build/spec wiring

1. `build-windows.ps1` requires native Windows x64 Python and creates a fresh venv
   for each run. The supported standard CPython range is >=3.11,<3.15; the lock
   records CPython 3.12.10. Flash enablement requires a clean source revision;
   the spec also rejects the PyInstaller fetch-note marker before freezing.
2. The venv installs `build/windows-build.lock` with `--require-hashes` and
   `--only-binary=:all:`, including dependencies. `-Wheelhouse` adds `--no-index`
   and `--find-links` for offline installation. The required `-LibusbDll` is a
   local official VS2022/MS64 DLL, verified against `windows-libusb.json` before
   creating the venv, then copied into the fresh run's `work/libusb/` directory.
   No libusb binary is committed or downloaded by the wrapper.
3. The spec creates both onedir EXEs with shared `_internal` dependencies.
   Staging copies the collection, promotes explicitly selected licence/source
   sidecars, and adds recovery notes, the lock and this inventory document.
   Windows uses a generated `hook-usb.py` that collects no host library, an
   explicit pinned DLL input, and a post-Analysis refusal gate. Linux/macOS
   retain their existing contributed hook and library collection.
4. `BUILD-METADATA.txt` records interpreter/seed-pip versions, PyInstaller version,
   source revision, native manifest hash and freeze options. `SHA256SUMS` covers
   every regular staged file except itself with UTF-8/LF, lowercase hashes,
   two spaces and sorted relative POSIX paths.
5. The build runs `deterministic_zip.py` twice to fresh output paths and checks
   matching hashes. Sorted names, fixed attributes/time and ZIP_STORED establish
   archive determinism. This does not make the compiled EXEs reproducible.

## Measured native runtime and notice coverage limits

The inventory uses CPython 3.12.10 x64. Component versions come from DLL
metadata; canonical notices and the packaging allowlist are in `licenses/`
and `build/diskos-installer.spec`.

| Component / version | Licence | Complete notice | Source / packaging requirement |
|---|---|---|---|
| CPython 3.12.10 x64 (`python312.dll` and stdlib `.pyd` files) | PSF-2.0 and historical Python terms; incorporated libraries have separate terms | Windows and incorporated texts, plus base terms | `licenses/CPython-3.12.10-Incorporated-Software.txt`, `licenses/CPython-3.12.10-Windows-LICENSE.txt` and `licenses/PSF-Python.txt`. |
| OpenSSL 3.0.16 (`libcrypto-3.dll`, `_hashlib.pyd`) | Apache-2.0 | `licenses/Apache-2.0-OpenSSL.txt` | Verbatim from `/usr/share/common-licenses/Apache-2.0`; SHA256 `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`. Copyright 1998-2025 The OpenSSL Authors in DLL metadata. Windows LICENSE also carries Apache-2.0 at lines 389-564. |
| libffi (`libffi-8.dll`, 39696 bytes; no version metadata) | MIT-style libffi terms | Windows LICENSE lines 366-386 | Copyright (c) 1996-2022 Anthony Green, Red Hat, Inc and others; permission and disclaimer retained verbatim. The ABI filename does not identify a source version. |
| zlib 1.3.1 (`zlib1.dll`) | zlib | `licenses/zlib-1.3.1-LICENSE-README.txt` | Upstream 1.3.1 README; copyright 1995-2024. Retain `licenses/windows/zlib-LICENSE.txt` for MSYS static zlib 1.3.2. DLL metadata says 1995-2022; do not rewrite copyright text to match metadata. |
| Tcl 8.6.15 (`tcl86t.dll`) | Tcl permissive and incorporated terms | Windows LICENSE lines 566-605 plus Tcl sidecars | Tcl base text matches supplied source after newline normalization; incorporated texts below. |
| Tk 8.6.15 (`tk86t.dll`) | Tk/Tcl, X11 and giftoppm grants; Tango script CC BY-SA 3.0 | Base, incorporated and Tango texts | `licenses/Tk-8.6.15-license.terms.txt`, `licenses/Tk-8.6.15-Incorporated-NOTICES.txt` and `licenses/CC-BY-SA-3.0-Tango.txt`. Exclude images/ and mkpsenc.tcl; retain icons.tcl. |
| Microsoft `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, both 14.42.34438.0 | Microsoft Visual Studio runtime redistribution terms | Windows LICENSE lines 283-320 | Microsoft Distributable Code redistributed with CPython 3.12.10. Microsoft's terms govern, separately from this project's licences: [Visual Studio 2022 redistribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution), Visual C++ Runtime Files. Redistributors must satisfy Microsoft's conditions; the "licensed Visual Studio user" condition is a separate Microsoft-terms question. The reproduced CPython conditions contain no reverse-engineering, decompilation or disassembly restriction. |
| Dynamic libusb 1.0.27 (`libusb-1.0.dll`) | LGPL-2.1-or-later | `licenses/windows/libusb-COPYING.txt` | Notice SHA256 `5df07007198989c622f5d41de8d703e7bef3d0e79d62e24332ee739a452af62a`; source `libusb/libusb.h` grants 2.1-or-later. Use the official VS2022/MS64 DLL pinned in `windows-libusb.json`, with source and signature. |
| MSYS2 runtime 3.6.10 (`msys-2.0.dll`, both locations) | LGPL-3.0-or-later with Cygwin linking exception; newlib/BSD/per-file terms | Five runtime notices | Preserve `windows/msys2-runtime-{CYGWIN_LICENSE,COPYING.LIB,COPYING,COPYING.NEWLIB,BSD-NOTICES}.txt` and source/signature sidecars. Package version is pinned to 3.6.10-6. |

### Exact-install notice provenance and limits

Authoritative inputs: `LICENSE.txt`
and `tcl_tk8.6_license.terms`, supplied from the exact CPython 3.12.10 x64
installation used for this package. The Windows LICENSE is 36874 bytes and
702 lines, SHA256
`e502c6b880ff58d614901495a9009c136539cd0b1e2a2abb8fc00b934c203419`.
It is copied byte for byte, including mixed newline styles, as
`licenses/CPython-3.12.10-Windows-LICENSE.txt`. Its sections include Python base
terms (1-279), Microsoft conditions (283-320), bzip2 (326-362), libffi (366-386),
Apache-2.0 (389-564), and two Tcl/Tk-style notices (566-605 and 607-646).
It contains no zlib licence text. The Tcl/Tk pointer at 693-696 does not remove
or replace those earlier complete notices.

Keep `licenses/PSF-Python.txt` alongside the Windows notice: its 13936 bytes,
SHA256 `3b2f81fe21d181c499c59a256c8e1968455d6689d269aa85373bfb6af41da3bf`,
match the Windows notice's opening base terms after CRLF-to-LF normalization.
It remains the existing platform-neutral Python reference; the full Windows
notice is required separately and is not satisfied by that base file.

The supplied Tcl 8.6.15 source
`license.terms` equals Windows LICENSE lines 566-605 after CRLF-to-LF
normalization. No extra base file is required. This differs from Tk's text
(Apple Inc. and DFARS 252.227-7013 instead of Tcl's 252.227-7014); retain both.
The Tcl source tarball SHA256 is
`861e159753f2e2fbd6ec1484103715b0be56be3357522b858d3cbb5f893ffef1`.
This source supplies licence terms and upstream object lists.

The local installed `zlib1g-dev` version is
`1:1.3.dfsg+really1.3.1-1ubuntu3.1`. Its `/usr/include/zlib.h:2-4` identifies
upstream 1.3.1 and Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler.
`/usr/share/doc/zlib1g-dev/README.gz` decompresses to the complete upstream
README, copied without edits as `licenses/zlib-1.3.1-LICENSE-README.txt`:
5317 bytes, SHA256
`d106dcdb2a0b3087cb3a2380b9f00a0bbadc7eb835cd80408f94a2d78d69727d`.
It identifies 1.3.1 at line 3 and includes the full copyright/permission text
at lines 84-106. This supplies version-matched upstream terms; the DLL's older
copyright metadata does not identify its exact source.

The spec explicitly includes the Windows, incorporated-software, Tcl and
wheel notices. The inventory requires canonical bytes for the Python DLL,
12 stdlib .pyd files, runtime DLLs and any Crypto/**/*.pyd, at both notice
locations. Notice retention does not replace source correspondence or
Microsoft's redistribution conditions. The Tcl/Tk and PyCryptodome source
mappings do not supply exact DLL or wheel producer build records. The official
libusb DLL's matching-version source and project files do not establish exact
producer build inputs or binary reproducibility.

### CPython incorporated components

Exact-install Doc-license.html SHA256:
`4633449165a1a6f16f5e2497458ecbf94f1f9a20f08606553ee5d3d948c1a12a`.
The whole incorporated-software section (HTML lines 531-1315) is shipped as
`licenses/CPython-3.12.10-Incorporated-Software.txt`, SHA256
`345c1a8708efb0ea43e34dc83a8b0bd05dce82decb4f5d8de8dee4932f7367d6`.
The document itself calls that upstream list incomplete; do not claim it is a
universal audit of CPython. Shipping the whole section preserves a conservative
superset rather than omitting conditional notices. The conversion method is
recorded in `licenses/README.md`.

| Native(s), inventory lines | Incorporated component / evidence and limits |
|---|---|
| python312.dll (7) | Core dtoa/strtod and SipHash code: Doc-license.html:823-876 explicitly identifies Python/pyhash.c and Python/dtoa.c. cfuhash underlies tracemalloc (:1154-1189); Mersenne Twister underlies _random (:537-581). These notices are preserved. _random is not a separate .pyd in the inventory. |
| _decimal.pyd (17) | libmpdec: exact-install documentation :1194-1221 identifies _decimal and the included library unless configured with a system copy. No separate mpdec DLL is in the native inventory. Its complete Stefan Krah notice is shipped; static linkage is inferred from this mapping and the absence of a separate DLL. |
| _bz2.pyd (15) | bzip2: exact-install Windows LICENSE:326-362 supplies its full notice. No separate bzip2 DLL is listed; static linkage is inferred. |
| _lzma.pyd (19) | XZ/liblzma: no separate liblzma DLL listed. Existing XZ-COPYING.txt and 0BSD-xz.txt preserved and gated for this extension. The HTML section has no XZ notice. |
| _ctypes.pyd (16), libffi-8.dll (4) | libffi: Doc-license.html:1095-1120 maps _ctypes to libffi. Retain the newer exact-install Windows libffi notice (:366-386), as well as the older copyright wording in the unedited HTML section. |
| _hashlib.pyd (18), libcrypto-3.dll (3) | OpenSSL: Doc-license.html:880-1063 and DLL metadata identify the backend; Apache-2.0 is preserved. _ssl.pyd and libssl are absent. |
| _socket.pyd (21) | Sockets/WIDE notice: Doc-license.html:586-615. Preserve the notice with the incorporated-software section. |
| _tkinter.pyd (22), tcl86t.dll/tk86t.dll (9-10) | Tcl/Tk backend; base terms plus supplied-source incorporated terms below. |
| select.pyd, unicodedata.pyd, _queue.pyd, _uuid.pyd, _wmi.pyd (8,11,20,23,24) | No additional component identified for these Windows extensions by the supplied notices. select's kqueue notice concerns BSD platforms; preserve it as part of the whole section without claiming Windows kqueue support. |
| Not listed: pyexpat.pyd, _elementtree.pyd, audioop.pyd | Their notices and asyncio/uvloop terms remain in the complete incorporated-software section. |

The documentation maps components to their notices. It does not supply
an exact static object map for the Windows CPython binaries.

### Tcl incorporated terms

Source paths below are relative to the supplied Tcl 8.6.15 tree. Both Windows
makefiles link regex and libtommath into the Tcl library; Tcl sources cannot
establish the objects linked into tk86t.dll.

| Component / build evidence | Separate terms and shipped text |
|---|---|
| generic/regcomp.c, regerror.c, regexec.c, regfree.c; win/makefile.vc:218-223 and Makefile.in:231-235. regcomp/regexec include regc_*/rege_dfa files; regguts.h/regex.h supply headers | Henry Spencer's notices permit binaries without a mandatory documentation credit; source copies must retain the notice and describe modifications. The complete regcomp.c:1-31 comment is preserved verbatim in Tcl-8.6.15-regex-NOTICE.txt as an attribution sidecar. Other regex source comments retain their respective wording in the source; this is not a substitute for distributing modified regex source. |
| libtommath/bn_*.c, headers, Tcl adaptations; win/makefile.vc:323-388,412 and Makefile.in:319-385,427 | libtommath/LICENSE dedicates code to the public domain (Unlicense), with no binary-notice condition. Tcl adaptations point to the Tcl base licence. Complete Tcl-8.6.15-libtommath-LICENSE.txt preserved for clarity. |
| generic/tclDate.c generated parser; win/makefile.vc:239 and Makefile.in:252 | tclDate.c:1-32 contains GPL-3.0-or-later Bison skeleton terms with the larger-work exception. Tcl's date grammar uses base terms. Tcl is not a parser generator using that skeleton; preserve the complete unedited opening notices as Tcl-8.6.15-Bison-NOTICE.txt, alongside already shipped GPL-3.0.txt. |
| compat/zlib; win/makefile.vc:306-321,548-561 and Makefile.in:414-427,522 | VC dynamic builds link zdll.lib and copy architecture-specific zlib1.dll; static builds compile the listed zlib objects. Makefile.in selects @ZLIB_OBJS@ at configure time. compat/zlib/LICENSE requires notice retention in source, not an extra binary notice. Preserve it verbatim as Tcl-8.6.15-zlib-LICENSE.txt; this does not prove the 1.3.1 DLL came from that tree. |
| generic/tclStrToD.c:10-13, tclUtf.c:18 includes tclUniData.c | Base Tcl terms, not David M. Gay's dtoa licence. The generated Unicode table has a Scriptics copyright header; no separate terms identified there. |
| compat/fake-rfc2553.[ch], dlfcn.h, unistd.h | Separate WIDE/Damien Miller, Helios and UC notices exist, but no objects for these files are in either supplied Windows list and no includes of these headers were found in the Windows port. Do not claim them compiled into the DLL. |

### Tk incorporated terms

Paths here are relative to the Tk 8.6.15 source tree.
Local tarball SHA256:
`550969f35379f952b3020f3ab7b9dd5bfd11c1ef7c9b7c6a75f5c49aca793fec`.
Root, compat/, xlib/ and xlib/X11/ license.terms are byte-identical, and equal
the exact-install Tk sidecar after CRLF-to-LF normalization.

Both Windows makefiles select the same 142 C object names. The VC DLL target
links TKOBJS at win/makefile.vc:425-429; the other target uses TK_OBJS plus
TK_RES at win/Makefile.in:654-656. This identifies upstream build inputs,
not the exact producer's object map, patches or compiler switches. Preserve a
conservative header/resource superset in Tk-8.6.15-Incorporated-NOTICES.txt;
the extraction and hash are in licenses/README.md.

| Component / build evidence | Terms and notice handling |
|---|---|
| xlib/xcolors.c, xdraw.c, xgc.c, ximage.c, xutil.c; win/makefile.vc:139-150, Makefile.in:244-255 | Base Tk grants; their complete copyright comments are preserved. These are Tk's Windows X emulation, not a separate linked X server/library. |
| xlib/X11 headers; generic/tk.h:99, win/tkWinPort.h:71-75; Xlib.h:44,52 and Xutil.h:54 continue includes | Open Group/DEC permission notices require retention in copies and supporting documentation (e.g. Xlib.h:3-23, Xutil.h:4-45). Ship complete original comment blocks, including those in X.h, Xfuncproto.h, keysym.h, keysymdef.h and cursorfont.h. Superset also retains the optional Apollo/HP, Sun/Oracle, DEC and HP keysym header grants. XF86keysym.h used by win/tkWinKey.c:14 has no separate grant in the supplied header; no invented licence is assigned. |
| compat/stdlib.h and unistd.h | No compat C objects or compat include directory are selected in either Windows list. win/tkWinPort.h:27 uses the compiler's stdlib.h. compat/unistd.h:6-12 has a UC notice requiring retention; preserved conservatively without claiming it compiled on Windows. compat/license.terms equals the base Tk terms. |
| generic/ttk and win/ttkWin*.c; win/makefile.vc:257-290, Makefile.in:359-392 | Joe English, Pat Thoyts and other per-file credits, often "Freely redistributable", or references to base terms; no separate licence file in generic/ttk. Preserve all notice-bearing comments, including ttkEntry.c:1-9's copied Tk/UC/Sun/Ajuba/ActiveState credits. The source superset also retains optional square-widget and stub-header notices. |
| generic/tkImgGIF.c; win/makefile.vc:212, Makefile.in:317 | tkImgGIF.c:1-31 contains the base credits plus David Koblas's giftoppm grant. It requires copyright/permission notice in supporting documentation. Complete header shipped verbatim. |
| generic/tkImgPNG.c, tkImgPPM.c and photo/image support; win/makefile.vc:211-217, Makefile.in:316-322 | Base Tk terms. PNG uses Tcl_ZlibStreamInit at tkImgPNG.c:336 and Tcl_ZlibCRC32 at :508, not bundled libpng. Neither Windows object list links libpng or libjpeg; no bundled image-library directory was found in this source. GIF/giftoppm is the identified image-code exception. Existing Tcl and standalone zlib notices remain. |
| win/rc/tk.rc and tk_base.rc; win/makefile.vc:85,254; library/*.tcl, library/ttk/*.tcl; Makefile.in:527-538 | Resource and runtime script base credits preserved; demos excluded from runtime script extraction. library/icons.tcl:3-11 declares Tango Desktop icons under CC BY-SA 3.0, with project/licence URLs and Pat Thoyts credit. Its complete header is preserved unchanged in the sidecar. |
| library/images/*; win/makefile.vc:689-690, Makefile.in:538-543 | Separate data assets, not image libraries compiled into the DLL. Excluded from Windows builds; GUI PNGs come from diskos_installer/assets/. The complete images/README logo-use permission remains in the unchanged conservative notice sidecar. |

### Collected Tcl/Tk script and image groups

`windows-tcltk-files-4b2687f0.txt` is the supplied
path|size native-data inventory: 922 entries (one _tkinter.pyd,
832 _tcl_data files, 89 _tk_data files), 41001 bytes, SHA256
`7982d3042462b71d35be56358ceae76294e2e9db25b2604b6117c1e259f6c071`.
There are no tcl8 paths or .tm files in this manifest. Do not infer that
msgcat, modern http or other Tcl modules were collected from a stock Tcl
installation layout. This assessment is of the supplied complete manifest.

Map _tcl_data to Tcl library/, except opt0.4/ maps to library/opt/
(win/makefile.vc:989-991). Map _tk_data to Tk library/. Of 921 data files,
920 map to supplied source files: 13 have the source's raw size and 907 have
its size after LF-to-CRLF conversion. All mapped sizes fit one of those forms.
Tk's pkgIndex.tcl (manifest line 856, 376 bytes) is generated by
win/makefile.vc:662-672; its collected bytes were not supplied. Size equality
is not byte identity. The manifest alone does not establish retained headers
or unmodified icons. The icons.tcl entry has
SHA256 `1264940e62b9a37967925418e9d0dc0befd369e8c181b9bab3d1607e3cc14b85`,
matching Tk 8.6.15 library/icons.tcl after LF-to-CRLF conversion. Its content,
embedded icons and complete attribution header are unmodified. Preserve the collected scripts and their notice headers.

| Collected group (manifest lines) | Separate terms / notice handling |
|---|---|
| Tcl root scripts and tclIndex: 10 (2-11) | Tcl library/license.terms:1-10 applies to all associated files unless individually disclaimed, requires existing copyrights retained in copies and the base notice verbatim in distributions. Headers in auto, clock, history, init, package, parray, safe and word explicitly use those terms. tm.tcl and tclIndex have no separate grant. The complete base text already ships at CPython Windows notice lines 566-605; no additional licence sidecar is needed. Preserve script headers during collection. |
| Tcl http1.0: 2 (94-95); opt0.4: 2 (223-224) | library/http1.0/{http,pkgIndex}.tcl and library/opt/{optparse,pkgIndex}.tcl have no separate licence exception. Base Tcl terms apply; no additional notice beyond the base grant and retained headers. |
| Tcl encoding tables: 82 (12-93) | All are library/encoding/*.enc. These supplied data tables have format/name comments and mapping values, no separate grant, copyright, licence exception or accompanying encoding licence file. Under library/license.terms:1-10 they use the already shipped Tcl base notice. No separate encoding notice was identified; do not infer a Unicode-data licence from hexadecimal values alone. |
| Tcl message catalogues: 127 (96-222) | library/msgs/*.msg contains locale/calendar data; no separate copyright/grant/exception found in these files. Tcl base terms apply. This is message data, not msgcat implementation code. No additional licence sidecar needed. |
| Tcl timezone files: 609 (225-833) | Generated data/links, not the zic executable. library/tzdata/America/New_York:1 identifies tools/tclZIC.tcl; that generator's lines 5-7 identify Olson/IANA data and 28-30 use Tcl base terms. changes:9361 records tzdata2024b. Local /usr/share/doc/tzdata/copyright:6-9 states the IANA database is public domain. No extra data-notice requirement identified; Tcl-generated wrappers use the base notice. The local copyright does not identify the exact generator input bytes. No ICU data or Unicode terms inferred from Debian's separate debian/icu stanza. |
| Tcl tcl8/modules, including msgcat: 0 | No tcl8 or .tm entry. Tcl win/makefile.vc:992-1003 installs modern http, msgcat, tcltest and platform as modules outside the library directory. library/msgcat/msgcat.tcl:7-13 explicitly uses Tcl base terms, not a separate msgcat licence. No separate notice is required for an absent file; this manifest does not establish its presence elsewhere in the frozen Python archive. |
| Tk root scripts, indices and base licence: 32, excluding icons/mkpsenc below (within 834-867) | Base Tk terms apply, with upstream per-file credits already preserved in Tk-8.6.15-Incorporated-NOTICES.txt. Generated pkgIndex uses the makefile's base terms; the manifest records its size. No further licence exception identified in the other root scripts/indices. |
| Tk ttk scripts: 24 (899-922) | Base Tk terms and per-file credits; entry.tcl:3-11 credits UC/Sun/Joe English and scale.tcl:1 credits Pat Thoyts. Supplied runtime headers are already in the native/script sidecar. No separate ttk grant needed. |
| Tk message catalogues: 18 (881-898) | library/msgs/*.msg contains translations with no separate copyright/grant/exception. Base Tk notice already ships; no additional licence sidecar needed. |
| Tk icons.tcl: 1 (845, 11037 bytes) | library/icons.tcl:3-11 declares Tango Desktop project icons under CC BY-SA 3.0, credits Pat Thoyts, and supplies project/licence URLs. These are embedded warning/error/information/question PNGs, not the separate images/ GIF logos. Preserve that full header (already in the sidecar) and the attribution/URLs in NOTICE.md; ship CC-BY-SA-3.0-Tango.txt. Sections 4(a)-(c) require licence copy or URI, retained notices, applicable adaptation terms and supplied author/title/URI credits. Section 4(a) does not place the whole collection under CC BY-SA. The collected hash matches the source after newline conversion and the complete Tk header is retained. No later public-domain relicensing is assumed. |
| Tk mkpsenc.tcl: inventory 1 (850, 30840 bytes); packaged 0 | Excluded: only needed by canvas PostScript, which the GUI never calls. The Windows inventory requires its absence. The shipped Tk incorporated native/script notice sidecar is retained unchanged. |
| Tk images/: inventory 13 (868-880); packaged 0 | Excluded in full: ten GIFs (four logo, five pwrdLogo, tai-ku), two EPS logos and README. GUI PNGs are loaded from diskos_installer/assets/, independently of this subtree. The conservative native/script notice sidecar is retained unchanged. |
| Tk prolog.ps: 0 | No collected prolog.ps and no library/prolog.ps in the supplied Tk tree. mkpsenc is the PostScript prolog generator and is excluded from new Windows packages. |

The spec excludes 14 Tk data files from this inventory, leaving 75 of its
89 Tk files. Keep the complete Tango legal code and icons.tcl attribution
header. The script and embedded icons are unmodified; no later public-domain
relicensing is assumed.

### Windows Tk load paths and exclusion decision

Tk references below are relative to the Tk 8.6.15 source tree;
PyInstaller references are relative to its 6.16.0 source tree.

| File/group | Reachable load path and packaging decision |
|---|---|
| tk.tcl | generic/tkWindow.c:3425-3440 uses tcl_findLibrary with TK_LIBRARY. PyInstaller hooks/rthooks/pyi_rth__tkinter.py:19-35 sets it to _MEIPASS/_tk_data. Keep. |
| Startup class bindings | library/tk.tcl:502-517 unconditionally sources icons, button, entry, listbox, menu, panedwindow, scale, scrlbar, spinbox and text. This includes Windows. Keep all, especially icons.tcl; removing it would fail tk.Tk() before the GUI opens. |
| ttk/ and msgs/ | tk.tcl:41 loads available message catalogues, :57-61 adds Tk/ttk to auto_path, :700-702 sources ttk/ttk.tcl. ttk/ttk.tcl:15-17,115-126 loads fonts, cursors, utils and widget bindings; :136-159 loads defaults and available themes (Windows: classic, alt, clam, winnative, xp/vista). GUI selects clam. Keep the whole directory and catalogues. |
| icons.tcl | Direct startup source, not auto_load/tclIndex. It creates four embedded PNG photo images (icons.tcl:15 onward); they do not read images/. Also used by bgerror.tcl:196 and non-native msgbox.tcl:302-311. Keep unchanged with Tango notices. |
| mkpsenc.tcl | tclIndex:123 only registers ::tk::ensure_psenc_is_loaded. Tcl library/init.tcl:426-466,479-517 loads index entries on command demand; registration does not source every indexed script. generic/tkCanvPs.c:196-200 invokes the command for canvas PostScript; mkpsenc.tcl:35-36 holds Adobe glyph data and :1487 defines the command. The installer uses Canvas for scrolling and PhotoImage for PNG assets; it does not request Canvas PostScript. Exclude only this root script. Keep tclIndex unchanged; this unused command will be unavailable in the frozen app. |
| msgbox.tcl and dialog.tcl | tclIndex:124 autoloads ::tk::MessageBox; :64 autoloads tk_dialog. generic/tkWindow.c:188-190 registers Windows native file/message commands; win/tkWinDialog.c:956-962,1677,2785,2949 implements GetFileName/GetOpenFileNameW and MessageBoxW. GUI message boxes and file chooser use these native commands. Keep the fallback scripts anyway; no separate licence blocker. |
| ttk::dialog | No such procedure or ttk/dialog.tcl in this 8.6.15 tree. ttk/ttk.tcl does not source a dialog implementation; ChangeLog.2007:1556 records its omission. The GUI confirmation is its own Toplevel with ttk widgets, not ttk::dialog or tk_dialog. |
| images/ | The installer loads its own PNG assets from diskos_installer/assets/. Demo image/canvas consumers are excluded by PyInstaller utils/hooks/tcl_tk.py:216-220. Logo GIFs, EPS procsets and README are independent of embedded icons and native Windows dialog resources. Exclude this exact subtree, including future entries. |

The installer uses Tk and ttk widgets, fonts, styles, a scrolling Canvas,
PhotoImage PNG loading and drawn control images. `gui_theme.py` loads display,
wordmark and Disc PNGs from `diskos_installer/assets/` and uses Tk image-copy
and scaling calls. These paths do not use Tk's images/ logos or mkpsenc.tcl.

The file chooser is `gui.py:_pick_firmware`; confirmations use a custom Toplevel
in `_show_confirm_dialog`. Validation errors appear inline through `_fail`.
`_on_close` uses native warnings for driver setup and flashing, and an
OK/Cancel question for other running operations. Startup errors use
`gui_entry.py:_show_error` (user32.MessageBoxW); direct/CLI GUI startup also
has a Tk error-dialog fallback in `gui.py:main`.

The _tkinter hook adds tcltk_info.data_files as full DATA TOC entries
(hooks/hook-_tkinter.py:15-27); utils/hooks/tcl_tk.py:216-220,235-259
recursively collects _tk_data, including the unused assets. The spec filters
only Windows Analysis.datas destinations _tk_data/mkpsenc.tcl and
_tk_data/images/**, then refuses survivors in datas/binaries and COLLECT.toc.
Follow [windows-qualification.md](windows-qualification.md) for the package dialog procedure.

### Locked-wheel comparisons

Both supplied wheel hashes equal windows-build.lock pins:
pyusb SHA256 `bf9b754557af4717fe80c2b07cc2b923a9151f5c08d17bdb5345dac09d6a0430`;
pycryptodome SHA256 `c75b52aacc6c0c260f204cbdd834f76edc9fb0d8e0da9fbf8352ef58202564e2`.
PyUSB's dist-info/LICENSE equals BSD-3-Clause-pyusb.txt byte for byte (including
upstream Unicode). PyCryptodome's dist-info/LICENSE.rst differs from the existing
pycryptodome-LICENSE.txt only by CRLF versus LF; retain the old file and ship
PyCryptodome-3.23.0-Windows-LICENSE.rst byte for byte. Its notice SHA256 is
`60b8958a9ef9b7ec512087b725555372175ed2b02b969f8725b8534fde48acdd`.

The Windows wheel has one named licence file, no separate libtom or Keccak
licence/notice file and no C source. The presence of _keccak.pyd does not prove
its source licensing lineage. Runtime Python modules do carry per-author
copyright/BSD notices. Preserve their 16 distinct opening comment blocks,
plus _mode_ocb.py's opening docstring (including its patent/licence statement),
verbatim in PyCryptodome-3.23.0-Windows-NOTICES.txt; extraction is documented in
licenses/README.md. This reports what the selected wheel contains; it does not
assert the historical OCB patent statement is current or establish a separate
patent grant. Matching-version sdist native-source terms are below.

### PyCryptodome native source terms

Source distribution: `pycryptodome-3.23.0.tar.gz` (PyCryptodome 3.23.0),
SHA256 `447700a657182d60338bab09fdb27518f8856aecd80ae4c6bdddb67ff5da44ef`.
Member paths below are relative to pycryptodome-3.23.0/ in that archive.
setup.py:283-477 declares 42 extensions, exactly the 42 names in the locked
Windows wheel and the native inventory:25-66. Parse declarations
without executing them, then follow literal local includes and both conditional
architecture branches: 90 source/header files, 26 distinct notice comments.
The complete comments are preserved in PyCryptodome-3.23.0-Native-NOTICES.txt.
This mapping comes from the source declarations and include closure.

| Native modules / source evidence | Separate terms and notices |
|---|---|
| Cipher/_raw_des, _raw_des3; setup.py:373-379; src/DES.c:42 includes libtom/tomcrypt_des.c; DES3.c:26 includes DES.c | LibTomCrypt 1.17 headers (src/libtom/tomcrypt.h:19-20); source header says free for all purposes. Doc/LEGAL/copy/LICENSE.libtom explicitly dedicates LibTomCrypt to the public domain; complete member copied to PyCryptodome-3.23.0-libtom-LICENSE.txt. DES wrapper dedication and LibTomCrypt comments also preserved. No binary attribution requirement in these grants. |
| Hash/_keccak; setup.py:321-324; src/keccak.c:1-27 | Fabrizio Tarizzo's public-domain dedication with unconditional fallback permission. No separate mandatory binary notice; complete header preserved. No claim that this is a different third-party Keccak implementation. |
| Hash/_BLAKE2b, _BLAKE2s; setup.py:325-332; src/blake2b.c:67 and blake2s.c:67 include blake2.c | All three headers :1-30 give Copyright (c) 2014 Legrandin and BSD-2-Clause terms requiring binary notice reproduction. Complete distinct comment shipped. No separate BLAKE2 reference-library licence appears in these inputs. |
| PublicKey/_ec_ws, _curve448, _ed448; Math/_modexp; setup.py:443-474 | mont.c is compiled directly or via mont1/2/3.c:6. mont.c:60 includes modexp_utils.c, whose :5 includes siphash.c. siphash.c:1-16 names Jean-Philippe Aumasson and Daniel J. Bernstein and CC0 1.0; header and complete CC0 dedication shipped in CC0-1.0-SipHash.txt. Existing CPython SipHash notice names different code and does not replace this one. |
| Cipher/_raw_aes; setup.py:349-352; src/AES.c:1-25 | Rijndael code by Vincent Rijmen, Antoon Bosselaers and Paulo Barreto, expressly public domain with disclaimer; complete header retained, no mandatory binary attribution condition. |
| Cipher/_ARC4, _raw_cast, _raw_des, _raw_des3, _Salsa20; Hash/_MD2, _MD4, _RIPEMD160, _SHA224, _SHA256, _SHA384, _SHA512, _ghash_portable | The corresponding src/ files have public-domain/fallback grants; credits include Kuchling, Wim Lewis, Litzenberger, Lorenz Quack, and Fabrizio Tarizzo's Salsa20 derived from Bernstein. Complete distinct comments preserved. Salsa20's include_dirs mentions libtom but its C source only includes common.h/endianess.h (:28-29); no LibTomCrypt implementation is inferred there. |
| Remaining 21 extensions: Cipher/_raw_aesni, _raw_arc2, _raw_blowfish, _raw_eksblowfish, _chacha20, _pkcs1_decode, _raw_ecb, _raw_cbc, _raw_cfb, _raw_ofb, _raw_ctr, _raw_ocb; Hash/_MD5, _SHA1, _ghash_clmul, _poly1305; PublicKey/_curve25519, _ed25519; Protocol/_scrypt; Util/_cpuid_c, _strxor | Corresponding src/ implementations, shared common.h/endianess.h, block_common.c and arithmetic code have Legrandin/Helder Eijs BSD-2-Clause notices. All distinct copyrights and terms retained, including 2021 pkcs1_decode, 2022 Edwards and 2024 curve credits. _raw_eksblowfish includes blowfish.c (:7); shared constants/tables add no separate terms identified in the inspected closure. |

The sdist root LICENSE.rst equals the exact wheel's text after newline
normalization. The native comments, LibTomCrypt member and CC0 dedication
are explicitly allowlisted, hash-pinned and required for any Crypto/**/*.pyd.
Preserve OCB's historical wheel statement without inferring a current patent
status or a new patent grant.

### Pinned Windows libusb input and source evidence

The `build/windows-libusb.json` pins the official release asset,
its selected member, the DLL size/hash and the existing source size/hash:

| Input | SHA256 / size |
|---|---|
| `libusb-1.0.27.7z` | `19835e290f46fab6bd8ce4be6ab7dc5209f1c04bad177065df485e51dc4118c8` |
| `VS2022/MS64/dll/libusb-1.0.dll` | `d4e5db4fad8bef7201dc5a4a71ca997e6dccc9da25e8a988d4be63b13a02208d`, 157184 bytes |
| Existing `corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2` | `ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575`, 643680 bytes |

Both libusb candidates are PE32+ x64 with no delay imports (from the binaries). VS2022/MS64 imports KERNEL32.dll,
VCRUNTIME140.dll and UCRT API sets heap/stdio/convert/environment/string/runtime
(`api-ms-win-crt-*-l1-1-0.dll`). VCRUNTIME140.dll is already shipped for CPython
(inventory line 12); UCRT APIs are already used by usbboot's import
allowlist in `build-windows-tools.sh`. MinGW64 imports only KERNEL32.dll,
msvcrt.dll and USER32.dll; SHA256
`3f050dee9fbf995bfbed2f4969131877fcd1b59bfd3afd2a767ff6844e872430`,
228945 bytes. Both satisfy the no-new-runtime-DLL requirement. VS2022/MS64
is selected to use the existing CPython VC/UCRT runtime path. Import names
come from these candidates, not from their toolchain labels.

The wrapper requires a local `-LibusbDll` and copies it into the fresh run's
`work/libusb/libusb-1.0.dll`. PyInstaller 6.16.0 appends the spec name to
`--workpath` before cleaning (`building/build_main.py:1144-1167`); the input
is a sibling of that cleaned directory. The spec verifies DLL and source
bytes, explicitly supplies the DLL as a binary at `_internal` root, and
generates a Windows-only user hook that collects no host library. User hooks
take precedence (`depend/analysis.py:60-64`). The existing USB runtime hook
still loads from `_MEIPASS`; its `glob` hidden import is retained. After
Analysis, any other libusb source path, destination, data entry, missing or
duplicate DLL is refused, including a host copy with identical bytes. The
selected input is hashed again before EXE/COLLECT. The manifest and source
signature are allowlisted sidecars. Static usbboot links the separately built libusb archive.

The shipped source archive, SHA256
`ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575`,
is byte-identical to upstream libusb-1.0.27.tar.bz2. Its shipped `.asc` uses
signing fingerprint `9C7EA94939C69C4FBC3DBFA8AA0639079EFB61B9` (2024-01-31).
The signing key is supplied in libusb's Debian 1.0.27-1 source package
(`debian/upstream/signing-key.asc`).
The selected official 7z member's size and SHA256 match the pin above.
The source's COPYING matches the shipped notice; libusb/libusb.h and all
nine examples/source/ files also match the release 7z. The source contains
the DLL project and build recipe, including v143 in msvc/build_all.ps1:1
and shared-CRT Release configuration in msvc/Configuration.DynamicLibrary.props:14-15.
The 7z contains no complete source archive; ship the source archive separately.

Build-box procedure: preserve the source archive, signature and notices
unchanged; do not substitute a host libusb DLL. Verify the 7z hash above, extract only the selected
member into a local input folder outside the repository. Prepare the native tools
and authenticated runtime source as ordered in [windows-qualification.md](windows-qualification.md).
With native x64 Python, run `build/build-windows.ps1 -Wheelhouse
<local-wheels> -LibusbDll <local-input>/libusb-1.0.dll
-NativeManifestSha256 <hash-from-retained-tools-build-log>`.
Use `-EnableWindowsFlash` only under that runsheet's clean-revision and qualification rules.
Inspect Analysis TOC and the resulting ZIP: exactly one `libusb-1.0.dll` at
`_internal/libusb-1.0.dll`, matching the pinned hash. Qualify GUI/CLI USB
enumeration and confirm no new runtime dependency on the target Windows box.

## Why the MSYS runtime is collected twice

The spec adds `vendor/windows-x86_64/msys-2.0.dll` and the vendor EXEs via
`datas` (spec `_required_native` and `datas` construction). PyInstaller 6.16.0
reclassifies PE data as binaries in `PyInstaller/building/build_main.py:732-770`,
then calls `find_binary_dependencies` at line 967. In
`PyInstaller/depend/bindepend.py:99-107` dependencies outside preserved package
paths get their basename as destination; lines 148-188 scan imports and append
those dependencies. The squashfs EXEs import MSYS (`build/build-windows-tools.sh:107`).
Thus Analysis keeps the explicitly located vendor DLL and also collects its
import dependency as `msys-2.0.dll`; COLLECT places the latter under `_internal`.
This is dependency analysis of vendor binaries, not a dedicated MSYS hook.
Source reference: PyInstaller 6.16.0; paths and line numbers above refer to that version.

The known vendor tools require the adjacent DLL, as enforced by
`diskos_installer/bundle.py:228-232`; their PATH is restricted to that directory.
The two inventory entries are 3369089 bytes each. Preserve both locations
and their notices. Before excluding either DLL, establish every collected
binary's import dependencies and whether it requires that location.

## OpenSSL runtime use (report only)

`diskos_installer/imagebuild.py:174-186` decrypts local OTA chunks with
`hashlib.pbkdf2_hmac("sha256", ...)`, then PyCryptodome AES. CPython 3.12's
`hashlib.pbkdf2_hmac` is imported from `_hashlib`, with no Python fallback
(local `/usr/lib/python3.12/hashlib.py:180-185`). Therefore `_hashlib` and its
OpenSSL `libcrypto` backend are needed for OTA decryption, in addition to the
many SHA256 integrity checks (`bundle.py:96-110`, `state.py:150`, `basegate.py:43`).
There are no ssl imports, urllib imports or HTTPS calls in `diskos_installer`.
Neither `_ssl.pyd` nor any `libssl` DLL is present in the native inventory.

## Remaining-work classification

Use the following packaging requirements with the qualification worksheet.
Notice copies, source archives and signatures must travel with their components.

| Subject | Classification / reason |
|---|---|
| Windows dependency closure | Install exact locked wheels with hashes; inventory the collected runtime. |
| PyInstaller bootloader | Preserve the complete COPYING text and Bootloader Exception. |
| CPython incorporated components | Ship both the Windows LICENSE and the whole incorporated-software section, plus the base PSF terms. |
| Tcl/Tk data | Keep base and incorporated notices, icons.tcl and Tango legal code; omit images/ and mkpsenc.tcl. |
| PyUSB/PyCryptodome | Preserve locked-wheel and native-source notice sidecars, including LibTomCrypt and SipHash terms. |
| Microsoft runtime | Microsoft's redistribution conditions govern; the DLLs are outside the project's GPL grant. |
| Dynamic libusb | Use exactly the pinned official DLL and ship matching source, signature and LGPL text. |
| MSYS runtime | Preserve both DLL locations with the complete five-file notice set and authenticated source/signature sidecars. |
| Artifact inventory | Require canonical notice bytes, exactly one pinned libusb DLL, Tk data exclusions and the GUI/CLI procedure in windows-qualification.md. |
