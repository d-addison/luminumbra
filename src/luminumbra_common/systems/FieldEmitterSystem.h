#pragma once

//  -5: FieldEmitterSystem — the deposit GATHER pass
// feeding the stateful energy layer.  of the two-phase deposit path:
// visit every FieldEmitterComponent carrier in entity-id order and QUEUE its
// (emitter_id, cell, amount) deposits into the EnergyFieldState; the layer's
// own Tick re-sorts by (cell, channel, emitter_id) and applies with
// saturation (/ ordering law), so neither registry iteration
// order nor gather order can ever reach the field bytes.
//
// DETERMINISTIC: id-ordered traversal (sort by entt::to_integral — the
// EcologyHash rule); world -> cell quantization is std::floor over the shared
// 24 m grid identity (kAetherCellSizeM), byte-matching GameSession's anchor
// quantization; the falloff is integer shift math only (no libm, no RNG).
// Participant-gated: a world with no emitter component queues nothing — the
// canonical roster runs this as a pure no-op.

#include "RegionConsumerGate.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "entt/entt.hpp"

#include "../components/CoreComponents.h"
#include "../components/FieldEmitterComponents.h"
#include "../fields/EnergyFieldState.h"
#include "AetherFieldSystem.h" // kAetherCellSizeM — the shared 24 m grid identity

namespace Luminumbra::Systems {

struct FieldEmitterGatherStats {
    std::uint64_t emitters = 0; // carriers that queued at least one deposit
    std::uint64_t deposits = 0; // QueueDeposit calls issued this tick
};

struct RegionalEmitterGatherStats {
    FieldEmitterGatherStats queued;
    std::uint64_t invalid_emitters = 0;
    bool accepted = true;
};

template<typename EntityGate, typename CellGate, typename EntityWork>
inline RegionalEmitterGatherStats
GatherRegionalFieldEmitterDeposits(entt::registry& registry,
                                   luminumbra::fields::EnergyFieldState& field,
                                   const EntityGate& gate,
                                   const CellGate& cell_gate,
                                   const EntityWork& completed,
                                   bool persistent_identity);

// Queue this tick's emitter deposits into `field` (call right before the
// layer's Tick, which sorts and applies them). Centre cell receives the full
// rate; when radius_cells > 0 every cell within that Chebyshev radius receives
// rate >> (Chebyshev distance) — deterministic integer halving per ring, with
// zero-amount rings skipped (a deposit of 0 is not queued). Emitters with
// rate 0 are inert and skipped outright.
inline FieldEmitterGatherStats
GatherFieldEmitterDeposits(entt::registry& registry,
                           luminumbra::fields::EnergyFieldState& field,
                           const Luminumbra::world::RegionEntityGate& gate = {},
                           const Luminumbra::world::RegionCellGate& cell_gate = {},
                           const Luminumbra::world::RegionEntityWork& completed = {}) {
    using Luminumbra::Components::FieldEmitterComponent;
    using Luminumbra::Components::TransformComponent;

    if (gate) {
        const auto gathered = GatherRegionalFieldEmitterDeposits(
            registry,
            field,
            [&](auto entity) { return gate(entity); },
            [&](int x, int z) { return !cell_gate || cell_gate(x, z); },
            [&](auto entity) {
                if (completed)
                    completed(entity);
            },
            true);
        if (!gathered.accepted)
            throw std::runtime_error("Creature field deposit admission refused");
        return gathered.queued;
    }

    FieldEmitterGatherStats stats;

    // id-ordered traversal (the EcologyHash rule) so the queue order is a pure
    // function of sim state, never of entt storage layout.
    std::vector<entt::entity> emitters;
    {
        auto view = registry.view<const TransformComponent, const FieldEmitterComponent>();
        for (auto e : view)
            emitters.push_back(e);
        std::sort(emitters.begin(), emitters.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
    }

    for (auto e : emitters) {
        const auto& emitter = registry.get<const FieldEmitterComponent>(e);
        if (emitter.rate_raw_per_tick == 0u)
            continue; // zero-default = inert

        // World -> cell: floor semantics byte-matching GameSession's anchor
        // quantization (std::floor over the shared 24 m cell size).
        const auto& tf = registry.get<const TransformComponent>(e);
        const int cx = static_cast<int>(std::floor(tf.position.x / kAetherCellSizeM));
        const int cz = static_cast<int>(std::floor(tf.position.z / kAetherCellSizeM));

        const auto emitter_id = static_cast<std::uint64_t>(entt::to_integral(e));
        const int radius = emitter.radius_cells > 0 ? emitter.radius_cells : 0;
        bool queued = false;
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int cheb = std::max(dx < 0 ? -dx : dx, dz < 0 ? -dz : dz);
                // Integer ring falloff; guard the shift (uint32 >> 32+ is UB)
                // every ring at distance >= 32 is zero anyway.
                const std::uint32_t amount =
                    (cheb < 32) ? (emitter.rate_raw_per_tick >> static_cast<unsigned>(cheb)) : 0u;
                if (amount == 0u || (cell_gate && !cell_gate(cx + dx, cz + dz)))
                    continue;
                field.QueueDeposit(emitter_id, cx + dx, cz + dz, emitter.channel, amount);
                ++stats.deposits;
                queued = true;
            }
        }
        if (queued) {
            if (completed)
                completed(e);
            ++stats.emitters;
        }
    }
    return stats;
}

// Enabled policy: skip invalid emitters in the selected identity order and report their count.
// Radius is restricted to the 32 nonzero uint32 falloff rings (0..31).
// Check world bounds BEFORE floor/cast, and the whole footprint before gathering.
inline bool RegionalEmitterCell(const Components::TransformComponent& transform,
                                const Components::FieldEmitterComponent& emitter,
                                int channels,
                                int& cx,
                                int& cz) {
    using luminumbra::fields::EnergyFieldState;
    using world::ActiveRegionLedger;
    if (!ActiveRegionLedger::valid_anchor(transform.position) || emitter.radius_cells < 0 ||
        emitter.radius_cells > 31 || emitter.channel < 0 || emitter.channel >= channels)
        return false;
    cx = static_cast<int>(std::floor(transform.position.x / kAetherCellSizeM));
    cz = static_cast<int>(std::floor(transform.position.z / kAetherCellSizeM));
    for (const int offset : {-emitter.radius_cells, emitter.radius_cells}) {
        const auto owner = EnergyFieldState::OwnerOfCell(cx + offset, cz + offset);
        if (owner.x < -32768 || owner.x >= 32768 || owner.z < -32768 || owner.z >= 32768)
            return false;
    }
    return true;
}

template<typename CellGate>
inline bool
AppendRegionalEmitter(std::vector<luminumbra::fields::EnergyFieldState::RegionalDeposit>& deposits,
                      std::uint64_t emitter_id,
                      const Components::FieldEmitterComponent& emitter,
                      int cx,
                      int cz,
                      const CellGate& cell_gate,
                      std::size_t& candidates) {
    using luminumbra::fields::EnergyFieldState;
    const int radius = emitter.radius_cells;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int distance = std::max(std::abs(dx), std::abs(dz));
            const auto amount = emitter.rate_raw_per_tick >> static_cast<unsigned>(distance);
            if (amount == 0)
                continue;
            // Bound the complete source footprints before recipient filtering, so
            // sealed cells cannot hide an oversized batch. Storage has the same cap.
            if (candidates == EnergyFieldState::kMaxRegionalDeposits)
                return false;
            ++candidates;
            if (cell_gate(cx + dx, cz + dz))
                deposits.push_back({emitter_id, cx + dx, cz + dz, emitter.channel, amount});
        }
    }
    return true;
}

// A capacity refusal drops the ENTIRE new batch, preserving previous input and
// field bytes. The caller still ticks that input, so a bad/oversized roster
// cannot poison future ticks. Empty legacy admission retains the original recipe.
// Concrete predicates keep the regional session path free of type-erased calls.
template<typename EntityGate, typename CellGate, typename EntityWork>
inline RegionalEmitterGatherStats
GatherRegionalFieldEmitterDeposits(entt::registry& registry,
                                   luminumbra::fields::EnergyFieldState& field,
                                   const EntityGate& gate,
                                   const CellGate& cell_gate,
                                   const EntityWork& completed,
                                   bool persistent_identity) {
    RegionalEmitterGatherStats stats;
    std::vector<entt::entity> emitters;
    for (auto entity :
         registry
             .view<const Components::TransformComponent, const Components::FieldEmitterComponent>())
        if (gate(entity))
            emitters.push_back(entity);
    std::sort(emitters.begin(), emitters.end(), [&](entt::entity a, entt::entity b) {
        return world::RegionEntityIdentity(registry, a, persistent_identity) <
               world::RegionEntityIdentity(registry, b, persistent_identity);
    });
    std::vector<luminumbra::fields::EnergyFieldState::RegionalDeposit> deposits;
    FieldEmitterGatherStats queued;
    std::size_t candidates = 0, queued_emitters = 0;
    for (auto entity : emitters) {
        const auto& emitter = registry.get<const Components::FieldEmitterComponent>(entity);
        if (emitter.rate_raw_per_tick == 0)
            continue;
        int cx = 0, cz = 0;
        if (!RegionalEmitterCell(registry.get<const Components::TransformComponent>(entity),
                                 emitter,
                                 field.channels(),
                                 cx,
                                 cz)) {
            ++stats.invalid_emitters;
            continue;
        }
        const auto before = deposits.size();
        if (!AppendRegionalEmitter(
                deposits,
                world::RegionEntityIdentity(registry, entity, persistent_identity),
                emitter,
                cx,
                cz,
                cell_gate,
                candidates)) {
            stats.accepted = false;
            return stats;
        }
        if (deposits.size() != before)
            emitters[queued_emitters++] = entity;
    }
    stats.accepted = field.QueueRegionalDeposits(deposits);
    if (stats.accepted) {
        queued.emitters = static_cast<std::uint64_t>(queued_emitters);
        queued.deposits = static_cast<std::uint64_t>(deposits.size());
        stats.queued = queued;
        // Reuse the source roster for completion; refused batches charge no work.
        for (std::size_t i = 0; i < queued_emitters; ++i)
            completed(emitters[i]);
    }
    return stats;
}

inline RegionalEmitterGatherStats
GatherRegionalFieldEmitterDeposits(entt::registry& registry,
                                   luminumbra::fields::EnergyFieldState& field) {
    return GatherRegionalFieldEmitterDeposits(
        registry,
        field,
        [](entt::entity) { return true; },
        [](int, int) { return true; },
        [](entt::entity) {},
        false);
}

} // namespace Luminumbra::Systems
