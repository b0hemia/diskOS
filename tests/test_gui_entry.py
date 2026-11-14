# SPDX-License-Identifier: GPL-3.0-or-later
"""Windowed startup and inherited streams, without Windows or USB access."""
from contextlib import ExitStack
import errno
import io
import itertools
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from diskos_installer import gui_entry as entry, service, state

native_dialog = entry._show_error


class StartupFailureRegressionTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.file = io.BytesIO()
        self.log = entry._DiagnosticLog(mock.Mock(), {entry.LOG_NAMES[0]: self.file}, 0)
        self.stack.enter_context(mock.patch.object(entry, '_open_log',
                                                   return_value=(self.log, 'bounded/gui-0.log')))
        self.dialog = self.stack.enter_context(mock.patch.object(entry, '_show_error'))
        self.saved = sys.stdin, sys.stdout, sys.stderr

    def test_real_windowed_dispatch_logs_startup_failures_without_temp_traceback(self):
        from diskos_installer import __main__ as cli, gui
        private = 'C:/Users/private/music/startup.tcl SSID=secret'
        for stage in ('Tk', 'App', 'mainloop'):
            with self.subTest(stage=stage), ExitStack() as stack:
                self.file.seek(0)
                self.file.truncate()
                # main closes its log; each startup needs a fresh stream.
                self.log = entry._DiagnosticLog(mock.Mock(), {entry.LOG_NAMES[0]: self.file}, 0)
                stack.enter_context(mock.patch.object(entry, '_open_log',
                                                      return_value=(self.log, 'bounded/gui-0.log')))
                tk = types.SimpleNamespace(Tk=mock.Mock())
                app = stack.enter_context(mock.patch.object(gui, 'App'))
                target = {'Tk': tk.Tk, 'App': app, 'mainloop': tk.Tk.return_value.mainloop}[stage]
                target.side_effect = RuntimeError(private)
                mb = types.SimpleNamespace(showerror=mock.Mock())
                tk.messagebox = mb
                stack.enter_context(mock.patch.dict(sys.modules, tkinter=tk,
                                                    **{'tkinter.messagebox': mb}))
                stack.enter_context(mock.patch.object(cli, '_Lock'))
                temp = stack.enter_context(mock.patch('tempfile.mkstemp',
                                                       side_effect=OSError('no temp logs permitted')))
                self.dialog.reset_mock()
                self.assertEqual(entry.main([]), 1)
                temp.assert_not_called()
                mb.showerror.assert_not_called()
                self.dialog.assert_called_once()
                self.assertIn('bounded/gui-0.log', self.dialog.call_args.args[0])
                data = self.file.getvalue().decode('ascii')
                self.assertIn('failure RuntimeError', data)
                self.assertNotIn(private, data)
                self.assertNotIn('Traceback', data)
                self.assertLessEqual(len(data), entry.LOG_LIMIT)
                self.assertEqual((sys.stdin, sys.stdout, sys.stderr), self.saved)

    def test_console_startup_keeps_existing_traceback_log_and_dialog(self):
        from diskos_installer import gui
        captured = []
        class Capture(io.StringIO):
            def close(self):
                captured.append(self.getvalue())
                super().close()
        tk = types.SimpleNamespace(Tk=mock.Mock(side_effect=RuntimeError('console detail')))
        mb = types.SimpleNamespace(showerror=mock.Mock())
        tk.messagebox = mb
        with mock.patch.dict(sys.modules, tkinter=tk, **{'tkinter.messagebox': mb}), \
                mock.patch.object(sys, 'stderr', io.StringIO()), \
                mock.patch('tempfile.mkstemp', return_value=(99, 'temporary/startup.log')) as temp, \
                mock.patch.object(gui.os, 'fdopen', return_value=Capture()):
            self.assertEqual(gui.main(), 1)
        temp.assert_called_once()
        mb.showerror.assert_called_once()
        self.assertIn('Traceback', captured[0])
        self.assertIn('console detail', captured[0])

    def test_failed_diagnostic_writes_always_reach_startup_dialog(self):
        for stage in ('startup', 'success', 'status', 'exception'):
            with self.subTest(stage=stage), ExitStack() as stack:
                self.log = entry._DiagnosticLog(mock.Mock(), {entry.LOG_NAMES[0]: io.BytesIO()}, 0)
                stack.enter_context(mock.patch.object(entry, '_open_log',
                                                      return_value=(self.log, 'bounded/gui-0.log')))
                # Fail the initial write or a later result/failure event, then
                # fail every attempt to report that failure as on a full disk.
                writes = itertools.chain([] if stage == 'startup' else [None],
                                         itertools.repeat(OSError('disk full')))
                stack.enter_context(mock.patch.object(self.log.file, 'write', side_effect=writes))
                run = stack.enter_context(mock.patch.object(entry, '_run_installer',
                                                             return_value=1 if stage == 'status' else 0))
                if stage == 'exception':
                    run.side_effect = RuntimeError('startup error')
                self.dialog.reset_mock()
                self.assertEqual(entry.main([]), 1)
                self.dialog.assert_called_once()
                self.assertIn('bounded/gui-0.log', self.dialog.call_args.args[0])
                self.assertEqual((sys.stdin, sys.stdout, sys.stderr), self.saved)
                self.assertTrue(self.log.closed)
                if stage == 'startup':
                    run.assert_not_called()


class CleanupRegressionTests(unittest.TestCase):
    def test_posix_cleanup_refuses_even_explicit_localappdata(self):
        for local in ('', 'relative', '/profile'):
            with self.subTest(local=local), mock.patch.object(entry.os, 'name', 'posix'), \
                    mock.patch.dict(os.environ, LOCALAPPDATA=local), \
                    mock.patch.object(entry, '_LogDirectory',
                                      side_effect=AssertionError('POSIX cleanup must not open paths')) as directory:
                entry.remove_logs()
                directory.assert_not_called()

    def test_windows_cleanup_deletes_validated_handles_in_order(self):
        directory = mock.Mock()
        marker, file = mock.Mock(), mock.Mock()
        marker.fileno.return_value, file.fileno.return_value = 10, 11
        directory.directory = 12
        directory.owned_files.return_value = marker, {entry.LOG_NAMES[0]: file}
        with mock.patch.object(entry.os, 'name', 'nt'), \
                mock.patch.object(entry, '_LogDirectory', return_value=directory) as opened, \
                mock.patch.dict(sys.modules, msvcrt=types.SimpleNamespace(get_osfhandle=lambda fd: fd + 100)), \
                mock.patch.object(entry.os, 'unlink') as unlink, mock.patch.object(entry.os, 'rmdir') as rmdir:
            entry.remove_logs()
        opened.assert_called_once_with(removing=True)
        self.assertEqual(directory.win.delete.call_args_list, [mock.call(111), mock.call(110), mock.call(12)])
        file.close.assert_called_once()
        marker.close.assert_called_once()
        directory.close.assert_called_once()
        unlink.assert_not_called()
        rmdir.assert_not_called()


def fixture_mounts(stack, home):
    # The sandbox mounts both writable roots. Trust that fixture anchor while
    # retaining real mount checks on the profile itself and all descendants.
    real = entry._mountpoint
    stack.enter_context(mock.patch.object(entry, '_mountpoint', side_effect=lambda path, *args:
                        real(path, *args) if path == str(home) or path.startswith(str(home) + os.sep) else False))


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.home = Path(self.stack.enter_context(tempfile.TemporaryDirectory()))
        fixture_mounts(self.stack, self.home)
        self.stack.enter_context(mock.patch.dict(os.environ, LOCALAPPDATA=str(self.home)))
        for name in ('stdin', 'stdout', 'stderr'):
            self.stack.enter_context(mock.patch.object(sys, name, None))
        self.dialog = self.stack.enter_context(mock.patch.object(entry, '_show_error'))

    def logs(self):
        return [p for p in (self.home / entry.LOG_FOLDER).glob('gui-*.log') if p.stat().st_size]

    def test_fresh_profile_state_initializes_after_bootstrap(self):
        for explicit in (False, True):
            with self.subTest(explicit=explicit), mock.patch.dict(os.environ), \
                    mock.patch.object(sys, 'platform', 'win32'):
                local = self.home / ('explicit' if explicit else 'default')
                base = local / state.APP
                os.environ['LOCALAPPDATA'] = str(local)
                if explicit:
                    os.environ['DISKOS_INSTALLER_HOME'] = str(base)
                else:
                    os.environ.pop('DISKOS_INSTALLER_HOME', None)

                def run(argv):
                    self.assertEqual(state.state_dir(), str(base))
                    self.assertTrue((base / state.SENTINEL).is_file())
                    return 0

                with mock.patch.object(entry, '_run_installer', side_effect=run):
                    self.assertEqual(entry.main([]), 0)
                self.assertEqual(len(list((local / entry.LOG_FOLDER).glob('gui-*.log'))), entry.LOG_SLOTS)
                self.assertEqual(list(base.iterdir()), [base / state.SENTINEL])
                self.dialog.assert_not_called()

    def test_bootstrap_preserves_foreign_state_directory_refusal(self):
        base = self.home / state.APP
        base.mkdir()
        foreign = base / 'foreign.txt'
        foreign.write_text('unrelated data')
        with mock.patch.dict(os.environ, DISKOS_INSTALLER_HOME=str(base)), \
                mock.patch.object(sys, 'platform', 'win32'), \
                mock.patch.object(entry, '_run_installer', side_effect=lambda argv: state.state_dir()):
            self.assertEqual(entry.main([]), 1)
        self.assertEqual(list(base.iterdir()), [foreign])
        self.assertEqual(foreign.read_text(), 'unrelated data')
        log, = self.logs()
        self.assertIn('E142', log.read_text())
        self.assertIn('refusing to use it', self.dialog.call_args.args[0])

    def test_parent_streams_ready_log_retained_and_runs_unique(self):
        def run(argv):
            self.assertEqual(argv, [])
            self.assertIs(sys.stdout, sys.stderr)
            self.assertFalse(sys.stdout.isatty())
            self.assertEqual(sys.stdin.buffer.read(), b'')
            print('retained diagnostics')
            return 0
        with mock.patch.object(entry, '_run_installer', side_effect=run):
            self.assertEqual(entry.main([]), 0)
            self.assertEqual(entry.main([]), 0)
        self.assertEqual(len(self.logs()), 2)
        for log in self.logs():
            self.assertIn('installer returned success', log.read_text())
            self.assertNotIn('retained diagnostics', log.read_text())
        self.assertIsNone(sys.stdout)
        self.dialog.assert_not_called()

    def test_import_failure_traceback_and_dialog_include_retained_path(self):
        with mock.patch.object(entry, '_run_installer', side_effect=ImportError('Tk import failed')):
            self.assertEqual(entry.main([]), 1)
        log, = self.logs()
        self.assertIn('failure ImportError', log.read_text())
        self.assertNotIn('Tk import failed', log.read_text())
        message = self.dialog.call_args.args[0]
        self.assertIn(str(log), message)
        self.assertIn('diskos-installer-cli.exe doctor', message)

    def test_reported_gui_failure_and_system_exit_are_visible(self):
        for result in (1, 2, SystemExit('lock unavailable'), SystemExit(2)):
            with self.subTest(result=result), mock.patch.object(entry, '_run_installer') as run:
                if isinstance(result, SystemExit):
                    run.side_effect = result
                else:
                    run.return_value = result
                self.dialog.reset_mock()
                self.assertEqual(entry.main([]), 1)
                self.dialog.assert_called_once()
        with mock.patch.object(entry, '_run_installer', side_effect=SystemExit(0)):
            self.dialog.reset_mock()
            self.assertEqual(entry.main([]), 0)
            self.dialog.assert_not_called()

    def test_log_failure_stops_before_import_and_shows_error(self):
        for home in ('', 'relative'):
            with mock.patch.dict(os.environ, LOCALAPPDATA=home), \
                    mock.patch.object(entry, '_run_installer') as run:
                self.assertEqual(entry.main([]), 1)
                run.assert_not_called()
        with mock.patch.object(entry, '_open_log', side_effect=PermissionError('read-only profile')):
            self.assertEqual(entry.main([]), 1)
        self.assertIn('read-only profile', self.dialog.call_args.args[0])

    def test_missing_child_stream_stops_without_gui_or_log(self):
        opened = io.StringIO()
        with mock.patch.object(entry, '_child_stream', side_effect=[opened, OSError('missing pipe')]), \
                mock.patch.object(entry, '_run_child') as child, \
                mock.patch.object(entry, '_run_installer') as parent:
            self.assertEqual(entry.main([entry.CHILD_MODE, 'a' * 64]), 1)
            child.assert_not_called()
            parent.assert_not_called()
        self.assertTrue(opened.closed)
        self.assertEqual(self.logs(), [])
        self.dialog.assert_not_called()

    def test_fresh_process_imports_installer_only_after_stream_setup(self):
        script = """
import sys
sys.stdin = sys.stdout = sys.stderr = None
from diskos_installer import gui_entry
import os
real_mount = gui_entry._mountpoint
home = os.environ['LOCALAPPDATA']
gui_entry._mountpoint = lambda path, *args: real_mount(path, *args) if path == home or path.startswith(home + os.sep) else False
def run(argv):
    from diskos_installer import __main__, ui
    assert ui._TTY is False
    print('fresh import succeeded')
    return 0
gui_entry._run_installer = run
raise SystemExit(gui_entry.main([]))
"""
        result = subprocess.run([sys.executable, '-c', script], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('installer returned success', self.logs()[0].read_text())

    def test_native_dialog_does_not_depend_on_tk(self):
        api = mock.Mock()
        with mock.patch.object(entry.ctypes, 'WinDLL', return_value=api, create=True):
            from ctypes import wintypes as w
            native_dialog('startup failed')
            self.assertEqual(api.MessageBoxW.argtypes, [w.HWND, w.LPCWSTR, w.LPCWSTR, w.UINT])
        api.MessageBoxW.assert_called_once_with(None, 'startup failed', 'diskOS Installer', 0x10010)


class LogFixture:
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.home = Path(self.stack.enter_context(tempfile.TemporaryDirectory()))
        fixture_mounts(self.stack, self.home)
        self.stack.enter_context(mock.patch.dict(os.environ, LOCALAPPDATA=str(self.home)))
        self.folder = self.home / entry.LOG_FOLDER

    def start(self):
        log, path = entry._open_log()
        self.addCleanup(log.close)
        return log, Path(path)

    def contents(self):
        return ''.join(p.read_text() for p in self.folder.glob('gui-*.log'))


class LogLifecycleTests(LogFixture, unittest.TestCase):

    def test_symlink_directory_and_ancestor_refused_without_external_writes(self):
        external = self.home / 'external'
        external.mkdir()
        foreign = external / 'private.txt'
        foreign.write_text('untouched')
        for link, base in ((self.folder, self.home), (self.home / 'profile', self.home / 'profile')):
            with self.subTest(link=link):
                try:
                    link.symlink_to(external, target_is_directory=True)
                except OSError:
                    self.skipTest('symlink creation requires OS permission; reparse mocks cover Windows')
                with mock.patch.dict(os.environ, LOCALAPPDATA=str(base)):
                    with self.assertRaises(OSError):
                        entry._open_log()
        self.assertEqual(list(external.iterdir()), [foreign])
        self.assertEqual(foreign.read_text(), 'untouched')

    @unittest.skipIf(os.name == 'nt', 'Windows reparse handles tested separately')
    def test_mount_point_refused(self):
        real = os.path.ismount
        with mock.patch.object(entry.os.path, 'ismount', side_effect=lambda p: p == str(self.folder) or real(p)):
            with self.assertRaises(OSError):
                entry._open_log()
        self.assertEqual(list(self.folder.iterdir()), [])

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux bind mount table')
    def test_bind_mount_on_same_device_refused(self):
        real_open = open
        line = f'42 1 0:1 / {self.folder} rw - ext4 disk rw\n'
        def open_table(path, *args, **kwargs):
            return io.StringIO(line) if path == '/proc/self/mountinfo' else real_open(path, *args, **kwargs)
        with mock.patch('builtins.open', side_effect=open_table):
            with self.assertRaises(OSError):
                entry._open_log()

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux file bind mount table')
    def test_bind_mounted_log_file_never_truncated(self):
        log, path = self.start()
        log.event('untouched file')
        log.close()
        real_open = open
        line = f'42 1 0:1 / {path} rw - ext4 disk rw\n'
        def open_table(name, *args, **kwargs):
            return io.StringIO(line) if name == '/proc/self/mountinfo' else real_open(name, *args, **kwargs)
        with mock.patch('builtins.open', side_effect=open_table):
            with self.assertRaisesRegex(OSError, 'mount point'):
                entry._open_log()
        self.assertEqual(path.read_text(), 'untouched file\n')

    def test_unknown_or_unmarked_directory_is_never_adopted(self):
        self.folder.mkdir()
        foreign = self.folder / 'gui-old.log'
        foreign.write_text('legacy or foreign, ownership unverified')
        with self.assertRaises(OSError):
            entry._open_log()
        self.assertEqual(list(self.folder.iterdir()), [foreign])

    def test_symlink_and_hardlink_files_never_truncated(self):
        external = self.home / 'private.txt'
        external.write_text('private contents')
        self.folder.mkdir(mode=0o700)
        marker = self.folder / entry.LOG_MARKER
        marker.write_bytes(entry.LOG_MAGIC)
        marker.chmod(0o600)
        (self.folder / entry.LOG_NAMES[0]).symlink_to(external)
        with self.assertRaises(OSError):
            entry._open_log()
        # A separate profile exercises hard links without changing the symlink.
        profile = self.home / 'hardlinks'
        profile.mkdir()
        folder = profile / entry.LOG_FOLDER
        folder.mkdir(mode=0o700)
        marker = folder / entry.LOG_MARKER
        marker.write_bytes(entry.LOG_MAGIC)
        marker.chmod(0o600)
        os.link(external, folder / entry.LOG_NAMES[0])
        with mock.patch.dict(os.environ, LOCALAPPDATA=str(profile)):
            with self.assertRaises(OSError):
                entry._open_log()
        self.assertEqual(external.read_text(), 'private contents')

    @unittest.skipIf(os.name == 'nt', 'POSIX fd boundary; Windows handle boundary below')
    def test_file_replacement_between_stat_and_open_refused(self):
        log, path = self.start()
        log.close()
        replacement = self.folder / 'replacement'
        replacement.write_text('untouched replacement')
        replacement.chmod(0o600)
        real_open = os.open
        def swapped(name, flags, *args, **kwargs):
            if name == path.name:
                os.replace(replacement, path)
            return real_open(name, flags, *args, **kwargs)
        # Test the open primitive directly: the foreign name would otherwise
        # correctly prevent adoption before reaching this race boundary.
        directory = entry._LogDirectory()
        self.addCleanup(directory.close)
        with mock.patch.object(entry.os, 'open', side_effect=swapped):
            with self.assertRaisesRegex(OSError, 'changed while opening'):
                directory.open_file(path.name)
        self.assertEqual(path.read_text(), 'untouched replacement')

    def test_second_instance_refused_and_first_can_keep_writing(self):
        log, path = self.start()
        log.event('first instance')
        with self.assertRaisesRegex(OSError, 'diskOS Installer is already open'):
            entry._open_log()
        with mock.patch.object(entry, '_show_error') as dialog, \
                mock.patch.object(entry, '_run_installer') as run:
            self.assertEqual(entry.main([]), 1)
            run.assert_not_called()
        dialog.assert_called_once_with('diskOS Installer is already open. Close it and retry.')
        self.assertIn('first instance', path.read_text())
        log.event('still running')
        self.assertIn('still running', path.read_text())

    @unittest.skipIf(os.name == 'nt', 'POSIX flock boundary')
    def test_only_flock_contention_gets_the_already_open_refusal(self):
        import fcntl
        for number in (errno.EWOULDBLOCK, errno.EAGAIN, errno.EACCES, errno.EIO):
            error = OSError(number, 'lock failure')
            with self.subTest(number=number), \
                    mock.patch.object(fcntl, 'flock', side_effect=error), \
                    mock.patch.object(entry, '_show_error') as dialog, \
                    mock.patch.object(entry, '_run_installer') as run:
                self.assertEqual(entry.main([]), 1)
                run.assert_not_called()
                text = dialog.call_args.args[0]
                if number in (errno.EWOULDBLOCK, errno.EAGAIN):
                    self.assertEqual(text, 'diskOS Installer is already open. Close it and retry.')
                else:
                    self.assertIn('Could not start diskOS Installer', text)
                    self.assertIn('lock failure', text)

    def test_ring_rotates_across_launches_and_within_a_large_session(self):
        for n in range(entry.LOG_SLOTS + 3):
            log, _ = self.start()
            log.event(f'session {n}')
            log.close()
        text = self.contents()
        self.assertNotIn('session 0', text)
        for n in range(3, entry.LOG_SLOTS + 3):
            self.assertIn(f'session {n}', text)
        log, _ = self.start()
        for _ in range(40):
            log.event('x' * (entry.LOG_LIMIT // 3))
        log.close()
        files = list(self.folder.glob('gui-*.log'))
        self.assertEqual(len(files), entry.LOG_SLOTS)
        self.assertTrue(all(p.stat().st_size <= entry.LOG_LIMIT for p in files))
        self.assertLessEqual(sum(p.stat().st_size for p in self.folder.iterdir()),
                             entry.LOG_SLOTS * entry.LOG_LIMIT + len(entry.LOG_MAGIC))

    def test_tied_mtimes_keep_recent_sessions_and_resume_after_size_rotation(self):
        stamp = 1_600_000_000_000_000_000
        def tie_mtimes():
            for path in self.folder.glob('gui-*.log'):
                os.utime(path, ns=(stamp, stamp))
            self.assertEqual(len({p.stat().st_mtime_ns
                                  for p in self.folder.glob('gui-*.log')}), 1)

        launches = entry.LOG_SLOTS * 3
        for n in range(launches):
            log, _ = self.start()
            log.event(f'session {n}')
            log.close()
            tie_mtimes()
            self.assertCountEqual(self.contents().splitlines(),
                                  [f'session {k}' for k in
                                   range(max(0, n - entry.LOG_SLOTS + 1), n + 1)])

        log, _ = self.start()
        full = 'x' * (entry.LOG_LIMIT - 1)
        log.event(full)
        log.event('overflow')
        log.close()
        tie_mtimes()
        log, _ = self.start()
        log.event('next launch')
        log.close()
        self.assertCountEqual(self.contents().splitlines(),
                              [f'session {launches - 1}', full, 'overflow', 'next launch'])

    def test_existing_timestamp_order_is_preserved_on_rotation(self):
        log, _ = self.start()
        log.close()
        newest_first = list(reversed(entry.LOG_NAMES))
        for rank, name in enumerate(newest_first):
            path = self.folder / name
            path.write_text(f'old chunk {rank}\n')
            stamp = (1_600_000_000 - rank) * 1_000_000_000
            os.utime(path, ns=(stamp, stamp))
        log, _ = self.start()
        log.event('new chunk')
        log.close()
        self.assertEqual((self.folder / entry.LOG_NAMES[0]).read_text(), 'new chunk\n')
        for rank, name in enumerate(entry.LOG_NAMES[1:]):
            self.assertEqual((self.folder / name).read_text(), f'old chunk {rank}\n')

    def test_private_raw_output_and_exception_details_are_not_persisted(self):
        secrets = ['SSID=LivingRoom', 'serial=123456789', 'MAC=aa:bb:cc:dd:ee:ff',
                   str(self.home / 'personal' / 'music.flac')]
        def run(argv):
            for value in secrets:
                sys.stdout.write(value[:4])
                sys.stderr.write(value[4:] + '\n')
            raise RuntimeError(' '.join(secrets))
        with mock.patch.object(entry, '_run_installer', side_effect=run), \
                mock.patch.object(entry, '_show_error'):
            self.assertEqual(entry.main([]), 1)
        text = self.contents()
        self.assertIn('failure RuntimeError', text)
        for secret in secrets:
            self.assertNotIn(secret, text)
        self.assertNotIn(str(self.home), text)


class LogRotationFailureTests(unittest.TestCase):
    def test_migration_copy_failure_preserves_the_newest_chunk(self):
        class FaultFile(io.BytesIO):
            def __init__(self, name, data):
                super().__init__(data)
                self.name = name

            def write(self, data):
                if self.name == failed_name and operation == 'write':
                    raise OSError('injected copy failure')
                return super().write(data)

            def truncate(self, size=None):
                if self.name == failed_name and operation == 'truncate':
                    raise OSError('injected truncate failure')
                return super().truncate(size)

        for newest_slot in range(entry.LOG_SLOTS):
            order = entry.LOG_NAMES[newest_slot:] + entry.LOG_NAMES[:newest_slot]
            for failed_name in entry.LOG_NAMES:
                operations = ('truncate',) if failed_name == entry.LOG_NAMES[0] else ('write', 'truncate')
                for operation in operations:
                    with self.subTest(newest_slot=newest_slot, file=failed_name, operation=operation):
                        files = {name: FaultFile(name, f'old chunk {rank}\n'.encode('ascii'))
                                 for rank, name in enumerate(order)}
                        history = [files[name].getvalue() for name in order[:-1]]
                        with self.assertRaises(OSError):
                            entry._DiagnosticLog(mock.Mock(), files, 0, history)
                        self.assertIn(history[0], [file.getvalue() for file in files.values()])
                        self.assertTrue(all(len(file.getvalue()) <= entry.LOG_LIMIT
                                            for file in files.values()))

    def test_rotation_completes_short_copy_writes(self):
        class ShortFile(io.BytesIO):
            def write(self, data):
                return super().write(data[:3])

        newest = b'newest complete chunk\n'
        files = {name: ShortFile() for name in entry.LOG_NAMES}
        files[entry.LOG_NAMES[0]] = io.BytesIO(newest)
        log = entry._DiagnosticLog(mock.Mock(), files, 0)
        self.addCleanup(log.close)
        self.assertEqual(files[entry.LOG_NAMES[1]].getvalue(), newest)
        self.assertEqual(files[entry.LOG_NAMES[0]].getvalue(), b'')

    def test_rotation_rejects_a_stalled_copy_without_clearing_the_source(self):
        class StalledFile(io.BytesIO):
            def write(self, data):
                return 0

        newest = b'newest complete chunk\n'
        files = {name: io.BytesIO() for name in entry.LOG_NAMES}
        files[entry.LOG_NAMES[0]] = io.BytesIO(newest)
        files[entry.LOG_NAMES[1]] = StalledFile()
        with self.assertRaises(OSError):
            entry._DiagnosticLog(mock.Mock(), files, 0)
        self.assertEqual(files[entry.LOG_NAMES[0]].getvalue(), newest)


class WindowsLogBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.api = mock.Mock()
        self.attrs, self.links, self.identity = 0, 1, 123
        self.api.CreateFileW.return_value = 99
        self.api.GetFileInformationByHandle.side_effect = self.fill_info
        with mock.patch.object(entry.ctypes, 'WinDLL', return_value=self.api, create=True):
            self.win = entry._WindowsIO()

    def fill_info(self, handle, pointer):
        info = pointer._obj
        info[0], info[10], info[12] = self.attrs, self.links, self.identity
        return 1

    def test_junction_symlink_and_volume_mount_reparse_handles_refused(self):
        # Each Windows redirection kind carries FILE_ATTRIBUTE_REPARSE_POINT,
        # including junctions and volume mounts with IO_REPARSE_TAG_MOUNT_POINT.
        for attrs in (0x400, 0x410):
            with self.subTest(attrs=attrs):
                self.attrs = attrs
                with self.assertRaisesRegex(OSError, 'redirected'):
                    self.win.open(r'C:\profile\logs', directory=bool(attrs & 0x10))
        self.assertEqual(self.api.CloseHandle.call_count, 2)

    def test_share_flags_pin_directory_names_and_exclude_file_writers(self):
        self.attrs = 0x10
        self.win.open(r'C:\profile', directory=True, expected=types.SimpleNamespace(st_ino=123))
        args = self.api.CreateFileW.call_args.args
        self.assertEqual(args[2], 3)  # READ | WRITE, never DELETE sharing
        self.assertEqual(args[5], 0x02200000)  # OPEN_REPARSE_POINT | BACKUP_SEMANTICS
        self.attrs = 0
        self.win.open(r'C:\profile\gui-0.log', create=True)
        args = self.api.CreateFileW.call_args.args
        self.assertEqual(args[2], 1)  # READ only, excludes writers and deletion
        self.assertEqual(args[4], 1)  # CREATE_NEW, no destructive open

    def test_second_gui_sharing_violation_shows_calm_refusal(self):
        error = OSError('raw sharing violation')
        error.winerror = 32
        self.api.CreateFileW.return_value = entry.ctypes.c_void_p(-1).value
        with mock.patch.object(entry.ctypes, 'get_last_error', return_value=32, create=True), \
                mock.patch.object(entry.ctypes, 'WinError', return_value=error, create=True), \
                mock.patch.object(entry, '_open_log',
                                  side_effect=lambda: self.win.open(r'C:\profile\gui-0.log')), \
                mock.patch.object(entry, '_show_error') as dialog, \
                mock.patch.object(entry, '_run_installer') as run:
            self.assertEqual(entry.main([]), 1)
            run.assert_not_called()
        dialog.assert_called_once_with('diskOS Installer is already open. Close it and retry.')
        self.assertEqual(self.api.CreateFileW.call_args.args[2], 1)

    def test_other_windows_open_failures_keep_startup_error_details(self):
        for number, directory in ((5, False), (32, True)):
            error = OSError('real open failure')
            error.winerror = number
            self.api.CreateFileW.return_value = entry.ctypes.c_void_p(-1).value
            with self.subTest(number=number, directory=directory), \
                    mock.patch.object(entry.ctypes, 'get_last_error', return_value=number, create=True), \
                    mock.patch.object(entry.ctypes, 'WinError', return_value=error, create=True), \
                    mock.patch.object(entry, '_open_log', side_effect=lambda:
                                      self.win.open(r'C:\profile\logs', directory=directory)), \
                    mock.patch.object(entry, '_show_error') as dialog:
                self.assertEqual(entry.main([]), 1)
                text = dialog.call_args.args[0]
                self.assertIn('Could not start diskOS Installer', text)
                self.assertIn('real open failure', text)
                self.assertIn('Details:', text)

    def test_identity_changed_and_hardlinked_handles_refused(self):
        with self.assertRaisesRegex(OSError, 'changed while opening'):
            self.win.open(r'C:\profile\gui-0.log', expected=types.SimpleNamespace(st_ino=124))
        self.links = 2
        with self.assertRaisesRegex(OSError, 'hard-linked'):
            self.win.open(r'C:\profile\gui-0.log')
        self.assertEqual(self.api.CloseHandle.call_count, 2)

    def test_full_windows_file_identity_checked(self):
        identity = (1 << 96) + 123
        def fill_id(handle, kind, pointer, size):
            self.assertEqual((kind, size), (18, 24))
            data = identity.to_bytes(16, 'little')
            pointer._obj[8:] = data
            return 1
        self.api.GetFileInformationByHandleEx.side_effect = fill_id
        expected = types.SimpleNamespace(st_ino=identity)
        self.assertEqual(self.win.open(r'C:\profile\gui-0.log', expected=expected), 99)
        expected.st_ino += 1
        with self.assertRaisesRegex(OSError, 'changed while opening'):
            self.win.open(r'C:\profile\gui-0.log', expected=expected)

    def test_cleanup_uses_delete_access_and_handle_disposition(self):
        self.win.open(r'C:\profile\gui-0.log', removing=True)
        self.assertTrue(self.api.CreateFileW.call_args.args[1] & 0x10000)
        self.win.delete(99)
        args = self.api.SetFileInformationByHandle.call_args.args
        self.assertEqual((args[0], args[1], args[3]), (99, 4, 1))
        self.assertEqual(args[2]._obj.value, 1)


@unittest.skipIf(os.name == 'nt', 'POSIX cleanup refusal; Windows handles tested separately')
class RemoveLogTests(LogFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.rep = mock.Mock()
        self.confirm = mock.Mock(return_value=True)
        self.stack.enter_context(mock.patch.object(service.state, 'load', return_value={}))
        self.wipe = self.stack.enter_context(mock.patch.object(service.state, 'wipe_all',
                                                              side_effect=lambda: ('test state', [])))

    def remove(self, **params):
        return service.do_remove(params, self.rep, self.confirm)

    def test_posix_remove_closes_live_gui_handles_and_retains_folder(self):
        log, _ = self.start()
        with mock.patch.object(sys, 'stdout', log), mock.patch.object(sys, 'stderr', log):
            result = self.remove()
            self.assertTrue(log.closed)
            print('completion must not recreate logs')
            log.event('completion')
        self.assertTrue(result['ok'])
        self.assertTrue(self.folder.exists())
        self.assertNotIn('completion', self.contents())
        self.wipe.assert_called_once()

    def test_cli_remove_closed_logs_and_absent_logs(self):
        log, _ = self.start()
        log.close()
        self.assertTrue(self.remove()['ok'])
        self.assertTrue(self.folder.exists())
        with mock.patch.dict(os.environ, LOCALAPPDATA=str(self.home / 'absent')):
            self.assertTrue(self.remove()['ok'])
        self.assertTrue(self.remove()['ok'])

    @unittest.skipIf(os.name == 'nt', 'native POSIX Remove')
    def test_native_remove_without_windows_log_location(self):
        with mock.patch.dict(os.environ):
            os.environ.pop('LOCALAPPDATA', None)
            self.assertTrue(self.remove()['ok'])
        self.assertFalse(self.folder.exists())

    def test_posix_remove_ignores_redirected_folder_without_touching_target(self):
        external = self.home / 'external'
        external.mkdir()
        foreign = external / 'private.txt'
        foreign.write_text('untouched')
        self.folder.symlink_to(external, target_is_directory=True)
        result = self.remove()
        self.assertTrue(result['ok'])
        self.assertTrue(self.folder.is_symlink())
        self.assertEqual(list(external.iterdir()), [foreign])

    def test_posix_remove_ignores_foreign_contents_and_bad_marker(self):
        log, _ = self.start()
        log.close()
        (self.folder / entry.LOG_MARKER).write_bytes(b'foreign marker')
        before = {p.name: p.read_bytes() for p in self.folder.iterdir()}
        self.assertTrue(self.remove()['ok'])
        self.assertEqual({p.name: p.read_bytes() for p in self.folder.iterdir()}, before)
        (self.folder / entry.LOG_MARKER).write_bytes(entry.LOG_MAGIC)
        (self.folder / 'private.txt').write_text('foreign')
        self.assertTrue(self.remove()['ok'])
        self.assertTrue((self.folder / 'private.txt').exists())

    def test_posix_remove_ignores_redirected_file_and_retains_all_logs(self):
        log, path = self.start()
        log.close()
        external = self.home / 'private.txt'
        external.write_text('untouched')
        redirected = self.home / 'redirected'
        redirected.symlink_to(external)
        os.replace(redirected, self.folder / entry.LOG_NAMES[-1])
        self.assertTrue(self.remove()['ok'])
        self.assertTrue(path.exists())
        self.assertTrue((self.folder / entry.LOG_MARKER).exists())
        self.assertEqual(external.read_text(), 'untouched')

    @unittest.skipIf(os.name == 'nt', 'Windows reparse handles tested separately')
    def test_posix_remove_ignores_mount_point(self):
        log, _ = self.start()
        log.close()
        real = os.path.ismount
        with mock.patch.object(entry.os.path, 'ismount', side_effect=lambda p: p == str(self.folder) or real(p)):
            self.assertTrue(self.remove()['ok'])
        self.assertTrue((self.folder / entry.LOG_MARKER).exists())

    def test_posix_remove_never_unlinks_or_removes_directories(self):
        log, _ = self.start()
        log.close()
        before = {p.name: p.read_bytes() for p in self.folder.iterdir()}
        with mock.patch.object(entry.os, 'unlink') as unlink, \
                mock.patch.object(entry.os, 'rmdir') as rmdir:
            self.assertTrue(self.remove()['ok'])
            unlink.assert_not_called()
            rmdir.assert_not_called()
        self.assertEqual({p.name: p.read_bytes() for p in self.folder.iterdir()}, before)

    def test_posix_remove_leaves_other_live_instance_untouched(self):
        log, path = self.start()
        log.event('active')
        self.assertTrue(self.remove()['ok'])
        self.assertFalse(log.closed)
        self.assertEqual(path.read_text(), 'active\n')

    def test_aborted_remove_keeps_state_and_live_logs(self):
        log, _ = self.start()
        self.confirm.return_value = False
        with mock.patch.object(service.state, 'load', return_value={'installed': True}), \
                mock.patch.object(sys, 'stdout', log), mock.patch.object(sys, 'stderr', log):
            self.assertTrue(self.remove()['aborted'])
        self.wipe.assert_not_called()
        self.assertFalse(log.closed)
        self.assertTrue(self.folder.exists())

    def test_posix_state_cleanup_failure_retains_logs_and_reports_failure(self):
        log, _ = self.start()
        log.close()
        self.wipe.side_effect = lambda: ('test state', [('stock.bin', 'cannot remove')])
        result = self.remove()
        self.assertFalse(result['ok'])
        self.assertTrue(self.folder.exists())
        self.assertEqual(result['errors'], [('stock.bin', 'cannot remove')])


class WindowsRemoveIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.directory = mock.Mock()
        self.directory.owned_files.return_value = mock.Mock(), {}
        self.stack.enter_context(mock.patch.object(entry.os, 'name', 'nt'))
        self.opened = self.stack.enter_context(mock.patch.object(entry, '_LogDirectory',
                                                               return_value=self.directory))
        self.stack.enter_context(mock.patch.dict(sys.modules, msvcrt=types.SimpleNamespace(get_osfhandle=lambda fd: fd)))
        self.stack.enter_context(mock.patch.object(service.state, 'load', return_value={}))
        self.wipe = self.stack.enter_context(mock.patch.object(service.state, 'wipe_all',
                                                              return_value=('test state', [])))

    def remove(self):
        return service.do_remove({}, mock.Mock(), mock.Mock(return_value=True))

    def test_validation_failure_reports_cleanup_error_before_any_deletion(self):
        self.directory.owned_files.side_effect = OSError('ownership or identity validation failed')
        result = self.remove()
        self.assertFalse(result['ok'])
        self.assertEqual(result['errors'][0][0], entry.LOG_FOLDER)
        self.directory.win.delete.assert_not_called()
        self.directory.close.assert_called_once()

    def test_deletion_failure_closes_directory_and_reports_error(self):
        self.directory.win.delete.side_effect = OSError('cannot delete pinned handle')
        result = self.remove()
        self.assertFalse(result['ok'])
        self.assertEqual(result['errors'][0][0], entry.LOG_FOLDER)
        self.directory.close.assert_called_once()

    def test_state_failure_still_attempts_handle_cleanup(self):
        self.wipe.return_value = 'test state', [('stock.bin', 'cannot remove')]
        result = self.remove()
        self.assertFalse(result['ok'])
        self.assertEqual(result['errors'], [('stock.bin', 'cannot remove')])
        self.assertEqual(self.directory.win.delete.call_count, 2)
        self.directory.close.assert_called_once()

    def test_absent_log_directory_is_successful(self):
        self.opened.side_effect = FileNotFoundError('no logs')
        self.assertTrue(self.remove()['ok'])
        self.directory.win.delete.assert_not_called()


class ChildStreamTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.read, self.write = os.pipe()
        self.addCleanup(os.close, self.read)
        self.addCleanup(os.close, self.write)
        self.win = mock.Mock(DUPLICATE_SAME_ACCESS=2)
        self.win.GetStdHandle.return_value = self.read
        self.win.GetCurrentProcess.return_value = 123
        self.win.DuplicateHandle.side_effect = lambda *args: os.dup(args[1])
        self.win.CloseHandle.side_effect = os.close
        self.crt = mock.Mock()
        self.crt.open_osfhandle.side_effect = lambda handle, flags: handle
        self.stack.enter_context(mock.patch.dict(sys.modules, _winapi=self.win, msvcrt=self.crt))
        self.stack.enter_context(mock.patch.object(os, 'O_BINARY', 0x8000, create=True))

    def test_binary_gate_and_duplicate_ownership(self):
        def crt_open(handle, flags):
            os.set_inheritable(handle, True)
            return handle
        self.crt.open_osfhandle.side_effect = crt_open
        raw = b'G\x00\r\n\xff'
        os.write(self.write, raw)
        with entry._child_stream(-10, True) as stream:
            self.assertEqual(stream.buffer.read(len(raw)), raw)
            duplicate = stream.fileno()
            self.assertFalse(os.get_inheritable(duplicate))
            self.win.DuplicateHandle.assert_called_once_with(123, self.read, 123, 0, False, 2)
            self.crt.open_osfhandle.assert_called_once_with(duplicate, os.O_RDONLY | 0x8000)
        with self.assertRaises(OSError):
            os.fstat(duplicate)
        os.fstat(self.read)  # inherited original still belongs to Windows

    def test_output_is_line_buffered_and_uses_binary_descriptor(self):
        self.win.GetStdHandle.return_value = self.write
        with entry._child_stream(-11, False) as stream:
            stream.write('diagnostic\n')
            self.assertEqual(os.read(self.read, 11), b'diagnostic\n')
            self.assertTrue(stream.line_buffering)
            self.crt.open_osfhandle.assert_called_once_with(stream.fileno(), os.O_WRONLY | 0x8000)

    def test_missing_handles_and_conversion_failure_close_only_duplicate(self):
        for handle in (None, 0, -1, entry.ctypes.c_void_p(-1).value):
            self.win.GetStdHandle.return_value = handle
            with self.assertRaises(OSError):
                entry._child_stream(-10, True)
        self.win.DuplicateHandle.assert_not_called()
        self.win.GetStdHandle.return_value = self.read
        self.crt.open_osfhandle.side_effect = OSError('CRT failed')
        with self.assertRaises(OSError):
            entry._child_stream(-10, True)
        self.win.CloseHandle.assert_called_once()
        os.fstat(self.read)


if __name__ == '__main__':
    unittest.main()
