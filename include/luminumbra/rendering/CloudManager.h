#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <vector>
#include "luminumbra/rendering/Shader.h"
#include "luminumbra/rendering/Camera.h" 

namespace Luminumbra::Rendering {

class CloudManager {
public:
    CloudManager();
    ~CloudManager();

    void init();
    void update(float deltaTime);
    void render(const Camera& camera);

    Shader& getShader() { return m_Shader; }

private:
    struct Cloud {
        glm::vec3 position;
        glm::vec2 scale;
        float speed;
    };

    std::vector<Cloud> m_Clouds;
    GLuint m_VAO, m_VBO, m_EBO;
    GLuint m_Texture;
    Shader m_Shader;
};

} // namespace Luminumbra::Rendering
