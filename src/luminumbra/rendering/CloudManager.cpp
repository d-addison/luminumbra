#include "luminumbra/rendering/CloudManager.h"
#include "luminumbra/core/GLError.h"
#include "luminumbra/core/ResourceManager.h"
#include "luminumbra/rendering/Camera.h" // Added for billboarding
#include "stb_image.h"
#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

namespace Luminumbra::Rendering {

// Vertices for a simple quad
float quadVertices[] = {
    // positions      // texCoords
    -0.5f, -0.5f, 0.0f, 0.0f, 0.0f,
     0.5f, -0.5f, 0.0f, 1.0f, 0.0f,
     0.5f,  0.5f, 0.0f, 1.0f, 1.0f,
    -0.5f,  0.5f, 0.0f, 0.0f, 1.0f
};
unsigned int quadIndices[] = {
    0, 1, 2,
    2, 3, 0
};

// Renamed shader files to be more specific
CloudManager::CloudManager() : m_Shader("res/shaders/cloud.vert", "res/shaders/cloud.frag") {}

CloudManager::~CloudManager() {
    glDeleteVertexArrays(1, &m_VAO);
    glDeleteBuffers(1, &m_VBO);
    glDeleteBuffers(1, &m_EBO);
}

void CloudManager::init() {
    GLCall(glGenVertexArrays(1, &m_VAO));
    GLCall(glGenBuffers(1, &m_VBO));
    GLCall(glGenBuffers(1, &m_EBO));

    GLCall(glBindVertexArray(m_VAO));
    GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_VBO));
    GLCall(glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices, GL_STATIC_DRAW));
    
    GLCall(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO));
    GLCall(glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quadIndices), quadIndices, GL_STATIC_DRAW));

    // a_Position
    GLCall(glEnableVertexAttribArray(0));
    GLCall(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0));

    // a_TexCoords
    GLCall(glEnableVertexAttribArray(1));
    GLCall(glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float))));
    
    glBindVertexArray(0);

    // Load cloud texture
    std::string cloudTexturePath = Core::ResourceManager::getInstance().getResourcePath("src/luminumbra/assets/cloud.png");
    int width, height, nrChannels;
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
        cloud.scale = glm::vec2((rand() % 200) + 100, (rand() % 100) + 50);
        cloud.speed = (rand() % 10) + 5.0f;
        m_Clouds.push_back(cloud);
    }
}

void CloudManager::update(float deltaTime) {
    for (auto& cloud : m_Clouds) {
        cloud.position.x += cloud.speed * deltaTime;
        if (cloud.position.x > 1500) { // Increased wrap distance
            cloud.position.x = -1500;
        }
    }
}

void CloudManager::render(const Camera& camera) {
    GLCall(glEnable(GL_BLEND));
    GLCall(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    GLCall(glDepthMask(GL_FALSE));

    m_Shader.use();
    m_Shader.setMat4("u_projection", camera.getProjectionMatrix());
    m_Shader.setMat4("u_view", camera.getViewMatrix());

    GLCall(glBindVertexArray(m_VAO));
    GLCall(glActiveTexture(GL_TEXTURE0));
    GLCall(glBindTexture(GL_TEXTURE_2D, m_Texture));
    m_Shader.setInt("u_albedoTexture", 0);

    for (const auto& cloud : m_Clouds) {
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, cloud.position);

        // Billboarding: Make the cloud quad always face the camera
        model[0][0] = camera.getViewMatrix()[0][0];
        model[0][1] = camera.getViewMatrix()[1][0];
        model[0][2] = camera.getViewMatrix()[2][0];
        model[1][0] = camera.getViewMatrix()[0][1];
        model[1][1] = camera.getViewMatrix()[1][1];
        model[1][2] = camera.getViewMatrix()[2][1];
        model[2][0] = camera.getViewMatrix()[0][2];
        model[2][1] = camera.getViewMatrix()[1][2];
        model[2][2] = camera.getViewMatrix()[2][2];
        
        model = glm::scale(model, glm::vec3(cloud.scale.x, cloud.scale.y, 1.0f));
        m_Shader.setMat4("u_model", model);
        GLCall(glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0));
    }

    GLCall(glDepthMask(GL_TRUE));
    GLCall(glDisable(GL_BLEND));
}

} // namespace Luminumbra::Rendering