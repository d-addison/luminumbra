#include "Shader.h"
#include "core/Log.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace Luminumbra::Rendering {

Shader::Shader(const char* vertexPath, const char* fragmentPath) {
    m_debug_name = std::string(vertexPath) + " | " + fragmentPath;
    std::string vertexCode, fragmentCode;
    std::ifstream vShaderFile, fShaderFile;
    vShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    fShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);

    try {
        vShaderFile.open(vertexPath);
        fShaderFile.open(fragmentPath);
        std::stringstream vShaderStream, fShaderStream;
        vShaderStream << vShaderFile.rdbuf();
        fShaderStream << fShaderFile.rdbuf();
        vertexCode = vShaderStream.str();
        fragmentCode = fShaderStream.str();
    } catch (const std::exception& e) {
        LUMINUMBRA_CORE_ERROR("SHADER IO ERROR ({} / {}): {}", vertexPath, fragmentPath, e.what());
        m_diagnostic = e.what();
        m_id = 0;
        return;
    }

    const char* vShaderCode = vertexCode.c_str();
    const char* fShaderCode = fragmentCode.c_str();
    GLuint vertex = 0, fragment = 0;

    // Compile Vertex Shader
    vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, nullptr);
    glCompileShader(vertex);
    const bool vertex_ok = checkCompileErrors(vertex, "VERTEX");

    // Compile Fragment Shader
    fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    const bool fragment_ok = checkCompileErrors(fragment, "FRAGMENT");

    if (!vertex_ok || !fragment_ok) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        m_id = 0;
        m_valid = false;
        return;
    }

    // Link Program
    m_id = glCreateProgram();
    glAttachShader(m_id, vertex);
    glAttachShader(m_id, fragment);
    glLinkProgram(m_id);
    const bool program_ok = checkCompileErrors(m_id, "PROGRAM");

    // Delete the shaders as they're linked into our program now and no longer necessary
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    if (!program_ok) {
        glDeleteProgram(m_id);
        m_id = 0;
        m_valid = false;
        return;
    }

    m_valid = true;
}

Shader::~Shader() {
    if (m_id) glDeleteProgram(m_id);
}

void Shader::use() const {
    if (m_id) glUseProgram(m_id);
}

// Helper function to get uniform location from cache or query it if not present
GLint Shader::getUniformLocation(const std::string& name) const {
    if (m_uniformLocationCache.find(name) != m_uniformLocationCache.end()) {
        return m_uniformLocationCache[name];
    }

    GLint location = glGetUniformLocation(m_id, name.c_str());
    m_uniformLocationCache[name] = location;
    return location;
}

// --- Uniform Setters ---
// All setters are now modified to use the caching helper function.

void Shader::setBool(const std::string& name, bool value) const {
    glUniform1i(getUniformLocation(name), (int)value);
}

void Shader::setInt(const std::string& name, int value) const {
    glUniform1i(getUniformLocation(name), value);
}

void Shader::setFloat(const std::string& name, float value) const {
    glUniform1f(getUniformLocation(name), value);
}

void Shader::setVec2(const std::string& name, const glm::vec2& value) const {
    glUniform2fv(getUniformLocation(name), 1, &value[0]);
}

void Shader::setVec2(const std::string& name, float x, float y) const {
    glUniform2f(getUniformLocation(name), x, y);
}

void Shader::setVec3(const std::string& name, const glm::vec3& value) const {
    glUniform3fv(getUniformLocation(name), 1, &value[0]);
}

void Shader::setVec3(const std::string& name, float x, float y, float z) const {
    glUniform3f(getUniformLocation(name), x, y, z);
}

void Shader::setVec4(const std::string& name, const glm::vec4& value) const {
    glUniform4fv(getUniformLocation(name), 1, &value[0]);
}

void Shader::setVec4(const std::string& name, float x, float y, float z, float w) const {
    glUniform4f(getUniformLocation(name), x, y, z, w);
}

void Shader::setMat2(const std::string& name, const glm::mat2& mat) const {
    glUniformMatrix2fv(getUniformLocation(name), 1, GL_FALSE, &mat[0][0]);
}

void Shader::setMat3(const std::string& name, const glm::mat3& mat) const {
    glUniformMatrix3fv(getUniformLocation(name), 1, GL_FALSE, &mat[0][0]);
}

void Shader::setMat4(const std::string& name, const glm::mat4& mat) const {
    glUniformMatrix4fv(getUniformLocation(name), 1, GL_FALSE, &mat[0][0]);
}

// checkCompileErrors implementation remains the same
bool Shader::checkCompileErrors(GLuint shader, const std::string& type) {
    GLint success;
    GLint log_length = 0;
    if (type != "PROGRAM") {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
            std::vector<GLchar> info_log(static_cast<size_t>(std::max(log_length, 1)));
            glGetShaderInfoLog(shader, static_cast<GLsizei>(info_log.size()), NULL, info_log.data());
            m_diagnostic = std::string(info_log.data());
            LUMINUMBRA_CORE_ERROR("SHADER_COMPILATION_ERROR of type: {0} ({1})\n{2}", type, m_debug_name, m_diagnostic);
            return false;
        }
    } else {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramiv(shader, GL_INFO_LOG_LENGTH, &log_length);
            std::vector<GLchar> info_log(static_cast<size_t>(std::max(log_length, 1)));
            glGetProgramInfoLog(shader, static_cast<GLsizei>(info_log.size()), NULL, info_log.data());
            m_diagnostic = std::string(info_log.data());
            LUMINUMBRA_CORE_ERROR("PROGRAM_LINKING_ERROR of type: {0} ({1})\n{2}", type, m_debug_name, m_diagnostic);
            return false;
        }
    }
    return true;
}

} // namespace Luminumbra::Rendering
