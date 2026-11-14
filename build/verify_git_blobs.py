# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare packaged inputs with raw committed blobs, without checkout filters."""
import argparse
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from deterministic_zip import checked_stat

PAYLOAD = (
    'mq_ui', 'S96diskos_select', 'S97diskos_install', 'S99usbserial',
    'diskos-debug.sh', 'dropbearmulti', 'diskos-rmguard', 'diskos-selected',
    'diskos-bootprobe', 'diskos-artdec', 'diskos-launch', 'diskos-verify.sh',
    'diskos-root.pub.pem', 'diskos-flashstate',
    'S97diskos_names', 'diskos-names', 'diskos-udhcpc', 'diskos-network-names-v1',
)
DEVICE = ('my_write6_dram.bin', 'disc_spl_lpddr3.bin')


class BlobGuard:
    def __init__(self, root, ref='HEAD'):
        self.root = Path(root).resolve()
        top = Path(self.git('rev-parse', '--show-toplevel').decode('utf-8').strip())
        if top.resolve() != self.root:
            raise ValueError('installer/ must be the Git repository root')
        self.commit = self.git('rev-parse', '--verify', ref + '^{commit}').decode('ascii').strip()
        if not re.fullmatch(r'[0-9a-f]{40}', self.commit):
            raise ValueError('Expected a full source commit SHA')

    def git(self, *args):
        return subprocess.run(
            ['git', '--no-optional-locks', '-C', str(self.root), *args],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout

    def verify(self, pairs):
        pairs = list(pairs)
        if not pairs:
            raise ValueError('Empty blob comparison')
        for target, name in pairs:
            relative = PurePosixPath(name)
            if (relative.is_absolute() or '..' in relative.parts
                    or relative.as_posix() != name or '\\' in name):
                raise ValueError('Unsafe blob path: ' + name)
            target = Path(target).absolute()
            for ancestor in (target, *target.parents):
                checked_stat(ancestor)
            if not stat.S_ISREG(checked_stat(target).st_mode):
                raise ValueError('Expected a regular file: ' + str(target))
            # Binary subprocess output: no PowerShell pipe, decoding or filters.
            expected = self.git('cat-file', 'blob', self.commit + ':' + name)
            if target.read_bytes() != expected:
                raise ValueError('Committed blob mismatch: ' + str(target) + ' != ' + name)

    def devices(self, data, tag, frozen=False):
        data = Path(data)
        pairs = [(data / 'payload' / name, 'payload/' + name) for name in PAYLOAD]
        if not frozen:
            # Source tarballs ship the complete tracked payload/ and flash/ trees.
            records = self.git('ls-tree', '-r', '-z', self.commit, '--', 'payload', 'flash')
            seen = set()
            for record in records.split(b'\0'):
                if not record:
                    continue
                header, raw_name = record.split(b'\t', 1)
                mode, kind, oid = header.split()
                name = raw_name.decode('utf-8')
                if mode not in (b'100644', b'100755') or kind != b'blob':
                    raise ValueError('Nonregular committed device input: ' + name)
                seen.add(name)
                pairs.append((data / name, name))
            required = {'payload/' + name for name in PAYLOAD}
            required.update('flash/' + name for name in DEVICE)
            if not required <= seen:
                raise ValueError('Incomplete committed device inventory')
        pairs += [(data / 'vendor' / tag / name, 'flash/' + name) for name in DEVICE]
        self.verify(pairs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root')
    parser.add_argument('data')
    parser.add_argument('tag', choices=('windows-x86_64', 'linux-x86_64', 'linux-arm64',
                                       'macos-x86_64', 'macos-arm64'))
    parser.add_argument('--ref', default='HEAD')
    parser.add_argument('--frozen', action='store_true')
    args = parser.parse_args()
    try:
        guard = BlobGuard(args.root, args.ref)
        guard.devices(args.data, args.tag, args.frozen)
        print(guard.commit)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, 'Git blob verification failed: ' + str(exc) + '\n')
