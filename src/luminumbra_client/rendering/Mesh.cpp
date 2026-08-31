#include "Mesh.h"
#include "luminumbra_common/animation/SkinnedMeshFormat.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>
#include "core/Log.h"

namespace Luminumbra::Rendering {
namespace {

constexpr uint32_t kLMeshMagic =
    static_cast<uint32_t>('L') |
    (static_cast<uint32_t>('M') << 8) |
    (static_cast<uint32_t>('S') << 16) |
    (static_cast<uint32_t>('H') << 24);
constexpr std::string_view kDataPrefix = "data/";

bool HasDataPrefix(const std::filesystem::path& path) {
    return path.generic_string().starts_with(kDataPrefix);
}

std::filesystem::path DataRelativePath(const std::filesystem::path& path) {
    const std::string genericPath = path.generic_string();

    if (genericPath.starts_with(kDataPrefix)) {
        return genericPath.substr(kDataPrefix.size());
    }

    return path;
}

void AddCandidate(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& candidate) {
    for (const auto& existing : candidates) {
        if (existing == candidate) {
            return;
        }
    }

    candidates.push_back(candidate);
}

std::vector<std::filesystem::path> BuildMeshPathCandidates(const std::string& path) {
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path requestedPath(path);

    if (requestedPath.is_absolute()) {
        AddCandidate(candidates, requestedPath);
        return candidates;
    }

    const bool hasDataPrefix = HasDataPrefix(requestedPath);
    const std::filesystem::path dataRelativePath = DataRelativePath(requestedPath);

    if (!hasDataPrefix) {
        AddCandidate(candidates, requestedPath);
    }

#ifdef LUMINUMBRA_BUILD_DATA_DIR
    AddCandidate(candidates, std::filesystem::path(LUMINUMBRA_BUILD_DATA_DIR) / dataRelativePath);
#endif

#ifdef LUMINUMBRA_SOURCE_DATA_DIR
    AddCandidate(candidates, std::filesystem::path(LUMINUMBRA_SOURCE_DATA_DIR) / dataRelativePath);
#endif

    if (hasDataPrefix) {
        AddCandidate(candidates, requestedPath);
    }

    return candidates;
}

std::ifstream OpenMeshFile(const std::string& path, std::filesystem::path& resolvedPath) {
    for (const auto& candidate : BuildMeshPathCandidates(path)) {
        std::ifstream file(candidate, std::ios::binary);
        if (file) {
            resolvedPath = candidate;
            return file;
        }
    }

    return {};
}

} // namespace

struct LMeshHeader {
    uint32_t magic;
    uint32_t vertexCount;
    uint32_t indexCount;
    float boundingSphere[4];
};

std::unique_ptr<Mesh> MeshLoader::Load(const std::string& path) {
    std::filesystem::path resolvedPath;
    std::ifstream inFile = OpenMeshFile(path, resolvedPath);
    if (!inFile) { LUMINUMBRA_CORE_ERROR("Failed to open mesh file: {}", path); return nullptr; }

    LMeshHeader header;
    inFile.read(reinterpret_cast<char*>(&header), sizeof(LMeshHeader));
    if (!inFile) { LUMINUMBRA_CORE_ERROR("Failed to read mesh header: {}", resolvedPath.string()); return nullptr; }
    if (header.magic != kLMeshMagic) { LUMINUMBRA_CORE_ERROR("Invalid mesh file format for: {}", resolvedPath.string()); return nullptr; }
    if (header.vertexCount == 0 || header.indexCount == 0) { LUMINUMBRA_CORE_ERROR("Mesh file has no geometry: {}", resolvedPath.string()); return nullptr; }

    const auto vertexBytes = static_cast<std::uintmax_t>(header.vertexCount) * sizeof(Vertex);
    const auto indexBytes = static_cast<std::uintmax_t>(header.indexCount) * sizeof(uint32_t);
    const auto expectedSize = static_cast<std::uintmax_t>(sizeof(LMeshHeader)) + vertexBytes + indexBytes;

    std::error_code fileSizeError;
    const auto actualSize = std::filesystem::file_size(resolvedPath, fileSizeError);
    if (fileSizeError || actualSize != expectedSize) {
        LUMINUMBRA_CORE_ERROR("Mesh file size mismatch for: {}", resolvedPath.string());
        return nullptr;
    }

    std::vector<Vertex> vertices(header.vertexCount);
    std::vector<uint32_t> indices(header.indexCount);
    inFile.read(reinterpret_cast<char*>(vertices.data()), vertices.size() * sizeof(Vertex));
    if (!inFile) { LUMINUMBRA_CORE_ERROR("Failed to read mesh vertices: {}", resolvedPath.string()); return nullptr; }
    inFile.read(reinterpret_cast<char*>(indices.data()), indices.size() * sizeof(uint32_t));
    if (!inFile) { LUMINUMBRA_CORE_ERROR("Failed to read mesh indices: {}", resolvedPath.string()); return nullptr; }

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

    LUMINUMBRA_CORE_INFO("Loaded mesh '{}' ({} verts, {} indices)", resolvedPath.string(), header.vertexCount, header.indexCount);
    return mesh;
}

std::unique_ptr<Mesh> MeshLoader::CreateFromArrays(const std::vector<Vertex>& vertices,
                                                  const std::vector<uint32_t>& indices) {
    auto mesh = std::make_unique<Mesh>();
    if (vertices.empty() || indices.empty()) return mesh;  // empty mesh (caller checks indexCount)
    mesh->indexCount = static_cast<uint32_t>(indices.size());

    // Bounding sphere from the AABB centre + farthest vertex (the instanced renderer frustum-
    // culls per instance against this).
    glm::vec3 lo = vertices[0].pos, hi = vertices[0].pos;
    for (const Vertex& v : vertices) { lo = glm::min(lo, v.pos); hi = glm::max(hi, v.pos); }
    const glm::vec3 c = (lo + hi) * 0.5f;
    float r2 = 0.0f;
    for (const Vertex& v : vertices) r2 = glm::max(r2, glm::dot(v.pos - c, v.pos - c));
    mesh->boundingSphere = glm::vec4(c, std::sqrt(r2));

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
    return mesh;
}

std::unique_ptr<Mesh> MeshLoader::LoadSkinned(const std::string& path) {
    namespace anim = luminumbra::animation;

    std::filesystem::path resolvedPath;
    {
        // Reuse the candidate resolution the v1 loader uses, then hand the
        // resolved file to the shared LMS2 reader.
        std::ifstream probe = OpenMeshFile(path, resolvedPath);
        if (!probe) { LUMINUMBRA_CORE_ERROR("Failed to open skinned mesh file: {}", path); return nullptr; }
    }

    anim::SkinnedMeshAsset asset;
    if (!anim::LoadSkinnedMeshAsset(resolvedPath.string(), asset)) {
        LUMINUMBRA_CORE_ERROR("Invalid LMS2 skinned mesh file: {}", resolvedPath.string());
        return nullptr;
    }
    if (asset.header.vertexCount == 0 || asset.header.indexCount == 0 || asset.header.jointCount == 0) {
        LUMINUMBRA_CORE_ERROR("Skinned mesh file has no geometry or skeleton: {}", resolvedPath.string());
        return nullptr;
    }

    auto mesh = std::make_unique<Mesh>();
    mesh->indexCount = asset.header.indexCount;
    mesh->jointCount = asset.header.jointCount;
    mesh->boundingSphere = {
        asset.header.boundingSphere[0],
        asset.header.boundingSphere[1],
        asset.header.boundingSphere[2],
        asset.header.boundingSphere[3]};

    glGenVertexArrays(1, &mesh->vao);
    glGenBuffers(1, &mesh->vbo);
    glGenBuffers(1, &mesh->ebo);

    glBindVertexArray(mesh->vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, asset.vertices.size() * sizeof(anim::SkinnedVertexData), asset.vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, asset.indices.size() * sizeof(uint32_t), asset.indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = static_cast<GLsizei>(sizeof(anim::SkinnedVertexData));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(anim::SkinnedVertexData, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(anim::SkinnedVertexData, norm));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(anim::SkinnedVertexData, uv));
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 4, GL_UNSIGNED_BYTE, stride, (void*)offsetof(anim::SkinnedVertexData, joints));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(anim::SkinnedVertexData, weights));
    glBindVertexArray(0);

    LUMINUMBRA_CORE_INFO(
        "Loaded skinned mesh '{}' ({} verts, {} indices, {} joints)",
        resolvedPath.string(), asset.header.vertexCount, asset.header.indexCount, asset.header.jointCount);
    return mesh;
}
}
