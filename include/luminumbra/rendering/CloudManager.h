#pragma once

#include <vector>
#include <glm/glm.hpp>
#include "Shader.h"

namespace Luminumbra::Rendering {

struct Cloud {
    glm::vec3 position;
    glm::vec2 scale;
    float speed;
};
class CloudManager {
public:
    CloudManager();
    ~CloudManager();

    void init();
    void update(float deltaTime);
    void render(const glm::mat4& view, const glm::mat4& projection);

    Shader& getShader() { return m_Shader; }

private:
    std::vector<Cloud> m_Clouds;
    Shader m_Shader;
    GLuint m_VAO, m_VBO, m_Texture;
    GLuint m_InstanceVBO;
};

} // namespace Luminumbra::Rendering
