#include <gtest/gtest.h>

#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace Luminumbra::world;
using Luminumbra::Vec3;

const RegionKey kFar{10, -3};

// Legacy envelope/retention fixtures represent notifications already delivered by a host.
void AcknowledgeAllActivations(ActiveRegionLedger& ledger) {
    const auto pending = ledger.pending_activations();
    for (const auto& [key, activation] : pending) {
        (void)activation;
        ledger.acknowledge_activation(key.first, key.second);
    }
}
ActiveRegionLedger Distant(RegionSchedulerConfig config = {10, 2, 2}) {
    ActiveRegionLedger ledger(config);
    ledger.mark_edited(kFar, WorldClock{});
    ledger.schedule(WorldClock(1)); // consume edit wake
    AcknowledgeAllActivations(ledger);
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

TEST(ActiveRegionLedgerTest, V1FrozenHistoryLoadsAndRoundTripsVerbatimIncludingAfterWake) {
    for (const bool wake : {false, true}) {
        auto original = FrozenLedger();
        if (wake) {
            original.pin(kFar, true, WorldClock(4));
            original.schedule(WorldClock(5));
            ASSERT_EQ(original.records().at(kFar).state, RegionState::Active);
        }
        AcknowledgeAllActivations(original);
        const auto good = original.encode();
        const std::size_t ext = good.size() - 8 - 11;
        auto bytes = good.substr(0, ext) + std::string(8, '\0');
        bytes[4] = 1;
        Seal(bytes);
        ActiveRegionLedger destination = Distant();
        std::string error;
        ASSERT_TRUE(ActiveRegionLedger::decode(bytes, destination, error)) << error;
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(destination.records(), original.records());
        EXPECT_EQ(destination.canonical_bytes(), original.canonical_bytes());
        EXPECT_EQ(destination.encode(), bytes);
        EXPECT_EQ(Reload(destination).encode(), bytes);
    }
}

TEST(ActiveRegionLedgerTest, LegacyFrozenHistoryRoundTripsWithGroundExtensionsUntilRefreeze) {
    const auto original = FrozenLedger();
    const auto projected = original.encode();
    auto legacy = projected.substr(0, projected.size() - 8 - 11) + std::string(8, '\0');
    legacy[4] = 1;
    Seal(legacy);
    // Allocator only, completed work only, and both extensions must preserve legacy history.
    for (const unsigned extensions : {1u, 2u, 3u}) {
        SCOPED_TRACE(extensions);
        ActiveRegionLedger restored;
        std::string error;
        ASSERT_TRUE(ActiveRegionLedger::decode(legacy, restored, error)) << error;
        std::size_t extension_bytes = 0;
        if (extensions & 1u) {
            std::uint64_t id = 0;
            ASSERT_TRUE(restored.allocate_persistent_id(id));
            EXPECT_EQ(id, 1u);
            extension_bytes += 14;
        }
        if (extensions & 2u) {
            const std::array completed{RegionWork{.key = kFar,
                                                  .region_ticks = 1,
                                                  .entity_updates = 0,
                                                  .water_cells = 0,
                                                  .page_updates = 0}};
            restored.publish_completed_work(completed);
            extension_bytes += 26;
        }
        const auto bytes = restored.encode();
        EXPECT_EQ(Version(bytes), 2u);
        // The extension contains only ground TLVs, without declaring projection 2.
        EXPECT_EQ(bytes.size(), legacy.size() + 4 + extension_bytes);
        auto reopened = Reload(restored);
        EXPECT_EQ(reopened.encode(), bytes);
        EXPECT_EQ(reopened.records(), original.records());
        EXPECT_EQ(reopened.next_persistent_id(), restored.next_persistent_id());
        EXPECT_EQ(reopened.completed_work(), restored.completed_work());
        EXPECT_EQ(reopened.canonical_bytes(), restored.canonical_bytes());

        reopened.mark_edited(kFar, WorldClock(3));
        for (std::uint64_t tick = 4; tick <= 6; ++tick)
            Step(reopened, tick, 1);
        AcknowledgeAllActivations(reopened);
        ASSERT_EQ(reopened.records().at(kFar).frozen_at, 6u);
        EXPECT_TRUE(reopened.completed_work().empty());
        // Refreezing replaces the last historical digest and now declares projection 2.
        const std::size_t allocator_bytes = (extensions & 1u) ? 14 : 0;
        EXPECT_EQ(reopened.encode().size(), legacy.size() + 4 + allocator_bytes + 7);
        EXPECT_EQ(Reload(reopened).encode(), reopened.encode());
    }
}

TEST(ActiveRegionLedgerTest, LegacyEnvelopeUpgradesOnlyAfterEveryHistoricalDigestIsRefreshed) {
    const RegionKey second{11, -3};
    auto original = FrozenLedger();
    original.mark_edited(second, WorldClock(3));
    for (std::uint64_t tick = 4; tick <= 6; ++tick)
        Step(original, tick, 1);
    ASSERT_EQ(original.records().at(second).state, RegionState::Frozen);
    AcknowledgeAllActivations(original);
    const auto good = original.encode();
    auto bytes = good.substr(0, good.size() - 8 - 11) + std::string(8, '\0');
    bytes[4] = 1;
    Seal(bytes);
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(bytes, restored, error)) << error;

    restored.mark_edited(kFar, WorldClock(6));
    for (std::uint64_t tick = 7; tick <= 9; ++tick)
        Step(restored, tick, 1);
    ASSERT_EQ(restored.records().at(kFar).frozen_at, 9u);
    AcknowledgeAllActivations(restored);
    EXPECT_EQ(Version(restored.encode()), 1u);
    EXPECT_EQ(Reload(restored).canonical_bytes(), restored.canonical_bytes());

    restored.mark_edited(second, WorldClock(9));
    for (std::uint64_t tick = 10; tick <= 12; ++tick)
        Step(restored, tick, 1);
    ASSERT_EQ(restored.records().at(second).frozen_at, 12u);
    AcknowledgeAllActivations(restored);
    EXPECT_EQ(Version(restored.encode()), 2u);
    EXPECT_EQ(Reload(restored).encode(), restored.encode());
}

TEST(ActiveRegionLedgerTest, FrozenProjectionAdoptionIsAtomicAcrossAllHistoricalOwners) {
    const RegionKey second{11, -3};
    auto original = FrozenLedger();
    original.mark_edited(second, WorldClock(3));
    for (std::uint64_t tick = 4; tick <= 6; ++tick)
        Step(original, tick, 1);
    AcknowledgeAllActivations(original);
    ASSERT_EQ(original.records().at(second).state, RegionState::Frozen);
    const auto encoded = original.encode();
    // Historical v1 has the same records and checksum, without the projection extension.
    auto bytes = encoded.substr(0, encoded.size() - 19) + std::string(8, '\0');
    bytes[4] = 1;
    Seal(bytes);
    ActiveRegionLedger ledger;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(bytes, ledger, error)) << error;
    ASSERT_TRUE(ledger.needs_frozen_projection_adoption());
    std::vector<RegionKey> visited;
    EXPECT_THROW(ledger.adopt_frozen_projection([&](RegionKey key) -> std::uint64_t {
        visited.push_back(key);
        if (key == second)
            throw std::runtime_error("Unavailable candidate authority");
        return 111;
    }),
                 std::runtime_error);
    EXPECT_EQ(visited, (std::vector<RegionKey>{kFar, second}));
    EXPECT_EQ(ledger.encode(), bytes);
    EXPECT_TRUE(ledger.needs_frozen_projection_adoption());
    auto expected = ledger.records();
    expected.at(kFar).frozen_digest = 111;
    expected.at(second).frozen_digest = 222;
    ledger.adopt_frozen_projection([&](RegionKey key) { return key == kFar ? 111u : 222u; });
    EXPECT_EQ(ledger.records(), expected);
    EXPECT_FALSE(ledger.needs_frozen_projection_adoption());
    EXPECT_EQ(Version(ledger.encode()), 2u);
    EXPECT_EQ(Reload(ledger).canonical_bytes(), ledger.canonical_bytes());
    const auto adopted = ledger.encode();
    ledger.adopt_frozen_projection([](RegionKey) -> std::uint64_t {
        throw std::runtime_error("Already adopted stamps must not be read again");
    });
    EXPECT_EQ(ledger.encode(), adopted);
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
    AcknowledgeAllActivations(ledger);
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
// Stage separate Frozen coverage with a valid freeze stamp after each record's last tick.
void FreezeForRetention(ActiveRegionLedger& ledger) {
    const WorldClock frozen_at(ledger.header_tick() + 1);
    for (auto& [key, record] :
         const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records())) {
        record.wake_pending = false;
        record.state = RegionState::Frozen;
        record.frozen_at = frozen_at.tick();
        record.cadence_shift = ledger.config().reduced_shift;
        record.cadence_phase = ActiveRegionLedger::phase(key, record.cadence_shift);
        record.over_hold = record.under_hold = 0;
    }
    ledger.acknowledge_world_clock(frozen_at);
}
// Schedules an explicit empty anchor span, so nothing is visited.
RegionSchedule Idle(ActiveRegionLedger& ledger,
                    std::uint64_t tick,
                    const ActiveRegionLedger::OwnershipProbe& owned = {}) {
    return ledger.schedule(WorldClock(tick), std::span<const Vec3>{}, {}, {}, owned);
}

TEST(ActiveRegionLedgerTest, EvictedActiveDueKeysAreFilteredBeforePublishingSurvivingWork) {
    auto ledger = HomeVisit();
    const auto keys = Keys(ledger);
    ASSERT_GE(keys.size(), 4u);
    // Rank puts the pinned final key first, so survivor order differs from map order.
    ledger.pin(keys.back(), true, WorldClock(2));
    const std::vector<RegionKey> survivors{keys.back(), keys.front(), keys[2]};
    const RegionKey expired = keys[1];
    const auto& record = ledger.records().at(expired);
    ASSERT_EQ(record.state, RegionState::Active);
    ASSERT_EQ(record.last_proximity, 1u);
    ASSERT_FALSE(record.pinned);
    ASSERT_FALSE(record.edited);
    ASSERT_FALSE(record.populated);
    ASSERT_EQ(record.water_cursor, 0u);
    ASSERT_TRUE(ledger.completed_work().empty());
    ASSERT_TRUE(ledger.energy_ages().empty());

    const std::uint64_t due_tick = record.last_proximity + kIdle;
    // With no probe, every Active record is due; this is also the pre-eviction trace oracle.
    auto trace_reference = ledger;
    const auto unfiltered = Idle(trace_reference, due_tick);
    ASSERT_NE(std::find(unfiltered.due.begin(), unfiltered.due.end(), expired),
              unfiltered.due.end());
    ASSERT_EQ(trace_reference.records().at(expired).last_ticked, due_tick);

    const auto schedule = Idle(ledger, due_tick, [&survivors](RegionKey key) {
        return std::find(survivors.begin(), survivors.end(), key) != survivors.end();
    });
    EXPECT_FALSE(ledger.records().contains(expired));
    EXPECT_EQ(std::find(schedule.due.begin(), schedule.due.end(), expired), schedule.due.end());
    EXPECT_EQ(schedule.due, survivors);
    EXPECT_EQ(schedule.ledger_records, survivors.size());
    EXPECT_EQ(schedule.digest, unfiltered.digest);
    EXPECT_EQ(schedule.transitions, unfiltered.transitions);

    std::vector<RegionWork> completed;
    for (const auto key : schedule.due) {
        ASSERT_TRUE(ledger.records().contains(key));
        EXPECT_EQ(ledger.records().at(key).last_ticked, due_tick);
        completed.push_back({key, 1, 0, 0, 0});
    }
    ASSERT_NO_THROW(ledger.publish_completed_work(completed));
    ASSERT_EQ(ledger.completed_work().size(), survivors.size());
    for (const auto key : survivors)
        EXPECT_EQ(ledger.completed_work().at(key), 1u);
}

TEST(ActiveRegionLedgerTest, RetentionDropsOnlyIdleUnownedRecords) {
    auto ledger = HomeVisit();
    const auto visited = ledger.records().size();
    ASSERT_GT(visited, 1u);
    for (const auto& [key, record] : ledger.records()) {
        (void)key;
        ASSERT_EQ(record.state, RegionState::Active);
        ASSERT_EQ(record.last_proximity, 1u);
    }
    const auto before = Idle(ledger, 1 + kIdle - 1, kNobodyOwns);
    EXPECT_EQ(ledger.records().size(), visited);
    EXPECT_EQ(before.ledger_records, visited);
    EXPECT_EQ(before.due, Keys(ledger));
    const auto after = Idle(ledger, 1 + kIdle, kNobodyOwns);
    EXPECT_TRUE(ledger.records().empty());
    EXPECT_EQ(after.ledger_records, 0u);
    EXPECT_TRUE(after.due.empty());
}

TEST(ActiveRegionLedgerTest, FrozenRetentionKeepsOwnedRecordsAndEvictsThemWhenReleased) {
    auto ledger = HomeVisit();
    FreezeForRetention(ledger);
    const auto keys = Keys(ledger);
    ASSERT_GT(keys.size(), 1u);
    const auto owner = keys.front();
    const auto before = Idle(ledger, kIdle, kNobodyOwns);
    EXPECT_EQ(Keys(ledger), keys);
    EXPECT_TRUE(before.due.empty());
    const auto retained = Idle(ledger, 1 + kIdle, [owner](RegionKey key) { return key == owner; });
    ASSERT_EQ(Keys(ledger), (std::vector<RegionKey>{owner}));
    EXPECT_EQ(retained.ledger_records, 1u);
    EXPECT_TRUE(retained.due.empty());
    EXPECT_EQ(ledger.records().at(owner).state, RegionState::Frozen);
    EXPECT_EQ(Reload(ledger).records(), ledger.records());
    const auto released = Idle(ledger, 2 + kIdle, kNobodyOwns);
    EXPECT_TRUE(ledger.records().empty());
    EXPECT_EQ(released.ledger_records, 0u);
    EXPECT_TRUE(released.due.empty());
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
    EXPECT_EQ(schedule.due, Keys(ledger));
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
    AcknowledgeAllActivations(ledger);
    EXPECT_GT(ledger.records().size(), 1000u);
    const std::array lone{Vec3(0, 0, 0)};
    ActiveRegionLedger fresh;
    const auto fresh_schedule = fresh.schedule(WorldClock(1 + kIdle), lone);
    const auto schedule =
        ledger.schedule(WorldClock(1 + kIdle), std::span<const Vec3>(lone), {}, {}, kNobodyOwns);
    EXPECT_EQ(Keys(ledger), Keys(fresh));
    EXPECT_EQ(schedule.due, fresh_schedule.due);
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

std::string LittleEndian(std::uint64_t value, unsigned width) {
    std::string out;
    for (unsigned i = 0; i < width; ++i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 255));
    return out;
}
std::string Tlv(std::uint64_t type, std::string_view value) {
    return LittleEndian(type, 2) + LittleEndian(value.size(), 4) + std::string(value);
}
// Re-seals a version 2 envelope over the records of Distant() with the given TLV bytes.
std::string Version2With(const std::string& tlvs) {
    auto bytes = Distant().encode();
    bytes.resize(bytes.size() - 8);
    bytes[4] = 2;
    bytes += LittleEndian(tlvs.size(), 4) + tlvs + std::string(8, '\0');
    Seal(bytes);
    return bytes;
}

TEST(ActiveRegionLedgerTest, PersistentIdsAllocateSequentiallyFromOne) {
    ActiveRegionLedger ledger;
    EXPECT_EQ(ledger.next_persistent_id(), 1u);
    std::uint64_t id = 0;
    for (std::uint64_t expected = 1; expected <= 3; ++expected) {
        ASSERT_TRUE(ledger.allocate_persistent_id(id));
        EXPECT_EQ(id, expected);
    }
    EXPECT_EQ(ledger.next_persistent_id(), 4u);
}

TEST(ActiveRegionLedgerTest, PersistentIdExhaustionRefusesWithoutChanging) {
    const auto limit = ActiveRegionLedger::kPersistentIdLimit;
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(
        ActiveRegionLedger::decode(Version2With(Tlv(1, LittleEndian(limit, 8))), restored, error))
        << error;
    EXPECT_EQ(restored.next_persistent_id(), limit);
    std::uint64_t id = 42;
    EXPECT_FALSE(restored.allocate_persistent_id(id));
    EXPECT_EQ(id, 42u);
    EXPECT_EQ(restored.next_persistent_id(), limit);
    for (const std::uint64_t refused : {limit + 1, std::uint64_t{1}, std::uint64_t{0}})
        ExpectRefused(Version2With(Tlv(1, LittleEndian(refused, 8))),
                      ActiveRegionLedger::kCorruptMessage);
}

TEST(ActiveRegionLedgerTest, PersistentIdTlvShapeIsChecked) {
    ExpectRefused(Version2With(Tlv(1, LittleEndian(5, 4))), ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(Version2With(Tlv(1, LittleEndian(5, 9))), ActiveRegionLedger::kCorruptMessage);
    // Duplicate, reversed and unknown TLV types.
    ExpectRefused(Version2With(Tlv(1, LittleEndian(5, 8)) + Tlv(1, LittleEndian(6, 8))),
                  ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(Version2With(Tlv(2, std::string(1, '\x02')) + Tlv(1, LittleEndian(5, 8))),
                  ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(Version2With(Tlv(3, std::string(1, '\x01'))),
                  ActiveRegionLedger::kCorruptMessage);
    // A redundant digest projection TLV without frozen history is refused.
    ExpectRefused(Version2With(Tlv(2, std::string(1, '\x02'))),
                  ActiveRegionLedger::kCorruptMessage);
}

TEST(ActiveRegionLedgerTest, PersistentIdOnlyEnvelopeRoundTripsByteIdentical) {
    auto ledger = Distant();
    std::uint64_t id = 0;
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(ledger.allocate_persistent_id(id));
    const auto bytes = ledger.encode();
    EXPECT_EQ(Version(bytes), 2u);
    EXPECT_EQ(bytes.size(), 56u + 104u + 4u + 14u + 8u);
    ActiveRegionLedger decoded;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(bytes, decoded, error)) << error;
    EXPECT_EQ(decoded.next_persistent_id(), 4u);
    EXPECT_EQ(decoded.encode(), bytes);
}

TEST(ActiveRegionLedgerTest, PersistentIdWithFrozenHistoryRoundTripsByteIdentical) {
    auto ledger = FrozenLedger();
    std::uint64_t id = 0;
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(ledger.allocate_persistent_id(id));
    const auto bytes = ledger.encode();
    EXPECT_EQ(Version(bytes), 2u);
    EXPECT_EQ(bytes.size(), 56u + 104u + 4u + 21u + 8u);
    // Ascending TLVs: type 1 (next id 4) then type 2 (projection 2).
    EXPECT_EQ(bytes.substr(bytes.size() - 8 - 25, 25),
              std::string("\x15\0\0\0", 4) + std::string("\x01\0\x08\0\0\0", 6) +
                  LittleEndian(4, 8) + std::string("\x02\0\x01\0\0\0\x02", 7));
    ActiveRegionLedger decoded;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(bytes, decoded, error)) << error;
    EXPECT_EQ(decoded.next_persistent_id(), 4u);
    EXPECT_EQ(decoded.encode(), bytes);
}

TEST(ActiveRegionLedgerTest, EncodeStaysVersionOneWhileNextIdIsOne) {
    const ActiveRegionLedger fresh;
    const auto bytes = fresh.encode();
    EXPECT_EQ(Version(bytes), 1u);
    EXPECT_EQ(bytes.size(), 56u + 8u);
}

TEST(ActiveRegionLedgerTest, CanonicalBytesTrackPersistentIdAndAreEmptyOnlyWhenFresh) {
    ActiveRegionLedger ledger;
    EXPECT_TRUE(ledger.canonical_bytes().empty());
    std::uint64_t id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(id));
    const auto one = ledger.canonical_bytes();
    EXPECT_FALSE(one.empty());
    EXPECT_NE(one.find("|next_id:2"), std::string::npos);
    ASSERT_TRUE(ledger.allocate_persistent_id(id));
    EXPECT_NE(ledger.canonical_bytes(), one);
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), restored, error)) << error;
    EXPECT_EQ(restored.canonical_bytes(), ledger.canonical_bytes());
}

TEST(ActiveRegionLedgerTest, AllocatorOnlyLedgerIsSavedAndReloads) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir =
        std::filesystem::temp_directory_path() / ("arl-allocator-only-" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    ActiveRegionLedger ledger;
    std::uint64_t id = 0;
    for (int i = 0; i < 4; ++i)
        ASSERT_TRUE(ledger.allocate_persistent_id(id));
    ASSERT_EQ(ledger.next_persistent_id(), 5u);
    std::vector<std::string> save_errors;
    EXPECT_TRUE(Luminumbra::Persistence::WorldSaveService::save_active_regions(
        ledger, WorldClock{}, dir, &save_errors));
    EXPECT_TRUE(save_errors.empty());
    EXPECT_TRUE(std::filesystem::exists(
        Luminumbra::Persistence::WorldSaveService::active_regions_path(dir)));
    ActiveRegionLedger restored;
    std::vector<std::string> load_errors;
    EXPECT_TRUE(Luminumbra::Persistence::WorldSaveService::load_active_regions(
        restored, dir, &load_errors));
    EXPECT_TRUE(load_errors.empty());
    EXPECT_EQ(restored.next_persistent_id(), 5u);
    std::filesystem::remove_all(dir);
}

TEST(ActiveRegionLedgerTest, CompletedWorkIsDurableConsumedOnceAndEmptyReplacesIt) {
    auto ledger = Distant();
    const auto empty = ledger.canonical_bytes();
    const std::array completed{RegionWork{kFar, 1, 2, 3, 4}};
    ledger.publish_completed_work(completed);
    EXPECT_NE(ledger.canonical_bytes(), empty);
    EXPECT_EQ(Version(ledger.encode()), 2u);
    auto restored = Reload(ledger);
    EXPECT_EQ(restored.completed_work().at(kFar), 10u);
    EXPECT_EQ(restored.schedule(WorldClock(2)).work, 10u);
    EXPECT_TRUE(restored.completed_work().empty());
    restored.publish_completed_work(completed);
    restored.publish_completed_work({});
    EXPECT_EQ(restored.schedule(WorldClock(3)).work, 0u);
    EXPECT_EQ(Version(restored.encode()), 1u);
}

TEST(ActiveRegionLedgerTest, RestoredClockGapExpiresStaleCompletedWorkOnlyAfterSuccess) {
    auto ledger = Distant();
    const std::array completed{RegionWork{kFar, 100, 0, 0, 0}};
    ledger.publish_completed_work(completed);
    ledger.restore_clock(WorldClock(3));
    const auto before = ledger.canonical_bytes();
    EXPECT_THROW(ledger.schedule(WorldClock(3)), std::invalid_argument);
    EXPECT_EQ(ledger.canonical_bytes(), before);
    EXPECT_EQ(ledger.schedule(WorldClock(4)).work, 0u);
    EXPECT_TRUE(ledger.completed_work().empty());
}

TEST(ActiveRegionLedgerTest, CompletedWorkCodecRefusesUnknownOwnerAndZeroTotalAtomically) {
    auto ledger = Distant();
    const std::array completed{RegionWork{kFar, 1, 0, 0, 0}};
    ledger.publish_completed_work(completed);
    const auto good = ledger.encode();
    const auto extension = ActiveRegionLedger::kHeaderBytes + ActiveRegionLedger::kRecordBytes;
    // ext_len(4), type(2), length(4), count(4), key(8), total(8), checksum(8).
    for (const bool unknown_owner : {false, true}) {
        auto bytes = good;
        if (unknown_owner)
            bytes[extension + 14] = 99;
        else
            for (std::size_t i = 0; i < 8; ++i)
                bytes[extension + 22 + i] = 0;
        Seal(bytes);
        ExpectRefused(bytes, ActiveRegionLedger::kCorruptMessage);
    }
}

std::string EnergyAgePayload(std::uint32_t count, RegionKey owner = kFar, std::uint64_t age = 0) {
    return LittleEndian(count, 4) + LittleEndian(static_cast<std::uint32_t>(owner.x), 4) +
           LittleEndian(static_cast<std::uint32_t>(owner.z), 4) + LittleEndian(age, 8);
}

TEST(ActiveRegionLedgerTest, EnergyAgeZeroIsDurableAndEmptyMapKeepsLegacyBytes) {
    auto ledger = Distant();
    const auto legacy = ledger.encode();
    const auto canonical = ledger.canonical_bytes();
    EXPECT_EQ(canonical.find("|energy_age:"), std::string::npos);
    EXPECT_EQ(Version(legacy), 1u);
    ASSERT_TRUE(ledger.set_energy_age(kFar, 0));
    const auto encoded = ledger.encode();
    EXPECT_EQ(encoded, Version2With(Tlv(4, EnergyAgePayload(1))));
    EXPECT_NE(ledger.canonical_bytes().find("|energy_age:"), std::string::npos);
    auto restored = Reload(ledger);
    EXPECT_EQ(restored.energy_ages(), (ActiveRegionLedger::EnergyAges{{kFar, 0}}));
    EXPECT_EQ(restored.encode(), encoded);
    EXPECT_EQ(restored.canonical_bytes(), ledger.canonical_bytes());
    restored.reset_energy_ages();
    EXPECT_EQ(restored.encode(), legacy);
    EXPECT_EQ(restored.canonical_bytes(), canonical);
    ActiveRegionLedger fresh;
    EXPECT_TRUE(fresh.energy_ages().empty());
    fresh.reset_energy_ages();
    EXPECT_TRUE(fresh.canonical_bytes().empty());
    EXPECT_EQ(fresh.encode(), ActiveRegionLedger{}.encode());
}

TEST(ActiveRegionLedgerTest, EnergyAgeSettersRefuseTransactionally) {
    auto ledger = Distant();
    ASSERT_TRUE(ledger.set_energy_age(kFar, 7));
    std::uint64_t read_age = 99;
    EXPECT_FALSE(ledger.energy_age({11, -3}, read_age));
    EXPECT_EQ(read_age, 99u);
    ASSERT_TRUE(ledger.energy_age(kFar, read_age));
    EXPECT_EQ(read_age, 7u);
    const auto before = ledger.encode();
    EXPECT_FALSE(ledger.set_energy_age({11, -3}, 9));
    EXPECT_FALSE(ledger.set_energy_age(kFar, WorldClock::kTickLimit));
    EXPECT_FALSE(ledger.set_energy_age(kFar, UINT64_MAX));
    EXPECT_FALSE(ledger.set_energy_ages({{kFar, 4}, {{11, -3}, 2}}));
    EXPECT_FALSE(ledger.set_energy_ages({{kFar, WorldClock::kTickLimit}}));
    EXPECT_EQ(ledger.encode(), before);
    ASSERT_TRUE(ledger.set_energy_ages({{kFar, WorldClock::kTickLimit - 1}}));
    EXPECT_EQ(Reload(ledger).energy_ages().at(kFar), WorldClock::kTickLimit - 1);
    ASSERT_TRUE(ledger.set_energy_ages({}));
    EXPECT_TRUE(ledger.energy_ages().empty());
}

TEST(ActiveRegionLedgerTest, EnergyAgeResealedCorruptionRefusesWithoutChangingOutput) {
    // Count zero is noncanonical even if the checksum and length are valid.
    ExpectRefused(Version2With(Tlv(4, LittleEndian(0, 4))), ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(Version2With(Tlv(4, EnergyAgePayload(1, {11, -3}, 0))),
                  ActiveRegionLedger::kCorruptMessage);
    for (const auto age : {WorldClock::kTickLimit, UINT64_MAX})
        ExpectRefused(Version2With(Tlv(4, EnergyAgePayload(1, kFar, age))),
                      ActiveRegionLedger::kCorruptMessage);
    // Count beyond owner capacity, wrong width, duplicate TLV, unsupported identity.
    ExpectRefused(Version2With(Tlv(4, EnergyAgePayload(ActiveRegionLedger::kMaxRegions + 1))),
                  ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(Version2With(Tlv(4, EnergyAgePayload(1) + "x")),
                  ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(Version2With(Tlv(4, EnergyAgePayload(1)) + Tlv(4, EnergyAgePayload(1))),
                  ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(Version2With(Tlv(5, LittleEndian(1, 8))), ActiveRegionLedger::kCorruptMessage);
}

TEST(ActiveRegionLedgerTest, EnergyAgeDuplicateAndDescendingOwnersRefuse) {
    auto ledger = Distant();
    const RegionKey other{11, -3};
    ledger.mark_edited(other, WorldClock(1));
    ASSERT_TRUE(ledger.set_energy_ages({{kFar, 0}, {other, 17}}));
    const auto good = ledger.encode();
    auto restored = Reload(ledger);
    EXPECT_EQ(restored.encode(), good);
    const auto extension = ActiveRegionLedger::kHeaderBytes + 2 * ActiveRegionLedger::kRecordBytes;
    // ext_len(4), type(2), length(4), count(4), two records each 16 bytes.
    for (const bool duplicate : {true, false}) {
        auto corrupt = good;
        const auto first = extension + 14;
        const auto second = first + 16;
        if (duplicate)
            corrupt.replace(second, 8, corrupt.substr(first, 8));
        else {
            const auto a = corrupt.substr(first, 16), b = corrupt.substr(second, 16);
            corrupt.replace(first, 16, b);
            corrupt.replace(second, 16, a);
        }
        Seal(corrupt);
        ExpectRefused(corrupt, ActiveRegionLedger::kCorruptMessage);
    }
}

TEST(ActiveRegionLedgerTest, EnergyAgeCoexistsInAscendingAllocatorProjectionWorkEnvelope) {
    auto ledger = FrozenLedger();
    std::uint64_t id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(id));
    ASSERT_TRUE(ledger.set_energy_age(kFar, 6));
    const std::array completed{RegionWork{kFar, 1, 2, 3, 4}};
    ledger.publish_completed_work(completed);
    const auto bytes = ledger.encode();
    const auto extensions = Tlv(1, LittleEndian(2, 8)) + Tlv(2, std::string(1, '\x02')) +
                            Tlv(4, EnergyAgePayload(1, kFar, 6)) +
                            Tlv(6, EnergyAgePayload(1, kFar, 10));
    const auto offset = ActiveRegionLedger::kHeaderBytes + ActiveRegionLedger::kRecordBytes;
    EXPECT_EQ(bytes.substr(offset, bytes.size() - offset - 8),
              LittleEndian(extensions.size(), 4) + extensions);
    auto restored = Reload(ledger);
    EXPECT_EQ(restored.encode(), bytes);
    EXPECT_EQ(restored.energy_ages().at(kFar), 6u);
    EXPECT_EQ(restored.completed_work().at(kFar), 10u);
    EXPECT_EQ(ActiveRegionLedger::kMaxFileBytes,
              ActiveRegionLedger::kHeaderBytes +
                  ActiveRegionLedger::kMaxRegions * ActiveRegionLedger::kRecordBytes + 4u + 14u +
                  7u + 6u + SimulationContentIdentity::kEncodedBytes +
                  3u * (10u + 16u * ActiveRegionLedger::kMaxRegions) + 8u);
}

TEST(ActiveRegionLedgerTest, ZeroEnergyAgeOwnsAnIdleRecordUntilExplicitReset) {
    auto ledger = HomeVisit();
    const auto owner = Keys(ledger).front();
    ASSERT_TRUE(ledger.set_energy_age(owner, 0));
    Idle(ledger, 1 + kIdle, kNobodyOwns);
    ASSERT_EQ(ledger.records().size(), 1u);
    EXPECT_TRUE(ledger.records().contains(owner));
    EXPECT_EQ(Reload(ledger).energy_ages().at(owner), 0u);
    ledger.reset_energy_ages();
    Idle(ledger, 2 + kIdle, kNobodyOwns);
    EXPECT_TRUE(ledger.records().empty());
}

TEST(ActiveRegionLedgerTest, WaterDebtEnvelopeIsSparseStrictAndAtomic) {
    auto ledger = Distant();
    ASSERT_TRUE(ledger.set_water_debts({{kFar, ActiveRegionLedger::kMaxWaterDebt}}));
    const auto encoded = ledger.encode();
    EXPECT_EQ(Reload(ledger).water_debts(), ledger.water_debts());
    EXPECT_NE(Reload(ledger).canonical_bytes().find("|water_debt:"), std::string::npos);
    EXPECT_FALSE(ledger.set_water_debts({{kFar, 0}}));
    EXPECT_FALSE(ledger.set_water_debts({{kFar, ActiveRegionLedger::kMaxWaterDebt + 1}}));
    EXPECT_FALSE(ledger.set_water_debts({{{9, 9}, 1}}));
    EXPECT_EQ(ledger.encode(), encoded);
    const auto offset = ActiveRegionLedger::kHeaderBytes + ActiveRegionLedger::kRecordBytes;
    const auto expected = Tlv(3, EnergyAgePayload(1, kFar, ActiveRegionLedger::kMaxWaterDebt));
    EXPECT_EQ(encoded.substr(offset, encoded.size() - offset - 8),
              LittleEndian(expected.size(), 4) + expected);
    for (const auto debt : {std::uint64_t{0}, ActiveRegionLedger::kMaxWaterDebt + 1}) {
        auto malformed = encoded;
        malformed.replace(offset + 22, 8, LittleEndian(debt, 8));
        Seal(malformed);
        ExpectRefused(malformed, ActiveRegionLedger::kCorruptMessage);
    }
    auto unknown = encoded;
    unknown.replace(offset + 14, 8, LittleEndian(9, 4) + LittleEndian(9, 4));
    Seal(unknown);
    ExpectRefused(unknown, ActiveRegionLedger::kCorruptMessage);
    ASSERT_TRUE(ledger.set_water_debts({}));
    EXPECT_EQ(ledger.encode(), Distant().encode());
}

TEST(ActiveRegionLedgerTest, WaterDebtOwnsIdleRecordAndCoexistsWithOtherExtensions) {
    auto ledger = HomeVisit();
    const auto owner = Keys(ledger).front();
    ASSERT_TRUE(ledger.set_water_debts({{owner, 1}}));
    ASSERT_TRUE(ledger.set_energy_age(owner, 0));
    const std::array work{RegionWork{owner, 1, 2, 3, 4}};
    ledger.publish_completed_work(work);
    auto reopened = Reload(ledger);
    EXPECT_EQ(reopened.water_debts(), ledger.water_debts());
    EXPECT_EQ(reopened.energy_ages(), ledger.energy_ages());
    EXPECT_EQ(reopened.completed_work(), ledger.completed_work());
    ledger.reset_energy_ages();
    ledger.clear_completed_work();
    Idle(ledger, 1 + kIdle, kNobodyOwns);
    EXPECT_TRUE(ledger.records().contains(owner));
    ASSERT_TRUE(ledger.set_water_debts({}));
    Idle(ledger, 2 + kIdle, kNobodyOwns);
    EXPECT_FALSE(ledger.records().contains(owner));
}

TEST(ActiveRegionLedgerTest, ResumeAcknowledgementClearsOnlyMatchingThawedStampOnce) {
    auto ledger = FrozenLedger();
    const auto frozen = ledger.records().at(kFar).frozen_at;
    ASSERT_GT(frozen, 0u);
    EXPECT_FALSE(ledger.acknowledge_resume(kFar, frozen));
    // pin advances the header ceiling; scheduling needs the next absolute tick.
    ledger.pin(kFar, true, WorldClock(ledger.tick()));
    Step(ledger, 4, 0);
    ASSERT_EQ(ledger.records().at(kFar).state, RegionState::Active);
    EXPECT_FALSE(ledger.acknowledge_resume(kFar, frozen + 1));
    ASSERT_TRUE(ledger.acknowledge_resume(kFar, frozen));
    EXPECT_EQ(Reload(ledger).records().at(kFar).frozen_at, 0u);
    EXPECT_FALSE(ledger.acknowledge_resume(kFar, frozen));
    EXPECT_FALSE(ledger.acknowledge_resume({8, 8}, frozen));
}

TEST(ActiveRegionLedgerTest, ResumeAcknowledgementCommitsWindowBetweenReducedDuePhases) {
    auto ledger = FrozenLedger();
    const auto frozen = ledger.records().at(kFar).frozen_at;
    const auto thaw_tick = ActiveRegionLedger::phase(kFar, 1) == 0 ? 5u : 4u;
    const auto schedule = Step(ledger, thaw_tick, 0);
    ASSERT_EQ(ledger.records().at(kFar).state, RegionState::Reduced);
    ASSERT_TRUE(schedule.due.empty());
    ASSERT_LT(ledger.records().at(kFar).last_ticked, thaw_tick);
    const auto before = ledger.encode();
    EXPECT_FALSE(ledger.acknowledge_resume(kFar, frozen + 1));
    EXPECT_EQ(ledger.encode(), before);
    ASSERT_TRUE(ledger.acknowledge_resume(kFar, frozen));
    const auto restored = Reload(ledger);
    EXPECT_EQ(restored.records().at(kFar).last_ticked, thaw_tick);
    EXPECT_EQ(restored.records().at(kFar).frozen_at, 0u);
    EXPECT_EQ(restored.tick(), thaw_tick);
    const auto committed = ledger.encode();
    EXPECT_FALSE(ledger.acknowledge_resume(kFar, frozen));
    EXPECT_EQ(ledger.encode(), committed);
}

TEST(ActiveRegionLedgerTest, ContentIdentityIsImmutableAndHasAnExactVersionedWireRecord) {
    SimulationContentIdentity identity;
    identity.effective_policy_digest = 0x0102030405060708ull;
    identity.creature_definitions_digest = 11;
    identity.plant_definitions_digest = 12;
    identity.population_digest = 13;
    const auto expected = LittleEndian(1, 2) + LittleEndian(1, 2) + LittleEndian(1, 2) +
                          LittleEndian(0, 2) + LittleEndian(0x0102030405060708ull, 8) +
                          LittleEndian(11, 8) + LittleEndian(12, 8) + LittleEndian(13, 8);
    ASSERT_EQ(expected.size(), 40u);
    EXPECT_EQ(identity.encode(), expected);
    ActiveRegionLedger ledger;
    EXPECT_TRUE(ledger.canonical_bytes().empty());
    ASSERT_TRUE(ledger.adopt_content_identity(identity));
    ASSERT_TRUE(ledger.adopt_content_identity(identity));
    EXPECT_FALSE(ledger.canonical_bytes().empty());
    EXPECT_EQ(Version(ledger.encode()), 2u);
    const auto restored = Reload(ledger);
    ASSERT_TRUE(restored.content_identity());
    EXPECT_EQ(*restored.content_identity(), identity);
    EXPECT_EQ(restored.encode(), ledger.encode());
    auto changed = identity;
    ++changed.population_digest;
    const auto before = ledger.encode();
    EXPECT_FALSE(ledger.adopt_content_identity(changed));
    EXPECT_EQ(ledger.encode(), before);
    std::string error;
    EXPECT_FALSE(ledger.validate_content_identity(changed, error));
    EXPECT_EQ(error, ActiveRegionLedger::kContentMismatchMessage);
    ActiveRegionLedger historical;
    EXPECT_FALSE(historical.validate_content_identity(identity, error));
    EXPECT_EQ(error, ActiveRegionLedger::kContentVersionMessage);
}
TEST(ActiveRegionLedgerTest, ContentIdentityPrecedesCompletedWorkAndPreservesNextDecision) {
    auto ledger = Distant();
    SimulationContentIdentity identity;
    identity.effective_policy_digest = 123;
    ASSERT_TRUE(ledger.adopt_content_identity(identity));
    const std::array work{RegionWork{kFar, 1, 2, 3, 4}};
    ledger.publish_completed_work(work);
    const auto bytes = ledger.encode();
    const auto extensions = Tlv(5, identity.encode()) + Tlv(6, EnergyAgePayload(1, kFar, 10));
    const auto offset = ActiveRegionLedger::kHeaderBytes + ActiveRegionLedger::kRecordBytes;
    EXPECT_EQ(bytes.substr(offset, bytes.size() - offset - 8),
              LittleEndian(extensions.size(), 4) + extensions);
    auto restored = Reload(ledger);
    EXPECT_EQ(restored.completed_work(), ledger.completed_work());
    EXPECT_EQ(restored.canonical_bytes(), ledger.canonical_bytes());
    EXPECT_EQ(restored.schedule(WorldClock(2)), ledger.schedule(WorldClock(2)));
}
TEST(ActiveRegionLedgerTest, ContentAndWorkEnvelopeRefusesMalformedOrMisorderedTlvsAtomically) {
    auto ledger = Distant();
    const SimulationContentIdentity identity;
    ASSERT_TRUE(ledger.adopt_content_identity(identity));
    const std::array work{RegionWork{kFar, 7, 0, 0, 0}};
    ledger.publish_completed_work(work);
    const auto valid = ledger.encode();
    const auto prefix = ActiveRegionLedger::kHeaderBytes + ActiveRegionLedger::kRecordBytes;
    const auto replace_extensions = [&](const std::string& extensions) {
        auto bytes = valid.substr(0, prefix) + LittleEndian(extensions.size(), 4) + extensions +
                     LittleEndian(0, 8);
        Seal(bytes);
        return bytes;
    };
    const auto work_tlv = Tlv(6, EnergyAgePayload(1, kFar, 7));
    for (const auto& bad : {identity.encode().substr(1), identity.encode() + "x"})
        ExpectRefused(replace_extensions(Tlv(5, bad) + work_tlv),
                      ActiveRegionLedger::kCorruptMessage);
    for (std::size_t offset : {0u, 2u, 4u, 6u}) {
        auto bad = identity.encode();
        bad[offset] = 2;
        ExpectRefused(replace_extensions(Tlv(5, bad) + work_tlv),
                      ActiveRegionLedger::kCorruptMessage);
    }
    ExpectRefused(replace_extensions(work_tlv + Tlv(5, identity.encode())),
                  ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(replace_extensions(Tlv(5, identity.encode()) + Tlv(5, identity.encode())),
                  ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(
        replace_extensions(Tlv(5, identity.encode()) + Tlv(6, EnergyAgePayload(1, kFar, 0))),
        ActiveRegionLedger::kCorruptMessage);
    ExpectRefused(replace_extensions(Tlv(5, identity.encode()) + Tlv(7, "")),
                  ActiveRegionLedger::kCorruptMessage);
}

} // namespace

namespace {
TEST(ActiveRegionLedger, ActivationIncludesAbsentToActive) {
    ActiveRegionLedger ledger;
    ledger.mark_edited(kFar, WorldClock{});
    ASSERT_TRUE(ledger.activation_intents().at(kFar).creation_pending);
    const auto schedule = ledger.schedule(WorldClock(1), std::span<const Vec3>{});
    EXPECT_TRUE(schedule.transitions.empty());
    ASSERT_EQ(ledger.pending_activations().size(), 1u);
    EXPECT_EQ(ledger.pending_activations().begin()->second,
              (PendingRegionActivation{kFar, 1, ActivationCause::Edit}));
    EXPECT_TRUE(ledger.activation_intents().empty());
    ActiveRegionLedger presence;
    const std::array anchors{Vec3(256.0f, 0.0f, 256.0f)};
    presence.schedule(WorldClock(1), anchors);
    EXPECT_EQ(presence.pending_activations().size(), presence.records().size());
    for (const auto& [key, activation] : presence.pending_activations()) {
        EXPECT_EQ(key.first, 1u);
        EXPECT_EQ(activation.cause, ActivationCause::Presence);
    }
}
TEST(ActiveRegionLedger, ActivationIncludesDirectFrozenToActive) {
    auto ledger = FrozenLedger();
    ledger.pin(kFar, true, WorldClock(3));
    EXPECT_EQ(ledger.activation_intents().at(kFar).wake_cause, ActivationCause::Pin);
    ledger.schedule(WorldClock(4), std::span<const Vec3>{});
    ASSERT_EQ(ledger.pending_activations().size(), 1u);
    EXPECT_EQ(ledger.pending_activations().begin()->second,
              (PendingRegionActivation{kFar, 4, ActivationCause::Pin}));
}
TEST(ActiveRegionLedger, ReducedToActiveRecoveryDoesNotActivateAgain) {
    auto ledger = Distant({0, 1, 1});
    Step(ledger, 2, 1);
    ASSERT_EQ(ledger.records().at(kFar).state, RegionState::Reduced);
    Step(ledger, 3, 0);
    EXPECT_EQ(ledger.records().at(kFar).state, RegionState::Active);
    EXPECT_TRUE(ledger.pending_activations().empty());
    auto frozen = FrozenLedger();
    Step(frozen, 4, 0);
    Step(frozen, 5, 0);
    EXPECT_EQ(frozen.records().at(kFar).state, RegionState::Active);
    EXPECT_TRUE(frozen.pending_activations().empty());
}
TEST(ActiveRegionLedger, ActivationCauseUsesCurrentEditPinPresencePrecedence) {
    for (unsigned mask = 0; mask < 4; ++mask) {
        auto ledger = FrozenLedger();
        if (mask & 1u)
            ledger.pin(kFar, true, WorldClock(3));
        if (mask & 2u)
            ledger.mark_edited(kFar, WorldClock(3));
        const std::array anchors{Vec3(5376.0f, 0.0f, -1280.0f)};
        ledger.schedule(WorldClock(4), anchors);
        const auto activation = ledger.pending_activations().at({4, kFar});
        EXPECT_EQ(activation.cause,
                  (mask & 2u)   ? ActivationCause::Edit
                  : (mask & 1u) ? ActivationCause::Pin
                                : ActivationCause::Presence);
    }
}
TEST(ActiveRegionLedger, StickyFlagsDoNotDetermineActivationCause) {
    auto ledger = FrozenLedger();
    ASSERT_TRUE(ledger.records().at(kFar).edited);
    ledger.pin(kFar, true, WorldClock(3));
    ledger.pin(kFar, false, WorldClock(3));
    ledger.schedule(WorldClock(4), std::span<const Vec3>{});
    EXPECT_EQ(ledger.pending_activations().at({4, kFar}).cause, ActivationCause::Pin);
    AcknowledgeAllActivations(ledger);
    Step(ledger, 5, 1);
    Step(ledger, 6, 1);
    const std::array anchors{Vec3(5376.0f, 0.0f, -1280.0f)};
    ledger.schedule(WorldClock(7), anchors);
    EXPECT_EQ(ledger.pending_activations().at({7, kFar}).cause, ActivationCause::Presence);
}
TEST(ActiveRegionLedger, RejectedInputsCreateNoActivationIntent) {
    ActiveRegionLedger ledger;
    const auto before = ledger.encode();
    EXPECT_THROW(ledger.mark_edited({32768, 0}, WorldClock{}), std::invalid_argument);
    EXPECT_THROW(ledger.pin({0, -32769}, true, WorldClock{}), std::invalid_argument);
    ledger.pin(kFar, false, WorldClock{});
    EXPECT_EQ(ledger.encode(), before);
    const std::array invalid{Vec3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f)};
    ledger.schedule(WorldClock(1), invalid);
    EXPECT_TRUE(ledger.activation_intents().empty());
    EXPECT_TRUE(ledger.pending_activations().empty());
    EXPECT_TRUE(ledger.records().empty());
}
TEST(ActiveRegionLedger, ActivationIntentAndOutboxRoundTrip) {
    ActiveRegionLedger ledger;
    ledger.pin({2, 3}, true, WorldClock{});
    ledger.schedule(WorldClock(1), std::span<const Vec3>{});
    ledger.mark_edited({-2, -3}, WorldClock(1));
    auto restored = Reload(ledger);
    EXPECT_EQ(restored.activation_intents(), ledger.activation_intents());
    EXPECT_EQ(restored.pending_activations(), ledger.pending_activations());
    EXPECT_EQ(restored.canonical_bytes(), ledger.canonical_bytes());
    EXPECT_EQ(restored.encode(), ledger.encode());
    const auto canonical = restored.canonical_bytes();
    restored.acknowledge_activation(1, {2, 3});
    EXPECT_NE(restored.canonical_bytes(), canonical);
    ledger.schedule(WorldClock(2), std::span<const Vec3>{});
    ASSERT_EQ(ledger.pending_activations().size(), 2u);
    EXPECT_EQ(ledger.pending_activations().rbegin()->second.key, (RegionKey{-2, -3}));
}
TEST(ActiveRegionLedger, AbsentActivationExtensionAdoptsEmpty) {
    const auto historical = Distant();
    ASSERT_EQ(Version(historical.encode()), 1u);
    auto restored = Reload(historical);
    EXPECT_TRUE(restored.activation_intents().empty());
    EXPECT_TRUE(restored.pending_activations().empty());
    restored.schedule(WorldClock(2), std::span<const Vec3>{});
    EXPECT_TRUE(restored.pending_activations().empty());
    EXPECT_EQ(Version(restored.encode()), 1u);
}
std::string ActivationPayload(std::uint64_t tick = 1, std::uint8_t cause = 3) {
    return LittleEndian(0, 4) + LittleEndian(1, 4) + LittleEndian(tick, 8) +
           LittleEndian(static_cast<std::uint32_t>(kFar.x), 4) +
           LittleEndian(static_cast<std::uint32_t>(kFar.z), 4) + LittleEndian(cause, 1);
}
TEST(ActiveRegionLedger, MalformedActivationExtensionRefusesTransactionally) {
    for (const auto& value : {std::string{},
                              LittleEndian(0, 8),
                              ActivationPayload(0),
                              ActivationPayload(2),
                              ActivationPayload(1, 0),
                              ActivationPayload(1, 4),
                              ActivationPayload() + "extra",
                              LittleEndian(ActiveRegionLedger::kMaxRegions + 1ull, 4)})
        ExpectRefused(Version2With(Tlv(7, value)), ActiveRegionLedger::kCorruptMessage);
    auto unknown = ActivationPayload();
    unknown.replace(16, 4, LittleEndian(99, 4));
    ExpectRefused(Version2With(Tlv(7, unknown)), ActiveRegionLedger::kCorruptMessage);
    auto duplicate = LittleEndian(0, 4) + LittleEndian(2, 4) + ActivationPayload().substr(8) +
                     ActivationPayload().substr(8);
    ExpectRefused(Version2With(Tlv(7, duplicate)), ActiveRegionLedger::kCorruptMessage);
    const auto intent = LittleEndian(1, 4) + LittleEndian(static_cast<std::uint32_t>(kFar.x), 4) +
                        LittleEndian(static_cast<std::uint32_t>(kFar.z), 4);
    for (const auto& suffix :
         {std::string("\2\3", 2), std::string("\1\4", 2), std::string("\0\3", 2)})
        ExpectRefused(Version2With(Tlv(7, intent + suffix + LittleEndian(0, 4))),
                      ActiveRegionLedger::kCorruptMessage);
}
TEST(ActiveRegionLedger, PendingActivationRetainsOwnerUntilAcknowledgement) {
    ActiveRegionLedger ledger;
    const std::array anchors{Vec3(100.0f, 0.0f, 100.0f)};
    ledger.schedule(WorldClock(1), anchors);
    const auto count = ledger.records().size();
    Idle(ledger, 1 + kIdle, kNobodyOwns);
    EXPECT_EQ(ledger.records().size(), count);
    EXPECT_EQ(ledger.pending_activations().size(), count);
    ledger = Reload(ledger);
    AcknowledgeAllActivations(ledger);
    Idle(ledger, 2 + kIdle, kNobodyOwns);
    EXPECT_TRUE(ledger.records().empty());
}
TEST(ActiveRegionLedger, DefaultNoneDoesNotPreventIdleRetention) {
    auto ledger = HomeVisit();
    for (const auto& [key, record] : ledger.records()) {
        (void)key;
        EXPECT_FALSE(record.populated);
    }
    Idle(ledger, 1 + kIdle, kNobodyOwns);
    EXPECT_TRUE(ledger.records().empty());
}
TEST(ActiveRegionLedger, ActivationMetadataDoesNotRaiseLedgerFileLimit) {
    constexpr auto expected = ActiveRegionLedger::kHeaderBytes +
                              ActiveRegionLedger::kMaxRegions * ActiveRegionLedger::kRecordBytes +
                              4u + 14u + 7u + 6u + SimulationContentIdentity::kEncodedBytes +
                              3u * (10u + 16u * ActiveRegionLedger::kMaxRegions) + 8u;
    static_assert(ActiveRegionLedger::kMaxFileBytes == expected);
    ActiveRegionLedger ledger;
    ledger.mark_edited(kFar, WorldClock{});
    ledger.schedule(WorldClock(1), std::span<const Vec3>{});
    EXPECT_EQ(ledger.encode().size(),
              ActiveRegionLedger::kHeaderBytes + ActiveRegionLedger::kRecordBytes + 4u + 6u + 25u +
                  8u);
    EXPECT_EQ(Reload(ledger).encode(), ledger.encode());
    std::string oversized(ActiveRegionLedger::kMaxFileBytes + 1, '\0');
    oversized.replace(0, 6, std::string("ARL1\2\0", 6));
    auto destination = Distant();
    const auto before = destination.encode();
    std::string error;
    EXPECT_FALSE(ActiveRegionLedger::decode(oversized, destination, error));
    EXPECT_EQ(destination.encode(), before);
}
} // namespace
