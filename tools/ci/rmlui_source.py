#!/usr/bin/env python3
"""Deliver the reviewed RmlUi source inside an exclusive, fresh build workspace.

The workspace excludes non-cooperating namespace writers. Interrupted or unknown
state requires a new workspace; this tool never adopts, resets or cleans it.
"""
from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import stat
import subprocess
import sys
import threading
import uuid

REVISION = "58c751531f4c1a1eb2dc56f409b8e8a323a0c9ad"
PATCH_SHA = "1b49aa929bc7411d7d26059135fd029341c0cc769c4b588388ad66a57fb3f196"
SOURCE_ID = "60304a1ae56d9a21c2e0db0853d02b5946bc4ac2030aa8f21ef84d9ae752f1de"
UPSTREAM = "https://github.com/mikke89/RmlUi.git"
POLICY = "fresh-exclusive-workspace-v1"
FILE_COUNT = 1118
SOURCE_BYTES = 21004671
# Bounds cover the complete pinned source, Git administration and command output.
STORAGE_BYTES = 128 * 1024 * 1024
STORAGE_ENTRIES = 8192
OUTPUT_BYTES = 4 * 1024 * 1024
HEX = re.compile(r"[0-9a-f]{64}\Z")
REPARSE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


class Refusal(ValueError):
    """Input or workspace does not satisfy the delivery contract."""


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise Refusal(reason)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ordinary(info: os.stat_result, directory: bool = False) -> bool:
    return (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)) and not (
        getattr(info, "st_file_attributes", 0) & REPARSE)


def read_file(path: Path, bound: int) -> bytes:
    require(ordinary(os.lstat(path)), f"not an ordinary file: {path.name}")
    # Nonblocking open prevents a substituted FIFO from blocking admission.
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(ordinary(info) and info.st_nlink == 1 and info.st_size <= bound,
                f"invalid file size or links: {path.name}")
        data = stream.read(bound + 1)
        require(len(data) == info.st_size and len(data) <= bound, f"partial file: {path.name}")
        return data


def safe_relative(name: str) -> None:
    require(isinstance(name, str) and bool(name) and "\\" not in name and ":" not in name,
            "invalid relative path")
    parts = name.split("/")
    require(all(p and p not in {".", ".."} and not p.endswith((" ", ".")) and "~" not in p
                and not re.fullmatch(r"(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", p)
                for p in parts), "nonportable relative path")


def canonical(raw: str | Path) -> Path:
    text = str(raw)
    require(bool(text) and len(text) <= 32768, "empty or excessive workspace path")
    windows = PureWindowsPath(text)
    require(not windows.drive or windows.is_absolute(), "drive-relative path")
    require(not {".", ".."}.intersection(text.replace("\\", "/").split("/")), "parent or dot path alias")
    require(os.name == "nt" or (not windows.drive and "\\" not in text), "foreign platform path alias")
    path = Path(os.path.abspath(text))
    require(path == path.resolve(), "workspace path alias")
    for part in [path, *path.parents]:
        if os.path.lexists(part):
            info = os.lstat(part)
            require(ordinary(info, True) or (part == path and ordinary(info)), "workspace symlink or reparse path")
    if os.name == "nt":
        existing = path
        while not existing.exists():
            existing = existing.parent
        check_windows_spelling(str(existing), windows_long_name(str(existing)))
    return path


def windows_long_name(path: str) -> str:
    import ctypes
    buffer = ctypes.create_unicode_buffer(32768)
    length = ctypes.windll.kernel32.GetLongPathNameW(path, buffer, len(buffer))
    require(0 < length < len(buffer), "unreadable Windows long path")
    return buffer.value


def check_windows_spelling(path: str, long_name: str) -> None:
    require(bool(long_name) and PureWindowsPath(path) == PureWindowsPath(long_name),
            "workspace short-name alias or unreadable long path")


def inventory(root: Path, ignore_git: bool = False, allowed_directories: set[str] | None = None) -> dict[str, os.stat_result]:
    result = {}
    pending = [root]
    entries = size = 0
    while pending:
        directory = pending.pop()
        require(ordinary(os.lstat(directory), True), "nonordinary source directory")
        with os.scandir(directory) as scan:
            for entry in scan:
                entries += 1
                require(entries <= STORAGE_ENTRIES, "source entry bound exceeded")
                name = Path(entry.path).relative_to(root).as_posix()
                safe_relative(name)
                info = os.lstat(entry.path)
                if ignore_git and name == ".git":
                    require(ordinary(info, True), "Git administration must be a local directory")
                    continue
                if ordinary(info, True):
                    require(allowed_directories is None or name in allowed_directories, "extra source directory")
                    pending.append(Path(entry.path))
                else:
                    require(ordinary(info) and info.st_nlink == 1, "nonordinary source entry")
                    size += info.st_size
                    require(size <= STORAGE_BYTES, "source storage bound exceeded")
                    result[name] = info
    return result


class Specification:
    def __init__(self, manifest: Path, patch: Path):
        self.manifest_bytes = read_file(canonical(manifest), 1024 * 1024)
        self.patch = canonical(patch)
        self.patch_bytes = read_file(self.patch, 22655)
        require(len(self.patch_bytes) == 22655 and digest(self.patch_bytes) == PATCH_SHA, "patch identity mismatch")
        self.data = json.loads(self.manifest_bytes)
        m = self.data
        require(m["schema"] == 1 and m["revision"] == REVISION and m["patch_sha256"] == PATCH_SHA
                and m["source_identity"] == SOURCE_ID and m["file_count"] == FILE_COUNT
                and m["pristine_bytes"] == SOURCE_BYTES, "manifest identity mismatch")
        require(digest(f"RmlUi-source-v1\n{REVISION}\n{PATCH_SHA}\n".encode()) == SOURCE_ID,
                "source identity mismatch")
        self.files = m["files"]
        require(len(self.files) == FILE_COUNT, "manifest file count mismatch")
        aliases = set()
        for name, entry in self.files.items():
            safe_relative(name)
            require(name.casefold() not in aliases, "manifest path alias")
            aliases.add(name.casefold())
            require(entry["mode"] == "100644" and re.fullmatch(r"[0-9a-f]{40}", entry["blob"])
                    and HEX.fullmatch(entry["pristine_sha256"]) and HEX.fullmatch(entry["patched_sha256"]),
                    "invalid manifest file identity")
            for key in ("size", "patched_size"):
                require(type(entry[key]) is int and 0 <= entry[key] <= SOURCE_BYTES, "invalid manifest size")
        require(sum(e["size"] for e in self.files.values()) == SOURCE_BYTES
                and sum(e["patched_size"] for e in self.files.values()) == m["patched_bytes"], "manifest byte total mismatch")
        self.delivery = dict(schema=1, policy=POLICY, revision=REVISION, source_identity=SOURCE_ID,
                             patch_sha256=PATCH_SHA, manifest_sha256=digest(self.manifest_bytes),
                             helper_sha256=digest(read_file(Path(__file__).resolve(), 128 * 1024)))

    def verify_files(self, source: Path, patched: bool) -> None:
        directories = {parent.as_posix() for name in self.files for parent in Path(name).parents if parent != Path(".")}
        actual = inventory(source, ignore_git=True, allowed_directories=directories)
        require(set(actual) == set(self.files), "extra or missing source files")
        for name, entry in self.files.items():
            size = entry["patched_size" if patched else "size"]
            require(actual[name].st_size == size, f"source size mismatch: {name}")
            if os.name != "nt":
                require(not actual[name].st_mode & 0o111, f"source mode mismatch: {name}")
            data = read_file(source / name, size)
            require(digest(data) == entry["patched_sha256" if patched else "pristine_sha256"],
                    f"source digest mismatch: {name}")


def git(source: Path, *args: str, input_bytes: bytes | None = None) -> bytes:
    # Exclude inherited Git directories, filters, hooks, credential helpers and config.
    environment = {k.upper(): v for k, v in os.environ.items()
                   if k.upper() in {"PATH", "SYSTEMROOT", "COMSPEC", "PATHEXT", "TEMP", "TMP", "TMPDIR"}}
    environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                       GIT_CONFIG_SYSTEM=os.devnull, GIT_ATTR_NOSYSTEM="1",
                       GIT_NO_REPLACE_OBJECTS="1", GIT_OPTIONAL_LOCKS="0", GIT_TERMINAL_PROMPT="0")
    path_entries = environment.get("PATH", "").split(os.pathsep)
    require(all(entry and Path(entry).is_absolute() for entry in path_entries), "empty or relative executable search path")
    executable = shutil.which("git", path=environment.get("PATH", ""))
    require(executable is not None and Path(executable).is_absolute(), "Git executable unavailable")
    argv = [str(Path(executable).resolve()), "-c", "core.autocrlf=false", "-c", "core.safecrlf=false",
            "-c", "core.hooksPath=" + os.devnull, "-c", "core.fsmonitor=false",
            "-c", "protocol.file.allow=never", "-C", str(source), *args]
    output = bytearray()
    failures = []
    stopped = threading.Event()
    require(input_bytes is None or len(input_bytes) <= 22655, "Git input bound exceeded")
    process = subprocess.Popen(argv, env=environment, stdin=subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    writer = None
    if input_bytes is not None:
        def send_input() -> None:
            try:
                assert process.stdin is not None
                with process.stdin:
                    process.stdin.write(input_bytes)
            except OSError as error:
                failures.append(str(error))
        writer = threading.Thread(target=send_input)
        writer.start()

    def monitor() -> None:
        while not stopped.wait(0.1):
            try:
                inventory(source)
            except FileNotFoundError:
                # Git can rename its own lock and pack files during this scan.
                # The final scan, after Git exits, remains strict.
                continue
            except (OSError, Refusal) as error:
                failures.append(str(error))
                process.kill()
                return

    watcher = threading.Thread(target=monitor)
    watcher.start()
    try:
        assert process.stdout is not None
        with process.stdout:
            while True:
                data = process.stdout.read(65536)
                if not data:
                    break
                require(len(output) + len(data) <= OUTPUT_BYTES, "Git output bound exceeded")
                output.extend(data)
        code = process.wait()
        stopped.set()
        watcher.join()
        if writer is not None:
            writer.join()
        require(not failures and code == 0, "Git command refused: " + " ".join(args[:1]) + "\n" + output[-4096:].decode(errors="replace"))
        inventory(source)
        return bytes(output)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        stopped.set()
        watcher.join()
        if writer is not None:
            writer.join()


def verify_git(source: Path, spec: Specification) -> None:
    admin = source / ".git"
    require(ordinary(os.lstat(admin), True), "external Git administration")
    metadata = inventory(admin)
    for name in metadata:
        require(name in {"HEAD", "config", "description", "index", "FETCH_HEAD", "ORIG_HEAD", "shallow", "logs/HEAD"}
                or re.fullmatch(r"objects/[0-9a-f]{2}/[0-9a-f]{38}", name)
                or re.fullmatch(r"objects/pack/pack-[0-9a-f]{40}\.(pack|idx|rev)", name),
                "unexpected Git administrative file")
    read_file(admin / "index", 1024 * 1024)
    for name in ("commondir", "gitdir", "objects/info/alternates", "objects/info/http-alternates", "info/grafts", "refs/replace"):
        require(not os.path.lexists(admin / name), "shared or substituted Git administration")
    config = configparser.ConfigParser(interpolation=None)
    config.read_string(read_file(admin / "config", 8192).decode("utf-8"))
    require(config.sections() == ["core"] and not config.defaults(), "nonordinary Git configuration")
    allowed = {"repositoryformatversion": {"0"}, "bare": {"false"}, "filemode": {"true", "false"},
               "logallrefupdates": {"true"}, "ignorecase": {"true", "false"}, "symlinks": {"true", "false"}}
    require(all(k in allowed and v.lower() in allowed[k] for k, v in config["core"].items()), "unsafe local Git configuration")
    require(read_file(admin / "HEAD", 128) == (REVISION + "\n").encode(), "HEAD is not the detached pin")
    require(git(source, "rev-parse", "HEAD").strip().decode() == REVISION, "wrong Git revision")
    require(git(source, "rev-parse", "HEAD^{tree}").strip().decode() == spec.data["tree"], "wrong Git tree")
    expected = {name: (e["mode"], e["blob"]) for name, e in spec.files.items()}
    index = {}
    for row in git(source, "ls-files", "--stage", "-z").split(b"\0")[:-1]:
        metadata, name = row.split(b"\t", 1)
        mode, blob, stage = metadata.decode().split()
        require(stage == "0" and name.decode() not in index, "mixed Git index")
        index[name.decode()] = (mode, blob)
    require(index == expected, "Git index differs from pinned manifest")
    flags = git(source, "ls-files", "-v", "-z").split(b"\0")[:-1]
    require(len(flags) == FILE_COUNT and all(r.startswith(b"H ") for r in flags), "hidden index state")
    tree = {}
    for row in git(source, "ls-tree", "-rz", REVISION).split(b"\0")[:-1]:
        metadata, name = row.split(b"\t", 1)
        mode, kind, blob = metadata.decode().split()
        require(kind == "blob", "nonregular Git tree")
        tree[name.decode()] = (mode, blob)
    require(tree == expected, "Git tree differs from manifest")


def write_exclusive(path: Path, data: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def encoded(value: dict) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


class Workspace:
    def __init__(self, root: Path, spec: Specification):
        self.root = canonical(root)
        self.spec = spec
        self.owner_path = self.root / ".rmlui-owner.json"
        self.source = self.root / "_deps" / ("rmlui-" + SOURCE_ID + "-src")
        self.stage = self.source.with_name(self.source.name + ".stage")
        self.population = self.source.with_name("rmlui-" + SOURCE_ID + "-population")
        self.receipt = self.root / ".rmlui-ready.json"
        self.admission = self.root / ".rmlui-admission"

    def initialize(self) -> None:
        require(not os.path.lexists(self.root), "build root already exists; choose a fresh root")
        self.root.parent.mkdir(parents=True, exist_ok=True)
        self.root.mkdir()
        write_exclusive(self.owner_path, encoded(dict(schema=1, root=str(self.root), nonce=uuid.uuid4().hex,
                                                      delivery=self.spec.delivery)))

    def owner(self) -> bytes:
        raw = read_file(self.owner_path, 4096)
        value = json.loads(raw)
        require(raw == encoded(value), "noncanonical ownership record")
        require(set(value) == {"schema", "root", "nonce", "delivery"} and value["schema"] == 1
                and value["root"] == str(self.root) and re.fullmatch(r"[0-9a-f]{32}", value["nonce"])
                and value["delivery"] == self.spec.delivery, "workspace ownership or delivery mismatch")
        return raw

    def expected_receipt(self, owner: bytes) -> dict:
        return dict(schema=1, owner_sha256=digest(owner), delivery=self.spec.delivery, source=str(self.source),
                    head_sha256=digest(read_file(self.source / ".git/HEAD", 128)),
                    index_sha256=digest(read_file(self.source / ".git/index", 1024 * 1024)))

    def verify_population(self) -> None:
        # FetchContent may inspect SOURCE_SUBDIR before returning. Only the
        # empty ordinary population directory is eligible for that operation.
        canonical(self.population)
        if os.path.lexists(self.population):
            require(ordinary(os.lstat(self.population), True), "nonordinary population directory")
            with os.scandir(self.population) as entries:
                require(next(entries, None) is None, "nonempty population directory")

    def verify(self) -> None:
        owner = self.owner()
        self.verify_population()
        require(not os.path.lexists(self.stage) and not os.path.lexists(self.admission), "unfinished source delivery")
        require(canonical(self.source) == self.source, "source path alias")
        receipt_bytes = read_file(self.receipt, 4096)
        receipt = json.loads(receipt_bytes)
        require(receipt_bytes == encoded(receipt), "noncanonical receipt")
        require(receipt == self.expected_receipt(owner), "source receipt mismatch")
        verify_git(self.source, self.spec)
        self.spec.verify_files(self.source, True)

    def preflight(self, disconnected: bool) -> None:
        self.owner()
        canonical(self.source.parent)
        self.verify_population()
        if os.path.lexists(self.receipt):
            self.verify()
            return
        require(not disconnected, "fully disconnected source unavailable")
        require(not any(os.path.lexists(p) for p in (self.source, self.stage, self.admission)),
                "ambiguous or preexisting source; choose a fresh build root")

    def publish(self, owner: bytes) -> None:
        # Caller owns the exclusive admission; all payloads precede readiness.
        require(read_file(self.owner_path, 4096) == owner, "ownership changed during delivery")
        verify_git(self.stage, self.spec)
        self.spec.verify_files(self.stage, False)
        git(self.stage, "apply", "--unidiff-zero", "-p1", "--check", input_bytes=self.spec.patch_bytes)
        git(self.stage, "apply", "--unidiff-zero", "-p1", input_bytes=self.spec.patch_bytes)
        verify_git(self.stage, self.spec)
        self.spec.verify_files(self.stage, True)
        require(not os.path.lexists(self.source) and not os.path.lexists(self.receipt), "publication destination occupied")
        os.rename(self.stage, self.source)
        receipt = self.expected_receipt(owner)
        write_exclusive(self.receipt, encoded(receipt))
        # Only this invocation creates/removes admission, under workspace exclusivity.
        self.admission.unlink()

    def download(self) -> None:
        self.preflight(False)
        if os.path.lexists(self.receipt):
            return
        write_exclusive(self.admission, b"exclusive source delivery\n")
        owner = self.owner()
        require(not any(os.path.lexists(p) for p in (self.source, self.stage, self.receipt)), "source state changed")
        canonical(self.stage.parent)
        self.stage.parent.mkdir(exist_ok=True)
        self.stage.mkdir()
        git(self.stage, "init", "--template=")
        git(self.stage, "fetch", "--depth=1", "--no-tags", UPSTREAM, REVISION)
        git(self.stage, "-c", "advice.detachedHead=false", "checkout", "--detach", REVISION)
        self.publish(owner)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("init", "preflight", "download", "verify"))
    parser.add_argument("--root", required=True)
    parser.add_argument("--fully-disconnected", action="store_true")
    args = parser.parse_args(argv)
    repository = Path(__file__).resolve().parents[2]
    try:
        spec = Specification(repository / "vendor/rmlui/rmlui-6.1-source.json",
                             repository / "cmake/patches/rmlui-6.1-shadow.patch")
        workspace = Workspace(args.root, spec)
        if args.operation == "init":
            workspace.initialize()
        elif args.operation == "preflight":
            workspace.preflight(args.fully_disconnected)
        elif args.operation == "verify":
            workspace.verify()
        else:
            if args.fully_disconnected:
                workspace.preflight(True)
            else:
                workspace.download()
        return 0
    except (OSError, ValueError, KeyError, TypeError, configparser.Error) as error:
        print(f"RmlUi source refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
