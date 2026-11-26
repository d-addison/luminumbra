#pragma once

#include "RegionConsumerGate.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace luminumbra::foliage::plant_elapsed_detail {

// Tick-local schedule for the admitted plant roster, never the resident world.
// Windows end at due_tick, include the birth tick, and cannot exceed the existing
// 65,536-tick regional bound. Validate every participant before changing any plant.
class RegionalPlantTicks {
public:
    RegionalPlantTicks(const entt::registry& registry,
                       std::span<const entt::entity> participants,
                       const ::Luminumbra::world::RegionConsumerGate& gate,
                       std::uint64_t due_tick)
        : m_registry(registry)
        , m_dueTick(due_tick)
        , m_firstTick(due_tick) {
        m_entries.reserve(participants.size()); // At most one entry per admitted plant.
        m_active.reserve(participants.size());
        for (const auto entity : participants) {
            const auto elapsed = gate.ElapsedTicks(entity);
            if (elapsed > 65536 || (elapsed != 0 && elapsed - 1u > due_tick)) {
                throw std::invalid_argument("Invalid regional plant window");
            }
            if (elapsed == 0)
                continue;
            auto first = due_tick - (elapsed - 1u);
            if (const auto* growth =
                    registry.try_get<const ::Luminumbra::Components::PlantGrowthComponent>(
                        entity)) {
                first = std::max(first, growth->planted_tick);
            }
            if (first <= due_tick) {
                m_entries.push_back({entity, first});
                m_firstTick = std::min(m_firstTick, first);
            }
        }
    }

    template<typename Step>
    void Run(const Step& step) {
        if (m_entries.empty())
            return;
        // At most 65,536 steps, each visiting only the initially admitted roster.
        // Filtering retains identity order, allocates nothing and excludes history
        // before birth. Crop children are seeds: later crop-only steps are no-ops.
        for (auto tick = m_firstTick;; ++tick) {
            m_active.clear();
            for (const auto& entry : m_entries) {
                if (entry.first_tick <= tick && m_registry.valid(entry.entity))
                    m_active.push_back(entry.entity);
            }
            step(std::span<const entt::entity>(m_active),
                 tick,
                 static_cast<std::uint32_t>(m_dueTick - tick + 1u));
            if (tick == m_dueTick)
                break; // Also safe when due_tick is UINT64_MAX.
        }
    }

private:
    struct Entry {
        entt::entity entity;
        std::uint64_t first_tick;
    };
    const entt::registry& m_registry;
    std::uint64_t m_dueTick;
    std::uint64_t m_firstTick;
    std::vector<Entry> m_entries;
    std::vector<entt::entity> m_active;
};

// Regional telemetry counts events over the whole window. Keep the public legacy
// int counters and saturate their sums rather than overflowing on large rosters.
inline void AddCount(int& total, int count) {
    total += std::min(count, std::numeric_limits<int>::max() - total);
}

} // namespace luminumbra::foliage::plant_elapsed_detail
