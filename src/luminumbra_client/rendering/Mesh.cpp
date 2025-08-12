#include "Mesh.h"
#include <fstream>
#include <vector>
#include "core/Log.h"

namespace Luminumbra::Rendering {

struct LMeshHeader {
    uint32_t magic;
    uint32_t vertexCount;
    uint32_t indexCount;
    float boundingSphere[4];
};

std::unique_ptr<Mesh> MeshLoader::Load(const std::string& path) {
    std::ifstream inFile(path, std::ios::binary);
    if (!inFile) { LUMINUMBRA_CORE_ERROR("Failed to open mesh file: {}", path); return nullptr; }

    LMeshHeader header;
    inFile.read(reinterpret_cast<char*>(&header), sizeof(LMeshHeader));
    if (header.magic != *reinterpret_cast<const uint32_t*>("LMSH")) { LUMINUMBRA_CORE_ERROR("Invalid mesh file format for: {}", path); return nullptr; }

    std::vector<Vertex> vertices(header.vertexCount);
    std::vector<uint32_t> indices(header.indexCount);
    inFile.read(reinterpret_cast<char*>(vertices.data()), vertices.size() * sizeof(Vertex));
    inFile.read(reinterpret_cast<char*>(indices.data()), indices.size() * sizeof(uint32_t));

    auto mesh = std::make_unique<Mesh>();
    mesh->indexCount = header.indexCount;
    mesh->boundingSphere = {header.boundingSphere[0], header.boundingSphere[1], header.boundingSphere[2], header.boundingSphere[3]};

    glGenVertexArrays(1, &mesh->vao);
    glGenBuffers(1, &mesh->vbo);
    glGenBuffers(1, &mesh->ebo);

    glBindVertexArray(mesh->vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, norm));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
    glBindVertexArray(0);

    LUMINUMBRA_CORE_INFO("Loaded mesh '{}' ({} verts, {} indices)", path, header.vertexCount, header.indexCount);
    return mesh;
}
}