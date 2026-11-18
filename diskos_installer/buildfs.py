# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Opt-in filesystem interface for shared builder edits. No dispatch changes.

Paths are canonical relative bytes (b'/' denotes the root). Archive creation
modes are effective modes supplied by the caller, after its extraction policy.
Errors remain ordinary filesystem/ValueError exceptions for consumers to map.
"""
import io
import os
from pathlib import Path
import shutil
import stat
import time

from .pseudofs import CHUNK, path_ok


class FileSystem:
    def read_bytes(self, path):
        return b''.join(self.iter_bytes(path))

    def stat(self, path):
        return self.lstat(self.resolve(path))


class TreeFS(FileSystem):
    def __init__(self, root, clock=None):
        if os.name == 'nt':
            raise ValueError('image namespace is archive-only on Windows')
        self.root = Path(root).resolve()
        self.clock = int(time.time()) if clock is None else clock

    def _path(self, path):
        path_ok(path)
        return self.root if path == b'/' else self.root / os.fsdecode(path)

    def resolve(self, path, follow_final=True):
        p = self._path(path)
        if p == self.root:
            return b'/'
        try:
            target = p if follow_final else p.parent
            resolved = (Path(os.path.realpath(target, strict=os.path.ALLOW_MISSING))
                        if hasattr(os.path, 'ALLOW_MISSING') else target.resolve())
            if not follow_final:
                resolved /= p.name
        except RuntimeError as exc:
            raise ValueError('symlink cycle') from exc
        if not resolved.is_relative_to(self.root):
            raise ValueError('path escapes rootfs')
        rel = resolved.relative_to(self.root)
        return os.fsencode(rel.as_posix()) if rel.parts else b'/'

    def lstat(self, path):
        self.resolve(path, follow_final=False)
        return self._path(path).lstat()

    def iter_bytes(self, path):
        p = self._path(self.resolve(path))
        if not stat.S_ISREG(p.stat().st_mode):
            raise ValueError('read requires regular file')
        with p.open('rb') as src:
            while block := src.read(CHUNK):
                yield block

    def mkdir(self, path, mode=0o755):
        p = self._path(self.resolve(path))
        p.mkdir(mode=mode, parents=True, exist_ok=True)

    def replace_file(self, path, source, mode=None):
        self.resolve(path)
        p = self._path(path)
        if p.exists() and not p.is_symlink():
            info = p.stat()
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise ValueError('replacement requires unlinked regular file')
        p.parent.mkdir(parents=True, exist_ok=True)
        if p.is_symlink():
            p.unlink()
        with p.open('wb') as out:
            shutil.copyfileobj(io.BytesIO(source) if isinstance(source, bytes) else source, out, CHUNK)
        if mode is not None:
            p.chmod(mode)
        os.utime(p, (self.clock, self.clock))

    def unlink(self, path, follow_final=True):
        self.resolve(path, follow_final=follow_final)
        self._path(path).unlink()


class ArchiveFS(FileSystem):
    def __init__(self, archive, clock=None, directory_mode=0o755, file_mode=0o644):
        if any(e[0] == b'R' and e[7] for e in archive.entries.values()):
            raise ValueError('sparse imports are not qualified')
        self.archive = archive
        self.clock = int(time.time()) if clock is None else clock
        self.directory_mode, self.file_mode = directory_mode, file_mode

    def resolve(self, path, follow_final=True):
        path_ok(path)
        pending = [] if path == b'/' else path.split(b'/')
        parts, links = [], 0
        while pending:
            part = pending.pop(0)
            if part in (b'', b'.'):
                continue
            if part == b'..':
                if not parts:
                    raise ValueError('path escapes rootfs')
                parts.pop()
                continue
            p = b'/'.join(parts + [part])
            e = self.archive.entries.get(p)
            if e and e[0] == b'L':
                e = self.archive.entries[e[1]]
            if e and e[0] == b'S' and (pending or follow_final):
                links += 1
                if links > 40:
                    raise ValueError('symlink cycle or excessive depth')
                if e[5].startswith(b'/'):
                    raise ValueError('absolute symlink escapes rootfs')
                pending = e[5].split(b'/') + pending
                continue
            if pending and e and e[0] != b'D':
                raise NotADirectoryError(p)
            parts.append(part)
        return b'/'.join(parts) or b'/'

    def lstat(self, path):
        p = self.resolve(path, follow_final=False)
        e = self.archive.entries.get(p)
        if e is None:
            raise FileNotFoundError(p)
        origin = e[1] if e[0] == b'L' else p
        e = self.archive.entries[origin]
        kinds = {b'R': stat.S_IFREG, b'D': stat.S_IFDIR, b'S': stat.S_IFLNK,
                 b'C': stat.S_IFCHR, b'B': stat.S_IFBLK}
        kind = kinds[e[0]] if e[0] != b'I' else stat.S_IFIFO if e[5] == b'f' else stat.S_IFSOCK
        size = e[5] if e[0] == b'R' else len(e[5]) if e[0] == b'S' else 0
        nlink = 1 + sum(v == [b'L', origin] for v in self.archive.entries.values())
        return os.stat_result((kind | e[2], 0, 0, nlink, e[3], e[4], size, e[1], e[1], e[1]))

    def iter_bytes(self, path):
        p = self.resolve(path)
        info = self.lstat(p)
        if not stat.S_ISREG(info.st_mode):
            raise ValueError('read requires regular file')
        e = self.archive.entries[p]
        if e[0] == b'L':
            e = self.archive.entries[e[1]]
        yield from self.archive.chunks(e[6], e[5])

    def mkdir(self, path, mode=None):
        p = self.resolve(path)
        parts = [] if p == b'/' else p.split(b'/')
        for i in range(1, len(parts) + 1):
            parent = b'/'.join(parts[:i])
            e = self.archive.entries.get(parent)
            if e and e[0] != b'D':
                raise FileExistsError(parent)
            if e is None:
                effective = self.directory_mode if mode is None or i < len(parts) else mode
                self.archive.entries[parent] = [b'D', self.clock, effective, 0, 0]

    def replace_file(self, path, source, mode=None):
        self.resolve(path)
        p = self.resolve(path, follow_final=False)
        old = self.archive.entries.get(p)
        if old and old[0] != b'S' and (old[0] != b'R' or self.lstat(p).st_nlink != 1):
            raise ValueError('replacement requires unlinked regular file')
        self.mkdir(p.rpartition(b'/')[0] or b'/')
        if old and old[0] == b'S':
            self.unlink(p)
        self.archive.file(p, source, self.file_mode if mode is None else mode)
        e = self.archive.entries[p]
        e[1] = self.clock
        if mode is not None:
            e[2] = mode

    def unlink(self, path, follow_final=True):
        self.resolve(path, follow_final=follow_final)
        p = self.resolve(path, follow_final=False)
        e = self.archive.entries.get(p)
        if e is None:
            raise FileNotFoundError(p)
        if e[0] == b'D':
            raise IsADirectoryError(p)
        aliases = [q for q, v in self.archive.entries.items() if v == [b'L', p]]
        if aliases:
            origin = aliases[0]
            self.archive.entries[origin] = e
            for q in aliases[1:]:
                self.archive.entries[q] = [b'L', origin]
            if p in self.archive.attrs:
                self.archive.attrs[origin] = self.archive.attrs.pop(p)
        del self.archive.entries[p]
        self.archive.attrs.pop(p, None)


def archive_extraction_fs(archive, clock=None):
    """Mirror unsquashfs extraction modes before edits; never apply to output checks.

    POSIX follows the caller's euid/umask. Windows uses non-root/022, matching
    the qualified ordinary-user reference. Ownership is normalized at pack.
    Call once per freshly exported input, before creating ArchiveFS edits.
    """
    root = os.name != 'nt' and os.geteuid() == 0
    if os.name == 'nt':
        mask = 0o022
    else:
        mask = os.umask(0)
        os.umask(mask)
    for e in archive.entries.values():
        if e[0] != b'L' and not root:
            e[2] &= ~0o6000
            if e[0] == b'R' and not e[2] & 0o1000:
                e[2] &= ~mask
    return ArchiveFS(archive, clock=clock, directory_mode=0o777 & ~mask,
                     file_mode=0o666 & ~mask)
