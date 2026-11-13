#pragma once

#include "Chunk.h"

#include <array>
#include <cmath>
#include <span>

namespace Luminumbra::World {

static_assert(CHUNK_SIZE_X == 16 && CHUNK_SIZE_Y == 16 && CHUNK_SIZE_Z == 16);
inline constexpr std::size_t kNearTerrainDensityCount = 17u * 17u * 17u;

inline bool IsHeightLikeNearTerrainSdf(std::span<const float> densities) {
    if (densities.size() != kNearTerrainDensityCount) {
        return false;
    }
    // One complete 17^3 lattice, X fastest. No scan of live world data.
    for (int z = 0; z < 17; ++z) {
        for (int x = 0; x < 17; ++x) {
            float previous = densities[static_cast<std::size_t>(x + z * 17 * 17)];
            if (!std::isfinite(previous)) {
                return false;
            }
            for (int y = 1; y < 17; ++y) {
                const float current = densities[static_cast<std::size_t>(x + y * 17 + z * 17 * 17)];
                if (!std::isfinite(current) || std::abs((current - previous) - 1.0f) > 0.0001f) {
                    return false;
                }
                previous = current;
            }
        }
    }
    return true;
}

// Render-only immutable evidence carried with one mesh version. Weak ownership
// never keeps chunks resident. Missing evidence refuses; every face, edge and
// corner neighbour participates, including the source chunk at index 13.
struct NearTerrainMeshSnapshot {
    struct Dependency {
        std::weak_ptr<const Chunk> chunk;
        u64 epoch = 0;
    };
    u32 mesh_version = 0;
    int sample_step = 0;
    bool generated_current_params = false;
    bool unedited_halo = false;
    bool complete_height_like_sdf = false;
    std::array<Dependency, 27> halo{};
    // At most 4913 floats, retained only on explicit refinement request.
    std::vector<float> densities;

    [[nodiscard]] bool dependencies_current() const {
        for (const auto& dependency : halo) {
            const auto chunk = dependency.chunk.lock();
            if (!chunk || dependency.epoch == std::numeric_limits<u64>::max() ||
                chunk->near_terrain_epoch.load(std::memory_order_acquire) != dependency.epoch) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool matches(const Chunk& chunk, u32 version) const {
        return version != 0 && mesh_version == version &&
               chunk.mesh_version.load(std::memory_order_acquire) == version &&
               chunk.near_terrain_snapshot.load(std::memory_order_acquire).get() == this &&
               dependencies_current();
    }

    [[nodiscard]] bool
    eligible(const Chunk& chunk, u32 version, float chunk_distance_squared) const {
        return generated_current_params && unedited_halo && complete_height_like_sdf &&
               sample_step == 1 && std::isfinite(chunk_distance_squared) &&
               chunk_distance_squared >= 0.0f && chunk_distance_squared <= 32.0f * 32.0f &&
               matches(chunk, version);
    }
};

inline std::shared_ptr<const NearTerrainMeshSnapshot>
CurrentNearTerrainMeshSnapshot(const Chunk& chunk, u32 version) {
    const auto snapshot = chunk.near_terrain_snapshot.load(std::memory_order_acquire);
    return snapshot && snapshot->matches(chunk, version) ? snapshot : nullptr;
}

// Complete only after meshing has finished reading the lattice. Eligibility
// needs only its classification; refinement can explicitly retain the payload.
// Invalid input never retains an unbounded or partial density payload.
inline std::shared_ptr<const NearTerrainMeshSnapshot>
CompleteNearTerrainMeshSnapshot(const NearTerrainMeshSnapshot& evidence,
                                std::vector<float>& densities,
                                bool retain_densities = false) {
    auto snapshot = std::make_shared<NearTerrainMeshSnapshot>(
        NearTerrainMeshSnapshot{.mesh_version = evidence.mesh_version,
                                .sample_step = evidence.sample_step,
                                .generated_current_params = evidence.generated_current_params,
                                .unedited_halo = evidence.unedited_halo,
                                .complete_height_like_sdf = false,
                                .halo = evidence.halo,
                                .densities = {}});
    snapshot->complete_height_like_sdf = IsHeightLikeNearTerrainSdf(densities);
    if (retain_densities && snapshot->sample_step == 1 && snapshot->generated_current_params &&
        snapshot->unedited_halo && snapshot->complete_height_like_sdf) {
        snapshot->densities = std::move(densities);
    }
    return snapshot;
}

} // namespace Luminumbra::World
