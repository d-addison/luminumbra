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

    def test_replay_calendar_admission_and_transient_snapshots_share_creation_values(self):
        helper = read_source(MODES + "ModeHelpers.cpp")
        for body in (helper.split("RuntimeHelloFrom(", 1)[1].split("\nLuminumbra::Server::ServerWorldRunnerConfig RunnerConfigFrom(", 1)[0],
                     helper.split("ReplayRunnerConfigFrom(", 1)[1]):
            # A session that has not created a world still has legacy presentation
            # defaults when active regions are off; admission needs creation values.
            self.assertRegex(body, r'SystemConfigIdentity\(Luminumbra::world::WorldCalendar\{\}\)')
        creation = read_source("src/luminumbra_common/world/GameSession.cpp").split(
            "bool GameSession::CreateWorldInternal(", 1)[1].split(
                "bool GameSession::LoadWorld(", 1)[0]
        self.assertIn("std::string saved_preset_json;", creation)
        self.assertRegex(creation, r'BoundedJsonDump\(\s*resolved,\s*'
                         r'Persistence::SavedInputLimits::kPresetBytes - 1,\s*saved_preset_json,\s*-1,\s*false\)')
        self.assertLess(creation.index("BoundedJsonDump("),
                        creation.index("fs::create_directories(worldPath.parent_path())"))
        self.assertLess(creation.index("requiredPresetContentHash"),
                        creation.index("fs::create_directories(worldPath.parent_path())"))
        self.assertLess(creation.index("LoadTerrainPresetFromJson(resolved"),
                        creation.index("ResetWorldSystems();"))
        self.assertIn("m_savedPresetJson = std::move(saved_preset_json);", creation)
        native = read_source("test/common/EffectivePresetIdentity_test.cpp")
        for name in ("ReplayAdmissionUsesTheCreatedCalendarWithActiveRegionsOnAndOff",
                     "TransientSnapshotsRetainTheEffectivePresetAndRenderCalendar"):
            self.assertIn(f"TEST_F(EffectivePresetIdentity, {name})", native)

    def test_voxel_edit_preparation_indexes_and_publishes_synchronous_generation(self):
        source = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        prepare = source.split("bool SHIELD_WorldSystem::PrepareTerrainVoxelEdit(", 1)[1].split(
            "int SHIELD_WorldSystem::EditTerrainVoxel(", 1)[0]
        insertion = prepare.split("if (iterator == m_streaming_state.chunks.end()) {", 1)[1].split(
            "auto& chunk = iterator->second;", 1)[0]
        self.assertIn("m_activation_index.insert(iterator->second);", insertion)
        self.assertIn("++m_dirty_generation;", insertion)
        generation = prepare.split("if (chunk->sdf_data.empty()) {", 1)[1].split(
            "if (chunk->sdf_data.size() != kFullSdfLattice)", 1)[0]
        self.assertRegex(generation, r'GenerateChunkData\(\*chunk, 1\);\s*'
                         r'publish_generated_column_span\(\*chunk\);\s*'
                         r'\+\+m_dirty_generation;')
        self.assertLess(prepare.index("return false;"), prepare.index("wait_for_streaming_jobs();"))
        native = read_source("test/common/ActivationResidencyIndex_test.cpp")
        self.assertIn("VoxelEditPreparationMaintainsResidencyAndPublication", native)
        for token in ("EXPECT_FALSE(world.PrepareTerrainVoxelEdit(anchor, 0.0f));",
                      "EXPECT_FALSE(world.PrepareTerrainVoxelEdit(anchor, 17.0f));",
                      "EXPECT_EQ(loaded->sdf_data, partial_sdf);",
                      "EXPECT_EQ(prepared.index_updates, initial.index_updates + 2u);",
                      "const auto missing = FullScan(world, {anchor}, 1);",
                      "ASSERT_EQ(missing.size(), 14u);",
                      "EXPECT_TRUE(FullScan(world, {anchor}, 1).empty());",
                      "world.get_last_streaming_budget_stats().generation_candidates, missing.size()"):
            self.assertIn(token, native)
        self.assertIn("common/ActivationResidencyIndex_test.cpp", read_source("test/CMakeLists.txt"))
        self.assertNotRegex(native, r'(?<![\w:])Test::SyntheticTerrainParams\(')

    def test_surface_span_cache_preserves_revision_and_owner_publication(self):
        source = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        sampler = source.split("SHIELD_WorldSystem::compute_column_surface_span(", 1)[1].split(
            "SHIELD_WorldSystem::ColumnSurfaceSpan SHIELD_WorldSystem::column_surface_span_from_heights(", 1)[0]
        self.assertRegex(sampler, r'ComputeShapedHeightsAtPositionsImpl<true>\('
                         r'[^;]*m_params.surface_sampling_revision >=\s*kSurfaceSamplingRevision\);')
        self.assertIn("return column_surface_span_from_heights(chunk_x, chunk_z, heights);", sampler)
        publication = source.split("void SHIELD_WorldSystem::publish_generated_column_span(", 1)[1].split(
            "SHIELD_WorldSystem::ColumnSurfaceSpan SHIELD_WorldSystem::column_surface_span(", 1)[0]
        self.assertRegex(publication, r'if \(m_params.surface_sampling_revision < kSurfaceSamplingRevision &&\s*'
                         r'\(m_params.biome_relief_enabled \|\| m_params.cliffs_enabled \|\| m_params.lakes_enabled \|\|\s*'
                         r'm_params.surface_breaks_enabled\)\)\s*return;')
        adoption = source.split("bool SHIELD_WorldSystem::adopt_streamed_chunk(", 1)[1]
        for token in ("chunk->stage_water_seed_inputs({});", "m_activation_index.insert(chunk);",
                      "m_activation_index.content_changed(chunk->get_coords());"):
            self.assertIn(token, adoption)
        native = read_source("test/common/ActivationResidencyIndex_test.cpp")
        for token in ("params.surface_sampling_revision < Systems::kSurfaceSamplingRevision",
                      "legacy_params.cliffs_enabled = false;",
                      "GeneratedCliffHeightmapsPreserveFullScanActivationPolicy",
                      "CurrentRevisionPublishesCompleteGeneratedCliffSpans"):
            self.assertIn(token, native)

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
            "bool SHIELD_WorldSystem::EnsureSurfaceReadyNear(", 1)[1].split(
                "\nfloat SHIELD_WorldSystem::GetTerrainHeightAt(", 1)[0]
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

    def test_surface_exact_admission_precedes_all_publication_drains(self):
        source = read_source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        primitive = source.split("bool SHIELD_WorldSystem::EnsureSurfaceReadyNear(", 1)[1].split(
            "\nfloat SHIELD_WorldSystem::GetTerrainHeightAt(", 1)[0]
        # The real function must finish every admission check before any drain;
        # searching the rest of the translation unit could accept a later call.
        admissions = [re.search(
            r'if \(!World::AddSurfaceColumnChunks\('
            r'span.min_y, span.max_y, surface_capacity\)\)\s*return false;', primitive)]
        for vector in ("chunks_to_build", "chunks_to_consider_for_collision"):
            admissions.append(re.search(
                rf'if \(surface_capacity > {vector}\.max_size\(\)\)\s*return false;',
                primitive))
        for admission in admissions:
            self.assertIsNotNone(admission)
        drains = list(re.finditer(r'wait_for_(?:generation|meshing|promotion|streaming)_jobs\(\);',
                                  primitive))
        self.assertEqual([drain[0] for drain in drains], [
            "wait_for_generation_jobs();", "wait_for_meshing_jobs();",
            "wait_for_streaming_jobs();"])
        # The full streaming barrier also settles deferred work. Pin both the
        # direct lane drains and the finite deferred loop, not just its name.
        admitted = source.split(
            "void SHIELD_WorldSystem::wait_for_admitted_streaming_jobs() {", 1)[1].split(
                "\n}", 1)[0]
        self.assertEqual(re.findall(r'wait_for_\w+_jobs\(\);', admitted), [
            "wait_for_generation_jobs();", "wait_for_hydro_prefetch_jobs();",
            "wait_for_meshing_jobs();", "wait_for_promotion_jobs();",
            "wait_for_meshing_jobs();"])
        streaming = source.split(
            "void SHIELD_WorldSystem::wait_for_streaming_jobs() {", 1)[1].split(
                "\n}", 1)[0]
        self.assertRegex(streaming, r'wait_for_admitted_streaming_jobs\(\);\s*'
                         r'(?:/[^\n]*\n\s*)*while \(deferred_meshing_pending\(\)\) \{\s*'
                         r'dispatch_deferred_meshing\(\);\s*wait_for_admitted_streaming_jobs\(\);\s*\}')
        for admission in admissions:
            self.assertLess(admission.end(), drains[0].start())
        self.assertLess(primitive.index("JobWatchdogEnabled()"), drains[0].start())
        for vector in ("chunks_to_build", "chunks_to_consider_for_collision"):
            self.assertLess(drains[-1].end(), primitive.index(f"{vector}.reserve(surface_capacity)"))
        self.assertLess(drains[-1].end(), primitive.index("m_streaming_state.chunks.emplace"))

        native = read_source("test/common/SurfaceWorkBudget_test.cpp")
        self.assertIn('#include "luminumbra_common/core/JobSystem.h"', native)
        for lane in ("Mesh", "Generation", "Promotion"):
            name = f"TallSurfaceRefusalRetainsUnpublished{lane}Batch"
            marker = f"TEST(SurfaceWorkBudget, {name})"
            self.assertEqual(native.count(marker), 1)
            case = native.split(marker, 1)[1].split("\nTEST(", 1)[0]
            for token in ("jobs.startup(1);", "world.begin_tick(0);",
                          "world.quiesce_streaming_jobs_for_save();",
                          "ASSERT_EQ(world.pending_publication_chunks(), 1u);",
                          "EXPECT_FALSE(world.EnsureSurfaceReadyNear(anchor, &physics, 0, 0));",
                          "EXPECT_FALSE(world.EnsureCollisionReadyNear(anchor, &physics, 0));",
                          "EXPECT_EQ(world.EditTerrainVoxel(anchor, 1.0f, false, &physics), 0);",
                          "EXPECT_EQ(world.pending_publication_chunks(), 1u);",
                          "EXPECT_EQ(chunk->current_lod.load(), original_lod);",
                          "EXPECT_EQ(chunk->mesh_version.load(), original_version);",
                          "EXPECT_EQ(chunk->sdf_data, original_sdf);",
                          "EXPECT_EQ(world.find_streamed_chunk(Luminumbra::IVec3(0)), chunk);",
                          "EXPECT_EQ(world.snapshot_streamed_chunks().size(), 1u);",
                          "EXPECT_EQ(world.far_lod_authority_revision(), original_revision);",
                          "world.begin_tick(8);", "world.activate_due(8);",
                          "EXPECT_EQ(world.pending_publication_chunks(), 0u);"):
                self.assertIn(token, case)
            self.assertLess(case.index("world.quiesce_streaming_jobs_for_save();"),
                            case.index("EXPECT_FALSE(world.EnsureSurfaceReadyNear("))
            if lane == "Generation":
                self.assertIn("world.dispatch_generation_jobs(", case)
                self.assertIn("ASSERT_TRUE(chunk->pending_generation_ready.load());", case)
            else:
                self.assertIn("world.update(registry, anchor, nullptr);", case)
                self.assertIn("ASSERT_TRUE(world.adopt_streamed_chunk(chunk));", case)
                ready = "mesh" if lane == "Mesh" else "promotion"
                self.assertIn(f"ASSERT_TRUE(chunk->pending_{ready}_ready.load());", case)
                self.assertIn("EXPECT_EQ(chunk->mesh_version.load(), original_version + 1u);", case)
        cmake = read_source("test/CMakeLists.txt")
        target = re.search(r'add_executable\(world_open_refusal_test\s+(.*?)\)',
                           cmake, re.DOTALL)
        self.assertIsNotNone(target)
        self.assertEqual(target[1].split().count("common/SurfaceWorkBudget_test.cpp"), 1)
        targets = cmake.split("set(LUMINUMBRA_GTEST_TARGETS", 1)[1].split("\n)", 1)[0]
        self.assertEqual(targets.split().count("world_open_refusal_test"), 1)
        self.assertIn("foreach(_lum_gtest_target ${LUMINUMBRA_GTEST_TARGETS})", cmake)
        self.assertIn("gtest_discover_tests(${_lum_gtest_target}", cmake)

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
                         r'EnsureSurfaceReadyNear\([^;]*,\s*'
                         r'capture_horizon \? scenario_config.horizon_radius : 12,\s*'
                         r'capture_horizon \? scenario_config.collision_radius : 4\);')
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


class SessionUsabilitySourceContract(unittest.TestCase):
    @staticmethod
    def function(source, name, prefix="GameSession::"):
        signature = re.escape(prefix).replace(r"\ ", r"\s+") + re.escape(name) + r"\("
        match = re.search(signature, source)
        if match is None:
            raise AssertionError("missing function " + prefix + name)
        start = match.start()
        cursor = match.end()
        parameter_depth = 1
        while parameter_depth:
            if source[cursor] == "(":
                parameter_depth += 1
            elif source[cursor] == ")":
                parameter_depth -= 1
            cursor += 1
        opening = source.index("{", cursor)
        # Ignore braces inside diagnostics and comments when locating the body.
        tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\{|\}', source[opening:])
        depth = 0
        for token in tokens:
            if token[0] == "{":
                depth += 1
            elif token[0] == "}":
                depth -= 1
                if depth == 0:
                    return source[start:opening + token.end()]
        raise AssertionError("unterminated session function")

    def test_every_plant_reader_admits_and_reads_the_same_regular_file_handle(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        reader = self.function(source, "load_plant_entities", "WorldSaveService::")
        self.assertIn("const SavedInputFile input(path);", reader)
        self.assertNotIn("std::filesystem::exists(", reader)
        self.assertNotIn("ifstream", reader)
        self.assertLess(reader.index("input.read(bytes, SavedInputLimits::kPlantFileBytes, stop)"),
                        reader.index("LoadEntityRegistrySnapshotJson(bytes,"))
        admission = (self.function(source, "SavedInputFile", "SavedInputFile::") +
                     read_source("src/luminumbra_common/persistence/SavedInputFile.h"))
        self.assertIn("O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC", admission)
        self.assertIn("FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS", admission)
        for check in ("::fstat(m_descriptor, &information)", "S_ISREG(information.st_mode)",
                      "::GetFileType(m_handle) != FILE_TYPE_DISK",
                      "::GetFileInformationByHandle(m_handle, &information)",
                      "FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY",
                      "::GetFileSizeEx(m_handle, &length)", "size > bytes.max_size()"):
            self.assertLess(admission.index(check), admission.index("std::string candidate("))
        self.assertLess(admission.index("std::string candidate("), admission.index("::read(m_descriptor,"))
        self.assertLess(admission.index("std::string candidate("), admission.index("::ReadFile(m_handle,"))
        self.assertIn("size - offset", admission)
        self.assertLess(admission.index("size > maximum"),
                        admission.index("std::string candidate("))
        self.assertLess(reader.index("AdmitPlantComponentBytes(bytes)"),
                        reader.index("LoadEntityRegistrySnapshotJson(bytes,"))
        for name in ("read_exact", "at_end"):
            body = self.function(admission, name, "bool ")
            self.assertIn("detail::ReadSavedInputRetryingInterrupt(", body)
        retry = self.function(admission, "ReadSavedInputRetryingInterrupt", "auto ")
        self.assertIn("for (;;) {", retry)
        self.assertIn("received >= 0 || errno != EINTR", retry)
        self.assertIn("::read(m_descriptor, &trailing, 1)", admission)
        self.assertIn("::ReadFile(m_handle, &trailing, 1, &received, nullptr) && received == 0",
                      admission)
        world = self.function(source, "load_world", "WorldSaveService::")
        self.assertIn("load_plant_entities(plants, save_dir, &errors, stop)", world)
        session = self.function(read_source("src/luminumbra_common/world/GameSession.cpp"),
                                "ReadWorldStateFrom")
        self.assertIn("WorldSaveService::load_plant_entities(state.plants, save_dir, &errors)",
                      session)

    def test_native_saved_input_holds_publication_until_after_close(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        header = read_source("src/luminumbra_common/persistence/SavedInputFile.h")
        constructor = self.function(source, "SavedInputFile", "SavedInputFile::")
        destructor = self.function(source, "~SavedInputFile", "SavedInputFile::")
        self.assertIn("detail::FilePublicationReadGuard m_publication;", header)
        for opening in ("::CreateFileW(", "::open("):
            self.assertLess(constructor.index("m_publication(path)"), constructor.index(opening))
        for closing in ("::CloseHandle(m_handle)", "::close(m_descriptor)"):
            self.assertLess(destructor.index(closing), destructor.index("observer(m_path, false)"))
        reader = self.function(header, "read_exact", "bool ")
        self.assertLess(reader.index("stop.stop_requested()"), reader.index("::ReadFile("))
        self.assertLess(reader.index("stop.stop_requested()"), reader.index("::read("))

    def test_saved_input_budgets_precede_allocations_and_publication(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        region = source.split("bool ReadRegionFile(", 1)[1].split("bool AdmitRegionWrite(", 1)[0]
        self.assertLess(region.index("RegionFileLengthAllowed(file_size)"), region.index("std::string manifest("))
        self.assertLess(region.index("ReadU16(control + 4)"), region.index("std::string manifest("))
        for budget in ("kRegionRecordBytes", "kRegionCompressedBytes", "kRegionReadWorkBytes",
                       "kRegionCompressedAggregateBytes"):
            self.assertLess(region.index(budget), region.index("payload.resize("))
        energy_file = read_source("src/luminumbra_common/persistence/SavedEnergyInput.h")
        for budget in ("length > kEnergyReadRecordBytes", "++pages > kEnergyReadPages",
                       "EnergyFieldState::AdmitRecord(header)"):
            self.assertLess(energy_file.index(budget), energy_file.index("input.read(candidate,"))
        energy = read_source("src/luminumbra_common/fields/EnergyFieldState.cpp")
        decode = self.function(energy, "DeserializeRecord", "EnergyFieldState::")
        self.assertLess(decode.index("AdmitRecord(record)"), decode.index("std::istringstream"))
        session = read_source("src/luminumbra_common/world/GameSession.cpp")
        save = self.function(session, "SaveWorldStateToImpl")
        for admission in ("PrepareWorldMetadataTo(", "prepared_plants.prepare(",
                          "prepared_chunks.prepare(", "energy_snapshot.SerializeRecord(" ):
            self.assertLess(save.index(admission), save.index("prepared_chunks.publish("))
        self.assertRegex(save, r"write_chunks\s*&&\s*!prepared_chunks\.prepare\(")
        self.assertIn("!full_snapshot && has_snapshot", save)
        self.assertNotIn("admit_world_write(", save)
        self.assertNotIn("service.save_world(", save)
        self.assertNotIn("save_dirty_chunks(", save)
        metadata = self.function(session, "SaveWorldMetadataTo")
        self.assertLess(metadata.index("PrepareWorldMetadataTo("), metadata.index("PersistSavedPreset("))
        reader = self.function(session, "ReadWorldStateFrom")
        self.assertIn('m_worldOpenError.find("Saved-input admission refused:") == 0', reader)
        self.assertNotIn('m_worldOpenError.find("oversized")', reader)
        metadata_admission = self.function(source, "admit_metadata_bytes", "WorldSaveService::")
        self.assertLess(metadata_admission.index("clock.write_metadata(metadata)"),
                        metadata_admission.index("BoundedJsonDump("))
        pair = self.function(source, "save_metadata_and_active_regions", "WorldSaveService::")
        self.assertLess(pair.index("admit_metadata_bytes(bytes"), pair.index("save_active_regions("))

    def test_saved_json_depth_is_admitted_before_parsing_and_component_copying(self):
        admission = read_source("src/luminumbra_common/persistence/SavedJsonAdmission.h")
        traversal = admission.split("inline bool AdmitJsonValueDepth(", 1)[1].split("inline bool BoundedJsonDump(", 1)[0]
        for guard in ("value.is_binary()", "child.is_binary()", "value.is_discarded()", "child.is_discarded()"):
            self.assertIn(guard, traversal)
        reader = admission.split("inline bool ReadSavedJson(", 1)[1].split("// Lexical preflight", 1)[0]
        self.assertLess(reader.index("JsonDepthAllowed(bytes)"),
                        reader.index("nlohmann::json::parse(bytes,"))
        plant = admission.split("inline bool AdmitPlantComponentBytes(", 1)[1]
        self.assertIn("JsonDepthAllowed(bytes)", plant)
        entity = read_source("src/luminumbra_common/ecs/EntitySnapshot.h")
        loader = entity.split("inline bool LoadEntityRegistrySnapshotJson(", 1)[1]
        self.assertLess(loader.index("JsonDepthAllowed(json_text)"),
                        loader.index("nlohmann::json::parse(json_text)"))
        self.assertLess(loader.index("JsonDepthAllowed(json_text)"),
                        loader.index("entity.components.push_back("))
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        writer = self.function(source, "prepare_plant_entities", "WorldSaveService::")
        self.assertLess(writer.index("AdmitJsonValueDepth(component.data,"),
                        writer.index('values.push_back({{"type", component.type}'))

    def test_metadata_writer_and_catalog_share_one_limit_and_creation_is_session_owned(self):
        limits = read_source("src/luminumbra_common/persistence/SavedInputLimits.h")
        self.assertNotIn("kCatalogMetadataBytes", limits)
        catalog = read_source("src/luminumbra_common/persistence/SavedWorldCatalog.cpp")
        self.assertRegex(catalog, r"ReadSavedJson\(\s*metadata_path, SavedInputLimits::kMetadataBytes,")
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        writer = self.function(source, "save_metadata", "WorldSaveService::")
        self.assertNotIn("create_directories", writer)
        session = read_source("src/luminumbra_common/world/GameSession.cpp")
        reconcile = self.function(session, "ReconcileWorldStateMetadataTo")
        self.assertLess(reconcile.index("PrepareWorldMetadataTo("),
                        reconcile.index("create_directories("))
        self.assertLess(reconcile.index("create_directories("),
                        reconcile.index("WorldSaveService::save_metadata("))

    def test_checked_rename_uses_explicit_source_link_policy_on_both_platforms(self):
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        rename = self.function(source, "rename_file_durably", "WorldSaveService::")
        self.assertIn("SavedInputFile input(source, SavedInputFile::LeafSymlinks::Follow)", rename)
        reader = read_source("src/luminumbra_common/persistence/SavedInputFile.h")
        self.assertIn("LeafSymlinks leaf_symlinks = LeafSymlinks::Refuse", reader)
        constructor = self.function(source, "SavedInputFile", "SavedInputFile::")
        self.assertEqual(constructor.count("leaf_symlinks == LeafSymlinks::Refuse"), 2)
        self.assertLess(rename.index("input.read(contents, expected_bytes.size())"),
                        rename.index("ReclaimAbandonedStaging("))

    def test_synthetic_save_preserves_collision_precedence_and_write_diagnostics(self):
        source = read_source("src/luminumbra_common/persistence/SyntheticSaveFixture.cpp")
        writer = self.function(source, "WriteSyntheticSave", "SyntheticSaveReport ")
        self.assertLess(writer.index("save already exists"), writer.index("SavedInputFile input("))
        self.assertLess(writer.index("SavedInputFile input("), writer.index("fs::create_directories("))
        self.assertRegex(writer, r'write_file_durably\(\s*report.save_dir / "preset.json", '
                         r'preset_bytes, &save_errors\)')
        self.assertIn('save_errors.empty() ? "cannot persist preset" : save_errors.front()', writer)

    def test_preset_hash_encoding_and_existing_exact_limit_are_consistent(self):
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        create = self.function(source, "CreateWorldInternal")
        self.assertRegex(create, r'kPresetBytes - 1,\s*saved_preset_json,\s*-1,\s*false\)')
        compatible = self.function(source, "SavedPresetCompatible")
        self.assertNotIn("canonical.size()", compatible.split("if (has_preset)", 1)[0])
        self.assertIn("canonical.size() < Persistence::SavedInputLimits::kPresetBytes", compatible)
        persist = self.function(source, "PersistSavedPreset", "bool ")
        self.assertLess(persist.index("if (!missing)"), persist.index("canonical.size() >="))
        self.assertRegex(persist, r'write_file_durably\(\s*path, canonical \+ "\\n", nullptr, artifacts\)')

    def test_energy_writer_counts_normalized_pages_and_plants_publish_prepared_bytes(self):
        energy = read_source("src/luminumbra_common/fields/EnergyFieldState.cpp")
        encode = self.function(energy, "SerializeRecord", "EnergyFieldState::")
        self.assertLess(encode.index("normalized.NormalizeAllPages()"),
                        encode.index("normalized.m_pages.size() > kEnergyWritePages"))
        source = read_source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        publish = self.function(source, "publish", "WorldSaveService::PreparedPlantWrite::")
        self.assertIn("WriteFileDurably(path, m_bytes, staging, errors)", publish)
        self.assertNotIn("prepare_plant_entities", publish)
        session = self.function(read_source("src/luminumbra_common/world/GameSession.cpp"),
                                "SaveWorldStateToImpl")
        self.assertIn("prepared_plants.publish(save_dir, &errors)", session)
        self.assertNotIn("save_plant_entities(", session)
        self.assertEqual(session.count("SerializeRecord("), 1)
        self.assertIn("write_file_durably(energy_path, energy_bytes)", session)
        self.assertLess(session.index("prepared_plants.prepare("),
                        session.index("prepared_chunks.publish("))

    def test_operation_refusal_cannot_change_current_session_usability(self):
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        header = read_source("src/luminumbra_common/world/GameSession.h")
        self.assertIn("bool m_worldUsable = false;", header)
        self.assertIn("return m_worldOpenError;", header)
        reset = self.function(source, "ResetWorldSystems")
        self.assertIn("m_worldUsable = false;", reset)
        for name in ("CreateWorldInternal", "LoadWorld", "LoadWorldFrom"):
            body = self.function(source, name)
            preparation, adoption = body.split("ResetWorldSystems();", 1)
            if name in ("LoadWorld", "LoadWorldFrom"):
                self.assertIn("LoadedWorldState loaded;", preparation)
                preparation, state_read = preparation.split("LoadedWorldState loaded;", 1)
                self.assertIn("if (!ReadWorldStateFrom(worldPath, loaded, true))", state_read)
                self.assertLess(state_read.index("m_worldUsable = false;"),
                                state_read.index("m_worldSystem->clear_world("))
            self.assertNotIn("m_worldUsable", preparation)
            self.assertIn("m_worldOpenError =", preparation)
            self.assertLess(adoption.index("m_speciesTable ="),
                            adoption.index("m_worldUsable = true;"))
        creation = self.function(source, "CreateWorldInternal")
        metadata_failure = creation.split("if (!transient && !SaveWorldMetadataTo(GetWorldSaveDir(), artifacts))", 1)[1]
        self.assertLess(metadata_failure.index("m_worldUsable = false;"),
                        metadata_failure.index("return false;"))
        opening = self.function(source, "LoadWorld")
        self.assertLess(opening.index("ReadWorldStateFrom(worldPath, loaded, true)"),
                        opening.index("ResetWorldSystems();"))
        self.assertLess(opening.index("AdoptWorldStateFrom(worldPath, loaded);"),
                        opening.index("m_worldUsable = true;"))

        body = self.function(source, "LoadWorldFrom")
        preparation, adoption = body.split("ResetWorldSystems();", 1)
        preparation, state_read = preparation.split("LoadedWorldState loaded;", 1)
        self.assertNotIn("m_worldUsable", preparation)
        self.assertIn("m_worldOpenError =", preparation)
        self.assertIn("Persistence::InspectSavedWorld(save_root, RuntimeRoot(m_rootPath), worldId)",
                      preparation)
        self.assertIn('const auto worldPath = save_root / "worlds/saves" / worldId;', preparation)
        self.assertNotIn("m_saveRootPath =", body)
        self.assertLess(adoption.index("m_speciesTable ="),
                        adoption.index("m_worldUsable = true;"))
        self.assertIn("if (!ReadWorldStateFrom(worldPath, loaded, true))", state_read)
        self.assertLess(body.index("ReadWorldStateFrom(worldPath, loaded, true)"),
                        body.index("ResetWorldSystems();"))
        self.assertLess(adoption.index("AdoptWorldStateFrom(worldPath, loaded);"),
                        adoption.index("m_worldUsable = true;"))
        self.assertIn("m_renderCalendar = loaded.render_calendar;", adoption)
        self.assertIn("m_worldClock = loaded.clock;", adoption)

    def test_persistence_uses_session_state_and_fatal_snapshot_refusals_latch_it(self):
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        for name in ("SaveWorldMetadataTo", "SaveWorldStateToImpl",
                     "LoadWorldStateFrom"):
            with self.subTest(consumer=name):
                body = self.function(source, name)
                admission = body[:body.index("LoadedWorldState state;")] \
                    if name == "LoadWorldStateFrom" else body[:body.index("return false;")]
                self.assertIn("!m_worldUsable", admission)
                self.assertNotIn("m_worldOpenError", admission)
        wrapper = self.function(source, "SaveWorldStateTo")
        self.assertIn("return SaveWorldStateToImpl(", wrapper)
        readiness = self.function(source, "LoadWorldState")
        self.assertIn("return m_worldSystem && m_worldUsable;", readiness)
        snapshot = self.function(source, "LoadWorldStateFrom")
        # Fatal snapshot refusals invalidate; admission refuses before live state changes.
        failure = snapshot.split("if (!ReadWorldStateFrom(save_dir, state))", 1)[1]
        self.assertLess(failure.index("m_worldUsable = false;"),
                        failure.index("m_worldSystem->clear_world("))
        self.assertLess(failure.index("return false;"),
                        failure.index("AdoptWorldStateFrom(save_dir, state);"))
        self.assertEqual(snapshot.count("m_worldSystem->clear_world("), 1)
        self.assertIn("if (!state.input_refused)", failure)
        for name in ("LoadWorld", "LoadWorldFrom"):
            self.assertIn("if (!loaded.input_refused)", self.function(source, name))
        reader = self.function(source, "ReadWorldStateFrom")
        self.assertEqual(reader.count("return false;"), 7)
        self.assertEqual(reader.count("m_worldOpenError ="), 7)
        for validation in ("service.load_world(", "WorldSaveService::read_clock_metadata(",
                           "WorldSaveService::load_active_regions(", "m_regionSchedulerOverride",
                           "chunk->sdf_data.size() != kFullSdfLattice",
                           "ReadSavedEnergy(input, record)",
                           "state.energy->DeserializeRecord(record, tick_base)"):
            self.assertIn(validation, reader)
        self.assertLess(reader.index("load_plant_entities(state.plants"),
                        reader.index("service.load_world("))
        adoption = self.function(source, "AdoptWorldStateFrom")
        self.assertNotIn("load_plant_entities(", adoption)
        self.assertNotIn("aether_state.efs", adoption)
        self.assertIn("*m_energyFieldState = std::move(*saved.energy)", adoption)
        self.assertIn("ApplyPlantEntitySnapshot(m_registry, saved.plants)", adoption)
        self.assertNotIn("m_worldUsable", reader)
        self.assertNotIn("m_worldSystem->", reader)
        self.assertNotIn("m_renderCalendar =", reader)
        self.assertNotIn("RestoreWorldClock(", reader)
        self.assertNotIn("m_worldUsable = true;", snapshot)
        saving = self.function(source, "SaveWorldStateToImpl")
        self.assertLess(saving.index("ValidatePlantEntitySnapshot(plant_snapshot)"),
                        saving.index("prepared_chunks.publish("))
        self.assertLess(saving.index("ValidatePlantEntitySnapshot(plant_snapshot)"),
                        saving.index("prepared_plants.publish("))
        runner = read_source("src/luminumbra_server/ServerWorldRunner.cpp")
        full_snapshot = runner.split("ServerWorldRunner::SaveFullSnapshot()", 1)[1].split(
            "ServerWorldRunner::StreamedChunkCount()", 1)[0]
        self.assertNotIn("GetWorldOpenError()", full_snapshot)
        self.assertIn("m_session->SaveWorldStateTo(", full_snapshot)

    def test_normal_custom_validation_precedes_directory_and_uses_prepared_identity(self):
        source = read_source("src/luminumbra_common/world/GameSession.cpp")
        creation = self.function(source, "CreateWorldInternal")
        directory = creation.index("fs::create_directories(worldPath.parent_path())")
        for validation in ("nlohmann::json::parse(*customPresetJson)",
                           "ResolveEffectivePreset(resolved, presetUse)",
                           "LoadTerrainPresetFromJson(resolved, RuntimeRoot(m_rootPath)",
                           "if (!preset.ok)",
                           "preset.preset_content_hash != requiredPresetContentHash"):
            self.assertLess(creation.index(validation), directory)
        self.assertIn('BoundedJsonDump(', creation)
        self.assertIn('saved_preset_json + "\\n"', creation)
        adoption = creation.split("ResetWorldSystems();", 1)[1]
        self.assertIn("m_presetContentHash = preset.preset_content_hash;", adoption)
        self.assertIn("m_speciesTable = preset.params.worldgen_dependencies->species_table;", adoption)
        self.assertNotIn("LoadTerrainPreset", adoption)


class FarVolumeAdmissionSourceContract(unittest.TestCase):
    """Guard bounded publication structure; native behaviour stays a native test."""

    def method(self, path, start, end):
        source = read_source("src/luminumbra_client/rendering/" + path)
        return source.split(start, 1)[1].split(end, 1)[0]

    def test_admission_uses_bounded_sorted_scratch_and_monotonic_free_cursors(self):
        for path, start, end in (
                ("FarVolumeHandoff.cpp", "bool FarVolumeHandoff::StageTarget(",
                 "void FarVolumeHandoff::Reject("),
                ("FarVolumeResidency.cpp", "bool FarVolumeResidency::ReplaceWanted(",
                 "void FarVolumeResidency::Reject(")):
            with self.subTest(path=path):
                body = self.method(path, start, end)
                self.assertIn("std::sort(incoming.begin(), incoming.end()", body)
                self.assertIn("tier.key_index[tier.entries.size() + count++]", body)
                self.assertLess(body.index("count == tier.entries.size()"),
                                body.index("tier.key_index[tier.entries.size() + count++]"))
                self.assertIn("tier.order[index] == 0", body)
                self.assertIn("auto& cursor = next_free[number]", body)
                self.assertIn("entry = &tier.entries[cursor++]", body)
                self.assertIn("tier.queue.ForgetMatching", body)
                self.assertNotIn("queue.Forget(", body)
                self.assertNotRegex(body, r"std::(?:none_of|find_if|count_if)")
                self.assertNotRegex(body, r"for\s*\([^;]*;\s*j\s*<\s*(?:i|index)\b")

    def test_live_lookup_and_rebuild_have_no_hash_collision_chains(self):
        for owner in ("FarVolumeHandoff", "FarVolumeResidency"):
            with self.subTest(owner=owner):
                lookup = self.method(owner + ".cpp", "const " + owner + "::Entry* " +
                                     owner + "::Find(", owner + "::Entry* " + owner + "::Find(")
                self.assertIn("std::lower_bound", lookup)
                self.assertNotIn("for (", lookup)
                self.assertNotIn("while (", lookup)
                rebuild = self.method(owner + ".cpp", "void " + owner +
                                      "::Tier::RebuildIndex(", owner + "::" + owner + "(")
                self.assertIn("std::sort", rebuild)
                self.assertNotIn("while (", rebuild)
                self.assertIn("++work.key_comparisons", rebuild)
                drain = read_source("src/luminumbra_client/rendering/" + owner + ".cpp").split(
                    "void " + owner + "::CancelAndDrain()", 1)[1]
                self.assertIn("index_count = 0", drain)

    def test_batch_retirement_does_not_search_descriptors_per_selected_key(self):
        header = read_source("src/luminumbra_client/rendering/FarVolumeBuildQueue.h")
        batch = header.split("std::size_t ForgetMatching(", 1)[1].split(
            "bool Retry(", 1)[0]
        self.assertIn("noexcept(predicate", batch)
        self.assertIn("index < descriptors_.size()", batch)
        self.assertIn("ForgetAt(index)", batch)
        self.assertNotIn("Find(", batch)
        self.assertNotIn("Cancel(", batch)
        forget = self.method("FarVolumeBuildQueue.cpp", "void FarVolumeBuildQueue::ForgetAt(",
                             "bool FarVolumeBuildQueue::Retry(")
        self.assertIn("CancelAt(index)", forget)
        self.assertNotIn("Find(", forget)
        self.assertNotIn("Cancel(", forget)


if __name__ == "__main__":
    unittest.main()
