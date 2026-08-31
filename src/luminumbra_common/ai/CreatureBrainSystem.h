#pragma once

//  the live creature tick — reads each creature's senses, decides via the Utility-AI
// brain (CreatureBrain.h -> IAUS), and MOVES the creature deterministically (flee = away from
// the nearest predator, hunt = toward the nearest prey, wander = golden-angle drift, graze/rest
// = recover). This is "wire Utility AI into the creature tick" in practice.
//
// DETERMINISM (this changes sim state -> world_hash): id-ordered traversal; senses use a
// PRE-tick position snapshot so the result is independent of move order; libm-free
// (DeterministicMath Sin/Cos/Sqrt only). A world with no CreatureComponent runs it as a no-op,
// so the canonical roster is byte-identical (the component is the opt-in, like PlantTag).

#include "CreatureBrain.h"
#include "CreatureSpeciesRegistry.h"
#include "Flocking.h"
#include "PerceptionSubstrate.h" // canonical shared neighbor-scan substrate
#include "ScentField.h"          // in-brain scent tracking (GradientSteer)
#include "SpatialGrid.h"

#include "../components/AlarmComponents.h"
#include "../components/CircadianComponents.h"
#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/ThirstComponents.h" // thirst joins the arbiter
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
// DEFAULT 0.5 — the alignment term is ON (just below cohesion 0.6) so a herd converges to a
// common heading (emergent flocking). Setting it to 0 (via EcologyTuning / SystemConfig
// sim.ecology) disables the term and reverts the steer to the pure cohesion+separation result.
// A lone creature has no same-role neighbour -> zero steer -> 1v1 flee/hunt behaviour is
// unchanged either way; the canonical (empty-roster) world runs the brain as a no-op, so the
// default --smoke hash is independent of this weight.
inline constexpr float kAlignmentWeight = 0.5f;
// Predator catch reach (m) and how much catching a prey sates the predator's hunger.
inline constexpr float kCatchRadius = 2.2f;
inline constexpr float kCatchSatiation = 0.8f;
// energy (long-term sleep need). Drains slowly every tick (being awake costs energy);
// Rest recovers it faster than it drains (net positive) and Sleep recovers it fastest. Per-second
// rates (scaled by dt). Energy IS read by DecideCreatureAction (the integrated brain path: the
// Sleep action's "tired" consideration, circadian-gated via CreatureSenses.circadian_activity), so
// it steers the sim trajectory of any sleeping roster. EcologyHash folds the value into the
// authoritative ecology sub-hash.
inline constexpr float kEnergyDrainPerSecond = 0.006f;
inline constexpr float kEnergyRestRecover = 0.080f;
// Sleep recovers energy faster than Rest (deep rest at the nest/off-phase).
inline constexpr float kEnergySleepRecover = 0.160f;
// Per-second need rates that were inline literals (defaults preserved exactly).
inline constexpr float kHungerGrowthPerSecond = 0.02f;   // hunger grows while awake
inline constexpr float kHungerGrazeSatePerSecond = 0.5f; // grazing sates hunger
inline constexpr float kStaminaRestRecover = 0.3f;       // Rest/Sleep recover sprint fuel
inline constexpr float kStaminaMoveDrain = 0.10f;        // moving tires

//  DATA-DRIVEN ecology tuning. Every field defaults to the constant above,
// so a default-constructed EcologyTuning reproduces today's behaviour BYTE-IDENTICALLY (the brain
// tick, the tests, and the canonical gate roster are all unchanged unless a caller overrides it
// from SystemConfig `sim.ecology`). This makes the difficulty/balance levers tunable from data
// (energy/sleep cadence, hunger/stamina pacing, herd cohesion, predator reach) with no recompile.
struct EcologyTuning {
    float energy_drain_per_second = kEnergyDrainPerSecond;
    float energy_rest_recover = kEnergyRestRecover;
    float energy_sleep_recover = kEnergySleepRecover;
    float hunger_growth_per_second = kHungerGrowthPerSecond;
    float hunger_graze_sate = kHungerGrazeSatePerSecond;
    float stamina_rest_recover = kStaminaRestRecover;
    float stamina_move_drain = kStaminaMoveDrain;
    float herd_weight = kHerdWeight;
    float alignment_weight = kAlignmentWeight;
    float catch_radius = kCatchRadius;
    float catch_satiation = kCatchSatiation;
    // Boids/Reynolds flocking geometry (defaults == Flocking.h FlockParams -> byte-identical).
    float flock_neighbor_radius = 12.0f;  // grid cell + cohesion/alignment neighbour radius (m)
    float flock_separation_radius = 3.0f; // separation kicks in below this spacing (m)
    float flock_cohesion_weight = 0.6f;   // pull toward the group centroid
    float flock_separation_weight = 1.4f; // push off crowding
};

// Advance every CreatureComponent by one fixed tick. Pure function of registry state + dt + tuning.
// `tuning` defaults to the compiled constants, so existing callers/tests are byte-identical.
// World XZ to scent-grid cell (floor toward negative infinity; mirrors the
// ScentSteeringSystem::WorldToCell used on the LocomotionIntent path).
inline int WorldToScentCell(float world, float origin, float cell_size) {
    const float v = (world - origin) / cell_size;
    int c = static_cast<int>(v);
    if (v < static_cast<float>(c))
        --c;
    return c;
}

// Optional scent-tracking inputs. When a ScentField is
// supplied, a PREDATOR with no directly-perceived target steers UP the prey-scent
// gradient (channel 0) instead of wandering blind — the vertebrate half of the
// stigmergy substrate on the BRAIN path (the ant/GOAP path uses ScentSteering's
// LocomotionIntent, which ambient creatures never carry). Default nullptr keeps
// every existing call byte-identical.
//
// `use_perception_substrate` defaults true: the shared substrate is the canonical
// path; false selects the retained inline scan
// (the byte-identical regression reference). When false, the per-creature nearest-target scan runs
// the original inline snapshot loop and the tick is byte-identical. When true, that scan routes
// through the SHARED PerceptionField (PerceptionSubstrate.h) — a deterministic, id-ordered
// neighbour scan built once per tick over the opposite-role live-target set — and
// the per-creature considerTarget re-applies the EXACT sensory gate, so the chosen
// target (and thus every downstream decision/movement) is UNCHANGED. The flag exists
// so the shared substrate can be exercised without touching the default sim
// trajectory (proven equivalent by test/ai/creature_brain_substrate_test.cpp).
inline CreatureBrainStats
RunCreatureBrainSystemOnTick(entt::registry& reg,
                             float dt,
                             const EcologyTuning& tuning = {},
                             const ScentField* scent = nullptr,
                             float scent_origin_x = 0.0f,
                             float scent_origin_z = 0.0f,
                             float scent_cell_size = 0.0f,
                             bool use_perception_substrate = true,
                             const CreatureSpeciesRegistry* species = nullptr) {
    CreatureBrainStats stats;
    auto view = reg.view<Comp::CreatureComponent, Comp::TransformComponent>();

    std::vector<entt::entity> ents;
    for (auto e : view)
        ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    // Pre-tick snapshot so all creatures sense the SAME state regardless of update order.
    struct Snap {
        entt::entity e;
        float x, z;
        float hx, hz; // prior-tick heading (wish velocity) for the Reynolds alignment term
        bool predator;
        bool eaten;
    };
    std::vector<Snap> snap;
    snap.reserve(ents.size());
    for (auto e : ents) {
        const auto& tf = view.get<Comp::TransformComponent>(e);
        const auto& c = view.get<Comp::CreatureComponent>(e);
        snap.push_back(
            {e, tf.position.x, tf.position.z, c.wish_x, c.wish_z, c.is_predator, c.eaten});
    }

    // Herd-flocking acceleration: bucket the snapshot into a per-role uniform spatial grid
    // ONCE per tick (cell size == flocking neighbour radius), so each creature gathers its
    // same-role herd via a 3x3-cell radius query (O(N+k)) instead of scanning the whole
    // snapshot (the former O(N^2) herd gather). Determinism is unchanged: the grid only
    // narrows WHICH same-role neighbours are visited and in what order, and the downstream
    // Flocking accumulation is order-invariant (fixed-point). The herd membership matches the
    // old full scan EXACTLY (same role, self excluded later via index, eaten same-role bodies
    // still counted), so the gathered SET — and the steer — is byte-identical.
    UniformSpatialGrid predGrid(tuning.flock_neighbor_radius);
    UniformSpatialGrid preyGrid(tuning.flock_neighbor_radius);
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
    std::vector<std::uint32_t> herdHits; // reused query scratch buffer across creatures

    // When selected, build the shared PerceptionField over the
    // creatures' TARGET set ONCE per tick — one field of live PREDATORS (queried by prey looking
    // for threats) and one of live PREY (queried by predators looking for food). Each source's
    // ordinal is its snapshot index, and `snap` is id-sorted, so a per-creature Query returns the
    // perceived set in ascending index == id order — exactly the order the inline scan visits it,
    // so the first-wins nearest tie-break matches. Carcasses are excluded at Build (the inline
    // scan skips o.eaten); self is auto-excluded (self is same-role, so it never lands in the
    // OPPOSITE-role field it queries). Sources are UNGATED (radius 0): the per-PERCEIVER sense
    // gate (genome hearing/vision cone + starvation shrink; genome-less = unbounded range) is NOT
    // expressible as a per-source radius, so the field returns the full opposite-role candidate
    // set and the per-creature considerTarget re-applies the EXACT inline gate. Built only when
    // the flag is set, so the default path allocates nothing and stays byte-identical.
    PerceptionField predTargetField;   // live predators — queried by PREY
    PerceptionField preyTargetField;   // live prey — queried by PREDATORS
    PerceptionSnapshot targetSnapshot; // reused per-creature query buffer (Query clears it)
    if (use_perception_substrate) {
        std::vector<PerceptionSourceInput> predSources, preySources;
        predSources.reserve(snap.size());
        preySources.reserve(snap.size());
        for (std::uint32_t i = 0; i < snap.size(); ++i) {
            const Snap& o = snap[i];
            if (o.eaten)
                continue; // carcasses are not live targets (inline scan skips o.eaten)
            PerceptionSourceInput in;
            in.index = i; // ordinal == snapshot index (id-sorted) -> id-ordered query
            in.has_position = true;
            in.x = o.x;
            in.z = o.z;       // y stays 0 -> the 3-D metric collapses to the XZ scan's plane
            in.radius = 0.0f; // ungated: considerTarget applies the per-perceiver gate
            (o.predator ? predSources : preySources).push_back(in);
        }
        predTargetField.Build(predSources);
        preyTargetField.Build(preySources);
    }

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

        //  emergent PERCEPTION: a creature carrying a sensory genome only TARGETS the opposite-
        // role creatures it can actually SENSE — hearing is omnidirectional within hearing_range;
        // vision is a gene-width cone (vision_cos_half_fov) within vision_range, faced along the
        // prior-tick heading. Heritable + mutable sensory genes are thus SELECTED by survival (a
        // creature that cannot sense a threat/food misses it). Genome-less creatures keep the
        // unfiltered global-nearest scan (byte-identical to the pre- brain; empty-roster worlds
        // run the whole brain as a no-op, so canonical/networked hashes are unaffected).
        const Comp::CreatureGenomeComponent* sg = reg.try_get<Comp::CreatureGenomeComponent>(e);
        float faceX = 0.0f, faceZ = 0.0f;
        if (sg != nullptr) {
            const float hh =
                dm::Sqrt(snap[selfIdx].hx * snap[selfIdx].hx + snap[selfIdx].hz * snap[selfIdx].hz);
            if (hh > 1e-4f) {
                faceX = snap[selfIdx].hx / hh;
                faceZ = snap[selfIdx].hz / hh;
            }
        }

        //  ( starvation handling): SUSTAINED STARVATION DEGRADES before the
        // LifespanSystem death at hunger 1.0 — effective speed falls toward 0.5x
        // and the sensory ranges shrink alike as hunger crosses 0.85 -> 1.0
        // (needs have consequences, ). Stateless: a pure function of the
        // CURRENT hunger, so no new hashed state; a well-fed creature multiplies
        // by exactly 1.0f (IEEE identity) and stays byte-identical.
        const float starve01 = utility_clamp01((cr.hunger - 0.85f) / 0.15f);
        const float starve_degrade = 1.0f - 0.5f * starve01;

        // Nearest LIVE opposite-role creature: prey -> nearest predator (threat); predator ->
        // nearest live prey (food). Carcasses are skipped so a predator moves on to live prey.
        float bestDist = 1.0e9f, tx = sx, tz = sz;
        bool found = false;
        entt::entity te = entt::null;
        // Per-candidate sensory gate + first-wins nearest-keep, factored so the inline snapshot
        // scan (default) and the  substrate path apply the IDENTICAL gate (genome
        // hearing/vision cone, starvation shrink) and selection over the SAME id-ordered candidate
        // set — the two paths are equivalent. `o` is already known to be a LIVE opposite-role
        // creature (the inline filter / the field's Build partition guarantee it).
        auto considerTarget = [&](const Snap& o) {
            const float dx = o.x - sx, dz = o.z - sz;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (sg != nullptr) {
                // Perceivable? hearing (omnidirectional within range) OR vision (cone
                // within range).: both ranges shrink under starvation.
                bool sensed = (d <= sg->hearing_range * starve_degrade);
                if (!sensed && d <= sg->vision_range * starve_degrade) {
                    if (faceX == 0.0f && faceZ == 0.0f) {
                        sensed = true; // no prior heading (stationary) -> vision omnidirectional in
                                       // range
                    } else if (d > 1e-4f) {
                        sensed = ((dx * faceX + dz * faceZ) / d) >= sg->vision_cos_half_fov;
                    } else {
                        sensed = true; // target coincident with self
                    }
                }
                if (!sensed)
                    return;
            }
            if (d < bestDist) {
                bestDist = d;
                tx = o.x;
                tz = o.z;
                te = o.e;
                found = true;
            }
        };
        if (use_perception_substrate) {
            // The shared substrate returns the
            // OPPOSITE-role live-target set (self, same-role and carcasses pre-excluded at Build)
            // in id order — the same set/order the inline scan below visits after its
            // `o.e==e || same-role || eaten` skip. The field is UNGATED, so considerTarget
            // re-applies the EXACT per-perceiver sensory gate above; bestDist/tx/tz/te/found are
            // byte-identical to the inline scan. Predators query the PREY field, prey the PREDATOR
            // field (opposite role — note this is the INVERSE of the same-role flocking grids).
            const PerceptionField& targetField = cr.is_predator ? preyTargetField : predTargetField;
            PerceptionQueryInput tq;
            tq.has_position = true;
            tq.x = sx;
            tq.z = sz; // y stays 0 -> the substrate's 3-D metric collapses onto the XZ scan plane
            targetField.Query(tq, targetSnapshot);
            for (const PerceivedSource& ps : targetSnapshot.perceived) {
                considerTarget(snap[ps.index]);
            }
        } else {
            for (const Snap& o : snap) {
                if (o.e == e || o.predator == cr.is_predator || o.eaten)
                    continue;
                considerTarget(o);
            }
        }

        //  ( scent tracking): SCENT TRACKING. A predator with NO directly-
        // perceived prey follows the prey-scent gradient (channel 0) up-wind-trail:
        // the tracked point is a fixed stride along the Weber-normalized gradient
        // direction, at a nominal outside-catch distance so the arbiter picks Hunt
        // and the pursuit code closes on it. Cold/absent trails (confidence 0)
        // leave the creature exactly as before — and a null field (every existing
        // caller) is byte-identical by construction.
        if (scent != nullptr && cr.is_predator && !found && scent_cell_size > 0.0f) {
            const int cx = WorldToScentCell(sx, scent_origin_x, scent_cell_size);
            const int cz = WorldToScentCell(sz, scent_origin_z, scent_cell_size);
            float gdx = 0.0f, gdz = 0.0f;
            const float conf = scent->GradientSteer(/*ch=*/0,
                                                    cx,
                                                    cz,
                                                    /*sign=*/+1.0f,
                                                    /*floor=*/1e-4f,
                                                    /*k=*/0.05f,
                                                    gdx,
                                                    gdz);
            if (conf > 0.0f) {
                tx = sx + gdx * 12.0f; // a stride up the gradient
                tz = sz + gdz * 12.0f;
                bestDist = 20.0f; // nominal "smelled, not seen" distance
                found = true;     // te stays null: nothing to catch yet
            }
        }

        // Catch: a predator within reach of its nearest live prey EATS it -- the prey becomes
        // a carcass and the predator's hunger is sated. Marking is idempotent + id-ordered.
        // The target was chosen from the PRE-tick snapshot, so an earlier (lower-id) predator
        // this same tick may have already eaten it -- re-check the LIVE eaten flag so one prey
        // sates at most ONE predator per tick (no feeding from an already-dead carcass).
        if (cr.is_predator && found && bestDist < tuning.catch_radius && reg.valid(te) &&
            !reg.get<Comp::CreatureComponent>(te).eaten) {
            reg.get<Comp::CreatureComponent>(te).eaten = true;
            cr.hunger = utility_clamp01(cr.hunger - tuning.catch_satiation);
        }

        CreatureSenses s;
        s.is_predator = cr.is_predator;
        s.hunger = cr.hunger;
        s.stamina = cr.stamina;
        s.energy = cr.energy;
        // Circadian off-phase drives Sleep. Absent component -> activity stays 1.0 (always
        // active) -> Sleep utility 0 -> byte-identical to a world with no circadian creatures.
        if (const auto* cc = reg.try_get<Comp::CircadianComponent>(e))
            s.circadian_activity = cc->activity;
        // thirst joins the arbiter. Absent ThirstComponent -> both stay 0
        // -> Drink utility 0 -> byte-identical. The values are LAST tick's (the thirst
        // system runs after the brain in the session order) — the same fixed 1-tick
        // phase convention scent uses for wind.
        const auto* th = reg.try_get<Comp::ThirstComponent>(e);
        if (th != nullptr) {
            s.thirst = th->thirst;
            s.water_proximity = th->water_proximity;
        }
        const float nearNorm = found ? (1.0f - utility_clamp01(bestDist / 30.0f)) : 0.0f;
        if (cr.is_predator) {
            s.food_proximity = nearNorm;
        } else {
            s.threat_proximity = nearNorm;
            s.food_proximity = 0.6f; // prey graze ambient plants
            // Herd alarm (opt-in via AlarmComponent): a prey that directly senses a predator
            // RAISES its alarm; and it flees on either the direct threat OR the propagated herd
            // alarm level (HerdAlarmSystem, slot 7) -- so the whole herd bolts together even if
            // only one saw the predator. Collective vigilance.
            if (auto* al = reg.try_get<Comp::AlarmComponent>(e)) {
                al->alarmed = (nearNorm > 0.4f) ? 1u : 0u;
                if (al->level > s.threat_proximity)
                    s.threat_proximity = al->level;
            }
        }

        const CreatureSpecies* species_definition =
            species ? species->Find(cr.species_id) : nullptr;
        const CreatureAction act = DecideCreatureAction(
            s, species_definition ? species_definition->brain : CreatureBrainParams{});
        cr.last_action = static_cast<int>(act);

        float dirx = 0.0f, dirz = 0.0f;
        switch (act) {
            case CreatureAction::Flee:
                if (found) {
                    dirx = sx - tx;
                    dirz = sz - tz;
                }
                break;
            case CreatureAction::Hunt:
                if (found) {
                    dirx = tx - sx;
                    dirz = tz - sz;
                }
                break;
            case CreatureAction::Wander: {
                const float ang =
                    2.39996323f * static_cast<float>(entt::to_integral(e) % 16u +
                                                     1u); // deterministic per-id heading
                dirx = dm::Cos(ang);
                dirz = dm::Sin(ang);
                break;
            }
            case CreatureAction::Graze:
                cr.hunger =
                    utility_clamp01(cr.hunger - tuning.hunger_graze_sate * dt); // eating sates
                break;
            case CreatureAction::Rest:
                cr.stamina =
                    utility_clamp01(cr.stamina + tuning.stamina_rest_recover * dt); // sprint fuel
                cr.energy = utility_clamp01(cr.energy +
                                            tuning.energy_rest_recover * dt); // rest off fatigue
                break;
            case CreatureAction::Sleep:
                // Deep rest: stay put (dirx/dirz remain 0 -> no wish velocity) and recover energy
                // fast. Stamina recovers too. The per-tick drain below still applies (net recover).
                cr.stamina = utility_clamp01(cr.stamina + tuning.stamina_rest_recover * dt);
                cr.energy = utility_clamp01(cr.energy + tuning.energy_sleep_recover * dt);
                break;
            case CreatureAction::Drink:
                // the arbiter chose water — head along the thirst system's
                // cached direction toward the nearest hole (drinking itself happens in
                // RunThirstOnTick once inside the radius). No component -> stay put.
                if (th != nullptr) {
                    dirx = th->wish_x;
                    dirz = th->wish_z;
                }
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
            std::vector<std::pair<float, float>> herdHeadings; // index-aligned with `herd`
            herd.reserve(herdHits.size());
            herdHeadings.reserve(herdHits.size());
            for (std::uint32_t hi : herdHits) {
                if (static_cast<std::size_t>(hi) == selfIdx)
                    continue;
                herd.emplace_back(snap[hi].x, snap[hi].z);
                herdHeadings.emplace_back(snap[hi].hx, snap[hi].hz);
            }
            // alignment_weight defaults to kAlignmentWeight (0.5) -> the Reynolds alignment term
            // is ACTIVE by default: the pre-tick headings passed below bias the steer toward the
            // herd's mean heading. Tuning it to 0 (EcologyTuning / SystemConfig sim.ecology) skips
            // alignment inside ComputeFlockSteer and reverts to the pure cohesion+separation steer.
            FlockParams fp{};
            fp.neighbor_radius = tuning.flock_neighbor_radius;
            fp.separation_radius = tuning.flock_separation_radius;
            fp.cohesion_weight = tuning.flock_cohesion_weight;
            fp.separation_weight = tuning.flock_separation_weight;
            fp.alignment_weight = tuning.alignment_weight;
            const FlockSteer fs = ComputeFlockSteer(sx, sz, herd, fp, &herdHeadings);
            adirx += fs.x * tuning.herd_weight;
            adirz += fs.z * tuning.herd_weight;
        }

        // Resolve the blended heading into a wish VELOCITY (m/s). Flee/hunt sprint at 1.5x.
        const float len = dm::Sqrt(adirx * adirx + adirz * adirz);
        cr.wish_x = 0.0f;
        cr.wish_z = 0.0f;
        if (len > 1.0e-5f) {
            const float inv = 1.0f / len;
            // the starving move at up to half pace (sprint included).
            const float speed =
                ((act == CreatureAction::Flee || act == CreatureAction::Hunt) ? cr.move_speed * 1.5f
                                                                              : cr.move_speed) *
                starve_degrade;
            cr.wish_x = adirx * inv * speed;
            cr.wish_z = adirz * inv * speed;
            cr.stamina =
                utility_clamp01(cr.stamina - tuning.stamina_move_drain * dt); // moving tires
            // When a Jolt character owns this creature (CreaturePhysicsComponent), it
            // integrates the wish velocity against the terrain (gravity/collision/slopes);
            // the physics bridge reads the resolved position back. Otherwise — the pure,
            // unit-tested path — integrate X/Z directly here (terrain-independent).
            if (!reg.all_of<Comp::CreaturePhysicsComponent>(e)) {
                tf.position.x += cr.wish_x * dt;
                tf.position.z += cr.wish_z * dt;
            }
        }
        cr.hunger =
            utility_clamp01(cr.hunger + tuning.hunger_growth_per_second * dt); // hunger grows
        cr.energy = utility_clamp01(cr.energy - tuning.energy_drain_per_second *
                                                    dt); // awake tires (Rest net-recovers)
        ++stats.updated;
    }
    return stats;
}

} // namespace luminumbra::ai
