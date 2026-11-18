# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Validated squashfs-tools 4.6.1 export adapter; not a general pseudo interpreter.

Kept opt-in until the image builder's shared edit and validation layer is ready.
Streaming storage is opt-in; sparse files and hardlink replacement remain
outside the supported build subset.
"""
import hashlib
import io
import re
import shutil
import tempfile
import weakref

CHUNK = 64 * 1024
MAX_HEADER = 16 * 1024 * 1024

MARK = b'#\n# START OF DATA - DO NOT MODIFY\n#\n'


def path_ok(p):
    if p != b'/' and (p.startswith(b'/') or any(x in (b'', b'.', b'..') for x in p.split(b'/'))):
        raise ValueError('noncanonical path')
    if not p or any(c < 32 or c == 127 for c in p):
        raise ValueError('control character in path')
    return p


def quote(p):
    path_ok(p)
    return re.sub(rb'([\s"\\#])', rb'\\\1', p)


def token(line):
    # Export uses backslash escaping, not shell quoting. Reject other grammars.
    p, i = bytearray(), 0
    while i < len(line) and line[i] != 32:
        if line[i] == 92:
            i += 1
            if i == len(line):
                raise ValueError('dangling escape')
        elif line[i] == 34:
            raise ValueError('unescaped quote')
        p.append(line[i])
        i += 1
    return path_ok(bytes(p)), line[i + 1:]


def number(s, base=10, maximum=(1 << 32) - 1):
    if not re.fullmatch(rb'[0-7]+' if base == 8 else rb'[0-9]+', s):
        raise ValueError('numeric fields only')
    n = int(s, base)
    if n > maximum:
        raise ValueError('out of range')
    return n


def xattr_value(value):
    # Native hex_decode underallocates; base64 is outside this proof's grammar.
    if value.startswith((b'0t', b'0T')):
        payload = value[2:]
        if not re.fullmatch(rb'(?:[\x20-\x5b\x5d-\x7e]|\\[0-3][0-7]{2})+', payload):
            raise ValueError('invalid octal-text xattr')
        return re.sub(rb'\\([0-3][0-7]{2})', lambda m: bytes([int(m[1], 8)]), payload)
    elif (not re.fullmatch(rb'[\x20-\x7e]+', value)
          or value.startswith((b'0x', b'0X', b'0s', b'0S', b'0b', b'0B'))):
        raise ValueError('unsupported xattr value encoding')
    return value


class Archive:
    def __init__(self, raw):
        self._store = tempfile.TemporaryFile()
        weakref.finalize(self, self._store.close)
        try:
            shutil.copyfileobj(io.BytesIO(raw) if isinstance(raw, bytes) else raw,
                               self._store, CHUNK)
            self._size = self._store.tell()
            self._store.seek(0)
            self._parse()
        except BaseException:
            self.close()
            raise

    def close(self):
        self._store.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _definitions(self):
        while self._store.tell() <= MAX_HEADER:
            line = self._store.readline(16385)
            if line == b'#\n':
                if line + self._store.read(len(MARK) - 2) != MARK:
                    raise ValueError('missing data marker')
                self._base = self._store.tell()
                return
            if not line.endswith(b'\n') or len(line) > 16384:
                raise ValueError('invalid definition line')
            yield line[:-1]
        raise ValueError('definition header too large')

    def _parse(self):
        self.entries, self.attrs = {}, {}
        end = 0
        for line in self._definitions():
            if not line or len(line) >= 16384 or any(c < 32 for c in line) or (len(line) - len(line.rstrip(b'\\'))) % 2:
                raise ValueError('invalid definition line')
            p, rest = token(line)
            t, _, tail = rest.partition(b' ')
            if t == b'x':
                name, equal, value = tail.partition(b'=')
                if not equal or not re.fullmatch(rb'(user|security|trusted)\.[A-Za-z0-9_.-]+', name):
                    raise ValueError('invalid xattr')
                attrs = self.attrs.setdefault(p, {})
                if name in attrs:
                    raise ValueError('duplicate xattr')
                attrs[name] = xattr_value(value)
                continue
            if p in self.entries:
                raise ValueError('duplicate definition')
            if t == b'L':
                target, junk = token(tail)
                if junk or target not in self.entries or self.entries[target][0] in (b'D', b'L'):
                    raise ValueError('invalid hardlink')
                e = [t, target]
            elif t in (b'D', b'R', b'S', b'C', b'B', b'I'):
                fields = tail.split(b' ', 4)
                if len(fields) < 4:
                    raise ValueError('missing metadata')
                nums = [number(fields[0]), number(fields[1], 8, 0o7777), number(fields[2]), number(fields[3])]
                extra = fields[4] if len(fields) == 5 else b''
                e = [t, *nums]
                if t == b'R':
                    v = extra.split(b' ')
                    if len(v) != 3:
                        raise ValueError('missing data fields')
                    size, off, sparse = number(v[0], maximum=1 << 40), number(v[1], maximum=1 << 40), number(v[2], maximum=1)
                    if off != end or off + size > self._size:
                        raise ValueError('bad offset or truncated data')
                    end += size
                    e += [size, off, sparse]
                elif t in (b'C', b'B'):
                    v = extra.split(b' ')
                    if len(v) != 2:
                        raise ValueError('invalid device')
                    if number(v[1], maximum=1048575) > 255:
                        raise ValueError('extended device minors mis-exported by 4.6.1')
                    e += [number(v[0], maximum=4095), number(v[1], maximum=255)]
                elif t == b'I':
                    if extra not in (b's', b'f'):
                        raise ValueError('invalid IPC type')
                    e += [extra]
                elif t == b'S':
                    if not extra or b'\0' in extra or len(extra) > 65535 or nums[1] != 0o777:
                        raise ValueError('invalid symlink')
                    e += [extra]
                elif extra:
                    raise ValueError('trailing fields')
            else:
                raise ValueError('unsupported/command definition')
            self.entries[p] = e
        if end != self._size - self._base or self.entries.get(b'/', [None])[0] != b'D':
            raise ValueError('unclaimed data or missing root')
        for p in self.entries:
            if p != b'/':
                parent = p.rpartition(b'/')[0] or b'/'
                if self.entries.get(parent, [None])[0] != b'D':
                    raise ValueError('missing/non-directory parent')
        if any(p not in self.entries or self.entries[p][0] == b'L' for p in self.attrs):
            raise ValueError('orphan/conflicting xattr')

    def manifest(self):
        groups = {}
        for p, e in self.entries.items():
            groups.setdefault(e[1] if e[0] == b'L' else p, []).append(p)
        result = {}
        for p, link in self.entries.items():
            origin = link[1] if link[0] == b'L' else p
            e = self.entries[origin]
            detail = e[5:]
            if e[0] == b'R':
                size, off, sparse = detail
                digest = hashlib.sha256()
                for chunk in self.chunks(off, size):
                    digest.update(chunk)
                detail = [size, digest.hexdigest(), sparse]
            result[p] = (e[:5], detail, sorted(groups[origin]), self.attrs.get(origin, {}))
        return result

    @property
    def data(self):
        # Legacy proof API; interface consumers use chunks()/write(), never this.
        return b''.join(self.chunks(0, self._size - self._base))

    def chunks(self, off, size):
        if off < 0 or size < 0 or off + size > self._size - self._base:
            raise ValueError('bad offset or truncated data')
        while size:
            self._store.seek(self._base + off)
            block = self._store.read(min(CHUNK, size))
            if not block:
                raise ValueError('truncated data')
            yield block
            off, size = off + len(block), size - len(block)

    def file(self, p, data, mode=0o644):
        path_ok(p)
        old = self.entries.get(p)
        if old and (old[0] != b'R' or any(e == [b'L', p] for e in self.entries.values())):
            raise ValueError('replacement requires unlinked regular file')
        meta = old[1:5] if old else [0, mode, 0, 0]
        self._store.seek(0, 2)
        start = self._store.tell()
        shutil.copyfileobj(io.BytesIO(data) if isinstance(data, bytes) else data,
                           self._store, CHUNK)
        self._size = self._store.tell()
        self.entries[p] = [b'R', *meta, self._size - start, start - self._base, 0]

    def symlink(self, p, target):
        path_ok(p)
        if not target or any(c < 32 or c == 127 for c in target) or p in self.entries:
            raise ValueError('insertion already exists')
        self.entries[p] = [b'S', 0, 0o777, 0, 0, target]

    def _write(self, out):
        off = 0
        for p, entry in self.entries.items():
            e = entry.copy()
            if e[0] == b'R':
                size, start, sparse = e[5:]
                e[6] = off
                off += size
            fields = [quote(e[1])] if e[0] == b'L' else [str(v).encode() if isinstance(v, int) else v for v in e[1:]]
            if e[0] not in (b'L',):
                fields[1] = format(e[2], 'o').encode()
            out.write(quote(p) + b' ' + e[0] + b' ' + b' '.join(fields) + b'\n')
            for name, value in self.attrs.get(p, {}).items():
                encoded = b'0t' + b''.join(('\\%03o' % c).encode() for c in value)
                out.write(quote(p) + b' x ' + name + b'=' + encoded + b'\n')
        out.write(MARK)
        for e in self.entries.values():
            if e[0] == b'R':
                for block in self.chunks(e[6], e[5]):
                    out.write(block)

    def write(self, out):
        # Validate a bounded-memory serialization before modifying caller output.
        with tempfile.TemporaryFile() as stage:
            self._write(stage)
            stage.seek(0)
            with Archive(stage):
                pass
            stage.seek(0)
            while block := stage.read(CHUNK):
                remaining = memoryview(block)
                while remaining:
                    written = out.write(remaining)
                    if (type(written) is not int
                            or not 0 < written <= len(remaining)):
                        raise OSError('invalid output write progress')
                    remaining = remaining[written:]

    def dump(self):
        out = io.BytesIO()  # Legacy proof API; streaming callers use write().
        self.write(out)
        return out.getvalue()
