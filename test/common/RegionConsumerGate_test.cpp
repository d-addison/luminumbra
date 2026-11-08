#include "../support/UniqueTempPath.h"
#include "luminumbra_common/ai/LifespanSystem.h"
#include "luminumbra_common/ai/WildlifeFoliageSystem.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CropLifecycleSystem.h"
#include "luminumbra_common/systems/FireSpreadSystem.h"
#include "luminumbra_common/systems/PlantDiseaseSystem.h"
#include "luminumbra_common/systems/PollinationSystem.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {
namespace C = Luminumbra::Components;
namespace F = luminumbra::foliage;
using Luminumbra::world::RegionConsumerGate;
using Luminumbra::world::RegionKey;

entt::entity Plant(entt::registry& registry, float x) {
    const auto entity = registry.create();
    registry.emplace<C::PlantTag>(entity);
    registry.emplace<C::TransformComponent>(entity).position = {x, 0.0f, 0.0f};
    registry.emplace<C::PlantGenomeComponent>(entity).genes.fill(0.4f);
    registry.emplace<C::PlantGrowthComponent>(entity).stage =
        static_cast<std::uint8_t>(C::PlantStage::Fruiting);
    registry.emplace<C::PollinationTag>(entity);
    registry.emplace<C::PollinationComponent>(entity);
    registry.emplace<C::PlantHealthComponent>(entity);
    return entity;
}

std::string PlantBytes(const entt::registry& registry) {
    return Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        F::BuildPlantEntitySnapshot(registry, true));
}

void AddCounters(entt::registry& registry, entt::entity entity) {
    auto& crop = registry.emplace<C::CropLifecycleComponent>(entity);
    crop.perennial = true;
    crop.lifespan_ticks = 2;
    crop.ripe_ticks = 1;
    auto& health = registry.get<C::PlantHealthComponent>(entity);
    health.state = static_cast<std::uint8_t>(C::PlantDiseaseState::Infected);
    health.infection = 800;
    auto& fire = registry.emplace<C::CombustibleComponent>(entity);
    fire.set_state(C::BurnState::Burning);
    fire.burn_ticks_remaining = 20;
    registry.emplace<C::GrazeableComponent>(entity).biomass = 0.8f;
    registry.emplace<C::MortalComponent>(entity).lifespan_ticks = 1;
}

// Return all telemetry as well as comparing the complete component projection.
nlohmann::json
StepConsumers(entt::registry& registry, const RegionConsumerGate& gate, bool explicit_gate = true) {
    const auto pollen = explicit_gate ? F::RunPollinationOnTick(registry, 100, {}, 0, 0.05f, gate)
                                      : F::RunPollinationOnTick(registry, 100);
    const auto disease = explicit_gate ? F::RunPlantDiseaseOnTick(registry, 100, 0, gate)
                                       : F::RunPlantDiseaseOnTick(registry, 100);
    const auto crops = explicit_gate ? F::RunCropLifecycleOnTick(registry, 100, 0, gate)
                                     : F::RunCropLifecycleOnTick(registry, 100);
    const auto fire = explicit_gate
                          ? luminumbra::sim::RunFireSpreadOnTick(registry, 100, {}, {}, gate)
                          : luminumbra::sim::RunFireSpreadOnTick(registry, 100);
    const auto grazing = explicit_gate
                             ? luminumbra::ai::RunWildlifeFoliageOnTick(registry, 100, {}, gate)
                             : luminumbra::ai::RunWildlifeFoliageOnTick(registry, 100);
    const auto lifespan = explicit_gate ? luminumbra::ai::RunLifespanOnTick(registry, 100, gate)
                                        : luminumbra::ai::RunLifespanOnTick(registry, 100);
    return {pollen.considered,
            pollen.donors,
            pollen.crossed,
            disease.considered,
            disease.infected,
            disease.new_infections,
            disease.recovered,
            disease.died,
            crops.ripening,
            crops.reseeded,
            crops.reset,
            crops.died,
            fire.considered,
            fire.ignited,
            fire.burnt_out,
            grazing.grazers,
            grazing.plants,
            grazing.grazed_plants,
            grazing.biomass_removed,
            grazing.biomass_regrown,
            lifespan.aged,
            lifespan.died};
}

TEST(RegionConsumerGate, BlockedOwnersRetainCountersAndCannotReseedOrReset) {
    entt::registry registry;
    for (const auto stage : {C::PlantStage::Juvenile, C::PlantStage::Fruiting}) {
        const auto entity = Plant(registry, 513.0f);
        AddCounters(registry, entity);
        registry.get<C::PlantGrowthComponent>(entity).stage = static_cast<std::uint8_t>(stage);
        registry.emplace<C::PlantRegionComponent>(
            entity, std::uint64_t{1} + entt::to_integral(entity), 1, 0, std::uint64_t{0});
    }
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(registry, due);
    const auto before = PlantBytes(registry);
    const auto telemetry = StepConsumers(registry, gate);
    EXPECT_EQ(PlantBytes(registry), before);
    for (const auto& counter : telemetry) {
        EXPECT_EQ(counter, 0);
    }
    // The same roster takes all six mutation paths when admission is removed.
    const auto advanced = StepConsumers(registry, {});
    EXPECT_NE(PlantBytes(registry), before);
    EXPECT_GT(advanced.at(0).get<int>(), 0);
    EXPECT_GT(advanced.at(3).get<int>(), 0);
    EXPECT_EQ(advanced.at(9), 1);
    EXPECT_GT(advanced.at(12).get<int>(), 0);
    EXPECT_GT(advanced.at(19).get<float>(), 0.0f);
    EXPECT_GT(advanced.at(20).get<int>(), 0);
}

TEST(RegionConsumerGate, DiseaseAndPollenRequireBothSourceAndTargetAdmission) {
    for (const bool block_source : {false, true}) {
        SCOPED_TRACE(block_source);
        entt::registry registry;
        const auto source = Plant(registry, 512.5f);
        const auto target = Plant(registry, 511.5f);
        auto& health = registry.get<C::PlantHealthComponent>(source);
        health.state = static_cast<std::uint8_t>(C::PlantDiseaseState::Infected);
        health.infection = 1000;
        registry.get<C::PlantGrowthComponent>(target).stage =
            static_cast<std::uint8_t>(C::PlantStage::Juvenile);
        const std::array<RegionKey, 1> due{{{block_source ? 0 : 1, 0}}};
        const RegionConsumerGate gate(registry, due);
        const auto blocked = block_source ? source : target;
        const auto blocked_before = registry.get<C::PlantHealthComponent>(blocked);
        F::RunPlantDiseaseOnTick(registry, 1, 0, gate);
        F::RunPollinationOnTick(registry, 1, {}, 0, 0.05f, gate);
        EXPECT_EQ(registry.get<C::PlantHealthComponent>(target).infection, 0u);
        EXPECT_EQ(registry.get<C::PollinationComponent>(target).crosses, 0u);
        EXPECT_EQ(registry.get<C::PlantHealthComponent>(blocked).infection,
                  blocked_before.infection);
        EXPECT_EQ(registry.get<C::PlantHealthComponent>(blocked).infected_ticks,
                  blocked_before.infected_ticks);
        EXPECT_EQ(registry.get<C::PollinationComponent>(blocked).last_pollen_tick, 0u);
        F::RunPlantDiseaseOnTick(registry, 2);
        F::RunPollinationOnTick(registry, 2);
        EXPECT_GT(registry.get<C::PlantHealthComponent>(target).infection, 0u);
        EXPECT_EQ(registry.get<C::PollinationComponent>(target).crosses, 1u);
    }
}

TEST(RegionConsumerGate, EveryFireSourceAndTargetRequiresAdmission) {
    for (int source_kind = 0; source_kind < 3; ++source_kind) {
        for (const bool block_source : {false, true}) {
            SCOPED_TRACE(source_kind);
            SCOPED_TRACE(block_source);
            entt::registry registry;
            const auto source = Plant(registry, 512.25f);
            const auto target = Plant(registry, 511.75f);
            registry.emplace<C::CombustibleComponent>(target);
            std::vector<luminumbra::sim::FireIgnitionSource> external;
            if (source_kind == 0) {
                auto& burning = registry.emplace<C::CombustibleComponent>(source);
                burning.set_state(C::BurnState::Burning);
                burning.burn_ticks_remaining = 20;
            } else if (source_kind == 1) {
                registry.emplace<C::IgnitionSourceComponent>(source);
            } else {
                external.push_back({{512.25f, 0.0f}, 3.0f, 1.0f});
            }
            const std::array<RegionKey, 1> due{{{block_source ? 0 : 1, 0}}};
            const RegionConsumerGate gate(registry, due);
            luminumbra::sim::RunFireSpreadOnTick(registry, 1, {}, external, gate);
            EXPECT_EQ(registry.get<C::CombustibleComponent>(target).state(), C::BurnState::Unburnt);
            if (source_kind == 0 && block_source) {
                EXPECT_EQ(registry.get<C::CombustibleComponent>(source).burn_ticks_remaining, 20u);
            }
            luminumbra::sim::RunFireSpreadOnTick(registry, 2, {}, external);
            EXPECT_EQ(registry.get<C::CombustibleComponent>(target).state(), C::BurnState::Burning);
        }
    }
}

TEST(RegionConsumerGate, GrazingRequiresBothPlantAndCreatureAdmission) {
    for (const bool block_creature : {false, true}) {
        SCOPED_TRACE(block_creature);
        entt::registry registry;
        const auto plant = Plant(registry, 511.75f);
        registry.emplace<C::GrazeableComponent>(plant).biomass = 0.8f;
        const auto creature = registry.create();
        registry.emplace<C::CreatureComponent>(creature).hunger = 0.5f;
        registry.emplace<C::TransformComponent>(creature).position = {512.25f, 0.0f, 0.0f};
        const std::array<RegionKey, 1> due{{{block_creature ? 0 : 1, 0}}};
        const RegionConsumerGate gate(registry, due);
        const auto stats = luminumbra::ai::RunWildlifeFoliageOnTick(registry, 1, {}, gate);
        EXPECT_EQ(stats.grazed_plants, 0);
        EXPECT_EQ(registry.get<C::CreatureComponent>(creature).hunger, 0.5f);
        if (block_creature) {
            EXPECT_GT(registry.get<C::GrazeableComponent>(plant).biomass, 0.8f);
        } else {
            EXPECT_EQ(registry.get<C::GrazeableComponent>(plant).biomass, 0.8f);
        }
        luminumbra::ai::RunWildlifeFoliageOnTick(registry, 2);
        EXPECT_LT(registry.get<C::CreatureComponent>(creature).hunger, 0.5f);
        EXPECT_LT(registry.get<C::GrazeableComponent>(plant).biomass, 0.8f);
    }
}

TEST(RegionConsumerGate, DefaultGateMatchesLegacyBytesAndTelemetry) {
    entt::registry implicit, explicit_default, admitted, blocked;
    for (auto* registry : {&implicit, &explicit_default, &admitted, &blocked}) {
        AddCounters(*registry, Plant(*registry, 100.0f));
        Plant(*registry, 101.0f);
    }
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(admitted, due);
    const auto legacy = StepConsumers(implicit, {}, false);
    EXPECT_EQ(StepConsumers(explicit_default, {}), legacy);
    EXPECT_EQ(StepConsumers(admitted, gate), legacy);
    EXPECT_EQ(PlantBytes(explicit_default), PlantBytes(implicit));
    EXPECT_EQ(PlantBytes(admitted), PlantBytes(implicit));
    const auto before = PlantBytes(blocked);
    EXPECT_NE(StepConsumers(blocked, RegionConsumerGate(blocked, {})), legacy);
    EXPECT_EQ(PlantBytes(blocked), before);
}

TEST(RegionConsumerGate, SessionUsesSealedDueOwnersAndFlagOffRetainsLegacyProgress) {
    for (const bool enabled : {false, true}) {
        SCOPED_TRACE(enabled);
        Luminumbra::world::GameSession session;
        session.SetActiveRegionsEnabled(enabled);
        session.SetReplicatedSimulationAnchors({});
        session.PinActiveRegion({0, 0}, true);
        auto& registry = session.GetRegistry();
        const auto entity = Plant(registry, 513.0f);
        AddCounters(registry, entity);
        Plant(registry, 514.0f);
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(entity).generations, enabled ? 0u : 1u);
        EXPECT_EQ(registry.get<C::PlantHealthComponent>(entity).infected_ticks, enabled ? 0u : 1u);
        EXPECT_EQ(registry.get<C::PollinationComponent>(entity).crosses, enabled ? 0u : 1u);
        EXPECT_EQ(registry.get<C::CombustibleComponent>(entity).burn_ticks_remaining,
                  enabled ? 20u : 19u);
        EXPECT_EQ(registry.get<C::GrazeableComponent>(entity).biomass > 0.8f, !enabled);
        EXPECT_EQ(registry.get<C::MortalComponent>(entity).age_ticks, enabled ? 0u : 1u);
    }
}

TEST(RegionConsumerGate, FrozenRegionDigestIncludesExactWaterDebtAndEmptyDebtKeepsLegacyBytes) {
    namespace W = Luminumbra::world;
    namespace P = Luminumbra::Persistence;
    constexpr RegionKey owner{8, 0};
    const W::RegionSchedulerConfig config{.work_limit = 1, .hold_ticks = 1, .reduced_shift = 1};
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    for (const std::uint64_t debt :
         {std::uint64_t{0}, std::uint64_t{1}, W::ActiveRegionLedger::kMaxWaterDebt}) {
        SCOPED_TRACE(debt);
        const Luminumbra::TestSupport::ScopedTempFile temporary("region_water_debt_", ".json");
        const auto directory = temporary.Path().parent_path();
        W::GameSession session;
        session.SetRootPath(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT).generic_string() + '/');
        session.SetSaveRootPath(directory);
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(true);
        session.SetAetherStateEnabled(false);
        session.SetRegionSchedulerConfig(config);
        ASSERT_TRUE(session.CreateTransientWorld("Regional water debt", "1337", "default"));
        W::WorldClock clock;
        W::ActiveRegionLedger ledger(config, clock);
        ledger.activate_owned(owner, clock);
        ledger.pin({0, 0}, true, clock);
        if (debt != 0) {
            ASSERT_TRUE(ledger.set_water_debts({{owner, debt}}));
        }
        nlohmann::json metadata{{"container_version", P::WorldSaveService::kContainerVersion}};
        clock.write_metadata(metadata);
        ASSERT_TRUE(P::WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, directory));
        ASSERT_TRUE(session.LoadWorldStateFrom(directory)) << session.GetWorldOpenError();
        session.SetReplicatedSimulationAnchors({});
        // One wake and two budget transitions suffice for this two-owner roster.
        for (int step = 0; step < 3; ++step) {
            session.RecordActiveRegionWork({{0, 0}, 2, 0, 0, 0});
            ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
            ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        }
        const auto& record = session.GetActiveRegionLedger().records().at(owner);
        ASSERT_EQ(record.state, W::RegionState::Frozen);
        P::RegionDigestInputs inputs;
        inputs.require_index = false;
        if (debt != 0) {
            inputs.field_page_bytes = "|water_debt:";
            for (unsigned byte = 0; byte < 8; ++byte) {
                inputs.field_page_bytes.push_back(static_cast<char>((debt >> (8 * byte)) & 255));
            }
        }
        EXPECT_EQ(record.frozen_digest,
                  P::WorldSaveService::region_simulation_digest(directory, owner, inputs));
        if (debt != 0) {
            inputs.field_page_bytes.clear();
            EXPECT_NE(record.frozen_digest,
                      P::WorldSaveService::region_simulation_digest(directory, owner, inputs));
        }
    }
}

TEST(RegionConsumerGate, EmptyUnknownAndInvalidOwnersAreBlockedOnlyWithInstalledGate) {
    entt::registry registry;
    const auto entity = registry.create();
    const RegionConsumerGate empty(registry, {});
    EXPECT_TRUE(RegionConsumerGate{}.Allows(entity));
    EXPECT_FALSE(empty.Allows(entity));
    registry.emplace<C::TransformComponent>(entity).position = {-0.5f, 0.0f, -0.5f};
    const std::array<RegionKey, 3> due{{{2, 1}, {-1, -1}, {2, 1}}};
    const RegionConsumerGate gate(registry, due);
    EXPECT_TRUE(gate.Allows(entity));
    EXPECT_FALSE(empty.Allows(entity));
    auto& position = registry.get<C::TransformComponent>(entity).position;
    position.x = 1024.0f;
    EXPECT_FALSE(gate.Allows(entity));
    position.x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(gate.Allows(entity));
    EXPECT_TRUE(RegionConsumerGate{}.Allows(entity));
    registry.emplace<C::PlantRegionComponent>(entity, std::uint64_t{1}, -1, -1, std::uint64_t{0});
    EXPECT_TRUE(gate.Allows(entity));
    registry.get<C::PlantRegionComponent>(entity).owner_x = 0;
    EXPECT_FALSE(gate.Allows(entity));
}

TEST(RegionConsumerGate, AdmissionWorkScalesWithDueOwnersAndQueriedParticipants) {
    entt::registry registry;
    const auto participant = Plant(registry, 1.0f);
    for (int index = 0; index < 100; ++index) {
        Plant(registry, 1024.0f);
    }
    const std::array<RegionKey, 2> due{{{0, 0}, {1, 0}}};
    const RegionConsumerGate gate(registry, due);
    EXPECT_EQ(gate.owner_visits(), 0u);
    for (int query = 0; query < 10; ++query) {
        EXPECT_TRUE(gate.Allows(participant));
    }
    EXPECT_EQ(gate.owner_visits(), 12u);
}

TEST(RegionConsumerGate, MembershipIsLazyAndReusesCallerScratch) {
    entt::registry registry;
    const auto participant = Plant(registry, 1.0f);
    const std::array<RegionKey, 3> due{{{1, 0}, {0, 0}, {-1, 2}}};
    std::vector<std::uint64_t> scratch;
    {
        const RegionConsumerGate untouched(registry, due, scratch);
        EXPECT_TRUE(scratch.empty());
        EXPECT_EQ(untouched.owner_visits(), 0u);
    }
    EXPECT_TRUE(scratch.empty());
    const RegionConsumerGate first(registry, due, scratch);
    EXPECT_TRUE(first.Allows(participant));
    ASSERT_EQ(scratch.size(), 3u);
    EXPECT_TRUE(std::is_sorted(scratch.begin(), scratch.end()));
    const std::uint64_t* const storage = scratch.data();
    const std::array<RegionKey, 1> next{{{5, 5}}};
    const RegionConsumerGate second(registry, next, scratch);
    EXPECT_FALSE(second.Allows(participant));
    EXPECT_EQ(scratch.size(), 1u);
    EXPECT_EQ(scratch.data(), storage);
}
} // namespace
