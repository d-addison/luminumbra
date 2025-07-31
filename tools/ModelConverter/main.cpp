#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

// This Vertex structure MUST exactly match the one in your engine
// at 'src/luminumbra/rendering/model/Model.h'
struct Vertex {
    float pos[3];
    float normal[3];
    float texCoords[2];
};

// Header for the entire .lbm file
struct LBMHeader {
    char magic[4] = {'L', 'B', 'M', ' '};
    uint32_t version = 1;
    uint32_t meshCount = 0;
};

// Header for each individual mesh within the file
struct LBMMeshInfo {
    uint64_t vertexCount = 0;
    uint64_t indexCount = 0;
    uint32_t diffuseTexturePathLength = 0;
};

void convertModel(const std::string& inputPath, const std::string& outputPath) {
    std::cout << "Loading model: " << inputPath << "..." << std::endl;

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(inputPath, 
        aiProcess_Triangulate | 
        aiProcess_GenSmoothNormals | 
        aiProcess_FlipUVs | 
        aiProcess_CalcTangentSpace
    );

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        std::cerr << "ERROR::ASSIMP::" << importer.GetErrorString() << std::endl;
        return;
    }

    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile.is_open()) {
        std::cerr << "ERROR: Could not open output file " << outputPath << std::endl;
        return;
    }

    // --- Write File Header ---
    LBMHeader header;
    header.meshCount = scene->mNumMeshes;
    outFile.write(reinterpret_cast<const char*>(&header), sizeof(LBMHeader));

    std::cout << "Model has " << scene->mNumMeshes << " meshes. Processing..." << std::endl;

    // --- First, write all mesh info headers ---
    std::vector<std::string> diffuseMapPaths;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[i];
        aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
        
        LBMMeshInfo meshInfo;
        meshInfo.vertexCount = mesh->mNumVertices;
        meshInfo.indexCount = mesh->mNumFaces * 3;

        std::string diffusePath = ""; // Fallback texture
        if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0) {
            aiString str;
            material->GetTexture(aiTextureType_DIFFUSE, 0, &str);
            // Get just the filename from the path
            diffusePath = std::string(str.C_Str());
            size_t lastSlash = diffusePath.find_last_of("/\\");
            if (lastSlash != std::string::npos) {
                diffusePath = diffusePath.substr(lastSlash + 1);
            }
        }
        diffuseMapPaths.push_back(diffusePath);
        meshInfo.diffuseTexturePathLength = static_cast<uint32_t>(diffusePath.length());
        
        outFile.write(reinterpret_cast<const char*>(&meshInfo), sizeof(LBMMeshInfo));
    }

    // --- Second, write all mesh data blocks ---
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[i];

        // 1. Write texture path string
        outFile.write(diffuseMapPaths[i].c_str(), diffuseMapPaths[i].length());
        
        // 2. Write vertex data
        std::vector<Vertex> vertices;
        vertices.reserve(mesh->mNumVertices);
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            Vertex vertex;
            vertex.pos[0] = mesh->mVertices[v].x;
            vertex.pos[1] = mesh->mVertices[v].y;
            vertex.pos[2] = mesh->mVertices[v].z;

            if (mesh->HasNormals()) {
                vertex.normal[0] = mesh->mNormals[v].x;
                vertex.normal[1] = mesh->mNormals[v].y;
                vertex.normal[2] = mesh->mNormals[v].z;
            }

            if (mesh->mTextureCoords[0]) {
                vertex.texCoords[0] = mesh->mTextureCoords[0][v].x;
                vertex.texCoords[1] = mesh->mTextureCoords[0][v].y;
            } else {
                vertex.texCoords[0] = 0.0f;
                vertex.texCoords[1] = 0.0f;
            }
            vertices.push_back(vertex);
        }
        outFile.write(reinterpret_cast<const char*>(vertices.data()), vertices.size() * sizeof(Vertex));

        // 3. Write index data
        std::vector<unsigned int> indices;
        indices.reserve(mesh->mNumFaces * 3);
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            aiFace face = mesh->mFaces[f];
            for (unsigned int k = 0; k < face.mNumIndices; ++k) {
                indices.push_back(face.mIndices[k]);
            }
        }
        outFile.write(reinterpret_cast<const char*>(indices.data()), indices.size() * sizeof(unsigned int));
    }

    outFile.close();
    std::cout << "Successfully converted model to " << outputPath << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input_model_path> <output_lbm_path>" << std::endl;
        return 1;
    }

    convertModel(argv[1], argv[2]);

    return 0;
}