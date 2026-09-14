#include "FarVolumeRenderMesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Luminumbra::Rendering {
namespace {

glm::dvec3 FaceNormal(const World::FarVolumeMesh& mesh, std::size_t offset) {
    const glm::dvec3 a(mesh.vertices[mesh.indices[offset]].position);
    const glm::dvec3 b(mesh.vertices[mesh.indices[offset + 1]].position);
    const glm::dvec3 c(mesh.vertices[mesh.indices[offset + 2]].position);
    return glm::cross(b - a, c - a);
}

bool IsTriangle(const glm::dvec3& normal) {
    return glm::dot(normal, normal) > 1.0e-10;
}

std::size_t BufferBytes(std::size_t vertices, std::size_t indices) {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (vertices > maximum / sizeof(VoxelVertex) ||
        indices > (maximum - vertices * sizeof(VoxelVertex)) / sizeof(std::uint32_t))
        throw std::length_error("Far-volume render buffer size overflow");
    return vertices * sizeof(VoxelVertex) + indices * sizeof(std::uint32_t);
}

} // namespace

std::size_t FarVolumeRenderMesh::owned_bytes() const {
    return BufferBytes(vertices.capacity(), indices.capacity());
}

FarVolumeRenderMesh AdaptFarVolumeRenderMesh(const World::FarVolumeMesh& source,
                                             std::size_t max_buffer_bytes) {
    if (max_buffer_bytes > kFarVolumeRenderMeshBudget)
        throw std::invalid_argument("Far-volume converted limit exceeds fixture ceiling");
    if (source.indices.size() % 3u != 0)
        throw std::invalid_argument("Far-volume render indices are not triangles");
    for (const auto& vertex : source.vertices)
        for (const float value : {vertex.position.x, vertex.position.y, vertex.position.z})
            if (!std::isfinite(value))
                throw std::invalid_argument("Non-finite far-volume render position");
    for (const auto index : source.indices)
        if (index >= source.vertices.size())
            throw std::invalid_argument("Invalid far-volume render index");

    const auto maximum =
        std::min<std::size_t>(std::numeric_limits<std::uint32_t>::max(),
                              max_buffer_bytes / (sizeof(VoxelVertex) + sizeof(std::uint32_t)));
    std::size_t count = 0;
    for (std::size_t i = 0; i < source.indices.size(); i += 3) {
        if (!IsTriangle(FaceNormal(source, i)))
            continue;
        if (maximum - count < 3)
            throw std::length_error("Far-volume converted mesh budget exceeded");
        count += 3;
    }

    FarVolumeRenderMesh mesh;
    mesh.vertices.reserve(count);
    mesh.indices.reserve(count);
    if (mesh.owned_bytes() > max_buffer_bytes)
        throw std::length_error("Far-volume converted capacity budget exceeded");
    FarVolumeMeshBounds bounds{Vec3(std::numeric_limits<float>::max()),
                               Vec3(std::numeric_limits<float>::lowest())};
    for (std::size_t i = 0; i < source.indices.size(); i += 3) {
        const auto face = FaceNormal(source, i);
        if (!IsTriangle(face))
            continue;
        const Vec3 normal(glm::normalize(face));
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto& vertex = source.vertices[source.indices[i + corner]];
            mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
            mesh.vertices.push_back({vertex.position, normal, vertex.material});
            bounds.min = glm::min(bounds.min, vertex.position);
            bounds.max = glm::max(bounds.max, vertex.position);
        }
    }
    if (!mesh.vertices.empty())
        mesh.bounds = bounds;
    return mesh;
}

} // namespace Luminumbra::Rendering
