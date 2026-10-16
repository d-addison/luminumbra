#!/usr/bin/env python3
"""Per-translation-unit clang-tidy result cache for the blocking tidy gate.

The cache can only ever *skip* a clang-tidy run for a translation unit whose
complete analysis input is byte-for-byte identical to an earlier run that
exited 0.  Anything that is missing, unreadable, malformed, or in doubt is a
miss, and a miss runs clang-tidy.  A failing run never writes a record.

The analysis input of one translation unit (the cache key) is the SHA-256 of:

  * FORMAT_VERSION, the contents of this script and of every --tool-file
    (scripts/tidy.sh by default, which also carries the check list);
  * the exact --config JSON handed to clang-tidy for this unit;
  * the clang-tidy executable and every compiler named in the compile
    database: resolved path, SHA-256 of the executable's content, and its
    --version output;
  * for EVERY compile-database entry of the unit: the working directory, the
    full argument vector, and the path and content hash of every file the
    preprocessor reads, obtained by re-running the entry's own compiler
    command, plus clang-tidy's own -D__clang_analyzer__, with -M in this very
    run (so system headers, compiler builtin headers, generated headers, and
    a newly shadowing header are all seen). Response files (@file) are not
    expanded, so a unit that uses one is always a miss.

Every dependency is hashed afresh for every key (no memoization across units; executables
are memoized per stat fingerprint), the key includes the include-path and clock environment
variables, and any unit using clock macros, -U options, vfs overlays, modules, PCH or
response files is never cached,
and the key is recomputed from a freshly loaded compile database after the
analysis; a record is written only if both keys are equal.

Source edits, NOLINT comments, header edits, flag changes, config changes and
toolchain changes therefore all change the key.  Anything this script cannot
read becomes a miss.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import ntpath
import os
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time

FORMAT_VERSION = "tidy-cache-v1"
DEPS_TIMEOUT_SECONDS = 300
TIDY_TIMEOUT_SECONDS = 1800

_DROP_WITH_VALUE = {"-o", "-MF", "-MT", "-MQ", "-MJ", "--serialize-diagnostics"}
_DROP_ALONE = {"-c", "-MD", "-MMD", "-MG", "-MP", "-M", "-MM"}
_DROP_PREFIXES = ("-MF", "-MT", "-MQ", "-MJ")
# Options that make the compiler read files through a path the dependency scan
# and clang-tidy might resolve differently, or that inject arbitrary frontend
# behaviour: a unit using any of them is never cached.
_UNSUPPORTED_PREFIXES = (
    "@", "-ivfsoverlay", "-vfsoverlay", "-fmodule", "-fcxx-modules", "-fprebuilt",
    "-include-pch", "-fpch", "-Xclang", "-cc1", "--sysroot", "-isysroot",
)
# Predefined macros whose value depends on the clock; a file mentioning one is
# never cached, because its analysis can change with no change in its bytes.
_VOLATILE_TOKENS = (b"__TIMESTAMP__", b"__TIME__", b"__DATE__")
# Environment variables the compiler and clang-tidy read when locating headers
# or evaluating time macros.
_ENV_NAMES = (
    "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
    "OBJCPLUS_INCLUDE_PATH", "SDKROOT", "SOURCE_DATE_EPOCH", "TZ",
    "COMPILER_PATH", "GCC_EXEC_PREFIX", "CCC_OVERRIDE_OPTIONS", "CLANG_TIDY_ARGS",
)


class CacheMiss(Exception):
    """The key for this unit cannot be established; run clang-tidy."""


def parse_shard(value: str) -> tuple[int, int]:
    """Parse a 1-based INDEX/COUNT without accepting signs or non-integers."""
    if re.fullmatch(r"[0-9]+/[0-9]+", value):
        index, count = map(int, value.split("/"))
        if 1 <= index <= count:
            return index, count
    raise argparse.ArgumentTypeError("--shard must be INDEX/COUNT with 1 <= INDEX <= COUNT")


def select_shard(files: list[str], repo_root: str, shard: tuple[int, int]) -> list[str]:
    """Assign each repo-relative POSIX path by SHA-256 modulo the shard count.

    The checkout location, input order and other files cannot change a unit's
    shard. Sort and deduplicate the result for deterministic execution order.
    """
    index, count = shard
    selected = []
    for path in sorted(set(files)):
        relative = os.path.relpath(path, repo_root).replace(os.sep, "/")
        digest = hashlib.sha256(relative.encode("utf-8", "surrogateescape")).digest()
        if int.from_bytes(digest, "big") % count + 1 == index:
            selected.append(path)
    return selected


def shard_command(args: argparse.Namespace) -> int:
    for path in select_shard(args.files, args.repo_root, args.shard):
        sys.stdout.buffer.write(os.fsencode(path) + b"\0")
    return 0


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def environment_fingerprint() -> str:
    return json.dumps({name: os.environ.get(name) for name in _ENV_NAMES}, sort_keys=True)


def parse_make_deps(text: str) -> list[str]:
    """Parse the one-rule Makefile output of a compiler's -M option.

    Returns the prerequisites.  Raises CacheMiss for anything that does not
    look like exactly "target: prerequisite..." output.
    """
    if "\t" in text:
        raise CacheMiss("ambiguous dependency output (tab in a file name)")
    joined = text.replace("\\\r\n", " ").replace("\\\n", " ")
    tokens: list[str] = []
    current: list[str] = []
    in_token = False
    index = 0
    while index < len(joined):
        char = joined[index]
        if char == "\\" and index + 1 < len(joined) and joined[index + 1] in " #":
            current.append(joined[index + 1])
            in_token = True
            index += 2
            continue
        if char == "$" and index + 1 < len(joined) and joined[index + 1] == "$":
            current.append("$")
            in_token = True
            index += 2
            continue
        if char in " \t\r\n":
            if in_token:
                tokens.append("".join(current))
                current = []
                in_token = False
            index += 1
            continue
        current.append(char)
        in_token = True
        index += 1
    if in_token:
        tokens.append("".join(current))
    if len(tokens) < 2 or not tokens[0].endswith(":"):
        raise CacheMiss("unparseable dependency output")
    # Exactly one rule: a second "xxx:" token means a multi-rule output.
    deps = tokens[1:]
    for dep in deps:
        if dep.endswith(":"):
            raise CacheMiss("unexpected multi-rule dependency output")
    return deps


def windows_command_arguments(command: str) -> list[str]:
    """Split a Windows command without a shell, using CommandLineToArgvW rules.

    Backslashes are literal except immediately before a double quote: pairs
    produce one slash, and an odd remaining slash escapes the quote. Quote
    runs use the Windows three-quote rule, including within quoted arguments.
    """
    arguments, index = [], 0
    while index < len(command):
        if command[index] in " \t":
            index += 1
            continue
        current, quotes, backslashes = [], 0, 0
        while index < len(command):
            char = command[index]
            if char in " \t" and not quotes:
                break
            if char == "\\":
                current.append(char)
                backslashes += 1
                index += 1
            elif char == '"':
                remove = (backslashes + 1) // 2
                if remove:
                    del current[-remove:]
                if backslashes % 2:
                    current.append('"')
                else:
                    quotes += 1
                index += 1
                backslashes = 0
                while index < len(command) and command[index] == '"':
                    quotes += 1
                    if quotes == 3:
                        current.append('"')
                        quotes = 0
                    index += 1
                if quotes == 2:
                    quotes = 0
            else:
                current.append(char)
                backslashes = 0
                index += 1
        arguments.append("".join(current))
    return arguments


def has_posix_quoted_token(command: str) -> bool:
    """Recognize shell-quoted tokens, excluding native paths and double quotes."""
    index, double_quoted = 0, False
    while index < len(command):
        char = command[index]
        if char == "\\":
            index += 2
            continue
        if char == '"':
            double_quoted = not double_quoted
        elif char == "'" and not double_quoted and (index == 0 or command[index - 1].isspace()):
            try:
                shlex.split(command)
            except ValueError:
                return False
            return True
        index += 1
    return False


def entry_arguments(entry: dict) -> list[str]:
    """Parse using the producer's context; command_platform can override it.

    POSIX shell quoting is also used by MSYS producers with native working
    directories. Prefer that syntax when present, then use the directory or
    native driver path as evidence; an .exe suffix alone is not evidence.
    Explicit markers resolve commands that are valid in both dialects.
    """
    if "arguments" in entry:
        arguments = [str(a) for a in entry["arguments"]]
    elif "command" in entry:
        command = entry["command"]
        windows = windows_command_arguments(command)
        drivers = windows[:1]
        if drivers and ntpath.basename(drivers[0]).casefold().removesuffix(".exe") in {"ccache", "sccache"}:
            drivers += windows[1:2]
        platform = entry.get("command_platform")
        if platform not in {None, "windows", "posix"}:
            raise CacheMiss("command_platform must be windows or posix")
        directory = str(entry.get("directory", ""))
        if platform is None:
            # shlex.join uses single-quoted tokens, including on Windows.
            # Ignore apostrophes inside double quotes or native path words.
            # A complete shell-quoted token is producer evidence stronger than
            # the working directory's path syntax (MSYS uses both forms).
            if has_posix_quoted_token(command):
                platform = "posix"
            elif ntpath.splitdrive(directory)[0]:
                platform = "windows"
            elif directory.startswith("/"):
                platform = "posix"
            elif any(ntpath.splitdrive(driver)[0] or "\\" in driver for driver in drivers):
                platform = "windows"
            else:
                platform = "posix"
        if platform == "windows":
            arguments = windows
        else:
            arguments = shlex.split(command)
    else:
        raise CacheMiss("compile database entry has no command")
    if not arguments:
        raise CacheMiss("empty compile command")
    return arguments


def dependency_command(arguments: list[str]) -> list[str]:
    """The entry's own command with output/dependency-file options replaced by -M."""
    # clang-tidy predefines __clang_analyzer__ before any user -D/-U, so the
    # scan does the same; headers guarded by it must be seen.
    result = [arguments[0], "-D__clang_analyzer__"]
    skip_next = False
    for argument in arguments[1:]:
        if argument.startswith(_UNSUPPORTED_PREFIXES) or argument.startswith("-U"):
            raise CacheMiss(f"unsupported compile option {argument}")
        if skip_next:
            skip_next = False
            continue
        if argument in _DROP_WITH_VALUE:
            skip_next = True
            continue
        if argument in _DROP_ALONE:
            continue
        if argument.startswith(_DROP_PREFIXES):
            continue
        if argument.startswith("-o") and len(argument) > 2:
            continue
        result.append(argument)
    result.append("-M")
    return result


class ToolIdentity:
    """Executable identity: resolved path, content SHA-256 and --version output.

    The (expensive) content hash and version query are memoized per executable
    fingerprint (realpath, kind, device, inode, size, mtime_ns), re-stat'ed for every
    key, so replacing or rebuilding a tool invalidates its identity. A sample
    taken within its mtime's two-second tick is revalidated on reuse until an
    observation after the tick closes establishes a stable identity.
    """

    def __init__(self) -> None:
        self._cache: dict[tuple, tuple[str, int]] = {}
        self._lock = threading.Lock()

    def describe(self, executable: str, directory: str) -> str:
        probe = executable
        if os.sep in probe and not os.path.isabs(probe):
            probe = os.path.join(directory, probe)
        resolved = probe if os.sep in probe else (shutil.which(probe) or "")
        if not resolved or not os.path.isfile(resolved):
            raise CacheMiss(f"cannot resolve executable {executable}")
        resolved = os.path.realpath(resolved)
        # Record the start, not the end: identification itself can span ticks.
        checked_at = time.time_ns()
        try:
            info = os.stat(resolved)
        except OSError as error:
            raise CacheMiss(f"cannot stat {resolved}: {error}") from error
        if not stat.S_ISREG(info.st_mode):
            raise CacheMiss(f"executable is not a regular file: {resolved}")
        fingerprint = (resolved, stat.S_IFMT(info.st_mode), info.st_dev, info.st_ino,
                       info.st_size, info.st_mtime_ns)
        with self._lock:
            known = self._cache.get(fingerprint)
        if known is not None and info.st_mtime_ns + 2_000_000_000 <= known[1]:
            return known[0]
        try:
            content = sha256_file(resolved)
            completed = subprocess.run(
                [resolved, "--version"],
                stdin=subprocess.DEVNULL,
                capture_output=True,
                timeout=60,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise CacheMiss(f"cannot identify {executable}: {error}") from error
        if completed.returncode != 0 or not completed.stdout:
            raise CacheMiss(f"{executable} --version failed")
        text = completed.stdout.decode("utf-8", "replace")
        value = f"{resolved}\n{content}\n{text}"
        with self._lock:
            self._cache[fingerprint] = (value, checked_at)
        return value


class KeyBuilder:
    def __init__(self, tool_hash: str, clang_tidy: str, tools: ToolIdentity):
        self._tool_hash = tool_hash
        self._clang_tidy = clang_tidy
        self._tools = tools

    @staticmethod
    def _hash_dependency(path: str) -> str:
        try:
            with open(path, "rb") as handle:
                data = handle.read()
        except OSError as error:
            raise CacheMiss(f"cannot read dependency {path}: {error}") from error
        if any(token in data for token in _VOLATILE_TOKENS):
            raise CacheMiss(f"{path} uses a clock-dependent macro")
        return sha256_bytes(data)

    def key_for(self, tu: str, entries: list[dict], config: str) -> str:
        if not entries:
            raise CacheMiss("no compile database entry")
        parts = [
            FORMAT_VERSION,
            self._tool_hash,
            f"config:{config}",
            f"env:{environment_fingerprint()}",
            f"clang-tidy:{self._tools.describe(self._clang_tidy, os.getcwd())}",
            f"tu:{tu}",
        ]
        entry_parts = []
        for entry in entries:
            directory = entry.get("directory")
            if not isinstance(directory, str) or not os.path.isdir(directory):
                raise CacheMiss("compile database directory is missing")
            arguments = entry_arguments(entry)
            compiler_version = self._tools.describe(arguments[0], directory)
            command = dependency_command(arguments)
            try:
                completed = subprocess.run(
                    command,
                    cwd=directory,
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    timeout=DEPS_TIMEOUT_SECONDS,
                    check=False,
                )
            except (OSError, subprocess.SubprocessError) as error:
                raise CacheMiss(f"dependency scan failed to run: {error}") from error
            if completed.returncode != 0:
                raise CacheMiss("dependency scan exited non-zero")
            deps = parse_make_deps(completed.stdout.decode("utf-8", "replace"))
            # Paths are opened exactly as the compiler printed them (no lexical
            # normalization: "link/../x.h" must follow the symlink like the
            # compiler did).
            absolute = [dep if os.path.isabs(dep) else os.path.join(directory, dep) for dep in deps]
            if os.path.realpath(tu) not in {os.path.realpath(path) for path in absolute}:
                raise CacheMiss("translation unit missing from its own dependency list")
            dep_lines = sorted({f"{path}:{self._hash_dependency(path)}" for path in absolute})
            entry_parts.append(
                json.dumps(
                    {
                        "directory": directory,
                        "arguments": arguments,
                        "compiler": compiler_version,
                        "deps": dep_lines,
                    },
                    sort_keys=True,
                )
            )
        parts.extend(sorted(entry_parts))
        return sha256_bytes("\n".join(parts).encode("utf-8"))


def load_entries(build_dir: str) -> dict[str, list[dict]]:
    with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as handle:
        database = json.load(handle)
    by_file: dict[str, list[dict]] = {}
    for entry in database:
        directory = entry.get("directory", "")
        path = entry["file"]
        if not os.path.isabs(path):
            path = os.path.join(directory, path)
        by_file.setdefault(os.path.normpath(path), []).append(entry)
    return by_file


def record_path(cache_dir: str, key: str) -> str:
    return os.path.join(cache_dir, key[:2], key + ".json")


def lookup(cache_dir: str, key: str) -> bool:
    path = record_path(cache_dir, key)
    try:
        with open(path, encoding="utf-8") as handle:
            record = json.load(handle)
    except (OSError, ValueError):
        return False
    if not isinstance(record, dict):
        return False
    if record.get("format") != FORMAT_VERSION or record.get("key") != key:
        return False
    exit_code = record.get("tidy_exit")
    if type(exit_code) is not int or exit_code != 0:
        return False
    if not isinstance(record.get("tu"), str):
        return False
    try:
        os.utime(path)  # mark as used so prune keeps it
    except OSError:
        pass
    return True


def store(cache_dir: str, key: str, tu: str) -> None:
    path = record_path(cache_dir, key)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    record = {"format": FORMAT_VERSION, "key": key, "tidy_exit": 0, "tu": tu}
    handle, temporary = tempfile.mkstemp(dir=os.path.dirname(path), suffix=".tmp")
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            json.dump(record, stream)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def combined_tool_hash(tool_files: list[str]) -> str:
    digest = hashlib.sha256()
    for path in [os.path.abspath(__file__), *tool_files]:
        digest.update(os.path.basename(path).encode("utf-8") + b"\0")
        digest.update(sha256_file(path).encode("ascii") + b"\0")
    return digest.hexdigest()


def run_command(args: argparse.Namespace) -> int:
    files = [os.path.normpath(os.path.abspath(path)) for path in args.files]
    tool_hash = combined_tool_hash(args.tool_file)
    tools = ToolIdentity()
    builder = KeyBuilder(tool_hash, args.clang_tidy, tools)
    try:
        entries_by_file = load_entries(args.build_dir)
    except (OSError, ValueError, KeyError) as error:
        print(f"tidy cache: cannot read the compile database ({error}); running uncached", file=sys.stderr)
        entries_by_file = {}
    read_enabled = args.mode == "readwrite"
    os.makedirs(args.cache_dir, exist_ok=True)

    def prepare(tu: str) -> tuple[str, str | None, bool]:
        try:
            key = builder.key_for(tu, entries_by_file.get(tu, []), args.config)
        except (CacheMiss, OSError, ValueError) as error:
            print(f"tidy cache: miss (uncached) {tu}: {error}", file=sys.stderr)
            return tu, None, False
        return tu, key, read_enabled and lookup(args.cache_dir, key)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        prepared = list(pool.map(prepare, files))

    hits = [item for item in prepared if item[2]]
    misses = [item for item in prepared if not item[2]]
    print(f"tidy cache: {len(hits)} hit(s), {len(misses)} to analyze ({args.mode}, {len(files)} unit(s))")

    failures: list[str] = []
    recorded = 0
    output_lock = threading.Lock()

    def analyze(item: tuple[str, str | None, bool]) -> None:
        nonlocal recorded
        tu, key, _ = item
        command = [args.clang_tidy, "-p", args.build_dir, "-quiet", f"--config={args.config}", tu]
        try:
            completed = subprocess.run(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=TIDY_TIMEOUT_SECONDS,
                check=False,
            )
            status = completed.returncode
            output = completed.stdout.decode("utf-8", "replace")
        except (OSError, subprocess.SubprocessError) as error:
            status = -1
            output = f"clang-tidy did not complete: {error}\n"
        with output_lock:
            if output.strip():
                sys.stdout.write(output)
                sys.stdout.flush()
            if status != 0:
                failures.append(tu)
                return
        if key is None:
            return
        # The inputs must be unchanged after the analysis, or the record could
        # describe a different input than the one that was analyzed.
        verifier = KeyBuilder(tool_hash, args.clang_tidy, tools)
        try:
            fresh_entries = load_entries(args.build_dir)
            after = verifier.key_for(tu, fresh_entries.get(tu, []), args.config)
        except (CacheMiss, OSError, ValueError, KeyError):
            return
        if after != key:
            return
        try:
            store(args.cache_dir, key, tu)
        except OSError as error:
            print(f"tidy cache: could not write record for {tu}: {error}", file=sys.stderr)
            return
        with output_lock:
            recorded += 1

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        list(pool.map(analyze, misses))

    print(f"tidy cache: analyzed {len(misses)}, recorded {recorded}, failed {len(failures)}")
    for tu in failures:
        print(f"tidy cache: clang-tidy failed on {tu}", file=sys.stderr)
    return 1 if failures else 0


def prune_command(args: argparse.Namespace) -> int:
    removed = 0
    kept = 0
    for root, _, names in os.walk(args.cache_dir):
        for name in names:
            path = os.path.join(root, name)
            try:
                if not name.endswith(".json") or os.path.getmtime(path) < args.before:
                    os.unlink(path)
                    removed += 1
                else:
                    kept += 1
            except OSError:
                pass
    print(f"tidy cache: pruned {removed} unused record(s), kept {kept}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    shard = sub.add_parser("shard", help="select a stable path-hash shard; emit NUL-separated paths")
    shard.add_argument("--repo-root", required=True)
    shard.add_argument("--shard", type=parse_shard, default=(1, 1))
    shard.add_argument("files", nargs="*")
    shard.set_defaults(func=shard_command)

    run = sub.add_parser("run", help="analyze translation units, skipping proven-identical ones")
    run.add_argument("--build-dir", required=True)
    run.add_argument("--cache-dir", required=True)
    run.add_argument("--mode", choices=("readwrite", "writeonly"), default="readwrite")
    run.add_argument("--jobs", type=int, default=1)
    run.add_argument("--clang-tidy", required=True)
    run.add_argument("--config", required=True, help="exact JSON config passed to clang-tidy")
    run.add_argument("--tool-file", action="append", default=[], help="file whose content is part of every key")
    run.add_argument("files", nargs="+")
    run.set_defaults(func=run_command)

    prune = sub.add_parser("prune", help="delete records not used since --before")
    prune.add_argument("--cache-dir", required=True)
    prune.add_argument("--before", type=float, default=time.time())
    prune.set_defaults(func=prune_command)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
