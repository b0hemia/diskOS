# SPDX-License-Identifier: GPL-3.0-or-later
"""Run real Homebrew pin and compile checks offline, without running the build."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(os.environ.get('DISKOS_SETUP_MACOS_SCRIPT', ROOT / 'vendor/setup-macos.sh'))
SOURCE = SCRIPT.read_text()
CHECK = re.search(r'^check_brew_version\(\) \{\n.*?^\}', SOURCE, re.M | re.S)[0]


class BrewVersionTests(unittest.TestCase):
    def check(self, versions, linked, want='5.8.4', name='xz'):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'brew with spaces'
            cellar = root / 'Cellar' / name
            cellar.mkdir(parents=True)
            for version in versions:
                (cellar / version).mkdir()
            opt = root / 'opt' / name
            opt.parent.mkdir()
            if linked is not None:
                opt.symlink_to(Path('../Cellar') / name / linked)
            commands = root / 'commands'
            commands.mkdir()
            brew = commands / 'brew'
            brew.write_text('#!/bin/bash\ncase "$*" in\n'
                            '  "list --versions $FAKE_NAME")\n'
                            '    [ -n "$FAKE_VERSIONS" ] || exit 1\n'
                            '    echo "$FAKE_NAME $FAKE_VERSIONS";;\n'
                            '  "--prefix $FAKE_NAME") echo "$FAKE_OPT";;\n'
                            '  "--cellar $FAKE_NAME") echo "$FAKE_CELLAR";;\n'
                            '  *) exit 99;;\nesac\n')
            brew.chmod(0o755)
            env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ['PATH'],
                       FAKE_NAME=name, FAKE_VERSIONS=' '.join(versions),
                       FAKE_OPT=str(opt), FAKE_CELLAR=str(cellar))
            env.pop('BASH_ENV', None)
            return subprocess.run(['bash', '-c', 'set -euo pipefail\n' + CHECK +
                                   '\ncheck_brew_version "$1" "$2"', 'check', name, want],
                                  env=env, capture_output=True, text=True)

    def refused(self, result, want, linked):
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn('expected Homebrew ' + want, result.stderr)
        self.assertIn('linked', result.stderr)
        self.assertIn(linked if linked == 'not installed or not linked' else '/' + linked + "'",
                      result.stderr)
        self.assertIn('Install/link the pinned version', result.stderr)

    def test_exact_single_keg(self):
        result = self.check(['5.8.4'], '5.8.4')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_multiple_kegs_pinned_linked(self):
        result = self.check(['5.6.3', '5.8.4', '5.8.3'], '5.8.4')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_multiple_kegs_wrong_linked(self):
        self.refused(self.check(['5.6.3', '5.8.4', '5.8.3'], '5.8.3'), 'xz 5.8.4', '5.8.3')

    def test_revision_must_match_published_pin(self):
        for versions in (['1.5.7_1'], ['1.5.7', '1.5.7_1']):
            with self.subTest(versions=versions):
                result = self.check(versions, '1.5.7_1', '1.5.7_1', 'zstd')
                self.assertEqual(result.returncode, 0, result.stderr)
        for linked in ('1.5.7', '1.5.7_2'):
            with self.subTest(linked=linked):
                self.refused(self.check(['1.5.7', '1.5.7_1', '1.5.7_2'], linked,
                                        '1.5.7_1', 'zstd'), 'zstd 1.5.7_1', linked)
        self.refused(self.check(['5.8.4_1'], '5.8.4_1'), 'xz 5.8.4', '5.8.4_1')

    def test_not_installed(self):
        self.refused(self.check([], None), 'xz 5.8.4', 'not installed or not linked')

    def test_unlinked_and_dangling_opt(self):
        for linked in (None, '5.8.3'):
            with self.subTest(linked=linked):
                self.refused(self.check(['5.8.4'], linked), 'xz 5.8.4',
                             'not installed or not linked')

    def test_wrong_base_version(self):
        self.refused(self.check(['5.8.3'], '5.8.3'), 'xz 5.8.4', '5.8.3')


class MacosBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='macos checks ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.brew = self.root / 'brew with spaces'
        self.libs = {}
        for name, version in (('libusb', '1.0.30'), ('lzo', '2.10'), ('lz4', '1.10.0'),
                              ('xz', '5.8.4'), ('zstd', '1.5.7_1')):
            keg = self.brew / 'Cellar' / name / version
            (keg / 'lib').mkdir(parents=True)
            opt = self.brew / 'opt' / name
            opt.parent.mkdir(exist_ok=True)
            opt.symlink_to(Path('../Cellar') / name / version)
            self.libs[name] = self.macho(keg / 'lib' / ('lib' + name + '.dylib'))
        self.old = self.macho(self.brew / 'Cellar/xz/5.8.3/lib/libxz.dylib')
        self.binary = self.macho(self.root / 'usbboot')
        self.log = self.root / 'otool.log'
        commands = self.root / 'commands'
        commands.mkdir()
        fakes = {
            'brew': '[ "$#" = 2 ] && [ "$1" = --prefix ] || exit 99\n'
                    '[ -d "$FAKE_BREW/opt/$2" ] || exit 98\n'
                    'printf "%s\\n" "$FAKE_BREW/opt/$2"\n',
            'otool': '[ "$#" = 2 ] && [ "$1" = -L ] || exit 99\n'
                     'printf "%s\\n" "$2" >> "$FAKE_OTOOL_LOG"\n'
                     'printf "%s:\\n" "$2"\n'
                     'while IFS= read -r dep; do\n'
                     '  printf "\\t%s (compatibility version 1.0.0, current version 1.0.0)\\n" "$dep"\n'
                     'done < "$2.deps"\n',
            'cc': 'printf "%s\\n" "$@"\n',
            # Also exercise the pre-fix command with the same spaced prefix.
            'pkg-config': '[ "$#" = 2 ] && [ "$2" = libusb-1.0 ] || exit 99\n'
                          'case "$1" in\n'
                          '  --cflags) printf -- "-I%s/include/libusb-1.0\\n" "$FAKE_BREW/opt/libusb";;\n'
                          '  --libs) printf -- "-L%s/lib -lusb-1.0\\n" "$FAKE_BREW/opt/libusb";;\n'
                          '  *) exit 98;;\nesac\n',
        }
        for name, body in fakes.items():
            path = commands / name
            path.write_text('#!/bin/bash\nset -euo pipefail\n' + body)
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ['PATH'],
                        FAKE_BREW=str(self.brew), FAKE_OTOOL_LOG=str(self.log))
        self.env.pop('BASH_ENV', None)

    def macho(self, path, *deps):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        self.dependencies(path, *deps)
        return path

    def dependencies(self, path, *deps):
        Path(str(path) + '.deps').write_text(''.join(str(dep) + '\n' for dep in deps))

    def shell(self, code, *args):
        return subprocess.run(['bash', '-c', 'set -euo pipefail\n' + code, 'check', *map(str, args)],
                              cwd=self.root, env=self.env, capture_output=True, text=True,
                              timeout=10)

    def check(self):
        functions = []
        for name in ('physical_file', 'check_pinned_macho'):
            match = re.search(r'^' + name + r'\(\) \{\n.*?^\}', SOURCE, re.M | re.S)
            self.assertIsNotNone(match, f'{SCRIPT}: missing {name}() provenance function')
            functions.append(match[0])
        return self.shell('\n'.join(functions) + '\nCHECKED_MACHO=""\n'
                          'check_pinned_macho "$1"', self.binary)

    def passed(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')

    def refused(self, result, message):
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(message, result.stderr)
        self.assertIn('rebuild against the pinned kegs', result.stderr)

    def test_all_pinned_lib_directories_pass(self):
        self.dependencies(self.binary, *self.libs.values())
        self.passed(self.check())
        self.assertCountEqual(self.log.read_text().splitlines(),
                              list(map(str, [self.binary, *self.libs.values()])))

    def test_inactive_older_keg_is_unpinned(self):
        self.dependencies(self.binary, self.old)
        self.refused(self.check(), "unpinned dylib '" + str(self.old) + "'")

    def test_transitive_inactive_keg_is_unpinned(self):
        self.dependencies(self.binary, self.libs['libusb'])
        self.dependencies(self.libs['libusb'], self.old)
        result = self.check()
        self.refused(result, "unpinned dylib '" + str(self.old) + "'")
        self.assertIn(str(self.libs['libusb']) + ' uses', result.stderr)

    def test_system_dependencies_are_ignored(self):
        self.dependencies(self.binary, '/usr/lib/libSystem.B.dylib',
                          '/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation',
                          self.libs['xz'])
        self.passed(self.check())
        self.assertCountEqual(self.log.read_text().splitlines(),
                              list(map(str, [self.binary, self.libs['xz']])))

    def test_unresolved_dependencies_are_refused(self):
        for dep in ('@rpath/libxz.dylib', str(self.root / 'missing.dylib')):
            with self.subTest(dep=dep):
                self.dependencies(self.binary, dep)
                self.refused(self.check(), 'unresolved dylib')

    def test_symlinked_dylib_resolves_to_physical_file(self):
        lib = self.libs['xz']
        relative = lib.parent / 'relative.dylib'
        relative.symlink_to(lib.name)
        absolute = self.root / 'absolute.dylib'
        absolute.symlink_to(self.brew / 'opt/xz/lib' / relative.name)
        self.dependencies(self.binary, absolute)
        self.passed(self.check())
        self.assertEqual(self.log.read_text().splitlines(), list(map(str, [self.binary, lib])))

    def test_symlink_inside_pinned_lib_cannot_hide_unpinned_file(self):
        alias = self.libs['xz'].parent / 'old.dylib'
        alias.symlink_to(self.old)
        self.dependencies(self.binary, self.brew / 'opt/xz/lib' / alias.name)
        self.refused(self.check(), "unpinned dylib '" + str(self.old) + "'")

    def test_dependency_cycle_terminates(self):
        first, second = self.libs['xz'], self.libs['zstd']
        self.dependencies(self.binary, first)
        self.dependencies(first, second)
        self.dependencies(second, self.brew / 'opt/xz/lib' / first.name)
        self.passed(self.check())
        self.assertEqual(self.log.read_text().splitlines(), list(map(str, [self.binary, first, second])))

    def test_usbboot_compile_preserves_spaced_include_and_library_arguments(self):
        match = re.search(r'^cc\b(?:[^\n]*\\\n)*[^\n]*', SOURCE, re.M)
        self.assertIsNotNone(match, f'{SCRIPT}: missing usbboot cc command')
        result = self.shell('OUT="$1"\n' + match[0], self.root / 'output with spaces')
        self.passed(result)
        prefix = str(self.brew / 'opt/libusb')
        self.assertEqual(result.stdout.splitlines(), [
            '-O2', '-std=c99', '-Wall', '-I' + prefix + '/include/libusb-1.0',
            '-o', str(self.root / 'output with spaces/usbboot'), 'src/usbboot/usbboot.c',
            '-L' + prefix + '/lib', '-lusb-1.0',
        ])


if __name__ == '__main__':
    unittest.main()
