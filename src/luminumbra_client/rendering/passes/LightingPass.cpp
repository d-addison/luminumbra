#include "LightingPass.h"
#include "../FarWorldReplay.h"
#include "../RenderingDefaults.h"
#include "DebugViewPass.h"
#include "core/Profiler.h"
#include "rendering/AttributedGl.h"

#include "../EnvironmentBrdfLut.gen.h"
#include "../PassShaderLayouts.h"
#include "../RenderContext.h"
#include "../RenderResourceRegistry.h"
#include "../ShadowMap.h"
#include "PassGlHelpers.h"
#include "core/Log.h"
#include "luminumbra_common/core/Environment.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <luminumbra/rendering/RenderView.h>
#include <stdexcept>
#include <string>

namespace Luminumbra::Rendering {

LightingPass::LightingPass() = default;
LightingPass::~LightingPass() {
    reset_contributions();
}

namespace {
// Only the opt-in draw touches attachment one. Save at most 32 draw-buffer slots.
class ContributionAttachment {
public:
    ContributionAttachment(GLuint framebuffer, GLuint texture)
        : framebuffer_(framebuffer) {
        if (texture == 0 || framebuffer_ == 0) {
            return;
        }
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &count_);
        GLint occupied = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                                              GL_COLOR_ATTACHMENT1,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                              &occupied);
        if (count_ < 2 || count_ > static_cast<GLint>(buffers_.size()) || occupied != GL_NONE) {
            return;
        }
        for (GLint index = 0; index < count_; ++index) {
            GLint buffer = 0;
            glGetIntegerv(GL_DRAW_BUFFER0 + static_cast<GLenum>(index), &buffer);
            buffers_[static_cast<std::size_t>(index)] = static_cast<GLenum>(buffer);
        }
        glGetBooleani_v(GL_COLOR_WRITEMASK, 1, mask_.data());
        blend_ = glIsEnabledi(GL_BLEND, 1);
        glDisablei(GL_BLEND, 1);
        // Initialize every texel, even when the submitted draw is scissored or clipped.
        glClearTexImage(texture, 0, GL_RGBA, GL_FLOAT, nullptr);
        attached_ = true;
        glFramebufferTexture2D(
            GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, texture, 0);
        constexpr std::array<GLenum, 2> outputs = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(static_cast<GLsizei>(outputs.size()), outputs.data());
        glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        ready_ = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!ready_) {
            detach();
        }
    }
    ~ContributionAttachment() {
        detach();
    }
    bool ready() const {
        return ready_;
    }
    void detach() {
        if (!attached_) {
            return;
        }
        GLint previous = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        glDrawBuffers(count_, buffers_.data());
        glColorMaski(1, mask_[0], mask_[1], mask_[2], mask_[3]);
        if (blend_ == GL_TRUE) {
            glEnablei(GL_BLEND, 1);
        } else {
            glDisablei(GL_BLEND, 1);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous));
        attached_ = false;
    }

private:
    GLuint framebuffer_;
    GLint count_ = 0;
    std::array<GLenum, 32> buffers_{};
    std::array<GLboolean, 4> mask_{};
    GLboolean blend_ = GL_FALSE;
    bool attached_ = false;
    bool ready_ = false;
};
} // namespace

void LightingPass::reset_contributions() {
    auto& control = DebugViewPass::controls();
    if (control.contribution_texture == m_contribution_texture) {
        control.contribution_texture = 0;
        control.contribution_view = DebugViewPass::None;
        control.contribution_size = {};
    }
    if (m_contribution_texture != 0) {
        glDeleteTextures(1, &m_contribution_texture);
    }
    m_contribution_texture = 0;
    m_contribution_width = 0;
    m_contribution_height = 0;
}

bool LightingPass::prepare_contributions(const RenderContext& ctx) {
    if (!DebugViewPass::contribution_extent(ctx.internal_w(), ctx.internal_h())) {
        return false;
    }
    if (m_contribution_source_program != m_lighting_shader->Id()) {
        // The normal shader participates in reload-all; rebuild the opt-in sibling lazily.
        m_contribution_shader.reset();
        m_contribution_source_program = m_lighting_shader->Id();
    }
    if (!m_contribution_shader) {
        m_contribution_shader =
            std::make_unique<Shader>(m_lighting_shader->VertexPath().c_str(),
                                     m_lighting_shader->FragmentPath().c_str(),
                                     std::vector<std::string>{},
                                     std::vector<std::string>{"LUMIN_DIAG_CONTRIBUTIONS 1"});
    }
    if (!m_contribution_shader->IsValid()) {
        return false;
    }
    if (m_contribution_width == ctx.internal_w() && m_contribution_height == ctx.internal_h()) {
        return m_contribution_texture != 0;
    }
    reset_contributions();
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (maximum <= 0 || ctx.internal_w() > static_cast<unsigned int>(maximum) ||
        ctx.internal_h() > static_cast<unsigned int>(maximum)) {
        return false;
    }
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glGenTextures(1, &m_contribution_texture);
    if (m_contribution_texture == 0) {
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, m_contribution_texture);
    glTexStorage2D(GL_TEXTURE_2D,
                   1,
                   GL_RGBA16F,
                   static_cast<GLsizei>(ctx.internal_w()),
                   static_cast<GLsizei>(ctx.internal_h()));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint allocated = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &allocated);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    if (allocated != static_cast<GLint>(ctx.internal_w())) {
        reset_contributions();
        return false;
    }
    m_contribution_width = ctx.internal_w();
    m_contribution_height = ctx.internal_h();
    return true;
}

void LightingPass::init_shader(const std::filesystem::path& root_path) {
    m_contribution_shader.reset();
    m_contribution_source_program = 0;
    reset_contributions();
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

void LightingPass::init_environment_brdf(RenderResourceRegistry& registry) {
    TextureDesc desc;
    desc.width = desc.height = kEnvironmentBrdfLutSize;
    desc.internal_format = GL_RG16F;
    desc.format = GL_RG;
    desc.type = GL_HALF_FLOAT;
    desc.min_filter = desc.mag_filter = GL_LINEAR;
    desc.wrap_s = desc.wrap_t = GL_CLAMP_TO_EDGE;
    desc.expected_layout = "sampled";
    desc.debug_label = "lighting.environment_brdf";
    m_environment_brdf = registry.create_texture("environment_brdf", desc).id;
    if (m_environment_brdf == 0)
        throw std::runtime_error("Unable to allocate the environment BRDF texture");
    glBindTexture(GL_TEXTURE_2D, m_environment_brdf);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    kEnvironmentBrdfLutSize,
                    kEnvironmentBrdfLutSize,
                    GL_RG,
                    GL_HALF_FLOAT,
                    kEnvironmentBrdfLut.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void LightingPass::destroy_environment_brdf(RenderResourceRegistry& registry) {
    registry.destroy_owned("environment_brdf");
    m_environment_brdf = 0;
}

void LightingPass::init_lighting_fbo(RenderResourceRegistry& registry, u32 width, u32 height) {
    // allocate the lighting FBO + attachments THROUGH the
    // registry. The descs reproduce the retired glTexImage2D/glRenderbufferStorage
    // calls exactly (RGBA16F LINEAR HDR color; RGBA16F LINEAR + CLAMP_TO_EDGE
    // opaque copy; DEPTH_COMPONENT32F renderbuffer) so the objects are
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
    depth.internal_format = GL_DEPTH_COMPONENT32F;
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
    reset_contributions();
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
    m_contribution_shader.reset();
    m_contribution_source_program = 0;
    reset_contributions();
    m_lighting_shader.reset();
}

void LightingPass::copy_lighting_color_to_opaque_texture(const RenderContext& ctx) {
    if (!m_lighting_fbo.fbo_id || !m_lighting_fbo.color_texture ||
        !m_lighting_fbo.opaque_color_texture) {
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    PreserveReplayObject(ReplayObjectKind::Framebuffer, m_lighting_fbo.fbo_id);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D, m_lighting_fbo.opaque_color_texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        0,
                        0,
                        static_cast<GLsizei>(ctx.internal_w()),
                        static_cast<GLsizei>(ctx.internal_h())); // internal-sized lit color
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void LightingPass::execute(const RenderContext& ctx) {
    LUMIN_PROFILE_ZONE_N("GL LightingPass.execute");
    if (!ctx.render_view && !ctx.camera)
        throw std::invalid_argument("Lighting requires a validated view or production camera");
    if (ctx.static_studio && (!ctx.render_view || ctx.authored_surface.id == 0))
        throw std::invalid_argument(
            "Static studio requires its exact view and material attachment");
    const glm::mat4 view = ctx.render_view ? glm::make_mat4(ctx.render_view->view().data())
                                           : ctx.camera->GetViewMatrix();
    const glm::mat4 projection =
        ctx.render_view ? glm::make_mat4(ctx.render_view->projection().data())
                        : ctx.camera->GetProjectionMatrix(static_cast<int>(ctx.screen_width),
                                                          static_cast<int>(ctx.screen_height));
    const glm::vec3 eye =
        ctx.render_view ? glm::make_vec3(ctx.render_view->eye().data()) : ctx.camera->Position;
    const float far_plane = ctx.render_view
                                ? static_cast<float>(ctx.render_view->description().far_plane)
                                : ctx.camera->GetFarPlane();
    auto& diagnostics = DebugViewPass::controls();
    diagnostics.contribution_texture = 0;
    diagnostics.contribution_view = DebugViewPass::None;
    const bool requested = diagnostics.enabled &&
                           DebugViewPass::supported_contribution(diagnostics.view) &&
                           !ctx.frozen_replay;
    const bool prepared = requested && prepare_contributions(ctx);
    Shader* lighting_shader = m_lighting_shader.get();
    const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    // The fullscreen quad lies at clip depth zero. Scene visibility comes from
    // the G-buffer, and its depth is blitted here after deferred lighting.
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    glViewport(0,
               0,
               static_cast<GLsizei>(ctx.internal_w()),
               static_cast<GLsizei>(ctx.internal_h())); // deferred lighting into the internal FBO
    glDepthMask(GL_TRUE);
    glClearDepth(0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ContributionAttachment contribution(m_lighting_fbo.fbo_id,
                                        prepared ? m_contribution_texture : 0);
    if (contribution.ready()) {
        lighting_shader = m_contribution_shader.get();
    }
    lighting_shader->use();
    if (contribution.ready()) {
        lighting_shader->setInt("u_contributionMode", diagnostics.view);
    }
    lighting_shader->setInt("u_farWorldLadder", ctx.far_world_ladder ? 1 : 0);
    lighting_shader->setMat4("u_gbufferInverseProjection", ctx.gbuffer_inverse_projection);
    lighting_shader->setInt("u_staticStudio", ctx.static_studio ? 1 : 0);
    lighting_shader->setVec3("u_studioBackground", ctx.studio_background);
    lighting_shader->setInt("u_authoredEnabled", ctx.authored_surface.id != 0 ? 1 : 0);
    lighting_shader->setInt("u_authoredSurface", 13);
    lighting_shader->setInt("u_sceneDepth", 4);
    glActiveTexture(GL_TEXTURE13);
    glBindTexture(GL_TEXTURE_2D, ctx.authored_surface.id);
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
    // Aether emissive field at unit 10 (gated by u_aetherActive). When
    // no field is uploaded the texture is 0 and u_aetherActive=0, so the glow term
    // is skipped -> pixel-identical to the pre- path.
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, ctx.aether_field.id);
    lighting_shader->setInt("u_aetherField", 10);
    // the tinted-transmission cascade at unit 11. The
    // array is init-cleared WHITE, so with no glass the multiply is exactly 1.0
    // (pixel-identical); u_shadowTintEnabled==0 skips the sampling entirely (the
    // same-build A/B lever: off vs on-with-white must FLIP to exactly 0.0).
    glActiveTexture(GL_TEXTURE11);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.shadow_tint_array.id);
    lighting_shader->setInt("u_shadowTintCascades", 11);
    lighting_shader->setInt("u_shadowTintEnabled", ctx.shadow_tint_array.id != 0 ? 1 : 0);
    glActiveTexture(GL_TEXTURE12);
    glBindTexture(GL_TEXTURE_2D, m_environment_brdf);
    lighting_shader->setInt("u_environmentBrdf", 12);
    if (ctx.aether_active && ctx.aether_extent > 0) {
        const float world_span = static_cast<float>(ctx.aether_extent) * ctx.aether_cell_size;
        lighting_shader->setFloat("u_aetherActive", 1.0f);
        lighting_shader->setVec2("u_aetherFieldWorldOrigin", ctx.aether_world_origin);
        lighting_shader->setFloat("u_aetherFieldInvWorldSpan",
                                  world_span > 0.0f ? (1.0f / world_span) : 0.0f);
    } else {
        lighting_shader->setFloat("u_aetherActive", 0.0f);
    }
    // the glow grade from the context — the defaults
    // equal the GLSL initializers, so untouched contexts stay pixel-identical.
    lighting_shader->setVec3("u_aetherGlowColor", ctx.aether_glow_color);
    lighting_shader->setFloat("u_aetherGlowIntensity", ctx.aether_glow_intensity);
    //  ( -6): emissive modulation — 0.0 default is a
    // multiply by exactly 1.0, so untouched contexts stay pixel-identical.
    lighting_shader->setFloat("u_aetherMaterialModulation", ctx.aether_material_modulation);
    //  (-8): polarity tint gate — 0.0 keeps the glow color
    // untouched (the single-channel/no-upload default).
    lighting_shader->setFloat("u_aetherPolarityActive", ctx.aether_polarity_active ? 1.0f : 0.0f);
    // the render-only snow cover (0 default = byte-identical).
    lighting_shader->setFloat("u_snowCover", ctx.snow_cover);
    glActiveTexture(GL_TEXTURE0);
    lighting_shader->setMat4("u_inverseView", glm::inverse(view));
    lighting_shader->setInt("gPosition", 0);
    lighting_shader->setInt("gNormalMaterial", 1);  // Octahedral normal + material
    lighting_shader->setInt("gAlbedoRoughness", 2); // Albedo + roughness
    lighting_shader->setInt("gMetallicAO", 3);      // Metallic + AO
    lighting_shader->setInt("gDepth", 4);
    lighting_shader->setInt("u_shadowCascades", 5);
    lighting_shader->setInt("u_ssao", 6);
    lighting_shader->setInt("u_terrainTextures", 7);
    lighting_shader->setInt("u_materialLUT", 8);
    lighting_shader->setFloat("u_emissiveLutScale", ctx.emissive_lut_scale);
    lighting_shader->setVec3("u_skyAmbientColor", ctx.sky_ambient_color);
    lighting_shader->setVec3("u_viewPos", eye);
    lighting_shader->setInt("u_orthographic", ctx.render_view && ctx.render_view->orthographic());
    lighting_shader->setVec3("u_sun.direction", ctx.sun.direction);
    lighting_shader->setVec3("u_sun.color", ctx.sun.color);
    // moon-shadows: the moon's TOWARD-LIGHT direction (anti-sun, overhead at
    // midnight; same convention the shader uses for u_sun.direction). The shader
    // lights + keys the cast-shadow lookup off this so moonlit terrain has real
    // directional form and shadows from the now-moon shadow cascade.
    lighting_shader->setVec3("u_moonDir", ctx.moon_light_dir);
    lighting_shader->setFloat("u_moonIllum", ctx.moon_illumination); // rendering: lunar phase
    lighting_shader->setVec3("u_moonRadiance",
                             ctx.moon_radiance); // rendering : dedicated moon radiance channel
    if (ctx.static_studio) {
        lighting_shader->setFloat("u_moonWrapFloor", 0.25f);
        lighting_shader->setFloat("u_exposure", ctx.exposure > 0 ? ctx.exposure : 1.0f);
        lighting_shader->setFloat("u_saturation", 1.0f);
        lighting_shader->setFloat("u_contrast", 1.0f);
        lighting_shader->setVec3("u_lightWarmth", glm::vec3(1));
        lighting_shader->setVec3("u_shadowTint", glm::vec3(1));
        lighting_shader->setVec3("u_highlightTint", glm::vec3(1));
        lighting_shader->setFloat("u_splitToneStrength", 0.0f);
    } else {
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
        lighting_shader->setFloat("u_moonWrapFloor", s_moon_wrap_floor);

        //  cinematic grade (-style): BOLD default — lifted exposure, rich
        // saturation, strong contrast, and a cool-shadow / warm-highlight split-tone
        // (the key/fill cue). Tunable via LUMIN_GRADE="exposure,saturation,contrast,
        // warmR,warmG,warmB" (parsed once); the split-tone is fixed cinematic.
        struct Grade {
            float exposure, saturation, contrast, wr, wg, wb;
        };
        static const Grade s_grade = [] {
            Grade g{1.12f,
                    1.30f,
                    1.42f,
                    1.06f,
                    1.0f,
                    0.92f}; // richer sat + punchier contrast (de-wash
                            // noon; owner "white filter" pass 2026-07-07)
            if (const auto env = Core::ReadEnvironment("LUMIN_GRADE")) {
                ParseCommaSeparatedPrefix(
                    *env, g.exposure, g.saturation, g.contrast, g.wr, g.wg, g.wb);
            }
            return g;
        }();
        //  rendering: the RenderContext exposure seam slot wins when set; the
        // sentinel 0 falls back to the static LUMIN_GRADE exposure (byte-identical until ).
        lighting_shader->setFloat("u_exposure",
                                  ctx.exposure > 0.0f ? ctx.exposure : s_grade.exposure);
        lighting_shader->setFloat("u_saturation", s_grade.saturation);
        lighting_shader->setFloat("u_contrast", s_grade.contrast);
        lighting_shader->setVec3("u_lightWarmth", glm::vec3(s_grade.wr, s_grade.wg, s_grade.wb));
        lighting_shader->setVec3("u_shadowTint", glm::vec3(0.88f, 0.96f, 1.14f));    // cool
        lighting_shader->setVec3("u_highlightTint", glm::vec3(1.14f, 1.04f, 0.84f)); // warm
        lighting_shader->setFloat("u_splitToneStrength", 0.55f);
    }
    const size_t point_light_count = ctx.point_lights ? ctx.point_lights->size() : 0;
    if (point_light_count > 32)
        throw std::invalid_argument("Lighting point-light count exceeds shader capacity");
    lighting_shader->setInt("u_pointLightCount", static_cast<int>(point_light_count));
    for (size_t i = 0; i < point_light_count; ++i) {
        std::string prefix = "u_pointLights[" + std::to_string(i) + "].";
        lighting_shader->setVec3(prefix + "position", (*ctx.point_lights)[i].position);
        lighting_shader->setVec3(prefix + "color", (*ctx.point_lights)[i].color);
        lighting_shader->setFloat(prefix + "radius", (*ctx.point_lights)[i].radius);
        lighting_shader->setFloat(prefix + "intensity", (*ctx.point_lights)[i].intensity);
    }
    // Cave / sky-visibility ambient occlusion (render-only). Default OFF =>
    // u_caveAmbientOcclusion 0.0 => the shader's skyVis term is exactly 1.0 =>
    // pixel-identical to the pre-fix path. Enabled + tuned via the LUMIN_CAVE_AO
    // env knob ("enabled,maxDist,floor,steps,thickness"). The probe needs the
    // same projection the SSAO pass builds, plus the screen size.
    {
        lighting_shader->setMat4("u_projection",
                                 ctx.far_world_ladder ? ctx.gbuffer_projection : projection);
        lighting_shader->setVec2(
            "u_screenSize",
            glm::vec2(ctx.internal_w(), ctx.internal_h())); // cave AO marches the internal G-buffer

        if (ctx.static_studio) {
            lighting_shader->setFloat("u_caveAmbientOcclusion", 0.0f);
        } else {
            static const auto s_caveAO =
                ResolveCaveAmbientOcclusion(Core::ReadEnvironment("LUMIN_CAVE_AO"));
            lighting_shader->setFloat("u_caveAmbientOcclusion", s_caveAO.enabled);
            lighting_shader->setFloat("u_caveSkyMaxDist", s_caveAO.maxDist);
            lighting_shader->setInt("u_caveSkySteps", s_caveAO.steps);
            lighting_shader->setFloat("u_caveAmbientFloor", s_caveAO.floor);
            lighting_shader->setFloat("u_caveThickness", s_caveAO.thickness);
        }
        // Optional point-light punch (shader Patch 4, not applied); defaults keep
        // legacy behaviour. setFloat on an absent uniform is a harmless no-op.
        lighting_shader->setFloat("u_pointLightFalloff", 0.05f);
        lighting_shader->setFloat("u_pointLightInvSqMix", 0.0f);
    }
    lighting_shader->setFloat("u_farPlane", far_plane);
    // -T12: the shadow-cascade validity/refresh fixup (which MUTATES the
    // shared ShadowMap private state + calls the pipeline-private
    // get_light_space_matrices) is hoisted to make_lighting_context; the resolved
    // splits + matrices arrive via ctx.cascade_splits + ctx.light_space_matrices.
    lighting_shader->setVec4("u_cascadeSplits", ctx.cascade_splits);
    for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i) {
        if (!ctx.light_space_matrices ||
            ctx.light_space_matrices->size() <= static_cast<size_t>(i)) {
            if (!ctx.static_studio)
                throw std::invalid_argument("Missing lighting cascade matrices");
            lighting_shader->setMat4("u_lightSpaceMatrices[" + std::to_string(i) + "]",
                                     glm::mat4(1));
            continue;
        }
        lighting_shader->setMat4("u_lightSpaceMatrices[" + std::to_string(i) + "]",
                                 (*ctx.light_space_matrices)[static_cast<std::size_t>(i)]);
    }
    glm::vec3 terrainOrigin(floor(eye.x / CHUNK_SIZE_X) * CHUNK_SIZE_X,
                            0.0f,
                            floor(eye.z / CHUNK_SIZE_Z) * CHUNK_SIZE_Z);
    lighting_shader->setVec3("u_terrainOrigin", terrainOrigin);
    // project the wind-advected cloud coverage onto the terrain as a
    // crawling cast shadow (directSun *= 1 - cloudShadow). The uniforms must match
    // the sky-dome's cloudCoverageAt field exactly so a dome cloud and its ground
    // shadow stay registered. u_cloudShadowEnabled==0 is the zero-cost OFF path
    // (the gate captures clouds-on vs clouds-off lighting ms).  .
    {
        const CloudRenderState& cloud = ctx.cloud_state;
        const bool shadow_on =
            cloud.enabled && cloud.shadow_enabled && cloud.shadow_strength > 0.0f;
        lighting_shader->setInt("u_cloudShadowEnabled", shadow_on ? 1 : 0);
        lighting_shader->setVec2("u_cloudScrollOffset", cloud.scroll_offset);
        lighting_shader->setFloat("u_cloudCoverageAmount",
                                  cloud.enabled ? cloud.coverage_amount : 0.0f);
        lighting_shader->setFloat("u_cloudBiomeVariation", cloud.biome_variation);
        lighting_shader->setFloat("u_cloudPlaneHeight", cloud.plane_height);
        lighting_shader->setFloat("u_cloudShadowStrength", cloud.shadow_strength);
        lighting_shader->setVec3("u_cloudSunDir", cloud.sun_travel_dir);
    }
    glBindVertexArray(ctx.screen_quad_vao);
    Luminumbra::Rendering::AttributedGl::Submit(
        glDrawArrays, static_cast<GLenum>(GL_TRIANGLE_STRIP), 0, 4);
    if (ctx.lighting_draws)
        ++(*ctx.lighting_draws);
    if (contribution.ready()) {
        diagnostics.contribution_texture = m_contribution_texture;
        diagnostics.contribution_size = {m_contribution_width, m_contribution_height};
        diagnostics.contribution_view = diagnostics.view;
    }
    contribution.detach();
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (depth_was_enabled)
        glEnable(GL_DEPTH_TEST);
}

void LightingPass::execute_lightning_overlay(const RenderContext& ctx) {
    LUMIN_PROFILE_ZONE_N("GL LightingPass.execute_lightning_overlay");
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
        if (ctx.frozen_replay)
            return;
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
        if (ctx.frozen_replay)
            return;
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
    PreserveReplayObject(ReplayObjectKind::Framebuffer, m_lighting_fbo.fbo_id);
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
    Luminumbra::Rendering::AttributedGl::Submit(
        glDrawArrays, static_cast<GLenum>(GL_TRIANGLE_STRIP), 0, 4);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (ctx.lighting_draws)
        ++(*ctx.lighting_draws);
}

} // namespace Luminumbra::Rendering
