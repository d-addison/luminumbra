#include "../support/ActiveRegionAcceptanceWorld.h"
#include "luminumbra_common/ai/CircadianSystem.h"
#include "luminumbra_common/ai/DecompositionSystem.h"
#include "luminumbra_common/ai/HerdAlarmSystem.h"
#include "luminumbra_common/ai/LifespanSystem.h"
#include "luminumbra_common/ai/MigrationSystem.h"
#include "luminumbra_common/ai/PredatorPackSystem.h"
#include "luminumbra_common/ai/TerritorySystem.h"
#include "luminumbra_common/ai/WildlifeFoliageSystem.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/systems/CropLifecycleSystem.h"
#include "luminumbra_common/systems/FireSpreadSystem.h"
#include "luminumbra_common/systems/FoliageRegistryStage.h"
#include "luminumbra_common/systems/PlantCoarse.h"
#include "luminumbra_common/systems/PlantDiseaseSystem.h"
#include "luminumbra_common/systems/PollinationSystem.h"
#include "luminumbra_common/systems/RegionalFoliage.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
namespace C = Luminumbra::Components;
namespace W = Luminumbra::world;
namespace F = luminumbra::foliage;
namespace AI = luminumbra::ai;

entt::entity Plant(entt::registry& registry, float x) {
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity).position = {x, 0, 8};
    registry.emplace<C::PlantTag>(entity);
    registry.emplace<C::PlantGenomeComponent>(entity).genes.fill(0.5f);
    registry.emplace<C::PlantGrowthComponent>(entity);
    registry.emplace<C::PollinationTag>(entity);
    registry.emplace<C::PollinationComponent>(entity);
    registry.emplace<C::PlantHealthComponent>(entity);
    registry.emplace<C::CropLifecycleComponent>(entity);
    registry.emplace<C::MortalComponent>(entity).lifespan_ticks = 100000;
    return entity;
}

std::string Plants(const entt::registry& registry) {
    return Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        F::BuildPlantEntitySnapshot(registry, true));
}

// Execute the approved system order against one candidate and one immutable window view.
// Constant environmental inputs make the independent integer oracle applicable.
void AdvanceCandidate(F::FoliageRegistryStage& stage,
                      const std::map<W::RegionKey, std::uint32_t>& windows,
                      std::uint64_t tick) {
    auto& registry = stage.registry();
    std::vector<W::RegionKey> due;
    for (const auto& [owner, elapsed] : windows) {
        if (elapsed != 0)
            due.push_back(owner);
    }
    const W::RegionConsumerGate gate(registry, due, W::RegionElapsedTicks(windows), tick);
    AI::CreatureTickAdmission admission;
    admission.Capture(registry, gate);
    std::vector<entt::entity> growth;
    const auto environment = [](const C::TransformComponent&) {
        return F::PlantEnvSample{0.5f, 0.5f, 0.5f, 0.5f};
    };
    F::RunRegionalPlantGrowthOnTick(registry, tick, environment, gate, growth);
    F::RunPollinationRegional(registry, tick, gate);
    F::RunPlantDiseaseRegional(registry, tick, gate);
    const auto allocate = [&stage](std::uint64_t& id) {
        return stage.AllocatePersistentId(id);
    };
    F::RunCropLifecycleRegional(registry, tick, gate, 0, allocate);
    luminumbra::sim::RunFireSpreadOnTick<false, true>(registry, tick, {}, {}, gate);
    AI::RunWildlifeFoliageOnTick<true>(registry, tick, {}, gate, &admission);
    AI::RunLifespanOnTick<true>(registry, tick, gate, &admission);
    AI::RunHerdAlarmOnTick(registry, 1.0f / 30.0f, &admission, true);
    AI::RunDecompositionOnTick(registry, tick, &admission, gate, nullptr, true);
    AI::RunCircadianOnTick(registry, 0.5f, 1.0f, &admission, &gate);
    AI::RunTerritoryOnTick(registry, tick, &admission, &gate);
    AI::RunPredatorPackOnTick(registry, tick, &admission, &gate);
    AI::RunMigrationOnTick(registry, 0.25f, &admission, &gate);
}

TEST(RegionalCadenceIntegration, IntegerPathsMatchIndependentFullRateOracle) {
    // Exercise actual session dispatch, including creature capture before movement.
    // Alternate pressure after demotion so neither hysteresis counter can reach two.
    for (const bool elapsed : {false, true}) {
        W::GameSession session({.elapsed_consumers = elapsed},
                               W::GameSession::AuthorityProfileTestOnly{});
        session.SetReplicatedSimulationAnchors({});
        session.SetRegionSchedulerConfig(
            {.work_limit = 1000000, .hold_ticks = 2, .reduced_shift = 1});
        session.PinActiveRegion({0, 0}, true);
        auto& registry = session.GetRegistry();
        const auto mortal = registry.create();
        registry.emplace<C::TransformComponent>(mortal).position = {8, 0, 8};
        registry.emplace<C::MortalComponent>(mortal).lifespan_ticks = 100000;
        registry.emplace<C::CreatureComponent>(mortal).eaten = true;
        const auto corpse = registry.create();
        registry.emplace<C::TransformComponent>(corpse).position = {9, 0, 8};
        registry.emplace<C::MortalComponent>(corpse).dead = 1;
        registry.emplace<C::DecayComponent>(corpse).decay_duration = 17;
        std::uint32_t serviced = 0;
        bool saw_reduced = false;
        for (std::uint32_t tick = 1; tick <= 12; ++tick) {
            if (tick == 2 || tick == 3 || (tick > 3 && tick % 2 == 1))
                session.RecordActiveRegionWork({{0, 0}, 2000000, 0, 0, 0});
            ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
            ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
            const auto& record = session.GetActiveRegionLedger().records().at({0, 0});
            saw_reduced = saw_reduced || record.state == W::RegionState::Reduced;
            if (!session.GetRegionSchedule().due.empty()) {
                ++serviced;
                // Independent full-rate integer paths: age and decay each add one;
                // cumulative nutrient release floors once at the current total.
                const auto expected = elapsed ? tick : serviced;
                EXPECT_EQ(registry.get<C::MortalComponent>(mortal).age_ticks, expected);
                EXPECT_EQ(registry.get<C::DecayComponent>(corpse).decay_ticks, expected);
                EXPECT_EQ(registry.get<C::DecayComponent>(corpse).nutrient_released_milli,
                          (1000u * expected) / 17u);
            }
        }
        EXPECT_TRUE(saw_reduced);
        EXPECT_LT(serviced, 12u);
    }

    // The growth oracle uses the independent one-tick primitive, with constant inputs.
    entt::registry live, oracle;
    const auto plant = Plant(live, 8);
    const auto reference = Plant(oracle, 8);
    F::FoliageRegistryStage stage;
    stage.Begin(live, 1, true);
    AdvanceCandidate(stage, {{{0, 0}, 8}}, 8);
    for (std::uint64_t tick = 1; tick <= 8; ++tick) {
        F::RunPlantGrowthSystemOnTick(oracle, tick, [](const C::TransformComponent&) {
            return F::PlantEnvSample{0.5f, 0.5f, 0.5f, 0.5f};
        });
    }
    stage.PreparePublication(live);
    stage.Commit(live);
    EXPECT_EQ(live.get<C::PlantGrowthComponent>(plant).growth_points,
              oracle.get<C::PlantGrowthComponent>(reference).growth_points);
    EXPECT_EQ(live.get<C::PlantGrowthComponent>(plant).stress_points,
              oracle.get<C::PlantGrowthComponent>(reference).stress_points);
    EXPECT_EQ(live.get<C::MortalComponent>(plant).age_ticks, 8u);
}

TEST(RegionalCadenceIntegration, FrozenOwnersRemainByteIdentical) {
    W::GameSession session({.elapsed_consumers = true}, W::GameSession::AuthorityProfileTestOnly{});
    session.SetReplicatedSimulationAnchors({});
    session.PinActiveRegion({0, 0}, true);
    auto& registry = session.GetRegistry();
    const auto secondary = registry.create();
    registry.emplace<C::TransformComponent>(secondary).position = {8, 0, 8};
    registry.emplace<C::CircadianComponent>(secondary).activity = 0.321f;
    registry.emplace<C::TerritoryComponent>(secondary);
    registry.emplace<C::MigratoryComponent>(secondary);
    const auto frozen = Plant(registry, 520);
    registry.emplace<C::CircadianComponent>(frozen).activity = 0.123f;
    registry.emplace<C::TerritoryComponent>(frozen);
    registry.emplace<C::MigratoryComponent>(frozen);
    const auto before = Plants(registry);
    for (int frame = 0; frame < 3; ++frame) {
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt() * 2), 2u);
        ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        EXPECT_EQ(Plants(registry), before);
        EXPECT_EQ(registry.get<C::TerritoryComponent>(secondary).established, 1u);
        EXPECT_TRUE(registry.all_of<C::TerritoryBiasComponent>(secondary));
        EXPECT_NE(registry.get<C::CircadianComponent>(secondary).activity, 0.321f);
        EXPECT_FLOAT_EQ(registry.get<C::CircadianComponent>(frozen).activity, 0.123f);
        EXPECT_EQ(registry.get<C::TerritoryComponent>(frozen).established, 0u);
        EXPECT_FALSE(registry.all_of<C::TerritoryBiasComponent>(frozen));
        EXPECT_EQ(registry.get<C::MortalComponent>(frozen).age_ticks, 0u);
    }
    // Non-creature secondary opt-ins must enter the owner index on first construction.
    session.PinActiveRegion({1, 0}, true);
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(registry.get<C::TerritoryComponent>(frozen).established, 1u);
    EXPECT_TRUE(registry.all_of<C::TerritoryBiasComponent>(frozen));
    EXPECT_NE(registry.get<C::CircadianComponent>(frozen).activity, 0.123f);
}

TEST(RegionalCadenceIntegration, ReopenPreservesNextDueWindow) {
    using Luminumbra::TestSupport::ActiveRegionAcceptanceWorld;
    constexpr W::RegionKey owner{40, -40};
    // Two checkpoints: Reduced just before freeze, and Frozen before thaw.
    // Each trajectory is twelve ticks, one plant and two tiny chunks.
    for (const std::uint64_t checkpoint : {4u, 6u}) {
        SCOPED_TRACE(checkpoint);
        ActiveRegionAcceptanceWorld fixture(1);
        W::GameSession uninterrupted, reopened;
        fixture.Configure(uninterrupted);
        ActiveRegionAcceptanceWorld::AdoptAuthority(uninterrupted);
        uninterrupted.PinActiveRegion(owner, true);
        Luminumbra::TestSupport::ClockTestPlant(uninterrupted.GetRegistry(), {20488, 0, -20472});
        std::string frozen_plants;
        bool saw_reduced_skip = false;
        bool saw_thaw = false;
        for (std::uint64_t tick = 1; tick <= 12; ++tick) {
            SCOPED_TRACE(tick);
            for (auto* session : {&uninterrupted, &reopened}) {
                if (session == &reopened && tick <= checkpoint)
                    continue;
                // Script scheduler pressure independently of measured consumer work.
                auto& ledger = const_cast<W::ActiveRegionLedger&>(session->GetActiveRegionLedger());
                ledger.clear_completed_work();
                session->RecordActiveRegionWork({owner, tick <= 7 ? 11u : 10u, 0, 0, 0});
                ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
                ASSERT_FALSE(session->GetRegionSchedulerFault());
            }
            const auto& ledger = uninterrupted.GetActiveRegionLedger();
            const auto& record = ledger.records().at(owner);
            const auto expected = tick < 3 || tick >= 11  ? W::RegionState::Active
                                  : tick < 5 || tick >= 9 ? W::RegionState::Reduced
                                                          : W::RegionState::Frozen;
            ASSERT_EQ(record.state, expected);
            const auto& due = uninterrupted.GetRegionSchedule().due;
            if (expected == W::RegionState::Reduced &&
                std::find(due.begin(), due.end(), owner) == due.end())
                saw_reduced_skip = true;
            if (tick == 5)
                frozen_plants = Plants(uninterrupted.GetRegistry());
            if (tick >= 5 && tick <= 8) {
                EXPECT_EQ(Plants(uninterrupted.GetRegistry()), frozen_plants);
            }
            if (tick == 9)
                saw_thaw = true;
            if (tick == checkpoint) {
                ASSERT_TRUE(uninterrupted.SaveWorldState()) << uninterrupted.GetWorldOpenError();
                fixture.Configure(reopened);
                ASSERT_TRUE(reopened.LoadWorldStateFrom(uninterrupted.GetWorldSaveDir()))
                    << reopened.GetWorldOpenError();
                EXPECT_EQ(reopened.GetActiveRegionLedger().encode(), ledger.encode());
                EXPECT_EQ(reopened.GetWorldClock().canonical_bytes(),
                          uninterrupted.GetWorldClock().canonical_bytes());
            }
            if (tick >= checkpoint) {
                EXPECT_EQ(reopened.GetActiveRegionLedger().canonical_bytes(),
                          ledger.canonical_bytes());
                EXPECT_EQ(Plants(reopened.GetRegistry()), Plants(uninterrupted.GetRegistry()));
                EXPECT_EQ(reopened.GetFieldPages().serialize(),
                          uninterrupted.GetFieldPages().serialize());
            }
            if (tick > checkpoint) {
                EXPECT_EQ(reopened.GetRegionSchedule().due, due);
                EXPECT_EQ(reopened.GetRegionSchedule().prior_ticks,
                          uninterrupted.GetRegionSchedule().prior_ticks);
                EXPECT_EQ(reopened.GetRegionSchedule().digest,
                          uninterrupted.GetRegionSchedule().digest);
            }
        }
        EXPECT_TRUE(saw_reduced_skip);
        EXPECT_TRUE(saw_thaw);
        EXPECT_NE(Plants(uninterrupted.GetRegistry()), frozen_plants);
    }
}

TEST(RegionalCadenceIntegration, BatchedFramesAbortAndResumePublishOnce) {
    for (const bool prepare : {false, true}) {
        entt::registry live;
        const auto parent = Plant(live, 8);
        live.get<C::PlantGrowthComponent>(parent).stage =
            static_cast<std::uint8_t>(C::PlantStage::Fruiting);
        auto& crop = live.get<C::CropLifecycleComponent>(parent);
        crop.perennial = true;
        crop.lifespan_ticks = 1;
        live.emplace<C::CircadianComponent>(parent);
        live.emplace<C::TerritoryComponent>(parent);
        live.emplace<C::MigratoryComponent>(parent);
        live.emplace<C::PlantRegionComponent>(parent, std::uint64_t{1}, 0, 0, std::uint64_t{0});
        live.get<C::PlantGrowthComponent>(parent).planted_tick = 32;
        const auto before = Plants(live);
        const std::map<W::RegionKey, std::uint32_t> windows{{{0, 0}, 33}};
        W::FieldPageState fields;
        ASSERT_TRUE(fields.set_irrigation({8, 8}, 400));
        const auto field_bytes = fields.serialize();
        F::FoliageRegistryStage stage;
        std::string expected;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            stage.Begin(live, 2, true);
            auto candidate_fields = fields;
            F::FoliageWindowReplay replay(candidate_fields, windows, 33, true);
            replay.Advance(stage.registry(), candidate_fields);
            EXPECT_EQ(replay.remaining_history_steps(), 0u);
            EXPECT_EQ(Plants(live), before);
            EXPECT_EQ(fields.serialize(), field_bytes);
            replay.FinishLiveTick(stage.registry(), candidate_fields);
            AdvanceCandidate(stage, windows, 33);
            if (attempt == 0) {
                expected = Plants(stage.registry());
                if (prepare)
                    stage.PreparePublication(live);
                stage.Abort();
                stage.Commit(live);
                EXPECT_EQ(stage.publications(), 0u);
                EXPECT_EQ(Plants(live), before);
                EXPECT_FALSE(live.all_of<C::TerritoryBiasComponent>(parent));
            } else {
                EXPECT_EQ(Plants(stage.registry()), expected);
                stage.PreparePublication(live);
                EXPECT_EQ(Plants(live), before);
                EXPECT_FALSE(live.all_of<C::TerritoryBiasComponent>(parent));
                stage.Commit(live);
                stage.Commit(live);
                EXPECT_EQ(stage.publications(), 1u);
                EXPECT_EQ(live.view<C::PlantTag>().size(), 2u);
                EXPECT_EQ(live.get<C::CropLifecycleComponent>(parent).generations, 1u);
                EXPECT_EQ(stage.next_id(), 3u);
                EXPECT_TRUE(live.all_of<C::TerritoryBiasComponent>(parent));
                for (const auto child : live.view<C::PlantTag>()) {
                    if (child != parent) {
                        EXPECT_EQ(live.get<C::PlantRegionComponent>(child).persistent_id, 2u);
                        EXPECT_EQ(live.get<C::PlantGrowthComponent>(child).planted_tick, 32u);
                        const std::array<W::RegionKey, 1> due{{{0, 0}}};
                        const W::RegionConsumerGate gate(
                            live, due, W::RegionElapsedTicks(windows), 33);
                        EXPECT_EQ(gate.ElapsedTicks(child), 2u);
                    }
                }
                EXPECT_TRUE(replay.FinishLiveTick(stage.registry(), candidate_fields).empty());
            }
        }
    }
}
} // namespace
