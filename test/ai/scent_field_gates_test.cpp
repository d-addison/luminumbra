#include "gtest/gtest.h"

#include "luminumbra_common/ai/CreatureBrainSystem.h"
#include "luminumbra_common/ai/ForagingSystem.h"
#include "luminumbra_common/ai/ScentDepositSystem.h"
#include "luminumbra_common/ai/ScentFieldGates.h"
#include "luminumbra_common/ai/ScentSteeringSystem.h"
#include "luminumbra_common/fields/FieldPageState.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace {
using luminumbra::ai::ScentField;
using luminumbra::ai::detail::CacheNearScentGates;
using luminumbra::ai::detail::ScentStepGates;
using namespace Luminumbra::world;

constexpr int kCells = 128;
constexpr int kChannels = 4;
constexpr double kDiffusionRate = 0.25;
constexpr std::size_t kDiffusionIterations = 4;
constexpr double kEvaporation = 0.05;
constexpr double kTauMin = 1.0e-9;
constexpr double kTauMax = 1.0e6;
constexpr std::array<std::pair<double, double>, 6> kWinds{
    std::pair{0.0, 0.0}, {0.375, -1.25}, {-2.625, 0.875}, {4.25, -3.5}, {0.0, 2.125}, {-1.75, 0.0}};

// Reference owner/due/edge lookups mirror GameSession's original live callbacks.
// They deliberately do not use tile splits, corner decisions or cached gates.
struct LiveSchedule {
    ScentCellKey origin;
    std::map<RegionKey, ActiveRegionRecord> records;
    std::vector<RegionKey> due;
    std::uint64_t tick = 0;
    bool interior = true;

    explicit LiveSchedule(const ScentCellKey& domain_origin)
        : origin(domain_origin) {
        for (const int z : {0, kCells - 1})
            for (const int x : {0, kCells - 1})
                Set(Owner(x, z), RegionState::Active, std::uint8_t{0});
    }

    RegionKey Owner(int x, int z) const {
        return FieldPageState::region_of_scent_cell({origin.x + x, origin.z + z});
    }

    void Set(const RegionKey& owner, RegionState state, std::uint8_t shift) {
        auto& record = records[owner];
        record.state = state;
        record.cadence_shift = shift;
        record.cadence_phase = ActiveRegionLedger::phase(owner, shift);
    }

    void Reschedule(std::uint64_t next_tick) {
        tick = next_tick;
        due.clear();
        for (const auto& [owner, record] : records)
            if (record.state != RegionState::Frozen &&
                (tick & ((std::uint64_t{1} << record.cadence_shift) - 1)) == record.cadence_phase)
                due.push_back(owner);
    }

    bool CellDue(int x, int z) const {
        return std::find(due.begin(), due.end(), Owner(x, z)) != due.end();
    }

    bool EdgeOpen(int x, int z, int nx, int nz) const {
        const auto source = Owner(x, z), destination = Owner(nx, nz);
        if (source == destination)
            return CellDue(x, z);
        const auto a = records.find(source), b = records.find(destination);
        if (a == records.end() || b == records.end() || a->second.state == RegionState::Frozen ||
            b->second.state == RegionState::Frozen)
            return false;
        const auto shift = (std::max)(a->second.cadence_shift, b->second.cadence_shift);
        return shift < 63 && (tick & ((std::uint64_t{1} << shift) - 1)) == 0;
    }

    ScentStepGates LiveGates() const {
        return {[this](int x, int z) { return interior && CellDue(x, z); },
                [this](int x, int z, int nx, int nz) {
                    return EdgeOpen(x, z, nx, nz);
                }};
    }

    ScentStepGates CachedGates() const {
        return CacheNearScentGates(
            kCells,
            [this](int x, int z) { return Owner(x, z); },
            [this](int x, int z) { return interior && CellDue(x, z); },
            [this](int x, int z, int nx, int nz) { return EdgeOpen(x, z, nx, nz); });
    }
};

ScentField InitialField() {
    ScentField field(kCells, kCells, kChannels);
    for (int channel = 0; channel < kChannels; ++channel)
        for (int z = 0; z < kCells; ++z)
            for (int x = 0; x < kCells; ++x) {
                // Nonuniform binary64 inputs distinguish bilinear sample/path choices.
                // Zeros and spikes also exercise both ends of the clamp.
                const int pattern = (x * 17 + z * 31 + channel * 43) % 101;
                const double value = pattern == 0   ? 0.0
                                     : pattern == 1 ? 2.0e6
                                                    : static_cast<double>(pattern) / 37.0;
                field.RestoreCell(channel, x, z, value);
            }
    return field;
}

testing::AssertionResult SameBits(const ScentField& reference, const ScentField& optimized) {
    for (int channel = 0; channel < kChannels; ++channel)
        for (int z = 0; z < kCells; ++z)
            for (int x = 0; x < kCells; ++x) {
                const auto expected = std::bit_cast<std::uint64_t>(reference.Sample(channel, x, z));
                const auto actual = std::bit_cast<std::uint64_t>(optimized.Sample(channel, x, z));
                if (expected != actual)
                    return testing::AssertionFailure()
                           << "channel=" << channel << " cell=(" << x << ',' << z
                           << ") expected bits=" << expected << " actual bits=" << actual;
            }
    return testing::AssertionSuccess();
}

void AdvanceAndCompare(ScentField& reference,
                       ScentField& optimized,
                       const LiveSchedule& schedule,
                       const ScentStepGates& cached,
                       const std::pair<double, double>& wind) {
    const auto live = schedule.LiveGates();
    ASSERT_TRUE(live.participants);
    ASSERT_TRUE(live.exchange);
    // Supplying a nonempty live EdgeGate keeps the pre-optimization BilinearSealed
    // path reachable, even when every lookup returns true.
    reference.Step(kDiffusionRate,
                   kDiffusionIterations,
                   kEvaporation,
                   wind.first,
                   wind.second,
                   live.participants,
                   live.exchange);
    optimized.Step(kDiffusionRate,
                   kDiffusionIterations,
                   kEvaporation,
                   wind.first,
                   wind.second,
                   cached.participants,
                   cached.exchange);
    ASSERT_TRUE(SameBits(reference, optimized)) << "after Step, before Clamp";
    reference.Clamp(kTauMin, kTauMax, live.participants);
    optimized.Clamp(kTauMin, kTauMax, cached.participants);
    ASSERT_TRUE(SameBits(reference, optimized)) << "after Clamp";
}
} // namespace

TEST(ScentFieldGates, FullyOpenDomainsMatchLiveSolverBitExactly) {
    // One owner, either axis split, four owners, negative coordinates, and splits
    // leaving only a single cell on one side of an owner boundary.
    const std::array<ScentCellKey, 7> origins{ScentCellKey{100, 100},
                                              {480, 100},
                                              {100, 480},
                                              {480, 500},
                                              {-13, -67},
                                              {385, 511},
                                              {512, 512}};
    for (const auto origin : origins) {
        SCOPED_TRACE(testing::Message() << "origin=(" << origin.x << ',' << origin.z << ')');
        LiveSchedule schedule(origin);
        auto reference = InitialField();
        auto optimized = reference;
        for (std::size_t step = 0; step < kWinds.size(); ++step) {
            SCOPED_TRACE(step);
            schedule.Reschedule(static_cast<std::uint64_t>(step) + 1);
            const auto cached = schedule.CachedGates();
            ASSERT_FALSE(cached.participants);
            ASSERT_FALSE(cached.exchange); // Must actually exercise the ungated solver.
            AdvanceAndCompare(reference, optimized, schedule, cached, kWinds[step]);
        }
    }
}

TEST(ScentFieldGates, ReducedFrozenAbsentAndMixedOwnersMatchLiveSolverBitExactly) {
    const std::array<ScentCellKey, 4> origins{
        ScentCellKey{100, 100}, {480, 100}, {100, 480}, {-13, -67}};
    for (const auto origin : origins) {
        for (int regime = 0; regime < 4; ++regime) {
            SCOPED_TRACE(testing::Message()
                         << "origin=(" << origin.x << ',' << origin.z << ") regime=" << regime);
            LiveSchedule schedule(origin);
            std::vector<RegionKey> owners;
            for (const auto& [owner, record] : schedule.records) {
                (void)record;
                owners.push_back(owner);
            }
            for (std::size_t index = 0; index < owners.size(); ++index) {
                const int mode = regime == 3 ? static_cast<int>(index % 4) : regime;
                if (mode == 0)
                    schedule.Set(owners[index],
                                 RegionState::Reduced,
                                 static_cast<std::uint8_t>(1 + index % 2));
                else if (mode == 1)
                    schedule.Set(owners[index], RegionState::Frozen, std::uint8_t{0});
                else if (mode == 2)
                    schedule.records.erase(owners[index]);
                else
                    schedule.Set(owners[index], RegionState::Active, std::uint8_t{0});
            }
            auto reference = InitialField();
            auto optimized = reference;
            for (std::uint64_t tick = 1; tick <= 8; ++tick) {
                SCOPED_TRACE(tick);
                schedule.Reschedule(tick);
                const auto cached = schedule.CachedGates();
                AdvanceAndCompare(reference,
                                  optimized,
                                  schedule,
                                  cached,
                                  kWinds[static_cast<std::size_t>(tick % kWinds.size())]);
            }
        }
    }
}

TEST(ScentFieldGates, RebuiltGatesMatchLiveSolverAcrossScheduleChangesBitExactly) {
    LiveSchedule schedule({-31, -19});
    const std::array<RegionKey, 4> owners{schedule.Owner(0, 0),
                                          schedule.Owner(kCells - 1, 0),
                                          schedule.Owner(0, kCells - 1),
                                          schedule.Owner(kCells - 1, kCells - 1)};
    auto reference = InitialField();
    auto optimized = reference;
    for (std::size_t step = 0; step < 10; ++step) {
        SCOPED_TRACE(step);
        for (std::size_t index = 0; index < owners.size(); ++index) {
            const auto owner = owners[index];
            const std::size_t mode = step == 0 || step == 9 ? 0 : (step + index) % 4;
            if (mode == 0)
                schedule.Set(owner, RegionState::Active, std::uint8_t{0});
            else if (mode == 1)
                schedule.Set(owner, RegionState::Reduced, static_cast<std::uint8_t>(1 + step % 2));
            else if (mode == 2)
                schedule.Set(owner, RegionState::Frozen, std::uint8_t{0});
            else
                schedule.records.erase(owner);
        }
        schedule.interior = step != 3 && step != 7;
        schedule.Reschedule(static_cast<std::uint64_t>(step) + 1);
        const auto cached = schedule.CachedGates(); // No cache may survive a schedule change.
        if (step == 0 || step == 9) {
            ASSERT_FALSE(cached.participants);
            ASSERT_FALSE(cached.exchange);
        } else {
            ASSERT_TRUE(cached.participants);
            ASSERT_TRUE(cached.exchange);
        }
        AdvanceAndCompare(reference, optimized, schedule, cached, kWinds[step % kWinds.size()]);
    }
}

TEST(ScentFieldGates, BorderOnlyOpenDomainMatchesLiveSolverBitExactly) {
    LiveSchedule schedule({480, 500});
    schedule.interior = false;
    schedule.Reschedule(4);
    const auto cached = schedule.CachedGates();
    ASSERT_TRUE(cached.participants);
    ASSERT_FALSE(cached.participants(0, 0));
    ASSERT_FALSE(cached.exchange); // Open edges are independent of interior participation.
    auto reference = InitialField();
    auto optimized = reference;
    AdvanceAndCompare(reference, optimized, schedule, cached, {0.375, -1.25});
}

TEST(ScentFieldGates, CachedDecisionsStaySealedUntilRebuilt) {
    LiveSchedule schedule({480, 500});
    const auto frozen_owner = schedule.Owner(kCells - 1, kCells - 1);
    schedule.Set(frozen_owner, RegionState::Frozen, std::uint8_t{0});
    schedule.Reschedule(4);
    int owner_queries = 0, due_queries = 0, edge_queries = 0;
    const auto cached = CacheNearScentGates(
        kCells,
        [&](int x, int z) {
            ++owner_queries;
            return schedule.Owner(x, z);
        },
        [&](int x, int z) {
            ++due_queries;
            return schedule.CellDue(x, z);
        },
        [&](int x, int z, int nx, int nz) {
            ++edge_queries;
            return schedule.EdgeOpen(x, z, nx, nz);
        });
    ASSERT_TRUE(cached.participants);
    ASSERT_TRUE(cached.exchange);
    const int cached_owner_queries = owner_queries;
    EXPECT_LE(owner_queries, 2 * kCells - 1);
    EXPECT_EQ(due_queries, 4);
    EXPECT_EQ(edge_queries, 16);
    auto reference = InitialField();
    auto optimized = reference;
    AdvanceAndCompare(reference, optimized, schedule, cached, {0.375, -1.25});
    EXPECT_EQ(owner_queries, cached_owner_queries);
    EXPECT_EQ(due_queries, 4);
    EXPECT_EQ(edge_queries, 16);

    schedule.Set(frozen_owner, RegionState::Active, std::uint8_t{0});
    schedule.Reschedule(5);
    EXPECT_FALSE(cached.participants(kCells - 1, kCells - 1));
    EXPECT_FALSE(cached.exchange(0, 0, kCells - 1, kCells - 1));
    const auto rebuilt = schedule.CachedGates();
    EXPECT_FALSE(rebuilt.participants);
    EXPECT_FALSE(rebuilt.exchange);
}

namespace {

void CheckNearDueQueryCost(std::size_t unrelated) {
    SCOPED_TRACE(unrelated);
    LiveSchedule schedule({480, 500});
    schedule.Reschedule(4);
    const auto owner_of = [&schedule](int x, int z) {
        return schedule.Owner(x, z);
    };
    auto due = schedule.due;
    for (std::size_t i = 0; i < unrelated; ++i) {
        due.push_back({static_cast<std::int32_t>(100 + i), 100});
    }
    luminumbra::ai::detail::NearScentDueCache<RegionKey> cache;
    std::size_t lookups = 0;
    const auto refresh = [&] {
        const std::set<RegionKey> index(due.begin(), due.end());
        const auto before = cache.query_visits();
        lookups = 0;
        cache.Refresh(kCells, owner_of, [&](RegionKey owner) {
            ++lookups;
            return index.contains(owner);
        });
        EXPECT_LE(lookups, 4u);
        EXPECT_GT(cache.query_visits() - before, lookups);
        EXPECT_LE(cache.query_visits() - before, 14u);
    };
    refresh();
    const auto queries = [&] {
        const auto before = cache.query_visits();
        for (int x = 0; x < kCells; ++x) {
            for (int z = 0; z < kCells; ++z) {
                EXPECT_EQ(cache.Allows(owner_of(x, z)), schedule.CellDue(x, z));
            }
        }
        return cache.query_visits() - before;
    };
    const auto work = queries();
    EXPECT_LE(work, 4u * kCells * kCells);
    due.clear();
    // A changed global schedule cannot leak into queries until refresh.
    EXPECT_EQ(queries(), work);
    refresh();
    EXPECT_FALSE(cache.Allows(owner_of(0, 0)));
    due.push_back(schedule.Owner(kCells - 1, kCells - 1));
    refresh();
    EXPECT_TRUE(cache.Allows(owner_of(kCells - 1, kCells - 1)));
    EXPECT_FALSE(cache.Allows(owner_of(0, 0)));
    const auto shifted_owner = [](int, int) {
        return RegionKey{1000, -1000};
    };
    const std::set<RegionKey> shifted_index(due.begin(), due.end());
    cache.Refresh(
        kCells, shifted_owner, [&](RegionKey owner) { return shifted_index.contains(owner); });
    EXPECT_FALSE(cache.Allows(shifted_owner(0, 0)));
    due.push_back(shifted_owner(0, 0));
    const std::set<RegionKey> refreshed_index(due.begin(), due.end());
    cache.Refresh(
        kCells, shifted_owner, [&](RegionKey owner) { return refreshed_index.contains(owner); });
    EXPECT_TRUE(cache.Allows(shifted_owner(0, 0)));
    EXPECT_FALSE(cache.Allows(owner_of(0, 0)));
}

} // namespace

TEST(ScentFieldGates, NearDueQueriesStayBoundedAsGlobalScheduleGrows) {
    for (const std::size_t unrelated : {std::size_t{0}, std::size_t{128}, std::size_t{8192}}) {
        ASSERT_NO_FATAL_FAILURE(CheckNearDueQueryCost(unrelated));
    }
}

TEST(ScentFieldGates, DepositSkipsFrozenEmittersAndEmptyGateRestoresEligibility) {
    namespace Comp = Luminumbra::Components;
    entt::registry registry;
    ScentField field(4, 3);
    for (const int x : {1, 2}) {
        const auto entity = registry.create();
        registry.emplace<Comp::TransformComponent>(entity).position =
            Luminumbra::Vec3(static_cast<float>(x), 0.0f, 1.0f);
        auto& emitter = registry.emplace<Comp::SensableComponent>(entity);
        emitter.scent_channel = 0;
        emitter.scent_deposit = 3.5f;
    }
    field.BindDepositGate([](int x, int) { return x < 2; });
    EXPECT_EQ(luminumbra::ai::RunScentDepositOnTick(registry, field, 0.0f, 0.0f, 1.0f).emitters,
              1u);
    EXPECT_DOUBLE_EQ(field.Sample(0, 1, 1), 3.5);
    EXPECT_DOUBLE_EQ(field.Sample(0, 2, 1), 0.0);
    field.BindDepositGate({});
    EXPECT_TRUE(field.SimulationCellAllowed(-1, -1));
    EXPECT_EQ(luminumbra::ai::RunScentDepositOnTick(registry, field, 0.0f, 0.0f, 1.0f).emitters,
              2u);
    EXPECT_DOUBLE_EQ(field.Sample(0, 1, 1), 7.0);
    EXPECT_DOUBLE_EQ(field.Sample(0, 2, 1), 3.5);
}

TEST(ScentFieldGates, SteeringLeavesFrozenIntentUntouched) {
    namespace Comp = Luminumbra::Components;
    entt::registry registry;
    ScentField field(4, 3);
    for (int z = 0; z < 3; ++z) {
        for (int x = 0; x < 4; ++x) {
            field.Deposit(0, x, z, static_cast<double>(x) * 8.0);
        }
    }
    std::array<entt::entity, 2> sniffers{};
    for (std::size_t index = 0; index < sniffers.size(); ++index) {
        const auto entity = registry.create();
        sniffers[index] = entity;
        registry.emplace<Comp::TransformComponent>(entity).position =
            Luminumbra::Vec3(static_cast<float>(index + 1), 0.0f, 1.0f);
        registry.emplace<Comp::ScentSenseComponent>(entity).channel = 0;
        registry.emplace<Comp::LocomotionProfile>(entity);
        registry.emplace<Comp::LocomotionIntentComponent>(entity).wish_xz =
            Luminumbra::Vec2(0.0f, 0.5f);
    }
    field.BindDepositGate([](int x, int) { return x < 2; });
    const auto stats = luminumbra::ai::RunScentSteeringOnTick(registry, field, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(stats.considered, 2u);
    EXPECT_EQ(stats.steered, 1u);
    EXPECT_GT(registry.get<Comp::LocomotionIntentComponent>(sniffers[0]).wish_xz.x, 0.0f);
    const auto& frozen_intent = registry.get<Comp::LocomotionIntentComponent>(sniffers[1]);
    EXPECT_FLOAT_EQ(frozen_intent.wish_xz.x, 0.0f);
    EXPECT_FLOAT_EQ(frozen_intent.wish_xz.y, 0.5f);
    field.BindDepositGate({});
    EXPECT_EQ(luminumbra::ai::RunScentSteeringOnTick(registry, field, 0.0f, 0.0f, 1.0f).steered,
              2u);
    EXPECT_GT(registry.get<Comp::LocomotionIntentComponent>(sniffers[1]).wish_xz.x, 0.0f);
}

TEST(ScentFieldGates, ForagingSkipsFrozenForagersAndFood) {
    namespace Comp = Luminumbra::Components;
    entt::registry registry;
    ScentField field(8, 3, 4);
    const auto active_ant = registry.create();
    registry.emplace<Comp::ForagerComponent>(active_ant) = {3, 1, 0, 1, false, 0};
    const auto frozen_ant = registry.create();
    registry.emplace<Comp::ForagerComponent>(frozen_ant) = {5, 1, 5, 1, true, 7};
    const auto active_food = registry.create();
    registry.emplace<Comp::FoodSourceComponent>(active_food) = {0, 1, 3};
    const auto frozen_food = registry.create();
    registry.emplace<Comp::FoodSourceComponent>(frozen_food) = {5, 1, 3};
    field.BindDepositGate([](int x, int) { return x < 5; });
    const auto stats = luminumbra::ai::RunForagingOnTick(registry, field);
    EXPECT_EQ(stats.foragers, 1u);
    EXPECT_EQ(stats.pickups, 0u);
    EXPECT_EQ(stats.deliveries, 0u);
    // The frozen food is closer, but the active forager must head toward x=0.
    const auto& active_state = registry.get<Comp::ForagerComponent>(active_ant);
    EXPECT_EQ(active_state.cell_x, 2);
    EXPECT_EQ(active_state.cell_z, 1);
    EXPECT_FALSE(active_state.carrying_food);
    const auto& frozen_state = registry.get<Comp::ForagerComponent>(frozen_ant);
    EXPECT_EQ(frozen_state.cell_x, 5);
    EXPECT_EQ(frozen_state.cell_z, 1);
    EXPECT_TRUE(frozen_state.carrying_food);
    EXPECT_EQ(frozen_state.deliveries, 7u);
    EXPECT_EQ(registry.get<Comp::FoodSourceComponent>(active_food).amount, 3);
    EXPECT_EQ(registry.get<Comp::FoodSourceComponent>(frozen_food).amount, 3);
    EXPECT_DOUBLE_EQ(field.Sample(luminumbra::ai::kHomeTrailChannel, 3, 1), 1.0);
    EXPECT_DOUBLE_EQ(field.Sample(luminumbra::ai::kFoodTrailChannel, 5, 1), 0.0);
}

TEST(ScentFieldGates, ForagingCannotFollowAStrongTrailIntoAFrozenCell) {
    namespace Comp = Luminumbra::Components;
    entt::registry registry;
    ScentField field(8, 3, 4);
    const auto ant = registry.create();
    registry.emplace<Comp::ForagerComponent>(ant) = {4, 1, 0, 1, false, 0};
    field.Deposit(luminumbra::ai::kFoodTrailChannel, 5, 1, 100.0);
    field.BindDepositGate([](int x, int) { return x < 5; });
    const auto stats = luminumbra::ai::RunForagingOnTick(registry, field);
    EXPECT_EQ(stats.foragers, 1u);
    EXPECT_EQ(registry.get<Comp::ForagerComponent>(ant).cell_x, 3);
    EXPECT_EQ(registry.get<Comp::ForagerComponent>(ant).cell_z, 1);
    EXPECT_DOUBLE_EQ(field.Sample(luminumbra::ai::kFoodTrailChannel, 5, 1), 100.0);
}

namespace {
void CheckBrainScentEligibility(RegionState state, bool due) {
    namespace Comp = Luminumbra::Components;
    LiveSchedule schedule({100, 100});
    const auto owner = schedule.Owner(16, 16);
    schedule.Set(owner, state, state == RegionState::Reduced ? std::uint8_t{2} : std::uint8_t{0});
    const auto phase = ActiveRegionLedger::phase(owner, 2);
    schedule.Reschedule(due ? 32 + phase : 33 + phase);
    if (state == RegionState::Frozen) {
        due = false;
    }
    ScentField field(kCells, kCells, 4);
    for (int x = 0; x < kCells; ++x) {
        for (int z = 0; z < kCells; ++z) {
            field.RestoreCell(0, x, z, static_cast<double>(x + 1));
        }
    }
    field.BindDepositGate([&](int x, int z) { return schedule.CellDue(x, z); });
    entt::registry sampled, cold;
    const auto spawn = [](entt::registry& registry) {
        const auto entity = registry.create();
        registry.emplace<Comp::TransformComponent>(entity).position = {116, 0, 116};
        auto& creature = registry.emplace<Comp::CreatureComponent>(entity);
        creature.is_predator = true;
        creature.hunger = 0.95f;
        return entity;
    };
    const auto animal = spawn(sampled);
    const auto control = spawn(cold);
    luminumbra::ai::RunCreatureBrainSystemOnTick(sampled, 1.0f / 30.0f, {}, &field, 100, 100, 1);
    luminumbra::ai::RunCreatureBrainSystemOnTick(cold, 1.0f / 30.0f);
    const auto& actual = sampled.get<Comp::CreatureComponent>(animal);
    const auto& expected = cold.get<Comp::CreatureComponent>(control);
    if (due) {
        EXPECT_EQ(actual.last_action, static_cast<int>(luminumbra::ai::CreatureAction::Hunt));
        EXPECT_GT(actual.wish_x, 0.0f);
    } else {
        EXPECT_EQ(actual.last_action, expected.last_action);
        EXPECT_FLOAT_EQ(actual.wish_x, expected.wish_x);
        EXPECT_FLOAT_EQ(actual.wish_z, expected.wish_z);
        EXPECT_EQ(sampled.get<Comp::TransformComponent>(animal).position,
                  cold.get<Comp::TransformComponent>(control).position);
    }
}
} // namespace

TEST(ScentFieldGates, CreatureBrainSamplesOnlyDueUnfrozenOwners) {
    ASSERT_NO_FATAL_FAILURE(CheckBrainScentEligibility(RegionState::Active, true));
    ASSERT_NO_FATAL_FAILURE(CheckBrainScentEligibility(RegionState::Reduced, true));
    ASSERT_NO_FATAL_FAILURE(CheckBrainScentEligibility(RegionState::Reduced, false));
    ASSERT_NO_FATAL_FAILURE(CheckBrainScentEligibility(RegionState::Frozen, false));
}

namespace {
constexpr std::array<std::pair<int, int>, 4> kScentDirections{
    std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

void CheckBoundaryScentConsumers(const std::pair<int, int>& direction,
                                 RegionState source_state,
                                 bool source_due) {
    namespace Comp = Luminumbra::Components;
    const auto [dx, dz] = direction;
    SCOPED_TRACE(testing::Message() << "direction=(" << dx << ',' << dz << ") state="
                                    << static_cast<int>(source_state) << " due=" << source_due);
    // The window starts at world cell 480; the owner boundary is at local cell 32.
    const int x = dx == 0 ? 16 : (dx > 0 ? 31 : 32);
    const int z = dz == 0 ? 16 : (dz > 0 ? 31 : 32);
    LiveSchedule schedule({480, 480});
    const auto source_owner = schedule.Owner(x + dx, z + dz);
    schedule.Set(source_owner, source_state, std::uint8_t{2});
    const auto due_tick = 32 + ActiveRegionLedger::phase(source_owner, 2);
    schedule.Reschedule(source_due ? due_tick : due_tick + 1);
    ASSERT_NE(schedule.Owner(x, z), source_owner);
    ASSERT_TRUE(schedule.CellDue(x, z));
    ASSERT_EQ(schedule.CellDue(x + dx, z + dz), source_due);

    ScentField field(kCells, kCells);
    field.RestoreCell(0, x + dx, z + dz, 8.0); // Only the neighbouring owner has scent.
    field.BindDepositGate([&schedule](int cx, int cz) { return schedule.CellDue(cx, cz); });
    float gx = 1.0f, gz = 1.0f;
    const float confidence = field.GradientSteer(0, x, z, 1.0f, 1e-4f, 0.05f, gx, gz);
    EXPECT_FLOAT_EQ(confidence, source_due ? 8.0f / 8.05f : 0.0f);
    EXPECT_FLOAT_EQ(gx, source_due ? static_cast<float>(dx) : 0.0f);
    EXPECT_FLOAT_EQ(gz, source_due ? static_cast<float>(dz) : 0.0f);

    entt::registry sampled, cold;
    const auto spawn = [x, z](entt::registry& registry) {
        const auto entity = registry.create();
        registry.emplace<Comp::TransformComponent>(entity).position = {
            static_cast<float>(480 + x), 0.0f, static_cast<float>(480 + z)};
        auto& creature = registry.emplace<Comp::CreatureComponent>(entity);
        creature.is_predator = true;
        creature.hunger = 0.95f;
        return entity;
    };
    const auto animal = spawn(sampled);
    const auto control = spawn(cold);
    luminumbra::ai::RunCreatureBrainSystemOnTick(sampled, 1.0f / 30.0f, {}, &field, 480, 480, 1);
    luminumbra::ai::RunCreatureBrainSystemOnTick(cold, 1.0f / 30.0f);
    const auto& actual = sampled.get<Comp::CreatureComponent>(animal);
    const auto& expected = cold.get<Comp::CreatureComponent>(control);
    if (source_due) {
        EXPECT_EQ(actual.last_action, static_cast<int>(luminumbra::ai::CreatureAction::Hunt));
        EXPECT_GT(actual.wish_x * static_cast<float>(dx) + actual.wish_z * static_cast<float>(dz),
                  0.0f);
        EXPECT_FLOAT_EQ(
            actual.wish_x * static_cast<float>(dz) - actual.wish_z * static_cast<float>(dx), 0.0f);
    } else {
        EXPECT_EQ(actual.last_action, expected.last_action);
        EXPECT_FLOAT_EQ(actual.wish_x, expected.wish_x);
        EXPECT_FLOAT_EQ(actual.wish_z, expected.wish_z);
        EXPECT_EQ(sampled.get<Comp::TransformComponent>(animal).position,
                  cold.get<Comp::TransformComponent>(control).position);
    }

    // A fresh predator exercises the independent locomotion-intent consumer.
    entt::registry steering;
    const auto sniffer = spawn(steering);
    steering.emplace<Comp::ScentSenseComponent>(sniffer).channel = 0;
    steering.emplace<Comp::LocomotionProfile>(sniffer).move_speed = 2.0f;
    steering.emplace<Comp::LocomotionIntentComponent>(sniffer).wish_xz = {0.0f, 0.0f};
    const auto stats = luminumbra::ai::RunScentSteeringOnTick(steering, field, 480, 480, 1);
    const auto& wish = steering.get<Comp::LocomotionIntentComponent>(sniffer).wish_xz;
    EXPECT_EQ(stats.considered, 1u);
    EXPECT_EQ(stats.steered, source_due ? 1u : 0u);
    // Magnitude 8, default Weber k=1 and speed=2 give a bias of 16/9, below the cap.
    EXPECT_FLOAT_EQ(wish.x, source_due ? static_cast<float>(dx) * (16.0f / 9.0f) : 0.0f);
    EXPECT_FLOAT_EQ(wish.y, source_due ? static_cast<float>(dz) * (16.0f / 9.0f) : 0.0f);
    EXPECT_DOUBLE_EQ(field.Sample(0, x + dx, z + dz), 8.0);
}

void CheckGradientCentreSubstitute(const std::pair<int, int>& direction) {
    const auto [dx, dz] = direction;
    SCOPED_TRACE(testing::Message() << "direction=(" << dx << ',' << dz << ')');
    ScentField field(3, 3);
    for (int z = 0; z < 3; ++z) {
        for (int x = 0; x < 3; ++x) {
            field.RestoreCell(0, x, z, 4.0);
        }
    }
    field.RestoreCell(0, 1 + dx, 1 + dz, 20.0);
    std::vector<std::pair<int, int>> visits;
    field.BindDepositGate([&](int x, int z) {
        visits.emplace_back(x, z);
        return x != 1 + dx || z != 1 + dz;
    });
    float gx = 1.0f, gz = 1.0f;
    EXPECT_DOUBLE_EQ(field.Gradient(0, 1, 1, gx, gz), 0.0);
    EXPECT_FLOAT_EQ(gx, 0.0f);
    EXPECT_FLOAT_EQ(gz, 0.0f);
    // Exactly the four stencil neighbours are queried, independent of world size.
    std::sort(visits.begin(), visits.end());
    const std::vector<std::pair<int, int>> expected{{0, 1}, {1, 0}, {1, 2}, {2, 1}};
    EXPECT_EQ(visits, expected);
}

void CheckOpenGradientBits(int width, int height) {
    ScentField ungated(width, height, 2);
    for (int ch = 0; ch < 2; ++ch) {
        for (int z = 0; z < height; ++z) {
            for (int x = 0; x < width; ++x) {
                ungated.RestoreCell(ch, x, z, static_cast<double>(x * 17 + z * 31 + ch) / 37.0);
            }
        }
    }
    auto gated = ungated;
    int queries = 0;
    bool outside = false;
    gated.BindDepositGate([&](int x, int z) {
        ++queries;
        outside = outside || !gated.in_bounds(x, z);
        return gated.in_bounds(x, z);
    });
    for (int ch = -1; ch <= 2; ++ch) {
        for (int z = -1; z <= height; ++z) {
            for (int x = -1; x <= width; ++x) {
                SCOPED_TRACE(testing::Message() << "size=(" << width << ',' << height << ") ch="
                                                << ch << " cell=(" << x << ',' << z << ')');
                float ux = 1.0f, uz = 1.0f, gx = 1.0f, gz = 1.0f;
                const auto um = ungated.Gradient(ch, x, z, ux, uz);
                const auto gm = gated.Gradient(ch, x, z, gx, gz);
                EXPECT_EQ(std::bit_cast<std::uint64_t>(um), std::bit_cast<std::uint64_t>(gm));
                EXPECT_EQ(std::bit_cast<std::uint32_t>(ux), std::bit_cast<std::uint32_t>(gx));
                EXPECT_EQ(std::bit_cast<std::uint32_t>(uz), std::bit_cast<std::uint32_t>(gz));
            }
        }
    }
    EXPECT_FALSE(outside); // Eligibility is queried after the existing edge clamp.
    EXPECT_EQ(queries, 2 * width * height * 4); // Invalid inputs do not consult the gate.
}
} // namespace

TEST(ScentFieldGates, ConsumersIgnoreFrozenNeighbourScent) {
    for (const auto& direction : kScentDirections) {
        ASSERT_NO_FATAL_FAILURE(CheckBoundaryScentConsumers(direction, RegionState::Frozen, false));
    }
}

TEST(ScentFieldGates, ConsumersFollowNeighbourScentOnlyWhenItsOwnerIsDue) {
    for (const auto& direction : kScentDirections) {
        ASSERT_NO_FATAL_FAILURE(
            CheckBoundaryScentConsumers(direction, RegionState::Reduced, false));
        ASSERT_NO_FATAL_FAILURE(CheckBoundaryScentConsumers(direction, RegionState::Reduced, true));
    }
}

TEST(ScentFieldGates, BlockedGradientNeighboursUseCentreConcentrationWithBoundedQueries) {
    for (const auto& direction : kScentDirections) {
        ASSERT_NO_FATAL_FAILURE(CheckGradientCentreSubstitute(direction));
    }
}

TEST(ScentFieldGates, OpenGradientMatchesUngatedBitsAtEdgesAndForInvalidInputs) {
    ASSERT_NO_FATAL_FAILURE(CheckOpenGradientBits(3, 3));
    ASSERT_NO_FATAL_FAILURE(CheckOpenGradientBits(1, 3));
    ASSERT_NO_FATAL_FAILURE(CheckOpenGradientBits(3, 1));
    ASSERT_NO_FATAL_FAILURE(CheckOpenGradientBits(1, 1));
}
