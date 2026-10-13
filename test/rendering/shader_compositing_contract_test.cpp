#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadShader(const std::filesystem::path& relative_path) {
    std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / relative_path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void ExpectContains(const std::string& source, const std::string& fragment) {
    EXPECT_NE(source.find(fragment), std::string::npos) << "missing shader contract: " << fragment;
}

std::size_t CountOccurrences(const std::string& source, const std::string& fragment) {
    std::size_t count = 0;
    for (auto pos = source.find(fragment); pos != std::string::npos;
         pos = source.find(fragment, pos + fragment.size())) {
        ++count;
    }
    return count;
}

// Shadow orchestration must delegate only live terrain/glass to ShadowPass.
// Reject direct owner access as well as the selected G-buffer submit seam;
// owner names are independent of whitespace around ->Submit / draw calls.
bool HasFarSubmissionAccess(const std::string& source) {
    const char* const forbidden[] = {"m_far_world",
                                     "far_world_renderer",
                                     "m_farlod",
                                     "FarWorld",
                                     "FarLod",
                                     "submit_ladder",
                                     "SubmitSelectedFarWorld",
                                     "DrawFarVolumeGpu"};
    for (const char* identifier : forbidden) {
        if (source.find(identifier) != std::string::npos)
            return true;
    }
    return false;
}

// Returns the brace-delimited body that follows the first occurrence of `signature`, or an empty
// string when the signature is absent. The sources this is used on must not contain braces inside
// comments or string literals.
std::string ExtractBlock(const std::string& source, const std::string& signature) {
    const std::size_t signature_pos = source.find(signature);
    if (signature_pos == std::string::npos)
        return {};
    const std::size_t open = source.find('{', signature_pos);
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (std::size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') {
            ++depth;
        } else if (source[i] == '}' && --depth == 0) {
            return source.substr(open, i - open + 1);
        }
    }
    return {};
}

TEST(ShaderCompositingContract, WaterSamplesTheResolvedSceneAtFarDepth) {
    const std::string source = ReadShader("res/shaders/water.frag");
    ASSERT_FALSE(source.empty());

    ExpectContains(source, "bool has_opaque_depth(float depth)");
    ExpectContains(source, "background_has_opaque_depth = has_opaque_depth");
    ExpectContains(source, "refracted_color = texture(u_opaque_scene_color, refraction_uv).rgb");
    ExpectContains(source, "background_has_opaque_depth ? u_sky_color : resolved_background_color");
    ExpectContains(source, "if (!has_opaque_depth(mid_depth))");
    EXPECT_EQ(source.find("refraction_depth"), std::string::npos);
}

TEST(FarWorldPositionContract, EveryPositionReadIncludingNeighborsUsesTheSharedHelper) {
    const char* files[]{"lighting_pass.frag",
                        "ssao.frag",
                        "ssao_gtao.frag",
                        "ssao_bilateral_upsample.frag",
                        "scent_decal.frag",
                        "debug_view.frag"};
    const std::size_t expected_calls[]{2, 2, 3, 2, 1, 1};
    for (std::size_t i = 0; i < 6; ++i) {
        SCOPED_TRACE(files[i]);
        const auto source = ReadShader(std::filesystem::path("res/shaders") / files[i]);
        ExpectContains(source, "#include \"far_world_position.glsl\"");
        EXPECT_EQ(CountOccurrences(source, "farWorldPosition("), expected_calls[i]);
        EXPECT_EQ(source.find("texture(gPosition"), std::string::npos);
        EXPECT_EQ(source.find("texelFetch(gPosition"), std::string::npos);
    }
    const auto helper = ReadShader("res/shaders/far_world_position.glsl");
    ExpectContains(helper, "uniform int u_farWorldLadder = 0;");
    ExpectContains(helper, "if (u_farWorldLadder == 0)");
    ExpectContains(helper, "position = texture(storedPosition, uv).rgb;");
    ExpectContains(helper, "depth <= 0.0");
    ExpectContains(helper, "abs(q.w) <= 1e-8");
    ExpectContains(helper, "any(isnan(q)) || any(isinf(q))");
    ExpectContains(helper, "vec4(uv * 2.0 - 1.0, depth, 1.0)");
    ExpectContains(helper, "length(reconstructed) > FAR_WORLD_POSITION_THRESHOLD");
    ExpectContains(helper, "(floor(uv * textureSizePixels) + 0.5) / textureSizePixels");
    EXPECT_EQ(helper.find("depth * 2.0"), std::string::npos);
}

TEST(FarWorldFogContract, FinalFadeFollowsBothCapsAndKeepsUnderwaterSeparate) {
    const auto source = ReadShader("res/shaders/volumetric_lighting.frag");
    const auto compose = ExtractBlock(source, "vec4 composeFinalFog(");
    ExpectContains(compose, "float fog = min(analyticFog, 0.94);");
    ExpectContains(compose, "float outFog = clamp(fog * fx.a + fxFog, 0.0, 0.94);");
    const auto fade = compose.find("float finalTransmittance");
    ASSERT_NE(fade, std::string::npos);
    EXPECT_LT(compose.find("fog = outFog;"), fade);
    EXPECT_EQ(compose.find("0.94", fade), std::string::npos);
    ExpectContains(compose, "(1.0 - fog) * aerialOuterFade(rawRayDistance, 16384.0)");
    ExpectContains(source, "1.0 - smoothstep(0.9 * outerRadius, outerRadius, rayDistance)");
    const auto main = ExtractBlock(source, "void main()");
    ExpectContains(main, "float dist = min(rawRayDistance, u_aerialMaxDistance);");
    ExpectContains(main, "rawRayDistance = length(viewPosition);");
    ExpectContains(main, "clamp(murk, 0.0, 0.92)");
    EXPECT_LT(main.find("if (u_underwater > 0.5)"), main.find("composeFinalFog("));
}

TEST(FarWorldPositionContract, EffectiveFrameProjectionAndPostOverrideDistanceSeams) {
    const auto pipeline = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    for (const char* name : {"gbuffer", "plant", "ssao", "debug_view", "ground_decal",
                             "lighting", "skybox", "water", "aerial"}) {
        const auto body = ExtractBlock(
            pipeline, std::string("RenderContext RenderPipeline::make_") + name + "_context(");
        ExpectContains(body, "m_frame_far_selection.effective == FarWorldMode::Ladder");
        ExpectContains(body, "m_frame_prepared.projection");
        ExpectContains(body, "m_taau_jitter_ndc");
    }
    const auto aerial =
        ExtractBlock(pipeline, "RenderContext RenderPipeline::make_aerial_context(");
    EXPECT_LT(aerial.find("atmo = *s_atmos_override;"), aerial.find("ctx.aerial_max_distance ="));
    ExpectContains(aerial, "World::kFarOuterRadiusMeters");
    const auto context = ReadShader("src/luminumbra_client/rendering/RenderContext.h");
    ExpectContains(context, "selected_projection[2][0] += jitter.x;");
    ExpectContains(context, "selected_projection[2][1] += jitter.y;");
    const char* passes[]{
        "SsaoPass", "AerialPass", "LightingPass", "GroundDecalPass", "DebugViewPass"};
    for (const char* pass : passes) {
        const auto source =
            ReadShader(std::filesystem::path("src/luminumbra_client/rendering/passes") /
                       (std::string(pass) + ".cpp"));
        ExpectContains(source, "ctx.far_world_ladder ? 1 : 0");
        ExpectContains(source, "ctx.gbuffer_inverse_projection");
    }
    for (const char* pass : {"WaterPass", "SkyboxPass"}) {
        const auto source =
            ReadShader(std::filesystem::path("src/luminumbra_client/rendering/passes") /
                       (std::string(pass) + ".cpp"));
        ExpectContains(source, "ctx.far_world_ladder");
        ExpectContains(source,
                       std::string(pass) == "WaterPass" ? "ctx.gbuffer_projection"
                                                        : "ctx.gbuffer_inverse_projection");
    }
    // These existing depth consumers use the zero-to-one convention. Water
    // excludes cleared depth before reconstruction; weather keeps its sky fog.
    const auto water = ReadShader("res/shaders/water.frag");
    const auto weather = ReadShader("res/shaders/weather_system.frag");
    ExpectContains(water, "float z = depth;");
    ExpectContains(weather, "float z = depth;");
}

TEST(FarWorldPositionContract, EveryGbufferDepthWriterConsumesTheSharedLadderProjection) {
    const auto gbuffer = ReadShader("src/luminumbra_client/rendering/passes/GBufferPass.cpp");
    for (const char* name : {"chunks", "static_meshes", "skinned_meshes"}) {
        const auto body = ExtractBlock(
            gbuffer, std::string("void GBufferPass::geometry_pass_") + name + "(");
        ExpectContains(body, "ctx.gbuffer_projection");
        ExpectContains(body, "if (!ctx.far_world_ladder)");
    }
    // Tree impostors inherit the static writer's projection, including jitter.
    ExpectContains(gbuffer, "m_tree_impostor_shader->setMat4(\"u_proj\", static_proj);");
    const auto plant = ReadShader("src/luminumbra_client/rendering/passes/PlantProcgenPass.cpp");
    ExpectContains(plant, "ctx.far_world_ladder");
    ExpectContains(plant, "? ctx.gbuffer_projection");
    ExpectContains(plant, "m_shader->setMat4(\"projection\", projection);");
    const auto pipeline = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    const auto stage = ExtractBlock(pipeline, "void RenderPipeline::execute_stage_gbuffer(");
    ExpectContains(stage, "? gbuffer_ctx.gbuffer_projection");
}

TEST(ShaderCompositingContract, CloudAndAuroraGatesCoverTheFullStormSlab) {
    const std::string source = ReadShader("res/shaders/enhanced_skybox.frag");
    ASSERT_FALSE(source.empty());

    ExpectContains(source, "float lowCov = cloudCoverageAt(viewDir.xz * tEnter)");
    ExpectContains(source, "float highCov = cloudCoverageAt(viewDir.xz * tExit)");
    ExpectContains(source, "midCov = max(midCov, max(lowCov, highCov))");
    ExpectContains(source, "float stormGate = 1.0 - smoothstep(0.05, 0.35, u_stormSkyFloor)");
    ExpectContains(source, "overcastGate * stormGate");
}

TEST(ShaderCompositingContract, StarsAreGatedByNightAndComposedBeforeClouds) {
    const std::string source = ReadShader("res/shaders/enhanced_skybox.frag");
    ASSERT_FALSE(source.empty());

    const std::string stars =
        ExtractBlock(source, "vec3 renderStars(vec3 viewDir, float nightIntensity)");
    ExpectContains(stars, "nightIntensity < 0.1");

    const std::string main_body = ExtractBlock(source, "void main()");
    ASSERT_FALSE(main_body.empty());
    const std::size_t stars_pos = main_body.find("skyColor += renderStars(viewDir, nightFactor);");
    const std::size_t clouds_pos =
        main_body.find("skyColor = renderClouds(viewDir, skyColor, dayFactor);");
    ASSERT_NE(stars_pos, std::string::npos) << "renderStars composition missing from main()";
    ASSERT_NE(clouds_pos, std::string::npos) << "renderClouds composition missing from main()";
    EXPECT_LT(stars_pos, clouds_pos);
}

TEST(GlassOitContract, UsesInternalExtentAndSharedLightingDepth) {
    const std::string source =
        ReadShader("src/luminumbra_client/rendering/passes/GlassOitPass.cpp");
    ASSERT_FALSE(source.empty());

    ExpectContains(source, "ctx.internal_w()");
    ExpectContains(source, "ctx.internal_h()");
    ExpectContains(source, "GL_RENDERBUFFER");
    ExpectContains(source, "ctx.lit_scene_depth.id");
    // Screen-size accessors must not leak into the internal-scene path.
    EXPECT_EQ(source.find("screen_width"), std::string::npos);
    EXPECT_EQ(source.find("screen_height"), std::string::npos);
    EXPECT_EQ(source.find("dest_width()"), std::string::npos);
    EXPECT_EQ(source.find("dest_height()"), std::string::npos);
}

TEST(GlassOitContract, DestroyedBeforeLightingDepthInEveryLifecyclePath) {
    const std::string source = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    ASSERT_FALSE(source.empty());

    const char* const functions[] = {
        "void RenderPipeline::on_resize(u32 new_width, u32 new_height)",
        "void RenderPipeline::set_render_scale(float scale)",
        "void RenderPipeline::cleanup_gpu_resources()",
    };
    for (const char* signature : functions) {
        SCOPED_TRACE(signature);
        const std::string body = ExtractBlock(source, signature);
        ASSERT_FALSE(body.empty());
        const std::size_t destroy_pos = body.find("m_glass_oit_pass->destroy()");
        const std::size_t depth_pos = body.find("m_lighting_pass->destroy_lighting_fbo(");
        ASSERT_NE(destroy_pos, std::string::npos);
        ASSERT_NE(depth_pos, std::string::npos);
        EXPECT_LT(destroy_pos, depth_pos);
    }
}

TEST(FarWorldShadowContract, SignedTagIsLadderOnlyAndAfterAllMaterialPaths) {
    const auto source = ReadShader("res/shaders/g_buffer.frag");
    ASSERT_FALSE(source.empty());
    const auto body = ExtractBlock(source, "void main()");
    ExpectContains(body,
                   "gMetallicAO = vec2(metallic, ao);\n"
                   "#ifdef LUMINUMBRA_FAR_WORLD_LADDER");
    ExpectContains(body,
                   "if (u_farWorldLadder == 1)\n"
                   "        gMetallicAO.g = -(1.0 + ao);\n#endif");
    const auto tag = body.find("gMetallicAO.g = -(1.0 + ao);");
    ASSERT_NE(tag, std::string::npos);
    EXPECT_LT(body.find("case 200u:"), tag);
    EXPECT_EQ(body.find("return;"), std::string::npos);
    EXPECT_EQ(body.find("discard;", tag), std::string::npos);
    // Full byte material ID and the marker lane retain their existing writes.
    ExpectContains(body, "float material_id_normalized = float(fs_in.MaterialID) / 255.0;");
    ExpectContains(
        body, "gNormalMaterial = vec4(encoded_normal * 0.5 + 0.5, 0.0, material_id_normalized);");
    const auto marker = ReadShader("res/shaders/plant_procgen.frag");
    ExpectContains(marker, "gNormalMaterial");
    EXPECT_EQ(marker.find("LUMINUMBRA_FAR_WORLD_LADDER"), std::string::npos);
}

TEST(FarWorldShadowContract, NegativeAOBypassesOnlyTheThreeCascadeLookups) {
    const auto source = ReadShader("res/shaders/lighting_pass.frag");
    const auto body = ExtractBlock(source, "void main()");
    ASSERT_FALSE(body.empty());
    ExpectContains(body, "bool isFarSurface = matData.g < 0.0;");
    ExpectContains(body, "float materialAO = isFarSurface ? -matData.g - 1.0 : matData.g;");
    ExpectContains(body, "if (authored) ao *= clamp(materialAO, 0.0, 1.0);");
    // GLSL conditionals evaluate only the selected operand: far fragments never
    // call the cascade samplers. The untagged operands are the original calls.
    ExpectContains(body,
                   "float shadow = (isFarSurface || u_staticStudio != 0) ? 1.0 : "
                   "CalculateShadow(FragPos, Normal, L_sun, abs(viewPos.z));");
    ExpectContains(body,
                   "vec3 shadowTint = isFarSurface ? vec3(1.0) : "
                   "SampleShadowTint(FragPos, abs(viewPos.z));");
    ExpectContains(body,
                   "float moonShadow = isFarSurface ? 1.0 : "
                   "CalculateShadow(FragPos, Normal, L_moon, abs(viewPos.z));");
    ExpectContains(body, "if (!isFarSurface) moonShadow = mix(moonShadowFloor, 1.0, moonShadow);");
    EXPECT_EQ(CountOccurrences(body, "CalculateShadow("), 2u);
    EXPECT_EQ(CountOccurrences(body, "SampleShadowTint("), 1u);
    ExpectContains(body, "uint MaterialID = uint(round(normalData.a * 255.0));");
    ExpectContains(body, "float markerEmissive = normalData.b;");
    ExpectContains(body, "float cloud_shadow = cloudShadow(FragPos);");
    ExpectContains(body, "* shadow * (1.0 - cloud_shadow) * shadowTint;");
    ExpectContains(body, "texture(u_ssao, TexCoords).r");
    ExpectContains(body, "skyVis = computeSkyVisibility(viewPos, viewNormal);");
}

TEST(FarWorldShadowContract, ProductionCascadesHaveOnlyLiveAndGlassInputs) {
    const auto input = ReadShader("src/luminumbra_client/rendering/RenderInputs.h");
    const auto shadow_input = ExtractBlock(input, "struct ShadowPassInput");
    ASSERT_FALSE(shadow_input.empty());
    ExpectContains(shadow_input, "SubmitTerrainChunksFn submit_terrain;");
    EXPECT_EQ(shadow_input.find("FarWorld"), std::string::npos);
    EXPECT_EQ(shadow_input.find("FarLod"), std::string::npos);
    const auto pipeline = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    const auto stage = ExtractBlock(pipeline, "void RenderPipeline::execute_stage_shadow(");
    ASSERT_FALSE(stage.empty());
    ExpectContains(stage, "shadow_input.submit_terrain = make_terrain_submitter();");
    EXPECT_FALSE(HasFarSubmissionAccess(stage));
    // Mutation controls close the reviewed hole: a direct owner Submit or draw
    // added inside this stage must trip the very same source-contract predicate.
    const auto before_closing_brace = stage.substr(0, stage.size() - 1);
    EXPECT_TRUE(HasFarSubmissionAccess(before_closing_brace +
                                       "\nm_far_world -> Submit(camera, prepare);\n}"));
    EXPECT_TRUE(HasFarSubmissionAccess(before_closing_brace +
                                       "\nDrawFarVolumeGpu(allocation, driver);\n}"));
    const auto live =
        ExtractBlock(pipeline, "SubmitTerrainChunksFn RenderPipeline::make_terrain_submitter(");
    ExpectContains(live, "CullHierarchical");
    ExpectContains(live, "draw_chunks_mdi");
    EXPECT_FALSE(HasFarSubmissionAccess(live));
    const auto pass = ReadShader("src/luminumbra_client/rendering/passes/ShadowPass.cpp");
    ExpectContains(pass, "i < ShadowMap::CASCADE_COUNT");
    ExpectContains(pass, "input.submit_terrain(cascade_planes)");
    EXPECT_FALSE(HasFarSubmissionAccess(pass));
}

} // namespace
