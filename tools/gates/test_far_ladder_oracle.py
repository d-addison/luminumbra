"""File-only verification for the same helper used by FarLodHorizon. No GL/build."""
import copy
import json
import tempfile
import subprocess
import sys
import unittest
from pathlib import Path

from far_ladder_oracle import (ArtifactError, IDENTITY, SNAPSHOT_FIELDS, TIER_COUNTERS,
                               legacy_count, read_artifact, validate_artifact, wanted_tiles)


def fixture(mode="ladder", mixed=False, camera=(-512.0, 12.0, 1024.0)):
    x, y, z = camera
    wanted = wanted_tiles(x, z)
    counts = [sum(key[0] == tier for key in wanted) for tier in range(1, 6)]
    tiers = []
    for tier, count in enumerate(counts, 1):
        counters = dict.fromkeys(TIER_COUNTERS, 0)
        counters.update(tier=tier, pool_limit_bytes=256*1024*1024)
        if mode == "ladder":
            counters.update(wanted=count, current_covering=count, settled=count, empty_pairs=count)
        tiers.append(counters)
    tiles = []
    if mode == "ladder":
        for key in wanted:
            identity = dict(world_epoch=7, content_identity=42, cave_policy=0,
                            key=list(key), bake_generation=5, span_generation=2)
            tiles.append(dict(desired=identity, built=copy.deepcopy(identity), has_pair=True,
                              state="Empty", current=True, span_ok=True, eligibility=4,
                              requested_span=[-256, 80], built_span=[-320, 128]))
        if mixed:
            tiles[0]["state"] = "Uploaded"
            tier = tiers[tiles[0]["desired"]["key"][0] - 1]
            tier.update(uploaded_pairs=1, empty_pairs=tier["empty_pairs"]-1,
                        draws=1, indices=6, retained_gpu_logical_store_bytes=144,
                        pool_gpu_logical_store_bytes=144)
    snapshot = dict(requested_mode=mode, effective_mode=mode, refusal="None", submission_refusal="None", suppression="None",
                    selected=mode == "ladder", world_epoch=7, frame_id=9, content_identity=42,
                    bake_generation=5, far_plane_m=17408 if mode == "ladder" else 3200,
                    cave_policy=0, numeric_policy=1, captured=True, complete=True,
                    advancement_occurred=True, submission_occurred=mixed,
                    draws=int(mixed), indices=6 if mixed else 0, ladder_water_available=False,
                    memory_sampling="asynchronous_diagnostic_not_atomic_admission", gpu_bytes_kind="logical_stores",
                    camera=list(camera), frame_upload=dict(attempts=0, debited_bytes=0, successful_bytes=0,
                                                          horizon_refused=False), tiers=tiers, tiles=tiles)
    row = dict(name="origin", camera=dict(world_x=x, world_y=y, world_z=z, yaw_degrees=0, pitch_degrees=0),
               settle_seconds=4.5, ready_within_deadline=True, paired_capture_preserved=True, snapshot=snapshot,
               legacy=dict(regions_wanted=legacy_count(x, z), regions_resident=legacy_count(x, z), regions_missing=0),
               ladder=dict(wanted_error="None", expected_tiles=[list(key) for key in wanted],
                           expected_by_tier=counts, resident_tiles=[list(key) for key in wanted] if mode == "ladder" else [],
                           stale_tiles=0))
    artifact = dict(schema="luminumbra.farlod_horizon.v2", mode=mode, world_preset="fixture",
                ladder_checked=mode == "ladder", expected_stations=1, stations=[row], complete=True,
                ladder_water="absent_S13_out_of_scope")
    if mode == "legacy":
        artifact.update(camera=dict(world_x=x, world_z=z),
                        legacy=dict(row["legacy"], region_size_m=512, f1_outer_range_m=768, f2_outer_range_m=3000),
                        ladder=copy.deepcopy(row["ladder"]))
    return artifact


def submission_fixture(draws=(1, 1), indices=(3, 6)):
    """Two Uploaded tiers; aggregate evidence exactly matches the tier sums."""
    a = fixture()
    s = a["stations"][0]["snapshot"]
    for tier_number, draw_count, index_count in zip((1, 2), draws, indices):
        uploaded = [t for t in s["tiles"] if t["desired"]["key"][0] == tier_number][:draw_count]
        for tile in uploaded:
            tile["state"] = "Uploaded"
        tier = s["tiers"][tier_number - 1]
        tier.update(uploaded_pairs=draw_count, empty_pairs=tier["empty_pairs"]-draw_count,
                    draws=draw_count, indices=index_count)
    s.update(draws=sum(draws), indices=sum(indices), submission_occurred=True)
    return a


def invalid_submission_fixtures():
    return (
        ("reviewer_zero_and_six", submission_fixture(indices=(0, 6))),
        ("not_triangle_aligned", submission_fixture(indices=(4, 6))),
        ("too_few_indices", submission_fixture(draws=(3, 1), indices=(6, 6))),
    )


def required_station_fixture(mode="ladder"):
    a = fixture(mode)
    original = a["stations"][0]
    a["stations"] = []
    for i, (name, yaw, pitch) in enumerate((("eye_yaw_000", 0, 0), ("eye_yaw_090", 90, 0),
                                           ("eye_yaw_180", 180, 0), ("eye_yaw_270", 270, 0),
                                           ("elevated", 45, -20))):
        row = copy.deepcopy(original)
        row["name"] = name
        row["camera"].update(yaw_degrees=yaw, pitch_degrees=pitch)
        row["snapshot"]["frame_id"] += i
        if name == "elevated":
            row["camera"]["world_y"] += 78.2
            row["snapshot"]["camera"][1] += 78.2
        a["stations"].append(row)
    a["expected_stations"] = 5
    return a


class HorizonValidatorTests(unittest.TestCase):
    def validate(self, a, **kwargs):
        return validate_artifact(a, a["mode"], **kwargs)

    def refused(self, mutate, message=None):
        a = fixture(mixed=True)
        mutate(a)
        with self.assertRaises(ArtifactError) as error:
            self.validate(a)
        if message:
            self.assertIn(message, str(error.exception))

    def test_independent_oracle_published_counts_boundaries_and_negative_translation(self):
        # Pinned independently in #199's existing PowerShell contract.
        for x, z in ((0, 0), (-512, 1024)):
            self.assertEqual(legacy_count(x, z), 132)
            actual = wanted_tiles(x, z)
            self.assertEqual(len(actual), len(set(actual)))
            if x == 0 and z == 0:
                for tier in range(1, 5):
                    self.assertEqual(sum(k[0] == tier for k in actual), 24)
        origin = set(wanted_tiles(0, 0))
        translated = set(wanted_tiles(-8192, 8192))
        self.assertEqual(translated, {(t, x - 8192 // (512*2**(t-1)), z + 8192 // (512*2**(t-1))) for t, x, z in origin})
        for tier in range(1, 6):
            spacing = 4 * 2**(tier - 1)
            edge = 128 * spacing
            radius = 256 * spacing + (4 * spacing if tier < 5 else 0)
            self.assertIn((tier, 0, 0), wanted_tiles(-radius, edge / 2))
            self.assertNotIn((tier, 0, 0), wanted_tiles(-radius - 0.25, edge / 2))
        # V5 contact at exactly 16384 includes cell whose nearest point is x=16384.
        self.assertIn((5, 2, 0), origin)
        self.assertNotIn((5, 2, 0), wanted_tiles(-0.001, 0))
        for x in (float("nan"), float("inf"), 8388608.01):
            with self.assertRaises(ArtifactError):
                wanted_tiles(x, 0)

    def test_valid_empty_and_mixed_fixtures(self):
        self.assertTrue(self.validate(fixture())["LadderChecked"])
        self.assertTrue(self.validate(fixture(mixed=True))["LadderChecked"])
        # GPU pool includes the owner's logical ownership store even with Empty.
        a = fixture()
        a["stations"][0]["snapshot"]["tiers"][4]["pool_gpu_logical_store_bytes"] = 131072
        self.validate(a)

    def test_legacy_is_explicitly_unqualified(self):
        a = fixture("legacy")
        self.assertFalse(self.validate(a)["LadderChecked"])
        with self.assertRaises(ArtifactError):
            validate_artifact(a, "ladder")
        del a["ladder_checked"]
        with self.assertRaises(ArtifactError):
            self.validate(a)

    def test_schema_completeness_every_snapshot_and_counter_field(self):
        for field in SNAPSHOT_FIELDS:
            with self.subTest(snapshot=field):
                self.refused(lambda a: a["stations"][0]["snapshot"].pop(field))
        for field in TIER_COUNTERS:
            with self.subTest(counter=field):
                self.refused(lambda a: a["stations"][0]["snapshot"]["tiers"][0].pop(field))
        for field in IDENTITY:
            with self.subTest(identity=field):
                self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"][0]["built"].pop(field))

    def test_each_station_deadline_and_readiness(self):
        a = fixture()
        a["stations"][0]["settle_seconds"] = 90
        self.validate(a)
        a["stations"][0]["settle_seconds"] = 90.001
        with self.assertRaisesRegex(ArtifactError, "timeout"):
            self.validate(a)
        self.refused(lambda a: a["stations"][0].update(ready_within_deadline=False), "not ready")
        self.refused(lambda a: a["stations"][0].update(paired_capture_preserved=False), "not preserved")
        a = fixture()
        second = copy.deepcopy(a["stations"][0])
        second["name"] = "other"
        second["snapshot"]["frame_id"] += 1
        a["stations"].append(second)
        a["expected_stations"] = 2
        self.validate(a, required_stations=("origin", "other"))
        second["settle_seconds"] = 91
        with self.assertRaisesRegex(ArtifactError, "timeout"):
            self.validate(a)

    def test_wrong_mode_refusal_and_stale_binding_generation_or_camera(self):
        for field, value in (("effective_mode", "legacy"), ("requested_mode", "legacy"),
                             ("refusal", "ShaderProgramUnavailable"), ("suppression", "SubmissionOverride"),
                             ("world_epoch", 8), ("bake_generation", 6), ("content_identity", 43),
                             ("far_plane_m", 3200), ("camera", [0, 0, 0])):
            with self.subTest(field=field):
                self.refused(lambda a: a["stations"][0]["snapshot"].update({field: value}))
        self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"][0]["built"].update(span_generation=1), "stale-generation")
        self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"][0].update(span_ok=False), "spanning")
        self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"][0].update(built_span=[-128, 128]), "span refusal")

    def test_independent_oracle_disagreement_and_omitted_tile_tier_station(self):
        self.refused(lambda a: a["stations"][0]["ladder"]["expected_tiles"].pop(), "oracle disagreement")
        self.refused(lambda a: a["stations"][0]["ladder"]["resident_tiles"].pop(), "missing_expected")
        self.refused(lambda a: a["stations"][0]["snapshot"]["tiles"].pop(), "missing_expected")
        self.refused(lambda a: a["stations"][0]["snapshot"]["tiers"].pop(), "omitted tier")
        self.refused(lambda a: a["stations"][0]["ladder"]["expected_by_tier"].pop(), "omitted expected tier")
        self.refused(lambda a: a["stations"].clear(), "missing station")
        with self.assertRaisesRegex(ArtifactError, "oracle disagreement"):
            self.validate(fixture(), oracle=lambda x, z: wanted_tiles(x + 600, z))
        with self.assertRaisesRegex(ArtifactError, "required station"):
            self.validate(fixture(), required_stations=("origin", "omitted"))

    def test_rejected_horizon_and_counters_are_not_submission_proof(self):
        self.refused(lambda a: a["stations"][0]["snapshot"]["tiers"][0].update(rejected=1), "rejected")
        self.refused(lambda a: a["stations"][0]["snapshot"]["frame_upload"].update(horizon_refused=True), "horizon refusal")
        self.refused(lambda a: a["stations"][0]["snapshot"].update(draws=0, indices=0, submission_occurred=False), "draw totals")
        self.refused(lambda a: a["stations"][0]["snapshot"]["tiers"][0].update(draws=0, indices=0), "GL submission")
        self.refused(lambda a: a["stations"][0]["snapshot"].update(draws=True), "integer")

    def test_each_nonzero_draw_tier_requires_enough_triangle_indices(self):
        self.validate(submission_fixture())  # exactly 3 indices for one draw is valid
        self.validate(submission_fixture(draws=(3, 1), indices=(9, 6)))
        for fault, a in invalid_submission_fixtures():
            with self.subTest(fault=fault), self.assertRaisesRegex(ArtifactError, "invalid submitted index count"):
                self.validate(a)

    def test_aggregate_submission_must_equal_per_tier_sums(self):
        for field, increment in (("draws", 1), ("indices", 3)):
            a = submission_fixture(indices=(6, 6))
            a["stations"][0]["snapshot"][field] += increment
            with self.subTest(field=field), self.assertRaisesRegex(ArtifactError, "per-tier draw totals differ"):
                self.validate(a)

    def test_gate_cli_rejects_corrupt_per_tier_submission(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "artifact.json"
            helper = Path(__file__).with_name("far_ladder_oracle.py")
            for fault, a in invalid_submission_fixtures():
                path.write_text(json.dumps(a))
                result = subprocess.run([sys.executable, "-B", str(helper), "validate", str(path),
                                         "--renderer", "ladder", "--station", "origin"],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, fault)
                self.assertIn("invalid submitted index count", result.stderr, fault)
            for field, increment in (("draws", 1), ("indices", 3)):
                a = submission_fixture(indices=(6, 6))
                a["stations"][0]["snapshot"][field] += increment
                path.write_text(json.dumps(a))
                result = subprocess.run([sys.executable, "-B", str(helper), "validate", str(path),
                                         "--renderer", "ladder", "--station", "origin"],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, field)
                self.assertIn("per-tier draw totals differ", result.stderr, field)

    def test_aggregate_only_v2_and_reused_station_frames_do_not_qualify(self):
        a = fixture()
        del a["stations"]
        with self.assertRaises(ArtifactError):
            self.validate(a)
        a = fixture()
        second = copy.deepcopy(a["stations"][0])
        second["name"] = "other"
        a["stations"].append(second)
        a["expected_stations"] = 2
        with self.assertRaisesRegex(ArtifactError, "stale frame"):
            self.validate(a)
        self.refused(lambda a: a["stations"][0]["snapshot"].update(submission_refusal="BackendUnavailable"), "refusal")

    def test_gate_cli_requires_stations_and_explicit_renderer(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "artifact.json"
            path.write_text(json.dumps(fixture(mixed=True)))
            helper = Path(__file__).with_name("far_ladder_oracle.py")
            base = [sys.executable, "-B", str(helper), "validate", str(path)]
            missing = subprocess.run(base + ["--renderer", "ladder"], capture_output=True, text=True)
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("required station", missing.stderr)
            wrong = subprocess.run(base + ["--station", "origin"], capture_output=True, text=True)
            self.assertNotEqual(wrong.returncode, 0)
            self.assertIn("wrong-mode", wrong.stderr)
            valid = subprocess.run(base + ["--renderer", "ladder", "--station", "origin"], capture_output=True, text=True)
            self.assertEqual(valid.returncode, 0, valid.stderr)
            self.assertTrue(json.loads(valid.stdout)["LadderChecked"])

    def test_new_path_checks_every_legacy_aggregate_assertion(self):
        mutations = (
            lambda a: a.update(camera=None),
            lambda a: a["camera"].update(world_x=None),
            lambda a: a["ladder"].update(expected_tiles=[]),
            lambda a: a["ladder"].update(expected_by_tier=[]),
            lambda a: a["ladder"].update(stale_tiles=1),
            lambda a: a["legacy"].update(regions_wanted=1),
            lambda a: a["legacy"].update(regions_missing=1),
            lambda a: a["legacy"].update(region_size_m=None),
        )
        for mutate in mutations:
            a = required_station_fixture("legacy")
            mutate(a)
            with self.subTest(mutation=mutate), self.assertRaises(ArtifactError):
                self.validate(a)

    def test_required_station_orientations_and_elevated_height(self):
        for mode in ("legacy", "ladder"):
            self.validate(required_station_fixture(mode))
        a = required_station_fixture()
        for row in a["stations"]:
            row["camera"].update(a["stations"][0]["camera"])
            row["snapshot"]["camera"] = list(a["stations"][0]["snapshot"]["camera"])
        with self.assertRaisesRegex(ArtifactError, "orientation"):
            self.validate(a)
        for index in range(5):
            for field in ("yaw_degrees", "pitch_degrees"):
                a = required_station_fixture()
                a["stations"][index]["camera"][field] += 1
                with self.subTest(station=index, field=field), self.assertRaisesRegex(ArtifactError, "orientation"):
                    self.validate(a)
        a = required_station_fixture()
        a["stations"][4]["camera"]["world_y"] += 1
        a["stations"][4]["snapshot"]["camera"][1] += 1
        with self.assertRaisesRegex(ArtifactError, "elevated height"):
            self.validate(a)
        a = required_station_fixture()
        a["stations"][3]["camera"]["yaw_degrees"] = -90.02
        self.validate(a)

    def test_new_gate_cli_rejects_corrupt_compatibility_data(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "artifact.json"
            helper = Path(__file__).with_name("far_ladder_oracle.py")
            for fault in ("camera", "expected_tiles", "expected_by_tier", "stale_tiles"):
                a = required_station_fixture("legacy")
                if fault == "camera": a["camera"] = None
                else: a["ladder"][fault] = 1 if fault == "stale_tiles" else []
                path.write_text(json.dumps(a))
                result = subprocess.run([sys.executable, "-B", str(helper), "validate", str(path),
                                         "--renderer", "legacy"], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, fault)
                self.assertIn("aggregate", result.stderr)

    def test_malformed_truncated_duplicate_and_missing_artifacts(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "artifact.json"
            with self.assertRaises(ArtifactError):
                read_artifact(path)
            valid = json.dumps(fixture())
            for cut in (0, 1, len(valid)//2, len(valid)-1):
                path.write_text(valid[:cut])
                with self.assertRaises(ArtifactError):
                    read_artifact(path)
            for text in ('{"schema":1,"schema":2}', '{"camera":NaN}', '{"a":}'): 
                path.write_text(text)
                with self.assertRaises(ArtifactError):
                    read_artifact(path)
            path.write_text(valid)
            self.validate(read_artifact(path))


if __name__ == "__main__":
    unittest.main()
