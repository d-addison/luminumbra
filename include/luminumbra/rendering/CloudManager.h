#pragma once

#include <vector>
#include <glm/glm.hpp>
#include "Shader.h"

namespace Luminumbra::Rendering {

class CloudManager {
public:
    CloudManager();
    ~CloudManager();

    void init();
    void update(float deltaTime);
    void render(const glm::mat4& view, const glm::mat4& projection);

    Shader& getShader() { return m_Shader; }

private:
    struct Cloud {
        glm::vec3 position;
        glm::vec2 scale;
        float speed;
    };

    std::vector<Cloud> m_Clouds;
    unsigned int m_VAO = 0, m_VBO = 0;
    unsigned int m_Texture = 0;
    Shader m_Shader;
};

} // namespace Luminumbra::Rendering
