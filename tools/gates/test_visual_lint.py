"""Synthetic capture regressions; no native renderer or reference assets needed."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path, PureWindowsPath
import tempfile
import subprocess
import sys
import unittest
from unittest import mock

import numpy as np
from PIL import Image, ImageDraw

SPEC = importlib.util.spec_from_file_location("visual_lint", Path(__file__).with_name("visual_lint.py"))
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class VisualLint(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.shots = self.root / "shots"
        self.shots.mkdir()

    def image(self, name="frame.png", color=(80, 100, 120), size=(96, 64), root=None):
        path = (root or self.shots) / name
        Image.new("RGB", size, color).save(path)
        return path

    def sidecar(self, metadata, name="frame", root=None):
        ((root or self.shots) / (name + ".json")).write_text(json.dumps(metadata), encoding="utf-8")

    def checks(self, report):
        return {finding["check"] for frame in report["frames"] for finding in frame["findings"]}

    def run_cli(self, *args):
        output, summary = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(summary):
            code = TOOL.main([str(self.shots), *map(str, args)])
        return code, json.loads(output.getvalue()), summary.getvalue()

    def test_cli_process_exit_codes_and_streams(self):
        self.image()
        command = [sys.executable, "-I", "-B", str(Path(TOOL.__file__)), str(self.shots)]
        for color, expected in (((80, 100, 120), 0), ((255, 0, 255), 1)):
            self.image(color=color)
            result = subprocess.run(command, capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(result.returncode, expected)
            self.assertEqual(json.loads(result.stdout)["errors"], [])
            self.assertIn("Visual lint:", result.stderr)
        (self.shots / "frame.png").write_bytes(b"partial")
        result = subprocess.run(command, capture_output=True, text=True, timeout=10, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(len(json.loads(result.stdout)["errors"]), 1)
        error_report = dict(frames=[], comparisons=[], finding_count=0,
                            errors=[dict(file="bad\n.png", error="denied\x1b[0m")])
        with mock.patch.object(TOOL, "analyze", return_value=error_report):
            code, report, summary = self.run_cli()
        self.assertEqual(code, 2)
        self.assertEqual(report, error_report)
        self.assertIn(r"bad\n.png", summary)
        self.assertNotIn("\x1b", summary)

    def test_cluster_colors_areas_and_connectivity(self):
        for color, check in (((255, 0, 255), "magenta_cluster"), ((0, 0, 0), "black_cluster"),
                             ((255, 255, 255), "white_cluster")):
            with self.subTest(check=check):
                image = Image.new("RGB", (96, 64), (80, 100, 120))
                ImageDraw.Draw(image).rectangle((20, 20, 27, 27), fill=color)
                image.save(self.shots / "frame.png")
                code, report, summary = self.run_cli()
                self.assertEqual(code, 1)
                self.assertIn(check, self.checks(report))
                self.assertIn(check, summary)
                finding = next(f for f in report["frames"][0]["findings"] if f["check"] == check)
                self.assertEqual(finding["area"], 64)
                self.assertEqual(finding["box"], [20, 20, 28, 28])
        mask = np.array([[1, 0, 1], [1, 1, 1], [0, 0, 0]], dtype=bool)
        self.assertEqual(TOOL.components(mask), [[5, [0, 0, 3, 2]]])
        self.assertEqual(len(TOOL.components(np.eye(4, dtype=bool))), 4)
        self.assertEqual(TOOL.components(np.zeros((3, 3), dtype=bool)), [])

    def test_isolated_sentinels_and_near_extremes_are_not_clusters(self):
        image = Image.new("RGB", (96, 64), (1, 1, 1))
        for index, color in enumerate(((255, 0, 255), (0, 0, 0), (255, 255, 255))):
            image.putpixel((index * 20, 2), color)
        image.save(self.shots / "frame.png")
        self.assertFalse(self.checks(TOOL.analyze(self.shots)))
        self.image(color=(254, 254, 254))
        self.assertFalse(self.checks(TOOL.analyze(self.shots)))

    def test_opaque_rectangle_and_nonrectangular_or_smooth_controls(self):
        image = Image.new("RGB", (96, 64), (100, 130, 170))
        ImageDraw.Draw(image).rectangle((20, 30, 40, 50), fill=(40, 50, 60))
        image.save(self.shots / "frame.png")
        self.assertIn("opaque_rectangle", self.checks(TOOL.analyze(self.shots)))
        array = np.array(image, dtype=np.int16)
        for x in range(20, 41):
            array[30:51, x] += x-20
        Image.fromarray(array.astype(np.uint8)).save(self.shots / "frame.png")
        self.assertNotIn("opaque_rectangle", self.checks(TOOL.analyze(self.shots)))
        for shape in ("ellipse", "polygon"):
            image = Image.new("RGB", (96, 64), (100, 130, 170))
            draw = ImageDraw.Draw(image)
            if shape == "ellipse":
                draw.ellipse((20, 30, 40, 50), fill=(40, 50, 60))
            else:
                draw.polygon(((20, 30), (30, 50), (40, 30)), fill=(40, 50, 60))
            image.save(self.shots / "frame.png")
            self.assertNotIn("opaque_rectangle", self.checks(TOOL.analyze(self.shots)))

    def test_sidecar_horizon_pitch_and_estimated_horizon(self):
        rgb = np.full((64, 96, 3), (30, 90, 180), dtype=np.int16)
        rgb[52:] = (90, 60, 30)
        for metadata, source in (({"horizon_row_from_top": 32}, "sidecar"),
                                 ({"pixels": {"horizon_row_from_top": 32}}, "sidecar"),
                                 ({"camera": {"pitch": 0, "fov": 70}}, "pitch"),
                                 ({"pitch_degrees": 0}, "pitch_default_fov")):
            with self.subTest(metadata=metadata):
                self.sidecar(metadata)
                Image.fromarray(rgb.astype(np.uint8)).save(self.shots / "frame.png")
                report = TOOL.analyze(self.shots)
                self.assertIn("sky_below_horizon", self.checks(report))
                self.assertEqual(report["frames"][0]["horizon_source"], source)
        self.assertGreater(TOOL.horizon(rgb, {"camera": {"pitch": 25}}, 64)[0], 32)
        self.assertLess(TOOL.horizon(rgb, {"pitch_degrees": -25}, 64)[0], 32)
        rgb[32:] = (90, 60, 30)
        self.assertEqual(TOOL.horizon(rgb, {}, 64), (32, "estimated"))
        self.assertEqual(TOOL.sky_check(rgb, 32), [])
        self.assertEqual(TOOL.sky_check(rgb, -10), [])
        self.assertEqual(TOOL.sky_check(rgb, 100), [])
        self.assertEqual(TOOL.horizon(np.ones_like(rgb)*100, {}, 64), (None, "unresolved"))

    def test_invalid_sidecars_and_unknown_metadata(self):
        self.image()
        for metadata in ({"camera": []}, {"pixels": []}, {"pitch_degrees": 1e38},
                         {"pitch_degrees": 10**400}, {"pitch_degrees": True},
                         {"camera": {"pitch": float("nan")}},
                         {"camera": {"pitch": 0, "fov": 0}}, {"horizon_row_from_top": "32"},
                         {"horizon_row_from_top": float("inf")}, {"comparison_group": "group"},
                         {"comparison_group": "x"*257, "variant": "wet"},
                         {"comparison_group": "g", "variant": " "}):
            with self.subTest(metadata=metadata):
                self.sidecar(metadata)
                code, report, _ = self.run_cli()
                self.assertEqual(code, 2)
                self.assertEqual(len(report["errors"]), 1)
        self.sidecar({"unrecognized": {"pitch": "anything"}})
        self.assertEqual(self.run_cli()[0], 0)
        for text in ("{", "[]", "["*2000 + "]"*2000):
            (self.shots / "frame.json").write_text(text, encoding="utf-8")
            self.assertEqual(self.run_cli()[0], 2)
        self.sidecar({})
        self.sidecar({}, "frame.png")
        self.assertIn("Ambiguous", self.run_cli()[1]["errors"][0]["error"])

    def test_banding_repeated_steps_and_smooth_noisy_controls(self):
        banded = np.repeat(np.arange(12)*4+40, 8)
        rgb = np.broadcast_to(banded[None, :, None], (64, 96, 3)).astype(np.int16)
        self.assertEqual(TOOL.banding(rgb)[0]["axis"], "horizontal")
        self.assertEqual(TOOL.banding(rgb.transpose(1, 0, 2))[0]["axis"], "vertical")
        self.assertEqual(TOOL.banding(np.ones((64, 96, 3), dtype=np.int16)*100), [])
        gradient = np.broadcast_to(np.arange(96)[None, :, None]+40, (64, 96, 3))
        self.assertEqual(TOOL.banding(gradient), [])
        rng = np.random.default_rng(23)
        self.assertEqual(TOOL.banding(gradient+rng.integers(-8, 9, size=(64, 96, 3))), [])
        Image.fromarray(rgb.astype(np.uint8)).save(self.shots / "frame.png")
        self.assertIn("gradient_banding", self.checks(self.run_cli()[1]))
        self.assertAlmostEqual(float(TOOL.luma(np.array([[[255, 255, 255]]], dtype=np.int16))[0, 0]), 255)

    def test_weather_ground_equality_despite_changed_sky(self):
        for name, sky in (("weather-baseline", (30, 50, 100)), ("weather-visual", (50, 70, 120))):
            image = Image.new("RGB", (96, 64), sky)
            ImageDraw.Draw(image).rectangle((0, 40, 95, 63), fill=(100, 90, 70))
            image.save(self.shots / (name + ".png"))
        report = TOOL.analyze(self.shots)
        self.assertEqual(len(report["comparisons"]), 1)
        self.assertEqual(report["comparisons"][0]["region"], "ground")
        # Small temporal noise should still identify an unchanged ground region.
        with Image.open(self.shots / "weather-visual.png") as image:
            array = np.asarray(image.convert("RGB"), dtype=np.int16).copy()
        array[40:] += 1
        Image.fromarray(array.astype(np.uint8)).save(self.shots / "weather-visual.png")
        self.assertEqual(len(TOOL.analyze(self.shots)["comparisons"]), 1)
        self.image("weather-visual.png", (70, 80, 90))
        self.assertFalse(TOOL.analyze(self.shots)["comparisons"])

    def test_sweep_pairs_duplicates_explicit_metadata_and_unrelated_frames(self):
        for name in ("summer__noon__yaw000__clear", "summer__noon__yaw000__storm"):
            self.image("sweep__" + name + ".png")
        self.assertEqual(len(TOOL.analyze(self.shots)["comparisons"]), 1)
        self.image("sweep__png__summer__noon__yaw000__clear.png")
        self.assertEqual(len(TOOL.analyze(self.shots)["comparisons"]), 1)
        self.image("sweep__png__summer__noon__yaw000__clear.png", (90, 100, 120))
        self.assertTrue(TOOL.analyze(self.shots)["errors"])
        for path in self.shots.iterdir():
            path.unlink()
        self.image("one.png")
        self.image("two.png")
        self.assertFalse(TOOL.analyze(self.shots)["comparisons"])
        for name, variant in (("one", "dry"), ("two", "wet")):
            self.sidecar(dict(comparison_group="same_camera", variant=variant), name)
        self.assertEqual(len(TOOL.analyze(self.shots)["comparisons"]), 1)
        self.image("two.png", size=(48, 32))
        self.assertEqual(self.run_cli()[0], 2)
        self.image("two.png")
        with mock.patch.dict(TOOL.T, max_variants=1):
            self.assertTrue(TOOL.analyze(self.shots)["errors"])

    def test_baseline_histogram_and_dimensions_missing_corrupt_inputs(self):
        baseline = self.root / "baseline"
        baseline.mkdir()
        self.image()
        self.image(root=baseline)
        self.assertNotIn("histogram_drift", self.checks(TOOL.analyze(self.shots, baseline)))
        self.image(root=baseline, color=(180, 200, 220))
        self.assertIn("histogram_drift", self.checks(TOOL.analyze(self.shots, baseline)))
        self.image(root=baseline, size=(48, 32))
        self.assertTrue(TOOL.analyze(self.shots, baseline)["errors"])
        (baseline / "frame.png").unlink()
        self.assertTrue(TOOL.analyze(self.shots, baseline)["errors"])
        (baseline / "frame.png").write_bytes(b"partial")
        self.assertTrue(TOOL.analyze(self.shots, baseline)["errors"])

    def test_transparent_and_animated_images_are_not_static_opaque_captures(self):
        path = self.shots / "frame.png"
        Image.new("RGBA", (96, 64), (80, 100, 120, 0)).save(path)
        self.assertEqual(self.run_cli()[0], 2)
        Image.new("RGBA", (96, 64), (80, 100, 120, 255)).save(path)
        self.assertEqual(self.run_cli()[0], 0)
        Image.new("RGB", (96, 64), (80, 100, 120)).save(
            path, save_all=True, append_images=[Image.new("RGB", (96, 64), (40, 60, 80))])
        self.assertEqual(self.run_cli()[0], 2)

    def test_empty_bad_directories_partial_images_and_read_refusals(self):
        self.assertEqual(self.run_cli()[0], 2)
        with self.assertRaisesRegex(ValueError, "empty"):
            TOOL.directory("")
        self.assertEqual(self.run_cli("--baseline", self.root / "absent")[0], 2)
        self.image()
        self.assertEqual(self.run_cli("--baseline", "")[0], 2)
        self.assertEqual(self.run_cli("--sidecars", "")[0], 2)
        self.assertEqual(self.run_cli("--sidecars", self.root / "absent")[0], 2)
        path = self.shots / "frame.png"
        payload = path.read_bytes()
        for cut in (1, 8, 12):
            path.write_bytes(payload[:-cut])
            self.assertEqual(self.run_cli()[0], 2)
        path.write_bytes(payload[:len(payload)//2])
        self.assertEqual(self.run_cli()[0], 2)
        self.image()
        with mock.patch.object(TOOL, "read_regular", side_effect=PermissionError("denied")):
            self.assertEqual(self.run_cli()[0], 2)
        with mock.patch.object(TOOL.os, "fstat", return_value=mock.Mock(st_mode=0o040755, st_size=0)):
            with self.assertRaisesRegex(ValueError, "regular"):
                TOOL.read_regular(path, 1024)
        with mock.patch.object(TOOL.os, "fstat", return_value=mock.Mock(st_mode=0o100644, st_size=len(payload)+1,
                                                                       st_file_attributes=0)):
            with self.assertRaisesRegex(ValueError, "truncated"):
                TOOL.read_regular(path, 1024)
        with mock.patch.object(TOOL.os, "fstat", return_value=mock.Mock(st_mode=0o100644, st_size=1,
                                                                       st_file_attributes=0)):
            with self.assertRaisesRegex(ValueError, "grew"):
                TOOL.read_regular(path, 32)
        with self.assertRaisesRegex(ValueError, "byte limit"):
            TOOL.read_regular(path, 1)

    def test_bounds_determinism_external_sidecars_and_json_bom_crlf(self):
        self.image()
        sidecars = self.root / "sidecars"
        sidecars.mkdir()
        (sidecars / "frame.json").write_bytes(b'\xef\xbb\xbf{\r\n"pitch_degrees": 0\r\n}\r\n')
        first = TOOL.analyze(self.shots, sidecar_dir=sidecars)
        self.assertEqual(first, TOOL.analyze(self.shots, sidecar_dir=sidecars))
        self.assertEqual(first["frames"][0]["horizon_source"], "pitch_default_fov")
        for key, limit in (("max_pixels", 1), ("max_file_bytes", 1), ("max_sidecar_bytes", 1)):
            with self.subTest(key=key), mock.patch.dict(TOOL.T, {key: limit}):
                self.assertEqual(self.run_cli("--sidecars", sidecars)[0], 2)
        self.image("other.png")
        with mock.patch.dict(TOOL.T, max_frames=1):
            self.assertEqual(self.run_cli()[0], 2)
        with mock.patch.dict(TOOL.T, max_entries=1):
            self.assertEqual(self.run_cli()[0], 2)
        Image.new("RGB", (96, 64)).save(self.shots / "wrong.png", format="BMP")
        self.assertEqual(self.run_cli()[0], 2)

    def test_windows_handle_paths_refusals_and_ownership(self):
        import ctypes
        from ctypes import wintypes
        import ntpath
        from types import SimpleNamespace

        payload_path = self.image()
        payload = payload_path.read_bytes()
        api = mock.Mock()
        api.CreateFileW.return_value = 123
        final_name = [r"\\?\C:\Long captures\frame.png"]

        def final_path(_handle, buffer, _size, _flags):
            buffer.value = final_name[0]
            return len(buffer.value)

        api.GetFinalPathNameByHandleW.side_effect = final_path
        adapter = SimpleNamespace(open_osfhandle=mock.Mock(
            side_effect=lambda _handle, _flags: os.open(payload_path, os.O_RDONLY)))
        path = PureWindowsPath("C:/Long captures/frame.png")
        # Native handles are emulated; all actual byte reads still use a regular
        # temporary descriptor, including the denied/escaped handle paths.
        with mock.patch.object(ctypes, "WinDLL", return_value=api, create=True), \
                mock.patch.object(ctypes, "WinError", side_effect=lambda _error: OSError("win32 error"), create=True), \
                mock.patch.object(ctypes, "get_last_error", return_value=5, create=True), \
                mock.patch.dict(sys.modules, msvcrt=adapter), \
                mock.patch.object(TOOL.os, "name", "nt"), mock.patch.object(TOOL.os, "path", ntpath):
            self.assertEqual(TOOL.read_regular(path, 1024), payload)
            self.assertEqual(api.CreateFileW.call_args.args[0], r"\\?\C:\Long captures\frame.png")
            api.CloseHandle.assert_not_called()
            self.assertEqual(TOOL.read_regular(PureWindowsPath(r"\\?\C:\Long captures\frame.png"), 1024), payload)
            final_name[0] = r"\\?\C:\Long captures\CON.png"
            self.assertEqual(TOOL.read_regular(PureWindowsPath("C:/Long captures/CON.png"), 1024), payload)
            self.assertEqual(api.CreateFileW.call_args.args[0], r"\\?\C:\Long captures\CON.png")
            final_name[0] = r"\\?\C:\elsewhere\frame.png"
            with self.assertRaisesRegex(ValueError, "escapes"):
                TOOL.read_regular(path, 1024)
            api.CloseHandle.assert_called_with(123)
            final_name[0] = ""
            with self.assertRaisesRegex(OSError, "resolve"):
                TOOL.read_regular(path, 1024)
            final_name[0] = r"\\?\UNC\server\captures\frame.png"
            self.assertEqual(TOOL.read_regular(PureWindowsPath(r"\\server\captures\frame.png"), 1024), payload)
            self.assertEqual(api.CreateFileW.call_args.args[0], final_name[0])
            final_name[0] = r"\\?\C:\Long captures\frame.png"
            with mock.patch.object(TOOL.os, "fstat", return_value=SimpleNamespace(
                    st_mode=0o100644, st_size=len(payload), st_file_attributes=0x400)):
                with self.assertRaisesRegex(ValueError, "reparse"):
                    TOOL.read_regular(path, 1024)
            adapter.open_osfhandle.side_effect = OSError("denied descriptor")
            with self.assertRaisesRegex(OSError, "denied"):
                TOOL.read_regular(path, 1024)
            api.CreateFileW.return_value = wintypes.HANDLE(-1).value
            with self.assertRaisesRegex(OSError, "win32"):
                TOOL.read_regular(path, 1024)

    def test_symlink_and_fifo_inputs_are_refused_without_following_or_blocking(self):
        self.image()
        if os.name == "posix":
            (self.shots / "frame.png").unlink()
            target = self.image("target.png", root=self.root)
            (self.shots / "frame.png").symlink_to(target)
            self.assertEqual(self.run_cli()[0], 2)
            (self.shots / "frame.png").unlink()
            os.mkfifo(self.shots / "frame.png")
            result = subprocess.run([sys.executable, "-I", "-B", str(Path(TOOL.__file__)), str(self.shots)],
                                    capture_output=True, text=True, timeout=5, check=False)
            self.assertEqual(result.returncode, 2)
            alias = self.root / "redirected"
            alias.symlink_to(self.shots, target_is_directory=True)
            with self.assertRaises(OSError):
                TOOL.read_regular(alias / "frame.png", 1024)
        else:
            # No symlink privilege is required on Windows to exercise read refusal.
            with mock.patch.object(TOOL, "read_regular", side_effect=OSError("reparse refused")):
                self.assertEqual(self.run_cli()[0], 2)


if __name__ == "__main__":
    unittest.main()
