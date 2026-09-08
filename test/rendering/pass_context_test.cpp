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
#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/RenderContext.h"
#include "luminumbra_client/rendering/RenderPipeline.h"
#include "luminumbra_client/rendering/RenderResourceRegistry.h"
#include "luminumbra_client/rendering/ScentFieldRenderMirror.h"
#include "luminumbra_client/rendering/passes/DebugViewPass.h"
#include "luminumbra_client/rendering/passes/FoliagePass.h"
#include "luminumbra_client/rendering/passes/GBufferPass.h"
#include "luminumbra_client/rendering/passes/GlassOitPass.h"
#include "luminumbra_client/rendering/passes/GroundDecalPass.h"
#include "luminumbra_client/rendering/passes/LightingPass.h"
#include "luminumbra_client/rendering/passes/ParticlePass.h"
#include "luminumbra_client/rendering/passes/ShadowPass.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
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
    std::vector<unsigned char> px(static_cast<std::size_t>(rt.w) * rt.h * 4, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, rt.w, rt.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
}

TEST(PassContext, FloatDepthFramebuffersSurviveResizeAndRenderScaleChange) {
    HiddenGlContext gl;
    ASSERT_TRUE(gl.ready()) << gl.error();
    using namespace Luminumbra::Rendering;
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
            std::vector<float>(static_cast<std::size_t>(extent) * extent * 4, 0.8f));
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
            pass.execute_resolve(ctx, input);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            const auto pixels = ReadTarget(target);
            const std::size_t center =
                (static_cast<std::size_t>(extent / 2) * extent + extent / 2) * 4;
            if (occluded) {
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(pixels[center + channel], 204, 1);
            } else {
                EXPECT_LT(pixels[center], 100) << "visible glass must tint the lit scene";
                EXPECT_GT(pixels[center + 1], pixels[center] + 60);
            }
        }
        // A spatially varying background must stay registered with the pane when
        // the output resolution changes independently of the internal OIT extent.
        std::vector<float> background(static_cast<std::size_t>(extent) * extent * 4);
        for (int y = 0; y < extent; ++y) {
            for (int x = 0; x < extent; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * extent + x) * 4;
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
                    (static_cast<std::size_t>(extent / 4) * extent + extent / 4) * 4;
                const std::size_t far_corner =
                    (static_cast<std::size_t>(3 * extent / 4) * extent + 3 * extent / 4) * 4;
                EXPECT_GT(pixels[far_corner], pixels[near_corner] + 60);
                EXPECT_GT(pixels[far_corner + 1], pixels[near_corner + 1] + 60);
            } else {
                EXPECT_EQ(pixels, full_scale)
                    << "glass refraction must use the internal scene's pixel coordinates";
            }
        }
        panes[0].tint = glm::vec3(0.2f, 0.8f, 0.3f);
        pass.destroy(); // the pipeline destroys OIT before replacing its shared depth on resize
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
            const std::size_t o = (static_cast<std::size_t>(j) * W + i) * 4;
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
        pass.execute(ctx, input);
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
