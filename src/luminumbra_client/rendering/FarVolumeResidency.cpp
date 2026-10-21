#include "FarVolumeResidency.h"
#include "FarVolumeBuildQueue.h"
#include "FarVolumeRenderMesh.h"
#include "FarVolumeUpload.h"
#include "core/JobSystem.h"
#include "world/Chunk.h"
#include "world/FarTierTable.h"
#include "world/FarVolume.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeRuntime.h"
#include "world/FarVolumeTierMemory.h"
#include "world/FarWantedSet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
// Construction and residency selection guarantee these values are present.
template<typename Optional>
auto& RequireEngaged(Optional& value) {
    if (!value)
        throw std::logic_error("Far-volume residency missing required state");
    return *value;
}
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
std::size_t FarVolumeResidency::Tier::IndexSize(std::size_t count,
                                                const FarVolumeQueueLimits& limits) {
    if (count == 0 || count > limits.descriptor_slots || limits.result_slots == 0)
        throw std::invalid_argument("Far-volume residency needs bounded nonzero entries");
    constexpr auto bytes_per_entry = std::max(sizeof(Entry), 2 * sizeof(std::size_t));
    if (count > std::numeric_limits<std::size_t>::max() / bytes_per_entry)
        throw World::FarVolumeMemoryRefused();
    return count * 2;
}
FarVolumeResidency::Tier::Tier(JobSystem& jobs,
                               World::FarVolumeMemoryPool& pool,
                               std::size_t count,
                               const FarVolumeQueueLimits& limits,
                               std::pmr::memory_resource* upstream)
    : control(pool)
    , resource(control, upstream)
    , entries(&resource)
    , key_index(IndexSize(count, limits), 0, &resource)
    , order(&resource)
    , queue(jobs, pool, limits, upstream)
    , completion_slots(limits.result_slots) {
    entries.resize(count);
    order.reserve(count);
}
void FarVolumeResidency::Tier::RebuildIndex(FarVolumeAdmissionWork& work) {
    index_count = 0;
    // At most entry_slots descriptors and O(entry_slots * log(entry_slots))
    // comparisons, independent of coordinates or hash collisions. No allocation.
    for (std::size_t index = 0; index < entries.size(); ++index) {
        ++work.descriptor_visits;
        if (entries[index].desired)
            key_index[index_count++] = index;
    }
    std::sort(key_index.begin(),
              key_index.begin() + static_cast<std::ptrdiff_t>(index_count),
              [&](std::size_t a, std::size_t b) {
                  ++work.key_comparisons;
                  return entries[a].desired->identity.key < entries[b].desired->identity.key;
              });
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
    try {
        CancelAndDrain();
    } catch (...) {
        std::terminate();
    }
}
void FarVolumeResidency::CheckOwner() const {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("Far-volume residency outside owner thread");
}
const FarVolumeResidency::Entry* FarVolumeResidency::Find(const World::FarTileKey& key,
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
            return RequireEngaged(entry.desired).identity.key < sought;
        });
    if (found == end)
        return nullptr;
    if (work)
        ++work->key_comparisons;
    const auto& entry = tier->entries[*found];
    return entry.desired && entry.desired->identity.key == key ? &entry : nullptr;
}
FarVolumeResidency::Entry* FarVolumeResidency::Find(const World::FarTileKey& key,
                                                    FarVolumeAdmissionWork* work) {
    return const_cast<Entry*>(std::as_const(*this).Find(key, work));
}
FarVolumeAdmissionWork FarVolumeResidency::AdmissionWork() const {
    CheckOwner();
    return admission_work_;
}
bool FarVolumeResidency::ReplaceWanted(std::span<const FarVolumeWantedRequest> requests) {
    Guard const guard(*this);
    admission_work_ = {};
    auto& work = admission_work_;
    std::array<std::size_t, World::kFarTierCount> counts{}, next_free{};
    for (auto& tier : tiers_) {
        auto& value = RequireEngaged(tier);
        value.order.assign(value.entries.size(), 0);
    }
    // Each tier admits at most entry_slots input positions into charged scratch.
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        if (!Valid(request))
            return false;
        const auto number = std::size_t(request.identity.key.tier - 1);
        auto& tier = RequireEngaged(tiers_[number]);
        auto& count = counts[number];
        if (count == tier.entries.size())
            return false;
        tier.key_index[tier.entries.size() + count++] = i;
    }
    for (std::size_t i = 0; i < tiers_.size(); ++i) {
        auto& tier = RequireEngaged(tiers_[i]);
        auto incoming = std::span(tier.key_index).subspan(tier.entries.size(), counts[i]);
        std::sort(incoming.begin(), incoming.end(), [&](std::size_t a, std::size_t b) {
            ++work.key_comparisons;
            return requests[a].identity.key < requests[b].identity.key;
        });
        for (std::size_t j = 1; j < incoming.size(); ++j) {
            ++work.key_comparisons;
            if (requests[incoming[j - 1]].identity.key == requests[incoming[j]].identity.key)
                return false;
        }
    }
    for (const auto& request : requests) {
        const auto* old = Find(request.identity.key, &work);
        if (old) {
            auto& tier = RequireEngaged(tiers_[request.identity.key.tier - 1]);
            tier.order[static_cast<std::size_t>(old - tier.entries.data())] = 1;
        }
        if (old && old->desired && old->desired->identity == request.identity &&
            (old->desired->intent != request.intent ||
             old->desired->build_headroom_bytes != request.build_headroom_bytes ||
             bool(old->desired->water) != bool(request.water) ||
             old->desired->water_bound != request.water_bound ||
             (old->desired->water && request.water &&
              old->desired->water->content_identity() != request.water->content_identity())))
            return false;
        if (old && old->desired &&
            old->desired->identity.world_epoch == request.identity.world_epoch &&
            old->desired->intent.extra_span &&
            (!request.intent.extra_span ||
             request.intent.extra_span->min_y > old->desired->intent.extra_span->min_y ||
             request.intent.extra_span->max_y < old->desired->intent.extra_span->max_y))
            return false;
    }
    bool keys_changed = false;
    std::size_t old_count = 0;
    for (const auto& slot : tiers_) {
        const auto& tier = RequireEngaged(slot);
        for (std::size_t index = 0; index < tier.entries.size(); ++index) {
            ++work.descriptor_visits;
            if (tier.entries[index].desired) {
                ++old_count;
                keys_changed |= tier.order[index] == 0;
            }
        }
    }
    keys_changed = keys_changed || old_count != requests.size();
    // Validation is complete. Batch queue retirement avoids a descriptor search
    // for every removed/replaced key. Scratch and entries never grow here.
    for (std::size_t i = 0; i < tiers_.size(); ++i) {
        auto& tier = RequireEngaged(tiers_[i]);
        const auto incoming = std::span(tier.key_index).subspan(tier.entries.size(), counts[i]);
        work.descriptor_visits +=
            tier.queue.ForgetMatching([&](const World::FarTileKey& key) noexcept {
                const auto found =
                    std::lower_bound(incoming.begin(),
                                     incoming.end(),
                                     key,
                                     [&](std::size_t index, const World::FarTileKey& sought) {
                                         ++work.key_comparisons;
                                         return requests[index].identity.key < sought;
                                     });
                if (found == incoming.end())
                    return true;
                ++work.key_comparisons;
                if (requests[*found].identity.key != key)
                    return true;
                const auto* entry = Find(key, &work);
                return !entry || !entry->desired ||
                       entry->desired->identity != requests[*found].identity ||
                       (keys_changed &&
                        (entry->eligibility == FarVolumeEligibility::RequestRejected ||
                         entry->eligibility == FarVolumeEligibility::BudgetEvicted));
            });
        for (std::size_t index = 0; index < tier.entries.size(); ++index) {
            ++work.descriptor_visits;
            auto& entry = tier.entries[index];
            if (entry.desired && tier.order[index] == 0)
                entry = Entry{};
        }
        if (keys_changed)
            tier.RebuildIndex(work);
    }
    for (const auto& request : requests) {
        const auto number = std::size_t(request.identity.key.tier - 1);
        auto& tier = RequireEngaged(tiers_[number]);
        auto* entry = Find(request.identity.key, &work);
        if (!entry) {
            // Monotonic per-tier cursor: at most entry_slots visits in total.
            auto& cursor = next_free[number];
            while (cursor < tier.entries.size()) {
                ++work.descriptor_visits;
                if (!tier.entries[cursor].desired)
                    break;
                ++cursor;
            }
            if (cursor == tier.entries.size())
                throw std::logic_error("Far-volume residency missing admitted entry");
            entry = &tier.entries[cursor++];
        }
        const bool changed = !entry->desired || entry->desired->identity != request.identity;
        if (changed) {
            entry->completion.reset();
            if (entry->eligibility != FarVolumeEligibility::BudgetEvicted || keys_changed)
                entry->eligibility = FarVolumeEligibility::Wanted;
            entry->desired = request;
        } else if (keys_changed && (entry->eligibility == FarVolumeEligibility::RequestRejected ||
                                    entry->eligibility == FarVolumeEligibility::BudgetEvicted)) {
            entry->eligibility = FarVolumeEligibility::Wanted;
        }
    }
    if (keys_changed)
        for (auto& tier : tiers_)
            RequireEngaged(tier).RebuildIndex(work);
    return true;
}
void FarVolumeResidency::Reject(Entry& entry, FarVolumeFrameResult& result) {
    const auto& desired = RequireEngaged(entry.desired);
    auto& tier = RequireEngaged(tiers_[desired.identity.key.tier - 1]);
    tier.queue.Forget(desired.identity.key);
    entry.completion.reset();
    entry.eligibility = FarVolumeEligibility::RequestRejected;
    ++result.rejected;
    if (desired.identity.key.tier == World::kFarTierCount)
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
    auto& tier = RequireEngaged(tiers_[number - 1]);
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
                (entry.distance == candidate->distance &&
                 key < RequireEngaged(candidate->desired).identity.key))
                candidate = &entry;
        }
        if (!candidate)
            break;
        auto& entry = *candidate;
        const auto& key = RequireEngaged(entry.desired).identity.key;
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
    Guard const guard(*this);
    FarVolumeFrameResult result;
    if (!std::isfinite(x))
        x = 0;
    if (!std::isfinite(z))
        z = 0;
    for (auto& tier : tiers_) {
        if (!tier)
            throw std::logic_error("Far-volume residency missing tier");
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
        auto& tier = RequireEngaged(tiers_[n - 1]);
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
            const auto& desired = RequireEngaged(entry.desired);
            if (!Headroom(n, *bytes, desired.identity.key, result)) {
                Reject(entry, result);
                continue;
            }
            const auto remaining = limits.upload_bytes - result.upload_bytes;
            result.upload_bytes += *bytes;
            if (!UploadFarVolume(*entry.completion,
                                 desired.identity,
                                 desired.intent,
                                 memory_,
                                 remaining,
                                 backend,
                                 entry.uploaded)) {
                Reject(entry, result);
                continue;
            }
            entry.completion.reset();
            entry.eligibility = FarVolumeEligibility::Settled;
            ++result.uploaded;
            result.successful_upload_bytes += *bytes;
        }
    }
    // One global dispatch budget; explicit single-key Pump avoids fallback.
    for (std::size_t n = tiers_.size(); n > 0; --n) {
        auto& tier = RequireEngaged(tiers_[n - 1]);
        // Retained results share the queue allowance until upload or discard.
        // Headroom eviction can only reduce this frame's conservative count.
        std::size_t held = 0;
        for (const auto& entry : tier.entries)
            held += entry.completion.has_value();
        for (const auto i : tier.order) {
            auto& entry = tier.entries[i];
            if (entry.eligibility != FarVolumeEligibility::Wanted &&
                entry.eligibility != FarVolumeEligibility::Pending)
                continue;
            if (result.build_attempts == limits.builds)
                break;
            if (tier.queue.ResultCapacity(held) == 0)
                break;
            ++result.build_attempts;
            if (entry.eligibility == FarVolumeEligibility::Pending) {
                const std::array key{RequireEngaged(entry.desired).identity.key};
                const auto dispatched = tier.queue.Pump(1, key, held);
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
                submitted = tier.queue.Submit({entry.desired->identity,
                                               entry.desired->intent,
                                               *authority,
                                               entry.desired->water,
                                               entry.desired->water_bound});
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
            const auto dispatched = tier.queue.Pump(1, key, held);
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
    if (!entry || !entry->desired)
        return {};
    return FarVolumeResidentObservation{
        entry->desired->identity,
        entry->eligibility,
        entry->uploaded.Status(entry->desired->identity, entry->desired->intent),
        entry->completion ? entry->completion->retained_bytes() : 0,
        entry->uploaded.gpu_bytes(),
        entry->completion ? UploadBytes(entry->completion->mesh())
                                .value_or(std::numeric_limits<std::uint64_t>::max())
                          : 0,
        entry->uploaded.identity(),
        entry->uploaded.request(),
        entry->uploaded.water_complete(),
        entry->uploaded.water_known_dry(),
        entry->uploaded.terrain_indices(),
        entry->uploaded.water_indices()};
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
        for (const auto& entry : RequireEngaged(tiers_[i]).entries) {
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
            observation.retained_water_indices += entry.uploaded.water_indices();
            if (World::IsFarTileCovering(coverage) && entry.uploaded.water_complete()) {
                observation.water_known_dry_tiles += entry.uploaded.water_known_dry();
                observation.water_wet_tiles += !entry.uploaded.water_known_dry();
            }
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
            if (!entry || !entry->desired ||
                !World::IsFarTileCovering(
                    entry->uploaded.Status(entry->desired->identity, entry->desired->intent)))
                return false;
        }
    return true;
}
void FarVolumeResidency::CancelAndDrain() {
    Guard const guard(*this);
    for (auto& tier : tiers_)
        RequireEngaged(tier).queue.CancelAndDrain();
    for (auto& tier : tiers_) {
        auto& value = RequireEngaged(tier);
        for (auto& entry : value.entries)
            entry = Entry{};
        value.index_count = 0;
    }
}
std::size_t FarVolumeResidency::VisitDraws(FarVolumeDrawVisitor visitor, void* context) {
    Guard const guard(*this);
    if (!visitor)
        return 0;
    std::size_t count = 0;
    for (std::size_t number = tiers_.size(); number > 0; --number) {
        const auto& dimensions = World::kFarTierTable[number - 1];
        const auto edge = std::int64_t(dimensions.tile_edge_meters);
        for (const auto& entry : RequireEngaged(tiers_[number - 1]).entries) {
            if (!entry.desired)
                continue;
            const auto& identity = entry.desired->identity;
            const auto coverage = entry.uploaded.Status(identity, entry.desired->intent);
            if (!World::IsFarTileCovering(coverage) || !entry.uploaded.resource())
                continue;
            const auto& request = entry.uploaded.request();
            FarVolumeDrawRecord record{
                identity,
                request,
                {identity.key.tx * edge, request.span.min_y, identity.key.tz * edge},
                entry.uploaded.resource(),
                World::FarWantedRadiusMeters(number),
                double(dimensions.sample_spacing_meters) / 2,
                {}};
            record.terrain_indices = entry.uploaded.terrain_indices();
            record.water_indices = entry.uploaded.water_indices();
            record.arrival.eligible = CoarseMayYield(identity.key);
            if (record.arrival.eligible) {
                record.arrival.finer_outer_radius_meters = World::FarWantedRadiusMeters(number - 1);
                for (std::int64_t z = 0; z < 2; ++z) {
                    for (std::int64_t x = 0; x < 2; ++x) {
                        const World::FarTileKey child{std::uint8_t(number - 1),
                                                      identity.key.tx * 2 + x,
                                                      identity.key.tz * 2 + z};
                        record.arrival.children[static_cast<std::size_t>(x + 2 * z)] =
                            Find(child)->uploaded.request().span;
                    }
                }
            }
            visitor(context, record);
            ++count;
        }
    }
    return count;
}
} // namespace Luminumbra::Rendering
