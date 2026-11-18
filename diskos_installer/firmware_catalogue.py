# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned official packages and a verified, owned cache. No network access."""
import contextlib
import hashlib
import os
import re
import sys
import tempfile
from types import MappingProxyType
from typing import NamedTuple

from . import basegate, imagebuild, state


class Firmware(NamedTuple):
    url: str
    size: int
    sha256: str
    name: str


CATALOGUE = MappingProxyType({
    '209': Firmware(
        'https://fiio-firmware.oss-us-west-1.aliyuncs.com/DISC/SNOWSKY_DISC_update_20260603_v209.zip',
        98605727, 'aebd9a2cbb11d7690a79291e7b7534cba6f3194e1411167641b38d26562d9bfd', 'FiiO V2.09'),
    '228': Firmware(
        'https://fiio-firmware.fiio.net/DISC/SNOWSKY_DISC_update_20260724_v228.zip',
        112808122, '90e7644465b40840d501acd1dd933b5b1d0cef679576457cdc000f0cdfa7d25b', 'FiiO V2.28'),
    '240': Firmware(
        'https://fiio-firmware.fiio.net/DISC/SNOWSKY_DISC_update_20260820_v240.zip',
        128281609, '2eaa605858b1ed9b9fdb930d5352f87acaf1197cea1317bc520fb48a0ded10cb', 'FiiO V2.40'),
    '257': Firmware(
        'https://fiio-firmware.fiio.net/DISC/SNOWSKY_DISC_update_20260909_v257.zip',
        105680009, '2bfad000609440bcbf492468e9e5f8f3d5e0bade797323431a15b5755688bfcf', 'FiiO V2.57'),
})


def check_consistency():
    supported = (set(imagebuild.INSTALL_FW) & set(imagebuild.PINNED_ROOTFS)
                 & set(basegate.CATALOGUE))
    if set(CATALOGUE) != supported:
        raise RuntimeError('firmware catalogue version drift: ZIP=%s, supported intersection=%s'
                           % (sorted(CATALOGUE), sorted(supported)))


check_consistency()


def _pin(version):
    check_consistency()
    return CATALOGUE[version]


def _verify_stream(stream, pin, output=None, check=None):
    digest = hashlib.sha256()
    length = 0
    while True:
        if check is not None:
            check()
        # Read at most one byte beyond the pin, even for an unbounded source.
        chunk = stream.read(min(1 << 20, pin.size + 1 - length))
        if not chunk:
            break
        length += len(chunk)
        if length > pin.size:
            raise ValueError('Firmware ZIP exceeds pinned length')
        digest.update(chunk)
        if output is not None:
            output.write(chunk)
    if length != pin.size or digest.hexdigest() != pin.sha256:
        raise ValueError('Firmware ZIP does not match pinned length and SHA-256')


def verify(path, version):
    """Stream a file against the whole-ZIP pin before any ZIP parsing."""
    pin = _pin(version)
    with open(path, 'rb') as stream:
        _verify_stream(stream, pin)


@contextlib.contextmanager
def _locked(run_lock):
    # CLI/GUI callers already inside their run lock must pass that live object.
    # Standalone callers take the same lock, including its Windows global mutex.
    from .__main__ import _Lock
    if run_lock is None:
        with _Lock():
            yield
    else:
        classes = (_Lock, getattr(sys.modules['__main__'], '_Lock', _Lock))
        if not isinstance(run_lock, classes) or (run_lock.fd is None and run_lock.handle is None):
            raise RuntimeError('A held installer run lock is required')
        if not run_lock.windows:
            import fcntl
            try:
                if not os.path.samestat(os.fstat(run_lock.fd),
                                       os.stat(run_lock.path)):
                    raise OSError('Run-lock descriptor names another file')
                fcntl.flock(run_lock.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as exc:
                raise RuntimeError('A held installer run lock is required') from exc
        yield


_SAFE_NAME = re.compile(r'[A-Za-z0-9._-]+\.zip')


def _cache_name(pin):
    """FiiO's own filename: the last URL segment, as a plain safe basename."""
    name = pin.url.rsplit('/', 1)[-1]
    if not _SAFE_NAME.fullmatch(name) or name.startswith('.') or '..' in name:
        raise ValueError('Unsafe firmware filename in catalogue URL')
    return name


def _cache_path(pin):
    name = _cache_name(pin)
    directory = os.path.join(state.state_dir(), 'firmware')
    os.makedirs(directory, exist_ok=True)
    return os.path.join(directory, name)


def cached(version, run_lock=None):
    """Return a rehashed cache path, or None. Remove corrupt owned entries."""
    pin = _pin(version)
    with _locked(run_lock):
        path = _cache_path(pin)
        try:
            with open(path, 'rb') as stream:
                _verify_stream(stream, pin)
        except FileNotFoundError:
            return None
        except ValueError:
            os.unlink(path)
            state._fsync_dir(os.path.dirname(path))
            return None
        return path


def store(source, version, run_lock=None):
    """Copy and verify local bytes; publish only a flushed, complete cache file.

    Originals are never moved or deleted. Interrupted .part files are not usable
    entries and are included in Remove installer data. Transfers restart at zero.
    """
    pin = _pin(version)
    with _locked(run_lock):
        path = _cache_path(pin)
        directory = os.path.dirname(path)
        fd, partial = tempfile.mkstemp(prefix=os.path.basename(path) + '.', suffix='.part', dir=directory)
        try:
            with os.fdopen(fd, 'wb') as output:
                with open(source, 'rb') as stream:
                    _verify_stream(stream, pin, output)
                output.flush()
                os.fsync(output.fileno())
            # Check the saved bytes too, rather than trusting the source copy.
            _publish(partial, pin, path)
            return path
        finally:
            if os.path.exists(partial):
                os.unlink(partial)


def _publish(partial, pin, path, check=None):
    with open(partial, 'rb') as stream:
        _verify_stream(stream, pin, check=check)
    directory = os.path.dirname(path)
    if check is not None:
        check()
    os.replace(partial, path)
    state._fsync_dir(directory)
    state._fsync_dir(state.state_dir())


# Narrow hook for the streaming downloader: it writes network bytes straight into
# a cache-owned .part file and this module verifies and promotes it, so hashing
# and promotion stay in one place.
def new_partial(version, run_lock=None):
    """Create an empty, cache-owned .part file and return its path."""
    pin = _pin(version)
    with _locked(run_lock):
        path = _cache_path(pin)
        fd, partial = tempfile.mkstemp(prefix=os.path.basename(path) + '.', suffix='.part',
                                       dir=os.path.dirname(path))
        os.close(fd)
        return partial


def promote(partial, version, run_lock=None, check=None):
    """Flush, verify (length + SHA-256) and atomically publish a .part file.

    Raises ValueError on mismatch; the caller removes the .part file.
    """
    pin = _pin(version)
    with _locked(run_lock):
        path = _cache_path(pin)
        if os.path.dirname(os.path.abspath(partial)) != os.path.abspath(os.path.dirname(path)) \
                or not partial.endswith('.part'):
            raise ValueError('Not a cache-owned partial download')
        with open(partial, 'rb+') as stream:
            os.fsync(stream.fileno())
        _publish(partial, pin, path, check=check)
        return path
