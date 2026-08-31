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
#include <cstdint>
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

// Queue this tick's emitter deposits into `field` (call right before the
// layer's Tick, which sorts and applies them). Centre cell receives the full
// rate; when radius_cells > 0 every cell within that Chebyshev radius receives
// rate >> (Chebyshev distance) — deterministic integer halving per ring, with
// zero-amount rings skipped (a deposit of 0 is not queued). Emitters with
// rate 0 are inert and skipped outright.
inline FieldEmitterGatherStats
GatherFieldEmitterDeposits(entt::registry& registry, luminumbra::fields::EnergyFieldState& field) {
    using Luminumbra::Components::FieldEmitterComponent;
    using Luminumbra::Components::TransformComponent;

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
                if (amount == 0u)
                    continue;
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

} // namespace Luminumbra::Systems
