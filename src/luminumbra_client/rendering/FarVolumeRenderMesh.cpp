#include "FarVolumeRenderMesh.h"

#include "luminumbra_common/world/FarVolumeRuntime.h"

#include <cmath>
#include <glm/common.hpp>
#include <limits>
#include <new>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
bool FloatVector(const World::FarVolumeVector& input, Vec3& output) {
    for (int axis = 0; axis < 3; ++axis) {
        const auto component = static_cast<std::size_t>(axis);
        if (!std::isfinite(input[component]) ||
            std::abs(input[component]) > std::numeric_limits<float>::max())
            return false;
        output[axis] = static_cast<float>(input[component]);
    }
    return true;
}
bool SourceMetadata(const World::FarVolumeMesh& source) {
    World::FarTileKey key;
    if (!World::ToFarTileKey(source.request.key, key))
        return false;
    const auto tier = World::FarTierAt(key.tier);
    if (!tier)
        return false;
    const auto edge = static_cast<std::int64_t>(tier->tile_edge_meters);
    const auto brick = static_cast<std::int64_t>(tier->brick_edge_meters);
    const auto& span = source.request.span;
    if (span.min_y <= -World::kFarVolumeExactCoordinateLimitMeters ||
        span.max_y >= World::kFarVolumeExactCoordinateLimitMeters || span.min_y >= span.max_y ||
        span.min_y % brick != 0 || span.max_y % brick != 0)
        return false;
    return source.origin_meters ==
               World::FarVolumePosition{key.tx * edge, span.min_y, key.tz * edge} &&
           source.sampled_max_meters ==
               World::FarVolumePosition{key.tx * edge + edge, span.max_y, key.tz * edge + edge} &&
           (source.content == World::FarVolumeContent::Air ||
            source.content == World::FarVolumeContent::Solid ||
            source.content == World::FarVolumeContent::Mixed);
}
} // namespace

std::uint64_t FarVolumeRenderRetainedBytes(const FarVolumeRenderMesh& mesh) {
    return static_cast<std::uint64_t>(mesh.vertices.capacity()) * sizeof(VoxelVertex) +
           static_cast<std::uint64_t>(mesh.indices.capacity()) * sizeof(std::uint32_t);
}

bool AdaptFarVolumeRenderMesh(const World::FarVolumeMesh& source,
                              std::uint64_t max_buffer_bytes,
                              const World::FarVolumeMemory& memory,
                              FarVolumeRenderMesh& output,
                              FarVolumeRenderError* error) {
    const auto fail = [error](FarVolumeRenderError value) {
        if (error)
            *error = value;
        return false;
    };
    World::FarVolumeMemoryScope scope(memory, true);
    if (!memory.pool || !scope.valid())
        return fail(FarVolumeRenderError::InvalidMemory);
    if (!SourceMetadata(source) || source.indices.size() % 3 != 0 ||
        source.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
        (source.vertices.empty() != source.indices.empty()) ||
        (source.content != World::FarVolumeContent::Mixed && !source.vertices.empty()))
        return fail(FarVolumeRenderError::InvalidSource);
    if (source.vertices.size() > max_buffer_bytes / sizeof(VoxelVertex))
        return fail(FarVolumeRenderError::ByteLimit);
    const auto vertex_bytes =
        static_cast<std::uint64_t>(source.vertices.size()) * sizeof(VoxelVertex);
    if (source.indices.size() > (max_buffer_bytes - vertex_bytes) / sizeof(std::uint32_t))
        return fail(FarVolumeRenderError::ByteLimit);
    for (const auto& vertex : source.vertices) {
        Vec3 position{0.0f}, normal{0.0f};
        if (!FloatVector(vertex.position, position) || !FloatVector(vertex.normal, normal) ||
            (normal.x == 0 && normal.y == 0 && normal.z == 0))
            return fail(FarVolumeRenderError::InvalidSource);
        const auto tier = World::FarTierAt(source.request.key.tier);
        if (!tier)
            return fail(FarVolumeRenderError::InvalidSource);
        const auto edge = tier->tile_edge_meters;
        const auto height = source.request.span.max_y - source.request.span.min_y;
        if (vertex.position[0] < 0 || vertex.position[0] > edge || vertex.position[1] < 0 ||
            vertex.position[1] > static_cast<double>(height) || vertex.position[2] < 0 ||
            vertex.position[2] > edge)
            return fail(FarVolumeRenderError::InvalidSource);
    }
    for (const auto index : source.indices)
        if (index >= source.vertices.size())
            return fail(FarVolumeRenderError::InvalidSource);
    try {
        FarVolumeRenderMesh candidate;
        candidate.request = source.request;
        candidate.origin_meters = source.origin_meters;
        candidate.sampled_max_meters = source.sampled_max_meters;
        candidate.content = source.content;
        candidate.terrain_content = source.content;
        candidate.terrain_indices = source.indices.size();
        if (!World::FarVolumeReserveCharged(candidate.vertices,
                                            source.vertices.size(),
                                            source.vertices.size(),
                                            scope.output()) ||
            !World::FarVolumeReserveCharged(
                candidate.indices, source.indices.size(), source.indices.size(), scope.output()))
            return fail(FarVolumeRenderError::AllocationFailure);
        if (FarVolumeRenderRetainedBytes(candidate) > max_buffer_bytes)
            return fail(FarVolumeRenderError::ByteLimit);
        FarVolumeRenderBounds bounds{Vec3{0.0f}, Vec3{0.0f}};
        bool has_bounds = false;
        for (const auto& vertex : source.vertices) {
            Vec3 position{0.0f}, normal{0.0f};
            if (!FloatVector(vertex.position, position) || !FloatVector(vertex.normal, normal))
                return fail(FarVolumeRenderError::InvalidSource);
            candidate.vertices.push_back({position, normal, vertex.material});
            if (has_bounds) {
                bounds.min = glm::min(bounds.min, position);
                bounds.max = glm::max(bounds.max, position);
            } else {
                bounds = FarVolumeRenderBounds{position, position};
                has_bounds = true;
            }
        }
        if (has_bounds)
            candidate.bounds = bounds;
        candidate.indices.insert(
            candidate.indices.end(), source.indices.begin(), source.indices.end());
        output = std::move(candidate);
        scope.Commit();
        if (error)
            *error = FarVolumeRenderError::None;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(FarVolumeRenderError::AllocationFailure);
    }
}

} // namespace Luminumbra::Rendering
