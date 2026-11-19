#pragma once

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"

#include <cmath>

namespace Luminumbra::Rendering {

// Owner-thread presentation query. Unknown/unstreamed support retains the
// generated dressing. Only a complete, authoritative resident SDF can remove it.
// Eight lattice reads per anchor; no allocation, surface search or world scan.
inline bool EditedTerrainRemovesAnchor(const Chunk* chunk, const Vec3& position) {
    constexpr int side = CHUNK_SIZE_X + 1;
    constexpr std::size_t volume = static_cast<std::size_t>(side) * side * side;
    static_assert(CHUNK_SIZE_X == CHUNK_SIZE_Y && CHUNK_SIZE_Y == CHUNK_SIZE_Z);
    if (!chunk || !chunk->has_terrain_authority() || chunk->sdf_data.size() != volume) {
        return false;
    }
    const Vec3 local = position - Vec3(chunk->get_coords() * CHUNK_SIZE_X);
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(local[axis]) || local[axis] < 0.0f ||
            local[axis] >= static_cast<float>(CHUNK_SIZE_X)) {
            return false;
        }
    }
    const IVec3 cell(glm::floor(local));
    const Vec3 fraction = local - Vec3(cell);
    float density = 0.0f;
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                const auto index = static_cast<std::size_t>(cell.x + x + (cell.y + y) * side +
                                                            (cell.z + z) * side * side);
                const float sample = chunk->sdf_data[index];
                if (!std::isfinite(sample)) {
                    return false;
                }
                const float weight = (x == 0 ? 1.0f - fraction.x : fraction.x) *
                                     (y == 0 ? 1.0f - fraction.y : fraction.y) *
                                     (z == 0 ? 1.0f - fraction.z : fraction.z);
                density += sample * weight;
            }
        }
    }
    // Half a voxel of air is enough to distinguish excavation from surface
    // interpolation error. Solid, refilled and untouched anchors stay visible.
    return density > 0.5f;
}

// Chunk that owns an anchor, or false when the position is non-finite or outside
// the packed coordinate range (such anchors are never removed).
inline bool AnchorChunkCoords(const Vec3& position, IVec3& coords) {
    for (int axis = 0; axis < 3; ++axis) {
        const double coordinate = std::floor(static_cast<double>(position[axis]) / CHUNK_SIZE_X);
        const int minimum = axis == 1 ? Chunk::kPackedMinY : Chunk::kPackedMinXz;
        const int maximum = axis == 1 ? Chunk::kPackedMaxY : Chunk::kPackedMaxXz;
        if (!std::isfinite(coordinate) || coordinate < minimum || coordinate > maximum) {
            return false;
        }
        coords[axis] = static_cast<int>(coordinate);
    }
    return true;
}

inline bool TerrainRemovesAnchor(const Systems::SHIELD_WorldSystem& world, const Vec3& position) {
    IVec3 coords{};
    if (!AnchorChunkCoords(position, coords)) {
        return false;
    }
    const auto chunk = world.find_streamed_chunk(coords);
    return EditedTerrainRemovesAnchor(chunk.get(), position);
}

} // namespace Luminumbra::Rendering
