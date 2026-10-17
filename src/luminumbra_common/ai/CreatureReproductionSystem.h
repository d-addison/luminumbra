#pragma once
#include "../systems/RegionConsumerGate.h"
#include "../world/RegionRng.h"
#include <functional>
#include <limits>

// Creature evolution: CREATURE EVOLUTION via SEXUAL reproduction. Makes the ecology generational
// AND requires a MALE + a FEMALE to find each other and COURT before a baby is born:
//   * a "ready" creature (mature, well-fed, healthy, off cooldown) SEEKS the nearest ready
//     opposite-sex mate (RunMateSeekingOnTick — runs BEFORE the physics bridge so the steer
//     becomes real movement),
//   * once a ready pair is adjacent for the courtship duration, ONE offspring is born from a
//     deterministic blend-crossover of BOTH parents' genomes (RunMatingResolveOnTick — runs
//     AFTER the bridge, on the settled positions),
//   * caught prey (CreatureComponent.eaten) die and never breed -> natural selection.
//
// DETERMINISM (sim path -> world_hash once wired): id-ordered traversal; the per-birth RNG is
// seeded ONLY from integers (offset + parent ids + tick) -> no wall-clock/std::random, the
// Gaussian mutation is the libm-free Irwin-Hall one. Mate search uses a PRE snapshot so the
// result is order-independent; births are collected then created after the read pass so the
// view is never invalidated. run==replay holds.
//
// GATING: a creature only mates if it carries a CreatureGenomeComponent (the opt-in). A world
// whose creatures carry NO genome — and any world with NO creatures — does nothing, so the
// canonical NetworkStateHash baseline stays byte-identical.
//
// SEED OFFSET REGISTRY: wind+11, weather+12/13, aether+14, plant+15 taken; CREATURE
// REPRODUCTION CLAIMS +16. Recorded here and at the GameSession wire site.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "CreatureBrain.h" // CreatureAction (skip mate-seeking while fleeing)
#include "CreatureGenome.h"
#include "CreatureSpeciesRegistry.h"
#include "UtilityAI.h" // utility_clamp01

#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/InstinctComponents.h" // PerceptionComponent ( sensory phenotype)
#include "../core/DeterministicMath.h"
#include "../core/DeterministicRng.h"

namespace luminumbra::ai {

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

struct CreatureBirthProposal {
    std::uint64_t mother_id = 0, father_id = 0;
    ::Luminumbra::world::RegionKey mother_owner{};
    std::uint64_t seed = 0;
    CreatureGenome genome;
    float x = 0, y = 0, z = 0;
    std::uint32_t generation = 0;
    bool female = false;
    std::uint16_t species_id = 0;
};
using CreatureBirthAdmission = std::function<bool(const CreatureBirthProposal&)>;

// Ordinary births validate the destination before allocating a shared ID.
inline bool
AdmitFullCreatureBirthDestination(const ::Luminumbra::world::ActiveRegionLedger& ledger,
                                  const std::vector<::Luminumbra::world::RegionKey>& due,
                                  const ::Luminumbra::Vec3& position) {
    using namespace ::Luminumbra::world;
    if (!ActiveRegionLedger::valid_anchor(position))
        return false;
    const auto key = ActiveRegionLedger::region_at(position);
    const auto it = ledger.records().find(key);
    return it != ledger.records().end() && it->second.state == RegionState::Active &&
           std::find(due.begin(), due.end(), key) != due.end();
}

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

inline entt::entity CreateCreatureBirthEntity(entt::registry& reg,
                                              const CreatureBirthProposal& b,
                                              std::uint32_t cooldown_ticks) {
    const auto child = reg.create();
    try {
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
        gn.reproduce_cooldown = cooldown_ticks; // newborn can't immediately breed
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

    } catch (...) {
        reg.destroy(child);
        throw;
    }
    return child;
}

// PHASE A (run BEFORE the physics bridge): ready creatures steer toward the nearest ready
// opposite-sex mate by OVERRIDING the brain's wish velocity — unless they are fleeing a
// threat (survival beats courtship). When already within courtship range they hold still
// (wish 0) so they stay together and court. id-ordered; reads a pre snapshot (order-stable).
inline void RunMateSeekingOnTick(entt::registry& reg,
                                 const ReproductionTuning& tuning = {},
                                 const ::Luminumbra::world::RegionEntityGate& gate = {},
                                 const ::Luminumbra::world::RegionEntityWork& completed = {}) {
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
    ::Luminumbra::world::FilterRegionEntities(reg, ents, gate);
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
        if (completed)
            completed(self.e);
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
                       const CreatureSpeciesRegistry* species = nullptr,
                       const ::Luminumbra::world::RegionEntityGate& gate = {},
                       const ::Luminumbra::world::RegionEntityWork& completed = {},
                       const CreatureBirthAdmission& admit_birth = {}) {
    CreatureReproductionStats stats;
    auto view = reg.view<Comp::CreatureComponent,
                         Comp::CreatureGenomeComponent,
                         Comp::TransformComponent>();
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    ::Luminumbra::world::FilterRegionEntities(reg, ents, gate);

    // Bookkeeping: age everyone, tick down cooldowns (id-ordered, deterministic).
    for (auto e : ents) {
        if (completed)
            completed(e);
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

    using Birth = CreatureBirthProposal;
    std::vector<Birth> births;
    std::vector<std::pair<entt::entity, entt::entity>> parents;
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
        if (!gate || gn.courting_ticks < std::numeric_limits<std::uint32_t>::max())
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

        auto legacy_rng = luminumbra::core::DeterministicRng::seeded(
            kReproSeedOffset ^ world_seed,
            (static_cast<std::uint64_t>(entt::to_integral(e)) * 0x9E3779B97F4A7C15ull) ^
                static_cast<std::uint64_t>(entt::to_integral(maleE)),
            tick);
        std::uint64_t seed = 0;
        if (gate) {
            const auto& owner = reg.get<Comp::CreatureRegionComponent>(e);
            seed = ::Luminumbra::world::RegionRng(
                       world_seed,
                       {owner.owner_x, owner.owner_z},
                       ::Luminumbra::world::RegionRngTag::CreatureReproduction,
                       tick,
                       ::Luminumbra::world::RegionEntityIdentity(reg, e, true))
                       .draw(::Luminumbra::world::RegionEntityIdentity(reg, maleE, true));
        }
        auto rng = gate ? luminumbra::core::DeterministicRng(seed) : legacy_rng;
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
        if (gate) {
            const auto& owner = reg.get<Comp::CreatureRegionComponent>(e);
            b.mother_owner = {owner.owner_x, owner.owner_z};
            b.mother_id = owner.persistent_id;
            b.father_id = reg.get<Comp::CreatureRegionComponent>(maleE).persistent_id;
            b.seed = seed;
        }
        b.x = (tf.position.x + mtf.position.x) * 0.5f;
        b.y = (tf.position.y + mtf.position.y) * 0.5f;
        b.z = (tf.position.z + mtf.position.z) * 0.5f + tuning.spawn_radius;
        const auto generation = std::max(gn.generation, mgn.generation);
        b.generation = gate && generation == std::numeric_limits<std::uint32_t>::max()
                           ? generation
                           : generation + 1u;
        b.female = childFemale;
        b.species_id = cr.species_id; // offspring inherit the mother's catalog species identity
        births.push_back(b);

        parents.emplace_back(e, maleE);
        if (!gate) {
            gn.courting_ticks = 0;
            gn.reproduce_cooldown = tuning.cooldown_ticks;
            mgn.reproduce_cooldown = tuning.cooldown_ticks;
        }
    }

    // Create offspring AFTER the read pass (no view invalidation mid-iteration).
    for (std::size_t i = 0; i < births.size(); ++i) {
        const auto& b = births[i];
        if (gate) {
            const auto [mother, father] = parents[i];
            if (!::Luminumbra::world::AdmitRegionEntity(gate, mother) ||
                !::Luminumbra::world::AdmitRegionEntity(gate, father) || !admit_birth ||
                !admit_birth(b))
                continue;
            auto& gn = reg.get<Comp::CreatureGenomeComponent>(mother);
            auto& mgn = reg.get<Comp::CreatureGenomeComponent>(father);
            gn.courting_ticks = mgn.courting_ticks = 0;
            gn.reproduce_cooldown = mgn.reproduce_cooldown = tuning.cooldown_ticks;
        } else {
            CreateCreatureBirthEntity(reg, b, tuning.cooldown_ticks);
        }
        ++stats.born;
    }
    return stats;
}

} // namespace luminumbra::ai
