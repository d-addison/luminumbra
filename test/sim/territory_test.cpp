// sim.territory: HOME-RANGE / TERRITORIALITY. A creature claims its spawn position as
// HOME on the first tick, then prefers to stay near it: the system emits a HOMING wish bias
// that is zero inside the territory radius and points TOWARD home (growing with distance)
// outside it. Deterministic (id-ordered, two-phase, DeterministicMath only, NO rng), gated by
// the TerritoryComponent opt-in.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

#include <entt/entt.hpp>

#include "ai/TerritorySystem.h"
#include "components/CoreComponents.h"
#include "components/TerritoryComponents.h"
#include "core/DeterministicMath.h"

namespace {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;
using luminumbra::ai::kTerritoryMaxBias;
using luminumbra::ai::RunTerritoryOnTick;
using luminumbra::ai::TerritoryStats;

// Spawn a creature carrying a TerritoryComponent (the opt-in) at (x,z) with the given radius.
entt::entity spawnTerritorial(entt::registry& r, float x, float z, float radius = 20.0f) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    auto& terr = r.emplace<Comp::TerritoryComponent>(e);
    terr.radius = radius;
    return e;
}

// Move an existing creature to a new position (simulating it straying from home).
void moveTo(entt::registry& r, entt::entity e, float x, float z) {
    r.get<Comp::TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
}

const Comp::TerritoryComponent& terrOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::TerritoryComponent>(e);
}
const Comp::TerritoryBiasComponent& biasOf(entt::registry& r, entt::entity e) {
    return r.get<Comp::TerritoryBiasComponent>(e);
}

// ---- gating / no-op ----

// Empty roster: the system reports nothing and creates/changes nothing.
TEST(Territory, EmptyRosterNoOp) {
    entt::registry r;
    TerritoryStats s = RunTerritoryOnTick(r, 0);
    EXPECT_EQ(s.participants, 0);
    EXPECT_EQ(s.claimed, 0);
    EXPECT_EQ(s.homing, 0);
}

// Creatures WITHOUT a TerritoryComponent are invisible (opt-in gate): a world of
// non-participants is a pure no-op and no TerritoryBiasComponent is ever attached.
TEST(Territory, NonParticipantsIgnored) {
    entt::registry r;
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position = Luminumbra::Vec3(5.0f, 0.0f, 7.0f);
    TerritoryStats s = RunTerritoryOnTick(r, 0);
    EXPECT_EQ(s.participants, 0);
    EXPECT_FALSE(r.any_of<Comp::TerritoryBiasComponent>(e));
}

// ---- claiming home ----

// On the first tick a creature claims its CURRENT position as home and marks established; on
// the claiming tick it is AT home, so its homing bias is zero.
TEST(Territory, ClaimsHomeOnFirstTick) {
    entt::registry r;
    auto e = spawnTerritorial(r, 12.0f, -8.0f);

    EXPECT_EQ(terrOf(r, e).established, 0);
    TerritoryStats s = RunTerritoryOnTick(r, 0);

    EXPECT_EQ(s.participants, 1);
    EXPECT_EQ(s.claimed, 1);
    const auto& terr = terrOf(r, e);
    EXPECT_EQ(terr.established, 1);
    EXPECT_FLOAT_EQ(terr.home_x, 12.0f);
    EXPECT_FLOAT_EQ(terr.home_z, -8.0f);

    // At home on the claiming tick -> zero homing bias.
    EXPECT_FLOAT_EQ(biasOf(r, e).wish_x, 0.0f);
    EXPECT_FLOAT_EQ(biasOf(r, e).wish_z, 0.0f);
    EXPECT_EQ(s.homing, 0);
}

// A creature does NOT re-claim home on later ticks: home stays fixed at the original spawn
// even after the creature moves.
TEST(Territory, HomeStaysFixedAfterClaim) {
    entt::registry r;
    auto e = spawnTerritorial(r, 0.0f, 0.0f, 20.0f);
    RunTerritoryOnTick(r, 0); // claim home at origin

    moveTo(r, e, 100.0f, 100.0f);
    TerritoryStats s = RunTerritoryOnTick(r, 1);

    EXPECT_EQ(s.claimed, 0); // already established
    const auto& terr = terrOf(r, e);
    EXPECT_FLOAT_EQ(terr.home_x, 0.0f);
    EXPECT_FLOAT_EQ(terr.home_z, 0.0f);
}

// ---- bias behaviour ----

// Inside the radius the homing bias is ~zero (the creature roams its home range freely).
TEST(Territory, InsideRadiusZeroBias) {
    entt::registry r;
    auto e = spawnTerritorial(r, 0.0f, 0.0f, 20.0f);
    RunTerritoryOnTick(r, 0); // claim home at origin

    // Move to (10,0): distance 10 < radius 20 -> inside.
    moveTo(r, e, 10.0f, 0.0f);
    TerritoryStats s = RunTerritoryOnTick(r, 1);

    EXPECT_FLOAT_EQ(biasOf(r, e).wish_x, 0.0f);
    EXPECT_FLOAT_EQ(biasOf(r, e).wish_z, 0.0f);
    EXPECT_EQ(s.homing, 0);
}

// Outside the radius the bias points TOWARD home. Home at origin, creature at +X -> bias is
// in the -X direction (back toward home), with no Z component on the axis.
TEST(Territory, OutsideRadiusPointsTowardHome) {
    entt::registry r;
    auto e = spawnTerritorial(r, 0.0f, 0.0f, 20.0f);
    RunTerritoryOnTick(r, 0); // claim home at origin

    moveTo(r, e, 30.0f, 0.0f); // distance 30 > radius 20 -> outside
    TerritoryStats s = RunTerritoryOnTick(r, 1);

    const auto& bias = biasOf(r, e);
    EXPECT_LT(bias.wish_x, 0.0f);            // points back toward home (-X)
    EXPECT_NEAR(bias.wish_z, 0.0f, 1.0e-5f); // on-axis: no Z pull
    EXPECT_EQ(s.homing, 1);
}

// The homing bias GROWS with distance: straying further past the edge yields a larger pull
// (until it saturates at the cap).
TEST(Territory, BiasGrowsWithDistance) {
    entt::registry r;
    auto near = spawnTerritorial(r, 0.0f, 0.0f, 20.0f);
    auto far = spawnTerritorial(r, 0.0f, 0.0f, 20.0f);
    RunTerritoryOnTick(r, 0); // both claim home at origin

    moveTo(r, near, 25.0f, 0.0f); // overshoot 5
    moveTo(r, far, 35.0f, 0.0f);  // overshoot 15
    RunTerritoryOnTick(r, 1);

    const float magNear = -biasOf(r, near).wish_x; // both point -X
    const float magFar = -biasOf(r, far).wish_x;
    EXPECT_GT(magNear, 0.0f);
    EXPECT_GT(magFar, magNear);                     // further -> stronger
    EXPECT_LE(magFar, kTerritoryMaxBias + 1.0e-4f); // never exceeds the cap
}

// The bias magnitude saturates at the cap for a creature stranded very far from home.
TEST(Territory, BiasSaturatesAtCap) {
    entt::registry r;
    auto e = spawnTerritorial(r, 0.0f, 0.0f, 20.0f);
    RunTerritoryOnTick(r, 0);

    moveTo(r, e, 100000.0f, 0.0f); // absurdly far
    RunTerritoryOnTick(r, 1);

    const auto& bias = biasOf(r, e);
    const float mag = dm::Sqrt(bias.wish_x * bias.wish_x + bias.wish_z * bias.wish_z);
    EXPECT_NEAR(mag, kTerritoryMaxBias, 1.0e-3f);
}

// Off-axis homing: home at origin, creature out at (+X,+Z) beyond radius -> bias points into
// the third quadrant (both components negative), straight back toward home.
TEST(Territory, OffAxisPointsTowardHome) {
    entt::registry r;
    auto e = spawnTerritorial(r, 0.0f, 0.0f, 10.0f);
    RunTerritoryOnTick(r, 0);

    moveTo(r, e, 40.0f, 30.0f); // distance 50 > radius 10
    RunTerritoryOnTick(r, 1);

    const auto& bias = biasOf(r, e);
    EXPECT_LT(bias.wish_x, 0.0f);
    EXPECT_LT(bias.wish_z, 0.0f);
    // Direction must be colinear with the home vector (-40,-30): wish_x/wish_z == 40/30.
    EXPECT_NEAR(bias.wish_x / bias.wish_z, 40.0f / 30.0f, 1.0e-3f);
}

// ---- determinism: run == replay ----

// Two independent registries seeded + driven identically through several ticks must yield
// byte-identical territory + bias state on every participant (no rng, no wall-clock).
TEST(Territory, RunEqualsReplay) {
    auto build = [](entt::registry& r) {
        // Spawn in a deterministic order; ids will match across the two registries.
        spawnTerritorial(r, 0.0f, 0.0f, 20.0f);
        spawnTerritorial(r, 50.0f, -50.0f, 15.0f);
        spawnTerritorial(r, -30.0f, 10.0f, 25.0f);
    };
    auto drive = [](entt::registry& r) {
        std::vector<entt::entity> ents;
        for (auto e : r.view<Comp::TerritoryComponent>())
            ents.push_back(e);
        std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        for (std::uint64_t t = 0; t < 8; ++t) {
            // Perturb positions deterministically so creatures stray in/out of range.
            for (std::size_t i = 0; i < ents.size(); ++i) {
                auto& tf = r.get<Comp::TransformComponent>(ents[i]);
                const float k = static_cast<float>((t * 7 + i * 13) % 11);
                tf.position.x += k - 5.0f;
                tf.position.z += (k * 2.0f) - 3.0f;
            }
            RunTerritoryOnTick(r, t);
        }
    };

    entt::registry a, b;
    build(a);
    build(b);
    drive(a);
    drive(b);

    std::vector<entt::entity> ea, eb;
    for (auto e : a.view<Comp::TerritoryComponent>())
        ea.push_back(e);
    for (auto e : b.view<Comp::TerritoryComponent>())
        eb.push_back(e);
    std::sort(ea.begin(), ea.end(), [](entt::entity x, entt::entity y) {
        return entt::to_integral(x) < entt::to_integral(y);
    });
    std::sort(eb.begin(), eb.end(), [](entt::entity x, entt::entity y) {
        return entt::to_integral(x) < entt::to_integral(y);
    });
    ASSERT_EQ(ea.size(), eb.size());

    for (std::size_t i = 0; i < ea.size(); ++i) {
        const auto& ta = a.get<Comp::TerritoryComponent>(ea[i]);
        const auto& tb = b.get<Comp::TerritoryComponent>(eb[i]);
        EXPECT_EQ(ta.established, tb.established);
        EXPECT_EQ(dm::BitsOf(ta.home_x), dm::BitsOf(tb.home_x));
        EXPECT_EQ(dm::BitsOf(ta.home_z), dm::BitsOf(tb.home_z));

        const auto& ba = a.get<Comp::TerritoryBiasComponent>(ea[i]);
        const auto& bb = b.get<Comp::TerritoryBiasComponent>(eb[i]);
        EXPECT_EQ(dm::BitsOf(ba.wish_x), dm::BitsOf(bb.wish_x));
        EXPECT_EQ(dm::BitsOf(ba.wish_z), dm::BitsOf(bb.wish_z));
    }
}

// Order-independence: spawning the same creatures in a DIFFERENT registry-id order yields the
// same per-creature bias (the system sorts by id, and each output depends only on the
// creature's own state).
TEST(Territory, OrderIndependent) {
    entt::registry r;
    auto e = spawnTerritorial(r, 0.0f, 0.0f, 20.0f);
    RunTerritoryOnTick(r, 0);
    moveTo(r, e, 40.0f, 0.0f);
    RunTerritoryOnTick(r, 1);
    const float ref_x = biasOf(r, e).wish_x;

    // A second creature interleaved should not change the first's computed bias.
    entt::registry r2;
    auto a = spawnTerritorial(r2, 100.0f, 100.0f, 5.0f); // claims its own distant home
    auto b = spawnTerritorial(r2, 0.0f, 0.0f, 20.0f);
    RunTerritoryOnTick(r2, 0);
    moveTo(r2, b, 40.0f, 0.0f);
    RunTerritoryOnTick(r2, 1);
    (void)a;
    EXPECT_FLOAT_EQ(biasOf(r2, b).wish_x, ref_x);
}

TEST(Territory, RegionalElapsedPreservesClaimAndFrozenState) {
    namespace W = Luminumbra::world;
    for (const auto ticks : {1u, 8u, 36001u, 65536u}) {
        SCOPED_TRACE(ticks);
        entt::registry r;
        const auto active = spawnTerritorial(r, 4, 4);
        const auto frozen = spawnTerritorial(r, 513, 4);
        const auto zero = spawnTerritorial(r, 1025, 4);
        const auto unclaimed = spawnTerritorial(r, 513, 8);
        r.emplace<Comp::CreatureComponent>(active);
        r.emplace<Comp::CreatureComponent>(frozen);
        // Non-creature opt-ins must use their gate, even with a creature cache installed.
        r.emplace<Comp::TerritoryBiasComponent>(zero, 0.75f, -0.25f);
        r.emplace<Comp::TerritoryBiasComponent>(frozen, -0.5f, 0.25f);
        const std::array<W::RegionKey, 2> due{{{0, 0}, {2, 0}}};
        const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, ticks}, {{2, 0}, 0}};
        const W::RegionConsumerGate gate(r, due, W::RegionElapsedTicks(windows));
        luminumbra::ai::CreatureTickAdmission admission;
        admission.Capture(r, gate);
        moveTo(r, active, 520, 4);
        moveTo(r, frozen, 4, 4);
        const auto claimed = RunTerritoryOnTick(r, 65536, &admission, &gate);
        EXPECT_EQ(claimed.participants, 1);
        EXPECT_EQ(claimed.claimed, 1);
        EXPECT_EQ(claimed.homing, 0);
        EXPECT_EQ(terrOf(r, active).home_x, 520);
        EXPECT_EQ(biasOf(r, active).wish_x, 0);
        moveTo(r, active, 570, 4);
        const auto homing = RunTerritoryOnTick(r, 65537, &admission, &gate);
        EXPECT_EQ(homing.claimed, 0);
        EXPECT_EQ(homing.homing, 1);
        EXPECT_EQ(terrOf(r, active).home_x, 520);
        EXPECT_EQ(biasOf(r, active).wish_x, -kTerritoryMaxBias);
        EXPECT_EQ(biasOf(r, active).wish_z, 0);
        for (const auto entity : {frozen, zero, unclaimed}) {
            EXPECT_EQ(terrOf(r, entity).established, 0);
            EXPECT_EQ(terrOf(r, entity).home_x, 0);
            EXPECT_EQ(terrOf(r, entity).home_z, 0);
        }
        EXPECT_EQ(biasOf(r, frozen).wish_x, -0.5f);
        EXPECT_EQ(biasOf(r, frozen).wish_z, 0.25f);
        EXPECT_EQ(biasOf(r, zero).wish_x, 0.75f);
        EXPECT_EQ(biasOf(r, zero).wish_z, -0.25f);
        EXPECT_FALSE(r.all_of<Comp::TerritoryBiasComponent>(unclaimed));
        const auto resumed = spawnTerritorial(r, 4, 8); // non-creature, positive owner
        EXPECT_EQ(RunTerritoryOnTick(r, 65538, &admission, &gate).claimed, 1);
        EXPECT_EQ(terrOf(r, resumed).home_x, 4);
        const W::RegionConsumerGate empty(r, std::span<const W::RegionKey>{});
        EXPECT_EQ(RunTerritoryOnTick(r, 65539, nullptr, &empty).participants, 0);
    }
}

} // namespace
