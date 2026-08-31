#include <gtest/gtest.h>

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

} // namespace
