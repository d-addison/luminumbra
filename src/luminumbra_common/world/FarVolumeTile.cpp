#include "FarVolumeTile.h"

#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace Luminumbra::World {
namespace {
bool tile_bounds(std::int64_t tile, std::int64_t edge, std::int64_t& lo, std::int64_t& hi) {
    // Every table edge is a power of two; INT64_MIN is divisible by it.
    if (tile < std::numeric_limits<std::int64_t>::min() / edge ||
        tile > (std::numeric_limits<std::int64_t>::max() - edge) / edge) {
        return false;
    }
    lo = tile * edge;
    hi = lo + edge;
    return true;
}
} // namespace

bool BuildFarVolumeTile(const FarVolumeRequest& request,
                        const FarVolumeBuildLimits& limits,
                        const FarVolumeSampler& sampler,
                        FarVolumeTile& output,
                        FarVolumeBuildError* error) {
    const auto fail = [error](FarVolumeBuildError value) {
        if (error)
            *error = value;
        return false;
    };
    const auto dimensions = FarTierAt(request.key.tier);
    if (!dimensions)
        return fail(FarVolumeBuildError::InvalidTier);
    const std::int64_t edge = dimensions->brick_edge_meters;
    const std::int64_t tile_edge = dimensions->tile_edge_meters;
    const std::int64_t step = dimensions->sample_spacing_meters;
    if (request.span.min_y >= request.span.max_y || request.span.min_y % edge != 0 ||
        request.span.max_y % edge != 0) {
        return fail(FarVolumeBuildError::InvalidSpan);
    }
    FarVolumeTile tile;
    tile.request = request;
    tile.min_meters.y = request.span.min_y;
    tile.max_meters.y = request.span.max_y;
    if (!tile_bounds(request.key.x, tile_edge, tile.min_meters.x, tile.max_meters.x) ||
        !tile_bounds(request.key.z, tile_edge, tile.min_meters.z, tile.max_meters.z)) {
        return fail(FarVolumeBuildError::CoordinateOverflow);
    }
    // Divide before subtracting: metre span may overflow int64, but aligned
    // brick indices are at most +/-2^59 (the smallest brick edge is 16 m).
    const auto layers =
        static_cast<std::uint64_t>(request.span.max_y / edge - request.span.min_y / edge);
    const auto columns =
        static_cast<std::uint64_t>(tile_edge / edge) * static_cast<std::uint64_t>(tile_edge / edge);
    if (layers > limits.max_candidate_bricks / columns ||
        layers > std::numeric_limits<std::uint64_t>::max() / columns / kFarVolumeBrickSamples) {
        return fail(FarVolumeBuildError::WorkLimit);
    }
    if (!sampler)
        return fail(FarVolumeBuildError::InvalidSampler);

    bool solid = false, air = false;
    try {
        for (std::int64_t z = tile.min_meters.z; z < tile.max_meters.z; z += edge) {
            for (std::int64_t x = tile.min_meters.x; x < tile.max_meters.x; x += edge) {
                for (std::int64_t y = request.span.min_y; y < request.span.max_y; y += edge) {
                    FarVolumeBrick brick;
                    brick.origin_meters = {x, y, z};
                    bool brick_solid = false, brick_air = false;
                    std::size_t index = 0;
                    for (std::int64_t dz = 0; dz <= edge; dz += step) {
                        for (std::int64_t dy = 0; dy <= edge; dy += step) {
                            for (std::int64_t dx = 0; dx <= edge; dx += step) {
                                const auto value = sampler({x + dx, y + dy, z + dz});
                                if (!value)
                                    return fail(FarVolumeBuildError::SamplerFailure);
                                if (!std::isfinite(value->density))
                                    return fail(FarVolumeBuildError::NonFiniteDensity);
                                brick.samples[index++] = *value;
                                brick_solid = brick_solid || value->density < 0.0f;
                                brick_air = brick_air || value->density >= 0.0f;
                            }
                        }
                    }
                    ++tile.candidates_visited;
                    tile.samples_evaluated += kFarVolumeBrickSamples;
                    solid = solid || brick_solid;
                    air = air || brick_air;
                    if (brick_solid && brick_air) {
                        if (tile.bricks.size() >= limits.max_surface_bricks)
                            return fail(FarVolumeBuildError::SurfaceLimit);
                        tile.bricks.push_back(std::move(brick));
                    }
                }
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(FarVolumeBuildError::AllocationFailure);
    } catch (const std::length_error&) {
        return fail(FarVolumeBuildError::AllocationFailure);
    } catch (...) {
        return fail(FarVolumeBuildError::SamplerFailure);
    }
    tile.content =
        solid ? (air ? FarVolumeContent::Mixed : FarVolumeContent::Solid) : FarVolumeContent::Air;
    output = std::move(tile);
    if (error)
        *error = FarVolumeBuildError::None;
    return true;
}
} // namespace Luminumbra::World
