#include "Mesh.h"
#include "UploadBudget.h"
#include "core/Log.h"
#include "luminumbra_common/animation/SkinnedMeshFormat.h"
#include "luminumbra_common/core/CaptureContentPaths.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <vector>

namespace Luminumbra::Rendering {
namespace {

constexpr uint32_t kLMeshMagic = static_cast<uint32_t>('L') | (static_cast<uint32_t>('M') << 8) |
                                 (static_cast<uint32_t>('S') << 16) |
                                 (static_cast<uint32_t>('H') << 24);
std::atomic<std::uint64_t>& MeshFileReads() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

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

void AddCandidate(std::vector<std::filesystem::path>& candidates,
                  const std::filesystem::path& candidate) {
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

Filesystem::ContentInput OpenMeshFile(const std::string& path,
                                      std::filesystem::path& resolvedPath) {
    // Capture resolution never falls back to a different build/source data tree.
    if (Filesystem::capture_content_paths().active()) {
        const std::filesystem::path requested(path);
        bool rooted_content = false;
        if (!requested.empty())
            for (const auto* tree : Filesystem::kCaptureContentTrees) {
                const std::filesystem::path content_tree(tree);
                rooted_content = rooted_content || *requested.begin() == *content_tree.begin();
            }
        resolvedPath =
            Filesystem::IsAbsolutePath(requested)
                ? requested
                : Filesystem::capture_content_paths().root() /
                      (rooted_content ? requested : std::filesystem::path("data") / requested);
        MeshFileReads().fetch_add(1, std::memory_order_relaxed);
        return Filesystem::ContentInput(resolvedPath, std::ios::binary);
    }
    for (const auto& candidate : BuildMeshPathCandidates(path)) {
        MeshFileReads().fetch_add(1, std::memory_order_relaxed);
        Filesystem::ContentInput file(candidate, std::ios::binary);
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

std::uint64_t MeshLoader::file_read_count() {
    return MeshFileReads().load(std::memory_order_relaxed);
}

std::size_t MeshPayload::upload_bytes() const {
    return vertices.size() * sizeof(Vertex) + skinnedVertices.size() * sizeof(SkinnedVertex) +
           indices.size() * sizeof(uint32_t);
}

std::unique_ptr<MeshPayload> MeshLoader::Read(const std::string& path, bool optional) {
    std::filesystem::path resolvedPath;
    Filesystem::ContentInput inFile = OpenMeshFile(path, resolvedPath);
    if (!inFile) {
        // Only absence is quiet. Probe after the failed open solely to retain
        // the previous unreadable-asset log; no subsequent read uses this path.
        std::error_code lookupError;
        const bool report = !optional || std::filesystem::exists(path, lookupError) || lookupError;
        if (report)
            LUMINUMBRA_CORE_ERROR("Failed to open mesh file: {}", path);
        return nullptr;
    }

    LMeshHeader header;
    inFile.read(reinterpret_cast<char*>(&header), sizeof(LMeshHeader));
    if (!inFile) {
        LUMINUMBRA_CORE_ERROR("Failed to read mesh header: {}", resolvedPath.string());
        return nullptr;
    }
    if (header.magic != kLMeshMagic) {
        LUMINUMBRA_CORE_ERROR("Invalid mesh file format for: {}", resolvedPath.string());
        return nullptr;
    }
    if (header.vertexCount == 0 || header.indexCount == 0) {
        LUMINUMBRA_CORE_ERROR("Mesh file has no geometry: {}", resolvedPath.string());
        return nullptr;
    }

    const auto vertexBytes = static_cast<std::uintmax_t>(header.vertexCount) * sizeof(Vertex);
    const auto indexBytes = static_cast<std::uintmax_t>(header.indexCount) * sizeof(uint32_t);
    const auto expectedSize =
        static_cast<std::uintmax_t>(sizeof(LMeshHeader)) + vertexBytes + indexBytes;

    // Measure and consume the same stream; never stat/reopen the pathname.
    const auto payloadStart = inFile.tellg();
    inFile.seekg(0, std::ios::end);
    const auto actualSize = inFile.tellg();
    inFile.seekg(payloadStart);
    std::size_t uploadBytes = 0;
    const auto maximumRead =
        static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max());
    if (!inFile || actualSize < 0 || static_cast<std::uintmax_t>(actualSize) != expectedSize ||
        vertexBytes > maximumRead || indexBytes > maximumRead ||
        !mesh_upload_bytes(header.vertexCount, sizeof(Vertex), header.indexCount, uploadBytes)) {
        LUMINUMBRA_CORE_ERROR("Mesh file size mismatch for: {}", resolvedPath.string());
        return nullptr;
    }

    std::vector<Vertex> vertices(header.vertexCount);
    std::vector<uint32_t> indices(header.indexCount);
    inFile.read(reinterpret_cast<char*>(vertices.data()),
                static_cast<std::streamsize>(vertexBytes));
    if (!inFile) {
        LUMINUMBRA_CORE_ERROR("Failed to read mesh vertices: {}", resolvedPath.string());
        return nullptr;
    }
    inFile.read(reinterpret_cast<char*>(indices.data()), static_cast<std::streamsize>(indexBytes));
    if (!inFile) {
        LUMINUMBRA_CORE_ERROR("Failed to read mesh indices: {}", resolvedPath.string());
        return nullptr;
    }

    auto payload = std::make_unique<MeshPayload>();
    payload->vertices = std::move(vertices);
    payload->indices = std::move(indices);
    payload->boundingSphere = {header.boundingSphere[0],
                               header.boundingSphere[1],
                               header.boundingSphere[2],
                               header.boundingSphere[3]};
    payload->resolvedPath = resolvedPath.string();
    return payload;
}

std::unique_ptr<Mesh> MeshLoader::Load(const std::string& path) {
    auto payload = Read(path);
    return payload ? Adopt(*payload) : nullptr;
}

std::unique_ptr<Mesh> MeshLoader::CreateFromArrays(const std::vector<Vertex>& vertices,
                                                   const std::vector<uint32_t>& indices) {
    auto mesh = std::make_unique<Mesh>();
    if (vertices.empty() || indices.empty())
        return mesh; // empty mesh (caller checks indexCount)
    std::size_t uploadBytes = 0;
    if (!mesh_upload_bytes(vertices.size(), sizeof(Vertex), indices.size(), uploadBytes))
        return nullptr;
    mesh->indexCount = static_cast<uint32_t>(indices.size());

    // Bounding sphere from the AABB centre + farthest vertex (the instanced renderer frustum-
    // culls per instance against this).
    glm::vec3 lo = vertices[0].pos, hi = vertices[0].pos;
    for (const Vertex& v : vertices) {
        lo = glm::min(lo, v.pos);
        hi = glm::max(hi, v.pos);
    }
    const glm::vec3 c = (lo + hi) * 0.5f;
    float r2 = 0.0f;
    for (const Vertex& v : vertices)
        r2 = glm::max(r2, glm::dot(v.pos - c, v.pos - c));
    mesh->boundingSphere = glm::vec4(c, std::sqrt(r2));

    glGenVertexArrays(1, &mesh->vao);
    glGenBuffers(1, &mesh->vbo);
    glGenBuffers(1, &mesh->ebo);
    glBindVertexArray(mesh->vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                 vertices.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
                 indices.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, norm));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
    glBindVertexArray(0);
    return mesh;
}

std::unique_ptr<MeshPayload> MeshLoader::ReadSkinned(const std::string& path) {
    namespace anim = luminumbra::animation;
    std::filesystem::path resolvedPath;
    {
        // Preserve the shared path-based LMS2 reader and candidate resolution.
        Filesystem::ContentInput input = OpenMeshFile(path, resolvedPath);
        if (!input) {
            LUMINUMBRA_CORE_ERROR("Failed to open skinned mesh file: {}", path);
            return nullptr;
        }
    }
    anim::SkinnedMeshAsset asset;
    MeshFileReads().fetch_add(1, std::memory_order_relaxed);
    if (!anim::LoadSkinnedMeshAsset(resolvedPath.string(), asset)) {
        LUMINUMBRA_CORE_ERROR("Invalid LMS2 skinned mesh file: {}", resolvedPath.string());
        return nullptr;
    }
    if (asset.header.vertexCount == 0 || asset.header.indexCount == 0 ||
        asset.header.jointCount == 0) {
        LUMINUMBRA_CORE_ERROR("Skinned mesh file has no geometry or skeleton: {}",
                              resolvedPath.string());
        return nullptr;
    }

    std::size_t uploadBytes = 0;
    if (!mesh_upload_bytes(
            asset.vertices.size(), sizeof(SkinnedVertex), asset.indices.size(), uploadBytes)) {
        LUMINUMBRA_CORE_ERROR("Invalid LMS2 skinned mesh file: {}", resolvedPath.string());
        return nullptr;
    }
    auto payload = std::make_unique<MeshPayload>();
    payload->skinnedVertices.resize(asset.vertices.size());
    static_assert(sizeof(SkinnedVertex) == sizeof(anim::SkinnedVertexData));
    std::memcpy(payload->skinnedVertices.data(),
                asset.vertices.data(),
                asset.vertices.size() * sizeof(SkinnedVertex));
    payload->indices = std::move(asset.indices);
    payload->jointCount = asset.header.jointCount;
    payload->boundingSphere = {asset.header.boundingSphere[0],
                               asset.header.boundingSphere[1],
                               asset.header.boundingSphere[2],
                               asset.header.boundingSphere[3]};
    payload->resolvedPath = resolvedPath.string();
    return payload;
}

std::unique_ptr<Mesh> MeshLoader::LoadSkinned(const std::string& path) {
    auto payload = ReadSkinned(path);
    return payload ? Adopt(*payload) : nullptr;
}

std::unique_ptr<Mesh> MeshLoader::Adopt(const MeshPayload& payload) {
    const bool skinned = payload.jointCount != 0;
    const auto count = skinned ? payload.skinnedVertices.size() : payload.vertices.size();
    std::size_t uploadBytes = 0;
    if (count == 0 || payload.indices.empty() ||
        !mesh_upload_bytes(count,
                           skinned ? sizeof(SkinnedVertex) : sizeof(Vertex),
                           payload.indices.size(),
                           uploadBytes))
        return nullptr;
    auto mesh = std::make_unique<Mesh>();
    mesh->indexCount = static_cast<uint32_t>(payload.indices.size());
    mesh->jointCount = payload.jointCount;
    mesh->boundingSphere = payload.boundingSphere;
    glGenVertexArrays(1, &mesh->vao);
    glGenBuffers(1, &mesh->vbo);
    glGenBuffers(1, &mesh->ebo);
    glBindVertexArray(mesh->vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    const auto vertexBytes = skinned ? count * sizeof(SkinnedVertex) : count * sizeof(Vertex);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertexBytes),
                 skinned ? static_cast<const void*>(payload.skinnedVertices.data())
                         : static_cast<const void*>(payload.vertices.data()),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(payload.indices.size() * sizeof(uint32_t)),
                 payload.indices.data(),
                 GL_STATIC_DRAW);
    const auto stride = static_cast<GLsizei>(skinned ? sizeof(SkinnedVertex) : sizeof(Vertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, norm));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, uv));
    if (skinned) {
        glEnableVertexAttribArray(3);
        glVertexAttribIPointer(
            3, 4, GL_UNSIGNED_BYTE, stride, (void*)offsetof(SkinnedVertex, joints));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(
            4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(SkinnedVertex, weights));
    }
    glBindVertexArray(0);
    if (skinned) {
        LUMINUMBRA_CORE_INFO("Loaded skinned mesh '{}' ({} verts, {} indices, {} joints)",
                             payload.resolvedPath,
                             count,
                             mesh->indexCount,
                             mesh->jointCount);
    } else {
        LUMINUMBRA_CORE_INFO("Loaded mesh '{}' ({} verts, {} indices)",
                             payload.resolvedPath,
                             count,
                             mesh->indexCount);
    }
    return mesh;
}
} // namespace Luminumbra::Rendering
