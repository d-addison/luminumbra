#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Shader.h"

namespace Luminumbra::Rendering {

class Skybox {
public:
    Skybox();
    ~Skybox();

    void loadCubemap(const std::vector<std::string>& faces);
    void render(const glm::mat4& view, const glm::mat4& projection);

    Shader& getShader() { return m_Shader; }

private:
    unsigned int m_VAO = 0, m_VBO = 0;
    unsigned int m_CubemapTexture = 0;
    Shader m_Shader;
};

} // namespace Luminumbra::Rendering
