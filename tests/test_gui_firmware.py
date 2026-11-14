# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Firmware flow of the GUI (F1 slice 8): states 01-12, run tokens, cancel/retry, verified-only Install.
Fake resolver/supervisor/scanner/browser; no network, no device."""
import os
import queue
import sys
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from diskos_installer import firmware_download as dl, firmware_local, firmware_resolve as fr, gui, platform_probe, readiness  # noqa: E402

CACHE = "/cache/SNOWSKY_DISC_update_20260909_v257.zip"
MB = 10 ** 6


class Handle:
    def __init__(self, path=CACHE, err=None):
        self.gate, self.path, self.err, self.cancelled = threading.Event(), path, err, False

    def wait(self, timeout=None):
        self.gate.wait(5)
        if self.cancelled:
            raise dl.Cancelled("Firmware download cancelled")
        if self.err:
            raise self.err
        return self.path

    def cancel(self):
        self.cancelled = True
        self.gate.set()


class Fake:
    def __init__(self):
        self.cached, self.handle, self.threads, self.cb = None, Handle(), set(), None
        self.ident_err, self.space_err, self.plan_err = None, None, None
        self.scan = firmware_local.ScanResult("none", detail="No verified FiiO V2.57 firmware found in Downloads")
        self.browser = firmware_local.BrowserResult(True, "https://example.invalid/page")
        self.calls = []

    def resolve_firmware(self, ver, lock, cb):
        self.threads.add(threading.get_ident())
        self.cb, self.last_lock = cb, lock
        if self.cached:
            return fr.Resolution("cached", ver, path=self.cached)
        return fr.Resolution("acquiring", ver, handle=self.handle)

    def identify_zip(self, path, expected=None):
        self.calls.append(("identify", path, expected))
        if self.ident_err:
            raise self.ident_err
        return fr.ZipIdentity(expected, path)

    def check_space(self, ver, path=None):
        if self.space_err:
            raise self.space_err

    def check_downloads(self, ver, run_lock=None, cancel=None):
        self.calls.append(("scan", ver))
        return self.scan

    def open_download_page(self, ver):
        self.calls.append(("page", ver))
        return self.browser

    def plan_restore(self, ver=None):
        if isinstance(self.plan_err, BaseException):
            raise self.plan_err
        if self.plan_err:
            raise fr.FirmwareResolveError("Cannot restore: no saved stock backup")
        return fr.RestorePlan("backup", "240")


class FirmwareFlow(unittest.TestCase):
    def _app(self, saved=False):
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as e:
            self.skipTest("no Tk display: %s" % e)
        root.withdraw()
        self.addCleanup(root.destroy)
        for p in (mock.patch.object(gui, "ota_key_available", return_value=True),
                  mock.patch.object(platform_probe, "maskrom_count", return_value=0),
                  mock.patch.object(gui.state, "stock_image_exists", return_value=saved),
                  mock.patch.object(gui.readiness, "probe", return_value={
                      "os": "linux", "count": 1, "access": ("ok", "x"), "inv": None, "inv_error": None,
                      "missing_tools": []})):
            p.start()
            self.addCleanup(p.stop)
        self.fake = Fake()
        self.app = app = gui.App(root, run_lock=object())
        app._tick_device = lambda: None
        app._fr = self.fake
        return app

    def pump(self, until, what="condition"):
        app = self.app
        for _ in range(100):
            if until():
                return
            try:
                app._handle(*app.q.get(timeout=0.1))
            except queue.Empty:
                pass
        self.fail("timed out waiting for " + what)

    def texts(self):
        out = []
        def walk(w):
            for c in w.winfo_children():
                if c.winfo_class() in ("Label", "TLabel", "TButton"):
                    out.append(str(c.cget("text")))
                walk(c)
        walk(self.app.fw_state)
        return out

    def disabled(self):
        a = self.app
        return {n for n, w in (("get", a.fw_get), ("browse", a.fw_browse), ("entry", a.fw_entry),
                               ("install", a.action_btn), ("combo", a.fw_combo)) if str(w.cget("state")) == "disabled"}

    def click(self, label):
        for c in self.app.fw_state.winfo_children():
            for w in [c] + list(c.winfo_children()):
                if w.winfo_class() == "TButton" and str(w.cget("text")) == label:
                    return w.invoke()
        self.fail("no %s button in %s" % (label, self.texts()))

    def row(self):
        return {r["key"]: r for r in self.app._ready_rows()}["firmware"]

    # ---- 01-04
    def test_01_idle_dropdown_and_hint(self):
        app = self._app()
        self.assertEqual(list(app.fw_combo.cget("values")), ["V2.57 (latest)", "V2.40", "V2.28", "V2.09"])
        self.assertEqual(app.fw_ver_var.get(), "V2.57 (latest)")
        self.assertEqual(str(app.fw_combo.cget("state")), "readonly")
        self.assertEqual(self.texts(), ["Get firmware downloads the official FiiO V2.57 firmware (106 MB) and checks it. "
                                        "Nothing is sent about your Disc."])
        self.assertEqual((self.row()["text"], self.row()["fix"]), ("FiiO V2.57 firmware not downloaded yet", "Press Get firmware."))
        self.assertEqual(self.disabled(), set())

    def test_02_03_04_download_progress_verify_verified(self):
        app = self._app()
        app.fw_get.invoke()
        self.assertEqual(self.disabled(), {"get", "browse", "entry", "install", "combo"})
        self.assertEqual(self.texts()[::2][0], "Downloading FiiO V2.57 firmware from FiiO...")
        self.assertIn("Cancel", self.texts())
        self.pump(lambda: self.fake.cb is not None, "download start")
        threading.Thread(target=lambda: self.fake.cb(1, "progress", 44 * MB, 105680009)).start()
        self.pump(lambda: "44 of 106 MB - 42%" in self.texts(), "progress")
        self.fake.cb(1, "verifying", 105680009, 105680009)
        self.pump(lambda: "Verifying firmware..." in self.texts(), "verifying")
        self.assertIn("Checking the download against the installer's recorded checksum", self.texts())
        self.fake.handle.gate.set()
        self.pump(lambda: not app._fw_busy, "verified")
        self.assertEqual(self.texts(), ["FiiO V2.57 firmware verified.", "Downloaded from FiiO and saved on this computer for next time."])
        self.assertEqual(app.fw_var.get(), CACHE)              # the cache copy, scrolled to its end
        self.assertGreater(app.fw_entry.xview()[1], 0.95)
        self.assertEqual(self.disabled(), set())
        self.assertTrue(self.row()["ok"])
        self.assertIs(self.fake.last_lock, app.run_lock)

    def test_04b_cache_hit(self):
        app = self._app()
        self.fake.cached = CACHE
        app.fw_get.invoke()
        self.pump(lambda: not app._fw_busy)
        self.assertIn("Saved on this computer from an earlier run - no download needed.", self.texts())

    # ---- 05-10
    def _fail_download(self, err):
        app = self._app()
        self.fake.handle = Handle(err=err)
        self.fake.handle.gate.set()
        app.fw_get.invoke()
        self.pump(lambda: not app._fw_busy)

    def test_05_found_in_downloads_uses_the_cache_copy(self):
        app = self._app()
        self.fake.scan = firmware_local.ScanResult("found", path=CACHE, source="/home/u/Downloads/update_v257.zip")
        self.fake.handle = Handle(err=dl.Timeout("Connection timed out"))
        self.fake.handle.gate.set()
        app.fw_get.invoke()
        self.pump(lambda: not app._fw_busy)
        self.assertEqual(self.texts(), ["Could not download firmware: connection timed out.",
                                        "Found and verified FiiO V2.57 firmware in Downloads.",
                                        "Copied into the installer's own folder; your original file is untouched."])
        self.assertEqual(app.fw_var.get(), CACHE)

    def test_06_no_match_buttons_and_page_only_on_press(self):
        self._fail_download(dl.Timeout("Connection timed out"))
        self.assertEqual(self.texts(), ["Could not download firmware: connection timed out.",
                                        "No verified V2.57 firmware found in Downloads.",
                                        "Open FiiO download page", "Check Downloads"])
        self.assertNotIn(("page", "257"), self.fake.calls)
        self.assertEqual(self.row()["fix"], "Press Get firmware to retry, or Browse for a ZIP.")
        self.click("Open FiiO download page")
        self.assertIn(("page", "257"), self.fake.calls)
        self.fake.browser = firmware_local.BrowserResult(False, "https://example.invalid/page", "no browser")
        self.click("Open FiiO download page")
        self.assertIn("https://example.invalid/page", [str(w.get()) for w in self.app.fw_state.winfo_children() if w.winfo_class() == "TEntry"])
        self.fake.scan = firmware_local.ScanResult("found", path=CACHE, source="/d/x.zip")
        self.click("Check Downloads")
        self.pump(lambda: self.app.fw_var.get() == CACHE, "scan")

    def test_07_manual_zip_mismatch_keeps_the_path(self):
        app = self._app()
        self.fake.ident_err = fr.WrongVersion("That file is FiiO V2.40, but FiiO V2.57 was chosen")
        with mock.patch("tkinter.filedialog.askopenfilename", return_value="/home/u/other.zip"):
            app._pick_firmware()
        self.pump(lambda: not app._fw_busy)
        self.assertEqual(app.fw_var.get(), "/home/u/other.zip")
        self.assertEqual(self.texts()[0], "This file does not match the verified FiiO V2.57 download. It cannot be used.")
        self.assertEqual(self.fake.calls[0], ("identify", "/home/u/other.zip", "257"))
        self.assertEqual(self.row()["text"], "The chosen file is not verified FiiO firmware")
        with mock.patch.object(gui.service, "do_install") as inst:
            app._on_action()
            self.pump(lambda: not app._fw_busy)
        inst.assert_not_called()

    def test_08_tls(self):
        self._fail_download(dl.TlsError("Could not verify FiiO's secure connection (x)"))
        self.assertEqual(self.texts()[0], "Could not verify FiiO's secure connection. Check this computer's date or use the browser download.")
        self.assertEqual(self.texts()[1:], ["Open FiiO download page", "Check Downloads"])
        self.assertNotIn("scan", [c[0] for c in self.fake.calls])

    def test_09_no_space_retry_only(self):
        self._fail_download(dl.DiskFull("Cannot write"))
        self.assertTrue(self.texts()[0].startswith("Not enough space to save the firmware in "))
        self.assertIn("Free some space, then retry.", self.texts()[0])
        self.assertNotIn(" MB ", self.texts()[0])          # DiskFull measured nothing: no invented shortfall
        self.assertEqual(self.texts()[1], "Retry")
        self.assertEqual(len(self.texts()), 2)
        self.fake.handle = Handle()
        self.fake.handle.gate.set()
        self.click("Retry")
        self.pump(lambda: self.app._fw_verified, "retry")

    def test_10_cancel_then_retry(self):
        app = self._app()
        app.fw_get.invoke()
        self.pump(lambda: self.fake.cb is not None)
        self.pump(lambda: app._fw_ctx["handle"] is not None, "handle")
        self.click("Cancel")
        self.assertTrue(self.fake.handle.cancelled)
        self.assertEqual(self.texts(), ["Firmware download cancelled.", "Retry"])
        self.assertEqual(self.disabled(), set())
        self.assertIsNone(app._fw_verified)
        self.fake.handle = Handle()
        self.fake.handle.gate.set()
        self.click("Retry")
        self.pump(lambda: app._fw_verified, "retry")

    # ---- stale events, Install gate, thread seam
    def test_dropdown_or_path_change_ignores_the_old_run(self):
        for change in (lambda a: (a.fw_ver_var.set("V2.40"), a._fw_dropdown()), lambda a: a.fw_var.set("/typed.zip")):
            app = self._app()
            app.fw_get.invoke()
            self.pump(lambda: self.fake.cb is not None)
            old = self.fake.handle
            self.pump(lambda: app._fw_ctx["handle"] is not None)
            change(app)
            self.assertTrue(old.cancelled)
            self.assertEqual(self.disabled(), set())
            old.cancelled = False
            app._fw_post(app._fw_tok - 1, "verified", path=CACHE, how="download", ver="257")   # late event, old token
            self.fake.cb(1, "progress", MB, 105680009)
            while not app.q.empty():
                app._handle(*app.q.get())
            self.assertIsNone(app._fw_verified)
            self.assertNotIn(app.fw_var.get(), (CACHE,))
            self.assertNotIn("Cancel", self.texts())

    def test_dropdown_clears_a_filled_path_and_retargets_the_text(self):
        app = self._app()
        self.fake.cached = CACHE
        app.fw_get.invoke()
        self.pump(lambda: not app._fw_busy)
        app.fw_ver_var.set("V2.40")
        app._fw_dropdown()
        self.assertEqual(app.fw_var.get(), "")
        self.assertIsNone(app._fw_verified)
        self.assertIn("official FiiO V2.40 firmware (128 MB)", self.texts()[0])

    def test_install_needs_verification_and_space(self):
        app = self._app()
        seen = []
        with mock.patch.object(gui.service, "do_install", side_effect=lambda p, r, c: seen.append(dict(p)) or {}):
            app.fw_var.set("/typed.zip")
            app._on_action()                      # typed path: verified first, then Install continues
            self.pump(lambda: seen, "install")
            self.assertEqual(seen[0]["firmware"], "/typed.zip")
            self.assertEqual(self.fake.calls[0], ("identify", "/typed.zip", "257"))
        app = self._app()
        self.fake.space_err = fr.InsufficientSpace(2 * 10 ** 9, 10 ** 9, "/home/u/.local/share/diskos-installer")
        with mock.patch.object(gui.service, "do_install") as inst:
            app.fw_var.set("/typed.zip")
            app._on_action()
            self.pump(lambda: self.texts() and self.texts()[0].startswith("Not enough space"), "space")
        inst.assert_not_called()
        self.assertIn("Free 1000 MB in /home/u/.local/share/diskos-installer", self.texts()[0])

    def test_install_rechecks_a_previously_verified_path(self):
        app = self._app()
        self.fake.cached = CACHE
        app.fw_get.invoke()
        self.pump(lambda: not app._fw_busy)
        self.fake.ident_err = fr.UnknownFirmware("The file was replaced")
        with mock.patch.object(app, "_launch") as launch:
            app._on_action()
            self.pump(lambda: not app._fw_busy)
        launch.assert_not_called()
        self.assertIn(("identify", CACHE, "257"), self.fake.calls)
        self.assertIsNone(app._fw_verified)

    def test_space_view_uses_the_error_path_and_only_measured_shortfalls(self):
        app = self._app()
        with mock.patch.object(gui.state, "state_dir", side_effect=OSError("state dir gone")) as sd:
            app._fw_space("257", fr.InsufficientSpace(150 * MB, 30 * MB, "/data/dio"), lambda: None)
            self.assertEqual(self.texts()[0], "Not enough space to save the firmware. Free 120 MB in /data/dio, then retry.")
            with mock.patch.object(gui.state, "_state_base", return_value="/base/dio"):
                app._fw_space("257", dl.DiskFull("Cannot write"), lambda: None)
            self.assertEqual(self.texts()[0], "Not enough space to save the firmware in /base/dio. Free some space, then retry.")
        sd.assert_not_called()
        self.assertEqual(self.texts()[1:], ["Retry"])

    def test_workers_never_touch_tk(self):
        app = self._app()
        app.fw_get.invoke()
        self.pump(lambda: self.fake.cb is not None)
        self.assertNotIn(threading.main_thread().ident, self.fake.threads)
        errs = []
        t = threading.Thread(target=lambda: errs.append(self._call_event(app)))
        t.start()
        t.join()
        self.assertIsInstance(errs[0], RuntimeError)
        self.fake.cb(1, "progress", MB, 105680009)               # a supervisor thread only queues
        self.assertEqual(app.q.get(timeout=2)[0] in ("fw", "ready"), True)

    @staticmethod
    def _call_event(app):
        try:
            app._fw_event({"tok": app._fw_tok, "ev": "progress"})
        except RuntimeError as e:
            return e

    # ---- restore 11 / 12
    def test_11_restore_saved_image_is_one_click_and_downloads_nothing(self):
        app = self._app(saved=True)
        app._set_mode("restore")
        self.assertIsNone(app.fw_get)
        self.assertEqual(self.texts(), ["Nothing is downloaded."])
        seen = []
        with mock.patch.object(gui.service, "do_restore", side_effect=lambda p, r, c: seen.append(dict(p)) or {}):
            app._on_action()
            self.pump(lambda: seen, "restore")
        self.assertEqual(seen, [{"firmware": None}])
        self.assertEqual(self.fake.calls, [])

    def test_11_unusable_saved_image_falls_back_to_the_dropdown_flow(self):
        app = self._app(saved=True)
        app._set_mode("restore")
        self.fake.plan_err = True
        app._on_action()
        self.pump(lambda: app.fw_get is not None, "rebuild layout")
        self.assertIsNotNone(app.restore_fw_var)

    def test_11_restore_plan_os_error_falls_back_and_does_not_stick(self):
        import errno
        app = self._app(saved=True)
        app._set_mode("restore")
        self.fake.plan_err = OSError(errno.ENOSPC, "No space left on device")
        with mock.patch.object(app, "_launch") as launch:
            app._on_action()
            self.pump(lambda: app.fw_get is not None, "rebuild layout")
        launch.assert_not_called()
        self.assertFalse(app._fw_busy)
        self.assertIsNotNone(app.restore_fw_var)

    def test_12_restore_rebuild_uses_the_same_flow(self):
        app = self._app(saved=False)
        app._set_mode("restore")
        self.assertEqual(app.fw_ver_var.get(), "V2.57 (latest)")
        self.assertIn("Get firmware downloads the official FiiO V2.57", self.texts()[0])
        self.fake.cached = CACHE
        app.fw_get.invoke()
        self.pump(lambda: not app._fw_busy)
        self.assertEqual(app.restore_fw_var.get(), CACHE)
        seen = []
        with mock.patch.object(gui.service, "do_restore", side_effect=lambda p, r, c: seen.append(dict(p)) or {}):
            app._on_action()
            self.pump(lambda: seen)
        self.assertEqual(seen, [{"firmware": CACHE}])


class MainLockHandoverTests(unittest.TestCase):
    def test_main_hands_the_held_run_lock_to_gui_main(self):
        from diskos_installer import __main__ as cli
        held = object()

        class FakeLock:
            def __enter__(self):
                return held

            def __exit__(self, *exc):
                return False

        with mock.patch.object(cli, "_Lock", FakeLock), mock.patch.object(gui, "main", return_value=0) as gm:
            self.assertEqual(cli._main([]), 0)
        gm.assert_called_once_with(held)


if __name__ == "__main__":
    unittest.main()
