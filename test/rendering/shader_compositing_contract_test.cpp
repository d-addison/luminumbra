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

} // namespace
