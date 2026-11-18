# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Acquisition supervision against fake helper processes. No network, no device."""
import contextlib
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
import types
import unittest
from unittest import mock

from diskos_installer import firmware_catalogue as fw, firmware_download as dl, firmware_fetch as ff
from diskos_installer import flash_child, gui_entry
from diskos_installer import __main__ as cli

FAKE = textwrap.dedent('''
    import json, os, subprocess, sys, time
    cfg = json.loads(sys.stdin.readline()); op = cfg["op"]; mode = os.environ["FAKE"]; d = os.environ["FAKE_DIR"]
    def send(**e): sys.stdout.write(json.dumps(e) + "\\n"); sys.stdout.flush()
    def part():
        p = os.path.join(d, "x.zip.1.part"); open(p, "wb").close(); send(op=op, ev="partial", path=p); return p
    if mode == "ok":
        part()
        for n in (0, 10, 50, 100): send(op=op, ev="progress", done=n, total=100)
        send(op=op, ev="result", ok=True, path="/cache/ok.zip")
    elif mode == "stale":
        send(op=op + 1000, ev="progress", done=5, total=100); send(op=op + 1000, ev="result", ok=False, code="E161", message="x")
        send(op=op, ev="progress", done=1, total=100); send(op=op, ev="result", ok=True, path="/cache/ok.zip")
    elif mode == "fail":
        send(op=op, ev="result", ok=False, code=os.environ["CODE"], message="boom", action="act", status=404)
    elif mode == "crash": sys.exit(3)
    elif mode == "garbage": print("not json", flush=True); time.sleep(60)
    elif mode == "chatty":
        part()
        while True: send(op=op, ev="progress", done=1, total=100); time.sleep(0.01)
    elif mode == "soft":
        p = part()
        if sys.stdin.readline().strip() == "cancel":
            os.unlink(p); open(os.path.join(d, "polite"), "w").close()
            send(op=op, ev="result", ok=False, code="E170", message="cancelled")
    elif mode == "gc":  # the leader finishes at once; a grandchild keeps the inherited stdout open
        g = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(8)"])
        open(os.path.join(d, "grandchild"), "w").write(str(g.pid))
        send(op=op, ev="result", ok=True, path="/cache/ok.zip")
    elif mode == "late":
        time.sleep(0.3); send(op=op, ev="result", ok=True, path="/cache/ok.zip")
    elif mode == "beat":  # no byte progress at all, only liveness
        for _ in range(12): send(op=op, ev="heartbeat"); time.sleep(0.1)
        send(op=op, ev="result", ok=True, path="/cache/ok.zip")
    elif mode == "foreign":  # chatter that belongs to another operation must not count as liveness
        while True: send(op=op + 1000, ev="heartbeat"); time.sleep(0.05)
    elif mode in ("silent_create", "announce"):  # a created partial; the announcement never reaches the parent (or is queued)
        p = cfg.get("partial") or os.path.join(d, "x.zip.1.part")
        open(p, "wb").write(b"data")
        if mode == "announce": send(op=op, ev="partial", path=p)
        time.sleep(60)
    else:  # stall: a blocked DNS lookup/connect that ignores the polite cancel, plus a grandchild
        part(); g = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        open(os.path.join(d, "grandchild"), "w").write(str(g.pid)); time.sleep(60)
''')


def alive(pid):
    try:
        with open('/proc/%d/stat' % pid) as f:
            return f.read().rsplit(')', 1)[1].split()[0] != 'Z'
    except OSError:
        return False


class FetchTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = tmp.name
        env = mock.patch.dict(os.environ, DISKOS_INSTALLER_HOME=os.path.join(self.root, 'state'))
        env.start()
        self.addCleanup(env.stop)
        self.dir = os.path.dirname(fw._cache_path(fw._pin('240')))
        os.environ['FAKE_DIR'] = self.dir
        self.script = os.path.join(self.root, 'fake.py')
        with open(self.script, 'w') as f:
            f.write(FAKE)
        self.lock = cli._Lock(windows=False)
        self.lock.path = os.path.join(self.root, 'run.lock')
        self.lock.__enter__()
        self.addCleanup(self.lock.__exit__, None, None, None)
        self.events = []
        self.spawned = []
        self.addCleanup(lambda: [os.killpg(p, signal.SIGKILL) for p in self.spawned if alive(p)])

    def go(self, mode, **kw):
        os.environ['FAKE'] = mode
        kw.setdefault('interval', 0)
        acq = ff.start('240', lambda *e: self.events.append(e), run_lock=self.lock,
                       argv=[sys.executable, self.script], **kw)
        self.spawned.append(acq._proc.pid)
        return acq

    def parts(self):
        return [n for n in os.listdir(self.dir) if n.endswith('.part')]

    def test_progress_then_verifying_then_success(self):
        acq = self.go('ok')
        self.assertEqual(acq.wait(5), '/cache/ok.zip')
        self.assertEqual([e[1:] for e in self.events],
                         [('progress', 0, 100), ('progress', 10, 100), ('progress', 50, 100),
                          ('progress', 100, 100), ('verifying', 100, 100)])
        self.assertTrue(all(e[0] == acq.op_id for e in self.events))
        self.assertIsNotNone(acq._proc.returncode)

    def test_progress_is_throttled_but_final_is_kept(self):
        self.go('ok', interval=60).wait(5)
        self.assertEqual([e[1:] for e in self.events],
                         [('progress', 0, 100), ('progress', 100, 100), ('verifying', 100, 100)])

    def test_typed_failures_come_from_the_helper(self):
        for code, cls in (('E165', dl.HttpStatusError), ('E169', dl.DiskFull), ('E164', dl.TlsError),
                          ('E167', dl.SizeMismatch), ('E170', dl.Cancelled)):
            os.environ['CODE'] = code
            with self.subTest(code=code), self.assertRaises(cls) as cm:
                self.go('fail').wait(5)
            self.assertEqual(cm.exception.code, code)
        os.environ['CODE'] = 'E165'
        with self.assertRaises(dl.HttpStatusError) as cm:
            self.go('fail').wait(5)
        self.assertEqual(cm.exception.status, 404)
        os.environ['CODE'] = 'E151'
        with self.assertRaises(dl.DownloadError) as cm:
            self.go('fail').wait(5)
        self.assertEqual(cm.exception.code, 'E151')

    def test_helper_crash_and_garbage_are_typed_failures(self):
        with self.assertRaises(dl.DownloadError) as cm:
            self.go('crash').wait(5)
        self.assertIn('stopped unexpectedly', str(cm.exception))
        t = time.monotonic()
        with self.assertRaises(dl.DownloadError) as cm:
            self.go('garbage').wait(5)
        self.assertIn('invalid reply', str(cm.exception))
        self.assertLess(time.monotonic() - t, 3)

    def test_stale_operation_events_are_ignored(self):
        acq = self.go('stale')
        self.assertEqual(acq.wait(5), '/cache/ok.zip')
        self.assertEqual([e[1:] for e in self.events], [('progress', 1, 100)])

    def test_cancel_during_stalled_connect_is_prompt_and_leaves_nothing(self):
        acq = self.go('stall')
        deadline = time.monotonic() + 5
        while not os.path.exists(os.path.join(self.dir, 'grandchild')) and time.monotonic() < deadline:
            time.sleep(0.02)
        with open(os.path.join(self.dir, 'grandchild')) as f:
            grandchild = int(f.read())
        self.assertIn('x.zip.1.part', self.parts())
        t = time.monotonic()
        acq.cancel()
        with self.assertRaises(dl.Cancelled):
            acq.wait(5)
        self.assertLess(time.monotonic() - t, ff.GRACE + 1)
        self.assertIsNotNone(acq._proc.poll())
        time.sleep(0.2)
        self.assertFalse(alive(grandchild))
        self.assertEqual(self.parts(), [])
        self.assertIsNone(fw.cached('240', self.lock))

    def test_polite_cancel_lets_the_helper_exit_by_itself_within_the_grace(self):
        acq = self.go('soft')
        time.sleep(0.3)
        t = time.monotonic()
        acq.cancel()
        with self.assertRaises(dl.Cancelled):
            acq.wait(5)
        self.assertLess(time.monotonic() - t, ff.GRACE)  # no wait-out of the grace period
        self.assertTrue(os.path.exists(os.path.join(self.dir, 'polite')))  # it cleaned up itself
        self.assertEqual(self.parts(), [])

    def test_no_event_is_delivered_after_cancel(self):
        acq = self.go('chatty')
        while not self.events:
            time.sleep(0.01)
        acq.cancel()
        seen = len(self.events)
        with self.assertRaises(dl.Cancelled):
            acq.wait(5)
        time.sleep(0.1)
        self.assertEqual(len(self.events), seen)
        self.assertEqual(self.parts(), [])

    def test_overall_deadline_and_silence_watchdog(self):
        with mock.patch.object(ff, 'DEADLINE_SLACK', 0.05):
            t = time.monotonic()
            with self.assertRaises(dl.DeadlineExceeded):
                self.go('stall', deadline=0.3).wait(5)
            self.assertLess(time.monotonic() - t, 3)
        with self.assertRaises(dl.Timeout):
            self.go('stall', stall=0.3).wait(5)
        self.assertEqual(self.parts(), [])

    def test_only_the_helper_is_signalled_never_a_flash_child(self):
        flash = subprocess.Popen(['sleep', '60'], start_new_session=True)
        self.addCleanup(lambda: (flash.kill(), flash.wait()))
        acq = self.go('stall')
        time.sleep(0.3)
        hit = []
        real = os.killpg
        with mock.patch.object(ff.os, 'killpg', lambda pid, sig: (hit.append(pid), real(pid, sig))):
            acq.cancel()
            with self.assertRaises(dl.Cancelled):
                acq.wait(5)
        self.assertEqual(hit, [acq._proc.pid])
        self.assertNotEqual(acq._proc.pid, os.getpgid(flash.pid))
        self.assertIsNone(flash.poll())

    def test_start_requires_a_held_run_lock(self):
        with self.assertRaises(RuntimeError):
            ff.start('240', run_lock=None)

    def test_live_helpers_are_killed_at_exit(self):
        acq = self.go('stall')
        time.sleep(0.2)
        ff._kill_all()
        self.assertIsNotNone(acq._proc.wait(5))
        acq.cancel()

    def test_exited_leader_with_a_grandchild_holding_stdout_completes_promptly(self):
        t = time.monotonic()
        acq = self.go('gc')
        self.assertEqual(acq.wait(5), '/cache/ok.zip')
        self.assertLess(time.monotonic() - t, ff.GRACE + 2)
        with open(os.path.join(self.dir, 'grandchild')) as f:
            grandchild = int(f.read())
        time.sleep(0.2)
        self.assertFalse(alive(grandchild))
        self.assertIsNotNone(acq._proc.returncode)

    def test_kill_all_racing_normal_finish_never_signals_a_reaped_leader(self):
        # _kill_all enters the kill and is held up inside killpg while the helper finishes
        # normally and the supervisor completes: nobody else may reap the leader meanwhile.
        acq, bad, real = self.go('late'), [], os.killpg

        def killpg(pid, sig):
            time.sleep(0.6)
            if acq._proc.returncode is not None:
                bad.append(pid)  # reaped under our feet: its pid could already be reused
            real(pid, sig)
        with mock.patch.object(ff.os, 'killpg', killpg):
            time.sleep(0.1)
            killer = threading.Thread(target=ff._kill_all)
            killer.start()
            try:
                acq.wait(5)
            except dl.DownloadError:
                pass  # either outcome is fine; unsafe signalling is not
            killer.join(5)
        self.assertFalse(killer.is_alive())
        self.assertEqual(bad, [])
        self.assertIsNotNone(acq._proc.returncode)

    def test_cancel_before_the_helper_announces_its_partial_removes_it(self):
        acq = self.go('silent_create')
        time.sleep(0.4)
        self.assertTrue(self.parts())
        acq.cancel()
        with self.assertRaises(dl.Cancelled):
            acq.wait(5)
        self.assertEqual(self.parts(), [])

    def test_cancel_with_the_announcement_queued_but_unconsumed_removes_the_partial(self):
        original, gate = ff.Acquisition._supervise, threading.Event()
        with mock.patch.object(ff.Acquisition, '_supervise', lambda self: (gate.wait(5), original(self))):
            acq = self.go('announce')
            end = time.monotonic() + 5
            while acq._queue.qsize() < 1 and time.monotonic() < end:
                time.sleep(0.01)
            self.assertGreaterEqual(acq._queue.qsize(), 1)  # queued, supervisor not yet reading
            acq.cancel()
            gate.set()
            with self.assertRaises(dl.Cancelled):
                acq.wait(5)
        self.assertEqual(self.parts(), [])

    def test_the_reserved_partial_is_removed_when_the_helper_cannot_start(self):
        os.environ['FAKE'] = 'ok'
        with self.assertRaises(dl.DownloadError):
            ff.start('240', run_lock=self.lock, argv=[os.path.join(self.root, 'missing-helper')])
        self.assertEqual(self.parts(), [])

    def test_heartbeats_keep_a_progress_free_acquisition_alive_past_the_silence_limit(self):
        acq = self.go('beat', stall=0.4)  # 1.2 s of quiet, three times the silence limit
        self.assertEqual(acq.wait(10), '/cache/ok.zip')
        self.assertEqual(self.events, [])

    def test_silent_and_foreign_chatty_helpers_still_trip_the_watchdog(self):
        with self.assertRaises(dl.Timeout):
            self.go('stall', stall=0.3, deadline=5).wait(5)
        with self.assertRaises(dl.Timeout):
            self.go('foreign', stall=0.3, deadline=5).wait(5)
        self.assertEqual(self.parts(), [])


class HelperTests(unittest.TestCase):
    def test_launch_arguments_are_private_and_distinct_from_the_flash_child(self):
        self.assertNotEqual(ff.MODE, flash_child.CHILD_MODE)
        self.assertEqual(gui_entry.FETCH_MODE, ff.MODE)
        with mock.patch.object(sys, 'frozen', True, create=True):
            self.assertEqual(ff.helper_argv(), [sys.executable, ff.MODE])
        with mock.patch.object(sys, 'frozen', False, create=True):
            self.assertEqual(ff.helper_argv(), [sys.executable, '-I', os.path.abspath(ff.__file__), ff.MODE])

    def test_source_launch_refuses_bad_arguments_and_config(self):
        run = lambda *a, i=b'': subprocess.run([sys.executable, '-I', ff.__file__, *a], input=i,
                                               capture_output=True, timeout=20)
        for args in ((), ('--x',), (flash_child.CHILD_MODE, 'n'), (ff.MODE, 'extra')):
            self.assertEqual(run(*args).returncode, 2)
        done = run(ff.MODE, i=b'not json\n')
        self.assertEqual((done.returncode, done.stdout), (2, b''))
        good = {'op': 1, 'version': '240', 'timeout': 15, 'deadline': 60, 'win': False}
        for field in ('timeout', 'deadline'):
            with self.subTest(oversized=field):
                cfg = dict(good, **{field: 10 ** 400})
                done = run(ff.MODE, i=(json.dumps(cfg) + '\n').encode())
                self.assertEqual((done.returncode, done.stdout), (2, b''))

    def test_helper_streams_events_borrows_the_lock_and_restores_the_hook(self):
        lock_dir = tempfile.TemporaryDirectory()
        self.addCleanup(lock_dir.cleanup)
        lock = cli._Lock(windows=False)
        lock.path = os.path.join(lock_dir.name, 'l')
        lock.__enter__()
        self.addCleanup(lock.__exit__, None, None, None)
        r, w = os.pipe()
        cfg = {'op': 7, 'version': '240', 'timeout': 15, 'deadline': 60, 'win': False,
               'lock_path': lock.path, 'lock_fd': lock.fd}
        os.write(w, (json.dumps(cfg) + '\n').encode())
        out, real = io.StringIO(), fw.new_partial
        seen = {}

        def download(version, run_lock, progress, cancel, timeout, deadline):
            with fw._locked(run_lock):
                seen['locked'] = True
            progress(1, 2)
            self.assertFalse(cancel())
            raise dl.DiskFull('Not enough space', action='free space')
        with mock.patch.object(dl, 'download', download):
            self.assertEqual(ff.helper_main([ff.MODE], stdin_fd=r, out=out), 0)
        lines = [json.loads(x) for x in out.getvalue().splitlines()]
        self.assertTrue(seen['locked'])
        self.assertEqual([x['ev'] for x in lines], ['progress', 'result'])
        self.assertEqual((lines[1]['ok'], lines[1]['code'], {x['op'] for x in lines}), (False, 'E169', {7}))
        self.assertIs(fw.new_partial, real)
        os.close(w)
        os.close(r)

    def test_parent_loss_before_new_partial_removes_the_reserved_partial(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        part = os.path.join(d.name, 'reserved.part')
        open(part, 'w').close()
        r, w = os.pipe()
        cfg = {'op': 1, 'version': '240', 'timeout': 15, 'deadline': 60, 'win': True, 'partial': part}
        os.write(w, (json.dumps(cfg) + '\n').encode())
        gate, hit, errors = threading.Event(), [], []

        def download(version, run_lock, progress, cancel, timeout, deadline):
            gate.wait(5)  # blocked in DNS/connect: new_partial() is never reached
            raise dl.Cancelled('x')

        def run():
            try:
                ff.helper_main(argv=[ff.MODE], stdin_fd=r, out=io.StringIO(),
                               exit_fn=lambda c: (hit.append(c), gate.set()))
            except BaseException as exc:
                errors.append(exc)
        with mock.patch.object(dl, 'download', download), mock.patch.object(ff, 'ORPHAN_EXIT', 0.05):
            t = threading.Thread(target=run)
            t.start()
            time.sleep(0.2)
            os.close(w)
            t.join(5)
        os.close(r)
        self.assertFalse(t.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(hit, [3])
        self.assertFalse(os.path.exists(part))

    def test_parent_loss_cancels_then_exits_a_blocked_helper_and_removes_its_partial(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        part = os.path.join(d.name, 'a.part')
        open(part, 'w').close()
        r, w = os.pipe()
        cfg = {'op': 1, 'version': '240', 'timeout': 15, 'deadline': 60, 'win': True}
        os.write(w, (json.dumps(cfg) + '\n').encode())
        gate, hit, errors, cancelled = threading.Event(), [], [], []

        def download(version, run_lock, progress, cancel, timeout, deadline):
            fw.new_partial('240')  # the helper's tracking wrapper, around the fake creator
            if not gate.wait(5):
                raise AssertionError('orphan watchdog did not fire')
            cancelled.append(cancel())
            raise dl.Cancelled('x')

        def run():
            try:
                ff.helper_main(argv=[ff.MODE], stdin_fd=r, out=io.StringIO(),
                               exit_fn=lambda c: (hit.append(c), gate.set()))
            except BaseException as exc:  # an assertion inside the download must not pass unnoticed
                errors.append(exc)
        with mock.patch.object(fw, 'new_partial', return_value=part), \
                mock.patch.object(dl, 'download', download), mock.patch.object(ff, 'ORPHAN_EXIT', 0.05):
            t = threading.Thread(target=run)
            t.start()
            time.sleep(0.2)
            os.close(w)
            t.join(5)
        os.close(r)
        self.assertFalse(t.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(cancelled, [True])
        self.assertEqual(hit, [3])
        self.assertFalse(os.path.exists(part))

    def _run_helper(self, download, timeout, extra=None):
        r, w = os.pipe()
        cfg = dict({'op': 4, 'version': '240', 'timeout': timeout, 'deadline': 60, 'win': True}, **(extra or {}))
        os.write(w, (json.dumps(cfg) + '\n').encode())
        out = io.StringIO()
        try:
            with mock.patch.object(dl, 'download', download):
                self.assertEqual(ff.helper_main([ff.MODE], stdin_fd=r, out=out), 0)
        finally:
            os.close(w)
            os.close(r)
        return [json.loads(x) for x in out.getvalue().splitlines()]

    def test_helper_sends_heartbeats_while_a_download_makes_no_progress(self):
        def download(version, run_lock, progress, cancel, timeout, deadline):
            time.sleep(0.7)  # several heartbeat periods (timeout / 2 = 0.1 s) with no progress
            return '/cache/ok.zip'
        lines = self._run_helper(download, 0.2)
        beats = [x for x in lines if x['ev'] == 'heartbeat']
        self.assertGreaterEqual(len(beats), 3)
        self.assertEqual({x['op'] for x in lines}, {4})
        self.assertEqual(lines[-1]['ev'], 'result')
        self.assertEqual([x['ev'] for x in lines if x['ev'] != 'heartbeat'], ['result'])

    def test_helper_uses_the_partial_the_parent_reserved(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        reserved = os.path.join(d.name, 'r.part')
        made = []

        def download(version, run_lock, progress, cancel, timeout, deadline):
            with mock.patch.object(fw, '_locked'):
                made.append(fw.new_partial(version, run_lock))
            return made[0]
        with mock.patch.object(fw, 'new_partial', side_effect=AssertionError('second partial')):
            lines = self._run_helper(download, 15, {'partial': reserved})
        self.assertEqual(made, [reserved])
        self.assertEqual([(x['ev'], x.get('path')) for x in lines], [('partial', reserved), ('result', reserved)])


class DispatchTests(unittest.TestCase):
    def _dispatch(self, name, pipe_error=None):
        streams = mock.Mock(side_effect=pipe_error or (lambda *a: io.StringIO()))
        fake_os = types.SimpleNamespace(name=name)
        with mock.patch.object(gui_entry, '_run_fetch_helper', return_value=0) as fetch, \
                mock.patch.object(gui_entry, '_run_child', return_value=5) as child, \
                mock.patch.object(gui_entry, '_run_installer') as gui, \
                mock.patch.object(gui_entry, '_open_log') as log, \
                mock.patch.object(gui_entry, '_show_error') as shown, \
                mock.patch.object(gui_entry, '_child_stream', streams), \
                mock.patch.object(gui_entry, 'os', fake_os), \
                contextlib.redirect_stderr(io.StringIO()):
            result = gui_entry.main([ff.MODE])
        child.assert_not_called(); gui.assert_not_called(); log.assert_not_called(); shown.assert_not_called()
        return result, fetch, streams

    def test_frozen_windowed_dispatch_selects_helper_only_for_its_argument(self):
        result, fetch, streams = self._dispatch('posix')
        self.assertEqual(result, 0)
        fetch.assert_called_once_with([ff.MODE])
        self.assertEqual(streams.call_count, 0)  # POSIX inherits real pipes
        result, fetch, streams = self._dispatch('nt')
        self.assertEqual(result, 0)
        fetch.assert_called_once_with([ff.MODE])
        streams.assert_has_calls([mock.call(-10, True), mock.call(-11, False), mock.call(-12, False)])
        self.assertEqual(streams.call_count, 3)

    def test_windowed_dispatch_without_pipes_fails_before_any_gui_or_log(self):
        result, fetch, streams = self._dispatch('nt', pipe_error=OSError('no pipe'))
        self.assertEqual(result, 1)
        fetch.assert_not_called()
        self.assertEqual(streams.call_count, 1)

    def test_flash_child_argument_never_reaches_the_helper(self):
        with mock.patch.object(gui_entry, '_run_fetch_helper') as fetch, \
                mock.patch.object(gui_entry, '_run_child', return_value=5) as child, \
                mock.patch.object(gui_entry, '_child_stream', return_value=io.StringIO()), \
                mock.patch.object(gui_entry, '_run_installer') as gui:
            self.assertEqual(gui_entry.main([flash_child.CHILD_MODE, 'n']), 5)
            child.assert_called_once()
            fetch.assert_not_called(); gui.assert_not_called()

    def test_cli_dispatch_has_no_gui_lock_or_flash_entry(self):
        with mock.patch.object(ff, 'helper_main', return_value=4) as helper, \
                mock.patch.object(flash_child, 'main') as flash, \
                mock.patch.object(cli, 'build_parser') as parser, mock.patch.object(cli, '_Lock') as lock:
            self.assertEqual(cli.main([ff.MODE]), 4)
            helper.assert_called_once_with([ff.MODE])
            flash.assert_not_called(); parser.assert_not_called(); lock.assert_not_called()
        with mock.patch.object(ff, 'helper_main') as helper, mock.patch.object(flash_child, 'main', return_value=2):
            self.assertEqual(cli.main([flash_child.CHILD_MODE, 'n']), 2)
            helper.assert_not_called()


if __name__ == '__main__':
    unittest.main()
