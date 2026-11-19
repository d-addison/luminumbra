"""Source contracts for wet-only interpolation of the live water render mesh."""

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
        self.assertRegex(self.producer, r"chunk.water_depth_mm\[[^;]+ > "
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
        self.assertIn("snapshot.water_depth_mm = chunk->water_depth_mm;", world)
        self.assertIn("auto& snapshot = *scratch_owner;", world)
        self.assertIn("auto& scratch = *scratch_owner;", world)
        self.assertIn("*m_water_system, *this, scratch);", world)
        self.assertIn("presentation.water_depth_mm = chunk->water_depth_mm;", world)

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


if __name__ == "__main__":
    unittest.main()
