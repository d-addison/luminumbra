#!/usr/bin/env python3
"""Independently verify a published Luminumbra source release.

Run this from a fresh checkout on an operator machine, not on the release runner.
External commands (gh, git, and the release validators) go through an injectable
Runner so the logic can be exercised with fakes. The exit status is 0 only when
every check passes. A JSON report and a text table are written to --out.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
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
REPORT_NAME = "verify-report.json"
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
                encoding="utf-8",
                errors="replace",
                check=False,
                timeout=600,
            )
        except subprocess.TimeoutExpired:
            return 124, "", "command timed out"
        except OSError as exc:
            return 127, "", str(exc)
        return proc.returncode, proc.stdout, proc.stderr

    def run_bytes(self, argv: Sequence[str], cwd: Optional[Path] = None) -> Tuple[int, bytes, str]:
        """Like run(), but stdout is returned as raw bytes for binary-safe output."""
        try:
            proc = subprocess.run(
                list(argv),
                cwd=str(cwd) if cwd is not None else None,
                capture_output=True,
                check=False,
                timeout=600,
            )
        except subprocess.TimeoutExpired:
            return 124, b"", "command timed out"
        except OSError as exc:
            return 127, b"", str(exc)
        return proc.returncode, proc.stdout, proc.stderr.decode("utf-8", "replace")


@dataclass
class Check:
    name: str
    status: str
    detail: str


class ShapeError(Exception):
    """A response parsed as JSON but does not have the nested shape the checks rely on."""


def add_check(checks: List[Check], name: str, ok: bool, detail: str) -> bool:
    checks.append(Check(name, "PASS" if ok else "FAIL", detail))
    return ok


def record_failure(checks: List[Check], names: Sequence[str], exc: Exception) -> None:
    """Turn an exception raised while evaluating a section into FAIL checks for its names.

    Names that were already recorded before the exception are left alone, so a partial
    section never produces duplicate entries.
    """
    if isinstance(exc, ShapeError):
        detail = f"malformed response: {exc}"
    else:
        detail = f"unexpected response shape: {type(exc).__name__}: {exc}"
    recorded = {c.name for c in checks}
    for name in names:
        if name not in recorded:
            add_check(checks, name, False, detail)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


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


def _obj(parent: dict, key: str) -> dict:
    """Return parent[key] as a dict; missing or null means {}; any other type is malformed."""
    value = parent.get(key)
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ShapeError(f"{key} must be a JSON object, got {type(value).__name__}")
    return value


def expected_signer(repo: str, tag: str) -> str:
    return f"https://github.com/{repo}/{SIGNER_WORKFLOW}@refs/tags/{tag}"


def certificate_of(result: dict) -> dict:
    verification = _obj(result, "verificationResult")
    return _obj(_obj(verification, "signature"), "certificate")


def signer_matches(result: dict, repo: str, tag: str) -> bool:
    """Exact signer identity check. Substring or other-field matches never count.

    buildSignerURI must be present and equal the expected workflow URI exactly. If a
    signerWorkflow field is present it must be the exact `<repo>/<workflow>@refs/tags/<tag>`.
    """
    certificate = certificate_of(result)
    if certificate.get("buildSignerURI") != expected_signer(repo, tag):
        return False
    if "signerWorkflow" in certificate:
        if certificate["signerWorkflow"] != f"{repo}/{SIGNER_WORKFLOW}@refs/tags/{tag}":
            return False
    return True


def commit_pinned(result: dict, commit: str) -> bool:
    """The attestation certificate must name the expected commit as its source digest.

    buildSignerDigest is checked too when present; a mismatch is a failure.
    """
    certificate = certificate_of(result)
    if certificate.get("sourceRepositoryDigest") != commit:
        return False
    build_digest = certificate.get("buildSignerDigest")
    if build_digest is not None and build_digest != commit:
        return False
    return True


def statement_of(result: dict) -> dict:
    return _obj(_obj(result, "verificationResult"), "statement")


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


def cmd_git_archive(tar_path: Path, commit: str, version: str) -> List[str]:
    # Same tree as release.yml's `git archive ... HEAD` on the tag checkout; the commit is
    # the one the signed tag points at, so it is pinned explicitly here.
    return ["git", "archive", "--format=tar", f"--prefix=luminumbra-{version}/",
            f"--output={tar_path}", commit]


def cmd_gzip(tar_path: Path) -> List[str]:
    # Mirrors `... | gzip -n` in release.yml, reading the tar from a file instead of a pipe.
    return ["gzip", "-n", "-c", str(tar_path)]


def cmd_attest(repo: str, subject: Path, predicate: str, signer_workflow: bool,
               commit: str) -> List[str]:
    argv = ["gh", "attestation", "verify", str(subject), "--repo", repo,
            "--source-digest", commit]
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
    commit = args.expected_commit
    plan = [cmd_tag_ref(args.repo, args.tag), cmd_tag_object(args.repo, "<tag-object-sha>"),
            cmd_release(args.repo, args.tag)]
    plan += [cmd_download(args.repo, args.tag, out, name) for name in (archive_name, spdx_name, SUMS_NAME)]
    plan.append(cmd_git_archive(Path("<tmp>/source.tar"), commit, version))
    plan.append(cmd_gzip(Path("<tmp>/source.tar")))
    plan.append(cmd_validate_package(archive))
    plan.append(cmd_validate_sbom(python, spdx, archive))
    plan.append(cmd_attest(args.repo, archive, SLSA_PREDICATE, True, commit))
    plan.append(cmd_attest(args.repo, spdx, SLSA_PREDICATE, True, commit))
    plan.append(cmd_attest(args.repo, out / SUMS_NAME, SLSA_PREDICATE, True, commit))
    plan.append(cmd_attest(args.repo, archive, SPDX_PREDICATE, False, commit))
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
    try:
        ok, ref, err = api_json(runner, cmd_tag_ref(repo, tag))
        ref_obj = _obj(ref, "object") if ok else {}
        tag_sha = ref_obj.get("sha") if ref_obj.get("type") == "tag" else None
        if tag_sha is not None and not isinstance(tag_sha, str):
            raise ShapeError("object.sha must be a string")
        tag_data: dict = {}
        if tag_sha:
            _, tag_data, err2 = api_json(runner, cmd_tag_object(repo, tag_sha))
            err = err or err2
        tag_obj = _obj(tag_data, "object")
        verification = _obj(tag_data, "verification")
        add_check(checks, "tag-annotated-commit",
                  bool(tag_sha) and tag_obj.get("type") == "commit" and tag_obj.get("sha") == expected,
                  f"ref type={ref_obj.get('type')!r}; tag object -> {tag_obj.get('type')!r} "
                  f"{tag_obj.get('sha')!r}; expected commit {expected}" + (f"; {err}" if err else ""))
        add_check(checks, "tag-github-verified", verification.get("verified") is True,
                  f"verification.verified={verification.get('verified')!r} "
                  f"reason={verification.get('reason')!r}")
    except Exception as exc:
        record_failure(checks, ["tag-annotated-commit", "tag-github-verified"], exc)

    # Release metadata: published, not draft or prerelease, exactly three assets.
    try:
        ok, release, err = api_json(runner, cmd_release(repo, tag))
        add_check(checks, "release-state",
                  ok and release.get("draft") is False and release.get("prerelease") is False,
                  f"draft={release.get('draft')!r} prerelease={release.get('prerelease')!r}"
                  + (f"; {err}" if err else ""))
        observed: List[str] = []
        asset_problems: List[str] = []
        if ok:
            raw_assets = release.get("assets", [])
            if not isinstance(raw_assets, list):
                raise ShapeError(f"assets must be a list, got {type(raw_assets).__name__}")
            for index, entry in enumerate(raw_assets):
                if not isinstance(entry, dict):
                    asset_problems.append(f"entry {index} is {type(entry).__name__}, not an object")
                elif not isinstance(entry.get("name"), str):
                    asset_problems.append(f"entry {index} has no string name")
                else:
                    observed.append(entry["name"])
        observed.sort()
        if asset_problems:
            add_check(checks, "release-assets", False,
                      "malformed asset entries: " + "; ".join(asset_problems))
        else:
            add_check(checks, "release-assets", ok and observed == sorted(expected_assets),
                      f"observed={observed} expected={sorted(expected_assets)}"
                      + (f"; {err}" if err else ""))
    except Exception as exc:
        record_failure(checks, ["release-state", "release-assets"], exc)

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
    try:
        entries: Optional[List[Tuple[str, str]]] = None
        sums_reason = "SHA256SUMS missing or not sha256sum format"
        if present[SUMS_NAME]:
            try:
                sums_text = paths[SUMS_NAME].read_bytes().decode("utf-8")
            except UnicodeDecodeError as exc:
                sums_text = None
                sums_reason = f"SHA256SUMS is not valid UTF-8 ({exc.reason} at byte {exc.start})"
            if sums_text is not None:
                entries = parse_sums(sums_text)
        if entries is None:
            add_check(checks, "sha256sums-manifest", False, sums_reason)
        else:
            listed = sorted(name for _, name in entries)
            names_ok = listed == sorted([archive_name, spdx_name])
            mismatches = [name for digest, name in entries
                          if name not in digests or digests[name] != digest]
            add_check(checks, "sha256sums-manifest", names_ok and not mismatches,
                      f"listed={listed}; mismatched={mismatches or 'none'}; "
                      + ", ".join(f"{n}={d}" for n, d in sorted(digests.items())))
    except Exception as exc:
        record_failure(checks, ["sha256sums-manifest"], exc)

    # The archive must be byte-identical to `git archive | gzip -n` run on the expected commit.
    archive_path, spdx_path = paths[archive_name], paths[spdx_name]
    if present[archive_name]:
        with tempfile.TemporaryDirectory() as tmp:
            tar_path = Path(tmp) / "source.tar"
            rc, _, gerr = runner.run(cmd_git_archive(tar_path, expected, version), cwd=source_root)
            if rc != 0 or not tar_path.is_file():
                add_check(checks, "archive-regenerated", False,
                          f"git archive failed rc={rc} {gerr.strip()[:200]}")
            else:
                zrc, regenerated_bytes, zerr = runner.run_bytes(cmd_gzip(tar_path), cwd=source_root)
                if zrc != 0:
                    add_check(checks, "archive-regenerated", False,
                              f"gzip -n failed rc={zrc} {zerr.strip()[:200]}")
                else:
                    regenerated = sha256_bytes(regenerated_bytes)
                    downloaded = digests[archive_name]
                    add_check(checks, "archive-regenerated", regenerated == downloaded,
                              f"regenerated sha256={regenerated}; downloaded sha256={downloaded}; "
                              f"equality depends on this verifier's gzip build matching the "
                              f"runner's (both GNU gzip -n; header OS byte and deflate output "
                              f"must agree)")
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
        check_name = f"attestation-provenance-{label}"
        if not present[name]:
            add_check(checks, check_name, False, f"{name} not downloaded")
            continue
        try:
            argv = cmd_attest(repo, paths[name], SLSA_PREDICATE, True, expected)
            ok, results, detail = attestation_results(runner, argv)
            signer_ok = ok and all(signer_matches(r, repo, tag) for r in results)
            commit_ok = ok and all(commit_pinned(r, expected) for r in results)
            pred_ok = ok and all(statement_of(r).get("predicateType") == SLSA_PREDICATE for r in results)
            add_check(checks, check_name, ok and signer_ok and commit_ok and pred_ok,
                      detail or f"{len(results)} attestation(s); signer identity "
                      f"{'matches' if signer_ok else 'does NOT match'} "
                      f"{expected_signer(repo, tag)}; source digest "
                      f"{'pinned' if commit_ok else 'NOT pinned'} to {expected}")
        except Exception as exc:
            record_failure(checks, [check_name], exc)

    # SBOM attestation: archive subject, SPDX predicate, equal to the downloaded SPDX.
    if present[archive_name] and present[spdx_name]:
        try:
            argv = cmd_attest(repo, archive_path, SPDX_PREDICATE, False, expected)
            ok, results, detail = attestation_results(runner, argv)
            signer_ok = ok and all(signer_matches(r, repo, tag) for r in results)
            commit_ok = ok and all(commit_pinned(r, expected) for r in results)
            add_check(checks, "attestation-sbom", ok and signer_ok and commit_ok,
                      detail or f"{len(results)} SBOM attestation(s); signer identity "
                      f"{'matches' if signer_ok else 'does NOT match'} "
                      f"{expected_signer(repo, tag)}; source digest "
                      f"{'pinned' if commit_ok else 'NOT pinned'} to {expected}")
            try:
                local_spdx = json.loads(spdx_path.read_text(encoding="utf-8"))
            except ValueError:
                local_spdx = None
            predicates = [statement_of(r).get("predicate") for r in results] if ok else []
            pred_ok = ok and local_spdx is not None and all(p == local_spdx for p in predicates)
            add_check(checks, "sbom-predicate-matches-spdx", pred_ok,
                      f"{len(predicates)} attested predicate(s) compared with downloaded SPDX JSON"
                      if pred_ok else "attested SBOM predicate differs from downloaded SPDX or was unavailable")
        except Exception as exc:
            record_failure(checks, ["attestation-sbom", "sbom-predicate-matches-spdx"], exc)
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


def report_path(out: Path) -> Path:
    return out / REPORT_NAME


def refuse_existing_report(out: Path) -> Optional[str]:
    """Return an error message if the report path already exists in any form.

    Checked before any network work. A symlink (even a dangling one) or any existing file
    is refused, so the report is only ever created fresh inside --out.
    """
    target = report_path(out)
    if os.path.lexists(target):
        kind = "symlink" if target.is_symlink() else "existing file"
        return (f"refusing to write {target}: {kind} already present; "
                f"use a fresh or empty --out directory")
    return None


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
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(str(report_path(out)), flags, 0o644)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(report, indent=2, sort_keys=True) + "\n")
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
    out = Path(args.out)
    refusal = refuse_existing_report(out)
    if refusal is not None:
        print(refusal, file=sys.stderr)
        return 2
    runner = runner or Runner()
    checks, assets = run_checks(args, runner)
    out.mkdir(parents=True, exist_ok=True)
    try:
        overall = write_report(out, args, checks, assets)
    except OSError as exc:
        print(f"could not write {report_path(out)}: {exc}", file=sys.stderr)
        return 2
    print(format_table(checks))
    print(f"overall: {'PASS' if overall else 'FAIL'}; report: {report_path(out)}")
    return 0 if overall else 1


if __name__ == "__main__":
    raise SystemExit(main())
