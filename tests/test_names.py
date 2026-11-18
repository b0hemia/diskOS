# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Boot-only network naming: real native helpers and real init/launch fragments."""
import hashlib
import os
from pathlib import Path
import shutil
import shlex
import signal
import subprocess
import tempfile
import time
import unittest

from diskos_installer import bundle, imagebuild as ib
from diskos_installer.buildfs import TreeFS

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = os.environ.get('DISKOS_WIN_SCRATCH')
FIXTURES = Path(os.environ.get('DISKOS_NAMES_FIXTURES', str(ROOT / '.author-tmp/firmware-fixtures')))
FORMS = [
    ['-i', 'wlan0', '-x', 'hostname:SNOWSKY_DISC'],
    ['-i', 'wlan0', '-x', 'hostname:SNOWSKY', 'DISC'],
    ['-i', 'wlan0', '-x', 'hostname:FIIO_'],
]


@unittest.skipUnless(os.name == 'posix' and shutil.which('cc'), 'POSIX native compiler required')
class NamesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='names-', dir=SCRATCH)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.active = self.root / 'state' / 'active'
        self.saved = self.root / 'saved'
        self.hostname = self.root / 'hostname'
        self.bb = self.root / 'busybox'
        self.boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        src = self.root / 'busybox.c'
        src.write_text('''#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    printf("%ld%c", (long)getpid(), 0);
    for(int i=0;i<argc;i++) printf("%s%c",argv[i],0);
    fflush(stdout);
    if(getenv("WAIT_SIG")) for(;;) pause();
    return 0;
}
''')
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(self.bb)], check=True)
        self.compile()

    def compile(self, delay=0, deadline=750):
        definitions = [f'-DNAMES_DIR="{self.active.parent}"', f'-DNAMES_SAVED="{self.saved}"',
                       f'-DNAMES_BUSYBOX="{self.bb}"', f'-DNAMES_UID={os.getuid()}',
                       f'-DNAMES_TEST_HOSTNAME="{self.hostname}"',
                       f'-DNAMES_DEADLINE_MS={deadline}', f'-DNAMES_TEST_DELAY_MS={delay}']
        for part in ('names', 'udhcpc'):
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', *definitions,
                            str(ROOT / 'src/names' / f'diskos_{part}.c'),
                            '-o', str(self.root / part)], check=True)

    def prepare(self):
        return subprocess.run([str(self.root / 'names')], timeout=3).returncode

    def save(self, name='MiXeD-Disc'):
        self.saved.write_bytes(b'v1\nname=' + name.encode() + b'\n')

    def invoke(self, args, token='v1'):
        env = dict(os.environ)
        env.pop('DISKOS_NETWORK_NAMES', None)
        if token is not None:
            env['DISKOS_NETWORK_NAMES'] = token
        r = subprocess.run([str(self.root / 'udhcpc'), *args], env=env,
                           stdout=subprocess.PIPE, timeout=3, check=True)
        fields = r.stdout.split(b'\0')
        return [os.fsdecode(x) for x in fields[3:-1]]

    def test_stock_argv_all_three_forms(self):
        for state in ('missing', 'empty'):
            if state == 'empty':
                self.save(''); self.assertEqual(self.prepare(), 0)
            for args in FORMS:
                self.assertEqual(self.invoke(args), args)

    def test_override_option12_and_other_arguments(self):
        self.save(); self.assertEqual(self.prepare(), 0)
        extras = ['-s', '/a script', '-p', '/a pid', '-V', 'vendor', '-c', 'client',
                  '-F', 'fqdn', '-r', '1.2.3.4', '-t', '3', '-T', '5', '-O', 'router',
                  '-x', 'vendor:untouched', '-b']
        for form in FORMS:
            args = form[:-1] + extras + form[-1:] if form[-1] == 'DISC' else form + extras
            # DISC is only a supported accidental positional tail immediately after SNOWSKY.
            if form[-1] == 'DISC':
                args = extras + form
            expected = list(args)
            expected[expected.index(form[3])] = 'hostname:MiXeD-Disc'
            self.assertEqual(self.invoke(args), expected)
            self.assertEqual(sum(x.startswith('hostname:') for x in self.invoke(args)), 1)

    def test_unknown_and_malformed_fallback(self):
        self.save(); self.assertEqual(self.prepare(), 0)
        cases = [['-i'], ['-i', 'wlan0', '-x'], ['-x', 'hostname:old'],
                 FORMS[0] + ['-x', 'hostname:second'], FORMS[0] + ['--mystery'],
                 FORMS[0] + ['-h', 'second'], FORMS[0] + ['-x', '0x0c:other'],
                 FORMS[0] + ['DISC'], ['-iwlan0', '-x', 'hostname:old'],
                 ['-i', 'wlan0', '-x', 'hostname:'], FORMS[0] + ['-i', 'eth0']]
        for args in cases:
            self.assertEqual(self.invoke(args), args)

    def test_first_exec_failure_retries_original_argv(self):
        self.save(); self.assertEqual(self.prepare(), 0)
        probe = self.root / 'exec-probe.c'
        probe.write_text('''#include <errno.h>
#include <stdio.h>
int __real_execv(const char *, char *const []);
int __wrap_execv(const char *path, char *const argv[]) {
    static int attempt;
    char file[32];
    snprintf(file, sizeof file, "attempt-%d", ++attempt);
    FILE *f = fopen(file, "wb");
    if (!f) return -1;
    fprintf(f, "%s%c", path, 0);
    for (int i = 0; argv[i]; ++i) fprintf(f, "%s%c", argv[i], 0);
    if (fclose(f)) return -1;
    if (attempt == 1) { errno = E2BIG; return -1; }
    return __real_execv(path, argv);
}
''')
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror',
                        f'-DNAMES_DIR="{self.active.parent}"',
                        f'-DNAMES_BUSYBOX="{self.bb}"', f'-DNAMES_UID={os.getuid()}',
                        str(ROOT / 'src/names/diskos_udhcpc.c'), str(probe),
                        '-Wl,--wrap=execv', '-o', str(self.root / 'udhcpc')], check=True)
        args = FORMS[1]
        result = subprocess.run([str(self.root / 'udhcpc'), *args], cwd=self.root,
                                env=dict(os.environ, DISKOS_NETWORK_NAMES='v1'),
                                capture_output=True, timeout=3)
        first = (self.root / 'attempt-1').read_bytes().split(b'\0')[:-1]
        self.assertEqual(first, [os.fsencode(self.bb), os.fsencode(self.bb), b'udhcpc',
                                 b'-i', b'wlan0', b'-x', b'hostname:MiXeD-Disc', b'DISC'])
        print('first exec reached rewritten argv including DISC; injected E2BIG', flush=True)
        self.assertEqual(result.returncode, 0, 'failed first exec must retry original argv')
        second = (self.root / 'attempt-2').read_bytes().split(b'\0')[:-1]
        self.assertEqual(second, [os.fsencode(self.bb), b'udhcpc', *map(os.fsencode, args)])
        self.assertEqual(result.stdout.split(b'\0')[2:-1], [*map(os.fsencode, args)])

    def test_saved_invalid_files_and_stock_hostname(self):
        bad = [b'', b'v2\nname=foo\n', b'v1\nname=A\x00B\n', b'v1\nname=foo\nname=bar\n',
               b'v1\nname=foo\nunknown=x\n', b'v1\nname=foo', b'v1\nname=-foo\n',
               b'v1\nname=foo-\n', b'v1\nname=foo bar\n', b'v1\nname=' + b'a'*33 + b'\n',
               b'x' * 8192, b'v1\nname=\xff\n']
        for value in bad + [None, 'fifo', 'symlink', 'directory', 'hardlink']:
            if self.active.parent.exists(): shutil.rmtree(self.active.parent)
            if self.saved.is_dir(): self.saved.rmdir()
            elif self.saved.exists() or self.saved.is_symlink(): self.saved.unlink()
            if isinstance(value, bytes): self.saved.write_bytes(value)
            elif value == 'fifo': os.mkfifo(self.saved)
            elif value == 'directory': self.saved.mkdir()
            elif value in ('symlink', 'hardlink'):
                target = self.root / ('target-' + value)
                target.write_bytes(b'v1\nname=ShouldReject\n')
                if value == 'symlink': self.saved.symlink_to(target)
                else: os.link(target, self.saved)
            self.assertEqual(self.prepare(), 0)
            self.assertEqual(self.active.read_text(), f'v1\nboot={self.boot}\nname=\n')
            self.assertFalse(self.hostname.exists())
            self.assertEqual(self.invoke(FORMS[0]), FORMS[0])

    def test_validator_case_length_and_inner_hyphens(self):
        for name in ('A', '0', 'MiXeD-Disc', 'A--B', 'a'*32):
            if self.active.parent.exists(): shutil.rmtree(self.active.parent)
            self.save(name); self.assertEqual(self.prepare(), 0)
            self.assertEqual(self.hostname.read_text(), name)
            self.assertEqual(self.invoke(FORMS[0]), ['-i', 'wlan0', '-x', 'hostname:' + name])

    def test_snapshot_immutable_until_reboot(self):
        self.save(); self.assertEqual(self.prepare(), 0)
        before = self.active.read_bytes(); self.save('Changed')
        self.assertNotEqual(self.prepare(), 0)
        self.assertEqual(self.active.read_bytes(), before)
        self.assertEqual(self.hostname.read_text(), 'MiXeD-Disc')
        self.assertEqual(self.invoke(FORMS[2])[-1], 'hostname:MiXeD-Disc')

    def test_activation_requires_token_and_valid_boot_record(self):
        self.save(); self.assertEqual(self.prepare(), 0)
        valid = self.active.read_bytes()
        for token in (None, 'v2', ''):
            self.assertEqual(self.invoke(FORMS[0], token), FORMS[0])
        for content in (b'v1\nname=Fake\n', valid.replace(self.boot.encode(), b'0'*36),
                        valid + b'unknown=yes\n', valid.replace(b'MiXeD-Disc', b'bad name'), b'x'*4096):
            self.active.chmod(0o644); self.active.write_bytes(content); self.active.chmod(0o444)
            self.assertEqual(self.invoke(FORMS[0]), FORMS[0])
        self.active.chmod(0o644); self.active.write_bytes(valid)
        self.assertEqual(self.invoke(FORMS[0]), FORMS[0])
        self.active.chmod(0o444)
        self.active.parent.chmod(0o777)
        self.assertEqual(self.invoke(FORMS[0]), FORMS[0])
        self.active.parent.chmod(0o755)
        self.active.unlink()
        target = self.root / 'active-copy'; target.write_bytes(valid); target.chmod(0o444)
        self.active.symlink_to(target)
        self.assertEqual(self.invoke(FORMS[0]), FORMS[0])
        self.active.unlink(); os.mkfifo(self.active)
        self.assertEqual(self.invoke(FORMS[0]), FORMS[0])

    def test_timeout_revokes_late_worker(self):
        self.save(); self.compile(delay=350, deadline=80)
        start = time.monotonic()
        self.assertNotEqual(self.prepare(), 0)
        self.assertLess(time.monotonic() - start, 0.3)
        time.sleep(0.4)
        self.assertFalse(self.active.exists()); self.assertFalse(self.hostname.exists())
        self.assertEqual(self.invoke(FORMS[0]), FORMS[0])

    def test_exec_pid_signals_and_cmdline(self):
        self.save(); self.assertEqual(self.prepare(), 0)
        env = dict(os.environ, DISKOS_NETWORK_NAMES='v1', WAIT_SIG='1')
        p = subprocess.Popen([str(self.root / 'udhcpc'), *FORMS[0]], env=env, stdout=subprocess.PIPE)
        try:
            # Wait until the replacement executable has printed its PID and argv.
            import select
            self.assertTrue(select.select([p.stdout], [], [], 2)[0])
            data = os.read(p.stdout.fileno(), 4096).split(b'\0')
            self.assertEqual(int(data[0]), p.pid)
            self.assertEqual(data[2], b'udhcpc')
            cmd = Path(f'/proc/{p.pid}/cmdline').read_bytes().split(b'\0')
            self.assertEqual(cmd[0], os.fsencode(self.bb))
            self.assertEqual(cmd[1], b'udhcpc')
            self.assertEqual(cmd[5], b'hostname:MiXeD-Disc')
            p.send_signal(signal.SIGTERM)
            self.assertEqual(p.wait(timeout=2), -signal.SIGTERM)
        finally:
            if p.poll() is None: p.kill(); p.wait()
            p.stdout.close()

    def test_stock_selection_and_failed_readiness_bypass(self):
        script = (ROOT / 'payload/S97diskos_names').read_text()
        marker = self.root / 'called'
        for selected, ready in ((False, False), (False, True), (True, False), (True, True)):
            if marker.exists(): marker.unlink()
            private = script.replace('/opt/diskos/bin/diskos-selected', 'true' if selected else 'false')
            private = private.replace('[ -f /tmp/.diskos_ready ]', 'true' if ready else 'false')
            private = private.replace('/opt/diskos/bin/diskos-names', f'touch {marker}')
            path = self.root / 'init'; path.write_text(private)
            shells = [['/bin/sh']]
            qemu = shutil.which('qemu-mipsel-static')
            if qemu:
                shells += [[qemu, '-L', str(FIXTURES / ('v' + v)),
                            str(FIXTURES / ('v' + v) / 'bin/busybox'), 'ash']
                           for v in ('209', '228', '240', '257')
                           if (FIXTURES / ('v' + v) / 'bin/busybox').is_file()]
            for shell in shells:
                if marker.exists(): marker.unlink()
                subprocess.run([*shell, str(path), 'start'], check=True)
                self.assertEqual(marker.exists(), selected and ready)

    def test_watchdog_inherits_path_and_token(self):
        self.save(); self.assertEqual(self.prepare(), 0)
        fragment = ib._NAMES_PATH.replace('/opt/diskos/bin/diskos-selected', 'true')
        fragment = fragment.replace('/opt/diskos/bin/diskos-names', str(self.root / 'names'))
        fragment = fragment.replace('/opt/diskos/netbin', str(self.root))
        # The same exported parent environment reaches each subsequent launch.
        script = fragment + '\nsh -c \'printf "%s %s\\n" "$DISKOS_NETWORK_NAMES" "$PATH"\'\n'*3
        result = subprocess.run(['/bin/sh', '-c', script], capture_output=True, text=True, check=True)
        self.assertEqual(len(result.stdout.splitlines()), 3)
        for line in result.stdout.splitlines(): self.assertTrue(line.startswith(f'v1 {self.root}:'))
        self.assertLess(ib._BLOCK.index(ib._NAMES_PATH), ib._BLOCK.index(ib._OTA_BRANCH))

    def test_missing_helper_and_snapshot_fail_closed(self):
        self.save(); self.assertEqual(self.prepare(), 0)
        self.active.unlink()
        self.assertNotEqual(subprocess.run([str(self.root / 'names'), '--active']).returncode, 0)
        self.assertEqual(self.invoke(FORMS[0]), FORMS[0])
        fragment = ib._NAMES_PATH.replace('/opt/diskos/bin/diskos-selected', 'true')
        fragment = fragment.replace('/opt/diskos/bin/diskos-names', str(self.root / 'missing'))
        result = subprocess.run(['/bin/sh', '-c', 'unset DISKOS_NETWORK_NAMES\n' + fragment +
                                '\nprintf "%s" "${DISKOS_NETWORK_NAMES-unset}"'], capture_output=True, text=True)
        self.assertEqual(result.stdout, 'unset')
        corrupt = self.root / 'corrupt'; corrupt.write_bytes(b'\x7fELFbroken'); corrupt.chmod(0o755)
        for helper in (self.root / 'missing', corrupt):
            script = (ROOT / 'payload/S97diskos_names').read_text()
            script = script.replace('/opt/diskos/bin/diskos-selected', 'true')
            script = script.replace('[ -f /tmp/.diskos_ready ]', 'true')
            script = script.replace('/opt/diskos/bin/diskos-names', str(helper))
            result = subprocess.run(['/bin/sh', '-c', script, 'init', 'start'],
                                    capture_output=True, timeout=2)
            self.assertEqual(result.returncode, 0)
            self.assertFalse(self.active.exists())


@unittest.skipUnless(all((FIXTURES / ('v' + v)).is_dir() for v in ('209', '228', '240', '257')),
                     'set DISKOS_NAMES_FIXTURES to all four extracted firmware fixtures')
class NamingImageTests(unittest.TestCase):
    def avahi_boot_sequence(self, selected):
        import test_image_edits as edits
        qemu = shutil.which('qemu-mipsel-static')
        if not qemu:
            self.skipTest('environment: qemu-mipsel-static required for fixture BusyBox ash')
        for version in ('209', '228', '240', '257'):
            with self.subTest(firmware=version, selected=selected):
                fixture = FIXTURES / ('v' + version)
                shell = [qemu, '-L', str(fixture), str(fixture / 'bin/busybox'), 'ash']
                with tempfile.TemporaryDirectory(prefix='avahi-boot-', dir=SCRATCH) as td:
                    work = Path(td)
                    fs = TreeFS(work / 'image')
                    for name in ('usr/project/fiio_init.sh', 'etc/init.d/S50avahi-daemon', 'bin/busybox'):
                        fs.replace_file(name.encode(), (fixture / name).read_bytes(), 0o755)
                    ib.edit_image(fs, bundle.data('mq_ui'), 'public', None, flash_id=edits.FLASH_ID)
                    ready, running, called = (work / n for n in ('ready', 'running', 'names-called'))
                    state = shlex.quote(str(running))
                    probe = f'''DAEMON=daemon_probe
daemon_probe() {{
    case "$1" in
        -c) [ -f {state} ] ;;
        -D) echo START; : > {state} ;;
        -k) echo STOP; rm {state} ;;
        -r) echo RELOAD ;;
        *) exit 90 ;;
    esac
}}'''
                    for name in ('S50avahi-daemon', 'S97zavahi'):
                        script = fs.read_bytes(('etc/init.d/' + name).encode()).decode('ascii')
                        script = script.replace('DAEMON=/usr/sbin/avahi-daemon', probe)
                        script = script.replace('/run/diskos-avahi-ready', str(ready))
                        (work / name).write_text(script)
                    naming = (ROOT / 'payload/S97diskos_names').read_text()
                    naming = naming.replace('/opt/diskos/bin/diskos-selected', 'true' if selected else 'false')
                    naming = naming.replace('[ -f /tmp/.diskos_ready ]', 'true')
                    naming = naming.replace('/opt/diskos/bin/diskos-names',
                                            'touch ' + shlex.quote(str(called)))
                    (work / 'S97diskos_names').write_text(naming)
                    # Confirm that this fixture's recovery path really uses stop then start.
                    recovery = (fixture / 'usr/bin/kill_mvc.sh').read_text()
                    self.assertLess(recovery.index('S50avahi-daemon stop'),
                                    recovery.index('S50avahi-daemon start'))

                    def run(name, action, expected):
                        result = subprocess.run([*shell, str(work / name), action],
                                                capture_output=True, text=True, timeout=5)
                        print(f'v{version} selected={selected} {name} {action}: '
                              f'exit={result.returncode} events={result.stdout.strip()!r} '
                              f'running={running.exists()}', flush=True)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout.splitlines(), expected)

                    run('S50avahi-daemon', 'start', [])
                    self.assertFalse(running.exists())
                    self.assertFalse(ready.exists())
                    run('S97diskos_names', 'start', [])
                    self.assertEqual(called.exists(), selected)
                    run('S97zavahi', 'start', ['START'])
                    self.assertTrue(running.exists())
                    run('S97zavahi', 'start', [])
                    run('S50avahi-daemon', 'start', [])
                    run('S50avahi-daemon', 'stop', ['STOP'])
                    self.assertFalse(running.exists())
                    run('S50avahi-daemon', 'start', ['START'])
                    self.assertTrue(running.exists(), 'kill_mvc.sh must leave Avahi running')
                    self.assertTrue(ready.is_dir())
                    run('S50avahi-daemon', 'reload', ['RELOAD'])
                    self.assertTrue(running.exists())

    def test_avahi_boot_and_later_stock_controls(self):
        self.avahi_boot_sequence(selected=True)

    def test_stock_selection_avahi_boot_and_later_stock_controls(self):
        self.avahi_boot_sequence(selected=False)

    def test_four_firmware_images_and_avahi_order(self):
        import test_image_edits as edits
        for version in ('209', '228', '240', '257'):
            root = FIXTURES / f'v{version}'
            self.assertTrue(root.is_dir(), f'missing firmware fixture: {root}')
            with tempfile.TemporaryDirectory(prefix='names-image-', dir=SCRATCH) as td:
                fs = TreeFS(td)
                for name in ('usr/project/fiio_init.sh', 'etc/init.d/S50avahi-daemon', 'bin/busybox'):
                    fs.replace_file(name.encode(), (root / name).read_bytes(), 0o755)
                boot, bb, sha, size = ib.edit_image(fs, bundle.data('mq_ui'), 'public', None,
                                                 flash_id=edits.FLASH_ID)
                ib.validate_image(fs, sha, size, bb, boot, None, edits.FLASH_ID)
                self.assertEqual(fs.read_bytes(b'etc/init.d/S97zavahi'), ib._LATE_AVAHI)
                self.assertIn(b'[ -d /run/diskos-avahi-ready ] || exit 0',
                              fs.read_bytes(b'etc/init.d/S50avahi-daemon'))
                order = sorted(['S50avahi-daemon', 'S96diskos_select', 'S97diskos_install',
                                'S97diskos_names', 'S97zavahi', 'S98FIIO'])
                self.assertLess(order.index('S97diskos_names'), order.index('S97zavahi'))
                # Both newly installed hooks contain exact validated payload bytes.
                for rel, name in ib._NAMES_FILES:
                    self.assertEqual(fs.read_bytes(rel.encode()), Path(bundle.data(name)).read_bytes())

    def test_naming_declaration_and_avahi_corruption_rejected(self):
        import test_image_edits as edits
        root = FIXTURES / 'v257'
        for path in (ib._NAMES_DECL, 'etc/init.d/S50avahi-daemon', 'etc/init.d/S97zavahi'):
            for fault in ('missing', 'corrupt', 'mode', 'symlink'):
                with tempfile.TemporaryDirectory(prefix='names-image-', dir=SCRATCH) as td:
                    fs = TreeFS(td)
                    for name in ('usr/project/fiio_init.sh', 'etc/init.d/S50avahi-daemon', 'bin/busybox'):
                        fs.replace_file(name.encode(), (root / name).read_bytes(), 0o755)
                    boot, bb, sha, size = ib.edit_image(fs, bundle.data('mq_ui'), 'public', None,
                                                     flash_id=edits.FLASH_ID)
                    p = Path(td, path)
                    data = p.read_bytes(); p.unlink()
                    if fault == 'corrupt': p.write_bytes(b'bad'); p.chmod(0o755)
                    elif fault == 'mode': p.write_bytes(data); p.chmod(0o600)
                    elif fault == 'symlink': p.symlink_to('missing')
                    with self.assertRaises(ib.BuildError):
                        ib.validate_image(fs, sha, size, bb, boot, None, edits.FLASH_ID)
