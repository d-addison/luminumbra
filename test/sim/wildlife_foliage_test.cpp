// sim.wildlife_foliage: the ECOLOGY x FOLIAGE coupling: non-predator creatures
// GRAZE/TRAMPLE nearby grazeable plants (biomass down), and plants REGROW when ungrazed.
// Deterministic: id-ordered traversal, creature positions snapshotted first + graze demand
// summed per plant so the result is order-independent; NO rng / wall-clock / libm. Gated by
// GrazeableComponent: a registry with none performs ZERO biomass changes (no-op), so the
// canonical NetworkStateHash baseline stays byte-identical.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/WildlifeFoliageSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/GrazeableComponent.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::RunWildlifeFoliageOnTick;
using luminumbra::ai::WildlifeFoliageStats;
using luminumbra::ai::kGrazeRadius;
using luminumbra::ai::kRegrowPerTick;

// Spawn a grazeable plant at (x,z) with a starting biomass.
entt::entity spawnPlant(entt::registry& r, float x, float z, float biomass = 1.0f) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    auto& g = r.emplace<Comp::GrazeableComponent>(e);
    g.biomass = biomass;
    return e;
}

// Spawn a creature (default: a live herbivore = non-predator, not eaten).
entt::entity spawnCreature(entt::registry& r, float x, float z, bool predator = false,
                           bool eaten = false, float hunger = 0.5f) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = predator;
    cr.eaten = eaten;
    cr.hunger = hunger;
    return e;
}

// ---- gating: empty roster / no opt-in ----

// No entities at all -> nothing happens (true no-op).
TEST(WildlifeFoliage, EmptyRosterNoOp) {
    entt::registry r;
    const WildlifeFoliageStats stats = RunWildlifeFoliageOnTick(r, 1);
    EXPECT_EQ(stats.grazers, 0);
    EXPECT_EQ(stats.plants, 0);
    EXPECT_EQ(stats.grazed_plants, 0);
    EXPECT_EQ(stats.biomass_removed, 0.0f);
    EXPECT_EQ(stats.biomass_regrown, 0.0f);
}

// Creatures but NO grazeable plant -> no plants considered, creatures untouched (no-op).
TEST(WildlifeFoliage, CreaturesButNoPlantsNoOp) {
    entt::registry r;
    auto c = spawnCreature(r, 0.0f, 0.0f, /*predator=*/false, /*eaten=*/false, /*hunger=*/0.5f);
    const WildlifeFoliageStats stats = RunWildlifeFoliageOnTick(r, 1);
    EXPECT_EQ(stats.plants, 0);
    EXPECT_EQ(stats.grazed_plants, 0);
    // Hunger is unchanged: with no plants there is nothing to feed on.
    EXPECT_FLOAT_EQ(r.get<Comp::CreatureComponent>(c).hunger, 0.5f);
}

// A plant WITHOUT the GrazeableComponent opt-in is ignored entirely.
TEST(WildlifeFoliage, PlantWithoutGrazeableIsIgnored) {
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = 0.0f;
    tf.position.z = 0.0f;
    spawnCreature(r, 0.0f, 0.0f); // adjacent grazer
    const WildlifeFoliageStats stats = RunWildlifeFoliageOnTick(r, 1);
    EXPECT_EQ(stats.plants, 0) << "a plant without GrazeableComponent is not a participant";
    EXPECT_EQ(stats.grazed_plants, 0);
}

// ---- core: an adjacent creature grazes a plant down ----

TEST(WildlifeFoliage, AdjacentCreatureReducesBiomass) {
    entt::registry r;
    auto plant = spawnPlant(r, 0.0f, 0.0f, 1.0f);
    spawnCreature(r, 0.5f, 0.0f); // within graze radius

    const float before = r.get<Comp::GrazeableComponent>(plant).biomass;
    const WildlifeFoliageStats stats = RunWildlifeFoliageOnTick(r, 7);
    const float after = r.get<Comp::GrazeableComponent>(plant).biomass;

    EXPECT_EQ(stats.grazers, 1);
    EXPECT_EQ(stats.plants, 1);
    EXPECT_EQ(stats.grazed_plants, 1);
    EXPECT_LT(after, before) << "an adjacent creature reduces the plant's biomass";
    EXPECT_GT(stats.biomass_removed, 0.0f);
    // last_grazed_tick records the tick.
    EXPECT_EQ(r.get<Comp::GrazeableComponent>(plant).last_grazed_tick, 7u);
}

// ---- a distant plant is untouched (and regrows instead) ----

TEST(WildlifeFoliage, DistantPlantUntouched) {
    entt::registry r;
    // Plant far outside the graze radius, started slightly below full so regrow is visible.
    auto plant = spawnPlant(r, 100.0f, 100.0f, 0.5f);
    spawnCreature(r, 0.0f, 0.0f);

    const float before = r.get<Comp::GrazeableComponent>(plant).biomass;
    RunWildlifeFoliageOnTick(r, 1);
    const float after = r.get<Comp::GrazeableComponent>(plant).biomass;

    // No creature in range -> not grazed -> it regrows (does NOT drop).
    EXPECT_GT(after, before) << "a distant plant is not grazed; it regrows";
    EXPECT_FLOAT_EQ(after, before + kRegrowPerTick);
}

// ---- predators and carcasses do NOT graze ----

TEST(WildlifeFoliage, PredatorsAndEatenDoNotGraze) {
    entt::registry r;
    auto plant = spawnPlant(r, 0.0f, 0.0f, 0.5f);
    spawnCreature(r, 0.1f, 0.0f, /*predator=*/true);                 // predator: no graze
    spawnCreature(r, 0.1f, 0.1f, /*predator=*/false, /*eaten=*/true); // carcass: no graze

    const float before = r.get<Comp::GrazeableComponent>(plant).biomass;
    const WildlifeFoliageStats stats = RunWildlifeFoliageOnTick(r, 1);
    const float after = r.get<Comp::GrazeableComponent>(plant).biomass;

    EXPECT_EQ(stats.grazers, 0) << "predators and carcasses are not herbivore grazers";
    EXPECT_EQ(stats.grazed_plants, 0);
    EXPECT_GT(after, before) << "with no grazer in range the plant regrows";
}

// ---- biomass regrows when ungrazed, clamped to 1.0 ----

TEST(WildlifeFoliage, BiomassRegrowsAndClampsAtCap) {
    entt::registry r;
    auto plant = spawnPlant(r, 0.0f, 0.0f, 0.5f);
    // No creatures -> pure regrowth.
    float prev = r.get<Comp::GrazeableComponent>(plant).biomass;
    for (int t = 0; t < 200; ++t) {
        RunWildlifeFoliageOnTick(r, static_cast<std::uint64_t>(t));
        const float now = r.get<Comp::GrazeableComponent>(plant).biomass;
        EXPECT_GE(now, prev) << "ungrazed biomass monotonically recovers";
        EXPECT_LE(now, 1.0f) << "regrowth never overshoots the cap";
        prev = now;
    }
    EXPECT_FLOAT_EQ(prev, 1.0f) << "biomass fully recovers to the cap";
}

// Grazing never drives biomass below zero (clamped).
TEST(WildlifeFoliage, GrazingClampsAtZero) {
    entt::registry r;
    auto plant = spawnPlant(r, 0.0f, 0.0f, 0.1f);
    // A herd on top of the plant grazes harder than the biomass available.
    for (int i = 0; i < 10; ++i) spawnCreature(r, 0.0f, 0.0f);
    RunWildlifeFoliageOnTick(r, 1);
    EXPECT_GE(r.get<Comp::GrazeableComponent>(plant).biomass, 0.0f)
        << "biomass is clamped at zero, never negative";
}

// A herd flattens a patch faster than a single grazer.
TEST(WildlifeFoliage, HerdGrazesHarderThanSingle) {
    entt::registry oneReg;
    auto onePlant = spawnPlant(oneReg, 0.0f, 0.0f, 1.0f);
    spawnCreature(oneReg, 0.2f, 0.0f);
    RunWildlifeFoliageOnTick(oneReg, 1);
    const float oneLeft = oneReg.get<Comp::GrazeableComponent>(onePlant).biomass;

    entt::registry herdReg;
    auto herdPlant = spawnPlant(herdReg, 0.0f, 0.0f, 1.0f);
    spawnCreature(herdReg, 0.2f, 0.0f);
    spawnCreature(herdReg, 0.0f, 0.2f);
    spawnCreature(herdReg, -0.2f, 0.0f);
    RunWildlifeFoliageOnTick(herdReg, 1);
    const float herdLeft = herdReg.get<Comp::GrazeableComponent>(herdPlant).biomass;

    EXPECT_LT(herdLeft, oneLeft) << "more grazers in range trample the patch down further";
}

// ---- grazing feeds the creature (lowers hunger) ----

TEST(WildlifeFoliage, GrazingLowersHunger) {
    entt::registry r;
    spawnPlant(r, 0.0f, 0.0f, 1.0f);
    auto c = spawnCreature(r, 0.3f, 0.0f, /*predator=*/false, /*eaten=*/false, /*hunger=*/0.8f);
    RunWildlifeFoliageOnTick(r, 1);
    EXPECT_LT(r.get<Comp::CreatureComponent>(c).hunger, 0.8f)
        << "grazing on a plant feeds the creature";
    EXPECT_GE(r.get<Comp::CreatureComponent>(c).hunger, 0.0f) << "hunger clamps at zero";
}

// ---- determinism: order-independent over creatures ----

// The grazed biomass is independent of the order creatures were created in (positions are
// snapshotted + graze pressure is summed per plant before being applied).
TEST(WildlifeFoliage, OrderIndependentOverCreatures) {
    auto run = [](bool reversed) {
        entt::registry r;
        auto plant = spawnPlant(r, 0.0f, 0.0f, 1.0f);
        if (!reversed) {
            spawnCreature(r, 0.2f, 0.0f);
            spawnCreature(r, 0.0f, 0.2f);
            spawnCreature(r, -0.2f, 0.0f);
        } else {
            spawnCreature(r, -0.2f, 0.0f);
            spawnCreature(r, 0.0f, 0.2f);
            spawnCreature(r, 0.2f, 0.0f);
        }
        RunWildlifeFoliageOnTick(r, 3);
        return r.get<Comp::GrazeableComponent>(plant).biomass;
    };
    EXPECT_FLOAT_EQ(run(false), run(true));
}

// ---- determinism: run == replay ----

TEST(WildlifeFoliage, SystemDeterministicRunEqualsReplay) {
    auto run = [] {
        entt::registry r;
        auto p1 = spawnPlant(r, 0.0f, 0.0f, 1.0f);
        auto p2 = spawnPlant(r, 5.0f, 5.0f, 0.7f);
        auto p3 = spawnPlant(r, 50.0f, 50.0f, 0.3f); // far, regrows
        auto c1 = spawnCreature(r, 0.5f, 0.0f, false, false, 0.9f);
        auto c2 = spawnCreature(r, 5.2f, 5.0f, false, false, 0.6f);
        spawnCreature(r, 5.0f, 5.2f, false, false, 0.4f); // second grazer on p2
        std::vector<float> trail;
        for (std::uint64_t t = 0; t < 20; ++t) {
            RunWildlifeFoliageOnTick(r, t);
            trail.push_back(r.get<Comp::GrazeableComponent>(p1).biomass);
            trail.push_back(r.get<Comp::GrazeableComponent>(p2).biomass);
            trail.push_back(r.get<Comp::GrazeableComponent>(p3).biomass);
            trail.push_back(r.get<Comp::CreatureComponent>(c1).hunger);
            trail.push_back(r.get<Comp::CreatureComponent>(c2).hunger);
        }
        return trail;
    };
    EXPECT_EQ(run(), run());
}

} // namespace
