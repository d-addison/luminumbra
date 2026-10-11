"""Regression cases for omitted/misplaced capture evidence; synthetic pixels only."""
import contextlib
import hashlib
import importlib.util
import io
import itertools
import json
from pathlib import Path
import tempfile
import unittest

from PIL import Image

SPEC = importlib.util.spec_from_file_location("sweep_montages", Path(__file__).with_name("build-visual-sweep-montages.py"))
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class SweepReview(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "sweep").mkdir()
        self.manifest_path = self.root / "world-visual-sweep-manifest.json"

    def fixture(self, seasons=1):
        cells = []
        for i, (season, tod, weather, angle) in enumerate(itertools.product(
                ("summer", "winter")[:seasons], ("dawn", "noon", "dusk", "night"),
                ("clear", "storm"), ("yaw000", "yaw120", "yaw240", "down35", "up25", "water"))):
            name = f"sweep/{season}__{tod}__{angle}__{weather}.ppm"
            Image.new("RGB", (64, 32), (40 + i, 70, 90)).save(self.root/name)
            cells.append(dict(file=name, season=season, tod=tod, weather=weather, angle=angle,
                              produced=True, non_black=True))
        return dict(schema="luminumbra.world_visual_sweep.v1", passed=True, failures=[],
                    matrix=dict(seasons=seasons, times_of_day=4, angles=6, weather=2),
                    expected_cell_count=len(cells), produced_cell_count=len(cells),
                    non_black_cell_count=len(cells), cells=cells)

    def run_tool(self, manifest):
        self.manifest_path.write_text(json.dumps(manifest))
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = TOOL.main([str(self.root)])
        report = json.loads((self.root / "sweep/montages/review.json").read_text())
        return code, report

    def test_complete_matrix_preserves_original_pixels_and_has_no_visual_approval(self):
        code, report = self.run_tool(self.fixture())
        self.assertEqual(code, 0)
        self.assertTrue(report["evidence_complete"])
        self.assertEqual(report["visual_approval"], "pending_user_review")
        self.assertEqual(len(report["cells"]), 48)
        self.assertEqual(len(report["sheets"]), 8)
        self.assertEqual(report["manifest_sha256"], hashlib.sha256(self.manifest_path.read_bytes()).hexdigest())
        for cell in report["cells"]:
            with Image.open(self.root/cell["source_file"]) as original, Image.open(self.root/cell["png"]) as derived:
                self.assertEqual(original.tobytes(), derived.tobytes())
        with Image.open(self.root/"sweep/montages/montage_summer_dawn.png") as sheet:
            self.assertEqual(sheet.size, (1920, 480))
            # Letterbox preserves the 2:1 source aspect ratio in the 320x180 image region.
            self.assertEqual(sheet.getpixel((160, 32+44+2)), (16, 16, 20))
            self.assertEqual(sheet.getpixel((160, 32+44+90)), (40, 70, 90))

    def test_missing_records_and_unproduced_frames_keep_exact_slots_and_remove_stale_png(self):
        manifest = self.fixture()
        self.run_tool(manifest)
        manifest["cells"].pop(0)
        manifest["cells"][5]["produced"] = False  # storm/yaw000, not the next clear cell
        code, report = self.run_tool(manifest)
        self.assertEqual(code, 1)
        self.assertEqual(report["cells"][0]["status"], "missing record")
        self.assertEqual(report["cells"][6]["status"], "not produced")
        self.assertFalse((self.root/"sweep/png/summer__dawn__yaw000__clear.png").exists())
        with Image.open(self.root/"sweep/montages/montage_summer_dawn.png") as sheet:
            self.assertEqual(sheet.getpixel((320+160, 32+44+90)), (41, 70, 90))
            self.assertEqual(sheet.getpixel((320+160, 32+224+44+90)), (47, 70, 90))

    def test_missing_corrupt_and_misaddressed_images_remain_visible(self):
        manifest = self.fixture()
        (self.root/manifest["cells"][0]["file"]).unlink()
        (self.root/manifest["cells"][1]["file"]).write_bytes(b"broken image")
        manifest["cells"][2]["file"] = "../outside.ppm"
        code, report = self.run_tool(manifest)
        self.assertEqual(code, 1)
        self.assertEqual([c["status"] for c in report["cells"][:3]], ["unreadable capture"]*3)
        self.assertEqual(len(report["cells"]), 48)
        self.assertEqual(len(report["sheets"]), 8)

    def test_duplicate_records_and_false_producer_verdict_cannot_pass(self):
        manifest = self.fixture()
        manifest["cells"][-1] = dict(manifest["cells"][0])
        manifest["passed"] = False
        manifest["failures"] = ["no_water:" + manifest["cells"][5]["file"]]
        code, report = self.run_tool(manifest)
        self.assertEqual(code, 1)
        self.assertEqual(report["cells"][0]["status"], "duplicate record")
        self.assertEqual(report["cells"][5]["status"], "producer failure")
        self.assertEqual(report["cells"][-1]["status"], "missing record")
        self.assertEqual(report["producer_failures"], manifest["failures"])

    def test_omitted_winter_records_do_not_shrink_declared_matrix(self):
        manifest = self.fixture(2)
        manifest["cells"] = manifest["cells"][:48]
        code, report = self.run_tool(manifest)
        self.assertEqual(code, 1)
        self.assertEqual(report["expected_cells"], 96)
        self.assertEqual(len(report["cells"]), 96)
        self.assertEqual(len(report["sheets"]), 12)
        self.assertTrue(all(c["status"] == "missing record" for c in report["cells"][48:]))
        self.assertTrue((self.root/"sweep/montages/montage_winter_night.png").exists())

    def test_unknown_schema_and_matrix_fail_before_deriving_images(self):
        manifest = self.fixture()
        for change in ({"schema": "unknown"}, {"matrix": {"seasons": True}}, {"cells": [None]}):
            self.manifest_path.write_text(json.dumps({**manifest, **change}))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(TOOL.main([str(self.root)]), 2)
            self.assertFalse((self.root/"sweep/montages").exists())


if __name__ == "__main__":
    unittest.main()
