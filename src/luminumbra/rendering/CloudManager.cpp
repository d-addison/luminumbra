#include "luminumbra/rendering/CloudManager.h"
#include "luminumbra/core/GLError.h"
#include "luminumbra/core/ResourceManager.h"
#include "stb_image.h"
#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

namespace Luminumbra::Rendering {

float quadVertices[] = {
    // positions        // texture Coords
    -0.5f, -0.5f, 0.0f,  0.0f, 1.0f,
     0.5f, -0.5f, 0.0f,  1.0f, 1.0f,
     0.5f,  0.5f, 0.0f,  1.0f, 0.0f,

    -0.5f, -0.5f, 0.0f,  0.0f, 1.0f,
     0.5f,  0.5f, 0.0f,  1.0f, 0.0f,
    -0.5f,  0.5f, 0.0f,  0.0f, 0.0f
};

CloudManager::CloudManager() : m_Shader("res/shaders/cloud.vert", "res/shaders/cloud.frag") {
}

CloudManager::~CloudManager() {
    glDeleteVertexArrays(1, &m_VAO);
    glDeleteBuffers(1, &m_VBO);
}

void CloudManager::init() {
    // Create VAO and VBO for a quad
    GLCall(glGenVertexArrays(1, &m_VAO));
    GLCall(glGenBuffers(1, &m_VBO));

    GLCall(glBindVertexArray(m_VAO));
    GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_VBO));
    GLCall(glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices, GL_STATIC_DRAW));

    // Position attribute
    GLCall(glEnableVertexAttribArray(0));
    GLCall(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0));

    // Texture coord attribute
    GLCall(glEnableVertexAttribArray(1));
    GLCall(glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float))));

    // Load cloud texture
    int width, height, nrChannels;
    std::string cloudTexturePath = Core::ResourceManager::getInstance().getResourcePath("src/luminumbra/assets/cloud.png");
    unsigned char *data = stbi_load(cloudTexturePath.c_str(), &width, &height, &nrChannels, 0);
    if (data) {
        GLCall(glGenTextures(1, &m_Texture));
        GLCall(glBindTexture(GL_TEXTURE_2D, m_Texture));
        GLCall(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data));
        GLCall(glGenerateMipmap(GL_TEXTURE_2D));
        stbi_image_free(data);
    } else {
        std::cerr << "Failed to load cloud texture" << std::endl;
    }

    // Create some clouds
    for (int i = 0; i < 20; ++i) {
        Cloud cloud;
        cloud.position = glm::vec3((rand() % 2000) - 1000, 150.0f, (rand() % 2000) - 1000);
        cloud.scale = glm::vec2((rand() % 100) + 50, (rand() % 50) + 25);
        cloud.speed = (rand() % 10) + 5.0f;
        m_Clouds.push_back(cloud);
    }
}

void CloudManager::update(float deltaTime) {
    for (auto& cloud : m_Clouds) {
        cloud.position.x += cloud.speed * deltaTime;
        if (cloud.position.x > 1000) {
            cloud.position.x = -1000;
        }
    }
}

void CloudManager::render(const glm::mat4& view, const glm::mat4& projection) {
    GLCall(glEnable(GL_BLEND));
    GLCall(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    GLCall(glDepthMask(GL_FALSE));

    m_Shader.use();
    m_Shader.setMat4("projection", projection);
    m_Shader.setMat4("view", view);

    GLCall(glBindVertexArray(m_VAO));
    GLCall(glActiveTexture(GL_TEXTURE0));
    GLCall(glBindTexture(GL_TEXTURE_2D, m_Texture));
    m_Shader.setInt("texture1", 0);

    for (const auto& cloud : m_Clouds) {
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, cloud.position);
        model = glm::scale(model, glm::vec3(cloud.scale.x, cloud.scale.y, 1.0f));
        m_Shader.setMat4("model", model);
        GLCall(glDrawArrays(GL_TRIANGLES, 0, 6));
    }

    GLCall(glDepthMask(GL_TRUE));
    GLCall(glDisable(GL_BLEND));
}

} // namespace Luminumbra::Rendering
