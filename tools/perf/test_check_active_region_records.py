#!/usr/bin/env python3
"""Saved-member record decoding and edit-range regressions; synthetic artifacts only.

These share the evidence fixture but run as their own CTest, so each suite stays
inside its 5 s budget.
"""
from __future__ import annotations

import copy
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_active_regions as check
from test_capture_active_regions import receipt, region
from test_check_active_regions import EvidenceFixture


class ActiveRegionRecordEvidenceContract(EvidenceFixture):
    def test_edit_survives_other_chunk_and_water_changes_at_each_boundary(self):
        tour = self.artifact("tour.json")
        for index, stage in enumerate(tour["stages"][2:]):
            prefix = b"other chunk" * (index + 1)
            member = region(prefix + b"edit" + b"water" * (index + 1),
                            others=((9, b"other saved chunk" * (index + 1)),))
            saved = receipt((self.root / "tour/ledger.arl").read_bytes(), member)
            stage["saved_member_sha256"] = saved["files"][0]["sha256"]
            stage["payload_offset"] = len(prefix)
            self.update(stage["saved_member"], member)
            self.update(stage["receipt"], saved)
        self.update("tour.json", tour)
        self.assertTrue(self.validate()["passed"])

    def test_edit_proof_decodes_record_when_compression_and_water_change(self):
        tour = self.artifact("tour.json")
        for index, stage in enumerate(tour["stages"][2:]):
            water = b"water" if index < 2 else b"flood"
            payload = b"edit" * 3 + water
            block = None if index == 0 else b"\x44edit\x04\x00\x50" + water
            member = region(payload, block=block)
            saved = receipt((self.root / "tour/ledger.arl").read_bytes(), member)
            stage["saved_member_sha256"] = saved["files"][0]["sha256"]
            self.update(stage["saved_member"], member)
            self.update(stage["receipt"], saved)
        self.update("tour.json", tour)
        self.assertTrue(self.validate()["passed"])

    def test_region_and_lz4_framing_refusals_and_overlap(self):
        member = region()
        self.assertEqual(check.saved_record(member, 7), b"edit")
        pair = region(others=((9, b"sibling"),))
        first_size = struct.unpack_from("<I", pair, 22)[0]
        reordered = (pair[:8] + pair[26:44] + pair[8:26]
                     + pair[44 + first_size:] + pair[44:44 + first_size])
        self.assertEqual(check.saved_record(reordered, 7), b"edit")
        variants = [b"", b"other" + member[5:], member[:7], member[:-1], member + b"tail",
                    region(others=((7, b"duplicate"),))]
        for offset, format_string, values in ((4, "<H", (0, 1, 3)), (6, "<H", (0, 65535)),
                                              (16, "B", (1, 3)), (17, "B", (4,)),
                                              (18, "<I", (0, check.MAX_RECORD + 1)),
                                              (22, "<I", (0, check.MAX_RECORD + check.MAX_RECORD // 255 + 17))):
            for value in values:
                changed = bytearray(member)
                struct.pack_into(format_string, changed, offset, value)
                variants.append(changed)
        header = struct.pack("<QBBII", 9, 0, 0, check.MAX_RECORD, 1)
        variants.append(b"LMR1" + struct.pack("<HH", 2, 17)
                        + b"".join(struct.pack("<Q", index) + header[8:] for index in range(17)) + b"x" * 17)
        for raw in variants:
            with self.subTest(region=raw[:26]), self.assertRaises(ValueError):
                check.saved_record(raw, 7)
        with self.assertRaisesRegex(ValueError, "missing edited record"):
            check.saved_record(member, 8)
        maximal = region(others=((identity, b"x") for identity in range(65535) if identity != 7))
        self.assertEqual(check.saved_record(maximal, 7), b"edit")
        for raw, size in ((b"", 1), (b"\xf0", 1), (b"\xf0\xff", 1), (b"\x20x", 2),
                          (b"\x20xy", 1), (b"\x10x\x01", 5), (b"\x10x\x00\x00", 5),
                          (b"\x10x\x02\x00", 5), (b"\x10x\x01\x00", 4),
                          (b"\x1fx\x01\x00", 30), (b"\x10x\x01\x00", 5),
                          (b"\x18a\x01\x00\x10x", 14), (b"\x10a\x01\x00\x50bcdef", 10),
                          (b"\x00", 0), (b"\x00", check.MAX_RECORD + 1)):
            with self.subTest(lz4=raw, size=size), self.assertRaises(ValueError):
                check.decode_lz4(raw, size)
        for raw, expected in ((b"\x18a\x01\x00\x50bcdef", b"a" * 13 + b"bcdef"),
                              (b"\x1fa\x01\x00\x00\x50bcdef", b"a" * 20 + b"bcdef"),
                              (b"\xf0\xff\x01" + b"x" * 271, b"x" * 271)):
            self.assertEqual(check.decode_lz4(raw, len(expected)), expected)

    def test_saved_member_range_receipt_and_artifact_failures_at_every_boundary(self):
        original_tour = self.artifact("tour.json")
        for index in range(2, len(check.TOUR_STAGES)):
            for failure in ("missing", "inventory", "tampered", "receipt", "size", "digest", "empty", "offset",
                            "negative", "boolean", "end", "truncated", "huge", "unknown"):
                with self.subTest(stage=check.TOUR_STAGES[index], failure=failure):
                    tour = copy.deepcopy(original_tour)
                    stage = tour["stages"][index]
                    name = stage["saved_member"]
                    # Each case touches only this member and tour.json; restoring
                    # just those keeps the suite inside its 5 s CTest budget.
                    self.restore(name, "tour.json")
                    if failure == "missing":
                        (self.root / name).unlink()
                    elif failure == "inventory":
                        del self.manifest["files"][name]
                    elif failure == "tampered":
                        (self.root / name).write_bytes(b"lost")
                    elif failure == "receipt":
                        stage["saved_member_sha256"] = "f" * 64
                    elif failure == "size":
                        self.update(name, b"edit plus water")
                    elif failure in ("digest", "empty"):
                        self.update(name, b"lost" if failure == "digest" else b"")
                    elif failure == "unknown":
                        stage["saved_member"] = "unregistered.bin"
                    else:
                        stage["payload_offset"] = {"offset": None, "negative": -1,
                                                   "boolean": True, "end": 4, "truncated": 1,
                                                   "huge": 1e38}[failure]
                    self.update("tour.json", tour)
                    with self.assertRaises((OSError, ValueError)):
                        self.validate_retained_tour()

    def test_edited_payload_size_must_be_known_positive_and_bounded(self):
        original = self.artifact("tour.json")
        for field, values in (("size", (None, 0, -1, True, check.MAX_FILE + 1, 1e38)),
                              ("record_id", (None, -1, True, 2**64, 1e38, "7"))):
            for value in values:
                with self.subTest(field=field, value=value):
                    tour = copy.deepcopy(original)
                    tour["edited_payload"][field] = value
                    self.update("tour.json", tour)
                    with self.assertRaisesRegex(ValueError, "missing edited payload"):
                        self.validate_retained_tour()


if __name__ == "__main__":
    unittest.main()
