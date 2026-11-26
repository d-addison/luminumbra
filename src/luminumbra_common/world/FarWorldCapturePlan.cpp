#include "FarWorldCapturePlan.h"
#include "systems/SHIELD_WorldSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace Luminumbra::World {
using Producer = FarWorldAuthorityProducer;
using Save = Persistence::WorldSaveService;
struct FarWorldCapturePlan::Impl {
    // Fixed descriptor storage: the producer's existing 512-slot envelope.
    std::array<FarTileKey, Producer::kSlots> keys{};
    std::array<Save::AuthorityWindow, Producer::kSlots> windows{};
    std::size_t count = 0;
    std::shared_ptr<const Image> accepted;
    std::weak_ptr<Producer> accepted_producer;
    std::weak_ptr<const Systems::SHIELD_WorldSystem> accepted_world;
    std::shared_ptr<Image> pending;
    std::shared_ptr<Producer> producer;
    std::shared_ptr<const Systems::SHIELD_WorldSystem> world;
    Producer::FrozenPolicy policy;
    FarFrozenCaptureLimits sampling;
    Limits limits;
    std::optional<FarFrozenCaptureWork> aggregate;
    std::uint64_t generation = 0, operations = 0;
    std::size_t cursor = 0;
    std::size_t history_cursor = 0, wanted_cursor = 0;
    bool history_ready = false;
    bool sampling_active = false, tile_ready = false;
    FarVolumeRequest request;
    bool Current() const {
        const auto status = producer->capture_status();
        return status.error == Producer::Error::None && !status.paused &&
               status.serving_serial == pending->source_serial &&
               generation == world->water_seed_terrain_generation();
    }
    bool AdmitSpan(Tile& tile) const {
        const auto& span = tile.request.span;
        const auto& bounds = limits.spans;
        if (static_cast<double>(span.min_y) < bounds.min_y ||
            static_cast<double>(span.max_y) > bounds.max_y ||
            static_cast<double>(span.max_y - span.min_y) > bounds.max_extent_meters) {
            return false;
        }
        tile.span_generation = 1;
        if (!accepted) {
            return true;
        }
        const auto found = std::lower_bound(
            accepted->spans.begin(),
            accepted->spans.end(),
            tile.key,
            [](const Tile& entry, const FarTileKey& key) { return entry.key < key; });
        if (found == accepted->spans.end() || found->key != tile.key) {
            return true;
        }
        // Aligned spans are half-open: merging them does not apply another
        // inclusive top-plane extension and cannot grow an unchanged span.
        tile.request.span.min_y = std::min(span.min_y, found->request.span.min_y);
        tile.request.span.max_y = std::max(span.max_y, found->request.span.max_y);
        tile.span_generation = found->span_generation;
        if (tile.request.span != found->request.span) {
            if (tile.span_generation == std::numeric_limits<std::uint64_t>::max()) {
                return false;
            }
            ++tile.span_generation;
        }
        return static_cast<double>(span.min_y) >= bounds.min_y &&
               static_cast<double>(span.max_y) <= bounds.max_y &&
               static_cast<double>(span.max_y - span.min_y) <= bounds.max_extent_meters;
    }
};
FarWorldCapturePlan::FarWorldCapturePlan(const std::shared_ptr<FarVolumeMemoryPool>& pool)
    : pool_(pool) {
    if (!pool_) {
        throw FarVolumeMemoryRefused();
    }
    control_ = FarVolumeMemoryLease(*pool_);
    if (!control_.TryCharge(sizeof(FarWorldCapturePlan))) {
        throw FarVolumeMemoryRefused();
    }
    impl_ = std::allocate_shared<Impl>(FarVolumePoolAllocator<Impl>(pool_));
}
FarWorldCapturePlan::~FarWorldCapturePlan() {
    Cancel();
}
FarWorldCapturePlan::Error FarWorldCapturePlan::SetWanted(std::span<const FarTileKey> keys) {
    if (active()) {
        return Error::SourceRefused;
    }
    if (keys.size() > Producer::kSlots) {
        return Error::Capacity;
    }
    std::array<FarTileKey, Producer::kSlots> canonical{};
    std::copy(keys.begin(), keys.end(), canonical.begin());
    auto end = canonical.begin() + static_cast<std::ptrdiff_t>(keys.size());
    std::sort(canonical.begin(), end);
    end = std::unique(canonical.begin(), end);
    std::array<Save::AuthorityWindow, Producer::kSlots> windows{};
    const auto count = static_cast<std::size_t>(end - canonical.begin());
    for (std::size_t index = 0; index < count; ++index) {
        FarVolumeTileKey key;
        if (!ToFarVolumeTileKey(canonical[index], key) ||
            !Producer::source_window(key, windows[index])) {
            return Error::InvalidWanted;
        }
    }
    std::copy_n(canonical.begin(), count, impl_->keys.begin());
    std::copy_n(windows.begin(), count, impl_->windows.begin());
    impl_->count = count;
    return Error::None;
}
FarWorldCapturePlan::Error FarWorldCapturePlan::SetWanted(double camera_x, double camera_z) {
    if (active()) {
        return Error::SourceRefused;
    }
    // The existing radius/edge ratios enumerate at most 9x9 candidates per
    // tier (405 total). Reserve the existing 512-slot bound once, avoiding
    // overlapping growth buffers in the canonical wanted policy.
    FarVolumeMemoryLease scratch(*pool_);
    if (!scratch.TryCharge(Producer::kSlots * sizeof(FarTileKey))) {
        return Error::AllocationFailure;
    }
    try {
        std::vector<FarTileKey> keys;
        keys.reserve(Producer::kSlots);
        if (ComputeFarWantedSet(camera_x, camera_z, keys) != FarWantedError::None) {
            return Error::InvalidWanted;
        }
        return SetWanted(keys);
    } catch (const std::bad_alloc&) {
        return Error::AllocationFailure;
    }
}
std::span<const FarTileKey> FarWorldCapturePlan::wanted_keys() const {
    return {impl_->keys.data(), impl_->count};
}
std::span<const Save::AuthorityWindow> FarWorldCapturePlan::source_windows() const {
    return {impl_->windows.data(), impl_->count};
}
FarWorldCapturePlan::Result
FarWorldCapturePlan::Start(const std::shared_ptr<Producer>& producer,
                           const std::shared_ptr<const Systems::SHIELD_WorldSystem>& world,
                           const Producer::FrozenPolicy& policy,
                           const FarFrozenCaptureLimits& sampling,
                           const Limits& limits) {
    if (active()) {
        return {Error::SourceRefused};
    }
    const auto& bounds = limits.spans;
    constexpr auto guard = kFarVolumeExactCoordinateLimitMeters - 1024;
    if (limits.tiles == 0 || limits.tiles > Producer::kSlots || limits.operations == 0 ||
        !std::isfinite(bounds.min_y) || !std::isfinite(bounds.max_y) ||
        bounds.min_y > bounds.max_y || bounds.min_y <= -guard || bounds.max_y >= guard ||
        !std::isfinite(bounds.max_extent_meters) || bounds.max_extent_meters <= 0) {
        return {Error::InvalidLimits};
    }
    if (impl_->count > limits.tiles) {
        return {Error::Capacity};
    }
    if (!producer || !world || producer->frozen_active() || !producer->capture() ||
        producer->capture_status().active || producer->capture_status().paused ||
        producer->capture_status().error != Producer::Error::None) {
        return {Error::SourceRefused};
    }
    if (impl_->accepted &&
        (impl_->accepted_producer.lock() != producer || impl_->accepted_world.lock() != world)) {
        return {Error::SourceChanged};
    }
    try {
        auto pending = std::allocate_shared<Image>(FarVolumePoolAllocator<Image>(pool_), pool_);
        // count was admitted before allocation; it cannot exceed 512 descriptors.
        pending->tiles.reserve(impl_->count);
        pending->spans.reserve(Producer::kSlots);
        pending->source_serial = producer->capture()->serial;
        pending->committed_generation = producer->capture()->committed.generation;
        impl_->pending = std::move(pending);
    } catch (const std::bad_alloc&) {
        return {Error::AllocationFailure};
    }
    impl_->producer = producer;
    impl_->world = world;
    impl_->generation = world->water_seed_terrain_generation();
    impl_->policy = policy;
    impl_->sampling = sampling;
    auto& aggregate = impl_->aggregate.emplace(
        FarFrozenCaptureWork::Counts{
            sampling.max_height_samples, sampling.max_density_samples, sampling.max_water_samples},
        sampling.work,
        sampling.deadline);
    // The producer creates a fresh per-tile counter from sampling's image caps.
    // Across tiles, charge the supplied transaction directly; applying the image
    // caps again here would refuse a valid multi-tile discovery. Without an
    // injected transaction, retain the existing finite aggregate allowance.
    // The aggregate still checks both deadlines between tiles in Pump().
    impl_->sampling.work = sampling.work ? sampling.work : &aggregate;
    impl_->limits = limits;
    impl_->operations = 0;
    impl_->cursor = 0;
    impl_->history_cursor = 0;
    impl_->wanted_cursor = 0;
    impl_->history_ready = false;
    return {};
}
FarWorldCapturePlan::Result FarWorldCapturePlan::Pump(std::uint64_t allowance) {
    if (!active()) {
        return {Error::Inactive};
    }
    Result result;
    while (result.work < allowance) {
        if (!impl_->Current()) {
            result.error = Error::SourceChanged;
            break;
        }
        auto& aggregate = impl_->aggregate;
        if (!aggregate.has_value() || !aggregate->CheckDeadline() ||
            impl_->producer->capture()->deadline.expired()) {
            result.error = Error::SourceRefused;
            result.source.capture = FarFrozenCaptureError::DeadlineExceeded;
            break;
        }
        if (impl_->operations == impl_->limits.operations) {
            result.error = Error::WorkLimit;
            break;
        }
        ++result.work;
        ++impl_->operations;
        if (!impl_->history_ready) {
            const auto history = impl_->accepted ? std::span<const Tile>(impl_->accepted->spans)
                                                 : std::span<const Tile>{};
            if (impl_->history_cursor == history.size() && impl_->wanted_cursor == impl_->count) {
                impl_->history_ready = true;
                continue;
            }
            if (impl_->pending->spans.size() == Producer::kSlots) {
                result.error = Error::Capacity;
                break;
            }
            if (impl_->wanted_cursor == impl_->count ||
                (impl_->history_cursor < history.size() &&
                 history[impl_->history_cursor].key < impl_->keys[impl_->wanted_cursor])) {
                impl_->pending->spans.push_back(history[impl_->history_cursor++]);
            } else {
                const auto wanted = impl_->keys[impl_->wanted_cursor++];
                if (impl_->history_cursor < history.size() &&
                    history[impl_->history_cursor].key == wanted) {
                    impl_->pending->spans.push_back(history[impl_->history_cursor++]);
                } else {
                    impl_->pending->spans.push_back({wanted, {}, 0});
                }
            }
            continue;
        }
        if (impl_->cursor == impl_->count) {
            impl_->accepted = impl_->pending;
            impl_->accepted_producer = impl_->producer;
            impl_->accepted_world = impl_->world;
            result.complete = true;
            break;
        }
        const auto key = impl_->keys[impl_->cursor];
        if (impl_->tile_ready) {
            Tile tile{key, impl_->request, 0};
            if (!impl_->AdmitSpan(tile)) {
                result.error = Error::InvalidSpan;
                break;
            }
            impl_->pending->tiles.push_back(tile);
            const auto found = std::lower_bound(
                impl_->pending->spans.begin(),
                impl_->pending->spans.end(),
                tile.key,
                [](const Tile& entry, const FarTileKey& item) { return entry.key < item; });
            if (found == impl_->pending->spans.end() || found->key != tile.key) {
                result.error = Error::InvalidWanted;
                break;
            }
            *found = tile;
            impl_->tile_ready = false;
            ++impl_->cursor;
        } else if (impl_->sampling_active) {
            const auto sampled = impl_->producer->pump_coverage(1, impl_->request);
            result.source = sampled.result;
            if (!sampled.result.ok()) {
                result.error = Error::SourceRefused;
                break;
            }
            if (sampled.complete) {
                impl_->sampling_active = false;
                impl_->tile_ready = true;
            }
        } else {
            FarVolumeTileKey volume_key;
            if (!ToFarVolumeTileKey(key, volume_key)) {
                result.error = Error::InvalidWanted;
                break;
            }
            result.source = impl_->producer->start_coverage(
                impl_->world, {volume_key, {}}, impl_->policy, impl_->sampling);
            if (!result.source.ok()) {
                result.error = Error::SourceRefused;
                break;
            }
            impl_->sampling_active = true;
        }
    }
    if (!result.ok() || result.complete) {
        Cancel();
    }
    return result;
}
void FarWorldCapturePlan::Cancel() {
    if (impl_->sampling_active) {
        impl_->producer->cancel_frozen();
    }
    impl_->sampling_active = false;
    impl_->tile_ready = false;
    impl_->pending.reset();
    impl_->producer.reset();
    impl_->world.reset();
    impl_->aggregate.reset();
}
void FarWorldCapturePlan::Reset() {
    Cancel();
    impl_->accepted.reset();
    impl_->accepted_producer.reset();
    impl_->accepted_world.reset();
    impl_->count = 0;
}
bool FarWorldCapturePlan::active() const {
    return impl_->pending != nullptr;
}
std::shared_ptr<const FarWorldCapturePlan::Image> FarWorldCapturePlan::accepted() const {
    return impl_->accepted;
}
} // namespace Luminumbra::World
