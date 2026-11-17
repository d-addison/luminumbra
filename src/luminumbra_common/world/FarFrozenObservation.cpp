#include "FarFrozenObservation.h"
#include "world/FarEditedBake.h"
#include "world/FarTierTable.h"
#include "world/FarVolume.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeTile.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

namespace Luminumbra::World {
namespace {
using Error = FarFrozenCaptureError;
Error WorkError(const FarFrozenCaptureWork& work) {
    return work.source_changed()      ? Error::IdentityMismatch
           : work.deadline_exceeded() ? Error::DeadlineExceeded
                                      : Error::WorkLimit;
}

template<class T>
class PoolAllocator {
    std::shared_ptr<FarVolumeMemoryPool> pool_;
    std::uint64_t cap_;

public:
    using value_type = T;
    PoolAllocator(std::shared_ptr<FarVolumeMemoryPool> p, std::uint64_t c)
        : pool_(std::move(p))
        , cap_(c) {}
    template<class U>
    PoolAllocator(const PoolAllocator<U>& other)
        : pool_(other.pool())
        , cap_(other.cap()) {}
    const std::shared_ptr<FarVolumeMemoryPool>& pool() const noexcept {
        return pool_;
    }
    std::uint64_t cap() const noexcept {
        return cap_;
    }
    T* allocate(std::size_t n) {
        if (n > cap_ / sizeof(T) || !pool_->TryReserve(n * sizeof(T)))
            throw FarVolumeMemoryRefused();
        try {
            return static_cast<T*>(::operator new(n * sizeof(T)));
        } catch (...) {
            pool_->Release(n * sizeof(T));
            throw;
        }
    }
    void deallocate(T* p, std::size_t n) noexcept {
        ::operator delete(p);
        pool_->Release(n * sizeof(T));
    }
    template<class U>
    bool operator==(const PoolAllocator<U>& other) const noexcept {
        return pool_ == other.pool() && cap_ == other.cap();
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
Error Resolve(std::initializer_list<const Callback*> layers,
              FarFrozenCaptureWork& work,
              T& value,
              Args... args) {
    for (const auto* callback : layers) {
        if (!*callback)
            continue;
        if (!work.ChargeDensity())
            return WorkError(work);
        const auto result = (*callback)(args...);
        if (!work.CheckDeadline())
            return WorkError(work);
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

bool FarFrozenCaptureWork::ChargeHeight() {
    if (!CheckDeadline() || counts_.height >= limits_.height ||
        (transaction_ && !transaction_->ChargeHeight())) {
        return false;
    }
    ++counts_.height;
    return true;
}
bool FarFrozenCaptureWork::ChargeDensity() {
    if (!CheckDeadline() || counts_.density >= limits_.density ||
        (transaction_ && !transaction_->ChargeDensity())) {
        return false;
    }
    ++counts_.density;
    return true;
}
bool FarFrozenCaptureWork::ChargeWater() {
    if (!CheckDeadline() || counts_.water >= limits_.water ||
        (transaction_ && !transaction_->ChargeWater())) {
        return false;
    }
    ++counts_.water;
    return true;
}
bool FarFrozenCaptureWork::CheckDeadline() {
    source_changed_ = source_changed_ || (current_ && !current_(source_context_));
    if (source_changed_) {
        return false;
    }
    const bool transaction_ok = !transaction_ || transaction_->CheckDeadline();
    deadline_exceeded_ = deadline_exceeded_ || deadline_.expired() || source_deadline_.expired() ||
                         (transaction_ && transaction_->deadline_exceeded());
    return transaction_ok && !deadline_exceeded_ && !source_changed();
}

FarFrozenCaptureError
CaptureFarVolumeObservation(const FarVolumeRequest& request,
                            const FarFrozenCaptureSource& source,
                            const FarFrozenCaptureLimits& limits,
                            const std::shared_ptr<FarVolumeMemoryPool>& pool,
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
    return CaptureFarFrozenObservation({&region, 1}, source, limits, pool, output);
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
                            const std::shared_ptr<FarVolumeMemoryPool>& pool,
                            std::shared_ptr<const FarFrozenObservation>& output) {
    FarFrozenObservation::Capture capture;
    capture.StartBorrowed(regions, source, limits, pool);
    for (;;) {
        const auto result = capture.Pump(std::numeric_limits<std::uint64_t>::max(), output);
        if (result.complete || result.error != Error::None) {
            return result.error;
        }
    }
}

void FarFrozenObservation::Capture::Cancel() {
    image_.reset();
    work_.reset();
    source_ = nullptr;
    regions_ = {};
    input_.reset();
    pool_.reset();
    limits_ = {};
}
void FarFrozenObservation::Capture::StartBorrowed(
    std::span<const FarFrozenRegion> regions,
    const FarFrozenCaptureSource& source,
    const FarFrozenCaptureLimits& limits,
    const std::shared_ptr<FarVolumeMemoryPool>& pool) {
    Cancel();
    source_ = &source;
    regions_ = regions;
    limits_ = limits;
    pool_ = pool;
    phase_ = Phase::Source;
    columns_ = samples_ = water_samples_ = bytes_ = 0;
    cursor_ = grid_ = hash_phase_ = 0;
    density_phase_ = false;
    work_.emplace(FarFrozenCaptureWork::Counts{limits.max_height_samples,
                                               limits.max_density_samples,
                                               limits.max_water_samples},
                  limits.work,
                  limits.deadline);
}
void FarFrozenObservation::Capture::Start(const std::shared_ptr<const FarFrozenCaptureInput>& input,
                                          const FarFrozenCaptureLimits& limits,
                                          const std::shared_ptr<FarVolumeMemoryPool>& pool) {
    Cancel();
    if (input) {
        StartBorrowed(input->regions, input->source, limits, pool);
        input_ = input;
        work_.emplace(FarFrozenCaptureWork::Counts{limits.max_height_samples,
                                                   limits.max_density_samples,
                                                   limits.max_water_samples},
                      limits.work,
                      limits.deadline,
                      FarFrozenCaptureDeadline{},
                      input->current,
                      input->owner.get());
    }
}
FarFrozenCapturePump
FarFrozenObservation::Capture::Pump(std::uint64_t budget,
                                    std::shared_ptr<const FarFrozenObservation>& output) {
    FarFrozenCapturePump result;
    if (!active() || !work_) {
        result.error = Error::InvalidSource;
        return result;
    }
    auto& work = *work_;
    try {
        while (result.work < budget) {
            if (input_ &&
                (!input_->owner || !input_->current || !input_->current(input_->owner.get()))) {
                result.error = Error::IdentityMismatch;
                break;
            }
            // Preserve synchronous preflight refusal precedence and allocation timing.
            if (phase_ != Phase::Source && phase_ != Phase::Validate && phase_ != Phase::Allocate &&
                !work.CheckDeadline()) {
                result.error = WorkError(work);
                break;
            }
            ++result.work;
            const bool publish = phase_ == Phase::Publish;
            result.error = Step();
            if (result.error != Error::None) {
                break;
            }
            if (input_ && !input_->current(input_->owner.get())) {
                result.error = Error::IdentityMismatch;
                break;
            }
            if (publish) {
                if (!work.CheckDeadline()) {
                    result.error = WorkError(work);
                    break;
                }
                output = std::move(image_);
                result.complete = true;
                break;
            }
        }
    } catch (const FarVolumeMemoryRefused&) {
        result.error = Error::ByteLimit;
    } catch (const std::bad_alloc&) {
        result.error = Error::AllocationFailure;
    } catch (...) {
        result.error = Error::Unavailable;
    }
    if (result.complete || result.error != Error::None) {
        Cancel();
    }
    return result;
}
FarFrozenCaptureError FarFrozenObservation::Capture::Step() {
    if (!work_) {
        return Error::InvalidSource;
    }
    auto& work = *work_;
    const auto before = [](const auto& a, const auto& b) {
        return Before(a.region, b.region);
    };
    switch (phase_) {
        case Phase::Source: {
            if (!pool_ || !source_->pristine.height || !source_->pristine.density ||
                !source_->committed || !source_->water ||
                (source_->use != FarFrozenUse::Committed && source_->use != FarFrozenUse::Preview &&
                 source_->use != FarFrozenUse::SessionRender) ||
                (source_->durable.state != FarEditedImageState::Committed &&
                 source_->durable.state != FarEditedImageState::Prepared) ||
                (source_->pristine.caves != FarCaveMode::BandLimited &&
                 source_->pristine.caves != FarCaveMode::BoxFiltered) ||
                (source_->pristine.numeric != FarVolumeNumericProfile::RawFloat &&
                 source_->pristine.numeric != FarVolumeNumericProfile::LegacySigned16))
                return Error::InvalidSource;
            // Prepared authority is bake-only; it cannot become render authority here.
            if (source_->use != FarFrozenUse::Committed &&
                source_->durable.state != FarEditedImageState::Committed)
                return Error::IdentityMismatch;
            if (regions_.empty() || regions_.size() > limits_.max_regions)
                return Error::WorkLimit;

            session_edits_ = source_->use != FarFrozenUse::Committed;
            terrain_calls_ = std::uint64_t{1} + std::uint64_t{bool(source_->committed)} +
                             (session_edits_ ? std::uint64_t{bool(source_->live)} +
                                                   std::uint64_t{bool(source_->parked)}
                                             : 0);
            water_calls_ =
                std::uint64_t{1} + (session_edits_ ? std::uint64_t{bool(source_->live_water)} +
                                                         std::uint64_t{bool(source_->parked_water)}
                                                   : 0);
            phase_ = Phase::Validate;
            break;
        }
        case Phase::Validate: {
            const auto& region = regions_[cursor_];
            std::array<std::size_t, 3> side{};
            const auto error = Dimensions(region, side);
            if (error != Error::None)
                return error;
            std::uint64_t count = 1;
            for (const auto extent : side) {
                if (count > limits_.max_density_samples / terrain_calls_ / extent)
                    return Error::WorkLimit;
                count *= extent;
            }
            if (!Add(count, terrain_calls_, limits_.max_density_samples, samples_))
                return Error::WorkLimit;
            if (side[0] > limits_.max_height_samples / side[2])
                return Error::WorkLimit;
            if (!Add(side[0], side[2], limits_.max_height_samples, columns_))
                return Error::WorkLimit;
            // Bound aggregate water callbacks across all regions and enabled layers,
            // including overlapping halos, before allocating or observing the source.
            if (!Add(std::uint64_t(side[0]) * side[2],
                     water_calls_,
                     limits_.max_water_samples,
                     water_samples_))
                return Error::WorkLimit;

            if (++cursor_ == regions_.size()) {
                phase_ = Phase::Allocate;
                cursor_ = 0;
            }
            break;
        }
        case Phase::Allocate: {
            const auto density_count = samples_ / terrain_calls_;
            if (!Add(regions_.size(),
                     sizeof(FarFrozenObservation::Grid),
                     limits_.max_bytes,
                     bytes_) ||
                !Add(columns_,
                     sizeof(float) + sizeof(FarFrozenWaterSample),
                     limits_.max_bytes,
                     bytes_) ||
                !Add(density_count, 2 * sizeof(FarVolumeSample), limits_.max_bytes, bytes_) ||
                bytes_ > std::numeric_limits<std::size_t>::max())
                return Error::ByteLimit;
            if (!work.CheckDeadline()) {
                return Error::DeadlineExceeded;
            }
            image_ = std::allocate_shared<FarFrozenObservation>(
                PoolAllocator<FarFrozenObservation>{pool_, limits_.max_bytes - bytes_},
                CaptureToken{},
                pool_,
                *source_);
            auto reserve = [this](auto& vector, std::size_t count) {
                return FarVolumeReserveCharged(vector, count, count, &image_->lease_);
            };
            if (!reserve(image_->grids_, regions_.size()) ||
                !reserve(image_->heights_, static_cast<std::size_t>(columns_)) ||
                !reserve(image_->water_, static_cast<std::size_t>(columns_)) ||
                !reserve(image_->pristine_, static_cast<std::size_t>(density_count)) ||
                !reserve(image_->composed_, static_cast<std::size_t>(density_count)))
                return Error::ByteLimit;
            // Containers use exact requested capacities on supported standard libraries.
            // Refuse rather than retain any implementation-specific surplus.
            if (image_->lease_.bytes() != bytes_)
                return Error::ByteLimit;

            phase_ = Phase::Grids;
            break;
        }
        case Phase::Grids: {
            Grid grid;
            grid.region = regions_[cursor_];
            const auto error = Dimensions(grid.region, grid.side);
            if (error != Error::None) {
                return error;
            }
            const auto tier = FarTierAt(grid.region.tier);
            if (!tier) {
                return Error::InvalidRegion;
            }
            const auto step = std::int64_t(tier->sample_spacing_meters);
            grid.origin = {grid.region.support.min.x - step,
                           grid.region.support.min.y - step,
                           grid.region.support.min.z - step};
            image_->grids_.push_back(grid);
            std::push_heap(image_->grids_.begin(), image_->grids_.end(), before);
            if (++cursor_ == regions_.size()) {
                phase_ = Phase::Sort;
            }
            break;
        }
        case Phase::Sort: {
            std::pop_heap(image_->grids_.begin(),
                          image_->grids_.begin() + static_cast<std::ptrdiff_t>(cursor_),
                          before);
            if (--cursor_ == 0) {
                phase_ = Phase::Sample;
            }
            break;
        }
        case Phase::Sample: {
            auto& grid = image_->grids_[grid_];
            if (cursor_ == 0 && !density_phase_) {
                grid.column_offset = image_->heights_.size();
                grid.sample_offset = image_->pristine_.size();
                grid.water_bound = {
                    true, std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
            }
            const auto error = density_phase_ ? Density() : Column();
            if (error != Error::None) {
                return error;
            }
            const auto count = grid.side[0] * grid.side[2] * (density_phase_ ? grid.side[1] : 1);
            if (++cursor_ == count) {
                cursor_ = 0;
                if (!density_phase_) {
                    density_phase_ = true;
                } else {
                    density_phase_ = false;
                    if (++grid_ == image_->grids_.size()) {
                        phase_ = Phase::Hash;
                        fingerprint_ = 14695981039346656037ULL;
                    }
                }
            }
            break;
        }
        case Phase::Hash:
            HashStep();
            break;
        case Phase::Publish:
            break;
    }
    return Error::None;
}
FarFrozenCaptureError FarFrozenObservation::Capture::Column() {
    if (!work_) {
        return Error::InvalidSource;
    }
    auto& work = *work_;
    auto& grid = image_->grids_[grid_];
    const auto tier = FarTierAt(grid.region.tier);
    if (!tier) {
        return Error::InvalidRegion;
    }
    const auto step = tier->sample_spacing_meters;
    const auto x = cursor_ % grid.side[0], z = cursor_ / grid.side[0];
    const auto wx = grid.origin.x + std::int64_t(x) * step;
    const auto wz = grid.origin.z + std::int64_t(z) * step;
    if (!work.ChargeHeight())
        return WorkError(work);
    const auto height = source_->pristine.height(wx, wz);
    if (!work.CheckDeadline())
        return WorkError(work);
    if (!height)
        return Error::Unavailable;
    if (!std::isfinite(*height))
        return Error::NonFiniteSample;
    image_->heights_.push_back(*height);
    FarFrozenWaterSample observed_water;
    bool selected = false;
    if (session_edits_) {
        for (const auto* callback : {&source_->live_water, &source_->parked_water}) {
            if (!*callback)
                continue;
            if (!work.ChargeWater())
                return WorkError(work);
            const auto value = (*callback)(wx, wz);
            if (!work.CheckDeadline())
                return WorkError(work);
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
        if (!work.ChargeWater())
            return WorkError(work);
        const auto value = source_->water(wx, wz);
        if (!work.CheckDeadline())
            return WorkError(work);
        if (!value)
            return Error::Unavailable;
        observed_water = *value;
    }
    if (!std::isfinite(observed_water.surface_height))
        return Error::NonFiniteSample;
    image_->water_.push_back(observed_water);
    grid.water_bound.known_dry &= !observed_water.wet;
    grid.water_bound.min_y = std::min(grid.water_bound.min_y, observed_water.surface_height);
    grid.water_bound.max_y = std::max(grid.water_bound.max_y, observed_water.surface_height);

    return Error::None;
}
FarFrozenCaptureError FarFrozenObservation::Capture::Density() {
    if (!work_) {
        return Error::InvalidSource;
    }
    auto& work = *work_;
    const auto& grid = image_->grids_[grid_];
    const auto tier = FarTierAt(grid.region.tier);
    if (!tier) {
        return Error::InvalidRegion;
    }
    const auto step = tier->sample_spacing_meters;
    const auto x = cursor_ % grid.side[0], y = (cursor_ / grid.side[0]) % grid.side[1],
               z = cursor_ / (grid.side[0] * grid.side[1]);
    const FarVolumePosition p{grid.origin.x + std::int64_t(x) * step,
                              grid.origin.y + std::int64_t(y) * step,
                              grid.origin.z + std::int64_t(z) * step};
    if (!work.ChargeDensity())
        return WorkError(work);
    auto sample =
        source_->pristine.density(p,
                                  image_->heights_[grid.column_offset + z * grid.side[0] + x],
                                  step,
                                  source_->pristine.caves);
    if (!work.CheckDeadline())
        return WorkError(work);
    if (!sample)
        return Error::Unavailable;
    if (!std::isfinite(sample->density))
        return Error::NonFiniteSample;
    image_->pristine_.push_back(*sample);
    const auto error =
        session_edits_
            ? Resolve<FarVolumeSample>({&source_->live, &source_->parked, &source_->committed},
                                       work,
                                       *sample,
                                       grid.region.tier,
                                       p)
            : Resolve<FarVolumeSample>({&source_->committed}, work, *sample, grid.region.tier, p);
    if (error != Error::None)
        return error;
    if (!std::isfinite(sample->density))
        return Error::NonFiniteSample;
    image_->composed_.push_back(*sample);

    return Error::None;
}
void FarFrozenObservation::Capture::HashStep() {
    switch (hash_phase_) {
        case 0:
            Fingerprint(fingerprint_, image_->identity_);
            Fingerprint(fingerprint_, image_->pristine_identity_);
            Fingerprint(fingerprint_, static_cast<std::uint8_t>(image_->caves_));
            Fingerprint(fingerprint_, static_cast<std::uint8_t>(image_->numeric_));
            Fingerprint(fingerprint_, image_->durable_.generation);
            Fingerprint(fingerprint_, static_cast<std::uint8_t>(image_->durable_.state));
            ++hash_phase_;
            break;
        case 1: {
            const auto& grid = image_->grids_[cursor_];
            Fingerprint(fingerprint_, grid.region.tier);
            for (const auto p : {grid.region.support.min, grid.region.support.max}) {
                for (const auto axis : Axes(p)) {
                    Fingerprint(fingerprint_, static_cast<std::uint64_t>(axis));
                }
            }
            if (++cursor_ == image_->grids_.size()) {
                cursor_ = 0;
                ++hash_phase_;
            }
            break;
        }
        case 2:
            Fingerprint(fingerprint_, std::bit_cast<std::uint32_t>(image_->heights_[cursor_]));
            if (++cursor_ == image_->heights_.size()) {
                cursor_ = 0;
                ++hash_phase_;
            }
            break;
        case 3:
        case 4: {
            const auto sample = (hash_phase_ == 3 ? image_->pristine_ : image_->composed_)[cursor_];
            Fingerprint(fingerprint_, std::bit_cast<std::uint32_t>(sample.density));
            Fingerprint(fingerprint_, sample.material);
            if (++cursor_ == image_->pristine_.size()) {
                cursor_ = 0;
                ++hash_phase_;
            }
            break;
        }
        default: {
            const auto water = image_->water_[cursor_];
            Fingerprint(fingerprint_, water.wet);
            Fingerprint(fingerprint_, std::bit_cast<std::uint32_t>(water.surface_height));
            if (++cursor_ == image_->water_.size()) {
                image_->identity_ = fingerprint_;
                phase_ = Phase::Publish;
            }
            break;
        }
    }
}

std::optional<FarFrozenRegion> FarFrozenObservation::Region(std::size_t region) const {
    return region < grids_.size() ? std::optional{grids_[region].region} : std::nullopt;
}
std::optional<float>
FarFrozenObservation::Height(std::uint8_t tier, std::int64_t x, std::int64_t z) const {
    const auto dimensions = FarTierAt(tier);
    if (!dimensions)
        return std::nullopt;
    for (const auto& grid : grids_) {
        if (grid.region.tier != tier)
            continue;
        const auto step = dimensions->sample_spacing_meters;
        const auto ix = AxisIndex(x, grid.origin.x, grid.side[0], step);
        const auto iz = AxisIndex(z, grid.origin.z, grid.side[2], step);
        if (ix && iz)
            return heights_[grid.column_offset + *iz * grid.side[0] + *ix];
    }
    return std::nullopt;
}
std::optional<FarVolumeSample>
FarFrozenObservation::Sample(std::uint8_t tier, const FarVolumePosition& p, bool pristine) const {
    const auto dimensions = FarTierAt(tier);
    if (!dimensions)
        return std::nullopt;
    for (const auto& grid : grids_) {
        if (grid.region.tier != tier)
            continue;
        const auto step = dimensions->sample_spacing_meters;
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
    const auto tier = FarTierAt(grid.region.tier);
    if (!tier)
        return std::nullopt;
    const auto step = tier->sample_spacing_meters;
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
    if (frozen.use() == FarFrozenUse::SessionRender)
        return FarEditedBakeError::IdentityMismatch;
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
