#include <gtest/gtest.h>

#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
// Same FNV-1a 64 as the implementation's Hash(), used to re-seal hand-crafted envelopes.
std::uint64_t Fnv1a64(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}
void Seal(std::string& bytes) {
    const auto checksum = Fnv1a64(std::string_view(bytes).substr(0, bytes.size() - 8));
    for (unsigned i = 0; i < 8; ++i)
        bytes[bytes.size() - 8 + i] = static_cast<char>((checksum >> (8 * i)) & 255);
}
// One record that has been frozen (state Frozen, frozen_at 3, fake digest).
ActiveRegionLedger FrozenLedger() {
    auto ledger = Distant({0, 1, 1});
    Step(ledger, 2, 1);
    Step(ledger, 3, 1);
    return ledger;
}
unsigned Version(std::string_view bytes) {
    return static_cast<unsigned char>(bytes[4]) |
           (static_cast<unsigned>(static_cast<unsigned char>(bytes[5])) << 8);
}
void ExpectRefused(const std::string& bytes, const char* expected) {
    auto destination = Distant();
    const auto before = destination.encode();
    std::string error;
    EXPECT_FALSE(ActiveRegionLedger::decode(bytes, destination, error));
    EXPECT_EQ(error, expected);
    EXPECT_EQ(destination.encode(), before);
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

TEST(ActiveRegionLedgerTest, RestoringNewerClockPreservesHoldsAndFrozenSimulationState) {
    auto ledger = Distant();
    ledger.set_water_cursor(kFar, 42);
    for (std::uint64_t tick = 2; tick <= 6; ++tick) {
        Step(ledger, tick, tick < 6 ? 11 : 0);
        const auto& record = ledger.records().at(kFar);
        if (tick == 2)
            ASSERT_EQ(record.over_hold, 1u);
        if (tick == 6) {
            ASSERT_EQ(record.state, RegionState::Frozen);
            ASSERT_EQ(record.under_hold, 1u);
            ASSERT_EQ(record.frozen_digest, 0x123456789abcdef0ull);
        }
        auto loaded = Reload(ledger);
        const auto canonical_before = loaded.canonical_bytes();
        const auto tick_before = loaded.tick();
        loaded.restore_clock(WorldClock(100));
        // Reconciliation raises only the validation ceiling. The canonical tick
        // and the hash projection belong to the scheduler and must not move
        // without simulation, or recovery would rewrite an accepted state.
        EXPECT_EQ(loaded.header_tick(), 100u);
        EXPECT_EQ(loaded.tick(), tick_before);
        EXPECT_EQ(loaded.canonical_bytes(), canonical_before);
        EXPECT_EQ(loaded.records(), ledger.records());
        EXPECT_EQ(loaded.config(), ledger.config());
        EXPECT_EQ(Reload(loaded).encode(), loaded.encode());
        EXPECT_EQ(Reload(loaded).canonical_bytes(), canonical_before);
        const auto before = loaded.encode();
        EXPECT_THROW(loaded.restore_clock(WorldClock(99)), std::invalid_argument);
        EXPECT_EQ(loaded.encode(), before);
        EXPECT_THROW(loaded.schedule(WorldClock(100)), std::invalid_argument);
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
    EXPECT_EQ(bytes.size(), 56u + 104u + 8u);
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
    future[4] = 3;
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
    // Header reserved/ceiling/config/count; record key/flags/state/phase/hold/timestamp.
    // A ceiling below the canonical tick is refused: recovery may raise it, never lower it.
    for (const auto& [offset, value] :
         std::array<std::pair<std::size_t, unsigned char>, 12>{{{6, 1},
                                                                {16, 0},
                                                                {32, 0},
                                                                {36, 17},
                                                                {37, 2},
                                                                {52, 2},
                                                                {59, 127},
                                                                {64, 128},
                                                                {65, 3},
                                                                {68, 1},
                                                                {136, 3},
                                                                {120, 2}}}) {
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

TEST(ActiveRegionLedgerTest, ValidAnchorBoundaries) {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    EXPECT_TRUE(ActiveRegionLedger::valid_anchor(Vec3(-16777216.0f, 0, -16777216.0f)));
    EXPECT_TRUE(ActiveRegionLedger::valid_anchor(Vec3(16777215.0f, 5, 16777215.0f)));
    EXPECT_TRUE(ActiveRegionLedger::valid_anchor(Vec3(0, -1e30f, 0)));
    EXPECT_FALSE(ActiveRegionLedger::valid_anchor(Vec3(16777216.0f, 0, 0)));
    EXPECT_FALSE(ActiveRegionLedger::valid_anchor(Vec3(0, 0, 16777216.0f)));
    EXPECT_FALSE(ActiveRegionLedger::valid_anchor(Vec3(-16777218.0f, 0, 0)));
    EXPECT_FALSE(ActiveRegionLedger::valid_anchor(Vec3(nan, 0, 0)));
    EXPECT_FALSE(ActiveRegionLedger::valid_anchor(Vec3(0, 0, inf)));
    EXPECT_FALSE(ActiveRegionLedger::valid_anchor(Vec3(0, nan, 0)));
    EXPECT_FALSE(ActiveRegionLedger::valid_anchor(Vec3(0, -inf, 0)));
}

TEST(ActiveRegionLedgerTest, TrySetLocalAnchorDoesNotThrowAndKeepsPreviousAnchor) {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    ActiveRegionLedger ledger;
    EXPECT_TRUE(ledger.try_set_local_anchor(Vec3(256, 0, 256)));
    EXPECT_FALSE(ledger.try_set_local_anchor(Vec3(nan, 0, 0)));
    EXPECT_FALSE(ledger.try_set_local_anchor(Vec3(inf, 0, 0)));
    const std::optional<Vec3> anchor = ledger.local_anchor();
    ASSERT_TRUE(anchor.has_value());
    if (anchor.has_value()) {
        const Vec3 position = anchor.value();
        EXPECT_EQ(position.x, 256.0f);
        EXPECT_EQ(position.z, 256.0f);
    }
}

TEST(ActiveRegionLedgerTest, AcknowledgeWorldClockRaisesOnlyTheCeiling) {
    ActiveRegionLedger ledger;
    ledger.mark_edited(kFar, WorldClock{});
    ledger.schedule(WorldClock(1));
    const auto before = ledger.canonical_bytes();
    ledger.acknowledge_world_clock(WorldClock(9));
    EXPECT_EQ(ledger.header_tick(), 9u);
    EXPECT_EQ(ledger.tick(), 1u);
    EXPECT_EQ(ledger.canonical_bytes(), before);
    ledger.acknowledge_world_clock(WorldClock(3));
    EXPECT_EQ(ledger.header_tick(), 9u);
    EXPECT_THROW(ledger.schedule(WorldClock(9)), std::invalid_argument);
    EXPECT_NO_THROW(ledger.schedule(WorldClock(10)));
}

TEST(ActiveRegionLedgerTest, InvalidSchedulerInputsAreSkippedAndCounted) {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const std::array anchors{Vec3(256, 0, 256), Vec3(nan, 0, 0), Vec3(0, 0, 1e30f)};
    const std::array work{RegionWork{kFar, 3, 0, 0, 0}, RegionWork{{999, 999}, 5, 0, 0, 0}};
    const std::array clean{Vec3(256, 0, 256)};

    ActiveRegionLedger ledger;
    ledger.mark_edited(kFar, WorldClock{});
    const auto first = ledger.schedule(WorldClock(1), std::span<const Vec3>(anchors), work);
    EXPECT_EQ(first.rejected.anchors, 2u);
    EXPECT_EQ(first.rejected.work, 1u);
    EXPECT_EQ(first.rejected.capacity, 0u);
    EXPECT_EQ(first.work, 3u);
    EXPECT_TRUE(ledger.records().contains({0, 0}));
    const auto next = ledger.schedule(WorldClock(2), std::span<const Vec3>(clean));
    EXPECT_EQ(next.rejected, RegionRejections{});

    ActiveRegionLedger twin;
    twin.mark_edited(kFar, WorldClock{});
    twin.schedule(WorldClock(1), std::span<const Vec3>(anchors), work);
    const auto twin_next = twin.schedule(WorldClock(2), std::span<const Vec3>(clean));
    EXPECT_EQ(next.digest, twin_next.digest);
}

TEST(ActiveRegionLedgerTest, RejectedCountsAreAppendedToTheTraceOnlyWhenNonZero) {
    const std::array nan_anchor{Vec3(std::numeric_limits<float>::quiet_NaN(), 0, 0)};
    ActiveRegionLedger a, b;
    a.mark_edited(kFar, WorldClock{});
    b.mark_edited(kFar, WorldClock{});
    const auto clean = a.schedule(WorldClock(1), std::span<const Vec3>{});
    const auto dirty = b.schedule(WorldClock(1), std::span<const Vec3>(nan_anchor));
    EXPECT_NE(clean.digest, dirty.digest);
    EXPECT_FALSE(clean.rejected.any());
    EXPECT_EQ(dirty.rejected.anchors, 1u);
    EXPECT_EQ(a.canonical_bytes(), b.canonical_bytes());
}

TEST(ActiveRegionLedgerTest, ScheduleTraceMatchesTheHandBuiltByteOracle) {
    const auto put = [](std::string& out, std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i)
            out.push_back(static_cast<char>((value >> (i * 8)) & 255));
    };
    // Expected trace: header, tick, work, one Active region at (0,0) ticked
    // this tick, no transitions, then (only when rejected) three LE u32 counts.
    const auto oracle = [&put](bool rejected_anchor) {
        std::string trace = "region_schedule:v1:";
        put(trace, 1, 8);
        put(trace, 0, 8);
        put(trace, 1, 4);
        put(trace, 0, 4);
        put(trace, 0, 4);
        put(trace, 0, 1);
        put(trace, 1, 1);
        put(trace, 0, 4);
        if (rejected_anchor) {
            put(trace, 1, 4);
            put(trace, 0, 4);
            put(trace, 0, 4);
        }
        return Luminumbra::Persistence::StableChecksum(trace);
    };
    ActiveRegionLedger ledger;
    ledger.pin({0, 0}, true, WorldClock{});
    const auto s = ledger.schedule(WorldClock(1));
    ASSERT_EQ(s.due.size(), 1u);
    EXPECT_EQ(s.digest, oracle(false));

    const std::array nan_anchor{Vec3(std::numeric_limits<float>::quiet_NaN(), 0, 0)};
    ActiveRegionLedger rejected;
    rejected.pin({0, 0}, true, WorldClock{});
    const auto dirty = rejected.schedule(WorldClock(1), std::span<const Vec3>(nan_anchor));
    ASSERT_EQ(dirty.due.size(), 1u);
    EXPECT_EQ(dirty.rejected.anchors, 1u);
    EXPECT_EQ(dirty.digest, oracle(true));
}

TEST(ActiveRegionLedgerTest, EnvelopeIsV1WithoutFrozenHistory) {
    auto ledger = Distant({0, 1, 1});
    Step(ledger, 2, 1);
    ASSERT_EQ(ledger.records().at(kFar).state, RegionState::Reduced);
    const auto bytes = ledger.encode();
    EXPECT_EQ(Version(bytes), 1u);
    EXPECT_EQ(bytes.size(), 56u + 104u + 8u);
    ActiveRegionLedger decoded;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(bytes, decoded, error)) << error;
    EXPECT_EQ(decoded.encode(), bytes);
}

TEST(ActiveRegionLedgerTest, EnvelopeIsV2WithProjectionExtensionAfterAFreeze) {
    const auto ledger = FrozenLedger();
    ASSERT_EQ(ledger.records().at(kFar).state, RegionState::Frozen);
    const auto bytes = ledger.encode();
    EXPECT_EQ(Version(bytes), 2u);
    EXPECT_EQ(bytes.size(), 56u + 104u + 4u + 7u + 8u);
    // Little endian: u32 ext_len=7, u16 type=2, u32 length=1, u8 value=2.
    EXPECT_EQ(bytes.substr(bytes.size() - 19, 11),
              std::string("\x07\0\0\0\x02\0\x01\0\0\0\x02", 11));
    ActiveRegionLedger decoded;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(bytes, decoded, error)) << error;
    EXPECT_EQ(decoded.encode(), bytes);
    EXPECT_EQ(decoded.canonical_bytes(), ledger.canonical_bytes());
}

TEST(ActiveRegionLedgerTest, FrozenHistoryPersistsAfterWake) {
    auto ledger = FrozenLedger();
    ledger.pin(kFar, true, WorldClock(4));
    ledger.schedule(WorldClock(5));
    const auto& record = ledger.records().at(kFar);
    ASSERT_EQ(record.state, RegionState::Active);
    ASSERT_NE(record.frozen_at, 0u);
    const auto bytes = ledger.encode();
    EXPECT_EQ(Version(bytes), 2u);
    EXPECT_EQ(Reload(ledger).encode(), bytes);
}

TEST(ActiveRegionLedgerTest, V2RefusalMatrix) {
    const auto good = FrozenLedger().encode();
    ASSERT_EQ(Version(good), 2u);
    // Extension block: ext_len at ext, TLV type at ext+4, length at ext+6, value at ext+10.
    const std::size_t ext = good.size() - 8 - 11;
    const auto edited = [&](std::size_t offset, unsigned char value) {
        auto bytes = good;
        bytes[offset] = static_cast<char>(value);
        Seal(bytes);
        return bytes;
    };
    {
        // ext_len 0 with the TLV removed and the size adjusted.
        auto bytes = good.substr(0, ext) + std::string(4, '\0') + std::string(8, '\0');
        Seal(bytes);
        ExpectRefused(bytes, ActiveRegionLedger::kCorruptMessage);
    }
    ExpectRefused(edited(ext + 4, 1), ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(edited(ext + 6, 2), ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(edited(ext + 10, 1), ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(edited(ext + 10, 3), ActiveRegionLedger::kProjectionMessage);
    ExpectRefused(edited(4, 3), ActiveRegionLedger::kFutureMessage);
    {
        // Duplicate type 2 with ext_len 14.
        auto bytes = good.substr(0, ext) + std::string("\x0e\0\0\0", 4) + good.substr(ext + 4, 7) +
                     good.substr(ext + 4, 7) + good.substr(good.size() - 8);
        Seal(bytes);
        ExpectRefused(bytes, ActiveRegionLedger::kCorruptMessage);
    }
    {
        // Garbage byte before the checksum, checksum re-sealed.
        auto bytes = good.substr(0, good.size() - 8) + "x" + good.substr(good.size() - 8);
        Seal(bytes);
        ExpectRefused(bytes, ActiveRegionLedger::kCorruptMessage);
    }
    ExpectRefused(good + "x", ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(good.substr(0, good.size() - 1), ActiveRegionLedger::kCorruptMessage);
    {
        // A v2 file whose records hold no frozen history but declares the projection.
        const auto plain = Distant().encode();
        auto bytes = plain.substr(0, plain.size() - 8) +
                     std::string("\x07\0\0\0\x02\0\x01\0\0\0\x02", 11) + std::string(8, '\0');
        bytes[4] = 2;
        Seal(bytes);
        ExpectRefused(bytes, ActiveRegionLedger::kCorruptMessage);
    }
}

TEST(ActiveRegionLedgerTest, V1FileWithFrozenHistoryIsRefused) {
    const auto good = FrozenLedger().encode();
    const std::size_t ext = good.size() - 8 - 11;
    auto bytes = good.substr(0, ext) + std::string(8, '\0');
    bytes[4] = 1;
    Seal(bytes);
    ActiveRegionLedger destination = Distant();
    const auto before = destination.encode();
    std::string error;
    EXPECT_FALSE(ActiveRegionLedger::decode(bytes, destination, error));
    EXPECT_EQ(error, "Unsupported frozen digest projection.");
    EXPECT_EQ(destination.encode(), before);
}

const std::uint64_t kIdle = ActiveRegionLedger::kIdleEvictionTicks;
const ActiveRegionLedger::OwnershipProbe kNobodyOwns = [](RegionKey) {
    return false;
};

// One replicated visit at tick 1 near the origin; every record it creates has last_proximity 1.
ActiveRegionLedger HomeVisit() {
    ActiveRegionLedger ledger;
    const std::array anchors{Vec3(100, 0, 100)};
    ledger.schedule(WorldClock(1), anchors);
    return ledger;
}
std::vector<RegionKey> Keys(const ActiveRegionLedger& ledger) {
    std::vector<RegionKey> keys;
    for (const auto& [key, record] : ledger.records()) {
        (void)record;
        keys.push_back(key);
    }
    return keys;
}
// Schedules an explicit empty anchor span, so nothing is visited and only retention can act.
RegionSchedule Idle(ActiveRegionLedger& ledger,
                    std::uint64_t tick,
                    const ActiveRegionLedger::OwnershipProbe& owned = {}) {
    return ledger.schedule(WorldClock(tick), std::span<const Vec3>{}, {}, {}, owned);
}

TEST(ActiveRegionLedgerTest, RetentionDropsOnlyIdleUnownedRecords) {
    auto ledger = HomeVisit();
    const auto visited = ledger.records().size();
    ASSERT_GT(visited, 1u);
    for (const auto& [key, record] : ledger.records()) {
        (void)key;
        ASSERT_EQ(record.last_proximity, 1u);
    }
    const auto before = Idle(ledger, 1 + kIdle - 1, kNobodyOwns);
    EXPECT_EQ(ledger.records().size(), visited);
    EXPECT_EQ(before.ledger_records, visited);
    const auto after = Idle(ledger, 1 + kIdle, kNobodyOwns);
    EXPECT_TRUE(ledger.records().empty());
    EXPECT_EQ(after.ledger_records, 0u);
}

TEST(ActiveRegionLedgerTest, RetentionKeepsPinnedEditedPopulatedWaterAndOwnedRecords) {
    auto ledger = HomeVisit();
    const auto keys = Keys(ledger);
    ASSERT_GE(keys.size(), 6u);
    // Flags use tick 2, below the idle tick, so none of them re-stamp last_proximity.
    ledger.pin(keys[0], true, WorldClock(2));
    ledger.mark_edited(keys[1], WorldClock(2));
    ledger.set_populated(keys[2], true);
    ledger.set_water_cursor(keys[3], 5);
    const RegionKey owned_key = keys[4];
    const ActiveRegionLedger::OwnershipProbe owned = [&](RegionKey key) {
        return key == owned_key;
    };
    const std::vector<RegionKey> kept(keys.begin(), keys.begin() + 5);

    const auto before = Idle(ledger, kIdle, owned);
    EXPECT_EQ(before.ledger_records, keys.size());
    const auto after = Idle(ledger, 1 + kIdle, owned);
    EXPECT_EQ(Keys(ledger), kept);
    EXPECT_EQ(after.ledger_records, kept.size());
}

TEST(ActiveRegionLedgerTest, NoProbeNeverEvicts) {
    auto ledger = HomeVisit();
    const auto visited = ledger.records().size();
    const auto schedule = Idle(ledger, 3 * kIdle);
    EXPECT_EQ(ledger.records().size(), visited);
    EXPECT_EQ(schedule.ledger_records, visited);
}

TEST(ActiveRegionLedgerTest, EvictedRegionStartsFreshWhenVisitedAgain) {
    auto ledger = HomeVisit();
    const std::uint64_t evicted_at = 1 + kIdle;
    Idle(ledger, evicted_at, kNobodyOwns);
    ASSERT_TRUE(ledger.records().empty());
    const std::array anchors{Vec3(100, 0, 100)};
    const std::uint64_t revisit = evicted_at + 4;
    ledger.schedule(WorldClock(revisit), anchors);
    ASSERT_TRUE(ledger.records().contains({0, 0}));
    const auto& record = ledger.records().at({0, 0});
    EXPECT_EQ(record.first_active, revisit);
    EXPECT_EQ(record.last_proximity, revisit);
    EXPECT_EQ(record.state, RegionState::Active);
}

TEST(ActiveRegionLedgerTest, RetentionKeepsTheLedgerBounded) {
    std::vector<Vec3> grid;
    for (int i = 0; i < 600; ++i)
        grid.emplace_back(
            static_cast<float>((i % 30) * 2048), 0.0f, static_cast<float>((i / 30) * 2048));
    ActiveRegionLedger ledger;
    ledger.schedule(WorldClock(1), std::span<const Vec3>(grid));
    EXPECT_GT(ledger.records().size(), 1000u);
    const std::array lone{Vec3(0, 0, 0)};
    const auto schedule =
        ledger.schedule(WorldClock(1 + kIdle), std::span<const Vec3>(lone), {}, {}, kNobodyOwns);
    EXPECT_LT(ledger.records().size(), 20u);
    EXPECT_EQ(schedule.ledger_records, ledger.records().size());
}

TEST(ActiveRegionLedgerTest, EvictionRoundTripsThroughEncodeDecode) {
    auto ledger = HomeVisit();
    const auto keys = Keys(ledger);
    ledger.pin(keys.front(), true, WorldClock(2));
    Idle(ledger, 1 + kIdle, kNobodyOwns);
    ASSERT_EQ(ledger.records().size(), 1u);
    const auto restored = Reload(ledger);
    EXPECT_EQ(restored.encode(), ledger.encode());
    EXPECT_EQ(restored.records(), ledger.records());
}

TEST(ActiveRegionLedgerTest, PointerRowsMatchACopyScheduledIdentically) {
    ActiveRegionLedger ledger({0, 1, 1});
    ledger.pin({1, 0}, true, WorldClock{});
    ledger.mark_edited({2, 0}, WorldClock{});
    const std::array anchors{Vec3(2304, 0, 2304)};
    ledger.schedule(WorldClock(1), anchors);
    const std::array work{RegionWork{{1, 0}, 3, 0, 0, 0}, RegionWork{{2, 0}, 5, 0, 0, 0}};
    // Unvisited work with a zero work limit degrades regions each tick.
    for (std::uint64_t tick = 2; tick <= 6; ++tick)
        ledger.schedule(WorldClock(tick), {}, work, [](RegionKey) { return 42u; });
    bool degraded = false;
    for (const auto& [key, record] : ledger.records()) {
        (void)key;
        degraded = degraded || record.state != RegionState::Active;
    }
    ASSERT_TRUE(degraded);

    auto copy = ledger;
    const std::array next{Vec3(2304, 0, 2304)};
    const auto a = ledger.schedule(WorldClock(7), next, work, [](RegionKey) { return 42u; });
    const auto b = copy.schedule(WorldClock(7), next, work, [](RegionKey) { return 42u; });
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.digest, b.digest);
    EXPECT_EQ(ledger.encode(), copy.encode());
}

} // namespace
