# SPDX-License-Identifier: MIT
# Copyright (c) 2026 diskOS contributors
"""The build-time OTA option: default OFF, --no-ota is absolute, --ota-key bakes the given public key."""
import base64
import os
import tempfile
import unittest
from unittest import mock

from diskos_installer import __main__ as cli
from diskos_installer import bundle, imagebuild

_SPKI = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")


def _pem(tag, body):
    b64 = base64.b64encode(body).decode()
    return f"-----BEGIN {tag}-----\n{b64}\n-----END {tag}-----\n".encode()


def _pub(fill):
    return _pem("PUBLIC KEY", _SPKI + bytes([fill]) * 65)


class OtaOptionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="diskos-ota-opt-")
        self.dir = self.tmp.name
        self.env_key = self._write("env.pem", _pub(1))
        self.payload_key = self._write("payload.pem", _pub(2))
        self.own_key = self._write("own.pem", _pub(3))
        ota_env = {"DISKOS_OTA_ROOTPUB", "DISKOS_MIN_KEY_EPOCH",
                   "DISKOS_MIN_UI_EPOCH"}
        env = {k: v for k, v in os.environ.items() if k not in ota_env}
        p = mock.patch.dict(os.environ, env, clear=True)
        p.start()
        self.addCleanup(p.stop)
        self.addCleanup(self.tmp.cleanup)

    def _write(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def _payload(self, path):
        real = bundle.data
        return mock.patch.object(bundle, "data",
                                 lambda n, required=True: path if n == "diskos-root.pub.pem" else real(n, required))

    def test_default_without_a_key_is_off(self):
        with self._payload(None):
            self.assertIsNone(imagebuild.resolve_ota_config())
            self.assertTrue(imagebuild.describe_ota(None).startswith("OFF"))

    def test_payload_key_bakes_release_floors(self):
        with self._payload(self.payload_key):
            cfg = imagebuild.resolve_ota_config()
        self.assertEqual(cfg["epochs"],
                         b"MIN_KEY_EPOCH=1\nMIN_UI_EPOCH=8\n")

    def test_floor_overrides_keep_their_precedence(self):
        os.environ["DISKOS_MIN_UI_EPOCH"] = "9"
        with self._payload(self.payload_key):
            self.assertEqual(imagebuild.resolve_ota_config()["epochs"],
                             b"MIN_KEY_EPOCH=1\nMIN_UI_EPOCH=9\n")
            for floor in (1, 5, 6, 7):
                with self.subTest(floor=floor):
                    cfg = imagebuild.resolve_ota_config(min_ui_epoch=floor)
                    self.assertEqual(
                        cfg["epochs"],
                        f"MIN_KEY_EPOCH=1\nMIN_UI_EPOCH={floor}\n".encode("ascii"))

    def test_no_ota_overrides_env_and_payload_key(self):
        with self._payload(self.payload_key):
            self.assertIsNotNone(imagebuild.resolve_ota_config())          # the key really is picked up otherwise
            os.environ["DISKOS_OTA_ROOTPUB"] = self.env_key
            self.assertIsNotNone(imagebuild.resolve_ota_config())
            self.assertIsNone(imagebuild.resolve_ota_config(no_ota=True))

    def test_no_ota_with_an_explicit_key_is_a_contradiction(self):
        with self._payload(None), self.assertRaises(imagebuild.BuildError):
            imagebuild.resolve_ota_config(self.own_key, no_ota=True)

    def test_own_key_is_the_one_baked(self):
        os.environ["DISKOS_OTA_ROOTPUB"] = self.env_key
        with self._payload(self.payload_key):
            cfg = imagebuild.resolve_ota_config(self.own_key)
        self.assertEqual(cfg["rootpub"], _pub(3))
        self.assertTrue(imagebuild.describe_ota(cfg).startswith("ON"))

    def test_private_key_is_refused(self):
        priv = self._write("priv.pem", _pem("PRIVATE KEY", b"\x30\x81\x87" + b"\x01" * 132))
        ec = self._write("ec.pem", _pem("EC PRIVATE KEY", b"\x30\x77" + b"\x02" * 119))
        for path in (priv, ec):
            with self.assertRaises(imagebuild.BuildError):
                imagebuild.resolve_ota_config(path)
        both = self._write("both.pem", _pub(4) + _pem("PRIVATE KEY", b"\x01" * 100))
        with self.assertRaises(imagebuild.BuildError):
            imagebuild.resolve_ota_config(both)

    def test_cli_flags_reach_the_install_params_and_exclude_each_other(self):
        p = cli.build_parser()
        a = p.parse_args(["install", "--stock", "x", "--no-ota"])
        self.assertTrue(a.no_ota)
        self.assertIsNone(a.ota_key)
        a = p.parse_args(["install", "--stock", "x", "--ota-key", "k.pem"])
        self.assertEqual(a.ota_key, "k.pem")
        self.assertFalse(a.no_ota)
        with self.assertRaises(SystemExit), mock.patch("sys.stderr"):
            p.parse_args(["install", "--stock", "x", "--no-ota", "--ota-key", "k.pem"])
        with mock.patch.object(cli.service, "do_install", return_value={"ok": True}) as di, \
                mock.patch.object(cli.flasher, "flash_eligible", return_value=True):
            cli.cmd_install(p.parse_args(["install", "--stock", "x", "--no-ota", "-y"]))
        self.assertTrue(di.call_args[0][0]["no_ota"])


if __name__ == "__main__":
    unittest.main()
