//  multiplayer polish: InstinctLocomotionSystem coverage — the GOAP executor
// converting ActionPlanComponent into a seek/arrival wish-velocity intent. Pure,
// deterministic, engine-generic (no action-name vocabulary). Synthetic test data.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/InstinctLocomotionSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"

#include <cmath>
#include <cstring>

namespace {

using luminumbra::ai::RunInstinctLocomotionOnTick;
using Luminumbra::Components::Action;
using Luminumbra::Components::ActionPlanComponent;
using Luminumbra::Components::LocomotionIntentComponent;
using Luminumbra::Components::LocomotionPathComponent;
using Luminumbra::Components::LocomotionProfile;
using Luminumbra::Components::TransformComponent;

// Make an agent at (x,0,z) with a single-action plan targeting `target`.
entt::entity MakeAgent(
    entt::registry& reg, float x, float z, entt::entity target, LocomotionProfile profile = {}) {
    const auto e = reg.create();
    auto& tf = reg.emplace<TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    reg.emplace<LocomotionProfile>(e) = profile;
    auto& plan = reg.emplace<ActionPlanComponent>(e);
    Action a;
    a.name = "approach"; // game-data string; engine must not key on it
    a.target = target;
    plan.plan.push_back(a);
    return e;
}

entt::entity MakeTarget(entt::registry& reg, float x, float z) {
    const auto e = reg.create();
    auto& tf = reg.emplace<TransformComponent>(e);
    tf.position = Luminumbra::Vec3(x, 0.0f, z);
    return e;
}

float Len(const Luminumbra::Vec2& v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

} // namespace

// Far from target: wish points at the target at full cruise speed.
TEST(InstinctLocomotion, SeeksTargetAtCruiseSpeedWhenFar) {
    entt::registry reg;
    const auto target = MakeTarget(reg, 100.0f, 0.0f);
    LocomotionProfile p;
    p.move_speed = 3.0f;
    p.arrival_radius = 1.5f;
    p.slow_radius = 4.0f;
    const auto agent = MakeAgent(reg, 0.0f, 0.0f, target, p);

    const auto stats = RunInstinctLocomotionOnTick(reg);

    EXPECT_EQ(stats.agents_steered, 1u);
    EXPECT_EQ(stats.agents_arrived, 0u);
    const auto& intent = reg.get<LocomotionIntentComponent>(agent);
    EXPECT_FALSE(intent.arrived);
    EXPECT_NEAR(intent.wish_xz.x, 3.0f, 1e-4f); // +X toward target
    EXPECT_NEAR(intent.wish_xz.y, 0.0f, 1e-4f);
    EXPECT_NEAR(Len(intent.wish_xz), 3.0f, 1e-4f);
}

// Inside slow_radius but outside arrival_radius: speed ramps linearly with dist.
TEST(InstinctLocomotion, ArrivalRampReducesSpeedNearTarget) {
    entt::registry reg;
    const auto target = MakeTarget(reg, 2.0f, 0.0f); // dist 2, slow_radius 4 -> half speed
    LocomotionProfile p;
    p.move_speed = 4.0f;
    p.arrival_radius = 1.0f;
    p.slow_radius = 4.0f;
    const auto agent = MakeAgent(reg, 0.0f, 0.0f, target, p);

    RunInstinctLocomotionOnTick(reg);

    const auto& intent = reg.get<LocomotionIntentComponent>(agent);
    EXPECT_FALSE(intent.arrived);
    EXPECT_NEAR(Len(intent.wish_xz), 4.0f * (2.0f / 4.0f), 1e-4f); // 2.0 m/s
    EXPECT_GT(Len(intent.wish_xz), 0.0f);
    EXPECT_LT(Len(intent.wish_xz), p.move_speed);
}

// Within arrival_radius: wish is zero, arrived flagged, and the plan advances.
TEST(InstinctLocomotion, ArrivesHoldsAndAdvancesPlan) {
    entt::registry reg;
    const auto target = MakeTarget(reg, 0.5f, 0.0f); // dist 0.5 < arrival_radius
    const auto agent = MakeAgent(reg, 0.0f, 0.0f, target);

    const auto stats = RunInstinctLocomotionOnTick(reg);

    EXPECT_EQ(stats.agents_arrived, 1u);
    const auto& intent = reg.get<LocomotionIntentComponent>(agent);
    EXPECT_TRUE(intent.arrived);
    EXPECT_NEAR(Len(intent.wish_xz), 0.0f, 1e-6f);
    // Plan advanced past the consumed move action -> next tick is idle (holds).
    EXPECT_EQ(reg.get<ActionPlanComponent>(agent).current_action_index, 1u);

    const auto stats2 = RunInstinctLocomotionOnTick(reg);
    EXPECT_EQ(stats2.agents_idle, 1u);
    EXPECT_NEAR(Len(reg.get<LocomotionIntentComponent>(agent).wish_xz), 0.0f, 1e-6f);
}

// No positioned target / no plan -> idle, zero wish, no crash.
TEST(InstinctLocomotion, NoTargetIsIdle) {
    entt::registry reg;
    const auto agent = MakeAgent(reg, 0.0f, 0.0f, entt::null);

    const auto stats = RunInstinctLocomotionOnTick(reg);

    EXPECT_EQ(stats.agents_idle, 1u);
    EXPECT_EQ(stats.agents_steered, 0u);
    EXPECT_NEAR(Len(reg.get<LocomotionIntentComponent>(agent).wish_xz), 0.0f, 1e-6f);
}

// Determinism: identical state in two registries -> byte-identical wish bits.
TEST(InstinctLocomotion, IsDeterministicAcrossRuns) {
    auto build_and_run = []() {
        entt::registry reg;
        const auto t = MakeTarget(reg, 13.7f, -8.2f);
        LocomotionProfile p;
        p.move_speed = 2.5f;
        const auto a = MakeAgent(reg, -3.1f, 4.4f, t, p);
        RunInstinctLocomotionOnTick(reg);
        return reg.get<LocomotionIntentComponent>(a).wish_xz;
    };
    const auto wish_a = build_and_run();
    const auto wish_b = build_and_run();
    std::uint32_t ax, ay, bx, by;
    std::memcpy(&ax, &wish_a.x, 4);
    std::memcpy(&ay, &wish_a.y, 4);
    std::memcpy(&bx, &wish_b.x, 4);
    std::memcpy(&by, &wish_b.y, 4);
    EXPECT_EQ(ax, bx);
    EXPECT_EQ(ay, by);
}

// with a path component the agent seeks the WAYPOINT, not the target.
TEST(InstinctLocomotion, FollowsWaypointBeforeSeekingTarget) {
    entt::registry reg;
    const auto target = MakeTarget(reg, 10.0f, 0.0f); // target on +X
    LocomotionProfile p;
    p.move_speed = 3.0f;
    p.arrival_radius = 1.0f;
    p.slow_radius = 2.0f;
    const auto agent = MakeAgent(reg, 0.0f, 0.0f, target, p);
    auto& path = reg.emplace<LocomotionPathComponent>(agent);
    path.waypoints = {Luminumbra::Vec2(0.0f, 5.0f)}; // waypoint on +Z (perpendicular)

    RunInstinctLocomotionOnTick(reg);

    const auto& w = reg.get<LocomotionIntentComponent>(agent).wish_xz;
    EXPECT_GT(w.y, 1.0f);          // heads toward the +Z waypoint...
    EXPECT_NEAR(w.x, 0.0f, 1e-4f); //...NOT directly toward the +X target
}

// reaching a waypoint advances the route; once exhausted the agent
// resumes seeking the action target.
TEST(InstinctLocomotion, WaypointArrivalAdvancesThenPathExhaustsToTarget) {
    entt::registry reg;
    const auto target = MakeTarget(reg, 10.0f, 0.0f); // +X
    LocomotionProfile p;
    p.move_speed = 3.0f;
    p.arrival_radius = 1.0f;
    p.slow_radius = 2.0f;
    const auto agent = MakeAgent(reg, 0.0f, 0.0f, target, p);
    auto& path = reg.emplace<LocomotionPathComponent>(agent);
    path.waypoints = {Luminumbra::Vec2(0.5f, 0.0f)}; // already within arrival_radius

    // Tick 1: within arrival_radius of the lone waypoint -> advance index, hold.
    RunInstinctLocomotionOnTick(reg);
    EXPECT_EQ(reg.get<LocomotionPathComponent>(agent).index, 1u);
    EXPECT_NEAR(Len(reg.get<LocomotionIntentComponent>(agent).wish_xz), 0.0f, 1e-6f);

    // Tick 2: path exhausted -> seeks the +X action target.
    RunInstinctLocomotionOnTick(reg);
    const auto& w = reg.get<LocomotionIntentComponent>(agent).wish_xz;
    EXPECT_GT(w.x, 1.0f);
    EXPECT_NEAR(w.y, 0.0f, 1e-4f);
}

// a flee action moves the agent directly AWAY from the (threat) target.
TEST(InstinctLocomotion, FleeMovesAwayFromThreat) {
    entt::registry reg;
    const auto threat = MakeTarget(reg, 2.0f, 0.0f); // threat on +X, close
    LocomotionProfile p;
    p.move_speed = 3.0f;
    p.slow_radius = 5.0f; // safe distance
    const auto agent = MakeAgent(reg, 0.0f, 0.0f, threat, p);
    reg.get<ActionPlanComponent>(agent).plan[0].flee = true;

    const auto stats = RunInstinctLocomotionOnTick(reg);

    EXPECT_EQ(stats.agents_steered, 1u);
    EXPECT_EQ(stats.agents_arrived, 0u);
    const auto& w = reg.get<LocomotionIntentComponent>(agent).wish_xz;
    EXPECT_NEAR(w.x, -3.0f, 1e-4f); // flees in -X, away from the +X threat
    EXPECT_NEAR(w.y, 0.0f, 1e-4f);
    EXPECT_NEAR(Len(w), 3.0f, 1e-4f); // full cruise while unsafe
}

// once beyond slow_radius (the safe distance) the flee action completes.
TEST(InstinctLocomotion, FleeStopsWhenSafe) {
    entt::registry reg;
    const auto threat = MakeTarget(reg, 10.0f, 0.0f); // far -> already safe
    LocomotionProfile p;
    p.move_speed = 3.0f;
    p.slow_radius = 5.0f;
    const auto agent = MakeAgent(reg, 0.0f, 0.0f, threat, p);
    reg.get<ActionPlanComponent>(agent).plan[0].flee = true;

    const auto stats = RunInstinctLocomotionOnTick(reg);

    EXPECT_EQ(stats.agents_arrived, 1u);
    const auto& w = reg.get<LocomotionIntentComponent>(agent).wish_xz;
    EXPECT_NEAR(Len(w), 0.0f, 1e-6f);                                        // safe: holds
    EXPECT_EQ(reg.get<ActionPlanComponent>(agent).current_action_index, 1u); // advanced
}

//  flocking: cohesion adds a pull toward the neighbor group's center.
TEST(InstinctLocomotion, CohesionPullsTowardGroupCenter) {
    auto wish_with_cohesion = [](float strength) {
        entt::registry reg;
        const auto target = MakeTarget(reg, 0.0f, 100.0f); // far on +Z -> seek is +Z
        LocomotionProfile p;
        p.move_speed = 3.0f;
        p.flock_radius = 20.0f;
        p.cohesion_strength = strength;
        const auto a = MakeAgent(reg, 0.0f, 0.0f, target, p);
        // Two neighbors clustered on +X so the group centroid is on +X of agent A.
        (void)MakeAgent(reg, 10.0f, 0.0f, target, p);
        (void)MakeAgent(reg, 10.0f, 2.0f, target, p);
        RunInstinctLocomotionOnTick(reg);
        return reg.get<LocomotionIntentComponent>(a).wish_xz;
    };
    const auto off = wish_with_cohesion(0.0f);
    const auto on = wish_with_cohesion(1.0f);
    EXPECT_NEAR(off.x, 0.0f, 1e-4f); // pure +Z seek, no lateral pull
    EXPECT_GT(on.x, off.x + 0.1f);   // cohesion pulls toward the +X group center
}

//  flocking: alignment steers toward the neighbors' mean heading.
TEST(InstinctLocomotion, AlignmentMatchesNeighborHeading) {
    auto wish_with_alignment = [](float strength) {
        entt::registry reg;
        const auto target = MakeTarget(reg, 0.0f, 100.0f); // agent A seeks +Z
        LocomotionProfile p;
        p.move_speed = 3.0f;
        p.flock_radius = 20.0f;
        p.alignment_strength = strength;
        const auto a = MakeAgent(reg, 0.0f, 0.0f, target, p);
        // A neighbor already moving on +X (prior-tick heading), nearby.
        const auto nb = MakeAgent(reg, 3.0f, 0.0f, target, p);
        reg.emplace<LocomotionIntentComponent>(nb).wish_xz = Luminumbra::Vec2(3.0f, 0.0f);
        RunInstinctLocomotionOnTick(reg);
        return reg.get<LocomotionIntentComponent>(a).wish_xz;
    };
    const auto off = wish_with_alignment(0.0f);
    const auto on = wish_with_alignment(1.0f);
    EXPECT_NEAR(off.x, 0.0f, 1e-4f); // pure +Z seek
    EXPECT_GT(on.x, off.x + 0.1f);   // alignment turns A toward the +X mean heading
}

// separation (crowd/obstacle avoidance) pushes two crowded agents that
// seek the SAME target apart laterally, while both still advance toward it.
TEST(InstinctLocomotion, SeparationPushesCrowdedAgentsApart) {
    entt::registry reg;
    const auto target = MakeTarget(reg, 100.0f, 0.0f); // far, on +X
    LocomotionProfile p;
    p.move_speed = 3.0f;
    p.separation_radius = 3.0f;
    p.separation_strength = 1.0f;
    // Two agents 1 m apart on the Z axis, both seeking the same far target.
    const auto a = MakeAgent(reg, 0.0f, 0.0f, target, p);
    const auto b = MakeAgent(reg, 0.0f, 1.0f, target, p);

    RunInstinctLocomotionOnTick(reg);

    const auto& wa = reg.get<LocomotionIntentComponent>(a).wish_xz;
    const auto& wb = reg.get<LocomotionIntentComponent>(b).wish_xz;
    // Both still head toward the target (+X)...
    EXPECT_GT(wa.x, 0.0f);
    EXPECT_GT(wb.x, 0.0f);
    //...but A (at lower Z) is pushed -Z and B (at higher Z) +Z, away from each other.
    EXPECT_LT(wa.y, -0.1f);
    EXPECT_GT(wb.y, 0.1f);
    // Avoidance never exceeds the locomotion budget.
    EXPECT_LE(Len(wa), p.move_speed + 1e-4f);
    EXPECT_LE(Len(wb), p.move_speed + 1e-4f);
}

// separation defaults OFF (strength 0) — a crowded neighbor must NOT
// perturb the wish, so the canonical roster (which never sets separation) is
// byte-identical and world_hash is unchanged. This is the determinism guarantee.
TEST(InstinctLocomotion, SeparationDefaultOffIsByteIdentical) {
    auto wish_with_neighbor = [](bool add_neighbor) {
        entt::registry reg;
        const auto target = MakeTarget(reg, 50.0f, 0.0f);
        LocomotionProfile p; // defaults: separation_strength == 0
        p.move_speed = 2.5f;
        const auto a = MakeAgent(reg, 0.0f, 0.0f, target, p);
        if (add_neighbor) {
            (void)MakeAgent(reg, 0.0f, 0.5f, target, p); // very close crowding agent
        }
        RunInstinctLocomotionOnTick(reg);
        return reg.get<LocomotionIntentComponent>(a).wish_xz;
    };
    const auto solo = wish_with_neighbor(false);
    const auto crowded = wish_with_neighbor(true);
    std::uint32_t sx, sy, cx, cy;
    std::memcpy(&sx, &solo.x, 4);
    std::memcpy(&sy, &solo.y, 4);
    std::memcpy(&cx, &crowded.x, 4);
    std::memcpy(&cy, &crowded.y, 4);
    EXPECT_EQ(sx, cx); // identical bits with separation off, neighbor or not
    EXPECT_EQ(sy, cy);
}

// Visiting order is independent of entity insertion order (deterministic sort).
TEST(InstinctLocomotion, OrderIndependentPerEntityResult) {
    // Registry A: target first, then agent. Registry B: agent first, then target.
    Luminumbra::Vec2 wish_a, wish_b;
    {
        entt::registry reg;
        const auto t = MakeTarget(reg, 10.0f, 10.0f);
        const auto a = MakeAgent(reg, 0.0f, 0.0f, t);
        RunInstinctLocomotionOnTick(reg);
        wish_a = reg.get<LocomotionIntentComponent>(a).wish_xz;
    }
    {
        entt::registry reg;
        // create a throwaway to shift entity ids, then agent, then target
        (void)reg.create();
        const auto t = MakeTarget(reg, 10.0f, 10.0f);
        const auto a = MakeAgent(reg, 0.0f, 0.0f, t);
        RunInstinctLocomotionOnTick(reg);
        wish_b = reg.get<LocomotionIntentComponent>(a).wish_xz;
    }
    EXPECT_NEAR(wish_a.x, wish_b.x, 1e-5f);
    EXPECT_NEAR(wish_a.y, wish_b.y, 1e-5f);
}
