# Windows release qualification worksheet

Maintainer procedure. Keep dated logs,
command exit codes, host versions and hashes outside the release checkout.

## Authenticate the runtime source on the test box

Use the MSYS2 MSYS shell from the independently installed `C:\msys64`.
Do not import a key delivered beside the source archive or trust its key ID.
The installed pacman keyring must have a previously established MSYS2 trust
root; if that provenance is unknown, stop and establish it independently.
Do not weaken SigLevel or locally sign an unknown package signer to pass.

```sh
pacman -Q msys2-keyring
pacman -Qkk msys2-keyring
pacman-key --list-keys
# Retain the installed keyring version, integrity output and trust provenance.
export MSYS2_GNUPGHOME=/etc/pacman.d/gnupg
bash build/fetch-windows-sources.sh --verify /c/diskos-release-sources
# This verifies the pinned archive plus its .sig without downloading keys.
# gpg must exit 0, with one VALIDSIG and TRUST_FULLY or TRUST_ULTIMATE.
```

Save stdout AND stderr, the full signing/primary fingerprints from VALIDSIG,
and the archive/signature SHA256. Missing key, revoked/expired key, bad signature
or unknown trust is a failure, even if the archive matches the pinned hash.
If the established keyring needs maintenance, do that separately using MSYS2's
trusted package process, then repeat verification. Never use a source-supplied
keyring. Signature authentication of the pinned MSYS2 runtime source archive
is required on every run via `fetch-windows-sources.sh --verify` with the
established MSYS2 keyring. Its pinned hash alone does not establish authenticity.

## Source and relink inputs

Fetch and verify outside the checkout, then copy only the verified archive and
.sig into the checkout at `corresponding-source/windows/` before freezing, as
ordered below. Preserve them alongside the packaged app (also for onefile).
Include the source trees and recipes named
in the tools MANIFEST's Input SHA256 section, the squashfs/LZO/libusb archives,
and every file under licenses/. Preserve NOTICE.md and README.md alongside
onefile so notices and recovery instructions remain readable without execution.
For onedir these files may live at the package root or its _internal data root.

To rebuild the runtime, unpack the authenticated source archive in a separate
MSYS2 build workspace, install the PKGBUILD's makedepends through trusted pacman,
and run `makepkg --holdver` from its msys2-runtime directory. The embedded Git
repository supplies tag cygwin-3.6.10; do not fetch a newer upstream revision.
Keep PKGBUILD, all 45 patches, msys2-runtime.commit, package versions, compiler
flags, cygwin.map and output hashes. Replacing msys-2.0.dll with a modified build
must remain possible; ship it as an adjacent DLL, never embed it into an EXE.
Native recipes: build-windows-tools.sh and build-windows-driver.sh. Modified
libusb/LZO source can be relinked by updating its archive/hash in the recipe.

Preserve the runtime notice set in `licenses/windows/README.md`. Audit frozen
CPython, Tcl/Tk, PyInstaller bootloader, pyusb, pycryptodome and every added DLL
against the packaged dependency inventory.

## Exact enabled-ZIP qualification

Clone the candidate installer revision at version 1.2.3. In MSYS2 bash, from
the clone root, with the pinned packages already installed, run:

```sh
bash build/build-windows-tools.sh
cmp flash/my_write6_dram.bin vendor/windows-x86_64/my_write6_dram.bin
cmp flash/disc_spl_lpddr3.bin vendor/windows-x86_64/disc_spl_lpddr3.bin
export MSYS2_GNUPGHOME=/etc/pacman.d/gnupg
bash build/fetch-windows-sources.sh /c/diskos-release-sources
bash build/fetch-windows-sources.sh --verify /c/diskos-release-sources
sha256sum /c/diskos-release-sources/msys2-runtime-3.6.10-6.src.tar.zst /c/diskos-release-sources/msys2-runtime-3.6.10-6.src.tar.zst.sig
mkdir -p corresponding-source/windows
cp -- /c/diskos-release-sources/msys2-runtime-3.6.10-6.src.tar.zst /c/diskos-release-sources/msys2-runtime-3.6.10-6.src.tar.zst.sig corresponding-source/windows/
cmp /c/diskos-release-sources/msys2-runtime-3.6.10-6.src.tar.zst corresponding-source/windows/msys2-runtime-3.6.10-6.src.tar.zst
cmp /c/diskos-release-sources/msys2-runtime-3.6.10-6.src.tar.zst.sig corresponding-source/windows/msys2-runtime-3.6.10-6.src.tar.zst.sig
```

Require every command to succeed, PASS authentication and equal copies. Retain
the fetch/verify stdout, stderr, fingerprints and hashes outside the checkout.
The recipe copies the tracked device
blobs; no manual copy is needed. Keep the full build log and its final MANIFEST.txt
SHA256 outside the checkout. Do not recompute that expected hash from a bundle
that has since been changed. Retain the workspaces; they are ignored build evidence,
excluded by the packaging allowlists. An existing bundle is refused; rebuild in
a fresh clone rather than deleting the previous evidence.

In native Windows PowerShell, from the same clone root, run
`git status --porcelain --untracked-files=all`; require exit 0 and empty output
after all copies. The two exact runtime-source paths are ignored; any other
unexpected file must still fail the unchanged clean-revision guard. Then build with
`powershell -File build/build-windows.ps1 -EnableWindowsFlash -LibusbDll <verified-official-DLL> -NativeManifestSha256 <hash-from-retained-tools-build-log>`.
The wrapper verifies all native manifest inputs/outputs and compares both blobs
with tracked flash inputs before freezing and again in the frozen collection.
The wrapper also runs the existing source verifier before freezing and on the
frozen sidecars, using the independently installed `C:\msys64` bash and its
established `/etc/pacman.d/gnupg` keyring. Verification checks the pinned archive
hash and the detached signature with full trust, without fetching keys.
The supplied manifest hash detects changes to both a tool and its manifest.
The manifest is a build receipt, not proof of compiler or package authenticity.
Verify the official release archive hash and the selected
`VS2022/MS64/dll/libusb-1.0.dll` member's size/hash against
`build/windows-libusb.json` in that revision. Record the full
build command, clean-source status, embedded SOURCE_COMMIT and WINDOWS_FLASH=True,
Python/PyInstaller/dependency versions, input hashes and final EXE/package SHA256.
Audit dependency closure against `build/windows-build.lock`.
Use the full upstream PyInstaller 6.16.0 `COPYING` text at
`licenses/PyInstaller-COPYING.txt`. Never ship a placeholder.
Package both GUI and CLI EXEs, readable RECOVERY.txt, notices, source/relink
inputs and authenticated runtime source/signature. Finish the actual frozen
inventory. Require equal hashes from two deterministic ZIPs of this unchanged
tree; record the chosen ZIP SHA256 before qualification.
The capability grants host admission only; it is not evidence of qualification.

Test this exact enabled artifact through GUI install/restore and CLI install/
restore-stock without development flags or environment variables. Prove that
both parent and private child run the embedded hook, including the child's false
development-request boolean. Attempt adjacent capability-module files and
PYTHONPATH spoofing: neither may alter the embedded flag or commit. Confirm a
default-off artifact stays disabled even with DISKOS_WINDOWS_DEV_FLASH=1.
Keep confirmation, backup, image/base/writer/count/WinUSB/manifest refusal,
sleep inhibition, supervision, timeout and UNKNOWN recovery checks in the record.
Preserve and distribute the exact qualified artifact; any rebuild needs fresh
artifact qualification and hashes, even when the source commit is identical.

Run `python -m unittest discover -s tests -p 'test_windows_release_inventory.py' -v`.
Set DISKOS_WINDOWS_PACKAGE to the unpacked release directory to require an
artifact check. Absence of an artifact is a skip, never release qualification.

1. Use the recorded enabled ZIP, downloaded with its file provenance retained.
   Extract through Explorer as a clean non-admin user without Python/MSYS2
   installed or on PATH. Record Windows build/architecture and normal Defender/
   SmartScreen results without exclusions. Test spaces and non-ASCII characters
   in package, user profile, firmware and state paths; launch both EXEs from
   unrelated working directories. Verify package SHA256SUMS and inventory.
2. Test GUI, CLI help/doctor and redirected stdout/stderr, AES firmware/image
   build, diagnostics, the shared GUI/CLI lock and persistent backup/state.
   Provoke startup failures on a separate copied package; confirm a visible
   dialog and retained log. Do not modify the qualification package.
3. With a Code 28 mask-ROM device, test cancelled UAC, separate administrator
   credentials, successful WinUSB binding and re-probe. Only bind_winusb.exe
   elevates; driver setup must never start flashing or change another device.
4. Repeat frozen child/self-dispatch, inherited pipes, job assignment, timeout,
   parent-death and sleep-inhibition checks for both EXEs. Exercise missing job/
   pipe handles and refusal paths. Repeat embedded capability checks above.
5. Perform owner-supervised checks A-E: presence/binding/open/claim, small and
   production RAM round trips, powered 25-minute idle, NAND base probe and the
   complete no-write gate. Then GUI and CLI install/restore, normal boot and
   the separately authorized UNKNOWN/Windows/Linux recovery exercise below.
   Record exact app/native/payload/source hashes, raw USB identity, physical
   path, transfer timings, debug verdicts, confirmations, refusals and boot.

Build recovery images independently on Windows and Linux using identical inputs.
Record each image's flash ID, padded size, firmware base and full-file hash
separately. Require DIFFERENT flash IDs between independent builds. Compare
exported filesystem content and metadata, excluding only flash-ID file content
and the existing permitted directory-mtime differences. Check each ID's format
(32 lowercase hexadecimal digits plus LF), regular-file type and mode 0644
separately. Retain full-file hashes for each actual recovery artifact. Use the
existing fixed-ID test harness for deterministic cross-host comparisons; no
production ID override is needed.

With explicit owner authorization, stop the host after a
logged writer start while keeping the Disc powered. Treat the result as UNKNOWN,
wait at least 25 minutes, power-cycle, then prove boot or complete stock restore
from Windows and Linux. Preserve retained backup-generation paths and logs.
Cable/power-loss experiments require a separate explicit owner decision.

## Tk data exclusions and real-package dialog smoke check

Use the exact candidate ZIP and GUI EXE above on a real Windows desktop.
Exercise the retained Tk paths and installer PNG assets after Tk pruning.
Record ZIP/EXE hashes, Windows build, screenshots, dialog text/buttons, each
return to the main window and retained logs. Source mocks or a host Python/Tk
session do not qualify the frozen package. Keep its original tree unchanged.
Before launch, the artifact inventory must assert `_internal/_tk_data/images/`
and `_internal/_tk_data/mkpsenc.tcl` absent, with icons.tcl, Tk/ttk startup
scripts and the shipped Tk incorporated and Tango notices present.
Use a candidate that excludes the specified Tk data.

| GUI path | Real-package exercise and expected result |
|---|---|
| Startup/widgets/fonts/theme | Launch normally, switch configuration modes, edit/select/copy/paste firmware text, tab through controls and scroll the log. Main window, clam widgets, PNG wordmark/Disc images, display lettering, drawn controls and scrolling Canvas render at 100%, 125%, 150% and 200% scaling; no missing script, image or font error. Do not run Remove installer data. |
| Native file chooser, gui.py:_pick_firmware | Browse in Install: cancel once, then select a firmware ZIP from a path with spaces/non-ASCII and confirm the entry updates. Repeat Browse in restore mode when no saved stock exists, using a separately prepared empty state location. Selection alone starts no operation. |
| Inline validation error, gui.py:_fail | With no Disc connected, use a deliberately invalid local firmware ZIP to provoke validation failure. Read the inline error and recovery action, open Details, and return with Back. The log remains readable and controls re-enable. No flashing starts. |
| Custom non-destructive confirmation, gui.py:_show_confirm_dialog | During the separately authorized Code 28 driver test, reach Set up USB driver confirmation. Exercise Cancel, Escape and window-close on separate attempts; each resumes with refusal. Then exercise Proceed in the authorized driver test; only the helper's normal Windows UAC dialog follows. |
| Remove-tool confirmation, gui.py:_show_confirm_dialog; service.py:do_remove | Only with a separately prepared disposable state that is verified to have installed=true, reach the Remove installer data confirmation. Cancel immediately and verify state/logs remain. Never press Proceed. Without installed=true the service deletes state without confirmation, so do not invoke this action in that state. |
| Custom destructive confirmation, gui.py:_show_confirm_dialog | During authorized non-writing install and saved-stock restore refusal trials, reach both confirmations. Begin flash is disabled until acknowledgement. Verify unchecked Return cannot proceed; cancel by button, Escape and dialog-close on separate attempts. Never begin a write for this smoke check. Avoid restore-with-firmware cancellation that may discard a staged backup. |
| Native running-operation question, gui.py:_on_close | During firmware/image preparation, before confirmation/flashing, close the main window: Operation in progress shows OK/Cancel. Cancel keeps GUI/worker usable. Exercise OK only in a separately authorized no-device attempt and record normal closure. |
| Native driver warning, gui.py:_on_close | During authorized driver setup, leave UAC pending and close the main window. Driver setup warning appears and GUI remains open; dismiss warning, cancel UAC, observe normal return. |
| Native flash warning, gui.py:_on_close | During the separately authorized GUI install/restore, after positive writer-start evidence, press window-close once. Flash in progress warning appears and the operation continues. Dismiss and allow completion; do not interrupt power or the app. |
| Windowed startup error, gui_entry.py:_show_error | Launch the separately prepared fault copy omitting _tkinter.pyd. Native user32 error box appears even without Tk and retains the bootstrap log. This is the existing startup-error check, not a modification of the candidate. gui.py:main's Tk startup fallback applies to direct/CLI startup; the frozen GUI re-raises to this bootstrap. |

Driver/device/write rows inherit the worksheet's separate owner authorizations;
the smoke check grants none. No-device rows can run first, but the smoke record
is incomplete until every reachable dialog path above has direct artifact
evidence. Any missing-script/image error, hidden or unusable dialog, failed
cancel/close behavior or absent log fails the check. The GUI uses a scrolling
Canvas and PhotoImage PNG loading from its own
`diskos_installer/assets/`; it does not call Canvas PostScript or ttk::dialog.
Do not add artificial calls to excluded facilities to this smoke check.
Complete load-path evidence is in
WINDOWS_FROZEN_DEPENDENCIES.md's Windows Tk load paths section.

## Assemble without rebuilding

Stage the matching committed source via git archive and preserve the ZIP bytes.
Rebuilding/repacking or sidecar changes require fresh ZIP qualification.
Take both the full SOURCE_COMMIT and ZIP SHA256 from the same qualification
record. Staging requires both and checks the resolved DISKOS_REF against that
commit for all installer source changes, as well as verifying the ZIP hash.
Name every earlier artifact with --artifact; extra files, non-regular entries
and missing artifacts fail, with or without an earlier SHA256SUMS:

```sh
DISKOS_REF=<qualified-source-commit> bash build/stage-release.sh \
  --windows-zip /path/to/qualified.zip --windows-sha256 <recorded-zip-sha256> \
  --windows-source-commit <recorded-full-source-commit> \
  --artifact <authenticated-source-asset-already-in-build/release> \
  linux-x86_64 macos-arm64 windows-x86_64
(cd build/release && sha256sum --check --strict SHA256SUMS)
```

Repeat --artifact for every other attachment. Verify the staged ZIP hash and
complete SHA256SUMS. Publication still requires the owner's explicit go.
