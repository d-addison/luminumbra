#include "luminumbra/rendering/Skybox.h"
#include "luminumbra/core/GLError.h"
#include "luminumbra/core/ResourceManager.h"
#include "stb_image.h"
#include <glad/gl.h>
#include <iostream>

namespace Luminumbra::Rendering {

// Cube vertices for the skybox
float skyboxVertices[] = {
    // positions          
    -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,
     1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,

    -1.0f, -1.0f,  1.0f,
    -1.0f, -1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f,  1.0f,
    -1.0f, -1.0f,  1.0f,

     1.0f, -1.0f, -1.0f,
     1.0f, -1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
     1.0f,  1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,

    -1.0f, -1.0f,  1.0f,
    -1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
     1.0f, -1.0f,  1.0f,
    -1.0f, -1.0f,  1.0f,

    -1.0f,  1.0f, -1.0f,
     1.0f,  1.0f, -1.0f,
     1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
    -1.0f,  1.0f,  1.0f,
    -1.0f,  1.0f, -1.0f,

    -1.0f, -1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,
     1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,
     1.0f, -1.0f,  1.0f
};

Skybox::Skybox() : m_Shader("res/shaders/skybox.vert", "res/shaders/skybox.frag") {
    GLCall(glGenVertexArrays(1, &m_VAO));
    GLCall(glGenBuffers(1, &m_VBO));

    GLCall(glBindVertexArray(m_VAO));
    GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_VBO));
    GLCall(glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW));

    GLCall(glEnableVertexAttribArray(0));
    GLCall(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0));

    GLCall(glBindVertexArray(0));
}

Skybox::~Skybox() {
    glDeleteVertexArrays(1, &m_VAO);
    glDeleteBuffers(1, &m_VBO);
    glDeleteTextures(1, &m_CubemapTexture);
}

void Skybox::loadCubemap(const std::vector<std::string>& faces) {
    GLCall(glGenTextures(1, &m_CubemapTexture));
    GLCall(glBindTexture(GL_TEXTURE_CUBE_MAP, m_CubemapTexture));

    int width, height, nrChannels;
    for (unsigned int i = 0; i < faces.size(); i++) {
        std::string fullPath = Core::ResourceManager::getInstance().getResourcePath(faces[i]);
        unsigned char *data = stbi_load(fullPath.c_str(), &width, &height, &nrChannels, 0);
        if (data) {
            GLCall(glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data));
            stbi_image_free(data);
        } else {
            std::cerr << "Cubemap texture failed to load at path: " << faces[i] << std::endl;
            stbi_image_free(data);
        }
    }

    GLCall(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GLCall(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GLCall(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GLCall(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GLCall(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE));
}

void Skybox::render(const glm::mat4& view, const glm::mat4& projection) {
    // Change depth function so depth test passes when values are equal to depth buffer's content
    GLCall(glDepthFunc(GL_LEQUAL)); 
    
    m_Shader.use();
    // The skybox.vert shader now handles removing the translation from the view matrix.
    m_Shader.setMat4("u_view", view);
    m_Shader.setMat4("u_projection", projection);

    GLCall(glBindVertexArray(m_VAO));
    GLCall(glActiveTexture(GL_TEXTURE0));
    GLCall(glBindTexture(GL_TEXTURE_CUBE_MAP, m_CubemapTexture));
    m_Shader.setInt("u_skybox", 0);

    GLCall(glDrawArrays(GL_TRIANGLES, 0, 36));
    GLCall(glBindVertexArray(0));
    
    // Set depth function back to default
    GLCall(glDepthFunc(GL_LESS));
}

} // namespace Luminumbra::Rendering
