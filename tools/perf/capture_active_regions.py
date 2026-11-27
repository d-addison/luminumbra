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
import re
import stat
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_active_regions as check
from fixture_hash import FixtureBudget, fixture_entries
from fixture_paths import open_regular
from resolved_fixture import resolved_fixture_hash


commands = check.commands


def staged_libraries(binary, budget=None):
    """Bound all sibling entries, including ignored names, before collecting."""
    budget = FixtureBudget() if budget is None else budget
    libraries = []
    with os.scandir(binary.parent) as scanner:
        for entry in scanner:
            budget.take(1)
            name = entry.name.lower()
            if name.endswith((".dll", ".so", ".dylib")) or ".so." in name:
                libraries.append(Path(entry.path))
    return sorted(libraries, key=lambda path: path.name)


def library_identity(binary):
    libraries = {}
    remaining = check.MAX_TOTAL
    for path in staged_libraries(binary):
        item = check.digest_file(path, limit=remaining)
        remaining -= item["size"]
        libraries[path.name] = item
    return hashlib.sha256(check.canonical(libraries).encode("utf-8")).hexdigest()


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
            "binary_libraries_sha256": library_identity(binary),
            "dump_binary_libraries_sha256": library_identity(dump_binary),
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


def check_linux_loader(path, *, library_names=None):
    """Refuse ELF imports which can bypass the private library search directory."""
    library_names = {os.fsencode(path.name)} if library_names is None else library_names
    with open_regular(path.parent, path) as stream:
        size = os.fstat(stream.fileno()).st_size

        def read_at(offset, count):
            check.require(0 <= offset <= size and 0 <= count <= size - offset,
                          "cannot isolate Linux dependencies: invalid ELF range")
            stream.seek(offset)
            raw = stream.read(count)
            check.require(len(raw) == count, "cannot isolate Linux dependencies: truncated ELF")
            return raw

        ident = read_at(0, 16)
        check.require(ident[:4] == b"\x7fELF" and ident[4] in (1, 2)
                      and ident[5] in (1, 2) and ident[6] == 1,
                      "cannot isolate Linux dependencies: unsupported executable/library format")
        endian = "<" if ident[5] == 1 else ">"
        wide = ident[4] == 2
        header_format = endian + ("HHIQQQIHHHHHH" if wide else "HHIIIIIHHHHHH")
        header = struct.unpack(header_format, read_at(16, struct.calcsize(header_format)))
        offset, stride, count = header[4], header[8], header[9]
        ph_format = endian + ("IIQQQQQQ" if wide else "IIIIIIII")
        ph_size = struct.calcsize(ph_format)
        # Bound both metadata scans to 4096 entries (at most 224 KiB of headers).
        check.require(stride == ph_size and count <= 4096,
                      "cannot isolate Linux dependencies: unsupported ELF program headers")
        headers = [struct.unpack(ph_format, read_at(offset + index * stride, stride))
                   for index in range(count)]
        loads, dynamics = [], []
        for row in headers:
            start, address, length = (row[2], row[3], row[5]) if wide else (row[1], row[2], row[4])
            if row[0] == 1:  # PT_LOAD maps the dynamic string table's virtual address.
                loads.append((start, address, length))
            elif row[0] == 2:  # PT_DYNAMIC
                dynamics.append((start, length))
        check.require(len(dynamics) <= 1, "cannot isolate Linux dependencies: multiple dynamic tables")
        if not dynamics:
            return  # A static executable has no loader search to redirect.
        offset, length = dynamics[0]
        dyn_format = endian + ("qQ" if wide else "iI")
        stride = struct.calcsize(dyn_format)
        check.require(length % stride == 0 and 0 < length // stride <= 4096,
                      "cannot isolate Linux dependencies: invalid dynamic table size")
        tags = []
        for index in range(length // stride):
            tag, value = struct.unpack(dyn_format, read_at(offset + index * stride, stride))
            if tag == 0:  # DT_NULL
                break
            # RPATH precedes LD_LIBRARY_PATH. Auditors and filter objects can
            # introduce imports outside the ordinary DT_NEEDED search contract.
            check.require(tag not in (15, 0x6FFFFEFB, 0x6FFFFEFC, 0x7FFFFFFD, 0x7FFFFFFF),
                          "cannot isolate Linux dependencies: RPATH/audit/filter dynamic tag")
            tags.append((tag, value))
        else:
            check.require(False, "cannot isolate Linux dependencies: unterminated dynamic table")
        runpaths = [value for tag, value in tags if tag == 29]  # DT_RUNPATH
        check.require(len(runpaths) <= 1, "cannot isolate Linux dependencies: ambiguous RUNPATH")
        references = [(tag, value) for tag, value in tags if tag in (1, 14, 29)]  # NEEDED, SONAME, RUNPATH
        if not references:
            return
        strings = [value for tag, value in tags if tag == 5]  # DT_STRTAB
        lengths = [value for tag, value in tags if tag == 10]  # DT_STRSZ
        check.require(len(strings) == len(lengths) == 1,
                      "cannot isolate Linux dependencies: missing/ambiguous dynamic strings")
        mappings = [(start + strings[0] - address, length - (strings[0] - address))
                    for start, address, length in loads
                    if address <= strings[0] < address + length]
        check.require(len(mappings) == 1 and lengths[0] <= mappings[0][1],
                      "cannot isolate Linux dependencies: unmapped dynamic strings")
        needed, directories = [], []
        staged = os.path.normpath(os.fsencode(path.parent))
        for tag, name in references:
            check.require(name < lengths[0], "cannot isolate Linux dependencies: invalid import offset")
            # Each import or RUNPATH must terminate within this 4096-byte buffer.
            raw = read_at(mappings[0][0] + name, min(4096, lengths[0] - name))
            dependency, terminator, _ = raw.partition(b"\0")
            if tag == 29:
                check.require(terminator, "cannot isolate Linux dependencies: unterminated RUNPATH")
                # Only complete ORIGIN tokens have known, object-local semantics.
                origin_token = rb"\$(?:ORIGIN(?![A-Za-z0-9_])|\{ORIGIN\})"
                # glibc also accepts semicolons as search-path separators.
                for entry in re.split(b"[:;]", dependency):
                    check.require(b"$" not in re.sub(origin_token, b"", entry),
                                  "cannot isolate Linux dependencies: unsupported RUNPATH token")
                    expanded = re.sub(origin_token, lambda match: os.fsencode(path.parent), entry)
                    check.require(entry and os.path.isabs(expanded),
                                  "cannot isolate Linux dependencies: empty/relative RUNPATH entry")
                    directory = os.path.normpath(expanded)
                    if directory != staged:
                        directories.append(directory)
                continue
            check.require(terminator and dependency and b"/" not in dependency and b"$" not in dependency,
                          "cannot isolate Linux dependencies: path-bearing/invalid import")
            check.require(tag != 14 or dependency in library_names,
                          "cannot isolate Linux dependencies: SONAME missing from staged set")
            if tag == 1 and dependency not in library_names:
                needed.append(dependency)
        # At most 4096 dynamic tags by 2048 entries in the bounded RUNPATH.
        # Probe names only: never open or follow an unstaged file or symlink.
        for dependency in needed:
            for directory in directories:
                try:
                    os.lstat(os.path.join(directory, dependency))
                except (FileNotFoundError, NotADirectoryError):
                    continue  # No RUNPATH candidate; allow the system default search.
                check.require(False, "cannot isolate Linux dependencies: RUNPATH resolves unstaged "
                              + os.fsdecode(dependency))


def loader_environment(binary):
    """Select sibling libraries without changing the recorded engine overrides."""
    if os.name == "nt":
        # Windows searches the application directory before PATH for staged DLLs;
        # LD_* and DYLD_* do not control its native loader.
        return {}
    if sys.platform.startswith("linux"):
        directory = str(binary.parent)
        # The loader splits on both delimiters and expands dynamic path tokens.
        check.require(not any(char in directory for char in (":", ";", "$")),
                      "cannot isolate Linux dependencies: unsafe staged library directory")
        libraries = staged_libraries(binary)
        names = {os.fsencode(path.name) for path in [binary, *libraries]}
        check_linux_loader(binary, library_names=names)
        for library in libraries:
            # DT_NEEDED basenames need not end in .so; inspect the entire staged
            # set, including DLL/dylib names, rather than trusting the suffix.
            check_linux_loader(library, library_names=names)
        return {"LD_LIBRARY_PATH": directory}
    # DYLD_LIBRARY_PATH alone cannot guarantee isolation of macOS install names
    # (and hardened executables can ignore it). No other POSIX loader is assumed.
    check.require(not staged_libraries(binary),
                  "cannot isolate staged libraries on this platform")
    return {}


def capture(args, runner=None):
    runner = subprocess.run if runner is None else runner
    # Private copies live through every invocation and are removed on refusal as
    # well as success. TemporaryDirectory also handles read-only files on Windows.
    with tempfile.TemporaryDirectory(prefix="active-region-inputs-") as directory:
        return capture_snapshot(args, runner, Path(directory))


def snapshot_inputs(root, binary, dump_binary, directory):
    """Freeze runtime inputs and executables without hard links or path rereads."""
    runtime = directory / "runtime"
    runtime.mkdir()
    # All runtime and executable-directory scans share 4096 entries/depth 64.
    # All copied bytes, including binaries and libraries, share the 2 GiB cap;
    # each copy uses at most 1 MiB of memory and a verified regular-file handle.
    budget = FixtureBudget()
    data = root / "data"
    entries = list(fixture_entries(root, budget=budget, separate_roots=(data,)))
    entries.extend(fixture_entries(data, budget=budget, depth_offset=1))
    copies = [(root, path, runtime / path.relative_to(root), False)
              for path, _, status in entries if stat.S_ISREG(status.st_mode)]
    for path, _, status in entries:
        if stat.S_ISDIR(status.st_mode):
            (runtime / path.relative_to(root)).mkdir()
    selected = []
    for name, source in (("server", binary), ("exporter", dump_binary)):
        target = directory / name / source.name
        target.parent.mkdir()
        copies.append((source.parent, source, target, True))
        for library in staged_libraries(source, budget):
            copies.append((source.parent, library, target.parent / library.name, False))
        selected.append(target)
    remaining = check.MAX_TOTAL
    for source_root, source, target, executable in copies:
        with open_regular(source_root, source) as stream, target.open("xb") as output:
            size = os.fstat(stream.fileno()).st_size
            check.require(size <= remaining, "input snapshot byte budget exceeded")
            copied = 0
            while True:
                block = stream.read(min(1024 * 1024, remaining + 1))
                if not block:
                    break
                check.require(len(block) <= remaining, "input snapshot byte budget exceeded")
                check.require(output.write(block) == len(block), "short input snapshot write")
                remaining -= len(block)
                copied += len(block)
            check.require(copied == size, "input snapshot file size changed")
            output.flush()
        target.chmod(0o500 if executable else 0o400)
    return runtime, *selected


def capture_snapshot(args, runner, directory):
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
    check.require(isinstance(tour, dict) and tour.get("schema") == "luminumbra.active_region_tour.v2"
                  and tour.get("identity") == identity, "tour identity/schema mismatch")
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
            refs.append(stage.get("saved_member"))
    # Read a bounded, complete source bundle before creating the destination.
    check.require(len(set(refs)) == len(refs), "duplicate tour artifact reference")
    inputs = {}
    input_bytes = 0
    for name in refs:
        raw = check.read(tour_path.parent, name, limit=min(check.MAX_FILE, check.MAX_TOTAL - input_bytes))
        input_bytes += len(raw)
        inputs[name] = raw
    source_root, source_binary, source_dump_binary = root, binary, dump_binary
    root, binary, dump_binary = snapshot_inputs(root, binary, dump_binary, directory)
    check.require(identity == identities(repository, root, binary, dump_binary, args.preset, runner),
                  "input snapshot identity mismatch")
    loaders = {path.as_posix(): loader_environment(path) for path in (binary, dump_binary)}
    systems = check.read(root, "data/common/systems.json")
    preset = check.read(root, "worlds/atlas/presets/" + args.preset + ".json")
    evidence.mkdir(parents=True)  # Exclusive: existing, partial evidence is never reused.
    manifest = {"schema": "luminumbra.active_region_capture.v2", "identity": identity,
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
                   if not key.upper().startswith("LUMINUMBRA_")
                   and (os.name == "nt" or (key not in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT")
                                            and not key.startswith("DYLD_")))}

    def invoke(argv, log_name, worker=None, stdout_name=None):
        env = environment.copy()
        env.update(loaders[argv[0]])
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
            stage["saved_member"] = "tour/" + stage["saved_member"]
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
    check.require(identity == identities(repository, source_root, source_binary, source_dump_binary,
                                         args.preset, runner)
                  == identities(repository, root, binary, dump_binary, args.preset, runner),
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
