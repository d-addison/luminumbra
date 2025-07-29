#pragma once

#include <string>
#include <glm/glm.hpp>
#include <unordered_map> // Added for caching
#include <glad/gl.h>     // Added for GLint

namespace Luminumbra::Rendering {

class Shader {
public:
    Shader(const char* vertexPath, const char* fragmentPath);
    ~Shader();

    void use() const;

    // Setters for uniforms
    void setBool(const std::string &name, bool value) const;
    void setInt(const std::string &name, int value) const;
    void setFloat(const std::string &name, float value) const;
    void setVec2(const std::string &name, const glm::vec2 &value) const;
    void setVec3(const std::string &name, const glm::vec3 &value) const;
    void setVec4(const std::string &name, const glm::vec4 &value) const;
    void setMat4(const std::string &name, const glm::mat4 &mat) const;

    // Getters for debugging
    unsigned int getID() const { return m_ID; }
    const std::string& getVertexPath() const { return m_VertexPath; }
    const std::string& getFragmentPath() const { return m_FragmentPath; }

private:
    unsigned int m_ID;
    std::string m_VertexPath;
    std::string m_FragmentPath;

    // SOLUTION: Add a cache for uniform locations.
    // 'mutable' allows it to be modified in const methods.
    mutable std::unordered_map<std::string, GLint> m_UniformLocationCache;
    
    // Helper to get location from cache or query OpenGL
    GLint getUniformLocation(const std::string& name) const;
    
    void checkCompileErrors(unsigned int shader, const std::string& type);
};

} // namespace Luminumbra::Rendering