# Install diskOS 1.2.3

[Home](../README.md) | [Tour](TOUR.md) | [Privacy](PRIVACY.md)

> [!CAUTION]
> Flashing replaces the Disc's system software. Stored data is not wiped, and the SD card is not changed. Back up the card and keep a copy of the saved stock image on separate storage. Use a reliable USB cable, keep it connected, and keep the computer awake until flashing finishes. An interrupted flash may require [mask-ROM recovery](#restore-stock-and-recover).

The installer builds an image locally from an official FiiO firmware archive. Select the version already on your Disc, then use **Get firmware** to download and verify its ZIP, or **Browse...** for a matching local copy. It accepts V2.09, V2.28, V2.40, and V2.57. 1.2.1 users can install the signed 1.2.2 app update if updates are allowed; fresh installs and older versions use the installer. App updates do not replace the boot install script. The network name hook, per-flash identity and boot-script update-retention fix require a full flash with the complete 1.2.2 installer; see the changes marked as requiring a full flash in the [release notes](RELEASE_NOTES_1.2.2.md).

## Requirements

- A **FiiO Snowsky Disc** with supported stock firmware.
- The version already on your Disc: V2.09, V2.28, V2.40 or V2.57. **Get firmware** obtains
  its matching official FiiO `.zip`; **Browse...** accepts a local copy for offline use.
  The installer decrypts and extracts its root filesystem locally.
- A reliable USB cable and about 20 uninterrupted minutes.
- **Python 3.8+** and the dependencies installed by `./install.sh` on Linux and source checkouts.
  The macOS app and Windows release ZIP include Python and their tools.
- USB access to mask-ROM device `a108:eaef`.
- **Linux x86-64** (Debian, Ubuntu, Arch, CachyOS, Fedora), **macOS**, or
  **Windows 10/11 x64**; see
  [Host support](#host-support).

On Linux and macOS, do **not** run the installer with `sudo`. Its saved recovery image and state belong under your user
account. On Linux, install the included udev rule once instead:

```bash
sudo cp udev/70-diskos-maskrom.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Build, save, and flash run under your user account.

## OS packages

These packages and setup commands apply to Linux and source checkouts. For the
macOS release app, follow [macOS](#macos). On Windows, follow [Windows](#windows).

Install Python 3.8 or newer and the host packages before running setup. `tkinter` is needed for the graphical installer; the command line works without it. The setup script reports anything missing.

| Host | Python and GUI | USB and firmware tools |
|---|---|---|
| Debian / Ubuntu | `sudo apt install python3 python3-venv python3-tk` | `sudo apt install libusb-1.0-0 squashfs-tools` |
| Arch / CachyOS | `sudo pacman -S python tk` | `sudo pacman -S libusb squashfs-tools` |
| Fedora | `sudo dnf install python3 python3-tkinter` | `sudo dnf install libusb1 squashfs-tools` |
| macOS source checkout | Python from python.org with Tk >= 8.6, or Homebrew Python with matching python-tk | See the source steps below. |

## macOS

1. Download `diskos-installer-macos-arm64.tar.gz` from the release for an Apple
   Silicon Mac. Double-click the archive, then drag **diskOS Installer.app** to
   Applications or another writable folder. Keep the accompanying source and
   licence files with your download. No Python, Tk, Homebrew, libusb or LZO
   installation is needed.
2. Double-click the app. This distribution uses ad-hoc signing, without a verified
   developer identity. If Gatekeeper blocks it, first try opening it once, then
   use **System Settings > Privacy & Security > Open Anyway** and confirm Open.
   Only do this for the package downloaded from the diskOS release page.
3. Select the firmware version already on your Disc and press **Get firmware**.
   For a local download, use the original FiiO ZIP. If Safari already unpacked it,
   right-click the download link and choose **Download Linked File**. The installer
   requires the original archive to verify its recorded checksum.
4. Power off the Disc, hold **Volume Down**, and connect USB directly with a data
   cable. The screen stays black in mask-ROM mode. Select **Install diskOS...**,
   read the confirmation and begin flashing. Keep the lid open and USB connected.
   The installer prevents sleep automatically during the flash and logs that it
   is active. Wait for successful verification before power-cycling the Disc.

If macOS cannot unblock a tool in a read-only location, move the app to a writable
folder and reopen it. If it is still blocked, download and extract a fresh complete
release. The app changes only its own bundled tools' attributes.

### macOS source checkout

Install Python from [python.org](https://www.python.org/downloads/macos/) with
Tk >= 8.6. For Homebrew Python, install the matching `python-tk@<major.minor>`.
`install.sh` checks Tk before selecting Python and refuses an existing environment
that still uses old Tk; rename that `.venv` and rerun setup if instructed.
With Homebrew and the Xcode command line tools available, run from the checkout:

```sh
bash vendor/setup-macos.sh
./install.sh
./diskos-installer doctor
./diskos-installer gui
```

Native setup installs LZO when missing and checks pinned tool versions. It may
require installing/linking the recorded Homebrew versions. See
[native tools](../build/NATIVE_TOOLS.md). Intel source builds need matching x86_64
inputs; the Apple Silicon release is the packaged path.

## Setup

For Linux and source checkouts, run these commands from the installer directory.

The installer runs with your own **Python 3.8+**. The setup script creates a local virtual
environment and installs four Python dependencies into it; nothing is installed system-wide.

```bash
./install.sh
```

The setup check reports missing components. Release bundles include native tools. A source checkout needs native tools built or installed separately.

After setup, run commands through `./diskos-installer`; it selects the local environment for you.

> [!NOTE]
> A source checkout does not include the large host-native flash tools. Build them once using the
> scripts in [native tools](../build/NATIVE_TOOLS.md). A prepared release bundle places them under
> `vendor/<os>-<arch>/`.

## Graphical installer

1. Run `./diskos-installer gui`.
2. Under **Which firmware is on your Disc?**, select its current version, then press
   **Get firmware**. V2.57 is preselected; change it if your Disc runs an older supported version.
   Wait for verification and the filled path. You can do this before connecting the Disc.
   **Browse...** also verifies a matching official ZIP. Then select a variant:
   - **Public** *(recommended):* no always-on root shell. SSH can still be enabled temporarily
     from Debug Mode in the UI.
   - **Dev** (under **Advanced**): adds a passwordless USB-serial root shell on every boot. Use this only on a dedicated
     development device you control.
   - **Allow diskOS updates over Wi-Fi:** checked by default in the 1.2.3 release package, which
     includes the diskOS release key. Untick it to leave the key out. The box is disabled if a
     package has no key. Updates still require a tap on the Disc and accept only signed app bundles.
3. Power the Disc off. Hold **Volume Down** and plug in USB to enter mask-ROM mode. The screen stays
   black; that is expected.
4. Read the Ready checklist. It checks for a Disc in flash mode, USB driver or permission hints,
   firmware readiness, and installer tools. It shows items needing attention and flash-mode help;
   **Refresh** checks again. Checklist warnings do not block the install; the installer validates
   the firmware and actual USB access before flashing.
5. Select **Install diskOS...**. After building the image, the installer shows a confirmation
   screen. Tick **I understand and will keep the USB cable connected until it finishes.**, then
   select **Begin flash (~20 min)**. The run screen shows elapsed time and progress; **Details**
   opens the log. Do not disconnect the cable or let the host sleep. The result screen gives
   the next step; an unknown flash result requires the 25-minute wait described below.
6. After verification succeeds, power-cycle the device. diskOS is embedded in the flashed image and
   installs on first boot; no microSD installation step is needed.

The flash replaces the system software, not the writable data area (`/usr/data`).
On first boot, diskOS installs or updates its own files there; this is not a factory wipe.
The installer does not change your SD card.

## Get firmware

Read the version on the Disc before entering flash mode: in diskOS, use Settings > Device (Settings > System on 1.2.1 and earlier) >
Device Info > Firmware. The stock menu hint is Settings > System Settings > About device.
Choose that version; downloading another version does not upgrade the Disc's kernel or recovery.
The final flash gate still checks the actual Disc and refuses a mismatched base.

**Get firmware** rechecks the saved ZIP first, then downloads from FiiO if needed. No firmware
request happens at startup, when changing the dropdown, or on Install. Download progress shows
bytes and percentage, followed by a separate verification stage. **Cancel** stops acquisition;
**Retry** starts an interrupted download again from zero. Nothing has been flashed at this stage.

After an ordinary download failure, the installer checks Downloads for a matching file. It searches
only that folder, verifies candidates by size and SHA-256, and copies a match into its own cache.
Your original stays untouched. If no match is found, use **Open FiiO download page**, download the
selected version in your browser, then press **Check Downloads**, or use **Browse...**. TLS errors
also offer these buttons. A browser failure leaves a selectable page address. For space or write
errors, follow the displayed message and retry. See the [privacy disclosure](PRIVACY.md#the-installer-this-repo).

In the GUI, downloaded, cached and manually selected ZIPs must match the installer's recorded whole-file size
and SHA-256 before use. The installer checks the selected file again on Install or Restore; a path
alone is not verification. A changed archive is refused, with no override. The pins were measured
from official archives; they are not a claim that FiiO publishes these checksums.

The cache uses FiiO's original filename under `firmware/` inside the installer state folder:

- Linux: `~/.local/share/diskos-installer` (or `$XDG_DATA_HOME/diskos-installer`).
- macOS: `~/Library/Application Support/diskos-installer`.
- Windows: `%LOCALAPPDATA%\diskos-installer`.

`DISKOS_INSTALLER_HOME` overrides that location. **Get firmware** can reuse a matching verified cache
entry offline. **Browse...** works with a local ZIP without network access; the command line still
requires `--firmware` for install and does not download automatically. Downloads need roughly
99-128 MB for the ZIP plus headroom; extraction, building, temporary files and backup staging need
additional free space, checked separately before building.

**Remove installer data** removes cached firmware, partial downloads, saved recovery images and
owned GUI logs. Keep a separate backup first. It leaves Downloads and manually selected originals
alone. This project distributes installer code and verification pins, not FiiO firmware or generated
firmware images.

HTTPS uses system certificate roots plus the bundled certifi roots, with hostname checking and
TLS 1.2 or newer. An explicit `SSL_CERT_FILE` adds trusted certificates; an unreadable or invalid
file fails visibly. Verification cannot be disabled. Normal HTTPS proxy settings are supported.

## Command line

```bash
./install.sh
./diskos-installer doctor
./diskos-installer install --firmware SNOWSKY_DISC_update_*.zip --variant public
```

The macOS app runs the same commands with no setup. Open the app once first so macOS
approves it, then use its executable:

```bash
"/Applications/diskOS Installer.app/Contents/MacOS/diskos-installer" doctor
"/Applications/diskOS Installer.app/Contents/MacOS/diskos-installer" install --firmware ~/Downloads/SNOWSKY_DISC_update_*.zip --variant public
```

Add `--no-ota` to build without an update key, or `--ota-key PATH` to use your own public key.
See [Downgrading or installing your own build](#downgrading-or-installing-your-own-build).

Useful recovery and cleanup commands:

```bash
./diskos-installer restore-stock
./diskos-installer remove
```

`restore-stock` acts on the player. `remove` cleans up the installer's files on the host, including
saved recovery files, firmware cache, partial downloads and its GUI logs; restore first if needed and keep a separate copy of your
saved stock image. It keeps the executable and the Windows USB driver binding. In the GUI,
this action is **Remove installer data...**; it does not restore firmware or remove diskOS
from the Disc.

## Windows

Use **Windows 10 or 11, x64**. Follow the [Windows install steps](../README.md#windows)
with the official FiiO firmware ZIP matching the firmware already on your Disc.
Linux setup commands (`./install.sh`, `sudo`, and the udev rule) do not apply to Windows.
The Windows installer offers WinUSB setup for the Disc in mask-ROM mode; you do not need
to install a separate driver package. Keep the app under your normal user account.
For terminal use, open PowerShell in the extracted folder:

```text
.\diskos-installer-cli.exe setup-driver
.\diskos-installer-cli.exe doctor
```

`setup-driver` asks for confirmation before driver setup and never flashes. `doctor`
shows the detected driver and USB access; it does not set up the driver.

If the graphical installer cannot start, its error dialog gives the diagnostic log location
when available. Run `diskos-installer-cli.exe doctor` from PowerShell for diagnostics.

During flashing, Windows may show an empty console window for the flash tool.
Leave it open; closing it interrupts the host flash tool, and the Disc may keep writing.

### Windows errors

Quote the complete error code and keep the log. Driver setup does not flash the Disc.
If a flash may have started and the result is unknown, leave the Disc connected and
powered for **at least 25 minutes after the error**, with the computer awake. Do not
retry or disconnect during this wait. Then power-cycle it and restore stock if it
will not boot. Follow [RECOVERY.txt](../RECOVERY.txt), also included in the Windows ZIP, for the full recovery steps.

| Code | What to do |
|---|---|
| E101 | The host, runtime or build does not support flashing. Use the public flash-enabled 1.2.3 Windows ZIP on native Windows x64. Source runs and disabled builds are not the public flashing package; use Linux if your host is unsupported. |
| E102 | A required bundled component is missing. Obtain the complete matching installer package; keep its files together. Keep the log if it still fails. |
| E103 | A native tool could not start. Follow the message: use an executable filesystem, supply the required libraries, or rebuild the matching tools. |
| E104 | The image tools lack required compression or archive support. On Linux, if the bundled tools fail this check, the installer checks a complete pair of system `mksquashfs` and `unsquashfs` from `PATH` the same way and uses them; installing squashfs-tools with LZO support fixes it there. Otherwise use the complete matching package; report the error if it persists. |
| E105 | A required firmware-decryption component cannot load. For a standalone app, obtain the complete package again. For a source run, install `requirements.txt` in the Python environment running the installer. |
| E106 | macOS could not safely prepare its bundled tools. Move the app to a writable folder and reopen it; if still blocked, download and extract a fresh complete release. |
| E110 | No connected Disc was found in mask-ROM mode. Power it off, hold **Volume Down**, and connect USB; the screen stays black. |
| E111 | More than one boot-mode device was found. Disconnect the other Ingenic devices and leave only the Disc connected. |
| E112 | Windows could not list the USB devices. Reconnect the Disc in mask-ROM mode, then check USB access again. |
| E113 | The Disc was found but USB access failed. Select **Refresh** in the Ready checklist. Use **Set up USB driver...** only if offered; if WinUSB is already ready, close other USB tools and reconnect the Disc. |
| E114 | The Disc is present but WinUSB is not ready. Select **Set up USB driver...** in the Ready checklist when offered and follow the message. |
| E115 | The boot-mode USB check failed. Close other USB tools, power-cycle the Disc into mask-ROM mode, and check its driver and connection again. |
| E116 | Windows or macOS could not keep the computer awake for flashing. Nothing was sent to the Disc. Resolve the reported power-request or caffeinate error before retrying. |
| E117 | The bundled USB or driver tool failed its integrity checks. Use a complete matching installer package; do not mix tools from different versions or bypass the check. |
| E118 | Administrator approval was cancelled. Select **Set up USB driver...** again when ready. |
| E119 | Administrator approval or credentials are needed. Ask an administrator to approve driver setup. |
| E120 / E121 / E122 | The image is missing, the wrong size, or invalid. Rebuild it with this installer from your matching official firmware ZIP. |
| E123 / E124 | The writer and image do not match. Use the complete matching installer package; do not replace its writer. |
| E125 / E143 | The installer has no supported firmware base for this image or backup. Do not substitute another firmware version; keep the log and get help. |
| E126 | Driver detection or setup could not be confirmed, or the connected device changed. Setup may still be running. Wait for it to finish, reconnect only the Disc, and check access before trying setup again. |
| E127 | The Disc has an unfamiliar driver. Setup will not replace it. Keep the displayed driver details and get help before changing the driver. |
| E128 | WinUSB is bound but USB access failed. Close other USB tools, reconnect the Disc in mask-ROM mode, and check access again. |
| E140 | Select the matching official firmware ZIP. |
| E141 | No saved stock image is available. Restore with the matching official firmware ZIP as described below. |
| E142 | The saved image or installer state path is unusable. Keep the state folder and log, follow the specific message, and get help if needed. Do not truncate a backup or discard recovery files. |
| E144 / E145 | The backup record disagrees with the image or is damaged. Preserve the state folder and restore with the matching official firmware ZIP. |
| E201-E226 | Firmware extraction or validation failed, or a required payload is unsuitable. Use the matching unmodified supported official ZIP and complete package. Do not bypass a version or hash refusal; keep the specific message if it persists. |
| E230-E250 | Image construction or validation failed. Follow the specific message; use the standard Public variant and complete package. Do not flash an image that failed validation. |
| E301 / E302 / E303 / E321 | The flash result is unknown or timed out. Follow the 25-minute wait and recovery steps above. |
| E304 | Flashing could not start; nothing was sent to the Disc. Fix the problem shown in the message before retrying. |
| E310 | Verification failed or the writer returned an unknown result. If completion is uncertain, follow the 25-minute wait first; then restore the complete stock image. |
| E311 | The writer reported the wrong capacity after flashing. Treat the image as incomplete; restore using a complete matching installer package. |
| E320 | The firmware base gate refused before writing. Read the full reason and follow the matching F201-F211 action in [RECOVERY.txt](../RECOVERY.txt); a NAND-state refusal requires a full power-off and fresh mask-ROM entry. |
| F000 | The device result is unknown. Follow the 25-minute wait and recovery steps above. |
| F101-F106 | The writer stopped with a flash-memory error. Return to mask-ROM mode and restore the complete stock image; keep the log if it fails again. |
| F201-F211 | The writer refused before writing. Follow the specific code in [RECOVERY.txt](../RECOVERY.txt): firmware mismatch, damaged plan, NAND state, and unreadable partitions need different actions. Never bypass the gate. |

### Restore stock on Windows

Power off the Disc, hold **Volume Down**, and connect USB to enter mask-ROM mode;
the screen stays black. Read the Ready checklist and select **Restore stock firmware...**.
The restore screen checks the saved stock image when you request restore. A usable image needs
no download. If it is missing or fails validation, choose the Disc's version and use **Get firmware**
or **Browse...** to rebuild it. On the confirmation screen, tick the acknowledgement and select
**Begin flash (~20 min)**. The installer verifies the saved stock image before writing. Keep the
computer awake and wait for successful verification before power-cycling the Disc.
Restoring replaces the Disc's system software with FiiO's stock firmware and deactivates diskOS.
It is not a factory wipe: stored data and inactive diskOS files under `/usr/data` remain,
and the SD card is not changed. Keep the USB cable connected until it finishes.

Keep the entire installer state folder, normally `%LOCALAPPDATA%\diskos-installer`
(or your configured state location), and a separate copy of the stock backup.
Keep interrupted backup generations too. Automatic restore uses the committed backup.
For **E141/E144/E145**, preserve the state folder. The GUI offers the firmware acquisition flow
when the saved image cannot be used; the command-line firmware option below is also available.
An acquisition or rebuild failure keeps the previous backup and committed pointer.

From PowerShell in the extracted folder:

```text
.\diskos-installer-cli.exe restore-stock
.\diskos-installer-cli.exe restore-stock --firmware "C:\path\to\matching-firmware.zip"
```

Keep [RECOVERY.txt](../RECOVERY.txt) with the package for recovery instructions when offline.

Use only the official ZIP matching the Disc's existing kernel and recovery firmware.
The installer rebuilds and checks the stock image before writing. Never resume at a
guessed block or bypass a refusal. If you cannot establish the matching firmware version,
keep the logs and get help. The Linux installer also accepts
`restore-stock --firmware <zip>` with the same matching firmware.

## Upgrading to 1.2.2

1.2.1 users can install the signed 1.2.2 app update over Wi-Fi from Settings > System >
Update diskOS if updates are allowed. App updates do not replace the boot install script.
The network name hook, per-flash identity and boot-script update-retention fix require a full flash with the
complete 1.2.2 installer; see the [release notes](RELEASE_NOTES_1.2.2.md). Fresh installs
and older versions also use the installer.

For a full flash, use the complete 1.2.2 installer and matching flash tools, supply your
official firmware archive, and follow the same steps. Back up the card and keep your
saved stock image first. The installer accepts only unmodified official V2.09, V2.28, V2.40, and
V2.57 firmware. Before writing, it checks that the Disc's kernel and recovery match the selected
firmware. The saved stock image remains available for restore.

The 1.2.3 release image includes the diskOS release key by default.
Settings > Device > Update diskOS checks for a newer signed release and updates only the diskOS app,
not stock firmware or the whole flashed image. After a trial update, choose Keep or Go back;
a broken update rolls back automatically. Settings > Device > Allow diskOS Updates can stop
downloads. Off still allows a release check, but nothing is downloaded or staged. Turning it off
while an update is pending offers Discard or Cancel. An install without a key shows "Not supported
on this install". Untick the GUI checkbox when flashing or use `--no-ota` to leave the key out.

## Downgrading or installing your own build

The installer can flash an older diskOS release, or your own build of the UI (`--ui PATH`), on top
of supported stock firmware. The in-app update path is designed to accept only newer releases
signed by the key in the image. A full flash with the complete 1.2.3 installer clears downloaded app updates and starts
the UI included in that installer or supplied with `--ui`. Settings and music are kept.
Older installers may retain a newer accepted app update, which can run at the next boot.

Choose how the image treats over-the-air updates when you build it:

```bash
./diskos-installer install --firmware SNOWSKY_DISC_update_*.zip --ui ./my_mq_ui --no-ota
./diskos-installer install --firmware SNOWSKY_DISC_update_*.zip --ui ./my_mq_ui --ota-key my_root.pub.pem
```

- `--no-ota` builds an image with no update key at all, so it never accepts in-app updates. It
  overrides `DISKOS_OTA_ROOTPUB` and any key bundled with the installer.
- `--ota-key PATH` bakes your own public key (PEM, ECDSA P-256). That image accepts only updates
  you sign with the matching private key. A private key is refused.
- With neither option the installer uses `DISKOS_OTA_ROOTPUB` or a bundled key if one exists.
  The 1.2.3 release package bundles the diskOS release key; a source checkout without a key
  builds without updates. `./diskos-installer doctor` and the confirmation before flashing both
  say which state the image will have.

An older UI can be incompatible with a newer stock base.
Reinstalling a diskOS version from before the playlist ID fix can remove stock playlist members
when a new playlist is created. Keep a copy of important playlists before downgrading.

## Host support

- **Linux x86-64:** supported.
- **macOS, Apple Silicon:** use the self-contained release app; no separate runtime or tool installation.
- **Windows 10/11 x64:** use the complete flash-enabled 1.2.3 release ZIP. It includes the runtime and native tools; only Disc driver setup asks for administrator approval. Source runs require a development opt-in to flash; use the release ZIP for normal installation.
- **macOS, Intel:** build the native tools with `vendor/setup-macos.sh` (Homebrew build dependencies required).

**Image builds work on the default case-insensitive macOS filesystem.** The installer creates
a case-sensitive APFS scratch image with `hdiutil` for rootfs extraction and validation,
then detaches and removes it. No manual volume setup is needed for a normal build.

The scratch image avoids the E230 `unsquashfs ... already exists` failure caused by stock files
whose names differ only in case, such as `back_home.png` and `BACK_HOME.png`.
Linux and case-sensitive macOS volumes use the existing build path.

## Wi-Fi after a restart

In 1.2.1, Wi-Fi skips the extra startup wait on V2.57 when Bluetooth is
saved off; other startup cases can still take about a minute. If no IP appears, reconnect in
Settings > Network > Wi-Fi.

## Restore stock and recover

`restore-stock` reflashes the checksum-verified stock root filesystem reconstructed and saved during
installation. This is built from your official firmware archive, not a dump of the device's original
partition. It deactivates diskOS, but it is not a factory wipe; inactive files under `/usr/data`
remain until you remove them.

The GUI validates the saved image before using it. When it cannot be used, the same version
dropdown, **Get firmware** and **Browse...** controls rebuild it from matching official firmware.
The command-line equivalent is `restore-stock --firmware <zip>`. A valid saved image is usable offline.

Older, shorter saved stock images remain usable: restore creates a copy padded to the current
768-block image size, then validates it before flashing. Keep your saved stock image on separate
storage.

You can also switch without reflashing:

- **Persistently:** Settings > Device > Default UI > Stock
- **Once:** hold **Volume Up** from power-on until the other UI appears

Both keep the diskOS delete guard, because the diskOS image stays installed. Restoring the saved
stock image removes the guard.

On any host, if flashing may have started and the result is uncertain, leave the Disc powered
and connected for **at least 25 minutes after the error**, with the computer awake. Do not
retry or disconnect during the wait. Then follow [RECOVERY.txt](../RECOVERY.txt).

If a flash fails, the root filesystem can be left partially written. To attempt recovery,
power off, hold **Volume Down**, and reconnect USB to enter mask-ROM mode,
then reflash diskOS or the saved stock image. Mask-ROM lives in on-chip ROM and is entered before
flashed code runs.

## Debug Mode

<details>
<summary><strong>Optional SSH access and security notes</strong></summary>

Debug Mode is **off by default**. Open **Settings > Device > Debug Mode > Enable Debug** to show an
SSH command and a newly generated password. The password rotates each time Debug Mode is enabled.

While enabled, the password is stored in plaintext at `/usr/data/sshd/current_pw` with mode `0600`
so the UI can display it again after a restart. Disabling Debug Mode removes the file. A reboot can
leave a stale copy, but the SSH overlay is inactive until Debug Mode is enabled again.

Debug Mode uses Dropbear 2022.83, which predates the CVE-2023-48795 Terrapin Strict-KEX mitigation.
Use it only for short sessions on a trusted network and turn it off when finished.

</details>

## Error codes

<details>
<summary><strong>Installer and device error reference</strong></summary>

Quote the complete code in bug reports. A stopped flash can leave the root filesystem partially
written even when the writer fails closed.

| Code | Meaning |
|---|---|
| **E1xx** | Environment or preflight; nothing was written |
| E101 / E102 | Unsupported host or missing required component |
| E103 | A native tool cannot execute; check the filesystem's execute permissions and required libraries |
| E105 | pycryptodome AES dependency missing or cannot load. Source runs: install requirements.txt in the Python environment running the installer (Windows included). Standalone builds: re-download or rebuild with those requirements. Refused before firmware work or device steps |
| E110-E113 | Mask-ROM device detection or USB permission problem (E113: seen but cannot be opened; the message gives the fix) |
| E120-E122 | Image missing, wrong size, or not a squashfs |
| E123 / E124 | Writer is not the exact bundled build, or its capacity differs from the image; refused before any NAND write |
| E140-E142 | Firmware ZIP, saved stock image, or state-directory problem |
| **E2xx** | Firmware extraction and image build |
| E201-E226 | Unsafe archive, OTA manifest, decrypt, rootfs, version, payload, hash, or firmware-base problem. E221/E224/E225 mean the firmware is not an unmodified supported version; install and build cannot override them |
| E230-E250 | Squashfs extraction/build, size, validation, filesystem, symlink, partition, or variant problem |
| E234 | Build filesystem could not be verified as case-sensitive, or scratch-volume setup failed; see the accompanying message |
| **E3xx** | Host-side flashing |
| E301 / E302 | Missing or truncated result; outcome unknown |
| E303 | Flash timed out; outcome unknown - leave the Disc connected and powered with the host awake for at least 25 minutes after the error before recovery |
| E304 | The flash could not be started on this computer; nothing was sent to the Disc |
| E310 | The writer reported a failure or an unknown result; if completion is uncertain, wait at least 25 minutes as above before recovery |
| E311 | Writer capacity reported after flashing does not match the image; treat the flash as failed |
| E320 | The firmware-base gate refused before writing; follow the specific F201-F211 action in [RECOVERY.txt](../RECOVERY.txt) |
| E321 | The writer refused after it had started erasing; outcome unknown |
| **F1xx** | Device writer aborted; return to mask-ROM and reflash or restore |

Process exit codes are `0` success, `1` error (including most preflight refusals),
`2` usage or an install refused for an unsupported host or missing firmware argument,
`3` cancelled, and `130` interrupted.

</details>
