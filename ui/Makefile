# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# diskOS native UI (LVGL 9.2.2) for the FiiO Snowsky Disc.
# Requires a static mipsel-linux-musl cross toolchain and LVGL 9.2.2 (vendored in ./lvgl).
# Override paths for your environment on the command line, in the environment, or in a local
# (gitignored) config.mk, e.g.:
#   make CROSS=/opt/x-tools/mipsel-linux-musl/bin/mipsel-linux-musl-  LVGL=/path/to/lvgl
-include config.mk
APP   := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
NUI   ?= $(APP)
LVGL  ?= $(APP)/lvgl
CROSS ?= mipsel-linux-musl-
CC    := $(CROSS)gcc

# Reproducible builds: strip the build folder from __FILE__ (asserts) and debug info, so the binary does not
# change when the tree is moved or built in another place.
REPRO_FLAGS := -ffile-prefix-map=$(APP)/= -ffile-prefix-map=$(LVGL)/=lvgl/ -ffile-prefix-map=$(NUI)/third_party/=third_party/
# No fused multiply-add contraction: floating-point results stay exactly the same as the separate multiply and add
# the code is written with (the MIPS32r2 target otherwise uses madd.s/madd.d).
FP_FLAGS := -ffp-contract=off
CFLAGS := -Os -pipe -std=gnu11 -static -pthread -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE \
  -ffunction-sections -fdata-sections \
  -I$(APP) -I$(NUI) -I$(LVGL)/.. -Wall -MMD -MP $(REPRO_FLAGS) $(FP_FLAGS)
# Device-test builds only: make clean && make EXTRA_CFLAGS=-DDISKOS_TEST_OUTPUTS (shows SPDIF Output + USB Audio)
CFLAGS += $(EXTRA_CFLAGS)
FT_SOURCE ?= $(if $(wildcard $(NUI)/third_party/freetype-2.13.3/include/ft2build.h),$(NUI)/third_party/freetype-2.13.3,$(DEP_OUT)/src/freetype-2.13.3)
CFLAGS += -I$(FT_SOURCE)/include -I$(NUI)/third_party/freetype-2.13.3/include -I$(NUI)/third_party/harfbuzz-14.6.0/src
TLS_DIR := $(NUI)/third_party
DEP_OUT ?= $(abspath $(APP)/../../scratch/native-deps)
TLS_LIBS = $(addprefix $(DEP_OUT)/$(TLS_ARCH)/lib,mbedtls.a mbedx509.a mbedcrypto.a)
SENDER_LIBS = $(addprefix $(DEP_OUT)/$(TLS_ARCH)/lib,monocypher.a plist.a speexdsp.a)
TLS_ARCH ?= mipsel
TLS_FLAGS = -I$(TLS_DIR)/mbedtls-3.6.7/include -I$(TLS_DIR)/mbedconfig -DMBEDTLS_CONFIG_FILE='"airplay_mbedtls_config.h"'
CFLAGS += -I$(DEP_OUT)/src/freetype-2.13.3/include -I$(TLS_DIR)/monocypher-4.0.2/src -I$(TLS_DIR)/monocypher-4.0.2/src/optional -I$(TLS_DIR)/libplist-2.6.0/include -I$(TLS_DIR)/dr_libs-dfe8377631000664666519fdb83da193fd8037f4 -I$(TLS_DIR)/speexdsp-1.2.1/include/speex -I$(TLS_DIR)/speexconfig
CFLAGS += $(TLS_FLAGS) -DPB_VALIDATE_UTF8=1 -I$(TLS_DIR)/nanopb-0.4.9.1 -I$(TLS_DIR)/cast
# --gc-sections drops every provably-unreferenced function/data section at link (unused LVGL fonts,
# widgets, draw backends, sqlite fns) with NO behavior change - only unreachable symbols are removed.
LDFLAGS := -static -pthread -Wl,--gc-sections
SB_OUT ?= $(DEP_OUT)/sheen-mipsel
SB_LIB := $(SB_OUT)/libsheenbidi.a
CFLAGS += -I$(SB_OUT)/src/Headers
HB_LIB := $(TLS_DIR)/out/mipsel/libharfbuzz.a
LDLIBS  := $(SB_LIB) $(HB_LIB) $(DEP_OUT)/mipsel/libfreetype.a $(TLS_LIBS) $(SENDER_LIBS) -lm -lrt

LVGL_SRCS := $(sort $(shell find $(LVGL)/src -name "*.c"))   # sorted: find order depends on the disk, link order must not
APP_SRCS  := $(APP)/main.c $(APP)/remote_discovery.c $(APP)/remote_browse.c $(APP)/audio_decode.c $(APP)/audio_convert.c $(APP)/audio_feed.c $(APP)/audio_timeline.c $(APP)/airplay_crypto.c $(APP)/airplay_tlv.c $(APP)/airplay_store.c $(APP)/airplay_pair.c $(APP)/airplay_rtsp.c $(APP)/airplay_rtp.c $(APP)/airplay_raop.c $(APP)/airplay_plist.c $(APP)/airplay_ipc.c $(APP)/airplay_session.c $(APP)/airplay_remote.c $(APP)/airplay_events.c $(APP)/airplay_mrp.c $(APP)/airplay_worker.c $(APP)/airplay_queue.c $(APP)/airplay_sender.c $(APP)/airplay_pin.c $(APP)/remote_tls.c $(APP)/cast_wire.c $(APP)/cast_messages.c $(APP)/cast_session.c $(APP)/cast_discovery.c $(APP)/cast_http.c $(APP)/cast_handoff.c $(APP)/cast_bridge.c $(APP)/cast_art.c $(APP)/cast_queue.c $(APP)/cast_ctl.c $(APP)/cast_ctlw.c $(APP)/cast_format.c $(APP)/cast_ui.c $(TLS_DIR)/cast/cast_channel.pb.c $(TLS_DIR)/nanopb-0.4.9.1/pb_encode.c $(TLS_DIR)/nanopb-0.4.9.1/pb_decode.c $(TLS_DIR)/nanopb-0.4.9.1/pb_common.c $(APP)/sdio.c $(APP)/fontft.c $(APP)/indic_faces.c $(APP)/indic_layout.c $(APP)/indic_rtl.c $(APP)/indic_viewport.c $(APP)/indic.c $(APP)/font_cjk_extra_16.c $(APP)/playstate.c $(APP)/playback.c $(APP)/fb_pan.c $(APP)/ipc.c $(APP)/receiver.c $(APP)/receiver_progress.c $(APP)/ui.c $(APP)/art.c $(APP)/screenmgr.c $(APP)/home.c $(APP)/library.c $(APP)/rowpool.c $(APP)/musicdb.c $(APP)/custom_saver_db.c $(APP)/custom_saver_apply.c $(APP)/custom_saver_picker.c $(APP)/custom_picker_ui.c $(APP)/custom_preview.c $(APP)/custom_saver_overlay.c $(APP)/settings.c $(APP)/power.c $(APP)/controls.c $(APP)/config.c $(APP)/anim.c $(APP)/search.c $(APP)/saver.c $(APP)/quicksettings.c $(APP)/songinfo.c $(APP)/npmenus.c $(APP)/eqcustom.c $(APP)/apps.c $(APP)/kbinput.c $(APP)/playlistview.c $(APP)/folderbrowser.c $(APP)/books.c $(APP)/upnext.c $(APP)/datetime.c $(APP)/i18n.c $(APP)/i18n_tables.c $(APP)/theme.c $(APP)/theme_kit.c $(APP)/theme_defs.c $(APP)/theme_kits.c $(APP)/theme_validate.c $(APP)/theme_store.c $(APP)/artcache.c $(APP)/artstore.c $(APP)/colorpick.c $(APP)/albumwall.c $(APP)/debug_ui.c $(APP)/modes.c $(APP)/wifi.c $(APP)/bt.c $(APP)/bt_codec_observe.c $(APP)/bt_prepare.c $(APP)/weather.c $(APP)/lyrics.c $(APP)/netart.c $(APP)/toast.c $(APP)/lastfm.c $(APP)/lastfm_ui.c $(APP)/splash.c $(APP)/shutdown_anim.c $(APP)/md5.c $(APP)/scanner.c $(APP)/opustitle.c $(APP)/opustitle_worker.c $(APP)/system.c $(APP)/ota.c $(APP)/fwcaps.c $(APP)/font_icons_28.c $(APP)/font_icons_20.c $(APP)/font_weather16.c $(APP)/font_intl_14.c $(APP)/font_intl_16.c $(APP)/font_intl_18.c $(APP)/font_intl_20.c $(APP)/font_mono_14.c $(APP)/font_mono_16.c $(APP)/font_mono_18.c $(APP)/font_mono_20.c $(APP)/font_mono_24.c $(APP)/font_mono_bold_12.c $(APP)/font_dot_18.c $(APP)/font_dot_28.c $(APP)/font_bauh_bold_22.c $(APP)/font_bauh_bold_32.c $(APP)/font_bauh_clk_140.c $(APP)/font_bauh_clk_96.c $(APP)/font_bauh_ui_12.c $(APP)/font_bauh_ui_14.c $(APP)/font_bauh_ui_16.c $(APP)/font_bauh_ui_18.c $(APP)/font_bauh_ui_20.c $(APP)/font_bauh_ui_24.c $(APP)/font_blue_clk_56.c $(APP)/font_blue_sans_14.c $(APP)/font_blue_sans_16.c $(APP)/font_blue_sans_18.c $(APP)/font_blue_sans_20.c $(APP)/font_hifi_seg_20.c $(APP)/font_hifi_seg_64.c $(APP)/font_hifi_ui_12.c $(APP)/font_hifi_ui_14.c $(APP)/font_hifi_ui_16.c $(APP)/font_hifi_ui_18.c $(APP)/font_hifi_ui_20.c $(APP)/font_hifi_ui_24.c $(APP)/font_stone_clk_64.c $(APP)/font_stone_ui_12.c $(APP)/font_stone_ui_14.c $(APP)/font_stone_ui_16.c $(APP)/font_stone_ui_18.c $(APP)/font_stone_ui_20.c $(APP)/font_stone_ui_24.c $(APP)/font_term_clk_72.c $(APP)/font_term_ui_12.c $(APP)/font_term_ui_14.c $(APP)/font_term_ui_16.c $(APP)/font_term_ui_18.c $(APP)/font_term_ui_20.c $(APP)/font_term_ui_24.c $(APP)/font_zine_bold_16.c $(APP)/font_zine_bold_20.c $(APP)/font_zine_clk_50.c $(APP)/font_zine_glitch_26.c $(APP)/font_zine_ui_12.c $(APP)/font_zine_ui_14.c $(APP)/font_zine_ui_16.c $(APP)/font_zine_ui_18.c $(APP)/font_zine_ui_20.c $(APP)/font_zine_ui_24.c $(APP)/font_brand_disk_32.c $(APP)/font_brand_os_32.c $(APP)/bookmeta.c $(APP)/ota_policy.c $(APP)/procrun.c $(APP)/names.c $(APP)/btalias.c $(APP)/device_names.c $(APP)/device_names_ui.c $(APP)/names_reset.c $(APP)/radio_cfg.c $(APP)/radio_bt.c $(APP)/radio_wifi.c $(APP)/ui_dialog.c $(APP)/ui_wake.c $(APP)/file_observer.c $(APP)/radio_monitor.c
# bundled SQLite amalgamation (compiled in-process; replaces popen sqlite3 CLI)
SQLITE_FLAGS := -Os -pipe -static -ffunction-sections -fdata-sections -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED $(REPRO_FLAGS) $(FP_FLAGS)
OBJS := $(patsubst $(TLS_DIR)/%.o,$(DEP_OUT)/$(TLS_ARCH)/objects/%.o,$(LVGL_SRCS:.c=.o) $(APP_SRCS:.c=.o) $(APP)/sqlite3.o)

# Optional isolated objects for scratch builds of the same production target.
ifneq ($(strip $(OBJECT_OUT)),)
OBJS := $(patsubst $(NUI)/%.c,$(OBJECT_OUT)/%.o,$(LVGL_SRCS) $(APP_SRCS) $(APP)/sqlite3.c)
$(OBJECT_OUT)/%.o: $(NUI)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJECT_OUT)/ipodos-native/sqlite3.o: $(APP)/sqlite3.c
	mkdir -p $(dir $@)
	$(CC) $(SQLITE_FLAGS) -c $< -o $@
endif

# AirPlay proof build (slice P1, private): `make AIRPLAY_PROOF=1 airplay-proof` builds out/airplay-proof/mq_ui.airplay-proof and the helper.
# App objects go to out/airplay-proof/ (so main.o of the production build is never rebuilt with the proof flag); LVGL, third-party and
# SQLite objects carry no proof flag and are shared. The flag is refused for every other goal, and via EXTRA_CFLAGS for all of them.
# Nothing in packaging references these outputs.
AP_OUT := $(APP)/out/airplay-proof
AP_HELPER_ROOT ?= $(abspath $(APP)/../../tools/airplay-proof)
AP_HELPER_CROSS ?= $(abspath $(APP)/../../toolchain/build/mipsel-n2008-cxx-musl-cross/bin/mipsel-linux-musl-)
# HarfBuzz is C++: the C-only CROSS has no g++, so it builds with the same C++ toolchain as the AirPlay helper.
HB_CROSS ?= $(AP_HELPER_CROSS)
AP_ON := $(strip $(AIRPLAY_PROOF)$(DISKOS_AIRPLAY_PROOF))
ifneq ($(findstring DISKOS_AIRPLAY_PROOF,$(EXTRA_CFLAGS)),)
$(error the AirPlay proof flag goes through AIRPLAY_PROOF=1 airplay-proof only, never EXTRA_CFLAGS)
endif
# Bluetooth experiments must not populate the ordinary production object paths.
ifneq ($(findstring DISKOS_TEST_BT_PREPARE,$(EXTRA_CFLAGS)),)
ifneq ($(MAKECMDGOALS),airplay-proof)
$(error use the isolated bt-prepare target; combined experiments use AIRPLAY_PROOF=1 airplay-proof)
endif
endif
ifneq ($(AP_ON),)
ifneq ($(MAKECMDGOALS),airplay-proof)
$(error AIRPLAY_PROOF is set: only the airplay-proof target may be built with it, never the production target)
endif
else ifneq ($(filter airplay-proof,$(MAKECMDGOALS)),)
$(error airplay-proof needs AIRPLAY_PROOF=1)
endif
AP_LOCAL_SRCS := $(filter-out $(TLS_DIR)/%,$(APP_SRCS))
AP_OBJS   := $(patsubst $(APP)/%.c,$(AP_OUT)/%.o,$(AP_LOCAL_SRCS) $(APP)/airplay_probe.c)
AP_SHARED := $(filter-out $(AP_LOCAL_SRCS:.c=.o),$(OBJS))
.PHONY: airplay-proof ap-helper-force
ap-helper-force:
airplay-proof: check-theme $(AP_OUT)/mq_ui.airplay-proof $(AP_OUT)/diskos_airplay_probe
$(AP_OUT)/%.o: $(APP)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DDISKOS_AIRPLAY_PROOF -c $< -o $@
$(AP_OUT)/mq_ui.airplay-proof: $(AP_OBJS) $(AP_SHARED) $(DEP_OUT)/mipsel/libfreetype.a $(TLS_LIBS)
	$(CC) $(LDFLAGS) -o $@ $(AP_OBJS) $(AP_SHARED) $(LDLIBS)
	$(CROSS)strip $@ || true
	@ls -l $@
$(AP_OUT)/diskos_airplay_probe: ap-helper-force
	CROSS="$(AP_HELPER_CROSS)" sh "$(AP_HELPER_ROOT)/build.sh" mipsel
	mkdir -p $(dir $@)
	cp "$(AP_HELPER_ROOT)/out/mipsel-n2008-cxx/diskos_airplay_probe" "$@"
	@ls -l $@

# Standalone Bluetooth experiment: isolated objects and binary, as for the AirPlay proof.
BT_OUT := $(APP)/out/bt-prepare
BT_OBJS := $(patsubst $(APP)/%.c,$(BT_OUT)/%.o,$(AP_LOCAL_SRCS))
BT_SHARED := $(filter-out $(AP_LOCAL_SRCS:.c=.o),$(OBJS))
.PHONY: bt-prepare
bt-prepare: check-theme $(BT_OUT)/mq_ui.bt-prepare
$(BT_OUT)/%.o: $(APP)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DDISKOS_TEST_BT_PREPARE -c $< -o $@
$(BT_OUT)/mq_ui.bt-prepare: $(BT_OBJS) $(BT_SHARED) $(DEP_OUT)/mipsel/libfreetype.a $(TLS_LIBS)
	$(CC) $(LDFLAGS) -o $@ $(BT_OBJS) $(BT_SHARED) $(LDLIBS)
	$(CROSS)strip $@ || true

# Every on-screen colour is a theme role (theme.h); theme.c is the only place RGB becomes a colour. The link fails if
# any other app source (.c, or any header in the tree) builds a colour itself: an LVGL colour constructor, palette,
# mix/lighten/darken or HSV conversion, a colour macro or struct initialiser, a literal passed to the runtime-colour
# helpers, or a literal B/G/R byte written into a generated image buffer. Fonts: no direct Montserrat/icon font
# (address or copy; LV_FONT_DECLARE lines are fine). Known limits: a colour laundered through a macro, a decimal or
# computed-index pixel write, or an unrelated struct field named red/green/blue are not told apart.
.DEFAULT_GOAL := mq_ui   # plain `make` builds the UI (check-theme runs inside it)
THEME_SRCS := $(filter-out $(APP)/theme.c $(APP)/font_%,$(APP_SRCS))
THEME_HDRS  = $(filter-out $(APP)/theme.h $(APP)/sqlite3%,$(shell find $(APP) -name '*.h' -not -path '$(APP)/lvgl/*' -not -name 'stb_image.h'))   # stb_image.h: vendored decoder, not UI
THEME_COLOUR_RE := lv_color(32|16)?_(hex|hex3|make|white|black|mix|mix32|16_16_mix|lighten|darken|hsv_to_rgb|from_int|to_hsv|premultiply)[[:space:]]*\(|lv_color(32|16)_t[[:space:]]+[A-Za-z_0-9]+[[:space:]]*=[[:space:]]*\{|\(lv_color(32|16)_t\)[[:space:]]*\{|lv_palette_[a-z]+[[:space:]]*\(|LV_COLOR_MAKE[[:space:]]*\(|theme_(color|accent)_from_(rgb|hsv)[[:space:]]*\([[:space:]]*(0x|[0-9])|\(lv_color_t\)[[:space:]]*\{|lv_color_t[[:space:]]+[A-Za-z_0-9]+[[:space:]]*=[[:space:]]*\{|(\.|->)(red|green|blue)[[:space:]]*=[^=]|\[[0-2]\][[:space:]]*=[[:space:]]*0x[0-9A-Fa-f]{2}
check-theme:
	@if grep -n -E '$(THEME_COLOUR_RE)' $(THEME_SRCS) $(THEME_HDRS); then \
	  echo "check-theme: FAIL - use a theme role (TC(...)) or theme_color_from_rgb() on a runtime value instead"; exit 1; fi
	@if grep -n -E '\b(lv_font_montserrat_[0-9]+|font_icons_[0-9]+)\b' $(THEME_SRCS) $(filter-out $(APP)/lv_conf.h,$(THEME_HDRS)) | grep -v 'LV_FONT_DECLARE'; then \
	  echo "check-theme: FAIL - use a font role (TF(...)) instead"; exit 1; fi

DEPS_RUNNER := $(APP)/tools/build-native-deps.py
DEPS_ARGS := --app "$(APP)" --third-party "$(TLS_DIR)" --output "$(DEP_OUT)"

$(APP)/fontft.o $(APP)/indic_faces.o $(APP)/indic_layout.o $(OBJECT_OUT)/ipodos-native/fontft.o $(OBJECT_OUT)/ipodos-native/indic_faces.o $(OBJECT_OUT)/ipodos-native/indic_layout.o: $(DEP_OUT)/mipsel/libfreetype.a

$(DEP_OUT)/mipsel/libfreetype.a:
	CROSS="$(CROSS)" python3 $(DEPS_RUNNER) mipsel freetype $(DEPS_ARGS)

TLS_INPUTS := $(shell find $(TLS_DIR)/mbedtls-3.6.7/include $(TLS_DIR)/mbedtls-3.6.7/library $(TLS_DIR)/mbedconfig -type f) $(TLS_DIR)/build-mbedtls.sh $(TLS_DIR)/vendor-build-common.sh $(APP)/airplay_mbedtls_config.h $(APP)/tools/build-native-deps.py
$(TLS_LIBS) &: $(TLS_INPUTS)
	CROSS="$(CROSS)" python3 $(DEPS_RUNNER) $(TLS_ARCH) mbedtls $(DEPS_ARGS)
$(DEP_OUT)/$(TLS_ARCH)/libmonocypher.a:
	CROSS="$(CROSS)" python3 $(DEPS_RUNNER) $(TLS_ARCH) monocypher $(DEPS_ARGS)
$(DEP_OUT)/$(TLS_ARCH)/libplist.a:
	CROSS="$(CROSS)" python3 $(DEPS_RUNNER) $(TLS_ARCH) libplist $(DEPS_ARGS)
$(DEP_OUT)/$(TLS_ARCH)/libspeexdsp.a:
	CROSS="$(CROSS)" python3 $(DEPS_RUNNER) $(TLS_ARCH) speexdsp $(DEPS_ARGS)
.PHONY: tls-host
tls-host:
	$(MAKE) --no-print-directory TLS_ARCH=host $(subst /mipsel/,/host/,$(TLS_LIBS))

# Embedded stock-library helper (see the diskos-audiodec rules at the end) and artwork helper (diskos-artdec rules).
AUDIODEC_TOOL := $(APP)/tools/embed_audiodec.py
AUDIODEC_RAW := $(APP)/audio_stock_image.raw
AUDIODEC_OBJ := $(APP)/audio_stock_image.o
ARTDEC_RAW := $(APP)/artdec_image.raw
ARTDEC_OBJ := $(APP)/artdec_image.o
mq_ui: $(SENDER_LIBS) $(OBJS) $(AUDIODEC_OBJ) $(ARTDEC_OBJ) $(HB_LIB) $(DEP_OUT)/mipsel/libfreetype.a $(TLS_LIBS) $(SB_LIB)
	@$(MAKE) --no-print-directory check-theme
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(AUDIODEC_OBJ) $(ARTDEC_OBJ) $(LDLIBS)
	cp -f $@ $@.debug                 # unstripped twin for crash symbolication (not shipped)
	$(CROSS)strip $@ || true
	python3 -I -B $(AUDIODEC_TOOL) verify $@ diskos-audiodec $(AUDIODEC_RAW) || { rm -f $@; exit 1; }
	python3 -I -B $(AUDIODEC_TOOL) verify --kind artdec $@ diskos-artdec $(ARTDEC_RAW) || { rm -f $@; exit 1; }
	@ls -l $@
	@{ echo "artifact: $(notdir $@)"; \
	   echo "md5: $$(md5sum $@ | cut -d' ' -f1)"; \
	   echo "size: $$(stat -c%s $@)"; \
	   echo "built: $$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
	   echo "cc: $$($(CC) --version | head -1)"; \
	   echo "src_md5: $$(cat $(APP_SRCS) $(APP)/*.h 2>/dev/null | md5sum | cut -d' ' -f1)"; \
	 } > $@.manifest
	@cat $@.manifest

# Boot-selector probe (installed read-only at /opt/diskos/bin/diskos-bootprobe by the installer).
diskos-bootprobe: $(APP)/tools/diskos_bootprobe.c
	$(CC) -Os -static -Wall -Wextra -Werror $(FP_FLAGS) -o $@ $<
	$(CROSS)strip $@ || true
	@md5sum $@

# Artwork helper (installed read-only at /opt/diskos/bin/diskos-artdec): extracts + decodes covers in its own
# process so no stock ffmpeg is needed (stock V2.57's ffmpeg has no image decoders). stb_image.h is vendored.
diskos-artdec: $(APP)/tools/diskos_artdec.c $(APP)/tools/stb_image.h $(APP)/art_identity.h $(DEP_OUT)/$(TLS_ARCH)/libmbedcrypto.a
	$(CC) -O2 -static -Wall -Wextra -Werror $(REPRO_FLAGS) $(FP_FLAGS) -I$(APP) $(TLS_FLAGS) -o $@ $< $(DEP_OUT)/$(TLS_ARCH)/libmbedcrypto.a -lm
	$(CROSS)strip $@ || true
	@md5sum $@
# It also travels inside the signed mq_ui (owner decision 2026-11-09), like diskos-audiodec: an app update must bring
# the helper its art.c calls, and the image's copy may be older. The read-only .diskos_artdec section carries the
# finished ELF with a generated length and SHA-256; art.o (only) is built to write a private copy before each run,
# verify it and execute that copy, never a pathname. The post-strip check in the mq_ui recipe proves the exact bytes.
$(ARTDEC_RAW): diskos-artdec $(AUDIODEC_TOOL)
	python3 -I -B $(AUDIODEC_TOOL) image --kind artdec diskos-artdec $@
$(ARTDEC_OBJ): $(APP)/artdec_image.S $(APP)/embedded_image.inc $(ARTDEC_RAW)
	$(CC) -c -DARTDEC_IMAGE_RAW='"$(ARTDEC_RAW)"' $< -o $@
$(APP)/art.o: CFLAGS += -DART_HELPER_EMBEDDED=1
ifneq ($(strip $(OBJECT_OUT)),)
$(OBJECT_OUT)/ipodos-native/art.o: CFLAGS += -DART_HELPER_EMBEDDED=1
endif

# Standalone TTC probe and a two-link size comparison. Neither target changes mq_ui.
FT_DIR := $(NUI)/third_party
FT_OUT := $(FT_DIR)/out/mipsel
FT_INC := -I$(FT_DIR)/freetype-2.13.3/include
.PHONY: ft_cross_build ttc_probe ttc_probe_size
ft_cross_build:
	CROSS="$(CROSS)" sh $(FT_DIR)/build-freetype.sh mipsel
ttc_probe: ft_cross_build
	$(CC) -Os -pipe -std=gnu11 -static -Wall -Wextra -Werror $(FP_FLAGS) \
	  $(REPRO_FLAGS) $(FT_INC) -o $(FT_OUT)/ttc_probe \
	  $(APP)/tools/ttc_probe.c $(FT_OUT)/libfreetype.a -lm
	$(CROSS)strip $(FT_OUT)/ttc_probe

ttc_probe_size: ft_cross_build
	@for object in $(OBJS); do test -f "$$object" || { echo "missing object: $$object" >&2; exit 1; }; done
	$(CC) -Os -pipe -std=gnu11 -ffunction-sections -fdata-sections $(FP_FLAGS) \
	  $(REPRO_FLAGS) $(FT_INC) -c $(FT_DIR)/ftconfig/size_anchor.c -o $(FT_OUT)/size_anchor.o
	$(CC) $(LDFLAGS) -o $(FT_OUT)/mq_ui.base $(OBJS) $(LDLIBS)
	$(CC) $(LDFLAGS) -Wl,-u,diskos_ft_size_anchor -o $(FT_OUT)/mq_ui.freetype \
	  $(OBJS) $(FT_OUT)/size_anchor.o $(FT_OUT)/libfreetype.a $(LDLIBS)
	$(CROSS)strip $(FT_OUT)/mq_ui.base $(FT_OUT)/mq_ui.freetype
	@base=$$(stat -c%s $(FT_OUT)/mq_ui.base); with=$$(stat -c%s $(FT_OUT)/mq_ui.freetype); \
	  echo "mq_ui stripped baseline: $$base bytes"; \
	  echo "mq_ui stripped with FreeType: $$with bytes"; \
	  echo "FreeType delta: $$((with - base)) bytes"; \
	  stat -c 'libfreetype.a: %s bytes' $(FT_OUT)/libfreetype.a

.PHONY: checkpunct
checkpunct:   # device punctuation scrub of app sources (em/en dash, ellipsis, curly quotes)
	@if grep -rnP '[\x{2013}\x{2014}\x{2018}\x{2019}\x{201C}\x{201D}\x{2026}]' $(APP_SRCS) $(APP)/*.h; then \
	  echo "checkpunct: FAIL (unsupported punctuation above)"; exit 1; else echo "checkpunct: clean"; fi

$(APP)/sqlite3.o: $(APP)/sqlite3.c
	$(CC) $(SQLITE_FLAGS) -c $< -o $@

$(DEP_OUT)/$(TLS_ARCH)/objects/%.o: $(TLS_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(AP_OUT) $(BT_OUT)
	rm -f $(OBJS) $(OBJS:.o=.d) mq_ui mq_ui.debug mq_ui.manifest ipodos-ui diskos-bootprobe diskos-artdec diskos-audiodec
	rm -f $(AUDIODEC_RAW) $(AUDIODEC_RAW).part $(AUDIODEC_OBJ) $(ARTDEC_RAW) $(ARTDEC_RAW).part $(ARTDEC_OBJ)
-include $(OBJS:.o=.d)
-include $(AP_OBJS:.o=.d)
-include $(BT_OBJS:.o=.d)

# Separate dynamically linked PCM exporter; the UI retains its static musl ABI.
# Build hosts supply the read-only stock fixture and verified header archive.
STOCK_SYSROOT ?= $(abspath $(APP)/../../scratch/fixtures/v257)
AUDIODEC_OUT ?= $(DEP_OUT)/stock-audiodec
STOCK_AV_HEADERS ?= $(abspath $(APP)/../../vendor-src/ffmpeg/ffmpeg-4.2.2.tar.xz)
# Validate production inputs even when the helper is already up to date.
ifeq ($(strip $(MAKECMDGOALS)),)
STOCK_CHECK_GOALS := mq_ui
else
STOCK_CHECK_GOALS := $(filter mq_ui diskos-audiodec,$(MAKECMDGOALS))
endif
ifneq ($(STOCK_CHECK_GOALS),)
STOCK_HEADER_CHECK := $(shell STOCK_AV_HEADERS="$(STOCK_AV_HEADERS)" sh "$(TLS_DIR)/build-stock-audiodec.sh" --verify-only 2>&1 || echo "Header archive verification failed")
ifneq ($(strip $(STOCK_HEADER_CHECK)),)
$(error $(STOCK_HEADER_CHECK))
endif
endif
mq_ui: diskos-audiodec
# The helper travels inside the signed mq_ui (owner decision 2026-10-22): its finished ELF is the read-only
# .diskos_audiodec section, with a generated length and SHA-256. mq_ui never executes a pathname: before each
# start it writes a private copy, verifies it and executes that copy (audio_stock_process.h). Only the two
# objects that start the helper are built for it; the post-strip check above proves the exact bytes are carried.
$(AUDIODEC_RAW): diskos-audiodec $(AUDIODEC_TOOL)
	python3 -I -B $(AUDIODEC_TOOL) image diskos-audiodec $@
$(AUDIODEC_OBJ): $(APP)/audio_stock_image.S $(AUDIODEC_RAW) $(APP)/embedded_image.inc
	$(CC) -c -DAUDIO_STOCK_IMAGE_RAW='"$(AUDIODEC_RAW)"' $< -o $@
$(APP)/audio_decode.o $(APP)/airplay_sender.o: CFLAGS += -DAUDIO_STOCK_EMBEDDED=1
ifneq ($(strip $(OBJECT_OUT)),)
$(OBJECT_OUT)/ipodos-native/audio_decode.o $(OBJECT_OUT)/ipodos-native/airplay_sender.o: CFLAGS += -DAUDIO_STOCK_EMBEDDED=1
endif
diskos-audiodec: $(APP)/tools/diskos_audiodec.c $(APP)/tools/audio_stock_start.S $(APP)/audio_stock_wire.h $(TLS_DIR)/build-stock-audiodec.sh
	STOCK_APP="$(APP)" STOCK_SYSROOT="$(STOCK_SYSROOT)" STOCK_AV_HEADERS="$(STOCK_AV_HEADERS)" STOCK_AUDIO_OUT="$(AUDIODEC_OUT)" sh "$(TLS_DIR)/build-stock-audiodec.sh"
	cp "$(AUDIODEC_OUT)/diskos-audiodec" "$@"

$(HB_LIB): $(DEP_OUT)/mipsel/libfreetype.a $(TLS_DIR)/build-harfbuzz.sh $(wildcard $(TLS_DIR)/harfbuzz-14.6.0.tar.xz)
	CROSS="$(HB_CROSS)" FT_SRC="$(FT_SOURCE)" sh $(TLS_DIR)/build-harfbuzz.sh mipsel

include $(APP)/indic_fonts.mk

$(SB_LIB): $(TLS_DIR)/build-sheenbidi.sh $(wildcard $(TLS_DIR)/SheenBidi-3.0.0.tar.gz)
	SB_OUT="$(SB_OUT)" CROSS="$(CROSS)" sh $(TLS_DIR)/build-sheenbidi.sh mipsel
$(OBJECT_OUT)/ipodos-native/indic_rtl.o $(APP)/indic_rtl.o: $(SB_LIB)
