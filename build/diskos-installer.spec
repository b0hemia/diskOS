# PyInstaller spec - Windows onedir, macOS app bundle, Linux onefile.
# Bundles the native flashing tools (vendor/<os>-<arch>/) and the payload
# (mq_ui + on-device init scripts) as data, plus pyusb. The result needs no
# system Python and no system packages.
import os
import sys
import argparse
import re
import subprocess
import hashlib
import json
from pathlib import Path

sys.path.insert(0, str(Path('build').resolve()))
from verify_git_blobs import BlobGuard, DEVICE, PAYLOAD

ROOT = os.path.abspath(os.path.join(os.getcwd()))   # run from installer/
sys.path.insert(0, ROOT)  # The build-time dependency check runs before Analysis sets pathex.
# host tag for the vendor dir to bundle (build on each target OS)
import platform
_sys = platform.system().lower()
_os = {"darwin": "macos", "linux": "linux"}.get(_sys, _sys)
_arch = {"x86_64": "x86_64", "amd64": "x86_64", "arm64": "arm64",
         "aarch64": "arm64"}.get(platform.machine().lower(), platform.machine().lower())
TAG = f"{_os}-{_arch}"

# Arguments after PyInstaller's '--' are explicit maintainer build choices.
from diskos_installer import __version__, platform_probe
options = argparse.ArgumentParser()
options.add_argument("--enable-windows-flash", action="store_true")
options.add_argument("--source-commit", default="")
capability = options.parse_args()
if capability.enable_windows_flash and (
        not platform_probe.windows_native_runtime() or __version__ != "1.2.3"
        or not re.fullmatch(r"[0-9a-f]{40}", capability.source_commit)):
    raise SystemExit("REFUSING to enable Windows flash: native x64 Windows CPython, "
                     "version 1.2.3 and a full source commit SHA are required")

if capability.enable_windows_flash:
    def repository_git(*args):
        return subprocess.run(
            ["git", "--no-optional-locks", "-C", ROOT, *args],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace").stdout

    try:
        repository_root = repository_git("rev-parse", "--show-toplevel").strip()
        if os.path.normcase(os.path.realpath(repository_root)) != os.path.normcase(os.path.realpath(ROOT)):
            raise ValueError("installer/ must be the Git repository root")
        head = repository_git("rev-parse", "--verify", "HEAD^{commit}").strip()
        if head != capability.source_commit:
            raise ValueError("source commit SHA must equal installer HEAD")
        if repository_git("status", "--porcelain", "--untracked-files=no"):
            raise ValueError("installer has tracked changes")
        # Ignored native build outputs are expected; other untracked files in
        # packaged directories must not acquire the identity of a clean commit.
        if repository_git("ls-files", "--others", "--exclude-standard", "-z", "--",
                          "diskos_installer", "payload", f"vendor/{TAG}", "src/usbboot"):
            raise ValueError("installer has untracked files in packaged paths")
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        raise SystemExit(f"REFUSING to enable Windows flash: cannot verify a clean installer revision: {exc}") from exc

# Refuse the fixed fetch-note marker in every Windows build, including development.
# A build must carry the licence text before it can produce a distributable tree.
if _os == "windows":
    _copying = os.path.join(ROOT, "licenses", "PyInstaller-COPYING.txt")
    try:
        with open(_copying, encoding="utf-8") as notice:
            _placeholder = "[VERIFY] PLACEHOLDER ONLY - NOT THE PYINSTALLER LICENCE TEXT." in notice.read().splitlines()
    except OSError as exc:
        raise SystemExit(f"REFUSING to build: cannot read licenses/PyInstaller-COPYING.txt: {exc}") from exc
    if _placeholder:
        raise SystemExit("REFUSING to build: licenses/PyInstaller-COPYING.txt contains the placeholder marker")

# Rewrite even default builds: an earlier enabled build must leave no stale flag.
# Analysis embeds this hook as code; it is never an adjacent data-file override.
os.makedirs(workpath, exist_ok=True)
capability_hook = os.path.join(workpath, "diskos_build_capabilities.py")
with open(capability_hook, "w", encoding="ascii") as hook:
    hook.write("from diskos_installer import _build_capabilities\n"
               f"_build_capabilities.WINDOWS_FLASH = {capability.enable_windows_flash!r}\n"
               f"_build_capabilities.SOURCE_COMMIT = {capability.source_commit!r}\n")

binaries = []
hookspath = []
if _os == "macos":
    from macos_inputs import verify as verify_macos_inputs
    verify_macos_inputs(ROOT, os.environ.get("DISKOS_MACOS_INPUTS", ""),
                        os.environ.get("DISKOS_MACOS_INPUTS_SHA256", ""))
    # PyInstaller's supported binary collection rewrites Mach-O dependencies.
    # Override host-discovery so USB always uses our explicitly provided dylib.
    _usb_hooks = os.path.join(workpath, "macos-hooks")
    os.makedirs(_usb_hooks, exist_ok=True)
    with open(os.path.join(_usb_hooks, "hook-usb.py"), "w", encoding="ascii") as hook:
        hook.write("hiddenimports = ['glob']\nbinaries = []\n")
    hookspath = [_usb_hooks]
if _os == "windows":
    # Only the explicitly supplied official DLL may enter the Windows freeze.
    # PyInstaller appends the spec name to --workpath and cleans that subdirectory.
    # The wrapper's private input stays alongside it, outside the cleaned directory.
    with open(os.path.join(ROOT, "build", "windows-libusb.json"), encoding="ascii") as manifest:
        _libusb = json.load(manifest)
    _libusb_dll = os.path.abspath(os.path.join(os.path.dirname(workpath), "libusb", "libusb-1.0.dll"))

    def verify_libusb_file(path, digest, size):
        try:
            with open(path, "rb") as stream:
                content = stream.read()
            if len(content) != size or hashlib.sha256(content).hexdigest() != digest:
                raise ValueError("size or SHA256 mismatch")
        except (OSError, ValueError) as exc:
            raise SystemExit(f"REFUSING to build: pinned libusb input {path}: {exc}") from exc

    verify_libusb_file(_libusb_dll, _libusb["dll_sha256"], _libusb["dll_size"])
    verify_libusb_file(os.path.join(ROOT, _libusb["source_path"]),
                      _libusb["source_sha256"], _libusb["source_size"])
    binaries = [(_libusb_dll, ".")]
    # Override hooks-contrib's host-discovering hook-usb only on Windows.
    # glob is needed by its existing runtime hook, which still loads from _MEIPASS.
    _usb_hooks = os.path.join(workpath, "windows-hooks")
    os.makedirs(_usb_hooks, exist_ok=True)
    with open(os.path.join(_usb_hooks, "hook-usb.py"), "w", encoding="ascii") as hook:
        hook.write("hiddenimports = ['glob']\nbinaries = []\n")
    hookspath = [_usb_hooks]

# EXPLICIT allowlists: only these files are bundled. Anything else lying in vendor/<tag>/ or payload/
# (backups, scratch tools such as diskos-prefstat, keys) can never be picked up by a directory listing.
_required_native = ["usbboot", "mksquashfs", "unsquashfs",
                    "my_write6_dram.bin", "disc_spl_lpddr3.bin"]
if _os == "windows":
    _required_native = [n + ".exe" if not n.endswith(".bin") else n for n in _required_native]
    _required_native += ["bind_winusb.exe", "msys-2.0.dll", "MANIFEST.txt"]
_required_payload = list(PAYLOAD)
vend = os.path.join(ROOT, "vendor", TAG)
payload = os.path.join(ROOT, "payload")

# FAIL the build (don't ship an unusable artifact) if any required native tool or
# payload for this host is missing.
_missing = [n for n in _required_native if not os.path.exists(os.path.join(vend, n))]
_missing += [f"payload/{n}" for n in _required_payload if not os.path.exists(os.path.join(payload, n))]
if _missing:
    raise SystemExit(f"REFUSING to build: missing bundled files for {TAG}: {_missing}. "
                     f"Populate vendor/{TAG}/ and payload/ first.")

datas = [(os.path.join(vend, n), f"vendor/{TAG}") for n in _required_native]
if _os == "macos":
    _mac_libusb = os.path.join(vend, "lib", "libusb-1.0.0.dylib")
    if not os.path.isfile(_mac_libusb):
        raise SystemExit("REFUSING to build: missing bundled libusb-1.0.0.dylib")
    datas = [(os.path.join(vend, n), f"vendor/{TAG}") for n in _required_native if n.endswith('.bin')]
    binaries += [(os.path.join(vend, n), f"vendor/{TAG}") for n in _required_native if not n.endswith('.bin')]
    binaries += [(str(p), f"vendor/{TAG}/lib") for p in sorted(Path(vend, 'lib').glob('*.dylib'))]
if _os == "windows":
    # Keep manifest inputs at the paths used by runtime verification. The helper
    # embeds its UAC manifest; the hashed original belongs here, not in vendor/.
    _windows_sidecars = [
        "build/windows-libusb.json",
        "src/usbboot/usbboot.c", "build/build-windows-tools.sh",
        "build/build-windows-driver.sh", "src/windows-driver/bind_winusb.c",
        "src/windows-driver/bind_winusb.manifest", "src/windows-driver/bind_winusb.rc",
        "corresponding-source/squashfs-tools_4.6.1.orig.tar.gz",
        "corresponding-source/lzo2_2.10.orig.tar.gz",
        "corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2",
        "corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2.asc",
        "corresponding-source/msys2-squashfs.patch",
        "corresponding-source/windows/msys2-runtime-3.6.10-6.src.tar.zst",
        "corresponding-source/windows/msys2-runtime-3.6.10-6.src.tar.zst.sig",
        "LICENSE", "NOTICE.md", "README.md",
    ]
    # Preserve the landed notice texts byte for byte, including upstream Unicode.
    _license_files = """
        0BSD-xz.txt Apache-2.0-OpenSSL.txt
        BSD-2-Clause-lz4.txt BSD-3-Clause-pyusb.txt BSD-3-Clause-zstd.txt
        Apache-2.0-mbedTLS.txt BSD-2-Clause-Monocypher.txt BSD-3-Clause-SpeexDSP.txt
        BSD-3-Clause-openscreen.txt LGPL-2.1-libplist.txt MIT-libplist-time64.txt
        Unlicense-or-MIT-0-dr_libs.txt Zlib-nanopb.txt
        BSD-3-Clause-SpeexDSP-resampler.txt MIT-libplist-jsmn.txt Zlib-LodePNG.txt
        MIT-or-Unlicense-stb_image_write.txt
        HarfBuzz-COPYING SheenBidi-Apache-2.0.txt Unicode-License-v3.txt
        OFL-Devanagari.txt OFL-Gurmukhi.txt OFL-Lato.txt
        OFL-NotoNaskhArabic.txt OFL-NotoSansHebrew.txt
        BSL-1.0-dlg.txt CC-BY-4.0.txt CPython-3.12.10-Windows-LICENSE.txt
        CPython-3.12.10-Incorporated-Software.txt
        FTL-FreeType.txt Tk-8.6.15-license.terms.txt
        Tk-8.6.15-Incorporated-NOTICES.txt
        CC-BY-SA-3.0-Tango.txt
        Tcl-8.6.15-Bison-NOTICE.txt Tcl-8.6.15-regex-NOTICE.txt
        Tcl-8.6.15-libtommath-LICENSE.txt Tcl-8.6.15-zlib-LICENSE.txt
        PyCryptodome-3.23.0-Windows-LICENSE.rst PyCryptodome-3.23.0-Windows-NOTICES.txt
        PyCryptodome-3.23.0-Native-NOTICES.txt PyCryptodome-3.23.0-libtom-LICENSE.txt
        CC0-1.0-SipHash.txt
        GPL-2.0-zstd.txt GPL-2.0.txt GPL-3.0.txt LGPL-2.1-libusb.txt
        LGPL-2.1.txt LICENSE-TJpgDec.txt LICENSE-dropbear.txt LZ4-LICENSE.txt
        MIT-JSMN.txt MIT-LVGL.txt MIT-musl.txt MIT-qrcodegen.txt
        OFL-1.1-Archivo.txt OFL-1.1-CourierPrime.txt OFL-1.1-DSEG.txt OFL-1.1-Doto.txt
        OFL-1.1-FontAwesome.README.txt OFL-1.1-FontAwesome.txt
        OFL-1.1-IBMPlexMono.txt OFL-1.1-IBMPlexSans.txt OFL-1.1-Inter.txt
        OFL-1.1-JetBrainsMono.txt OFL-1.1-Montserrat.txt OFL-1.1-NotoSans.txt
        OFL-1.1-Nunito.txt OFL-1.1-RubikGlitch.txt OFL-1.1-SourceHanSans.txt
        OFL-1.1-SpaceGrotesk.txt PSF-Python.txt PyInstaller-COPYING.txt
        README.md THIRD_PARTY_BUNDLED.md XZ-COPYING.txt pycryptodome-LICENSE.txt
        MPL-2.0-certifi.txt MPL-2.0.txt MIT-platformdirs.txt
        zlib-1.3.1-LICENSE-README.txt
        bundled/libBLT.copyright.txt bundled/libX11.copyright.txt
        bundled/libXau.copyright.txt bundled/libXdmcp.copyright.txt
        bundled/libXext.copyright.txt bundled/libXft.copyright.txt
        bundled/libXrender.copyright.txt bundled/libXss.copyright.txt
        bundled/libbrotli.copyright.txt bundled/libbsd.copyright.txt
        bundled/libbz2.copyright.txt bundled/libcap.copyright.txt
        bundled/libcrypto.copyright.txt windows/README.md windows/libusb-COPYING.txt
        windows/lzo-COPYING.txt windows/mingw-w64-crt-COPYING.MinGW-w64-runtime.txt
        windows/mingw-w64-crt-COPYING.MinGW-w64.txt windows/mingw-w64-crt-COPYING.txt
        windows/msys-libgcc-RUNTIME.LIBRARY.EXCEPTION.txt
        windows/msys2-runtime-BSD-NOTICES.txt windows/msys2-runtime-COPYING.LIB.txt
        windows/msys2-runtime-COPYING.NEWLIB.txt windows/msys2-runtime-COPYING.txt
        windows/msys2-runtime-CYGWIN_LICENSE.txt windows/ucrt64-libgcc-COPYING.RUNTIME.txt
        windows/winpthreads-COPYING.txt windows/zlib-LICENSE.txt
    """.split()
    _windows_sidecars += [f"licenses/{n}" for n in _license_files]
    _missing = [n for n in _windows_sidecars if not os.path.isfile(os.path.join(ROOT, n))]
    if _missing:
        raise SystemExit(f"REFUSING to build: missing Windows sidecars: {_missing}")
    datas += [(os.path.join(ROOT, n), os.path.dirname(n) or ".") for n in _windows_sidecars]
datas += [(os.path.join(payload, n), "payload") for n in _required_payload]
# Installer window assets: wordmark, display sheet (titles + kickers), hero disc, font licences
# (gui_theme.asset_dirs looks under <data root>/diskos_installer/assets).
_assets = os.path.join(ROOT, "diskos_installer", "assets")
_asset_files = ["wordmark.png", "wordmark@2x.png", "display.png", "display@1.5x.png", "display@2x.png",
                "display.json", "disc.png", "disc@1.5x.png", "disc@2x.png",
                "LICENSE-archivo-OFL.txt", "LICENSE-michroma-OFL.txt"]
_missing_assets = [n for n in _asset_files if not os.path.isfile(os.path.join(_assets, n))]
if _missing_assets:
    raise SystemExit(f"REFUSING to build: missing installer assets: {_missing_assets}")
datas += [(os.path.join(_assets, n), "diskos_installer/assets") for n in _asset_files]
libdir = os.path.join(vend, "lib")           # bundled dylibs (macOS), built by vendor/setup-macos.sh
if _os not in ("windows", "macos") and os.path.isdir(libdir):
    for fn in sorted(os.listdir(libdir)):
        if os.path.isfile(os.path.join(libdir, fn)):
            datas.append((os.path.join(libdir, fn), f"vendor/{TAG}/lib"))

# All freezes, including Linux/macOS and direct spec builds, verify raw blobs.
_blob_pairs = [(Path(payload) / n, 'payload/' + n) for n in PAYLOAD]
_blob_pairs += [(Path(ROOT) / 'flash' / n, 'flash/' + n) for n in DEVICE]
_blob_pairs += [(Path(vend) / n, 'flash/' + n) for n in DEVICE]
_blob_pairs += [(Path(_assets) / n, 'diskos_installer/assets/' + n) for n in _asset_files]
if _os == 'windows':
    # These two generated assets are authenticated separately with the keyring.
    _blob_pairs += [(Path(ROOT) / n, n) for n in _windows_sidecars
                    if not n.startswith('corresponding-source/windows/')]
BlobGuard(ROOT, capability.source_commit or 'HEAD').verify(_blob_pairs)

# Collect pycryptodome's runtime modules to AES-decrypt the FiiO OTA chunks.
# Its SelfTest suite is not needed by the frozen app.
from PyInstaller.utils.hooks import collect_data_files, collect_submodules
from diskos_installer.service import require_firmware_dependencies
require_firmware_dependencies()  # Refuse a frozen build with no working AES backend.
hidden = ["usb", "usb.core", "usb.backend.libusb1"] + collect_submodules(
    "Crypto", filter=lambda name: name != "Crypto.SelfTest" and not name.startswith("Crypto.SelfTest."))

# net_tls.build_context() loads certifi's cacert.pem on every platform; a freeze without it cannot
# verify TLS, so refuse to build one. The data lands at <data root>/certifi/cacert.pem.
_certifi_data = collect_data_files("certifi")
if not any(os.path.basename(src) == "cacert.pem" and dest.replace("\\", "/") == "certifi"
           for src, dest in _certifi_data):
    raise SystemExit("REFUSING to build: certifi's cacert.pem was not collected (pip install -r requirements.txt)")
datas += _certifi_data

cli_entry = os.path.join(ROOT, "diskos_installer", "__main__.py")
entries = [cli_entry]
if _os == "windows":
    gui_entry = os.path.join(ROOT, "diskos_installer", "gui_entry.py")
    entries.append(gui_entry)

if _os == "windows":
    def is_usb_backend_name(path):
        name = str(path).replace("\\", "/").rsplit("/", 1)[-1].lower()
        # hooks-contrib 2025.9 pyi_rth_usb.py matches <candidate>*.dll.
        # Include its Cygwin candidates and hook-usb.py's collection names.
        return name.endswith(".dll") and name.startswith(
            ("usb", "libusb", "openusb", "cygusb-1.0", "cygusb0"))

    def verify_usb_entry(dest, source, kind, binary_container=True):
        if not (is_usb_backend_name(dest) or is_usb_backend_name(source)):
            return False
        if (not binary_container or dest.lower() != "libusb-1.0.dll" or kind != "BINARY"
                or os.path.normcase(os.path.realpath(source)) !=
                   os.path.normcase(os.path.realpath(_libusb_dll))):
            raise SystemExit(f"REFUSING to build: unexpected USB backend DLL: {dest} <- {source}")
        return True

    # Analysis normalizes hook, ctypes and dependency additions before returning.
    # Inspect every raw TOC before equal-destination entries can be discarded.
    from PyInstaller.building import build_main as _build_main
    _original_normalize_toc = _build_main.normalize_toc

    def checked_normalize_toc(toc):
        toc = list(toc)
        for dest, source, kind in toc:
            verify_usb_entry(dest, source, kind)
        return _original_normalize_toc(toc)

    _build_main.normalize_toc = checked_normalize_toc

try:
    a = Analysis(
        entries,
        pathex=[ROOT],
        binaries=binaries,
        datas=datas,
        hiddenimports=hidden,
        hookspath=hookspath,
        runtime_hooks=[capability_hook],
        excludes=[],
        noarchive=False,
    )
finally:
    if _os == "windows":
        _build_main.normalize_toc = _original_normalize_toc

def is_unused_windows_tk_data(dest):
    name = str(dest).replace("\\", "/").lower()
    return (name in ("_tk_data/mkpsenc.tcl", "_tk_data/images")
            or name.startswith("_tk_data/images/"))


def filter_windows_tk_data(toc):
    # Tk 8.6.15: tk.tcl:507 needs icons.tcl even on Windows. Keep it.
    # mkpsenc is only autoloaded for PostScript; images/ contains demo logos.
    return [entry for entry in toc if not is_unused_windows_tk_data(entry[0])]


def verify_windows_tk_data(*tocs):
    for toc in tocs:
        for dest, source, kind in toc:
            if is_unused_windows_tk_data(dest):
                raise SystemExit(f"REFUSING to build: unused Tk data still collected: {dest}")


if _os == "windows":
    # The _tkinter hook adds full DATA destinations under _tk_data. Filter
    # only that TOC after Analysis; Tcl, Tk DLLs and other platforms stay intact.
    a.datas = filter_windows_tk_data(a.datas)
    verify_windows_tk_data(a.datas, a.binaries)

if _os == "windows":
    _libusb_entries = []
    for toc in (a.binaries, a.datas):
        for dest, source, kind in toc:
            if verify_usb_entry(dest, source, kind, binary_container=toc is a.binaries):
                _libusb_entries.append((dest, source, kind))
    if len(_libusb_entries) != 1:
        raise SystemExit("REFUSING to build: expected exactly one pinned libusb-1.0.dll in Analysis.binaries")
    verify_libusb_file(_libusb_dll, _libusb["dll_sha256"], _libusb["dll_size"])
pyz = PYZ(a.pure)
if _os == "windows":
    # Keep every runtime hook in each executable, followed by its sole entry.
    # Self-spawned children run the same hooks before private child dispatch.
    entry_paths = {os.path.normcase(os.path.abspath(path)) for path in entries}
    shared_scripts = [script for script in a.scripts
                      if os.path.normcase(os.path.abspath(script[1])) not in entry_paths]

    def scripts_for(entry):
        selected = [script for script in a.scripts
                    if os.path.normcase(os.path.abspath(script[1])) == os.path.normcase(entry)]
        if len(selected) != 1:
            raise ValueError(f"Expected exactly one frozen entry script: {entry}")
        return shared_scripts + selected

    exe = EXE(
        pyz, scripts_for(gui_entry), [], exclude_binaries=True, contents_directory="_internal",
        name="diskos-installer",
        debug=False, bootloader_ignore_signals=False, strip=False, upx=False,
        console=False,
    )
    cli_exe = EXE(
        pyz, scripts_for(cli_entry), [], exclude_binaries=True, contents_directory="_internal",
        name="diskos-installer-cli",
        debug=False, bootloader_ignore_signals=False, strip=False, upx=False,
        console=True,
    )
    executables = [exe, cli_exe]
    coll = COLLECT(
        *executables, a.binaries, a.datas,
        strip=False, upx=False, name="diskos-installer",
    )
    verify_windows_tk_data(coll.toc)
elif _os == "macos":
    exe = EXE(
        pyz, a.scripts, [], exclude_binaries=True,
        name="diskos-installer", debug=False, bootloader_ignore_signals=False,
        strip=False, upx=False, console=True, target_arch=_arch,
        codesign_identity="-", argv_emulation=False,
    )
    coll = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name="diskos-installer")
    app = BUNDLE(coll, name="diskOS Installer.app", bundle_identifier="org.diskos.installer",
                 info_plist={"CFBundleShortVersionString": __version__,
                             "NSHighResolutionCapable": True}, codesign_identity="-")
else:
    exe = EXE(
        pyz, a.scripts, a.binaries, a.datas, [],
        name="diskos-installer",
        debug=False, bootloader_ignore_signals=False, strip=False, upx=False,
        console=True,
    )
