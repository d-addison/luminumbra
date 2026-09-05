#include "GlassOitPass.h"

#include "PassGlHelpers.h"
#include "core/Log.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

namespace Luminumbra::Rendering {

GlassOitPass::GlassOitPass() = default;
GlassOitPass::~GlassOitPass() = default;

void GlassOitPass::execute_accum(const RenderContext& ctx, const GlassOitPassInput& input) {
    // WBOIT accumulation — each visible pane writes its
    // depth-weighted premultiplied color into accum (blend ONE/ONE) and its
    // coverage into reveal (ZERO/ONE_MINUS_SRC_ALPHA -> the (1-a) product),
    // depth-TESTED against the SHARED lighting depth (write off). Empty glass
    // list = zero GL work (the trace slot still records).
    if (!input.glass_items || input.glass_items->empty() || input.glass_vao == 0) {
        return;
    }
    if (!m_accum_shader) {
        // Lazy init: shaders + the accum/reveal MRT FBO sharing the lighting depth.
        m_accum_shader = std::make_unique<Shader>(
            (input.root_path / "res/shaders/glass_oit.vert").string().c_str(),
            (input.root_path / "res/shaders/glass_oit.frag").string().c_str());
        PassGl::label_gl_object(
            GL_PROGRAM, m_accum_shader ? m_accum_shader->Id() : 0u, "shader.glass_oit");
        m_resolve_shader = std::make_unique<Shader>(
            (input.root_path / "res/shaders/volumetric_lighting.vert").string().c_str(),
            (input.root_path / "res/shaders/glass_oit_resolve.frag").string().c_str());
        PassGl::label_gl_object(
            GL_PROGRAM, m_resolve_shader ? m_resolve_shader->Id() : 0u, "shader.glass_oit_resolve");
        if (m_accum_shader && m_accum_shader->IsValid()) {
            ExpectedLayout layout;
            layout.pass_name = "glass_oit";
            layout.samplers = {{"u_opaqueScene", GL_SAMPLER_2D, -1}};
            m_accum_shader->ValidateLayout(layout);
        }
        if (m_resolve_shader && m_resolve_shader->IsValid()) {
            ExpectedLayout layout;
            layout.pass_name = "glass_oit_resolve";
            layout.samplers = {{"u_accum", GL_SAMPLER_2D, -1}, {"u_reveal", GL_SAMPLER_2D, -1}};
            m_resolve_shader->ValidateLayout(layout);
        }

        auto make_target = [&](GLenum ifmt, const char* name) {
            GLuint t = 0;
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexStorage2D(GL_TEXTURE_2D,
                           1,
                           ifmt,
                           ctx.internal_w(),
                           ctx.internal_h()); // match internal lighting depth
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            PassGl::label_gl_object(GL_TEXTURE, t, name);
            return t;
        };
        m_accum_tex = make_target(GL_RGBA16F, "oit.accum");
        m_reveal_tex = make_target(GL_R16F, "oit.reveal");
        glGenFramebuffers(1, &m_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_accum_tex, 0);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_reveal_tex, 0);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, ctx.lit_scene_depth.id, 0);
        PassGl::label_gl_object(GL_FRAMEBUFFER, m_fbo, "oit.fbo");
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LUMINUMBRA_CORE_ERROR("glass_oit: MRT FBO incomplete; OIT disabled");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &m_fbo);
            m_fbo = 0;
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (m_fbo == 0 || !m_accum_shader->IsValid()) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // OIT composites into internal scene
    const GLfloat clear_accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clear_reveal[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glClearBufferfv(GL_COLOR, 0, clear_accum);
    glClearBufferfv(GL_COLOR, 1, clear_reveal);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunci(0, GL_ONE, GL_ONE);
    glBlendFunci(1, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);

    m_accum_shader->use();
    m_accum_shader->setMat4("u_view", ctx.view);
    m_accum_shader->setMat4("u_projection", ctx.projection);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.opaque_scene.id);
    m_accum_shader->setInt("u_opaqueScene", 0);
    m_accum_shader->setVec2(
        "u_screenSize",
        glm::vec2(static_cast<float>(ctx.screen_width), static_cast<float>(ctx.screen_height)));
    m_accum_shader->setVec3("u_cameraPos", ctx.camera->Position);
    m_accum_shader->setFloat("u_refractionStrength", 0.35f);
    glBindVertexArray(input.glass_vao);
    for (const GlassPaneItem& pane : *input.glass_items) {
        m_accum_shader->setMat4("u_model", pane.model);
        m_accum_shader->setVec3("u_tint", pane.tint);
        m_accum_shader->setFloat("u_thickness", pane.thickness);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(0);

    // Restore the default blend state (per-buffer funcs revert to the global).
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlassOitPass::execute_resolve(const RenderContext& ctx, const GlassOitPassInput& input) {
    // the WBOIT resolve — the weighted-average glass
    // color composited over the lit scene with coverage = 1 - reveal. Runs
    // BEFORE the weather snapshot so god-rays/weather observe resolved glass.
    if (!input.glass_items || input.glass_items->empty() || m_fbo == 0 || !m_resolve_shader ||
        !m_resolve_shader->IsValid() || ctx.screen_quad_vao == 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.lit_scene.id);
    glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // into internal lighting FBO
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_resolve_shader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_accum_tex);
    m_resolve_shader->setInt("u_accum", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_reveal_tex);
    m_resolve_shader->setInt("u_reveal", 1);
    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlassOitPass::destroy() {
    m_accum_shader.reset();
    m_resolve_shader.reset();
    if (m_fbo) {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    if (m_accum_tex) {
        glDeleteTextures(1, &m_accum_tex);
        m_accum_tex = 0;
    }
    if (m_reveal_tex) {
        glDeleteTextures(1, &m_reveal_tex);
        m_reveal_tex = 0;
    }
}

} // namespace Luminumbra::Rendering
