"""Process-owned MG2 ledger and backend-only orchestration.

No BuildService is constructed here. When a future operation needs both locks,
acquire the MG2 job lock first, then the build-session lock; never reverse this
order. Cross-OS shared WSL-directory locking is not a qualified lane.

Raw-ready is `finishing`, not candidate success. E4 supplies a trusted commit
callback to commit_success; cancellation and that commit share one transition.
"""
from concurrent.futures import Future, ThreadPoolExecutor
from contextlib import contextmanager
from copy import deepcopy
import os
from pathlib import Path
import re
import secrets
import threading
import time
import uuid

from . import file_io
from .contracts import (Refusal, canonical, digest, keys, path_is_link,
                        read_bounded, relative_path, require)
from .locking import ProjectLock
from .meshgen_backend import (BackendClient, bounded_json,
    image_snapshot, submission, token_from_source, validate_capability, validate_result)
from .meshgen_config import PROJECT_CONFIG, load_config
from .meshgen_contracts import STATE_ROOT, candidate_seeds, validate_backend_params, validate_request
from .meshgen_licence import candidate_eligibility, gate

TERMINAL = frozenset(("succeeded", "failed", "cancelled"))


# Admission ceilings cover all documents; replay evidence is never evicted.
PROJECT_RECORD_LIMIT = 256
PROJECT_BYTE_LIMIT = 4 * 1024**3


def matches(pattern, value):
    return isinstance(value, str) and re.fullmatch(pattern, value)


def configured_tokens(project, loader=load_config):
    if not (Path(project) / PROJECT_CONFIG).exists():
        return set()
    effective = loader(project, "default").effective
    return {token_from_source(project, backend["token_source"])
            for backend in effective["backends"].values() if backend.get("enabled")}


def sync_directory(path):
    if os.name != "nt":
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)


def durable_replace(source, destination):
    file_io.replace_file(source, destination, durable=True)
    sync_directory(destination.parent)


def durable_replace_directory(source, destination):
    """Directory moves remain separate from regular-file publication."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        move = kernel.MoveFileExW
        move.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD]
        move.restype = wintypes.BOOL
        # Publish complete bytes and wait for the move to reach disk.
        if not move(str(source), str(destination), 0x1 | 0x8):
            raise ctypes.WinError(ctypes.get_last_error())
    else:
        os.replace(source, destination)
        sync_directory(destination.parent)


def durable_mkdir(path):
    if path.is_dir():
        return
    durable_mkdir(path.parent)
    if os.name == "nt":
        temporary = path.with_name(".mkdir-" + uuid.uuid4().hex)
        temporary.mkdir(mode=0o700)
        try:
            try:
                durable_replace_directory(temporary, path)
            except OSError:
                if not path.is_dir():
                    raise
        finally:
            if temporary.exists():
                temporary.rmdir()
    else:
        path.mkdir(mode=0o700, exist_ok=True)
        sync_directory(path.parent)


def durable_write(path, data):
    durable_mkdir(path.parent)
    temporary = path.with_name(".publish-" + uuid.uuid4().hex)
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        durable_replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def durable_remove(path):
    if not path.exists():
        return
    if os.name == "nt":
        # Durable removal from the authoritative namespace; a crash can leave
        # only an ignored tombstone, never a resurrected owner file.
        tombstone = path.with_name(".removed-" + uuid.uuid4().hex)
        durable_replace(path, tombstone)
        tombstone.unlink()
    else:
        path.unlink()
        sync_directory(path.parent)


def process_start(pid):
    """OS process creation identity, not a PID liveness guess (PID reuse matters)."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.GetExitCodeProcess.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            require(ctypes.get_last_error() == 87, "busy", "Cannot verify the process owning the MG2 lock.")
            return None
        try:
            exit_code = wintypes.DWORD()
            require(kernel.GetExitCodeProcess(handle, ctypes.byref(exit_code)), "busy", "Cannot inspect MG2 owner liveness.")
            if exit_code.value != 259:  # STILL_ACTIVE
                return None
            times = [wintypes.FILETIME() for _ in range(4)]
            require(kernel.GetProcessTimes(handle, *(ctypes.byref(item) for item in times)),
                    "busy", "Cannot inspect MG2 owner creation time.")
            return str((times[0].dwHighDateTime << 32) | times[0].dwLowDateTime)
        finally:
            kernel.CloseHandle(handle)
    try:
        # comm may contain spaces and parentheses; fields after its final ')' are fixed.
        record = Path(f"/proc/{pid}/stat").read_text()
        fields = record.rsplit(")", 1)[1].split()
        return None if fields[0] in ("Z", "X") else fields[19]
    except FileNotFoundError:
        return None
    except (OSError, IndexError) as error:
        raise Refusal("busy", "Cannot verify MG2 owner creation time on this platform.") from error


class RequestLedger:
    def __init__(self, project, *, document="_headless", clock=time.time,
                 owner_start=process_start, recover_cancel=None, terminal_limit=256, byte_limit=PROJECT_BYTE_LIMIT,
                 secret_source=None):
        require(matches(r"[A-Za-z0-9_-]{1,64}", document), "request.invalid", "Invalid document identifier.")
        require(type(terminal_limit) is int and 1 <= terminal_limit <= 256, "request.limit", "Invalid ledger quota.")
        self.project = Path(project).resolve()
        require(self.project.is_dir(), "path.missing", "Project directory is unavailable.")
        self.root = self.project / STATE_ROOT
        self.document, self.clock, self.owner_start = document, clock, owner_start
        require(type(byte_limit) is int and 1024 <= byte_limit <= PROJECT_BYTE_LIMIT,
                "request.limit", "Invalid project byte quota.")
        self.byte_limit = byte_limit
        self.recover_cancel, self.terminal_limit = recover_cancel, terminal_limit
        self.mutex, self.active, self.closed = threading.RLock(), None, False
        self.identity = None
        self.secret_source = secret_source or (lambda: configured_tokens(self.project))

    def _reject_secrets(self, *payloads):
        for token in self.secret_source():
            reject_secret(token, *payloads)

    def _path(self, name, *, directory=False):
        path = self.root
        require(not path_is_link(path), "path.symlink", "MG2 state root cannot be a link.")
        for part in Path(name).parts:
            require(part not in ("..", ".") and not Path(part).is_absolute(), "path.escape", "Invalid ledger path.")
            path /= part
            require(not path_is_link(path), "path.symlink", "MG2 state ancestry cannot be a link.")
        if self.identity is not None:
            info = self.root.stat()
            require((info.st_dev, info.st_ino) == self.identity, "path.symlink", "MG2 state root was replaced.")
        if directory:
            durable_mkdir(path)
        return path

    def _read(self, name):
        return bounded_json(read_bounded(self._path(name), 1024**2))

    def retain(self, name, data):
        decoded = bounded_json(data) if name.endswith(".json") or name == ".meshgen.lock" else data
        self._reject_secrets(decoded, data)
        path = self._path(name)
        used = sum(member.stat().st_size for member in self.root.rglob("*") if member.is_file())
        # Include the temporary publication bytes while the prior version is
        # still present. The admission reservation leaves transaction headroom.
        require(used + len(data) <= self.byte_limit, "request.limit", "Project retained byte quota is full.")
        durable_write(path, data)

    def _write(self, name, value):
        data = canonical(value) + b"\n"
        self._reject_secrets(value, data)
        require(len(data) <= 1024**2, "request.limit", "Ledger record exceeds its bound.")
        self.retain(name, data)

    @contextmanager
    def _guard(self):
        self._path("", directory=True)
        info = self.root.stat()
        if self.identity is None:
            self.identity = info.st_dev, info.st_ino
        try:
            guard = ProjectLock(self.root, self._path(".meshgen.guard"))
        except OSError as error:
            raise Refusal("busy", "Another process is updating the MG2 ledger.") from error
        try:
            yield
        finally:
            guard.close()

    def _record_name(self, job_id, document=None):
        require(matches(r"[0-9a-f]{32}", job_id), "request.id", "Invalid MG2 job identifier.")
        document = document or self.document
        require(matches(r"[A-Za-z0-9_-]{1,64}", document), "request.id", "Invalid MG2 document identifier.")
        return f"{document}/jobs/{job_id}/running.json"

    def _validate_record(self, record, document, identifier):
        require(isinstance(record, dict), "request.conflict", "Malformed running marker.")
        require(record.get("job_id") == identifier and matches(r"[0-9a-f]{64}", record.get("plan_digest", ""))
                and matches(r"(?:generate|import):[A-Za-z0-9_.:-]{1,119}", record.get("request_id", ""))
                and record.get("state") in TERMINAL | {"queued", "running", "finishing", "cancelling", "blocked"},
                "request.conflict", "Invalid running marker identity or state.")
        owner = record.get("owner")
        require(isinstance(owner, dict) and type(owner.get("pid")) is int and owner["pid"] > 0
                and isinstance(owner.get("start_time"), str) and bool(owner["start_time"])
                and matches(r"[0-9a-f]{32}", owner.get("nonce", ""))
                and owner.get("job_id") == identifier and owner.get("document") == document
                and type(owner.get("started_at")) in (int, float), "request.conflict", "Invalid marker owner.")
        if record["state"] in TERMINAL:
            base = f"{document}/jobs/{identifier}"
            try:
                data = read_bounded(self._path(base + "/terminal.json"), 1024**2)
                identity = read_bounded(self._path(base + "/terminal.sha256"), 65).decode("ascii").strip()
                terminal = bounded_json(data)
                require(digest(data) == identity and all(terminal.get(key) == record[key]
                        for key in ("job_id", "request_id", "plan_digest", "state")),
                        "request.conflict", "Terminal marker does not match its durable commit.")
                require(not record.get("backend_job_id") or (record.get("task_drained") is True and record.get("backend_stopped") is True),
                        "request.conflict", "Terminal backend ownership has no stopped/drained evidence.")
            except (OSError, UnicodeError, AttributeError) as error:
                raise Refusal("request.conflict", "Terminal marker has no validated commit.") from error
        return record

    def _records(self):
        records = []
        for document in self._path("").iterdir():
            if document.name.startswith(".") or not document.is_dir():
                continue
            jobs = self._path(f"{document.name}/jobs")
            if not jobs.is_dir():
                continue
            for directory in jobs.iterdir():
                name = self._record_name(directory.name, document.name)
                if self._path(name).is_file():
                    records.append((document.name, self._validate_record(self._read(name), document.name, directory.name)))
                else:
                    # Count even a crash before the first marker. No identity
                    # can safely be inferred for replay; keep it as quota usage.
                    require(directory.is_dir(), "request.conflict", "Malformed job directory.")
        return records

    def _reservation(self, record, document):
        name = f"{document}/requests/{digest(record['request_id'].encode())}.json"
        expected = {key: record[key] for key in ("request_id", "plan_digest", "job_id")}
        if self._path(name).exists():
            require(self._read(name) == expected, "request.conflict", "Reservation differs from its running marker.")
        else:
            self._write(name, expected)

    def _recover_locked(self):
        records = self._records()
        for document, record in records:
            self._reservation(record, document)
        lock_path = self._path(".meshgen.lock")
        if lock_path.exists():
            try:
                candidate = self._read(".meshgen.lock")
            except (ValueError, OSError, TypeError):
                # Complete markers are the durable authority for an interrupted
                # legacy empty/truncated owner publication.
                candidate = None
            if candidate is not None:
                matching = [record for _, record in records if record["owner"] == candidate]
                if matching:
                    require(len(matching) == 1, "request.conflict", "Ambiguous owner markers.")
                else:
                    require(not isinstance(candidate, dict) or not {"pid", "start_time", "nonce", "job_id", "started_at", "document"} <= candidate.keys(),
                            "busy", "Owner has no validated marker; recovery cannot establish safety.")
        blocking = []
        for document, record in records:
            if record["state"] in TERMINAL:
                continue
            live = self.owner_start(record["owner"]["pid"]) == record["owner"]["start_time"]
            if live and not (record["state"] == "blocked" and record.get("task_drained") is True):
                blocking.append(record["owner"])
            elif not self._recover_record(record, document):
                blocking.append(record["owner"])
        require(len(blocking) <= 1, "busy", "Multiple unreconciled MG2 owners block admission.")
        if blocking:
            self._write(".meshgen.lock", blocking[0])
            return blocking[0]
        durable_remove(lock_path)
        return None

    def _recover_record(self, record, document):
        if record.get("backend_job_id"):
            try:
                require(self.recover_cancel is not None, "backend.unavailable", "Backend stop cannot be confirmed.")
                self.recover_cancel(record)
            except Exception as error:
                refusal = error if isinstance(error, Refusal) else Refusal("backend.unavailable", "Backend cancellation is unconfirmed.")
                self._reject_secrets(refusal.finding)
                record.update(state="blocked", task_drained=True,
                    errors=[refusal.finding])
                self._write(self._record_name(record["job_id"], document), record)
                return False
        record.update(task_drained=True, backend_stopped=True)
        base = f"{document}/jobs/{record['job_id']}"
        if self._path(base + "/terminal.sha256").is_file():
            data = read_bounded(self._path(base + "/terminal.json"), 1024**2)
            identity = read_bounded(self._path(base + "/terminal.sha256"), 65).decode("ascii").strip()
            require(digest(data) == identity, "request.conflict", "Interrupted terminal result is corrupt.")
            terminal = bounded_json(data)
            if terminal["state"] in ("failed", "cancelled"):
                record.update(terminal)
                record.update(task_drained=True, backend_stopped=True)
                self._write(self._record_name(record["job_id"], document), record)
                return True
        record.update(state=record.get("pending_terminal", "failed"),
            errors=record.get("pending_errors", [Refusal("backend.unavailable", "MG2 owner disappeared before candidate commit.").finding]))
        self._publish(record, document)
        return True

    def _usage(self):
        records = self._records()
        identities = {(path.parent.parent.name, path.name) for path in self.root.glob("*/jobs/*") if path.is_dir()}
        for path in self.root.glob("*/requests/*"):
            require(path.is_file() and not path_is_link(path), "request.conflict", "Invalid reservation member.")
            value = self._read(str(path.relative_to(self.root)))
            keys(value, ("request_id", "plan_digest", "job_id"))
            require(isinstance(value["request_id"], str) and re.fullmatch(r"(?:generate|import):[A-Za-z0-9_.:-]{1,119}", value["request_id"])
                    and matches(r"[0-9a-f]{64}", value["plan_digest"])
                    and path.name == digest(value["request_id"].encode()) + ".json", "request.conflict", "Invalid reservation identity.")
            self._record_name(value["job_id"], path.parent.parent.name)
            identities.add((path.parent.parent.name, value["job_id"]))
        used = 0
        for path in self.root.rglob("*"):
            require(not path_is_link(path), "path.symlink", "Linked ledger member.")
            if path.is_file():
                used += path.stat().st_size
        # Charge promised retention before uploading; later documents cannot
        # consume capacity reserved by an outstanding job.
        for document, record in records:
            if record["state"] not in TERMINAL:
                base = self._path(f"{document}/jobs/{record['job_id']}")
                retained = sum(path.stat().st_size for path in base.rglob("*") if path.is_file())
                used += max(0, record.get("retained_budget", 0) - retained)
        return len(identities), used

    def recover(self):
        if not self.root.exists():
            return None
        with self.mutex, self._guard():
            return self._recover_locked()

    def begin(self, request_id, plan_digest, *, retained_budget=0, backend_context=None):
        require(not self.closed, "cancelled", "MG2 owner is closed.")
        require(isinstance(request_id, str) and re.fullmatch(r"(?:generate|import):[A-Za-z0-9_.:-]{1,119}", request_id),
                "request.id", "Use a stage-specific MG2 request identifier.")
        require(matches(r"[0-9a-f]{64}", plan_digest), "request.invalid", "Invalid plan digest.")
        self._reject_secrets(request_id, plan_digest, self.document, backend_context)
        with self.mutex, self._guard():
            owner = self._recover_locked()
            request_name = f"{self.document}/requests/{digest(request_id.encode())}.json"
            if self._path(request_name).exists():
                previous = self._read(request_name)
                require(previous["request_id"] == request_id and previous["plan_digest"] == plan_digest,
                        "request.conflict", "Request ID is already bound to a different plan digest.")
                require(self._path(self._record_name(previous["job_id"])).is_file(),
                        "request.conflict", "Reservation has no recoverable marker; evidence is retained.")
                record = self.status(previous["job_id"])
                return self.result(previous["job_id"]) if record["state"] in TERMINAL else record
            if owner:
                raise Refusal("busy", f"MG2 job {owner['job_id']} has owned the project since {owner['started_at']}.")
            records, used = self._usage()
            require(records < min(self.terminal_limit, PROJECT_RECORD_LIMIT), "request.limit", "Project request ledger is full; entries are never evicted.")
            require(type(retained_budget) is int and retained_budget >= 0 and used + retained_budget + 16384 <= self.byte_limit,
                    "request.limit", "Project retained byte quota is full.")
            identifier = uuid.uuid4().hex
            owner = {"pid": os.getpid(), "start_time": self.owner_start(os.getpid()), "nonce": uuid.uuid4().hex,
                     "job_id": identifier, "started_at": self.clock(), "document": self.document}
            require(owner["start_time"] is not None, "busy", "Cannot identify the current MG2 owner.")
            self._path(f"{self.document}/jobs/{identifier}", directory=True)
            # Marker and request reservation are durable before any backend work.
            record = {"request_id": request_id, "plan_digest": plan_digest, "job_id": identifier,
                      "owner": owner, "state": "queued", "errors": [], "retained_budget": retained_budget}
            if backend_context is not None:
                keys(backend_context, ("profile", "backend_id", "backend_endpoint", "config_digest", "overrides"))
                record.update(deepcopy(backend_context))
            # Own admission before its first publication. No backend task can
            # exist yet, so rollback under the guard safely removes all writes,
            # including a write that raised after its rename reached disk.
            self.active = identifier
            try:
                self._write(self._record_name(identifier), record)
                self._write(request_name, {"request_id": request_id, "plan_digest": plan_digest, "job_id": identifier})
                self._write(".meshgen.lock", owner)
            except Exception as error:
                try:
                    for name in (".meshgen.lock", request_name, self._record_name(identifier)):
                        durable_remove(self._path(name))
                    directory = self._path(f"{self.document}/jobs/{identifier}")
                    directory.rmdir()
                    sync_directory(directory.parent)
                    self.active = None
                except Exception:
                    # Keep a known drained owner if rollback itself fails.
                    # Recovery may retry it even while this PID remains live.
                    record.update(state="blocked", task_drained=True, backend_stopped=True,
                        pending_terminal="failed", pending_errors=[Refusal("backend.unavailable", "MG2 admission publication failed.").finding])
                    self._write(self._record_name(identifier), record)
                if isinstance(error, Refusal):
                    raise
                raise Refusal("backend.unavailable", "MG2 admission publication failed.") from error
            return deepcopy(record)

    def status(self, identifier):
        record = self._read(self._record_name(identifier))
        if record["state"] in TERMINAL:
            self.result(identifier)  # Validate terminal bytes/hash even on status.
        else:
            require(identifier == self.active, "busy", "Active jobs can only be queried by their original owner.")
        return record

    def result(self, identifier):
        base = f"{self.document}/jobs/{identifier}"
        self._record_name(identifier)
        data = read_bounded(self._path(base + "/terminal.json"), 1024**2)
        identity = read_bounded(self._path(base + "/terminal.sha256"), 65).decode("ascii").strip()
        require(digest(data) == identity, "request.conflict", "Terminal result bytes differ from their commit hash.")
        return bounded_json(data)

    def cancel(self, identifier):
        return self.finish(identifier, "cancelled", errors=[Refusal("cancelled", "MG2 owner cancelled the job.").finding])

    def update(self, identifier, **changes):
        with self.mutex:
            require(identifier == self.active, "session.unauthorized", "Only the job owner may mutate it.")
            record = self._read(self._record_name(identifier))
            if record["state"] not in TERMINAL:
                record.update(changes)
                self._write(self._record_name(identifier), record)
            return record

    def _publish(self, record, document=None):
        document = document or self.document
        base = f"{document}/jobs/{record['job_id']}"
        terminal = {key: value for key, value in record.items() if key != "owner"}
        self._write(base + "/terminal.json", terminal)
        data = read_bounded(self._path(base + "/terminal.json"), 1024**2)
        # The terminal hash is the publication commit, written before the marker
        # switches state. Interrupted publication can be repeated during recovery.
        self.retain(base + "/terminal.sha256", (digest(data) + "\n").encode())
        self._write(self._record_name(record["job_id"], document), record)

    def finish(self, identifier, state, *, result=None, errors=()):
        require(state in TERMINAL, "request.invalid", "Invalid terminal transition.")
        with self.mutex, self._guard():
            record = self._read(self._record_name(identifier))
            if record["state"] in TERMINAL:
                return self.result(identifier)
            require(identifier == self.active, "session.unauthorized", "Only the job owner may finish it.")
            require(not record.get("backend_job_id") or (record.get("task_drained") is True and record.get("backend_stopped") is True),
                    "busy", "Owned backend work must be confirmed stopped and drained before terminal publication.")
            record.update(state=state, errors=list(errors))
            if result is not None:
                record["result"] = result
            self._publish(record)
            if self._path(".meshgen.lock").exists():
                owner = self._read(".meshgen.lock")
                require(owner["nonce"] == record["owner"]["nonce"], "busy", "MG2 owner lock changed.")
            durable_remove(self._path(".meshgen.lock"))
            self.active = None
            return self.result(identifier)

    def commit_success(self, identifier, commit):
        """E4 hook: validate and seal in the same transition as cancellation.

        commit is trusted parent code, never request-selected code. It owns the
        prepared receipt / seal transaction and recovery of a committed seal.
        """
        with self.mutex:
            record = self.status(identifier)
            if record["state"] in TERMINAL:
                return self.result(identifier)
            require(identifier == self.active and record["state"] == "finishing", "stage.out_of_order", "Candidate is not ready to commit.")
            return self.finish(identifier, "succeeded", result=commit())


def read_notes(project, policy):
    records = [(record.get("note_id"), record.get("note_sha256"))
               for record in policy.get("attestations", {}).values() if record.get("value")]
    records += [(record["counsel_note_id"], record["text_sha256"]) for record in policy.get("counsel_notes", [])]
    notes = {}
    for identity, _ in records:
        require(isinstance(identity, str) and re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]{0,63}", identity),
                "config.type", "Invalid policy note identity.")
        try:
            notes[identity] = read_bounded(relative_path(project, f"authoring/meshgen/notes/{identity}.md"), 1024**2)
        except (Refusal, OSError):
            continue  # Missing or linked note bytes never verify an attestation.
    return notes


class BackendBroker:
    """Small backend-only API. Finishing and candidate publication require E4."""
    def __init__(self, project, *, document="_headless", session="headless", clock=time.monotonic,
                 loader=load_config, client_factory=BackendClient, owner_start=process_start, executor=None):
        self.project = Path(project).resolve()
        self.session, self.clock, self.loader, self.client_factory = session, clock, loader, client_factory
        self.plans, self.clients, self.raw = {}, {}, {}
        self.futures, self.tasks = {}, {}
        self.closed, self.mutex = False, threading.RLock()
        self._stop_event = threading.Event()
        self._tokens, self._token_sources = set(), {}
        self.ledger = RequestLedger(project, document=document, owner_start=owner_start, recover_cancel=self._recover_cancel,
                                    secret_source=self._ledger_tokens)
        # The broker owns and shuts down the supplied executor, too.
        self.executor = executor if executor is not None else ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="meshgen-backend")

    def _client(self, effective):
        backend = effective["backends"][effective["backend"]["id"]]
        token = token_from_source(self.project, backend["token_source"])
        self._tokens.add(token)
        self._token_sources[digest(canonical(backend["token_source"]))] = deepcopy(backend["token_source"])
        connect = min(effective["timeouts"]["connect_s"], backend.get("timeouts", {}).get("connect_s", 30))
        return self.client_factory(backend["endpoint"], token, connect_s=connect, clock=self.clock,
            json_limit=effective["limits"]["response_bytes"], json_depth=effective["limits"]["json_depth"],
            collection_items=effective["limits"]["collection_items"])

    def _ledger_tokens(self):
        # Credentials are never stored. Resolve current sources at every
        # publication, and also exclude tokens already used by owned clients.
        return self._tokens | configured_tokens(self.project, self.loader) | {
            token_from_source(self.project, source) for source in tuple(self._token_sources.values())}

    def _matching_backend(self, record):
        resolved = self.loader(self.project, record["profile"], overrides=record.get("overrides"))
        effective = resolved.effective
        backend_id = effective["backend"]["id"]
        require(backend_id == record.get("backend_id")
                and effective["backends"][backend_id]["endpoint"] == record.get("backend_endpoint")
                and resolved.config_digest == record.get("config_digest"),
                "backend.pin_mismatch", "Owned backend endpoint or configuration changed; ownership remains blocked.")
        return effective

    def _recover_cancel(self, record):
        self._confirm_stop(self._client(self._matching_backend(record)), record["backend_job_id"])

    @staticmethod
    def _confirm_stop(client, backend_job):
        response = client.cancel(backend_job)
        require(isinstance(response, dict) and response.get("job_id") == backend_job
                and response.get("state") == "cancelled", "backend.unavailable", "Backend stop is unconfirmed.")

    def plan(self, request):
        require(not self.closed, "cancelled", "Broker is closed.")
        validate_request(request)
        resolved = self.loader(self.project, request["asset_class"], overrides=request.get("overrides"))
        effective = resolved.effective
        validate_request(request, effective)
        backend_id = request["backend_id"]
        backend = effective["backends"][backend_id]
        require(backend["kind"] == "mock", "backend.unavailable", "Real backend execution is disabled in P1 E3.")
        require(effective["candidates"]["select"] == "manual", "backend.unavailable", "Automatic selection needs measured scoring evidence.")
        client = self._client(effective)
        card = client.capabilities()
        reject_secret(getattr(client, "token", None), card)
        card_digest = validate_capability(card, backend_id, backend)
        policy = effective["policy"]["licence"]
        licence = gate(card, policy, read_notes(self.project, policy))
        require(licence["runnable"], licence["findings"][0]["rule_id"] if licence["findings"] else "licence.status_not_allowed",
                "Licence gate refused backend admission.")
        snapshot = image_snapshot(self.project, request["image_ref"], effective["limits"], policy["reference_image_provenance"])
        require(snapshot["format"] in card["inputs"]["image"]["formats"]
                and len(snapshot["bytes"]) <= card["inputs"]["image"]["max_bytes"], "request.limit", "Image is unsupported by the pinned backend.")
        require(not request.get("prompt") or card["inputs"]["prompt"]["supported"], "request.invalid", "Backend does not support prompts.")
        reject_secret(getattr(client, "token", None), request, snapshot["bytes"], effective, licence)
        params = {**backend.get("params", {}), **effective["backend"].get("params", {}), **request.get("params", {})}
        validate_backend_params(params, card["params_schema"])
        deadline_s = min(effective["timeouts"]["job_s"], backend.get("timeouts", {}).get("job_s", 1800),
                         card["limits"]["job_deadline_s_default"], card["limits"]["job_deadline_s_max"])
        count = effective["candidates"]["count"]
        seeds = candidate_seeds(request["seed"], count) if effective["candidates"]["seed_policy"] == "fixed" else (
            [request["seed"]] + [secrets.randbits(32) for _ in range(count-1)])
        content = {"request": deepcopy(request), "config_digest": resolved.config_digest, "capability_digest": card_digest,
                   "licence_digest": licence["sha256"], "recipe_digest": digest(b"meshgen.backend-only.e3.v1"),
                   "session": self.session, "project": str(self.project), "seeds": seeds}
        plan_digest = digest(canonical(content))
        identifier = plan_digest[:32]
        with self.mutex:
            require(not self.closed, "cancelled", "Broker was closed during planning.")
            require(identifier in self.plans or len(self.plans) < effective["limits"]["plans"], "request.limit", "MG2 plan quota exceeded.")
            self.plans[identifier] = {**content, "effective": effective, "card": card, "licence": licence,
                "snapshot": snapshot, "params": params, "deadline_s": deadline_s, "plan_digest": plan_digest}
        return {"plan_id": identifier, "plan_digest": plan_digest, "config_digest": resolved.config_digest,
                "licence": licence, "eligibility": candidate_eligibility(licence, backend["kind"], backend_id),
                "warnings": resolved.findings, "execution_scope": "backend_only"}

    def start(self, identifier, plan_digest):
        with self.mutex:
            require(not self.closed and identifier in self.plans, "stale.plan", "Plan is unavailable in this owner session.")
            plan = self.plans[identifier]
            require(plan_digest == plan["plan_digest"], "request.conflict", "Plan digest differs.")
            require(plan["session"] == self.session and plan["project"] == str(self.project), "stale.plan", "Session or project changed.")
            current = self.loader(self.project, plan["request"]["asset_class"], overrides=plan["request"].get("overrides"))
            require(current.config_digest == plan["config_digest"], "stale.plan", "Configuration changed since planning.")
            client = self._client(current.effective)
            self.ledger._reject_secrets(plan, self.ledger.document)
            self.ledger.terminal_limit = min(self.ledger.terminal_limit, plan["effective"]["limits"]["terminal_requests"])
            limits = plan["effective"]["limits"]
            budget = len(plan["seeds"]) * (len(plan["snapshot"]["bytes"]) + limits["result_bytes"]
                      + limits["response_bytes"] + limits["log_bytes"] + 16384)
            context = {"profile": plan["request"]["asset_class"], "backend_id": plan["request"]["backend_id"],
                "backend_endpoint": current.effective["backends"][plan["request"]["backend_id"]]["endpoint"],
                "config_digest": current.config_digest, "overrides": plan["request"].get("overrides")}
            record = self.ledger.begin(plan["request"]["request_id"], plan_digest, retained_budget=budget, backend_context=context)
            if record["state"] in TERMINAL or record["job_id"] != self.ledger.active:
                return record
            if record["state"] != "queued":
                return record
            job = record["job_id"]
            self.clients[job] = client
            completed = Future()
            self.futures[job] = completed
            try:
                self.ledger.update(job, state="running")
                task = self.executor.submit(self._run, job, plan)
                self.tasks[job] = task
                task.add_done_callback(lambda owned: self._drained(job, owned, completed))
            except Exception:
                # There is no executor task to drain. Complete the wait handle
                # even if a further disk error interrupts terminal cleanup.
                completed.set_result(None)
                self.ledger.finish(job, "failed", errors=[Refusal("backend.unavailable", "Backend admission or executor submission failed.").finding])
                return self.ledger.result(job)
            return {"job_id": record["job_id"], "state": "running"}

    def _run(self, identifier, plan):
        backend_job = None
        client = None
        deadline = self.clock() + plan["deadline_s"]
        try:
            client = self.clients[identifier]
            live = client.capabilities(deadline=deadline)
            reject_secret(getattr(client, "token", None), live)
            require(validate_capability(live, plan["request"]["backend_id"], plan["effective"]["backends"][plan["request"]["backend_id"]])
                    == plan["capability_digest"], "backend.pin_mismatch", "Complete capability changed since planning.")
            current = self.loader(self.project, plan["request"]["asset_class"], overrides=plan["request"].get("overrides"))
            require(current.config_digest == plan["config_digest"], "stale.plan", "Configuration changed since planning.")
            policy = current.effective["policy"]["licence"]
            require(gate(live, policy, read_notes(self.project, policy))["sha256"] == plan["licence_digest"],
                    "backend.pin_mismatch", "Licence evidence changed since planning.")
            snapshot = image_snapshot(self.project, plan["request"]["image_ref"], plan["effective"]["limits"],
                                      plan["effective"]["policy"]["licence"]["reference_image_provenance"])
            require(snapshot["bytes"] == plan["snapshot"]["bytes"], "request.conflict", "Reference image changed since planning.")
            raw_candidates = []
            for index, seed in enumerate(plan["seeds"]):
                request = deepcopy(plan["request"])
                request["seed"] = seed
                if index:
                    request["request_id"] = "generate:" + digest((plan["request"]["request_id"] + ":" + str(index)).encode())
                self._check_cancel(identifier)
                # The pinned mock's deterministic identity is persisted before
                # POST, closing the submit/owner-marker crash window.
                backend_job = digest(request["request_id"].encode())[:32]
                self.ledger.update(identifier, backend_job_id=backend_job)
                reported_id = client.submit(submission(request, snapshot, plan["params"], plan["deadline_s"]), deadline=deadline)
                require(reported_id == backend_job, "backend.protocol", "Pinned mock returned an unexpected job identity.")
                self.ledger.update(identifier, backend_job_id=backend_job)
                self._check_cancel(identifier)
                while True:
                    require(self.clock() < deadline, "backend.deadline", "Backend job deadline expired.")
                    self._check_cancel(identifier)
                    status = client.status(backend_job, deadline=deadline)
                    if status["state"] in TERMINAL:
                        break
                    self.stop_event.wait(0.02)
                require(status["state"] == "succeeded", "backend.unavailable", "Backend did not produce a result.")
                data = client.result(backend_job, deadline=deadline, limit=plan["effective"]["limits"]["result_bytes"])
                metadata = status["result"]
                validate_result(metadata)
                require(metadata["backend_reported"]["seed"] == seed
                        and metadata["backend_reported"]["peak_vram_mib"] is None,
                        "backend.protocol", "Mock result seed or GPU evidence is inconsistent.")
                require(len(metadata["logs_tail"].encode()) <= plan["effective"]["limits"]["log_bytes"],
                        "backend.limit", "Backend logs exceed the resolved limit.")
                token = getattr(client, "token", None)
                reject_secret(token, live, status, metadata, data, snapshot["bytes"], plan["effective"], plan["licence"])
                require(len(data) == metadata["bytes"] and digest(data) == metadata["sha256"], "backend.protocol", "Result bytes differ from metadata.")
                inspect_raw_glb(data, plan["effective"])
                require(self.clock() < deadline, "backend.deadline", "Job deadline expired during raw intake.")
                raw_candidates.append({"image": snapshot["bytes"], "glb": data, "capabilities": live,
                    "result": metadata, "licence": plan["licence"], "effective_config": plan["effective"], "seed": seed})
            require(self.session == plan["session"] and str(self.project) == plan["project"], "stale.plan", "Session or project changed during inference.")
            with self.ledger.mutex:
                self._check_cancel(identifier)
                for index, raw in enumerate(raw_candidates):
                    base = f"{self.ledger.document}/jobs/{identifier}/" + (f"raw-{index}/inputs" if index else "inputs")
                    self.ledger._path(base, directory=True)
                    for name, payload in (("reference_image", raw["image"]), ("raw_backend.glb", raw["glb"]),
                            ("capabilities.json", canonical(live)), ("result.json", canonical(raw["result"])),
                            ("backend.log", raw["result"]["logs_tail"].encode())):
                        reject_secret(getattr(client, "token", None), payload)
                        self.ledger.retain(base + "/" + name, payload)
                self.raw[identifier] = {**raw_candidates[0], "candidates": raw_candidates}
                self.ledger.update(identifier, state="finishing", raw_sha256=digest(raw_candidates[0]["glb"]), seeds=plan["seeds"])
        except Exception as error:
            refusal = error if isinstance(error, Refusal) else Refusal("backend.protocol", "Backend orchestration could not validate or retain the result.")
            try:
                reject_secret(getattr(client, "token", None), refusal.finding)
            except Refusal:
                refusal = Refusal("backend.protocol", "Backend refusal contained a credential.")
            return "failed", [refusal.finding]
        return None

    def _check_cancel(self, identifier):
        record = self.ledger.status(identifier)
        require(not record.get("cancel_requested"), "cancelled", "MG2 owner cancelled the job.")

    def _drained(self, identifier, task, completed):
        # Called only after the executor Future is done: the POST and every
        # receive are drained before cancellation confirmation or lock release.
        try:
            with self.mutex:
                outcome = task.result()
                record = self.ledger.status(identifier)
                if record.get("cancel_requested"):
                    outcome = ("cancelled", [Refusal("cancelled", "MG2 owner cancelled the job.").finding])
                self.ledger.update(identifier, task_drained=True, backend_stopped=outcome is None)
                if outcome:
                    self._complete_stopped(identifier, *outcome)
            completed.set_result(None)
        except Exception as error:
            completed.set_exception(error)

    def _complete_stopped(self, identifier, state, errors):
        with self.ledger.mutex:
            record = self.ledger.status(identifier)
            if record["state"] in TERMINAL:
                return self.ledger.result(identifier)
            self.ledger.update(identifier, task_drained=True, pending_terminal=state, pending_errors=errors)
            if record.get("backend_job_id"):
                try:
                    self._matching_backend(record)
                    self._confirm_stop(self.clients[identifier], record["backend_job_id"])
                except Exception as error:
                    refusal = error if isinstance(error, Refusal) else Refusal("backend.unavailable",
                        "Owned task drained; backend cancellation is unconfirmed. Retry cancellation or recovery.")
                    return self.ledger.update(identifier, state="blocked", errors=[Refusal(
                        refusal.finding["rule_id"], "Owned task drained; backend identity or cancellation is unconfirmed. Retry cancellation or recovery.").finding])
            self.ledger.update(identifier, backend_stopped=True)
            self.raw.pop(identifier, None)
            for index in range(4):
                base = f"{self.ledger.document}/jobs/{identifier}/" + (f"raw-{index}/inputs" if index else "inputs")
                for name in ("reference_image", "raw_backend.glb", "capabilities.json", "result.json", "backend.log"):
                    durable_remove(self.ledger._path(base + "/" + name))
            return self.ledger.finish(identifier, state, errors=errors)

    @property
    def stop_event(self):
        if not hasattr(self, "_stop_event"):
            self._stop_event = threading.Event()
        return self._stop_event

    def cancel(self, identifier):
        with self.mutex, self.ledger.mutex:
            record = self.ledger.status(identifier)
            if record["state"] in TERMINAL:
                return self.ledger.result(identifier)
            self.ledger.update(identifier, cancel_requested=True, state="cancelling")
            future = self.futures.get(identifier)
        # Never wait while holding the mutex needed by the drained callback.
        if future:
            future.result()
        with self.mutex:
            record = self.ledger.status(identifier)
            if record["state"] in TERMINAL:
                return self.ledger.result(identifier)
            return self._complete_stopped(identifier, "cancelled", [Refusal("cancelled", "MG2 owner cancelled the job.").finding])

    def close(self):
        with self.mutex:
            self.closed = True
            self.stop_event.set()
            identifier = self.ledger.active
        if identifier:
            self.cancel(identifier)
        self.executor.shutdown(wait=True)
        self.ledger.closed = True

    def on_eof(self):
        """Inherited-pipe session owners call this on EOF or shutdown; no detach."""
        self.close()

    def __enter__(self):
        self.ledger.recover()
        return self

    def __exit__(self, *args):
        self.close()


def reject_secret(token, *payloads):
    if not token:
        return
    needle = token.encode("ascii")
    pending = list(payloads)
    while pending:
        value = pending.pop()
        if isinstance(value, (bytes, bytearray)):
            require(needle not in value, "backend.protocol", "Retained payload contains a credential.")
            # Inspect decoded GLB JSON as well as its raw bytes.
            if value[:4] == b"glTF" and len(value) >= 20:
                size = raw_glb_container(value, {"metadata_bytes": 1024**2})
                pending.append(bounded_json(value[20:20+size]))
        elif isinstance(value, str):
            require(token not in value, "backend.protocol", "Decoded observation contains a credential.")
        elif isinstance(value, dict):
            pending.extend(value.keys())
            pending.extend(value.values())
        elif isinstance(value, (tuple, list)):
            pending.extend(value)


def raw_glb_container(data, limits):
    """Validate supported chunk headers before slicing or collecting payloads."""
    import struct
    require(len(data) >= 20, "glb.header", "Truncated raw GLB.")
    magic, version, length = struct.unpack_from("<4sII", data)
    require(magic == b"glTF" and version == 2 and length == len(data),
            "glb.header", "Expected a complete glTF 2.0 binary snapshot.")
    offset, count, json_size = 12, 0, None
    while offset < length:
        require(offset+8 <= length, "glb.chunk", "Truncated GLB chunk header.")
        size, kind = struct.unpack_from("<II", data, offset)
        require(count < 2 and kind == (0x4e4f534a if count == 0 else 0x004e4942),
                "glb.chunk", "This profile supports JSON followed by an optional BIN chunk.")
        require(size % 4 == 0 and size <= length-offset-8, "glb.chunk", "Invalid GLB chunk extent.")
        if count == 0:
            require(size <= limits["metadata_bytes"], "glb.limit", "GLB JSON metadata exceeds its bound.")
            json_size = size
        offset += 8+size
        count += 1
    return json_size


def inspect_raw_glb(data, effective):
    """Minimal bounded static triangle intake; finisher decisions belong to E4/G2."""
    import math
    import struct
    from .formats import validate_glb
    require(len(data) <= effective["limits"]["result_bytes"], "backend.limit", "Raw GLB exceeds result cap.")
    json_size = raw_glb_container(data, effective["limits"])
    bounded_json(data[20:20+json_size], depth=effective["limits"]["json_depth"], items=effective["limits"]["collection_items"])
    document = validate_glb(data)
    require(not document.get("skins") and not document.get("animations"), "glb.geometry", "Static geometry is required.")
    pending = [document]
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            require(not value.get("extensions"), "glb.encoding", "GLB extensions are unsupported in E3 intake.")
            pending.extend(value.values())
        elif isinstance(value, list):
            pending.extend(value)
    require(not document.get("extensionsRequired") and not document.get("extensionsUsed"),
            "glb.encoding", "GLB extension encodings are unsupported.")
    def collection(name):
        value = document.get(name, [])
        require(isinstance(value, list) and len(value) <= effective["limits"]["collection_items"]
                and all(isinstance(item, dict) for item in value), "glb.structure", "GLB collection is malformed or exceeds its bound.")
        return value
    views, accessors = collection("bufferViews"), collection("accessors")
    binary_at = 20 + json_size
    require(binary_at + 8 <= len(data), "glb.geometry", "Raw GLB has no binary geometry.")
    binary = data[binary_at+8:]
    require(len(document.get("buffers", [])) == 1 and type(document["buffers"][0].get("byteLength")) is int
            and 0 < document["buffers"][0]["byteLength"] <= len(binary)
            and len(binary) - document["buffers"][0]["byteLength"] <= 3, "glb.bounds", "Invalid GLB buffer size.")
    binary = binary[:document["buffers"][0]["byteLength"]]
    expanded = 0

    def accessor(index, vector=False):
        nonlocal expanded
        require(type(index) is int and 0 <= index < len(accessors), "glb.bounds", "Invalid accessor index.")
        item = accessors[index]
        require("sparse" not in item and "extensions" not in item and item.get("normalized", False) is False,
                "glb.encoding", "Sparse, compressed and normalized geometry is unsupported.")
        count, view_index = item.get("count"), item.get("bufferView")
        require(type(count) is int and 0 < count <= effective["limits"]["glb_accessor_elements"]
                and type(view_index) is int and 0 <= view_index < len(views), "glb.bounds", "Accessor bounds are invalid.")
        expanded += count
        require(expanded <= effective["limits"]["glb_accessor_elements"], "glb.limit", "Accessor expansion exceeds bounds.")
        view = views[view_index]
        require(type(view.get("buffer")) is int and view["buffer"] == 0 and not view.get("extensions"), "glb.encoding", "Invalid or compressed buffer view.")
        width, fmt = (12, "3f") if vector else {5121: (1, "B"), 5123: (2, "H"), 5125: (4, "I")}.get(item.get("componentType"), (0, ""))
        require(width > 0 and item.get("type") == ("VEC3" if vector else "SCALAR")
                and (not vector or item.get("componentType") == 5126), "glb.encoding", "Unsupported position/index accessor.")
        offset, start, size, stride = item.get("byteOffset", 0), view.get("byteOffset", 0), view.get("byteLength"), view.get("byteStride", width)
        require(all(type(value) is int and value >= 0 for value in (offset, start, size, stride))
                and stride >= width and stride <= 252 and start+size <= len(binary)
                and offset+(count-1)*stride+width <= size, "glb.bounds", "Accessor escapes its buffer view.")
        for i in range(count):
            values = struct.unpack_from("<" + fmt, binary, start+offset+i*stride)
            require(all(math.isfinite(value) for value in values), "glb.nonfinite", "Non-finite mesh coordinates.")
            yield values

    triangles = vertices = 0
    mesh_counts = []
    for mesh in collection("meshes"):
        mesh_triangles = mesh_vertices = 0
        require(isinstance(mesh.get("primitives"), list) and len(mesh["primitives"]) <= effective["limits"]["collection_items"]
                and all(isinstance(item, dict) for item in mesh["primitives"]),
                "glb.limit", "Primitive collection is invalid or exceeds its bound.")
        for primitive in mesh.get("primitives", []):
            require(isinstance(primitive.get("attributes"), dict), "glb.structure", "Primitive attributes must be an object.")
            require(set(primitive["attributes"]) == {"POSITION"}, "glb.encoding",
                    "Only POSITION attributes are supported by E3 intake.")
            require(primitive.get("mode", 4) == 4 and not primitive.get("extensions") and not primitive.get("targets"),
                    "glb.encoding", "Only uncompressed static triangles are supported.")
            count = sum(1 for _ in accessor(primitive.get("attributes", {}).get("POSITION"), True))
            vertices += count
            mesh_vertices += count
            indices = 0
            if "indices" in primitive:
                for index, in accessor(primitive["indices"]):
                    require(index < count, "glb.bounds", "Triangle index exceeds position count.")
                    indices += 1
            else:
                indices = count
            require(indices % 3 == 0, "glb.geometry", "Incomplete triangle indices.")
            triangles += indices // 3
            mesh_triangles += indices // 3
            require(triangles <= effective["budgets"]["max_tris_prefinish"], "glb.limit", "Raw triangle count exceeds the pre-finish cap.")
        mesh_counts.append((mesh_triangles, mesh_vertices))
    # Count scene instances as well as accessor data. Reusing a mesh in many
    # nodes must not bypass the pre-finish triangle ceiling.
    nodes, scenes = collection("nodes"), collection("scenes")
    scene = document.get("scene", 0)
    require(isinstance(nodes, list) and isinstance(scenes, list)
            and len(nodes) <= effective["limits"]["collection_items"]
            and len(scenes) <= effective["limits"]["collection_items"]
            and type(scene) is int and 0 <= scene < len(scenes), "glb.bounds", "Invalid bounded scene graph.")
    roots = scenes[scene].get("nodes", [])
    require(isinstance(roots, list) and len(roots) <= effective["limits"]["collection_items"], "glb.structure", "Invalid scene root nodes.")
    pending = [(index, 0) for index in roots]
    seen, instance_triangles, instance_vertices = set(), 0, 0
    while pending:
        index, depth = pending.pop()
        require(type(index) is int and 0 <= index < len(nodes) and index not in seen
                and depth <= effective["limits"]["json_depth"], "glb.bounds", "Scene graph is cyclic, shared or out of bounds.")
        seen.add(index)
        node = nodes[index]
        require("skin" not in node, "glb.encoding", "Skinned nodes are unsupported.")
        for name, size in (("translation", 3), ("rotation", 4), ("scale", 3), ("matrix", 16)):
            if name in node:
                values = node[name]
                require(isinstance(values, list) and len(values) == size
                        and all(type(value) in (int, float) and abs(value) <= 1e9 and math.isfinite(value) for value in values),
                        "glb.nonfinite", "Node transform is invalid or non-finite.")
        if "mesh" in node:
            mesh = node["mesh"]
            require(type(mesh) is int and 0 <= mesh < len(mesh_counts), "glb.bounds", "Invalid scene mesh reference.")
            tris, verts = mesh_counts[mesh]
            instance_triangles += tris
            instance_vertices += verts
        children = node.get("children", [])
        require(isinstance(children, list) and len(children) <= effective["limits"]["collection_items"], "glb.limit", "Too many child nodes.")
        pending.extend((child, depth+1) for child in children)
        require(instance_triangles <= effective["budgets"]["max_tris_prefinish"]
                and instance_vertices <= effective["limits"]["glb_accessor_elements"], "glb.limit", "Instanced geometry exceeds its bound.")
    triangles, vertices = instance_triangles, instance_vertices
    require(triangles > 0 and vertices > 0, "glb.geometry", "Empty geometry is refused.")
    return {"triangles": triangles, "vertices": vertices}
