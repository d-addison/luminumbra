"""Synthetic contract controls; these never constitute native qualification."""
import copy
import itertools
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from far_edited_qualification import (ArtifactError, NATIVE_SCHEMA, STATION_SCHEMA,
                                      STATIONS, STATION_ORIENTATIONS,
                                      production_tier_budget, read_artifact, validate_native, validate_stations, wanted_tiles)


def stations():
    rows = []
    for index, name in enumerate(STATIONS):
        yaw, pitch = STATION_ORIENTATIONS[name]
        tiles = []
        for tier, tx, tz in wanted_tiles(-512, -512):
            target = dict(world_epoch=7, content_hash=42, cave_policy=0, tier=tier,
                          tx=tx, tz=tz, bake_generation=3, span_generation=1,
                          requested_span=[-256, 256])
            tiles.append(dict(tier=tier, tx=tx, tz=tz, target=target,
                              serving=copy.deepcopy(target), serving_covering=True,
                              target_covering=True, target_water_complete=True,
                              target_state=4, target_built_span=[-256, 256],
                              target_build_failure=None, target_upload_failure=None,
                              target_terrain_indices=0, target_water_indices=0,
                              target_water_known_dry=True))
        snapshot = dict(schema="luminumbra.far_handoff.v1", captured=True, complete=True,
                        world_epoch=7, committed_generation=3, frame=index + 1,
                        target_serial=5, serving_serial=5, target_ready=True, tiles=tiles,
                        submission_refusals=0, submitted_draws=0, submitted_indices=0,
                        submitted_water_indices=0, draws=[],
                        memory=[dict(limit=production_tier_budget(), total_bytes=256, peak_bytes=512) for _ in range(5)])
        rows.append(dict(name=name, requested_mode="ladder", effective_mode="ladder",
                         selection_refused=False, selection_advance=True, selection_submit=True, settle_seconds=90,
                         ready_within_deadline=True, paired_capture_preserved=True,
                         camera=dict(world_x=-512, world_y=80 if name == "elevated" else 1.8,
                                     world_z=-512, yaw_degrees=yaw, pitch_degrees=pitch),
                         snapshot=snapshot))
    return dict(schema=STATION_SCHEMA, world_preset="mountains", complete=True, expected_stations=5, stations=rows)


def native():
    return dict(schema=NATIVE_SCHEMA, execution="native", complete=True,
                chunk_count=5433, edited_chunk_count=2, edit_ms=1,
                authoritative_commit_ms=2, all_tier_bake_ms=3, peak_memory_bytes=4096,
                authoritative_bytes=2048, derived_bytes=1024,
                tiers=[dict(tier=tier, bake_ms=1, bytes=128, complete=True) for tier in range(1, 6)],
                hash_runs=[dict(rendering=render, schedule=schedule, cache=cache, seed=12345,
                                tick_hashes=["0123456789abcdef", "0123456789abcdea", "0123456789abcdeb"])
                           for render, schedule, cache in itertools.product(
                               (False, True), ("serial", "permuted"), ("absent", "present", "removed"))],
                stations=stations())


class SavedFarStationOracle(unittest.TestCase):
    def refused(self, mutate, message):
        artifact = stations()
        mutate(artifact)
        before = copy.deepcopy(artifact)
        with self.assertRaisesRegex(ArtifactError, message):
            validate_stations(artifact)
        self.assertEqual(artifact, before, "refused validation mutated its input")

    def test_all_five_stations_accept_inclusive_ninety_seconds(self):
        self.assertEqual(validate_stations(stations()), dict(status="PASS", stations=5))
        self.refused(lambda a: a["stations"][4].update(settle_seconds=90.000001), "90 seconds")
        self.refused(lambda a: a["stations"][0].update(settle_seconds=-1), "90 seconds")
        self.refused(lambda a: a["stations"][0].update(settle_seconds=float("nan")), "finite")

    def test_stale_serving_pending_failed_and_superseded_targets_refuse(self):
        for update in (dict(target_covering=False, target_state=1),
                       dict(target_covering=False, target_state=5, target_build_failure=1),
                       dict(target_covering=False, target_state=5, target_upload_failure=1),
                       dict(target_covering=False, target_state=2)):
            with self.subTest(update=update):
                self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"][0].update(update), "stale serving")
        self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"][0].update(target=None), "wanted-set")
        self.refused(lambda a: a["stations"][0]["snapshot"].update(target_ready=False), "readiness")

    def test_air_and_solid_empty_and_wet_dry_transitions(self):
        # Empty has no sign payload: both homogeneous signs have the same
        # allocation-free coverage contract (the native CPU regression checks both).
        artifact = stations()
        tile = artifact["stations"][0]["snapshot"]["tiles"][0]
        tile.update(target_state=3, target_water_known_dry=False, target_water_indices=6)
        snapshot = artifact["stations"][0]["snapshot"]
        snapshot.update(submitted_draws=1, submitted_indices=6, submitted_water_indices=6,
                        draws=[dict(tile["target"], submitted=True)])
        self.assertEqual(validate_stations(artifact)["status"], "PASS")
        snapshot.update(submitted_draws=0, submitted_indices=0, submitted_water_indices=0, draws=[])
        tile.update(target_state=4, target_water_known_dry=True, target_water_indices=0)
        self.assertEqual(validate_stations(artifact)["status"], "PASS")
        for update in (dict(target_water_complete=False),
                       dict(target_state=4, target_water_known_dry=False, target_water_indices=6),
                       dict(target_state=3, target_water_known_dry=False, target_water_indices=0)):
            with self.subTest(update=update):
                self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"][0].update(update), "water|Empty|geometry")

    def test_every_station_tier_key_and_pose_is_required(self):
        self.refused(lambda a: a["stations"].pop(), "missing station")
        self.refused(lambda a: a["stations"][3].update(name=STATIONS[0]), "duplicate station")
        self.refused(lambda a: a["stations"][1]["snapshot"].update(frame=1), "strictly ordered")
        self.refused(lambda a: a["stations"][2]["camera"].update(yaw_degrees=0), "orientation")
        self.refused(lambda a: a["stations"][4]["camera"].update(world_y=1.8), "height")
        self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"].pop(), "wanted-set")
        self.refused(lambda a: a["stations"][0]["snapshot"]["memory"].pop(), "memory tier")
        self.refused(lambda a: a["stations"][0].update(paired_capture_preserved=False), "not preserved")

    def test_target_identity_spans_failures_and_memory_cannot_be_borrowed(self):
        artifact = stations()
        for row in artifact["stations"]:
            row["snapshot"]["tiles"][0]["target"]["requested_span"] = [1.8, 1.8]
        self.assertEqual(validate_stations(artifact)["status"], "PASS")
        for update, message in ((dict(world_epoch=8), "foreign"),
                                (dict(bake_generation=4), "future"),
                                (dict(requested_span=[-512, 512]), "does not cover")):
            with self.subTest(update=update):
                self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"][0]["target"].update(update), message)
        self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"][0].update(target_upload_failure=2), "failed target")
        self.refused(lambda a: a["stations"][0]["snapshot"]["memory"][0].update(peak_bytes=production_tier_budget() + 1), "budget")
        self.refused(lambda a: a["stations"][0].update(effective_mode="legacy"), "selection")
        self.refused(lambda a: a["stations"][0]["snapshot"].update(complete=False), "incomplete")

    def test_on_selection_and_actual_submission_are_required_but_empty_is_valid(self):
        self.assertEqual(validate_stations(stations())["status"], "PASS")
        for flag in ("selection_advance", "selection_submit"):
            self.refused(lambda a: a["stations"][0].update({flag: False}), "selection")
            self.refused(lambda a: a["stations"][0].pop(flag), "missing fields")
        artifact = stations()
        snapshot = artifact["stations"][0]["snapshot"]
        tile = snapshot["tiles"][0]
        tile.update(target_state=3, target_terrain_indices=3)
        with self.assertRaisesRegex(ArtifactError, "not submitted"):
            validate_stations(artifact)
        snapshot.update(submitted_draws=1, submitted_indices=3,
                        draws=[dict(tile["target"], submitted=True)])
        self.assertEqual(validate_stations(artifact)["status"], "PASS")
        for mutate, message in (
                (lambda s: s["draws"][0].update(submitted=False), "submitted draw"),
                (lambda s: s["draws"][0].update(content_hash=43), "current target"),
                (lambda s: s.update(submitted_indices=0), "index totals"),
                (lambda s: s.update(submitted_indices=6), "index totals"),
                (lambda s: s.update(submitted_water_indices=3), "index totals"),
                (lambda s: s.update(submitted_draws=0), "count mismatch"),
                (lambda s: s.update(submission_refusals=1), "submission refusal")):
            broken = copy.deepcopy(artifact)
            mutate(broken["stations"][0]["snapshot"])
            with self.subTest(message=message), self.assertRaisesRegex(ArtifactError, message):
                validate_stations(broken)

    def test_one_binding_and_strict_frame_order_across_all_stations(self):
        def change_epoch(artifact):
            snapshot = artifact["stations"][1]["snapshot"]
            snapshot["world_epoch"] = 8
            for tile in snapshot["tiles"]:
                tile["target"]["world_epoch"] = 8
        self.refused(change_epoch, "binding changed")
        self.refused(lambda a: a["stations"][1]["snapshot"].update(committed_generation=4), "binding changed")
        for field, value in (("cave_policy", 1), ("content_hash", 43), ("bake_generation", 2)):
            def change_binding(artifact):
                for tile in artifact["stations"][1]["snapshot"]["tiles"]:
                    tile["target"][field] = value
            self.refused(change_binding, "binding changed")
        for frame in (1, 0):
            self.refused(lambda a: a["stations"][1]["snapshot"].update(frame=frame), "strictly ordered")
        self.refused(lambda a: a["stations"][1]["snapshot"].update(target_serial=4), "serial regressed")
        self.refused(lambda a: a["stations"].reverse(), "station order")
        # Common support must be established before the first station; a later
        # serial alone is allowed, but capture support cannot change mid-sweep.
        artifact = stations()
        artifact["stations"][-1]["snapshot"]["target_serial"] += 1
        self.assertEqual(validate_stations(artifact)["status"], "PASS")
        self.refused(lambda a: a["stations"][-1]["snapshot"]["tiles"][0]["target"].update(
            span_generation=2), "binding changed")

    def test_exact_submission_rejects_omitted_tile_or_whole_tier_and_extra_draws(self):
        artifact = stations()
        snapshot = artifact["stations"][0]["snapshot"]
        tiles = snapshot["tiles"]
        drawable = tiles[:2] + [next(t for t in tiles if t["tier"] == 2)]
        for tile in drawable:
            tile.update(target_state=3, target_terrain_indices=3, target_water_indices=6,
                        target_water_known_dry=False)
        def submit(value, selected):
            value.update(submitted_draws=len(selected), submitted_indices=9 * len(selected),
                         submitted_water_indices=6 * len(selected),
                         draws=[dict(t["target"], submitted=True) for t in selected])
        submit(snapshot, drawable)
        self.assertEqual(validate_stations(artifact)["status"], "PASS")
        for selected in (drawable[1:], drawable[:2], [], drawable + [drawable[0]],
                         drawable + [next(t for t in tiles if t not in drawable)]):
            broken = copy.deepcopy(artifact)
            submit(broken["stations"][0]["snapshot"], selected)
            with self.subTest(keys=[t["target"]["tier"] for t in selected]), self.assertRaises(ArtifactError):
                validate_stations(broken)
        broken = copy.deepcopy(artifact)
        broken["stations"][0]["snapshot"]["draws"][0]["tx"] += 10000
        with self.assertRaisesRegex(ArtifactError, "foreign"):
            validate_stations(broken)

    def test_common_support_refuses_growth_shrink_or_authority_change_during_sweep(self):
        for update in (dict(content_hash=43), dict(cave_policy=1), dict(bake_generation=2),
                       dict(span_generation=2, requested_span=[-512, 512]),
                       dict(requested_span=[-128, 128])):
            def mutate(artifact):
                for tile in artifact["stations"][-1]["snapshot"]["tiles"]:
                    tile["target"].update(update)
                    if update.get("span_generation") == 2:
                        tile["target_built_span"] = [-512, 512]
            with self.subTest(update=update):
                self.refused(mutate, "binding changed")
        self.refused(lambda a: a["stations"][-1]["snapshot"]["tiles"][0].update(
            target_built_span=[-512, 512]), "binding changed")
        artifact = stations()
        for row in artifact["stations"]:
            for tile in row["snapshot"]["tiles"]:
                tile["target"]["requested_span"] = None
                tile["target_built_span"] = [-16, 16]
        with self.assertRaisesRegex(ArtifactError, "station height"):
            validate_stations(artifact)

    def test_memory_limits_cannot_be_raised_above_engine_production_profile(self):
        budget = production_tier_budget()
        artifact = stations()
        for row in artifact["stations"]:
            for memory in row["snapshot"]["memory"]:
                memory.update(total_bytes=budget, peak_bytes=budget)
        self.assertEqual(validate_stations(artifact)["status"], "PASS")
        for tier in range(5):
            self.refused(lambda a: a["stations"][0]["snapshot"]["memory"][tier].update(
                limit=budget + 1, peak_bytes=budget + 1), "production memory budget")
            self.refused(lambda a: a["stations"][0]["snapshot"]["memory"][tier].update(
                limit=budget + 1), "production memory budget")

    def test_absent_extra_span_accepts_public_serializer_null(self):
        artifact = stations()
        for row in artifact["stations"]:
            for tile in row["snapshot"]["tiles"]:
                tile["target"]["requested_span"] = None
        self.assertEqual(validate_stations(artifact)["status"], "PASS")
        for value in ([], "absent", [1], [-512, 512], [256, -256]):
            self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"][0]["target"].update(
                requested_span=value), "span")


def writer_contract(executable):
    # Exercise the actual non-null saved-world writer, serializer and readiness
    # helper. These bounded, file-only controls never constitute native evidence.
    with tempfile.TemporaryDirectory() as directory:
        for mode in ("saved", "saved-empty", "saved-suppressed", "saved-unsubmitted", "saved-incomplete",
                     "saved-omit-tile", "saved-omit-tier", "saved-common-support", "saved-recapture",
                     "saved-authority-change"):
            path = Path(directory) / mode
            subprocess.run([executable, str(path), mode], check=True)
            artifact = read_artifact(path / "farlod-horizon-edited-oracle.json")
            if mode in ("saved", "saved-empty", "saved-common-support"):
                assert validate_stations(artifact) == dict(status="PASS", stations=5)
            else:
                assert artifact["complete"] is (mode in ("saved-recapture", "saved-authority-change"))
                if mode != "saved-incomplete":
                    # Recheck raw ON evidence even if readiness/completion claims
                    # were forged; serializer flags/counters remain authoritative.
                    artifact["complete"] = True
                    for row in artifact["stations"]:
                        row["ready_within_deadline"] = True
                try:
                    validate_stations(artifact)
                except ArtifactError:
                    continue
                raise AssertionError(f"writer output incorrectly qualified: {mode}")


class NativeFarQualification(unittest.TestCase):
    def test_unexecuted_native_qualification_is_explicitly_pending(self):
        for execution, complete in (("pending", False), ("analogue", True), ("native", False)):
            artifact = dict(schema=NATIVE_SCHEMA, execution=execution, complete=complete)
            self.assertEqual(validate_native(artifact)["status"], "PENDING")

    def test_all_measurements_and_tiers_are_required(self):
        self.assertEqual(validate_native(native())["status"], "PASS")
        for field in ("edit_ms", "authoritative_commit_ms", "all_tier_bake_ms",
                      "peak_memory_bytes", "authoritative_bytes", "derived_bytes"):
            artifact = native()
            del artifact[field]
            with self.subTest(field=field), self.assertRaisesRegex(ArtifactError, "missing fields"):
                validate_native(artifact)
        for change in (lambda a: a.update(chunk_count=5432),
                       lambda a: a["tiers"].pop(),
                       lambda a: a["tiers"][4].update(complete=False),
                       lambda a: a.update(edit_ms=float("inf"))):
            artifact = native()
            change(artifact)
            with self.assertRaises(ArtifactError):
                validate_native(artifact)

    def test_hashes_match_rendering_schedules_and_cache_creation_removal(self):
        for index in range(12):
            artifact = native()
            artifact["hash_runs"][index]["tick_hashes"][1] = "ffffffffffffffff"
            with self.subTest(index=index), self.assertRaisesRegex(ArtifactError, "simulation hashes differ"):
                validate_native(artifact)
        artifact = native()
        artifact["hash_runs"][-1] = copy.deepcopy(artifact["hash_runs"][0])
        with self.assertRaisesRegex(ArtifactError, "duplicate hash"):
            validate_native(artifact)
        artifact = native()
        artifact["hash_runs"][0]["seed"] = 7
        with self.assertRaisesRegex(ArtifactError, "fixed seed"):
            validate_native(artifact)

    def test_native_cannot_select_the_best_or_skip_a_station(self):
        artifact = native()
        artifact["stations"]["stations"][0]["snapshot"]["tiles"][0]["target_covering"] = False
        with self.assertRaisesRegex(ArtifactError, "stale serving"):
            validate_native(artifact)

    def test_cli_missing_is_pending_and_malformed_is_failure(self):
        script = Path(__file__).with_name("far_edited_qualification.py")
        bootstrap = "import runpy,sys; sys.path.insert(0,sys.argv[1]); sys.argv=sys.argv[2:]; runpy.run_path(sys.argv[0],run_name='__main__')"
        command = [sys.executable, "-I", "-B", "-c", bootstrap, str(script.parent), str(script), "native"]
        pending = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(pending.returncode, 3, pending.stderr)
        self.assertEqual(json.loads(pending.stdout)["status"], "PENDING")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "native.json"
            path.write_text('{"schema":', encoding="utf-8")
            failed = subprocess.run(command + [str(path)], capture_output=True, text=True, check=False)
            self.assertEqual(failed.returncode, 1, failed.stderr)
            self.assertEqual(json.loads(failed.stdout)["status"], "FAIL")
            path.write_text(json.dumps(dict(schema=NATIVE_SCHEMA, execution="pending", complete=False)), encoding="utf-8")
            pending = subprocess.run(command + [str(path)], capture_output=True, text=True, check=False)
            self.assertEqual(pending.returncode, 3, pending.stderr)
            path.write_text('{"schema":"x","schema":"y"}', encoding="utf-8")
            failed = subprocess.run(command + [str(path)], capture_output=True, text=True, check=False)
            self.assertEqual(failed.returncode, 1, failed.stderr)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--writer-fixture":
        writer_contract(sys.argv[2])
    else:
        unittest.main()
