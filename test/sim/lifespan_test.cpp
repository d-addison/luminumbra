// sim.lifespan: creatures (and any opted-in entity) AGE and DIE: old age
// (age >= lifespan) OR starvation (a CreatureComponent with hunger >= 1.0).
// Deterministic (id-ordered, integer ticks, no rng), gated by MortalComponent.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/LifespanSystem.h"
#include "components/CreatureComponents.h"
#include "components/MortalComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::LifespanStats;
using luminumbra::ai::RunLifespanOnTick;

// Spawn a bare mortal entity (no creature). `lifespan` controls old-age death.
entt::entity spawnMortal(entt::registry& r, std::uint32_t lifespan, std::uint32_t age = 0) {
    auto e = r.create();
    auto& m = r.emplace<Comp::MortalComponent>(e);
    m.lifespan_ticks = lifespan;
    m.age_ticks = age;
    return e;
}

// Spawn a mortal creature so starvation (hunger) can also kill it.
entt::entity spawnMortalCreature(entt::registry& r, std::uint32_t lifespan, float hunger) {
    auto e = r.create();
    auto& m = r.emplace<Comp::MortalComponent>(e);
    m.lifespan_ticks = lifespan;
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.hunger = hunger;
    return e;
}

// Empty roster: the tick is a no-op.
TEST(Lifespan, EmptyRosterNoOp) {
    entt::registry r;
    const LifespanStats s = RunLifespanOnTick(r, 1);
    EXPECT_EQ(s.aged, 0);
    EXPECT_EQ(s.died, 0);
}

// An entity past its lifespan is marked dead.
TEST(Lifespan, PastLifespanDies) {
    entt::registry r;
    // lifespan 5, already aged to 4: one tick -> age 5 >= 5 -> dies.
    const entt::entity e = spawnMortal(r, /*lifespan*/ 5, /*age*/ 4);
    const LifespanStats s = RunLifespanOnTick(r, 1);
    EXPECT_EQ(s.died, 1);
    EXPECT_EQ(r.get<Comp::MortalComponent>(e).dead, 1u);
}

// A young entity (well below lifespan) is NOT marked dead.
TEST(Lifespan, YoungSurvives) {
    entt::registry r;
    const entt::entity e = spawnMortal(r, /*lifespan*/ 1000, /*age*/ 0);
    for (int i = 0; i < 100; ++i)
        RunLifespanOnTick(r, static_cast<std::uint64_t>(i));
    EXPECT_EQ(r.get<Comp::MortalComponent>(e).dead, 0u);
    EXPECT_EQ(r.get<Comp::MortalComponent>(e).age_ticks, 100u);
}

// A starving creature (hunger >= 1.0) dies even when far from its lifespan.
TEST(Lifespan, StarvationKills) {
    entt::registry r;
    const entt::entity e = spawnMortalCreature(r, /*lifespan*/ 0xFFFFFFFFu, /*hunger*/ 1.0f);
    const LifespanStats s = RunLifespanOnTick(r, 1);
    EXPECT_EQ(s.died, 1);
    EXPECT_EQ(r.get<Comp::MortalComponent>(e).dead, 1u);
    // The carcass seam is set so the brain/markers treat it as inert.
    EXPECT_TRUE(r.get<Comp::CreatureComponent>(e).eaten);
}

// A well-fed creature (hunger below 1.0) does not starve.
TEST(Lifespan, WellFedCreatureSurvives) {
    entt::registry r;
    const entt::entity e = spawnMortalCreature(r, /*lifespan*/ 0xFFFFFFFFu, /*hunger*/ 0.5f);
    for (int i = 0; i < 50; ++i)
        RunLifespanOnTick(r, static_cast<std::uint64_t>(i));
    EXPECT_EQ(r.get<Comp::MortalComponent>(e).dead, 0u);
    EXPECT_FALSE(r.get<Comp::CreatureComponent>(e).eaten);
}

// Aging is monotonic up to death, and a dead entity stops aging (latched).
TEST(Lifespan, AgingMonotonicThenLatches) {
    entt::registry r;
    const entt::entity e = spawnMortal(r, /*lifespan*/ 3, /*age*/ 0);
    std::uint32_t prev = r.get<Comp::MortalComponent>(e).age_ticks;
    for (int i = 0; i < 10; ++i) {
        RunLifespanOnTick(r, static_cast<std::uint64_t>(i));
        const auto& m = r.get<Comp::MortalComponent>(e);
        EXPECT_GE(m.age_ticks, prev) << "age must never decrease";
        prev = m.age_ticks;
    }
    const auto& m = r.get<Comp::MortalComponent>(e);
    EXPECT_EQ(m.dead, 1u);
    // Once dead at age == lifespan it must stop advancing (stable for replay).
    EXPECT_EQ(m.age_ticks, 3u);
}

// A dead entity is never "aged" again (stats.aged excludes it).
TEST(Lifespan, DeadEntityNotAged) {
    entt::registry r;
    spawnMortal(r, /*lifespan*/ 1, /*age*/ 0); // dies on tick 1 (age 1 >= 1)
    const LifespanStats first = RunLifespanOnTick(r, 1);
    EXPECT_EQ(first.aged, 1);
    EXPECT_EQ(first.died, 1);
    const LifespanStats second = RunLifespanOnTick(r, 2);
    EXPECT_EQ(second.aged, 0) << "a latched-dead entity is inert";
    EXPECT_EQ(second.died, 0);
}

// run == replay: identical setup + ticks -> identical ages + death flags.
TEST(Lifespan, Deterministic) {
    auto run = [] {
        entt::registry r;
        spawnMortal(r, 7, 0);
        spawnMortal(r, 50, 10);
        spawnMortalCreature(r, 0xFFFFFFFFu, 1.0f); // starves immediately
        spawnMortalCreature(r, 1000, 0.2f);        // long-lived, well-fed
        for (std::uint64_t t = 0; t < 60; ++t)
            RunLifespanOnTick(r, t);
        std::vector<std::uint32_t> out;
        for (auto e : r.view<Comp::MortalComponent>()) {
            const auto& m = r.get<Comp::MortalComponent>(e);
            out.push_back(m.age_ticks);
            out.push_back(m.dead);
        }
        return out;
    };
    EXPECT_EQ(run(), run());
}

} // namespace
