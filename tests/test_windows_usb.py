# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only inventory and raw diagnostic contracts, without Windows or USB."""
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from diskos_installer import platform_probe, windows_usb as usb


def row(instance=r'USB\VID_A108&PID_EAEF\DISC', problem=0, service='WINUSB'):
    return dict(instance_id=instance, problem_code=problem, service=service,
                inf='winusb.inf', provider='Microsoft', location_paths=['PCIROOT(0)#USBROOT(0)#USB(1)'])


class WindowsUSBTests(unittest.TestCase):
    def inventory(self, present, phantom=()):
        api = mock.Mock()
        api.devices.side_effect = [present + list(phantom), present]
        result = usb.inventory(api)
        self.assertEqual(api.devices.call_args_list, [mock.call(False), mock.call(True)])
        return result

    def test_absent_phantom_and_wrong_ids(self):
        for rows, state in (([], 'absent'), ([row()], 'phantom'),
                            ([row(r'USB\VID_A108&PID_EAEF&MI_00\OTHER')], 'absent'),
                            ([row(r'USB\VID_A108&PID_EAE0\OTHER')], 'absent')):
            result = self.inventory([], rows)
            self.assertEqual((result['state'], result['count']), (state, 0))
            with self.assertRaises(usb.PreflightError) as error:
                usb.require_target(result)
            self.assertEqual(error.exception.code, 'E110')

    def test_bound_driver_problem_and_ambiguity(self):
        target = row()
        result = self.inventory([target], [row(r'USB\VID_A108&PID_EAEF\OLD')])
        self.assertEqual(result['state'], 'winusb')
        self.assertEqual(usb.require_target(result), target)
        for problem, service in ((28, None), (0, 'usbser'), (10, 'WINUSB')):
            result = self.inventory([row(problem=problem, service=service)])
            with self.assertRaises(usb.PreflightError) as error:
                usb.require_target(result)
            self.assertEqual(error.exception.code, 'E114')
            self.assertIn(str(problem), str(error.exception))
            self.assertIn('WinUSB', error.exception.action)
        result = self.inventory([target, row(r'USB\VID_A108&PID_EAEF\SECOND')])
        self.assertEqual(result['state'], 'ambiguous')
        with self.assertRaises(usb.PreflightError) as error:
            usb.require_target(result)
        self.assertEqual(error.exception.code, 'E111')

    def test_windows_probe_never_uses_pyusb(self):
        with mock.patch.object(platform_probe, 'host', return_value=('windows', 'x86_64')), \
                mock.patch.object(usb, 'inventory', return_value=self.inventory([row(problem=28, service=None)])), \
                mock.patch.object(platform_probe, '_maskrom_count_pyusb', side_effect=AssertionError):
            self.assertEqual(platform_probe.maskrom_count(), 1)
            self.assertIn('E114', platform_probe.maskrom_access()[1])
        with mock.patch.object(platform_probe, 'host', return_value=('windows', 'x86_64')), \
                mock.patch.object(usb, 'inventory', side_effect=OSError('enumeration failed')):
            self.assertEqual(platform_probe.maskrom_count(), -1)
            self.assertEqual(platform_probe.maskrom_access()[0], 'unknown')

    def test_diagnostic_raw_short_and_errors(self):
        good = dict(schema=1, status='raw', count=1, error=0, length=5, hex='0102030405')
        runner = mock.Mock(return_value=subprocess.CompletedProcess([], 0, json.dumps(good)))
        self.assertEqual(usb.diagnostic('usbboot.exe', runner), good)
        self.assertEqual(runner.call_args.args[0], ['usbboot.exe', '--native-diagnostic'])
        for update in ({'length': 8}, {'hex': 'zz'}, {'length': True}, {'schema': 2},
                       {'status': 'claim', 'error': -6}, {'status': 'open', 'error': -3},
                       {'count': 2}, {'error': -1}):
            runner.return_value = subprocess.CompletedProcess([], 0, json.dumps(good | update))
            with self.subTest(update=update), self.assertRaises(ValueError):
                usb.diagnostic('usbboot.exe', runner)
        runner.return_value = subprocess.CompletedProcess([], 1, json.dumps(good))
        with self.assertRaises(ValueError): usb.diagnostic('usbboot.exe', runner)
        runner.return_value = subprocess.CompletedProcess([], 0, 'not JSON')
        with self.assertRaises(ValueError): usb.diagnostic('usbboot.exe', runner)

    def test_setupapi_properties_and_handle_cleanup(self):
        api = usb.SetupAPI.__new__(usb.SetupAPI)
        api.setup, api.cfg = mock.Mock(), mock.Mock()
        api.setup.SetupDiGetClassDevsW.return_value = 99
        api.setup.SetupDiEnumDeviceInfo.return_value = False
        with mock.patch.object(usb.C, 'get_last_error', return_value=259, create=True):
            self.assertEqual(api.devices(True), [])
        self.assertEqual(api.setup.SetupDiGetClassDevsW.call_args.args[-1], 6)
        api.setup.SetupDiDestroyDeviceInfoList.assert_called_once_with(99)
        api.setup.SetupDiGetDevicePropertyW.return_value = False
        with mock.patch.object(usb.C, 'get_last_error', return_value=1168, create=True):
            self.assertIsNone(api.property(99, usb.DevInfo(), 5, True))
        with mock.patch.object(usb.C, 'get_last_error', return_value=5, create=True):
            with self.assertRaises(OSError): api.property(99, usb.DevInfo(), 5, True)
        self.assertEqual(ctypes.sizeof(usb.DevInfo), 32 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28)

    def test_diagnostic_opened_identity_contract(self):
        good = dict(schema=1, status='raw', count=1, error=0, length=5, hex='5832303030',
                    instance_id=r'USB\VID_A108&PID_EAEF\6&21FC78A&0&1')
        def check(data, required=True):
            runner = mock.Mock(return_value=subprocess.CompletedProcess([], 0, json.dumps(data)))
            return usb.diagnostic('usbboot', runner, require_identity=required)
        self.assertEqual(check(good), good)
        legacy = {k: v for k, v in good.items() if k != 'instance_id'}
        self.assertEqual(check(legacy, False), legacy)  # Linux schema 1 remains usable.
        with self.assertRaises(ValueError): check(legacy)
        for value in (None, '', True, 1, [], {}, 'USB', 'USB\\VID_A108&PID_EAEF\\',
                      r'USB\VID_A108&PID_EAEF&MI_00\DISC', r'USB\VID_A108&PID_EAE0\DISC',
                      'USB\\VID_A108&PID_EAEF\\DISC\n', 'USB\\VID_A108&PID_EAEF\\D\u00dcSC',
                      'USB\\VID_A108&PID_EAEF\\D\u0131SC',
                      'USB\\VID_A108&PID_EAEF\\' + 'X' * 201):
            with self.subTest(value=value), self.assertRaises(ValueError):
                check(good | {'instance_id': value})
        for key in ('schema', 'count', 'error'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                check(good | {key: bool(good[key])})

    def test_diagnostic_rejects_duplicate_identity_and_extra_output(self):
        line = json.dumps(dict(schema=1, status='raw', count=1, error=0, length=0, hex='',
                               instance_id=r'USB\VID_A108&PID_EAEF\DISC'))
        for text in (line[:-1] + ', "instance_id": "USB\\\\VID_A108&PID_EAEF\\\\OTHER"}',
                     line + '\n' + line, line + '\nnoise', ' ' * 16384 + line):
            runner = mock.Mock(return_value=subprocess.CompletedProcess([], 0, text))
            with self.subTest(text=text[:100]), self.assertRaises(ValueError):
                usb.diagnostic('usbboot', runner, require_identity=True)

class NativeDiagnosticTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux mock compiler harness')
    def test_windows_native_diagnostic_binds_identity_to_opened_path(self):
        import shutil
        import tempfile
        if not shutil.which('gcc'):
            self.skipTest('gcc required for Windows API mocks')
        source = (Path(__file__).resolve().parents[1] / 'src/usbboot/usbboot.c').read_text()
        # Compile the actual Windows implementation with API mocks, never USB.
        implementation = source.split('#define DIAG_CAP 4096', 1)[1].split(
            '#endif\n\n/* Standalone raw diagnostic', 1)[0]
        prelude = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <wchar.h>
#define DIAG_CAP 4096
#define VR_GET_CPU_INFO 0
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
typedef uint32_t DWORD, ULONG;
typedef int32_t LONG;
typedef unsigned char BYTE;
typedef void *HANDLE, *HMODULE, *HKEY, *HDEVINFO, *WINUSB_INTERFACE_HANDLE;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; BYTE Data4[8]; } GUID;
typedef struct { DWORD cbSize, DevInst; } SP_DEVINFO_DATA;
typedef struct { DWORD cbSize; } SP_DEVICE_INTERFACE_DATA;
typedef struct { DWORD cbSize; wchar_t DevicePath[1]; } SP_DEVICE_INTERFACE_DETAIL_DATA_W;
typedef struct { BYTE RequestType, Request; uint16_t Value, Index, Length; } WINUSB_SETUP_PACKET;
#define INVALID_HANDLE_VALUE ((void *)-1)
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800
#define DIGCF_PRESENT 2
#define DIGCF_ALLCLASSES 4
#define DIGCF_DEVICEINTERFACE 16
#define DICS_FLAG_GLOBAL 1
#define DIREG_DEV 1
#define KEY_READ 1
#define REG_SZ 1
#define REG_MULTI_SZ 7
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_DATA 13
#define ERROR_NO_MORE_ITEMS 259
#define GENERIC_READ 1
#define GENERIC_WRITE 2
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define OPEN_EXISTING 3
#define FILE_FLAG_OVERLAPPED 4
#define PIPE_TRANSFER_TIMEOUT 3
static int scenario, opens, closes, frees, destroyed, keys, controls, initialized;
static int loads, unloads, live_modules, live_sets, live_keys, live_file, live_win;
static const char *missing_symbol;
static GUID interface_guid;
static DWORD last_error;
static int standard_guid(const GUID *g) {
    const GUID expected = {0xa5dcbf10, 0x6530, 0x11d2,
                           {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}};
    return !memcmp(g, &expected, sizeof(expected));
}
static const wchar_t *id(void) {
    return scenario == 9 ? L"USB\\VID_A108&PID_EAEF\\6&21FC78A&0&2" :
                          L"USB\\VID_A108&PID_EAEF\\6&21FC78A&0&1";
}
DWORD GetLastError(void) { return last_error; }
HMODULE LoadLibraryExW(const wchar_t *name, void *reserved, DWORD flags) {
    if(flags != LOAD_LIBRARY_SEARCH_SYSTEM32) abort();
    int index = !wcscmp(name, L"setupapi.dll") ? 0 : !wcscmp(name, L"advapi32.dll") ? 1 :
                !wcscmp(name, L"winusb.dll") ? 2 : -1;
    if(index < 0 || loads++ != index) abort();
    last_error = 126 + index;
    if(scenario == 1 || scenario == 30 + index) return NULL;
    live_modules |= 1 << index;
    return (void *)(uintptr_t)(index + 1);
}
int FreeLibrary(HMODULE module) {
    int index = (int)(uintptr_t)module - 1;
    if(index < 0 || index > 2 || !(live_modules & (1 << index)) ||
       live_modules >> (index + 1) || live_sets || live_keys || live_file || live_win) abort();
    live_modules &= ~(1 << index); unloads++; last_error = 999; return 1;
}
HDEVINFO SetupDiGetClassDevsW(const GUID *g, const wchar_t *enumerator, void *parent, DWORD flags) {
    if(live_modules != 7) abort();
    if(g) {
        if(wcscmp(enumerator, id()) || flags != (DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)) abort();
        interface_guid = *g; live_sets++;
        return (void *)2;
    }
    if(wcscmp(enumerator, L"USB") || flags != (DIGCF_PRESENT | DIGCF_ALLCLASSES)) abort();
    if(scenario == 25) { last_error = 5; return INVALID_HANDLE_VALUE; }
    live_sets++;
    return (void *)1;
}
int SetupDiEnumDeviceInfo(HDEVINFO set, DWORD i, SP_DEVINFO_DATA *dev) {
    if(scenario == 2 || i >= (scenario == 3 ? 2 : 1)) { last_error = ERROR_NO_MORE_ITEMS; return 0; }
    if(dev->cbSize != sizeof(*dev)) abort(); dev->DevInst = i; return 1;
}
int SetupDiGetDeviceInstanceIdW(HDEVINFO set, SP_DEVINFO_DATA *dev, wchar_t *buf, DWORD cap, DWORD *size) {
    wcscpy(buf, id());
    if(scenario == 24 && set == (void *)2) buf[wcslen(buf) - 1] = L'2';
    return 1;
}
HKEY SetupDiOpenDevRegKey(HDEVINFO set, SP_DEVINFO_DATA *dev, DWORD a, DWORD b, DWORD c, DWORD d) {
    if(scenario == 19) { last_error = 2; return INVALID_HANDLE_VALUE; }
    live_keys++; return (void *)3;
}
LONG RegQueryValueExW(HKEY key, const wchar_t *name, void *reserved, DWORD *type, BYTE *buf, DWORD *size) {
    if(scenario == 18 || scenario == 22 || scenario == 23 || scenario == 24) return ERROR_FILE_NOT_FOUND;
    if(scenario == 21) return 5;
    const wchar_t guid[] = L"{12345678-1234-1234-1234-123456789abc}\0{22345678-1234-1234-1234-123456789abc}\0";
    memcpy(buf, guid, sizeof(guid)); *size = sizeof(guid); *type = REG_MULTI_SZ;
    if(scenario == 10 || scenario == 20) *type = 4;
    if(scenario == 11) ((wchar_t *)buf)[*size / sizeof(wchar_t) - 1] = L'X';
    if(scenario == 12) ((wchar_t *)buf)[1] = L'Z';
    if(scenario == 13 && !wcscmp(name, L"DeviceInterfaceGUIDs")) return ERROR_FILE_NOT_FOUND;
    return 0;
}
LONG RegCloseKey(HKEY key) { keys++; live_keys--; last_error = 998; return 0; }
int SetupDiEnumDeviceInterfaces(HDEVINFO set, SP_DEVINFO_DATA *dev, const GUID *guid, DWORD index, SP_DEVICE_INTERFACE_DATA *iface) {
    if(set != (void *)2 || dev || index || iface->cbSize != sizeof(*iface)) abort();
    if(standard_guid(guid) && !((scenario >= 18 && scenario <= 24) || scenario == 27)) {
        last_error = ERROR_NO_MORE_ITEMS; return 0;
    }
    if(scenario == 22) { last_error = ERROR_NO_MORE_ITEMS; return 0; }
    if(scenario == 14 && guid->Data1 == 0x12345678) { last_error = ERROR_NO_MORE_ITEMS; return 0; }
    return 1;
}
int SetupDiGetDeviceInterfaceDetailW(HDEVINFO set, SP_DEVICE_INTERFACE_DATA *iface,
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail, DWORD cap, DWORD *size, SP_DEVINFO_DATA *dev) {
    if(set != (void *)2 || detail->cbSize != sizeof(*detail)) abort();
    wchar_t path[512]; wcscpy(path, L"\\\\?\\"); wcscat(path, id());
    for(wchar_t *p = path + 4; *p; p++) if(*p == L'\\') *p = L'#';
    wcscat(path, standard_guid(&interface_guid) ? L"#{a5dcbf10-6530-11d2-901f-00c04fb951ed}" :
                L"#{12345678-1234-1234-1234-123456789abc}");
    if(scenario == 6 || scenario == 23) path[4 + wcslen(id()) - 1] = L'2';  /* A metadata, B open path. */
    if(scenario == 15) path[0] = L'X';
    *size = offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) + (wcslen(path) + 1) * sizeof(wchar_t);
    if(*size > cap) abort(); wcscpy(detail->DevicePath, path); return 1;
}
int SetupDiDestroyDeviceInfoList(HDEVINFO set) { destroyed++; live_sets--; last_error = 997; return 1; }
HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share, void *sec, DWORD creation, DWORD flags, HANDLE template) {
    opens++; last_error = 5;
    if(access != 3 || share != 3 || creation != OPEN_EXISTING || flags != FILE_FLAG_OVERLAPPED) abort();
    if(scenario == 27 && !wcsstr(path, L"#{a5dcbf10-6530-11d2-901f-00c04fb951ed}")) abort();
    if(scenario == 4) return INVALID_HANDLE_VALUE;
    live_file++; return (void *)4;
}
int CloseHandle(HANDLE handle) { if(handle != (void *)4 || live_win) abort(); closes++; live_file--; last_error = 996; return 1; }
int WinUsb_Initialize(HANDLE file, WINUSB_INTERFACE_HANDLE *win) {
    if(file != (void *)4) abort(); initialized++; last_error = 6;
    if(scenario == 5) return 0; *win = (void *)5; live_win++; return 1;
}
int WinUsb_Free(WINUSB_INTERFACE_HANDLE win) { if(win != (void *)5 || live_modules != 7) abort(); frees++; live_win--; last_error = 995; return 1; }
int WinUsb_SetPipePolicy(WINUSB_INTERFACE_HANDLE win, BYTE pipe, ULONG policy, ULONG size, void *value) {
    if(win != (void *)5 || pipe || policy != PIPE_TRANSFER_TIMEOUT || size != sizeof(ULONG) || *(ULONG *)value != 1000) abort();
    last_error = 87; return scenario != 16;
}
int WinUsb_ControlTransfer(WINUSB_INTERFACE_HANDLE win, WINUSB_SETUP_PACKET packet,
    BYTE *raw, ULONG size, ULONG *transferred, void *overlap) {
    if(win != (void *)5 || packet.RequestType != 0xc0 || packet.Request || packet.Value || packet.Index || packet.Length != 8 || size != 8 || overlap) abort();
    controls++; last_error = 121;
    if(scenario == 7) return 0;
    memcpy(raw, "X2000abc", 8); *transferred = scenario == 8 ? 8 : scenario == 17 ? 9 : 5; return 1;
}
void *GetProcAddress(HMODULE module, const char *name) {
    if(live_modules != 7) abort();
    if(missing_symbol && !strcmp(name, missing_symbol)) { last_error = 127; return NULL; }
#define API(fn) if(!strcmp(name, #fn)) { \
    int expected = !strncmp(name, "SetupDi", 7) ? 1 : !strncmp(name, "Reg", 3) ? 2 : 3; \
    if((int)(uintptr_t)module != expected) abort(); return (void *)fn; }
    API(SetupDiGetClassDevsW) API(SetupDiEnumDeviceInfo) API(SetupDiGetDeviceInstanceIdW)
    API(SetupDiOpenDevRegKey) API(SetupDiEnumDeviceInterfaces) API(SetupDiGetDeviceInterfaceDetailW)
    API(SetupDiDestroyDeviceInfoList) API(RegQueryValueExW) API(RegCloseKey)
    API(WinUsb_Initialize) API(WinUsb_Free) API(WinUsb_SetPipePolicy) API(WinUsb_ControlTransfer)
#undef API
    abort();
}
'''
        main = r'''
int main(int argc, char **argv) {
    scenario = atoi(argv[1]); missing_symbol = argc > 2 ? argv[2] : NULL;
    int rc = windows_native_diagnostic();
    if(live_modules || live_sets || live_keys || live_file || live_win) abort();
    fprintf(stderr, "%d %d %d %d %d %d %d %d %d", opens, closes, initialized, frees, keys, destroyed, controls, loads, unloads);
    return rc;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c, exe = Path(tmp) / 'windows.c', Path(tmp) / 'windows'
            c.write_text(prelude + implementation + main, encoding='ascii')
            compiled = subprocess.run(['gcc', '-O2', '-std=c99', '-Wall', str(c), '-o', str(exe)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            cases = [(0, 'raw', '1 1 1 1 1 3 1'), (1, 'init', '0 0 0 0 0 0 0'),
                     (2, 'absent', '0 0 0 0 0 1 0'), (3, 'ambiguous', '0 0 0 0 0 1 0'),
                     (4, 'open', '1 0 0 0 1 3 0'), (5, 'claim', '1 1 1 0 1 3 0'),
                     (6, 'identity', '0 0 0 0 1 3 0'), (7, 'control', '1 1 1 1 1 3 1'),
                     (8, 'raw', '1 1 1 1 1 3 1'), (9, 'raw', '1 1 1 1 1 3 1'),
                     (10, 'identity', '0 0 0 0 1 2 0'), (11, 'identity', '0 0 0 0 1 2 0'),
                     (12, 'raw', '1 1 1 1 1 3 1'), (13, 'raw', '1 1 1 1 1 3 1'),
                     (14, 'raw', '1 1 1 1 1 4 1'), (15, 'identity', '0 0 0 0 1 3 0'),
                     (16, 'control', '1 1 1 1 1 3 0'), (17, 'control', '1 1 1 1 1 3 1'),
                     (18, 'raw', '1 1 1 1 1 2 1'),  # Both registry GUID values absent.
                     (19, 'raw', '1 1 1 1 0 2 1'),  # Registry key absent.
                     (20, 'raw', '1 1 1 1 1 2 1'),  # Invalid optional value type.
                     (21, 'raw', '1 1 1 1 1 2 1'),  # Registry access denied.
                     (22, 'identity', '0 0 0 0 1 2 0'),  # No interface in any class.
                     (23, 'identity', '0 0 0 0 1 2 0'),  # Standard path disagrees.
                     (24, 'identity', '0 0 0 0 1 2 0'),  # Standard metadata disagrees.
                     (25, 'enumerate', '0 0 0 0 0 0 0'),
                     (27, 'raw', '1 1 1 1 1 2 1')]  # Standard precedes registry extras.
            errors = {1: 126, 4: 5, 5: 6, 7: 121, 16: 87, 25: 5}
            for scenario, status, cleanup in cases:
                with self.subTest(scenario=scenario):
                    proc = subprocess.run([str(exe), str(scenario)], capture_output=True, text=True)
                    self.assertEqual(len(proc.stdout.splitlines()), 1)
                    data = json.loads(proc.stdout)
                    self.assertEqual(data['status'], status)
                    self.assertEqual(proc.stderr, cleanup + (' 1 0' if scenario == 1 else ' 3 3'))
                    self.assertEqual(data['error'], errors.get(scenario, 0 if status == 'raw' else 13))
                    self.assertEqual(proc.returncode, 0 if status == 'raw' else 1)
                    if status == 'raw':
                        self.assertEqual(data['instance_id'], r'USB\VID_A108&PID_EAEF\6&21FC78A&0&' + ('2' if scenario == 9 else '1'))
                        self.assertEqual(data['hex'], '5832303030616263' if scenario == 8 else '5832303030')
                        self.assertEqual(data['length'], 8 if scenario == 8 else 5)
                    elif scenario not in (5, 7, 16, 17):
                        self.assertIsNone(data['instance_id'])
            # Fail every DLL load and every symbol resolution. Cleanup overwrites
            # GetLastError, so the reported loader error must already be saved.
            symbols = ('SetupDiGetClassDevsW', 'SetupDiEnumDeviceInfo', 'SetupDiGetDeviceInstanceIdW',
                       'SetupDiOpenDevRegKey', 'SetupDiEnumDeviceInterfaces', 'SetupDiGetDeviceInterfaceDetailW',
                       'SetupDiDestroyDeviceInfoList', 'RegQueryValueExW', 'RegCloseKey',
                       'WinUsb_Initialize', 'WinUsb_Free', 'WinUsb_SetPipePolicy', 'WinUsb_ControlTransfer')
            failures = [(30 + i, None, i + 1, i, 126 + i) for i in range(3)]
            failures += [(33, symbol, 3, 3, 127) for symbol in symbols]
            for scenario, symbol, loads, unloads, error in failures:
                with self.subTest(scenario=scenario, symbol=symbol):
                    proc = subprocess.run([str(exe), str(scenario)] + ([symbol] if symbol else []),
                                          capture_output=True, text=True)
                    self.assertEqual(proc.returncode, 1, proc.stderr)
                    data = json.loads(proc.stdout)
                    self.assertEqual(data, dict(schema=1, status='init', count=0, error=error,
                                                length=-1, hex='', instance_id=None))
                    self.assertEqual(proc.stderr, f'0 0 0 0 0 0 0 {loads} {unloads}')

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux mock compiler harness')
    def test_native_diagnostic_no_device_io(self):
        import shutil
        import tempfile
        if not shutil.which('gcc') or not Path('/usr/include/libusb-1.0/libusb.h').is_file():
            self.skipTest('gcc and libusb headers required for native mocks')
        source = Path(__file__).resolve().parents[1] / 'src/usbboot/usbboot.c'
        harness = r'''
#define main usbboot_original_main
#include "SOURCE"
#undef main
static int matches, open_rc, claim_rc, reply, opens, closes, releases;
static libusb_device *list[4];
int libusb_init(libusb_context **ctx) { return 0; }
void libusb_exit(libusb_context *ctx) {}
ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***out) {
    *out = list; return matches;
}
int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *desc) {
    desc->idVendor = 0xa108; desc->idProduct = 0xeaef; return 0;
}
int libusb_open(libusb_device *d, libusb_device_handle **h) {
    opens++; if(!open_rc) *h = (void*)1; return open_rc;
}
int libusb_claim_interface(libusb_device_handle *h, int i) { return claim_rc; }
int libusb_release_interface(libusb_device_handle *h, int i) { releases++; return 0; }
void libusb_close(libusb_device_handle *h) { closes++; }
void libusb_free_device_list(libusb_device **d, int unref) {}
int libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t req,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t len, unsigned int timeout) {
    if(type != 0xc0 || req != VR_GET_CPU_INFO || value || index || len != 8) abort();
    for(int i=0; i<8; i++) data[i] = i; return reply;
}
int main(int argc, char **argv) {
    matches=atoi(argv[1]); open_rc=atoi(argv[2]); claim_rc=atoi(argv[3]); reply=atoi(argv[4]);
    int rc=native_diagnostic();
    fprintf(stderr, "%d %d %d", opens, closes, releases); return rc;
}
'''.replace('SOURCE', str(source))
        with tempfile.TemporaryDirectory() as tmp:
            c, exe = Path(tmp) / 'mock.c', Path(tmp) / 'mock'
            c.write_text(harness, encoding='ascii')
            subprocess.run(['gcc', '-O2', '-std=c99', '-ffunction-sections', '-fdata-sections',
                            '-I/usr/include/libusb-1.0', str(c), '-Wl,--gc-sections', '-o', str(exe)],
                           check=True, capture_output=True)
            for args, status, cleanup in [((0, 0, 0, 8), 'absent', '0 0 0'),
                    ((2, 0, 0, 8), 'ambiguous', '0 0 0'), ((1, -3, 0, 8), 'open', '1 0 0'),
                    ((1, 0, -6, 8), 'claim', '1 1 0'), ((1, 0, 0, -7), 'control', '1 1 1'),
                    ((1, 0, 0, 5), 'raw', '1 1 1'), ((1, 0, 0, 8), 'raw', '1 1 1')]:
                with self.subTest(args=args):
                    proc = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
                    data = json.loads(proc.stdout)
                    self.assertEqual(data['status'], status)
                    self.assertEqual(proc.stderr, cleanup)
                    self.assertEqual(proc.returncode, 0 if status == 'raw' else 1)
                    if status == 'raw':
                        self.assertEqual(data['length'], args[-1])
                        self.assertEqual(data['hex'], bytes(range(args[-1])).hex())
