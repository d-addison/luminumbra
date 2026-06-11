#include "InstinctSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "InstinctPlanner.h"
#include "../components/CoreComponents.h"
#include "../components/InstinctComponents.h"

namespace luminumbra::ai {
namespace {

float ClampUnit(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

double Round4(double value) {
    return std::round(value * 10000.0) / 10000.0;
}

struct OpportunitySource {
    entt::entity entity = entt::null;
    const Luminumbra::Components::OpportunityComponent* component = nullptr;
    bool has_position = false;
    float position[3] = {0.0f, 0.0f, 0.0f};
};

} // namespace

InstinctSystemTickStats RunInstinctSystemOnTick(entt::registry& registry, std::uint64_t tick) {
    using Luminumbra::Components::ActionPlanComponent;
    using Luminumbra::Components::InstinctAgentComponent;
    using Luminumbra::Components::NeedsComponent;
    using Luminumbra::Components::OpportunityComponent;
    using Luminumbra::Components::TransformComponent;

    InstinctSystemTickStats stats;

    // Snapshot the opportunities once per tick, sorted by id so planner
    // input order is independent of registry storage order.
    std::vector<OpportunitySource> opportunities;
    {
        auto opportunity_view = registry.view<const OpportunityComponent>();
        for (auto entity : opportunity_view) {
            OpportunitySource source;
            source.entity = entity;
            source.component = &opportunity_view.get<const OpportunityComponent>(entity);
            if (const auto* transform = registry.try_get<const TransformComponent>(entity)) {
                source.has_position = true;
                source.position[0] = transform->position.x;
                source.position[1] = transform->position.y;
                source.position[2] = transform->position.z;
            }
            opportunities.push_back(source);
        }
        std::stable_sort(opportunities.begin(), opportunities.end(),
                         [](const OpportunitySource& lhs, const OpportunitySource& rhs) {
                             return lhs.component->id < rhs.component->id;
                         });
    }

    auto agent_view = registry.view<InstinctAgentComponent, NeedsComponent>();
    for (auto entity : agent_view) {
        auto& agent = agent_view.get<InstinctAgentComponent>(entity);
        auto& needs = agent_view.get<NeedsComponent>(entity);
        ++stats.agents_seen;

        // 1. Need growth on every fixed tick.
        for (auto& need : needs.needs) {
            need.pressure = ClampUnit(need.pressure + need.growth_per_tick);
        }

        // 2. Replan when due.
        const bool never_planned = agent.plans_executed == 0;
        const std::uint64_t interval = std::max<std::uint32_t>(1u, agent.replan_interval_ticks);
        if (!never_planned && tick < agent.last_planned_tick + interval) {
            continue;
        }

        InstinctPlanRequest request;
        request.actor_id = agent.actor_id;
        request.archetype = agent.archetype;
        request.needs.reserve(needs.needs.size());
        for (const auto& need : needs.needs) {
            request.needs.push_back({need.name, static_cast<double>(need.pressure)});
        }

        const auto* agent_transform = registry.try_get<const TransformComponent>(entity);
        std::vector<entt::entity> candidate_entities;
        request.opportunities.reserve(opportunities.size());
        candidate_entities.reserve(opportunities.size());
        for (const OpportunitySource& source : opportunities) {
            const OpportunityComponent& component = *source.component;
            double distance = 0.0;
            if (agent_transform != nullptr && source.has_position) {
                const double dx = static_cast<double>(agent_transform->position.x) - source.position[0];
                const double dy = static_cast<double>(agent_transform->position.y) - source.position[1];
                const double dz = static_cast<double>(agent_transform->position.z) - source.position[2];
                distance = Round4(std::sqrt(dx * dx + dy * dy + dz * dz));
                if (component.radius > 0.0f && distance > static_cast<double>(component.radius)) {
                    continue;
                }
            }
            InstinctOpportunity opportunity;
            opportunity.id = component.id;
            opportunity.action = component.action;
            opportunity.target = component.target;
            opportunity.need = component.need;
            opportunity.satisfaction = static_cast<double>(component.satisfaction);
            opportunity.urgency = static_cast<double>(component.urgency);
            opportunity.distance = distance;
            opportunity.risk = static_cast<double>(component.risk);
            opportunity.stamina_cost = static_cast<double>(component.stamina_cost);
            request.opportunities.push_back(opportunity);
            candidate_entities.push_back(source.entity);
            ++stats.opportunities_considered;
        }

        agent.current_plan = PlanInstincts(request);
        agent.last_planned_tick = tick;
        ++agent.plans_executed;
        ++stats.agents_replanned;

        // Write the winner into the action plan. The candidate ranking is a
        // permutation of the request opportunities; recover the winning
        // opportunity entity by id.
        auto& action_plan = registry.emplace_or_replace<ActionPlanComponent>(entity);
        if (agent.current_plan.selected_index >= 0 &&
            static_cast<std::size_t>(agent.current_plan.selected_index) < agent.current_plan.candidates.size()) {
            const auto& winner = agent.current_plan.candidates[static_cast<std::size_t>(agent.current_plan.selected_index)];
            Luminumbra::Components::Action action;
            action.name = winner.action;
            for (std::size_t i = 0; i < request.opportunities.size(); ++i) {
                if (request.opportunities[i].id == winner.id) {
                    action.target = candidate_entities[i];
                    break;
                }
            }
            action_plan.plan.push_back(std::move(action));
        }
    }

    return stats;
}

} // namespace luminumbra::ai
