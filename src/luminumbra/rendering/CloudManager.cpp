#include "luminumbra/rendering/CloudManager.h"
#include "luminumbra/core/GLError.h"
#include "luminumbra/core/ResourceManager.h"
#include "stb_image.h"
#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <random> // Use <random> for better random number generation

namespace Luminumbra::Rendering {

// Data and implementation details remain largely the same as they were correct.
// Using a modern random number generator instead of rand().
static std::mt19937 s_CloudRandomEngine(std::random_device{}());

float quadVertices[] = {
    // positions      // texture Coords
    -0.5f, -0.5f, 0.0f, 0.0f, 1.0f,
     0.5f, -0.5f, 0.0f, 1.0f, 1.0f,
     0.5f,  0.5f, 0.0f, 1.0f, 0.0f,

    -0.5f, -0.5f, 0.0f, 0.0f, 1.0f,
     0.5f,  0.5f, 0.0f, 1.0f, 0.0f,
    -0.5f,  0.5f, 0.0f, 0.0f, 0.0f
};

CloudManager::CloudManager() : m_Shader("res/shaders/cloud.vert", "res/shaders/cloud.frag") {}

CloudManager::~CloudManager() {
    glDeleteVertexArrays(1, &m_VAO);
    glDeleteBuffers(1, &m_VBO);
    glDeleteBuffers(1, &m_InstanceVBO);
    glDeleteTextures(1, &m_Texture);
}

void CloudManager::init() {
    // Create VAO and VBO for a quad
    GLCall(glGenVertexArrays(1, &m_VAO));
    GLCall(glGenBuffers(1, &m_VBO));
    GLCall(glGenBuffers(1, &m_InstanceVBO)); // Create instance VBO

    GLCall(glBindVertexArray(m_VAO));

    // Base Quad VBO
    GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_VBO));
    GLCall(glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices, GL_STATIC_DRAW));
    // Location 0: Position attribute
    GLCall(glEnableVertexAttribArray(0));
    GLCall(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0));
    // Location 1: Texture coord attribute
    GLCall(glEnableVertexAttribArray(1));
    GLCall(glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float))));

    // SOLUTION: Instance VBO for model matrices
    GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_InstanceVBO));
    // A mat4 is 4 vec4s. We need to enable 4 vertex attributes.
    for (int i = 0; i < 4; ++i) {
        // Locations 2, 3, 4, 5 for the four vec4s of the mat4
        GLCall(glEnableVertexAttribArray(2 + i));
        GLCall(glVertexAttribPointer(2 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(sizeof(glm::vec4) * i)));
        // Tell OpenGL this is an instanced vertex attribute.
        GLCall(glVertexAttribDivisor(2 + i, 1));
    }

    GLCall(glBindVertexArray(0));

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

    // Create some clouds with better randomness
    std::uniform_real_distribution<float> posDist(-1000.0f, 1000.0f);
    std::uniform_real_distribution<float> scaleXDist(50.0f, 150.0f);
    std::uniform_real_distribution<float> scaleYDist(25.0f, 75.0f);
    std::uniform_real_distribution<float> speedDist(5.0f, 15.0f);

    for (int i = 0; i < 20; ++i) {
        Cloud cloud;
        cloud.position = glm::vec3(posDist(s_CloudRandomEngine), 150.0f, posDist(s_CloudRandomEngine));
        cloud.scale = glm::vec2(scaleXDist(s_CloudRandomEngine), scaleYDist(s_CloudRandomEngine));
        cloud.speed = speedDist(s_CloudRandomEngine);
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
    if (m_Clouds.empty()) return;

    GLCall(glEnable(GL_BLEND));
    GLCall(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    GLCall(glDepthMask(GL_FALSE));

    m_Shader.use();
    m_Shader.setMat4("projection", projection);
    m_Shader.setMat4("view", view);

    // SOLUTION: Collect model matrices for all clouds
    std::vector<glm::mat4> modelMatrices;
    modelMatrices.reserve(m_Clouds.size());
    for (const auto& cloud : m_Clouds) {
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, cloud.position);
        model = glm::scale(model, glm::vec3(cloud.scale.x, cloud.scale.y, 1.0f));
        modelMatrices.push_back(model);
    }

    // SOLUTION: Upload all instance data in one batch
    GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_InstanceVBO));
    GLCall(glBufferData(GL_ARRAY_BUFFER, m_Clouds.size() * sizeof(glm::mat4), modelMatrices.data(), GL_STREAM_DRAW));
    
    GLCall(glActiveTexture(GL_TEXTURE0));
    GLCall(glBindTexture(GL_TEXTURE_2D, m_Texture));
    m_Shader.setInt("texture1", 0);

    // SOLUTION: Draw all clouds in a single instanced draw call
    GLCall(glBindVertexArray(m_VAO));
    GLCall(glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(m_Clouds.size())));
    GLCall(glBindVertexArray(0));

    GLCall(glDepthMask(GL_TRUE));
    GLCall(glDisable(GL_BLEND));
}

} // namespace Luminumbra::Rendering
