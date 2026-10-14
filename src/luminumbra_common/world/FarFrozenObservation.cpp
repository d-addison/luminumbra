#include "FarFrozenObservation.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <tuple>

namespace Luminumbra::World {
namespace {
using Error = FarFrozenCaptureError;

template<class T>
struct PoolAllocator {
    using value_type = T;
    std::shared_ptr<FarVolumeMemoryPool> pool;
    std::uint64_t cap;
    PoolAllocator(std::shared_ptr<FarVolumeMemoryPool> p, std::uint64_t c)
        : pool(std::move(p))
        , cap(c) {}
    template<class U>
    PoolAllocator(const PoolAllocator<U>& other)
        : pool(other.pool)
        , cap(other.cap) {}
    T* allocate(std::size_t n) {
        if (n > cap / sizeof(T) || !pool->TryReserve(n * sizeof(T)))
            throw FarVolumeMemoryRefused();
        try {
            return static_cast<T*>(::operator new(n * sizeof(T)));
        } catch (...) {
            pool->Release(n * sizeof(T));
            throw;
        }
    }
    void deallocate(T* p, std::size_t n) noexcept {
        ::operator delete(p);
        pool->Release(n * sizeof(T));
    }
    template<class U>
    bool operator==(const PoolAllocator<U>& other) const noexcept {
        return pool == other.pool && cap == other.cap;
    }
};

std::array<std::int64_t, 3> Axes(const FarVolumePosition& p) {
    return {p.x, p.y, p.z};
}
bool Coordinate(std::int64_t p) {
    return p > -kFarVolumeExactCoordinateLimitMeters && p < kFarVolumeExactCoordinateLimitMeters;
}
bool Add(std::uint64_t count, std::uint64_t size, std::uint64_t cap, std::uint64_t& used) {
    if (used > cap || count > (cap - used) / size)
        return false;
    used += count * size;
    return true;
}
Error Dimensions(const FarFrozenRegion& region, std::array<std::size_t, 3>& side) {
    const auto tier = FarTierAt(region.tier);
    if (!tier)
        return Error::InvalidRegion;
    const auto lo = Axes(region.support.min), hi = Axes(region.support.max);
    const auto step = std::int64_t(tier->sample_spacing_meters);
    for (std::size_t a = 0; a < 3; ++a) {
        if (!Coordinate(lo[a]) || !Coordinate(hi[a]))
            return Error::CoordinateRange;
        if (lo[a] > hi[a] || lo[a] % step || hi[a] % step)
            return Error::InvalidRegion;
        if (!Coordinate(lo[a] - step) || !Coordinate(hi[a] + step))
            return Error::CoordinateRange;
        side[a] = static_cast<std::size_t>((hi[a] - lo[a]) / step + 3);
    }
    return Error::None;
}
std::optional<std::size_t>
AxisIndex(std::int64_t p, std::int64_t origin, std::size_t side, std::uint32_t step) {
    if (!Coordinate(p) || p < origin || (p - origin) % step)
        return std::nullopt;
    const auto index = static_cast<std::size_t>((p - origin) / step);
    return index < side ? std::optional<std::size_t>{index} : std::nullopt;
}
template<class T, class Callback, class... Args>
Error Resolve(std::initializer_list<const Callback*> layers, T& value, Args... args) {
    for (const auto* callback : layers) {
        if (!*callback)
            continue;
        const auto result = (*callback)(args...);
        switch (result.presence) {
            case FarFrozenPresence::Absent:
                break;
            case FarFrozenPresence::Present:
                value = result.sample;
                return Error::None;
            case FarFrozenPresence::Unavailable:
                return Error::Unavailable;
            default:
                return Error::InvalidSource;
        }
    }
    return Error::None;
}
void Fingerprint(std::uint64_t& hash, std::uint64_t value) {
    // Explicit little-endian scalar bytes, independent of padding and containers.
    for (int i = 0; i < 8; ++i) {
        hash ^= value & 255;
        hash *= 1099511628211ULL;
        value >>= 8;
    }
}
bool Before(const FarFrozenRegion& a, const FarFrozenRegion& b) {
    return std::tie(a.tier,
                    a.support.min.z,
                    a.support.min.x,
                    a.support.min.y,
                    a.support.max.z,
                    a.support.max.x,
                    a.support.max.y) < std::tie(b.tier,
                                                b.support.min.z,
                                                b.support.min.x,
                                                b.support.min.y,
                                                b.support.max.z,
                                                b.support.max.x,
                                                b.support.max.y);
}
} // namespace

FarFrozenCaptureError
CaptureFarVolumeObservation(const FarVolumeRequest& request,
                            const FarFrozenCaptureSource& source,
                            const FarFrozenCaptureLimits& limits,
                            std::shared_ptr<FarVolumeMemoryPool> pool,
                            std::shared_ptr<const FarFrozenObservation>& output) {
    const auto tier = FarTierAt(request.key.tier);
    if (!tier || request.span.min_y >= request.span.max_y ||
        request.span.min_y % tier->brick_edge_meters ||
        request.span.max_y % tier->brick_edge_meters)
        return Error::InvalidRegion;
    const auto edge = std::int64_t(tier->tile_edge_meters);
    const auto limit = kFarVolumeExactCoordinateLimitMeters / edge;
    if (request.key.x <= -limit || request.key.x >= limit || request.key.z <= -limit ||
        request.key.z >= limit)
        return Error::CoordinateRange;
    if (request.field_identity != source.content_identity)
        return Error::IdentityMismatch;
    const FarFrozenRegion region{
        static_cast<std::uint8_t>(request.key.tier),
        {{request.key.x * edge, request.span.min_y, request.key.z * edge},
         {(request.key.x + 1) * edge, request.span.max_y, (request.key.z + 1) * edge}}};
    return CaptureFarFrozenObservation({&region, 1}, source, limits, std::move(pool), output);
}

FarFrozenObservation::FarFrozenObservation(CaptureToken,
                                           std::shared_ptr<FarVolumeMemoryPool> pool,
                                           const FarFrozenCaptureSource& source)
    : pool_(std::move(pool))
    , lease_(*pool_)
    , identity_(source.content_identity)
    , pristine_identity_(source.pristine.field_identity)
    , durable_(source.durable)
    , use_(source.use)
    , caves_(source.pristine.caves)
    , numeric_(source.pristine.numeric) {}

FarFrozenCaptureError
CaptureFarFrozenObservation(std::span<const FarFrozenRegion> regions,
                            const FarFrozenCaptureSource& source,
                            const FarFrozenCaptureLimits& limits,
                            std::shared_ptr<FarVolumeMemoryPool> pool,
                            std::shared_ptr<const FarFrozenObservation>& output) {
    if (!pool || !source.pristine.height || !source.pristine.density || !source.water ||
        (source.use != FarFrozenUse::Committed && source.use != FarFrozenUse::Preview) ||
        (source.durable.state != FarEditedImageState::Committed &&
         source.durable.state != FarEditedImageState::Prepared) ||
        (source.pristine.caves != FarCaveMode::BandLimited &&
         source.pristine.caves != FarCaveMode::BoxFiltered) ||
        (source.pristine.numeric != FarVolumeNumericProfile::RawFloat &&
         source.pristine.numeric != FarVolumeNumericProfile::LegacySigned16))
        return Error::InvalidSource;
    // Prepared authority is bake-only; it cannot become render authority here.
    if (source.use == FarFrozenUse::Preview &&
        source.durable.state != FarEditedImageState::Committed)
        return Error::IdentityMismatch;
    if (regions.empty() || regions.size() > limits.max_regions)
        return Error::WorkLimit;
    std::uint64_t columns = 0, samples = 0, bytes = 0;
    const auto terrain_calls = std::uint64_t(
        1 + bool(source.committed) +
        (source.use == FarFrozenUse::Preview ? bool(source.live) + bool(source.parked) : 0));
    const auto water_calls =
        std::uint64_t(1 + (source.use == FarFrozenUse::Preview
                               ? bool(source.live_water) + bool(source.parked_water)
                               : 0));
    for (const auto& region : regions) {
        std::array<std::size_t, 3> side{};
        const auto error = Dimensions(region, side);
        if (error != Error::None)
            return error;
        std::uint64_t count = 1;
        for (const auto extent : side) {
            if (count > limits.max_density_samples / terrain_calls / extent)
                return Error::WorkLimit;
            count *= extent;
        }
        if (!Add(count, terrain_calls, limits.max_density_samples, samples))
            return Error::WorkLimit;
        if (side[0] > limits.max_height_samples / side[2])
            return Error::WorkLimit;
        if (!Add(side[0], side[2], limits.max_height_samples, columns))
            return Error::WorkLimit;
    }
    const auto density_count = samples / terrain_calls;
    if (columns > limits.max_water_samples / water_calls)
        return Error::WorkLimit;
    if (!Add(regions.size(), sizeof(FarFrozenObservation::Grid), limits.max_bytes, bytes) ||
        !Add(columns, sizeof(float) + sizeof(FarFrozenWaterSample), limits.max_bytes, bytes) ||
        !Add(density_count, 2 * sizeof(FarVolumeSample), limits.max_bytes, bytes) ||
        bytes > std::numeric_limits<std::size_t>::max())
        return Error::ByteLimit;
    try {
        auto image = std::allocate_shared<FarFrozenObservation>(
            PoolAllocator<FarFrozenObservation>{pool, limits.max_bytes - bytes},
            FarFrozenObservation::CaptureToken{},
            pool,
            source);
        auto reserve = [&image](auto& vector, std::size_t count) {
            return FarVolumeReserveCharged(vector, count, count, &image->lease_);
        };
        if (!reserve(image->grids_, regions.size()) ||
            !reserve(image->heights_, static_cast<std::size_t>(columns)) ||
            !reserve(image->water_, static_cast<std::size_t>(columns)) ||
            !reserve(image->pristine_, static_cast<std::size_t>(density_count)) ||
            !reserve(image->composed_, static_cast<std::size_t>(density_count)))
            return Error::ByteLimit;
        // Containers use exact requested capacities on supported standard libraries.
        // Refuse rather than retain any implementation-specific surplus.
        if (image->lease_.bytes() != bytes)
            return Error::ByteLimit;
        for (const auto& region : regions) {
            FarFrozenObservation::Grid grid;
            grid.region = region;
            Dimensions(region, grid.side);
            const auto step = std::int64_t(FarTierAt(region.tier)->sample_spacing_meters);
            grid.origin = {region.support.min.x - step,
                           region.support.min.y - step,
                           region.support.min.z - step};
            image->grids_.push_back(grid);
        }
        std::sort(image->grids_.begin(), image->grids_.end(), [](const auto& a, const auto& b) {
            return Before(a.region, b.region);
        });
        for (auto& grid : image->grids_) {
            const auto step = FarTierAt(grid.region.tier)->sample_spacing_meters;
            grid.column_offset = image->heights_.size();
            grid.sample_offset = image->pristine_.size();
            grid.water_bound = {
                true, std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
            for (std::size_t z = 0; z < grid.side[2]; ++z) {
                for (std::size_t x = 0; x < grid.side[0]; ++x) {
                    const auto wx = grid.origin.x + std::int64_t(x) * step;
                    const auto wz = grid.origin.z + std::int64_t(z) * step;
                    const auto height = source.pristine.height(wx, wz);
                    if (!height)
                        return Error::Unavailable;
                    if (!std::isfinite(*height))
                        return Error::NonFiniteSample;
                    image->heights_.push_back(*height);
                    FarFrozenWaterSample observed_water;
                    bool selected = false;
                    if (source.use == FarFrozenUse::Preview) {
                        for (const auto* callback : {&source.live_water, &source.parked_water}) {
                            if (!*callback)
                                continue;
                            const auto value = (*callback)(wx, wz);
                            if (value.presence == FarFrozenPresence::Unavailable)
                                return Error::Unavailable;
                            if (value.presence == FarFrozenPresence::Present) {
                                observed_water = value.sample;
                                selected = true;
                                break;
                            }
                            if (value.presence != FarFrozenPresence::Absent)
                                return Error::InvalidSource;
                        }
                    }
                    if (!selected) {
                        const auto value = source.water(wx, wz);
                        if (!value)
                            return Error::Unavailable;
                        observed_water = *value;
                    }
                    if (!std::isfinite(observed_water.surface_height))
                        return Error::NonFiniteSample;
                    image->water_.push_back(observed_water);
                    grid.water_bound.known_dry &= !observed_water.wet;
                    grid.water_bound.min_y =
                        std::min(grid.water_bound.min_y, observed_water.surface_height);
                    grid.water_bound.max_y =
                        std::max(grid.water_bound.max_y, observed_water.surface_height);
                }
            }
            for (std::size_t z = 0; z < grid.side[2]; ++z) {
                for (std::size_t y = 0; y < grid.side[1]; ++y) {
                    for (std::size_t x = 0; x < grid.side[0]; ++x) {
                        const FarVolumePosition p{grid.origin.x + std::int64_t(x) * step,
                                                  grid.origin.y + std::int64_t(y) * step,
                                                  grid.origin.z + std::int64_t(z) * step};
                        auto sample = source.pristine.density(
                            p,
                            image->heights_[grid.column_offset + z * grid.side[0] + x],
                            step,
                            source.pristine.caves);
                        if (!sample)
                            return Error::Unavailable;
                        if (!std::isfinite(sample->density))
                            return Error::NonFiniteSample;
                        image->pristine_.push_back(*sample);
                        const auto error =
                            source.use == FarFrozenUse::Preview
                                ? Resolve<FarVolumeSample>(
                                      {&source.live, &source.parked, &source.committed},
                                      *sample,
                                      grid.region.tier,
                                      p)
                                : Resolve<FarVolumeSample>(
                                      {&source.committed}, *sample, grid.region.tier, p);
                        if (error != Error::None)
                            return error;
                        if (!std::isfinite(sample->density))
                            return Error::NonFiniteSample;
                        image->composed_.push_back(*sample);
                    }
                }
            }
        }
        std::uint64_t fingerprint = 14695981039346656037ULL;
        Fingerprint(fingerprint, image->identity_);
        Fingerprint(fingerprint, image->pristine_identity_);
        Fingerprint(fingerprint, static_cast<std::uint8_t>(image->caves_));
        Fingerprint(fingerprint, static_cast<std::uint8_t>(image->numeric_));
        Fingerprint(fingerprint, image->durable_.generation);
        Fingerprint(fingerprint, static_cast<std::uint8_t>(image->durable_.state));
        for (const auto& grid : image->grids_) {
            Fingerprint(fingerprint, grid.region.tier);
            for (const auto p : {grid.region.support.min, grid.region.support.max})
                for (const auto axis : Axes(p))
                    Fingerprint(fingerprint, static_cast<std::uint64_t>(axis));
        }
        for (const auto height : image->heights_)
            Fingerprint(fingerprint, std::bit_cast<std::uint32_t>(height));
        for (const auto& samples_view : {std::span<const FarVolumeSample>{image->pristine_},
                                         std::span<const FarVolumeSample>{image->composed_}})
            for (const auto sample : samples_view) {
                Fingerprint(fingerprint, std::bit_cast<std::uint32_t>(sample.density));
                Fingerprint(fingerprint, sample.material);
            }
        for (const auto water : image->water_) {
            Fingerprint(fingerprint, water.wet);
            Fingerprint(fingerprint, std::bit_cast<std::uint32_t>(water.surface_height));
        }
        image->identity_ = fingerprint;
        output = std::move(image);
        return Error::None;
    } catch (const FarVolumeMemoryRefused&) {
        return Error::ByteLimit;
    } catch (const std::bad_alloc&) {
        return Error::AllocationFailure;
    } catch (...) {
        return Error::Unavailable;
    }
}

std::optional<FarFrozenRegion> FarFrozenObservation::Region(std::size_t region) const {
    return region < grids_.size() ? std::optional{grids_[region].region} : std::nullopt;
}
std::optional<float>
FarFrozenObservation::Height(std::uint8_t tier, std::int64_t x, std::int64_t z) const {
    for (const auto& grid : grids_) {
        if (grid.region.tier != tier)
            continue;
        const auto step = FarTierAt(tier)->sample_spacing_meters;
        const auto ix = AxisIndex(x, grid.origin.x, grid.side[0], step);
        const auto iz = AxisIndex(z, grid.origin.z, grid.side[2], step);
        if (ix && iz)
            return heights_[grid.column_offset + *iz * grid.side[0] + *ix];
    }
    return std::nullopt;
}
std::optional<FarVolumeSample>
FarFrozenObservation::Sample(std::uint8_t tier, const FarVolumePosition& p, bool pristine) const {
    for (const auto& grid : grids_) {
        if (grid.region.tier != tier)
            continue;
        const auto step = FarTierAt(tier)->sample_spacing_meters;
        const auto x = AxisIndex(p.x, grid.origin.x, grid.side[0], step);
        const auto y = AxisIndex(p.y, grid.origin.y, grid.side[1], step);
        const auto z = AxisIndex(p.z, grid.origin.z, grid.side[2], step);
        if (x && y && z)
            return (
                pristine
                    ? pristine_
                    : composed_)[grid.sample_offset + *x + grid.side[0] * (*y + grid.side[1] * *z)];
    }
    return std::nullopt;
}
FarFrozenWaterCapability FarFrozenObservation::WaterCapability(std::size_t region) const noexcept {
    if (region >= grids_.size())
        return FarFrozenWaterCapability::Unavailable;
    return grids_[region].water_bound.known_dry ? FarFrozenWaterCapability::ExplicitlyDry
                                                : FarFrozenWaterCapability::Wet;
}
std::optional<FarFrozenWaterBound> FarFrozenObservation::WaterBound(std::size_t region) const {
    return region < grids_.size() ? std::optional{grids_[region].water_bound} : std::nullopt;
}
std::optional<FarFrozenWaterSample>
FarFrozenObservation::WaterSample(std::size_t region, std::int64_t x, std::int64_t z) const {
    if (region >= grids_.size())
        return std::nullopt;
    const auto& grid = grids_[region];
    const auto step = FarTierAt(grid.region.tier)->sample_spacing_meters;
    const auto ix = AxisIndex(x, grid.origin.x, grid.side[0], step);
    const auto iz = AxisIndex(z, grid.origin.z, grid.side[2], step);
    if (!ix || !iz)
        return std::nullopt;
    return water_[grid.column_offset + *iz * grid.side[0] + *ix];
}
FarVolumeAuthority FarFrozenObservation::Authority(std::uint8_t tier) const {
    if (durable_.state != FarEditedImageState::Committed)
        return {};
    const auto image = shared_from_this();
    return {
        identity_,
        caves_,
        numeric_,
        [image, tier](std::int64_t x, std::int64_t z) { return image->Height(tier, x, z); },
        [image, tier](const FarVolumePosition& p, float, std::uint32_t step, FarCaveMode caves) {
            const auto dimensions = FarTierAt(tier);
            if (!dimensions || step != dimensions->sample_spacing_meters || caves != image->caves())
                return std::optional<FarVolumeSample>{};
            return image->Sample(tier, p);
        }};
}
FarEditedPristineSampler FarFrozenObservation::PristineSampler() const {
    const auto image = shared_from_this();
    return [image](std::uint8_t tier, const FarVolumePosition& p) {
        return image->Sample(tier, p, true);
    };
}
FarEditedBakeError BakeFarEditedTilesFrozen(const FarEditedSourceView& source,
                                            std::span<const FarEditedFootprint> changed,
                                            const FarFrozenObservation& frozen,
                                            const FarEditedBakeLimits& limits,
                                            FarEditedBakeResult& output) {
    try {
        const auto identity = source.Identity(), captured = frozen.durable_identity();
        if (identity.generation != captured.generation || identity.state != captured.state ||
            frozen.numeric() != FarVolumeNumericProfile::LegacySigned16)
            return FarEditedBakeError::IdentityMismatch;
        return BakeFarEditedTiles(source, changed, frozen.PristineSampler(), limits, output);
    } catch (const std::bad_alloc&) {
        return FarEditedBakeError::AllocationFailure;
    } catch (...) {
        return FarEditedBakeError::SamplerFailure;
    }
}
} // namespace Luminumbra::World
