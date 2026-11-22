//  (  — the pilot-pass seam): the two default-OFF pilot
// passes (DebugView + GroundDecal) now take a const RenderContext& and read their
// whole input from it. A byte-identical headless flip is VACUOUS for both (DebugView
// is mode None by default; GroundDecal has no scent mirror headless), so this drives
// each pass's execute(ctx) ACTIVE path against a real hidden GL context with synthetic
// inputs and proves it reads the context handles it declares:
//   * DebugView Albedo mode outputs ctx.gbuffer_albedo verbatim -> the readback pixel
//     equals the distinctive albedo we planted (proves gbuffer_albedo is wired);
//   * GroundDecal with a strong food mirror + in-grid positions tints the target amber
//     (proves gbuffer_position + the scent path run);
//   * each pass is a true no-op when OFF (mode None / inactive mirror) -> target stays
//     as cleared.
#include "luminumbra_client/core/IsolationConfig.h"
#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarWorldReplayGl.h"
#include "luminumbra_client/rendering/Mesh.h"
#include "luminumbra_client/rendering/RenderContext.h"
#include "luminumbra_client/rendering/RenderPipeline.h"
#include "luminumbra_client/rendering/RenderResourceRegistry.h"
#include "luminumbra_client/rendering/ScentFieldRenderMirror.h"
#include "luminumbra_client/rendering/WaterVisibility.h"
#include "luminumbra_client/rendering/passes/DebugViewPass.h"
#include "luminumbra_client/rendering/passes/FoliagePass.h"
#include "luminumbra_client/rendering/passes/FroxelPass.h"
#include "luminumbra_client/rendering/passes/GBufferPass.h"
#include "luminumbra_client/rendering/passes/GlassOitPass.h"
#include "luminumbra_client/rendering/passes/GroundDecalPass.h"
#include "luminumbra_client/rendering/passes/LightingPass.h"
#include "luminumbra_client/rendering/passes/ParticlePass.h"
#include "luminumbra_client/rendering/passes/PlantProcgenPass.h"
#include "luminumbra_client/rendering/passes/ShadowPass.h"
#include "luminumbra_client/rendering/passes/SkyboxPass.h"
#include "luminumbra_client/rendering/passes/SsaoPass.h"
#include "luminumbra_client/rendering/passes/StaticDrawPass.h"
#include "luminumbra_client/rendering/passes/TaauPass.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/core/GraphicsPresets.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include <glm/gtc/type_ptr.hpp>
#include <luminumbra/rendering/RenderView.h>
#include <luminumbra/rendering/StaticScene.h>

#define GLFW_INCLUDE_NONE
#include "luminumbra_client/rendering/Shader.h"
#include <GLFW/glfw3.h>
#include <cstdlib>
#include <cstring>
#include <glad/glad.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

namespace {

using Luminumbra::Rendering::DebugViewPass;
using Luminumbra::Rendering::GroundDecalPass;
using Luminumbra::Rendering::RenderContext;
using Luminumbra::Rendering::ScentFieldRenderMirror;
using Luminumbra::Rendering::TextureHandle;

class HiddenGlContext {
public:
    HiddenGlContext() {
        if (!glfwInit()) {
            m_error = "glfwInit failed";
            return;
        }
        m_glfw_initialized = true;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        m_window = glfwCreateWindow(64, 64, "pass_context_test", nullptr, nullptr);
        if (!m_window) {
            m_error = "glfwCreateWindow failed (no GL 4.5 context)";
            return;
        }
        glfwMakeContextCurrent(m_window);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            m_error = "gladLoadGLLoader failed";
            return;
        }
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(
            [](GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* message, const void*) {
                if (type == GL_DEBUG_TYPE_ERROR)
                    ADD_FAILURE() << "GL debug error: " << message;
            },
            nullptr);
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        glDepthFunc(GL_GREATER);
        glClearDepth(0.0);
        m_ready = true;
    }
    ~HiddenGlContext() {
        if (m_window)
            glfwDestroyWindow(m_window);
        if (m_glfw_initialized)
            glfwTerminate();
    }
    bool ready() const {
        return m_ready;
    }
    const std::string& error() const {
        return m_error;
    }

private:
    GLFWwindow* m_window = nullptr;
    bool m_glfw_initialized = false;
    bool m_ready = false;
    std::string m_error;
};

// A small RGBA16F texture holding per-texel float content.
GLuint MakeFloatTexture(int w, int h, const std::vector<float>& rgba /*w*h*4*/) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// An RGBA8 render target + FBO, cleared to opaque black.
struct RenderTarget {
    GLuint tex = 0;
    GLuint fbo = 0;
    int w = 0, h = 0;
};
RenderTarget MakeTarget(int w, int h) {
    RenderTarget rt;
    rt.w = w;
    rt.h = h;
    glGenTextures(1, &rt.tex);
    glBindTexture(GL_TEXTURE_2D, rt.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.tex, 0);
    const GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, bufs);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    return rt;
}
std::vector<unsigned char> ReadTarget(const RenderTarget& rt) {
    std::vector<unsigned char> px(
        static_cast<std::size_t>(rt.w) * static_cast<std::size_t>(rt.h) * 4, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, rt.w, rt.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
}

// Fixed-size state oracle: include enables these passes own as well as those they preserve.
std::array<GLint, 9> PassBoundaryState() {
    constexpr std::array<GLenum, 7> enables{GL_DEPTH_TEST,
                                            GL_CULL_FACE,
                                            GL_BLEND,
                                            GL_POLYGON_OFFSET_FILL,
                                            GL_SCISSOR_TEST,
                                            GL_FRAMEBUFFER_SRGB,
                                            GL_RASTERIZER_DISCARD};
    std::array<GLint, 9> state{};
    for (std::size_t index = 0; index < enables.size(); ++index)
        state[index] = glIsEnabled(enables[index]);
    glGetIntegerv(GL_DEPTH_FUNC, &state[7]);
    glGetIntegerv(GL_DEPTH_WRITEMASK, &state[8]);
    return state;
}

TEST(PassContext, LightingAndSkyboxKnownAndExternalEntriesAreEquivalent) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra::Rendering;
    RenderResourceRegistry resources;
    LightingPass lighting;
    SkyboxPass skybox;
    const auto target = MakeTarget(32, 32);
    // A complete zero-depth input selects the studio background in the actual lighting shader.
    const GLuint input = MakeFloatTexture(1, 1, {0, 0, 0, 0});
    GLuint quad = 0, buffer = 0;
    struct Cleanup {
        RenderResourceRegistry& resources;
        LightingPass& lighting;
        SkyboxPass& skybox;
        RenderTarget target;
        GLuint input;
        GLuint& quad;
        GLuint& buffer;
        ~Cleanup() {
            lighting.destroy_lighting_fbo(resources);
            skybox.destroy_geometry();
            glDeleteFramebuffers(1, &target.fbo);
            glDeleteTextures(1, &target.tex);
            glDeleteTextures(1, &input);
            glDeleteVertexArrays(1, &quad);
            glDeleteBuffers(1, &buffer);
        }
    } cleanup{resources, lighting, skybox, target, input, quad, buffer};
    lighting.init_shader(LUMINUMBRA_SOURCE_ROOT);
    lighting.init_lighting_fbo(resources, 32, 32);
    skybox.init_shader(LUMINUMBRA_SOURCE_ROOT);
    skybox.init_geometry();
    ASSERT_TRUE(lighting.shader()->IsValid());
    ASSERT_TRUE(skybox.shader()->IsValid());
    constexpr std::array<float, 20> vertices{-1, -1, 0, 0, 0, 1, -1, 0, 1, 0,
                                             -1, 1,  0, 0, 1, 1, 1,  0, 1, 1};
    glGenVertexArrays(1, &quad);
    glGenBuffers(1, &buffer);
    glBindVertexArray(quad);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          5 * sizeof(float),
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    Camera camera;
    RenderViewDescription description;
    description.width = description.height = 32;
    description.revision = 1;
    description.near_plane = camera.GetNearPlane();
    description.far_plane = camera.GetFarPlane();
    const auto view_matrix = camera.GetViewMatrix();
    const auto projection = camera.GetProjectionMatrix(32, 32);
    std::copy_n(glm::value_ptr(view_matrix), 16, description.view.begin());
    std::copy_n(glm::value_ptr(projection), 16, description.projection.begin());
    const auto view = RenderView::Validate(description);
    Luminumbra::Client::ScenarioHarness::IsolationConfig isolation;
    WeatherRenderState weather;
    std::size_t sky_draws = 0;
    RenderContext ctx;
    ctx.screen_width = ctx.screen_height = 32;
    ctx.camera = &camera;
    ctx.render_view = &view;
    ctx.static_studio = true;
    ctx.studio_background = {0.25f, 0.5f, 0.75f};
    ctx.authored_surface = TextureHandle{input};
    ctx.gbuffer_depth = TextureHandle{input};
    ctx.screen_quad_vao = quad;
    ctx.isolation = &isolation;
    ctx.weather_state = &weather;
    ctx.skybox_draw_counter = &sky_draws;
    ctx.sun.direction = {0, -1, 0};
    ctx.sky_day_factor = 1;
    ctx.moon_direction = {0, 1, 0};
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    // Two enable values, normal/frozen dispatch, and both skybox overlay entry choices.
    for (const bool replay : {false, true}) {
        ctx.frozen_replay = replay;
        for (const GLboolean enabled : {GLboolean{GL_FALSE}, GLboolean{GL_TRUE}}) {
            for (const bool dome : {false, true}) {
                std::vector<unsigned char> reference;
                std::array<GLint, 9> reference_state{};
                for (const bool external : {false, true}) {
                    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
                    glViewport(0, 0, 32, 32);
                    glClearColor(0, 0, 0, 0);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glDisable(GL_BLEND);
                    glDepthMask(GL_TRUE);
                    glEnable(GL_DEPTH_TEST);
                    glEnable(GL_CULL_FACE);
                    if (dome) {
                        if (!enabled)
                            glDisable(GL_CULL_FACE);
                        // No depth attachment reproduces the reduced-resolution dome target.
                        glDisable(GL_DEPTH_TEST);
                        const SkyboxPass::CallerState caller{.cull_enabled = enabled};
                        const auto before = sky_draws;
                        if (external)
                            skybox.execute_external(ctx, camera, replay);
                        else
                            skybox.execute(ctx, camera, replay, caller);
                        EXPECT_EQ(sky_draws, before + 1);
                        EXPECT_EQ(glIsEnabled(GL_CULL_FACE), enabled);
                    } else {
                        if (!enabled)
                            glDisable(GL_DEPTH_TEST);
                        const LightingPass::CallerState caller{.depth_test_enabled = enabled};
                        if (external)
                            lighting.execute_external(ctx);
                        else
                            lighting.execute(ctx, caller);
                        EXPECT_EQ(glIsEnabled(GL_DEPTH_TEST), enabled);
                    }
                    const auto state = PassBoundaryState();
                    const RenderTarget output =
                        dome ? target
                             : RenderTarget{lighting.lighting_fbo().color_texture,
                                            lighting.lighting_fbo().fbo_id,
                                            32,
                                            32};
                    const auto pixels = ReadTarget(output);
                    EXPECT_TRUE(std::any_of(pixels.begin(), pixels.end(), [](unsigned char value) {
                        return value != 0;
                    }));
                    if (external) {
                        EXPECT_EQ(state, reference_state);
                        EXPECT_EQ(pixels, reference);
                    } else {
                        reference_state = state;
                        reference = pixels;
                    }
                }
            }
        }
    }
    // Existing early validation remains a refusal before any GL state mutation.
    ctx.render_view = nullptr;
    ctx.camera = nullptr;
    const auto before = PassBoundaryState();
    EXPECT_THROW(lighting.execute(ctx, LightingPass::CallerState{}), std::invalid_argument);
    EXPECT_EQ(PassBoundaryState(), before);
    EXPECT_THROW(lighting.execute_external(ctx), std::invalid_argument);
    EXPECT_EQ(PassBoundaryState(), before);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(PassContext, FoliageKnownAndExternalEntriesAreEquivalentOnBothBackends) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra::Rendering;
    const auto target = MakeTarget(64, 64);
    struct TargetCleanup {
        RenderTarget target;
        ~TargetCleanup() {
            glDeleteFramebuffers(1, &target.fbo);
            glDeleteTextures(1, &target.tex);
        }
    } target_cleanup{target};
    Camera camera({8, 12.4f, 8}, {0, 1, 0}, 35, -18);
    RenderContext ctx;
    ctx.screen_width = ctx.screen_height = 64;
    ctx.lit_scene = FboHandle{target.fbo};
    ctx.sun.direction = {0, -1, 0};
    ctx.sun.color = {1, 1, 1};
    ctx.sun.intensity = 1;
    ctx.moon_light_dir = {0, -1, 0};
    ctx.exposure = 1;
    ctx.sky_ambient_color = {.2f, .2f, .2f};
    const auto ground = +[](void*, float, float) {
        return FoliagePass::SurfaceSample{10, 0, 1, true};
    };
    FoliagePass::ChunkScatter chunk;
    chunk.density = 1;
    chunk.biome_id = 1;
    for (const bool gpu : {false, true}) {
        FoliagePass pass;
        struct PassCleanup {
            FoliagePass& pass;
            ~PassCleanup() {
                pass.destroy_compute();
                pass.destroy_buffers();
            }
        } cleanup{pass};
        pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
        pass.init_buffers();
        ASSERT_TRUE(pass.shader()->IsValid());
        ASSERT_TRUE(pass.load_scatter_set(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                                          "data/common/foliage/scatter_set.json"));
        if (gpu) {
            pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
            ASSERT_TRUE(pass.gpu_scatter_active());
        }
        pass.rebuild_instances(
            {chunk}, ground, nullptr, camera.Position, FoliagePass::Preparation::Complete);
        for (const bool replay : {false, true}) {
            ctx.frozen_replay = replay;
            for (const GLboolean enabled : {GLboolean{GL_FALSE}, GLboolean{GL_TRUE}}) {
                std::vector<unsigned char> reference;
                std::array<GLint, 9> reference_state{};
                for (const bool external : {false, true}) {
                    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
                    glClearColor(0, 0, 0, 0);
                    glClear(GL_COLOR_BUFFER_BIT);
                    if (enabled)
                        glEnable(GL_CULL_FACE);
                    else
                        glDisable(GL_CULL_FACE);
                    const FoliagePass::CallerState caller{.cull_enabled = enabled};
                    if (external)
                        pass.execute_external(ctx, camera);
                    else
                        pass.execute(ctx, camera, caller);
                    EXPECT_EQ(glIsEnabled(GL_CULL_FACE), enabled);
                    EXPECT_GT(pass.last_draw_calls(), 0u);
                    const auto state = PassBoundaryState();
                    const auto pixels = ReadTarget(target);
                    EXPECT_TRUE(std::any_of(pixels.begin(), pixels.end(), [](unsigned char value) {
                        return value != 0;
                    }));
                    if (external) {
                        EXPECT_EQ(state, reference_state);
                        EXPECT_EQ(pixels, reference);
                    } else {
                        reference_state = state;
                        reference = pixels;
                    }
                }
            }
        }
        // Missing target, disabled pass and missing shader all retain the no-op boundary.
        for (const int phase : {0, 1, 2}) {
            ctx.lit_scene = phase == 0 ? FboHandle{} : FboHandle{target.fbo};
            pass.set_enabled(phase != 1);
            if (phase == 2)
                pass.reset_shader();
            const auto before = PassBoundaryState();
            EXPECT_EQ(pass.execute(ctx, camera, FoliagePass::CallerState{}), 0u);
            EXPECT_EQ(PassBoundaryState(), before);
            EXPECT_EQ(pass.execute_external(ctx, camera), 0u);
            EXPECT_EQ(PassBoundaryState(), before);
        }
        ctx.lit_scene = FboHandle{target.fbo};
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

// Scoped override of an environment variable, restored on destruction, so the test
// does not inherit (or leak) a render-scale setting from the surrounding session.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value)
        : m_name(name) {
        if (const char* previous = std::getenv(name)) {
            m_had_previous = true;
            m_previous = previous;
        }
        assign(name, value);
    }
    ~ScopedEnv() {
        assign(m_name, m_had_previous ? m_previous.c_str() : nullptr);
    }

private:
    // setenv/unsetenv are POSIX only; MSVC provides _putenv_s, where an empty value
    // removes the variable.
    static void assign(const char* name, const char* value) {
#if defined(_WIN32)
        _putenv_s(name, value ? value : "");
#else
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
#endif
    }

    const char* m_name;
    bool m_had_previous = false;
    std::string m_previous;
};

TEST(PassContext, FloatDepthFramebuffersSurviveResizeAndRenderScaleChange) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra::Rendering;
    // Pin the render scale for the duration of the test so the expected attachment
    // sizes below hold regardless of the caller's environment.
    ScopedEnv pinned_scale("LUMIN_RENDER_SCALE", "1.0");
    // Establish the CONVENTIONAL depth state first: the assertions after startup
    // then prove that startup itself installed the reversed convention, rather than
    // observing state some earlier fixture happened to leave behind.
    glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
    glDepthFunc(GL_LESS);
    glClearDepth(1.0);
    RenderPipeline pipeline;
    ASSERT_TRUE(pipeline.startup(64, 48, LUMINUMBRA_SOURCE_ROOT));
    const auto check = [&](int width, int height) {
        const auto& gbuffer = pipeline.gbuffer();
        const auto& lighting = pipeline.lighting_fbo();
        GLint value = 0;
        glBindTexture(GL_TEXTURE_2D, gbuffer.depth_texture);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &value);
        EXPECT_EQ(value, GL_DEPTH_COMPONENT32F);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &value);
        EXPECT_EQ(value, width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &value);
        EXPECT_EQ(value, height);
        std::array<float, 4> border{};
        glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border.data());
        EXPECT_EQ(border, (std::array<float, 4>{0, 0, 0, 0}));
        glBindRenderbuffer(GL_RENDERBUFFER, lighting.depth_texture);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &value);
        EXPECT_EQ(value, GL_DEPTH_COMPONENT32F);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &value);
        EXPECT_EQ(value, width);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &value);
        EXPECT_EQ(value, height);
        for (GLuint fbo : {gbuffer.fbo_id, lighting.fbo_id}) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
        }
        // Blitting depth requires matching formats, not just individually complete FBOs.
        glBindFramebuffer(GL_FRAMEBUFFER, gbuffer.fbo_id);
        glDepthMask(GL_TRUE);
        glClearDepth(0.125);
        glClear(GL_DEPTH_BUFFER_BIT);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer.fbo_id);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lighting.fbo_id);
        glBlitFramebuffer(
            0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, lighting.fbo_id);
        float copied = 0;
        glReadPixels(width / 2, height / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &copied);
        EXPECT_FLOAT_EQ(copied, 0.125f);
        glClearDepth(0.0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    };
    GLint mode = 0, func = 0;
    GLdouble clear = 1;
    glGetIntegerv(GL_CLIP_DEPTH_MODE, &mode);
    glGetIntegerv(GL_DEPTH_FUNC, &func);
    glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clear);
    EXPECT_EQ(mode, GL_ZERO_TO_ONE);
    EXPECT_EQ(func, GL_GREATER);
    EXPECT_DOUBLE_EQ(clear, 0.0);
    check(64, 48);
    pipeline.on_resize(80, 60);
    check(80, 60);
    pipeline.set_render_scale(0.5f);
    check(40, 30);
    pipeline.shutdown();
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(PassContext, GlassUsesSharedRenderbufferDepthAndSurvivesRecreation) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();
    using namespace Luminumbra::Rendering;
    GlassOitPass pass;
    EXPECT_FALSE(pass.has_targets());
    Camera camera(glm::vec3(0, 0, 2));
    GLuint pane_vao = 0, quad_vao = 0;
    std::array<GLuint, 2> buffers{};
    glGenBuffers(2, buffers.data());
    glGenVertexArrays(1, &pane_vao);
    glBindVertexArray(pane_vao);
    constexpr float pane_vertices[] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0};
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(pane_vertices), pane_vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glGenVertexArrays(1, &quad_vao);
    glBindVertexArray(quad_vao);
    constexpr float quad[] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1, 1, 1, 0, 1, 1};
    glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    std::vector<GlassPaneItem> panes(1);
    panes[0].model = glm::mat4(1);
    panes[0].tint = glm::vec3(0.2f, 0.8f, 0.3f);
    panes[0].thickness = 1;
    GlassOitPassInput input{&panes, pane_vao, LUMINUMBRA_SOURCE_ROOT};
    for (int extent : {16, 32}) {
        const RenderTarget target = MakeTarget(extent, extent);
        const GLuint opaque = MakeFloatTexture(
            extent,
            extent,
            std::vector<float>(
                static_cast<std::size_t>(extent) * static_cast<std::size_t>(extent) * 4, 0.8f));
        GLuint depth = 0;
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, extent, extent);
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
        RenderContext ctx;
        ctx.camera = &camera;
        ctx.screen_width = static_cast<unsigned>(extent);
        ctx.screen_height = static_cast<unsigned>(extent);
        ctx.lit_scene = FboHandle{target.fbo};
        ctx.lit_scene_depth = RenderbufferHandle{depth};
        ctx.opaque_scene = TextureHandle{opaque};
        ctx.screen_quad_vao = quad_vao;
        // Convert the synthetic identity projection's [-1,1] clip depth to
        // reversed [1,0], preserving the pane's screen footprint and depth .5.
        ctx.projection[2][2] = -0.5f;
        ctx.projection[3][2] = 0.5f;
        // A .75 foreground surface hides the pane; cleared sky at 0 admits it.
        for (bool occluded : {true, false}) {
            glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
            glDepthMask(GL_TRUE);
            glClearDepth(occluded ? 0.75 : 0.0);
            glClearColor(0.8f, 0.8f, 0.8f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            pass.execute_accum(ctx, input);
            EXPECT_TRUE(pass.has_targets());
            pass.execute_resolve(ctx, input);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            const auto pixels = ReadTarget(target);
            const std::size_t center =
                (static_cast<std::size_t>(extent / 2) * static_cast<std::size_t>(extent) +
                 static_cast<std::size_t>(extent / 2)) *
                4;
            if (occluded) {
                for (int channel = 0; channel < 3; ++channel) {
                    EXPECT_NEAR(pixels[center + static_cast<std::size_t>(channel)], 204, 1);
                }
            } else {
                EXPECT_LT(pixels[center], 100) << "visible glass must tint the lit scene";
                EXPECT_GT(pixels[center + 1], pixels[center] + 60);
            }
        }
        // A spatially varying background must stay registered with the pane when
        // the output resolution changes independently of the internal OIT extent.
        std::vector<float> background(static_cast<std::size_t>(extent) *
                                      static_cast<std::size_t>(extent) * 4);
        for (int y = 0; y < extent; ++y) {
            for (int x = 0; x < extent; ++x) {
                const auto offset =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(extent) +
                     static_cast<std::size_t>(x)) *
                    4;
                background[offset] = static_cast<float>(x) / static_cast<float>(extent - 1);
                background[offset + 1] = static_cast<float>(y) / static_cast<float>(extent - 1);
                background[offset + 2] = 0.2f;
                background[offset + 3] = 1.0f;
            }
        }
        glBindTexture(GL_TEXTURE_2D, opaque);
        glTexSubImage2D(
            GL_TEXTURE_2D, 0, 0, 0, extent, extent, GL_RGBA, GL_FLOAT, background.data());
        panes[0].tint = glm::vec3(1.0f);
        ctx.internal_width = static_cast<unsigned>(extent);
        ctx.internal_height = static_cast<unsigned>(extent);
        std::vector<unsigned char> full_scale;
        for (int output_multiple : {1, 2, 4}) {
            SCOPED_TRACE(output_multiple);
            ctx.screen_width = static_cast<unsigned>(extent * output_multiple);
            ctx.screen_height = static_cast<unsigned>(extent * output_multiple);
            glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
            glDepthMask(GL_TRUE);
            glClearDepth(0.0);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            pass.execute_accum(ctx, input);
            pass.execute_resolve(ctx, input);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            const auto pixels = ReadTarget(target);
            if (output_multiple == 1) {
                full_scale = pixels;
                const std::size_t near_corner =
                    (static_cast<std::size_t>(extent / 4) * static_cast<std::size_t>(extent) +
                     static_cast<std::size_t>(extent / 4)) *
                    4;
                const std::size_t far_corner =
                    (static_cast<std::size_t>(3 * extent / 4) * static_cast<std::size_t>(extent) +
                     static_cast<std::size_t>(3 * extent / 4)) *
                    4;
                EXPECT_GT(pixels[far_corner], pixels[near_corner] + 60);
                EXPECT_GT(pixels[far_corner + 1], pixels[near_corner + 1] + 60);
            } else {
                EXPECT_EQ(pixels, full_scale)
                    << "glass refraction must use the internal scene's pixel coordinates";
            }
        }
        panes[0].tint = glm::vec3(0.2f, 0.8f, 0.3f);
        pass.destroy(); // the pipeline destroys OIT before replacing its shared depth on resize
        EXPECT_FALSE(pass.has_targets());
        glDeleteRenderbuffers(1, &depth);
        glDeleteTextures(1, &opaque);
        glDeleteTextures(1, &target.tex);
        glDeleteFramebuffers(1, &target.fbo);
    }
    glDeleteVertexArrays(1, &pane_vao);
    glDeleteVertexArrays(1, &quad_vao);
    glDeleteBuffers(2, buffers.data());
}

TEST(PassContext, DebugViewAlbedoModeReadsGbufferAlbedoFromContext) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();

    DebugViewPass pass;
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    pass.init_buffers();

    // A distinctive flat albedo the Albedo debug mode should echo verbatim.
    const int W = 16, H = 16;
    std::vector<float> albedo(static_cast<std::size_t>(W) * H * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(W) * H; ++i) {
        albedo[i * 4 + 0] = 0.30f;
        albedo[i * 4 + 1] = 0.60f;
        albedo[i * 4 + 2] = 0.90f;
        albedo[i * 4 + 3] = 1.0f;
    }
    const std::vector<float> zeros(static_cast<std::size_t>(W) * H * 4, 0.0f);
    const GLuint albedoTex = MakeFloatTexture(W, H, albedo);
    const GLuint zeroTex = MakeFloatTexture(W, H, zeros);

    RenderTarget rt = MakeTarget(W, H);

    RenderContext ctx;
    ctx.camera = nullptr; // near/far fall back to defaults (unused in Albedo mode)
    ctx.screen_width = W;
    ctx.screen_height = H;
    ctx.gbuffer_albedo = TextureHandle{albedoTex};
    ctx.gbuffer_position = TextureHandle{zeroTex};
    ctx.gbuffer_normal = TextureHandle{zeroTex};
    ctx.gbuffer_depth = TextureHandle{zeroTex};

    // OFF (mode None): true no-op, the target stays cleared black.
    pass.set_mode(DebugViewPass::None);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glViewport(0, 0, W, H);
    pass.execute(ctx);
    glFinish();
    {
        const auto px = ReadTarget(rt);
        EXPECT_EQ(px[0], 0) << "mode None must be a no-op";
        EXPECT_EQ(px[1], 0);
        EXPECT_EQ(px[2], 0);
    }

    // ON (Albedo): the pass samples ctx.gbuffer_albedo and writes it out.
    pass.set_mode(DebugViewPass::Albedo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glViewport(0, 0, W, H);
    pass.execute(ctx);
    glFinish();
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    const auto px = ReadTarget(rt);
    // 0.30/0.60/0.90 -> ~77/153/230 in 8-bit; allow generous slack for gamma/encoding.
    EXPECT_NEAR(px[0], 77, 24) << "Albedo mode should echo ctx.gbuffer_albedo.r";
    EXPECT_NEAR(px[1], 153, 24) << "Albedo mode should echo ctx.gbuffer_albedo.g";
    EXPECT_NEAR(px[2], 230, 24) << "Albedo mode should echo ctx.gbuffer_albedo.b";

    pass.destroy_buffers();
    glDeleteTextures(1, &albedoTex);
    glDeleteTextures(1, &zeroTex);
}

TEST(PassContext, GroundDecalTintsFromContextPositionAndScentMirror) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();

    GroundDecalPass pass;
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    pass.init_buffers();

    // A 16-cell food-only scent mirror over a 16x16 world span at origin (0,0).
    const int CELLS = 16;
    ScentFieldRenderMirror mirror;
    mirror.resize(CELLS);
    mirror.cell_size = 1.0f;
    mirror.origin_x = 0.0f;
    mirror.origin_z = 0.0f;
    mirror.valid = true;
    mirror.any_scent = true;
    for (std::size_t k = 0; k < static_cast<std::size_t>(CELLS) * CELLS; ++k) {
        mirror.rg[k * 2 + 0] = 5.0f; // strong food trail (R); home (G) left at 0
    }

    // View-space positions covering the whole [0,16] world span (camera == identity),
    // so every screen pixel projects into the grid. y = 1 keeps dot(viewPos)>0.
    const int W = 32, H = 32;
    std::vector<float> pos(static_cast<std::size_t>(W) * H * 4);
    for (int j = 0; j < H; ++j) {
        for (int i = 0; i < W; ++i) {
            const std::size_t o =
                (static_cast<std::size_t>(j) * W + static_cast<std::size_t>(i)) * 4;
            pos[o + 0] = (i + 0.5f) / W * 16.0f; // world X in [0,16]
            pos[o + 1] = 1.0f;                   // nonzero -> not rejected as sky
            pos[o + 2] = (j + 0.5f) / H * 16.0f; // world Z in [0,16]
            pos[o + 3] = 1.0f;
        }
    }
    const GLuint posTex = MakeFloatTexture(W, H, pos);

    RenderTarget rt = MakeTarget(W, H);

    RenderContext ctx;
    ctx.camera = nullptr; // inverse_view falls back to identity -> world == view
    ctx.screen_width = W;
    ctx.screen_height = H;
    ctx.gbuffer_position = TextureHandle{posTex};

    // update_scent uploads the RG16F mirror and flips active on.
    pass.update_scent(mirror);
    ASSERT_TRUE(pass.active());

    // The host sets additive blend before the decal draw (mirrors RenderPipeline).
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glViewport(0, 0, W, H);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    pass.execute(ctx);
    glDisable(GL_BLEND);
    glFinish();
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    const auto px = ReadTarget(rt);
    // Amber FOOD_COLOR (0.95,0.62,0.18) at full intensity -> R clearly dominant, non-black.
    std::size_t tinted = 0;
    for (std::size_t p = 0; p < static_cast<std::size_t>(W) * H; ++p) {
        if (px[p * 4 + 0] > 80 && px[p * 4 + 0] > px[p * 4 + 2])
            ++tinted;
    }
    EXPECT_GT(tinted, static_cast<std::size_t>(W) * H / 2)
        << "food trail should tint most of the ground amber";

    // No-op guard: an all-zero (any_scent=false) mirror deactivates the pass.
    ScentFieldRenderMirror empty;
    empty.resize(CELLS);
    empty.valid = true;
    empty.any_scent = false;
    pass.update_scent(empty);
    EXPECT_FALSE(pass.active());
    RenderTarget rt2 = MakeTarget(W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, rt2.fbo);
    glViewport(0, 0, W, H);
    pass.execute(ctx);
    glFinish();
    const auto px2 = ReadTarget(rt2);
    EXPECT_EQ(px2[0], 0) << "inactive mirror must be a no-op";

    pass.destroy_buffers();
    glDeleteTextures(1, &posTex);
}

TEST(PassContext, ShadowCascadesDiscardPreviousFrameDepth) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();

    using namespace Luminumbra::Rendering;
    RenderResourceRegistry registry;
    ShadowPass pass;
    constexpr int resolution = 8;
    pass.shadow_map().resolution = resolution;
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    pass.init_shadow_map(registry);
    ASSERT_NE(pass.shadow_map().fbo_id, 0u);
    ASSERT_EQ(glGetError(), GL_NO_ERROR) << "shadow initialization must use complete framebuffers";
    std::vector<unsigned char> tint(resolution * resolution * ShadowMap::CASCADE_COUNT * 4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, pass.tint_texture_array());
    glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, tint.data());
    EXPECT_TRUE(std::all_of(tint.begin(), tint.end(), [](unsigned char v) { return v == 255; }))
        << "empty shadow tint must transmit all light in every cascade";

    ShadowPassInput input;
    input.light_space_matrices.assign(ShadowMap::CASCADE_COUNT, glm::mat4(1.0f));
    input.submit_terrain = [](const glm::vec4* planes) {
        GLint mode = 0, func = 0;
        glGetIntegerv(GL_CLIP_DEPTH_MODE, &mode);
        glGetIntegerv(GL_DEPTH_FUNC, &func);
        EXPECT_EQ(mode, GL_NEGATIVE_ONE_TO_ONE);
        EXPECT_EQ(func, GL_LESS);
        EXPECT_FLOAT_EQ(planes[4].w, 1.0f); // identity light matrix: near at -1
        EXPECT_FLOAT_EQ(planes[5].w, 1.0f); // far at +1
        return TerrainSubmitStats{};
    };
    const RenderContext ctx;
    std::vector<float> depths(resolution * resolution * ShadowMap::CASCADE_COUNT);
    for (int frame = 0; frame < 3; ++frame) {
        // An occluder from the previous frame has left every cascade occupied.
        // The next empty frame must restore visibility in every layer.
        const float stale_depth = 0.2f + 0.1f * static_cast<float>(frame);
        glClearTexImage(
            pass.shadow_map().depth_texture_array, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &stale_depth);
        glDepthMask(GL_FALSE); // A preceding transparent pass may disable writes.
        glClearDepth(0.0);     // The shadow pass owns its depth-clear convention.
        pass.execute_external(ctx, input);
        GLint mode = 0, func = 0;
        GLdouble clear = 1;
        glGetIntegerv(GL_CLIP_DEPTH_MODE, &mode);
        glGetIntegerv(GL_DEPTH_FUNC, &func);
        glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clear);
        EXPECT_EQ(mode, GL_ZERO_TO_ONE);
        EXPECT_EQ(func, GL_GREATER);
        EXPECT_DOUBLE_EQ(clear, 0.0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, pass.shadow_map().depth_texture_array);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depths.data());
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        for (int cascade = 0; cascade < ShadowMap::CASCADE_COUNT; ++cascade) {
            const auto begin = depths.begin() + cascade * resolution * resolution;
            EXPECT_TRUE(std::all_of(
                begin, begin + resolution * resolution, [](float v) { return v == 1.0f; }))
                << "frame " << frame << ", cascade " << cascade;
        }
    }
    pass.destroy_shadow_map(registry);
}

void ExpectShadowRasterState(const Luminumbra::Rendering::ShadowPass::CallerState& expected) {
    const auto actual = Luminumbra::Rendering::ShadowPass::CallerState::QueryExternal();
    EXPECT_EQ(actual.depth.function, expected.depth.function);
    EXPECT_EQ(actual.depth.write_mask, expected.depth.write_mask);
    EXPECT_EQ(actual.depth.test_enabled, expected.depth.test_enabled);
    EXPECT_EQ(actual.clip_origin, expected.clip_origin);
    EXPECT_EQ(actual.clip_depth, expected.clip_depth);
    EXPECT_EQ(actual.clear_depth, expected.clear_depth);
    EXPECT_EQ(actual.cull_face, expected.cull_face);
    EXPECT_EQ(actual.cull_enabled, expected.cull_enabled);
    // Neither entry point may change polygon offset, including its dormant values.
    EXPECT_EQ(glIsEnabled(GL_POLYGON_OFFSET_FILL), GL_TRUE);
    GLfloat factor = 0, units = 0;
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &factor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &units);
    EXPECT_EQ(factor, -2.5f);
    EXPECT_EQ(units, 3.25f);
}

TEST(PassContext, ShadowKnownAndExternalRasterRestorationAreEquivalent) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra::Rendering;
    RenderResourceRegistry registry;
    ShadowPass pass;
    pass.shadow_map().resolution = 8;
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    ASSERT_TRUE(pass.init_shadow_map(registry));
    struct Cleanup {
        ShadowPass& pass;
        RenderResourceRegistry& registry;
        GLuint vao = 0;
        GLuint vbo = 0;
        ~Cleanup() {
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &vbo);
            pass.destroy_shadow_map(registry);
        }
    } cleanup{pass, registry, 0, 0};
    glGenVertexArrays(1, &cleanup.vao);
    glGenBuffers(1, &cleanup.vbo);
    const std::array<float, 18> vertices{
        -1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0};
    glBindVertexArray(cleanup.vao);
    glBindBuffer(GL_ARRAY_BUFFER, cleanup.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    ShadowPassInput input;
    input.light_space_matrices.assign(ShadowMap::CASCADE_COUNT, glm::mat4(1.0f));
    unsigned submissions = 0;
    input.submit_terrain = [&](const glm::vec4(&)[6]) {
        ++submissions;
        return TerrainSubmitStats{};
    };
    const std::vector<GlassPaneItem> glass{
        {.model = glm::mat4(1.0f), .tint = glm::vec3(0.25f, 0.5f, 0.75f), .thickness = 1.0f}};
    input.glass_vao = cleanup.vao;
    // Three caller conventions, two entry points and four tint transitions.
    const std::array<ShadowPass::CallerState, 3> callers{
        ShadowPass::CallerState{
            .depth = {.function = GL_GREATER, .write_mask = GL_TRUE, .test_enabled = GL_TRUE},
            .clip_origin = GL_LOWER_LEFT,
            .clip_depth = GL_ZERO_TO_ONE,
            .clear_depth = 0.0,
            .cull_face = GL_BACK,
            .cull_enabled = GL_TRUE},
        ShadowPass::CallerState{
            .depth = {.function = GL_ALWAYS, .write_mask = GL_FALSE, .test_enabled = GL_FALSE},
            .clip_origin = GL_UPPER_LEFT,
            .clip_depth = GL_NEGATIVE_ONE_TO_ONE,
            .clear_depth = 0.375,
            .cull_face = GL_FRONT,
            .cull_enabled = GL_FALSE},
        ShadowPass::CallerState{
            .depth = {.function = GL_LEQUAL, .write_mask = GL_FALSE, .test_enabled = GL_TRUE},
            .clip_origin = GL_LOWER_LEFT,
            .clip_depth = GL_NEGATIVE_ONE_TO_ONE,
            .clear_depth = 1.0,
            .cull_face = GL_FRONT_AND_BACK,
            .cull_enabled = GL_TRUE}};
    const RenderContext context;
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-2.5f, 3.25f);
    for (const auto& caller : callers) {
        // Eight texels per side, four cascades: fixed-size readback oracle.
        std::array<std::array<float, 8 * 8 * ShadowMap::CASCADE_COUNT>, 4> reference_depth{};
        std::array<std::array<unsigned char, 8 * 8 * ShadowMap::CASCADE_COUNT * 4>, 4>
            reference_tint{};
        for (const bool external : {false, true}) {
            for (int phase = 0; phase < 4; ++phase) {
                SCOPED_TRACE(phase);
                input.glass_items = phase == 1 ? &glass : nullptr;
                caller.Restore();
                const unsigned before = submissions;
                if (external)
                    pass.execute_external(context, input);
                else
                    pass.execute(context, input, caller);
                EXPECT_EQ(submissions - before, ShadowMap::CASCADE_COUNT);
                EXPECT_EQ(pass.tint_dirty(), phase == 1);
                ExpectShadowRasterState(caller);
                const auto index = static_cast<std::size_t>(phase);
                auto depths = reference_depth[index];
                auto tints = reference_tint[index];
                glGetTextureImage(pass.shadow_map().depth_texture_array,
                                  0,
                                  GL_DEPTH_COMPONENT,
                                  GL_FLOAT,
                                  static_cast<GLsizei>(sizeof(depths)),
                                  depths.data());
                glGetTextureImage(pass.tint_texture_array(),
                                  0,
                                  GL_RGBA,
                                  GL_UNSIGNED_BYTE,
                                  static_cast<GLsizei>(sizeof(tints)),
                                  tints.data());
                if (external) {
                    EXPECT_EQ(depths, reference_depth[index]);
                    EXPECT_EQ(tints, reference_tint[index]);
                } else {
                    reference_depth[index] = depths;
                    reference_tint[index] = tints;
                }
                if (phase == 1)
                    EXPECT_TRUE(std::any_of(tints.begin(), tints.end(), [](unsigned char value) {
                        return value != 255;
                    }));
                else
                    EXPECT_TRUE(std::all_of(tints.begin(), tints.end(), [](unsigned char value) {
                        return value == 255;
                    }));
            }
        }
    }
    const auto& caller = callers[1];
    caller.Restore();
    input.submit_terrain = [](const glm::vec4(&)[6]) -> TerrainSubmitStats {
        throw std::runtime_error("terrain submission failed");
    };
    EXPECT_THROW(pass.execute(context, input, caller), std::runtime_error);
    ExpectShadowRasterState(caller);
    EXPECT_THROW(pass.execute_external(context, input), std::runtime_error);
    ExpectShadowRasterState(caller);
    // Missing matrices and resources must refuse before changing caller state.
    for (const std::size_t count : {0u, 3u}) {
        input.light_space_matrices.assign(count, glm::mat4(1.0f));
        pass.execute(context, input, caller);
        ExpectShadowRasterState(caller);
        pass.execute_external(context, input);
        ExpectShadowRasterState(caller);
    }
    pass.reset_shader();
    pass.execute(context, input, caller);
    ExpectShadowRasterState(caller);
    pass.execute_external(context, input);
    ExpectShadowRasterState(caller);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

} // namespace

TEST(PassContext, LivePrecipitationStartsStopsAndRetainsOtherEmitters) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();
    using Luminumbra::Rendering::ParticlePass;
    ParticlePass pass;
    pass.init_buffers();
    const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "data/common/particles";
    const auto ambient = pass.add_emitter(root / "fixture_sparkle.json", glm::vec3(0));
    ASSERT_NE(ambient, ParticlePass::kInvalidEmitter);
    pass.set_precipitation(root, glm::vec3(0), 0, 0);
    pass.rebuild_emitter_descriptors(1337, 0);
    ASSERT_EQ(pass.emitter_descriptors().size(), 1u);
    pass.set_precipitation(root, glm::vec3(0), 1, 0);
    pass.rebuild_emitter_descriptors(1337, 0);
    ASSERT_EQ(pass.emitter_descriptors().size(), 2u);
    pass.update(0.1f);
    EXPECT_GT(pass.frame_instance_count(), 400u)
        << "live rain must actually emit world-space drops";
    // A weather change retains the existing emitter identities and particles.
    pass.set_precipitation(root, glm::vec3(100, 200, -100), 0, 1);
    pass.rebuild_emitter_descriptors(1337, 1);
    ASSERT_EQ(pass.emitter_descriptors().size(), 3u);
    EXPECT_EQ(pass.emitter_descriptors().front().id, ambient);
    for (int i = 0; i < 20; ++i) {
        pass.set_precipitation(root, glm::vec3(100, 200, -100), 0, 1);
        pass.update(0.1f);
    }
    EXPECT_GT(pass.frame_instance_count(), 1000u) << "snow must emit beyond the old rain lifetime";
    pass.rebuild_emitter_descriptors(1337, 2);
    EXPECT_EQ(pass.emitter_descriptors().size(), 3u)
        << "repeated weather updates must not duplicate emitters";
    pass.set_precipitation(root, glm::vec3(100, 200, -100), 0, 0);
    for (int i = 0; i < 60; ++i)
        pass.update(0.1f);
    EXPECT_LT(pass.frame_instance_count(), 400u)
        << "precipitation must stop while ambient emission survives";
    EXPECT_GT(pass.frame_instance_count(), 0u);
    pass.clear_emitters();
    pass.set_precipitation(root, glm::vec3(0), 1, 0);
    pass.rebuild_emitter_descriptors(1337, 3);
    EXPECT_EQ(pass.emitter_descriptors().size(), 1u)
        << "world reset must recreate the weather emitter";
    pass.update(0.1f);
    EXPECT_GT(pass.frame_instance_count(), 400u);
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    pass.destroy_buffers();
}

TEST(PassContext, CpuAndGpuGrassTrackUploadedMeshReplacementAndRemoval) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();
    using Luminumbra::Rendering::FoliageGroundMesh;
    using Luminumbra::Rendering::FoliagePass;
    const auto fallback = +[](void*, float, float) {
        FoliagePass::SurfaceSample s;
        s.height = 100; // visibly wrong analytic surface; the uploaded mesh must win
        return s;
    };
    const std::vector<Luminumbra::u32> indices{0, 2, 3, 0, 3, 1};
    for (bool gpu : {false, true}) {
        SCOPED_TRACE(gpu ? "GPU" : "CPU");
        FoliagePass pass;
        pass.init_buffers();
        pass.set_readback_enabled(true);
        pass.use_rendered_ground();
        ASSERT_TRUE(pass.load_scatter_set(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                                          "data/common/foliage/scatter_set.json"));
        if (gpu) {
            pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
            ASSERT_TRUE(pass.gpu_scatter_active());
        }
        FoliagePass::ChunkScatter chunk;
        chunk.density = 0.5f;
        chunk.biome_id = 1;
        const auto rebuild = [&] {
            for (int frame = 0; frame < 5; ++frame) {
                pass.rebuild_instances({chunk}, fallback, nullptr, glm::vec3(16, 10, 16));
                glFinish(); // drain the actual asynchronous GPU readback on the next rebuild
            }
        };
        std::vector<Luminumbra::VoxelVertex> vertices{{{0, 8, 0}, {0, 1, 0}, 3},
                                                      {{32, 11.2f, 0}, {0, 1, 0}, 3},
                                                      {{0, 11.2f, 32}, {0, 1, 0}, 3},
                                                      {{32, 8, 32}, {0, 1, 0}, 3}};
        pass.update_ground_mesh(1, glm::ivec3(0), vertices, indices);
        rebuild();
        ASSERT_GT(pass.instances().size(), 100u);
        const FoliageGroundMesh original(glm::vec3(0), vertices, indices);
        for (const auto& blade : pass.instances()) {
            const auto ground = original.sample(blade.pos[0], blade.pos[2]);
            ASSERT_TRUE(ground.valid);
            EXPECT_NEAR(blade.pos[1], ground.height, 0.0001f);
        }
        const auto original_count = pass.instances().size();
        for (auto& vertex : vertices)
            vertex.position.y -= 2;
        pass.update_ground_mesh(1, glm::ivec3(0), vertices, indices);
        rebuild();
        ASSERT_EQ(pass.instances().size(), original_count);
        for (const auto& blade : pass.instances())
            EXPECT_NEAR(
                blade.pos[1], original.sample(blade.pos[0], blade.pos[2]).height - 2, 0.0001f);
        const auto first_position = pass.instances().front();
        chunk.biome_id = 2;
        rebuild();
        ASSERT_GT(pass.instances().size(), 100u);
        EXPECT_NE(pass.instances().front().pos[0], first_position.pos[0]);
        for (const auto& blade : pass.instances())
            EXPECT_NEAR(
                blade.pos[1], original.sample(blade.pos[0], blade.pos[2]).height - 2, 0.0001f);
        chunk.density = 0.1f;
        rebuild();
        EXPECT_LT(pass.instances().size(), original_count / 2);
        pass.remove_ground_mesh(1);
        rebuild();
        EXPECT_TRUE(pass.instances().empty()) << "unloaded terrain must not leave floating grass";
        pass.update_ground_mesh(1, glm::ivec3(0), vertices, indices);
        rebuild();
        ASSERT_FALSE(pass.instances().empty());
        pass.clear_ground_meshes();
        EXPECT_EQ(pass.frame_instance_count(), 0u);
        rebuild();
        EXPECT_TRUE(pass.instances().empty())
            << "world changes must discard all mesh and readback history";
        pass.destroy_compute();
        pass.destroy_buffers();
    }
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST(PassContext, AuthoredStaticDrawsRespectTerrainIsolation) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra::Rendering;
    namespace SH = Luminumbra::Client::ScenarioHarness;
    constexpr unsigned extent = 64;
    RenderResourceRegistry resources;
    GBufferPass pass;
    struct Cleanup {
        GBufferPass& pass;
        RenderResourceRegistry& resources;
        ~Cleanup() {
            pass.reset_shaders();
            pass.destroy_instanced_static_mesh();
            pass.destroy_gbuffer(resources);
        }
    } cleanup{pass, resources};
    pass.init_geometry_shader(LUMINUMBRA_SOURCE_ROOT);
    pass.init_instanced_static_mesh(LUMINUMBRA_SOURCE_ROOT);
    ASSERT_TRUE(pass.geometry_shader() && pass.geometry_shader()->IsValid());
    ASSERT_TRUE(pass.instanced_static_mesh_shader() &&
                pass.instanced_static_mesh_shader()->IsValid());
    pass.init_gbuffer(resources, extent, extent);
    ASSERT_NE(pass.gbuffer().fbo_id, 0u);

    Camera camera({0, 0, 0}, {0, 1, 0}, -90, 0);
    RenderViewDescription description;
    description.width = description.height = extent;
    description.revision = 1;
    description.near_plane = camera.GetNearPlane();
    description.far_plane = camera.GetFarPlane();
    const auto view_matrix = camera.GetViewMatrix();
    const auto projection = camera.GetProjectionMatrix(extent, extent);
    std::copy_n(glm::value_ptr(view_matrix), 16, description.view.begin());
    std::copy_n(glm::value_ptr(projection), 16, description.projection.begin());
    const auto view = RenderView::Validate(description);
    glm::vec4 planes[6];
    for (size_t i = 0; i < 6; ++i)
        planes[i] = glm::make_vec4(view.frustum_planes()[i].data());

    auto mesh = std::make_shared<StaticMesh>();
    mesh->identity = "isolation-quad";
    mesh->vertices = {{{-1, -1, -3}, {0, 0, 1}, {0, 0}},
                      {{1, -1, -3}, {0, 0, 1}, {1, 0}},
                      {{1, 1, -3}, {0, 0, 1}, {1, 1}},
                      {{-1, 1, -3}, {0, 0, 1}, {0, 1}}};
    mesh->indices = {0, 1, 2, 0, 2, 3};
    auto material = std::make_shared<StaticMaterial>();
    material->identity = "isolation-material";
    material->base_color = {.2, .6, .9, 1};
    auto snapshot = std::make_shared<StaticDrawSnapshot>();
    snapshot->revision = 1;
    StaticDraw draw;
    draw.key = {"placed", "quad", 0};
    draw.mesh = mesh;
    draw.material = material;
    draw.normal = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    snapshot->draws.push_back(std::move(draw));

    entt::registry entities;
    SH::IsolationConfig isolation;
    isolation.layer_mask = SH::IsolationLayer::Terrain;
    RenderContext context;
    context.camera = &camera;
    context.registry = &resources;
    context.screen_width = context.screen_height = extent;
    context.frustum_planes = planes;
    context.isolation = &isolation;
    unsigned terrain_submissions = 0;
    GBufferPassInput input;
    input.root_path = LUMINUMBRA_SOURCE_ROOT;
    input.authored_view = &view;
    input.authored_draws = snapshot;
    // No legacy terrain or entities: every covered pixel must come from the
    // actual authored pass reached through GBufferPass::execute.
    input.submit_terrain_chunks = [&](const glm::vec4(&)[6]) {
        ++terrain_submissions;
        return TerrainSubmitStats{};
    };
    glViewport(0, 0, extent, extent);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    const auto render = [&] {
        pass.execute(context, entities, input);
        std::vector<float> depth(extent * extent), authored(extent * extent * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, pass.gbuffer().fbo_id);
        glReadPixels(0, 0, extent, extent, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
        glReadBuffer(GL_COLOR_ATTACHMENT5);
        glReadPixels(0, 0, extent, extent, GL_RGBA, GL_FLOAT, authored.data());
        for (size_t i = 0; i < depth.size(); ++i)
            EXPECT_EQ(authored[i * 4 + 3], depth[i] > 0 ? 1.0f : 0.0f);
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
        return depth;
    };
    const auto enabled = render();
    const auto covered =
        std::count_if(enabled.begin(), enabled.end(), [](float depth) { return depth > 0; });
    ASSERT_GT(covered, 256);
    ASSERT_LT(covered, extent * extent);
    EXPECT_EQ(terrain_submissions, 1u);

    isolation.layer_mask = SH::IsolationLayer::Foliage;
    const auto suppressed = render();
    EXPECT_TRUE(
        std::all_of(suppressed.begin(), suppressed.end(), [](float depth) { return depth == 0; }));
    EXPECT_EQ(terrain_submissions, 1u);

    isolation.layer_mask = SH::IsolationLayer::Terrain;
    EXPECT_EQ(render(), enabled);
    EXPECT_EQ(terrain_submissions, 2u);

    // Exercise both StaticDrawPass boundaries against the same actual draws.
    // The pipeline already knows depth/target; an installed host may not.
    StaticDrawPass direct(LUMINUMBRA_SOURCE_ROOT);
    const StaticDrawPass::CallerState caller{
        .depth = {.function = GL_LEQUAL, .write_mask = GL_FALSE, .test_enabled = GL_FALSE},
        .draw_framebuffer = pass.gbuffer().fbo_id};
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, caller.draw_framebuffer);
    caller.depth.Restore();
    const auto expect_restored = [&] {
        const auto restored = PassDepthState::QueryExternal();
        EXPECT_EQ(restored.function, caller.depth.function);
        EXPECT_EQ(restored.write_mask, caller.depth.write_mask);
        EXPECT_EQ(restored.test_enabled, caller.depth.test_enabled);
        GLint framebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        EXPECT_EQ(static_cast<GLuint>(framebuffer), caller.draw_framebuffer);
    };
    const auto empty = std::make_shared<StaticDrawSnapshot>();
    const auto invalid = std::make_shared<StaticDrawSnapshot>(*snapshot);
    invalid->draws[0].model[0] = std::numeric_limits<double>::infinity();
    for (const bool known : {false, true}) {
        const auto stats =
            direct.Render(view, snapshot, pass.gbuffer(), false, known ? &caller : nullptr);
        EXPECT_EQ(stats.draws, 1u);
        expect_restored();
        EXPECT_EQ(
            direct.Render(view, empty, pass.gbuffer(), false, known ? &caller : nullptr).draws, 0u);
        expect_restored();
        EXPECT_THROW(direct.Render(view, snapshot, pass.gbuffer(), true, known ? &caller : nullptr),
                     std::invalid_argument);
        expect_restored();
        // This refusal happens after the GL guard has changed bindings/depth.
        EXPECT_THROW(direct.Render(view, invalid, pass.gbuffer(), false, known ? &caller : nullptr),
                     std::invalid_argument);
        expect_restored();
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(PassContext, FoliageCappedCompactionAndIndependentRebuildAreByteStable) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();
    using Luminumbra::Rendering::FoliagePass;
    const auto ground = +[](void*, float, float) {
        return FoliagePass::SurfaceSample{10, 0, 1, true};
    };
    std::vector<FoliagePass::ChunkScatter> chunks;
    for (int x = -3; x <= 3; ++x)
        for (int z = -3; z <= 3; ++z) {
            FoliagePass::ChunkScatter chunk;
            chunk.chunk_xz = {x, z};
            chunk.origin = {x * 32.0f, 0, z * 32.0f};
            chunk.density = 1;
            chunk.biome_id = 1;
            chunks.push_back(chunk);
        }
    std::vector<FoliagePass::InstanceRecord> reference;
    for (int run = 0; run < 2; ++run) {
        FoliagePass pass;
        pass.init_buffers();
        pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
        ASSERT_TRUE(pass.gpu_scatter_active());
        ASSERT_TRUE(pass.load_scatter_set(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                                          "data/common/foliage/scatter_set.json"));
        pass.set_fade_distances(100, 200);
        pass.set_density_scale(1.6f);
        pass.set_wind({0, 0});
        pass.set_readback_enabled(true);
        pass.set_evidence_enabled(true);
        for (std::uint64_t frame = 1; frame <= 4; ++frame) {
            pass.set_evidence_frame(frame, 1);
            pass.rebuild_instances(chunks, ground, nullptr, {8, 12.4f, 8});
            glFinish(); // Native test only: next loop consumes the actual async result.
        }
        ASSERT_EQ(pass.instances().size(), FoliagePass::kMaxInstances)
            << "must exercise saturation";
        if (run == 0)
            reference = pass.instances();
        else
            ASSERT_EQ(std::memcmp(reference.data(),
                                  pass.instances().data(),
                                  reference.size() * sizeof(reference[0])),
                      0)
                << "fresh output allocation and reversed chunk input order must preserve capped "
                   "bytes";
        ASSERT_TRUE(pass.begin_qualification());
        glFinish();
        pass.set_evidence_frame(5, 1);
        pass.rebuild_instances(chunks, ground, nullptr, {8, 12.4f, 8});
        const auto proof = pass.qualification_evidence().rebuild;
        EXPECT_TRUE(proof.completed);
        EXPECT_TRUE(proof.output_cleared);
        EXPECT_TRUE(proof.byte_equal);
        EXPECT_LT(proof.first_generation, proof.second_generation);
        EXPECT_EQ(proof.first_hash, proof.second_hash);
        pass.destroy_compute();
        pass.destroy_buffers();
        std::reverse(chunks.begin(), chunks.end());
    }
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST(PassContext, FoliageGpuCapacityCountsOnlyColumnsInsideTheScatterHorizon) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();
    using Luminumbra::Rendering::FoliagePass;
    const auto ground = +[](void* context, float, float) {
        ++*static_cast<std::size_t*>(context);
        return FoliagePass::SurfaceSample{10, 0, 1, true};
    };
    std::vector<FoliagePass::InstanceRecord> reference;
    std::size_t reference_queries = 0;
    // The live initial surface spans 25 x 25 columns. Its outer columns must
    // not force CPU fallback when the unchanged foliage horizon fits the GPU.
    for (const int radius : {6, 12}) {
        std::vector<FoliagePass::ChunkScatter> chunks;
        for (int x = -radius; x <= radius; ++x)
            for (int z = -radius; z <= radius; ++z) {
                FoliagePass::ChunkScatter chunk;
                chunk.chunk_xz = {x, z};
                chunk.origin = {x * 16.0f, 0, z * 16.0f};
                chunk.extent_m = 16;
                chunk.density = 1;
                chunk.biome_id = 1;
                chunks.push_back(chunk);
            }
        if (radius == 12)
            std::reverse(chunks.begin(), chunks.end());
        FoliagePass pass;
        pass.init_buffers();
        pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
        ASSERT_TRUE(pass.gpu_scatter_active());
        ASSERT_TRUE(pass.load_scatter_set(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                                          "data/common/foliage/scatter_set.json"));
        pass.set_fade_distances(48, 92);
        pass.set_density_scale(1.6f);
        pass.set_readback_enabled(true);
        pass.set_evidence_enabled(true);
        pass.set_wind({0, 0});
        std::size_t queries = 0;
        for (std::uint64_t frame = 1; frame <= 4; ++frame) {
            pass.set_evidence_frame(frame, 1);
            pass.rebuild_instances(chunks, ground, &queries, {8, 12.4f, 8});
            glFinish(); // Test only: retain the production nonblocking ring.
        }
        EXPECT_TRUE(pass.instances_from_gpu_readback()) << "surface radius " << radius;
        EXPECT_TRUE(pass.begin_qualification()) << "surface radius " << radius;
        EXPECT_EQ(pass.instances().size(), FoliagePass::kMaxInstances);
        if (radius == 6) {
            reference = pass.instances();
            reference_queries = queries;
        } else {
            EXPECT_EQ(queries, reference_queries) << "out-of-range columns must not sample ground";
            ASSERT_EQ(pass.instances().size(), reference.size());
            EXPECT_EQ(std::memcmp(reference.data(),
                                  pass.instances().data(),
                                  reference.size() * sizeof(reference[0])),
                      0)
                << "outer columns and reversed input order must preserve capped GPU membership";
        }
        pass.destroy_compute();
        pass.destroy_buffers();
    }
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST(PassContext, FoliageGpuCapacityKeepsTheExistingUploadedColumnLimit) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();
    using Luminumbra::Rendering::FoliagePass;
    const auto ground = +[](void*, float, float) {
        return FoliagePass::SurfaceSample{10, 0, 1, true};
    };
    for (const int live_columns : {512, 513}) {
        std::vector<FoliagePass::ChunkScatter> chunks;
        for (int i = 0; i < live_columns + 600; ++i) {
            FoliagePass::ChunkScatter chunk;
            chunk.chunk_xz = {i % 32, i / 32};
            chunk.origin = {(i % 32) * 16.0f, 0, (i / 32) * 16.0f};
            chunk.extent_m = 16;
            chunk.density = i < live_columns ? 1.0f : 0.0f;
            chunk.biome_id = 1;
            chunks.push_back(chunk);
        }
        FoliagePass pass;
        pass.init_buffers();
        pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
        ASSERT_TRUE(pass.gpu_scatter_active());
        ASSERT_TRUE(pass.load_scatter_set(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                                          "data/common/foliage/scatter_set.json"));
        // Deliberately enlarge only this synthetic fixture's horizon so every
        // positive-density column reaches admission. Production stays at 92 m.
        pass.set_fade_distances(900, 1000);
        pass.set_density_scale(1.6f);
        pass.set_readback_enabled(true);
        pass.set_evidence_enabled(true);
        pass.set_wind({0, 0});
        for (std::uint64_t frame = 1; frame <= 4; ++frame) {
            pass.set_evidence_frame(frame, 1);
            pass.rebuild_instances(chunks, ground, nullptr, {8, 12.4f, 8});
            glFinish();
        }
        EXPECT_EQ(pass.instances_from_gpu_readback(), live_columns == 512);
        if (live_columns == 512) {
            EXPECT_EQ(pass.qualification_blocker(), nullptr);
            EXPECT_TRUE(pass.begin_qualification());
            EXPECT_EQ(pass.instances().size(), FoliagePass::kMaxInstances);
        } else {
            EXPECT_STREQ(pass.qualification_blocker(), "gpu_path_inactive");
            EXPECT_FALSE(pass.begin_qualification());
            EXPECT_FALSE(pass.instances().empty()) << "over-cap keeps the existing CPU fallback";
        }
        pass.destroy_compute();
        pass.destroy_buffers();
    }
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
}

TEST(PassContext, FoliageActualDrawFeedbackSurvivesReloadAndPreservesFrameIdentity) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();
    using namespace Luminumbra::Rendering;
    FoliagePass pass;
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    pass.init_buffers();
    pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
    ASSERT_TRUE(pass.shader()->IsValid());
    ASSERT_TRUE(pass.shader()->Reload());
    ASSERT_TRUE(pass.gpu_scatter_active());
    ASSERT_TRUE(pass.load_scatter_set(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                                      "data/common/foliage/scatter_set.json"));
    pass.set_fade_distances(48, 92);
    pass.set_density_scale(1.6f);
    pass.set_readback_enabled(true);
    pass.set_evidence_enabled(true);
    Camera camera({8, 12.4f, 8}, {0, 1, 0}, 35, -18);
    camera.Zoom = 60;
    const auto ground = +[](void*, float, float) {
        return FoliagePass::SurfaceSample{10, 0, 1, true};
    };
    std::vector<FoliagePass::ChunkScatter> chunks;
    for (int x = -3; x <= 3; ++x)
        for (int z = -3; z <= 3; ++z) {
            FoliagePass::ChunkScatter chunk;
            chunk.chunk_xz = {x, z};
            chunk.origin = {x * 32.0f, 0, z * 32.0f};
            chunk.density = 1;
            chunk.biome_id = 1;
            chunks.push_back(chunk);
        }
    const auto target = MakeTarget(64, 64);
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    RenderContext ctx;
    ctx.screen_width = ctx.screen_height = 64;
    ctx.lit_scene = FboHandle{target.fbo};
    ctx.sun.direction = {0, -1, 0};
    ctx.sun.color = {1, 1, 1};
    ctx.sun.intensity = 1;
    ctx.sky_ambient_color = {.2f, .2f, .2f};
    std::uint32_t phase = 1;
    for (std::uint64_t frame = 1; frame <= 100; ++frame) {
        pass.set_evidence_frame(frame, phase);
        pass.set_wind(phase == 1 ? glm::vec2(0) : glm::vec2(6, 0));
        pass.rebuild_instances(chunks, ground, nullptr, camera.Position);
        ctx.time_seconds = static_cast<float>(frame); // Gate pins actual shader time independently.
        const auto drawn = pass.execute(ctx, camera);
        if (pass.vertex_submission_frame(phase) == frame) {
            EXPECT_GE(pass.last_draw_calls(), 2u);
            EXPECT_GT(drawn, 100000u);
            if (phase == 1)
                phase = 2;
        }
        glFinish();
        if (frame == 4) {
            ASSERT_TRUE(pass.begin_qualification());
        }
        if (pass.vertex_evidence_ready())
            break;
    }
    const auto proof = pass.qualification_evidence();
    ASSERT_TRUE(proof.rebuild.byte_equal);
    ASSERT_EQ(proof.vertices[0].vertices.size(), 768u);
    ASSERT_EQ(proof.vertices[1].vertices.size(), 768u);
    ASSERT_EQ(proof.gpu.size(), 64u);
    EXPECT_LT(proof.vertices[0].source_frame, proof.vertices[1].source_frame);
    for (std::size_t i = 0; i < proof.gpu.size(); ++i) {
        EXPECT_GT(proof.gpu[i].milliseconds, 0);
        EXPECT_EQ(proof.gpu[i].query_id, i + 1);
        EXPECT_NE(proof.gpu[i].source_frame, proof.vertices[0].source_frame);
        EXPECT_NE(proof.gpu[i].source_frame, proof.vertices[1].source_frame);
    }
    for (std::size_t blade = 0; blade < 64; ++blade)
        for (std::size_t j = 0; j < 12; ++j) {
            const auto& a = proof.vertices[0].vertices[blade * 12 + j];
            const auto& b = proof.vertices[1].vertices[blade * 12 + j];
            const double displacement =
                glm::distance(glm::dvec3(a[0], a[1], a[2]), glm::dvec3(b[0], b[1], b[2]));
            if (a[3] == 0 || !proof.vertices[0].sways[blade])
                EXPECT_LE(displacement, .00001);
            else {
                EXPECT_GT(displacement, .00001);
                EXPECT_LE(displacement, proof.vertices[0].blade_heights[blade] * .45 + .0001);
            }
        }
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    pass.destroy_compute();
    pass.destroy_buffers();
    pass.reset_shader();
    glDeleteFramebuffers(1, &target.fbo);
    glDeleteTextures(1, &target.tex);
}

TEST(PassContext, SsaoReinitializationKeepsExactlyTheOriginalKernelAndNoise) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra::Rendering;
    RenderResourceRegistry resources;
    SsaoPass pass;
    struct Cleanup {
        SsaoPass& pass;
        RenderResourceRegistry& resources;
        ~Cleanup() {
            pass.destroy_ssao(resources);
        }
    } cleanup{pass, resources};
    pass.init_ssao(resources, 8, 6);
    const auto original = pass.ssao().kernel;
    ASSERT_EQ(original.size(), 64u);
    std::array<float, 16 * 3> original_noise{};
    glBindTexture(GL_TEXTURE_2D, pass.ssao().noiseTexture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_FLOAT, original_noise.data());
    // Both unchanged and changed extents use the same default-seeded stream.
    for (const unsigned width : {8u, 12u, 1u}) {
        pass.destroy_ssao(resources);
        pass.init_ssao(resources, width, 6);
        ASSERT_EQ(pass.ssao().kernel.size(), 64u);
        EXPECT_EQ(pass.ssao().kernel, original);
        std::array<float, 16 * 3> noise{};
        glBindTexture(GL_TEXTURE_2D, pass.ssao().noiseTexture);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_FLOAT, noise.data());
        EXPECT_EQ(noise, original_noise);
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

namespace {
std::size_t ReplayTargetBytes(const Luminumbra::Rendering::RenderResourceRegistry& resources) {
    const auto* desc = resources.owned_texture_desc("taau_replay_output");
    return desc ? static_cast<std::size_t>(desc->width) * desc->height * 8u : 0u;
}

void ExpectReplayAllocationFailure(Luminumbra::Rendering::TaauPass& pass,
                                   Luminumbra::Rendering::RenderResourceRegistry& resources) {
    const auto original = pass.history_state();
    struct RejectStorage {
        PFNGLTEXIMAGE2DPROC original = glad_glTexImage2D;
        GLboolean debug = glIsEnabled(GL_DEBUG_OUTPUT);
        RejectStorage() {
            glDisable(GL_DEBUG_OUTPUT);
            glad_glTexImage2D =
                [](GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) {
                    // A zero level count deterministically refuses storage.
                    glTexStorage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 1, 1);
                };
        }
        ~RejectStorage() {
            glad_glTexImage2D = original;
            if (debug)
                glEnable(GL_DEBUG_OUTPUT);
        }
    } reject_storage;
    EXPECT_FALSE(pass.prepare_replay(resources));
    EXPECT_FALSE(pass.replay_available());
    EXPECT_EQ(resources.texture("taau_replay_output").id, 0u);
    EXPECT_EQ(pass.history_state(), original);
}

bool PrepareReplayPreservingBindings(Luminumbra::Rendering::TaauPass& pass,
                                     Luminumbra::Rendering::RenderResourceRegistry& resources,
                                     GLuint texture,
                                     GLuint buffer) {
    using namespace Luminumbra::Rendering;
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    bool ready = false;
    {
        auto access = MakeFarWorldReplayGlAccess();
        const ReplayGlGuard guard(*access);
        ready = pass.prepare_replay(resources);
    }
    GLint binding = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &binding);
    EXPECT_EQ(binding, GL_TEXTURE4);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    EXPECT_EQ(static_cast<GLuint>(binding), texture);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &binding);
    EXPECT_EQ(static_cast<GLuint>(binding), buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    return ready;
}
} // namespace

TEST(PassContext, TaauReplayTargetBytesReturnToZeroAfterCompletionAndUnwind) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra::Rendering;
    RenderResourceRegistry resources;
    TaauPass pass;
    struct Cleanup {
        TaauPass& pass;
        RenderResourceRegistry& resources;
        ~Cleanup() {
            pass.destroy(resources);
        }
    } cleanup{pass, resources};
    pass.release_replay(resources);
    EXPECT_EQ(ReplayTargetBytes(resources), 0u);
    // Reinitialize while armed, then rearm against the new output extent.
    for (const unsigned width : {4u, 7u}) {
        pass.destroy(resources);
        pass.init(resources, width, 3);
        EXPECT_EQ(ReplayTargetBytes(resources), 0u);
        ASSERT_TRUE(pass.prepare_replay(resources));
        EXPECT_EQ(ReplayTargetBytes(resources), width * 3u * 8u);
    }
    pass.release_replay(resources);
    EXPECT_EQ(ReplayTargetBytes(resources), 0u);
    const auto history = pass.history_state();
    for (const bool interrupted : {false, true}) {
        GLuint scratch = 0;
        try {
            const ReplayRollback release([&]() noexcept { pass.release_replay(resources); });
            auto access = MakeFarWorldReplayGlAccess();
            RunFarWorldReplay(
                *access,
                [] {
                    return []() noexcept {
                    };
                },
                [] {},
                [&] {
                    EXPECT_TRUE(pass.prepare_replay(resources));
                    scratch = resources.texture("taau_replay_output").id;
                    EXPECT_NE(scratch, 0u);
                    EXPECT_EQ(ReplayTargetBytes(resources), 7u * 3u * 8u);
                    if (interrupted) {
                        throw std::runtime_error("interrupted replay");
                    }
                });
            EXPECT_FALSE(interrupted);
        } catch (const std::runtime_error&) {
            EXPECT_TRUE(interrupted);
        }
        EXPECT_EQ(ReplayTargetBytes(resources), 0u);
        EXPECT_FALSE(pass.replay_available());
        EXPECT_FALSE(glIsTexture(scratch));
        EXPECT_EQ(pass.history_state(), history);
        pass.release_replay(resources);
        EXPECT_EQ(ReplayTargetBytes(resources), 0u);
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(PassContext, TaauReplayStorageIsLazyReusableAndMatchesNormalResolveAfterResize) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra::Rendering;
    RenderResourceRegistry resources;
    TaauPass pass;
    struct Cleanup {
        TaauPass& pass;
        RenderResourceRegistry& resources;
        GLuint quad = 0, buffer = 0;
        std::array<GLuint, 3> inputs{};
        RenderTarget target;
        ~Cleanup() {
            pass.destroy(resources);
            glDeleteVertexArrays(1, &quad);
            glDeleteBuffers(1, &buffer);
            glDeleteTextures(static_cast<GLsizei>(inputs.size()), inputs.data());
            glDeleteFramebuffers(1, &target.fbo);
            glDeleteTextures(1, &target.tex);
        }
    } cleanup{pass, resources, 0, 0, {}, {}};
    EXPECT_FALSE(pass.prepare_replay(resources));
    pass.init(resources, 0, 6);
    EXPECT_FALSE(pass.prepare_replay(resources));
    EXPECT_EQ(resources.texture("taau_replay_output").id, 0u);
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    constexpr std::array<float, 20> vertices{-1, 1, 0, 0, 1, -1, -1, 0, 0, 0,
                                             1,  1, 0, 1, 1, 1,  -1, 0, 1, 0};
    glGenVertexArrays(1, &cleanup.quad);
    glGenBuffers(1, &cleanup.buffer);
    glBindVertexArray(cleanup.quad);
    glBindBuffer(GL_ARRAY_BUFFER, cleanup.buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
    // Two resize cycles cover full-size and scaled lighting inputs. Each cycle
    // checks both invalid first-frame history and accumulated valid history.
    for (const unsigned output_width : {8u, 12u}) {
        SCOPED_TRACE(output_width);
        pass.init(resources, output_width, 8);
        ASSERT_FALSE(pass.replay_available());
        ASSERT_EQ(resources.texture("taau_replay_output").id, 0u);
        EXPECT_EQ(ReplayTargetBytes(resources), 0u);
        const unsigned input_width = output_width == 8 ? output_width : output_width / 2;
        const unsigned input_height = output_width == 8 ? 8u : 4u;
        std::vector<float> pixels(input_width * input_height * 4);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<float>((i * 7) % 16) / 16.0f;
        }
        cleanup.inputs[0] =
            MakeFloatTexture(static_cast<int>(input_width), static_cast<int>(input_height), pixels);
        cleanup.inputs[1] = MakeFloatTexture(1, 1, {0, 0, 0, 0});
        cleanup.inputs[2] = MakeFloatTexture(1, 1, {1, 1, 1, 1});
        cleanup.target = MakeTarget(static_cast<int>(input_width), static_cast<int>(input_height));
        RenderContext ctx;
        ctx.screen_width = output_width;
        ctx.screen_height = 8;
        ctx.internal_width = input_width;
        ctx.internal_height = input_height;
        ctx.screen_quad_vao = cleanup.quad;
        ctx.lit_scene_color = TextureHandle{cleanup.inputs[0]};
        ctx.motion_vectors = TextureHandle{cleanup.inputs[1]};
        ctx.gbuffer_depth = TextureHandle{cleanup.inputs[2]};
        ctx.lit_scene = FboHandle{cleanup.target.fbo};
        const auto history = pass.history_state();
        const std::array<float, 4> seed{.25f, .5f, .75f, 1};
        for (GLuint texture : history.textures) {
            glClearTexImage(texture, 0, GL_RGBA, GL_FLOAT, seed.data());
        }
        for (bool valid : {false, true}) {
            const auto frozen = pass.history_state();
            ASSERT_EQ(frozen.valid, valid);
            pass.execute(ctx);
            const auto after_normal = pass.history_state();
            const auto expected_lighting = ReadTarget(cleanup.target);
            std::vector<std::uint16_t> expected(output_width * 8 * 4);
            const auto write_index = static_cast<std::size_t>(frozen.write_index);
            glBindTexture(GL_TEXTURE_2D, frozen.textures[write_index]);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_HALF_FLOAT, expected.data());
            if (!valid) {
                ASSERT_EQ(resources.texture("taau_replay_output").id, 0u);
                ExpectReplayAllocationFailure(pass, resources);
            }
            ASSERT_TRUE(PrepareReplayPreservingBindings(
                pass, resources, cleanup.inputs[2], cleanup.buffer));
            const auto scratch = resources.texture("taau_replay_output").id;
            ASSERT_NE(scratch, 0u);
            EXPECT_NE(scratch, frozen.textures[0]);
            EXPECT_NE(scratch, frozen.textures[1]);
            ASSERT_TRUE(pass.prepare_replay(resources));
            EXPECT_EQ(resources.texture("taau_replay_output").id, scratch);
            const auto* desc = resources.owned_texture_desc("taau_replay_output");
            ASSERT_NE(desc, nullptr);
            EXPECT_EQ(desc->width, output_width);
            EXPECT_EQ(desc->height, 8u);
            EXPECT_EQ(desc->internal_format, static_cast<unsigned>(GL_RGBA16F));
            EXPECT_EQ(ReplayTargetBytes(resources), output_width * 8u * 8u);
            pass.execute(ctx, &frozen);
            EXPECT_EQ(pass.history_state(), after_normal);
            EXPECT_EQ(ReadTarget(cleanup.target), expected_lighting);
            std::vector<std::uint16_t> actual(expected.size());
            glBindTexture(GL_TEXTURE_2D, scratch);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_HALF_FLOAT, actual.data());
            EXPECT_EQ(actual, expected);
            EXPECT_TRUE(std::any_of(actual.begin(), actual.end(), [](auto v) { return v != 0; }));
            glBindTexture(GL_TEXTURE_2D, frozen.textures[write_index]);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_HALF_FLOAT, actual.data());
            EXPECT_EQ(actual, expected);
            glBindTexture(GL_TEXTURE_2D, 0);
            pass.release_replay(resources);
            EXPECT_EQ(ReplayTargetBytes(resources), 0u);
            EXPECT_FALSE(glIsTexture(scratch));
            EXPECT_EQ(pass.history_state(), after_normal);
        }
        pass.destroy(resources);
        EXPECT_FALSE(pass.prepare_replay(resources));
        EXPECT_EQ(resources.texture("taau_replay_output").id, 0u);
        glDeleteTextures(static_cast<GLsizei>(cleanup.inputs.size()), cleanup.inputs.data());
        cleanup.inputs.fill(0);
        glDeleteFramebuffers(1, &cleanup.target.fbo);
        glDeleteTextures(1, &cleanup.target.tex);
        cleanup.target = {};
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(PassContext, QualificationUploadsDrainBeforeDispatchAndTemporalHistory) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra;
    using namespace Luminumbra::Rendering;
    ScopedEnv pinned_scale("LUMIN_RENDER_SCALE", "1.0");
    RenderPipeline pipeline;
    ASSERT_TRUE(pipeline.startup(64, 48, LUMINUMBRA_SOURCE_ROOT));
    pipeline.set_far_lod_enabled(false);
    pipeline.set_taau_enabled(true);
    Systems::TerrainGenParams params;
    params.biomes_enabled = params.structures_enabled = params.hydro_enabled = false;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    entt::registry registry;
    Camera camera({8, 80, 8}, {0, 1, 0}, -90, 0);
    CloudRenderState clouds;
    clouds.enabled = true;
    pipeline.set_cloud_state(clouds);
    pipeline.set_time_of_day(0.37f);
    pipeline.render_frame(registry, world, camera, 1.0f / 30.0f);
    const float time_of_day = pipeline.get_time_of_day();
    const auto sun_direction = pipeline.sun_direction();
    const auto cloud_offset = pipeline.get_cloud_state().scroll_offset;
    pipeline.set_taau_enabled(false);
    pipeline.set_taau_enabled(true);
    // Nine tiny public streaming adoptees exceed the eight-attempt ceiling,
    // independent of machine speed. No generation, workers or physics are needed.
    constexpr int chunks = 9;
    for (int x = 0; x < chunks; ++x) {
        auto chunk = std::make_shared<Chunk>(IVec3{x, 0, 0});
        chunk->mesh_vertices = {
            {{0, 0, 0}, {0, 1, 0}, 1}, {{0, 0, 1}, {0, 1, 0}, 1}, {{1, 0, 0}, {0, 1, 0}, 1}};
        chunk->mesh_indices = {0, 1, 2};
        chunk->mesh_version.store(1);
        chunk->set_state(ChunkState::Ready);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    }
    pipeline.render_frame(registry, world, camera, 0.0f, false, UploadPresentation::Settled);
    ASSERT_FALSE(pipeline.render_uploads_settled());
    EXPECT_TRUE(pipeline.frame_stage_trace().empty());
    EXPECT_FALSE(pipeline.taau_history_valid());
    EXPECT_EQ(pipeline.get_time_of_day(), time_of_day);
    EXPECT_EQ(pipeline.sun_direction(), sun_direction);
    EXPECT_EQ(pipeline.get_cloud_state().scroll_offset, cloud_offset);
    // The progress floor guarantees completion within one pass per chunk.
    for (int frame = 1; frame < chunks && !pipeline.render_uploads_settled(); ++frame) {
        pipeline.render_frame(registry, world, camera, 0.0f, false, UploadPresentation::Settled);
        EXPECT_EQ(pipeline.get_time_of_day(), time_of_day);
        EXPECT_EQ(pipeline.sun_direction(), sun_direction);
        EXPECT_EQ(pipeline.get_cloud_state().scroll_offset, cloud_offset);
        if (!pipeline.render_uploads_settled()) {
            EXPECT_TRUE(pipeline.frame_stage_trace().empty());
            EXPECT_FALSE(pipeline.taau_history_valid());
        }
    }
    ASSERT_TRUE(pipeline.render_uploads_settled());
    EXPECT_FALSE(pipeline.frame_stage_trace().empty());
    EXPECT_TRUE(pipeline.taau_history_valid());
    // A normal measured/interactive frame still dispatches with pending work.
    for (auto* chunk : world.get_renderable_chunks())
        chunk->mesh_version.store(2);
    pipeline.render_frame(registry, world, camera, 1.0f / 30.0f);
    EXPECT_GT(pipeline.get_time_of_day(), time_of_day);
    EXPECT_NE(pipeline.sun_direction(), sun_direction);
    EXPECT_NE(pipeline.get_cloud_state().scroll_offset, cloud_offset);
    EXPECT_FALSE(pipeline.render_uploads_settled());
    EXPECT_FALSE(pipeline.frame_stage_trace().empty());
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(PassContext, ShippingTemporalResolveBuildsHistoryJittersAndResets) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra;
    using namespace Luminumbra::Rendering;
    ScopedEnv pinned_scale("LUMIN_RENDER_SCALE", "1.0");
    RenderPipeline pipeline;
    ASSERT_TRUE(pipeline.startup(64, 48, LUMINUMBRA_SOURCE_ROOT));
    pipeline.set_far_lod_enabled(false);
    EXPECT_TRUE(pipeline.taau_enabled());
    EXPECT_EQ(pipeline.volumetric_quality(), 1);
    EXPECT_FALSE(pipeline.taau_history_valid());
    Systems::TerrainGenParams params;
    params.biomes_enabled = params.structures_enabled = params.hydro_enabled = false;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    entt::registry registry;
    Camera camera({8, 80, 8}, {0, 1, 0}, -90, 0);
    pipeline.render_frame(registry, world, camera, 1.0f / 30.0f);
    EXPECT_TRUE(pipeline.taau_history_valid());
    const auto first_jitter = pipeline.taau_jitter_ndc();
    EXPECT_NE(first_jitter, glm::vec2(0.0f));
    for (int replay = 0; replay < 2; ++replay) {
        ASSERT_TRUE(pipeline.render_far_world_off_pair(camera));
        EXPECT_TRUE(pipeline.taau_history_valid());
        EXPECT_EQ(pipeline.taau_jitter_ndc(), first_jitter);
    }
    pipeline.render_frame(registry, world, camera, 1.0f / 30.0f);
    EXPECT_NE(pipeline.taau_jitter_ndc(), first_jitter);
    pipeline.on_resize(80, 64);
    EXPECT_FALSE(pipeline.taau_history_valid());
    EXPECT_FALSE(pipeline.render_far_world_off_pair(camera));
    pipeline.render_frame(registry, world, camera, 1.0f / 30.0f);
    EXPECT_TRUE(pipeline.taau_history_valid());
    EXPECT_TRUE(pipeline.render_far_world_off_pair(camera));
    pipeline.set_taau_enabled(false);
    EXPECT_FALSE(pipeline.taau_history_valid());
    EXPECT_EQ(pipeline.taau_jitter_ndc(), glm::vec2(0.0f));
    pipeline.render_frame(registry, world, camera, 1.0f / 30.0f);
    EXPECT_FALSE(pipeline.taau_history_valid());
    pipeline.set_taau_enabled(true);
    pipeline.render_frame(registry, world, camera, 1.0f / 30.0f);
    EXPECT_TRUE(pipeline.taau_history_valid());
    EXPECT_NE(pipeline.taau_jitter_ndc(), glm::vec2(0.0f));
    // Settings transactions invalidate history even when render scale is unchanged.
    // Exercise every non-scale graphics control through the public renderer API.
    using namespace luminumbra::core;
    for (const GraphicsOverrides& overrides :
         {GraphicsOverrides{{"grass_density", 0.8}},
          GraphicsOverrides{{"grass_fade_start_m", 32.0}},
          GraphicsOverrides{{"grass_fade_end_m", 64.0}},
          GraphicsOverrides{{"far_terrain_radius_m", 1500.0}},
          GraphicsOverrides{{"shadow_resolution", 1024.0}},
          GraphicsOverrides{{"volumetric_quality", 0.0}},
          GraphicsOverrides{{"grass_enabled", false}},
          GraphicsOverrides{{"ui_blur", std::string("off")}}}) {
        GraphicsSettings settings;
        settings.overrides = overrides;
        ResolvedGraphics resolved;
        std::string error;
        ASSERT_TRUE(ResolveGraphics(settings, {}, resolved, error)) << error;
        ASSERT_TRUE(pipeline.apply_graphics(resolved));
        EXPECT_FALSE(pipeline.taau_history_valid());
        pipeline.render_frame(registry, world, camera, 1.0f / 30.0f);
        EXPECT_TRUE(pipeline.taau_history_valid());
        ASSERT_TRUE(pipeline.apply_graphics(resolved));
        EXPECT_TRUE(pipeline.taau_history_valid()); // Unchanged application preserves history.
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(PassContext, DeferredOnlyDiagnosticPolicyOmitsUnsupportedDrawsForOneAdmittedFrame) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra;
    using namespace Luminumbra::Rendering;
    using Policy = RenderPipeline::DiagnosticCapturePolicy;
    ScopedEnv pinned_scale("LUMIN_RENDER_SCALE", "1.0");
    RenderPipeline pipeline;
    pipeline.enable_static_model_content(false);
    ASSERT_TRUE(pipeline.startup(64, 48, LUMINUMBRA_SOURCE_ROOT));
    pipeline.set_far_world_mode(FarWorldMode::Legacy);
    pipeline.set_far_lod_enabled(false);
    pipeline.set_taau_enabled(false);
    pipeline.set_capture_diagnostics({.enabled = true, .probe = std::nullopt});
    Systems::TerrainGenParams params;
    params.biomes_enabled = params.structures_enabled = params.hydro_enabled = false;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    entt::registry registry;
    Camera camera({0, 0, 2}, {0, 1, 0}, -90, 0);
    const std::string mesh_path = "test/diagnostic_policy_triangle.lmesh";
    pipeline.register_procgen_mesh(mesh_path,
                                   MeshLoader::CreateFromArrays({{{-2, -2, -1}, {0, 0, 1}, {0, 0}},
                                                                 {{2, -2, -1}, {0, 0, 1}, {1, 0}},
                                                                 {{0, 2, -1}, {0, 0, 1}, {.5f, 1}}},
                                                                {0, 1, 2}));
    ASSERT_TRUE(pipeline.has_procgen_mesh(mesh_path));
    const auto prop = registry.create();
    registry.emplace<Components::TransformComponent>(prop);
    registry.emplace<Components::StaticMeshComponent>(prop).meshPath = mesh_path;
    pipeline.render_frame(registry, world, camera, 0.0f);
    ASSERT_TRUE(pipeline.diagnostic_capture_ready());
    const auto trace = pipeline.frame_stage_trace();
    const auto read_depth = [&] {
        float depth = 0;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, pipeline.gbuffer().fbo_id);
        glReadPixels(32, 24, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        return depth;
    };
    const float supported_depth = read_depth();
    ASSERT_GT(supported_depth, 0.0f);
    pipeline.render_frame(registry,
                          world,
                          camera,
                          0.0f,
                          false,
                          UploadPresentation::Settled,
                          Policy::DeferredOnlyFixedSite);
    ASSERT_TRUE(pipeline.diagnostic_capture_ready());
    EXPECT_FLOAT_EQ(read_depth(), supported_depth);

    Camera mismatched_camera = camera;
    mismatched_camera.Position.x += 1.0f;
    EXPECT_FALSE(pipeline.render_far_world_off_pair(mismatched_camera));
    EXPECT_TRUE(pipeline.diagnostic_capture_ready());
    EXPECT_TRUE(pipeline.diagnostic_capture_manifest().render_settings.contains("capture_policy"));
    ASSERT_TRUE(pipeline.render_far_world_off_pair(camera));
    EXPECT_FALSE(pipeline.diagnostic_capture_ready());
    EXPECT_FALSE(pipeline.diagnostic_capture_manifest().render_settings.contains("capture_policy"));

    // A visible plant plus active decals/glass would make A4 refuse. The
    // policy must omit the draws, preserve their inputs, and keep the trace.
    const std::vector<PlantProcgenPass::Vertex> vertices = {
        {{-1, -1, 0}, {0, 0, 1}, {0, 0}, {1, 0, 0}},
        {{1, -1, 0}, {0, 0, 1}, {0, 0}, {1, 0, 0}},
        {{0, 1, 0}, {0, 0, 1}, {0, 0}, {1, 0, 0}}};
    pipeline.plant_procgen()->set_plants(vertices, {0, 1, 2}, 1);
    pipeline.plant_procgen()->set_enabled(true);
    ScentFieldRenderMirror scent;
    scent.resize(2);
    scent.valid = scent.any_scent = true;
    std::fill(scent.rg.begin(), scent.rg.end(), 1.0f);
    pipeline.UpdateScentDecals(scent);
    pipeline.set_glass_panes({GlassPaneItem{}});
    for (const auto policy : {Policy::DeferredOnlyFixedSite,
                              Policy::Complete,
                              Policy::DeferredOnlyFixedSite,
                              static_cast<Policy>(99)}) {
        pipeline.render_frame(
            registry, world, camera, 0.0f, false, UploadPresentation::Settled, policy);
        const bool deferred = policy == Policy::DeferredOnlyFixedSite;
        EXPECT_EQ(pipeline.diagnostic_capture_ready(), deferred);
        EXPECT_EQ(pipeline.frame_stage_trace(), trace);
        const auto settings = pipeline.diagnostic_capture_manifest().render_settings;
        EXPECT_EQ(settings.contains("capture_policy"), deferred);
        if (deferred) {
            const auto* policy_name = std::get_if<std::string>(&settings.at("capture_policy"));
            ASSERT_NE(policy_name, nullptr);
            EXPECT_EQ(*policy_name, "deferred_only_fixed_site");
            EXPECT_FLOAT_EQ(read_depth(), supported_depth);
        } else {
            EXPECT_GT(read_depth(), supported_depth);
        }
        EXPECT_TRUE(pipeline.plant_procgen()->enabled());
        EXPECT_EQ(pipeline.plant_procgen()->index_count(), 3u);
    }
    // The default call must retain the refusal after an explicit policy call.
    pipeline.render_frame(registry,
                          world,
                          camera,
                          0.0f,
                          false,
                          UploadPresentation::Settled,
                          Policy::DeferredOnlyFixedSite);
    ASSERT_TRUE(pipeline.diagnostic_capture_ready());
    pipeline.render_frame(registry, world, camera, 0.0f);
    EXPECT_FALSE(pipeline.diagnostic_capture_ready());
    EXPECT_FALSE(pipeline.diagnostic_capture_manifest().render_settings.contains("capture_policy"));
    // No suppression when instrumentation is disarmed, disabled or inadmissible.
    for (int failure = 0; failure < 3; ++failure) {
        pipeline.set_diagnostic_capture_armed(failure != 0);
        pipeline.set_capture_diagnostics({.enabled = failure != 1, .probe = std::nullopt});
        pipeline.set_taau_enabled(failure == 2);
        pipeline.render_frame(registry,
                              world,
                              camera,
                              0.0f,
                              false,
                              UploadPresentation::Settled,
                              Policy::DeferredOnlyFixedSite);
        EXPECT_FALSE(
            pipeline.diagnostic_capture_manifest().render_settings.contains("capture_policy"));
        if (failure != 1) {
            EXPECT_FALSE(pipeline.diagnostic_capture_ready());
        }
        EXPECT_GT(read_depth(), supported_depth);
    }
    pipeline.end_diagnostic_capture();
    EXPECT_FALSE(pipeline.diagnostic_capture_manifest().render_settings.contains("capture_policy"));
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(PassContext, DeferredOnlyWeatherOmitsSceneOverlayAndCompleteRetainsRefusal) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra;
    using namespace Luminumbra::Rendering;
    using Policy = RenderPipeline::DiagnosticCapturePolicy;
    ScopedEnv pinned_scale("LUMIN_RENDER_SCALE", "1.0");
    RenderPipeline pipeline;
    pipeline.enable_static_model_content(false);
    ASSERT_TRUE(pipeline.startup(64, 48, LUMINUMBRA_SOURCE_ROOT));
    pipeline.set_far_world_mode(FarWorldMode::Legacy);
    pipeline.set_far_lod_enabled(false);
    pipeline.set_taau_enabled(false);
    pipeline.set_capture_diagnostics({.enabled = true, .probe = std::nullopt});
    Systems::TerrainGenParams params;
    params.biomes_enabled = params.structures_enabled = params.hydro_enabled = false;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    entt::registry registry;
    Camera camera({0, 0, 2}, {0, 1, 0}, -90, 0);
    for (const auto weather : {WeatherType::None,
                               WeatherType::Rain,
                               WeatherType::Snow,
                               WeatherType::Fog,
                               WeatherType::Storm}) {
        for (const float intensity : {0.0f, 1.0f}) {
            pipeline.set_weather(weather, intensity);
            for (const auto policy : {Policy::DeferredOnlyFixedSite, Policy::Complete}) {
                pipeline.render_frame(
                    registry, world, camera, 0.0f, false, UploadPresentation::Settled, policy);
                const bool complete = policy == Policy::Complete;
                const bool active = weather != WeatherType::None && intensity > 0;
                EXPECT_EQ(pipeline.diagnostic_capture_ready(), !complete || !active);
                EXPECT_EQ(pipeline.diagnostic_capture_refusal(),
                          complete && active ? "producer_coverage_incomplete" : "");
                EXPECT_EQ(pipeline.get_weather_type(), weather);
            }
        }
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(PassContext, OmittedWaterInvalidatesVisibilityAcrossPolicyAndIsolationChanges) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra;
    using namespace Luminumbra::Rendering;
    using namespace Luminumbra::Client::ScenarioHarness;
    using Policy = RenderPipeline::DiagnosticCapturePolicy;
    ScopedEnv pinned_scale("LUMIN_RENDER_SCALE", "1.0");
    RenderPipeline pipeline;
    pipeline.enable_static_model_content(false);
    ASSERT_TRUE(pipeline.startup(64, 48, LUMINUMBRA_SOURCE_ROOT));
    pipeline.set_far_world_mode(FarWorldMode::Legacy);
    pipeline.set_far_lod_enabled(false);
    pipeline.set_taau_enabled(false);
    pipeline.set_water_visibility_capture_enabled(true);
    pipeline.set_capture_diagnostics({.enabled = true, .probe = std::nullopt});
    Systems::TerrainGenParams params;
    params.biomes_enabled = params.structures_enabled = params.hydro_enabled = false;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    auto chunk = std::make_shared<Chunk>(IVec3(0));
    chunk->set_state(ChunkState::Ready);
    chunk->water_mesh_vertices = {{{0, 6, 0}, {0, 1, 0}, 0u},
                                  {{0, 6, 16}, {0, 1, 0}, 0u},
                                  {{16, 6, 0}, {0, 1, 0}, 0u},
                                  {{16, 6, 16}, {0, 1, 0}, 0u}};
    chunk->water_mesh_indices = {0u, 1u, 2u, 2u, 1u, 3u};
    chunk->water_mesh_version.store(1u);
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    entt::registry registry;
    Camera camera({8, 12, 8}, {0, 1, 0}, -90, -89);
    for (const bool isolate_water : {false, true}) {
        pipeline.set_diagnostic_capture_armed(false);
        pipeline.render_frame(registry, world, camera, 0.0f);
        ASSERT_EQ(pipeline.get_last_render_pass_stats().water_draws, 1u);
        ASSERT_TRUE(pipeline.water_visibility_mask().valid());
        ASSERT_GT(pipeline.water_visibility_mask().visible_pixels_in_target(), 0u);
        pipeline.set_diagnostic_capture_armed(true);
        if (isolate_water) {
            pipeline.set_isolation_config(
                {.layer_mask = IsolationLayer::All & ~IsolationLayer::Water,
                 .backdrop = BackdropMode::Scene});
        }
        for (int repeat = 0; repeat < 2; ++repeat) {
            pipeline.render_frame(registry,
                                  world,
                                  camera,
                                  0.0f,
                                  false,
                                  UploadPresentation::Settled,
                                  isolate_water ? Policy::Complete : Policy::DeferredOnlyFixedSite);
            EXPECT_EQ(pipeline.get_last_render_pass_stats().water_draws, 0u);
            pipeline.end_diagnostic_capture();
            pipeline.set_isolation_config({});
            EXPECT_FALSE(pipeline.water_visibility_mask().valid());
            if (isolate_water) {
                pipeline.set_isolation_config(
                    {.layer_mask = IsolationLayer::All & ~IsolationLayer::Water,
                     .backdrop = BackdropMode::Scene});
            }
        }
        pipeline.set_isolation_config({});
    }
    pipeline.set_diagnostic_capture_armed(false);
    pipeline.render_frame(registry, world, camera, 0.0f);
    ASSERT_TRUE(pipeline.water_visibility_mask().valid());
    pipeline.set_water_visibility_capture_enabled(false);
    pipeline.render_frame(registry, world, camera, 0.0f);
    pipeline.set_water_visibility_capture_enabled(true);
    EXPECT_FALSE(pipeline.water_visibility_mask().valid());
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(PassContext, PairedReplayRefusesColdGlassWithoutChangingReducedCapture) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra;
    using namespace Luminumbra::Rendering;
    using Policy = RenderPipeline::DiagnosticCapturePolicy;
    ScopedEnv pinned_scale("LUMIN_RENDER_SCALE", "1.0");
    RenderPipeline pipeline;
    pipeline.enable_static_model_content(false);
    ASSERT_TRUE(pipeline.startup(64, 48, LUMINUMBRA_SOURCE_ROOT));
    pipeline.set_far_world_mode(FarWorldMode::Legacy);
    pipeline.set_far_lod_enabled(false);
    pipeline.set_taau_enabled(false);
    pipeline.set_capture_diagnostics({.enabled = true, .probe = std::nullopt});
    Systems::TerrainGenParams params;
    params.biomes_enabled = params.structures_enabled = params.hydro_enabled = false;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    entt::registry registry;
    Camera camera({0, 0, 2}, {0, 1, 0}, -90, 0);
    pipeline.set_glass_panes({GlassPaneItem{}});
    const auto reduced = [&] {
        pipeline.render_frame(registry,
                              world,
                              camera,
                              0.0f,
                              false,
                              UploadPresentation::Settled,
                              Policy::DeferredOnlyFixedSite);
    };
    reduced();
    ASSERT_TRUE(pipeline.diagnostic_capture_ready());
    const auto trace = pipeline.frame_stage_trace();
    for (int repeat = 0; repeat < 2; ++repeat) {
        EXPECT_FALSE(pipeline.render_far_world_off_pair(camera));
        EXPECT_TRUE(pipeline.diagnostic_capture_ready());
        EXPECT_TRUE(
            pipeline.diagnostic_capture_manifest().render_settings.contains("capture_policy"));
        EXPECT_EQ(pipeline.frame_stage_trace(), trace);
    }
    pipeline.end_diagnostic_capture();
    EXPECT_FALSE(pipeline.render_far_world_off_pair(camera));
    pipeline.set_glass_panes({});
    EXPECT_TRUE(pipeline.render_far_world_off_pair(camera));
    pipeline.set_glass_panes({GlassPaneItem{}});
    pipeline.render_frame(registry, world, camera, 0.0f);
    EXPECT_FALSE(pipeline.diagnostic_capture_ready());
    reduced();
    ASSERT_TRUE(pipeline.diagnostic_capture_ready());
    EXPECT_TRUE(pipeline.render_far_world_off_pair(camera));
    EXPECT_FALSE(pipeline.diagnostic_capture_ready());
    pipeline.on_resize(80, 60);
    reduced();
    ASSERT_TRUE(pipeline.diagnostic_capture_ready());
    EXPECT_FALSE(pipeline.render_far_world_off_pair(camera));
    EXPECT_TRUE(pipeline.diagnostic_capture_ready());
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

namespace {

// Real compilation/storage with deterministic failures at the public GL boundary.
// At most two initialization attempts create four programs and four textures.
class FroxelInitCalls {
public:
    FroxelInitCalls() {
        current() = this;
        glad_glGetShaderiv = ShaderStatus;
        glad_glGetProgramResourceiv = ResourceProperties;
        glad_glCreateProgram = CreateProgram;
        glad_glDeleteProgram = DeleteProgram;
        glad_glGenTextures = GenTextures;
        glad_glDeleteTextures = DeleteTextures;
        glad_glTexStorage3D = TexStorage;
        glad_glObjectLabel = ObjectLabel;
        glad_glDispatchCompute = Dispatch;
    }
    ~FroxelInitCalls() {
        glad_glGetShaderiv = shader_status;
        glad_glGetProgramResourceiv = resource_properties;
        glad_glCreateProgram = create_program;
        glad_glDeleteProgram = delete_program;
        glad_glGenTextures = gen_textures;
        glad_glDeleteTextures = delete_textures;
        glad_glTexStorage3D = tex_storage;
        glad_glObjectLabel = object_label;
        glad_glDispatchCompute = dispatch;
        current() = nullptr;
    }
    FroxelInitCalls(const FroxelInitCalls&) = delete;
    FroxelInitCalls& operator=(const FroxelInitCalls&) = delete;

    int failed_shader = 0;
    int failed_texture = 0;
    int failed_storage = 0;
    bool invalid_layout = false;
    bool label_exception = false;
    int shader_checks = 0;
    int texture_calls = 0;
    int storage_calls = 0;
    int live_programs = 0;
    int live_textures = 0;
    int dispatches = 0;

private:
    static FroxelInitCalls*& current() {
        static FroxelInitCalls* instance = nullptr;
        return instance;
    }
    static void GLAPIENTRY ShaderStatus(GLuint shader, GLenum property, GLint* value) {
        auto& calls = *current();
        calls.shader_status(shader, property, value);
        if (property == GL_COMPILE_STATUS && ++calls.shader_checks == calls.failed_shader)
            *value = GL_FALSE;
    }
    static void GLAPIENTRY ResourceProperties(GLuint program,
                                              GLenum interface_type,
                                              GLuint index,
                                              GLsizei count,
                                              const GLenum* properties,
                                              GLsizei size,
                                              GLsizei* length,
                                              GLint* values) {
        auto& calls = *current();
        calls.resource_properties(
            program, interface_type, index, count, properties, size, length, values);
        if (calls.invalid_layout && interface_type == GL_UNIFORM) {
            for (GLsizei offset = 0; offset < count && offset < size; ++offset) {
                if (properties[offset] == GL_TYPE && values[offset] == GL_SAMPLER_2D_ARRAY)
                    values[offset] = GL_SAMPLER_2D;
            }
        }
    }
    static GLuint GLAPIENTRY CreateProgram() {
        auto& calls = *current();
        const GLuint program = calls.create_program();
        if (program)
            ++calls.live_programs;
        return program;
    }
    static void GLAPIENTRY DeleteProgram(GLuint program) {
        auto& calls = *current();
        if (program)
            --calls.live_programs;
        calls.delete_program(program);
    }
    static void GLAPIENTRY GenTextures(GLsizei count, GLuint* textures) {
        auto& calls = *current();
        EXPECT_EQ(count, 1);
        if (++calls.texture_calls == calls.failed_texture) {
            textures[0] = 0;
            return;
        }
        calls.gen_textures(count, textures);
        calls.live_textures += count;
    }
    static void GLAPIENTRY DeleteTextures(GLsizei count, const GLuint* textures) {
        auto& calls = *current();
        for (GLsizei index = 0; index < count; ++index) {
            if (textures[index])
                --calls.live_textures;
        }
        calls.delete_textures(count, textures);
    }
    static void GLAPIENTRY TexStorage(GLenum target,
                                      GLsizei levels,
                                      GLenum format,
                                      GLsizei width,
                                      GLsizei height,
                                      GLsizei depth) {
        auto& calls = *current();
        if (++calls.storage_calls != calls.failed_storage)
            calls.tex_storage(target, levels, format, width, height, depth);
    }
    static void GLAPIENTRY ObjectLabel(GLenum type,
                                       GLuint name,
                                       GLsizei length,
                                       const char* label) {
        auto& calls = *current();
        if (calls.label_exception && std::strcmp(label, "froxel.integrated") == 0)
            throw std::runtime_error("volume label failure");
        calls.object_label(type, name, length, label);
    }
    static void GLAPIENTRY Dispatch(GLuint, GLuint, GLuint) {
        auto& calls = *current();
        // Verify injection/integration bind real allocated images without running
        // a kernel against unrelated shadow fixtures.
        GLint texture = 0;
        glGetIntegeri_v(GL_IMAGE_BINDING_NAME, 0, &texture);
        EXPECT_NE(texture, 0);
        EXPECT_EQ(glIsTexture(static_cast<GLuint>(texture)), GL_TRUE);
        ++calls.dispatches;
    }
    PFNGLGETSHADERIVPROC shader_status = glad_glGetShaderiv;
    PFNGLGETPROGRAMRESOURCEIVPROC resource_properties = glad_glGetProgramResourceiv;
    PFNGLCREATEPROGRAMPROC create_program = glad_glCreateProgram;
    PFNGLDELETEPROGRAMPROC delete_program = glad_glDeleteProgram;
    PFNGLGENTEXTURESPROC gen_textures = glad_glGenTextures;
    PFNGLDELETETEXTURESPROC delete_textures = glad_glDeleteTextures;
    PFNGLTEXSTORAGE3DPROC tex_storage = glad_glTexStorage3D;
    PFNGLOBJECTLABELPROC object_label = glad_glObjectLabel;
    PFNGLDISPATCHCOMPUTEPROC dispatch = glad_glDispatchCompute;
};

struct ScopedFroxelPass {
    Luminumbra::Rendering::FroxelPass pass;
    ~ScopedFroxelPass() {
        pass.destroy();
    }
};

} // namespace

TEST(PassContext, FroxelInitializationFailureCleansUpAndRequiresExplicitRetry) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    // Either shader, either texture name/storage, reflection, missing files, and unwind.
    for (int failure = 0; failure < 9; ++failure) {
        SCOPED_TRACE(failure);
        FroxelInitCalls calls;
        ScopedFroxelPass scoped;
        auto& pass = scoped.pass;
        Luminumbra::Rendering::Camera camera;
        RenderContext ctx;
        ctx.camera = &camera;
        ctx.screen_width = 64;
        ctx.screen_height = 64;
        ctx.volumetric_quality = 1;
        calls.failed_shader = failure < 2 ? failure + 1 : 0;
        calls.failed_texture = failure >= 2 && failure < 4 ? failure - 1 : 0;
        calls.failed_storage = failure >= 4 && failure < 6 ? failure - 3 : 0;
        calls.invalid_layout = failure == 6;
        calls.label_exception = failure == 8;
        const std::filesystem::path root = LUMINUMBRA_SOURCE_ROOT;
        if (calls.label_exception)
            EXPECT_THROW(pass.execute_inject(ctx, root), std::runtime_error);
        else
            EXPECT_FALSE(pass.execute_inject(ctx, failure == 7 ? root / "res" : root));
        EXPECT_EQ(pass.integrated_texture(), 0u);
        EXPECT_EQ(calls.live_programs, 0);
        EXPECT_EQ(calls.live_textures, 0);
        EXPECT_EQ(calls.dispatches, 0);
        const int checks = calls.shader_checks;
        const int textures = calls.texture_calls;
        calls.failed_shader = calls.failed_texture = calls.failed_storage = 0;
        calls.invalid_layout = false;
        calls.label_exception = false;
        // Fixing the input alone does not clear failure, create, or dispatch.
        EXPECT_FALSE(pass.execute_inject(ctx, root));
        pass.execute_integrate(ctx);
        EXPECT_EQ(calls.shader_checks, checks);
        EXPECT_EQ(calls.texture_calls, textures);
        EXPECT_EQ(calls.dispatches, 0);

        pass.destroy();
        pass.destroy();
        ASSERT_TRUE(pass.execute_inject(ctx, root));
        EXPECT_EQ(calls.shader_checks, checks + 2);
        EXPECT_EQ(calls.texture_calls, textures + 2);
        EXPECT_EQ(calls.live_programs, 2);
        EXPECT_EQ(calls.live_textures, 2);
        ASSERT_NE(pass.integrated_texture(), 0u);
        GLint immutable = GL_FALSE;
        glGetTextureParameteriv(pass.integrated_texture(), GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
        EXPECT_EQ(immutable, GL_TRUE);
        pass.execute_integrate(ctx);
        EXPECT_EQ(calls.dispatches, 2);
        ASSERT_TRUE(pass.execute_inject(ctx, root));
        EXPECT_EQ(calls.shader_checks, checks + 2);
        EXPECT_EQ(calls.texture_calls, textures + 2);
        pass.destroy();
        EXPECT_EQ(calls.live_programs, 0);
        EXPECT_EQ(calls.live_textures, 0);
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    }
}

TEST(PassContext, FroxelOffAndUninitializedFrozenReplayDoNotAllocateOrLatchFailure) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    FroxelInitCalls calls;
    ScopedFroxelPass scoped;
    RenderContext ctx;
    ctx.volumetric_quality = 0;
    EXPECT_TRUE(scoped.pass.execute_inject(ctx, LUMINUMBRA_SOURCE_ROOT));
    scoped.pass.execute_integrate(ctx);
    ctx.volumetric_quality = 1;
    ctx.frozen_replay = true;
    EXPECT_FALSE(scoped.pass.execute_inject(ctx, LUMINUMBRA_SOURCE_ROOT));
    scoped.pass.execute_integrate(ctx);
    EXPECT_EQ(calls.shader_checks, 0);
    EXPECT_EQ(calls.texture_calls, 0);
    EXPECT_EQ(calls.dispatches, 0);
    Luminumbra::Rendering::Camera camera;
    ctx.camera = &camera;
    ctx.screen_width = 64;
    ctx.screen_height = 64;
    ctx.frozen_replay = false;
    ASSERT_TRUE(scoped.pass.execute_inject(ctx, LUMINUMBRA_SOURCE_ROOT));
    EXPECT_EQ(calls.live_programs, 2);
    EXPECT_EQ(calls.live_textures, 2);
    EXPECT_EQ(calls.dispatches, 1);
    ctx.frozen_replay = true;
    ASSERT_TRUE(scoped.pass.execute_inject(ctx, LUMINUMBRA_SOURCE_ROOT));
    EXPECT_EQ(calls.shader_checks, 2);
    EXPECT_EQ(calls.texture_calls, 2);
    scoped.pass.destroy();
    EXPECT_EQ(calls.live_programs, 0);
    EXPECT_EQ(calls.live_textures, 0);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}
