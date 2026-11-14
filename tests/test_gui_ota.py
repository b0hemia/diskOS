# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 diskOS contributors
"""GUI OTA checkbox: key detection and the checkbox -> no_ota mapping (no Tk needed)."""
import os
import unittest
from unittest import mock

from diskos_installer import gui, imagebuild


class GuiOtaTests(unittest.TestCase):
    def setUp(self):
        ota_env = {"DISKOS_OTA_ROOTPUB", "DISKOS_MIN_KEY_EPOCH",
                   "DISKOS_MIN_UI_EPOCH"}
        env = {k: v for k, v in os.environ.items() if k not in ota_env}
        p = mock.patch.dict(os.environ, env, clear=True)
        p.start()
        self.addCleanup(p.stop)

    def test_enabled_gui_uses_the_shipped_release_floor(self):
        self.assertTrue(gui.ota_key_available())
        cfg = imagebuild.resolve_ota_config(**gui.ota_params(True, True))
        self.assertIsNotNone(cfg)
        self.assertEqual(cfg["epochs"],
                         b"MIN_KEY_EPOCH=1\nMIN_UI_EPOCH=8\n")

    def test_ticked_with_key_means_ota_on(self):
        self.assertEqual(gui.ota_params(True, True), {"no_ota": False})

    def test_unticked_means_no_ota(self):
        self.assertEqual(gui.ota_params(False, True), {"no_ota": True})

    def test_no_key_forces_off_even_if_ticked(self):
        self.assertEqual(gui.ota_params(True, False), {"no_ota": True})

    def test_key_available_follows_resolve_ota_config(self):
        with mock.patch.object(imagebuild, "resolve_ota_config", return_value={"rootpub": b"x"}):
            self.assertTrue(gui.ota_key_available())
        with mock.patch.object(imagebuild, "resolve_ota_config", return_value=None):
            self.assertFalse(gui.ota_key_available())

    def test_broken_key_counts_as_available(self):
        err = imagebuild.BuildError("bad key", code="E240")
        with mock.patch.object(imagebuild, "resolve_ota_config", side_effect=err):
            self.assertTrue(gui.ota_key_available())

    def test_strings(self):
        self.assertIn("Allow diskOS updates over Wi-Fi", gui.OTA_LABEL)
        self.assertEqual(gui.OTA_NO_KEY, "This installer has no release key; updates are off")
        for s in (gui.OTA_LABEL, gui.OTA_HELP, gui.OTA_NO_KEY):
            s.encode("ascii")

    def test_confirm_dialog_lists_ota_line(self):
        src = open(gui.__file__).read()
        self.assertIn('("Updates over Wi-Fi", summary.get("ota"))', src)


if __name__ == "__main__":
    unittest.main()
