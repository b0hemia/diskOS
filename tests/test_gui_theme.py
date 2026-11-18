# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Theme tables and the Welcome mode mapping of the GUI (display tests skip without Tk/display)."""
import os
import sys
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from diskos_installer import errors, gui, gui_theme as t, service  # noqa: E402
gui_theme_mod = t

BUTTONS = ("TButton", "Accent.TButton")
CHOICES = ("TRadiobutton", "Panel.TRadiobutton", "TCheckbutton", "Panel.TCheckbutton")


class ThemeTables(unittest.TestCase):
    def test_tokens(self):
        for name, val in (("BG", "#050506"), ("SURFACE", "#121216"), ("HOVER", "#1c1c20"),
                          ("PRESSED", "#242428"), ("TEXT", "#f5f5f7"), ("SECONDARY", "#b9b8b4"),
                          ("DIVIDER", "#29292f"), ("LINE", "#5f5f68"), ("ACCENT", "#ff375f"),
                          ("FOCUS", "#ffb0c0"), ("WARN", "#ffcc00"), ("ERR", "#ff453a"), ("OK", "#34c759"),
                          ("KICKER", "#908f8b"), ("PANEL", "#050506")):
            self.assertEqual(getattr(t, name), val)

    def test_every_state_has_explicit_colours(self):
        for name in BUTTONS:
            self.assertEqual(set(t.STYLES[name]), {"normal", "hover", "pressed", "disabled", "focus"}, name)
        for name in CHOICES:
            self.assertEqual(set(t.STYLES[name]),
                             {"normal", "hover", "pressed", "disabled", "focus", "selected"}, name)
        self.assertTrue({"normal", "disabled", "focus"} <= set(t.STYLES["TEntry"]))
        for name, states in t.STYLES.items():
            for st, pair in states.items():
                self.assertEqual(len(pair), 2, (name, st))
                for c in pair:
                    self.assertRegex(c, r"^#[0-9a-f]{6}$", (name, st))

    def test_body_text_contrast(self):
        for name, states in t.STYLES.items():
            for st, (fg, bg) in states.items():
                if st == "disabled":
                    continue
                self.assertGreaterEqual(t.contrast(fg, bg), 4.5, (name, st, fg, bg))
        for fg, bg in (t.LOG, t.BANNER_WARN, t.BANNER_OK, (t.WARN, t.SURFACE), (t.SECONDARY, t.SURFACE),
                       (t.OK, t.BG), (t.ON_ACCENT, t.ACCENT_HOVER), (t.WARN, t.PANEL), (t.ERR, t.PANEL),
                       (t.SECONDARY, t.PANEL), (t.KICKER, t.BG), (t.ON_ACCENT, t.ACCENT_PRESSED)):
            self.assertGreaterEqual(t.contrast(fg, bg), 4.5, (fg, bg))

    def test_disabled_and_outlines_stay_visible(self):
        for name in BUTTONS + CHOICES:
            fg, bg = t.STYLES[name]["disabled"]
            self.assertGreaterEqual(t.contrast(fg, bg), 3.0, name)
        self.assertGreaterEqual(t.contrast(t.LINE, t.BG), 3.0)       # interactive outline
        self.assertGreaterEqual(t.contrast(t.FOCUS, t.BG), 3.0)      # focus ring
        self.assertGreaterEqual(t.contrast(t.ACCENT, t.SURFACE), 3.0)  # selected indicator


class NoLiteralLook(unittest.TestCase):
    def test_gui_py_has_no_hard_coded_colours_or_fonts(self):
        import re
        src = open(os.path.join(os.path.dirname(gui.__file__), "gui.py")).read()
        self.assertEqual(re.findall(r"#[0-9a-fA-F]{6}", src), [])
        self.assertEqual(re.findall(r"font=\(", src), [])
        self.assertEqual(re.findall(r"""family\s*=""", src), [])


class AsciiGuiText(unittest.TestCase):
    def test_decoded_gui_string_literals_are_ascii(self):
        import ast
        for module in (gui, t):
            tree = ast.parse(open(module.__file__).read())
            for node in ast.walk(tree):
                if isinstance(node, ast.Constant) and isinstance(node.value, str):
                    self.assertTrue(node.value.isascii(), (module.__name__, node.lineno, node.value))


class ValidationRefusalText(unittest.TestCase):
    def test_e304_pre_launch_metadata_suppresses_unknown_guidance_while_flashing(self):
        message = "The flash could not start because Windows flash validation failed. " \
                  "Nothing was sent by the flash transfer."
        action = "See Details for the validation failure. Correct the reported problem " \
                 "and retry; no flash-recovery wait is needed."
        error = errors.FlashError(message, code="E304", action=action)
        info = gui.error_info(str(error), {"code": error.code, "message": error.message,
                              "action": error.action, "pre_launch": True}, flashing=True)
        self.assertEqual(info["code"], "E304")
        self.assertTrue(info["nothing_written"])
        self.assertFalse(info["unknown"])
        self.assertEqual(info["action"], action)
        self.assertNotIn(errors.WAIT_THEN_RECOVER, info["action"])
        # A text-only E304 cannot claim that the transfer never started.
        self.assertFalse(gui.error_info(str(error), flashing=True)["nothing_written"])


class FontChoice(unittest.TestCase):
    """make_fonts(): on Windows use Segoe UI only when Tk lists it, else keep TkDefaultFont's family."""

    def _root(self):
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as e:
            self.skipTest("no Tk display: %s" % e)
        root.withdraw()
        self.addCleanup(root.destroy)
        return root

    def _family(self, platform, families, default="Default Sans"):
        """The default family is controlled too, so the result never depends on what this host's Tk resolved."""
        import tkinter.font as tkfont
        root = self._root()
        real = tkfont.nametofont

        def nametofont(name, *a, **k):
            if name == "TkDefaultFont":
                return mock.Mock(actual=lambda *x, **y: default)
            return real(name, *a, **k)
        with mock.patch.object(sys, "platform", platform), \
                mock.patch.object(tkfont, "families", return_value=families), \
                mock.patch.object(tkfont, "nametofont", nametofont):
            fonts = t.make_fonts(root)
        return default, fonts["b"].actual("family"), fonts["b"].cget("family")

    def test_windows_with_segoe(self):
        _, _, asked = self._family("win32", ("Arial", "Segoe UI"))
        self.assertEqual(asked, "Segoe UI")

    def test_windows_without_segoe_keeps_default(self):
        default, _, asked = self._family("win32", ("Arial", "Tahoma"), default="Tahoma")
        self.assertEqual(asked, default)
        self.assertNotEqual(asked, "Segoe UI")

    def test_other_platform_ignores_segoe(self):
        default, _, asked = self._family("linux", ("Segoe UI",))
        self.assertEqual(asked, default)


def _find(widget, cls=None, text=None):
    """All descendants (and widget itself) of a Tk class and/or exact text."""
    out = []
    for w in [widget] + list(widget.winfo_children()):
        if w is not widget:
            out += _find(w, cls, text)
            continue
        if (cls is None or w.winfo_class() == cls):
            try:
                ok = text is None or str(w.cget("text")) == text
            except Exception:
                ok = False
            if ok:
                out.append(w)
    return out


class AppFlows(unittest.TestCase):
    """The real App, driven through its widgets. Only the service layer is stubbed, and the stubs
    record their FULL arguments and call the confirm gate the way the engine does."""

    def setUp(self):
        self.calls = []                       # (name, params, rep, confirm, confirmed)
        self.gate = None                      # threading.Event: hold the fake service until set
        self.service_error = None             # str (RuntimeError) or an exception instance to raise
        self.after_confirm = None             # callable(rep) run after an accepted confirm (may raise)
        self.errors = []                      # messagebox.showerror calls
        self.close_answers = []               # askokcancel calls
        self.confirm_summary = {"action": "install", "variant": "public", "ota": "on", "duration": "20 min"}
        self.key = True
        self.saved_image = True

    # ---- harness ----
    def _make(self, key=True, saved_image=True, scaling=None):
        try:
            import tkinter as tk
            self.root = tk.Tk()
        except Exception as e:
            self.skipTest("no Tk display: %s" % e)
        if scaling:
            self.root.tk.call("tk", "scaling", scaling)
        self.root.withdraw()
        for target in (mock.patch.object(gui, "ota_key_available", return_value=key),
                       mock.patch.object(gui.readiness, "probe", return_value={
                           "os": "linux", "count": 0, "access": None, "inv": None,
                           "inv_error": None, "missing_tools": []}),
                       mock.patch.object(gui.platform_probe, "host", return_value=("linux", "x86_64")),
                       mock.patch.object(gui.state, "stock_image_exists", return_value=saved_image),
                       mock.patch.object(gui.firmware_resolve, "identify_zip", side_effect=lambda path, expected=None: gui.firmware_resolve.ZipIdentity(expected, path)),
                       mock.patch.object(gui.firmware_resolve, "check_space", return_value=None),
                       mock.patch.object(gui.firmware_resolve, "plan_restore", return_value=gui.firmware_resolve.RestorePlan("backup", "240")),
                       mock.patch.object(gui.service, "do_install", side_effect=self._svc("install")),
                       mock.patch.object(gui.service, "do_restore", side_effect=self._svc("restore")),
                       mock.patch.object(gui.service, "do_remove", side_effect=self._svc("remove")),
                       mock.patch("tkinter.messagebox.showerror",
                                  side_effect=lambda *a, **k: self.errors.append(a)),
                       mock.patch("tkinter.messagebox.showwarning"),
                       mock.patch("tkinter.messagebox.askokcancel",
                                  side_effect=lambda *a, **k: self.close_answers.pop(0))):
            target.start()
            self.addCleanup(target.stop)
        self.addCleanup(self._teardown)
        self.app = gui.App(self.root)
        self.app._tick_device = lambda: None
        self.root.update()
        return self.app

    def _teardown(self):
        if self.gate is not None:
            self.gate.set()
        w = self.app.worker
        if w is not None:
            w.join(5)
        try:
            self.root.destroy()
        except Exception:
            pass

    def _svc(self, name):
        def fake(params, rep, confirm):
            rec = [name, dict(params), rep, confirm, None]
            self.calls.append(rec)
            if self.gate is not None:
                rep.status("holding")
                self.gate.wait(10)
            rep.status("working")
            rep.log("detail line 1")
            rep.log("detail line 2")
            if self.service_error:
                raise (self.service_error if isinstance(self.service_error, BaseException)
                       else RuntimeError(self.service_error))
            summary = dict(self.confirm_summary)
            if name == "restore":
                summary["action"] = "restore-stock"
            if name == "remove":
                summary = {"action": "remove-tool", "consequence": "x"}
            rec[4] = bool(confirm(summary))
            if rec[4] and self.after_confirm:
                self.after_confirm(rep)
            return {"ok": True} if rec[4] else {"ok": False, "aborted": True}
        return fake

    def _pump(self, cond, what, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            self.root.update()
            if cond():
                return
            time.sleep(0.01)
        self.fail_("timeout waiting for " + what)

    def fail_(self, msg):
        self.fail_msg = msg
        raise AssertionError(msg)

    def _dialog(self):
        self._pump(lambda: [w for w in self.root.winfo_children() if w.winfo_class() == "Toplevel"],
                   "confirm dialog")
        return [w for w in self.root.winfo_children() if w.winfo_class() == "Toplevel"][0]

    def _answer(self, accept):
        dlg = self._dialog()
        if accept:
            for cb in _find(dlg, "TCheckbutton"):
                cb.invoke()                    # the "I understand" gate on destructive dialogs
            btn = [b for b in _find(dlg, "TButton") if str(b.cget("text")) in ("Proceed", "Begin flash (~20 min)")]
        else:
            btn = _find(dlg, "TButton", "Cancel")
        self.assertEqual(len(btn), 1)
        if accept:
            self.assertEqual(str(btn[0].cget("state")), "normal")
        btn[0].invoke()

    def _finish(self):
        self._pump(lambda: self.app.state != gui.RUNNING, "operation end")
        self.app.worker.join(5)

    def _button(self, text):
        b = _find(self.root, "TButton", text)
        self.assertEqual(len(b), 1, text)
        return b[0]

    def _go(self, firmware=None):
        if firmware is not None:
            self.app.fw_var.set(firmware)
        self.app.action_btn.invoke()

    # ---- install ----
    def test_install_public_exact_arguments(self):
        app = self._make()
        self._go("/fw/disc.zip")
        self._answer(True)
        self._finish()
        (name, params, rep, confirm, confirmed), = self.calls
        self.assertEqual(name, "install")
        self.assertEqual(params, {"firmware": "/fw/disc.zip", "variant": "public", "no_ota": False})
        self.assertIs(rep, app.reporter)
        self.assertEqual(confirm, app._worker_confirm)
        self.assertIs(confirmed, True)
        self.assertEqual(app.state, gui.SUCCESS)

    def test_install_firmware_is_stripped(self):
        self._make()
        self._go("  /fw/disc.zip \n")
        self._answer(True)
        self._finish()
        self.assertEqual(self.calls[0][1]["firmware"], "/fw/disc.zip")

    def test_install_dev_through_radio_widget(self):
        app = self._make()
        _find(self.root, "TRadiobutton", "Dev (root shell)")[0].invoke()
        self.assertEqual(app.variant.get(), "dev")
        self._go("/fw/disc.zip")
        self._answer(True)
        self._finish()
        self.assertEqual(self.calls[0][1], {"firmware": "/fw/disc.zip", "variant": "dev", "no_ota": False})

    def test_install_ota_off_through_checkbox(self):
        app = self._make()
        cb = _find(self.root, "TCheckbutton", gui.OTA_LABEL)[0]
        self.assertTrue(app.ota_var.get())
        cb.invoke()
        self.assertFalse(app.ota_var.get())
        self._go("/fw/disc.zip")
        self._answer(True)
        self._finish()
        self.assertEqual(self.calls[0][1], {"firmware": "/fw/disc.zip", "variant": "public", "no_ota": True})

    def test_install_no_release_key_forces_ota_off_and_says_so(self):
        app = self._make(key=False)
        cb = _find(self.root, "TCheckbutton", gui.OTA_LABEL)[0]
        self.assertTrue(cb.instate(["disabled"]))
        self.assertFalse(app.ota_var.get())
        labels = [str(w.cget("text")) for w in _find(app.cfg, "Label")]
        self.assertIn(gui.OTA_NO_KEY, labels)
        self.assertNotIn(gui.OTA_HELP, labels)
        app.ota_var.set(True)                  # even if something ticks it, the install stays off
        self._go("/fw/disc.zip")
        self._answer(True)
        self._finish()
        self.assertTrue(self.calls[0][1]["no_ota"])
        self.assertTrue(cb.instate(["disabled"]), "no-key checkbox must stay locked after the run")

    def test_install_with_key_shows_help_not_nokey_message(self):
        app = self._make(key=True)
        labels = [str(w.cget("text")) for w in _find(app.cfg, "Label")]
        self.assertIn(gui.OTA_HELP, labels)
        self.assertNotIn(gui.OTA_NO_KEY, labels)

    def test_install_without_firmware_makes_no_service_call(self):
        app = self._make()
        self._go("   ")
        self.root.update()
        self.assertEqual(self.calls, [])
        self.assertEqual(app.state, gui.IDLE)
        self.assertEqual(str(app.fw_err.cget("text")), "Choose your FiiO firmware .zip first.")
        self.assertTrue(app.fw_err.grid_info())
        self.assertIn("Choose your FiiO firmware", app.log.get("1.0", "end"))

    def test_install_cancelled_at_confirmation(self):
        app = self._make()
        self._go("/fw/disc.zip")
        self._answer(False)
        self._finish()
        (name, params, _r, _c, confirmed), = self.calls
        self.assertIs(confirmed, False)
        self.assertFalse(app.flashing)
        self.assertEqual(app.state, gui.IDLE)
        self.assertIn("Cancelled", str(app.phase_lbl.cget("text")))
        self.assertEqual(self.errors, [])

    def test_destructive_accept_marks_flashing_synchronously(self):
        app = self._make()
        self.gate = None
        self._go("/fw/disc.zip")
        dlg = self._dialog()
        self.assertFalse(app.flashing)
        # Begin stays disabled until the acknowledgement is ticked
        begin = _find(dlg, "TButton", "Begin flash (~20 min)")[0]
        self.assertEqual(str(begin.cget("state")), "disabled")
        _find(dlg, "TCheckbutton")[0].invoke()
        begin.invoke()
        self.assertTrue(app.flashing)
        self._finish()

    # ---- restore ----
    def test_restore_with_saved_image(self):
        app = self._make(saved_image=True)
        self._button("Restore stock firmware...").invoke()
        self.assertEqual(str(app.action_btn.cget("text")), gui.MODE_TEXT["restore"][2])
        self.assertEqual(self.calls, [])       # selecting the mode starts nothing
        self.app.action_btn.invoke()
        self._answer(True)
        self._finish()
        (name, params, rep, confirm, confirmed), = self.calls
        self.assertEqual((name, params), ("restore", {"firmware": None}))
        self.assertIs(rep, app.reporter)
        self.assertEqual(confirm, app._worker_confirm)
        self.assertIs(confirmed, True)

    def test_restore_without_saved_image_uses_picked_firmware(self):
        app = self._make(saved_image=False)
        self._button("Restore stock firmware...").invoke()
        entries = _find(app.cfg, "TEntry")
        self.assertEqual(len(entries), 1)
        entries[0].insert(0, "/fw/stock.zip")
        self.app.action_btn.invoke()
        self._answer(True)
        self._finish()
        self.assertEqual(self.calls[0][:2], ["restore", {"firmware": "/fw/stock.zip"}])

    def test_restore_cancelled(self):
        app = self._make()
        self._button("Restore stock firmware...").invoke()
        app.action_btn.invoke()
        self._answer(False)
        self._finish()
        self.assertIs(self.calls[0][4], False)
        self.assertEqual(app.state, gui.IDLE)

    # ---- remove ----
    def test_remove_installer_data(self):
        app = self._make()
        self._button("Remove installer data...").invoke()
        self.assertEqual(str(app.action_btn.cget("text")), "Remove installer data")
        self.assertEqual(self.calls, [])
        app.action_btn.invoke()
        self._answer(True)
        self._finish()
        (name, params, rep, confirm, confirmed), = self.calls
        self.assertEqual((name, params), ("remove", {"force": False}))
        self.assertIs(rep, app.reporter)
        self.assertIs(confirmed, True)
        self.assertEqual(app.state, gui.SUCCESS)

    def test_remove_cancelled(self):
        app = self._make()
        self._button("Remove installer data...").invoke()
        app.action_btn.invoke()
        self._answer(False)
        self._finish()
        self.assertIs(self.calls[0][4], False)
        self.assertEqual(app.state, gui.IDLE)

    def test_back_to_install_returns_to_install_config(self):
        app = self._make()
        self._button("Remove installer data...").invoke()
        self._button("Back to install").invoke()
        self.assertEqual(app.mode.get(), "install")
        self.assertEqual(str(app.action_btn.cget("text")), "Install diskOS...")
        self.assertEqual(self.calls, [])

    # ---- Dev warning ----
    def test_dev_warning_only_for_dev(self):
        app = self._make()
        lbl = app.variant_warn_lbl
        self.assertEqual(str(lbl.cget("text")), "")
        self.assertEqual(lbl.winfo_manager(), "")            # no empty gap while Public is selected
        _find(self.root, "TRadiobutton", "Dev (root shell)")[0].invoke()
        self.assertIn("always-on passwordless USB root", str(lbl.cget("text")))
        self.assertIn("Development devices only.", str(lbl.cget("text")))
        self.assertEqual(lbl.winfo_manager(), "grid")
        _find(self.root, "TRadiobutton", "Public (recommended)")[0].invoke()
        self.assertEqual(str(lbl.cget("text")), "")
        self.assertEqual(lbl.winfo_manager(), "")

    # ---- busy state ----
    def _controls(self):
        """Every widget the pre-slice _set_controls disabled: mode_frame + cfg descendants of the
        listed classes, plus the action button (and the Windows driver button when present)."""
        classes = ("TRadiobutton", "TButton", "TEntry", "TCheckbutton", "Radiobutton", "Button",
                   "Entry", "Checkbutton")
        out = []

        def walk(w):
            for c in w.winfo_children():
                if c.winfo_class() in classes:
                    out.append(c)
                walk(c)
        walk(self.app.mode_frame)
        walk(self.app.cfg)
        out.append(self.app.action_btn)
        return out

    @staticmethod
    def _disabled(w):
        if w.winfo_class() in ("TButton", "TEntry", "TCheckbutton", "TRadiobutton"):
            return w.instate(["disabled"])
        return str(w.cget("state")) == "disabled"

    def test_busy_state_disables_every_control_and_restores_them(self):
        app = self._make()
        ctrls = self._controls()
        # install mode: Restore, Remove, (hidden) Back, Browse, entry, 2 radios, OTA checkbox, action
        desc = sorted(w.winfo_class() + ":" + (str(w.cget("text")) if w.winfo_class() != "TEntry" else "")
                      for w in ctrls)
        self.assertEqual(desc, sorted([
            "TButton:Restore stock firmware...", "TButton:Remove installer data...", "TButton:Back to install",
            "TButton:Browse...", "TButton:Get firmware", "TEntry:", "TRadiobutton:Public (recommended)", "TButton:Advanced",
            "TRadiobutton:Dev (root shell)", "TCheckbutton:" + gui.OTA_LABEL, "TButton:Install diskOS..."]))
        self.assertFalse(any(self._disabled(w) for w in ctrls))
        self.gate = threading.Event()
        self._go("/fw/disc.zip")
        self._pump(lambda: self.calls, "service start")
        self.assertEqual(app.state, gui.RUNNING)
        self.assertTrue(all(self._disabled(w) for w in ctrls),
                        [str(w) for w in ctrls if not self._disabled(w)])
        # a second click while running starts nothing
        app.action_btn.config(state="normal")
        app.action_btn.invoke()
        self.root.update()
        self.assertEqual(len(self.calls), 1)
        self.gate.set()
        self._answer(True)
        self._finish()
        self.assertFalse(any(self._disabled(w) for w in ctrls))

    def test_busy_state_no_key_checkbox_stays_locked(self):
        app = self._make(key=False)
        cb = app.ota_cb
        self.gate = threading.Event()
        self._go("/fw/disc.zip")
        self._pump(lambda: self.calls, "service start")
        self.assertTrue(cb.instate(["disabled"]))
        self.gate.set()
        self._answer(True)
        self._finish()
        self.assertTrue(cb.instate(["disabled"]))

    def test_busy_state_blocks_mode_switch_buttons(self):
        app = self._make()
        self.gate = threading.Event()
        self._go("/fw/disc.zip")
        self._pump(lambda: self.calls, "service start")
        before = app.mode.get()
        for text in ("Restore stock firmware...", "Remove installer data..."):
            self._button(text).invoke()      # ttk invoke() on a disabled button does nothing
        self.assertEqual(app.mode.get(), before)
        self.gate.set()
        self._answer(True)
        self._finish()

    # ---- details / log ----
    def test_log_lines_queued_while_collapsed_all_appear_when_expanded(self):
        app = self._make()
        self.assertFalse(app._details_open)
        for i in range(30):
            app.reporter.log("queued %d" % i)
        app.reporter.status("a status")
        self.assertEqual(app.log.get("1.0", "end").strip(), "")     # still queued, not drained
        self._pump(lambda: "queued 29" in app.log.get("1.0", "end"), "drain")
        self.assertFalse(app._details_open)
        self.assertEqual(app.logbox.winfo_manager(), "")            # hidden
        app.details_btn.invoke()
        self.assertTrue(app._details_open)
        self.assertEqual(app.logbox.winfo_manager(), "pack")
        text = app.log.get("1.0", "end")
        for i in range(30):
            self.assertIn("queued %d" % i, text)
        self.assertIn("a status", text)
        self.assertEqual(str(app.details_btn.cget("text")), "Details")
        self.assertTrue(app.details_btn._diskos_open)              # chevron turned down
        app.details_btn.invoke()
        self.assertFalse(app._details_open)
        self.assertEqual(app.logbox.winfo_manager(), "")

    def test_service_log_lines_reach_the_log(self):
        app = self._make()
        self._go("/fw/disc.zip")
        self._answer(True)
        self._finish()
        self.root.update()
        text = app.log.get("1.0", "end")
        self.assertIn("detail line 1", text)
        self.assertIn("detail line 2", text)

    # ---- failure ----
    def test_failing_service_presents_error_and_opens_details(self):
        app = self._make()
        self.service_error = "disc exploded"
        self.assertFalse(app._details_open)
        self._go("/fw/disc.zip")
        self._finish()
        self.assertEqual(app.state, gui.FAILED)
        self.assertEqual(str(app.phase_lbl.cget("text")), "The install stopped")
        self.assertTrue(app._details_open)
        self.assertEqual(app.logbox.winfo_manager(), "pack")
        self.assertIn("disc exploded", app.log.get("1.0", "end"))
        self.assertEqual(self.errors, [])        # the result is on screen; no second pop-up
        self.assertIn("Disc exploded.", self._result_text())
        self.assertFalse(app.flashing)
        self.assertFalse(any(self._disabled(w) for w in self._controls()))

    def test_finish_with_errors_opens_details(self):
        app = self._make()
        app.q.put(("done", {"errors": ["could not remove x"]}))
        self._pump(lambda: app.state == gui.FAILED, "failed state")
        self.assertTrue(app._details_open)
        self.assertIn("Finished with problems", str(app.phase_lbl.cget("text")))

    # ---- window close ----
    def test_close_while_running_cancelled_keeps_window(self):
        app = self._make()
        self.gate = threading.Event()
        self._go("/fw/disc.zip")
        self._pump(lambda: self.calls, "service start")
        self.close_answers = [False]
        app._on_close()
        self.assertEqual(self.close_answers, [])
        self.assertTrue(self.root.winfo_exists())

    def test_close_while_running_accepted_destroys_window(self):
        app = self._make()
        self.gate = threading.Event()
        self._go("/fw/disc.zip")
        self._pump(lambda: self.calls, "service start")
        self.close_answers = [True]
        app._on_close()
        with self.assertRaises(Exception):
            self.root.winfo_exists()
            self.root.update()

    # ---- slice 3: the destructive gate ----
    def _texts(self, widget, classes=("Label", "TLabel")):
        out = []
        for w in [widget] + list(widget.winfo_children()):
            if w is not widget:
                out += self._texts(w, classes)
            elif w.winfo_class() in classes:
                out.append(str(w.cget("text")))
        return out

    def _result_text(self):
        return "\n".join(self._texts(self.app.result_box) + [str(self.app.phase_lbl.cget("text")),
                                                              str(self.app.chip.cget("text"))])

    def _open_gate(self, summary=None, mode=None):
        """Start the stubbed service and return (app, dialog) with the confirm gate on screen."""
        app = self._make()
        self.root.deiconify()                  # keys and focus reach a dialog only when its parent is on screen
        self.root.update()
        if summary:
            self.confirm_summary = summary
        if mode == "restore":
            self._button("Restore stock firmware...").invoke()
            app.action_btn.invoke()
        elif mode == "remove":
            self._button("Remove installer data...").invoke()
            app.action_btn.invoke()
        else:
            self._go("/fw/disc.zip")
        return app, self._dialog()

    def _focus(self, w):
        w.focus_force()
        self.root.update()

    def test_gate_states_the_action_and_starts_unticked(self):
        _app, dlg = self._open_gate({"action": "install", "variant": "public", "ota": "ON key 9c1e",
                                     "duration": "about 20 minutes", "consequence": "This rewrites it.",
                                     "requires": "The Disc must already run V2.40."})
        text = "\n".join(self._texts(dlg))
        for want in ("Install diskOS (public)", "ON key 9c1e", "about 20 minutes", "This rewrites it.",
                     "The Disc must already run V2.40.", "Warning."):
            self.assertIn(want, text)
        ack = _find(dlg, "TCheckbutton")
        self.assertEqual(len(ack), 1)
        self.assertEqual(str(ack[0].cget("text")), gui.ACK_TEXT)
        self.assertEqual(str(ack[0].tk.globalgetvar(str(ack[0].cget("variable")))), "0")      # unticked
        self.assertEqual(str(_find(dlg, "TButton", gui.BEGIN_TEXT)[0].cget("state")), "disabled")
        self._answer(False)
        self._finish()

    def test_gate_begin_follows_the_tick_both_ways(self):
        app, dlg = self._open_gate()
        ack, begin = _find(dlg, "TCheckbutton")[0], _find(dlg, "TButton", gui.BEGIN_TEXT)[0]
        ack.invoke()
        self.assertEqual(str(begin.cget("state")), "normal")
        ack.invoke()
        self.assertEqual(str(begin.cget("state")), "disabled")
        begin.invoke()                                  # a disabled ttk button does nothing
        self.root.update()
        self.assertFalse(app.flashing)
        self.assertIsNone(self.calls[0][4])
        self.assertTrue(dlg.winfo_exists())
        self._answer(False)
        self._finish()

    def test_gate_cannot_begin_with_the_tick_off_even_if_called_directly(self):
        app, dlg = self._open_gate()
        self._focus(_find(dlg, "TButton", gui.BEGIN_TEXT)[0])
        dlg.event_generate("<Return>")                  # Enter on a disabled Begin
        self.root.update()
        self.assertFalse(app.flashing)
        self.assertIsNone(self.calls[0][4])
        self.assertTrue(dlg.winfo_exists())
        self._answer(False)
        self._finish()

    def test_gate_guard_blocks_a_begin_that_bypasses_the_disabled_button(self):
        app, dlg = self._open_gate()
        begin = _find(dlg, "TButton", gui.BEGIN_TEXT)[0]
        begin.config(state="normal")                    # something enabled it without the tick
        begin.invoke()
        self.root.update()
        self.assertFalse(app.flashing)
        self.assertIsNone(self.calls[0][4])
        self.assertTrue(dlg.winfo_exists())
        self._answer(False)
        self._finish()

    def test_gate_escape_cancels(self):
        app, dlg = self._open_gate()
        self._focus(dlg)
        dlg.event_generate("<Escape>")
        self._finish()
        self.assertIs(self.calls[0][4], False)
        self.assertFalse(app.flashing)
        self.assertEqual(app.state, gui.IDLE)

    def test_gate_window_close_cancels(self):
        app, dlg = self._open_gate()
        dlg.tk.call(dlg.protocol("WM_DELETE_WINDOW"))
        self._finish()
        self.assertIs(self.calls[0][4], False)
        self.assertFalse(app.flashing)

    def test_gate_escape_cancels_even_when_ticked(self):
        app, dlg = self._open_gate()
        _find(dlg, "TCheckbutton")[0].invoke()
        self._focus(dlg)
        dlg.event_generate("<Escape>")
        self._finish()
        self.assertIs(self.calls[0][4], False)
        self.assertFalse(app.flashing)

    def test_gate_enter_on_a_non_begin_control_answers_no(self):
        for cls, text in (("TCheckbutton", None), ("TButton", "Cancel")):
            with self.subTest(control=cls):
                app, dlg = self._open_gate()
                _find(dlg, "TCheckbutton")[0].invoke()
                self._focus(_find(dlg, cls, text)[0])
                dlg.event_generate("<Return>")
                self._finish()
                self.assertIs(self.calls[0][4], False)
                self.assertFalse(app.flashing)
                self.root.destroy()
                self.calls.clear()

    def test_gate_second_confirm_answers_no_without_releasing_the_first(self):
        app, dlg = self._open_gate()
        answers = []
        second = threading.Thread(target=lambda: answers.append(app._worker_confirm({"action": "install"})),
                                  daemon=True)
        second.start()
        second.join(1)
        self.assertFalse(second.is_alive())
        self.assertEqual(answers, [False])
        self.assertIsNone(self.calls[0][4])
        self.assertTrue(dlg.winfo_exists())
        self._answer(True)
        self._finish()
        self.assertIs(self.calls[0][4], True)

    def test_gate_construction_exception_answers_no(self):
        app = self._make()
        with mock.patch.object(app, "_build_confirm_summary", side_effect=RuntimeError("dialog build")):
            self._go("/fw/disc.zip")
            self._finish()
        self.assertIs(self.calls[0][4], False)
        self.assertFalse(app.flashing)
        self.assertEqual(_find(self.root, "Toplevel"), [])

    def test_gate_acknowledgement_callback_exception_answers_no(self):
        app, dlg = self._open_gate()
        begin = _find(dlg, "TButton", gui.BEGIN_TEXT)[0]
        with mock.patch.object(begin, "config", side_effect=RuntimeError("button state")):
            _find(dlg, "TCheckbutton")[0].invoke()
        self._finish()
        self.assertIs(self.calls[0][4], False)
        self.assertFalse(app.flashing)

    def test_gate_acceptance_exception_answers_no_and_clears_flash_flag(self):
        app, dlg = self._open_gate()
        _find(dlg, "TCheckbutton")[0].invoke()
        with mock.patch.object(app, "_show_flash_ui", side_effect=RuntimeError("banner")):
            _find(dlg, "TButton", gui.BEGIN_TEXT)[0].invoke()
        self._finish()
        self.assertIs(self.calls[0][4], False)
        self.assertFalse(app.flashing)

    def test_gate_dialog_destroy_exception_still_answers_no(self):
        app, dlg = self._open_gate()
        _find(dlg, "TCheckbutton")[0].invoke()
        destroy = dlg.destroy
        with mock.patch.object(dlg, "destroy", side_effect=(RuntimeError("destroy"), None)):
            _find(dlg, "TButton", gui.BEGIN_TEXT)[0].invoke()
        destroy()
        self._finish()
        self.assertIs(self.calls[0][4], False)
        self.assertFalse(app.flashing)

    def test_gate_app_close_wakes_the_worker_with_no(self):
        app, _dlg = self._open_gate()
        self.close_answers.append(True)
        app._on_close()
        app.worker.join(1)
        self.assertFalse(app.worker.is_alive())
        self.assertIs(self.calls[0][4], False)
        self.assertFalse(app.flashing)

    def test_gate_app_destroy_before_the_queue_is_drained_answers_no(self):
        app = self._make()
        answers = []
        worker = threading.Thread(target=lambda: answers.append(app._worker_confirm({"action": "install"})),
                                  daemon=True)
        worker.start()
        while app.q.get(timeout=1)[0] != "confirm":      # skip readiness events
            pass
        self.root.destroy()
        worker.join(1)
        self.assertFalse(worker.is_alive())
        self.assertEqual(answers, [False])
        self.assertIs(app._worker_confirm({"action": "install"}), False)

    def test_gate_enter_on_enabled_begin_begins(self):
        app, dlg = self._open_gate()
        _find(dlg, "TCheckbutton")[0].invoke()
        self._focus(_find(dlg, "TButton", gui.BEGIN_TEXT)[0])
        dlg.event_generate("<Return>")
        self._finish()
        self.assertIs(self.calls[0][4], True)

    def test_gate_focus_order_and_initial_focus(self):
        _app, dlg = self._open_gate()
        order = []

        def walk(w):
            for c in w.winfo_children():
                if c.winfo_class() in ("TCheckbutton", "TButton"):
                    order.append(str(c.cget("text")))
                walk(c)
        walk(dlg)
        self.assertEqual(order, [gui.ACK_TEXT, "Cancel", gui.BEGIN_TEXT])      # tab order = creation order
        self.assertEqual(dlg.focus_get(), _find(dlg, "TCheckbutton")[0])
        cancel, begin = _find(dlg, "TButton", "Cancel")[0], _find(dlg, "TButton", gui.BEGIN_TEXT)[0]
        self.assertLess(cancel.winfo_rootx(), begin.winfo_rootx())              # Cancel sits left of Begin
        self._answer(False)
        self._finish()

    def test_gate_accept_returns_true_with_flag_set_before_release(self):
        app = self._make()
        seen = []

        class Rec(threading.Event):
            def set(ev):                                 # what the worker would observe the instant it wakes
                seen.append((app.flashing, app._confirm_result))
                super().set()
        app._confirm_event = Rec()
        self._go("/fw/disc.zip")
        self._answer(True)
        self._finish()
        self.assertEqual(seen, [(True, True)])
        self.assertIs(self.calls[0][4], True)

    def test_gate_cancel_returns_false_and_never_sets_the_flag(self):
        app = self._make()
        seen = []

        class Rec(threading.Event):
            def set(ev):
                seen.append((app.flashing, app._confirm_result))
                super().set()
        app._confirm_event = Rec()
        self._go("/fw/disc.zip")
        self._answer(False)
        self._finish()
        self.assertEqual(seen, [(False, False)])
        self.assertIs(self.calls[0][4], False)

    def test_gate_non_destructive_has_no_tick_and_does_not_flag_flashing(self):
        app, dlg = self._open_gate(mode="remove")
        self.assertEqual(_find(dlg, "TCheckbutton"), [])
        self.assertEqual(len(_find(dlg, "TButton", "Proceed")), 1)
        self.assertNotIn("Warning.", "\n".join(self._texts(dlg)))
        self.assertEqual(dlg.focus_get(), _find(dlg, "TButton", "Proceed")[0])
        _find(dlg, "TButton", "Proceed")[0].invoke()
        self._finish()
        self.assertFalse(app.flashing)
        self.assertIs(self.calls[0][4], True)

    def test_gate_restore_and_dev_variant_texts(self):
        _app, dlg = self._open_gate(mode="restore")
        self.assertIn("Restore stock firmware", "\n".join(self._texts(dlg)))
        self.assertEqual(len(_find(dlg, "TCheckbutton")), 1)
        self._answer(False)
        self._finish()
        self.root.destroy()
        self.calls.clear()
        _app, dlg = self._open_gate({"action": "install", "variant": "dev", "duration": "x", "ota": "y"})
        text = "\n".join(self._texts(dlg))
        self.assertIn("Install diskOS (dev)", text)
        self.assertIn(gui.DEV_CONFIRM_NOTE, text)
        self._answer(False)
        self._finish()

    def test_gate_double_resolve_answers_once(self):
        app, dlg = self._open_gate()
        _find(dlg, "TCheckbutton")[0].invoke()
        begin = _find(dlg, "TButton", gui.BEGIN_TEXT)[0]
        begin.invoke()
        try:
            begin.invoke()
        except Exception:
            pass                                         # already destroyed: fine
        self._finish()
        self.assertEqual(len(self.calls), 1)
        self.assertIs(self.calls[0][4], True)

    def test_gate_confirmed_flash_shows_do_not_unplug_at_once(self):
        app, dlg = self._open_gate()
        self.assertEqual(app.warn_banner.winfo_manager(), "")
        _find(dlg, "TCheckbutton")[0].invoke()
        _find(dlg, "TButton", gui.BEGIN_TEXT)[0].invoke()
        self.assertEqual(app.warn_banner.winfo_manager(), "pack")      # before the engine's first flash event
        self.assertEqual(str(app.warn_banner.cget("text")), gui.FLASH_KEEP_NOTE)
        self._finish()

    # ---- slice 4: progress ----
    def _emit(self, kind, **kw):
        self.app.q.put((kind, kw))
        self._pump(lambda: self.app.q.empty(), "drain")
        self.root.update()

    def test_progress_event_moves_the_bar_and_names_the_percent(self):
        app = self._make()
        app.state, app.start_time = gui.RUNNING, time.monotonic() - 5
        self._emit("phase", name="Extracting stock firmware", destructive=False)
        self._emit("progress", completed=30, total=100)
        self.assertEqual(str(app.bar["mode"]), "determinate")
        self.assertEqual(float(app.bar["value"]), 30.0)
        self.assertEqual(float(app.bar["maximum"]), 100.0)
        self.assertEqual(str(app.phase_lbl.cget("text")), "Extracting stock firmware")
        self.assertIn("30%", str(app.elapsed_lbl.cget("text")))
        self.assertIn("elapsed 0m0", str(app.elapsed_lbl.cget("text")))

    def test_remaining_time_from_progress_events(self):
        app = self._make()
        now = 1000.0
        app.start_time = app._stage_start = now - 20
        app._progress = (25, 100)
        self.assertEqual(app._timing_text(now), "elapsed 0m20s - 25% - about 60 s left")
        app._progress = (10, 1000)                       # under 5%: too little to extrapolate
        self.assertEqual(app._timing_text(now), "elapsed 0m20s - 1%")
        app._progress = (10, 100)
        app._stage_start = now - 1200
        self.assertEqual(app._timing_text(now), "elapsed 0m20s - 10% - about 180 min left")
        app._progress = (50, 100)
        app._stage_start = now - 1                       # too early to extrapolate: no remaining estimate
        self.assertNotIn("left", app._timing_text(now))

    def test_flash_elapsed_and_remaining_use_the_engine_expectation(self):
        app = self._make()
        app.state, app.start_time = gui.RUNNING, time.monotonic() - 9999
        self._emit("phase", name="Flashing (mask-ROM) - about 20 minutes", destructive=True)
        self._emit("indeterminate", active=True, note="flashing (scan -> erase)", expect_secs=1200)
        self.assertTrue(app.flashing)
        self.assertEqual(str(app.bar["mode"]), "indeterminate")
        self.assertEqual(str(app.status_lbl.cget("text")), "Flashing: scan, erase")
        self.assertEqual(str(app.phase_lbl.cget("text")), "Writing diskOS to the Disc - about 20 minutes")
        self.assertIn("about 20 min left (estimate)", str(app.elapsed_lbl.cget("text")))
        self.assertTrue(str(app.elapsed_lbl.cget("text")).startswith("elapsed 0m0"))   # since the flash began
        now = app._flash_start + 1000
        self.assertEqual(app._timing_text(now), "elapsed 16m40s - about 4 min left (estimate)")
        self.assertIn("longer than the usual 20 minutes; keep waiting", app._timing_text(app._flash_start + 1300))

    def test_flashing_screen_says_do_not_unplug_and_blocks_close(self):
        app = self._make()
        app.state = gui.RUNNING
        self._emit("phase", name="Flashing (mask-ROM) - about 20 minutes", destructive=True)
        self.assertEqual(app.warn_banner.winfo_manager(), "pack")
        self.assertEqual(str(app.warn_banner.cget("text")), gui.FLASH_KEEP_NOTE)
        self.assertEqual(app.lock_lbl.winfo_manager(), "pack")
        with mock.patch("tkinter.messagebox.showwarning") as warn:
            app._on_close()
        warn.assert_called_once()
        self.assertEqual(self.close_answers, [])                     # no "close anyway?" offer either
        self.assertTrue(self.root.winfo_exists())
        self.root.update()

    def test_close_is_vetoed_end_to_end_while_the_flash_runs(self):
        app = self._make()
        self.gate = threading.Event()
        hold = threading.Event()                        # keeps the stub "flashing" until the test says so
        self.after_confirm = lambda rep: (rep.phase("Flashing", destructive=True),
                                          rep.indeterminate(True, expect_secs=60), hold.wait(20))
        self._go("/fw/disc.zip")
        self._pump(lambda: self.calls, "service start")
        self.gate.set()
        self._answer(True)
        self._pump(lambda: app.flashing and str(app.bar["mode"]) == "indeterminate", "flashing", timeout=20)
        with mock.patch("tkinter.messagebox.showwarning") as warn:
            app._on_close()
        warn.assert_called_once()
        self.assertTrue(self.root.winfo_exists())
        hold.set()
        self._finish()
        self.assertEqual(app.state, gui.SUCCESS)
        self.assertFalse(app.flashing)

    def test_running_hides_the_welcome_and_shows_progress_only(self):
        app = self._make()
        before = self._pack_order(app)
        self.gate = threading.Event()
        self._go("/fw/disc.zip")
        self._pump(lambda: self.calls, "service start")
        for w in (app.action_btn, app.cfg, app.mode_frame):
            self.assertFalse(w.winfo_ismapped(), w)
        self.assertEqual(app.prog.winfo_manager(), "pack")
        self.gate.set()
        self._answer(False)
        self._finish()
        self.root.update()
        for w in (app.action_btn, app.cfg, app.mode_frame):      # cancel returns to the welcome, in order
            self.assertTrue(w.winfo_ismapped(), w)
        self.assertEqual(self._pack_order(app), before)

    # ---- slice 4: results ----
    @staticmethod
    def _pack_order(app):
        return [str(w) for w in app.body.pack_slaves()]

    def _run_to_result(self, firmware="/fw/disc.zip", accept=True):
        self._go(firmware)
        if accept:
            self._answer(True)
        self._finish()

    def _buttons(self, parent):
        return [str(b.cget("text")) for b in _find(parent, "TButton")]

    def test_success_screen_install(self):
        app = self._make()
        self._run_to_result()
        self.assertEqual(app.state, gui.SUCCESS)
        text = self._result_text()
        for want in ("Success", "diskOS is installed", "diskOS was written to the Disc and checked.",
                     "Next step", "Turn the Disc off and on again. On the first start it finishes setting up diskOS by itself."):
            self.assertIn(want, text)
        self.assertEqual(self._buttons(app.foot), ["Close"])
        self.assertEqual(app.foot.winfo_manager(), "pack")
        self.assertEqual(app.bar.winfo_manager(), "")
        self.assertFalse(app._details_open)
        self.assertEqual(str(app.chip.cget("text")), "Success")
        self.assertTrue(str(app.chip.cget("image")))                 # drawn status dot (STYLE.md: no glyphs)
        self.assertEqual(self.errors, [])

    def test_success_screen_restore_and_remove(self):
        app = self._make()
        self._button("Restore stock firmware...").invoke()
        app.action_btn.invoke()
        self._answer(True)
        self._finish()
        self.assertIn("Stock firmware restored", self._result_text())
        self.assertIn("Turn the Disc off and on again to start the stock firmware.", self._result_text())
        self.root.destroy()
        self.calls.clear()
        app = self._make()
        self._button("Remove installer data...").invoke()
        app.action_btn.invoke()
        self._answer(True)
        self._finish()
        self.assertIn("Installer data removed", self._result_text())
        self.assertIn("Saved firmware, recovery images, installer state and logs were removed from this computer.", self._result_text())
        self.assertIn("The installer app itself is kept. Delete it if you no longer need it.", self._result_text())

    def test_close_button_on_result_closes_the_window(self):
        app = self._make()
        self._run_to_result()
        _find(app.foot, "TButton", "Close")[0].invoke()
        with self.assertRaises(Exception):
            self.root.winfo_exists()
            self.root.update()

    def test_error_screen_shows_code_message_and_engine_action(self):
        app = self._make()
        self.service_error = errors.BuildError("this firmware version is not supported by this installer",
                                               code="E226", action="download the supported FiiO firmware .zip")
        self._run_to_result(accept=False)
        self.assertEqual(app.state, gui.FAILED)
        texts = self._texts(app.result_box)
        self.assertIn("Error E226", str(app.chip.cget("text")))
        self.assertIn("This firmware version is not supported by this installer.", texts)      # display capitalised
        self.assertIn("What to do now.", texts)
        self.assertIn("Download the supported FiiO firmware .zip.", texts)                 # display only
        self.assertIn(gui.NOTHING_WRITTEN, texts)
        self.assertNotIn(errors.WAIT_THEN_RECOVER, "\n".join(texts))                      # not an unknown result
        self.assertTrue(app._details_open)                                                # Details auto-expanded
        self.assertIn("[E226]", app.log.get("1.0", "end"))
        self.assertIn("this firmware version is not supported by this installer", app.log.get("1.0", "end"))   # log keeps engine text
        self.assertEqual(str(self.service_error).count("download the supported FiiO firmware .zip"), 1)
        self.assertEqual(sorted(self._buttons(app.foot)), ["Back", "Close"])
        self.assertEqual(self.errors, [])

    def test_display_helpers(self):
        self.assertEqual(gui.display_sentence("this is wrong"), "This is wrong.")
        self.assertEqual(gui.display_sentence("Already fine."), "Already fine.")
        self.assertEqual(gui.display_sentence("ask?"), "Ask?")
        self.assertEqual(gui.flash_phase_text("Flashing (mask-ROM) - about 20 minutes", "restore"),
                         "Writing stock firmware to the Disc - about 20 minutes")
        self.assertEqual(gui.flash_phase_text("Extracting stock firmware", "install"), "Extracting stock firmware")
        self.assertEqual(gui.stage_text("flashing (scan -> erase -> program -> verify -> retry)"),
                         "Flashing: scan, erase, program, verify, retry")

    def test_error_back_returns_to_the_welcome_and_a_new_run_works(self):
        app = self._make()
        self.service_error = errors.BuildError("bad", code="E226", action="fix it")
        self._run_to_result(accept=False)
        _find(app.foot, "TButton", "Back")[0].invoke()
        self.assertEqual(app.state, gui.IDLE)
        self.assertFalse(app._run_view)
        self.assertEqual(app.prog_head.winfo_manager(), "")           # idle again: no empty "Ready." + bar
        self.root.update(); self.assertTrue(app.action_btn.winfo_ismapped())
        self.assertEqual(app.foot.winfo_manager(), "")
        self.assertEqual(str(app.phase_lbl.cget("text")), "Ready.")
        self.assertEqual(app.result_box.winfo_children(), [])
        self.assertFalse(app._details_open)
        self.service_error = None
        self._go()
        self._answer(True)
        self._finish()
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(app.state, gui.SUCCESS)

    def test_cancel_at_confirm_goes_back_to_the_welcome(self):
        app = self._make()
        self._go("/fw/disc.zip")
        self._answer(False)
        self._finish()
        self.root.update(); self.assertTrue(app.action_btn.winfo_ismapped())
        self.assertEqual(app.foot.winfo_manager(), "")
        self.assertIn("Cancelled - device unchanged.", str(app.phase_lbl.cget("text")))

    UNKNOWN_CASES = (("E301", "flash result UNKNOWN: usbboot lost the device"),
                     ("E302", "short debug readback (12 bytes) - flash result UNKNOWN"),
                     ("E303", "flash timed out - the device stopped responding after 33 min. Result UNKNOWN."),
                     ("E321", "the writer did not report completion"),
                     ("E310", "flash result UNKNOWN or FAILED (device result [F000] UNKNOWN)"))

    def test_windows_validation_refusal_shows_nothing_written_without_wait(self):
        app = self._make()
        error = errors.FlashError(
            "The flash could not start because Windows flash validation failed. "
            "Nothing was sent by the flash transfer.", code="E304",
            action="See Details for the validation failure. Correct the reported problem "
                   "and retry; no flash-recovery wait is needed.")
        error.pre_launch = True
        def refuse(rep):
            rep.phase("Flashing", destructive=True)
            raise error
        self.after_confirm = refuse
        self._go("/fw/disc.zip")
        self._answer(True)
        self._finish()
        self.assertEqual(app.state, gui.FAILED)
        texts = self._texts(app.result_box)
        self.assertIn(gui.NOTHING_WRITTEN, texts)
        self.assertIn(error.action, texts)
        self.assertNotIn(errors.WAIT_THEN_RECOVER, texts)
        self.assertNotIn(gui.WAIT_NOTE, texts)
        self.assertEqual(str(app.phase_lbl.cget("text")), "The flash did not complete")
        self.assertIn("Error E304", str(app.chip.cget("text")))
        self.assertIn("Back", self._buttons(app.foot))
        self.assertEqual(self.errors, [])

    def test_unknown_result_codes_show_the_persistent_wait_panel(self):
        for code, message in self.UNKNOWN_CASES:
            with self.subTest(code=code):
                self.calls.clear()
                app = self._make()
                self.service_error = errors.FlashError(message, code=code, action=errors.WAIT_THEN_RECOVER)
                self.after_confirm = None
                self._go("/fw/disc.zip")
                self._finish()
                self.assertEqual(app.state, gui.FAILED)
                texts = self._texts(app.result_box)
                self.assertIn(errors.WAIT_THEN_RECOVER, texts)           # the engine's 25-minute text, verbatim
                self.assertIn("25 minutes", errors.WAIT_THEN_RECOVER)
                self.assertIn(gui.WAIT_NOTE, texts)
                self.assertIn(gui.display_sentence(message), texts)
                self.assertIn("Error " + code, str(app.chip.cget("text")))
                self.assertEqual(str(app.phase_lbl.cget("text")), "The flash result is unknown")
                self.assertEqual(self._buttons(app.foot), ["Close"])      # no Back / Retry after an unknown result
                self.assertNotIn(gui.NOTHING_WRITTEN, texts)
                self.assertTrue(app._details_open)
                # persistent: a panel in the window with a red border, not a pop-up; it survives time and Details
                panel = [w for w in app.result_box.winfo_children() if w.winfo_class() == "Frame"][0]
                self.assertEqual(str(panel.cget("highlightbackground")), gui_theme_mod.ERR)
                app._toggle_details(False)
                app._toggle_details(True)
                for _ in range(20):
                    self.root.update()
                    time.sleep(0.01)
                self.assertIn(errors.WAIT_THEN_RECOVER, self._texts(app.result_box))
                self.assertEqual(self.errors, [])
                self.root.destroy()

    def test_unknown_result_without_engine_action_still_shows_the_wait_text(self):
        app = self._make()
        self.service_error = errors.FlashError("flash result UNKNOWN: x", code="E301")
        self._go("/fw/disc.zip")
        self._finish()
        self.assertIn(errors.WAIT_THEN_RECOVER, self._texts(app.result_box))

    def test_text_only_unknown_failure_is_split_into_message_and_wait_text(self):
        app = self._make()
        msg = str(errors.FlashError("flash timed out", code="E303", action=errors.WAIT_THEN_RECOVER))
        app._fail(msg)
        texts = self._texts(app.result_box)
        self.assertIn("Flash timed out.", texts)
        self.assertIn(errors.WAIT_THEN_RECOVER, texts)
        self.assertEqual(sum(errors.WAIT_THEN_RECOVER in t for t in texts), 1)     # not repeated in the message

    def test_finished_writer_with_host_error_is_not_an_unknown_result(self):
        app = self._make()
        self.service_error = errors.FlashError("flash writer finished; host/reporting error: x", code="E301",
                                               action=errors.RECOVERABLE)
        self._go("/fw/disc.zip")
        self._finish()
        texts = self._texts(app.result_box)
        self.assertIn(errors.RECOVERABLE, texts)
        self.assertNotIn(errors.WAIT_THEN_RECOVER, "\n".join(texts))
        self.assertEqual(str(app.phase_lbl.cget("text")), "The flash did not complete")
        self.assertIn("Back", self._buttons(app.foot))

    def test_uncoded_failure_during_the_flash_is_treated_as_unknown(self):
        app = self._make()

        def boom(rep):
            rep.phase("Flashing", destructive=True)
            raise RuntimeError("something odd")
        self.after_confirm = boom
        self._go("/fw/disc.zip")
        self._answer(True)
        self._finish()
        self.assertEqual(str(app.phase_lbl.cget("text")), "The flash result is unknown")
        self.assertIn(errors.WAIT_THEN_RECOVER, self._texts(app.result_box))
        self.assertFalse(app.flashing)
        self.assertEqual(app.warn_banner.winfo_manager(), "")

    def test_error_clears_the_flash_banner_and_lock_note(self):
        app = self._make()

        def boom(rep):
            rep.phase("Flashing", destructive=True)
            raise errors.FlashError("flash refused", code="F202", action="nothing written")
        self.after_confirm = boom
        self._go("/fw/disc.zip")
        self._answer(True)
        self._finish()
        self.assertEqual((app.warn_banner.winfo_manager(), app.lock_lbl.winfo_manager()), ("", ""))
        self.assertFalse(app.flashing)

    def test_problems_result_is_an_error_screen(self):
        app = self._make()
        app._enter_run_view()
        app.state = gui.RUNNING
        app.q.put(("done", {"errors": ["could not remove x"]}))
        self._pump(lambda: app.state == gui.FAILED, "failed state")
        self.assertIn("Finished with problems", self._result_text())
        self.assertEqual(sorted(self._buttons(app.foot)), ["Back", "Close"])

    # ---- height budget ----
    def _budget_states(self, app):
        """Drive the real handlers to each state; yield (name, shown) after the window settles."""
        def settle():
            for _ in range(6):
                self.root.update_idletasks()
                self.root.update()
        app.state, app.start_time = gui.RUNNING, time.monotonic()
        app._enter_run_view()
        app._handle("phase", {"name": "Building diskOS image (public)", "destructive": False})
        app._handle("progress", {"completed": 62, "total": 100})
        settle()
        yield "building"
        app._handle("phase", {"name": "Flashing (mask-ROM) - about 20 minutes", "destructive": True})
        app._handle("indeterminate", {"active": True, "note": "flashing (scan -> erase -> program -> verify -> retry)",
                                      "expect_secs": 1200})
        app._toggle_details(True)
        for i in range(60):
            app._log("   detail line %d with some text to fill the log box" % i)
        settle()
        yield "flashing details open"
        app._toggle_details(False)
        app._finish({"ok": True})
        settle()
        yield "success"
        app._back_to_start()
        app._on_action = None
        app._enter_run_view()
        app._fail("[E226] this firmware version is not supported by this installer - download the supported "
                  "FiiO firmware .zip", {"code": "E226", "action": "download the supported FiiO firmware .zip",
                                         "message": "this firmware version is not supported by this installer"})
        settle()
        yield "error"
        app._back_to_start()
        app._enter_run_view()
        app.flashing = True
        app._fail(str(errors.FlashError("flash timed out - the device stopped responding after 33 min (it most "
                                        "likely reset mid-flash). Result UNKNOWN.", code="E303",
                                        action=errors.WAIT_THEN_RECOVER)),
                  {"code": "E303", "action": errors.WAIT_THEN_RECOVER,
                   "message": "flash timed out - the device stopped responding after 33 min (it most likely "
                              "reset mid-flash). Result UNKNOWN."})
        settle()
        yield "unknown result"

    def test_height_budget_at_100_percent(self):
        """At 100% (96 dpi = Tk scaling 4/3) every run/result state fits a 1366x768 laptop: <= 700 px, no scrolling."""
        app = self._make(scaling=96 / 72)
        for name, in ((n,) for n in self._budget_states(app)):
            h = self.root.winfo_reqheight()
            self.assertLessEqual(h, gui.MAX_WINDOW_H, (name, h))
            self.assertFalse(app._scrolling, name)

    def test_height_budget_at_125_percent_scrolls_instead_of_clipping(self):
        app = self._make(scaling=120 / 72)
        scrolled = []
        for name in self._budget_states(app):
            for _ in range(4):
                self.root.update()
            h = self.root.winfo_reqheight()
            self.assertLessEqual(h, gui.MAX_WINDOW_H, (name, h))
            inner, view = app.body.winfo_reqheight(), int(app._canvas.cget("height"))
            if inner > view:                       # content taller than the window allows: it must scroll
                self.assertTrue(app._scrolling, name)
                self.assertEqual(app._vbar.winfo_manager(), "pack", name)
                scrolled.append(name)
            else:
                self.assertFalse(app._scrolling, name)
            self.assertEqual(app.foot.winfo_manager() == "pack", name in ("success", "error", "unknown result"))
        self.assertTrue(scrolled, "at 125% at least one state should need the scrollbar")

    def test_height_budget_literal_tk_scaling_1_25(self):
        app = self._make(scaling=1.25)
        for name in self._budget_states(app):
            self.assertLessEqual(self.root.winfo_reqheight(), gui.MAX_WINDOW_H, name)

    def test_confirm_dialog_fits_the_budget_at_both_scales(self):
        for scaling, action in ((dpi / 72, action) for dpi in (96, 120)
                                for action in ("install", "restore-stock")):
            with self.subTest(scaling=scaling, action=action):
                self.calls.clear()
                app = self._make(scaling=scaling)
                self.confirm_summary = {
                    "action": "install", "variant": "dev", "ota": "ON (accepts updates signed by the baked root "
                    "key, key sha256 9c1e5ab27f40d3e8...)", "duration": "about 20 minutes",
                    "consequence": service.INSTALL_CONSEQUENCE,
                    "requires": "The Disc must already run FiiO's full V2.40 (recovery 17) (kernel + recovery). "
                                "The writer checks this before changing anything and stops with nothing written "
                                "if it doesn't match."}
                if action == "restore-stock":
                    self.confirm_summary.update(action=action, consequence=service.RESTORE_CONSEQUENCE)
                    self.confirm_summary.pop("variant")
                    self.confirm_summary.pop("ota")
                    self._button("Restore stock firmware...").invoke()
                    app.action_btn.invoke()
                else:
                    self._go("/fw/disc.zip")
                dlg = self._dialog()
                self.root.update()
                self.assertLessEqual(dlg.winfo_height(), gui.MAX_WINDOW_H)
                self.assertGreater(dlg.winfo_height(), 200)
                self.assertIn(self.confirm_summary["consequence"], self._texts(dlg))
                title = ("Install diskOS (dev)" if action == "install" else "Restore stock firmware")
                self.assertTrue(str(_find(dlg, "TLabel", title)[0].cget("image")))
                for b in _find(dlg, "TButton"):                       # nothing clipped: buttons inside the dialog
                    self.assertLessEqual(b.winfo_rooty() + b.winfo_height(), dlg.winfo_rooty() + dlg.winfo_height())
                self._answer(False)
                self._finish()
                self.root.destroy()

    def test_result_stays_readable_when_text_is_long(self):
        app = self._make(scaling=120 / 72)
        app.state, app.start_time = gui.RUNNING, time.monotonic()
        app._enter_run_view()
        long = "x " * 400
        app._fail("[E226] " + long, {"code": "E226", "message": long, "action": long})
        for _ in range(6):
            self.root.update()
        self.assertLessEqual(self.root.winfo_reqheight(), gui.MAX_WINDOW_H)
        self.assertTrue(app._scrolling)
        self.assertEqual(app.foot.winfo_manager(), "pack")

    # ---- unchanged smoke ----
    def test_wordmark_loads_with_text_fallback(self):
        app = self._make()
        self.assertIsNotNone(app._wordmark)
        self.assertIn("brand", app.fonts)
        with mock.patch.object(t, "wordmark", return_value=None):
            root2 = self.root.__class__()
            root2.withdraw()
            self.addCleanup(root2.destroy)
            app2 = gui.App(root2)
            self.assertIsNone(app2._wordmark)
            self.assertIn("brand", app2.fonts)


if __name__ == "__main__":
    unittest.main()
