#!/usr/bin/env python3
"""Offline SBOM regressions; Git operations are confined to temporary fixtures."""

from __future__ import annotations

import copy
import datetime as dt
import hashlib
import io
import json
import os
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import generate_sbom as generator
import validate_sbom as validator


CREATED = "2026-09-07T12:34:56Z"
SCRIPT = Path(generator.__file__).resolve()


def fixture_environment() -> dict[str, str]:
    # Parent Git overrides must never redirect fixture commits/index writes.
    return {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}


def fixture_document() -> dict:
    inventory = {name: generator.checksums(io.BytesIO(data))
                 for name, data in {"pkg/a.txt": b"same", "pkg/b.txt": b"same", "pkg/c.bin": b"\0\xff"}.items()}
    return generator.make_document(inventory, "luminumbra-source", "1.2.3", CREATED, "Test timestamp.")


def write_archive(path: Path, entries: list[tuple[str, bytes | None]], member_type: bytes | None = None) -> None:
    with tarfile.open(path, "w:gz") as archive:
        for name, data in entries:
            member = tarfile.TarInfo(name)
            member.type = member_type or (tarfile.DIRTYPE if data is None else tarfile.REGTYPE)
            member.size = len(data) if data is not None and member.isfile() else 0
            member.linkname = "elsewhere" if member.issym() or member.islnk() else ""
            archive.addfile(member, io.BytesIO(data) if data is not None and member.isfile() else None)


class DocumentTests(unittest.TestCase):
    def test_streaming_hashes_cover_all_file_bytes(self):
        data = bytes(range(256)) * 10000
        self.assertEqual(generator.checksums(io.BytesIO(data)), [
            {"algorithm": "SHA1", "checksumValue": hashlib.sha1(data).hexdigest()},
            {"algorithm": "SHA256", "checksumValue": hashlib.sha256(data).hexdigest()}])

    def test_required_fields_and_independent_hashes(self):
        document = fixture_document()
        self.assertEqual(document["creationInfo"]["created"], CREATED)
        file_sha1s = []
        payloads = {"./pkg/a.txt": b"same", "./pkg/b.txt": b"same", "./pkg/c.bin": b"\0\xff"}
        for item in document["files"]:
            expected = payloads[item["fileName"]]
            digests = {entry["algorithm"]: entry["checksumValue"] for entry in item["checksums"]}
            self.assertEqual(digests, {"SHA1": hashlib.sha1(expected).hexdigest(),
                                       "SHA256": hashlib.sha256(expected).hexdigest()})
            file_sha1s.append(digests["SHA1"])
            self.assertEqual(item["licenseInfoInFiles"], ["NOASSERTION"])
        expected = hashlib.sha1("".join(sorted(file_sha1s)).encode("ascii")).hexdigest()
        self.assertEqual(document["packages"][0]["packageVerificationCode"],
                         {"packageVerificationCodeValue": expected})
        self.assertNotEqual(expected, hashlib.sha1("".join(sorted(set(file_sha1s))).encode("ascii")).hexdigest())
        validator.validate_semantics(document)

    def test_namespace_changes_with_content_profile_metadata_and_time(self):
        original = fixture_document()
        for field in ("bytes", "name", "version", "created", "creator", "comment", "path"):
            changed = copy.deepcopy(original)
            if field == "bytes":
                changed["files"][0]["checksums"][1]["checksumValue"] = "0" * 64
            elif field in ("name", "version"):
                changed["packages"][0]["name" if field == "name" else "versionInfo"] += "-linux"
            elif field == "created":
                changed["creationInfo"]["created"] = "2026-09-08T12:34:56Z"
            elif field == "creator":
                changed["creationInfo"]["creators"] = ["Tool: updated-generator"]
            elif field == "comment":
                changed["comment"] += " Updated metadata."
            else:
                changed["files"][0]["fileName"] += ".renamed"
            with self.subTest(field=field):
                self.assertNotEqual(generator.document_namespace(changed), original["documentNamespace"])
        body = {key: value for key, value in original.items() if key != "documentNamespace"}
        independent_hash = hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":"),
                                                    ensure_ascii=True).encode("ascii")).hexdigest()
        self.assertTrue(original["documentNamespace"].endswith("/" + independent_hash))
        inventory = {"a": generator.checksums(io.BytesIO(b"same"))}
        profiles = [generator.make_document(inventory, name, "1.2.3", CREATED, "Same timestamp.")
                    for name in ("luminumbra-source", "luminumbra-linux", "luminumbra-windows")]
        self.assertEqual(len({item["documentNamespace"] for item in profiles}), 3)

    def test_created_validation_and_epoch_convention(self):
        for bad in ("2026-9-07T12:34:56Z", "2026-02-30T12:34:56Z", "2026-09-07T12:34:56+00:00",
                    "2026-09-07T12:34:56.000Z", "2026-09-07T24:34:56Z", ""):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                generator.validate_created(bad)
        with patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "0"}):
            created, comment = generator.creation_time(None)
            self.assertEqual(created, "1970-01-01T00:00:00Z")
            self.assertIn("committer timestamp", comment)
            self.assertEqual(generator.creation_time(CREATED)[0], CREATED)
        for bad in ("-1", "1.5", "", "not-a-date", "9" * 100):
            with self.subTest(bad=bad), patch.dict(os.environ, {"SOURCE_DATE_EPOCH": bad}):
                with self.assertRaises((ValueError, OverflowError, OSError)):
                    generator.creation_time(None)
        with patch.dict(os.environ, {}, clear=True):
            before = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
            created, comment = generator.creation_time(None)
            after = dt.datetime.now(dt.timezone.utc)
            parsed = dt.datetime.fromisoformat(created.replace("Z", "+00:00"))
            self.assertLessEqual(before, parsed)
            self.assertLessEqual(parsed, after)
            self.assertIn("wall clock", comment)

    def test_semantics_reject_original_defects_and_corruptions(self):
        mutations = {
            "missing created": lambda d: d["creationInfo"].pop("created"),
            "missing SHA1": lambda d: d["files"][0]["checksums"].pop(0),
            "missing package code": lambda d: d["packages"][0].pop("packageVerificationCode"),
            "wrong package code": lambda d: d["packages"][0]["packageVerificationCode"].update(packageVerificationCodeValue="0" * 40),
            "duplicate ID": lambda d: d["files"][0].update(SPDXID=d["files"][1]["SPDXID"]),
            "duplicate path": lambda d: d["files"][0].update(fileName=d["files"][1]["fileName"]),
            "dangling relationship": lambda d: d["relationships"][1].update(relatedSpdxElement="SPDXRef-Missing"),
            "missing describes": lambda d: d["relationships"].pop(0),
            "missing contains": lambda d: d["relationships"].pop(),
            "duplicate relationship": lambda d: d["relationships"].append(d["relationships"][0]),
            "invalid digest": lambda d: d["files"][0]["checksums"][0].update(checksumValue="G" * 40),
            "duplicate digest algorithm": lambda d: d["files"][0]["checksums"].append(d["files"][0]["checksums"][0]),
            "excluded file": lambda d: d["packages"][0]["packageVerificationCode"].update(packageVerificationCodeExcludedFiles=["pkg/a.txt"]),
            "old namespace": lambda d: d.update(documentNamespace="https://github.com/d-addison/luminumbra/sbom/1.2.3"),
            "path traversal": lambda d: d["files"][0].update(fileName="./../a.txt"),
        }
        for name, mutation in mutations.items():
            document = fixture_document()
            mutation(document)
            # Avoid a stale namespace masking the intended semantic defect.
            if name != "old namespace":
                document["documentNamespace"] = generator.document_namespace(document)
            with self.subTest(name=name), self.assertRaises((ValueError, KeyError)):
                validator.validate_semantics(document)

    def test_optional_license_info_omission_means_noassertion(self):
        document = fixture_document()
        for item in document["files"]:
            item.pop("licenseInfoInFiles")
        document["packages"][0].pop("licenseInfoFromFiles")
        document["documentNamespace"] = generator.document_namespace(document)
        validator.validate_semantics(document)

    def test_duplicate_json_keys_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            json.loads('{"name": "first", "name": "second"}', object_pairs_hook=validator.no_duplicate_keys)


class SbomFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        # Windows temp paths may use an 8.3 alias; match root_inventory canonicalization.
        self.base = Path(self.temp.name).resolve()
        self.root = self.base / "stage"
        self.root.mkdir()
        (self.root / "a.txt").write_bytes(b"hello\r\n")
        self.output = self.base / "inventory.json"

    def cli(self, *args, success=True):
        env = dict(fixture_environment(), SOURCE_DATE_EPOCH="1788784496")
        result = subprocess.run([sys.executable, str(SCRIPT), "--name", "luminumbra-source",
                                 "--version", "1.2.3", "--output", str(self.output), *map(str, args)],
                                capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0 if success else 1, result.stderr)
        return result


class InputTests(SbomFixture):
    def test_cli_reproducibility_and_actual_staged_bytes(self):
        self.cli("--root", self.root)
        first = self.output.read_bytes()
        self.assertTrue(first.endswith(b"\n"))
        self.assertNotIn(b"\r\n", first)
        self.cli("--root", self.root)
        self.assertEqual(self.output.read_bytes(), first)
        document = json.loads(first)
        self.assertEqual(document["files"][0]["checksums"][1]["checksumValue"],
                         hashlib.sha256(b"hello\r\n").hexdigest())
        validator.validate_semantics(document)

    def test_output_inside_stage_rejected_before_creation_and_on_rerun(self):
        self.output = self.root / "sbom.json"
        self.cli("--root", self.root, success=False)
        self.assertFalse(self.output.exists())
        self.output.write_bytes(b"old sidecar")
        self.cli("--root", self.root, success=False)
        self.assertEqual(self.output.read_bytes(), b"old sidecar")

    def test_archive_output_overwrite_rejected(self):
        archive = self.base / "source.tar.gz"
        write_archive(archive, [("pkg/a", b"hello")])
        original = archive.read_bytes()
        self.output = archive
        self.cli("--archive", archive, success=False)
        self.assertEqual(archive.read_bytes(), original)

    def test_missing_root_invalid_created_and_incompatible_options(self):
        self.cli("--root", self.base / "missing", success=False)
        self.cli("--root", self.root, "--created", "bad", success=False)
        self.cli("--archive", self.base / "missing.tar", "--tracked", success=False)

    def test_portable_path_validation(self):
        for bad in ("", "/absolute", "../outside", "a/../b", "a/./b", "a//b", "a\\b", "C:/file", "a\nb", "bad\udcff"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                generator.valid_path(bad)
        self.assertEqual(generator.valid_path("directory/a file-雪.txt"), "directory/a file-雪.txt")

    def test_symlinks_and_special_entries_rejected_without_following(self):
        # Mock metadata so this regression also runs on Windows without symlink privileges.
        original = Path.lstat
        for mode in (0o120777, 0o010600):
            intercepted = []

            def fake_lstat(path, *args, **kwargs):
                if path == self.root / "a.txt":
                    intercepted.append(path)
                    info = list(original(path, *args, **kwargs))
                    info[0] = mode
                    return os.stat_result(info)
                return original(path, *args, **kwargs)
            with self.subTest(mode=mode), patch.object(Path, "lstat", fake_lstat):
                with self.assertRaisesRegex(ValueError, "unsupported package entry"):
                    generator.root_inventory(self.root, False, self.output)
                self.assertEqual(intercepted, [self.root / "a.txt"])

    def test_link_detection_with_optional_windows_attributes(self):
        # A synthetic Windows os.stat_result can expose attributes as None.
        for attributes in ({}, {"st_file_attributes": None}, {"st_file_attributes": 0}):
            for mode in (stat.S_IFREG | 0o600, stat.S_IFIFO | 0o600):
                with self.subTest(attributes=attributes, mode=mode):
                    self.assertFalse(generator.is_link(SimpleNamespace(st_mode=mode, **attributes)))
            self.assertTrue(generator.is_link(SimpleNamespace(st_mode=stat.S_IFLNK, **attributes)))
        for mode in (stat.S_IFREG, stat.S_IFDIR):
            self.assertTrue(generator.is_link(SimpleNamespace(
                st_mode=mode, st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT)))

    def test_windows_junction_attributes_rejected_before_traversal(self):
        junction = self.root / "junction"
        junction.mkdir()
        (junction / "outside.txt").write_bytes(b"must not be inventoried")
        original = Path.lstat
        intercepted = []

        def junction_lstat(path, *args, **kwargs):
            info = original(path, *args, **kwargs)
            if path == junction:
                intercepted.append(path)
                return SimpleNamespace(st_mode=info.st_mode,
                                       st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT)
            return info

        # Portable simulation of Windows lstat metadata; no junction privilege needed.
        with patch.object(Path, "lstat", junction_lstat):
            with self.assertRaisesRegex(ValueError, "unsupported package entry.*junction"):
                generator.root_inventory(self.root, False, self.output)
        self.assertEqual(intercepted, [junction])


class ArchiveTests(SbomFixture):
    def test_exact_archive_inventory_and_drift(self):
        archive = self.base / "source.tar.gz"
        entries = [("pkg/", None), ("pkg/b.txt", b"same"), ("pkg/a.txt", b"same"), ("pkg/c.bin", b"\0\xff")]
        write_archive(archive, entries)
        self.cli("--archive", archive)
        document = json.loads(self.output.read_text())
        # Independent tar enumeration and hashlib calls compare exact archive bytes.
        with tarfile.open(archive) as stream:
            expected = {"./" + member.name: stream.extractfile(member).read()
                        for member in stream if member.isfile()}
        self.assertEqual({item["fileName"] for item in document["files"]}, set(expected))
        for item in document["files"]:
            self.assertEqual(item["checksums"], [
                {"algorithm": "SHA1", "checksumValue": hashlib.sha1(expected[item["fileName"]]).hexdigest()},
                {"algorithm": "SHA256", "checksumValue": hashlib.sha256(expected[item["fileName"]]).hexdigest()}])
        validator.validate_semantics(document)
        validator.validate_archive(document, archive)
        for drift in ([*entries[:-1], ("pkg/c.bin", b"changed")], entries[:-1], [*entries, ("pkg/new", b"new")]):
            write_archive(archive, drift)
            with self.assertRaisesRegex(ValueError, "archive/SBOM"):
                validator.validate_archive(document, archive)

    def test_archive_rejects_unsupported_members_and_aliases(self):
        archive = self.base / "bad.tar.gz"
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE):
            write_archive(archive, [("pkg/file", b"data")], kind)
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "unsupported archive entry"):
                generator.archive_inventory(archive)
        for name in ("../escape", "/absolute", "pkg/../file", "pkg\\file", "pkg//file"):
            write_archive(archive, [(name, b"data")])
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "unsupported package path"):
                generator.archive_inventory(archive)
        write_archive(archive, [("pkg/a", b"one"), ("pkg/a", b"two")])
        with self.assertRaisesRegex(ValueError, "duplicate archive path"):
            generator.archive_inventory(archive)
        for entries in ([('pkg/a', b'file'), ('pkg/a/b', b'child')],
                        [('pkg/a/b', b'child'), ('pkg/a', b'file')]):
            write_archive(archive, entries)
            with self.assertRaisesRegex(ValueError, "file/directory archive path collision"):
                generator.archive_inventory(archive)

    def test_empty_archive_verification_code(self):
        archive = self.base / "empty.tar.gz"
        write_archive(archive, [])
        self.cli("--archive", archive)
        document = json.loads(self.output.read_text())
        self.assertEqual(document["files"], [])
        self.assertEqual(document["packages"][0]["packageVerificationCode"]["packageVerificationCodeValue"],
                         hashlib.sha1(b"").hexdigest())
        validator.validate_semantics(document)
        validator.validate_archive(document, archive)


class GitArchiveTests(SbomFixture):
    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), "-c", "core.autocrlf=false",
                                        "-c", "core.eol=lf",
                                        "-c", "core.hooksPath=" + str(self.base / "no-hooks"),
                                        "-c", "commit.gpgsign=false", "-c", "user.name=SBOM Test",
                                        "-c", "user.email=sbom-test@example.invalid", *args],
                                       stderr=subprocess.PIPE, env=fixture_environment())

    def setUp(self):
        super().setUp()
        self.git("init", "-q")
        (self.root / ".gitattributes").write_bytes(b"* text=auto\nexcluded.txt export-ignore\nsubstitute.txt export-subst\n")
        (self.root / "excluded.txt").write_bytes(b"not in archive\n")
        (self.root / "substitute.txt").write_bytes(b"$Format:%H$\n")
        self.git("add", ".")
        self.git("commit", "-qm", "Create archive fixture")

    def test_git_archive_normalization_and_export_attributes(self):
        archive = self.base / "source.tar"
        archive.write_bytes(self.git("archive", "--format=tar", "--prefix=pkg/", "HEAD"))
        self.assertEqual((self.root / "a.txt").read_bytes(), b"hello\r\n")
        self.cli("--archive", archive)
        document = json.loads(self.output.read_text())
        files = {item["fileName"]: item for item in document["files"]}
        self.assertNotIn("./pkg/excluded.txt", files)
        self.assertEqual(files["./pkg/a.txt"]["checksums"][1]["checksumValue"],
                         hashlib.sha256(b"hello\n").hexdigest())
        commit = self.git("rev-parse", "HEAD").strip() + b"\n"
        self.assertEqual(files["./pkg/substitute.txt"]["checksums"][1]["checksumValue"],
                         hashlib.sha256(commit).hexdigest())
        validator.validate_archive(document, archive)

    def test_git_archive_crlf_bytes_match_inventory(self):
        # Archive applies checkout conversion: Windows native EOL can differ from the blob.
        archive = self.base / "crlf-source.tar"
        archive.write_bytes(self.git("-c", "core.eol=crlf", "archive", "--format=tar",
                                     "--prefix=pkg/", "HEAD"))
        self.assertEqual(self.git("show", "HEAD:a.txt"), b"hello\n")
        with tarfile.open(archive) as stream:
            self.assertEqual(stream.extractfile("pkg/a.txt").read(), b"hello\r\n")
        self.cli("--archive", archive)
        document = json.loads(self.output.read_text())
        files = {item["fileName"]: item for item in document["files"]}
        self.assertEqual(files["./pkg/a.txt"]["checksums"], [
            {"algorithm": "SHA1", "checksumValue": hashlib.sha1(b"hello\r\n").hexdigest()},
            {"algorithm": "SHA256", "checksumValue": hashlib.sha256(b"hello\r\n").hexdigest()}])
        validator.validate_archive(document, archive)

    def test_tracked_sidecar_reproducibility_and_self_inclusion(self):
        self.output = self.root / "sidecar.json"
        self.cli("--root", self.root, "--tracked")
        first = self.output.read_bytes()
        self.cli("--root", self.root, "--tracked")
        self.assertEqual(first, self.output.read_bytes())
        self.git("add", "sidecar.json")
        self.cli("--root", self.root, "--tracked", success=False)
        self.assertEqual(first, self.output.read_bytes())

    def test_tracked_gitlink_and_link_modes_rejected(self):
        commit = self.git("rev-parse", "HEAD").strip().decode("ascii")
        self.git("update-index", "--add", "--cacheinfo", f"160000,{commit},submodule")
        with self.assertRaisesRegex(ValueError, "unsupported tracked entry"):
            generator.files_for(self.root.resolve(), True)
        self.git("update-index", "--force-remove", "submodule")
        blob = self.git("rev-parse", "HEAD:a.txt").strip().decode("ascii")
        self.git("update-index", "--add", "--cacheinfo", f"120000,{blob},link")
        with self.assertRaisesRegex(ValueError, "unsupported tracked entry"):
            generator.files_for(self.root.resolve(), True)


if __name__ == "__main__":
    unittest.main()
