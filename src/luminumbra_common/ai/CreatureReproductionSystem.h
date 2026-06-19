#pragma once

// Track (a) — CREATURE EVOLUTION: the deterministic creature REPRODUCTION tick. Makes the
// ecology GENERATIONAL — well-fed, healthy, mature prey breed and pass a MUTATED copy of
// their genome to ONE offspring; caught prey (CreatureComponent.eaten) die without breeding.
// Over many ticks this is natural selection: traits that survive predation propagate.
//
// DETERMINISM (this changes sim state -> world_hash): id-ordered traversal; the per-birth
// mutation RNG is seeded ONLY from integers — DeterministicRng::seeded(kReproSeedOffset,
// parent_id, tick) — so there is NO wall-clock / std::random and run==replay holds. The
// mutation noise is the libm-free Irwin-Hall Gaussian (DeterministicRng). New entities are
// created AFTER the read pass (we snapshot eligible parents first) so the view is never
// invalidated mid-iteration and creation order is id-deterministic.
//
// GATING: a creature only reproduces if it carries a CreatureGenomeComponent (the opt-in,
// like PlantTag / CreatureComponent). A world whose creatures carry NO genome — and in
// particular a world with NO creatures — performs ZERO mutation and creates ZERO entities,
// so the canonical NetworkStateHash baseline (frontier_gates_test) stays byte-identical.
//
// SEED OFFSET REGISTRY (GameSession): wind+11, weather+12/13, aether+14, plant+15 are taken;
// CREATURE REPRODUCTION CLAIMS +16. Recorded here and at the GameSession wire site.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "CreatureGenome.h"
#include "UtilityAI.h"  // utility_clamp01

#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../core/DeterministicMath.h"  // Cos/Sin (libm-free)
#include "../core/DeterministicRng.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;

// Distinct world-seed offset for the reproduction stream (see registry note above).
inline constexpr std::uint64_t kReproSeedOffset = 16ull;

// Eligibility tuning (deterministic integer/float constants; no data dependency yet).
//   * a creature must reach maturity (age) before it can breed,
//   * it must be WELL-FED (hunger at/below its genome's threshold),
//   * it must be HEALTHY (stamina at/above this floor),
//   * after a birth it waits this cooldown before breeding again (bounds population growth).
inline constexpr std::uint32_t kReproMaturityTicks = 90;   // ~3s @ 30Hz
inline constexpr std::uint32_t kReproCooldownTicks = 300;  // ~10s @ 30Hz
inline constexpr float kReproHealthyStamina = 0.5f;
// Offspring spawn offset radius (m) from the parent — small so the newborn appears beside it.
inline constexpr float kReproSpawnRadius = 1.5f;

struct CreatureReproductionStats {
    int born = 0;       // offspring created this tick
    int considered = 0; // creatures with a genome examined this tick
};

// Copy the heritable traits from a genome component onto a CreatureComponent so the genome
// actually drives behaviour (e.g. move_speed feeds the brain). Pure helper.
inline void ApplyGenomeToCreature(const Comp::CreatureGenomeComponent& g, Comp::CreatureComponent& c) {
    c.move_speed = g.move_speed;
}

// Advance reproduction by one fixed tick. Pure function of registry state + the tick id (the
// RNG seed). Returns birth/consider counts (telemetry / sub-hash). `world_seed` lets distinct
// worlds diverge; default 0 keeps unit tests simple and reproducible.
inline CreatureReproductionStats RunCreatureReproductionOnTick(entt::registry& reg,
                                                               std::uint64_t tick,
                                                               std::uint64_t world_seed = 0) {
    CreatureReproductionStats stats;

    auto view = reg.view<Comp::CreatureComponent, Comp::CreatureGenomeComponent,
                         Comp::TransformComponent>();

    // id-ordered parent ids so age/cooldown advance and birth order are deterministic.
    std::vector<entt::entity> ents;
    for (auto e : view) ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    // Phase 1 (read/update): advance bookkeeping and collect the parents that breed THIS
    // tick. We don't create entities here — that would invalidate the view.
    struct Birth {
        entt::entity parent;
        CreatureGenome offspring;
        float px, py, pz;       // parent position (offspring spawns near it)
        bool predator;
        std::uint32_t generation;  // offspring generation
    };
    std::vector<Birth> births;

    for (auto e : ents) {
        ++stats.considered;
        auto& cr = view.get<Comp::CreatureComponent>(e);
        auto& gn = view.get<Comp::CreatureGenomeComponent>(e);
        const auto& tf = view.get<Comp::TransformComponent>(e);

        // Age every (living or dead) creature; tick down the cooldown.
        if (gn.age_ticks < 0xFFFFFFFFu) ++gn.age_ticks;
        if (gn.reproduce_cooldown > 0) --gn.reproduce_cooldown;

        // A carcass never breeds (selection: caught prey leave no offspring). Predators do
        // not breed in this slice (prey are the heritable population being selected on).
        if (cr.eaten || cr.is_predator) continue;

        const bool mature = gn.age_ticks >= kReproMaturityTicks;
        const bool ready = gn.reproduce_cooldown == 0;
        const bool well_fed = cr.hunger <= gn.hunger_threshold;
        const bool healthy = cr.stamina >= kReproHealthyStamina;
        if (!(mature && ready && well_fed && healthy)) continue;

        // Seed ONLY from integers: offset, parent id, tick (+ optional world seed). Pure.
        luminumbra::core::DeterministicRng rng = luminumbra::core::DeterministicRng::seeded(
            kReproSeedOffset ^ world_seed, static_cast<std::uint64_t>(entt::to_integral(e)), tick);

        CreatureGenome parent_genome;
        parent_genome.move_speed = gn.move_speed;
        parent_genome.vigilance = gn.vigilance;
        parent_genome.hunger_threshold = gn.hunger_threshold;
        parent_genome.size_scale = gn.size_scale;

        Birth b;
        b.parent = e;
        b.offspring = MutateOffspring(parent_genome, rng);
        b.px = tf.position.x;
        b.py = tf.position.y;
        b.pz = tf.position.z;
        b.predator = cr.is_predator;
        b.generation = gn.generation + 1u;
        // Deterministic spawn offset around the parent (golden-angle by id, scaled by rng).
        births.push_back(b);

        // Parent pays the cost: cooldown + a hunger/stamina hit (raising young is taxing).
        gn.reproduce_cooldown = kReproCooldownTicks;
        cr.hunger = utility_clamp01(cr.hunger + 0.25f);
        cr.stamina = utility_clamp01(cr.stamina - 0.25f);
    }

    // Phase 2 (create): spawn offspring in id-deterministic parent order.
    for (const Birth& b : births) {
        // Re-derive the SAME rng stream for the spawn offset (seeded purely from integers),
        // so the offspring position is deterministic and independent of creation order.
        luminumbra::core::DeterministicRng rng = luminumbra::core::DeterministicRng::seeded(
            kReproSeedOffset ^ world_seed,
            static_cast<std::uint64_t>(entt::to_integral(b.parent)), tick);
        // Advance past the mutation draws (4 genes * 12 gaussians each) so the offset draws
        // are a distinct, stable sub-stream. (next_gaussian consumes 12 next_unit -> next_u64.)
        for (int i = 0; i < static_cast<int>(kCreatureGeneCount) * 12; ++i) rng.next_u64();
        const float ang = rng.next_range(0.0f, 6.2831853f);
        const float rad = rng.next_range(0.5f, 1.0f) * kReproSpawnRadius;
        const float ox = ::Luminumbra::DeterministicMath::Cos(ang) * rad;
        const float oz = ::Luminumbra::DeterministicMath::Sin(ang) * rad;

        const auto child = reg.create();
        auto& tf = reg.emplace<Comp::TransformComponent>(child);
        tf.position.x = b.px + ox;
        tf.position.y = b.py;
        tf.position.z = b.pz + oz;
        tf.scale = ::Luminumbra::Vec3(b.offspring.size_scale);

        auto& cr = reg.emplace<Comp::CreatureComponent>(child);
        cr.is_predator = b.predator;
        cr.hunger = 0.5f;     // newborn starts moderately hungry
        cr.stamina = 1.0f;    // fresh
        cr.move_speed = b.offspring.move_speed;  // genome drives the brain

        auto& gn = reg.emplace<Comp::CreatureGenomeComponent>(child);
        gn.move_speed = b.offspring.move_speed;
        gn.vigilance = b.offspring.vigilance;
        gn.hunger_threshold = b.offspring.hunger_threshold;
        gn.size_scale = b.offspring.size_scale;
        gn.age_ticks = 0;
        gn.reproduce_cooldown = kReproCooldownTicks;  // newborn can't immediately breed
        gn.generation = b.generation;

        ++stats.born;
    }

    return stats;
}

}  // namespace luminumbra::ai
