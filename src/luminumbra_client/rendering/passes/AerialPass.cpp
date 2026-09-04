#include "AerialPass.h"

#include "../PassShaderLayouts.h"
#include "PassGlHelpers.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Luminumbra::Rendering {

AerialPass::AerialPass() = default;
AerialPass::~AerialPass() = default;

void AerialPass::init_shader(const std::filesystem::path& root_path) {
    //  analytic aerial-perspective term wiring the dormant
    // volumetric_lighting.frag as a fullscreen pass over the lit scene. Reuses
    // the SSAO fullscreen-quad vertex stage.
    m_shader = std::make_unique<Shader>(
        (root_path / "res/shaders/ssao.vert").string().c_str(),
        (root_path / "res/shaders/volumetric_lighting.frag").string().c_str());
    PassGl::label_gl_object(
        GL_PROGRAM, m_shader ? m_shader->Id() : 0u, "shader.aerial_perspective");
    if (m_shader && m_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("aerial_perspective"))
            m_shader->ValidateLayout(*layout);
    }
}

void AerialPass::reset_shader() {
    m_shader.reset();
}

void AerialPass::set_froxel_input(int volumetric_quality, u32 integrated_texture) {
    m_volumetric_quality = volumetric_quality;
    m_froxel_integrated_texture = integrated_texture;
}

void AerialPass::execute(const RenderContext& ctx) {
    //  analytic aerial-perspective in-scatter composited OVER the lit
    // scene in the lighting FBO. Reads the SAME sky-view/transmittance LUTs the
    // dome uses (coherent palette). A no-op if the LUT/shader are unavailable.
    // Only the shader stays a member; all frame state comes from ctx.
    if (!m_shader || !m_shader->IsValid() || !ctx.sky_lut_ready || ctx.screen_quad_vao == 0) {
        return;
    }
    const GLuint lit_fbo = ctx.lit_scene.id;
    if (!lit_fbo) {
        return;
    }
    const Camera& camera = *ctx.camera;

    glBindFramebuffer(GL_FRAMEBUFFER, lit_fbo);
    glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // aerial into internal lit FBO
    const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    const GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_shader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_depth.id);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.sky_view_lut.id);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.transmittance_lut.id);
    m_shader->setInt("gDepth", 0);
    m_shader->setInt("u_skyViewLut", 1);
    m_shader->setInt("u_transmittanceLut", 2);
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom),
                                            (float)ctx.screen_width / (float)ctx.screen_height,
                                            camera.GetNearPlane(),
                                            camera.GetFarPlane());
    m_shader->setMat4("u_inverseView", glm::inverse(camera.GetViewMatrix()));
    m_shader->setMat4("u_inverseProjection", glm::inverse(projection));
    m_shader->setVec3("u_viewPos", camera.Position);
    // Toward-sun direction (sun-disc convention), matching the sky-view LUT frame.
    m_shader->setVec3("u_sunDirection", -ctx.sun.direction);
    const float sun_up = glm::dot(ctx.sun.direction, glm::vec3(0.0f, -1.0f, 0.0f));
    m_shader->setFloat("u_sunCosZenith", sun_up);
    m_shader->setFloat("u_skyDayFactor", ctx.sky_day_factor);
    m_shader->setFloat("u_underwater", ctx.underwater_factor);
    m_shader->setFloat("u_aerialDensity", ctx.aerial_density);
    m_shader->setFloat("u_aerialMaxDistance", ctx.aerial_max_distance);
    m_shader->setFloat("u_inscatterStrength", ctx.inscatter_strength);
    m_shader->setFloat("u_atmosphereWarmth", ctx.atmosphere_warmth);
    //  rendering: compose the integrated froxel volume
    // ( — extends the analytic term, never replaces it). Mode 0 (the
    // default) skips the sampling entirely: byte-identical to pre-froxel.
    const bool froxel_on = m_volumetric_quality > 0 && m_froxel_integrated_texture != 0;
    m_shader->setInt("u_volumetricMode", froxel_on ? 1 : 0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_3D, froxel_on ? m_froxel_integrated_texture : 0u);
    m_shader->setInt("u_froxelIntegrated", 3);

    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    if (!blend_was_enabled) {
        glDisable(GL_BLEND);
    }
    if (depth_was_enabled) {
        glEnable(GL_DEPTH_TEST);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace Luminumbra::Rendering
