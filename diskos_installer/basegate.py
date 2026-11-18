# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 diskOS contributors
"""Installed-base gate: what the Disc must already be running before a rootfs can be written, and the flash plan that
tells the NAND writer (flash/my_write6.c) to check it.

A diskOS image replaces only the main rootfs; the kernel and the recovery system stay as they are on the Disc. So an
image built from firmware X may only be written onto a Disc whose main kernel + recovery kernel + recovery rootfs are
exactly X's. The writer checks that ON THE DEVICE, in the same USB session, before it unlocks or erases anything; this
module holds the exact identities (from FiiO's official packages, tools/base_catalogue.py) and builds/reads the plan.

Catalogue: every component reassembled from the official update packages and checked against their signed manifests
(size + SHA-256); V2.57's also measured on a real Disc. The recovery VERSION is not an identity: V2.09 ships a different
"recovery 17" build than V2.28/V2.40.
"""
import hashlib
import struct

# MAIN_OS_VER -> base. Lengths in bytes; each is what the component's own header gives (uImage size+64, squashfs
# bytes_used rounded up to 4 KiB), which is how the writer finds it on the NAND.
CATALOGUE = {
    "209": {"id": "v209-r17a", "name": "V2.09 (recovery 17, early build)",
            "main_kernel": (4415552, "f843cfcc28fe3c0d26bf27e35a2d8d595aa200ef82750200c888af5348f4f033"),
            "recovery_kernel": (4042816, "db8a2a7aaf286c381e8a760481fa13b3168a1716ef510834bdea03495a8876df"),
            "recovery_rootfs": (14151680, "367ea3159ad63c63ea8b2d752d3fa1f4d5fc292171db846c82fcfec360389331")},
    "228": {"id": "v228-r17", "name": "V2.28 (recovery 17)",
            "main_kernel": (4423744, "f3c1820f5258a319eca820e0e6d64a4bac764ae36a9eac5de05f255d8910cfeb"),
            "recovery_kernel": (4042816, "5da45c40894c2101249c91063bc164a67f850e1f0b3e7bdf39856e940c26b314"),
            "recovery_rootfs": (14151680, "b093b73076bf4e6040b0d90f8e1a042818db3a8417ea6da520d3de4c10dbb4c5")},
    "240": {"id": "v240-r17", "name": "V2.40 (recovery 17)",
            "main_kernel": (4423744, "5ce48bb5b5e2149777ed367b4bc68146ff132ec45e3a461e62c916d0a3ab2a7a"),
            "recovery_kernel": (4042816, "5da45c40894c2101249c91063bc164a67f850e1f0b3e7bdf39856e940c26b314"),
            "recovery_rootfs": (14151680, "b093b73076bf4e6040b0d90f8e1a042818db3a8417ea6da520d3de4c10dbb4c5")},
    "257": {"id": "v257-r18", "name": "V2.57 (recovery 18)",
            "main_kernel": (4423744, "1a9484efcb64c391b23910306e867cb628af07e059d0d1ccc13e85269e030ed2"),
            "recovery_kernel": (4051008, "9561ccb7ad1bd752eb421509847760a5a88d4ccb49c8a75eb6710b1e9aaf9113"),
            "recovery_rootfs": (16527360, "73b0f8e834d36c0fae9b23f8832df59195bd0f4252a42a1afe4dd15ce0898780")},
}
PARTS = ("main_kernel", "recovery_kernel", "recovery_rootfs")


def catalogue_fingerprint():
    """SHA-256 of the catalogue in a canonical form; recorded in every plan and backup manifest."""
    h = hashlib.sha256()
    for ver in sorted(CATALOGUE):
        e = CATALOGUE[ver]
        h.update(("%s|%s|" % (ver, e["id"])).encode())
        for p in PARTS:
            h.update(("%d:%s|" % e[p]).encode())
    return h.digest()


# ---- the flash plan the writer reads at BLOB (0xa0b00000) --------------------------------------------------------------
BLOB_MAGIC = 0x42534744      # "DGSB"
BLOB_SCHEMA = 1
BLOB_WORDS = 68


def _digest_words(hexdigest):
    """A SHA-256 as the writer holds it: 8 words, word i = big-endian bytes 4i..4i+3."""
    b = bytes.fromhex(hexdigest)
    return [int.from_bytes(b[4 * i:4 * i + 4], "big") for i in range(8)]


def build_plan(main_os_ver, image_path):
    """The immutable plan for writing `image_path` (a padded diskOS/stock rootfs image) onto a Disc that must be on the
    base of `main_os_ver`. Returns the 272-byte blob. Raises KeyError for a version without a complete base entry."""
    base = CATALOGUE[main_os_ver]
    h = hashlib.sha256()
    size = 0
    with open(image_path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            size += len(chunk)
    w = [BLOB_MAGIC, BLOB_SCHEMA, BLOB_WORDS * 4, 0]
    w += list(struct.unpack("<4I", base["id"].encode().ljust(16, b"\0")[:16]))
    for p in PARTS:
        length, sha = base[p]
        w += [length] + _digest_words(sha)
    w += [size] + _digest_words(h.hexdigest())
    w += list(struct.unpack(">8I", catalogue_fingerprint()))
    w += [0] * 8                                          # reserved, must be zero
    body = struct.pack("<60I", *w)
    w += list(struct.unpack(">8I", hashlib.sha256(body).digest()))
    assert len(w) == BLOB_WORDS
    return struct.pack("<%dI" % BLOB_WORDS, *w)


# ---- reading the writer's verdict ------------------------------------------------------------------------------------
G_RESULT, G_A0, G_B0, G_PART, G_ECC_CORR, G_ECC_BAD, G_IMG_SHA, G_SUBCODE, G_ERASING, G_VARIANT, G_BADPOS = \
    128, 129, 130, 131, 161, 162, 163, 171, 172, 173, 200

GATE_CODES = {
    1: "base verified",
    0x9A7E0B5E: "probe: base read (nothing checked, nothing written)",
    0xDEAD0001: "the NAND did not respond to reset",
    0xDEAD0007: "the flash plan was missing or damaged",
    0xDEAD0008: "the Disc is not on the firmware base this image was built for",
    0xDEAD0009: "the image in the device's memory is not the one the plan was made for",
    0xDEAD000A: "the NAND was not in its power-on state (ECC off or already unlocked)",
    0xDEAD000B: "a base partition has an unreadable page (uncorrectable ECC)",
    0xDEAD000C: "a base partition's bad-block marker could not be read reliably",
    0xDEAD000D: "a base partition has no good blocks",
    0xDEAD000E: "a base partition page could not be read",
    0xDEAD0010: "the kernel partition does not hold a kernel image",
    0xDEAD0011: "the recovery partition does not hold a squashfs",
    0xDEAD0012: "a base component claims to be longer than its partition",
}


def _hex(words):
    return b"".join(struct.pack(">I", x) for x in words).hex()


def read_verdict(dbg):
    """Parse the writer's 1 KiB debug block. Returns a dict: result code, observed base parts (length + sha256, for the
    parts it got to), the catalogue base they match (or None), and flags."""
    w = struct.unpack("<256I", dbg[:1024])
    obs, bad_pos = {}, {}
    for i, p in enumerate(PARTS):
        o = w[G_PART + 10 * i:G_PART + 10 * i + 10]
        if o[0]:
            obs[p] = (o[0], _hex(o[1:9]) if any(o[1:9]) else None, o[9])   # (len, sha or None, bad blocks)
        bad_pos[p] = [b for b in w[G_BADPOS + 8 * i:G_BADPOS + 8 * i + min(o[9], 8)]]
    matched = None
    if all(p in obs and obs[p][1] for p in PARTS):
        for ver, e in CATALOGUE.items():
            if all(obs[p][0] == e[p][0] and obs[p][1] == e[p][1] for p in PARTS):
                matched = ver
                break
    return {"result": w[G_RESULT], "meaning": GATE_CODES.get(w[G_RESULT], "unknown result 0x%08X" % w[G_RESULT]),
            "subcode": w[G_SUBCODE], "a0": w[G_A0], "b0": w[G_B0], "ecc_corrected": w[G_ECC_CORR],
            "ecc_bad_page": None if w[G_ECC_BAD] == 0xFFFFFFFF else w[G_ECC_BAD],
            "erase_started": w[G_ERASING] == 1, "variant": w[G_VARIANT], "observed": obs, "matched_base": matched,
            "bad_positions": bad_pos}


def refusal_message(verdict, target_ver):
    """Plain words for a refused flash (nothing was written)."""
    want = CATALOGUE[target_ver]["name"]
    have = verdict.get("matched_base")
    if verdict["result"] == 0xDEAD0008:
        if have:
            return ("Nothing was written. This Disc is running %s, but the selected image is built for %s. A diskOS "
                    "install only replaces the main system; the kernel and recovery come from FiiO's full update. "
                    "Install FiiO's full %s update first, let it finish and boot once, then run the installer again."
                    % (CATALOGUE[have]["name"], want, want.split(" ")[0]))
        return ("Nothing was written. This Disc's kernel/recovery are not a FiiO release this installer knows, so it "
                "cannot confirm the image fits. Install FiiO's full %s update first, then run the installer again."
                % want.split(" ")[0])
    if verdict["result"] == 0xDEAD000A:
        return "Nothing was written. Unplug the Disc, turn it fully off, enter boot mode again and retry."
    return "Nothing was written: %s." % verdict["meaning"]
