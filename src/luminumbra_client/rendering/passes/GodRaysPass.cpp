#include "GodRaysPass.h"

#include "../PassShaderLayouts.h"
#include "PassGlHelpers.h"
#include "rendering/Shader.h"

namespace Luminumbra::Rendering {

GodRaysPass::GodRaysPass() = default;
GodRaysPass::~GodRaysPass() = default;

void GodRaysPass::init_shader(const std::filesystem::path& root_path) {
    // Screen-space crepuscular rays.
    m_shader = std::make_unique<Shader>((root_path / "res/shaders/ssao.vert").string().c_str(),
                                        (root_path / "res/shaders/god_rays.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_shader ? m_shader->Id() : 0u, "shader.god_rays");
    if (m_shader && m_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("god_rays"))
            m_shader->ValidateLayout(*layout);
    }
}

void GodRaysPass::reset_shader() {
    m_shader.reset();
}

void GodRaysPass::execute(const RenderContext& ctx) {
    // Screen-space crepuscular rays: additive shafts fanning from the sun around
    // occluders. Only when the sun is above the horizon and on screen (zero cost
    // otherwise). Only the shader stays a member; all frame state comes from ctx.
    if (!m_shader || !m_shader->IsValid() || ctx.screen_quad_vao == 0) {
        return;
    }
    const float sun_visible = ctx.sun_visible;
    const glm::vec2 sun_uv(ctx.sun_uv_x, ctx.sun_uv_y);
    const GLuint lit_fbo = ctx.lit_scene.id;
    const GLuint opaque_tex = ctx.opaque_scene.id;
    if (sun_visible > 0.002f && lit_fbo && opaque_tex) {
        glBindFramebuffer(GL_FRAMEBUFFER, lit_fbo);
        glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // god-rays into internal lit FBO
        const GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
        glDisable(GL_DEPTH_TEST);
        const GLboolean blend_was = glIsEnabled(GL_BLEND);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE); // additive
        m_shader->use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, opaque_tex);
        m_shader->setInt("u_scene", 0);
        m_shader->setVec2("u_sunUV", sun_uv);
        m_shader->setFloat("u_sunVisible", sun_visible);
        m_shader->setFloat("u_strength", 0.85f);
        glBindVertexArray(ctx.screen_quad_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!blend_was)
            glDisable(GL_BLEND);
        if (depth_was)
            glEnable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

} // namespace Luminumbra::Rendering
