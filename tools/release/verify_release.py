#!/usr/bin/env python3
"""Independently verify a published Luminumbra source release.

Run this from a fresh checkout on an operator machine, not on the release runner.
External commands (gh, git, and the release validators) go through an injectable
Runner so the logic can be exercised with fakes. The exit status is 0 only when
every check passes. A JSON report and a text table are written to --out.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


SCHEMA = "luminumbra.verify_release.v1"
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = Path(__file__).resolve().parents[2]
SIGNER_WORKFLOW = ".github/workflows/release.yml"
SLSA_PREDICATE = "https://slsa.dev/provenance/v1"
SPDX_PREDICATE = "https://spdx.dev/Document/v2.3"
SUMS_NAME = "SHA256SUMS"
SHA_LINE = re.compile(r"^([0-9a-f]{64}) [ *](.+)$")
REPO_PATTERN = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
TAG_PATTERN = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+$")
SHA40_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class Runner:
    """Runs external commands. Tests substitute an object with the same run()."""

    def run(self, argv: Sequence[str], cwd: Optional[Path] = None) -> Tuple[int, str, str]:
        try:
            proc = subprocess.run(
                list(argv),
                cwd=str(cwd) if cwd is not None else None,
                capture_output=True,
                text=True,
                check=False,
                timeout=600,
            )
        except subprocess.TimeoutExpired:
            return 124, "", "command timed out"
        except OSError as exc:
            return 127, "", str(exc)
        return proc.returncode, proc.stdout, proc.stderr


@dataclass
class Check:
    name: str
    status: str
    detail: str


def add_check(checks: List[Check], name: str, ok: bool, detail: str) -> bool:
    checks.append(Check(name, "PASS" if ok else "FAIL", detail))
    return ok


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def gzip_deterministic(data: bytes) -> bytes:
    """Equivalent of `gzip -n` at level 6: no name, mtime 0.

    The stream is written through a file object because GzipFile with an empty
    filename and no file object would try to open a file named ''.
    """
    buffer = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buffer, mtime=0, compresslevel=6) as gz:
        gz.write(data)
    return buffer.getvalue()


def parse_sums(text: str) -> Optional[List[Tuple[str, str]]]:
    entries: List[Tuple[str, str]] = []
    for line in text.splitlines():
        match = SHA_LINE.match(line)
        if match is None:
            return None
        entries.append((match.group(1), match.group(2)))
    return entries


def api_json(runner: Runner, argv: Sequence[str]) -> Tuple[bool, dict, str]:
    rc, out, err = runner.run(argv)
    if rc != 0:
        return False, {}, f"gh exit {rc}: {err.strip()[:300]}"
    try:
        data = json.loads(out)
    except ValueError:
        return False, {}, "non-JSON response"
    if not isinstance(data, dict):
        return False, {}, "unexpected JSON shape"
    return True, data, ""


def signer_matches(result: dict, tag: str) -> bool:
    needle = f"{SIGNER_WORKFLOW}@refs/tags/{tag}"
    verification = result.get("verificationResult") or {}
    certificate = (verification.get("signature") or {}).get("certificate") or {}
    return any(isinstance(value, str) and needle in value for value in certificate.values())


def statement_of(result: dict) -> dict:
    return (result.get("verificationResult") or {}).get("statement") or {}


# Command builders. Shared by live execution and --dry-run-plan.

def cmd_tag_ref(repo: str, tag: str) -> List[str]:
    return ["gh", "api", f"repos/{repo}/git/ref/tags/{tag}"]


def cmd_tag_object(repo: str, sha: str) -> List[str]:
    return ["gh", "api", f"repos/{repo}/git/tags/{sha}"]


def cmd_release(repo: str, tag: str) -> List[str]:
    return ["gh", "api", f"repos/{repo}/releases/tags/{tag}"]


def cmd_download(repo: str, tag: str, out: Path, name: str) -> List[str]:
    return ["gh", "release", "download", tag, "--repo", repo, "--dir", str(out),
            "--clobber", "--pattern", name]


def cmd_git_archive(tar_path: Path, tag: str, version: str) -> List[str]:
    return ["git", "archive", "--format=tar", f"--prefix=luminumbra-{version}/",
            "--output", str(tar_path), tag]


def cmd_attest(repo: str, subject: Path, predicate: str, signer_workflow: bool) -> List[str]:
    argv = ["gh", "attestation", "verify", str(subject), "--repo", repo]
    if signer_workflow:
        argv += ["--signer-workflow", f"{repo}/{SIGNER_WORKFLOW}"]
    return argv + ["--predicate-type", predicate, "--format", "json"]


def cmd_validate_package(archive: Path) -> List[str]:
    return [sys.executable, str(SCRIPT_DIR / "validate_package.py"), str(archive)]


def cmd_validate_sbom(python: str, spdx: Path, archive: Path) -> List[str]:
    return [python, str(SCRIPT_DIR / "validate_sbom.py"), str(spdx), "--archive", str(archive)]


def asset_names(version: str) -> Tuple[str, str]:
    return (f"luminumbra-{version}-source.tar.gz", f"luminumbra-{version}-source.spdx.json")


def plan_commands(args: argparse.Namespace) -> List[List[str]]:
    version = args.tag[1:]
    archive_name, spdx_name = asset_names(version)
    out = Path(args.out)
    python = args.python or sys.executable
    archive, spdx = out / archive_name, out / spdx_name
    plan = [cmd_tag_ref(args.repo, args.tag), cmd_tag_object(args.repo, "<tag-object-sha>"),
            cmd_release(args.repo, args.tag)]
    plan += [cmd_download(args.repo, args.tag, out, name) for name in (archive_name, spdx_name, SUMS_NAME)]
    plan.append(cmd_git_archive(Path("<tmp>/source.tar"), args.tag, version))
    plan.append(cmd_validate_package(archive))
    plan.append(cmd_validate_sbom(python, spdx, archive))
    plan.append(cmd_attest(args.repo, archive, SLSA_PREDICATE, True))
    plan.append(cmd_attest(args.repo, spdx, SLSA_PREDICATE, True))
    plan.append(cmd_attest(args.repo, out / SUMS_NAME, SLSA_PREDICATE, True))
    plan.append(cmd_attest(args.repo, archive, SPDX_PREDICATE, False))
    return plan


def run_checks(args: argparse.Namespace, runner: Runner) -> Tuple[List[Check], Dict[str, dict]]:
    repo, tag, expected = args.repo, args.tag, args.expected_commit
    version = tag[1:]
    archive_name, spdx_name = asset_names(version)
    expected_assets = [archive_name, spdx_name, SUMS_NAME]
    out = Path(args.out)
    source_root = Path(args.source_root) if args.source_root else REPO_ROOT
    python = args.python or sys.executable
    checks: List[Check] = []

    # Tag object: annotated, pointing at the expected commit, and GitHub-verified.
    ok, ref, err = api_json(runner, cmd_tag_ref(repo, tag))
    ref_obj = (ref.get("object") or {}) if ok else {}
    tag_sha = ref_obj.get("sha") if ref_obj.get("type") == "tag" else None
    tag_data: dict = {}
    if tag_sha:
        _, tag_data, err2 = api_json(runner, cmd_tag_object(repo, tag_sha))
        err = err or err2
    tag_obj = tag_data.get("object") or {}
    add_check(checks, "tag-annotated-commit",
              bool(tag_sha) and tag_obj.get("type") == "commit" and tag_obj.get("sha") == expected,
              f"ref type={ref_obj.get('type')!r}; tag object -> {tag_obj.get('type')!r} "
              f"{tag_obj.get('sha')!r}; expected commit {expected}" + (f"; {err}" if err else ""))
    verification = tag_data.get("verification") or {}
    add_check(checks, "tag-github-verified", verification.get("verified") is True,
              f"verification.verified={verification.get('verified')!r} "
              f"reason={verification.get('reason')!r}")

    # Release metadata: published, not draft or prerelease, exactly three assets.
    ok, release, err = api_json(runner, cmd_release(repo, tag))
    add_check(checks, "release-state",
              ok and release.get("draft") is False and release.get("prerelease") is False,
              f"draft={release.get('draft')!r} prerelease={release.get('prerelease')!r}"
              + (f"; {err}" if err else ""))
    observed = sorted(a.get("name") for a in release.get("assets", []) if isinstance(a, dict)) if ok else []
    add_check(checks, "release-assets", ok and observed == sorted(expected_assets),
              f"observed={observed} expected={sorted(expected_assets)}")

    # Download the exact assets into the output directory.
    out.mkdir(parents=True, exist_ok=True)
    download_errors = []
    for name in expected_assets:
        rc, _, derr = runner.run(cmd_download(repo, tag, out, name))
        if rc != 0:
            download_errors.append(f"{name}: rc={rc} {derr.strip()[:200]}")
    paths = {name: out / name for name in expected_assets}
    present = {name: path.is_file() for name, path in paths.items()}
    digests = {name: sha256_file(path) for name, path in paths.items() if present[name]}
    missing = [name for name, here in present.items() if not here]
    download_ok = not download_errors and not missing
    download_detail = "all three assets downloaded"
    if not download_ok:
        problems = download_errors + ([f"missing: {', '.join(missing)}"] if missing else [])
        download_detail = "; ".join(problems)
    add_check(checks, "download-assets", download_ok, download_detail)

    # SHA256SUMS must list exactly the archive and SPDX, with matching digests.
    if present[SUMS_NAME]:
        entries = parse_sums(paths[SUMS_NAME].read_text(encoding="utf-8"))
    else:
        entries = None
    if entries is None:
        add_check(checks, "sha256sums-manifest", False, "SHA256SUMS missing or not sha256sum format")
    else:
        listed = sorted(name for _, name in entries)
        names_ok = listed == sorted([archive_name, spdx_name])
        mismatches = [name for digest, name in entries
                      if name not in digests or digests[name] != digest]
        add_check(checks, "sha256sums-manifest", names_ok and not mismatches,
                  f"listed={listed}; mismatched={mismatches or 'none'}; "
                  + ", ".join(f"{n}={d}" for n, d in sorted(digests.items())))

    # The archive must be byte-identical to a regeneration from the tagged tree.
    archive_path, spdx_path = paths[archive_name], paths[spdx_name]
    if present[archive_name]:
        with tempfile.TemporaryDirectory() as tmp:
            tar_path = Path(tmp) / "source.tar"
            rc, _, gerr = runner.run(cmd_git_archive(tar_path, tag, version), cwd=source_root)
            if rc != 0 or not tar_path.is_file():
                add_check(checks, "archive-regenerated", False,
                          f"git archive failed rc={rc} {gerr.strip()[:200]}")
            else:
                regenerated = sha256_bytes(gzip_deterministic(tar_path.read_bytes()))
                downloaded = digests[archive_name]
                add_check(checks, "archive-regenerated", regenerated == downloaded,
                          f"regenerated sha256={regenerated}; downloaded sha256={downloaded}; "
                          "gzip -n equivalence assumes zlib level 6 and matching header bytes")
    else:
        add_check(checks, "archive-regenerated", False, "archive not downloaded")

    # Package and SBOM validators.
    if present[archive_name]:
        rc, sout, serr = runner.run(cmd_validate_package(archive_path))
        add_check(checks, "package-validation", rc == 0,
                  (sout.strip() or serr.strip())[:300] or f"rc={rc}")
    else:
        add_check(checks, "package-validation", False, "archive not downloaded")
    if present[archive_name] and present[spdx_name]:
        rc, sout, serr = runner.run(cmd_validate_sbom(python, spdx_path, archive_path))
        message = (sout.strip() or serr.strip())[:300] or f"rc={rc}"
        add_check(checks, "sbom-validation", rc == 0,
                  f"{message} [needs the pinned schema deps from requirements-sbom.txt "
                  f"installed in {python}; stdlib alone is not enough]")
    else:
        add_check(checks, "sbom-validation", False, "archive or SPDX not downloaded")

    # Provenance attestations for the three release files.
    for label, name in (("archive", archive_name), ("spdx", spdx_name), ("sha256sums", SUMS_NAME)):
        if not present[name]:
            add_check(checks, f"attestation-provenance-{label}", False, f"{name} not downloaded")
            continue
        argv = cmd_attest(repo, paths[name], SLSA_PREDICATE, True)
        ok, results, detail = attestation_results(runner, argv)
        signer_ok = ok and all(signer_matches(r, tag) for r in results)
        pred_ok = ok and all(statement_of(r).get("predicateType") == SLSA_PREDICATE for r in results)
        add_check(checks, f"attestation-provenance-{label}", ok and signer_ok and pred_ok,
                  detail or f"{len(results)} attestation(s); signer identity "
                  f"{'matches' if signer_ok else 'does NOT match'} "
                  f"{SIGNER_WORKFLOW}@refs/tags/{tag}")

    # SBOM attestation: archive subject, SPDX predicate, equal to the downloaded SPDX.
    if present[archive_name] and present[spdx_name]:
        argv = cmd_attest(repo, archive_path, SPDX_PREDICATE, False)
        ok, results, detail = attestation_results(runner, argv)
        signer_ok = ok and all(signer_matches(r, tag) for r in results)
        add_check(checks, "attestation-sbom", ok and signer_ok,
                  detail or f"{len(results)} SBOM attestation(s); signer identity "
                  f"{'matches' if signer_ok else 'does NOT match'} "
                  f"{SIGNER_WORKFLOW}@refs/tags/{tag}")
        try:
            local_spdx = json.loads(spdx_path.read_text(encoding="utf-8"))
        except ValueError:
            local_spdx = None
        predicates = [statement_of(r).get("predicate") for r in results] if ok else []
        pred_ok = ok and local_spdx is not None and all(p == local_spdx for p in predicates)
        add_check(checks, "sbom-predicate-matches-spdx", pred_ok,
                  f"{len(predicates)} attested predicate(s) compared with downloaded SPDX JSON"
                  if pred_ok else "attested SBOM predicate differs from downloaded SPDX or was unavailable")
    else:
        add_check(checks, "attestation-sbom", False, "archive or SPDX not downloaded")
        add_check(checks, "sbom-predicate-matches-spdx", False, "archive or SPDX not downloaded")

    assets = {name: {"sha256": digests[name], "bytes": paths[name].stat().st_size}
              for name in expected_assets if present[name]}
    return checks, assets


def attestation_results(runner: Runner, argv: Sequence[str]) -> Tuple[bool, List[dict], str]:
    rc, out, err = runner.run(argv)
    if rc != 0:
        return False, [], f"gh attestation verify exit {rc}: {err.strip()[:300]}"
    try:
        data = json.loads(out)
    except ValueError:
        return False, [], "gh attestation verify returned non-JSON output"
    if not isinstance(data, list) or not data or not all(isinstance(d, dict) for d in data):
        return False, [], "no attestations returned"
    return True, data, ""


def write_report(out: Path, args: argparse.Namespace, checks: List[Check],
                 assets: Dict[str, dict]) -> bool:
    overall = all(c.status == "PASS" for c in checks)
    report = {
        "schema": SCHEMA,
        "repo": args.repo,
        "tag": args.tag,
        "expected_commit": args.expected_commit,
        "overall": "PASS" if overall else "FAIL",
        "assets": assets,
        "checks": [asdict(c) for c in checks],
    }
    (out / "verify-report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                            encoding="utf-8")
    return overall


def format_table(checks: List[Check]) -> str:
    width = max(len(c.name) for c in checks)
    lines = [f"{'CHECK'.ljust(width)}  STATUS  DETAIL"]
    lines += [f"{c.name.ljust(width)}  {c.status:<6}  {c.detail}" for c in checks]
    return "\n".join(lines)


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=_pattern(REPO_PATTERN, "OWNER/NAME"))
    parser.add_argument("--tag", required=True, type=_pattern(TAG_PATTERN, "vX.Y.Z"))
    parser.add_argument("--expected-commit", required=True, type=_pattern(SHA40_PATTERN, "40 lowercase hex"))
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--source-root", type=Path, default=None,
                        help="checkout used for git archive (default: the repository containing this script)")
    parser.add_argument("--python", default=None,
                        help="interpreter for validate_sbom.py; it needs requirements-sbom.txt installed")
    parser.add_argument("--dry-run-plan", action="store_true",
                        help="print the commands that would run, without executing them")
    return parser.parse_args(argv)


def _pattern(regex: "re.Pattern[str]", label: str):
    def convert(value: str) -> str:
        if not regex.fullmatch(value):
            raise argparse.ArgumentTypeError(f"expected {label}, got {value!r}")
        return value
    return convert


def main(argv: Optional[Sequence[str]] = None, runner: Optional[Runner] = None) -> int:
    args = parse_args(argv)
    if args.dry_run_plan:
        for command in plan_commands(args):
            print(shlex.join(command))
        return 0
    runner = runner or Runner()
    out = Path(args.out)
    checks, assets = run_checks(args, runner)
    out.mkdir(parents=True, exist_ok=True)
    overall = write_report(out, args, checks, assets)
    print(format_table(checks))
    print(f"overall: {'PASS' if overall else 'FAIL'}; report: {out / 'verify-report.json'}")
    return 0 if overall else 1


if __name__ == "__main__":
    raise SystemExit(main())
