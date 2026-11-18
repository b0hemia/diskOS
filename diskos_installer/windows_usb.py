# SPDX-License-Identifier: GPL-3.0-or-later
"""SetupAPI inventory and isolated driver setup; no extra libusb DLL."""
import ctypes as C
import json
import ntpath
import re
import subprocess
import sys
import uuid

from . import bundle, platform_probe
from .errors import PreflightError

TARGET = re.compile(r"^USB\\VID_A108&PID_EAEF(?:&REV_[0-9A-F]{4})?\\", re.I)
DWORD = C.c_uint32

_HELPER_MESSAGES = {
    "refused": {
        "inventory_error": "Windows could not read the USB device list",
        "invalid_target": "Windows could not verify the Disc's USB identity",
        "not_unique": "Setup needs exactly one connected mask-ROM Disc",
        "unfamiliar_driver": "The Disc has an unfamiliar driver; setup will not replace it",
        "inf_unavailable": "Windows could not find its built-in WinUSB driver file",
        "inf_path_too_long": "The Windows driver file path is too long for setup",
        "invalid_driver_detail": "Windows returned incomplete driver information",
        "provider_mismatch": "The available driver is not Microsoft's WinUSB driver",
        "inf_mismatch": "The available driver does not use the built-in WinUSB driver file",
        "ambiguous_winusb_node": "Windows found more than one matching WinUSB driver",
        "no_winusb_node": "Windows could not find a matching WinUSB driver",
    },
    "api_error": {
        "get_params_failed": "Windows could not read the driver setup options",
        "set_params_failed": "Windows could not apply the driver setup options",
        "build_list_failed": "Windows could not build the list of available drivers",
        "driver_enum_failed": "Windows could not read the list of available drivers",
        "driver_detail_failed": "Windows could not read the available driver's details",
        "select_driver_failed": "Windows could not select the WinUSB driver",
        "install_failed": "Windows could not install the WinUSB driver",
    },
    "loader_policy_refused": {"loader_policy_refused": "Setup could not load the required Windows system libraries"},
    "arguments_refused": {"arguments_refused": "Driver setup received unexpected command options"},
    "elevation_required": {"elevation_required": "Windows administrator approval or credentials are required"},
    "system_path_refused": {"system_path_refused": "Windows could not provide a usable system directory"},
    "target_changed": {"target_changed": "The USB device or driver changed during setup"},
    "installed_status_unknown": {"installed_status_unknown":
        "The driver was installed, but Windows could not confirm whether a restart is needed"},
}


def _windows_error(message, error):
    if type(error) is int and 0 <= error <= 0xffffffff:
        return f"{message} (Windows error {error})"
    return message


class GUID(C.Structure):
    _fields_ = [("raw", C.c_ubyte * 16)]

    @classmethod
    def make(cls, value):
        return cls.from_buffer_copy(uuid.UUID(value).bytes_le)


class DevInfo(C.Structure):
    _fields_ = [("cbSize", DWORD), ("ClassGuid", GUID),
                ("DevInst", DWORD), ("Reserved", C.c_size_t)]


class PropertyKey(C.Structure):
    _fields_ = [("fmtid", GUID), ("pid", DWORD)]


class SetupAPI:
    def __init__(self):
        self.setup = C.WinDLL("setupapi", use_last_error=True)
        self.cfg = C.WinDLL("cfgmgr32", use_last_error=True)
        signatures = {
            "SetupDiGetClassDevsW": ([C.c_void_p, C.c_wchar_p, C.c_void_p, DWORD], C.c_void_p),
            "SetupDiEnumDeviceInfo": ([C.c_void_p, DWORD, C.POINTER(DevInfo)], C.c_int),
            "SetupDiGetDeviceInstanceIdW": ([C.c_void_p, C.POINTER(DevInfo), C.c_wchar_p,
                                             DWORD, C.POINTER(DWORD)], C.c_int),
            "SetupDiGetDevicePropertyW": ([C.c_void_p, C.POINTER(DevInfo), C.POINTER(PropertyKey),
                                           C.POINTER(DWORD), C.c_void_p, DWORD,
                                           C.POINTER(DWORD), DWORD], C.c_int),
            "SetupDiDestroyDeviceInfoList": ([C.c_void_p], C.c_int),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.setup, name)
            fn.argtypes, fn.restype = args, result
        self.cfg.CM_Get_DevNode_Status.argtypes = [C.POINTER(DWORD), C.POINTER(DWORD), DWORD, DWORD]
        self.cfg.CM_Get_DevNode_Status.restype = DWORD

    def property(self, handle, info, pid, driver=False):
        guid = "a8b865dd-2e3d-4094-ad97-e593a70c75d6" if driver else "a45c254e-df1c-4efd-8020-67d146a850e0"
        key = PropertyKey(GUID.make(guid), pid)
        kind, size = DWORD(), DWORD()
        fn = self.setup.SetupDiGetDevicePropertyW
        if not fn(handle, C.byref(info), C.byref(key), C.byref(kind), None, 0, C.byref(size), 0):
            error = C.get_last_error()
            if error == 1168:  # ERROR_NOT_FOUND: unbound devices lack driver properties
                return None
            if error != 122:
                raise OSError(error, "SetupDiGetDevicePropertyW")
        if not size.value or size.value > 65536:
            raise OSError("invalid device property size")
        buf = (C.c_ubyte * size.value)()
        if not fn(handle, C.byref(info), C.byref(key), C.byref(kind), buf, size,
                  C.byref(size), 0):
            raise C.WinError(C.get_last_error())
        if kind.value not in (18, 0x2012):  # DEVPROP_TYPE_STRING / STRING_LIST
            raise OSError("unexpected device property type")
        values = bytes(buf)[:size.value].decode("utf-16-le").rstrip("\0").split("\0")
        return values if kind.value == 0x2012 else values[0]

    def devices(self, present_only):
        handle = self.setup.SetupDiGetClassDevsW(None, "USB", None, 4 | (2 if present_only else 0))
        if handle == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        rows = []
        try:
            index = 0
            while True:
                info = DevInfo()
                info.cbSize = C.sizeof(info)
                if not self.setup.SetupDiEnumDeviceInfo(handle, index, C.byref(info)):
                    if C.get_last_error() == 259:
                        break
                    raise C.WinError(C.get_last_error())
                index += 1
                instance = C.create_unicode_buffer(4096)
                if not self.setup.SetupDiGetDeviceInstanceIdW(handle, C.byref(info), instance, 4096, None):
                    raise C.WinError(C.get_last_error())
                if not TARGET.match(instance.value):
                    continue
                status, problem = DWORD(), DWORD()
                ret = self.cfg.CM_Get_DevNode_Status(C.byref(status), C.byref(problem), info.DevInst, 0)
                if present_only and ret:
                    raise OSError(ret, "CM_Get_DevNode_Status")
                rows.append({"instance_id": instance.value, "problem_code": problem.value if ret == 0 else None,
                             "service": self.property(handle, info, 6),
                             "inf": self.property(handle, info, 5, True),
                             "provider": self.property(handle, info, 9, True),
                             "location_paths": self.property(handle, info, 37)})
        finally:
            self.setup.SetupDiDestroyDeviceInfoList(handle)
        return rows


def inventory(api=None):
    api = api or SetupAPI()
    # All first, then present: never count a phantom as an attached target.
    all_rows = api.devices(False)
    present = [r for r in api.devices(True) if TARGET.match(r["instance_id"])]
    ids = {r["instance_id"].upper() for r in present}
    phantom = [r for r in all_rows if TARGET.match(r["instance_id"]) and r["instance_id"].upper() not in ids]
    state = "ambiguous" if len(present) > 1 else "absent"
    if len(present) == 1:
        row = present[0]
        state = "winusb" if row["problem_code"] == 0 and (row["service"] or "").lower() == "winusb" else "driver"
    elif not present and phantom:
        state = "phantom"
    return {"state": state, "count": len(present), "present": present, "phantom": phantom}


def require_target(result):
    state = result["state"]
    if state in ("absent", "phantom"):
        raise PreflightError("no present mask-ROM device", code="E110",
                             action="power the device OFF, hold Volume-Down and plug in USB; retry inventory")
    if state == "ambiguous":
        raise PreflightError("more than one mask-ROM device", code="E111", action="unplug the other Ingenic devices")
    if state != "winusb":
        row = result["present"][0]
        raise PreflightError(f"mask-ROM device present but WinUSB is not ready (problem code {row['problem_code']})",
                             code="E114", action="bind this exact mask-ROM instance to WinUSB, then repeat check A")
    return result["present"][0]


def diagnostic(tool, runner=subprocess.run, *, require_identity=False):
    """Open/claim/control only; Windows setup also requires the opened path's ID."""
    proc = runner([str(tool), "--native-diagnostic"], capture_output=True, text=True, timeout=15)
    if len(proc.stdout) > 16384 or len(proc.stdout.splitlines()) != 1:
        raise ValueError("native USB diagnostic must return one JSON line")
    def unique(pairs):
        data = {}
        for key, value in pairs:
            if key in data:
                raise ValueError("duplicate native USB diagnostic key")
            data[key] = value
        return data
    data = json.loads(proc.stdout, object_pairs_hook=unique)
    if not isinstance(data, dict):
        raise ValueError("native USB diagnostic is not an object")
    length, raw = data.get("length"), data.get("hex")
    if (type(data.get("schema")) is not int or data["schema"] != 1 or data.get("status") != "raw"
            or type(data.get("count")) is not int or data["count"] != 1
            or type(data.get("error")) is not int or data["error"] != 0 or type(length) is not int
            or not 0 <= length <= 8 or not isinstance(raw, str)
            or not re.fullmatch(r"[0-9a-f]*", raw) or len(raw) != length * 2 or proc.returncode != 0):
        raise ValueError("native USB diagnostic failed or malformed: " + proc.stdout.strip())
    if require_identity or "instance_id" in data:
        instance = data.get("instance_id")
        if (not isinstance(instance, str) or not instance.isascii() or len(instance) > 200
                or not re.fullmatch(r"USB\\VID_A108&PID_EAEF\\[A-Z0-9&_.{}+-]+", instance, re.I)):
            raise ValueError("native USB diagnostic has no valid opened-device identity")
    return data


def driver_inventory():
    try:
        return inventory()
    except (OSError, ValueError) as e:
        raise PreflightError(_windows_error("Windows could not read the USB driver state",
                                           getattr(e, "winerror", getattr(e, "errno", None))), code="E126",
                             action="reconnect the Disc and check again") from e


def driver_description(result):
    if result["count"] != 1:
        return {"absent": "No Disc in mask-ROM mode. Power off, hold Volume-Down and connect USB.",
                "phantom": "Only a disconnected Disc is recorded. Reconnect in mask-ROM mode.",
                "ambiguous": "More than one mask-ROM device. Unplug the other devices."}[result["state"]]
    row = result["present"][0]
    return (f"USB driver: service={row['service'] or 'none'}, INF={row['inf'] or 'none'}, "
            f"provider={row['provider'] or 'none'}, problem code={row['problem_code']}")


def _known_driver(row):
    values = tuple((row[k] or "").lower() for k in ("service", "inf", "provider"))
    return ((row["problem_code"] == 28 and values == ("", "", ""))
            or (row["problem_code"] == 0 and values == ("winusb", "winusb.inf", "microsoft")))


def _helper_result(proc):
    try:
        if len(proc.stdout.splitlines()) != 1 or len(proc.stdout) > 4096:
            raise ValueError("expected one JSON line")
        def unique(pairs):
            data = {}
            for key, value in pairs:
                if key in data:
                    raise ValueError("duplicate JSON key")
                data[key] = value
            return data
        data = json.loads(proc.stdout, object_pairs_hook=unique)
        statuses = {"installed", "already_bound", "reboot_required"} | _HELPER_MESSAGES.keys()
        if (not isinstance(data, dict) or set(data) != {"schema", "status", "reason", "error",
                                                      "installed", "reboot_required"}
                or type(data["schema"]) is not int or data["schema"] != 1
                or data["status"] not in statuses or not isinstance(data["reason"], str)
                or not re.fullmatch(r"[a-z_]+", data["reason"])
                or type(data["error"]) is not int or not 0 <= data["error"] <= 0xffffffff
                or type(data["installed"]) is not bool or type(data["reboot_required"]) is not bool):
            raise ValueError("invalid helper schema")
        status = data["status"]
        success = status in ("installed", "already_bound", "reboot_required")
        if data["reason"] not in _HELPER_MESSAGES.get(status, {"none"}):
            raise ValueError("unknown helper reason")
        if (proc.returncode != (0 if success else 1)
                or (success and (data["error"] != 0 or data["reason"] != "none"))
                or data["installed"] != (status in ("installed", "reboot_required", "installed_status_unknown"))
                or data["reboot_required"] != (status in ("reboot_required", "installed_status_unknown"))):
            raise ValueError("inconsistent helper result")
        return data
    except (ValueError, TypeError, KeyError, AttributeError) as e:
        raise PreflightError("Driver setup returned an unreadable result", code="E126",
                             action="setup may have run; check the driver state before trying again") from e


class _Overlapped(C.Structure):
    _fields_ = [("Internal", C.c_size_t), ("InternalHigh", C.c_size_t),
                ("Offset", DWORD), ("OffsetHigh", DWORD), ("hEvent", C.c_void_p)]


class _ShellInfo(C.Structure):
    _fields_ = [("cbSize", DWORD), ("fMask", DWORD), ("hwnd", C.c_void_p),
                ("lpVerb", C.c_wchar_p), ("lpFile", C.c_wchar_p), ("lpParameters", C.c_wchar_p),
                ("lpDirectory", C.c_wchar_p), ("nShow", C.c_int), ("hInstApp", C.c_void_p),
                ("lpIDList", C.c_void_p), ("lpClass", C.c_wchar_p), ("hkeyClass", C.c_void_p),
                ("dwHotKey", DWORD), ("hIcon", C.c_void_p), ("hProcess", C.c_void_p)]


def _elevated_helper(tool):
    """Capture the fixed helper's stdout through a local pipe, without a privileged file write.

    ShellExecuteEx cannot redirect stdout. System32 cmd only supplies redirection;
    /d disables AutoRun, paths are quoted, and expansion characters are refused.
    The helper receives no arguments. No installer code runs elevated.
    """
    drive, rest = ntpath.splitdrive(tool)
    if (len(drive) != 2 or drive[1] != ":" or not drive[0].isascii() or not drive[0].isalpha()
            or not rest.startswith(("\\", "/")) or any(c in tool for c in '%!"\r\n\0')):
        raise ValueError("move the installer to a local path without %, ! or quotes")
    kernel = C.WinDLL("kernel32", use_last_error=True)
    shell = C.WinDLL("shell32", use_last_error=True)
    adv = C.WinDLL("advapi32", use_last_error=True)
    ole = C.WinDLL("ole32", use_last_error=True)
    ptr, dp, op = C.c_void_p, C.POINTER(DWORD), C.POINTER(_Overlapped)
    signatures = {
        "GetSystemDirectoryW": ([C.c_wchar_p, DWORD], C.c_uint),
        "CreateNamedPipeW": ([C.c_wchar_p, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, ptr], ptr),
        "CreateEventW": ([ptr, C.c_int, C.c_int, C.c_wchar_p], ptr),
        "ConnectNamedPipe": ([ptr, op], C.c_int), "WaitForSingleObject": ([ptr, DWORD], DWORD),
        "WaitForMultipleObjects": ([DWORD, C.POINTER(ptr), C.c_int, DWORD], DWORD),
        "GetOverlappedResult": ([ptr, op, dp, C.c_int], C.c_int),
        "GetNamedPipeClientProcessId": ([ptr, dp], C.c_int), "GetProcessId": ([ptr], DWORD),
        "ReadFile": ([ptr, ptr, DWORD, dp, op], C.c_int),
        "GetExitCodeProcess": ([ptr, dp], C.c_int), "CancelIoEx": ([ptr, op], C.c_int),
        "CloseHandle": ([ptr], C.c_int), "LocalFree": ([ptr], ptr),
    }
    for name, (args, restype) in signatures.items():
        fn = getattr(kernel, name)
        fn.argtypes, fn.restype = args, restype
    shell.ShellExecuteExW.argtypes, shell.ShellExecuteExW.restype = [C.POINTER(_ShellInfo)], C.c_int
    convert = adv.ConvertStringSecurityDescriptorToSecurityDescriptorW
    convert.argtypes, convert.restype = [C.c_wchar_p, DWORD, C.POINTER(ptr), ptr], C.c_int
    ole.CoInitializeEx.argtypes, ole.CoInitializeEx.restype = [ptr, DWORD], C.c_int32
    ole.CoUninitialize.argtypes, ole.CoUninitialize.restype = [], None
    class Security(C.Structure):
        _fields_ = [("length", DWORD), ("descriptor", ptr), ("inherit", C.c_int)]
    descriptor, pipe, event = ptr(), None, None
    com = False
    info, ov = _ShellInfo(), _Overlapped()
    def check(ok):
        if not ok:
            raise C.WinError(C.get_last_error())
    def wait(handle):
        if kernel.WaitForSingleObject(handle, 300000) != 0:
            raise OSError("driver setup timed out; it may still be running")
    try:
        com = ole.CoInitializeEx(None, 2) >= 0
        system = C.create_unicode_buffer(32768)
        length = kernel.GetSystemDirectoryW(system, len(system))
        if not 0 < length < len(system):
            raise OSError("Windows system directory is unavailable")
        # cmd redirection fails with GW; GRGW succeeded in Windows rehearsal.
        # Allow other administrator credentials; deny anonymous tokens.
        # PIPE_REJECT_REMOTE_CLIENTS below rejects remote pipe connections.
        # The inbound pipe gives clients no read endpoint, and one instance
        # prevents another server reader. Verify the writer's PID below.
        check(convert("D:(D;;GA;;;AN)(A;;GRGW;;;WD)", 1, C.byref(descriptor), None))
        security = Security(C.sizeof(Security), descriptor, False)
        name = "\\\\.\\pipe\\diskos-driver-" + uuid.uuid4().hex
        # Byte stream preserves all stdout writes for the single-line validation.
        pipe = kernel.CreateNamedPipeW(name, 0x40080001, 8, 1, 4096, 4096, 0, C.byref(security))
        if pipe == C.c_void_p(-1).value:
            pipe = None
            check(False)
        event = kernel.CreateEventW(None, True, False, None)
        check(event)
        ov.hEvent = event
        if not kernel.ConnectNamedPipe(pipe, C.byref(ov)) and C.get_last_error() != 997:
            check(False)
        info.cbSize, info.fMask = C.sizeof(info), 0x540
        info.lpVerb, info.lpFile = "runas", ntpath.join(system.value, "cmd.exe")
        info.lpParameters = f'/d /v:off /s /c ""{tool}" > "{name}""'
        info.lpDirectory, info.nShow = ntpath.dirname(tool), 0
        check(shell.ShellExecuteExW(C.byref(info)))
        check(info.hProcess)
        if kernel.WaitForMultipleObjects(2, (ptr * 2)(event, info.hProcess), False, 300000) != 0:
            raise OSError("driver setup exited without output or timed out; check the driver state")
        size, pid, code = DWORD(), DWORD(), DWORD()
        check(kernel.GetOverlappedResult(pipe, C.byref(ov), C.byref(size), False))
        check(kernel.GetNamedPipeClientProcessId(pipe, C.byref(pid)))
        if pid.value != kernel.GetProcessId(info.hProcess):
            raise OSError("driver setup output came from another process")
        wait(info.hProcess)
        check(kernel.GetExitCodeProcess(info.hProcess, C.byref(code)))
        buf = C.create_string_buffer(4096)
        ov = _Overlapped(hEvent=event)
        if not kernel.ReadFile(pipe, buf, len(buf), C.byref(size), C.byref(ov)):
            if C.get_last_error() != 997:
                check(False)
            wait(event)
            check(kernel.GetOverlappedResult(pipe, C.byref(ov), C.byref(size), False))
        if size.value >= len(buf):
            raise ValueError("driver setup output was too long")
        return subprocess.CompletedProcess([tool], code.value, buf.raw[:size.value].decode("ascii"))
    finally:
        if pipe:
            kernel.CancelIoEx(pipe, None)
            # Keep OVERLAPPED storage alive until cancellation has completed.
            kernel.GetOverlappedResult(pipe, C.byref(ov), C.byref(DWORD()), True)
            kernel.CloseHandle(pipe)
        for handle in (event, info.hProcess):
            if handle:
                kernel.CloseHandle(handle)
        if descriptor.value:
            kernel.LocalFree(descriptor)
        if com:
            ole.CoUninitialize()


def setup_driver(report, confirm):
    """Separate setup operation: never call the flash service or retain a USB handle."""
    if sys.platform != "win32" or platform_probe.host() != ("windows", "x86_64") or C.sizeof(C.c_void_p) != 8:
        raise PreflightError("Driver setup needs native x64 Windows", code="E101")
    before = driver_inventory()
    report(driver_description(before))
    if before["count"] != 1:
        require_target(before)
    row = before["present"][0]
    if not _known_driver(row):
        raise PreflightError("The Disc has an unfamiliar driver", code="E127",
                             action="check its service, INF and provider; setup will not replace it")
    if before["state"] != "winusb":
        if not confirm():
            return {"aborted": True, "message": "Driver setup cancelled."}
        tool = bundle.native("bind_winusb")
        bundle.verify_windows_driver_bundle()
        try:
            result = _helper_result(_elevated_helper(tool))
        except (OSError, ValueError) as e:
            error = getattr(e, "winerror", None)
            if error == 1223:
                raise PreflightError(_windows_error("Windows administrator approval was cancelled", error), code="E118",
                                     action="choose Set up USB driver again when ready") from e
            if error in (5, 1326, 1385, 1314):
                raise PreflightError(_windows_error("Windows administrator credentials are required", error), code="E119",
                                     action="ask an administrator to approve driver setup") from e
            raise PreflightError(_windows_error("Driver setup could not complete", error), code="E126",
                                 action="setup may still be running; check the driver before retrying") from e
        if result["reboot_required"]:
            message = (_HELPER_MESSAGES[result["status"]][result["reason"]]
                       if result["status"] == "installed_status_unknown" else "Driver setup needs a Windows restart")
            return {"reboot_required": True, "message": _windows_error(message, result["error"]) + ". "
                    "Restart Windows, reconnect the Disc in mask-ROM mode, then check USB access. "
                    "No flash has started."}
        if result["status"] not in ("installed", "already_bound"):
            code = "E127" if result["reason"] == "unfamiliar_driver" else "E119" if result["status"] == "elevation_required" else "E126"
            raise PreflightError(_windows_error(_HELPER_MESSAGES[result["status"]][result["reason"]], result["error"]),
                                 code=code, action="check the USB driver state before trying again")
    def same_target(result):
        target = require_target(result)
        if (result["count"] != 1 or len(result["present"]) != 1 or not _known_driver(target)
                or target["instance_id"].upper() != row["instance_id"].upper()
                or not row["location_paths"] or target["location_paths"] != row["location_paths"]):
            raise PreflightError("The USB device or driver changed during setup", code="E126",
                                 action="reconnect only the Disc and check again")
    bundle.verify_windows_flash_bundle()
    tool = bundle.native("usbboot")
    env = bundle.native_env(tool)
    # Inventories still check attachment and driver state around access. Only the
    # native opened-path identity can exclude an A -> B -> A swap between them.
    same_target(driver_inventory())
    try:
        access = diagnostic(tool, lambda args, **kw: subprocess.run(args, env=env, **kw),
                            require_identity=True)
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        raise PreflightError(_windows_error("WinUSB is bound but USB access could not be proved",
                                           getattr(e, "winerror", None)), code="E128",
                             action="close other USB tools, reconnect the Disc and check USB access again") from e
    if access["instance_id"].upper() != row["instance_id"].upper():
        raise PreflightError("USB access checked a different device", code="E126",
                             action="reconnect only the Disc and check again")
    after = driver_inventory()
    same_target(after)
    report(driver_description(after))
    from .flasher import flash_eligible
    return {"message": "USB open, claim and control succeeded. No flash has started. "
                       + ("The Disc is ready to install." if flash_eligible() else
                          "This build cannot flash on Windows.")}


if __name__ == "__main__":
    print(json.dumps(inventory(), sort_keys=True))
