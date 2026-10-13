#include "FarVolumeBuildQueue.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
bool Valid(const FarVolumeQueueRequest& request) {
    World::FarTileKey key;
    const auto& authority = request.authority;
    if (!World::ToFarTileKey(request.intent.key, key) || key != request.identity.key ||
        request.identity.content_hash != authority.field_identity ||
        request.identity.cave_policy != static_cast<std::uint8_t>(authority.caves) ||
        (authority.caves != World::FarCaveMode::BandLimited &&
         authority.caves != World::FarCaveMode::BoxFiltered) ||
        (authority.numeric != World::FarVolumeNumericProfile::RawFloat &&
         authority.numeric != World::FarVolumeNumericProfile::LegacySigned16) ||
        !authority.height || !authority.density)
        return false;
    if (request.intent.extra_span) {
        const auto& span = *request.intent.extra_span;
        constexpr auto guard = World::kFarVolumeExactCoordinateLimitMeters - 1024;
        if (!std::isfinite(span.min_y) || !std::isfinite(span.max_y) || span.min_y > span.max_y ||
            span.min_y <= -guard || span.max_y >= guard)
            return false;
    }
    return true;
}
bool Finished(const JobHandle& handle) {
    return !handle.counter || handle.counter->load(std::memory_order_acquire) == 0;
}
void Increment(std::uint64_t& counter) {
    if (counter != std::numeric_limits<std::uint64_t>::max())
        ++counter;
}
bool Successful(FarVolumeBuildStatus status) {
    return status == FarVolumeBuildStatus::Ready || status == FarVolumeBuildStatus::ReadyEmpty;
}
bool Covers(const FarVolumeRenderMesh& mesh, const World::FarVolumeCoverageIntent& intent) {
    if (mesh.request.key != intent.key)
        return false;
    return !intent.extra_span || (mesh.request.span.min_y <= double(intent.extra_span->min_y) &&
                                  mesh.request.span.max_y >= double(intent.extra_span->max_y));
}
} // namespace

FarVolumeQueueCompletion&
FarVolumeQueueCompletion::operator=(FarVolumeQueueCompletion&& other) noexcept {
    if (this != &other) {
        // Free old buffers while their charge remains held, then replace it.
        mesh_ = std::move(other.mesh_);
        retained_ = std::move(other.retained_);
        identity_ = other.identity_;
        status_ = other.status_;
        compilation_failure_ = other.compilation_failure_;
        conversion_failure_ = other.conversion_failure_;
    }
    return *this;
}

FarVolumeBuildQueue::Slot::Slot(Slot&& other) noexcept
    : occupied(other.occupied)
    , ready(other.ready)
    , descriptor(other.descriptor)
    , pool(other.pool)
    , upstream(other.upstream)
    , limits(other.limits)
    , request(std::move(other.request))
    , result(std::move(other.result))
    , cancelled(other.cancelled.load(std::memory_order_relaxed))
    , published(other.published.load(std::memory_order_relaxed))
    , handle(std::move(other.handle)) {}

FarVolumeBuildQueue::FarVolumeBuildQueue(JobSystem& jobs,
                                         World::FarVolumeMemoryPool& pool,
                                         const FarVolumeQueueLimits& limits,
                                         std::pmr::memory_resource* upstream)
    : jobs_(jobs)
    , pool_(pool)
    , limits_(limits)
    , upstream_(upstream)
    , control_(pool)
    , resource_(control_, upstream)
    , descriptors_(&resource_)
    , slots_(&resource_) {
    if (limits.descriptor_slots == 0 || limits.result_slots == 0)
        throw std::invalid_argument("Far-volume queue needs explicit nonzero control counts");
    descriptors_.resize(limits.descriptor_slots);
    // Slots are stable for the whole queue lifetime; no worker sees relocation.
    slots_.reserve(limits.result_slots);
    for (std::size_t i = 0; i < limits.result_slots; ++i)
        slots_.emplace_back();
}
FarVolumeBuildQueue::~FarVolumeBuildQueue() {
    CancelAndDrain();
}
void FarVolumeBuildQueue::CheckOwner() const {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("Far-volume queue called outside its owner thread");
}
std::optional<std::size_t> FarVolumeBuildQueue::Find(const World::FarTileKey& key) const {
    for (std::size_t i = 0; i < descriptors_.size(); ++i) {
        const auto& descriptor = descriptors_[i];
        if (descriptor.desired && descriptor.desired->identity.key == key)
            return i;
    }
    return std::nullopt;
}

FarVolumeSubmitStatus FarVolumeBuildQueue::Submit(const FarVolumeQueueRequest& request) {
    CheckOwner();
    if (!Valid(request))
        return FarVolumeSubmitStatus::Invalid;
    const auto existing = Find(request.identity.key);
    auto index = existing;
    if (existing) {
        const auto& held = descriptors_[*existing].desired;
        if (!held)
            return FarVolumeSubmitStatus::Invalid;
        const auto& previous = *held;
        if (previous.identity == request.identity) {
            if (previous.intent != request.intent ||
                previous.authority.numeric != request.authority.numeric)
                return FarVolumeSubmitStatus::Invalid;
            // Forgotten in-flight work revives only after cancellation drains.
            // Ordinary equal requests still cannot restart/retry completed work.
            if (!descriptors_[*existing].forget)
                return FarVolumeSubmitStatus::Duplicate;
        }
    } else {
        for (std::size_t i = 0; i < descriptors_.size(); ++i)
            if (!descriptors_[i].desired) {
                index = i;
                break;
            }
        if (!index)
            return FarVolumeSubmitStatus::Full;
    }
    if (!index)
        return FarVolumeSubmitStatus::Full;
    // Authority copies can allocate; finish them before replacing desired work.
    std::optional<FarVolumeQueueRequest> staged;
    try {
        staged.emplace(request);
    } catch (const std::bad_alloc&) {
        return FarVolumeSubmitStatus::AllocationFailure;
    } catch (...) {
        return FarVolumeSubmitStatus::Invalid;
    }
    auto& descriptor = descriptors_[*index];
    descriptor.desired.swap(staged);
    descriptor.pending = true;
    descriptor.forget = false;
    if (descriptor.slot) {
        auto& slot = slots_[*descriptor.slot];
        slot.cancelled.store(true, std::memory_order_release);
        if (slot.ready)
            ClearSlot(*descriptor.slot);
    }
    return existing ? FarVolumeSubmitStatus::Coalesced : FarVolumeSubmitStatus::Queued;
}

void FarVolumeBuildQueue::Run(Slot* slot) noexcept {
    if (!slot || !slot->request || !slot->result) {
        if (slot)
            slot->published.store(true, std::memory_order_release);
        return;
    }
    const auto* request = &*slot->request;
    auto& result = *slot->result;
    try {
        if (slot->cancelled.load(std::memory_order_acquire))
            result.status_ = FarVolumeBuildStatus::Cancelled;
        else {
            // Callbacks stay in the stable slot. The dispatched Job captures
            // only slot*; all worker-local cleanup finishes before the counter drops.
            World::FarVolumeAuthority authority;
            authority.field_identity = request->authority.field_identity;
            authority.caves = request->authority.caves;
            authority.numeric = request->authority.numeric;
            authority.height = [slot, request](std::int64_t x,
                                               std::int64_t z) -> std::optional<float> {
                if (slot->cancelled.load(std::memory_order_acquire))
                    return std::nullopt;
                return request->authority.height(x, z);
            };
            authority.density =
                [slot, request](const World::FarVolumePosition& p,
                                float height,
                                std::uint32_t spacing,
                                World::FarCaveMode caves) -> std::optional<World::FarVolumeSample> {
                if (slot->cancelled.load(std::memory_order_acquire))
                    return std::nullopt;
                return request->authority.density(p, height, spacing, caves);
            };
            World::FarVolumeMemoryLease compilation_charge(*slot->pool);
            World::FarVolumeCompilation compilation;
            const auto compiled =
                World::CompileFarVolumeCharged(request->intent,
                                               authority,
                                               slot->limits.compilation,
                                               {slot->pool, &compilation_charge, slot->upstream},
                                               compilation,
                                               &result.compilation_failure_);
            if (slot->cancelled.load(std::memory_order_acquire))
                result.status_ = FarVolumeBuildStatus::Cancelled;
            else if (!compiled)
                result.status_ = FarVolumeBuildStatus::CompileRefused;
            else if (!AdaptFarVolumeRenderMesh(compilation.mesh,
                                               slot->limits.max_converted_bytes,
                                               {slot->pool, &result.retained_, slot->upstream},
                                               result.mesh_,
                                               &result.conversion_failure_))
                result.status_ = FarVolumeBuildStatus::ConvertRefused;
            else if (slot->cancelled.load(std::memory_order_acquire))
                result.status_ = FarVolumeBuildStatus::Cancelled;
            else
                result.status_ = result.mesh_.indices.empty() ? FarVolumeBuildStatus::ReadyEmpty
                                                              : FarVolumeBuildStatus::Ready;
        }
    } catch (...) {
        result.status_ = FarVolumeBuildStatus::Failed;
    }
    // Owner consumption additionally waits for JobSystem's completion counter.
    slot->published.store(true, std::memory_order_release);
}

void FarVolumeBuildQueue::ClearSlot(std::size_t index) {
    auto& slot = slots_[index];
    auto& descriptor = descriptors_[slot.descriptor];
    descriptor.slot.reset();
    slot.result.reset();
    slot.request.reset();
    slot.handle = {};
    slot.occupied = false;
    slot.ready = false;
    if (descriptor.forget) {
        descriptor.desired.reset();
        descriptor.pending = false;
        descriptor.forget = false;
    }
}
void FarVolumeBuildQueue::Reap() {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto& slot = slots_[i];
        if (!slot.occupied || slot.ready || !Finished(slot.handle))
            continue;
        jobs_.wait(slot.handle);
        if (!slot.result) {
            ClearSlot(i);
            continue;
        }
        auto& descriptor = descriptors_[slot.descriptor];
        if (!slot.published.load(std::memory_order_acquire)) {
            slot.result->status_ = FarVolumeBuildStatus::DispatchRejected;
            Increment(rejected_);
        }
        if (slot.cancelled.load(std::memory_order_acquire) || descriptor.forget ||
            !descriptor.desired || descriptor.desired->identity != slot.result->identity_) {
            if (descriptor.forget || !descriptor.desired ||
                descriptor.desired->identity != slot.result->identity_)
                Increment(stale_);
            ClearSlot(i);
        } else {
            // All job-local cleanup completed; retained bindings can now drop.
            slot.request.reset();
            slot.ready = true;
        }
    }
}
std::size_t FarVolumeBuildQueue::Pump(std::size_t max_dispatches,
                                      std::span<const World::FarTileKey> order) {
    CheckOwner();
    Reap();
    std::size_t dispatched = 0;
    const auto candidates = order.empty() ? descriptors_.size() : order.size();
    for (std::size_t rank = 0; rank < candidates && dispatched < max_dispatches; ++rank) {
        const auto selected = order.empty() ? std::optional<std::size_t>(rank) : Find(order[rank]);
        if (!selected)
            continue;
        const auto d = *selected;
        auto& descriptor = descriptors_[d];
        if (!descriptor.desired || !descriptor.pending || descriptor.slot)
            continue;
        std::optional<std::size_t> available;
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (!slots_[i].occupied) {
                available = i;
                break;
            }
        if (!available)
            break;
        auto& slot = slots_[*available];
        slot.descriptor = d;
        slot.pool = &pool_;
        slot.upstream = upstream_;
        slot.limits = limits_;
        slot.cancelled.store(false, std::memory_order_release);
        slot.published.store(false, std::memory_order_release);
        try {
            slot.result.emplace(FarVolumeQueueCompletion(pool_, descriptor.desired->identity));
            slot.request.emplace(*descriptor.desired);
            const std::vector<Job> batch{[pointer = &slot]() {
                Run(pointer);
            }};
            // Exactly one job: the inspected enqueue path cannot throw after
            // publishing that job. Everything after the return is nonthrowing.
            slot.handle = jobs_.dispatch_batch(batch, JobPriority::Normal);
            slot.occupied = true;
            descriptor.slot = *available;
            descriptor.pending = false;
            ++dispatched;
            Increment(dispatched_);
        } catch (...) {
            slot.request.reset();
            if (slot.result) {
                slot.result->status_ = FarVolumeBuildStatus::Failed;
                slot.occupied = true;
                slot.ready = true;
                descriptor.slot = *available;
                descriptor.pending = false;
            }
            // No automatic retry loop after dispatch/callback-copy failure.
            break;
        }
    }
    return dispatched;
}

void FarVolumeBuildQueue::Cancel(const World::FarTileKey& key) {
    CheckOwner();
    const auto index = Find(key);
    if (!index)
        return;
    auto& descriptor = descriptors_[*index];
    descriptor.pending = false;
    if (descriptor.slot) {
        auto& slot = slots_[*descriptor.slot];
        slot.cancelled.store(true, std::memory_order_release);
        if (slot.ready)
            ClearSlot(*descriptor.slot);
    }
}
void FarVolumeBuildQueue::Forget(const World::FarTileKey& key) {
    CheckOwner();
    const auto index = Find(key);
    if (!index)
        return;
    descriptors_[*index].forget = true;
    Cancel(key);
    if (!descriptors_[*index].slot) {
        descriptors_[*index].desired.reset();
        descriptors_[*index].forget = false;
    }
}
bool FarVolumeBuildQueue::Retry(const World::FarTileKey& key) {
    CheckOwner();
    Reap();
    const auto index = Find(key);
    if (!index || descriptors_[*index].slot || descriptors_[*index].pending ||
        descriptors_[*index].forget)
        return false;
    descriptors_[*index].pending = true;
    return true;
}
std::optional<FarVolumeQueueCompletion>
FarVolumeBuildQueue::TakeCompleted(const World::FarBuildIdentity& expected,
                                   const World::FarVolumeCoverageIntent& current_intent) {
    CheckOwner();
    Reap();
    const auto index = Find(expected.key);
    if (!index || !descriptors_[*index].slot)
        return std::nullopt;
    auto& descriptor = descriptors_[*index];
    if (!descriptor.slot)
        return std::nullopt;
    const auto slot_index = *descriptor.slot;
    auto& slot = slots_[slot_index];
    if (!slot.ready || !slot.result || !descriptor.desired)
        return std::nullopt;
    if (descriptor.desired->identity != expected || slot.result->identity_ != expected ||
        descriptor.desired->intent != current_intent ||
        (Successful(slot.result->status_) && !Covers(slot.result->mesh_, current_intent))) {
        Increment(stale_);
        ClearSlot(slot_index);
        return std::nullopt;
    }
    std::optional<FarVolumeQueueCompletion> taken(std::move(*slot.result));
    ClearSlot(slot_index);
    return taken;
}
void FarVolumeBuildQueue::CancelAndDrain() {
    CheckOwner();
    for (auto& descriptor : descriptors_)
        descriptor.pending = false;
    for (auto& slot : slots_)
        if (slot.occupied)
            slot.cancelled.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].occupied) {
            jobs_.wait(slots_[i].handle);
            ClearSlot(i);
        }
    for (auto& descriptor : descriptors_) {
        descriptor.desired.reset();
        descriptor.slot.reset();
        descriptor.forget = false;
    }
}
FarVolumeQueueStats FarVolumeBuildQueue::Stats() const {
    CheckOwner();
    FarVolumeQueueStats stats;
    for (const auto& descriptor : descriptors_)
        if (descriptor.desired) {
            ++stats.descriptors;
            stats.pending += descriptor.pending;
        }
    for (const auto& slot : slots_)
        if (slot.occupied) {
            if (slot.ready && slot.result) {
                ++stats.ready;
                stats.retained_bytes += slot.result->retained_.bytes();
            } else
                ++stats.running;
        }
    stats.control_bytes = control_.bytes();
    stats.dispatched = dispatched_;
    stats.stale = stale_;
    stats.rejected = rejected_;
    return stats;
}

} // namespace Luminumbra::Rendering
