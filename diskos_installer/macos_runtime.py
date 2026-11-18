# SPDX-License-Identifier: GPL-3.0-or-later
"""macOS native-tool preparation and power assertion. No system-wide changes."""
import os
from pathlib import Path
import sys
import subprocess

from .errors import PreflightError, FlashError

_prepared = set()


def prepare_tools(vendor, run=None):
    """Remove quarantine only from this package's tools and private dylibs.

    App bundles may use internal symlinks. Never follow one outside the package.
    An unwritable or translocated app gets an actionable error before execution.
    """
    run = run or subprocess.run
    vendor = Path(vendor).absolute()
    # Frozen resource paths can point from Frameworks to Resources inside Contents.
    from . import bundle
    root = Path(bundle.resource_root()).resolve()
    in_app = (getattr(sys, 'frozen', False) and root.parent.name == 'Contents'
              and root.name in ('Frameworks', 'Resources'))
    package = root.parent if in_app else root
    resolved_vendor = vendor.resolve()
    if package != resolved_vendor and package not in resolved_vendor.parents:
        raise PreflightError('Bundled native directory escapes the package: ' + str(vendor), code='E106')
    boundary = package if in_app else resolved_vendor
    paths = [vendor / n for n in ('usbboot', 'mksquashfs', 'unsquashfs')]
    paths += sorted((vendor / 'lib').glob('*.dylib'))
    for path in paths:
        if not path.exists():
            continue
        target = path.resolve()
        if boundary != target and boundary not in target.parents:
            raise PreflightError('Bundled native file escapes the package: ' + str(path), code='E106')
        st = target.stat()
        key = (str(target), st.st_ino, st.st_mtime_ns)
        if key in _prepared:
            continue
        try:
            attrs = run(['/usr/bin/xattr', str(target)], check=True,
                        capture_output=True, text=True, timeout=10)
            if 'com.apple.quarantine' in attrs.stdout.splitlines():
                run(['/usr/bin/xattr', '-d', 'com.apple.quarantine', str(target)],
                    check=True, capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.SubprocessError) as exc:
            raise PreflightError('macOS could not unblock bundled tools', code='E106',
                                 action='move the installer to a writable folder and reopen it; '
                                        'if it is still blocked, download and extract a fresh complete release') from exc
        _prepared.add(key)


class NoSleep:
    """Scope a system-provided assertion to this process and the flash interval."""
    def __init__(self, rep=None, popen=None, pid=None):
        self.rep = rep
        self.popen = popen or subprocess.Popen
        self.pid = os.getpid() if pid is None else pid
        self.proc = None

    def __enter__(self):
        try:
            self.proc = self.popen(['/usr/bin/caffeinate', '-dims', '-w', str(self.pid)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            # A missing/broken assertion must be detected before usbboot is launched.
            try:
                self.proc.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                pass
            else:
                raise OSError('caffeinate exited before flashing')
        except (OSError, subprocess.SubprocessError) as exc:
            self.__exit__(None, None, None)
            raise FlashError('macOS could not keep the computer awake', code='E116',
                             action='check that /usr/bin/caffeinate can run, then retry; '
                                    'nothing was sent to the Disc') from exc
        if self.rep:
            self.rep.log('macOS sleep prevention active (caffeinate -dims); keep the lid open and USB connected')
        return self

    def __exit__(self, *exc):
        if self.proc is not None:
            try:
                if self.proc.poll() is None:
                    self.proc.terminate()
                    try:
                        self.proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        self.proc.kill()
                        self.proc.wait(timeout=5)
            except (OSError, subprocess.SubprocessError):
                if self.rep:
                    self.rep.warning('Could not release macOS sleep prevention; it ends when the installer exits')
        return False
