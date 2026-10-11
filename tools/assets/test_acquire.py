import gzip
import hashlib
import io
import os
from pathlib import Path
import stat
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

import acquire


def identity(data):
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


class AssetAcquisition(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / "cache"
        self.files = {"data/tree.lmesh": b"verified mesh", "data/leaf.ltex": b"verified texture"}
        self.pack, self.archive = self.make_archive(self.files)

    def make_archive(self, members, *, link=None):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            for name, data in members.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
            if link:
                info = tarfile.TarInfo(link)
                info.type = tarfile.SYMTYPE
                info.linkname = "../../outside"
                archive.addfile(info)
        expanded = buffer.getvalue()
        compressed = gzip.compress(expanded, mtime=0)
        path = self.root / (hashlib.sha256(compressed).hexdigest() + ".tar.gz")
        path.write_bytes(compressed)
        return {"version": "1.0.0", "install_dir": "game-assets/tree/1.0.0",
                "archive": {**identity(compressed), "uncompressed_size": len(expanded),
                            "url": "https://example.invalid/tree.tar.gz"},
                "files": [{"path": name, **identity(data)} for name, data in self.files.items()]}, path

    def install(self, **kwargs):
        return acquire.install(self.pack, self.root, self.cache, **kwargs)

    def test_clean_install_and_offline_reuse_do_not_download(self):
        target = self.install(local=self.archive)
        with mock.patch.object(acquire.urllib.request, "urlopen", side_effect=AssertionError("network")):
            self.assertEqual(target, self.install(offline=True))
            second = acquire.install(self.pack, self.root / "second", self.cache, offline=True)
        self.assertEqual((second / "data/tree.lmesh").read_bytes(), self.files["data/tree.lmesh"])

    def test_corrupt_archive_never_installs_or_enters_cache(self):
        self.archive.write_bytes(b"broken")
        with self.assertRaises(ValueError):
            self.install(local=self.archive)
        self.assertFalse((self.root / self.pack["install_dir"]).exists())
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_corrupt_cached_archive_is_not_offline_reuse(self):
        target = self.install(local=self.archive)
        cache = self.cache / (self.pack["archive"]["sha256"] + ".tar.gz")
        cache.write_bytes(b"broken")
        with self.assertRaisesRegex(ValueError, "offline"):
            acquire.install(self.pack, self.root / "second", self.cache, offline=True)
        acquire.verify_install(target, acquire.validate_pack(self.pack))

    def test_extra_missing_and_link_members_are_refused(self):
        variants = [(dict(self.files, extra=b"extra"), None),
                    ({"data/tree.lmesh": self.files["data/tree.lmesh"]}, None),
                    (self.files, "link")]
        for members, link in variants:
            with self.subTest(members=members, link=link):
                self.pack, self.archive = self.make_archive(members, link=link)
                with self.assertRaises(ValueError):
                    self.install(local=self.archive)
                self.assertFalse((self.root / self.pack["install_dir"]).exists())

    def test_file_hash_mismatch_refused_even_when_archive_hash_matches(self):
        self.pack, self.archive = self.make_archive(dict(self.files, **{"data/tree.lmesh": b"tampered mesh"}))
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            self.install(local=self.archive)

    def test_expansion_bound_is_enforced(self):
        self.pack["archive"]["uncompressed_size"] = 128
        with self.assertRaisesRegex(ValueError, "Decompressed archive"):
            self.install(local=self.archive)

    def test_unsafe_manifest_paths_are_refused(self):
        for path in ["../out", "/out", "C:/out", "data\\out", "data/../out", "data//out", "./out",
                     "data/CON.txt", "data/leaf.", "data/leaf "]:
            with self.subTest(path=path), self.assertRaises(ValueError):
                acquire.relative_path(path)

    def test_redirected_install_ancestor_is_refused(self):
        outside = self.root / "outside"
        outside.mkdir()
        link = self.root / "game-assets"
        if os.name == "nt":
            # Junctions need no developer mode or elevated symlink privilege.
            subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/j", str(link), str(outside)],
                           check=True, capture_output=True)
        else:
            link.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symbolic link"):
            self.install(local=self.archive)
        self.assertEqual(list(outside.iterdir()), [])

    def test_reparse_attribute_is_refused_even_without_symlink_mode(self):
        info = mock.Mock(st_mode=stat.S_IFDIR, st_file_attributes=0x400)
        with mock.patch.object(Path, "lstat", return_value=info):
            with self.assertRaisesRegex(ValueError, "reparse point"):
                acquire.checked_path(self.root, self.pack["install_dir"])

    def test_corrupt_install_requires_explicit_repair_and_preserves_old_bytes(self):
        target = self.install(local=self.archive)
        (target / "data/tree.lmesh").write_bytes(b"old corruption")
        with self.assertRaisesRegex(ValueError, "--repair"):
            self.install(offline=True)
        self.install(offline=True, repair=True)
        preserved = list(target.parent.glob("1.0.0.corrupt-*"))
        self.assertEqual(len(preserved), 1)
        self.assertEqual((preserved[0] / "data/tree.lmesh").read_bytes(), b"old corruption")
        acquire.verify_install(target, acquire.validate_pack(self.pack))

    def test_interrupted_install_preserves_existing_bytes(self):
        target = self.install(local=self.archive)
        (target / "data/tree.lmesh").write_bytes(b"old corruption")
        replace = acquire.os.replace

        def interrupted(source, destination):
            if Path(source).name.startswith(".install-"):
                raise OSError("simulated installation interruption")
            return replace(source, destination)

        with mock.patch.object(acquire.os, "replace", side_effect=interrupted):
            with self.assertRaisesRegex(OSError, "interruption"):
                self.install(offline=True, repair=True)
        self.assertEqual((target / "data/tree.lmesh").read_bytes(), b"old corruption")
        self.assertEqual(list(target.parent.glob(".install-*")), [])

    def test_interrupted_download_cleans_partial_cache(self):
        stream = mock.MagicMock()
        stream.__enter__.return_value = stream
        stream.geturl.return_value = self.pack["archive"]["url"]
        stream.read.side_effect = [b"partial", OSError("connection lost")]
        with mock.patch.object(acquire.urllib.request, "urlopen", return_value=stream):
            with self.assertRaisesRegex(OSError, "connection lost"):
                self.install()
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_transfer_has_a_total_deadline(self):
        with mock.patch.object(acquire.time, "monotonic", side_effect=[0, 121]):
            with self.assertRaisesRegex(ValueError, "deadline"):
                self.install(local=self.archive)
        self.assertEqual(list(self.cache.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
