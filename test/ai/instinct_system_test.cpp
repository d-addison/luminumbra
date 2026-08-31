// InstinctSystem runtime coverage — the planner running over EnTT
// on the fixed simulation tick. Content here is synthetic engine-test data;
// game archetypes live under data/.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/InstinctSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"

#include <string>
#include <vector>

namespace {

using Luminumbra::Components::ActionPlanComponent;
using Luminumbra::Components::InstinctAgentComponent;
using Luminumbra::Components::Need;
using Luminumbra::Components::NeedsComponent;
using Luminumbra::Components::OpportunityComponent;
using Luminumbra::Components::TransformComponent;

entt::entity MakeAgent(entt::registry& registry, std::uint32_t replan_interval_ticks = 5) {
    const auto agent = registry.create();
    auto& identity = registry.emplace<InstinctAgentComponent>(agent);
    identity.actor_id = "test-actor";
    identity.archetype = "test-archetype";
    identity.replan_interval_ticks = replan_interval_ticks;
    auto& needs = registry.emplace<NeedsComponent>(agent);
    needs.needs = {
        Need{"alpha", 0.9f, 0.0f},
        Need{"beta", 0.2f, 0.01f},
    };
    return agent;
}

entt::entity MakeOpportunity(entt::registry& registry,
                             const std::string& id,
                             const std::string& need,
                             float satisfaction,
                             float urgency) {
    const auto entity = registry.create();
    auto& opportunity = registry.emplace<OpportunityComponent>(entity);
    opportunity.id = id;
    opportunity.action = "act-" + id;
    opportunity.target = "target-" + id;
    opportunity.need = need;
    opportunity.satisfaction = satisfaction;
    opportunity.urgency = urgency;
    return entity;
}

} // namespace

TEST(InstinctSystem, PlansOverRegistryAndWritesActionPlan) {
    entt::registry registry;
    const auto agent = MakeAgent(registry);
    const auto best = MakeOpportunity(registry, "best", "alpha", 0.95f, 0.8f);
    MakeOpportunity(registry, "weak", "beta", 0.30f, 0.2f);

    const auto stats = luminumbra::ai::RunInstinctSystemOnTick(registry, 1);
    EXPECT_EQ(stats.agents_seen, 1u);
    EXPECT_EQ(stats.agents_replanned, 1u);
    EXPECT_EQ(stats.opportunities_considered, 2u);

    const auto& identity = registry.get<InstinctAgentComponent>(agent);
    ASSERT_EQ(identity.current_plan.candidates.size(), 2u);
    EXPECT_TRUE(identity.current_plan.passed);
    EXPECT_EQ(identity.current_plan.selected_index, 0);
    EXPECT_EQ(identity.current_plan.candidates.front().id, "best");
    EXPECT_EQ(identity.current_plan.candidates.front().need, "alpha");

    ASSERT_TRUE(registry.all_of<ActionPlanComponent>(agent));
    const auto& action_plan = registry.get<ActionPlanComponent>(agent);
    ASSERT_EQ(action_plan.plan.size(), 1u);
    EXPECT_EQ(action_plan.plan.front().name, "act-best");
    EXPECT_EQ(action_plan.plan.front().target, best);
}

TEST(InstinctSystem, GrowsNeedsEveryTickAndReplansOnInterval) {
    entt::registry registry;
    const auto agent = MakeAgent(registry, 5);
    MakeOpportunity(registry, "only", "alpha", 0.9f, 0.5f);

    luminumbra::ai::RunInstinctSystemOnTick(registry, 1); // first plan
    for (std::uint64_t tick = 2; tick <= 5; ++tick) {
        const auto stats = luminumbra::ai::RunInstinctSystemOnTick(registry, tick);
        EXPECT_EQ(stats.agents_replanned, 0u) << "tick " << tick;
    }
    const auto stats = luminumbra::ai::RunInstinctSystemOnTick(registry, 6);
    EXPECT_EQ(stats.agents_replanned, 1u);

    const auto& identity = registry.get<InstinctAgentComponent>(agent);
    EXPECT_EQ(identity.plans_executed, 2u);
    EXPECT_EQ(identity.last_planned_tick, 6u);

    // beta grew 0.01 per tick over 6 ticks; alpha stayed put.
    const auto& needs = registry.get<NeedsComponent>(agent);
    EXPECT_FLOAT_EQ(needs.needs[0].pressure, 0.9f);
    EXPECT_NEAR(needs.needs[1].pressure, 0.2f + 6.0f * 0.01f, 1.0e-5f);
}

TEST(InstinctSystem, PlanIsIndependentOfOpportunityInsertionOrder) {
    const auto run = [](bool reversed) {
        entt::registry registry;
        const auto agent = MakeAgent(registry);
        if (reversed) {
            MakeOpportunity(registry, "zeta", "alpha", 0.40f, 0.9f);
            MakeOpportunity(registry, "apex", "alpha", 0.95f, 0.8f);
        } else {
            MakeOpportunity(registry, "apex", "alpha", 0.95f, 0.8f);
            MakeOpportunity(registry, "zeta", "alpha", 0.40f, 0.9f);
        }
        luminumbra::ai::RunInstinctSystemOnTick(registry, 1);
        return registry.get<InstinctAgentComponent>(agent).current_plan.checksum;
    };

    const std::string forward = run(false);
    const std::string backward = run(true);
    EXPECT_FALSE(forward.empty());
    EXPECT_EQ(forward, backward);
}

TEST(InstinctSystem, RespectsOpportunityRadiusWithTransforms) {
    entt::registry registry;
    const auto agent = MakeAgent(registry);
    registry.emplace<TransformComponent>(agent).position = {0.0f, 0.0f, 0.0f};

    const auto near_entity = MakeOpportunity(registry, "near", "alpha", 0.5f, 0.5f);
    registry.emplace<TransformComponent>(near_entity).position = {3.0f, 0.0f, 0.0f};
    registry.get<OpportunityComponent>(near_entity).radius = 10.0f;

    const auto far_entity = MakeOpportunity(registry, "far", "alpha", 0.99f, 0.99f);
    registry.emplace<TransformComponent>(far_entity).position = {100.0f, 0.0f, 0.0f};
    registry.get<OpportunityComponent>(far_entity).radius = 10.0f;

    const auto stats = luminumbra::ai::RunInstinctSystemOnTick(registry, 1);
    EXPECT_EQ(stats.opportunities_considered, 1u);

    const auto& identity = registry.get<InstinctAgentComponent>(agent);
    ASSERT_EQ(identity.current_plan.candidates.size(), 1u);
    EXPECT_EQ(identity.current_plan.candidates.front().id, "near");
    // Distance feeds the score: 3 m at weight 0.35.
    EXPECT_NEAR(identity.current_plan.candidates.front().score,
                (0.9 * 0.5 * 2.0) + (0.5 * 1.5) - (3.0 * 0.35),
                1.0e-4);
}
