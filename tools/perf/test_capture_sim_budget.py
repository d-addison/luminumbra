#!/usr/bin/env python3
"""Capture command and evidence refusal tests; no engine or third-party modules needed."""

from __future__ import annotations

import copy
from pathlib import Path
import unittest

import capture_sim_budget as capture


def artifact() -> dict:
    return {
        "schema": "luminumbra.server_tick.v1", "preset": "default", "seed": "1337",
        "ticks_requested": 2, "passed": True, "world_hash": "abc", "world_hash_replay": "abc",
        "sim_budget": {
            "schema": "luminumbra.sim_budget.v1", "work_replay_match": True,
            "stages": [{"name": name, "samples": 2, "work_total": 3,
                        "work_trace": [{"tick": 1, "work": 1}, {"tick": 2, "work": 2}],
                        "duration_ms": {"p50": 0.5, "p95": 0.95, "p99": 0.99, "maximum": 1.0}}
                       for name in capture.STAGES],
        },
    }


class SimBudgetCaptureTests(unittest.TestCase):
    def validate(self, data: dict) -> None:
        capture.validate_artifact(data, "default", "1337", 2)

    def test_unknown_object_keys_are_ignored(self) -> None:
        data = artifact()
        data["extension"] = 1
        data["sim_budget"]["extension"] = 1
        data["sim_budget"]["stages"][0]["extension"] = 1
        self.validate(data)

    def test_absent_corrupt_future_and_mismatched_replay_are_refused(self) -> None:
        for value in (None, {}, {"schema": "luminumbra.sim_budget.v2"},
                      {**artifact()["sim_budget"], "work_replay_match": False}):
            with self.subTest(value=value):
                data = artifact()
                data["sim_budget"] = value
                with self.assertRaises(ValueError):
                    self.validate(data)
        data = artifact()
        data["world_hash_replay"] = "different"
        with self.assertRaises(ValueError):
            self.validate(data)

    def test_missing_reordered_unknown_and_duplicate_stages_are_refused(self) -> None:
        original = artifact()["sim_budget"]["stages"]
        for stages in (original[:-1], list(reversed(original)),
                       original + [original[0]], [{**original[0], "name": "future"}] + original[1:]):
            with self.subTest(stages=stages):
                data = artifact()
                data["sim_budget"]["stages"] = stages
                with self.assertRaises(ValueError):
                    self.validate(data)

    def test_invalid_counts_coverage_and_timers_are_refused(self) -> None:
        bad_stages = []
        for field, values in (("samples", (0, 1, True, 2.0)),
                              ("work_total", (-1, True, 3.0, 4, 2**64))):
            for value in values:
                bad_stages.append({**artifact()["sim_budget"]["stages"][0], field: value})
        for value in (-1, True, 1.0, 2**64):
            stage = copy.deepcopy(artifact()["sim_budget"]["stages"][0])
            stage["work_trace"][0]["work"] = value
            bad_stages.append(stage)
        stage = copy.deepcopy(artifact()["sim_budget"]["stages"][0])
        stage["work_trace"][1]["tick"] = 1
        bad_stages.append(stage)
        for value in (None, -1, float("nan"), float("inf"), True, 2.0):
            stage = copy.deepcopy(artifact()["sim_budget"]["stages"][0])
            stage["duration_ms"]["p50"] = value
            bad_stages.append(stage)
        for stage in bad_stages:
            with self.subTest(stage=stage):
                data = artifact()
                data["sim_budget"]["stages"][0] = stage
                with self.assertRaises(ValueError):
                    self.validate(data)

    def test_empty_population_is_refused(self) -> None:
        data = artifact()
        plants = data["sim_budget"]["stages"][capture.STAGES.index("plants")]
        plants["work_total"] = 0
        for sample in plants["work_trace"]:
            sample["work"] = 0
        with self.assertRaises(ValueError):
            self.validate(data)

    def test_capture_command_pins_presets_population_audio_and_switch(self) -> None:
        self.assertEqual(capture.PRESETS, ("default", "mountains", "archipelago"))
        for preset in capture.PRESETS:
            argv = capture.command(Path("server"), Path("root"), Path("evidence.json"),
                                   preset, "1337", 3600, 1, 1)
            for flag in ("--smoke", "--no-audio", "--sim-budget", "--ecology-roster", "--planted-roster"):
                self.assertIn(flag, argv)
            self.assertEqual(argv[argv.index("--preset") + 1], preset)
            self.assertEqual(argv[argv.index("--ticks") + 1], "3600")
            self.assertEqual(argv[argv.index("--seed") + 1], "1337")


if __name__ == "__main__":
    unittest.main()
