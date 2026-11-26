#pragma once

#include "Chunk.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace Luminumbra::World {
// Requires a complete finite SDF lattice and in-range x/z. Visits at most
// 2 * CHUNK_SIZE_Y endpoints (some inputs are shared by adjacent pairs).
inline float
TerrainCollisionSurface(const Chunk& chunk, std::size_t x, std::size_t z, u64* work = nullptr) {
    constexpr std::size_t kSide = CHUNK_SIZE_X + 1;
    constexpr std::size_t kVertical = CHUNK_SIZE_Y + 1;
    const float base = static_cast<float>(chunk.get_coords().y * CHUNK_SIZE_Y);
    const auto density = [&](std::size_t y) {
        if (work)
            ++*work;
        return chunk.sdf_data[x + y * kSide + z * kSide * kVertical];
    };
    for (std::size_t y = CHUNK_SIZE_Y; y > 0; --y) {
        const float lower = density(y - 1);
        const float upper = density(y);
        if (upper < 0)
            return base + static_cast<float>(y);
        if (lower < 0) {
            // Scale before subtracting: even finite SDF endpoints may overflow.
            const float scale = std::max(-lower, upper);
            const float fraction = (-lower / scale) / (upper / scale - lower / scale);
            return base + static_cast<float>(y - 1) + fraction;
        }
    }
    return base;
}

// Overlay one authoritative slab, highest slabs first. This remains a heightfield:
// caves below the top solid surface and overhang undersides cannot be represented.
// Generated chunks retain their original height samples, including their exact bits.
inline bool OverlayTerrainCollision(const Chunk& chunk, std::span<float> heights) {
    constexpr std::size_t kSide = CHUNK_SIZE_X + 1;
    constexpr std::size_t kVertical = CHUNK_SIZE_Y + 1;
    constexpr std::size_t kSamples = kSide * kVertical * (CHUNK_SIZE_Z + 1);
    if (heights.size() != kSide * (CHUNK_SIZE_Z + 1))
        return false;
    if (!chunk.has_terrain_authority() || chunk.sdf_data.empty())
        return true;
    if (chunk.sdf_data.size() != kSamples ||
        !std::all_of(chunk.sdf_data.begin(), chunk.sdf_data.end(), [](float value) {
            return std::isfinite(value);
        }))
        return false;
    const float base = static_cast<float>(chunk.get_coords().y * CHUNK_SIZE_Y);
    // Fixed 17x17 columns, each visiting at most 17 SDF samples. No allocation.
    for (std::size_t z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (std::size_t x = 0; x < kSide; ++x) {
            auto& height = heights[z * kSide + x];
            const float surface = TerrainCollisionSurface(chunk, x, z);
            if (height >= base && height <= base + CHUNK_SIZE_Y)
                height = surface;
            else if (surface > base)
                height = std::max(height, surface);
        }
    }
    return true;
}
} // namespace Luminumbra::World
