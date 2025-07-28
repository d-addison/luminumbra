#pragma once
#include <glad/gl.h>
#include "luminumbra/core/GLError.h"

namespace Luminumbra::Rendering {

class WaterRenderer {
public:
    // Constants for the FBO dimensions, can be lower than screen resolution for performance
    static constexpr int REFLECTION_WIDTH = 1280;
    static constexpr int REFLECTION_HEIGHT = 720;
    static constexpr int REFRACTION_WIDTH = 1280;
    static constexpr int REFRACTION_HEIGHT = 720;

    WaterRenderer();
    ~WaterRenderer();

    void bindReflectionFBO();
    void bindRefractionFBO();
    void unbindCurrentFBO(int screenWidth, int screenHeight);

    GLuint getReflectionTexture() const { return m_ReflectionTexture; }
    GLuint getRefractionTexture() const { return m_RefractionTexture; }
    GLuint getRefractionDepthTexture() const;

private:
    void initFBOs();
    GLuint createFBO();
    GLuint createTextureAttachment(int width, int height);
    GLuint createDepthBufferAttachment(int width, int height);

    GLuint m_ReflectionFBO = 0;
    GLuint m_ReflectionTexture = 0;
    GLuint m_ReflectionDepthBuffer = 0;

    GLuint m_RefractionFBO = 0;
    GLuint m_RefractionTexture = 0;
    GLuint m_RefractionDepthTexture = 0; // Use a texture for refraction depth
};

} // namespace Luminumbra::Rendering