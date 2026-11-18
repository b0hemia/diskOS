# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Slice 2a leftovers: other-actions list, tick checkbox, stale Remove text, restore firmware, wordmark packaging."""
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from diskos_installer import gui, gui_theme, platform_probe  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), "..")


class Packaging(unittest.TestCase):
    def test_asset_dirs_include_pyinstaller_root(self):
        with mock.patch.object(sys, "_MEIPASS", "/frozen", create=True):
            dirs = gui_theme.asset_dirs()
        self.assertEqual(dirs[0], os.path.join("/frozen", "diskos_installer", "assets"))
        self.assertTrue(dirs[-1].endswith(os.path.join("diskos_installer", "assets")))
        self.assertTrue(os.path.isfile(os.path.join(dirs[-1], "wordmark.png")))

    def test_spec_and_stage_script_ship_the_assets(self):
        spec = open(os.path.join(ROOT, "build", "diskos-installer.spec")).read()
        stage = open(os.path.join(ROOT, "build", "stage-release.sh")).read()
        for n in ("wordmark.png", "wordmark@2x.png", "display.png", "display@1.5x.png", "display@2x.png",
                  "display.json", "disc.png", "disc@1.5x.png", "disc@2x.png",
                  "LICENSE-archivo-OFL.txt", "LICENSE-michroma-OFL.txt"):
            self.assertIn(n, spec)
            self.assertIn("diskos_installer/assets/" + n, stage)
            self.assertTrue(os.path.isfile(os.path.join(ROOT, "diskos_installer", "assets", n)))
        self.assertIn('"diskos_installer/assets"', spec)


class GuiLeftovers(unittest.TestCase):
    def _app(self):
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as e:
            self.skipTest("no Tk display: %s" % e)
        root.withdraw()
        self.addCleanup(root.destroy)
        patches = [mock.patch.object(gui, "ota_key_available", return_value=True),
                   mock.patch.object(platform_probe, "maskrom_count", return_value=0),
                   mock.patch.object(gui.state, "stock_image_exists", return_value=False),
                   mock.patch.object(gui.firmware_resolve, "identify_zip",
                                     side_effect=lambda path, expected=None: gui.firmware_resolve.ZipIdentity(expected, path)),
                   mock.patch.object(gui.firmware_resolve, "check_space", return_value=None),
                   mock.patch.object(gui.firmware_resolve, "plan_restore",
                                     return_value=gui.firmware_resolve.RestorePlan("backup", "240"))]
        rd = getattr(gui, "readiness", None)
        if rd is not None:                       # the checklist (slice 2b) must not touch real USB here
            patches.append(mock.patch.object(rd, "probe", return_value={
                "os": "linux", "count": 0, "access": None, "inv": None, "inv_error": None, "missing_tools": []}))
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        app = gui.App(root)
        app._tick_device = lambda: None
        return app

    def _all(self, w):
        out = []
        for c in w.winfo_children():
            out.append(c)
            out += self._all(c)
        return out

    def test_other_actions_never_offer_the_current_screen(self):
        app = self._app()
        names = {"install": "Back to install", "restore": "Restore stock firmware...",
                 "remove": "Remove installer data..."}
        for mode in names:
            app._set_mode(mode)
            shown = {str(w.cget("text")) for w in self._all(app.mode_frame)
                     if w.winfo_class() == "TButton" and w.winfo_manager()}
            self.assertEqual(shown, set(names.values()) - {names[mode]}, mode)

    def test_checkbox_draws_a_tick_not_an_x(self):
        app = self._app()
        st = app.ttk.Style(app.root)
        for style in ("TCheckbutton", "Panel.TCheckbutton"):
            self.assertIn(style.replace(".", "_") + ".indicator", str(st.layout(style)))
        imgs = app.root._diskos_check_imgs          # Panel/BG sets: off, on, ...; first set is TCheckbutton
        off, on = imgs[0], imgs[1]
        fill, dark = gui_theme._hex(gui_theme.ACCENT), gui_theme._hex(gui_theme.ON_ACCENT)

        def close(px, want):
            return all(abs(a - b) < 40 for a, b in zip(px, want))
        # tick: dark marks low-left (short stroke) and up to the right; an X would also mark the top-left
        n = gui_theme.indicator_size(app.root)          # 16 px at 100%, scaled with the display

        def at(fx, fy):
            return int(fx * n), int(fy * n)
        self.assertTrue(close(on.get(*at(0.45, 0.68)), dark))      # the tick's corner
        self.assertTrue(close(on.get(*at(0.68, 0.42)), dark))      # its long stroke, up to the right
        self.assertTrue(close(on.get(*at(0.22, 0.22)), fill))      # an X would mark the top-left
        self.assertTrue(close(on.get(*at(0.78, 0.78)), fill))      # nor the bottom-right of an X
        self.assertFalse(close(off.get(*at(0.45, 0.68)), fill))

    @staticmethod
    def _until_worker(app):
        """Apply queued events (probe, plan, verification) until a service worker has started."""
        app.worker = None
        while app.worker is None:
            app._handle(*app.q.get(timeout=5))
        app.worker.join(5)

    # ---- restore never reuses the Install page's firmware ----------------------
    def test_restore_with_saved_image_passes_no_firmware(self):
        app = self._app()
        app.fw_var.set("/from/install.zip")
        seen = []
        with mock.patch.object(gui.state, "stock_image_exists", return_value=True), \
                mock.patch.object(gui.service, "do_restore",
                                  side_effect=lambda params, rep, conf: seen.append(dict(params)) or {}):
            app._set_mode("restore")
            self.assertIsNone(app.restore_fw_var)
            app._on_action()
            self._until_worker(app)
        self.assertEqual(seen, [{"firmware": None}])

    def test_restore_without_saved_image_uses_its_own_field_only(self):
        app = self._app()
        app.fw_var.set("/from/install.zip")
        seen = []
        with mock.patch.object(gui.state, "stock_image_exists", return_value=False), \
                mock.patch.object(gui.service, "do_restore",
                                  side_effect=lambda params, rep, conf: seen.append(dict(params)) or {}):
            app._set_mode("restore")
            self.assertIsNotNone(app.restore_fw_var)
            self.assertEqual(app.restore_fw_var.get(), "")
            app._on_action()                      # nothing chosen: no service call, no stale Install path
            self.assertEqual(seen, [])
            app.restore_fw_var.set("/restore/own.zip")
            app._on_action()                      # verified first, then the restore starts with that path
            self._until_worker(app)
        self.assertEqual(seen, [{"firmware": "/restore/own.zip"}])
        self.assertEqual(app.fw_var.get(), "/from/install.zip")

    def test_picked_firmware_goes_to_the_screen_on_show(self):
        app = self._app()
        app.fw_var.set("/install.zip")
        with mock.patch.object(gui.state, "stock_image_exists", return_value=False), \
                mock.patch("tkinter.filedialog.askopenfilename", return_value="/picked.zip"):
            app._set_mode("restore")
            app._pick_firmware()
            self.assertEqual(app.restore_fw_var.get(), "/picked.zip")
            self.assertEqual(app.fw_var.get(), "/install.zip")
            app._set_mode("install")
            app._pick_firmware()
            self.assertEqual(app.fw_var.get(), "/picked.zip")

    def test_stale_remove_text_gone(self):
        self.assertNotIn("Remove diskOS", open(gui.__file__).read())


if __name__ == "__main__":
    unittest.main()
