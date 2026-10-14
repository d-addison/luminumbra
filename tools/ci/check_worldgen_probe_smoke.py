#!/usr/bin/env python3
"""Run the worldgen smoke probe and validate its report without speed limits."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import sys

SCHEMA = "luminumbra.worldgen_probe.v1"
# Absolute metres only: allow C++ float rounding of terrain_height + 1.95f.
SPAWN_Y_ABS_TOLERANCE = 1e-5
PHASES = (
    "generate_step1", "generate_step4", "polygonise",
    "height_query_cold", "height_query_warm", "shaped_heights",
)
LIBM_FUNCTIONS = (
    "sinf", "cosf", "powf", "expf", "sqrtf", "atan2f", "floorf",
    "lroundf", "fmaf", "hypotf",
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def object_value(value, label):
    require(isinstance(value, dict), f"{label} must be an object")
    return value


def integer(value, label, minimum=0):
    require(type(value) is int and value >= minimum,
            f"{label} must be an integer >= {minimum}")
    return value


def number(value, label, nonnegative=True):
    require(type(value) in (int, float), f"{label} must be a number")
    try:
        finite = math.isfinite(value)
    except OverflowError:
        finite = False
    require(finite and (not nonnegative or value >= 0),
            f"{label} must be finite" + (" and nonnegative" if nonnegative else ""))
    return value


def text(value, label):
    require(isinstance(value, str) and bool(value.strip()), f"{label} must be nonempty text")


def stats(value, label, samples):
    value = object_value(value, label)
    require(integer(value.get("samples"), f"{label}.samples", 1) == samples,
            f"{label}.samples must be {samples}")
    for field in ("min_ms", "median_ms", "max_ms", "total_ms"):
        number(value.get(field), f"{label}.{field}")
    require(value["min_ms"] <= value["median_ms"] <= value["max_ms"] <= value["total_ms"],
            f"{label} timing order is inconsistent")


def validate_report(report):
    report = object_value(report, "report")
    require(report.get("schema") == SCHEMA, "unexpected schema")
    require(report.get("mode") == "smoke", "mode must be smoke")
    for field in ("compiler", "cpu", "preset"):
        text(report.get(field), field)
    integer(report.get("hardware_concurrency"), "hardware_concurrency")
    integer(report.get("default_worker_count"), "default_worker_count", 1)
    require(integer(report.get("seed"), "seed") == 424242, "unexpected seed")
    require(integer(report.get("requested_chunks"), "requested_chunks", 1) == 2,
            "smoke must request two chunks")
    selected = integer(report.get("selected_chunks"), "selected_chunks", 1)
    candidates = integer(report.get("initial_chunk_list_size"), "initial_chunk_list_size", 1)
    require(selected <= report["requested_chunks"] and selected <= candidates,
            "selected chunk count is inconsistent")
    require(selected == report["requested_chunks"],
            "selected_chunks must match requested_chunks")
    spawn = object_value(report.get("spawn"), "spawn")
    for axis in ("x", "y", "z"):
        number(spawn.get(axis), f"spawn.{axis}", nonnegative=False)
    for axis in ("x", "z"):
        require(spawn[axis] == 8, f"spawn.{axis} must be 8")
    terrain_height = number(report.get("terrain_height"), "terrain_height", nonnegative=False)
    require(math.isclose(spawn["y"], terrain_height + 1.95,
                         rel_tol=0.0, abs_tol=SPAWN_Y_ABS_TOLERANCE),
            "spawn.y must equal terrain_height + 1.95 within 1e-5 metres")

    phases = object_value(report.get("phases"), "phases")
    for phase in PHASES:
        samples = selected if phase in PHASES[:3] else (2 if phase == "shaped_heights" else 1024)
        stats(phases.get(phase), phase, samples)
    integer(phases["polygonise"].get("mesh_triangles"), "polygonise.mesh_triangles", 1)
    for phase in PHASES[3:]:
        require(phases[phase].get("grid") == "32x32", f"{phase}.grid must be 32x32")
    number(phases["height_query_cold"].get("first_call_ms"), "height_query_cold.first_call_ms")
    shaped = phases["shaped_heights"]
    require(integer(shaped.get("positions"), "shaped_heights.positions") == 1024,
            "shaped_heights.positions must be 1024")
    require(integer(shaped.get("reps"), "shaped_heights.reps") == 2,
            "shaped_heights.reps must be two")

    scaling = report.get("scaling")
    require(isinstance(scaling, list) and len(scaling) == 4, "scaling must contain four entries")
    for entry, requested in zip(scaling, (1, 2, 4, "default")):
        entry = object_value(entry, "scaling entry")
        actual_request = entry.get("requested_workers")
        require(type(actual_request) is type(requested) and actual_request == requested,
                "unexpected requested_workers")
        integer(entry.get("worker_count"), "scaling.worker_count", 1)
        require(integer(entry.get("chunks"), "scaling.chunks", 1) == selected,
                "scaling.chunks must match selected_chunks")
        number(entry.get("wall_ms"), "scaling.wall_ms")

    for observation in ("libm", "libm_after_generation"):
        suite = object_value(report.get(observation), observation)
        require(integer(suite.get("calls"), f"{observation}.calls", 1) == 1000,
                f"{observation}.calls must be 1000")
        functions = object_value(suite.get("functions"), f"{observation}.functions")
        for function in LIBM_FUNCTIONS:
            value = object_value(functions.get(function), f"{observation}.{function}")
            require(integer(value.get("calls"), f"{observation}.{function}.calls", 1) == suite["calls"],
                    f"{observation}.{function}.calls must match suite")
            number(value.get("total_ms"), f"{observation}.{function}.total_ms")
            number(value.get("ns_per_call"), f"{observation}.{function}.ns_per_call")


def reject_constant(value):
    raise ValueError(f"nonfinite JSON constant: {value}")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def validate_output(path):
    with Path(path).open(encoding="utf-8") as stream:
        report = json.load(stream, parse_constant=reject_constant, object_pairs_hook=unique_object)
    validate_report(report)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        args.out.unlink(missing_ok=True)
        result = subprocess.run([args.probe, "--smoke", "--json", str(args.out)], check=False)
        if result.returncode:
            return result.returncode if result.returncode > 0 else 128 - result.returncode
        validate_output(args.out)
    except (OSError, ValueError) as error:
        print(f"worldgen smoke check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
