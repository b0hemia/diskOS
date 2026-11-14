# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Tkinter/ttk graphical installer.

Architecture:
  * All Tk work on the main thread; a single worker thread runs the engine.
  * The worker drives a QueueReporter; the GUI drains the queue via root.after.
  * A blocking confirm() gate: the worker asks, the main thread shows a modal
    acknowledgement dialog, the worker resumes on the user's decision.
  * Explicit operation states; no Cancel once the destructive flash starts;
    window-close is intercepted while flashing.
"""

import math
import os
import queue
import re
import sys
import threading
import time

from diskos_installer import (__version__, bundle, errors, firmware_catalogue, firmware_download, firmware_resolve,
                              gui_theme, imagebuild, platform_probe, readiness, service, state, windows_usb)
from diskos_installer.reporter import QueueReporter

ACCENT, BG, FG, SUBFG = gui_theme.ACCENT, gui_theme.BG, gui_theme.TEXT, gui_theme.SECONDARY
PANEL, WARN, OKC, ERRC = gui_theme.PANEL, gui_theme.WARN, gui_theme.OK, gui_theme.ERR

# operation states
IDLE, RUNNING, CONFIRM, SUCCESS, FAILED = "idle", "running", "confirm", "success", "failed"

# firmware the Disc may run: (catalogue version, dropdown label); the first is preselected
FW_CHOICES = (("257", "V2.57 (latest)"), ("240", "V2.40"), ("228", "V2.28"), ("209", "V2.09"))
FW_HINT = ("On the Disc: Settings > System > Device Info > Firmware (diskOS), or "
           "Settings > System Settings > About device (stock FiiO).")
MB = 10 ** 6

# mode -> (title, one-line intro, primary button label)
MODE_TEXT = {
    "install": ("Install diskOS",
                "Uses the official FiiO firmware for your Disc, builds diskOS from it and flashes it over USB.",
                "Install diskOS..."),
    "restore": ("Restore stock firmware",
                "Reflashes bone-stock firmware and removes diskOS from the Disc.",
                "Restore stock firmware..."),
    "remove": ("Remove installer data",
               "Deletes saved firmware, recovery images, installer state and logs from this computer. The Disc is not touched.",
               "Remove installer data"),
}

# the whole window must fit a 1366x768 laptop (screen minus title bar and taskbar); taller content scrolls
MAX_WINDOW_H = gui_theme.MAX_WINDOW_H
WRAP = gui_theme.WRAP

# confirmation dialog
ACK_TEXT = "I understand and will keep the USB cable connected until it finishes."
BEGIN_TEXT = "Begin flash (~20 min)"
CONFIRM_TITLE = {"install": "Install diskOS", "restore-stock": "Restore stock firmware",
                 "remove-tool": "Remove installer data"}
DEV_CONFIRM_NOTE = ("Dev build: an always-on passwordless root shell over USB. "
                    "Development devices only, never an everyday one.")

# result screens
FLASH_LOCK_NOTE = ("Closing this window is blocked until the flash finishes. "
                   "Nothing else can be changed while the flash runs.")
# Wording is based on the engine: service.py do_install rep.ok (L230), do_restore rep.ok (L319), do_remove (L328, L347),
# docs/INSTALL.md L85-86 and INSTALL.md L123/L132 for first boot. Sources per sentence: merge/HANDOFF.md.
SUCCESS_TEXT = {
    "install": ("diskOS is installed", "diskOS was written to the Disc and checked.",
                "Turn the Disc off and on again. On the first start it finishes setting up diskOS by itself."),
    "restore": ("Stock firmware restored", "The Disc has FiiO's stock firmware again.",
                "Turn the Disc off and on again to start the stock firmware."),
    "remove": ("Installer data removed",
               "Saved firmware, recovery images, installer state and logs were removed from this computer.",
               "The installer app itself is kept. Delete it if you no longer need it."),
}
FLASH_KEEP_NOTE = "Keep the Disc plugged in and this computer awake until the flash finishes."
FLASH_PHASE_PREFIX = "Flashing (mask-ROM)"


def display_sentence(text):
    """Display only: capitalise the first letter and end with a full stop. The engine text is never altered."""
    t = (text or "").strip()
    if not t:
        return t
    t = t[0].upper() + t[1:]
    return t if t[-1] in ".!?" else t + "."


def flash_phase_text(name, mode):
    """Plain title for the engine's flash phase; other phase names pass through."""
    if not str(name).startswith(FLASH_PHASE_PREFIX):
        return name
    what = "stock firmware" if mode == "restore" else "diskOS"
    return f"Writing {what} to the Disc - about 20 minutes"


def stage_text(note):
    """Sentence-case stage line from the engine's note, e.g. 'flashing (scan -> erase)' -> 'Flashing: scan, erase'."""
    t = (note or "").strip()
    m = re.match(r"^(.*?)\s*\((.*)\)\s*$", t)
    if m:
        steps = ", ".join(x.strip() for x in m.group(2).split("->") if x.strip())
        t = f"{m.group(1)}: {steps}" if m.group(1) else steps
    return t[:1].upper() + t[1:]
NOTHING_WRITTEN = "Nothing was written to the Disc."
WAIT_NOTE = "Do not retry or disconnect during the wait."
# codes whose result is UNKNOWN after the writer may have started (errors.WAIT_THEN_RECOVER)
UNKNOWN_CODES = ("E301", "E302", "E303", "E321")
_CODE_RE = re.compile(r"^\[([A-Z]\d{3})\]\s*(.*)$", re.S)


def error_info(msg, info=None, flashing=False):
    """Normalise a failure into display parts: code, message, action, unknown-result flag, nothing-written flag.
    `info` carries the engine exception's own fields; `msg` is its str() (code + message + action)."""
    info = dict(info or {})
    code, message = info.get("code"), info.get("message")
    m = _CODE_RE.match(msg or "")
    if not code and m:
        code = m.group(1)
    if not message:
        message = m.group(2) if m else (msg or "unknown error")
    action = info.get("action")
    wait = errors.WAIT_THEN_RECOVER
    if not action and wait in message:          # text-only failure: split the engine's own recovery text off
        message, action = message.replace(" - " + wait, "").rstrip(), wait
    waits = bool(action and wait in action)
    # a writer that FINISHED (RECOVERABLE) is not unknown even under E301; otherwise these codes always are
    unknown = waits or (code in UNKNOWN_CODES and action != errors.RECOVERABLE) \
        or (flashing and not code) or ("[F000]" in message and action != errors.RECOVERABLE)
    if unknown and not waits:
        action = wait + (f" {action}" if action else "")
    safe = bool(info.get("pre_launch")) or bool(code and code[0] == "E" and code[1] in "12")
    return {"code": code, "message": message, "action": action, "unknown": unknown,
            "nothing_written": safe and not unknown}


OTA_LABEL = "Allow diskOS updates over Wi-Fi"
OTA_HELP = ("diskOS only updates when you tap Update diskOS on the Disc, and only installs releases "
            "signed by the diskOS release key. Untick to build without updates.")
OTA_NO_KEY = "This installer has no release key; updates are off"
WINDOWS_REMOVE_NOTE = ("The boot-mode driver choice stays. To undo: Device Manager > "
                       "boot-mode device > Uninstall device; leave the driver-package box unticked.")


def ota_key_available():
    """True when this installer has a release key (env or payload), as resolve_ota_config finds it.
    A key that exists but is unreadable/invalid counts as available: the build reports that error."""
    try:
        return imagebuild.resolve_ota_config() is not None
    except imagebuild.BuildError:
        return True


def ota_params(ticked, key_available):
    """GUI checkbox state -> the service.do_install param. No key means always off."""
    return {"no_ota": not (ticked and key_available)}


class App:
    # firmware acquisition state (see "firmware: pick, download, verify"); class defaults so a bare App is safe
    _fw_tok, _fw_ctx, _fw_busy = 0, None, False      # run token: events of older runs are dropped
    _fw_verified = _fw_filled = _fw_row = _restore_why = None   # verified path, path we filled in, checklist note
    _fw_setting = False
    fw_get = fw_browse = fw_entry = fw_combo = fw_state = None
    _fw_bar = _fw_cap = None

    def __init__(self, root, run_lock=None):
        self.root = root
        self.run_lock = run_lock           # the held installer run lock (firmware acquisition borrows it)
        self._fr = firmware_resolve        # seam: resolve / identify / check_downloads / plan_restore / browser
        self.q = queue.Queue()
        self.reporter = QueueReporter(self.q)
        self.worker = None
        self.state = IDLE
        self.flashing = False              # True only during the destructive phase
        self._driver_running = False
        self._inventory_running = False
        self._inventory_worker = None
        self._ready_probe = None           # last readiness.probe() result (None until the first one lands)
        self._ready_running, self._ready_gen = False, 0
        self._ready_cancel = None
        self._ready_view = None
        self._closing = False
        self._confirm_event = threading.Event()
        self._confirm_result = False
        self._confirm_lock = threading.Lock()
        self._confirm_pending = False
        self._confirm_cancel = None
        self._closing = False
        self.start_time = None
        self._run_view = False             # True while the welcome group is hidden for a run/result
        self._saved_pack = []
        self._stage_start = self._flash_start = self._expect_secs = self._progress = None

        root.title("diskOS Installer")
        root.minsize(620, 360)
        root.protocol("WM_DELETE_WINDOW", self._on_close)
        root.bind("<Destroy>", self._on_destroy, add="+")

        self._build_style()
        self._build_widgets()
        self._refresh_state_labels()
        self.root.after(80, self._drain)
        self.root.after(1000, self._tick_elapsed)
        self.root.after(4000, self._tick_device)

    # ---- styling -----------------------------------------------------------
    def _build_style(self):
        from tkinter import ttk
        self.ttk = ttk
        fonts = gui_theme.make_fonts(self.root)
        self.fonts = fonts              # keep every Font (incl. brand): Tk fonts die with their Python object
        self.f_title, self.f_h, self.f_b = fonts["title"], fonts["h"], fonts["b"]
        self.f_sub, self.f_btn, self.f_mono = fonts["sub"], fonts["btn"], fonts["mono"]
        gui_theme.apply(self.root, fonts)

    # ---- layout ------------------------------------------------------------
    def _make_body(self):
        """All content lives in self.body, inside a canvas that shows a scrollbar only when the content is
        taller than the screen allows (small laptops, 125% scaling). Otherwise the window fits the content."""
        import tkinter as tk
        ttk = self.ttk
        self._canvas = tk.Canvas(self.root, bg=BG, highlightthickness=0, bd=0, height=1)
        self._vbar = ttk.Scrollbar(self._canvas.master, orient="vertical", command=self._canvas.yview)
        self._canvas.configure(yscrollcommand=self._vbar.set)
        self._canvas.pack(side="left", fill="both", expand=True)
        self.body = ttk.Frame(self._canvas)
        self._body_win = self._canvas.create_window(0, 0, window=self.body, anchor="nw")
        self._scrolling = False
        self.body.bind("<Configure>", lambda e: self._fit_body())
        self._canvas.bind("<Configure>", self._canvas_configured)
        self.root.bind_all("<FocusIn>", self._on_body_focus, add="+")
        for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
            self.root.bind_all(seq, self._on_wheel, add="+")

    def max_window_height(self):
        """Tallest window we allow: the screen minus title bar and taskbar. During a run or result the window
        also stays within MAX_WINDOW_H (a 1366x768 laptop), and the Close/Back footer takes its share."""
        limit = max(360, self.root.winfo_screenheight() - gui_theme.SCREEN_MARGIN)
        if getattr(self, "_run_view", False):
            limit = min(limit, MAX_WINDOW_H)
        foot = getattr(self, "foot", None)
        if foot is not None and foot.winfo_manager():
            pady = foot.pack_info().get("pady", 0)
            pady = sum(int(float(x)) for x in (pady if isinstance(pady, (tuple, list)) else (pady, pady)))
            limit -= foot.winfo_reqheight() + pady
        return max(360, limit)

    def _scroll_top(self):
        self._canvas.yview_moveto(0)

    def _fit_body(self):
        req = self.body.winfo_reqheight()
        limit = self.max_window_height()
        self._canvas.configure(height=min(req, limit), width=self.body.winfo_reqwidth(),
                               scrollregion=(0, 0, self.body.winfo_reqwidth(), req))
        viewport = self._canvas.winfo_height() if self._canvas.winfo_ismapped() else limit
        self._set_scrolling(req > min(limit, viewport))

    def _set_scrolling(self, need):
        if need != self._scrolling:
            self._scrolling = need
            if need:
                self._vbar.pack(side="right", fill="y", before=self._canvas)
            else:
                self._vbar.pack_forget()
                self._canvas.yview_moveto(0)

    def _canvas_configured(self, e):
        self._canvas.itemconfigure(self._body_win, width=e.width)
        self._set_scrolling(self.body.winfo_reqheight() > e.height)

    def _in_body(self, widget):
        while widget is not None:
            if widget is self.body or widget is self._canvas:
                return True
            widget = getattr(widget, "master", None)
        return False

    def _on_body_focus(self, e):
        if not self._scrolling or not self._in_body(e.widget):
            return
        top = e.widget.winfo_rooty() - self.body.winfo_rooty()
        bottom = top + e.widget.winfo_height()
        visible = self._canvas.canvasy(0)
        height = self._canvas.winfo_height()
        req = self.body.winfo_reqheight()
        if top < visible:
            self._canvas.yview_moveto(top / max(1, req))
        elif bottom > visible + height:
            self._canvas.yview_moveto((bottom - height) / max(1, req))

    def _on_wheel(self, e):
        if not self._scrolling or not self._in_body(e.widget) or e.widget.winfo_class() == "Text":
            return
        num, delta = getattr(e, "num", None), getattr(e, "delta", 0)
        if num not in (4, 5) and not delta:
            return
        step = -1 if num == 4 or delta > 0 else 1
        self._canvas.yview_scroll(step * 3, "units")

    def _build_widgets(self):
        ttk = self.ttk
        import tkinter as tk
        self._make_body()
        sp, sec = gui_theme.SP, gui_theme.SECTION
        pad = {"padx": gui_theme.GUTTER}
        # brand row (wordmark) + hero: the task in one sentence beside the pressed disc
        brand = ttk.Frame(self.body)
        brand.pack(fill="x", pady=(sp["xl"], 0), **pad)
        self._brand = brand
        self._wordmark = gui_theme.wordmark(self.root)      # kept: Tk drops unreferenced images
        if self._wordmark is not None:
            ttk.Label(brand, image=self._wordmark, style="Brand.TLabel").pack(side="left")
        else:
            ttk.Label(brand, text="diskOS", style="Brand.TLabel").pack(side="left")
        hero = ttk.Frame(self.body)
        hero.pack(fill="x", pady=(sp["s"], sec), **pad)
        gui_theme.hero_bleed(self.body, hero, brand)
        txt = ttk.Frame(hero)
        txt.pack(side="left", fill="x", expand=True, anchor="s", padx=(0, sp["xl"]))   # text sits on the disc's baseline
        self.title_lbl = ttk.Label(txt, text="", style="Title.TLabel")
        self.title_lbl.pack(anchor="w")
        self.intro_lbl = ttk.Label(txt, text="", wraplength=360, justify="left", style="Lede.TLabel")
        self.intro_lbl.pack(anchor="w", pady=(sp["m"], 0))
        self.driver_lbl = self.driver_btn = None
        if platform_probe.host()[0] == "windows":
            self.driver_lbl = ttk.Label(txt, text="", style="Sub.TLabel", wraplength=380, justify="left")
            # Show inventory details here; the checklist offers driver setup when needed.
            self.driver_lbl.pack_forget()

        # mode is still one variable; the quiet buttons below select it
        self.mode = tk.StringVar(value="install")

        # ready check: warns, never blocks; flasher.preflight remains the real gate
        self.ready_frame = ttk.Frame(self.body, style="Panel.TFrame")
        self.ready_frame.pack(fill="x", **pad)
        gui_theme.rule(self.ready_frame).pack(fill="x")
        rhead = self._ready_head = ttk.Frame(self.ready_frame, style="Panel.TFrame")
        rhead.pack(fill="x", padx=gui_theme.PANEL_PAD, pady=(sec - 2, 0))
        self.ready_title = ttk.Label(rhead, text="Checking...", style="Panel.TLabel")
        self.ready_title.pack(side="left")
        gui_theme.set_status(self.ready_title, "Checking...", SUBFG)
        self.ready_refresh = ttk.Button(rhead, text="Refresh", style="Link.TButton", command=self._on_ready_refresh)
        self.ready_refresh.pack(side="right")
        self.ready_rows = ttk.Frame(self.ready_frame, style="Panel.TFrame")
        self.ready_rows.pack(fill="x", padx=gui_theme.PANEL_PAD)
        ttk.Frame(self.ready_frame, style="Panel.TFrame", height=sec).pack(side="bottom")
        self.ready_help = ttk.Frame(self.ready_frame, style="Panel.TFrame")   # packed only with no Disc found
        ttk.Label(self.ready_help, style="PanelSub.TLabel", wraplength=500, justify="left",
                  text="Enter flash mode:  " + "  ".join("%d. %s" % (i, step) for i, step in enumerate(readiness.STEPS[:2], 1))
                  + "\n3. " + readiness.STEPS[2]
                  ).pack(anchor="w")

        # config panel (per-mode)
        self.cfg = ttk.Frame(self.body, style="Panel.TFrame")
        self.cfg.pack(fill="x", **pad)

        # footer: other actions as quiet links on the left, the primary action on the right
        self.page_foot = ttk.Frame(self.body)
        self.page_foot.pack(fill="x", **pad)
        gui_theme.rule(self.page_foot).pack(fill="x")
        frow = ttk.Frame(self.page_foot)
        frow.pack(fill="x", pady=(sec - 2, 0))
        self.action_btn = ttk.Button(frow, text="Install diskOS...",
                                      style="Accent.TButton", command=self._on_action)
        self.action_btn.pack(side="right", anchor="center")
        self.mode_frame = ttk.Frame(frow)
        self.mode_frame.pack(side="left", fill="x", expand=True, anchor="center")
        gui_theme.kicker(self.mode_frame, "Other actions").pack(anchor="w", pady=(0, sp["xs"]))
        row = ttk.Frame(self.mode_frame)
        row.pack(fill="x")
        self.back_btn = ttk.Button(row, text="Back to install", style="Link.TButton", command=lambda: self._set_mode("install"))
        self.restore_btn = ttk.Button(row, text="Restore stock firmware...", style="Link.TButton",
                                      command=lambda: self._set_mode("restore"))
        self.remove_btn = ttk.Button(row, text="Remove installer data...", style="Link.TButton",
                                     command=lambda: self._set_mode("remove"))
        self._action_btns = {"restore": self.restore_btn, "remove": self.remove_btn, "install": self.back_btn}

        # progress / result block. In a run (see _enter_run_view) it is the only thing in the page body.
        prog = ttk.Frame(self.body)
        prog.pack(fill="both", expand=True, **pad)
        self.prog = prog
        self.chip = gui_theme.status_word(prog)                  # results only
        self.warn_banner = gui_theme.banner(prog, WRAP)          # banner, lock note: packed only while flashing
        self.prog_head = ttk.Frame(prog)             # shown only once something is happening (see _sync_progress)
        self.phase_lbl = gui_theme.title(self.prog_head, "Ready.", wrap=WRAP)
        self.phase_lbl.pack(anchor="w")
        self.status_lbl = ttk.Label(self.prog_head, text="", style="Sub.TLabel", wraplength=WRAP, justify="left")
        self.status_lbl.pack(anchor="w", pady=(0, gui_theme.SP["m"]))
        self.bar = ttk.Progressbar(self.prog_head, style="diskos.Horizontal.TProgressbar",
                                   mode="determinate", maximum=100)
        self.bar.pack(fill="x")
        self.elapsed_lbl = ttk.Label(self.prog_head, text="", style="Sub.TLabel")
        self.elapsed_lbl.pack(anchor="w", pady=(gui_theme.SP["xs"], gui_theme.SP["m"]))
        self.lock_lbl = ttk.Label(prog, text=FLASH_LOCK_NOTE, style="Sub.TLabel", wraplength=WRAP, justify="left")
        self.result_box = ttk.Frame(prog)           # success card / error text and recovery panel
        # Close/Back live outside the scrolling page so they never scroll away
        self.foot = ttk.Frame(self.root)

        # log behind a Details toggle: it still receives every line while hidden
        self.logwrap = ttk.Frame(prog)
        self.logwrap.pack(fill="both", expand=True, pady=(gui_theme.SP["s"], gui_theme.SP["m"]))
        drow = ttk.Frame(self.logwrap)
        drow.pack(fill="x")
        self.details_btn = gui_theme.disclosure(drow, "Details", self._toggle_details)
        self.details_btn.pack(side="left")
        self.host_lbl = ttk.Label(drow, text="", style="Meta.TLabel", justify="right")   # quiet, bottom right
        self.host_lbl.pack(side="right")
        self.logbox = ttk.Frame(self.logwrap)
        self.log = tk.Text(self.logbox, width=1, height=7, font=self.f_mono, wrap="word", relief="flat")   # width follows the window
        gui_theme.style_log(self.log)
        sb = ttk.Scrollbar(self.logbox, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set, state="disabled")
        self.log.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self._details_open = False
        self._on_mode()
        self._sync_progress()

    def _sync_progress(self):
        """Idle and nothing to say: hide the phase/bar block (the checklist already says Ready)."""
        head = getattr(self, "prog_head", None)
        if head is None:
            return
        idle = self.state == IDLE and not self._run_view and str(self.phase_lbl.cget("text")) in ("", "Ready.") \
            and not str(self.status_lbl.cget("text"))
        if idle and head.winfo_manager():
            head.pack_forget()
        elif not idle and not head.winfo_manager():
            head.pack(fill="x", before=self.logwrap)

    def _ensure_head(self):
        """The phase/bar block is always shown for a run or a result (before chip/banner are packed relative to it)."""
        if not self.prog_head.winfo_manager():
            self.prog_head.pack(fill="x", before=self.logwrap)

    def _toggle_details(self, show=None):
        self._details_open = (not self._details_open) if show is None else show
        if self._details_open:
            self.logbox.pack(fill="both", expand=True, pady=(6, 0))
            self.root.after_idle(lambda: self.log.see("end"))
        else:
            self.logbox.pack_forget()
        gui_theme.set_disclosure(self.details_btn, self._details_open)

    def _toggle_advanced(self, show=None):
        """Show or hide the Dev choice. It stays open while Dev is selected, so the choice is never hidden."""
        adv = getattr(self, "_adv", None)
        if adv is None:
            return
        show = (not adv.winfo_manager()) if show is None else show
        if not show and self.variant.get() == "dev":
            show = True
        if show and not adv.winfo_manager():
            adv.pack(side="left", padx=(gui_theme.SP["l"], 0))
        elif not show:
            adv.pack_forget()
        gui_theme.set_disclosure(self._adv_btn, show)

    def _variant_arrow(self, _event):
        """Tk's radio traversal includes hidden siblings; keep it on the visible choice."""
        if not self._adv.winfo_manager():
            return "break"
        return None                    # Advanced is open: use Tk's normal radio traversal

    def _set_mode(self, m):
        self._restore_why = None
        self.mode.set(m)
        self._on_mode()

    def _build_config(self):
        for w in self.cfg.winfo_children():
            w.destroy()
        ttk = self.ttk
        import tkinter as tk
        m = self.mode.get()
        self.fw_get = self.fw_browse = self.fw_entry = self.fw_combo = self.fw_state = None   # destroyed above
        self._fw_stop()
        self._fw_verified = self._fw_filled = None
        self.ota_cb = None
        self.fw_err = None
        sp, sec = gui_theme.SP, gui_theme.SECTION
        gui_theme.rule(self.cfg).pack(fill="x")
        inner = ttk.Frame(self.cfg, style="Panel.TFrame")
        inner.pack(fill="x", padx=gui_theme.PANEL_PAD, pady=(sec, sec))
        hang = gui_theme.hang(self.root)                 # text under a checkbox lines up with its label
        if m == "install":
            gui_theme.kicker(inner, "Your firmware").grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, sp["m"]))
            self.fw_var = tk.StringVar()
            self.fw_var.trace_add("write", lambda *a: self._fw_changed())
            self._build_fw_section(inner, self.fw_var, 1)
            self.variant = tk.StringVar(value="public")
            gui_theme.kicker(inner, "Variant").grid(row=8, column=0, columnspan=2, sticky="w", pady=(sec + 2, sp["s"]))
            vr = ttk.Frame(inner, style="Panel.TFrame")
            vr.grid(row=9, column=0, columnspan=2, sticky="w")
            public_radio = ttk.Radiobutton(
                vr, text="Public (recommended)", value="public", style="Panel.TRadiobutton",
                variable=self.variant, command=self._variant_warn)
            public_radio.pack(side="left")
            # Dev sits under Advanced: same variable, same warning; it opens by itself whenever Dev is chosen
            self._adv_btn = gui_theme.disclosure(vr, "Advanced", self._toggle_advanced)
            self._adv_btn.pack(side="left", padx=(sp["xl"] + 4, 0))
            self._adv = ttk.Radiobutton(vr, text="Dev (root shell)", value="dev", style="Panel.TRadiobutton",
                                        variable=self.variant, command=self._variant_warn)
            for radio in (public_radio, self._adv):
                for key in ("<Up>", "<Down>"):
                    radio.bind(key, self._variant_arrow)
            self.variant_warn_lbl = tk.Label(
                inner, text="", bg=PANEL, fg=WARN, font=self.f_sub, justify="left", wraplength=540)
            self.variant_warn_lbl.grid(row=11, column=0, columnspan=2, sticky="w", pady=(sp["s"], 0), padx=(hang, 0))
            self.variant_warn_lbl.grid_remove()          # no empty gap until Dev is picked
            self.ota_key = ota_key_available()
            self.ota_var = tk.BooleanVar(value=self.ota_key)
            self.ota_cb = ota_cb = ttk.Checkbutton(inner, text=OTA_LABEL, variable=self.ota_var,
                                                    style="Panel.TCheckbutton")
            ota_cb.grid(row=12, column=0, columnspan=2, sticky="w", pady=(sp["m"], 0))
            if not self.ota_key:
                ota_cb.state(["disabled"])
            tk.Label(inner, text=(OTA_HELP if self.ota_key else OTA_NO_KEY), bg=PANEL, fg=SUBFG,
                     font=self.f_sub, justify="left", wraplength=500).grid(
                row=13, column=0, columnspan=2, sticky="w", pady=(0, 0), padx=(hang, 0))
            inner.columnconfigure(0, weight=1)
        elif m == "restore":
            have = state.stock_image_exists() and self._restore_why is None   # fast; plan_restore decides at click time
            msg = ("A saved bone-stock image is available - restore is one click."
                   if have else
                   "No saved stock image. The installer rebuilds it from the official FiiO firmware."
                   if self._restore_why is None else
                   "The saved stock image cannot be used (%s). The installer rebuilds it from the official FiiO firmware."
                   % self._restore_why)
            gui_theme.kicker(inner, "Stock firmware").grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, sp["m"]))
            ttk.Label(inner, text=msg, style="Panel.TLabel", wraplength=540).grid(
                row=1, column=0, columnspan=2, sticky="w", pady=(0, sp["m"] if not have else 0))
            self.restore_fw_var = None          # only the no-saved-image field below sets this
            if not have:
                self.restore_fw_var = tk.StringVar()
                self.restore_fw_var.trace_add("write", lambda *a: self._fw_changed())
                self._build_fw_section(inner, self.restore_fw_var, 2)
            else:
                self.fw_state = ttk.Frame(inner, style="Panel.TFrame")
                self.fw_state.grid(row=2, column=0, columnspan=2, sticky="we")
                self._fw_idle()
            inner.columnconfigure(0, weight=1)
        else:  # remove
            driver_note = (WINDOWS_REMOVE_NOTE + " "
                           if platform_probe.host()[0] == "windows" else
                           "Nothing was installed system-wide. ")
            gui_theme.kicker(inner, "This computer").pack(anchor="w", pady=(0, sp["m"]))
            ttk.Label(inner, wraplength=540, style="Panel.TLabel",
                      text=("Deletes cached firmware, partial downloads, saved recovery images, installer state and owned GUI logs from THIS computer. "
                            + driver_note + "If diskOS is still on the device, "
                            "use 'Restore stock firmware' first.")).pack(anchor="w")

    # ---- firmware: pick, download, verify -------------------------------------
    # Workers never touch Tk: they only call _fw_post(), the main thread applies it in _fw_event().
    # Every run owns a token; _fw_stop() (dropdown/path change, cancel, close) bumps it, so late events are dropped.
    def _build_fw_section(self, inner, var, row):
        import tkinter as tk
        ttk, sp = self.ttk, gui_theme.SP
        tk.Label(inner, text="Which firmware is on your Disc?", bg=PANEL, fg=FG, font=self.f_b).grid(
            row=row, column=0, columnspan=2, sticky="w")
        top = ttk.Frame(inner, style="Panel.TFrame")
        top.grid(row=row + 1, column=0, columnspan=2, sticky="w", pady=(sp["s"], 0))
        self.fw_ver_var = tk.StringVar(value=FW_CHOICES[0][1])
        self.fw_combo = ttk.Combobox(top, textvariable=self.fw_ver_var, state="readonly", width=14,
                                     values=[label for _, label in FW_CHOICES])
        self.fw_combo.pack(side="left")
        self.fw_combo.bind("<<ComboboxSelected>>", lambda e: self._fw_dropdown())
        self.fw_get = ttk.Button(top, text="Get firmware", style="Small.TButton", command=self._fw_get_click)
        self.fw_get.pack(side="left", padx=(sp["m"], 0))
        tk.Label(inner, text=FW_HINT, bg=PANEL, fg=SUBFG, font=self.f_sub, justify="left", wraplength=480).grid(
            row=row + 2, column=0, columnspan=2, sticky="w", pady=(sp["s"], sp["m"]))
        self.fw_entry = ttk.Entry(inner, textvariable=var, width=44)
        self.fw_entry.grid(row=row + 3, column=0, sticky="we")
        self.fw_browse = ttk.Button(inner, text="Browse...", style="Small.TButton", command=self._pick_firmware)
        self.fw_browse.grid(row=row + 3, column=1, padx=(sp["m"], 0))
        self.fw_err = tk.Label(inner, text="", bg=PANEL, fg=ERRC, font=self.f_sub, justify="left", anchor="w")
        self.fw_err.grid(row=row + 4, column=0, columnspan=2, sticky="w", pady=(sp["s"], 0))
        self.fw_err.grid_remove()
        self.fw_state = ttk.Frame(inner, style="Panel.TFrame")
        self.fw_state.grid(row=row + 5, column=0, columnspan=2, sticky="we")
        self._fw_idle()

    def _fw_version(self):
        label = self.fw_ver_var.get() if self.fw_combo is not None else None
        return next((v for v, text in FW_CHOICES if text == label), FW_CHOICES[0][0])

    def _fw_name(self, ver):
        return firmware_catalogue.CATALOGUE[ver].name

    def _fw_show(self, lines=(), buttons=(), bar=None, url=None, row=None):
        """Redraw the state area. lines: (text, "err"|"ok"|"sub"|"text"); bar: None | ("det"|"ind", caption)."""
        import tkinter as tk
        ttk, sp = self.ttk, gui_theme.SP
        f = self.fw_state
        for w in f.winfo_children():
            w.destroy()
        self._fw_bar = self._fw_cap = None
        self._fw_row = row
        colours = {"err": ERRC, "ok": OKC, "sub": SUBFG, "text": FG}
        for text, kind in lines:
            tk.Label(f, text=text, bg=PANEL, fg=colours[kind], font=self.f_sub, justify="left", anchor="w",
                     wraplength=540).pack(anchor="w", pady=(sp["s"], 0))
        if url:
            e = ttk.Entry(f)
            e.insert(0, url)
            e.config(state="readonly")
            e.pack(fill="x", pady=(sp["s"], 0))
        if bar:
            line = ttk.Frame(f, style="Panel.TFrame")
            line.pack(fill="x", pady=(sp["m"], 0))
            self._fw_bar = ttk.Progressbar(line, style="diskos.Horizontal.TProgressbar", maximum=100,
                                           mode="determinate" if bar[0] == "det" else "indeterminate")
            self._fw_bar.pack(side="left", fill="x", expand=True)
            ttk.Button(line, text="Cancel", style="Small.TButton", command=self._fw_cancel).pack(
                side="right", padx=(sp["m"], 0))
            if bar[0] == "ind":
                self._fw_bar.start(12)
            self._fw_cap = tk.Label(f, text=bar[1], bg=PANEL, fg=SUBFG, font=self.f_sub, anchor="w")
            self._fw_cap.pack(anchor="w", pady=(sp["s"], 0))
        if buttons:
            line = ttk.Frame(f, style="Panel.TFrame")
            line.pack(anchor="w", pady=(sp["m"], 0))
            for label, cmd in buttons:
                ttk.Button(line, text=label, style="Small.TButton", command=cmd).pack(side="left", padx=(0, sp["m"]))
        self._ready_render()

    def _fw_idle(self):
        if self.fw_get is None:            # restore with a saved image: nothing to choose, nothing to download
            return self._fw_show([("Nothing is downloaded.", "sub")])
        ver = self._fw_version()
        pin = firmware_catalogue.CATALOGUE[ver]
        self._fw_show([("Get firmware downloads the official %s firmware (%d MB) and checks it. "
                        "Nothing is sent about your Disc." % (pin.name, round(pin.size / MB)), "sub")])

    def _fw_fallbacks(self):
        return [("Open FiiO download page", self._fw_open_page), ("Check Downloads", self._fw_scan_click)]

    def _fw_lock(self, busy):
        if self.fw_get is None:
            return
        st = "disabled" if busy else "normal"
        for w in (self.fw_get, self.fw_browse, self.fw_entry, self.action_btn, *self._action_btns.values()):
            w.config(state=st)
        self.fw_combo.config(state="disabled" if busy else "readonly")

    def _fw_stop(self):
        """Invalidate the current run and any verification; its late events are dropped."""
        self._fw_tok += 1
        ctx, self._fw_ctx = self._fw_ctx, None
        if ctx is not None:
            ctx["cancel"].set()
            if ctx["handle"] is not None:
                ctx["handle"].cancel()
        self._fw_busy, self._fw_verified = False, None
        self._fw_install_ticket = None
        self._fw_lock(False)

    def _fw_begin(self, kind, lines, bar):
        self._fw_stop()
        tok = self._fw_tok
        self._fw_ctx = {"cancel": threading.Event(), "handle": None, "kind": kind}
        self._fw_busy = True
        self._fw_lock(True)
        self._fw_error("")
        self._fw_show(lines, bar=bar, row=("Getting the firmware...", ""))
        return tok, self._fw_ctx

    def _fw_post(self, tok, ev, **kw):             # the only call a worker makes into the GUI
        self.q.put(("fw", dict(kw, tok=tok, ev=ev)))

    def _fw_changed(self):
        self._fw_error("")
        if self._fw_setting or self.fw_state is None:
            return self._ready_render()
        self._fw_stop()                    # a typed or browsed path invalidates the old verification
        self._fw_idle()

    def _fw_dropdown(self):
        if self._fw_filled is not None and self._active_fw_var().get() == self._fw_filled:
            self._fw_set_path("")                  # the verified file belonged to the old choice
        self._fw_stop()
        self._fw_idle()

    def _fw_set_path(self, path):
        self._fw_setting = True
        try:
            self._active_fw_var().set(path)
            self._fw_filled = path or None
            if self.fw_entry is not None:
                self.fw_entry.xview_moveto(1)      # the file name is the end of the path
        finally:
            self._fw_setting = False

    def _fw_ready(self):
        """Checklist override (verified, text, fix) for the screen on show, or None (saved image / no section)."""
        if self.fw_get is None:
            return None
        if self._fw_busy:
            return (True, "", "")          # a run is under way: no warning row
        path = self._active_fw_var().get().strip()
        if self._fw_verified and self._fw_verified == path:
            return (True, "", "")
        if self._fw_row is not None:
            return (False,) + tuple(self._fw_row)
        if path:
            return None                    # typed/browsed but not checked yet: Install checks it first
        return (False, "%s firmware not downloaded yet" % self._fw_name(self._fw_version()), "Press Get firmware.")

    def _fw_get_click(self):
        if self._fw_busy:
            return
        ver = self._fw_version()
        tok, ctx = self._fw_begin("get", [("Downloading %s firmware from FiiO..." % self._fw_name(ver), "text")],
                                  ("det", "0 of %d MB - 0%%" % round(firmware_catalogue.CATALOGUE[ver].size / MB)))
        post = lambda op, kind, done, total: self._fw_post(tok, kind, done=done, total=total)

        def run():
            try:
                res = self._fr.resolve_firmware(ver, self.run_lock, post)
                if res.kind == "cached":
                    return self._fw_post(tok, "verified", path=res.path, how="cache", ver=ver)
                ctx["handle"] = res.handle
                if ctx["cancel"].is_set():
                    return res.handle.cancel()
                self._fw_post(tok, "verified", path=res.handle.wait(), how="download", ver=ver)
            except Exception as exc:
                kind = ("tls" if isinstance(exc, firmware_download.TlsError)
                        else "space" if isinstance(exc, (firmware_download.DiskFull, firmware_resolve.InsufficientSpace))
                        else "cancel" if isinstance(exc, firmware_download.Cancelled) else "other")
                self._fw_post(tok, "error", kind=kind, ver=ver, exc=exc,
                              scan=self._fw_scan_now(ver, ctx) if kind == "other" else None)
        threading.Thread(target=run, daemon=True).start()

    def _fw_scan_now(self, ver, ctx):
        try:
            return self._fr.check_downloads(ver, run_lock=self.run_lock, cancel=ctx["cancel"].is_set)
        except Exception:
            return None

    def _fw_scan_click(self):
        if self._fw_busy:
            return
        ver = self._fw_version()
        tok, ctx = self._fw_begin("scan", [("Checking Downloads for %s firmware..." % self._fw_name(ver), "text")],
                                  ("ind", ""))
        threading.Thread(target=lambda: self._fw_post(tok, "scan", ver=ver, scan=self._fw_scan_now(ver, ctx)),
                         daemon=True).start()

    def _fw_manual(self, then_install=False):
        """Verify the path in the field (typed or from Browse) against the dropdown's version."""
        ver, path = self._fw_version(), self._active_fw_var().get().strip()
        tok, ctx = self._fw_begin("manual", [("Checking the file...", "text")], ("ind", ""))

        def run():
            try:
                self._fr.identify_zip(path, expected=ver)
                self._fw_post(tok, "verified", path=path, how="manual", ver=ver, then=then_install)
            except Exception as exc:
                self._fw_post(tok, "rejected", ver=ver, exc=exc)
        threading.Thread(target=run, daemon=True).start()

    def _fw_cancel(self):
        kind = self._fw_ctx["kind"] if self._fw_ctx else "get"
        ver = self._fw_version()
        self._fw_stop()
        if kind == "get":
            self._fw_show([("Firmware download cancelled.", "text")], [("Retry", self._fw_get_click)],
                          row=("%s firmware not downloaded yet" % self._fw_name(ver), "Press Retry or Get firmware."))
        else:
            self._fw_show([("Cancelled.", "text")], self._fw_fallbacks(),
                          row=("%s firmware not downloaded yet" % self._fw_name(ver), "Press Get firmware."))

    def _fw_open_page(self):
        res = self._fr.open_download_page(self._fw_version())
        if not res.ok:
            self._fw_show([("Could not open a browser. Open this address yourself, then press Check Downloads.", "err")],
                          self._fw_fallbacks(), url=res.url, row=self._fw_row)

    def _fw_found(self, ver, scan, first=()):
        self._fw_verified = scan.path
        self._fw_show(list(first) + [("Found and verified %s firmware in Downloads." % self._fw_name(ver), "ok"),
                                     ("Copied into the installer's own folder; your original file is untouched.", "sub")])
        self._fw_set_path(scan.path)

    def _fw_no_match(self, ver, first, scan):
        detail = scan.detail if scan is not None and scan.kind != "none" else None
        text = detail or "No verified %s firmware found in Downloads." % self._fw_name(ver).split(" ", 1)[1]
        self._fw_show(list(first) + [(text, "text")], self._fw_fallbacks(),
                      row=("%s firmware not downloaded yet" % self._fw_name(ver), "Press Get firmware to retry, or Browse for a ZIP."))

    def _fw_event(self, p):
        if threading.current_thread() is not threading.main_thread():
            raise RuntimeError("firmware events are applied on the main thread only")
        if p["tok"] != self._fw_tok:           # a run that was cancelled, replaced or invalidated
            return
        ev, ver = p["ev"], p.get("ver")
        if ev in ("progress", "verifying") and self._fw_bar is not None:
            done, total = p["done"], p["total"] or 1
            if ev == "verifying":
                self._fw_show([("Verifying firmware...", "text")], bar=("det", "Checking the download against the installer's recorded checksum"),
                              row=self._fw_row)
                self._fw_bar.config(value=100)
            else:
                self._fw_bar.config(value=100 * done / total)
                self._fw_cap.config(text="%d of %d MB - %d%%" % (done // MB, round(total / MB), round(100 * done / total)))
            return
        if ev in ("progress", "verifying"):
            return
        self._fw_busy, self._fw_ctx = False, None
        if self._fw_bar is not None:
            self._fw_bar.stop()
        self._fw_lock(False)
        if ev == "verified":
            self._fw_verified = p["path"]
            name = self._fw_name(ver)
            tail = {"cache": "Saved on this computer from an earlier run - no download needed.",
                    "download": "Downloaded from FiiO and saved on this computer for next time.",
                    "manual": "Using the file you chose."}[p["how"]]
            self._fw_show([("%s firmware verified." % name, "ok"), (tail, "sub")])
            if p["how"] != "manual":
                self._fw_set_path(p["path"])
            else:
                self._ready_render()
            if p.get("then"):
                self._fw_install_ticket = (p["tok"], p["path"])
                self._on_action()
        elif ev == "rejected":
            self._fw_show([("This file does not match the verified %s download. It cannot be used." % self._fw_name(ver), "err")],
                          self._fw_fallbacks(), row=("The chosen file is not verified FiiO firmware",
                                                     "Pick another file, or press Get firmware."))
        elif ev == "scan":
            scan = p["scan"]
            if scan is not None and scan.kind == "found":
                self._fw_found(ver, scan)
            elif scan is not None and scan.kind == "cancelled":
                self._fw_cancel_view(ver)
            else:
                self._fw_no_match(ver, (), scan)
        elif ev == "error":
            self._fw_error_view(ver, p)
        elif ev == "plan":
            if p["ok"]:
                self._launch(service.do_restore, {"firmware": None})
            else:
                self._restore_why = p["why"]
                self._build_config()
                self._ready_render()

    def _fw_space(self, ver, exc, retry):
        where = getattr(exc, "where", None) or state._state_base()
        if isinstance(exc, firmware_resolve.InsufficientSpace):
            shortfall = -(-max(exc.needed - exc.available, 1) // MB)
            text = ("Not enough space to save the firmware. "
                    "Free %d MB in %s, then retry." % (shortfall, where))
        else:
            text = ("Not enough space to save the firmware in %s. "
                    "Free some space, then retry." % where)
        self._fw_show(
            [(text, "err")], [("Retry", retry)],
            row=("%s firmware not downloaded yet" % self._fw_name(ver),
                 "Free some space, then press Retry."))

    def _fw_cancel_view(self, ver):
        self._fw_show([("Cancelled.", "text")], self._fw_fallbacks())

    def _fw_error_view(self, ver, p):
        exc, kind, name = p["exc"], p["kind"], self._fw_name(ver)
        if kind == "cancel":
            return self._fw_cancel_view(ver)
        if kind == "space":
            return self._fw_space(ver, exc, self._fw_get_click)
        row = ("%s firmware not downloaded yet" % name, "Press Get firmware to retry, or Browse for a ZIP.")
        if kind == "tls":
            return self._fw_show([("Could not verify FiiO's secure connection. Check this computer's date or use the "
                                   "browser download.", "err")], self._fw_fallbacks(), row=row)
        msg = str(getattr(exc, "message", exc)).rstrip(".")
        first = [("Could not download firmware: %s." % (msg[:1].lower() + msg[1:]), "err")]
        scan = p["scan"]
        if scan is not None and scan.kind == "found":
            self._fw_found(ver, scan, first)
        elif scan is not None and scan.kind == "cancelled":
            self._fw_cancel_view(ver)
        else:
            self._fw_no_match(ver, first, scan)

    def _pick_firmware(self):
        from tkinter import filedialog
        if self._fw_busy:
            return
        p = filedialog.askopenfilename(title="Select FiiO firmware .zip",
                                       filetypes=[("Firmware zip", "*.zip"), ("All", "*")])
        if p:
            self._active_fw_var().set(p)
            if self.fw_get is not None:
                self._fw_manual()

    def _active_fw_var(self):
        """The firmware field of the screen on show (Install and Restore never share one)."""
        return self.restore_fw_var if self.mode.get() == "restore" else self.fw_var

    def _fw_error(self, msg=""):
        """Inline message under the firmware box (shown on an empty Install click)."""
        lbl = getattr(self, "fw_err", None)
        if lbl is None:
            return
        lbl.config(text=msg)
        if msg:
            lbl.grid()
        else:
            lbl.grid_remove()

    # ---- ready check -------------------------------------------------------
    def _ready_pending(self):
        windows = platform_probe.host()[0] == "windows"
        return {"os": platform_probe.host()[0], "count": None, "access": None, "inv": None,
                "inv_error": "Windows USB state has not been read yet" if windows else None,
                "missing_tools": [], "pending": True}

    def _ready_rows(self):
        fw = getattr(self, "restore_fw_var" if self.mode.get() == "restore" else "fw_var", None)
        try:
            have = state.stock_image_exists() if self.mode.get() == "restore" else False
        except Exception:
            have = False
        return readiness.evaluate(self._ready_probe or self._ready_pending(), self.mode.get(),
                                  fw.get() if fw is not None else "", have, self._fw_ready())

    def _ready_render(self):
        """Redraw the checklist from the last probe (pure Tk, no USB). Hidden on the Remove screen."""
        frame = getattr(self, "ready_frame", None)
        if frame is None or self._run_view:
            return
        if self.mode.get() == "remove":
            frame.pack_forget()
            return
        if not frame.winfo_manager():
            frame.pack(fill="x", padx=gui_theme.GUTTER, before=self.cfg)
        import tkinter as tk
        ttk = self.ttk
        rows = self._ready_rows()
        view = (self.mode.get(), self._ready_probe is None, self.state == RUNNING,
                tuple(tuple(row.items()) for row in rows))
        bad = [r for r in rows if r["ok"] is False]
        if self._ready_probe is None:
            title, colour = "Checking...", SUBFG
        elif bad:
            title, colour = "%d to sort out - you can still continue" % len(bad), WARN
        elif rows[0]["ok"]:
            title, colour = "Ready - " + rows[0]["text"], OKC       # everything fine: one line, rows collapsed
        else:
            title, colour = "Checking...", SUBFG
        gui_theme.set_status(self.ready_title, title, colour)
        if view == self._ready_view:
            return
        self._ready_view = view
        for w in self.ready_rows.winfo_children():
            w.destroy()
        sp = gui_theme.SP
        # an empty pack master keeps its old size, so the rows frame is only shown while it has rows
        if bad:
            self.ready_rows.pack(fill="x", padx=gui_theme.PANEL_PAD, pady=(sp["s"], 0), after=self._ready_head)
        else:
            self.ready_rows.pack_forget()
        for r in bad:                                               # only what needs attention, with its fix
            line = ttk.Frame(self.ready_rows, style="Panel.TFrame")
            line.pack(fill="x", pady=(sp["xs"], 0))
            ttk.Label(line, image=gui_theme.dot(line, WARN), style="Panel.TLabel").pack(side="left", anchor="n",
                                                                                    pady=(gui_theme.dot_top(line), 0))
            col = ttk.Frame(line, style="Panel.TFrame")
            col.pack(side="left", fill="x", expand=True, anchor="n")
            short = len(r["text"]) + len(r["fix"]) <= 56           # fix beside the text when both are short
            top = ttk.Frame(col, style="Panel.TFrame")
            top.pack(anchor="w")
            ttk.Label(top, text=r["text"], style="Panel.TLabel", wraplength=440, justify="left").pack(
                side="left", anchor="n")
            if r["fix"]:
                fix = ttk.Label((top if short else col), text=r["fix"], style="PanelSub.TLabel", wraplength=440,
                                justify="left")
                if short:
                    fix.pack(side="left", anchor="n", padx=(sp["m"], 0))
                else:
                    fix.pack(anchor="w")
            if r["action"] == "driver":
                b = ttk.Button(line, text="Set up USB driver...", style="Small.TButton",
                               command=lambda: self._on_driver_setup())
                b.pack(side="right", anchor="n")
                if self.state == RUNNING:
                    b.config(state="disabled")
        if readiness.needs_flash_mode_help(rows):
            self.ready_help.pack(fill="x", padx=(gui_theme.PANEL_PAD + gui_theme.status_indent(self.ready_help), 0),
                                 pady=(sp["m"], 0))
        else:
            self.ready_help.pack_forget()

    def _ready_start(self):
        """One background job; Windows inventory and readiness share the existing worker."""
        if (getattr(self, "ready_frame", None) is None or self._closing
                or self.state == RUNNING or self._ready_running):
            return
        windows = platform_probe.host()[0] == "windows"
        self._ready_running = True
        self._inventory_running = windows
        self._ready_gen += 1
        gen = self._ready_gen
        cancel = self._ready_cancel = threading.Event()

        def run():
            inventory, p = None, None
            try:
                if windows:
                    try:
                        inventory = windows_usb.driver_inventory()
                        text = windows_usb.driver_description(inventory)
                    except windows_usb.PreflightError as e:
                        text = str(e)
                    except Exception:
                        text = "[E126] Windows could not read the USB driver state - reconnect the Disc and check again"
                    self.q.put(("driver_inventory", {"gen": gen, "text": text, "inv": inventory}))
                    if inventory is None:
                        inventory = text
                if not cancel.is_set():
                    p = readiness.probe(inventory)
            except Exception as e:
                p = {"os": platform_probe.host()[0], "count": -1, "access": None, "inv": None,
                     "inv_error": str(e), "missing_tools": []}
            finally:
                self.q.put(("ready", {"gen": gen, "probe": None if cancel.is_set() else p}))

        worker = threading.Thread(target=run, daemon=True)
        if windows:
            self._inventory_worker = worker
        worker.start()

    def _ready_apply(self, p):
        if p["gen"] != self._ready_gen:
            return
        cancelled = self._ready_cancel is not None and self._ready_cancel.is_set()
        self._ready_running = self._inventory_running = False
        self._ready_cancel = None
        if cancelled or self._closing or self.state == RUNNING or p["probe"] is None:
            return
        self._ready_probe = p["probe"]
        self._ready_render()

    def _on_ready_refresh(self):
        if self.state == RUNNING:
            return
        gui_theme.set_status(self.ready_title, "Checking...", SUBFG)
        self._refresh_state_labels()

    def _variant_warn(self):
        lbl = getattr(self, "variant_warn_lbl", None)
        if lbl is None:
            return
        if self.variant.get() == "dev":
            self._toggle_advanced(True)
            lbl.grid()
            lbl.config(text="Dev: always-on passwordless USB root. Development devices only.")
        else:
            lbl.config(text="")
            lbl.grid_remove()

    def _refresh_state_labels(self):
        o, a = platform_probe.host()
        if o == "windows":
            self.host_lbl.config(text=f"{o}-{a} - v{__version__} - {platform_probe.host_status()}")
            self._ready_start()
            return
        if getattr(self, "ready_frame", None) is not None:
            self.host_lbl.config(text=f"{o}-{a} - v{__version__}")
            self._ready_start()          # counts devices off the UI thread; the host line follows
            return
        n = platform_probe.maskrom_count()
        dev = {0: "no device in flash mode", -1: "USB not enumerable"}.get(n, f"{n} device(s) in flash mode")
        self.host_lbl.config(text=f"{o}-{a} - v{__version__} - {dev}")
        self.host_lbl.config(foreground=OKC if n == 1 else SUBFG)

    def _on_mode(self):
        self._build_config()
        m = self.mode.get()
        title, intro, label = MODE_TEXT[m]
        gui_theme.set_display(self.title_lbl, title)
        self.intro_lbl.config(text=intro)
        self.action_btn.config(text=label)
        for key, btn in self._action_btns.items():      # every screen except the current one
            btn.pack_forget()
            if key != m:
                btn.pack(side="left", padx=(0, gui_theme.SP["xl"]))
        self._ready_render()

    def _log(self, line, color=None):
        self.log.config(state="normal")
        self.log.insert("end", line + "\n")
        # cap the log so it can't grow unbounded
        if int(self.log.index("end-1c").split(".")[0]) > 500:
            self.log.delete("1.0", "100.0")
        self.log.see("end")
        self.log.config(state="disabled")

    # ---- action / worker ---------------------------------------------------
    def _on_driver_setup(self):
        if self.state == RUNNING:
            return
        self.state, self._driver_running = RUNNING, True
        self.start_time = time.monotonic()
        self._set_controls(False)
        self._clear_finish_extra()
        self.phase_lbl.config(text="Checking USB driver...")
        self._sync_progress()
        def run():
            try:
                result = windows_usb.setup_driver(
                    self.reporter.status,
                    lambda: self._worker_confirm({"action": "Set up USB driver",
                        "consequence": "Bind only the mask-ROM Disc to Microsoft's WinUSB driver.",
                        "requires": "Windows administrator approval or credentials. No flash will start."}))
                self.q.put(("driver_done", result))
            except Exception as e:
                self.q.put(("failed", {"error": str(e)}))
        self.worker = threading.Thread(target=run, daemon=True)
        self.worker.start()

    def _fw_gate(self, fw):
        """Install/Restore never runs on an unverified path: verify first (then continue), and check space."""
        if not fw:
            self._log("Choose your FiiO firmware .zip first.")
            self._fw_error("Choose your FiiO firmware .zip first.")
            return False
        ticket = getattr(self, "_fw_install_ticket", None)
        self._fw_install_ticket = None
        if self._fw_verified != fw or ticket != (self._fw_tok, fw):
            self._fw_manual(then_install=True)
            return False
        ver = self._fw_version()
        try:
            self._fr.check_space(ver, fw)
        except firmware_resolve.InsufficientSpace as exc:
            self._fw_space(ver, exc, self._on_action)
            return False
        return True

    def _on_action(self):
        if self.state == RUNNING or self._fw_busy:
            return
        m = self.mode.get()
        params = {}
        if m == "install":
            fw = getattr(self, "fw_var", None) and self.fw_var.get().strip()
            if not self._fw_gate(fw):
                return
            params = {"firmware": fw, "variant": self.variant.get()}
            params.update(ota_params(self.ota_var.get(), self.ota_key))
            fn = service.do_install
        elif m == "restore":
            fw = getattr(self, "restore_fw_var", None)       # never the Install page's field
            if fw is None:                                   # saved image: plan it off the UI thread
                tok, _ = self._fw_begin("plan", [("Checking the saved stock image...", "text")], None)

                def plan():
                    try:
                        self._fr.plan_restore(None)
                        self._fw_post(tok, "plan", ok=True)
                    except Exception as exc:
                        why = str(getattr(exc, "message", exc))
                        self._fw_post(tok, "plan", ok=False,
                                      why=why[len("Cannot restore: "):] if why.startswith("Cannot restore: ") else why)
                threading.Thread(target=plan, daemon=True).start()
                return
            if not self._fw_gate(fw.get().strip()):
                return
            params = {"firmware": fw.get().strip()}
            fn = service.do_restore
        else:
            params = {"force": False}
            fn = service.do_remove
        self._launch(fn, params)

    def _launch(self, fn, params):
        self.state = RUNNING
        self.start_time = time.monotonic()
        self._stage_start, self._flash_start, self._expect_secs, self._progress = self.start_time, None, None, None
        self._set_controls(False)
        self._clear_finish_extra()
        self.bar.config(mode="determinate", value=0)
        self.status_lbl.config(text="")
        self.elapsed_lbl.config(text="")
        self.phase_lbl.config(text="Starting...")
        self._toggle_details(False)
        self._enter_run_view()
        self._sync_progress()

        def run():
            try:
                r = fn(params, self.reporter, self._worker_confirm)
                self.q.put(("done", r))
            except Exception as e:  # BuildError/FlashError/etc.
                # the engine's own fields travel with the text so the result screen can show them apart
                self.q.put(("failed", {"error": str(e), "code": getattr(e, "code", None),
                                       "message": getattr(e, "message", None),
                                       "action": getattr(e, "action", None),
                                       "pre_launch": bool(getattr(e, "pre_launch", False))}))

        self.worker = threading.Thread(target=run, daemon=True)
        self.worker.start()

    def _set_controls(self, enabled):
        """Enable/disable all pre-flight controls (mode + config + action) so nothing
        can be changed or re-triggered while a worker is running."""
        st = "normal" if enabled else "disabled"
        if not enabled and self._ready_cancel is not None:
            self._ready_cancel.set()

        def walk(w):
            for c in w.winfo_children():
                cls = c.winfo_class()
                if cls in ("TRadiobutton", "TButton", "TEntry", "TCheckbutton",
                           "Radiobutton", "Button", "Entry", "Checkbutton"):
                    try:
                        c.config(state=st)
                    except Exception:
                        pass
                walk(c)
        walk(self.mode_frame)
        walk(self.cfg)
        if getattr(self, "ready_frame", None) is not None:
            walk(self.ready_frame)
        cb = getattr(self, "ota_cb", None)
        if cb is not None and not self.ota_key:
            cb.state(["disabled"])          # no release key: stays off and locked
        self.action_btn.config(state=st)
        if self.driver_btn is not None:
            self.driver_btn.config(state=st)

    def _clear_finish_extra(self):
        """Remove a previous result (chip, success card / error text, footer) and restore the progress widgets."""
        self.chip.pack_forget()
        self.result_box.pack_forget()
        for c in self.result_box.winfo_children():
            c.destroy()
        if self.foot.winfo_manager():
            self.foot.pack_forget()
            self.root.after_idle(self._fit_body)
        for c in self.foot.winfo_children():
            c.destroy()
        gui_theme.retitle(self.phase_lbl, self.phase_lbl.cget("text"), "h")
        if not self.bar.winfo_manager():
            for w in (self.status_lbl, self.bar, self.elapsed_lbl):
                w.pack_forget()
            self.status_lbl.pack(anchor="w", pady=(0, gui_theme.SP["m"]))
            self.bar.pack(fill="x")
            self.elapsed_lbl.pack(anchor="w", pady=(gui_theme.SP["xs"], gui_theme.SP["m"]))

    def _enter_run_view(self):
        """Hide the welcome group so the run/result gets the window. Whatever is packed is remembered and
        put back in order by _leave_run_view."""
        if self._run_view:
            return
        keep = (self._brand, self.prog)
        self._saved_pack = []
        for w in self.body.pack_slaves():
            if w in keep:
                continue
            info = w.pack_info()
            info.pop("in", None)
            self._saved_pack.append((w, info))
            w.pack_forget()
        self._run_view = True
        self._scroll_top()

    def _leave_run_view(self):
        if not self._run_view:
            return
        self._clear_finish_extra()
        for w, info in self._saved_pack:
            w.pack(before=self.prog, **info)
        self._saved_pack = []
        self._run_view = False
        self._scroll_top()

    def _back_to_start(self):
        """Back from a failed result to the welcome screen (never offered after an unknown result)."""
        if self.state == RUNNING:
            return
        self.state = IDLE
        self._leave_run_view()
        self.bar.config(mode="determinate", value=0)
        self.phase_lbl.config(text="Ready.")
        self.status_lbl.config(text="")
        self.elapsed_lbl.config(text="")
        self._toggle_details(False)
        self._sync_progress()

    def _on_destroy(self, event):
        if event.widget is not self.root:
            return
        with self._confirm_lock:
            self._closing = True
            if self._confirm_pending and not self._confirm_event.is_set():
                self._confirm_result = False
                self._confirm_event.set()

    def _worker_confirm(self, summary):
        """Worker only. One outstanding request; closing or overlapping requests answer no."""
        with self._confirm_lock:
            if self._closing or self._confirm_pending:
                return False
            self._confirm_pending = True
            self._confirm_result = False
            self._confirm_event.clear()
            self.q.put(("confirm", summary))
        try:
            # No timeout can turn a pending decision into consent. Destroy always wakes us.
            self._confirm_event.wait()
            with self._confirm_lock:
                return self._confirm_result
        finally:
            with self._confirm_lock:
                self._confirm_pending = False

    def _show_confirm_dialog(self, summary):
        """Only an acknowledged Begin can accept; every dialog failure rejects the request."""
        with self._confirm_lock:
            if self._closing or not self._confirm_pending or self._confirm_event.is_set():
                return
        if self._confirm_cancel is not None:
            return
        dlg = ack = None
        destructive = False
        done = [False]

        def _resolve(val):
            if done[0]:
                return
            accepted = False
            try:
                if val and destructive and not ack.get():
                    return
                done[0] = True
                if val and destructive:
                    self.flashing = True
                    self._show_flash_ui()
                if dlg is not None:
                    dlg.destroy()
                accepted = bool(val)
            except Exception:
                done[0] = True
            finally:
                if done[0]:
                    if not accepted:
                        if destructive:
                            self.flashing = False
                            try:
                                self._hide_flash_ui()
                            except Exception:
                                pass
                        if dlg is not None:
                            try:
                                dlg.destroy()
                            except Exception:
                                pass
                    self._confirm_cancel = None
                    with self._confirm_lock:
                        if self._confirm_pending and not self._confirm_event.is_set():
                            self._confirm_result = accepted and not self._closing
                            self._confirm_event.set()

        def guard(callback):
            def guarded(*args):
                try:
                    return callback(*args)
                except Exception:
                    _resolve(False)
                    return "break"
            return guarded

        self._confirm_cancel = lambda: _resolve(False)
        try:
            import tkinter as tk
            action = summary.get("action", "confirm")
            destructive = action in ("install", "restore-stock")
            scale = max(1.0, self.root.winfo_fpixels("1i") / 96)
            W = int(gui_theme.DIALOG_W * scale)
            dlg = tk.Toplevel(self.root)
            dlg.bind("<Destroy>", lambda e: _resolve(False) if e.widget is dlg else None)
            dlg.title("Confirm")
            gui_theme.style_dialog(dlg)
            dlg.transient(self.root)
            dlg.resizable(False, False)
            ack = tk.BooleanVar(master=dlg, value=not destructive)
            body = self._build_confirm_summary(dlg, summary, destructive, W, scale)
            ack_cb, cancel, begin = self._build_confirm_actions(
                body, destructive, ack, W, scale, _resolve, guard)

            def _enter(_e):
                f = dlg.focus_get()
                if f is begin:
                    if str(begin["state"]) == "normal":
                        _resolve(True)
                else:
                    _resolve(False)
                return "break"

            dlg.update_idletasks()
            h = dlg.winfo_reqheight()
            self.root.update_idletasks()
            px, py = self.root.winfo_rootx(), self.root.winfo_rooty()
            pw = self.root.winfo_width()
            dlg.geometry(f"{W}x{h}+{px + max(0, (pw - W) // 2)}+{py + 110}")
            dlg.grab_set()
            dlg.protocol("WM_DELETE_WINDOW", lambda: _resolve(False))
            dlg.bind("<Escape>", lambda e: _resolve(False))
            dlg.bind("<Return>", guard(_enter))
            (ack_cb if destructive else begin).focus_force()
        except Exception:
            _resolve(False)

    def _build_confirm_summary(self, dlg, summary, destructive, W, scale):
        """Title, what will happen (rows), and the warning. Returns the body frame."""
        ttk = self.ttk
        th = gui_theme
        action = summary.get("action", "confirm")
        padx, pady = th.DIALOG_PAD
        inner_w = W - 2 * padx
        body = ttk.Frame(dlg)
        body.pack(fill="both", expand=True, padx=padx, pady=pady)
        text = CONFIRM_TITLE.get(action, action)
        if action == "install" and summary.get("variant"):
            text += f" ({summary['variant']})"
        th.title(body, text, level="title", wrap=inner_w).pack(anchor="w", pady=(0, th.SP["l"]))
        rows = [("Updates over Wi-Fi", summary.get("ota")), ("Duration", summary.get("duration")),
                ("Firmware base" if destructive else "Needs", summary.get("requires"))]
        if action == "install" and summary.get("variant") == "dev":
            rows.insert(0, ("Variant", DEV_CONFIRM_NOTE))
        kv = ttk.Frame(body)
        kv.pack(fill="x")
        n = 0
        for label, value in rows:
            if value:
                th.kicker(kv, label).grid(row=n, column=0, sticky="nw", pady=th.SP["s"])
                ttk.Label(kv, text=value, wraplength=inner_w - int(th.KV_LABEL_W * scale), justify="left").grid(
                    row=n, column=1, sticky="nw", padx=(th.SP["l"], 0), pady=th.SP["s"])
                n += 1
        kv.columnconfigure(1, weight=1)
        if summary.get("consequence"):
            if destructive:
                box = th.panel(body, "warn")
                box.pack(fill="x", pady=(th.SP["l"], 0))
                th.panel_row(box, head="Warning.", text=summary["consequence"], head_kind="warn",
                             wrap=inner_w - 2 * th.SP["xs"])
            else:
                ttk.Label(body, text=summary["consequence"], wraplength=inner_w, justify="left").pack(
                    anchor="w", pady=(th.SP["l"], 0))
        return body

    def _build_confirm_actions(self, body, destructive, ack, W, scale, resolve, guard):
        """Acknowledgement + Cancel/Begin. Tab order = creation order: acknowledgement, Cancel, Begin."""
        ttk = self.ttk
        th = gui_theme
        ack_cb = None
        begin = None
        if destructive:
            ack_cb = th.acknowledgement(
                body, ACK_TEXT, ack, guard(lambda: begin.config(state=("normal" if ack.get() else "disabled"))))
            ack_cb.pack(anchor="w", pady=(th.SP["xl"], 0))
        th.rule(body).pack(fill="x", pady=(th.SP["xl"], th.SECTION))
        row = ttk.Frame(body)
        row.pack(fill="x")
        cancel = ttk.Button(row, text="Cancel", command=lambda: resolve(False))
        begin = ttk.Button(row, text=(BEGIN_TEXT if destructive else "Proceed"),
                           style="Accent.TButton", command=lambda: resolve(True))
        begin.pack(side="right")
        cancel.pack(side="right", padx=(0, th.SP["l"]))        # Cancel sits left of Begin
        if destructive:
            begin.config(state="disabled")
        return ack_cb, cancel, begin

    # ---- queue drain (main thread) ----------------------------------------
    def _drain(self):
        budget = 200   # bounded per tick so a flood of events can't starve Tk
        try:
            while budget > 0:
                kind, payload = self.q.get_nowait()
                budget -= 1
                try:
                    self._handle(kind, payload)
                except Exception as e:   # one malformed event must not stop the drain
                    self._log(f"[ui error handling {kind}: {e}]")
        except queue.Empty:
            pass
        self.root.after(80, self._drain)

    def _handle(self, kind, p):
        self._handle_event(kind, p)
        self._sync_progress()

    def _handle_event(self, kind, p):
        if kind == "phase":
            self.phase_lbl.config(text=flash_phase_text(p["name"], self.mode.get()))
            self._stage_start, self._progress = time.monotonic(), None
            if p.get("destructive"):
                self.flashing = True
                self.bar.config(mode="indeterminate")
                self.bar.start(12)
                self._show_flash_ui()
            self._log("-> " + p["name"])
        elif kind == "status":
            self.status_lbl.config(text=p["message"])
            self._log(p["message"])
        elif kind == "log":
            self._log("   " + p["line"])
        elif kind == "progress":
            if str(self.bar["mode"]) == "indeterminate":
                self.bar.stop(); self.bar.config(mode="determinate")
            total = p["total"] or 1
            self.bar.config(maximum=total, value=p["completed"])
            self._progress = (p["completed"], total)
            self.elapsed_lbl.config(text=self._timing_text(time.monotonic()))
        elif kind == "indeterminate":
            if p["active"]:
                self.bar.config(mode="indeterminate"); self.bar.start(12)
                self.status_lbl.config(text=stage_text(p.get("note", "")))
                self._expect_secs = p.get("expect_secs")
                self.elapsed_lbl.config(text=self._timing_text(time.monotonic()))
            else:
                self.bar.stop(); self.bar.config(mode="determinate")
        elif kind == "warning":
            self._log("! " + p["message"])
        elif kind == "ok":
            self._log("OK " + p["message"])
        elif kind == "error":
            self._log("ERROR " + p["message"])
        elif kind == "confirm":
            self._show_confirm_dialog(p)
        elif kind == "done":
            self._finish(p)
        elif kind == "driver_inventory":
            if p["gen"] != self._ready_gen:
                return
            self._inventory_running = False
            if (self._closing or self.state == RUNNING
                    or (self._ready_cancel is not None and self._ready_cancel.is_set())):
                return
            self.driver_lbl.config(text=p["text"])
            self.driver_lbl.pack(anchor="w", pady=(6, 2))
        elif kind == "ready":
            self._ready_apply(p)
        elif kind == "fw":
            self._fw_event(p)
        elif kind == "driver_done":
            self._driver_running = False
            self.state = IDLE
            self._set_controls(True)
            self.phase_lbl.config(text="Restart Windows to finish driver setup." if p.get("reboot_required")
                                  else "Driver setup cancelled." if p.get("aborted") else "USB check complete.")
            self.status_lbl.config(text=p["message"], wraplength=520)
            self._log(p["message"])
            self._refresh_state_labels()
        elif kind == "failed":
            self._failure_context = "driver" if self._driver_running else None
            self._driver_running = False
            info = {k: v for k, v in p.items() if k != "error"}
            if info:
                self._fail(p.get("error", "unknown error"), info)
            else:
                self._fail(p.get("error", "unknown error"))

    # ---- result screens ----------------------------------------------------
    def _show_flash_ui(self):
        """The do-not-unplug banner and the close-blocked note; shown from the instant the flash is confirmed."""
        self.warn_banner.config(text=FLASH_KEEP_NOTE)
        self._ensure_head()
        if not self.warn_banner.winfo_manager():
            self.warn_banner.pack(fill="x", pady=(gui_theme.SP["l"], gui_theme.SP["l"]),
                                  before=self.prog_head)
        if not self.lock_lbl.winfo_manager():
            self.lock_lbl.pack(anchor="w", pady=(0, gui_theme.SP["m"]), before=self.logwrap)

    def _hide_flash_ui(self):
        self.warn_banner.pack_forget()
        self.lock_lbl.pack_forget()

    def _timing_text(self, now):
        """elapsed (and, when the engine gave a basis, remaining) for the current stage."""
        if not self.start_time:
            return ""
        if self.flashing:
            if self._flash_start is None:
                self._flash_start = now
            secs = int(now - self._flash_start)
            txt = f"elapsed {secs // 60}m{secs % 60:02d}s"
            exp = self._expect_secs
            if exp:
                left = exp - secs
                if left > 0:
                    txt += f" - about {math.ceil(left / 60)} min left (estimate)"
                else:
                    txt += f" - longer than the usual {int(exp // 60)} minutes; keep waiting"
            return txt
        secs = int(now - self.start_time)
        txt = f"elapsed {secs // 60}m{secs % 60:02d}s"
        if self._progress and self._progress[1] > 0:
            c, t = self._progress
            txt += f" - {int(100 * min(c, t) / t)}%"
            spent = now - (self._stage_start or self.start_time)
            if c > 0 and 0.05 <= c / t < 1 and spent >= 3:
                left = int(spent * (t - c) / c)
                txt += (f" - about {left} s left" if left < 90 else f" - about {math.ceil(left / 60)} min left")
        return txt

    def _present(self, kind, chip, title, body=None, card=None, panel=None, note=None, back=False):
        """One result screen: status word, title, text, optional next-step card or bordered recovery panel, footer."""
        th = gui_theme
        self._clear_finish_extra()
        self._ensure_head()
        self.bar.pack_forget()
        self.elapsed_lbl.pack_forget()
        self.status_lbl.pack_forget()
        th.set_status_word(self.chip, chip, kind)
        self.chip.pack(anchor="w", pady=(0, th.SP["s"]), before=self.prog_head)
        th.retitle(self.phase_lbl, title, "title")
        self.status_lbl.config(text="")
        self._build_result_body(self.result_box, body, card, panel, note)
        self.result_box.pack(fill="x", pady=(th.SP["m"], th.SP["m"]), before=self.logwrap)
        if self._run_view:          # inline (driver setup) results have no footer: the window is still the welcome
            self._build_result_footer(kind, back)
        self._scroll_top()

    def _build_result_body(self, rb, body, card, panel, note):
        """Message, next-step card or recovery panel, and the short note under it."""
        ttk = self.ttk
        th = gui_theme
        if body:
            ttk.Label(rb, text=body, wraplength=WRAP, justify="left").pack(anchor="w", pady=(0, th.SP["m"]))
        if card:
            box = th.panel(rb, "quiet")
            box.pack(fill="x", pady=(th.SP["s"], 0))
            th.panel_row(box, head=card[0], text=card[1])
        if panel:
            head, text, border = panel
            box = th.panel(rb, "err" if border == "err" else "line")
            box.pack(fill="x", pady=(th.SP["s"], 0))
            th.panel_row(box, head=head, text=text, head_kind=("err" if border == "err" else "warn"))
        if note:
            ttk.Label(rb, text=note, style="Sub.TLabel", wraplength=WRAP, justify="left").pack(
                anchor="w", pady=(th.SP["m"], 0))

    def _build_result_footer(self, kind, back):
        ttk = self.ttk
        th = gui_theme
        th.rule(self.foot).pack(fill="x", pady=(0, th.SECTION))
        row = ttk.Frame(self.foot)
        row.pack(fill="x")
        # one rose primary per screen: Back when there is a way back to retry, else Close
        backb = ttk.Button(row, text="Back", command=self._back_to_start, style="Accent.TButton") if back else None
        close = ttk.Button(row, text="Close", command=self._on_close,
                           style=("Link.TButton" if backb is not None else "Accent.TButton"))
        if backb is not None:
            backb.pack(side="right")
            close.pack(side="left")
        else:
            close.pack(side="right")
        self.foot.pack(side="bottom", fill="x", padx=th.GUTTER, pady=(th.SP["s"], th.SP["xl"]), before=self._canvas)
        self.root.update_idletasks()
        self._fit_body()
        close.focus_set()

    def _finish(self, r):
        self.flashing = False
        if str(self.bar["mode"]) == "indeterminate":
            self.bar.stop()
        self._hide_flash_ui()
        self._set_controls(True)
        mode = self.mode.get()
        if r.get("aborted"):
            self.state = IDLE
            self.bar.config(mode="determinate", value=0)
            self._leave_run_view()
            self.phase_lbl.config(text="Cancelled - device unchanged.")
            self.elapsed_lbl.config(text="")
        elif r.get("errors"):        # e.g. uninstall couldn't remove everything
            self.state = FAILED
            self._toggle_details(True)
            self._present("err", "Problems", "Finished with problems",
                          body="Some installer data could not be removed. The Details below list each item.",
                          back=True)
        else:
            self.state = SUCCESS
            self.bar.config(mode="determinate", maximum=100, value=100)
            title, body, nxt = SUCCESS_TEXT.get(mode, SUCCESS_TEXT["install"])
            self._present("ok", "Success", title, body=body, card=("Next step", nxt))
        self._refresh_state_labels()

    def _fail(self, msg, info=None):
        was_flashing = self.flashing
        self.flashing = False
        try:
            self.bar.stop()
        except Exception:
            pass
        self._hide_flash_ui()
        self.state = FAILED
        self._set_controls(True)
        e = error_info(msg, info, flashing=was_flashing)
        code = e["code"]
        mode = self.mode.get()
        if e["unknown"]:
            title = "The flash result is unknown"
        elif getattr(self, "_failure_context", None) == "driver":
            title = "USB driver setup stopped"
        elif was_flashing or (code and (code.startswith("E3") or code[0] == "F")):
            title = "The flash did not complete"
        else:
            title = {"install": "The install stopped", "restore": "The restore stopped",
                     "remove": "The removal stopped"}.get(mode, "Something went wrong")
        self._failure_context = None
        panel = note = None
        if e["unknown"]:
            panel = ("What to do now.", display_sentence(e["action"]), "err")
            note = WAIT_NOTE
        elif e["action"]:
            panel = ("What to do now.", display_sentence(e["action"]), "line")
        if e["nothing_written"]:
            note = NOTHING_WRITTEN
        self._toggle_details(True)
        self._log("ERROR " + msg)
        self._present("err", f"Error {code}" if code else "Error", title, body=display_sentence(e["message"]), panel=panel,
                      note=note, back=not e["unknown"])

    def _tick_elapsed(self):
        if self.state == RUNNING and self.start_time:
            self.elapsed_lbl.config(text=self._timing_text(time.monotonic()))
        self.root.after(1000, self._tick_elapsed)

    def _tick_device(self):
        # keep the device-status line fresh, but never scan USB mid-operation
        if self.state != RUNNING:
            self._refresh_state_labels()
        self.root.after(4000, self._tick_device)

    def _on_close(self):
        from tkinter import messagebox
        if self._driver_running:
            messagebox.showwarning("Driver setup", "Wait for driver setup to finish. "
                                   "You can cancel the Windows approval prompt.")
            return
        if self.flashing:
            messagebox.showwarning(
                "Flash in progress",
                "A flash is in progress. Closing now can leave the device needing a "
                "mask-ROM recovery. Please wait until it finishes.")
            return
        if self.state == RUNNING:
            if not messagebox.askokcancel(
                    "Operation in progress",
                    "An operation is still running. Close anyway?"):
                return
        self._closing = True
        self._fw_stop()
        if self._ready_cancel is not None:
            self._ready_cancel.set()
        self.root.destroy()


def main(run_lock=None):
    try:
        import tkinter as tk
    except Exception as e:
        sys.stderr.write(f"diskOS Installer: Tkinter unavailable ({e}). Use the CLI: "
                         "diskos-installer --help\n")
        return 2
    try:
        root = tk.Tk()
        App(root, run_lock)
        root.mainloop()
        return 0
    except Exception as e:
        if getattr(sys.stderr, "_diskos_log", False):
            raise  # The windowed bootstrap owns sanitized logging and its dialog.
        # A packaged GUI hides early tracebacks - persist one and show a dialog.
        import tempfile
        import traceback
        # Unique, 0600 crash log (a fixed name in a world-writable temp dir could be pre-placed as a
        # symlink to truncate an arbitrary target). The exact path is shown in the message below.
        try:
            fd, logp = tempfile.mkstemp(prefix="diskos-installer-startup-", suffix=".log")
            with os.fdopen(fd, "w") as f:
                f.write(traceback.format_exc())
        except OSError:
            logp = "(could not write a log file)"
        sys.stderr.write(f"diskOS Installer failed to start: {e}\n(log: {logp})\n"
                         "Try the CLI: diskos-installer doctor\n")
        try:
            import tkinter.messagebox as mb
            mb.showerror("diskOS Installer",
                         f"Failed to start:\n{e}\n\nDetails: {logp}\n"
                         "You can still use the command line: diskos-installer doctor")
        except Exception:
            pass
        return 1


if __name__ == "__main__":
    sys.exit(main())
