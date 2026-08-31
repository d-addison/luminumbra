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
#include "luminumbra_client/rendering/RenderContext.h"
#include "luminumbra_client/rendering/ScentFieldRenderMirror.h"
#include "luminumbra_client/rendering/passes/DebugViewPass.h"
#include "luminumbra_client/rendering/passes/GroundDecalPass.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <gtest/gtest.h>

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

} // namespace
