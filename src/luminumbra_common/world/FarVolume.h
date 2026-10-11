#pragma once

#include "FarTierTable.h"
#include "luminumbra/core/Types.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
}

namespace Luminumbra::World {

// Both choices leave V1/V2 unchanged. BandLimited retains filtered noise at V3
// and only analytic openings beyond it; BoxFiltered retains noise at every tier.
// This is an in-memory experiment, not a persisted or runtime-selected format.
enum class FarCaveMode : std::uint8_t {
    BandLimited,
    BoxFiltered
};
inline constexpr std::size_t kFarVolumeBrickSide = 5;
inline constexpr std::size_t kFarVolumeBrickSamples = 125;
inline constexpr float kFarVolumeInteriorDepthMeters = 256.0f;
inline constexpr std::int64_t kFarVolumeExactCoordinateLimitMeters = 1 << 23;

struct FarVolumeSpan {
    float min_y, max_y;
};
struct FarVolumeRequest {
    std::uint32_t tier = 1;
    std::int32_t tile_x = 0, tile_z = 0;
    // Extends the sampled surface band, never replaces or truncates it.
    std::optional<FarVolumeSpan> extra_span;
    FarCaveMode caves = FarCaveMode::BandLimited;
};
struct FarVolumeLimits {
    std::uint64_t max_density_samples = 8'000'000;
    std::size_t max_bricks = 65'536;
    std::size_t max_buffer_bytes = 128u * 1024u * 1024u;
};
struct FarVolumeSample {
    std::int16_t density = 0;
    std::uint8_t material = 0;
    bool operator==(const FarVolumeSample&) const = default;
};
struct FarVolumeBrickKey {
    std::int32_t x = 0, y = 0, z = 0; // global brick indices, including signed Y
    auto operator<=>(const FarVolumeBrickKey&) const = default;
};
struct FarVolumeBrick {
    FarVolumeBrickKey key;
    // X fastest, then Y, then Z. Negative density is solid; zero is air.
    std::array<FarVolumeSample, kFarVolumeBrickSamples> samples{};
    std::uint32_t crc32 = 0;
};
struct FarVolumeTile {
    std::uint32_t tier = 1;
    std::int32_t tile_x = 0, tile_z = 0;
    FarCaveMode caves = FarCaveMode::BandLimited;
    // Entire scanned interval [first,last), in global brick Y coordinates.
    std::int32_t first_brick_y = 0, last_brick_y = 0;
    std::uint64_t sampled_bricks = 0;
    std::vector<FarVolumeBrick> bricks; // sorted by signed (X,Y,Z), zero crossings only
    std::uint32_t crc32 = 0;
};
struct FarVolumeDensity {
    float density;
    std::uint8_t material;
};
struct FarVolumeSamplers {
    std::function<float(float, float)> height;
    std::function<FarVolumeDensity(const Vec3&, float)> density;
};

constexpr std::size_t FarVolumeSampleIndex(std::size_t x, std::size_t y, std::size_t z) {
    return x + kFarVolumeBrickSide * (y + kFarVolumeBrickSide * z);
}
// Eight equally weighted midpoint taps, accumulated in double in Z/Y/X order.
// A finite quadrature kernel, not an ideal spectral low-pass.
float BoxFilterFarVolumeCarve(const Vec3& position,
                              std::uint32_t spacing,
                              const std::function<float(const Vec3&)>& carve);
std::uint32_t FarVolumeBrickCrc(const FarVolumeBrick& brick);
std::uint32_t FarVolumeTileCrc(const FarVolumeTile& tile);
// Throws on invalid/corrupt/inconsistent streams; no background or invented halo.
void ValidateFarVolumeTile(const FarVolumeTile& tile, const FarVolumeLimits& limits = {});
// Bounds are checked before sample-buffer allocation. Exceeding any limit or
// receiving a non-finite sample throws; no incomplete result is returned.
FarVolumeTile BuildFarVolumeTile(const FarVolumeRequest& request,
                                 const FarVolumeSamplers& samplers,
                                 const FarVolumeLimits& limits = {});
FarVolumeTile BuildPristineFarVolumeTile(const Systems::SHIELD_WorldSystem& world,
                                         const FarVolumeRequest& request,
                                         const FarVolumeLimits& limits = {});

// Geometry only: normals, upload, ownership transitions and runtime shading are
// separate integration work. Absolute positions make shared edges bit-identical.
struct FarVolumeVertex {
    Vec3 position;
    std::uint8_t material;
};
struct FarVolumeMesh {
    std::vector<FarVolumeVertex> vertices;
    std::vector<std::uint32_t> indices;
};
struct FarVolumeMeshLimits {
    std::size_t max_vertices = 2'000'000;
    std::size_t max_indices = 6'000'000;
    std::size_t max_buffer_bytes = 64u * 1024u * 1024u;
};

} // namespace Luminumbra::World
