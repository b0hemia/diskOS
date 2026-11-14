# SPDX-License-Identifier: GPL-3.0-or-later
"""Supervised firmware acquisition: the download runs in a private helper process.

DNS resolution and TCP connects cannot be interrupted from a thread, so the GUI
worker never downloads itself. `start()` launches a helper (this file, run with
the private argument MODE, distinct from the flash-child entry) that calls
firmware_download.download(), and an `Acquisition` supervises it.

Protocol (UTF-8 JSON lines). Parent -> helper stdin: one config line (it carries
the parent-reserved partial path), later the line "cancel". Helper -> parent
stdout: {op, ev: partial|progress|heartbeat|result, ...}. Heartbeats prove the
helper is alive during network waits that make no byte progress.
Every line carries the operation id; lines for any other id are dropped, and
nothing is delivered after cancel() or after the result, so a stale request
cannot overwrite a newer one.

Process model: the helper is its own process group (POSIX session / Windows
process group) and only that process and group are ever signalled, never a flash
child. Cancel first asks the helper to stop (it then removes its .part and skips
promotion), and after GRACE kills it. The supervisor also kills it at the overall
deadline (+ DEADLINE_SLACK) or when it is silent for `stall` seconds. If the
parent dies the helper sees EOF on stdin and exits; live helpers are also killed
at interpreter exit. The helper never takes the run lock: it borrows the held
one from the parent (inherited fd on POSIX).
"""
import atexit
import itertools
import json
import os
import queue
import re
import signal
import subprocess
import sys
import threading
import time
import weakref

MODE = "--_diskos-fetch-helper"
TIMEOUT = 15.0           # socket-operation timeout (seconds)
DEADLINE = 1800.0        # overall acquisition deadline (seconds)
GRACE = 0.5              # wait for a polite exit after cancel before killing
DEADLINE_SLACK = 2.0
ORPHAN_EXIT = 3.0        # helper: seconds after the parent vanished before it exits
PROGRESS_INTERVAL = 0.1
_MAX_LINE = 1 << 16
_CODE = re.compile(r'E\d{3}\Z')
_ops = itertools.count(1)
_live = weakref.WeakSet()


def helper_argv():
    if getattr(sys, "frozen", False):
        return [sys.executable, MODE]
    return [sys.executable, "-I", os.path.abspath(__file__), MODE]


# ---------------------------------------------------------------- helper side
def _lines(fd):
    buf = b""
    while True:
        try:
            data = os.read(fd, 4096)
        except OSError:
            data = b""
        if not data:
            return
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line.decode("utf-8", "replace")
        if len(buf) > _MAX_LINE:
            return


def _borrowed_lock(cfg):
    from diskos_installer.__main__ import _Lock
    lock = _Lock(windows=bool(cfg["win"]))
    if lock.windows:
        lock.handle = 1  # the parent holds the machine-wide mutex for us
    else:
        lock.path, lock.fd = cfg["lock_path"], int(cfg["lock_fd"])
    return lock


def helper_main(argv=None, stdin_fd=None, out=None, exit_fn=os._exit):
    """Run one download for the parent; exits 2 on a bad launch, never parses commands."""
    if list(sys.argv[1:] if argv is None else argv) != [MODE]:
        return 2
    from diskos_installer import firmware_catalogue, firmware_download
    out = out or sys.stdout
    lines = _lines(sys.stdin.fileno() if stdin_fd is None else stdin_fd)

    emit_lock = threading.Lock()

    def emit(**ev):
        with emit_lock:
            try:
                out.write(json.dumps(ev, separators=(",", ":")) + "\n")
                out.flush()
            except (OSError, ValueError):
                pass
    try:
        cfg = json.loads(next(lines, ""))
        op, version = cfg["op"], cfg["version"]
        timeout, deadline = float(cfg["timeout"]), float(cfg["deadline"])
        if (type(op) is not int or version not in firmware_catalogue.CATALOGUE
                or not 0 < timeout <= 120 or not 0 < deadline <= 7200):
            return 2
        reserved = cfg.get("partial")
        if reserved is not None and not isinstance(reserved, str):
            return 2
        lock = _borrowed_lock(cfg)
    except (ValueError, KeyError, TypeError, OverflowError):
        return 2
    cancel, done = threading.Event(), threading.Event()
    partial = [reserved] if reserved else []

    def die():  # the parent vanished while we were blocked: leave nothing behind
        if not done.is_set():
            for path in partial:
                try:
                    os.unlink(path)
                except OSError:
                    pass
            exit_fn(3)

    def watch():
        for line in lines:
            if line.strip() == "cancel":
                cancel.set()
        cancel.set()
        timer = threading.Timer(ORPHAN_EXIT, die)
        timer.daemon = True
        timer.start()
    threading.Thread(target=watch, daemon=True).start()
    real = firmware_catalogue.new_partial
    reserved = [reserved] if reserved else []

    def heartbeat():
        while not done.wait(timeout / 2):
            emit(op=op, ev="heartbeat")
    threading.Thread(target=heartbeat, daemon=True).start()

    def new_partial(*args, **kwargs):
        path = reserved.pop() if reserved else real(*args, **kwargs)  # the parent made the first one
        partial.append(path)
        emit(op=op, ev="partial", path=path)
        return path
    firmware_catalogue.new_partial = new_partial
    try:
        path = firmware_download.download(
            version, lock, progress=lambda d, t: emit(op=op, ev="progress", done=d, total=t),
            cancel=cancel.is_set, timeout=timeout, deadline=deadline)
        emit(op=op, ev="result", ok=True, path=path)
    except Exception as exc:
        code = getattr(exc, "code", "")
        known = hasattr(exc, "message")
        emit(op=op, ev="result", ok=False, code=code if known and _CODE.match(code) else "E160",
             message=exc.message if known else "The download helper failed (%s)" % type(exc).__name__,
             action=getattr(exc, "action", None), status=getattr(exc, "status", None))
    finally:
        done.set()
        firmware_catalogue.new_partial = real
    return 0


# ------------------------------------------------------------ supervisor side
def _kill_all():
    for acq in list(_live):
        acq._kill()


atexit.register(_kill_all)


def _typed(msg):
    from . import firmware_download as dl
    classes = {c.code: c for c in vars(dl).values()
               if isinstance(c, type) and issubclass(c, dl.DownloadError)}
    code, text = msg.get("code"), str(msg.get("message") or "Download failed")[:300]
    action = msg.get("action")
    action = str(action)[:200] if action else None
    cls = classes.get(code)
    if cls is dl.HttpStatusError and type(msg.get("status")) is int:
        return cls(msg["status"])
    if cls is None or cls is dl.HttpStatusError:
        return dl.DownloadError(text, code=code if isinstance(code, str) and _CODE.match(code) else None,
                                action=action)
    return cls(text, action=action)


class Acquisition:
    """One supervised download. wait() returns the cache path or raises a DownloadError."""

    def __init__(self, version, on_event, op_id, run_lock, timeout, deadline, stall, argv, interval):
        from . import firmware_catalogue
        self.op_id, self._cb, self._interval = op_id, on_event, interval
        self._deadline, self._stall = deadline, stall
        self._cache_dir = os.path.dirname(firmware_catalogue._cache_path(firmware_catalogue._pin(version)))
        self._gate = threading.RLock()
        self._cancel, self._closed = threading.Event(), False
        self._life = threading.Lock()  # sole owner of signalling and reaping the helper
        self._partials, self._path, self._error = [], None, None
        cfg = {"op": op_id, "version": version, "timeout": timeout, "deadline": deadline,
               "win": bool(run_lock.windows), "lock_path": run_lock.path, "lock_fd": run_lock.fd}
        kw = {}
        if os.name == "nt":
            kw["creationflags"] = 0x08000200  # CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
        else:
            kw.update(start_new_session=True, pass_fds=() if run_lock.fd is None else (run_lock.fd,))
        try:
            # Reserved here so cancellation never depends on the helper's announcement.
            cfg["partial"] = firmware_catalogue.new_partial(version, run_lock)
            self._partials.append(cfg["partial"])
            self._proc = subprocess.Popen(
                argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, **kw)
            self._proc.stdin.write((json.dumps(cfg) + "\n").encode())
            self._proc.stdin.flush()
        except OSError as exc:
            self._kill()
            self._remove_partials()
            from .firmware_download import DownloadError
            raise DownloadError("Could not start the download helper: %s" % exc.strerror) from exc
        _live.add(self)
        self._queue = queue.Queue()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()
        self._thread = threading.Thread(target=self._supervise, daemon=True)
        self._thread.start()

    def cancel(self):
        """Stop this acquisition only. No event is delivered after this returns."""
        with self._gate:
            self._cancel.set()

    def wait(self, timeout=None):
        self._thread.join(timeout)
        if self._thread.is_alive():
            raise TimeoutError("acquisition still running")
        if self._error is not None:
            raise self._error
        return self._path

    # -- internals
    def _read(self):
        stream = self._proc.stdout
        try:
            while True:
                line = stream.readline(_MAX_LINE + 1)
                if not line:
                    break
                self._queue.put(line if line.endswith(b"\n") and len(line) <= _MAX_LINE else False)
                if len(line) > _MAX_LINE:
                    break
        except (OSError, ValueError):
            pass
        self._queue.put(None)

    def _emit(self, *event):
        with self._gate:
            if self._closed or self._cancel.is_set():
                return
            try:
                self._cb(self.op_id, *event)
            except Exception:
                pass

    def _supervise(self):
        from . import firmware_download as dl
        start = last = time.monotonic()
        seen = sent = -1
        verifying = False
        try:
            while self._error is None and self._path is None:
                now = time.monotonic()
                if self._cancel.is_set():
                    self._error = dl.Cancelled("Firmware download cancelled")
                elif now - start >= self._deadline + DEADLINE_SLACK:
                    self._error = dl.DeadlineExceeded("The download took too long",
                                                      action="retry, or use the browser download")
                elif now - last >= self._stall:
                    self._error = dl.Timeout("The connection timed out", action="check the connection and retry")
                if self._error:
                    break
                try:
                    raw = self._queue.get(timeout=0.05)
                except queue.Empty:
                    continue
                if raw is None:
                    code = self._proc.returncode  # diagnostic only; never reaps
                    self._error = dl.DownloadError("The download helper stopped unexpectedly%s" % (
                        "" if code is None else " (exit %s)" % code))
                    break
                try:
                    msg = json.loads(raw) if raw else None
                    if not isinstance(msg, dict) or type(msg.get("op")) is not int:
                        raise ValueError
                except ValueError:
                    self._error = dl.DownloadError("The download helper sent an invalid reply")
                    break
                if msg["op"] != self.op_id:
                    continue  # stale: belongs to another request
                ev = msg.get("ev")
                if ev in ("partial", "progress", "heartbeat", "result"):
                    last = time.monotonic()  # only this operation's recognised events prove liveness
                if ev == "partial" and isinstance(msg.get("path"), str):
                    self._partials.append(msg["path"])
                elif ev == "heartbeat":
                    pass
                elif ev == "progress":
                    done, total = msg.get("done"), msg.get("total")
                    if (type(done) is int and type(total) is int and 0 <= done <= total and done >= seen
                            and (done == total or now - sent >= self._interval or seen < 0)):
                        seen, sent = done, now
                        self._emit("progress", done, total)
                        if done == total and not verifying:
                            verifying = True
                            self._emit("verifying", done, total)
                elif ev == "result":
                    if msg.get("ok") is True and isinstance(msg.get("path"), str):
                        self._path = msg["path"]
                    else:
                        self._error = _typed(msg)
        finally:
            self._finish()

    def _finish(self):
        from . import firmware_download as dl
        with self._gate:
            self._closed = True
            cancelled = self._cancel.is_set()
        if cancelled:
            self._path, self._error = None, dl.Cancelled("Firmware download cancelled")
        if self._error is None and self._path is None:
            self._error = dl.DownloadError("The download helper stopped unexpectedly")
        try:
            if self._error is not None:
                try:
                    self._proc.stdin.write(b"cancel\n")
                    self._proc.stdin.flush()
                except (OSError, ValueError):
                    pass
            # A polite helper exits and closes stdout; wait for that without reaping it.
            self._reader.join(GRACE)
        finally:
            self._kill()
            self._reader.join(1.0)
            streams = [self._proc.stdin]
            if not self._reader.is_alive():  # close() would block behind a pending readline()
                streams.append(self._proc.stdout)
            for stream in streams:
                try:
                    stream.close()
                except (OSError, ValueError):
                    pass
            _live.discard(self)
            self._remove_partials()

    def _kill(self):
        """Kill the helper's whole group, then reap it. The leader stays unreaped until
        its group is dead, so the pid cannot be reused while it is signalled."""
        proc = getattr(self, "_proc", None)
        if proc is None:
            return
        with self._life:
            if proc.returncode is not None:
                return
            try:
                if os.name == "nt":
                    subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
                    proc.kill()
                else:
                    os.killpg(proc.pid, signal.SIGKILL)  # the helper's own session only
                proc.wait(5)
            except (OSError, subprocess.SubprocessError):
                pass

    def _remove_partials(self):
        for path in self._partials:
            if os.path.dirname(os.path.abspath(path)) == os.path.abspath(self._cache_dir) \
                    and path.endswith(".part"):
                try:
                    os.unlink(path)
                except OSError:
                    pass


def start(version, on_event=None, run_lock=None, op_id=None, timeout=TIMEOUT, deadline=DEADLINE,
          stall=None, argv=None, interval=PROGRESS_INTERVAL):
    """Start a supervised download of `version`; return the Acquisition.

    on_event(op_id, kind, done, total) with kind "progress" (throttled, bytes
    against the pinned size) or "verifying" (the separate verification stage).
    `run_lock` is the held installer run lock, borrowed by the helper.
    """
    if run_lock is None or (run_lock.fd is None and run_lock.handle is None):
        raise RuntimeError("A held installer run lock is required")
    return Acquisition(version, on_event or (lambda *a: None), next(_ops) if op_id is None else op_id,
                       run_lock, timeout, deadline, 2 * timeout if stall is None else stall,
                       helper_argv() if argv is None else list(argv), interval)


if __name__ == "__main__":
    # Isolated source launch: import only this installer's package, never cwd/PATH.
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    sys.exit(helper_main())
