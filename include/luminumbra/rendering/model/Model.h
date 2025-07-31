#pragma once

#include "luminumbra/rendering/Shader.h"
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
};

struct Texture {
    GLuint id;
    std::string type;
    std::string path;
};

class Mesh {
public:
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<Texture> textures;

    Mesh(std::vector<Vertex> vertices, std::vector<unsigned int> indices, std::vector<Texture> textures);
    void setupInstancing(GLuint instanceVBO);
    void drawInstanced(Shader& shader, GLsizei instanceCount);

private:
    GLuint m_VAO, m_VBO, m_EBO;
    void setupMesh();
};

class Model {
public:
    explicit Model(const std::string& path);
    void setupInstancing(GLuint instanceVBO);
    void drawInstanced(Shader& shader, GLsizei instanceCount);

private:
    std::vector<Mesh> m_Meshes;
    std::vector<Texture> m_TexturesLoaded;
    std::string m_Directory;

    void loadModel(const std::string& path);
};

} // namespace Luminumbra::Rendering