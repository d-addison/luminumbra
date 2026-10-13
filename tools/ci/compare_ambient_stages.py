#!/usr/bin/env python3
"""Diff four downloaded ambient stage artifacts without decimal float conversion."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


LANES = ("linux-gcc", "asan", "windows-msvc", "windows-ucrt64")
SCHEMA = "luminumbra.ambient-stage-diagnostic.v1"
MAX_BYTES = 16 * 1024 * 1024


def load(path: Path) -> tuple[dict, dict]:
    if path.stat().st_size > MAX_BYTES:
        raise ValueError(f"artifact exceeds fixed size limit: {path}")
    artifact = json.loads(path.read_text(encoding="utf-8"))
    if artifact["schema"] != SCHEMA or artifact["seed"] != 424242:
        raise ValueError(f"unexpected schema or seed: {path}")
    stages = {}
    for stage in artifact["stages"]:
        key = (stage["tick"], stage["stage"])
        if key in stages or key[0] not in (0, 64):
            raise ValueError(f"duplicate stage or unexpected tick: {path}: {key}")
        width = {"f32le": 4, "bytes": 1}[stage["encoding"]]
        bits = stage["bits"]
        if len(bits) != stage["count"] or any(len(word) != width * 2 for word in bits):
            raise ValueError(f"inconsistent dump length: {path}: {key}")
        value = 14695981039346656037
        for word in bits:
            for byte in int(word, 16).to_bytes(width, "little"):
                value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
        if f"{value:016x}" != stage["fnv1a64"]:
            raise ValueError(f"canonical hash does not match dump: {path}: {key}")
        stages[key] = stage
    if not stages or {key[0] for key in stages} != {0, 64}:
        raise ValueError(f"missing capture ticks: {path}")
    return artifact, stages


def compare(baseline: tuple[dict, dict], candidate: tuple[dict, dict], lane: str) -> bool:
    reference, reference_stages = baseline
    artifact, stages = candidate
    ref_identity = reference["identity"]
    identity = artifact["identity"]
    if identity["revision"] != ref_identity["revision"]:
        raise ValueError(f"{lane}: revision differs from linux-gcc baseline")
    print(f"{lane}: {identity['compiler']} {identity['compiler_version']} "
          f"revision={identity['revision']}")
    different = False
    for field in ("selected_isa", "ceiling"):
        if identity[field] != ref_identity[field]:
            print(f"  identity {field}: linux-gcc={ref_identity[field]} {lane}={identity[field]}")
            different = True
    for field in ("mxcsr_before", "mxcsr_tick0", "mxcsr_tick64", "mxcsr_after"):
        # Sticky exception bits in raw MXCSR are evidence, not FP mode settings.
        for mode in ("rounding", "ftz", "daz"):
            if identity[field].get(mode) != ref_identity[field].get(mode):
                print(f"  {field}.{mode}: linux-gcc={ref_identity[field].get(mode)} "
                      f"{lane}={identity[field].get(mode)}")
                different = True
    if stages.keys() != reference_stages.keys():
        raise ValueError(f"{lane}: stage inventory differs from linux-gcc baseline")
    if artifact["receipts"] != reference["receipts"]:
        print("  final receipts differ")
        different = True
    for key, expected in reference_stages.items():
        actual = stages[key]
        if actual["encoding"] != expected["encoding"] or actual["count"] != expected["count"]:
            raise ValueError(f"{lane}: stage geometry differs: {key}")
        if actual["bits"] == expected["bits"]:
            continue
        different = True
        index = next(i for i, pair in enumerate(zip(expected["bits"], actual["bits"]))
                     if pair[0] != pair[1])
        print(f"  tick={key[0]} stage={key[1]} first_index={index} "
              f"linux-gcc={expected['bits'][index]} {lane}={actual['bits'][index]} "
              f"hashes={expected['fnv1a64']}/{actual['fnv1a64']}")
    if not different:
        print("  all stage bits, metadata, receipts, selected ISAs and FP modes agree")
    return different


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="download root with one subdirectory per lane")
    args = parser.parse_args()
    try:
        captures = {lane: load(args.directory / lane / "test-artifacts/ambient/stages.json")
                    for lane in LANES}
        baseline = captures["linux-gcc"]
        print(f"linux-gcc baseline: revision={baseline[0]['identity']['revision']}")
        differences = [compare(baseline, captures[lane], lane) for lane in LANES[1:]]
        return int(any(differences))
    except (OSError, ValueError, KeyError, TypeError, OverflowError, StopIteration) as exc:
        print(f"ambient stage diff: invalid evidence: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
