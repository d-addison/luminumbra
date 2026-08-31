// StimulusChannelGate. The ecology stimulus-channel registry
// feeding the InstinctSystem planner. Asserts:
//   * INERT: a creature with NO StimulusSubscriptionComponent plans IDENTICALLY
//     whether or not a stimulus context is supplied (the canonical-neutral
//     property that keeps world_hash at d950a6afc12a5cdc -- regression review / ).
//   * BEHAVIOR DIFFERS: a reactive creature (game-data opt-in) plans DIFFERENTLY
//     across weather fixtures (rain vs clear) and time-of-day fixtures (dawn vs
//     noon) -- the channels actually reach the plan.
//   * DETERMINISTIC: run==replay within a fixture (same tick + context => same
//     plan checksum, every run).
//
// All content here is synthetic engine-test data; game archetypes live under
// data/. The registry is pure-engine; the channel->need mapping is game data
// (a StimulusSubscriptionComponent built locally to stand in for an archetype).

#include "gtest/gtest.h"

#include "luminumbra_common/ai/InstinctSystem.h"
#include "luminumbra_common/ai/StimulusChannels.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"

#include <string>
#include <vector>

namespace {

using luminumbra::ai::RunInstinctSystemOnTick;
using luminumbra::ai::StimulusChannel;
using luminumbra::ai::StimulusChannelRegistry;
using luminumbra::ai::StimulusContext;
using Luminumbra::Components::InstinctAgentComponent;
using Luminumbra::Components::Need;
using Luminumbra::Components::NeedsComponent;
using Luminumbra::Components::OpportunityComponent;
using Luminumbra::Components::StimulusSubscription;
using Luminumbra::Components::StimulusSubscriptionComponent;

// A creature with two competing opportunities answering two different needs.
// As one need's pressure rises (driven by a stimulus channel) the planner's
// top pick flips -- so a plan-checksum / selected-action difference is a clean
// behavior-differs signal.
entt::entity MakeAgent(entt::registry& registry) {
    const auto agent = registry.create();
    auto& identity = registry.emplace<InstinctAgentComponent>(agent);
    identity.actor_id = "stimulus-actor";
    identity.archetype = "stimulus-archetype";
    identity.replan_interval_ticks = 1; // replan every tick so the fixture is crisp
    auto& needs = registry.emplace<NeedsComponent>(agent);
    needs.needs = {
        Need{"shelter_drive", 0.05f, 0.0f}, // raised by the weather channel
        Need{"forage_drive", 0.55f, 0.0f},  // the ambient baseline need
    };
    return agent;
}

void MakeOpportunity(entt::registry& registry,
                     const std::string& id,
                     const std::string& action,
                     const std::string& need,
                     float satisfaction,
                     float urgency) {
    const auto entity = registry.create();
    auto& opportunity = registry.emplace<OpportunityComponent>(entity);
    opportunity.id = id;
    opportunity.action = action;
    opportunity.target = "target-" + id;
    opportunity.need = need;
    opportunity.satisfaction = satisfaction;
    opportunity.urgency = urgency;
}

void AddShelterSubscription(entt::registry& registry, entt::entity agent, float gain) {
    auto& sub = registry.emplace<StimulusSubscriptionComponent>(agent);
    sub.subscriptions.push_back(
        StimulusSubscription{StimulusChannel::Weather, "shelter_drive", gain});
    sub.subscriptions.push_back(
        StimulusSubscription{StimulusChannel::TimeOfDay, "forage_drive", 0.20f});
}

// Build a registry of two opportunities: a shelter (answers shelter_drive) and a
// graze (answers forage_drive). With low shelter pressure the graze wins; once
// the weather channel pushes shelter_drive up, the shelter wins.
entt::entity StageScene(entt::registry& registry, bool subscribe, float gain) {
    const auto agent = MakeAgent(registry);
    MakeOpportunity(registry, "shelter-rock", "shelter", "shelter_drive", 0.90f, 0.80f);
    MakeOpportunity(registry, "graze-meadow", "graze", "forage_drive", 0.85f, 0.70f);
    if (subscribe) {
        AddShelterSubscription(registry, agent, gain);
    }
    return agent;
}

std::string PlanFor(entt::registry& registry, entt::entity agent) {
    const auto& plan = registry.get<InstinctAgentComponent>(agent).current_plan;
    if (plan.selected_index < 0 ||
        static_cast<std::size_t>(plan.selected_index) >= plan.candidates.size()) {
        return std::string();
    }
    return plan.candidates[static_cast<std::size_t>(plan.selected_index)].action;
}

} // namespace

// --- INERT: a non-subscriber plans identically with or without a context. ---
TEST(StimulusChannelGate, InertForNonSubscribers) {
    const auto run = [](bool with_context) {
        entt::registry registry;
        const auto agent = StageScene(registry, /*subscribe=*/false, /*gain=*/0.0f);
        StimulusContext ctx;
        ctx.tick = 1800; // a non-trivial tick (drives time-of-day/light channels)
        StimulusChannelRegistry stimulus(ctx);
        RunInstinctSystemOnTick(registry, 1, with_context ? &stimulus : nullptr);
        return registry.get<InstinctAgentComponent>(agent).current_plan.checksum;
    };
    const std::string without = run(false);
    const std::string with = run(true);
    EXPECT_FALSE(without.empty());
    // No subscription => the stimulus context never touches the plan: byte-equal
    // checksums. This is the property that keeps world_hash unchanged.
    EXPECT_EQ(without, with);
}

// --- BEHAVIOR DIFFERS across weather (clear vs rain). Same creature, same tick:
//     only the Weather channel scalar changes (clear precip 0.0 vs rain 0.9). The
//     subscriber's shelter_drive rises under rain and the plan flips graze->shelter.
TEST(StimulusChannelGate, BehaviorDiffersAcrossWeather) {
    const auto plan_under_precip = [](float precip) {
        entt::registry registry;
        const auto agent = StageScene(registry, /*subscribe=*/true, /*gain=*/1.0f);
        StimulusContext ctx;
        ctx.tick = 1800;              // FIXED tick: time-of-day is identical across fixtures
        ctx.precip_override = precip; // the ONLY difference: clear (0.0) vs rain (0.9)
        StimulusChannelRegistry stimulus(ctx);
        RunInstinctSystemOnTick(registry, 1, &stimulus);
        return PlanFor(registry, agent);
    };

    const std::string clear_plan = plan_under_precip(0.0f);
    const std::string rain_plan = plan_under_precip(0.9f);

    EXPECT_FALSE(clear_plan.empty());
    EXPECT_FALSE(rain_plan.empty());
    EXPECT_NE(clear_plan, rain_plan) << "clear=" << clear_plan << " rain=" << rain_plan;
    // Concretely: clear -> graze (low shelter pressure), rain -> shelter.
    EXPECT_EQ(clear_plan, "graze");
    EXPECT_EQ(rain_plan, "shelter");
}

// --- BEHAVIOR DIFFERS across time-of-day (dawn/midnight vs noon). ---
TEST(StimulusChannelGate, BehaviorDiffersAcrossTimeOfDay) {
    using luminumbra::ai::kTicksPerDayCycle;

    // The TimeOfDay channel is 0 at midnight and 1 at midday. Subscribe BOTH
    // needs so the plan responds to the day phase: shelter_drive via Weather (0
    // here) + a strong TimeOfDay push on forage_drive at noon.
    const auto plan_at = [](std::uint64_t tick) {
        entt::registry registry;
        const auto agent = registry.create();
        auto& identity = registry.emplace<InstinctAgentComponent>(agent);
        identity.actor_id = "tod-actor";
        identity.archetype = "tod-archetype";
        identity.replan_interval_ticks = 1;
        auto& needs = registry.emplace<NeedsComponent>(agent);
        // shelter_drive is the strong STATIC need; forage_drive starts low and is
        // boosted by the TimeOfDay channel at noon so the pick flips by time of day.
        needs.needs = {Need{"shelter_drive", 0.70f, 0.0f}, Need{"forage_drive", 0.05f, 0.0f}};
        MakeOpportunity(registry, "shelter-rock", "shelter", "shelter_drive", 0.80f, 0.55f);
        MakeOpportunity(registry, "graze-meadow", "graze", "forage_drive", 0.95f, 0.90f);
        auto& sub = registry.emplace<StimulusSubscriptionComponent>(agent);
        sub.subscriptions.push_back(
            StimulusSubscription{StimulusChannel::TimeOfDay, "forage_drive", 0.60f});
        StimulusContext ctx;
        ctx.tick = tick;
        StimulusChannelRegistry stimulus(ctx);
        RunInstinctSystemOnTick(registry, 1, &stimulus);
        return PlanFor(registry, agent);
    };

    const std::string midnight = plan_at(0);                 // TimeOfDay ~0
    const std::string noon = plan_at(kTicksPerDayCycle / 2); // TimeOfDay ~1

    EXPECT_FALSE(midnight.empty());
    EXPECT_FALSE(noon.empty());
    EXPECT_NE(midnight, noon) << "midnight=" << midnight << " noon=" << noon;
    EXPECT_EQ(midnight, "shelter"); // low forage drive at night
    EXPECT_EQ(noon, "graze");       // noon TimeOfDay boost wins forage
}

// --- DETERMINISTIC: run==replay within a fixture. ---
TEST(StimulusChannelGate, DeterministicRunEqualsReplay) {
    const auto run = [] {
        entt::registry registry;
        const auto agent = StageScene(registry, /*subscribe=*/true, /*gain=*/2.0f);
        StimulusContext ctx;
        ctx.tick = 2718;
        StimulusChannelRegistry stimulus(ctx);
        // Several ticks so need growth + channel modulation compound.
        for (std::uint64_t t = 1; t <= 8; ++t) {
            ctx.tick = 2718 + t;
            StimulusChannelRegistry step(ctx);
            RunInstinctSystemOnTick(registry, t, &step);
        }
        return registry.get<InstinctAgentComponent>(agent).current_plan.checksum;
    };
    const std::string first = run();
    const std::string second = run();
    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

// --- Channel scalars are bounded + the expected shape. ---
TEST(StimulusChannelGate, ChannelScalarsBoundedAndShaped) {
    using luminumbra::ai::kTicksPerDayCycle;
    StimulusContext midnight;
    midnight.tick = 0;
    StimulusChannelRegistry mid(midnight);

    StimulusContext noon;
    noon.tick = kTicksPerDayCycle / 2;
    StimulusChannelRegistry day(noon);

    for (const auto channel : {StimulusChannel::Weather,
                               StimulusChannel::Temperature,
                               StimulusChannel::TimeOfDay,
                               StimulusChannel::Season,
                               StimulusChannel::LightLevel}) {
        for (const auto* reg : {&mid, &day}) {
            const float s = reg->Sample(channel);
            EXPECT_GE(s, 0.0f);
            EXPECT_LE(s, 1.0f);
        }
    }
    // TimeOfDay rises from ~0 at midnight to ~1 at noon.
    EXPECT_LT(mid.Sample(StimulusChannel::TimeOfDay), 0.05f);
    EXPECT_GT(day.Sample(StimulusChannel::TimeOfDay), 0.95f);
}
