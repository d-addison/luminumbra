#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarWorldDepthPosition.h"
#include "luminumbra_client/rendering/FarWorldFog.h"
#include "luminumbra_client/rendering/passes/PlantProcgenPass.h"
#include "rendering/Shader.h"

#include <gtest/gtest.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;

class FarWorldFogGl : public ::testing::Test {
protected:
    static constexpr int extent = 9;
    static constexpr int center = extent * extent / 2;

    void SetUp() override {
        initialized = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(initialized);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(extent, extent, "far_world_fog", nullptr, nullptr);
        ASSERT_NE(window, nullptr);
        glfwMakeContextCurrent(window);
        loaded = gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0;
        ASSERT_TRUE(loaded);
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        const auto output = Texture(GL_TEXTURE_2D, GL_RGBA32F, GL_RGBA, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
                  static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        glViewport(0, 0, extent, extent);
    }

    void TearDown() override {
        if (loaded) {
            glUseProgram(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindVertexArray(0);
            for (const auto texture : allocated)
                glDeleteTextures(1, &texture);
            glDeleteFramebuffers(1, &fbo);
            glDeleteVertexArrays(1, &vao);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }
        if (window)
            glfwDestroyWindow(window);
        if (initialized)
            glfwTerminate();
    }

    GLuint Texture(GLenum target, GLint internal, GLenum channels, const void* pixels) {
        const GLenum binding = target == GL_TEXTURE_3D         ? GL_TEXTURE_BINDING_3D
                               : target == GL_TEXTURE_2D_ARRAY ? GL_TEXTURE_BINDING_2D_ARRAY
                                                               : GL_TEXTURE_BINDING_2D;
        GLint previous = 0;
        glGetIntegerv(binding, &previous);
        GLuint texture = 0;
        glGenTextures(1, &texture);
        allocated.push_back(texture);
        glBindTexture(target, texture);
        if (target == GL_TEXTURE_2D)
            glTexImage2D(target, 0, internal, extent, extent, 0, channels, GL_FLOAT, pixels);
        else
            glTexImage3D(target, 0, internal, extent, extent, 4, 0, channels, GL_FLOAT, pixels);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (target != GL_TEXTURE_2D)
            glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glBindTexture(target, static_cast<GLuint>(previous));
        return texture;
    }

    std::unique_ptr<Shader> Production(const char* fragment) {
        const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
        auto shader = std::make_unique<Shader>((root / "fullscreen_tri.vert").string().c_str(),
                                               (root / fragment).string().c_str());
        EXPECT_TRUE(shader->IsValid()) << shader->Diagnostic();
        if (!shader->IsValid())
            return shader;
        shader->use();
        units.clear();
        // Every active sampler gets a complete texture on its own unit. This
        // also prevents cross-type sampler aliasing on branches the test disables.
        std::vector<glm::vec4> defaults(extent * extent * 4, glm::vec4(0, 0, 0, 1));
        int unit = 0;
        for (const auto& sampler : shader->Reflected().samplers) {
            const GLenum target = sampler.type == GL_SAMPLER_3D         ? GL_TEXTURE_3D
                                  : sampler.type == GL_SAMPLER_2D_ARRAY ? GL_TEXTURE_2D_ARRAY
                                                                        : GL_TEXTURE_2D;
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(target, Texture(target, GL_RGBA32F, GL_RGBA, defaults.data()));
            shader->setInt(sampler.name, unit);
            units.emplace(sampler.name, unit++);
        }
        shader->setMat4("u_inverseView", glm::mat4(1));
        shader->setVec3("u_viewPos", glm::vec3(0));
        return shader;
    }

    void Bind(Shader& shader, const char* name, GLuint texture, GLenum target = GL_TEXTURE_2D) {
        const auto found = units.find(name);
        ASSERT_NE(found, units.end()) << name;
        glActiveTexture(GL_TEXTURE0 + found->second);
        glBindTexture(target, texture);
        shader.setInt(name, found->second);
    }

    template<typename T>
    GLuint Input(Shader& shader,
                 const char* name,
                 GLint format,
                 GLenum channels,
                 const std::vector<T>& pixels) {
        const auto texture = Texture(GL_TEXTURE_2D, format, channels, pixels.data());
        Bind(shader, name, texture);
        return texture;
    }

    void Depth(Shader& shader, const char* name, const std::vector<float>& pixels) {
        const auto texture = Input(shader, name, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, pixels);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        const std::array<float, 4> border{};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border.data());
    }

    glm::vec4 Draw() {
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glm::vec4 pixel;
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(extent / 2, extent / 2, 1, 1, GL_RGBA, GL_FLOAT, glm::value_ptr(pixel));
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        return pixel;
    }

    void Near(glm::vec4 actual, glm::vec4 expected, float tolerance = .003f) {
        for (int i = 0; i < 4; ++i) {
            EXPECT_TRUE(std::isfinite(actual[i]));
            EXPECT_NEAR(actual[i], expected[i], tolerance);
        }
    }

    // Independent double scalar projection expectation, including jitter.
    static float DepthAt(double axial, double plane) {
        return static_cast<float>(.1 * (plane / axial - 1) / (plane - .1));
    }
    static glm::vec3 PositionAt(int x, int y, double axial, double fov, glm::vec2 jitter) {
        constexpr double pi = 3.14159265358979323846;
        const double scale = std::tan(fov * pi / 360);
        return {static_cast<float>(((x + .5) / extent * 2 - 1 + jitter.x) * axial * scale),
                static_cast<float>(((y + .5) / extent * 2 - 1 + jitter.y) * axial * scale),
                static_cast<float>(-axial)};
    }

    GLFWwindow* window = nullptr;
    bool initialized = false, loaded = false;
    GLuint vao = 0, fbo = 0;
    std::vector<GLuint> allocated;
    std::map<std::string, int> units;
};

TEST_F(FarWorldFogGl, ProductionAerialMatchesCompleteCppCompositionAndRawRadialFade) {
    auto program = Production("volumetric_lighting.frag");
    ASSERT_TRUE(program->IsValid());
    auto& shader = *program;
    const glm::vec4 froxel(.2f, .1f, .05f, .25f);
    std::vector<glm::vec4> volume(extent * extent * 4, froxel);
    Bind(shader,
         "u_froxelIntegrated",
         Texture(GL_TEXTURE_3D, GL_RGBA32F, GL_RGBA, volume.data()),
         GL_TEXTURE_3D);
    shader.setFloat("u_skyDayFactor", 1);
    shader.setFloat("u_sunCosZenith", 0);
    shader.setFloat("u_fogHeight", 100);
    shader.setFloat("u_fogThickness", 40);
    // Deliberately clamp evaluation short: the final fade must still use raw distance.
    shader.setFloat("u_aerialMaxDistance", 1000);
    for (const float plane : {kLegacyFarPlaneMeters, kFarPlaneMeters}) {
        for (const float altitude : {0.0f, 6000.0f}) {
            glm::mat4 inverse_view(1);
            inverse_view[3][1] = altitude;
            shader.setMat4("u_inverseView", inverse_view);
            shader.setVec3("u_viewPos", 0, altitude, 0);
            for (const float density : {0.0f, .002f, .02f}) {
                for (const float ground_density : {0.0f, .01f}) {
                    shader.setFloat("u_aerialDensity", density);
                    shader.setFloat("u_groundFogDensity", ground_density);
                    for (const glm::vec2 jitter : {glm::vec2(0), glm::vec2(.1f, -.08f)}) {
                        auto projection = ReversedZPerspective(glm::radians(45.0f), 1, .1f, plane);
                        projection[2][0] += jitter.x;
                        projection[2][1] += jitter.y;
                        shader.setMat4("u_inverseProjection", glm::inverse(projection));
                        for (const float axial : {1000.0f,
                                                  std::min(15000.0f, plane - 10),
                                                  std::min(16384.0f, plane - 1)}) {
                            Depth(shader,
                                  "gDepth",
                                  std::vector<float>(extent * extent, DepthAt(axial, plane)));
                            const auto position =
                                PositionAt(extent / 2, extent / 2, axial, 45, jitter);
                            const float raw = glm::length(position);
                            const double eval = std::min(static_cast<double>(raw), 1000.0);
                            const double mean = std::exp(-altitude / 250.0);
                            const double h = altitude == 0 ? 1 : 0;
                            const float analytic = static_cast<float>(
                                std::max(1 - std::exp(-eval * density * mean),
                                         std::min(1 - std::exp(-eval * ground_density * h), .9)));
                            shader.setInt("u_volumetricMode", 0);
                            shader.setInt("u_farWorldLadder", 0);
                            const auto legacy_analytic = Draw();
                            EXPECT_NEAR(legacy_analytic.a, std::min(analytic, .94f), 2e-5);
                            for (const bool froxel_on : {false, true}) {
                                shader.setInt("u_volumetricMode", froxel_on ? 1 : 0);
                                for (const bool ladder : {false, true}) {
                                    shader.setInt("u_farWorldLadder", ladder ? 1 : 0);
                                    const auto expected =
                                        ComposeFinalFog(analytic,
                                                        glm::vec3(legacy_analytic),
                                                        froxel_on,
                                                        froxel,
                                                        raw,
                                                        ladder);
                                    const auto actual = Draw();
                                    Near(actual, expected, 2e-4f);
                                    if (ladder && raw >= 16384)
                                        EXPECT_LE(1 - actual.a, .05f);
                                    if (ladder && raw >= 16384) {
                                        // Measure the composited terrain contribution, not
                                        // only shader alpha: white-minus-black = transmittance.
                                        glEnable(GL_BLEND);
                                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                                        glClearColor(0, 0, 0, 1);
                                        glClear(GL_COLOR_BUFFER_BIT);
                                        const auto black = Draw();
                                        glClearColor(1, 1, 1, 1);
                                        glClear(GL_COLOR_BUFFER_BIT);
                                        const auto white = Draw();
                                        for (int i = 0; i < 3; ++i)
                                            EXPECT_LE(white[i] - black[i], .05f);
                                        glDisable(GL_BLEND);
                                    }
                                    if (!ladder)
                                        EXPECT_LE(actual.a, .94f);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST_F(FarWorldFogGl, ProductionMarkerDepthRoundTripsWithSharedAerialInverseAndJitter) {
    // Execute the real writer (including its uniform upload), rather than
    // injecting synthetic depth. Registered in the existing software-GL lane.
    auto cleanup = [](PlantProcgenPass* pass) {
        pass->destroy_buffers();
        delete pass;
    };
    std::unique_ptr<PlantProcgenPass, decltype(cleanup)> pass(new PlantProcgenPass, cleanup);
    pass->init_shader(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT));
    pass->init_buffers();
    pass->set_enabled(true);

    GLint output = 0;
    glGetFramebufferAttachmentParameteriv(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &output);
    const auto position = Texture(GL_TEXTURE_2D, GL_RGB16F, GL_RGB, nullptr);
    const auto normal = Texture(GL_TEXTURE_2D, GL_RGBA8, GL_RGBA, nullptr);
    const auto albedo = Texture(GL_TEXTURE_2D, GL_RGBA8, GL_RGBA, nullptr);
    const auto material = Texture(GL_TEXTURE_2D, GL_RG16F, GL_RG, nullptr);
    const auto depth = Texture(GL_TEXTURE_2D, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, nullptr);
    const std::array<GLuint, 4> colors{position, normal, albedo, material};
    for (std::size_t i = 1; i < colors.size(); ++i)
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i),
                               GL_TEXTURE_2D,
                               colors[i],
                               0);

    Camera camera;
    const auto selection =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
    camera.SelectFarWorldPlane(selection.effective);
    camera.Zoom = 90;
    ASSERT_EQ(camera.GetFarPlane(), kFarPlaneMeters);
    std::uint64_t signature = 0;
    for (const auto jitter : {glm::vec2(.001f, -.002f), glm::vec2(-.0015f, .0007f)}) {
        RenderContext ctx;
        ctx.screen_width = ctx.screen_height = extent;
        ctx.set_gbuffer_depth_contract(selection.effective == FarWorldMode::Ladder,
                                       camera.GetProjectionMatrix(extent, extent),
                                       jitter);
        for (const float axial : {8192.0f, 16000.0f}) {
            SCOPED_TRACE(axial);
            SCOPED_TRACE(signature);
            // A broad, rigid marker triangle at a known view-space plane. UV.x
            // is zero so the real marker wind code leaves these vertices alone.
            const glm::vec3 n(0, 0, 1), color(1, .5f, .25f);
            glBindVertexArray(vao); // Element-buffer uploads require a VAO in core GL.
            pass->set_plants({{{-axial, -axial, -axial}, n, {0, 0}, color},
                              {{axial, -axial, -axial}, n, {0, 0}, color},
                              {{0, axial, -axial}, n, {0, 0}, color}},
                             {0, 1, 2},
                             ++signature);
            glFramebufferTexture2D(
                GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, position, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
            const GLenum buffers[]{GL_COLOR_ATTACHMENT0,
                                   GL_COLOR_ATTACHMENT1,
                                   GL_COLOR_ATTACHMENT2,
                                   GL_COLOR_ATTACHMENT3};
            glDrawBuffers(4, buffers);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
                      static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_GREATER);
            glDepthMask(GL_TRUE);
            glDisable(GL_CULL_FACE);
            glClearDepth(0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glUseProgram(0); // A refused/no-op writer must not reuse the preceding reader.
            pass->execute(ctx, camera);

            GLint writer = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &writer);
            ASSERT_NE(writer, 0);
            const GLint location = glGetUniformLocation(static_cast<GLuint>(writer), "projection");
            ASSERT_GE(location, 0);
            glm::mat4 uploaded;
            glGetUniformfv(static_cast<GLuint>(writer), location, glm::value_ptr(uploaded));
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    EXPECT_EQ(uploaded[c][r], ctx.gbuffer_projection[c][r]);
            float written_depth = 0;
            glm::vec3 stored;
            glReadPixels(
                extent / 2, extent / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &written_depth);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(extent / 2, extent / 2, 1, 1, GL_RGB, GL_FLOAT, glm::value_ptr(stored));
            ASSERT_GT(written_depth, 0);
            glm::vec3 reconstructed;
            // This is the exact shared inverse uploaded by production AerialPass.
            ASSERT_TRUE(ReconstructFarWorldDepthPosition(
                {.5f, .5f}, written_depth, ctx.gbuffer_inverse_projection, reconstructed));
            const auto expected = PositionAt(extent / 2, extent / 2, axial, camera.Zoom, jitter);
            for (int i = 0; i < 3; ++i) {
                EXPECT_NEAR(reconstructed[i], expected[i], .02f);
                EXPECT_NEAR(reconstructed[i], stored[i], .02f);
            }

            // Feed the rasterized writer textures into a production Q54 reader.
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D,
                                   static_cast<GLuint>(output),
                                   0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glDisable(GL_DEPTH_TEST);
            auto reader = Production("debug_view.frag");
            ASSERT_TRUE(reader->IsValid());
            Bind(*reader, "gPosition", position);
            Bind(*reader, "gDepth", depth);
            reader->setInt("u_farWorldLadder", 1);
            reader->setInt("u_mode", 5);
            reader->setFloat("u_posScale", .00002f);
            reader->setMat4("u_gbufferInverseProjection", ctx.gbuffer_inverse_projection);
            glBindVertexArray(vao);
            Near(Draw(), glm::vec4(expected * .00002f + .5f, 1), 2e-6f);
        }
    }
}

TEST_F(FarWorldFogGl, ProductionConsumersUseDepthForFarCentersAndAllNeighbors) {
    const char* fragments[]{"lighting_pass.frag",
                            "ssao.frag",
                            "ssao_gtao.frag",
                            "ssao_bilateral_upsample.frag",
                            "scent_decal.frag",
                            "debug_view.frag"};
    for (const auto fragment : fragments) {
        SCOPED_TRACE(fragment);
        auto program = Production(fragment);
        ASSERT_TRUE(program->IsValid());
        auto& shader = *program;
        const std::string name(fragment);
        const bool lighting = name == "lighting_pass.frag";
        const bool decal = name == "scent_decal.frag";
        const bool debug = name == "debug_view.frag";
        const bool ssao = name == "ssao.frag";
        const bool upsample = name == "ssao_bilateral_upsample.frag";
        if (units.count("gNormalMaterial"))
            Input(shader,
                  "gNormalMaterial",
                  GL_RGBA8,
                  GL_RGBA,
                  std::vector<glm::vec4>(extent * extent, {.5f, .5f, 0, 1.0f / 255}));
        if (lighting) {
            Input(shader,
                  "gAlbedoRoughness",
                  GL_RGBA8,
                  GL_RGBA,
                  std::vector<glm::vec4>(extent * extent, {.3f, .2f, .1f, .7f}));
            Input(shader,
                  "gMetallicAO",
                  GL_RG16F,
                  GL_RG,
                  std::vector<glm::vec2>(extent * extent, {0, -2})); // S12 negative tag.
            Input(shader, "u_ssao", GL_R16F, GL_RED, std::vector<float>(extent * extent, 1));
            shader.setVec3("u_skyAmbientColor", glm::vec3(.3f));
            shader.setVec3("u_moonRadiance", glm::vec3(0));
            shader.setVec3("u_sun.direction", 0, 0, -1);
            shader.setVec3("u_moonDir", 0, 0, -1);
            shader.setInt("u_pointLightCount", 1);
            shader.setVec3("u_pointLights[0].position", 0, 0, -15990);
            shader.setVec3("u_pointLights[0].color", glm::vec3(1));
            shader.setFloat("u_pointLights[0].intensity", 1);
            shader.setFloat("u_pointLights[0].radius", 50);
            shader.setFloat("u_caveSkyMaxDist", 1);
            shader.setInt("u_caveSkySteps", 4);
            shader.setFloat("u_caveAmbientFloor", .1f);
        }
        if (ssao) {
            Input(shader,
                  "u_noiseTexture",
                  GL_RGB16F,
                  GL_RGB,
                  std::vector<glm::vec3>(extent * extent, {1, 0, 0}));
            for (int i = 0; i < 64; ++i)
                shader.setVec3("u_samples[" + std::to_string(i) + "]", .2f, .2f, .4f);
        }
        shader.setFloat("u_radius", 8);
        shader.setVec2("u_screenSize", extent, extent);
        if (upsample) {
            std::vector<float> ao(extent * extent, .2f);
            ao[center] = .9f;
            Input(shader, "u_aoHalf", GL_R16F, GL_RED, ao);
            shader.setVec2("u_halfTexel", 1.0f / extent, 1.0f / extent);
        }
        if (decal) {
            Input(shader,
                  "u_scentField",
                  GL_RG16F,
                  GL_RG,
                  std::vector<glm::vec2>(extent * extent, {.5f, .25f}));
            shader.setVec2("u_scentWorldOrigin", -10, -16010);
            shader.setFloat("u_scentInvWorldSpan", .05f);
            shader.setFloat("u_scentScale", 1);
        }
        if (debug) {
            shader.setInt("u_mode", 5);
            shader.setFloat("u_posScale", .00001f);
        }
        for (const bool mixed : {false, true}) {
            if (mixed && (decal || debug))
                continue; // These shaders have only a center position read.
            // Narrow view puts horizon/AO taps within a metre at the 2 km seam.
            const float fov = mixed ? .02f : 45;
            const glm::vec2 jitter = mixed ? glm::vec2(0) : glm::vec2(.001f, -.002f);
            auto projection = ReversedZPerspective(glm::radians(fov), 1, .1f, kFarPlaneMeters);
            projection[2][0] += jitter.x;
            projection[2][1] += jitter.y;
            shader.setMat4("u_projection", projection);
            shader.setMat4("u_gbufferInverseProjection", glm::inverse(projection));
            std::vector<float> depths(extent * extent);
            std::vector<glm::vec3> reference(extent * extent),
                poison(extent * extent, {0, 0, -1999});
            for (int y = 0; y < extent; ++y) {
                for (int x = 0; x < extent; ++x) {
                    const int index = y * extent + x;
                    const float axial = mixed ? (index == center ? 1999.75f : 2000.125f) : 16000;
                    depths[index] = DepthAt(axial, kFarPlaneMeters);
                    reference[index] = PositionAt(x, y, axial, fov, jitter);
                }
            }
            if (mixed) {
                reference[center] = {0, 0, -2000}; // actual near RGB16F value
                poison[center] = reference[center];
            }
            Depth(shader, lighting ? "u_sceneDepth" : "gDepth", depths);
            const auto good = Texture(GL_TEXTURE_2D, GL_RGB32F, GL_RGB, reference.data());
            const auto bad = Texture(GL_TEXTURE_2D, GL_RGB16F, GL_RGB, poison.data());
            for (const bool cave : {false, true}) {
                if (!lighting && cave)
                    continue;
                shader.setFloat("u_caveAmbientOcclusion", cave ? 1.0f : 0.0f);
                Bind(shader, "gPosition", good);
                shader.setInt("u_farWorldLadder", 0);
                const auto expected = Draw();
                Bind(shader, "gPosition", bad);
                shader.setInt("u_farWorldLadder", 1);
                const auto actual = Draw();
                Near(actual, expected);
                shader.setInt("u_farWorldLadder", 0);
                const auto negative_control = Draw();
                // Demonstrate the poisoned reads matter, rather than accepting
                // a fixture whose position samples have no observable effect.
                if (mixed && (ssao || upsample || name == "ssao_gtao.frag" || (lighting && cave)))
                    EXPECT_GT(std::abs(negative_control.r - expected.r), .005f);
                if (!mixed && (decal || debug || lighting))
                    EXPECT_GT(glm::length(glm::vec3(negative_control - expected)), .005f);
                shader.setMat4("u_gbufferInverseProjection", glm::mat4(0));
                EXPECT_EQ(Draw(), negative_control) << "Legacy ignores the reconstruction matrix";
                shader.setMat4("u_gbufferInverseProjection", glm::inverse(projection));
            }
            if (mixed) {
                // Sky neighbors must stay open even when their stored position
                // contains a plausible occluder. The near center remains valid.
                std::vector<float> sky_depths(extent * extent, 0);
                sky_depths[center] = depths[center];
                Depth(shader, lighting ? "u_sceneDepth" : "gDepth", sky_depths);
                std::vector<glm::vec3> sky_positions(extent * extent, glm::vec3(0));
                sky_positions[center] = reference[center];
                Input(shader, "gPosition", GL_RGB32F, GL_RGB, sky_positions);
                shader.setInt("u_farWorldLadder", 0);
                const auto open = Draw();
                Bind(shader, "gPosition", bad);
                shader.setInt("u_farWorldLadder", 1);
                Near(Draw(), open);
                shader.setInt("u_farWorldLadder", 0);
                const auto legacy = Draw();
                Depth(shader,
                      lighting ? "u_sceneDepth" : "gDepth",
                      std::vector<float>(extent * extent, 0));
                EXPECT_EQ(Draw(), legacy)
                    << "Legacy position reads ignore depth, including neighbors";
            }
        }
    }
}

TEST_F(FarWorldFogGl, ProductionSkyInvalidSamplesAndUnderwaterPreserveBackgrounds) {
    const char* fragments[]{"lighting_pass.frag",
                            "ssao.frag",
                            "ssao_gtao.frag",
                            "ssao_bilateral_upsample.frag",
                            "scent_decal.frag",
                            "debug_view.frag",
                            "volumetric_lighting.frag"};
    for (const auto fragment : fragments) {
        SCOPED_TRACE(fragment);
        auto program = Production(fragment);
        ASSERT_TRUE(program->IsValid());
        auto& shader = *program;
        const bool aerial = std::string(fragment) == "volumetric_lighting.frag";
        const bool ao = std::string(fragment).find("ssao") == 0;
        if (!aerial)
            Input(shader,
                  "gPosition",
                  GL_RGB16F,
                  GL_RGB,
                  std::vector<glm::vec3>(extent * extent, {0, 0, -16000}));
        shader.setInt("u_farWorldLadder", 1);
        shader.setInt("u_mode", 5);
        const char* depth_name =
            std::string(fragment) == "lighting_pass.frag" ? "u_sceneDepth" : "gDepth";
        for (const float depth : {0.0f, .5f}) {
            Depth(shader, depth_name, std::vector<float>(extent * extent, depth));
            for (const auto inverse :
                 {glm::mat4(0), glm::mat4(std::numeric_limits<float>::quiet_NaN())}) {
                shader.setMat4("u_gbufferInverseProjection", inverse);
                shader.setMat4("u_inverseProjection", inverse);
                const auto actual = Draw();
                EXPECT_EQ(actual.r, ao ? 1.0f : 0.0f);
                EXPECT_EQ(actual.g, 0);
                EXPECT_EQ(actual.b, 0);
                if (std::string(fragment) == "debug_view.frag" && depth == 0) {
                    // Depth diagnostics retain the hardware-depth sky fallback;
                    // sky still has no position in Position/Normal/Material views.
                    shader.setInt("u_mode", 3);
                    EXPECT_EQ(Draw().r, 1);
                    shader.setInt("u_mode", 5);
                }
                if (aerial) {
                    shader.setFloat("u_underwater", 1);
                    const auto murk = Draw();
                    EXPECT_GT(murk.a, .9f);
                    EXPECT_LE(murk.a, .92f);
                    Near(glm::vec4(glm::vec3(murk), 0), {.04f, .18f, .26f, 0}, 1e-6f);
                    shader.setFloat("u_underwater", 0);
                }
            }
        }
    }
}
} // namespace
