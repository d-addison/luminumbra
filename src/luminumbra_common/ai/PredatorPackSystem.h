#pragma once

// sim.predator_pack: EMERGENT PACK HUNTING via FLANKING COORDINATION.
//
// Predators don't just all charge the same point (which lets prey bolt out the open side);
// when several hunt close together they SURROUND the quarry. This system maintains a
// per-predator coordination "wish" over every predator carrying a PackHunterComponent:
//   * a predator is a PACK-MATE this tick if at least one OTHER opted-in predator is within
//     kPackRadius of it (so a lone hunter, or a hunter whose nearest ally is too far, hunts
//     solo). Pack membership is per-predator (its allies-in-range), not a global cluster.
//   * a pack hunting the SAME prey spreads its members around that prey: the shared target is
//     the nearest live prey to the PACK CENTROID (the mean position of this predator's pack —
//     so allies converge on one quarry rather than scattering), and each member is assigned a
//     FLANK approach angle by its RANK within the id-sorted pack, spread evenly around the
//     prey. The wish points at the member's flank approach POINT (prey + offset on its angle),
//     so the members arrive from different sides and ENCIRCLE the prey (a pincer).
//   * a lone predator (no pack-mate in range) gets in_pack=0 and a DIRECT wish straight at its
//     nearest prey (ordinary pursuit).
//   * dead prey (CreatureComponent.eaten) are not targets; a carcass-predator (eaten predator)
//     and a predator with no live prey in the world get a zero wish.
//
// SteeringConsumer blends coord_x/coord_z into the predator's movement after
// this system computes the coordination field.
//
// DETERMINISM. id-ordered traversal (sort by entt::to_integral). TWO-PHASE so the result is
// order-INDEPENDENT:  SNAPSHOTS every predator's position + every live prey's position;
//  computes each predator's wish from that frozen snapshot (its pack is its allies in
// range from the snapshot; its flank rank is its index among the id-sorted pack). Because all
// reads are from the snapshot, no predator's update can see another's in-this-tick write, so
// iteration order cannot change the outcome. Math is DeterministicMath only (Cos/Sin for the
// flank angle, Sqrt for distance; +-*/); NO rng is consumed (the flank assignment is a pure
// function of the id-sorted pack index — seed offset +31 is reserved for collision-avoidance
// only and never drawn). NO wall-clock, NO libm transcendentals. run==replay holds.
//
// GATING. A predator only participates if it carries a PackHunterComponent (the opt-in). A
// world whose creatures carry none — and any world with no creatures — does nothing, so the
// canonical NetworkStateHash baseline stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/PackHunterComponents.h"
#include "../core/DeterministicMath.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry: ... herd-alarm+26, predator-pack+31). This system is
// deterministic and consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kPredatorPackSeedOffset = 31ull;

// Distance (world units) within which two opted-in predators count as pack-mates.
inline constexpr float kPackRadius = 20.0f;

// How far OUT from the prey each pack-mate's flank approach point sits (world units). Members
// aim for a ring of this radius around the prey at their assigned angle so they encircle it
// rather than all piling onto the exact same point. Small enough that the wish still pulls
// them toward the prey overall.
inline constexpr float kFlankStandoff = 4.0f;

struct PredatorPackStats {
    int participants = 0; // predators carrying a PackHunterComponent that took part
    int packed = 0;       // predators that found at least one pack-mate in range (in_pack=1)
    int hunting = 0;      // predators that got a non-zero wish (had a live prey target)
};

// RunPredatorPackOnTick: maintain the pack-hunting coordination field. id-ordered, two-phase
// snapshot. Returns per-tick stats (telemetry / test hooks).
inline PredatorPackStats RunPredatorPackOnTick(entt::registry& reg, std::uint64_t tick) {
    (void)tick; // no rng / time dependence; kept for signature parity with sibling systems.
    PredatorPackStats stats;

    // Predators that opted in (carry a PackHunterComponent).
    auto predView =
        reg.view<Comp::PackHunterComponent, Comp::CreatureComponent, Comp::TransformComponent>();

    // id-ordered participant list (deterministic traversal + stable flank ranks).
    std::vector<entt::entity> preds(predView.begin(), predView.end());
    std::sort(preds.begin(), preds.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (preds.empty())
        return stats; // empty roster -> pure no-op.

    // ---- First pass: snapshot predator state and live prey positions. ----
    struct PredSnap {
        entt::entity e;
        float x, z;
        bool predator; // only true predators coordinate (a non-predator carrying the tag is inert)
        bool dead;     // a carcass predator produces no wish
    };
    std::vector<PredSnap> ps;
    ps.reserve(preds.size());
    for (auto e : preds) {
        const auto& cr = predView.get<Comp::CreatureComponent>(e);
        const auto& tf = predView.get<Comp::TransformComponent>(e);
        ps.push_back({e, tf.position.x, tf.position.z, cr.is_predator, cr.eaten});
    }
    stats.participants = static_cast<int>(ps.size());

    // Live PREY positions: any creature that is NOT a predator and NOT eaten. Prey need not
    // carry a PackHunterComponent, so read the full creature roster.
    struct PreySnap {
        float x, z;
    };
    std::vector<PreySnap> prey;
    {
        auto preyView = reg.view<Comp::CreatureComponent, Comp::TransformComponent>();
        for (auto e : preyView) {
            const auto& cr = preyView.get<Comp::CreatureComponent>(e);
            if (cr.is_predator || cr.eaten)
                continue;
            const auto& tf = preyView.get<Comp::TransformComponent>(e);
            prey.push_back({tf.position.x, tf.position.z});
        }
    }

    // ---- Second pass: compute coordination wishes from the frozen snapshot. ----
    for (std::size_t i = 0; i < ps.size(); ++i) {
        const PredSnap& self = ps[i];
        auto& pk = predView.get<Comp::PackHunterComponent>(self.e);

        // An inert participant (non-predator tag holder) or a carcass: no coordination.
        if (!self.predator || self.dead) {
            pk.in_pack = 0;
            pk.coord_x = 0.0f;
            pk.coord_z = 0.0f;
            continue;
        }

        // Build this predator's PACK = live predator pack-mates within kPackRadius (including
        // self). Members are kept in id order (ps is id-sorted) so flank ranks are stable.
        // Track this predator's RANK (index within its own pack member list) for the angle.
        std::vector<std::size_t> pack; // indices into ps
        std::size_t selfRank = 0;
        float sumX = 0.0f, sumZ = 0.0f;
        for (std::size_t j = 0; j < ps.size(); ++j) {
            const PredSnap& o = ps[j];
            if (!o.predator || o.dead)
                continue;
            if (j == i) {
                selfRank = pack.size();
                pack.push_back(j);
                sumX += o.x;
                sumZ += o.z;
                continue;
            }
            const float dx = o.x - self.x;
            const float dz = o.z - self.z;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (d <= kPackRadius) {
                pack.push_back(j);
                sumX += o.x;
                sumZ += o.z;
            }
        }

        // Order-INDEPENDENT flank rank: rank pack-mates by POSITION (x then z), not by entity
        // id, so a predator's flank angle depends on WHERE it is — invariant to spawn order.
        // (ps is id-sorted, so the index `j` is the id; use it as a stable positional tiebreak.)
        std::sort(pack.begin(), pack.end(), [&](std::size_t a, std::size_t b) {
            if (ps[a].x != ps[b].x)
                return ps[a].x < ps[b].x;
            if (ps[a].z != ps[b].z)
                return ps[a].z < ps[b].z;
            return a < b;
        });
        for (std::size_t k = 0; k < pack.size(); ++k) {
            if (pack[k] == i) {
                selfRank = k;
                break;
            }
        }

        const std::size_t packSize = pack.size(); // >= 1 (self always included)
        const bool inPack = packSize >= 2;        // a true pack needs a mate in range
        pk.in_pack = inPack ? 1u : 0u;
        if (inPack)
            ++stats.packed;

        // Shared target = nearest LIVE prey to the PACK CENTROID (so all members converge on
        // ONE quarry). For a lone predator the centroid is just its own position, so this is
        // its own nearest prey (ordinary pursuit).
        if (prey.empty()) {
            pk.coord_x = 0.0f;
            pk.coord_z = 0.0f;
            continue;
        }
        const float invN = 1.0f / static_cast<float>(packSize);
        const float cx = sumX * invN;
        const float cz = sumZ * invN;

        float bestD2 = 0.0f;
        float preyX = prey[0].x, preyZ = prey[0].z;
        bool first = true;
        for (const PreySnap& q : prey) {
            const float dx = q.x - cx;
            const float dz = q.z - cz;
            const float d2 = dx * dx + dz * dz;
            if (first || d2 < bestD2) {
                bestD2 = d2;
                preyX = q.x;
                preyZ = q.z;
                first = false;
            }
        }

        float targetX, targetZ;
        if (inPack) {
            // FLANK: assign this member a slice of the circle around the prey by its rank, so
            // members approach from DIFFERENT sides and encircle the quarry. The base angle is
            // anchored on the line from the pack centroid to the prey (atan2(prey - centroid))
            // so the spread is oriented relative to the approach, then offset evenly by rank.
            const float baseAngle = dm::Atan2(preyZ - cz, preyX - cx);
            const float step = dm::kTwoPi / static_cast<float>(packSize);
            const float angle = baseAngle + step * static_cast<float>(selfRank);
            // Approach POINT = a standoff ring position around the prey at this member's angle.
            targetX = preyX + kFlankStandoff * dm::Cos(angle);
            targetZ = preyZ + kFlankStandoff * dm::Sin(angle);
        } else {
            // Lone predator: aim straight at the prey (direct pursuit).
            targetX = preyX;
            targetZ = preyZ;
        }

        // Wish = unit-ish vector from this predator toward its (flank or direct) target.
        const float dx = targetX - self.x;
        const float dz = targetZ - self.z;
        const float d = dm::Sqrt(dx * dx + dz * dz);
        if (d > 1.0e-5f) {
            const float inv = 1.0f / d;
            pk.coord_x = dx * inv;
            pk.coord_z = dz * inv;
            ++stats.hunting;
        } else {
            // Already on top of the target point: no steer this tick.
            pk.coord_x = 0.0f;
            pk.coord_z = 0.0f;
        }
    }

    return stats;
}

} // namespace luminumbra::ai
