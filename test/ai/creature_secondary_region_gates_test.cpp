#include "../support/UniqueTempPath.h"
#include "luminumbra_common/ai/ForagingSystem.h"
#include "luminumbra_common/ai/LifespanSystem.h"
#include "luminumbra_common/ai/ScentDepositSystem.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/systems/FieldEmitterSystem.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

namespace {
namespace A = luminumbra::ai;
namespace C = Luminumbra::Components;
namespace W = Luminumbra::world;
namespace S = Luminumbra::Systems;
using luminumbra::fields::EnergyFieldState;

entt::entity Actor(entt::registry& registry, float x = 4.0f) {
    const auto entity = registry.create();
    registry.emplace<C::CreatureComponent>(entity);
    registry.emplace<C::TransformComponent>(entity).position = {x, 0, 4};
    auto& sensable = registry.emplace<C::SensableComponent>(entity);
    sensable.scent_channel = 0;
    sensable.scent_deposit = 2.0f;
    registry.emplace<C::ScentSenseComponent>(entity).channel = 0;
    registry.emplace<C::LocomotionProfile>(entity).move_speed = 3.0f;
    registry.emplace<C::LocomotionIntentComponent>(entity).wish_xz = {0, 0.25f};
    registry.emplace<C::ForagerComponent>(entity, 4, 4, 4, 4, false, 0u);
    registry.emplace<C::FieldEmitterComponent>(entity, 0, 128u, 0);
    return entity;
}

entt::entity Food(entt::registry& registry, float x, int cell_x) {
    const auto entity = registry.create();
    registry.emplace<C::CreatureComponent>(entity);
    registry.emplace<C::TransformComponent>(entity).position = {x, 0, 4};
    registry.emplace<C::FoodSourceComponent>(entity, cell_x, 4, 10);
    return entity;
}

void ExpectSameField(const A::ScentField& actual, const A::ScentField& expected) {
    for (int ch = 0; ch < actual.channels(); ++ch) {
        for (int z = 0; z < actual.height(); ++z) {
            for (int x = 0; x < actual.width(); ++x) {
                EXPECT_EQ(std::bit_cast<std::uint64_t>(actual.Sample(ch, x, z)),
                          std::bit_cast<std::uint64_t>(expected.Sample(ch, x, z)));
            }
        }
    }
}

auto ForagerState(const entt::registry& registry, entt::entity entity) {
    const auto& ant = registry.get<C::ForagerComponent>(entity);
    return std::tuple{
        ant.cell_x, ant.cell_z, ant.home_x, ant.home_z, ant.carrying_food, ant.deliveries};
}

TEST(CreatureSecondaryRegionGates, CapturedOwnersFreezeEveryConsumerAndResumeWithPlant) {
    // Unknown owner, absent transform, invalid anchor and an empty due roster.
    for (int mode = 0; mode < 4; ++mode) {
        SCOPED_TRACE(mode);
        entt::registry registry;
        const auto actor = Actor(registry, 513);
        const auto plant = registry.create();
        registry.emplace<C::PlantTag>(plant);
        registry.emplace<C::TransformComponent>(plant).position = {513, 0, 4};
        registry.emplace<C::MortalComponent>(plant).lifespan_ticks = 10000u;
        if (mode == 1) {
            registry.remove<C::TransformComponent>(actor);
        }
        if (mode == 2) {
            registry.get<C::TransformComponent>(actor).position.x =
                std::numeric_limits<float>::infinity();
        }
        const std::vector<W::RegionKey> due =
            mode == 3 ? std::vector<W::RegionKey>{} : std::vector<W::RegionKey>{{0, 0}};
        W::RegionConsumerGate gate(registry, due);
        A::CreatureTickAdmission admission;
        admission.Capture(registry, gate);
        EXPECT_EQ(admission.owner_visits(), 1u);
        // Consumers must use the pre-movement decision even in an open destination cell.
        registry.emplace_or_replace<C::TransformComponent>(actor).position = {4, 0, 4};
        A::ScentField scent(12, 12, 4);
        scent.Deposit(0, 5, 4, 10);
        const auto original = scent;
        const auto ant_before = ForagerState(registry, actor);
        const auto wish_before = registry.get<C::LocomotionIntentComponent>(actor).wish_xz;
        EnergyFieldState energy;
        const EnergyFieldState::RegionRecords records{{{0, 0}, {}}};
        EXPECT_EQ(A::RunScentDepositOnTick(registry, scent, 0, 0, 1, &admission).emitters, 0u);
        EXPECT_EQ(A::RunScentSteeringOnTick(registry, scent, 0, 0, 1, &admission).considered, 0u);
        EXPECT_EQ(A::RunForagingOnTick(registry, scent, {}, &admission).foragers, 0u);
        EXPECT_EQ(S::GatherFieldEmitterDeposits(registry, energy, false, &admission).deposits, 0u);
        const auto gathered = S::GatherRegionalFieldEmitterDeposits(
            registry, energy, records, false, EnergyFieldState::kMaxRegionalDeposits, &admission);
        EXPECT_TRUE(gathered.accepted);
        EXPECT_EQ(gathered.visited_emitters, 1u);
        EXPECT_EQ(gathered.queued.deposits, 0u);
        A::RunLifespanOnTick(registry, 1, gate, &admission);
        EXPECT_EQ(registry.get<C::MortalComponent>(plant).age_ticks, 0u);
        EXPECT_EQ(registry.get<C::LocomotionIntentComponent>(actor).wish_xz, wish_before);
        EXPECT_EQ(ForagerState(registry, actor), ant_before);
        ExpectSameField(scent, original);
        EXPECT_EQ(energy.regional_deposit_capacity(), EnergyFieldState::kMaxRegionalDeposits);

        registry.get<C::TransformComponent>(actor).position.x = 513;
        const std::array<W::RegionKey, 1> resumed_due{{{1, 0}}};
        W::RegionConsumerGate resumed(registry, resumed_due);
        admission.Capture(registry, resumed);
        registry.get<C::TransformComponent>(actor).position.x = 4;
        EXPECT_EQ(A::RunScentDepositOnTick(registry, scent, 0, 0, 1, &admission).emitters, 1u);
        EXPECT_EQ(A::RunScentSteeringOnTick(registry, scent, 0, 0, 1, &admission).steered, 1u);
        EXPECT_EQ(A::RunForagingOnTick(registry, scent, {}, &admission).foragers, 1u);
        EXPECT_EQ(S::GatherFieldEmitterDeposits(registry, energy, false, &admission).deposits, 1u);
        EXPECT_EQ(S::GatherRegionalFieldEmitterDeposits(registry,
                                                        energy,
                                                        records,
                                                        false,
                                                        EnergyFieldState::kMaxRegionalDeposits,
                                                        &admission)
                      .queued.deposits,
                  1u);
        A::RunLifespanOnTick(registry, 2, resumed, &admission);
        EXPECT_EQ(registry.get<C::MortalComponent>(plant).age_ticks, 1u);
        EXPECT_NE(registry.get<C::LocomotionIntentComponent>(actor).wish_xz, wish_before);
        EXPECT_NE(ForagerState(registry, actor), ant_before);
    }
}

TEST(CreatureSecondaryRegionGates, DeniedFoodCannotAttractOrFeedAdmittedForager) {
    for (int mode = 0; mode < 4; ++mode) {
        entt::registry registry;
        const auto denied_actor = Actor(registry, 513);
        auto& denied_ant = registry.get<C::ForagerComponent>(denied_actor);
        denied_ant.cell_x = 5;
        denied_ant.carrying_food = true;
        const auto denied_before = ForagerState(registry, denied_actor);
        const auto actor = Actor(registry);
        const auto denied_food = Food(registry, 513, 5);
        Food(registry, 4, 2);
        if (mode == 1) {
            registry.remove<C::TransformComponent>(denied_food);
        }
        if (mode == 2) {
            registry.get<C::TransformComponent>(denied_food).position.x =
                std::numeric_limits<float>::quiet_NaN();
        }
        const std::array<W::RegionKey, 1> due{{{0, 0}}};
        A::CreatureTickAdmission admission;
        admission.Capture(registry, W::RegionConsumerGate(registry, due));
        registry.emplace_or_replace<C::TransformComponent>(denied_food).position = {4, 0, 4};
        // mode 3 places the denied food on the actual arrival cell.
        if (mode == 3) {
            registry.get<C::FoodSourceComponent>(denied_food).cell_x = 3;
        }
        A::ScentField scent(12, 12, 4);
        const auto stats = A::RunForagingOnTick(registry, scent, {}, &admission);
        EXPECT_EQ(stats.foragers, 1u);
        EXPECT_EQ(stats.pickups, 0u);
        EXPECT_EQ(ForagerState(registry, denied_actor), denied_before);
        EXPECT_EQ(scent.Sample(A::kFoodTrailChannel, 5, 4), 0);
        EXPECT_EQ(registry.get<C::ForagerComponent>(actor).cell_x, 3);
        EXPECT_EQ(registry.get<C::FoodSourceComponent>(denied_food).amount, 10);
        // Admit the target next tick: its existing food becomes a real goal and recipient.
        registry.emplace_or_replace<C::TransformComponent>(denied_food).position = {4, 0, 4};
        admission.Capture(registry, W::RegionConsumerGate(registry, due));
        registry.get<C::ForagerComponent>(actor).cell_x = 4;
        registry.get<C::FoodSourceComponent>(denied_food).cell_x = 5;
        EXPECT_EQ(A::RunForagingOnTick(registry, scent, {1, 0, 1}, &admission).pickups, 1u);
        EXPECT_EQ(registry.get<C::FoodSourceComponent>(denied_food).amount, 9);
    }
}

TEST(CreatureSecondaryRegionGates, ClosedCellsStillGateTrailsGoalsAndEmitterRecipients) {
    entt::registry registry;
    const auto actor = Actor(registry);
    Food(registry, 4, 5);
    const std::array<W::RegionKey, 1> due{{{0, 0}}};
    A::CreatureTickAdmission admission;
    admission.Capture(registry, W::RegionConsumerGate(registry, due));
    A::ScentField scent(12, 12, 4);
    scent.BindDepositGate([](int, int) { return false; });
    EXPECT_EQ(A::RunScentDepositOnTick(registry, scent, 0, 0, 1, &admission).emitters, 0u);
    EXPECT_EQ(A::RunScentSteeringOnTick(registry, scent, 0, 0, 1, &admission).steered, 0u);
    const auto before = ForagerState(registry, actor);
    EXPECT_EQ(A::RunForagingOnTick(registry, scent, {}, &admission).foragers, 0u);
    EXPECT_EQ(ForagerState(registry, actor), before);
    // A sealed home is not a directional goal for a laden forager. Equal local
    // scores select +X; the inaccessible home would otherwise pull toward -X.
    auto& ant = registry.get<C::ForagerComponent>(actor);
    ant.carrying_food = true;
    ant.home_x = 0;
    scent.BindDepositGate([](int x, int) { return x != 0; });
    A::RunForagingOnTick(registry, scent, {1, 0, 1}, &admission);
    EXPECT_EQ(ant.cell_x, 5);
    EXPECT_EQ(ant.deliveries, 0u);
    ant.home_x = 6;
    EXPECT_EQ(A::RunForagingOnTick(registry, scent, {1, 0, 1}, &admission).deliveries, 1u);
    // Source owner is due, but the footprint crosses into a frozen recipient.
    registry.get<C::TransformComponent>(actor).position = {480, 0, 24};
    registry.get<C::FieldEmitterComponent>(actor).radius_cells = 1;
    EnergyFieldState energy;
    EnergyFieldState::RegionRecords records{{{0, 0}, {}}, {{1, 0}, {}}};
    records.at({1, 0}).state = W::RegionState::Frozen;
    const auto stats = S::GatherRegionalFieldEmitterDeposits(
        registry, energy, records, true, EnergyFieldState::kMaxRegionalDeposits, &admission);
    ASSERT_TRUE(stats.accepted);
    EXPECT_EQ(stats.queued.deposits, 6u);
    EnergyFieldState::RegionalResult result;
    ASSERT_TRUE(energy.TickRegional(1, records, due, {}, result));
    EXPECT_EQ(energy.at_cell(21, 1), 0u);
    EXPECT_GT(energy.at_cell(20, 1), 0u);
}

TEST(CreatureSecondaryRegionGates, EmitterKeysAndDepositsIgnoreRegistryOrderAndHandles) {
    using DepositTuple = std::tuple<std::uint64_t, int, int, int, std::uint64_t>;
    std::vector<DepositTuple> reference;
    std::string reference_bytes;
    for (const bool reverse : {false, true}) {
        entt::registry registry;
        if (reverse) {
            const auto unused = registry.create();
            registry.destroy(unused);
            (void)registry.create();
        }
        for (const auto rate : reverse ? std::array{128u, 65535u} : std::array{65535u, 128u}) {
            const auto entity = Actor(registry, 24);
            registry.get<C::TransformComponent>(entity).position.z = 24;
            registry.get<C::FieldEmitterComponent>(entity) = {0, rate, 1};
        }
        const std::array<W::RegionKey, 1> due{{{0, 0}}};
        A::CreatureTickAdmission admission;
        admission.Capture(registry, W::RegionConsumerGate(registry, due));
        const EnergyFieldState::RegionRecords records{{{0, 0}, {}}};
        std::vector<EnergyFieldState::RegionalDeposit> deposits;
        for (const auto entity : registry.view<C::FieldEmitterComponent>()) {
            ASSERT_TRUE(
                S::AppendRegionalEmitterFootprint(deposits,
                                                  entity,
                                                  registry.get<C::TransformComponent>(entity),
                                                  registry.get<C::FieldEmitterComponent>(entity),
                                                  2,
                                                  records,
                                                  false,
                                                  &admission));
        }
        std::vector<DepositTuple> canonical;
        for (const auto& deposit : deposits) {
            EXPECT_EQ(deposit.emitter_id, 0u);
            canonical.emplace_back(
                deposit.emitter_id, deposit.cx, deposit.cz, deposit.channel, deposit.amount);
        }
        std::sort(canonical.begin(), canonical.end());
        EnergyFieldState energy, full;
        const auto gathered = S::GatherRegionalFieldEmitterDeposits(
            registry, energy, records, false, EnergyFieldState::kMaxRegionalDeposits, &admission);
        ASSERT_TRUE(gathered.accepted);
        EXPECT_EQ(gathered.visited_emitters, 2u);
        EXPECT_EQ(gathered.queued.deposits, 18u);
        EXPECT_EQ(S::GatherFieldEmitterDeposits(registry, full, false, &admission).deposits, 18u);
        std::vector<DepositTuple> queued;
        for (std::size_t index = 0; index < 18; ++index) {
            const auto deposit = energy.pending_deposit(index);
            const auto full_deposit = full.pending_deposit(index);
            ASSERT_TRUE(deposit.has_value());
            ASSERT_TRUE(full_deposit.has_value());
            EXPECT_EQ(deposit->emitter_id, 0u);
            EXPECT_EQ(full_deposit->emitter_id, 0u);
            queued.emplace_back(
                deposit->emitter_id, deposit->cx, deposit->cz, deposit->channel, deposit->amount);
        }
        EXPECT_FALSE(energy.pending_deposit(18).has_value());
        std::sort(queued.begin(), queued.end());
        EXPECT_EQ(queued, canonical);
        EnergyFieldState::RegionalResult regional_result, full_result;
        ASSERT_TRUE(energy.TickRegional(1, records, due, {}, regional_result));
        ASSERT_TRUE(full.TickRegional(1, records, due, {}, full_result));
        EXPECT_EQ(energy.CanonicalRegionalBytes(), full.CanonicalRegionalBytes());
        if (reverse) {
            EXPECT_EQ(canonical, reference);
            EXPECT_EQ(energy.CanonicalRegionalBytes(), reference_bytes);
        } else {
            reference = canonical;
            reference_bytes = energy.CanonicalRegionalBytes();
        }
    }
}

TEST(CreatureSecondaryRegionGates, EmptyAndDeniedMalformedEmittersAreNeutral) {
    entt::registry registry;
    A::CreatureTickAdmission admission;
    admission.Capture(registry, W::RegionConsumerGate(registry, {}));
    A::ScentField scent(12, 12, 4);
    EnergyFieldState energy;
    EXPECT_FALSE(energy.pending_deposit(0).has_value());
    EXPECT_FALSE(energy.pending_deposit(std::numeric_limits<std::size_t>::max()).has_value());
    EXPECT_EQ(A::RunScentDepositOnTick(registry, scent, 0, 0, 1, &admission).emitters, 0u);
    EXPECT_EQ(A::RunScentSteeringOnTick(registry, scent, 0, 0, 1, &admission).considered, 0u);
    EXPECT_EQ(A::RunForagingOnTick(registry, scent, {}, &admission).foragers, 0u);
    const auto actor = Actor(registry, 513);
    registry.get<C::FieldEmitterComponent>(actor) = {-1, 128u, 32};
    admission.Capture(registry, W::RegionConsumerGate(registry, {}));
    EXPECT_TRUE(
        S::GatherRegionalFieldEmitterDeposits(registry, energy, {}, false, 0, &admission).accepted);
    // Admitted malformed input retains the existing all-or-nothing refusal.
    admission.Capture(registry, W::RegionConsumerGate{});
    EXPECT_FALSE(
        S::GatherRegionalFieldEmitterDeposits(registry, energy, {}, false, 0, &admission).accepted);
    EXPECT_EQ(energy.regional_deposit_capacity(), EnergyFieldState::kMaxRegionalDeposits);
}

TEST(CreatureSecondaryRegionGates, SessionForagingAndEmitterSourcesResumeWithTheirPlant) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    const Luminumbra::TestSupport::ScopedTempFile temporary("secondary_regions_", ".json");
    W::GameSession session;
    session.SetRootPath(std::string(LUMINUMBRA_SOURCE_ROOT) + '/');
    session.SetSaveRootPath(temporary.Path().parent_path());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetAetherStateEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Secondary regions", "1337", "default"));
    session.SetReplicatedSimulationAnchors({});
    session.PinActiveRegion({0, 0}, true);
    auto& registry = session.GetRegistry();
    const auto actor = Actor(registry, 513);
    registry.get<C::CreatureComponent>(actor).move_speed = 0;
    registry.get<C::FieldEmitterComponent>(actor).radius_cells = 1;
    auto& ant = registry.get<C::ForagerComponent>(actor);
    ant.cell_x = session.ScentWorldToCellX(session.GetMetadata().spawnPoint.x);
    ant.cell_z = session.ScentWorldToCellZ(session.GetMetadata().spawnPoint.z);
    ant.home_x = ant.cell_x;
    ant.home_z = ant.cell_z;
    const auto before = ForagerState(registry, actor);
    ASSERT_NE(session.GetScentField(), nullptr);
    session.GetScentField()->RestoreCell(0, ant.cell_x, ant.cell_z, 10);
    const auto plant = registry.create();
    registry.emplace<C::PlantTag>(plant);
    registry.emplace<C::TransformComponent>(plant).position = {513, 0, 4};
    registry.emplace<C::MortalComponent>(plant).lifespan_ticks = 10000u;
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(ForagerState(registry, actor), before);
    EXPECT_EQ(registry.get<C::MortalComponent>(plant).age_ticks, 0u);
    EXPECT_GT(session.GetScentField()->Sample(0, ant.cell_x, ant.cell_z), 0);
    EXPECT_LT(session.GetScentField()->Sample(0, ant.cell_x, ant.cell_z), 10);
    ASSERT_NE(session.GetEnergyFieldState(), nullptr);
    EXPECT_EQ(session.GetEnergyFieldState()->at_cell(20, 0), 0u);
    session.PinActiveRegion({1, 0}, true);
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_NE(ForagerState(registry, actor), before);
    EXPECT_EQ(registry.get<C::MortalComponent>(plant).age_ticks, 1u);
    EXPECT_GT(session.GetEnergyFieldState()->at_cell(20, 0), 0u);
}

TEST(CreatureSecondaryRegionGates, BirthsAndOtherEntityKindsRetainFullRateConventions) {
    entt::registry registry;
    A::CreatureTickAdmission admission;
    admission.Capture(registry, W::RegionConsumerGate(registry, {}));
    const auto newborn = Actor(registry);
    const auto other = Actor(registry);
    registry.remove<C::CreatureComponent>(other);
    const auto inert = Actor(registry);
    registry.remove<C::CreatureComponent>(inert);
    registry.get<C::SensableComponent>(inert).scent_channel = -1;
    registry.get<C::ScentSenseComponent>(inert).channel = -1;
    registry.get<C::FieldEmitterComponent>(inert).rate_raw_per_tick = 0;
    registry.remove<C::ForagerComponent>(inert);
    A::ScentField scent(12, 12, 4);
    scent.Deposit(0, 5, 4, 10);
    EXPECT_EQ(A::RunScentDepositOnTick(registry, scent, 0, 0, 0, &admission).emitters, 0u);
    EXPECT_EQ(A::RunScentSteeringOnTick(registry, scent, 0, 0, 0, &admission).considered, 0u);
    EXPECT_EQ(A::RunScentDepositOnTick(registry, scent, 0, 0, 1, &admission).emitters, 2u);
    EXPECT_EQ(A::RunScentSteeringOnTick(registry, scent, 0, 0, 1, &admission).steered, 2u);
    EXPECT_EQ(A::RunForagingOnTick(registry, scent, {}, &admission).foragers, 2u);
    EnergyFieldState energy;
    EXPECT_EQ(S::GatherFieldEmitterDeposits(registry, energy, false, &admission).emitters, 2u);
    EXPECT_TRUE(admission.Allows(newborn));
    EXPECT_TRUE(admission.Allows(other));
}

TEST(CreatureSecondaryRegionGates, InactiveConsumersMatchIndependentFullRateArithmetic) {
    // This oracle includes the secondary state omitted by the core creature oracle:
    // every field sample, locomotion wish bit, trail, pickup, delivery and energy byte.
    entt::registry registry;
    const auto actor = Actor(registry);
    const auto food = Food(registry, 513, 5);
    A::ScentField scent(12, 12, 4), expected(12, 12, 4);
    EnergyFieldState energy, expected_energy;
    energy.SetAnchorCell(0, 0);
    expected_energy.SetAnchorCell(0, 0);
    scent.Deposit(0, 5, 4, 10);
    expected.Deposit(0, 5, 4, 10);
    for (std::uint64_t tick = 1; tick <= 16; ++tick) {
        expected.Deposit(0, 4, 4, 2.0);
        A::RunScentDepositOnTick(registry, scent, 0, 0, 1);
        const auto& sense = registry.get<C::ScentSenseComponent>(actor);
        float dx = 0, dz = 0;
        const float confidence = expected.GradientSteer(
            sense.channel, 4, 4, sense.sign, sense.floor, sense.weber_k, dx, dz);
        auto wish = registry.get<C::LocomotionIntentComponent>(actor).wish_xz;
        if (confidence > 0) {
            wish.x += dx * confidence * sense.strength * 3.0f;
            wish.y += dz * confidence * sense.strength * 3.0f;
            const float magnitude =
                Luminumbra::DeterministicMath::Sqrt(wish.x * wish.x + wish.y * wish.y);
            if (magnitude > 3.0f && magnitude > 0) {
                const float scale = 3.0f / magnitude;
                wish.x *= scale;
                wish.y *= scale;
            }
        }
        A::RunScentSteeringOnTick(registry, scent, 0, 0, 1);
        const auto actual_wish = registry.get<C::LocomotionIntentComponent>(actor).wish_xz;
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual_wish.x),
                  std::bit_cast<std::uint32_t>(wish.x));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual_wish.y),
                  std::bit_cast<std::uint32_t>(wish.y));
        // With trail weight zero, this one-cell route alternates pickup and delivery.
        const bool outbound = tick % 2 != 0;
        expected.Deposit(outbound ? 3 : 2, outbound ? 4 : 5, 4, 1);
        const auto stats = A::RunForagingOnTick(registry, scent, {1, 0, 1});
        EXPECT_EQ(stats.pickups, outbound ? 1u : 0u);
        EXPECT_EQ(stats.deliveries, outbound ? 0u : 1u);
        EXPECT_EQ(registry.get<C::ForagerComponent>(actor).cell_x, outbound ? 5 : 4);
        EXPECT_EQ(registry.get<C::ForagerComponent>(actor).carrying_food, outbound);
        EXPECT_EQ(registry.get<C::ForagerComponent>(actor).deliveries, tick / 2);
        EXPECT_EQ(registry.get<C::FoodSourceComponent>(food).amount,
                  10 - static_cast<int>((tick + 1) / 2));
        S::GatherFieldEmitterDeposits(registry, energy);
        expected_energy.QueueDeposit(
            static_cast<std::uint64_t>(entt::to_integral(actor)), 0, 0, 0, 128);
        EXPECT_EQ(energy.Tick(tick), expected_energy.Tick(tick));
        EXPECT_FALSE(energy.CanonicalBytes().empty());
        EXPECT_EQ(energy.CanonicalBytes(), expected_energy.CanonicalBytes());
        EXPECT_EQ(energy.SerializeRecord(tick), expected_energy.SerializeRecord(tick));
        scent.Step(0.2, 1, 0.05);
        expected.Step(0.2, 1, 0.05);
        ExpectSameField(scent, expected);
    }
}
} // namespace
