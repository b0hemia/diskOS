# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the real transport against native mocks; never open USB devices."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform.startswith('linux'), 'Linux native harness')
class TransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = str(Path(cls.tmp.name) / 'transport')
        subprocess.run(['gcc', '-O2', '-std=c99', '-ffunction-sections', '-fdata-sections',
                        '-I/usr/include/libusb-1.0', str(Path(__file__).with_name('usbboot_transport_mock.c')),
                        '-Wl,--gc-sections', '-o', cls.exe], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case, *args):
        return subprocess.run([self.exe, str(case), *map(str, args)],
                              capture_output=True, text=True, timeout=3,
                              env={**os.environ, 'USBBOOT_RETRIES': '5'})

    def test_bulk_progress_and_failures_both_directions(self):
        for direction in (0, 1):
            for case in (1, 2, 3, 4, 5, 6, 17, 18):
                with self.subTest(case=case, direction=direction):
                    p = self.run_case(case, direction)
                    self.assertEqual(p.returncode, 0 if case == 1 else 1, p.stderr)
                    self.assertEqual(p.stderr.count('bulk-call='), 4 if case == 1 else 2 if case == 6 else 1)
                    if case == 6: self.assertIn('deadline exceeded', p.stderr)

    def test_rediscovery_and_observation(self):
        for case, message in ((7, 'reconnect-ms=200'), (8, 'changed physical location'),
                              (9, 'Ambiguous'), (10, 'deadline exceeded'),
                              (14, 'descriptor failed'), (15, 'port path'), (16, 'changed physical location')):
            with self.subTest(case=case):
                p = self.run_case(case)
                self.assertEqual(p.returncode, 0 if case == 7 else 1, p.stderr)
                self.assertIn(message, p.stderr)
        p = self.run_case(7)
        self.assertIn('target=absent old-handle=-4', p.stderr)
        self.assertIn('target=changed-location', self.run_case(8).stderr)
        self.assertIn('target=ambiguous', self.run_case(9).stderr)

    def test_start_never_replayed(self):
        for case in (11, 12, 21):
            p = self.run_case(case)
            self.assertEqual(p.returncode, 0 if case == 12 else 1)
            self.assertEqual(p.stderr.count('control='), 1)
        self.assertIn('old-handle=1', self.run_case(12).stderr)

    def test_observation_errors_allow_wait_and_readback(self):
        for case, error in ((22, 'enumeration failed'), (23, 'descriptor failed'),
                            (24, 'port path')):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                readback = Path(tmp) / 'readback'
                p = self.run_case(case, '--observe-start1', '--start1', '0xa0c00030',
                                  '-v', '--wait', 2, '--addr', '0xa0a00000',
                                  '--length', 10, '--upload', readback)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertIn(error, p.stderr)
                self.assertIn('wait=2', p.stderr)
                self.assertEqual(readback.read_bytes(), b'X' * 10)
                self.assertEqual(p.stderr.count('control=4 '), 1)

    def test_observation_opt_in_and_writer_exclusion(self):
        for flags, expected in (((), 0), (('--observe-stage1',), 3),
                                (('--observe-start1',), 6)):
            with self.subTest(flags=flags), tempfile.TemporaryDirectory() as tmp:
                pattern, readback = Path(tmp) / 'pattern', Path(tmp) / 'readback'
                pattern.write_bytes(b'X' * 10)
                p = self.run_case(13, '-v', *flags, '--stage1', pattern, '--wait', 2,
                                  '--addr', '0xa0c00000', '--download', pattern,
                                  '--start1', '0xa0c00030', '--wait', 1080,
                                  '--addr', '0xa0a00000', '--length', 10, '--upload', readback)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertEqual(readback.read_bytes(), pattern.read_bytes())
                self.assertEqual(p.stderr.count('get-configuration'), expected)
                self.assertEqual(p.stderr.count('scan='), expected + 1)
                self.assertEqual(p.stderr.count('control=4 '), 2)

    def test_linux_existing_option_sequence_and_no_partial_replay(self):
        with tempfile.TemporaryDirectory() as tmp:
            pattern, readback = Path(tmp) / 'pattern', Path(tmp) / 'readback'
            pattern.write_bytes(b'X' * 10)
            p = self.run_case(13, '-v', '--cpu', 'x2000', '--stage1', pattern, '--wait', 0,
                              '--renumerate', '--addr', '0xa1000000', '--download', pattern,
                              '--length', 10, '--upload', readback, '--flush-caches',
                              '--med', 6, '--ops', '0x60000', '--handshake', 0, '--strcmd')
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(readback.read_bytes(), pattern.read_bytes())
            self.assertEqual(p.stderr.count('control=4 '), 1)
            p = self.run_case(19, '--stage1', pattern, '--start1', '0xa0c00030')
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertEqual(p.stderr.count('bulk-call='), 2)
            self.assertNotIn('control=4 ', p.stderr)
        for args, text in ((('--cpu', 'bad'), "not known"), (('--upload', 'unused'), 'before --upload'),
                           (('--addr', 'bad'), 'Invalid argument'), (('--help',), 'Usage: usbboot')):
            p = self.run_case(13, *args)
            self.assertEqual(p.returncode, 1)
            self.assertIn(text, p.stdout + p.stderr)
