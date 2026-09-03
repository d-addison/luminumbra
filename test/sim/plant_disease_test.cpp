// sim.disease — the deterministic PLANT PEST/BLIGHT tick.
// Deterministic (id-ordered, two-phase snapshot/expose, fixed-point integer state,
// IEEE-sqrt distance — no RNG, no wall-clock). Gated by PlantTag + PlantHealthComponent:
// a world with none — and the canonical headless roster — does ZERO work (the
// NetworkStateHash baseline stays byte-identical).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

#include "components/CoreComponents.h"
#include "components/DiseaseComponents.h"
#include "components/PlantComponents.h"
#include "systems/PlantDiseaseSystem.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::foliage::RunPlantDiseaseOnTick;
using DState = Comp::PlantDiseaseState;

// Spawn a plant participant (PlantTag + PlantHealthComponent + Transform) at a
// position with a given health state, infection, and resistance (fixed-point).
entt::entity spawnPlant(entt::registry& r,
                        float x,
                        float z,
                        DState state = DState::Healthy,
                        std::uint16_t infection = 0,
                        std::uint16_t resistance = 0) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    r.emplace<Comp::PlantTag>(e);
    auto& h = r.emplace<Comp::PlantHealthComponent>(e);
    h.state = static_cast<std::uint8_t>(state);
    h.infection = infection;
    h.resistance = resistance;
    return e;
}

bool isState(entt::registry& r, entt::entity e, DState s) {
    return r.get<Comp::PlantHealthComponent>(e).state == static_cast<std::uint8_t>(s);
}

// ---- gating: empty roster / no participants ----

// No plants at all -> the system does nothing and reports zeros.
TEST(PlantDisease, EmptyRosterNoOp) {
    entt::registry r;
    const auto stats = RunPlantDiseaseOnTick(r, /*tick*/ 1);
    EXPECT_EQ(stats.considered, 0);
    EXPECT_EQ(stats.new_infections, 0);
    EXPECT_EQ(stats.infected, 0);
    EXPECT_EQ(stats.recovered, 0);
    EXPECT_EQ(stats.died, 0);
}

// A plant WITHOUT a PlantHealthComponent is never considered (the opt-in is health).
TEST(PlantDisease, NoHealthComponentIsIgnored) {
    entt::registry r;
    auto e = r.create();
    r.emplace<Comp::TransformComponent>(e);
    r.emplace<Comp::PlantTag>(e);
    const auto stats = RunPlantDiseaseOnTick(r, /*tick*/ 1);
    EXPECT_EQ(stats.considered, 0);
}

// ---- proximity spread ----

// An infected plant spreads to a NEARBY low-resistance plant: a single tick raises
// the neighbour's infection, and enough ticks tip it to Infected.
TEST(PlantDisease, SpreadsToNearbyLowResistanceNeighbour) {
    entt::registry r;
    spawnPlant(r, 0.0f, 0.0f, DState::Infected, /*infection*/ 800, /*resistance*/ 0);
    auto victim = spawnPlant(r, 1.0f, 0.0f, DState::Healthy, /*infection*/ 0, /*resistance*/ 0);

    // One tick must raise the victim's infection above zero (transmission happened).
    RunPlantDiseaseOnTick(r, /*tick*/ 1);
    EXPECT_GT(r.get<Comp::PlantHealthComponent>(victim).infection, 0u);

    // Keep ticking; a close, zero-resistance neighbour must eventually tip to Infected.
    bool became_infected = isState(r, victim, DState::Infected);
    for (int t = 2; t <= 20 && !became_infected; ++t) {
        RunPlantDiseaseOnTick(r, static_cast<std::uint64_t>(t));
        became_infected = isState(r, victim, DState::Infected);
    }
    EXPECT_TRUE(became_infected) << "a close zero-resistance plant must catch the blight";
}

// A HIGH-resistance (tended) neighbour resists: its admitted pressure is near zero,
// so it never crosses the infection threshold.
TEST(PlantDisease, HighResistanceNeighbourResists) {
    entt::registry r;
    spawnPlant(r, 0.0f, 0.0f, DState::Infected, /*infection*/ 1000, /*resistance*/ 0);
    // Fully resistant (1000 == 1.0): admits ZERO infection.
    auto tended = spawnPlant(r, 1.0f, 0.0f, DState::Healthy, /*infection*/ 0, /*resistance*/ 1000);

    for (int t = 1; t <= 60; ++t) {
        RunPlantDiseaseOnTick(r, static_cast<std::uint64_t>(t));
    }
    EXPECT_TRUE(isState(r, tended, DState::Healthy))
        << "a fully-resistant tended plant must not be infected";
    EXPECT_EQ(r.get<Comp::PlantHealthComponent>(tended).infection, 0u);
}

// A far-away plant (beyond the radius) is not infected even at zero resistance.
TEST(PlantDisease, OutOfRangePlantIsNotInfected) {
    entt::registry r;
    spawnPlant(r, 0.0f, 0.0f, DState::Infected, /*infection*/ 1000, /*resistance*/ 0);
    // Well beyond kDiseaseRadius (4.0).
    auto far = spawnPlant(r, 50.0f, 0.0f, DState::Healthy, /*infection*/ 0, /*resistance*/ 0);

    for (int t = 1; t <= 30; ++t) {
        RunPlantDiseaseOnTick(r, static_cast<std::uint64_t>(t));
    }
    EXPECT_TRUE(isState(r, far, DState::Healthy));
    EXPECT_EQ(r.get<Comp::PlantHealthComponent>(far).infection, 0u);
}

// ---- resolution: recover vs die ----

// A LOW-resistance infected plant eventually DIES (deterministically) once it has
// been sick past the resolution window.
TEST(PlantDisease, LowResistanceInfectedDies) {
    entt::registry r;
    auto sick = spawnPlant(r,
                           0.0f,
                           0.0f,
                           DState::Infected,
                           /*infection*/ 600,
                           /*resistance*/ 0); // below kRecoverResistance

    for (int t = 1; t <= 120; ++t) {
        RunPlantDiseaseOnTick(r, static_cast<std::uint64_t>(t));
    }
    EXPECT_TRUE(isState(r, sick, DState::Dead))
        << "a low-resistance infected plant must die at resolution";
}

// A HIGH-resistance infected plant eventually RECOVERS, and recovery RAISES its
// resistance (acquired immunity).
TEST(PlantDisease, HighResistanceInfectedRecoversAndGainsResistance) {
    entt::registry r;
    auto sick = spawnPlant(r,
                           0.0f,
                           0.0f,
                           DState::Infected,
                           /*infection*/ 600,
                           /*resistance*/ 600); // at/above kRecoverResistance
    const std::uint16_t before = r.get<Comp::PlantHealthComponent>(sick).resistance;

    for (int t = 1; t <= 120; ++t) {
        RunPlantDiseaseOnTick(r, static_cast<std::uint64_t>(t));
    }
    EXPECT_TRUE(isState(r, sick, DState::Recovered))
        << "a resistant infected plant must recover at resolution";
    EXPECT_GT(r.get<Comp::PlantHealthComponent>(sick).resistance, before)
        << "recovery must raise resistance (acquired immunity)";
}

// ---- determinism: run == replay ----

// Identical setup + identical tick sequence -> byte-identical health state.
TEST(PlantDisease, RunEqualsReplay) {
    auto run = [] {
        entt::registry r;
        // A small infected cluster + susceptible + tended neighbours.
        spawnPlant(r, 0.0f, 0.0f, DState::Infected, 700, 100);
        spawnPlant(r, 1.5f, 0.0f, DState::Healthy, 0, 50);
        spawnPlant(r, -1.0f, 1.0f, DState::Healthy, 0, 900);
        spawnPlant(r, 2.0f, 2.0f, DState::Healthy, 0, 500);
        spawnPlant(r, 0.5f, -2.5f, DState::Infected, 400, 700);

        for (int t = 1; t <= 100; ++t) {
            RunPlantDiseaseOnTick(r, static_cast<std::uint64_t>(t));
        }

        // Read out every plant's full health state in id order.
        std::vector<entt::entity> ents;
        for (auto e : r.view<Comp::PlantHealthComponent>())
            ents.push_back(e);
        std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        std::vector<std::uint32_t> out;
        for (auto e : ents) {
            const auto& h = r.get<Comp::PlantHealthComponent>(e);
            out.push_back(h.infection);
            out.push_back(h.resistance);
            out.push_back(h.state);
            out.push_back(h.infected_ticks);
        }
        return out;
    };
    EXPECT_EQ(run(), run());
}

// Determinism is INDEPENDENT of entity creation/insertion order: building the same
// plants in a different order yields the same per-position outcome (id-ordered
// traversal + two-phase snapshot make spread order-independent).
TEST(PlantDisease, OrderIndependentSpread) {
    // Helper: run a fixed scenario, return (victim infection, victim state).
    auto scenario = [](bool source_first) {
        entt::registry r;
        entt::entity src, victim;
        if (source_first) {
            src = spawnPlant(r, 0.0f, 0.0f, DState::Infected, 900, 0);
            victim = spawnPlant(r, 1.0f, 0.0f, DState::Healthy, 0, 0);
        } else {
            victim = spawnPlant(r, 1.0f, 0.0f, DState::Healthy, 0, 0);
            src = spawnPlant(r, 0.0f, 0.0f, DState::Infected, 900, 0);
        }
        (void)src;
        for (int t = 1; t <= 10; ++t) {
            RunPlantDiseaseOnTick(r, static_cast<std::uint64_t>(t));
        }
        const auto& h = r.get<Comp::PlantHealthComponent>(victim);
        return std::pair<std::uint16_t, std::uint8_t>{h.infection, h.state};
    };
    EXPECT_EQ(scenario(true), scenario(false));
}

} // namespace
