"""Source and interval contracts for the shipping terrain G-buffer shader."""

import importlib.util
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def source(path):
    uncommented = re.sub(r"//[^\n]*|/\*.*?\*/", "",
                         (ROOT / path).read_text(encoding="utf-8"), flags=re.DOTALL)
    return re.sub(r"\s+", " ", uncommented)


class TerrainShaderContract(unittest.TestCase):
    def setUp(self):
        self.shader = source("res/shaders/g_buffer.frag")
        self.main = self.shader.split("void main()", 1)[1]
        self.terrain = self.main.split("else if (texInfo.a > 0.5 && u_forceFlat == 0)", 1)[1]

    def test_projection_and_slope_use_the_face_before_normal_mapping_or_discard(self):
        # Collapsed relative derivatives retain a finite interpolated normal;
        # back-facing winding is aligned to the authored surface orientation.
        self.assertIn("extent > 1e-20 ? face * inversesqrt(extent) : smoothNormal", self.shader)
        self.assertIn("dot(n, smoothNormal) < 0.0 ? -n : n", self.shader)
        call = "terrain_surface_normal(fs_in.FragPos, normalize(fs_in.WorldNormal))"
        self.assertIn(call, self.main)
        self.assertLess(self.main.index(call), self.main.index("discard;"))
        self.assertIn("vec3 weights = triplanar_weights(u_macroRockOverlay == 1 ? surfaceN : worldN);",
                      self.terrain)
        self.assertIn("float geomSlope = surfaceN.y;", self.terrain)
        self.assertLess(self.terrain.index("float geomSlope"),
                        self.terrain.index("triplanar_normal("))
        for channel in ("albedo", "normal", "roughness"):
            self.assertIn(f"triplanar_{channel}(fs_in.WorldPos, "
                          + ("worldN, " if channel == "normal" else "") + "weights,",
                          self.terrain)

    def test_face_derivatives_use_view_relative_positions_and_return_world_normals(self):
        helper = re.search(r"vec3 terrain_surface_normal\(vec3 viewPos, vec3 smoothNormal\) "
                           r"\{([^{}]+)\}", self.shader)
        self.assertIsNotNone(helper)
        self.assertIn("vec3 face = transpose(u_normalViewMatrix) * "
                      "cross(dFdx(viewPos), dFdy(viewPos));", helper[1])
        self.assertNotIn("WorldPos", helper[1])
        self.assertIn("terrain_surface_normal(fs_in.FragPos, normalize(fs_in.WorldNormal))",
                      self.main)
        # Every producer carries view space, including the unbiased ladder input.
        for vertex, transform in (
                ("g_buffer.vert", "view * worldPos"),
                ("instanced_mesh.vert", "view * worldPos"),
                ("skinned_mesh.vert", "viewModel * skinnedPos")):
            with self.subTest(vertex=vertex):
                text = source("res/shaders/" + vertex)
                self.assertIn(transform, text)
                self.assertIn("vs_out.FragPos = vec3(viewPos);", text)
                self.assertIn("gl_Position = projection * viewPos;", text)
        terrain_vertex = source("res/shaders/g_buffer.vert")
        self.assertIn("vec4 viewPos = u_farWorldLadder == 1 ? "
                      "vec4(mat3(view) * (aPos + u_farRelativeOrigin), 1.0) : view * worldPos;",
                      terrain_vertex)
        renderer = source("src/luminumbra_client/rendering/FarWorldRenderer.cpp")
        self.assertIn("const std::array<double, 3>& camera", renderer)
        self.assertIn("candidate.relative_origin[i] = static_cast<float>(origin[i] - camera[i]);",
                      renderer)
        uniforms = source("src/luminumbra_client/rendering/passes/FarWorldPassUniforms.cpp")
        self.assertIn('relative_ = glGetUniformLocation(program_, "u_farRelativeOrigin");', uniforms)
        self.assertIn("glProgramUniform3fv(program_, relative_, 1, command.relative_origin.data());",
                      uniforms)
        self.assertIn("mat4 viewModel = view * model;", source("res/shaders/skinned_mesh.vert"))
        pass_source = source("src/luminumbra_client/rendering/passes/GBufferPass.cpp")
        for view in ("view", "static_view", "skinned_view"):
            self.assertIn(f'shader->setMat3("u_normalViewMatrix", glm::mat3({view}));', pass_source)
        self.assertIn("return glm::lookAt(Position, Position + Front, Up);",
                      source("src/luminumbra_client/rendering/Camera.h"))

    def test_face_normal_work_is_only_in_a_uniform_terrain_gate_before_discard(self):
        # Anchor the entire prologue: no varying branch or earlier discard may
        # surround the derivatives, including in the ladder program variant.
        self.assertRegex(self.main, r"^ \{ vec3 surfaceN = vec3\(0.0\); "
                         r"if \(u_macroRockOverlay == 1\) \{ "
                         r"surfaceN = terrain_surface_normal\([^;]+; \}")
        self.assertEqual(self.main.count("terrain_surface_normal("), 1)
        self.assertIn("uniform int u_macroRockOverlay = 0;", self.shader)
        # Props that reuse triplanar materials keep smooth projection weights;
        # foliage, authored UV models and untextured draws never consume the face.
        self.assertIn("triplanar_weights(u_macroRockOverlay == 1 ? surfaceN : worldN)",
                      self.terrain)
        self.assertLess(self.main.index("terrain_surface_normal("), self.main.index("discard;"))

    def test_steep_sand_has_defined_rock_coverage_and_flat_ground_is_preserved(self):
        guard = re.search(r"if \(u_macroRockOverlay == 1 && geomSlope < ([\d.]+) && "
                          r"fs_in.MaterialID >= (\d+)u && fs_in.MaterialID <= (\d+)u\)",
                          self.terrain)
        self.assertIsNotNone(guard)
        cutoff, first, last = float(guard[1]), int(guard[2]), int(guard[3])
        self.assertEqual((first, last), (1, 4))
        blend = re.search(r"float rockW = 1.0 - smoothstep\(([\d.]+) \+ jitter, "
                          r"([\d.]+) \+ jitter, geomSlope\);", self.terrain)
        self.assertIsNotNone(blend, "GLSL smoothstep requires ascending edges")
        low, high = float(blend[1]), float(blend[2])
        jitter = re.search(r"float jitter = \(vnoise\(fs_in.WorldPos \* [\d.]+\) - 0.5\) "
                           r"\* ([\d.]+);", self.terrain)
        self.assertIsNotNone(jitter)
        radius = float(jitter[1]) / 2
        self.assertLess(low, high)
        self.assertAlmostEqual(cutoff, high + radius)
        for offset in (-radius, 0, radius):
            for slope, expected in ((-1, 1), (0, 1), (1, 0), (cutoff, 0)):
                with self.subTest(offset=offset, slope=slope):
                    t = min(1, max(0, (slope - low - offset) / (high - low)))
                    self.assertAlmostEqual(1 - t * t * (3 - 2 * t), expected)

    def test_rock_uses_its_own_calibration_and_scalar_roughness_fallback(self):
        self.assertRegex(self.terrain, r"vec3 baseAlbedo\s*=\s*triplanar_albedo\([^;]+\)"
                         r"\s*\* albedoScale \* albedoTint;")
        self.assertIn("rockAlbedo *= rockAlbedoScale * rockAlbedoTint;", self.terrain)
        for row in (2, 3):
            self.assertRegex(self.terrain, rf"vec2\(1.0\s*/\s*255.0, lutRowV\({row}\)\)")
        self.assertIn("albedo = baseAlbedo;", self.terrain)
        self.assertNotIn("albedo = baseAlbedo *", self.terrain)
        self.assertRegex(self.terrain, r"float rockRough = u_terrainRoughnessValid == 1\s*"
                         r"\? triplanar_roughness\([^;]+\)\s*: rockProps.g;")
        self.assertIn("baseRoughness = mix(baseRoughness, rockRough, rockW);", self.terrain)
        self.assertIn("metallic = mix(metallic, rockProps.r, rockW);", self.terrain)

    def test_each_albedo_projection_blurs_relative_to_its_own_mip(self):
        albedo = self.shader.split("vec3 triplanar_albedo(", 1)[1].split(
            "vec3 triplanar_normal(", 1)[0]
        self.assertIn("vec3 baseLod =", albedo)
        self.assertIn("vec3 blurLod = baseLod + kDetailBlurLod + u_lodBias;", albedo)
        for axis in "xyz":
            self.assertEqual(albedo.count(f"textureQueryLod(u_terrainTextures, uv_{axis})"), 1)
            self.assertIn(f"vec3(uv_{axis}, layer), blurLod.{axis})", albedo)

    def test_only_terrain_draws_opt_into_slope_material_substitution(self):
        self.assertIn("uniform int u_macroRockOverlay = 0;", self.shader)
        pass_source = source("src/luminumbra_client/rendering/passes/GBufferPass.cpp")
        self.assertIn('shader->setInt("u_macroRockOverlay", 1);', pass_source)
        self.assertIn('shader->setInt("u_macroRockOverlay", 0);', pass_source)
        specs = source("test/rendering/render_smoke_test.cpp")
        for vertex in ("g_buffer.vert", "instanced_mesh.vert", "skinned_mesh.vert"):
            self.assertRegex(specs, '"' + re.escape(vertex) + '", "g_buffer.frag"')
        self.assertIn("LUMINUMBRA_FAR_WORLD_LADDER", specs)

    def test_shader_changes_run_this_registered_suite_on_both_ci_platforms(self):
        spec = importlib.util.spec_from_file_location("terrain_ci_paths", ROOT / "tools/ci/ci_paths.py")
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        self.assertIn("tools/ci/test_terrain_shader_contract.py", gate.PYTHON_TESTS)
        self.assertTrue(gate.needs_python("res/shaders/g_buffer.frag"))
        cmake = source("tools/CMakeLists.txt")
        self.assertRegex(cmake, r"NAME TerrainShaderContract\s+COMMAND "
                         r'"\$\{Python3_EXECUTABLE\}" -I -B\s+'
                         r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/ci/test_terrain_shader_contract.py"')
        for workflow in ("ci.yml", "windows-build.yml"):
            text = (ROOT / ".github/workflows" / workflow).read_text(encoding="utf-8")
            self.assertIn("tools/ci/ci_paths.py python-tests", text)


if __name__ == "__main__":
    unittest.main()
