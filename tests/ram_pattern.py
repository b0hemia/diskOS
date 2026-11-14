# SPDX-License-Identifier: GPL-3.0-or-later
"""Usage: py ram_pattern.py generate N file | compare expected readback.

Offset-tagged SHA256 blocks detect corruption, repeated blocks and wrong lengths.
Generation prints the SHA256; comparison checks every byte and prints both hashes.
"""
import hashlib
from pathlib import Path
import sys

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()

def main(args):
    if len(args) != 3 or args[0] not in ('generate', 'compare'):
        raise SystemExit(__doc__)
    if args[0] == 'generate':
        length = int(args[1])
        if length <= 0:
            raise SystemExit('N must be positive')
        path = Path(args[2])
        with path.open('xb') as f:
            for offset in range(0, length, 65536):
                block = b''.join(hashlib.sha256(i.to_bytes(8, 'little')).digest()
                                 for i in range(offset // 32, (offset + 65536) // 32))
                f.write(block[:min(65536, length - offset)])
        print(digest(path), path)
    else:
        left, right = map(Path, args[1:])
        print(digest(left), left)
        print(digest(right), right)
        with left.open('rb') as a, right.open('rb') as b:
            while True:
                x, y = a.read(1 << 20), b.read(1 << 20)
                if x != y:
                    raise SystemExit('FAIL: read-back differs')
                if not x:
                    break
        print('PASS: exact byte equality')


if __name__ == '__main__':
    main(sys.argv[1:])
