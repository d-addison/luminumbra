// Track (a) — SEXUAL creature reproduction: a male + a female must find each other and COURT
// before a baby is born. Deterministic (id-ordered, seeded-from-ints, libm-free), gated by the
// CreatureGenomeComponent opt-in.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CreatureReproductionSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::RunMateSeekingOnTick;
using luminumbra::ai::RunMatingResolveOnTick;
using luminumbra::ai::kCourtshipTicks;

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
        gn.age_ticks = 120;          // mature (>= kReproMaturityTicks)
        gn.reproduce_cooldown = 0;
        cr.hunger = 0.05f;           // <= hunger_threshold (0.3)
        cr.stamina = 1.0f;           // >= healthy floor
    } else {
        gn.age_ticks = 0;            // too young
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
    r.emplace<Comp::CreatureComponent>(e);  // no CreatureGenomeComponent
    for (int i = 0; i < 200; ++i) RunMatingResolveOnTick(r, static_cast<std::uint64_t>(i));
    EXPECT_EQ(creatureCount(r), 1u);
}

// An adjacent ready male+female court for the duration, then produce exactly one baby; both
// parents go on cooldown afterwards.
TEST(CreatureReproduction, AdjacentPairCourtsThenBreeds) {
    entt::registry r;
    const entt::entity female = spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    const entt::entity male = spawnMate(r, 1.0f, 0.0f, /*female*/ false);  // within courtship radius
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
    for (std::uint32_t t = 0; t < kCourtshipTicks + 50; ++t) born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 0);
    EXPECT_EQ(r.get<Comp::CreatureGenomeComponent>(female).courting_ticks, 0u);
}

// Two ready FEMALES (same sex) never breed.
TEST(CreatureReproduction, SameSexDoesNotBreed) {
    entt::registry r;
    spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    spawnMate(r, 1.0f, 0.0f, /*female*/ true);
    int born = 0;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 10; ++t) born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 0);
}

// Immature creatures don't court (age gate).
TEST(CreatureReproduction, YoungPairDoesNotBreed) {
    entt::registry r;
    spawnMate(r, 0.0f, 0.0f, /*female*/ true, /*ready*/ false);
    spawnMate(r, 1.0f, 0.0f, /*female*/ false, /*ready*/ false);
    int born = 0;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 5; ++t) born += RunMatingResolveOnTick(r, t).born;
    EXPECT_EQ(born, 0);
}

// Mate seeking: a ready female steers TOWARD a ready male within sense range (+x).
TEST(CreatureReproduction, MateSeekingSteersTowardMate) {
    entt::registry r;
    const entt::entity female = spawnMate(r, 0.0f, 0.0f, /*female*/ true);
    spawnMate(r, 12.0f, 0.0f, /*female*/ false);  // far but within kMateSeekRadius
    RunMateSeekingOnTick(r);
    EXPECT_GT(r.get<Comp::CreatureComponent>(female).wish_x, 0.0f) << "should steer toward the mate";
}

// Offspring genome is a blend of the two parents (between their move_speeds).
TEST(CreatureReproduction, OffspringGenomeBlendsParents) {
    entt::registry r;
    auto f = spawnMate(r, 0.0f, 0.0f, true);
    auto m = spawnMate(r, 1.0f, 0.0f, false);
    r.get<Comp::CreatureGenomeComponent>(f).move_speed = 2.0f;
    r.get<Comp::CreatureGenomeComponent>(m).move_speed = 6.0f;
    for (std::uint32_t t = 0; t < kCourtshipTicks + 2; ++t) RunMatingResolveOnTick(r, t);
    float childSpeed = -1.0f;
    for (auto e : r.view<Comp::CreatureGenomeComponent>()) {
        const auto& gn = r.get<Comp::CreatureGenomeComponent>(e);
        if (gn.generation == 1u) childSpeed = gn.move_speed;
    }
    ASSERT_GT(childSpeed, 0.0f) << "a child should exist";
    EXPECT_GT(childSpeed, 2.0f - 1.0f);
    EXPECT_LT(childSpeed, 6.0f + 1.0f);  // blended between parents (+ small mutation)
}

// run == replay: identical setup + ticks -> identical population + genomes.
TEST(CreatureReproduction, Deterministic) {
    auto run = [] {
        entt::registry r;
        spawnMate(r, 0.0f, 0.0f, true);
        spawnMate(r, 1.0f, 0.0f, false);
        for (std::uint32_t t = 0; t < kCourtshipTicks + 20; ++t) RunMatingResolveOnTick(r, t);
        std::vector<float> out;
        out.push_back(static_cast<float>(r.view<Comp::CreatureComponent>().size()));
        for (auto e : r.view<Comp::CreatureGenomeComponent>())
            out.push_back(r.get<Comp::CreatureGenomeComponent>(e).move_speed);
        return out;
    };
    EXPECT_EQ(run(), run());
}

}  // namespace
