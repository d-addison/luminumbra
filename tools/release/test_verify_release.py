#!/usr/bin/env python3
"""Offline regressions for verify_release.py using a fake Runner; no network or gh calls."""

from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
import tarfile
import tempfile
import unittest
import sys
from pathlib import Path
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


def source_tar() -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w") as archive:
        body = b"project(Luminumbra)\n"
        info = tarfile.TarInfo(f"luminumbra-{VERSION}/CMakeLists.txt")
        info.size = len(body)
        archive.addfile(info, io.BytesIO(body))
    return buffer.getvalue()


def build_scenario() -> Dict[str, object]:
    tar = source_tar()
    archive = verifier.gzip_deterministic(tar)
    spdx_doc = {"spdxVersion": "SPDX-2.3", "name": f"luminumbra-source-{VERSION}"}
    spdx = (json.dumps(spdx_doc, indent=2, sort_keys=True) + "\n").encode("utf-8")
    sums = (f"{hashlib.sha256(archive).hexdigest()}  {ARCHIVE}\n"
            f"{hashlib.sha256(spdx).hexdigest()}  {SPDX}\n").encode("utf-8")

    def attestation(predicate_type: str, predicate: object, signer: str = SIGNER) -> str:
        return json.dumps([{
            "verificationResult": {
                "signature": {"certificate": {"buildSignerURI": signer}},
                "statement": {"predicateType": predicate_type, "predicate": predicate},
            }
        }])

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


class FakeRunner:
    """Answers the commands verify_release issues from a scenario dictionary."""

    def __init__(self, scenario: Dict[str, object]) -> None:
        self.s = scenario
        self.calls: List[List[str]] = []

    def run(self, argv: Sequence[str], cwd: Optional[Path] = None) -> Tuple[int, str, str]:
        argv = list(argv)
        self.calls.append(argv)
        if argv[0] == "git":
            Path(argv[argv.index("--output") + 1]).write_bytes(self.s["tar"])
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
            "--out", str(self.out), "--source-root", self.tmp.name, "--python", "/usr/bin/true",
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
            0, json.dumps([{"verificationResult": {
                "signature": {"certificate": {"buildSignerURI": SIGNER}},
                "statement": {"predicateType": verifier.SPDX_PREDICATE, "predicate": tampered}}}]))
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

    def test_gzip_is_deterministic(self) -> None:
        data = source_tar()
        self.assertEqual(verifier.gzip_deterministic(data), verifier.gzip_deterministic(data))
        self.assertEqual(verifier.gzip_deterministic(data)[4:8], b"\0\0\0\0")

    def test_bad_arguments_rejected(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            verifier.parse_args(["--repo", REPO, "--tag", "0.3.0", "--expected-commit", COMMIT,
                                 "--out", "x"])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            verifier.parse_args(["--repo", REPO, "--tag", TAG, "--expected-commit", "ABC",
                                 "--out", "x"])


if __name__ == "__main__":
    unittest.main()
