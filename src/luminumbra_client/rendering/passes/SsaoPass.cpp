#include "SsaoPass.h"

#include "GBufferPass.h"
#include "PassGlHelpers.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

SsaoPass::SsaoPass() = default;
SsaoPass::~SsaoPass() = default;

void SsaoPass::init_shaders(const std::filesystem::path& root_path) {
    m_ssao.ssaoShader = std::make_unique<Shader>((root_path / "res/shaders/ssao.vert").string().c_str(), (root_path / "res/shaders/ssao.frag").string().c_str());
    m_ssao.blurShader = std::make_unique<Shader>((root_path / "res/shaders/ssao.vert").string().c_str(), (root_path / "res/shaders/ssao_blur.frag").string().c_str());
    // Render-optimization (ssao-gtao): GTAO horizon-slice variant, used when
    // ssao_quality > 0. Same fullscreen-quad vertex stage + same FBO target.
    m_ssao.gtaoShader = std::make_unique<Shader>((root_path / "res/shaders/ssao.vert").string().c_str(), (root_path / "res/shaders/ssao_gtao.frag").string().c_str());
    // ssao_quality 3: joint-bilateral depth-aware upsample of the half-res GTAO.
    m_ssao.upsampleShader = std::make_unique<Shader>((root_path / "res/shaders/ssao.vert").string().c_str(), (root_path / "res/shaders/ssao_bilateral_upsample.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_ssao.ssaoShader ? m_ssao.ssaoShader->Id() : 0u, "shader.ssao");
    PassGl::label_gl_object(GL_PROGRAM, m_ssao.blurShader ? m_ssao.blurShader->Id() : 0u, "shader.ssao_blur");
    PassGl::label_gl_object(GL_PROGRAM, m_ssao.gtaoShader ? m_ssao.gtaoShader->Id() : 0u, "shader.ssao_gtao");
    PassGl::label_gl_object(GL_PROGRAM, m_ssao.upsampleShader ? m_ssao.upsampleShader->Id() : 0u, "shader.ssao_bilateral_upsample");
}

void SsaoPass::init_ssao(u32 width, u32 height) {
    glGenFramebuffers(1, &m_ssao.fbo);
    glGenFramebuffers(1, &m_ssao.blurFBO);
    PassGl::label_gl_object(GL_FRAMEBUFFER, m_ssao.fbo, "ssao.fbo");
    PassGl::label_gl_object(GL_FRAMEBUFFER, m_ssao.blurFBO, "ssao.blur_fbo");
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.fbo);
    glGenTextures(1, &m_ssao.ssaoColorBuffer);
    PassGl::label_gl_object(GL_TEXTURE, m_ssao.ssaoColorBuffer, "ssao.raw");
    glBindTexture(GL_TEXTURE_2D, m_ssao.ssaoColorBuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, width, height, 0, GL_RED, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssao.ssaoColorBuffer, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.blurFBO);
    glGenTextures(1, &m_ssao.ssaoColorBufferBlur);
    PassGl::label_gl_object(GL_TEXTURE, m_ssao.ssaoColorBufferBlur, "ssao.blur");
    glBindTexture(GL_TEXTURE_2D, m_ssao.ssaoColorBufferBlur);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, width, height, 0, GL_RED, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssao.ssaoColorBufferBlur, 0);
    // Half-res GTAO target (ssao_quality 3): 1/2 per axis. The horizon march runs at
    // 1/4 the fragments, then a depth-aware upsample reconstructs full res.
    m_ssao.halfW = (width > 1) ? width / 2u : 1u;
    m_ssao.halfH = (height > 1) ? height / 2u : 1u;
    glGenFramebuffers(1, &m_ssao.halfFBO);
    PassGl::label_gl_object(GL_FRAMEBUFFER, m_ssao.halfFBO, "ssao.half_fbo");
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.halfFBO);
    glGenTextures(1, &m_ssao.halfTex);
    PassGl::label_gl_object(GL_TEXTURE, m_ssao.halfTex, "ssao.half");
    glBindTexture(GL_TEXTURE_2D, m_ssao.halfTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, static_cast<GLsizei>(m_ssao.halfW), static_cast<GLsizei>(m_ssao.halfH), 0, GL_RED, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssao.halfTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::uniform_real_distribution<float> randomFloats(0.0, 1.0);
    std::default_random_engine generator;
    for (unsigned int i = 0; i < 64; ++i) {
        glm::vec3 sample(randomFloats(generator) * 2.0 - 1.0, randomFloats(generator) * 2.0 - 1.0, randomFloats(generator));
        sample = glm::normalize(sample);
        sample *= randomFloats(generator);
        float scale = (float)i / 64.0f;
        scale = std::lerp(0.1f, 1.0f, scale * scale);
        sample *= scale;
        m_ssao.kernel.push_back(sample);
    }
    std::vector<glm::vec3> ssaoNoise;
    for (unsigned int i = 0; i < 16; i++) {
        ssaoNoise.push_back(glm::vec3(randomFloats(generator) * 2.0 - 1.0, randomFloats(generator) * 2.0 - 1.0, 0.0f));
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

void SsaoPass::destroy_ssao() {
    if (m_ssao.fbo) { glDeleteFramebuffers(1, &m_ssao.fbo); m_ssao.fbo = 0; }
    if (m_ssao.blurFBO) { glDeleteFramebuffers(1, &m_ssao.blurFBO); m_ssao.blurFBO = 0; }
    if (m_ssao.ssaoColorBuffer) { glDeleteTextures(1, &m_ssao.ssaoColorBuffer); m_ssao.ssaoColorBuffer = 0; }
    if (m_ssao.ssaoColorBufferBlur) { glDeleteTextures(1, &m_ssao.ssaoColorBufferBlur); m_ssao.ssaoColorBufferBlur = 0; }
    if (m_ssao.noiseTexture) { glDeleteTextures(1, &m_ssao.noiseTexture); m_ssao.noiseTexture = 0; }
    if (m_ssao.halfFBO) { glDeleteFramebuffers(1, &m_ssao.halfFBO); m_ssao.halfFBO = 0; }
    if (m_ssao.halfTex) { glDeleteTextures(1, &m_ssao.halfTex); m_ssao.halfTex = 0; }
    m_ssao.halfW = 0; m_ssao.halfH = 0;
}

void SsaoPass::reset_shaders() {
    m_ssao.ssaoShader.reset();
    m_ssao.blurShader.reset();
    m_ssao.gtaoShader.reset();
    m_ssao.upsampleShader.reset();
}

void SsaoPass::execute_ssao(RenderPipeline& pipeline, const Camera& camera) {
    const int quality = pipeline.get_ssao_quality();
    // quality 3 = half-res GTAO: render into the 1/2-per-axis FBO (1/4 the fragments),
    // then execute_blur does the depth-aware upsample to full res.
    const bool halfres = (quality == 3) && m_ssao.halfFBO != 0 && m_ssao.upsampleShader && m_ssao.upsampleShader->IsValid();
    glBindFramebuffer(GL_FRAMEBUFFER, halfres ? m_ssao.halfFBO : m_ssao.fbo);
    if (halfres) glViewport(0, 0, static_cast<GLsizei>(m_ssao.halfW), static_cast<GLsizei>(m_ssao.halfH));
    glClear(GL_COLOR_BUFFER_BIT);
    const glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    // gPosition is FULL-res; the march metric uses the full screen size regardless of
    // the (possibly half-res) output viewport.
    const glm::vec2 screen_size(pipeline.m_screen_width, pipeline.m_screen_height);

    if (quality > 0 && m_ssao.gtaoShader && m_ssao.gtaoShader->IsValid()) {
        // Render-optimization (ssao-gtao): XeGTAO horizon-slice AO. Reads the SAME
        // view-space G-buffer. High = 3x6 = 18 spp; Low = 2x4 = 8 spp.
        m_ssao.gtaoShader->use();
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, pipeline.m_gbuffer_pass->gbuffer().position_texture);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, pipeline.m_gbuffer_pass->gbuffer().normal_texture);
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
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, pipeline.m_gbuffer_pass->gbuffer().position_texture);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, pipeline.m_gbuffer_pass->gbuffer().normal_texture);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_ssao.noiseTexture);
        m_ssao.ssaoShader->setInt("gPosition", 0);
        m_ssao.ssaoShader->setInt("gNormalMaterial", 1);
        m_ssao.ssaoShader->setInt("u_noiseTexture", 2);
        for (unsigned int i = 0; i < 64; ++i)
            m_ssao.ssaoShader->setVec3("u_samples[" + std::to_string(i) + "]", m_ssao.kernel[i]);
        m_ssao.ssaoShader->setMat4("u_projection", projection);
        m_ssao.ssaoShader->setVec2("u_screenSize", screen_size);
    }
    glBindVertexArray(pipeline.m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    pipeline.m_last_render_pass_stats.ssao_draws++;
    glBindVertexArray(0);
    if (halfres) glViewport(0, 0, static_cast<GLsizei>(pipeline.m_screen_width), static_cast<GLsizei>(pipeline.m_screen_height));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SsaoPass::execute_blur(RenderPipeline& pipeline) {
    // Half-res GTAO (quality 3): joint-bilateral depth-aware upsample of the half-res
    // AO into the full-res blur target (replaces the box blur; also denoises).
    if (pipeline.get_ssao_quality() == 3 && m_ssao.halfFBO != 0 &&
        m_ssao.upsampleShader && m_ssao.upsampleShader->IsValid()) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.blurFBO);
        glViewport(0, 0, static_cast<GLsizei>(pipeline.m_screen_width), static_cast<GLsizei>(pipeline.m_screen_height));
        glClear(GL_COLOR_BUFFER_BIT);
        m_ssao.upsampleShader->use();
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_ssao.halfTex);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, pipeline.m_gbuffer_pass->gbuffer().position_texture);
        m_ssao.upsampleShader->setInt("u_aoHalf", 0);
        m_ssao.upsampleShader->setInt("gPosition", 1);
        m_ssao.upsampleShader->setVec2("u_halfTexel", glm::vec2(1.0f / (float)m_ssao.halfW, 1.0f / (float)m_ssao.halfH));
        glBindVertexArray(pipeline.m_screen_quad_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        pipeline.m_last_render_pass_stats.ssao_blur_draws++;
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
    glBindVertexArray(pipeline.m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    pipeline.m_last_render_pass_stats.ssao_blur_draws++;
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace Luminumbra::Rendering
