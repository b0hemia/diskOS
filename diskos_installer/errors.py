# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Stable, user-quotable error codes for the diskOS installer.

Every user-facing failure carries a short code a beta tester can quote from a
screenshot so we can triage it instantly. Namespaces:

  E1xx  environment / preflight  (nothing written to the device yet)
  E2xx  firmware extract & image build
  E3xx  flash orchestration (host side)
  F1xx  device writer result (mirrors the on-device dbg[16] codes)

An exception's str() renders as:  "[E301] <what happened> - <what to do>"
so existing `rep.error(str(e))` call sites show the code with no other change.
"""


class DiskOSError(Exception):
    """Base installer error carrying a stable `code` and an optional `action`
    (a short, plain-language "what to do next")."""
    code = "E000"

    def __init__(self, message, code=None, action=None):
        self.message = message
        if code:
            self.code = code
        self.action = action
        super().__init__(self.render())

    def render(self):
        s = f"[{self.code}] {self.message}"
        if self.action:
            s += f" - {self.action}"
        return s


class PreflightError(DiskOSError):
    """E1xx - environment/preflight; the device has NOT been touched."""
    code = "E100"


class BuildError(DiskOSError):
    """E2xx - firmware extraction / diskOS image build."""
    code = "E200"


class FlashError(DiskOSError):
    """E3xx - flash orchestration (host side). F0xx = device-writer verdicts."""
    code = "E300"


# Device-writer result codes (from my_write6.c dbg[16]) -> stable F-codes for the user. 0xDEAD0007.. are base-gate
# refusals: the writer stopped before unlocking or erasing anything (see basegate.GATE_CODES).
DEVICE_RESULT_CODES = {
    0x600DF10C: ("F001", "SUCCESS"),
    0xDEAD0001: ("F101", "ABORT init/ECC"),
    0xDEAD0002: ("F102", "ABORT out-of-space"),
    0xDEAD0003: ("F103", "ABORT persistent block-write fail"),
    0xDEAD0004: ("F104", "ABORT too many bad blocks"),
    0xDEAD0005: ("F105", "ABORT ECC re-enable failed"),
    0xDEAD0006: ("F106", "ABORT bad-block marker unreadable"),
    0xDEAD0007: ("F201", "REFUSED flash plan missing/damaged (nothing written)"),
    0xDEAD0008: ("F202", "REFUSED Disc not on the image's firmware base (nothing written)"),
    0xDEAD0009: ("F203", "REFUSED image in memory is not the planned one (nothing written)"),
    0xDEAD000A: ("F204", "REFUSED NAND not at power-on state (nothing written)"),
    0xDEAD000B: ("F205", "REFUSED uncorrectable page in a base partition (nothing written)"),
    0xDEAD000C: ("F206", "REFUSED base bad-block marker unreadable (nothing written)"),
    0xDEAD000D: ("F207", "REFUSED base partition has no good blocks (nothing written)"),
    0xDEAD000E: ("F208", "REFUSED base page unreadable (nothing written)"),
    0xDEAD0010: ("F209", "REFUSED no kernel image in the kernel partition (nothing written)"),
    0xDEAD0011: ("F210", "REFUSED no squashfs in the recovery partition (nothing written)"),
    0xDEAD0012: ("F211", "REFUSED base component longer than its partition (nothing written)"),
}

# For a result that is UNKNOWN after the writer may have started: it may still be running on the Disc by itself.
WAIT_THEN_RECOVER = ("The Disc may still be writing by itself: leave it connected and powered for at least 25 "
                     "minutes, then power-cycle it. If it does not boot, it is normally recoverable via mask-ROM "
                     "(hold Volume-Down while plugging in USB) - re-flash your saved stock image or diskOS.")

RECOVERABLE = ("The device is normally recoverable via mask-ROM (not guaranteed for every unit "
               "or failure): power the device OFF, hold Volume-Down, plug in USB to return to "
               "mask-ROM, and re-flash your saved stock image or diskOS.")
