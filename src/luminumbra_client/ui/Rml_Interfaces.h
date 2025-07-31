#pragma once
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/RenderInterface.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace Luminumbra::Client {

// RmlUi System Interface
class RmlSystem : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        return glfwGetTime();
    }
};

// RmlUi Render Interface (OpenGL)
class RmlRenderer : public Rml::RenderInterface {
public:
    RmlRenderer();
    void SetViewport(int width, int height);

    // Rml::RenderInterface implementation
    void RenderGeometry(Rml::Vertex* vertices, int num_vertices, int* indices, int num_indices, Rml::TextureHandle texture, const Rml::Vector2f& translation) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(int x, int y, int width, int height) override;
    bool LoadTexture(Rml::TextureHandle& texture_handle, Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    bool GenerateTexture(Rml::TextureHandle& texture_handle, const Rml::byte* source, const Rml::Vector2i& source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture_handle) override;

private:
    GLuint m_program = 0;
    GLuint m_vbo = 0, m_vao = 0, m_ebo = 0;
    GLint m_translation_loc = -1, m_projection_loc = -1;
    int m_width = 0, m_height = 0;
};

} // namespace Luminumbra::Client