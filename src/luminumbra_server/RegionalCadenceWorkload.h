#pragma once

#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/Chunk.h"
#include <entt/entity/registry.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace Luminumbra::Server {

// Explicit finite smoke input. Eight distant region centres plus one unanchored
// control; nine sources fit the existing 256-source and 64-anchor admission caps.
struct RegionalCadenceWorkload {
    static constexpr std::size_t kAnchors = 8;
    static constexpr std::size_t kParticipants = kAnchors + 1;
    std::array<Vec3, kParticipants> positions{};
    std::array<entt::entity, kParticipants> plants{};

    explicit RegionalCadenceWorkload(const Vec3& origin) {
        if (!world::ActiveRegionLedger::valid_anchor(origin))
            throw std::invalid_argument("Invalid regional workload origin");
        const auto owner = world::ActiveRegionLedger::region_at(origin);
        // Fixed 2 km spacing, inside the existing coordinate envelope. Validate
        // every position before creating any entities or publishing host inputs.
        for (std::size_t i = 0; i < positions.size(); ++i) {
            positions[i] = Vec3(static_cast<float>(owner.x) * 512.0f + 256.0f +
                                    static_cast<float>(i % 3) * 2048.0f,
                                origin.y,
                                static_cast<float>(owner.z) * 512.0f + 256.0f +
                                    static_cast<float>(i / 3) * 2048.0f);
            if (!world::ActiveRegionLedger::valid_anchor(positions[i]))
                throw std::invalid_argument("Regional workload position outside world bounds");
        }
        plants.fill(entt::null);
    }

    [[nodiscard]] std::span<const Vec3> Anchors() const {
        return {positions.data(), kAnchors};
    }

    // The caller owns the game roster factory; this workload only supplies
    // bounded positions and retains handles for observation.
    template<typename Factory>
    void Populate(entt::registry& registry, const Factory& factory) {
        for (const auto entity : plants) {
            if (entity != entt::null)
                throw std::invalid_argument("Regional workload requires a fresh roster");
        }
        for (std::size_t i = 0; i < positions.size(); ++i) {
            plants[i] = factory(registry, positions[i]);
            if (plants[i] == entt::null)
                throw std::invalid_argument("Regional workload plant refused");
        }
    }
};

// Bounded observations only: no per-tick trace allocation and no world/page scan.
// Plant service is measured from the representative's published last_tick, never
// inferred from scheduler due membership. All counters stay outside session truth.
struct RegionalCadenceDiagnostics {
    struct Owner {
        world::RegionKey key{};
        std::uint64_t services = 0, elapsed_steps = 0, last_plant_tick = 0;
        std::uint32_t last_age = 0;
        std::uint64_t max_service_gap = 0, service_gap = 0;
        std::uint64_t frozen_samples = 0, unadmitted_samples = 0;
        std::uint64_t frozen_services = 0, missing_samples = 0;
        std::uint64_t water_services = 0, water_interior_windows = 0;
        std::uint64_t water_service_gap = 0, max_water_service_gap = 0;
        std::uint64_t frozen_water_windows = 0;
        bool operator==(const Owner&) const = default;
    };
    std::array<Owner, RegionalCadenceWorkload::kParticipants> owners{};
    std::uint64_t observations = 0, tick = 0;
    std::uint64_t owner_visits = 0, water_window_visits = 0;
    // These foliage counters describe the latest replay/preparation pass, not
    // cumulative units. Repeated readings must not be summed as new work.
    std::uint64_t replay_cell_visits_last = 0, replay_cell_visits_max = 0;
    std::uint64_t preparation_pages_last = 0, preparation_pages_max = 0;
    std::uint32_t replay_remaining_steps = 0;
    std::uint64_t water_publications = 0, water_deferred_samples = 0;
    std::uint64_t water_granted_windows = 0, water_border_windows = 0;
    std::uint64_t water_completed_cells = 0, water_preparation_cells = 0;
    std::uint64_t water_total_cells_max = 0, water_resident_visits = 0;
    std::uint64_t water_participant_visits = 0;
    std::int64_t water_sources_seen = 0;
    bool operator==(const RegionalCadenceDiagnostics&) const = default;

    template<typename Growth, typename Mortal>
    static RegionalCadenceDiagnostics Start(const RegionalCadenceWorkload& workload,
                                            const entt::registry& registry,
                                            std::uint64_t start_tick,
                                            const Systems::WaterSystem* water) {
        RegionalCadenceDiagnostics result;
        result.tick = start_tick;
        result.water_sources_seen = water ? water->debug_sources_seen() : 0;
        for (std::size_t i = 0; i < result.owners.size(); ++i) {
            auto& output = result.owners[i];
            output.key = world::ActiveRegionLedger::region_at(workload.positions[i]);
            if (const auto* growth = registry.try_get<Growth>(workload.plants[i]))
                output.last_plant_tick = growth->last_tick;
            if (const auto* mortal = registry.try_get<Mortal>(workload.plants[i]))
                output.last_age = mortal->age_ticks;
        }
        return result;
    }

    template<typename Growth, typename Mortal, typename Session>
    void Observe(const Session& session,
                 const entt::registry& registry,
                 const RegionalCadenceWorkload& workload,
                 const Systems::WaterSystem* water) {
        const auto now = session.GetSimulationTickCount();
        if (now <= tick || session.GetRegionSchedulerFault())
            return; // Refused/repeated opportunities are not completed tick samples.
        const auto interval = now - tick;
        tick = now;
        ++observations;
        const auto& records = session.GetActiveRegionLedger().records();
        for (std::size_t i = 0; i < owners.size(); ++i) {
            ++owner_visits;
            auto& output = owners[i];
            const auto record = records.find(output.key);
            const bool absent = record == records.end();
            const bool frozen = !absent && record->second.state == world::RegionState::Frozen;
            output.unadmitted_samples += absent ? 1u : 0u;
            output.frozen_samples += frozen ? 1u : 0u;
            output.water_service_gap += interval;
            output.max_water_service_gap =
                std::max(output.max_water_service_gap, output.water_service_gap);
            output.service_gap += interval;
            output.max_service_gap = std::max(output.max_service_gap, output.service_gap);
            const auto entity = workload.plants[i];
            const auto* growth =
                registry.valid(entity) ? registry.try_get<Growth>(entity) : nullptr;
            if (!growth) {
                ++output.missing_samples;
            } else if (growth->last_tick > output.last_plant_tick) {
                ++output.services;
                if (const auto* mortal = registry.try_get<Mortal>(entity)) {
                    if (mortal->age_ticks >= output.last_age)
                        output.elapsed_steps += mortal->age_ticks - output.last_age;
                    output.last_age = mortal->age_ticks;
                }
                output.last_plant_tick = growth->last_tick;
                output.frozen_services += frozen ? 1u : 0u;
                output.service_gap = 0;
            }
        }
        replay_cell_visits_last = session.GetFoliageReplayCellVisits();
        replay_cell_visits_max = std::max(replay_cell_visits_max, replay_cell_visits_last);
        preparation_pages_last = session.GetFoliageReplayPreparationPages();
        preparation_pages_max = std::max(preparation_pages_max, preparation_pages_last);
        replay_remaining_steps = session.GetFoliageReplayRemainingSteps();
        // Every completed regional water publication inspects all nine physical
        // sources, even on empty grants. This cumulative counter distinguishes a
        // publication from parking deferral, which retains the previous work object.
        if (!water || water->debug_sources_seen() == water_sources_seen) {
            ++water_deferred_samples;
            return;
        }
        water_sources_seen = water->debug_sources_seen();
        const auto& work = water->regional_work();
        ++water_publications;
        water_granted_windows += work.allocated_windows;
        water_border_windows += work.border_windows;
        water_completed_cells += work.water_cells;
        water_preparation_cells += work.preparation_cells;
        water_total_cells_max = std::max(water_total_cells_max, work.total_cells);
        water_resident_visits += work.residents_visited;
        water_participant_visits += work.participants_visited;
        // Interior visits are bounded by the existing global cell grant. The
        // nine-owner lookup is fixed work, independent of world/ledger size.
        for (auto& output : owners) {
            std::uint64_t windows = 0;
            for (const auto id : work.visits) {
                ++water_window_visits;
                const auto coords = Chunk::decode_id(id);
                const Vec3 position(static_cast<float>(coords.x) * CHUNK_SIZE_X,
                                    0,
                                    static_cast<float>(coords.z) * CHUNK_SIZE_Z);
                windows += world::ActiveRegionLedger::region_at(position) == output.key ? 1u : 0u;
            }
            if (windows == 0)
                continue;
            ++output.water_services;
            output.water_interior_windows += windows;
            output.water_service_gap = 0;
            const auto record = records.find(output.key);
            if (record != records.end() && record->second.state == world::RegionState::Frozen)
                output.frozen_water_windows += windows;
        }
    }
};

} // namespace Luminumbra::Server
