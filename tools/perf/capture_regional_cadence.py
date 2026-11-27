#!/usr/bin/env python3
"""Capture or compare diagnostic regional smoke matrices; no budget proposals."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_sim_budget as budget
from fixture_paths import decode_text, open_regular, read_bytes
from resolved_fixture import PRESETS, resolved_fixture_hash, unique_object
import render_runner_process as process


SCHEMA = "luminumbra.regional_cadence_capture.v2"
LEGACY_SCHEMA = "luminumbra.regional_cadence_capture.v1"
WORKLOAD = "eight_distant_regions_v1"
# Capture/parser allocation bounds only, independent of shipping admission.
MAX_CELLS, MAX_TICKS = 32, 36000
MAX_FILE, MAX_TOTAL, MAX_BINARY = 64 * 1024**2, 512 * 1024**2, 512 * 1024**2
# Existing production water cap, including preparation and both seam endpoints.
WATER_CELL_CAP = 4096
COUNTERS = (
    "tick", "observations", "owner_visits", "water_window_visits",
    "replay_cell_visits_last", "replay_cell_visits_max", "preparation_pages_last",
    "preparation_pages_max", "replay_remaining_steps", "water_publications",
    "water_deferred_samples", "water_granted_windows", "water_border_windows",
    "water_completed_cells", "water_preparation_cells", "water_total_cells_max",
    "water_resident_visits", "water_participant_visits",
)
OWNER_COUNTERS = (
    "plant_services", "elapsed_steps", "last_plant_tick", "max_service_gap", "service_gap",
    "frozen_samples", "unadmitted_samples", "frozen_services", "missing_samples",
    "water_services", "water_interior_windows", "water_service_gap", "max_water_service_gap",
    "frozen_water_windows",
)
SUB_HASHES = ("terrain", "water", "entities", "wind", "weather", "aether",
              "aether_state", "scents", "ecology", "plants")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def digest(value):
    return hashlib.sha256(canonical(value).encode("utf-8")).hexdigest()


def read_json(path, limit=MAX_FILE):
    return read_packet(path, limit)[0]


def read_packet(path, limit):
    raw = read_bytes(path.parent, path, limit=limit)
    return (json.loads(decode_text(raw, encoding="utf-8-sig"), object_pairs_hook=unique_object), len(raw))


def file_digest(path):
    checksum = hashlib.sha256()
    with open_regular(path.parent, path) as stream:
        require(os.fstat(stream.fileno()).st_size <= MAX_BINARY, "executable exceeds capture bound")
        remaining = MAX_BINARY
        while chunk := stream.read(min(1024**2, remaining + 1)):
            remaining -= len(chunk)
            require(remaining >= 0, "executable exceeds capture bound")
            checksum.update(chunk)
    return checksum.hexdigest()


def matrix(presets, seeds, repeats, ticks, radius, collision_radius):
    require(isinstance(presets, list) and 0 < len(presets) <= len(PRESETS)
            and all(preset in PRESETS for preset in presets)
            and len(set(presets)) == len(presets), "invalid preset matrix")
    require(isinstance(seeds, list) and 0 < len(seeds) <= MAX_CELLS
            and all(isinstance(seed, str) and re.fullmatch(r"[0-9]{1,20}", seed)
                    and int(seed) < 2**64 for seed in seeds)
            and len({int(seed) for seed in seeds}) == len(seeds), "invalid seed matrix")
    require(type(repeats) is int and 2 <= repeats <= MAX_CELLS, "matrix requires repeated runs")
    require(len(presets) * len(seeds) * repeats <= MAX_CELLS, "capture matrix exceeds cell bound")
    require(type(ticks) is int and 1 <= ticks <= MAX_TICKS, "invalid capture tick count")
    require(all(type(value) is int and 0 <= value <= 12 for value in (radius, collision_radius)),
            "invalid surface/collision radius")
    return {"workload": WORKLOAD, "anchors": 8, "participants": 9,
            "presets": presets, "seeds": seeds, "repeats": repeats, "ticks": ticks,
            "surface_radius": radius, "collision_radius": collision_radius,
            "autosave_ticks": 30, "no_audio": True}


def command(binary, root, artifact, workload, preset, seed):
    def generic(path):
        return path.as_posix() if isinstance(path, Path) else path
    return [generic(binary), "--root", generic(root), "--smoke", "--no-audio", "--sim-budget",
            "--regional-cadence-workload", "--preset", preset, "--seed", seed,
            "--ticks", str(workload["ticks"]), "--autosave-ticks", "30",
            "--radius", str(workload["surface_radius"]), "--collision-radius",
            str(workload["collision_radius"]), "--artifact", generic(artifact)]


def cells(workload):
    # Product validated before iteration; at most MAX_CELLS process launches.
    for preset in workload["presets"]:
        for seed in workload["seeds"]:
            for repeat in range(workload["repeats"]):
                yield preset, seed, repeat


def validate_diagnostics(data, ticks):
    require(isinstance(data, dict), "missing regional diagnostic run")
    require(all(budget.integer(data.get(key)) for key in COUNTERS), "invalid regional counter")
    result = {key: data[key] for key in COUNTERS}
    require(data["observations"] == ticks and data["tick"] >= ticks
            and data["owner_visits"] == ticks * 9, "incomplete regional observations")
    require(data["water_publications"] + data["water_deferred_samples"] == ticks,
            "incomplete water observations")
    require(data["water_total_cells_max"] <= WATER_CELL_CAP
            and data["water_completed_cells"] + data["water_preparation_cells"]
            <= data["water_publications"] * data["water_total_cells_max"], "exceeded water cell cap")
    require(data["replay_cell_visits_last"] <= data["replay_cell_visits_max"]
            and data["preparation_pages_last"] <= data["preparation_pages_max"],
            "invalid replay/page maxima")
    owners = data.get("owners")
    require(isinstance(owners, list) and len(owners) == 9, "incomplete regional owners")
    result["owners"] = []
    for index, owner in enumerate(owners):
        require(isinstance(owner, dict)
                and all(type(owner.get(key)) is int and -(2**31) <= owner[key] < 2**31
                        for key in ("x", "z"))
                and all(budget.integer(owner.get(key)) for key in OWNER_COUNTERS),
                "invalid owner counters")
        require(owner["x"] == owners[0]["x"] + (index % 3) * 4
                and owner["z"] == owners[0]["z"] + (index // 3) * 4, "wrong owner layout")
        require(owner["missing_samples"] == 0, "missing workload participant")
        require(owner["frozen_services"] == 0 and owner["frozen_water_windows"] == 0,
                "frozen owner advanced")
        require(all(owner[key] <= ticks for key in (
            "plant_services", "water_services", "max_service_gap", "max_water_service_gap"))
            and owner["service_gap"] <= owner["max_service_gap"]
            and owner["water_service_gap"] <= owner["max_water_service_gap"]
            and owner["last_plant_tick"] <= data["tick"]
            and owner["frozen_samples"] + owner["unadmitted_samples"] <= ticks
            and max(owner["plant_services"], owner["water_services"])
            <= ticks - owner["frozen_samples"] - owner["unadmitted_samples"],
            "inconsistent owner service")
        if index < 8:
            require(owner["plant_services"] > 0 and owner["elapsed_steps"] > 0
                    and owner["water_services"] > 0 and owner["water_interior_windows"] > 0,
                    "missing remote plant/resource or water work")
        else:
            require(owner["frozen_samples"] + owner["unadmitted_samples"] == ticks
                    and all(owner[key] == 0 for key in (
                        "plant_services", "elapsed_steps", "water_services", "water_interior_windows")),
                    "unanchored frozen control advanced or unobserved")
        result["owners"].append({key: owner[key] for key in ("x", "z", *OWNER_COUNTERS)})
    require(data["water_completed_cells"] > 0 and data["water_granted_windows"] > 0,
            "missing water work")
    # The visit counter includes interiors outside the nine observed owners.
    require(data["water_window_visits"] % 9 == 0
            and sum(owner["water_interior_windows"] for owner in owners) * 9
            <= data["water_window_visits"], "inconsistent water visits")
    return result


def validate_artifact(data, workload, preset, seed):
    ticks = workload["ticks"]
    require(isinstance(data, dict), "artifact must be an object")
    budget.validate_smoke_identity(data, preset, seed, ticks)
    require(all(data.get(key) is True for key in ("deterministic", "sub_hashes_match")),
            "divergent deterministic smoke")
    require(all(type(data.get(key)) is int and data[key] == workload[key]
                for key in ("surface_radius", "collision_radius")), "artifact radius mismatch")
    gates = data["effective_simulation"]
    require(all(type(value) is bool for value in gates.values()), "invalid effective gates")
    runs = data.get("runs")
    require(isinstance(runs, list) and len(runs) == 2, "missing smoke runs")
    hashes = []
    for run in runs:
        require(isinstance(run, dict) and run.get("ok") is True
                and type(run.get("ticks_executed")) is int and run["ticks_executed"] == ticks
                and run.get("world_hash") == data["world_hash"], "incomplete smoke run")
        sub = run.get("sub_hashes")
        # Native truth strings include empty-neutral systems and labelled water cursors.
        require(isinstance(sub, dict)
                and all(isinstance(sub.get(key), str) for key in SUB_HASHES), "missing truth hashes")
        hashes.append({key: sub[key] for key in SUB_HASHES})
    require(hashes[0] == hashes[1], "divergent deterministic sub-hashes")
    telemetry = data.get("sim_budget")
    require(isinstance(telemetry, dict) and telemetry.get("schema") == "luminumbra.sim_budget.v2"
            and telemetry.get("work_replay_match") is True, "divergent or missing work trace")
    budget.validate_work_items(telemetry.get("work_items"), ticks)
    stages = telemetry.get("stages")
    require(isinstance(stages, list) and len(stages) == len(budget.STAGES)
            and all(isinstance(stage, dict) for stage in stages)
            and [stage.get("name") for stage in stages] == list(budget.STAGES), "incomplete stages")
    combined = stages[budget.STAGES.index("wind_weather")].get("samples") == ticks
    inactive = {"wind", "weather"} if combined else {"wind_weather"}
    durations, work = {}, {}
    for stage in stages:
        name = stage["name"]
        expected = 0 if name in inactive else ticks
        require(type(stage.get("samples")) is int and stage["samples"] == expected
                and budget.integer(stage.get("work_total")), "incomplete stage samples")
        trace = stage.get("work_trace")
        require(isinstance(trace, list) and len(trace) == expected, "incomplete stage trace")
        for tick, sample in enumerate(trace, 1):
            require(isinstance(sample, dict) and type(sample.get("tick")) is int
                    and sample["tick"] == tick and budget.integer(sample.get("work")),
                    "invalid stage sample")
        require(sum(sample["work"] for sample in trace) == stage["work_total"], "wrong stage total")
        if name == "server_water":
            require(all(sample["work"] <= WATER_CELL_CAP for sample in trace), "exceeded water cell cap")
        duration = stage.get("duration_ms")
        if expected:
            require(isinstance(duration, dict), "missing stage durations")
            values = [duration.get(key) for key in ("p50", "p95", "p99", "maximum")]
            require(all(type(value) in (int, float) and 0 <= value <= sys.float_info.max
                        and math.isfinite(value)
                        for value in values) and values == sorted(values), "invalid stage durations")
        else:
            require("duration_ms" in stage and duration is None, "inactive stage has durations")
        durations[name] = duration
        work[name] = {"total": stage["work_total"], "trace": [s["work"] for s in trace]}
    for counter in telemetry["work_items"]["counters"]:
        if counter["name"] == "water_cells":
            require([sample["work"] for sample in counter["work_trace"]] == work["server_water"]["trace"],
                    "inconsistent water cell trace")
    regional = data.get("regional_cadence")
    require(isinstance(regional, dict) and regional.get("schema") == "luminumbra.regional_cadence.v1"
            and regional.get("workload") == WORKLOAD
            and type(regional.get("anchors")) is int and regional["anchors"] == 8
            and type(regional.get("participants")) is int and regional["participants"] == 9,
            "mismatched workload identity")
    first = validate_diagnostics(regional.get("run"), ticks)
    replay = validate_diagnostics(regional.get("replay"), ticks)
    require(regional.get("observations_replay_match") is True and first == replay,
            "divergent deterministic regional traces")
    work_items = {"schema": "luminumbra.sim_work.v1", "counters": [
        {"name": counter["name"], "work_trace": [{"tick": sample["tick"], "work": sample["work"]}
         for sample in counter["work_trace"]]} for counter in telemetry["work_items"]["counters"]]}
    return {"integer": {"regional": first, "stages": work,
                        "work_items": work_items, "effective_simulation": gates,
                        "world_hash": data["world_hash"], "sub_hashes": hashes[0]},
            "duration_ms": durations}


def identities(repository, root, binary, runner):
    def git(*options):
        return runner(["git", *options], cwd=repository, check=True, capture_output=True,
                      text=True, timeout=30).stdout.strip()
    revision = git("rev-parse", "HEAD")
    require(re.fullmatch(r"[0-9a-f]{40}", revision) is not None, "invalid source revision")
    return {"revision": revision, "source_dirty": bool(git("status", "--porcelain", "--untracked-files=all")),
            "binary_sha256": file_digest(binary), "content_sha256": resolved_fixture_hash(root)}


def write_new(path, value):
    raw = (canonical(value) + "\n").encode("utf-8")
    with path.open("xb") as stream:
        require(stream.write(raw) == len(raw), "short evidence write")
        stream.flush()


def path_labels(identity):
    return {"binary": "binary-sha256-" + identity["binary_sha256"],
            "runtime_root": "runtime-sha256-" + identity["content_sha256"],
            "capture_root": "."}


def capture(args, runner=None):
    runner = subprocess.run if runner is None else runner
    workload = matrix(list(args.presets), list(args.seeds), args.repeats, args.ticks,
                      args.radius, args.collision_radius)
    repository, root = args.repository.resolve(strict=True), args.root.resolve(strict=True)
    binary = args.binary.resolve(strict=True)
    evidence = args.evidence_dir.resolve()
    require(all(not evidence.is_relative_to(path) and not path.is_relative_to(evidence)
                for path in (repository, root)), "evidence must be outside repository and runtime root")
    identity = identities(repository, root, binary, runner)
    evidence.mkdir(parents=True)  # Exclusive; an existing partial matrix is never reused.
    manifest = {"schema": SCHEMA, "identity": identity, "workload": workload,
                "workload_sha256": digest(workload), **path_labels(identity), "cells": []}
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith("LUMINUMBRA_")}
    total = 0
    previous = {}
    for index, (preset, seed, repeat) in enumerate(cells(workload)):
        name = f"cell-{index}.json"
        argv = command(binary, root, evidence / name, workload, preset, seed)
        status = {"evidence_errors": []}
        try:
            # Shared pipe drainer retains at most 16 MiB, with bounded shutdown
            # draining on POSIX and Windows; no child writes directly to disk.
            with process.bounded_log(evidence / f"cell-{index}.log", status, exclusive=True) as log:
                completed = runner(argv, cwd=evidence, env=environment.copy(), stdout=log,
                                   stderr=subprocess.STDOUT, check=False, timeout=600)
        finally:
            # Preserve safe diagnostics even on spawn failure or deadline expiry.
            write_new(evidence / f"cell-{index}.log.json", {
                key: status.get(key) for key in (
                    "log_truncated", "log_limit_bytes", "log_bytes_retained")
            } | {"log_complete": status.get("log_truncated") is False and not status["evidence_errors"]})
        require(not status["evidence_errors"], "incomplete smoke log (truncation or I/O failure)")
        require(type(completed.returncode) is int and completed.returncode == 0, "unsuccessful smoke process")
        data, size = read_packet(evidence / name, min(MAX_FILE, MAX_TOTAL - total))
        # Canonical digest binds exactly the parsed object; never reopen raw evidence for hashing.
        total += size
        require(total <= MAX_TOTAL, "capture exceeds total artifact bound")
        summary = validate_artifact(data, workload, preset, seed)
        trace_digest = digest(summary["integer"])
        require(previous.get((preset, seed), trace_digest) == trace_digest,
                "divergent deterministic repeated traces")
        previous[preset, seed] = trace_digest
        manifest["cells"].append({"preset": preset, "seed": seed, "repeat": repeat,
                                  "artifact": name, "artifact_sha256": digest(data),
                                  "command": command(manifest["binary"], manifest["runtime_root"],
                                                     name, workload, preset, seed)})
    require(identity == identities(repository, root, binary, runner), "capture identity changed")
    # Revalidate retained bytes, commands and the complete matrix before publishing the manifest last.
    load_capture(evidence, manifest)
    write_new(evidence / "capture.pending", manifest)
    (evidence / "capture.pending").rename(evidence / "capture.json")
    return manifest


def load_capture(directory, manifest=None):
    manifest = read_json(directory / "capture.json") if manifest is None else manifest
    require(isinstance(manifest, dict) and manifest.get("schema") in (SCHEMA, LEGACY_SCHEMA),
            "unsupported capture schema")
    value = manifest.get("workload")
    require(isinstance(value, dict), "missing workload identity")
    workload = matrix(value.get("presets"), value.get("seeds"), value.get("repeats"), value.get("ticks"),
                      value.get("surface_radius"), value.get("collision_radius"))
    require(canonical(value) == canonical(workload) and manifest.get("workload_sha256") == digest(workload),
            "mismatched workload identity")
    identity = manifest.get("identity")
    require(isinstance(identity, dict) and type(identity.get("source_dirty")) is bool
            and isinstance(identity.get("revision"), str)
            and re.fullmatch(r"[0-9a-f]{40}", identity["revision"]) is not None, "invalid source identity")
    require(all(isinstance(identity.get(key), str) and re.fullmatch(r"[0-9a-f]{64}", identity[key])
                for key in ("binary_sha256", "content_sha256")), "invalid executable/content identity")
    labels = path_labels(identity)
    bindings = {key: manifest.get(key) for key in labels}
    # Accept legacy absolute bindings for comparison, but publish only safe roles.
    legacy_paths = all(isinstance(manifest.get(key), str) and manifest[key] and "\0" not in manifest[key]
                and (PurePosixPath(manifest[key]).is_absolute() or PureWindowsPath(manifest[key]).is_absolute())
                for key in ("binary", "runtime_root", "capture_root"))
    require(legacy_paths if manifest["schema"] == LEGACY_SCHEMA else bindings == labels,
            "invalid capture paths")
    rows = manifest.get("cells")
    require(isinstance(rows, list) and len(rows) == len(workload["presets"]) * len(workload["seeds"])
            * workload["repeats"], "incomplete capture matrix")
    result, published_rows, previous, total = [], [], {}, 0
    for index, ((preset, seed, repeat), row) in enumerate(zip(cells(workload), rows)):
        name = f"cell-{index}.json"
        require(isinstance(row, dict) and row.get("preset") == preset and row.get("seed") == seed
                and type(row.get("repeat")) is int and row["repeat"] == repeat
                and row.get("artifact") == name, "invalid matrix cell")
        require(row.get("command") == command(manifest["binary"], manifest["runtime_root"],
                Path(manifest["capture_root"]) / name, workload, preset, seed), "capture command mismatch")
        data, size = read_packet(directory / name, min(MAX_FILE, MAX_TOTAL - total))
        total += size
        require(total <= MAX_TOTAL, "capture exceeds total artifact bound")
        require(row.get("artifact_sha256") == digest(data), "artifact digest mismatch")
        summary = validate_artifact(data, workload, preset, seed)
        checksum = digest(summary["integer"])
        require(previous.get((preset, seed), checksum) == checksum, "divergent deterministic repeated traces")
        previous[preset, seed] = checksum
        result.append(summary)
        published_rows.append({key: row[key] for key in (
            "preset", "seed", "repeat", "artifact", "artifact_sha256")})
        published_rows[-1]["command"] = command(labels["binary"], labels["runtime_root"], name,
                                               workload, preset, seed)
    # Project known fields so legacy path bindings and unknown metadata cannot
    # leak through either endpoint of the comparison report.
    published = {"schema": SCHEMA, **labels, "workload": workload,
                 "workload_sha256": digest(workload), "cells": published_rows,
                 "identity": {key: identity[key] for key in (
                     "revision", "source_dirty", "binary_sha256", "content_sha256")}}
    return {"manifest": published, "cells": result}


def compare(before, after):
    left, right = load_capture(before), load_capture(after)
    require(left["manifest"]["workload"] == right["manifest"]["workload"], "mismatched workload identity")
    require(left["manifest"]["identity"]["content_sha256"] == right["manifest"]["identity"]["content_sha256"],
            "mismatched content identity")
    integer, durations = [], []
    for index, (old, new) in enumerate(zip(left["cells"], right["cells"])):
        require(old["integer"]["effective_simulation"] == new["integer"]["effective_simulation"],
                "mismatched effective simulation")
        integer.append({"cell": index, "same": old["integer"] == new["integer"],
                        "before": old["integer"], "after": new["integer"]})
        durations.append({"cell": index, "before": old["duration_ms"], "after": new["duration_ms"]})
    return {"schema": "luminumbra.regional_cadence_compare.v2",
            "before": left["manifest"], "after": right["manifest"],
            "integer_work_and_service": integer, "duration_ms": durations}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    collect = modes.add_parser("capture")
    collect.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[2])
    for name in ("root", "binary", "evidence-dir"):
        collect.add_argument("--" + name, type=Path, required=True)
    collect.add_argument("--presets", nargs="+", choices=PRESETS, default=list(PRESETS))
    collect.add_argument("--seeds", nargs="+", default=["1337"])
    collect.add_argument("--repeats", type=int, default=2)
    collect.add_argument("--ticks", type=int, default=3600)
    collect.add_argument("--radius", type=int, default=1)
    collect.add_argument("--collision-radius", type=int, default=1)
    comparison = modes.add_parser("compare")
    comparison.add_argument("before", type=Path)
    comparison.add_argument("after", type=Path)
    args = parser.parse_args(argv)
    try:
        result = capture(args) if args.mode == "capture" else compare(args.before, args.after)
        print(canonical(result))
    except (OSError, ValueError, TypeError, KeyError, AttributeError, RecursionError,
            subprocess.SubprocessError) as error:
        print(f"Regional cadence {args.mode} refused: {error}; partial evidence is unqualified", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
