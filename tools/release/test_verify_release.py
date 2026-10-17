#!/usr/bin/env python3
"""Offline regressions for verify_release.py using a fake Runner; no network or gh calls."""

from __future__ import annotations

import contextlib
import copy
import errno
import gzip
import hashlib
import io
import json
import os
import stat
import tarfile
import tempfile
import unittest
import sys
from pathlib import Path
from unittest import mock
from typing import Dict, List, Optional, Sequence, Tuple

# Import from this directory even under `python3 -I`, which drops the script directory from sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_release as verifier  # noqa: E402


REPO = "example-owner/example-repo"
TAG = "v9.9.9"
VERSION = "9.9.9"
COMMIT = "0123456789abcdef0123456789abcdef01234567"
TAG_OBJECT = "fedcba9876543210fedcba9876543210fedcba98"
ARCHIVE = f"luminumbra-{VERSION}-source.tar.gz"
SPDX = f"luminumbra-{VERSION}-source.spdx.json"
SUMS = "SHA256SUMS"
SIGNER = f"https://github.com/{REPO}/.github/workflows/release.yml@refs/tags/{TAG}"
SLSA_PRED = verifier.SLSA_PREDICATE
SIGNER_CHECKS = ("attestation-provenance-archive", "attestation-provenance-spdx",
                 "attestation-provenance-sha256sums", "attestation-sbom")


def certificate(signer: str = SIGNER, source: str = COMMIT, build: str = COMMIT) -> dict:
    """Certificate fields as gh reports them; the digests default to the expected commit."""
    return {"buildSignerURI": signer, "sourceRepositoryDigest": source, "buildSignerDigest": build}


def attestation_json(predicate_type: str, predicate: object, cert: Optional[dict] = None) -> str:
    return json.dumps([{
        "verificationResult": {
            "signature": {"certificate": certificate() if cert is None else cert},
            "statement": {"predicateType": predicate_type, "predicate": predicate},
        }
    }])


def source_tar() -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w") as archive:
        body = b"project(Luminumbra)\n"
        info = tarfile.TarInfo(f"luminumbra-{VERSION}/CMakeLists.txt")
        info.size = len(body)
        archive.addfile(info, io.BytesIO(body))
    return buffer.getvalue()


def gzip_n(data: bytes) -> bytes:
    """Fake for `gzip -n`: no name, mtime 0. Byte-equal to GNU gzip only by accident; the
    fake runner uses it for both the downloaded archive and the regeneration, so they agree."""
    buffer = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buffer, mtime=0, compresslevel=6) as gz:
        gz.write(data)
    return buffer.getvalue()


def build_scenario() -> Dict[str, object]:
    tar = source_tar()
    archive = gzip_n(tar)
    spdx_doc = {"spdxVersion": "SPDX-2.3", "name": f"luminumbra-source-{VERSION}"}
    spdx = (json.dumps(spdx_doc, indent=2, sort_keys=True) + "\n").encode("utf-8")
    sums = (f"{hashlib.sha256(archive).hexdigest()}  {ARCHIVE}\n"
            f"{hashlib.sha256(spdx).hexdigest()}  {SPDX}\n").encode("utf-8")

    def attestation(predicate_type: str, predicate: object) -> str:
        return attestation_json(predicate_type, predicate)

    return {
        "tar": tar,
        "ref": {"ref": f"refs/tags/{TAG}", "object": {"type": "tag", "sha": TAG_OBJECT}},
        "tag": {"sha": TAG_OBJECT, "object": {"type": "commit", "sha": COMMIT},
                "verification": {"verified": True, "reason": "valid"}},
        "release": {"draft": False, "prerelease": False,
                    "assets": [{"name": ARCHIVE}, {"name": SPDX}, {"name": SUMS}]},
        "assets": {ARCHIVE: archive, SPDX: spdx, SUMS: sums},
        "git_rc": 0,
        "validators": {},
        "attest": {
            (ARCHIVE, verifier.SLSA_PREDICATE): (0, attestation(verifier.SLSA_PREDICATE, {})),
            (SPDX, verifier.SLSA_PREDICATE): (0, attestation(verifier.SLSA_PREDICATE, {})),
            (SUMS, verifier.SLSA_PREDICATE): (0, attestation(verifier.SLSA_PREDICATE, {})),
            (ARCHIVE, verifier.SPDX_PREDICATE): (0, attestation(verifier.SPDX_PREDICATE, spdx_doc)),
        },
    }


class FakeRunner(verifier.Runner):
    """Answers the commands verify_release issues from a scenario dictionary."""

    def __init__(self, scenario: Dict[str, object]) -> None:
        self.s = scenario
        self.calls: List[List[str]] = []

    def run_bytes(self, argv: Sequence[str], cwd: Optional[Path] = None) -> Tuple[int, bytes, str]:
        argv = list(argv)
        self.calls.append(argv)
        if argv[:3] == ["gzip", "-n", "-c"] and len(argv) == 4:
            if self.s.get("gzip_rc", 0) != 0:
                return self.s["gzip_rc"], b"", "gzip failed"
            return 0, gzip_n(Path(argv[3]).read_bytes()), ""
        return 1, b"", f"unexpected binary command: {argv}"

    def run(self, argv: Sequence[str], cwd: Optional[Path] = None) -> Tuple[int, str, str]:
        argv = list(argv)
        self.calls.append(argv)
        if argv[0] == "git":
            if argv[1] != "archive" or argv[-1] != COMMIT:
                return 1, "", f"git archive must target the expected commit: {argv}"
            output = [a for a in argv if a.startswith("--output=")]
            if len(output) != 1:
                return 1, "", "missing --output="
            Path(output[0].split("=", 1)[1]).write_bytes(self.s["tar"])
            return self.s["git_rc"], "", "" if self.s["git_rc"] == 0 else "git failed"
        if argv[:2] == ["gh", "api"]:
            path = argv[2]
            if "/git/ref/tags/" in path:
                return 0, json.dumps(self.s["ref"]), ""
            if "/git/tags/" in path:
                return 0, json.dumps(self.s["tag"]), ""
            if "/releases/tags/" in path:
                return 0, json.dumps(self.s["release"]), ""
            return 1, "", "unexpected api path"
        if argv[:3] == ["gh", "release", "download"]:
            name = argv[argv.index("--pattern") + 1]
            directory = Path(argv[argv.index("--dir") + 1])
            if name not in self.s["assets"]:
                return 1, "", f"release asset not found: {name}"
            directory.mkdir(parents=True, exist_ok=True)
            (directory / name).write_bytes(self.s["assets"][name])
            return 0, "", ""
        if argv[:3] == ["gh", "attestation", "verify"]:
            key = (Path(argv[3]).name, argv[argv.index("--predicate-type") + 1])
            rc, body = self.s["attest"].get(key, (1, ""))
            return rc, body, "" if rc == 0 else "verification failed"
        if len(argv) > 1 and Path(argv[1]).name in ("validate_package.py", "validate_sbom.py"):
            return self.s["validators"].get(Path(argv[1]).name, (0, "ok", ""))
        return 1, "", f"unexpected command: {argv}"


@contextlib.contextmanager
def report_symlink(link: Path, target: Path):
    """Use native links when possible, otherwise simulate link metadata and existence."""
    with contextlib.ExitStack() as stack:
        try:
            os.symlink(str(target), str(link))
        except (OSError, NotImplementedError) as error:
            if (isinstance(error, OSError)
                    and error.errno not in (errno.EPERM, errno.EACCES, errno.ENOSYS, errno.EOPNOTSUPP)
                    and getattr(error, "winerror", None) not in (1, 50, 1314)):
                raise
            original_lstat = os.lstat
            original_lexists = os.path.lexists
            original_islink = os.path.islink
            def lstat(path, *args, **kwargs):
                if Path(path) == link:
                    return mock.Mock(st_mode=stat.S_IFLNK)
                return original_lstat(path, *args, **kwargs)
            def lexists(path):
                return Path(path) == link or original_lexists(path)
            def islink(path):
                return Path(path) == link or original_islink(path)
            stack.enter_context(mock.patch.object(os, "lstat", new=lstat))
            stack.enter_context(mock.patch.object(Path, "lstat", new=lstat))
            # Windows native existence and link queries can bypass lstat.
            stack.enter_context(mock.patch.object(os.path, "lexists", new=lexists))
            stack.enter_context(mock.patch.object(os.path, "islink", new=islink))
        yield


class VerifyReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scenario = build_scenario()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = Path(self.tmp.name) / "out"

    def run_with(self, scenario: Optional[Dict[str, object]] = None):
        scenario = scenario or self.scenario
        args = verifier.parse_args([
            "--repo", REPO, "--tag", TAG, "--expected-commit", COMMIT,
            "--out", str(self.out), "--source-root", self.tmp.name, "--python", sys.executable,
        ])
        checks = verifier.run_checks(args, FakeRunner(scenario))[0]
        return {c.name: c for c in checks}

    def assertFailed(self, results, name: str) -> None:
        self.assertEqual(results[name].status, "FAIL", results[name].detail)

    def test_pass_when_everything_matches(self) -> None:
        results = self.run_with()
        failing = [c for c in results.values() if c.status != "PASS"]
        self.assertEqual(failing, [], [f"{c.name}: {c.detail}" for c in failing])
        self.assertEqual(len(results), 14)

    def test_wrong_asset_set_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["release"]["assets"].append({"name": "luminumbra-9.9.9-linux.tar.gz"})
        self.assertFailed(self.run_with(scenario), "release-assets")

    def test_draft_release_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["release"]["draft"] = True
        self.assertFailed(self.run_with(scenario), "release-state")

    def test_wrong_commit_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["tag"]["object"]["sha"] = "f" * 40
        self.assertFailed(self.run_with(scenario), "tag-annotated-commit")

    def test_lightweight_tag_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["ref"]["object"] = {"type": "commit", "sha": COMMIT}
        self.assertFailed(self.run_with(scenario), "tag-annotated-commit")

    def test_unverified_tag_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["tag"]["verification"]["verified"] = False
        self.assertFailed(self.run_with(scenario), "tag-github-verified")

    def test_bad_checksum_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["assets"][SUMS] = (f"{'0' * 64}  {ARCHIVE}\n"
                                    f"{hashlib.sha256(scenario['assets'][SPDX]).hexdigest()}  {SPDX}\n"
                                    ).encode("utf-8")
        self.assertFailed(self.run_with(scenario), "sha256sums-manifest")

    def test_checksum_file_listing_extra_entry_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["assets"][SUMS] += b"0" * 64 + b"  luminumbra-9.9.9-extra.tar.gz\n"
        self.assertFailed(self.run_with(scenario), "sha256sums-manifest")

    def test_archive_not_reproducible_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["tar"] = source_tar() + b"\0" * 512
        self.assertFailed(self.run_with(scenario), "archive-regenerated")

    def test_provenance_attestation_failure_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["attest"][(ARCHIVE, verifier.SLSA_PREDICATE)] = (1, "")
        self.assertFailed(self.run_with(scenario), "attestation-provenance-archive")

    def test_wrong_signer_identity_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        wrong = f"https://github.com/{REPO}/.github/workflows/other.yml@refs/tags/{TAG}"
        scenario["attest"][(SPDX, verifier.SLSA_PREDICATE)] = (
            0, json.dumps([{"verificationResult": {
                "signature": {"certificate": {"buildSignerURI": wrong}},
                "statement": {"predicateType": verifier.SLSA_PREDICATE, "predicate": {}}}}]))
        self.assertFailed(self.run_with(scenario), "attestation-provenance-spdx")

    def test_sbom_attestation_failure_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["attest"][(ARCHIVE, verifier.SPDX_PREDICATE)] = (1, "")
        self.assertFailed(self.run_with(scenario), "attestation-sbom")

    def test_sbom_predicate_mismatch_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        tampered = {"spdxVersion": "SPDX-2.3", "name": "tampered"}
        scenario["attest"][(ARCHIVE, verifier.SPDX_PREDICATE)] = (
            0, attestation_json(verifier.SPDX_PREDICATE, tampered))
        results = self.run_with(scenario)
        self.assertEqual(results["attestation-sbom"].status, "PASS")
        self.assertFailed(results, "sbom-predicate-matches-spdx")

    def test_validator_failure_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["validators"]["validate_sbom.py"] = (1, "", "SBOM validation: FAIL - bad")
        self.assertFailed(self.run_with(scenario), "sbom-validation")

    def test_dry_run_plan_executes_nothing(self) -> None:
        args = verifier.parse_args([
            "--repo", REPO, "--tag", TAG, "--expected-commit", COMMIT, "--out", str(self.out),
        ])
        plan = verifier.plan_commands(args)
        self.assertTrue(any("attestation" in cmd for cmd in plan))
        self.assertFalse(self.out.exists())

    def test_archive_regeneration_uses_expected_commit_and_gzip_n(self) -> None:
        runner = FakeRunner(self.scenario)
        args = verifier.parse_args([
            "--repo", REPO, "--tag", TAG, "--expected-commit", COMMIT,
            "--out", str(self.out), "--source-root", self.tmp.name, "--python", sys.executable,
        ])
        checks = {c.name: c for c in verifier.run_checks(args, runner)[0]}
        self.assertEqual(checks["archive-regenerated"].status, "PASS", checks["archive-regenerated"].detail)
        git_calls = [c for c in runner.calls if c[0] == "git"]
        self.assertEqual(len(git_calls), 1)
        self.assertEqual(git_calls[0][-1], COMMIT)
        self.assertIn(["gzip", "-n", "-c", git_calls[0][-2].split("=", 1)[1]], runner.calls)

    def test_regenerated_mismatch_reports_both_hashes(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["tar"] = source_tar() + b"\0" * 512
        results = self.run_with(scenario)
        check = results["archive-regenerated"]
        self.assertEqual(check.status, "FAIL")
        regenerated = hashlib.sha256(gzip_n(scenario["tar"])).hexdigest()
        downloaded = hashlib.sha256(scenario["assets"][ARCHIVE]).hexdigest()
        self.assertIn(f"regenerated sha256={regenerated}", check.detail)
        self.assertIn(f"downloaded sha256={downloaded}", check.detail)
        self.assertIn("gzip build matching", check.detail)

    def test_gzip_failure_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["gzip_rc"] = 2
        self.assertFailed(self.run_with(scenario), "archive-regenerated")

    def test_dry_run_plan_shows_regeneration_commands(self) -> None:
        args = verifier.parse_args([
            "--repo", REPO, "--tag", TAG, "--expected-commit", COMMIT, "--out", str(self.out),
        ])
        plan = [" ".join(cmd).replace("\\", "/") for cmd in verifier.plan_commands(args)]
        self.assertIn(f"git archive --format=tar --prefix=luminumbra-{VERSION}/ "
                      f"--output=<tmp>/source.tar {COMMIT}", plan)
        self.assertIn("gzip -n -c <tmp>/source.tar", plan)

    def test_bad_arguments_rejected(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            verifier.parse_args(["--repo", REPO, "--tag", "0.3.0", "--expected-commit", COMMIT,
                                 "--out", "x"])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            verifier.parse_args(["--repo", REPO, "--tag", TAG, "--expected-commit", "ABC",
                                 "--out", "x"])

    # Helpers for the regressions below.

    def cli_args(self) -> List[str]:
        return ["--repo", REPO, "--tag", TAG, "--expected-commit", COMMIT,
                "--out", str(self.out), "--source-root", self.tmp.name, "--python", sys.executable]

    def archive_attestation(self, body: str) -> Dict[str, object]:
        scenario = copy.deepcopy(self.scenario)
        scenario["attest"][(ARCHIVE, SLSA_PRED)] = (0, body)
        return scenario

    def all_attestations_with(self, cert: dict) -> Dict[str, object]:
        """Every attestation carries `cert`; predicates are kept as in the clean scenario."""
        scenario = copy.deepcopy(self.scenario)
        for key, (rc, body) in list(scenario["attest"].items()):
            predicate_type = key[1]
            predicate = json.loads(body)[0]["verificationResult"]["statement"]["predicate"]
            scenario["attest"][key] = (rc, attestation_json(predicate_type, predicate, cert))
        return scenario

    # Finding 1: exact signer identity.

    def test_signer_with_suffixed_tag_rejected(self) -> None:
        # v9.9.90 contains v9.9.9 as a prefix, so a substring search would accept it.
        suffixed = SIGNER + "0"
        self.assertTrue(suffixed.endswith("release.yml@refs/tags/v9.9.90"))
        scenario = self.archive_attestation(attestation_json(SLSA_PRED, {}, certificate(signer=suffixed)))
        self.assertFailed(self.run_with(scenario), "attestation-provenance-archive")

    def test_signer_with_prefix_or_suffix_rejected(self) -> None:
        for signer in ("https://evil.example/" + SIGNER, SIGNER + "/../x", " " + SIGNER):
            with self.subTest(signer=signer):
                scenario = self.archive_attestation(
                    attestation_json(SLSA_PRED, {}, certificate(signer=signer)))
                self.assertFailed(self.run_with(scenario), "attestation-provenance-archive")

    def test_misleading_unrelated_field_rejected(self) -> None:
        cert = certificate(signer=f"https://github.com/{REPO}/.github/workflows/other.yml@refs/tags/{TAG}")
        cert["sourceRepositoryRef"] = SIGNER
        cert["githubWorkflowRef"] = SIGNER
        cert["workflowRef"] = f"refs/tags/{TAG}"
        scenario = self.archive_attestation(attestation_json(SLSA_PRED, {}, cert))
        self.assertFailed(self.run_with(scenario), "attestation-provenance-archive")

    def test_signer_field_missing_rejected(self) -> None:
        cert = certificate()
        del cert["buildSignerURI"]
        cert["githubWorkflowRef"] = SIGNER
        self.assertFailed(self.run_with(self.archive_attestation(attestation_json(SLSA_PRED, {}, cert))),
                          "attestation-provenance-archive")

    def test_signer_workflow_field_must_be_exact_when_present(self) -> None:
        bad = certificate()
        bad["signerWorkflow"] = f"{REPO}/{verifier.SIGNER_WORKFLOW}@refs/tags/v9.9.90"
        self.assertFailed(self.run_with(self.archive_attestation(attestation_json(SLSA_PRED, {}, bad))),
                          "attestation-provenance-archive")
        good = certificate()
        good["signerWorkflow"] = f"{REPO}/{verifier.SIGNER_WORKFLOW}@refs/tags/{TAG}"
        results = self.run_with(self.archive_attestation(attestation_json(SLSA_PRED, {}, good)))
        self.assertEqual(results["attestation-provenance-archive"].status, "PASS",
                         results["attestation-provenance-archive"].detail)

    # Finding 2: commit pin via --source-digest and certificate digests.

    def test_every_attestation_call_passes_source_digest(self) -> None:
        runner = FakeRunner(self.scenario)
        verifier.run_checks(verifier.parse_args(self.cli_args()), runner)
        attest = [c for c in runner.calls if c[:3] == ["gh", "attestation", "verify"]]
        self.assertEqual(len(attest), 4)
        for argv in attest:
            with self.subTest(argv=argv):
                self.assertIn("--source-digest", argv)
                self.assertEqual(argv[argv.index("--source-digest") + 1], COMMIT)

    def test_dry_run_plan_pins_source_digest(self) -> None:
        args = verifier.parse_args(["--repo", REPO, "--tag", TAG, "--expected-commit", COMMIT,
                                    "--out", str(self.out)])
        attest = [c for c in verifier.plan_commands(args) if "attestation" in c]
        self.assertEqual(len(attest), 4)
        for argv in attest:
            self.assertEqual(argv[argv.index("--source-digest") + 1], COMMIT)

    def test_source_digest_mismatch_fails_all_attestations(self) -> None:
        results = self.run_with(self.all_attestations_with(certificate(source="f" * 40)))
        for name in SIGNER_CHECKS:
            with self.subTest(check=name):
                self.assertFailed(results, name)

    def test_build_signer_digest_mismatch_fails_all_attestations(self) -> None:
        results = self.run_with(self.all_attestations_with(certificate(build="e" * 40)))
        for name in SIGNER_CHECKS:
            with self.subTest(check=name):
                self.assertFailed(results, name)

    def test_missing_source_digest_fails_all_attestations(self) -> None:
        cert = certificate()
        del cert["sourceRepositoryDigest"]
        results = self.run_with(self.all_attestations_with(cert))
        for name in SIGNER_CHECKS:
            with self.subTest(check=name):
                self.assertFailed(results, name)

    def test_absent_build_signer_digest_is_allowed(self) -> None:
        cert = certificate()
        del cert["buildSignerDigest"]
        results = self.run_with(self.all_attestations_with(cert))
        for name in SIGNER_CHECKS:
            with self.subTest(check=name):
                self.assertEqual(results[name].status, "PASS", results[name].detail)

    # Finding 3: malformed responses become FAIL checks, never escaping exceptions.

    def test_non_object_asset_entry_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["release"]["assets"].append(f"luminumbra-{VERSION}-extra.tar.gz")
        results = self.run_with(scenario)
        self.assertFailed(results, "release-assets")
        self.assertIn("malformed", results["release-assets"].detail)

    def test_asset_without_string_name_fails(self) -> None:
        for entry in ({"size": 3}, {"name": 7}):
            with self.subTest(entry=entry):
                scenario = copy.deepcopy(self.scenario)
                scenario["release"]["assets"].append(entry)
                results = self.run_with(scenario)
                self.assertFailed(results, "release-assets")
                self.assertIn("no string name", results["release-assets"].detail)

    def test_assets_not_a_list_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["release"]["assets"] = {"name": ARCHIVE}
        results = self.run_with(scenario)
        self.assertFailed(results, "release-assets")
        self.assertIn("assets must be a list", results["release-assets"].detail)

    def test_tag_object_not_an_object_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["ref"]["object"] = "abc"
        results = self.run_with(scenario)
        self.assertFailed(results, "tag-annotated-commit")
        self.assertFailed(results, "tag-github-verified")

    def test_tag_verification_not_an_object_fails(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["tag"]["verification"] = True
        self.assertFailed(self.run_with(scenario), "tag-github-verified")

    def test_verification_result_not_an_object_fails(self) -> None:
        scenario = self.archive_attestation(json.dumps([{"verificationResult": "x"}]))
        results = self.run_with(scenario)
        self.assertFailed(results, "attestation-provenance-archive")
        self.assertIn("malformed", results["attestation-provenance-archive"].detail)

    def test_certificate_not_an_object_fails(self) -> None:
        body = json.dumps([{"verificationResult": {
            "signature": {"certificate": ["not", "an", "object"]},
            "statement": {"predicateType": SLSA_PRED, "predicate": {}}}}])
        self.assertFailed(self.run_with(self.archive_attestation(body)), "attestation-provenance-archive")

    def test_invalid_utf8_sums_is_reported_as_fail(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["assets"][SUMS] = b"\xff\xfe  not utf-8\n"
        results = self.run_with(scenario)
        self.assertEqual(len(results), 14)
        self.assertFailed(results, "sha256sums-manifest")
        self.assertIn("UTF-8", results["sha256sums-manifest"].detail)

    def test_unexpected_error_is_recorded_not_raised(self) -> None:
        with mock.patch.object(verifier, "signer_matches", side_effect=RuntimeError("boom")):
            results = self.run_with()
        self.assertEqual(len(results), 14)
        for name in SIGNER_CHECKS:
            with self.subTest(check=name):
                self.assertFailed(results, name)
                self.assertIn("RuntimeError", results[name].detail)

    def test_malformed_response_still_writes_report(self) -> None:
        scenario = copy.deepcopy(self.scenario)
        scenario["tag"]["verification"] = "bad"
        scenario["release"]["assets"].append("stray")
        scenario["attest"][(ARCHIVE, SLSA_PRED)] = (0, json.dumps([{"verificationResult": "x"}]))
        with contextlib.redirect_stdout(io.StringIO()):
            rc = verifier.main(self.cli_args(), runner=FakeRunner(scenario))
        self.assertEqual(rc, 1)
        report = json.loads((self.out / verifier.REPORT_NAME).read_text(encoding="utf-8"))
        self.assertEqual(report["overall"], "FAIL")
        by_name = {c["name"]: c for c in report["checks"]}
        self.assertEqual(by_name["tag-github-verified"]["status"], "FAIL")
        self.assertEqual(by_name["release-assets"]["status"], "FAIL")

    # Finding 4: refuse an existing report path (symlink or any file) before network work.

    def test_symlinked_report_refused_before_network(self) -> None:
        self.out.mkdir(parents=True)
        victim = Path(self.tmp.name) / "victim.txt"
        victim.write_text("keep", encoding="utf-8")
        link = verifier.report_path(self.out)
        with report_symlink(link, victim):
            runner = FakeRunner(self.scenario)
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = verifier.main(self.cli_args(), runner=runner)
            self.assertEqual(rc, 2)
            self.assertEqual(runner.calls, [])
            self.assertIn("symlink", stderr.getvalue())
            self.assertEqual(victim.read_text(encoding="utf-8"), "keep")
            self.assertTrue(link.is_symlink())

    def test_dangling_symlink_report_refused_without_creating_target(self) -> None:
        self.out.mkdir(parents=True)
        target = Path(self.tmp.name) / "nowhere.json"
        link = verifier.report_path(self.out)
        with report_symlink(link, target):
            runner = FakeRunner(self.scenario)
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = verifier.main(self.cli_args(), runner=runner)
            self.assertEqual(rc, 2)
            self.assertEqual(runner.calls, [])
            self.assertIn("symlink", stderr.getvalue())
            self.assertTrue(link.is_symlink())
            self.assertFalse(target.exists())

    def test_symlinked_report_refused_without_symlink_privilege(self) -> None:
        # Model Windows Python 3.14 queries that bypass lstat, even on POSIX.
        with mock.patch.object(os, "symlink", side_effect=PermissionError(errno.EACCES, "denied")), \
                mock.patch.object(os.path, "lexists", new=os.path.exists), \
                mock.patch.object(os.path, "islink", return_value=False), \
                mock.patch.object(Path, "is_symlink", new=lambda path: os.path.islink(path)):
            self.test_symlinked_report_refused_before_network()

    def test_dangling_symlink_report_refused_without_symlink_privilege(self) -> None:
        with mock.patch.object(os, "symlink", side_effect=PermissionError(errno.EACCES, "denied")), \
                mock.patch.object(os.path, "lexists", new=os.path.exists), \
                mock.patch.object(os.path, "islink", return_value=False), \
                mock.patch.object(Path, "is_symlink", new=lambda path: os.path.islink(path)):
            self.test_dangling_symlink_report_refused_without_creating_target()

    def test_existing_regular_report_refused(self) -> None:
        self.out.mkdir(parents=True)
        report = verifier.report_path(self.out)
        report.write_text("old", encoding="utf-8")
        runner = FakeRunner(self.scenario)
        with contextlib.redirect_stderr(io.StringIO()):
            rc = verifier.main(self.cli_args(), runner=runner)
        self.assertEqual(rc, 2)
        self.assertEqual(runner.calls, [])
        self.assertEqual(report.read_text(encoding="utf-8"), "old")

    def test_fresh_output_writes_regular_report(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()):
            rc = verifier.main(self.cli_args(), runner=FakeRunner(self.scenario))
        self.assertEqual(rc, 0)
        report = verifier.report_path(self.out)
        self.assertTrue(report.is_file())
        self.assertFalse(report.is_symlink())
        self.assertEqual(json.loads(report.read_text(encoding="utf-8"))["overall"], "PASS")


class RunnerDecodingTests(unittest.TestCase):
    def test_undecodable_output_is_replaced_not_raised(self) -> None:
        code = "import sys; sys.stdout.buffer.write(b'bad \\xff byte'); sys.stderr.buffer.write(b'\\xfe')"
        rc, out, err = verifier.Runner().run([sys.executable, "-I", "-c", code])
        self.assertEqual(rc, 0)
        self.assertIn("bad", out)
        self.assertIn("\ufffd", out)
        self.assertIn("\ufffd", err)


if __name__ == "__main__":
    unittest.main()
