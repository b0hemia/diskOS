# SPDX-License-Identifier: GPL-3.0-or-later
"""Archive a stable, qualified tree with a complete SHA256SUMS file."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import stat
import zipfile

PREFIX = 'diskos-installer-windows-x86_64'
INVENTORY = 'SHA256SUMS'
STAMP = (1980, 1, 1, 0, 0, 0)


def safe_name(name):
    for part in name.split('/'):
        if (not part or part in ('.', '..') or part.endswith((' ', '.'))
                or re.search(r'[\x00-\x1f\x7f\\:<>"|?*]', part)
                or re.fullmatch(r'(CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[0-9\u00b9\u00b2\u00b3]|LPT[0-9\u00b9\u00b2\u00b3])',
                                part.split('.')[0].rstrip(' .'), re.I)):
            raise ValueError('Unsafe Windows package name: ' + name)


def checked_stat(path):
    info = path.lstat()
    if (stat.S_ISLNK(info.st_mode)
            or getattr(info, 'st_file_attributes', 0) & 0x400):
        raise ValueError('Links/reparse points are forbidden')
    if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
        raise ValueError('Only regular files and directories are allowed')
    if stat.S_ISREG(info.st_mode) and info.st_nlink != 1:
        raise ValueError('Hard links are forbidden')
    return info


def inventory(root):
    files, seen = {}, set()
    def fail(error):
        raise error
    for folder, dirs, names in os.walk(root, followlinks=False, onerror=fail):
        for name in dirs + names:
            path = Path(folder) / name
            relative = path.relative_to(root).as_posix()
            safe_name(relative)
            key = relative.casefold()
            if key in seen:
                raise ValueError('Case-colliding package paths')
            seen.add(key)
            if stat.S_ISREG(checked_stat(path).st_mode):
                files[relative] = path
    manifest = files[INVENTORY].read_bytes()
    expected = {}
    for line in manifest.decode('utf-8').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        if not match:
            raise ValueError('Malformed SHA256SUMS record')
        sha, name = match.groups()
        safe_name(name)
        if name in expected or name == INVENTORY:
            raise ValueError('Duplicate/self-referencing inventory record')
        expected[name] = sha
    if not expected or set(expected) != set(files) - {INVENTORY}:
        raise ValueError('SHA256SUMS must cover exactly every package file except itself')
    expected[INVENTORY] = hashlib.sha256(manifest).hexdigest()
    return files, expected


def zip32_preflight(files):
    """Match zipfile's ZIP64 triggers for stored entries without extras/comments."""
    if len(files) > zipfile.ZIP_FILECOUNT_LIMIT:
        raise ValueError('Package file count would require ZIP64')
    offset = directory_size = 0
    for name, path in files.items():
        size = checked_stat(path).st_size
        # zipfile reserves 5% headroom even for ZIP_STORED in writestr().
        if size * 1.05 > zipfile.ZIP64_LIMIT:
            raise ValueError('Package file size would require ZIP64: ' + name)
        name_size = len((PREFIX + '/' + name).encode('utf-8'))
        if name_size > 65535:
            raise ValueError('Package filename exceeds ZIP header limit: ' + name)
        offset += zipfile.sizeFileHeader + name_size + size
        directory_size += zipfile.sizeCentralDir + name_size
    # The directory starts after all local headers and stored data. Bounding
    # that offset also bounds each entry's local-header offset.
    if offset > zipfile.ZIP64_LIMIT:
        raise ValueError('Package central directory offset would require ZIP64')
    if directory_size > zipfile.ZIP64_LIMIT:
        raise ValueError('Package central directory size would require ZIP64')


def archive(package, output):
    package, output = Path(package).absolute(), Path(output).absolute()
    # Inspect ancestors before resolving: a link to a tree is still a link.
    for path in (package, *package.parents):
        checked_stat(path)
    root = package.resolve()
    if not root.is_dir():
        raise ValueError('Package must be a directory')
    if output.resolve().is_relative_to(root):
        raise ValueError('ZIP output must be outside the package')
    files, expected = inventory(root)
    zip32_preflight(files)
    # Finish validation before creating output. Input must stay unchanged throughout.
    for name, path in files.items():
        with path.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != expected[name]:
                raise ValueError('Inventory hash mismatch: ' + name)
    with output.open('xb') as target, zipfile.ZipFile(
            target, 'w', compression=zipfile.ZIP_STORED, allowZip64=False) as zipped:
        for name in sorted(files):
            checked_stat(files[name])
            data = files[name].read_bytes()
            if hashlib.sha256(data).hexdigest() != expected[name]:
                raise ValueError('Package changed during archiving: ' + name)
            info = zipfile.ZipInfo(PREFIX + '/' + name, STAMP)
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            info.compress_type = zipfile.ZIP_STORED
            info.extra = info.comment = b''
            zipped.writestr(info, data)
    with output.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package')
    parser.add_argument('output')
    args = parser.parse_args()
    try:
        print(archive(args.package, args.output))
    except (OSError, ValueError, KeyError, zipfile.LargeZipFile) as exc:
        parser.exit(1, str(exc) + '\n')
