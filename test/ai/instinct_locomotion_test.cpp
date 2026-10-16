//  multiplayer polish: InstinctLocomotionSystem coverage — the GOAP executor
// converting ActionPlanComponent into a seek/arrival wish-velocity intent. Pure,
// deterministic, engine-generic (no action-name vocabulary). Synthetic test data.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/InstinctLocomotionSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <random>
#include <vector>

#include "luminumbra_common/core/DeterministicMath.h"

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

// Exhaustive reference: retain the original tick arithmetic and full id-ordered
// scans independently of the production candidate selection.
luminumbra::ai::InstinctLocomotionTickStats RunBruteForceTick(entt::registry& registry) {
    luminumbra::ai::InstinctLocomotionTickStats stats;

    std::vector<entt::entity> agents;
    {
        auto view =
            registry.view<ActionPlanComponent, const TransformComponent, const LocomotionProfile>();
        for (auto entity : view) {
            agents.push_back(entity);
        }
    }
    std::sort(agents.begin(), agents.end(), [](entt::entity lhs, entt::entity rhs) {
        return entt::to_integral(lhs) < entt::to_integral(rhs);
    });

    struct AgentPos {
        entt::entity id;
        float x;
        float z;
        float wx;
        float wz;
    };
    std::vector<AgentPos> neighbors;
    neighbors.reserve(agents.size());
    for (auto entity : agents) {
        const auto& tf = registry.get<const TransformComponent>(entity);
        const auto* prev = registry.try_get<LocomotionIntentComponent>(entity);
        const float pwx = prev ? prev->wish_xz.x : 0.0f;
        const float pwz = prev ? prev->wish_xz.y : 0.0f;
        neighbors.push_back({entity, tf.position.x, tf.position.z, pwx, pwz});
    }

    for (auto entity : agents) {
        ++stats.agents_seen;

        auto& plan = registry.get<ActionPlanComponent>(entity);
        const auto& tf = registry.get<const TransformComponent>(entity);
        const auto& profile = registry.get<const LocomotionProfile>(entity);

        auto& intent = registry.get_or_emplace<LocomotionIntentComponent>(entity);
        intent.wish_xz = Luminumbra::Vec2(0.0f);
        intent.arrived = false;

        if (plan.plan.empty() || plan.current_action_index >= plan.plan.size()) {
            ++stats.agents_idle;
            continue;
        }

        const Luminumbra::EntityID target = plan.plan[plan.current_action_index].target;
        if (target == entt::null || !registry.valid(target) ||
            !registry.all_of<TransformComponent>(target)) {
            ++stats.agents_idle;
            continue;
        }

        const auto& target_tf = registry.get<const TransformComponent>(target);
        const bool flee = plan.plan[plan.current_action_index].flee;

        auto* path = registry.try_get<LocomotionPathComponent>(entity);
        const bool following = !flee && path != nullptr && path->index < path->waypoints.size();
        const float goal_x = following ? path->waypoints[path->index].x : target_tf.position.x;
        const float goal_z = following ? path->waypoints[path->index].y : target_tf.position.z;

        const float dx = goal_x - tf.position.x;
        const float dz = goal_z - tf.position.z;
        const float dist = Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);

        const float slow = profile.slow_radius > profile.arrival_radius ? profile.slow_radius
                                                                        : profile.arrival_radius;

        if (flee) {
            const float safe = slow > 0.0f ? slow : profile.arrival_radius;
            if (dist >= safe) {
                intent.arrived = true;
                ++stats.agents_arrived;
                ++plan.current_action_index;
                plan.time_in_current_action = 0.0f;
                continue;
            }

            float adx = dx, adz = dz;
            if (dist < 1e-4f) {
                adx = -1.0f;
                adz = 0.0f;
            }
            const float ainv = 1.0f / Luminumbra::DeterministicMath::Sqrt(adx * adx + adz * adz);
            intent.wish_xz = Luminumbra::Vec2(-adx * ainv * profile.move_speed,
                                              -adz * ainv * profile.move_speed);
        } else {
            if (dist <= profile.arrival_radius) {
                if (following) {
                    ++path->index;
                    continue;
                }

                intent.arrived = true;
                ++stats.agents_arrived;
                ++plan.current_action_index;
                plan.time_in_current_action = 0.0f;
                continue;
            }

            float speed = profile.move_speed;
            if (dist < slow && slow > 0.0f) {
                speed = profile.move_speed * (dist / slow);
            }
            const float inv = 1.0f / dist;
            intent.wish_xz = Luminumbra::Vec2(dx * inv * speed, dz * inv * speed);
        }

        if (profile.separation_strength > 0.0f && profile.separation_radius > 0.0f) {
            const float sep_r = profile.separation_radius;
            float push_x = 0.0f;
            float push_z = 0.0f;
            for (const auto& nb : neighbors) {
                ++stats.neighbor_candidates_examined;
                if (nb.id == entity) {
                    continue;
                }
                const float nx = tf.position.x - nb.x;
                const float nz = tf.position.z - nb.z;
                const float nd = Luminumbra::DeterministicMath::Sqrt(nx * nx + nz * nz);
                if (nd > 0.0f && nd < sep_r) {
                    const float w = (sep_r - nd) / (sep_r * nd);
                    push_x += nx * w;
                    push_z += nz * w;
                }
            }

            intent.wish_xz.x += push_x * profile.separation_strength * profile.move_speed;
            intent.wish_xz.y += push_z * profile.separation_strength * profile.move_speed;
            const float wmag = Luminumbra::DeterministicMath::Sqrt(
                intent.wish_xz.x * intent.wish_xz.x + intent.wish_xz.y * intent.wish_xz.y);
            if (wmag > profile.move_speed && wmag > 0.0f) {
                const float s = profile.move_speed / wmag;
                intent.wish_xz.x *= s;
                intent.wish_xz.y *= s;
            }
        }

        if ((profile.cohesion_strength > 0.0f || profile.alignment_strength > 0.0f) &&
            profile.flock_radius > 0.0f) {
            const float fr = profile.flock_radius;
            float cx = 0.0f, cz = 0.0f;
            float hx = 0.0f, hz = 0.0f;
            int count = 0;
            for (const auto& nb : neighbors) {
                ++stats.neighbor_candidates_examined;
                if (nb.id == entity)
                    continue;
                const float nx = nb.x - tf.position.x;
                const float nz = nb.z - tf.position.z;
                const float nd = Luminumbra::DeterministicMath::Sqrt(nx * nx + nz * nz);
                if (nd > 0.0f && nd < fr) {
                    cx += nb.x;
                    cz += nb.z;
                    hx += nb.wx;
                    hz += nb.wz;
                    ++count;
                }
            }
            if (count > 0) {
                const float invn = 1.0f / static_cast<float>(count);
                if (profile.cohesion_strength > 0.0f) {
                    const float ccx = cx * invn - tf.position.x;
                    const float ccz = cz * invn - tf.position.z;
                    const float cl = Luminumbra::DeterministicMath::Sqrt(ccx * ccx + ccz * ccz);
                    if (cl > 0.0f) {
                        const float cs = profile.cohesion_strength * profile.move_speed / cl;
                        intent.wish_xz.x += ccx * cs;
                        intent.wish_xz.y += ccz * cs;
                    }
                }
                if (profile.alignment_strength > 0.0f) {
                    const float mhx = hx * invn;
                    const float mhz = hz * invn;
                    const float al = Luminumbra::DeterministicMath::Sqrt(mhx * mhx + mhz * mhz);
                    if (al > 0.0f) {
                        const float as = profile.alignment_strength * profile.move_speed / al;
                        intent.wish_xz.x += mhx * as;
                        intent.wish_xz.y += mhz * as;
                    }
                }
                const float wmag = Luminumbra::DeterministicMath::Sqrt(
                    intent.wish_xz.x * intent.wish_xz.x + intent.wish_xz.y * intent.wish_xz.y);
                if (wmag > profile.move_speed && wmag > 0.0f) {
                    const float s = profile.move_speed / wmag;
                    intent.wish_xz.x *= s;
                    intent.wish_xz.y *= s;
                }
            }
        }
        ++stats.agents_steered;
    }

    return stats;
}

struct GridTestAgent {
    float x = 0.0f;
    float z = 0.0f;
    LocomotionProfile profile{};
    Luminumbra::Vec2 heading{0.0f};
    bool idle = false;
    bool flee = false;
    bool waypoint = false;
};

std::vector<entt::entity> PopulateGridRoster(entt::registry& registry,
                                             const std::vector<GridTestAgent>& roster) {
    const auto target = MakeTarget(registry, 10000.0f, 10000.0f);
    std::vector<entt::entity> entities;
    entities.reserve(roster.size());
    for (const auto& entry : roster) {
        const auto entity = MakeAgent(registry, entry.x, entry.z, target, entry.profile);
        registry.emplace<LocomotionIntentComponent>(entity).wish_xz = entry.heading;
        auto& plan = registry.get<ActionPlanComponent>(entity);
        plan.plan[0].flee = entry.flee;
        if (entry.idle) {
            plan.plan.clear();
        }
        if (entry.waypoint) {
            registry.emplace<LocomotionPathComponent>(entity).waypoints = {
                Luminumbra::Vec2(entry.x + 0.25f, entry.z),
                Luminumbra::Vec2(entry.x + 100.0f, entry.z + 20.0f)};
        }
        entities.push_back(entity);
    }
    return entities;
}

void ExpectSameTickState(const entt::registry& grid_registry,
                         const entt::registry& reference_registry,
                         const std::vector<entt::entity>& entities) {
    for (const auto entity : entities) {
        SCOPED_TRACE(entt::to_integral(entity));
        const auto& actual = grid_registry.get<LocomotionIntentComponent>(entity);
        const auto& expected = reference_registry.get<LocomotionIntentComponent>(entity);
        EXPECT_EQ(Luminumbra::DeterministicMath::BitsOf(actual.wish_xz.x),
                  Luminumbra::DeterministicMath::BitsOf(expected.wish_xz.x));
        EXPECT_EQ(Luminumbra::DeterministicMath::BitsOf(actual.wish_xz.y),
                  Luminumbra::DeterministicMath::BitsOf(expected.wish_xz.y));
        EXPECT_EQ(actual.arrived, expected.arrived);
        const auto& actual_plan = grid_registry.get<ActionPlanComponent>(entity);
        const auto& expected_plan = reference_registry.get<ActionPlanComponent>(entity);
        EXPECT_EQ(actual_plan.current_action_index, expected_plan.current_action_index);
        EXPECT_EQ(Luminumbra::DeterministicMath::BitsOf(actual_plan.time_in_current_action),
                  Luminumbra::DeterministicMath::BitsOf(expected_plan.time_in_current_action));
        const auto* actual_path = grid_registry.try_get<LocomotionPathComponent>(entity);
        const auto* expected_path = reference_registry.try_get<LocomotionPathComponent>(entity);
        ASSERT_EQ(actual_path != nullptr, expected_path != nullptr);
        if (actual_path != nullptr && expected_path != nullptr) {
            EXPECT_EQ(actual_path->index, expected_path->index);
        }
    }
}

luminumbra::ai::InstinctLocomotionTickStats
ExpectSameTick(entt::registry& grid_registry,
               entt::registry& reference_registry,
               const std::vector<entt::entity>& entities) {
    const auto expected = RunBruteForceTick(reference_registry);
    const auto actual = RunInstinctLocomotionOnTick(grid_registry);
    EXPECT_EQ(actual.agents_seen, expected.agents_seen);
    EXPECT_EQ(actual.agents_steered, expected.agents_steered);
    EXPECT_EQ(actual.agents_arrived, expected.agents_arrived);
    EXPECT_EQ(actual.agents_idle, expected.agents_idle);
    ExpectSameTickState(grid_registry, reference_registry, entities);
    return actual;
}

luminumbra::ai::InstinctLocomotionTickStats
ExpectRosterMatches(const std::vector<GridTestAgent>& roster, int ticks = 3) {
    entt::registry grid_registry;
    entt::registry reference_registry;
    const auto entities = PopulateGridRoster(grid_registry, roster);
    const auto reference_entities = PopulateGridRoster(reference_registry, roster);
    EXPECT_EQ(entities, reference_entities);
    luminumbra::ai::InstinctLocomotionTickStats stats;
    for (int tick = 0; tick < ticks; ++tick) {
        SCOPED_TRACE(tick);
        stats = ExpectSameTick(grid_registry, reference_registry, entities);
    }
    return stats;
}

std::vector<GridTestAgent> SpreadRoster(std::size_t count) {
    std::vector<GridTestAgent> roster(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto& entry = roster[index];
        entry.x = static_cast<float>(index % 50) * 32.0f - 800.0f;
        entry.z = static_cast<float>(index / 50) * 32.0f - 640.0f;
        entry.heading = Luminumbra::Vec2(static_cast<float>(index % 7) - 3.0f,
                                         static_cast<float>(index % 11) - 5.0f);
    }
    return roster;
}

} // namespace

// Far from target: wish points at the target at full cruise speed.
TEST(InstinctLocomotion, SeeksTargetAtCruiseSpeedWhenFar) {
    entt::registry reg;
    const auto target = MakeTarget(reg, 100.0f, 0.0f);
    LocomotionProfile p;
    p.separation_strength = 0.0f;
    p.cohesion_strength = 0.0f;
    p.alignment_strength = 0.0f;
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
    p.separation_strength = 0.0f;
    p.cohesion_strength = 0.0f;
    p.alignment_strength = 0.0f;
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
        p.separation_strength = 0.0f;
        p.cohesion_strength = 0.0f;
        p.alignment_strength = 0.0f;
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
    p.separation_strength = 0.0f;
    p.cohesion_strength = 0.0f;
    p.alignment_strength = 0.0f;
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
    p.separation_strength = 0.0f;
    p.cohesion_strength = 0.0f;
    p.alignment_strength = 0.0f;
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
    p.separation_strength = 0.0f;
    p.cohesion_strength = 0.0f;
    p.alignment_strength = 0.0f;
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
    p.separation_strength = 0.0f;
    p.cohesion_strength = 0.0f;
    p.alignment_strength = 0.0f;
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
        p.separation_strength = 0.0f;
        p.cohesion_strength = 0.0f;
        p.alignment_strength = 0.0f;
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
        p.separation_strength = 0.0f;
        p.cohesion_strength = 0.0f;
        p.alignment_strength = 0.0f;
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
    p.separation_strength = 0.0f;
    p.cohesion_strength = 0.0f;
    p.alignment_strength = 0.0f;
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

// Explicitly disabling all steering terms leaves a crowded neighbor unable to
// perturb the wish. Shipping profiles enable these terms by default.
TEST(InstinctLocomotion, ExplicitlyDisabledSteeringIsByteIdentical) {
    auto wish_with_neighbor = [](bool add_neighbor) {
        entt::registry reg;
        const auto target = MakeTarget(reg, 50.0f, 0.0f);
        LocomotionProfile p;
        p.separation_strength = 0.0f;
        p.cohesion_strength = 0.0f;
        p.alignment_strength = 0.0f; // explicitly disable all three terms
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

TEST(InstinctLocomotion, ShippingSteeringDefaults) {
    const LocomotionProfile p;
    EXPECT_FLOAT_EQ(p.separation_radius, 2.0f);
    EXPECT_FLOAT_EQ(p.separation_strength, 0.6f);
    EXPECT_FLOAT_EQ(p.flock_radius, 12.0f);
    EXPECT_FLOAT_EQ(p.cohesion_strength, 0.5f);
    EXPECT_FLOAT_EQ(p.alignment_strength, 0.4f);
}

TEST(InstinctLocomotion, GridMatchesBruteForceForSeededMixedRosters) {
    for (const std::uint32_t seed : {7u, 101u, 1234567u}) {
        SCOPED_TRACE(seed);
        std::mt19937 generator(seed);
        const auto sample = [&generator]() {
            return static_cast<float>(static_cast<int>(generator() % 8193u) - 4096) / 64.0f;
        };
        std::vector<GridTestAgent> roster(320);
        const std::vector<float> strengths{-0.5f, 0.0f, 0.4f, 0.6f, 1.0f};
        const std::vector<float> radii{-2.0f, 0.0f, 0.25f, 2.0f, 6.0f, 12.0f};
        for (std::size_t index = 0; index < roster.size(); ++index) {
            auto& entry = roster[index];
            entry.x = sample();
            entry.z = sample();
            if (index % 4 == 0) { // Dense clusters and coincident positions.
                entry.x = static_cast<float>(index % 3) * 0.125f;
                entry.z = 0.0f;
            } else if (index % 4 == 1) { // Positive and negative cell boundaries.
                entry.x = static_cast<float>(static_cast<int>(index % 9) - 4) * 12.0f;
                entry.z = static_cast<float>(static_cast<int>(index % 7) - 3) * 12.0f;
            }
            const float heading_x = sample();
            const float heading_z = sample();
            entry.heading = Luminumbra::Vec2(heading_x, heading_z);
            entry.profile.separation_strength = strengths[index % strengths.size()];
            entry.profile.cohesion_strength = strengths[(index / 3) % strengths.size()];
            entry.profile.alignment_strength = strengths[(index / 7) % strengths.size()];
            entry.profile.separation_radius = radii[index % radii.size()];
            entry.profile.flock_radius = radii[(index / 5) % radii.size()];
            entry.idle = index % 23 == 0;
            entry.flee = index % 29 == 0;
            entry.waypoint = index % 31 == 0;
        }
        // Idle entries still participate in the prior-heading/position snapshot.
        ExpectRosterMatches(roster);
    }
}

TEST(InstinctLocomotion, GridBoundsCandidateWorkForTwoThousandSpreadAgents) {
    const auto stats = ExpectRosterMatches(SpreadRoster(2000), 1);
    EXPECT_EQ(stats.agents_steered, 2000u);
    // Both enabled passes together: <= 4 candidates per agent versus 4,000
    // entries per agent in the exhaustive scans (8,000,000 entries per tick).
    EXPECT_LE(stats.neighbor_candidates_examined, 8000u);
    EXPECT_GE(stats.neighbor_candidates_examined, 4000u); // Self in each pass.
    EXPECT_EQ(stats.neighbor_exhaustive_queries, 0u);
}

TEST(InstinctLocomotion, GridDenseFallbackVisitsEachSnapshotEntryOncePerQuery) {
    constexpr std::size_t kAgentCount = 256;
    for (const int mode : {0, 1, 2}) {
        SCOPED_TRACE(mode);
        for (const std::size_t clustered_count :
             {kAgentCount, kAgentCount - 1, kAgentCount / 2 + 1}) {
            SCOPED_TRACE(clustered_count);
            // A single active entry checks the per-query bound; the full roster
            // checks both dense and sparse queries. Idle entries stay in the snapshot.
            for (const std::size_t active_count : {std::size_t{1}, kAgentCount}) {
                SCOPED_TRACE(active_count);
                auto roster = SpreadRoster(kAgentCount);
                for (std::size_t index = 0; index < roster.size(); ++index) {
                    auto& entry = roster[index];
                    // The cluster occupies one positive 12-metre cell; all
                    // remaining entries occupy isolated, distant cells.
                    if (index < clustered_count) {
                        entry.x = 0.125f + static_cast<float>(index % 16) * 0.03125f;
                        entry.z = 0.125f + static_cast<float>(index / 16) * 0.03125f;
                    }
                    entry.idle = index >= active_count;
                    if (mode == 0) {
                        entry.profile.cohesion_strength = 0.0f;
                        entry.profile.alignment_strength = 0.0f;
                    } else if (mode == 1) {
                        entry.profile.separation_strength = 0.0f;
                    }
                }
                const std::uint64_t enabled_passes = mode == 2 ? 2u : 1u;
                const auto dense_queries =
                    static_cast<std::uint64_t>(std::min(active_count, clustered_count));
                const auto sparse_queries =
                    static_cast<std::uint64_t>(active_count) - dense_queries;
                const std::uint64_t expected_work =
                    (dense_queries * static_cast<std::uint64_t>(kAgentCount) + sparse_queries) *
                    enabled_passes;
                const auto stats = ExpectRosterMatches(roster);
                EXPECT_EQ(stats.agents_steered, static_cast<std::uint64_t>(active_count));
                EXPECT_EQ(stats.neighbor_candidates_examined, expected_work);
                // Dense queries visit exactly N entries without counting buckets
                // twice; each isolated query visits only self through the grid.
                EXPECT_EQ(stats.neighbor_exhaustive_queries, dense_queries * enabled_passes);
            }
        }
    }
}

TEST(InstinctLocomotion, GridSortingBudgetSwitchesBeforeHalfSnapshotDensity) {
    // Check the inclusive grid limit and the next count, including ceil(log2 N)
    // rounding: 256 / 8 = 32 candidates, and floor(257 / 9) = 28 candidates.
    for (const std::size_t agent_count : {std::size_t{256}, std::size_t{257}}) {
        SCOPED_TRACE(agent_count);
        const std::size_t grid_limit = agent_count == 256 ? 32u : 28u;
        for (const std::size_t clustered_count : {grid_limit, grid_limit + 1}) {
            SCOPED_TRACE(clustered_count);
            auto roster = SpreadRoster(agent_count);
            for (std::size_t index = 0; index < roster.size(); ++index) {
                auto& entry = roster[index];
                entry.idle = index != 0;
                if (index < clustered_count) {
                    entry.x = 0.125f + static_cast<float>(index) * 0.015625f;
                    entry.z = 0.125f;
                }
            }
            const auto stats = ExpectRosterMatches(roster);
            EXPECT_EQ(stats.agents_steered, 1u);
            if (clustered_count == grid_limit) {
                EXPECT_EQ(stats.neighbor_exhaustive_queries, 0u);
                EXPECT_EQ(stats.neighbor_candidates_examined,
                          static_cast<std::uint64_t>(grid_limit) * 2u);
            } else {
                EXPECT_EQ(stats.neighbor_exhaustive_queries, 2u);
                EXPECT_EQ(stats.neighbor_candidates_examined,
                          static_cast<std::uint64_t>(agent_count) * 2u);
            }
        }
    }
}

TEST(InstinctLocomotion, GridFallbackMatchesBruteForceForUnsafeInputs) {
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Includes the finite query-cell bound, failed cell conversion, subnormal
    // radii, and non-finite coordinates and radii. Only one entry is active so
    // the work counter proves both of its queries take the exhaustive path.
    for (int scenario = 0; scenario < 10; ++scenario) {
        SCOPED_TRACE(scenario);
        auto roster = SpreadRoster(80);
        for (auto& entry : roster) {
            entry.idle = true;
        }
        auto& active = roster.front();
        active.idle = false;
        if (scenario == 0) {
            active.profile.separation_radius = infinity;
            active.profile.flock_radius = infinity;
        } else if (scenario == 1) {
            active.profile.separation_radius = std::numeric_limits<float>::max();
            active.profile.flock_radius = std::numeric_limits<float>::max();
        } else if (scenario == 2) {
            active.profile.separation_radius = 4096.0f;
            active.profile.flock_radius = 4096.0f;
        } else if (scenario == 3) {
            active.x = infinity;
        } else if (scenario == 4) {
            active.z = -infinity;
        } else if (scenario == 5) {
            active.x = nan;
        } else if (scenario == 6) {
            active.x = std::numeric_limits<float>::max();
        } else if (scenario == 7) {
            active.profile.separation_radius = std::numeric_limits<float>::denorm_min();
            active.profile.flock_radius = std::numeric_limits<float>::denorm_min();
        } else if (scenario == 8) {
            active.profile.separation_radius = nan;
            active.profile.flock_radius = nan;
        } else {
            active.profile.separation_radius = -infinity;
            active.profile.flock_radius = -infinity;
        }
        const auto stats = ExpectRosterMatches(roster, 1);
        const std::uint64_t expected_work = scenario < 8 ? 160u : 0u;
        EXPECT_EQ(stats.neighbor_candidates_examined, expected_work);
    }
}

TEST(InstinctLocomotion, GridPreservesStrictRadiusAndCoincidentPositionTests) {
    std::vector<GridTestAgent> roster(12);
    const std::vector<float> offsets{0.0f,
                                     -0.0f,
                                     2.0f,
                                     -2.0f,
                                     12.0f,
                                     -12.0f,
                                     std::nextafter(2.0f, 0.0f),
                                     std::nextafter(2.0f, 3.0f),
                                     std::nextafter(12.0f, 0.0f),
                                     std::nextafter(12.0f, 13.0f),
                                     0.5f,
                                     -0.5f};
    for (const float scale : {0x1p-62f, 0x1p-30f, 1.0f, 128.0f}) {
        SCOPED_TRACE(scale);
        for (std::size_t index = 0; index < roster.size(); ++index) {
            auto& entry = roster[index];
            entry.x = offsets[index] * scale;
            entry.heading = Luminumbra::Vec2(static_cast<float>(index) - 6.0f, 1.0f);
            entry.profile.separation_radius = 2.0f * scale;
            entry.profile.flock_radius = 12.0f * scale;
        }
        ExpectRosterMatches(roster);
    }
}

TEST(InstinctLocomotion, GridRebuildsAfterPositionsAndRadiiChange) {
    const auto roster = SpreadRoster(120);
    entt::registry grid_registry;
    entt::registry reference_registry;
    const auto entities = PopulateGridRoster(grid_registry, roster);
    EXPECT_EQ(entities, PopulateGridRoster(reference_registry, roster));
    ExpectSameTick(grid_registry, reference_registry, entities);
    for (std::size_t index = 0; index < entities.size(); ++index) {
        for (auto* registry : {&grid_registry, &reference_registry}) {
            const auto entity = entities[index];
            registry->get<TransformComponent>(entity).position = Luminumbra::Vec3(
                static_cast<float>(index % 5) * 0.5f, 0.0f, static_cast<float>(index % 7) * 0.25f);
            auto& profile = registry->get<LocomotionProfile>(entity);
            profile.separation_radius = 0.75f;
            profile.flock_radius = 3.0f;
        }
    }
    ExpectSameTick(grid_registry, reference_registry, entities);
    ExpectSameTick(grid_registry, reference_registry, entities);
}

TEST(InstinctLocomotion, GridHandlesEmptyAndFullyDisabledRosters) {
    EXPECT_EQ(ExpectRosterMatches({}).neighbor_candidates_examined, 0u);
    auto roster = SpreadRoster(80);
    for (auto& entry : roster) {
        entry.profile.separation_strength = 0.0f;
        entry.profile.cohesion_strength = -0.5f;
        entry.profile.alignment_strength = 0.0f;
    }
    EXPECT_EQ(ExpectRosterMatches(roster).neighbor_candidates_examined, 0u);
    for (auto& entry : roster) {
        entry.profile = LocomotionProfile{};
        entry.profile.separation_radius = 0.0f;
        entry.profile.flock_radius = -1.0f;
    }
    EXPECT_EQ(ExpectRosterMatches(roster).neighbor_candidates_examined, 0u);
    // No positive finite radius exists from which to construct a usable grid.
    for (auto& entry : roster) {
        entry.profile.separation_radius = std::numeric_limits<float>::infinity();
        entry.profile.flock_radius = std::numeric_limits<float>::infinity();
    }
    EXPECT_EQ(ExpectRosterMatches(roster, 1).neighbor_candidates_examined, 12800u);
}

TEST(InstinctLocomotion, GridIncludesUnindexableSnapshotEntriesWithoutChangingResults) {
    auto roster = SpreadRoster(80);
    for (auto& entry : roster) {
        entry.idle = true;
    }
    roster.front().idle = false;
    roster[1].x = std::numeric_limits<float>::infinity();
    roster[2].z = -std::numeric_limits<float>::infinity();
    roster[3].x = std::numeric_limits<float>::quiet_NaN();
    roster[4].z = std::numeric_limits<float>::max();
    roster[5].x = -std::numeric_limits<float>::max();
    // A finite position outside the checked key range, alongside ordinary peers.
    roster[6].x = 0x1p35f;
    const auto stats = ExpectRosterMatches(roster);
    // Both ordinary grid queries retain self plus all six unindexable entries.
    EXPECT_EQ(stats.neighbor_candidates_examined, 14u);
}
