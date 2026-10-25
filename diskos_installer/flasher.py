"""Mask-ROM USB flashing - Python wrapper around the PROVEN native binaries
(usbboot + the my_write6 DRAM NAND writer + the X2000 SPL). We do NOT reimplement
the flashing protocol; we orchestrate it, watch it, and interpret the result.

Faithful port of flash_diskos.sh, with the python helper snippets (poison blob,
debug-struct parse) folded in natively, plus honest progress and fail-closed
result interpretation.
"""

import os
import ctypes
import json
import secrets
import shutil
import signal
import struct
import sys
import subprocess
import tempfile
from contextlib import contextmanager
from contextvars import ContextVar

from . import _build_capabilities, basegate, bundle, flash_child, platform_probe, windows_usb
from .reporter import CLIReporter
from .errors import FlashError, DEVICE_RESULT_CODES, WAIT_THEN_RECOVER, RECOVERABLE


class BaseRefused(FlashError):
    """The writer's base gate refused BEFORE unlocking/erasing anything: the Disc is untouched. `.verdict` holds the
    parsed gate verdict (basegate.read_verdict), incl. the base the Disc is really on when it is a known one."""
    code = "E320"

    def __init__(self, message, verdict, **kw):
        super().__init__(message, **kw)
        self.verdict = verdict


# The ONLY writer this installer will run: flash/my_write6.c built by flash/build_nand.sh (reproducible, byte-exact).
# Identified by its exact SHA-256 - not by a code offset - so an older writer that would ignore the base plan can
# never be started. Its descriptor (magic "DGWD") must also agree on protocol, variant and capacity.
WRITER_NAME = "my_write6_dram.bin"
WRITER_SHA256 = "105f95bbd560f8f00596f55ca4de0b99750e7b29e0030225efbaaa24578b3a0d"
PLAN_ADDR = "0xa0b00000"

IMG_SIZE = 100663296
SQUASH_MAGIC = b"hsqs"

# usbboot waits FLASH_WAIT seconds for the writer, then reads the DONE marker back. A
# missing DONE means UNKNOWN: the Disc may still be writing. Leave it powered and connected
# for the recovery interval before power-cycling; never automatically retry an uncertain write.
# DISKOS_FLASH_WAIT sets the fixed wait; this path does not poll for early completion.
# my_write6 first hashes the ~96 MiB image and ~25 MiB of base partitions from uncached DRAM (time not yet measured on
# a device), so the wait has 3 min more than my_write5's 15.
FLASH_WAIT = os.environ.get("DISKOS_FLASH_WAIT", "1080")   # 18 min
_FLASH_WAIT_SECS = int(FLASH_WAIT) if FLASH_WAIT.isdigit() else 1080

RESULT_NAMES = {code: name for code, (_fcode, name) in DEVICE_RESULT_CODES.items()}

FLASH_EXPECT_SECS = 20 * 60   # ~20 min expected (the base check + image hash add time; not yet measured)
# Hard ceiling for the whole usbboot invocation: the writer --wait plus margin for the image
# download + result readback; then treat a still-running usbboot as a hung/reset device and
# terminate it (E303) rather than blocking forever.
FLASH_HARD_TIMEOUT_SECS = _FLASH_WAIT_SECS + 15 * 60   # writer wait + 15 min margin


_WINDOWS_DEV_FLASH = ContextVar("windows_dev_flash", default=False)


def flash_eligible(*, dev_requested=None):
    """Shared host admission; the image, USB and writer gates remain separate."""
    if platform_probe.host()[0] != "windows":
        return platform_probe.is_supported()
    if getattr(sys, "frozen", False):
        return (_build_capabilities.WINDOWS_FLASH is True
                and platform_probe.windows_native_runtime())
    requested = _WINDOWS_DEV_FLASH.get() if dev_requested is None else dev_requested
    return (requested is True and os.environ.get("DISKOS_WINDOWS_DEV_FLASH") == "1"
            and platform_probe.windows_dev_runtime())


@contextmanager
def windows_dev_flash(requested=False):
    """Scope source opt-ins; frozen admission comes only from the artifact."""
    if requested and (platform_probe.host()[0] != "windows"
                      or not flash_eligible(dev_requested=requested)):
        raise FlashError("development Windows flash requires native x64 Windows CPython "
                         "and DISKOS_WINDOWS_DEV_FLASH=1; frozen, MSYS/Cygwin, ARM and "
                         "32-bit runtimes are refused", code="E101",
                         action="use native x64 Windows CPython and --windows-dev-flash "
                                "only for an owner-supervised development install")
    token = _WINDOWS_DEV_FLASH.set(requested is True and not getattr(sys, "frozen", False))
    try:
        yield
    finally:
        _WINDOWS_DEV_FLASH.reset(token)


class _SuspendedProcess:
    """Keep CreateProcess's primary thread handle until assignment is complete."""
    def __init__(self, argv, stdin, creationflags, stdout=None, stderr=None, env=None):
        import _winapi as win
        import msvcrt
        self.win, self.argv = win, argv
        self._handle = self._thread = None
        self.stdin = None
        read, write = win.CreatePipe(None, 0)
        handles = [read, write]
        try:
            current = win.GetCurrentProcess()
            def inherited(handle):
                copy = win.DuplicateHandle(current, handle, current, 0, True,
                                           win.DUPLICATE_SAME_ACCESS)
                handles.append(copy)
                return copy
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = win.STARTF_USESTDHANDLES
            startup.hStdInput = inherited(read)
            startup.hStdOutput = inherited(msvcrt.get_osfhandle(
                (stdout if stdout is not None else sys.stdout).fileno()))
            startup.hStdError = startup.hStdOutput if stderr == subprocess.STDOUT else inherited(
                msvcrt.get_osfhandle((stderr if stderr is not None else sys.stderr).fileno()))
            startup.lpAttributeList = {"handle_list": list({startup.hStdInput,
                startup.hStdOutput, startup.hStdError})}
            self._handle, self._thread, self.pid, _ = win.CreateProcess(
                argv[0], subprocess.list2cmdline(argv), None, None, True,
                creationflags, env, None, startup)
            self.stdin = os.fdopen(msvcrt.open_osfhandle(write, os.O_WRONLY | os.O_BINARY), "wb")
            handles.remove(write)  # fd now owns it; not inheritable
        except BaseException:
            if self._handle is not None:
                try:
                    self.kill()
                    self.wait(timeout=30)
                finally:
                    self.close_handles()
            raise
        finally:
            for handle in handles:
                win.CloseHandle(handle)

    def wait(self, timeout=None):
        ms = self.win.INFINITE if timeout is None else max(0, int(timeout * 1000))
        result = self.win.WaitForSingleObject(self._handle, ms)
        if result == self.win.WAIT_TIMEOUT:
            raise subprocess.TimeoutExpired(self.argv, timeout)
        if result != 0:
            raise OSError("could not wait for Windows child")
        return self.win.GetExitCodeProcess(self._handle)

    def kill(self):
        self.win.TerminateProcess(self._handle, 1)

    def close_handles(self):
        for name in ("_thread", "_handle"):
            handle = getattr(self, name)
            if handle is not None:
                self.win.CloseHandle(handle)
                setattr(self, name, None)


class _WindowsChild:
    """An unnamed parent-owned job; no child code runs before assignment."""
    def __init__(self, cmd, target_ver=None, api=None, popen=None, **kwargs):
        if not flash_eligible():
            raise FlashError(platform_probe.host_status(), code="E101")
        nonce = secrets.token_hex(32)
        argv = flash_child.launch_argv(nonce)
        request = dict(nonce=nonce, parent=os.getpid(), dev_requested=_WINDOWS_DEV_FLASH.get(),
                       cmd=flash_child.validate_command(cmd), target=target_ver)
        self.api = api or flash_child.windows_api()
        self.job = self.api.CreateJobObjectW(None, None)
        self.proc = None
        if not self.job:
            raise OSError("could not create child job")
        try:
            if not self.api.SetHandleInformation(self.job, 1, 0):
                raise OSError("could not make parent job non-inheritable")
            limits = flash_child.JobLimits()
            limits.basic.flags = 0x2000
            if not self.api.SetInformationJobObject(self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
                raise OSError("could not set kill-on-close job limit")
            self.proc = (popen or _SuspendedProcess)(argv, stdin=subprocess.PIPE,
                                                    creationflags=0x204, **kwargs)
            if not self.api.AssignProcessToJobObject(self.job, int(self.proc._handle)):
                raise OSError("could not assign child to job")
            if self.api.ResumeThread(int(self.proc._thread)) != 1:
                raise OSError("could not resume assigned child")
            self.proc.stdin.write(b"G" + json.dumps(request, ensure_ascii=True).encode("ascii"))
            self.proc.stdin.close()
        except BaseException:
            try:
                if self.proc is not None:
                    self.proc.kill()
                    self.proc.wait(timeout=30)
            finally:
                try:
                    if self.proc is not None and self.proc.stdin is not None:
                        self.proc.stdin.close()
                finally:
                    self.close()
            raise

    def wait(self, timeout=None):
        return self.proc.wait(timeout=timeout)

    def kill(self):
        if not self.api.TerminateJobObject(self.job, 1):
            raise OSError("could not terminate child job")

    def close(self):
        if self.job:
            self.api.CloseHandle(self.job)
            self.job = None
        if self.proc is not None:
            self.proc.close_handles()

def _start_child(cmd, **kwargs):
    if platform_probe.host()[0] == "windows":
        return _WindowsChild(cmd, **kwargs)
    return subprocess.Popen(cmd, start_new_session=True, **kwargs)

def _kill_child(proc):
    if isinstance(proc, _WindowsChild):
        proc.kill()
    else:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except OSError:
            proc.kill()
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        pass

def _probe_helpers():
    """Confirm every bundled native tool can actually execute (and load its libs)
    BEFORE the destructive gate. Catches a noexec temp mount / missing dylibs while
    the device is still untouched. Non-zero exit is fine; an OSError is not."""
    probes = [(bundle.native("usbboot"), ["--help"]),
              (bundle.native("mksquashfs"), ["-version"]),
              (bundle.native("unsquashfs"), ["-version"])]
    for path, args in probes:
        try:
            subprocess.run([path] + args, env=bundle.native_env(path),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=15)
        except OSError as e:
            raise FlashError(
                f"native tool '{os.path.basename(path)}' at '{path}' cannot execute ({e})",
                code="E103",
                action="the tool's directory may be on a noexec mount, or a required library is "
                       "missing; move the installer to an exec-capable filesystem, install "
                       "squashfs-tools, or rebuild the bundled tools, and retry")
        except subprocess.TimeoutExpired:
            pass   # it started (that's all we needed to prove)


def preflight(image_path, rep=None):
    """Validate the image and that exactly one device is in mask-ROM. Fail closed."""
    windows = platform_probe.host()[0] == "windows"
    if not flash_eligible():
        raise FlashError(platform_probe.host_status(), code="E101")
    rep = rep or CLIReporter()
    if not os.path.exists(image_path):
        raise FlashError(f"image not found: {image_path}", code="E120")
    sz = os.path.getsize(image_path)
    if sz != IMG_SIZE:
        raise FlashError(f"wrong image size {sz} (must be {IMG_SIZE})", code="E121",
                         action="use a diskOS image built by this installer")
    with open(image_path, "rb") as f:
        if f.read(4) != SQUASH_MAGIC:
            raise FlashError("image is not a squashfs (bad magic) - not a diskOS/stock image", code="E122")

    if windows:
        bundle.verify_windows_flash_bundle()
    _probe_helpers()   # every bundled tool must actually execute (catches noexec /tmp, missing libs)

    if windows:
        from .errors import PreflightError
        try:
            windows_usb.require_target(windows_usb.inventory())
        except PreflightError as e:
            raise FlashError(e.message, code=e.code, action=e.action) from e
        except OSError as e:
            raise FlashError(f"Windows USB inventory failed: {e}", code="E112",
                             action="fix USB enumeration and repeat check A") from e
        tool = bundle.native("usbboot")
        try:
            def run(cmd, **kw):
                return subprocess.run(cmd, env=bundle.native_env(tool), **kw)
            raw = windows_usb.diagnostic(tool, runner=run)
            if raw["length"] != 5 or raw["hex"] != "5832303030":
                raise ValueError("mask-ROM reply is not the validated X2000 identity")
        except (OSError, ValueError, subprocess.TimeoutExpired) as e:
            raise FlashError(f"native mask-ROM access check failed: {e}", code="E115",
                             action="power-cycle into fresh mask-ROM and repeat check A; "
                                    "check WinUSB binding and USB connection") from e
        return

    n = platform_probe.maskrom_count()
    if n == 0:
        raise FlashError(
            "no device in mask-ROM mode", code="E110",
            action="power the device OFF, hold Volume-Down, plug in USB (screen stays "
                   "black), then retry")
    if n > 1:
        raise FlashError(f"{n} devices in mask-ROM mode - need exactly 1", code="E111",
                         action="unplug the other Ingenic devices")
    # Seeing the device is not enough: usbboot must OPEN it (GitHub #10 - counted fine, then E301). Refuse with the
    # exact fix instead. An unknown result (not Linux, unexpected error) is left to usbboot, which reports itself.
    access, where = platform_probe.maskrom_access()
    if access == "denied":
        raise FlashError(f"the device is in mask-ROM mode but this user cannot open it ({where}) - USB permissions",
                         code="E113", action=platform_probe.USB_ACCESS_FIX)
    if n < 0:
        # FAIL CLOSED: we could not prove exactly one device (no libusb backend or a
        # permission error). Never cross the destructive gate on an unproven count.
        raise FlashError(
            "cannot confirm exactly one device (USB enumeration failed - missing libusb "
            "backend or insufficient USB permissions)", code="E112",
            action="refusing to flash on an unproven device count; fix USB access and retry")


def _parse_debug(dbg_path):
    """Parse the 1KB little-endian debug readback (256 x uint32). Returns a dict.
    Mirrors the field offsets in flash_diskos.sh."""
    with open(dbg_path, "rb") as f:
        raw = f.read(1024)
    if len(raw) < 1024:
        raise FlashError(f"short debug readback ({len(raw)} bytes) - flash result UNKNOWN", code="E302",
                         action=WAIT_THEN_RECOVER)
    w = struct.unpack("<256I", raw)
    nbad = w[20]
    return {
        "magic": w[0],
        "start_block": w[5],
        "nlogblocks": w[6],   # writer's compiled logical-block capacity - MUST cover the whole image
        "done": w[9],
        "skipped": w[10],
        "result": w[16],
        "retried": w[17],
        "worst_retries": w[18],
        "bad_found": nbad,
        "bad_list": [w[40 + i] for i in range(min(nbad, 64))],
    }


def _writer_descriptor(writer_path):
    """(nlogblocks, blob_schema) of the pinned production writer, or None if the file is not exactly it."""
    import hashlib
    try:
        data = open(writer_path, "rb").read()
    except OSError:
        return None
    if hashlib.sha256(data).hexdigest() != WRITER_SHA256:
        return None
    k = data.find(struct.pack("<I", 0x44574744))              # "DGWD" descriptor
    if k < 0 or k + 32 > len(data):
        return None
    magic, tool, variant, schema, start, nlog, blob, dbg = struct.unpack("<8I", data[k:k + 32])
    if tool != 6 or variant != 1 or schema != basegate.BLOB_SCHEMA or start != 80 or blob != 0xa0b00000:
        return None
    return nlog, schema


def flash(image_path, target_ver, log_path=None, rep=None):
    """Flash `image_path` (built from firmware `target_ver`'s rootfs) via mask-ROM. The writer first proves ON THE
    DEVICE that the Disc's kernel + recovery are exactly `target_ver`'s (basegate) and that the image in its memory is
    this one; only then does it unlock and write. Returns the parsed debug dict on SUCCESS; raises BaseRefused if the
    gate refused (nothing written), FlashError otherwise (fail-closed)."""
    rep = rep or CLIReporter()
    launched = [False]
    completed = [False]
    try:
        return _flash(image_path, target_ver, log_path, rep, launched, completed)
    except FlashError as e:
        if not launched[0]:
            e.pre_launch = True                          # usbboot never started: the Disc is untouched
        raise
    except Exception as e:
        if launched[0]:
            if completed[0]:
                raise FlashError(f"flash writer finished; host/reporting error: {e}",
                                 code="E301", action=RECOVERABLE) from e
            raise FlashError(f"flash result UNKNOWN: {e}", code="E301", action=WAIT_THEN_RECOVER) from e
        # an OS/tool error before usbboot started (temp dir, plan/log file, Popen itself): the Disc is untouched
        err = FlashError(f"could not start the flash: {e}", code="E304",
                         action="nothing was sent to the Disc; fix the problem above and retry")
        err.pre_launch = True
        raise err from e


def _flash_command(usbboot, spl, writer, image_path, plan, poison, dbg):
    return [
        usbboot, "-v", "--cpu", "x2000", "--stage1", spl, "--wait", "2",
        "--addr", "0xa0c00000", "--download", writer,
        "--addr", "0xa1000000", "--download", image_path,
        "--addr", PLAN_ADDR, "--download", plan,
        "--addr", "0xa0a00000", "--download", poison,
        "--start1", "0xa0c00030", "--wait", FLASH_WAIT,
        "--addr", "0xa0a00000", "--length", "0x400", "--upload", dbg,
    ]


def _flash(image_path, target_ver, log_path, rep, launched, completed):
    preflight(image_path, rep)
    image_path = os.path.abspath(image_path)

    usbboot = bundle.native("usbboot")
    writer = bundle.native(WRITER_NAME)
    spl = bundle.native("disc_spl_lpddr3.bin")

    # PRE-WRITE capacity gate: refuse a truncating writer before it touches the NAND (the post-write
    # E311 check is too late - the image is already partially programmed). This is the 580-vs-768 bug.
    need_blocks = IMG_SIZE // (128 * 1024)
    desc = _writer_descriptor(writer)
    if desc is None:
        raise FlashError(
            "the NAND writer is not the exact build this installer was made with - refusing to run it "
            "(an unknown writer might not check the Disc's firmware base)", code="E123",
            action="use a diskOS installer bundle built by this project")
    wcap = desc[0]
    if target_ver not in basegate.CATALOGUE:
        raise FlashError(f"no known firmware base for V{target_ver} - refusing to flash", code="E125",
                         action="this firmware version is not supported by this installer")
    if wcap != need_blocks:
        raise FlashError(
            f"NAND writer covers {wcap} x 128 KB blocks but the image is exactly {need_blocks} - the writer "
            f"and the image must match exactly (nothing has been written).", code="E124",
            action=f"rebuild my_write6 with NLOGBLOCKS={need_blocks} (flash/build_nand.sh) and the bundle")
    rep.log(f"writer capacity (pre-write): {wcap} blocks (image needs {need_blocks})")

    tmp = tempfile.mkdtemp(prefix="diskos-flash-")
    poison = os.path.join(tmp, "poison.bin")
    dbg = os.path.join(tmp, "dbg.bin")
    with open(poison, "wb") as f:
        f.write(b"\xee" * 128)
    plan = os.path.join(tmp, "plan.bin")
    with open(plan, "wb") as f:
        f.write(basegate.build_plan(target_ver, image_path))     # binds the base AND this exact image
    rep.log(f"base required on the Disc: {basegate.CATALOGUE[target_ver]['name']}")

    cmd = _flash_command(usbboot, spl, writer, image_path, plan, poison, dbg)

    rep.phase("Flashing (mask-ROM) - about 20 minutes", destructive=True)
    rep.warning("Do NOT disconnect the device or let the host sleep during the flash.")

    logf = open(log_path, "w") if log_path else open(os.path.join(tmp, "flash.log"), "w")
    rep.indeterminate(True, note="flashing (scan -> erase -> program -> verify -> retry)",
                      expect_secs=FLASH_EXPECT_SECS)
    proc = None
    try:
        try:
            with _sleep_inhibited(rep):
                # own session so a GUI/parent crash can't SIGPIPE the flasher; output goes
                # to a file (never a pipe) so a closed reader can't deadlock or kill it.
                child_kw = {"target_ver": target_ver} if platform_probe.host()[0] == "windows" else {}
                proc = _start_child(cmd, **child_kw, stdout=logf, stderr=subprocess.STDOUT,
                                    env=bundle.native_env(cmd[0]))
                launched[0] = True                       # usbboot is running: from here on the Disc may be written

                def _kill_flasher():
                    # The flasher runs in its OWN session (start_new_session) so a parent SIGPIPE
                    # can't kill it - but that also means it SURVIVES us. Kill the host process group
                    # and reap without blocking. NB this stops only the HOST side: once usbboot has
                    # started the writer, the writer keeps running ON THE DISC regardless.
                    _kill_child(proc)

                try:
                    # A3: bound the wait. If the device resets/re-enumerates mid-flash,
                    # usbboot can block on a dead handle forever - never hang the app.
                    rc = proc.wait(timeout=FLASH_HARD_TIMEOUT_SECS)
                except subprocess.TimeoutExpired:
                    _kill_flasher()
                    raise FlashError(
                        f"flash timed out - the device stopped responding after "
                        f"{FLASH_HARD_TIMEOUT_SECS // 60} min (it most likely reset "
                        "mid-flash). Result UNKNOWN.", code="E303", action=WAIT_THEN_RECOVER)
                except BaseException:
                    # Ctrl-C, SIGTERM, a GUI/parent crash, or any error during the wait. Stop the host
                    # side, and say plainly that the Disc may STILL be writing: the writer runs on the
                    # device by itself once started, so unplugging now could leave it half-written.
                    rep.warning("The flash was interrupted on this computer. " + WAIT_THEN_RECOVER)   # first:
                    _kill_flasher()                      # a second Ctrl-C during the reap must not lose the warning
                    raise
        finally:
            if isinstance(proc, _WindowsChild):
                proc.close()
            rep.indeterminate(False)
            logf.close()

        if isinstance(proc, _WindowsChild) and rc == flash_child.VALIDATION_REFUSED:
            error = FlashError(
                "The flash could not start because Windows flash validation failed. "
                "Nothing was sent by the flash transfer.", code="E304",
                action="See Details for the validation failure. Correct the reported problem "
                       "and retry; no flash-recovery wait is needed.")
            error.pre_launch = True
            raise error

        rep.log(f"usbboot exit={rc}")
        if not os.path.exists(dbg):
            raise FlashError(
                "no debug readback produced - flash result UNKNOWN; assume FAILED",
                code="E301", action=WAIT_THEN_RECOVER)

        d = _parse_debug(dbg)
        with open(dbg, "rb") as f:
            raw = f.read(1024)
        verdict = basegate.read_verdict(raw)
        d["gate"] = verdict
        complete = d["magic"] == 0x4006E006 and d["done"] == 0x55555555
        refusal = verdict["result"] in basegate.GATE_CODES and verdict["result"] not in (1, 0x9A7E0B5E)
        if complete and refusal and d["result"] == verdict["result"]:
            # the gate refused: the writer returned before unlocking/erasing anything
            if struct.unpack_from("<I", raw, basegate.G_ERASING * 4)[0] != 0:
                raise FlashError("the writer reported a refusal AFTER starting to erase - flash result UNKNOWN",
                                 code="E321", action=WAIT_THEN_RECOVER)
            completed[0] = True
            rep.log(f"base gate: 0x{verdict['result']:08X} ({verdict['meaning']}); Disc base seen: "
                    f"{verdict['matched_base'] or 'unknown'}")
            raise BaseRefused(basegate.refusal_message(verdict, target_ver), verdict)
        ok = (d["magic"] == 0x4006E006 and d["done"] == 0x55555555
              and d["result"] == 0x600DF10C and verdict["result"] == 1)
        # DONE alone is insufficient: accept only success or a known post-gate abort.
        completed[0] = complete and verdict["result"] == 1 and (
            ok or 0xDEAD0001 <= d["result"] <= 0xDEAD0006)
        # SAFETY: the writer only programs its compiled NLOGBLOCKS (dbg[6]) logical blocks. If that is
        # fewer than the image occupies, the TAIL is silently dropped - and squashfs keeps its
        # inode/directory/fragment tables at the tail, so a truncated image mounts-fails and won't
        # boot, WHILE the device still reports SUCCESS for the part it did write. Require the writer's
        # capacity to cover the whole image. (This is exactly the 580-vs-768 bug; this check catches it.)
        need_blocks = IMG_SIZE // (128 * 1024)
        cap_ok = d["nlogblocks"] == need_blocks
        rep.log(f"scan: bad-blocks-found={d['bad_found']} list={d['bad_list']}")
        rep.log(f"writer capacity: {d['nlogblocks']} blocks (image needs {need_blocks})")
        # B4: dbg[17]/[18] mean retried/worst only on SUCCESS; on the out-of-space /
        # block-write-fail aborts they hold the last phys/logical block; on the other
        # aborts they are 0 and must NOT be shown as "last block" (would be misleading).
        if ok:
            rep.log(f"write: skipped={d['skipped']} retried={d['retried']} "
                    f"worst={d['worst_retries']}")
        elif d["result"] in (0xDEAD0002, 0xDEAD0003):
            rep.log(f"write: skipped={d['skipped']} last-phys-block={d['retried']} "
                    f"last-logical-block={d['worst_retries']}")
        else:
            rep.log(f"write: skipped={d['skipped']}")
        fcode, name = DEVICE_RESULT_CODES.get(d["result"], ("F000", "UNKNOWN")) if complete else ("F000", "UNKNOWN")
        if not ok and "nothing written" in name:
            fcode, name = "F000", "UNKNOWN (unvalidated refusal)"
        rep.log(f"result: 0x{d['result']:08X} [{fcode}] {name}")

        if not ok:
            e = FlashError(
                f"flash result UNKNOWN or FAILED (device result [{fcode}] {name}, "
                f"magic=0x{d['magic']:08X} done=0x{d['done']:08X})",
                code="E310", action=RECOVERABLE if completed[0] else WAIT_THEN_RECOVER)
            e.verdict = verdict                                  # preserve the observed gate report for recovery
            raise e
        if not cap_ok:
            raise FlashError(
                f"writer/image size MISMATCH: the flashing tool programs {d['nlogblocks']} x 128 KB "
                f"blocks but this image needs {need_blocks}. The image was TRUNCATED - the device "
                f"reports success but will not boot. This is a build/tooling defect; do not ship or "
                f"trust this build.", code="E311", action=RECOVERABLE)
        rep.ok("flash verified OK")
        return d
    finally:
        # B3: never leak the per-flash tempdir (poison/dbg/flash.log), but keep the log
        # debuggable: if no external log_path was given, preserve the internal log to a
        # single stable file (overwritten each flash - bounded, not a growing leak).
        internal_log = os.path.join(tmp, "flash.log")
        if not log_path and os.path.exists(internal_log):
            try:
                # Unique, 0600, no symlink-follow: write THROUGH the mkstemp fd (never reopen the path,
                # which could follow a swapped-in symlink and truncate an arbitrary target). Path reported.
                fd, kept = tempfile.mkstemp(prefix="diskos-flash-", suffix=".log")
                with os.fdopen(fd, "wb") as out, open(internal_log, "rb") as src:
                    shutil.copyfileobj(src, out)
                rep.log(f"flash log saved to {kept}")
            except OSError:
                pass
        if platform_probe.host()[0] == "windows" and not completed[0]:
            # Keep the plan (which binds the image hash), readback and host log for recovery.
            rep.log(f"Windows flash evidence retained: {tmp}; do not resume a partial transfer")
        else:
            shutil.rmtree(tmp, ignore_errors=True)


class _sleep_inhibited:
    """Hold sleep inhibition through the flash; Windows fails closed before launch.
    Linux/macOS retain their existing best-effort warning behaviour."""

    def __init__(self, rep=None, host=None, api=None):
        self.rep = rep
        self.host = host
        self.api = api
        self.windows_active = False
        self.macos_assertion = None

    def __enter__(self):
        self.proc = None
        o = self.host or platform_probe.host()[0]
        if o == "macos":
            from .macos_runtime import NoSleep
            self.macos_assertion = NoSleep(self.rep)
            self.macos_assertion.__enter__()
            return self
        try:
            if o == "windows":
                self.api = self.api or bundle._windows_api()
                self.windows_active = bool(self.api.SetThreadExecutionState(0x80000001))
            elif o == "linux":
                import shutil
                if shutil.which("systemd-inhibit"):
                    # keep a long-lived inhibitor process alive; we kill it on exit
                    self.proc = subprocess.Popen(
                        ["systemd-inhibit", "--what=sleep:idle",
                         "--why=diskOS flash in progress", "sleep", "infinity"])
        except Exception:
            self.proc = None
        if o == "windows" and not self.windows_active:
            raise FlashError("could not hold the Windows system sleep request", code="E116",
                             action="fix Windows power-request access before retrying; "
                                    "nothing was sent to the Disc")
        if self.proc is None and not self.windows_active and self.rep is not None:
            self.rep.warning(
                "could not auto-inhibit system sleep on this host - make sure your "
                "computer will NOT sleep/suspend for the next ~25 minutes (a suspend "
                "mid-flash aborts it; the device stays recoverable).")
        return self

    def __exit__(self, *exc):
        if self.macos_assertion is not None:
            return self.macos_assertion.__exit__(*exc)
        if self.windows_active:
            if not self.api.SetThreadExecutionState(0x80000000) and self.rep:
                self.rep.warning("could not release the Windows sleep request")
            self.windows_active = False
        if self.proc:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=5)   # reap so no zombie / stray 'sleep infinity' lingers
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass
        return False
