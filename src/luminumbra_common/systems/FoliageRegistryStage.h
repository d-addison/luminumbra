#pragma once

#include "../components/AlarmComponents.h"
#include "../components/CombustionComponents.h"
#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/CropLifecycleComponents.h"
#include "../components/DecayComponents.h"
#include "../components/DiseaseComponents.h"
#include "../components/GrazeableComponent.h"
#include "../components/IrrigationComponents.h"
#include "../components/MortalComponents.h"
#include "../components/PackHunterComponents.h"
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h"
#include "../components/SoilComponents.h"
#include "../core/WaterComponents.h"
#include "../world/ActiveRegionLedger.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entt.hpp>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace luminumbra::foliage {
namespace stage_detail {
namespace C = ::Luminumbra::Components;
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
template<typename Function>
void Participants(const Function& function) {
    function.template operator()<C::PlantTag>();
    function.template operator()<C::PollinationTag>();
    function.template operator()<C::CropLifecycleComponent>();
    function.template operator()<C::SoilFeederComponent>();
    function.template operator()<C::WaterSourceComponent>();
    function.template operator()<C::CombustibleComponent>();
    function.template operator()<C::IgnitionSourceComponent>();
    function.template operator()<C::GrazeableComponent>();
    function.template operator()<C::MortalComponent>();
    function.template operator()<C::CreatureComponent>();
    function.template operator()<C::AlarmComponent>();
    function.template operator()<C::DecayComponent>();
}
template<typename Function>
void Components(const Function& function) {
    function.template operator()<C::TransformComponent>();
    function.template operator()<C::PlantTag>();
    function.template operator()<C::PlantRegionComponent>();
    function.template operator()<C::PlantGenomeComponent>();
    function.template operator()<C::PlantGrowthComponent>();
    function.template operator()<C::SoilFeederComponent>();
    function.template operator()<C::PlantHealthComponent>();
    function.template operator()<C::CropLifecycleComponent>();
    function.template operator()<C::PollinationComponent>();
    function.template operator()<C::PollinationTag>();
    function.template operator()<C::CombustibleComponent>();
    function.template operator()<C::IgnitionSourceComponent>();
    function.template operator()<C::WaterSourceComponent>();
    function.template operator()<C::FlowingWaterSourceComponent>();
    function.template operator()<C::GrazeableComponent>();
    function.template operator()<C::MortalComponent>();
    function.template operator()<C::CreatureComponent>();
    function.template operator()<C::PackHunterComponent>();
    function.template operator()<C::AlarmComponent>();
    function.template operator()<C::DecayComponent>();
}
inline void CopyFoliage(const entt::registry& from,
                        entt::entity source,
                        entt::registry& to,
                        entt::entity target) {
    Components([&]<typename T> { Copy<T>(from, source, to, target); });
}
} // namespace stage_detail
// Candidate storage; the owner keeps live pools stable and free of mutation listeners.
// Retained handles stay stable; births acquire live handles after consumers succeed.
class FoliageRegistryStage {
public:
    static bool HasWork(const entt::registry& original) {
        bool has_work = false;
        stage_detail::Participants(
            [&]<typename T> { has_work = has_work || !original.view<const T>().empty(); });
        return has_work;
    }
    void Begin(const entt::registry& original, std::uint64_t next_id) {
        if (!m_births.empty()) {
            throw std::runtime_error("Unfinished foliage publication");
        }
        ++m_visits;
        m_prepared = false;
        m_committed = false;
        m_initialId = m_nextId = next_id;
        m_registry.clear();
        m_retained.clear();
        // EnTT pools have the entity-mask bound; known capacities survive ticks.
        stage_detail::Participants([&]<typename T> { Collect<T>(original); });
        std::sort(m_retained.begin(), m_retained.end());
        stage_detail::Components([&]<typename T> {
            if (const auto* storage = original.storage<T>()) {
                m_registry.storage<T>().reserve(std::min(storage->size(), m_retained.size()));
            }
        });
        for (const auto entity : m_retained) {
            stage_detail::CopyFoliage(original, entity, m_registry, entity);
        }
    }
    entt::registry& registry() {
        return m_registry;
    }
    bool AllocatePersistentId(std::uint64_t& id) {
        if (m_nextId >= ::Luminumbra::world::ActiveRegionLedger::kPersistentIdLimit) {
            return false;
        }
        id = m_nextId++;
        return true;
    }
    std::uint64_t initial_id() const {
        return m_initialId;
    }
    std::uint64_t next_id() const {
        return m_nextId;
    }
    std::uint64_t visits() const {
        return m_visits;
    }
    std::uint64_t entity_visits() const {
        return m_entityVisits;
    }
    std::uint64_t publications() const {
        return m_publications;
    }
    // All potentially allocating publication work precedes retained mutations.
    // Abort restores provisional births in reverse allocation order, including
    // their versions, so the next live handle is unchanged after a refusal.
    void PreparePublication(entt::registry& original) {
        if (m_prepared || m_committed) {
            throw std::runtime_error("Repeated foliage preparation");
        }
        for (const auto entity : m_retained) {
            if (!original.valid(entity)) {
                throw std::runtime_error("Stale foliage handle");
            }
            if (!m_registry.valid(entity)) {
                continue;
            }
            stage_detail::Components([&]<typename T> {
                if (original.all_of<T>(entity) != m_registry.all_of<T>(entity)) {
                    throw std::runtime_error("Foliage component topology changed");
                }
            });
        }
        const auto plants = m_registry.view<const ::Luminumbra::Components::PlantTag>();
        m_newPlants.clear();
        m_newPlants.reserve(plants.size());
        for (const auto entity : plants) {
            if (!std::binary_search(m_retained.begin(), m_retained.end(), entity)) {
                m_newPlants.push_back(entity);
            }
        }
        // Persistent identities retain birth order even when candidate handles recycle.
        std::sort(m_newPlants.begin(), m_newPlants.end(), [this](auto left, auto right) {
            using Region = ::Luminumbra::Components::PlantRegionComponent;
            const auto* left_region = m_registry.try_get<const Region>(left);
            const auto* right_region = m_registry.try_get<const Region>(right);
            const auto left_id = left_region ? left_region->persistent_id : std::uint64_t{0};
            const auto right_id = right_region ? right_region->persistent_id : std::uint64_t{0};
            return left_id == right_id ? left < right : left_id < right_id;
        });
        m_births.reserve(m_newPlants.size());
        try {
            for (const auto entity : m_newPlants) {
                const auto live = original.create();
                m_births.push_back(live);
                stage_detail::CopyFoliage(m_registry, entity, original, live);
            }
            m_prepared = true;
        } catch (...) {
            Abort(original);
            throw;
        }
    }
    void Commit(entt::registry& original) noexcept {
        if (!m_prepared || m_committed) {
            return;
        }
        for (const auto entity : m_retained) {
            if (!m_registry.valid(entity)) {
                original.destroy(entity);
                continue;
            }
            stage_detail::Components([&]<typename T> {
                static_assert(std::is_nothrow_copy_assignable_v<T>);
                if constexpr (!std::is_empty_v<T>) {
                    if (const auto* value = m_registry.try_get<const T>(entity)) {
                        original.get<T>(entity) = *value;
                    }
                }
            });
        }
        m_births.clear();
        m_committed = true;
        ++m_publications;
    }
    void Abort(entt::registry& original) noexcept {
        for (auto birth = m_births.rbegin(); birth != m_births.rend(); ++birth) {
            original.destroy(*birth, entt::to_version(*birth));
        }
        m_births.clear();
        m_prepared = false;
    }

private:
    entt::registry m_registry;
    std::vector<entt::entity> m_retained, m_births, m_newPlants;
    std::uint64_t m_initialId = 1, m_nextId = 1;
    std::uint64_t m_visits = 0, m_entityVisits = 0, m_publications = 0;
    bool m_prepared = false, m_committed = false;
    template<typename T>
    void Collect(const entt::registry& original) {
        const auto view = original.view<const T>();
        if (view.size() > m_retained.max_size() - m_retained.size()) {
            throw std::length_error("Foliage roster too large");
        }
        m_retained.reserve(m_retained.size() + view.size());
        for (const auto entity : view) {
            ++m_entityVisits;
            if (m_registry.valid(entity)) {
                continue;
            }
            if (m_retained.size() == m_retained.max_size()) {
                throw std::length_error("Foliage roster too large");
            }
            m_retained.push_back(entity);
            if (m_registry.create(entity) != entity) {
                throw std::runtime_error("Foliage handle staging refused");
            }
        }
    }
};
} // namespace luminumbra::foliage
