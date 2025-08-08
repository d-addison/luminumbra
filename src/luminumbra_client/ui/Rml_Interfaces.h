#pragma once
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/FileInterface.h>
#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <string>
#include <cstdio>

namespace Luminumbra::Client {

// RmlUi System Interface
class RmlSystem : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        return glfwGetTime();
    }
};

// RmlUi File Interface
class RmlFileInterface : public Rml::FileInterface {
public:
    RmlFileInterface(const std::string& root_path);
    Rml::FileHandle Open(const Rml::String& path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
private:
    std::string m_root;
};

// RmlUi Render Interface (OpenGL)
class RmlRenderer : public Rml::RenderInterface {
public:
    RmlRenderer();
    void SetViewport(int width, int height);

    // Rml::RenderInterface implementation
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    
    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const unsigned char> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture_handle) override;

private:
    GLuint m_program = 0;
    GLint m_translation_loc = -1, m_projection_loc = -1;
    int m_width = 0, m_height = 0;
    
    struct CompiledGeometry {
        GLuint vao, vbo, ebo;
        int num_indices;
    };
};

} // namespace Luminumbra::Client
