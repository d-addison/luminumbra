"""Guard replay fixture symbol ownership without claiming native link coverage."""

import ast
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = "test/persistence/host_scheduler_fault_test.cpp"
MODES = "src/luminumbra_server/modes/"
ENTRY_POINTS = {
    "RunRecord": "RunSchedulerFaultRecord",
    "RunReplay": "RunSchedulerFaultReplay",
    "RunMutateReplayFixture": "RunSchedulerFaultMutateReplayFixture",
    "RunServer": "RunSchedulerFaultServer",
}


def without_comments(source):
    # Keep strings intact so commented-out directives cannot satisfy the check.
    return re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',
                  lambda match: "" if match[0].startswith(("//", "/*")) else match[0],
                  source, flags=re.DOTALL)


def read_source(path):
    return without_comments((ROOT / path).read_text(encoding="utf-8"))


class ReplaySourceContract(unittest.TestCase):
    def assert_fixture_ownership(self, fixture):
        includes = list(re.finditer(
            r'^#include "../../src/luminumbra_server/modes/(Replay|Server)\.cpp"$',
            fixture, re.MULTILINE))
        self.assertEqual([match[1] for match in includes], ["Replay", "Server"])
        first, last = includes[0].start(), includes[-1].end()
        prefix, suffix = fixture[:first], fixture[last:]
        self.assertIn('#include "luminumbra_server/modes/Modes.h"', prefix)
        # Discover every external mode definition, including the fixture mutator
        # even though the fault cases do not call it. All need distinct symbols.
        exported = set()
        for mode in ("Replay", "Server"):
            exported.update(re.findall(
                r'^int (Run\w+)\(const ServerCliOptions& options\)\s*\{',
                read_source(MODES + mode + ".cpp"), re.MULTILINE))
        self.assertEqual(exported, set(ENTRY_POINTS))
        self.assertEqual(len(set(ENTRY_POINTS.values())), len(exported))
        substitutions = {"ServerWorldRunner": "SchedulerFaultTestRunner", **ENTRY_POINTS}
        for original, adapted in substitutions.items():
            define = f"#define {original} {adapted}"
            undef = f"#undef {original}"
            self.assertEqual(fixture.count(define + "\n"), 1)
            self.assertEqual(fixture.count(undef + "\n"), 1)
            self.assertIn(define + "\n", prefix)
            self.assertIn(undef + "\n", suffix)
            # No intervening condition, undef or replacement may leave one
            # include with shipping names, or leak a macro into the test cases.
            scope = fixture[prefix.index(define):suffix.index(undef) + last]
            directives = re.findall(r'^#(\w+).*$', scope, re.MULTILINE)
            self.assertTrue(all(word in ("define", "include", "undef")
                                for word in directives))
            self.assertNotIn(undef, fixture[:last])
            self.assertLess(suffix.index(undef), suffix.index("TEST_F("))
        for original in ENTRY_POINTS:
            self.assertNotRegex(suffix, rf'\b{original}\s*\(')
        self.assertEqual(len(re.findall(r'\bRunSchedulerFaultRecord\(options\)', suffix)), 3)
        self.assertEqual(len(re.findall(r'\bRunSchedulerFaultServer\(options\)', suffix)), 1)

    def test_fixture_compiles_real_modes_with_distinct_entry_points(self):
        self.assert_fixture_ownership(read_source(FIXTURE))

    def test_every_missing_or_ineffective_substitution_is_detected(self):
        fixture = read_source(FIXTURE)
        for original, adapted in ENTRY_POINTS.items():
            define = f"#define {original} {adapted}\n"
            undef = f"#undef {original}\n"
            for mutation, changed in enumerate((
                fixture.replace(define, ""),
                fixture.replace(define, f"#define {original} {original}\n"),
                fixture.replace(undef, ""),
                fixture.replace(define, "").replace(undef, define + undef),
            )):
                with self.subTest(symbol=original, mutation=mutation), self.assertRaises(AssertionError):
                    self.assert_fixture_ownership(changed)

    def test_fault_cases_cannot_silently_call_unadapted_modes(self):
        fixture = read_source(FIXTURE)
        for original in ("RunRecord", "RunServer"):
            with self.subTest(symbol=original), self.assertRaises(AssertionError):
                self.assert_fixture_ownership(fixture.replace(
                    ENTRY_POINTS[original] + "(options)", original + "(options)", 1))

    def assert_shipping_owner(self, cmake, identity):
        target = re.search(r'add_executable\(world_open_refusal_test\s+(.*?)\)',
                           cmake, re.DOTALL)
        self.assertIsNotNone(target)
        for source in ("persistence/host_scheduler_fault_test.cpp",
                       "common/EffectivePresetIdentity_test.cpp",
                       "../" + MODES + "Replay.cpp"):
            self.assertEqual(target[1].split().count(source), 1)
        # The identity roundtrip must keep using the unadapted production bodies.
        self.assertRegex(identity, r'ASSERT_EQ\(RunRecord\(options\), 0\)')
        self.assertRegex(identity, r'ASSERT_EQ\(RunReplay\(options\), 0\)')
        self.assertNotIn("#define Run", identity)
        shipping = read_source("src/luminumbra_server/sources.cmake")
        for mode in ("Replay", "Server"):
            self.assertEqual(shipping.count(f'/modes/{mode}.cpp"'), 1)

    def test_shipping_replay_and_identity_roundtrip_keep_their_owner(self):
        self.assert_shipping_owner(read_source("test/CMakeLists.txt"),
                                   read_source("test/common/EffectivePresetIdentity_test.cpp"))

    def test_removing_shipping_replay_coverage_is_detected(self):
        cmake = read_source("test/CMakeLists.txt")
        identity = read_source("test/common/EffectivePresetIdentity_test.cpp")
        with self.assertRaises(AssertionError):
            self.assert_shipping_owner(cmake.replace("../" + MODES + "Replay.cpp", ""), identity)
        with self.assertRaises(AssertionError):
            self.assert_shipping_owner(cmake, identity.replace("RunReplay(options)", "FakeReplay(options)"))

    def test_replay_admission_fixture_distinguishes_supported_and_unknown_versions(self):
        identity = read_source("test/common/EffectivePresetIdentity_test.cpp")
        refusal = identity.split(
            "ReplayRefusesInvalidOrMismatchedIdentityBeforeCreatingDirectories) {", 1)[1].split(
                "\nTEST_F(", 1)[0]
        self.assertIn("for (const bool custom : {false, true})", refusal)
        self.assertIn("constexpr auto unsupported_version = "
                      "(std::numeric_limits<std::uint16_t>::max)();", refusal)
        self.assertRegex(refusal, r'static_assert\(unsupported_version > '
                         r'Luminumbra::Replay::kLrec1Version &&\s*unsupported_version > '
                         r'Luminumbra::Replay::kLrec1ElidedVersion\);')
        self.assertIn("header.version = unsupported_version;\n"
                      "        EXPECT_FALSE(ReplayRunnerConfigFrom(options, header).has_value());",
                      refusal)
        self.assertNotIn("++header.version", refusal)
        self.assertRegex(refusal, r'for \(const auto version :\s*'
                         r'\{Luminumbra::Replay::kLrec1Version, '
                         r'Luminumbra::Replay::kLrec1ElidedVersion\}\) \{\s*'
                         r'SCOPED_TRACE\(version\);\s*header = valid;\s*'
                         r'header.version = version;\s*'
                         r'const auto admitted = ReplayRunnerConfigFrom\(options, header\);\s*'
                         r'ASSERT_TRUE\(admitted.has_value\(\)\);')
        self.assertIn("EXPECT_EQ(admitted->required_preset_content_hash, "
                      "resolved.preset_content_hash);", refusal)
        self.assertRegex(refusal, r'EXPECT_TRUE\(Luminumbra::Replay::ConfigurationMatches\(\s*'
                         r'header, retained.SystemConfigIdentity\(\), '
                         r'admitted->required_preset_content_hash\)\);')

    def test_contract_is_registered_in_ctest_and_both_python_platform_plans(self):
        path = "tools/ci/test_replay_source_contract.py"
        self.assertIn('add_test(NAME ReplaySourceContract COMMAND "${Python3_EXECUTABLE}" -I -B\n'
                      f'    ${{CMAKE_SOURCE_DIR}}/{path})', read_source("test/CMakeLists.txt"))
        plan = ast.parse(read_source("tools/ci/ci_paths.py"))
        tests = next(ast.literal_eval(node.value) for node in plan.body
                     if isinstance(node, ast.Assign)
                     and any(isinstance(name, ast.Name) and name.id == "PYTHON_TESTS"
                             for name in node.targets))
        self.assertEqual(tests.count(path), 1)
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("tools/ci/ci_paths.py python-tests", workflow)

    def test_surface_admission_precedes_boot_output_and_allocation(self):
        helper = read_source(MODES + "ModeHelpers.cpp").split("ReplayRunnerConfigFrom(", 1)[1]
        guard = "SurfaceRadiiSupported(header.surface_radius, header.collision_radius)"
        self.assertIn(guard, helper)
        self.assertRegex(helper, r'if \(!Luminumbra::World::SurfaceRadiiSupported\('
                         r'header.surface_radius, header.collision_radius\)\)\s*return std::nullopt;')
        self.assertLess(helper.index(guard), helper.index("ServerWorldRunnerConfig config;"))
        runner = read_source("src/luminumbra_server/ServerWorldRunner.cpp").split(
            "bool ServerWorldRunner::Boot()", 1)[1]
        self.assertRegex(runner, r'if \(!World::SurfaceRadiiSupported\('
                         r'm_config.surface_radius, m_config.collision_radius\)\)\s*\{'
                         r'[^}]*return false;')
        self.assertLess(runner.index("SurfaceRadiiSupported"), runner.index("m_jobSystem.startup"))
        stream = read_source("src/luminumbra_common/replay/ReplayStream.cpp")
        decode = stream.split("if (!c.GetU32(h.collision_radius))", 1)[1]
        self.assertRegex(decode, r'SurfaceRadiiSupported\(h.surface_radius, h.collision_radius\)\)'
                         r'\s*return false;')
        self.assertLess(decode.index("SurfaceRadiiSupported"), decode.index("c.GetString"))
        writer = stream.split("bool ReplayWriter::Open(", 1)[1]
        self.assertLess(writer.index("SurfaceRadiiSupported"), writer.index("m_path = path"))
        self.assertRegex(writer, r'SurfaceRadiiSupported\(header.surface_radius, '
                         r'header.collision_radius\)\)\s*return false;')
        primitive = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp").split(
            "bool SHIELD_WorldSystem::EnsureSurfaceReadyNear(", 1)[1]
        self.assertIn("World::SurfaceColumnCount(static_cast<std::uint64_t>(radius))", primitive)
        self.assertRegex(primitive, r'if \(!physics_system \|\| !column_count \|\|\s*'
                         r'!World::SurfaceRadiiSupported\(radius, collision_range\)\)\s*\{'
                         r'\s*return false;')
        self.assertLess(primitive.index("SurfaceRadiiSupported"),
                        primitive.index("wait_for_generation_jobs"))
        self.assertRegex(primitive, r'if \(!World::AddSurfaceColumnChunks\('
                         r'span.min_y, span.max_y, surface_capacity\)\)\s*return false;')
        self.assertLess(primitive.index("AddSurfaceColumnChunks"),
                        primitive.index("chunks_to_build.reserve(surface_capacity)"))
        self.assertLess(primitive.index("AddSurfaceColumnChunks"),
                        primitive.index("m_streaming_state.chunks.emplace"))
        self.assertIn("const ColumnSurfaceSpan& span = surface_spans[surface_index++];", primitive)
        self.assertNotIn("(radius * 2 + 1) * (radius * 2 + 1)", primitive)
        for vector in ("chunks_to_build", "chunks_to_consider_for_collision"):
            self.assertIn(f"surface_capacity > {vector}.max_size()", primitive)
        edit = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp").split(
            "int SHIELD_WorldSystem::EditTerrainVoxel(", 1)[1]
        self.assertIn("(World::kMaxSurfaceRadius - 2) * CHUNK_SIZE_X", edit)
        self.assertRegex(edit, r'if \(!std::isfinite\(radius_m\) \|\| radius_m <= 0.0f \|\| '
                         r'radius_m > max_edit_radius\)\s*return 0;')
        self.assertLess(edit.index("std::isfinite(radius_m)"), edit.index("world_to_chunk_coords"))
        self.assertRegex(edit, r'if \(!World::AddSurfaceColumnChunks\('
                         r'span.min_y, span.max_y, remesh_chunks\)\)\s*return 0;')
        self.assertLess(edit.index("AddSurfaceColumnChunks"), edit.index("chunk->sdf_data[idx] ="))
        budget = read_source("src/luminumbra_common/world/SurfaceWorkBudget.h")
        for check in ("kMaxSurfaceRadius = 12", "kMaxSurfaceColumns = 625",
                      "kMaxSurfaceChunks = 8192", "radius > (max_size - 1) / 2",
                      "diameter > max_size / diameter",
                      "count > kMaxSurfaceChunks - total"):
            self.assertIn(check, budget)

    def test_surface_cli_and_direct_siblings_share_admission_and_native_owners(self):
        server = read_source("src/luminumbra_server/main_server.cpp")
        for flag, member in (("--radius", "surface_radius"),
                             ("--collision-radius", "collision_radius")):
            branch = server.split(f'std::strcmp(arg, "{flag}") == 0', 1)[1].split(
                "} else if", 1)[0]
            self.assertIn(f"TryParseSurfaceRadius(v, options.{member})", branch)
            self.assertNotIn("atoi", branch)
        client = read_source("src/luminumbra_client/core/RuntimeScenarioConfig.cpp")
        self.assertIn('std::string_view("--horizon-radius")', client)
        self.assertIn('std::string_view("--collision-radius")', client)
        self.assertIn("World::TryParseSurfaceRadius(value, radius)", client)
        self.assertIn("throw std::invalid_argument", client)
        paths = client.split("RuntimePathOptions ParseRuntimePathOptions(", 1)[1].split(
            "RuntimeScenarioConfig\nParseRuntimeScenarioConfig", 1)[0]
        self.assertIn("ParseSurfaceRadiusOptions(", paths)
        self.assertLess(paths.index("ParseSurfaceRadiusOptions("),
                        paths.index("std::filesystem::create_directories"))
        main = read_source("src/luminumbra_client/main_client.cpp")
        self.assertIn("RuntimeScenarioConfig scenario_config;", main)
        main = main.split(
            "RuntimeScenarioConfig scenario_config;", 1)[1].split("std::string override_error", 1)[0]
        self.assertRegex(main, r'try\s*\{\s*scenario_config = ParseRuntimeScenarioConfig\('
                         r'argc, argv, root_dir\);\s*\} catch '
                         r'\(const std::invalid_argument& error\)\s*\{[\s\S]*return 2;')
        for filename, function, first_write in (
            ("NetworkedSessionDriver", "bool NetworkedSessionDriver::Begin(",
             "m_impl->config = config"),
            ("PersistenceRoundtrip", "RunPersistenceRoundtripSavePhase(",
             "CarveSphereIntoChunk(*chunk"),
        ):
            body = read_source("src/luminumbra_client/core/scenarios/" + filename + ".cpp").split(
                function, 1)[1]
            self.assertIn("World::SurfaceRadiiSupported(", body)
            self.assertLess(body.index("SurfaceRadiiSupported"), body.index(first_write))
        cmake = read_source("test/CMakeLists.txt")
        target = re.search(r'add_executable\(world_open_refusal_test\s+(.*?)\)',
                           cmake, re.DOTALL)
        self.assertIsNotNone(target)
        self.assertEqual(target[1].split().count("common/SurfaceWorkBudget_test.cpp"), 1)
        native = read_source("test/common/SurfaceWorkBudget_test.cpp")
        for name in ("CheckedColumnsAndExactVerticalBudget",
                     "DecimalAdmissionHasNoOverflowOrPartialFallback",
                     "HeaderDecodeAndWriterRefuseBothRadiiBeforeOutput",
                     "RefusedWriterReopenRetainsQueuedRecordingAndPath",
                     "RunnerAndRecordingRefuseBeforeSessionOrFiles",
                     "DirectSurfaceAndCollisionRefuseBeforeChunkMutation",
                     "TallSurfaceRefusesExactWorkBeforeCreatingChunks"):
            self.assertRegex(native, rf'TEST(?:_F)?\(\w+, {name}\)')
        self.assertIn("stream.seekp(offset)", native)
        self.assertIn("RunReplay(options)", native)
        self.assertIn("RunRecord(options)", native)

    def test_preview_refusal_preserves_live_ownership_and_releases_worker(self):
        source = read_source("src/luminumbra_client/world/WorldgenPreview.cpp")
        build = source.split("void WorldgenPreview::build_world_pending()", 1)[1].split(
            "void WorldgenPreview::swap_pending_into_live(", 1)[0]
        self.assertRegex(build, r'if \(!m_pending_world->EnsureSurfaceReadyNear\('
                         r'[\s\S]*?\)\)\s*\{\s*m_pending_water.reset\(\);\s*'
                         r'm_pending_world.reset\(\);\s*return;\s*\}')
        self.assertLess(build.index("return;"), build.index("m_pending_world->update("))
        swap = source.split("void WorldgenPreview::swap_pending_into_live(", 1)[1].split(
            "bool WorldgenPreview::tick(", 1)[0]
        self.assertRegex(swap, r'if \(m_pending_world\)\s*\{\s*'
                         r'pipeline.prepare_world_swap\(\);\s*m_water.reset\(\);')
        success, failure = swap.split("} else {", 1)
        self.assertIn("++m_rebuild_generation", success)
        self.assertRegex(failure, r'm_last_build_failed = true;[\s\S]*'
                         r'm_first_build_requested = true;\s*\}')
        for forbidden in ("m_world =", "m_water.reset", "m_registry =",
                          "++m_rebuild_generation", "prepare_world_swap"):
            self.assertNotIn(forbidden, failure)
        self.assertIn("m_build_done.store(false);", failure)
        self.assertIn("m_build_inflight.store(m_build_pending.load());", failure)
        self.assertIn("m_build_cv.notify_one();", failure)
        native = read_source("test/ui/worldgen_preview_test.cpp")
        for name in ("RefusedFirstSurfaceKeepsWorldEmptyAndAcceptsNextCandidate",
                     "RefusedSurfaceRetainsLiveWorldAndAllowsRecovery"):
            self.assertIn(f"TEST(WorldgenPreviewTest, {name})", native)
        cmake = read_source("test/CMakeLists.txt")
        self.assertRegex(cmake, r'add_executable\(worldgen_preview_test\s+'
                         r'ui/worldgen_preview_test.cpp\s*\)')
        for targets in ("LUMINUMBRA_GTEST_TARGETS", "LUMINUMBRA_GL_GTEST_TARGETS"):
            registered = re.search(rf'^set\({targets}\s+(.*?)^\)', cmake,
                                   re.DOTALL | re.MULTILINE)
            self.assertIsNotNone(registered)
            self.assertEqual(registered[1].split().count("worldgen_preview_test"), 1)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

    def test_surface_consumers_stop_before_capture_save_or_world_entry(self):
        persistence = read_source(
            "src/luminumbra_client/core/scenarios/PersistenceRoundtrip.cpp").split(
                "RunPersistenceRoundtripSavePhase(", 1)[1].split(
                    "RunPersistenceRoundtripLoadPhase(", 1)[0]
        self.assertEqual(persistence.count("if (!world_system->EnsureSurfaceReadyNear("), 2)
        self.assertLess(persistence.index("if (!world_system->EnsureSurfaceReadyNear("),
                        persistence.index("CarveSphereIntoChunk(*chunk"))
        self.assertEqual(len(re.findall(
            r'if \(!world_system->EnsureSurfaceReadyNear\([\s\S]*?\)\)\s*\{\s*'
            r'result.failure_reason = "surface_not_ready";\s*return result;', persistence)), 2)
        drive = read_source("src/luminumbra_client/core/ScenarioRunnerDrive.cpp")
        self.assertEqual(drive.count("EnsureSurfaceReadyNear("), 3)
        self.assertEqual(len(re.findall(
            r'if \(![^\n]+->EnsureSurfaceReadyNear\([\s\S]*?\)\)\s*\{\s*'
            r'scenario_failed = true;\s*'
            r'scenario_failure_reason = "surface_preparation_refused";\s*'
            r'glfwSetWindowShouldClose\(window, true\);\s*return true;', drive)), 3)
        self.assertIn("return scenario_failed ? InGameDrive::kBreakCase : InGameDrive::kHandled;",
                      drive)
        survey = read_source("src/luminumbra_client/rendering/SceneSurvey.cpp")
        self.assertRegex(survey, r'if \(!w->EnsureSurfaceReadyNear\(anchor, phys, 12, 4\)\)'
                         r'\s*\{\s*LUMINUMBRA_CORE_ERROR\([^;]*;\s*return false;')
        self.assertIn("bool RunSceneSurvey(", survey)
        self.assertIn("bool RunSceneSurvey(", read_source(
            "src/luminumbra_client/rendering/SceneSurvey.h"))
        self.assertLess(survey.index("if (!w->EnsureSurfaceReadyNear("),
                        survey.index("const FrameScanReport rep = ScanFrame"))
        main = read_source("src/luminumbra_client/main_client.cpp")
        self.assertRegex(main, r'if \(!Luminumbra::Rendering::RunSceneSurvey\('
                         r'[^;]*\)\)\s*\{\s*exit_code = 2;')
        entry = main.split("auto finish_world_entry =", 1)[1].split(
            "auto start_world_creation =", 1)[0]
        self.assertRegex(entry, r'if \(!horizon_ready\)\s*\{\s*refuse_world_entry\('
                         r'[^;]*;\s*return;')
        self.assertLess(entry.index("if (!horizon_ready)"), entry.index("g_camera ="))
        self.assertRegex(main, r'if \(!gameSession->GetWorldSystem\(\)->EnsureSurfaceReadyNear\('
                         r'[^;]*, 4, 2\)\)\s*\{[^}]*return 2;')
        horizon = main.split("auto prepare_entry_horizon =", 1)[1].split(
            "auto drain_entry =", 1)[0]
        self.assertRegex(horizon, r'return gameSession->GetWorldSystem\(\)->'
                         r'EnsureSurfaceReadyNear\([^;]*, 12, 4\);')
        staged = main.split("case WorldEntryStage::PrepareHorizon:", 1)[1].split(
            "case WorldEntryStage::Activate:", 1)[0]
        self.assertRegex(staged, r'if \(!prepare_entry_horizon\(\)\)\s*\{\s*'
                         r'refuse_world_entry\([^;]*;\s*return WorldEntryResult::Failed;')
        loading = main.split("case GameState::WORLD_LOADING: {", 1)[1].split(
            "case GameState::IN_GAME:", 1)[0]
        self.assertRegex(loading, r'if \(!prepare_entry_horizon\(\)\)\s*\{[\s\S]*'
                         r'refuse_world_entry\([^;]*;\s*break;')
        self.assertIn('g_uiManager->RequestLoadDocument("main_menu.rml");', loading)
        self.assertLess(loading.index("if (!prepare_entry_horizon())"),
                        loading.index("activate_loaded_world();"))
        backdrop = main.split('CreateTransientWorld("Menu Vista"', 1)[1].split(
            "int exit_code = 0;", 1)[0]
        self.assertRegex(backdrop, r'if \(auto\* ws = gameSession->GetWorldSystem\(\);\s*'
                         r'ws && gameSession->GetPhysicsSystem\(\) &&\s*'
                         r'ws->EnsureSurfaceReadyNear\(')
        self.assertIn('refuse_world_entry("Menu surface preparation failed.", false);', backdrop)

    def assert_runtime_surface_owner(self, cmake, native):
        target = "runtime_surface_options_test"
        sources = re.search(rf'add_executable\({target}\s+(.*?)\)', cmake, re.DOTALL)
        self.assertIsNotNone(sources)
        self.assertEqual(sources[1].split(), [
            "client/runtime_surface_options_test.cpp",
            "../src/luminumbra_client/core/RuntimeScenarioConfig.cpp",
        ])
        for command, dependencies in (
            ("target_include_directories", ("../src", "../src/luminumbra_client")),
            ("target_link_libraries", ("gtest_main", "luminumbra_common", "nlohmann_json", "glm")),
        ):
            block = re.search(rf'{command}\({target}\s+PRIVATE\s+(.*?)\)', cmake, re.DOTALL)
            self.assertIsNotNone(block)
            for dependency in dependencies:
                self.assertEqual(block[1].split().count(dependency), 1)
        registered = re.search(r'^set\(LUMINUMBRA_GTEST_TARGETS\s+(.*?)^\)', cmake,
                               re.DOTALL | re.MULTILINE)
        self.assertIsNotNone(registered)
        self.assertEqual(registered[1].split().count(target), 1)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)
        self.assertIn('#include "core/RuntimeScenarioConfig.h"', native)
        self.assertIn('#include "../support/UniqueTempPath.h"', native)
        self.assertIn("using Luminumbra::Client::ScenarioHarness::ParseRuntimeScenarioConfig;",
                      native)
        self.assertRegex(native, r'return ParseRuntimeScenarioConfig\('
                         r'static_cast<int>\(argv.size\(\)\), argv.data\(\), root\);')
        self.assertRegex(native, r'return Luminumbra::Client::ScenarioHarness::'
                         r'ParseRuntimePathOptions\(\s*static_cast<int>\(argv.size\(\)\), '
                         r'argv.data\(\)\);')
        self.assertIn("CreateUniqueTempDirectory(", native)
        self.assertIn("fs::remove_all(path, error);", native)
        for name in ("RefusesBeforeCreatingSaveRootInEitherArgumentOrder",
                     "RefusesInvalidRadiiAndAcceptsBoundariesInBothSpellings"):
            self.assertEqual(native.count(f"TEST(RuntimeSurfaceOptionsTest, {name})"), 1)
        for check in ("for (const bool radius_first : {false, true})",
                      "for (const bool equals : {false, true})",
                      "EXPECT_THROW(ParsePaths(arguments), std::invalid_argument);",
                      "EXPECT_FALSE(fs::exists(save_root));",
                      "EXPECT_TRUE(fs::is_directory(save_root));",
                      'EXPECT_EQ(Parse({"client"}).horizon_radius, 12);',
                      'EXPECT_EQ(Parse({"client"}).collision_radius, 4);'):
            self.assertIn(check, native)

    def test_runtime_surface_cases_use_registered_shipping_parser_target(self):
        self.assert_runtime_surface_owner(read_source("test/CMakeLists.txt"),
                                          read_source("test/client/runtime_surface_options_test.cpp"))

    def test_runtime_surface_owner_rejects_missing_registration_and_fake_helpers(self):
        cmake = read_source("test/CMakeLists.txt")
        native = read_source("test/client/runtime_surface_options_test.cpp")
        for token in ("client/runtime_surface_options_test.cpp",
                      "../src/luminumbra_client/core/RuntimeScenarioConfig.cpp",
                      "    runtime_surface_options_test\n"):
            # Mutate the new target itself, even when another target owns the
            # same production source. The target must retain every dependency.
            start = cmake.index("add_executable(runtime_surface_options_test")
            changed = cmake[:start] + cmake[start:].replace(token, "", 1)
            with self.subTest(token=token), self.assertRaises(AssertionError):
                self.assert_runtime_surface_owner(changed, native)
        for original in ("return ParseRuntimeScenarioConfig(",
                         "ParseRuntimePathOptions(",
                         "EXPECT_FALSE(fs::exists(save_root));"):
            with self.subTest(helper=original), self.assertRaises(AssertionError):
                self.assert_runtime_surface_owner(cmake, native.replace(original, "", 1))


if __name__ == "__main__":
    unittest.main()
