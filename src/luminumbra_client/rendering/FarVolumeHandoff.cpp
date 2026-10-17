#include "FarVolumeHandoff.h"
#include "core/JobSystem.h"
#include "luminumbra_client/rendering/FarVolumeBuildQueue.h"
#include "luminumbra_client/rendering/FarVolumeResidency.h"
#include "luminumbra_client/rendering/FarVolumeUpload.h"
#include "luminumbra_client/rendering/FarWorldWaterMesh.h"
#include "world/Chunk.h"
#include "world/FarEditedBake.h"
#include "world/FarFrozenObservation.h"
#include "world/FarTierTable.h"
#include "world/FarVolume.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeRuntime.h"
#include "world/FarVolumeTierMemory.h"
#include "world/FarVolumeTile.h"
#include "world/FarWantedSet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
// The admitted wrapper delegates all water semantics to the existing frozen
// bridge. Its private type cannot be relabeled from an uncharged caller object.
class HandoffWater final : public FarWorldWaterObservation {
public:
    HandoffWater(std::shared_ptr<const World::FarFrozenObservation> image,
                 const World::FarTileKey& key)
        : image_(std::move(image))
        , water_(image_, key) {}
    const World::FarFrozenObservation* observation() const noexcept {
        return image_.get();
    }
    std::uint64_t content_identity() const noexcept override {
        return water_.content_identity();
    }
    FarWorldWaterCapability WaterCapability() const noexcept override {
        return water_.WaterCapability();
    }
    std::optional<FarWorldWaterBound> WaterBound(const World::FarTileKey& key) const override {
        return water_.WaterBound(key);
    }
    std::optional<FarWorldWaterSample> WaterSample(std::int64_t x, std::int64_t z) const override {
        return water_.WaterSample(x, z);
    }

private:
    std::shared_ptr<const World::FarFrozenObservation> image_;
    FarWorldFrozenWaterObservation water_;
};
template<class T>
class WaterAllocator {
public:
    using value_type = T;
    explicit WaterAllocator(std::shared_ptr<World::FarVolumeMemoryPool> p)
        : pool_(std::move(p)) {}
    template<class U>
    WaterAllocator(const WaterAllocator<U>& other)
        : pool_(other.Pool()) {}
    T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T) ||
            !pool_->TryReserve(count * sizeof(T)))
            throw World::FarVolumeMemoryRefused();
        try {
            return std::allocator<T>{}.allocate(count);
        } catch (...) {
            pool_->Release(count * sizeof(T));
            throw;
        }
    }
    void deallocate(T* pointer, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
        pool_->Release(count * sizeof(T));
    }
    template<class U>
    bool operator==(const WaterAllocator<U>& other) const noexcept {
        return pool_ == other.Pool();
    }
    const std::shared_ptr<World::FarVolumeMemoryPool>& Pool() const noexcept {
        return pool_;
    }

private:
    std::shared_ptr<World::FarVolumeMemoryPool> pool_;
};
constexpr auto kEmptyIndex = std::numeric_limits<std::size_t>::max();
std::size_t KeyHash(const World::FarTileKey& key) noexcept {
    // Mix signed coordinates as unsigned bits, including negative child keys.
    const auto mix = [](std::uint64_t value) {
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    };
    return static_cast<std::size_t>(
        mix(static_cast<std::uint64_t>(key.tx)) ^
        mix(static_cast<std::uint64_t>(key.tz) + 0x9e3779b97f4a7c15ULL));
}
bool Same(const FarVolumeWantedRequest& a, const FarVolumeWantedRequest& b) {
    return a.identity == b.identity && a.intent == b.intent &&
           a.build_headroom_bytes == b.build_headroom_bytes && a.water_bound == b.water_bound;
}
std::uint64_t UploadBytes(const FarVolumeQueueCompletion& completion) {
    const auto& mesh = completion.mesh();
    return mesh.vertices.size() * sizeof(VoxelVertex) + mesh.indices.size() * sizeof(std::uint32_t);
}
World::FarVolumeMemoryLease AdmitControl(World::FarVolumeTierMemory& memory) {
    World::FarVolumeMemoryLease control(*memory.Pool(World::kFarTierCount));
    if (!control.TryCharge(sizeof(FarVolumeHandoff)))
        throw World::FarVolumeMemoryRefused();
    return control;
}
} // namespace
std::optional<FarHandoffTileInput>
MakeFarHandoffTileInput(FarVolumeWantedRequest wanted,
                        std::shared_ptr<const World::FarFrozenObservation> image) {
    if (!image)
        return {};
    try {
        auto water = std::allocate_shared<HandoffWater>(
            WaterAllocator<HandoffWater>(image->memory_pool_handle()), image, wanted.identity.key);
        wanted.water = std::move(water);
        wanted.water_bound = wanted.water->WaterBound(wanted.identity.key);
        return FarHandoffTileInput{std::move(wanted), std::move(image)};
    } catch (...) {
        return {};
    }
}
class FarVolumeHandoff::Guard {
public:
    explicit Guard(FarVolumeHandoff& owner)
        : owner_(owner) {
        owner.CheckOwner();
        if (owner.active_)
            throw std::logic_error("Far-volume handoff reentrancy");
        owner.active_ = true;
    }
    ~Guard() {
        owner_.active_ = false;
    }

private:
    FarVolumeHandoff& owner_;
};
std::size_t FarVolumeHandoff::Tier::IndexSize(std::size_t count,
                                              const FarVolumeQueueLimits& limits) {
    if (count == 0 || count > limits.descriptor_slots || limits.result_slots == 0)
        throw std::invalid_argument("Far-volume handoff needs bounded entries/results");
    constexpr auto bytes_per_entry =
        std::max({sizeof(Entry), sizeof(World::FarVolumeRequest), 2 * sizeof(std::size_t)});
    if (count > std::numeric_limits<std::size_t>::max() / bytes_per_entry)
        throw World::FarVolumeMemoryRefused();
    return count * 2;
}
FarVolumeHandoff::Tier::Tier(JobSystem& jobs,
                             World::FarVolumeMemoryPool& pool,
                             std::size_t count,
                             const FarVolumeQueueLimits& limits,
                             std::pmr::memory_resource* upstream)
    : charge(pool)
    , resource(charge, upstream)
    , entries(&resource)
    , key_index(IndexSize(count, limits), kEmptyIndex, &resource)
    , order(&resource)
    , validation(&resource)
    , queue(jobs, pool, limits, upstream)
    , completion_slots(limits.result_slots)
    , authority_limits(limits.compilation.authority) {
    entries.resize(count);
    order.reserve(count);
    validation.resize(count);
}
void FarVolumeHandoff::Tier::RebuildIndex() {
    std::fill(key_index.begin(), key_index.end(), kEmptyIndex);
    // Staging only: at most entry_slots insertions into 2 * entry_slots buckets.
    // Rebuilding preserves collision chains without retaining deleted buckets.
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& key = entries[index].key;
        if (!key)
            continue;
        auto bucket = KeyHash(*key) % key_index.size();
        while (key_index[bucket] != kEmptyIndex)
            bucket = (bucket + 1) % key_index.size();
        key_index[bucket] = index;
    }
}
FarVolumeHandoff::FarVolumeHandoff(JobSystem& jobs,
                                   World::FarVolumeTierMemory& memory,
                                   const FarVolumeResidencyLimits& limits,
                                   std::pmr::memory_resource* upstream)
    : FarVolumeHandoff(jobs, memory, limits, AdmitControl(memory), upstream) {}
FarVolumeHandoff::FarVolumeHandoff(JobSystem& jobs,
                                   World::FarVolumeTierMemory& memory,
                                   const FarVolumeResidencyLimits& limits,
                                   World::FarVolumeMemoryLease&& control,
                                   std::pmr::memory_resource* upstream)
    : memory_(memory)
    , control_(std::move(control)) {
    for (std::size_t i = 1; i <= World::kFarTierCount; ++i)
        if (memory.Pool(i)->limit() == 0 ||
            memory.Pool(i)->limit() == std::numeric_limits<std::uint64_t>::max())
            throw std::invalid_argument("Far-volume handoff needs finite memory budgets");
    if (control_.pool() != memory.Pool(World::kFarTierCount) || control_.bytes() != sizeof(*this))
        throw std::invalid_argument("Far-volume handoff control admission mismatch");
    for (std::size_t i = 0; i < tiers_.size(); ++i)
        tiers_[i].emplace(
            jobs, *memory.Pool(i + 1), limits.entry_slots[i], limits.queues[i], upstream);
}
FarVolumeHandoff::~FarVolumeHandoff() {
    try {
        CancelAndDrain();
    } catch (...) {
        std::terminate();
    }
}
void FarVolumeHandoff::CheckOwner() const {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("Far-volume handoff outside owner thread");
}
const FarVolumeHandoff::Entry* FarVolumeHandoff::Find(const World::FarTileKey& key) const {
    if (key.tier == 0 || key.tier > tiers_.size())
        return nullptr;
    const auto& tier = tiers_[key.tier - 1];
    if (!tier)
        return nullptr;
    auto bucket = KeyHash(key) % tier->key_index.size();
    while (tier->key_index[bucket] != kEmptyIndex) {
        const auto& entry = tier->entries[tier->key_index[bucket]];
        if (entry.key == key)
            return &entry;
        bucket = (bucket + 1) % tier->key_index.size();
    }
    return nullptr;
}
FarVolumeHandoff::Entry* FarVolumeHandoff::Find(const World::FarTileKey& key) {
    return const_cast<Entry*>(std::as_const(*this).Find(key));
}
const FarVolumeUploadedTile* FarVolumeHandoff::Pair(const Entry& entry,
                                                    FarHandoffCoverageView view) const {
    if (view == FarHandoffCoverageView::Serving)
        return entry.serving ? &entry.serving_pair : nullptr;
    if (!entry.target)
        return nullptr;
    const auto& request = entry.target->wanted;
    if (entry.serving && Same(*entry.serving, request) &&
        World::IsFarTileCovering(entry.serving_pair.Status(request.identity, request.intent)))
        return &entry.serving_pair;
    return &entry.staged_pair;
}
bool FarVolumeHandoff::Covers(const Entry& entry, FarHandoffCoverageView view) const {
    const auto* pair = Pair(entry, view);
    if (!pair || !pair->water_complete())
        return false;
    if (view == FarHandoffCoverageView::Serving) {
        if (!entry.serving)
            return false;
        return World::IsFarTileCovering(
            pair->Status(entry.serving->identity, entry.serving->intent));
    }
    if (!entry.target)
        return false;
    const auto& request = entry.target->wanted;
    return World::IsFarTileCovering(pair->Status(request.identity, request.intent));
}
bool FarVolumeHandoff::ValidateInput(const FarHandoffManifest& manifest,
                                     const FarHandoffTileInput& input,
                                     const Tier& tier,
                                     World::FarVolumeRequest& resolved,
                                     bool duplicate) const {
    if (!input.observation)
        return false;
    const auto& wanted = input.wanted;
    const auto& identity = wanted.identity;
    const auto& key = identity.key;
    const auto& image = *input.observation;
    const auto* water = dynamic_cast<const HandoffWater*>(wanted.water.get());
    if (image.use() != World::FarFrozenUse::Committed ||
        image.memory_pool() != memory_.Pool(key.tier) ||
        image.durable_identity().state != World::FarEditedImageState::Committed ||
        image.durable_identity().generation != manifest.committed_generation ||
        image.caves() != manifest.caves || image.numeric() != manifest.numeric ||
        image.content_identity() != identity.content_hash || !water ||
        water->observation() != &image || water->content_identity() != identity.content_hash ||
        water->WaterCapability() == FarWorldWaterCapability::Unavailable ||
        water->WaterBound(key) != wanted.water_bound)
        return false;
    const auto* old = Find(key);
    if (duplicate && (!old || !old->target || !Same(old->target->wanted, wanted)))
        return false;
    if (old && old->history && old->history->identity == identity && !Same(*old->history, wanted))
        return false;
    if (old && old->target && old->target->wanted.identity == identity &&
        !Same(old->target->wanted, wanted))
        return false;
    if (old && old->serving && old->serving->identity == identity && !Same(*old->serving, wanted))
        return false;
    if (duplicate) {
        if (!old || !old->target)
            return false;
        if (old->target->observation != input.observation)
            return false;
        return true;
    }
    World::FarVolumeAuthority authority;
    try {
        authority = image.Authority(key.tier);
    } catch (...) {
        return false;
    }
    if (!World::ResolveFarVolumeCoverageCharged(wanted.intent,
                                                authority,
                                                tier.authority_limits,
                                                {memory_.Pool(key.tier), nullptr, nullptr},
                                                resolved))
        return false;
    const auto edge = World::FarTileEdgeMeters(key.tier);
    bool support = false;
    for (std::size_t r = 0; r < image.region_count(); ++r) {
        const auto region = image.Region(r);
        if (!region)
            return false;
        if (region->tier == key.tier && region->support.min.x == key.tx * edge &&
            region->support.min.z == key.tz * edge &&
            region->support.max.x == (key.tx + 1) * edge &&
            region->support.max.z == (key.tz + 1) * edge &&
            region->support.min.y <= resolved.span.min_y &&
            region->support.max.y >= resolved.span.max_y)
            support = true;
    }
    const auto bound = wanted.water_bound;
    if (!support || !bound || !std::isfinite(bound->min_y) || !std::isfinite(bound->max_y) ||
        bound->min_y > bound->max_y ||
        (!bound->known_dry && (double(bound->min_y) < double(resolved.span.min_y) ||
                               double(bound->max_y) > double(resolved.span.max_y))))
        return false;
    if (old && old->history &&
        (resolved.span.min_y > old->history_span.span.min_y ||
         resolved.span.max_y < old->history_span.span.max_y ||
         (old->history->intent.extra_span &&
          (!wanted.intent.extra_span ||
           wanted.intent.extra_span->min_y > old->history->intent.extra_span->min_y ||
           wanted.intent.extra_span->max_y < old->history->intent.extra_span->max_y))))
        return false;
    return true;
}
bool FarVolumeHandoff::StageTarget(const FarHandoffManifest& manifest) {
    Guard const guard(*this);
    if (manifest.world_epoch == 0 || manifest.publication_serial == 0 ||
        (manifest.caves != World::FarCaveMode::BandLimited &&
         manifest.caves != World::FarCaveMode::BoxFiltered) ||
        (manifest.numeric != World::FarVolumeNumericProfile::RawFloat &&
         manifest.numeric != World::FarVolumeNumericProfile::LegacySigned16) ||
        (state_.world_epoch && (manifest.world_epoch != state_.world_epoch ||
                                manifest.caves != caves_ || manifest.numeric != numeric_)) ||
        manifest.publication_serial < state_.target_serial ||
        manifest.committed_generation < state_.committed_generation)
        return false;
    const bool duplicate = manifest.publication_serial == state_.target_serial;
    if (duplicate && manifest.committed_generation != state_.committed_generation)
        return false;
    std::array<std::size_t, World::kFarTierCount> counts{}, new_keys{};
    std::size_t fallbacks = 0, old_count = 0;
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        tier->order.assign(tier->entries.size(), 0);
        for (const auto& entry : tier->entries)
            old_count += entry.target.has_value();
    }
    if (duplicate && old_count != manifest.tiles.size())
        return false;
    // Only charged, preallocated validation scratch changes before commit.
    for (std::size_t index = 0; index < manifest.tiles.size(); ++index) {
        const auto& input = manifest.tiles[index];
        const auto& wanted = input.wanted;
        const auto& identity = wanted.identity;
        World::FarTileKey key;
        if (!World::ToFarTileKey(wanted.intent.key, key) || key != identity.key ||
            identity.world_epoch != manifest.world_epoch ||
            identity.cave_policy != static_cast<std::uint8_t>(manifest.caves) || !input.observation)
            return false;
        for (std::size_t j = 0; j < index; ++j)
            if (manifest.tiles[j].wanted.identity.key == key)
                return false;
        auto& slot = tiers_[key.tier - 1];
        if (!slot)
            throw std::logic_error("Far-volume handoff missing tier");
        auto& tier = *slot;
        if (++counts[key.tier - 1] > tier.entries.size())
            return false;
        fallbacks += key.tier == World::kFarTierCount;
        if (const auto* existing = Find(key))
            tier.order[static_cast<std::size_t>(existing - tier.entries.data())] = 1;
        else
            ++new_keys[key.tier - 1];
        if (!ValidateInput(
                manifest, input, tier, tier.validation.at(counts[key.tier - 1] - 1), duplicate))
            return false;
    }
    if (fallbacks == 0)
        return false;
    for (std::size_t i = 0; i < tiers_.size(); ++i) {
        const auto& tier = tiers_[i];
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        std::size_t free = 0;
        for (std::size_t index = 0; index < tier->entries.size(); ++index) {
            const auto& entry = tier->entries[index];
            // Omitted target-only entries retire at commit. Serving entries and
            // every incoming key retain their descriptors and span history.
            free += !entry.key || (!entry.serving && tier->order[index] == 0);
        }
        if (new_keys[i] > free)
            return false;
    }
    if (duplicate)
        return true;
    // Accept descriptors only after the entire bounded manifest validates.
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        for (auto& entry : tier->entries) {
            if (!entry.target)
                continue;
            const auto key = entry.target->wanted.identity.key;
            if (std::none_of(manifest.tiles.begin(), manifest.tiles.end(), [&](const auto& input) {
                    return input.wanted.identity.key == key;
                })) {
                tier->queue.Forget(key);
                entry.completion.reset();
                entry.staged_pair = {};
                entry.target.reset();
            }
        }
    }
    for (std::size_t i = 0; i < tiers_.size(); ++i) {
        auto& slot = tiers_[i];
        if (!slot)
            throw std::logic_error("Far-volume handoff missing tier");
        auto& tier = *slot;
        auto free = static_cast<std::size_t>(
            std::count_if(tier.entries.begin(), tier.entries.end(), [](const auto& entry) {
                return !entry.key;
            }));
        bool reclaimed = false;
        // Only enough idle history to admit the validated manifest is replaced,
        // in stable slot order. Work and storage are bounded by entry_slots.
        for (std::size_t index = 0; free < new_keys[i] && index < tier.entries.size(); ++index) {
            auto& entry = tier.entries[index];
            if (!entry.key || entry.serving || entry.target || tier.order[index] != 0)
                continue;
            entry = Entry{};
            ++free;
            reclaimed = true;
        }
        if (reclaimed)
            tier.RebuildIndex();
    }
    counts = {};
    for (const auto& input : manifest.tiles) {
        const auto& wanted = input.wanted;
        auto& slot = tiers_[wanted.identity.key.tier - 1];
        if (!slot)
            throw std::logic_error("Far-volume handoff missing tier");
        auto& tier = *slot;
        auto* entry = Find(wanted.identity.key);
        if (!entry) {
            const auto free = std::find_if(
                tier.entries.begin(), tier.entries.end(), [](const auto& e) { return !e.key; });
            if (free == tier.entries.end())
                throw std::logic_error("Far-volume handoff missing admitted entry");
            entry = &*free;
            auto bucket = KeyHash(wanted.identity.key) % tier.key_index.size();
            while (tier.key_index[bucket] != kEmptyIndex)
                bucket = (bucket + 1) % tier.key_index.size();
            tier.key_index[bucket] = static_cast<std::size_t>(free - tier.entries.begin());
        }
        const bool same = entry->target && Same(entry->target->wanted, wanted);
        if (!same) {
            tier.queue.Forget(wanted.identity.key);
            entry->completion.reset();
            entry->staged_pair = {};
        }
        if (!same || entry->eligibility == FarVolumeEligibility::RequestRejected) {
            tier.queue.Forget(wanted.identity.key);
            entry->eligibility = FarVolumeEligibility::Wanted;
            entry->build_failure.reset();
            entry->upload_failure.reset();
        }
        entry->key = wanted.identity.key;
        entry->target = input;
        entry->resolved = tier.validation[counts[wanted.identity.key.tier - 1]++];
        auto& history = entry->history.emplace(wanted);
        history.water.reset();
        entry->history_span = entry->resolved;
        if (Covers(*entry, FarHandoffCoverageView::Target))
            entry->eligibility = FarVolumeEligibility::Settled;
    }
    state_.world_epoch = manifest.world_epoch;
    state_.committed_generation = manifest.committed_generation;
    state_.target_serial = manifest.publication_serial;
    state_.awaiting_initial_fallback = state_.serving_serial == 0;
    state_.awaiting_replacement_fallback = state_.serving_serial != 0;
    caves_ = manifest.caves;
    numeric_ = manifest.numeric;
    return true;
}
void FarVolumeHandoff::Reject(Entry& entry, FarVolumeFrameResult& result) {
    if (!entry.key)
        throw std::logic_error("Far-volume handoff missing key");
    auto& tier = tiers_[entry.key->tier - 1];
    if (!tier)
        throw std::logic_error("Far-volume handoff missing tier");
    tier->queue.Forget(*entry.key);
    entry.completion.reset();
    entry.eligibility = FarVolumeEligibility::RequestRejected;
    ++result.rejected;
    result.horizon_refused |= entry.key->tier == World::kFarTierCount;
}
FarVolumeFrameResult FarVolumeHandoff::AdvanceFrame(double x,
                                                    double z,
                                                    const FarVolumeFrameLimits& limits,
                                                    const FarVolumeUploadBackend& backend) {
    Guard const guard(*this);
    FarVolumeFrameResult result;
    if (!std::isfinite(x) || !std::isfinite(z) ||
        std::abs(x) > World::kFarWantedCameraLimitMeters ||
        std::abs(z) > World::kFarWantedCameraLimitMeters)
        return result;
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        tier->queue.Pump(0);
        tier->order.clear();
        std::size_t held = 0;
        for (const auto& entry : tier->entries)
            held += entry.completion.has_value();
        for (std::size_t i = 0; i < tier->entries.size(); ++i) {
            auto& entry = tier->entries[i];
            if (!entry.target)
                continue;
            tier->order.push_back(i);
            entry.distance = World::FarTileNearestDistance(entry.target->wanted.identity.key, x, z);
            if ((entry.eligibility == FarVolumeEligibility::Building ||
                 entry.eligibility == FarVolumeEligibility::Pending) &&
                held < tier->completion_slots) {
                const auto& request = entry.target->wanted;
                auto completion = tier->queue.TakeCompleted(request.identity, request.intent);
                if (completion) {
                    tier->queue.Forget(request.identity.key);
                    if ((completion->status() == FarVolumeBuildStatus::Ready ||
                         completion->status() == FarVolumeBuildStatus::ReadyEmpty) &&
                        completion->identity() == request.identity) {
                        entry.completion.emplace(std::move(*completion));
                        entry.eligibility = FarVolumeEligibility::Ready;
                        ++held;
                    } else {
                        entry.build_failure = completion->status();
                        Reject(entry, result);
                    }
                }
            }
        }
        const auto& entries = tier->entries;
        std::sort(tier->order.begin(), tier->order.end(), [&](auto a, auto b) {
            const auto& aa = entries[a];
            const auto& bb = entries[b];
            if (!aa.key || !bb.key)
                throw std::logic_error("Far-volume handoff missing key");
            return aa.distance != bb.distance ? aa.distance < bb.distance : *aa.key < *bb.key;
        });
    }
    for (std::size_t n = tiers_.size(); n > 0; --n) {
        auto& slot = tiers_[n - 1];
        if (!slot)
            throw std::logic_error("Far-volume handoff missing tier");
        auto& tier = *slot;
        for (auto i : tier.order) {
            auto& entry = tier.entries[i];
            if (!entry.target || !entry.completion || result.upload_attempts == limits.uploads)
                continue;
            const auto bytes = UploadBytes(*entry.completion);
            if (bytes > limits.upload_bytes) {
                ++result.upload_attempts;
                entry.upload_failure = FarVolumeUploadError::ByteLimit;
                Reject(entry, result);
                continue;
            }
            if (bytes > limits.upload_bytes - result.upload_bytes)
                continue;
            ++result.upload_attempts;
            const auto& request = entry.target->wanted;
            auto* pool = memory_.Pool(n);
            if (bytes > pool->limit() || pool->used() > pool->limit() - bytes) {
                entry.upload_failure = FarVolumeUploadError::AdmissionRefused;
                Reject(entry, result);
                continue;
            }
            const auto remaining = limits.upload_bytes - result.upload_bytes;
            // Attempt allowance is independent of retained GPU admission.
            result.upload_bytes += bytes;
            FarVolumeUploadError error = FarVolumeUploadError::Stale;
            if (entry.completion->identity() != request.identity ||
                !UploadFarVolume(*entry.completion,
                                 request.identity,
                                 request.intent,
                                 memory_,
                                 remaining,
                                 backend,
                                 entry.staged_pair,
                                 &error)) {
                entry.upload_failure = error;
                Reject(entry, result);
                continue;
            }
            entry.completion.reset();
            entry.eligibility = FarVolumeEligibility::Settled;
            ++result.uploaded;
            result.successful_upload_bytes += bytes;
        }
    }
    for (std::size_t n = tiers_.size(); n > 0; --n) {
        auto& slot = tiers_[n - 1];
        if (!slot)
            throw std::logic_error("Far-volume handoff missing tier");
        auto& tier = *slot;
        // Upload/discard is the only point that releases a taken result's slot.
        std::size_t held = 0;
        for (const auto& entry : tier.entries)
            held += entry.completion.has_value();
        for (auto i : tier.order) {
            auto& entry = tier.entries[i];
            if (!entry.target || result.build_attempts == limits.builds ||
                (entry.eligibility != FarVolumeEligibility::Wanted &&
                 entry.eligibility != FarVolumeEligibility::Pending))
                continue;
            if (tier.queue.ResultCapacity(held) == 0)
                break;
            ++result.build_attempts;
            const auto& request = entry.target->wanted;
            const std::array key{request.identity.key};
            if (entry.eligibility == FarVolumeEligibility::Wanted) {
                auto* pool = memory_.Pool(n);
                if (request.build_headroom_bytes > pool->limit() ||
                    pool->used() > pool->limit() - request.build_headroom_bytes) {
                    Reject(entry, result);
                    continue;
                }
                try {
                    const auto status =
                        tier.queue.Submit({request.identity,
                                           request.intent,
                                           entry.target->observation->Authority(key[0].tier),
                                           request.water,
                                           request.water_bound});
                    if (status != FarVolumeSubmitStatus::Queued &&
                        status != FarVolumeSubmitStatus::Coalesced &&
                        status != FarVolumeSubmitStatus::Duplicate) {
                        entry.build_failure = FarVolumeBuildStatus::DispatchRejected;
                        Reject(entry, result);
                        continue;
                    }
                } catch (...) {
                    entry.build_failure = FarVolumeBuildStatus::DispatchRejected;
                    Reject(entry, result);
                    continue;
                }
            }
            const auto dispatched = tier.queue.Pump(1, key, held);
            result.dispatched += dispatched;
            entry.eligibility =
                dispatched ? FarVolumeEligibility::Building : FarVolumeEligibility::Pending;
        }
    }
    return result;
}
bool FarVolumeHandoff::TryPromote() {
    Guard const guard(*this);
    if (state_.target_serial == 0)
        return false;
    const bool replacement = state_.serving_serial != state_.target_serial;
    if (replacement) {
        const auto& tier = tiers_.back();
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        for (const auto& entry : tier->entries)
            if (entry.target && !Covers(entry, FarHandoffCoverageView::Target))
                return false;
    }
    bool changed = replacement;
    // All prerequisites checked; fixed entries also supply retirement scratch.
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        for (auto& entry : tier->entries) {
            if (!replacement &&
                (!entry.target || entry.serving || !Covers(entry, FarHandoffCoverageView::Target)))
                continue;
            if (entry.target && Covers(entry, FarHandoffCoverageView::Target)) {
                if (Pair(entry, FarHandoffCoverageView::Target) != &entry.serving_pair) {
                    entry.retired_pair = std::move(entry.serving_pair);
                    entry.serving_pair = std::move(entry.staged_pair);
                }
                entry.serving = entry.target->wanted;
                changed = true;
            } else {
                entry.retired_pair = std::move(entry.serving_pair);
                entry.serving.reset();
            }
        }
    }
    if (!changed)
        return false;
    state_.last_promotion = replacement
                                ? (state_.serving_serial ? FarHandoffPromotion::ReplacementFallback
                                                         : FarHandoffPromotion::InitialFallback)
                                : FarHandoffPromotion::FinerArrival;
    state_.serving_serial = state_.target_serial;
    state_.serving_committed_generation = state_.committed_generation;
    ++state_.promotions;
    state_.awaiting_initial_fallback = state_.awaiting_replacement_fallback = false;
    // Publication precedes destruction; no upload, allocation or wait in commit.
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        for (auto& entry : tier->entries)
            entry.retired_pair = {};
    }
    return true;
}
std::optional<FarHandoffTileObservation>
FarVolumeHandoff::Observe(const World::FarTileKey& key) const {
    CheckOwner();
    const auto* entry = Find(key);
    if (!entry || (!entry->serving && !entry->target))
        return {};
    return ObserveEntry(*entry);
}
FarHandoffTileObservation FarVolumeHandoff::ObserveEntry(const Entry& entry) const {
    FarHandoffTileObservation result;
    result.serving = entry.serving;
    if (entry.target)
        result.target = entry.target->wanted;
    // Diagnostics are values, never additional immutable authority owners.
    if (result.serving)
        result.serving->water.reset();
    if (result.target)
        result.target->water.reset();
    result.target_eligibility = entry.eligibility;
    result.target_build_failure = entry.build_failure;
    result.target_upload_failure = entry.upload_failure;
    for (const auto view : {FarHandoffCoverageView::Serving, FarHandoffCoverageView::Target}) {
        const auto* pair = Pair(entry, view);
        const auto& wanted =
            view == FarHandoffCoverageView::Serving ? result.serving : result.target;
        if (!wanted || !pair)
            continue;
        auto status = pair->Status(wanted->identity, wanted->intent);
        status.current &= pair->water_complete();
        if (view == FarHandoffCoverageView::Serving) {
            result.serving_coverage = status;
            result.serving_built = pair->request();
            result.serving_water_complete = pair->water_complete();
            result.serving_water_known_dry = pair->water_known_dry();
            result.serving_terrain_indices = pair->terrain_indices();
            result.serving_water_indices = pair->water_indices();
        } else {
            result.target_coverage = status;
            result.target_built = pair->request();
            result.target_water_complete = pair->water_complete();
            result.target_water_known_dry = pair->water_known_dry();
            result.target_terrain_indices = pair->terrain_indices();
            result.target_water_indices = pair->water_indices();
        }
    }
    result.serving_gpu_bytes = entry.serving_pair.gpu_bytes();
    result.staged_gpu_bytes = entry.staged_pair.gpu_bytes();
    if (entry.serving) {
        const auto* water = dynamic_cast<const HandoffWater*>(entry.serving->water.get());
        if (water)
            result.serving_source_payload_bytes = water->observation()->payload_bytes();
    }
    if (entry.target)
        result.target_source_payload_bytes = entry.target->observation->payload_bytes();
    if (entry.completion) {
        result.target_built = entry.completion->mesh().request;
        result.ready_cpu_bytes = entry.completion->retained_bytes();
        result.ready_upload_bytes = UploadBytes(*entry.completion);
    }
    return result;
}
bool FarVolumeHandoff::CoarseMayYield(const World::FarTileKey& key,
                                      FarHandoffCoverageView view) const {
    CheckOwner();
    const auto* coarse = Find(key);
    if (!coarse || key.tier <= 1 || key.tier > World::kFarTierCount ||
        !(view == FarHandoffCoverageView::Serving ? bool(coarse->serving) : bool(coarse->target)))
        return false;
    for (std::int64_t z = 0; z < 2; ++z)
        for (std::int64_t x = 0; x < 2; ++x) {
            const auto* child = Find({std::uint8_t(key.tier - 1), key.tx * 2 + x, key.tz * 2 + z});
            if (!child || !Covers(*child, view))
                return false;
        }
    return true;
}
std::size_t FarVolumeHandoff::VisitServingDraws(FarVolumeDrawVisitor visitor, void* context) {
    Guard const guard(*this);
    if (!visitor)
        return 0;
    std::size_t count = 0;
    for (std::size_t n = tiers_.size(); n > 0; --n) {
        const auto& tier = tiers_[n - 1];
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        for (const auto& entry : tier->entries) {
            if (!entry.serving || !Covers(entry, FarHandoffCoverageView::Serving) ||
                !entry.serving_pair.resource())
                continue;
            const auto& identity = entry.serving->identity;
            const auto& pair = entry.serving_pair;
            const auto edge = World::FarTileEdgeMeters(n);
            FarVolumeDrawRecord record{
                identity,
                pair.request(),
                {identity.key.tx * edge, pair.request().span.min_y, identity.key.tz * edge},
                pair.resource(),
                World::FarWantedRadiusMeters(n),
                double(World::kFarTierTable[n - 1].sample_spacing_meters) / 2,
                {}};
            record.terrain_indices = pair.terrain_indices();
            record.water_indices = pair.water_indices();
            record.arrival.eligible = CoarseMayYield(identity.key, FarHandoffCoverageView::Serving);
            if (record.arrival.eligible) {
                record.arrival.finer_outer_radius_meters = World::FarWantedRadiusMeters(n - 1);
                for (std::int64_t z = 0; z < 2; ++z)
                    for (std::int64_t x = 0; x < 2; ++x) {
                        const auto* child = Find({std::uint8_t(n - 1),
                                                  identity.key.tx * 2 + x,
                                                  identity.key.tz * 2 + z});
                        record.arrival.children[std::size_t(x + 2 * z)] =
                            child->serving_pair.request().span;
                    }
            }
            visitor(context, record);
            ++count;
        }
    }
    return count;
}
void FarVolumeHandoff::VisitObservations(FarHandoffObservationVisitor visitor, void* context) {
    Guard const guard(*this);
    if (visitor) {
        for (const auto& tier : tiers_) {
            if (!tier)
                throw std::logic_error("Far-volume handoff missing tier");
            for (const auto& entry : tier->entries)
                if (entry.key && (entry.serving || entry.target))
                    visitor(context, *entry.key, ObserveEntry(entry));
        }
    }
}
FarHandoffState FarVolumeHandoff::State() const {
    CheckOwner();
    return state_;
}
std::array<World::FarVolumeTierMemoryStats, World::kFarTierCount> FarVolumeHandoff::Memory() const {
    CheckOwner();
    std::array<World::FarVolumeTierMemoryStats, World::kFarTierCount> result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto stats = memory_.Stats(i + 1);
        if (!stats)
            throw std::logic_error("Far-volume handoff missing memory statistics");
        result[i] = *stats;
    }
    return result;
}
void FarVolumeHandoff::CancelAndDrain() {
    Guard const guard(*this);
    // Drain every worker before either generation releases its input authority.
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        tier->queue.CancelAndDrain();
    }
    // Release each resource class across all tiers before the next class.
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        for (auto& entry : tier->entries) {
            entry.completion.reset();
            entry.staged_pair = {};
        }
    }
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        for (auto& entry : tier->entries) {
            entry.serving_pair = {};
            entry.retired_pair = {};
        }
    }
    // Observation handles release only after all completions and pairs.
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        for (auto& entry : tier->entries) {
            entry.target.reset();
            entry.serving.reset();
            entry.history.reset();
            entry = Entry{};
        }
        std::fill(tier->key_index.begin(), tier->key_index.end(), kEmptyIndex);
    }
    state_ = {};
}
} // namespace Luminumbra::Rendering
