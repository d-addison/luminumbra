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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Luminumbra::Rendering {

LightingPass::LightingPass() = default;
LightingPass::~LightingPass() = default;

void LightingPass::init_shader(const std::filesystem::path& root_path) {
    m_root_path = root_path; // retained for the lazy lightning overlay (T-I5a-5)
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
    if (m_lightning_scene_copy) { glDeleteTextures(1, &m_lightning_scene_copy); m_lightning_scene_copy = 0; m_lightning_copy_w = 0; m_lightning_copy_h = 0; }
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
    // T-I6-A1d: Aetheric emissive field at unit 10 (gated by u_aetherActive). When
    // no field is uploaded the texture is 0 and u_aetherActive=0, so the glow term
    // is skipped -> pixel-identical to the pre-A1d path.
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, pipeline.m_aetherFieldTexture);
    m_lighting_shader->setInt("u_aetherField", 10);
    if (pipeline.m_aetherFieldActive && pipeline.m_aetherFieldExtent > 0) {
        const float world_span = static_cast<float>(pipeline.m_aetherFieldExtent) *
                                 pipeline.m_aetherFieldCellSize;
        m_lighting_shader->setFloat("u_aetherActive", 1.0f);
        m_lighting_shader->setVec2("u_aetherFieldWorldOrigin", pipeline.m_aetherFieldWorldOrigin);
        m_lighting_shader->setFloat("u_aetherFieldInvWorldSpan",
                                    world_span > 0.0f ? (1.0f / world_span) : 0.0f);
    } else {
        m_lighting_shader->setFloat("u_aetherActive", 0.0f);
    }
    glActiveTexture(GL_TEXTURE0);
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
    // moon-shadows: the moon's TOWARD-LIGHT direction (anti-sun, overhead at
    // midnight; same convention the shader uses for u_sun.direction). The shader
    // lights + keys the cast-shadow lookup off this so moonlit terrain has real
    // directional form and shadows from the now-moon shadow cascade.
    m_lighting_shader->setVec3("u_moonDir", pipeline.m_moonLightDir);
    m_lighting_shader->setFloat("u_sea_level", SEA_LEVEL);
    // T-I7 cinematic grade (BF1-style): BOLD default — lifted exposure, rich
    // saturation, strong contrast, and a cool-shadow / warm-highlight split-tone
    // (the key/fill cue). Tunable via LUMIN_GRADE="exposure,saturation,contrast,
    // warmR,warmG,warmB" (parsed once); the split-tone is fixed cinematic.
    struct Grade {
        float exposure, saturation, contrast, wr, wg, wb;
    };
    static const Grade s_grade = [] {
        Grade g{1.12f, 1.13f, 1.32f, 1.06f, 1.0f, 0.92f}; // richer sat + punchier contrast (de-wash noon)
        if (const char* env = std::getenv("LUMIN_GRADE")) {
            std::sscanf(env, "%f,%f,%f,%f,%f,%f", &g.exposure, &g.saturation,
                        &g.contrast, &g.wr, &g.wg, &g.wb);
        }
        return g;
    }();
    m_lighting_shader->setFloat("u_exposure", s_grade.exposure);
    m_lighting_shader->setFloat("u_saturation", s_grade.saturation);
    m_lighting_shader->setFloat("u_contrast", s_grade.contrast);
    m_lighting_shader->setVec3("u_lightWarmth", glm::vec3(s_grade.wr, s_grade.wg, s_grade.wb));
    m_lighting_shader->setVec3("u_shadowTint", glm::vec3(0.88f, 0.96f, 1.14f));   // cool
    m_lighting_shader->setVec3("u_highlightTint", glm::vec3(1.14f, 1.04f, 0.84f)); // warm
    m_lighting_shader->setFloat("u_splitToneStrength", 0.55f);
    m_lighting_shader->setInt("u_pointLightCount", static_cast<int>(pipeline.m_point_lights_this_frame.size()));
    for(size_t i = 0; i < pipeline.m_point_lights_this_frame.size(); ++i) {
        std::string prefix = "u_pointLights[" + std::to_string(i) + "].";
        m_lighting_shader->setVec3(prefix + "position", pipeline.m_point_lights_this_frame[i].position);
        m_lighting_shader->setVec3(prefix + "color", pipeline.m_point_lights_this_frame[i].color);
        m_lighting_shader->setFloat(prefix + "radius", pipeline.m_point_lights_this_frame[i].radius);
        m_lighting_shader->setFloat(prefix + "intensity", pipeline.m_point_lights_this_frame[i].intensity);
    }
    // Cave / sky-visibility ambient occlusion (render-only). Default OFF =>
    // u_caveAmbientOcclusion 0.0 => the shader's skyVis term is exactly 1.0 =>
    // pixel-identical to the pre-fix path. Enabled + tuned via the LUMIN_CAVE_AO
    // env knob ("enabled,maxDist,floor,steps,thickness"). The probe needs the
    // same projection the SSAO pass builds, plus the screen size.
    {
        const glm::mat4 cave_proj = glm::perspective(
            glm::radians(camera.Zoom),
            static_cast<float>(pipeline.m_screen_width) / static_cast<float>(pipeline.m_screen_height),
            camera.GetNearPlane(), camera.GetFarPlane());
        m_lighting_shader->setMat4("u_projection", cave_proj);
        m_lighting_shader->setVec2("u_screenSize",
            glm::vec2(pipeline.m_screen_width, pipeline.m_screen_height));

        struct CaveAO { float enabled, maxDist, floor, thickness; int steps; };
        static const CaveAO s_caveAO = [] {
            CaveAO c{0.0f, 24.0f, 0.06f, 1.5f, 8}; // DEFAULT OFF (enabled=0)
            if (const char* env = std::getenv("LUMIN_CAVE_AO")) {
                // "enabled,maxDist,floor,steps,thickness"
                float en = 0, md = 24, fl = 0.06f, th = 1.5f; int st = 8;
                std::sscanf(env, "%f,%f,%f,%d,%f", &en, &md, &fl, &st, &th);
                c = CaveAO{en, md, fl, th, st};
            }
            return c;
        }();
        m_lighting_shader->setFloat("u_caveAmbientOcclusion", s_caveAO.enabled);
        m_lighting_shader->setFloat("u_caveSkyMaxDist", s_caveAO.maxDist);
        m_lighting_shader->setInt("u_caveSkySteps", s_caveAO.steps);
        m_lighting_shader->setFloat("u_caveAmbientFloor", s_caveAO.floor);
        m_lighting_shader->setFloat("u_caveThickness", s_caveAO.thickness);
        // Optional point-light punch (shader Patch 4, not applied); defaults keep
        // legacy behaviour. setFloat on an absent uniform is a harmless no-op.
        m_lighting_shader->setFloat("u_pointLightFalloff", 0.05f);
        m_lighting_shader->setFloat("u_pointLightInvSqMix", 0.0f);
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
    // T-I5a-8 (C3): project the wind-advected cloud coverage onto the terrain as a
    // crawling cast shadow (directSun *= 1 - cloudShadow). The uniforms must match
    // the sky-dome's cloudCoverageAt field exactly so a dome cloud and its ground
    // shadow stay registered. u_cloudShadowEnabled==0 is the zero-cost OFF path
    // (the gate captures clouds-on vs clouds-off lighting ms). RENDER-ONLY (F2).
    {
        const CloudRenderState& cloud = pipeline.get_cloud_state();
        const bool shadow_on = cloud.enabled && cloud.shadow_enabled && cloud.shadow_strength > 0.0f;
        m_lighting_shader->setInt("u_cloudShadowEnabled", shadow_on ? 1 : 0);
        m_lighting_shader->setVec2("u_cloudScrollOffset", cloud.scroll_offset);
        m_lighting_shader->setFloat("u_cloudCoverageAmount", cloud.enabled ? cloud.coverage_amount : 0.0f);
        m_lighting_shader->setFloat("u_cloudBiomeVariation", cloud.biome_variation);
        m_lighting_shader->setFloat("u_cloudPlaneHeight", cloud.plane_height);
        m_lighting_shader->setFloat("u_cloudShadowStrength", cloud.shadow_strength);
        m_lighting_shader->setVec3("u_cloudSunDir", cloud.sun_travel_dir);
    }
    glBindVertexArray(pipeline.m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    pipeline.m_last_render_pass_stats.lighting_draws++;
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void LightingPass::execute_lightning_overlay(RenderPipeline& pipeline, const Camera& camera) {
    (void)camera;
    const LightningRenderState& lit = pipeline.get_lightning_state();
    if (!lit.active || lit.pulse_intensity <= 0.0f) {
        return; // zero-cost OFF path (no strike this frame)
    }
    const int w = static_cast<int>(pipeline.m_screen_width);
    const int h = static_cast<int>(pipeline.m_screen_height);
    if (w <= 0 || h <= 0 || !m_lighting_fbo.fbo_id || !m_lighting_fbo.color_texture) {
        return;
    }

    // Lazily build the overlay program on first strike.
    if (!m_lightning_overlay_shader) {
        m_lightning_overlay_shader = std::make_unique<Shader>(
            (m_root_path / "res/shaders/lightning_overlay.vert").string().c_str(),
            (m_root_path / "res/shaders/lightning_overlay.frag").string().c_str());
        PassGl::label_gl_object(GL_PROGRAM,
            m_lightning_overlay_shader ? m_lightning_overlay_shader->Id() : 0u, "shader.lightning_overlay");
    }
    if (!m_lightning_overlay_shader || !m_lightning_overlay_shader->IsValid()) {
        return;
    }

    // (Re)allocate the scene-copy texture if the framebuffer size changed.
    if (m_lightning_scene_copy == 0 || m_lightning_copy_w != w || m_lightning_copy_h != h) {
        if (m_lightning_scene_copy != 0) {
            glDeleteTextures(1, &m_lightning_scene_copy);
            m_lightning_scene_copy = 0;
        }
        glGenTextures(1, &m_lightning_scene_copy);
        PassGl::label_gl_object(GL_TEXTURE, m_lightning_scene_copy, "lighting.lightning_scene_copy");
        glBindTexture(GL_TEXTURE_2D, m_lightning_scene_copy);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_lightning_copy_w = w;
        m_lightning_copy_h = h;
    }

    // Snapshot the composited (lit + sky + water + particles) FBO color into the
    // scratch texture; the overlay reads it and writes the additive result back.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D, m_lightning_scene_copy);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND); // shader reads the scene copy and writes pulse+bolt added

    m_lightning_overlay_shader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_lightning_scene_copy);
    m_lightning_overlay_shader->setInt("u_scene", 0);
    m_lightning_overlay_shader->setInt("u_active", 1);
    m_lightning_overlay_shader->setFloat("u_pulse", lit.pulse_intensity);
    m_lightning_overlay_shader->setVec3("u_color", lit.pulse_color);
    m_lightning_overlay_shader->setVec2("u_strikeNdc", lit.strike_ndc);
    m_lightning_overlay_shader->setFloat("u_boltWidth", lit.bolt_width_ndc);
    m_lightning_overlay_shader->setFloat("u_boltGlow", lit.bolt_glow_ndc);
    m_lightning_overlay_shader->setVec2("u_groundNdc", lit.ground_ndc);
    m_lightning_overlay_shader->setFloat("u_groundFlash", lit.ground_flash);
    // T-I5a-DR-storm-motion-v3: dark storm-cloud mass the bolt emerges from.
    m_lightning_overlay_shader->setVec2("u_cloudNdc", lit.cloud_anchor_ndc);
    m_lightning_overlay_shader->setFloat("u_cloudDark", lit.cloud_darkness);
    m_lightning_overlay_shader->setFloat("u_aspect",
        static_cast<float>(w) / std::max(1.0f, static_cast<float>(h)));
    const int point_count =
        std::min<int>(static_cast<int>(lit.bolt_points_ndc.size()), kMaxBoltSegmentPoints);
    m_lightning_overlay_shader->setInt("u_boltCount", point_count);
    for (int i = 0; i < point_count; ++i) {
        m_lightning_overlay_shader->setVec2(
            "u_bolt[" + std::to_string(i) + "]", lit.bolt_points_ndc[static_cast<std::size_t>(i)]);
    }

    glBindVertexArray(pipeline.m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    pipeline.m_last_render_pass_stats.lighting_draws++;
}

} // namespace Luminumbra::Rendering
