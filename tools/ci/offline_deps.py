#!/usr/bin/env python3
"""Provision, verify and copy pinned FetchContent sources for offline checks."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MAX_GIT_REDIRECTIONS = 32


class DependencyError(ValueError):
    pass


def declarations(root: Path) -> dict:
    text = (root / "vendor/CMakeLists.txt").read_text(encoding="utf-8")
    text = re.sub(r"#[^\n]*", "", text)
    matches = re.findall(r"FetchContent_Declare\s*\(([^()]*)\)", text, re.I)
    if len(matches) != len(re.findall(r"FetchContent_Declare\s*\(", text, re.I)):
        raise DependencyError("unsupported FetchContent declaration")
    result = {}
    for body in matches:
        tokens = shlex.split(body)
        name = tokens[0].lower()
        if not re.fullmatch(r"[a-z][a-z0-9_]*", name) or name in result:
            raise DependencyError("invalid or duplicate dependency name")
        fields = dict(zip(tokens[1::2], tokens[2::2]))
        if (len(tokens) % 2 != 1 or set(fields) - {
            "GIT_REPOSITORY", "GIT_TAG", "GIT_SHALLOW", "SOURCE_SUBDIR"
        } or not fields.get("GIT_REPOSITORY", "").startswith("https://")
                or not fields.get("GIT_TAG")):
            raise DependencyError(f"unsupported dependency declaration: {name}")
        result[name] = {"repository": fields["GIT_REPOSITORY"], "revision": fields["GIT_TAG"]}
    if not result:
        raise DependencyError("no FetchContent dependencies found")
    return result


def unresolved_lock(root: Path) -> dict:
    return {"version": 1, "status": "unresolved", "dependencies": {
        name: {**decl, "commit": None, "sha256": None, "submodules": None}
        for name, decl in declarations(root).items()
    }}


def contained(path: Path, parent: Path) -> Path:
    try:
        resolved = path.resolve()
    except RuntimeError as error:
        raise DependencyError(f"cyclic path: {path}") from error
    if resolved == parent.resolve() or not resolved.is_relative_to(parent.resolve()):
        raise DependencyError(f"path must be strictly inside {parent.name}: {path}")
    return resolved


def source_digest(source: Path) -> str:
    """Hash sorted paths, types, executable bits and bytes; omit Git metadata.

    Only relative symlinks resolving inside this source are accepted. Copying
    them therefore preserves their meaning without exposing shared files.
    """
    if source.is_symlink() or not source.is_dir():
        raise DependencyError(f"missing source directory: {source.name}")
    digest = hashlib.sha256()

    def record(*parts):
        for part in parts:
            data = part if isinstance(part, bytes) else os.fsencode(part)
            digest.update(len(data).to_bytes(8, "big"))
            digest.update(data)

    def visit(directory):
        for item in sorted(directory.iterdir(), key=lambda p: os.fsencode(p.name)):
            if item.name == ".git":
                continue
            rel = item.relative_to(source).as_posix()
            mode = item.lstat().st_mode
            if stat.S_ISLNK(mode):
                target = os.readlink(item)
                if os.path.isabs(target) or not item.resolve().is_relative_to(source.resolve()):
                    raise DependencyError(f"source symlink escapes: {rel}")
                if not item.exists():
                    raise DependencyError(f"dangling source symlink: {rel}")
                record("link", rel, target)
            elif stat.S_ISDIR(mode):
                record("dir", rel)
                visit(item)
            elif stat.S_ISREG(mode):
                content = hashlib.sha256()
                with item.open("rb") as handle:
                    for block in iter(lambda: handle.read(1 << 20), b""):
                        content.update(block)
                record("file", rel, str(mode & 0o111), content.digest())
            else:
                raise DependencyError(f"unsupported source file: {rel}")

    visit(source)
    return digest.hexdigest()


def git_alternate_paths(data: bytes, *, windows: bool) -> list[str]:
    """Accept Windows CRLF records without changing POSIX pathname semantics.

    Windows text writers translate LF to CRLF; CR cannot occur in an ordinary
    Windows filename. POSIX Git retains CR as a pathname byte. Preserve embedded
    CR, an unpaired final CR, whitespace and Unicode separators on both platforms.
    """
    if windows:
        data = data.replace(b"\r\n", b"\n")
    result = []
    for line in data.split(b"\n"):
        if not line or line.startswith(b"#"):
            continue
        # Quoted encodings need Git's C-string parser; refuse rather than
        # validate a different path from Git's input.
        if line.startswith(b'"') or b"\0" in line:
            raise DependencyError("unsupported Git alternate path encoding")
        result.append(os.fsdecode(line))
    return result


def git_metadata(source: Path, cache: Path) -> None:
    """Require self-contained metadata, including submodule gitdir files.

    Follow internal directory links only once. Resolve every link before Git
    sees it, even links in otherwise unused metadata such as hooks and refs.
    """
    marker = source / ".git"
    if marker.is_symlink():
        raise DependencyError(f"symlinked Git identity: {source.name}")
    seen = set()

    def visit(path):
        if path.is_symlink():
            contained(path, cache)
            if not path.exists():
                raise DependencyError("dangling Git metadata symlink")
        resolved = contained(path, cache)
        if resolved in seen:
            return
        seen.add(resolved)
        if path.is_dir():
            for child in sorted(path.iterdir(), key=lambda p: os.fsencode(p.name)):
                visit(child)
        elif not path.is_file():
            raise DependencyError("unsupported Git metadata file")

    active, complete = set(), {}

    def redirect(kind, path, depth):
        # Recheck a reused subtree if reached at greater depth, so a diamond
        # cannot conceal an overlong path. Active nodes distinguish cycles.
        if depth > MAX_GIT_REDIRECTIONS:
            raise DependencyError("Git metadata redirection depth exceeded")
        resolved = contained(path, cache)
        key = (kind, resolved)
        if key in active:
            raise DependencyError("cyclic Git metadata redirection")
        if complete.get(key, -1) >= depth:
            return
        active.add(key)
        visit(resolved)
        if kind == "metadata" and resolved.is_file():
            text = resolved.read_text(encoding="utf-8")
            match = re.fullmatch(r"gitdir: ([^\r\n\0]+)\r?\n?", text)
            if not match:
                raise DependencyError(f"invalid submodule Git identity: {source.name}")
            redirect("metadata", resolved.parent / match[1], depth + 1)
        elif not resolved.is_dir():
            raise DependencyError(f"missing source Git identity: {source.name}")
        elif kind == "metadata":
            common = resolved / "commondir"
            if common.exists():
                target = common.read_text(encoding="utf-8").rstrip("\r\n")
                if not target or any(c in target for c in "\r\n\0"):
                    raise DependencyError("invalid Git common directory")
                redirect("metadata", resolved / target, depth + 1)
            objects = resolved / "objects"
            if objects.exists():
                redirect("objects", objects, depth + 1)
        else:
            alternates = resolved / "info/alternates"
            if alternates.exists():
                for target in git_alternate_paths(alternates.read_bytes(), windows=os.name == "nt"):
                    redirect("objects", resolved / target, depth + 1)
        active.remove(key)
        complete[key] = depth

    redirect("metadata", marker, 0)


def git(source: Path, *args: str, cache: Path, network=False) -> str:
    if args != ("init", "--quiet"):
        git_metadata(source, cache)
    # Do not inherit config injection, repository redirection, tracing, helpers
    # or transport commands from the caller. Local config is still read by Git,
    # so command-line overrides disable its executable behaviour as well.
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith("GIT_")}
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
               GIT_TERMINAL_PROMPT="0", GIT_OPTIONAL_LOCKS="0",
               GIT_NO_REPLACE_OBJECTS="1", GIT_NO_LAZY_FETCH="1",
               GIT_ATTR_NOSYSTEM="1", GIT_ALLOW_PROTOCOL="https" if network else "")
    config = ("core.fsmonitor=false", f"core.hooksPath={os.devnull}",
              f"core.sshCommand={os.devnull}", f"core.askPass={os.devnull}",
              f"core.attributesFile={os.devnull}", f"core.excludesFile={os.devnull}",
              "credential.helper=", "credential.interactive=false",
              "submodule.recurse=false", "protocol.allow=never",
              "protocol.file.allow=never", "protocol.ssh.allow=never",
              "protocol.ext.allow=never",
              "protocol.http.allow=never",
              "protocol.https.allow=" + ("always" if network else "never"),
              "init.templateDir=", "core.pager=cat", "core.useReplaceRefs=false")
    options = [option for value in config for option in ("-c", value)]
    # Preserve NUL-framed tree records and trailing whitespace in paths.
    return subprocess.check_output(
        ["git", *options, *args], cwd=source, stderr=subprocess.PIPE, env=env,
    ).decode("utf-8", errors="surrogateescape")


def head(source: Path, cache: Path) -> str:
    # ^0 peels to a commit without braces that MSYS argument globbing expands.
    return git(source, "rev-parse", "--verify", "HEAD^0", cache=cache).strip()


def submodules(source: Path, cache: Path) -> dict:
    """Read committed gitlinks, then verify each initialized checkout's HEAD."""
    result = {}

    def visit(checkout):
        tree = git(checkout, "ls-tree", "-rz", "HEAD", cache=cache)
        if tree and not tree.endswith("\0"):
            raise DependencyError("unparseable submodule tree")
        for record in tree.split("\0")[:-1]:
            match = re.fullmatch(r"([0-7]{6}) (blob|tree|commit) ([0-9a-f]{40})\t(.+)", record, re.S)
            if not match:
                raise DependencyError("unparseable submodule tree")
            mode, kind, commit, path = match.groups()
            if mode != "160000":
                continue
            if kind != "commit":
                raise DependencyError("invalid submodule gitlink")
            child = checkout / path
            contained(child, checkout)
            if child.is_symlink() or not child.is_dir() or not (child / ".git").exists():
                raise DependencyError(f"incomplete submodules in {checkout.name}")
            git_metadata(child, cache)
            if head(child, cache) != commit:
                raise DependencyError(f"wrong submodule HEAD: {path}")
            result[child.relative_to(source).as_posix()] = commit
            visit(child)

    visit(source)
    return result


def glad_inputs(source: Path) -> None:
    for name in ("glad/__main__.py", "glad/files/gl.xml", "glad/files/khrplatform.h"):
        if not (source / name).is_file():
            raise DependencyError(f"missing bundled GLAD input: {name}")


def verify(root: Path, cache: Path, lock_path: Path) -> dict:
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    expected = declarations(root)
    if not isinstance(lock, dict):
        raise DependencyError("invalid dependency lock")
    deps = lock.get("dependencies", {})
    if lock.get("version") != 1 or lock.get("status") != "resolved":
        raise DependencyError("unresolved lock; owner must run snapshot with network access")
    if not isinstance(deps, dict) or set(deps) != set(expected):
        raise DependencyError("lock dependency set differs from vendor declarations")
    for name, declaration in expected.items():
        entry = deps[name]
        if not isinstance(entry, dict):
            raise DependencyError(f"invalid dependency entry: {name}")
        if any(entry.get(k) != v for k, v in declaration.items()):
            raise DependencyError(f"wrong declared dependency: {name}")
        if (not re.fullmatch(r"[0-9a-f]{40}", entry.get("commit") or "")
                or not re.fullmatch(r"[0-9a-f]{64}", entry.get("sha256") or "")
                or not isinstance(entry.get("submodules"), dict)):
            raise DependencyError(f"unresolved dependency: {name}")
        source = cache / name
        contained(source, cache)
        if source_digest(source) != entry["sha256"]:
            raise DependencyError(f"source digest mismatch: {name}")
        if re.fullmatch(r"[0-9a-f]{40}", declaration["revision"]) and declaration["revision"] != entry["commit"]:
            raise DependencyError(f"lock commit differs from pinned revision: {name}")
        if not (source / ".git").is_dir() or (source / ".git").is_symlink():
            raise DependencyError(f"missing source Git identity: {name}")
        git_metadata(source, cache)
        if head(source, cache) != entry["commit"]:
            raise DependencyError(f"wrong resolved revision: {name}")
        if submodules(source, cache) != entry["submodules"]:
            raise DependencyError(f"wrong submodule revisions: {name}")
        if name == "glad":
            glad_inputs(source)
    return lock


def snapshot(root: Path, cache: Path, lock_path: Path) -> None:
    # Fresh directories avoid reusing stale or partially populated sources.
    if cache.exists() and any(cache.iterdir()):
        raise DependencyError("snapshot requires an empty cache directory")
    cache.mkdir(parents=True, exist_ok=True)
    lock = unresolved_lock(root)
    for name, entry in lock["dependencies"].items():
        source = cache / name
        source.mkdir()
        git(source, "init", "--quiet", cache=cache)
        git(source, "remote", "add", "origin", entry["repository"], cache=cache)
        git(source, "fetch", "--depth=1", "--no-tags", "origin", entry["revision"],
            cache=cache, network=True)
        git(source, "checkout", "--quiet", "--detach", "FETCH_HEAD", cache=cache)
        git(source, "submodule", "update", "--init", "--recursive", "--depth=1",
            cache=cache, network=True)
        entry["commit"] = head(source, cache)
        entry["submodules"] = submodules(source, cache)
        if name == "glad":
            glad_inputs(source)
        entry["sha256"] = source_digest(source)
    lock["status"] = "resolved"
    # Verify a temporary lock before atomically replacing the published lock.
    scratch = root / "scratch"
    contained(scratch, root)
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="deps-lock-", dir=scratch) as temporary:
        candidate = Path(temporary) / "lock.json"
        candidate.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
        verify(root, cache, candidate)
        candidate.replace(lock_path)


def cmake_string(value: str) -> str:
    equals = "="
    while "]" + equals + "]" in value:
        equals += "="
    return "[" + equals + "[" + value + "]" + equals + "]"


def remove_stage(dest: Path) -> None:
    """Remove copied read-only directories and Windows read-only files."""
    def writable(path):
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            return
        # Older Windows Python lacks follow_symlinks for chmod. Only chmod
        # non-links there; use explicit no-follow semantics where supported.
        options = {"follow_symlinks": False} if os.chmod in os.supports_follow_symlinks else {}
        os.chmod(path, mode | stat.S_IWUSR, **options)

    def retry(function, path, exc_info):
        if (not isinstance(exc_info[1], PermissionError)
                or function not in (os.unlink, os.remove, os.rmdir)):
            raise exc_info[1]
        path = Path(path)
        # POSIX unlink needs a writable parent; Windows deletion also needs
        # the file's read-only attribute cleared. Never chmod link targets.
        if path != dest:
            writable(path.parent)
        writable(path)
        function(path)

    # onerror is available on every supported Python, including 3.10/3.11.
    shutil.rmtree(dest, onerror=retry)


def stage(root: Path, cache: Path, lock_path: Path, dest: Path, init: Path) -> None:
    scratch = root / "scratch"
    if scratch.is_symlink():
        raise DependencyError("symlinked scratch root")
    contained(scratch, root)

    def destination(path):
        # Inspect the original components before resolve() erases symlinks,
        # including dangling links and links followed by '..'.
        try:
            relative = path.relative_to(root)
        except ValueError as error:
            raise DependencyError("stage paths must be inside the checkout") from error
        current = root
        for part in relative.parts:
            current = current / part
            if current.is_symlink():
                raise DependencyError(f"symlinked stage path component: {current}")
        return contained(path, scratch)

    dest, init = destination(dest), destination(init)
    if (dest.exists() or init.exists() or init.is_relative_to(dest) or dest.is_relative_to(init)
            or dest.is_relative_to(cache.resolve()) or cache.resolve().is_relative_to(dest)
            or init.is_relative_to(cache.resolve())):
        raise DependencyError("stage needs fresh, separate destination and initial cache paths")
    lock = verify(root, cache, lock_path)
    dest.mkdir(parents=True)
    candidate = None
    try:
        lines = []
        for name, entry in lock["dependencies"].items():
            target = dest / name
            shutil.copytree(cache / name, target, symlinks=True,
                            ignore=shutil.ignore_patterns(".git"))
            if source_digest(target) != entry["sha256"]:
                raise DependencyError(f"source changed during copy: {name}")
            lines.append(f'set(FETCHCONTENT_SOURCE_DIR_{name.upper()} '
                         f'{cmake_string(str(target))} CACHE PATH "" FORCE)')
        lines += [f'set(FETCHCONTENT_BASE_DIR {cmake_string(str(dest / "_deps"))} CACHE PATH "" FORCE)',
                  'set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)',
                  'set(GLAD_REPRODUCIBLE ON CACHE BOOL "" FORCE)']
        init.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=init.parent,
                                         prefix=init.name + ".", suffix=".tmp", delete=False) as handle:
            candidate = Path(handle.name)
            handle.write("\n".join(lines) + "\n")
        os.replace(candidate, init)
    except BaseException as error:
        # Attempt both cleanups independently, preserving the initiating error
        # even if storage becomes inaccessible while unwinding.
        try:
            remove_stage(dest)
        except BaseException as cleanup_error:
            if sys.version_info >= (3, 11):
                error.add_note(f"stage cleanup failed: {cleanup_error}")
        try:
            if candidate is not None:
                candidate.unlink(missing_ok=True)
        except BaseException as cleanup_error:
            if sys.version_info >= (3, 11):
                error.add_note(f"initial cache cleanup failed: {cleanup_error}")
        raise


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("snapshot", "verify", "stage"))
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--lock", type=Path, default=Path("tools/ci/deps-lock.json"))
    parser.add_argument("--dest", type=Path, default=Path("scratch/offline-deps"))
    parser.add_argument("--init", type=Path, default=Path("scratch/offline-init.cmake"))
    args = parser.parse_args(argv)
    root = args.root.resolve()
    cache = args.cache.resolve()
    lock = root / args.lock
    try:
        if args.command == "snapshot":
            snapshot(root, cache, lock)
        elif args.command == "verify":
            verify(root, cache, lock)
        else:
            stage(root, cache, lock, root / args.dest, root / args.init)
    except (DependencyError, OSError, ValueError, TypeError, KeyError,
            subprocess.SubprocessError) as error:
        print(f"offline dependencies: {error}", file=sys.stderr)
        return 1
    print(f"offline dependencies: {args.command} complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
