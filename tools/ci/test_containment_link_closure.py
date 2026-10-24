#!/usr/bin/env python3
"""Offline source/link audit for containment users, including inline loaders.

This reads the repository's explicit CMake source lists and source manifests,
not arbitrary CMake programs. Conditional branches are unioned so Windows and
POSIX sources are both checked. Vendor includes and external libraries are
outside this check; no configure, compiler, or downloaded dependencies are used.
"""

from copy import deepcopy
from functools import lru_cache
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
POLICY = Path("src/luminumbra_common/core/CaptureContentPaths.h")
OWNER = Path("src/luminumbra_common/core/FilesystemPath.cpp")
COMMAND = re.compile(r"\b([a-z_][a-z_0-9]*)\s*\(", re.IGNORECASE)
BRACKET = re.compile(r'\[(=*)\[')
TOKEN = re.compile(r'"([^"\n]*)"|([^\s]+)')
VARIABLE = re.compile(r"\$\{([^${}]+)\}")
OBJECTS = re.compile(r'\$<TARGET_OBJECTS:([^$<>:;\s]+)>')
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.MULTILINE)


def cmake_commands(body):
    """Read whole commands, including quoted parentheses and bracket arguments."""
    commands = []
    offset = 0
    while offset < len(body):
        if body[offset].isspace():
            offset += 1
            continue
        if body[offset] == '#':
            offset = body.find('\n', offset)
            if offset < 0:
                break
            continue
        match = COMMAND.match(body, offset)
        if not match:
            raise ValueError('invalid CMake command')
        offset = match.end()
        depth, quoted = 1, False
        argument = []
        while offset < len(body) and depth:
            char = body[offset]
            bracket = BRACKET.match(body, offset) if not quoted else None
            if char == '\\' and offset + 1 < len(body):
                argument.append(body[offset:offset + 2])
                offset += 2
                continue
            if bracket:
                end = body.find(']' + bracket[1] + ']', bracket.end())
                if end < 0:
                    raise ValueError('unterminated CMake bracket argument')
                end += len(bracket[1]) + 2
                argument.append(body[offset:end])
                offset = end
                continue
            if char == '"':
                quoted = not quoted
            elif not quoted:
                if char == '#':
                    end = body.find('\n', offset)
                    offset = len(body) if end < 0 else end
                    argument.append(' ')
                    continue
                if char == '(':
                    depth += 1
                elif char == ')':
                    depth -= 1
            if depth:
                argument.append(char)
            offset += 1
        if depth or quoted:
            raise ValueError('unterminated CMake command')
        commands.append((match[1].lower(), ''.join(argument)))
    return commands


def targets_in(root, cmake):
    """Return explicit sources and links, with source-list variables expanded."""
    # The offline graph uses a symbolic binary directory; FetchContent's default
    # source location is resolved without configuring or downloading dependencies.
    variables = {"CMAKE_SOURCE_DIR": str(root), "ENGINE_SOURCE": str(root),
                 "CMAKE_BINARY_DIR": str(root / 'build'),
                 "CMAKE_CURRENT_BINARY_DIR": str(root / 'build' / cmake.parent.relative_to(root))}
    targets = {}
    reading = set()

    def expand(value):
        for _ in range(20):
            result = VARIABLE.sub(lambda match: variables.get(match[1], match[0]), value)
            if result == value:
                return result
            value = result
        raise ValueError(f"recursive CMake variable in {cmake}: {value}")

    def execute(commands, path):
        index = 0
        while index < len(commands):
            command, text = commands[index]
            index += 1
            args = [expand(a or b) for a, b in TOKEN.findall(text)]
            if command == 'endforeach':
                raise ValueError('unexpected CMake endforeach')
            if command == 'foreach' and not args:
                raise ValueError('missing CMake foreach variable')
            if not args:
                continue
            if command in {'add_executable', 'add_library', 'target_sources', 'target_link_libraries'}:
                if '${' in args[0] or '$<' in args[0]:
                    raise ValueError(f'unresolved target in {cmake}: {args[0]}')
            if command == 'foreach':
                start, depth = index, 1
                while index < len(commands) and depth:
                    nested = commands[index][0]
                    depth += (nested == 'foreach') - (nested == 'endforeach')
                    index += 1
                if depth:
                    raise ValueError('unterminated CMake foreach')
                values = args[1:]
                if values[:1] == ['IN']:
                    if len(values) < 2 or values[1] not in ('ITEMS', 'LISTS'):
                        raise ValueError('unsupported CMake foreach')
                    values = (values[2:] if values[1] == 'ITEMS' else
                              [variables.get(name, '') for name in values[2:]])
                previous = variables.get(args[0])
                for value in (item for value in values for item in value.split(';') if item):
                    variables[args[0]] = value
                    execute(commands[start:index - 1], path)
                if previous is None:
                    variables.pop(args[0], None)
                else:
                    variables[args[0]] = previous
                continue
            if command == "set":
                values = args[1:]
                if 'CACHE' in values:
                    values = values[:values.index('CACHE')]
                variables[args[0]] = ";".join(values)
            elif command == 'list' and args[0] == 'APPEND':
                if len(args) < 2:
                    raise ValueError('missing CMake list variable')
                variables[args[1]] = ';'.join(filter(None, [variables.get(args[1], ''), *args[2:]]))
            elif command == 'fetchcontent_getproperties':
                if len(args) != 3 or args[1] != 'SOURCE_DIR':
                    raise ValueError('unsupported FetchContent properties')
                base = variables.get('FETCHCONTENT_BASE_DIR', variables['CMAKE_BINARY_DIR'] + '/_deps')
                variables[args[2]] = variables.get(args[0] + '_SOURCE_DIR',
                                                  str(Path(base) / (args[0].lower() + '-src')))
            elif command == "get_filename_component" and args[-1] == "ABSOLUTE":
                variables[args[0]] = str((path.parent / args[1]).resolve())
            elif command == "include" and args[0].endswith("/sources.cmake"):
                read(Path(args[0]))
            elif command in {"add_executable", "add_library", "target_sources"}:
                target = targets.setdefault(args[0], {"sources": set(), "links": set()})
                for arg in args[1:]:
                    for source in arg.split(";"):
                        objects = OBJECTS.fullmatch(source)
                        if objects:
                            target.setdefault('objects', set()).add(objects[1])
                            continue
                        if "${" in source or "$<" in source:
                            raise ValueError(f"unresolved source in {cmake}: {source}")
                        if Path(source).suffix.lower() not in {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp"}:
                            continue
                        resolved = (cmake.parent / source).resolve()
                        # Only first-party files can use the policy.
                        if resolved.is_relative_to(root) and resolved.is_file():
                            target["sources"].add(resolved.relative_to(root))
            elif command == "target_link_libraries":
                target = targets.setdefault(args[0], {"sources": set(), "links": set()})
                for arg in args[1:]:
                    for link in arg.split(';'):
                        objects = OBJECTS.fullmatch(link)
                        if objects:
                            target.setdefault('objects', set()).add(objects[1])
                        else:
                            # This form is used by the networking runtime fixture.
                            link = re.sub(r'\$<TARGET_LINKER_FILE:([^<>]+)>', r'\1', link)
                            if '$<' in link:
                                raise ValueError(f'unresolved link in {cmake}: {link}')
                            target['links'].add(link)

    def read(path):
        path = path.resolve()
        if path in reading:
            raise ValueError(f'recursive CMake include: {path}')
        reading.add(path)
        previous = variables.get("CMAKE_CURRENT_LIST_DIR")
        variables["CMAKE_CURRENT_LIST_DIR"] = str(path.parent)
        variables["CMAKE_CURRENT_SOURCE_DIR"] = str(cmake.parent)
        execute(cmake_commands(path.read_text(encoding="utf-8")), path)
        if previous is not None:
            variables["CMAKE_CURRENT_LIST_DIR"] = previous
        else:
            variables.pop('CMAKE_CURRENT_LIST_DIR', None)
        reading.remove(path)

    read(cmake)
    return targets


def repository_targets(root):
    """Read platform-unioned source and library lists once per repository scan."""
    targets = {}
    cmake_files = sorted(path for directory in ("src", "tools", "test")
                         for path in (root / directory).rglob("CMakeLists.txt"))
    for cmake in cmake_files:
        relative = cmake.relative_to(root)
        for name, target in targets_in(root, cmake).items():
            targets[(relative, name)] = target
    return targets


class IncludeGraph:
    """Snapshot source dependencies; target mutations never require rereading files."""
    def __init__(self, root):
        self.root = root
        self.edges = {}

    def includes(self, source):
        if source not in self.edges:
            result = set()
            for name in INCLUDE.findall((self.root / source).read_text(encoding="utf-8")):
                # Local headers take priority, followed by the engine include roots.
                for directory in (source.parent, Path("src"), Path("src/luminumbra_common"),
                                  Path("src/luminumbra_client"), Path("include"), Path("tools")):
                    candidate = (self.root / directory / name).resolve()
                    if candidate.is_relative_to(self.root) and candidate.is_file():
                        result.add(candidate.relative_to(self.root))
                        break
            self.edges[source] = result
        return self.edges[source]


def audit(targets, graph):
    """Find targets reaching the policy without the implementation in their closure."""

    def candidates(key, name):
        local = (key[0], name)
        # Standalone projects may reuse executable and fixture target names.
        return [local] if local in targets else [
            candidate for candidate in targets if candidate[1] == name
        ]

    @lru_cache(maxsize=None)
    def direct_sources(key):
        sources = set(targets[key]['sources'])
        for name in targets[key].get('objects', ()):
            matches = candidates(key, name)
            if len(matches) != 1:
                raise ValueError(f'unresolved CMake object target: {name}')
            # TARGET_OBJECTS includes only objects compiled by that target,
            # without its link requirements or other referenced object files.
            sources.update(targets[matches[0]]['sources'])
        return frozenset(sources)

    @lru_cache(maxsize=None)
    def uses_policy(sources):
        pending = list(sources)
        seen = set()
        while pending:
            source = pending.pop()
            if source == POLICY:
                return True
            if source not in seen:
                seen.add(source)
                pending.extend(graph.includes(source))
        return False

    def closure(key, seen):
        if key in seen:
            return set()
        seen.add(key)
        target = targets[key]
        sources = set(direct_sources(key))
        for link in target["links"]:
            for candidate in candidates(key, link):
                sources.update(closure(candidate, seen))
        return sources

    users = {}
    failures = []
    for key, target in targets.items():
        if uses_policy(direct_sources(key)):
            users[key] = target
            if OWNER not in closure(key, set()):
                failures.append(f"{key[0]}: {key[1]} uses containment without {OWNER}")
    return users, failures


class CMakeSourceParserTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.cmake = self.root / 'CMakeLists.txt'

    def sources(self, body, names=()):
        for name in names:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('', encoding='utf-8')
        self.cmake.write_text(body, encoding='utf-8')
        return targets_in(self.root, self.cmake)

    def test_set_nested_variables_and_source_lists(self):
        names = ('imgui/imgui.cpp', 'imgui/imgui_draw.cpp', 'extra (copy)#1.cpp')
        targets = self.sources('''
set(_dependency imgui)
set(imgui_source "${CMAKE_CURRENT_SOURCE_DIR}/imgui")
set(_developer_imgui_source "${${_dependency}_source}")
set(core "${_developer_imgui_source}/imgui.cpp;${_developer_imgui_source}/imgui_draw.cpp")
set(sources ${core})
list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/extra (copy)#1.cpp")
add_executable(developer_console_imgui_test ${sources})
''', names)
        self.assertEqual(targets['developer_console_imgui_test']['sources'], set(map(Path, names)))

    def test_generated_sources_use_current_binary_directory(self):
        for nested in (False, True):
            with self.subTest(nested=nested):
                directory = self.root / 'audio' if nested else self.root
                directory.mkdir(exist_ok=True)
                source = directory / 'shutdown.cpp'
                source.write_text('', encoding='utf-8')
                cmake = directory / 'CMakeLists.txt'
                cmake.write_text('''set(body "${CMAKE_CURRENT_BINARY_DIR}/shutdown_body.inc")
add_executable(shutdown shutdown.cpp "${body}")
''', encoding='utf-8')
                targets = targets_in(self.root, cmake)
                self.assertEqual(targets['shutdown']['sources'],
                                 {source.relative_to(self.root)})

    def test_fetchcontent_properties_resolve_sources(self):
        names = tuple(f'build/_deps/imgui_src-src/{name}.cpp' for name in
                      ('imgui', 'imgui_draw', 'imgui_tables', 'imgui_widgets'))
        targets = self.sources('''
FetchContent_GetProperties(imgui_src SOURCE_DIR _developer_imgui_source)
add_executable(imgui_test "${_developer_imgui_source}/imgui.cpp"
    "${_developer_imgui_source}/imgui_draw.cpp"
    "${_developer_imgui_source}/imgui_tables.cpp"
    "${_developer_imgui_source}/imgui_widgets.cpp")
set(imgui_src_SOURCE_DIR "${CMAKE_SOURCE_DIR}/local")
FetchContent_GetProperties(imgui_src SOURCE_DIR local_source)
target_sources(imgui_test PRIVATE "${local_source}/local.cpp")
''', (*names, 'local/local.cpp'))
        self.assertEqual(targets['imgui_test']['sources'], set(map(Path, (*names, 'local/local.cpp'))))

    def test_foreach_lists_items_and_generator_links(self):
        targets = self.sources('''
set(targets client;qa)
set(empty)
foreach(target IN LISTS empty)
    add_executable(ghost missing.cpp)
endforeach()
foreach(target ${targets})
    add_executable(${target} main.cpp)
    foreach(library IN ITEMS windows posix)
        target_link_libraries(${target} PRIVATE ${library})
    endforeach()
endforeach()
foreach(target IN LISTS targets)
    target_link_libraries(${target} PRIVATE "$<TARGET_LINKER_FILE:leaf>")
endforeach()
''', ('main.cpp',))
        self.assertEqual(set(targets), {'client', 'qa'})
        for target in targets.values():
            self.assertEqual(target['sources'], {Path('main.cpp')})
            self.assertEqual(target['links'], {'PRIVATE', 'windows', 'posix', 'leaf'})

    def test_included_manifests_restore_list_directory(self):
        manifest = self.root / 'nested' / 'sources.cmake'
        manifest.parent.mkdir()
        manifest.write_text('set(sources "${CMAKE_CURRENT_LIST_DIR}/included.cpp")', encoding='utf-8')
        targets = self.sources('''
include("${CMAKE_CURRENT_LIST_DIR}/nested/sources.cmake")
add_library(core ${sources} "${CMAKE_CURRENT_LIST_DIR}/local.cpp")
''', ('nested/included.cpp', 'local.cpp'))
        self.assertEqual(targets['core']['sources'], {Path('nested/included.cpp'), Path('local.cpp')})

    def test_object_sources_in_all_source_commands_and_forward_references(self):
        targets = self.sources('''
set(objects "$<TARGET_OBJECTS:entry>")
add_executable(app main.cpp ${objects})
add_library(wrapper STATIC "$<TARGET_OBJECTS:entry>")
target_sources(app PRIVATE "$<TARGET_OBJECTS:entry>")
target_link_libraries(wrapper PRIVATE "$<TARGET_OBJECTS:entry>")
add_library(entry OBJECT object.cpp)
''', ('main.cpp', 'object.cpp'))
        self.assertEqual(targets['app']['sources'], {Path('main.cpp')})
        self.assertEqual(targets['app']['objects'], {'entry'})
        self.assertEqual(targets['wrapper']['objects'], {'entry'})
        self.assertEqual(targets['entry']['sources'], {Path('object.cpp')})

    def test_object_sources_do_not_inherit_link_requirements(self):
        for command in ('target_sources', 'target_link_libraries'):
            with self.subTest(command=command):
                self.check_object_link_requirements(command)

    def check_object_link_requirements(self, command):
        targets = self.sources('''
add_executable(app main.cpp)
add_library(entry OBJECT object.cpp)
add_library(owner src/luminumbra_common/core/FilesystemPath.cpp)
target_link_libraries(entry PRIVATE owner)
''' + command + '(app PRIVATE "$<TARGET_OBJECTS:entry>")',
                               ('main.cpp', 'object.cpp', str(POLICY), str(OWNER)))
        (self.root / 'object.cpp').write_text(
            '#include "luminumbra_common/core/CaptureContentPaths.h"\n', encoding='utf-8')
        indexed = {(Path('CMakeLists.txt'), name): target for name, target in targets.items()}
        graph = IncludeGraph(self.root)
        users, failures = audit(indexed, graph)
        self.assertIn((Path('CMakeLists.txt'), 'app'), users)
        self.assertEqual(failures, [f'CMakeLists.txt: app uses containment without {OWNER}'])
        targets['app']['links'].add('owner')
        self.assertFalse(audit(indexed, graph)[1])
        targets['owner']['sources'].clear()
        self.assertEqual(len(audit(indexed, graph)[1]), 2)

    def test_object_sources_from_other_files_and_empty_targets(self):
        targets = self.sources('''
add_executable(app "$<TARGET_OBJECTS:entry>" "$<TARGET_OBJECTS:empty>")
add_library(empty OBJECT)
target_link_libraries(app PRIVATE forwarding)
''', ('object.cpp', str(POLICY), str(OWNER)))
        (self.root / 'object.cpp').write_text(
            '#include "luminumbra_common/core/CaptureContentPaths.h"\n', encoding='utf-8')
        indexed = {(Path('CMakeLists.txt'), name): target for name, target in targets.items()}
        external = Path('nested/CMakeLists.txt')
        indexed[(external, 'entry')] = {'sources': {Path('object.cpp')}, 'links': {'forwarding'}}
        indexed[(external, 'forwarding')] = {'sources': set(), 'links': {'owner'}}
        indexed[(external, 'owner')] = {'sources': {OWNER}, 'links': set()}
        users, failures = audit(indexed, IncludeGraph(self.root))
        self.assertIn((Path('CMakeLists.txt'), 'app'), users)
        self.assertFalse(failures)
        targets['app']['links'].clear()
        self.assertEqual(audit(indexed, IncludeGraph(self.root))[1],
                         [f'CMakeLists.txt: app uses containment without {OWNER}'])

    def test_object_sources_do_not_include_objects_linked_by_referenced_target(self):
        targets = self.sources('''
add_executable(app main.cpp "$<TARGET_OBJECTS:entry>")
add_library(entry OBJECT object.cpp)
add_library(owner OBJECT src/luminumbra_common/core/FilesystemPath.cpp)
target_link_libraries(entry PRIVATE "$<TARGET_OBJECTS:owner>")
''', ('main.cpp', 'object.cpp', str(POLICY), str(OWNER)))
        (self.root / 'object.cpp').write_text(
            '#include "luminumbra_common/core/CaptureContentPaths.h"\n', encoding='utf-8')
        indexed = {(Path('CMakeLists.txt'), name): target for name, target in targets.items()}
        _, failures = audit(indexed, IncludeGraph(self.root))
        self.assertEqual(failures, [f'CMakeLists.txt: app uses containment without {OWNER}'])

    def test_unknown_and_ambiguous_object_targets_are_refused(self):
        targets = self.sources('add_executable(app "$<TARGET_OBJECTS:missing>")')
        indexed = {(Path('CMakeLists.txt'), name): target for name, target in targets.items()}
        graph = IncludeGraph(self.root)
        with self.assertRaisesRegex(ValueError, 'unresolved CMake object target'):
            audit(indexed, graph)
        for directory in ('one', 'two'):
            indexed[(Path(directory) / 'CMakeLists.txt', 'missing')] = {
                'sources': set(), 'links': set()}
        with self.assertRaisesRegex(ValueError, 'unresolved CMake object target'):
            audit(indexed, graph)

    def test_unresolved_and_partial_input_is_refused(self):
        for body in ('add_library(core ${missing})',
                     'add_library(core "$<UNKNOWN:main.cpp>")',
                     'add_library(core "$<TARGET_OBJECTS:>")',
                     'add_library(core "$<TARGET_OBJECTS:entry")',
                     'add_library(core "$<TARGET_OBJECTS:${missing}>")',
                     'add_library(core "$<TARGET_OBJECTS:$<UNKNOWN:entry>>")',
                     'target_link_libraries(core "$<TARGET_OBJECTS:>")',
                     'target_link_libraries(core "$<TARGET_OBJECTS:entry")',
                     'target_link_libraries(core "$<TARGET_OBJECTS:${missing}>")',
                     'target_link_libraries(core "$<TARGET_OBJECTS:$<UNKNOWN:entry>>")',
                     'target_link_libraries(core "$<TARGET_OBJECTS:$<TARGET_LINKER_FILE:entry>>")',
                     'target_link_libraries(core "$<UNKNOWN:leaf>")',
                     'add_library(${missing} main.cpp)',
                     'target_link_libraries(${missing} PRIVATE leaf)',
                     'list(APPEND)',
                     'FetchContent_GetProperties(imgui SOURCE_DIR)',
                     'FetchContent_GetProperties(imgui UNKNOWN value)',
                     'foreach()\nendforeach()',
                     'foreach(item IN RANGE 2)\nendforeach()',
                     'foreach(item one)\nadd_library(core main.cpp)',
                     'endforeach()', 'set(source "main.cpp)',
                     'set(source [==[main.cpp)', 'invalid input'):
            with self.subTest(body=body), self.assertRaises(ValueError):
                self.sources(body)

    def test_recursive_expansion_and_includes_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'recursive CMake variable'):
            self.sources('set(loop "${loop}x")\nadd_library(core ${loop})')
        (self.root / 'sources.cmake').write_text(
            'include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'recursive CMake include'):
            self.sources('include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")')

    def test_fetchcontent_base_and_cached_variables(self):
        targets = self.sources('''
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/dependencies" CACHE PATH "unused.cpp")
FetchContent_GetProperties(ImGui SOURCE_DIR source)
add_library(core "${source}/imgui.cpp")
''', ('dependencies/imgui-src/imgui.cpp',))
        self.assertEqual(targets['core']['sources'], {Path('dependencies/imgui-src/imgui.cpp')})

    def test_unreadable_manifest_is_refused(self):
        with self.assertRaises(OSError):
            self.sources('include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")')

    def test_quoted_and_bracket_commands_do_not_create_targets(self):
        targets = self.sources('''
file(GENERATE OUTPUT unused CONTENT "set(fake missing.cpp)\\nadd_library(ghost ${fake})")
message([==[parentheses (and # comments) do not end this command]==])
add_library(core main.cpp) # trailing comment
''', ('main.cpp',))
        self.assertEqual(targets, {'core': {'sources': {Path('main.cpp')}, 'links': set()}})


class ContainmentLinkClosureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.targets = repository_targets(ROOT)
        cls.graph = IncludeGraph(ROOT)

    def test_ordinary_windows_names_return_before_expansion_io(self):
        source = (ROOT / OWNER).read_text(encoding='utf-8')
        body = source.split('std::filesystem::path LongNamePath(', 1)[1].split(
            'bool PathSpellingEqual(', 1)[0]
        condition = "path.native().find(L'~') == std::wstring::npos"
        self.assertIn(condition, body)
        guard = body.index(condition)
        returned = body.index('return OrdinaryWindowsPath(spelling.native());', guard)
        probe = body.index('GetLongPathNameW(')
        self.assertLess(returned, probe)
        self.assertIn('error.clear();', body[guard:returned])
        self.assertNotRegex(body[:returned], r'\b(?:GetLongPathNameW|CreateFileW|canonical)\s*\(')

    def test_real_imgui_sources_and_platform_loop_are_resolved(self):
        imgui = self.targets[(Path('test/CMakeLists.txt'), 'developer_console_imgui_test')]
        self.assertIn(Path('test/client/developer_console_imgui_test.cpp'), imgui['sources'])
        self.assertIn(Path('src/luminumbra_client/debug/DeveloperConsoleUi.cpp'), imgui['sources'])
        for name in ('luminumbra_client_app', 'luminumbra_client_qa_app'):
            with self.subTest(target=name):
                links = self.targets[(Path('src/CMakeLists.txt'), name)]['links']
                self.assertTrue({'opengl32', 'DbgHelp', 'Psapi', 'OpenGL::GL'} <= links)
        self.assertFalse(any('${' in name for _, name in self.targets))

    def test_repository_targets_have_complete_closures(self):
        users, failures = audit(self.targets, self.graph)
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
        for cmake, name in (
            ("tools/CMakeLists.txt", "asset_processor"),
            ("tools/blender/authoring/probes/tool_build/CMakeLists.txt", "asset_processor"),
            ("tools/blender/authoring/probes/tool_build/CMakeLists.txt", "animation_readback"),
            ("test/CMakeLists.txt", "game_assets_test"),
            ("test/CMakeLists.txt", "morph_asset_test"),
            ("tools/prefab_runtime/CMakeLists.txt", "luminumbra_prefab_runtime"),
        ):
            with self.subTest(target=name, cmake=cmake):
                targets = deepcopy(self.targets)
                targets[(Path(cmake), name)]["sources"].remove(OWNER)
                _, failures = audit(targets, self.graph)
                self.assertIn(f"{Path(cmake)}: {name} uses containment without {OWNER}", failures)
                if name == "luminumbra_prefab_runtime":
                    self.assertTrue(any("luminumbra_render_static uses containment" in failure
                                        for failure in failures))

    def test_repeated_audits_reuse_source_graph(self):
        audit(self.targets, self.graph)
        with patch.object(Path, 'read_text', side_effect=AssertionError('source graph reread')):
            users, failures = audit(self.targets, self.graph)
        self.assertTrue(users)
        self.assertFalse(failures, '\n'.join(failures))


if __name__ == "__main__":
    unittest.main()
