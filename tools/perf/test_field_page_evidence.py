#!/usr/bin/env python3
"""Tiny synthetic contracts; no engine process or native evidence capture."""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import field_page_evidence as evidence


def packet():
    frames = []
    for tick in range(1, 13):
        owners = []
        due = []
        for number in range(1 if tick < 5 else 2):
            frozen = number == 0 and tick >= 9
            owner_key = [number, 0]
            if not frozen:
                due.append(owner_key)
            owners.append({"key": owner_key, "state": 2 if frozen else (1 if number == 0 and tick >= 7 else 0), "cells": 1,
                           "digest": "a" * 16 if number == 0 else "b" * 16,
                           "activated": tick == (1 if number == 0 else 5),
                           "frozen_at": 9 if frozen else 0, "frozen_digest": 42 if frozen else 0,
                           "last_ticked": min(tick, 8) if number == 0 else tick,
                           "completed_work": 0 if frozen else 1})
        frames.append({"tick": tick, "anchor": [256 + ((tick - 1) // 4) * 512, 0, 256],
                       "schedule_digest": "c" * 16, "fields_digest": "d" * 16, "ledger_digest": "e" * 16,
                       "schedule_work": 2, "activation_visits": len(due), "due": due,
                       "owners": owners, "masks": [{"key": row["key"], "words": [1, 0, 0, 0]}
                                                    for row in owners], "tick_us": 1})
    return {"schema": "luminumbra.field_page_evidence.v1", "profile": "diagnostic-pressure",
            "scheduler": {"work_limit": 10, "hold_ticks": 2, "reduced_shift": 1}, "seed": "1337",
            "distance_m": 1024, "stride_m": 512, "frames": frames,
            "reopen": {"tick": 12, "fields_digest": "d" * 16, "ledger_digest": "e" * 16,
                       "exact_equal": True, "continuation_ticks": 2, "save_us": 1, "load_us": 1}}


class FieldPageEvidenceContract(unittest.TestCase):
    def test_valid_pressure_and_separate_measurements(self):
        original = packet()
        self.assertEqual(evidence.validate(original)["freezes"], 1)
        self.assertEqual(evidence.validate(original)["stable_frozen_observations"], 3)
        self.assertEqual(evidence.validate(evidence.parse(json.dumps(original))), evidence.validate(original))
        self.assertEqual(original, packet())
        shipped = packet()
        shipped["profile"] = "shipped-settings"
        shipped["scheduler"].update(work_limit=evidence.U64, hold_ticks=30)
        self.remove_freezing(shipped)
        with self.assertRaisesRegex(ValueError, "pressure"):
            evidence.validate(shipped)
        self.assertEqual(evidence.validate(shipped, measurement=True)["freezes"], 0)
        with self.assertRaisesRegex(ValueError, "20000"):
            evidence.validate(original, full_route=True)

    @staticmethod
    def remove_freezing(value):
        for frame in value["frames"]:
            owner = frame["owners"][0]
            owner.update(state=0, frozen_at=0, frozen_digest=0, last_ticked=frame["tick"], completed_work=1)
            if [0, 0] not in frame["due"]:
                frame["due"].insert(0, [0, 0])
                frame["activation_visits"] += 1

    def test_zero_freeze_activation_work_and_missing_reopen_refuse(self):
        value = packet()
        self.remove_freezing(value)
        with self.assertRaisesRegex(ValueError, "zero freeze"):
            evidence.validate(value)
        value = packet()
        for frame in value["frames"]:
            for owner in frame["owners"]:
                owner["activated"] = False
        with self.assertRaisesRegex(ValueError, "activation"):
            evidence.validate(value)
        value = packet()
        for frame in value["frames"]:
            frame["masks"] = []
            for owner in frame["owners"]:
                owner.update(cells=0, activated=False, state=0, frozen_at=0, frozen_digest=0, completed_work=0)
        with self.assertRaisesRegex(ValueError, "zero activation"):
            evidence.validate(value)
        value = packet()
        for frame in value["frames"]:
            for owner in frame["owners"]:
                owner["completed_work"] = 0
        with self.assertRaisesRegex(ValueError, "completed work"):
            evidence.validate(value)
        for name in ("reopen", "frames", "scheduler"):
            value = packet()
            del value[name]
            with self.subTest(name=name), self.assertRaises(ValueError):
                evidence.validate(value)

    def test_truncation_malformed_json_and_duplicate_fields(self):
        raw = json.dumps(packet()).encode()
        for invalid in (b"", b"{", raw[:-1], b"[]", b"null", b"true", b"1", b'"text"',
                        b'{"schema":1,"schema":2}', b'{"x":NaN}', b'\xff', b"[" * 2000):
            with self.subTest(invalid=invalid[:40]), self.assertRaises(ValueError):
                evidence.validate(evidence.parse(invalid))
        with mock.patch.object(evidence, "MAX_BYTES", 2), self.assertRaises(ValueError):
            evidence.parse(raw)
        for count in (0, 1, 11):
            value = packet()
            value["frames"] = value["frames"][:count]
            with self.subTest(count=count), self.assertRaises(ValueError):
                evidence.validate(value)

    def test_all_fields_reject_missing_unknown_and_invalid_types(self):
        paths = ((), ("scheduler",), ("reopen",), ("frames", 0),
                 ("frames", 0, "owners", 0), ("frames", 0, "masks", 0))
        for path in paths:
            value = packet()
            selected = value
            for part in path:
                selected = selected[part]
            names = list(selected)
            for name in names + ["unknown"]:
                value = packet()
                selected = value
                for part in path:
                    selected = selected[part]
                if name == "unknown":
                    selected[name] = 0
                else:
                    del selected[name]
                with self.subTest(path=path, name=name), self.assertRaises(ValueError):
                    evidence.validate(value)
        for path in (("distance_m",), ("stride_m",), ("frames",), ("scheduler",), ("reopen",),
                     ("frames", 0, "owners"), ("frames", 0, "due"), ("frames", 0, "masks"),
                     ("frames", 0, "owners", 0, "state"), ("frames", 0, "owners", 0, "cells"),
                     ("frames", 0, "owners", 0, "key"), ("frames", 0, "tick_us"),
                     ("frames", 0, "masks", 0, "words")):
            for invalid in (None, True, -1, 2**64, 1e38, "", {}, []):
                value = packet()
                selected = value
                for part in path[:-1]:
                    selected = selected[part]
                selected[path[-1]] = invalid
                with self.subTest(path=path, invalid=invalid), self.assertRaises(ValueError):
                    evidence.validate(value)

    def test_cross_field_refusals(self):
        mutations = (("tick", 2), ("anchor", [257, 0, 256]), ("schedule_digest", "bad"),
                     ("fields_digest", "bad"), ("activation_visits", 0),
                     ("due", [[0, 0], [0, 0]]), ("due", [[42, 0]]))
        for name, invalid in mutations:
            value = packet()
            value["frames"][0][name] = invalid
            with self.subTest(name=name), self.assertRaises(ValueError):
                evidence.validate(value)
        for name, invalid in (("digest", "f" * 16), ("cells", 2), ("completed_work", 1),
                              ("frozen_at", 0), ("frozen_digest", 0), ("last_ticked", 9)):
            value = packet()
            value["frames"][8]["owners"][0][name] = invalid
            with self.subTest(name=name), self.assertRaises(ValueError):
                evidence.validate(value)
        for name, invalid in (("exact_equal", False), ("tick", 11), ("continuation_ticks", 0),
                              ("fields_digest", "f" * 16), ("ledger_digest", ""), ("save_us", -1)):
            value = packet()
            value["reopen"][name] = invalid
            with self.subTest(name=name), self.assertRaises(ValueError):
                evidence.validate(value)
        for name in ("owners", "masks"):
            value = packet()
            value["frames"][0][name] *= 2
            with self.subTest(name=name), self.assertRaises(ValueError):
                evidence.validate(value)
        value = packet()
        value["frames"][0]["masks"][0]["words"] = [0, 0, 0, 0]
        with self.assertRaises(ValueError):
            evidence.validate(value)

    def test_file_and_cli_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "packet.json"
            path.write_bytes(json.dumps(packet()).encode() + b"\r\n")
            self.assertEqual(evidence.read_packet(path), packet())
            result = subprocess.run([sys.executable, "-I", "-B", evidence.__file__, str(path)],
                                    capture_output=True, check=False)
            self.assertEqual(result.returncode, 1)  # Short packets cannot qualify a full route.
            self.assertIn(b"20000", result.stderr)
            with mock.patch.object(evidence, "MAX_BYTES", 2), self.assertRaises(ValueError):
                evidence.read_packet(path)
            with self.assertRaises(OSError):
                evidence.read_packet(Path(directory) / "absent")
            if os.name == "posix":
                fifo = Path(directory) / "fifo"
                os.mkfifo(fifo)
                with self.assertRaises(ValueError):
                    evidence.read_packet(fifo)

    def test_complete_full_route_cli(self):
        value = packet()
        value.update(distance_m=20000, stride_m=128)
        frames = []
        for index in range(632):
            frame = copy.deepcopy(value["frames"][min(index, 11)])
            frame.update(tick=index + 1, anchor=[256 + min(20000, (index // 4) * 128), 0, 256])
            for owner in frame["owners"]:
                if owner["state"] != 2:
                    owner["last_ticked"] = index + 1
            frames.append(frame)
        value["frames"] = frames
        value["reopen"]["tick"] = 632
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "full.json"
            path.write_text(json.dumps(value), encoding="utf-8")
            result = subprocess.run([sys.executable, "-I", "-B", evidence.__file__, str(path)],
                                    capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(json.loads(result.stdout)["distance_m"], 20000)

    def test_bounds(self):
        for name, count in (("frames", evidence.MAX_FRAMES + 1),):
            value = packet()
            value[name] = [copy.deepcopy(value[name][0]) for _ in range(count)]
            with self.assertRaises(ValueError):
                evidence.validate(value)
        for name, count in (("owners", 257), ("masks", 1025), ("due", 257)):
            value = packet()
            value["frames"][0][name] = [copy.deepcopy(value["frames"][0][name][0]) for _ in range(count)]
            with self.subTest(name=name), self.assertRaises(ValueError):
                evidence.validate(value)


if __name__ == "__main__":
    unittest.main()
