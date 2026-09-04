// sim.thirst: creatures get THIRSTY and seek water (complements hunger). Thirst rises
// each tick; a thirsty creature steers toward the nearest water hole; inside a hole's radius
// it drinks (thirst falls, drinking=1). Deterministic (id-ordered, DeterministicMath only,
// NO rng), gated by the ThirstComponent opt-in.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/ThirstSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/ThirstComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::kThirstSeekThreshold;
using luminumbra::ai::RunThirstOnTick;
using luminumbra::ai::ThirstStats;

// One fixed sim tick at 30Hz (matches the engine's fixed dt; constants are tuned for it).
constexpr float kDt = Luminumbra::SECONDS_PER_TICK;

// Spawn a creature carrying a ThirstComponent (the opt-in). `thirst` seeds the state;
// `eaten` makes it a carcass (neither thirsts nor drinks).
entt::entity
spawnCreature(entt::registry& r, float x, float z, float thirst = 0.0f, bool eaten = false) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& cr = r.emplace<Comp::CreatureComponent>(e);
    cr.eaten = eaten;
    auto& th = r.emplace<Comp::ThirstComponent>(e);
    th.thirst = thirst;
    return e;
}

// Spawn a water hole (WaterHoleComponent + TransformComponent).
entt::entity spawnHole(entt::registry& r, float x, float z, float radius = 4.0f) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& wh = r.emplace<Comp::WaterHoleComponent>(e);
    wh.radius = radius;
    return e;
}

const Comp::ThirstComponent& thirstOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::ThirstComponent>(e);
}

// ---- gating / no-op ----

TEST(Thirst, EmptyRosterNoOp) {
    entt::registry r;
    ThirstStats s = RunThirstOnTick(r, kDt);
    EXPECT_EQ(s.participants, 0);
    EXPECT_EQ(s.seeking, 0);
    EXPECT_EQ(s.drinking, 0);
}

// A creature without a ThirstComponent is invisible to the system (opt-in gate).
TEST(Thirst, NonParticipantsIgnored) {
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(0.0f, 0.0f, 0.0f);
    r.emplace<Comp::CreatureComponent>(e);
    // A water hole present but no thirst participants.
    spawnHole(r, 5.0f, 0.0f);
    ThirstStats s = RunThirstOnTick(r, kDt);
    EXPECT_EQ(s.participants, 0);
}

// ---- core: thirst rises over ticks ----

TEST(Thirst, RisesOverTicks) {
    entt::registry r;
    auto e = spawnCreature(r, 0.0f, 0.0f, /*thirst=*/0.0f);

    const float t0 = thirstOf(r, e).thirst;
    RunThirstOnTick(r, kDt);
    const float t1 = thirstOf(r, e).thirst;
    RunThirstOnTick(r, kDt);
    const float t2 = thirstOf(r, e).thirst;

    EXPECT_GT(t1, t0);
    EXPECT_GT(t2, t1);
    EXPECT_LE(t2, 1.0f); // never exceeds parched.
}

// Thirst saturates at 1 (clamped) over many ticks.
TEST(Thirst, SaturatesAtOne) {
    entt::registry r;
    auto e = spawnCreature(r, 0.0f, 0.0f, /*thirst=*/0.95f);
    for (int i = 0; i < 200; ++i)
        RunThirstOnTick(r, kDt);
    EXPECT_LE(thirstOf(r, e).thirst, 1.0f);
    EXPECT_GE(thirstOf(r, e).thirst, 0.999f);
}

// ---- a thirsty creature's wish points toward the nearest water hole ----

TEST(Thirst, WishPointsTowardNearestHole) {
    entt::registry r;
    // Creature at origin, already thirsty enough to seek. A hole to the +x side, far enough
    // that the creature is OUTSIDE its radius (so it steers rather than drinks).
    auto e = spawnCreature(r, 0.0f, 0.0f, /*thirst=*/0.8f);
    spawnHole(r, 20.0f, 0.0f, /*radius=*/4.0f);

    ThirstStats s = RunThirstOnTick(r, kDt);
    const auto& th = thirstOf(r, e);

    EXPECT_GE(s.seeking, 1);
    EXPECT_GT(th.wish_x, 0.0f);            // points toward +x (the hole).
    EXPECT_NEAR(th.wish_z, 0.0f, 1.0e-5f); // no z component for an on-axis hole.
    EXPECT_EQ(th.drinking, 0);             // outside the radius: not drinking.
    // Magnitude scaled by thirst (unit direction * thirst).
    const float mag = std::sqrt(th.wish_x * th.wish_x + th.wish_z * th.wish_z);
    EXPECT_NEAR(mag, th.thirst, 1.0e-5f);
}

// Of two holes, the wish steers toward the NEARER one.
TEST(Thirst, SteersTowardNearerOfTwoHoles) {
    entt::registry r;
    auto e = spawnCreature(r, 0.0f, 0.0f, /*thirst=*/0.9f);
    spawnHole(r, 30.0f, 0.0f, /*radius=*/4.0f);  // far, +x
    spawnHole(r, 0.0f, -10.0f, /*radius=*/2.0f); // nearer, -z

    RunThirstOnTick(r, kDt);
    const auto& th = thirstOf(r, e);
    // Nearer hole is along -z, so the dominant wish component is negative z.
    EXPECT_LT(th.wish_z, 0.0f);
    EXPECT_LT(std::fabs(th.wish_x), std::fabs(th.wish_z));
}

// Below the seek threshold the creature does not steer (wish stays zero) even with a hole.
TEST(Thirst, DoesNotSeekWhenBarelyThirsty) {
    entt::registry r;
    auto e = spawnCreature(r, 0.0f, 0.0f, /*thirst=*/0.0f); // essentially quenched
    spawnHole(r, 20.0f, 0.0f, /*radius=*/4.0f);

    RunThirstOnTick(r, kDt); // one tick: thirst still well below threshold
    const auto& th = thirstOf(r, e);
    ASSERT_LT(th.thirst, kThirstSeekThreshold);
    EXPECT_FLOAT_EQ(th.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(th.wish_z, 0.0f);
}

// ---- inside a hole it drinks: thirst falls, drinking=1 ----

TEST(Thirst, DrinksInsideHole) {
    entt::registry r;
    // Creature sits AT a water hole (well within radius) and is thirsty.
    auto e = spawnCreature(r, 0.0f, 0.0f, /*thirst=*/0.8f);
    spawnHole(r, 1.0f, 0.0f, /*radius=*/4.0f);

    const float before = thirstOf(r, e).thirst;
    ThirstStats s = RunThirstOnTick(r, kDt);
    const auto& th = thirstOf(r, e);

    EXPECT_EQ(s.drinking, 1);
    EXPECT_EQ(th.drinking, 1);
    EXPECT_LT(th.thirst, before);     // drinking outpaces the per-tick rise.
    EXPECT_GE(th.thirst, 0.0f);       // never negative.
    EXPECT_FLOAT_EQ(th.wish_x, 0.0f); // arrived: no steering wish while drinking.
    EXPECT_FLOAT_EQ(th.wish_z, 0.0f);
}

// Drinking over many ticks quenches thirst to ~0 (clamped).
TEST(Thirst, DrinkingQuenchesToZero) {
    entt::registry r;
    auto e = spawnCreature(r, 0.0f, 0.0f, /*thirst=*/1.0f);
    spawnHole(r, 0.0f, 0.0f, /*radius=*/4.0f);
    for (int i = 0; i < 100; ++i)
        RunThirstOnTick(r, kDt);
    EXPECT_GE(thirstOf(r, e).thirst, 0.0f);
    EXPECT_LT(thirstOf(r, e).thirst, 0.05f);
}

// ---- a creature with NO water hole keeps a zero wish but still gets thirsty ----

TEST(Thirst, NoHoleStillThirstyZeroWish) {
    entt::registry r;
    auto e = spawnCreature(r, 0.0f, 0.0f, /*thirst=*/0.9f); // very thirsty, no water anywhere

    const float before = thirstOf(r, e).thirst;
    RunThirstOnTick(r, kDt);
    const auto& th = thirstOf(r, e);

    EXPECT_GT(th.thirst, before);     // still gets thirstier.
    EXPECT_FLOAT_EQ(th.wish_x, 0.0f); // nowhere to go.
    EXPECT_FLOAT_EQ(th.wish_z, 0.0f);
    EXPECT_EQ(th.drinking, 0);
}

// ---- a carcass neither thirsts nor drinks ----

TEST(Thirst, CarcassInert) {
    entt::registry r;
    auto e = spawnCreature(r, 0.0f, 0.0f, /*thirst=*/0.6f, /*eaten=*/true);
    spawnHole(r, 0.5f, 0.0f, /*radius=*/4.0f);

    const float before = thirstOf(r, e).thirst;
    RunThirstOnTick(r, kDt);
    const auto& th = thirstOf(r, e);

    EXPECT_FLOAT_EQ(th.thirst, before); // thirst unchanged (no rise, no drink).
    EXPECT_FLOAT_EQ(th.wish_x, 0.0f);
    EXPECT_FLOAT_EQ(th.wish_z, 0.0f);
    EXPECT_EQ(th.drinking, 0);
}

// ---- determinism: run == replay (bit-for-bit over many ticks) ----

TEST(Thirst, RunEqualsReplay) {
    auto simulate = []() {
        entt::registry r;
        std::vector<entt::entity> es;
        es.push_back(spawnCreature(r, 0.0f, 0.0f, 0.0f));
        es.push_back(spawnCreature(r, 5.0f, 2.0f, 0.4f));
        es.push_back(spawnCreature(r, -7.0f, 3.0f, 0.7f));
        es.push_back(spawnCreature(r, 12.0f, -4.0f, 0.2f));
        spawnHole(r, 10.0f, 0.0f, 4.0f);
        spawnHole(r, -3.0f, -8.0f, 3.0f);

        std::vector<float> trace;
        for (int t = 0; t < 60; ++t) {
            RunThirstOnTick(r, kDt);
            for (auto e : es) {
                const auto& th = r.get<Comp::ThirstComponent>(e);
                trace.push_back(th.thirst);
                trace.push_back(th.wish_x);
                trace.push_back(th.wish_z);
                trace.push_back(static_cast<float>(th.drinking));
            }
        }
        return trace;
    };

    const std::vector<float> run = simulate();
    const std::vector<float> replay = simulate();
    ASSERT_EQ(run.size(), replay.size());
    for (std::size_t i = 0; i < run.size(); ++i) {
        EXPECT_EQ(run[i], replay[i]) << "divergence at sample " << i;
    }
}

// ---- order-independence: shuffled creation order yields the same per-creature state ----

TEST(Thirst, OrderIndependent) {
    struct Spec {
        float x, z, thirst;
    };
    std::vector<Spec> specs = {
        {0.0f, 0.0f, 0.5f},
        {6.0f, 0.0f, 0.6f},
        {2.0f, 5.0f, 0.8f},
        {-4.0f, -2.0f, 0.3f},
    };

    auto build = [](const std::vector<Spec>& order) {
        entt::registry r;
        std::vector<entt::entity> es;
        for (const auto& sp : order) {
            es.push_back(spawnCreature(r, sp.x, sp.z, sp.thirst));
        }
        spawnHole(r, 8.0f, 1.0f, 4.0f);
        for (int t = 0; t < 10; ++t)
            RunThirstOnTick(r, kDt);
        std::vector<std::pair<std::pair<float, float>, std::vector<float>>> out;
        for (std::size_t i = 0; i < order.size(); ++i) {
            const auto& tf = r.get<Comp::TransformComponent>(es[i]);
            const auto& th = r.get<Comp::ThirstComponent>(es[i]);
            out.push_back({{tf.position.x, tf.position.z},
                           {th.thirst, th.wish_x, th.wish_z, static_cast<float>(th.drinking)}});
        }
        std::sort(
            out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return out;
    };

    auto forward = build(specs);
    std::reverse(specs.begin(), specs.end());
    auto reversed = build(specs);

    ASSERT_EQ(forward.size(), reversed.size());
    for (std::size_t i = 0; i < forward.size(); ++i) {
        EXPECT_EQ(forward[i].first, reversed[i].first);
        ASSERT_EQ(forward[i].second.size(), reversed[i].second.size());
        for (std::size_t j = 0; j < forward[i].second.size(); ++j) {
            EXPECT_FLOAT_EQ(forward[i].second[j], reversed[i].second[j]);
        }
    }
}

} // namespace
