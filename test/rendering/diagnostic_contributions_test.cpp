#include "rendering/AttributedGl.h"
#include "rendering/Camera.h"
#include "rendering/RenderContext.h"
#include "rendering/RenderResourceRegistry.h"
#include "rendering/Shader.h"
#include "rendering/passes/DebugViewPass.h"
#include "rendering/passes/LightingPass.h"
#include <glad/glad.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
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

std::string CaptureOff(std::string_view text) {
    // Preserve every byte outside additive, unnested contribution guards, including blank lines.
    constexpr std::string_view guard = "#ifdef LUMIN_DIAG_CONTRIBUTIONS\n";
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
} // namespace

TEST(DiagnosticContributions, CatalogueLegendsAndCycleCoverEveryNamedTermAndUnsupportedPlane) {
    auto& control = Pass::controls();
    control = {};
    control.enabled = true;
    int legacy = Pass::Position;
    for (int mode = Pass::DrawId; mode <= Pass::ShadowPlane; ++mode) {
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
    EXPECT_EQ(Hash(CaptureOff(lighting)), 0xe07744c933605551ULL);
    // No G-buffer instrumentation is needed: its existing material channels are authoritative.
    EXPECT_EQ(Hash(Source("res/shaders/g_buffer.frag")), 0x3dbc418fae6c670dULL);
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
    const auto configure = [](Shader& shader) {
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
        shader.setInt("u_staticStudio", 1);
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
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
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
