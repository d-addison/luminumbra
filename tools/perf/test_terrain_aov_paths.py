"""Stdlib-only containment checks before AOV numeric processing."""
import importlib.util
import os
from pathlib import Path
import stat
import tempfile
import types
import unittest
from unittest import mock

from test_fixture_hash import symlink_or_skip


class TerrainAovPathTests(unittest.TestCase):
    def load_inspector(self):
        spec = importlib.util.spec_from_file_location(
            "aov_paths_under_test", Path(__file__).with_name("inspect_terrain_aovs.py"))
        aovs = importlib.util.module_from_spec(spec)
        # These refusals must precede all numeric processing, even where numpy
        # is unavailable. This stub provides no numeric operations.
        with mock.patch.dict("sys.modules", {"numpy": types.ModuleType("numpy")}):
            spec.loader.exec_module(aovs)
        return aovs

    def test_manifest_and_every_attachment_refuse_external_symlinks_before_read(self):
        aovs = self.load_inspector()
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

    def test_every_manifest_and_attachment_requires_regular_files_before_read(self):
        aovs = self.load_inspector()
        names = ('manifest.json', 'color.ppm', 'depth.pfm', 'position.pfm',
                 'normal.pfm', 'albedo.pfm', 'material.pgm')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in names:
                (root / name).write_text('{}')
            original_stat = Path.stat
            for name in names:
                for mode in (stat.S_IFIFO, stat.S_IFSOCK, stat.S_IFCHR, stat.S_IFBLK, stat.S_IFDIR):
                    def status(path, *args, **kwargs):
                        if path == root / name:
                            return os.stat_result((mode,) + (0,) * 9)
                        return original_stat(path, *args, **kwargs)

                    with self.subTest(name=name, mode=mode), \
                            mock.patch.object(Path, 'stat', status), \
                            mock.patch.object(Path, 'read_text', side_effect=AssertionError('unchecked read')), \
                            mock.patch.object(aovs, 'plane', side_effect=AssertionError('numeric processing')), \
                            self.assertRaisesRegex(ValueError, 'not a regular file'):
                        aovs.inspect(root)

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'OS has no mkfifo')
    def test_fifo_manifest_and_attachments_are_refused_without_reading(self):
        aovs = self.load_inspector()
        names = ('manifest.json', 'color.ppm', 'depth.pfm', 'position.pfm',
                 'normal.pfm', 'albedo.pfm', 'material.pgm')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in names:
                (root / name).write_text('{}')
            for name in names:
                path = root / name
                path.unlink()
                os.mkfifo(path)
                try:
                    with self.subTest(name=name), \
                            mock.patch.object(Path, 'read_text', side_effect=AssertionError('unchecked read')), \
                            self.assertRaisesRegex(ValueError, 'not a regular file'):
                        aovs.inspect(root)
                finally:
                    path.unlink()
                    path.write_text('{}')


if __name__ == "__main__":
    unittest.main()
