"""Source contracts for live water mesh interpolation and consumer-driven preparation."""

import importlib.util
import math
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def source(path):
    text = (ROOT / path).read_text(encoding="utf-8-sig")
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"\s+", " ", text)


class WaterSpecularContract(unittest.TestCase):
    def setUp(self):
        shader = source("res/shaders/water.frag")
        self.assertIn("vec3 view_dir = normalize(fs_in.world_pos - u_camera_pos);", shader)
        # Evaluate the shipping expression's signs, rather than an independent
        # corrected formula. Unknown expressions fail instead of using a fallback.
        expression = re.search(
            r"vec3 half_vector = normalize\(\s*(-?)\s*u_sun_direction\s*([+-])\s*view_dir\s*\);",
            shader)
        self.assertIsNotNone(expression)
        self.light_sign = -1 if expression[1] == "-" else 1
        self.view_sign = -1 if expression[2] == "-" else 1
        self.assertIn('setVec3("u_sun_direction", ctx.sun.direction);',
                      source("src/luminumbra_client/rendering/passes/WaterPass.cpp"))

    @staticmethod
    def dot(left, right):
        return sum(a * b for a, b in zip(left, right))

    def normalized(self, vector):
        length = math.sqrt(self.dot(vector, vector))
        self.assertGreater(length, 1e-12, "half vector must be defined at the reflection")
        return tuple(value / length for value in vector)

    def half_vector(self, travel, view):
        return self.normalized(tuple(self.light_sign * light + self.view_sign * eye
                                     for light, eye in zip(travel, view)))

    def test_sun_reflection_aligns_half_vector_with_flat_and_ripple_normals(self):
        # Fixed 24 cases, each using three-component vectors. Reflection of the
        # incident light independently constructs the surface-to-camera ray.
        normals = ((0, 1, 0), self.normalized((.08, 1, -.06)))
        for elevation in (5, 30, 60, 90):
            height, radius = math.sin(math.radians(elevation)), math.cos(math.radians(elevation))
            for azimuth in (0, 90, 225):
                angle = math.radians(azimuth)
                travel = (-radius * math.cos(angle), -height, -radius * math.sin(angle))
                for normal in normals:
                    with self.subTest(elevation=elevation, azimuth=azimuth, normal=normal):
                        incidence = self.dot(travel, normal)
                        self.assertLess(incidence, 0)
                        to_eye = tuple(light - 2 * incidence * axis
                                       for light, axis in zip(travel, normal))
                        view = tuple(-axis for axis in to_eye)
                        half = self.half_vector(travel, view)
                        ndoth = self.dot(normal, half)
                        self.assertAlmostEqual(ndoth, 1, places=12)
                        self.assertAlmostEqual(max(0, ndoth) ** 64, 1, places=12)
                        self.assertEqual(max(0, -ndoth) ** 64, 0)
                        # The old formula is tangent to the surface, or zero at
                        # exact normal incidence; neither can produce the peak.
                        old = tuple(light - eye for light, eye in zip(travel, view))
                        if self.dot(old, old) > 1e-24:
                            self.assertAlmostEqual(self.dot(normal, self.normalized(old)),
                                                   0, places=12)
                        else:
                            self.assertLessEqual(self.dot(old, old), 1e-24)

    def test_camera_away_from_the_sun_road_has_no_peak(self):
        travel = (-math.sqrt(.75), -.5, 0)
        # Same camera elevation, a quarter-turn away from the reflected sun.
        view = (0, -.5, -math.sqrt(.75))
        half = self.half_vector(travel, view)
        self.assertLess(max(0, half[1]) ** 64, 1e-10)


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
        self.assertTrue(gate.needs_python("res/shaders/water.frag"))
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
        probe = capture.split("SampleCausticsTextureDelta(", 1)[0]
        self.assertIn("render_pass_stats.water_draws > 0", probe.rsplit("if (", 1)[1])
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
