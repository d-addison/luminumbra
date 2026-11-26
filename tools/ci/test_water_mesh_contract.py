"""Source contracts for live water mesh interpolation and consumer-driven preparation."""

import importlib.util
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def source(path):
    text = (ROOT / path).read_text(encoding="utf-8-sig")
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"\s+", " ", text)


class WaterMeshContract(unittest.TestCase):
    maxDiff = 1000

    def setUp(self):
        self.mesh = source("src/luminumbra_common/world/MarchingCubes.cpp")
        self.sampler = self.mesh.split("float SampleChunkWaterLevel(", 1)[1].split(
            "u32 FallbackTransitionMaterial(", 1)[0]
        self.producer = self.mesh.split("void GenerateWaterMesh(", 1)[1]

    def test_dry_bed_samples_cannot_contribute_to_a_wet_surface(self):
        self.assertIn("const std::array<std::size_t, 4> samples", self.sampler)
        self.assertIn("const std::array<float, 4> weights", self.sampler)
        self.assertIn("corner < samples.size()", self.sampler)
        self.assertRegex(self.sampler, r"if \(chunk.water_depth_mm\[index\] > "
                         r"kMinRenderableWaterDepthMm\) \{ "
                         r"surface_sum \+= chunk.water_level_data\[index\] \* weights\[corner\]; "
                         r"weight_sum \+= weights\[corner\]; \} else \{ all_wet = false; \}")
        self.assertIn("return all_wet || weight_sum == 0.0f ? legacy_height : "
                      "surface_sum / weight_sum;", self.sampler)
        self.assertEqual(self.producer.count("SampleChunkWaterLevel("), 4)

    def test_sampling_and_quad_culling_use_one_strict_threshold(self):
        self.assertEqual(self.mesh.count("constexpr std::int32_t kMinRenderableWaterDepthMm"), 1)
        self.assertIn("kMinRenderableWaterDepthMm = 50;", self.mesh)
        self.assertRegex(self.producer, r"water_source.water_depth_mm\[[^;]+ > "
                         r"kMinRenderableWaterDepthMm;")
        self.assertIn("if (renderable) {", self.producer)

    def test_missing_partial_and_fully_wet_grids_preserve_legacy_interpolation(self):
        self.assertIn("if (chunk.water_depth_mm.size() != cells) { return legacy_height; }",
                      self.sampler)
        self.assertIn("const float legacy_height = glm::mix(h_z0, h_z1, tz);", self.sampler)
        self.assertLess(self.sampler.index("chunk.water_depth_mm.size() != cells"),
                        self.sampler.index("chunk.water_depth_mm[index]"))
        self.assertIn("all_wet || weight_sum == 0.0f ? legacy_height", self.sampler)
        # Both inline and worker lanes copy the depth grid into their scratch chunk.
        world = source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        # The worker snapshot copies depth from the shared column water source.
        self.assertIn("snapshot.water_depth_mm = water_source.water_depth_mm;", world)
        self.assertIn("auto& snapshot = *scratch_owner;", world)
        self.assertIn("auto& scratch = *scratch_owner;", world)
        self.assertIn("*m_water_system, *this, scratch);", world)
        # Initial presentation captures the same column source and owner interval.
        for capture in ("presentation.water_depth_mm = water_source.water_depth_mm;",
                        "presentation.water_mesh_min_y = chunk->water_mesh_min_y;",
                        "presentation.water_mesh_max_y = chunk->water_mesh_max_y;",
                        "presentation.water_mesh_inputs_captured = true;"):
            self.assertIn(capture, world)

    def test_native_and_source_regressions_are_registered(self):
        native = source("test/shield/async_water_meshing_test.cpp")
        for name in ("DryBedsCannotPullStandingWaterOntoBanksOrIntoExcavations",
                     "WetGradientsAndIncompleteDepthGridsKeepBilinearHeights"):
            self.assertIn(f"TEST(WaterMesh, {name})", native)
        self.assertIn("shield/async_water_meshing_test.cpp", source("test/CMakeLists.txt"))
        spec = importlib.util.spec_from_file_location("water_ci_paths", ROOT / "tools/ci/ci_paths.py")
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        self.assertIn("tools/ci/test_water_mesh_contract.py", gate.PYTHON_TESTS)
        self.assertTrue(gate.needs_python("src/luminumbra_common/world/MarchingCubes.cpp"))
        self.assertRegex(source("tools/CMakeLists.txt"), r"NAME WaterMeshContract COMMAND "
                         r'"\$\{Python3_EXECUTABLE\}" -I -B '
                         r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/ci/test_water_mesh_contract.py"')
        for workflow in ("ci.yml", "windows-build.yml"):
            self.assertIn("tools/ci/ci_paths.py python-tests",
                          (ROOT / ".github/workflows" / workflow).read_text(encoding="utf-8"))


class WaterPreparationContract(unittest.TestCase):
    def test_preparation_requires_the_submitted_water_glass_or_capture_consumer(self):
        pipeline = source("src/luminumbra_client/rendering/RenderPipeline.cpp")
        snapshot = pipeline.split("void RenderPipeline::execute_stage_opaque_snapshot(", 1)[1].split(
            "void RenderPipeline::execute_stage_water(", 1)[0]
        self.assertIn("m_frame_water_input.draw_items.clear();", snapshot)
        self.assertIn("WaterPreparationDrawConsumes(it->second.element_count)", snapshot)
        self.assertIn("PresentedLadderWaterEligible", snapshot)
        self.assertIn("m_isolation_config.renders(Client::ScenarioHarness::IsolationLayer::Water)",
                      snapshot)
        self.assertIn(".water = !m_frame_water_input.draw_items.empty()", snapshot)
        self.assertIn(".glass = !m_glass_pane_items.empty() && m_glass_quad_vao != 0", snapshot)
        self.assertIn(".capture = m_terrain_coverage_enabled || m_water_visibility_capture_enabled",
                      snapshot)
        self.assertRegex(snapshot, r"if \(consumers.opaque_snapshot\(\)\) \{ "
                         r"begin_gpu_pass_timer\(GpuTimerPass::OpaqueSnapshot\); "
                         r"m_lighting_pass->copy_lighting_color_to_opaque_texture\(m_frame_lighting_ctx\); "
                         r"end_gpu_pass_timer\(GpuTimerPass::OpaqueSnapshot\);")
        water = pipeline.split("void RenderPipeline::execute_stage_water(", 1)[1].split(
            "const WaterVisibilityMask&", 1)[0]
        self.assertIn("water_ctx, water_input, camera, m_water_visibility_capture_enabled",
                      water)
        self.assertIn("const auto& water_input = m_frame_water_input;", water)
        self.assertNotIn("draw_items.push_back", water)

    def test_empty_and_overflow_draws_cannot_trigger_caustics(self):
        water = source("src/luminumbra_client/rendering/passes/WaterPass.cpp")
        execute = water.split("WaterDrawStats WaterPass::execute(", 1)[1].split(
            "void WaterPass::capture_visibility(", 1)[0]
        self.assertIn("m_caustics_valid = false;", execute)
        self.assertIn("return WaterPreparationDrawConsumes(item.element_count);", execute)
        self.assertIn(".water = water_draws, .glass = false, .capture = capture_visibility_requested",
                      execute)
        self.assertRegex(execute, r"if \(consumers.caustics\(\)\) \{ generate_caustics\(ctx\); \}")
        self.assertEqual(execute.count("generate_caustics(ctx)"), 1)
        self.assertIn("std::numeric_limits<GLsizei>::max() == std::numeric_limits<std::int32_t>::max()", water)
        self.assertIn("m_caustics_valid = true;", water)
        helper = source("src/luminumbra_client/rendering/passes/WaterPass.h")
        self.assertIn("return water || glass || capture;", helper)
        self.assertIn("return water || capture;", helper)
        self.assertIn("elements != 0 && elements <= "
                      "static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())",
                      helper)

    def test_remaining_readers_have_current_producers(self):
        # Deferred lighting/debug views have no sampler for either resource.
        for path in ("res/shaders/lighting_pass.frag", "res/shaders/debug_view.frag"):
            self.assertNotRegex(source(path), r"uniform sampler\w* .*?(?:caustic|opaque)")
        pipeline = source("src/luminumbra_client/rendering/RenderPipeline.cpp")
        for name in ("god_rays", "weather"):
            stage = pipeline.split(f"void RenderPipeline::execute_stage_{name}_opaque_snapshot(",
                                   1)[1].split("void RenderPipeline::", 1)[0]
            self.assertIn("copy_lighting_color_to_opaque_texture", stage)
        capture = source("src/luminumbra_client/core/ScenarioRunnerCapture.cpp")
        capture = capture.split("void ScenarioRunnerImpl::captureWaterVisual(", 1)[1]
        capture = capture.split("void ScenarioRunnerImpl::", 1)[0]
        probe = capture.split("SampleCausticsTextureDelta(", 1)[0]
        producer_guard = "if (!water_visual_capture_written && render_pass_stats.water_draws > 0 &&"
        self.assertIn(producer_guard, probe)
        guarded_probe = probe.split(producer_guard, 1)[1]
        self.assertEqual(guarded_probe.count("{") - guarded_probe.count("}"), 1)
        self.assertIn("ReadScenarioSkyMask(", guarded_probe)
        self.assertIn("if (!sky_mask.valid_for(sample_width, sample_height))", guarded_probe)
        refusal = guarded_probe.split("if (!sky_mask.valid_for(sample_width, sample_height))", 1)[1]
        refusal = refusal.split("}", 1)[0]
        self.assertIn('refuseScenarioCapture("scenario_geometry_readback_unavailable")', refusal)
        self.assertIn("return;", refusal)
        self.assertIn("SampleBackbufferWaterLuminance(", guarded_probe)
        self.assertLess(guarded_probe.index("ReadScenarioSkyMask("),
                        guarded_probe.index("SampleBackbufferWaterLuminance("))
        terrain = source("src/luminumbra_client/rendering/TerrainCoverageCapture.cpp")
        terrain = terrain.split("bool RenderPipeline::capture_terrain_coverage(", 1)[1]
        self.assertIn("!m_terrain_coverage_enabled", terrain)
        self.assertIn('"opaque-scene", context.opaque_scene.id', terrain)
        shader = source("res/shaders/caustics_generator.frag")
        self.assertNotIn("sampler", shader)
        self.assertEqual(re.findall(r"uniform \w+ (\w+)", shader), ["u_time", "u_resolution"])

    def test_registered_native_work_counters_and_equivalence_cover_the_contract(self):
        native = source("test/rendering/render_smoke_test.cpp")
        for name in ("WaterPreparationConsumerTruthTableAndDrawLimits",
                     "WaterPreparationCountsActualWritesForDryGlassCaptureAndWaterFrames",
                     "WaterCausticsRegenerateIdenticallyAfterSkippedFrames"):
            self.assertIn(f"TEST(RenderSmokeTest, {name})", native)
        self.assertIn("glad_glDrawArrays = Draw;", native)
        self.assertIn("glad_glCopyTexSubImage2D = Copy;", native)
        self.assertIn("EXPECT_EQ(sample(4.0f, true), expected);", native)
        self.assertIn("render(0, 0);", native)
        self.assertIn("render(1, 1);", native)
        self.assertIn("rendering/render_smoke_test.cpp", source("test/CMakeLists.txt"))
        spec = importlib.util.spec_from_file_location("water_prep_ci_paths", ROOT / "tools/ci/ci_paths.py")
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        for path in ("RenderPipeline.cpp", "passes/WaterPass.cpp",
                     "passes/WaterPass.h"):
            self.assertTrue(gate.needs_python("src/luminumbra_client/rendering/" + path))


if __name__ == "__main__":
    unittest.main()
