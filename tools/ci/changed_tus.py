#!/usr/bin/env python3
"""Select changed translation units, then compile or lint them without linking.

Selection is deliberately uncached: every header scan uses every command
variant, in ordinary and clang analyzer preprocessing contexts. No Git fetches.
"""

from __future__ import annotations

import argparse
import fnmatch
import functools
import hashlib
import importlib.util
import json
import ntpath
import os
from os import walk as walk_header_directories
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location("_changed_tus_deps", ROOT / "scripts/tidy_cache.py")
_DEPS = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_DEPS)
_PATH_SPEC = importlib.util.spec_from_file_location("_changed_tus_paths", ROOT / "tools/ci/ci_paths.py")
_PATHS = importlib.util.module_from_spec(_PATH_SPEC)
_PATH_SPEC.loader.exec_module(_PATHS)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".inc"}
INPUT_VALUE_OPTIONS = {"-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros",
                       "-isysroot", "--sysroot", "-D", "-U", "-x", "-target", "--target"}
# Arity and replay policy are shared by cleaning and source identification.
# Recognize unsafe pass-through/PCH and MSVC-style value options explicitly.
# Bounded PCH normalization happens before parsing; other forms stay refused.
VALUE_OPTIONS = {
    **{option: "keep" for option in INPUT_VALUE_OPTIONS},
    **{option: "drop" for option in ("-o", "-MF", "-MT", "-MQ", "-MJ")},
    **{option: "refuse" for option in (
        "-include-pch", "-include-pth", "-Xclang", "-Xpreprocessor", "-Xassembler",
        "-Xlinker", "-B", "-iprefix", "-iwithprefix", "-iwithprefixbefore",
        "-iframework", "-F", "-resource-dir", "-gcc-toolchain", "--gcc-toolchain",
        "-serialize-diagnostics", "--serialize-diagnostics",
        "/Fo", "/Fi", "/FI", "/Fd", "/Fp", "/Fa", "/Fe", "/I", "/D", "/U",
        "/Tc", "/Tp", "/external:I", "/imsvc", "/clang",
    )},
}

# Ordered MSVC /W4 approximation, deliberately separate from target policy.
# Repeated flags are deduplicated in this order. These are broader than MSVC;
# calibration records exact diagnostics rather than disabling a whole category.
MSVC_APPROX = (
    {"warning": "C4456", "gcc": ("-Wshadow",), "clang": ("-Wshadow-all",),
     "rationale": "A local declaration hides another local declaration."},
    {"warning": "C4457", "gcc": ("-Wshadow",), "clang": ("-Wshadow-all",),
     "rationale": "A local declaration hides a function parameter."},
    {"warning": "C4458", "gcc": ("-Wshadow",), "clang": ("-Wshadow-all",),
     "rationale": "A declaration hides a class member, including inherited members."},
    {"warning": "C4459", "gcc": ("-Wshadow",), "clang": ("-Wshadow-all",),
     "rationale": "A declaration hides a global declaration."},
    {"warning": "C4244", "gcc": ("-Wconversion", "-Wnarrowing"),
     "clang": ("-Wconversion", "-Wnarrowing"),
     "rationale": "Conversions can lose value or precision; narrowing covers C++ list initialization."},
    {"warning": "C4267", "gcc": ("-Wconversion", "-Wsign-conversion"),
     "clang": ("-Wconversion", "-Wsign-conversion"),
     "rationale": "size_t conversions can lose width or change signedness."},
)


class SelectionError(ValueError):
    pass


def reject_reparse_points(path):
    try:
        _DEPS.reject_reparse_points(path)
    except OSError as error:
        raise SelectionError(str(error)) from error


def checked_resolve(path):
    reject_reparse_points(path)
    return path.resolve()


# Compiler installations/search lists are stable during one invocation. PCH
# scan results also record directory identities and timestamps: adding a cache
# after database validation must still invalidate a successful scan.
_HEADER_SEARCH_LOCK = threading.RLock()
_PCH_SEARCH_CACHE: dict[tuple, tuple[dict, Path | None, int]] = {}


def first_party(relative: Path) -> bool:
    return (relative.parts[0] in {"src", "include", "test", "tools"}
            and relative.parts[:4] != ("src", "luminumbra_client", "ui", "gl3"))


def changed_paths(root: Path, base_ref: str, worktree=True, include_untracked=True) -> set[Path]:
    def git(*args):
        return subprocess.check_output(["git", *args], cwd=root,
                                       env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"})

    base = git("merge-base", base_ref, "HEAD").decode("ascii").strip()
    if not re.fullmatch(r"[0-9a-f]{40,64}", base):
        raise SelectionError("no local merge base; provision local history and base ref")
    commands = [("diff", "--name-only", "--no-renames", "-z", base, "HEAD", "--")]
    if worktree:
        commands += [("diff", "--name-only", "--no-renames", "-z", "HEAD", "--"),
                     ("diff", "--name-only", "--no-renames", "-z", "--cached", "HEAD", "--")]
    if include_untracked:
        commands.append(("ls-files", "--others", "--exclude-standard", "-z"))
    paths = set()
    for command in commands:
        for raw in git(*command).split(b"\0"):
            if raw:
                path = root / os.fsdecode(raw)
                if not checked_resolve(path).is_relative_to(root):
                    raise SelectionError(f"changed path escapes checkout: {os.fsdecode(raw)}")
                paths.add(path)
    return paths


def read_database(root: Path, build_dir: Path) -> list[dict]:
    data = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise SelectionError("compile database must be an array")
    entries = []
    for raw in data:
        if not isinstance(raw, dict) or not isinstance(raw.get("file"), str):
            raise SelectionError("malformed compile database entry")
        directory = Path(raw.get("directory", ""))
        if not directory.is_absolute() or not directory.is_dir():
            raise SelectionError("compile database needs existing absolute working directories")
        source = checked_resolve(directory / raw["file"])
        if not source.is_relative_to(root) or not first_party(source.relative_to(root)):
            continue
        if source.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if not checked_resolve(directory).is_relative_to(root):
            raise SelectionError(f"working directory escapes checkout: {source}")
        try:
            if "arguments" in raw and (not isinstance(raw["arguments"], list)
                    or not all(isinstance(a, str) for a in raw["arguments"])):
                raise SelectionError("malformed compiler arguments")
            args = normalise_pch_arguments(_DEPS.entry_arguments(raw), directory, build_dir)
            # Reject unsupported arguments and wrong source operands before probing.
            cleaned, operands = parse_arguments(args)
            if len(operands) != 1 or checked_resolve(directory / operands[0]) != source:
                raise SelectionError(f"command must compile exactly one database source: {source}")
            validate_textual_includes(cleaned, directory)
        except (_DEPS.CacheMiss, ValueError, TypeError) as error:
            raise SelectionError(str(error)) from error
        entries.append({"file": source, "directory": directory, "arguments": args})
    return entries


def normalise_pch_arguments(arguments: list[str], directory: Path, build_dir: Path) -> list[str]:
    """Replay CMake's textual includes without requiring prebuilt Clang PCHs.

    Only the paired cc1 include forms are recognized, never arbitrary -Xclang.
    A binary PCH may be omitted only with its matching textual include present.
    Dependency PCH generation entries are outside the first-party source scope.
    """
    result, headers, binaries = [], set(), []
    index = 0
    while index < len(arguments):
        arg = arguments[index]
        if arg == "-Xclang":
            if (index + 3 >= len(arguments)
                    or arguments[index + 1] not in {"-include", "-include-pch"}
                    or arguments[index + 2] != "-Xclang"):
                raise SelectionError("unsupported compiler option: -Xclang")
            option, value = arguments[index + 1], arguments[index + 3]
            index += 4
        elif arg in {"-include", "-include-pch"}:
            if index + 1 >= len(arguments):
                raise SelectionError(f"missing value after {arg}")
            option, value = arg, arguments[index + 1]
            index += 2
            if arg == "-include" and Path(value).name not in {"cmake_pch.h", "cmake_pch.hxx"}:
                # Ordinary forced includes retain the existing input policy.
                # They can also provide the textual counterpart of a PCH.
                path = checked_resolve(directory / value)
                if path.is_relative_to(checked_resolve(build_dir)) and path.is_file():
                    headers.add(path)
                result.extend([arg, value])
                continue
        else:
            result.append(arg)
            index += 1
            # Do not reinterpret option values as pass-through options.
            if arg in VALUE_OPTIONS and index < len(arguments):
                result.append(arguments[index])
                index += 1
            continue
        if not value or value.startswith(("-", "@")):
            raise SelectionError(f"missing value after {option}")
        path = checked_resolve(directory / value)
        if not path.is_relative_to(checked_resolve(build_dir)):
            raise SelectionError("PCH reference escapes build tree")
        if option == "-include":
            if not path.is_file():
                raise SelectionError("missing textual PCH header")
            headers.add(path)
            result.extend(["-include", str(path)])
        else:
            binaries.append(path)
    for path in binaries:
        if path.suffix != ".pch" or path.with_suffix("") not in headers:
            raise SelectionError("binary PCH requires matching textual include")
    return result


def clean_arguments(arguments: list[str]) -> list[str]:
    return parse_arguments(arguments)[0]


def compiler_header_roots(arguments: list[str], directory: Path) -> tuple[Path, ...]:
    """Query effective GCC/Clang roots with the variant's target and sysroot.

    Preprocess empty stdin, never a header or TU, so the probe cannot consume
    a PCH. Fail closed if the compiler is unavailable or its list is unknown.
    Memoize equivalent probes for this process; macros and TU paths do not
    affect the driver's search list. Directory contents are checked separately.
    """
    command, operands = parse_arguments(arguments)
    language = "c++" if "++" in executable_name(command[0]) or Path(operands[0]).suffix != ".c" else "c"
    probe = command[:1]
    source_seen = False
    index = 1
    while index < len(command):
        option = command[index]
        if option == "-x":
            # -x affects only later operands; flags after the TU cannot
            # change its language (other search flags remain global).
            if not source_seen:
                language = command[index + 1]
        elif option in VALUE_OPTIONS:
            if option not in {"-include", "-imacros", "-D", "-U"}:
                probe.extend(command[index:index + 2])
        elif option.startswith("-"):
            if not option.startswith(("-D", "-U")):
                probe.append(option)
        else:
            source_seen = True
        index += 2 if option in VALUE_OPTIONS else 1
    probe.extend(["-E", "-v", "-x", language, "-"])
    environment = tuple(sorted({**os.environ, "LC_ALL": "C", "LANG": "C"}.items()))
    # Serialize misses too, so parallel TU variants cannot duplicate a probe.
    with _HEADER_SEARCH_LOCK:
        return _compiler_header_roots(tuple(probe), checked_resolve(directory), environment)


@functools.lru_cache(maxsize=None)
def _compiler_header_roots(probe: tuple[str, ...], directory: Path,
                           environment: tuple[tuple[str, str], ...]) -> tuple[Path, ...]:
    try:
        result = _DEPS.run_process(list(probe), cwd=directory, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, timeout=1800,
                                env=dict(environment))
    except (OSError, subprocess.SubprocessError) as error:
        raise SelectionError(f"cannot query compiler header search paths: {error}") from error
    roots, scanning, complete = [], False, False
    for line in result.stderr.splitlines():
        line = line.strip()
        if line in {'#include "..." search starts here:', '#include <...> search starts here:'}:
            scanning = True
        elif scanning and line == "End of search list.":
            complete = True
            break
        elif scanning:
            roots.append(directory / line.removesuffix(" (framework directory)"))
    if result.returncode or not complete:
        raise SelectionError("cannot establish compiler header search paths; refusing replay")
    # Forced includes follow the driver's quote/system search order.
    return tuple(dict.fromkeys(roots))


def directory_stamp(path: Path) -> tuple | None:
    """Use stat, not DirEntry's incomplete Windows identity fields."""
    reject_reparse_points(path)
    try:
        info = path.stat()
    except FileNotFoundError:
        return None
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def directory_entries(path: Path, names=None) -> tuple:
    """Record entry kinds and identities without following symlink targets.

    Use lstat because Windows DirEntry.stat omits device and inode identity.
    Target changes, including dangling links, have separate directory stamps.
    """
    reject_reparse_points(path)
    entries = []
    for name in sorted(os.listdir(path) if names is None else names):
        reject_reparse_points(path / name)
        info = os.lstat(path / name)
        entries.append((name, stat.S_IFMT(info.st_mode), info.st_dev, info.st_ino))
    return tuple(entries)


def check_pch_search_roots(compiler: str, search: set[Path]) -> None:
    """Reuse directory walks until a directory's identity or contents change.

    Retain both successes and refusals per compiler/root set for this process.
    Stat the visited directories on reuse, including links and missing roots;
    newly added nested .gch entries or retargeted links require another walk.
    Track dangling links too, so creating their targets invalidates reuse.
    """
    def fail(error):
        raise SelectionError(f"cannot check binary PCH search directory: {error}") from error

    def unchanged(stamps, checked_at):
        for path, (stamp, entries) in stamps.items():
            if directory_stamp(path) != stamp:
                return False
            # Directory timestamps can have DOS's two-second granularity.
            # A snapshot captured within that tick remains suspect even when
            # replay happens later. Check membership until an observation
            # after the tick closes confirms the recorded entry identities.
            if (entries is not None and stamp is not None
                    and stamp[4] + 2_000_000_000 > checked_at
                    and directory_entries(path) != entries):
                return False
        return True

    with _HEADER_SEARCH_LOCK:
        try:
            # Resolve before deduplication and keying: Windows handles expand
            # short names, and Path equality folds native case/separators.
            roots = tuple(sorted({checked_resolve(path) for path in search}))
            # Compiler spellings are case/separator insensitive on Windows.
            # Keep the launch argument intact; only the scan identity changes.
            compiler_key = ntpath.normcase(ntpath.normpath(compiler)) if os.name == "nt" else compiler
            key = (compiler_key, roots)
            # Sample before walking or checking membership, so crossing a
            # timestamp tick during either operation cannot confer trust.
            checked_at = time.time_ns()
            cached = _PCH_SEARCH_CACHE.get(key)
            if cached is not None and unchanged(cached[0], cached[2]):
                stamps, binary = cached[:2]
            else:
                stamps, seen, binary = {}, set(), None
                for root in roots:
                    stamps[root] = (directory_stamp(root), None)
                    if stamps[root][0] is None:
                        continue  # Compilers ignore nonexistent include directories.
                    for current, children, files in walk_header_directories(root, followlinks=True, onerror=fail):
                        path = Path(current)
                        # Capture each directory before os.walk enumerates it,
                        # so a concurrent insertion cannot acquire a cached
                        # timestamp newer than the membership we checked.
                        entries = directory_entries(path, children + files)
                        stamps[path] = (stamps[path][0], entries)
                        directories = set(children)
                        for name, kind, _, _ in entries:
                            child = path / name
                            if name in directories or kind == stat.S_IFLNK:
                                # Track targets even for links classified as files;
                                # dangling targets may become traversable later.
                                child_stamp = directory_stamp(child)
                                stamps[child] = (child_stamp, None)
                                if kind == stat.S_IFLNK:
                                    kind = stat.S_IFMT(child_stamp[2]) if child_stamp else None
                            if (kind == stat.S_IFDIR) != (name in directories):
                                raise SelectionError(
                                    f"header search entry changed type during scan: {child}")
                        resolved = checked_resolve(path)
                        if resolved in seen:
                            children[:] = []
                            continue
                        seen.add(resolved)
                        children.sort()
                        for name in sorted(children + files):
                            if name.lower().endswith(".gch"):
                                binary = path / name
                                break
                        if binary is not None:
                            break
                    if binary is not None:
                        break
            _PCH_SEARCH_CACHE[key] = (stamps, binary, checked_at)
        except OSError as error:
            fail(error)
        if binary is not None:
            raise SelectionError(f"binary PCH cache in header search path: {binary}; "
                                 "remove the cache before replaying textual headers")


def validate_textual_includes(arguments: list[str], directory: Path, *, compiler_roots=False) -> None:
    """Refuse implicit binary PCH lookup for forced and ordinary includes.

    Check both the spelled and resolved names: a header symlink can have a
    cache beside either name. Directories and dangling cache links also count.
    Locate forced headers in the working directory, then in the driver's
    ordered quote/system search paths. Conservatively scan all explicit
    search roots, environment roots and source/forced-header directories,
    including subdirectories and directory links. Before replay also query
    compiler defaults/system roots; never guess platform-specific locations.
    GCC may silently use a stale .gch without diagnostics;
    rejecting the cache is safer than relying on invalid-PCH warnings. Call
    again at replay in case a build populated it after database validation.
    """
    search, forced = set(), []
    for variable in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH"):
        if variable in os.environ:
            # An empty component means the compiler's working directory.
            search.update(directory / value for value in os.environ[variable].split(os.pathsep))
    index = 1
    while index < len(arguments):
        option = arguments[index]
        if option in {"-include", "-imacros"}:
            forced.append(arguments[index + 1])
        elif option in {"-I", "-iquote", "-isystem", "-idirafter"}:
            search.add(directory / arguments[index + 1])
        elif option.startswith("-I"):
            search.add(directory / option[2:])
        elif option not in VALUE_OPTIONS and not option.startswith("-"):
            source = directory / option
            search.update((source.parent, checked_resolve(source).parent))
        index += 2 if option in VALUE_OPTIONS else 1

    roots = None
    for value in forced:
        candidates = [directory / value]
        index = 0
        while index < len(candidates):
            header = candidates[index]
            for path in (header, checked_resolve(header)):
                for suffix in (".gch", ".pch"):
                    binary = Path(str(path) + suffix)
                    if os.path.lexists(binary):
                        raise SelectionError(f"binary PCH cache beside forced include: {binary}; "
                                             "remove the cache before replaying textual headers")
            if header.is_file():
                search.update((header.parent, checked_resolve(header).parent))
                break
            if index == 0 and not Path(value).is_absolute():
                if roots is None:
                    roots = compiler_header_roots(arguments, directory)
                candidates.extend(root / value for root in roots)
            index += 1
        else:
            raise SelectionError(f"cannot locate textual forced include: {value}")

    if compiler_roots and roots is None:
        roots = compiler_header_roots(arguments, directory)
    search.update(roots or ())
    check_pch_search_roots(arguments[0], search)


def replay_arguments(entry: dict) -> list[str]:
    arguments = clean_arguments(entry["arguments"])
    validate_textual_includes(arguments, entry["directory"], compiler_roots=True)
    return arguments


def executable_name(executable: str) -> str:
    """Identify native and Windows drivers without rewriting their launch path."""
    name = ntpath.basename(executable)
    # Also recognize Windows command spellings when inspecting a database on
    # POSIX. Keep case-sensitive POSIX driver names otherwise.
    if os.name == "nt" or name.lower().endswith(".exe"):
        name = name.casefold()
    return name.removesuffix(".exe")


def parse_arguments(arguments: list[str]) -> tuple[list[str], list[str]]:
    """Allow only supported input/configuration flags; discard rewritten outputs.

    Unknown options fail before any process is started. In particular, broad
    -f*, -m* and driver pass-through prefixes are deliberately not accepted.
    """
    if not arguments or not all(isinstance(a, str) for a in arguments):
        raise SelectionError("malformed compiler arguments")
    args = list(arguments)
    if executable_name(args[0]) in {"ccache", "sccache"}:
        args.pop(0)
    if not args or not re.fullmatch(r"(?:[\w.+-]*-)?(?:gcc|g\+\+|cc|c\+\+|clang|clang\+\+)(?:-[0-9.]+)?", executable_name(args[0])):
        raise SelectionError("unsupported compiler or command wrapper")
    result, operands = [args[0]], []
    rewritten_values = {option for option, policy in VALUE_OPTIONS.items() if policy == "drop"}
    rewritten_alone = {"-c", "-MD", "-MMD", "-MP"}
    safe_exact = {
        "-pipe", "-pthread", "-ansi", "-pedantic", "-pedantic-errors", "-w", "-W",
        "-fPIC", "-fpic", "-fPIE", "-fpie", "-fno-omit-frame-pointer",
        "-fomit-frame-pointer", "-fno-exceptions", "-fexceptions", "-fno-rtti", "-frtti",
        "-fno-strict-aliasing", "-fstrict-aliasing", "-fno-math-errno", "-ffast-math",
        "-fno-fast-math", "-fno-common", "-fcommon", "-fwrapv", "-fno-wrapv",
        "-fvisibility-inlines-hidden", "-fno-stack-protector", "-fno-fat-lto-objects",
        "-flto", "-fno-lto", "-fpch-instantiate-templates", "-m32", "-m64", "-Wa,-mbig-obj",
    }
    # Explicit x86 features, including their disable forms; no driver/path options.
    x86_features = {"avx", "avx2", "avx512dq", "avx512f", "bmi", "f16c", "fma",
                    "lzcnt", "popcnt", "sse", "sse2", "sse3", "ssse3", "sse4.1", "sse4.2"}
    safe_exact.update(f"-m{prefix}{feature}" for prefix in ("", "no-") for feature in x86_features)
    safe_pattern = re.compile(
        r"(?:-O(?:[0-3gsz]|fast)?|-g(?:[0-3]|gdb[0-3]?|dwarf-[2-5])?"
        r"|-std=(?:gnu|c)(?:\+\+)?[a-z0-9]+|-W[a-zA-Z][a-zA-Z0-9-]*(?:=[a-zA-Z0-9,+-]+)?"
        r"|-ffp-contract=(?:off|on|fast)|-fvisibility=(?:default|hidden|protected)"
        r"|-ffp-model=(?:precise)|-flto=(?:auto|thin)"
        r"|-fsanitize=(?:address|undefined|thread)(?:,(?:address|undefined|thread))*"
        r"|-mfpmath=(?:sse|387)|-m(?:arch|tune)=[a-zA-Z0-9_+-]+)"
    )
    index = 1
    while index < len(args):
        arg = args[index]
        if arg in VALUE_OPTIONS:
            policy = VALUE_OPTIONS[arg]
            if policy == "refuse":
                raise SelectionError(f"unsupported compiler option: {arg}")
            if index + 1 >= len(args) or not args[index + 1] or args[index + 1].startswith(("-", "@")):
                raise SelectionError(f"missing value after {arg}")
            if policy == "keep":
                if arg == "-x" and args[index + 1] not in {"c", "c++"}:
                    raise SelectionError(f"unsupported compiler language: {args[index + 1]}")
                result.extend(args[index:index + 2])
            index += 2
            continue
        if arg in rewritten_alone or any(arg.startswith(p) and len(arg) > len(p)
                                        for p in rewritten_values):
            index += 1
            continue
        if not (arg in safe_exact or safe_pattern.fullmatch(arg)
                or (arg.startswith(("-I", "-D", "-U")) and len(arg) > 2)
                or arg.startswith(("--sysroot=", "--target="))
                or (not arg.startswith(("-", "@")) and Path(arg).suffix.lower() in SOURCE_SUFFIXES)):
            raise SelectionError(f"unsupported compiler option: {arg}")
        result.append(arg)
        if not arg.startswith("-"):
            operands.append(arg)
        index += 1
    return result, operands


def execute(command, directory: Path, check=True):
    completed = _DEPS.run_process(command, cwd=directory, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, timeout=1800)
    if check and completed.returncode:
        raise SelectionError(f"command failed ({completed.returncode}): {shlex.join(map(str, command))}\n"
                             + completed.stdout + completed.stderr)
    return completed


def dependencies(entry: dict) -> set[Path]:
    command = replay_arguments(entry)
    found = set()
    for analyzer in (False, True):
        args = command[:1] + (["-D__clang_analyzer__"] if analyzer else []) + command[1:]
        output = execute([*args, "-M", "-MT", "selected-tu"], entry["directory"]).stdout
        try:
            paths = _DEPS.parse_make_deps(output)
        except _DEPS.CacheMiss as error:
            raise SelectionError(str(error)) from error
        resolved = {checked_resolve(entry["directory"] / p) for p in paths}
        if entry["file"] not in resolved or any(not p.is_file() for p in resolved):
            raise SelectionError(f"incomplete dependency scan: {entry['file']}")
        found.update(resolved)
    return found


def parallel(function, items, jobs: int):
    try:
        return _DEPS.parallel_map(function, items, jobs, collect_errors=(
            ValueError, OSError, subprocess.SubprocessError))
    except ValueError as error:
        raise SelectionError(str(error)) from error


def requires_full_selection(relative: Path) -> bool:
    if relative.suffix.lower() in SOURCE_SUFFIXES | HEADER_SUFFIXES:
        return False
    # Only known non-native edits can avoid selection. Unknown inputs invalidate
    # the graph, including flags, dependency pins, scripts and extensionless files.
    return _PATHS.needs_cpp(relative.as_posix())


def configured_header_inputs(root: Path, build_dir: Path) -> tuple[dict[Path, set[Path]], bool]:
    """Recover literal configure_file / file(COPY_FILE|COPY) relationships.

    This is not a CMake interpreter. Only resolved scalar paths are evidence;
    computed paths, directory copies and unsupported syntax stay unknown and
    require conservative selection for header edits. A script containing any
    control flow or unsupported command cannot establish mappings: assignments
    may never run, run repeatedly, or mutate variables outside this reader.
    """
    mappings, incomplete = {}, False
    for directory, children, files in os.walk(root):
        reject_reparse_points(directory)
        for name in children + files:
            reject_reparse_points(Path(directory) / name)
        children[:] = sorted(c for c in children if c not in {
            ".git", "scratch", "vendor", "third_party", "deps", "node_modules"
        } and not (Path(directory) / c).is_symlink()
            and not checked_resolve(Path(directory) / c).is_relative_to(build_dir))
        for name in sorted(files):
            if name != "CMakeLists.txt" and not name.endswith(".cmake"):
                continue
            script = Path(directory) / name
            variables = {"CMAKE_SOURCE_DIR": str(root), "CMAKE_BINARY_DIR": str(build_dir)}
            if name == "CMakeLists.txt" and script.parent == root:
                variables.update(CMAKE_CURRENT_SOURCE_DIR=str(root),
                                 CMAKE_CURRENT_BINARY_DIR=str(build_dir))
            try:
                text = script.read_text(encoding="utf-8")
            except UnicodeError:
                # An unreadable script cannot establish configure provenance.
                incomplete = True
                continue
            # Preserve quoted '#' while dropping comments. Bracket quoting is
            # outside this reader's grammar and cannot establish provenance.
            if re.search(r"\[=*\[", text):
                incomplete = True
                continue
            text = re.sub(r'"(?:\\.|[^"\\])*"|#[^\n]*',
                          lambda m: "" if m[0].startswith("#") else m[0], text)
            # Consume the entire script, not just recognized calls. Unknown
            # commands (including user functions) can change subsequent inputs.
            # Do not trust even earlier mappings from such a script.
            calls, offset = [], 0
            while text[offset:].strip():
                call = re.match(r"\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*\(([^()]*)\)", text[offset:])
                if not call or call[1].lower() not in {"set", "configure_file", "file"}:
                    break
                tokens, position = [], 0
                body = call[2]
                while body[position:].strip():
                    token = re.match(r'''\s*("[^"\\]*"|[^\s"'\\()]+)(?=\s|$)''', body[position:])
                    if not token:
                        break
                    tokens.append(token[1][1:-1] if token[1].startswith('"') else token[1])
                    position += token.end()
                if body[position:].strip():
                    break
                calls.append((call[1].lower(), tokens))
                offset += call.end()
            if text[offset:].strip():
                incomplete = True
                continue
            script_mappings, script_incomplete = {}, False
            for kind, tokens in calls:
                def expand(token):
                    value = re.sub(r"\$\{([^}]+)\}", lambda m: variables.get(m[1], m[0]), token)
                    return None if any(c in value for c in "$;[]") else value
                if kind == "set":
                    if (len(tokens) == 2 and re.fullmatch(r"[a-zA-Z_][a-zA-Z0-9_]*", tokens[0])
                            and expand(tokens[1]) is not None
                            and expand(tokens[1]) not in {"PARENT_SCOPE", "CACHE", "FORCE"}):
                        variables[tokens[0]] = expand(tokens[1])
                    else:
                        script_incomplete = True
                    continue
                tokens = [expand(t) for t in tokens]
                relevant = kind == "configure_file" or (tokens and tokens[0] in {"COPY", "COPY_FILE"})
                if not relevant:
                    script_incomplete = True
                    continue
                pair = None
                if (kind == "configure_file" and len(tokens) in {2, 3}
                        and (len(tokens) == 2 or tokens[2] in {"@ONLY", "COPYONLY"})):
                    pair = tokens[:2]
                elif kind == "file" and tokens:
                    if tokens[0] == "COPY_FILE" and len(tokens) == 3:
                        pair = tokens[1:3]
                    elif tokens[0] == "COPY" and len(tokens) == 4 and tokens[2] == "DESTINATION":
                        if tokens[1] and tokens[3]:
                            pair = [tokens[1], str(Path(tokens[3]) / Path(tokens[1]).name)]
                if not pair or not all(pair):
                    script_incomplete = True
                    continue
                source, output = map(Path, pair)
                # Relative paths are unambiguous only in the top-level list.
                if (not source.is_absolute() or not output.is_absolute()) and script != root / "CMakeLists.txt":
                    script_incomplete = True
                    continue
                source, output = checked_resolve(root / source), checked_resolve(build_dir / output)
                if (source.is_file() and source.is_relative_to(root)
                        and output != source and not output.is_dir()):
                    script_mappings.setdefault(output, set()).add(source)
                else:
                    script_incomplete = True
            if script_incomplete:
                incomplete = True
            else:
                for output, sources in script_mappings.items():
                    mappings.setdefault(output, set()).update(sources)
    # Include/call scope cannot be inferred across scripts either. Never expose
    # partial provenance when another script could have changed its meaning.
    return ({}, True) if incomplete else (mappings, False)


def select(root: Path, entries: list[dict], changed: set[Path], jobs=2,
           build_dir: Path | None = None) -> tuple[list[dict], dict]:
    by_file = {entry["file"] for entry in entries}
    sources, headers = set(), set()
    full = False
    for path in changed:
        relative = path.relative_to(root)
        suffix = path.suffix.lower()
        if suffix in SOURCE_SUFFIXES and first_party(relative):
            if path.exists():
                resolved = checked_resolve(path)
                sources.add(resolved)
            else:
                # A removed TU has nothing to compile, but can invalidate targets.
                full = True
        elif suffix in HEADER_SUFFIXES:
            headers.add(checked_resolve(path))
            full |= not path.exists()
        elif suffix in SOURCE_SUFFIXES or requires_full_selection(relative):
            full = True
    if (full or headers) and not entries:
        raise SelectionError("incomplete selection: no first-party compile database entries")
    scans = []
    if full or headers or sources:
        scans = parallel(dependencies, entries, jobs)
    matched_sources = set().union(*scans) if scans else set()
    missing_sources = sources - by_file - matched_sources
    if missing_sources:
        source = min(missing_sources)
        raise SelectionError("missing compile database entry (possibly disabled configuration): "
                             f"{source.relative_to(root)}")
    fallback = []
    if headers and not full:
        mappings, incomplete = configured_header_inputs(root, build_dir) if build_dir else ({}, True)
        expanded = []
        unknown_generated = set()
        for scan in scans:
            closure = set(scan)
            pending = list(scan)
            while pending:
                dependency = pending.pop()
                if dependency in mappings:
                    for source in mappings[dependency] - closure:
                        closure.add(source)
                        pending.append(source)
                elif (dependency.is_relative_to(root / "scratch")
                      or (build_dir and dependency.is_relative_to(build_dir))):
                    unknown_generated.add(dependency)
            expanded.append(closure)
        scans = expanded
        matched = set().union(*scans)
        if headers - matched:
            fallback.append("changed header has no proven dependency mapping")
        if unknown_generated:
            fallback.append("generated dependency has no proven input mapping")
        if incomplete:
            fallback.append("unresolved configure/copy relationships")
        full = bool(fallback)
    selected = []
    changed_inputs = sources | headers
    for index, entry in enumerate(entries):
        if full or (scans and scans[index] & changed_inputs):
            if not entry["file"].is_file():
                raise SelectionError(f"stale database entry for deleted TU: {entry['file']}")
            selected.append(entry)
    return selected, {"full_selection": full, "selection_fallback": fallback, "scan_variants": len(scans),
                      "cache": "disabled; fresh dependency scans"}


def warning_family(option: str) -> str | None:
    if "shadow" in option:
        return "shadow"
    if "conversion" in option or "narrowing" in option or "shorten-64-to-32" in option:
        return "conversion"
    return None


def profile_compiler(entry: dict) -> str:
    name = executable_name(clean_arguments(entry["arguments"])[0])
    if "clang" in name:
        return "clang"
    if "gcc" in name or "g++" in name:
        return "gcc"
    if "profile_compiler" not in entry:
        # cc/c++ may be either compiler (including non-symlink platform aliases).
        # Probe once per variant; no shell, compilation, or generated outputs.
        output = execute([clean_arguments(entry["arguments"])[0], "--version"], entry["directory"])
        version = output.stdout + output.stderr
        if re.search(r"\bclang\b", version, re.I):
            entry["profile_compiler"] = "clang"
        elif re.search(r"\b(?:gcc|Free Software Foundation)\b|g\+\+", version, re.I):
            entry["profile_compiler"] = "gcc"
        else:
            raise SelectionError("cannot identify GCC/Clang warning-profile compiler")
    return entry["profile_compiler"]


def profile_flags(entry: dict, profile: str) -> list[str]:
    if profile == "none":
        return []
    if profile != "msvc-approx":
        raise SelectionError(f"unknown warning profile: {profile}")
    command = clean_arguments(entry["arguments"])
    if "-w" in command or "-Wno-everything" in command:
        return []
    # Conservatively keep an entire explicitly configured family unchanged.
    # This preserves disables, demotions and narrower subgroup exceptions even
    # where GCC and Clang disagree about group specificity and option order.
    configured = {warning_family(arg) for arg in command if arg.startswith("-W")}
    compiler = profile_compiler(entry)
    cxx = entry["file"].suffix.lower() != ".c" or "++" in executable_name(command[0])
    index = 1
    while index < len(command):
        arg = command[index]
        if arg == "-x":
            cxx = command[index + 1] == "c++"
        if arg in INPUT_VALUE_OPTIONS:
            index += 2
            continue
        if not arg.startswith("-") and checked_resolve(entry["directory"] / arg) == entry["file"]:
            break
        index += 1
    flags = []
    for mapping in MSVC_APPROX:
        for flag in mapping[compiler]:
            if (flag not in flags and warning_family(flag) not in configured
                    and (flag != "-Wnarrowing" or cxx)):
                flags.append(flag)
    return flags


def check_arguments(entry: dict, profile="none") -> list[str]:
    command = replay_arguments(entry)
    if profile != "none":
        compiler = profile_compiler(entry)
        command += profile_flags(entry, profile) + [
            "-fdiagnostics-color=never", "-fdiagnostics-show-option", "-fmessage-length=0",
            "-fno-caret-diagnostics" if compiler == "clang" else "-fno-diagnostics-show-caret"]
    return command


def compile_command(entry: dict, output: Path, ccache=False, warning_profile="none") -> list[str]:
    """Replay each variant exactly, then append the optional profile."""
    command = check_arguments(entry, warning_profile)
    return (["ccache"] if ccache else []) + command + ["-c", "-o", str(output)]


def variant_key(entry: dict, profile: str) -> str:
    data = {"file": str(entry["file"]), "directory": str(entry["directory"]),
            "arguments": check_arguments(entry, profile)}
    return hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()


def clean_prefixes(root: Path, prefixes: list[Path]) -> tuple[Path, ...]:
    resolved = []
    for prefix in prefixes:
        path = checked_resolve(root / prefix)
        if prefix.is_absolute() or ".." in prefix.parts or not path.is_relative_to(root) or not path.is_dir():
            raise SelectionError("clean prefixes must be existing checkout-relative directories")
        resolved.append(path)
    return tuple(sorted(set(resolved)))


def read_warning_baseline(path: Path) -> list[dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    fields = {"variant", "file", "line", "column", "option", "message", "source_line"}
    if (not isinstance(data, dict) or set(data) != {"version", "diagnostics"}
            or type(data["version"]) is not int or data["version"] != 1
            or not isinstance(data["diagnostics"], list)):
        raise SelectionError("unsupported warning baseline")
    for record in data["diagnostics"]:
        if (not isinstance(record, dict) or set(record) != fields
                or any(not isinstance(record[f], str) for f in fields - {"line", "column"})
                or any(type(record[f]) is not int or record[f] < 1 for f in ("line", "column"))
                or not warning_family(record["option"])
                or not re.fullmatch(r"[0-9a-f]{64}", record["variant"])):
            raise SelectionError("malformed warning baseline diagnostic")
    return data["diagnostics"]


def profile_diagnostics(result, entry: dict, profile: str, root: Path,
                        prefixes: tuple[Path, ...]) -> tuple[list[dict], list[dict], bool]:
    """Recognize only added profile diagnostics; unknown errors fail closed.

    Diagnostics stay visible. A baseline changes only their blocking status.
    Source-line content prevents accepting an edited offending line at the same
    location. Variant identity includes the complete ordered replay and layer.
    """
    families = {warning_family(flag) for flag in profile_flags(entry, profile)}
    records, errors, unknown_error = [], [], False
    # An early-stop option can hide a later, new diagnostic behind a calibrated
    # error. Such a partial stream cannot establish a passing check.
    fatal_errors = False
    for flag in clean_arguments(entry["arguments"]):
        if flag in {"-Wfatal-errors", "-Wno-fatal-errors"}:
            fatal_errors = flag == "-Wfatal-errors"
    pattern = re.compile(r"^(.*?):([1-9][0-9]*):([1-9][0-9]*): (warning|error): "
                         r"(.*?) \[-W(?:error(?:=|,-W))?([a-zA-Z0-9_+.=-]+)\]$")
    for line in (result.stdout + "\n" + result.stderr).splitlines():
        match = pattern.fullmatch(line)
        if not match:
            unknown_error |= bool(re.search(r"\b(?:fatal )?error:", line))
            continue
        filename, lineno, column, severity, message, option = match.groups()
        path = checked_resolve(entry["directory"] / filename)
        family = warning_family(option)
        # Clean scopes cannot acquire conversion baseline exemptions, including
        # diagnostics in a baseline header included by a clean translation unit.
        clean = family == "conversion" and any(
            path.is_relative_to(p) or entry["file"].is_relative_to(p) for p in prefixes)
        if family not in families or clean or not path.is_relative_to(root) or not path.is_file():
            unknown_error |= severity == "error"
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        if int(lineno) > len(lines):
            unknown_error |= severity == "error"
            continue
        record = {"variant": variant_key(entry, profile), "file": path.relative_to(root).as_posix(),
                  "line": int(lineno), "column": int(column), "option": option,
                  "message": message, "source_line": lines[int(lineno) - 1]}
        records.append(record)
        if severity == "error":
            errors.append(record)
    return records, errors, unknown_error or (fatal_errors and bool(errors))


def gate_config(root: Path, source: Path) -> dict:
    """Read the literal curated configuration; never evaluate shell code."""
    text = (root / "scripts/tidy.sh").read_text(encoding="utf-8")
    checks = re.findall(r'^gate_checks(?:\+)?="([^"\n]*)"$', text, re.M)
    if not checks or checks[0] != "-*":
        raise SelectionError("cannot read curated tidy checks")
    tracked = re.search(r"^tracked_global_tus=\((.*?)^\)", text, re.M | re.S)
    exception = re.search(r'^tracked_global_gate_checks="\$gate_checks([^"\n]*)"$', text, re.M)
    if not tracked or not exception:
        raise SelectionError("cannot read curated tidy configuration")
    check_string = "".join(checks)
    names = re.findall(r'^\s*"([^"\n]+)"\s*$', tracked[1], re.M)
    is_tracked = source.relative_to(root).as_posix() in names
    if is_tracked:
        check_string += exception[1]
    variable = "tracked_global_gate_config" if is_tracked else "gate_config"
    match = re.search(r"^" + variable + r"='(.*?)'\n", text, re.M | re.S)
    if not match:
        raise SelectionError("cannot read curated tidy configuration")
    placeholder = "'\"$tracked_global_gate_checks\"'" if is_tracked else "'\"$gate_checks\"'"
    config = match[1].replace(placeholder, check_string)
    try:
        return json.loads(config)
    except ValueError as error:
        raise SelectionError("unsupported curated tidy configuration") from error


def ci_gate_file(root: Path, source: Path) -> bool:
    """Read the ordered literal shell case patterns used by the CI gate."""
    text = (root / "scripts/tidy.sh").read_text(encoding="utf-8")
    match = re.search(r'is_first_party_tu\(\) \{\s*case "\$1" in(.*?)esac\s*return 1\s*\}', text, re.S)
    if not match:
        raise SelectionError("cannot read curated tidy file filter")
    body = match[1].replace("\\\n", "")
    branches = re.findall(r"\s*([^()]+)\)\s*return ([01])\s*;;", body)
    remainder = re.sub(r"\s*([^()]+)\)\s*return ([01])\s*;;", "", body)
    if not branches or remainder.strip():
        raise SelectionError("unsupported curated tidy file filter")
    rules = []
    for patterns, status in branches:
        for pattern in patterns.split("|"):
            pattern = pattern.strip()
            if not re.fullmatch(r"[a-zA-Z0-9_./*+-]+", pattern):
                raise SelectionError("unsupported curated tidy file pattern")
            rules.append((pattern, status))
    relative = source.relative_to(root).as_posix()
    for pattern, status in rules:
        if fnmatch.fnmatchcase(relative, pattern):
            return status == "0"
    return False


def run_checks(args, root: Path, selected: list[dict], build_dir: Path) -> list[dict]:
    scratch = root / "scratch"
    if scratch.is_symlink() or not checked_resolve(scratch).is_relative_to(root):
        raise SelectionError("scratch escapes checkout")
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="local-checks-", dir=scratch) as temporary:
        work = Path(temporary)
        if args.command == "compile":
            if args.ccache and not shutil.which("ccache"):
                raise SelectionError("ccache not found")
            def compile_one(item):
                index, entry = item
                profile = args.warning_profile
                output = work / f"{index}.o"
                if args.write_warning_baseline or args.baseline_records:
                    # Calibration cannot excuse an error in the target's own
                    # policy. Verify that policy first, before adding warnings.
                    execute(compile_command(entry, output, args.ccache), entry["directory"])
                command = compile_command(entry, output, args.ccache, profile)
                if profile == "none":
                    result = execute(command, entry["directory"])
                    return result.stdout + result.stderr, []
                result = execute(command, entry["directory"], check=False)
                records, errors, unknown = profile_diagnostics(
                    result, entry, profile, root, args.clean_prefixes)
                allowed = records if args.write_warning_baseline else args.baseline_records
                if result.returncode and (result.returncode != 1 or unknown or not errors
                                          or any(record not in allowed for record in errors)):
                    raise SelectionError(f"command failed ({result.returncode}): {shlex.join(command)}\n"
                                         + result.stdout + result.stderr)
                accepted = f"Accepted {len(errors)} calibrated profile diagnostics.\n" if errors else ""
                return accepted + result.stdout + result.stderr, records
            diagnostics = []
            for output, records in parallel(compile_one, list(enumerate(selected)), args.jobs):
                print(output, end="")
                diagnostics.extend(records)
            return diagnostics
        else:
            tidy = shutil.which("clang-tidy-18") or shutil.which("clang-tidy")
            if not tidy:
                raise SelectionError("clang-tidy-18 or clang-tidy not found")
            config = checked_resolve(root / args.config_file)
            if not config.is_file():
                raise SelectionError(f"missing tidy configuration: {config}")
            def tidy_one(item):
                index, entry = item
                # One database per variant ensures clang-tidy does not silently
                # choose only the first target's flags for a duplicated TU.
                directory = work / str(index)
                directory.mkdir()
                (directory / "compile_commands.json").write_text(json.dumps([{
                    "file": str(entry["file"]), "directory": str(entry["directory"]),
                    "arguments": replay_arguments(entry) + ["-c", "-o", str(directory / "tidy.o")],
                }]), encoding="utf-8")
                commands = [[tidy, "-quiet", "-p", str(directory),
                             f"--config-file={config}", str(entry["file"])]]
                if args.also_ci_gate and ci_gate_file(root, entry["file"]):
                    commands.append([tidy, "-quiet", "-p", str(directory),
                                     "--config=" + json.dumps(gate_config(root, entry["file"])),
                                     str(entry["file"])])
                output = []
                for command in commands:
                    result = execute(command, root)
                    output.append(result.stdout + result.stderr)
                return "".join(output)
            for output in parallel(tidy_one, list(enumerate(selected)), args.jobs):
                print(output, end="")
    return []


def warning_baseline_destination(root: Path, destination: Path) -> Path:
    scratch = root / "scratch"
    if scratch.is_symlink() or destination.is_symlink():
        raise SelectionError("new warning baseline must be a fresh file under scratch")
    destination = checked_resolve(destination)
    if (destination.exists() or not destination.parent.is_dir()
            or not destination.is_relative_to(checked_resolve(scratch))):
        raise SelectionError("new warning baseline must be a fresh file under scratch")
    return destination


def publish_warning_baseline(root: Path, destination: Path, diagnostics: list[dict]) -> None:
    """Publish a new local baseline only after every variant passes calibration."""
    destination = warning_baseline_destination(root, destination)
    # Exclusive creation leaves an existing baseline untouched, including races.
    # Serialize before opening and remove incomplete writes on I/O failure.
    text = json.dumps({"version": 1, "diagnostics": diagnostics}, sort_keys=True, indent=2) + "\n"
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=destination.parent,
                                         prefix="warning-baseline-", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(text)
        os.link(temporary, destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("compile", "tidy"))
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--base-ref", default="origin/devel")
    parser.add_argument("--worktree", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--include-untracked", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--ccache", action="store_true")
    parser.add_argument("--config-file", type=Path, default=Path(".clang-tidy"))
    parser.add_argument("--also-ci-gate", action="store_true")
    parser.add_argument("--warning-profile", choices=("none", "msvc-approx"), default="none")
    baselines = parser.add_mutually_exclusive_group()
    baselines.add_argument("--warning-baseline", type=Path)
    baselines.add_argument("--write-warning-baseline", type=Path)
    parser.add_argument("--conversion-clean-prefix", type=Path, action="append", default=[])
    args = parser.parse_args(argv)
    start = time.monotonic()
    try:
        root = checked_resolve(args.root)
        if args.jobs < 1:
            raise SelectionError("--jobs must be positive")
        if ((args.warning_baseline or args.write_warning_baseline or args.conversion_clean_prefix)
                and args.warning_profile == "none"):
            raise SelectionError("warning calibration requires --warning-profile msvc-approx")
        if args.command != "compile" and args.warning_profile != "none":
            raise SelectionError("warning profiles apply to compile checks only")
        args.clean_prefixes = clean_prefixes(root, args.conversion_clean_prefix)
        args.baseline_records = (read_warning_baseline(checked_resolve(root / args.warning_baseline))
                                 if args.warning_baseline else [])
        if args.write_warning_baseline:
            args.write_warning_baseline = warning_baseline_destination(root, root / args.write_warning_baseline)
        changed = changed_paths(root, args.base_ref, args.worktree, args.include_untracked)
        build_dir = checked_resolve(root / args.build_dir)
        entries = read_database(root, build_dir)
        selected, report = select(root, entries, changed, args.jobs, build_dir)
        scan_done = time.monotonic()
        # Hash resolved, ordered per-variant arguments, not a global policy name.
        command_data = [{"file": str(e["file"]), "directory": str(e["directory"]),
                         "arguments": check_arguments(e, args.warning_profile)} for e in selected]
        fingerprint_data = {"commands": command_data, "baseline": args.baseline_records,
                            "conversion_clean_prefixes": [str(p) for p in args.clean_prefixes],
                            "calibration": "capture" if args.write_warning_baseline else
                                           "replay" if args.warning_baseline else "none"}
        report.update({"selected": [str(e["file"].relative_to(root)) for e in selected],
                       "scope": "configured first-party database; other configurations require separate checks",
                       "command_variants": len(selected), "warning_source": "compile_commands.json",
                       "warning_profile": args.warning_profile,
                       "command_fingerprint": hashlib.sha256(json.dumps(fingerprint_data, sort_keys=True).encode()).hexdigest(),
                       "selection_seconds": round(scan_done - start, 3)})
        print(json.dumps(report, sort_keys=True), flush=True)
        if selected:
            diagnostics = run_checks(args, root, selected, build_dir)
        else:
            diagnostics = []
            print("No native work selected.")
        if args.write_warning_baseline:
            publish_warning_baseline(root, args.write_warning_baseline, diagnostics)
        print(f"{args.command} seconds: {time.monotonic() - scan_done:.3f}")
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"changed translation units: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
