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
    PassGl::label_gl_object(GL_PROGRAM, m_ssao.ssaoShader ? m_ssao.ssaoShader->Id() : 0u, "shader.ssao");
    PassGl::label_gl_object(GL_PROGRAM, m_ssao.blurShader ? m_ssao.blurShader->Id() : 0u, "shader.ssao_blur");
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
}

void SsaoPass::reset_shaders() {
    m_ssao.ssaoShader.reset();
    m_ssao.blurShader.reset();
}

void SsaoPass::execute_ssao(RenderPipeline& pipeline, const Camera& camera) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.fbo);
    glClear(GL_COLOR_BUFFER_BIT);
    m_ssao.ssaoShader->use();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, pipeline.m_gbuffer_pass->gbuffer().position_texture);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, pipeline.m_gbuffer_pass->gbuffer().normal_texture);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_ssao.noiseTexture);
    m_ssao.ssaoShader->setInt("gPosition", 0);
    m_ssao.ssaoShader->setInt("gNormalMaterial", 1);
    m_ssao.ssaoShader->setInt("u_noiseTexture", 2);
    for (unsigned int i = 0; i < 64; ++i)
        m_ssao.ssaoShader->setVec3("u_samples[" + std::to_string(i) + "]", m_ssao.kernel[i]);
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    m_ssao.ssaoShader->setMat4("u_projection", projection);
    m_ssao.ssaoShader->setVec2("u_screenSize", glm::vec2(pipeline.m_screen_width, pipeline.m_screen_height));
    glBindVertexArray(pipeline.m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    pipeline.m_last_render_pass_stats.ssao_draws++;
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SsaoPass::execute_blur(RenderPipeline& pipeline) {
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
