#include "FarVolume.h"

#include "FarDensityQuantization.h"
#include "core/Crc32.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace Luminumbra::World {
namespace {
constexpr std::int64_t kCoordinateLimit = kFarVolumeExactCoordinateLimitMeters;
constexpr std::size_t kTileSide = 129, kHeightSide = kTileSide + 2;

bool ValidCaves(FarCaveMode caves) {
    return caves == FarCaveMode::BandLimited || caves == FarCaveMode::BoxFiltered;
}
bool Coordinate(std::int64_t value) {
    return value > -kCoordinateLimit && value < kCoordinateLimit;
}
bool TileOrigin(std::int64_t tile, std::uint32_t edge, std::int64_t& output) {
    if (tile <= -kCoordinateLimit / edge || tile >= kCoordinateLimit / edge)
        return false;
    const auto origin = tile * edge; // The preceding bound makes multiplication representable.
    if (!Coordinate(origin) || !Coordinate(origin + edge))
        return false;
    output = origin;
    return true;
}

class AuthoritySession {
public:
    AuthoritySession(const FarVolumeAuthority& authority, const FarVolumeAuthorityLimits& limits)
        : authority_(authority)
        , limits_(limits) {}

    bool resolve(const FarVolumeCoverageIntent& intent, FarVolumeRequest& request) {
        const auto d = FarTierAt(intent.key.tier);
        if (!d || (intent.extra_span && (!std::isfinite(intent.extra_span->min_y) ||
                                         !std::isfinite(intent.extra_span->max_y) ||
                                         intent.extra_span->min_y > intent.extra_span->max_y)))
            return fail(FarVolumeFacadeError::InvalidIntent);
        if (!ValidCaves(authority_.caves) ||
            (authority_.numeric != FarVolumeNumericProfile::RawFloat &&
             authority_.numeric != FarVolumeNumericProfile::LegacySigned16) ||
            !authority_.height || !authority_.density)
            return fail(FarVolumeFacadeError::InvalidAuthority);
        dimensions_ = *d;
        if (!TileOrigin(intent.key.x, d->tile_edge_meters, origin_x_) ||
            !TileOrigin(intent.key.z, d->tile_edge_meters, origin_z_))
            return fail(FarVolumeFacadeError::CoordinateRange);
        if (limits_.max_height_samples < kTileSide * kTileSide)
            return fail(FarVolumeFacadeError::HeightLimit);
        heights_.resize(kHeightSide * kHeightSide);
        float low = std::numeric_limits<float>::max();
        float high = std::numeric_limits<float>::lowest();
        for (std::size_t z = 0; z < kTileSide; ++z)
            for (std::size_t x = 0; x < kTileSide; ++x) {
                const auto value =
                    height(origin_x_ + static_cast<std::int64_t>(x) * d->sample_spacing_meters,
                           origin_z_ + static_cast<std::int64_t>(z) * d->sample_spacing_meters);
                if (!value)
                    return false;
                low = std::min(low, *value);
                high = std::max(high, *value);
            }
        // Preserve the foundation's float arithmetic and inclusive top-plane
        // behavior before resolving to canonical integer brick boundaries.
        low -= kFarVolumeInteriorDepthMeters;
        if (intent.extra_span) {
            low = std::min(low, intent.extra_span->min_y);
            high = std::max(high, intent.extra_span->max_y);
        }
        if (low <= -kCoordinateLimit + 1024 || high >= kCoordinateLimit - 1024)
            return fail(FarVolumeFacadeError::CoordinateRange);
        const auto first =
            static_cast<std::int64_t>(std::floor(static_cast<double>(low) / d->brick_edge_meters));
        const auto last = static_cast<std::int64_t>(
                              std::floor(static_cast<double>(high) / d->brick_edge_meters)) +
                          1;
        request = {intent.key,
                   {first * d->brick_edge_meters, last * d->brick_edge_meters},
                   authority_.field_identity};
        return true;
    }

    std::optional<FarVolumeSample> sample(const FarVolumePosition& position) {
        if (!Coordinate(position.y)) {
            fail(FarVolumeFacadeError::CoordinateRange);
            return std::nullopt;
        }
        const auto h = height(position.x, position.z);
        if (!h)
            return std::nullopt;
        if (failure_.work.density_samples >= limits_.max_density_samples) {
            fail(FarVolumeFacadeError::DensityLimit);
            return std::nullopt;
        }
        ++failure_.work.density_samples;
        std::optional<FarVolumeSample> value;
        try {
            value = authority_.density(
                position, *h, dimensions_.sample_spacing_meters, authority_.caves);
        } catch (const std::bad_alloc&) {
            fail(FarVolumeFacadeError::AllocationFailure);
            return std::nullopt;
        } catch (...) {
            fail(FarVolumeFacadeError::SamplerFailure);
            return std::nullopt;
        }
        if (!value) {
            fail(FarVolumeFacadeError::SamplerFailure);
            return std::nullopt;
        }
        if (!std::isfinite(value->density)) {
            fail(FarVolumeFacadeError::NonFiniteSample);
            return std::nullopt;
        }
        if (authority_.numeric == FarVolumeNumericProfile::LegacySigned16)
            value->density = DequantizeFarLodSdf(QuantizeFarLodSdf(value->density));
        return value;
    }

    bool fail(FarVolumeFacadeError error) {
        if (failure_.code == FarVolumeFacadeError::None)
            failure_.code = error;
        return false;
    }
    FarVolumeFacadeFailure& failure() {
        return failure_;
    }
    const FarVolumeAuthority& authority() const {
        return authority_;
    }

private:
    std::optional<float> height(std::int64_t x, std::int64_t z) {
        const auto spacing = static_cast<std::int64_t>(dimensions_.sample_spacing_meters);
        if (!Coordinate(x) || !Coordinate(z) || x < origin_x_ - spacing ||
            z < origin_z_ - spacing || x > origin_x_ + dimensions_.tile_edge_meters + spacing ||
            z > origin_z_ + dimensions_.tile_edge_meters + spacing ||
            (x - origin_x_) % spacing != 0 || (z - origin_z_) % spacing != 0) {
            fail(FarVolumeFacadeError::CoordinateRange);
            return std::nullopt;
        }
        const auto ix = static_cast<std::size_t>((x - origin_x_) / spacing + 1);
        const auto iz = static_cast<std::size_t>((z - origin_z_) / spacing + 1);
        auto& cached = heights_[ix + iz * kHeightSide];
        if (cached)
            return cached;
        if (failure_.work.height_samples >= limits_.max_height_samples) {
            fail(FarVolumeFacadeError::HeightLimit);
            return std::nullopt;
        }
        ++failure_.work.height_samples;
        std::optional<float> value;
        try {
            value = authority_.height(x, z);
        } catch (const std::bad_alloc&) {
            fail(FarVolumeFacadeError::AllocationFailure);
            return std::nullopt;
        } catch (...) {
            fail(FarVolumeFacadeError::SamplerFailure);
            return std::nullopt;
        }
        if (!value) {
            fail(FarVolumeFacadeError::SamplerFailure);
            return std::nullopt;
        }
        if (!std::isfinite(*value)) {
            fail(FarVolumeFacadeError::NonFiniteSample);
            return std::nullopt;
        }
        if (*value <= -kCoordinateLimit + 1024 || *value >= kCoordinateLimit - 1024) {
            fail(FarVolumeFacadeError::CoordinateRange);
            return std::nullopt;
        }
        cached = value;
        return value;
    }

    const FarVolumeAuthority authority_;
    const FarVolumeAuthorityLimits limits_;
    FarTierDimensions dimensions_{};
    std::int64_t origin_x_ = 0, origin_z_ = 0;
    std::vector<std::optional<float>> heights_;
    FarVolumeFacadeFailure failure_;
};

void Mix(Core::Crc32Accumulator& crc, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        const auto byte = static_cast<std::uint8_t>(value >> (8 * i));
        crc.Update(&byte, 1);
    }
}
void MixBrick(Core::Crc32Accumulator& crc, const FarVolumeBrick& brick, std::uint32_t edge) {
    for (const auto value : {brick.origin_meters.x, brick.origin_meters.y, brick.origin_meters.z})
        Mix(crc, static_cast<std::uint32_t>(value / edge), 4);
    for (const auto& sample : brick.samples) {
        Mix(crc, static_cast<std::uint16_t>(QuantizeFarLodSdf(sample.density)), 2);
        Mix(crc, sample.material, 1);
    }
}
bool ZxyBefore(const FarVolumePosition& a, const FarVolumePosition& b) {
    return std::tie(a.z, a.x, a.y) < std::tie(b.z, b.x, b.y);
}
bool ValidCompatibilityTile(const FarVolumeTile& tile, FarCaveMode caves) {
    const auto d = FarTierAt(tile.request.key.tier);
    if (!d || !ValidCaves(caves))
        return false;
    const auto edge = static_cast<std::int64_t>(d->brick_edge_meters);
    const auto& span = tile.request.span;
    std::int64_t ox = 0, oz = 0;
    if (!TileOrigin(tile.request.key.x, d->tile_edge_meters, ox) ||
        !TileOrigin(tile.request.key.z, d->tile_edge_meters, oz) || !Coordinate(span.min_y) ||
        !Coordinate(span.max_y) || span.min_y >= span.max_y || span.min_y % edge != 0 ||
        span.max_y % edge != 0 || tile.min_meters != FarVolumePosition{ox, span.min_y, oz} ||
        tile.max_meters !=
            FarVolumePosition{ox + d->tile_edge_meters, span.max_y, oz + d->tile_edge_meters})
        return false;
    const auto candidates = 1024ull * static_cast<std::uint64_t>((span.max_y - span.min_y) / edge);
    if (tile.candidates_visited != candidates || tile.samples_evaluated != candidates * 125 ||
        tile.bricks.size() > candidates ||
        (tile.content != FarVolumeContent::Air && tile.content != FarVolumeContent::Solid &&
         tile.content != FarVolumeContent::Mixed) ||
        (!tile.bricks.empty() && tile.content != FarVolumeContent::Mixed))
        return false;
    for (std::size_t i = 0; i < tile.bricks.size(); ++i) {
        const auto& brick = tile.bricks[i];
        const auto& origin = brick.origin_meters;
        if ((i && !ZxyBefore(tile.bricks[i - 1].origin_meters, origin)) || origin.x < ox ||
            origin.x >= tile.max_meters.x || origin.y < span.min_y || origin.y >= span.max_y ||
            origin.z < oz || origin.z >= tile.max_meters.z || origin.x % edge != 0 ||
            origin.y % edge != 0 || origin.z % edge != 0)
            return false;
        bool solid = false, air = false;
        for (const auto& sample : brick.samples) {
            const auto encoded = QuantizeFarLodSdf(sample.density);
            if (encoded == kFarLodSdfInvalid ||
                std::bit_cast<std::uint32_t>(sample.density) !=
                    std::bit_cast<std::uint32_t>(DequantizeFarLodSdf(encoded)))
                return false;
            solid |= encoded < 0;
            air |= encoded >= 0;
        }
        if (!solid || !air)
            return false;
    }
    // All ordering is checked before binary search. Retained neighbours share
    // faces, edges and corners, without inventing absent homogeneous bricks.
    for (const auto& brick : tile.bricks)
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const FarVolumePosition key{brick.origin_meters.x + dx * edge,
                                                brick.origin_meters.y + dy * edge,
                                                brick.origin_meters.z + dz * edge};
                    if (!ZxyBefore(brick.origin_meters, key))
                        continue;
                    const auto other =
                        std::lower_bound(tile.bricks.begin(),
                                         tile.bricks.end(),
                                         key,
                                         [](const FarVolumeBrick& a, const FarVolumePosition& b) {
                                             return ZxyBefore(a.origin_meters, b);
                                         });
                    if (other == tile.bricks.end() || other->origin_meters != key)
                        continue;
                    const auto index = [](int x, int y, int z) {
                        return x + 5 * (y + 5 * z);
                    };
                    for (int z = std::max(0, dz * 4); z <= std::min(4, dz * 4 + 4); ++z)
                        for (int y = std::max(0, dy * 4); y <= std::min(4, dy * 4 + 4); ++y)
                            for (int x = std::max(0, dx * 4); x <= std::min(4, dx * 4 + 4); ++x)
                                if (brick.samples[index(x, y, z)] !=
                                    other->samples[index(x - dx * 4, y - dy * 4, z - dz * 4)])
                                    return false;
                }
    return true;
}
} // namespace

bool ResolveFarVolumeCoverage(const FarVolumeCoverageIntent& intent,
                              const FarVolumeAuthority& authority,
                              const FarVolumeAuthorityLimits& limits,
                              FarVolumeRequest& output,
                              FarVolumeFacadeFailure* failure) {
    try {
        AuthoritySession session(authority, limits);
        FarVolumeRequest request;
        const bool success = session.resolve(intent, request);
        if (failure)
            *failure = session.failure();
        if (success)
            output = request;
        return success;
    } catch (const std::bad_alloc&) {
        if (failure)
            *failure = {FarVolumeFacadeError::AllocationFailure,
                        FarVolumeBuildError::None,
                        FarVolumeMeshError::None,
                        {}};
        return false;
    } catch (...) {
        // A user-owned std::function target can also throw while being copied.
        if (failure)
            *failure = {FarVolumeFacadeError::SamplerFailure,
                        FarVolumeBuildError::None,
                        FarVolumeMeshError::None,
                        {}};
        return false;
    }
}

bool CompileFarVolume(const FarVolumeCoverageIntent& intent,
                      const FarVolumeAuthority& authority,
                      const FarVolumeCompilationLimits& limits,
                      FarVolumeCompilation& output,
                      FarVolumeFacadeFailure* failure) {
    try {
        AuthoritySession session(authority, limits.authority);
        const auto publish_failure = [&] {
            if (failure)
                *failure = session.failure();
            return false;
        };
        FarVolumeRequest request;
        if (!session.resolve(intent, request))
            return publish_failure();
        FarVolumeCompilation candidate;
        candidate.intent = intent;
        candidate.caves = session.authority().caves;
        candidate.numeric = session.authority().numeric;
        const FarVolumeSampler sampler = [&](const FarVolumePosition& p) {
            return session.sample(p);
        };
        if (!BuildFarVolumeTile(request,
                                limits.generation,
                                sampler,
                                candidate.tile,
                                &session.failure().generation)) {
            session.fail(FarVolumeFacadeError::GenerationRefused);
            return publish_failure();
        }
        if (!MeshFarVolumeTile(candidate.tile,
                               request.field_identity,
                               sampler,
                               limits.mesh,
                               candidate.mesh,
                               &session.failure().mesh)) {
            session.fail(FarVolumeFacadeError::MeshRefused);
            return publish_failure();
        }
        if (session.authority().numeric == FarVolumeNumericProfile::LegacySigned16) {
            std::uint32_t crc = 0;
            if (!FarVolumeLegacyChecksum(candidate.tile, session.authority().caves, crc)) {
                session.fail(FarVolumeFacadeError::InvalidCompatibilityTile);
                return publish_failure();
            }
            candidate.legacy_checksum = crc;
        }
        candidate.work = session.failure().work;
        if (failure)
            *failure = session.failure();
        output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        if (failure)
            *failure = {FarVolumeFacadeError::AllocationFailure,
                        FarVolumeBuildError::None,
                        FarVolumeMeshError::None,
                        {}};
        return false;
    } catch (...) {
        // A user-owned std::function target can also throw while being copied.
        if (failure)
            *failure = {FarVolumeFacadeError::SamplerFailure,
                        FarVolumeBuildError::None,
                        FarVolumeMeshError::None,
                        {}};
        return false;
    }
}

bool FarVolumeLegacyChecksum(const FarVolumeTile& tile, FarCaveMode caves, std::uint32_t& output) {
    try {
        if (!ValidCompatibilityTile(tile, caves))
            return false;
        const auto edge = FarTierAt(tile.request.key.tier)->brick_edge_meters;
        std::vector<const FarVolumeBrick*> ordered;
        ordered.reserve(tile.bricks.size());
        for (const auto& brick : tile.bricks)
            ordered.push_back(&brick);
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            const auto& x = a->origin_meters;
            const auto& y = b->origin_meters;
            return std::tie(x.x, x.y, x.z) < std::tie(y.x, y.y, y.z);
        });
        Core::Crc32Accumulator crc;
        Mix(crc, tile.request.key.tier, 4);
        Mix(crc, static_cast<std::uint32_t>(tile.request.key.x), 4);
        Mix(crc, static_cast<std::uint32_t>(tile.request.key.z), 4);
        Mix(crc, static_cast<std::uint8_t>(caves), 1);
        Mix(crc, static_cast<std::uint32_t>(tile.request.span.min_y / edge), 4);
        Mix(crc, static_cast<std::uint32_t>(tile.request.span.max_y / edge), 4);
        Mix(crc, tile.candidates_visited, 8);
        Mix(crc, tile.bricks.size(), 8);
        for (const auto* brick : ordered) {
            MixBrick(crc, *brick, edge);
            Core::Crc32Accumulator brick_crc;
            MixBrick(brick_crc, *brick, edge);
            Mix(crc, brick_crc.Value(), 4);
        }
        output = crc.Value();
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

float BoxFilterFarVolumeCarve(const glm::vec3& position,
                              std::uint32_t spacing,
                              const std::function<float(const glm::vec3&)>& carve) {
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
                const float value = carve(position + glm::vec3(static_cast<float>(x) * offset,
                                                               static_cast<float>(y) * offset,
                                                               static_cast<float>(z) * offset));
                if (!std::isfinite(value) || value < 0.0f)
                    throw std::invalid_argument("Invalid far-volume carve contribution");
                sum += value;
            }
    return static_cast<float>(sum * 0.125);
}

} // namespace Luminumbra::World
