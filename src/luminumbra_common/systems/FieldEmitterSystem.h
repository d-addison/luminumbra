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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "entt/entt.hpp"

#include "../ai/CreatureTickAdmission.h"
#include "../components/CoreComponents.h"
#include "../components/FieldEmitterComponents.h"
#include "../fields/EnergyFieldState.h"
#include "AetherFieldSystem.h" // kAetherCellSizeM — the shared 24 m grid identity

namespace Luminumbra::Systems {

struct FieldEmitterGatherStats {
    std::uint64_t emitters = 0; // carriers that queued at least one deposit
    std::uint64_t deposits = 0; // QueueDeposit calls issued this tick
};

// Registry-owned scratch follows the existing exclusive owner-thread gather contract.
// The ordinary roster is bounded by live entities; regional buffers by kMaxRegionalBatchDeposits.
struct FieldEmitterScratch {
    std::vector<entt::entity> emitters, regional_emitters;
    std::vector<luminumbra::fields::EnergyFieldState::RegionalDeposit> deposits;
};

inline FieldEmitterScratch& EmitterScratch(entt::registry& registry) {
    auto* scratch = registry.ctx().find<FieldEmitterScratch>();
    return scratch ? *scratch : registry.ctx().emplace<FieldEmitterScratch>();
}

// Queue this tick's emitter deposits into `field` (call right before the
// layer's Tick, which sorts and applies them). Centre cell receives the full
// rate; when radius_cells > 0 every cell within that Chebyshev radius receives
// rate >> (Chebyshev distance) — deterministic integer halving per ring, with
// zero-amount rings skipped (a deposit of 0 is not queued). Emitters with
// rate 0 are inert and skipped outright.
inline FieldEmitterGatherStats
GatherFieldEmitterDeposits(entt::registry& registry,
                           luminumbra::fields::EnergyFieldState& field,
                           bool clip_to_world = false,
                           const luminumbra::ai::CreatureTickAdmission* admission = nullptr) {
    using Luminumbra::Components::FieldEmitterComponent;
    using Luminumbra::Components::TransformComponent;

    FieldEmitterGatherStats stats;

    // id-ordered traversal (the EcologyHash rule) so the queue order is a pure
    // function of sim state, never of entt storage layout.
    auto& emitters = EmitterScratch(registry).emitters;
    emitters.clear();
    {
        auto view = registry.view<const TransformComponent, const FieldEmitterComponent>();
        for (auto e : view)
            emitters.push_back(e);
        std::sort(emitters.begin(), emitters.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
    }

    for (auto e : emitters) {
        if (admission && !admission->Allows(e)) {
            continue;
        }
        const auto& emitter = registry.get<const FieldEmitterComponent>(e);
        if (emitter.rate_raw_per_tick == 0u)
            continue; // zero-default = inert

        // World -> cell: floor semantics byte-matching GameSession's anchor
        // quantization (std::floor over the shared 24 m cell size).
        const auto& tf = registry.get<const TransformComponent>(e);
        const int cx = static_cast<int>(std::floor(tf.position.x / kAetherCellSizeM));
        const int cz = static_cast<int>(std::floor(tf.position.z / kAetherCellSizeM));

        // Admitted deposits are additive integer input. A neutral key lets the field
        // sort by cell/channel/amount without leaking runtime handles into ordering.
        const auto emitter_id = admission ? 0u : static_cast<std::uint64_t>(entt::to_integral(e));
        const int radius = emitter.radius_cells > 0 ? emitter.radius_cells : 0;
        bool queued = false;
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int cheb = std::max(dx < 0 ? -dx : dx, dz < 0 ? -dz : dz);
                // Integer ring falloff; guard the shift (uint32 >> 32+ is UB)
                // every ring at distance >= 32 is zero anyway.
                const std::uint32_t amount =
                    (cheb < 32) ? (emitter.rate_raw_per_tick >> static_cast<unsigned>(cheb)) : 0u;
                if (amount == 0u)
                    continue;
                if (clip_to_world) {
                    const auto owner =
                        luminumbra::fields::EnergyFieldState::OwnerOfCell(cx + dx, cz + dz);
                    if (owner.x < -32768 || owner.x >= 32768 || owner.z < -32768 ||
                        owner.z >= 32768)
                        continue;
                }
                field.QueueDeposit(emitter_id, cx + dx, cz + dz, emitter.channel, amount);
                ++stats.deposits;
                queued = true;
            }
        }
        if (queued)
            ++stats.emitters;
    }
    return stats;
}

struct RegionalEmitterGatherStats {
    FieldEmitterGatherStats queued;
    std::uint64_t visited_emitters = 0; // Component-view visits, including inert carriers.
    bool accepted = false;
};

inline bool AppendRegionalEmitterFootprint(
    std::vector<luminumbra::fields::EnergyFieldState::RegionalDeposit>& deposits,
    entt::entity entity,
    const Components::TransformComponent& transform,
    const Components::FieldEmitterComponent& emitter,
    int channels,
    const luminumbra::fields::EnergyFieldState::RegionRecords& records,
    bool clip_to_owners,
    const luminumbra::ai::CreatureTickAdmission* admission = nullptr) {
    if (admission && !admission->Allows(entity)) {
        return true;
    }
    using luminumbra::fields::EnergyFieldState;
    if (!world::ActiveRegionLedger::valid_anchor(transform.position) || emitter.radius_cells < 0 ||
        emitter.radius_cells > 31 || emitter.channel < 0 || emitter.channel >= channels)
        return false;
    const int cx = static_cast<int>(std::floor(transform.position.x / kAetherCellSizeM));
    const int cz = static_cast<int>(std::floor(transform.position.z / kAetherCellSizeM));
    // Checked anchors leave room for all offsets. At most 63x63 cells are visited;
    // all larger uint32 falloff rings would be zero.
    const int radius = emitter.radius_cells;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const auto distance = static_cast<unsigned>((std::max)(std::abs(dx), std::abs(dz)));
            const auto amount = emitter.rate_raw_per_tick >> distance;
            if (amount == 0)
                continue;
            if (clip_to_owners) {
                const auto owner = EnergyFieldState::OwnerOfCell(cx + dx, cz + dz);
                const auto found = records.find(owner);
                if (owner.x < -32768 || owner.x >= 32768 || owner.z < -32768 || owner.z >= 32768 ||
                    found == records.end() || found->second.state == world::RegionState::Frozen)
                    continue;
            }
            if (deposits.size() >= EnergyFieldState::kMaxRegionalBatchDeposits)
                return false;
            deposits.push_back(
                {admission ? 0u : static_cast<std::uint64_t>(entt::to_integral(entity)),
                 cx + dx,
                 cz + dz,
                 emitter.channel,
                 amount});
        }
    }
    return true;
}

// One bounded roster batch; a refusal leaves all prior pending input intact.
// Session clipping selects the in-world, nonfrozen ledger footprint first,
// preserving the session's existing sealed halo behavior. Direct calls are strict.
inline RegionalEmitterGatherStats GatherRegionalFieldEmitterDeposits(
    entt::registry& registry,
    luminumbra::fields::EnergyFieldState& field,
    const luminumbra::fields::EnergyFieldState::RegionRecords& records,
    bool clip_to_owners = false,
    std::size_t available_slots = luminumbra::fields::EnergyFieldState::kMaxRegionalDeposits,
    const luminumbra::ai::CreatureTickAdmission* admission = nullptr) {
    using luminumbra::fields::EnergyFieldState;
    RegionalEmitterGatherStats stats;
    // Both scratch vectors are bounded by kMaxRegionalBatchDeposits, including
    // a roster of emitters whose footprints eventually clip to nothing. They grow
    // with the roster, so an ordinary tick with a few emitters allocates little.
    auto& scratch = EmitterScratch(registry);
    auto& emitters = scratch.regional_emitters;
    auto& deposits = scratch.deposits;
    emitters.clear();
    deposits.clear();
    const auto view =
        registry
            .view<const Components::TransformComponent, const Components::FieldEmitterComponent>();
    emitters.reserve((std::min)(view.size_hint(), EnergyFieldState::kMaxRegionalBatchDeposits));
    for (auto entity : view) {
        ++stats.visited_emitters;
        if (admission && !admission->Allows(entity)) {
            continue;
        }
        if (registry.get<const Components::FieldEmitterComponent>(entity).rate_raw_per_tick == 0)
            continue;
        if (emitters.size() == EnergyFieldState::kMaxRegionalBatchDeposits)
            return stats;
        emitters.push_back(entity);
    }
    std::sort(emitters.begin(), emitters.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    FieldEmitterGatherStats queued;
    for (auto entity : emitters) {
        const auto previous = deposits.size();
        if (!AppendRegionalEmitterFootprint(
                deposits,
                entity,
                registry.get<const Components::TransformComponent>(entity),
                registry.get<const Components::FieldEmitterComponent>(entity),
                field.channels(),
                records,
                clip_to_owners,
                admission))
            return stats;
        queued.emitters += deposits.size() != previous ? 1u : 0u;
    }
    // The session stages emitter input separately from already queued live input.
    if (deposits.size() > available_slots)
        return stats;
    stats.accepted = field.QueueRegionalDeposits(deposits, records);
    if (stats.accepted) {
        queued.deposits = static_cast<std::uint64_t>(deposits.size());
        stats.queued = queued;
    }
    return stats;
}

} // namespace Luminumbra::Systems
