#include "LightingPass.h"

#include "GBufferPass.h"
#include "PassGlHelpers.h"
#include "ShadowPass.h"
#include "SsaoPass.h"
#include "WaterPass.h"
#include "core/Log.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"
#include "luminumbra_common/world/Chunk.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <string>

namespace Luminumbra::Rendering {

LightingPass::LightingPass() = default;
LightingPass::~LightingPass() = default;

void LightingPass::init_shader(const std::filesystem::path& root_path) {
    m_lighting_shader = std::make_unique<Shader>((root_path / "res/shaders/lighting_pass.vert").string().c_str(), (root_path / "res/shaders/lighting_pass.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_lighting_shader ? m_lighting_shader->Id() : 0u, "shader.lighting");
}

void LightingPass::init_lighting_fbo(u32 width, u32 height) {
    glGenFramebuffers(1, &m_lighting_fbo.fbo_id);
    PassGl::label_gl_object(GL_FRAMEBUFFER, m_lighting_fbo.fbo_id, "lighting.fbo");
    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_fbo.fbo_id);

    // Color attachment (for the final lit scene)
    glGenTextures(1, &m_lighting_fbo.color_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_lighting_fbo.color_texture, "lighting.color");
    glBindTexture(GL_TEXTURE_2D, m_lighting_fbo.color_texture);
    // Use RGBA16F for HDR lighting to avoid clamping colors between 0 and 1
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_lighting_fbo.color_texture, 0);

    glGenTextures(1, &m_lighting_fbo.opaque_color_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_lighting_fbo.opaque_color_texture, "lighting.opaque_color_copy");
    glBindTexture(GL_TEXTURE_2D, m_lighting_fbo.opaque_color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // We will blit the depth from the G-Buffer later, so we only need a renderbuffer object for depth testing.
    // However, if you wanted to do post-processing on this FBO that needs depth, you would use a depth texture.
    glGenRenderbuffers(1, &m_lighting_fbo.depth_texture); // Note: this is a renderbuffer ID, not a texture ID
    PassGl::label_gl_object(GL_RENDERBUFFER, m_lighting_fbo.depth_texture, "lighting.depth");
    glBindRenderbuffer(GL_RENDERBUFFER, m_lighting_fbo.depth_texture);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_lighting_fbo.depth_texture);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        LUMINUMBRA_CORE_ERROR("Lighting FBO not complete!");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void LightingPass::destroy_lighting_fbo() {
    if (m_lighting_fbo.fbo_id) { glDeleteFramebuffers(1, &m_lighting_fbo.fbo_id); m_lighting_fbo.fbo_id = 0; }
    if (m_lighting_fbo.color_texture) { glDeleteTextures(1, &m_lighting_fbo.color_texture); m_lighting_fbo.color_texture = 0; }
    if (m_lighting_fbo.opaque_color_texture) { glDeleteTextures(1, &m_lighting_fbo.opaque_color_texture); m_lighting_fbo.opaque_color_texture = 0; }
    if (m_lighting_fbo.depth_texture) { glDeleteRenderbuffers(1, &m_lighting_fbo.depth_texture); m_lighting_fbo.depth_texture = 0; }
}

void LightingPass::reset_shader() {
    m_lighting_shader.reset();
}

void LightingPass::copy_lighting_color_to_opaque_texture(RenderPipeline& pipeline) {
    if (!m_lighting_fbo.fbo_id || !m_lighting_fbo.color_texture || !m_lighting_fbo.opaque_color_texture) {
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D, m_lighting_fbo.opaque_color_texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, pipeline.m_screen_width, pipeline.m_screen_height);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void LightingPass::execute(RenderPipeline& pipeline, const Camera& camera) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    glViewport(0, 0, pipeline.m_screen_width, pipeline.m_screen_height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    m_lighting_shader->use();
    const GBuffer& gbuffer = pipeline.m_gbuffer_pass->gbuffer();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gbuffer.position_texture);    // View-space position
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gbuffer.normal_texture);      // Octahedral normal + material
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, gbuffer.albedo_texture);      // Albedo + roughness
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, gbuffer.material_texture);    // Metallic + AO
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, gbuffer.depth_texture);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_shadow_pass->shadow_map().depth_texture_array);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, pipeline.m_ssao_pass->ssao().ssaoColorBufferBlur);
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D_ARRAY, pipeline.m_terrainTextureArray);
    glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D, pipeline.m_materialLUT);
    glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_2D, pipeline.m_water_pass->black_texture());
    m_lighting_shader->setMat4("u_inverseView", glm::inverse(camera.GetViewMatrix()));
    m_lighting_shader->setInt("gPosition", 0);
    m_lighting_shader->setInt("gNormalMaterial", 1);    // Octahedral normal + material
    m_lighting_shader->setInt("gAlbedoRoughness", 2);   // Albedo + roughness
    m_lighting_shader->setInt("gMetallicAO", 3);        // Metallic + AO
    m_lighting_shader->setInt("gDepth", 4);
    m_lighting_shader->setInt("u_shadowCascades", 5);
    m_lighting_shader->setInt("u_ssao", 6);
    m_lighting_shader->setInt("u_terrainTextures", 7);
    m_lighting_shader->setInt("u_materialLUT", 8);
    m_lighting_shader->setFloat("u_emissiveLutScale", RenderPipeline::kEmissiveLutScale);
    m_lighting_shader->setInt("u_causticsTexture", 9);
    m_lighting_shader->setVec3("u_skyAmbientColor", pipeline.m_skyAmbientColor);
    m_lighting_shader->setVec3("u_viewPos", camera.Position);
    m_lighting_shader->setVec3("u_sun.direction", pipeline.m_sun.direction);
    m_lighting_shader->setVec3("u_sun.color", pipeline.m_sun.color);
    m_lighting_shader->setFloat("u_sea_level", SEA_LEVEL);
    m_lighting_shader->setInt("u_pointLightCount", static_cast<int>(pipeline.m_point_lights_this_frame.size()));
    for(size_t i = 0; i < pipeline.m_point_lights_this_frame.size(); ++i) {
        std::string prefix = "u_pointLights[" + std::to_string(i) + "].";
        m_lighting_shader->setVec3(prefix + "position", pipeline.m_point_lights_this_frame[i].position);
        m_lighting_shader->setVec3(prefix + "color", pipeline.m_point_lights_this_frame[i].color);
        m_lighting_shader->setFloat(prefix + "radius", pipeline.m_point_lights_this_frame[i].radius);
        m_lighting_shader->setFloat(prefix + "intensity", pipeline.m_point_lights_this_frame[i].intensity);
    }
    m_lighting_shader->setFloat("u_farPlane", camera.GetFarPlane());
    ShadowMap& shadow_map = pipeline.m_shadow_pass->shadow_map();
    if (!PassGl::has_valid_shadow_cascade_splits(shadow_map)) {
        LUMINUMBRA_CORE_ERROR("Shadow cascade splits were invalid during lighting; restoring defaults.");
        PassGl::set_default_shadow_cascade_splits(shadow_map);
    }
    if (shadow_map.light_space_matrices.size() < ShadowMap::CASCADE_COUNT) {
        shadow_map.light_space_matrices = pipeline.get_light_space_matrices(camera);
    }
    m_lighting_shader->setVec4("u_cascadeSplits", glm::vec4(shadow_map.cascade_splits[1], shadow_map.cascade_splits[2], shadow_map.cascade_splits[3], shadow_map.cascade_splits[4]));
    for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i) {
        m_lighting_shader->setMat4("u_lightSpaceMatrices[" + std::to_string(i) + "]", shadow_map.light_space_matrices[i]);
    }
    glm::vec3 terrainOrigin(floor(camera.Position.x / CHUNK_SIZE_X) * CHUNK_SIZE_X, 0.0f, floor(camera.Position.z / CHUNK_SIZE_Z) * CHUNK_SIZE_Z);
    m_lighting_shader->setVec3("u_terrainOrigin", terrainOrigin);
    glBindVertexArray(pipeline.m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    pipeline.m_last_render_pass_stats.lighting_draws++;
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace Luminumbra::Rendering
