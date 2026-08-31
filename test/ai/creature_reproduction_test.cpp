// Creature evolution: SEXUAL creature reproduction: a male + a female must find each other and
// COURT before a baby is born. Deterministic (id-ordered, seeded-from-ints, libm-free), gated by
// the CreatureGenomeComponent opt-in.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CreatureReproductionSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/InstinctComponents.h" // PerceptionComponent

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::BreedOffspring;
using luminumbra::ai::BreedSensoryInto;
using luminumbra::ai::CreatureGenome;
using luminumbra::ai::CreatureSensoryGeneBounds;
using luminumbra::ai::kCourtshipTicks;
using luminumbra::ai::RunMateSeekingOnTick;
using luminumbra::ai::RunMatingResolveOnTick;

// Spawn a prey with a genome. `ready` => mature, well-fed, healthy, off cooldown.
entt::entity spawnMate(entt::registry& r, float x, float z, bool female, bool ready = true) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = false;
    auto& gn = r.emplace<Comp::CreatureGenomeComponent>(e);
    gn.female = female;
    if (ready) {
        gn.age_ticks = 120; // mature (>= kReproMaturityTicks)
        gn.reproduce_cooldown = 0;
        cr.hunger = 0.05f; // <= hunger_threshold (0.3)
        cr.stamina = 1.0f; // >= healthy floor
    } else {
        gn.age_ticks = 0; // too young
    }
    return e;
}

std::size_t creatureCount(entt::registry& r) {
    return r.view<Comp::CreatureComponent>().size();
}

// Empty roster: both phases are no-ops.
TEST(CreatureReproduction, EmptyRosterNoOp) {
    entt::registry r;
    RunMateSeekingOnTick(r);
    EXPECT_EQ(RunMatingResolveOnTick(r, 1).born, 0);
}

// A creature without a genome never mates.
TEST(CreatureReproduction, NoGenomeIgnored) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e);
    r.emplace<Comp::CreatureComponent>(e); // no CreatureGenomeComponent
    for (int i = 0; i < 200; ++i)
        RunMatingResolveOnTick(r, static_cast<std::uint64_t>(i));
    EXPECT_EQ(creatureCount(r), 1u);
}

// An adjacent ready male+female court for the duration, then produce exactly one baby; both
// parents go on cooldown afterwards.
TEST(CreatureReproduction, AdjacentPairCourtsThenBreeds) {
    entt::registry r;
    const entt::entity female = spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    const entt::entity male = spawnMate(r, 1.0f, 0.0f, /*female*/ false); // within courtship radius
    int born = 0;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 2; ++t)
        born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 1) << "courtship should yield exactly one baby";
    EXPECT_EQ(creatureCount(r), 3u);
    EXPECT_GT(r.get<Comp::CreatureGenomeComponent>(female).reproduce_cooldown, 0u);
    EXPECT_GT(r.get<Comp::CreatureGenomeComponent>(male).reproduce_cooldown, 0u);
}

// A ready female with NO male never breeds and never accumulates courtship.
TEST(CreatureReproduction, LoneFemaleDoesNotBreed) {
    entt::registry r;
    const entt::entity female = spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    int born = 0;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 50; ++t)
        born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 0);
    EXPECT_EQ(r.get<Comp::CreatureGenomeComponent>(female).courting_ticks, 0u);
}

// Two ready FEMALES (same sex) never breed.
TEST(CreatureReproduction, SameSexDoesNotBreed) {
    entt::registry r;
    spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    spawnMate(r, 1.0f, 0.0f, /*female*/ true);
    int born = 0;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 10; ++t)
        born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 0);
}

// Immature creatures don't court (age gate).
TEST(CreatureReproduction, YoungPairDoesNotBreed) {
    entt::registry r;
    spawnMate(r, 0.0f, 0.0f, /*female*/ true, /*ready*/ false);
    spawnMate(r, 1.0f, 0.0f, /*female*/ false, /*ready*/ false);
    int born = 0;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 5; ++t)
        born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 0);
}

// Mate seeking: a ready female steers TOWARD a ready male within sense range (+x).
TEST(CreatureReproduction, MateSeekingSteersTowardMate) {
    entt::registry r;
    const entt::entity female = spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    spawnMate(r, 12.0f, 0.0f, /*female*/ false); // far but within kMateSeekRadius
    RunMateSeekingOnTick(r);
    EXPECT_GT(r.get<Comp::CreatureComponent>(female).wish_x, 0.0f)
        << "should steer toward the mate";
}

// Offspring genome is a blend of the two parents (between their move_speeds).
TEST(CreatureReproduction, OffspringGenomeBlendsParents) {
    entt::registry r;
    auto f = spawnMate(r, 0.0f, 0.0f, true);
    auto m = spawnMate(r, 1.0f, 0.0f, false);
    r.get<Comp::CreatureGenomeComponent>(f).move_speed = 2.0f;
    r.get<Comp::CreatureGenomeComponent>(m).move_speed = 6.0f;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 2; ++t)
        RunMatingResolveOnTick(r, t);
    float childSpeed = -1.0f;
    for (auto e : r.view<Comp::CreatureGenomeComponent>()) {
        const auto& gn = r.get<Comp::CreatureGenomeComponent>(e);
        if (gn.generation == 1u)
            childSpeed = gn.move_speed;
    }
    ASSERT_GT(childSpeed, 0.0f) << "a child should exist";
    EXPECT_GT(childSpeed, 2.0f - 1.0f);
    EXPECT_LT(childSpeed, 6.0f + 1.0f); // blended between parents (+ small mutation)
}

// the WORLD SEED feeds the per-birth RNG. Same-seed
// worlds reproduce byte-identically; DIFFERENT seeds diverge (offspring genomes
// were previously identical across worlds with the same entity ids + ticks,
// because GameSession passed world_seed 0).
TEST(CreatureReproduction, WorldSeedDrivesOffspringGenomes) {
    auto run = [](std::uint64_t world_seed) {
        entt::registry r;
        auto f = spawnMate(r, 0.0f, 0.0f, true);
        auto m = spawnMate(r, 1.0f, 0.0f, false);
        r.get<Comp::CreatureGenomeComponent>(f).move_speed = 2.0f;
        r.get<Comp::CreatureGenomeComponent>(m).move_speed = 6.0f;
        for (std::uint32_t t = 0; t < kCourtshipTicks + 2; ++t)
            RunMatingResolveOnTick(r, t, world_seed);
        std::vector<float> out;
        for (auto e : r.view<Comp::CreatureGenomeComponent>()) {
            const auto& gn = r.get<Comp::CreatureGenomeComponent>(e);
            if (gn.generation == 1u) {
                out.insert(
                    out.end(),
                    {gn.move_speed, gn.vision_range, gn.hearing_range, gn.vision_cos_half_fov});
            }
        }
        return out;
    };
    const auto seed_a1 = run(424242ull);
    const auto seed_a2 = run(424242ull);
    const auto seed_b = run(999999ull);
    ASSERT_FALSE(seed_a1.empty()) << "no offspring born (vacuous)";
    EXPECT_EQ(seed_a1, seed_a2) << "same-seed worlds must reproduce identically";
    EXPECT_NE(seed_a1, seed_b) << "different world seeds must diverge offspring genomes";
}

// run == replay: identical setup + ticks -> identical population + genomes.
TEST(CreatureReproduction, Deterministic) {
    auto run = [] {
        entt::registry r;
        spawnMate(r, 0.0f, 0.0f, true);
        spawnMate(r, 1.0f, 0.0f, false);
        for (std::uint32_t t = 0; t < kCourtshipTicks + 20; ++t)
            RunMatingResolveOnTick(r, t);
        std::vector<float> out;
        out.push_back(static_cast<float>(r.view<Comp::CreatureComponent>().size()));
        for (auto e : r.view<Comp::CreatureGenomeComponent>())
            out.push_back(r.get<Comp::CreatureGenomeComponent>(e).move_speed);
        return out;
    };
    EXPECT_EQ(run(), run());
}

// the SENSORY genes inherit as a bounded blend+mutate and are deterministic for a given rng.
TEST(CreatureReproduction, SensoryGenesInheritWithinBoundsAndDeterministic) {
    CreatureGenome a;
    a.vision_cos_half_fov = 0.30f;
    a.vision_range = 10.0f;
    a.hearing_range = 12.0f;
    CreatureGenome b;
    b.vision_cos_half_fov = 0.90f;
    b.vision_range = 40.0f;
    b.hearing_range = 38.0f;
    const auto bounds = CreatureSensoryGeneBounds();
    auto breed = [&](std::uint64_t seed) {
        luminumbra::core::DeterministicRng rng =
            luminumbra::core::DeterministicRng::seeded(seed, 1, 2);
        return BreedSensoryInto(CreatureGenome{}, a, b, rng);
    };
    const CreatureGenome c1 = breed(7);
    const CreatureGenome c2 = breed(7);
    EXPECT_FLOAT_EQ(c1.vision_cos_half_fov, c2.vision_cos_half_fov); // run==replay
    EXPECT_FLOAT_EQ(c1.vision_range, c2.vision_range);
    EXPECT_FLOAT_EQ(c1.hearing_range, c2.hearing_range);
    EXPECT_GE(c1.vision_cos_half_fov, bounds[0].lo);
    EXPECT_LE(c1.vision_cos_half_fov, bounds[0].hi);
    EXPECT_GE(c1.vision_range, bounds[1].lo);
    EXPECT_LE(c1.vision_range, bounds[1].hi);
    EXPECT_GE(c1.hearing_range, bounds[2].lo);
    EXPECT_LE(c1.hearing_range, bounds[2].hi);
}

//  determinism guard: breeding the sensory genes (drawn AFTER the core breed + sex draw) leaves
// the 4-gene CORE genome and the sex bit byte-identical -> the ecology hash (move_speed/gen/age) is
// unchanged for genome rosters, so this slice needs no re-pin.
TEST(CreatureReproduction, SensoryBreedingDoesNotPerturbCoreGenomeOrSex) {
    CreatureGenome a;
    a.move_speed = 2.5f;
    a.vision_range = 11.0f;
    CreatureGenome b;
    b.move_speed = 6.5f;
    b.vision_range = 39.0f;
    // Path 1: core breed + sex only (the pre- stream).
    luminumbra::core::DeterministicRng r1 = luminumbra::core::DeterministicRng::seeded(9, 3, 4);
    const CreatureGenome core1 = BreedOffspring(a, b, r1);
    const bool sex1 = (r1.next_u64() & 1ull) == 0ull;
    // Path 2: same stream, then sensory breeding appended after the sex draw.
    luminumbra::core::DeterministicRng r2 = luminumbra::core::DeterministicRng::seeded(9, 3, 4);
    CreatureGenome core2 = BreedOffspring(a, b, r2);
    const bool sex2 = (r2.next_u64() & 1ull) == 0ull;
    core2 = BreedSensoryInto(core2, a, b, r2);
    EXPECT_FLOAT_EQ(core1.move_speed, core2.move_speed);
    EXPECT_FLOAT_EQ(core1.vigilance, core2.vigilance);
    EXPECT_FLOAT_EQ(core1.hunger_threshold, core2.hunger_threshold);
    EXPECT_FLOAT_EQ(core1.size_scale, core2.size_scale);
    EXPECT_EQ(sex1, sex2);
}

// a born offspring is stamped with a PerceptionComponent expressed from its genome.
TEST(CreatureReproduction, OffspringPerceptionStampedFromGenome) {
    entt::registry r;
    auto f = spawnMate(r, 0.0f, 0.0f, true);
    auto m = spawnMate(r, 1.0f, 0.0f, false);
    // Distinct sensory genes so the child's values are clearly genome-driven (and in-bounds).
    auto& fg = r.get<Comp::CreatureGenomeComponent>(f);
    fg.vision_cos_half_fov = 0.85f;
    fg.vision_range = 35.0f;
    fg.hearing_range = 30.0f;
    auto& mg = r.get<Comp::CreatureGenomeComponent>(m);
    mg.vision_cos_half_fov = 0.80f;
    mg.vision_range = 33.0f;
    mg.hearing_range = 28.0f;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 2; ++t)
        RunMatingResolveOnTick(r, t);
    entt::entity child = entt::null;
    for (auto e : r.view<Comp::CreatureGenomeComponent>())
        if (r.get<Comp::CreatureGenomeComponent>(e).generation == 1u)
            child = e;
    ASSERT_TRUE(child != entt::null)
        << "a child should exist"; // boolean form: no entt::null_t printer
    ASSERT_TRUE(r.all_of<Comp::PerceptionComponent>(child))
        << "offspring must carry PerceptionComponent";
    const auto& gn = r.get<Comp::CreatureGenomeComponent>(child);
    const auto& pc = r.get<Comp::PerceptionComponent>(child);
    EXPECT_FLOAT_EQ(pc.vision_cos_half_fov, gn.vision_cos_half_fov); // expressed from the genome
    EXPECT_FLOAT_EQ(pc.vision_range, gn.vision_range);
    EXPECT_FLOAT_EQ(pc.ear.range, gn.hearing_range);
}

} // namespace
