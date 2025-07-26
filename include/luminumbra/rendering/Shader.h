// include/luminumbra/rendering/Shader.h
#pragma once

#include <glm/glm.hpp>
#include <string>

namespace Luminumbra::Rendering {

class Shader {
public:
    // Constructor reads and builds the shader from two files
    Shader(const char* vertexPath, const char* fragmentPath);
    ~Shader();

    // Use/activate the shader
    void use() const;

    // Utility uniform functions (we'll need these later)
    void setMat4(const std::string &name, const glm::mat4 &mat) const;
    void setVec3(const std::string &name, const glm::vec3 &value) const;
    // void setBool(const std::string &name, bool value) const;
    // void setInt(const std::string &name, int value) const;
    // void setFloat(const std::string &name, float value) const;

    unsigned int getID() const { return m_ID; }

private:
    void checkCompileErrors(unsigned int shader, const std::string& type);

    unsigned int m_ID; // The shader program ID
};

} // namespace Luminumbra::Rendering