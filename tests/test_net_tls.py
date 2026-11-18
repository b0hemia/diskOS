# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""TLS context, pinned CA bundle and exact-host URL allowlist. No network."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

from diskos_installer import errors, net_tls

ROOT = Path(__file__).resolve().parents[1]
GOOD = ('https://fiio-firmware.fiio.net/DISC/x.zip',
        'https://fiio-firmware.oss-us-west-1.aliyuncs.com/DISC/x.zip',
        'https://fiio-firmware.fiio.net:443/DISC/x.zip',
        'HTTPS://FIIO-Firmware.FiiO.net/DISC/x.zip',
        'https://fiio-firmware.fiio.net')
BAD = ('http://fiio-firmware.fiio.net/DISC/x.zip',
       'https://fiio-firmware.fiio.net:8443/DISC/x.zip',
       'https://fiio-firmware.fiio.net:/DISC/x.zip',
       'https://user@fiio-firmware.fiio.net/x',
       'https://user:pw@fiio-firmware.fiio.net/x',
       'https://fiio-firmware.fiio.net@evil.example/x',
       'https://evil.example\\@fiio-firmware.fiio.net/x',
       'https://fiio-firmware.fiio.net.evil.com/x',
       'https://evilfiio-firmware.fiio.net/x',
       'https://www.fiio-firmware.fiio.net/x',
       'https://sub.fiio-firmware.oss-us-west-1.aliyuncs.com/x',
       'https://fiio-firmware.fiio.net./x',
       'https://fiio-firmware.fiio.net%2eevil.com/x',
       'https://fiio-firmware.fiio.net\t.evil.com/x',
       'https://fiio-firmware.fiio.net /x',
       'https://f\u0131io-firmware.fiio.net/x',
       'https://fiio-firmware.fiio.net\uff0eevil.com/x',
       'https://xn--fiio-firmware-x.fiio.net/x',
       'https://127.0.0.1/x', 'https://[::1]/x', 'https://localhost/x',
       'https://forum.example/x', 'ftp://fiio-firmware.fiio.net/x',
       '//fiio-firmware.fiio.net/x', '/DISC/x.zip', '', 'https://', None, b'https://fiio-firmware.fiio.net/')


class UrlAllowlist(unittest.TestCase):
    def test_accept(self):
        for url in GOOD:
            with self.subTest(url=url):
                self.assertEqual(net_tls.check_url(url), url)

    def test_reject(self):
        for url in BAD:
            with self.subTest(url=url):
                with self.assertRaises(net_tls.UrlNotAllowed):
                    net_tls.check_url(url)

    def test_hosts_are_exact(self):
        self.assertEqual(sorted(net_tls.ALLOWED_HOSTS),
                         ['fiio-firmware.fiio.net', 'fiio-firmware.oss-us-west-1.aliyuncs.com'])
        self.assertIsInstance(net_tls.ALLOWED_HOSTS, frozenset)

    def test_catalogue_urls_pass(self):
        from diskos_installer import firmware_catalogue
        for pin in firmware_catalogue.CATALOGUE.values():
            net_tls.check_url(pin.url)


class ContextProperties(unittest.TestCase):
    def test_verified_context(self):
        ctx = net_tls.build_context()
        self.assertEqual(ctx.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(ctx.check_hostname)
        self.assertGreaterEqual(ctx.minimum_version, ssl.TLSVersion.TLSv1_2)
        self.assertGreater(ctx.cert_store_stats()['x509_ca'], 0)

    def test_certifi_bundle_is_loaded(self):
        import certifi
        with open(certifi.where(), 'rb') as stream:
            certs = stream.read().count(b'-----BEGIN CERTIFICATE-----')
        self.assertGreater(certs, 50)
        defaults = ssl.get_default_verify_paths()._replace(
            openssl_cafile=None, openssl_capath=None)
        with mock.patch.object(ssl.SSLContext, 'load_default_certs', lambda self, *a, **k: None), \
                mock.patch.object(ssl, 'get_default_verify_paths', return_value=defaults):
            ctx = net_tls.build_context(environ={})
        self.assertEqual(ctx.cert_store_stats()['x509_ca'], certs)

    def test_missing_certifi_data_fails_clearly(self):
        with mock.patch.dict(sys.modules, {'certifi': None}):
            with self.assertRaises(errors.PreflightError) as caught:
                net_tls.build_context()
        self.assertEqual(caught.exception.code, 'E150')
        self.assertIn('certifi', str(caught.exception))
        import certifi
        with mock.patch.object(certifi, 'where', return_value=os.path.join(os.sep, 'nope', 'cacert.pem')):
            with self.assertRaises(errors.PreflightError) as caught:
                net_tls.build_context()
        self.assertEqual(caught.exception.code, 'E150')

    def test_garbage_bundle_fails_clearly(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = os.path.join(tmp, 'bad.pem')
            with open(bad, 'w') as stream:
                stream.write('not a certificate\n')
            with self.assertRaises(errors.PreflightError) as caught:
                net_tls.build_context(bundle=bad)
        self.assertEqual(caught.exception.code, 'E150')

    def test_custom_ca_file_must_be_usable(self):
        for value in ('/nonexistent/ca.pem', ):
            with self.subTest(value=value):
                with self.assertRaises(errors.PreflightError) as caught:
                    net_tls.build_context(environ={'SSL_CERT_FILE': value})
                self.assertEqual(caught.exception.code, 'E151')
                self.assertIn('SSL_CERT_FILE', str(caught.exception))
        with tempfile.TemporaryDirectory() as tmp:
            bad = os.path.join(tmp, 'bad.pem')
            with open(bad, 'w') as stream:
                stream.write('junk\n')
            with self.assertRaises(errors.PreflightError):
                net_tls.build_context(environ={'SSL_CERT_FILE': bad})
        # Empty means unset; it never disables anything.
        ctx = net_tls.build_context(environ={'SSL_CERT_FILE': ''})
        self.assertEqual(ctx.verify_mode, ssl.CERT_REQUIRED)


def _write_ca(tmp, name):
    subprocess.run(('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', name + '.key',
                    '-out', name + '.pem', '-days', '2', '-subj', '/CN=diskos test ' + name,
                    '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign'),
                   cwd=tmp, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return os.path.join(tmp, name + '.pem')


def _subjects(ctx):
    return sorted(c['subject'][0][0][1] for c in ctx.get_ca_certs())


@unittest.skipUnless(shutil.which('openssl'), 'openssl command not available')
class TrustUnion(unittest.TestCase):
    def test_system_certifi_and_custom_roots_all_trusted_despite_env(self):
        with tempfile.TemporaryDirectory() as tmp:
            system = _write_ca(tmp, 'system')
            certifi_ca = _write_ca(tmp, 'certifi')
            custom = _write_ca(tmp, 'custom')
            nodir = os.path.join(tmp, 'no-such-dir')
            fake = ssl.DefaultVerifyPaths(cafile=None, capath=None, openssl_cafile_env='SSL_CERT_FILE',
                                          openssl_cafile=system, openssl_capath_env='SSL_CERT_DIR',
                                          openssl_capath=nodir)
            for env in ({'SSL_CERT_FILE': custom}, {'SSL_CERT_FILE': ''},
                        {'SSL_CERT_FILE': custom, 'SSL_CERT_DIR': nodir}):
                with self.subTest(env=env):
                    with mock.patch.dict(os.environ, env), \
                            mock.patch.object(ssl, 'get_default_verify_paths', return_value=fake):
                        ctx = net_tls.build_context(bundle=certifi_ca)
                        got = _subjects(ctx)
                    self.assertIn('diskos test system', got)
                    self.assertIn('diskos test certifi', got)
                    if env['SSL_CERT_FILE']:
                        self.assertIn('diskos test custom', got)
                    else:
                        self.assertNotIn('diskos test custom', got)


def make_pki(tmp):
    def run(*args):
        subprocess.run(('openssl',) + args, cwd=tmp, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key', '-out', 'ca.pem',
        '-days', '2', '-subj', '/CN=diskos test ca',
        '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign')
    run('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'srv.key', '-out', 'srv.csr', '-subj', '/CN=127.0.0.1')
    with open(os.path.join(tmp, 'ext.cnf'), 'w') as stream:
        stream.write('subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\n')
    run('x509', '-req', '-in', 'srv.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial',
        '-out', 'srv.pem', '-days', '2', '-extfile', 'ext.cnf')


@unittest.skipUnless(shutil.which('openssl'), 'openssl command not available')
class LocalServer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='diskos-tls-')
        self.addCleanup(shutil.rmtree, self.tmp, True)
        make_pki(self.tmp)
        server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server.load_cert_chain(os.path.join(self.tmp, 'srv.pem'), os.path.join(self.tmp, 'srv.key'))
        self.sock = socket.socket()
        self.addCleanup(self.sock.close)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(4)
        self.sock.settimeout(10)
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self.serve, args=(server,), daemon=True).start()

    def serve(self, server):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            try:
                with server.wrap_socket(conn, server_side=True) as tls:
                    tls.recv(1)
                    tls.sendall(b'k')
            except (OSError, ssl.SSLError):
                pass

    def connect(self, ctx, host='127.0.0.1'):
        with socket.create_connection(('127.0.0.1', self.port), timeout=10) as raw:
            with ctx.wrap_socket(raw, server_hostname=host) as tls:
                tls.sendall(b'x')
                return tls.recv(1)

    def test_injected_bundle_is_the_trust_anchor(self):
        ctx = net_tls.build_context(bundle=os.path.join(self.tmp, 'ca.pem'), environ={})
        self.assertEqual(self.connect(ctx), b'k')
        # The production bundle and system roots do not know this CA.
        with self.assertRaises(ssl.SSLCertVerificationError):
            self.connect(net_tls.build_context(environ={}))
        # Hostname checking stays on.
        with self.assertRaises(ssl.SSLCertVerificationError):
            self.connect(ctx, host='fiio-firmware.fiio.net')
        # The production allowlist still refuses the local server.
        with self.assertRaises(net_tls.UrlNotAllowed):
            net_tls.check_url('https://127.0.0.1:%d/x' % self.port)

    def test_custom_ca_file_is_additive(self):
        env = {'SSL_CERT_FILE': os.path.join(self.tmp, 'ca.pem')}
        self.assertEqual(self.connect(net_tls.build_context(environ=env)), b'k')


class PackagingPins(unittest.TestCase):
    def test_pins_have_licences_lock_and_spec_entries(self):
        pins = dict(re.findall(r'^([\w-]+)==([\d.]+)', (ROOT / 'requirements.txt').read_text(), re.M))
        self.assertIn('certifi', pins)
        self.assertIn('platformdirs', pins)
        lock = (ROOT / 'build/windows-build.lock').read_text()
        spec = (ROOT / 'build/diskos-installer.spec').read_text()
        notices = {'pyusb': 'BSD-3-Clause-pyusb.txt', 'pycryptodome': 'pycryptodome-LICENSE.txt',
                   'certifi': 'MPL-2.0-certifi.txt', 'platformdirs': 'MIT-platformdirs.txt'}
        mapped = {
            'certifi': ('MPL-2.0-certifi.txt', 'MPL-2.0.txt', 'MPL-2.0'),
            'platformdirs': ('MIT-platformdirs.txt', 'MIT'),
        }
        mpl = (ROOT / 'licenses/MPL-2.0.txt').read_bytes()
        self.assertEqual(hashlib.sha256(mpl).hexdigest(),
                         'fab3dd6bdab226f1c08630b1dd917e11fcb4ec5e1e020e2c16f83a0a13863e85')
        self.assertIn(b'Mozilla Public License Version 2.0', mpl)
        self.assertIn(b'Exhibit B', mpl)
        self.assertIn('MPL-2.0.txt', spec)
        for doc in ('licenses/README.md', 'NOTICE.md'):
            text = (ROOT / doc).read_text(encoding='utf-8')
            for name, needles in mapped.items():
                with self.subTest(doc=doc, package=name):
                    rows = [ln for ln in text.splitlines() if ln.startswith('|') and name in ln]
                    self.assertTrue(rows)
                    for needle in needles:
                        self.assertTrue(any(needle in ln for ln in rows), needle)
        for name, version in pins.items():
            with self.subTest(package=name):
                self.assertIn(name, notices)
                self.assertTrue((ROOT / 'licenses' / notices[name]).read_text(encoding='utf-8').strip())
                self.assertIn('%s==%s ' % (name, version), lock)
                if name in ('certifi', 'platformdirs'):
                    self.assertTrue(notices[name] in spec)

    def test_spec_ships_the_ca_bundle(self):
        spec = (ROOT / 'build/diskos-installer.spec').read_text()
        self.assertTrue('collect_data_files("certifi")' in spec and 'cacert.pem' in spec)


if __name__ == '__main__':
    unittest.main()
