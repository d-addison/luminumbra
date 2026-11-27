// sim.lifespan: creatures (and any opted-in entity) AGE and DIE: old age
// (age >= lifespan) OR starvation (a CreatureComponent with hunger >= 1.0).
// Deterministic (id-ordered, integer ticks, no rng), gated by MortalComponent.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
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

TEST(Lifespan, RegionalElapsedStopsAtDeathAndMatchesSingleTicks) {
    namespace W = ::Luminumbra::world;
    namespace A = luminumbra::ai;
    for (const std::uint32_t steps : {0u, 1u, 8u, 36000u, 36001u, 65536u}) {
        SCOPED_TRACE(steps);
        entt::registry actual, reference;
        auto seed = [](entt::registry& registry) {
            const std::array entities{spawnMortal(registry, 5),
                                      spawnMortal(registry, 0xFFFFFFFFu, 0xFFFFFFFDu),
                                      spawnMortal(registry, 0xFFFFFFFFu, 0xFFFFFFFFu),
                                      spawnMortal(registry, 20, 3),
                                      spawnMortal(registry, 0),
                                      spawnMortalCreature(registry, 1000000, 1.0f),
                                      spawnMortalCreature(registry, 1000000, 0.5f),
                                      spawnMortalCreature(registry, 1000000, 1.0f)};
            registry.get<Comp::MortalComponent>(entities[3]).dead = 1;
            for (std::size_t index = 0; index < 5; ++index)
                registry.emplace<Comp::PlantRegionComponent>(entities[index]);
            for (std::size_t index = 5; index < entities.size(); ++index)
                registry.emplace<Comp::TransformComponent>(entities[index]).position.x =
                    index == 7 ? 512.0f : 0.0f;
            return entities;
        };
        const auto entities = seed(actual);
        const auto expected = seed(reference);
        const std::array<W::RegionKey, 1> due{{{0, 0}}};
        const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, steps}};
        const W::RegionConsumerGate gate(actual, due, W::RegionElapsedTicks(windows));
        const W::RegionConsumerGate single(reference, due);
        A::CreatureTickAdmission captured, reference_capture;
        captured.Capture(actual, gate);
        reference_capture.Capture(reference, single);
        actual.get<Comp::TransformComponent>(entities[6]).position.x = 512;
        actual.get<Comp::TransformComponent>(entities[7]).position.x = 0;
        reference.get<Comp::TransformComponent>(expected[6]).position.x = 512;
        reference.get<Comp::TransformComponent>(expected[7]).position.x = 0;
        const auto stats = RunLifespanOnTick<true>(actual, 65536, gate, &captured);
        int deaths = 0;
        for (std::uint32_t step = 0; step < steps; ++step)
            deaths +=
                RunLifespanOnTick(reference, 65537 - steps + step, single, &reference_capture).died;
        EXPECT_EQ(stats.died, deaths);
        EXPECT_LE(gate.owner_visits(), 40u);
        for (std::size_t index = 0; index < entities.size(); ++index) {
            const auto& got = actual.get<Comp::MortalComponent>(entities[index]);
            const auto& want = reference.get<Comp::MortalComponent>(expected[index]);
            EXPECT_EQ(got.age_ticks, want.age_ticks);
            EXPECT_EQ(got.dead, want.dead);
            if (index >= 5) {
                EXPECT_EQ(actual.get<Comp::CreatureComponent>(entities[index]).eaten,
                          reference.get<Comp::CreatureComponent>(expected[index]).eaten);
            }
        }
        if (steps >= 8) {
            EXPECT_EQ(RunLifespanOnTick<true>(actual, 65536, gate, &captured).died, 0);
        }
        EXPECT_EQ(actual.get<Comp::MortalComponent>(entities[7]).age_ticks, 0u);
    }
}

TEST(Lifespan, RegionalElapsedNewbornAndReusedHandleHaveOnlyCurrentTick) {
    namespace W = ::Luminumbra::world;
    entt::registry registry;
    const auto original = spawnMortalCreature(registry, 1000, 0.5f);
    registry.emplace<Comp::TransformComponent>(original);
    const std::array<W::RegionKey, 1> due{{{0, 0}}};
    const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, 8}};
    const W::RegionConsumerGate gate(registry, due, W::RegionElapsedTicks(windows));
    luminumbra::ai::CreatureTickAdmission admission;
    admission.Capture(registry, gate);
    registry.destroy(original);
    const auto reused = spawnMortalCreature(registry, 1000, 0.5f);
    const auto newborn = spawnMortalCreature(registry, 1000, 0.5f);
    RunLifespanOnTick<true>(registry, 8, gate, &admission);
    EXPECT_EQ(registry.get<Comp::MortalComponent>(reused).age_ticks, 1u);
    EXPECT_EQ(registry.get<Comp::MortalComponent>(newborn).age_ticks, 1u);
    // Without a capture, an admitted creature uses its due-time owner window.
    entt::registry uncaptured;
    const auto creature = spawnMortalCreature(uncaptured, 1000, 0.5f);
    uncaptured.emplace<Comp::TransformComponent>(creature);
    const W::RegionConsumerGate current(uncaptured, due, W::RegionElapsedTicks(windows));
    RunLifespanOnTick<true>(uncaptured, 8, current);
    EXPECT_EQ(uncaptured.get<Comp::MortalComponent>(creature).age_ticks, 8u);
    // A default gate remains a single tick even on the explicit elapsed entry.
    RunLifespanOnTick<true>(uncaptured);
    EXPECT_EQ(uncaptured.get<Comp::MortalComponent>(creature).age_ticks, 9u);
    entt::registry empty;
    EXPECT_EQ(RunLifespanOnTick<true>(empty).aged, 0);
}

TEST(Lifespan, RegionalOversizedWindowRefusesWholeRosterBeforeMutation) {
    namespace W = ::Luminumbra::world;
    // Bare mortal, uncaptured creature, and captured creature owner windows.
    for (const int mode : {0, 1, 2}) {
        const bool capture = mode == 2;
        for (const bool invalid_dead : {false, true}) {
            for (const auto ticks : {65537u, 0xFFFFFFFFu}) {
                SCOPED_TRACE(mode);
                SCOPED_TRACE(invalid_dead);
                SCOPED_TRACE(ticks);
                entt::registry registry;
                const auto valid = spawnMortalCreature(registry, 1, 1.0f);
                const auto invalid = spawnMortalCreature(registry, 5, 1.0f);
                const auto plant = spawnMortal(registry, 5, 2);
                registry.emplace<Comp::PlantRegionComponent>(valid).owner_x = 0;
                registry.emplace<Comp::PlantRegionComponent>(invalid).owner_x = mode == 0 ? 0 : 1;
                registry.emplace<Comp::PlantRegionComponent>(plant).owner_x = mode == 0 ? 1 : 0;
                registry.get<Comp::MortalComponent>(mode == 0 ? plant : invalid).dead =
                    invalid_dead ? 1 : 0;
                const std::array<W::RegionKey, 2> due{{{0, 0}, {1, 0}}};
                const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, 1}, {{1, 0}, ticks}};
                const W::RegionConsumerGate gate(registry, due, W::RegionElapsedTicks(windows));
                luminumbra::ai::CreatureTickAdmission admission;
                admission.Capture(registry, gate);
                EXPECT_THROW(
                    RunLifespanOnTick<true>(registry, ticks, gate, capture ? &admission : nullptr),
                    std::invalid_argument);
                EXPECT_EQ(registry.get<Comp::MortalComponent>(valid).age_ticks, 0u);
                EXPECT_EQ(registry.get<Comp::MortalComponent>(valid).dead, 0u);
                EXPECT_FALSE(registry.get<Comp::CreatureComponent>(valid).eaten);
                EXPECT_EQ(registry.get<Comp::MortalComponent>(invalid).age_ticks, 0u);
                EXPECT_EQ(registry.get<Comp::MortalComponent>(invalid).dead,
                          mode != 0 && invalid_dead ? 1u : 0u);
                EXPECT_FALSE(registry.get<Comp::CreatureComponent>(invalid).eaten);
                EXPECT_EQ(registry.get<Comp::MortalComponent>(plant).age_ticks, 2u);
                EXPECT_EQ(registry.get<Comp::MortalComponent>(plant).dead,
                          mode == 0 && invalid_dead ? 1u : 0u);
                // The legacy entry still ignores observational elapsed values.
                EXPECT_EQ(RunLifespanOnTick(registry, ticks, gate).aged, invalid_dead ? 2 : 3);
            }
        }
    }
}

TEST(Lifespan, RegionalDeniedOversizedWindowIsIgnored) {
    namespace W = ::Luminumbra::world;
    entt::registry registry;
    const auto denied = spawnMortal(registry, 5);
    registry.emplace<Comp::PlantRegionComponent>(denied).owner_x = 1;
    const std::array<W::RegionKey, 1> due{{{0, 0}}};
    const std::map<W::RegionKey, std::uint32_t> windows{{{1, 0}, 0xFFFFFFFFu}};
    const W::RegionConsumerGate gate(registry, due, W::RegionElapsedTicks(windows));
    EXPECT_EQ(RunLifespanOnTick<true>(registry, 1, gate).aged, 0);
    EXPECT_EQ(registry.get<Comp::MortalComponent>(denied).age_ticks, 0u);
}

} // namespace
