#pragma once

#include "../components/CombustionComponents.h"
#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/CropLifecycleComponents.h"
#include "../components/DiseaseComponents.h"
#include "../components/GrazeableComponent.h"
#include "../components/IrrigationComponents.h"
#include "../components/MortalComponents.h"
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h"
#include "../components/SoilComponents.h"
#include "../core/WaterComponents.h"
#include "../fields/FieldPageState.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <entt/entt.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace luminumbra::foliage {
namespace stage_detail {
namespace C = ::Luminumbra::Components;
inline std::atomic<std::uint64_t>& RegistryStageCount() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}
inline std::atomic<std::uint64_t>& RegistryStagedEntityCount() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

inline bool BorderDue(const std::map<::Luminumbra::world::RegionKey,
                                     ::Luminumbra::world::ActiveRegionRecord>& records,
                      ::Luminumbra::world::RegionKey owner,
                      ::Luminumbra::world::RegionKey other,
                      std::uint64_t tick) {
    using ::Luminumbra::world::RegionState;
    if (owner == other)
        return false;
    const auto a = records.find(owner), b = records.find(other);
    if (a == records.end() || b == records.end() || a->second.state == RegionState::Frozen ||
        b->second.state == RegionState::Frozen)
        return false;
    const auto shift = std::max(a->second.cadence_shift, b->second.cadence_shift);
    return tick % (std::uint64_t{1} << shift) == 0;
}

template<class T>
void Copy(const entt::registry& from,
          entt::entity source,
          entt::registry& to,
          entt::entity target) {
    // EnTT's empty-component storage has no addressable payload for try_get.
    if constexpr (std::is_empty_v<T>) {
        if (from.all_of<T>(source))
            to.emplace_or_replace<T>(target);
    } else {
        if (const auto* component = from.try_get<const T>(source))
            to.emplace_or_replace<T>(target, *component);
    }
}
inline void CopyFoliage(const entt::registry& from,
                        entt::entity source,
                        entt::registry& to,
                        entt::entity target) {
    Copy<C::TransformComponent>(from, source, to, target);
    Copy<C::PlantTag>(from, source, to, target);
    Copy<C::PlantRegionComponent>(from, source, to, target);
    Copy<C::PlantGenomeComponent>(from, source, to, target);
    Copy<C::PlantGrowthComponent>(from, source, to, target);
    Copy<C::SoilFeederComponent>(from, source, to, target);
    Copy<C::PlantHealthComponent>(from, source, to, target);
    Copy<C::CropLifecycleComponent>(from, source, to, target);
    Copy<C::PollinationComponent>(from, source, to, target);
    Copy<C::PollinationTag>(from, source, to, target);
    Copy<C::CombustibleComponent>(from, source, to, target);
    Copy<C::IgnitionSourceComponent>(from, source, to, target);
    Copy<C::WaterSourceComponent>(from, source, to, target);
    Copy<C::WaterFlowSourceComponent>(from, source, to, target);
    Copy<C::GrazeableComponent>(from, source, to, target);
    Copy<C::MortalComponent>(from, source, to, target);
    Copy<C::CreatureComponent>(from, source, to, target);
}
} // namespace stage_detail

struct RegionalFoliageWindows {
    std::map<::Luminumbra::world::RegionKey, std::uint32_t> elapsed;
    std::map<::Luminumbra::world::RegionKey, std::uint64_t> resumes;
};

// Prepare the exact windows consumed by the tick, before taking any snapshots.
inline RegionalFoliageWindows BuildRegionalFoliageWindows(
    const ::Luminumbra::world::ActiveRegionLedger& ledger,
    const std::vector<::Luminumbra::world::RegionKey>& due,
    const std::map<::Luminumbra::world::RegionKey, std::uint64_t>& previous_ticks,
    const ::Luminumbra::world::WorldClock& clock,
    std::uint64_t tick) {
    using ::Luminumbra::world::ActiveRegionLedger;
    using ::Luminumbra::world::RegionState;
    RegionalFoliageWindows result;
    for (const auto& [key, record] : ledger.records()) {
        if (record.state == RegionState::Frozen)
            continue;
        if (record.frozen_at) {
            const auto window =
                ActiveRegionLedger::resume_window(key, record.frozen_at, clock, 36000);
            result.elapsed[key] = static_cast<std::uint32_t>(window.elapsed_ticks);
            result.resumes[key] = record.frozen_at;
        } else if (std::binary_search(due.begin(), due.end(), key)) {
            const auto old = previous_ticks.find(key);
            // Zero is a valid persisted tick. Fresh owners start at first_active.
            const auto prior = old == previous_ticks.end()
                                   ? record.first_active
                                   : std::max(old->second, record.first_active);
            result.elapsed[key] =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(tick - prior, 65536));
        }
    }
    return result;
}

inline bool HasRegionalFoliageWindowWork(const entt::registry& registry,
                                         const RegionalFoliageWindows& windows,
                                         std::uint64_t tick) {
    if (!windows.resumes.empty())
        return true;
    for (const auto& [key, elapsed] : windows.elapsed)
        if (elapsed != 0)
            return true;
    if (windows.elapsed.empty())
        return false;
    // Zero-length windows can still publish a plant stamp. Inspect entities
    // only in this case; an off-due tick never walks the plant registry.
    namespace C = ::Luminumbra::Components;
    for (auto entity : registry.view<const C::PlantTag,
                                     const C::PlantRegionComponent,
                                     const C::PlantGrowthComponent>()) {
        const auto& owner = registry.get<const C::PlantRegionComponent>(entity);
        if (windows.elapsed.contains({owner.owner_x, owner.owner_z}) &&
            (owner.region_stamp != tick ||
             registry.get<const C::PlantGrowthComponent>(entity).last_tick != tick))
            return true;
    }
    return false;
}

// Border exchange counts every eligible edge, even with zero moisture flux.
// Probe only the positive page edges: an interior cell cannot cross a region
// boundary. This reads at most 32 edges per page, bounded by kMaxPages (4096),
// without expanding or copying pages. Validated page coordinates fit x/z * 16.
namespace stage_detail {
template<class Visitor>
inline bool
VisitRegionalFoliageBorderPages(const ::Luminumbra::world::FieldPageState& pages,
                                const std::map<::Luminumbra::world::RegionKey,
                                               ::Luminumbra::world::ActiveRegionRecord>& records,
                                std::uint64_t tick,
                                Visitor&& visit) {
    using ::Luminumbra::world::FieldPageState;
    const auto& irrigation = pages.irrigation();
    for (const auto& [key, page] : irrigation.pages()) {
        const int x = key.x * 16, z = key.z * 16;
        const auto owner = FieldPageState::region_of_scent_cell({x, z});
        const bool along_x = stage_detail::BorderDue(
            records, owner, FieldPageState::region_of_scent_cell({x + 16, z}), tick);
        const bool along_z = stage_detail::BorderDue(
            records, owner, FieldPageState::region_of_scent_cell({x, z + 16}), tick);
        if (!along_x && !along_z)
            continue;
        for (int offset = 0; offset < 16; ++offset) {
            if (along_x && irrigation.initialized({x + 15, z + offset}) &&
                irrigation.initialized({x + 16, z + offset}) &&
                visit(key, ::Luminumbra::world::FieldPageKey{key.x + 1, key.z}))
                return true;
            if (along_z && irrigation.initialized({x + offset, z + 15}) &&
                irrigation.initialized({x + offset, z + 16}) &&
                visit(key, ::Luminumbra::world::FieldPageKey{key.x, key.z + 1}))
                return true;
        }
    }
    return false;
}
} // namespace stage_detail

inline bool
HasRegionalFoliageBorderWork(const ::Luminumbra::world::FieldPageState& pages,
                             const std::map<::Luminumbra::world::RegionKey,
                                            ::Luminumbra::world::ActiveRegionRecord>& records,
                             std::uint64_t tick) {
    return stage_detail::VisitRegionalFoliageBorderPages(
        pages, records, tick, [](auto, auto) { return true; });
}

inline std::set<::Luminumbra::world::FieldPageKey>
BuildRegionalFoliageBorderPages(const ::Luminumbra::world::FieldPageState& pages,
                                const std::map<::Luminumbra::world::RegionKey,
                                               ::Luminumbra::world::ActiveRegionRecord>& records,
                                std::uint64_t tick) {
    // Both endpoints already exist: the set is bounded by kMaxPages (4096).
    std::set<::Luminumbra::world::FieldPageKey> result;
    stage_detail::VisitRegionalFoliageBorderPages(pages, records, tick, [&result](auto a, auto b) {
        result.insert(a);
        result.insert(b);
        return false;
    });
    return result;
}

// Read-only diagnostic: number of registry staging attempts in this process.
// It is observational only and never enters simulation or persistence state.
inline std::uint64_t FoliageRegistryStageCount() {
    return stage_detail::RegistryStageCount().load(std::memory_order_relaxed);
}

// Read-only diagnostic: cumulative entities copied into foliage snapshots.
// Like the attempt count, this never enters simulation or persistence state.
inline std::uint64_t FoliageRegistryStagedEntityCount() {
    return stage_detail::RegistryStagedEntityCount().load(std::memory_order_relaxed);
}

// Staging preserves existing handles. Fresh births receive live handles only at
// commit; their simulation identity is the already allocated persistent ID.
// All unrelated components on retained entities remain in the original registry.
inline entt::registry StageFoliageRegistry(const entt::registry& original,
                                           std::set<entt::entity>& retained) {
    stage_detail::RegistryStageCount().fetch_add(1, std::memory_order_relaxed);
    namespace C = ::Luminumbra::Components;
    entt::registry stage;
    for (auto e : original.view<const C::TransformComponent>()) {
        if (!original.any_of<C::PlantTag,
                             C::CombustibleComponent,
                             C::IgnitionSourceComponent,
                             C::WaterSourceComponent,
                             C::GrazeableComponent,
                             C::MortalComponent,
                             C::CreatureComponent>(e))
            continue;
        if (stage.create(e) != e)
            throw std::runtime_error("Foliage handle staging refused");
        stage_detail::CopyFoliage(original, e, stage, e);
        retained.insert(e);
        stage_detail::RegistryStagedEntityCount().fetch_add(1, std::memory_order_relaxed);
    }
    return stage;
}

// Snapshot only the due participants; the live registry remains the rollback
// source. Component views avoid walking unrelated transformed entities. Keep the
// original transform-storage order so retained component traversal is unchanged.
inline entt::registry
StageFoliageRegistry(const entt::registry& original,
                     std::set<entt::entity>& retained,
                     const RegionalFoliageWindows& windows,
                     const std::set<::Luminumbra::world::FieldPageKey>& border_pages) {
    using ::Luminumbra::world::ActiveRegionLedger;
    namespace C = ::Luminumbra::Components;
    stage_detail::RegistryStageCount().fetch_add(1, std::memory_order_relaxed);
    const auto collect = [&]<class T>() {
        for (auto e : original.view<const T>()) {
            if (retained.contains(e))
                continue;
            const auto* transform = original.try_get<const C::TransformComponent>(e);
            if (!transform)
                continue;
            const auto& position = transform->position;
            // Plant stamps use durable ownership, including zero-length windows.
            const auto* owner = original.try_get<const C::PlantRegionComponent>(e);
            bool participates = original.all_of<C::PlantTag>(e) && owner &&
                                windows.elapsed.contains({owner->owner_x, owner->owner_z});
            if (!participates && ActiveRegionLedger::valid_anchor(position)) {
                const auto window = windows.elapsed.find(ActiveRegionLedger::region_at(position));
                participates = window != windows.elapsed.end() && window->second != 0;
                if (!participates && !border_pages.empty()) {
                    // Valid anchors bound page coordinates to [-1048576, 1048576).
                    const ::Luminumbra::world::FieldPageKey page{
                        static_cast<std::int32_t>(
                            std::floor(static_cast<double>(position.x) / 16.0)),
                        static_cast<std::int32_t>(
                            std::floor(static_cast<double>(position.z) / 16.0))};
                    participates = border_pages.contains(page);
                }
            }
            if (participates)
                retained.insert(e);
        }
    };
    collect.template operator()<C::PlantTag>();
    collect.template operator()<C::CombustibleComponent>();
    collect.template operator()<C::IgnitionSourceComponent>();
    collect.template operator()<C::WaterSourceComponent>();
    collect.template operator()<C::GrazeableComponent>();
    collect.template operator()<C::MortalComponent>();
    collect.template operator()<C::CreatureComponent>();
    std::vector<entt::entity> ordered(retained.begin(), retained.end());
    const auto* transforms = original.storage<C::TransformComponent>();
    std::sort(ordered.begin(), ordered.end(), [transforms](auto a, auto b) {
        return transforms->index(a) > transforms->index(b);
    });
    entt::registry stage;
    for (auto e : ordered) {
        if (stage.create(e) != e)
            throw std::runtime_error("Foliage handle staging refused");
        stage_detail::CopyFoliage(original, e, stage, e);
        stage_detail::RegistryStagedEntityCount().fetch_add(1, std::memory_order_relaxed);
    }
    return stage;
}
inline void CommitFoliageRegistry(entt::registry& original,
                                  const entt::registry& stage,
                                  const std::set<entt::entity>& retained) {
    for (const auto e : retained) {
        if (stage.valid(e))
            stage_detail::CopyFoliage(stage, e, original, e);
        else
            original.destroy(e);
    }
    for (auto e : stage.view<const ::Luminumbra::Components::PlantTag>())
        if (!retained.contains(e)) {
            const auto live = original.create();
            stage_detail::CopyFoliage(stage, e, original, live);
        }
}
} // namespace luminumbra::foliage
