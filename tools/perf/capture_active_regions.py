#!/usr/bin/env python3
"""Retain an enabled worker/replay matrix and edited-tour qualification evidence.

Use a clean --repository, an existing isolated --root containing shipping content,
--binary, --dump-binary, --tour (see check_active_regions.py), and a fresh
--evidence-dir outside both repository and runtime/save root. Tour inputs are
read-only. The tour bundle supplies a copied ledger, export, stage
logs and save receipts; the standalone production exporter verifies that copy.
A failed capture retains diagnostics but never publishes capture.json.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_active_regions as check
from resolved_fixture import resolved_fixture_hash


commands = check.commands


def source_revision(repository, runner):
    result = runner(["git", "rev-parse", "HEAD"], cwd=repository, check=True,
                    capture_output=True, text=True, timeout=30)
    revision = result.stdout.strip()
    check.require(check.hex_string(revision, 40), "invalid source revision")
    status = runner(["git", "status", "--porcelain", "--untracked-files=all"], cwd=repository,
                    check=True, capture_output=True, text=True, timeout=30)
    check.require(not status.stdout.strip(), "source repository must be clean")
    return revision


def identities(repository, root, binary, dump_binary, preset, runner):
    return {"revision": source_revision(repository, runner),
            "binary_sha256": check.digest_file(binary)["sha256"],
            "dump_binary_sha256": check.digest_file(dump_binary)["sha256"],
            "content_sha256": resolved_fixture_hash(root),
            "systems_sha256": check.digest_file(root / "data/common/systems.json")["sha256"],
            "preset_sha256": check.digest_file(root / "worlds/atlas/presets" / (preset + ".json"))["sha256"]}


def outside(path, roots):
    # Resolve before comparing, including Windows short names and directory links.
    resolved = path.resolve()
    check.require(all(not resolved.is_relative_to(root.resolve()) for root in roots),
                  "evidence/runtime destination must be outside repository and save roots")
    return resolved


def write_new(root, name, raw):
    target = check.relative(root, name)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("xb") as output:
        check.require(output.write(raw) == len(raw), "short evidence write")
        output.flush()


def capture(args, runner=None):
    runner = subprocess.run if runner is None else runner
    repository = args.repository.resolve(strict=True)
    root = args.root.resolve(strict=True)
    outside(root, (repository,))
    check.relative(root, "worlds/saves")
    evidence = outside(args.evidence_dir, (repository, root))
    # Prevent a runtime tree nested in evidence from aliasing captured artifacts.
    check.require(not root.is_relative_to(evidence), "evidence must not contain runtime root")
    binary, dump_binary = args.binary.resolve(strict=True), args.dump_binary.resolve(strict=True)
    config = {"preset": args.preset, "seed": args.seed, "ticks": args.ticks,
              "surface_radius": 1, "collision_radius": 1, "autosave_ticks": 30}
    check.configuration(config)
    identity = identities(repository, root, binary, dump_binary, args.preset, runner)
    tour_path = args.tour.absolute()
    tour = check.parse(check.read(tour_path.parent, tour_path.name))
    check.require(isinstance(tour, dict) and tour.get("identity") == identity,
                  "tour identity mismatch")
    check.location(tour.get("save_root"))
    outside(evidence, (Path(tour["save_root"]), tour_path.parent))
    stages = tour.get("stages")
    check.require(isinstance(stages, list) and len(stages) == len(check.TOUR_STAGES)
                  and all(isinstance(stage, dict) for stage in stages), "missing tour stages")
    refs = [tour.get("ledger"), tour.get("ledger_dump")]
    for stage in stages:
        refs.append(stage.get("log"))
        if stage.get("name") in check.TOUR_STAGES[2:]:
            refs.append(stage.get("receipt"))
    # Read a bounded, complete source bundle before creating the destination.
    check.require(len(set(refs)) == len(refs), "duplicate tour artifact reference")
    inputs = {}
    input_bytes = 0
    for name in refs:
        raw = check.read(tour_path.parent, name, limit=min(check.MAX_FILE, check.MAX_TOTAL - input_bytes))
        input_bytes += len(raw)
        inputs[name] = raw
    systems = check.read(root, "data/common/systems.json")
    preset = check.read(root, "worlds/atlas/presets/" + args.preset + ".json")
    evidence.mkdir(parents=True)  # Exclusive: existing, partial evidence is never reused.
    manifest = {"schema": "luminumbra.active_region_capture.v1", "identity": identity,
                "configuration": config, "systems": "systems.json", "preset": "preset.json",
                "binary": binary.as_posix(), "dump_binary": dump_binary.as_posix(),
                "runtime_root": root.as_posix(), "capture_root": evidence.as_posix(),
                "cells": [], "files": {}}

    def retain(name, raw):
        check.require(len(raw) <= check.MAX_FILE and name not in manifest["files"], "invalid retained artifact")
        check.require(len(manifest["files"]) < check.MAX_FILES
                      and sum(row["size"] for row in manifest["files"].values()) + len(raw) <= check.MAX_TOTAL,
                      "evidence admission budget exceeded")
        write_new(evidence, name, raw)
        manifest["files"][name] = {"size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}

    def register(name):
        raw = check.read(evidence, name)
        check.require(name not in manifest["files"] and len(manifest["files"]) < check.MAX_FILES
                      and sum(row["size"] for row in manifest["files"].values()) + len(raw) <= check.MAX_TOTAL,
                      "evidence admission budget exceeded")
        manifest["files"][name] = {"size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}

    # Pass a private environment copy. Neither successful nor failed calls mutate
    # the caller's worker/throttle settings. All other engine overrides are absent.
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith("LUMINUMBRA_")}

    def invoke(argv, log_name, worker=None, stdout_name=None):
        env = environment.copy()
        overrides = {}
        if worker is not None:
            overrides = {"LUMINUMBRA_JOB_WORKERS": str(worker), "LUMINUMBRA_JOB_THROTTLE": "0"}
            env.update(overrides)
        log_path = check.relative(evidence, log_name)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("xb") as log:
            if stdout_name is None:
                result = runner(argv, cwd=evidence, env=env, stdout=log, stderr=subprocess.STDOUT,
                                check=False, timeout=600)
            else:
                with check.relative(evidence, stdout_name).open("xb") as output:
                    result = runner(argv, cwd=evidence, env=env, stdout=output, stderr=log,
                                    check=False, timeout=600)
        register(log_name)
        check.require(type(result.returncode) is int and result.returncode == 0, "unsuccessful process")
        if stdout_name is not None:
            register(stdout_name)
        return {"command": argv, "returncode": result.returncode, "environment": overrides, "log": log_name}

    retain("systems.json", systems)
    retain("preset.json", preset)
    for name, raw in inputs.items():
        retain("tour/" + name, raw)
    tour = copy.deepcopy(tour)
    for key in ("ledger", "ledger_dump"):
        tour[key] = "tour/" + tour[key]
    for stage in tour["stages"]:
        stage["log"] = "tour/" + stage["log"]
        if "receipt" in stage:
            stage["receipt"] = "tour/" + stage["receipt"]
    tour["verified_dump"] = "verified-ledger.json"
    tour["exporter"] = invoke([dump_binary.as_posix(), (evidence / tour["ledger"]).as_posix()],
                              "exporter.log", stdout_name=tour["verified_dump"])
    effective = tour.get("effective_simulation")
    check.validate_tour(tour, check.Evidence(evidence, manifest["files"]), identity, effective, config)
    manifest["effective_simulation"] = effective
    retain("tour.json", (check.canonical(tour) + "\n").encode("utf-8"))
    manifest["tour"] = "tour.json"
    for worker in check.WORKERS:
        prefix = f"w{worker}"
        (evidence / prefix).mkdir()
        cell = {"worker_count": worker, "identity": identity,
                "recording": prefix + "/session.lrec", "trace": prefix + "/session.lrec.regions.json",
                "replay_artifact": prefix + "/replay.json", "moving_artifact": prefix + "/moving.json"}
        for mode, argv in commands(binary.as_posix(), root.as_posix(), (evidence / prefix).as_posix(), config).items():
            cell[mode] = invoke(argv, prefix + "/" + mode + ".log", worker)
            # Refuse incomplete recordings before invoking playback.
            if mode == "record":
                register(cell["recording"])
                register(cell["trace"])
                check.schedule(check.parse(check.read(evidence, cell["trace"])), args.ticks)
            else:
                register(cell[mode + "_artifact"])
        manifest["cells"].append(cell)
    check.require(identity == identities(repository, root, binary, dump_binary, args.preset, runner),
                  "source, executable or content identity changed during capture")
    verdict = check.validate(evidence, manifest)
    # Manifest publication is last. Partial files remain diagnostic-only.
    write_new(evidence, "capture.pending", (check.canonical(manifest) + "\n").encode("utf-8"))
    (evidence / "capture.pending").replace(evidence / "capture.json")
    return verdict


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[2])
    for name in ("root", "binary", "dump-binary", "tour", "evidence-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--preset", choices=("default", "mountains", "archipelago"), default="default")
    parser.add_argument("--seed", default="1337")
    parser.add_argument("--ticks", type=int, default=90)
    args = parser.parse_args(argv)
    try:
        print(check.canonical(capture(args)))
    except (OSError, ValueError, TypeError, KeyError, AttributeError, subprocess.SubprocessError) as error:
        print(f"Active-region capture refused: {error}; incomplete artifacts retained if created", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
