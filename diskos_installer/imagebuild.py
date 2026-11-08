"""Build a flashable diskOS image from the user's OWN stock firmware.

Faithful Python port of the proven mkdiskos.sh / extract_stock_rootfs.sh logic:
  extract_stock_rootfs(zip) -> stock rootfs.squashfs  (byte-exact from FiiO's zip)
  build_image(stock, mq_ui, variant) -> diskos_<variant>.bin (76021760 bytes)

We NEVER ship FiiO's rootfs; the user supplies their official firmware zip and we
build locally. squashfs pack/unpack delegates to the bundled mksquashfs/unsquashfs
(reference tools) - we do not reimplement squashfs.
"""

import contextlib
import os
import re
import secrets
import stat
import subprocess
import sys
import zipfile

from . import bundle
from .buildfs import FileSystem, TreeFS, ArchiveFS, archive_extraction_fs
from .pseudofs import Archive
from .reporter import CLIReporter

IMG_SIZE = 100663296         # diskOS image size: 768 NAND blocks (~96 MiB) - fits v2.40's 88 MB rootfs and covers v2.09/v2.28. The NAND writer (my_write6) MUST be built for this many blocks (NLOGBLOCKS=768); the flasher checks the pinned writer's descriptor and dbg[6] so a mismatched writer can't silently truncate. Written to the start of the mtd2 rootfs partition (128 MB); the RO squashfs need not fill it.

# Known-good stock rootfs.squashfs, verified out-of-band (NOT trusting the in-zip OTA manifest,
# which an attacker could modify consistently). A tested firmware whose extracted rootfs does not
# match its pin is refused: this rejects a modified/tampered/corrupt rootfs before it is patched
# and flashed. Map: MAIN_OS_VER -> (sha256, size_bytes). Add a version's pin only after hashing an
# authentic copy of that firmware.
PINNED_ROOTFS = {
    "228": ("0ffd877bca2c69ddff9ca70f4494da0d9e580c18d0f587e2c6d9921f2db82bd2", 72957952),
    "209": ("f1e3c69fb0e88b923c135558e01f4387a661f68839c8118e8ad490bdc9fc74e6", 75919360),
    "240": ("b479e159db5134325819b5f6e5a54388f3adefae373a4ee60680f02d5dcf0bb8", 88420352),
    # V2.57: qualified 2026-09-28 on a real V2.57 Disc (installed-base probe x2, gate good/wrong-base/wrong-image,
    # production flash verified, first boot installed the embedded UI); fits the 768-block image.
    "257": ("111e4dd7ee3d7ff91ba7e61181690be7ffd22bd1bbd13ad513f5f015ffb302ae", 80596992),
}
# Firmware versions diskOS has been flash-tested against. Others have DIFFERENT command-tag
# meanings, so diskOS built on them can send wrong commands and misbehave/reboot.
# v2.40's 88 MB rootfs needs IMG_SIZE=96 MB + the 768-block writer (see IMG_SIZE note).
TESTED_FW = {"209", "228", "240", "257"}
# Firmware diskOS may be INSTALLED on in this release: the four stock versions it supports. Only their exact
# pinned images are accepted (validate_stock_rootfs(..., allow_override=False) on install/build), and there is no
# override. On the Disc the writer's base gate additionally proves the kernel + recovery are that version's.
# TESTED_FW governs validation of saved stock images for restore.
INSTALL_FW = {"209", "228", "240", "257"}


def require_installable(mver):
    """Refuse to build or install diskOS on any firmware outside INSTALL_FW. No override."""
    if mver not in INSTALL_FW:
        raise BuildError(
            f"firmware MAIN_OS_VER={mver or '?'} is not supported for installing diskOS (supported: "
            f"{', '.join('V' + v[0] + '.' + v[1:] for v in sorted(INSTALL_FW))}). Restoring stock is still supported.",
            code="E225")
SQUASH_MAGIC = b"hsqs"


def validate_stock_rootfs(stock_squashfs, rep=None, allow_override=True):
    """Validate that `stock_squashfs` is a genuine, supported, known-good Snowsky Disc rootfs -
    a SHARED gate called BEFORE the image is saved as the recovery copy, BEFORE build, and BEFORE
    every restore-flash (so a wrong-device or crafted rootfs can never be saved or flashed on the
    strength of a size/magic preflight alone). Raises BuildError (E220 not-a-Disc-rootfs / E221
    untested version / E224 hash mismatch). Returns the MAIN_OS_VER string. Only extracts the tiny
    version.in - cheap enough to run on every path. DISKOS_ALLOW_UNTESTED_FW=1 relaxes E221/E224."""
    rep = rep or CLIReporter()
    import hashlib, tempfile, shutil
    unsq = (_selected_squashfs_pair[1]
            if bundle.platform_probe.host()[0] == "linux" and _selected_squashfs_pair
            else bundle.native("unsquashfs"))
    with open(stock_squashfs, "rb") as f:
        if f.read(4) != SQUASH_MAGIC:
            raise BuildError("not a squashfs image (bad magic) - not a Snowsky Disc rootfs", code="E220")
    tmp = tempfile.mkdtemp(prefix="diskos-vchk-")
    try:
        r = _run([unsq, "-d", os.path.join(tmp, "x"), "-f", stock_squashfs,
                  "etc/product_version/version.in"], capture_output=True, text=True)
        ver_in = os.path.join(tmp, "x", "etc/product_version/version.in")
        if not os.path.exists(ver_in):
            # The file we need was NOT extracted (e.g. unsquashfs without LZO, or not a readable
            # rootfs) - report that plainly rather than mislabeling it as a wrong-PRODUCT rootfs
            # below. Keyed on the file's presence, not on returncode: unsquashfs can exit nonzero
            # (status 2) for a metadata-setting warning while still writing the file correctly.
            raise BuildError(
                f"could not extract version.in from the stock rootfs using '{unsq}': "
                f"{(r.stderr or r.stdout or '').strip()[:300]}", code="E230",
                action="check the firmware image and that unsquashfs supports LZO "
                       "(install squashfs-tools with LZO support, or use the bundled tools)")
        prod = _grep1(ver_in, r"PRODUCT=([A-Za-z0-9_]+)")
        mver = _grep1(ver_in, r"MAIN_OS_VER=([0-9]+)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return _validate_stock_version(stock_squashfs, prod, mver, rep, allow_override)


def _validate_stock_version(stock_squashfs, prod, mver, rep, allow_override):
    import hashlib
    # allow_override=False (install/build of this release): the pin is absolute - the override may only ever
    # relax validation of a saved stock image being RESTORED, never what diskOS gets built on or installed onto.
    override = allow_override and os.environ.get("DISKOS_ALLOW_UNTESTED_FW") == "1"
    if prod != "SNOWSKY_DISC":
        raise BuildError(f"not a Snowsky Disc rootfs (PRODUCT={prod!r})", code="E220")
    if mver not in TESTED_FW and not override:
        raise BuildError(
            f"firmware MAIN_OS_VER={mver or '?'} is not tested (supported: {', '.join(sorted(TESTED_FW))}). "
            "Other versions can have incompatible command meanings."
            + (" Re-run with DISKOS_ALLOW_UNTESTED_FW=1 at your own risk." if allow_override else ""),
            code="E221")
    pin = PINNED_ROOTFS.get(mver)
    if pin:
        exp_sha, exp_sz = pin
        got_sz = os.path.getsize(stock_squashfs)
        # The pin is over the EXACT extracted rootfs (exp_sz bytes). But the SAVED recovery copy is
        # those same bytes zero-PADDED to the partition size (_save_stock pads to IMG_SIZE), so this
        # gate is called with both the unpadded (install/build) and padded (restore) forms. Validate
        # the first exp_sz bytes against the pin and require every byte AFTER to be zero padding - so a
        # padded copy verifies identically to the original, while arbitrary appended data still fails.
        got_sha, tail_zero = None, False
        if exp_sz <= got_sz <= IMG_SIZE:
            with open(stock_squashfs, "rb") as f:
                h = hashlib.sha256(); remaining = exp_sz
                while remaining > 0:
                    chunk = f.read(min(1 << 20, remaining))
                    if not chunk:
                        break
                    h.update(chunk); remaining -= len(chunk)
                got_sha = h.hexdigest() if remaining == 0 else None
                tail_zero = True                       # anything past exp_sz must be pure zero padding
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    if chunk.strip(b"\x00"):
                        tail_zero = False; break
        if got_sha != exp_sha or not tail_zero:
            if override:
                rep.warning(f"stock rootfs hash {(got_sha or '?')[:12]}... != pinned V{mver} - proceeding (override set)")
            else:
                raise BuildError(
                    f"stock rootfs does not match the known-good V{mver} image (got "
                    f"{(got_sha or 'short/oversize')[:12]}..., expected {exp_sha[:12]}...). The firmware "
                    "may be modified, corrupt, or repackaged - re-download the official FiiO firmware."
                    + (" (Set DISKOS_ALLOW_UNTESTED_FW=1 at your own risk.)" if allow_override else ""),
                    code="E224")
        else:
            rep.log(f"stock rootfs matches the pinned known-good V{mver} image (sha256 verified)")
    elif mver in TESTED_FW:
        rep.warning(f"no pinned hash for V{mver} yet - rootfs authenticity is NOT verified against a pin")
    rep.log(f"stock rootfs OK: PRODUCT={prod} MAIN_OS_VER={mver or '?'}")
    return mver


from .errors import BuildError  # coded (E2xx); re-exported so imagebuild.BuildError still resolves


# --- safe zip extraction (reject traversal / symlink escape / bombs) ---------
def _safe_extract_member(zf, member, dest_root):
    name = member.filename
    if name.startswith("/") or os.path.isabs(name) or ".." in name.replace("\\", "/").split("/"):
        raise BuildError(f"unsafe path in zip: {name!r}", code="E202")
    target = os.path.realpath(os.path.join(dest_root, name))
    if not (target == os.path.realpath(dest_root) or
            target.startswith(os.path.realpath(dest_root) + os.sep)):
        raise BuildError(f"zip entry escapes extraction dir: {name!r}", code="E202")
    return target


# FiiO chunks the rootfs and wraps each chunk (and the manifest) in AES-256-CBC
# (openssl -pbkdf2) under the fixed key "fo123" (their reused OTA string -
# obfuscation, not protection). We decrypt + concatenate in index order. We only
# READ the image; no signature is involved.
_OTA_KEY = "fo123"


def _openssl_aes_decrypt(data, password):
    """Replicate `openssl enc -d -aes-256-cbc -pbkdf2 -k <password>`:
    'Salted__' + 8-byte salt header, PBKDF2-HMAC-SHA256 (10000 iters) -> 32B key +
    16B IV, AES-256-CBC, PKCS7 padding."""
    import hashlib
    from Crypto.Cipher import AES  # pycryptodome (bundled)
    if data[:8] != b"Salted__":
        raise BuildError("encrypted blob missing openssl 'Salted__' header (not a FiiO OTA chunk?)", code="E211")
    salt = data[8:16]
    ct = data[16:]
    if len(ct) == 0 or len(ct) % 16 != 0:
        raise BuildError("encrypted blob has bad length (truncated chunk?)", code="E211")
    keyiv = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 10000, 48)
    pt = AES.new(keyiv[:32], AES.MODE_CBC, keyiv[32:48]).decrypt(ct)
    pad = pt[-1] if pt else 0
    if pad < 1 or pad > 16 or pt[-pad:] != bytes([pad]) * pad:
        raise BuildError("bad PKCS7 padding after AES decrypt (wrong key or firmware?)", code="E211")
    return pt[:-pad]


def extract_stock_rootfs(fw_zip, out_squashfs, workdir, rep=None):
    """Pull the exact stock rootfs.squashfs out of FiiO's official update zip by
    decrypting + reassembling the main_os OTA chunks. Byte-exact; written only
    after it validates (manifest size + squashfs magic), so a failure never
    clobbers the output."""
    rep = rep or CLIReporter()
    rep.phase("Extracting stock firmware")
    if sys.platform == 'darwin' and os.path.isdir(fw_zip):
        raise BuildError('Safari unpacked the firmware ZIP; the recorded checksum requires the original ZIP.',
                         code='E201', action='In Safari, right-click the download link and choose Download Linked File, '
                                             'then select the .zip; or use Get firmware')
    if not zipfile.is_zipfile(fw_zip):
        raise BuildError(f"not a zip archive: {fw_zip}", code="E201")

    ex = os.path.join(workdir, "fw_unzip")
    import shutil
    if os.path.isdir(ex):                 # fresh every time - never mix two firmwares' chunks
        shutil.rmtree(ex, ignore_errors=True)
    os.makedirs(ex)
    total_uncompressed = 0
    with zipfile.ZipFile(fw_zip) as zf:
        infos = zf.infolist()
        if len(infos) > 20000:            # per-entry count bound (not just aggregate size)
            raise BuildError("zip has an implausible number of entries - refusing.", code="E202")
        for m in infos:
            total_uncompressed += m.file_size
            if total_uncompressed > 4 * (1 << 30):   # 4 GiB bomb guard
                raise BuildError("zip expands beyond 4 GiB - refusing (possible zip bomb)", code="E202")
        rep.status("Unpacking firmware zip")
        for i, m in enumerate(infos):
            tgt = _safe_extract_member(zf, m, ex)
            if m.is_dir():
                os.makedirs(tgt, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(tgt), exist_ok=True)
                with zf.open(m) as src, open(tgt, "wb") as dst:
                    while True:
                        chunk = src.read(1 << 20)
                        if not chunk:
                            break
                        dst.write(chunk)
            rep.progress(i + 1, len(infos))

    # locate the ONE main_os OTA manifest (refuse ambiguity)
    import glob
    mans = sorted(glob.glob(os.path.join(ex, "**", "main_os", "ota_v*", "ota_update.in.enc"),
                            recursive=True))
    if len(mans) == 0:
        raise BuildError("no main_os/ota_v*/ota_update.in.enc in this zip - not a Disc "
                         "main-OS firmware?", code="E210")
    if len(mans) > 1:
        raise BuildError(f"{len(mans)} main_os manifests in this zip - ambiguous, aborting.", code="E210")
    man_enc = mans[0]
    ota_dir = os.path.dirname(man_enc)
    rep.log(f"OTA dir: {os.path.relpath(ota_dir, ex)}")

    manifest = _openssl_aes_decrypt(open(man_enc, "rb").read(), _OTA_KEY).decode("utf-8", "ignore")
    img_name, img_size = _parse_rootfs_manifest(manifest)
    if not img_name:
        raise BuildError("no rootfs image in the OTA manifest.", code="E210")
    # img_name must be a bare basename (no path separators / traversal)
    if img_name != os.path.basename(img_name) or img_name in ("", ".", "..") or "/" in img_name or "\\" in img_name:
        raise BuildError(f"OTA manifest rootfs image name is not a safe basename: {img_name!r}", code="E210")
    if img_size is not None:
        if not img_size.isdigit() or not (0 < int(img_size) <= 256 * (1 << 20)):
            raise BuildError(f"OTA manifest img_size is implausible: {img_size!r}", code="E210")
    rep.log(f"rootfs image={img_name} expected_size={img_size or '?'}")

    # order the $img.NNNN.enc chunks by numeric index (skip ota_sha256_* etc.);
    # reject DUPLICATE indices and require a contiguous 0..N-1 sequence.
    by_idx = {}
    for f in glob.glob(os.path.join(ota_dir, glob.escape(img_name) + ".*.enc")):
        rest = os.path.basename(f)[len(img_name) + 1:]      # "NNNN.<hash>.enc"
        idx = rest.split(".", 1)[0]
        if not idx.isdigit():
            continue
        i = int(idx)
        if i in by_idx:
            raise BuildError(f"duplicate rootfs chunk index {i} in the OTA dir - refusing.", code="E212")
        by_idx[i] = f
    if not by_idx:
        raise BuildError(f"no {img_name}.NNNN.*.enc chunks found in the OTA dir.", code="E212")
    idxs = sorted(by_idx)
    if idxs != list(range(len(idxs))):
        raise BuildError(f"rootfs chunk indices are not a contiguous 0..{len(idxs)-1} sequence "
                         f"(got {idxs[:3]}...{idxs[-1]}) - missing chunk, refusing.", code="E212")
    chunks = [(i, by_idx[i]) for i in idxs]

    tmpout = os.path.join(workdir, "rootfs.assembled")
    rep.status("Decrypting + assembling rootfs")
    with open(tmpout, "wb") as out:
        for i, (_idx, f) in enumerate(chunks):
            out.write(_openssl_aes_decrypt(open(f, "rb").read(), _OTA_KEY))
            rep.progress(i + 1, len(chunks))

    got = os.path.getsize(tmpout)
    rep.log(f"assembled {len(chunks)} chunks ({got} bytes)")
    if img_size and got != int(img_size):
        raise BuildError(f"assembled size {got} != manifest {img_size} (missing/dup chunk?)", code="E213")
    with open(tmpout, "rb") as f:
        if f.read(4) != SQUASH_MAGIC:
            raise BuildError("assembled output is not a squashfs (bad magic) - wrong key/firmware.", code="E213")

    os.replace(tmpout, out_squashfs)
    rep.ok(f"stock rootfs extracted -> {out_squashfs} ({got} bytes)")
    return out_squashfs


def _parse_rootfs_manifest(text):
    """From the decrypted ota_update.in, return (img_name, img_size) for the block
    whose img_type=rootfs (mirrors the awk in extract_stock_rootfs.sh)."""
    in_rootfs = False
    name = size = None
    for line in text.splitlines():
        line = line.strip()
        if line == "img_type=rootfs" or line.endswith("=rootfs") and line.startswith("img_type"):
            in_rootfs = True
            continue
        if in_rootfs:
            if line.startswith("img_name="):
                name = line[len("img_name="):]
            elif line.startswith("img_size="):
                size = line[len("img_size="):]
            if name and size:
                break
    return name, size


def _copyfile(src, dst):
    with open(src, "rb") as s, open(dst, "wb") as d:
        while True:
            b = s.read(1 << 20)
            if not b:
                break
            d.write(b)


# --- fiio_init.sh boot-hook patch (python-native, single-match-or-refuse) ----
_OLD_IF = 'if [ "$COREDUMP_FLAG" == "1" ]; then'
_LAUNCH = _OLD_IF + "\n    /usr/data/mq_ui &"
# Read-only launcher (OTA builds only): hashes /usr/data/mq_ui against what S97 verified this boot, copies the very
# bytes it hashed into a private in-RAM file and execs that (closes the verify-then-exec race); walks trial -> good ->
# good.prev -> flashed -> stock in-process. Source: src/launcher/diskos_launch.c (build/build-launcher.sh).
#
# NO CHANGE FOR USERS WHO NEVER UPDATE: the boot path they run is exactly the v1.1.3 one (`/usr/data/mq_ui &` in the
# hook). The launcher is used only when S97 selected a signed, non-flashed build for THIS boot (BUILD=trial|good|prev
# in /tmp/.diskos_run, which S97 writes before the hook runs). The OTA additions are two clearly delimited pieces
# (helpers, and one extra leading branch); removing them and turning that branch's `elif` back into `if` gives the
# v1.1.3 hook byte for byte.
_LAUNCHER = "/opt/diskos/bin/diskos-launch"
_LAUNCH_RUN = _LAUNCHER + " /usr/data/mq_ui"
_LAUNCH_CMD = "diskos_ui_start &"
_WATCH_NAMES = 'PROCESS_MQ_UI="mq_ui"; PROCESS_MQ_PLAYER="mq_player"'
_NORMAL_RUN_SECS = 30           # a UI that ran this long is left to the stock watchdog when it exits
_HASH_BOUND_SECS = 10           # the flashed-UI hash check in the fallback must finish by then, else stock starts
_STARTUP_BOUND_SECS = 15        # the OTA launch must have produced a running diskOS UI by then, else the supervisor acts
_OTA_HELPERS_BEGIN = "# --- diskOS OTA helpers (used only when S97 selected a signed non-flashed build) ---\n"
_OTA_HELPERS_END = "# --- end OTA helpers ---\n"
_OTA_BRANCH_BEGIN = "# --- OTA branch: only when S97 selected a signed non-flashed build for this boot ---\n"
_OTA_BRANCH_END = "# --- end OTA branch ---\n"
_OTA_HELPERS = (
    _OTA_HELPERS_BEGIN +
    "diskos_ota_build() {\n"
    "    [ -f /tmp/.diskos_run ] && [ ! -L /tmp/.diskos_run ] && [ -x " + _LAUNCHER + " ] || return 1\n"
    "    _b=$(grep '^BUILD=' /tmp/.diskos_run 2>/dev/null | head -n 1 | cut -d= -f2)\n"
    "    case \"$_b\" in trial|good|prev) return 0;; esac\n"
    "    return 1\n"
    "}\n"
    # Startup supervisor. The launcher runs in the background under the watched name mq_ui and walks its ladder
    # itself; if it has NOT produced a running diskOS UI (its watcher writes /tmp/.diskos_launched only after a
    # successful exec) within the bound, and it is not already the stock UI, the supervisor REVOKES the launch token
    # (the launcher checks it right before every exec), kills only the launcher WITHOUT waiting for it (a process in
    # uninterruptible I/O may not die at once), and starts the fallback immediately: the READ-ONLY flashed diskOS if it
    # matches the RO manifest, else the stock UI. Nothing here touches mq_player or /usr/data/mq_ui.
    # The manifest read and the hash run in a background subshell with a hard deadline and are NEVER waited for: a
    # stalled read means "not verified" and the caller launches stock at once.
    "diskos_flashed_ok() {\n"
    "    rm -f /tmp/.diskos_flashed_chk /tmp/.diskos_flashed_chk.tmp\n"
    "    (\n"
    "        _m=$(grep '^SHA256=' /etc/diskos_manifest 2>/dev/null | head -n 1 | cut -d= -f2)\n"
    "        _h=$(sha256sum /opt/diskos/mq_ui 2>/dev/null | cut -d' ' -f1)\n"
    "        if [ -n \"$_m\" ] && [ \"$_h\" = \"$_m\" ]; then echo ok; else echo no; fi > /tmp/.diskos_flashed_chk.tmp\n"
    "        mv -f /tmp/.diskos_flashed_chk.tmp /tmp/.diskos_flashed_chk\n"
    "    ) &\n"
    "    _hp=$!\n"
    "    _hw=0\n"
    "    while [ \"$_hw\" -lt " + str(_HASH_BOUND_SECS) + " ] && [ ! -f /tmp/.diskos_flashed_chk ]; do\n"
    "        sleep 1\n"
    "        _hw=$((_hw + 1))\n"
    "    done\n"
    "    _r=$(cat /tmp/.diskos_flashed_chk 2>/dev/null)\n"
    "    kill -9 \"$_hp\" 2>/dev/null\n"
    "    rm -f /tmp/.diskos_flashed_chk /tmp/.diskos_flashed_chk.tmp\n"
    "    [ \"$_r\" = ok ]\n"
    "}\n"
    "diskos_ui_start() {\n"
    "    _u0=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)\n"
    "    : > /tmp/.diskos_launch_token\n"
    "    " + _LAUNCH_RUN + " &\n"
    "    _lp=$!\n"
    "    _w=0\n"
    "    _k=0\n"
    "    while [ \"$_w\" -lt " + str(_STARTUP_BOUND_SECS) + " ]; do\n"
    "        [ -f /tmp/.diskos_launched ] && break\n"
    "        kill -0 \"$_lp\" 2>/dev/null || break\n"
    "        sleep 1\n"
    "        _w=$((_w + 1))\n"
    "    done\n"
    "    if [ ! -f /tmp/.diskos_launched ] && kill -0 \"$_lp\" 2>/dev/null; then\n"
    "        case \"$(readlink /proc/$_lp/exe 2>/dev/null)\" in\n"
    "            /usr/bin/mq_ui) ;;\n"
    "            *) rm -f /tmp/.diskos_launch_token; kill -9 \"$_lp\" 2>/dev/null; _k=1 ;;\n"
    "        esac\n"
    "    fi\n"
    "    [ \"$_k\" = 0 ] && wait \"$_lp\" 2>/dev/null\n"
    "    rm -f /tmp/.diskos_launch_token\n"
    "    _u1=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)\n"
    "    [ \"$_k\" = 0 ] && [ \"$((_u1 - _u0))\" -ge " + str(_NORMAL_RUN_SECS) + " ] && return 0\n"
    "    if diskos_flashed_ok; then\n"
    "        PATH=/opt/diskos:$PATH mq_ui\n"
    "        _u2=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)\n"
    "        [ \"$((_u2 - _u1))\" -ge " + str(_NORMAL_RUN_SECS) + " ] && return 0\n"
    "    fi\n"
    "    mq_ui\n"
    "}\n" +
    _OTA_HELPERS_END
)
_GUARD_PATH_LINE = "export PATH=/opt/diskos/bin:$PATH"
# S96 writes exactly "diskos\n" or "stock\n". Only a POSITIVE record launches diskOS: missing, partial or
# malformed (selector hung, crashed, timed out or never ran) means stock. The record is read ONLY through
# the shared helper (payload/diskos-selected), which rejects a non-regular file before opening it, so a FIFO
# at the path cannot block the boot hook (short of the path being swapped between its check and its read);
# if the helper itself is missing the test fails, which is stock.
_SELECTED_HELPER = "/opt/diskos/bin/diskos-selected"
_BOOTPROBE = "/opt/diskos/bin/diskos-bootprobe"
# Artwork helper: the UI runs it to make covers (stock V2.57's ffmpeg has no image decoders). Not part of the boot
# decision, but shipped and validated the same way: exact bytes, a regular file, mode 0755.
_ARTDEC = "/opt/diskos/bin/diskos-artdec"
_SUPPORT_FILES = ((_ARTDEC.lstrip("/"), "diskos-artdec"),)
_SELECT_LINE = _SELECTED_HELPER
# (image path, payload name) of every file the boot's stock/diskOS decision runs through
_BOOT_DECISION_FILES = (
    ("etc/init.d/S96diskos_select", "S96diskos_select"),
    ("etc/init.d/S97diskos_install", "S97diskos_install"),
    (_SELECTED_HELPER.lstrip("/"), "diskos-selected"),
    (_BOOTPROBE.lstrip("/"), "diskos-bootprobe"),
    (_LAUNCHER.lstrip("/"), "diskos-launch"),
    ("opt/diskos/bin/diskos-flashstate", "diskos-flashstate"),
)
_IF_LINE = "if " + _SELECT_LINE + " && [ -f /tmp/.diskos_ready ] && [ -f /usr/data/mq_ui ] && [ -f /usr/data/mq_player ]"
_OTA_BRANCH = (
    _OTA_BRANCH_BEGIN +
    _IF_LINE + " && diskos_ota_build; then\n"
    "    " + _WATCH_NAMES + "\n"
    "    " + _LAUNCH_CMD + "\n    sleep 2\n    /usr/data/mq_player &\n" +
    _OTA_BRANCH_END
)
# Naming runs after selection/install and before the one initial Avahi start.
# All supported fixtures have these exact stock bytes; refuse a new boot order.
_STOCK_AVAHI = b'#!/bin/sh\n#\n# avahi-daemon init script\n\nDAEMON=/usr/sbin/avahi-daemon\ncase "$1" in\n    start)\n\t$DAEMON -c || $DAEMON -D\n\t;;\n    stop)\n\t$DAEMON -c && $DAEMON -k\n\t;;\n    reload)\n\t$DAEMON -c && $DAEMON -r\n\t;;\n    *)\n\techo "Usage: S50avahi-daemon {start|stop|reload}" >&2\n\texit 1\n\t;;\nesac\n'
_DEFERRED_AVAHI = _STOCK_AVAHI.replace(
    b'\t$DAEMON -c || $DAEMON -D',
    b'\t# Defer boot startup; retain later stock stop/start behavior.\n'
    b'\t[ -d /run/diskos-avahi-ready ] || exit 0\n'
    b'\t$DAEMON -c || $DAEMON -D')
_LATE_AVAHI = _STOCK_AVAHI.replace(
    b'\t$DAEMON -c || $DAEMON -D',
    b'\tmkdir -p /run/diskos-avahi-ready\n'
    b'\t$DAEMON -c || $DAEMON -D')
_NAMES_FILES = (
    ("etc/init.d/S97diskos_names", "S97diskos_names"),
    ("opt/diskos/bin/diskos-names", "diskos-names"),
    ("opt/diskos/netbin/udhcpc", "diskos-udhcpc"),
)
_NAMES_DECL = "opt/diskos/network-names-v1"
_NAMES_PATH = (
    'unset DISKOS_NETWORK_NAMES\n'
    '# Boot snapshot only; descendants and both watchdog branches inherit this.\n'
    'if /opt/diskos/bin/diskos-selected && /opt/diskos/bin/diskos-names --active; then\n'
    '    export DISKOS_NETWORK_NAMES=v1\n'
    '    export PATH=/opt/diskos/netbin:$PATH\n'
    'fi\n'
)

_BLOCK = (
    "# diskOS SD guard: the stock player runs 'umount /tmp/sdcard; rm -rf /tmp/sdcard' without checking\n"
    "# the umount; on a busy (mounted) card that empties the card. /opt/diskos/bin/rm refuses card\n"
    "# operands. Exported HERE (before any launch) so the initial launch, BOTH watchdog respawn\n"
    "# branches and the stock fallback all inherit it.\n"
    + _GUARD_PATH_LINE + "\n" + _NAMES_PATH
    + _OTA_HELPERS +
    "# READINESS GATE: /tmp/.diskos_ready is written by S97 only when it COMPLETED and left a verified\n"
    "# override in place. It is on tmpfs, so it means 'this boot'. Requiring it means a first-boot hook\n"
    "# that hung, was killed, or could not quarantine a bad install falls back to STOCK - the decision no\n"
    "# longer depends on S97 having successfully deleted anything from NAND.\n"
    "# STOCK SELECTION: S96 sampled the boot preference and the Vol-Up override once, before any diskOS\n"
    "# code ran, and recorded the result. Only a positive 'diskos' record launches diskOS, so reaching the\n"
    "# stock firmware never depends on diskOS - or on the selector itself - completing.\n"
    + _OTA_BRANCH +
    "el" + _IF_LINE + "; then\n"
    "    # diskOS override: run our UI + the player from /usr/data (persists across\n"
    "    # rootfs flashes).  Falls back to the stock rootfs binaries if either is absent.\n"
    "    /usr/data/mq_ui &\n    sleep 2\n    /usr/data/mq_player &\n"
    # The stock hook has a SECOND launch site: when its coredump flag is set it runs the /usr/data
    # binaries directly. On a diskOS device those are ours, so that branch was a way to launch diskOS
    # with the readiness gate bypassed entirely. It keeps its original meaning (launch from /usr/data)
    # but only when readiness says so; otherwise it falls through to the bare names, which are stock.
    'elif ' + _SELECT_LINE + ' && [ -f /tmp/.diskos_ready ] && [ "$COREDUMP_FLAG" == "1" ]; then'
)


# The pristine stock fiio_init.sh the boot-hook patch was designed and tested against, hashed as RAW BYTES.
# It is byte-identical on V1.95, V2.09 (from the pin-verified stock image), V2.28, V2.40 and V2.57. Any
# other script is refused outright - there is no override for this one - because the patch's safety rests
# on the script's exact structure, and no text heuristic can establish shell control flow (the previous
# heuristic was bypassed three different ways). A new firmware with a different script needs its own
# reviewed entry here.
KNOWN_FIIO_INIT_SHA256 = {"ad8455ad0a3d9577260419a06ad2e0802c24bc6bb8e9ed4b64637b7bd968ccf2"}

# The only forms in which a patched script may mention the diskOS (/usr/data) binaries, after strip().
_USR_DATA_ALLOWED = (
    'PROCESS_MQ_UI="/usr/data/mq_ui"',
    'PROCESS_MQ_PLAYER="/usr/data/mq_player"',
    '/usr/data/mq_ui &',
    '/usr/data/mq_player &',
    _LAUNCH_CMD,
    _LAUNCH_RUN + " &",
)


def _validate_boot_hook(fi):
    """Sanity-check a PATCHED fiio_init.sh (bytes or str). Raises BuildError (E232).

    This is NOT the safety proof and does not claim to establish shell control flow. The guarantee is:
    _patch_fiio_init only ever patches the one pinned stock script (KNOWN_FIIO_INIT_SHA256, raw bytes), and
    the output image must contain exactly the bytes that patch produced (checked by the caller). These
    checks catch gross corruption early and with a readable message: strict UTF-8, no carriage returns,
    exactly one patch, the SD-guard PATH export first, the generated block intact, and no mention of the
    /usr/data binaries outside the known forms or outside a gated condition.
    """
    def bad(msg):
        raise BuildError("repacked image: fiio_init.sh " + msg + " - do NOT flash", code="E232")
    if isinstance(fi, (bytes, bytearray)):
        try:
            fi = bytes(fi).decode("utf-8")
        except UnicodeDecodeError:
            bad("is not valid UTF-8")
    # A lone CR is invisible to line-splitting tools but not to the shell: it can fold a following
    # command into a comment. The known script has none, so any CR is corruption.
    if "\r" in fi:
        bad("contains a carriage return")
    if fi.count("diskOS override") != 1:
        bad("does not contain exactly one diskOS boot-hook patch")
    g = fi.find(_GUARD_PATH_LINE)
    if g < 0 or g > fi.find("diskOS override"):
        bad("lacks the SD-guard PATH export before the launch block")
    if fi.count(_OTA_BRANCH) != 1 or fi.count(_WATCH_NAMES) != 1:
        bad("does not contain exactly one OTA branch (gated on diskos_ota_build)")
    if fi.count(_OTA_HELPERS) != 1 or fi.find(_OTA_HELPERS) > fi.find("diskOS override"):
        bad("lacks the OTA helper functions before the launch block")
    if fi.count(_BLOCK + "\n    /usr/data/mq_ui &") != 1:
        bad("does not contain the generated launch block followed by the branch it replaced")
    lines = fi.splitlines()
    for i, ln in enumerate(lines):
        st = ln.strip()
        if st.startswith("#") or ("/usr/data/mq_ui" not in st and "/usr/data/mq_player" not in st):
            continue
        if st in _USR_DATA_ALLOWED:
            continue
        if st.startswith("if " + _SELECT_LINE + " && [ -f /tmp/.diskos_ready ] && [ -f /usr/data/mq_ui ]") \
                or st.startswith("elif " + _SELECT_LINE + " && [ -f /tmp/.diskos_ready ] && [ -f /usr/data/mq_ui ]"):
            continue
        bad(f"line {i + 1} mentions the /usr/data binaries in an unrecognised form: {st[:80]!r}")
    launches = 0
    in_fn = False
    for i, ln in enumerate(lines):
        st = ln.strip()
        if st in ("diskos_ui_start() {", "diskos_ota_build() {"):
            in_fn = True
            continue
        if in_fn:
            in_fn = st != "}"
            continue                  # the function body is the checked launcher call itself; its CALL sites are gated below
        if st.startswith("#") or not (st.startswith("/usr/data/mq_ui") or st.startswith("/usr/data/mq_player")
                                      or st.startswith("diskos_ui_start")):
            continue
        launches += 1
        cond = None
        for j in range(i - 1, -1, -1):
            pj = lines[j].strip()
            if pj.startswith("if ") or pj.startswith("elif "):
                cond = pj
                break
            if pj in ("else", "fi") or pj.startswith("else "):
                break          # the nearest enclosing test is not a positive condition
        if cond is None or _SELECT_LINE not in cond or "[ -f /tmp/.diskos_ready ]" not in cond:
            bad(f"line {i + 1} launches /usr/data binaries without the stock selection and readiness "
                f"gate in its own condition")
    if launches < 2:
        bad("has no gated /usr/data launch (unexpected structure)")


def _check_decision_file(path, payload_path, name, fs=None, mode=0o755):
    """One boot-decision file in the output image: a regular file (lstat - never a symlink), permission
    bits exactly as requested (no setuid/setgid/sticky), and byte-identical to the payload."""
    import stat as _st
    if fs is None:
        fs, path = TreeFS(os.path.dirname(path)), os.fsencode(os.path.basename(path))
    m = fs.lstat(path).st_mode
    if not _st.S_ISREG(m) or _st.S_IMODE(m) != mode:
        raise BuildError(f"repacked image: {name} is not a regular {mode:04o} file (mode {oct(m)}) - do NOT flash",
                         code="E232")
    with open(payload_path, "rb") as b:
        if fs.read_bytes(path) != b.read():
            raise BuildError(f"repacked image: {name} differs from the shipped payload - do NOT flash", code="E232")


def _check_boot_script_output(actual, expected):
    """The output-image check for the boot script, on RAW BYTES: sanity checks, then exact equality with
    the bytes the build wrote (which _patch_fiio_init only ever derives from the one pinned script)."""
    _validate_boot_hook(actual)
    if actual != expected:
        raise BuildError("repacked image: fiio_init.sh differs from the patched script the build wrote - "
                         "do NOT flash", code="E232")


def _patched_boot_bytes(raw):
    """Derive the boot hook from pinned raw bytes, independent of storage."""
    import hashlib
    sha = hashlib.sha256(raw).hexdigest()
    if sha not in KNOWN_FIIO_INIT_SHA256:
        raise BuildError(f"fiio_init.sh (sha256 {sha[:12]}...) is not the stock boot script the diskOS boot "
                         "hook was built and tested against - refusing to patch this firmware.", code="E223")
    s = raw.decode("utf-8")       # the known script is valid UTF-8; strict, never lossy
    if "diskOS override" in s:    # cannot happen for the known hash; kept as a belt-and-braces refusal
        raise BuildError("fiio_init.sh already contains a diskOS marker - refusing to patch it again.", code="E223")
    n = s.count(_LAUNCH)
    if n != 1:
        raise BuildError(
            "boot-hook anchor (COREDUMP launch block) "
            f"{'not found' if n == 0 else 'ambiguous'} in fiio_init.sh - "
            "incompatible firmware boot structure; do not ship this base untested.", code="E223")
    i = s.index(_LAUNCH)
    s = s[:i] + _BLOCK + s[i + len(_OLD_IF):]
    return s.encode("utf-8")


def _patch_fiio_init(path, rep):
    # Compatibility entry point for tree callers; the rule lives above.
    from pathlib import Path
    p = Path(path)
    fs = TreeFS(p.parent)
    target = fs.resolve(os.fsencode(p.name))
    fs.replace_file(target, _patched_boot_bytes(fs.read_bytes(target)))


# --- over-the-air update trust anchor (Tier A; contract: update-system/OTA_CONTRACT.md) ------------------------
# The read-only rootfs carries the OFFLINE ROOT PUBLIC KEY, the anti-downgrade floors and the verifier script.
# /etc/diskos-ota/{root.pub.pem, epochs, diskos-verify.sh}. Without a root key the image is built with OTA OFF
# (nothing is written, and S97/the UI treat the install as "updates not supported"): a public build never
# trusts a key nobody chose. Key sources, first hit wins: the build_image() argument, $DISKOS_OTA_ROOTPUB,
# payload/diskos-root.pub.pem. Floors: argument, environment, then defaults below.
OTA_DIR = "etc/diskos-ota"
DEFAULT_MIN_KEY_EPOCH = 1
# Match the UI release epoch shipped by this installer.
DEFAULT_MIN_UI_EPOCH = 8
_P256_SPKI_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")   # SubjectPublicKeyInfo, id-ecPublicKey/prime256v1, uncompressed point
_OTA_FILES = (("root.pub.pem", 0o644), ("epochs", 0o644), ("diskos-verify.sh", 0o644))


def _check_rootpub_pem(raw):
    """A root key must be exactly one PEM PUBLIC KEY holding a P-256 SubjectPublicKeyInfo (91 bytes DER).
    Structural check only (no OpenSSL needed on the user's machine); the device verifier does the crypto."""
    import base64
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError:
        raise BuildError("OTA root key is not ASCII PEM", code="E240")
    lines = [ln.strip() for ln in text.strip().splitlines()]
    if len(lines) < 3 or lines[0] != "-----BEGIN PUBLIC KEY-----" or lines[-1] != "-----END PUBLIC KEY-----":
        raise BuildError("OTA root key must be a single 'BEGIN PUBLIC KEY' PEM block (no private key, no chain)", code="E240")
    try:
        der = base64.b64decode("".join(lines[1:-1]), validate=True)
    except Exception:
        raise BuildError("OTA root key PEM body is not valid base64", code="E240")
    if len(der) != 91 or not der.startswith(_P256_SPKI_PREFIX):
        raise BuildError("OTA root key is not an ECDSA P-256 public key", code="E240")
    return raw


def _epoch(val, name):
    v = str(val).strip()
    if not v.isdigit() or len(v) > 15:
        raise BuildError(f"{name} must be a non-negative integer (got {val!r})", code="E241")
    return int(v)


def resolve_ota_config(rootpub=None, min_key_epoch=None, min_ui_epoch=None, no_ota=False):
    """-> None (OTA disabled: no root key configured, or no_ota) or {'rootpub': bytes, 'epochs': bytes}.
    no_ota is absolute: it ignores $DISKOS_OTA_ROOTPUB and the payload key, and refuses an explicit rootpub."""
    if no_ota:
        if rootpub:
            raise BuildError("--no-ota and an OTA root key (--ota-key) contradict each other", code="E242")
        return None
    path = rootpub or os.environ.get("DISKOS_OTA_ROOTPUB") or bundle.data("diskos-root.pub.pem", required=False)
    if not path:
        return None
    try:
        with open(path, "rb") as f:
            raw = f.read(8192)
    except OSError as exc:
        raise BuildError(f"cannot read the OTA root key {path}: {exc}", code="E240")
    _check_rootpub_pem(raw)
    if min_key_epoch is None:
        min_key_epoch = os.environ.get("DISKOS_MIN_KEY_EPOCH", DEFAULT_MIN_KEY_EPOCH)
    if min_ui_epoch is None:
        min_ui_epoch = os.environ.get("DISKOS_MIN_UI_EPOCH", DEFAULT_MIN_UI_EPOCH)
    ke, ue = _epoch(min_key_epoch, "MIN_KEY_EPOCH"), _epoch(min_ui_epoch, "MIN_UI_EPOCH")
    return {"rootpub": raw, "epochs": f"MIN_KEY_EPOCH={ke}\nMIN_UI_EPOCH={ue}\n".encode("ascii")}


def _install_ota(rf, cfg):
    _edit_ota(TreeFS(rf), cfg)


def _edit_ota(fs, cfg):
    d = OTA_DIR.encode("ascii")
    _edit_within(fs, d)
    fs.mkdir(d)
    for name, mode in _OTA_FILES:
        dst = d + b"/" + name.encode("ascii")
        _edit_within(fs, dst)
        if name == "diskos-verify.sh":
            _edit_install(fs, bundle.data(name), dst, mode)
        else:
            fs.replace_file(dst, cfg["rootpub"] if name == "root.pub.pem" else cfg["epochs"], mode)


def _check_ota_output(dst, cfg):
    """Output-image check for the OTA files: present iff configured, exact bytes, regular 0644 files."""
    import stat as _st
    fs = dst if isinstance(dst, FileSystem) else TreeFS(dst)
    d = OTA_DIR.encode("ascii")
    if cfg is None:
        try:
            fs.lstat(d)
        except FileNotFoundError:
            return
        raise BuildError("repacked image: unexpected /etc/diskos-ota although OTA is not configured - do NOT flash", code="E232")
    _edit_within(fs, d, code="E232")
    with open(bundle.data("diskos-verify.sh"), "rb") as source:
        want = {"root.pub.pem": cfg["rootpub"], "epochs": cfg["epochs"],
                "diskos-verify.sh": source.read()}
    for name, mode in _OTA_FILES:
        p = d + b"/" + name.encode("ascii")
        _edit_within(fs, p, code="E232")
        try:
            m = fs.lstat(p).st_mode
            ok = _st.S_ISREG(m) and _st.S_IMODE(m) == mode and fs.read_bytes(p) == want[name]
        except OSError:
            ok = False
        if not ok:
            raise BuildError(f"repacked image: OTA file {name} missing or differs from what the build wrote - do NOT flash", code="E232")


def _install_sd_guards(rf):
    return _edit_sd_guards(TreeFS(rf))


def _edit_sd_guards(fs):
    """Replace the stock rm entry, never its BusyBox target or an outside path."""
    import hashlib, stat
    _edit_within(fs, b"bin")
    _edit_within(fs, b"bin/busybox")
    if not stat.S_ISREG(fs.lstat(b"bin/busybox").st_mode):
        raise BuildError("stock /bin/busybox is not a regular file", code="E233")
    before = hashlib.sha256(fs.read_bytes(b"bin/busybox")).hexdigest()
    # Only rm may have an outside final symlink. Check its parent and unlink
    # the entry without resolving that final component, including hardlinks.
    try:
        mode = fs.lstat(b"bin/rm").st_mode
    except FileNotFoundError:
        mode = None
    if mode is not None:
        if stat.S_ISDIR(mode):
            raise BuildError("stock /bin/rm is a directory", code="E233")
        fs.unlink(b"bin/rm", follow_final=False)
    src = bundle.data("diskos-rmguard")
    _edit_install(fs, src, b"bin/rm", 0o755)
    _edit_install(fs, src, b"opt/diskos/bin/rm", 0o755)
    if hashlib.sha256(fs.read_bytes(b"bin/busybox")).hexdigest() != before:
        raise BuildError("BusyBox changed while installing the SD guard", code="E233")
    return before


def _validate_sd_guards(rf, expected_busybox_sha=None):
    import hashlib, stat
    from pathlib import Path
    fs = rf if isinstance(rf, FileSystem) else TreeFS(rf)
    expected = Path(bundle.data("diskos-rmguard")).read_bytes()
    for rel in ("bin/rm", "opt/diskos/bin/rm"):
        path = rel.encode("ascii")
        _edit_within(fs, path, code="E232")
        try:
            mode = fs.lstat(path).st_mode
            if not stat.S_ISREG(mode) or not mode & 0o111:
                raise OSError("not an executable regular file")
            if fs.read_bytes(path) != expected:
                raise OSError("content mismatch")
        except OSError as exc:
            raise BuildError(f"repacked image: invalid SD guard {rel}: {exc} - do NOT flash", code="E232") from exc
    if expected_busybox_sha is not None:
        path = b"bin/busybox"
        _edit_within(fs, path, code="E232")
        if (not stat.S_ISREG(fs.lstat(path).st_mode)
                or hashlib.sha256(fs.read_bytes(path)).hexdigest() != expected_busybox_sha):
            raise BuildError("repacked image: BusyBox differs from stock - do NOT flash", code="E232")


# --- ELF sanity for the UI binary -------------------------------------------
def _validate_ui_elf(ui_path):
    if not os.path.exists(ui_path):
        raise BuildError(f"UI binary not found: {ui_path}", code="E222")
    with open(ui_path, "rb") as f:
        hdr = f.read(20)
    if hdr[:4] != b"\x7fELF":
        raise BuildError(f"'{ui_path}' is not an ELF", code="E222")
    if hdr[4:6] != b"\x01\x01":
        raise BuildError(f"'{ui_path}' is not ELF32 little-endian (EI_CLASS/DATA)", code="E222")
    if hdr[18:20] != b"\x08\x00":
        raise BuildError(f"'{ui_path}' e_machine is not MIPS-LE", code="E222")


def _run(cmd, msys_paths=(), **kw):
    # cmd[0] is the resolved tool path; native_env tailors the loader path to whether
    # that tool is bundled (gets vendor/lib) or a system copy (clean env).
    env = bundle.native_env(cmd[0])
    if bundle.platform_probe.host()[0] == "windows" and msys_paths:
        cmd = list(cmd)
        for index in msys_paths:
            cmd[index] = bundle.msys_path(cmd[index])
    return subprocess.run(cmd, env=env, **kw)


# A capability-qualified Linux fallback belongs to this process run. Keep it out
# of bundle.native: native resolution must continue to expose broken vendor files.
_selected_squashfs_pair = None


def _squashfs_tools():
    if bundle.platform_probe.host()[0] == "linux" and _selected_squashfs_pair:
        return _selected_squashfs_pair
    unsq = bundle.native("unsquashfs")
    return bundle.native("mksquashfs"), unsq


def _check_squashfs_pair(mksq, unsq, probe, rep):
    global _selected_squashfs_pair
    linux = bundle.platform_probe.host()[0] == "linux"
    if linux and _selected_squashfs_pair:
        mksq, unsq = _selected_squashfs_pair
    try:
        if linux and bundle.is_vendored(mksq) != bundle.is_vendored(unsq):
            raise BuildError("squashfs LZO capability check failed: incomplete bundled pair",
                             code="E104", action="install squashfs-tools with LZO support, or "
                             "repair/rebuild both bundled tools with " + bundle.native_build_script("squashfs"))
        probe(mksq, unsq)
    except BuildError as original:
        if (original.code != "E104" or not linux or
                not any(bundle.is_vendored(p) for p in (mksq, unsq))):
            raise
        # Explicit retry AFTER capability failure; never combine tool origins.
        pair = tuple(bundle._which_abs(name) for name in ("mksquashfs", "unsquashfs"))
        vendor_real = os.path.realpath(bundle.vendor_dir())
        if not all(pair) or any(
                bundle.is_vendored(p) or
                os.path.commonpath((os.path.realpath(p), vendor_real)) == vendor_real
                for p in pair):
            raise
        try:
            probe(*pair)
        except (BuildError, OSError, ValueError, subprocess.SubprocessError):
            raise original from None  # preserve the original message and repair hint
        _selected_squashfs_pair = pair
        (rep or CLIReporter()).log(
            f"Bundled squashfs capability check failed ({original}); "
            f"using system squashfs-tools pair for this run: {pair[0]} + {pair[1]}")


def check_squashfs_tools(mksq, unsq, rep=None):
    """Check LZO capability, explicitly selecting a Linux system pair on failure."""
    _check_squashfs_pair(mksq, unsq,
                         lambda mk, un: _probe_squashfs_tools(mk, un, rep), rep)


def _probe_squashfs_tools(mksq, unsq, rep=None):
    """Prove the resolved squashfs tools can actually PACK and EXTRACT with the exact
    stock LZO parameters, BEFORE the real build touches the firmware. A system mksquashfs
    built without LZO (or an otherwise broken tool) fails here with a clear, actionable
    message instead of a cryptic mid-build error. Applies to bundled and system tools
    alike. A failure is reported as an LZO CAPABILITY failure (it can also be caused by
    disk space, permissions, resource limits or missing libraries), never asserted as
    'built without LZO'. Raises BuildError E104 on any failure."""
    import tempfile, filecmp
    _script = bundle.native_build_script("squashfs")
    _fixhint = ("diskOS requires working LZO compression and extraction; install squashfs-tools "
                f"with LZO support, or repair/rebuild the bundled tools with {_script} from the "
                "diskOS source")

    def _fail(stage, path, detail):
        detail = (detail or "").strip()[:300]
        raise BuildError(
            f"squashfs LZO capability check failed during {stage} using '{path}'"
            + (f": {detail}" if detail else ""), code="E104", action=_fixhint)

    try:
        with tempfile.TemporaryDirectory(prefix="diskos-sqcheck-") as td:
            src = os.path.join(td, "src")
            os.makedirs(src)
            payload = os.path.join(src, "probe.bin")
            # compressible, multi-block content (> one 128 KiB block) so the LZO data path runs
            with open(payload, "wb") as f:
                f.write(b"diskos-lzo-probe-0123456789abcdef" * 8192)   # ~256 KiB
            sqfs = os.path.join(td, "probe.sqfs")
            ext = os.path.join(td, "ext")

            try:
                pack = _run([mksq, src, sqfs, "-comp", "lzo", "-b", "131072", "-no-xattrs",
                             "-all-root", "-noappend", "-processors", "1"],
                            capture_output=True, text=True, timeout=120)
            except (OSError, subprocess.TimeoutExpired) as e:
                _fail("pack", mksq, str(e))
            if pack.returncode != 0 or not os.path.exists(sqfs):
                _fail("pack", mksq, (pack.stderr or "") + (pack.stdout or ""))
            try:
                unp = _run([unsq, "-processors", "1", "-d", ext, sqfs],
                           capture_output=True, text=True, timeout=120)
            except (OSError, subprocess.TimeoutExpired) as e:
                _fail("extract", unsq, str(e))
            if unp.returncode != 0:
                _fail("extract", unsq, (unp.stderr or "") + (unp.stdout or ""))
            out = os.path.join(ext, "probe.bin")
            if not os.path.exists(out) or not filecmp.cmp(payload, out, shallow=False):
                raise BuildError(
                    "squashfs LZO capability check failed: pack/extract round-trip bytes differ",
                    code="E104", action=_fixhint)
    except OSError as e:
        # the scratch dir / payload write / compare / cleanup failed (e.g. ENOSPC, EACCES) -
        # translate to a coded, actionable error rather than an uncoded OSError.
        raise BuildError(
            f"squashfs LZO capability check could not run: {e}", code="E104",
            action="check free disk space and temp-dir permissions and retry; " + _fixhint)
    if rep:
        rep.log("squashfs LZO pack/extract check: OK")


# --- case-sensitive scratch space (macOS) ------------------------------------
# The stock Snowsky Disc rootfs is built on Linux and contains filenames that differ
# ONLY by case in the same directory (e.g. main_1/back_home.png and main_1/BACK_HOME.png) -
# fine on a case-sensitive filesystem, but macOS's default APFS volume is case-INsensitive
# (case-preserving), so the second unsquashfs write collides with the first ("already
# exists") and the extraction fails deterministically, every time, on stock Mac setups.
def _is_case_sensitive(dir_path):
    """Probe whether dir_path's filesystem folds case. Uses a PRIVATE, exclusively-created
    temp subdirectory so it can never clobber a real file, be fooled by an unrelated
    differently-cased file, or race a concurrent probe. Raises OSError on I/O or permission
    failure - callers must NOT treat that as 'case-sensitive'."""
    import tempfile, shutil
    os.makedirs(dir_path, exist_ok=True)
    probe = tempfile.mkdtemp(prefix=".diskos-cs-", dir=dir_path)
    try:
        with open(os.path.join(probe, "csprobe"), "w") as f:
            f.write("x")
        # case-insensitive fs: the uppercase alias resolves back to the file we just wrote.
        # Use os.stat (not os.path.exists, which swallows EIO etc. and would misreport a failing
        # filesystem as case-sensitive): only a real absence -> case-sensitive; any other error
        # propagates so the caller can fail closed (E234).
        try:
            os.stat(os.path.join(probe, "CSPROBE"))
            return False
        except FileNotFoundError:
            return True
    finally:
        shutil.rmtree(probe, ignore_errors=True)


def _hdiutil(args, what, timeout=180):
    """Run one hdiutil subcommand, mapping a missing tool, a timeout, or a nonzero exit to E234."""
    try:
        r = subprocess.run(["hdiutil", *args], capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError:
        raise BuildError("hdiutil not found - a case-sensitive scratch volume is required to "
                         "build on a case-insensitive macOS filesystem.", code="E234")
    except subprocess.TimeoutExpired:
        raise BuildError(f"failed to {what}: hdiutil timed out after {timeout}s", code="E234")
    if r.returncode != 0:
        raise BuildError(f"failed to {what}: {(r.stderr or r.stdout).strip()[:400]}", code="E234")
    return r


def _scratch_detach(mnt, rep):
    """Detach the scratch volume at mnt and CONFIRM it is gone. Bounded retries, escalating to
    -force, so a transiently-busy volume never leaves a live mount the caller then 'succeeds'
    over. Warns (never silently accepts) if it truly cannot be detached. Returns True if unmounted."""
    import time
    for i in range(6):
        if not os.path.ismount(mnt):
            return True
        try:
            subprocess.run(["hdiutil", "detach", mnt] + (["-force"] if i >= 3 else []),
                           capture_output=True, text=True, timeout=60)
        except FileNotFoundError:
            break
        except subprocess.TimeoutExpired:
            pass   # count this as a failed attempt; the loop retries and then escalates to -force
        if not os.path.ismount(mnt):
            return True
        time.sleep(0.4 * (i + 1))
    if os.path.ismount(mnt):
        rep.warning(f"the case-sensitive scratch volume at {mnt} could not be detached; run "
                    "'hdiutil detach' on it before deleting the build directory.")
        return False
    return True


@contextlib.contextmanager
def _case_sensitive_extract_root(workdir, rep):
    """Yield a directory the case-colliding stock rootfs can be unsquashfs'd into safely.

    If workdir is already on a case-sensitive filesystem (Linux, or a case-sensitive macOS
    volume) yield it unchanged - a true no-op. Otherwise, on macOS only, create a FRESH
    case-sensitive APFS sparse image, mount it under workdir, VERIFY it, yield the mountpoint,
    and ALWAYS detach + delete it afterwards. Fails closed (E234) on any hdiutil error, a
    case-insensitive non-macOS host, or a scratch volume that does not verify - never an unsafe
    fallback extraction."""
    try:
        cs = _is_case_sensitive(workdir)
    except OSError as e:
        raise BuildError(f"could not probe the build filesystem at '{workdir}': {e}", code="E234")
    if cs:
        yield workdir
        return
    if sys.platform != "darwin":
        raise BuildError(
            f"'{workdir}' is on a case-insensitive filesystem, which cannot hold this rootfs "
            "(it has files that differ only by case). Rebuild on a case-sensitive filesystem.",
            code="E234")

    base = os.path.join(workdir, ".cs-scratch")
    img = base + ".sparseimage"
    mnt = base
    # Never let a planted symlink redirect create / attach / delete.
    for p in (img, mnt):
        if os.path.islink(p):
            raise BuildError(f"refusing a symlinked scratch path: {p}", code="E234")
    # A prior interrupted build can leave a stale mount/image. Detach a stale mount (verified) and
    # drop a stale image so we always build on a FRESH, verified volume. Fail closed if a stale
    # mount will not detach - do NOT reuse it or delete anything under it.
    if os.path.ismount(mnt) and not _scratch_detach(mnt, rep):
        raise BuildError(f"a previous scratch volume at {mnt} is still mounted; unmount it and retry.",
                         code="E234")
    if os.path.exists(img):
        try:
            os.remove(img)
        except OSError as e:
            raise BuildError(f"could not remove a stale scratch image {img}: {e}", code="E234")

    rep.log("build directory is on a case-insensitive filesystem - creating a case-sensitive "
            "APFS scratch volume for extraction")
    try:
        # create is INSIDE the cleanup scope, so a partial image from a failed/interrupted
        # create is removed by the finally below.
        _hdiutil(["create", "-size", "2g", "-type", "SPARSE", "-fs", "Case-sensitive APFS",
                  "-volname", "diskOS-build", base], "create the case-sensitive scratch volume")
        os.makedirs(mnt, exist_ok=True)
        _hdiutil(["attach", img, "-mountpoint", mnt, "-nobrowse", "-owners", "on"],
                 "mount the case-sensitive scratch volume")
        if not os.path.ismount(mnt):
            raise BuildError("the case-sensitive scratch volume did not mount as expected", code="E234")
        try:
            if not _is_case_sensitive(mnt):
                raise BuildError("the scratch volume is not case-sensitive as requested", code="E234")
        except OSError as e:
            raise BuildError(f"the scratch volume is not usable: {e}", code="E234")
        yield mnt
    finally:
        _scratch_detach(mnt, rep)
        # Remove the backing image only once the volume is actually detached.
        if not os.path.ismount(mnt):
            try:
                if os.path.exists(img):
                    os.remove(img)
            except OSError as e:
                rep.warning(f"could not remove scratch image {img}: {e}")
            try:
                if os.path.isdir(mnt):
                    os.rmdir(mnt)
            except OSError:
                pass


def _validate_squashfs_output(sq_path, unsq, expect_ui_sha, expect_ui_sz, extract_root, rep=None, expected_busybox_sha=None,
                              expected_fiio_init=None, ota_cfg=None, flash_id=None):
    """Validate a freshly-repacked squashfs by its CONTENT, not the repacker's exit status. Does a
    COMPLETE independent extraction (so silent corruption ANYWHERE fails, not just in two files),
    then verifies every boot-critical artefact: the boot-hook patch in fiio_init.sh, the executable
    first-boot installer hook, the manifest (agreeing with the embedded UI), and the embedded UI
    itself (exact sha256 + size + exec bit). Raises BuildError (E232) on any problem."""
    import hashlib, tempfile, shutil
    with open(sq_path, "rb") as f:
        if f.read(4) != b"hsqs":
            raise BuildError("repacked image is not a valid squashfs (bad superblock magic) - "
                             "the repacker produced a corrupt file", code="E232")
    # This is a FULL re-extraction of the same rootfs (same case-colliding filenames as the
    # original unpack), so it needs the same case-sensitive destination - never the system tmp
    # dir, which on macOS lives on the same case-insensitive volume as everything else.
    tmp = tempfile.mkdtemp(prefix="diskos-sqcheck-", dir=extract_root)
    try:
        dst = os.path.join(tmp, "x")
        # FULL extraction (no file subset): a corrupt inode / metadata block / file anywhere in the
        # bootable filesystem makes this fail, which two-file extraction would miss.
        r = _run([unsq, "-d", dst, "-f", sq_path], capture_output=True, text=True)
        if r.returncode != 0:
            raise BuildError(f"repacked image failed full extraction (unsquashfs rc={r.returncode}) - "
                             f"corrupt/truncated repack: {r.stderr.strip()[:200]}", code="E232")

        validate_image(TreeFS(dst), expect_ui_sha, expect_ui_sz, expected_busybox_sha,
                       expected_fiio_init, ota_cfg, flash_id)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if rep is not None:
        rep.log("output squashfs validated (full extraction + boot hook + S97 + manifest + UI hash/mode)")


@contextlib.contextmanager
def _export_archive(sq_path, unsq, spool_root, code="E232"):
    """Independent complete strict export; consume every regular inode before success."""
    import tempfile
    with contextlib.ExitStack() as stack:
        try:
            td = stack.enter_context(tempfile.TemporaryDirectory(
                prefix="diskos-pfcheck-", dir=spool_root))
            pf = os.path.join(td, "output.pf")
            r = _run([unsq, "-processors", "1", "-strict-errors", "-pf", pf, sq_path],
                     msys_paths=(5, 6), capture_output=True, text=True, timeout=300)
            if r.returncode:
                raise ValueError(f"unsquashfs rc={r.returncode}: {(r.stderr or '').strip()[:200]}")
            source = stack.enter_context(open(pf, "rb"))
            archive = stack.enter_context(Archive(source))
            for e in archive.entries.values():
                if e[0] == b"R":
                    for _ in archive.chunks(e[6], e[5]):
                        pass
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            raise BuildError(f"squashfs pseudo export failed: {exc}", code=code) from exc
        yield archive


def validate_archive_output(sq_path, unsq, expect_ui_sha, expect_ui_sz, spool_root,
                            expected_busybox_sha=None, expected_fiio_init=None, ota_cfg=None,
                            intended_archive=None, flash_id=None):
    """Check contents and, when supplied, every intended inode after pack transforms."""
    try:
        sqsz = os.path.getsize(sq_path)
        if sqsz > IMG_SIZE:
            raise BuildError(
                f"squashfs is {sqsz} > {IMG_SIZE} partition - refusing (truncating would "
                "make it unbootable). Trim content or use a smaller UI.", code="E231")
        with open(sq_path, "rb") as source:
            if source.read(4) != SQUASH_MAGIC:
                raise BuildError("repacked image is not a valid squashfs (bad superblock magic) - "
                                 "the repacker produced a corrupt file", code="E232")
    except OSError as exc:
        raise BuildError(f"repacked image could not be read: {exc}", code="E232") from exc
    with _export_archive(sq_path, unsq, spool_root) as archive:
        validate_image(ArchiveFS(archive), expect_ui_sha, expect_ui_sz, expected_busybox_sha,
                       expected_fiio_init, ota_cfg, flash_id)
        if intended_archive is not None:
            try:
                expected = _packed_manifest(intended_archive)
                actual = _packed_manifest(archive, intended=False)
            except (OSError, ValueError) as exc:
                raise BuildError(f"repacked archive could not be compared: {exc} - do NOT flash",
                                 code="E232") from exc
            if expected != actual:
                changed = sorted(p for p in expected.keys() | actual.keys()
                                 if expected.get(p) != actual.get(p))
                raise BuildError(f"repacked archive differs from intended image: {changed[:5]!r}",
                                 code="E232")


def _packed_manifest(archive, intended=True):
    result = archive.manifest()
    for path, (meta, detail, links, attrs) in result.items():
        if meta[0] == b"D":
            meta[1] = 0  # Host directory edits change mtimes; retain every other field.
        if intended:
            meta[3:5] = [0, 0]  # -all-root
            result[path] = (meta, detail, links, {})  # -no-xattrs
    return result


def validate_stock_archive(stock_squashfs, unsq, spool_root, rep=None, allow_override=True):
    """Archive lookup with the same stock product/version/pin policy as the tree path."""
    rep = rep or CLIReporter()
    try:
        with open(stock_squashfs, "rb") as source:
            if source.read(4) != SQUASH_MAGIC:
                raise BuildError("not a squashfs image (bad magic) - not a Snowsky Disc rootfs", code="E220")
        with _export_archive(stock_squashfs, unsq, spool_root, code="E230") as archive:
            fs = ArchiveFS(archive)
            prod = _fs_grep1(fs, b"etc/product_version/version.in", r"PRODUCT=([A-Za-z0-9_]+)")
            mver = _fs_grep1(fs, b"etc/product_version/version.in", r"MAIN_OS_VER=([0-9]+)")
    except (OSError, ValueError) as exc:
        raise BuildError(f"could not read version.in from the stock rootfs: {exc}", code="E230") from exc
    return _validate_stock_version(stock_squashfs, prod, mver, rep, allow_override)


def check_archive_squashfs_tools(mksq, unsq, spool_root, rep=None):
    """Check pseudo support with the same explicit Linux pair fallback policy."""
    _check_squashfs_pair(mksq, unsq,
                         lambda mk, un: _probe_archive_squashfs_tools(mk, un, spool_root, rep), rep)


def _probe_archive_squashfs_tools(mksq, unsq, spool_root, rep=None):
    """Opt-in pseudo pack/export capability check with stock LZO parameters."""
    import tempfile
    from .pseudofs import MARK
    payload = b"diskos-lzo-probe-0123456789abcdef" * 8192
    try:
        with tempfile.TemporaryDirectory(prefix="diskos-pfprobe-", dir=spool_root) as td:
            empty, pf, sq = (os.path.join(td, name) for name in ("empty", "input.pf", "probe.sq"))
            os.mkdir(empty)
            with Archive(b"/ D 1 755 0 0\n" + MARK) as archive:
                archive.file(b"probe.bin", payload)
                with open(pf, "wb") as output:
                    archive.write(output)
            r = _run([mksq, empty, sq, "-pf", pf, "-comp", "lzo", "-b", "131072",
                      "-no-xattrs", "-all-root", "-noappend", "-processors", "1"],
                     msys_paths=(1, 2, 4), capture_output=True, text=True, timeout=120)
            if r.returncode:
                raise ValueError(f"mksquashfs rc={r.returncode}: {(r.stderr or '').strip()[:200]}")
            with _export_archive(sq, unsq, td, code="E104") as result:
                if ArchiveFS(result).read_bytes(b"probe.bin") != payload:
                    raise ValueError("pack/export round-trip bytes differ")
    except (OSError, ValueError, subprocess.SubprocessError, BuildError) as exc:
        raise BuildError(f"squashfs LZO pseudo capability check failed: {exc}", code="E104",
                         action="repair/rebuild squashfs tools with working LZO and pseudo-file support") from exc
    if rep is not None:
        rep.log("squashfs LZO pseudo capability check passed")


def _fs_sha256(fs, path):
    import hashlib
    digest = hashlib.sha256()
    for block in fs.iter_bytes(path):
        digest.update(block)
    return digest.hexdigest()


def _fs_grep1(fs, path, pattern):
    m = re.search(pattern, fs.read_bytes(path).decode("utf-8", errors="ignore"))
    return m.group(1) if m else None


def validate_image(fs, expect_ui_sha, expect_ui_sz, expected_busybox_sha=None,
                   expected_fiio_init=None, ota_cfg=None, flash_id=None):
    """The single content validator for independently unpacked output."""
    try:
        _validate_image_contents(fs, expect_ui_sha, expect_ui_sz, expected_busybox_sha,
                                 expected_fiio_init, ota_cfg, flash_id)
    except (OSError, ValueError) as exc:
        raise BuildError(f"repacked image content could not be read: {exc} - do NOT flash", code="E232") from exc


def _validate_image_contents(fs, expect_ui_sha, expect_ui_sz, expected_busybox_sha,
                             expected_fiio_init, ota_cfg, flash_id):
    def _need(rel, what):
        p = rel.encode("ascii")
        _edit_within(fs, p, code="E232")   # a crafted rootfs must not escape via a symlink
        try:
            fs.stat(p)
        except FileNotFoundError:
            raise BuildError(f"repacked image is missing {what} ({rel}) - do NOT flash", code="E232")
        return p

    _check_flash_id(flash_id, code="E232")
    identity = _need("etc/diskos-flash-id", "flash identity")
    info = fs.lstat(identity)
    if not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) != 0o644:
        raise BuildError("repacked image: flash identity must be a regular file with mode 0644 - "
                         "do NOT flash", code="E232")
    if fs.read_bytes(identity) != flash_id:
        raise BuildError("repacked image: flash identity mismatch - do NOT flash", code="E232")

    # boot hook present + patched
    fi = fs.read_bytes(_need("usr/project/fiio_init.sh", "boot script"))                 # RAW bytes: no newline translation, no lossy decoding
    if expected_fiio_init is None:
        raise BuildError("internal: output validation needs the patched boot script bytes", code="E232")
    _check_boot_script_output(fi, expected_fiio_init)
    sel = _need("etc/init.d/S96diskos_select", "boot selector")
    if not (fs.stat(sel).st_mode & 0o111):
        raise BuildError("repacked image: S96diskos_select is not executable", code="E232")
    # exact identity, not just existence: the boot-time scripts are what decide stock vs diskOS
    # every file the stock/diskOS decision runs through: exact bytes, a regular file (not a symlink),
    # and permission bits exactly 0755 - a non-executable helper would silently force every boot to stock
    for rel, name in _BOOT_DECISION_FILES + _SUPPORT_FILES + _NAMES_FILES:
        _check_decision_file(_need(rel, name), bundle.data(name), name, fs)
    _check_decision_file(_need(_NAMES_DECL, "network naming declaration"),
                         bundle.data("diskos-network-names-v1"), "network naming declaration", fs, mode=0o644)
    for rel, expected in (("etc/init.d/S50avahi-daemon", _DEFERRED_AVAHI),
                          ("etc/init.d/S97zavahi", _LATE_AVAHI)):
        path = _need(rel, "ordered Avahi start")
        info = fs.lstat(path)
        if (not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) != 0o755
                or fs.read_bytes(path) != expected):
            raise BuildError("repacked image: Avahi boot order differs - do NOT flash", code="E232")
    _validate_sd_guards(fs, expected_busybox_sha)
    _check_ota_output(fs, ota_cfg)
    # first-boot installer hook present + executable
    s97 = _need("etc/init.d/S97diskos_install", "first-boot installer hook")
    if not (fs.stat(s97).st_mode & 0o111):
        raise BuildError("repacked image: S97diskos_install is not executable", code="E232")
    # embedded UI: exact identity + exec bit
    ui = _need("opt/diskos/mq_ui", "embedded UI")
    if fs.stat(ui).st_size != expect_ui_sz:
        raise BuildError(f"repacked image: embedded UI size {fs.stat(ui).st_size} != {expect_ui_sz}", code="E232")
    if _fs_sha256(fs, ui) != expect_ui_sha:
        raise BuildError("repacked image: embedded UI hash mismatch (repack corrupted it) - do NOT flash", code="E232")
    if not (fs.stat(ui).st_mode & 0o111):
        raise BuildError("repacked image: embedded UI is not executable", code="E232")
    # manifest present + agrees with the embedded UI (the on-device hook trusts it)
    man = _need("etc/diskos_manifest", "diskOS manifest")
    if (_fs_grep1(fs, man, r"SHA256=([0-9a-fA-F]+)") != expect_ui_sha
            or _fs_grep1(fs, man, r"SIZE=([0-9]+)") != str(expect_ui_sz)):
        raise BuildError("repacked image: manifest/UI mismatch - do NOT flash", code="E232")


def _edit_within(fs, path, code="E233"):
    if isinstance(fs, TreeFS):
        _assert_within_rf(str(fs.root), str(fs._path(path)), code=code)
    try:
        fs.resolve(path)
    except (ValueError, OSError) as exc:
        raise BuildError(f"refusing to write outside the rootfs: {path!r}: {exc}", code=code) from exc


def _edit_install(fs, src, dst, mode):
    _edit_within(fs, dst)
    with open(src, "rb") as source:
        fs.replace_file(dst, source, mode)


def _check_flash_id(flash_id, code):
    """Require the build's expected bytes; never invent an identity during checks."""
    if not isinstance(flash_id, bytes) or re.fullmatch(rb"[0-9a-f]{32}\n", flash_id) is None:
        raise BuildError("internal: flash identity must be 32 lowercase hex digits plus LF", code=code)


def edit_image(fs, ui_binary, variant, ota_cfg, rep=None, flash_id=None):
    """The single image edit phase. Paths are image bytes, never host paths."""
    import hashlib, time
    if variant not in ("public", "dev"):
        raise BuildError(f"variant must be 'public' or 'dev', got {variant!r}", code="E250")
    _check_flash_id(flash_id, code="E233")
    try:
        identity = b"etc/diskos-flash-id"
        _edit_within(fs, identity)
        fs.replace_file(identity, flash_id, 0o644)
        boot = b"usr/project/fiio_init.sh"
        _edit_within(fs, boot)
        patched = _patched_boot_bytes(fs.read_bytes(boot))
        fs.replace_file(fs.resolve(boot), patched)
        for rel, name in _BOOT_DECISION_FILES + _NAMES_FILES:
            _edit_install(fs, bundle.data(name), rel.encode("ascii"), 0o755)
        _edit_install(fs, bundle.data("diskos-network-names-v1"), _NAMES_DECL.encode("ascii"), 0o644)
        avahi = b"etc/init.d/S50avahi-daemon"
        _edit_within(fs, avahi)
        if fs.read_bytes(avahi) != _STOCK_AVAHI:
            raise BuildError("unrecognized stock Avahi init script", code="E223")
        _edit_within(fs, b"etc/init.d/S97zavahi")
        fs.replace_file(b"etc/init.d/S97zavahi", _LATE_AVAHI, 0o755)
        fs.replace_file(avahi, _DEFERRED_AVAHI, 0o755)
        if ota_cfg:
            _edit_ota(fs, ota_cfg)
        for rel, name in _SUPPORT_FILES:
            _edit_install(fs, bundle.data(name), rel.encode("ascii"), 0o755)
        # Guard the actual /bin entry too: default PATH and stock fallbacks must
        # remain protected even when the custom UI/NAND override is unavailable.
        busybox_sha = _edit_sd_guards(fs)
        with open(ui_binary, "rb") as source:
            ui_sha = hashlib.sha256(source.read()).hexdigest()
        ui_sz = os.path.getsize(ui_binary)
        manifest = b"etc/diskos_manifest"
        _edit_within(fs, manifest)
        fs.replace_file(manifest,
                        (f"SHA256={ui_sha}\nSIZE={ui_sz}\nARCH=mips-le\nVARIANT={variant}\n"
                         f"BUILT={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(fs.clock))}\n").encode("ascii"))
        # Embed the UI INSIDE the rootfs at a fixed path the S97 hook installs from FIRST (SD is only a
        # fallback source). This makes first boot need no SD card: flash -> reboot -> diskOS. The hook
        # still verify_ui's this copy against the manifest above, so a corrupt flash can't run it.
        _edit_install(fs, ui_binary, b"opt/diskos/mq_ui", 0o755)
        # Debug-access tooling (BOTH variants): the diskos-debug helper + a static dropbear. mq_ui's
        # "Debug Mode" toggle drives these to start SSH (random per-enable password) and/or the USB
        # serial shell on demand. Shipping them in the public image too means a normal user can enable
        # debug access from the UI without needing the dev build.
        for name in ("dropbearmulti", "diskos-debug.sh"):
            _edit_install(fs, bundle.data(name), b"usr/project/" + name.encode("ascii"), 0o755)
        if variant == "dev":
            # Dev only: an ALWAYS-ON USB serial recovery shell (builds the gadget + attaches the one
            # diskos-debug shell at boot), so a dev build is reachable over USB even before the UI runs.
            _edit_install(fs, bundle.data("S99usbserial"), b"etc/init.d/S99usbserial", 0o755)
        return patched, busybox_sha, ui_sha, ui_sz
    except ValueError as exc:
        raise BuildError(f"refusing to edit crafted or corrupt rootfs: {exc}", code="E233") from exc


def describe_ota(cfg):
    """One line saying which OTA state an image built with this resolve_ota_config() result will have."""
    if cfg is None:
        return "OFF (no OTA root key baked; the image never accepts in-app updates)"
    import hashlib
    fp = hashlib.sha256(cfg["rootpub"]).hexdigest()[:16]
    return f"ON (accepts updates signed by the baked root key, key sha256 {fp}...)"


def build_image(stock_squashfs, ui_binary, variant, out_bin, workdir, rep=None, ota_rootpub=None,
                min_key_epoch=None, min_ui_epoch=None, no_ota=False):
    """Build diskos_<variant>.bin from a stock rootfs + the diskOS UI."""
    rep = rep or CLIReporter()
    if variant not in ("public", "dev"):
        raise BuildError(f"variant must be 'public' or 'dev', got {variant!r}", code="E250")
    host = bundle.platform_probe.host()[0]
    if host not in ("linux", "macos", "windows"):
        # e.g. an MSYS2/Cygwin Python on Windows: POSIX tree extraction onto NTFS and
        # MSYS-parented tools are both unqualified. Use native Windows Python instead.
        from .errors import PreflightError
        raise PreflightError(f"image building is not supported on host {bundle.platform_probe.host_tag()}",
                             code="E101", action="run the installer with native Windows, Linux or macOS Python")
    rep.phase(f"Building diskOS image ({variant})")
    ota_cfg = resolve_ota_config(ota_rootpub, min_key_epoch, min_ui_epoch, no_ota)     # BEFORE any heavy work: a bad key fails fast
    rep.log("OTA updates: " + describe_ota(ota_cfg))

    mksq, unsq = _squashfs_tools()
    # A fresh identity belongs to this image, including same-version rebuilds.
    flash_id = (secrets.token_hex(16) + "\n").encode("ascii")
    if host == "windows":
        return _build_archive_image(stock_squashfs, ui_binary, variant, out_bin,
                                    workdir, rep, ota_cfg, mksq, unsq, flash_id)
    # Prove the resolved squashfs tools can pack+extract with the stock LZO params BEFORE
    # unpacking the firmware - so a system tool without LZO fails cleanly, not mid-build.
    check_squashfs_tools(mksq, unsq, rep)
    if host == "linux" and _selected_squashfs_pair:
        mksq, unsq = _selected_squashfs_pair
    with _case_sensitive_extract_root(workdir, rep) as extract_root:
        rf = os.path.join(extract_root, "rf")
        if os.path.isdir(rf):
            import shutil
            shutil.rmtree(rf)

        rep.status("[1/6] unpacking stock rootfs")
        r = _run([unsq, "-d", rf, stock_squashfs], capture_output=True, text=True)
        if r.returncode != 0:
            raise BuildError(f"unsquashfs failed: {r.stderr.strip()[:400]}", code="E230")

        rep.status("[2/6] validating base is a Snowsky Disc rootfs")
        require_installable(validate_stock_rootfs(stock_squashfs, rep, allow_override=False))   # exact pin, then policy

        rep.status("[3/6] validating the diskOS UI binary")
        _validate_ui_elf(ui_binary)

        rep.status("[4/6] patching fiio_init.sh + installing first-boot hook")
        patched_fiio_init, busybox_sha, ui_sha, ui_sz = edit_image(
            TreeFS(rf), ui_binary, variant, ota_cfg, rep, flash_id=flash_id)

        rep.status("[5/6] repacking squashfs (stock params: lzo, -b 131072)")
        out_sq = os.path.join(workdir, "out.squashfs")
        if os.path.exists(out_sq):
            os.remove(out_sq)
        r = _run([mksq, rf, out_sq, "-comp", "lzo", "-b", "131072",
                  "-no-xattrs", "-all-root", "-noappend"], capture_output=True, text=True)
        if r.returncode != 0:
            raise BuildError(f"mksquashfs failed: {r.stderr.strip()[:400]}", code="E230")
        sqsz = os.path.getsize(out_sq)
        if sqsz > IMG_SIZE:
            raise BuildError(
                f"squashfs is {sqsz} > {IMG_SIZE} partition - refusing (truncating would "
                "make it unbootable). Trim content or use a smaller UI.", code="E231")

        # Trust the OUTPUT, not the repacker's exit code: confirm the superblock magic AND that the
        # embedded UI extracts byte-identical (an independent unsquashfs round-trip). Catches a
        # silently-corrupt/truncated repack - the exact failure mode a nonzero-exit-but-valid (or, worse,
        # zero-exit-but-corrupt) mksquashfs could hide - before it ever reaches the device.
        _validate_squashfs_output(out_sq, unsq, ui_sha, ui_sz, extract_root, rep, busybox_sha,
                                  expected_fiio_init=patched_fiio_init, ota_cfg=ota_cfg, flash_id=flash_id)

        rep.status("[6/6] finalizing image (pad to partition size)")
        _copyfile(out_sq, out_bin)
        with open(out_bin, "r+b") as f:      # pad to exact partition size
            f.truncate(IMG_SIZE)

        import hashlib as _h
        md5 = _h.md5(open(out_bin, "rb").read()).hexdigest()
        rep.ok(f"image built: {out_bin} ({os.path.getsize(out_bin)} bytes) md5={md5}")
        return out_bin


def _build_archive_image(stock, ui, variant, out_bin, workdir, rep, ota_cfg, mksq, unsq, flash_id):
    """Windows build only: Linux inode names and metadata never reach the host FS."""
    import hashlib
    import tempfile
    check_archive_squashfs_tools(mksq, unsq, workdir, rep)
    rep.status("[1/6] unpacking stock rootfs")
    rep.status("[2/6] validating base is a Snowsky Disc rootfs")
    require_installable(validate_stock_archive(stock, unsq, workdir, rep, allow_override=False))
    rep.status("[3/6] validating the diskOS UI binary")
    _validate_ui_elf(ui)
    with _export_archive(stock, unsq, workdir, code="E230") as archive:
        rep.status("[4/6] patching fiio_init.sh + installing first-boot hook")
        try:
            patched, bb_sha, ui_sha, ui_sz = edit_image(
                archive_extraction_fs(archive), ui, variant, ota_cfg, rep, flash_id=flash_id)
        except OSError as exc:
            raise BuildError(f"archive edit failed: {exc}", code="E230") from exc
        rep.status("[5/6] repacking squashfs (stock params: lzo, -b 131072)")
        try:
            with tempfile.TemporaryDirectory(prefix="diskos-pfbuild-", dir=workdir) as td:
                empty, pf, sq = (os.path.join(td, p) for p in ("empty", "edited.pf", "out.squashfs"))
                try:
                    os.mkdir(empty)
                    with open(pf, "wb") as output:
                        archive.write(output)
                    root = archive.entries[b"/"]
                    r = _run([mksq, empty, sq, "-pf", pf, "-comp", "lzo", "-b", "131072",
                              "-no-xattrs", "-all-root", "-noappend",
                              "-root-time", str(root[1]), "-root-mode", format(root[2], "o")],
                             msys_paths=(1, 2, 4), capture_output=True, text=True, timeout=600)
                    if r.returncode:
                        raise ValueError(f"mksquashfs failed: {(r.stderr or '').strip()[:400]}")
                except (OSError, ValueError, subprocess.SubprocessError) as exc:
                    raise BuildError(f"archive repack failed: {exc}", code="E230") from exc
                validate_archive_output(sq, unsq, ui_sha, ui_sz, workdir, bb_sha, patched,
                                        ota_cfg, intended_archive=archive, flash_id=flash_id)
                rep.status("[6/6] finalizing image (pad to partition size)")
                # Finish a sibling first; the destination only changes once it is complete.
                partial = out_bin + ".partial"
                try:
                    _copyfile(sq, partial)
                    with open(partial, "r+b") as output:
                        output.truncate(IMG_SIZE)
                    os.replace(partial, out_bin)
                except OSError as exc:
                    with contextlib.suppress(OSError):
                        os.unlink(partial)
                    raise BuildError(f"finalizing image failed: {exc}", code="E230") from exc
        except OSError as exc:
            # temp workspace creation/cleanup; coded stage errors pass through unchanged
            raise BuildError(f"archive repack workspace failed: {exc}", code="E230") from exc
    try:
        with open(out_bin, "rb") as source:
            md5 = hashlib.md5(source.read()).hexdigest()
    except OSError as exc:
        raise BuildError(f"built image could not be read back: {exc}", code="E230") from exc
    rep.ok(f"image built: {out_bin} ({os.path.getsize(out_bin)} bytes) md5={md5}")
    return out_bin


def _grep1(path, pattern):
    try:
        with open(path, encoding="utf-8", errors="ignore") as f:
            m = re.search(pattern, f.read())
            return m.group(1) if m else None
    except OSError:
        return None


def _assert_within_rf(rf, dst, code="E233"):
    """Refuse a write target that, via a symlink AT the target or in any PARENT component, resolves
    OUTSIDE the extracted rootfs `rf`. Image building runs with the caller's privileges (often sudo,
    since mask-ROM USB needs it), so a crafted stock squashfs that ships e.g.
    `etc/init.d/S97diskos_install -> /etc/cron.d/x` could make a plain open() clobber a host file.
    realpath() resolves every symlink on the path, so an escape is caught here before any write."""
    rroot = os.path.realpath(rf)
    real = os.path.realpath(dst)
    if not (real == rroot or real.startswith(rroot + os.sep)):
        raise BuildError(
            f"refusing to write outside the rootfs: {dst!r} resolves to {real!r} via a symlink - "
            "the stock firmware may be crafted or corrupt. Re-download the official FiiO firmware.",
            code=code)


def _install(src, dst, mode, rf):
    _assert_within_rf(rf, dst)
    _edit_install(TreeFS(rf), src, os.fsencode(os.path.relpath(dst, rf)), mode)
