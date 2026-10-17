#!/usr/bin/env python3
"""Host-installed Linux metadata policy; not an execution admission boundary.

The stock runner can execute always()/failure() steps after this hook fails.
Activation requires the external worker boundary in docs/development.md.
Never execute the checkout copy as a host policy.
"""

import json
import os
from pathlib import Path
import re
import stat
import sys


REPOSITORY = "d-addison/luminumbra"
MAX_JSON = 1024 * 1024  # Bound configuration and event reads to one MiB each.
MAX_ENTRIES = 100000  # Bound cleanup work across the entire checkout tree.
MAX_DEPTH = 64


class Denied(ValueError):
    """Metadata or checkout validation failed; the caller must enforce refusal."""


def require(condition, message):
    if not condition:
        raise Denied(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def invalid_constant(value):
    raise Denied("non-JSON numeric constant: " + value)


def read_json(path, directory=None, uid=None):
    # Nonblocking open refuses FIFOs/devices without waiting; inspect and read
    # the same descriptor. Linux production also refuses final symlinks.
    flags = os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
    require(not stat.S_ISLNK(os.stat(path, dir_fd=directory, follow_symlinks=False).st_mode), "JSON path is a symlink")
    with os.fdopen(os.open(path, flags, dir_fd=directory), "rb") as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_size <= MAX_JSON, "invalid JSON file")
        require(uid is None or info.st_uid == uid, "JSON file is not runner owned")
        data = stream.read(MAX_JSON + 1)
    require(len(data) <= MAX_JSON, "JSON file exceeds bound")
    payload = json.loads(data, object_pairs_hook=unique_object, parse_constant=invalid_constant)
    require(isinstance(payload, dict), "JSON object required")
    return payload


def repository(value):
    return isinstance(value, dict) and value.get("full_name") == REPOSITORY


def sha(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{40}", value) is not None


def positive_integer(value):
    return type(value) is int and value > 0


def validate_event(environment, payload):
    require(environment.get("GITHUB_REPOSITORY") == REPOSITORY, "wrong repository")
    require(repository(payload.get("repository")), "missing or wrong event repository")
    event = environment.get("GITHUB_EVENT_NAME")
    if event == "pull_request":
        pr = payload.get("pull_request")
        require(isinstance(pr, dict), "missing pull request")
        head, base = pr.get("head"), pr.get("base")
        require(isinstance(head, dict) and repository(head.get("repo")), "external or missing PR head")
        require(isinstance(base, dict) and repository(base.get("repo")), "wrong PR base")
        require(positive_integer(payload.get("number")) and positive_integer(pr.get("number"))
                and pr["number"] == payload["number"], "wrong PR number")
        require(sha(head.get("sha")), "missing PR head revision")
    elif event == "push":
        ref = payload.get("ref")
        require(isinstance(ref, str) and re.fullmatch(r"refs/(heads|tags)/[^\s]+", ref) is not None, "missing push ref")
        require(sha(payload.get("after")), "missing push revision")
    elif event == "schedule":
        require(isinstance(payload.get("schedule"), str) and bool(payload["schedule"].strip()), "missing schedule")
    elif event == "workflow_run":
        run = payload.get("workflow_run")
        require(isinstance(run, dict), "missing workflow run")
        require(repository(run.get("repository")) and repository(run.get("head_repository")), "external workflow run")
        require(positive_integer(run.get("id")) and sha(run.get("head_sha")), "missing workflow run identity")
        require(isinstance(run.get("head_branch"), str) and bool(run["head_branch"].strip()), "missing workflow branch")
        require(run.get("event") in {"push", "pull_request", "schedule", "workflow_dispatch"}, "unknown upstream event")
        require(payload.get("action") in {"requested", "in_progress", "completed"}, "unknown workflow run action")
    else:
        raise Denied("unsupported event")


def protected_install(path):
    # The script, its companion, its JSON config, and all parents must be owned
    # by root and unwritable by any job account, including through a group.
    for item in (path, *path.parents):
        info = os.lstat(item)
        require(not stat.S_ISLNK(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o022,
                "hook installation is not protected")


def validate_paths(config, environment):
    require(set(config) == {"work_root", "runner_uid", "metadata_verified"}, "invalid host configuration")
    require(config["metadata_verified"] is True, "host has not verified runner metadata")
    uid = config["runner_uid"]
    require(type(uid) is int and uid > 0 and uid == os.getuid(), "wrong runner account")
    raw = config["work_root"]
    require(isinstance(raw, str) and raw.startswith("/") and "\n" not in raw, "invalid work root")
    root = Path(raw)
    require(str(root) == raw and ".." not in root.parts and root.name == "_work" and len(root.parts) >= 5,
            "work root must be a dedicated instance directory")
    require(root.parts[1] in {"srv", "var"}, "work root must be below srv or var")
    workspace = root / "luminumbra" / "luminumbra"
    require(environment.get("GITHUB_WORKSPACE") == str(workspace), "wrong checkout path")
    require(environment.get("RUNNER_TEMP") == str(root / "_temp"), "wrong runner temporary path")
    require(environment.get("GITHUB_EVENT_PATH") == str(root / "_temp/_github_workflow/event.json"),
            "missing or unexpected event path")
    return root, workspace, uid


def open_directory(path, uid):
    # Walk from / without following any symlink, retaining the validated handle.
    descriptor = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in path.parts[1:]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        require(os.fstat(descriptor).st_uid == uid, "directory is not runner owned")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def clean_contents(descriptor, uid, device, budget, depth=0):
    require(depth <= MAX_DEPTH, "checkout depth exceeds bound")
    # scandir streams names rather than allocating a list for an unbounded tree.
    with os.scandir(descriptor) as entries:
        for entry in entries:
            budget[0] -= 1
            require(budget[0] >= 0, "checkout entry count exceeds bound")
            info = os.stat(entry.name, dir_fd=descriptor, follow_symlinks=False)
            require(info.st_uid == uid and info.st_dev == device, "foreign checkout contents")
            if stat.S_ISDIR(info.st_mode):
                child = os.open(entry.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor)
                try:
                    opened = os.fstat(child)
                    require((opened.st_dev, opened.st_ino) == (info.st_dev, info.st_ino), "checkout changed")
                    clean_contents(child, uid, device, budget, depth + 1)
                    current = os.stat(entry.name, dir_fd=descriptor, follow_symlinks=False)
                    require((current.st_dev, current.st_ino) == (opened.st_dev, opened.st_ino), "checkout changed")
                    os.rmdir(entry.name, dir_fd=descriptor)
                finally:
                    os.close(child)
            else:
                # Unlink a symlink itself, never its target (even outside work).
                current = os.stat(entry.name, dir_fd=descriptor, follow_symlinks=False)
                require((current.st_dev, current.st_ino) == (info.st_dev, info.st_ino), "checkout changed")
                os.unlink(entry.name, dir_fd=descriptor)


def admit(environment, installed):
    require(sys.platform == "linux", "host hook requires Linux")
    for name in ("self_hosted_job_hook.sh", "self_hosted_job_hook.py", "self_hosted_job_hook.json"):
        protected_install(installed.parent / name)
    config = read_json(installed.with_suffix(".json"))
    root, workspace, uid = validate_paths(config, environment)
    # Validate the runner-owned parent chain before reading the event. Payload
    # absence at this hook phase is a denial, never a fallback to workflow input.
    descriptors = []
    try:
        for directory in (root, root / "_temp", root / "_temp/_github_workflow"):
            descriptors.append(open_directory(directory, uid))
        payload = read_json("event.json", descriptors[-1], uid)
        validate_event(environment, payload)
        # A first run may have no checkout yet. Validate its parent, then leave
        # creation to checkout. No cleanup touches _temp, _actions or other jobs.
        descriptors.append(open_directory(workspace.parent, uid))
        try:
            checkout = open_directory(workspace, uid)
        except FileNotFoundError:
            return
        descriptors.append(checkout)
        info = os.fstat(checkout)
        clean_contents(checkout, uid, info.st_dev, [MAX_ENTRIES])
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


def main():
    try:
        admit(os.environ, Path(__file__).absolute())
    except (OSError, ValueError, TypeError, RecursionError) as error:
        print(f"Self-hosted job denied: {error}", file=sys.stderr)
        return 1
    print("Self-hosted metadata checks passed; external execution admission required")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
