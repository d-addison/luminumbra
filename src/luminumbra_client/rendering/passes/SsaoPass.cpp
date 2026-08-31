#include "SsaoPass.h"

#include "../PassShaderLayouts.h" // enumerable ExpectedLayout registry
#include "../RenderResourceRegistry.h"
#include "PassGlHelpers.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

SsaoPass::SsaoPass() = default;
SsaoPass::~SsaoPass() = default;

void SsaoPass::init_shaders(const std::filesystem::path& root_path) {
    m_ssao.ssaoShader =
        std::make_unique<Shader>((root_path / "res/shaders/ssao.vert").string().c_str(),
                                 (root_path / "res/shaders/ssao.frag").string().c_str());
    m_ssao.blurShader =
        std::make_unique<Shader>((root_path / "res/shaders/ssao.vert").string().c_str(),
                                 (root_path / "res/shaders/ssao_blur.frag").string().c_str());
    // Render-optimization (ssao-gtao): GTAO horizon-slice variant, used when
    // ssao_quality > 0. Same fullscreen-quad vertex stage + same FBO target.
    m_ssao.gtaoShader =
        std::make_unique<Shader>((root_path / "res/shaders/ssao.vert").string().c_str(),
                                 (root_path / "res/shaders/ssao_gtao.frag").string().c_str());
    // ssao_quality 3: joint-bilateral depth-aware upsample of the half-res GTAO.
    m_ssao.upsampleShader = std::make_unique<Shader>(
        (root_path / "res/shaders/ssao.vert").string().c_str(),
        (root_path / "res/shaders/ssao_bilateral_upsample.frag").string().c_str());
    PassGl::label_gl_object(
        GL_PROGRAM, m_ssao.ssaoShader ? m_ssao.ssaoShader->Id() : 0u, "shader.ssao");
    PassGl::label_gl_object(
        GL_PROGRAM, m_ssao.blurShader ? m_ssao.blurShader->Id() : 0u, "shader.ssao_blur");
    PassGl::label_gl_object(
        GL_PROGRAM, m_ssao.gtaoShader ? m_ssao.gtaoShader->Id() : 0u, "shader.ssao_gtao");
    PassGl::label_gl_object(GL_PROGRAM,
                            m_ssao.upsampleShader ? m_ssao.upsampleShader->Id() : 0u,
                            "shader.ssao_bilateral_upsample");

    // validate each AO program's sampler bindings against the registry.
    const auto validate = [](const std::unique_ptr<Shader>& s, const char* name) {
        if (s && s->IsValid()) {
            if (const ExpectedLayout* layout = FindPassExpectedLayout(name))
                s->ValidateLayout(*layout);
        }
    };
    validate(m_ssao.ssaoShader, "ssao");
    validate(m_ssao.blurShader, "ssao_blur");
    validate(m_ssao.gtaoShader, "ssao_gtao");
    validate(m_ssao.upsampleShader, "ssao_bilateral_upsample");
}

void SsaoPass::init_ssao(RenderResourceRegistry& registry, u32 width, u32 height) {
    // the AO render targets are registry-owned. Each desc
    // reproduces the retired glTexImage2D/glTexParameter calls exactly (R16F +
    // the filter/wrap each target used) so the objects are parameter-identical;
    // the SSAOData struct caches the owned ids. The single-COLOR0 FBOs use the
    // registry's empty-draw-buffers default (matching the retired code, which
    // never called glDrawBuffers). The noise texture below stays pass-owned.
    const auto ao_desc = [](u32 w, u32 h, u32 filter, u32 wrap, const char* label) {
        TextureDesc d;
        d.width = w;
        d.height = h;
        d.internal_format = GL_R16F;
        d.format = GL_RED;
        d.type = GL_FLOAT;
        d.min_filter = filter;
        d.mag_filter = filter;
        d.wrap_s = wrap; // 0 = leave GL default (the raw/blur targets set no wrap)
        d.wrap_t = wrap;
        d.expected_layout = "color_attachment";
        d.debug_label = label;
        return d;
    };
    const auto ao_fbo = [](const char* tex_name, const char* label) {
        FboDesc f;
        f.attachments = {{GL_COLOR_ATTACHMENT0, tex_name}};
        f.debug_label = label;
        return f;
    };

    // Full-res raw AO: R16F, NEAREST, default wrap.
    m_ssao.ssaoColorBuffer =
        registry.create_texture("ssao_raw", ao_desc(width, height, GL_NEAREST, 0, "ssao.raw")).id;
    m_ssao.fbo = registry.create_fbo("ssao_fbo", ao_fbo("ssao_raw", "ssao.fbo")).id;
    // Full-res blurred AO: R16F, NEAREST, default wrap.
    m_ssao.ssaoColorBufferBlur =
        registry.create_texture("ssao_blur", ao_desc(width, height, GL_NEAREST, 0, "ssao.blur")).id;
    m_ssao.blurFBO = registry.create_fbo("ssao_blur_fbo", ao_fbo("ssao_blur", "ssao.blur_fbo")).id;
    // Half-res GTAO target (ssao_quality 3): 1/2 per axis, R16F, LINEAR,
    // CLAMP_TO_EDGE. The horizon march runs at 1/4 the fragments, then a
    // depth-aware upsample reconstructs full res.
    m_ssao.halfW = (width > 1) ? width / 2u : 1u;
    m_ssao.halfH = (height > 1) ? height / 2u : 1u;
    m_ssao.halfTex =
        registry
            .create_texture(
                "ssao_half",
                ao_desc(m_ssao.halfW, m_ssao.halfH, GL_LINEAR, GL_CLAMP_TO_EDGE, "ssao.half"))
            .id;
    m_ssao.halfFBO = registry.create_fbo("ssao_half_fbo", ao_fbo("ssao_half", "ssao.half_fbo")).id;

    std::uniform_real_distribution<float> randomFloats(0.0, 1.0);
    std::default_random_engine generator;
    for (unsigned int i = 0; i < 64; ++i) {
        glm::vec3 sample(randomFloats(generator) * 2.0 - 1.0,
                         randomFloats(generator) * 2.0 - 1.0,
                         randomFloats(generator));
        sample = glm::normalize(sample);
        sample *= randomFloats(generator);
        float scale = (float)i / 64.0f;
        scale = std::lerp(0.1f, 1.0f, scale * scale);
        sample *= scale;
        m_ssao.kernel.push_back(sample);
    }
    std::vector<glm::vec3> ssaoNoise;
    for (unsigned int i = 0; i < 16; i++) {
        ssaoNoise.push_back(glm::vec3(
            randomFloats(generator) * 2.0 - 1.0, randomFloats(generator) * 2.0 - 1.0, 0.0f));
    }
    glGenTextures(1, &m_ssao.noiseTexture);
    PassGl::label_gl_object(GL_TEXTURE, m_ssao.noiseTexture, "ssao.noise");
    glBindTexture(GL_TEXTURE_2D, m_ssao.noiseTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 4, 4, 0, GL_RGB, GL_FLOAT, &ssaoNoise[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void SsaoPass::destroy_ssao(RenderResourceRegistry& registry) {
    // Ownership contract: the registry deletes the owned AO render targets.
    registry.destroy_owned("ssao_fbo");
    registry.destroy_owned("ssao_blur_fbo");
    registry.destroy_owned("ssao_half_fbo");
    registry.destroy_owned("ssao_raw");
    registry.destroy_owned("ssao_blur");
    registry.destroy_owned("ssao_half");
    m_ssao.fbo = 0;
    m_ssao.blurFBO = 0;
    m_ssao.halfFBO = 0;
    m_ssao.ssaoColorBuffer = 0;
    m_ssao.ssaoColorBufferBlur = 0;
    m_ssao.halfTex = 0;
    // The 4x4 noise texture is pass-owned (uploaded data) - delete it directly.
    if (m_ssao.noiseTexture) {
        glDeleteTextures(1, &m_ssao.noiseTexture);
        m_ssao.noiseTexture = 0;
    }
    m_ssao.halfW = 0;
    m_ssao.halfH = 0;
}

void SsaoPass::reset_shaders() {
    m_ssao.ssaoShader.reset();
    m_ssao.blurShader.reset();
    m_ssao.gtaoShader.reset();
    m_ssao.upsampleShader.reset();
}

// converted from execute_ssao(RenderPipeline&, Camera&).
// Mechanical seam swap — identical GL sequence, sourcing G-buffer position/normal,
// the fullscreen quad, screen size, quality, and camera from the RenderContext.
// The ssao_draws stat bump moved to the call site (the pipeline owns stats).
void SsaoPass::execute_ssao(const RenderContext& ctx) {
    const Camera& camera = *ctx.camera;
    const int quality = ctx.ssao_quality;
    // quality 3 = half-res GTAO: render into the 1/2-per-axis FBO (1/4 the fragments),
    // then execute_blur does the depth-aware upsample to full res.
    const bool halfres = (quality == 3) && m_ssao.halfFBO != 0 && m_ssao.upsampleShader &&
                         m_ssao.upsampleShader->IsValid();
    glBindFramebuffer(GL_FRAMEBUFFER, halfres ? m_ssao.halfFBO : m_ssao.fbo);
    if (halfres)
        glViewport(0, 0, static_cast<GLsizei>(m_ssao.halfW), static_cast<GLsizei>(m_ssao.halfH));
    glClear(GL_COLOR_BUFFER_BIT);
    const glm::mat4 projection =
        glm::perspective(glm::radians(camera.Zoom),
                         (float)ctx.screen_width / (float)ctx.screen_height,
                         camera.GetNearPlane(),
                         camera.GetFarPlane());
    // the G-buffer position/normal are the INTERNAL (scaled) extent, so the AO
    // march metric + noise tiling must use the internal size (at scale 1.0 internal==screen,
    // byte-identical). The half-res GTAO sub-scaling composes on top of this.
    const glm::vec2 screen_size(ctx.internal_w(), ctx.internal_h());

    if (quality > 0 && m_ssao.gtaoShader && m_ssao.gtaoShader->IsValid()) {
        // Render-optimization (ssao-gtao): XeGTAO horizon-slice AO. Reads the SAME
        // view-space G-buffer. High = 3x6 = 18 spp; Low = 2x4 = 8 spp.
        m_ssao.gtaoShader->use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_position.id);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_normal.id);
        m_ssao.gtaoShader->setInt("gPosition", 0);
        m_ssao.gtaoShader->setInt("gNormalMaterial", 1);
        m_ssao.gtaoShader->setMat4("u_projection", projection);
        m_ssao.gtaoShader->setVec2("u_screenSize", screen_size);
        // quality 1 = Low full-res; 2 = High full-res; 3 = half-res (Low spp — the
        // bilateral upsample/denoise compensates, and half-res already cuts the
        // fragment count ~4x, so High spp there is wasteful + busts the budget).
        const bool high = (quality == 2);
        m_ssao.gtaoShader->setInt("u_sliceCount", high ? 3 : 2);
        m_ssao.gtaoShader->setInt("u_stepsPerSlice", high ? 6 : 4);
        m_ssao.gtaoShader->setFloat("u_radius", 0.8f);
    } else {
        m_ssao.ssaoShader->use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_position.id);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_normal.id);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_ssao.noiseTexture);
        m_ssao.ssaoShader->setInt("gPosition", 0);
        m_ssao.ssaoShader->setInt("gNormalMaterial", 1);
        m_ssao.ssaoShader->setInt("u_noiseTexture", 2);
        for (unsigned int i = 0; i < 64; ++i)
            m_ssao.ssaoShader->setVec3("u_samples[" + std::to_string(i) + "]", m_ssao.kernel[i]);
        m_ssao.ssaoShader->setMat4("u_projection", projection);
        m_ssao.ssaoShader->setVec2("u_screenSize", screen_size);
    }
    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    if (halfres)
        glViewport(0,
                   0,
                   static_cast<GLsizei>(ctx.internal_w()),
                   static_cast<GLsizei>(ctx.internal_h())); // internal extent
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SsaoPass::execute_blur(const RenderContext& ctx) {
    // Half-res GTAO (quality 3): joint-bilateral depth-aware upsample of the half-res
    // AO into the full-res blur target (replaces the box blur; also denoises).
    if (ctx.ssao_quality == 3 && m_ssao.halfFBO != 0 && m_ssao.upsampleShader &&
        m_ssao.upsampleShader->IsValid()) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.blurFBO);
        glViewport(0,
                   0,
                   static_cast<GLsizei>(ctx.internal_w()),
                   static_cast<GLsizei>(ctx.internal_h())); // internal extent
        glClear(GL_COLOR_BUFFER_BIT);
        m_ssao.upsampleShader->use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_ssao.halfTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_position.id);
        m_ssao.upsampleShader->setInt("u_aoHalf", 0);
        m_ssao.upsampleShader->setInt("gPosition", 1);
        m_ssao.upsampleShader->setVec2(
            "u_halfTexel", glm::vec2(1.0f / (float)m_ssao.halfW, 1.0f / (float)m_ssao.halfH));
        glBindVertexArray(ctx.screen_quad_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.blurFBO);
    glClear(GL_COLOR_BUFFER_BIT);
    m_ssao.blurShader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssao.ssaoColorBuffer);
    m_ssao.blurShader->setInt("u_ssaoInput", 0);
    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace Luminumbra::Rendering
