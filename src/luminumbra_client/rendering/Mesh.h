#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <memory>

namespace Luminumbra::Rendering {

struct Vertex {
    glm::vec3 pos;
    glm::vec3 norm;
    glm::vec2 uv;
};

struct Mesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    uint32_t indexCount = 0;
    glm::vec4 boundingSphere; // x, y, z, radius

    ~Mesh() {
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (ebo) glDeleteBuffers(1, &ebo);
    }
};

class MeshLoader {
public:
    static std::unique_ptr<Mesh> Load(const std::string& path);
};

}