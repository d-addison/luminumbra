#include "luminumbra_common/ai/CreatureCoarse.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace {
namespace A = luminumbra::ai;
namespace C = Luminumbra::Components;
using Luminumbra::world::ActiveRegionLedger;
using Luminumbra::world::RegionKey;
using Luminumbra::world::RegionState;
constexpr std::array<RegionKey, 1> kDue{{{0, 0}}};

A::CoarseRegionRecords Regions(std::span<const RegionKey> keys, std::uint8_t shift = 0) {
    A::CoarseRegionRecords regions;
    for (const auto key : keys) {
        auto& record = regions[key];
        record.state = shift == 0 ? RegionState::Active : RegionState::Reduced;
        record.cadence_shift = shift;
        record.cadence_phase = ActiveRegionLedger::phase(key, shift);
    }
    return regions;
}

A::CoarseCreature Creature(std::uint64_t id = 1) {
    A::CoarseCreature c;
    c.region.persistent_id = id;
    c.coarse.tier = C::CreatureTier::Coarse;
    c.coarse.x_mm = c.coarse.z_mm = 100000;
    c.species_id = 123;
    return c;
}

A::CoarseStepResult Step(const A::CoarseCreature& creature, std::uint64_t tick = 1) {
    const std::array previous{creature};
    return A::StepCoarseCreatures(previous, kDue, Regions(kDue), 0, tick);
}

TEST(CreatureCoarse, OwnershipUsesFloorAndChecksBeforeNarrowing) {
    EXPECT_EQ(A::CreatureRegionAtMillimetres(-1, -512000), (RegionKey{-1, -1}));
    EXPECT_EQ(A::CreatureRegionAtMillimetres(-512001, 511999), (RegionKey{-2, 0}));
    EXPECT_EQ(A::CreatureRegionAtMillimetres(-16777216000ll, 16777215999ll),
              (RegionKey{-32768, 32767}));
    EXPECT_THROW(A::CreatureRegionAtMillimetres(16777216000ll, 0), std::invalid_argument);
    EXPECT_THROW(A::CreatureRegionAtMillimetres(std::numeric_limits<std::int64_t>::min(), 0),
                 std::invalid_argument);
    auto c = Creature();
    c.coarse.x_mm = -1;
    EXPECT_THROW(Step(c), std::invalid_argument);
    c.region.owner_x = -1;
    const std::array due{RegionKey{-1, 0}};
    const auto result = A::StepCoarseCreatures(std::array{c}, due, Regions(due), 0, 1);
    EXPECT_EQ(result.creatures.front().coarse.age_cadences, 1u);
}

TEST(CreatureCoarse, NeedsAndCadenceCountersSaturateWithoutOverflow) {
    auto c = Creature();
    c.policy.coarse_need_rate_ppm = std::numeric_limits<std::uint32_t>::max();
    c.coarse.hunger_ppm = 999999;
    c.coarse.thirst_ppm = 1;
    c.coarse.needs = {{"rest", 100}, {"safety", 0}};
    c.coarse.age_cadences = std::numeric_limits<std::uint64_t>::max();
    c.coarse.reproduce_cadences = std::numeric_limits<std::uint64_t>::max();
    c.coarse.reproduce_cooldown_cadences = 1;
    const auto result = Step(c);
    const auto& next = result.creatures.front().coarse;
    EXPECT_EQ(next.hunger_ppm, 1000000u);
    EXPECT_EQ(next.thirst_ppm, 1000000u);
    EXPECT_EQ(next.needs[0].pressure_ppm, 1000000u);
    EXPECT_EQ(next.needs[1].pressure_ppm, 1000000u);
    EXPECT_EQ(next.age_cadences, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(next.reproduce_cadences, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(next.reproduce_cooldown_cadences, 0u);
    EXPECT_EQ(c.coarse.hunger_ppm, 999999u); // Input was not mutated.
    ASSERT_EQ(result.work.size(), 1u);
    EXPECT_EQ(result.work[0].entity_updates, 1u);
}

TEST(CreatureCoarse, OneDueStepIsOneCadenceAndFrozenTimeContributesZero) {
    auto c = Creature();
    c.policy.coarse_need_rate_ppm = 7;
    c.coarse.reproduce_cooldown_cadences = 9;
    c.coarse.courting_cadences = 2;
    const auto result = Step(c, 36000);
    const auto& next = result.creatures.front();
    EXPECT_EQ(next.coarse.age_cadences, 1u);
    EXPECT_EQ(next.coarse.reproduce_cadences, 1u);
    EXPECT_EQ(next.coarse.reproduce_cooldown_cadences, 8u);
    EXPECT_EQ(next.coarse.courting_cadences, 0u);
    EXPECT_EQ(next.coarse.hunger_ppm, 7u);
    EXPECT_EQ(next.region.region_stamp, 36000u);
    const auto frozen = A::StepCoarseCreatures(result.creatures, {}, Regions({}), 0, 72000);
    EXPECT_EQ(frozen.creatures.front().coarse.age_cadences, 1u);
    EXPECT_EQ(frozen.creatures.front().coarse.hunger_ppm, 7u);
    EXPECT_EQ(frozen.creatures.front().region.region_stamp, 36000u);
    EXPECT_TRUE(frozen.work.empty());
    EXPECT_TRUE(frozen.births.empty());
    EXPECT_TRUE(frozen.deaths.empty());
    EXPECT_EQ(Step(frozen.creatures.front(), 72001).creatures.front().coarse.age_cadences, 2u);
}

TEST(CreatureCoarse, FullDeadAndAlreadyStampedCreaturesDoNotAdvance) {
    for (int mode = 0; mode < 3; ++mode) {
        auto c = Creature();
        c.policy.coarse_need_rate_ppm = 100;
        if (mode == 0)
            c.coarse.tier = C::CreatureTier::Full;
        if (mode == 1)
            c.alive = false;
        if (mode == 2)
            c.region.region_stamp = 1;
        const auto result = Step(c);
        EXPECT_EQ(result.creatures.front().coarse.age_cadences, 0u);
        EXPECT_EQ(result.creatures.front().coarse.hunger_ppm, 0u);
        EXPECT_TRUE(result.work.empty());
    }
}

TEST(CreatureCoarse, ZeroRatesDisableNeedsTravelDeathAndReproduction) {
    auto c = Creature();
    c.policy.promote_distance_m = 0;
    c.has_genome = true;
    c.coarse.age_cadences = 1000;
    c.coarse.reproduce_cadences = 1000;
    c.coarse.hunger_ppm = 12;
    c.coarse.thirst_ppm = 34;
    c.coarse.needs = {{"rest", 56}};
    c.coarse.travel_pending = true;
    c.coarse.destination_x = 1;
    const auto result = Step(c);
    const auto& next = result.creatures.front();
    EXPECT_TRUE(next.alive);
    EXPECT_EQ(next.coarse.hunger_ppm, 12u);
    EXPECT_EQ(next.coarse.thirst_ppm, 34u);
    EXPECT_EQ(next.coarse.needs[0].pressure_ppm, 56u);
    EXPECT_EQ(next.coarse.x_mm, c.coarse.x_mm);
    EXPECT_TRUE(next.coarse.travel_pending);
    EXPECT_EQ(next.coarse.transition, C::CreatureTransition::None);
    EXPECT_TRUE(result.births.empty());
    EXPECT_TRUE(result.deaths.empty());
}

TEST(CreatureCoarse, LifespanOwnsOneDeathAndStopsTravel) {
    auto c = Creature();
    c.policy.lifespan_cadences = 2;
    c.policy.coarse_travel_m_per_cadence = 100;
    c.coarse.age_cadences = 1;
    const auto result = Step(c);
    ASSERT_EQ(result.deaths, std::vector<std::uint64_t>{1});
    EXPECT_FALSE(result.creatures.front().alive);
    EXPECT_EQ(result.creatures.front().coarse.x_mm, c.coarse.x_mm);
    EXPECT_TRUE(Step(result.creatures.front(), 2).deaths.empty());
}

TEST(CreatureCoarse, FrozenBorderParksAndThawTransfersWithoutDoubleUpdate) {
    auto c = Creature();
    c.policy.coarse_travel_m_per_cadence = 10;
    c.policy.coarse_need_rate_ppm = 9;
    c.coarse.x_mm = 511000;
    c.coarse.travel_pending = true;
    c.coarse.destination_x = 1;
    const auto parked = Step(c);
    const auto& wait = parked.creatures.front();
    EXPECT_EQ(wait.coarse.x_mm, 511999);
    EXPECT_TRUE(wait.coarse.travel_pending);
    EXPECT_EQ(wait.coarse.destination_x, 1);
    EXPECT_EQ(wait.region.owner_x, 0);
    const std::array both{RegionKey{1, 0}, RegionKey{0, 0}, RegionKey{1, 0}};
    const auto transferred = A::StepCoarseCreatures(parked.creatures, both, Regions(both), 0, 2);
    const auto& moved = transferred.creatures.front();
    EXPECT_EQ(moved.coarse.x_mm, 521999);
    EXPECT_EQ(moved.region.owner_x, 1);
    EXPECT_FALSE(moved.coarse.travel_pending);
    EXPECT_EQ(moved.coarse.age_cadences, 2u);
    EXPECT_EQ(moved.coarse.hunger_ppm, 18u);
    EXPECT_EQ(transferred.work.front().key, (RegionKey{0, 0}));
    EXPECT_EQ(transferred.work.front().entity_updates, 1u);
    const auto repeated = A::StepCoarseCreatures(transferred.creatures, both, Regions(both), 0, 2);
    EXPECT_EQ(repeated.creatures.front().coarse.age_cadences, 2u);
    EXPECT_EQ(repeated.creatures.front().coarse.x_mm, 521999);
    EXPECT_TRUE(repeated.work.empty());
}

TEST(CreatureCoarse, DifferentCadencePhasesAllowTravelAndAdvanceTheNextWindow) {
    const RegionKey source{0, 0}, destination{1, 0};
    const std::array owners{source, destination};
    const auto regions = Regions(owners, 1);
    ASSERT_EQ(regions.at(source).cadence_phase, 1u);
    ASSERT_EQ(regions.at(destination).cadence_phase, 0u);
    auto c = Creature();
    c.policy.coarse_travel_m_per_cadence = 10;
    c.policy.coarse_need_rate_ppm = 9;
    c.coarse.x_mm = 511000;
    c.coarse.travel_pending = true;
    c.coarse.destination_x = destination.x;

    // The destination exists and is Reduced, but only the source is due.
    const auto transferred = A::StepCoarseCreatures(std::array{c}, kDue, regions, 0, 1);
    const auto& moved = transferred.creatures.front();
    EXPECT_EQ(moved.region.owner_x, destination.x);
    EXPECT_EQ(moved.coarse.x_mm, 521000);
    EXPECT_FALSE(moved.coarse.travel_pending);
    EXPECT_EQ(moved.region.region_stamp, 1u);
    EXPECT_EQ(moved.coarse.age_cadences, 1u);
    EXPECT_EQ(moved.coarse.hunger_ppm, 9u);
    ASSERT_EQ(transferred.work.size(), 1u);
    EXPECT_EQ(transferred.work.front().key, source);
    EXPECT_EQ(transferred.work.front().entity_updates, 1u);

    auto previous = transferred.creatures;
    previous.front().policy.coarse_travel_m_per_cadence = 0;
    const std::array destination_due{destination};
    // Tick 2 begins the next cadence window, even though it is the following tick.
    const auto next = A::StepCoarseCreatures(previous, destination_due, regions, 0, 2);
    EXPECT_EQ(next.creatures.front().coarse.age_cadences, 2u);
    EXPECT_EQ(next.creatures.front().coarse.hunger_ppm, 18u);
    EXPECT_EQ(next.creatures.front().region.region_stamp, 2u);
    ASSERT_EQ(next.work.size(), 1u);
    EXPECT_EQ(next.work.front().key, destination);
    const auto repeated = A::StepCoarseCreatures(next.creatures, destination_due, regions, 0, 2);
    EXPECT_EQ(repeated.creatures.front().coarse.age_cadences, 2u);
    EXPECT_TRUE(repeated.work.empty());
    const auto off_due = A::StepCoarseCreatures(next.creatures, kDue, regions, 0, 3);
    EXPECT_EQ(off_due.creatures.front().coarse.age_cadences, 2u);
    EXPECT_TRUE(off_due.work.empty());
    const auto following =
        A::StepCoarseCreatures(off_due.creatures, destination_due, regions, 0, 4);
    EXPECT_EQ(following.creatures.front().coarse.age_cadences, 3u);
    EXPECT_EQ(following.creatures.front().coarse.hunger_ppm, 27u);
}

TEST(CreatureCoarse, TransferStampSuppressesLaterPhaseButDoesNotSkipNextCadence) {
    const RegionKey source{1, 0}, destination{0, 0};
    const std::array owners{source, destination};
    const auto regions = Regions(owners, 1);
    auto c = Creature();
    c.region.owner_x = source.x;
    c.coarse.x_mm = 513000;
    c.policy.coarse_travel_m_per_cadence = 10;
    c.policy.coarse_need_rate_ppm = 9;
    c.coarse.travel_pending = true;
    c.coarse.destination_x = destination.x;
    const std::array source_due{source};
    auto transferred = A::StepCoarseCreatures(std::array{c}, source_due, regions, 0, 2);
    auto& moved = transferred.creatures.front();
    ASSERT_EQ(moved.region.owner_x, destination.x);
    EXPECT_EQ(moved.coarse.x_mm, 503000);
    EXPECT_EQ(moved.coarse.age_cadences, 1u);
    moved.policy.coarse_travel_m_per_cadence = 0;

    // The destination's due tick is later in the same [2,3] cadence window.
    const auto later = A::StepCoarseCreatures(transferred.creatures, kDue, regions, 0, 3);
    EXPECT_EQ(later.creatures.front().coarse.age_cadences, 1u);
    EXPECT_EQ(later.creatures.front().coarse.reproduce_cadences, 1u);
    EXPECT_EQ(later.creatures.front().coarse.hunger_ppm, 9u);
    EXPECT_EQ(later.creatures.front().coarse.thirst_ppm, 9u);
    EXPECT_EQ(later.creatures.front().coarse.x_mm, 503000);
    EXPECT_EQ(later.creatures.front().region.region_stamp, 2u);
    EXPECT_TRUE(later.work.empty());
    const auto next = A::StepCoarseCreatures(later.creatures, kDue, regions, 0, 5);
    EXPECT_EQ(next.creatures.front().coarse.age_cadences, 2u);
    EXPECT_EQ(next.creatures.front().coarse.hunger_ppm, 18u);
    EXPECT_EQ(next.creatures.front().region.region_stamp, 5u);
    ASSERT_EQ(next.work.size(), 1u);
    EXPECT_EQ(next.work.front().key, destination);
}

TEST(CreatureCoarse, FrozenAndAbsentDestinationsParkUntilAvailableEvenWhenOffDue) {
    const RegionKey source{0, 0}, destination{1, 0};
    const std::array owners{source, destination};
    for (const bool absent : {false, true}) {
        SCOPED_TRACE(absent);
        auto regions = Regions(owners, 1);
        if (absent)
            regions.erase(destination);
        else
            regions.at(destination).state = RegionState::Frozen;
        auto c = Creature();
        c.coarse.x_mm = 511000;
        c.coarse.travel_pending = true;
        c.coarse.destination_x = destination.x;
        c.policy.coarse_travel_m_per_cadence = 10;
        c.policy.coarse_need_rate_ppm = 9;
        const auto parked = A::StepCoarseCreatures(std::array{c}, kDue, regions, 0, 1);
        EXPECT_EQ(parked.creatures.front().region.owner_x, source.x);
        EXPECT_EQ(parked.creatures.front().coarse.x_mm, 511999);
        EXPECT_TRUE(parked.creatures.front().coarse.travel_pending);
        EXPECT_EQ(parked.creatures.front().coarse.destination_x, destination.x);
        const auto still_parked = A::StepCoarseCreatures(parked.creatures, kDue, regions, 0, 3);
        EXPECT_EQ(still_parked.creatures.front().region.owner_x, source.x);
        EXPECT_EQ(still_parked.creatures.front().coarse.x_mm, 511999);
        EXPECT_TRUE(still_parked.creatures.front().coarse.travel_pending);
        EXPECT_EQ(still_parked.creatures.front().coarse.age_cadences, 2u);
        // Admission/thaw opens the border without making the destination due.
        regions = Regions(owners, 1);
        const auto opened = A::StepCoarseCreatures(still_parked.creatures, kDue, regions, 0, 5);
        EXPECT_EQ(opened.creatures.front().region.owner_x, destination.x);
        EXPECT_EQ(opened.creatures.front().coarse.x_mm, 521999);
        EXPECT_FALSE(opened.creatures.front().coarse.travel_pending);
        EXPECT_EQ(opened.creatures.front().coarse.age_cadences, 3u);
    }
}

TEST(CreatureCoarse, PhasedTransfersIgnoreSnapshotDueAndRegionInsertionOrder) {
    const std::array owners{RegionKey{0, 0}, RegionKey{1, 0}, RegionKey{1, 1}, RegionKey{2, 1}};
    auto regions = Regions(owners, 1);
    std::vector<RegionKey> due;
    for (const auto key : owners)
        if (regions.at(key).cadence_phase == 1)
            due.push_back(key);
    ASSERT_EQ(due.size(), 2u);
    auto first = Creature();
    first.coarse.x_mm = 511000;
    first.coarse.travel_pending = true;
    first.coarse.destination_x = 1;
    first.policy.coarse_travel_m_per_cadence = 10;
    first.policy.coarse_need_rate_ppm = 9;
    auto second = first;
    second.region.persistent_id = 2;
    second.region.owner_x = second.region.owner_z = 1;
    second.coarse.x_mm = 1023000;
    second.coarse.z_mm = 612000;
    second.coarse.destination_x = 2;
    second.coarse.destination_z = 1;
    std::array previous{second, first};
    const auto result = A::StepCoarseCreatures(previous, due, regions, 987, 1);
    ASSERT_EQ(result.creatures.size(), 2u);
    EXPECT_EQ(result.creatures[0].region.owner_x, 1);
    EXPECT_EQ(result.creatures[1].region.owner_x, 2);
    std::reverse(previous.begin(), previous.end());
    std::reverse(due.begin(), due.end());
    due.push_back(due.front());
    A::CoarseRegionRecords reversed;
    for (auto it = regions.rbegin(); it != regions.rend(); ++it)
        reversed.insert(*it);
    const auto reordered = A::StepCoarseCreatures(previous, due, reversed, 987, 1);
    EXPECT_EQ(result.births, reordered.births);
    EXPECT_EQ(result.deaths, reordered.deaths);
    ASSERT_EQ(result.work.size(), reordered.work.size());
    for (std::size_t i = 0; i < result.work.size(); ++i) {
        EXPECT_EQ(result.work[i].key, reordered.work[i].key);
        EXPECT_EQ(result.work[i].entity_updates, reordered.work[i].entity_updates);
    }
    ASSERT_EQ(result.creatures.size(), reordered.creatures.size());
    for (std::size_t i = 0; i < result.creatures.size(); ++i) {
        const auto& a = result.creatures[i];
        const auto& b = reordered.creatures[i];
        EXPECT_EQ(a.region.persistent_id, b.region.persistent_id);
        EXPECT_EQ(a.region.owner_x, b.region.owner_x);
        EXPECT_EQ(a.region.owner_z, b.region.owner_z);
        EXPECT_EQ(a.region.region_stamp, b.region.region_stamp);
        EXPECT_EQ(a.coarse.x_mm, b.coarse.x_mm);
        EXPECT_EQ(a.coarse.z_mm, b.coarse.z_mm);
        EXPECT_EQ(a.coarse.age_cadences, b.coarse.age_cadences);
        EXPECT_EQ(a.coarse.reproduce_cadences, b.coarse.reproduce_cadences);
        EXPECT_EQ(a.coarse.hunger_ppm, b.coarse.hunger_ppm);
        EXPECT_EQ(a.coarse.thirst_ppm, b.coarse.thirst_ppm);
        EXPECT_EQ(a.coarse.travel_pending, b.coarse.travel_pending);
        EXPECT_EQ(a.coarse.destination_x, b.coarse.destination_x);
        EXPECT_EQ(a.coarse.destination_z, b.coarse.destination_z);
    }
}

TEST(CreatureCoarse, InvalidRegionAvailabilityOrDueCadenceRefuses) {
    const auto c = Creature();
    auto regions = Regions(kDue, 1);
    EXPECT_THROW(A::StepCoarseCreatures(std::array{c}, kDue, {}, 0, 1), std::invalid_argument);
    regions.begin()->second.state = RegionState::Frozen;
    EXPECT_THROW(A::StepCoarseCreatures(std::array{c}, kDue, regions, 0, 1), std::invalid_argument);
    regions.begin()->second.state = RegionState::Reduced;
    EXPECT_THROW(A::StepCoarseCreatures(std::array{c}, kDue, regions, 0, 2), std::invalid_argument);
    regions.begin()->second.cadence_shift = 17;
    EXPECT_THROW(A::StepCoarseCreatures(std::array{c}, kDue, regions, 0, 1), std::invalid_argument);
    regions.begin()->second.cadence_shift = 1;
    regions.begin()->second.cadence_phase = 2;
    EXPECT_THROW(A::StepCoarseCreatures(std::array{c}, kDue, regions, 0, 1), std::invalid_argument);
    EXPECT_EQ(c.region.region_stamp, 0u);
    EXPECT_EQ(c.coarse.x_mm, 100000);
}

TEST(CreatureCoarse, NegativeBorderAndLargeTravelCannotSkipSealedOwners) {
    auto c = Creature();
    c.coarse.x_mm = 0;
    c.coarse.travel_pending = true;
    c.coarse.destination_x = -1;
    c.policy.coarse_travel_m_per_cadence = std::numeric_limits<std::uint32_t>::max();
    const auto blocked = Step(c);
    EXPECT_EQ(blocked.creatures.front().coarse.x_mm, 0);
    EXPECT_TRUE(blocked.creatures.front().coarse.travel_pending);
    const std::array due{RegionKey{0, 0}, RegionKey{-1, 0}, RegionKey{-3, 0}};
    const auto moved = A::StepCoarseCreatures(std::array{c}, due, Regions(due), 0, 1);
    EXPECT_EQ(moved.creatures.front().region.owner_x, -1);
    EXPECT_EQ(moved.creatures.front().coarse.x_mm, -512000);
    EXPECT_EQ(moved.creatures.front().coarse.destination_x, -2);
}

TEST(CreatureCoarse, EveryCardinalBorderParksOnItsSourceSide) {
    const std::array destinations{
        RegionKey{1, 0}, RegionKey{-1, 0}, RegionKey{0, 1}, RegionKey{0, -1}};
    for (const auto destination : destinations) {
        auto c = Creature();
        c.policy.coarse_travel_m_per_cadence = 1000;
        c.coarse.travel_pending = true;
        c.coarse.destination_x = destination.x;
        c.coarse.destination_z = destination.z;
        const auto result = Step(c);
        const auto& next = result.creatures.front();
        EXPECT_TRUE(next.coarse.travel_pending);
        EXPECT_EQ(next.coarse.destination_x, destination.x);
        EXPECT_EQ(next.coarse.destination_z, destination.z);
        EXPECT_EQ(A::CreatureRegionAtMillimetres(next.coarse.x_mm, next.coarse.z_mm),
                  (RegionKey{0, 0}));
        if (destination.x != 0)
            EXPECT_EQ(next.coarse.x_mm, destination.x > 0 ? 511999 : 0);
        else
            EXPECT_EQ(next.coarse.z_mm, destination.z > 0 ? 511999 : 0);
    }
}

TEST(CreatureCoarse, WorldEdgesStayInsidePersistedDomain) {
    for (const bool positive : {false, true}) {
        auto c = Creature();
        c.region.owner_x = positive ? 32767 : -32768;
        c.coarse.x_mm = positive ? 16777215999ll : -16777216000ll;
        c.policy.coarse_travel_m_per_cadence = std::numeric_limits<std::uint32_t>::max();
        const std::array due{RegionKey{c.region.owner_x, 0}};
        // Version-one vectors: these seeds point east and west respectively.
        const auto result =
            A::StepCoarseCreatures(std::array{c}, due, Regions(due), positive ? 5 : 2, 1);
        EXPECT_EQ(result.creatures.front().coarse.x_mm, c.coarse.x_mm);
        EXPECT_FALSE(result.creatures.front().coarse.travel_pending);
        EXPECT_EQ(A::CreatureRegionAtMillimetres(result.creatures.front().coarse.x_mm,
                                                 result.creatures.front().coarse.z_mm),
                  due[0]);
    }
}

std::vector<A::CoarseCreature> MatingGroup() {
    std::vector<A::CoarseCreature> group;
    for (std::uint64_t id = 1; id <= 4; ++id) {
        auto c = Creature(id);
        c.has_genome = true;
        c.female = id <= 2;
        c.policy.reproduce_cadences = 5;
        c.coarse.age_cadences = A::kCoarseMaturityCadences;
        c.coarse.reproduce_cadences = 5;
        c.coarse.courting_cadences = A::kCoarseCourtshipCadences - 1;
        group.push_back(c);
    }
    return group;
}

TEST(CreatureCoarse, ReproductionUsesPreviousStatePairsAndPersistentOrdering) {
    auto previous = MatingGroup();
    const auto result = A::StepCoarseCreatures(previous, kDue, Regions(kDue), 987, 1);
    std::reverse(previous.begin(), previous.end());
    const auto reordered = A::StepCoarseCreatures(previous, kDue, Regions(kDue), 987, 1);
    EXPECT_EQ(result.births, reordered.births);
    ASSERT_EQ(result.births.size(), 2u);
    EXPECT_EQ(result.births[0].mother_id, 1u);
    EXPECT_EQ(result.births[0].father_id, 3u);
    EXPECT_EQ(result.births[1].mother_id, 2u);
    EXPECT_EQ(result.births[1].father_id, 4u);
    EXPECT_NE(result.births[0].seed, result.births[1].seed);
    EXPECT_EQ(result.creatures.size(), 4u); // Births are staged, never stepped here.
    for (const auto& c : result.creatures) {
        EXPECT_EQ(c.coarse.reproduce_cadences, 0u);
        EXPECT_EQ(c.coarse.reproduce_cooldown_cadences, 5u);
        EXPECT_EQ(c.coarse.courting_cadences, 0u);
    }
    EXPECT_TRUE(
        A::StepCoarseCreatures(result.creatures, kDue, Regions(kDue), 987, 2).births.empty());
}

TEST(CreatureCoarse, ReproductionRejectsUnreadyWrongSpeciesFrozenAndDistantPartners) {
    for (int mode = 0; mode < 15; ++mode) {
        SCOPED_TRACE(mode);
        auto group = MatingGroup();
        group.erase(group.begin() + 1);
        group.pop_back(); // One female and one male remain.
        auto& male = group.back();
        switch (mode) {
            case 0:
                male.species_id += 1;
                break;
            case 1:
                male.coarse.age_cadences = A::kCoarseMaturityCadences - 1;
                break;
            case 2:
                male.coarse.reproduce_cooldown_cadences = 1;
                break;
            case 3:
                male.coarse.hunger_ppm = male.mate_hunger_limit_ppm + 1;
                break;
            case 4:
                male.stamina_ppm = A::kCoarseHealthyStaminaPpm - 1;
                break;
            case 5:
                male.has_genome = false;
                break;
            case 6:
                male.coarse.x_mm += 2601;
                break;
            case 7:
                male.is_predator = true;
                break;
            case 8:
                male.policy.reproduce_cadences = 0;
                break;
            case 9:
                male.alive = false;
                break;
            case 10:
                male.region.owner_x = 1;
                male.coarse.x_mm = 512000;
                break;
            case 11:
                male.coarse.thirst_ppm = A::kCoarseMateThirstLimitPpm + 1;
                break;
            case 12:
                male.coarse.reproduce_cadences = 4;
                break;
            case 13:
                male.female = true;
                break;
            case 14:
                male.coarse.tier = C::CreatureTier::Full;
                break;
        }
        const auto result = A::StepCoarseCreatures(group, kDue, Regions(kDue), 0, 1);
        EXPECT_TRUE(result.births.empty());
        EXPECT_EQ(result.creatures.front().coarse.courting_cadences, 0u);
    }
}

TEST(CreatureCoarse, ReproductionSelectsNearestMaleBeforeIdentityTieBreak) {
    auto group = MatingGroup();
    group.erase(group.begin() + 1);
    group[1].coarse.x_mm += 1000;
    group[2].coarse.x_mm += 100;
    const auto result = A::StepCoarseCreatures(group, kDue, Regions(kDue), 0, 1);
    ASSERT_EQ(result.births.size(), 1u);
    EXPECT_EQ(result.births.front().father_id, 4u);
}

TEST(CreatureCoarse, ReproductionHasCourtshipAndUsesPreviousNeedsBeforeTravel) {
    auto group = MatingGroup();
    group.resize(1);
    const auto male = MatingGroup()[2];
    group.push_back(male);
    for (auto& c : group) {
        c.coarse.courting_cadences = 0;
        c.generation = std::numeric_limits<std::uint32_t>::max();
    }
    for (std::uint64_t tick = 1; tick < A::kCoarseCourtshipCadences; ++tick) {
        auto result = A::StepCoarseCreatures(group, kDue, Regions(kDue), 0, tick);
        EXPECT_TRUE(result.births.empty());
        group = result.creatures;
    }
    for (auto& c : group) {
        c.policy.coarse_need_rate_ppm = 1000000;
        c.policy.coarse_travel_m_per_cadence = 10;
    }
    const auto result =
        A::StepCoarseCreatures(group, kDue, Regions(kDue), 0, A::kCoarseCourtshipCadences);
    ASSERT_EQ(result.births.size(), 1u);
    EXPECT_EQ(result.births.front().generation, std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(result.births.front().x_mm, group.front().coarse.x_mm);
    EXPECT_EQ(result.creatures.front().coarse.hunger_ppm, 1000000u);
}

TEST(CreatureCoarse, InvalidSnapshotRefusesWithoutMutatingInput) {
    auto group = MatingGroup();
    group[1].region.persistent_id = group[0].region.persistent_id;
    EXPECT_THROW(A::StepCoarseCreatures(group, kDue, Regions(kDue), 0, 1), std::invalid_argument);
    EXPECT_EQ(group[0].region.region_stamp, 0u);
    auto c = Creature();
    c.coarse.needs = {{"rest", 1}, {"rest", 2}};
    EXPECT_THROW(Step(c), std::invalid_argument);
    c.coarse.needs.clear();
    c.coarse.hunger_ppm = 1000001;
    EXPECT_THROW(Step(c), std::invalid_argument);
    c.coarse.hunger_ppm = 0;
    c.region.region_stamp = 2;
    EXPECT_THROW(Step(c), std::invalid_argument);
}
} // namespace
