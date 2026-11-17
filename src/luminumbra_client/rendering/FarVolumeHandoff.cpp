#include "FarVolumeHandoff.h"
#include "FarWorldDiagnostics.h"
#include "core/JobSystem.h"
#include "luminumbra_client/rendering/FarVolumeBuildQueue.h"
#include "luminumbra_client/rendering/FarVolumeResidency.h"
#include "luminumbra_client/rendering/FarVolumeUpload.h"
#include "luminumbra_client/rendering/FarWorldWaterMesh.h"
#include "systems/SHIELD_WorldSystem.h"
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
#include <new>
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
FarWorldRefusal SourceRefusal(World::FarWorldAuthorityProducer::Error error) {
    return FarWorldProducerRefusal({error, World::FarFrozenCaptureError::None});
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
struct FarVolumeHandoff::Admission {
    enum class Phase {
        CaptureStart,
        CapturePump,
        Index,
        Sort,
        SortCopy,
        Keys,
        Descriptors,
        Header,
        Columns,
        Regions,
        Tail
    };
    explicit Admission(World::FarVolumeMemoryLease&& charge)
        : control(std::move(charge))
        , resource(control)
        , inputs(&resource)
        , resolved(&resource)
        , scratch(&resource) {}
    // Direct manifests are bounded by the preallocated descriptor union; session
    // requests retain the producer's fixed 512-slot bound. All buffers are charged.
    World::FarVolumeMemoryLease control;
    World::FarVolumeChargedResource resource;
    std::pmr::vector<FarHandoffTileInput> inputs;
    std::array<FarSessionHandoffTile, World::FarWorldAuthorityProducer::kSlots> requests{};
    std::pmr::vector<World::FarVolumeRequest> resolved;
    // At most count indices, separate from AdvanceFrame's descriptor ordering.
    std::pmr::vector<std::size_t> scratch;
    FarHandoffManifest manifest;
    FarHandoffState before;
    World::FarVolumeCoverageCursor coverage;
    std::shared_ptr<World::FarWorldAuthorityProducer> producer;
    std::shared_ptr<const Systems::SHIELD_WorldSystem> world;
    std::shared_ptr<World::FarVolumeTierMemory> memory;
    World::FarFrozenWorldSource world_guard;
    World::FarWorldAuthorityProducer::FrozenPolicy policy;
    World::FarFrozenCaptureLimits limits{};
    std::optional<World::FarFrozenCaptureWork> work;
    World::FarFrozenCaptureWork* sampling = nullptr;
    std::size_t count = 0, tile = 0, region = 0, height_region = 0;
    std::uint64_t height_observations = 0;
    bool support = false, duplicate = false;
    std::array<std::size_t, World::kFarTierCount> counts{};
    std::array<bool, World::kFarTierCount> sorted{true, true, true, true, true};
    std::size_t tier = 0, cursor = 0, old_count = 0, fallbacks = 0;
    std::size_t width = 1, left = 0, middle = 0, right = 0, output = 0;
    Phase phase = Phase::Index;
    World::FarWorldAuthorityProducer::FrozenResult Failure(const FarHandoffState& state) const {
        using Error = World::FarWorldAuthorityProducer::Error;
        using CaptureError = World::FarFrozenCaptureError;
        if (producer) {
            const auto status = producer->capture_status();
            if (status.error != Error::None)
                return {status.error, CaptureError::None};
            const auto* source = producer->capture();
            if (source && source->deadline.expired())
                return {Error::SourceReadExpired, CaptureError::None};
            if (!world_guard.current())
                return {Error::None, CaptureError::IdentityMismatch};
            if (!source || status.paused || status.serving_serial != manifest.publication_serial ||
                source->committed.generation != manifest.committed_generation)
                return {Error::AuthorityUnavailable, CaptureError::None};
        }
        if (before.target_serial != state.target_serial || before.promotions != state.promotions ||
            before.world_epoch != state.world_epoch)
            return {Error::AuthorityUnavailable, CaptureError::None};
        return {};
    }
};
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
void FarVolumeHandoff::Tier::RebuildIndex(FarVolumeAdmissionWork& work) {
    index_count = 0;
    // At most entry_slots descriptors and O(entry_slots * log(entry_slots))
    // comparisons, independent of coordinates or hash collisions. No allocation.
    for (std::size_t index = 0; index < entries.size(); ++index) {
        ++work.descriptor_visits;
        if (entries[index].key)
            key_index[index_count++] = index;
    }
    std::sort(key_index.begin(),
              key_index.begin() + static_cast<std::ptrdiff_t>(index_count),
              [&](std::size_t a, std::size_t b) {
                  ++work.key_comparisons;
                  return *entries[a].key < *entries[b].key;
              });
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
FarVolumeHandoff::Tier& FarVolumeHandoff::GetTier(std::size_t index) {
    if (index >= tiers_.size())
        throw std::logic_error("Far-volume handoff invalid tier");
    auto& tier = tiers_[index];
    if (!tier)
        throw std::logic_error("Far-volume handoff missing tier");
    return *tier;
}
void FarVolumeHandoff::CheckOwner() const {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("Far-volume handoff outside owner thread");
}
const FarVolumeHandoff::Entry* FarVolumeHandoff::Find(const World::FarTileKey& key,
                                                      FarVolumeAdmissionWork* work) const {
    if (key.tier == 0 || key.tier > tiers_.size())
        return nullptr;
    const auto& tier = tiers_[key.tier - 1];
    if (!tier)
        return nullptr;
    const auto end = tier->key_index.begin() + static_cast<std::ptrdiff_t>(tier->index_count);
    const auto found = std::lower_bound(
        tier->key_index.begin(), end, key, [&](std::size_t index, const World::FarTileKey& sought) {
            if (work)
                ++work->key_comparisons;
            const auto& entry = tier->entries[index];
            return *entry.key < sought;
        });
    if (found == end)
        return nullptr;
    if (work)
        ++work->key_comparisons;
    const auto& entry = tier->entries[*found];
    return entry.key == key ? &entry : nullptr;
}
FarVolumeHandoff::Entry* FarVolumeHandoff::Find(const World::FarTileKey& key,
                                                FarVolumeAdmissionWork* work) {
    return const_cast<Entry*>(std::as_const(*this).Find(key, work));
}
FarVolumeAdmissionWork FarVolumeHandoff::AdmissionWork() const {
    CheckOwner();
    return admission_work_;
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
FarWorldRefusal FarVolumeHandoff::ValidateInput(const FarHandoffManifest& manifest,
                                                const FarHandoffTileInput& input,
                                                bool duplicate,
                                                const Entry* old) const {
    if (!input.observation)
        return FarWorldRefusal::ImmutableAuthorityUnavailable;
    const auto& wanted = input.wanted;
    const auto& identity = wanted.identity;
    const auto& key = identity.key;
    const auto& image = *input.observation;
    const auto* water = dynamic_cast<const HandoffWater*>(wanted.water.get());
    if ((image.use() != World::FarFrozenUse::Committed &&
         image.use() != World::FarFrozenUse::SessionRender) ||
        image.durable_identity().state != World::FarEditedImageState::Committed ||
        image.durable_identity().generation != manifest.committed_generation ||
        image.numeric() != manifest.numeric || image.content_identity() != identity.content_hash)
        return FarWorldRefusal::ImmutableAuthorityUnavailable;
    if (image.caves() != manifest.caves)
        return FarWorldRefusal::CavePolicyMismatch;
    if (!water || water->observation() != &image ||
        water->content_identity() != identity.content_hash ||
        water->WaterCapability() == FarWorldWaterCapability::Unavailable ||
        water->WaterBound(key) != wanted.water_bound)
        return FarWorldRefusal::WaterUnavailable;
    if (image.memory_pool() != memory_.Pool(key.tier))
        return FarWorldRefusal::AdmissionFailed;
    if (duplicate && (!old || !old->target || !Same(old->target->wanted, wanted)))
        return FarWorldRefusal::InvalidRequests;
    if (old && old->history && old->history->identity == identity && !Same(*old->history, wanted))
        return FarWorldRefusal::InvalidRequests;
    if (old && old->target && old->target->wanted.identity == identity &&
        !Same(old->target->wanted, wanted))
        return FarWorldRefusal::InvalidRequests;
    if (old && old->serving && old->serving->identity == identity && !Same(*old->serving, wanted))
        return FarWorldRefusal::InvalidRequests;
    if (duplicate) {
        if (!old || !old->target)
            return FarWorldRefusal::InvalidRequests;
        if (old->target->observation != input.observation)
            return FarWorldRefusal::InvalidRequests;
        return FarWorldRefusal::None;
    }
    return FarWorldRefusal::None;
}
FarWorldRefusal FarVolumeHandoff::ValidateResolved(const FarHandoffTileInput& input,
                                                   const World::FarVolumeRequest& resolved,
                                                   bool support,
                                                   const Entry* old) const {
    const auto& wanted = input.wanted;
    const auto bound = wanted.water_bound;
    if (!support)
        return FarWorldRefusal::SpanRejected;
    if (!bound || !std::isfinite(bound->min_y) || !std::isfinite(bound->max_y) ||
        bound->min_y > bound->max_y)
        return FarWorldRefusal::WaterUnavailable;
    if (!bound->known_dry && (double(bound->min_y) < double(resolved.span.min_y) ||
                              double(bound->max_y) > double(resolved.span.max_y)))
        return FarWorldRefusal::WaterSpanRefused;
    if (old && old->history &&
        (resolved.span.min_y > old->history_span.span.min_y ||
         resolved.span.max_y < old->history_span.span.max_y ||
         (old->history->intent.extra_span &&
          (!wanted.intent.extra_span ||
           wanted.intent.extra_span->min_y > old->history->intent.extra_span->min_y ||
           wanted.intent.extra_span->max_y < old->history->intent.extra_span->max_y))))
        return FarWorldRefusal::SpanRejected;
    return FarWorldRefusal::None;
}
bool FarVolumeHandoff::StageSessionTarget(
    const World::FarWorldAuthorityProducer& producer,
    const Systems::SHIELD_WorldSystem& world,
    std::uint64_t world_epoch,
    std::span<const FarSessionHandoffTile> requests,
    const World::FarWorldAuthorityProducer::FrozenPolicy& policy,
    const World::FarFrozenCaptureLimits& limits,
    const World::FarFrozenCaptureWork::Counts& transaction_samples,
    const std::shared_ptr<World::FarVolumeTierMemory>& memory) {
    CheckOwner();
    // One fixed manifest, never a partially installed prefix. Water bridges and
    // frozen allocations are released automatically if a later tile refuses.
    std::array<FarHandoffTileInput, World::FarWorldAuthorityProducer::kSlots> inputs{};
    FarHandoffManifest manifest;
    const auto* source = producer.capture();
    World::FarFrozenCaptureWork work(transaction_samples,
                                     limits.work,
                                     limits.deadline,
                                     source ? source->deadline : World::FarFrozenCaptureDeadline{});
    {
        Guard const guard(*this);
        ReleaseAdmission();
        admission_status_ = FarHandoffAdmissionStatus::Idle;
        admission_work_ = {};
        admission_refusal_ = FarWorldRefusal::None;
        session_capture_failure_ = {};
        const auto refuse = [this](FarWorldRefusal reason) {
            admission_refusal_ = reason;
            admission_status_ = FarHandoffAdmissionStatus::Refused;
            return false;
        };
        if (policy.caves != World::FarCaveMode::BandLimited &&
            policy.caves != World::FarCaveMode::BoxFiltered) {
            return refuse(FarWorldRefusal::InvalidCavePolicy);
        }
        if (limits.max_regions == 0 || limits.max_height_samples == 0 ||
            limits.max_density_samples == 0 || limits.max_water_samples == 0 ||
            limits.max_bytes == 0 || transaction_samples.height == 0 ||
            transaction_samples.density == 0 || transaction_samples.water == 0) {
            return refuse(FarWorldRefusal::MissingLimits);
        }
        if (!world_epoch || requests.empty() || requests.size() > inputs.size() ||
            (policy.numeric != World::FarVolumeNumericProfile::RawFloat &&
             policy.numeric != World::FarVolumeNumericProfile::LegacySigned16)) {
            return refuse(FarWorldRefusal::InvalidRequests);
        }
        if (!memory || memory.get() != &memory_) {
            return refuse(FarWorldRefusal::AdmissionFailed);
        }
        const auto source_refusal = SourceRefusal(producer.capture_status().error);
        if (source_refusal != FarWorldRefusal::None) {
            session_capture_failure_.source = producer.capture_status().error;
            return refuse(source_refusal);
        }
        if (!source) {
            return refuse(FarWorldRefusal::ImmutableAuthorityUnavailable);
        }
        manifest = {world_epoch,
                    source->serial,
                    source->committed.generation,
                    policy.caves,
                    policy.numeric,
                    {inputs.data(), requests.size()}};
        auto transaction_limits = limits;
        transaction_limits.work = &work;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            const auto& item = requests[i];
            const auto& key = item.wanted.identity.key;
            if (key.tier != item.capture.key.tier || key.tx != item.capture.key.x ||
                key.tz != item.capture.key.z || !memory->Pool(key.tier)) {
                return refuse(FarWorldRefusal::InvalidRequests);
            }
            const auto pool =
                std::shared_ptr<World::FarVolumeMemoryPool>(memory, memory->Pool(key.tier));
            std::shared_ptr<const World::FarFrozenObservation> image;
            const auto result = producer.compose_frozen(
                world, item.capture, policy, transaction_limits, pool, image);
            if (!result.ok()) {
                session_capture_failure_ = result;
                if (result.source != World::FarWorldAuthorityProducer::Error::None) {
                    return refuse(SourceRefusal(result.source));
                }
                return refuse(FarWorldProducerRefusal(result));
            }
            auto wanted = item.wanted;
            wanted.identity.world_epoch = world_epoch;
            wanted.identity.bake_generation = source->committed.generation;
            wanted.identity.content_hash = image->content_identity();
            wanted.identity.cave_policy = static_cast<std::uint8_t>(policy.caves);
            if (state_.world_epoch == world_epoch && state_.target_serial == source->serial) {
                const auto* old = Find(key);
                if (old && old->target &&
                    old->target->observation->content_identity() == image->content_identity()) {
                    image = old->target->observation;
                }
            }
            auto input = MakeFarHandoffTileInput(std::move(wanted), std::move(image));
            if (!input) {
                return refuse(FarWorldRefusal::AdmissionFailed);
            }
            inputs[i] = std::move(*input);
        }
    }
    if (StartTarget(manifest, &work) != FarHandoffAdmissionStatus::Pending) {
        return false;
    }
    while (PumpAdmission(std::numeric_limits<std::uint64_t>::max()).status ==
           FarHandoffAdmissionStatus::Pending) {}
    return AdmissionStatus() == FarHandoffAdmissionStatus::Complete;
}

void FarVolumeHandoff::ReleaseAdmission() {
    if (admission_ && admission_->producer && admission_->phase == Admission::Phase::CapturePump) {
        admission_->producer->cancel_frozen();
    }
    admission_.reset();
}
FarHandoffAdmissionStatus FarVolumeHandoff::CancelAdmission() {
    Guard const guard(*this);
    ReleaseAdmission();
    return admission_status_ = FarHandoffAdmissionStatus::Idle;
}
FarHandoffAdmissionStatus FarVolumeHandoff::AdmissionStatus() const {
    CheckOwner();
    return admission_status_;
}
FarHandoffAdmissionStatus FarVolumeHandoff::StartTarget(const FarHandoffManifest& manifest) {
    return StartTarget(manifest, nullptr);
}
FarHandoffAdmissionStatus FarVolumeHandoff::StartTarget(const FarHandoffManifest& manifest,
                                                        World::FarFrozenCaptureWork* sampling,
                                                        bool capture) {
    Guard const guard(*this);
    ReleaseAdmission();
    admission_work_ = {};
    admission_refusal_ = FarWorldRefusal::None;
    session_capture_failure_ = {};
    admission_status_ = FarHandoffAdmissionStatus::Refused;
    if (!capture) {
        if (!ValidateManifest(manifest))
            return admission_status_;
    }
    std::size_t capacity = 0;
    for (const auto& tier : tiers_) {
        if (!tier || tier->entries.size() > std::numeric_limits<std::size_t>::max() - capacity)
            throw std::logic_error("Far-volume handoff invalid descriptor capacity");
        capacity += tier->entries.size();
    }
    if (manifest.tiles.size() > capacity ||
        manifest.tiles.size() >
            std::numeric_limits<std::size_t>::max() / sizeof(FarHandoffTileInput) ||
        manifest.tiles.size() >
            std::numeric_limits<std::size_t>::max() / sizeof(World::FarVolumeRequest) ||
        manifest.tiles.size() > std::numeric_limits<std::size_t>::max() / sizeof(std::size_t)) {
        admission_refusal_ = FarWorldRefusal::AdmissionFailed;
        return admission_status_;
    }
    try {
        World::FarVolumeMemoryLease control(*memory_.Pool(World::kFarTierCount));
        if (!control.TryCharge(sizeof(Admission)))
            throw World::FarVolumeMemoryRefused();
        auto pending = std::make_unique<Admission>(std::move(control));
        pending->inputs.resize(manifest.tiles.size());
        pending->resolved.resize(manifest.tiles.size());
        pending->scratch.resize(manifest.tiles.size());
        pending->before = state_;
        pending->count = manifest.tiles.size();
        std::copy(manifest.tiles.begin(), manifest.tiles.end(), pending->inputs.begin());
        pending->manifest = manifest;
        pending->manifest.tiles = {pending->inputs.data(), pending->count};
        pending->duplicate = manifest.publication_serial == state_.target_serial;
        pending->sampling = sampling;
        admission_ = std::move(pending);
        admission_status_ = FarHandoffAdmissionStatus::Pending;
    } catch (...) {
        admission_refusal_ = FarWorldRefusal::AdmissionFailed;
    }
    return admission_status_;
}
FarHandoffAdmissionStatus FarVolumeHandoff::StartSessionTarget(
    const std::shared_ptr<World::FarWorldAuthorityProducer>& producer,
    const std::shared_ptr<const Systems::SHIELD_WorldSystem>& world,
    std::uint64_t epoch,
    std::span<const FarSessionHandoffTile> requests,
    const World::FarWorldAuthorityProducer::FrozenPolicy& policy,
    const World::FarFrozenCaptureLimits& limits,
    const World::FarFrozenCaptureWork::Counts& samples,
    const std::shared_ptr<World::FarVolumeTierMemory>& memory) {
    // Allocate the same private transaction used by direct manifest admission.
    if (StartTarget({epoch, 0, 0, policy.caves, policy.numeric, {}}, nullptr, true) !=
        FarHandoffAdmissionStatus::Pending)
        return admission_status_;
    Guard const guard(*this);
    const auto refuse = [this](FarWorldRefusal reason) {
        admission_refusal_ = reason;
        ReleaseAdmission();
        return admission_status_ = FarHandoffAdmissionStatus::Refused;
    };
    if (!producer || !world || !memory || memory.get() != &memory_)
        return refuse(FarWorldRefusal::ImmutableAuthorityUnavailable);
    if (policy.caves != World::FarCaveMode::BandLimited &&
        policy.caves != World::FarCaveMode::BoxFiltered)
        return refuse(FarWorldRefusal::InvalidCavePolicy);
    if (!epoch || requests.empty() || requests.size() > admission_->requests.size() ||
        (policy.numeric != World::FarVolumeNumericProfile::RawFloat &&
         policy.numeric != World::FarVolumeNumericProfile::LegacySigned16))
        return refuse(FarWorldRefusal::InvalidRequests);
    if (!limits.max_regions || !limits.max_height_samples || !limits.max_density_samples ||
        !limits.max_water_samples || !limits.max_bytes || !samples.height || !samples.density ||
        !samples.water)
        return refuse(FarWorldRefusal::MissingLimits);
    const auto status = producer->capture_status();
    const auto* source = producer->capture();
    if (status.error != World::FarWorldAuthorityProducer::Error::None) {
        session_capture_failure_.source = status.error;
        return refuse(SourceRefusal(status.error));
    }
    if (producer->frozen_active())
        return refuse(FarWorldRefusal::AdmissionFailed);
    if (!source || status.paused ||
        (!status.latest_ready && status.serving_serial <= state_.target_serial))
        return refuse(FarWorldRefusal::AdmissionFailed);
    auto& pending = *admission_;
    pending.producer = producer;
    pending.world = world;
    pending.memory = memory;
    pending.policy = policy;
    pending.limits = limits;
    pending.work.emplace(samples, limits.work, limits.deadline, source->deadline);
    pending.sampling = &*pending.work;
    pending.limits.work = pending.sampling;
    try {
        pending.inputs.resize(requests.size());
        pending.resolved.resize(requests.size());
        pending.scratch.resize(requests.size());
    } catch (...) {
        return refuse(FarWorldRefusal::AdmissionFailed);
    }
    pending.count = requests.size();
    std::copy(requests.begin(), requests.end(), pending.requests.begin());
    pending.manifest.publication_serial = source->serial;
    pending.manifest.committed_generation = source->committed.generation;
    pending.manifest.tiles = {pending.inputs.data(), pending.count};
    pending.duplicate = source->serial == state_.target_serial;
    {
        const auto scope = world->acquire_worldgen_sample_scope();
        pending.world_guard.Start(*world, {});
    }
    pending.phase = Admission::Phase::CaptureStart;
    return admission_status_;
}
FarWorldRefusal FarVolumeHandoff::PrepareStep(Admission& pending) {
    const auto& manifest = pending.manifest;
    if (pending.phase == Admission::Phase::Index) {
        if (pending.cursor == pending.count) {
            if (pending.fallbacks == 0)
                return FarWorldRefusal::InvalidRequests;
            pending.cursor = 0;
            pending.phase = Admission::Phase::Sort;
            return FarWorldRefusal::None;
        }
        const auto index = pending.cursor++;
        const auto& input = pending.inputs[index];
        const auto& wanted = input.wanted;
        World::FarTileKey key;
        if (!World::ToFarTileKey(wanted.intent.key, key) || key != wanted.identity.key ||
            wanted.identity.world_epoch != manifest.world_epoch ||
            wanted.identity.cave_policy != static_cast<std::uint8_t>(manifest.caves))
            return FarWorldRefusal::InvalidRequests;
        if (!input.observation)
            return FarWorldRefusal::ImmutableAuthorityUnavailable;
        auto& tier = GetTier(key.tier - 1);
        auto& count = pending.counts[key.tier - 1];
        if (count == tier.entries.size())
            return FarWorldRefusal::AdmissionFailed;
        auto incoming = std::span(tier.key_index).subspan(tier.entries.size());
        if (count != 0) {
            ++admission_work_.key_comparisons;
            const bool in_order = manifest.tiles[incoming[count - 1]].wanted.identity.key < key;
            pending.sorted[key.tier - 1] = pending.sorted[key.tier - 1] && in_order;
        }
        incoming[count++] = index;
        pending.fallbacks += key.tier == World::kFarTierCount;
        return FarWorldRefusal::None;
    }
    if (pending.tier == tiers_.size()) {
        pending.tier = 0;
        pending.cursor = 0;
        if (pending.phase == Admission::Phase::Descriptors) {
            if (pending.duplicate && pending.old_count != pending.count)
                return FarWorldRefusal::InvalidRequests;
            pending.phase = Admission::Phase::Header;
        } else {
            pending.phase = Admission::Phase::Descriptors;
        }
        return FarWorldRefusal::None;
    }
    auto& tier = GetTier(pending.tier);
    const auto count = pending.counts[pending.tier];
    auto incoming = std::span(tier.key_index).subspan(tier.entries.size(), count);
    if (pending.phase == Admission::Phase::Descriptors) {
        if (pending.cursor == tier.entries.size()) {
            ++pending.tier;
            pending.cursor = 0;
        } else {
            const auto index = pending.cursor++;
            ++admission_work_.descriptor_visits;
            pending.old_count += tier.entries[index].target.has_value();
        }
    } else if (pending.phase == Admission::Phase::Sort) {
        if (pending.sorted[pending.tier] || pending.width >= count) {
            pending.phase = Admission::Phase::Keys;
            pending.cursor = 1;
        } else if (pending.output == count) {
            pending.phase = Admission::Phase::SortCopy;
            pending.cursor = 0;
        } else {
            // Bottom-up merge: one emitted index (at most one comparison) per
            // budget unit, using the charged manifest-sized scratch buffer.
            if (pending.output == pending.right) {
                pending.left = pending.output;
                pending.middle = pending.left + std::min(pending.width, count - pending.left);
                pending.right = pending.middle + std::min(pending.width, count - pending.middle);
                pending.cursor = pending.middle;
            }
            bool take_left = pending.left < pending.middle;
            if (take_left && pending.cursor < pending.right) {
                ++admission_work_.key_comparisons;
                take_left = manifest.tiles[incoming[pending.left]].wanted.identity.key <
                            manifest.tiles[incoming[pending.cursor]].wanted.identity.key;
            }
            pending.scratch[pending.output++] =
                incoming[take_left ? pending.left++ : pending.cursor++];
        }
    } else if (pending.phase == Admission::Phase::SortCopy) {
        if (pending.cursor == count) {
            // Avoid overflow even for the largest admitted descriptor capacity.
            pending.width = pending.width >= count - pending.width ? count : 2 * pending.width;
            pending.left = pending.middle = pending.right = pending.output = 0;
            pending.phase = Admission::Phase::Sort;
        } else {
            incoming[pending.cursor] = pending.scratch[pending.cursor];
            ++pending.cursor;
        }
    } else if (pending.phase == Admission::Phase::Keys) {
        if (pending.cursor >= count) {
            ++pending.tier;
            pending.width = 1;
            pending.left = pending.middle = pending.right = pending.output = 0;
            pending.phase = Admission::Phase::Sort;
        } else {
            const auto index = pending.cursor++;
            ++admission_work_.key_comparisons;
            if (manifest.tiles[incoming[index - 1]].wanted.identity.key ==
                manifest.tiles[incoming[index]].wanted.identity.key)
                return FarWorldRefusal::InvalidRequests;
        }
    }
    return FarWorldRefusal::None;
}
FarWorldRefusal FarVolumeHandoff::AdmissionStep(Admission& pending) {
    const auto& input = pending.inputs[pending.tile];
    const auto& wanted = input.wanted;
    const auto& key = wanted.identity.key;
    auto& resolved = pending.resolved[pending.tile];
    switch (pending.phase) {
        case Admission::Phase::CaptureStart: {
            if (pending.producer->frozen_active())
                return FarWorldRefusal::AdmissionFailed;
            const auto& item = pending.requests[pending.tile];
            const auto& item_key = item.wanted.identity.key;
            if (item_key.tier != item.capture.key.tier || item_key.tx != item.capture.key.x ||
                item_key.tz != item.capture.key.z || !pending.memory->Pool(item_key.tier))
                return FarWorldRefusal::InvalidRequests;
            const auto pool = std::shared_ptr<World::FarVolumeMemoryPool>(
                pending.memory, pending.memory->Pool(item_key.tier));
            const auto result = pending.producer->start_frozen(
                pending.world, item.capture, pending.policy, pending.limits, pool);
            if (!result.ok()) {
                session_capture_failure_ = result;
                return FarWorldProducerRefusal(result);
            }
            pending.phase = Admission::Phase::CapturePump;
            return FarWorldRefusal::None;
        }
        case Admission::Phase::CapturePump:
            throw std::logic_error("Capture pump requires its remaining budget");
        case Admission::Phase::Index:
        case Admission::Phase::Sort:
        case Admission::Phase::SortCopy:
        case Admission::Phase::Keys:
        case Admission::Phase::Descriptors:
            return PrepareStep(pending);
        case Admission::Phase::Header: {
            World::FarTileKey converted;
            if (!World::ToFarTileKey(wanted.intent.key, converted) || converted != key ||
                wanted.identity.world_epoch != pending.manifest.world_epoch ||
                wanted.identity.cave_policy != static_cast<std::uint8_t>(pending.manifest.caves))
                return FarWorldRefusal::InvalidRequests;
            const auto refusal = ValidateInput(
                pending.manifest, input, pending.duplicate, Find(key, &admission_work_));
            if (refusal != FarWorldRefusal::None)
                return refusal;
            if (pending.duplicate) {
                ++pending.tile;
                return FarWorldRefusal::None;
            }
            const auto error =
                pending.coverage.Start(wanted.intent, input.observation->content_identity());
            if (error != World::FarVolumeFacadeError::None)
                return FarWorldRefusal::SpanRejected;
            if (GetTier(key.tier - 1).authority_limits.max_height_samples < 129 * 129)
                return FarWorldRefusal::AdmissionFailed;
            pending.phase = Admission::Phase::Columns;
            return FarWorldRefusal::None;
        }
        case Admission::Phase::Columns: {
            const auto& image = *input.observation;
            if (pending.height_region == image.region_count())
                return FarWorldRefusal::ImmutableAuthorityUnavailable;
            const auto region = image.Region(pending.height_region);
            if (!region)
                return FarWorldRefusal::InvalidRequests;
            const auto [x, z] = pending.coverage.Column();
            const auto dimensions = World::FarTierAt(key.tier);
            if (!dimensions)
                return FarWorldRefusal::InvalidRequests;
            const auto step = std::int64_t(dimensions->sample_spacing_meters);
            const auto dx = x - (region->support.min.x - step);
            const auto dz = z - (region->support.min.z - step);
            if (region->tier != key.tier || dx < 0 || dz < 0 || dx % step != 0 || dz % step != 0 ||
                x > region->support.max.x + step || z > region->support.max.z + step) {
                ++pending.height_region;
                return FarWorldRefusal::None;
            }
            if (pending.sampling && !pending.sampling->ChargeHeight()) {
                session_capture_failure_.capture =
                    pending.sampling->deadline_exceeded()
                        ? World::FarFrozenCaptureError::DeadlineExceeded
                        : World::FarFrozenCaptureError::WorkLimit;
                return FarWorldRefusal::AdmissionFailed;
            }
            ++pending.height_observations;
            const auto error =
                pending.coverage.Observe(image.RegionHeight(pending.height_region,
                                                            static_cast<std::size_t>(dx / step),
                                                            static_cast<std::size_t>(dz / step)));
            pending.height_region = 0;
            if (error != World::FarVolumeFacadeError::None)
                return error == World::FarVolumeFacadeError::CoordinateRange
                           ? FarWorldRefusal::SpanRejected
                           : FarWorldRefusal::ImmutableAuthorityUnavailable;
            if (pending.coverage.Complete()) {
                if (pending.coverage.Finish(resolved) != World::FarVolumeFacadeError::None)
                    return FarWorldRefusal::SpanRejected;
                pending.phase = Admission::Phase::Regions;
                pending.region = 0;
                pending.support = false;
            }
            return FarWorldRefusal::None;
        }
        case Admission::Phase::Regions: {
            const auto& image = *input.observation;
            if (pending.region == image.region_count()) {
                pending.phase = Admission::Phase::Tail;
                return FarWorldRefusal::None;
            }
            const auto region = image.Region(pending.region++);
            if (!region)
                return FarWorldRefusal::InvalidRequests;
            const auto edge = World::FarTileEdgeMeters(key.tier);
            if (region->tier == key.tier && region->support.min.x == key.tx * edge &&
                region->support.min.z == key.tz * edge &&
                region->support.max.x == (key.tx + 1) * edge &&
                region->support.max.z == (key.tz + 1) * edge &&
                region->support.min.y <= resolved.span.min_y &&
                region->support.max.y >= resolved.span.max_y)
                pending.support = true;
            return FarWorldRefusal::None;
        }
        case Admission::Phase::Tail: {
            const auto refusal =
                ValidateResolved(input, resolved, pending.support, Find(key, &admission_work_));
            ++pending.tile;
            pending.phase = Admission::Phase::Header;
            return refusal;
        }
    }
    throw std::logic_error("Unknown admission phase");
}
FarHandoffAdmissionPump FarVolumeHandoff::PumpAdmission(std::uint64_t budget) {
    Guard const guard(*this);
    FarHandoffAdmissionPump result{admission_status_, 0, 0};
    if (!admission_)
        return result;
    const auto refuse = [this, &result](FarWorldRefusal reason) {
        admission_refusal_ = reason;
        ReleaseAdmission();
        result.status = admission_status_ = FarHandoffAdmissionStatus::Refused;
        return result;
    };
    auto& pending = *admission_;
    const auto check_current = [&]() {
        const auto failure = pending.Failure(state_);
        if (!failure.ok()) {
            if (pending.producer)
                session_capture_failure_ = failure;
            return FarWorldProducerRefusal(failure);
        }
        return FarWorldRefusal::None;
    };
    if (const auto refusal = check_current(); refusal != FarWorldRefusal::None)
        return refuse(refusal);
    while (result.work < budget && pending.tile < pending.count) {
        if (const auto refusal = check_current(); refusal != FarWorldRefusal::None)
            return refuse(refusal);
        if (pending.sampling && !pending.sampling->CheckDeadline()) {
            session_capture_failure_.capture = World::FarFrozenCaptureError::DeadlineExceeded;
            return refuse(FarWorldRefusal::AdmissionFailed);
        }
        if (pending.phase == Admission::Phase::CapturePump) {
            std::shared_ptr<const World::FarFrozenObservation> image;
            const auto capture = pending.producer->pump_frozen(budget - result.work, image);
            result.work += capture.work;
            if (!capture.result.ok()) {
                session_capture_failure_ = capture.result;
                return refuse(FarWorldProducerRefusal(capture.result));
            }
            if (!capture.complete)
                break;
            auto wanted = pending.requests[pending.tile].wanted;
            wanted.identity.world_epoch = pending.manifest.world_epoch;
            wanted.identity.bake_generation = pending.manifest.committed_generation;
            wanted.identity.content_hash = image->content_identity();
            wanted.identity.cave_policy = static_cast<std::uint8_t>(pending.policy.caves);
            if (pending.duplicate) {
                const auto* old = Find(wanted.identity.key);
                if (old && old->target &&
                    old->target->observation->content_identity() == image->content_identity())
                    image = old->target->observation;
            }
            auto input = MakeFarHandoffTileInput(std::move(wanted), std::move(image));
            if (!input)
                return refuse(FarWorldRefusal::AdmissionFailed);
            pending.inputs[pending.tile++] = std::move(*input);
            pending.phase = Admission::Phase::CaptureStart;
            if (pending.tile == pending.count) {
                pending.tile = 0;
                pending.phase = Admission::Phase::Index;
                if (!ValidateManifest(pending.manifest))
                    return refuse(admission_refusal_);
            }
        } else {
            ++result.work;
            const auto before = pending.height_observations;
            const auto refusal = AdmissionStep(pending);
            result.height_observations += pending.height_observations - before;
            if (refusal != FarWorldRefusal::None)
                return refuse(refusal);
        }
    }
    if (pending.tile == pending.count && budget != 0) {
        if (const auto refusal = check_current(); refusal != FarWorldRefusal::None)
            return refuse(refusal);
        if (pending.sampling && !pending.sampling->CheckDeadline()) {
            session_capture_failure_.capture = World::FarFrozenCaptureError::DeadlineExceeded;
            return refuse(FarWorldRefusal::AdmissionFailed);
        }
        if (!CommitTarget(pending))
            return refuse(admission_refusal_);
        ReleaseAdmission();
        result.status = admission_status_ = FarHandoffAdmissionStatus::Complete;
    }
    return result;
}

bool FarVolumeHandoff::StageTarget(const FarHandoffManifest& manifest) {
    if (StartTarget(manifest) != FarHandoffAdmissionStatus::Pending) {
        return false;
    }
    while (PumpAdmission(std::numeric_limits<std::uint64_t>::max()).status ==
           FarHandoffAdmissionStatus::Pending) {}
    return AdmissionStatus() == FarHandoffAdmissionStatus::Complete;
}

bool FarVolumeHandoff::ValidateManifest(const FarHandoffManifest& manifest) {
    admission_refusal_ = FarWorldRefusal::None;
    const auto refuse = [this](FarWorldRefusal reason) {
        admission_refusal_ = reason;
        return false;
    };
    if (manifest.caves != World::FarCaveMode::BandLimited &&
        manifest.caves != World::FarCaveMode::BoxFiltered)
        return refuse(FarWorldRefusal::InvalidCavePolicy);
    if (manifest.world_epoch == 0 || manifest.publication_serial == 0 ||
        (manifest.numeric != World::FarVolumeNumericProfile::RawFloat &&
         manifest.numeric != World::FarVolumeNumericProfile::LegacySigned16) ||
        (state_.world_epoch &&
         (manifest.world_epoch != state_.world_epoch || manifest.numeric != numeric_)) ||
        manifest.publication_serial < state_.target_serial)
        return refuse(FarWorldRefusal::InvalidRequests);
    if (manifest.committed_generation < state_.committed_generation)
        return refuse(FarWorldRefusal::ImmutableAuthorityUnavailable);
    if (state_.world_epoch && manifest.caves != caves_)
        return refuse(FarWorldRefusal::CavePolicyMismatch);
    const bool duplicate = manifest.publication_serial == state_.target_serial;
    if (duplicate && manifest.committed_generation != state_.committed_generation)
        return refuse(FarWorldRefusal::ImmutableAuthorityUnavailable);
    if (manifest.tiles.empty())
        return refuse(FarWorldRefusal::InvalidRequests);
    return true;
}
bool FarVolumeHandoff::CommitTarget(const Admission& pending) {
    const auto& manifest = pending.manifest;
    const auto& counts = pending.counts;
    const auto resolved = std::span(pending.resolved);
    std::array<std::size_t, World::kFarTierCount> new_keys{}, next_free{};
    const bool duplicate = pending.duplicate;
    auto& work = admission_work_;
    const auto refuse = [this](FarWorldRefusal reason) {
        admission_refusal_ = reason;
        return false;
    };
    // AdvanceFrame may reuse descriptor ordering between any two pumps. Reset
    // its membership marks only in this bounded, non-yielding publication step.
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        tier->order.assign(tier->entries.size(), 0);
    }
    std::array<std::size_t, World::kFarTierCount> validated{};
    for (std::size_t index = 0; index < manifest.tiles.size(); ++index) {
        const auto& input = manifest.tiles[index];
        const auto& key = input.wanted.identity.key;
        auto& tier = GetTier(key.tier - 1);
        const auto* existing = Find(key, &work);
        if (existing)
            tier.order[static_cast<std::size_t>(existing - tier.entries.data())] = 1;
        else
            ++new_keys[key.tier - 1];
        tier.validation[validated[key.tier - 1]++] = resolved[index];
    }
    for (std::size_t i = 0; i < tiers_.size(); ++i) {
        const auto& tier = GetTier(i);
        std::size_t free = 0;
        for (std::size_t index = 0; index < tier.entries.size(); ++index) {
            ++work.descriptor_visits;
            const auto& entry = tier.entries[index];
            // Omitted target-only entries retire at commit. Serving entries and
            // every incoming key retain their descriptors and span history.
            free += !entry.key || (!entry.serving && tier.order[index] == 0);
        }
        if (new_keys[i] > free)
            return refuse(FarWorldRefusal::AdmissionFailed);
    }
    if (duplicate)
        return true;
    // Validate first, then retire queue work in one descriptor pass per tier.
    // Binary searches over sorted input positions avoid per-key queue scans.
    for (std::size_t i = 0; i < tiers_.size(); ++i) {
        auto& tier = GetTier(i);
        const auto incoming = std::span(tier.key_index).subspan(tier.entries.size(), counts[i]);
        work.descriptor_visits +=
            tier.queue.ForgetMatching([&](const World::FarTileKey& key) noexcept {
                const auto found =
                    std::lower_bound(incoming.begin(),
                                     incoming.end(),
                                     key,
                                     [&](std::size_t index, const World::FarTileKey& sought) {
                                         ++work.key_comparisons;
                                         return manifest.tiles[index].wanted.identity.key < sought;
                                     });
                if (found == incoming.end())
                    return true;
                ++work.key_comparisons;
                const auto& input = manifest.tiles[*found];
                if (input.wanted.identity.key != key)
                    return true;
                const auto* entry = Find(key, &work);
                return !entry || !entry->target || !Same(entry->target->wanted, input.wanted) ||
                       entry->eligibility == FarVolumeEligibility::RequestRejected;
            });
        for (std::size_t index = 0; index < tier.entries.size(); ++index) {
            ++work.descriptor_visits;
            auto& entry = tier.entries[index];
            if (entry.target && tier.order[index] == 0) {
                entry.completion.reset();
                entry.staged_pair = {};
                entry.target.reset();
            }
        }
        std::size_t free = 0;
        for (const auto& entry : tier.entries) {
            ++work.descriptor_visits;
            free += !entry.key;
        }
        bool reclaimed = false;
        // Reclaim only enough idle history, in stable slot order.
        for (std::size_t index = 0; free < new_keys[i] && index < tier.entries.size(); ++index) {
            ++work.descriptor_visits;
            auto& entry = tier.entries[index];
            if (!entry.key || entry.serving || entry.target || tier.order[index] != 0)
                continue;
            entry = Entry{};
            ++free;
            reclaimed = true;
        }
        if (reclaimed)
            tier.RebuildIndex(work);
    }
    validated = {};
    for (const auto& input : manifest.tiles) {
        const auto& wanted = input.wanted;
        const auto number = std::size_t(wanted.identity.key.tier - 1);
        auto& tier = GetTier(number);
        auto* entry = Find(wanted.identity.key, &work);
        if (!entry) {
            // Each slot is examined at most once across all incoming new keys.
            auto& cursor = next_free[number];
            while (cursor < tier.entries.size()) {
                ++work.descriptor_visits;
                if (!tier.entries[cursor].key)
                    break;
                ++cursor;
            }
            if (cursor == tier.entries.size())
                throw std::logic_error("Far-volume handoff missing admitted entry");
            entry = &tier.entries[cursor++];
        }
        const bool same = entry->target && Same(entry->target->wanted, wanted);
        if (!same) {
            entry->completion.reset();
            entry->staged_pair = {};
        }
        if (!same || entry->eligibility == FarVolumeEligibility::RequestRejected) {
            entry->eligibility = FarVolumeEligibility::Wanted;
            entry->build_failure.reset();
            entry->upload_failure.reset();
        }
        entry->key = wanted.identity.key;
        entry->target = input;
        entry->resolved = tier.validation[validated[number]++];
        auto& history = entry->history.emplace(wanted);
        history.water.reset();
        entry->history_span = entry->resolved;
        if (Covers(*entry, FarHandoffCoverageView::Target))
            entry->eligibility = FarVolumeEligibility::Settled;
    }
    for (std::size_t i = 0; i < tiers_.size(); ++i)
        if (new_keys[i] != 0)
            GetTier(i).RebuildIndex(work);
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
std::size_t FarVolumeHandoff::RetireIdleHistory() {
    Guard const guard(*this);
    if (admission_)
        return 0;
    std::size_t retired = 0;
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume handoff missing tier");
        std::size_t retained = 0;
        // Compact the sorted index in place, bounded by this tier's entry_slots.
        // Omitted work was already forgotten by StageTarget; no worker owns Entry.
        for (std::size_t position = 0; position < tier->index_count; ++position) {
            const auto index = tier->key_index[position];
            tier->key_index[position] = kEmptyIndex;
            auto& entry = tier->entries[index];
            if (entry.serving || entry.target) {
                tier->key_index[retained++] = index;
            } else {
                entry = Entry{};
                ++retired;
            }
        }
        tier->index_count = retained;
    }
    return retired;
}
std::optional<World::FarVolumeRequest>
FarVolumeHandoff::TargetCoverageRequest(const World::FarTileKey& key) const {
    CheckOwner();
    const auto* entry = Find(key);
    return entry && entry->target ? std::optional{entry->resolved} : std::nullopt;
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
                if (entry.key && (entry.serving || entry.target || admission_))
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
    ReleaseAdmission();
    admission_status_ = FarHandoffAdmissionStatus::Idle;
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
        tier->index_count = 0;
    }
    state_ = {};
    admission_refusal_ = FarWorldRefusal::None;
    session_capture_failure_ = {};
}
World::FarWorldAuthorityProducer::FrozenResult FarVolumeHandoff::SessionCaptureFailure() const {
    CheckOwner();
    return session_capture_failure_;
}
FarWorldRefusal FarVolumeHandoff::AdmissionRefusal() const {
    CheckOwner();
    return admission_refusal_;
}
} // namespace Luminumbra::Rendering
