#include "TaauPass.h"

#include "../PassShaderLayouts.h"
#include "../RenderResourceRegistry.h"
#include "PassGlHelpers.h"
#include "rendering/Shader.h"

namespace Luminumbra::Rendering {

TaauPass::TaauPass() = default;
TaauPass::~TaauPass() = default;

void TaauPass::init_shader(const std::filesystem::path& root_path) {
    //  TAAU resolve. Reuses the SSAO fullscreen-quad vertex stage.
    m_shader =
        std::make_unique<Shader>((root_path / "res/shaders/ssao.vert").string().c_str(),
                                 (root_path / "res/shaders/taau_resolve.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_shader ? m_shader->Id() : 0u, "shader.taau_resolve");
    if (m_shader && m_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("taau_resolve"))
            m_shader->ValidateLayout(*layout);
    }
}

void TaauPass::init(RenderResourceRegistry& registry, u32 width, u32 height) {
    if (width == 0 || height == 0)
        return;
    glGenFramebuffers(1, &m_fbo);
    PassGl::label_gl_object(GL_FRAMEBUFFER, m_fbo, "taau.fbo");
    // the ping-pong resolved-color history textures are
    // registry-owned with History lifetime (they persist across frames by design;
    // resize recreates them without content preservation, matching the
    // "history invalidated on resize"). The FBO stays PASS-OWNED: it is a transient
    // container whose color attachment is the write-side history, bound per frame in
    // execute_taau_resolve - not a fixed-layout render target the registry can own.
    // The desc reproduces the retired glTexImage2D/glTexParameter calls exactly
    // (RGBA16F, LINEAR, CLAMP_TO_EDGE).
    for (int i = 0; i < 2; ++i) {
        TextureDesc history;
        history.width = width;
        history.height = height;
        history.internal_format = GL_RGBA16F;
        history.format = GL_RGBA;
        history.type = GL_FLOAT;
        history.min_filter = GL_LINEAR;
        history.mag_filter = GL_LINEAR;
        history.wrap_s = GL_CLAMP_TO_EDGE;
        history.wrap_t = GL_CLAMP_TO_EDGE;
        history.lifetime = ResourceLifetime::History;
        history.expected_layout = "color_attachment";
        history.debug_label = "taau.history";
        m_history[i] =
            registry.create_texture(i == 0 ? "taau_history_0" : "taau_history_1", history).id;
    }
    m_history_write = 0;
    m_history_valid = false; // no usable history until the first resolve fills it
}

void TaauPass::destroy(RenderResourceRegistry& registry) {
    if (m_fbo) {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    // The history textures are registry-owned.
    registry.destroy_owned("taau_history_0");
    registry.destroy_owned("taau_history_1");
    m_history[0] = 0;
    m_history[1] = 0;
    m_history_valid = false;
}

void TaauPass::reset_shader() {
    m_shader.reset();
}

void TaauPass::invalidate_history() {
    m_history_valid = false;
}

void TaauPass::execute(const RenderContext& ctx) {
    if (!m_shader || !m_shader->IsValid() || m_fbo == 0 || ctx.screen_quad_vao == 0 ||
        ctx.screen_width == 0 || ctx.screen_height == 0) {
        return;
    }
    const int wr = m_history_write;
    const int rd = 1 - wr;
    const GLuint lit_color = ctx.lit_scene_color.id;
    const GLuint lit_fbo = ctx.lit_scene.id;

    // Resolve current (lit HDR) + motion-reprojected history -> history[wr].
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_history[wr], 0);
    const GLenum draw0[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, draw0);
    glViewport(0, 0, ctx.screen_width, ctx.screen_height);

    const GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    const GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);

    m_shader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, lit_color);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_history[rd]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.motion_vectors.id);
    m_shader->setInt("u_current", 0);
    m_shader->setInt("u_history", 1);
    m_shader->setInt("u_motion", 2);
    m_shader->setVec2(
        "u_texel",
        glm::vec2(1.0f / (float)ctx.internal_w(),
                  1.0f / (float)ctx.internal_h())); // neighborhood steps by internal texels
                                                    // (u_current is internal-sized)
    m_shader->setFloat("u_blend", 0.9f);
    m_shader->setFloat("u_sharpness", 0.4f); // recover TAA temporal-blur softness
    m_shader->setInt("u_history_valid", m_history_valid ? 1 : 0);

    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    // Copy the resolved result back into the lighting color so the final blit shows it; keep
    // history[wr] for next frame's reprojection.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lit_fbo);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0,
                      0,
                      ctx.screen_width,
                      ctx.screen_height,
                      0,
                      0,
                      ctx.screen_width,
                      ctx.screen_height,
                      GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);

    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    m_history_write = rd; // ping-pong
    m_history_valid = true;
}

} // namespace Luminumbra::Rendering
