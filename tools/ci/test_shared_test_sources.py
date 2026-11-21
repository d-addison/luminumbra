#!/usr/bin/env python3
"""Ratchet repeated production compilation in the test CMake graph, offline.

Platform and optional branches are unioned. Allowances name the remaining
compilation owners, so neither a new source nor another owner can pass silently.
Run with --scan to print the census without enforcing the allowance.
"""

from collections import Counter
from pathlib import Path
import runpy
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
_PARSE = runpy.run_path(str(ROOT / "tools/ci/test_containment_link_closure.py"))["targets_in"]

# Remove owners as sharing becomes possible; do not extend existing allowances.
_CAPTURE_VARIANTS = (
    frozenset({"capture_profile_test_support", "capture_launch_test_support",
               "capture_manifest_test", "diagnostic_bundle_test"}),
    "Profile support uses first-party warnings/debug definitions; launch support "
    "does not. Manifest adds glad (including its shared-library export definition); "
    "bundle adds LUMINUMBRA_SOURCE_ROOT.",
)
_DEVELOPER_VARIANTS = (
    frozenset({"developer_console_pure_test", "developer_console_imgui_test"}),
    "The pure target links spdlog's compiled-library definitions; the ImGui target "
    "compiles an isolated core without that dependency.",
)
_UI_VARIANTS = (
    frozenset({"ui_smoke_test", "ui_page_load_test"}),
    "Both UI tests compile with source/artifact-root definitions absent from the "
    "client library. A new support archive would also link the client's copies; "
    "retain the existing direct-source override and archive extraction order.",
)
_REFLECTION_VARIANTS = (
    frozenset({"shader_reflection_test", "pilot_shader_reflection_test"}),
    "The pilot is excluded from default builds without slangc and defines "
    "PILOT_SHADER_REFLECT_DIR; the GL target defines LUMINUMBRA_TEST_ARTIFACT_DIR.",
)
ALLOWLIST = {
    "src/luminumbra_common/core/GraphicsPresetClassifier.cpp": (
        frozenset({"graphics_preset_classifier_test", "hardware_info_test"}),
        "Hardware collection adds the fake SDK include path; the standalone "
        "classifier has no SDK override in its compiler arguments."),
    "src/luminumbra_client/audio/AudioSpatialCluster.cpp": (
        frozenset({"audio_occlusion_test", "audio_physics_binding_test"}),
        "Physics binding inherits miniaudio's include path; occlusion keeps "
        "the common-only header search path."),
    "src/luminumbra_client/ui/core/UIComponent.cpp": _UI_VARIANTS,
    "src/luminumbra_client/ui/core/UIStateManager.cpp": _UI_VARIANTS,
    "src/luminumbra_client/core/RuntimeScenarioConfig.cpp": _CAPTURE_VARIANTS,
    "src/luminumbra_client/rendering/DiagnosticCapture.cpp": _CAPTURE_VARIANTS,
    "src/luminumbra_client/rendering/TemporalAudit.cpp": _CAPTURE_VARIANTS,
    "src/luminumbra_client/app/RegisterDeveloperWidgets.cpp": _DEVELOPER_VARIANTS,
    "src/luminumbra_client/debug/DeveloperCommandRegistry.cpp": _DEVELOPER_VARIANTS,
    "src/luminumbra_client/debug/DeveloperConsole.cpp": _DEVELOPER_VARIANTS,
    "src/luminumbra_client/debug/DeveloperLogSink.cpp": _DEVELOPER_VARIANTS,
    "src/luminumbra_client/debug/DeveloperWidgetRegistry.cpp": _DEVELOPER_VARIANTS,
    "src/luminumbra_client/rendering/PassShaderLayouts.cpp": _REFLECTION_VARIANTS,
    "src/luminumbra_client/rendering/ShaderReflection.cpp": _REFLECTION_VARIANTS,
    "src/luminumbra_client/rendering/CaptureHooks.cpp": (
        frozenset({"capture_sdk_trigger_test", "render_capture_test"}),
        "Render capture adds LUMINUMBRA_SOURCE_ROOT, first-party policy and GL "
        "dependencies; SDK triggering keeps its isolated API doubles."),
    "src/luminumbra_client/rendering/rhi/RhiBackend.cpp": (
        frozenset({"rhi_backend_test", "rhi_device_bringup_test"}),
        "Device bringup is conditional on LUMINUMBRA_ENABLE_DILIGENT and inherits "
        "Diligent backend definitions; the parser target has no backend dependency."),
    "src/luminumbra_common/core/FilesystemPath.cpp": (
        frozenset({"morph_asset_test", "filesystem_path_test", "game_assets_test",
                   "shader_reflection_test", "created_world_artifacts_test"}),
        "Morph pins FP and first-party policy; path defines LUMINUMBRA_SOURCE_ROOT; "
        "assets inherits JSON definitions; reflection defines source/artifact roots "
        "and inherits GL exports; created-world interception uses first-party "
        "policy without those definitions. Keep interception isolated."),
    "src/luminumbra_common/world/FarVolumeMesher.cpp": (
        frozenset({"far_volume_mesher_test", "far_volume_facade_test"}),
        "Facade adds LUMINUMBRA_SOURCE_ROOT and GLM/JSON usage requirements; "
        "the standalone mesher has only GoogleTest's usage requirements."),
    "src/luminumbra_common/world/FarVolumeTile.cpp": (
        frozenset({"far_volume_tile_test_support", "far_volume_facade_test"}),
        "Facade adds LUMINUMBRA_SOURCE_ROOT and GLM/JSON usage requirements; "
        "the shared standalone tile retains the GoogleTest-only policy."),
}


def test_targets(root=ROOT, cmake_text=None):
    return _PARSE(root, root / "test/CMakeLists.txt", strict_sources=True,
                  follow_includes=True, cmake_text=cmake_text)


def production(source):
    return source.parts[0] == "src" and source.suffix == ".cpp"


def source_owners(targets):
    owners = {}
    for name, target in sorted(targets.items()):
        for source in sorted(target["sources"]):
            if production(source):
                owners.setdefault(source.as_posix(), set()).add(name)
    return owners


def source_closure(targets, name):
    """Count distinct compilation owners through local static/object links."""
    seen = set()
    counts = Counter()

    def visit(target_name):
        if target_name in seen or target_name not in targets:
            return
        seen.add(target_name)
        target = targets[target_name]
        counts.update(source for source in target["sources"] if production(source))
        for link in sorted(target["links"] | target.get("objects", set())):
            visit(link)

    visit(name)
    return counts


def violations(targets, allowance):
    owners = source_owners(targets)
    errors = []
    for source, names in sorted(owners.items()):
        if len(names) < 2:
            continue
        allowed, reason = allowance.get(source, (frozenset(), ""))
        if not reason.strip() or not names <= allowed:
            errors.append(f"{source}: repeated in {', '.join(sorted(names))}")
        elif names != allowed:
            errors.append(f"{source}: shrink allowance to current owners")
    for source, (allowed, reason) in sorted(allowance.items()):
        if len(owners.get(source, ())) < 2:
            errors.append(f"{source}: remove obsolete allowance")
        if not reason.strip() or len(allowed) < 2:
            errors.append(f"{source}: allowance needs owners and a reason")
    for name in sorted(targets):
        for source, count in sorted(source_closure(targets, name).items()):
            if count > 1:
                errors.append(f"{name}: {source} has {count} linked compilation owners")
    return errors


class SharedTestSourcesTests(unittest.TestCase):
    def test_repository(self):
        self.assertEqual(violations(test_targets(), ALLOWLIST), [])

    def fixture(self, body, files=None):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name).resolve()
        files = {"src/leaf.cpp": "", "test/CMakeLists.txt": body, **(files or {})}
        for name, content in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        return root

    def test_empty_and_single_owner_are_valid(self):
        for body in ("", "add_executable(one ../src/leaf.cpp)"):
            with self.subTest(body=body):
                self.assertEqual(violations(test_targets(self.fixture(body)), {}), [])

    def test_includes_variables_lists_subdirectories_and_platform_union(self):
        root = self.fixture('''
set(leaf "${CMAKE_SOURCE_DIR}/src/leaf.cpp")
include(${CMAKE_CURRENT_LIST_DIR}/lists/owners.cmake)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/modules")
include(extra)
if(WIN32)
  add_subdirectory(platform)
endif()
''', {
            "test/lists/owners.cmake": '''
set(sources)
list(APPEND sources "${leaf}")
add_executable(first)
target_sources(first PRIVATE ${sources})
# add_executable(commented ${leaf})
message("add_executable(quoted ${leaf})")
''',
            "test/platform/CMakeLists.txt": '''
foreach(name IN ITEMS second third)
  add_executable(${name} ../../src/leaf.cpp)
endforeach()
''',
            "test/modules/extra.cmake": 'message("module without source owners")',
        })
        targets = test_targets(root)
        self.assertEqual(source_owners(targets), {"src/leaf.cpp": {"first", "second", "third"}})
        self.assertTrue(violations(targets, {}))
        allowance = {"src/leaf.cpp": (frozenset({"first", "second", "third"}), "platform policy")}
        self.assertEqual(violations(targets, allowance), [])
        reduced = {name: target for name, target in targets.items() if name != "third"}
        self.assertTrue(any("shrink allowance" in error for error in violations(reduced, allowance)))
        targets["fourth"] = {"sources": {Path("src/leaf.cpp")}, "links": set()}
        self.assertTrue(violations(targets, allowance))

    def test_allowance_requires_reason_and_shrinks(self):
        root = self.fixture("add_executable(first ../src/leaf.cpp)\n"
                            "add_executable(second ../src/leaf.cpp)")
        targets = test_targets(root)
        allowed = frozenset({"first", "second"})
        self.assertTrue(violations(targets, {"src/leaf.cpp": (allowed, " ")}))
        self.assertEqual(violations(targets, {"src/leaf.cpp": (allowed, "distinct flags")}), [])
        del targets["second"]
        self.assertTrue(violations(targets, {"src/leaf.cpp": (allowed, "distinct flags")}))

    def test_duplicate_linked_copy_is_never_allowlisted(self):
        for link in ("target_link_libraries(app PRIVATE shared)",
                     "add_library(alias ALIAS shared)\ntarget_link_libraries(app PRIVATE alias)",
                     'target_sources(app PRIVATE "$<TARGET_OBJECTS:shared>")'):
            with self.subTest(link=link):
                root = self.fixture("add_library(shared OBJECT ../src/leaf.cpp)\n"
                                    "add_executable(app ../src/leaf.cpp)\n" + link)
                allowance = {"src/leaf.cpp": (frozenset({"app", "shared"}), "different flags")}
                self.assertTrue(any("linked compilation owners" in error for error in
                                    violations(test_targets(root), allowance)))

    def test_shared_diamond_counts_one_owner(self):
        root = self.fixture('''
add_library(shared STATIC ../src/leaf.cpp)
add_library(left INTERFACE)
add_library(right INTERFACE)
target_link_libraries(left INTERFACE shared)
target_link_libraries(right INTERFACE shared)
add_executable(app)
target_link_libraries(app PRIVATE left right)
''')
        self.assertEqual(violations(test_targets(root), {}), [])

    def test_malformed_missing_recursive_and_unresolved_inputs_fail_closed(self):
        for body in ('add_executable(', 'add_executable(one ${missing})',
                     'add_executable(one ../src/missing.cpp)',
                     'include(missing.cmake)', 'include(CMakeLists.cmake)',
                     'include(unknown_module)', 'add_library(alias ALIAS)',
                     'add_subdirectory(missing)'):
            with self.subTest(body=body):
                root = self.fixture(body, {"test/CMakeLists.cmake": 'include(CMakeLists.cmake)'})
                with self.assertRaises((ValueError, OSError)):
                    test_targets(root)

    def test_optional_missing_include_and_unreadable_input(self):
        root = self.fixture('include(missing.cmake OPTIONAL)')
        self.assertEqual(test_targets(root), {})
        from unittest.mock import patch
        with patch.object(Path, "read_text", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                test_targets(root)

    def test_optional_include_keeps_nested_failures(self):
        root = self.fixture('include(optional.cmake OPTIONAL)', {
            'test/optional.cmake': 'include(missing.cmake)',
        })
        with self.assertRaises(FileNotFoundError):
            test_targets(root)
        from unittest.mock import patch
        original = Path.read_text

        def read(path, *args, **kwargs):
            if path.name == 'optional.cmake':
                raise PermissionError('denied')
            return original(path, *args, **kwargs)

        with patch.object(Path, 'read_text', read):
            with self.assertRaises(PermissionError):
                test_targets(root)

    def test_subdirectory_scope_restores_parent_variables(self):
        root = self.fixture('''
set(leaf ../src/leaf.cpp)
add_subdirectory(child)
add_executable(parent ${leaf})
''', {
            'src/child.cpp': '',
            'test/child/CMakeLists.txt': '''
set(leaf ../../src/child.cpp)
add_executable(child ${leaf})
''',
        })
        self.assertEqual(source_owners(test_targets(root)), {
            'src/leaf.cpp': {'parent'}, 'src/child.cpp': {'child'},
        })


if __name__ == "__main__":
    if sys.argv[1:] == ["--scan"]:
        repeated = {source: names for source, names in source_owners(test_targets()).items()
                    if len(names) > 1}
        for source, names in sorted(repeated.items()):
            print(f"{source}: {', '.join(sorted(names))}")
        print(f"{len(repeated)} repeated production paths; "
              f"{sum(len(names) - 1 for names in repeated.values())} redundant entries")
    else:
        unittest.main()
