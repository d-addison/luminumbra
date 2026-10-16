"""Asynchronous compilation and atomic publication into retained generations."""
from __future__ import annotations

import copy
from contextlib import ExitStack
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import pkgutil
import platform
import re
import stat
import subprocess
import sys
import threading
import time
import uuid
import zipfile

from . import VERSION
from .contracts import (ASSET, GENERATION, MAX_DOCUMENT, MAX_OUTPUT, MAX_SNAPSHOT,
                        PROFILE, PREFAB_PROFILE, RECEIPT, Refusal, Toolchain, atomic_json, canonical,
                        digest, file_digest, read_bounded, read_json, relative_path,
                        path_is_link, require, validate_asset)
from .formats import clip_info, mesh_info, texture_info, validate_glb
from .prefab import PrefabPlan, SCHEMA as PREFAB_SCHEMA, encode_glb
from .locking import ProjectLock

TERMINAL = {"succeeded", "failed", "cancelled", "stale"}
JOB_ID = re.compile(r"[0-9a-f]{32}\Z")


def directory_identity(info):
    return info.st_dev, info.st_ino


def lock_windows_directory(stack, path):
    """Pin a real directory against replacement where dir_fd is unavailable."""
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    create.restype = wintypes.HANDLE
    close = kernel.CloseHandle
    close.argtypes = [wintypes.HANDLE]
    close.restype = wintypes.BOOL
    # Read/write sharing, deliberately no FILE_SHARE_DELETE. OPEN_REPARSE_POINT
    # opens a link itself; the caller rechecks lstat after the handle is pinned.
    handle = create(str(path), 0, 0x1 | 0x2, None, 3, 0x02000000 | 0x00200000, None)
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    stack.callback(close, handle)


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
        self.cleanup_roots = {path: path.lstat() for path in (self.root, self.root / "jobs")}
        self.cleanup_jobs = {}
        require(not (self.root / "session.lock").is_symlink(), "path.symlink", "Invalid lock file.")
        try:
            self.project_lock = ProjectLock(self.project, self.root / "session.lock")
        except BlockingIOError as error:
            raise Refusal("session.busy", "Another authoring service owns this project.") from error
        self.mutex = threading.RLock()
        self.close_mutex = threading.Lock()
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
        # Acquiring session.lock first proves no prior service still owns jobs.
        current = self.current()
        self._resume_quarantine()
        paths = list((self.root / "jobs").iterdir())
        container = None
        for path in paths:
            require(path.is_dir() and not path_is_link(path) and JOB_ID.fullmatch(path.name),
                    "state.invalid", "Unexpected entry in the job directory.")
            receipt = path / "receipt.json"
            if not receipt.exists():
                # Crash between making a private job directory and recording it.
                container = self._quarantine_job(path, container)
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
                container = self._quarantine_job(path, container, retain_status=True)
                self.jobs[path.name] = read_json(path / "receipt.json")
        require(len(self.jobs) <= 256, "job.limit", "Project reached the retained-job limit.")

    def _resume_quarantine(self):
        intent = self.root / "quarantine-pending.json"
        require(not path_is_link(intent), "path.symlink", "Invalid quarantine intent path.")
        if not intent.exists():
            return
        transition = read_json(intent)
        require(isinstance(transition, dict) and set(transition) == {"job_id", "container"}
                and isinstance(transition["job_id"], str) and JOB_ID.fullmatch(transition["job_id"])
                and isinstance(transition["container"], str)
                and re.fullmatch(r"[0-9]{8}T[0-9]{6}Z(?:-[0-9]+)?", transition["container"]),
                "state.invalid", "Invalid quarantine transition.")
        job_id = transition["job_id"]
        path = self.root / "jobs" / job_id
        archived = self.root / "quarantine" / transition["container"] / job_id
        for directory in (self.root, path.parent, path, archived.parent.parent,
                          archived.parent, archived):
            require(not path_is_link(directory), "path.symlink", "Invalid quarantine path.")
        source = archived if archived.exists() else path
        require(source.is_dir(), "state.invalid", "Missing quarantine source.")
        job = read_json(relative_path(source, "receipt.json"))
        require(isinstance(job, dict) and job.get("schema") == RECEIPT
                and job.get("job_id") == job_id and job.get("mode") == "NATIVE"
                and job.get("status") not in TERMINAL,
                "state.invalid", "Invalid quarantined job receipt.")
        if source == path:
            path.rename(archived)
        # The original receipt remains evidence; replay only replaces the status copy.
        path.mkdir(exist_ok=True)
        job.update(status="failed", findings=[Refusal(
            "job.orphaned", "Service exited before publication; submit a new build.").finding])
        self._save(job)
        intent.unlink()

    def _quarantine_job(self, path, container, *, retain_status=False):
        quarantine = self.root / "quarantine"
        for directory in (self.root, self.root / "jobs", path, quarantine):
            require(not path_is_link(directory), "path.symlink", "Invalid quarantine path.")
        quarantine.mkdir(exist_ok=True)
        require(quarantine.is_dir() and quarantine.resolve().is_relative_to(self.root),
                "state.invalid", "Quarantine must be a directory inside authoring state.")
        if container is None:
            stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
            counter = 0
            while True:
                container = quarantine / (stamp if counter == 0 else f"{stamp}-{counter}")
                try:
                    os.mkdir(container)
                    break
                except FileExistsError:
                    counter += 1
        if retain_status:
            # Record the destination before moving the only receipt. Startup replays
            # this intent until a replacement receipt has been atomically published.
            atomic_json(self.root / "quarantine-pending.json",
                        {"job_id": path.name, "container": container.name})
            self._resume_quarantine()
        else:
            # Rename the entire directory, preserving all user data.
            path.rename(container / path.name)
        return container

    def capabilities(self):
        return {"version": VERSION, "protocol": 1, "mode": "NATIVE", "profiles": [PROFILE, PREFAB_PROFILE],
                "schemas": [ASSET, PREFAB_SCHEMA], "engine_compilation": True, "engine_preview": False,
                "graph_execution": False, "material_binding": True, "prefab_hierarchy": True,
                "source_revision_guards": True,
                "features": {"max_tris_setting": True},
                "static_prefab_compilation": True, "prefab_runtime_instantiation": False,
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
        asset, files, hashes = self._snapshot(source)
        if asset["profile"] == PREFAB_PROFILE:
            plan = PrefabPlan(files[asset["source"]], asset["asset_id"])
            return {"asset_id": asset["asset_id"], "revision": asset["revision"],
                    "input_hashes": hashes, "findings": [], "nodes": len(plan.description["nodes"]),
                    "parts": len(plan.tasks), "scope": "Static prefab contracts; build validates native outputs."}
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
            directory = self.root / "jobs" / job_id
            directory.mkdir()
            self.cleanup_jobs[job_id] = directory.lstat()
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

    def _compile(self, job_id, input_path, outputs, emit_lods, max_tris=None):
        arguments = ["--emit-lods"] if emit_lods else []
        if max_tris is not None:
            arguments.extend(["--max-tris", str(max_tris)])
        self._invoke(job_id, input_path, outputs / "asset.lmesh", arguments)

    def _invoke(self, job_id, input_path, output_path, arguments):
        directory = output_path.parent.parent
        result_path = directory / "compiler.json"
        command = [str(self.toolchain.processor), str(input_path), str(output_path), *arguments]
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

    def _compile_prefab(self, job_id, outputs, plan):
        parts = []
        for index, (name, task) in enumerate(sorted(plan.tasks.items())):
            self._check_cancelled(job_id)
            payload = encode_glb(task["payload"], plan.binary) if task["kind"] == "mesh" else task["payload"]
            path = outputs.parent / (f"part-{index:04d}.glb" if task["kind"] == "mesh" else f"part-{index:04d}.image")
            path.write_bytes(payload)
            expected = digest(payload)
            try:
                self._invoke(job_id, path, outputs / name, task["arguments"])
                require(file_digest(path) == expected, "input.corrupt", "Compiler changed a derived source part.")
            finally:
                path.unlink(missing_ok=True)
            parts.append({"output": name, "input_sha256": expected, "arguments": task["arguments"]})
            self._change(job_id, compiled_parts=parts)
            require(sum(p.stat().st_size for p in outputs.iterdir()) <= MAX_OUTPUT,
                    "output.limit", "Compiled prefab exceeds the output budget.")
        atomic_json(outputs / "prefab.json", plan.description)

    def _prefab_outputs(self, directory, plan):
        paths = list(directory.iterdir())
        require({p.name for p in paths} == set(plan.tasks) | {"prefab.json"},
                "output.prefab_members", "Unexpected or missing prefab output.")
        result, total = {}, 0
        for path in sorted(paths):
            require(path.is_file() and not path.is_symlink(), "output.path", "Invalid compiled prefab member.")
            data = read_bounded(path, MAX_OUTPUT - total)
            total += len(data)
            if path.name == "prefab.json":
                require(read_json(path) == plan.description, "output.prefab", "Prefab descriptor changed during compilation.")
                info = {"format": "PREFAB", "schema": PREFAB_SCHEMA, "nodes": len(plan.description["nodes"]),
                        "meshes": len(plan.meshes), "materials": len(plan.materials), "bytes": len(data)}
            elif plan.tasks[path.name]["kind"] == "mesh":
                info = mesh_info(data)
                require(info["format"] == "LMSH", "output.prefab_skin", "Static prefab produced a skinned mesh.")
                info.pop("joint_ids")
            else:
                info = texture_info(data)
                require([info["width"], info["height"]] == plan.tasks[path.name]["dimensions"],
                        "output.texture_dimensions", "Compiler changed the texture dimensions.")
            result[path.name] = {**info, "sha256": digest(data)}
        return result

    def _outputs(self, directory, max_tris=None):
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
        if max_tris is not None:
            require(primary["triangles"] <= max_tris, "output.budget_exceeded",
                    f"Primary mesh has {primary['triangles']} triangles; cap is {max_tris}.",
                    "asset.lmesh")
        outputs = {}
        for name, data in sorted(payloads.items()):
            info = clip_info(data, primary["joint_ids"]) if name.endswith(".lanim") else mesh_info(data)
            if max_tris is not None and name in ("asset.lod1.lmesh", "asset.lod2.lmesh"):
                require(info["triangles"] <= primary["triangles"], "output.lod_exceeded",
                        "LOD triangle count exceeds the primary mesh.", name)
            info.pop("joint_ids", None)
            outputs[name] = {"sha256": digest(data), **info}
        return outputs

    def _check_cleanup_directory(self, path, expected):
        info = path.lstat()
        require(not path_is_link(path, info) and stat.S_ISDIR(info.st_mode),
                "output.cleanup_refused", "Staging cleanup requires real directories.", str(path))
        require(path.resolve(strict=True).is_relative_to(self.root)
                and directory_identity(info) == directory_identity(expected),
                "output.cleanup_refused", "Staging directory escaped or was replaced.", str(path))
        return info

    def _cleanup_outputs(self, job_id, output_identity):
        """Delete only owned staging, with pinned ancestry and no-follow descent."""
        directory = self.root / "jobs" / job_id
        chain = [*self.cleanup_roots.items(), (directory, self.cleanup_jobs[job_id]),
                 (directory / "outputs", output_identity)]
        # Validate the entire chain before deleting even a single member.
        for path, expected in chain:
            self._check_cleanup_directory(path, expected)
        with ExitStack() as stack:
            if (os.open in os.supports_dir_fd and os.stat in os.supports_dir_fd
                    and os.unlink in os.supports_dir_fd and os.rmdir in os.supports_dir_fd
                    and os.listdir in os.supports_fd and hasattr(os, "O_NOFOLLOW")):
                flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
                parent = None
                descriptors = []
                for path, expected in chain:
                    fd = os.open(path if parent is None else path.name, flags, dir_fd=parent)
                    stack.callback(os.close, fd)
                    info = os.fstat(fd)
                    require(not path_is_link(None, info) and stat.S_ISDIR(info.st_mode)
                            and directory_identity(info) == directory_identity(expected),
                            "output.cleanup_refused", "Staging directory changed while opening.", str(path))
                    descriptors.append(fd)
                    parent = fd

                def empty(fd):
                    for name in os.listdir(fd):
                        info = os.stat(name, dir_fd=fd, follow_symlinks=False)
                        if path_is_link(None, info) or not stat.S_ISDIR(info.st_mode):
                            os.unlink(name, dir_fd=fd)
                        else:
                            child = os.open(name, flags, dir_fd=fd)
                            try:
                                require(directory_identity(os.fstat(child)) == directory_identity(info),
                                        "output.cleanup_refused", "Staging member changed while opening.", name)
                                empty(child)
                            finally:
                                os.close(child)
                            os.rmdir(name, dir_fd=fd)

                empty(descriptors[-1])
                os.rmdir("outputs", dir_fd=descriptors[-2])
            elif os.name == "nt":
                # Windows path operations are safe only while every ancestor is
                # pinned without delete sharing. Refuse if a handle cannot open.
                lock_windows_directory(stack, self.project)
                for path, expected in chain[:-1]:
                    lock_windows_directory(stack, path)
                    self._check_cleanup_directory(path, expected)

                def empty(path):
                    for entry in path.iterdir():
                        info = entry.lstat()
                        if path_is_link(entry, info):
                            # Junctions/directory symlinks are removed as links.
                            if info.st_file_attributes & stat.FILE_ATTRIBUTE_DIRECTORY:
                                entry.rmdir()
                            else:
                                entry.unlink()
                        elif stat.S_ISDIR(info.st_mode):
                            with ExitStack() as child_stack:
                                lock_windows_directory(child_stack, entry)
                                self._check_cleanup_directory(entry, info)
                                empty(entry)
                            entry.rmdir()
                        else:
                            entry.unlink()

                outputs, expected = chain[-1]
                with ExitStack() as output_stack:
                    lock_windows_directory(output_stack, outputs)
                    self._check_cleanup_directory(outputs, expected)
                    empty(outputs)
                # Only the empty outputs directory is unpinned for rmdir; its
                # job and every parent remain protected against replacement.
                outputs.rmdir()
            else:
                raise Refusal("output.cleanup_refused", "No safe staging deletion primitive is available.")

    def _run(self, job_id):
        directory = self.root / "jobs" / job_id
        try:
            started = time.monotonic()
            job = self.status(job_id)
            check = lambda: self._check_cancelled(job_id)
            asset, files, hashes = self._snapshot(job["source"], check)
            require(asset["revision"] == job["revision"], "revision.stale", "Expected scene revision changed.")
            settings = asset.get("settings", {})
            max_tris = settings.get("max_tris")
            if max_tris is not None:
                # Preserve the legacy receipt shape when no cap was requested.
                self._change(job_id, settings=copy.deepcopy(settings))
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
                "locking.py", "process.py", "service.py", "prefab.py", "file_io.py")}
            identity = digest(canonical({"inputs": hashes, "toolchain": toolchain,
                                         "service": service_hashes, "profile": asset["profile"],
                                         "host": self.host_profile}))
            self._change(job_id, status="compiling", asset_id=asset["asset_id"], input_hashes=hashes,
                         build_identity=identity, toolchain=toolchain, service_hashes=service_hashes,
                         host_profile=self.host_profile)
            outputs = directory / "outputs"
            outputs.mkdir()
            output_identity = outputs.lstat()
            check()
            plan = None
            if asset["profile"] == PREFAB_PROFILE:
                require(not asset.get("settings", {}).get("emit_lods"), "prefab.lods",
                        "Prefab LOD policies need an explicit reduction recipe.")
                plan = PrefabPlan(files[asset["source"]], asset["asset_id"])
                self._compile_prefab(job_id, outputs, plan)
            else:
                self._compile(job_id, input_path, outputs, settings.get("emit_lods", False), max_tris)
            check()
            self.toolchain.verify()
            require(file_digest(input_path) == hashes[asset["source"]],
                    "input.corrupt", "Compiler input snapshot changed.")
            for expected in hashes.values():
                require(file_digest(inputs / expected) == expected, "input.corrupt", "Input snapshot changed.")
            validated = self._prefab_outputs(outputs, plan) if plan else self._outputs(outputs, max_tris)
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
            cleanup_findings = []
            try:
                if refusal.finding["rule_id"] in ("output.budget_exceeded", "output.lod_exceeded"):
                    # Count refusals precede publication; retain only input evidence and receipts.
                    self._cleanup_outputs(job_id, output_identity)
            except Exception as cleanup_error:
                cleanup_findings.append(Refusal("output.cleanup_refused", str(cleanup_error)).finding)
            finally:
                self._change(job_id, status=status, findings=[refusal.finding],
                             **({"cleanup_findings": cleanup_findings} if cleanup_findings else {}))

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
        with self.close_mutex:
            with self.mutex:
                self.closed = True
                for job_id, event in self.cancellations.items():
                    if self.jobs[job_id]["status"] not in TERMINAL:
                        event.set()
            self.executor.shutdown(wait=True, cancel_futures=False)
            # Retry a failed release even though submissions are already closed.
            self.project_lock.close()
