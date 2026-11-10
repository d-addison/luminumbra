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
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <entt/entt.hpp>
#include <stdexcept>
#include <type_traits>
#include <utility>
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
        if (m_prepared && !m_committed) {
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
    // Counts participant and birth visits during publication, excluding unrelated entities.
    std::uint64_t publication_entity_visits() const {
        return m_publicationEntityVisits;
    }
    std::uint64_t publications() const {
        return m_publications;
    }
    // Birth payloads stay in the candidate. Detached indexes reserve sparse pages
    // without installing entities or components in live registry queries.
    void PreparePublication(entt::registry& original) {
        if (m_prepared || m_committed) {
            throw std::runtime_error("Repeated foliage preparation");
        }
        for (const auto entity : m_retained) {
            ++m_publicationEntityVisits;
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
            ++m_publicationEntityVisits;
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
        m_births.clear();
        m_births.reserve(m_newPlants.size());
        {
            using EntityStorage = entt::storage<entt::entity>;
            auto& entities = static_cast<EntityStorage&>(original.storage<entt::entity>());
            // Entity storage is bounded by EnTT's entity mask, including recycled handles.
            if (m_newPlants.size() >
                entt::entt_traits<entt::entity>::entity_mask - entities.free_list()) {
                throw std::length_error("Foliage entity space exhausted");
            }
            DetachedStorage<EntityStorage> detached(entities);
            try {
                for (std::size_t index = 0; index < m_newPlants.size(); ++index) {
                    ++m_publicationEntityVisits;
                    m_births.push_back(detached.storage.generate());
                }
            } catch (...) {
                ReleaseHandles(detached.storage);
                throw;
            }
            ReleaseHandles(detached.storage);
            detached.keep_cursor = true;
        }
        stage_detail::Components([&]<typename T> {
            auto& storage = original.storage<T>();
            std::size_t count = 0;
            static_assert(entt::storage<T>::storage_policy == entt::deletion_policy::swap_and_pop);
            for (const auto entity : m_newPlants) {
                ++m_publicationEntityVisits;
                count += static_cast<std::size_t>(m_registry.all_of<T>(entity));
            }
            if (count == 0) {
                return;
            }
            // Each pool has at most entity_mask entries. Check before adding sizes.
            if (count > entt::entt_traits<entt::entity>::entity_mask - storage.size()) {
                throw std::length_error("Foliage component space exhausted");
            }
            const auto required = storage.size() + count;
            const auto capacity = std::min<std::size_t>(
                entt::entt_traits<entt::entity>::entity_mask,
                std::min(storage.entt::sparse_set::capacity(), storage.capacity()));
            if (required > capacity) {
                // Geometric growth keeps shared dense indexes amortized O(1) per birth.
                const auto growth =
                    capacity +
                    std::min(capacity, entt::entt_traits<entt::entity>::entity_mask - capacity);
                storage.reserve(std::max(required, growth));
            }
            // Payload capacity alone does not reserve EnTT's sparse entity pages.
            // Move only the index; existing payloads and their addresses stay put.
            DetachedStorage<entt::sparse_set> detached(storage);
            const auto size = detached.storage.size();
            try {
                for (std::size_t index = 0; index < m_newPlants.size(); ++index) {
                    ++m_publicationEntityVisits;
                    if (m_registry.all_of<T>(m_newPlants[index])) {
                        detached.storage.push(m_births[index]);
                    }
                }
            } catch (...) {
                TrimIndex(detached.storage, size);
                throw;
            }
            TrimIndex(detached.storage, size);
        });
        m_prepared = true;
    }
    void Commit(entt::registry& original) noexcept {
        if (!m_prepared || m_committed) {
            return;
        }
        // Preparation reserved every dense slot, payload page and sparse page.
        // Publish births before deaths so reserved handle order stays unchanged.
        for (std::size_t index = 0; index < m_newPlants.size(); ++index) {
            ++m_publicationEntityVisits;
            const auto live = original.create();
            assert(live == m_births[index]);
            stage_detail::Components([&]<typename T> {
                static_assert(std::is_nothrow_copy_constructible_v<T>);
                stage_detail::Copy<T>(m_registry, m_newPlants[index], original, live);
            });
        }
        for (const auto entity : m_retained) {
            ++m_publicationEntityVisits;
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
    void Abort() noexcept {
        m_births.clear();
        m_prepared = false;
    }

private:
    template<typename Storage>
    struct DetachedStorage {
        Storage& original;
        Storage storage;
        bool keep_cursor = false;
        explicit DetachedStorage(Storage& source)
            : original(source)
            , storage(std::move(source)) {}
        ~DetachedStorage() {
            if (keep_cursor) {
                original = std::move(storage);
            } else {
                // Generation can advance its detached cursor before allocation
                // throws. Restore indexes while preserving the live cursor.
                static_cast<entt::sparse_set&>(original).swap(storage);
            }
        }
    };
    static void TrimIndex(entt::sparse_set& storage, std::size_t size) noexcept {
        while (storage.size() > size) {
            storage.erase(storage.data()[storage.size() - 1]);
        }
    }
    void ReleaseHandles(entt::storage<entt::entity>& storage) const noexcept {
        for (auto birth = m_births.rbegin(); birth != m_births.rend(); ++birth) {
            storage.erase(*birth);
            storage.bump(*birth);
        }
    }
    entt::registry m_registry;
    std::vector<entt::entity> m_retained, m_births, m_newPlants;
    std::uint64_t m_initialId = 1, m_nextId = 1;
    std::uint64_t m_visits = 0, m_entityVisits = 0, m_publications = 0;
    std::uint64_t m_publicationEntityVisits = 0;
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
