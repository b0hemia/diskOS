# Snowsky Disc - mq_player / mq_ui Reverse-Engineering Catalogue

Stock V2.09 binary interfaces for diskOS. Addresses and string-derived descriptions
below are from the binaries; current diskOS behavior is also described where relevant.

- Targets: stock `usr/bin/mq_player` (4126620 B) and `usr/bin/mq_ui` (4418300 B).
  MIPS32 little-endian, glibc dynamically linked (interpreter `/lib/ld-linux-mipsn8.so.1`;
  28 DT_NEEDED entries including libc.so.6, libsqlite3, and FFmpeg).
- diskOS's replacement `mq_ui` is static-musl.
- Disassembly, data, and string dumps can be regenerated with `mipsel-linux-gnu-objdump`.

---

<a id="note-corrections---full-verification-pass-2026-07"></a>

## Interface summary

- IPC frames contain a total length, including the header. Builder `mq_ui 0x43f82c`
  uses `%s%04X%04X%s`. `/player` has maxmsg=20 and msgsize=8192.
- `06b3` selects the BT codec (0=SBC, 1=AAC, 2/3/4=LDAC); `06b4` sets LDAC quality.
- `0657` switches source, `0666` switches output, and `0689` applies an EQ preset.
  Their shared teardown code does not change those meanings.
- The stock parser trusts declared LEN and uses `strlen` rather than the received byte
  count. Emit correct total lengths and never forward untrusted frames.
- Bluetooth routing needs a local-output pre-stop to avoid a player crash and MCU reboot.
  Use the sequence in [COMMAND_MAP.md](COMMAND_MAP.md).

---

## 1. How mq_player is driven - the command dispatch

mq_player is a background command server. The UI never touches audio; it mails
text commands to a POSIX message queue **`/player`**, and the player mails status
frames back on **`/ui`**.

### Wire format
- Command frame: `TAG(4 ASCII) + TOTAL_LEN(4 ASCII hex) + VALUE1(4hex) + optional payload`.
  Some command classes add VALUE2. The state request `02020008` has no value field.
- Response builder @ **0x488d64** uses format `%s%04X%04X%s`
  (tag, total length, value, then payload).

<a id="dispatch-tables-verified-structure"></a>

### Dispatch tables
Two arrays of 8-byte `{descriptor_ptr, handler_ptr}` entries:

- **Table A @ 0x7c9d30** - 131 primary entries; terminator @0x7ca148.
  Entry 0 = `{0x64f6e0, 0x411790}`.
- **Table B @ 0x7ca150** - 75 secondary/extended entries; terminator @0x7ca3a8.
  NULL handlers are declared without an implementation in that slot.

A command arrives -> tag matched against table -> handler thunk -> real handler.
Most handlers are tiny getters/setters over an in-memory settings struct.

<a id="confirmed-action-commands-tag-strings-verified-present-in-str"></a>

### Action commands
| Tag  | Action                                   |
|------|------------------------------------------|
| 0100 | play by list (list_type + index)         |
| 0103 | seek                                      |
| 0104 | favorite / unfavorite                     |
| 0201 | transport play/pause toggle             |
| 0202 | request current state                     |
| 0622 | stock scan command; does not populate song.db on V2.09 |
| 0657 | source/work-mode transition (NOT play-mode) |
| 0666 | set output route (2=BTSRC 4=SPDIF 6=local-DAC) |
| 0689 | select+APPLY EQ preset (21-way engine 0x449544; NOT DB-only) |
| 0715 | set volume                                |

See [COMMAND_MAP.md](COMMAND_MAP.md) for the dispatch table and current command meanings.

---

## 2. Audio pipeline

Decode -> process -> output.

### Decode
- **FFmpeg** stack for mp3/flac/wav/aac/aif/m4a/ape/ogg/wma.
- **libfiio_decoder** for DSD / DFF / DSF / SACD-ISO; DTS.
- libsndfile present.

### DSD / DoP
- The binary names NATIVE, DOP, and D2P modes. Roon configuration files in
  `usr/project/config/roon/` include `configure_768_dop` and `configure_384_d2p`.
  `configure_768_dop` advertises PCM rates up to 768 kHz with `dsd_mode="native"`;
  it does not establish a 768 kHz DoP carrier limit.


### Output routing (`out_dev`, tag 0666)
LOCAL_ANALOG (CS43131 headphone DAC) / BTSRC / USB_HOST / SPDIF / I2S3.
(BTSINK is NOT an out_dev route - it is an input/work-mode; see section 1 and the work-mode enum in section 7.)


<a id="volume---set_volume--0x489558-verified-prologue-0x489558"></a>

### Volume
- The mixer helper @0x45df4c sets **`ICODEC HPOUTL GAIN`** through TinyALSA
  (`mixer_get_ctl_by_name` and `mixer_ctl_set_value`). The external CS43131
  DACs use driver ioctls; this helper alone does not describe their volume path.
- On-board MCU over SPI also carries gain/filter/mute opcodes
  (SET_EQ_PARAMETER 0x100B etc.).

### Bluetooth
- bluealsa-based; source + sink; codecs sbc / aac / ldac.
- **Stock BT stack = BLUEZ, decoded from `mq_player` strings** (NOT BSA - `bsa_server`/`bt_enable_bsa*.sh`
  are dead code, wrong chip BCM4345C5, no caller; mq_player references bsa_server 0x). Full bring-up embedded
  in mq_player: `brcm_patchram_plus --enable_lpm --enable_hci --no2bytes --tosleep 200000 --baudrate 3000000
  --patchram /lib/firmware/bt_bcm/BCM4343A1_001.002.009.1026.1055.hcd /dev/ttyS0` (downloads the chip fw patch
  over UART -> creates working hci0; nothing does this at boot) -> `/usr/project/bluetoothd --noplugin=sap
  --plugin=a2dp,avrcp --mode=source` -> `bluealsa -S --device=hci0 --profile=a2dp-source --ldac-abr
  --ldac-quality=standard --codec=sbc --initial-volume=48` -> `hciconfig hci0 up/piscan/class 0x200414` ->
  `bluetoothctl agent on/default-agent/pairable on`. Sink mode = no-LPM patchram + `--mode=sink -p hfp-ag
  --codec=sbc/aac/ldac`. The missing patchram step is why a naive BT enable produces a
  poor scan.

### Gapless
- `audio_track_update_play_gapless`.

### ReplayGain
- diskOS sets ReplayGain with `0718`: 0=Off, 1=Track, 2=Album.

---

## 3. EQ - the apply path (the key unlock)

The EQ is a **software parametric biquad cascade inside mq_player**, NOT a
hardware DAC feature. The `0689` handler (`0x4961bc`) invokes the
**21-way preset engine `0x449544`**,
updates the rate caps and applies the DSP, then replies `a639`. Custom bands are set
separately via `0678` (`0x44a658`).

<a id="dsp-engine-verified-prologues-exist"></a>

### DSP engine
- Coefficient calc @ **0x448144** - uses pow/sqrt/sincos to turn
  {filterType, frequency, gain, qValue} into biquad coefficients.
- IIR cascade @ **0x447ed0** and @ **0x4483a4** - per-sample filter, 32-bit
  saturation.

<a id="storage---peq-table-sql-strings-verified-in-str"></a>

### Storage - PEQ table
Columns: `STYLE_NAME, MASTER_GAIN (REAL), PARAMS_JSON (text), STYLE_PRESET (int)`.
- PARAMS_JSON = array of bands `{filterType, frequency, gain, qValue}` (up to 10)
  with master gain in the separate `MASTER_GAIN` column.
- SQL @ ~0x257cb8 region:
  - `INSERT INTO PEQ (STYLE_NAME, MASTER_GAIN, PARAMS_JSON, STYLE_PRESET) VALUES ('%s', %.1f, '%s', %d)`
  - `UPDATE PEQ SET STYLE_NAME = ?, MASTER_GAIN = %.1f, PARAMS_JSON = ? WHERE STYLE_PRESET = ?`
  - `SELECT 1 FROM PEQ WHERE STYLE_PRESET = ? LIMIT 1`

### To apply a custom EQ from diskOS
1. Write/UPDATE the PEQ row (bands JSON + master gain) for a STYLE_PRESET.
2. Send **0689** to select that preset.
The player loads bands -> computes coeffs @0x448144 -> runs the cascade
@0x447ed0/0x4483a4 on every sample before output.

<a id="verified-wired--confirmed-in-diskos-2026-06-30"></a>

### Custom EQ in diskOS
`eqcustom.c` provides a 10-band editor with horizontal scrolling. `mdb_set_peq()` writes
user slots 11-20, and `ui_apply_eq()` sends `0689` to select a slot. Editing is enabled
on V2.09, V2.28, and V2.40. On V2.57 it is view-only: that player's PEQ loader can
reject and repair bands above half its internal processing rate.

<a id="captured-params_json-format-live-v209-2026-06-30"></a>

### PARAMS_JSON format (V2.09)
V2.09 PEQ storage format:
- **10 fixed bands**: 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 Hz, each +/-12.0 dB; plus a **master +/-12 dB** = the `MASTER_GAIN` REAL column.
- PARAMS_JSON = JSON **array of 10 objects**, e.g. one band:
  `{"filterType":0,"frequency":32,"position":0,"gain":"12.0","qValue":"0.7"}`
  - `filterType` = 0 (peaking) - **int**; `frequency` Hz - **int**; `position` 0-9 band index - **int**.
  - **`gain` and `qValue` are JSON STRINGS** (quoted: `"12.0"`, `"-12.0"`, `"0.7"`), NOT numbers. gain is one-decimal dB; default qValue `"0.7"`.
- **User slot -> STYLE_PRESET**: User 1-10 = **11-20** (User 1 = 11). Built-in presets = 0-10 (off,jazz,rock,r&b,hip-hop,pop,dance,classical,retro,sibilance-atten-1,sibilance-atten-2). Rows 160-169 exist but are unused/placeholder (`PARAMS_JSON` = literal string `"(null)"`).
- Stock writes via `UPDATE PEQ SET ... PARAMS_JSON = ? WHERE STYLE_PRESET = ?` then selects with **0689000C<preset hex>** (User 1 = `0689000C000B`).
- Other stock audio settings: Gain H/L, BT codec, SPDIF on/off, DAC digital filter (Fast/Slow LL, Fast/Slow PC, NOS, Wideband FF), DRE on/off.

---

## 4. The `/ui` frames - player -> UI

mq_ui opens the **`/ui`** mqueue (name built @0x41b388, recv thread @0x415584,
`mq_receive` wrapper @0x41b5c8). Each frame = `TAG(4) + %x extension + JSON "other"`
(parser @0x415674: `strncpy ...,4` then `sscanf %x`). Dispatch is a
`strncmp(cmd,tag,4)` chain in `ui_ctrl_response` (@0x41bfd0-0x41c098). Recv log
`[UI RECV]: %s` @0x272960.

Player->UI frames use **`a`-prefixed** tags (replies/reports) + `06d*` (OTA). The table describes the stock UI reply family. diskOS also consumes the compact
`a1` position, `a2` state, and `a714` volume frames.

| TAG | meaning | payload |
|------|---------|---------|
| a620 | version reply | version `%s` |
| a710 | max-volume reply | `maxVolume` |
| a715 | volume / UAC srate change | `currentVolume` |
| aa1b | UAC sample-rate change | rate; `usbAudio` |
| a704/a705 | wifi connect status | state; `wifi_ssid` |
| a706 | net status | `%d` |
| a615 | language config | `%d` |
| a609 | theme config | `%d` |
| aa27 | charge-protect | `charge_protect` |
| aa24 | sys config | `%d`; `sys_count` |
| a634 | memory-play | `memoryPlay`/`memoryType` |
| a639 | EQ type | preset + `filterType/frequency/gain/qValue` |
| a6b6 | BT paired devices | count + list |
| a6c5/a6c3/a6c2/a6c1/a6c4/a6c0 | BT disc/connect/open replies | state + address |
| aa1c | power-key event | `%d` |
| aa22 | TF-card (SD) insert/remove | event `%d` |
| aa0c/aa0f | screen status | `%X` |
| a60a | tip/toast popup (TIP_INFO_EVENT) | tip code (530 / 0x0212 = USB DAC connected, 531 / 0x0213 = USB DAC disconnected) |
| a644 | now-playing / UI state update | song JSON (below) |
| a622 | SD rescan / DB-rebuild status | scan state |
| 06d0/06d1/06d2 | OTA progress / success / file-count | `%d` |

Log strings: GET_VERSION_REPLY @0x27255c, GET_WIFI_CONNECT_STATUS
@0x272658, UAC_SRATE_CHANGE @0x27252c.

**Now-playing JSON payload:** `song_name`, `song_artist_name`
(@0x271b90), `song_album_name`, `song_style_name`, `song_track`, `song_channel`,
`song_duration_time` (@0x271b34), `duration`, `songposition` (@0x271c2c),
`song_sample_rate`, `song_encoding_rate`, `song_bit_rate`, `song_mimetype`,
`song_file_path`, `is_sacd/is_cue/is_dsd`, `love` (favorite), `state` (play state),
`playerflag` (@0x271c08), `curlistlength`, `pos_id`, `playing_num`,
`work_mode` (@0x271b... 0x272b34), `battery` (@0x272c58). Album art -> width/height/
quality/rgb -> `/usr/data/fiio/cover.png`.

diskOS consumes player state through `ui/ipc.c`.

### mq_ui feature surface (subsystems)
IPC core (`/ui` in; `/player`,`/bt_control` out)  ; frame dispatch (cJSON)  ; local
browse/play (all-songs/album/artist/style/favorite/custom, `db_song_ctrl.c`)  ;
now-playing (`class_play_screen.c`, album art `jpg_to_png.c`)  ; Roon endpoint  ;
home/menus (app/system/audio/others/popup `.json`)  ; EQ (`equalizer.json`)  ;
Bluetooth (`bt.json`)  ; WiFi/NAS (`wpa_supplicant`, `hostapd`, `wl rssi`)  ;
system/version/OTA (`ota_update.c`, `set_local_time.c`)  ; FiiO Link (UDP
224.0.0.255 discovery + TCP control, device id "SNOWSKY DISC")  ; bundled zlog.

---

## 5. Config + database schema

Stock SQLite 3.23.1, opened via `sqlite3_open_v2`. Database paths:

| File | Holds |
|------|-------|
| `/usr/data/fiio/db/sysconfig.db` | SYSCONFIG (settings, single row ID=1), CUSTOM_THEME, NAS_CONFIG |
| `/usr/data/fiio/db/song.db` | SONG, MY_LOVE, MEMORY_PLAY, PLAY_LIST, LIST_SONG_0/1/2, CUSTOM_PLAYLIST, PLAYLIST_INFO, PEQ |
| `/usr/data/fiio/db/dic.db` | HAN_PINYIN (CODE INTEGER, PINYIN char(1)) - CJK pinyin sort |
| `/usr/data/fiio/db/theme.db` | theme assets |
| `/usr/data/fiio/db/ebook.db` | e-book |
| `/usr/data/bluetooth.db` | bt_devices |

Only `dic.db` ships in the stock rootfs; the other databases are created in writable
storage at the paths above. diskOS creates and maintains its own playlist-name table
and synchronizes it with `CUSTOM_PLAYLIST_INDEX` when that stock registry is present.

<a id="song-songdb---literal-create-for-my_love-verified-verbatim-song--same--is_love"></a>

### SONG (song.db) - MY_LOVE columns plus IS_LOVE
ID(PK)  ; PATH  ; NAME  ; TITLE  ; ALBUM  ; ARTIST  ; GENRE  ; DISC  ; TRACK  ; IS_CUE  ;
IS_ISO  ; IS_DSD  ; OFFSET(BIGINT)  ; DURATION(BIGINT)  ; NAME_CODE  ; TITLE_CODE  ;
ALBUM_CODE  ; ARTIST_CODE  ; GENRE_CODE(pinyin sort keys)  ; ADD_TIME(INT8)  ;
SAMPLE_RATE  ; BIT_PER_SAMPLE  ; CHANNELS  ; BIT_RATE  ; SONG_MIMETYPE  ;
SONG_PRODUCTION_YEAR  ; IS_SELECT  ; ALBUM_ARTIST  ; ALBUM_ARTIST_CODE  ;
**IS_LOVE** (SONG only, added by upgrade ALTER - favorites flag).
MY_LOVE = same minus IS_LOVE, `UNIQUE(PATH,TRACK)` (@mq_ui.str:9793).

<a id="custom_playlist-songdb---literal-create-verified-mq_uistr9790"></a>

### CUSTOM_PLAYLIST (song.db)
MY_LOVE columns + `PLAYLIST_ID` (after ID), `FOREIGN KEY(PLAYLIST_ID) REFERENCES
PLAYLIST_INFO(ID) ON DELETE CASCADE`, `UNIQUE(PLAYLIST_ID,PATH,TRACK)`.

<a id="playlist_info-songdb---parent-of-custom_playlist-idpk-firm-namecount-inferred-low-confidence"></a>

### PLAYLIST_INFO (song.db)
diskOS playlist names: `ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT, ADD_TIME INT8`.

### PLAY_LIST (song.db) - queue registry: ID(PK)  ; LIST_ID  ; LIST_NAME.

<a id="list_song_012-songdb---active-play-queues-built-by-insert-into-list_song_d--select--from-song-verified-cols-id---list_id---pos_idmusic_id-join-key---path---name---title---album---artist---genre---disc---track---is_cue---is_iso---offset---duration---add_time---is_select---song_type---album_artist"></a>

### LIST_SONG_0/1/2 (song.db)
active play queues, built by `INSERT INTO LIST_SONG_%d ... SELECT ... FROM SONG`. Columns: ID  ; LIST_ID  ; POS_ID(=MUSIC_ID join key)  ; PATH  ; NAME  ; TITLE  ; ALBUM  ; ARTIST  ; GENRE  ; DISC  ; TRACK  ; IS_CUE  ; IS_ISO  ; OFFSET  ; DURATION  ; ADD_TIME  ; IS_SELECT  ; SONG_TYPE  ; ALBUM_ARTIST.

<a id="memory_play-songdb---resume-state-single-row-id1---music_id---is_playing---position---is_cue---is_iso---track-update-verified-mq_playerstr13412"></a>

### MEMORY_PLAY (song.db)
resume state, single row. ID(=1)  ; MUSIC_ID  ; IS_PLAYING  ; POSITION  ; IS_CUE  ; IS_ISO  ; TRACK. (UPDATE @mq_player.str:13412)

### PEQ (song.db) - see section 3. ID  ; STYLE_NAME  ; MASTER_GAIN(REAL)  ; PARAMS_JSON  ; STYLE_PRESET.

### CUSTOM_THEME (sysconfig.db) - ID  ; POS_ID  ; NAME  ; PATH  ; ALIAS  ; TYPE  ; IS_SYSTEM  ; ATTR  ; ALPHA  ; LOCK_TIME/DATE/BATTERY/ID3/MEM_TYPE  ; FRONT_COLOR.
### NAS_CONFIG (sysconfig.db) - ID  ; NAS_NAME  ; NAS_IP  ; USER  ; (password)  ; TYPE  ; AUTO_LOGIN.
### bt_devices (bluetooth.db) - ID  ; NAME  ; ADDR(MAC key)  ; CODEC  ; SAMPLING  ; VOLUME.

<a id="sysconfig-sysconfigdb---single-row-all-int-update-sysconfig-set-s--d-where-id--1-verified-0x25a910"></a>

### SYSCONFIG (sysconfig.db)
single row, all INT, `UPDATE SYSCONFIG set %s = %d where ID = 1` (@0x25a910)
Descriptor @mq_player.str:13091+; runtime pak dump @0x25e7a4: `DSD_MODE,OUT_DEV,VOLUME,WORK_MODE,LIGHT_LEVEL,LIGTH_ON_TIME
(sic),RGB_LEVEL,MUTE,VOL_MODE,RGB_STATUS,MEM_PLAY`. Key set (names do not by themselves establish numeric option mappings):

DSD_MODE  ; OUT_DEV/DEVICE_OUTPUT(->0666)  ; VOLUME  ; WORK_MODE  ; LIGHT_LEVEL  ;
LIGTH_ON_TIME(backlight timeout, sic)  ; RGB_LEVEL  ; MUTE  ; VOL_MODE  ; RGB_STATUS  ;
RGB_COLOUR  ; TRIGGER_IN  ; SYS_THEME  ; LANGUAGE  ; USB_MODE  ; NETWORK_MODE  ;
EQ_TYPE(->0689, =PEQ.STYLE_PRESET)  ; MAX_VOL  ; BALANCE_VOL  ; POWER_SAVE  ;
FILTER_TYPE(DAC digital filter)  ; PLAY_MODE(->0102)  ; MEMORY_PLAY(resume)  ;
FOLDER_JUMP  ; PLAY_GAP(gapless)  ; INPUT_MODE  ; VOL_KNOB_MODE  ; OTA_CFG  ; BATTERY  ;
BT_CODEC  ; SCREEN_ROT  ; PO_PRE_VOL/PO_VOL/PRE_VOL  ; THEME_MODE  ; CHARGE_PROTECT  ;
LOCK_THEME  ; TREBLE  ; BASS  ; WIFI_STATUS  ; BT_STATUS  ; LO_DISABLE  ; OS_MODE  ;
DSD_DECODE  ; SPDIF  ; AUTO_TIME  ; DRE_STATUS  ; LOCAL_IMG_ANIM  ; IMG_ANIM_BRIGHTNESS  ;
ARM_VERSION  ; MCU_OTA_FAILED_TIME  ; KEY_SINGLE/DOUBLE/LONG_CLICK_SLE(gesture->action)  ;
ARTIST_CLASS_TYPE  ; AUDIO_VOLUME_SET.
Player setting commands apply in memory and persist to SYSCONFIG on shutdown.
diskOS separately stores managed values in `diskos.conf` and reapplies them.

---

## 7. Network streaming + USB modes + COMPLETENESS

<a id="work-mode--out_dev-enum-the-master-audio-source-list---names-verified"></a>

### Work-mode / OUT_DEV names
The work-mode name array is near @0x7bb0a0 (strings @0x25f9e0+). The supervisor
@0x444604 compares names such as `AIRPLAY`, `DLNA`, and `ROON` and starts or stops
receivers. Names include `NO_DEFINED`, `I2S3_OUT`, `USB_HOST_NULL`, `NO_WORK_MODE`,
`LOCALPLAYER`, `BTSINK`, `ANALOG`, `UAC`, `DLNA`, `AIRPLAY`, `ROON`, `SPDIF_OPT`,
`SPDIF_RX`, `DMR`, `DMC`, `DMS`, `MIX`, `I2S_IN`, `STREAM_AUDIO`, and `QPLAY`.
Array positions are not command values; use the `0657` mode table in
[COMMAND_MAP.md](COMMAND_MAP.md).

<a id="airplay-receiver---built-in-was-completely-missed"></a>

### AirPlay receiver
The player embeds Shairport-related code: `fiio_airplay.c`, `shairport.c`, `rtsp.c`,
`rtp.c`, `dacp.c`, `metadata.c`, `mdns_avahi.c`, `player.c`, and `libshairplay.so`.
It advertises `_raop._tcp` / `_airplay._tcp` as "SNOWSKY DISC". Configuration is at
`/usr/project/config/airplay2/x2000_airplay2.conf` (Shairport-Sync style).
The supervisor sets
`airplay_play=1` (@0x7efbe4), starts `start_airplay` @0x436008, and creates
`airplay_func` @0x435fc0. It requires WLAN; `a706` reports network status.
The V2.40 trigger is `0657000C000A`. The receiver has artwork and DACP handling;
audio goes through ALSA `snd_pcm_writei` after the output switch.

<a id="also-built-in-network-receivers-missed"></a>

### Other network receivers
- **DLNA/UPnP renderer** - `dlna_player.c`, UPnP RenderingControl/AVTransport SCPD.
- **Roon Ready endpoint** - `roon_player.c` + `libroon.so` RAAT (`roon_transport_*`, volume/seek/shuffle). UI screen `ui_roon_play.c`.
- **QPlay** (Tencent/QQ Music) receiver - `mq_player_server_qplay.c`, MD5(Seed+PSK) auth, SetNetwork/SetLyric.
- **BT SINK** (phone->player A2DP in) - `bt_sink_server.c/bt_sink_control.c` + AVRCP.

<a id="usb-modes-partially-missed"></a>

### USB modes
`usb_mode_change_handler` @0x48eb80 handles the internal `USB_MODE` value.
Configfs gadgets include `uac_demo` (functions `uac1.a` / `uac2.a`, `/dev/usb_dac`,
rate report `aa1b`) and `storage_demo` (`mass_storage.0`, `lun.0/file=/dev/mmcblk0`).
`set_usb_host` is @0x24da24. Internal `USB_MODE=2` selects the card reader;
the UI's `0642` storage selector is **1**, a different enum.

The storage handoff unmounts `/tmp/sdcard` via `unmount_udisk` @0x668ed4, with
five retries and a lazy-force fallback. Use the UI's storage mode rather than
exporting the raw card while the player holds it. Local and host access must
never overlap. On dev builds, switching gadgets drops the USB serial connection.
Use the screen to exit reader mode. Safely eject the card on the host before
reconnecting USB or power-cycling the player.

<a id="corrections-to-earlier-sections"></a>

### Other stock subsystems
- NAS support includes `nas_control.c`, CIFS (`vers=3.0`), NFS, `smbclient -L`,
  and NAS_CONFIG. Some 02xx dispatch slots are still NULL.
- The binaries contain about 221 distinct command-tag strings and about 102 reply-tag strings.
  These counts include tags beyond the two dispatch tables.
- Other subsystems include lyrics (`lrc_paser.c`, `/usr/data/fiio/encoder.lrc`),
  stream/I2S input/capture, animated GIF screensavers, serial numbers, image loaders,
  SACD/DST handling, and MCU updates.

## 8. FULL command map - inventory complete, meanings mixed-confidence

**V2.09 dispatch tables** (entry = `{tag_str_ptr, handler_thunk_ptr}` 8B; thunks tail-jump via GOT to real handler):
- **Table A @0x7c9d30 = 131 entries** (terminator @0x7ca148). Tags 06xx/07xx/08xx/0a51.
- **Table B @0x7ca150 = 75 entries** (terminator @0x7ca3a8). Tags 00xx/01xx/02xx/04xx/05xx. ~36 in-table NULL (declared-unimplemented), ~11 thunk-but-GOT-null, ~28 live.
- **MCU/SPI name-commands** via hw_ctrl.c @0x48d0b8 + sysconfig-key dispatch @0x488000 (NOT 0x482328 = that's the tag<->enum resolver).

The per-tag V/I/U annotations in [COMMAND_MAP.md](COMMAND_MAP.md) distinguish
handler/string identifications from family inference and unknown entries.
Use its command descriptions rather than inferring behavior from nearby tags.

**MCU/SPI name-commands (hw_ctrl.c, SPI to MCU):** GET/SET_DEVICE_MAX_VOL, SET_DEVICE_VOL, GET/SET_INPUT_MODE, SET_OUTPUT_MODE, SET_GAIN, GET/SET_DAC_FILTER, **SET_USB_MODE**, GET/SET_EQ_PRE, GET/SET_EQ_PARAMETER, SET_EQ_RESET, SAVE_EQ, SET_AUDIO_FORMAT, SET_FACTORY, **ENTER_MCU_UPDATE_MODE**, SET_MCU_POWER, SET_MUTE, SET_POWER_DOWN_TO_MCU (shutdown path, 63 call sites), GET/SET_ZERO_DATA_DETECT_TIME, BT_REPORT_RATE/STATE/CODEC_TO_MCU. sysconfig-key setters: RGB_COLOUR/TRIGGER_IN/SYS_THEME/LANGUAGE/USB_MODE/NETWORK_MODE/EQ_TYPE/MAX_VOL/BALANCE_VOL/POWER_SAVE/FILTER_TYPE/PLAY_MODE/FOLDER_JUMP/PLAY_GAP/INPUT_MODE/VOL_KNOB_MODE/OTA_CFG/PO_PRE_VOL/PO_VOL/PRE_VOL/TREBLE/BASS/LO_DISABLE/OS_MODE/DSD_DECODE.

<a id="6-todo-next-passes"></a>

## 6. Unmapped interfaces
The CS43131 ioctl ABI, the `0715` runtime callback @0x822b14, the full named
`SET_USB_MODE` wire encoding, and the `0722` OTA worker chain remain unmapped here.
The tag map retains family-inferred entries where a handler meaning is unknown.

On V2.09, the stock `0622` scan command does not populate `song.db`; diskOS uses
its own scanner (`ui/scanner.c`). Stock tagless rows can have zero duration,
sample rate, bit rate, and channel fields, with ALBUM equal to TITLE.

## 5b. SYSCONFIG - the master settings table (decoded 2026-06-30)
`/usr/data/fiio/db/sysconfig.db` -> table **SYSCONFIG** (single row), one INT column per setting.
This is where every audio/system setting persists (the config-named commands write here).
Columns incl: DSD_MODE, OUT_DEV, VOLUME, WORK_MODE, LIGHT_LEVEL(brightness), LIGTH_ON_TIME,
MUTE, VOL_MODE, USB_MODE, NETWORK_MODE, EQ_TYPE(=0689 preset), MAX_VOL, BALANCE_VOL(channel
balance), POWER_SAVE, FILTER_TYPE(DAC filter), PLAY_MODE(=0102), MEMORY_PLAY, FOLDER_JUMP,
PLAY_GAP, INPUT_MODE, VOL_KNOB_MODE, BT_CODEC, SCREEN_ROT, PO_PRE_VOL/PO_VOL/PRE_VOL,
THEME_MODE, CHARGE_PROTECT, LOCK_THEME, TREBLE, BASS, WIFI_STATUS, BT_STATUS, LO_DISABLE,
OS_MODE, DSD_DECODE, SPDIF, AUTO_TIME, DRE_STATUS, DEVICE_OUTPUT, KEY_SINGLE/DOUBLE/LONG_CLICK_SLE,
ARTIST_CLASS_TYPE, AUDIO_VOLUME_SET.
The filter enum is 0=FAST_LL, 1=SLOW_LL, 2=SLOW_PC, 3=FAST_PC, 4=NON_OS,
5=Wideband_FF. Use the apply commands below. Memory playback uses `0684`;
`0815` saves the resume position rather than choosing the resume mode.
**Do not trace or kill `mq_player`:** interrupting it can release the SD and trigger
an MCU reboot. Trace `mq_ui` for UI command traffic instead.

<a id="5c-audiodac-apply-commands---decoded-live-2026-06-30-strace-of-stock-mq_ui"></a>

## 5c. Audio/DAC apply commands
Scalar setter format: `<tag>000C<value 4hex>`. The player applies these commands
in memory; diskOS manages persistence separately.

- **DRE** = `0812` - `0812000C0000` = ON, `0812000C0001` = OFF.
- **Gain** = `0645` on V2.09; `0649` on V2.28/V2.40/V2.57. Value 0=Low, 1=High.
- **Channel balance** = `0713`: center=`0000`; sides=`00NN` / `01NN`.
- **DAC Filter** = `0653` - enum = menu order: 0=FAST_LL, 1=SLOW_LL, 2=SLOW_PC, 3=FAST_PC, 4=NON_OS, 5=Wideband_FF.
- **SPDIF** = via output-route `0666` - SPDIF on = `0666000C0004`, analog = `0666000C0006`
  (0666 = OUT_DEV/output route: LOCAL_ANALOG=6, SPDIF=4; mutually exclusive with headphone).
- **BT codec** = `06b3`: 0=SBC, 1=AAC, 2/3/4=LDAC mobile/standard/high.
  Codec-menu frames have no payload. Routing uses `06b3001D0000<MAC>` initially;
  diskOS can then request another advertised codec on V2.57. `06b4` sets LDAC quality.
- `0642` selects the USB gadget. See [COMMAND_MAP.md](COMMAND_MAP.md) for route ordering.

<a id="5d-more-settings---decoded-live-2026-06-30-strace-stock-mq_ui-clean-batch"></a>

## 5d. More settings
- **Channel balance** = `0713` - `0713000C<HHLL>`: center=`0000`; one side `00NN` (NN=step), other side `01NN`.
- **Gapless** = `0647` - `0647000C0001` on / `0000` off. (matches COMMAND_MAP 0647=set gapless)
- **Artist list grouping** = `0648` - `0648000C0000` Artist / `0001` Album-Artist. (ARTIST_CLASS_TYPE)
- **Memory playback (resume)** = `0684` - `0684000C000<0|1|2>` = Off / Position / Song.
- **Max volume** = `0711` - `0711000C00<NN>` where NN(hex)=cap, 0..0x78(120).
Output routes must use the coordinated stop/switch/resume path in `ui/modes.c`;
do not send a raw route change during playback.

<a id="5e-sibilance-eq-presets-wired-2026-07-01-code-only-staged-not-yet-deployed"></a>

## 5e. Sibilance EQ presets
Stock and diskOS expose 11 built-in EQ presets (0-10), including preset 9="Sibilance 1"
and 10="Sibilance 2".
The `0689` path (`ui_apply_eq`, range 0-20) selects them with `0689000C0009` / `0689000C000A`.
