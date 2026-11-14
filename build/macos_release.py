# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage the signed macOS app, corresponding source, notices and hashed archive."""
import gzip
import hashlib
import json
from pathlib import Path
import platform
import shutil
import sys
import tarfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from macos_inputs import VERSIONS
from macos_links import inside, macho

# These must be copied verbatim from the selected macOS runtime distributions.
# Existing Windows-specific notices do not establish the macOS runtime's terms.
MAC_NOTICES = ('CPython-LICENSE.txt', 'CPython-Incorporated-NOTICES.txt',
               'Tcl-license.terms.txt', 'Tcl-Incorporated-NOTICES.txt',
               'Tk-license.terms.txt', 'Tk-Incorporated-NOTICES.txt',
               'PyCryptodome-LICENSE.txt', 'PyCryptodome-NOTICES.txt',
               'RUNTIME_COMPONENTS.json')


def check_notices(root):
    for name in MAC_NOTICES:
        path = root / 'licenses/macos' / name
        if not path.is_file() or not path.stat().st_size:
            raise ValueError('Missing selected-runtime notice: ' + str(path))
    # Explicit component -> version, licence and exact notice/source paths.
    components = json.loads((root / 'licenses/macos/RUNTIME_COMPONENTS.json').read_text())
    required = {'CPython', 'Tcl', 'Tk', 'PyCryptodome', 'OpenSSL', 'libffi', 'zlib', 'bzip2', 'xz'}
    if not isinstance(components, dict) or not required <= components.keys():
        raise ValueError('Incomplete runtime component inventory')
    for component, row in components.items():
        if not row['version'] or not row['license'] or not row['notices']:
            raise ValueError('Incomplete runtime component record: ' + component)
        for name in row['notices'] + row.get('sources', []):
            path = root / name
            if not inside(path, root) or not path.is_file():
                raise ValueError('Missing runtime notice/source: ' + name)
        if component == 'xz' and not row.get('sources'):
            raise ValueError('Provide source for selected runtime liblzma')


def release(app, run, inputs):
    root = Path.cwd()
    check_notices(root)
    stage = run / ('diskos-installer-macos-' + platform.machine())
    stage.mkdir()
    shutil.copytree(app, stage / app.name, symlinks=True)
    for name in ('licenses', 'corresponding-source', 'src', 'spl-src', 'ui', 'flash'):
        shutil.copytree(root / name, stage / name, symlinks=True)
    for name in ('LICENSE', 'NOTICE.md', 'README.md', 'SPL_SOURCE.md', 'requirements.txt',
                 'docs/INSTALL.md', 'docs/MACOS_RECOVERY.txt',
                 'vendor/setup-macos.sh', 'build/NATIVE_TOOLS.md',
                 'build/MACOS_FROZEN_DEPENDENCIES.md', 'build/build-macos.sh',
                 'build/diskos-installer.spec', 'build/macos_inputs.py',
                 'build/macos_links.py', 'build/macos_release.py'):
        target = stage / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / name, target)
    # Payload helper recipes and UI/SPL source travel with their GPL binaries.
    for source in sorted((root / 'build').iterdir()):
        if source.is_file():
            shutil.copyfile(source, stage / 'build' / source.name)
    shutil.copyfile(root / 'docs/MACOS_RECOVERY.txt', stage / 'RECOVERY.txt')
    shutil.copyfile(inputs, stage / 'BUILD-INPUTS.json')
    # Validate notice/source paths in the delivered tree as well as the checkout.
    check_notices(stage)
    records, binaries = [], []
    for path in sorted(stage.rglob('*')):
        name = path.relative_to(stage).as_posix()
        if path.is_symlink():
            if not inside(path, stage):
                raise ValueError('External release symlink: ' + name)
            records.append('LINK  ' + name + ' -> ' + str(path.readlink()))
        elif path.is_file():
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            records.append(digest + '  ' + name)
            if macho(path):
                binaries.append(digest + '  ' + name)
    metadata = ['CPython=' + platform.python_version(),
                'Architecture=' + platform.machine(), 'Signing=ad-hoc',
                *[name + '=' + version for name, version in sorted(VERSIONS.items())],
                '', 'Bundled binary SHA256:', *binaries, '', 'Package files:', *records]
    (stage / 'MANIFEST.txt').write_text('\n'.join(metadata) + '\n', encoding='utf-8')
    # macOS apps require executable modes and internal framework symlinks, unlike
    # the Windows-only ZIP writer. Deterministic headers; signatures can vary per build.
    output = run / (stage.name + '.tar.gz')
    with output.open('xb') as stream, gzip.GzipFile(filename='', fileobj=stream, mode='wb', mtime=0) as gz:
        with tarfile.open(fileobj=gz, mode='w', format=tarfile.PAX_FORMAT) as tar:
            for path in [stage, *sorted(stage.rglob('*'))]:
                info = tar.gettarinfo(str(path), arcname=str(path.relative_to(stage.parent)))
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ''
                info.pax_headers = {}
                if info.isfile():
                    with path.open('rb') as source:
                        tar.addfile(info, source)
                else:
                    tar.addfile(info)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    (run / (output.name + '.sha256')).write_text(digest + '  ' + output.name + '\n', encoding='ascii')
    print(output)
    print(digest)


if __name__ == '__main__':
    release(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
