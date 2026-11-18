# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Application service - the install / restore-stock / remove flows, independent
of any front-end. CLI and GUI both call these with a Reporter and a `confirm`
callback, so the two front-ends can never diverge in behaviour or safety checks.

`confirm(summary: dict) -> bool` is the destructive-action gate:
  - CLI: a y/N prompt.
  - GUI: the CONFIRMATION screen (acknowledge checkbox + explicit button),
    driven from the worker thread via a blocking Event.

Each flow returns a result dict; raises BuildError/FlashError on failure.
"""

import os
import time

from . import basegate, bundle, flasher, imagebuild, state

INSTALL_CONSEQUENCE = (
    "This replaces the Disc's system software to install diskOS. "
    "Reinstalling starts this installer's diskOS version and clears downloaded diskOS updates. "
    "Your settings and music are kept. "
    "Stored data is not wiped, and the SD card is not changed. "
    "Keep the USB cable connected until it finishes.")
RESTORE_CONSEQUENCE = (
    "This replaces the Disc's system software with FiiO's stock firmware and deactivates diskOS. "
    "Stored data is not wiped, and the SD card is not changed. "
    "Keep the USB cable connected until it finishes.")


def require_firmware_dependencies():
    """Check the actual AES backend before starting any firmware work."""
    try:
        from Crypto.Cipher import AES
        AES.new(bytes(32), AES.MODE_CBC, bytes(16))
    except (ImportError, OSError) as exc:
        raise imagebuild.BuildError("pycryptodome AES dependency is missing or cannot load", code="E105",
                                   action="source runs: install the packages in requirements.txt into the Python "
                                          "environment running the installer; standalone builds: re-download "
                                          "or rebuild the installer with those requirements") from exc


def _padded_stock(stock_sq):
    """The stock rootfs padded to the partition size (a new file beside it when padding is needed)."""
    sz = os.path.getsize(stock_sq)
    if sz > imagebuild.IMG_SIZE:
        raise imagebuild.BuildError(
            f"stock rootfs is {sz} bytes > {imagebuild.IMG_SIZE} partition - refusing "
            "(truncating it would produce a corrupt 'stock' image).", code="E240")
    if sz == imagebuild.IMG_SIZE:
        return stock_sq
    padded = stock_sq + ".padded"
    imagebuild._copyfile(stock_sq, padded)
    with open(padded, "r+b") as f:
        f.truncate(imagebuild.IMG_SIZE)
    return padded


def _stage_stock(stock_sq, mver, rep, source):
    """Stage the user's stock image as a PENDING backup generation (the current backup is untouched). It is promoted
    only after a flash run whose base gate proved the Disc is on V{mver}'s base. Returns the generation name."""
    rep.status("Saving your stock rootfs image (committed once the Disc's firmware base is confirmed)")
    base = basegate.CATALOGUE[mver]
    manifest = {"main_os_ver": mver, "base_id": base["id"], "base_name": base["name"],
                "base": {p: {"size": base[p][0], "sha256": base[p][1]} for p in basegate.PARTS},
                "catalogue_sha256": basegate.catalogue_fingerprint().hex(),
                "writer_sha256": flasher.WRITER_SHA256, "source": source,
                "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    gen, digest = state.stage_stock_generation(_padded_stock(stock_sq), manifest,
                                               progress=lambda r, t: rep.progress(r, t))
    rep.log(f"stock image staged as backup {gen} (sha256={digest[:16]}...), pending the base check")
    try:
        for g in state.prune_generations(gen):
            rep.log(f"removed old uncommitted backup {g}")
    except Exception as e:
        rep.warning(f"could not tidy old backups: {e}")
    return gen


def _settle(action, pending, rep, note):
    """promote/discard a pending backup. A failure here is reported on its own and NEVER replaces the flash outcome."""
    try:
        if action == "promote":
            state.promote_generation(pending)
        elif not state.discard_generation(pending):
            rep.warning(f"the uncommitted backup {pending} could not be removed; it stays on disk (not current)")
            return
        rep.log(f"backup {pending}: {note}")
    except Exception as e:  # disk full, permissions, ...
        rep.warning(f"the flash outcome stands, but updating the saved-backup record failed ({e}); "
                    f"the backup files are kept in {os.path.dirname(state.generation_paths(pending)[0])}")


def _flash_with_backup(image, mver, pending, log_name, rep):
    """flash(), then settle the pending backup generation by what the writer's base gate said."""
    try:
        d = flasher.flash(image, mver, log_path=os.path.join(state.state_dir(), log_name), rep=rep)
    except flasher.BaseRefused:
        if pending:                                    # the Disc is NOT on this base: this backup is wrong for it
            _settle("discard", pending, rep, "discarded (the Disc is on a different base)")
        raise
    except flasher.FlashError as e:
        v = getattr(e, "verdict", None)
        if pending and getattr(e, "pre_launch", False):
            _settle("discard", pending, rep, "discarded (nothing was sent to the Disc)")
        elif pending and v and v.get("result") == 1:   # base proven; the write failed - still the right recovery image
            _settle("promote", pending, rep, "committed (the Disc's base was confirmed before the write failed)")
        elif pending:
            rep.warning(f"flash result unknown - the new stock backup ({pending}) is kept but NOT made current")
        raise
    except BaseException:
        if pending:
            rep.warning(f"flash interrupted - the new stock backup ({pending}) is kept but NOT made current")
        raise
    if pending:
        _settle("promote", pending, rep, "committed")
    return d


def _save_stock(stock_sq, rep):
    """Copy the user's bone-stock image into state (padded to the partition size) as the COMMITTED backup, with no
    flash involved (used by tests and tooling). Returns its sha256."""
    rep.status("Saving your stock rootfs image (so diskOS can always be deactivated/reverted)")
    sz = os.path.getsize(stock_sq)
    if sz > imagebuild.IMG_SIZE:
        # NEVER truncate a larger image - a truncated file can still look flashable
        # (hsqs magic + right size) yet be a corrupt, unbootable stock.
        raise imagebuild.BuildError(
            f"stock rootfs is {sz} bytes > {imagebuild.IMG_SIZE} partition - refusing "
            "(truncating it would produce a corrupt 'stock' image).", code="E240")
    if sz < imagebuild.IMG_SIZE:
        padded = stock_sq + ".padded"
        imagebuild._copyfile(stock_sq, padded)
        with open(padded, "r+b") as f:
            f.truncate(imagebuild.IMG_SIZE)
        src = padded
    else:
        src = stock_sq
    digest = state.save_stock_image(src, progress=lambda r, t: rep.progress(r, t))
    rep.ok(f"bone-stock image saved (sha256={digest[:16]}...)")
    return digest


def _ensure_full_size(image_path, rep):
    """Repad a saved stock image that is SHORTER than IMG_SIZE up to IMG_SIZE with zero padding (the same
    convention _save_stock and validate_stock_rootfs use), so the 768-block writer covers the whole image.
    Fixes old 580-block/76 MB backups that would otherwise fail the exact-size preflight (E121). A backup
    LARGER than IMG_SIZE is real corruption - reject rather than truncate (truncation could hide it)."""
    sz = os.path.getsize(image_path)
    if sz == imagebuild.IMG_SIZE:
        return image_path
    if sz > imagebuild.IMG_SIZE:
        raise flasher.FlashError(
            f"saved stock image is {sz} bytes > {imagebuild.IMG_SIZE} - refusing (truncating it could "
            "hide a corrupt image)", code="E142",
            action="delete the saved image and rebuild it from FiiO's firmware .zip")
    padded = image_path + ".restorepad"   # next to the saved backup (an existing state dir)
    imagebuild._copyfile(image_path, padded)
    with open(padded, "r+b") as f:
        f.truncate(imagebuild.IMG_SIZE)   # zero-fills the tail to the partition size
    rep.log(f"repadded saved stock image {sz} -> {imagebuild.IMG_SIZE} bytes for the 768-block writer")
    return padded


def _save_state_soft(st, rep):
    """Persist state, but NEVER let a bookkeeping failure masquerade as a flash
    failure. Call only AFTER a verified flash."""
    try:
        state.save(st)
    except Exception as e:  # disk full, permissions, etc.
        rep.warning(f"flash succeeded, but saving local history failed: {e}")


def do_install(params, rep, confirm):
    """params: dict(firmware=?, stock=?, ui_binary=?, variant='public'|'dev').
    Extract -> save bone-stock -> build -> confirm -> flash. Returns result dict."""
    if params.get("firmware") and not params.get("stock"):
        require_firmware_dependencies()
    ui_bin = params.get("ui_binary") or bundle.data("mq_ui", required=False)
    if not ui_bin or not os.path.exists(ui_bin):
        raise imagebuild.BuildError("no diskOS UI binary (bundled 'mq_ui' missing).", code="E102",
                                   action="the install is incomplete; re-download the installer")
    variant = params.get("variant", "public")

    work = state.build_dir()
    st = state.load()
    st.update({"phase": "prepared", "variant": variant})
    state.save(st)

    # 1) obtain the stock rootfs + save it as the restore image
    stock_sq = os.path.join(work, "stock_rootfs.squashfs")
    if params.get("stock"):
        imagebuild._copyfile(params["stock"], stock_sq)
    elif params.get("firmware"):
        imagebuild.extract_stock_rootfs(params["firmware"], stock_sq, work, rep=rep)
    else:
        raise imagebuild.BuildError("need a firmware .zip or a stock rootfs.squashfs.", code="E140",
                                   action="pass your official FiiO firmware .zip")
    # Validate the stock image is a genuine, supported, known-good Disc rootfs BEFORE it overwrites
    # the saved recovery copy - so a wrong/corrupt image can't destroy a good recovery then abort.
    # The install policy (INSTALL_FW) is checked here too, for the same reason: refusing an unsupported
    # firmware must leave the saved recovery image untouched.
    mver = imagebuild.validate_stock_rootfs(stock_sq, rep, allow_override=False)
    imagebuild.require_installable(mver)
    if mver not in basegate.CATALOGUE:
        raise imagebuild.BuildError(f"no known firmware base for V{mver}", code="E226",
                                   action="this firmware version is not supported by this installer")

    # 2) build the diskOS image
    out_bin = os.path.join(work, f"diskos_{variant}.bin")
    ota_cfg = imagebuild.resolve_ota_config(params.get("ota_rootpub"), no_ota=bool(params.get("no_ota")))
    imagebuild.build_image(stock_sq, ui_bin, variant, out_bin, work, rep=rep,
                           ota_rootpub=params.get("ota_rootpub"), no_ota=bool(params.get("no_ota")))

    # 3) preflight + confirm (destructive gate), then flash
    flasher.preflight(out_bin, rep)
    summary = {
        "action": "install",
        "variant": variant,
        "image": out_bin,
        "ota": imagebuild.describe_ota(ota_cfg),
        "duration": "about 20 minutes",
        "consequence": INSTALL_CONSEQUENCE,
        "requires": (f"The Disc must already run FiiO's full {basegate.CATALOGUE[mver]['name']} "
                     "(kernel + recovery). The writer checks this before changing anything and stops "
                     "with nothing written if it doesn't match."),
    }
    if not confirm(summary):
        rep.warning("aborted before flashing (nothing written to the device).")
        return {"ok": False, "aborted": True}
    # staged only now (after the confirmation), so a failed build/preflight/cancel never leaves a backup behind
    pending = _stage_stock(stock_sq, mver, rep, "install")

    st.update({"phase": "flash-started"})
    _save_state_soft(st, rep)
    # From here, flash() either raises (real failure) or returns verified. A later
    # state-save error must NOT turn a verified flash into a reported failure.
    d = _flash_with_backup(out_bin, mver, pending, "last-flash.log", rep)
    st.update({"phase": "flash-verified", "installed": True,
               "installed_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())})
    _save_state_soft(st, rep)
    rep.ok("diskOS flashed and verified. Power-cycle the device to boot it.")
    return {"ok": True, "debug": d}


def do_restore(params, rep, confirm):
    """Reflash a stock rootfs (deactivates diskOS; leaves /usr/data files). With a firmware .zip, that firmware's rootfs
    is used (and becomes the saved backup once the Disc's base is confirmed); otherwise the saved backup. Either way the
    writer first proves the Disc's kernel + recovery belong to that firmware - a V2.40 backup is never written onto a
    V2.57 Disc."""
    pending = None
    if params.get("firmware"):
        require_firmware_dependencies()
        work = state.build_dir()
        stock_sq = os.path.join(work, "stock_rootfs.squashfs")
        imagebuild.extract_stock_rootfs(params["firmware"], stock_sq, work, rep=rep)
        mver = imagebuild.validate_stock_rootfs(stock_sq, rep)
        if mver not in basegate.CATALOGUE:
            raise flasher.FlashError(f"no known firmware base for V{mver}", code="E143",
                                     action="this firmware version is not supported by this installer")
        pending = _stage_stock(stock_sq, mver, rep, "restore --firmware")
        stock_bin = state.generation_paths(pending)[0]
    else:
        pst, why = state.pointer_status()
        if pst == "damaged":
            raise flasher.FlashError(
                f"the saved stock backup record is damaged: {why}", code="E145",
                action="restore with FiiO's firmware .zip instead (restore-stock --firmware <zip>)")
        if not state.have_stock_image():
            raise flasher.FlashError(
                "no saved bone-stock image", code="E141",
                action="provide FiiO's firmware .zip so I can rebuild your stock rootfs image")
        stock_bin, _ = state.stock_paths()
        # An OLD saved backup (from a prior installer that built 580-block/76 MB images) can be smaller than
        # IMG_SIZE; the 768-block writer needs the full partition-size image, so repad a short one first.
        stock_bin = _ensure_full_size(stock_bin, rep)
        # Never restore-flash an unvalidated image: product/version/known-good-hash, not just size+magic.
        mver = imagebuild.validate_stock_rootfs(stock_bin, rep)
        gen = state.current_generation()
        m = state.read_manifest(gen) if gen else None
        if m is not None and "main_os_ver" not in m:
            m = None                                   # saved without a base record: treat like a legacy backup
        if m is not None and m.get("main_os_ver") != mver:
            raise flasher.FlashError(
                f"the saved backup's record says V{m.get('main_os_ver')} but its image is V{mver} - refusing",
                code="E144", action="restore with your firmware .zip instead (restore-stock --firmware ...)")
        if mver not in basegate.CATALOGUE:
            raise flasher.FlashError(f"no known firmware base for the saved V{mver} backup", code="E143",
                                     action="restore with a supported firmware .zip instead")
        if m is None:
            # a backup from an older installer: re-save it WITH its base record; committed after the base check
            pending = _stage_stock(stock_bin, mver, rep, "legacy backup")
            stock_bin = state.generation_paths(pending)[0]
    try:
        flasher.preflight(stock_bin, rep)
    except BaseException:
        if pending:
            _settle("discard", pending, rep, "discarded (nothing was sent to the Disc)")
        raise
    summary = {
        "action": "restore-stock",
        "image": stock_bin,
        "duration": "about 20 minutes",
        "consequence": RESTORE_CONSEQUENCE,
        "requires": (f"This stock image is FiiO {basegate.CATALOGUE[mver]['name']}. The Disc must already run that "
                     "firmware's kernel + recovery; the writer checks this first and stops with nothing written "
                     "if it doesn't match."),
    }
    if not confirm(summary):
        if pending:
            _settle("discard", pending, rep, "discarded (restore cancelled)")
        rep.warning("aborted (device unchanged).")
        return {"ok": False, "aborted": True}

    st = state.load()
    st.update({"phase": "restore-started"})
    _save_state_soft(st, rep)
    try:
        d = _flash_with_backup(stock_bin, mver, pending, "last-restore.log", rep)
    except flasher.BaseRefused as e:
        have = e.verdict.get("matched_base")
        if have and have != mver and not params.get("firmware"):
            raise flasher.BaseRefused(
                f"Nothing was written. Your saved stock backup is FiiO {basegate.CATALOGUE[mver]['name']}, but this "
                f"Disc is on {basegate.CATALOGUE[have]['name']}. To restore stock on this Disc, run restore-stock "
                f"with FiiO's V{have[0]}.{have[1:]} firmware .zip (restore-stock --firmware <zip>).",
                e.verdict, code=e.code, action=None)
        raise
    st.update({"phase": "restore-verified", "installed": False})
    _save_state_soft(st, rep)
    rep.ok("Stock system reflashed. Power-cycle to boot stock.")
    rep.warning("This deactivates diskOS but is not a byte-for-byte factory wipe: the diskOS "
                "files under /usr/data (a separate partition) remain, inert - they do nothing "
                "without the boot hook this reflash removed. (The UI embedded in the diskOS "
                "rootfs is gone, since this reflash overwrote that partition with stock.)")
    return {"ok": True, "debug": d}


def do_remove(params, rep, confirm):
    """Remove saved state and owned GUI logs; retain the executable and USB driver."""
    if state.load().get("installed") and not params.get("force"):
        if not confirm({"action": "remove-tool",
                        "consequence": "diskOS still appears to be on the device. This only "
                                       "removes saved installer state and owned GUI logs from THIS "
                                       "computer (run restore-stock first to clear the device)."}):
            return {"ok": False, "aborted": True}
    d, errors = state.wipe_all()
    from . import gui_entry
    try:
        gui_entry.remove_logs()
    except OSError:
        errors.append((gui_entry.LOG_FOLDER, "GUI log cleanup refused or failed; close other installer windows "
                       "and check for redirected or foreign log files before retrying"))
    if errors:
        rep.error(f"could not fully remove {len(errors)} saved state/log item(s):")
        for p, m in errors[:8]:
            rep.log(f"  {p}: {m}")
        return {"ok": False, "errors": errors, "removed": d}
    rep.ok(f"removed all tool state and GUI logs: {d}")
    return {"ok": True, "removed": d}
