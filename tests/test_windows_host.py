# SPDX-License-Identifier: GPL-3.0-or-later
"""Host services only: no USB, image build or flash operations."""
import ctypes
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from diskos_installer import bundle, flash_child, flasher, platform_probe, state
from diskos_installer.errors import RECOVERABLE
from diskos_installer.__main__ import _Lock
from diskos_installer.reporter import QueueReporter
import queue

class API:
    def __init__(self, wait=0, assign=True, sleep=1):
        self.events, self.result, self.assign, self.sleep = [], wait, assign, sleep
    def mutex_security(self): return ctypes.c_int(0)
    def CreateMutexExW(self, sa, name, flags, access):
        self.events.append((name, access)); return 10
    def WaitForSingleObject(self, handle, ms): return self.result
    def ReleaseMutex(self, handle): self.events.append('release'); return 1
    def CloseHandle(self, handle): self.events.append(('close', handle)); return 1
    def CreateJobObjectW(self, sa, name):
        self.events.append(("job", sa, name)); return 11
    def SetHandleInformation(self, handle, mask, flags):
        self.events.append(("inherit", handle, mask, flags)); return 1
    def ResumeThread(self, thread): self.events.append("resume"); return 1
    def SetInformationJobObject(self, job, kind, ptr, size):
        self.events.append(('limit', kind, ctypes.cast(ptr, ctypes.POINTER(ctypes.c_uint32))[4])); return 1
    def AssignProcessToJobObject(self, job, proc): self.events.append('assign'); return self.assign
    def TerminateJobObject(self, job, code): self.events.append('terminate'); return 1
    def SetThreadExecutionState(self, flags): self.events.append(flags); return self.sleep

class Process:
    pid, _handle, _thread = 456, 789, 790
    def __init__(self, events):
        self.events = events
        self.stdin = self
    def write(self, data): self.events.append('gate')
    def close(self): self.events.append('pipe-close')
    def close_handles(self): self.events.append("process-close")
    def kill(self): self.events.append('kill')
    def wait(self, timeout=None): self.events.append('wait'); return 0

def pe(imports):
    data = bytearray(1024)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<H', data, 134, 1)
    struct.pack_into('<H', data, 148, 240)
    struct.pack_into('<H', data, 152, 0x20b)
    struct.pack_into('<II', data, 272, 0x1000, 100)
    struct.pack_into('<4I', data, 400, 512, 0x1000, 512, 512)
    pos = 700
    for i, name in enumerate(imports):
        raw = name.encode() + b'\0'
        struct.pack_into('<I', data, 512 + i * 20 + 12, 0x1000 + pos - 512)
        data[pos:pos + len(raw)] = raw
        pos += len(raw)
    return data

class WindowsHostTests(unittest.TestCase):
    def test_mutex_contention_abandonment_and_release(self):
        for result in (0, 0x80, 258, 0xffffffff):
            api = API(wait=result)
            lock = _Lock(windows=True, api=api)
            if result in (0, 0x80):
                with lock: pass
                self.assertIn('release', api.events)
            else:
                with self.assertRaises(SystemExit): lock.__enter__()
                self.assertNotIn('release', api.events)
            self.assertEqual(api.events[0], ('Global\\diskos-installer', 0x100001))
            self.assertIn(('close', 10), api.events)
        self.assertEqual(_Lock(windows=False).path, '/tmp/diskos-installer.lock')
    def test_job_orders_assignment_before_gate_and_cleans_failures(self):
        with mock.patch.object(flasher, '_WINDOWS_DEV_FLASH', mock.Mock(get=mock.Mock(return_value=True))), \
                mock.patch.dict(os.environ, DISKOS_WINDOWS_DEV_FLASH='1'), \
                mock.patch.object(platform_probe, 'windows_dev_runtime', return_value=True):
            for assign in (True, False):
                api = API(assign=assign)
                def start(cmd, **kw):
                    self.assertEqual(kw['creationflags'], 0x204)
                    api.events.append('spawn'); return Process(api.events)
                if assign:
                    child = flasher._WindowsChild([os.path.abspath('usbboot.exe'), '--help'], api=api, popen=start)
                    self.assertLess(api.events.index('assign'), api.events.index('gate'))
                    flasher._kill_child(child)
                    child.close()
                    self.assertIn('terminate', api.events)
                else:
                    with self.assertRaises(OSError):
                        flasher._WindowsChild([os.path.abspath('usbboot.exe'), '--help'], api=api, popen=start)
                    self.assertNotIn('gate', api.events)
                    self.assertIn('kill', api.events)
                self.assertIn(('limit', 9, 0x2000), api.events)
                self.assertIn(('close', 11), api.events)
    def test_sleep_release_on_exception_and_refusal_on_failure(self):
        api = API()
        with self.assertRaises(ValueError):
            with flasher._sleep_inhibited(host='windows', api=api): raise ValueError()
        self.assertEqual(api.events, [0x80000001, 0x80000000])
        q = queue.Queue()
        with self.assertRaises(flasher.FlashError) as caught:
            with flasher._sleep_inhibited(QueueReporter(q), host='windows', api=API(sleep=0)): pass
        self.assertEqual(caught.exception.code, 'E116')
        self.assertTrue(q.empty())
    def test_native_names_dll_closure_state_and_refusal(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(platform_probe, 'host', return_value=('windows', 'x86_64')):
            root = Path(tmp)
            tools = root / 'vendor' / 'windows-x86_64'
            tools.mkdir(parents=True)
            exe = tools / 'mksquashfs.exe'
            exe.write_bytes(pe(['KERNEL32.dll', 'msys-2.0.dll']))
            with mock.patch.object(bundle, 'resource_root', return_value=tmp):
                self.assertEqual(bundle.native('mksquashfs'), str(exe))
                with self.assertRaises(FileNotFoundError): bundle.native_env(str(exe))
                (tools / 'msys-2.0.dll').write_bytes(pe(['msys-z.dll']))
                with self.assertRaises(FileNotFoundError): bundle.native_env(str(exe))
                (tools / 'msys-z.dll').write_bytes(pe(['KERNEL32.dll']))
                self.assertEqual(bundle.native_env(str(exe))['PATH'], str(tools))
                self.assertIsNone(bundle.native('unsquashfs', required=False))
            with mock.patch.object(state.sys, 'platform', 'win32'), mock.patch.dict(os.environ, {'LOCALAPPDATA': tmp, 'DISKOS_INSTALLER_HOME': ''}):
                self.assertEqual(state._state_base(), str(root / state.APP))
                state.save({'phase': 'prepared'})
                self.assertEqual(state.load()['phase'], 'prepared')
            self.assertFalse(platform_probe.is_supported())
            self.assertEqual(platform_probe.host_status(), 'host services ready, flashing not yet supported')
            with self.assertRaises(flasher.FlashError) as caught: flasher.flash('absent', 257)
            self.assertTrue(caught.exception.pre_launch)
            self.assertEqual(caught.exception.code, 'E101')
    def test_os_shadows_and_private_closure(self):
        for name in ('KERNEL32.dll', 'ntdll.dll'):
            for imported in (True, False):
                with self.subTest(name=name, imported=imported), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    exe = root / 'tool.exe'
                    exe.write_bytes(pe([name] if imported else []))
                    (root / name).write_bytes(pe(['missing-private.dll']))
                    with self.assertRaisesRegex(ValueError, 'shadows an OS name'):
                        bundle._verify_windows_dlls(exe)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exe = root / 'tool.exe'
            exe.write_bytes(pe(['Private.dll']))
            (root / 'PRIVATE.DLL').write_bytes(pe(['leaf.dll']))
            with self.assertRaises(FileNotFoundError): bundle._verify_windows_dlls(exe)
            (root / 'leaf.dll').write_bytes(pe(['private.dll']))
            bundle._verify_windows_dlls(exe)  # Mixed case and a transitive cycle.
            for rva, size in ((0x1000, 32), (0x1000, 0), (0, 32), (0, 0)):
                with self.subTest(delay_rva=rva, delay_size=size):
                    delayed = pe([])
                    struct.pack_into('<II', delayed, 264 + 13 * 8, rva, size)
                    (root / 'leaf.dll').write_bytes(delayed)
                    if rva or size:
                        with self.assertRaisesRegex(ValueError, 'delay imports'):
                            bundle._verify_windows_dlls(exe)
                    else:
                        bundle._verify_windows_dlls(exe)

    def test_closed_os_allowlist_and_api_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exe = root / 'tool.exe'
            for name in ('KERNEL32.dll', 'ntdll.dll'):
                exe.write_bytes(pe([name]))
                bundle._verify_windows_dlls(exe)
            for name in ('api-ms-win-diskos-review-fake-l1-1-0.dll',
                         'api-ms-win-core-file-l1-1-0.dll',
                         'ext-ms-win-ntuser-window-l1-1-0.dll',
                         'api-ms-win-fake.dll', 'api-ms-win-core-file-l1-1.dll',
                         'api-ms-win-core-file-l1-1-0.dll.extra',
                         'ext-ms-win-', 'ext-ms-win-foo-lx-1-0.dll',
                         'version.dll', 'ucrtbase.dll', 'reserved.dll', 'other.dll'):
                with self.subTest(name=name):
                    exe.write_bytes(pe([name]))
                    with self.assertRaises(FileNotFoundError): bundle._verify_windows_dlls(exe)
                    if not name.endswith('.dll'):
                        continue
                    # Unknown names, including API-set syntax, require private closure.
                    (root / name).write_bytes(pe(['missing-private.dll']))
                    with self.assertRaises(FileNotFoundError): bundle._verify_windows_dlls(exe)
                    (root / name).write_bytes(pe(['KERNEL32.dll']))
                    bundle._verify_windows_dlls(exe)

    def test_real_tool_import_graph(self):
        # Import graph recorded from the built Windows squashfs tools.
        runtime_names = ('msys-2.0.dll', 'msys-gcc_s-seh-1.dll',
                         'msys-lzo2-2.dll', 'msys-z.dll')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in runtime_names:
                imports = ['KERNEL32.dll', 'ntdll.dll' if name == 'msys-2.0.dll' else 'msys-2.0.dll']
                (root / name).write_bytes(pe(imports))
            for name in ('mksquashfs.exe', 'unsquashfs.exe'):
                with self.subTest(tool=name):
                    exe = root / name
                    exe.write_bytes(pe(['KERNEL32.dll', *runtime_names]))
                    # An explicit RVA 0 / size 0 entry is not a delay import.
                    bundle._verify_windows_dlls(exe)

    def test_ucrt_exemptions_are_usbboot_only(self):
        parts = ('convert', 'environment', 'filesystem', 'heap', 'locale', 'math', 'private',
                 'runtime', 'stdio', 'string', 'time', 'utility')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            usb = root / 'usbboot.exe'
            other = root / 'mksquashfs.exe'
            private = root / 'private.dll'
            for part in parts:
                name = 'api-ms-win-crt-' + part + '-l1-1-0.dll'
                with self.subTest(contract=name):
                    usb.write_bytes(pe(['KERNEL32.dll', name]))
                    bundle._verify_windows_dlls(usb)
                    for filename in ('mksquashfs.exe', 'unsquashfs.exe', 'other.exe'):
                        tool = root / filename
                        tool.write_bytes(pe([name]))
                        with self.assertRaises(FileNotFoundError): bundle._verify_windows_dlls(tool)
                    usb.write_bytes(pe(['private.dll']))
                    private.write_bytes(pe([name]))
                    with self.assertRaises(FileNotFoundError): bundle._verify_windows_dlls(usb)
                    (root / name).write_bytes(pe([]))
                    for tool in (usb, other):
                        with self.assertRaisesRegex(ValueError, 'shadows an OS name'):
                            bundle._verify_windows_dlls(tool)
                    (root / name).unlink()
            for name in ('api-ms-win-crt-fake-l1-1-0.dll',
                         'api-ms-win-crt-runtime-l1-2-0.dll', 'ucrtbase.dll', 'libusb-1.0.dll'):
                usb.write_bytes(pe([name]))
                with self.subTest(unknown=name), self.assertRaises(FileNotFoundError):
                    bundle._verify_windows_dlls(usb)
        with mock.patch.object(platform_probe, 'host', return_value=('windows', 'x86_64')):
            self.assertEqual(bundle.native_build_script('usbboot'), 'build/build-windows-tools.sh')
            self.assertEqual(bundle.native_build_script('squashfs'), 'build/build-windows-tools.sh')

    @unittest.skipUnless(sys.platform == 'win32', 'real Windows loader required')
    def test_real_loader_adjacent_transitive_dll_wins(self):
        # Set DISKOS_WINDOWS_GCC to the MSYS2 ucrt64 gcc.exe absolute path.
        compiler = os.environ.get('DISKOS_WINDOWS_GCC')
        if not compiler:
            self.skipTest('set DISKOS_WINDOWS_GCC to build real PE loader fixtures')
        if not Path(compiler).is_file():
            self.fail('DISKOS_WINDOWS_GCC does not name a compiler file')

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tools, conflict = root / 'tools', root / 'conflict'
            tools.mkdir()
            conflict.mkdir()
            leaf = root / 'leaf.c'
            bridge = root / 'bridge.c'
            main = root / 'main.c'

            dll_entry = (
                '#include <windows.h>\n'
                'BOOL WINAPI fixture_dll_entry(HINSTANCE module, '
                'DWORD reason, LPVOID reserved){return TRUE;}\n'
            )
            leaf.write_text(
                dll_entry +
                '__declspec(dllexport) int fixture_leaf(void){return VALUE;}\n',
                encoding='ascii',
            )
            bridge.write_text(
                dll_entry +
                '__declspec(dllimport) int fixture_leaf(void);\n'
                '__declspec(dllexport) int fixture_bridge(void){'
                'return fixture_leaf();}\n',
                encoding='ascii',
            )
            main.write_text(
                '#include <windows.h>\n'
                '__declspec(dllimport) int fixture_bridge(void);\n'
                'void fixture_entry(void){'
                'ExitProcess((UINT)fixture_bridge());}\n',
                encoding='ascii',
            )

            def gcc(*args):
                subprocess.run(
                    [compiler, '-nostdlib', '-nostartfiles',
                     '-fno-stack-protector', *map(str, args)],
                    check=True, capture_output=True, timeout=60,
                )

            dll_flags = ('-shared', '-Wl,--entry,fixture_dll_entry')
            gcc(*dll_flags, '-DVALUE=42', leaf,
                '-o', tools / 'fixture_leaf.dll',
                '-Wl,--out-implib,' + str(root / 'leaf.a'))
            gcc(*dll_flags, '-DVALUE=7', leaf,
                '-o', conflict / 'fixture_leaf.dll')
            gcc(*dll_flags, bridge, root / 'leaf.a',
                '-o', tools / 'fixture_bridge.dll',
                '-Wl,--out-implib,' + str(root / 'bridge.a'))
            exe = tools / 'fixture.exe'
            gcc(main, root / 'bridge.a', '-Wl,--entry,fixture_entry',
                '-o', exe, '-lkernel32')

            env = dict(
                os.environ,
                PATH=str(conflict) + os.pathsep + os.environ.get('PATH', ''),
            )

            def run(environment):
                return subprocess.run(
                    [str(exe)], cwd=conflict, env=environment,
                    capture_output=True, timeout=10,
                ).returncode

            bundle._verify_windows_dlls(exe)
            self.assertEqual(run(env), 42)
            clean = bundle.native_env(str(exe))
            self.assertEqual(run(clean), 42)

            # Preserve the file while removing its imported filename.
            (tools / 'fixture_leaf.dll').rename(tools / 'fixture_leaf.saved')
            with self.assertRaises(FileNotFoundError):
                bundle._verify_windows_dlls(exe)
            self.assertEqual(run(env), 7)

    @unittest.skipUnless(sys.platform == 'win32', 'real Windows kernel required')
    def test_real_mutex_owner_death(self):
        script = 'from diskos_installer.__main__ import _Lock; import time; lock=_Lock(); lock.__enter__(); print("ready",flush=True); time.sleep(60)'
        proc = subprocess.Popen([sys.executable, '-c', script], stdout=subprocess.PIPE, text=True,
                                env=dict(os.environ, DISKOS_WINDOWS_DEV_FLASH='1',
                                         PYTHONPATH=str(Path(__file__).resolve().parents[1])))
        api = bundle._windows_api()
        observer = None
        results = []
        class ObservedAPI:
            def __getattr__(self, name): return getattr(api, name)
            def WaitForSingleObject(self, handle, ms):
                result = api.WaitForSingleObject(handle, ms)
                results.append(result)
                return result
        try:
            self.assertEqual(proc.stdout.readline().strip(), 'ready')
            observer = api.CreateMutexExW(None, "Global\\diskos-installer", 0, 0x100001)
            self.assertTrue(observer)
            self.assertEqual(api.WaitForSingleObject(observer, 0), 258)
            with self.assertRaises(SystemExit):
                with _Lock(): pass
            proc.kill(); proc.wait(timeout=10)
            # The observer keeps the kernel object alive through owner death.
            with _Lock(windows=True, api=ObservedAPI()):
                self.assertEqual(results, [0x80])  # Raw WAIT_ABANDONED, handled by _Lock.
            self.assertEqual(api.WaitForSingleObject(observer, 0), 0)
            self.assertTrue(api.ReleaseMutex(observer))
        finally:
            if proc.poll() is None: proc.kill(); proc.wait(timeout=10)
            proc.stdout.close()
            if observer: api.CloseHandle(observer)
    @unittest.skipUnless(sys.platform == 'win32', 'real Windows kernel required')
    def test_real_job_parent_death_and_sleep(self):
        tmp = tempfile.TemporaryDirectory(prefix='job space-\u00e9-')
        self.addCleanup(tmp.cleanup)
        launcher = Path(tmp.name) / 'harmless.py'
        launcher.write_text('import json,os,subprocess,sys,time\n'
                            'from diskos_installer import flash_child\n'
                            'assert sys.stdin.buffer.read(1)==b"G"\n'
                            'request=json.loads(sys.stdin.buffer.read())\n'
                            'flash_child.verify_supervision(request["parent"])\n'
                            'p=subprocess.Popen([sys.executable,"-c",'
                            '"import os,time;print(os.getpid(),flush=True);time.sleep(60)"])\n'
                            'print(os.getpid(),flush=True)\n'
                            'time.sleep(60)\n', encoding='ascii')
        script = ('from diskos_installer import flasher,flash_child,platform_probe; '
                  'import time; '
                  'platform_probe.windows_dev_runtime=lambda: True; '
                  'flash_child.launch_argv=lambda nonce: [' + repr(sys.executable) + ',' + repr(str(launcher)) + ']; '
                  'flasher._WINDOWS_DEV_FLASH.set(True); '
                  'p=flasher._WindowsChild([' + repr(str(Path(tmp.name) / 'usbboot.exe')) + ',"--help"]); '
                  'time.sleep(60)')
        parent = subprocess.Popen([sys.executable, '-c', script], stdout=subprocess.PIPE, text=True,
                                env=dict(os.environ, DISKOS_WINDOWS_DEV_FLASH='1',
                                         PYTHONPATH=str(Path(__file__).resolve().parents[1])))
        api = bundle._windows_api()
        api.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        api.OpenProcess.restype = ctypes.c_void_p
        handles = []
        try:
            while len(handles) < 2:
                line = parent.stdout.readline().strip()
                if not line:
                    self.fail('supervised child exited before printing both PIDs')
                if not line.isdecimal():
                    continue  # Private entry also writes supervision details to the log.
                handle = api.OpenProcess(0x100000, False, int(line))
                self.assertTrue(handle)
                handles.append(handle)
            parent.kill(); parent.wait(timeout=10)
            for handle in handles: self.assertEqual(api.WaitForSingleObject(handle, 10000), 0)
            with flasher._sleep_inhibited() as held: self.assertTrue(held.windows_active)
            self.assertFalse(held.windows_active)
        finally:
            if parent.poll() is None: parent.kill(); parent.wait(timeout=10)
            parent.stdout.close()
            for handle in handles: api.CloseHandle(handle)

if __name__ == '__main__':
    unittest.main()

class FlashRecoveryTests(unittest.TestCase):
    def run_result(self, raw, failure=None, report_failure=None):
        from contextlib import ExitStack
        class Child:
            def wait(self, timeout):
                if failure:
                    raise failure
                return 1
        with tempfile.TemporaryDirectory() as tmp, ExitStack() as stack:
            image = Path(tmp) / 'image'
            image.write_bytes(b'hsqs')
            def start(cmd, **kwargs):
                if raw is not None:
                    Path(cmd[-1]).write_bytes(raw)
                return Child()
            for name, value in [('preflight', None), ('_writer_descriptor', (768, 1)),
                                ('_start_child', start), ('_kill_child', None)]:
                stack.enter_context(mock.patch.object(flasher, name, side_effect=value if callable(value) else None,
                                                      return_value=value))
            stack.enter_context(mock.patch.object(bundle, 'native', return_value='unused'))
            stack.enter_context(mock.patch.object(bundle, 'native_env', return_value={}))
            stack.enter_context(mock.patch.object(flasher.basegate, 'build_plan', return_value=b'plan'))
            stack.enter_context(mock.patch.object(flasher, '_sleep_inhibited'))
            q = queue.Queue()
            rep = QueueReporter(q)
            original_log = rep.log
            def log(message):
                if report_failure and message.startswith(report_failure):
                    raise ValueError('reporting failed')
                original_log(message)
            stack.enter_context(mock.patch.object(rep, 'log', side_effect=log))
            with self.assertRaises((flasher.FlashError, KeyboardInterrupt)) as caught:
                flasher.flash(str(image), next(iter(flasher.basegate.CATALOGUE)), rep=rep)
            return caught.exception, list(q.queue)

    def terminal_report(self, result=0x600DF10C, capacity=768, gate=1):
        raw = bytearray(1024)
        for index, value in ((0, 0x4006E006), (6, capacity), (9, 0x55555555),
                             (16, result), (flasher.basegate.G_RESULT, gate)):
            struct.pack_into('<I', raw, index * 4, value)
        return raw

    def test_completed_aborts_are_recoverable(self):
        for result in range(0xDEAD0001, 0xDEAD0007):
            with self.subTest(result=hex(result)):
                error, _ = self.run_result(self.terminal_report(result=result))
                self.assertEqual(error.code, 'E310')
                self.assertEqual(error.action, RECOVERABLE)

    def test_completed_capacity_mismatch_is_recoverable(self):
        error, _ = self.run_result(self.terminal_report(capacity=580))
        self.assertEqual(error.code, 'E311')
        self.assertEqual(error.action, RECOVERABLE)

    def test_exception_after_validated_completion_is_recoverable(self):
        for result in (0x600DF10C, 0xDEAD0003):
            with self.subTest(result=hex(result)):
                error, _ = self.run_result(self.terminal_report(result=result), report_failure='scan:')
                self.assertEqual(error.action, RECOVERABLE)
                self.assertNotIn('UNKNOWN', error.message)
                self.assertIsInstance(error.__cause__, ValueError)

    def test_exception_before_validation_waits(self):
        error, _ = self.run_result(self.terminal_report(), report_failure='usbboot exit=')
        self.assertEqual(error.code, 'E301')
        self.assertEqual(error.action, flasher.WAIT_THEN_RECOVER)
        self.assertIsInstance(error.__cause__, ValueError)

    def test_unvalidated_terminal_reports_wait(self):
        for result, gate, erasing, code in ((0xDEAD0003, 0, 0, 'E310'),
                                          (0xDEAD0003, 0xDEAD0008, 0, 'E310'),
                                          (0x12345678, 1, 0, 'E310'),
                                          (0xDEAD0008, 0xDEAD0008, 1, 'E321')):
            with self.subTest(result=hex(result), gate=hex(gate)):
                raw = self.terminal_report(result=result, gate=gate)
                struct.pack_into('<I', raw, flasher.basegate.G_ERASING * 4, erasing)
                error, _ = self.run_result(raw)
                self.assertEqual(error.code, code)
                self.assertEqual(error.action, flasher.WAIT_THEN_RECOVER)

    def test_incomplete_results_wait_before_power_off(self):
        for raw in (None, b'short', bytes(1024)):
            with self.subTest(raw_length=None if raw is None else len(raw)):
                error, _ = self.run_result(raw)
                self.assertEqual(error.action, flasher.WAIT_THEN_RECOVER)
                self.assertNotIn('nothing written', str(error))

    def test_unvalidated_refusal_is_unknown(self):
        raw = bytearray(1024)
        for index, value in ((0, 0x4006E006), (9, 0x55555555), (16, 0xDEAD0008), (128, 0)):
            struct.pack_into('<I', raw, index * 4, value)
        error, _ = self.run_result(raw)
        self.assertNotIsInstance(error, flasher.BaseRefused)
        self.assertEqual(error.action, flasher.WAIT_THEN_RECOVER)

    def test_timeout_and_interrupt_keep_wait_guidance(self):
        for failure in (subprocess.TimeoutExpired('usbboot', 1), KeyboardInterrupt()):
            error, events = self.run_result(None, failure)
            if isinstance(error, flasher.FlashError):
                self.assertEqual(error.action, flasher.WAIT_THEN_RECOVER)
            else:
                self.assertTrue(any(flasher.WAIT_THEN_RECOVER in str(e) for e in events))
