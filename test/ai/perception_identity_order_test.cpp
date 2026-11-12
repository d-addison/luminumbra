#include <gtest/gtest.h>

#include "luminumbra_common/ai/InstinctLocomotionSystem.h"
#include "luminumbra_common/ai/InstinctSystem.h"
#include "luminumbra_common/ai/PerceptionSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/components/SimulationIdentityComponent.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Luminumbra::Components;
using luminumbra::ai::PerceptionIdentityOrder;
using luminumbra::ai::RunInstinctLocomotionOnTick;
using luminumbra::ai::RunInstinctSystemOnTick;
using luminumbra::ai::RunPerceptionSystemOnTick;

entt::entity Identified(entt::registry& registry, std::uint64_t identity, bool churn = false) {
    if (churn) {
        // Retain unrelated residents and recycle a slot with a changed generation.
        (void)registry.create();
        const auto hole = registry.create();
        registry.destroy(hole);
    }
    const auto entity = registry.create();
    registry.emplace<SimulationIdentityComponent>(entity, identity);
    return entity;
}

entt::entity Observer(entt::registry& registry) {
    const auto entity = Identified(registry, 100);
    registry.emplace<TransformComponent>(entity);
    registry.emplace<PerceptionComponent>(entity).faction = 1;
    registry.emplace<AwarenessComponent>(entity);
    return entity;
}

entt::entity Signal(entt::registry& registry, std::uint64_t identity, float z, bool churn) {
    const auto entity = Identified(registry, identity, churn);
    registry.emplace<TransformComponent>(entity).position = {10.0f, 0.0f, z};
    registry.emplace<SensableComponent>(entity).faction = 2;
    return entity;
}

entt::entity Planner(entt::registry& registry) {
    const auto entity = Identified(registry, 100);
    registry.emplace<InstinctAgentComponent>(entity);
    registry.emplace<NeedsComponent>(entity).needs.push_back(
        {.name = "food", .pressure = 0.9f, .growth_per_tick = 0.01f});
    registry.emplace<TransformComponent>(entity);
    return entity;
}

entt::entity
Opportunity(entt::registry& registry, std::uint64_t identity, bool positioned, bool churn) {
    const auto entity = Identified(registry, identity, churn);
    auto& source = registry.emplace<OpportunityComponent>(entity);
    source.id = "food:patch";
    source.action = "eat";
    source.target = "patch";
    source.need = "food";
    source.satisfaction = 0.9f;
    source.radius = 5.0f;
    if (positioned) {
        registry.emplace<TransformComponent>(entity).position = {1.0f, 0.0f, 0.0f};
    }
    return entity;
}

TEST(PerceptionIdentityOrder, EqualSignalsUseIdentityAcrossHandleGenerationsAndStorageOrders) {
    for (const bool reversed : {false, true}) {
        for (const bool hearing : {false, true}) {
            entt::registry registry;
            registry.ctx().emplace<PerceptionIdentityOrder>();
            const auto observer = Observer(registry);
            auto add = [&](std::uint64_t identity, float z) {
                const auto entity = Signal(registry, identity, z, reversed);
                if (hearing) {
                    registry.get<SensableComponent>(entity).noise_loudness = 1.0f;
                }
            };
            if (reversed) {
                add(0x100000001ull, -1.0f);
                add(9, 1.0f);
            } else {
                add(9, 1.0f);
                add(0x100000001ull, -1.0f);
            }
            if (hearing) {
                // Face away so only hearing supplies the equal signals.
                registry.get<PerceptionComponent>(observer).facing_x = -1.0f;
            }
            const auto stats = RunPerceptionSystemOnTick(registry, 1.0f);
            EXPECT_EQ(stats.sensing, 1u);
            const auto& awareness = registry.get<AwarenessComponent>(observer).awareness;
            EXPECT_TRUE(awareness.has_last_known);
            EXPECT_EQ(awareness.last_known_z, 1.0f);
        }
    }
}

TEST(PerceptionIdentityOrder, EqualOpportunityNamesMatchInlineAndIndexedWithHandleChurn) {
    std::array<std::string, 2> checksums;
    for (const bool indexed : {false, true}) {
        for (const bool reversed : {false, true}) {
            for (const bool positioned : {false, true}) {
                entt::registry registry;
                registry.ctx().emplace<PerceptionIdentityOrder>();
                const auto agent = Planner(registry);
                entt::entity low = entt::null;
                if (reversed) {
                    Opportunity(registry, 0x100000001ull, positioned, true);
                    low = Opportunity(registry, 9, positioned, true);
                } else {
                    low = Opportunity(registry, 9, positioned, false);
                    Opportunity(registry, 0x100000001ull, positioned, false);
                }
                const auto stats = RunInstinctSystemOnTick(registry, 1, nullptr, indexed);
                EXPECT_EQ(stats.opportunities_considered, 2u);
                const auto& actions = registry.get<ActionPlanComponent>(agent).plan;
                ASSERT_EQ(actions.size(), 1u);
                EXPECT_EQ(actions.front().target, low);
                const auto& plan = registry.get<InstinctAgentComponent>(agent).current_plan;
                ASSERT_EQ(plan.candidates.size(), 2u);
                EXPECT_EQ(plan.candidates.front().id, "food:patch:9");
                EXPECT_EQ(registry.get<OpportunityComponent>(low).id, "food:patch");
                // Compare each distance mode across both gathers and permutations.
                auto& checksum = checksums[positioned ? 1u : 0u];
                if (checksum.empty()) {
                    checksum = plan.checksum;
                }
                EXPECT_EQ(plan.checksum, checksum);
            }
        }
    }
}

TEST(PerceptionIdentityOrder, RepeatedNamesResolveTheActualWinnerAndRespectRadius) {
    for (const bool indexed : {false, true}) {
        entt::registry registry;
        registry.ctx().emplace<PerceptionIdentityOrder>();
        const auto agent = Planner(registry);
        const auto weak = Opportunity(registry, 1, true, false);
        registry.get<OpportunityComponent>(weak).satisfaction = 0.1f;
        const auto strong = Opportunity(registry, 2, true, false);
        const auto outside = Opportunity(registry, 3, true, false);
        registry.get<TransformComponent>(outside).position.x = 100.0f;
        const auto stats = RunInstinctSystemOnTick(registry, 1, nullptr, indexed);
        EXPECT_EQ(stats.opportunities_considered, 2u);
        const auto& actions = registry.get<ActionPlanComponent>(agent).plan;
        ASSERT_EQ(actions.size(), 1u);
        EXPECT_EQ(actions.front().target, strong);
    }
}

struct FlockResult {
    Luminumbra::Vec2 wish;
    luminumbra::ai::InstinctLocomotionTickStats stats;
};

FlockResult Flock(bool reversed, bool sparse, bool identified_order) {
    entt::registry registry;
    if (identified_order) {
        registry.ctx().emplace<PerceptionIdentityOrder>();
    }
    const auto target = Identified(registry, 200);
    registry.emplace<TransformComponent>(target).position = {0.0f, 0.0f, 100.0f};
    const auto agent = Identified(registry, 100);
    registry.emplace<TransformComponent>(agent);
    auto& profile = registry.emplace<LocomotionProfile>(agent);
    profile.separation_strength = 0.0f;
    profile.cohesion_strength = 0.0f;
    profile.alignment_strength = 1.0f;
    profile.flock_radius = 10.0f;
    registry.emplace<ActionPlanComponent>(agent).plan.push_back(
        {.name = "approach", .target = target, .duration = 1.0f, .flee = false});
    // All prior velocities fit the movement budget. The small term rounds away
    // between +1 and -1, but survives when their cancellation happens first.
    constexpr std::array headings{1.0f, 0x1p-25f, -1.0f};
    constexpr std::array<std::size_t, 3> permutation{0, 2, 1};
    for (std::size_t ordinal = 0; ordinal < headings.size(); ++ordinal) {
        const auto index = reversed ? permutation[ordinal] : ordinal;
        const auto neighbor = Identified(registry, 1 + index, reversed);
        registry.emplace<TransformComponent>(neighbor).position = {
            1.0f + static_cast<float>(index), 0.0f, 0.0f};
        auto& neighbor_profile = registry.emplace<LocomotionProfile>(neighbor);
        neighbor_profile.separation_strength = 0.0f;
        neighbor_profile.cohesion_strength = 0.0f;
        neighbor_profile.alignment_strength = 0.0f;
        registry.emplace<ActionPlanComponent>(neighbor);
        registry.emplace<LocomotionIntentComponent>(neighbor).wish_xz = {headings[index], 0.0f};
    }
    if (sparse) {
        // 64 remote participants make the local three-neighbor query use the grid.
        // They never contribute to its steering; no nonparticipant enters the scan.
        for (std::uint64_t index = 0; index < 64; ++index) {
            const auto remote = Identified(registry, 1000 + index, reversed);
            registry.emplace<TransformComponent>(remote).position = {
                1000.0f + static_cast<float>(index) * 100.0f, 0.0f, 0.0f};
            registry.emplace<LocomotionProfile>(remote);
            registry.emplace<ActionPlanComponent>(remote);
        }
    }
    const auto stats = RunInstinctLocomotionOnTick(registry);
    return {.wish = registry.get<LocomotionIntentComponent>(agent).wish_xz, .stats = stats};
}

TEST(PerceptionIdentityOrder, FlockAccumulationIsExactAcrossHandleAndGridPermutations) {
    const auto expected = Flock(false, false, true);
    for (const bool reversed : {false, true}) {
        for (const bool sparse : {false, true}) {
            const auto result = Flock(reversed, sparse, true);
            EXPECT_EQ(Luminumbra::DeterministicMath::BitsOf(result.wish.x),
                      Luminumbra::DeterministicMath::BitsOf(expected.wish.x));
            EXPECT_EQ(Luminumbra::DeterministicMath::BitsOf(result.wish.y),
                      Luminumbra::DeterministicMath::BitsOf(expected.wish.y));
            EXPECT_EQ(result.stats.neighbor_exhaustive_queries, sparse ? 0u : 1u);
            EXPECT_EQ(result.stats.neighbor_candidates_examined, 4u);
        }
    }
    EXPECT_EQ(expected.wish.x, 0.0f);
    EXPECT_EQ(expected.wish.y, 2.0f);
    // Control: handle ordering loses the middle term differently under permutation.
    const auto legacy = Flock(false, false, false);
    const auto permuted = Flock(true, false, false);
    EXPECT_NE(Luminumbra::DeterministicMath::BitsOf(legacy.wish.x),
              Luminumbra::DeterministicMath::BitsOf(permuted.wish.x));
}

TEST(PerceptionIdentityOrder, InvalidSourceIdentitiesRefuseBeforeAwarenessOrNeedsChange) {
    for (const auto invalid :
         {std::uint64_t{0}, std::uint64_t{1} << 63, std::numeric_limits<std::uint64_t>::max()}) {
        entt::registry perception;
        perception.ctx().emplace<PerceptionIdentityOrder>();
        const auto observer = Observer(perception);
        Signal(perception, invalid, 1.0f, false);
        EXPECT_THROW(RunPerceptionSystemOnTick(perception, 1.0f), std::logic_error);
        EXPECT_EQ(perception.get<AwarenessComponent>(observer).awareness.meter, 0.0f);

        entt::registry instinct;
        instinct.ctx().emplace<PerceptionIdentityOrder>();
        const auto agent = Planner(instinct);
        Opportunity(instinct, invalid, false, false);
        EXPECT_THROW(RunInstinctSystemOnTick(instinct, 1), std::logic_error);
        EXPECT_EQ(instinct.get<NeedsComponent>(agent).needs.front().pressure, 0.9f);
        EXPECT_FALSE(instinct.all_of<ActionPlanComponent>(agent));
    }
}

TEST(PerceptionIdentityOrder, MissingAndDuplicateParticipantIdentitiesRefuseBeforeMutation) {
    for (const bool missing : {false, true}) {
        entt::registry registry;
        registry.ctx().emplace<PerceptionIdentityOrder>();
        const auto first = Planner(registry);
        const auto second = Planner(registry); // duplicate identity 100
        if (missing) {
            registry.remove<SimulationIdentityComponent>(second);
        }
        EXPECT_THROW(RunInstinctSystemOnTick(registry, 1), std::logic_error);
        EXPECT_EQ(registry.get<NeedsComponent>(first).needs.front().pressure, 0.9f);
        EXPECT_FALSE(registry.all_of<ActionPlanComponent>(first));
        for (const auto entity : {first, second}) {
            registry.emplace<LocomotionProfile>(entity);
            registry.emplace<ActionPlanComponent>(entity);
        }
        EXPECT_THROW(RunInstinctLocomotionOnTick(registry), std::logic_error);
        EXPECT_FALSE(registry.all_of<LocomotionIntentComponent>(first));
    }
}

TEST(PerceptionIdentityOrder, EverySourceAndPerceiverGatherRejectsMissingAndDuplicateIds) {
    for (const bool missing : {false, true}) {
        for (const bool bad_source : {false, true}) {
            entt::registry perception;
            perception.ctx().emplace<PerceptionIdentityOrder>();
            const auto observer = Observer(perception);
            const auto other =
                bad_source ? Signal(perception, 1, 1.0f, false) : Observer(perception);
            if (bad_source) {
                Signal(perception, 1, -1.0f, false);
            }
            if (missing) {
                perception.remove<SimulationIdentityComponent>(other);
            }
            EXPECT_THROW(RunPerceptionSystemOnTick(perception, 1.0f), std::logic_error);
            EXPECT_EQ(perception.get<AwarenessComponent>(observer).awareness.meter, 0.0f);
        }
        entt::registry instinct;
        instinct.ctx().emplace<PerceptionIdentityOrder>();
        const auto agent = Planner(instinct);
        const auto source = Opportunity(instinct, 1, false, false);
        Opportunity(instinct, 1, false, false);
        if (missing) {
            instinct.remove<SimulationIdentityComponent>(source);
        }
        EXPECT_THROW(RunInstinctSystemOnTick(instinct, 1), std::logic_error);
        EXPECT_EQ(instinct.get<NeedsComponent>(agent).needs.front().pressure, 0.9f);
    }
}

TEST(PerceptionIdentityOrder, EmptySourcesAndSingletonAgentsProduceIdleOutputs) {
    entt::registry registry;
    registry.ctx().emplace<PerceptionIdentityOrder>();
    EXPECT_EQ(RunPerceptionSystemOnTick(registry, 1.0f).perceivers, 0u);
    EXPECT_EQ(RunInstinctSystemOnTick(registry, 1).agents_seen, 0u);
    EXPECT_EQ(RunInstinctLocomotionOnTick(registry).agents_seen, 0u);
    const auto agent = Planner(registry);
    for (const bool indexed : {false, true}) {
        const auto stats = RunInstinctSystemOnTick(registry, indexed ? 100u : 1u, nullptr, indexed);
        EXPECT_EQ(stats.opportunities_considered, 0u);
        EXPECT_TRUE(registry.get<ActionPlanComponent>(agent).plan.empty());
    }
    registry.emplace<LocomotionProfile>(agent);
    EXPECT_EQ(RunInstinctLocomotionOnTick(registry).agents_idle, 1u);
    EXPECT_EQ(registry.get<LocomotionIntentComponent>(agent).wish_xz, Luminumbra::Vec2(0.0f));
}

TEST(PerceptionIdentityOrder, CapturedRegionAdmissionFiltersBeforeIdentityValidation) {
    entt::registry registry;
    registry.ctx().emplace<PerceptionIdentityOrder>();
    const auto agent = Planner(registry);
    registry.get<TransformComponent>(agent).position = {4.0f, 0.0f, 4.0f};
    registry.emplace<CreatureComponent>(agent);
    registry.emplace<PerceptionComponent>(agent).faction = 1;
    registry.emplace<AwarenessComponent>(agent);
    registry.emplace<LocomotionProfile>(agent);
    const auto source = Opportunity(registry, 9, true, false);
    registry.get<TransformComponent>(source).position = {6.0f, 0.0f, 4.0f};
    registry.emplace<CreatureComponent>(source);
    registry.emplace<SensableComponent>(source).faction = 2;

    // A suspended participant is deliberately unidentified. None of the three
    // systems may admit it, validate it, or mutate its state during this tick.
    const auto suspended = registry.create();
    registry.emplace<CreatureComponent>(suspended);
    registry.emplace<TransformComponent>(suspended).position = {513.0f, 0.0f, 4.0f};
    registry.emplace<PerceptionComponent>(suspended);
    registry.emplace<AwarenessComponent>(suspended);
    registry.emplace<SensableComponent>(suspended);
    registry.emplace<OpportunityComponent>(suspended);
    registry.emplace<InstinctAgentComponent>(suspended);
    registry.emplace<NeedsComponent>(suspended);
    registry.emplace<LocomotionProfile>(suspended);
    registry.emplace<ActionPlanComponent>(suspended);

    const std::array<Luminumbra::world::RegionKey, 1> due{{{0, 0}}};
    const Luminumbra::world::RegionConsumerGate gate(registry, due);
    luminumbra::ai::CreatureTickAdmission admission;
    admission.Capture(registry, gate);
    ASSERT_FALSE(admission.Allows(suspended));
    EXPECT_EQ(RunPerceptionSystemOnTick(registry, 1.0f, &admission).perceivers, 1u);
    EXPECT_EQ(RunInstinctSystemOnTick(registry, 1, nullptr, true, &admission).agents_seen, 1u);
    EXPECT_EQ(RunInstinctLocomotionOnTick(registry, &admission).agents_seen, 1u);
    EXPECT_FALSE(registry.all_of<LocomotionIntentComponent>(suspended));
    EXPECT_EQ(registry.get<InstinctAgentComponent>(suspended).plans_executed, 0u);
    EXPECT_EQ(registry.get<AwarenessComponent>(suspended).awareness.meter, 0.0f);
}

TEST(PerceptionIdentityOrder, EmptySingletonAndAllIdentityBytesUseOnlySuppliedParticipants) {
    entt::registry registry;
    auto& order = registry.ctx().emplace<PerceptionIdentityOrder>();
    std::vector<entt::entity> entries;
    const auto entity_of = [](entt::entity entity) {
        return entity;
    };
    order.Sort(registry, entries, entity_of);
    EXPECT_EQ(order.participant_visits(), 0u);
    entries.push_back(Identified(registry, (std::uint64_t{1} << 63) - 1));
    order.Sort(registry, entries, entity_of);
    EXPECT_EQ(order.participant_visits(), 1u);
    for (unsigned shift = 0; shift < 63; shift += 8) {
        entries.push_back(Identified(registry, std::uint64_t{1} << shift));
    }
    for (int index = 0; index < 256; ++index) {
        (void)registry.create();
    }
    std::vector<std::uint64_t> expected;
    for (const auto entity : entries) {
        expected.push_back(registry.get<SimulationIdentityComponent>(entity).id);
    }
    std::sort(expected.begin(), expected.end());
    order.Sort(registry, entries, entity_of);
    EXPECT_EQ(order.participant_visits(), entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        EXPECT_EQ(registry.get<SimulationIdentityComponent>(entries[index]).id, expected[index]);
    }
    order.Sort(registry, entries, entity_of);
    EXPECT_EQ(order.participant_visits(), entries.size());
    const auto empty_stats = RunPerceptionSystemOnTick(registry, 1.0f);
    EXPECT_EQ(empty_stats.perceivers, 0u);
}
} // namespace
