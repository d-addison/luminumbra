"""Asynchronous compilation and atomic publication into retained generations."""
from __future__ import annotations

import copy
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import pkgutil
import platform
import re
import subprocess
import sys
import threading
import time
import uuid
import zipfile

from . import VERSION
from .contracts import (ASSET, GENERATION, MAX_DOCUMENT, MAX_OUTPUT, MAX_SNAPSHOT,
                        PROFILE, RECEIPT, Refusal, Toolchain, atomic_json, canonical,
                        digest, file_digest, read_bounded, read_json, relative_path,
                        require, validate_asset)
from .formats import clip_info, mesh_info, validate_glb
from .locking import ProjectLock

TERMINAL = {"succeeded", "failed", "cancelled", "stale"}
JOB_ID = re.compile(r"[0-9a-f]{32}\Z")


def worker_command():
    entry = Path(sys.argv[0]).resolve()
    if entry.is_file() and zipfile.is_zipfile(entry):
        return [sys.executable, str(entry), "_compiler"]
    return [sys.executable, "-m", "luminumbra_author", "_compiler"]


class BuildService:
    def __init__(self, project, toolchain, *, timeout=120):
        self.project = Path(project).resolve(strict=True)
        require(self.project.is_dir(), "project.invalid", "Project must be a directory.")
        require(0 < timeout <= 3600, "job.timeout", "Compiler timeout must be in (0, 3600] seconds.")
        self.timeout = timeout
        self.host_profile = {"system": platform.system(), "release": platform.release(),
                             "machine": platform.machine(), "python": platform.python_version(),
                             "byteorder": sys.byteorder}
        self.toolchain = Toolchain(toolchain)
        self.root = self.project / ".luminumbra-author"
        for path in (self.root, self.root / "jobs", self.root / "generations"):
            require(not path.is_symlink(), "path.symlink", "Authoring state cannot be a symlink.")
            path.mkdir(exist_ok=True)
        require(not (self.root / "session.lock").is_symlink(), "path.symlink", "Invalid lock file.")
        try:
            self.project_lock = ProjectLock(self.project, self.root / "session.lock")
        except BlockingIOError as error:
            raise Refusal("session.busy", "Another authoring service owns this project.") from error
        self.mutex = threading.RLock()
        self.jobs, self.cancellations = {}, {}
        self.closed = False
        try:
            self._recover()
        except Exception:
            self.project_lock.close()
            raise
        self.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="luminumbra-build")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _save(self, job):
        atomic_json(self.root / "jobs" / job["job_id"] / "receipt.json", job)

    def _recover(self):
        current = self.current()
        paths = list((self.root / "jobs").iterdir())
        require(len(paths) <= 256, "job.limit", "Project reached the retained-job limit.")
        for path in paths:
            require(path.is_dir() and not path.is_symlink() and JOB_ID.fullmatch(path.name),
                    "state.invalid", "Unexpected entry in the job directory.")
            receipt = path / "receipt.json"
            if not receipt.exists():
                # Crash between making a private job directory and recording it.
                continue
            job = read_json(relative_path(path, "receipt.json"))
            require(job.get("schema") == RECEIPT and job.get("job_id") == path.name
                    and job.get("mode") == "NATIVE",
                    "state.invalid", "Invalid persisted job receipt.")
            self.jobs[path.name] = job
            if current and current["job_id"] == path.name and job["status"] != "succeeded":
                self.generation(path.name)
                job.update(status="succeeded", recovery="Publication committed before service exit.")
                self._save(job)
            elif job["status"] not in TERMINAL:
                job.update(status="failed", findings=[Refusal(
                    "job.orphaned", "Service exited before publication; submit a new build.").finding])
                self._save(job)

    def capabilities(self):
        return {"version": VERSION, "protocol": 1, "mode": "NATIVE", "profiles": [PROFILE],
                "schemas": [ASSET], "engine_compilation": True, "engine_preview": False,
                "graph_execution": False, "material_binding": False, "prefab_hierarchy": False,
                "source_revision_guards": True,
                "operations": ["capabilities", "registry", "validate", "build", "job.status",
                               "job.cancel", "generation.inspect"],
                "toolchain": self.toolchain.document, "max_snapshot_bytes": MAX_SNAPSHOT,
                "host_profile": self.host_profile,
                "max_output_bytes": MAX_OUTPUT, "max_jobs": 256, "timeout_seconds": self.timeout}

    def _check_cancelled(self, job_id):
        require(not self.cancellations[job_id].is_set(), "job.cancelled", "Build cancelled.")

    def _snapshot(self, source, check=lambda: None):
        source_path = relative_path(self.project, source)
        require(not source_path.is_relative_to(self.root), "path.reserved", "State is not source content.")
        asset = validate_asset(read_json(source_path))
        names = [source, asset["source"], *asset["dependencies"]]
        require(len(set(names)) == len(names), "dependency.duplicate", "Snapshot paths must be distinct.")
        files, total = {}, 0
        for name in names:
            check()
            path = relative_path(self.project, name)
            require(not path.is_relative_to(self.root), "path.reserved", "State is not source content.")
            data = read_bounded(path, MAX_SNAPSHOT - total)
            total += len(data)
            files[name] = data
        require(read_bounded(source_path, MAX_DOCUMENT) == files[source]
                and read_json(source_path) == asset,
                "revision.stale", "Sidecar changed during extraction.")
        validate_glb(files[asset["source"]])
        hashes = {name: digest(data) for name, data in sorted(files.items())}
        for name, expected in asset.get("expected_dependency_hashes", {}).items():
            require(hashes[name] == expected, "revision.stale",
                    "A dependency changed since the authoring snapshot was captured.", name)
        for name, expected in hashes.items():
            check()
            require(file_digest(relative_path(self.project, name)) == expected,
                    "revision.stale", "Source/dependency changed during extraction.", name)
        return asset, files, hashes

    def validate(self, source):
        asset, _, hashes = self._snapshot(source)
        return {"asset_id": asset["asset_id"], "revision": asset["revision"],
                "input_hashes": hashes, "findings": [],
                "scope": "Sidecar and GLB containment; build performs native geometry validation."}

    def submit(self, source, revision):
        with self.mutex:
            require(not self.closed, "session.closed", "Service is closed.")
            require(isinstance(source, str) and type(revision) is int and revision >= 0,
                    "request.invalid", "Build needs a relative sidecar and nonnegative revision.")
            require(len(list((self.root / "jobs").iterdir())) < 256,
                    "job.limit", "Project reached its 256 retained-job limit.")
            job_id = uuid.uuid4().hex
            (self.root / "jobs" / job_id).mkdir()
            job = {"schema": RECEIPT, "mode": "NATIVE", "job_id": job_id,
                   "status": "extracting", "source": source, "revision": revision,
                   "findings": [], "engine_executed": False, "rendered": False}
            self.jobs[job_id] = job
            self.cancellations[job_id] = threading.Event()
            self._save(job)
            self.executor.submit(self._run, job_id)
            return copy.deepcopy(job)

    def status(self, job_id):
        with self.mutex:
            require(isinstance(job_id, str) and job_id in self.jobs, "job.unknown", "Unknown job ID.")
            return copy.deepcopy(self.jobs[job_id])

    def cancel(self, job_id):
        with self.mutex:
            job = self.status(job_id)
            if job["status"] not in TERMINAL:
                self.cancellations[job_id].set()
            return self.status(job_id)

    def _change(self, job_id, **changes):
        with self.mutex:
            self.jobs[job_id].update(changes)
            self._save(self.jobs[job_id])

    def _compile(self, job_id, input_path, outputs, emit_lods):
        directory = outputs.parent
        result_path = directory / "compiler.json"
        command = [str(self.toolchain.processor), str(input_path), str(outputs / "asset.lmesh")]
        if emit_lods:
            command.append("--emit-lods")
        env = os.environ.copy()
        # The source invocation and the installed zipapp use the same worker.
        if not zipfile.is_zipfile(Path(sys.argv[0]).resolve()):
            env["PYTHONPATH"] = str(Path(__file__).resolve().parent.parent)
        worker = subprocess.Popen(worker_command() + [str(result_path), str(self.timeout), *command],
                                  stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, cwd=directory, env=env)
        self._change(job_id, engine_executed=True)
        deadline = time.monotonic() + self.timeout + 10
        try:
            while worker.poll() is None:
                if self.cancellations[job_id].wait(0.02):
                    worker.stdin.close()
                    worker.wait(timeout=10)
                    self._check_cancelled(job_id)
                if time.monotonic() > deadline:
                    worker.stdin.close()
                    worker.wait(timeout=10)
                    raise Refusal("compiler.timeout", "Compiler supervisor exceeded its deadline.")
            require(worker.returncode == 0, "compiler.supervisor", "Compiler supervisor failed.")
            result = read_json(result_path)
            self._change(job_id, compiler=result)
            require(not result["timed_out"], "compiler.timeout", "Compiler exceeded its deadline.")
            require(not result["cancelled"], "job.cancelled", "Compiler ownership pipe closed.")
            require(result["returncode"] == 0, "compiler.failed", "Asset compiler rejected this snapshot; inspect the bounded log.")
        finally:
            worker.stdin.close()
            try:
                worker.wait(timeout=10)
            except subprocess.TimeoutExpired:
                worker.kill()
                worker.wait()

    def _outputs(self, directory):
        paths = list(directory.iterdir())
        require(1 <= len(paths) <= 256, "output.count", "Unexpected output count.")
        names = {path.name for path in paths}
        require("asset.lmesh" in names, "output.missing", "Compiler produced no primary mesh.")
        total, payloads = 0, {}
        for path in paths:
            require(not path.is_symlink() and path.is_file() and
                    (path.name in ("asset.lmesh", "asset.lod1.lmesh", "asset.lod2.lmesh")
                     or (path.name.startswith("asset.") and path.name.endswith(".lanim"))),
                    "output.unexpected", "Compiler produced an unsupported output entry.", path.name)
            data = read_bounded(path, MAX_OUTPUT - total)
            total += len(data)
            payloads[path.name] = data
        primary = mesh_info(payloads["asset.lmesh"])
        outputs = {}
        for name, data in sorted(payloads.items()):
            info = clip_info(data, primary["joint_ids"]) if name.endswith(".lanim") else mesh_info(data)
            info.pop("joint_ids", None)
            outputs[name] = {"sha256": digest(data), **info}
        return outputs

    def _run(self, job_id):
        directory = self.root / "jobs" / job_id
        try:
            started = time.monotonic()
            job = self.status(job_id)
            check = lambda: self._check_cancelled(job_id)
            asset, files, hashes = self._snapshot(job["source"], check)
            require(asset["revision"] == job["revision"], "revision.stale", "Expected scene revision changed.")
            toolchain = copy.deepcopy(self.toolchain.verify())
            inputs = directory / "inputs"
            inputs.mkdir()
            for name, data in files.items():
                check()
                (inputs / hashes[name]).write_bytes(data)
            input_path = directory / "source.glb"
            input_path.write_bytes(files[asset["source"]])
            # Hash exact service code as well as the executable/library manifest.
            service_hashes = {name: digest(pkgutil.get_data(__package__, name)) for name in (
                "__init__.py", "__main__.py", "contracts.py", "formats.py",
                "locking.py", "process.py", "service.py")}
            identity = digest(canonical({"inputs": hashes, "toolchain": toolchain,
                                         "service": service_hashes, "profile": PROFILE,
                                         "host": self.host_profile}))
            self._change(job_id, status="compiling", asset_id=asset["asset_id"], input_hashes=hashes,
                         build_identity=identity, toolchain=toolchain, service_hashes=service_hashes,
                         host_profile=self.host_profile)
            outputs = directory / "outputs"
            outputs.mkdir()
            check()
            self._compile(job_id, input_path, outputs, asset.get("settings", {}).get("emit_lods", False))
            check()
            self.toolchain.verify()
            require(file_digest(input_path) == hashes[asset["source"]],
                    "input.corrupt", "Compiler input snapshot changed.")
            for expected in hashes.values():
                require(file_digest(inputs / expected) == expected, "input.corrupt", "Input snapshot changed.")
            validated = self._outputs(outputs)
            manifest = {"schema": GENERATION, "mode": "NATIVE", "job_id": job_id,
                        "asset_id": asset["asset_id"], "revision": asset["revision"],
                        "build_identity": identity, "input_hashes": hashes, "outputs": validated}
            atomic_json(outputs / "manifest.json", manifest)
            self._change(job_id, status="publishing", outputs=validated)
            with self.mutex:
                check()
                _, _, current_hashes = self._snapshot(job["source"], check)
                require(current_hashes == hashes, "revision.stale", "Source changed; last valid generation retained.")
                # Retain every published generation. Consumers keep their path;
                # garbage collection is deliberately absent in this version.
                destination = self.root / "generations" / job_id
                os.replace(outputs, destination)
                self.generation(job_id)
                check()
                for name, expected in hashes.items():
                    require(file_digest(relative_path(self.project, name)) == expected,
                            "revision.stale", "Source changed during publication.", name)
                atomic_json(self.root / "current.json", {"schema": GENERATION, "job_id": job_id,
                                                           "build_identity": identity})
                self._change(job_id, status="succeeded", elapsed_seconds=time.monotonic() - started)
        except Exception as error:
            refusal = error if isinstance(error, Refusal) else Refusal("job.failed", str(error))
            status = {"job.cancelled": "cancelled", "revision.stale": "stale"}.get(refusal.finding["rule_id"], "failed")
            # If the pointer committed but the receipt write failed, recovery
            # recognizes that exact generation on the next service startup.
            self._change(job_id, status=status, findings=[refusal.finding])

    def current(self):
        if not (self.root / "current.json").exists():
            return None
        current = read_json(relative_path(self.root, "current.json"))
        require(current.get("schema") == GENERATION and isinstance(current.get("job_id"), str)
                and JOB_ID.fullmatch(current["job_id"]), "state.invalid", "Invalid generation pointer.")
        return current

    def generation(self, job_id=None):
        with self.mutex:
            if job_id is None:
                current = self.current()
                require(current is not None, "generation.missing", "No generation has been published.")
                job_id = current["job_id"]
            require(isinstance(job_id, str) and JOB_ID.fullmatch(job_id), "job.unknown", "Invalid generation ID.")
            prefix = f"generations/{job_id}/"
            manifest = read_json(relative_path(self.root, prefix + "manifest.json"))
            require(manifest.get("schema") == GENERATION and manifest.get("job_id") == job_id,
                    "output.identity", "Generation identity does not match.")
            for name, entry in manifest["outputs"].items():
                require(file_digest(relative_path(self.root, prefix + name)) == entry["sha256"],
                        "output.corrupt", "Generation failed hash verification.", name)
            return manifest

    def close(self):
        with self.mutex:
            if self.closed:
                return
            self.closed = True
            for job_id, event in self.cancellations.items():
                if self.jobs[job_id]["status"] not in TERMINAL:
                    event.set()
        self.executor.shutdown(wait=True, cancel_futures=False)
        self.project_lock.close()
