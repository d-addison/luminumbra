// Track (a) — CREATURE EVOLUTION + REPRODUCTION: the generational ecology tick.
// Deterministic (id-ordered, RNG seeded purely from offset 16 + parent id + tick; libm-free
// Irwin-Hall mutation). Gated by CreatureGenomeComponent: a world with none — and a world with
// no creatures — creates nothing (canonical NetworkStateHash baseline byte-identical).
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CreatureGenome.h"
#include "ai/CreatureReproductionSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "core/DeterministicRng.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::CreatureGenome;
using luminumbra::ai::RunCreatureReproductionOnTick;
using luminumbra::core::DeterministicRng;

// Spawn a prey creature with a genome that is ALREADY eligible (mature, off cooldown) unless
// overridden, so a single tick can breed it.
entt::entity spawnPrey(entt::registry& r, float x, float z, float hunger, float stamina,
                       std::uint32_t age = luminumbra::ai::kReproMaturityTicks,
                       std::uint32_t cooldown = 0) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = false;
    cr.hunger = hunger;
    cr.stamina = stamina;
    auto& gn = r.emplace<Comp::CreatureGenomeComponent>(e);
    gn.age_ticks = age;
    gn.reproduce_cooldown = cooldown;
    return e;
}

int creatureCount(entt::registry& r) {
    int n = 0;
    for (auto e : r.view<Comp::CreatureComponent>()) {
        (void)e;
        ++n;
    }
    return n;
}

// ---- gating: empty world / no genome ----

// No creatures at all -> the system creates nothing and reports zero (byte-identical roster).
TEST(CreatureReproduction, EmptyRosterNoOp) {
    entt::registry r;
    const auto stats = RunCreatureReproductionOnTick(r, /*tick*/ 1);
    EXPECT_EQ(stats.born, 0);
    EXPECT_EQ(stats.considered, 0);
    EXPECT_EQ(creatureCount(r), 0);
}

// A creature with NO genome component is never considered (the opt-in is the genome).
TEST(CreatureReproduction, NoGenomeIsIgnored) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = false;
    cr.hunger = 0.0f;
    cr.stamina = 1.0f;
    const auto stats = RunCreatureReproductionOnTick(r, /*tick*/ 1);
    EXPECT_EQ(stats.considered, 0);
    EXPECT_EQ(stats.born, 0);
    EXPECT_EQ(creatureCount(r), 1);
}

// ---- eligibility ----

// A well-fed (low hunger), healthy (high stamina), mature, off-cooldown prey reproduces.
TEST(CreatureReproduction, WellFedMaturePreyBreeds) {
    entt::registry r;
    spawnPrey(r, 0.0f, 0.0f, /*hunger*/ 0.1f, /*stamina*/ 1.0f);
    const auto stats = RunCreatureReproductionOnTick(r, /*tick*/ 7);
    EXPECT_EQ(stats.born, 1);
    EXPECT_EQ(creatureCount(r), 2);
}

// A YOUNG prey (below maturity) does not breed even when well-fed and healthy.
TEST(CreatureReproduction, YoungPreyDoesNotBreed) {
    entt::registry r;
    spawnPrey(r, 0.0f, 0.0f, /*hunger*/ 0.0f, /*stamina*/ 1.0f, /*age*/ 0);
    const auto stats = RunCreatureReproductionOnTick(r, /*tick*/ 7);
    EXPECT_EQ(stats.born, 0);
    EXPECT_EQ(creatureCount(r), 1);
}

// A HUNGRY prey (above its genome's hunger threshold) does not breed.
TEST(CreatureReproduction, HungryPreyDoesNotBreed) {
    entt::registry r;
    spawnPrey(r, 0.0f, 0.0f, /*hunger*/ 0.9f, /*stamina*/ 1.0f);
    const auto stats = RunCreatureReproductionOnTick(r, /*tick*/ 7);
    EXPECT_EQ(stats.born, 0);
    EXPECT_EQ(creatureCount(r), 1);
}

// A caught carcass (eaten) never breeds, even if otherwise eligible (selection).
TEST(CreatureReproduction, EatenPreyDoesNotBreed) {
    entt::registry r;
    auto e = spawnPrey(r, 0.0f, 0.0f, /*hunger*/ 0.0f, /*stamina*/ 1.0f);
    r.get<Comp::CreatureComponent>(e).eaten = true;
    const auto stats = RunCreatureReproductionOnTick(r, /*tick*/ 7);
    EXPECT_EQ(stats.born, 0);
}

// ---- offspring inheritance + mutation determinism ----

// MutateOffspring is a pure deterministic function of (parent, rng seed).
TEST(CreatureReproduction, MutationDeterministicSameSeed) {
    CreatureGenome parent;
    parent.move_speed = 3.0f;
    parent.size_scale = 1.0f;
    auto draw = [&] {
        DeterministicRng rng = DeterministicRng::seeded(luminumbra::ai::kReproSeedOffset, 42, 9);
        return luminumbra::ai::MutateOffspring(parent, rng);
    };
    const CreatureGenome a = draw();
    const CreatureGenome b = draw();
    EXPECT_FLOAT_EQ(a.move_speed, b.move_speed);
    EXPECT_FLOAT_EQ(a.vigilance, b.vigilance);
    EXPECT_FLOAT_EQ(a.hunger_threshold, b.hunger_threshold);
    EXPECT_FLOAT_EQ(a.size_scale, b.size_scale);
}

// Different seeds generally diverge (mutation actually moves the genome).
TEST(CreatureReproduction, MutationDiffersAcrossSeeds) {
    CreatureGenome parent;  // mid-range so mutation has room in both directions
    parent.move_speed = 4.0f;
    DeterministicRng r1 = DeterministicRng::seeded(luminumbra::ai::kReproSeedOffset, 1, 1);
    DeterministicRng r2 = DeterministicRng::seeded(luminumbra::ai::kReproSeedOffset, 2, 2);
    const CreatureGenome a = luminumbra::ai::MutateOffspring(parent, r1);
    const CreatureGenome b = luminumbra::ai::MutateOffspring(parent, r2);
    EXPECT_NE(a.move_speed, b.move_speed);
}

// Offspring genome stays within the canonical gene bounds after mutation.
TEST(CreatureReproduction, OffspringGenomeStaysInBounds) {
    CreatureGenome parent;
    parent.move_speed = 8.0f;        // at the upper bound
    parent.hunger_threshold = 0.6f;  // at the upper bound
    DeterministicRng rng = DeterministicRng::seeded(luminumbra::ai::kReproSeedOffset, 5, 5);
    const CreatureGenome c = luminumbra::ai::MutateOffspring(parent, rng);
    const auto b = luminumbra::ai::CreatureGeneBounds();
    EXPECT_GE(c.move_speed, b[0].lo);
    EXPECT_LE(c.move_speed, b[0].hi);
    EXPECT_GE(c.hunger_threshold, b[2].lo);
    EXPECT_LE(c.hunger_threshold, b[2].hi);
}

// run == replay through the SYSTEM: identical setup + tick -> identical offspring genome.
TEST(CreatureReproduction, SystemReproductionDeterministic) {
    auto run = [] {
        entt::registry r;
        // Stamp a non-default parent genome so inheritance is observable.
        auto e = spawnPrey(r, 2.0f, -1.0f, /*hunger*/ 0.05f, /*stamina*/ 1.0f);
        auto& gn = r.get<Comp::CreatureGenomeComponent>(e);
        gn.move_speed = 5.0f;
        gn.size_scale = 1.3f;
        RunCreatureReproductionOnTick(r, /*tick*/ 11);
        // Read the offspring (the higher entity id).
        std::vector<float> out;
        std::uint32_t best = 0;
        entt::entity child = entt::null;
        for (auto c : r.view<Comp::CreatureGenomeComponent>()) {
            const auto v = static_cast<std::uint32_t>(entt::to_integral(c));
            if (v >= best) {
                best = v;
                child = c;
            }
        }
        const auto& cg = r.get<Comp::CreatureGenomeComponent>(child);
        const auto& tf = r.get<Comp::TransformComponent>(child);
        out = {cg.move_speed, cg.size_scale, cg.hunger_threshold, tf.position.x, tf.position.z};
        return out;
    };
    EXPECT_EQ(run(), run());
}

// Offspring's move_speed is applied to its CreatureComponent (genome drives the brain).
TEST(CreatureReproduction, OffspringMoveSpeedFromGenome) {
    entt::registry r;
    auto parent = spawnPrey(r, 0.0f, 0.0f, /*hunger*/ 0.05f, /*stamina*/ 1.0f);
    r.get<Comp::CreatureGenomeComponent>(parent).move_speed = 6.0f;
    RunCreatureReproductionOnTick(r, /*tick*/ 3);
    // Find the offspring (entity != parent) and assert CreatureComponent.move_speed == genome.
    for (auto e : r.view<Comp::CreatureGenomeComponent>()) {
        if (e == parent) continue;
        const auto& cr = r.get<Comp::CreatureComponent>(e);
        const auto& gn = r.get<Comp::CreatureGenomeComponent>(e);
        EXPECT_FLOAT_EQ(cr.move_speed, gn.move_speed);
        EXPECT_EQ(gn.generation, 1u);
    }
}

// ---- bounded growth ----

// Over many ticks, one founder yields a BOUNDED population (cooldown + maturity stop runaway).
TEST(CreatureReproduction, PopulationBoundedOverManyTicks) {
    entt::registry r;
    // A single founder that will keep being well-fed/healthy (we top it up each tick).
    auto founder = spawnPrey(r, 0.0f, 0.0f, /*hunger*/ 0.0f, /*stamina*/ 1.0f);
    (void)founder;
    const int N = 200;  // ~6.6s @ 30Hz
    for (int t = 1; t <= N; ++t) {
        // Keep every living prey well-fed + healthy so eligibility is purely cooldown/maturity
        // gated (the worst case for runaway growth).
        for (auto e : r.view<Comp::CreatureComponent, Comp::CreatureGenomeComponent>()) {
            auto& cr = r.get<Comp::CreatureComponent>(e);
            cr.hunger = 0.0f;
            cr.stamina = 1.0f;
        }
        RunCreatureReproductionOnTick(r, static_cast<std::uint64_t>(t));
    }
    const int pop = creatureCount(r);
    // With a 300-tick cooldown and 90-tick maturity, 200 ticks cannot produce an explosion.
    // The founder breeds at most a couple of times; each child needs 90 ticks to mature then
    // 300 to breed. A loose upper bound that still fails on runaway (exponential) growth:
    EXPECT_GT(pop, 1) << "the founder should have bred at least once";
    EXPECT_LT(pop, 16) << "population must stay bounded (no runaway)";
}

// Cooldown is respected: an off-cooldown breed sets a fresh cooldown so a second immediate
// tick does NOT breed the same parent again.
TEST(CreatureReproduction, CooldownPreventsImmediateRebreed) {
    entt::registry r;
    spawnPrey(r, 0.0f, 0.0f, /*hunger*/ 0.0f, /*stamina*/ 1.0f);
    const auto s1 = RunCreatureReproductionOnTick(r, /*tick*/ 1);
    EXPECT_EQ(s1.born, 1);
    // Keep the parent well-fed; the newborn is immature + on cooldown. Next tick: no births.
    for (auto e : r.view<Comp::CreatureComponent>()) {
        auto& cr = r.get<Comp::CreatureComponent>(e);
        cr.hunger = 0.0f;
        cr.stamina = 1.0f;
    }
    const auto s2 = RunCreatureReproductionOnTick(r, /*tick*/ 2);
    EXPECT_EQ(s2.born, 0) << "parent on cooldown + immature newborn -> no births";
}

}  // namespace
