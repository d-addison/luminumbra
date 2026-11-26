#!/usr/bin/env python3
"""Qualify retained enabled-region evidence; never open or change a live save.

capture.json binds identities, fixed worker cells, process receipts and SHA-256
file receipts. tour.json is an operator's edited-tour attestation: ordered
edited/departed/saved/revisited/reopened stages, each with the same identity,
worker count, successful command and verified edited-payload digest. The last
three stages include standard save_receipt.v1 snapshots and saved_member_sha256
for the containing file. The edit-specific payload digest is independent of
other evolving simulation state in that file. Its ledger and the
production-codec export are retained and compared with a fresh exporter result.
Availability samples are observations; only final moving authority must agree.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fixture_paths import open_regular, read_bytes, resolve_reference
from resolved_fixture import unique_object

WORKERS = (1, 2, 4)
TOUR_STAGES = ("edited", "departed", "saved", "revisited", "reopened")
# Capture-only admission bounds: at most 128 artifacts, 256 MiB each, 2 GiB total;
# at most 100,000 ticks per cell and 50,000 files per save receipt. No sim limits.
MAX_FILE = 256 * 1024 * 1024
MAX_TOTAL = 2 * 1024 * 1024 * 1024
MAX_FILES = 128
MAX_TICKS = 100000
MAX_SAVE_FILES = 50000
IDENTITY_KEYS = {"revision", "binary_sha256", "content_sha256", "systems_sha256",
                 "preset_sha256", "dump_binary_sha256"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def uint(value, maximum=2**64 - 1):
    return type(value) is int and 0 <= value <= maximum


def hex_string(value, width=64):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{%d}" % width, value) is not None


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False)


def commands(binary, root, directory, config):
    """Existing CLI modes only; playback restores the recorded boot descriptor."""
    binary, root, directory = str(binary), str(root), str(directory)
    common = [binary, "--root", root, "--no-audio", "--systems-config",
              root + "/data/common/systems.json", "--preset-json",
              root + "/worlds/atlas/presets/" + config["preset"] + ".json"]
    workload = ["--preset", config["preset"], "--seed", config["seed"], "--ticks", str(config["ticks"]),
                "--radius", "1", "--collision-radius", "1", "--autosave-ticks", "30"]
    return {
        "record": common + workload + ["--record", directory + "/session.lrec"],
        "replay": common + ["--replay", directory + "/session.lrec", "--artifact", directory + "/replay.json"],
        "moving": common + workload + ["--smoke-moving", "--avail-trace", "--artifact", directory + "/moving.json"],
    }


def parse(raw):
    def finite_float(value):
        number = float(value)
        require(math.isfinite(number), "non-finite JSON number")
        return number

    try:
        return json.loads(raw.decode("utf-8-sig"), object_pairs_hook=unique_object,
                          parse_float=finite_float,
                          parse_constant=lambda value: (_ for _ in ()).throw(
                              ValueError(f"non-finite JSON: {value}")))
    except (UnicodeError, RecursionError) as error:
        raise ValueError("invalid JSON encoding or nesting") from error


def relative(root, name):
    require(isinstance(name, str) and len(name) <= 1024 and "\\" not in name
            and ":" not in name and all(p not in ("", ".", "..") for p in name.split("/")),
            "invalid evidence reference")
    return resolve_reference(root, name, max_depth=16)


def location(value):
    require(isinstance(value, str) and 0 < len(value) <= 32768 and "\0" not in value
            and (PurePosixPath(value).is_absolute() or PureWindowsPath(value).is_absolute())
            and ".." not in value.replace("\\", "/").split("/"), "invalid capture location")


def read(root, name, *, limit=MAX_FILE):
    return read_bytes(root, relative(root, name), limit=limit)


def digest_file(path):
    """Hash the verified open handle, with a fixed memory and file-size bound."""
    digest = hashlib.sha256()
    size = 0
    with open_regular(path.parent, path) as stream:
        require(os.fstat(stream.fileno()).st_size <= MAX_TOTAL, "oversized identity input")
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            size += len(block)
            require(size <= MAX_TOTAL, "growing identity input")
            digest.update(block)
    return {"size": size, "sha256": digest.hexdigest()}


def validate_identity(identity):
    require(isinstance(identity, dict) and set(identity) == IDENTITY_KEYS,
            "incomplete identity")
    for key, value in identity.items():
        require(hex_string(value, 40 if key == "revision" else 64), "invalid identity digest")


def configuration(config):
    require(isinstance(config, dict) and set(config) == {
        "preset", "seed", "ticks", "surface_radius", "collision_radius", "autosave_ticks"},
        "incomplete configuration")
    require(config["preset"] in ("default", "mountains", "archipelago")
            and isinstance(config["seed"], str) and 0 < len(config["seed"].encode("utf-8")) <= 4096
            and "\0" not in config["seed"] and uint(config["ticks"], MAX_TICKS)
            and config["ticks"] > 0 and type(config["surface_radius"]) is int
            and type(config["collision_radius"]) is int
            and config["surface_radius"] == config["collision_radius"] == 1
            and type(config["autosave_ticks"]) is int and config["autosave_ticks"] == 30,
            "unsupported capture configuration")


def process(receipt, worker=None):
    require(isinstance(receipt, dict) and type(receipt.get("returncode")) is int
            and receipt["returncode"] == 0, "unsuccessful process")
    argv = receipt.get("command")
    require(isinstance(argv, list) and 0 < len(argv) <= 64
            and all(isinstance(arg, str) and 0 < len(arg) <= 32768 and "\0" not in arg
                    for arg in argv), "invalid command")
    if worker is not None:
        require(receipt.get("environment") == {"LUMINUMBRA_JOB_WORKERS": str(worker),
                                               "LUMINUMBRA_JOB_THROTTLE": "0"},
                "worker environment mismatch")


def authority_digest(name, value):
    # Match HashCheckpoint's labelled cursor extension and the empty-neutral
    # scent/plant/aether-state contracts; mesh never participates in authority.
    if name == "water":
        return isinstance(value, str) and re.fullmatch(
            r"[0-9a-f]{16}(?:\|waterSimCursor:v1:[0-9a-f]{16})?", value) is not None
    if name in ("scents", "plants", "aether_state") and value == "":
        return True
    return hex_string(value, 16)


def schedule(data, ticks):
    require(isinstance(data, dict) and set(data) == {"schema", "checksum", "ticks"}
            and data["schema"] == "luminumbra.region_schedule_trace.v1", "invalid schedule schema")
    rows = data["ticks"]
    require(isinstance(rows, list) and len(rows) == ticks, "incomplete schedule trace")
    for tick, row in enumerate(rows, 1):
        require(isinstance(row, dict) and set(row) == {"tick", "digest"}
                and type(row["tick"]) is int and row["tick"] == tick
                and hex_string(row["digest"], 16), "invalid schedule tick")
    # Matches production StableChecksum(nlohmann::json::dump()), over ASCII-only
    # schema/tick/digest fields with lexically sorted object keys (FNV-1a 64).
    value = {"schema": data["schema"], "ticks": rows}
    checksum = 14695981039346656037
    for byte in canonical(value).encode("ascii"):
        checksum = ((checksum ^ byte) * 1099511628211) & (2**64 - 1)
    require(data["checksum"] == f"{checksum:016x}", "schedule checksum mismatch")
    return rows


def save_receipt(data):
    require(isinstance(data, dict) and data.get("schema") == "luminumbra.save_receipt.v1",
            "invalid save receipt schema")
    files = data.get("files")
    require(isinstance(files, list) and 0 < len(files) <= MAX_SAVE_FILES, "invalid save file count")
    previous = ""
    total = 0
    digest = hashlib.sha256()
    result = {}
    for item in files:
        require(isinstance(item, dict) and set(item) == {"path", "size", "sha256"},
                "invalid save entry")
        name = item["path"]
        # Receipt members are names only, never opened; use portable spelling.
        require(isinstance(name, str) and 0 < len(name) <= 1024 and "\\" not in name
                and ":" not in name and "\0" not in name
                and all(p not in ("", ".", "..") for p in name.split("/"))
                and name > previous and uint(item["size"]) and hex_string(item["sha256"]),
                "invalid or unsorted save entry")
        previous = name
        total += item["size"]
        digest.update(f"{name}\0{item['size']}\0{item['sha256']}\n".encode("utf-8"))
        result[name] = item
    require(type(data.get("file_count")) is int and data["file_count"] == len(files)
            and uint(data.get("total_bytes")) and data["total_bytes"] == total
            and data.get("tree_digest") == digest.hexdigest(), "save receipt totals/checksum mismatch")
    return result


class Evidence:
    def __init__(self, root, files):
        require(isinstance(files, dict) and 0 < len(files) <= MAX_FILES, "invalid artifact inventory")
        total = 0
        for name, item in files.items():
            relative(root, name)
            require(isinstance(item, dict) and set(item) == {"size", "sha256"}
                    and uint(item["size"], MAX_FILE) and hex_string(item["sha256"]),
                    "invalid artifact receipt")
            total += item["size"]
        require(total <= MAX_TOTAL, "oversized evidence inventory")
        self.root, self.files = root, files
        self.used = set()

    def raw(self, name):
        require(isinstance(name, str) and name in self.files, "missing artifact receipt")
        raw = read(self.root, name)
        require(self.files[name] == {"size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()},
                "artifact checksum mismatch")
        self.used.add(name)
        return raw

    def json(self, name):
        return parse(self.raw(name))


def validate_tour(tour, evidence, identity, effective, config):
    require(isinstance(tour, dict) and tour.get("schema") == "luminumbra.active_region_tour.v1"
            and tour.get("identity") == identity
            and canonical(tour.get("effective_simulation")) == canonical(effective)
            and canonical(tour.get("configuration")) == canonical(config),
            "tour identity/configuration mismatch")
    location(tour.get("save_root"))
    worker = tour.get("worker_count")
    require(type(worker) is int and worker in WORKERS, "invalid tour worker count")
    payload = tour.get("edited_payload")
    require(isinstance(payload, dict) and set(payload) == {"path", "sha256"}
            and hex_string(payload["sha256"]), "missing edited payload")
    stages = tour.get("stages")
    require(isinstance(stages, list) and len(stages) == len(TOUR_STAGES), "missing tour stages")
    final_files = None
    for expected, stage in zip(TOUR_STAGES, stages):
        require(isinstance(stage, dict) and stage.get("name") == expected
                and stage.get("identity") == identity
                and type(stage.get("worker_count")) is int and stage["worker_count"] == worker
                and stage.get("payload_sha256") == payload["sha256"]
                and stage.get("verified") is True, "incomplete or mixed tour stage")
        process(stage, worker)
        evidence.raw(stage.get("log"))
        if expected in TOUR_STAGES[2:]:
            final_files = save_receipt(evidence.json(stage.get("receipt")))
            require(isinstance(payload["path"], str) and payload["path"] in final_files
                    and final_files[payload["path"]]["sha256"] == stage.get("saved_member_sha256"),
                    "edited payload missing from saved/revisited/reopened receipt")
    ledger = evidence.raw(tour.get("ledger"))
    require(final_files is not None and "chunks/region/active-regions.arl" in final_files,
            "ledger missing from reopened receipt")
    ledger_item = final_files["chunks/region/active-regions.arl"]
    require(ledger_item["sha256"] == hashlib.sha256(ledger).hexdigest()
            and ledger_item["size"] == len(ledger), "tour ledger/receipt mismatch")
    supplied = evidence.json(tour.get("ledger_dump"))
    reproduced = evidence.json(tour.get("verified_dump"))
    exporter = tour.get("exporter")
    process(exporter)
    evidence.raw(exporter.get("log"))
    require(canonical(supplied) == canonical(reproduced) and isinstance(supplied, dict)
            and supplied.get("schema") == "luminumbra.active_regions_dump.v1"
            and type(supplied.get("source_bytes")) is int and supplied["source_bytes"] == len(ledger)
            and type(supplied.get("source_version")) is int and supplied["source_version"] in (1, 2, 3)
            and uint(supplied.get("tick")) and uint(supplied.get("header_tick"))
            and supplied["header_tick"] >= supplied["tick"]
            and supplied.get("config") == {"work_limit": 2**64 - 1, "hold_ticks": 30,
                                           "reduced_shift": 1}, "invalid or mismatched ledger dump")
    records = supplied.get("records")
    require(isinstance(records, list) and 0 < len(records) <= 1000000
            and all(isinstance(row, dict) for row in records)
            and any(row.get("edited") is True for row in records), "no edited ledger record")


def validate(root, manifest):
    require(isinstance(manifest, dict) and manifest.get("schema") == "luminumbra.active_region_capture.v1",
            "unsupported capture schema")
    identity, config = manifest.get("identity"), manifest.get("configuration")
    validate_identity(identity)
    configuration(config)
    for name in ("binary", "dump_binary", "runtime_root", "capture_root"):
        location(manifest.get(name))
    ticks = config["ticks"]
    effective = manifest.get("effective_simulation")
    require(isinstance(effective, dict) and effective and effective.get("active_regions") is True
            and all(type(v) is bool for v in effective.values()), "missing effective simulation")
    evidence = Evidence(root, manifest.get("files"))
    systems = evidence.json(manifest.get("systems"))
    require(hashlib.sha256(evidence.raw(manifest["systems"])).hexdigest() == identity["systems_sha256"],
            "systems identity mismatch")
    require(hashlib.sha256(evidence.raw(manifest.get("preset"))).hexdigest() == identity["preset_sha256"],
            "preset identity mismatch")
    require(isinstance(systems, dict) and isinstance(systems.get("sim"), dict), "invalid systems document")
    gates = {name for name, value in systems["sim"].items()
             if isinstance(value, dict) and "enabled" in value}
    require("active_regions" in gates and all(effective.get(name) is True for name in gates)
            and all(effective.get(name) is True for name in ("finite_hydrology", "weather_rain")),
            "shipping simulation gate missing or disabled")
    cells = manifest.get("cells")
    require(isinstance(cells, list) and len(cells) == len(WORKERS), "incomplete worker matrix")
    baseline = None
    moving_baseline = None
    for worker, cell in zip(WORKERS, cells):
        require(isinstance(cell, dict) and type(cell.get("worker_count")) is int
                and cell["worker_count"] == worker and cell.get("identity") == identity,
                "worker cell identity mismatch")
        prefix = f"w{worker}"
        for key, suffix in (("recording", "session.lrec"), ("trace", "session.lrec.regions.json"),
                            ("replay_artifact", "replay.json"), ("moving_artifact", "moving.json")):
            require(cell.get(key) == prefix + "/" + suffix, "worker artifact binding mismatch")
        directory = manifest["capture_root"] + f"/w{worker}"
        expected_commands = commands(manifest["binary"], manifest["runtime_root"], directory, config)
        for mode in ("record", "replay", "moving"):
            receipt = cell.get(mode)
            process(receipt, worker)
            require(receipt["command"] == expected_commands[mode], "capture command mismatch")
            require(receipt.get("log") == prefix + "/" + mode + ".log", "worker log binding mismatch")
            log = evidence.raw(receipt.get("log")).decode("utf-8", errors="replace")
            starts = re.findall(r"JobSystem started with ([0-9]+) threads", log)
            require(starts and all(int(count) == worker for count in starts), "effective worker mismatch")
        trace = schedule(evidence.json(cell.get("trace")), ticks)
        require(evidence.raw(cell.get("recording")).startswith(b"LREC1"), "missing replay stream")
        record_log = evidence.raw(cell["record"]["log"]).decode("utf-8", errors="replace")
        ends = re.findall(r"record: wrote .* \(([0-9]+) ticks, [0-9]+ input records, "
                          r"[0-9]+ checkpoints, end_hash=([0-9a-f]{16})\)", record_log)
        require(len(ends) == 1 and int(ends[0][0]) == ticks, "incomplete recorder completion")
        replay = evidence.json(cell.get("replay_artifact"))
        require(isinstance(replay, dict) and replay.get("schema") == "luminumbra.replay_roundtrip.v1"
                and replay.get("passed") is True and replay.get("diverged") is False
                and replay.get("start_world_hash_match") is True and replay.get("end_hash_match") is True
                and replay.get("engine_version") == replay.get("replayed_engine_version")
                and isinstance(replay.get("engine_version"), str) and replay["engine_version"]
                and replay.get("preset") == config["preset"] and replay.get("seed") == config["seed"]
                and all(type(replay.get(key)) is int and replay[key] == ticks
                        for key in ("tick_count", "ticks_replayed", "input_records"))
                and type(replay.get("checkpoints_verified")) is int
                and replay["checkpoints_verified"] == (ticks + 29) // 30
                and hex_string(replay.get("start_world_hash"), 16)
                and replay.get("end_world_hash") == ends[0][1], "invalid replay roundtrip")
        current = (replay["start_world_hash"], replay["end_world_hash"], trace, replay["engine_version"])
        require(baseline is None or current == baseline, "cross-worker record/replay mismatch")
        baseline = current
        moving = evidence.json(cell.get("moving_artifact"))
        require(isinstance(moving, dict) and moving.get("schema") == "luminumbra.server_tick.v1"
                and all(moving.get(key) is True for key in ("passed", "deterministic", "sub_hashes_match"))
                and hex_string(moving.get("world_hash"), 16)
                and moving.get("world_hash_replay") == moving["world_hash"]
                and canonical(moving.get("effective_simulation")) == canonical(effective)
                and moving.get("preset") == config["preset"] and moving.get("seed") == config["seed"]
                and type(moving.get("ticks_requested")) is int and moving["ticks_requested"] == ticks
                and type(moving.get("surface_radius")) is int and type(moving.get("collision_radius")) is int
                and moving["surface_radius"] == moving["collision_radius"] == 1,
                "invalid moving replay/identity")
        runs = moving.get("runs")
        require(isinstance(runs, list) and len(runs) == 2, "incomplete moving runs")
        authority = None
        keys = {"terrain", "water", "entities", "wind", "weather", "aether", "aether_state",
                "scents", "ecology", "plants"}
        for run in runs:
            require(isinstance(run, dict) and run.get("ok") is True
                    and type(run.get("ticks_executed")) is int and run["ticks_executed"] == ticks
                    and run.get("world_hash") == moving["world_hash"], "incomplete moving run")
            sub = run.get("sub_hashes")
            require(isinstance(sub, dict) and keys <= sub.keys()
                    and all(authority_digest(key, sub[key]) for key in keys), "incomplete moving authority")
            current_authority = {key: sub[key] for key in sorted(keys)}
            require(authority is None or current_authority == authority, "moving sub-hash mismatch")
            authority = current_authority
        current_moving = (moving["world_hash"], authority)
        require(moving_baseline is None or current_moving == moving_baseline, "cross-worker moving mismatch")
        moving_baseline = current_moving
        availability = moving.get("availability_trace")
        require(isinstance(availability, list) and len(availability) == ticks
                and type(moving.get("availability_trace_match")) is bool,
                "missing moving availability observation")
        for tick, row in enumerate(availability, 1):
            require(isinstance(row, dict) and type(row.get("tick")) is int and row["tick"] == tick
                    and hex_string(row.get("avail"), 16), "incomplete availability observation")
    tour = evidence.json(manifest.get("tour"))
    validate_tour(tour, evidence, identity, effective, config)
    require(tour["exporter"]["command"] == [manifest["dump_binary"],
            manifest["capture_root"] + "/" + tour["ledger"]], "exporter command mismatch")
    require(evidence.used == evidence.files.keys(), "unconsumed artifact inventory")
    return {"schema": "luminumbra.active_region_verdict.v1", "passed": True,
            "identity": identity, "configuration": config, "workers": list(WORKERS),
            "record_world_hash": baseline[1], "moving_world_hash": moving_baseline[0],
            "availability_timing": "observational", "tour_stages": list(TOUR_STAGES)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence_dir", type=Path)
    args = parser.parse_args(argv)
    try:
        root = args.evidence_dir.resolve(strict=True)
        verdict = validate(root, parse(read(root, "capture.json")))
        print(canonical(verdict))
    except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
        print(f"Active-region evidence refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
