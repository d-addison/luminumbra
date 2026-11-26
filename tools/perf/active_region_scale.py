#!/usr/bin/env python3
"""Capture and summarize bounded synthetic region/save costs; never enforce budgets."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fixture_paths import open_regular
from resolved_fixture import resolved_fixture_hash


SCHEDULER = ("cold", "warm", "anchor_change", "reduce", "freeze", "idle_evict")
SAVES = ("first", "unchanged", "sparse_edit")
LEDGER_COUNTS = ("records", "populated_records", "empty_unowned_records", "encoded_bytes",
                 "record_value_bytes", "priority_comparisons", "due", "transitions", "evicted")
SAVE_COUNTS = ("chunks", "payload_value_bytes", "ledger_records", "ledger_encoded_bytes",
               "snapshot_copies", "world_decodes",
               "chunk_decodes", "owners_serialized", "clean_siblings_serialized",
               "owners_rewritten", "regions_rewritten", "bytes_compressed")
MAX_RECORDS = 2**19
MAX_CHUNKS = 4096
MAX_RUNS = 128
IDENTITY_KEYS = ("revision", "source_dirty", "binary_sha256", "fixture_hash", "recipe")
RECIPE = "luminumbra.active_region_scale_fixture.v1"


def integer(value, lower=0, upper=2**64 - 1):
    return type(value) is int and lower <= value <= upper


def validate_identity(identity):
    if not isinstance(identity, dict) or set(identity) != set(IDENTITY_KEYS):
        raise ValueError("missing measurement identity")
    for key, width in (("revision", 40), ("binary_sha256", 64), ("fixture_hash", 64)):
        value = identity[key]
        if not isinstance(value, str) or re.fullmatch(f"[0-9a-f]{{{width}}}", value) is None:
            raise ValueError(f"invalid identity {key}")
    if type(identity["source_dirty"]) is not bool or identity["recipe"] != RECIPE:
        raise ValueError("invalid identity configuration")


def validate_sample(sample, records, chunks):
    if (not isinstance(sample, dict)
            or sample.get("schema") != "luminumbra.active_region_scale_samples.v1"
            or not integer(sample.get("records_requested"), 1, MAX_RECORDS)
            or not integer(sample.get("chunks_requested"), 1, MAX_CHUNKS)
            or sample["records_requested"] != records or sample["chunks_requested"] != chunks):
        raise ValueError("sample workload identity mismatch")
    for group, phases, counts in (("scheduler", SCHEDULER, LEDGER_COUNTS),
                                  ("saves", SAVES, SAVE_COUNTS)):
        observations = sample.get(group)
        if not isinstance(observations, dict) or set(observations) != set(phases):
            raise ValueError(f"missing or unknown {group} sample")
        for phase in phases:
            row = observations[phase]
            if not isinstance(row, dict) or any(not integer(row.get(key)) for key in counts):
                raise ValueError("invalid operation or memory count")
            duration = row.get("elapsed_ms")
            if (type(duration) not in (float, int) or not 0 <= duration <= 1e12
                    or not math.isfinite(duration)):
                raise ValueError("invalid elapsed sample")
            if group == "scheduler":
                if (row["records"] != row["populated_records"] + row["empty_unowned_records"]
                        or row["records"] > records + 16
                        or row["populated_records"] != (records + 1) // 2
                        or not isinstance(row.get("digest"), str)
                        or re.fullmatch("[0-9a-f]{16}", row["digest"]) is None):
                    raise ValueError("invalid ledger sample")
            elif row["chunks"] != chunks or row["ledger_records"] < records:
                raise ValueError("missing durable payload samples")


def distribution(values):
    ordered = sorted(values)
    def percentile(fraction):
        position = (len(ordered) - 1) * fraction
        lower = math.floor(position)
        upper = math.ceil(position)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)
    return {"p50": percentile(.50), "p95": percentile(.95), "p99": percentile(.99),
            "maximum": ordered[-1]}


def validate_matrix(sizes, chunks, repeats):
    if (not isinstance(sizes, list) or not sizes or len(sizes) > 16
            or any(not integer(size, 1, MAX_RECORDS) for size in sizes)
            or len(set(sizes)) != len(sizes) or not integer(chunks, 1, MAX_CHUNKS)
            or not integer(repeats, 1, 32) or len(sizes) * repeats > MAX_RUNS):
        raise ValueError("invalid measurement matrix")


def build_report(runs, sizes, chunks, repeats):
    validate_matrix(sizes, chunks, repeats)
    if not isinstance(runs, list) or len(runs) != len(sizes) * repeats:
        raise ValueError("missing measurement samples")
    identity = None
    by_size = {size: {} for size in sizes}
    for run in runs:
        if not isinstance(run, dict):
            raise ValueError("invalid run")
        validate_identity(run.get("identity"))
        if identity is not None and run["identity"] != identity:
            raise ValueError("measurement identity mismatch")
        identity = run["identity"]
        size, repeat = run.get("records"), run.get("repeat")
        if (not integer(size, 1, MAX_RECORDS) or size not in by_size
                or not integer(repeat, 0, repeats - 1) or repeat in by_size[size]):
            raise ValueError("duplicate or unknown measurement sample")
        validate_sample(run.get("sample"), size, chunks)
        by_size[size][repeat] = run["sample"]
    summaries = []
    for size in sorted(sizes):
        samples = by_size[size]
        if len(samples) != repeats:
            raise ValueError("missing measurement sample")
        for group, phases, counts in (("scheduler", SCHEDULER, LEDGER_COUNTS),
                                      ("saves", SAVES, SAVE_COUNTS)):
            for phase in phases:
                rows = [samples[index][group][phase] for index in range(repeats)]
                if group == "scheduler":
                    deterministic = (*counts, "digest")
                    if any(any(row[key] != rows[0][key] for key in deterministic) for row in rows):
                        raise ValueError("scheduler observations did not reproduce")
                # Cache occupancy can change sibling serialization counts across processes;
                # retain every integer observation rather than silently choosing one.
                summaries.append({"records": size, "chunks": chunks, "group": group,
                                  "phase": phase, "samples": repeats,
                                  "elapsed_ms": distribution([row["elapsed_ms"] for row in rows]),
                                  "observations": {key: [row[key] for row in rows] for key in counts},
                                  **({"digests": [row["digest"] for row in rows]}
                                     if group == "scheduler" else {})})
    return {"schema": "luminumbra.active_region_scale_report.v1", "identity": identity,
            "sizes": sorted(sizes), "chunks": chunks, "repeats": repeats,
            "summaries": summaries}


def file_digest(path):
    digest = hashlib.sha256()
    with open_regular(path.parent, path) as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate measurement JSON key")
        result[key] = value
    return result


def measurement_identity(root, binary):
    return {"revision": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
            "source_dirty": bool(subprocess.check_output(
                ["git", "status", "--porcelain"], cwd=root, text=True)),
            "binary_sha256": file_digest(binary),
            "fixture_hash": resolved_fixture_hash(root), "recipe": RECIPE}


def capture(binary, root, evidence, sizes, chunks, repeats):
    # Validate the entire bounded matrix and content identity before creating evidence.
    validate_matrix(sizes, chunks, repeats)
    if evidence.resolve().is_relative_to(root.resolve()):
        raise ValueError("evidence must be outside the source tree")
    identity = measurement_identity(root, binary)
    validate_identity(identity)
    evidence.mkdir()  # Exclusive leaf; never reuse or clean someone else's directory.
    runs = []
    for size in sizes:
        for repeat in range(repeats):
            command = [str(binary), "--root", str(root), "--work-dir",
                       str(evidence / f"fixture-{size}-{repeat}"),
                       "--records", str(size), "--chunks", str(chunks)]
            completed = subprocess.run(command, cwd=root, check=True, capture_output=True,
                                       text=True, encoding="utf-8")
            # Producer output is bounded by nine fixed phase rows, independent of ledger size.
            if len(completed.stdout) > 65536:
                raise ValueError("oversized measurement output")
            sample = json.loads(completed.stdout, object_pairs_hook=unique_object)
            validate_sample(sample, size, chunks)
            run = {"identity": identity, "records": size, "repeat": repeat,
                   "command": command, "sample": sample}
            runs.append(run)
            with (evidence / f"sample-{size}-{repeat}.json").open("x", encoding="utf-8") as output:
                json.dump(run, output, sort_keys=True, allow_nan=False)
                output.write("\n")
    if measurement_identity(root, binary) != identity:
        raise ValueError("measurement identity changed during capture")
    report = build_report(runs, sizes, chunks, repeats)
    pending_report = evidence / ".report.pending"
    with pending_report.open("x", encoding="utf-8") as output:
        json.dump(report, output, sort_keys=True, indent=2, allow_nan=False)
        output.write("\n")
    # Close the payload before renaming, including on Windows. A failed write or
    # close leaves diagnostic partial evidence without publishing a success record.
    pending_report.replace(evidence / "report.json")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--sizes", type=int, nargs="+", default=[8, 32])
    parser.add_argument("--chunks", type=int, default=4)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    try:
        capture(args.binary.resolve(), args.root.resolve(), args.evidence_dir.resolve(),
                args.sizes, args.chunks, args.repeats)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Measurement refused: {error}; partial evidence retained\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
