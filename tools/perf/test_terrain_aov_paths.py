"""Stdlib-only containment checks before AOV numeric processing."""
import importlib.util
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock

from test_fixture_hash import symlink_or_skip


class TerrainAovPathTests(unittest.TestCase):
    def test_manifest_and_every_attachment_refuse_external_symlinks_before_read(self):
        spec = importlib.util.spec_from_file_location(
            "aov_paths_under_test", Path(__file__).with_name("inspect_terrain_aovs.py"))
        aovs = importlib.util.module_from_spec(spec)
        # These refusals must precede all numeric processing, even where numpy
        # is unavailable. This stub provides no numeric operations.
        with mock.patch.dict("sys.modules", {"numpy": types.ModuleType("numpy")}):
            spec.loader.exec_module(aovs)
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            root = parent / "capture"
            root.mkdir()
            outside = parent / "outside"
            outside.write_bytes(b"must not be read")
            for name in ("manifest.json", "color.ppm", "depth.pfm", "position.pfm",
                         "normal.pfm", "albedo.pfm", "material.pgm"):
                with self.subTest(name=name):
                    link = root / name
                    symlink_or_skip(self, link, outside)
                    try:
                        with mock.patch.object(Path, "read_text", side_effect=AssertionError("read")), \
                                mock.patch.object(aovs, "plane", side_effect=AssertionError("plane")), \
                                self.assertRaisesRegex(ValueError, "resolves outside root"):
                            aovs.inspect(root)
                    finally:
                        link.unlink()


if __name__ == "__main__":
    unittest.main()
