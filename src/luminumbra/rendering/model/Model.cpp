#include "luminumbra/rendering/model/Model.h"
#include "luminumbra/rendering/TextureLoader.h"
#include "luminumbra/core/ResourceManager.h"
#include "luminumbra/core/Debug.h"
#include <fstream>
#include <iostream>

namespace Luminumbra::Rendering {

// --- Mesh Struct Definitions (Same as before) ---
// Defines the data layout for each vertex
struct LBMHeader {
    char magic[4];
    uint32_t version;
    uint32_t meshCount;
};

// Header for each mesh within the file
struct LBMMeshInfo {
    uint64_t vertexCount;
    uint64_t indexCount;
    uint32_t diffuseTexturePathLength;
};


Mesh::Mesh(std::vector<Vertex> vertices, std::vector<unsigned int> indices, std::vector<Texture> textures)
    : vertices(std::move(vertices)), indices(std::move(indices)), textures(std::move(textures)) {
    setupMesh();
}

void Mesh::setupMesh() {
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glGenBuffers(1, &m_EBO);

    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), &vertices[0], GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), &indices[0], GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, TexCoords));

    glBindVertexArray(0);
}

void Mesh::setupInstancing(GLuint instanceVBO) {
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);

    // A mat4 is 4 vec4s, so we need 4 attribute pointers.
    // We start at location 3 because locations 0, 1, and 2 are already used by Position, Normal, and TexCoords.
    for (int i = 0; i < 4; ++i) {
        // Locations 3, 4, 5, 6
        glEnableVertexAttribArray(3 + i);
        glVertexAttribPointer(3 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(sizeof(glm::vec4) * i));
        glVertexAttribDivisor(3 + i, 1);
    }
    glBindVertexArray(0);
}

void Mesh::drawInstanced(Shader& shader, GLsizei instanceCount) {
    unsigned int diffuseNr = 1;
    for (unsigned int i = 0; i < textures.size(); i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        std::string number;
        std::string name = textures[i].type;
        if (name == "texture_diffuse")
            number = std::to_string(diffuseNr++);
        
        shader.setInt(("material." + name + number).c_str(), i);
        glBindTexture(GL_TEXTURE_2D, textures[i].id);
    }

    glBindVertexArray(m_VAO);
    glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, 0, instanceCount);
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE0);
}

// --- Model Implementation (MODIFIED) ---
Model::Model(const std::string& path) {
    loadModel(path);
}

void Model::setupInstancing(GLuint instanceVBO) {
    for (auto& mesh : m_Meshes) {
        mesh.setupInstancing(instanceVBO);
    }
}

void Model::drawInstanced(Shader& shader, GLsizei instanceCount) {
    for (auto& mesh : m_Meshes) {
        mesh.drawInstanced(shader, instanceCount);
    }
}

void Model::loadModel(const std::string& path) {
    std::string fullPath = Core::ResourceManager::getInstance().getResourcePath(path);
    m_Directory = fullPath.substr(0, fullPath.find_last_of('/'));
    
    std::ifstream file(fullPath, std::ios::binary);
    if (!file.is_open()) {
        LOG("ERROR::MODEL::Failed to open model file: " + fullPath);
        return;
    }

    LBMHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(LBMHeader));

    if (strncmp(header.magic, "LBM ", 4) != 0) {
        LOG("ERROR::MODEL::Invalid file format or magic number mismatch for: " + fullPath);
        return;
    }

    std::vector<LBMMeshInfo> meshInfos(header.meshCount);
    file.read(reinterpret_cast<char*>(meshInfos.data()), header.meshCount * sizeof(LBMMeshInfo));

    for (const auto& meshInfo : meshInfos) {
        // Read texture path
        std::string diffusePathStr;
        diffusePathStr.resize(meshInfo.diffuseTexturePathLength);
        file.read(&diffusePathStr[0], meshInfo.diffuseTexturePathLength);

        // Read vertices
        std::vector<Vertex> vertices(meshInfo.vertexCount);
        file.read(reinterpret_cast<char*>(vertices.data()), meshInfo.vertexCount * sizeof(Vertex));

        // Read indices
        std::vector<unsigned int> indices(meshInfo.indexCount);
        file.read(reinterpret_cast<char*>(indices.data()), meshInfo.indexCount * sizeof(unsigned int));

        // Load textures
        std::vector<Texture> textures;
        if (!diffusePathStr.empty()) {
            bool skip = false;
            for(const auto& loadedTex : m_TexturesLoaded) {
                if (loadedTex.path == diffusePathStr) {
                    textures.push_back(loadedTex);
                    skip = true;
                    break;
                }
            }
            
            if (!skip) {
                Texture texture;
                texture.id = loadTexture(m_Directory + '/' + diffusePathStr);
                texture.type = "texture_diffuse";
                texture.path = diffusePathStr;
                textures.push_back(texture);
                m_TexturesLoaded.push_back(texture);
            }
        }
        
        m_Meshes.emplace_back(std::move(vertices), std::move(indices), std::move(textures));
    }
}

// These Assimp-related functions are no longer needed in the engine runtime
// void Model::processNode(...) {}
// Mesh Model::processMesh(...) {}
// std::vector<Texture> Model::loadMaterialTextures(...) {}

} // namespace Luminumbra::Rendering