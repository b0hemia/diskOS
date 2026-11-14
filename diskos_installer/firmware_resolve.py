# SPDX-License-Identifier: GPL-3.0-or-later
"""Install/restore orchestration for firmware acquisition (no GUI, never flashes).

The user says which firmware the Disc runs; this layer turns that into a verified
ZIP (cache, supervised download, or a manually chosen file), decides whether a
restore can use the saved backup, and checks disk space before any build starts.
Nothing here imports the flash writer, writes the backup store or moves the
current-generation pointer: those stay with service.py and its commit conditions.

Seams (`catalogue`, `supervisor`) default to the real slice 1 and slice 4
modules; tests pass fakes. Acquisition errors are never wrapped: they surface
unchanged from supervisor.start() or Acquisition.wait().
"""
import os
import shutil
import sys
import tempfile
import zipfile
from typing import NamedTuple

from . import basegate, errors, firmware_catalogue, firmware_fetch, firmware_local, imagebuild, state

MIB = 1 << 20
HEADROOM = 64 * MIB
# Current extracted stock fixtures contain at most 174 MB of regular-file
# contents. Leave room for the bundled payload, metadata and allocation.
UNPACKED_BUDGET = 256 * MIB
WORKSPACE_BYTES = 3 * UNPACKED_BUDGET


class FirmwareResolveError(errors.DiskOSError):
    code = 'E171'


class UnknownVersion(FirmwareResolveError):
    code = 'E171'


class UnknownFirmware(FirmwareResolveError):
    code = 'E172'


class WrongVersion(FirmwareResolveError):
    code = 'E173'


class InsufficientSpace(FirmwareResolveError):
    code = 'E174'

    def __init__(self, needed, available, where):
        self.needed, self.available, self.where = needed, available, where
        super().__init__(
            'Not enough free space: %d MB needed, %d MB available in %s' % (
                -(-needed // MIB), available // MIB, where),
            action='free the difference and retry; your saved stock backup was not touched')


class Resolution(NamedTuple):
    """kind "cached": `path` is a rehashed ZIP. kind "acquiring": `handle` is the
    supervisor's Acquisition (wait() -> path, cancel()); errors propagate from it."""
    kind: str
    version: str
    path: str = None
    handle: object = None


class ZipIdentity(NamedTuple):
    version: str
    path: str


class RestorePlan(NamedTuple):
    """action "backup": flash `backup_path` (version `version`). action "firmware": call
    resolve_firmware(version). `reason` says why the backup was not used (or None)."""
    action: str
    version: str
    backup_path: str = None
    reason: str = None


def _name(catalogue, version):
    return catalogue.CATALOGUE[version].name


def resolve_firmware(version, run_lock=None, on_event=None, catalogue=firmware_catalogue,
                     supervisor=firmware_fetch):
    """(a) verified cache hit -> Resolution("cached"); (b) else start supervised acquisition.

    cached() rehashes and removes a corrupt owned entry (slice 1), so a damaged
    cache file is re-acquired. Needs the held installer run lock for acquisition."""
    if version not in catalogue.CATALOGUE:
        raise UnknownVersion('No supported firmware version %r' % (version,),
                             action='choose one of the listed versions')
    path = catalogue.cached(version, run_lock)
    if path is not None:
        return Resolution('cached', version, path=path)
    return Resolution('acquiring', version, handle=supervisor.start(version, on_event, run_lock))


def check_downloads(version, directory=None, run_lock=None, cancel=None, **seams):
    """Fallback: scan Downloads for the official ZIP (see firmware_local.scan_downloads)."""
    return firmware_local.scan_downloads(version, directory, run_lock, cancel, **seams)


def open_download_page(version, **seams):
    """Fallback: open FiiO's page. Call only on an explicit button press (BrowserResult)."""
    return firmware_local.open_download_page(version, **seams)


def identify_zip(path, expected=None, catalogue=firmware_catalogue):
    """(c) Map a manually chosen ZIP to its version through the whole-ZIP verifier.

    Only a candidate whose pinned size matches is hashed. Raises UnknownFirmware
    (not an official pinned ZIP) or WrongVersion (a valid ZIP of another version
    than `expected`); the file is never copied, moved or deleted."""
    if sys.platform == 'darwin' and os.path.isdir(path):
        raise UnknownFirmware('Safari unpacked the firmware ZIP. A folder cannot be verified against the recorded ZIP checksum.',
                              action='In Safari, right-click the FiiO download link and choose Download Linked File, '
                                     'then select the .zip; or use Get firmware')
    try:
        size = os.path.getsize(path)
    except OSError as exc:
        raise UnknownFirmware('Cannot read that file: %s' % exc.strerror,
                              action='choose the firmware ZIP again') from exc
    for version, pin in catalogue.CATALOGUE.items():
        if pin.size != size:
            continue
        try:
            catalogue.verify(path, version)
        except ValueError:
            continue
        except OSError as exc:
            raise UnknownFirmware('Cannot read that file: %s' % exc.strerror) from exc
        if expected is not None and version != expected:
            raise WrongVersion('That file is %s, but %s was chosen' % (
                _name(catalogue, version), _name(catalogue, expected)),
                action='pick the matching version, or choose the %s ZIP' % _name(catalogue, expected))
        return ZipIdentity(version, path)
    raise UnknownFirmware('That file is not one of the official FiiO firmware ZIPs this installer supports',
                          action='download it again from FiiO, or use Get firmware')


def space_needed(version, zip_path=None):
    """Bytes the extract + build + backup staging of `version` may need. With no `zip_path`
    the ZIP's expansion is budgeted at its pinned size (a conservative stand-in)."""
    rootfs = imagebuild.PINNED_ROOTFS[version][1]
    if zip_path is not None:
        with zipfile.ZipFile(zip_path) as zf:
            expansion = sum(i.file_size for i in zf.infolist())
    else:
        expansion = firmware_catalogue.CATALOGUE[version].size
    # expansion + assembled rootfs + padded copy + workspace + built image + backup staging
    return (expansion + rootfs + imagebuild.IMG_SIZE + WORKSPACE_BYTES
            + imagebuild.IMG_SIZE + imagebuild.IMG_SIZE + HEADROOM)


def _existing_directory(path):
    path = os.path.abspath(path)
    while not os.path.isdir(path):
        parent = os.path.dirname(path)
        if parent == path:
            raise OSError('No existing directory for %r' % path)
        path = parent
    return path


def check_space(version, zip_path=None, where=None, usage=shutil.disk_usage,
                temp_root=None):
    """Check build/backup and temporary storage before extraction.

    Returns the needed/available pair for the state filesystem.
    Existing backups and the generation pointer are not changed.
    """
    state_path = _existing_directory(where or state._state_base())
    temp_path = _existing_directory(
        temp_root if temp_root is not None else tempfile.gettempdir())
    state_need = space_needed(version, zip_path)
    temp_need = HEADROOM
    if sys.platform == 'win32':
        temp_need += 3 * UNPACKED_BUDGET

    requirements = {}
    for path, needed in ((state_path, state_need), (temp_path, temp_need)):
        device = os.stat(path).st_dev
        if device in requirements:
            first, previous = requirements[device]
            requirements[device] = first, previous + needed
        else:
            requirements[device] = path, needed

    result = None
    state_device = os.stat(state_path).st_dev
    for device, (path, needed) in requirements.items():
        available = usage(path).free
        if available < needed:
            raise InsufficientSpace(needed, available, path)
        if device == state_device:
            result = needed, available
    return result


def _backup_problem(rep=None):
    """(version, path, None) for a valid saved backup, else (None, None, why). Read-only."""
    stock_root = os.path.join(state._state_base(), 'stock')
    if not os.path.isdir(stock_root):
        return None, None, 'no saved stock backup'
    status, why = state.pointer_status()
    if status == 'damaged':
        return None, None, 'the saved backup record is damaged'
    if not state.stock_image_exists():
        return None, None, 'no saved stock backup'
    if not state.have_stock_image():
        return None, None, 'the saved stock backup does not match its checksum'
    path = state.stock_paths()[0]
    if os.path.getsize(path) > imagebuild.IMG_SIZE:
        return None, None, 'the saved stock backup is larger than the system partition'
    try:
        mver = imagebuild.validate_stock_rootfs(path, rep)
    except errors.DiskOSError as exc:
        return None, None, 'the saved stock backup is not a valid Disc image (%s)' % exc.message
    if mver not in basegate.CATALOGUE:
        return None, None, 'the saved backup is for an unsupported version (V%s)' % mver
    gen = state.current_generation()
    manifest = state.read_manifest(gen) if gen else None
    if manifest is not None and 'main_os_ver' in manifest and manifest['main_os_ver'] != mver:
        return None, None, 'the saved backup record disagrees with its image'
    return mver, path, None


def plan_restore(version=None, rep=None):
    """Validate the saved backup (checksum, squashfs/product/pins, record agreement), not just its
    existence. Valid and matching `version` (or `version` None) -> action "backup"; otherwise
    action "firmware" for `version`. Read-only: backups and the pointer are never touched."""
    mver, path, why = _backup_problem(rep)
    if mver is not None and (version is None or version == mver):
        return RestorePlan('backup', mver, backup_path=path)
    if mver is not None:
        why = 'the saved backup is for V%s.%s, not V%s.%s' % (mver[0], mver[1:], version[0], version[1:])
    if version is None:
        raise FirmwareResolveError('Cannot restore: %s' % why,
                                   action='choose the Disc\'s firmware version so I can fetch it')
    if version not in firmware_catalogue.CATALOGUE:
        raise UnknownVersion('No supported firmware version %r' % (version,))
    return RestorePlan('firmware', version, reason=why)
