"""Ratchet literal engine-to-game includes against a reviewed baseline.

Ownership follows docs/engine-game-boundary.md: species/ecology rules, farming,
photography, abilities, game composition and presentation belong to the game.
The existing EngineGameSplitLint treats src/ as engine-side; its noun allowlist
is not an include policy. Mixed targets remain engine-side until extraction.
Generic fields, behavior planners, biome/material registration and UI services
remain engine facilities. The patterns below name policy-bearing code, including
mixed headers that still expose game rules. New files in these domains inherit
ownership; other src/ and include/ files default to engine ownership.

This is a lexical include contract, not proof of engine-only linkage or startup.
It unions preprocessor branches and CMake platform branches, follows engine
headers, and stops at the first game header. Baseline entries are unique directed
(source, header) edges per compile target; the headers bucket also covers public
and otherwise unreferenced engine headers. Removals are allowed; prune retired
entries when extracting code. No baseline is generated during a test run.
"""

from collections import Counter
import fnmatch
import posixpath
from pathlib import Path, PurePosixPath
import re
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
IO = runpy.run_path(str(ROOT / "tools/ci/ratchet.py"))
CMAKE = runpy.run_path(str(ROOT / "tools/ci/test_containment_link_closure.py"))
BASELINE = "tools/ci/engine_game_include_baseline.json"
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"})
HEADER_SUFFIXES = SOURCE_SUFFIXES - {".c", ".cc", ".cpp", ".cxx"}
# Each domain is a game responsibility in the ownership table. Reusable AI
# primitives (InstinctPlanner, perception, spatial grids) are intentionally absent.
GAME_PATTERNS = (
    "src/luminumbra_common/game/*",
    "src/luminumbra_client/ui/components/game/*",
    *("src/luminumbra_common/ai/" + name + "*" for name in (
        "Creature", "Ecology", "Evolution", "CircadianSystem", "DecompositionSystem",
        "ForagingSystem", "HerdAlarmSystem", "LifespanSystem", "MateSpatialIndex",
        "MigrationSystem", "PredatorPackSystem", "ScavengingSystem", "ScentDepositSystem",
        "ScentSteeringSystem", "TerritorySystem", "ThirstSystem", "WildlifeFoliageSystem")),
    *("src/luminumbra_common/components/" + name + "*" for name in (
        "Alarm", "Circadian", "Creature", "CropLifecycle", "Decay", "Disease", "Foraging",
        "Grazeable", "Irrigation", "Migratory", "Mortal", "PackHunter", "Plant",
        "Pollination", "Scavenger", "Soil", "Territory", "Thirst")),
    *("src/luminumbra_common/systems/" + name + "*" for name in (
        "CreatureProcgen", "CropLifecycle", "Farming", "Irrigation", "PhotoScoring",
        "PlantCoarse", "PlantDisease", "PlantGrowth", "PlantProcgen", "Pollination", "SoilNutrient")),
    "src/luminumbra_common/persistence/PlantPersistence.h",
    "src/luminumbra_common/world/GameSession.*",
    "src/luminumbra_common/world/PlayerAvatar.*",
    "src/luminumbra_client/WorldDressing.*",
    "src/luminumbra_client/player/PlayerController.*",
    *("src/luminumbra_client/app/" + name + "*" for name in (
        "BushFoliage", "CaveFlourishes", "CreatureBodyAttachment", "FarmSpeciesCache",
        "GameAssets", "MenuScreens", "PhotoEnvironment", "ProcgenPalettes")),
    "src/luminumbra_client/audio/EnvironmentalAudio*",
)
INCLUDE_ROOTS = ("src", "src/luminumbra_client", "src/luminumbra_common", "include")
DIRECTIVE = re.compile(r'^[^\S\n]*(?:#|%:)[^\S\n]*include\b([^\n]*)', re.MULTILINE)
LITERAL = re.compile(r'\s*(?:"([^"\n]+)"|<([^>\n]+)>)\s*')
# RmlUi's external custom GL loader is the sole existing computed include.
EXTERNAL_INCLUDE = ("src/luminumbra_client/ui/gl3/RmlUi_Renderer_GL3.cpp", "RMLUI_GL3_CUSTOM_LOADER")


def game_owned(path):
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in GAME_PATTERNS)


def include_names(path, source):
    # Preprocessing joins physical lines before removing comments. Masking also
    # prevents a directive-looking line inside a raw string becoming an edge.
    source = re.sub(r'\\\r?\n', '', source)
    comments = IO["mask"](source, literals=False)
    tokens = IO["mask"](source)
    for directive in DIRECTIVE.finditer(comments):
        if not re.search(r'#|%:', tokens[directive.start():directive.start(1)]):
            continue
        operand = directive[1].strip()
        literal = LITERAL.fullmatch(operand)
        if literal:
            yield literal[1] or literal[2], literal[1] is not None
        elif (path, operand) != EXTERNAL_INCLUDE:
            raise ValueError(f"unsupported include in {path}: {operand}")


def resolve_include(source, name, quoted, paths, folded=None):
    # Normalize lexically against the repository inventory; never probe arbitrary
    # include paths on disk. '..' in ordinary relative includes is supported.
    name = name.replace('\\', '/')
    if name.startswith('/') or ':' in name:
        raise ValueError(f"absolute include in {source}: {name}")
    if folded is None:
        folded = {path.casefold(): path for path in paths}
    roots = ((posixpath.dirname(source),) if quoted else ()) + INCLUDE_ROOTS
    for directory in roots:
        candidate = posixpath.normpath(posixpath.join(directory, name))
        if candidate == '..' or candidate.startswith('../'):
            if quoted and directory == posixpath.dirname(source):
                raise ValueError(f"escaping include in {source}: {name}")
            continue
        if candidate in paths:
            return candidate
        if candidate.casefold() in folded:
            raise ValueError(f"noncanonical include case in {source}: {name}")
    # System/vendor and generated headers are outside this source contract. A
    # misspelled or deleted game include must not silently erase a boundary edge.
    for directory in roots:
        candidate = posixpath.normpath(posixpath.join(directory, name))
        if game_owned(candidate):
            raise ValueError(f"missing game include in {source}: {name}")
    return None


def collect_edges(sources, targets):
    graph = {}
    folded = {path.casefold(): path for path in sources}
    for path, source in sorted(sources.items()):
        graph[path] = {resolved for name, quoted in include_names(path, source)
                       if (resolved := resolve_include(path, name, quoted, sources, folded)) is not None}
    # Work is bounded by the Git inventory (10 MiB), each input (10 MiB), and
    # each target visiting every reachable file/edge at most once. No runtime work.
    # Header-only and unreferenced engine services receive the same protection.
    groups = dict(targets)
    groups["headers"] = {path for path in sources if PurePosixPath(path).suffix in HEADER_SUFFIXES
                         and not game_owned(path)}
    result = set()
    for target, roots in sorted(groups.items()):
        pending, visited = list(sorted(roots)), set()
        while pending:
            source = pending.pop()
            if source in visited:
                continue
            visited.add(source)
            if source not in graph:
                raise ValueError(f"missing target source: {target}: {source}")
            for header in sorted(graph[source]):
                if game_owned(header):
                    result.add((target, source, header))
                else:
                    pending.append(header)
    return result


def read_baseline(tree):
    data = tree.json(BASELINE)
    if (not isinstance(data, dict) or set(data) != {"schema", "edges"}
            or type(data["schema"]) is not int or data["schema"] != 1
            or not isinstance(data["edges"], list)):
        raise ValueError("invalid include baseline")
    result = set()
    for row in data["edges"]:
        if (not isinstance(row, list) or len(row) != 3
                or any(not isinstance(value, str) or not value for value in row)):
            raise ValueError("invalid baseline edge")
        target, source, header = row
        IO["relative_path"](source)
        IO["relative_path"](header)
        if not game_owned(header) or (target, source, header) in result:
            raise ValueError("duplicate or non-game baseline edge")
        result.add((target, source, header))
    return result


def additions(current, baseline):
    return sorted(current - baseline)


def repository_edges(tree):
    sources = {path: tree.read(path) for path in sorted(tree.paths)
               if path.startswith(("src/", "include/")) and PurePosixPath(path).suffix in SOURCE_SUFFIXES}
    targets = {}
    # Reuse the existing tested CMake parser: it unions conditional branches,
    # expands manifests and tracks compile ownership without native configuration.
    for manifest in ("src/CMakeLists.txt", "src/luminumbra_common/CMakeLists.txt"):
        for target, details in CMAKE["targets_in"](tree.root, tree.root / manifest,
                                                strict_sources=True).items():
            owned = {path.as_posix() for path in details["sources"] if path.as_posix() in sources}
            if owned:
                if target in {"headers", "unregistered"}:
                    raise ValueError(f"reserved inventory target: {target}")
                if target in targets:
                    raise ValueError(f"duplicate compile target: {target}")
                targets[target] = owned
    if not targets or not sources:
        raise ValueError("empty engine source/target inventory")
    # An unregistered translation unit must not provide an escape hatch.
    compiled = set().union(*targets.values())
    targets["unregistered"] = {path for path in sources if path not in compiled
                               and PurePosixPath(path).suffix not in HEADER_SUFFIXES}
    return collect_edges(sources, targets), targets


class EngineGameBoundary(unittest.TestCase):
    def test_repository_has_no_new_game_include_edges(self):
        tree = IO["Tree"](ROOT)
        current, targets = repository_edges(tree)
        new = additions(current, read_baseline(tree))
        counts = Counter(target for target, _, _ in current)
        print("Game include edges: " + ", ".join(f"{target}={counts[target]}"
              for target in sorted({*targets, "headers"})))
        self.assertFalse(new, "New engine/game include edges (extract or invert the dependency):\n"
                         + '\n'.join(f"{target}: {source} -> {header}" for target, source, header in new))

    def test_new_source_and_target_are_checked_from_the_working_tree(self):
        with tempfile.TemporaryDirectory(prefix="engine-boundary-") as directory:
            root = Path(directory).resolve()
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            files = {
                "src/CMakeLists.txt": 'add_library(service "luminumbra_common/core/Service.cpp")',
                "src/luminumbra_common/CMakeLists.txt": '',
                "src/luminumbra_common/core/Service.cpp": '#include "../game/PhotoMode.h"',
                "src/luminumbra_common/game/PhotoMode.h": '',
                BASELINE: '{"schema": 1, "edges": []}',
            }
            for name, text in files.items():
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(text, encoding="utf-8")
            # The real repository assertion reads the mutated checkout, including
            # a new compile target and untracked files, rather than a mock graph.
            with patch.dict(globals(), ROOT=root):
                with self.assertRaisesRegex(AssertionError, "New engine/game include edges"):
                    self.test_repository_has_no_new_game_include_edges()
            current, _ = repository_edges(IO["Tree"](root))
            self.assertEqual(current, {("service", "src/luminumbra_common/core/Service.cpp",
                                       "src/luminumbra_common/game/PhotoMode.h")})
            (root / "src/CMakeLists.txt").write_text('', encoding="utf-8")
            (root / "src/luminumbra_common/CMakeLists.txt").write_text(
                'add_library(common "core/Service.cpp")', encoding="utf-8")
            (root / "src/Unregistered.cpp").write_text(
                '#include "luminumbra_common/game/PhotoMode.h"', encoding="utf-8")
            current, _ = repository_edges(IO["Tree"](root))
            self.assertIn(("unregistered", "src/Unregistered.cpp",
                           "src/luminumbra_common/game/PhotoMode.h"), current)
            for name in ("headers", "unregistered", "common"):
                (root / "src/CMakeLists.txt").write_text(
                    f'add_library({name} "Unregistered.cpp")', encoding="utf-8")
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, "reserved|duplicate"):
                    repository_edges(IO["Tree"](root))
            (root / "src/CMakeLists.txt").write_text('', encoding="utf-8")
            (root / "src/luminumbra_common/CMakeLists.txt").write_text('', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "empty engine source/target inventory"):
                repository_edges(IO["Tree"](root))

    def test_registration(self):
        registry = runpy.run_path(str(ROOT / "tools/ci/ci_paths.py"))
        self.assertEqual(registry["PYTHON_TESTS"].count("tools/ci/test_engine_game_boundary.py"), 1)
        for directory in ("src/luminumbra_common/new", "include/luminumbra/new"):
            for suffix in SOURCE_SUFFIXES | {".cmake"}:
                self.assertTrue(registry["needs_python"](directory + "/Service" + suffix))
        self.assertTrue(registry["needs_python"]("src/CMakeLists.txt"))
        self.assertTrue(registry["needs_python"](BASELINE))
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('add_test(NAME EngineGameBoundarySourceContract COMMAND "${Python3_EXECUTABLE}" -I -B\n'
                      '    ${CMAKE_SOURCE_DIR}/tools/ci/test_engine_game_boundary.py)', cmake)

    def test_new_edges_fail_even_when_total_decreases(self):
        sources = {
            "src/luminumbra_common/core/Service.cpp": '#include "../game/PhotoMode.h"\n',
            "src/luminumbra_common/game/PhotoMode.h": '',
        }
        current = collect_edges(sources, {"luminumbra_common": {next(iter(sources))}})
        old = {("luminumbra_common", "retired.cpp", "retired.h"),
               ("luminumbra_common", "other.cpp", "retired.h")}
        self.assertEqual(additions(current, old), [
            ("luminumbra_common", "src/luminumbra_common/core/Service.cpp",
             "src/luminumbra_common/game/PhotoMode.h")])
        self.assertEqual(additions(set(), current), [])
        self.assertEqual(additions(current, current), [])

    def test_headers_cycles_transitive_owners_and_platform_branches(self):
        sources = {
            "src/luminumbra_common/core/Service.cpp": '#include "Bridge.h"\n',
            "src/luminumbra_common/core/Bridge.h": '#include "Bridge.h"\n'
                '#ifdef _WIN32\n#include <luminumbra_common/game/PhotoMode.h>\n'
                '#else\n#include "../game/Objectives.h"\n#endif\n',
            "src/luminumbra_common/game/PhotoMode.h": '',
            "src/luminumbra_common/game/Objectives.h": '',
            "include/Unreferenced.hpp": '#include "luminumbra_common/game/PhotoMode.h"\n',
        }
        roots = {"common": {next(iter(sources))}, "server": {next(iter(sources))}}
        edges = collect_edges(sources, roots)
        self.assertEqual(Counter(row[0] for row in edges), {"common": 2, "server": 2, "headers": 3})
        self.assertEqual(len(additions(edges, set())), 7)

    def test_include_spelling_trivia_and_local_priority(self):
        source = ('// #include "ignored.h"\n/*\n#include "ignored.h"\n*/\n'
                  'const char* text = R"(\n#include "ignored.h"\n)";\n'
                  '# include \\\n /*comment*/ "../game/PhotoMode.h" // comment\n'
                  '#include <vector>\n#include "../game/PhotoMode.h"\n')
        path = "src/luminumbra_common/core/Service.cpp"
        game = "src/luminumbra_common/game/PhotoMode.h"
        self.assertEqual(collect_edges({path: source, game: ''}, {"common": {path}}),
                         {("common", path, game)})
        self.assertEqual(collect_edges({path: '%:include "../game/PhotoMode.h"\r\n', game: ''},
                                       {"common": {path}}), {("common", path, game)})
        names = {"src/luminumbra_client/core/Local.h", "src/luminumbra_common/Local.h"}
        self.assertEqual(resolve_include("src/luminumbra_client/core/Service.cpp", "Local.h", True, names),
                         "src/luminumbra_client/core/Local.h")
        self.assertEqual(resolve_include(path, '..\\game\\PhotoMode.h', True, {game}), game)

    def test_unreadable_or_incomplete_inputs_refuse(self):
        with patch.object(IO["Tree"], "read", side_effect=OSError("unreadable")):
            with self.assertRaises(OSError):
                repository_edges(IO["Tree"](ROOT))
        with self.assertRaisesRegex(ValueError, "noncanonical include case"):
            resolve_include("src/core/Service.cpp", "LUMINUMBRA_COMMON/game/PhotoMode.h", True,
                            {"src/luminumbra_common/game/PhotoMode.h"})
        for operand in ('GAME_HEADER', '"unfinished', '<unfinished', '"good.h" extra', ''):
            with self.subTest(operand=operand), self.assertRaisesRegex(ValueError, "unsupported include"):
                list(include_names("src/Service.cpp", '#include ' + operand))
        self.assertEqual(list(include_names(EXTERNAL_INCLUDE[0], '#include ' + EXTERNAL_INCLUDE[1])), [])
        for name in ('/absolute.h', 'C:/game/header.h', '../../../../outside.h',
                     'luminumbra_common/game/Missing.h'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                resolve_include("src/core/Service.cpp", name, True, set())
        with self.assertRaisesRegex(ValueError, "missing target source"):
            collect_edges({}, {"common": {"src/missing.cpp"}})

    def test_baseline_schema_refuses_invalid_or_duplicate_data(self):
        edge = ["common", "src/service.cpp", "src/luminumbra_common/game/PhotoMode.h"]
        invalid = (None, {}, {"schema": True, "edges": []}, {"schema": 2, "edges": []},
                   {"schema": 1, "edges": [edge, edge]}, {"schema": 1, "edges": [["short"]]},
                   {"schema": 1, "edges": [["common", "../escape", edge[2]]]},
                   {"schema": 1, "edges": [["common", "src/service.cpp", "src/core/Service.h"]]})
        tree = IO["Tree"](ROOT)
        for data in invalid:
            with self.subTest(data=data), patch.object(tree, 'json', return_value=data):
                with self.assertRaises(ValueError):
                    read_baseline(tree)
        with patch.object(tree, 'json', return_value={"schema": 1, "edges": [edge]}):
            self.assertEqual(read_baseline(tree), {tuple(edge)})
        with patch.object(tree, 'json', return_value={"schema": 1, "edges": []}):
            self.assertEqual(read_baseline(tree), set())

    def test_ownership_distinguishes_game_policy_from_reusable_services(self):
        for path in ("src/luminumbra_common/game/NewRules.h",
                     "src/luminumbra_common/ai/CreatureReproductionSystem.h",
                     "src/luminumbra_common/systems/FarmingSystem.h",
                     "src/luminumbra_client/ui/components/game/NewScreen.hpp"):
            self.assertTrue(game_owned(path), path)
        for path in ("src/luminumbra_common/ai/InstinctPlanner.h",
                     "src/luminumbra_common/fields/FieldGrid.h",
                     "src/luminumbra_common/world/BiomeTable.h",
                     "src/luminumbra_client/ui/core/UIComponent.h",
                     "include/luminumbra/rendering/StaticRenderer.h"):
            self.assertFalse(game_owned(path), path)


if __name__ == "__main__":
    unittest.main()
