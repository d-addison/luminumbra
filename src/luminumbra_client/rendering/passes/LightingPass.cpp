#include "LightingPass.h"

#include "../PassShaderLayouts.h"
#include "../RenderContext.h"
#include "../RenderResourceRegistry.h"
#include "../ShadowMap.h"
#include "PassGlHelpers.h"
#include "core/Log.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/world/Chunk.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include <string>

namespace Luminumbra::Rendering {

LightingPass::LightingPass() = default;
LightingPass::~LightingPass() = default;

void LightingPass::init_shader(const std::filesystem::path& root_path) {
    m_root_path = root_path; // retained for the lazy lightning overlay ()
    m_lighting_shader =
        std::make_unique<Shader>((root_path / "res/shaders/lighting_pass.vert").string().c_str(),
                                 (root_path / "res/shaders/lighting_pass.frag").string().c_str());
    PassGl::label_gl_object(
        GL_PROGRAM, m_lighting_shader ? m_lighting_shader->Id() : 0u, "shader.lighting");

    // validate the sampler bindings THIS pass
    // adopts in execute (the glActiveTexture+glBindTexture set above) against the
    // shader's reflected layout. A TYPE mismatch (e.g. binding a sampler2DArray
    // where the shader declares sampler2D) would render garbage -- this logs it
    // loudly at load instead. The ExpectedLayout is declared once in the enumerable
    // PassShaderLayouts registry (the single source the  fixture test +
    // the  coverage gate also read); registering it here also arms the
    // hot-reload rollback for this shader.
    if (m_lighting_shader && m_lighting_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("lighting")) {
            m_lighting_shader->ValidateLayout(*layout);
        }
    }
}

void LightingPass::init_lighting_fbo(RenderResourceRegistry& registry, u32 width, u32 height) {
    // allocate the lighting FBO + attachments THROUGH the
    // registry. The descs reproduce the retired glTexImage2D/glRenderbufferStorage
    // calls exactly (RGBA16F LINEAR HDR color; RGBA16F LINEAR + CLAMP_TO_EDGE
    // opaque copy; DEPTH_COMPONENT24 renderbuffer) so the objects are
    // parameter-identical; the FrameBufferObject struct caches the owned ids.
    TextureDesc color;
    color.width = width;
    color.height = height;
    color.internal_format = GL_RGBA16F; // HDR lighting: avoid clamping colors to [0,1]
    color.format = GL_RGBA;
    color.type = GL_FLOAT;
    color.min_filter = GL_LINEAR;
    color.mag_filter = GL_LINEAR;
    color.expected_layout = "color_attachment";
    color.debug_label = "lighting.color";
    m_lighting_fbo.color_texture = registry.create_texture("lighting_color", color).id;

    // Opaque-color copy: same format, but CLAMP_TO_EDGE. Standalone - the
    // copy_lighting_color_to_opaque_texture blit target, not an FBO attachment.
    TextureDesc opaque = color;
    opaque.wrap_s = GL_CLAMP_TO_EDGE;
    opaque.wrap_t = GL_CLAMP_TO_EDGE;
    opaque.expected_layout = "sampled";
    opaque.debug_label = "lighting.opaque_color_copy";
    m_lighting_fbo.opaque_color_texture =
        registry.create_texture("lighting_opaque_color", opaque).id;

    // Depth: a renderbuffer (non-samplable; the depth is blitted from the G-buffer).
    RenderbufferDesc depth;
    depth.width = width;
    depth.height = height;
    depth.internal_format = GL_DEPTH_COMPONENT24;
    depth.debug_label = "lighting.depth";
    m_lighting_fbo.depth_texture = registry.create_renderbuffer("lighting_depth", depth).id;

    FboDesc fbo_desc;
    fbo_desc.attachments = {
        {GL_COLOR_ATTACHMENT0, "lighting_color"},
        {GL_DEPTH_ATTACHMENT, "lighting_depth"},
    };
    fbo_desc.debug_label = "lighting.fbo";
    m_lighting_fbo.fbo_id = registry.create_fbo("lighting_fbo", fbo_desc).id;
    if (m_lighting_fbo.fbo_id == 0) {
        LUMINUMBRA_CORE_ERROR("Lighting FBO not complete!");
    }
}

void LightingPass::destroy_lighting_fbo(RenderResourceRegistry& registry) {
    // Ownership contract: the registry deletes the owned lighting GL objects.
    registry.destroy_owned("lighting_fbo");
    registry.destroy_owned("lighting_color");
    registry.destroy_owned("lighting_opaque_color");
    registry.destroy_owned("lighting_depth");
    m_lighting_fbo.fbo_id = 0;
    m_lighting_fbo.color_texture = 0;
    m_lighting_fbo.opaque_color_texture = 0;
    m_lighting_fbo.depth_texture = 0;
    // The lightning scene-copy scratch is pass-owned (lazy, framebuffer-sized).
    if (m_lightning_scene_copy) {
        glDeleteTextures(1, &m_lightning_scene_copy);
        m_lightning_scene_copy = 0;
        m_lightning_copy_w = 0;
        m_lightning_copy_h = 0;
    }
}

void LightingPass::reset_shader() {
    m_lighting_shader.reset();
}

void LightingPass::copy_lighting_color_to_opaque_texture(const RenderContext& ctx) {
    if (!m_lighting_fbo.fbo_id || !m_lighting_fbo.color_texture ||
        !m_lighting_fbo.opaque_color_texture) {
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D, m_lighting_fbo.opaque_color_texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        0,
                        0,
                        ctx.internal_w(),
                        ctx.internal_h()); // internal-sized lit color
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void LightingPass::execute(const RenderContext& ctx) {
    const Camera& camera = *ctx.camera;
    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // deferred lighting into the internal FBO
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    m_lighting_shader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_position.id); // View-space position
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_normal.id); // Octahedral normal + material
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_albedo.id); // Albedo + roughness
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_material.id); // Metallic + AO
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_depth.id);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.shadow_depth_array.id);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, ctx.ssao_blur.id);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.terrain_textures.id);
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, ctx.material_lut.id);
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D, ctx.caustics_tex.id);
    // Aether emissive field at unit 10 (gated by u_aetherActive). When
    // no field is uploaded the texture is 0 and u_aetherActive=0, so the glow term
    // is skipped -> pixel-identical to the pre- path.
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, ctx.aether_field.id);
    m_lighting_shader->setInt("u_aetherField", 10);
    // the tinted-transmission cascade at unit 11. The
    // array is init-cleared WHITE, so with no glass the multiply is exactly 1.0
    // (pixel-identical); u_shadowTintEnabled==0 skips the sampling entirely (the
    // same-build A/B lever: off vs on-with-white must FLIP to exactly 0.0).
    glActiveTexture(GL_TEXTURE11);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.shadow_tint_array.id);
    m_lighting_shader->setInt("u_shadowTintCascades", 11);
    m_lighting_shader->setInt("u_shadowTintEnabled", ctx.shadow_tint_array.id != 0 ? 1 : 0);
    if (ctx.aether_active && ctx.aether_extent > 0) {
        const float world_span = static_cast<float>(ctx.aether_extent) * ctx.aether_cell_size;
        m_lighting_shader->setFloat("u_aetherActive", 1.0f);
        m_lighting_shader->setVec2("u_aetherFieldWorldOrigin", ctx.aether_world_origin);
        m_lighting_shader->setFloat("u_aetherFieldInvWorldSpan",
                                    world_span > 0.0f ? (1.0f / world_span) : 0.0f);
    } else {
        m_lighting_shader->setFloat("u_aetherActive", 0.0f);
    }
    // the glow grade from the context — the defaults
    // equal the GLSL initializers, so untouched contexts stay pixel-identical.
    m_lighting_shader->setVec3("u_aetherGlowColor", ctx.aether_glow_color);
    m_lighting_shader->setFloat("u_aetherGlowIntensity", ctx.aether_glow_intensity);
    //  ( -6): emissive modulation — 0.0 default is a
    // multiply by exactly 1.0, so untouched contexts stay pixel-identical.
    m_lighting_shader->setFloat("u_aetherMaterialModulation", ctx.aether_material_modulation);
    //  (-8): polarity tint gate — 0.0 keeps the glow color
    // untouched (the single-channel/no-upload default).
    m_lighting_shader->setFloat("u_aetherPolarityActive", ctx.aether_polarity_active ? 1.0f : 0.0f);
    // the render-only snow cover (0 default = byte-identical).
    m_lighting_shader->setFloat("u_snowCover", ctx.snow_cover);
    glActiveTexture(GL_TEXTURE0);
    m_lighting_shader->setMat4("u_inverseView", glm::inverse(camera.GetViewMatrix()));
    m_lighting_shader->setInt("gPosition", 0);
    m_lighting_shader->setInt("gNormalMaterial", 1);  // Octahedral normal + material
    m_lighting_shader->setInt("gAlbedoRoughness", 2); // Albedo + roughness
    m_lighting_shader->setInt("gMetallicAO", 3);      // Metallic + AO
    m_lighting_shader->setInt("gDepth", 4);
    m_lighting_shader->setInt("u_shadowCascades", 5);
    m_lighting_shader->setInt("u_ssao", 6);
    m_lighting_shader->setInt("u_terrainTextures", 7);
    m_lighting_shader->setInt("u_materialLUT", 8);
    m_lighting_shader->setFloat("u_emissiveLutScale", ctx.emissive_lut_scale);
    m_lighting_shader->setInt("u_causticsTexture", 9);
    m_lighting_shader->setVec3("u_skyAmbientColor", ctx.sky_ambient_color);
    m_lighting_shader->setVec3("u_viewPos", camera.Position);
    m_lighting_shader->setVec3("u_sun.direction", ctx.sun.direction);
    m_lighting_shader->setVec3("u_sun.color", ctx.sun.color);
    // moon-shadows: the moon's TOWARD-LIGHT direction (anti-sun, overhead at
    // midnight; same convention the shader uses for u_sun.direction). The shader
    // lights + keys the cast-shadow lookup off this so moonlit terrain has real
    // directional form and shadows from the now-moon shadow cascade.
    m_lighting_shader->setVec3("u_moonDir", ctx.moon_light_dir);
    m_lighting_shader->setFloat("u_moonIllum", ctx.moon_illumination); // rendering: lunar phase
    m_lighting_shader->setVec3("u_moonRadiance",
                               ctx.moon_radiance); // rendering : dedicated moon radiance channel
    // LUMIN_MOON_WRAP_FLOOR is a supported photo-grade override for the moon
    // wrap floor. It is parsed once; 0.25 is the shipped navigable-night default.
    static const float s_moon_wrap_floor = [] {
        if (const auto value = Core::ReadEnvironment("LUMIN_MOON_WRAP_FLOOR")) {
            try {
                return std::stof(*value);
            } catch (...) {}
        }
        return 0.25f;
    }();
    m_lighting_shader->setFloat("u_moonWrapFloor", s_moon_wrap_floor);

    m_lighting_shader->setFloat("u_sea_level", SEA_LEVEL);
    //  cinematic grade (-style): BOLD default — lifted exposure, rich
    // saturation, strong contrast, and a cool-shadow / warm-highlight split-tone
    // (the key/fill cue). Tunable via LUMIN_GRADE="exposure,saturation,contrast,
    // warmR,warmG,warmB" (parsed once); the split-tone is fixed cinematic.
    struct Grade {
        float exposure, saturation, contrast, wr, wg, wb;
    };
    static const Grade s_grade = [] {
        Grade g{
            1.12f, 1.30f, 1.42f, 1.06f, 1.0f, 0.92f}; // richer sat + punchier contrast (de-wash
                                                      // noon; owner "white filter" pass 2026-07-07)
        if (const auto env = Core::ReadEnvironment("LUMIN_GRADE")) {
            std::sscanf(env->c_str(),
                        "%f,%f,%f,%f,%f,%f",
                        &g.exposure,
                        &g.saturation,
                        &g.contrast,
                        &g.wr,
                        &g.wg,
                        &g.wb);
        }
        return g;
    }();
    //  rendering: the RenderContext exposure seam slot wins when set; the
    // sentinel 0 falls back to the static LUMIN_GRADE exposure (byte-identical until ).
    m_lighting_shader->setFloat("u_exposure",
                                ctx.exposure > 0.0f ? ctx.exposure : s_grade.exposure);
    m_lighting_shader->setFloat("u_saturation", s_grade.saturation);
    m_lighting_shader->setFloat("u_contrast", s_grade.contrast);
    m_lighting_shader->setVec3("u_lightWarmth", glm::vec3(s_grade.wr, s_grade.wg, s_grade.wb));
    m_lighting_shader->setVec3("u_shadowTint", glm::vec3(0.88f, 0.96f, 1.14f));    // cool
    m_lighting_shader->setVec3("u_highlightTint", glm::vec3(1.14f, 1.04f, 0.84f)); // warm
    m_lighting_shader->setFloat("u_splitToneStrength", 0.55f);
    m_lighting_shader->setInt("u_pointLightCount", static_cast<int>(ctx.point_lights->size()));
    for (size_t i = 0; i < ctx.point_lights->size(); ++i) {
        std::string prefix = "u_pointLights[" + std::to_string(i) + "].";
        m_lighting_shader->setVec3(prefix + "position", (*ctx.point_lights)[i].position);
        m_lighting_shader->setVec3(prefix + "color", (*ctx.point_lights)[i].color);
        m_lighting_shader->setFloat(prefix + "radius", (*ctx.point_lights)[i].radius);
        m_lighting_shader->setFloat(prefix + "intensity", (*ctx.point_lights)[i].intensity);
    }
    // Cave / sky-visibility ambient occlusion (render-only). Default OFF =>
    // u_caveAmbientOcclusion 0.0 => the shader's skyVis term is exactly 1.0 =>
    // pixel-identical to the pre-fix path. Enabled + tuned via the LUMIN_CAVE_AO
    // env knob ("enabled,maxDist,floor,steps,thickness"). The probe needs the
    // same projection the SSAO pass builds, plus the screen size.
    {
        const glm::mat4 cave_proj = glm::perspective(glm::radians(camera.Zoom),
                                                     static_cast<float>(ctx.screen_width) /
                                                         static_cast<float>(ctx.screen_height),
                                                     camera.GetNearPlane(),
                                                     camera.GetFarPlane());
        m_lighting_shader->setMat4("u_projection", cave_proj);
        m_lighting_shader->setVec2(
            "u_screenSize",
            glm::vec2(ctx.internal_w(), ctx.internal_h())); // cave AO marches the internal G-buffer

        struct CaveAO {
            float enabled, maxDist, floor, thickness;
            int steps;
        };
        static const CaveAO s_caveAO = [] {
            CaveAO c{0.0f, 24.0f, 0.06f, 1.5f, 8}; // DEFAULT OFF (enabled=0)
            if (const auto env = Core::ReadEnvironment("LUMIN_CAVE_AO")) {
                // "enabled,maxDist,floor,steps,thickness"
                float en = 0, md = 24, fl = 0.06f, th = 1.5f;
                int st = 8;
                std::sscanf(env->c_str(), "%f,%f,%f,%d,%f", &en, &md, &fl, &st, &th);
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
    // -T12: the shadow-cascade validity/refresh fixup (which MUTATES the
    // shared ShadowMap private state + calls the pipeline-private
    // get_light_space_matrices) is hoisted to make_lighting_context; the resolved
    // splits + matrices arrive via ctx.cascade_splits + ctx.light_space_matrices.
    m_lighting_shader->setVec4("u_cascadeSplits", ctx.cascade_splits);
    for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i) {
        m_lighting_shader->setMat4("u_lightSpaceMatrices[" + std::to_string(i) + "]",
                                   (*ctx.light_space_matrices)[i]);
    }
    glm::vec3 terrainOrigin(floor(camera.Position.x / CHUNK_SIZE_X) * CHUNK_SIZE_X,
                            0.0f,
                            floor(camera.Position.z / CHUNK_SIZE_Z) * CHUNK_SIZE_Z);
    m_lighting_shader->setVec3("u_terrainOrigin", terrainOrigin);
    // project the wind-advected cloud coverage onto the terrain as a
    // crawling cast shadow (directSun *= 1 - cloudShadow). The uniforms must match
    // the sky-dome's cloudCoverageAt field exactly so a dome cloud and its ground
    // shadow stay registered. u_cloudShadowEnabled==0 is the zero-cost OFF path
    // (the gate captures clouds-on vs clouds-off lighting ms).  .
    {
        const CloudRenderState& cloud = ctx.cloud_state;
        const bool shadow_on =
            cloud.enabled && cloud.shadow_enabled && cloud.shadow_strength > 0.0f;
        m_lighting_shader->setInt("u_cloudShadowEnabled", shadow_on ? 1 : 0);
        m_lighting_shader->setVec2("u_cloudScrollOffset", cloud.scroll_offset);
        m_lighting_shader->setFloat("u_cloudCoverageAmount",
                                    cloud.enabled ? cloud.coverage_amount : 0.0f);
        m_lighting_shader->setFloat("u_cloudBiomeVariation", cloud.biome_variation);
        m_lighting_shader->setFloat("u_cloudPlaneHeight", cloud.plane_height);
        m_lighting_shader->setFloat("u_cloudShadowStrength", cloud.shadow_strength);
        m_lighting_shader->setVec3("u_cloudSunDir", cloud.sun_travel_dir);
    }
    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (ctx.lighting_draws)
        ++(*ctx.lighting_draws);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void LightingPass::execute_lightning_overlay(const RenderContext& ctx) {
    const LightningRenderState& lit = *ctx.lightning_state;
    if (!lit.active || lit.pulse_intensity <= 0.0f) {
        return; // zero-cost OFF path (no strike this frame)
    }
    const int w = static_cast<int>(ctx.internal_w()); // overlay scratch at internal res
    const int h = static_cast<int>(ctx.internal_h());
    if (w <= 0 || h <= 0 || !m_lighting_fbo.fbo_id || !m_lighting_fbo.color_texture) {
        return;
    }

    // Lazily build the overlay program on first strike.
    if (!m_lightning_overlay_shader) {
        m_lightning_overlay_shader = std::make_unique<Shader>(
            (m_root_path / "res/shaders/lightning_overlay.vert").string().c_str(),
            (m_root_path / "res/shaders/lightning_overlay.frag").string().c_str());
        PassGl::label_gl_object(GL_PROGRAM,
                                m_lightning_overlay_shader ? m_lightning_overlay_shader->Id() : 0u,
                                "shader.lightning_overlay");
        // validate the overlay's one sampler (u_scene) against the registry.
        if (m_lightning_overlay_shader && m_lightning_overlay_shader->IsValid()) {
            if (const ExpectedLayout* layout = FindPassExpectedLayout("lightning_overlay")) {
                m_lightning_overlay_shader->ValidateLayout(*layout);
            }
        }
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
        PassGl::label_gl_object(
            GL_TEXTURE, m_lightning_scene_copy, "lighting.lightning_scene_copy");
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
    //  dark storm-cloud mass the bolt emerges from.
    m_lightning_overlay_shader->setVec2("u_cloudNdc", lit.cloud_anchor_ndc);
    m_lightning_overlay_shader->setFloat("u_cloudDark", lit.cloud_darkness);
    m_lightning_overlay_shader->setFloat(
        "u_aspect", static_cast<float>(w) / std::max(1.0f, static_cast<float>(h)));
    const int point_count =
        std::min<int>(static_cast<int>(lit.bolt_points_ndc.size()), kMaxBoltSegmentPoints);
    m_lightning_overlay_shader->setInt("u_boltCount", point_count);
    for (int i = 0; i < point_count; ++i) {
        m_lightning_overlay_shader->setVec2("u_bolt[" + std::to_string(i) + "]",
                                            lit.bolt_points_ndc[static_cast<std::size_t>(i)]);
    }

    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (ctx.lighting_draws)
        ++(*ctx.lighting_draws);
}

} // namespace Luminumbra::Rendering
