#!/usr/bin/env python3
"""Bind explicitly requested AC2/AC4 CI captures; does not qualify native runs."""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import subprocess
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
REPO = "d-addison/luminumbra"
SCHEMA = "luminumbra.ci_qualification_capture.v1"
LANES = {
    "linux-gcc": ("ci.yml", "Linux / GCC", ("identity", "configure", "build", "tests")),
    "asan": ("asan.yml", "Linux / GCC ASan", ("identity", "configure", "build", "tests")),
    "windows-msvc": ("ci.yml", "Windows / MSVC", ("identity", "native")),
    "windows-ucrt64": ("ci.yml", "Windows / MSYS2 UCRT64", ("identity", "configure", "build", "tests")),
    "server-capture": ("performance.yml", "Simulation budget qualification / candidate", ("identity", "configure", "build", "capture", "proposal", "check")),
}
PRESETS = ("default", "mountains", "archipelago")
PARAMETERS = {"seed": "1337", "ticks": 600, "surface_radius": 1, "collision_radius": 1}
PREFIX = "AMBIENT_RECEIPT "
SERVER_BINARY = "build/qualification-build/bin/luminumbra_server_app"
BASELINE = ROOT / "tools/perf/baselines/sim-budgets.json"


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(value):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{40}", value), "full lowercase candidate SHA required")
    return value


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object)


def module(name):
    # Dependencies on unmerged AC2/AC4 code are checked only in enabled capture
    # paths. No alternate revision or replacement implementation is loaded.
    for directory in (ROOT / "tools/ci", ROOT / "tools/perf"):
        if str(directory) not in sys.path:
            sys.path.insert(0, str(directory))
    return importlib.import_module(name)


def command(argv):
    result = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True,
                            text=True, encoding="utf-8", timeout=120, check=False)
    require(result.returncode == 0, f"command failed: {argv[0]}")
    return result.stdout.strip()


def identity(lane, expected, environment, checkout):
    sha(expected)
    workflow = f".github/workflows/{LANES[lane][0]}"
    require(environment["GITHUB_REPOSITORY"] == REPO, "repository mismatch")
    require(environment["GITHUB_EVENT_NAME"] == "workflow_dispatch", "capture requires explicit manual dispatch")
    require(environment["GITHUB_SHA"] == expected == environment["QUALIFICATION_WORKFLOW_SHA"] == checkout,
            "workflow, event, candidate and checkout SHA must agree")
    reference = environment["QUALIFICATION_WORKFLOW_REF"]
    require(reference.startswith(f"{REPO}/{workflow}@refs/"), "workflow path/ref mismatch")
    for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT"):
        require(re.fullmatch(r"[1-9][0-9]*", environment[key]), f"invalid {key}")
    return {"repo": REPO, "candidate_sha": expected, "event_sha": expected,
            "workflow_sha": expected, "workflow_ref": reference, "workflow_path": workflow,
            "run_id": int(environment["GITHUB_RUN_ID"]), "run_attempt": int(environment["GITHUB_RUN_ATTEMPT"]),
            "lane": lane, "job_name": LANES[lane][1]}


def source_identity(lane, expected):
    result = identity(lane, expected, os.environ, command(["git", "-C", str(ROOT), "rev-parse", "HEAD"]))
    require(not command(["git", "-C", str(ROOT), "status", "--porcelain"]), "source checkout is dirty")
    result["source_dirty"] = False
    result["fixture_scope"] = "headless-server"
    result["fixture_hash"] = module("fixture_hash").fixture_hash(ROOT / "data/common", scope="headless-server")
    return result


def preflight(lane, expected):
    source_identity(lane, expected)
    dependencies = ("capture_sim_budget", "sim_budget_io", "propose_sim_budgets", "check_sim_budgets") if lane == "server-capture" else ("compare_ambient_receipts",)
    for name in dependencies:
        module(name)
    if lane == "server-capture":
        module("check_sim_budgets").load_budgets(BASELINE)


def receipt_lines(text):
    return [line.strip() for line in (text or "").splitlines() if line.strip().startswith(PREFIX)]


def ambient_receipt(path):
    root = ET.parse(path).getroot()
    producers, attributed = [], set()
    for testcase in root.iter("testcase"):
        representations = []
        attribute = testcase.get("ambient_receipt")
        if attribute is not None:
            representations.append(attribute)
        for element in testcase.iter("property"):
            if element.get("name") == "ambient_receipt":
                representations.append(element.get("value") or "")
                attributed.add(id(element))
        for element in testcase.iter("system-out"):
            representations.append("".join(element.itertext()))
            attributed.add(id(element))
        values = []
        for representation in representations:
            lines = receipt_lines(representation)
            require(len(lines) <= 1, "duplicate receipt lines in one representation")
            values.extend(lines)
        if values:
            require(not any(child.tag in ("failure", "error", "skipped") for child in testcase), "ambient testcase failed or skipped")
            require(testcase.get("name", "").endswith("AmbientDeterminismReceipt.EmitsConstructorAndEvolvedHashes"), "receipt from an unexpected testcase")
            require(len(set(values)) == 1, "conflicting ambient representations")
            producers.append(values[0])
    for element in root.iter():
        if id(element) not in attributed and element.tag in ("system-out", "property"):
            text = element.get("value") or "".join(element.itertext())
            require(not receipt_lines(text), "unattributed ambient receipt")
    require(len(producers) == 1, "exactly one ambient receipt testcase required")
    comparator = module("compare_ambient_receipts")
    try:
        receipt = comparator._parse_receipt("qualification", producers[0])
    except comparator.InputError as error:
        raise ValueError(str(error)) from error
    require(receipt["seed"] == "424242" and receipt["ticks"] == "64" and receipt["ceiling"] == "256", "unexpected ambient parameters/dispatch ceiling")
    return receipt


def successful_producers(lane, outcomes):
    require(set(outcomes) == set(LANES[lane][2]), "missing or unknown producer outcome")
    require(all(value == "success" for value in outcomes.values()), "producer failed, skipped, cancelled or missing")


def asan_producers(binding, records):
    require(len(records) == 2 and [item["shard"] for item in records] == [1, 2],
            "exactly two ordered ASan shard records required")
    for record in records:
        require(all(record[key] == binding[key] for key in binding if key != "run_attempt"),
                "ASan shard identity differs")
        # Partial reruns reuse successful shards from earlier attempts of this run.
        require(type(record["run_attempt"]) is int and 1 <= record["run_attempt"] <= binding["run_attempt"],
                "ASan shard attempt is invalid or newer than the aggregate")
        successful_producers("asan", record["producers"])
        require(re.fullmatch(r"[0-9a-f]{64}", record["binary_sha256"])
                and re.fullmatch(r"[0-9a-f]{64}", record["junit_sha256"]), "ASan shard digest missing")
    receipts = [record for record in records if "ambient" in record]
    require(len(receipts) == 1, "exactly one ASan shard must produce the ambient receipt")
    return receipts[0]


def write_asan_shard(args):
    require(args.lane == "asan", "ASan shard requires the ASan lane")
    binding = source_identity("asan", args.expected_sha)
    outcomes = json.loads(os.environ.get("QUALIFICATION_PRODUCERS", "{}"))
    successful_producers("asan", outcomes)
    record = dict(binding, shard=args.shard, producers=outcomes,
                  binary_sha256=digest(args.binary), junit_sha256=digest(args.junit))
    cases = ET.parse(args.junit).getroot().iter("testcase")
    if any(case.get("name", "").endswith("AmbientDeterminismReceipt.EmitsConstructorAndEvolvedHashes")
           for case in cases):
        record["ambient"] = ambient_receipt(args.junit)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(record, stream, sort_keys=True, indent=2)
        stream.write("\n")


def capture_contract(directory, expected, fixture, binary_hash, producer_root):
    loaded = module("sim_budget_io").load_capture(directory)
    manifest = loaded["manifest"]
    require(manifest["revision"] == expected and manifest["source_dirty"] is False, "capture source identity differs")
    require(manifest["fixture_hash"] == fixture and manifest["binary_sha256"] == binary_hash, "capture fixture/binary identity differs")
    require(all(manifest[key] == value and type(manifest[key]) is type(value) for key, value in PARAMETERS.items()), "capture parameters differ")
    require(tuple(run["preset"] for run in manifest["runs"]) == PRESETS, "exact three ordered presets required")
    # This is producer metadata; its filesystem syntax can differ from ours.
    root = PureWindowsPath(producer_root) if PureWindowsPath(producer_root).drive else PurePosixPath(producer_root)
    require(root.is_absolute() and str(root) == producer_root, "producer source root missing or malformed")
    binary = root / SERVER_BINARY
    for run in manifest["runs"]:
        require(run["artifact"] == f"{run['preset']}.json", "capture artifact path differs")
        argv = run.get("command")
        require(isinstance(argv, list) and len(argv) == 22, "capture command missing or malformed")
        artifact = root / "build/qualification/sim-budget-capture" / run["artifact"]
        expected_argv = module("capture_sim_budget").command(binary, root, artifact, run["preset"], "1337", 600, 1, 1)
        require(argv == expected_argv and type(root)(argv[-1]).name == run["artifact"], "capture command flags/identity differ")
    return loaded


def check_contract(loaded, baseline, path):
    checker = module("check_sim_budgets")
    budgets = checker.load_budgets(baseline)
    require(checker.identity_mismatch(budgets["identity"], loaded["manifest"]) is None, "baseline workload identity differs")
    report, code = checker.build_report(loaded, budgets, False)
    require(code == 0 and read_json(path) == report, "budget check does not match selected capture/baseline")


def write_manifest(args):
    document = {"schema": SCHEMA, "status": "incomplete", "lane": args.lane,
                "native_qualified": False, "binary_availability": "not-exported"}
    try:
        binding = source_identity(args.lane, args.expected_sha)
        outcomes = json.loads(os.environ.get("QUALIFICATION_PRODUCERS", "{}"))
        if args.lane == "asan":
            require(outcomes == {"tests": "success"}, "ASan aggregate checks did not succeed")
            records = []
            for shard in (1, 2):
                source = args.asan_shards / f"shard-{shard}"
                record = read_json(source / "qualification-shard.json")
                require(record["junit_sha256"] == digest(source / "ctest.junit.xml"), "ASan shard JUnit digest differs")
                records.append(record)
            producer = asan_producers(binding, records)
            require(producer["ambient"] == ambient_receipt(args.junit), "merged ASan receipt differs")
            document["asan_shards"] = records
            outcomes = dict.fromkeys(LANES["asan"][2], "success")
            document["binary_sha256"] = producer["binary_sha256"]
        else:
            require(args.binary is not None and args.binary.is_file(), "producer binary missing")
            document["binary_sha256"] = digest(args.binary)
        document.update(binding, producers=outcomes)
        successful_producers(args.lane, outcomes)
        if args.lane == "server-capture":
            require(args.binary.resolve() == ROOT / SERVER_BINARY, "server binary path differs")
            require(digest(args.baseline) == digest(BASELINE), "baseline is not from this candidate")
            document["producer_source_root"] = str(ROOT)
            loaded = capture_contract(args.capture, args.expected_sha, binding["fixture_hash"], document["binary_sha256"], str(ROOT))
            proposal = read_json(args.proposal)
            require(proposal == module("propose_sim_budgets").build_proposal(loaded, 3, 2), "proposal does not match capture")
            check_contract(loaded, args.baseline, args.check)
            document["parameters"] = PARAMETERS
            document["budget_complete"] = False
            document["paired_comparison"] = "unevaluated:not-requested"
            files = [args.proposal, args.check, args.baseline, *sorted(args.capture.glob("*"))]
            require(all(path.is_file() for path in files), "unexpected capture directory entry")
        else:
            document["ambient"] = ambient_receipt(args.junit)
            files = [args.junit]
        parent = args.output.parent.resolve()
        artifacts = {}
        for path in files:
            path = path.resolve()
            require(path.is_relative_to(parent), "manifest artifacts must be contained beside the manifest")
            artifacts[path.relative_to(parent).as_posix()] = digest(path)
        document["files"] = artifacts
        document["status"] = "complete"
    except (ValueError, OSError, KeyError, TypeError, AttributeError, ImportError, UnicodeError, zipfile.BadZipFile, ET.ParseError, subprocess.SubprocessError) as error:
        document["error"] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(document, stream, sort_keys=True, indent=2)
        stream.write("\n")
    require(document["status"] == "complete", document.get("error", "incomplete capture"))


def api(endpoint):
    return json.loads(command(["gh", "api", endpoint]))


def verify_producer(document):
    path = f"repos/{REPO}/actions/runs/{document['run_id']}/attempts/{document['run_attempt']}"
    run = api(path)
    require(run["head_sha"] == document["candidate_sha"] and run["event"] == "workflow_dispatch"
            and run["run_attempt"] == document["run_attempt"] and run["path"] == document["workflow_path"], "GitHub producer run identity differs")
    jobs, seen, total = [], set(), None
    for page in range(1, 10001):
        response = api(f"{path}/jobs?per_page=100&page={page}")
        rows = response["jobs"]
        require(isinstance(rows, list) and len(rows) <= 100, "invalid producer jobs page")
        total = response["total_count"] if total is None else total
        require(type(total) is int and total >= 0 and response["total_count"] == total, "producer pagination changed")
        ids = [job["id"] for job in rows]
        require(len(set(ids)) == len(ids) and not seen.intersection(ids), "duplicate producer jobs")
        seen.update(ids)
        jobs.extend(rows)
        if len(rows) < 100:
            require(len(jobs) == total, "producer job pagination incomplete")
            break
    else:
        raise ValueError("producer job pagination did not terminate")
    matches = [job for job in jobs if job["name"] == document["job_name"]]
    require(len(matches) == 1 and matches[0]["status"] == "completed" and matches[0]["conclusion"] == "success", "producer job not independently successful")
    return {"job_id": matches[0]["id"], "completed_at": matches[0]["completed_at"]}


def validate_chain(expected, paths):
    sha(expected)
    require(command(["git", "-C", str(ROOT), "rev-parse", "HEAD"]) == expected,
            "validate chain with helpers checked out at the same candidate")
    require(not command(["git", "-C", str(ROOT), "status", "--porcelain"]), "validator source is dirty")
    fixture = module("fixture_hash").fixture_hash(ROOT / "data/common", scope="headless-server")
    documents, manifests, proofs, observed = {}, {}, {}, {}
    for path in paths:
        observed[path] = digest(path)
        document = read_json(path)
        lane = document["lane"]
        require(lane in LANES and lane not in documents, "unknown or duplicate lane")
        require(document["schema"] == SCHEMA and document["status"] == "complete" and document["native_qualified"] is False
                and document["binary_availability"] == "not-exported", "incomplete/future qualification manifest")
        env = {"GITHUB_REPOSITORY": document["repo"], "GITHUB_EVENT_NAME": "workflow_dispatch",
               "GITHUB_SHA": document["event_sha"], "QUALIFICATION_WORKFLOW_SHA": document["workflow_sha"],
               "QUALIFICATION_WORKFLOW_REF": document["workflow_ref"], "GITHUB_RUN_ID": str(document["run_id"]),
               "GITHUB_RUN_ATTEMPT": str(document["run_attempt"])}
        identity(lane, expected, env, document["candidate_sha"])
        require(document["workflow_path"] == f".github/workflows/{LANES[lane][0]}"
                and document["job_name"] == LANES[lane][1], "workflow/job declaration differs")
        require(document["source_dirty"] is False and document["fixture_scope"] == "headless-server", "dirty source or fixture definition mismatch")
        successful_producers(lane, document["producers"])
        if lane == "asan":
            binding = {**identity(lane, expected, env, document["candidate_sha"]),
                       "source_dirty": False, "fixture_scope": "headless-server", "fixture_hash": fixture}
            producer = asan_producers(binding, document["asan_shards"])
            require(producer["binary_sha256"] == document["binary_sha256"]
                    and producer["ambient"] == document["ambient"], "ASan producer binding differs")
        require(re.fullmatch(r"[0-9a-f]{64}", document["binary_sha256"]), "binary identity missing")
        require(document["files"], "manifest has no evidence files")
        for name, value in document["files"].items():
            artifact = (path.parent / name).resolve()
            require(artifact.is_relative_to(path.parent.resolve()) and digest(artifact) == value, "artifact path/digest differs")
            observed[artifact] = value
        documents[lane], manifests[lane] = document, path
        proofs[lane] = verify_producer(document)
    require(set(documents) == set(LANES), "four ambient lanes and server capture required")
    require(all(document["fixture_hash"] == fixture for document in documents.values()), "mixed or inauthentic fixture identity")
    receipts = {}
    for lane in LANES:
        if lane == "server-capture":
            continue
        document, path = documents[lane], manifests[lane]
        require(set(document["files"]) == {"ctest.junit.xml"}, "unexpected ambient evidence set")
        receipt = ambient_receipt(path.parent / "ctest.junit.xml")
        require(receipt == document["ambient"], "ambient manifest differs from JUnit")
        receipts[lane] = receipt
    require(not module("compare_ambient_receipts")._mismatches(receipts), "ambient receipts disagree across lanes")
    document, path = documents["server-capture"], manifests["server-capture"]
    required = {"sim-budget-proposal.json", "sim-budget-check.json", "sim-budget-baseline.json", "sim-budget-capture/capture.json",
                *(f"sim-budget-capture/{preset}.{extension}" for preset in PRESETS for extension in ("json", "log"))}
    require(set(document["files"]) == required, "simulation evidence set incomplete or ambiguous")
    require(document["parameters"] == PARAMETERS and document["budget_complete"] is False
            and document["paired_comparison"] == "unevaluated:not-requested", "unsupported capture qualification claim")
    require(digest(path.parent / "sim-budget-baseline.json") == digest(BASELINE), "baseline is not from this candidate")
    loaded = capture_contract(path.parent / "sim-budget-capture", expected, document["fixture_hash"], document["binary_sha256"], document["producer_source_root"])
    require(read_json(path.parent / "sim-budget-proposal.json") == module("propose_sim_budgets").build_proposal(loaded, 3, 2), "proposal differs from selected capture")
    check_contract(loaded, path.parent / "sim-budget-baseline.json", path.parent / "sim-budget-check.json")
    require(all(digest(item) == value for item, value in observed.items()), "evidence changed during validation")
    require(command(["git", "-C", str(ROOT), "rev-parse", "HEAD"]) == expected
            and not command(["git", "-C", str(ROOT), "status", "--porcelain"]), "validator source changed")
    return {"schema": "luminumbra.ci_qualification_chain.v1", "candidate_sha": expected,
            "ambient_agreement": True, "candidate_capture_valid": True,
            "budget_qualified": False, "native_qualified": False,
            "manifests": {lane: {"sha256": digest(path), "run_id": documents[lane]["run_id"],
                "run_attempt": documents[lane]["run_attempt"], **proofs[lane]} for lane, path in manifests.items()}}



def verify_archive(lane, run_id, attempt, archive, document):
    require(lane in LANES and document["lane"] == lane, "selected archive lane differs")
    require(type(run_id) is int and run_id > 0 and type(attempt) is int and attempt > 0,
            "explicit positive run ID and attempt required")
    require(document["run_id"] == run_id and document["run_attempt"] == attempt,
            "selected archive run/attempt differs")
    name = f"qualification-{lane}-{document['candidate_sha']}-{run_id}-{attempt}"
    rows, seen, total = [], set(), None
    for page in range(1, 10001):
        response = api(f"repos/{REPO}/actions/runs/{run_id}/artifacts?per_page=100&page={page}")
        entries = response["artifacts"]
        total = response["total_count"] if total is None else total
        require(type(total) is int and total >= 0 and response["total_count"] == total,
                "artifact pagination changed")
        require(isinstance(entries, list) and len(entries) <= 100, "invalid artifact page")
        ids = [item["id"] for item in entries]
        require(len(set(ids)) == len(ids) and not seen.intersection(ids), "duplicate artifact IDs")
        seen.update(ids)
        rows.extend(entries)
        if len(entries) < 100:
            require(len(rows) == total, "artifact pagination incomplete")
            break
    else:
        raise ValueError("artifact pagination did not terminate")
    matches = [item for item in rows if item["name"] == name]
    require(len(matches) == 1, "exact run/attempt artifact missing or ambiguous")
    artifact = matches[0]
    require(artifact["expired"] is False and artifact["workflow_run"]["id"] == run_id
            and artifact["workflow_run"]["head_sha"] == document["candidate_sha"], "artifact producer identity differs")
    require(artifact["digest"] == f"sha256:{digest(archive)}"
            and artifact["size_in_bytes"] == archive.stat().st_size, "downloaded ZIP digest/size differs")
    return {"artifact_id": artifact["id"], "archive_sha256": digest(archive)}


def validate_archives(expected, selections):
    require(len(selections) == len(LANES), "exactly five explicitly selected archives required")
    proofs, paths = {}, []
    with tempfile.TemporaryDirectory(prefix="luminumbra-qualification-") as temporary:
        for lane, run_text, attempt_text, archive_text in selections:
            require(lane in LANES and lane not in proofs, "unknown or duplicate archive lane")
            require(re.fullmatch(r"[1-9][0-9]*", run_text) and re.fullmatch(r"[1-9][0-9]*", attempt_text),
                    "explicit positive run ID and attempt required")
            archive = Path(archive_text)
            require(archive.stat().st_size <= 1024 * 1024 * 1024, "qualification ZIP exceeds 1 GiB")
            original_digest = digest(archive)
            with zipfile.ZipFile(archive) as zipped:
                entries = zipped.infolist()
                names = [item.filename for item in entries]
                require(len(set(names)) == len(names), "duplicate ZIP member")
                require(sum(item.file_size for item in entries) <= 2 * 1024 * 1024 * 1024,
                        "expanded qualification archive exceeds 2 GiB")
                require("qualification-capture.json" in names, "ZIP has no root manifest")
                require(zipped.getinfo("qualification-capture.json").file_size <= 1024 * 1024, "manifest too large")
                document = json.loads(zipped.read("qualification-capture.json").decode("utf-8-sig"),
                                      object_pairs_hook=unique_object)
                sha(document["candidate_sha"])
                require(document["candidate_sha"] == expected, "selected archive is a different candidate")
                proofs[lane] = verify_archive(lane, int(run_text), int(attempt_text), archive, document)
                expected_names = {"qualification-capture.json", *document["files"]}
                require(set(name for name in names if not name.endswith("/")) == expected_names,
                        "ZIP contains unknown or missing evidence")
                destination = Path(temporary) / lane
                destination.mkdir()
                for name in expected_names:
                    target = (destination / name).resolve()
                    require(target.is_relative_to(destination.resolve()), "ZIP member escapes capture directory")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with zipped.open(name) as source, target.open("xb") as output:
                        for block in iter(lambda: source.read(1024 * 1024), b""):
                            output.write(block)
                paths.append(destination / "qualification-capture.json")
            require(digest(archive) == original_digest, "ZIP changed during validation")
        result = validate_chain(expected, paths)
        for lane, proof in proofs.items():
            result["manifests"][lane].update(proof)
        return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    for name in ("preflight", "write", "asan-shard"):
        sub = commands.add_parser(name)
        sub.add_argument("--lane", choices=tuple(LANES), required=True)
        sub.add_argument("--expected-sha", required=True)
        if name in ("write", "asan-shard"):
            sub.add_argument("--binary", type=Path, required=name == "asan-shard")
            sub.add_argument("--output", type=Path, required=True)
            sub.add_argument("--junit", type=Path)
            sub.add_argument("--capture", type=Path)
            sub.add_argument("--proposal", type=Path)
            sub.add_argument("--check", type=Path)
            sub.add_argument("--baseline", type=Path)
            sub.add_argument("--asan-shards", type=Path)
        if name == "asan-shard":
            sub.add_argument("--shard", type=int, choices=(1, 2), required=True)
    sub = commands.add_parser("validate-chain")
    sub.add_argument("--expected-sha", required=True)
    sub.add_argument("--artifact", nargs=4, action="append", required=True,
                     metavar=("LANE", "RUN_ID", "ATTEMPT", "ZIP"))
    sub.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.mode == "preflight":
            preflight(args.lane, args.expected_sha)
        elif args.mode == "write":
            write_manifest(args)
        elif args.mode == "asan-shard":
            write_asan_shard(args)
        else:
            result = validate_archives(args.expected_sha, args.artifact)
            with args.output.open("x", encoding="utf-8") as stream:
                json.dump(result, stream, sort_keys=True, indent=2)
                stream.write("\n")
        return 0
    except (ValueError, OSError, KeyError, TypeError, AttributeError, ImportError, UnicodeError, zipfile.BadZipFile, ET.ParseError, subprocess.SubprocessError) as error:
        print(f"Qualification refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
