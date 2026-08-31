#pragma once

#include <RmlUi/Core.h>
#include <glad/glad.h>
#include <string>
#include "core/Log.h" // Your spdlog wrapper

namespace Luminumbra::Client {

// --- RmlUi System Interface ---
// Handles time and logging for RmlUi.
class RmlSystem : public Rml::SystemInterface {
public:
    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
};

// --- RmlUi File Interface ---
// Handles loading files (like .rml, .rcss, fonts) from disk.
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

// --- RmlUi Render Interface ---
// Handles all OpenGL rendering for RmlUi.
class RmlRenderer : public Rml::RenderInterface {
public:
    RmlRenderer();

    // This is called by Rml_UIManager to update viewport size.
    void SetViewport(int width, int height);

    // Virtual functions required by Rml::RenderInterface
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture_handle) override;

private:
    struct CompiledGeometry {
        GLuint vao = 0, vbo = 0, ebo = 0;
        int num_indices = 0;
    };

    GLuint m_program = 0;
    GLint m_translation_loc = -1, m_projection_loc = -1;
    int m_width = 0, m_height = 0;
    // 1x1 opaque-white texture bound for untextured (solid-colour) geometry, so the
    // `texture(uTexture,uv) * fragColor` shader yields the fill colour instead of black
    // when RmlUi passes texture handle 0 (every background-color / border / button fill).
    GLuint m_whiteTexture = 0;
};

} // namespace Luminumbra::Client