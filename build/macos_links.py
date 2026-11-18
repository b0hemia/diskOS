# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate Mach-O closure; optionally replace internal loads with @loader_path."""
import argparse
import os
import posixpath
from pathlib import Path
import re
import subprocess

MAGICS = {bytes.fromhex(s) for s in ('feedface', 'feedfacf', 'cefaedfe', 'cffaedfe',
                                    'cafebabe', 'bebafeca', 'cafebabf', 'bfbafeca')}


def macho(path):
    with path.open('rb') as stream:
        return stream.read(4) in MAGICS


def dependencies(text):
    return [line.strip().split(' (compatibility version ', 1)[0]
            for line in text.splitlines()[1:] if ' (compatibility version ' in line]


def rpaths(text):
    return re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset \d+\)', text)


def system(path):
    return path.startswith(('/usr/lib/', '/System/Library/')) and posixpath.normpath(path) == path


def inside(path, root):
    path, root = Path(path).resolve(), Path(root).resolve()
    return path == root or root in path.parents


def expand(value, owner, executable):
    if value == '@loader_path':
        return owner.parent
    if value == '@executable_path':
        return executable.parent
    if value.startswith('@loader_path/'):
        return owner.parent / value[len('@loader_path/'):]
    if value.startswith('@executable_path/'):
        return executable.parent / value[len('@executable_path/'):]
    return Path(value) if value.startswith('/') else None


def resolve(dep, owner, executable, root, search=()):
    if system(dep):
        return None
    if dep.startswith('@rpath/'):
        candidates = [p if isinstance(p, Path) else expand(p, owner, executable) for p in search]
        candidates = [p / dep[len('@rpath/'):] for p in candidates if p is not None]
    else:
        path = expand(dep, owner, executable)
        candidates = [] if path is None else [path]
    hits = {p.resolve() for p in candidates if p.is_file() and inside(p, root)}
    if len(hits) != 1:
        raise ValueError(str(owner) + ': unresolved/external/ambiguous load ' + dep)
    return hits.pop()


def check_tree(root, executable, arch, rewrite=False, run=subprocess.run):
    root, executable = Path(root).resolve(), Path(executable).resolve()
    def output(*args):
        return run(list(args), check=True, capture_output=True, text=True).stdout
    exe_paths = rpaths(output('/usr/bin/otool', '-l', str(executable)))
    binaries = sorted({p.resolve() for p in root.rglob('*') if p.is_file() and macho(p)})
    if not binaries or executable not in binaries:
        raise ValueError('Missing Mach-O app executable')
    for path in root.rglob('*'):
        if path.is_symlink() and not inside(path, root):
            raise ValueError('External bundle symlink: ' + str(path))
    for path in binaries:
        if arch not in output('/usr/bin/lipo', '-archs', str(path)).split():
            raise ValueError('Wrong architecture: ' + str(path))
        loads = dependencies(output('/usr/bin/otool', '-L', str(path)))
        local_paths = rpaths(output('/usr/bin/otool', '-l', str(path)))
        search = [expand(p, path, executable) for p in local_paths]
        search += [expand(p, executable, executable) for p in exe_paths]
        search = [p for p in search if p is not None]
        for rp in local_paths:
            expanded = expand(rp, path, executable)
            if expanded is None or not inside(expanded, root):
                raise ValueError('External/unresolved LC_RPATH: ' + str(path) + ': ' + rp)
        # otool -D identifies LC_ID_DYLIB, which is not a dependency.
        ids = output('/usr/bin/otool', '-D', str(path)).splitlines()[1:]
        for dep in loads:
            if dep in ids:
                if rewrite:
                    output('/usr/bin/install_name_tool', '-id', '@loader_path/' + path.name, str(path))
                continue
            target = resolve(dep, path, executable, root, search)
            if target is not None and rewrite:
                relative = os.path.relpath(target, path.parent)
                output('/usr/bin/install_name_tool', '-change', dep, '@loader_path/' + relative, str(path))
    return binaries


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--arch', choices=('arm64', 'x86_64'), required=True)
    parser.add_argument('--rewrite', action='store_true')
    args = parser.parse_args()
    check_tree(args.root, args.executable, args.arch, args.rewrite)
