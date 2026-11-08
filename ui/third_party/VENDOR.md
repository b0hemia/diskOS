# Cast and AirPlay vendored dependencies

VENDOR.json records upstream URLs, archive hashes and pruned paths.
SHA256SUMS pins retained files, configuration, recipes and generated files.
Retained upstream source files are unmodified. The development tree also
retains pinned audit archives; public source packages omit these archives.

Public source packages include NOTICE.md and licenses/README.md at the
package root for licences and source inventories. Google descriptor.proto
retains its complete BSD-3-Clause notice in the source header. Cast schema
attribution is in cast/openscreen-LICENSE; VENDOR.json records its URL and hash.

For an offline build, see the UI README and supply the documented stock
firmware and FFmpeg header inputs. The UI tools/build-native-deps.py runner
uses the retained recipes with explicit source and output directories.
The production UI links mbedTLS, Monocypher, libplist, SpeexDSP and FreeType;
nanopb runtime sources and dr_libs headers compile directly into the UI.

libplist is LGPL-2.1-or-later, with MIT notices in src/jsmn.c and src/time64.c.
Its libcnary source is included. Complete UI source and dependency recipes
provide a rebuild and relink path; see NOTICE.md in the public package.

stb_image_write.h v1.16 is pinned at nothings/stb@1ee679ca2ef753a528db5ba6801e1067b40481b8.
The header is unchanged. Its final licence comment is extracted verbatim
into LICENSE and copied into the distributed licence notices.
The JPEG callback writer is compiled statically in cast_art.c. NO_STDIO
disables file output and HDR; unused PNG/BMP/TGA code is removed at -Os.
AirPlay uses quality 85 and a fixed 256 KiB output allocation, with PNG
fallback. The transparent 1x1 placeholder remains PNG.

HarfBuzz 14.6.0 (Old MIT) is linked into mq_ui for opt-in Gurmukhi and
Devanagari metadata. build-harfbuzz.sh compiles the upstream amalgamation
with FreeType callbacks and all OpenType script shapers, using the existing
FreeType archive. No C++ runtime, second FreeType library or canary Lato
face is linked. FT_SRC selects the existing unpacked FreeType headers;
HB_OUT overrides the archive output directory.

The embedded OFL-1.1 font bytes come from tools/hbcanary/fonts:
NotoSansGurmukhiUI-SemiBold.ttf (55,224 bytes, SHA-256
fc1710ed7c905f061295ced040d12327191c69d5f2462f46c8655a9d41198ca6)
NotoSansDevanagari-SemiBold.ttf (245,988 bytes, SHA-256
943a11e02d0d282bed619951ab4dc9a4912e9fceb00441bf4aacd54fa57b6e63).
The UI's tools/embed_indic.py reproduces indic_fonts.inc from those pinned
hinted/ttf assets (Gurmukhi v2.004, Devanagari v2.007). Complete font notices
and HarfBuzz's COPYING are in the UI licence directory. Both scripts use
embedded SemiBold faces, independent of the installed firmware fonts.
The standalone canary retains its Regular font inputs.

SheenBidi 3.0.0 (Apache-2.0) resolves Unicode 17 paragraph levels for the
opt-in metadata provider. build-sheenbidi.sh extracts the pinned upstream
archive into SB_OUT and compiles the upstream unity unit with its optional
text-editing API and default scratch pool disabled. All engine allocations
while resolving text use the application's bounded custom allocator.

The retained Headers/Source view is unmodified upstream evidence, except that
Source/Core/ThreadFence.h is omitted from that view because its upstream
comment contains non-ASCII punctuation. The pinned archive and the offline
build retain that original header verbatim. VENDOR.json lists every omitted
file. Complete Apache and OFL notices are in the UI licenses directory.

Arabic script uses Noto Naskh Arabic SemiBold v2.021 (hinted/ttf, 265016 bytes).
Hebrew uses Noto Sans Hebrew SemiBold v3.001 (hinted/ttf, 27100 bytes).
Both are OFL-1.1 and are embedded alongside the existing Indic faces by
ui/tools/embed_indic.py. Their release URLs and full hashes are recorded in
VENDOR.json and the font SHA256SUMS. No font is obtained during a build.
