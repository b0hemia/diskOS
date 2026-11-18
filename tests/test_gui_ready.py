# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Ready checklist (slice 2): pure row logic, then the real App with only the probes mocked."""
import os
import queue
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from diskos_installer import gui, gui_theme, platform_probe, readiness  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), "..")


def linux(count=1, access=("ok", "/dev/bus/usb/001/005"), missing=()):
    return {"os": "linux", "count": count, "access": access if count == 1 else None, "inv": None,
            "inv_error": None, "missing_tools": list(missing)}


def win(state, count=None, missing=(), error=None):
    count = (1 if state in ("winusb", "driver") else 0) if count is None else count
    inv = None if error else {"state": state, "count": count, "present": [], "phantom": []}
    return {"os": "windows", "count": None if error else inv["count"], "access": None, "inv": inv,
            "inv_error": error, "missing_tools": list(missing)}


def rows(p, **kw):
    return {r["key"]: r for r in readiness.evaluate(p, **kw)}


class RowLogic(unittest.TestCase):
    def test_linux_device_states(self):
        self.assertIs(rows(linux(1))["device"]["ok"], True)
        r = rows(linux(0))
        self.assertIs(r["device"]["ok"], False)
        self.assertTrue(r["device"]["help"])
        self.assertIsNone(r["access"]["ok"])
        self.assertIn("2 devices", rows(linux(2))["device"]["text"])
        self.assertFalse(rows(linux(2))["device"]["help"])
        self.assertIs(rows(linux(-1))["device"]["ok"], False)
        self.assertFalse(rows(linux(-1))["device"]["help"])

    def test_linux_access(self):
        self.assertIs(rows(linux(1))["access"]["ok"], True)
        d = rows(linux(1, access=("denied", "/dev/bus/usb/001/005")))["access"]
        self.assertIs(d["ok"], False)
        self.assertIn("udev/70-diskos-maskrom.rules", d["fix"])
        self.assertIsNone(rows(linux(1, access=("unknown", "x")))["access"]["ok"])

    def test_windows_driver_states(self):
        self.assertIs(rows(win("winusb"))["access"]["ok"], True)
        d = rows(win("driver"))
        self.assertIs(d["access"]["ok"], False)
        self.assertEqual(d["access"]["action"], "driver")
        self.assertIs(d["device"]["ok"], True)
        for state in ("absent", "phantom"):
            r = rows(win(state))
            self.assertTrue(r["device"]["help"], state)
            self.assertIsNone(r["access"]["ok"], state)
        self.assertIn("disconnected", rows(win("phantom"))["device"]["text"])
        r = rows(win(None, error="[E126] Windows could not read the USB driver state"))
        self.assertIs(r["access"]["ok"], False)
        self.assertIsNone(r["device"]["ok"])
        self.assertIsNone(rows(win(None, error="Windows USB state has not been read yet"))["access"]["ok"])

    def test_firmware_rows(self):
        self.assertIs(rows(linux(1), firmware="")["firmware"]["ok"], False)
        # Selection is a pure check; preflight/build validate the actual file.
        with mock.patch.object(readiness.os.path, "isfile", side_effect=AssertionError("filesystem read")):
            r = rows(linux(1), firmware="/chosen/firmware.zip")["firmware"]
        self.assertIs(r["ok"], True)
        self.assertEqual(r["text"], "firmware.zip")
        self.assertIs(rows(linux(1), mode="restore", have_stock=True)["firmware"]["ok"], True)
        self.assertIs(rows(linux(1), mode="restore", have_stock=False)["firmware"]["ok"], False)

    def test_tools_row_names_missing_files(self):
        r = rows(linux(1, missing=["usbboot"]))["tools"]
        self.assertIs(r["ok"], False)
        self.assertIn("usbboot", r["text"])
        self.assertIs(rows(linux(1))["tools"]["ok"], True)

    def test_probe_is_read_only_and_survives_errors(self):
        for host in ("linux", "macos", "windows"):
            with self.subTest(host=host), \
                    mock.patch.object(platform_probe, "host", return_value=(host, "x86_64")), \
                    mock.patch.object(platform_probe, "maskrom_nodes", return_value=["/dev/fake"]), \
                    mock.patch.object(platform_probe, "maskrom_count", return_value=1), \
                    mock.patch.object(platform_probe, "maskrom_access", side_effect=AssertionError("USB open")), \
                    mock.patch.object(readiness.os, "open", side_effect=AssertionError("device open")), \
                    mock.patch.object(readiness.os, "chmod", side_effect=AssertionError("chmod")), \
                    mock.patch.object(readiness.os.path, "exists", return_value=True), \
                    mock.patch.object(readiness.os.path, "lexists", return_value=True), \
                    mock.patch.object(readiness.os.path, "isfile", return_value=True), \
                    mock.patch.object(readiness.os, "access", return_value=True):
                p = readiness.probe(win("winusb")["inv"] if host == "windows" else None)
                self.assertEqual(p["count"], 1)
                self.assertEqual(p["missing_tools"], [])
                if host == "linux":
                    self.assertEqual(p["access"][0], "ok")
        with mock.patch.object(platform_probe, "host", return_value=("linux", "x86_64")), \
                mock.patch.object(platform_probe, "maskrom_nodes", return_value=["/dev/fake"]), \
                mock.patch.object(readiness.os.path, "exists", return_value=True), \
                mock.patch.object(readiness.os, "access", return_value=False), \
                mock.patch.object(readiness, "_tools", side_effect=OSError("boom")):
            p = readiness.probe()
        self.assertEqual(p["missing_tools"], ["bundled tools"])
        self.assertEqual(p["access"][0], "denied")

    def test_code28_and_phantom_use_real_inventory_states(self):
        def row(problem=0, service="WinUSB"):
            return {"instance_id": r"USB\VID_A108&PID_EAEF\DISC",
                    "problem_code": problem, "service": service}
        api = mock.Mock()
        api.devices.return_value = [row(28, None)]
        inv = gui.windows_usb.inventory(api)
        self.assertEqual(inv["state"], "driver")
        self.assertEqual(rows(win("driver"))["access"]["action"], "driver")
        api.devices.side_effect = [[row()], []]
        inv = gui.windows_usb.inventory(api)
        self.assertEqual(inv["state"], "phantom")
        p = win("phantom")
        p["inv"] = inv
        self.assertIn("disconnected", rows(p)["device"]["text"])


class GuiReady(unittest.TestCase):
    def _app(self, host=("linux", "x86_64"), probe=None, tools=True, scale=1.0):
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as e:
            self.skipTest("no Tk display: %s" % e)
        root.withdraw()
        base = float(root.tk.call("tk", "scaling"))
        root.tk.call("tk", "scaling", base * scale)
        self.addCleanup(root.destroy)
        self.addCleanup(root.tk.call, "tk", "scaling", base)
        patches = [mock.patch.object(gui, "ota_key_available", return_value=True),
                   mock.patch.object(platform_probe, "host", return_value=host),
                   mock.patch.object(readiness, "probe", side_effect=lambda inv=None: probe or linux(0)),
                   mock.patch.object(gui.state, "stock_image_exists", return_value=False),
                   mock.patch.object(gui.windows_usb, "driver_inventory",
                                     return_value={"state": "absent", "count": 0, "present": [], "phantom": []}),
                   mock.patch.object(gui.windows_usb, "driver_description", return_value="x")]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        app = gui.App(root)
        app._tick_device = lambda: None
        return app

    def _settle(self, app, want="ready"):
        """Drain queued events until `want` has been handled and the queue stays quiet."""
        end, seen = time.time() + 5, False
        while time.time() < end:
            try:
                kind, p = app.q.get(timeout=0.3 if seen else 0.1)
            except queue.Empty:
                if seen:
                    return
                continue
            app._handle(kind, p)
            seen = seen or kind == want
        self.fail("no %s event" % want)

    def _texts(self, w, acc=None):
        acc = [] if acc is None else acc
        for c in w.winfo_children():
            try:
                acc.append(str(c.cget("text")))
            except Exception:
                pass
            self._texts(c, acc)
        return acc

    def test_probe_runs_off_the_ui_thread_and_marshals_back(self):
        threads = []
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as e:
            self.skipTest(str(e))
        root.withdraw()
        self.addCleanup(root.destroy)

        def fake(inv=None):
            threads.append(threading.get_ident())
            return linux(1)
        with mock.patch.object(gui, "ota_key_available", return_value=True), \
                mock.patch.object(readiness, "probe", side_effect=fake):
            app = gui.App(root)
            app._tick_device = lambda: None
            self._settle(app)
        self.assertNotIn(threading.get_ident(), threads)
        self.assertEqual(app.ready_title.cget("text"), "1 to sort out - you can still continue")   # firmware not chosen yet

    def test_no_device_shows_help_and_warns_without_blocking_install(self):
        app = self._app(probe=linux(0))
        self._settle(app)
        self.assertEqual(app.ready_help.winfo_manager(), "pack")
        text = " ".join(self._texts(app.ready_frame))
        for s in ("Disc in flash mode", "No Disc in flash mode yet", "Enter flash mode",
                  "1. Power the Disc OFF.", "2. Hold Volume Down.", "3. Plug in USB"):
            self.assertIn(s, text)
        self.assertIn("to sort out - you can still continue", text)
        self.assertEqual(str(app.action_btn.cget("state")), "normal")
        # empty firmware: Install is still clickable and only shows the inline message
        with mock.patch.object(gui.service, "do_install") as inst:
            app._on_action()
        inst.assert_not_called()
        self.assertEqual(app.fw_err.cget("text"), "Choose your FiiO firmware .zip first.")
        self.assertTrue(app.fw_err.grid_info())
        app.fw_var.set("/x.zip")
        self.assertFalse(app.fw_err.grid_info())

    def test_one_device_hides_help_and_all_ready(self):
        app = self._app(probe=linux(1))
        with tempfile.NamedTemporaryFile(suffix=".zip") as f:
            app.fw_var.set(f.name)
            self._settle(app)
            self.assertEqual(app.ready_help.winfo_manager(), "")
            self.assertEqual(app.ready_title.cget("text"), "Ready - 1 Disc found in flash mode")
            self.assertEqual(self._bad_rows(app), [])          # collapsed: one summary line only
            self.assertEqual(str(app.action_btn.cget("state")), "normal")

    def test_linux_access_denied_shows_fix_and_install_stays_enabled(self):
        app = self._app(probe=linux(1, access=("denied", "/dev/bus/usb/001/005")))
        self._settle(app)
        self.assertIn("sudo udevadm trigger", " ".join(self._texts(app.ready_rows)))
        self.assertEqual(str(app.action_btn.cget("state")), "normal")

    def test_missing_tools_warn(self):
        app = self._app(probe=linux(1, missing=["mksquashfs"]))
        self._settle(app)
        self.assertIn("Installer tools missing: mksquashfs", " ".join(self._texts(app.ready_rows)))
        self.assertEqual(str(app.action_btn.cget("state")), "normal")

    def test_windows_driver_missing_then_present(self):
        app = self._app(host=("windows", "x86_64"), probe=win("driver"))
        self.assertIsNone(app.driver_btn)          # the driver action lives in the checklist row
        self._settle(app)
        btns = [w for w in self._all(app.ready_rows) if w.winfo_class() == "TButton"]
        self.assertEqual([str(b.cget("text")) for b in btns], ["Set up USB driver..."])
        self.assertEqual(str(app.action_btn.cget("state")), "normal")
        with mock.patch.object(app, "_on_driver_setup") as setup:
            btns[0].invoke()
        setup.assert_called_once()
        with tempfile.NamedTemporaryFile(suffix=".zip") as f, \
                mock.patch.object(readiness, "probe", return_value=win("winusb")):
            app.fw_var.set(f.name)
            app._on_ready_refresh()
            self._settle(app)
            self.assertEqual(app.ready_title.cget("text"), "Ready - 1 Disc found in flash mode")
        self.assertFalse([w for w in self._all(app.ready_rows) if w.winfo_class() == "TButton"])

    def test_driver_row_calls_existing_flow_with_original_callbacks(self):
        app = self._app(host=("windows", "x86_64"), probe=win("driver"))
        self._settle(app)
        button = next(w for w in self._all(app.ready_rows) if w.winfo_class() == "TButton")
        reports = []
        def setup(report, confirm):
            reports.append(report)
            self.assertTrue(confirm())
            return {"aborted": True, "message": "Driver setup cancelled."}
        with mock.patch.object(gui.windows_usb, "setup_driver", side_effect=setup) as flow, \
                mock.patch.object(app, "_worker_confirm", return_value=True) as confirm:
            button.invoke()
            self._settle(app, want="driver_done")
        flow.assert_called_once()
        self.assertEqual(reports, [app.reporter.status])
        confirm.assert_called_once_with({
            "action": "Set up USB driver",
            "consequence": "Bind only the mask-ROM Disc to Microsoft's WinUSB driver.",
            "requires": "Windows administrator approval or credentials. No flash will start."})

    def _bad_rows(self, app):
        return [w for w in self._all(app.ready_rows) if w.winfo_class() == "TFrame"
                and w.winfo_parent() == str(app.ready_rows)]

    def _all(self, w):
        out = []
        for c in w.winfo_children():
            out.append(c)
            out += self._all(c)
        return out

    def test_probe_coalesces_refresh_and_tick_and_close_uses_no_worker_tk(self):
        for host in ("linux", "windows"):
            with self.subTest(host=host):
                app = self._app(host=(host, "x86_64"))
                self._settle(app)
                entered, release, finished = threading.Event(), threading.Event(), threading.Event()
                self.addCleanup(release.set)
                calls = []
                def slow(inv=None):
                    calls.append(threading.get_ident())
                    entered.set()
                    release.wait(5)
                    finished.set()
                    return linux(1) if host == "linux" else win("winusb")
                with mock.patch.object(readiness, "probe", side_effect=slow):
                    app._on_ready_refresh()
                    self.assertTrue(entered.wait(2))
                    for _ in range(4):
                        gui.App._tick_device(app)
                        app._on_ready_refresh()
                    self.assertEqual(len(calls), 1)
                    self.assertNotEqual(calls[0], threading.get_ident())
                    with mock.patch.object(app.root, "destroy") as destroy:
                        app._on_close()
                    destroy.assert_called_once()
                    self.assertTrue(app._ready_cancel.is_set())
                    release.set()
                    self.assertTrue(finished.wait(2))
                    self._settle(app)
                    self.assertFalse(app._ready_running)

    def test_running_rejects_probe_and_discards_in_flight_result(self):
        app = self._app()
        self._settle(app)
        old = app._ready_probe
        app.state = gui.RUNNING
        with mock.patch.object(readiness, "probe") as probe:
            app._ready_start()
            app._on_ready_refresh()
        probe.assert_not_called()
        app._ready_running = True
        app._ready_apply({"gen": app._ready_gen, "probe": linux(2)})
        self.assertIs(app._ready_probe, old)
        self.assertFalse(app._ready_running)
        app.state = gui.IDLE

    def test_restore_field_change_does_not_touch_destroyed_install_error(self):
        app = self._app()
        self._settle(app)
        errors = []
        app.root.report_callback_exception = lambda *args: errors.append(args)
        app._set_mode("restore")
        app.restore_fw_var.set("/chosen/restore.zip")
        self.assertEqual(errors, [])
        self.assertNotIn("No firmware chosen yet", " ".join(self._texts(app.ready_rows)))

    def test_unchanged_probe_preserves_focused_row_button(self):
        app = self._app(host=("windows", "x86_64"), probe=win("driver"))
        self._settle(app)
        buttons = [w for w in self._all(app.ready_rows) if w.winfo_class() == "TButton"]
        app._ready_render()
        self.assertTrue(buttons[0].winfo_exists())

    def test_scroll_follows_keyboard_focus_and_wheel_on_each_platform(self):
        from types import SimpleNamespace
        app = self._app(scale=1.25)
        self._settle(app)
        app.root.deiconify()
        app.root.geometry("620x360")
        app.root.update()
        self.assertTrue(app._scrolling)
        app._canvas.yview_moveto(0)
        # Exercise Tk's Tab traversal and verify the final control is visible.
        app.ready_refresh.focus_force()
        app.root.update()
        for _ in range(40):
            current = app.root.focus_get()
            if current is app.details_btn:
                break
            current.tk_focusNext().focus_set()
            app.root.update()
        self.assertIs(app.root.focus_get(), app.details_btn)
        self.assertGreater(app._canvas.yview()[0], 0)
        top = app.details_btn.winfo_rooty()
        self.assertGreaterEqual(top, app._canvas.winfo_rooty())
        self.assertLessEqual(top + app.details_btn.winfo_height(),
                             app._canvas.winfo_rooty() + app._canvas.winfo_height())
        for num, delta in ((4, 0), (5, 0), ("??", 120), ("??", -120), ("??", 1), ("??", -1)):
            with mock.patch.object(app._canvas, "yview_scroll") as scroll:
                app._on_wheel(SimpleNamespace(widget=app.action_btn, num=num, delta=delta))
            self.assertEqual(scroll.call_args.args, (-3 if num == 4 or delta > 0 else 3, "units"))

    def test_unchanged_refresh_restores_title_and_preserves_row_button(self):
        app = self._app(host=("windows", "x86_64"), probe=win("driver"))
        self._settle(app)
        title = str(app.ready_title.cget("text"))
        button = next(w for w in self._all(app.ready_rows) if w.winfo_class() == "TButton")
        app._on_ready_refresh()
        self._settle(app)
        self.assertEqual(str(app.ready_title.cget("text")), title)
        self.assertTrue(button.winfo_exists())

    def test_operation_discards_already_queued_probe_events(self):
        for end_state in (gui.RUNNING, gui.IDLE):
            with self.subTest(end_state=end_state):
                app = self._app(host=("windows", "x86_64"))
                self._settle(app)
                old = app._ready_probe
                app._ready_start()
                app._inventory_worker.join(2)
                self.assertFalse(app._inventory_worker.is_alive())
                app.state = gui.RUNNING
                app._set_controls(False)
                app.state = end_state
                with mock.patch.object(app.driver_lbl, "config") as config, \
                        mock.patch.object(app.driver_lbl, "pack") as pack, \
                        mock.patch.object(app, "_ready_render") as render:
                    for expected in ("driver_inventory", "ready"):
                        kind, payload = app.q.get(timeout=2)
                        self.assertEqual(kind, expected)
                        app._handle(kind, payload)
                config.assert_not_called()
                pack.assert_not_called()
                render.assert_not_called()
                self.assertIs(app._ready_probe, old)
                self.assertFalse(app._ready_running)
                self.assertFalse(app._inventory_running)

    def test_operation_discards_inventory_that_finishes_later(self):
        app = self._app(host=("windows", "x86_64"))
        self._settle(app)
        old = app._ready_probe
        entered, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)
        def inventory():
            entered.set()
            release.wait(5)
            return win("winusb")["inv"]
        with mock.patch.object(gui.windows_usb, "driver_inventory", side_effect=inventory), \
                mock.patch.object(readiness, "probe") as probe:
            app._ready_start()
            try:
                self.assertTrue(entered.wait(2))
                app.state = gui.RUNNING
                app._set_controls(False)
                app.state = gui.IDLE
            finally:
                release.set()
                app._inventory_worker.join(2)
            self.assertFalse(app._inventory_worker.is_alive())
            with mock.patch.object(app.driver_lbl, "config") as config, \
                    mock.patch.object(app.driver_lbl, "pack") as pack:
                self._settle(app)
            config.assert_not_called()
            pack.assert_not_called()
            probe.assert_not_called()
        self.assertIs(app._ready_probe, old)
        self.assertFalse(app._ready_running)
        self.assertFalse(app._inventory_running)

    def test_remove_screen_has_no_checklist_and_restore_does(self):
        app = self._app(probe=linux(1))
        self._settle(app)
        app._set_mode("remove")
        self.assertEqual(app.ready_frame.winfo_manager(), "")
        app._set_mode("restore")
        self.assertEqual(app.ready_frame.winfo_manager(), "pack")
        self.assertIn("firmware not downloaded yet", " ".join(self._texts(app.ready_rows)))

    # ---- window height budget -------------------------------------------------
    BUDGET = 860            # request height; the window itself caps at gui_theme.MAX_WINDOW_H (700) and scrolls.
                            # 700 before the firmware picker (F1 slice 8) added the dropdown, hint and state area

    def _measure(self, app):
        app.root.update_idletasks()
        return app.body.winfo_reqheight()

    def _states(self, app):
        """(name, setup) pairs covering the tallest realistic screens."""
        fw = tempfile.NamedTemporaryFile(suffix=".zip")
        self.addCleanup(fw.close)
        win_driver = win("driver")

        def state(name, probe, mode="install", firmware=True, dev=False, key=True, err=False, stock=False):
            def go():
                gui.ota_key_available = lambda: key
                with mock.patch.object(gui.state, "stock_image_exists", return_value=stock):
                    app.mode.set(mode)
                    app._on_mode()
                    if mode == "install":
                        app.fw_var.set(fw.name if firmware else "")
                        app.variant.set("dev" if dev else "public")
                        app._variant_warn()
                        if err:
                            app._on_action()
                    app._ready_gen += 1
                    app._ready_probe = probe
                    app._ready_render()
            return name, go
        return [state("ready", linux(1)),
                state("no device + help", linux(0), firmware=False),
                state("dev warning", linux(1), dev=True),
                state("no device + help + dev warning", linux(0), firmware=False, dev=True),
                state("access denied", linux(1, access=("denied", "n"))),
                state("tools missing", linux(1, missing=["usbboot", "mksquashfs"])),
                state("no release key", linux(1), key=False),
                state("inline firmware error", linux(1), firmware=False, err=True),
                state("two devices", linux(2)),
                state("restore saved image", linux(0), mode="restore", stock=True),
                state("restore no image", linux(0), mode="restore"),
                state("remove", linux(1), mode="remove")], win_driver

    def test_every_state_fits_the_height_budget(self):
        app = self._app()
        states, _ = self._states(app)
        for name, go in states:
            go()
            h = self._measure(app)
            self.assertLessEqual(h, self.BUDGET, name)
            self.assertLessEqual(app.body.winfo_reqwidth(), 620, name + " widens the window")

    def test_windows_states_fit_the_height_budget(self):
        app = self._app(host=("windows", "x86_64"), probe=win("driver"))
        app.root.update_idletasks()
        fw = tempfile.NamedTemporaryFile(suffix=".zip")
        self.addCleanup(fw.close)
        app.fw_var.set(fw.name)
        for probe in (win("driver"), win("absent", 0), win("winusb")):
            app._ready_gen += 1
            app._ready_probe = probe
            app._ready_render()
            h = self._measure(app)
            self.assertLessEqual(h, self.BUDGET, probe["inv"]["state"])

    def test_window_never_exceeds_the_screen_and_scrolls_when_content_is_taller(self):
        app = self._app(scale=1.25)
        app.root.winfo_screenheight = lambda: 768
        limit = app.max_window_height()
        self.assertEqual(limit, 768 - gui_theme.SCREEN_MARGIN)
        states, _ = self._states(app)
        states[3][1]()                                   # access denied: the tallest checklist
        app.root.update()
        req = app.body.winfo_reqheight()
        self.assertLessEqual(int(app._canvas.cget("height")), limit)
        self.assertEqual(app._scrolling, req > limit)
        self.assertEqual(bool(app._vbar.winfo_manager()), req > limit)
        # force a much smaller screen: the body must scroll rather than clip
        app.root.winfo_screenheight = lambda: 520
        app._fit_body()
        self.assertTrue(app._scrolling)
        self.assertEqual(app._vbar.winfo_manager(), "pack")
        self.assertEqual(int(app._canvas.cget("height")), 520 - gui_theme.SCREEN_MARGIN)
        self.assertEqual(app._canvas.cget("scrollregion"), "0 0 %d %d" % (app.body.winfo_reqwidth(), req))

    def test_idle_hides_progress_block_and_it_returns_when_busy(self):
        app = self._app()
        self.assertEqual(app.prog_head.winfo_manager(), "")
        app.phase_lbl.config(text="Cancelled - device unchanged.")
        app._sync_progress()
        self.assertEqual(app.prog_head.winfo_manager(), "pack")
        app.phase_lbl.config(text="Ready.")
        app._sync_progress()
        self.assertEqual(app.prog_head.winfo_manager(), "")
        app.state = gui.RUNNING
        app._sync_progress()
        self.assertEqual(app.prog_head.winfo_manager(), "pack")
        app.state = gui.IDLE



if __name__ == "__main__":
    unittest.main()
