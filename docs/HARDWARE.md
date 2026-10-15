# Snowsky Disc - Hardware Capability Map

Hardware and stock V2.09 interfaces for the FiiO Snowsky Disc. This map also notes
how diskOS uses those interfaces and what a replacement audio engine would require.

---

## 1. SoC & Compute

| Item | Value | Source |
|---|---|---|
| SoC | Ingenic **X2000** (xburst2), board `ingenic,x2000_halley5_module_base` | `/proc/cpuinfo` |
| Cores | **2x XBurst II V2**, SMP | `/proc/cpuinfo` (processor 0,1) |
| Clock | ~1.2 GHz (BogoMIPS ~ 2390) | `/proc/cpuinfo` |
| FPU | Yes (per-core) | `/proc/cpuinfo` |
| SIMD | **MSA** (MIPS SIMD Architecture, 128-bit) - `ASEs implemented: msa` | `/proc/cpuinfo` |
| ISA | mips1 / mips2 / mips32r2 + MSA | `/proc/cpuinfo` |
| TLB | 288 entries; 1 HW watchpoint; 6 kscratch regs | `/proc/cpuinfo` |
| DVFS | **None exposed** - no `cpufreq/scaling_available_frequencies` | `/sys/.../cpufreq` |
| Thermal | **No thermal zones** - `/sys/class/thermal` empty | sysfs |
| GPU | **None** (X2000 has no GPU) | `/dev` has no gpu node |
| VPU / video decode | **None** - no `vpu`/`video`/`mem2mem` dev nodes | `/dev` |
| Hardware JPEG | **None** as a dev node (stock `jpg_to_png.c` is software) | `/dev`, RE |

diskOS uses LVGL's software renderer. The UI Makefile does not enable `-mmsa`;
MSA is available for CPU-side optimization. Display scaling is handled separately
by the LCD controller described below.

---

## 2. Memory

| Item | Value |
|---|---|
| RAM total | **117.5 MiB** (`MemTotal: 120308 kB`) |
| Free / available (stock running) | ~19 MB free / ~64 MB available |

Tight but workable. diskOS already runs in this budget. Big in-RAM buffers (e.g. a
full-res rotating album-art layer) must be sized carefully.

---

## 3. Storage

NAND (MTD) with a main system and a smaller recovery system, plus the user microSD.
The recovery rootfs is not large enough to hold the main image:

| mtd | size | name | notes |
|---|---|---|---|
| mtd0 | 2 MB | uboot | bootloader |
| mtd1 | 8 MB | kernel | main kernel |
| mtd2 | 128 MB | rootfs | main rootfs - **squashfs, read-only** (why `fiio_init.sh` can't be edited in place) |
| mtd3 | 8 MB | kernel2 | recovery kernel |
| mtd4 | 25 MB | rootfs2 | recovery rootfs |
| mtd5 | 1 MB | ota | OTA state |
| mtd6 | 1 MB | mac | MAC/calibration |
| mtd7 | 83 MB | **userdata** | writable - mounted as `/usr/data`, where **diskOS lives** |
| mmcblk0 | ~238 GB | microSD | single partition `mmcblk0p1` - the music card |

**Implication:** the **boot hook** is a small edit patched into the read-only rootfs's
`usr/project/fiio_init.sh` (which is why enabling it requires rewriting the rootfs partition - the
flash). The **payload it launches** - the `mq_ui` binary and diskOS's runtime state - lives in the
writable `/usr/data` (mtd7), the safe persistent target. So: the hook is baked into the RO rootfs;
only the UI/state are on `/usr/data`.

---

## 4. Audio - the headline subsystem

### 4.1 DACs - quad CS43131, fully balanced
Four Cirrus **CS43131** chips on I2C bus 3:

| I2C addr | sysfs name | /dev node (major) |
|---|---|---|
| 3-0030 | cs43131  | `/dev/cs43131`  (248) |
| 3-0031 | cs43131b | `/dev/cs43131b` (247) |
| 3-0032 | cs43131c | `/dev/cs43131c` (246) |
| 3-0033 | cs43131d | `/dev/cs43131d` (245) |

dmesg tags include `cs43131_left_negetive` and `cs43131b_open` -> the four DACs are wired
as **L+/L-/R+/R- (fully differential / balanced)**, two CS43131 per channel.
The player has a 3.5 mm output and a 4.4 mm balanced output.

Each CS43131 is a stereo DAC + integrated headphone amp; the family supports PCM to
384 kHz and **DSD64/128/256**. Control is via a **custom FiiO `cs43131` kernel driver**
exposing the four char devices above; stock `mq_player` drives them with **ioctl** (not
ALSA controls). Raw `/dev/i2c-3` is also present as a fallback.

### 4.2 SoC I2S/DMA path - the streaming ceiling
The Ingenic audio controller (ALSA card 0 `x2000`) exposes **5 playback + 5 capture DMA
channels** (`hw:0,0`-`hw:0,4` playback). `aplay --dump-hw-params` on an idle channel:

```
FORMAT:  ALL
SAMPLE_BITS: [3 64]
CHANNELS:    [1 8]
RATE:        [8000 768000]
```

These are the ALSA driver's advertised parameter ranges. The CS43131 output ceiling is 384 kHz PCM / DSD256.

### 4.3 ALSA mixer surface
`amixer controls` shows only the **SoC internal codec** (`ICODEC HPOUTL/MIC GAIN`,
`MICBIAS`), digital mic (`DMIC ...`), line-out muxes (`LO0_MUX`...`LO11_MUX`), and the five
audio-interface formatters (`baic0_fmt`...`baic4_fmt`). There is **no CS43131 control, no
DSD switch, and no digital-filter control in ALSA** - all of that is the kernel driver +
mq_player ioctl path. `BAIC: baic start/stop` in dmesg marks I2S on/off per track.

### 4.4 Decode & format support (from mq_player RE)
- Decoder backend: **libavcodec.so.58 (ffmpeg)** - string `decoder_ffmpeg`.
- DSD: `DSD_MODE_NONE` / `DSD_MODE_NATIVE` / `DSD_MODE_DOP` - native DSD **and** DoP.
- MQA: `is_mqa` flag (detection/passthrough).
- Containers: FLAC, APE, DSF/DFF, **SACD ISO**, CUE sheets; ffmpeg covers ALAC/OPUS/
  WavPack/etc.
- Param validation: `check_sample_param`, `reset hw params`, `AudioCodecOpen dsd`,
  `no support sample! dsd_mode/out_dev/...` - the player gates rate/format per DAC mode.

**Tools present:** `aplay`, `amixer`, `tinyplay`, `tinymix` - raw PCM playback is possible
today (the DAC just has to already be configured for the rate).

---

## 5. Display

| Item | Value |
|---|---|
| Driver | `ingenicfb` |
| Visible | 360x360, 32 bpp (XRGB8888/BGRA, panel mounted 180 degrees-rotated) |
| Framebuffer virtual | 360x**1080** = triple-buffered 360x360 |
| **Overlay layers** | `fb0`-`fb3` = **4 hardware LCDC planes** (`/dev/fb0..fb3`) |
| Backlight | standard `backlight` class, **41 levels (0-40)** |

**Layer control interface:** `ingenicfb` exposes `layer0`-`layer3`
under `/sys/class/graphics/fb0/device/`, each with `enable`, `src_fmt`, `src_size`,
`target_pos`, and **`target_size`**. `target_size` != `src_size` => the LCDC has a
**per-layer hardware scaler** (notable, since there's otherwise no GPU/VPU). layer0 is the
active UI plane (`enable: 1`, src 360x360). Layers position + scale in hardware but **do
not rotate**.

The extra planes offer a path for static scaled artwork. A spinning cover still needs
software rotation. Alpha/blend controls are not exposed in sysfs; the driver ioctl
interface for blending is not mapped here. `fbshot` reads framebuffer memory,
so an fb0 capture does not include other planes composited at scanout.

---

## 6. Input

| Device | node | notes |
|---|---|---|
| Touch | `/dev/input/event1` - **cst816t** | single-finger panel, but speaks **MT type-B** protocol (slot/tracking-id/ABS_MT_POSITION_X/Y/TOUCH_MAJOR/PRESSURE; no plain ABS_X/Y). Caps `EV=0xb`, `ABS=0x6618000` (high word). |
| Keys | `/dev/input/event0` - **x2000_key** | **physical buttons** (GPIO keys) |

We can synthesize input by writing the 32-bit-ABI `input_event` (16-byte) MT-B sequence to
`/dev/input/event1`. The injector source is `tools/tinj.c`. diskOS can also read
the hardware keys via event0.

---

## 7. Power

| IC | I2C | role |
|---|---|---|
| **SGM41513** | 2-001a | battery charger (Li-ion, ~3A class) |
| **CW221X** (Cellwise) | 2-0064 | battery **fuel gauge** -> `/sys/class/power_supply/cw221X-bat` |
| **AW35615** | 2-0022 | **USB-C PD / CC** controller (Type-C orientation + power delivery) |

The fuel gauge exposes capacity and voltage. `/dev/usbcc_ioctl` is the PD/CC
control path. Charging behavior depends on the board and driver configuration.

---

## 8. Connectivity (wireless)

| Item | Value | Source |
|---|---|---|
| Module | **AP6212** = Broadcom **BCM43438 / 4343A1** | dmesg: `chip:0xa9a6`, `fw_bcm43438a1.bin`, `nvram_ap6212a.txt` |
| WiFi | **2.4 GHz only**, 802.11 b/g/n (single-band) | BCM43438 spec |
| WiFi attach | SDIO (`[dhd]` driver v101.10.591.91.40, 512 KB dongle RAM) | dmesg |
| Bluetooth | Broadcom BT over **UART `/dev/ttyS0`** @3Mbaud (`hci0`, BD address (device-specific, redacted)) | `hciconfig` |
| BT firmware | `BCM4343A1_001.002.009.1026.1055.hcd` (the `BCM4345C5/C0` `.hcd` files are leftovers for other FiiO models) | `/lib/firmware/bt_bcm` |
| BT bring-up | **hci0 is NOT attached at boot** - `bcmdhd.ko` only loads the driver/GPIOs. The HCI iface is created on-demand by `brcm_patchram_plus --enable_lpm --enable_hci --baudrate 3000000 --patchram <.hcd> /dev/ttyS0`, then `/usr/project/bluetoothd` (a2dp,avrcp,source) + `bluealsa --device=hci0` (SBC/LDAC). **Stock = bluez; `bsa_server`/`bt_enable_bsa*.sh` = dead code for the wrong chip.** Decoded from `mq_player` strings. | mq_player RE |

Wi-Fi is limited to 2.4 GHz. Bluetooth codec selection depends on the stock
userspace stack and the connected device; see [COMMAND_MAP.md](COMMAND_MAP.md).

---

## 9. USB

| Item | Value |
|---|---|
| Controller | **DWC2 OTG** (`13500000.otg_new`) - **dual-role** (host *or* device) |
| Serial gadget | `serial_demo` exposing **ACM** (VID 0x0525 / PID 0xa4a7); the passwordless serial shell is limited to dev builds |
| USB-DAC mode | stock "UAC" work-mode reconfigures the gadget to **USB Audio Class** (device-as-DAC for a host PC) |
| USB Audio | stock V2.40/V2.57 "USB AUDIO" work-mode switches DWC2 to **USB host mode**, turns on SGM41513 5V VBUS OTG boost, and routes audio out to an external USB DAC via `snd-usb-audio` |

**USB Host Mode (V2.40 / V2.57):** DWC2 OTG host mode is supported in V2.40 and V2.57 firmware (`0642000C0005` gadget selector + `0666000C0003` output route). The device powers the external DAC with 5V via the SGM41513 battery charger/boost controller and outputs digital audio over USB-C. diskOS exposes this under Working Mode as "USB Audio".
The stock player also provides a `storage_demo` card-reader gadget. Do not
mount or access the SD locally while it is exported to a computer;
concurrent access can corrupt the card.

---

## 10. Misc

- **RTC:** `rtc0` present and correct - real hardware clock.
- **ADC:** SoC SAR ADC, 6 aux channels (`/dev/jz_adc_aux_0..5`) - analog reads (e.g.
  jack/line detect, if wired).
- **Watchdog:** hardware `/dev/jz_watchdog`.
- **LED:** **none.** `/sys/class/leds` is empty and there is **no physical LED** on the
  unit. The `RGB_LEVEL`/`RGB_STATUS` fields in mq_player's config blob
  are vestigial, inherited from the halley5 reference design / other FiiO products.
  -> diskOS should plan **no LED features**.
- **Motion sensors:** none on I2C (no accel/gyro) - no tilt/gesture input despite the
  round watch-like shape.

---

## 11. Stock-uses vs hardware-can-do (gap list)

| Capability | HW supports | Stock uses | diskOS opportunity |
|---|---|---|---|
| MSA SIMD | Yes (128-bit) | Stock compiler flags unspecified | CPU-side optimization; UI Makefile has no `-mmsa` flag |
| LCDC overlay planes | 4 (fb0-3) | fb0 only | HW-composited static art/backdrops; rotation remains software |
| I2S rate | 768k/64b/8ch | <=384k/DSD256 (DAC-limited) | none beyond DAC; already maxes the DAC |
| DSD native + DoP | Yes | Yes | parity - reuse driver ioctls |
| Quad balanced DACs | Yes | Yes | direct ioctl control for bit-perfect / HW volume |
| USB host (OTG) | Yes | Yes in V2.40/V2.57 (USB AUDIO) | Implemented in diskOS as USB Audio (mode 4) |
| UAC2 gadget | Yes | Yes (UAC mode) | expose/control USB-DAC from diskOS |
| USB-C PD | Yes (AW35615) | Charging | Power-role UI |
| Physical keys | Yes (x2000_key) | Yes | map HW buttons in diskOS |
| WiFi 5 GHz | **No** | - | hard ceiling: 2.4 GHz only |
| GPU / VPU / HW JPEG | **No** | - | hard ceiling: video is CPU-only (MSA-assisted at best) |
| Thermal / DVFS | **Not exposed** | - | no power/thermal tuning via standard sysfs |
| LED | **None** | vestigial config | none - drop from plans |

---

## 12. "Write our own mq_player / mq_ui?" - assessment

**mq_ui: already done.** diskOS *is* our own UI; it reuses stock `mq_player` purely as an
audio engine over mqueue IPC. No reason to change that split for UI work.

**mq_player: a full rewrite is high-effort but the local-playback core is feasible.**
What a from-scratch local player needs, and how hard each piece is:

| Piece | Feasibility | Notes |
|---|---|---|
| Decode | **Easy** | reuse on-device `libavcodec.so.58` (same as stock) |
| PCM out | **Easy** | tinyalsa/libasound to card 0; up to 768k/64b |
| DAC control (rate/DSD/filter/volume/mute) | **Medium** | open `/dev/cs43131*` and replay the kernel-driver **ioctls** - must RE the ioctl numbers + sequences from `mq_player.asm` (bounded but real work). This is the **critical enabler**. |
| Native DSD / DoP | **Medium** | once DAC ioctls are known, feed the right format |
| MCU glue (keys/charge/USB-detect/power) | **Medium** | stock player talks to the FiiO MCU; our player must too (MCU command surface mapped during RE) |
| Streaming receivers (AirPlay/DLNA/Roon/QPlay/BT-sink) | **Hard** | large independent stacks - **do not rewrite**; keep stock or graft open-source (e.g. shairport-sync) |

**Recommendation (hybrid, incremental):**
1. **Keep stock mq_player** as the engine for now - it already handles DAC/DSD/streaming/
   MCU and diskOS drives it fine over IPC.
2. **RE the `cs43131` ioctl interface regardless** - it's the key that lets diskOS (or a
   future thin player) control bit-perfect output, hardware volume, DSD, and filters
   directly. Highest-value RE next step.
3. **Only build our own *local* player** if we need something stock won't expose - e.g.
   a DSP chain or custom crossfade - using
   ffmpeg + tinyalsa + the RE'd DAC ioctls. Leave streaming to stock.

The streaming receivers and MCU dependency are what make a *complete* replacement
expensive; the audiophile local-playback path is the tractable, high-value slice.

---

## Appendix: probe commands (reproducible)
- SoC/mem: `cat /proc/cpuinfo /proc/meminfo`, `uname -a`
- Audio: `cat /proc/asound/{cards,pcm}`, `aplay -l`, `aplay -D hw:0,0 --dump-hw-params /dev/zero`, `amixer -c0 controls`
- I2C map: `for d in /sys/bus/i2c/devices/*/name; do echo "$d: $(cat $d)"; done`
- Power: `cat /sys/class/power_supply/*/{type,capacity,voltage_now,status}`
- Storage: `cat /proc/partitions /proc/mtd`
- USB: `ls /sys/class/udc`, `cat /sys/class/udc/*/state`, `ls /sys/kernel/config/usb_gadget/*`
- Wireless: `dmesg | grep -iE 'dhd|bcm|chip'`, `hciconfig -a`, `ls /lib/firmware/{wifi_bcm,bt_bcm}`
- Display/input: `cat /sys/class/graphics/fb0/{name,virtual_size,bits_per_pixel}`, `ls /dev/fb*`, `cat /proc/bus/input/devices`
- Engines: `ls /dev | grep -iE 'ipu|vpu|jpeg|adc|watchdog'`
