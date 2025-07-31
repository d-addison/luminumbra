#include "luminumbra/rendering/WaterRenderer.h"
#include <stdexcept>

namespace Luminumbra::Rendering {


WaterRenderer::WaterRenderer() {
    initFBOs();
}

WaterRenderer::~WaterRenderer() {
    glDeleteFramebuffers(1, &m_ReflectionFBO);
    glDeleteTextures(1, &m_ReflectionTexture);
    glDeleteRenderbuffers(1, &m_ReflectionDepthBuffer);
    glDeleteFramebuffers(1, &m_RefractionFBO);
    glDeleteTextures(1, &m_RefractionTexture);
    glDeleteTextures(1, &m_RefractionDepthTexture);
}

void WaterRenderer::bindReflectionFBO() {
    GLCall(glBindFramebuffer(GL_FRAMEBUFFER, m_ReflectionFBO));
    GLCall(glViewport(0, 0, REFLECTION_WIDTH, REFLECTION_HEIGHT));
}

void WaterRenderer::bindRefractionFBO() {
    GLCall(glBindFramebuffer(GL_FRAMEBUFFER, m_RefractionFBO));
    GLCall(glViewport(0, 0, REFRACTION_WIDTH, REFRACTION_HEIGHT));
}

void WaterRenderer::unbindCurrentFBO(int screenWidth, int screenHeight) {
    GLCall(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GLCall(glViewport(0, 0, screenWidth, screenHeight));
}

GLuint WaterRenderer::createDepthTextureAttachment(int width, int height) {
    GLuint texture;
    GLCall(glGenTextures(1, &texture));
    GLCall(glBindTexture(GL_TEXTURE_2D, texture));
    // Use GL_DEPTH_COMPONENT as the internal format and provide a depth-compatible type
    GLCall(glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr));
    GLCall(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GLCall(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    // Attach it as a depth texture
    GLCall(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture, 0));
    return texture;
}

void WaterRenderer::initFBOs() {
    // Reflection FBO
    m_ReflectionFBO = createFBO();
    m_ReflectionTexture = createTextureAttachment(REFLECTION_WIDTH, REFLECTION_HEIGHT);
    m_ReflectionDepthBuffer = createDepthBufferAttachment(REFLECTION_WIDTH, REFLECTION_HEIGHT);
    GLCall(glBindFramebuffer(GL_FRAMEBUFFER, 0));

    // Refraction FBO
    m_RefractionFBO = createFBO();
    m_RefractionTexture = createTextureAttachment(REFRACTION_WIDTH, REFRACTION_HEIGHT);
    // CRITICAL CHANGE: Call the new function to create a depth TEXTURE
    m_RefractionDepthTexture = createDepthTextureAttachment(REFRACTION_WIDTH, REFRACTION_HEIGHT); // <-- FIX

    // Check if framebuffer is complete
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        // Handle error: Framebuffer is not complete!
    }
    GLCall(glBindFramebuffer(GL_FRAMEBUFFER, 0));
}

GLuint WaterRenderer::createFBO() {
    GLuint fbo;
    GLCall(glGenFramebuffers(1, &fbo));
    GLCall(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    GLCall(glDrawBuffer(GL_COLOR_ATTACHMENT0));
    return fbo;
}

GLuint WaterRenderer::createTextureAttachment(int width, int height) {
    GLuint texture;
    GLCall(glGenTextures(1, &texture));
    GLCall(glBindTexture(GL_TEXTURE_2D, texture));
    GLCall(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr));
    GLCall(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GLCall(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GLCall(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0));
    return texture;
}

GLuint WaterRenderer::createDepthBufferAttachment(int width, int height) {
    GLuint depthBuffer;
    GLCall(glGenRenderbuffers(1, &depthBuffer));
    GLCall(glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer));
    GLCall(glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height));
    GLCall(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthBuffer));
    return depthBuffer;
}

GLuint WaterRenderer::getRefractionDepthTexture() const {
    return m_RefractionDepthTexture;
}

} // namespace Luminumbra::Rendering