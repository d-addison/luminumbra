#!/usr/bin/env python3
"""MOCK local stdio authoring service. Copies validated snapshots; never renders.

One owned process per project. Polling job.status advances one deterministic
phase, allowing reproducible cancellation/crash probes without worker threads.
Only the parent holding the inherited pipe and token can issue requests.
"""
from __future__ import annotations

import argparse
import copy
import hmac
import json
import os
from pathlib import Path
import sys
import uuid

from common import atomic_json, canonical, digest, parse_json, read_json
from contracts import (ASSET, PROFILE, RECEIPT, Refusal, project_path,
                       require, validate_asset)
from project_lock import ProjectLock

TERMINAL = {"succeeded", "failed", "cancelled", "stale"}
MAX_BYTES = 4 * 1024 * 1024


class MockService:
    def __init__(self, project, token):
        self.project = Path(project).resolve(strict=True)
        self.token = token
        self.root = self.project / ".mock-author"
        require(not self.root.is_symlink(), "path.escape", "Mock state directory cannot be a symlink.")
        self.root.mkdir(exist_ok=True)
        for name in ("jobs", "generations"):
            path = self.root / name
            require(not path.is_symlink(), "path.escape", "Mock state directory cannot be a symlink.")
            path.mkdir(exist_ok=True)
        try:
            self.lock = ProjectLock(self.project, self.root / "session.lock")
        except BlockingIOError as error:
            raise Refusal("session.busy", "A MOCK service already owns this project.") from error
        except OSError as error:
            raise Refusal("session.lock_unavailable", f"Cannot establish exclusive project ownership: {error}") from error
        self.jobs = {}
        self.previews = {}
        self.closed = False
        try:
            self._recover()
        except Exception:
            self.lock.close()
            self.closed = True
            raise

    def _recover(self):
        current = self.current()
        for path in sorted((self.root / "jobs").glob("*/receipt.json")):
            job = read_json(path)
            self.jobs[job["job_id"]] = job
            if job["status"] not in TERMINAL:
                if current and current["job_id"] == job["job_id"]:
                    self._verify_generation(job["job_id"])
                    job["status"] = "succeeded"
                    job["recovery"] = "publication committed before process exit"
                else:
                    job["status"] = "failed"
                    job["findings"] = [Refusal("job.orphaned", "Service exited before publication; rebuild the source.").finding]
                self._save(job)

    def current(self):
        path = self.root / "current.json"
        return read_json(path) if path.exists() else None

    def _save(self, job):
        atomic_json(self.root / "jobs" / job["job_id"] / "receipt.json", job)

    def _snapshot(self, relative):
        path = project_path(self.project, relative)
        require(path.stat().st_size <= MAX_BYTES, "source.limit", "MOCK source exceeds 4 MiB.", relative)
        value = validate_asset(read_json(path))
        files = {}
        total = 0
        for name in [relative] + value["dependencies"]:
            dep = project_path(self.project, name)
            require(not dep.is_relative_to(self.root), "path.reserved", "Sources cannot reference MOCK state.", name)
            total += dep.stat().st_size
            require(total <= MAX_BYTES, "source.limit", "MOCK snapshot exceeds 4 MiB.", name)
            files[name] = dep.read_bytes()
        # Parse the exact source bytes captured, rejecting an intervening edit.
        require(files[relative] == path.read_bytes() and read_json(path) == value,
                "source.changed", "Source changed during extraction; retry.", relative)
        hashes = {name: digest(data) for name, data in sorted(files.items())}
        return value, files, hashes

    def submit(self, relative, revision):
        require(len(self.jobs) < 256, "job.limit", "MOCK session has reached its 256-job limit; use a new fixture project.")
        value, files, hashes = self._snapshot(relative)
        require(type(revision) is int and value["revision"] == revision,
                "revision.conflict", "Scene revision changed before build.", "revision")
        tool_hashes = {name: digest(Path(__file__).with_name(name).read_bytes())
                       for name in ("mock_service.py", "contracts.py", "common.py", "project_lock.py")}
        identity = digest(canonical({"files": hashes, "profile": PROFILE, "tools": tool_hashes}))
        job_id = uuid.uuid4().hex
        directory = self.root / "jobs" / job_id
        directory.mkdir()
        inputs = directory / "inputs"
        inputs.mkdir()
        # Content-addressed flat storage avoids treating source paths as output paths.
        for name, data in files.items():
            (inputs / hashes[name]).write_bytes(data)
        job = {"schema": RECEIPT, "mode": "MOCK", "job_id": job_id,
               "status": "extracting", "source": relative, "revision": revision,
               "asset_id": value["asset_id"], "input_hashes": hashes,
               "build_identity": identity, "findings": [],
               "tool_hashes": tool_hashes,
               "engine_executed": False, "rendered": False}
        self.jobs[job_id] = job
        self._save(job)
        return copy.deepcopy(job)

    def _job(self, job_id):
        require(isinstance(job_id, str) and job_id in self.jobs,
                "job.unknown", "Unknown job ID.", "job_id")
        return self.jobs[job_id]

    def _verify_generation(self, job_id):
        directory = self.root / "generations" / job_id
        manifest = read_json(directory / "manifest.json")
        require(manifest["mode"] == "MOCK" and manifest["job_id"] == job_id,
                "output.invalid", "Generation identity does not match.")
        for name, expected in manifest["outputs"].items():
            path = project_path(directory, name)
            require(digest(path.read_bytes()) == expected, "output.corrupt", "Output failed hash validation.", name)
        return manifest

    def advance(self, job_id):
        job = self._job(job_id)
        if job["status"] in TERMINAL:
            return copy.deepcopy(job)
        try:
            _, _, hashes = self._snapshot(job["source"])
            require(hashes == job["input_hashes"], "revision.stale",
                    "Source or dependency changed; last valid generation retained.")
            directory = self.root / "jobs" / job_id
            if job["status"] == "extracting":
                job["status"] = "compiling"
            elif job["status"] == "compiling":
                for expected in job["input_hashes"].values():
                    require(digest((directory / "inputs" / expected).read_bytes()) == expected,
                            "input.corrupt", "Immutable input snapshot failed hash validation.")
                outputs = directory / "outputs"
                outputs.mkdir()
                result = {"mode": "MOCK", "build_identity": job["build_identity"],
                          "input_hashes": job["input_hashes"],
                          "message": "Validated snapshot metadata only; no compiled engine asset."}
                payload = canonical(result)
                (outputs / "snapshot.mock.json").write_bytes(payload)
                atomic_json(outputs / "manifest.json", {
                    "mode": "MOCK", "job_id": job_id,
                    "outputs": {"snapshot.mock.json": digest(payload)}})
                job["status"] = "publishing"
            elif job["status"] == "publishing":
                outputs = directory / "outputs"
                manifest = read_json(outputs / "manifest.json")
                require(manifest["outputs"] == {"snapshot.mock.json": digest((outputs / "snapshot.mock.json").read_bytes())},
                        "output.corrupt", "Candidate output failed validation.")
                os.replace(outputs, self.root / "generations" / job_id)
                self._verify_generation(job_id)
                # Linearization point. Requests are serialized by the owned service.
                # Also check source again immediately before publishing the pointer.
                _, _, hashes = self._snapshot(job["source"])
                require(hashes == job["input_hashes"], "revision.stale", "Source changed during publication.")
                atomic_json(self.root / "current.json", {"mode": "MOCK", "job_id": job_id,
                                                          "build_identity": job["build_identity"]})
                job["status"] = "succeeded"
        except (Refusal, OSError, ValueError, KeyError) as error:
            refusal = error if isinstance(error, Refusal) else Refusal("job.failed", str(error))
            job["status"] = "stale" if refusal.finding["rule_id"] == "revision.stale" else "failed"
            job["findings"] = [refusal.finding]
        self._save(job)
        return copy.deepcopy(job)

    def cancel(self, job_id):
        job = self._job(job_id)
        if job["status"] not in TERMINAL:
            job["status"] = "cancelled"
            self._save(job)
        return copy.deepcopy(job)

    def request(self, request):
        require(isinstance(request, dict), "request.invalid", "Expected a JSON object.")
        require(isinstance(request.get("token"), str) and hmac.compare_digest(request["token"], self.token),
                "session.unauthorized", "Invalid session token.")
        require(not self.closed, "session.closed", "Service has shut down.")
        op = request.get("op")
        params = request.get("params", {})
        require(isinstance(params, dict), "request.invalid", "params must be an object.")
        if op == "capabilities":
            return {"mode": "MOCK", "protocol": 1, "profiles": [PROFILE],
                    "engine_preview": False, "graph_execution": False,
                    "job_progress": "one phase per job.status poll",
                    "max_snapshot_bytes": MAX_BYTES}
        if op == "registry":
            return {"mode": "MOCK", "schemas": [ASSET], "components": [], "nodes": []}
        if op == "validate":
            value, _, hashes = self._snapshot(params["source"])
            return {"mode": "MOCK", "revision": value["revision"], "input_hashes": hashes,
                    "findings": [], "qualification": "draft sidecar only; GLB fidelity not checked"}
        if op == "build":
            return self.submit(params["source"], params["revision"])
        if op == "job.status":
            return self.advance(params["job_id"])
        if op == "job.cancel":
            return self.cancel(params["job_id"])
        if op == "preview.open":
            job = self._job(params["job_id"])
            require(job["status"] == "succeeded", "preview.generation", "Preview needs a published generation.")
            self._verify_generation(job["job_id"])
            preview = {"mode": "MOCK", "preview_id": uuid.uuid4().hex,
                       "job_id": job["job_id"], "camera_revision": 0, "epoch": 0}
            self.previews[preview["preview_id"]] = preview
            return copy.deepcopy(preview)
        if op in ("preview.update", "preview.reset", "preview.close", "preview.frame", "preview.capture"):
            key = params["preview_id"]
            require(key in self.previews, "preview.closed", "Preview is closed or belongs to another session.")
            preview = self.previews[key]
            if op == "preview.close":
                del self.previews[key]
                return {"mode": "MOCK", "closed": True}
            if op == "preview.update":
                revision = params["camera_revision"]
                require(type(revision) is int and revision > preview["camera_revision"],
                        "preview.stale", "Camera updates must increase their revision.")
                preview["camera_revision"] = revision
            if op == "preview.reset":
                preview["epoch"] += 1
            if op in ("preview.frame", "preview.capture"):
                return {**preview, "rendered": False, "color": None, "depth": None,
                        "message": "MOCK — frame metadata only"}
            return copy.deepcopy(preview)
        raise Refusal("operation.unsupported", f"Operation {op!r} is not implemented by this MOCK.")

    def close(self):
        if not self.closed:
            for job in self.jobs.values():
                if job["status"] not in TERMINAL:
                    self.cancel(job["job_id"])
            self.previews.clear()
            self.lock.close()
            self.closed = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--responses", type=Path, help="Optional owned directory for nonblocking Blender polling.")
    args = parser.parse_args()
    token = os.environ.get("LUMINUMBRA_MOCK_TOKEN", "")
    if len(token) < 32:
        parser.error("Set LUMINUMBRA_MOCK_TOKEN to an unpredictable token of at least 32 characters.")
    if args.responses:
        args.responses.mkdir(parents=True, exist_ok=True)
    service = MockService(args.project, token)
    sequence = 0
    try:
        while True:
            line = sys.stdin.buffer.readline(65537)
            if not line:
                break
            sequence += 1
            try:
                require(len(line) <= 65536, "request.limit", "Request exceeds 64 KiB.")
                request = parse_json(line)
                result = {"mode": "MOCK", "sequence": sequence, "ok": True,
                          "result": service.request(request)}
            except (Refusal, ValueError, TypeError, KeyError, OSError) as error:
                refusal = error if isinstance(error, Refusal) else Refusal("request.invalid", str(error))
                result = {"mode": "MOCK", "sequence": sequence, "ok": False,
                          "findings": [refusal.finding]}
            if args.responses:
                atomic_json(args.responses / f"{sequence}.json", result)
            else:
                print(canonical(result).decode(), flush=True)
            if len(line) > 65536:
                break
    finally:
        service.close()


if __name__ == "__main__":
    main()
