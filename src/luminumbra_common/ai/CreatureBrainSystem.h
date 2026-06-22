#pragma once

// I9-ECO: the live creature tick — reads each creature's senses, decides via the Utility-AI
// brain (CreatureBrain.h -> IAUS), and MOVES the creature deterministically (flee = away from
// the nearest predator, hunt = toward the nearest prey, wander = golden-angle drift, graze/rest
// = recover). This is "wire Utility AI into the creature tick" in practice.
//
// DETERMINISM (this changes sim state -> world_hash): id-ordered traversal; senses use a
// PRE-tick position snapshot so the result is independent of move order; libm-free
// (DeterministicMath Sin/Cos/Sqrt only). A world with no CreatureComponent runs it as a no-op,
// so the canonical roster is byte-identical (the component is the opt-in, like PlantTag).

#include "CreatureBrain.h"
#include "Flocking.h"
#include "SpatialGrid.h"

#include "../components/AlarmComponents.h"
#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../core/DeterministicMath.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

struct CreatureBrainStats {
    int updated = 0;
};

// How strongly herd flocking biases the action heading (unit-scale; the action dir is also
// unit, so this is a fractional blend that keeps flee/hunt dominant).
inline constexpr float kHerdWeight = 0.8f;
// How strongly herd flocking matches the group's mean heading (Reynolds alignment, the 3rd term).
// 0 = OFF, which keeps the steer byte-identical to the cohesion+separation result (canonical roster
// + 1v1 tests stay exact). Tune > 0 to make a herd converge to a common heading.
inline constexpr float kAlignmentWeight = 0.0f;
// Predator catch reach (m) and how much catching a prey sates the predator's hunger.
inline constexpr float kCatchRadius = 2.2f;
inline constexpr float kCatchSatiation = 0.8f;

// Advance every CreatureComponent by one fixed tick. Pure function of registry state + dt.
inline CreatureBrainStats RunCreatureBrainSystemOnTick(entt::registry& reg, float dt) {
    CreatureBrainStats stats;
    auto view = reg.view<Comp::CreatureComponent, Comp::TransformComponent>();

    std::vector<entt::entity> ents;
    for (auto e : view) ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    // Pre-tick snapshot so all creatures sense the SAME state regardless of update order.
    struct Snap {
        entt::entity e;
        float x, z;
        float hx, hz;  // prior-tick heading (wish velocity) for the Reynolds alignment term
        bool predator;
        bool eaten;
    };
    std::vector<Snap> snap;
    snap.reserve(ents.size());
    for (auto e : ents) {
        const auto& tf = view.get<Comp::TransformComponent>(e);
        const auto& c = view.get<Comp::CreatureComponent>(e);
        snap.push_back({e, tf.position.x, tf.position.z, c.wish_x, c.wish_z, c.is_predator, c.eaten});
    }

    // Herd-flocking acceleration: bucket the snapshot into a per-role uniform spatial grid
    // ONCE per tick (cell size == flocking neighbour radius), so each creature gathers its
    // same-role herd via a 3x3-cell radius query (O(N+k)) instead of scanning the whole
    // snapshot (the former O(N^2) herd gather). Determinism is unchanged: the grid only
    // narrows WHICH same-role neighbours are visited and in what order, and the downstream
    // Flocking accumulation is order-invariant (fixed-point). The herd membership matches the
    // old full scan EXACTLY (same role, self excluded later via index, eaten same-role bodies
    // still counted), so the gathered SET — and the steer — is byte-identical.
    UniformSpatialGrid predGrid(FlockParams{}.neighbor_radius);
    UniformSpatialGrid preyGrid(FlockParams{}.neighbor_radius);
    {
        std::vector<GridPoint> predPts, preyPts;
        predPts.reserve(snap.size());
        preyPts.reserve(snap.size());
        for (std::uint32_t i = 0; i < snap.size(); ++i) {
            const Snap& o = snap[i];
            (o.predator ? predPts : preyPts).push_back({i, o.x, o.z});
        }
        predGrid.Build(predPts);
        preyGrid.Build(preyPts);
    }
    std::vector<std::uint32_t> herdHits;  // reused query scratch buffer across creatures

    for (std::size_t selfIdx = 0; selfIdx < ents.size(); ++selfIdx) {
        const entt::entity e = ents[selfIdx];
        auto& tf = view.get<Comp::TransformComponent>(e);
        auto& cr = view.get<Comp::CreatureComponent>(e);

        // A caught carcass is inert: it neither decides nor moves (physics still grounds it).
        if (cr.eaten) {
            cr.wish_x = 0.0f;
            cr.wish_z = 0.0f;
            ++stats.updated;
            continue;
        }
        const float sx = tf.position.x, sz = tf.position.z;

        // FR-5 emergent PERCEPTION: a creature carrying a sensory genome only TARGETS the opposite-
        // role creatures it can actually SENSE — hearing is omnidirectional within hearing_range;
        // vision is a gene-width cone (vision_cos_half_fov) within vision_range, faced along the
        // prior-tick heading. Heritable + mutable sensory genes are thus SELECTED by survival (a
        // creature that cannot sense a threat/food misses it). Genome-less creatures keep the
        // unfiltered global-nearest scan (byte-identical to the pre-FR-5 brain; empty-roster worlds
        // run the whole brain as a no-op, so canonical/networked hashes are unaffected).
        const Comp::CreatureGenomeComponent* sg = reg.try_get<Comp::CreatureGenomeComponent>(e);
        float faceX = 0.0f, faceZ = 0.0f;
        if (sg != nullptr) {
            const float hh = dm::Sqrt(snap[selfIdx].hx * snap[selfIdx].hx + snap[selfIdx].hz * snap[selfIdx].hz);
            if (hh > 1e-4f) { faceX = snap[selfIdx].hx / hh; faceZ = snap[selfIdx].hz / hh; }
        }

        // Nearest LIVE opposite-role creature: prey -> nearest predator (threat); predator ->
        // nearest live prey (food). Carcasses are skipped so a predator moves on to live prey.
        float bestDist = 1.0e9f, tx = sx, tz = sz;
        bool found = false;
        entt::entity te = entt::null;
        for (const Snap& o : snap) {
            if (o.e == e || o.predator == cr.is_predator || o.eaten) continue;
            const float dx = o.x - sx, dz = o.z - sz;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (sg != nullptr) {
                // Perceivable? hearing (omnidirectional within range) OR vision (cone within range).
                bool sensed = (d <= sg->hearing_range);
                if (!sensed && d <= sg->vision_range) {
                    if (faceX == 0.0f && faceZ == 0.0f) {
                        sensed = true;  // no prior heading (stationary) -> vision omnidirectional in range
                    } else if (d > 1e-4f) {
                        sensed = ((dx * faceX + dz * faceZ) / d) >= sg->vision_cos_half_fov;
                    } else {
                        sensed = true;  // target coincident with self
                    }
                }
                if (!sensed) continue;
            }
            if (d < bestDist) {
                bestDist = d;
                tx = o.x;
                tz = o.z;
                te = o.e;
                found = true;
            }
        }

        // Catch: a predator within reach of its nearest live prey EATS it -- the prey becomes
        // a carcass and the predator's hunger is sated. Marking is idempotent + id-ordered.
        // The target was chosen from the PRE-tick snapshot, so an earlier (lower-id) predator
        // this same tick may have already eaten it -- re-check the LIVE eaten flag so one prey
        // sates at most ONE predator per tick (no feeding from an already-dead carcass).
        if (cr.is_predator && found && bestDist < kCatchRadius && reg.valid(te) &&
            !reg.get<Comp::CreatureComponent>(te).eaten) {
            reg.get<Comp::CreatureComponent>(te).eaten = true;
            cr.hunger = utility_clamp01(cr.hunger - kCatchSatiation);
        }

        CreatureSenses s;
        s.is_predator = cr.is_predator;
        s.hunger = cr.hunger;
        s.stamina = cr.stamina;
        const float nearNorm = found ? (1.0f - utility_clamp01(bestDist / 30.0f)) : 0.0f;
        if (cr.is_predator) {
            s.food_proximity = nearNorm;
        } else {
            s.threat_proximity = nearNorm;
            s.food_proximity = 0.6f;  // prey graze ambient plants
            // Herd alarm (opt-in via AlarmComponent): a prey that directly senses a predator
            // RAISES its alarm; and it flees on either the direct threat OR the propagated herd
            // alarm level (HerdAlarmSystem, slot 7) -- so the whole herd bolts together even if
            // only one saw the predator. Collective vigilance.
            if (auto* al = reg.try_get<Comp::AlarmComponent>(e)) {
                al->alarmed = (nearNorm > 0.4f) ? 1u : 0u;
                if (al->level > s.threat_proximity) s.threat_proximity = al->level;
            }
        }

        const CreatureAction act = DecideCreatureAction(s);
        cr.last_action = static_cast<int>(act);

        float dirx = 0.0f, dirz = 0.0f;
        switch (act) {
            case CreatureAction::Flee:
                if (found) { dirx = sx - tx; dirz = sz - tz; }
                break;
            case CreatureAction::Hunt:
                if (found) { dirx = tx - sx; dirz = tz - sz; }
                break;
            case CreatureAction::Wander: {
                const float ang = 2.39996323f *
                    static_cast<float>(entt::to_integral(e) % 16u + 1u);  // deterministic per-id heading
                dirx = dm::Cos(ang);
                dirz = dm::Sin(ang);
                break;
            }
            case CreatureAction::Graze:
                cr.hunger = utility_clamp01(cr.hunger - 0.5f * dt);  // eating sates
                break;
            case CreatureAction::Rest:
                cr.stamina = utility_clamp01(cr.stamina + 0.3f * dt);  // recover
                break;
        }

        // Normalize the action heading to a unit direction so herd flocking (a unit-scale
        // steer) blends in at a comparable weight.
        float adirx = 0.0f, adirz = 0.0f;
        const float alen = dm::Sqrt(dirx * dirx + dirz * dirz);
        if (alen > 1.0e-5f) {
            adirx = dirx / alen;
            adirz = dirz / alen;
        }

        // Herd flocking: for moving creatures, bias the heading by cohesion/separation over
        // SAME-ROLE neighbours (from the pre-tick snapshot) so prey flee as a coherent herd
        // (and predators can pack) instead of each moving alone. A lone creature has no
        // same-role neighbour -> zero steer -> behaviour unchanged (keeps the 1v1 tests exact).
        if (act == CreatureAction::Flee || act == CreatureAction::Hunt ||
            act == CreatureAction::Wander) {
            // Radius-query the SAME-ROLE grid for the 3x3 cell block around self (cell size ==
            // neighbour radius, so this is a superset of every neighbour within the cohesion
            // radius; ComputeFlockSteer distance-filters internally exactly as the old full
            // scan did). Exclude self by snapshot index. The Flocking accumulation is
            // order-invariant, so the bucket visitation order does not affect the steer.
            herdHits.clear();
            const UniformSpatialGrid& grid = cr.is_predator ? predGrid : preyGrid;
            grid.QueryRadius(sx, sz, herdHits);
            std::vector<std::pair<float, float>> herd;
            std::vector<std::pair<float, float>> herdHeadings;  // index-aligned with `herd`
            herd.reserve(herdHits.size());
            herdHeadings.reserve(herdHits.size());
            for (std::uint32_t hi : herdHits) {
                if (static_cast<std::size_t>(hi) == selfIdx) continue;
                herd.emplace_back(snap[hi].x, snap[hi].z);
                herdHeadings.emplace_back(snap[hi].hx, snap[hi].hz);
            }
            // alignment_weight default 0 (kAlignmentWeight) -> alignment is skipped and the steer is
            // byte-identical to the cohesion+separation result; passing headings has zero effect
            // until the weight is tuned > 0.
            FlockParams fp{};
            fp.alignment_weight = kAlignmentWeight;
            const FlockSteer fs = ComputeFlockSteer(sx, sz, herd, fp, &herdHeadings);
            adirx += fs.x * kHerdWeight;
            adirz += fs.z * kHerdWeight;
        }

        // Resolve the blended heading into a wish VELOCITY (m/s). Flee/hunt sprint at 1.5x.
        const float len = dm::Sqrt(adirx * adirx + adirz * adirz);
        cr.wish_x = 0.0f;
        cr.wish_z = 0.0f;
        if (len > 1.0e-5f) {
            const float inv = 1.0f / len;
            const float speed = (act == CreatureAction::Flee || act == CreatureAction::Hunt)
                                    ? cr.move_speed * 1.5f
                                    : cr.move_speed;
            cr.wish_x = adirx * inv * speed;
            cr.wish_z = adirz * inv * speed;
            cr.stamina = utility_clamp01(cr.stamina - 0.10f * dt);  // moving tires
            // When a Jolt character owns this creature (CreaturePhysicsComponent), it
            // integrates the wish velocity against the terrain (gravity/collision/slopes);
            // the physics bridge reads the resolved position back. Otherwise — the pure,
            // unit-tested path — integrate X/Z directly here (terrain-independent).
            if (!reg.all_of<Comp::CreaturePhysicsComponent>(e)) {
                tf.position.x += cr.wish_x * dt;
                tf.position.z += cr.wish_z * dt;
            }
        }
        cr.hunger = utility_clamp01(cr.hunger + 0.02f * dt);  // hunger grows
        ++stats.updated;
    }
    return stats;
}

}  // namespace luminumbra::ai
