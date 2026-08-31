//  the live creature-brain tick — deterministic movement emerging from the IAUS
// decisions (prey flee predators, predators hunt prey), id-ordered + libm-free.
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CreatureBrainSystem.h"
#include "components/CircadianComponents.h"
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

float xOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::TransformComponent>(e).position.x;
}

void tick(entt::registry& r, int n) {
    for (int i = 0; i < n; ++i)
        RunCreatureBrainSystemOnTick(r, 1.0f / 30.0f);
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

// A hungry predator adjacent to prey CATCHES it: the prey becomes an inert carcass and the
// predator's hunger drops.
TEST(CreatureBrainSystem, PredatorCatchesAdjacentPrey) {
    entt::registry r;
    const entt::entity pred = spawn(r, /*x*/ 0.0f, 0.0f, /*predator*/ true, /*hunger*/ 0.95f);
    const entt::entity prey = spawn(r, /*x*/ 1.0f, 0.0f, /*predator*/ false); // within catch reach
    RunCreatureBrainSystemOnTick(r, 1.0f / 30.0f);
    EXPECT_TRUE(r.get<Comp::CreatureComponent>(prey).eaten) << "adjacent prey should be caught";
    EXPECT_LT(r.get<Comp::CreatureComponent>(pred).hunger, 0.95f) << "predator should be sated";
}

// A caught carcass does not move (it is inert).
TEST(CreatureBrainSystem, EatenPreyIsInert) {
    entt::registry r;
    const entt::entity prey = spawn(r, /*x*/ 0.0f, 0.0f, /*predator*/ false);
    spawn(r, /*x*/ 5.0f, 0.0f, /*predator*/ true, /*hunger*/ 0.8f); // a threat it would flee
    r.get<Comp::CreatureComponent>(prey).eaten = true;
    tick(r, 30);
    EXPECT_NEAR(xOf(r, prey), 0.0f, 1.0e-4f) << "a carcass must not move";
}

//  Phase A: energy (the long-term sleep need) DRAINS while a creature is active.
// A fresh, mildly-hungry lone prey grazes/wanders -> energy ticks down from full.
TEST(CreatureBrainSystem, EnergyDrainsWhileActive) {
    entt::registry r;
    const entt::entity e = spawn(r, 0.0f, 0.0f, /*predator*/ false, /*hunger*/ 0.5f);
    auto& cr = r.get<Comp::CreatureComponent>(e);
    cr.stamina = 1.0f; // fresh -> will not Rest
    ASSERT_FLOAT_EQ(cr.energy, 1.0f);
    tick(r, 200);
    EXPECT_LT(r.get<Comp::CreatureComponent>(e).energy, 1.0f)
        << "being awake should tire the creature";
    EXPECT_GE(r.get<Comp::CreatureComponent>(e).energy, 0.0f) << "energy stays clamped >= 0";
}

// Energy RECOVERS while Resting (net positive vs the per-tick drain). A sated, exhausted, safe
// prey chooses Rest, which recovers energy faster than being awake drains it.
TEST(CreatureBrainSystem, EnergyRecoversWhileResting) {
    entt::registry r;
    const entt::entity e =
        spawn(r, 0.0f, 0.0f, /*predator*/ false, /*hunger*/ 0.0f); // sated -> won't graze
    auto& cr = r.get<Comp::CreatureComponent>(e);
    cr.stamina = 0.0f; // exhausted -> Rest scores highest (safe + low stamina)
    cr.energy = 0.30f;
    tick(r, 60);
    const auto& out = r.get<Comp::CreatureComponent>(e);
    EXPECT_EQ(out.last_action, static_cast<int>(luminumbra::ai::CreatureAction::Rest))
        << "a sated, exhausted, safe creature should Rest";
    EXPECT_GT(out.energy, 0.30f) << "resting should recover energy";
    EXPECT_LE(out.energy, 1.0f) << "energy stays clamped <= 1";
}

// a tired creature in its circadian OFF-phase (activity 0) and safe SLEEPS at its
// spot, recovering energy without moving.
TEST(CreatureBrainSystem, SleepsInOffPhaseWhenTired) {
    entt::registry r;
    const entt::entity e = spawn(r, 0.0f, 0.0f, /*predator*/ false, /*hunger*/ 0.0f);
    auto& cr = r.get<Comp::CreatureComponent>(e);
    cr.stamina = 1.0f; // not exhausted -> Rest util ~0, so Sleep must win on its own merits
    cr.energy = 0.20f; // tired
    auto& cc = r.emplace<Comp::CircadianComponent>(e);
    cc.activity = 0.0f; // deep off-phase (night for a diurnal creature)
    tick(r, 30);
    const auto& out = r.get<Comp::CreatureComponent>(e);
    EXPECT_EQ(out.last_action, static_cast<int>(luminumbra::ai::CreatureAction::Sleep))
        << "a tired, safe creature in its off-phase should sleep";
    EXPECT_GT(out.energy, 0.20f) << "sleeping recovers energy";
    EXPECT_NEAR(xOf(r, e), 0.0f, 1.0e-4f) << "a sleeping creature does not wander off";
}

// A creature with NO CircadianComponent (activity defaults to 1.0) NEVER sleeps, even tired,
// so worlds without circadian participants are byte-identical to before Sleep existed.
TEST(CreatureBrainSystem, NoCircadianNeverSleeps) {
    entt::registry r;
    const entt::entity e = spawn(r, 0.0f, 0.0f, /*predator*/ false, /*hunger*/ 0.0f);
    auto& cr = r.get<Comp::CreatureComponent>(e);
    cr.stamina = 1.0f;
    cr.energy = 0.05f; // very tired, but no circadian clock -> always "active"
    tick(r, 30);
    EXPECT_NE(r.get<Comp::CreatureComponent>(e).last_action,
              static_cast<int>(luminumbra::ai::CreatureAction::Sleep))
        << "without a circadian clock a creature must never sleep (byte-identical guarantee)";
}

// run == replay: identical setup + ticks -> identical final positions.
TEST(CreatureBrainSystem, Deterministic) {
    auto run = [] {
        entt::registry r;
        const entt::entity a = spawn(r, 0.0f, 0.0f, false);
        const entt::entity b = spawn(r, 4.0f, 1.0f, true, 0.7f);
        const entt::entity c = spawn(r, -3.0f, 2.0f, false, 0.3f);
        tick(r, 50);
        return std::vector<float>{
            xOf(r, a), r.get<Comp::TransformComponent>(a).position.z, xOf(r, b), xOf(r, c)};
    };
    EXPECT_EQ(run(), run());
}

} // namespace
