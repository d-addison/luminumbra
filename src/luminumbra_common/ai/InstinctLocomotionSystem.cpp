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
    using Luminumbra::Components::LocomotionProfile;
    using Luminumbra::Components::TransformComponent;

    InstinctLocomotionTickStats stats;

    // Collect agents and visit them in a deterministic id order so the outcome
    // never depends on registry storage internals (mirrors InstinctSystem's
    // sort discipline). entt entity ids are creation-ordered integers.
    std::vector<entt::entity> agents;
    {
        auto view = registry.view<ActionPlanComponent, const TransformComponent,
                                   const LocomotionProfile>();
        for (auto entity : view) {
            agents.push_back(entity);
        }
    }
    std::sort(agents.begin(), agents.end(),
              [](entt::entity lhs, entt::entity rhs) {
                  return entt::to_integral(lhs) < entt::to_integral(rhs);
              });

    for (auto entity : agents) {
        ++stats.agents_seen;

        auto& plan = registry.get<ActionPlanComponent>(entity);
        const auto& tf = registry.get<const TransformComponent>(entity);
        const auto& profile = registry.get<const LocomotionProfile>(entity);

        auto& intent = registry.get_or_emplace<LocomotionIntentComponent>(entity);
        intent.wish_xz = Luminumbra::Vec2(0.0f);
        intent.arrived = false;

        // No current action (empty plan or index past the end) -> hold/idle.
        if (plan.plan.empty() ||
            plan.current_action_index >= plan.plan.size()) {
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

        // Horizontal vector to the target (XZ plane; Y handled by the character's
        // ground-stick in physics). Float-only; Sqrt is IEEE-deterministic.
        const float dx = target_tf.position.x - tf.position.x;
        const float dz = target_tf.position.z - tf.position.z;
        const float dist = Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);

        if (dist <= profile.arrival_radius) {
            // Arrived: hold and advance the plan so the next action (if any) runs.
            intent.arrived = true;
            ++stats.agents_arrived;
            ++plan.current_action_index;
            plan.time_in_current_action = 0.0f;
            continue;
        }

        // Seek + arrival ramp (Reynolds): full cruise outside slow_radius, linear
        // ramp toward zero between slow_radius and arrival_radius.
        const float slow = profile.slow_radius > profile.arrival_radius
                               ? profile.slow_radius
                               : profile.arrival_radius;
        float speed = profile.move_speed;
        if (dist < slow && slow > 0.0f) {
            speed = profile.move_speed * (dist / slow);
        }

        const float inv = 1.0f / dist; // dist > arrival_radius >= 0 -> safe
        intent.wish_xz = Luminumbra::Vec2(dx * inv * speed, dz * inv * speed);
        ++stats.agents_steered;
    }

    return stats;
}

} // namespace luminumbra::ai
