# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Shared filesystem contract, with small synthetic archives and scratch trees."""
import io
import os
from pathlib import Path
import stat
import tempfile
import tracemalloc
import unittest
from unittest import mock

from diskos_installer.buildfs import ArchiveFS, TreeFS
from diskos_installer.pseudofs import Archive, CHUNK, MARK

HEAD = (b'/ D 1 755 0 0\nd D 1 755 0 0\nd/f R 1 751 0 0 3 0 0\n'
        b'Case R 1 644 0 0 1 3 0\ncase R 1 644 0 0 1 4 0\n'
        b'alias L d/f\nparent S 1 777 0 0 d\nescape S 1 777 0 0 ../outside\n'
        b'absolute S 1 777 0 0 /outside\na S 1 777 0 0 b\nb S 1 777 0 0 a\n')


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'tree'
        self.root.mkdir()
        self.archive = Archive(HEAD + MARK + b'abcUL')
        self.addCleanup(self.archive.close)
        self.backends = [ArchiveFS(self.archive, clock=123)]
        if os.name != 'nt':
            (self.root / 'd').mkdir()
            (self.root / 'd/f').write_bytes(b'abc')
            (self.root / 'd/f').chmod(0o751)
            for p, data in [('Case', b'U'), ('case', b'L')]:
                (self.root / p).write_bytes(data)
            os.link(self.root / 'd/f', self.root / 'alias')
            for name, target in [('parent', 'd'), ('escape', '../outside'),
                                 ('absolute', '/outside'), ('a', 'b'), ('b', 'a')]:
                (self.root / name).symlink_to(target)
            self.backends.append(TreeFS(self.root, clock=123))

    def test_case_and_link_metadata(self):
        for fs in self.backends:
            with self.subTest(backend=type(fs).__name__):
                self.assertTrue(stat.S_ISDIR(fs.lstat(b'/').st_mode))
                self.assertEqual(fs.read_bytes(b'Case'), b'U')
                self.assertEqual(fs.read_bytes(b'case'), b'L')
                self.assertTrue(stat.S_ISLNK(fs.lstat(b'parent').st_mode))
                self.assertTrue(stat.S_ISDIR(fs.stat(b'parent').st_mode))
                self.assertEqual(fs.read_bytes(b'parent/f'), b'abc')
                self.assertEqual(fs.stat(b'alias').st_nlink, 2)

    def test_windows_tree_refusal_and_archive_names(self):
        with mock.patch('diskos_installer.buildfs.os.name', 'nt'):
            with self.assertRaisesRegex(ValueError, 'archive-only'):
                TreeFS(self.root)
            fs = ArchiveFS(self.archive)
            for path in (b'back\\slash', b'C:drive', b'file:stream', b'CON',
                         b'NUL', b'trailing.', b'trailing ', b'Case', b'case'):
                fs.replace_file(path, path)
                self.assertEqual(fs.resolve(path), path)
                self.assertEqual(fs.read_bytes(path), path)
            with Archive(self.archive.dump()) as other:
                self.assertEqual(other.manifest(), self.archive.manifest())

    def test_hardlinked_symlink_resolution(self):
        raw = (b'/ D 1 755 0 0\nd D 1 755 0 0\nd/f R 1 644 0 0 4 0 0\n'
               b'sub D 1 755 0 0\nsub/d D 1 755 0 0\n'
               b'sub/d/f R 1 644 0 0 5 4 0\n'
               b's S 1 777 0 0 d\nalias L s\nsub/alias L s\n'
               b'sf S 1 777 0 0 d/f\nfilealias L sf\nsub/filealias L sf\n'
               b'escape S 1 777 0 0 ../outside\nescapealias L escape\n'
               b'absolute S 1 777 0 0 /outside\nabsolutealias L absolute\n'
               b'cycle S 1 777 0 0 cyclealias\ncyclealias L cycle\n' + MARK + b'rootlocal')
        with Archive(raw) as archive:
            backends = [ArchiveFS(archive)]
            if os.name != 'nt':
                root = self.root / 'links'
                root.mkdir()
                for name in ('d', 'sub', 'sub/d'):
                    (root / name).mkdir()
                (root / 'd/f').write_bytes(b'root')
                (root / 'sub/d/f').write_bytes(b'local')
                for name, target, aliases in (
                        ('s', 'd', ('alias', 'sub/alias')),
                        ('sf', 'd/f', ('filealias', 'sub/filealias')),
                        ('escape', '../outside', ('escapealias',)),
                        ('absolute', '/outside', ('absolutealias',)),
                        ('cycle', 'cyclealias', ('cyclealias',))):
                    (root / name).symlink_to(target)
                    for alias in aliases:
                        os.link(root / name, root / alias, follow_symlinks=False)
                backends.append(TreeFS(root))
            for fs in backends:
                with self.subTest(backend=type(fs).__name__):
                    for prefix, content in ((b'', b'root'), (b'sub/', b'local')):
                        alias = prefix + b'alias'
                        self.assertTrue(stat.S_ISLNK(fs.lstat(alias).st_mode))
                        self.assertEqual(fs.lstat(alias).st_nlink, 3)
                        self.assertEqual(fs.resolve(alias, follow_final=False), alias)
                        self.assertEqual(fs.resolve(alias), prefix + b'd')
                        self.assertTrue(stat.S_ISDIR(fs.stat(alias).st_mode))
                        self.assertEqual(fs.read_bytes(alias + b'/f'), content)
                        self.assertEqual(fs.read_bytes(prefix + b'filealias'), content)
                        self.assertTrue(stat.S_ISREG(fs.stat(prefix + b'filealias').st_mode))
                        fs.replace_file(alias + b'/new', b'new')
                        self.assertEqual(fs.read_bytes(prefix + b'd/new'), b'new')
                    for alias in (b'escapealias', b'absolutealias', b'cyclealias'):
                        self.assertTrue(stat.S_ISLNK(fs.lstat(alias).st_mode))
                        for path in (alias, alias + b'/f'):
                            with self.assertRaises((ValueError, OSError)):
                                fs.resolve(path)

    def test_archive_short_writes(self):
        class ShortWriter(io.BytesIO):
            def write(self, data):
                return super().write(data[:7])

        self.backends[0].replace_file(b'large', b'x' * (CHUNK + 13))
        out = ShortWriter()
        self.archive.write(out)
        self.assertEqual(out.getvalue(), self.archive.dump())
        with Archive(out.getvalue()) as other:
            self.assertEqual(other.manifest(), self.archive.manifest())

    def test_archive_invalid_write_progress(self):
        for progress in (0, -1, None, True, 1.5, '1', CHUNK + 1):
            with self.subTest(progress=progress):
                out = mock.Mock()
                out.write.return_value = progress
                with self.assertRaises(OSError):
                    self.archive.write(out)
                out.write.assert_called_once()

    def test_archive_output_failure(self):
        for after in (0, 1):
            with self.subTest(successful_writes=after):
                failure = OSError('output failed')
                out = mock.Mock()
                out.write.side_effect = [1] * after + [failure]
                with self.assertRaises(OSError) as caught:
                    self.archive.write(out)
                self.assertIs(caught.exception, failure)
                self.assertEqual(out.write.call_count, after + 1)
                # A failed caller output must not damage the source archive.
                with Archive(self.archive.dump()) as other:
                    self.assertEqual(other.manifest(), self.archive.manifest())

    def test_modes_streaming_replacement_and_unlink(self):
        for fs in self.backends:
            with self.subTest(backend=type(fs).__name__):
                fs.mkdir(b'new')
                fs.replace_file(b'new/f', io.BytesIO(b'long initial'), mode=0o751)
                fs.replace_file(b'new/f', io.BytesIO(b'x'))
                self.assertEqual(fs.read_bytes(b'new/f'), b'x')
                self.assertEqual(stat.S_IMODE(fs.stat(b'new/f').st_mode), 0o751)
                self.assertEqual(fs.stat(b'new/f').st_mtime, 123)
                fs.replace_file(b'parent/new', b'new', mode=0o600)
                self.assertEqual(fs.read_bytes(b'd/new'), b'new')
                with self.assertRaises((ValueError, IsADirectoryError)):
                    fs.replace_file(b'd', b'bad')
                fs.unlink(b'new/f')
                with self.assertRaises(FileNotFoundError):
                    fs.stat(b'new/f')
                with self.assertRaises(IsADirectoryError):
                    fs.unlink(b'd')

    def test_escapes_cycles_and_hardlink_refusals(self):
        for fs in self.backends:
            with self.subTest(backend=type(fs).__name__):
                with self.assertRaises((ValueError, OSError)):
                    fs.resolve(b'a')
                for path in (b'escape/f', b'absolute/f', b'a/f', b'../outside', b'/bad'):
                    with self.assertRaises((ValueError, OSError)):
                        fs.replace_file(path, b'bad')
                for path in (b'd/f', b'alias', b'parent/f'):
                    with self.assertRaises(ValueError):
                        fs.replace_file(path, b'bad')
                self.assertEqual(fs.read_bytes(b'd/f'), b'abc')
                with self.assertRaises(ValueError):
                    fs.unlink(b'escape')
                fs.unlink(b'escape', follow_final=False)
                with self.assertRaises(FileNotFoundError):
                    fs.lstat(b'escape')

    def test_archive_offsets_and_roundtrip(self):
        for bad in (HEAD + MARK + b'abcU', HEAD + MARK + b'abcULx',
                    (HEAD + MARK + b'abcUL').replace(b'3 0 0', b'3 1 0')):
            with self.assertRaises(ValueError):
                Archive(io.BytesIO(bad))
        with Archive((HEAD + MARK + b'abcUL').replace(b'3 0 0', b'3 0 1')) as sparse:
            with self.assertRaises(ValueError):
                ArchiveFS(sparse)
        fs = self.backends[0]
        fs.unlink(b'd/f')  # Preserve the surviving hardlink's data and metadata.
        self.assertEqual(fs.read_bytes(b'alias'), b'abc')
        rejected = io.BytesIO(b'untouched')
        self.archive.entries[b'case'][2] = 0o10000
        with self.assertRaises(ValueError):
            self.archive.write(rejected)
        self.assertEqual(rejected.getvalue(), b'untouched')
        self.archive.entries[b'case'][2] = 0o644
        fs.replace_file(b'case', b'replacement')
        out = io.BytesIO()
        self.archive.write(out)
        with Archive(io.BytesIO(out.getvalue())) as other:
            self.assertEqual(other.manifest(), self.archive.manifest())

    def test_bounded_memory(self):
        size = 16 * 1024 * 1024
        with tempfile.TemporaryFile() as source, tempfile.TemporaryFile() as out:
            source.write(b'/ D 0 755 0 0\nf R 0 644 0 0 ' + str(size).encode() + b' 0 0\n' + MARK)
            for _ in range(size // CHUNK):
                source.write(b'x' * CHUNK)
            source.seek(0)
            tracemalloc.start()
            try:
                with Archive(source) as archive:
                    fs = ArchiveFS(archive)
                    self.assertEqual(sum(map(len, fs.iter_bytes(b'f'))), size)
                    with (self.root / 'large').open('wb') as tree_file:
                        for block in fs.iter_bytes(b'f'):
                            tree_file.write(block)
                    with (self.root / 'large').open('rb') as replacement:
                        fs.replace_file(b'f', replacement)
                    if os.name != 'nt':
                        tree = TreeFS(self.root)
                        with (self.root / 'large').open('rb') as replacement:
                            tree.replace_file(b'copy', replacement, mode=0o600)
                        self.assertEqual(sum(map(len, tree.iter_bytes(b'copy'))), size)
                    archive.manifest()
                    archive.write(out)
                    self.assertLess(tracemalloc.get_traced_memory()[1], 2 * 1024 * 1024)
            finally:
                tracemalloc.stop()
