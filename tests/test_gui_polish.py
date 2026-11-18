# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Display assets and drawn controls of the installer window: titles/kickers come from the pre-rendered sheet
with a text fallback, the hero disc hides with the hero, Dev lives under Advanced without changing behaviour,
and every asset loads at both scales. Display tests skip without Tk/display."""
import json
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from diskos_installer import gui, gui_theme, platform_probe  # noqa: E402

ASSETS = os.path.join(os.path.dirname(__file__), "..", "diskos_installer", "assets")


class AssetFiles(unittest.TestCase):
    def test_sheet_index_covers_every_screen_title_and_fits_its_sheet(self):
        idx = json.load(open(os.path.join(ASSETS, "display.json")))
        self.assertEqual(set(idx), {"1", "1.5", "2"})
        titles = {text[0] for text in gui.MODE_TEXT.values()}
        titles.update(text[0] for text in gui.SUCCESS_TEXT.values())
        titles.update(gui.CONFIRM_TITLE.values())
        titles.update(("Install diskOS (public)", "Install diskOS (dev)", "Set up USB driver",
                       "Finished with problems", "Something went wrong", "The flash result is unknown",
                       "USB driver setup stopped", "The flash did not complete", "The install stopped",
                       "The restore stopped", "The removal stopped"))
        for scale in idx:
            for title in titles:
                self.assertIn("title:" + title, idx[scale], (scale, title))
        for key in ("kicker:Your firmware", "kicker:Variant", "kicker:Other actions"):
            self.assertIn(key, idx["1"])
        self.assertEqual(set(idx["1"]), set(idx["2"]))
        self.assertEqual(set(idx["1"]), set(idx["1.5"]))
        for scale, name in (("1", "display.png"), ("1.5", "display@1.5x.png"), ("2", "display@2x.png")):
            with open(os.path.join(ASSETS, name), "rb") as f:
                head = f.read(24)
            self.assertEqual(head[:8], b"\x89PNG\r\n\x1a\n")
            w, h = int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big")
            for key, (x, y, bw, bh) in idx[scale].items():
                self.assertLessEqual(x + bw, w, key)
                self.assertLessEqual(y + bh, h, key)

    def test_display_strings_are_ascii(self):
        idx = json.load(open(os.path.join(ASSETS, "display.json")))
        for key in idx["1"]:
            key.encode("ascii")

    def test_assets_stay_small(self):
        total = sum(os.path.getsize(os.path.join(ASSETS, n)) for n in os.listdir(ASSETS) if n.endswith(".png"))
        self.assertLess(total, 440 * 1024)         # 400 KB before the result-screen strings joined the sheet


class DisplayWidgets(unittest.TestCase):
    def _root(self):
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as e:
            self.skipTest("no Tk display: %s" % e)
        root.withdraw()
        self.addCleanup(root.destroy)
        return root

    def _app(self):
        root = self._root()
        patches = [mock.patch.object(gui, "ota_key_available", return_value=True),
                   mock.patch.object(platform_probe, "maskrom_count", return_value=0),
                   mock.patch.object(gui.state, "stock_image_exists", return_value=False),
                   mock.patch.object(gui.readiness, "probe", return_value={
                       "os": "linux", "count": 0, "access": None, "inv": None, "inv_error": None,
                       "missing_tools": []})]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        app = gui.App(root)
        app._tick_device = lambda: None
        return app

    def test_titles_are_images_and_keep_their_text(self):
        app = self._app()
        for mode, (title, _i, _l) in gui.MODE_TEXT.items():
            app._set_mode(mode)
            self.assertEqual(str(app.title_lbl.cget("text")), title)
            self.assertTrue(str(app.title_lbl.cget("image")), mode)
            self.assertEqual(str(app.title_lbl.cget("compound")), "image")

    def test_success_titles_are_images_and_keep_their_text(self):
        app = self._app()
        for mode, (title, _body, _next) in gui.SUCCESS_TEXT.items():
            with self.subTest(mode=mode):
                app.mode.set(mode)
                app._finish({"ok": True})
                self.assertEqual(str(app.phase_lbl.cget("text")), title)
                self.assertTrue(str(app.phase_lbl.cget("image")), mode)
                self.assertEqual(str(app.phase_lbl.cget("compound")), "image")

    def test_missing_sheet_falls_back_to_text(self):
        root = self._root()
        from tkinter import ttk
        with mock.patch.object(gui_theme, "_find_asset", return_value=None):
            lbl = ttk.Label(root)
            self.assertFalse(gui_theme.set_display(lbl, "Install diskOS"))
            self.assertEqual(str(lbl.cget("text")), "Install diskOS")
            self.assertEqual(str(lbl.cget("image")), "")
            k = gui_theme.kicker(root, "Variant")
            self.assertEqual(str(k.cget("text")), "VARIANT")
            self.assertIsNotNone(gui_theme.hero_bleed(root, ttk.Frame(root), ttk.Frame(root)))   # drawn disc

    def test_unknown_display_string_falls_back_to_text(self):
        root = self._root()
        from tkinter import ttk
        lbl = ttk.Label(root)
        self.assertFalse(gui_theme.set_display(lbl, "Not in the sheet"))
        self.assertEqual(str(lbl.cget("text")), "Not in the sheet")

    def test_each_display_scale_gets_the_nearest_images(self):
        root = self._root()
        base = float(root.tk.call("tk", "scaling"))
        self.addCleanup(root.tk.call, "tk", "scaling", base)
        got = {}
        for pct, want in ((100, "1"), (125, "1.5"), (150, "1.5"), (200, "2"), (250, "2")):
            root.tk.call("tk", "scaling", 96 / 72 * pct / 100)
            self.assertEqual(gui_theme.asset_scales(root)[0], want, pct)
            got[want] = gui_theme.display(root, "title", "Install diskOS").height()
        self.assertAlmostEqual(got["1.5"] / got["1"], 1.5, delta=0.2)
        self.assertAlmostEqual(got["2"] / got["1"], 2, delta=0.25)
        root.tk.call("tk", "scaling", 96 / 72 * 1.25)
        self.assertFalse(gui_theme.dense(root))
        self.assertIsNotNone(gui_theme.wordmark(root))     # no 1.5x wordmark: falls back to the nearest file

    def test_hero_disc_hides_with_the_hero(self):
        app = self._app()
        app.root.deiconify()
        app.root.update()
        disc = [w for w in app.body.winfo_children() if w.winfo_manager() == "place"]
        self.assertEqual(len(disc), 1)
        hero = app.title_lbl.master.master
        self.assertTrue(disc[0].winfo_ismapped())
        hero.pack_forget()
        app.root.update()
        self.assertFalse(disc[0].winfo_ismapped())

    def test_dev_is_under_advanced_and_opens_when_chosen(self):
        app = self._app()
        self.assertEqual(app._adv.winfo_manager(), "")              # hidden by default
        app._toggle_advanced()
        self.assertEqual(app._adv.winfo_manager(), "pack")
        app._toggle_advanced()
        self.assertEqual(app._adv.winfo_manager(), "")
        app.variant.set("dev")                                       # chosen any way: it shows, with the warning
        app._variant_warn()
        self.assertEqual(app._adv.winfo_manager(), "pack")
        self.assertIn("always-on passwordless USB root", str(app.variant_warn_lbl.cget("text")))
        app._toggle_advanced(False)                                  # never hides the selected choice
        self.assertEqual(app._adv.winfo_manager(), "pack")
        app.variant.set("public")
        app._variant_warn()
        app._toggle_advanced(False)
        self.assertEqual(app._adv.winfo_manager(), "")

    def test_radio_arrows_only_target_visible_choices(self):
        app = self._app()
        app.root.deiconify()
        app.root.update()
        public = next(w for w in app._adv.master.winfo_children()
                      if w.winfo_class() == "TRadiobutton" and w is not app._adv)
        public.focus_force()
        app.root.update()
        app.root.tk.eval("""
            set ::diskos_radio_targets {}
            rename ::tk::TabToWindow ::tk::OriginalTabToWindow
            proc ::tk::TabToWindow {w} {
                lappend ::diskos_radio_targets $w
                ::tk::OriginalTabToWindow $w
            }
        """)
        for key in ("<Down>", "<Up>"):
            public.event_generate(key)
            app.root.update()
            self.assertEqual(app.root.tk.splitlist(app.root.tk.call("set", "::diskos_radio_targets")), ())
            self.assertIs(app.root.focus_get(), public)
        app._toggle_advanced(True)
        app.root.update()
        public.event_generate("<Down>")
        app.root.update()
        self.assertIs(app.root.focus_get(), app._adv)
        app._adv.event_generate("<Up>")
        app.root.update()
        self.assertIs(app.root.focus_get(), public)
        self.assertEqual(app.variant.get(), "public")

    def test_primary_is_compact_and_right_aligned(self):
        app = self._app()
        app.root.deiconify()
        app.root.update()
        b = app.action_btn
        self.assertEqual(b.pack_info()["side"], "right")
        self.assertLess(b.winfo_width(), app.body.winfo_width() // 2)

    def test_buttons_are_drawn_pills_and_links_have_no_box(self):
        app = self._app()
        st = app.ttk.Style(app.root)
        for name in ("TButton", "Accent.TButton", "Small.TButton"):
            self.assertIn(name.replace(".", "_") + ".skin", str(st.layout(name)))
        self.assertNotIn("border", str(st.layout("Link.TButton")))
        self.assertEqual(str(app.restore_btn.cget("style")), "Link.TButton")


if __name__ == "__main__":
    unittest.main()
