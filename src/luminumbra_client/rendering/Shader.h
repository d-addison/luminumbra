#pragma once

#include <string>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <unordered_map> // Added for the cache

namespace Luminumbra::Rendering {

class Shader {
public:
    Shader(const char* vertexPath, const char* fragmentPath);
    ~Shader();

    void use() const;
    bool IsValid() const { return m_valid && m_id != 0; }
    GLuint Id() const { return m_id; }
    const std::string& DebugName() const { return m_debug_name; }
    const std::string& Diagnostic() const { return m_diagnostic; }

    void setBool(const std::string& name, bool value) const;
    void setInt(const std::string& name, int value) const;
    void setFloat(const std::string& name, float value) const;
    void setVec2(const std::string& name, const glm::vec2& value) const;
    void setVec2(const std::string& name, float x, float y) const;
    void setVec3(const std::string& name, const glm::vec3& value) const;
    void setVec3(const std::string& name, float x, float y, float z) const;
    void setVec4(const std::string& name, const glm::vec4& value) const;
    void setVec4(const std::string& name, float x, float y, float z, float w) const;
    void setMat2(const std::string& name, const glm::mat2& mat) const;
    void setMat3(const std::string& name, const glm::mat3& mat) const;
    void setMat4(const std::string& name, const glm::mat4& mat) const;

private:
    bool checkCompileErrors(GLuint shader, const std::string& type);
    GLint getUniformLocation(const std::string& name) const; // Helper to use the cache

    GLuint m_id = 0;
    bool m_valid = false;
    std::string m_debug_name;
    std::string m_diagnostic;
    // The cache for uniform locations. mutable allows it to be modified in const functions.
    mutable std::unordered_map<std::string, GLint> m_uniformLocationCache;
};

} // namespace Luminumbra::Rendering
