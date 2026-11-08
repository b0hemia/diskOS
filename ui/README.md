# diskOS UI (`mq_ui`) - native player interface for the FiiO Snowsky Disc

The on-device UI for [diskOS](https://github.com/b0hemia/diskos): a custom, round-screen music-player
interface for the **FiiO Snowsky Disc** ($80 round-display DAP; Ingenic X2000 / MIPS32, Linux 4.4).
It replaces the stock `mq_ui` while keeping the stock `mq_player` audio engine untouched, talking to
it over the device's POSIX message-queue IPC.

Built with **LVGL 9.2.2** (software renderer) on a static-musl mipsel toolchain. This produces the
`mq_ui` binary that the diskOS installer bakes into the flashed image.

> **License:** this UI is **GPL-3.0-or-later** (see [`COPYING`](COPYING)). That is a deliberate,
> different choice from the diskOS *installer/tooling*, which is MIT - the UI is the part meant to be
> forked and kept open. Bundled third-party components keep their own licenses (see [License](#license)).

## Hardware target
- **Display:** `/dev/fb0`, 360x360, 32bpp XRGB8888; partial redraws into the active page
  of a three-page framebuffer; panel mounted 180 degrees
  (default flush rotation in `fb_pan.c`; Screen Rotation can change it).
- **Touch:** `cst816t` capacitive controller on `/dev/input/event1` (multitouch evdev), calibrated
  for the 180-degree panel. Physical keys on `/dev/input/event0`.
- **IPC:** POSIX message queues `/ui` (state/metadata player->UI) and `/player` (commands UI->player);
  ASCII frames start with `TAG(4)+TOTAL_LEN(4hex)`; length includes the header.
  Most commands add `VALUE1(4hex)` and optional payload, which can be JSON.
  Some commands add another value field; `02020008` has no value field.
- **Library:** on-device SQLite `song.db` (bundled SQLite amalgamation, compiled in-process).

## Source layout
| Area | Files |
|---|---|
| Core / loop / nav | `main.c`, `screenmgr.c` / `screens.h`, `fb_pan.c`, `ipc.c` / `ipc.h`, `config.c` |
| Now Playing + art | `ui.c` / `ui.h`, `art.c` / `art.h`, `artcache.c`, `artstore.c` (shared cover cache), `artquota.h`, `artmigrate.h`, `saver.c`, `songinfo.c`, `npmenus.c` |
| Library / browse | `home.c`, `library.c`, `rowpool.c` (reused list rows), `musicdb.c`, `search.c`, `playlistview.c`, `scanner.c` |
| Text shaping | `fontft.c`, `indic.c`, `indic_faces.c`, `indic_layout.c`, `indic_rtl.c`, `indic_viewport.c` (Gurmukhi, Devanagari, Urdu, Hebrew) |
| Settings / features | `settings.c`, `eqcustom.c`, `quicksettings.c`, `modes.c`, `kbinput.c`, `colorpick.c`, `toast.c`, `apps.c`, `debug_ui.c`, `fwcaps.c` |
| Connectivity | `wifi.c`, `bt.c`, `weather.c`, `lyrics.c`, `lastfm.c` / `lastfm_ui.c` |
| Bundled third-party | `third_party/freetype-2.13.3/` (FTL), `third_party/harfbuzz-14.6.0/` (Old MIT), `third_party/SheenBidi-3.0.0/` (Apache-2.0), `sqlite3.c` (public domain), `jsmn.h` (MIT), `md5.c` (public domain), `font_*.c` (generated glyph data) |
| Dev/diagnostic tools | `fbshot.c`, and other small on-device helpers (not part of the shipped UI) |

## Build
Requires:
- A static **mipsel-linux-musl** cross toolchain built for the stock firmware's float ABI: **MIPS32r2, FP64,
  NaN2008** (GCC 11.2.0, musl 1.2.6). Build it with [musl-cross-make](https://github.com/richfelker/musl-cross-make)
  and the pinned config in [`toolchain/mcm-config.mak`](toolchain/mcm-config.mak), or just use the Docker builder
  below. Generic prebuilt mipsel toolchains (e.g. musl.cc) target the older MIPS-I / FP32 ABI: the result runs, but
  the Disc's kernel then emulates every floating-point instruction in software, which is much slower.
- A matching mipsel-linux-musl **C++** cross compiler for HarfBuzz (text shaping), given as `HB_CROSS`: the same
  recipe with [`toolchain/mcm-cxx.mak`](toolchain/mcm-cxx.mak) appended. The Docker builder includes it.
- **LVGL 9.2.2** - **vendored in `./lvgl`** (MIT). It is *lightly customized* (a merged Source Han
  Sans + Font Awesome CJK font, plus font/TJPG tweaks), so use the vendored copy to retain these
  customizations.

The embedded stock decoder also needs two external inputs, supplied by the builder:

- `STOCK_AV_HEADERS`: the FFmpeg 4.2.2 archive from
  <https://ffmpeg.org/releases/ffmpeg-4.2.2.tar.xz>, SHA-256
  `cb754255ab0ee2ea5f66f8850e1bd6ad5cac1cd855d0a2f4990fb8c668b0d29c`.
  The build checks this pin and extracts public headers only; it does not compile FFmpeg.
- `STOCK_SYSROOT`: your own extracted stock V2.57 root filesystem, containing
  `lib/ld-linux-mipsn8.so.1`, `lib/libc.so.6`, and `usr/lib/libav{format,codec,util}.so.*`.
  Stock binaries are not supplied in this source tree. The helper links against these
  stock libraries. It also requires `mipsel-linux-gnu-gcc` and `mipsel-linux-gnu-strip`
  on PATH (override with `STOCK_CC` and `STOCK_STRIP`).

From the public `ui/` directory, with its bundled `third_party/`, `tools/` and `lvgl/`:

```sh
make CROSS=/opt/x-tools/mipsel-linux-musl/bin/mipsel-linux-musl- \
  HB_CROSS=/opt/mipsel-n2008-cxx-musl-cross/bin/mipsel-linux-musl- \
  STOCK_AV_HEADERS=/path/to/ffmpeg-4.2.2.tar.xz \
  STOCK_SYSROOT=/path/to/stock-v257 DEP_OUT="$PWD/.build-deps" mq_ui
```

Keep those same overrides for subsequent builds and `make -q mq_ui`. Native
vendor dependencies are built offline using the included sources and recipes.

```sh
# toolchain on PATH as mipsel-linux-musl-gcc, LVGL at ./lvgl;
# export STOCK_AV_HEADERS and STOCK_SYSROOT before these examples:
make

# or point at your toolchain / LVGL explicitly:
make CROSS=/opt/mipsel-n2008-musl-cross/bin/mipsel-linux-musl-  LVGL=/path/to/lvgl
```

`make` produces the static `mq_ui` binary. For a persistent local setup, put your overrides in a
`config.mk` (gitignored) instead of passing them each time:

```make
CROSS := /opt/mipsel-n2008-musl-cross/bin/mipsel-linux-musl-
LVGL  := /path/to/lvgl
```

## Build in Docker (recommended)

The container builds the cross compiler from musl-cross-make at a pinned commit, with
source-download hashes from that recipe. It also builds FreeType from the in-tree source:

```sh
docker build -t diskos-ui-builder .                                  # from this ui/ directory
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" \
  -v /path/to/inputs:/inputs:ro -e STOCK_AV_HEADERS=/inputs/ffmpeg-4.2.2.tar.xz \
  -e STOCK_SYSROOT=/inputs/stock-v257 diskos-ui-builder
```

The static `mq_ui` lands in the current directory. Supply the two external inputs above
and ensure the container also has the stock helper compiler and strip tool. The `-u` flag keeps the build outputs owned by you, not root.

The first `docker build` compiles GCC twice (C, then C++ for HarfBuzz), so it takes a while; later UI builds reuse the image.

## Troubleshooting

- **`musl` / SQLite `fcntl64` errors:** check the toolchain and compile flags. The
  SQLite rule uses separate flags without `_GNU_SOURCE`. If your cross-compiler errors on
  `fcntl64`, check that it uses the pinned musl headers and libraries, or use the Docker
  builder. Preserve the dedicated SQLite rule when building by hand.
- **`Makefile:NN: *** missing separator`:** a recipe line (the command under a rule) must start with a
  real **Tab**, not spaces - an editor or a copy-paste turned the leading Tab into spaces. Re-indent
  that line with a single Tab.

## Deploy / flash
`mq_ui` is normally delivered by the diskOS **installer**, which bakes it into a rootfs image and
flashes it over the chip's mask-ROM USB mode - see the [main diskOS repo](https://github.com/b0hemia/diskos).
The installer keeps the stock `mq_player` audio engine; only the UI is replaced.

## License
- **This UI:** GPL-3.0-or-later - [`COPYING`](COPYING). (c) diskOS contributors.
- **LVGL 9.2.2** (vendored in `lvgl/`): MIT - [`licenses/LICENSE-LVGL-MIT.txt`](licenses/LICENSE-LVGL-MIT.txt).
  Enabled LVGL-bundled helpers: **TJpgDec** (JPEG decode, BSD-style (c) ChaN) and **qrcodegen**
  (QR codes, MIT (c) Project Nayuki) - texts in the repository
  [`../licenses/LICENSE-TJpgDec.txt`](../licenses/LICENSE-TJpgDec.txt) and
  [`../licenses/MIT-qrcodegen.txt`](../licenses/MIT-qrcodegen.txt).
- **FreeType 2.13.3:** FreeType License (FTL) -
  [`third_party/freetype-2.13.3/docs/FTL.TXT`](third_party/freetype-2.13.3/docs/FTL.TXT).
  This UI is based in part on the work of the FreeType Team. FreeType renders the stock
  `/usr/project/config/fonts/SourceHanSans-Regular.ttc` for CJK fallback.
- **HarfBuzz 14.6.0** (text shaping): Old MIT - [`licenses/HarfBuzz-COPYING`](licenses/HarfBuzz-COPYING).
- **SheenBidi 3.0.0** (right-to-left layout): Apache-2.0 - [`licenses/SheenBidi-Apache-2.0.txt`](licenses/SheenBidi-Apache-2.0.txt).
- **SQLite amalgamation** (`sqlite3.c/.h`): public domain.
- **jsmn** (`jsmn.h`): MIT (Serge Zaitsev).
- **md5, RFC 1321** (`md5.c`): public domain (Alexander Peslyak).
- **Embedded fonts** (generated glyph arrays), all under the SIL Open Font License 1.1:
  - **Montserrat** - Latin UI text - [`licenses/OFL-1.1-Montserrat.txt`](licenses/OFL-1.1-Montserrat.txt)
  - **Source Han Sans** - CJK glyph fallback - [`licenses/OFL-1.1-SourceHanSans.txt`](licenses/OFL-1.1-SourceHanSans.txt)
  - **Noto Sans** - Latin-extended / Greek / Cyrillic glyphs (`font_intl_14.c` .. `font_intl_20.c`) - [`licenses/OFL-1.1-NotoSans.txt`](licenses/OFL-1.1-NotoSans.txt)
  - **Font Awesome 4.7.0** - UI + weather icon glyphs (`font_icons_20.c`, `font_icons_28.c`, `font_weather16.c`) - [`licenses/OFL-1.1-FontAwesome.txt`](licenses/OFL-1.1-FontAwesome.txt)
  - **IBM Plex Mono** (SemiBold/Bold) - "something" theme text (`font_mono_*.c`); converted under the name `diskos_mono`
    because the licence reserves the name "Plex" - [`licenses/OFL-1.1-IBMPlexMono.txt`](licenses/OFL-1.1-IBMPlexMono.txt)
  - **Doto** (weight 700, square dots) - "something" theme titles and date (`font_dot_*.c`), converted as `diskos_dot` - [`licenses/OFL-1.1-Doto.txt`](licenses/OFL-1.1-Doto.txt)
  - **Inter** (SemiBold) - hi-fi theme text (`font_hifi_*.c`) - [`licenses/OFL-1.1-Inter.txt`](licenses/OFL-1.1-Inter.txt)
  - **Archivo** (expanded, SemiBold / Light) - the diskOS wordmark on the boot splash (`font_brand_*.c`) - [`licenses/OFL-1.1-Archivo.txt`](licenses/OFL-1.1-Archivo.txt)
  - **DSEG7 Classic** (Bold) - hi-fi theme clock and time counters (`font_hifi_*.c`), converted without the name (Reserved Font Name "DSEG") - [`licenses/OFL-1.1-DSEG.txt`](licenses/OFL-1.1-DSEG.txt)
  - **JetBrains Mono** (Medium / ExtraBold) - terminal theme text and clock (`font_term_*.c`) - [`licenses/OFL-1.1-JetBrainsMono.txt`](licenses/OFL-1.1-JetBrainsMono.txt)
  - **Space Grotesk** (Medium / Bold) - bauhaus theme text, titles and clock (`font_bauh_*.c`) - [`licenses/OFL-1.1-SpaceGrotesk.txt`](licenses/OFL-1.1-SpaceGrotesk.txt)
  - **IBM Plex Sans** (Medium) - blueprint theme text (`font_blue_*.c`), converted without the name (Reserved Font Name "Plex") - [`licenses/OFL-1.1-IBMPlexSans.txt`](licenses/OFL-1.1-IBMPlexSans.txt)
  - **Nunito** (Bold / ExtraBold) - stone theme text and clock (`font_stone_*.c`) - [`licenses/OFL-1.1-Nunito.txt`](licenses/OFL-1.1-Nunito.txt)
  - **Courier Prime** (Regular / Bold) - zine theme text and clock (`font_zine_*.c`) - [`licenses/OFL-1.1-CourierPrime.txt`](licenses/OFL-1.1-CourierPrime.txt)
  - **Rubik Glitch** (Regular) - zine theme titles (`font_zine_*.c`) - [`licenses/OFL-1.1-RubikGlitch.txt`](licenses/OFL-1.1-RubikGlitch.txt)
- **Embedded whole font files** (`indic_fonts.inc`, made by `tools/embed_indic.py` from `../tools/hbcanary/fonts/`, unmodified), OFL-1.1:
  - **Noto Sans Gurmukhi UI** (SemiBold) - [`licenses/OFL-Gurmukhi.txt`](licenses/OFL-Gurmukhi.txt)
  - **Noto Sans Devanagari** (SemiBold) - [`licenses/OFL-Devanagari.txt`](licenses/OFL-Devanagari.txt)
  - **Noto Naskh Arabic** (SemiBold) - [`licenses/OFL-NotoNaskhArabic.txt`](licenses/OFL-NotoNaskhArabic.txt)
  - **Noto Sans Hebrew** (SemiBold) - [`licenses/OFL-NotoSansHebrew.txt`](licenses/OFL-NotoSansHebrew.txt)

The "something" theme is an independent design; it is not affiliated with or endorsed by Nothing Technology Limited.

Bundled components retain their own licences, including LGPL-2.1-or-later for libplist.
See [the complete component index](../NOTICE.md) and [licence texts](../licenses/README.md).

## Contributing
Issues and patches welcome via the [diskOS repo](https://github.com/b0hemia/diskos). By contributing
to this directory you agree your changes are licensed GPL-3.0-or-later.
