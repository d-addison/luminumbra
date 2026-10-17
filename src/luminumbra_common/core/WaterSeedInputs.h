#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Luminumbra {

struct WaterSeedGeneration {
    std::uint64_t terrain = 0;
    std::uint64_t edit = 0;
};

inline bool operator==(const WaterSeedGeneration& lhs, const WaterSeedGeneration& rhs) {
    return lhs.terrain == rhs.terrain && lhs.edit == rhs.edit;
}

// Derived inputs only, never hashed or persisted. One payload per pending column,
// bounded to High's 256 cells (two 1 KiB arrays); no resolution-sized allocation.
// The main thread owns the chunk's shared_ptr; a worker retains its own copy.
// After ready is released, both arrays and the generation are immutable. Dropping
// the main-thread copy on fallback lets a late worker release its storage safely.
struct HighWaterSeedInputs {
    static constexpr int kResolution = 16;
    static constexpr std::size_t kCells = 256;
    WaterSeedGeneration generation{};
    std::array<float, kCells> terrain{};
    std::array<float, kCells> rest{};
    std::atomic<bool> ready{false};
};

// Call under the world's sampling scope. Keep the inline path's exact float
// operations and z-major/x-minor query order; no heightmap interpolation.
template<typename World>
void ComputeWaterSeedInputs(HighWaterSeedInputs& inputs,
                            const IVec3& coords,
                            int resolution,
                            const World& world,
                            std::span<const float> terrain = {}) {
    // Session resolutions divide the 16 m chunk: at most 256 cells. Refuse
    // unsupported sizes before indexing either fixed array or supplied terrain.
    if (resolution <= 1 || resolution > HighWaterSeedInputs::kResolution ||
        CHUNK_SIZE_X % resolution != 0 || CHUNK_SIZE_Z % resolution != 0)
        return;
    const auto cells = static_cast<std::size_t>(resolution) * resolution;
    if (!terrain.empty() && terrain.size() != cells)
        return;
    const float cell_width_x = CHUNK_SIZE_X / static_cast<float>(resolution);
    const float cell_width_z = CHUNK_SIZE_Z / static_cast<float>(resolution);
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            const float world_x = static_cast<float>(coords.x * CHUNK_SIZE_X) +
                                  (static_cast<float>(x) + 0.5f) * cell_width_x;
            const float world_z = static_cast<float>(coords.z * CHUNK_SIZE_Z) +
                                  (static_cast<float>(z) + 0.5f) * cell_width_z;
            const auto cell =
                static_cast<std::size_t>(z) * resolution + static_cast<std::size_t>(x);
            inputs.rest[cell] = world.WaterLevelAt(world_x, world_z);
            inputs.terrain[cell] =
                terrain.empty() ? world.GetTerrainHeightAt(world_x, world_z) : terrain[cell];
        }
    }
    inputs.ready.store(true, std::memory_order_release);
}

template<typename World>
void ComputeHighWaterSeedInputs(HighWaterSeedInputs& inputs,
                                const IVec3& coords,
                                const World& world) {
    ComputeWaterSeedInputs(inputs, coords, HighWaterSeedInputs::kResolution, world);
}

} // namespace Luminumbra
