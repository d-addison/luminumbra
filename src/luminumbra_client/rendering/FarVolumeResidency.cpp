#include "FarVolumeResidency.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Luminumbra::Rendering {
namespace {
bool Valid(const FarVolumeWantedRequest& request) {
    World::FarTileKey key;
    if (!World::ToFarTileKey(request.intent.key, key) || key != request.identity.key)
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
std::optional<std::uint64_t> UploadBytes(const FarVolumeRenderMesh& mesh) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (mesh.vertices.size() > max / sizeof(VoxelVertex) ||
        mesh.indices.size() > max / sizeof(std::uint32_t))
        return {};
    const auto vertices = std::uint64_t(mesh.vertices.size()) * sizeof(VoxelVertex);
    const auto indices = std::uint64_t(mesh.indices.size()) * sizeof(std::uint32_t);
    if (indices > max - vertices)
        return {};
    return vertices + indices;
}
} // namespace
class FarVolumeResidency::Guard {
public:
    explicit Guard(FarVolumeResidency& owner)
        : owner_(owner) {
        owner.CheckOwner();
        if (owner.active_)
            throw std::logic_error("Far-volume residency reentrancy");
        owner.active_ = true;
    }
    ~Guard() {
        owner_.active_ = false;
    }

private:
    FarVolumeResidency& owner_;
};
FarVolumeResidency::Tier::Tier(JobSystem& jobs,
                               World::FarVolumeMemoryPool& pool,
                               std::size_t count,
                               const FarVolumeQueueLimits& limits,
                               std::pmr::memory_resource* upstream)
    : control(pool)
    , resource(control, upstream)
    , entries(&resource)
    , order(&resource)
    , queue(jobs, pool, limits, upstream)
    , completion_slots(limits.result_slots) {
    if (count == 0 || count > limits.descriptor_slots)
        throw std::invalid_argument("Far-volume residency needs bounded nonzero entries");
    entries.resize(count);
    order.reserve(count);
}
FarVolumeResidency::FarVolumeResidency(JobSystem& jobs,
                                       World::FarVolumeTierMemory& memory,
                                       const FarVolumeResidencyLimits& limits,
                                       std::pmr::memory_resource* upstream)
    : memory_(memory) {
    for (std::size_t i = 0; i < tiers_.size(); ++i) {
        auto* pool = memory.Pool(i + 1);
        if (!pool)
            throw std::invalid_argument("Far-volume residency tier binding");
        tiers_[i].emplace(jobs, *pool, limits.entry_slots[i], limits.queues[i], upstream);
    }
}
FarVolumeResidency::~FarVolumeResidency() {
    CancelAndDrain();
}
void FarVolumeResidency::CheckOwner() const {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("Far-volume residency outside owner thread");
}
const FarVolumeResidency::Entry* FarVolumeResidency::Find(const World::FarTileKey& key) const {
    if (key.tier == 0 || key.tier > tiers_.size())
        return nullptr;
    const auto& tier = tiers_[key.tier - 1];
    if (!tier)
        return nullptr;
    for (const auto& entry : tier->entries)
        if (entry.desired && entry.desired->identity.key == key)
            return &entry;
    return nullptr;
}
FarVolumeResidency::Entry* FarVolumeResidency::Find(const World::FarTileKey& key) {
    return const_cast<Entry*>(std::as_const(*this).Find(key));
}
bool FarVolumeResidency::ReplaceWanted(std::span<const FarVolumeWantedRequest> requests) {
    Guard guard(*this);
    std::array<std::size_t, World::kFarTierCount> counts{};
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        if (!Valid(request))
            return false;
        const auto tier = std::size_t(request.identity.key.tier - 1);
        if (++counts[tier] > tiers_[tier]->entries.size())
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (requests[j].identity.key == request.identity.key)
                return false;
        const auto* old = Find(request.identity.key);
        if (old && old->desired->identity == request.identity &&
            (old->desired->intent != request.intent ||
             old->desired->build_headroom_bytes != request.build_headroom_bytes))
            return false;
        if (old && old->desired->identity.world_epoch == request.identity.world_epoch &&
            old->desired->intent.extra_span &&
            (!request.intent.extra_span ||
             request.intent.extra_span->min_y > old->desired->intent.extra_span->min_y ||
             request.intent.extra_span->max_y < old->desired->intent.extra_span->max_y))
            return false;
    }
    bool keys_changed = false;
    std::size_t old_count = 0;
    for (const auto& tier : tiers_)
        for (const auto& entry : tier->entries)
            if (entry.desired) {
                ++old_count;
                if (std::none_of(requests.begin(), requests.end(), [&](const auto& request) {
                        return request.identity.key == entry.desired->identity.key;
                    }))
                    keys_changed = true;
            }
    keys_changed = keys_changed || old_count != requests.size();
    // Validation is complete. Entries/scratch never grow after construction.
    for (auto& tier : tiers_)
        for (auto& entry : tier->entries)
            if (entry.desired && std::none_of(requests.begin(), requests.end(), [&](const auto& r) {
                    return r.identity.key == entry.desired->identity.key;
                })) {
                tier->queue.Forget(entry.desired->identity.key);
                entry = Entry{};
            }
    for (const auto& request : requests) {
        auto& tier = *tiers_[request.identity.key.tier - 1];
        auto* entry = Find(request.identity.key);
        if (!entry) {
            auto free = std::find_if(tier.entries.begin(),
                                     tier.entries.end(),
                                     [](const auto& candidate) { return !candidate.desired; });
            entry = &*free; // capacities validated above
        }
        const bool changed = !entry->desired || entry->desired->identity != request.identity;
        if (changed) {
            tier.queue.Forget(request.identity.key);
            entry->completion.reset();
            if (entry->eligibility != FarVolumeEligibility::BudgetEvicted || keys_changed)
                entry->eligibility = FarVolumeEligibility::Wanted;
            entry->desired = request;
        } else if (keys_changed && (entry->eligibility == FarVolumeEligibility::RequestRejected ||
                                    entry->eligibility == FarVolumeEligibility::BudgetEvicted)) {
            tier.queue.Forget(request.identity.key);
            entry->eligibility = FarVolumeEligibility::Wanted;
        }
    }
    return true;
}
void FarVolumeResidency::Reject(Entry& entry, FarVolumeFrameResult& result) {
    auto& tier = *tiers_[entry.desired->identity.key.tier - 1];
    tier.queue.Forget(entry.desired->identity.key);
    entry.completion.reset();
    entry.eligibility = FarVolumeEligibility::RequestRejected;
    ++result.rejected;
    if (entry.desired->identity.key.tier == World::kFarTierCount)
        result.horizon_refused = true;
}
bool FarVolumeResidency::Headroom(std::size_t number,
                                  std::uint64_t bytes,
                                  const World::FarTileKey& preserve,
                                  FarVolumeFrameResult& result) {
    auto* pool = memory_.Pool(number);
    const auto fits = [&] {
        return bytes <= pool->limit() && pool->used() <= pool->limit() - bytes;
    };
    if (fits())
        return true;
    if (number == World::kFarTierCount || bytes > pool->limit())
        return false;
    auto& tier = *tiers_[number - 1];
    // Farthest first, then canonical key. Other tiers cannot free this pool.
    for (std::size_t step = 0; step < tier.entries.size(); ++step) {
        Entry* candidate = nullptr;
        for (auto& entry : tier.entries) {
            if (!entry.desired)
                continue;
            const auto& key = entry.desired->identity.key;
            if (key == preserve || entry.eligibility == FarVolumeEligibility::BudgetEvicted ||
                (!entry.completion && entry.uploaded.gpu_bytes() == 0))
                continue;
            const auto* parent = Find(World::FarTileParent(key));
            if (!parent || !World::IsFarTileCovering(parent->uploaded.Status(
                               parent->desired->identity, parent->desired->intent)))
                continue;
            if (!candidate || entry.distance > candidate->distance ||
                (entry.distance == candidate->distance && key < candidate->desired->identity.key))
                candidate = &entry;
        }
        if (!candidate)
            break;
        auto& entry = *candidate;
        const auto& key = entry.desired->identity.key;
        tier.queue.Forget(key);
        entry.completion.reset();
        entry.uploaded = FarVolumeUploadedTile{};
        entry.eligibility = FarVolumeEligibility::BudgetEvicted;
        ++result.evicted;
        if (fits())
            return true;
    }
    return false;
}
FarVolumeFrameResult FarVolumeResidency::AdvanceFrame(double x,
                                                      double z,
                                                      const FarVolumeFrameLimits& limits,
                                                      const FarVolumeAuthorityProducer& producer,
                                                      const FarVolumeUploadBackend& backend) {
    Guard guard(*this);
    FarVolumeFrameResult result;
    if (!std::isfinite(x))
        x = 0;
    if (!std::isfinite(z))
        z = 0;
    for (auto& tier : tiers_) {
        tier->queue.Pump(0);
        tier->order.clear();
        std::size_t held = 0;
        for (const auto& entry : tier->entries)
            held += entry.completion.has_value();
        for (std::size_t i = 0; i < tier->entries.size(); ++i) {
            auto& entry = tier->entries[i];
            if (!entry.desired)
                continue;
            tier->order.push_back(i);
            entry.distance = World::FarTileNearestDistance(entry.desired->identity.key, x, z);
            if ((entry.eligibility == FarVolumeEligibility::Building ||
                 entry.eligibility == FarVolumeEligibility::Pending) &&
                held < tier->completion_slots) {
                auto completed =
                    tier->queue.TakeCompleted(entry.desired->identity, entry.desired->intent);
                if (completed) {
                    tier->queue.Forget(entry.desired->identity.key);
                    if (completed->status() == FarVolumeBuildStatus::Ready ||
                        completed->status() == FarVolumeBuildStatus::ReadyEmpty) {
                        entry.completion.emplace(std::move(*completed));
                        entry.eligibility = FarVolumeEligibility::Ready;
                        ++held;
                    } else {
                        Reject(entry, result);
                    }
                }
            }
        }
        std::sort(tier->order.begin(), tier->order.end(), [&](std::size_t a, std::size_t b) {
            const auto& ak = tier->entries[a].desired->identity.key;
            const auto& bk = tier->entries[b].desired->identity.key;
            const auto ad = tier->entries[a].distance;
            const auto bd = tier->entries[b].distance;
            return ad != bd ? ad < bd : ak < bk;
        });
    }
    // One global upload budget, V5 first. Frame deferral retains Ready CPU data.
    for (std::size_t n = tiers_.size(); n > 0; --n) {
        auto& tier = *tiers_[n - 1];
        for (const auto i : tier.order) {
            auto& entry = tier.entries[i];
            if (entry.eligibility != FarVolumeEligibility::Ready || !entry.completion)
                continue;
            if (result.upload_attempts == limits.uploads)
                break;
            const auto bytes = UploadBytes(entry.completion->mesh());
            if (!bytes || *bytes > limits.upload_bytes) {
                ++result.upload_attempts;
                Reject(entry, result);
                continue;
            }
            if (*bytes > limits.upload_bytes - result.upload_bytes)
                continue;
            ++result.upload_attempts;
            if (!Headroom(n, *bytes, entry.desired->identity.key, result) ||
                !UploadFarVolume(*entry.completion,
                                 entry.desired->identity,
                                 entry.desired->intent,
                                 memory_,
                                 limits.upload_bytes - result.upload_bytes,
                                 backend,
                                 entry.uploaded)) {
                Reject(entry, result);
                continue;
            }
            entry.completion.reset();
            entry.eligibility = FarVolumeEligibility::Settled;
            ++result.uploaded;
            result.upload_bytes += *bytes;
        }
    }
    // One global dispatch budget; explicit single-key Pump avoids fallback.
    for (std::size_t n = tiers_.size(); n > 0; --n) {
        auto& tier = *tiers_[n - 1];
        for (const auto i : tier.order) {
            auto& entry = tier.entries[i];
            if (entry.eligibility != FarVolumeEligibility::Wanted &&
                entry.eligibility != FarVolumeEligibility::Pending)
                continue;
            if (result.build_attempts == limits.builds)
                break;
            ++result.build_attempts;
            if (entry.eligibility == FarVolumeEligibility::Pending) {
                const std::array key{entry.desired->identity.key};
                const auto dispatched = tier.queue.Pump(1, key);
                result.dispatched += dispatched;
                if (dispatched != 0)
                    entry.eligibility = FarVolumeEligibility::Building;
                continue;
            }
            if (!producer ||
                !Headroom(
                    n, entry.desired->build_headroom_bytes, entry.desired->identity.key, result)) {
                Reject(entry, result);
                continue;
            }
            std::optional<World::FarVolumeAuthority> authority;
            try {
                authority = producer(*entry.desired, *memory_.Pool(n));
            } catch (...) {
                Reject(entry, result);
                continue;
            }
            if (!authority) {
                Reject(entry, result);
                continue;
            }
            FarVolumeSubmitStatus submitted;
            try {
                submitted =
                    tier.queue.Submit({entry.desired->identity, entry.desired->intent, *authority});
            } catch (...) {
                Reject(entry, result);
                continue;
            }
            if (submitted != FarVolumeSubmitStatus::Queued &&
                submitted != FarVolumeSubmitStatus::Coalesced &&
                submitted != FarVolumeSubmitStatus::Duplicate) {
                // Full includes forgotten running descriptors: terminal refusal,
                // not repeated producer allocations while drain catches up.
                Reject(entry, result);
                continue;
            }
            const std::array key{entry.desired->identity.key};
            const auto dispatched = tier.queue.Pump(1, key);
            result.dispatched += dispatched;
            entry.eligibility =
                dispatched == 0 ? FarVolumeEligibility::Pending : FarVolumeEligibility::Building;
        }
    }
    return result;
}
std::optional<FarVolumeResidentObservation>
FarVolumeResidency::Observe(const World::FarTileKey& key) const {
    CheckOwner();
    const auto* entry = Find(key);
    if (!entry)
        return {};
    return FarVolumeResidentObservation{
        entry->desired->identity,
        entry->eligibility,
        entry->uploaded.Status(entry->desired->identity, entry->desired->intent),
        entry->completion ? entry->completion->retained_bytes() : 0,
        entry->uploaded.gpu_bytes(),
        entry->completion ? UploadBytes(entry->completion->mesh())
                                .value_or(std::numeric_limits<std::uint64_t>::max())
                          : 0};
}
const FarVolumeGpuAllocation* FarVolumeResidency::Resource(const World::FarTileKey& key) const {
    CheckOwner();
    const auto* entry = Find(key);
    return entry ? entry->uploaded.resource() : nullptr;
}
std::array<FarVolumeTierObservation, World::kFarTierCount> FarVolumeResidency::Snapshot() const {
    CheckOwner();
    std::array<FarVolumeTierObservation, World::kFarTierCount> result{};
    for (std::size_t i = 0; i < tiers_.size(); ++i) {
        auto& observation = result[i];
        const auto memory = memory_.Stats(i + 1);
        if (memory)
            observation.memory = *memory;
        for (const auto& entry : tiers_[i]->entries) {
            if (!entry.desired)
                continue;
            ++observation.wanted;
            switch (entry.eligibility) {
                case FarVolumeEligibility::Wanted:
                    break;
                case FarVolumeEligibility::Pending:
                    ++observation.pending;
                    break;
                case FarVolumeEligibility::Building:
                    ++observation.building;
                    break;
                case FarVolumeEligibility::Ready:
                    ++observation.ready;
                    break;
                case FarVolumeEligibility::Settled:
                    ++observation.settled;
                    break;
                case FarVolumeEligibility::RequestRejected:
                    ++observation.request_rejected;
                    break;
                case FarVolumeEligibility::BudgetEvicted:
                    ++observation.budget_evicted;
                    break;
            }
            const auto coverage =
                entry.uploaded.Status(entry.desired->identity, entry.desired->intent);
            observation.uploaded_pairs += coverage.state == World::FarTileState::Uploaded;
            observation.empty_pairs += coverage.state == World::FarTileState::Empty;
            observation.covering += World::IsFarTileCovering(coverage);
            observation.ready_cpu_bytes +=
                entry.completion ? entry.completion->retained_bytes() : 0;
            observation.retained_gpu_bytes += entry.uploaded.gpu_bytes();
        }
    }
    return result;
}
bool FarVolumeResidency::CoarseMayYield(const World::FarTileKey& key) const {
    CheckOwner();
    const auto* coarse = Find(key);
    if (!coarse || key.tier <= 1 || key.tier > World::kFarTierCount)
        return false;
    for (std::int64_t z = 0; z < 2; ++z)
        for (std::int64_t x = 0; x < 2; ++x) {
            const World::FarTileKey child{
                std::uint8_t(key.tier - 1), key.tx * 2 + x, key.tz * 2 + z};
            const auto* entry = Find(child);
            if (!entry || !World::IsFarTileCovering(entry->uploaded.Status(entry->desired->identity,
                                                                           entry->desired->intent)))
                return false;
        }
    return true;
}
void FarVolumeResidency::CancelAndDrain() {
    Guard guard(*this);
    for (auto& tier : tiers_)
        tier->queue.CancelAndDrain();
    for (auto& tier : tiers_)
        for (auto& entry : tier->entries)
            entry = Entry{};
}
} // namespace Luminumbra::Rendering
