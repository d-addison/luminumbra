#!/usr/bin/env python3
"""One-shot Linux worker admission for a separately privileged trusted listener.

The caller supplies host-owned launch policy, never workflow environment or paths.
The authenticated listener sends one length-prefixed normalized assignment and
half-closes the channel. The worker adapter reads that exact assignment on stdin.
No stock runner hook calls this module; installation requires a listener adapter.
The caller owns cancellation, descendant containment and reaping after launch.
"""

from dataclasses import dataclass
import json
import math
import os
from pathlib import PurePosixPath
import re
import socket
import stat
import struct
import subprocess
import sys
import time


REPOSITORY = "d-addison/luminumbra"
MAX_JSON = 65536  # Bound the complete normalized assignment, including parsing.
ADMISSION_SECONDS = 5
IDENTITY_FIELDS = {"job_id", "run_id", "run_attempt"}
ASSIGNMENT_FIELDS = IDENTITY_FIELDS | {
    "repository", "event", "head_repository", "base_repository", "revision",
    "head_revision", "ref", "pull_request", "cancelled",
}


class Denied(ValueError):
    """No worker was created. The listener must discard this assignment."""


@dataclass(frozen=True)
class LaunchPlan:
    """Private listener policy; all fields must originate outside the job account."""

    command: tuple[str, ...]
    listener_pid: int
    listener_uid: int
    worker_uid: int
    worker_gid: int
    job_id: str
    run_id: int
    run_attempt: int
    enabled: str = ""


def require(condition, message):
    if not condition:
        raise Denied(message)


def positive_integer(value):
    return type(value) is int and 0 < value <= 2**63 - 1


def valid_job_id(value):
    return (type(value) is str and re.fullmatch(r"[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}", value)
            and value != "00000000-0000-0000-0000-000000000000")


def validate_plan(plan):
    require(type(plan.enabled) is str and plan.enabled.lower() == "true", "LUM_SELF_HOSTED is off")
    require(valid_job_id(plan.job_id) and positive_integer(plan.run_id) and positive_integer(plan.run_attempt),
            "invalid launch identity")
    require(positive_integer(plan.listener_pid), "invalid listener pid")
    require(type(plan.listener_uid) is int and 0 <= plan.listener_uid <= 2**31 - 1, "invalid listener uid")
    require(positive_integer(plan.worker_uid) and plan.worker_uid < 2**31
            and plan.worker_uid != plan.listener_uid, "worker account must be separate")
    require(positive_integer(plan.worker_gid) and plan.worker_gid < 2**31, "invalid worker group")
    require(isinstance(plan.command, tuple) and 1 <= len(plan.command) <= 64, "invalid worker command")
    # At most 64 arguments of 4096 characters; no shell or PATH lookup occurs.
    require(all(type(arg) is str and 0 < len(arg) <= 4096 and "\0" not in arg for arg in plan.command),
            "invalid worker arguments")
    path = PurePosixPath(plan.command[0])
    require(path.root == "/" and str(path) == plan.command[0] and ".." not in path.parts
            and len(path.parts) <= 64, "worker requires a canonical absolute path")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def invalid_constant(_value):
    raise Denied("non-JSON number")


def validate_assignment(data, plan):
    require(0 < len(data) <= MAX_JSON, "invalid assignment size")
    try:
        assignment = json.loads(data.decode("utf-8"), object_pairs_hook=unique_object,
                                parse_constant=invalid_constant)
    except (ValueError, RecursionError) as error:
        raise Denied("invalid assignment JSON") from error
    require(type(assignment) is dict and set(assignment) == ASSIGNMENT_FIELDS, "unknown or missing metadata")
    require(all(assignment[field] == getattr(plan, field) for field in IDENTITY_FIELDS), "assignment binding mismatch")
    require(valid_job_id(assignment["job_id"]), "invalid job id")
    require(positive_integer(assignment["run_id"]) and positive_integer(assignment["run_attempt"]), "invalid run identity")
    for field in ("repository", "head_repository", "base_repository"):
        require(assignment[field] == REPOSITORY, "external or missing repository")
    for field in ("revision", "head_revision"):
        value = assignment[field]
        require(type(value) is str and re.fullmatch(r"[0-9a-f]{40}", value) and value != "0" * 40,
                "invalid revision")
    require(assignment["cancelled"] is False, "assignment cancelled")
    event, ref, number = assignment["event"], assignment["ref"], assignment["pull_request"]
    require(type(ref) is str, "invalid ref")
    if event == "pull_request":
        require(positive_integer(number) and ref == f"refs/pull/{number}/merge", "invalid pull request ref")
    elif event == "push":
        require(number is None and re.fullmatch(r"refs/(heads|tags)/[A-Za-z0-9_][A-Za-z0-9_./-]{0,255}", ref)
                and ".." not in ref and "//" not in ref and "/." not in ref
                and all(not part.endswith((".lock", ".")) for part in ref.split("/")) and not ref.endswith("/")
                and assignment["revision"] == assignment["head_revision"], "invalid push identity")
    else:
        raise Denied("unsupported event")


def remaining(deadline):
    value = deadline - time.monotonic()
    require(math.isfinite(value) and value > 0, "admission deadline expired")
    return value


def receive_assignment(channel, plan, deadline):
    require(channel.family == socket.AF_UNIX and channel.type == socket.SOCK_STREAM, "private Unix stream required")
    credentials = channel.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
    pid, uid, gid = struct.unpack("3i", credentials)
    require((pid, uid) == (plan.listener_pid, plan.listener_uid) and gid != plan.worker_gid, "unauthenticated listener")

    def read_exact(size):
        data = bytearray()
        # Each successful read consumes at least one byte of the 64 KiB budget.
        while len(data) < size:
            channel.settimeout(remaining(deadline))
            block = channel.recv(size - len(data))
            require(bool(block), "partial assignment")
            data.extend(block)
        return bytes(data)

    size = struct.unpack("!I", read_exact(4))[0]
    require(0 < size <= MAX_JSON, "invalid assignment size")
    data = read_exact(size)
    channel.settimeout(remaining(deadline))
    require(channel.recv(1) == b"", "multiple assignments on one channel")
    validate_assignment(data, plan)
    return data


def open_worker(path):
    # Retain directory descriptors and open each component without symlinks.
    # All ancestors and the ELF worker must be root-owned and job-unwritable.
    descriptor = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        parts = PurePosixPath(path).parts[1:]
        for index, part in enumerate(parts):
            parent = os.fstat(descriptor)
            require(parent.st_uid == 0 and not parent.st_mode & 0o022, "unprotected worker directory")
            flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC
            if index + 1 < len(parts):
                flags |= os.O_DIRECTORY
            child = os.open(part, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        info = os.fstat(descriptor)
        require(stat.S_ISREG(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o6022
                and info.st_mode & 0o111, "unprotected worker executable")
        require(os.read(descriptor, 4) == b"\x7fELF", "native ELF worker required")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def launch(channel, plan):
    """Authenticate and admit once, then return the caller-owned worker process.

    Linux only. The root supervisor must itself be installed and started with a
    clean environment and isolated Python before accepting any assignment. The
    listener must never fall back to a stock worker after any exception here.
    """
    require(sys.platform == "linux" and os.geteuid() == 0, "privileged Linux supervisor required")
    validate_plan(plan)
    deadline = time.monotonic() + ADMISSION_SECONDS
    data = receive_assignment(channel, plan, deadline)
    worker = open_worker(plan.command[0])
    try:
        # A sealed, anonymous file binds the admitted bytes to this worker only.
        import fcntl
        assignment_fd = os.memfd_create("admitted-assignment", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
        try:
            require(os.write(assignment_fd, data) == len(data), "partial assignment write")
            os.lseek(assignment_fd, 0, os.SEEK_SET)
            fcntl.fcntl(assignment_fd, fcntl.F_ADD_SEALS,
                        fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
            remaining(deadline)
            return subprocess.Popen(plan.command, executable=f"/proc/self/fd/{worker}",
                                    stdin=assignment_fd, cwd="/", env={"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"},
                                    close_fds=True, pass_fds=(worker,), start_new_session=True,
                                    user=plan.worker_uid, group=plan.worker_gid, extra_groups=(), umask=0o077)
        finally:
            os.close(assignment_fd)
    finally:
        os.close(worker)


if __name__ == "__main__":
    raise SystemExit("Standalone execution denied: a trusted listener adapter is required")
