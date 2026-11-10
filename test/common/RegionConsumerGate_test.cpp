#include "../support/UniqueTempPath.h"
#include "luminumbra_common/ai/LifespanSystem.h"
#include "luminumbra_common/ai/WildlifeFoliageSystem.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CropLifecycleSystem.h"
#include "luminumbra_common/systems/FireSpreadSystem.h"
#include "luminumbra_common/systems/PlantCoarse.h"
#include "luminumbra_common/systems/PlantDiseaseSystem.h"
#include "luminumbra_common/systems/PollinationSystem.h"
#include "luminumbra_common/systems/SoilNutrientSystem.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace C = Luminumbra::Components;
namespace F = luminumbra::foliage;
using Luminumbra::world::RegionConsumerGate;
using Luminumbra::world::RegionKey;
namespace W = Luminumbra::world;
namespace P = Luminumbra::Persistence;

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
    // Seed dispersal can cross z = 0, so the neighbouring owner is due as well.
    const std::array<RegionKey, 2> due{{{0, 0}, {0, -1}}};
    const RegionConsumerGate gate(admitted, due);
    const auto legacy = StepConsumers(implicit, {}, false);
    EXPECT_EQ(StepConsumers(explicit_default, {}), legacy);
    EXPECT_EQ(StepConsumers(admitted, gate), legacy);
    EXPECT_EQ(PlantBytes(explicit_default), PlantBytes(implicit));
    EXPECT_NE(PlantBytes(admitted), PlantBytes(implicit));
    const auto before = PlantBytes(blocked);
    EXPECT_NE(StepConsumers(blocked, RegionConsumerGate(blocked, {})), legacy);
    EXPECT_EQ(PlantBytes(blocked), before);
}

TEST(RegionConsumerGate, CreatureMortalityStaysFullRateWhilePlantMortalityIsGated) {
    for (const bool starvation : {false, true}) {
        for (int owner_case = 0; owner_case < 4; ++owner_case) {
            for (int mode = 0; mode < 3; ++mode) {
                SCOPED_TRACE(::testing::Message()
                             << starvation << ':' << owner_case << ':' << mode);
                entt::registry registry;
                const auto plant = Plant(registry, 513.0f);
                registry.emplace<C::MortalComponent>(plant).lifespan_ticks = 1u;
                const auto creature = registry.create();
                registry.emplace<C::CreatureComponent>(creature).hunger = 0.5f;
                registry.emplace<C::MortalComponent>(creature).lifespan_ticks =
                    starvation ? 10u : 2u;
                registry.emplace<C::TransformComponent>(creature).position = {513.0f, 0.0f, 0.0f};
                for (const auto entity : {plant, creature}) {
                    if (owner_case == 1) {
                        registry.remove<C::TransformComponent>(entity);
                    } else if (owner_case == 2) {
                        registry.get<C::TransformComponent>(entity).position.x =
                            std::numeric_limits<float>::infinity();
                    }
                }
                std::vector<RegionKey> due;
                if (owner_case != 3) {
                    due.push_back({0, 0});
                }
                const auto gate =
                    mode == 2 ? RegionConsumerGate(registry, due) : RegionConsumerGate{};
                for (const std::uint64_t tick :
                     {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{3}}) {
                    registry.get<C::CreatureComponent>(creature).hunger =
                        starvation && tick >= 2u ? 1.0f : 0.5f;
                    const auto stats =
                        mode == 0 ? luminumbra::ai::RunLifespanOnTick(registry, tick)
                                  : luminumbra::ai::RunLifespanOnTick(registry, tick, gate);
                    const auto& mortal = registry.get<C::MortalComponent>(creature);
                    EXPECT_EQ(mortal.age_ticks, tick == 1u ? 1u : 2u);
                    EXPECT_EQ(mortal.dead, tick >= 2u ? 1u : 0u);
                    EXPECT_EQ(registry.get<C::CreatureComponent>(creature).eaten, tick >= 2u);
                    EXPECT_EQ(registry.get<C::MortalComponent>(plant).age_ticks,
                              mode == 2 ? 0u : 1u);
                    EXPECT_EQ(registry.get<C::MortalComponent>(plant).dead, mode == 2 ? 0u : 1u);
                    EXPECT_EQ(stats.aged, tick == 1u ? (mode == 2 ? 1 : 2) : (tick == 2u ? 1 : 0));
                    EXPECT_EQ(stats.died, tick == 1u ? (mode == 2 ? 0 : 1) : (tick == 2u ? 1 : 0));
                }
                EXPECT_EQ(gate.owner_visits(),
                          mode == 2 && (owner_case == 0 || owner_case == 3) ? 3u + due.size() : 0u);
            }
        }
    }
}

TEST(RegionConsumerGate, SoilConsumptionAndAbsorbedFollowPlantOwnerAdmission) {
    for (int mode = 0; mode < 8; ++mode) {
        SCOPED_TRACE(mode);
        entt::registry registry;
        F::SoilGrid soil(2, 1);
        const auto plant = Plant(registry, 0.5f);
        registry.emplace<C::SoilFeederComponent>(plant).absorbed = 7u;
        // Persistent ownership wins over the position inside the soil field.
        registry.emplace<C::PlantRegionComponent>(plant, std::uint64_t{1}, 1, 0, std::uint64_t{0});
        if (mode >= 5) {
            registry.remove<C::PlantRegionComponent>(plant);
        }
        if (mode == 5) {
            registry.get<C::TransformComponent>(plant).position.x =
                std::numeric_limits<float>::infinity();
        } else if (mode == 6) {
            registry.remove<C::TransformComponent>(plant);
        }
        std::vector<RegionKey> due;
        if (mode != 4) {
            due.push_back({mode == 2 ? 1 : 0, 0});
        }
        const auto gate = mode >= 2 ? RegionConsumerGate(registry, due) : RegionConsumerGate{};
        for (int step = 0; step < 2; ++step) {
            const auto stats =
                mode == 0 ? F::RunSoilNutrientOnTick(registry, soil, 0.0f, 0.0f, 1.0f)
                          : F::RunSoilNutrientOnTick(registry, soil, 0.0f, 0.0f, 1.0f, gate);
            const bool admitted = mode <= 2 || mode == 7;
            const auto steps = static_cast<std::uint32_t>(step + 1);
            EXPECT_EQ(stats.participants, admitted ? 1 : 0);
            EXPECT_EQ(stats.consumed, admitted ? 750 : 0);
            EXPECT_EQ(registry.get<C::SoilFeederComponent>(plant).absorbed,
                      7u + (admitted ? 750u * steps : 0u));
            EXPECT_EQ(soil.AtCell(0, 0),
                      F::kSoilBaseline -
                          (admitted ? (750 - F::kSoilRegenPerTick) * (step + 1) : 0));
            EXPECT_EQ(soil.AtCell(1, 0), F::kSoilBaseline);
            EXPECT_EQ(gate.owner_visits(),
                      mode >= 2 && mode != 5 && mode != 6 ? steps + due.size() : 0u);
        }
    }
}

TEST(RegionConsumerGate, DeniedSoilDemandCannotStealFromAdmittedPlantAndFieldStillRegenerates) {
    entt::registry registry;
    F::SoilGrid soil(2, 1, 100);
    const auto denied = Plant(registry, 0.5f);
    registry.emplace<C::SoilFeederComponent>(denied);
    registry.emplace<C::PlantRegionComponent>(denied, std::uint64_t{1}, 1, 0, std::uint64_t{0});
    const auto admitted = Plant(registry, 0.5f);
    registry.emplace<C::SoilFeederComponent>(admitted);
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(registry, due);
    const auto stats = F::RunSoilNutrientOnTick(registry, soil, 0.0f, 0.0f, 1.0f, gate);
    EXPECT_EQ(stats.participants, 1);
    EXPECT_EQ(stats.consumed, 100);
    EXPECT_EQ(registry.get<C::SoilFeederComponent>(denied).absorbed, 0u);
    EXPECT_EQ(registry.get<C::SoilFeederComponent>(admitted).absorbed, 100u);
    EXPECT_EQ(soil.AtCell(0, 0), F::kSoilRegenPerTick);
    EXPECT_EQ(soil.AtCell(1, 0), 100 + F::kSoilRegenPerTick);
    EXPECT_EQ(gate.owner_visits(), 3u);
}

TEST(RegionConsumerGate, SessionSoilConsumptionUsesSealedPlantOwners) {
    for (const bool enabled : {false, true}) {
        for (const int owner : {0, 1}) {
            SCOPED_TRACE(::testing::Message() << enabled << ':' << owner);
            W::GameSession session;
            session.SetActiveRegionsEnabled(enabled);
            session.SetReplicatedSimulationAnchors({});
            session.PinActiveRegion({0, 0}, true);
            auto& registry = session.GetRegistry();
            const auto plant = Plant(registry, 0.0f);
            registry.emplace<C::PlantRegionComponent>(
                plant, std::uint64_t{1}, owner, 0, std::uint64_t{0});
            registry.emplace<C::SoilFeederComponent>(plant).absorbed = 7u;
            ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
            ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
            EXPECT_EQ(registry.get<C::SoilFeederComponent>(plant).absorbed,
                      enabled && owner == 1 ? 7u : 757u);
        }
    }
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
    EXPECT_FALSE(RegionConsumerGate{}.UsesPersistentPlantIdentity());
    EXPECT_TRUE(empty.UsesPersistentPlantIdentity());
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

void Identify(entt::registry& registry, entt::entity entity, std::uint64_t id) {
    const auto owner =
        W::ActiveRegionLedger::region_at(registry.get<C::TransformComponent>(entity).position);
    registry.emplace<C::PlantRegionComponent>(entity, id, owner.x, owner.z, std::uint64_t{0});
}

void Ripen(entt::registry& registry, entt::entity entity, bool perennial) {
    auto& crop = registry.emplace<C::CropLifecycleComponent>(entity);
    crop.perennial = perennial;
    crop.lifespan_ticks = 2;
    crop.ripe_ticks = 1;
}

bool OpenLedger(W::GameSession& session,
                Luminumbra::JobSystem& jobs,
                const std::filesystem::path& directory,
                W::ActiveRegionLedger& ledger) {
    session.SetRootPath(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT).generic_string() + '/');
    session.SetSaveRootPath(directory);
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetAetherStateEnabled(false);
    if (!session.CreateTransientWorld("Plant identity", "1337", "default")) {
        return false;
    }
    std::filesystem::create_directories(directory);
    nlohmann::json metadata{{"container_version", P::WorldSaveService::kContainerVersion}};
    W::WorldClock{}.write_metadata(metadata);
    return P::WorldSaveService::save_metadata_and_active_regions(
               metadata.dump(), ledger, directory) &&
           session.LoadWorldStateFrom(directory);
}

TEST(RegionConsumerGate, IdentifiedAnnualAndPerennialReseedPassPersistentSnapshotAdmission) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    for (const bool perennial : {false, true}) {
        SCOPED_TRACE(perennial);
        const Luminumbra::TestSupport::ScopedTempFile temporary("plant_identity_", ".json");
        const auto directory = temporary.Path().parent_path();
        W::ActiveRegionLedger ledger;
        ledger.pin({0, 0}, true, W::WorldClock{});
        std::uint64_t parent_id = 0, reserved_id = 0;
        ASSERT_TRUE(ledger.allocate_persistent_id(parent_id));
        ASSERT_TRUE(ledger.allocate_persistent_id(reserved_id));
        W::GameSession session;
        ASSERT_TRUE(OpenLedger(session, jobs, directory / "input", ledger));
        auto& registry = session.GetRegistry();
        const auto parent = Plant(registry, 100.0f);
        registry.get<C::TransformComponent>(parent).position.z = 100.0f;
        Identify(registry, parent, parent_id);
        Ripen(registry, parent, perennial);
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        const auto snapshot = F::BuildPlantEntitySnapshot(registry, true);
        std::string error;
        ASSERT_TRUE(
            F::ValidatePlantEntitySnapshot(snapshot, session.GetActiveRegionLedger(), {}, error))
            << error;
        EXPECT_EQ(snapshot.entities.size(), perennial ? 2u : 1u);
        bool found_child = false;
        for (const auto entity : registry.view<C::PlantRegionComponent>()) {
            const auto& identity = registry.get<C::PlantRegionComponent>(entity);
            const auto& growth = registry.get<C::PlantGrowthComponent>(entity);
            EXPECT_LE(growth.planted_tick, identity.region_stamp);
            EXPECT_LE(identity.region_stamp, growth.last_tick);
            if (identity.persistent_id != parent_id) {
                found_child = true;
                EXPECT_EQ(identity.persistent_id, reserved_id + 1u);
                EXPECT_EQ(identity.region_stamp, 1u);
                const auto owner = W::ActiveRegionLedger::region_at(
                    registry.get<C::TransformComponent>(entity).position);
                EXPECT_EQ(identity.owner_x, owner.x);
                EXPECT_EQ(identity.owner_z, owner.z);
            }
        }
        EXPECT_TRUE(found_child);
        EXPECT_TRUE(F::HasPersistentPlantIdentity(registry));
    }
}

TEST(RegionConsumerGate, PersistedRosterIgnoresHandleHistoryForCrossesAndBirths) {
    entt::registry source;
    for (const auto id : {std::uint64_t{30}, std::uint64_t{20}, std::uint64_t{10}}) {
        const auto entity = Plant(source, id == 30 ? 100.0f : (id == 10 ? 99.0f : 101.0f));
        source.get<C::TransformComponent>(entity).position.z = 100.0f;
        source.get<C::PlantGenomeComponent>(entity).genes.fill(id == 10 ? 0.2f : 0.8f);
        source.get<C::PlantGrowthComponent>(entity).growth_points = static_cast<std::uint32_t>(id);
        Identify(source, entity, id);
        Ripen(source, entity, true);
    }
    auto snapshot = F::BuildPlantEntitySnapshot(source, true);
    W::GameSession first, second;
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(first.GetRegistry(), snapshot));
    auto& other = second.GetRegistry();
    for (std::uint32_t index = 0; index < 7u; ++index) {
        (void)other.create();
    }
    const auto filler = other.create();
    other.destroy(filler);
    std::reverse(snapshot.entities.begin(), snapshot.entities.end());
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(other, snapshot));
    EXPECT_EQ(first.ComputePlantSubHash(), second.ComputePlantSubHash());
    for (auto* registry : {&first.GetRegistry(), &other}) {
        const std::array<RegionKey, 1> due{{{0, 0}}};
        EXPECT_EQ(F::RunPollinationOnTick(*registry,
                                          std::uint64_t{1},
                                          {},
                                          std::uint64_t{0},
                                          0.05f,
                                          RegionConsumerGate(*registry, due))
                      .crossed,
                  3);
        for (const auto entity : registry->view<C::PlantRegionComponent>()) {
            if (registry->get<C::PlantRegionComponent>(entity).persistent_id == 30u) {
                C::PlantGenomeComponent donor;
                donor.genes.fill(0.2f);
                EXPECT_EQ(registry->get<C::PollinationComponent>(entity).next_genome.genes,
                          F::PollinateCross(
                              registry->get<C::PlantGenomeComponent>(entity), donor, 30, 10, 1)
                              .genes);
            }
        }
    }
    EXPECT_EQ(PlantBytes(first.GetRegistry()), PlantBytes(other));
    for (auto* registry : {&first.GetRegistry(), &other}) {
        W::ActiveRegionLedger ledger;
        std::uint64_t id = 0;
        for (std::uint64_t reserved = 0; reserved < 30; ++reserved) {
            ASSERT_TRUE(ledger.allocate_persistent_id(id));
        }
        const auto allocate = [&ledger](std::uint64_t& child_id) {
            return ledger.allocate_persistent_id(child_id);
        };
        const std::array<RegionKey, 1> due{{{0, 0}}};
        EXPECT_EQ(F::RunCropLifecycleOnTick(*registry,
                                            std::uint64_t{1},
                                            std::uint64_t{0},
                                            RegionConsumerGate(*registry, due),
                                            allocate)
                      .reseeded,
                  3);
        EXPECT_TRUE(F::ValidatePlantEntitySnapshot(F::BuildPlantEntitySnapshot(*registry, true)));
    }
    EXPECT_EQ(PlantBytes(first.GetRegistry()), PlantBytes(other));
    EXPECT_EQ(first.ComputePlantSubHash(), second.ComputePlantSubHash());
}

TEST(RegionConsumerGate, BlockedBirthDestinationPreservesParentUntilDue) {
    for (const bool perennial : {false, true}) {
        entt::registry registry;
        const auto parent = Plant(registry, 511.5f);
        registry.get<C::TransformComponent>(parent).position.z = 100.0f;
        Identify(registry, parent, 1);
        Ripen(registry, parent, perennial);
        // Pick a deterministic outward dispersal, independent of registry handles.
        std::uint64_t tick = 1;
        for (; tick < 100; ++tick) {
            auto rng =
                luminumbra::core::DeterministicRng::seeded(F::kGerminationSeedOffset, 1, tick);
            const float angle = rng.next_unit() * 6.2831853f;
            const float radius = 1.0f + rng.next_unit() * 2.0f;
            if (radius * Luminumbra::DeterministicMath::Cos(angle) > 0.5f) {
                break;
            }
        }
        ASSERT_LT(tick, 100u);
        const std::array<RegionKey, 1> due{{{0, 0}}};
        const RegionConsumerGate gate(registry, due);
        std::uint32_t allocations = 0;
        const auto allocate = [&allocations](std::uint64_t& id) {
            ++allocations;
            id = 2;
            return true;
        };
        const auto before = PlantBytes(registry);
        const auto stats = F::RunCropLifecycleOnTick(registry, tick, 0, gate, allocate);
        EXPECT_EQ(PlantBytes(registry), before);
        EXPECT_EQ(allocations, 0u);
        EXPECT_EQ(gate.owner_visits(), 3u);
        EXPECT_EQ(stats.ripening, 0);
        EXPECT_EQ(stats.reseeded, 0);
        EXPECT_EQ(stats.reset, 0);
        EXPECT_EQ(stats.died, 0);
        const std::array<RegionKey, 2> both{{{0, 0}, {1, 0}}};
        EXPECT_EQ(F::RunCropLifecycleOnTick(
                      registry, tick + 1u, 0, RegionConsumerGate(registry, both), allocate)
                      .reseeded,
                  1);
        EXPECT_EQ(allocations, 1u);
    }
}

TEST(RegionConsumerGate, FailedIdentityAllocationLeavesEveryParentUntouched) {
    for (const bool perennial : {false, true}) {
        entt::registry registry;
        const auto parent = Plant(registry, 100.0f);
        Identify(registry, parent, 1);
        Ripen(registry, parent, perennial);
        const auto before = PlantBytes(registry);
        const auto refuse = [](std::uint64_t&) {
            return false;
        };
        const auto stats = F::RunCropLifecycleOnTick(registry, 1, 0, {}, refuse);
        EXPECT_EQ(PlantBytes(registry), before);
        EXPECT_EQ(stats.ripening, 0);
        EXPECT_EQ(stats.reseeded, 0);
        EXPECT_EQ(stats.reset, 0);
        EXPECT_EQ(stats.died, 0);
        const auto full = [](std::uint64_t&) -> bool {
            throw std::length_error("Active-region ledger file capacity reached");
        };
        EXPECT_THROW(F::RunCropLifecycleOnTick(registry, 1, 0, {}, full), std::length_error);
        EXPECT_EQ(PlantBytes(registry), before);
    }
}

TEST(RegionConsumerGate, SessionLatchesExhaustedSharedAllocatorWithoutReseeding) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    for (const bool perennial : {false, true}) {
        const Luminumbra::TestSupport::ScopedTempFile temporary("plant_exhaustion_", ".json");
        W::ActiveRegionLedger ledger;
        ledger.pin({0, 0}, true, W::WorldClock{});
        std::uint64_t id = 0;
        ASSERT_TRUE(ledger.allocate_persistent_id(id));
        auto encoded = ledger.encode();
        ASSERT_EQ(encoded.size(),
                  W::ActiveRegionLedger::kHeaderBytes + W::ActiveRegionLedger::kRecordBytes + 4u +
                      14u + 8u);
        // The sole extension is the allocator TLV; its u64 payload precedes the checksum.
        for (unsigned byte = 0; byte < 8; ++byte) {
            encoded[encoded.size() - 16u + byte] = static_cast<char>(
                (W::ActiveRegionLedger::kPersistentIdLimit >> (8u * byte)) & 255u);
        }
        std::uint64_t checksum = 14695981039346656037ull;
        for (std::size_t index = 0; index < encoded.size() - 8u; ++index) {
            checksum ^= static_cast<unsigned char>(encoded[index]);
            checksum *= 1099511628211ull;
        }
        for (unsigned byte = 0; byte < 8; ++byte) {
            encoded[encoded.size() - 8u + byte] =
                static_cast<char>((checksum >> (8u * byte)) & 255u);
        }
        std::string error;
        ASSERT_TRUE(W::ActiveRegionLedger::decode(encoded, ledger, error)) << error;
        W::GameSession session;
        ASSERT_TRUE(OpenLedger(session, jobs, temporary.Path().parent_path(), ledger));
        auto& registry = session.GetRegistry();
        const auto parent = Plant(registry, 100.0f);
        registry.get<C::TransformComponent>(parent).position.z = 100.0f;
        Identify(registry, parent, id);
        Ripen(registry, parent, perennial);
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        const auto& fault = session.GetRegionSchedulerFault();
        if (fault.has_value()) {
            EXPECT_EQ(fault->message, "persistent id space exhausted");
        } else {
            FAIL() << "exhausted allocator did not latch a region fault";
        }
        ASSERT_TRUE(registry.valid(parent));
        EXPECT_EQ(registry.view<C::PlantTag>().size(), 1u);
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).ripe_ticks, 1u);
        EXPECT_EQ(registry.get<C::CropLifecycleComponent>(parent).generations, 0u);
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(parent).stage,
                  static_cast<std::uint8_t>(C::PlantStage::Fruiting));
        EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(),
                  W::ActiveRegionLedger::kPersistentIdLimit);
    }
}

TEST(RegionConsumerGate, GrowthStepsKeepPersistentStampCurrent) {
    entt::registry registry;
    const auto entity = Plant(registry, 100.0f);
    Identify(registry, entity, std::uint64_t{1});
    F::RunPlantGrowthSystemOnTick(
        registry, std::uint64_t{5}, [](const auto&) { return F::PlantEnvSample{}; });
    auto& identity = registry.get<C::PlantRegionComponent>(entity);
    EXPECT_EQ(identity.region_stamp, 5u);
    auto& growth = registry.get<C::PlantGrowthComponent>(entity);
    const auto& genome = registry.get<C::PlantGenomeComponent>(entity);
    F::AdvancePlantGrowthCoarse(growth, genome, {}, 0, 3, std::uint64_t{8}, &identity);
    EXPECT_EQ(identity.region_stamp, 8u);
    F::AdvancePlantGrowthCoarse(growth, genome, {}, 0, 0, std::uint64_t{9}, &identity);
    EXPECT_EQ(identity.region_stamp, 8u);
    EXPECT_EQ(growth.last_tick, 8u);
    const auto before = PlantBytes(registry);
    EXPECT_THROW(
        F::AdvancePlantGrowthCoarse(growth, genome, {}, 0, 36001, std::uint64_t{9}, &identity),
        std::invalid_argument);
    EXPECT_THROW(
        F::AdvancePlantGrowthCoarse(growth, genome, {}, 1001, 1, std::uint64_t{9}, &identity),
        std::invalid_argument);
    EXPECT_EQ(PlantBytes(registry), before);
    EXPECT_EQ(identity.region_stamp, 8u);
    EXPECT_EQ(growth.last_tick, 8u);
}

TEST(RegionConsumerGate, GrowthAdmissionPreservesDeniedStateAndSkipsSampling) {
    for (const bool persistent : {false, true}) {
        for (const bool empty_due : {false, true}) {
            entt::registry registry;
            const auto admitted = Plant(registry, 100.0f);
            const auto denied = Plant(registry, 513.0f);
            registry.emplace<C::PlantTag>(registry.create());
            for (int index = 0; index < 10; ++index) {
                (void)registry.create();
            }
            if (persistent) {
                Identify(registry, admitted, std::uint64_t{1});
                Identify(registry, denied, std::uint64_t{2});
                // Admission follows the saved owner even when the transform disagrees.
                registry.get<C::TransformComponent>(denied).position.x = 100.0f;
            }
            registry.get<C::PlantGrowthComponent>(denied).tended = 20;
            const auto before = PlantBytes(registry);
            std::uint32_t samples = 0;
            const auto sample = [&samples](const auto&) {
                ++samples;
                return F::PlantEnvSample{};
            };
            std::vector<RegionKey> due;
            if (!empty_due) {
                due.push_back({0, 0});
            }
            const RegionConsumerGate gate(registry, due);
            EXPECT_EQ(
                F::RunPlantGrowthSystemOnTick(registry, std::uint64_t{5}, sample, gate).updated,
                empty_due ? 0 : 1);
            EXPECT_EQ(samples, empty_due ? 0u : 1u);
            EXPECT_EQ(gate.owner_visits(), due.size() + 2u);
            const auto& growth = registry.get<C::PlantGrowthComponent>(denied);
            EXPECT_EQ(growth.last_tick, 0u);
            EXPECT_EQ(growth.growth_points, 0u);
            EXPECT_EQ(growth.stress_points, 0u);
            EXPECT_EQ(growth.tended, 20u);
            if (persistent) {
                EXPECT_EQ(registry.get<C::PlantRegionComponent>(denied).region_stamp, 0u);
            }
            if (empty_due) {
                EXPECT_EQ(PlantBytes(registry), before);
            }
            EXPECT_EQ(F::RunPlantGrowthSystemOnTick(registry, std::uint64_t{6}, sample).updated, 2);
        }
    }
}

TEST(RegionConsumerGate, EmptyAndInvalidGrowthParticipantsDoNotSampleEnvironment) {
    entt::registry registry;
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(registry, due);
    std::uint32_t samples = 0;
    const auto sample = [&samples](const auto&) {
        ++samples;
        return F::PlantEnvSample{};
    };
    EXPECT_EQ(F::RunPlantGrowthSystemOnTick(registry, std::uint64_t{1}, sample, gate).updated, 0);
    EXPECT_EQ(gate.owner_visits(), 0u);
    const auto invalid = Plant(registry, std::numeric_limits<float>::infinity());
    EXPECT_EQ(F::RunPlantGrowthSystemOnTick(registry, std::uint64_t{2}, sample, gate).updated, 0);
    EXPECT_EQ(samples, 0u);
    EXPECT_EQ(gate.owner_visits(), 0u);
    EXPECT_EQ(registry.get<C::PlantGrowthComponent>(invalid).last_tick, 0u);
}

TEST(RegionConsumerGate, SessionGrowthUsesSealedOwnersAndDefaultGateKeepsFullRate) {
    for (const bool enabled : {false, true}) {
        W::GameSession session;
        session.SetActiveRegionsEnabled(enabled);
        session.SetReplicatedSimulationAnchors({});
        session.PinActiveRegion({0, 0}, true);
        auto& registry = session.GetRegistry();
        const auto plant = Plant(registry, 513.0f);
        Identify(registry, plant, std::uint64_t{1});
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        const auto expected_tick = enabled ? std::uint64_t{0} : std::uint64_t{1};
        EXPECT_EQ(registry.get<C::PlantGrowthComponent>(plant).last_tick, expected_tick);
        EXPECT_EQ(registry.get<C::PlantRegionComponent>(plant).region_stamp, expected_tick);
    }
}

TEST(RegionConsumerGate, InactiveGatePinsRawPollinationIdsAndHandleOrder) {
    entt::registry registry;
    const auto receiver = Plant(registry, 100.0f);
    const auto donor = Plant(registry, 101.0f);
    registry.get<C::PlantGenomeComponent>(receiver).genes.fill(0.2f);
    registry.get<C::PlantGenomeComponent>(donor).genes.fill(0.8f);
    EXPECT_EQ(F::CollectPollinationParticipants(registry, {}),
              (std::vector<entt::entity>{receiver, donor}));
    const auto expected = F::PollinateCross(registry.get<C::PlantGenomeComponent>(receiver),
                                            registry.get<C::PlantGenomeComponent>(donor),
                                            static_cast<std::uint64_t>(entt::to_integral(receiver)),
                                            static_cast<std::uint64_t>(entt::to_integral(donor)),
                                            std::uint64_t{1});
    ASSERT_EQ(F::RunPollinationOnTick(registry, std::uint64_t{1}).crossed, 2);
    EXPECT_EQ(registry.get<C::PollinationComponent>(receiver).next_genome.genes, expected.genes);
    EXPECT_EQ(registry.get<C::PollinationComponent>(donor).next_genome.genes, expected.genes);
}

TEST(RegionConsumerGate, InactiveGatePinsRawGerminationAcrossGenerations) {
    entt::registry registry;
    auto parent = Plant(registry, 100.0f);
    Ripen(registry, parent, false);
    for (std::uint64_t tick = std::uint64_t{1}; tick <= std::uint64_t{3}; ++tick) {
        const auto position = registry.get<C::TransformComponent>(parent).position;
        const auto genome = registry.get<C::PlantGenomeComponent>(parent);
        auto rng = luminumbra::core::DeterministicRng::seeded(
            F::kGerminationSeedOffset, static_cast<std::uint64_t>(entt::to_integral(parent)), tick);
        const float angle = rng.next_unit() * 6.2831853f;
        const float radius = 1.0f + rng.next_unit() * 2.0f;
        auto& growth = registry.get<C::PlantGrowthComponent>(parent);
        growth.stage = static_cast<std::uint8_t>(C::PlantStage::Fruiting);
        registry.get<C::CropLifecycleComponent>(parent).ripe_ticks = 1;
        ASSERT_EQ(F::RunCropLifecycleOnTick(registry, tick, std::uint64_t{0}, {}).reseeded, 1);
        ASSERT_FALSE(registry.valid(parent));
        const auto children = F::CollectPollinationParticipants(registry, {});
        ASSERT_EQ(children.size(), 1u);
        parent = children.front();
        const auto& child = registry.get<C::TransformComponent>(parent).position;
        EXPECT_EQ(child.x, position.x + radius * Luminumbra::DeterministicMath::Cos(angle));
        EXPECT_EQ(child.z, position.z + radius * Luminumbra::DeterministicMath::Sin(angle));
        EXPECT_EQ(registry.get<C::PlantGenomeComponent>(parent).genes, genome.genes);
    }
}

TEST(RegionConsumerGate, MixedPlantIdsHaveDisjointRngDomainsAndSymmetricCrosses) {
    entt::registry registry;
    const auto persistent = Plant(registry, 100.0f);
    const auto legacy = Plant(registry, 101.0f);
    const auto handle = static_cast<std::uint64_t>(entt::to_integral(legacy));
    ASSERT_NE(handle, 0u);
    Identify(registry, persistent, handle);
    constexpr std::uint64_t legacy_tag = std::uint64_t{1} << 63;
    EXPECT_EQ(W::ActiveRegionLedger::kPersistentIdLimit, legacy_tag);
    EXPECT_EQ(F::PlantSimulationId(registry, persistent), handle);
    EXPECT_EQ(F::PlantSimulationId(registry, legacy), legacy_tag | handle);
    EXPECT_TRUE(F::PlantIdentityLess(registry, persistent, legacy));
    const std::array<RegionKey, 1> due{{{0, 0}}};
    const RegionConsumerGate gate(registry, due);
    EXPECT_EQ(F::CollectPollinationParticipants(registry, gate),
              (std::vector<entt::entity>{persistent, legacy}));
    registry.get<C::PlantGenomeComponent>(persistent).genes.fill(0.2f);
    registry.get<C::PlantGenomeComponent>(legacy).genes.fill(0.8f);
    const auto expected = F::PollinateCross(registry.get<C::PlantGenomeComponent>(persistent),
                                            registry.get<C::PlantGenomeComponent>(legacy),
                                            handle,
                                            legacy_tag | handle,
                                            std::uint64_t{1});
    ASSERT_EQ(F::RunPollinationOnTick(registry, std::uint64_t{1}, {}, std::uint64_t{0}, 0.05f, gate)
                  .crossed,
              2);
    EXPECT_EQ(registry.get<C::PollinationComponent>(persistent).next_genome.genes, expected.genes);
    EXPECT_EQ(registry.get<C::PollinationComponent>(legacy).next_genome.genes, expected.genes);
    registry.get<C::PlantRegionComponent>(persistent).persistent_id = legacy_tag - 1u;
    EXPECT_LT(F::PlantSimulationId(registry, persistent), F::PlantSimulationId(registry, legacy));
}

TEST(RegionConsumerGate, PollinationReservesParticipantBoundEvenWhenAdmissionIsSparse) {
    entt::registry registry;
    EXPECT_TRUE(F::CollectPollinationParticipants(registry, {}).empty());
    for (int index = 0; index < 8; ++index) {
        Plant(registry, 100.0f);
    }
    const auto blocked =
        F::CollectPollinationParticipants(registry, RegionConsumerGate(registry, {}));
    EXPECT_TRUE(blocked.empty());
    EXPECT_GE(blocked.capacity(), 8u);
    const auto admitted = F::CollectPollinationParticipants(registry, {});
    EXPECT_EQ(admitted.size(), 8u);
    EXPECT_GE(admitted.capacity(), 8u);
}

TEST(RegionConsumerGate, LegacyReseedKeepsHandleSeedAndNeverUsesIdentityAllocator) {
    entt::registry legacy, supplied;
    for (auto* registry : {&legacy, &supplied}) {
        const auto parent = Plant(*registry, 100.0f);
        Ripen(*registry, parent, true);
    }
    std::uint32_t allocations = 0;
    const auto allocate = [&allocations](std::uint64_t&) {
        ++allocations;
        return false;
    };
    EXPECT_EQ(F::RunCropLifecycleOnTick(legacy, std::uint64_t{1}).reseeded, 1);
    EXPECT_EQ(F::RunCropLifecycleOnTick(supplied, std::uint64_t{1}, std::uint64_t{0}, {}, allocate)
                  .reseeded,
              1);
    EXPECT_EQ(allocations, 0u);
    const auto bytes = [](const entt::registry& registry) {
        return Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
            F::BuildPlantEntitySnapshot(registry));
    };
    EXPECT_EQ(bytes(legacy), bytes(supplied));
    auto rng = luminumbra::core::DeterministicRng::seeded(
        F::kGerminationSeedOffset,
        static_cast<std::uint64_t>(entt::to_integral(entt::entity{0})),
        std::uint64_t{1});
    const float angle = rng.next_unit() * 6.2831853f;
    const float radius = 1.0f + rng.next_unit() * 2.0f;
    for (const auto entity : legacy.view<C::PlantGrowthComponent>()) {
        EXPECT_FALSE(legacy.all_of<C::PlantRegionComponent>(entity));
        if (legacy.get<C::PlantGrowthComponent>(entity).planted_tick == 1u) {
            const auto& position = legacy.get<C::TransformComponent>(entity).position;
            EXPECT_EQ(position.x, 100.0f + radius * Luminumbra::DeterministicMath::Cos(angle));
            EXPECT_EQ(position.z, radius * Luminumbra::DeterministicMath::Sin(angle));
        }
    }
}
TEST(RegionConsumerGate, IdentityOrderingIsStrictForMixedZeroAndDuplicateIds) {
    entt::registry registry;
    std::array<entt::entity, 5> entities{};
    for (auto& entity : entities) {
        entity = Plant(registry, 100.0f);
    }
    Identify(registry, entities[1], std::uint64_t{8});
    Identify(registry, entities[2], std::uint64_t{0});
    Identify(registry, entities[3], std::uint64_t{3});
    Identify(registry, entities[4], std::uint64_t{3});
    EXPECT_EQ(F::CollectPollinationParticipants(registry, {}),
              (std::vector<entt::entity>(entities.begin(), entities.end())));
    const std::array expected{entities[3], entities[4], entities[1], entities[0], entities[2]};
    std::sort(entities.begin(), entities.end(), [&registry](auto a, auto b) {
        return F::PlantIdentityLess(registry, a, b);
    });
    EXPECT_EQ(entities, expected);
    for (std::size_t a = 0; a < entities.size(); ++a) {
        for (std::size_t b = 0; b < entities.size(); ++b) {
            EXPECT_EQ(F::PlantIdentityLess(registry, entities[a], entities[b]), a < b);
        }
    }
    EXPECT_EQ(F::PlantSimulationId(registry, entities[0]), 3u);
    constexpr std::uint64_t legacy_tag = std::uint64_t{1} << 63;
    EXPECT_EQ(F::PlantSimulationId(registry, entities[3]),
              legacy_tag | static_cast<std::uint64_t>(entt::to_integral(entities[3])));
    EXPECT_EQ(F::PlantSimulationId(registry, entities[4]),
              legacy_tag | static_cast<std::uint64_t>(entt::to_integral(entities[4])));
}
} // namespace
