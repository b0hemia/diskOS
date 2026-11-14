# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Per-user state so the tool can always put the device back to bone-stock and
so `remove` can clean up after itself.

Everything the tool creates lives under ONE directory (state_dir()); nothing is
written system-wide. `remove` deletes that directory (and, if the user asks, the
tool itself) - that's the whole uninstall footprint on Linux/macOS.

Contents:
    state.json            what we've done (installed?, versions, timestamps)
    stock/stock.bin       the saved stock-rootfs image, rebuilt from the user's firmware (for restore-stock)
    stock/stock.sha256    its checksum
    build/                scratch for extract/build (cleaned opportunistically)
    firmware/             verified firmware ZIP cache and incomplete .part files
"""

import hashlib
import json
import os
import shutil
import sys
import time

from . import errors

APP = "diskos-installer"
# A marker file dropped in our state dir. wipe_all() refuses to recursively delete any directory
# that does not contain it, so a mis-set DISKOS_INSTALLER_HOME can never delete an arbitrary tree.
SENTINEL = ".diskos-installer-state"
# The diskOS install/restore lifecycle values written to state.json["phase"]. Used to recognise a
# legacy (pre-sentinel) state dir by CONTENT, specifically enough that a foreign project's state.json
# cannot masquerade as ours. Keep in sync with service.py.
_KNOWN_PHASES = {"prepared", "flash-started", "flash-verified", "restore-started", "restore-verified"}


def _fsync_dir(path):
    """Fsync a directory so a rename/create inside it is durable across a crash."""
    try:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        pass


def _state_base():
    """Compute the state-dir PATH only - no directory creation, no side effects.
    Used by wipe_all() so validation never re-creates the sentinel it is about to check."""
    env = os.environ.get("DISKOS_INSTALLER_HOME")
    if env:
        return env
    if sys.platform == "win32":
        local = os.environ.get("LOCALAPPDATA")
        if not local or not os.path.isabs(local):
            raise errors.PreflightError("LOCALAPPDATA must name an absolute per-user directory", code="E142")
        return os.path.join(local, APP)
    if sys.platform == "darwin":
        return os.path.expanduser(f"~/Library/Application Support/{APP}")
    xdg = os.environ.get("XDG_DATA_HOME")
    return os.path.join(xdg, APP) if xdg else os.path.expanduser(f"~/.local/share/{APP}")


def _is_our_statedir(base):
    """STRONG test that `base` is (or was) a diskOS state dir - governs whether we may adopt it and,
    in wipe_all(), recursively delete it. Requires GENUINE evidence, never a merely-common basename,
    so an unrelated directory that happens to contain a 'build/' or a 'state.json' is NEVER matched:
      - our sentinel file, OR
      - our exact stock-recovery pair stock/stock.bin + stock/stock.sha256, OR
      - a state.json that parses AND matches our specific schema (a boolean `installed` plus a
        `phase` drawn from our known lifecycle values) - not merely the generic key NAMES, so an
        unrelated project's state.json cannot be mistaken for ours and later deleted."""
    if os.path.isfile(os.path.join(base, SENTINEL)):
        return True
    if (os.path.isfile(os.path.join(base, "stock", "stock.bin"))
            and os.path.isfile(os.path.join(base, "stock", "stock.sha256"))):
        return True
    sj = os.path.join(base, "state.json")
    if os.path.isfile(sj):
        try:
            with open(sj) as f:
                d = json.load(f)
            if (isinstance(d, dict) and isinstance(d.get("installed"), bool)
                    and isinstance(d.get("phase"), str) and d.get("phase") in _KNOWN_PHASES):
                return True
        except (OSError, ValueError, TypeError):
            pass
    return False


def state_dir():
    """A single, per-user, non-system directory for all tool state (created on first use)."""
    base = _state_base()
    marker = os.path.join(base, SENTINEL)
    # SAFETY, only for an EXPLICITLY-set home: refuse to adopt a pre-existing, non-empty directory
    # that shows NO sign of being ours, so a mis-set DISKOS_INSTALLER_HOME (e.g. $HOME) can't be
    # tagged here and later wiped by 'remove'. The DEFAULT XDG/APP path is always ours; and an
    # existing state dir (even one predating the sentinel) is recognised by its contents.
    if (os.environ.get("DISKOS_INSTALLER_HOME") and os.path.isdir(base)
            and not _is_our_statedir(base)):
        try:
            nonempty = bool(os.listdir(base))
        except OSError:
            nonempty = True
        if nonempty:
            raise errors.PreflightError(
                f"DISKOS_INSTALLER_HOME={base!r} is a non-empty directory that is not a diskOS "
                "state dir - refusing to use it. Point it at a new or empty path.", code="E142")
    os.makedirs(base, exist_ok=True)
    if not os.path.exists(marker):
        try:
            with open(marker, "w") as f:
                f.write("diskOS installer state directory - safe to delete via 'diskos-installer remove'\n")
        except OSError:
            pass
    return base


def _state_path():
    return os.path.join(state_dir(), "state.json")


def load():
    """Return the saved state. A corrupt file is reported (not silently erased) so
    'never installed' and 'state damaged' stay distinguishable."""
    p = _state_path()
    if not os.path.exists(p):
        return {}
    try:
        with open(p) as f:
            return json.load(f)
    except ValueError:
        return {"_corrupt": True}
    except OSError:
        return {}


def save(d):
    d = {k: v for k, v in d.items() if k != "_corrupt"}
    d["updated"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    tmp = _state_path() + ".tmp"
    with open(tmp, "w") as f:
        json.dump(d, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, _state_path())
    _fsync_dir(state_dir())


def sha256_file(path, progress=None):
    h = hashlib.sha256()
    total = os.path.getsize(path)
    read = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            read += len(chunk)
            if progress:
                progress(read, total)
    return h.hexdigest()


# ---- saved stock backups ------------------------------------------------------------------------------------------
# Layout under <state>/stock/:
#   current.json            {"gen": "g0003"} - the committed backup (atomically replaced; the only commit point)
#   g0003/stock.bin         the padded stock rootfs image restore-stock flashes
#   g0003/stock.sha256      its SHA-256
#   g0003/stock.json        manifest: firmware version, the Disc base it needs, image/rootfs hashes, catalogue + writer
#                           identity, status ("pending" until a flash's base gate passed)
#   stock.bin + stock.sha256 (no current.json) = a backup from an older installer ("legacy"); still valid.
# A new backup is STAGED as a new generation and only becomes current when a flash run proves the Disc is on its base,
# so an interrupted or refused install never replaces a good recovery image.

def _stock_root():
    d = os.path.join(state_dir(), "stock")
    os.makedirs(d, exist_ok=True)
    return d


def _gen_names():
    try:
        return sorted(n for n in os.listdir(_stock_root()) if len(n) == 5 and n[0] == "g" and n[1:].isdigit())
    except OSError:
        return []


def generation_paths(gen):
    d = os.path.join(_stock_root(), gen)
    return os.path.join(d, "stock.bin"), os.path.join(d, "stock.sha256"), os.path.join(d, "stock.json")


def pointer_status():
    """("absent", None) - no pointer (a legacy backup or none); ("ok", gen); ("damaged", reason) - a pointer exists
    but is unreadable or names a missing generation. A damaged pointer is NEVER treated as absent: that would silently
    bring back an older legacy backup."""
    path = os.path.join(_stock_root(), "current.json")
    if not os.path.exists(path):
        return "absent", None
    try:
        with open(path) as f:
            gen = json.load(f).get("gen")
    except (OSError, ValueError, AttributeError) as e:
        return "damaged", "unreadable backup pointer (%s)" % e
    if not isinstance(gen, str) or gen not in _gen_names():
        return "damaged", "the backup pointer names a missing backup (%r)" % (gen,)
    return "ok", gen


def current_generation():
    """The committed generation's name, or None (no pointer, or a damaged one - see pointer_status)."""
    st, gen = pointer_status()
    return gen if st == "ok" else None


def read_manifest(gen):
    try:
        with open(generation_paths(gen)[2]) as f:
            m = json.load(f)
        return m if isinstance(m, dict) else None
    except (OSError, ValueError):
        return None


def stock_paths():
    """(image, sha256 file) of the COMMITTED backup: the current generation, else the legacy pair."""
    st, gen = pointer_status()
    if st == "ok":
        b, sh, _ = generation_paths(gen)
        return b, sh
    d = _stock_root()
    if st == "damaged":                                  # nothing usable: never fall back to the legacy pair
        return os.path.join(d, "damaged-pointer", "stock.bin"), os.path.join(d, "damaged-pointer", "stock.sha256")
    return os.path.join(d, "stock.bin"), os.path.join(d, "stock.sha256")


def _write_file_durable(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def stage_stock_generation(src_bin, manifest, progress=None):
    """Copy a padded stock image into a NEW generation (never touching the current one). Validated (squashfs magic,
    SHA-256) and fsynced before its manifest is written. Returns (gen, sha256). The manifest gets status "pending"."""
    names = _gen_names()
    gen = "g%04d" % ((int(names[-1][1:]) + 1) if names else 1)
    d = os.path.join(_stock_root(), gen)
    os.makedirs(d)
    dst, shafile, mfile = generation_paths(gen)
    h = hashlib.sha256()
    total = os.path.getsize(src_bin)
    read = 0
    with open(src_bin, "rb") as s, open(dst + ".tmp", "wb") as o:
        while True:
            chunk = s.read(1 << 20)
            if not chunk:
                break
            o.write(chunk)
            h.update(chunk)
            read += len(chunk)
            if progress:
                progress(read, total)
        o.flush()
        os.fsync(o.fileno())
    with open(dst + ".tmp", "rb") as f:
        if f.read(4) != b"hsqs":
            os.unlink(dst + ".tmp")
            os.rmdir(d)
            raise ValueError("refusing to save a non-squashfs stock image (bad magic).")
    digest = h.hexdigest()
    os.replace(dst + ".tmp", dst)
    _write_file_durable(shafile, (digest + "\n").encode())
    m = dict(manifest or {})
    m.update({"schema": 1, "image_sha256": digest, "image_size": total, "status": "pending"})
    _write_file_durable(mfile, json.dumps(m, indent=2, sort_keys=True).encode())
    _fsync_dir(d)
    _fsync_dir(_stock_root())
    return gen, digest


def promote_generation(gen):
    """Make `gen` the committed backup (one atomic pointer replace). The previous generation stays on disk."""
    if gen not in _gen_names():
        raise ValueError("no such backup generation: %s" % gen)
    m = read_manifest(gen)
    if m is not None and m.get("status") != "committed":
        m["status"] = "committed"
        _write_file_durable(generation_paths(gen)[2], json.dumps(m, indent=2, sort_keys=True).encode())
    _write_file_durable(os.path.join(_stock_root(), "current.json"), json.dumps({"gen": gen}).encode())
    _fsync_dir(_stock_root())


def discard_generation(gen):
    """Remove a PENDING generation this installer created (never the current one)."""
    if gen == current_generation() or gen not in _gen_names():
        return False
    for p in generation_paths(gen):
        for q in (p, p + ".tmp"):
            try:
                os.unlink(q)
            except FileNotFoundError:
                pass
    try:
        os.rmdir(os.path.join(_stock_root(), gen))
    except OSError:
        return False
    _fsync_dir(_stock_root())
    return True


def prune_generations(keep):
    """Remove old NON-current generations, keeping `keep` and the one newest other pending generation (a flash whose
    outcome was unknown may have used it). Bounds the ~96 MiB-each accumulation. Returns the removed names."""
    cur = current_generation()
    others = [g for g in _gen_names() if g not in (cur, keep)]
    removed = []
    for g in others[:-1]:                                # all but the newest other one
        if discard_generation(g):
            removed.append(g)
    return removed


def save_stock_image(src_bin, progress=None):
    """Save + COMMIT a stock image as the current backup (used when no flash gate is involved). Returns its sha256."""
    gen, digest = stage_stock_generation(src_bin, {"source": "saved without a flash"}, progress)
    promote_generation(gen)
    return digest


def stock_image_exists():
    """Fast existence check (no hashing) - safe to call on the UI thread."""
    dst, shafile = stock_paths()
    return os.path.exists(dst) and os.path.exists(shafile)


def have_stock_image():
    dst, shafile = stock_paths()
    try:
        if not (os.path.exists(dst) and os.path.exists(shafile)):
            return False
        want = open(shafile).read().strip()
        return sha256_file(dst) == want
    except OSError:
        return False


def build_dir():
    d = os.path.join(state_dir(), "build")
    os.makedirs(d, exist_ok=True)
    return d


def wipe_all():
    """Delete the entire tool state directory, including firmware ZIPs and partials.
    Returns (path, errors): errors is a list of (path, message) for anything that
    could NOT be removed, so the caller reports the truth instead of a false success."""
    d = _state_base()   # PATH only - never re-create the sentinel we are about to verify
    errs = []
    # SAFETY: only ever recursively delete a directory we can PROVE is our own state dir. This
    # guards against DISKOS_INSTALLER_HOME being set to '/', '$HOME', the cwd, or any other tree.
    real = os.path.realpath(d)
    forbidden = {os.path.realpath(os.sep), os.path.realpath(os.path.expanduser("~"))}
    try:
        forbidden.add(os.path.realpath(os.getcwd()))
    except OSError:
        pass
    if real in forbidden:
        return d, [(d, "refusing to delete a root/home/current directory")]
    if not (os.path.isdir(d) and _is_our_statedir(d)):
        return d, [(d, "not a diskOS state dir (no sentinel or known state) - refusing to delete")]
    try:
        if os.stat(d).st_uid != os.getuid():
            return d, [(d, "state dir is not owned by you - refusing to delete")]
    except OSError as e:
        return d, [(d, str(e))]
    # A leftover case-sensitive scratch mount from a macOS build must be detached BEFORE we
    # recurse: rmtree traversing a live mount could delete the mounted contents or the backing
    # image. Detach a stale one (bounded retries), and refuse to delete while it is still mounted.
    scratch = os.path.join(d, "build", ".cs-scratch")
    if os.path.ismount(scratch):
        import subprocess, time
        for i in range(6):
            if not os.path.ismount(scratch):
                break
            try:
                subprocess.run(["hdiutil", "detach", scratch] + (["-force"] if i >= 3 else []),
                               capture_output=True, text=True, timeout=60)
            except FileNotFoundError:
                break
            except subprocess.TimeoutExpired:
                pass
            time.sleep(0.4 * (i + 1))
        if os.path.ismount(scratch):
            return d, [(scratch, "a case-sensitive scratch volume is still mounted here; detach "
                        "it (hdiutil detach) and retry")]
    shutil.rmtree(d, onerror=lambda fn, path, exc: errs.append((path, str(exc[1]))))
    return d, errs
