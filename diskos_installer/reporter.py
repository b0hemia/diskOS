# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Reporter interface - decouples the engine (imagebuild/flasher/state) from the
front-end. The engine reports FACTS; it never touches widgets or prints directly.

  CLIReporter   -> renders to the terminal via ui.py
  QueueReporter -> enqueues immutable events for the Tk GUI to drain on its main
                   thread (the engine runs on a worker thread)

Contract:
  phase(name, destructive=False)   a new stage began
  status(message)                  transient 'what's happening now' line
  log(line)                        detail/log line
  progress(completed, total)       determinate progress (total > 0)
  indeterminate(active, note="")   long op with no count (the flash) on/off
  warning(message) / ok(message) / error(message)
"""

import threading

from . import ui


class Reporter:
    def phase(self, name, destructive=False): ...
    def status(self, message): ...
    def log(self, line): ...
    def progress(self, completed, total): ...
    def indeterminate(self, active, note="", expect_secs=None): ...
    def warning(self, message): ...
    def ok(self, message): ...
    def error(self, message): ...


class CLIReporter(Reporter):
    """Terminal renderer - preserves the existing CLI look via ui.py."""

    def __init__(self):
        self._phase = ""
        self._bar = None
        self._hb = None
        self._hb_stop = None
        self._hb_thread = None

    def phase(self, name, destructive=False):
        self._end_bar()
        self._phase = name
        ui.step(name + ("  (keep the USB cable connected)" if destructive else ""))

    def status(self, message):
        self._end_bar()
        ui.info(message)

    def log(self, line):
        ui.info(ui.dim(line))

    def progress(self, completed, total):
        if not total or total <= 0:
            return
        if self._bar is None:
            self._bar = ui.Bar(self._phase or "working", total)
        self._bar.update(completed)
        if completed >= total:
            self._bar.done()
            self._bar = None

    def _end_bar(self):
        if self._bar is not None:
            self._bar.done()
            self._bar = None

    def indeterminate(self, active, note="", expect_secs=None):
        if active:
            if self._hb is not None:
                return
            self._hb = ui.Heartbeat(note or self._phase or "working", expect_secs=expect_secs)
            self._hb_stop = threading.Event()

            def _run(hb, stop):
                while not stop.is_set():
                    hb.tick()
                    stop.wait(0.5)
                hb.stop()

            self._hb_thread = threading.Thread(target=_run, args=(self._hb, self._hb_stop), daemon=True)
            self._hb_thread.start()
        else:
            if self._hb_stop:
                self._hb_stop.set()
            if self._hb_thread:
                self._hb_thread.join(timeout=2)
            self._hb = self._hb_stop = self._hb_thread = None

    def warning(self, message):
        self._end_bar()
        ui.warn(message)

    def ok(self, message):
        self._end_bar()
        ui.ok(message)

    def error(self, message):
        self._end_bar()
        ui.err(message)


class QueueReporter(Reporter):
    """Enqueues immutable (kind, payload) events for the GUI to drain via
    root.after on the main thread. NEVER touches Tk widgets itself."""

    def __init__(self, q):
        self.q = q

    def _emit(self, kind, **kw):
        self.q.put((kind, kw))

    def phase(self, name, destructive=False):
        self._emit("phase", name=name, destructive=destructive)

    def status(self, message):
        self._emit("status", message=message)

    def log(self, line):
        self._emit("log", line=line)

    def progress(self, completed, total):
        self._emit("progress", completed=completed, total=total)

    def indeterminate(self, active, note="", expect_secs=None):
        self._emit("indeterminate", active=active, note=note, expect_secs=expect_secs)

    def warning(self, message):
        self._emit("warning", message=message)

    def ok(self, message):
        self._emit("ok", message=message)

    def error(self, message):
        self._emit("error", message=message)
