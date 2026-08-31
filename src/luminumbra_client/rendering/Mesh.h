#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

struct Vertex {
    glm::vec3 pos;
    glm::vec3 norm;
    glm::vec2 uv;
};

//.lmesh v2 (LMS2) vertex layout for skinned meshes. Guarded as a separate
// struct so the v1 (LMSH) load path above stays untouched: v1 files keep the
// 32-byte Vertex layout, LMS2 files carry u8x4 joints + u8x4 weights.
struct SkinnedVertex {
    glm::vec3 pos;
    glm::vec3 norm;
    glm::vec2 uv;
    uint8_t joints[4];
    uint8_t weights[4];
};
static_assert(sizeof(SkinnedVertex) == 40, "LMS2 vertex layout is 40 bytes");

struct Mesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    uint32_t indexCount = 0;
    // Non-zero only for skinned (LMS2) meshes: the joint palette uploaded to
    // the skinning SSBO must carry exactly this many mat4s.
    uint32_t jointCount = 0;
    glm::vec4 boundingSphere; // x, y, z, radius

    ~Mesh() {
        if (vao)
            glDeleteVertexArrays(1, &vao);
        if (vbo)
            glDeleteBuffers(1, &vbo);
        if (ebo)
            glDeleteBuffers(1, &ebo);
    }
};

class MeshLoader {
public:
    static std::unique_ptr<Mesh> Load(const std::string& path);
    // Build a Mesh from IN-MEMORY arrays (no disk I/O) — used to register runtime-generated
    // PROCEDURAL tree meshes into the instanced static-mesh cache (vast-forest palette). Same
    // VAO layout as Load (attrib 0=pos,1=norm,2=uv); the instanced path adds the matrix at 3-6.
    static std::unique_ptr<Mesh> CreateFromArrays(const std::vector<Vertex>& vertices,
                                                  const std::vector<uint32_t>& indices);
    // Loads an.lmesh v2 (LMS2) skinned mesh: VAO layout 0=pos, 1=norm,
    // 2=uv, 3=joints (u8x4 integer), 4=weights (u8x4 normalized)..
    static std::unique_ptr<Mesh> LoadSkinned(const std::string& path);
};

} // namespace Luminumbra::Rendering