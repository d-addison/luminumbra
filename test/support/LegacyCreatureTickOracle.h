#pragma once
// Frozen pre-D reference implementations. Keep independent of production entry points.

#include "ai/CreatureBrain.h"
#include "ai/CreatureSpeciesRegistry.h"
#include "ai/Flocking.h"
#include "ai/PerceptionSubstrate.h" // canonical shared neighbor-scan substrate
#include "ai/ScentField.h"          // in-brain scent tracking (GradientSteer)
#include "ai/SpatialGrid.h"
#include "components/AlarmComponents.h"
#include "components/CircadianComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/ThirstComponents.h" // thirst joins the arbiter
#include "core/DeterministicMath.h"
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <utility>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

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

} // namespace LegacyCreatureTickOracle

#include "ai/CreatureBrain.h" // CreatureAction (skip mate-seeking while fleeing)
#include "ai/CreatureGenome.h"
#include "ai/CreatureSpeciesRegistry.h"
#include "ai/UtilityAI.h" // utility_clamp01
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/InstinctComponents.h" // PerceptionComponent ( sensory phenotype)
#include "core/DeterministicMath.h"
#include "core/DeterministicRng.h"
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

inline constexpr std::uint64_t kReproSeedOffset = 16ull;

// Eligibility: mature, healthy, well-fed, off cooldown, alive (and prey — predators don't
// breed in this slice; the heritable population being selected on is the prey).
inline constexpr std::uint32_t kReproMaturityTicks = 90;  // ~3s @ 30Hz
inline constexpr std::uint32_t kReproCooldownTicks = 300; // ~10s @ 30Hz after a birth
inline constexpr float kReproHealthyStamina = 0.5f;
// Mate search + courtship geometry.
inline constexpr float kMateSeekRadius = 45.0f;      // can sense a mate this far
inline constexpr float kCourtshipRadius = 2.6f;      // must be this close to court
inline constexpr std::uint32_t kCourtshipTicks = 90; // time together before a baby (~3s)
inline constexpr float kReproSpawnRadius = 1.2f;     // newborn appears beside the pair

struct CreatureReproductionStats {
    int born = 0;     // offspring created this tick
    int courting = 0; // ready females currently adjacent to a ready male
};

// Full-control tuning (defaults == the k* constants -> byte-identical). From SystemConfig
// sim.reproduction. Tick fields are integer; the float-valued config params are cast on resolve.
struct ReproductionTuning {
    std::uint32_t maturity_ticks = kReproMaturityTicks;
    std::uint32_t cooldown_ticks = kReproCooldownTicks;
    std::uint32_t courtship_ticks = kCourtshipTicks;
    float healthy_stamina = kReproHealthyStamina;
    float mate_seek_radius = kMateSeekRadius;
    float courtship_radius = kCourtshipRadius;
    float spawn_radius = kReproSpawnRadius;
};

// Ready to mate? (prey, alive, mature, off cooldown, well-fed, healthy)
[[nodiscard]] inline bool IsReadyToMate(const Comp::CreatureComponent& cr,
                                        const Comp::CreatureGenomeComponent& gn,
                                        const ReproductionTuning& t = {}) {
    return !cr.eaten && !cr.is_predator && gn.age_ticks >= t.maturity_ticks &&
           gn.reproduce_cooldown == 0 && cr.hunger <= gn.hunger_threshold &&
           cr.stamina >= t.healthy_stamina;
}

// PHASE A (run BEFORE the physics bridge): ready creatures steer toward the nearest ready
// opposite-sex mate by OVERRIDING the brain's wish velocity — unless they are fleeing a
// threat (survival beats courtship). When already within courtship range they hold still
// (wish 0) so they stay together and court. id-ordered; reads a pre snapshot (order-stable).
inline void RunMateSeekingOnTick(entt::registry& reg, const ReproductionTuning& tuning = {}) {
    auto view = reg.view<Comp::CreatureComponent,
                         Comp::CreatureGenomeComponent,
                         Comp::TransformComponent>();
    struct M {
        entt::entity e;
        float x, z;
        bool female;
        bool ready;
        std::uint16_t species_id;
    };
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    std::vector<M> snap;
    snap.reserve(ents.size());
    for (auto e : ents) {
        const auto& tf = view.get<Comp::TransformComponent>(e);
        const auto& cr = view.get<Comp::CreatureComponent>(e);
        const auto& gn = view.get<Comp::CreatureGenomeComponent>(e);
        snap.push_back({e,
                        tf.position.x,
                        tf.position.z,
                        gn.female,
                        IsReadyToMate(cr, gn, tuning),
                        cr.species_id});
    }
    for (std::size_t i = 0; i < ents.size(); ++i) {
        const M& self = snap[i];
        if (!self.ready)
            continue;
        auto& cr = view.get<Comp::CreatureComponent>(self.e);
        if (cr.last_action == static_cast<int>(CreatureAction::Flee))
            continue; // survival first
        float best = tuning.mate_seek_radius, mx = 0.0f, mz = 0.0f;
        bool found = false;
        for (const M& o : snap) {
            if (o.e == self.e || !o.ready || o.female == self.female ||
                o.species_id != self.species_id)
                continue;
            const float dx = o.x - self.x, dz = o.z - self.z;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (d < best) {
                best = d;
                mx = o.x;
                mz = o.z;
                found = true;
            }
        }
        if (!found)
            continue;
        const float dx = mx - self.x, dz = mz - self.z;
        const float d = dm::Sqrt(dx * dx + dz * dz);
        if (d > tuning.courtship_radius && d > 1.0e-5f) {
            const float inv = cr.move_speed / d;
            cr.wish_x = dx * inv; // steer toward the mate (overrides the brain's wander)
            cr.wish_z = dz * inv;
        } else {
            cr.wish_x = 0.0f; // close enough — hold still and court
            cr.wish_z = 0.0f;
        }
    }
}

// PHASE B (run AFTER the physics bridge): advance age/cooldown, accumulate courtship for ready
// adjacent opposite-sex pairs, and birth one offspring per completed courtship (female-driven
// so each completion makes exactly one baby; the chosen male is consumed for the tick).
inline CreatureReproductionStats
RunMatingResolveOnTick(entt::registry& reg,
                       std::uint64_t tick,
                       std::uint64_t world_seed = 0,
                       const ReproductionTuning& tuning = {},
                       const CreatureSpeciesRegistry* species = nullptr) {
    CreatureReproductionStats stats;
    auto view = reg.view<Comp::CreatureComponent,
                         Comp::CreatureGenomeComponent,
                         Comp::TransformComponent>();
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    // Bookkeeping: age everyone, tick down cooldowns (id-ordered, deterministic).
    for (auto e : ents) {
        auto& gn = view.get<Comp::CreatureGenomeComponent>(e);
        if (gn.age_ticks < 0xFFFFFFFFu)
            ++gn.age_ticks;
        if (gn.reproduce_cooldown > 0)
            --gn.reproduce_cooldown;
    }

    // Snapshot ready males (by position) for the courtship search.
    struct Male {
        entt::entity e;
        float x, z;
        std::uint16_t species_id;
    };
    std::vector<Male> males;
    for (auto e : ents) {
        const auto& cr = view.get<Comp::CreatureComponent>(e);
        const auto& gn = view.get<Comp::CreatureGenomeComponent>(e);
        if (!gn.female && IsReadyToMate(cr, gn, tuning)) {
            const auto& tf = view.get<Comp::TransformComponent>(e);
            males.push_back({e, tf.position.x, tf.position.z, cr.species_id});
        }
    }

    struct Birth {
        CreatureGenome genome;
        float x, y, z;
        std::uint32_t generation;
        bool female;
        std::uint16_t species_id;
    };
    std::vector<Birth> births;
    std::vector<bool> male_used(males.size(), false);

    // Female-driven courtship: each ready female courts her nearest unused ready male in range.
    for (auto e : ents) {
        auto& cr = view.get<Comp::CreatureComponent>(e);
        auto& gn = view.get<Comp::CreatureGenomeComponent>(e);
        if (!gn.female || !IsReadyToMate(cr, gn, tuning)) {
            continue;
        }
        const auto& tf = view.get<Comp::TransformComponent>(e);
        int best = -1;
        float bestDist = tuning.courtship_radius;
        for (std::size_t m = 0; m < males.size(); ++m) {
            if (male_used[m] || males[m].species_id != cr.species_id)
                continue;
            const float dx = males[m].x - tf.position.x, dz = males[m].z - tf.position.z;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (d < bestDist) {
                bestDist = d;
                best = static_cast<int>(m);
            }
        }
        if (best < 0) {
            gn.courting_ticks = 0;
            continue;
        } // no mate adjacent -> reset
        ++stats.courting;
        ++gn.courting_ticks;
        if (gn.courting_ticks < tuning.courtship_ticks)
            continue; // still courting

        // Courtship complete -> a baby. Consume this male for the tick.
        const entt::entity maleE = males[static_cast<std::size_t>(best)].e;
        male_used[static_cast<std::size_t>(best)] = true;
        auto& mgn = view.get<Comp::CreatureGenomeComponent>(maleE);

        CreatureGenome fG;
        fG.move_speed = gn.move_speed;
        fG.vigilance = gn.vigilance;
        fG.hunger_threshold = gn.hunger_threshold;
        fG.size_scale = gn.size_scale;
        fG.vision_cos_half_fov = gn.vision_cos_half_fov;
        fG.vision_range = gn.vision_range;
        fG.hearing_range = gn.hearing_range;
        CreatureGenome mG;
        mG.move_speed = mgn.move_speed;
        mG.vigilance = mgn.vigilance;
        mG.hunger_threshold = mgn.hunger_threshold;
        mG.size_scale = mgn.size_scale;
        mG.vision_cos_half_fov = mgn.vision_cos_half_fov;
        mG.vision_range = mgn.vision_range;
        mG.hearing_range = mgn.hearing_range;

        luminumbra::core::DeterministicRng rng = luminumbra::core::DeterministicRng::seeded(
            kReproSeedOffset ^ world_seed,
            (static_cast<std::uint64_t>(entt::to_integral(e)) * 0x9E3779B97F4A7C15ull) ^
                static_cast<std::uint64_t>(entt::to_integral(maleE)),
            tick);
        const CreatureSpecies* species_definition =
            species ? species->Find(cr.species_id) : nullptr;
        const SpeciesGenomeRanges ranges =
            species_definition ? species_definition->genome_ranges : SpeciesGenomeRanges{};
        CreatureGenome childG = BreedOffspring(fG, mG, rng, ranges);
        const bool childFemale = (rng.next_u64() & 1ull) == 0ull;
        // inherit the SENSORY genes from draws taken AFTER the core breed + sex draw, so the
        // 4-gene core genome and the sex bit are byte-identical to before this slice (the ecology
        // hash reads only move_speed/generation/age_ticks, so it is unchanged for genome rosters).
        childG = BreedSensoryInto(childG, fG, mG, rng, ranges);

        const auto& mtf = view.get<Comp::TransformComponent>(maleE);
        Birth b;
        b.genome = childG;
        b.x = (tf.position.x + mtf.position.x) * 0.5f;
        b.y = (tf.position.y + mtf.position.y) * 0.5f;
        b.z = (tf.position.z + mtf.position.z) * 0.5f + tuning.spawn_radius;
        b.generation = (gn.generation > mgn.generation ? gn.generation : mgn.generation) + 1u;
        b.female = childFemale;
        b.species_id = cr.species_id; // offspring inherit the mother's catalog species identity
        births.push_back(b);

        gn.courting_ticks = 0;
        gn.reproduce_cooldown = tuning.cooldown_ticks;
        mgn.reproduce_cooldown = tuning.cooldown_ticks;
    }

    // Create offspring AFTER the read pass (no view invalidation mid-iteration).
    for (const Birth& b : births) {
        const auto child = reg.create();
        auto& tf = reg.emplace<Comp::TransformComponent>(child);
        tf.position.x = b.x;
        tf.position.y = b.y;
        tf.position.z = b.z;
        tf.scale = ::Luminumbra::Vec3(b.genome.size_scale);

        auto& cr = reg.emplace<Comp::CreatureComponent>(child);
        cr.is_predator = false;
        cr.species_id = b.species_id; // inherit catalog species from the mother
        cr.hunger = 0.5f;
        cr.stamina = 1.0f;
        cr.move_speed = b.genome.move_speed;

        auto& gn = reg.emplace<Comp::CreatureGenomeComponent>(child);
        gn.move_speed = b.genome.move_speed;
        gn.vigilance = b.genome.vigilance;
        gn.hunger_threshold = b.genome.hunger_threshold;
        gn.size_scale = b.genome.size_scale;
        gn.vision_cos_half_fov = b.genome.vision_cos_half_fov;
        gn.vision_range = b.genome.vision_range;
        gn.hearing_range = b.genome.hearing_range;
        gn.age_ticks = 0;
        gn.reproduce_cooldown = tuning.cooldown_ticks; // newborn can't immediately breed
        gn.generation = b.generation;
        gn.female = b.female;
        gn.courting_ticks = 0;

        // express the sensory genes into a PerceptionComponent so the offspring's senses are
        // genetically determined (vision cone + range, hearing range). PerceptionComponent is pure
        // data — it is not in the ecology hash, and the perception system needs an
        // AwarenessComponent to act — so this is inert for the canonical hash and only bites where
        // a roster opts its creatures into sensing. This is the read side that makes sensory
        // divergence observable.
        auto& pc = reg.emplace<Comp::PerceptionComponent>(child);
        pc.vision_cos_half_fov = b.genome.vision_cos_half_fov;
        pc.vision_range = b.genome.vision_range;
        pc.ear.range = b.genome.hearing_range;

        ++stats.born;
    }
    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/MigratoryComponents.h"
#include "components/PackHunterComponents.h"
#include "components/TerritoryComponents.h"
#include <entt/entt.hpp>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

namespace Comp = ::Luminumbra::Components;

struct SteeringConsumerStats {
    int packed = 0;   // predators steered by a pack flank direction
    int migrated = 0; // creatures given a migration drift
    int homed = 0;    // creatures given a territory homing bias
};

inline SteeringConsumerStats RunSteeringConsumerOnTick(entt::registry& reg) {
    SteeringConsumerStats stats;
    auto sv = reg.view<Comp::CreatureComponent, Comp::TransformComponent>();
    for (auto e : sv) {
        auto& cr = sv.get<Comp::CreatureComponent>(e);
        if (cr.eaten)
            continue; // a carcass is inert

        // Pack flanking OVERRIDES the predator's wish with its flank-steer DIRECTION.
        // PackHunterComponent.coord_x/z is a UNIT vector toward the flank approach point (NOT a
        // world position), so it is used directly as the heading.
        if (cr.is_predator) {
            if (auto* pk = reg.try_get<Comp::PackHunterComponent>(e);
                pk && pk->in_pack && (pk->coord_x != 0.0f || pk->coord_z != 0.0f)) {
                const float sp = cr.move_speed * 1.5f;
                cr.wish_x = pk->coord_x * sp;
                cr.wish_z = pk->coord_z * sp;
                ++stats.packed;
            }
        }
        // Migration + territory ADD a gentle drift on top (their wishes are velocity-ish vectors).
        if (auto* mig = reg.try_get<Comp::MigratoryComponent>(e)) {
            cr.wish_x += mig->wish_x;
            cr.wish_z += mig->wish_z;
            ++stats.migrated;
        }
        if (auto* tb = reg.try_get<Comp::TerritoryBiasComponent>(e)) {
            cr.wish_x += tb->wish_x;
            cr.wish_z += tb->wish_z;
            ++stats.homed;
        }
    }
    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/ThirstComponents.h"
#include "core/DeterministicMath.h"
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry:... herd-alarm+26, thirst+33). This system is
// deterministic and consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kThirstSeedOffset = 33ull;

// Per-second rate at which an idle creature's thirst rises (full parch in ~1/kThirstRise s
// at the sim's fixed dt). Tuned slow enough to complement, not dominate, hunger.
inline constexpr float kThirstRise = 0.04f;

// Per-second rate at which thirst falls while drinking inside a water hole (drinks faster
// than it accrues so a creature at a hole quenches and then leaves).
inline constexpr float kThirstDrink = 0.5f;

// A creature only bothers walking toward water once it is at least this thirsty; below it the
// wish is zeroed (it grazes/wanders normally). Keeps creatures from constantly micro-steering.
inline constexpr float kThirstSeekThreshold = 0.3f;

struct ThirstStats {
    int participants = 0; // creatures carrying a ThirstComponent that took part
    int seeking = 0;      // creatures that wrote a non-zero water-seeking wish this tick
    int drinking = 0;     // creatures inside a water hole's radius this tick
};

// Full-control tuning (defaults == the k* constants -> byte-identical). From SystemConfig
// sim.thirst.
struct ThirstTuning {
    float rise_rate = kThirstRise;
    float drink_rate = kThirstDrink;
    float seek_threshold = kThirstSeekThreshold;
};

[[nodiscard]] inline float ThirstClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// RunThirstOnTick: raise thirst, steer thirsty creatures toward the nearest water hole, and
// let them drink when in range. id-ordered. `dt` scales the rise/drink rates so the behaviour
// is frame-rate independent (the sim runs a fixed dt; the parameter keeps the signature honest
// and lets tests exercise multiple ticks).
inline ThirstStats RunThirstOnTick(entt::registry& reg, float dt, const ThirstTuning& tuning = {}) {
    ThirstStats stats;

    // Snapshot every water hole once (deterministic, read-only this tick). id-ordered for a
    // stable nearest-tie resolution.
    struct Hole {
        float x, z;
        float radius;
    };
    std::vector<Hole> holes;
    {
        auto hview = reg.view<Comp::WaterHoleComponent, Comp::TransformComponent>();
        std::vector<entt::entity> hents(hview.begin(), hview.end());
        std::sort(hents.begin(), hents.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        holes.reserve(hents.size());
        for (auto e : hents) {
            const auto& wh = hview.get<Comp::WaterHoleComponent>(e);
            const auto& tf = hview.get<Comp::TransformComponent>(e);
            holes.push_back({tf.position.x, tf.position.z, wh.radius});
        }
    }

    auto view = reg.view<Comp::ThirstComponent, Comp::TransformComponent>();

    // id-ordered participant list (deterministic traversal).
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    // dt-scaled rates: the sim's fixed dt (~1/30s) gives the tuned per-second constants.
    const float rise = tuning.rise_rate * dt;
    const float drink = tuning.drink_rate * dt;

    for (auto e : ents) {
        auto& th = view.get<Comp::ThirstComponent>(e);
        const auto& tf = view.get<Comp::TransformComponent>(e);

        // A carcass neither thirsts nor drinks: hold it inert and skip.
        if (const auto* cr = reg.try_get<Comp::CreatureComponent>(e); cr && cr->eaten) {
            th.wish_x = 0.0f;
            th.wish_z = 0.0f;
            th.drinking = 0;
            ++stats.participants;
            continue;
        }

        ++stats.participants;

        // Thirst rises this tick (clamped).
        th.thirst = ThirstClamp01(th.thirst + rise);

        // Find the nearest water hole (by squared distance; id-order breaks ties since the
        // first hole at a given distance wins the strict-less comparison).
        bool have_nearest = false;
        float best_d2 = 0.0f;
        float best_dx = 0.0f, best_dz = 0.0f;
        float best_radius = 0.0f;
        for (const Hole& h : holes) {
            const float dx = h.x - tf.position.x;
            const float dz = h.z - tf.position.z;
            const float d2 = dx * dx + dz * dz;
            if (!have_nearest || d2 < best_d2) {
                have_nearest = true;
                best_d2 = d2;
                best_dx = dx;
                best_dz = dz;
                best_radius = h.radius;
            }
        }

        // Default: not steering, not drinking, no water known.
        th.wish_x = 0.0f;
        th.wish_z = 0.0f;
        th.drinking = 0;
        th.water_proximity = 0.0f;

        if (!have_nearest) {
            // No water in the world: the creature still gets thirsty, but has nowhere to go.
            continue;
        }

        const float dist = dm::Sqrt(best_d2);
        // nearness for the arbiter's Drink utility — 1 at the hole,
        // fading to 0 at the 40 m sensing horizon.
        th.water_proximity = ThirstClamp01(1.0f - dist / 40.0f);

        if (dist <= best_radius) {
            // In range of the nearest hole: drink. Thirst falls (clamped >= 0). No wish — it
            // has arrived; the orchestrator can stop pulling it toward water.
            th.drinking = 1;
            th.thirst = ThirstClamp01(th.thirst - drink);
            ++stats.drinking;
            continue;
        }

        // Thirsty enough to seek? Write a unit direction toward the hole, scaled by thirst,
        // so a parched creature pulls harder than a mildly-thirsty one.
        if (th.thirst >= tuning.seek_threshold && dist > 0.0f) {
            const float inv = 1.0f / dist;
            th.wish_x = best_dx * inv * th.thirst;
            th.wish_z = best_dz * inv * th.thirst;
            ++stats.seeking;
        }
    }

    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/MortalComponents.h"
#include "components/ScavengerComponent.h"
#include "core/DeterministicMath.h"
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry: ... herd-alarm+26, ..., scavenging+34). This system
// is deterministic and consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kScavengingSeedOffset = 34ull;

// A scavenger only seeks food when its hunger is ABOVE this (0 sated .. 1 starving). Below it
// the scavenger is well-fed and ignores carcasses (zero wish, no feeding).
inline constexpr float kScavengeHungerThreshold = 0.3f;

// Horizontal distance (world units) within which a scavenger can FEED on a carcass.
inline constexpr float kScavengeFeedRadius = 1.5f;

// Hunger removed per tick of feeding (clamped so hunger never goes below 0). ~0.05/tick at
// 30Hz sates a starving scavenger over ~0.6s of feeding.
inline constexpr float kScavengeFeedRate = 0.05f;

struct ScavengingStats {
    int scavengers = 0; // creatures carrying a ScavengerComponent that took part
    int carcasses = 0;  // dead creatures available as food this tick
    int seeking = 0;    // hungry live scavengers that found a carcass to steer toward
    int feeding = 0;    // scavengers within feed radius that lowered hunger this tick
};

[[nodiscard]] inline float ScavengeClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// Full-control tuning (defaults == the k* constants -> byte-identical). From SystemConfig
// sim.scavenging.
struct ScavengingTuning {
    float hunger_threshold = kScavengeHungerThreshold;
    float feed_radius = kScavengeFeedRadius;
    float feed_rate = kScavengeFeedRate;
};

// Is this creature a CARCASS (dead body available to scavenge)? eaten prey OR a flagged-dead
// mortal. `mortal` may be null (creature carries no MortalComponent).
[[nodiscard]] inline bool ScavengeIsCarcass(const Comp::CreatureComponent& cr,
                                            const Comp::MortalComponent* mortal) {
    if (cr.eaten)
        return true;
    if (mortal != nullptr && mortal->dead != 0)
        return true;
    return false;
}

// RunScavengingOnTick: hungry scavengers seek + eat the nearest carcass. id-ordered,
// two-phase snapshot (order-independent). Returns telemetry; mutates ScavengerComponent
// .wish_x/z + .feeding and CreatureComponent.hunger on feeding scavengers.
inline ScavengingStats RunScavengingOnTick(entt::registry& reg,
                                           std::uint64_t /*tick*/,
                                           const ScavengingTuning& tuning = {}) {
    ScavengingStats stats;

    // ---- First pass: snapshot every carcass position. ----
    // Iterate ALL creatures (a carcass need not be a scavenger). MortalComponent is optional,
    // so fetch it via try_get rather than constraining the view.
    struct Carcass {
        float x, z;
    };
    std::vector<Carcass> carcasses;
    {
        auto cview = reg.view<Comp::CreatureComponent, Comp::TransformComponent>();
        for (auto e : cview) {
            const auto& cr = cview.get<Comp::CreatureComponent>(e);
            const auto* mortal = reg.try_get<Comp::MortalComponent>(e);
            if (!ScavengeIsCarcass(cr, mortal))
                continue;
            const auto& tf = cview.get<Comp::TransformComponent>(e);
            carcasses.push_back({tf.position.x, tf.position.z});
        }
    }
    stats.carcasses = static_cast<int>(carcasses.size());

    // ---- gather scavengers (id-ordered for deterministic traversal) ----
    auto sview =
        reg.view<Comp::ScavengerComponent, Comp::CreatureComponent, Comp::TransformComponent>();
    std::vector<entt::entity> ents(sview.begin(), sview.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.
    stats.scavengers = static_cast<int>(ents.size());

    // ---- Second pass: steer and feed against the frozen carcass list. ----
    for (auto e : ents) {
        auto& sc = sview.get<Comp::ScavengerComponent>(e);
        auto& cr = sview.get<Comp::CreatureComponent>(e);
        const auto& tf = sview.get<Comp::TransformComponent>(e);

        // Default: no target this tick. (Reset so a stale wish/feeding doesn't persist.)
        sc.wish_x = 0.0f;
        sc.wish_z = 0.0f;
        sc.feeding = 0;

        // A dead scavenger doesn't scavenge; a well-fed one doesn't seek.
        const auto* selfMortal = reg.try_get<Comp::MortalComponent>(e);
        const bool selfDead = ScavengeIsCarcass(cr, selfMortal);
        if (selfDead)
            continue;
        if (cr.hunger <= tuning.hunger_threshold)
            continue; // sated enough — ignore food.
        if (carcasses.empty())
            continue; // nothing to scavenge.

        // Find the NEAREST carcass (deterministic Sqrt distance; first-found wins ties, and
        // the carcass list is built in a fixed order so ties are stable).
        float bestD = -1.0f;
        float bestDx = 0.0f;
        float bestDz = 0.0f;
        for (const Carcass& c : carcasses) {
            const float dx = c.x - tf.position.x;
            const float dz = c.z - tf.position.z;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (bestD < 0.0f || d < bestD) {
                bestD = d;
                bestDx = dx;
                bestDz = dz;
            }
        }
        // (carcasses non-empty here, so bestD >= 0.)
        ++stats.seeking;

        // Within feed radius: feed (lower hunger) and flag feeding. Hold position (zero wish)
        // so the scavenger settles on the carcass rather than overshooting it.
        if (bestD <= tuning.feed_radius) {
            sc.feeding = 1;
            cr.hunger = ScavengeClamp01(cr.hunger - tuning.feed_rate);
            ++stats.feeding;
            // wish stays (0,0) — already on the food.
            continue;
        }

        // Otherwise steer toward the carcass: unit direction (guard the degenerate near-zero
        // distance, though bestD > feed radius > 0 here makes that impossible).
        if (bestD > 1.0e-5f) {
            const float inv = 1.0f / bestD;
            sc.wish_x = bestDx * inv;
            sc.wish_z = bestDz * inv;
        }
    }

    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/AlarmComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "core/DeterministicMath.h"
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry:... pollination+19, disease+20, irrigation+21,
// lifespan+22, wildlife+23, photo+24, herd-alarm+26). This system is deterministic and
// consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kHerdAlarmSeedOffset = 26ull;

// Distance (world units) within which an alarmed creature warns same-role neighbours.
inline constexpr float kAlarmRadius = 12.0f;

// A creature whose accumulated level is at/above this acts as a propagating SOURCE (so the
// alarm relays outward in  through a chain of creatures, not just from the originator).
inline constexpr float kAlarmSourceThreshold = 0.25f;

// Per-tick retention of a creature's own level when no source is nearby: new_self = level *
// kAlarmDecay. ~0.85 fades a full alarm to near-zero over ~1s @30Hz once the threat passes.
inline constexpr float kAlarmDecay = 0.85f;

// Fraction of a source's strength that transfers to a neighbour AT zero distance (it falls
// linearly to 0 at kAlarmRadius). Keeps a relayed creature slightly below its source so the
// chain forms a decreasing gradient outward rather than a flat plateau.
inline constexpr float kAlarmTransfer = 0.9f;

struct HerdAlarmStats {
    int participants = 0; // creatures carrying an AlarmComponent that took part
    int sources = 0;      // creatures acting as an alarm source this tick
    int raised = 0;       // creatures whose level INCREASED this tick (propagation reached)
};

[[nodiscard]] inline float AlarmClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// RunHerdAlarmOnTick: maintain the propagating alarm field. id-ordered, two-phase snapshot.
// `dt` scales the decay/propagation rate so the field is frame-rate independent (the sim
// runs a fixed dt; the parameter keeps the signature honest and lets tests exercise decay).
inline HerdAlarmStats RunHerdAlarmOnTick(entt::registry& reg, float dt) {
    HerdAlarmStats stats;

    auto view = reg.view<Comp::AlarmComponent, Comp::CreatureComponent, Comp::TransformComponent>();

    // id-ordered participant list (deterministic traversal).
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    // ---- First pass: snapshot each participant's pre-tick state. ----
    struct Snap {
        entt::entity e;
        float x, z;
        float level;        // pre-tick level
        bool predator;      // role to match on
        bool dead;          // carcass: neither emits nor receives
        float src_strength; // 0 if not a source this tick; else its emitting strength
    };
    std::vector<Snap> snap;
    snap.reserve(ents.size());
    for (auto e : ents) {
        const auto& al = view.get<Comp::AlarmComponent>(e);
        const auto& cr = view.get<Comp::CreatureComponent>(e);
        const auto& tf = view.get<Comp::TransformComponent>(e);
        const float level = AlarmClamp01(al.level);
        const bool dead = cr.eaten;
        float src = 0.0f;
        if (!dead) {
            // alarmed==1 is a full-strength source; otherwise above-threshold level relays.
            if (al.alarmed != 0) {
                src = 1.0f;
            } else if (level >= kAlarmSourceThreshold) {
                src = level;
            }
        }
        snap.push_back({e, tf.position.x, tf.position.z, level, cr.is_predator, dead, src});
    }

    stats.participants = static_cast<int>(snap.size());
    for (const Snap& s : snap) {
        if (s.src_strength > 0.0f)
            ++stats.sources;
    }

    // dt-scaled rates. The sim's fixed dt (~1/30s) gives the tuned constants; we keep the
    // behaviour bounded for any dt by clamping the scaled fractions to [0,1].
    const float rate = dt * static_cast<float>(::Luminumbra::TICKS_PER_SECOND); // 1.0 at 30Hz
    // Decay applied this tick: level retains (kAlarmDecay) per 1/30s. Approximate the
    // per-dt retention by scaling the *loss* (1-kAlarmDecay) with rate, clamped so a large
    // dt can drain but never invert.
    float decay_loss = (1.0f - kAlarmDecay) * rate;
    decay_loss = AlarmClamp01(decay_loss);
    const float keep = 1.0f - decay_loss;

    // ---- Second pass: compute each creature's level from the frozen snapshot. ----
    const float invR = kAlarmRadius > 0.0f ? (1.0f / kAlarmRadius) : 0.0f;
    for (std::size_t i = 0; i < snap.size(); ++i) {
        const Snap& self = snap[i];

        auto& al = view.get<Comp::AlarmComponent>(self.e);

        if (self.dead) {
            // The dead don't warn and don't fear: drain to zero deterministically.
            al.level = 0.0f;
            continue;
        }

        // Self term: own pre-tick level decayed toward 0. An alarmed source pins itself to
        // full so the originator stays lit while it senses the threat.
        float next = self.level * keep;
        if (al.alarmed != 0) {
            next = 1.0f; // pin ONLY a genuinely-alarmed originator (not a relay that hit 1.0).
        }

        // Propagation term: take the STRONGEST same-role source contribution in range
        // (max, not sum, so density doesn't blow the field past 1 and the gradient is the
        // distance to the nearest/loudest alarm). Scaled by closeness and source strength.
        float best_in = 0.0f;
        for (std::size_t j = 0; j < snap.size(); ++j) {
            if (j == i)
                continue;
            const Snap& o = snap[j];
            if (o.dead || o.src_strength <= 0.0f)
                continue;
            if (o.predator != self.predator)
                continue; // only warn your own kind/role.
            const float dx = o.x - self.x;
            const float dz = o.z - self.z;
            const float d = dm::Sqrt(dx * dx + dz * dz);
            if (d >= kAlarmRadius)
                continue;                            // out of earshot.
            const float closeness = 1.0f - d * invR; // 1 at source.. 0 at edge.
            const float contrib = o.src_strength * kAlarmTransfer * closeness * rate;
            if (contrib > best_in)
                best_in = contrib;
        }

        // Fold the incoming alarm in: the field rises to whichever is louder — its decayed
        // self level or the best incoming contribution (so a relayed creature lights up but
        // a creature already louder than its neighbour isn't pulled down by them).
        if (best_in > next)
            next = best_in;

        next = AlarmClamp01(next);
        if (next > self.level + 1.0e-6f)
            ++stats.raised;
        al.level = next;
    }

    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/CreatureComponents.h"
#include "components/DecayComponents.h"
#include "components/MortalComponents.h"
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

namespace Comp = ::Luminumbra::Components;

// Reserved seed-stream offset (registry: ... lifespan+22, wildlife+23, photo+24, herd-alarm+26,
// DECOMPOSITION CLAIMS +27). This system is deterministic and consumes NO rng; the offset is
// recorded for collision-avoidance only.
inline constexpr std::uint64_t kDecompositionSeedOffset = 27ull;

// Total nutrient a single fully-decomposed corpse releases, in milli-units (fixed-point:
// 1000 = one whole nutrient unit). The cumulative release reaches exactly this at
// decay_duration. Sized to fit the component's std::uint16_t store (<= 65535).
inline constexpr std::uint32_t kNutrientPerCorpseMilli = 1000u;

struct DecompositionStats {
    int decaying = 0;                 // dead entities that advanced their decay this tick
    std::uint32_t released_total = 0; // nutrient released THIS TICK across all corpses (milli)
    int finished = 0;                 // entities that reached full decomposition this tick
};

// Is this entity dead? Death is marked by MortalComponent.dead, OR (if the entity is a
// creature) by CreatureComponent.eaten (the carcass seam used by the brain/predation).
[[nodiscard]] inline bool DecompIsDead(entt::registry& reg, entt::entity e) {
    if (const auto* m = reg.try_get<Comp::MortalComponent>(e)) {
        if (m->dead != 0)
            return true;
    }
    if (const auto* cr = reg.try_get<Comp::CreatureComponent>(e)) {
        if (cr->eaten)
            return true;
    }
    return false;
}

// RunDecompositionOnTick: advance the decomposition clock + nutrient release for every dead
// participant. id-ordered, pure-integer, deterministic. `tick` is accepted for signature
// parity with the other sim systems (it is NOT consumed — decomposition is tick-count driven,
// not absolute-tick driven, so re-running from a snapshot reproduces exactly).
inline DecompositionStats RunDecompositionOnTick(entt::registry& reg, std::uint64_t /*tick*/) {
    DecompositionStats stats;

    auto view = reg.view<Comp::DecayComponent>();

    // id-ordered participant list (deterministic traversal).
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    for (auto e : ents) {
        auto& dc = view.get<Comp::DecayComponent>(e);

        // Already finished: inert and stable (latched) — never advance again.
        if (dc.fully_decomposed != 0)
            continue;

        // A degenerate duration is treated as immediate-complete on first dead tick (avoids a
        // divide-by-zero and keeps the contract: a 0-duration corpse releases its full load at
        // once). A live entity still must not advance.
        if (!DecompIsDead(reg, e))
            continue; // LIVE -> does not decay.

        // The pre-tick cumulative release for this corpse (already accounted in the component).
        const std::uint32_t prev_total = dc.nutrient_released_milli;

        // Advance the decomposition clock by one tick, saturating at the duration.
        if (dc.decay_ticks < dc.decay_duration) {
            ++dc.decay_ticks;
        }
        ++stats.decaying;

        // Cumulative release is EXACTLY proportional to elapsed decay (integer division), so it
        // rises monotonically and lands on kNutrientPerCorpseMilli precisely at decay_duration.
        std::uint32_t new_total;
        if (dc.decay_duration == 0u || dc.decay_ticks >= dc.decay_duration) {
            new_total = kNutrientPerCorpseMilli; // finished -> full load.
        } else {
            new_total = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(kNutrientPerCorpseMilli) * dc.decay_ticks) /
                dc.decay_duration);
        }
        if (new_total < prev_total)
            new_total = prev_total; // clamp monotonic (defensive).

        dc.nutrient_released_milli = static_cast<std::uint16_t>(new_total);
        stats.released_total += (new_total - prev_total);

        // Latch full decomposition once the clock reaches the duration.
        if (dc.decay_ticks >= dc.decay_duration) {
            dc.fully_decomposed = 1;
            ++stats.finished;
        }
    }

    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/CircadianComponents.h"
#include "core/DeterministicMath.h"
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry: ... photo+24, herd-alarm+26, day-night+29). This
// system is deterministic and consumes NO rng; the offset is recorded for collision-avoidance
// only.
inline constexpr std::uint64_t kCircadianSeedOffset = 29ull;

struct CircadianStats {
    int participants = 0; // creatures carrying a CircadianComponent that took part
    int diurnal = 0;      // of those, how many are diurnal (nocturnal == 0)
    int nocturnal = 0;    // of those, how many are nocturnal (nocturnal != 0)
};

[[nodiscard]] inline float CircadianClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// CircadianActivity: the pure day curve for one creature. `time_of_day01` is wrapped into
// [0,1) first (the caller's clock may overshoot at a day boundary), then mapped to an angle
// and run through DeterministicMath::Cos. Returns activity in [0,1]: diurnal peaks at noon
// (t=0.5), nocturnal peaks at midnight (t=0). Pure + deterministic; no rng, no libm.
[[nodiscard]] inline float
CircadianActivity(float time_of_day01, bool nocturnal, float amplitude = 1.0f) {
    // Wrap to [0,1) so a clock that reports e.g. 1.25 or -0.1 still maps onto the day. Done
    // with float +-* and an integer floor so it is bit-stable (no libm fmod).
    float t = time_of_day01;
    const float floor_t =
        static_cast<float>(static_cast<std::int64_t>(t)) - (t < 0.0f ? 1.0f : 0.0f);
    t = t - floor_t; // now t in [0,1) (the -1 above handles negatives).
    if (t >= 1.0f)
        t -= 1.0f; // guard the exact-integer edge.
    if (t < 0.0f)
        t = 0.0f; // numerical safety.

    // Angle for one full day. cos(2*pi*t): +1 at t=0 (midnight), -1 at t=0.5 (noon).
    const float c = dm::Cos(dm::kTwoPi * t);

    // Diurnal: 0.5*(1 - cos) -> 0 at midnight, 1 at noon.
    // Nocturnal: 0.5*(1 + cos) -> 1 at midnight, 0 at noon (the inverse).
    // amplitude scales day/night contrast (1.0 = default curve, byte-identical; >1 sharpens,
    // <1 flattens). Clamp keeps activity in [0,1].
    const float a = amplitude * (nocturnal ? 0.5f * (1.0f + c) : 0.5f * (1.0f - c));
    return CircadianClamp01(a);
}

// RunCircadianOnTick: write every participating creature's activity from the day clock.
// id-ordered. `time_of_day01` is the caller-supplied fraction of the day in [0,1] (0/1 =
// midnight, 0.5 = noon). Returns participation stats. Empty roster -> pure no-op.
inline CircadianStats
RunCircadianOnTick(entt::registry& reg, float time_of_day01, float amplitude = 1.0f) {
    CircadianStats stats;

    auto view = reg.view<Comp::CircadianComponent>();

    // id-ordered participant list (deterministic traversal). The per-creature writes are
    // independent (each reads only its own component + the shared time input), so ordering
    // cannot change the outcome, but we sort to keep traversal canonical.
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    for (auto e : ents) {
        auto& cc = view.get<Comp::CircadianComponent>(e);
        const bool nocturnal = cc.nocturnal != 0;
        cc.activity = CircadianActivity(time_of_day01, nocturnal, amplitude);
        ++stats.participants;
        if (nocturnal) {
            ++stats.nocturnal;
        } else {
            ++stats.diurnal;
        }
    }

    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/CoreComponents.h"
#include "components/TerritoryComponents.h"
#include "core/DeterministicMath.h"
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry: territory+30). This system is deterministic and
// consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kTerritorySeedOffset = 30ull;

// How strongly the homing bias grows per world-unit of OVERSHOOT past the territory edge.
// At an overshoot of 1/kTerritoryHomingGain world units the bias magnitude reaches its cap.
// Tuned gentle so the pull blends with (rather than overrides) normal wander/seek movement.
inline constexpr float kTerritoryHomingGain = 0.25f;

// Maximum homing-bias magnitude (m/s-scaled steer), so a creature stranded very far from home
// produces a bounded, blendable pull rather than an explosive velocity.
inline constexpr float kTerritoryMaxBias = 3.0f;

struct TerritoryStats {
    int participants = 0; // creatures carrying a TerritoryComponent that took part
    int claimed = 0;      // creatures that claimed their home this tick (first sighting)
    int homing = 0;       // creatures with a non-zero homing bias this tick (outside radius)
};

// RunTerritoryOnTick: claim home on first sight, then emit each creature's homing wish bias.
// id-ordered, two-phase. `tick` is accepted for signature parity with the other tick systems
// (this system is rng-free, so it does not consume the tick, but keeping it keeps the wire
// site uniform).
inline TerritoryStats RunTerritoryOnTick(entt::registry& reg, std::uint64_t /*tick*/ = 0) {
    TerritoryStats stats;

    auto view = reg.view<Comp::TerritoryComponent, Comp::TransformComponent>();

    // id-ordered participant list (deterministic traversal).
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    // ---- First pass: claim home where needed and snapshot state. ----
    struct Snap {
        entt::entity e;
        float x, z;   // current position
        float hx, hz; // claimed home
        float radius;
    };
    std::vector<Snap> snap;
    snap.reserve(ents.size());
    for (auto e : ents) {
        auto& terr = view.get<Comp::TerritoryComponent>(e);
        const auto& tf = view.get<Comp::TransformComponent>(e);
        if (terr.established == 0) {
            // First sighting: claim the CURRENT position as home.
            terr.home_x = tf.position.x;
            terr.home_z = tf.position.z;
            terr.established = 1;
            ++stats.claimed;
        }
        snap.push_back({e, tf.position.x, tf.position.z, terr.home_x, terr.home_z, terr.radius});
    }
    stats.participants = static_cast<int>(snap.size());

    // ---- Second pass: compute and write homing bias from the snapshot. ----
    for (const Snap& s : snap) {
        auto& bias = reg.get_or_emplace<Comp::TerritoryBiasComponent>(s.e);

        const float dx = s.hx - s.x; // vector TOWARD home
        const float dz = s.hz - s.z;
        const float dist = dm::Sqrt(dx * dx + dz * dz);

        // Inside the home range (or exactly at the edge): no homing pull.
        const float radius = s.radius > 0.0f ? s.radius : 0.0f;
        if (dist <= radius || dist <= 1.0e-6f) {
            bias.wish_x = 0.0f;
            bias.wish_z = 0.0f;
            continue;
        }

        // Outside: magnitude rises with the OVERSHOOT past the edge, capped.
        const float overshoot = dist - radius;        // > 0 here
        float mag = overshoot * kTerritoryHomingGain; // grows linearly with how far it strayed
        if (mag > kTerritoryMaxBias)
            mag = kTerritoryMaxBias;

        // Unit vector toward home * magnitude. dist > radius >= 0 and dist > 1e-6, safe divide.
        const float inv = mag / dist;
        bias.wish_x = dx * inv;
        bias.wish_z = dz * inv;
        ++stats.homing;
    }

    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/PackHunterComponents.h"
#include "core/DeterministicMath.h"
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

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

} // namespace LegacyCreatureTickOracle

#include "components/CoreComponents.h"
#include "components/MigratoryComponents.h"
#include "core/DeterministicMath.h"
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry:... herd-alarm+26, migration+32). This system is
// deterministic and consumes NO rng; the offset is recorded for collision-avoidance only.
inline constexpr std::uint64_t kMigrationSeedOffset = 32ull;

// Migration target path: a large ellipse centred on the world origin. The half-extents (world
// units) set how far the herds range over a year. Authored generously so the seasonal target
// clearly MOVES (different season01 -> visibly different target).
inline constexpr float kMigrationCenterX = 0.0f;
inline constexpr float kMigrationCenterZ = 0.0f;
inline constexpr float kMigrationRadiusX = 600.0f;
inline constexpr float kMigrationRadiusZ = 600.0f;

// Below this the seasonal drive is floored to exactly 0 (mid-season settle band) so a settled
// creature emits a clean zero wish and stays effectively gated.
inline constexpr float kMigrationDriveFloor = 0.05f;

struct MigrationTarget {
    float x = 0.0f;
    float z = 0.0f;
};

struct MigrationStats {
    int participants = 0;  // creatures carrying a MigratoryComponent that took part
    int driven = 0;        // creatures with a non-zero drive this tick (actively migrating)
    float target_x = 0.0f; // the seasonal target this tick (telemetry / test hook)
    float target_z = 0.0f;
    float drive = 0.0f; // the seasonal drive this tick (same for all participants)
};

[[nodiscard]] inline float MigrationClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// Wrap an arbitrary season phase into [0,1) deterministically (float floor via int truncation).
// Keeps the year a circle for callers that pass an unwrapped phase.
[[nodiscard]] inline float MigrationWrap01(float season01) {
    float s = season01 - static_cast<float>(static_cast<std::int64_t>(season01));
    if (s < 0.0f)
        s += 1.0f; // map negative phase into [0,1)
    return s;
}

// The seasonal migration TARGET for a given (wrapped) season phase. A point on the year-ellipse;
// theta sweeps a full turn over a year, so the target travels a closed loop and a full year
// returns to the start. Pure function of season01 (libm-free Cos/Sin).
[[nodiscard]] inline MigrationTarget MigrationTargetAt(float season01) {
    const float s = MigrationWrap01(season01);
    const float theta = dm::kTwoPi * s;
    MigrationTarget t;
    t.x = kMigrationCenterX + kMigrationRadiusX * dm::Cos(theta);
    t.z = kMigrationCenterZ + kMigrationRadiusZ * dm::Sin(theta);
    return t;
}

// The seasonal DRIVE for a given (wrapped) season phase: peaks at the four season transitions
// (season01 = 0, 0.25, 0.5, 0.75) and falls to ~0 at the quarter midpoints. Built from
// |cos(4*pi*season01)| (libm-free), then floored so the mid-season residue is a clean 0. In
// [0,1]. Pure function of season01.
[[nodiscard]] inline float MigrationDriveAt(float season01) {
    const float s = MigrationWrap01(season01);
    // |cos(4*pi*s)|: cos(4*pi*s) is +-1 at s = 0,0.25,0.5,0.75 (the four season TRANSITIONS) and
    // 0 at the quarter midpoints (0.125,0.375,...). Taking |.| gives a  PEAKS (==1) at
    // every transition and dips to 0 at every midpoint — exactly the spring/autumn-style pushes.
    float w = dm::Cos(dm::kTwoPi * 2.0f * s); // cos(4*pi*s): +-1 at transitions, 0 at midpoints
    if (w < 0.0f)
        w = -w; // |cos| -> [0,1], peaks (==1) AT the transitions
    w = MigrationClamp01(w);
    if (w < kMigrationDriveFloor)
        w = 0.0f; // floor the mid-season residue to a clean 0
    return w;
}

// RunMigrationOnTick: for each migratory creature, write a wish toward the current seasonal
// target scaled by the seasonal drive. id-ordered, deterministic. season01 is a plain input.
inline MigrationStats RunMigrationOnTick(entt::registry& reg, float season01) {
    MigrationStats stats;

    auto view = reg.view<Comp::MigratoryComponent, Comp::TransformComponent>();

    // id-ordered participant list (deterministic write order).
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty())
        return stats; // empty roster -> pure no-op.

    // Seasonal target + drive are shared by all participants this tick (pure functions of the
    // season phase). Compute once.
    const MigrationTarget target = MigrationTargetAt(season01);
    const float drive = MigrationDriveAt(season01);

    stats.participants = static_cast<int>(ents.size());
    stats.target_x = target.x;
    stats.target_z = target.z;
    stats.drive = drive;

    for (auto e : ents) {
        const auto& tf = view.get<Comp::TransformComponent>(e);
        auto& mig = view.get<Comp::MigratoryComponent>(e);

        mig.drive = drive;

        if (drive <= 0.0f) {
            // Settled mid-season: emit an exactly-zero wish (no migratory pull).
            mig.wish_x = 0.0f;
            mig.wish_z = 0.0f;
            continue;
        }

        // Direction toward the seasonal target, normalised, then scaled by the drive.
        const float dx = target.x - tf.position.x;
        const float dz = target.z - tf.position.z;
        const float d = dm::Sqrt(dx * dx + dz * dz);
        if (d > 1.0e-5f) {
            const float inv = drive / d; // unit direction * drive
            mig.wish_x = dx * inv;
            mig.wish_z = dz * inv;
            ++stats.driven;
        } else {
            // Already at the target: nowhere to migrate, so no wish (but still "driven" season).
            mig.wish_x = 0.0f;
            mig.wish_z = 0.0f;
        }
    }

    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/CreatureComponents.h" // CreatureComponent (starvation + carcass seam)
#include "components/MortalComponents.h"   // MortalComponent (the opt-in)
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

// Components live in the capital-L namespace (engine convention).
namespace Comp = ::Luminumbra::Components;

// Reserved seed-stream offset for the lifespan track. Death here is a pure
// DETERMINISTIC threshold (no stochastic culling), so this is recorded for
// collision-avoidance but intentionally never consumed.
inline constexpr std::uint64_t kLifespanSeedOffset = 22ull;

// Hunger at/above this is fatal starvation (hunger is 0 sated .. 1 starving).
inline constexpr float kStarvationHunger = 1.0f;

// Telemetry / sub-hash for the lifespan tick.
struct LifespanStats {
    int aged = 0; // mortal entities advanced this tick (alive before the step)
    int died = 0; // entities newly MARKED dead this tick (old age OR starvation)
};

// Advance the lifespan of every opted-in entity by ONE fixed tick. Pure function
// of registry state. id-ordered so the death-marking order (and thus any derived
// telemetry) is deterministic. No creation/destruction -> no two-phase needed,
// no view invalidation. Safe to call every tick; a no-op when no entity opts in.
inline LifespanStats RunLifespanOnTick(entt::registry& reg, std::uint64_t /*tick*/ = 0) {
    LifespanStats stats;

    auto view = reg.view<Comp::MortalComponent>();

    // id-ordered traversal so death-marking is deterministic / order-stable.
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return static_cast<std::uint64_t>(entt::to_integral(a)) <
               static_cast<std::uint64_t>(entt::to_integral(b));
    });

    for (auto e : ents) {
        auto& mortal = view.get<Comp::MortalComponent>(e);

        // A latched-dead entity is inert: it neither ages nor dies again, so its
        // age and death flag stay stable across run==replay.
        if (mortal.dead != 0) {
            continue;
        }

        ++stats.aged;

        // Age (saturating so a long-lived entity can't wrap its clock).
        if (mortal.age_ticks < 0xFFFFFFFFu) {
            ++mortal.age_ticks;
        }

        // Old-age death: reaching lifespan is fatal.
        bool dies = mortal.age_ticks >= mortal.lifespan_ticks;

        // Starvation death: only if this entity is also a creature (the brain's
        // hunger model lives there). Reading hunger >= 1.0 as "maxed out".
        if (!dies) {
            if (auto* cr = reg.try_get<Comp::CreatureComponent>(e)) {
                if (cr->hunger >= kStarvationHunger) {
                    dies = true;
                }
            }
        }

        if (dies) {
            mortal.dead = 1;
            // Reuse the existing carcass seam so the brain / markers treat the
            // corpse as inert (stops deciding/moving; predators ignore it).
            if (auto* cr = reg.try_get<Comp::CreatureComponent>(e)) {
                cr->eaten = true;
            }
            ++stats.died;
        }
    }

    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/CoreComponents.h"     // TransformComponent
#include "components/CreatureComponents.h" // CreatureComponent
#include "components/GrazeableComponent.h" // GrazeableComponent (opt-in)
#include "core/DeterministicMath.h"        // Sqrt
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

// Components live in the capital-L namespace (engine convention).
namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset for the wildlife-foliage track. The flow is PURE (no rng),
// so this is recorded for collision-avoidance but intentionally never consumed.
inline constexpr std::uint64_t kWildlifeFoliageSeedOffset = 23ull;

// Geometry + rates. All are constant (integer-valued) floats so the per-tick arithmetic
// is bit-stable. Biomass lives in [0,1].
inline constexpr float kGrazeRadius = 3.0f; // a creature grazes plants within this (m)
inline constexpr float kGrazeRadiusSq = kGrazeRadius * kGrazeRadius;
inline constexpr float kGrazePerCreature = 0.05f; // biomass removed per grazing creature/tick
inline constexpr float kRegrowPerTick = 0.01f;    // biomass regained per tick when ungrazed
inline constexpr float kFeedPerGraze = 0.02f;     // hunger reduced on a creature per plant fed
inline constexpr float kBiomassMin = 0.0f;
inline constexpr float kBiomassMax = 1.0f;

// Full-control tuning: fields default to the constants above, so a default-constructed
// WildlifeFoliageTuning is byte-identical. Threaded via the optional Run arg; resolved from
// SystemConfig sim.wildlife_foliage when that system is enabled.
struct WildlifeFoliageTuning {
    float graze_radius = kGrazeRadius;
    float graze_per_creature = kGrazePerCreature;
    float regrow_per_tick = kRegrowPerTick;
    float feed_per_graze = kFeedPerGraze;
};

inline float clampBiomass(float v) {
    if (v < kBiomassMin)
        return kBiomassMin;
    if (v > kBiomassMax)
        return kBiomassMax;
    return v;
}

// Telemetry / sub-hash for the wildlife-foliage tick.
struct WildlifeFoliageStats {
    int grazers = 0;              // non-predator creatures considered this tick
    int plants = 0;               // grazeable plants considered this tick
    int grazed_plants = 0;        // plants that were grazed (had >=1 grazer in range) this tick
    float biomass_removed = 0.0f; // total biomass removed by grazing this tick
    float biomass_regrown = 0.0f; // total biomass regrown this tick
};

// Advance the wildlife-foliage coupling by ONE fixed tick. Pure function of registry
// state. id-ordered; graze pressure is summed per plant (order-independent) and feed is
// summed per creature, then both are applied in id-ordered passes.
inline WildlifeFoliageStats RunWildlifeFoliageOnTick(entt::registry& reg,
                                                     std::uint64_t tick,
                                                     const WildlifeFoliageTuning& tuning = {}) {
    WildlifeFoliageStats stats;
    const float graze_radius_sq = tuning.graze_radius * tuning.graze_radius;

    // --- 1. Snapshot NON-PREDATOR creature positions, id-ordered (order-stable). ---
    struct Grazer {
        entt::entity e;
        float x, z;
    };
    std::vector<Grazer> grazers;
    {
        auto cview = reg.view<const Comp::CreatureComponent, const Comp::TransformComponent>();
        std::vector<entt::entity> cents(cview.begin(), cview.end());
        std::sort(cents.begin(), cents.end(), [](entt::entity a, entt::entity b) {
            return static_cast<std::uint64_t>(entt::to_integral(a)) <
                   static_cast<std::uint64_t>(entt::to_integral(b));
        });
        grazers.reserve(cents.size());
        for (auto e : cents) {
            const auto& cr = cview.get<const Comp::CreatureComponent>(e);
            if (cr.is_predator || cr.eaten)
                continue; // only live herbivores graze
            const auto& tf = cview.get<const Comp::TransformComponent>(e);
            grazers.push_back({e, tf.position.x, tf.position.z});
            ++stats.grazers;
        }
    }

    // --- 2. Gather grazeable plants, id-ordered. ---
    auto pview = reg.view<Comp::GrazeableComponent, const Comp::TransformComponent>();
    std::vector<entt::entity> pents(pview.begin(), pview.end());
    std::sort(pents.begin(), pents.end(), [](entt::entity a, entt::entity b) {
        return static_cast<std::uint64_t>(entt::to_integral(a)) <
               static_cast<std::uint64_t>(entt::to_integral(b));
    });

    // Per-creature feed accumulator, indexed parallel to `grazers` (summed so applying it
    // afterward is order-independent).
    std::vector<float> feed(grazers.size(), 0.0f);

    // --- 3. id-ordered pass over plants: count grazers in range, graze or regrow. ---
    for (auto pe : pents) {
        ++stats.plants;
        auto& gz = pview.get<Comp::GrazeableComponent>(pe);
        const auto& ptf = pview.get<const Comp::TransformComponent>(pe);

        // Count how many grazers are within range (trampling pressure) and remember which
        // (to feed them). Summing the count first keeps the bite order-independent.
        // A depleted plant (no biomass) still gets trampled but feeds NO ONE -- gate the
        // feed accrual on having biomass to give, so creatures can't eat from bare ground.
        const bool has_biomass = clampBiomass(gz.biomass) > 0.0f;
        int n_in_range = 0;
        for (std::size_t gi = 0; gi < grazers.size(); ++gi) {
            const float dx = grazers[gi].x - ptf.position.x;
            const float dz = grazers[gi].z - ptf.position.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 <= graze_radius_sq) {
                ++n_in_range;
                if (has_biomass)
                    feed[gi] += tuning.feed_per_graze; // only a living plant feeds
            }
        }

        if (n_in_range > 0) {
            // Graze/trample: biomass drops proportional to the number of grazers (so a
            // herd flattens a patch faster). Constant-step float arithmetic.
            const float removed_req = tuning.graze_per_creature * static_cast<float>(n_in_range);
            const float before = clampBiomass(gz.biomass);
            const float after = clampBiomass(before - removed_req);
            gz.biomass = after;
            gz.last_grazed_tick = static_cast<std::uint32_t>(tick);
            stats.biomass_removed += (before - after);
            ++stats.grazed_plants;
        } else {
            // Ungrazed: regrow slowly toward the cap.
            const float before = clampBiomass(gz.biomass);
            const float after = clampBiomass(before + tuning.regrow_per_tick);
            gz.biomass = after;
            stats.biomass_regrown += (after - before);
        }
    }

    // --- 4. Apply accumulated feed to creatures, id-ordered (lower hunger). ---
    // `grazers` is already id-ordered, so this pass is deterministic. Hunger clamps to
    // [0,1] (0 = sated). This only RUNS when grazeable plants exist (feed is otherwise
    // all-zero), preserving the no-op-when-no-plants gate.
    for (std::size_t gi = 0; gi < grazers.size(); ++gi) {
        if (feed[gi] <= 0.0f)
            continue;
        auto& cr = reg.get<Comp::CreatureComponent>(grazers[gi].e);
        float h = cr.hunger - feed[gi];
        if (h < 0.0f)
            h = 0.0f;
        cr.hunger = h;
    }

    return stats;
}

} // namespace LegacyCreatureTickOracle

#include "components/AlarmComponents.h"
#include "components/CircadianComponents.h" //  v2: circadian activity
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/PackHunterComponents.h"
#include "components/ThirstComponents.h"           //  v2: thirst state
#include "persistence/WorldPersistenceRoundtrip.h" // Persistence::StableChecksum
#include <algorithm>
#include <cstdint>
#include <entt/entt.hpp>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
namespace LegacyCreatureTickOracle {
using namespace ::luminumbra::ai;

// Computes the id-ordered ecology sub-hash over every CreatureComponent-bearing
// entity in `registry`, using only the frozen legacy v2 text recipe. Empty is neutral.
inline std::string ComputeEcologySubHash(const entt::registry& registry,
                                         bool /*active_regions*/ = false) {
    namespace Comp = ::Luminumbra::Components;

    // id-sorted creature set -> stable order independent of view iteration order
    // (exactly the gtest Snapshot's sort key: entt::to_integral ascending).
    auto view = registry.view<const Comp::CreatureComponent>();
    std::vector<entt::entity> es(view.begin(), view.end());
    if (es.empty()) {
        return {}; // additivity-neutral: empty roster == empty sub-hash
    }
    std::sort(es.begin(), es.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    // Canonical text in the same field set + order as the gtest Snapshot. Full
    // float precision (17 sig digits) so no projected bit is lost before the
    // StableChecksum folds it. Per-component presence is encoded so a creature
    // with a genome and one without are unambiguously distinguished.
    // sub-hash v2 — energy, species_id, the heritable
    // SENSORY genes, thirst, and circadian activity join the projection.
    // Divergence in any of these was previously invisible to the oracle until it
    // flipped an action; now it localizes under `ecology` directly. The version
    // literal flips v1 -> v2 (one deliberate populated-golden transition); the
    // empty roster still returns "" (additivity-neutral, canonical untouched).
    std::ostringstream bytes;
    bytes << "ecology:v2:" << es.size() << ':' << std::setprecision(17);
    for (auto e : es) {
        const auto& tf = registry.get<Comp::TransformComponent>(e);
        const auto& cr = registry.get<Comp::CreatureComponent>(e);
        bytes << tf.position.x << ',' << tf.position.y << ',' << tf.position.z << ',' << cr.wish_x
              << ',' << cr.wish_z << ',' << cr.hunger << ',' << cr.stamina << ','
              << static_cast<int>(cr.eaten) << ',' << cr.energy << ',' << cr.species_id;
        if (const auto* gn = registry.try_get<Comp::CreatureGenomeComponent>(e)) {
            bytes << ",g:" << gn->move_speed << ',' << gn->generation << ',' << gn->age_ticks << ','
                  << gn->vision_range << ',' << gn->hearing_range << ',' << gn->vision_cos_half_fov;
        }
        if (const auto* al = registry.try_get<Comp::AlarmComponent>(e)) {
            bytes << ",a:" << al->level;
        }
        if (const auto* pk = registry.try_get<Comp::PackHunterComponent>(e)) {
            bytes << ",p:" << pk->coord_x << ',' << pk->coord_z << ','
                  << static_cast<int>(pk->in_pack);
        }
        if (const auto* th = registry.try_get<Comp::ThirstComponent>(e)) {
            bytes << ",t:" << th->thirst << ',' << static_cast<int>(th->drinking);
        }
        if (const auto* cc = registry.try_get<Comp::CircadianComponent>(e)) {
            bytes << ",c:" << cc->activity << ',' << static_cast<int>(cc->nocturnal);
        }
        bytes << ';';
    }
    return ::Luminumbra::Persistence::StableChecksum(bytes.str());
}

} // namespace LegacyCreatureTickOracle

#include <bit>
#include <type_traits>
namespace LegacyCreatureTickOracle {
// Deterministic test roster: actual births, catches, drinking, scavenging and pack work.
inline entt::entity AddCreature(entt::registry& reg, float x, float z, bool predator = false) {
    const auto e = reg.create();
    reg.emplace<Comp::TransformComponent>(e).position = ::Luminumbra::Vec3(x, 0, z);
    auto& cr = reg.emplace<Comp::CreatureComponent>(e);
    cr.species_id = Comp::CreatureSpeciesId16("fixture_creature");
    cr.is_predator = predator;
    cr.hunger = predator ? .8f : .05f;
    return e;
}
inline void MakeRoster(entt::registry& reg, bool foliage = true) {
    const auto mother = AddCreature(reg, 0, 0);
    const auto father = AddCreature(reg, 1, 0);
    for (const auto e : {mother, father}) {
        reg.get<Comp::CreatureComponent>(e).move_speed = 0;
        auto& gn = reg.emplace<Comp::CreatureGenomeComponent>(e);
        gn.age_ticks = 120;
        gn.female = e == mother;
        reg.emplace<Comp::ThirstComponent>(e).thirst = .7f;
        reg.emplace<Comp::MortalComponent>(e);
    }
    const auto predator = AddCreature(reg, 25, 0, true);
    const auto ally = AddCreature(reg, 26, 1, true);
    const auto caught = AddCreature(reg, 25.1f, 0);
    reg.emplace<Comp::DecayComponent>(caught);
    for (const auto e : {predator, ally})
        reg.emplace<Comp::PackHunterComponent>(e);
    for (const auto x : {20.0f, 18.0f}) {
        const auto e = AddCreature(reg, x, 0);
        reg.emplace<Comp::AlarmComponent>(e);
        reg.emplace<Comp::CircadianComponent>(e);
        reg.emplace<Comp::TerritoryComponent>(e);
        reg.emplace<Comp::MigratoryComponent>(e);
    }
    const auto scavenger = AddCreature(reg, 200, 0, true);
    reg.emplace<Comp::ScavengerComponent>(scavenger);
    const auto corpse = AddCreature(reg, 200.5f, 0);
    reg.get<Comp::CreatureComponent>(corpse).eaten = true;
    reg.emplace<Comp::DecayComponent>(corpse);
    reg.emplace<Comp::MortalComponent>(corpse); // pins the historical eaten/age rule too.
    const auto water = reg.create();
    reg.emplace<Comp::TransformComponent>(water);
    reg.emplace<Comp::WaterHoleComponent>(water);
    // Stored references also exercise a handle that is destroyed and reused at tick 50.
    auto& senses = reg.emplace<Comp::SensesComponent>(corpse);
    senses.nearest_food_source = static_cast<entt::entity>(6);
    senses.nearest_water_source = water;
    senses.nearest_threat = predator;
    senses.threat_distance = 175.5f;
    if (foliage) {
        const auto plant = reg.create();
        reg.emplace<Comp::TransformComponent>(plant);
        reg.emplace<Comp::GrazeableComponent>(plant);
    }
}
inline void SurvivalBlend(entt::registry& reg) {
    for (auto e : reg.view<Comp::CreatureComponent>()) {
        auto& cr = reg.get<Comp::CreatureComponent>(e);
        if (cr.last_action == static_cast<int>(::luminumbra::ai::CreatureAction::Drink)) {
            if (const auto* th = reg.try_get<Comp::ThirstComponent>(e)) {
                cr.wish_x += th->wish_x;
                cr.wish_z += th->wish_z;
            }
        }
        if (const auto* sc = reg.try_get<Comp::ScavengerComponent>(e)) {
            cr.wish_x += sc->wish_x;
            cr.wish_z += sc->wish_z;
        }
    }
}
inline void Tick(entt::registry& reg, std::uint64_t tick, std::uint64_t seed) {
    constexpr float dt = static_cast<float>(1.0 / 30.0);
    LegacyCreatureTickOracle::RunCreatureBrainSystemOnTick(reg, dt);
    LegacyCreatureTickOracle::RunMateSeekingOnTick(reg);
    LegacyCreatureTickOracle::RunSteeringConsumerOnTick(reg);
    LegacyCreatureTickOracle::RunThirstOnTick(reg, dt);
    LegacyCreatureTickOracle::RunScavengingOnTick(reg, tick);
    SurvivalBlend(reg);
    LegacyCreatureTickOracle::RunMatingResolveOnTick(reg, tick, seed);
    LegacyCreatureTickOracle::RunWildlifeFoliageOnTick(reg, tick);
    LegacyCreatureTickOracle::RunLifespanOnTick(reg, tick);
    LegacyCreatureTickOracle::RunHerdAlarmOnTick(reg, dt);
    LegacyCreatureTickOracle::RunDecompositionOnTick(reg, tick);
    LegacyCreatureTickOracle::RunCircadianOnTick(reg, static_cast<float>(tick % 36000) / 36000.0f);
    LegacyCreatureTickOracle::RunTerritoryOnTick(reg, tick);
    LegacyCreatureTickOracle::RunPredatorPackOnTick(reg, tick);
    LegacyCreatureTickOracle::RunMigrationOnTick(reg,
                                                 static_cast<float>(tick % 108000) / 108000.0f);
}
// Explicit fields, presence and handles; no object padding or production serializer.
template<class... T>
inline void Values(std::ostringstream& out, T... values) {
    auto one = [&](auto value) {
        if constexpr (std::is_same_v<decltype(value), float>)
            out << std::bit_cast<std::uint32_t>(value);
        else
            out << +value;
        out << ',';
    };
    (one(values), ...);
}
inline std::string Fields(const entt::registry& reg, entt::entity e) {
    std::ostringstream out;
    Values(out, entt::to_integral(e));
    out << "TransformComponent:";
    if (const auto* v = reg.try_get<const Comp::TransformComponent>(e))
        Values(out,
               v->position.x,
               v->position.y,
               v->position.z,
               v->scale.x,
               v->scale.y,
               v->scale.z,
               v->rotation.x,
               v->rotation.y,
               v->rotation.z,
               v->rotation.w);
    else
        out << "absent";
    out << "CreatureComponent:";
    if (const auto* v = reg.try_get<const Comp::CreatureComponent>(e))
        Values(out,
               v->is_predator,
               v->species_id,
               v->hunger,
               v->stamina,
               v->energy,
               v->move_speed,
               v->wish_x,
               v->wish_z,
               v->last_action,
               v->eaten);
    else
        out << "absent";
    out << "CreatureGenomeComponent:";
    if (const auto* v = reg.try_get<const Comp::CreatureGenomeComponent>(e))
        Values(out,
               v->move_speed,
               v->vigilance,
               v->hunger_threshold,
               v->size_scale,
               v->vision_cos_half_fov,
               v->vision_range,
               v->hearing_range,
               v->age_ticks,
               v->reproduce_cooldown,
               v->generation,
               v->female,
               v->courting_ticks);
    else
        out << "absent";
    out << "ThirstComponent:";
    if (const auto* v = reg.try_get<const Comp::ThirstComponent>(e))
        Values(out, v->thirst, v->wish_x, v->wish_z, v->drinking, v->water_proximity);
    else
        out << "absent";
    out << "ScavengerComponent:";
    if (const auto* v = reg.try_get<const Comp::ScavengerComponent>(e))
        Values(out, v->wish_x, v->wish_z, v->feeding);
    else
        out << "absent";
    out << "AlarmComponent:";
    if (const auto* v = reg.try_get<const Comp::AlarmComponent>(e))
        Values(out, v->level, v->alarmed);
    else
        out << "absent";
    out << "DecayComponent:";
    if (const auto* v = reg.try_get<const Comp::DecayComponent>(e))
        Values(out,
               v->decay_ticks,
               v->decay_duration,
               v->nutrient_released_milli,
               v->fully_decomposed);
    else
        out << "absent";
    out << "MortalComponent:";
    if (const auto* v = reg.try_get<const Comp::MortalComponent>(e))
        Values(out, v->age_ticks, v->lifespan_ticks, v->dead);
    else
        out << "absent";
    out << "CircadianComponent:";
    if (const auto* v = reg.try_get<const Comp::CircadianComponent>(e))
        Values(out, v->activity, v->nocturnal);
    else
        out << "absent";
    out << "TerritoryComponent:";
    if (const auto* v = reg.try_get<const Comp::TerritoryComponent>(e))
        Values(out, v->home_x, v->home_z, v->radius, v->established);
    else
        out << "absent";
    out << "TerritoryBiasComponent:";
    if (const auto* v = reg.try_get<const Comp::TerritoryBiasComponent>(e))
        Values(out, v->wish_x, v->wish_z);
    else
        out << "absent";
    out << "PackHunterComponent:";
    if (const auto* v = reg.try_get<const Comp::PackHunterComponent>(e))
        Values(out, v->coord_x, v->coord_z, v->in_pack);
    else
        out << "absent";
    out << "MigratoryComponent:";
    if (const auto* v = reg.try_get<const Comp::MigratoryComponent>(e))
        Values(out, v->drive, v->wish_x, v->wish_z);
    else
        out << "absent";
    out << "GrazeableComponent:";
    if (const auto* v = reg.try_get<const Comp::GrazeableComponent>(e))
        Values(out, v->biomass, v->last_grazed_tick);
    else
        out << "absent";
    out << "WaterHoleComponent:";
    if (const auto* v = reg.try_get<const Comp::WaterHoleComponent>(e))
        Values(out, v->radius);
    else
        out << "absent";
    out << "CreatureRegionComponent:";
    if (const auto* v = reg.try_get<const Comp::CreatureRegionComponent>(e))
        Values(out, v->persistent_id, v->owner_x, v->owner_z, v->region_stamp);
    else
        out << "absent";
    out << "CreatureCoarseComponent:";
    if (const auto* v = reg.try_get<const Comp::CreatureCoarseComponent>(e)) {
        Values(out,
               static_cast<std::uint8_t>(v->tier),
               static_cast<std::uint8_t>(v->transition),
               v->x_mm,
               v->y_mm,
               v->z_mm,
               v->hunger_ppm,
               v->thirst_ppm,
               v->age_cadences,
               v->reproduce_cadences,
               v->reproduce_cooldown_cadences,
               v->courting_cadences,
               v->travel_pending,
               v->destination_x,
               v->destination_z);
        for (const auto& need : v->needs) {
            out << need.name.size() << ':' << need.name;
            Values(out, need.pressure_ppm);
        }
    } else
        out << "absent";
    out << "CreatureControllerComponent:";
    if (const auto* v = reg.try_get<const Comp::CreatureControllerComponent>(e))
        Values(out, v->physics_controlled);
    else
        out << "absent";
    out << "CreatureLocomotionStateComponent:";
    if (const auto* v = reg.try_get<const Comp::CreatureLocomotionStateComponent>(e))
        Values(out, v->velocity_x, v->velocity_y, v->velocity_z, v->grounded);
    else
        out << "absent";
    out << "SensesComponent:";
    if (const auto* v = reg.try_get<const Comp::SensesComponent>(e))
        Values(out,
               entt::to_integral(v->nearest_food_source),
               entt::to_integral(v->nearest_water_source),
               entt::to_integral(v->nearest_threat),
               v->threat_distance);
    else
        out << "absent";
    out << "PerceptionComponent:";
    if (const auto* v = reg.try_get<const Comp::PerceptionComponent>(e))
        Values(out,
               v->vision_cos_half_fov,
               v->vision_range,
               v->facing_x,
               v->facing_z,
               v->ear.range,
               v->ear.peak_pitch,
               v->ear.bandwidth,
               v->ear.gain,
               v->ear.threshold,
               v->faction);
    else
        out << "absent";
    return out.str();
}
inline std::string Fields(const entt::registry& reg) {
    std::vector<entt::entity> entities;
    for (auto e : reg.view<const Comp::TransformComponent>())
        entities.push_back(e);
    std::sort(entities.begin(), entities.end(), [](auto a, auto b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    std::string result;
    for (const auto e : entities)
        result += Fields(reg, e) + "\n";
    return result;
}
} // namespace LegacyCreatureTickOracle
