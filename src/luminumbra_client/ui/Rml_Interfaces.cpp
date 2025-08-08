#include "Rml_Interfaces.h"
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Platform.h>
#include <SOIL2/SOIL2.h>
#include <RmlUi/Core/FileInterface.h>

namespace Luminumbra::Client {

// --- RmlFileInterface ---

RmlFileInterface::RmlFileInterface(const std::string& root_path) : m_root(root_path) {}

Rml::FileHandle RmlFileInterface::Open(const Rml::String& path) {
    // RmlUi may provide paths that are relative to the document, absolute, or relative to the project root.
    // This implementation will form an absolute path to the asset, which is the most robust solution.
    Rml::String full_path;

    // If the path is already absolute, use it as is.
    if (path.find(':') != Rml::String::npos || path[0] == '/' || path[0] == '\\') {
        full_path = path;
    }
    // Otherwise, it's a relative path. Prepend the root path of our game.
    else {
        full_path = m_root + path;
    }

    // Finally, attempt to open the file.
    FILE* fp = fopen(full_path.c_str(), "rb");
    if (!fp) {
        // It's helpful to log when a file can't be opened.
        Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to open file: %s", full_path.c_str());
    }
    return (Rml::FileHandle)fp;
}

void RmlFileInterface::Close(Rml::FileHandle file) {
    fclose((FILE*)file);
}

size_t RmlFileInterface::Read(void* buffer, size_t size, Rml::FileHandle file) {
    return fread(buffer, 1, size, (FILE*)file);
}

bool RmlFileInterface::Seek(Rml::FileHandle file, long offset, int origin) {
    return fseek((FILE*)file, offset, origin) == 0;
}

size_t RmlFileInterface::Tell(Rml::FileHandle file) {
    return ftell((FILE*)file);
}


// --- RmlRenderer ---

namespace {
    const char* vertex_shader = R"(
        #version 330 core
        in vec2 inPosition;
        in vec4 inColor;
        in vec2 inTexCoord;

        out vec4 fragColor;
        out vec2 fragTexCoord;

        uniform mat4 projection;
        uniform mat4 translation;

        void main() {
            fragColor = inColor;
            fragTexCoord = inTexCoord;
            gl_Position = projection * translation * vec4(inPosition, 0.0, 1.0);
        }
    )";

    const char* fragment_shader = R"(
        #version 330 core
        in vec4 fragColor;
        in vec2 fragTexCoord;

        out vec4 outColor;

        uniform sampler2D uTexture;

        void main() {
            outColor = texture(uTexture, fragTexCoord) * fragColor;
        }
    )";
}

RmlRenderer::RmlRenderer() {
    // Create shader program
    unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertex_shader, nullptr);
    glCompileShader(vs);

    // Check for shader compile errors
    int success;
    char infoLog[512];
    glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vs, 512, nullptr, infoLog);
        Rml::Log::Message(Rml::Log::LT_ERROR, "Vertex shader compilation failed: %s", infoLog);
    }

    unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragment_shader, nullptr);
    glCompileShader(fs);

    // Check for shader compile errors
    glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fs, 512, nullptr, infoLog);
        Rml::Log::Message(Rml::Log::LT_ERROR, "Fragment shader compilation failed: %s", infoLog);
    }

    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);

    // Check for linking errors
    glGetProgramiv(m_program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(m_program, 512, nullptr, infoLog);
        Rml::Log::Message(Rml::Log::LT_ERROR, "Shader program linking failed: %s", infoLog);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    m_translation_loc = glGetUniformLocation(m_program, "translation");
    m_projection_loc = glGetUniformLocation(m_program, "projection");
}

void RmlRenderer::SetViewport(int width, int height) {
    m_width = width;
    m_height = height;
}

Rml::CompiledGeometryHandle RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    auto geometry = new CompiledGeometry();
    
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->vbo);
    glGenBuffers(1, &geometry->ebo);

    glBindVertexArray(geometry->vao);

    glBindBuffer(GL_ARRAY_BUFFER, geometry->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Rml::Vertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(int) * indices.size(), indices.data(), GL_STATIC_DRAW);

    geometry->num_indices = (int)indices.size();

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Rml::Vertex), (void*)offsetof(Rml::Vertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Rml::Vertex), (void*)offsetof(Rml::Vertex, colour));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Rml::Vertex), (void*)offsetof(Rml::Vertex, tex_coord));

    glBindVertexArray(0);

    return (Rml::CompiledGeometryHandle)geometry;
}

void RmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) {
    CompiledGeometry* geometry = (CompiledGeometry*)handle;

    glUseProgram(m_program);
    glBindVertexArray(geometry->vao);

    Rml::Matrix4f proj = Rml::Matrix4f::ProjectOrtho(0, (float)m_width, (float)m_height, 0, -1, 1);
    Rml::Matrix4f trans = Rml::Matrix4f::Translate(translation.x, translation.y, 0);

    glUniformMatrix4fv(m_projection_loc, 1, GL_FALSE, proj.data());
    glUniformMatrix4fv(m_translation_loc, 1, GL_FALSE, trans.data());

    glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
    glDrawElements(GL_TRIANGLES, geometry->num_indices, GL_UNSIGNED_INT, nullptr);
}

void RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
    CompiledGeometry* geometry = (CompiledGeometry*)handle;
    glDeleteVertexArrays(1, &geometry->vao);
    glDeleteBuffers(1, &geometry->vbo);
    glDeleteBuffers(1, &geometry->ebo);
    delete geometry;
}

void RmlRenderer::EnableScissorRegion(bool enable) {
    if (enable)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
}

void RmlRenderer::SetScissorRegion(Rml::Rectanglei region) {
    glScissor(region.p0.x, m_height - (region.p0.y + region.Height()), region.Width(), region.Height());
}

Rml::TextureHandle RmlRenderer::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) {
    // Load directly from file path using SOIL2.
    GLuint texture_id = SOIL_load_OGL_texture(
        source.c_str(),
        SOIL_LOAD_AUTO, SOIL_CREATE_NEW_ID,
        SOIL_FLAG_MIPMAPS | SOIL_FLAG_INVERT_Y | SOIL_FLAG_NTSC_SAFE_RGB | SOIL_FLAG_COMPRESS_TO_DXT
    );

    if (texture_id == 0) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "SOIL2 failed to load texture: %s", source.c_str());
        return 0;
    }

    // Optionally query width/height (not strictly needed by Rml)
    // GLint w = 0, h = 0;
    // glBindTexture(GL_TEXTURE_2D, texture_id);
    // glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    // glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    // texture_dimensions = { w, h };
    texture_dimensions = { 0, 0 };

    return (Rml::TextureHandle)texture_id;
}

Rml::TextureHandle RmlRenderer::GenerateTexture(Rml::Span<const unsigned char> source, Rml::Vector2i source_dimensions) {
    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    if (tex_id == 0) return 0;

    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, source_dimensions.x, source_dimensions.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, source.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return (Rml::TextureHandle)tex_id;
}

void RmlRenderer::ReleaseTexture(Rml::TextureHandle texture_handle) {
    glDeleteTextures(1, (GLuint*)&texture_handle);
}

} // namespace Luminumbra::Client
