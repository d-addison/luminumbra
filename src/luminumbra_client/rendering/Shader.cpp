#include "Shader.h"
#include "core/Log.h"
#include <fstream>
#include <sstream>

namespace Luminumbra::Rendering {

Shader::Shader(const char* vertexPath, const char* fragmentPath) {
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
        m_id = 0;
        return;
    }

    const char* vShaderCode = vertexCode.c_str();
    const char* fShaderCode = fragmentCode.c_str();

    GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, nullptr);
    glCompileShader(vertex);
    {
        GLint ok = GL_FALSE;
        glGetShaderiv(vertex, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char infoLog[1024];
            glGetShaderInfoLog(vertex, 1024, nullptr, infoLog);
            LUMINUMBRA_CORE_ERROR("VERTEX SHADER COMPILE ERROR ({}): {}", vertexPath, infoLog);
            glDeleteShader(vertex);
            m_id = 0;
            return;
        }
    }

    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    {
        GLint ok = GL_FALSE;
        glGetShaderiv(fragment, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char infoLog[1024];
            glGetShaderInfoLog(fragment, 1024, nullptr, infoLog);
            LUMINUMBRA_CORE_ERROR("FRAGMENT SHADER COMPILE ERROR ({}): {}", fragmentPath, infoLog);
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            m_id = 0;
            return;
        }
    }

    m_id = glCreateProgram();
    glAttachShader(m_id, vertex);
    glAttachShader(m_id, fragment);
    glLinkProgram(m_id);

    {
        GLint ok = GL_FALSE;
        glGetProgramiv(m_id, GL_LINK_STATUS, &ok);
        if (!ok) {
            char infoLog[1024];
            glGetProgramInfoLog(m_id, 1024, nullptr, infoLog);
            LUMINUMBRA_CORE_ERROR("PROGRAM LINK ERROR ({} , {}): {}", vertexPath, fragmentPath, infoLog);
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            glDeleteProgram(m_id);
            m_id = 0;
            return;
        }
    }

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

Shader::~Shader() {
    if (m_id) glDeleteProgram(m_id);
}

void Shader::use() const {
    if (m_id) glUseProgram(m_id);
}

void Shader::setBool(const std::string& name, bool value) const {
    glUniform1i(glGetUniformLocation(m_id, name.c_str()), (int)value);
}

void Shader::setInt(const std::string& name, int value) const {
    glUniform1i(glGetUniformLocation(m_id, name.c_str()), value);
}

void Shader::setFloat(const std::string& name, float value) const {
    glUniform1f(glGetUniformLocation(m_id, name.c_str()), value);
}

void Shader::setVec2(const std::string& name, const glm::vec2& value) const {
    glUniform2fv(glGetUniformLocation(m_id, name.c_str()), 1, &value[0]);
}

void Shader::setVec2(const std::string& name, float x, float y) const {
    glUniform2f(glGetUniformLocation(m_id, name.c_str()), x, y);
}

void Shader::setVec3(const std::string& name, const glm::vec3& value) const {
    glUniform3fv(glGetUniformLocation(m_id, name.c_str()), 1, &value[0]);
}

void Shader::setVec3(const std::string& name, float x, float y, float z) const {
    glUniform3f(glGetUniformLocation(m_id, name.c_str()), x, y, z);
}

void Shader::setVec4(const std::string& name, const glm::vec4& value) const {
    glUniform4fv(glGetUniformLocation(m_id, name.c_str()), 1, &value[0]);
}

void Shader::setVec4(const std::string& name, float x, float y, float z, float w) const {
    glUniform4f(glGetUniformLocation(m_id, name.c_str()), x, y, z, w);
}

void Shader::setMat2(const std::string& name, const glm::mat2& mat) const {
    glUniformMatrix2fv(glGetUniformLocation(m_id, name.c_str()), 1, GL_FALSE, &mat[0][0]);
}

void Shader::setMat3(const std::string& name, const glm::mat3& mat) const {
    glUniformMatrix3fv(glGetUniformLocation(m_id, name.c_str()), 1, GL_FALSE, &mat[0][0]);
}

void Shader::setMat4(const std::string& name, const glm::mat4& mat) const {
    glUniformMatrix4fv(glGetUniformLocation(m_id, name.c_str()), 1, GL_FALSE, &mat[0][0]);
}

void Shader::checkCompileErrors(GLuint shader, std::string type) {
    GLint success;
    GLchar infoLog[1024];
    if (type != "PROGRAM") {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(shader, 1024, NULL, infoLog);
            LUMINUMBRA_CORE_ERROR("SHADER_COMPILATION_ERROR of type: {0}\n{1}", type, infoLog);
        }
    }
    else {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shader, 1024, NULL, infoLog);
            LUMINUMBRA_CORE_ERROR("PROGRAM_LINKING_ERROR of type: {0}\n{1}", type, infoLog);
        }
    }
}

} // namespace Luminumbra::Rendering
