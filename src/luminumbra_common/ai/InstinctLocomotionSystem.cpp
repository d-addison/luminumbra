#include "InstinctLocomotionSystem.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../components/CoreComponents.h"
#include "../components/InstinctComponents.h"
#include "../core/DeterministicMath.h"

namespace luminumbra::ai {

InstinctLocomotionTickStats RunInstinctLocomotionOnTick(entt::registry& registry) {
    using Luminumbra::Components::ActionPlanComponent;
    using Luminumbra::Components::LocomotionIntentComponent;
    using Luminumbra::Components::LocomotionPathComponent;
    using Luminumbra::Components::LocomotionProfile;
    using Luminumbra::Components::TransformComponent;

    InstinctLocomotionTickStats stats;

    // Collect agents and visit them in a deterministic id order so the outcome
    // never depends on registry storage internals (mirrors InstinctSystem's
    // sort discipline). entt entity ids are creation-ordered integers.
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

    // snapshot of all locomotion agents (position + PRIOR-tick heading),
    // in deterministic id order, for the optional Reynolds flocking terms below
    // (separation / cohesion / alignment). Built ONCE per tick BEFORE the per-agent
    // wish is recomputed, so alignment reads last tick's velocities — there is no
    // intra-tick read-after-write coupling and the result is order-independent.
    // When no agent enables flocking this is unused; the canonical roster leaves
    // every strength at 0 so world_hash is unchanged.
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
    // `agents` is already id-sorted, so `neighbors` is too (deterministic scan).

    for (auto entity : agents) {
        ++stats.agents_seen;

        auto& plan = registry.get<ActionPlanComponent>(entity);
        const auto& tf = registry.get<const TransformComponent>(entity);
        const auto& profile = registry.get<const LocomotionProfile>(entity);

        auto& intent = registry.get_or_emplace<LocomotionIntentComponent>(entity);
        intent.wish_xz = Luminumbra::Vec2(0.0f);
        intent.arrived = false;

        // No current action (empty plan or index past the end) -> hold/idle.
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

        //  path-following: when the agent carries a LocomotionPathComponent
        // with a remaining waypoint (and is not fleeing), seek the WAYPOINT instead
        // of the action target directly. On waypoint arrival the index advances;
        // once the route is exhausted the agent resumes the plain action-target
        // seek so it still finishes onto its goal. No component -> byte-identical.
        auto* path = registry.try_get<LocomotionPathComponent>(entity);
        const bool following = !flee && path != nullptr && path->index < path->waypoints.size();
        const float goal_x = following ? path->waypoints[path->index].x : target_tf.position.x;
        const float goal_z = following ? path->waypoints[path->index].y : target_tf.position.z;

        // Horizontal vector to the goal (XZ plane; Y handled by the character's
        // ground-stick in physics). Float-only; Sqrt is IEEE-deterministic.
        const float dx = goal_x - tf.position.x;
        const float dz = goal_z - tf.position.z;
        const float dist = Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);

        const float slow = profile.slow_radius > profile.arrival_radius ? profile.slow_radius
                                                                        : profile.arrival_radius;

        if (flee) {
            //  flee/avoidance: move AWAY from the (threat) target until
            // beyond slow_radius (the "safe" distance), then the action completes.
            const float safe = slow > 0.0f ? slow : profile.arrival_radius;
            if (dist >= safe) {
                intent.arrived = true; // safe: stop fleeing, advance the plan
                ++stats.agents_arrived;
                ++plan.current_action_index;
                plan.time_in_current_action = 0.0f;
                continue;
            }
            // Full-speed flight directly away. Degenerate dist~0 (agent on top of
            // the threat) picks a deterministic +X so the wish is never NaN.
            float adx = dx, adz = dz;
            if (dist < 1e-4f) {
                adx = -1.0f;
                adz = 0.0f;
            } // away = -dir; -(-1)=+X
            const float ainv = 1.0f / Luminumbra::DeterministicMath::Sqrt(adx * adx + adz * adz);
            intent.wish_xz = Luminumbra::Vec2(-adx * ainv * profile.move_speed,
                                              -adz * ainv * profile.move_speed);
        } else {
            if (dist <= profile.arrival_radius) {
                if (following) {
                    // Reached this waypoint: advance the route (next tick seeks the
                    // next waypoint, or the action target once the path runs out).
                    // The PLAN is not advanced here — the action completes only on
                    // arrival at its real target, after the path is exhausted.
                    ++path->index;
                    continue;
                }
                // Arrived at the action target: hold and advance the plan.
                intent.arrived = true;
                ++stats.agents_arrived;
                ++plan.current_action_index;
                plan.time_in_current_action = 0.0f;
                continue;
            }
            // Seek + arrival ramp (Reynolds): full cruise outside slow_radius,
            // linear ramp toward zero between slow_radius and arrival_radius.
            float speed = profile.move_speed;
            if (dist < slow && slow > 0.0f) {
                speed = profile.move_speed * (dist / slow);
            }
            const float inv = 1.0f / dist; // dist > arrival_radius >= 0 -> safe
            intent.wish_xz = Luminumbra::Vec2(dx * inv * speed, dz * inv * speed);
        }

        //  obstacle/crowd avoidance (Reynolds separation), DATA-FLAGGED.
        // When separation is enabled, push away from nearby agents so they do not
        // pile onto a shared target at scale. Deterministic: neighbors are scanned
        // in id order; float-only; Sqrt is IEEE-deterministic. A zero strength (the
        // default) skips this entirely, keeping the canonical world_hash intact.
        if (profile.separation_strength > 0.0f && profile.separation_radius > 0.0f) {
            const float sep_r = profile.separation_radius;
            float push_x = 0.0f;
            float push_z = 0.0f;
            for (const auto& nb : neighbors) {
                if (nb.id == entity) {
                    continue;
                }
                const float nx = tf.position.x - nb.x;
                const float nz = tf.position.z - nb.z;
                const float nd = Luminumbra::DeterministicMath::Sqrt(nx * nx + nz * nz);
                if (nd > 0.0f && nd < sep_r) {
                    // Linear falloff: closer neighbors push harder. Normalize the
                    // away direction (1/nd) then weight by (1 - nd/sep_r).
                    const float w = (sep_r - nd) / (sep_r * nd);
                    push_x += nx * w;
                    push_z += nz * w;
                }
            }
            // Blend the avoidance into the seek wish, scaled to the cruise speed so
            // it is commensurate with the seek term, then clamp the combined wish to
            // move_speed so avoidance never exceeds the agent's locomotion budget.
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

        //  flocking/herding: cohesion (steer toward the local group's center
        // of mass) + alignment (match the group's mean heading). Both use
        // flock_radius and last tick's neighbor headings (snapshotted above), so the
        // result is deterministic and order-independent. DATA-FLAGGED: zero strength
        // skips, keeping world_hash intact. Composed on top of seek + separation;
        // the combined wish is then clamped to the move_speed budget.
        if ((profile.cohesion_strength > 0.0f || profile.alignment_strength > 0.0f) &&
            profile.flock_radius > 0.0f) {
            const float fr = profile.flock_radius;
            float cx = 0.0f, cz = 0.0f; // sum of neighbor positions (cohesion)
            float hx = 0.0f, hz = 0.0f; // sum of neighbor headings (alignment)
            int count = 0;
            for (const auto& nb : neighbors) {
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
                    // Toward the neighbor centroid (unit) * strength * cruise speed.
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
                    // Toward the mean neighbor heading (unit) * strength * cruise speed.
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

} // namespace luminumbra::ai
