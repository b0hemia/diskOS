# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Offline Windows release gates; optional real artifact inventory."""
import ast
import hashlib
from html.parser import HTMLParser
import os
from pathlib import Path
import posixpath
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
FETCH = ROOT / 'build/fetch-windows-sources.sh'
SOURCE = 'msys2-runtime-3.6.10-6.src.tar.zst'
NATIVE = {'mksquashfs.exe', 'unsquashfs.exe', 'usbboot.exe',
          'bind_winusb.exe', 'msys-2.0.dll'}
PAYLOAD = {'mq_ui', 'S96diskos_select', 'S97diskos_install', 'S99usbserial',
           'diskos-debug.sh', 'dropbearmulti', 'diskos-rmguard', 'diskos-selected',
           'diskos-bootprobe', 'diskos-artdec', 'diskos-launch', 'diskos-verify.sh',
           'diskos-root.pub.pem', 'diskos-flashstate', 'S97diskos_names',
           'diskos-names', 'diskos-udhcpc', 'diskos-network-names-v1'}
VERSIONS = {'zlib': '1.3.2-1', 'gcc-libs': '15.3.0-1', 'gcc': '15.3.0-1',
            'mingw-w64-ucrt-x86_64-gcc': '16.2.0-4',
            'mingw-w64-ucrt-x86_64-crt': '14.0.0.r426.g4564ee4b5-1',
            'mingw-w64-ucrt-x86_64-winpthreads': '14.0.0.r426.g4564ee4b5-1'}
NOTICE_FILES = ('msys2-runtime-COPYING.NEWLIB.txt', 'msys2-runtime-BSD-NOTICES.txt')
UI_NOTICE_FILES = ('FTL-FreeType.txt', 'BSL-1.0-dlg.txt')
PYTHON_WINDOWS_NOTICE = 'CPython-3.12.10-Windows-LICENSE.txt'
PYTHON_INCORPORATED_NOTICE = 'CPython-3.12.10-Incorporated-Software.txt'
TK_WINDOWS_NOTICE = 'Tk-8.6.15-license.terms.txt'
TK_INCORPORATED_NOTICE = 'Tk-8.6.15-Incorporated-NOTICES.txt'
TK_RESOURCE_NOTICES = ('CC-BY-SA-3.0-Tango.txt',)
# Retained in the source checkout for the old artifact, omitted from new freezes.
TK_OMITTED_NOTICE = 'Adobe-Glyph-List-NOTICE.txt'
ZLIB_WINDOWS_NOTICE = 'zlib-1.3.1-LICENSE-README.txt'
TCL_NOTICE_FILES = ('Tcl-8.6.15-Bison-NOTICE.txt', 'Tcl-8.6.15-regex-NOTICE.txt',
                    'Tcl-8.6.15-libtommath-LICENSE.txt', 'Tcl-8.6.15-zlib-LICENSE.txt')
CRYPTO_NOTICE_FILES = ('PyCryptodome-3.23.0-Windows-LICENSE.rst',
                       'PyCryptodome-3.23.0-Windows-NOTICES.txt',
                       'PyCryptodome-3.23.0-Native-NOTICES.txt',
                       'PyCryptodome-3.23.0-libtom-LICENSE.txt',
                       'CC0-1.0-SipHash.txt')
PYTHON_NOTICE_FILES = ('PSF-Python.txt', PYTHON_WINDOWS_NOTICE, PYTHON_INCORPORATED_NOTICE)
MSYS_NOTICE_FILES = tuple('windows/msys2-runtime-' + name + '.txt' for name in (
    'CYGWIN_LICENSE', 'COPYING.LIB', 'COPYING', 'COPYING.NEWLIB', 'BSD-NOTICES'))
FROZEN_NOTICE_FILES = {
    'python312.dll': (*PYTHON_NOTICE_FILES, 'BSD-3-Clause-pyusb.txt'),
    'libcrypto-3.dll': ('Apache-2.0-OpenSSL.txt',),
    'libffi-8.dll': (PYTHON_WINDOWS_NOTICE,),
    'zlib1.dll': ('windows/zlib-LICENSE.txt', ZLIB_WINDOWS_NOTICE, PYTHON_WINDOWS_NOTICE),
    'tcl86t.dll': (PYTHON_WINDOWS_NOTICE, *TCL_NOTICE_FILES),
    'tk86t.dll': (PYTHON_WINDOWS_NOTICE, TK_WINDOWS_NOTICE, TK_INCORPORATED_NOTICE),
    'VCRUNTIME140.dll': (PYTHON_WINDOWS_NOTICE,),
    'VCRUNTIME140_1.dll': (PYTHON_WINDOWS_NOTICE,),
    'libusb-1.0.dll': ('windows/libusb-COPYING.txt',),
    'msys-2.0.dll': MSYS_NOTICE_FILES,
    'vendor/windows-x86_64/msys-2.0.dll': MSYS_NOTICE_FILES,
}
# Each measured stdlib extension needs the incorporated-software notices even
# when a test package contains the extension without the core DLL.
for _module in ('select', 'unicodedata', '_bz2', '_ctypes', '_decimal', '_hashlib',
                '_lzma', '_queue', '_socket', '_tkinter', '_uuid', '_wmi'):
    FROZEN_NOTICE_FILES[_module + '.pyd'] = PYTHON_NOTICE_FILES
FROZEN_NOTICE_FILES['_hashlib.pyd'] += ('Apache-2.0-OpenSSL.txt',)
FROZEN_NOTICE_FILES['_lzma.pyd'] += ('0BSD-xz.txt', 'XZ-COPYING.txt')
FROZEN_NOTICE_FILES['_tkinter.pyd'] += (*TCL_NOTICE_FILES, TK_WINDOWS_NOTICE, TK_INCORPORATED_NOTICE)
FROZEN_NOTICE_PATTERNS = {'Crypto/**/*.pyd': CRYPTO_NOTICE_FILES,
                          '_tk_data/icons.tcl': (TK_INCORPORATED_NOTICE, TK_RESOURCE_NOTICES[0])}


def assert_unused_tk_data_absent(data):
    for path in data.rglob('*'):
        relative = path.relative_to(data).as_posix().lower()
        if (relative in ('_tk_data/mkpsenc.tcl', '_tk_data/images')
                or relative.startswith('_tk_data/images/')):
            raise ValueError('Unused Tk data collected: ' + relative)


def native_notice_blocks(files):
    """Keep distinct complete upstream C comments in sorted source order."""
    blocks = []
    for name in sorted(files):
        for match in re.finditer(rb'/\*.*?\*/', files[name], re.S):
            block = match.group()
            if re.search(rb'copyright|public domain|permission|licen[sc]e|LibTomCrypt',
                         block, re.I) and block not in blocks:
                blocks.append(block)
    return blocks


def tk_source_notices(source):
    """Windows object lists, header superset and installed runtime script headers."""
    vc = (source / 'win/makefile.vc').read_bytes()
    aliases = dict(re.findall(rb'\\(\w+)\.obj:\s*\$\(GENERICDIR\)\\(\w+)\.c', vc))
    mingw = (source / 'win/Makefile.in').read_bytes()
    vc = vc[vc.index(b'XLIBOBJS ='):vc.index(b'TKSTUBOBJS =')]
    mingw = mingw[mingw.index(b'XLIB_OBJS ='):mingw.index(b'STUB_OBJS =')]
    objects = set(re.findall(rb'\\(\w+)\.obj', vc))
    other = set(re.findall(rb'\b(\w+)\.\$\(OBJEXT\)', mingw))
    if objects != other:
        raise ValueError('Tk Windows object lists differ')
    files = {}
    for name in sorted(objects):
        stem = aliases.get(name, name).decode()
        candidates = [source / directory / (stem + '.c') for directory in
                      ('generic', 'generic/ttk', 'win', 'xlib', 'unix')]
        found = [path for path in candidates if path.is_file()]
        if len(found) != 1:
            raise ValueError('Cannot resolve Tk object: ' + name.decode())
        files[str(found[0].relative_to(source))] = found[0].read_bytes()
    for directory in ('generic', 'win', 'xlib', 'compat'):
        for path in (source / directory).rglob('*.h'):
            files[str(path.relative_to(source))] = path.read_bytes()
    for path in (source / 'win/rc').glob('*.rc'):
        files[str(path.relative_to(source))] = path.read_bytes()
    blocks = native_notice_blocks(files)
    # Demos are not installed by either Windows runtime library target.
    for path in sorted((source / 'library').rglob('*.tcl')):
        if 'demos' in path.relative_to(source / 'library').parts:
            continue
        header = re.match(rb'(?:#[^\n]*\n|\r?\n)*', path.read_bytes()).group()
        if re.search(rb'copyright|licen[sc]e', header, re.I) and header not in blocks:
            blocks.append(header)
    blocks.append((source / 'library/images/README').read_bytes())
    return objects, blocks


def crypto_source_notices(archive):
    """Parse build declarations and includes as data; never execute sdist code."""
    prefix = 'pycryptodome-3.23.0/'
    files = {m.name[len(prefix):]: archive.extractfile(m).read()
             for m in archive.getmembers() if m.isfile() and m.name.startswith(prefix)}
    modules = {}
    for node in ast.walk(ast.parse(files['setup.py'])):
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                and node.func.id == 'Extension'):
            modules[ast.literal_eval(node.args[0])] = ast.literal_eval(
                next(k.value for k in node.keywords if k.arg == 'sources'))
    pending = [name for sources in modules.values() for name in sources]
    closure = set()
    while pending:
        name = pending.pop()
        if name in closure:
            continue
        closure.add(name)
        for include in re.findall(rb'^\s*#\s*include\s*["<]([^">]+)[">]', files[name], re.M):
            include = include.decode('ascii')
            for candidate in (posixpath.normpath(posixpath.join(posixpath.dirname(name), include)),
                              'src/' + include, 'src/libtom/' + include):
                if candidate in files:
                    pending.append(candidate)
                    break
    blocks = native_notice_blocks({name: files[name] for name in closure})
    return modules, closure, blocks, files['Doc/LEGAL/copy/LICENSE.libtom']


class IncorporatedText(HTMLParser):
    """Extract visible blocks from the exact-install incorporated licence section."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.depth = 0
        self.block = None
        self.skip = 0
        self.parts = []
        self.text = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'section':
            if self.depth:
                self.depth += 1
            elif attrs.get('id') == 'licenses-and-acknowledgements-for-incorporated-software':
                self.depth = 1
        if not self.depth:
            return
        if tag == 'a' and 'headerlink' in attrs.get('class', '').split():
            self.skip += 1
        if tag in ('h2', 'h3', 'p', 'pre'):
            self.block, self.text = tag, []

    def handle_data(self, data):
        if self.depth and self.block and not self.skip:
            self.text.append(data)

    def handle_endtag(self, tag):
        if not self.depth:
            return
        if tag == 'a' and self.skip:
            self.skip -= 1
        if tag == self.block:
            value = ''.join(self.text)
            self.parts.append(value.rstrip('\n') if tag == 'pre' else ' '.join(value.split()))
            self.block, self.text = None, []
        if tag == 'section':
            self.depth -= 1


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_runtime_notices():
    notices = {}
    for name in NOTICE_FILES:
        relative = 'licenses/windows/' + name
        path = ROOT / relative
        if not path.is_file() or not path.read_bytes().strip():
            raise ValueError('RELEASE BLOCKED: missing/empty canonical reconciled runtime notice: ' + name)
        notices[relative] = path.read_bytes()
    return notices


def function(name):
    return re.search(r'^' + name + r'\(\) \{\n.*?^\}\n',
                     FETCH.read_text(), re.S | re.M).group(0)


def records(text, section):
    body = text.split(section + ':\n', 1)[1].split('\n\n', 1)[0]
    result = {}
    for line in body.splitlines():
        match = re.fullmatch(r'([0-9a-f]{64}) [ *]([^\\\r\n]+)', line)
        if not match:
            raise ValueError('Malformed manifest hash record')
        sha, name = match.groups()
        if name.startswith('/') or any(p in ('', '.', '..') for p in name.split('/')) or ':' in name:
            raise ValueError('Unsafe manifest path')
        if name in result:
            raise ValueError('Duplicate manifest path')
        result[name] = sha
    if not result:
        raise ValueError('Empty manifest section')
    return result


def frozen_runtime_notices(package, data):
    assert_unused_tk_data_absent(data)
    # Preserve exact-install, wheel and incorporated-library notices.
    # This checks shipped bytes, not unresolved source versions or provenance.
    required = {name for binary, names in FROZEN_NOTICE_FILES.items()
                if (data / binary).is_file() for name in names}
    for pattern, names in FROZEN_NOTICE_PATTERNS.items():
        if any(path.is_file() for path in data.glob(pattern)):
            required.update(names)
    for name in sorted(required):
        relative = 'licenses/' + name
        canonical = ROOT / relative
        if not canonical.is_file() or not canonical.read_bytes().strip():
            raise ValueError('RELEASE BLOCKED: missing/empty canonical frozen runtime notice: ' + relative)
        candidates = [path for path in (package / relative, data / relative) if path.is_file()]
        if not candidates:
            raise ValueError('Missing frozen runtime notice: ' + relative)
        for path in candidates:
            if path.read_bytes() != canonical.read_bytes():
                raise ValueError('Changed notice: ' + relative)


def inventory(package, data):
    def locate(name):
        # Sidecars are preferred; otherwise use retained onedir/onefile data.
        path = package / name
        return path if path.is_file() else data / name

    payload = data / 'payload'
    if {p.name for p in payload.iterdir()} != PAYLOAD:
        raise ValueError('Unexpected payload inventory')
    for name in PAYLOAD:
        if (payload / name).read_bytes() != (ROOT / 'payload' / name).read_bytes():
            raise ValueError('Changed payload: ' + name)
    for relative, canonical in canonical_runtime_notices().items():
        notice = locate(relative)
        if not notice.is_file() or not notice.read_bytes().strip():
            raise ValueError('Missing reconciled runtime notices: ' + relative)
        if notice.read_bytes() != canonical:
            raise ValueError('Changed notice: ' + relative)
    frozen_runtime_notices(package, data)
    for relative in ['LICENSE', 'NOTICE.md', 'README.md'] + [str(p.relative_to(ROOT))
                                                for p in (ROOT / 'licenses').rglob('*')
                                                if p.is_file() and p.name != TK_OMITTED_NOTICE]:
        if locate(relative).read_bytes() != (ROOT / relative).read_bytes():
            raise ValueError('Changed notice: ' + relative)
    native = data / 'vendor/windows-x86_64'
    manifest = (native / 'MANIFEST.txt').read_text()
    if 'runtime-package=msys2-runtime 3.6.10-6\n' not in manifest:
        raise ValueError('Wrong runtime version')
    for name, version in VERSIONS.items():
        if [line for line in manifest.splitlines() if line.startswith(name + ' ')] != [name + ' ' + version]:
            raise ValueError('Notice/package version mismatch: ' + name)
    outputs = records(manifest, 'Output SHA256')
    if set(outputs) != NATIVE:
        raise ValueError('Unexpected native manifest inventory')
    if {p.name for p in native.iterdir()} != NATIVE | {'MANIFEST.txt', 'my_write6_dram.bin', 'disc_spl_lpddr3.bin'}:
        raise ValueError('Unexpected native directory inventory')
    for name, sha in outputs.items():
        if digest(native / name) != sha:
            raise ValueError('Native hash mismatch: ' + name)
    inputs = records(manifest, 'Input SHA256 (paths relative to installer)')
    required = {'src/usbboot/usbboot.c', 'build/build-windows-tools.sh',
                'build/build-windows-driver.sh', 'src/windows-driver/bind_winusb.c',
                'src/windows-driver/bind_winusb.manifest', 'src/windows-driver/bind_winusb.rc',
                'corresponding-source/squashfs-tools_4.6.1.orig.tar.gz',
                'corresponding-source/lzo2_2.10.orig.tar.gz',
                'corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2',
                'corresponding-source/msys2-squashfs.patch'}
    if not required <= inputs.keys():
        raise ValueError('Missing source/relink inputs')
    for name, sha in inputs.items():
        if digest(locate(name)) != sha:
            raise ValueError('Source hash mismatch: ' + name)
    assets = locate('corresponding-source/windows/' + SOURCE).parent
    if not (assets / SOURCE).is_file() or not (assets / (SOURCE + '.sig')).is_file():
        raise ValueError('Missing runtime source/signature')
    run = subprocess.run(['bash', str(FETCH), '--verify', str(assets)],
                         capture_output=True, text=True)
    if run.returncode:
        raise ValueError('Unauthenticated runtime source: ' + run.stderr)


@unittest.skipUnless(shutil.which('bash') and os.name != 'nt', 'needs POSIX bash')
class SignatureGate(unittest.TestCase):
    def test_signature_status(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / SOURCE
            source.write_bytes(b'fixture')
            source.with_name(SOURCE + '.sig').write_bytes(b'fixture signature')
            valid = '[GNUPG:] VALIDSIG fingerprint\n'
            trusted = '[GNUPG:] TRUST_FULLY 0 pgp\n'
            for status, expected in ((valid + trusted, True),
                    (valid + '[GNUPG:] TRUST_ULTIMATE 0 pgp\n', True),
                    (valid, False), (trusted, False),
                    (valid + trusted + valid, False),
                    (valid + '[GNUPG:] TRUST_UNDEFINED 0 pgp\n', False),
                    *[(valid + trusted + '[GNUPG:] ' + bad + '\n', False)
                      for bad in ('BADSIG', 'ERRSIG', 'EXPSIG', 'EXPKEYSIG', 'REVKEYSIG', 'NO_PUBKEY')]):
                code = function('verify_runtime_status') + 'verify_runtime_status "$STATUS"'
                env = dict(os.environ, MSYS2_GNUPGHOME=folder, STATUS=status)
                with self.subTest(status=status):
                    run = subprocess.run(['bash', '-c', code, 'test', str(source)],
                                         env=env, capture_output=True)
                    self.assertEqual(run.returncode == 0, expected)
            env.pop('MSYS2_GNUPGHOME')
            code = function('verify_runtime_signature') + 'verify_runtime_signature "$1"'
            run = subprocess.run(['bash', '-c', code, 'test', str(source)], env=env, capture_output=True)
            self.assertNotEqual(run.returncode, 0)

    def test_full_verifier_rejects_matching_hash_without_authentication(self):
        # Substitute only sha256sum; the real main must still call the signature gate.
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / SOURCE).write_bytes(b'fixture')
            (path / (SOURCE + '.sig')).write_bytes(b'not authenticated')
            pin = re.search(r'^runtime_sha256=(\w+)', FETCH.read_text(), re.M).group(1)
            (path / 'sha256sum').write_text('#!/bin/sh\nprintf "%s  %s\\n" "' + pin + '" "$2"\n')
            (path / 'sha256sum').chmod(0o755)
            env = dict(os.environ, PATH=folder + os.pathsep + os.environ['PATH'])
            env.pop('MSYS2_GNUPGHOME', None)
            run = subprocess.run(['bash', str(FETCH), '--verify', folder], env=env, capture_output=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn(b'independently trusted', run.stderr)


class ManifestGate(unittest.TestCase):
    def test_windows_runtime_notices_are_verbatim_and_allowlisted(self):
        notices = {
            PYTHON_WINDOWS_NOTICE:
                'e502c6b880ff58d614901495a9009c136539cd0b1e2a2abb8fc00b934c203419',
            TK_WINDOWS_NOTICE:
                '0d1e4405f6273f091732764ed89b57066be63ce64869be6c71ea337dc4f2f9b5',
            ZLIB_WINDOWS_NOTICE:
                'd106dcdb2a0b3087cb3a2380b9f00a0bbadc7eb835cd80408f94a2d78d69727d',
            PYTHON_INCORPORATED_NOTICE:
                '345c1a8708efb0ea43e34dc83a8b0bd05dce82decb4f5d8de8dee4932f7367d6',
            'BSD-3-Clause-pyusb.txt':
                '03e39fdcee9c18f2f9d0c3500a993ddeac050695eb81070ea41347587c76a7fe',
            TCL_NOTICE_FILES[0]:
                'b070e4a0019dfb8bb07a5960df02f3bb200c8ceb4c0d188369b4c1d802409baa',
            TCL_NOTICE_FILES[1]:
                '6d72f48f5f851ee6b39537d9ce4ee31e354956fb51b563f290e250bdb4304861',
            TCL_NOTICE_FILES[2]:
                '2fa64b163659f41965c9815882a8296d3d03ff546b76153e11445f9bdecf955a',
            TCL_NOTICE_FILES[3]:
                '845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243',
            CRYPTO_NOTICE_FILES[0]:
                '60b8958a9ef9b7ec512087b725555372175ed2b02b969f8725b8534fde48acdd',
            CRYPTO_NOTICE_FILES[1]:
                '8346eef7cc9f8a11bf8cfec9cb5fef6f5ea862274100dab1385ce3fd754f0131',
            TK_INCORPORATED_NOTICE:
                '98519bd25a1db194e2d1a9df1703a955b8574b45c96b01dcc309808c2bcda589',
            TK_RESOURCE_NOTICES[0]:
                '40b2aac4aee68f59f6f6dc9aeb1beec64c6abef4a925ca04c1767db85a554eda',
            CRYPTO_NOTICE_FILES[2]:
                '45d0abd51a4ff1c47d1e27d8fe020bb3c075027f2b10f4b5f6e0f6ab024a17ce',
            CRYPTO_NOTICE_FILES[3]:
                '10c41e1aaf84bd3546d1ff7ef89454817744fd2c18cdd80a137e802095448016',
            CRYPTO_NOTICE_FILES[4]:
                'a2010f343487d3f7618affe54f789f5487602331c0a8d03f49e9a7c547cf0499',
        }
        spec = (ROOT / 'build/diskos-installer.spec').read_text(encoding='ascii')
        allowlist = re.search(r'_license_files = """(.*?)"""\.split\(\)', spec, re.S).group(1).split()
        self.assertNotIn(TK_OMITTED_NOTICE, allowlist)
        notice = (ROOT / 'NOTICE.md').read_text(encoding='ascii')
        catalogue = (ROOT / 'licenses/README.md').read_text(encoding='ascii')
        for name, expected in notices.items():
            with self.subTest(notice=name):
                self.assertEqual(digest(ROOT / 'licenses' / name), expected)
                self.assertIn(name, allowlist)
                self.assertIn(name, notice)
                self.assertIn(name, catalogue)
        windows = (ROOT / 'licenses' / PYTHON_WINDOWS_NOTICE).read_bytes()
        base = (ROOT / 'licenses/PSF-Python.txt').read_bytes()
        self.assertTrue(windows.replace(b'\r\n', b'\n').startswith(base))
        tk = (ROOT / 'licenses' / TK_WINDOWS_NOTICE).read_bytes()
        self.assertIn(tk.rstrip(b'\r\n'), windows)

    def test_incorporated_notices_match_exact_install_html_when_present(self):
        location = os.environ.get('DISKOS_WINDOWS_CPYTHON_LICENSE_HTML')
        if not location:
            self.skipTest('Set DISKOS_WINDOWS_CPYTHON_LICENSE_HTML to the qualification input')
        source = Path(location)
        if not source.is_file():
            self.skipTest('Exact-install qualification HTML not present')
        self.assertEqual(digest(source),
                         '4633449165a1a6f16f5e2497458ecbf94f1f9a20f08606553ee5d3d948c1a12a')
        parser = IncorporatedText()
        parser.feed(source.read_text(encoding='utf-8'))
        self.assertEqual(len(parser.parts), 67)
        expected = ('\n\n'.join(parser.parts) + '\n').encode('utf-8')
        self.assertEqual((ROOT / 'licenses' / PYTHON_INCORPORATED_NOTICE).read_bytes(), expected)

    def test_exact_wheel_notices_when_present(self):
        location = os.environ.get('DISKOS_WINDOWS_WHEELS')
        if not location:
            self.skipTest('Set DISKOS_WINDOWS_WHEELS to the qualification input')
        wheels = Path(location)
        entries = (
            ('pyusb-1.3.1-py3-none-any.whl',
             'bf9b754557af4717fe80c2b07cc2b923a9151f5c08d17bdb5345dac09d6a0430',
             'pyusb-1.3.1.dist-info/LICENSE', 'BSD-3-Clause-pyusb.txt'),
            ('pycryptodome-3.23.0-cp37-abi3-win_amd64.whl',
             'c75b52aacc6c0c260f204cbdd834f76edc9fb0d8e0da9fbf8352ef58202564e2',
             'pycryptodome-3.23.0.dist-info/LICENSE.rst', CRYPTO_NOTICE_FILES[0]),
        )
        if not all((wheels / name).is_file() for name, *_ in entries):
            self.skipTest('Exact locked qualification wheels not present')
        lock = (ROOT / 'build/windows-build.lock').read_text(encoding='ascii')
        for name, sha, member, notice in entries:
            with self.subTest(wheel=name), ZipFile(wheels / name) as wheel:
                self.assertEqual(digest(wheels / name), sha)
                self.assertIn('--hash=sha256:' + sha, lock)
                self.assertEqual(wheel.read(member), (ROOT / 'licenses' / notice).read_bytes())
                if name.startswith('pycryptodome-'):
                    headers = []
                    for module in sorted(wheel.namelist()):
                        if module.startswith('Crypto/') and '/SelfTest/' not in module and module.endswith('.py'):
                            header = re.match(rb'(?:#[^\n]*\n|\r?\n)*', wheel.read(module)).group()
                            if b'Copyright' in header and header not in headers:
                                headers.append(header)
                    module = wheel.read('Crypto/Cipher/_mode_ocb.py')
                    start = module.index(b'"""')
                    end = module.index(b'"""', start + 3) + 3
                    headers.append(module[start:end] + b'\r\n')
                    self.assertEqual(b''.join(headers), (ROOT / 'licenses' / CRYPTO_NOTICE_FILES[1]).read_bytes())

    def test_tcl_notices_match_supplied_source_when_present(self):
        location = os.environ.get('DISKOS_WINDOWS_TCL_SOURCE')
        if not location:
            self.skipTest('Set DISKOS_WINDOWS_TCL_SOURCE to the qualification input')
        source = Path(location)
        if not (source / 'license.terms').is_file():
            self.skipTest('Tcl qualification source not present')
        base = (source / 'license.terms').read_bytes()
        windows = (ROOT / 'licenses' / PYTHON_WINDOWS_NOTICE).read_bytes().replace(b'\r\n', b'\n')
        self.assertIn(base.rstrip(b'\n'), windows)
        regex = (source / 'generic/regcomp.c').read_bytes()
        bison = (source / 'generic/tclDate.c').read_bytes()
        expected = (bison[:bison.index(b'/* C LALR(1)')],
                    regex[:regex.index(b'*/') + 2] + b'\n',
                    (source / 'libtommath/LICENSE').read_bytes(),
                    (source / 'compat/zlib/LICENSE').read_bytes())
        for name, content in zip(TCL_NOTICE_FILES, expected):
            self.assertEqual((ROOT / 'licenses' / name).read_bytes(), content)

    def test_tk_incorporated_notices_match_supplied_source_when_present(self):
        location = os.environ.get('DISKOS_WINDOWS_TK_SOURCE')
        if not location:
            self.skipTest('Set DISKOS_WINDOWS_TK_SOURCE to the qualification input')
        source = Path(location)
        if not (source / 'license.terms').is_file():
            self.skipTest('Tk qualification source not present')
        installed = (ROOT / 'licenses' / TK_WINDOWS_NOTICE).read_bytes()
        self.assertEqual(installed.replace(b'\r\n', b'\n'),
                         (source / 'license.terms').read_bytes())
        for directory in ('compat', 'xlib', 'xlib/X11'):
            self.assertEqual((source / directory / 'license.terms').read_bytes(),
                             (source / 'license.terms').read_bytes())
        objects, blocks = tk_source_notices(source)
        self.assertEqual(len(objects), 142)
        self.assertEqual(len(blocks), 221)
        self.assertEqual((ROOT / 'licenses' / TK_INCORPORATED_NOTICE).read_bytes(),
                         b''.join(block + b'\n' for block in blocks))
        archive = source.parent / 'tk8.6.15-src.tar.gz'
        if archive.is_file():
            self.assertEqual(digest(archive),
                             '550969f35379f952b3020f3ab7b9dd5bfd11c1ef7c9b7c6a75f5c49aca793fec')
            with tarfile.open(archive) as upstream:
                for member in upstream.getmembers():
                    if member.isfile() and member.name.endswith(
                            ('.c', '.h', '.rc', '.tcl', 'license.terms', 'library/images/README')):
                        relative = member.name.split('/', 1)[1]
                        self.assertEqual((source / relative).read_bytes(),
                                         upstream.extractfile(member).read(), relative)

    def test_crypto_native_notices_match_sdist_and_wheel_when_present(self):
        location = os.environ.get('DISKOS_WINDOWS_CRYPTO_SDIST')
        if not location:
            self.skipTest('Set DISKOS_WINDOWS_CRYPTO_SDIST to the qualification input')
        source = Path(location)
        if not source.is_file():
            self.skipTest('PyCryptodome qualification sdist not present')
        self.assertEqual(digest(source),
                         '447700a657182d60338bab09fdb27518f8856aecd80ae4c6bdddb67ff5da44ef')
        with tarfile.open(source) as upstream:
            modules, closure, blocks, libtom = crypto_source_notices(upstream)
            base = upstream.extractfile('pycryptodome-3.23.0/LICENSE.rst').read()
        self.assertEqual(base, (ROOT / 'licenses' / CRYPTO_NOTICE_FILES[0]).read_bytes().replace(b'\r\n', b'\n'))
        self.assertEqual(len(modules), 42)
        self.assertEqual(len(closure), 90)
        self.assertEqual(len(blocks), 26)
        self.assertTrue({'src/libtom/tomcrypt_des.c', 'src/keccak.c', 'src/blake2.c',
                         'src/siphash.c'} <= closure)
        self.assertEqual((ROOT / 'licenses' / CRYPTO_NOTICE_FILES[2]).read_bytes(),
                         b''.join(block + b'\n' for block in blocks))
        self.assertEqual((ROOT / 'licenses' / CRYPTO_NOTICE_FILES[3]).read_bytes(), libtom)
        wheels_location = os.environ.get('DISKOS_WINDOWS_WHEELS')
        wheel = (Path(wheels_location) / 'pycryptodome-3.23.0-cp37-abi3-win_amd64.whl'
                 if wheels_location else None)
        if wheel is not None and wheel.is_file():
            self.assertEqual(digest(wheel),
                             'c75b52aacc6c0c260f204cbdd834f76edc9fb0d8e0da9fbf8352ef58202564e2')
            with ZipFile(wheel) as binary:
                natives = {name[:-4].replace('/', '.') for name in binary.namelist()
                           if name.endswith('.pyd')}
            self.assertEqual(set(modules), natives)
        cc0 = Path('/usr/share/common-licenses/CC0-1.0')
        if cc0.is_file():
            self.assertEqual((ROOT / 'licenses' / CRYPTO_NOTICE_FILES[4]).read_bytes(), cc0.read_bytes())

    def test_tk_resource_notice_copies_when_local_inputs_present(self):
        cc = Path('/usr/share/doc/libglib2.0-data/copyright')
        if cc.is_file():
            content = cc.read_bytes()
            start = content.index(b'License: CC-BY-SA-3.0\n SPDX license expression')
            end = content.index(b'\nLicense:', start + 1)
            self.assertEqual((ROOT / 'licenses' / TK_RESOURCE_NOTICES[0]).read_bytes(),
                             content[start:end])

    def test_collected_tcltk_manifest_maps_all_supplied_file_groups_when_present(self):
        locations = [os.environ.get(key) for key in (
            'DISKOS_WINDOWS_TCLTK_MANIFEST', 'DISKOS_WINDOWS_TCL_SOURCE',
            'DISKOS_WINDOWS_TK_SOURCE')]
        if not all(locations):
            self.skipTest('Set Tcl/Tk manifest and source qualification inputs')
        manifest, tcl, tk = map(Path, locations)
        if not manifest.is_file() or not all((p / 'library').is_dir() for p in (tcl, tk)):
            self.skipTest('Collected Tcl/Tk manifest or source not present')
        self.assertEqual(digest(manifest),
                         '7982d3042462b71d35be56358ceae76294e2e9db25b2604b6117c1e259f6c071')
        counts, generated, forms = {}, [], {'raw': 0, 'CRLF': 0}
        for line in manifest.read_text().splitlines():
            name, size = line.replace('\\', '/').split('|')
            parts = name.split('/')
            if parts == ['_internal', '_tkinter.pyd']:
                continue
            root, relative = parts[1], '/'.join(parts[2:])
            group = relative.split('/')[0] if '/' in relative else '(root)'
            counts[(root, group)] = counts.get((root, group), 0) + 1
            tree = {'_tcl_data': tcl, '_tk_data': tk}[root]
            if root == '_tcl_data' and relative.startswith('opt0.4/'):
                relative = 'opt/' + relative[len('opt0.4/'):]
            path = tree / 'library' / relative
            if not path.is_file():
                generated.append(name)
                continue
            content = path.read_bytes()
            if len(content) == int(size):
                forms['raw'] += 1
            else:
                self.assertEqual(len(content.replace(b'\n', b'\r\n')), int(size), name)
                forms['CRLF'] += 1
        self.assertEqual(counts, {
            ('_tcl_data', '(root)'): 10, ('_tcl_data', 'encoding'): 82,
            ('_tcl_data', 'http1.0'): 2, ('_tcl_data', 'opt0.4'): 2,
            ('_tcl_data', 'msgs'): 127, ('_tcl_data', 'tzdata'): 609,
            ('_tk_data', '(root)'): 34, ('_tk_data', 'ttk'): 24,
            ('_tk_data', 'msgs'): 18, ('_tk_data', 'images'): 13})
        self.assertEqual(generated, ['_internal/_tk_data/pkgIndex.tcl'])
        self.assertEqual(forms, {'raw': 13, 'CRLF': 907})

    def test_windows_tk_filter_preserves_startup_and_rejects_recollection(self):
        tree = ast.parse((ROOT / 'build/diskos-installer.spec').read_text())
        names = {'is_unused_windows_tk_data', 'filter_windows_tk_data', 'verify_windows_tk_data'}
        helpers = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
        namespace = {}
        exec(compile(ast.Module(body=helpers, type_ignores=[]), '<Tk spec helpers>', 'exec'), namespace)
        kept = [(name, 'input', 'DATA') for name in (
            '_tk_data/tk.tcl', '_tk_data/icons.tcl', '_tk_data/tclIndex',
            '_tk_data/msgbox.tcl', '_tk_data/dialog.tcl', '_tk_data/ttk/clamTheme.tcl',
            '_tk_data/msgs/en.msg', '_tcl_data/images/keep', 'payload/images/keep',
            '_tk_data/mkpsenc.tcl.backup', '_tk_data/images-extra/keep')]
        removed = [(name, 'input', 'DATA') for name in (
            '_tk_data/mkpsenc.tcl', r'_tk_data\mkpsenc.tcl', '_TK_DATA/MKPSENC.TCL',
            '_tk_data/images/README', '_tk_data/images/logo.eps',
            r'_tk_data\images\pwrdLogo100.gif', '_tk_data/images/future/nested.asset')]
        self.assertEqual(namespace['filter_windows_tk_data'](kept + removed), kept)
        namespace['verify_windows_tk_data'](kept, [])
        for entry in removed:
            for kind in ('DATA', 'BINARY'):
                with self.subTest(dest=entry[0], kind=kind), self.assertRaisesRegex(
                        SystemExit, 'unused Tk data still collected'):
                    namespace['verify_windows_tk_data'](kept, [(entry[0], entry[1], kind)])
        # Exercise the actual post-Analysis Windows block without running a build.
        block = next(n for n in tree.body if isinstance(n, ast.If) and any(
            isinstance(c, ast.Call) and isinstance(c.func, ast.Name)
            and c.func.id == 'filter_windows_tk_data' for c in ast.walk(n)))
        for host in ('windows', 'linux', 'macos'):
            analysis = mock.Mock(datas=kept + removed, binaries=[])
            namespace.update(_os=host, a=analysis)
            exec(compile(ast.Module(body=[block], type_ignores=[]), '<Tk Analysis gate>', 'exec'), namespace)
            self.assertEqual(analysis.datas, kept if host == 'windows' else kept + removed)
        analysis = mock.Mock(datas=kept, binaries=[removed[0]])
        namespace.update(_os='windows', a=analysis)
        with self.assertRaisesRegex(SystemExit, 'unused Tk data still collected'):
            exec(compile(ast.Module(body=[block], type_ignores=[]), '<Tk Analysis gate>', 'exec'), namespace)
        collection_checks = [n for n in ast.walk(tree) if isinstance(n, ast.Call)
                             and isinstance(n.func, ast.Name) and n.func.id == 'verify_windows_tk_data'
                             and any(isinstance(a, ast.Attribute) and isinstance(a.value, ast.Name)
                                     and a.value.id == 'coll' and a.attr == 'toc' for a in n.args)]
        self.assertEqual(len(collection_checks), 1)

    def test_inventory_refuses_unused_tk_assets_without_any_notice_requirement(self):
        for relative in ('_tk_data/mkpsenc.tcl', '_tk_data/images/logo.eps',
                         '_tk_data/images/logo64.gif', '_tk_data/images/nested/new.asset',
                         '_TK_DATA/IMAGES/README'):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as folder:
                data = Path(folder) / '_internal'
                path = data / relative
                path.parent.mkdir(parents=True)
                path.write_bytes(b'forbidden asset')
                with self.assertRaisesRegex(ValueError, 'Unused Tk data collected'):
                    frozen_runtime_notices(Path(folder), data)
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder) / '_internal'
            # Even an empty images directory is forbidden.
            (data / '_tk_data/images').mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, 'Unused Tk data collected'):
                frozen_runtime_notices(Path(folder), data)

    def test_tk_script_notices_required_without_native_dlls(self):
        with tempfile.TemporaryDirectory() as folder:
            package = Path(folder)
            data = package / '_internal'
            for script in ('icons.tcl',):
                path = data / '_tk_data' / script
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'collected script fixture')
            for name in (TK_INCORPORATED_NOTICE, *TK_RESOURCE_NOTICES):
                sidecar = package / 'licenses' / name
                sidecar.parent.mkdir(parents=True, exist_ok=True)
                sidecar.write_bytes((ROOT / 'licenses' / name).read_bytes())
            frozen_runtime_notices(package, data)
            for name in (TK_INCORPORATED_NOTICE, *TK_RESOURCE_NOTICES):
                sidecar = package / 'licenses' / name
                original = sidecar.read_bytes()
                for wrong in (b'', b'changed notice'):
                    sidecar.write_bytes(wrong)
                    with self.assertRaisesRegex(ValueError, 'Changed notice'):
                        frozen_runtime_notices(package, data)
                sidecar.write_bytes(original)
                is_file = Path.is_file
                with mock.patch.object(Path, 'is_file', lambda p: p != sidecar and is_file(p)):
                    with self.assertRaisesRegex(ValueError, 'Missing frozen'):
                        frozen_runtime_notices(package, data)

    def test_complete_openssl_notice_and_python_base_terms(self):
        apache = ROOT / 'licenses/Apache-2.0-OpenSSL.txt'
        self.assertEqual(digest(apache),
                         'cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30')
        local_apache = Path('/usr/share/common-licenses/Apache-2.0')
        if local_apache.is_file():
            self.assertEqual(apache.read_bytes(), local_apache.read_bytes())
        self.assertEqual(digest(ROOT / 'licenses/PSF-Python.txt'),
                         '3b2f81fe21d181c499c59a256c8e1968455d6689d269aa85373bfb6af41da3bf')

    def test_each_frozen_component_requires_its_notices(self):
        with tempfile.TemporaryDirectory() as folder:
            repo = Path(folder) / 'repo'
            package = Path(folder) / 'package'
            data = package / '_internal'
            names = {name for notices in FROZEN_NOTICE_FILES.values() for name in notices}
            for name in names:
                for root in (repo, package, data):
                    path = root / 'licenses' / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(('canonical ' + name).encode('ascii'))
            is_file = Path.is_file
            with mock.patch(__name__ + '.ROOT', repo):
                for binary, notices in FROZEN_NOTICE_FILES.items():
                    selected = data / binary
                    # Simulate the measured native inventory without creating executables.
                    def measured_file(path):
                        if path in {data / n for n in FROZEN_NOTICE_FILES}:
                            return path == selected
                        return is_file(path)
                    with self.subTest(binary=binary), mock.patch.object(Path, 'is_file', measured_file):
                        frozen_runtime_notices(package, data)
                        for name in notices:
                            canonical = repo / 'licenses' / name
                            sidecar = package / 'licenses' / name
                            internal = data / 'licenses' / name
                            for absent in ((sidecar,), (internal,)):
                                with mock.patch.object(Path, 'is_file',
                                        lambda p: p not in absent and measured_file(p)):
                                    frozen_runtime_notices(package, data)
                            for absent, message in (((sidecar, internal), 'Missing frozen'),
                                                    ((canonical,), 'RELEASE BLOCKED')):
                                with mock.patch.object(Path, 'is_file',
                                        lambda p: p not in absent and measured_file(p)):
                                    with self.assertRaisesRegex(ValueError, message):
                                        frozen_runtime_notices(package, data)
                            for path in (sidecar, internal, canonical):
                                original = path.read_bytes()
                                for wrong in (b'', b'wrong licence'):
                                    path.write_bytes(wrong)
                                    with self.assertRaises(ValueError):
                                        frozen_runtime_notices(package, data)
                                path.write_bytes(original)

    def test_crypto_native_notice_gate_covers_any_collected_module(self):
        with tempfile.TemporaryDirectory() as folder:
            package = Path(folder)
            data = package / '_internal'
            for name in ('Cipher/_raw_aes.pyd', 'NewModule/_future.pyd'):
                binary = data / 'Crypto' / name
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_bytes(b'native fixture')
            for name in CRYPTO_NOTICE_FILES:
                sidecar = package / 'licenses' / name
                sidecar.parent.mkdir(parents=True, exist_ok=True)
                sidecar.write_bytes((ROOT / 'licenses' / name).read_bytes())
            frozen_runtime_notices(package, data)
            for name in CRYPTO_NOTICE_FILES:
                sidecar = package / 'licenses' / name
                original = sidecar.read_bytes()
                for wrong in (b'', b'changed notice'):
                    sidecar.write_bytes(wrong)
                    with self.assertRaisesRegex(ValueError, 'Changed notice'):
                        frozen_runtime_notices(package, data)
                sidecar.write_bytes(original)
                is_file = Path.is_file
                with mock.patch.object(Path, 'is_file', lambda p: p != sidecar and is_file(p)):
                    with self.assertRaisesRegex(ValueError, 'Missing frozen'):
                        frozen_runtime_notices(package, data)

    def test_vendored_freetype_and_dlg_notices(self):
        freetype = ROOT / 'ui/third_party/freetype-2.13.3'
        self.assertEqual((ROOT / 'licenses/FTL-FreeType.txt').read_bytes(),
                         (freetype / 'docs/FTL.TXT').read_bytes())
        bsl = (ROOT / 'licenses/BSL-1.0-dlg.txt').read_bytes()
        # Standard Boost Software License 1.0 text (matches upstream copies shipped in GCC libphobos and zlib).
        self.assertTrue(bsl.startswith(b'Boost Software License - Version 1.0 - August 17th, 2003\n'))
        self.assertEqual(hashlib.sha256(bsl).hexdigest(),
                         'c9bff75738922193e67fa726fa225535870d2aa1059f91452c411736284ad566')
        notice = (ROOT / 'NOTICE.md').read_text(encoding='ascii')
        catalogue = (ROOT / 'licenses/README.md').read_text(encoding='ascii')
        for name in UI_NOTICE_FILES:
            self.assertIn('licenses/' + name, notice)
            self.assertIn(name, catalogue)
        for name in ('dlg.h', 'output.h'):
            header = (freetype / 'include/dlg' / name).read_text()
            copyright_line = header.splitlines()[0].removeprefix('// ')
            self.assertIn(copyright_line, notice)
            self.assertIn('Boost Software License, Version 1.0', header)

    def test_supplied_dlg_license_matches_when_present(self):
        location = os.environ.get('DISKOS_WINDOWS_DLG_LICENSE')
        if not location:
            self.skipTest('Set DISKOS_WINDOWS_DLG_LICENSE to an external Boost Software License 1.0 copy')
        source = Path(location)
        if not source.is_file():
            self.skipTest('Supplied Boost Software License 1.0 copy not present')
        self.assertEqual(source.read_bytes(), (ROOT / 'licenses/BSL-1.0-dlg.txt').read_bytes())

    def test_parser_rejects_unsafe_duplicate_and_malformed_records(self):
        sha = 'a' * 64
        self.assertEqual(records('Output SHA256:\n' + sha + '  tool.exe\n', 'Output SHA256'),
                         {'tool.exe': sha})
        self.assertEqual(records('Output SHA256:\n' + sha + ' *tool.exe\n', 'Output SHA256'),
                         {'tool.exe': sha})
        for name in ('../tool.exe', '/tool.exe', 'C:/tool.exe', 'dir//tool.exe', 'dir/./tool.exe', 'dir\\tool.exe'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                records('Output SHA256:\n' + sha + '  ' + name, 'Output SHA256')
        for body in ('', sha + '  tool.exe\n' + sha + '  tool.exe', 'bad hash  tool.exe'):
            with self.assertRaises(ValueError):
                records('Output SHA256:\n' + body, 'Output SHA256')

    def test_package_layouts_and_tamper_gates(self):
        with tempfile.TemporaryDirectory() as folder:
            repo = Path(folder) / 'repo'
            repo.mkdir()
            for name in ('LICENSE', 'NOTICE.md', 'README.md', 'licenses/windows/notice.txt',
                         *['licenses/' + n for n in UI_NOTICE_FILES],
                         *['licenses/' + n for n in MSYS_NOTICE_FILES],
                         *['licenses/windows/' + n for n in NOTICE_FILES]):
                path = repo / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(('canonical ' + name).encode() if name.split('/')[-1] in NOTICE_FILES else b'notice')
            (repo / 'payload').mkdir()
            for name in PAYLOAD:
                (repo / 'payload' / name).write_bytes(b'payload')
            recipe = (ROOT / 'build/build-windows-tools.sh').read_text()
            input_names = re.search(r'\(cd "\$root" && sha256sum (.*?)\)', recipe, re.S).group(1)
            input_names = input_names.replace('\\\n', '').split()
            for layout in ('onefile', 'onedir'):
                package = Path(folder) / layout
                data = package / '_internal' if layout == 'onedir' else package
                native = data / 'vendor/windows-x86_64'
                native.mkdir(parents=True)
                for name in NATIVE | {'my_write6_dram.bin', 'disc_spl_lpddr3.bin'}:
                    (native / name).write_bytes(b'binary')
                (data / 'payload').mkdir()
                for name in PAYLOAD:
                    (data / 'payload' / name).write_bytes((repo / 'payload' / name).read_bytes())
                for name in ['LICENSE', 'NOTICE.md', 'README.md', 'licenses/windows/notice.txt',
                             *['licenses/' + n for n in UI_NOTICE_FILES],
                             *['licenses/' + n for n in MSYS_NOTICE_FILES],
                             *['licenses/windows/' + n for n in NOTICE_FILES]]:
                    path = package / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes((repo / name).read_bytes())
                for name in input_names:
                    path = data / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b'input')
                assets = data / 'corresponding-source/windows'
                assets.mkdir(parents=True)
                (assets / SOURCE).write_bytes(b'runtime')
                (assets / (SOURCE + '.sig')).write_bytes(b'signature')
                manifest = 'runtime-package=msys2-runtime 3.6.10-6\n\n'
                manifest += ''.join(n + ' ' + v + '\n' for n, v in VERSIONS.items()) + '\n'
                manifest += 'Input SHA256 (paths relative to installer):\n'
                manifest += ''.join(digest(data / n) + '  ' + n + '\n' for n in input_names)
                manifest += '\nOutput SHA256:\n'
                manifest += ''.join(digest(native / n) + '  ' + n + '\n' for n in sorted(NATIVE))
                (native / 'MANIFEST.txt').write_text(manifest)
                with mock.patch(__name__ + '.ROOT', repo), mock.patch('subprocess.run') as verify:
                    verify.return_value.returncode = 0
                    for name in UI_NOTICE_FILES:
                        notice = package / 'licenses' / name
                        original = (repo / 'licenses' / name).read_bytes()
                        for wrong in (b'', b'changed licence'):
                            notice.write_bytes(wrong)
                            with self.assertRaisesRegex(ValueError, 'Changed notice'):
                                inventory(package, data)
                        notice.write_bytes(original)
                        read_bytes = Path.read_bytes
                        # Simulate an absent packaged notice without removing files.
                        def missing_notice(path):
                            if path in (notice, data / 'licenses' / name):
                                raise FileNotFoundError(str(path))
                            return read_bytes(path)
                        with mock.patch.object(Path, 'read_bytes', missing_notice):
                            with self.assertRaises(FileNotFoundError):
                                inventory(package, data)
                    for name in NOTICE_FILES:
                        notice = package / 'licenses/windows' / name
                        canonical = repo / 'licenses/windows' / name
                        original = canonical.read_bytes()
                        for wrong in (b'x', b'nonempty wrong notice'):
                            notice.write_bytes(wrong)
                            with self.assertRaisesRegex(ValueError, 'Changed notice'):
                                inventory(package, data)
                        notice.write_bytes(b'')
                        with self.assertRaisesRegex(ValueError, 'Missing reconciled'):
                            inventory(package, data)
                        notice.write_bytes(original)
                        canonical.write_bytes(b'')
                        with self.assertRaisesRegex(ValueError, 'RELEASE BLOCKED'):
                            inventory(package, data)
                        canonical.write_bytes(original)
                        is_file = Path.is_file
                        with mock.patch.object(Path, 'is_file',
                                               lambda p: p != canonical and is_file(p)):
                            with self.assertRaisesRegex(ValueError, 'RELEASE BLOCKED'):
                                inventory(package, data)
                    inventory(package, data)
                    self.assertIn('--verify', verify.call_args.args[0])
                    # Runtime verification must resolve the helper's hashed originals
                    # from _internal, rather than from optional release sidecars.
                    from diskos_installer import bundle
                    with mock.patch.object(bundle, 'resource_root', return_value=str(data)), \
                            mock.patch.object(bundle, 'vendor_dir', return_value=str(native)), \
                            mock.patch.object(bundle, '_verify_windows_dlls') as dlls:
                        bundle.verify_windows_flash_bundle()
                        bundle.verify_windows_driver_bundle()
                        dlls.assert_called_once_with(str(native / 'bind_winusb.exe'))
                    for directory, allowed, message in ((native, NATIVE | {'MANIFEST.txt',
                            'my_write6_dram.bin', 'disc_spl_lpddr3.bin'}, 'native directory'),
                            (data / 'payload', PAYLOAD, 'payload')):
                        entries = [directory / name for name in allowed]
                        iterdir = Path.iterdir
                        for wrong in (entries[:-1], entries + [directory / 'scratch']):
                            with mock.patch.object(Path, 'iterdir',
                                    lambda p: iter(wrong) if p == directory else iterdir(p)):
                                with self.assertRaisesRegex(ValueError, message):
                                    inventory(package, data)
                    for path in [native / n for n in NATIVE] + [data / n for n in input_names] + [
                            package / 'NOTICE.md', data / 'payload/diskos-root.pub.pem']:
                        original = path.read_bytes()
                        path.write_bytes(b'tampered')
                        with self.assertRaises(ValueError):
                            inventory(package, data)
                        path.write_bytes(original)
                    (native / 'MANIFEST.txt').write_text(manifest.replace('3.6.10-6', '3.6.10-7'))
                    with self.assertRaises(ValueError):
                        inventory(package, data)
                    (native / 'MANIFEST.txt').write_text(manifest)
                    verify.return_value.returncode = 1
                    verify.return_value.stderr = 'untrusted'
                    with self.assertRaisesRegex(ValueError, 'Unauthenticated'):
                        inventory(package, data)

    def test_canonical_reconciled_notices_ready(self):
        try:
            canonical_runtime_notices()
        except ValueError as error:
            self.skipTest(str(error))

    def test_release_artifact_when_present(self):
        selected = os.environ.get('DISKOS_WINDOWS_PACKAGE')
        dist = ROOT / 'build/dist'
        package = Path(selected) if selected else dist / 'diskos-installer'
        onefile = dist / 'diskos-installer.exe'
        if not selected and not (package / 'diskos-installer.exe').is_file():
            if not onefile.exists():
                self.skipTest('Slice 8 Windows artifact not built')
            package = dist
        self.assertTrue(package.is_dir(), 'Selected Windows package is missing')
        self.assertTrue((package / 'diskos-installer.exe').is_file(), 'Missing Windows app')
        extracted = os.environ.get('DISKOS_WINDOWS_EXTRACTED')
        data = Path(extracted) if extracted else package / '_internal'
        if not data.is_dir():
            data = package
        inventory(package, data)


if __name__ == '__main__':
    unittest.main()
