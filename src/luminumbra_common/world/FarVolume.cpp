#include "FarVolume.h"

#include "FarLodStore.h"
#include "core/Crc32.h"
#include "systems/SHIELD_WorldSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Luminumbra::World {
namespace {
constexpr std::int64_t kCoordinateLimit =
    kFarVolumeExactCoordinateLimitMeters; // exact integer float coordinates + filter taps
constexpr std::size_t kTileSide = 129;
constexpr std::size_t kTileColumns = kTileSide * kTileSide;

void Mix(Core::Crc32Accumulator& crc, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        const auto byte = static_cast<std::uint8_t>(value >> (8 * i));
        crc.Update(&byte, 1);
    }
}
void MixBrick(Core::Crc32Accumulator& crc, const FarVolumeBrick& brick) {
    for (auto v : {brick.key.x, brick.key.y, brick.key.z})
        Mix(crc, static_cast<std::uint32_t>(v), 4);
    for (const auto& sample : brick.samples) {
        Mix(crc, static_cast<std::uint16_t>(sample.density), 2);
        Mix(crc, sample.material, 1);
    }
}
FarTierDimensions Dimensions(std::uint32_t tier, FarCaveMode mode) {
    const auto dimensions = FarTierAt(tier);
    if (!dimensions || (mode != FarCaveMode::BandLimited && mode != FarCaveMode::BoxFiltered))
        throw std::invalid_argument("Invalid far-volume tier or cave mode");
    return *dimensions;
}
void Coordinate(std::int64_t position) {
    if (position <= -kCoordinateLimit || position >= kCoordinateLimit)
        throw std::invalid_argument("Far-volume coordinate exceeds exact sampling range");
}
std::int64_t TileOrigin(std::int32_t tile, std::uint32_t edge) {
    const auto origin = static_cast<std::int64_t>(tile) * edge;
    Coordinate(origin);
    Coordinate(origin + edge);
    return origin;
}
std::uint64_t CandidateCount(std::int32_t first, std::int32_t last, std::uint32_t edge) {
    if (first >= last)
        throw std::invalid_argument("Empty or reversed far-volume span");
    Coordinate(static_cast<std::int64_t>(first) * edge);
    Coordinate(static_cast<std::int64_t>(last) * edge);
    return 1024ull * static_cast<std::uint64_t>(static_cast<std::int64_t>(last) - first);
}
bool CrossesZero(const FarVolumeBrick& brick) {
    bool solid = false, air = false;
    for (const auto& sample : brick.samples) {
        if (sample.density == kFarLodSdfInvalid)
            throw std::invalid_argument("Invalid far-volume density sentinel");
        solid |= sample.density < 0;
        air |= sample.density >= 0;
    }
    return solid && air;
}
void CheckLimits(std::uint64_t candidates, const FarVolumeLimits& limits) {
    const std::uint64_t samples = kTileColumns * (candidates / 1024 * 4 + 1);
    if (samples > limits.max_density_samples)
        throw std::length_error("Far-volume density sample budget exceeded");
    const auto reserved = std::min<std::uint64_t>(candidates, limits.max_bricks);
    const std::uint64_t bytes = samples * sizeof(FarVolumeSample) + kTileColumns * sizeof(float) +
                                reserved * sizeof(FarVolumeBrick);
    if (bytes > limits.max_buffer_bytes)
        throw std::length_error("Far-volume sample/brick buffer budget exceeded");
}

constexpr std::int64_t kWindowCoordinateLimit = kCoordinateLimit - 1024;

void ValidateExtraSpan(const std::optional<FarVolumeSpan>& extra) {
    if (extra && (!std::isfinite(extra->min_y) || !std::isfinite(extra->max_y) ||
                  extra->min_y > extra->max_y))
        throw std::invalid_argument("Invalid far-volume requested span");
}
struct SurfaceGrid {
    std::vector<float> heights;
    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
};
SurfaceGrid SampleSurface(FarTierDimensions d,
                          std::int64_t ox,
                          std::int64_t oz,
                          const std::function<float(float, float)>& height,
                          const FarVolumeLimits& limits) {
    if (kTileColumns > limits.max_density_samples ||
        kTileColumns * sizeof(float) > limits.max_buffer_bytes)
        throw std::length_error("Far-volume surface sampling budget exceeded");
    SurfaceGrid grid;
    grid.heights.resize(kTileColumns);
    for (std::size_t z = 0; z < kTileSide; ++z)
        for (std::size_t x = 0; x < kTileSide; ++x) {
            const float h = height(
                static_cast<float>(ox + static_cast<std::int64_t>(x) * d.sample_spacing_meters),
                static_cast<float>(oz + static_cast<std::int64_t>(z) * d.sample_spacing_meters));
            if (!std::isfinite(h) || h <= -kCoordinateLimit + 1024 || h >= kCoordinateLimit - 1024)
                throw std::invalid_argument("Invalid far-volume surface height");
            grid.heights[x + z * kTileSide] = h;
            grid.low = std::min(grid.low, h);
            grid.high = std::max(grid.high, h);
        }
    return grid;
}
FarVolumeYWindow SurfaceWindow(const SurfaceGrid& grid,
                               const std::optional<FarVolumeSpan>& extra,
                               std::uint32_t edge) {
    float low = grid.low, high = grid.high;
    low -= kFarVolumeInteriorDepthMeters;
    if (extra) {
        low = std::min(low, extra->min_y);
        high = std::max(high, extra->max_y);
    }
    if (low <= -kCoordinateLimit + 1024 || high >= kCoordinateLimit - 1024)
        throw std::invalid_argument("Far-volume span exceeds exact sampling range");
    const auto first = static_cast<std::int32_t>(std::floor(static_cast<double>(low) / edge));
    // Include an air sample above a surface exactly on a brick plane.
    const auto last = static_cast<std::int32_t>(std::floor(static_cast<double>(high) / edge)) + 1;
    return {static_cast<std::int64_t>(first) * edge, static_cast<std::int64_t>(last) * edge};
}
FarTierDimensions WindowDimensions(const FarVolumeWindowRequest& request) {
    const auto d = Dimensions(request.tier, request.caves);
    TileOrigin(request.tile_x, d.tile_edge_meters);
    TileOrigin(request.tile_z, d.tile_edge_meters);
    const auto [first, last] = request.window;
    // Bound before subtraction/conversion, including INT64_MIN/MAX requests.
    if (first >= last || first < -kWindowCoordinateLimit || last > kWindowCoordinateLimit ||
        first % d.brick_edge_meters != 0 || last % d.brick_edge_meters != 0)
        throw std::invalid_argument("Invalid aligned far-volume window");
    return d;
}
void CheckWindowAllocation(std::uint64_t candidates, const FarVolumeLimits& limits) {
    CheckLimits(candidates, limits);
    const auto samples = kTileColumns * (candidates / 1024 * 4 + 1);
    const auto reserved = std::min<std::uint64_t>(candidates, limits.max_bricks);
    if (samples > std::vector<FarVolumeSample>().max_size() ||
        reserved > std::vector<FarVolumeBrick>().max_size())
        throw std::length_error("Far-volume window exceeds container capacity");
}
bool AdmitsAllCandidates(std::uint32_t layers, const FarVolumeLimits& limits) {
    // layers is bounded by the legal coordinate span before entering this helper.
    const std::uint64_t candidates = 1024ull * layers;
    const std::uint64_t samples = kTileColumns * (4ull * layers + 1);
    if (candidates > limits.max_bricks || samples > limits.max_density_samples ||
        samples > std::vector<FarVolumeSample>().max_size() ||
        candidates > std::vector<FarVolumeBrick>().max_size())
        return false;
    const auto heights = kTileColumns * sizeof(float);
    if (heights > limits.max_buffer_bytes)
        return false;
    auto remaining = limits.max_buffer_bytes - heights;
    if (samples > remaining / sizeof(FarVolumeSample))
        return false;
    remaining -= static_cast<std::size_t>(samples) * sizeof(FarVolumeSample);
    return candidates <= remaining / sizeof(FarVolumeBrick);
}
std::int64_t FloorDivide(std::int64_t value, std::uint32_t divisor) {
    const auto quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
}
FarVolumeTile BuildWindowSamples(const FarVolumeWindowRequest& request,
                                 const std::vector<float>& heights,
                                 const FarVolumeSamplers& samplers,
                                 const FarVolumeLimits& limits) {
    const auto d = Dimensions(request.tier, request.caves);
    const auto ox = TileOrigin(request.tile_x, d.tile_edge_meters);
    const auto oz = TileOrigin(request.tile_z, d.tile_edge_meters);
    const auto first = static_cast<std::int32_t>(request.window.min_y_meters / d.brick_edge_meters);
    const auto last = static_cast<std::int32_t>(request.window.max_y_meters / d.brick_edge_meters);
    const auto candidates = CandidateCount(first, last, d.brick_edge_meters);
    CheckLimits(candidates, limits);
    const auto ny = static_cast<std::size_t>((static_cast<std::int64_t>(last) - first) * 4 + 1);
    const auto grid_index = [ny](std::size_t x, std::size_t y, std::size_t z) {
        return x + kTileSide * (y + ny * z);
    };
    std::vector<FarVolumeSample> lattice(kTileColumns * ny);
    for (std::size_t z = 0; z < kTileSide; ++z)
        for (std::size_t y = 0; y < ny; ++y)
            for (std::size_t x = 0; x < kTileSide; ++x) {
                const Vec3 position(
                    static_cast<float>(ox + static_cast<std::int64_t>(x) * d.sample_spacing_meters),
                    static_cast<float>(static_cast<std::int64_t>(first) * d.brick_edge_meters +
                                       static_cast<std::int64_t>(y) * d.sample_spacing_meters),
                    static_cast<float>(oz +
                                       static_cast<std::int64_t>(z) * d.sample_spacing_meters));
                const auto sample = samplers.density(position, heights[x + z * kTileSide]);
                const auto q = QuantizeFarLodSdf(sample.density);
                if (q == kFarLodSdfInvalid)
                    throw std::invalid_argument("Non-finite far-volume density");
                lattice[grid_index(x, y, z)] = {q, sample.material};
            }
    FarVolumeTile tile{request.tier,
                       request.tile_x,
                       request.tile_z,
                       request.caves,
                       first,
                       last,
                       candidates,
                       {},
                       0};
    tile.bricks.reserve(
        static_cast<std::size_t>(std::min<std::uint64_t>(candidates, limits.max_bricks)));
    for (int x = 0; x < 32; ++x)
        for (std::int32_t by = first; by < last; ++by)
            for (int z = 0; z < 32; ++z) {
                FarVolumeBrick brick;
                brick.key = {request.tile_x * 32 + x, by, request.tile_z * 32 + z};
                for (std::size_t sz = 0; sz < 5; ++sz)
                    for (std::size_t sy = 0; sy < 5; ++sy)
                        for (std::size_t sx = 0; sx < 5; ++sx)
                            brick.samples[FarVolumeSampleIndex(sx, sy, sz)] =
                                lattice[grid_index(x * 4 + sx,
                                                   static_cast<std::size_t>(by - first) * 4 + sy,
                                                   z * 4 + sz)];
                if (!CrossesZero(brick))
                    continue;
                if (tile.bricks.size() == limits.max_bricks)
                    throw std::length_error("Far-volume brick budget exceeded");
                brick.crc32 = FarVolumeBrickCrc(brick);
                tile.bricks.push_back(brick);
            }
    tile.crc32 = FarVolumeTileCrc(tile);
    return tile;
}

} // namespace

float BoxFilterFarVolumeCarve(const Vec3& position,
                              std::uint32_t spacing,
                              const std::function<float(const Vec3&)>& carve) {
    if ((spacing != 16 && spacing != 32 && spacing != 64) || !carve)
        throw std::invalid_argument("Invalid far-volume box filter");
    for (float component : {position.x, position.y, position.z})
        if (!std::isfinite(component) || std::abs(component) >= kCoordinateLimit - 64)
            throw std::invalid_argument("Invalid far-volume filter position");
    double sum = 0.0;
    const float offset = static_cast<float>(spacing) * 0.25f;
    for (int z : {-1, 1})
        for (int y : {-1, 1})
            for (int x : {-1, 1}) {
                const float value = carve(position + Vec3(static_cast<float>(x) * offset,
                                                          static_cast<float>(y) * offset,
                                                          static_cast<float>(z) * offset));
                if (!std::isfinite(value) || value < 0.0f)
                    throw std::invalid_argument("Invalid far-volume carve contribution");
                sum += value;
            }
    return static_cast<float>(sum * 0.125);
}

std::uint32_t FarVolumeBrickCrc(const FarVolumeBrick& brick) {
    Core::Crc32Accumulator crc;
    MixBrick(crc, brick);
    return crc.Value();
}
std::uint32_t FarVolumeTileCrc(const FarVolumeTile& tile) {
    Core::Crc32Accumulator crc;
    Mix(crc, tile.tier, 4);
    Mix(crc, static_cast<std::uint32_t>(tile.tile_x), 4);
    Mix(crc, static_cast<std::uint32_t>(tile.tile_z), 4);
    Mix(crc, static_cast<std::uint8_t>(tile.caves), 1);
    Mix(crc, static_cast<std::uint32_t>(tile.first_brick_y), 4);
    Mix(crc, static_cast<std::uint32_t>(tile.last_brick_y), 4);
    Mix(crc, tile.sampled_bricks, 8);
    Mix(crc, tile.bricks.size(), 8);
    for (const auto& brick : tile.bricks) {
        MixBrick(crc, brick);
        Mix(crc, brick.crc32, 4);
    }
    return crc.Value();
}

void ValidateFarVolumeTile(const FarVolumeTile& tile, const FarVolumeLimits& limits) {
    const auto d = Dimensions(tile.tier, tile.caves);
    TileOrigin(tile.tile_x, d.tile_edge_meters);
    TileOrigin(tile.tile_z, d.tile_edge_meters);
    const auto candidates =
        CandidateCount(tile.first_brick_y, tile.last_brick_y, d.brick_edge_meters);
    CheckLimits(candidates, limits);
    if (tile.sampled_bricks != candidates || tile.bricks.size() > candidates)
        throw std::invalid_argument("Incomplete far-volume discovery");
    if (tile.bricks.size() > limits.max_bricks)
        throw std::length_error("Far-volume brick budget exceeded");
    if (tile.crc32 != FarVolumeTileCrc(tile))
        throw std::invalid_argument("Far-volume tile CRC mismatch");
    for (std::size_t i = 0; i < tile.bricks.size(); ++i) {
        const auto& brick = tile.bricks[i];
        const auto lx =
            static_cast<std::int64_t>(brick.key.x) - static_cast<std::int64_t>(tile.tile_x) * 32;
        const auto lz =
            static_cast<std::int64_t>(brick.key.z) - static_cast<std::int64_t>(tile.tile_z) * 32;
        if (lx < 0 || lx >= 32 || lz < 0 || lz >= 32 || brick.key.y < tile.first_brick_y ||
            brick.key.y >= tile.last_brick_y || (i && !(tile.bricks[i - 1].key < brick.key)))
            throw std::invalid_argument("Invalid far-volume brick identity/order");
        if (!CrossesZero(brick) || brick.crc32 != FarVolumeBrickCrc(brick))
            throw std::invalid_argument("Far-volume brick CRC or crossing mismatch");
    }
    // Validate the entire ordering before any binary search, including corrupt input.
    for (const auto& brick : tile.bricks) {
        // Check faces, edges AND corners shared by any two retained bricks.
        // No halo or homogeneous absent brick is required by a sparse stream.
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    FarVolumeBrickKey key{brick.key.x + dx, brick.key.y + dy, brick.key.z + dz};
                    if (!(brick.key < key))
                        continue;
                    const auto other = std::lower_bound(
                        tile.bricks.begin(),
                        tile.bricks.end(),
                        key,
                        [](const FarVolumeBrick& a, FarVolumeBrickKey b) { return a.key < b; });
                    if (other == tile.bricks.end() || other->key != key)
                        continue;
                    for (int z = std::max(0, dz * 4); z <= std::min(4, dz * 4 + 4); ++z)
                        for (int y = std::max(0, dy * 4); y <= std::min(4, dy * 4 + 4); ++y)
                            for (int x = std::max(0, dx * 4); x <= std::min(4, dx * 4 + 4); ++x)
                                if (!(brick.samples[FarVolumeSampleIndex(x, y, z)] ==
                                      other->samples[FarVolumeSampleIndex(
                                          x - dx * 4, y - dy * 4, z - dz * 4)]))
                                    throw std::invalid_argument(
                                        "Inconsistent shared far-volume sample");
                }
    }
}

FarVolumeTile BuildFarVolumeTile(const FarVolumeRequest& request,
                                 const FarVolumeSamplers& samplers,
                                 const FarVolumeLimits& limits) {
    const auto d = Dimensions(request.tier, request.caves);
    const auto ox = TileOrigin(request.tile_x, d.tile_edge_meters);
    const auto oz = TileOrigin(request.tile_z, d.tile_edge_meters);
    if (!samplers.height || !samplers.density)
        throw std::invalid_argument("Missing far-volume sampler");
    ValidateExtraSpan(request.extra_span);
    const auto surface = SampleSurface(d, ox, oz, samplers.height, limits);
    const auto window = SurfaceWindow(surface, request.extra_span, d.brick_edge_meters);
    return BuildWindowSamples({request.tier, request.tile_x, request.tile_z, request.caves, window},
                              surface.heights,
                              samplers,
                              limits);
}

FarVolumeTile BuildPristineFarVolumeTile(const Systems::SHIELD_WorldSystem& world,
                                         const FarVolumeRequest& request,
                                         const FarVolumeLimits& limits) {
    const auto d = Dimensions(request.tier, request.caves);
    return BuildFarVolumeTile(
        request,
        {[&world](float x, float z) { return world.GetTerrainHeightAt(x, z); },
         [&world, &request, d](const Vec3& p, float height) {
             const auto material = p.y < height - 4.0f
                                       ? MaterialType::Stone
                                       : world.SurfaceVertexMaterial(p.x, p.z, height);
             return FarVolumeDensity{
                 world.SamplePristineFarDensity(p, height, d.sample_spacing_meters, request.caves),
                 static_cast<std::uint8_t>(material)};
         }},
        limits);
}

FarVolumeWindowRequest DiscoverFarVolumeWindow(const FarVolumeRequest& request,
                                               const std::function<float(float, float)>& height,
                                               const FarVolumeLimits& limits) {
    const auto d = Dimensions(request.tier, request.caves);
    const auto ox = TileOrigin(request.tile_x, d.tile_edge_meters);
    const auto oz = TileOrigin(request.tile_z, d.tile_edge_meters);
    if (!height)
        throw std::invalid_argument("Missing far-volume sampler");
    ValidateExtraSpan(request.extra_span);
    const auto surface = SampleSurface(d, ox, oz, height, limits);
    return {request.tier,
            request.tile_x,
            request.tile_z,
            request.caves,
            SurfaceWindow(surface, request.extra_span, d.brick_edge_meters)};
}
FarVolumeWindowRequest DiscoverPristineFarVolumeWindow(const Systems::SHIELD_WorldSystem& world,
                                                       const FarVolumeRequest& request,
                                                       const FarVolumeLimits& limits) {
    return DiscoverFarVolumeWindow(
        request, [&world](float x, float z) { return world.GetTerrainHeightAt(x, z); }, limits);
}
FarVolumeWindowPlan PlanFarVolumeWindows(const FarVolumeWindowRequest& coverage,
                                         const FarVolumeLimits& limits,
                                         std::uint32_t max_windows) {
    const auto d = WindowDimensions(coverage);
    if (max_windows == 0)
        throw std::invalid_argument("Far-volume window count limit must be positive");
    std::uint32_t lower = 0;
    auto upper = static_cast<std::uint32_t>(2 * kWindowCoordinateLimit / d.brick_edge_meters);
    while (lower < upper) {
        const auto middle = lower + (upper - lower + 1) / 2;
        if (AdmitsAllCandidates(middle, limits))
            lower = middle;
        else
            upper = middle - 1;
    }
    if (lower == 0)
        throw std::length_error("Far-volume limits cannot admit one complete window layer");
    const auto first = coverage.window.min_y_meters / d.brick_edge_meters;
    const auto last = coverage.window.max_y_meters / d.brick_edge_meters;
    const auto count = FloorDivide(last - 1, lower) - FloorDivide(first, lower) + 1;
    if (count > max_windows)
        throw std::length_error("Far-volume window count budget exceeded");
    FarVolumeWindowPlan plan;
    plan.m_coverage = coverage;
    plan.m_stride = lower;
    plan.m_count = static_cast<std::uint32_t>(count);
    const auto layers = static_cast<std::uint64_t>(last - first);
    plan.m_bricks = 1024ull * layers;
    plan.m_samples = kTileColumns * (4 * layers + plan.m_count);
    return plan;
}
FarVolumeWindowRequest FarVolumeWindowAt(const FarVolumeWindowPlan& plan, std::uint32_t index) {
    if (index >= plan.WindowCount())
        throw std::invalid_argument("Far-volume window index is outside the plan");
    auto request = plan.Coverage();
    const auto d = Dimensions(request.tier, request.caves);
    const auto first = request.window.min_y_meters / d.brick_edge_meters;
    const auto first_page = FloorDivide(first, plan.LayerStride());
    // All intermediates fit int64 by the coordinate-domain and layer-stride
    // bounds established at construction; there is no mutable public plan state.
    const auto page = first_page + index;
    request.window.min_y_meters =
        std::max(request.window.min_y_meters, page * plan.LayerStride() * d.brick_edge_meters);
    request.window.max_y_meters = std::min(request.window.max_y_meters,
                                           (page + 1) * plan.LayerStride() * d.brick_edge_meters);
    return request;
}
FarVolumeTile BuildFarVolumeWindow(const FarVolumeWindowRequest& request,
                                   const FarVolumeSamplers& samplers,
                                   const FarVolumeLimits& limits) {
    const auto d = WindowDimensions(request);
    if (!samplers.height || !samplers.density)
        throw std::invalid_argument("Missing far-volume sampler");
    const auto layers = static_cast<std::uint64_t>(
        (request.window.max_y_meters - request.window.min_y_meters) / d.brick_edge_meters);
    CheckWindowAllocation(1024ull * layers, limits);
    const auto surface = SampleSurface(d,
                                       TileOrigin(request.tile_x, d.tile_edge_meters),
                                       TileOrigin(request.tile_z, d.tile_edge_meters),
                                       samplers.height,
                                       limits);
    return BuildWindowSamples(request, surface.heights, samplers, limits);
}
FarVolumeTile BuildPristineFarVolumeWindow(const Systems::SHIELD_WorldSystem& world,
                                           const FarVolumeWindowRequest& request,
                                           const FarVolumeLimits& limits) {
    const auto d = WindowDimensions(request);
    return BuildFarVolumeWindow(
        request,
        {[&world](float x, float z) { return world.GetTerrainHeightAt(x, z); },
         [&world, &request, d](const Vec3& p, float height) {
             const auto material = p.y < height - 4.0f
                                       ? MaterialType::Stone
                                       : world.SurfaceVertexMaterial(p.x, p.z, height);
             return FarVolumeDensity{
                 world.SamplePristineFarDensity(p, height, d.sample_spacing_meters, request.caves),
                 static_cast<std::uint8_t>(material)};
         }},
        limits);
}
} // namespace Luminumbra::World
