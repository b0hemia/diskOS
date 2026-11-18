# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""S97 retained-bundle verification under the pinned firmware's real BusyBox ash.

Private paths replace production paths; the complete shipped supervisor and worker run.
Signed ELF fixtures use host OpenSSL. Firmware applets execute through qemu with the
same hybrid loader-prefix arrangement as test_boot_select (not a chroot).
"""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from test_boot_select import FW_ROOT, QEMU, _have_fw_ash

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = ROOT / 'payload'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class BootEnv:
    def __init__(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ota-boot-')
        self.root = Path(self.temp.name)
        self.data = self.root / 'data'
        self.data.mkdir()
        self.upd = self.data / 'updates'
        self.upd.mkdir()
        self.ro = self.root / 'ota'
        self.ro.mkdir()
        self.tmp = self.root / 'tmp'
        self.tmp.mkdir()
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.witness = self.root / 'fault-ran'
        self.fault = self.root / 'fault'
        self.ash = self.root / 'ash'
        self.ash.write_text('#!/bin/sh\nexec ' + ' '.join(map(shlex.quote,
                            [QEMU, '-L', str(FW_ROOT), str(FW_ROOT / 'bin/busybox'), 'sh'])) + ' "$@"\n')
        self.ash.chmod(0o755)
        for applet in ('mkdir', 'chmod', 'cp', 'rm', 'mv', 'cat'):
            command = ' '.join(map(shlex.quote, [QEMU, '-L', str(FW_ROOT),
                                                str(FW_ROOT / 'bin/busybox'), applet]))
            inject = ''
            if applet in ('mkdir', 'chmod', 'cp'):
                # Affect only verifier staging, never installation or the test harness.
                arg = '$1' if applet != 'chmod' else '$2'
                pattern = (f'{self.tmp}/.diskos_ota.*' if applet != 'cp'
                           else f'{self.upd}/*/keyauthz')
                inject = (f'if [ "$(cat {self.fault} 2>/dev/null)" = {applet} ]; then\n'
                          f' case "{arg}" in {pattern}) echo {applet} >> {self.witness}; exit 1;; esac\nfi\n')
            p = self.bin / applet
            p.write_text('#!/bin/sh\n' + inject + f'exec {command} "$@"\n')
            p.chmod(0o755)
        (self.bin / 'sync').write_text('#!/bin/sh\nexit 0\n')
        (self.bin / 'sync').chmod(0o755)
        self.helper = self.root / 'flashstate'
        self.helper.write_text('#!/bin/sh\nexit 0\n')
        self.helper.chmod(0o755)
        self.rec = self.root / 'boot_select'
        self.rec.write_text('diskos\n')
        self.env = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}',
                        DISKOS_BOOT_SELECT=str(self.rec), DISKOS_SELECTED=str(PAYLOAD / 'diskos-selected'))
        self.verifier = self.ro / 'diskos-verify.sh'
        self.restore_verifier()
        self.key = self.root / 'key.pem'
        self.openssl('ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', self.key)
        self.openssl('pkey', '-in', self.key, '-pubout', '-out', self.ro / 'root.pub.pem')
        (self.ro / 'epochs').write_text('MIN_KEY_EPOCH=1\nMIN_UI_EPOCH=1\n')
        self.embedded = self.root / 'embedded'
        self.embedded.write_bytes(self.elf(b'flashed'))
        self.man = self.root / 'manifest'
        self.man.write_text(f'SHA256={digest(self.embedded)}\nSIZE={self.embedded.stat().st_size}\n')
        text = (PAYLOAD / 'S97diskos_install').read_text()
        for old, new in (('/opt/diskos/bin/diskos-flashstate', self.helper),
                         ('/opt/diskos/mq_ui', self.embedded), ('/usr/data', self.data),
                         ('/etc/diskos_manifest', self.man), ('/etc/diskos-ota', self.ro),
                         ('/tmp/', str(self.tmp) + '/'), ('/bin/sh', self.ash)):
            text = text.replace(old, str(new))
        self.script = self.root / 'S97'
        self.script.write_text(text)
        self.script.chmod(0o755)

    @staticmethod
    def elf(tag):
        data = bytearray(128)
        data[:6] = b'\x7fELF\x01\x01'
        data[18:20] = b'\x08\x00'
        return bytes(data) + tag

    def openssl(self, *args):
        subprocess.run(['openssl', *map(str, args)], check=True, capture_output=True, timeout=10)

    def restore_verifier(self):
        self.verifier.write_bytes((PAYLOAD / 'diskos-verify.sh').read_bytes())

    def bundle(self, name, epoch=2):
        d = self.upd / name
        d.mkdir()
        (d / 'mq_ui').write_bytes(self.elf(name.encode()))
        shutil.copyfile(self.ro / 'root.pub.pem', d / 'leafpub.pem')
        (d / 'keyauthz').write_text('diskos-keyauthz-v1\nkeyid=test\nkeyepoch=1\nrole=tier-a\n'
                                   f'pubkey_sha256={digest(d / "leafpub.pem")}\n')
        (d / 'update.manifest').write_text('diskos-update-v1\ncomponent=mq_ui\n'
                                          f'epoch={epoch}\nversion={name}\nsize={(d / "mq_ui").stat().st_size}\n'
                                          f'sha256={digest(d / "mq_ui")}\nchannel=stable\n')
        for name in ('keyauthz', 'update.manifest'):
            self.openssl('dgst', '-sha256', '-sign', self.key, '-out', d / (name + '.sig'), d / name)
        if d.name == 'active':
            (self.upd / 'trial').write_text(f'boots=0\nsha256={digest(d / "mq_ui")}\n')
        if d.name == 'pending':
            (d / 'ready').touch()
        return d

    def run(self):
        for name in ('.diskos_ready', '.diskos_run'):
            (self.tmp / name).unlink(missing_ok=True)
        r = subprocess.run([str(self.script), 'start'], env=self.env, capture_output=True, text=True, timeout=30)
        if r.returncode:
            raise AssertionError((r.returncode, r.stdout, r.stderr, self.log()))
        if not (self.tmp / '.diskos_ready').is_file():
            raise AssertionError(('S98 readiness missing', r.stderr, self.log()))
        return (self.data / 'mq_ui').read_bytes()

    def log(self):
        p = self.data / 'diskos_install.log'
        return p.read_text() if p.exists() else ''

    def close(self):
        self.temp.cleanup()


@unittest.skipUnless(_have_fw_ash() and shutil.which('openssl'), 'needs pinned firmware BusyBox, qemu and OpenSSL')
class RetainedBundleTests(unittest.TestCase):
    def make(self):
        e = BootEnv()
        self.addCleanup(e.close)
        return e

    def retry(self, fault, kind, previous=False):
        e = self.make()
        d = e.bundle(kind)
        want = (d / 'mq_ui').read_bytes()
        before = {p.name: p.read_bytes() for p in d.iterdir()}
        trial = (e.upd / 'trial').read_bytes() if kind == 'active' else None
        if previous:
            prev = e.bundle('good.prev', epoch=1)
            fallback = (prev / 'mq_ui').read_bytes()
        else:
            fallback = e.embedded.read_bytes()
        e.fault.write_text(fault)
        if fault == 'exec':
            e.verifier.write_text(f'echo exec >> {e.witness}\nexec /diskos-no-such-verifier\n')
        self.assertEqual(e.run(), fallback, e.log())
        self.assertTrue(e.witness.is_file(), 'fault path never ran')
        self.assertTrue(d.is_dir(), e.log())
        self.assertEqual({p.name: p.read_bytes() for p in d.iterdir()}, before, e.log())
        if trial is not None:
            self.assertEqual((e.upd / 'trial').read_bytes(), trial, e.log())
        self.assertIn('retry', e.log())
        self.assertNotIn('ROLLBACK', e.log())
        e.fault.unlink()
        e.restore_verifier()
        self.assertEqual(e.run(), want, e.log())
        self.assertIn('ota: selected', e.log())

    def test_tmp_creation_failure_keeps_good_and_trial(self):
        for kind in ('good', 'active'):
            with self.subTest(kind=kind): self.retry('mkdir', kind)

    def test_copy_failure_keeps_good_and_trial(self):
        for kind in ('good', 'active'):
            with self.subTest(kind=kind): self.retry('cp', kind)

    def test_verifier_exec_failure_keeps_good_and_trial(self):
        for kind in ('good', 'active'):
            with self.subTest(kind=kind): self.retry('exec', kind)

    def test_tmp_chmod_failure_keeps_good_and_trial(self):
        for kind in ('good', 'active'):
            with self.subTest(kind=kind): self.retry('chmod', kind)

    def test_pending_retries_instead_of_removal(self):
        for fault in ('mkdir', 'cp', 'exec'):
            with self.subTest(fault=fault): self.retry(fault, 'pending')

    def test_other_verified_bundle_is_fallback_without_replacing_good(self):
        # Fail only good's copy, allowing good.prev to verify in the same boot.
        e = self.make()
        good = e.bundle('good')
        prev = e.bundle('good.prev', epoch=1)
        cp = e.bin / 'cp'
        cp.write_text(cp.read_text().replace(f'{e.upd}/*/keyauthz', f'{good}/keyauthz'))
        e.fault.write_text('cp')
        self.assertEqual(e.run(), (prev / 'mq_ui').read_bytes(), e.log())
        self.assertTrue(good.is_dir())
        self.assertTrue(prev.is_dir())
        e.fault.unlink()
        self.assertEqual(e.run(), (good / 'mq_ui').read_bytes(), e.log())

    def test_incomplete_crashloop_verification_preserves_trial_proof(self):
        e = self.make()
        active = e.bundle('active')
        (e.upd / 'trial.crashed').touch()
        (e.upd / 'revert').write_text(f'SHA256={digest(active / "mq_ui")}\n')
        trial = (e.upd / 'trial').read_bytes()
        e.fault.write_text('cp')
        self.assertEqual(e.run(), e.embedded.read_bytes(), e.log())
        self.assertTrue(active.is_dir())
        self.assertTrue((e.upd / 'trial.crashed').is_file())
        self.assertTrue((e.upd / 'revert').is_file())
        self.assertEqual((e.upd / 'trial').read_bytes(), trial)
        e.fault.unlink()
        e.run()
        self.assertFalse(active.exists())
        self.assertIn('ROLLBACK (crashloop)', e.log())

    def test_invalid_bundles_are_still_removed_or_rolled_back(self):
        for kind in ('good', 'active', 'pending'):
            with self.subTest(kind=kind):
                e = self.make()
                d = e.bundle(kind)
                (d / 'mq_ui').write_bytes(e.elf(b'bad-content'))
                self.assertEqual(e.run(), e.embedded.read_bytes(), e.log())
                self.assertFalse(d.exists(), e.log())
                if kind == 'active':
                    self.assertFalse((e.upd / 'trial').exists())
                    self.assertIn('ROLLBACK (verify)', e.log())

    def test_previous_bundle_incomplete_after_invalid_good_is_kept(self):
        e = self.make()
        good = e.bundle('good')
        prev = e.bundle('good.prev')
        (good / 'mq_ui').write_bytes(b'bad')
        # First verification completes and rejects; fail the restored previous copy.
        cp = e.bin / 'cp'
        cp.write_text('#!/bin/sh\n'
                      f'if [ "$1" = {good}/keyauthz ] && [ ! -e {e.root}/seen ]; then\n'
                      f' touch {e.root}/seen\nelif [ "$1" = {good}/keyauthz ]; then\n'
                      ' exit 1\nfi\nexec /bin/cp "$@"\n')
        self.assertEqual(e.run(), e.embedded.read_bytes(), e.log())
        self.assertTrue(good.is_dir(), e.log())
        self.assertFalse(prev.exists())
        cp.write_text('#!/bin/sh\nexec /bin/cp "$@"\n')
        self.assertEqual(e.run(), (good / 'mq_ui').read_bytes(), e.log())

    def test_verifier_internal_exec_and_read_errors_are_retryable(self):
        for fault in ('openssl', 'hash', 'read'):
            with self.subTest(fault=fault):
                e = self.make()
                good = e.bundle('good')
                before = (good / 'mq_ui').read_bytes()
                text = e.verifier.read_text()
                shim = e.root / 'failed-tool'
                shim.write_text(f'#!/bin/sh\necho {fault} >> {e.witness}\nexit 127\n')
                shim.chmod(0o755)
                if fault == 'openssl':
                    text = text.replace('OPENSSL=/usr/bin/openssl', f'OPENSSL={shim}')
                elif fault == 'hash':
                    text = text.replace('sha256sum "$1"', f'{shim} "$1"')
                else:
                    text = text.replace('wc -c < "$1"', f'{shim} < "$1"')
                e.verifier.write_text(text)
                self.assertEqual(e.run(), e.embedded.read_bytes(), e.log())
                self.assertTrue(e.witness.is_file())
                self.assertTrue(good.is_dir(), e.log())
                self.assertEqual((good / 'mq_ui').read_bytes(), before)
                self.assertIn('retry', e.log())
                e.restore_verifier()
                self.assertEqual(e.run(), before, e.log())

    def test_invalid_signature_and_nul_text_still_reject(self):
        for fault in ('signature', 'nul'):
            with self.subTest(fault=fault):
                e = self.make()
                good = e.bundle('good')
                man = good / 'update.manifest'
                if fault == 'signature':
                    man.write_bytes(man.read_bytes().replace(b'channel=stable', b'channel=broken'))
                else:
                    man.write_bytes(man.read_bytes() + b'\x00')
                self.assertEqual(e.run(), e.embedded.read_bytes(), e.log())
                self.assertFalse(good.exists(), e.log())

    def test_missing_verifier_preserves_bundles_and_retries(self):
        e = self.make()
        good = e.bundle('good')
        e.verifier.unlink()
        self.assertEqual(e.run(), e.embedded.read_bytes(), e.log())
        self.assertTrue(good.is_dir())
        self.assertIn('retry', e.log())
        e.restore_verifier()
        self.assertEqual(e.run(), (good / 'mq_ui').read_bytes(), e.log())

    def test_staging_and_rename_preserve_hardlinked_bundle(self):
        e = self.make()
        good = e.bundle('good')
        original = (good / 'mq_ui').read_bytes()
        self.assertEqual(e.run(), original, e.log())
        live = e.data / 'mq_ui'
        self.assertTrue(os.path.samefile(live, good / 'mq_ui'))
        staged = e.data / 'mq_ui.new'
        staged.write_bytes(e.elf(b'deployment'))
        os.replace(staged, live)
        self.assertEqual((good / 'mq_ui').read_bytes(), original)
        self.assertFalse(os.path.samefile(live, good / 'mq_ui'))
        self.assertEqual(e.run(), original, e.log())

    def test_active_incomplete_uses_flashed_without_counting_trial(self):
        e = self.make()
        active = e.bundle('active')
        good = e.bundle('good', epoch=1)
        trial = (e.upd / 'trial').read_bytes()
        cp = e.bin / 'cp'
        cp.write_text(cp.read_text().replace(f'{e.upd}/*/keyauthz', f'{active}/keyauthz'))
        e.fault.write_text('cp')
        self.assertEqual(e.run(), e.embedded.read_bytes(), e.log())
        self.assertFalse((e.tmp / '.diskos_run').exists(), 'flashed path must bypass launcher proof clearing')
        self.assertEqual((e.upd / 'trial').read_bytes(), trial)
        self.assertTrue(active.is_dir())
        e.fault.unlink()
        self.assertEqual(e.run(), (active / 'mq_ui').read_bytes(), e.log())

    def test_healthy_promotion_keeps_previous_until_later_verification(self):
        e = self.make()
        active = e.bundle('active', epoch=3)
        good = e.bundle('good', epoch=2)
        previous_bytes = (good / 'mq_ui').read_bytes()
        want = (active / 'mq_ui').read_bytes()
        proof = f'SHA256={digest(active / "mq_ui")}\n'
        (e.upd / 'healthy').write_text(proof)
        (e.upd / 'trial.launched').write_text(proof)
        e.fault.write_text('cp')
        self.assertEqual(e.run(), e.embedded.read_bytes(), e.log())
        self.assertEqual((e.upd / 'trial.launched').read_text(), proof)
        self.assertEqual((e.upd / 'healthy').read_text(), proof)
        e.fault.unlink()
        self.assertEqual(e.run(), want, e.log())
        self.assertFalse(active.exists())
        self.assertEqual((e.upd / 'good.prev/mq_ui').read_bytes(), previous_bytes)
        # Both copies survive a later boot whose verification cannot complete.
        e.fault.write_text('cp')
        self.assertEqual(e.run(), e.embedded.read_bytes(), e.log())
        self.assertTrue(good.is_dir())
        self.assertEqual((e.upd / 'good.prev/mq_ui').read_bytes(), previous_bytes)
        e.fault.unlink()
        self.assertEqual(e.run(), want, e.log())
        self.assertFalse((e.upd / 'good.prev').exists())

    def test_unclassified_verifier_exit_is_retryable(self):
        for status in (1, 124):
            with self.subTest(status=status):
                e = self.make()
                good = e.bundle('good')
                e.verifier.write_text(f'echo unclassified >> {e.witness}\nexit {status}\n')
                self.assertEqual(e.run(), e.embedded.read_bytes(), e.log())
                self.assertTrue(e.witness.is_file())
                self.assertTrue(good.is_dir(), e.log())
                self.assertIn('retry', e.log())
                e.restore_verifier()
                self.assertEqual(e.run(), (good / 'mq_ui').read_bytes(), e.log())
