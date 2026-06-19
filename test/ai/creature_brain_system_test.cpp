// I9-ECO: the live creature-brain tick — deterministic movement emerging from the IAUS
// decisions (prey flee predators, predators hunt prey), id-ordered + libm-free.
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CreatureBrainSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::RunCreatureBrainSystemOnTick;

entt::entity spawn(entt::registry& r, float x, float z, bool predator, float hunger = 0.0f) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.is_predator = predator;
    cr.hunger = hunger;
    return e;
}

float xOf(entt::registry& r, entt::entity e) { return r.get<Comp::TransformComponent>(e).position.x; }

void tick(entt::registry& r, int n) {
    for (int i = 0; i < n; ++i) RunCreatureBrainSystemOnTick(r, 1.0f / 30.0f);
}

// No creatures -> the system is a no-op (byte-identical canonical roster).
TEST(CreatureBrainSystem, EmptyRosterNoOp) {
    entt::registry r;
    EXPECT_EQ(RunCreatureBrainSystemOnTick(r, 1.0f / 30.0f).updated, 0);
}

// Prey flees AWAY from a near predator: predator at +x, so the prey runs toward -x.
TEST(CreatureBrainSystem, PreyFleesPredator) {
    entt::registry r;
    const entt::entity prey = spawn(r, /*x*/ 0.0f, 0.0f, /*predator*/ false);
    spawn(r, /*x*/ 5.0f, 0.0f, /*predator*/ true, /*hunger*/ 0.8f);
    tick(r, 60);
    EXPECT_LT(xOf(r, prey), -0.5f) << "prey did not flee away from the predator";
}

// A hungry predator hunts TOWARD nearby prey (+x).
TEST(CreatureBrainSystem, PredatorHuntsPrey) {
    entt::registry r;
    const entt::entity pred = spawn(r, /*x*/ 0.0f, 0.0f, /*predator*/ true, /*hunger*/ 0.9f);
    spawn(r, /*x*/ 10.0f, 0.0f, /*predator*/ false);
    tick(r, 30);
    EXPECT_GT(xOf(r, pred), 0.5f) << "predator did not move toward the prey";
}

// run == replay: identical setup + ticks -> identical final positions.
TEST(CreatureBrainSystem, Deterministic) {
    auto run = [] {
        entt::registry r;
        const entt::entity a = spawn(r, 0.0f, 0.0f, false);
        const entt::entity b = spawn(r, 4.0f, 1.0f, true, 0.7f);
        const entt::entity c = spawn(r, -3.0f, 2.0f, false, 0.3f);
        tick(r, 50);
        return std::vector<float>{xOf(r, a), r.get<Comp::TransformComponent>(a).position.z,
                                  xOf(r, b), xOf(r, c)};
    };
    EXPECT_EQ(run(), run());
}

}  // namespace
