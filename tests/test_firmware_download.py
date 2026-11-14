# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Streaming firmware download against a local fake HTTPS server. No real network."""
import errno
import hashlib
import http.client
import http.server
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
from unittest import mock

from diskos_installer import firmware_catalogue as fw, firmware_download as dl, net_tls, state

PAYLOAD = bytes((i * 7 + i // 251) & 0xFF for i in range(400000))
URL = 'https://fiio-firmware.fiio.net/DISC/test_v240.zip'
OTHER = 'https://fiio-firmware.oss-us-west-1.aliyuncs.com/DISC/other.zip'
PKI = {}


def setUpModule():
    if not shutil.which('openssl'):
        raise unittest.SkipTest('openssl command not available')
    PKI['dir'] = tmp = tempfile.mkdtemp(prefix='diskos-dl-')

    def run(*args):
        subprocess.run(('openssl',) + args, cwd=tmp, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2',
        '-subj', '/CN=diskos test ca', '-addext', 'basicConstraints=critical,CA:TRUE',
        '-addext', 'keyUsage=critical,keyCertSign')
    run('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'srv.key', '-out', 'srv.csr', '-subj', '/CN=fiio-firmware.fiio.net')
    Path(tmp, 'ext.cnf').write_text('subjectAltName=DNS:fiio-firmware.fiio.net,'
                                    'DNS:fiio-firmware.oss-us-west-1.aliyuncs.com\nbasicConstraints=CA:FALSE\n')
    run('x509', '-req', '-in', 'srv.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial',
        '-out', 'srv.pem', '-days', '2', '-extfile', 'ext.cnf')


def tearDownModule():
    shutil.rmtree(PKI.get('dir', ''), True)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.log.append((self.path, self.headers.get('Host'), self.headers.get('Range'),
                                self.headers.get('Accept-Encoding')))
        try:
            self.server.routes[self.path.split('/')[-1]](self)
        except (OSError, ssl.SSLError):
            self.close_connection = True

    def send_body(self, data, length=None, status=200):
        self.send_response(status)
        self.send_header('Content-Length', str(len(data) if length is None else length))
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass  # expected: clients abort on purpose


class DownloadTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        env = {k: v for k, v in os.environ.items() if k.lower() not in ('http_proxy', 'https_proxy', 'all_proxy')}
        env.update(DISKOS_INSTALLER_HOME=str(self.root / 'state'), no_proxy='*')
        patch = mock.patch.dict(os.environ, env, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        pin = fw.Firmware(URL, len(PAYLOAD), hashlib.sha256(PAYLOAD).hexdigest(), 'FiiO V2.40')
        patch = mock.patch.dict(fw.__dict__, CATALOGUE=dict(fw.CATALOGUE, **{'240': pin}))
        patch.start()
        self.addCleanup(patch.stop)
        from diskos_installer.__main__ import _Lock
        if os.name == 'nt':
            self.skipTest('local lock fixture is POSIX only')
        self.lock = _Lock(windows=False)
        self.lock.path = str(self.root / 'run.lock')
        self.lock.__enter__()
        self.addCleanup(self.lock.__exit__, None, None, None)
        self.release = threading.Event()
        self.addCleanup(self.release.set)
        self.ctx = net_tls.build_context(bundle=os.path.join(PKI['dir'], 'ca.pem'), environ={})
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(os.path.join(PKI['dir'], 'srv.pem'), os.path.join(PKI['dir'], 'srv.key'))
        self.server = Server(('127.0.0.1', 0), Handler)
        self.server.log = []
        self.server.routes = {'test_v240.zip': lambda h: h.send_body(PAYLOAD)}
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True, do_handshake_on_connect=False)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        real = socket.create_connection
        patch = mock.patch.object(socket, 'create_connection',
                                  lambda address, *a, **k: real(('127.0.0.1', self.port), *a, **k))
        patch.start()
        self.addCleanup(patch.stop)
        self.firmware_dir = Path(state.state_dir()) / 'firmware'

    def fetch(self, **kw):
        kw.setdefault('context', self.ctx)
        return dl.download('240', self.lock, **kw)

    def assert_clean(self):
        self.assertEqual(list(self.firmware_dir.iterdir()) if self.firmware_dir.exists() else [], [])
        self.assertIsNone(fw.cached('240', self.lock))

    def test_success_streams_exact_bytes_and_promotes(self):
        seen = []
        path = self.fetch(progress=lambda d, t: seen.append((d, t)))
        self.assertEqual(Path(path).read_bytes(), PAYLOAD)
        self.assertEqual(Path(path).name, 'test_v240.zip')
        self.assertEqual(fw.cached('240', self.lock), path)
        self.assertEqual([p.name for p in self.firmware_dir.iterdir()], ['test_v240.zip'])
        self.assertEqual(seen[0], (0, len(PAYLOAD)))
        self.assertEqual(seen[-1], (len(PAYLOAD), len(PAYLOAD)))
        self.assertTrue(all(b[0] > a[0] for a, b in zip(seen, seen[1:])))
        self.assertGreater(len(seen), 2)
        self.assertEqual(self.server.log, [('/DISC/test_v240.zip', 'fiio-firmware.fiio.net', None, 'identity')])

    def test_truncated_body(self):
        def short(h):
            h.send_body(PAYLOAD[:150000], length=len(PAYLOAD))
        self.server.routes['test_v240.zip'] = short
        with self.assertRaises(dl.SizeMismatch):
            self.fetch()
        self.assert_clean()

    def test_oversize_body_is_bounded(self):
        written = []
        real = open

        class Counting:
            def __init__(self, f):
                self.f = f

            def write(self, data):
                written.append(len(data))
                return self.f.write(data)

            def __enter__(self):
                return self

            def __exit__(self, *a):
                self.f.close()
        def chunked(h):
            h.send_response(200)
            h.send_header('Transfer-Encoding', 'chunked')
            h.end_headers()
            block = b'x' * 65536
            for _ in range(400):  # 26 MB offered, far above the pin
                h.wfile.write(b'10000\r\n' + block + b'\r\n')
            h.close_connection = True
        self.server.routes['test_v240.zip'] = chunked
        requested, returned = [], []
        real_read1 = http.client.HTTPResponse.read1
        inside = []

        def spy(resp, n=-1):
            if inside:  # internal re-entry from the chunked decoder
                return real_read1(resp, n)
            inside.append(1)
            try:
                requested.append(n)
                data = real_read1(resp, n)
                returned.append(len(data))
                return data
            finally:
                inside.pop()
        with mock.patch.object(dl, '_open', lambda p, m: Counting(real(p, m))), \
                mock.patch.object(http.client.HTTPResponse, 'read1', spy):
            with self.assertRaises(dl.SizeMismatch):
                self.fetch()
        self.assertLessEqual(sum(written), len(PAYLOAD))
        self.assertTrue(requested and max(requested) <= dl.CHUNK)
        self.assertEqual(sum(returned), len(PAYLOAD) + 1)
        for i, n in enumerate(requested):
            self.assertLessEqual(n, len(PAYLOAD) + 1 - sum(returned[:i]))
        self.assert_clean()

    def test_wrong_declared_length_rejected_before_body(self):
        self.server.routes['test_v240.zip'] = lambda h: h.send_body(PAYLOAD + b'x')
        with self.assertRaises(dl.SizeMismatch):
            self.fetch()
        self.assert_clean()

    def test_wrong_hash_rejected_and_part_removed(self):
        bad = bytearray(PAYLOAD)
        bad[1000] ^= 1
        self.server.routes['test_v240.zip'] = lambda h: h.send_body(bytes(bad))
        with self.assertRaises(dl.HashMismatch):
            self.fetch()
        self.assert_clean()

    def test_redirect_within_policy_followed(self):
        def hop(h):
            h.send_response(302)
            h.send_header('Location', OTHER)
            h.send_header('Content-Length', '0')
            h.end_headers()
        def rel(h):
            h.send_response(301)
            h.send_header('Location', '/DISC/final.zip')
            h.send_header('Content-Length', '0')
            h.end_headers()
        self.server.routes.update({'test_v240.zip': rel, 'final.zip': hop, 'other.zip': lambda h: h.send_body(PAYLOAD)})
        path = self.fetch()
        self.assertEqual(Path(path).read_bytes(), PAYLOAD)
        self.assertEqual([e[1] for e in self.server.log],
                         ['fiio-firmware.fiio.net', 'fiio-firmware.fiio.net',
                          'fiio-firmware.oss-us-west-1.aliyuncs.com'])

    def test_off_policy_redirects_refused_without_contact(self):
        for target in ('http://fiio-firmware.fiio.net/DISC/x.zip', 'https://evil.example/DISC/x.zip',
                       'https://fiio-firmware.fiio.net:8443/x.zip', 'https://u@fiio-firmware.fiio.net/x.zip',
                       'ftp://fiio-firmware.fiio.net/x.zip', 'https://fiio-firmware.fiio.net.evil.example/x.zip'):
            with self.subTest(target=target):
                self.server.log.clear()

                def hop(h, target=target):
                    h.send_response(302)
                    h.send_header('Location', target)
                    h.send_header('Content-Length', '0')
                    h.end_headers()
                self.server.routes['test_v240.zip'] = hop
                with self.assertRaises(dl.RedirectRefused):
                    self.fetch()
                self.assertEqual(len(self.server.log), 1)
                self.assert_clean()

    def test_redirect_without_target_refused(self):
        def hop(h):
            h.send_response(302)
            h.send_header('Content-Length', '0')
            h.end_headers()
        self.server.routes['test_v240.zip'] = hop
        with self.assertRaises(dl.RedirectRefused):
            self.fetch()

    def test_redirect_loop_is_bounded(self):
        def loop(h):
            h.send_response(307)
            h.send_header('Location', URL)
            h.send_header('Content-Length', '0')
            h.end_headers()
        self.server.routes['test_v240.zip'] = loop
        with self.assertRaises(dl.RedirectRefused):
            self.fetch()
        self.assertEqual(len(self.server.log), dl.MAX_HOPS + 1)
        self.assert_clean()

    def test_http_errors_are_typed(self):
        for status in (404, 500, 403, 429):
            with self.subTest(status=status):
                self.server.routes['test_v240.zip'] = lambda h, s=status: h.send_body(b'no', status=s)
                with self.assertRaises(dl.HttpStatusError) as caught:
                    self.fetch()
                self.assertEqual(caught.exception.status, status)
                self.assert_clean()

    def test_stalled_server_times_out(self):
        def stall(h):
            h.send_response(200)
            h.send_header('Content-Length', str(len(PAYLOAD)))
            h.end_headers()
            h.wfile.write(PAYLOAD[:1000])
            h.wfile.flush()
            self.release.wait(10)
        self.server.routes['test_v240.zip'] = stall
        began = time.monotonic()
        with self.assertRaises(dl.Timeout):
            self.fetch(timeout=0.4)
        self.assertLess(time.monotonic() - began, 5)
        self.assert_clean()

    def test_overall_deadline_stops_a_slow_trickle(self):
        def trickle(h):
            h.send_response(200)
            h.send_header('Content-Length', str(len(PAYLOAD)))
            h.end_headers()
            for i in range(0, 2000, 10):
                h.wfile.write(PAYLOAD[i:i + 10])
                h.wfile.flush()
                if self.release.wait(0.05):
                    return
        self.server.routes['test_v240.zip'] = trickle
        with self.assertRaises(dl.DeadlineExceeded):
            self.fetch(deadline=0.4)
        self.assert_clean()

    def test_cancel_mid_stream_leaves_nothing(self):
        seen = []
        with self.assertRaises(dl.Cancelled):
            self.fetch(progress=lambda d, t: seen.append(d),
                       cancel=lambda: bool(seen) and seen[-1] >= 100000)
        self.assertTrue(100000 <= seen[-1] < len(PAYLOAD))
        self.assert_clean()
        with self.assertRaises(dl.Cancelled):
            self.fetch(cancel=lambda: True)
        self.assertEqual(len(self.server.log), 1)
        self.assert_clean()

    def test_cancel_at_eof_does_not_publish(self):
        flag = []
        real_read1 = http.client.HTTPResponse.read1

        def read1(resp, n=-1):
            data = real_read1(resp, n)
            if not data:
                flag.append(1)  # cancel turns true as the final read returns EOF
            return data
        with mock.patch.object(http.client.HTTPResponse, 'read1', read1), \
                mock.patch.object(fw.os, 'replace') as replace:
            with self.assertRaises(dl.Cancelled):
                self.fetch(cancel=lambda: bool(flag))
        self.assertEqual(flag, [1])
        replace.assert_not_called()
        self.assert_clean()

    def test_cancel_or_deadline_during_verification_does_not_publish(self):
        real_verify = fw._verify_stream
        for when in ('before', 'after'):
            for kind in ('cancel', 'deadline'):
                with self.subTest(when=when, kind=kind):
                    now = [0.0]
                    flag = []

                    def trip():
                        flag.append(1)
                        now[0] = 1e6

                    def verify(stream, pin, output=None, check=None):
                        if when == 'before':
                            trip()
                        real_verify(stream, pin, output, check)
                        if when == 'after':
                            trip()
                    kw = dict(clock=lambda: now[0])
                    if kind == 'cancel':
                        kw = dict(cancel=lambda: bool(flag))
                    expected = dl.Cancelled if kind == 'cancel' else dl.DeadlineExceeded
                    with mock.patch.object(fw, '_verify_stream', verify), \
                            mock.patch.object(fw.os, 'replace') as replace:
                        with self.assertRaises(expected):
                            self.fetch(**kw)
                    self.assertEqual(flag, [1])
                    replace.assert_not_called()
                    self.assert_clean()

    def test_disk_full_is_typed(self):
        real = open
        class Full:
            def __init__(self, f):
                self.f, self.n = f, 0

            def write(self, data):
                self.n += len(data)
                if self.n > 100000:
                    raise OSError(errno.ENOSPC, 'No space left on device')
                return self.f.write(data)

            def __enter__(self):
                return self

            def __exit__(self, *a):
                self.f.close()
        with mock.patch.object(dl, '_open', lambda p, m: Full(real(p, m))):
            with self.assertRaises(dl.DiskFull):
                self.fetch()
        self.assert_clean()

    def test_disk_full_on_close_is_typed(self):
        real = open
        for number in (errno.ENOSPC, getattr(errno, 'EDQUOT', errno.ENOSPC)):
            with self.subTest(errno=number):
                class FullOnClose:
                    def __init__(self, f):
                        self.f = f

                    def write(self, data):
                        return self.f.write(data)

                    def __enter__(self):
                        return self

                    def __exit__(self, *a):
                        self.f.close()
                        raise OSError(number, 'flush failed on close')
                with mock.patch.object(dl, '_open', lambda p, m: FullOnClose(real(p, m))):
                    with self.assertRaises(dl.DiskFull) as caught:
                        self.fetch()
                self.assertEqual(caught.exception.code, 'E169')
                self.assert_clean()

    def test_malformed_redirect_authority_is_refused_for_every_status(self):
        closed = []
        real_close = urllib.error.HTTPError.close

        def close(err):
            closed.append(err.code)
            return real_close(err)
        for status in (301, 302, 303, 307, 308):
            for target in ('https://[fiio-firmware.fiio.net/x.zip', 'https://fiio-firmware.fiio.net]/x.zip',
                           '//[fiio-firmware.fiio.net/x.zip'):
                with self.subTest(status=status, target=target):
                    self.server.log.clear()
                    del closed[:]

                    def hop(h, status=status, target=target):
                        h.send_response(status)
                        h.send_header('Location', target)
                        h.send_header('Content-Length', '0')
                        h.end_headers()
                    self.server.routes['test_v240.zip'] = hop
                    with mock.patch.object(urllib.error.HTTPError, 'close', close):
                        with self.assertRaises(dl.RedirectRefused) as caught:
                            self.fetch()
                    self.assertEqual(caught.exception.code, 'E166')
                    self.assertEqual(closed, [status])
                    self.assertEqual(len(self.server.log), 1)
                    self.assert_clean()

    def test_restart_after_interruption_starts_from_zero(self):
        state_ = {'n': 0}
        def flaky(h):
            state_['n'] += 1
            if state_['n'] == 1:
                h.send_body(PAYLOAD[:200000], length=len(PAYLOAD))
            else:
                h.send_body(PAYLOAD)
        self.server.routes['test_v240.zip'] = flaky
        with self.assertRaises(dl.SizeMismatch):
            self.fetch()
        self.assert_clean()
        seen = []
        path = self.fetch(progress=lambda d, t: seen.append(d))
        self.assertEqual(seen[0], 0)
        self.assertEqual(Path(path).read_bytes(), PAYLOAD)
        self.assertEqual([e[2] for e in self.server.log], [None, None])

    def test_untrusted_certificate_is_a_tls_error(self):
        with self.assertRaises(dl.TlsError):
            dl.download('240', self.lock, context=net_tls.build_context(environ={}))
        self.assert_clean()

    def test_connection_failure_is_a_network_error(self):
        with mock.patch.object(socket, 'create_connection', side_effect=ConnectionRefusedError(111, 'refused')):
            with self.assertRaises(dl.NetworkError):
                self.fetch()
        with mock.patch.object(socket, 'create_connection', side_effect=socket.gaierror(-2, 'no name')):
            with self.assertRaises(dl.NetworkError):
                self.fetch()
        self.assert_clean()


if __name__ == '__main__':
    unittest.main()
