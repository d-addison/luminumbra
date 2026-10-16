#!/usr/bin/env python3
"""Offline source/link audit for containment users, including inline loaders.

This reads the repository's explicit CMake source lists and source manifests,
not arbitrary CMake programs. Conditional branches are unioned so Windows and
POSIX sources are both checked. Vendor includes and external libraries are
outside this check; no configure, compiler, or downloaded dependencies are used.
"""

from functools import lru_cache
from pathlib import Path
import re
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
POLICY = Path("src/luminumbra_common/core/CaptureContentPaths.h")
OWNER = Path("src/luminumbra_common/core/FilesystemPath.cpp")
COMMAND = re.compile(
    r"\b(set|include|get_filename_component|add_executable|add_library|"
    r"target_sources|target_link_libraries)\s*\((.*?)\)", re.DOTALL | re.IGNORECASE,
)
TOKEN = re.compile(r'"([^"\n]*)"|([^\s]+)')
VARIABLE = re.compile(r"\$\{([^}]+)\}")
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.MULTILINE)


def targets_in(root, cmake):
    """Return explicit sources and links, with source-list variables expanded."""
    variables = {"CMAKE_SOURCE_DIR": str(root), "ENGINE_SOURCE": str(root)}
    targets = {}

    def expand(value):
        for _ in range(20):
            result = VARIABLE.sub(lambda match: variables.get(match[1], match[0]), value)
            if result == value:
                return result
            value = result
        raise ValueError(f"recursive CMake variable in {cmake}: {value}")

    def read(path):
        previous = variables.get("CMAKE_CURRENT_LIST_DIR")
        variables["CMAKE_CURRENT_LIST_DIR"] = str(path.parent)
        variables["CMAKE_CURRENT_SOURCE_DIR"] = str(cmake.parent)
        body = re.sub(r"#[^\n]*", "", path.read_text(encoding="utf-8"))
        for match in COMMAND.finditer(body):
            command = match[1].lower()
            args = [expand(a or b) for a, b in TOKEN.findall(match[2])]
            if not args:
                continue
            if command == "set":
                variables[args[0]] = ";".join(args[1:])
            elif command == "get_filename_component" and args[-1] == "ABSOLUTE":
                variables[args[0]] = str((path.parent / args[1]).resolve())
            elif command == "include" and args[0].endswith("/sources.cmake"):
                read(Path(args[0]))
            elif command in {"add_executable", "add_library", "target_sources"}:
                target = targets.setdefault(args[0], {"sources": set(), "links": set()})
                for arg in args[1:]:
                    for source in arg.split(";"):
                        if Path(source).suffix.lower() not in {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp"}:
                            continue
                        if "${" in source or "$<" in source:
                            raise ValueError(f"unresolved source in {cmake}: {source}")
                        resolved = (cmake.parent / source).resolve()
                        # Only first-party files can use the policy.
                        if resolved.is_relative_to(root) and resolved.is_file():
                            target["sources"].add(resolved.relative_to(root))
            elif command == "target_link_libraries":
                target = targets.setdefault(args[0], {"sources": set(), "links": set()})
                target["links"].update(item for arg in args[1:] for item in arg.split(";"))
        if previous is not None:
            variables["CMAKE_CURRENT_LIST_DIR"] = previous

    read(cmake)
    return targets


def audit(root):
    """Find targets reaching the policy without the implementation in their closure."""
    targets = {}
    cmake_files = sorted(path for directory in ("src", "tools", "test")
                         for path in (root / directory).rglob("CMakeLists.txt"))
    for cmake in cmake_files:
        relative = cmake.relative_to(root)
        for name, target in targets_in(root, cmake).items():
            targets[(relative, name)] = target

    @lru_cache(maxsize=None)
    def includes(source):
        result = set()
        for name in INCLUDE.findall((root / source).read_text(encoding="utf-8")):
            # Local headers take priority, followed by the engine include roots.
            for directory in (source.parent, Path("src"), Path("src/luminumbra_common"),
                              Path("src/luminumbra_client"), Path("include"), Path("tools")):
                candidate = (root / directory / name).resolve()
                if candidate.is_relative_to(root) and candidate.is_file():
                    result.add(candidate.relative_to(root))
                    break
        return result

    def uses_policy(sources):
        pending = list(sources)
        seen = set()
        while pending:
            source = pending.pop()
            if source == POLICY:
                return True
            if source not in seen:
                seen.add(source)
                pending.extend(includes(source))
        return False

    def closure(key, seen):
        if key in seen:
            return set()
        seen.add(key)
        target = targets[key]
        sources = set(target["sources"])
        for link in target["links"]:
            local = (key[0], link)
            # Standalone projects may reuse executable and fixture target names.
            candidates = [local] if local in targets else [
                candidate for candidate in targets
                if candidate[1] == link and targets[candidate]["sources"]
            ]
            for candidate in candidates:
                sources.update(closure(candidate, seen))
        return sources

    users = {}
    failures = []
    for key, target in targets.items():
        if uses_policy(target["sources"]):
            users[key] = target
            if OWNER not in closure(key, set()):
                failures.append(f"{key[0]}: {key[1]} uses containment without {OWNER}")
    return users, failures


class ContainmentLinkClosureTests(unittest.TestCase):
    def test_repository_targets_have_complete_closures(self):
        users, failures = audit(ROOT)
        self.assertFalse(failures, "\n".join(failures))
        # Guard the audit's ability to see header-only and indirect users.
        for cmake, target in (
            ("tools/CMakeLists.txt", "asset_processor"),
            ("tools/blender/authoring/probes/tool_build/CMakeLists.txt", "asset_processor"),
            ("tools/blender/authoring/probes/tool_build/CMakeLists.txt", "animation_readback"),
            ("test/CMakeLists.txt", "game_assets_test"),
            ("test/CMakeLists.txt", "morph_asset_test"),
            ("tools/prefab_runtime/CMakeLists.txt", "luminumbra_prefab_runtime"),
            ("tools/static_preview/CMakeLists.txt", "luminumbra_render_static"),
            ("src/luminumbra_common/CMakeLists.txt", "luminumbra_common"),
            ("src/CMakeLists.txt", "luminumbra_client"),
        ):
            with self.subTest(target=target, cmake=cmake):
                self.assertIn((Path(cmake), target), users)

    def test_missing_owners_are_detected_in_each_standalone_project(self):
        parse_targets = targets_in
        for cmake, name in (
            ("tools/CMakeLists.txt", "asset_processor"),
            ("tools/blender/authoring/probes/tool_build/CMakeLists.txt", "asset_processor"),
            ("tools/blender/authoring/probes/tool_build/CMakeLists.txt", "animation_readback"),
            ("test/CMakeLists.txt", "game_assets_test"),
            ("test/CMakeLists.txt", "morph_asset_test"),
            ("tools/prefab_runtime/CMakeLists.txt", "luminumbra_prefab_runtime"),
        ):
            with self.subTest(target=name, cmake=cmake):
                def without_owner(root, path):
                    targets = parse_targets(root, path)
                    if path == root / cmake:
                        targets[name]["sources"].discard(OWNER)
                    return targets

                with patch(__name__ + ".targets_in", side_effect=without_owner):
                    _, failures = audit(ROOT)
                self.assertIn(f"{Path(cmake)}: {name} uses containment without {OWNER}", failures)
                if name == "luminumbra_prefab_runtime":
                    self.assertTrue(any("luminumbra_render_static uses containment" in failure
                                        for failure in failures))


if __name__ == "__main__":
    unittest.main()
