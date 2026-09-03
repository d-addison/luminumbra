#include "Rml_Interfaces.h"
#include <GLFW/glfw3.h> // For glfwGetTime
#include <SOIL2/SOIL2.h>
#include <cstdio> // For FILE operations

// Note: The original file was missing some includes which I've added.

namespace Luminumbra::Client {

// --- RmlSystem Implementation ---
double RmlSystem::GetElapsedTime() {
    return glfwGetTime();
}

// Fixed: The base class virtual function returns a bool.
bool RmlSystem::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    switch (type) {
        case Rml::Log::LT_ALWAYS:
        case Rml::Log::LT_INFO:
            LUMINUMBRA_CORE_INFO("[RmlUi] {}", message);
            break;
        case Rml::Log::LT_DEBUG:
            LUMINUMBRA_CORE_TRACE("[RmlUi] {}", message);
            break;
        case Rml::Log::LT_WARNING:
            LUMINUMBRA_CORE_WARN("[RmlUi] {}", message);
            break;
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            LUMINUMBRA_CORE_ERROR("[RmlUi] {}", message);
            break;
        default:
            break;
    }
    return true; // Return true to indicate the message was handled.
}

// --- RmlFileInterface Implementation ---
RmlFileInterface::RmlFileInterface(const std::string& root_path)
    : m_root(root_path) {}

Rml::FileHandle RmlFileInterface::Open(const Rml::String& path) {
    std::string full_path = m_root + path;
    FILE* fp = fopen(full_path.c_str(), "rb");
    if (!fp) {
        LUMINUMBRA_CORE_ERROR("[RmlUi] Failed to open file: {}", full_path);
    }
    return (Rml::FileHandle)fp;
}

void RmlFileInterface::Close(Rml::FileHandle file) {
    if (file)
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

// --- RmlRenderer Implementation ---
namespace {
const char* vertex_shader_gl = R"(
        #version 330 core
        uniform mat4 projection; 
        uniform mat4 translation;

        in vec2 inPosition; 
        in vec4 inColor; 
        in vec2 inTexCoord;
        
        out vec4 fragColor; 
        out vec2 fragTexCoord;
        
        void main() {
            fragColor = inColor;
            fragTexCoord = inTexCoord;
            gl_Position = projection * translation * vec4(inPosition, 0.0, 1.0);
        }
    )";
const char* fragment_shader_gl = R"(
        #version 330 core
        uniform sampler2D uTexture;

        in vec4 fragColor; 
        in vec2 fragTexCoord;
        
        out vec4 outColor;
        
        void main() { 
            outColor = texture(uTexture, fragTexCoord) * fragColor; 
        }
    )";
} // namespace

RmlRenderer::RmlRenderer() {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertex_shader_gl, nullptr);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragment_shader_gl, nullptr);
    glCompileShader(fs);

    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glBindAttribLocation(m_program, 0, "inPosition");
    glBindAttribLocation(m_program, 1, "inColor");
    glBindAttribLocation(m_program, 2, "inTexCoord");
    glLinkProgram(m_program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    m_translation_loc = glGetUniformLocation(m_program, "translation");
    m_projection_loc = glGetUniformLocation(m_program, "projection");

    // 1x1 opaque-white fallback texture for untextured (solid-colour) geometry. RmlUi passes
    // texture handle 0 for every background-color / border / button fill; binding white makes
    // `texture(uTexture,uv) * fragColor` resolve to the fill colour instead of sampling an
    // unbound texture (black), which previously painted the entire UI black-on-black.
    const unsigned char white_px[4] = {255, 255, 255, 255};
    glGenTextures(1, &m_whiteTexture);
    glBindTexture(GL_TEXTURE_2D, m_whiteTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white_px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void RmlRenderer::SetViewport(int width, int height) {
    m_width = width;
    m_height = height;
}

Rml::CompiledGeometryHandle RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                         Rml::Span<const int> indices) {
    auto geometry = new CompiledGeometry();
    glGenVertexArrays(1, &geometry->vao);
    glBindVertexArray(geometry->vao);
    glGenBuffers(1, &geometry->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, geometry->vbo);
    glBufferData(
        GL_ARRAY_BUFFER, sizeof(Rml::Vertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &geometry->ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER, sizeof(int) * indices.size(), indices.data(), GL_STATIC_DRAW);

    geometry->num_indices = (int)indices.size();

    glEnableVertexAttribArray(0); // inPosition
    glVertexAttribPointer(
        0, 2, GL_FLOAT, GL_FALSE, sizeof(Rml::Vertex), (void*)offsetof(Rml::Vertex, position));
    glEnableVertexAttribArray(1); // inColor
    glVertexAttribPointer(
        1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Rml::Vertex), (void*)offsetof(Rml::Vertex, colour));
    glEnableVertexAttribArray(2); // inTexCoord
    glVertexAttribPointer(
        2, 2, GL_FLOAT, GL_FALSE, sizeof(Rml::Vertex), (void*)offsetof(Rml::Vertex, tex_coord));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    return (Rml::CompiledGeometryHandle)geometry;
}

void RmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle,
                                 Rml::Vector2f translation,
                                 Rml::TextureHandle texture) {
    CompiledGeometry* geometry = (CompiledGeometry*)handle;
    glUseProgram(m_program);

    Rml::Matrix4f proj =
        Rml::Matrix4f::ProjectOrtho(0.0f, (float)m_width, (float)m_height, 0.0f, -1.0f, 1.0f);
    Rml::Matrix4f trans = Rml::Matrix4f::Translate(translation.x, translation.y, 0.0f);

    glUniformMatrix4fv(m_projection_loc, 1, false, proj.data());
    glUniformMatrix4fv(m_translation_loc, 1, false, trans.data());

    // Untextured solid-colour geometry arrives with handle 0 → bind the 1x1 white fallback so
    // the fill colour survives the texture multiply (otherwise every background renders black).
    glBindTexture(GL_TEXTURE_2D, texture ? (GLuint)texture : m_whiteTexture);
    glBindVertexArray(geometry->vao);
    glDrawElements(GL_TRIANGLES, geometry->num_indices, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
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
    glScissor(
        region.p0.x, m_height - (region.p0.y + region.Height()), region.Width(), region.Height());
}

Rml::TextureHandle RmlRenderer::LoadTexture(Rml::Vector2i& texture_dimensions,
                                            const Rml::String& source) {
    // The path needs to be relative to the executable or an absolute path.
    // The RmlFileInterface prepends the root path, so we use `source` directly.
    std::string full_path = "assets/" + source; // Example, adjust if needed

    // OLD LINE WITH TYPO:
    // GLuint texture_id = SOIL_load_OGL_texture(full_path.c_str, SOIL_LOAD_AUTO,
    // SOIL_CREATE_NEW_ID, SOIL_FLAG_INVERT_Y | SOIL_FLAG_NTSC_RGB | SOIL_FLAG_COMPRESS_TO_DXT);

    // CORRECTED LINE:
    GLuint texture_id = SOIL_load_OGL_texture(full_path.c_str(),
                                              SOIL_LOAD_AUTO,
                                              SOIL_CREATE_NEW_ID,
                                              SOIL_FLAG_INVERT_Y | SOIL_FLAG_NTSC_SAFE_RGB |
                                                  SOIL_FLAG_COMPRESS_TO_DXT);

    if (texture_id == 0) {
        LUMINUMBRA_CORE_ERROR("[RmlUi] SOIL2 failed to load texture: {}", full_path);
        return 0;
    }

    // ... rest of the function is fine
    GLint w = 0, h = 0;
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    texture_dimensions = {w, h};

    return (Rml::TextureHandle)texture_id;
}

Rml::TextureHandle RmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source_data,
                                                Rml::Vector2i source_dimensions) {
    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    if (tex_id == 0)
        return 0;

    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA8,
                 source_dimensions.x,
                 source_dimensions.y,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 source_data.data());

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