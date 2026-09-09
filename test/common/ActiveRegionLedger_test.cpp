#include <gtest/gtest.h>

#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"

#include <array>
#include <limits>

namespace {
using namespace Luminumbra::world;
using Luminumbra::Vec3;

const RegionKey kFar{10, -3};

ActiveRegionLedger Distant(RegionSchedulerConfig config = {10, 2, 2}) {
    ActiveRegionLedger ledger(config);
    ledger.mark_edited(kFar, WorldClock{});
    ledger.schedule(WorldClock(1)); // consume edit wake
    return ledger;
}
RegionSchedule Step(ActiveRegionLedger& ledger, std::uint64_t tick, std::uint64_t units) {
    const std::array work{RegionWork{kFar, units, 0, 0, 0}};
    return ledger.schedule(
        WorldClock(tick), {}, work, [](RegionKey) { return 0x123456789abcdef0ull; });
}
ActiveRegionLedger Reload(const ActiveRegionLedger& ledger) {
    ActiveRegionLedger restored;
    std::string error;
    EXPECT_TRUE(ActiveRegionLedger::decode(ledger.encode(), restored, error)) << error;
    return restored;
}

TEST(ActiveRegionLedgerTest, RegionAddressingAndFixedDiscIgnoreHeight) {
    EXPECT_EQ(ActiveRegionLedger::region_at(Vec3(-0.1f, 0, -512.1f)), (RegionKey{-1, -2}));
    EXPECT_EQ(ActiveRegionLedger::region_at(Vec3(512, 0, 0)), (RegionKey{1, 0}));
    ActiveRegionLedger a, b;
    a.set_local_anchor(Vec3(256, 0, 256));
    b.set_local_anchor(Vec3(256, 9000, 256));
    const auto first = a.schedule(WorldClock(1));
    const auto second = b.schedule(WorldClock(1));
    EXPECT_EQ(first, second);
    EXPECT_EQ(a.records().size(), 9u);
    EXPECT_TRUE(a.records().contains({-1, -1}));
    EXPECT_FALSE(a.records().contains({-2, 0}));
    a.set_local_anchor(Vec3(10000, 0, 10000));
    a.schedule(WorldClock(2));
    EXPECT_EQ(a.records().at({0, 0}).last_proximity, 1u);
    EXPECT_EQ(a.records().at({0, 0}).first_active, 1u);
}

TEST(ActiveRegionLedgerTest, FullTickIntegerSumsAreStrictAndOrderIndependent) {
    auto a = Distant(), b = a;
    const std::array work{RegionWork{kFar, 2, 1, 1, 1}, RegionWork{kFar, 0, 0, 0, 5}};
    const std::array reversed{work[1], work[0]};
    EXPECT_EQ(a.schedule(WorldClock(2), {}, work), b.schedule(WorldClock(2), {}, reversed));
    EXPECT_EQ(a.records().at(kFar).over_hold, 0u); // equality is not over
    EXPECT_EQ(a.records().at(kFar).pressure, 10u);
    Step(a, 3, 11);
    EXPECT_EQ(a.records().at(kFar).over_hold, 1u);
    Step(a, 4, 10);
    EXPECT_EQ(a.records().at(kFar).over_hold, 0u);
    Step(a, 5, std::numeric_limits<std::uint64_t>::max());
    const std::array overflow{RegionWork{kFar, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
    EXPECT_EQ(a.schedule(WorldClock(6), {}, overflow).work, UINT64_MAX);
}

TEST(ActiveRegionLedgerTest, ReductionFreezeRecoveryAndHoldsSurviveReload) {
    auto uninterrupted = Distant(), resumed = uninterrupted, repeat = uninterrupted;
    for (std::uint64_t tick = 2; tick <= 12; ++tick) {
        SCOPED_TRACE(tick);
        const auto work = tick <= 6 ? 11u : 10u;
        const auto expected = Step(uninterrupted, tick, work);
        EXPECT_EQ(Step(resumed, tick, work), expected);
        EXPECT_EQ(Step(repeat, tick, work), expected);
        EXPECT_EQ(uninterrupted.canonical_bytes(), resumed.canonical_bytes());
        resumed = Reload(resumed); // boundaries inside both hold sequences
        const auto& r = uninterrupted.records().at(kFar);
        if (tick == 3) {
            EXPECT_EQ(r.state, RegionState::Reduced);
            EXPECT_EQ(r.over_hold, 0u);
            EXPECT_EQ(r.cadence_shift, 2u);
        }
        if (tick == 5) {
            EXPECT_EQ(r.state, RegionState::Frozen);
            EXPECT_EQ(r.frozen_at, 5u);
            EXPECT_EQ(r.frozen_digest, 0x123456789abcdef0ull);
        }
        if (tick == 8)
            EXPECT_EQ(r.state, RegionState::Reduced);
        if (tick == 10)
            EXPECT_EQ(r.state, RegionState::Active);
    }
}

TEST(ActiveRegionLedgerTest, PrioritySelectsLowestForReductionAndHighestForRecovery) {
    ActiveRegionLedger ledger({0, 1, 1});
    ledger.pin({1, 0}, true, WorldClock{});
    ledger.mark_edited({2, 0}, WorldClock{});
    const std::array anchors{Vec3(2304, 0, 2304)};
    ledger.schedule(WorldClock(1), anchors);
    // More recent proximity outranks coordinates: this group's coordinates
    // sort after the old group, so omitting/reversing age fails both loops.
    const std::array recent{Vec3(4352, 0, 4352)};
    ledger.schedule(WorldClock(2), recent);
    const std::vector<RegionKey> priority{{1, 0}, {2, 0}, {7, 7}, {7, 8}, {7, 9}, {8, 7}, {8, 8},
                                          {8, 9}, {9, 7}, {9, 8}, {9, 9}, {3, 3}, {3, 4}, {3, 5},
                                          {4, 3}, {4, 4}, {4, 5}, {5, 3}, {5, 4}, {5, 5}};
    ASSERT_EQ(ledger.records().size(), priority.size());
    ASSERT_EQ(ledger.records().at({3, 3}).last_proximity, 1u);
    ASSERT_EQ(ledger.records().at({7, 7}).last_proximity, 2u);
    ledger = Reload(ledger);
    const std::array work{RegionWork{{1, 0}, 1, 0, 0, 0}};
    std::uint64_t tick = 2;
    for (auto it = priority.rbegin(); it != priority.rend(); ++it) {
        for (const auto state : {RegionState::Reduced, RegionState::Frozen}) {
            const auto schedule =
                ledger.schedule(WorldClock(++tick), {}, work, [](RegionKey) { return 42u; });
            ASSERT_EQ(schedule.transitions.size(), 1u);
            EXPECT_EQ(schedule.transitions.front().key, *it);
            EXPECT_EQ(schedule.transitions.front().to, state);
        }
    }
    // Forced wakes with the same pin rank retain their different proximity
    // ages across reload. Current proximity would refresh both ages to zero.
    auto waking = ledger;
    waking.pin({3, 3}, true, WorldClock(tick));
    waking.pin({7, 7}, true, WorldClock(tick));
    waking = Reload(waking);
    const auto wake = waking.schedule(WorldClock(tick + 1), {}, work);
    const std::vector<RegionKey> wake_priority{{7, 7}, {3, 3}};
    ASSERT_EQ(wake.transitions.size(), wake_priority.size());
    EXPECT_EQ(wake.due, wake_priority);
    for (std::size_t i = 0; i < wake_priority.size(); ++i)
        EXPECT_EQ(wake.transitions[i],
                  (RegionTransition{wake_priority[i], RegionState::Frozen, RegionState::Active}));

    // Every region is eligible at once: recovery must choose forward priority,
    // including pin/edit rank, proximity age and coordinate tie breaks.
    for (const auto key : priority) {
        for (const auto state : {RegionState::Reduced, RegionState::Active}) {
            const auto schedule = ledger.schedule(WorldClock(++tick));
            ASSERT_EQ(schedule.transitions.size(), 1u);
            EXPECT_EQ(schedule.transitions.front().key, key);
            EXPECT_EQ(schedule.transitions.front().to, state);
        }
    }
}

TEST(ActiveRegionLedgerTest, NearbyRegionsStayFullCadenceAndWakeInPriorityOrder) {
    ActiveRegionLedger ledger({0, 1, 1});
    const Vec3 anchor(2304, 0, 2304);
    const std::array anchors{anchor};
    ledger.schedule(WorldClock(1), anchors);
    const std::array work{RegionWork{{4, 4}, 1, 0, 0, 0}};
    for (std::uint64_t tick = 2; tick <= 19; ++tick)
        ledger.schedule(WorldClock(tick), {}, work, [](RegionKey) { return 42u; });
    for (const auto& [key, record] : ledger.records()) {
        (void)key;
        ASSERT_EQ(record.state, RegionState::Frozen);
    }
    ledger.pin({4, 4}, true, WorldClock(19));
    ledger.mark_edited({3, 4}, WorldClock(19));
    ledger.set_local_anchor(anchor);
    ledger = Reload(ledger);
    const auto schedule = ledger.schedule(WorldClock(20), {}, work);
    const std::vector<RegionKey> priority{
        {4, 4}, {3, 4}, {3, 3}, {3, 5}, {4, 3}, {4, 5}, {5, 3}, {5, 4}, {5, 5}};
    ASSERT_EQ(schedule.transitions.size(), priority.size());
    for (std::size_t i = 0; i < priority.size(); ++i) {
        EXPECT_EQ(schedule.transitions[i],
                  (RegionTransition{priority[i], RegionState::Frozen, RegionState::Active}));
    }
    EXPECT_EQ(schedule.due, priority);
    for (std::uint64_t tick = 21; tick < 25; ++tick) {
        const auto near = ledger.schedule(WorldClock(tick), {}, work);
        EXPECT_TRUE(near.transitions.empty());
        EXPECT_EQ(near.due, priority);
        for (const auto& [key, record] : ledger.records()) {
            (void)key;
            EXPECT_EQ(record.state, RegionState::Active);
            EXPECT_EQ(record.over_hold, 0u);
            EXPECT_EQ(record.last_ticked, tick);
        }
    }
}

TEST(ActiveRegionLedgerTest, EditAndPinWakeFrozenRegionsAfterPersistence) {
    for (const bool pin : {false, true}) {
        auto ledger = Distant({0, 1, 1});
        Step(ledger, 2, 1);
        Step(ledger, 3, 1);
        ASSERT_EQ(ledger.records().at(kFar).state, RegionState::Frozen);
        if (pin)
            ledger.pin(kFar, true, WorldClock(3));
        else
            ledger.mark_edited(kFar, WorldClock(3));
        auto restored = Reload(ledger);
        EXPECT_EQ(Step(restored, 4, 1), Step(ledger, 4, 1));
        EXPECT_EQ(restored.records().at(kFar).state, RegionState::Active);
    }
}

TEST(ActiveRegionLedgerTest, FrozenContentsAndCursorDoNotAdvanceAndResumeIsBounded) {
    auto ledger = Distant({0, 1, 1});
    ledger.set_water_cursor(kFar, 1234);
    Step(ledger, 2, 1);
    Step(ledger, 3, 1);
    const auto frozen = ledger.records().at(kFar);
    for (std::uint64_t tick = 4; tick < 15; ++tick) {
        const auto schedule = Step(ledger, tick, 1);
        EXPECT_TRUE(schedule.due.empty());
        const auto& r = ledger.records().at(kFar);
        EXPECT_EQ(r.last_ticked, frozen.last_ticked);
        EXPECT_EQ(r.water_cursor, 1234u);
        EXPECT_EQ(r.frozen_digest, frozen.frozen_digest);
    }
    EXPECT_EQ(
        ActiveRegionLedger::resume_window(kFar, 3, WorldClock(100000), UINT64_MAX).elapsed_ticks,
        36000u);
    EXPECT_EQ(ActiveRegionLedger::resume_window(kFar, 3, WorldClock(100000), 7).elapsed_ticks, 7u);
    EXPECT_THROW(ActiveRegionLedger::resume_window(kFar, 3, WorldClock(2), 7),
                 std::invalid_argument);
}

TEST(ActiveRegionLedgerTest, UnlimitedDefaultNeverReducesOrFreezes) {
    auto ledger = Distant(RegionSchedulerConfig{});
    for (std::uint64_t tick = 2; tick <= 100; ++tick) {
        EXPECT_TRUE(Step(ledger, tick, UINT64_MAX).transitions.empty());
        EXPECT_EQ(ledger.records().at(kFar).state, RegionState::Active);
    }
}

TEST(ActiveRegionLedgerTest, CadenceAndAbsoluteTickLimitsAreChecked) {
    auto ledger = Distant({0, 1, 3});
    Step(ledger, 2, 1);
    // Reduced cadence itself is exercised across two full periods by
    // CadenceRunsOnlyAtDerivedPhaseAndFailedFreezeLeavesLedgerIntact.
    EXPECT_THROW(Step(ledger, 2, 0), std::invalid_argument);
    EXPECT_THROW(Step(ledger, 1, 0), std::invalid_argument);
    EXPECT_THROW(ActiveRegionLedger((RegionSchedulerConfig{1, 0, 1})), std::invalid_argument);
    EXPECT_THROW(ActiveRegionLedger((RegionSchedulerConfig{1, 1, 17})), std::invalid_argument);
    EXPECT_THROW(ledger.set_local_anchor(Vec3(std::numeric_limits<float>::infinity(), 0, 0)),
                 std::invalid_argument);
    EXPECT_THROW(ledger.pin({32768, 0}, true, WorldClock(8)), std::invalid_argument);
    auto last = Distant();
    Step(last, WorldClock::kTickLimit - 1, 0);
    EXPECT_EQ(Reload(last).canonical_bytes(), last.canonical_bytes());
}

TEST(ActiveRegionLedgerTest, CanonicalLayoutAndObservationalExclusion) {
    auto ledger = Distant();
    ledger.set_local_anchor(Vec3(-0.0f, 12.5f, -256.0f));
    ledger.set_water_cursor(kFar, 42);
    ledger.set_populated(kFar, true);
    const auto bytes = ledger.encode();
    EXPECT_EQ(bytes.substr(0, 8), std::string("ARL1\x01\0\0\0", 8));
    EXPECT_EQ(bytes.size(), 48u + 104u + 8u);
    const auto canonical = ledger.canonical_bytes();
    ledger.mark_saved(WorldClock(1));
    EXPECT_NE(ledger.encode(), bytes);
    EXPECT_EQ(ledger.canonical_bytes(), canonical);
    EXPECT_EQ(Reload(ledger).encode(), ledger.encode());
    auto modified = ledger;
    modified.set_water_cursor(kFar, 43);
    EXPECT_NE(modified.canonical_bytes(), canonical);
    modified = ledger;
    modified.pin(kFar, true, WorldClock(1));
    EXPECT_NE(modified.canonical_bytes(), canonical);
    EXPECT_TRUE(ActiveRegionLedger{}.canonical_bytes().empty());
}

TEST(ActiveRegionLedgerTest, CorruptTruncatedAndFuturePayloadsRefuseTransactionally) {
    const auto good = Distant().encode();
    auto destination = Distant();
    const auto before = destination.encode();
    std::string error;
    for (std::size_t length = 0; length < good.size(); ++length) {
        EXPECT_FALSE(ActiveRegionLedger::decode(
            std::string_view(good).substr(0, length), destination, error));
        EXPECT_EQ(error, ActiveRegionLedger::kCorruptMessage);
        EXPECT_EQ(destination.encode(), before);
    }
    for (std::size_t offset = 6; offset < good.size(); ++offset) {
        auto corrupt = good;
        corrupt[offset] ^= 0x40;
        EXPECT_FALSE(ActiveRegionLedger::decode(corrupt, destination, error));
        EXPECT_EQ(error, ActiveRegionLedger::kCorruptMessage);
    }
    auto future = good;
    future[4] = 2;
    EXPECT_FALSE(ActiveRegionLedger::decode(future, destination, error));
    EXPECT_EQ(error, ActiveRegionLedger::kFutureMessage);
    EXPECT_EQ(destination.encode(), before);
    EXPECT_FALSE(ActiveRegionLedger::decode(good + "x", destination, error));
}
TEST(ActiveRegionLedgerTest, ValidChecksumsCannotHideInvalidRecordSemantics) {
    const auto good = Distant().encode();
    const auto rewrite = [](std::string bytes, std::size_t offset, unsigned char value) {
        bytes.at(offset) = static_cast<char>(value);
        const auto checksum =
            std::stoull(Luminumbra::Persistence::StableChecksum(bytes.substr(0, bytes.size() - 8)),
                        nullptr,
                        16);
        for (unsigned i = 0; i < 8; ++i)
            bytes[bytes.size() - 8 + i] = static_cast<char>((checksum >> (8 * i)) & 255);
        return bytes;
    };
    // Header reserved/config/count; record key/flags/state/phase/hold/timestamp.
    for (const auto& [offset, value] :
         std::array<std::pair<std::size_t, unsigned char>, 11>{{{6, 1},
                                                                {24, 0},
                                                                {28, 17},
                                                                {29, 2},
                                                                {44, 2},
                                                                {51, 127},
                                                                {56, 128},
                                                                {57, 3},
                                                                {60, 1},
                                                                {128, 3},
                                                                {112, 2}}}) {
        SCOPED_TRACE(offset);
        ActiveRegionLedger restored;
        std::string error;
        EXPECT_FALSE(ActiveRegionLedger::decode(rewrite(good, offset, value), restored, error));
        EXPECT_EQ(error, ActiveRegionLedger::kCorruptMessage);
    }
}

TEST(ActiveRegionLedgerTest, CadenceRunsOnlyAtDerivedPhaseAndFailedFreezeLeavesLedgerIntact) {
    auto ledger = Distant({0, 2, 3});
    Step(ledger, 2, 1);
    Step(ledger, 3, 1);
    ASSERT_EQ(ledger.records().at(kFar).state, RegionState::Reduced);
    for (std::uint64_t tick = 4; tick < 20; ++tick) {
        // Alternate pressure so neither hold ever reaches two.
        const auto schedule = Step(ledger, tick, tick % 2);
        ASSERT_EQ(ledger.records().at(kFar).state, RegionState::Reduced);
        EXPECT_EQ(!schedule.due.empty(), (tick & 7u) == ActiveRegionLedger::phase(kFar, 3));
    }
    const auto before = ledger.encode();
    const std::array work{RegionWork{kFar, 1, 0, 0, 0}};
    EXPECT_THROW(
        ledger.schedule(WorldClock(20),
                        {},
                        work,
                        [](RegionKey) -> std::uint64_t { throw std::runtime_error("IO failure"); }),
        std::runtime_error);
    EXPECT_EQ(ledger.encode(), before);
}

} // namespace
