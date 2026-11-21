#include "rendering/AttributedGl.h"
#include "rendering/Camera.h"
#include "rendering/DiagnosticDrawTable.h"
#include "rendering/GBufferTargets.h"
#include "rendering/RenderContext.h"
#include "rendering/RenderResourceRegistry.h"
#include "rendering/Shader.h"
#include "rendering/passes/DebugViewPass.h"
#include "rendering/passes/LightingPass.h"
#include <glad/glad.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
using Pass = DebugViewPass;

std::string Source(std::string_view path) {
    // Match the existing source contracts: native text mode normalizes Windows checkout CRLF.
    std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / path);
    EXPECT_TRUE(input.good()) << path;
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::uint64_t Hash(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : text) {
        hash = (hash ^ static_cast<unsigned char>(character)) * 1099511628211ULL;
    }
    return hash;
}

std::string CaptureOff(std::string_view text,
                       std::string_view guard = "#ifdef LUMIN_DIAG_CONTRIBUTIONS\n") {
    // Preserve every byte outside additive, unnested contribution guards, including blank lines.
    std::string result;
    std::size_t position = 0;
    while (position < text.size()) {
        const auto begin = text.find(guard, position);
        if (begin == std::string_view::npos) {
            result += text.substr(position);
            break;
        }
        result += text.substr(position, begin - position);
        const auto end = text.find("#endif\n", begin);
        EXPECT_NE(end, std::string_view::npos);
        if (end == std::string_view::npos) {
            return {};
        }
        const auto body = text.substr(begin + guard.size(), end - begin - guard.size());
        EXPECT_EQ(body.find("#if"), std::string_view::npos);
        EXPECT_EQ(body.find("#else"), std::string_view::npos);
        position = end + std::string_view("#endif\n").size();
    }
    return result;
}

class DiagnosticContributionsGl : public ::testing::Test {
protected:
    void SetUp() override {
        initialized_ = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(initialized_);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window_ = glfwCreateWindow(8, 8, "contributions", nullptr, nullptr);
        ASSERT_NE(window_, nullptr);
        glfwMakeContextCurrent(window_);
        ASSERT_NE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)), 0);
    }
    void TearDown() override {
        Pass::controls() = {};
        if (window_) {
            glfwDestroyWindow(window_);
        }
        if (initialized_) {
            glfwTerminate();
        }
    }

private:
    bool initialized_ = false;
    GLFWwindow* window_ = nullptr;
};
void CheckShadowFootprintPositionValidity(Shader& shader,
                                          bool ladder,
                                          const std::array<GLuint, 9>& textures) {
    const auto draw = [&](int mode) {
        shader.setVec4("u_cascadeSplits", glm::vec4(1, 2, 3, 4));
        shader.setInt("u_contributionMode", mode);
        AttributedGl::Submit(glDrawArrays, static_cast<GLenum>(GL_TRIANGLE_STRIP), 0, 4);
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        std::array<float, 16> result{};
        glReadPixels(0, 0, 2, 2, GL_RGBA, GL_FLOAT, result.data());
        return result;
    };
    constexpr std::array<float, 4> regular = {0.5f, 1, 0, 0};
    glClearTexImage(textures[3], 0, GL_RGBA, GL_FLOAT, regular.data());
    glm::mat4 footprint_light(1);
    footprint_light[2][2] = 0;
    shader.setMat4("u_lightSpaceMatrices[0]", footprint_light);
    // All 16 validity masks cover single invalid lanes (including diagonals),
    // rows, columns, mixed lanes, an empty quad and the fully valid control.
    // Both position backends reject cleared/nonfinite storage; Ladder also
    // rejects cleared depth even when the stored position still looks valid.
    const int invalid_kinds = ladder ? 4 : 3;
    for (int invalid_kind = 0; invalid_kind < invalid_kinds; ++invalid_kind) {
        for (unsigned int valid_mask = 0; valid_mask < 16; ++valid_mask) {
            SCOPED_TRACE(::testing::Message() << (ladder) << invalid_kind << valid_mask);
            std::array<float, 16> positions{};
            std::array<float, 16> depths{};
            for (std::size_t pixel = 0; pixel < 4; ++pixel) {
                positions[4 * pixel] = 0.02f * static_cast<float>(pixel % 2);
                positions[4 * pixel + 1] = 0.02f * static_cast<float>(pixel / 2);
                positions[4 * pixel + 2] = -1;
                positions[4 * pixel + 3] = 1;
                depths[4 * pixel] = 0.5f;
                if ((valid_mask & (1U << pixel)) == 0) {
                    if (invalid_kind == 3) {
                        depths[4 * pixel] = 0;
                    } else {
                        const float invalid =
                            invalid_kind == 0
                                ? 0.0f
                                : (invalid_kind == 1 ? std::numeric_limits<float>::quiet_NaN()
                                                     : std::numeric_limits<float>::infinity());
                        positions[4 * pixel] = invalid;
                        positions[4 * pixel + 1] = invalid;
                        positions[4 * pixel + 2] = invalid;
                    }
                }
            }
            glTextureSubImage2D(textures[0], 0, 0, 0, 2, 2, GL_RGBA, GL_FLOAT, positions.data());
            glTextureSubImage2D(textures[4], 0, 0, 0, 2, 2, GL_RGBA, GL_FLOAT, depths.data());
            const auto footprint = draw(Pass::ShadowFootprint);
            const auto cascades = draw(Pass::ShadowCascade);
            // Frustum status must not overwrite an invalid-quad sentinel.
            footprint_light[3][0] = 10;
            shader.setMat4("u_lightSpaceMatrices[0]", footprint_light);
            const auto outside = draw(Pass::ShadowFootprint);
            footprint_light[3][0] = 0;
            shader.setMat4("u_lightSpaceMatrices[0]", footprint_light);
            for (std::size_t pixel = 0; pixel < 4; ++pixel) {
                if ((valid_mask & (1U << pixel)) != 0) {
                    const float outside_sentinel = valid_mask == 15 ? -1.0f : -2.0f;
                    EXPECT_EQ((std::array<float, 4>{outside[4 * pixel],
                                                    outside[4 * pixel + 1],
                                                    outside[4 * pixel + 2],
                                                    outside[4 * pixel + 3]}),
                              (std::array<float, 4>{
                                  outside_sentinel, outside_sentinel, outside_sentinel, 1}));
                    EXPECT_FLOAT_EQ(footprint[4 * pixel + 3], 1);
                    EXPECT_FLOAT_EQ(cascades[4 * pixel], 0);
                    if (valid_mask == 15) {
                        EXPECT_TRUE(std::isfinite(footprint[4 * pixel + 2]));
                        EXPECT_GE(footprint[4 * pixel], 0);
                        EXPECT_GE(footprint[4 * pixel + 1], 0);
                    } else {
                        EXPECT_EQ((std::array<float, 3>{footprint[4 * pixel],
                                                        footprint[4 * pixel + 1],
                                                        footprint[4 * pixel + 2]}),
                                  (std::array<float, 3>{-2, -2, -2}));
                    }
                }
            }
            if (valid_mask == 0 && (invalid_kind == 0 || invalid_kind == 3)) {
                for (std::size_t pixel = 0; pixel < 4; ++pixel) {
                    EXPECT_FLOAT_EQ(footprint[4 * pixel + 3], 0);
                }
            }
        }
    }
}

} // namespace

TEST(DiagnosticContributions, CatalogueLegendsAndCycleCoverEveryNamedTermAndUnsupportedPlane) {
    auto& control = Pass::controls();
    control = {};
    control.enabled = true;
    int legacy = Pass::Position;
    for (int mode = Pass::DrawId; mode <= Pass::Wireframe; ++mode) {
        Pass::cycle_mode(legacy);
        EXPECT_EQ(control.view, mode);
        EXPECT_EQ(legacy, Pass::None);
        EXPECT_EQ(
            Pass::identity_mode(Pass::identity_mode_names[static_cast<std::size_t>(mode - 5)]),
            mode);
        if (Pass::contribution_mode(mode)) {
            EXPECT_FALSE(Pass::contribution_legend(mode).empty());
            EXPECT_EQ(Pass::contribution_legend(mode).find("unsupported") != std::string_view::npos,
                      !Pass::supported_contribution(mode));
        }
    }
    Pass::cycle_mode(legacy);
    EXPECT_EQ(control.view, Pass::None);
    for (const int invalid : {-1, 0, 11, 30, std::numeric_limits<int>::max()}) {
        EXPECT_FALSE(Pass::supported_contribution(invalid));
    }
    EXPECT_EQ(Pass::identity_mode("direct-sun"), Pass::DirectSun);
    EXPECT_EQ(Pass::identity_mode("fog-atmosphere"), Pass::Fog);
    EXPECT_EQ(Pass::identity_mode("normal-world"), Pass::NormalWorld);
    EXPECT_EQ(Pass::identity_mode("unknown-term"), Pass::None);
    control = {};
}

TEST(DiagnosticContributions, ExtentsAreValidatedBeforeAllocationWithoutOverflow) {
    EXPECT_TRUE(Pass::contribution_extent(4096, 4096));
    EXPECT_TRUE(Pass::contribution_extent(16384, 1024));
    for (const auto& extent :
         std::array<std::array<unsigned int, 2>, 7>{{{0, 1},
                                                     {1, 0},
                                                     {16385, 1},
                                                     {1, 16385},
                                                     {4096, 4097},
                                                     {16384, 16384},
                                                     {std::numeric_limits<unsigned int>::max(),
                                                      std::numeric_limits<unsigned int>::max()}}}) {
        EXPECT_FALSE(Pass::contribution_extent(extent[0], extent[1]));
    }
}

TEST(DiagnosticContributions, LiveTermsNeverRequestIdentityReplayOrInspectorReadback) {
    auto& control = Pass::controls();
    control = {};
    Pass pass;
    for (int mode = Pass::DirectSun; mode <= Pass::ShadowPlane; ++mode) {
        control.enabled = true;
        control.refresh = true;
        control.inspector_open = true;
        control.view = mode;
        EXPECT_EQ(pass.mode(), mode);
        EXPECT_FALSE(pass.begin_frame(RenderContext{}));
        EXPECT_FALSE(control.refresh);
        EXPECT_TRUE(control.result.empty());
    }
    control.enabled = false;
    EXPECT_EQ(pass.mode(), Pass::None);
    control = {};
}

TEST(DiagnosticContributions, CaptureOffLightingAndGBufferKeepEveryOriginalByte) {
    const auto lighting = Source("res/shaders/lighting_pass.frag");
    EXPECT_NE(lighting.find("#ifdef LUMIN_DIAG_CONTRIBUTIONS\n"), std::string::npos);
    EXPECT_EQ(Hash(CaptureOff(lighting)), 0xc6e9bc9c6fc7110bULL);
    // Remove only additive instrumentation, retaining the original, fixed off-byte oracle.
    EXPECT_EQ(Hash(CaptureOff(Source("res/shaders/g_buffer.frag"), "#ifdef LUMIN_DIAG_SURFACE\n")),
              0x7289f9525f762361ULL);
}

TEST(DiagnosticContributions, UnsupportedAndStaleInputsAreHatchedWithoutSamplingGuessedPlanes) {
    const auto shader = Source("res/shaders/debug_view.frag");
    EXPECT_NE(shader.find("u_contributionReady != 0"), std::string::npos);
    EXPECT_NE(shader.find("contribution.a == 1.0"), std::string::npos);
    EXPECT_NE(shader.find("texelFetch(gContribution, pixel, 0)"), std::string::npos);
    const auto pass = Source("src/luminumbra_client/rendering/passes/DebugViewPass.cpp");
    for (const auto* check : {"control.contribution_view == mode()",
                              "!ctx.frozen_replay",
                              "control.contribution_size.width == ctx.internal_w()",
                              "control.contribution_size.height == ctx.internal_h()"}) {
        EXPECT_NE(pass.find(check), std::string::npos) << check;
    }
    EXPECT_NE(pass.find("overlay shader unavailable: solid magenta error sentinel"),
              std::string::npos);
    const auto lighting = Source("src/luminumbra_client/rendering/passes/LightingPass.cpp");
    for (const auto* check : {"texture == 0 || framebuffer_ == 0",
                              "occupied != GL_NONE",
                              "GL_MAX_TEXTURE_SIZE",
                              "GL_MAX_DRAW_BUFFERS",
                              "glCheckFramebufferStatus",
                              "glDrawBuffers(count_, buffers_.data())",
                              "glColorMaski(1, mask_[0]",
                              "contribution.detach()"}) {
        EXPECT_NE(lighting.find(check), std::string::npos) << check;
    }
    EXPECT_NE(lighting.find("m_contribution_source_program != m_lighting_shader->Id()"),
              std::string::npos);
    EXPECT_LT(lighting.find("contribution_extent(ctx.internal_w()"),
              lighting.find("glTexStorage2D"));
}

TEST_F(DiagnosticContributionsGl,
       LightingVariantsIsolateMaterialEmissionAndLinearLightWithoutChangingColor) {
    const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
    const auto vertex = (root / "lighting_pass.vert").string();
    const auto fragment = (root / "lighting_pass.frag").string();
    Shader normal(vertex.c_str(), fragment.c_str());
    Shader diagnostic(vertex.c_str(), fragment.c_str(), {}, {"LUMIN_DIAG_CONTRIBUTIONS 1"});
    Shader ladder(vertex.c_str(),
                  fragment.c_str(),
                  {},
                  {"LUMIN_DIAG_CONTRIBUTIONS 1", "LUMINUMBRA_FAR_WORLD_LADDER 1"});
    ASSERT_TRUE(normal.IsValid());
    ASSERT_TRUE(diagnostic.IsValid());
    ASSERT_TRUE(ladder.IsValid());
    EXPECT_EQ(glGetUniformLocation(normal.Id(), "u_contributionMode"), -1);
    EXPECT_GE(glGetUniformLocation(diagnostic.Id(), "u_contributionMode"), 0);

    // Fixed 1x1 nearest fixtures, no simulation or temporal inputs.
    std::array<GLuint, 9> textures{};
    glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
    const std::array<std::array<float, 4>, 7> data = {{{0, 0, -2, 1},
                                                       {0.5f, 0.5f, 0, 1.0f / 255},
                                                       {0.2f, 0.3f, 0.4f, 0.4f},
                                                       {0.25f, 0.6f, 0, 0},
                                                       {0.5f, 0, 0, 0},
                                                       {0, 0, 0, 0},
                                                       {0.1f, 0.2f, 0.3f, 1}}};
    for (std::size_t index = 0; index < textures.size(); ++index) {
        glBindTexture(GL_TEXTURE_2D, textures[index]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 1, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (index < data.size()) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, data[index].data());
        }
    }
    GLuint fbo = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    for (GLuint index = 0; index < 2; ++index) {
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, GL_TEXTURE_2D, textures[7 + index], 0);
    }
    constexpr std::array<GLenum, 2> outputs = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, outputs.data());
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    // lighting_pass.vert consumes position at location 0 and UV at location 1.
    constexpr std::array<float, 20> quad = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0,
                                            -1, 1,  0, 0, 1, 1, 1,  0, 1, 1};
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    for (GLuint unit = 0; unit < 7; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, textures[unit]);
    }
    bool cascade_case = false;
    bool outside_selected = false;
    bool outside_all = false;
    bool before_light_near = false;
    const auto configure = [&](Shader& shader) {
        shader.use();
        shader.setInt("gPosition", 0);
        shader.setInt("gNormalMaterial", 1);
        shader.setInt("gAlbedoRoughness", 2);
        shader.setInt("gMetallicAO", 3);
        shader.setInt("u_sceneDepth", 4);
        shader.setInt("u_environmentBrdf", 5);
        shader.setInt("u_authoredSurface", 6);
        shader.setInt("u_ssao", 4);
        shader.setInt("u_materialLUT", 5);
        shader.setInt("u_shadowCascades", 7);
        shader.setInt("u_shadowTintCascades", 7);
        shader.setInt("u_authoredEnabled", 1);
        shader.setInt("u_staticStudio", cascade_case ? 0 : 1);
        shader.setVec4("u_cascadeSplits", glm::vec4(1, 2, 3, 4));
        for (int index = 0; index < 4; ++index) {
            glm::mat4 light(1);
            light[0][0] = light[1][1] = 0.01f;
            light[2][2] = 0;
            light[3][2] = before_light_near ? -4.0f : 0.0f;
            light[3][0] = (outside_all || (outside_selected && index == 0))
                              ? 4.0f
                              : static_cast<float>(index) * 0.2f;
            shader.setMat4("u_lightSpaceMatrices[" + std::to_string(index) + "]", light);
        }
        shader.setMat4("u_inverseView", glm::mat4(1));
        shader.setVec3("u_viewPos", glm::vec3(0));
        shader.setVec3("u_sun.direction", glm::vec3(0, 0, -1));
        shader.setVec3("u_sun.color", glm::vec3(0.5f));
        shader.setVec3("u_skyAmbientColor", glm::vec3(0.4f));
    };
    glViewport(0, 0, 1, 1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    bool moon_case = false;
    bool point_case = false;
    bool rotated_case = false;
    const auto draw = [&](Shader& shader, int mode, GLenum attachment) {
        configure(shader);
        shader.setInt("u_contributionMode", mode);
        if (rotated_case) {
            glm::mat4 rotation(1);
            rotation[0] = glm::vec4(0, 0, -1, 0);
            rotation[2] = glm::vec4(1, 0, 0, 0);
            shader.setMat4("u_inverseView", rotation);
        }
        if (moon_case) {
            shader.setInt("u_staticStudio", 0);
            shader.setVec3("u_sun.color", glm::vec3(0));
            shader.setVec3("u_moonDir", glm::vec3(0, 0, -1));
            shader.setVec3("u_moonRadiance", glm::vec3(1));
            shader.setFloat("u_moonIllum", 1);
        }
        if (&shader == &ladder) {
            shader.setInt("u_farWorldLadder", 1);
            glm::mat4 inverse(1);
            inverse[2][2] = -1;
            shader.setMat4("u_gbufferInverseProjection", inverse);
        }
        shader.setInt("u_pointLightCount", point_case ? 1 : 0);
        if (point_case) {
            shader.setVec3("u_pointLights[0].position", glm::vec3(0));
            shader.setVec3("u_pointLights[0].color", glm::vec3(1));
            shader.setFloat("u_pointLights[0].radius", 4);
            shader.setFloat("u_pointLights[0].intensity", 2);
        }
        AttributedGl::Submit(glDrawArrays, static_cast<GLenum>(GL_TRIANGLE_STRIP), 0, 4);
        glReadBuffer(attachment);
        std::array<float, 4> result{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, result.data());
        return result;
    };
    const auto reference = draw(normal, 0, GL_COLOR_ATTACHMENT0);
    for (int mode = Pass::DirectSun; mode <= Pass::ShadowPlane; ++mode) {
        SCOPED_TRACE(mode);
        const auto value = draw(diagnostic, mode, GL_COLOR_ATTACHMENT1);
        EXPECT_EQ(value[3], Pass::supported_contribution(mode) ? 1.0f : 0.0f);
        EXPECT_EQ(draw(diagnostic, mode, GL_COLOR_ATTACHMENT0), reference);
    }
    for (const auto& [mode, expected] :
         std::array<std::pair<int, float>, 6>{{{Pass::Roughness, 0.4f},
                                               {Pass::Metallic, 0.25f},
                                               {Pass::Ao, 0.6f},
                                               {Pass::MaterialAo, 0.6f},
                                               {Pass::SkyVisibility, 1.0f},
                                               {Pass::ShadowFactor, 1.0f}}}) {
        const auto value = draw(diagnostic, mode, GL_COLOR_ATTACHMENT1);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            EXPECT_FLOAT_EQ(value[channel], expected);
        }
    }
    EXPECT_EQ(draw(diagnostic, Pass::Emissive, GL_COLOR_ATTACHMENT1), data[6]);
    EXPECT_EQ(draw(diagnostic, Pass::DirectMoon, GL_COLOR_ATTACHMENT1),
              (std::array<float, 4>{0, 0, 0, 1}));
    const std::array<float, 4> incident = {0.5f, 0.5f, 0.5f, 1};
    const auto irradiance = draw(diagnostic, Pass::Lighting, GL_COLOR_ATTACHMENT1);
    for (std::size_t channel = 0; channel < incident.size(); ++channel) {
        EXPECT_NEAR(irradiance[channel], incident[channel], 0.000001f);
    }
    // Incoming light is independent of albedo; the direct BRDF view changes.
    const auto original_sun = draw(diagnostic, Pass::DirectSun, GL_COLOR_ATTACHMENT1);
    const std::array<float, 4> changed_albedo = {0.8f, 0.1f, 0.7f, 0.4f};
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, textures[2]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, changed_albedo.data());
    EXPECT_EQ(draw(diagnostic, Pass::Lighting, GL_COLOR_ATTACHMENT1), irradiance);
    EXPECT_NE(draw(diagnostic, Pass::DirectSun, GL_COLOR_ATTACHMENT1), original_sun);
    EXPECT_EQ(draw(diagnostic, Pass::NormalView, GL_COLOR_ATTACHMENT1),
              (std::array<float, 4>{0.5f, 0.5f, 1, 1}));
    EXPECT_EQ(draw(diagnostic, Pass::NormalView, GL_COLOR_ATTACHMENT1),
              draw(diagnostic, Pass::NormalWorld, GL_COLOR_ATTACHMENT1));
    rotated_case = true;
    EXPECT_EQ(draw(diagnostic, Pass::NormalView, GL_COLOR_ATTACHMENT1),
              (std::array<float, 4>{0.5f, 0.5f, 1, 1}));
    EXPECT_EQ(draw(diagnostic, Pass::NormalWorld, GL_COLOR_ATTACHMENT1),
              (std::array<float, 4>{1, 0.5f, 0.5f, 1}));
    rotated_case = false;
    // Tagged far surfaces bypass cascade sampling; moon and point terms stay analytic.
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, textures[3]);
    const std::array<float, 4> far_material = {0.25f, -1.6f, 0, 0};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, far_material.data());
    moon_case = true;
    const auto moon = draw(diagnostic, Pass::DirectMoon, GL_COLOR_ATTACHMENT1);
    constexpr float pi = 3.14159265358979323846f;
    std::array<float, 3> moon_lit{};
    for (std::size_t channel = 0; channel < moon_lit.size(); ++channel) {
        moon_lit[channel] = changed_albedo[channel] * 0.12f * 0.85f + pi * 0.12f * 0.21f;
    }
    const float moon_luma = moon_lit[0] * 0.2126f + moon_lit[1] * 0.7152f + moon_lit[2] * 0.0722f;
    constexpr std::array<float, 3> lunar_tint = {0.70f, 0.80f, 1.05f};
    const auto lunar_irradiance = draw(diagnostic, Pass::Lighting, GL_COLOR_ATTACHMENT1);
    for (std::size_t channel = 0; channel < moon_lit.size(); ++channel) {
        EXPECT_NEAR(moon[channel],
                    moon_lit[channel] * 0.45f + moon_luma * lunar_tint[channel] * 0.55f,
                    0.000001f);
        EXPECT_NEAR(lunar_irradiance[channel], 0.102f, 0.000001f);
    }
    const auto lunar_specular = draw(diagnostic, Pass::Specular, GL_COLOR_ATTACHMENT1);
    point_case = true;
    const auto point_irradiance = draw(diagnostic, Pass::Lighting, GL_COLOR_ATTACHMENT1);
    const auto combined_specular = draw(diagnostic, Pass::Specular, GL_COLOR_ATTACHMENT1);
    for (std::size_t channel = 0; channel < moon_lit.size(); ++channel) {
        EXPECT_NEAR(point_irradiance[channel], 0.102f + 1.0f / pi, 0.000001f);
        EXPECT_GT(combined_specular[channel], lunar_specular[channel]);
    }
    // Production selection and diagnostic payload share the exact index and projection.
    moon_case = point_case = false;
    cascade_case = true;
    glTextureSubImage2D(textures[3], 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, data[3].data());
    GLuint shadows = 0;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &shadows);
    glTextureStorage3D(shadows, 1, GL_R32F, 16, 16, 4);
    glTextureParameteri(shadows, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(shadows, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    for (int layer = 0; layer < 4; ++layer) {
        const float stored = layer % 2 == 0 ? 0.4f : 0.6f;
        glClearTexSubImage(shadows, 0, 0, 0, layer, 16, 16, 1, GL_RED, GL_FLOAT, &stored);
    }
    glBindTextureUnit(7, shadows);
    constexpr std::array<std::pair<float, int>, 8> depths = {
        {{0.5f, 0}, {1, 0}, {1.0001f, 1}, {2, 1}, {2.0001f, 2}, {3, 2}, {3.0001f, 3}, {8, 3}}};
    for (const auto& [depth, selected] : depths) {
        SCOPED_TRACE(depth);
        const std::array<float, 4> position = {0, 0, -depth, 1};
        glTextureSubImage2D(textures[0], 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, position.data());
        for (Shader* variant : {&diagnostic, &ladder}) {
            const auto cascade = draw(*variant, Pass::ShadowCascade, GL_COLOR_ATTACHMENT1);
            EXPECT_FLOAT_EQ(cascade[0], static_cast<float>(selected));
            EXPECT_NEAR(cascade[1], 0.5f + static_cast<float>(selected) * 0.1f, 0.00001f);
            EXPECT_FLOAT_EQ(cascade[2], 0.5f);
            EXPECT_FLOAT_EQ(cascade[3], 1);
            const auto footprint = draw(*variant, Pass::ShadowFootprint, GL_COLOR_ATTACHMENT1);
            EXPECT_NEAR(footprint[0], cascade[1], 0.00001f);
            EXPECT_FLOAT_EQ(footprint[1], 0.5f);
            EXPECT_LT(footprint[2], -8); // constant 1x1 source: zero UV derivative
            EXPECT_EQ(draw(*variant, Pass::ShadowCascade, GL_COLOR_ATTACHMENT0),
                      draw(normal, Pass::None, GL_COLOR_ATTACHMENT0));
        }
        EXPECT_FLOAT_EQ(draw(normal, Pass::None, GL_COLOR_ATTACHMENT0)[3], 1);
        EXPECT_FLOAT_EQ(draw(diagnostic, Pass::ShadowFactor, GL_COLOR_ATTACHMENT1)[0],
                        selected % 2 == 0 ? 0.0f : 1.0f);
    }
    const std::array<float, 4> near_position = {0, 0, -0.5f, 1};
    glTextureSubImage2D(textures[0], 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, near_position.data());
    outside_selected = true;
    EXPECT_EQ(draw(diagnostic, Pass::ShadowCascade, GL_COLOR_ATTACHMENT1),
              (std::array<float, 4>{-1, -1, -1, 1}));
    outside_all = true;
    EXPECT_EQ(draw(diagnostic, Pass::ShadowCascade, GL_COLOR_ATTACHMENT1),
              (std::array<float, 4>{-3, -3, -3, 1}));
    outside_selected = outside_all = false;
    before_light_near = true;
    EXPECT_EQ(draw(diagnostic, Pass::ShadowCascade, GL_COLOR_ATTACHMENT1),
              (std::array<float, 4>{-3, -3, -3, 1}));
    cascade_case = false; // studio bypass must never invent a cascade
    EXPECT_EQ(draw(diagnostic, Pass::ShadowCascade, GL_COLOR_ATTACHMENT1),
              (std::array<float, 4>{-2, -2, -2, 1}));
    glDeleteTextures(1, &shadows);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
}

TEST_F(DiagnosticContributionsGl, ShadowFootprintStaysLocalAcrossSplitQuadsAndBypassedReceivers) {
    const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
    const auto vertex = (root / "lighting_pass.vert").string();
    const auto fragment = (root / "lighting_pass.frag").string();
    Shader diagnostic(vertex.c_str(), fragment.c_str(), {}, {"LUMIN_DIAG_CONTRIBUTIONS 1"});
    Shader ladder(vertex.c_str(),
                  fragment.c_str(),
                  {},
                  {"LUMIN_DIAG_CONTRIBUTIONS 1", "LUMINUMBRA_FAR_WORLD_LADDER 1"});
    ASSERT_TRUE(diagnostic.IsValid());
    ASSERT_TRUE(ladder.IsValid());

    // Nine fixed 2x2 RGBA32F textures: seven inputs and two outputs, one derivative quad.
    std::array<GLuint, 9> textures{};
    glCreateTextures(GL_TEXTURE_2D, static_cast<GLsizei>(textures.size()), textures.data());
    for (GLuint texture : textures) {
        glTextureStorage2D(texture, 1, GL_RGBA32F, 2, 2);
        glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        constexpr std::array<float, 4> value = {0.5f, 0.5f, 0.5f, 1};
        glClearTexImage(texture, 0, GL_RGBA, GL_FLOAT, value.data());
    }
    for (GLuint unit = 0; unit < 7; ++unit) {
        glBindTextureUnit(unit, textures[unit]);
    }
    GLuint shadows = 0;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &shadows);
    glTextureStorage3D(shadows, 1, GL_R32F, 64, 64, 4);
    glTextureParameteri(shadows, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(shadows, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    constexpr float stored_depth = 1;
    glClearTexImage(shadows, 0, GL_RED, GL_FLOAT, &stored_depth);
    glBindTextureUnit(7, shadows);
    GLuint fbo = 0;
    glCreateFramebuffers(1, &fbo);
    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, textures[7], 0);
    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT1, textures[8], 0);
    constexpr std::array<GLenum, 2> outputs = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glNamedFramebufferDrawBuffers(fbo, 2, outputs.data());
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    GLuint vao = 0;
    GLuint vbo = 0;
    constexpr std::array<float, 20> quad = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0,
                                            -1, 1,  0, 0, 1, 1, 1,  0, 1, 1};
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    glViewport(0, 0, 2, 2);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    const auto draw = [&](Shader& shader, const glm::vec4& splits, int mode) {
        shader.setVec4("u_cascadeSplits", splits);
        shader.setInt("u_contributionMode", mode);
        AttributedGl::Submit(glDrawArrays, static_cast<GLenum>(GL_TRIANGLE_STRIP), 0, 4);
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        std::array<float, 16> result{};
        glReadPixels(0, 0, 2, 2, GL_RGBA, GL_FLOAT, result.data());
        return result;
    };
    for (Shader* variant : {&diagnostic, &ladder}) {
        variant->use();
        variant->setInt("gPosition", 0);
        variant->setInt("gNormalMaterial", 1);
        variant->setInt("gAlbedoRoughness", 2);
        variant->setInt("gMetallicAO", 3);
        variant->setInt("u_sceneDepth", 4);
        variant->setInt("u_materialLUT", 5);
        variant->setInt("u_environmentBrdf", 5);
        variant->setInt("u_authoredSurface", 6);
        variant->setInt("u_shadowCascades", 7);
        variant->setInt("u_shadowTintCascades", 7);
        variant->setMat4("u_inverseView", glm::mat4(1));
        variant->setInt("u_farWorldLadder", variant == &ladder ? 1 : 0);
        glm::mat4 inverse_projection(1);
        inverse_projection[2][2] = -1;
        variant->setMat4("u_gbufferInverseProjection", inverse_projection);
        variant->setVec3("u_sun.direction", glm::vec3(0, 0, -1));
        variant->setVec3("u_moonDir", glm::vec3(0, 0, -1));
        for (bool perspective : {false, true}) {
            for (int index = 0; index < 4; ++index) {
                glm::mat4 light(1);
                light[0][0] = light[1][1] = 0.25f * static_cast<float>(index + 1);
                light[2][2] = 0;
                light[3][0] = 0.2f * static_cast<float>(index);
                light[0][3] = perspective ? 0.5f : 0.0f;
                light[1][3] = perspective ? 0.25f : 0.0f;
                variant->setMat4("u_lightSpaceMatrices[" + std::to_string(index) + "]", light);
            }
            for (int split = 1; split <= 3; ++split) {
                for (bool vertical : {false, true}) {
                    std::array<float, 16> positions{};
                    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
                        const std::size_t x = pixel % 2;
                        const std::size_t y = pixel / 2;
                        positions[4 * pixel] = 0.02f * static_cast<float>(x);
                        positions[4 * pixel + 1] = 0.02f * static_cast<float>(y);
                        positions[4 * pixel + 2] =
                            -static_cast<float>(split) + ((vertical ? y : x) == 0 ? 0.02f : -0.02f);
                        positions[4 * pixel + 3] = 1;
                    }
                    glTextureSubImage2D(
                        textures[0], 0, 0, 0, 2, 2, GL_RGBA, GL_FLOAT, positions.data());
                    for (bool night : {false, true}) {
                        SCOPED_TRACE(::testing::Message() << (variant == &ladder) << perspective
                                                          << split << vertical << night);
                        variant->setVec3("u_sun.color", glm::vec3(night ? 0.0f : 0.5f));
                        constexpr std::array<float, 4> regular = {0.5f, 1, 0, 0};
                        glClearTexImage(textures[3], 0, GL_RGBA, GL_FLOAT, regular.data());
                        // Force each neighbour's cascade uniformly for a single-cascade oracle.
                        glm::vec4 lower_splits(100);
                        glm::vec4 upper_splits(100);
                        for (int index = 0; index < split; ++index) {
                            upper_splits[index] = -100;
                            if (index < split - 1) {
                                lower_splits[index] = -100;
                            }
                        }
                        const auto lower_reference =
                            draw(*variant, lower_splits, Pass::ShadowFootprint);
                        const auto upper_reference =
                            draw(*variant, upper_splits, Pass::ShadowFootprint);
                        const auto actual =
                            draw(*variant, glm::vec4(1, 2, 3, 4), Pass::ShadowFootprint);
                        const auto cascades =
                            draw(*variant, glm::vec4(1, 2, 3, 4), Pass::ShadowCascade);
                        for (std::size_t pixel = 0; pixel < 4; ++pixel) {
                            const bool upper = (vertical ? pixel / 2 : pixel % 2) != 0;
                            const auto& reference = upper ? upper_reference : lower_reference;
                            EXPECT_FLOAT_EQ(cascades[4 * pixel],
                                            static_cast<float>(split - 1 + upper));
                            EXPECT_FLOAT_EQ(actual[4 * pixel + 3], 1);
                            const float ratio =
                                std::exp2(actual[4 * pixel + 2] - reference[4 * pixel + 2]);
                            EXPECT_GT(ratio, 0.8f);
                            EXPECT_LT(ratio, 1.25f);
                            EXPECT_GT(std::exp2(reference[4 * pixel + 2]), 0.1f);
                        }
                        // One lane of the same quad bypasses both shadow calls.
                        constexpr std::array<float, 4> bypassTexel = {0.5f, -2, 0, 0};
                        glTextureSubImage2D(
                            textures[3], 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, bypassTexel.data());
                        const auto mixed =
                            draw(*variant, glm::vec4(1, 2, 3, 4), Pass::ShadowFootprint);
                        EXPECT_EQ((std::array<float, 4>{mixed[0], mixed[1], mixed[2], mixed[3]}),
                                  (std::array<float, 4>{-2, -2, -2, 1}));
                        for (std::size_t pixel = 1; pixel < 4; ++pixel) {
                            EXPECT_NEAR(mixed[4 * pixel + 2], actual[4 * pixel + 2], 0.0001f);
                        }
                    }
                }
            }
        }
        CheckShadowFootprintPositionValidity(*variant, variant == &ladder, textures);
    }
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &shadows);
    glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
}

TEST_F(DiagnosticContributionsGl, OverlayUsesHatchingForUnsupportedMissingResizedAndFrozenPlanes) {
    Pass pass;
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    pass.init_buffers();
    std::array<GLuint, 2> textures{};
    glGenTextures(2, textures.data());
    for (const GLuint texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 1, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    const std::array<float, 4> material = {0.25f, 0.25f, 0.25f, 1};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, material.data());
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, 1, 1);
    RenderContext ctx;
    ctx.screen_width = ctx.screen_height = 1;
    auto& control = Pass::controls();
    control = {};
    control.enabled = true;
    control.view = Pass::Roughness;
    control.contribution_view = control.view;
    control.contribution_texture = textures[1];
    control.contribution_size = {1, 1};
    const auto draw = [&] {
        pass.execute(ctx);
        std::array<float, 4> pixel{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, pixel.data());
        return pixel;
    };
    EXPECT_EQ(draw(), material);
    const std::array<float, 4> hatch = {1, 0, 1, 1};
    for (const int mode : {Pass::Fog, Pass::ForwardPlane, Pass::SkyPlane, Pass::ShadowPlane}) {
        control.view = mode;
        control.contribution_view = mode;
        EXPECT_EQ(draw(), hatch);
        EXPECT_NE(control.status.find("unsupported"), std::string::npos);
    }
    control.view = Pass::Roughness;
    control.contribution_view = Pass::Metallic;
    EXPECT_EQ(draw(), hatch);
    control.contribution_view = control.view;
    control.contribution_size.width = 2;
    EXPECT_EQ(draw(), hatch);
    control.contribution_size = {1, 1};
    ctx.frozen_replay = true;
    EXPECT_EQ(draw(), hatch);
    ctx.frozen_replay = false;
    control.contribution_texture = 0;
    EXPECT_EQ(draw(), hatch);
    control.contribution_texture = textures[1];
    auto unsupported = material;
    unsupported[3] = 0;
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, unsupported.data());
    EXPECT_EQ(draw(), hatch);
    auto nonfinite = material;
    nonfinite[0] = std::numeric_limits<float>::infinity();
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, nonfinite.data());
    EXPECT_EQ(draw(), (std::array<float, 4>{0, 1, 1, 1}));
    nonfinite[0] = std::numeric_limits<float>::quiet_NaN();
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, nonfinite.data());
    EXPECT_EQ(draw(), hatch);
    // The existing renderer toggle also needs inspector storage with diagnostics closed.
    control.enabled = false;
    control.wireframe = true;
    EXPECT_EQ(draw(), hatch);
    control.wireframe = false;
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    pass.destroy_buffers();
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(2, textures.data());
}

TEST_F(DiagnosticContributionsGl,
       LightingTargetRestoresStateAndRefusesOccupiedOrUnsupportedRequests) {
    RenderResourceRegistry registry;
    LightingPass lighting;
    lighting.init_shader(LUMINUMBRA_SOURCE_ROOT);
    Camera camera;
    const std::vector<glm::mat4> matrices(4, glm::mat4(1));
    RenderContext ctx;
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    ctx.screen_quad_vao = vao;
    ctx.camera = &camera;
    ctx.screen_width = ctx.screen_height = 8;
    ctx.light_space_matrices = &matrices;
    auto& control = Pass::controls();
    control = {};
    control.enabled = true;
    control.view = Pass::Roughness;
    // No diagnostic attachment can be made to the default / absent lighting FBO.
    lighting.execute(ctx);
    EXPECT_EQ(control.contribution_texture, 0U);
    lighting.init_lighting_fbo(registry, 8, 8);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lighting.lighting_fbo().fbo_id);
    glColorMaski(1, GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    glEnablei(GL_BLEND, 1);
    lighting.execute(ctx);
    ASSERT_NE(control.contribution_texture, 0U);
    const GLuint retained = control.contribution_texture;
    EXPECT_EQ(control.contribution_view, Pass::Roughness);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lighting.lighting_fbo().fbo_id);
    GLint attachment = -1;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                          GL_COLOR_ATTACHMENT1,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                          &attachment);
    EXPECT_EQ(attachment, GL_NONE);
    std::array<GLboolean, 4> mask{};
    glGetBooleani_v(GL_COLOR_WRITEMASK, 1, mask.data());
    EXPECT_EQ(mask, (std::array<GLboolean, 4>{GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE}));
    EXPECT_EQ(glIsEnabledi(GL_BLEND, 1), GL_TRUE);
    GLint buffer = -1;
    glGetIntegerv(GL_DRAW_BUFFER1, &buffer);
    EXPECT_EQ(buffer, GL_NONE);
    // A competing owner is never overwritten, even when the extent is otherwise valid.
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, retained, 0);
    lighting.execute(ctx);
    EXPECT_EQ(control.contribution_texture, 0U);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lighting.lighting_fbo().fbo_id);
    GLint occupied = 0;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                          GL_COLOR_ATTACHMENT1,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                          &occupied);
    EXPECT_EQ(static_cast<GLuint>(occupied), retained);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
    for (const int mode : {Pass::Fog, Pass::ForwardPlane, Pass::SkyPlane, Pass::ShadowPlane}) {
        control.view = mode;
        lighting.execute(ctx);
        EXPECT_EQ(control.contribution_texture, 0U);
    }
    control.view = Pass::Roughness;
    ctx.frozen_replay = true;
    lighting.execute(ctx);
    EXPECT_EQ(control.contribution_texture, 0U);
    ctx.frozen_replay = false;
    control.enabled = false;
    lighting.execute(ctx);
    EXPECT_EQ(control.contribution_texture, 0U);
    control.enabled = true;
    lighting.execute(ctx);
    EXPECT_EQ(control.contribution_texture, retained);
    lighting.reset_shader();
    EXPECT_EQ(control.contribution_texture, 0U);
    EXPECT_EQ(glIsTexture(retained), GL_FALSE);
    lighting.destroy_lighting_fbo(registry);
    glDeleteVertexArrays(1, &vao);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST(DiagnosticContributions, SurfaceCatalogueLabelsAndWeightBoundariesAreGlFree) {
    auto& control = Pass::controls();
    control = {};
    Pass pass;
    constexpr std::array<const char*, 4> names = {
        "actual-mip", "texel-density", "triplanar-weights", "raw-finite"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const int mode = Pass::ActualMip + static_cast<int>(index);
        EXPECT_EQ(Pass::identity_mode(names[index]), mode);
        EXPECT_TRUE(DiagnosticContributions::surface_mode(mode));
        EXPECT_FALSE(Pass::contribution_mode(mode));
        EXPECT_FALSE(Pass::supported_contribution(mode));
        EXPECT_FALSE(Pass::contribution_legend(mode).empty());
        control.enabled = true;
        control.view = mode;
        control.refresh = true;
        EXPECT_FALSE(pass.begin_frame(RenderContext{}));
        EXPECT_FALSE(control.refresh);
        EXPECT_EQ(pass.mode(), mode);
        control.enabled = false;
        EXPECT_EQ(pass.mode(), Pass::None);
    }
    EXPECT_NE(Pass::contribution_legend(Pass::ActualMip).find("black=0, white=16"),
              std::string_view::npos);
    EXPECT_NE(Pass::contribution_legend(Pass::TexelDensity).find("texels/pixel"),
              std::string_view::npos);
    EXPECT_NE(Pass::contribution_legend(Pass::RawFinite).find("before"), std::string_view::npos);
    for (const int invalid : {-1, 0, 29, 34, std::numeric_limits<int>::max()}) {
        EXPECT_FALSE(DiagnosticContributions::surface_mode(invalid));
    }
    for (const auto& weights : std::array<std::array<float, 3>, 4>{
             {{1, 0, 0}, {0.25f, 0.5f, 0.25f}, {0.9995f, 0, 0}, {1, 0.0005f, 0}}}) {
        EXPECT_TRUE(DiagnosticContributions::normalized_weights(weights));
    }
    for (const auto& weights :
         std::array<std::array<float, 3>, 7>{{{0, 0, 0},
                                              {1, 1, 1},
                                              {-0.1f, 0.1f, 1},
                                              {1.0001f, 0, 0},
                                              {0.998f, 0, 0},
                                              {std::numeric_limits<float>::quiet_NaN(), 0, 1},
                                              {0, std::numeric_limits<float>::infinity(), 1}}}) {
        EXPECT_FALSE(DiagnosticContributions::normalized_weights(weights));
    }
    control = {};
}

TEST(DiagnosticContributions, SurfaceLodUsesTheProductionSamplerGradientsAndBias) {
    const auto shader = Source("res/shaders/g_buffer.frag");
    for (const auto* contract :
         {"diagnosticFootprint(image, dx * biasScale, dy * biasScale);",
          "textureGrad(image, uvw, dx * biasScale, dy * biasScale)",
          "diagnosticFootprint(s, dFdx(uvw.xy) * exp2(u_lodBias), dFdy(uvw.xy) * exp2(u_lodBias));",
          "vec2 size = vec2(textureSize(image, 0).xy);",
          "float rho = max(length(dx * size), length(dy * size));",
          "float lambda = log2(max(rho, 1e-8));",
          "textureQueryLod(image, projected.zy).x",
          "textureQueryLod(image, projected.xz).x",
          "textureQueryLod(image, projected.xy).x",
          "diagnosticRecordMip(diagnosticTerrainMips[materialId >= 1u && materialId <= 5u ? "
          "int(materialId) : 0]);",
          "diagnosticRecordMip(diagnosticSandMip);",
          "diagnosticRecordMip(diagnosticUvMips.x);",
          "diagnosticRecordMip(diagnosticUvMips.z);",
          "diagnosticDensity = max(diagnosticDensity, lambda);"}) {
        EXPECT_NE(shader.find(contract), std::string::npos) << contract;
    }
    EXPECT_LT(shader.find("    diagnosticQueryMips();"), shader.find("            discard;"));
    const auto pass = Source("src/luminumbra_client/rendering/passes/GBufferPass.cpp");
    std::size_t position = 0;
    for (int sibling = 0; sibling < 3; ++sibling) {
        position = pass.find("shader = DebugViewPass::surface_variant(", position);
        ASSERT_NE(position, std::string::npos);
        ++position;
    }
    EXPECT_EQ(pass.find("shader = DebugViewPass::surface_variant(", position), std::string::npos);
}

TEST(DiagnosticContributions, SurfaceWeightsObserveTheProductionResultAndRejectViolations) {
    const auto shader = Source("res/shaders/g_buffer.frag");
    for (const auto* contract :
         {"return w / max(w.x + w.y + w.z, 1e-4);",
          "vec3 weights = triplanar_weights(u_macroRockOverlay == 1 ? surfaceN : worldN);",
          "diagnosticWeights = weights;",
          "any(lessThan(weights, vec3(0.0)))",
          "any(greaterThan(weights, vec3(1.0)))",
          "abs(dot(weights, vec3(1.0)) - 1.0) > 0.001",
          "diagnosticBadWeights && u_surfaceMode == 32"}) {
        EXPECT_NE(shader.find(contract), std::string::npos) << contract;
    }
}

TEST(DiagnosticContributions, SurfaceFiniteChecksPrecedeStorageAndCoverEveryMaterialSampler) {
    const auto shader = Source("res/shaders/g_buffer.frag");
    for (const auto* contract :
         {"diagnosticNonfinite || any(isnan(value)) || any(isinf(value))",
          "diagnosticCheck(textureGrad(image, uvw, dx * biasScale, dy * biasScale));",
          "diagnosticCheck(u_lodBias != 0.0 ? texture(s, uvw, u_lodBias) : texture(s, uvw));",
          "diagnosticCheck(matProps);",
          "diagnosticCheck(texInfo);",
          "diagnosticCheck(rockProps);",
          "diagnosticCheck(rockInfo);",
          "diagnosticCheck(vec4(albedoScale, albedoTint));",
          "diagnosticCheck(vec4(rockAlbedoScale, rockAlbedoTint));",
          "diagnosticCheck(vec4(bx, 0.0));",
          "diagnosticCheck(vec4(by, 0.0));",
          "diagnosticCheck(vec4(bz, 0.0));",
          "diagnosticCheck(vec4(gPosition, 0.0));",
          "diagnosticCheck(gNormalMaterial);",
          "diagnosticCheck(gAlbedoRoughness);",
          "diagnosticCheck(vec4(gMetallicAO, gMotionVector));",
          "diagnosticCheck(gAuthoredSurface);",
          "if (diagnosticNonfinite) diagnosticValue = vec3(-1.0, 0.0, -1.0);"}) {
        EXPECT_NE(shader.find(contract), std::string::npos) << contract;
    }
    EXPECT_LT(shader.rfind("diagnosticCheck("),
              shader.find("gDiagnosticSurface = vec4(diagnosticValue"));
}

TEST(DiagnosticContributions,
     SurfaceOffGuardPrecedesAllGlWorkAndPreservesSubmissionAndAttachmentContracts) {
    const auto pass = Source("src/luminumbra_client/rendering/passes/DebugViewPass.cpp");
    const auto begin = pass.find("DebugViewPass::SurfaceFrame::SurfaceFrame(");
    ASSERT_NE(begin, std::string::npos);
    const auto end = pass.find("DebugViewPass::SurfaceFrame::~SurfaceFrame", begin);
    ASSERT_NE(end, std::string::npos);
    const auto body = pass.substr(begin, end - begin);
    EXPECT_LT(body.find("!geometry_requested()"), body.find("std::make_unique<SurfaceState>"));
    EXPECT_LT(body.find("ctx.frozen_replay"), body.find("->attach(ctx, gbuffer)"));
    EXPECT_LT(body.find("GBufferDiagnosticCapture::Current()"),
              body.find("->attach(ctx, gbuffer)"));
    for (const auto* contract : {"std::array<Variant, 6>",
                                 "entry.generation != base->ProgramGeneration()",
                                 "occupied != GL_NONE",
                                 "glDrawBuffers(buffer_count, buffers.data())",
                                 "glColorMaski(6, mask[0]",
                                 "std::uncaught_exceptions() > exceptions_",
                                 "m_surface->view == mode()",
                                 "m_surface->extent.width == ctx.internal_w()",
                                 "m_surface->extent.height == ctx.internal_h()",
                                 "if (identity_view || contribution_view) {"}) {
        EXPECT_NE(pass.find(contract), std::string::npos) << contract;
    }
    const auto shader = Source("res/shaders/debug_view.frag");
    EXPECT_NE(shader.find("surface.a == texelFetch(gDepth, pixel, 0).r"), std::string::npos);
    EXPECT_NE(shader.find("all(equal(surface.rgb, vec3(-1.0, 0.0, -1.0)))"), std::string::npos);
    EXPECT_NE(shader.find("all(equal(surface.rgb, vec3(-2.0, 0.0, -2.0)))"), std::string::npos);
    for (const auto* texture : {"gPosition",
                                "gNormalMaterial",
                                "gAlbedo",
                                "gDepth",
                                "gRawMaterial",
                                "gRawMotion",
                                "gRawAuthored"}) {
        EXPECT_NE(shader.find(std::string("rawTexelNonfinite(") + texture + ", pixel)"),
                  std::string::npos);
    }
    EXPECT_NE(shader.find("return rawNonfinite(texelFetch(target, pixel, 0));"), std::string::npos);
}

template<typename Draw>
void CheckRasterReplays(const Draw& draw,
                        Shader& normal,
                        Shader& instrumented,
                        bool& uv_case,
                        GLuint fbo,
                        GLuint vao,
                        GLuint vbo,
                        const std::array<GLuint, 7>& outputs,
                        const std::array<GLenum, 7>& buffers,
                        const std::array<GLuint, 2>& arrays,
                        const std::array<std::array<float, 4>, 2>& samples,
                        const std::array<float, 9>& triangle) {
    constexpr std::array<std::size_t, 6> channels = {3, 4, 4, 2, 2, 4};
    uv_case = false;
    const auto replay_reference = draw(normal, 0, 0);
    // Exercise the actual surface program through the runtime replay attachment scope.
    // Primary outputs and opaque depth must retain their original bytes.
    Pass pass;
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    pass.init_buffers();
    auto& control = Pass::controls();
    control = {};
    control.enabled = true;
    RenderContext ctx;
    ctx.screen_width = ctx.screen_height = 8;
    GBuffer gbuffer;
    gbuffer.fbo_id = fbo;
    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT6, 0, 0);
    glNamedFramebufferDrawBuffers(fbo, 6, buffers.data());
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLuint opaque_depth = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &opaque_depth);
    glTextureStorage2D(opaque_depth, 1, GL_DEPTH_COMPONENT32F, 8, 8);
    glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, opaque_depth, 0);
    glDepthMask(GL_TRUE);
    glClearDepth(0.25);
    glClear(GL_DEPTH_BUFFER_BIT);
    control.view = Pass::Wireframe;
    EXPECT_FALSE(pass.begin_frame(ctx));
    {
        Pass::SurfaceFrame frame(ctx, gbuffer, true);
        ASSERT_TRUE(frame.ready());
        GLint mask_plane = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                              GL_COLOR_ATTACHMENT6,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                              &mask_plane);
        ASSERT_GT(mask_plane, 0);
        GLint format = 0;
        glGetTextureLevelParameteriv(
            static_cast<GLuint>(mask_plane), 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
        EXPECT_EQ(format, GL_RGBA32F);
        // Cutout fragments leave the wire mask empty through the production discard path.
        uv_case = true;
        const std::array<float, 4> transparent = {0.4f, 0.5f, 0.6f, 0};
        for (int level = 0; level < 7; ++level) {
            glClearTexImage(arrays[0], level, GL_RGBA, GL_FLOAT, transparent.data());
        }
        glClearTexImage(static_cast<GLuint>(mask_plane), 0, GL_RGBA, GL_FLOAT, nullptr);
        instrumented.setInt("u_alphaTest", 3);
        EXPECT_FLOAT_EQ(draw(instrumented, Pass::RawFinite, 0)[6][3], 0);
        instrumented.setInt("u_alphaTest", 0);
        uv_case = false;
        for (int level = 0; level < 7; ++level) {
            glClearTexImage(arrays[0], level, GL_RGBA, GL_FLOAT, samples[0].data());
        }
    }
    // A triangle wholly within the viewport has both covered edge pixels and empty interior.
    constexpr std::array<float, 9> outlined = {
        -0.75f, -0.75f, -0.5f, 0.75f, -0.75f, -0.5f, 0, 0.75f, -0.5f};
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(outlined), outlined.data());
    ctx.gbuffer_depth.id = opaque_depth;
    control.view = Pass::Wireframe;
    GLuint composite = 0;
    glCreateFramebuffers(1, &composite);
    glNamedFramebufferTexture(composite, GL_COLOR_ATTACHMENT0, outputs[6], 0);
    for (const double scene_depth : {0.25, 0.5}) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glDepthMask(GL_TRUE);
        glClearDepth(scene_depth);
        glClear(GL_DEPTH_BUFFER_BIT);
        EXPECT_FALSE(pass.begin_frame(ctx));
        GLuint mask_texture = 0;
        {
            Pass::SurfaceFrame frame(ctx, gbuffer, true);
            ASSERT_TRUE(frame.ready());
            GLint attached = 0;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                                  GL_COLOR_ATTACHMENT6,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                                  &attached);
            mask_texture = static_cast<GLuint>(attached);
            draw(instrumented, Pass::RawFinite, 0);
        }
        std::array<std::array<float, 4>, 64> mask{};
        glGetTextureImage(mask_texture, 0, GL_RGBA, GL_FLOAT, sizeof(mask), mask.data());
        int edge_pixels = 0;
        for (const auto& pixel : mask) {
            if (pixel[3] > 0) {
                ++edge_pixels;
                EXPECT_FLOAT_EQ(pixel[3], 0.25f);
            }
        }
        if (scene_depth == 0.25) {
            EXPECT_GT(edge_pixels, 0);
            EXPECT_LT(edge_pixels, 64);
            EXPECT_FLOAT_EQ(mask[3 * 8 + 4][3], 0); // shaded triangle interior stays unoutlined
        } else {
            EXPECT_EQ(edge_pixels, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, composite);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        const std::array<float, 4> shaded = {0.2f, 0.3f, 0.4f, 0.7f};
        glClearTexImage(outputs[6], 0, GL_RGBA, GL_FLOAT, shaded.data());
        pass.execute(ctx);
        std::array<std::array<float, 4>, 64> composited{};
        glReadPixels(0, 0, 8, 8, GL_RGBA, GL_FLOAT, composited.data());
        for (std::size_t index = 0; index < mask.size(); ++index) {
            if (mask[index][3] == 0) {
                EXPECT_EQ(composited[index], shaded);
            } else {
                EXPECT_NEAR(composited[index][1], 0.85f + 0.15f * shaded[1], 0.00001f);
                EXPECT_FLOAT_EQ(composited[index][3], shaded[3]);
            }
        }
        std::array<float, 64> retained_depth{};
        glGetTextureImage(opaque_depth,
                          0,
                          GL_DEPTH_COMPONENT,
                          GL_FLOAT,
                          sizeof(retained_depth),
                          retained_depth.data());
        for (const float value : retained_depth) {
            EXPECT_FLOAT_EQ(value, static_cast<float>(scene_depth));
        }
    }
    // Restore the fixture and verify every production channel survived the wire replays.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    for (std::size_t attachment = 0; attachment < channels.size(); ++attachment) {
        std::array<float, 4> retained{};
        glReadBuffer(buffers[attachment]);
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_FLOAT, retained.data());
        for (std::size_t channel = 0; channel < channels[attachment]; ++channel) {
            EXPECT_FLOAT_EQ(retained[channel], replay_reference[attachment][channel]);
        }
    }
    pass.destroy_buffers();
    glDeleteFramebuffers(1, &composite);
    glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, 0, 0);
    glDeleteTextures(1, &opaque_depth);
    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT6, outputs[6], 0);
    glNamedFramebufferDrawBuffers(fbo, 7, buffers.data());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(triangle), triangle.data());
    control = {};
}

TEST_F(DiagnosticContributionsGl,
       SurfaceVariantsUseRealMipWeightsAndRawFiniteInputsWithoutChangingOutputs) {
    const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
    const auto fragment = (root / "g_buffer.frag").string();
    for (const auto* stage : {"g_buffer.vert", "instanced_mesh.vert", "skinned_mesh.vert"}) {
        const auto vertex = (root / stage).string();
        Shader legacy(vertex.c_str(), fragment.c_str(), {}, {"LUMIN_DIAG_SURFACE 1"});
        Shader ladder(vertex.c_str(),
                      fragment.c_str(),
                      {},
                      {"LUMIN_DIAG_SURFACE 1", "LUMINUMBRA_FAR_WORLD_LADDER 1"});
        ASSERT_TRUE(legacy.IsValid()) << stage;
        ASSERT_TRUE(ladder.IsValid()) << stage;
    }
    const auto vertex = (root / "g_buffer.vert").string();
    Shader normal(vertex.c_str(), fragment.c_str());
    Shader instrumented(vertex.c_str(), fragment.c_str(), {}, {"LUMIN_DIAG_SURFACE 1"});
    ASSERT_TRUE(normal.IsValid());
    ASSERT_TRUE(instrumented.IsValid());
    EXPECT_EQ(glGetUniformLocation(normal.Id(), "u_surfaceMode"), -1);
    EXPECT_GE(glGetUniformLocation(instrumented.Id(), "u_surfaceMode"), 0);
    EXPECT_EQ(glGetFragDataLocation(normal.Id(), "gDiagnosticSurface"), -1);
    EXPECT_EQ(glGetFragDataLocation(instrumented.Id(), "gDiagnosticSurface"), 6);

    std::array<GLuint, 7> outputs{};
    glCreateTextures(GL_TEXTURE_2D, static_cast<GLsizei>(outputs.size()), outputs.data());
    GLuint fbo = 0;
    glCreateFramebuffers(1, &fbo);
    std::array<GLenum, 7> buffers{};
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        glTextureStorage2D(outputs[index], 1, GL_RGBA32F, 8, 8);
        buffers[index] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(index);
        glNamedFramebufferTexture(fbo, buffers[index], outputs[index], 0);
    }
    glNamedFramebufferDrawBuffers(fbo, static_cast<GLsizei>(buffers.size()), buffers.data());
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    GLuint lut = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &lut);
    glTextureStorage2D(lut, 1, GL_RGBA32F, 1, 4);
    glTextureParameteri(lut, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(lut, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const std::array<std::array<float, 4>, 4> material = {
        {{0.2f, 0.7f, 1, 0}, {0, 0, 1.0f / 64, 1}, {0, 1, 0, 0}, {1, 1, 1, 0}}};
    glTextureSubImage2D(lut, 0, 0, 0, 1, 4, GL_RGBA, GL_FLOAT, material.data());
    std::array<GLuint, 2> arrays{};
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 2, arrays.data());
    constexpr std::array<std::array<float, 4>, 2> samples = {
        {{0.4f, 0.5f, 0.6f, 1}, {0.5f, 0.5f, 1, 1}}};
    for (std::size_t index = 0; index < arrays.size(); ++index) {
        glTextureStorage3D(arrays[index], 7, GL_RGBA32F, 64, 64, 1);
        glTextureParameteri(arrays[index], GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTextureParameteri(arrays[index], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        for (int level = 0; level < 7; ++level) {
            glClearTexImage(arrays[index], level, GL_RGBA, GL_FLOAT, samples[index].data());
        }
    }
    glBindTextureUnit(0, lut);
    for (GLuint unit = 1; unit <= 6; ++unit) {
        glBindTextureUnit(unit, arrays[unit == 2 || unit == 5 ? 1 : 0]);
    }
    GLuint vao = 0;
    GLuint vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    constexpr std::array<float, 9> triangle = {-1, -1, -0.5f, 3, -1, -0.5f, -1, 3, -0.5f};
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glVertexAttrib3f(1, 1, 2, 3);
    glVertexAttribI1ui(2, 1);
    glViewport(0, 0, 8, 8);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    bool uv_case = false;
    const auto draw = [&](Shader& shader, int mode, float bias) {
        shader.use();
        shader.setMat4("model", glm::mat4(1));
        shader.setMat4("view", glm::mat4(1));
        shader.setMat4("projection", glm::mat4(1));
        shader.setMat3("normalMatrix", glm::mat3(1));
        shader.setMat3("u_normalViewMatrix", glm::mat3(1));
        shader.setMat4("u_prev_view_proj", glm::mat4(1));
        shader.setVec2("u_inv_screen_size", glm::vec2(1.0f / 8));
        shader.setInt("u_materialLUT", 0);
        shader.setInt("u_terrainTextures", 1);
        shader.setInt("u_terrainNormals", 2);
        shader.setInt("u_skinnedTextures", 3);
        shader.setInt("u_terrainRoughness", 4);
        shader.setInt("u_skinnedAlbedoLayer", uv_case ? 0 : -1);
        shader.setInt("u_skinnedNormalLayer", uv_case ? 0 : -1);
        shader.setInt("u_staticSurfaceLayer", uv_case ? 0 : -1);
        shader.setInt("u_surfaceMode", mode);
        shader.setFloat("u_lodBias", bias);
        AttributedGl::Submit(glDrawArrays, static_cast<GLenum>(GL_TRIANGLES), 0, 3);
        std::array<std::array<float, 4>, 7> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            glReadBuffer(buffers[index]);
            glReadPixels(4, 4, 1, 1, GL_RGBA, GL_FLOAT, values[index].data());
        }
        return values;
    };
    const auto reference = draw(normal, 0, 0);
    constexpr std::array<std::size_t, 6> channels = {3, 4, 4, 2, 2, 4};
    for (int mode = Pass::ActualMip; mode <= Pass::RawFinite; ++mode) {
        const auto result = draw(instrumented, mode, 0);
        for (std::size_t attachment = 0; attachment < channels.size(); ++attachment) {
            for (std::size_t channel = 0; channel < channels[attachment]; ++channel) {
                EXPECT_FLOAT_EQ(result[attachment][channel], reference[attachment][channel]);
            }
        }
        EXPECT_FLOAT_EQ(result[6][3], 0.25f);
        if (mode == Pass::ActualMip || mode == Pass::TexelDensity) {
            EXPECT_NEAR(result[6][0], 4.0f, 0.00001f);
            EXPECT_NEAR(draw(instrumented, mode, 1)[6][0], 5.0f, 0.00001f);
            // Clamping changes selected mip, while density retains the true footprint.
            EXPECT_NEAR(draw(instrumented, mode, 8)[6][0],
                        mode == Pass::ActualMip ? 6.0f : 12.0f,
                        0.00001f);
        } else if (mode == Pass::TriplanarWeights) {
            EXPECT_NEAR(result[6][0], 1.0f / 98, 0.00001f);
            EXPECT_NEAR(result[6][1], 16.0f / 98, 0.00001f);
            EXPECT_NEAR(result[6][2], 81.0f / 98, 0.00001f);
            EXPECT_TRUE(DiagnosticContributions::normalized_weights(
                {result[6][0], result[6][1], result[6][2]}));
        } else {
            EXPECT_EQ(result[6], (std::array<float, 4>{0, 0, 0, 0.25f}));
        }
    }
    // The terrain vertex fixture has constant mesh UVs: all UV material arrays magnify at mip 0.
    uv_case = true;
    EXPECT_FLOAT_EQ(draw(instrumented, Pass::ActualMip, 0)[6][0], 0.0f);
    EXPECT_NEAR(draw(instrumented, Pass::TexelDensity, 0)[6][0], std::log2(1e-8f), 0.00001f);
    EXPECT_EQ(draw(instrumented, Pass::RawFinite, 0)[6], (std::array<float, 4>{0, 0, 0, 0.25f}));
    CheckRasterReplays(draw,
                       normal,
                       instrumented,
                       uv_case,
                       fbo,
                       vao,
                       vbo,
                       outputs,
                       buffers,
                       arrays,
                       samples,
                       triangle);
    for (const float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        auto corrupt = material;
        // This raw LUT channel is otherwise hidden by the material path's clamp/encoding.
        corrupt[3][0] = invalid;
        glTextureSubImage2D(lut, 0, 0, 0, 1, 4, GL_RGBA, GL_FLOAT, corrupt.data());
        EXPECT_EQ(draw(instrumented, Pass::RawFinite, 0)[6],
                  (std::array<float, 4>{-1, 0, -1, 0.25f}));
    }
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(2, arrays.data());
    glDeleteTextures(1, &lut);
    glDeleteTextures(static_cast<GLsizei>(outputs.size()), outputs.data());
    glDeleteFramebuffers(1, &fbo);
}

TEST_F(DiagnosticContributionsGl,
       SurfaceScopeRestoresAttachmentsAndRefusesCollisionsReplayAndIdentityOwnership) {
    Pass pass;
    auto& control = Pass::controls();
    control = {};
    control.enabled = true;
    control.view = Pass::RawFinite;
    RenderContext ctx;
    ctx.screen_width = ctx.screen_height = 8;
    EXPECT_FALSE(pass.begin_frame(ctx));
    GBuffer gbuffer;
    glGenFramebuffers(1, &gbuffer.fbo_id);
    GLuint color = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &color);
    glTextureStorage2D(color, 1, GL_RGBA32F, 8, 8);
    glBindFramebuffer(GL_FRAMEBUFFER, gbuffer.fbo_id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glColorMaski(6, GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    glEnablei(GL_BLEND, 6);
    const auto attachment = [&] {
        GLint value = GL_NONE;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                              GL_COLOR_ATTACHMENT6,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                              &value);
        return value;
    };
    {
        Pass::SurfaceFrame frame(ctx, gbuffer);
        EXPECT_EQ(attachment(), GL_TEXTURE);
        EXPECT_EQ(glIsEnabledi(GL_BLEND, 6), GL_FALSE);
        std::array<GLboolean, 4> mask{};
        glGetBooleani_v(GL_COLOR_WRITEMASK, 6, mask.data());
        EXPECT_EQ(mask, (std::array<GLboolean, 4>{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE}));
        {
            Pass::SurfaceFrame nested(ctx, gbuffer);
            EXPECT_EQ(attachment(), GL_TEXTURE);
        }
        EXPECT_EQ(attachment(), GL_TEXTURE);
        Pass::unsupported_surface();
    }
    EXPECT_EQ(attachment(), GL_NONE);
    EXPECT_EQ(glIsEnabledi(GL_BLEND, 6), GL_TRUE);
    std::array<GLboolean, 4> mask{};
    glGetBooleani_v(GL_COLOR_WRITEMASK, 6, mask.data());
    EXPECT_EQ(mask, (std::array<GLboolean, 4>{GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE}));
    GLint buffer = -1;
    glGetIntegerv(GL_DRAW_BUFFER6, &buffer);
    EXPECT_EQ(buffer, GL_NONE);
    for (const int refusal : {0, 1, 2, 3, 4}) {
        SCOPED_TRACE(refusal);
        control.enabled = refusal != 0;
        ctx.frozen_replay = refusal == 1;
        ctx.screen_width = refusal == 2 ? 0U : 8U;
        if (refusal == 3) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, color, 0);
        }
        DiagnosticDrawTable table;
        std::unique_ptr<GBufferDiagnosticCapture> capture;
        if (refusal == 4) {
            capture = std::make_unique<GBufferDiagnosticCapture>(table);
        }
        {
            Pass::SurfaceFrame frame(ctx, gbuffer);
            EXPECT_EQ(attachment(), refusal == 3 ? GL_TEXTURE : GL_NONE);
        }
        if (refusal == 3) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT6, GL_TEXTURE_2D, 0, 0);
        }
    }
    control.enabled = true;
    ctx.screen_width = 8;
    try {
        Pass::SurfaceFrame frame(ctx, gbuffer);
        ASSERT_EQ(attachment(), GL_TEXTURE);
        throw std::runtime_error("surface scope exit");
    } catch (const std::runtime_error&) {
        EXPECT_EQ(attachment(), GL_NONE);
    }
    pass.reset_shader();
    glDeleteFramebuffers(1, &gbuffer.fbo_id);
    glDeleteTextures(1, &color);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST_F(DiagnosticContributionsGl, SurfaceOverlayHasDisjointSentinelsAndRejectsHiddenPixels) {
    const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
    const auto vertex = (root / "fullscreen_tri.vert").string();
    const auto fragment = (root / "debug_view.frag").string();
    Shader shader(vertex.c_str(), fragment.c_str(), {}, {"LUMIN_DIAG_VIEWS 1"});
    ASSERT_TRUE(shader.IsValid());
    std::array<GLuint, 4> textures{};
    glCreateTextures(GL_TEXTURE_2D, 4, textures.data());
    for (const GLuint texture : textures) {
        glTextureStorage2D(texture, 1, GL_RGBA32F, 1, 1);
        glClearTexImage(texture, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    const std::array<float, 4> depth = {0.5f, 0, 0, 0};
    glTextureSubImage2D(textures[1], 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, depth.data());
    GLuint fbo = 0;
    glCreateFramebuffers(1, &fbo);
    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, textures[0], 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, 1, 1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindTextureUnit(3, textures[1]);
    glBindTextureUnit(6, textures[2]);
    glBindTextureUnit(7, textures[3]);
    shader.use();
    shader.setInt("gDepth", 3);
    shader.setInt("gIdentity", 4);
    shader.setInt("gDrawMetadata", 5);
    shader.setInt("u_contributionReady", 1);
    shader.setInt("u_rawMaterialReady", 1);
    const auto draw = [&](int mode, const std::array<float, 4>& input) {
        shader.setInt("u_mode", mode);
        glTextureSubImage2D(textures[2], 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, input.data());
        AttributedGl::Submit(glDrawArrays, static_cast<GLenum>(GL_TRIANGLES), 0, 3);
        std::array<float, 4> pixel{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, pixel.data());
        return pixel;
    };
    EXPECT_EQ(draw(Pass::ActualMip, {4, 4, 4, 0.5f}),
              (std::array<float, 4>{0.25f, 0.25f, 0.25f, 1}));
    EXPECT_EQ(draw(Pass::TexelDensity, {0, 0, 0, 0.5f}),
              (std::array<float, 4>{0.5f, 0.5f, 0.5f, 1}));
    // Valid magnification footprints at -1/-2 octaves must never collide with sentinels.
    EXPECT_EQ(draw(Pass::TexelDensity, {-1, -1, -1, 0.5f}),
              (std::array<float, 4>{0.4375f, 0.4375f, 0.4375f, 1}));
    EXPECT_EQ(draw(Pass::TexelDensity, {-2, -2, -2, 0.5f}),
              (std::array<float, 4>{0.375f, 0.375f, 0.375f, 1}));
    EXPECT_EQ(draw(Pass::TriplanarWeights, {0.25f, 0.5f, 0.25f, 0.5f}),
              (std::array<float, 4>{0.25f, 0.5f, 0.25f, 1}));
    EXPECT_EQ(draw(Pass::TriplanarWeights, {-2, 0, -2, 0.5f}), (std::array<float, 4>{1, 1, 0, 1}));
    EXPECT_EQ(draw(Pass::RawFinite, {-1, 0, -1, 0.5f}), (std::array<float, 4>{1, 0, 1, 1}));
    EXPECT_EQ(draw(Pass::RawFinite, {0, 0, 0, 0.5f}), (std::array<float, 4>{0, 0, 0, 1}));
    const std::array<float, 4> hatch = {1, 0, 1, 1};
    EXPECT_EQ(draw(Pass::ActualMip, {4, 4, 4, 0.25f}), hatch);
    EXPECT_EQ(draw(Pass::ActualMip, {4, 4, 4, 0}), hatch);
    for (const float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        const std::array<float, 4> stored = {0, invalid, 0, 0};
        glTextureSubImage2D(textures[3], 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, stored.data());
        EXPECT_EQ(draw(Pass::RawFinite, {0, 0, 0, 0.5f}), (std::array<float, 4>{1, 0, 1, 1}));
    }
    EXPECT_EQ(draw(Pass::ShadowCascade, {-3, -3, -3, 1}), (std::array<float, 4>{1, 1, 1, 1}));
    EXPECT_EQ(draw(Pass::ShadowCascade, {-1, -1, -1, 1}), (std::array<float, 4>{1, 0.4f, 0, 1}));
    EXPECT_EQ(draw(Pass::ShadowCascade, {-2, -2, -2, 1}), hatch);
    EXPECT_EQ(draw(Pass::ShadowFootprint, {0.25f, 0.75f, 0, 1}),
              (std::array<float, 4>{0.25f, 0.75f, 0.5f, 1}));
    EXPECT_EQ(draw(Pass::Wireframe, {1, 1, 1, 0.5f}),
              (std::array<float, 4>{0.05f, 1, 0.2f, 0.85f}));
    EXPECT_EQ(draw(Pass::Wireframe, {1, 1, 1, 0.25f}), (std::array<float, 4>{0, 0, 0, 0}));
    EXPECT_EQ(draw(Pass::Wireframe, {0, 0, 0, 0}), (std::array<float, 4>{0, 0, 0, 0}));
    shader.setInt("u_contributionReady", 0);
    EXPECT_EQ(draw(Pass::Wireframe, {0, 0, 0, 0}), hatch);
    EXPECT_EQ(draw(Pass::ActualMip, {4, 4, 4, 0.5f}), hatch);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(4, textures.data());
}

TEST(DiagnosticContributions, CascadeAndReplayContractsUseProductionInputsAndBoundedStorage) {
    const auto lighting = Source("res/shaders/lighting_pass.frag");
    const auto selector = lighting.find("float CalculateShadow(");
    const auto projection = lighting.find("vec4 fragPosLightSpace =", selector);
    const auto observation =
        lighting.find("diagnosticCascade = vec3(float(cascadeIndex), projCoords.xy);", selector);
    ASSERT_NE(selector, std::string::npos);
    EXPECT_LT(projection, observation);
    EXPECT_LT(observation, lighting.find("// 3. PCF", selector));
    EXPECT_NE(
        lighting.find("diagnosticShadowUv = vec3(projCoords.xy, log2(max(footprint, 1e-8)));"),
        std::string::npos);
    EXPECT_NE(lighting.find("diagnosticCascade = vec3(insideAny ? -1.0 : -3.0);"),
              std::string::npos);
    const auto implementation = Source("src/luminumbra_client/rendering/passes/DebugViewPass.cpp");
    EXPECT_NE(implementation.find("view == Wireframe ? RawFinite : view"), std::string::npos);
    EXPECT_NE(implementation.find("outputs.fill(GL_NONE)"), std::string::npos);
    EXPECT_NE(implementation.find("glDepthMask(GL_FALSE);"), std::string::npos);
    EXPECT_NE(implementation.find("glDepthFunc(GL_GEQUAL);"), std::string::npos);
    EXPECT_NE(implementation.find("replay_guard.reset();"), std::string::npos);
    const auto overlay = Source("res/shaders/debug_view.frag");
    EXPECT_NE(overlay.find("replay.a >= texelFetch(gDepth, pixel, 0).r"), std::string::npos);
    EXPECT_NE(Pass::contribution_legend(Pass::Wireframe).find("GEQUAL"), std::string_view::npos);
    const auto pipeline = Source("src/luminumbra_client/rendering/RenderPipeline.cpp");
    EXPECT_NE(pipeline.find("DebugViewPass::controls().wireframe = m_frame_prepared.wireframe;"),
              std::string::npos);
    EXPECT_EQ(pipeline.find("glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)"), std::string::npos);
}

TEST(DiagnosticContributions, ReplayAndExistingWireframeToggleAreOptInWithoutGlWorkOff) {
    Pass pass;
    auto& control = Pass::controls();
    control = {};
    EXPECT_FALSE(Pass::geometry_requested());
    EXPECT_FALSE(Pass::replaying());
    Pass::replay_depth_policy(); // disabled path needs no loader/context
    for (const int mode : {Pass::ShadowCascade, Pass::ShadowFootprint, Pass::Wireframe}) {
        control.view = mode;
        EXPECT_EQ(pass.mode(), Pass::None);
        pass.execute(RenderContext{});
        control.enabled = control.refresh = true;
        EXPECT_FALSE(pass.begin_frame(RenderContext{}));
        EXPECT_FALSE(control.refresh);
        EXPECT_EQ(pass.mode(), mode);
        control.enabled = false;
    }
    control.wireframe = true;
    EXPECT_EQ(pass.mode(), Pass::Wireframe);
    EXPECT_EQ(Pass::geometry_view(), Pass::Wireframe);
    EXPECT_TRUE(Pass::geometry_requested());
    control = {};
}

TEST_F(DiagnosticContributionsGl, ReplayRestoresRasterBlendBindingsAndAttachmentsOnEveryExit) {
    Pass pass;
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    pass.init_buffers();
    auto& control = Pass::controls();
    control = {};
    control.enabled = true;
    GBuffer gbuffer;
    glCreateFramebuffers(1, &gbuffer.fbo_id);
    GLuint color = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &color);
    glTextureStorage2D(color, 1, GL_RGBA32F, 8, 8);
    glNamedFramebufferTexture(gbuffer.fbo_id, GL_COLOR_ATTACHMENT0, color, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, gbuffer.fbo_id);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    RenderContext ctx;
    ctx.screen_width = ctx.screen_height = 8;
    glViewport(0, 0, 8, 8);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    const auto snapshot = [] {
        std::vector<GLint64> values;
        GLint bound_texture = 0;
        GLint active_texture = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_texture);
        glActiveTexture(static_cast<GLenum>(active_texture));
        values.push_back(bound_texture);
        for (const GLenum name : std::array<GLenum, 10>{GL_DRAW_FRAMEBUFFER_BINDING,
                                                        GL_READ_FRAMEBUFFER_BINDING,
                                                        GL_CURRENT_PROGRAM,
                                                        GL_VERTEX_ARRAY_BINDING,
                                                        GL_ACTIVE_TEXTURE,
                                                        GL_ARRAY_BUFFER_BINDING,
                                                        GL_DRAW_INDIRECT_BUFFER_BINDING,
                                                        GL_SHADER_STORAGE_BUFFER_BINDING,
                                                        GL_DEPTH_FUNC,
                                                        GL_CULL_FACE_MODE}) {
            GLint value = 0;
            glGetIntegerv(name, &value);
            values.push_back(value);
        }
        std::array<GLint, 2> polygon{};
        glGetIntegerv(GL_POLYGON_MODE, polygon.data());
        values.insert(values.end(), polygon.begin(), polygon.end());
        GLboolean depth_mask = GL_FALSE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        values.push_back(depth_mask);
        GLfloat width = 0;
        glGetFloatv(GL_LINE_WIDTH, &width);
        values.push_back(static_cast<GLint64>(width * 1024));
        for (const GLenum capability : std::array<GLenum, 5>{GL_DEPTH_TEST,
                                                             GL_CULL_FACE,
                                                             GL_DITHER,
                                                             GL_POLYGON_OFFSET_FILL,
                                                             GL_POLYGON_OFFSET_LINE}) {
            values.push_back(glIsEnabled(capability));
        }
        for (GLuint index = 0; index < 7; ++index) {
            GLint buffer = 0;
            glGetIntegerv(GL_DRAW_BUFFER0 + index, &buffer);
            values.push_back(buffer);
            values.push_back(glIsEnabledi(GL_BLEND, index));
            std::array<GLboolean, 4> mask{};
            glGetBooleani_v(GL_COLOR_WRITEMASK, index, mask.data());
            values.insert(values.end(), mask.begin(), mask.end());
            for (const GLenum parameter : std::array<GLenum, 6>{GL_BLEND_SRC_RGB,
                                                                GL_BLEND_DST_RGB,
                                                                GL_BLEND_SRC_ALPHA,
                                                                GL_BLEND_DST_ALPHA,
                                                                GL_BLEND_EQUATION_RGB,
                                                                GL_BLEND_EQUATION_ALPHA}) {
                GLint value = 0;
                glGetIntegeri_v(parameter, index, &value);
                values.push_back(value);
            }
        }
        GLint attachment = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                              GL_COLOR_ATTACHMENT6,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                              &attachment);
        values.push_back(attachment);
        return values;
    };
    GLuint previous_plane = 0;
    control.view = Pass::Wireframe;
    for (const bool exceptional : {false, true}) {
        EXPECT_FALSE(pass.begin_frame(ctx));
        glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
        glDisable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glEnable(GL_DITHER);
        glEnablei(GL_BLEND, 6);
        glBlendFuncSeparatei(6, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ZERO);
        glBlendEquationSeparatei(6, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);
        glColorMaski(6, GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
        glBindTextureUnit(0, previous_plane);
        const auto before = snapshot();
        try {
            Pass::SurfaceFrame frame(ctx, gbuffer, true);
            ASSERT_TRUE(frame.ready());
            GLint plane = 0;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                                  GL_COLOR_ATTACHMENT6,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                                  &plane);
            previous_plane = static_cast<GLuint>(plane);
            EXPECT_TRUE(Pass::replaying());
            EXPECT_EQ(glIsEnabled(GL_DEPTH_TEST), GL_TRUE);
            GLint depth = 0;
            glGetIntegerv(GL_DEPTH_FUNC, &depth);
            EXPECT_EQ(depth, GL_GEQUAL);
            GLboolean mask = GL_TRUE;
            glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
            EXPECT_EQ(mask, GL_FALSE);
            glBindVertexArray(vao);
            glActiveTexture(GL_TEXTURE9);
            glDisable(GL_CULL_FACE);
            // Simulate the ladder's state installation and exercise its pre-draw repair.
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_GREATER);
            glDepthMask(GL_TRUE);
            Pass::replay_depth_policy();
            glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
            EXPECT_EQ(mask, GL_FALSE);
            if (exceptional) {
                throw std::runtime_error("replay exit");
            }
        } catch (const std::runtime_error&) {
            EXPECT_TRUE(exceptional);
        }
        EXPECT_FALSE(Pass::replaying());
        EXPECT_EQ(snapshot(), before);
        const auto overlay_before = snapshot();
        ctx.gbuffer_depth.id = previous_plane;
        pass.execute(ctx);
        EXPECT_EQ(snapshot(), overlay_before);
    }
    // Every refusal preserves state: disabled, frozen, invalid extent, collision, identity owner.
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    for (int refusal = 0; refusal < 5; ++refusal) {
        control.view = Pass::Wireframe;
        control.enabled = refusal != 0;
        ctx.frozen_replay = refusal == 1;
        ctx.screen_width = refusal == 2 ? 0U : 8U;
        if (refusal == 3) {
            glNamedFramebufferTexture(gbuffer.fbo_id, GL_COLOR_ATTACHMENT6, color, 0);
        }
        DiagnosticDrawTable table;
        std::unique_ptr<GBufferDiagnosticCapture> capture;
        if (refusal == 4) {
            capture = std::make_unique<GBufferDiagnosticCapture>(table);
        }
        const auto before = snapshot();
        {
            Pass::SurfaceFrame frame(ctx, gbuffer, true);
            EXPECT_FALSE(frame.ready());
        }
        EXPECT_EQ(snapshot(), before);
        if (refusal == 3) {
            glNamedFramebufferTexture(gbuffer.fbo_id, GL_COLOR_ATTACHMENT6, 0, 0);
        }
    }
    control.enabled = control.wireframe = false;
    const auto off_state = snapshot();
    pass.execute(ctx);
    {
        Pass::SurfaceFrame off(ctx, gbuffer, true);
        EXPECT_FALSE(off.ready());
    }
    EXPECT_EQ(snapshot(), off_state);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    pass.reset_shader();
    pass.destroy_buffers();
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &color);
    glDeleteFramebuffers(1, &gbuffer.fbo_id);
}
