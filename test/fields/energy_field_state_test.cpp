// AetherEmitterDeterminism provides proving signals for the stateful energy-field
// layer. The kernel constants are public and pinned; the tests
// replicate single ops (one decay step) from those constants where an expected
// value is needed — never from a parallel implementation of the kernel.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/systems/FieldEmitterSystem.h"

namespace {

using luminumbra::fields::EnergyFieldState;
using luminumbra::fields::kEnergyCadenceTicks;
using luminumbra::fields::kEnergyDecayD;
using luminumbra::fields::kEnergyDecayShift;
using luminumbra::fields::kEnergyPageCells;
using luminumbra::fields::kEnergyWindowCells;

// The single pinned decay step, from the public constants (used only to build
// expected values for one step at a time).
std::uint16_t DecayOnceRef(std::uint16_t v) {
    const std::uint32_t mul = (1u << kEnergyDecayShift) - kEnergyDecayD;
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(v) * mul) >> kEnergyDecayShift);
}

// Window origin for an anchor at world cell (0, 0) — mirrors SetAnchorCell's
// page-aligned centering so tests can address exact window cells.
constexpr int kWinO = -(kEnergyWindowCells / 2); // -32, already page-aligned

// Sum every cell in the active window via the public point reader.
std::uint64_t WindowSum(const EnergyFieldState& f) {
    std::uint64_t sum = 0;
    for (int cz = kWinO; cz < kWinO + kEnergyWindowCells; ++cz) {
        for (int cx = kWinO; cx < kWinO + kEnergyWindowCells; ++cx) {
            sum += f.at_cell(cx, cz);
        }
    }
    return sum;
}

// Expected post-fire total: read every window cell, apply ONE reference decay
// step, sum. Diffusion must then conserve this sum exactly.
std::uint64_t ExpectedTotalAfterOneFire(const EnergyFieldState& f) {
    std::uint64_t sum = 0;
    for (int cz = kWinO; cz < kWinO + kEnergyWindowCells; ++cz) {
        for (int cx = kWinO; cx < kWinO + kEnergyWindowCells; ++cx) {
            sum += DecayOnceRef(f.at_cell(cx, cz));
        }
    }
    return sum;
}

TEST(AetherEmitterDeterminism, EmptyFieldEmptySubHash) {
    EnergyFieldState f;
    EXPECT_TRUE(f.CanonicalBytes().empty());
    EXPECT_EQ(f.page_count(), 0u);
    EXPECT_EQ(f.Tick(1), 0u); // unanchored, no deposits: inert
    EXPECT_TRUE(f.CanonicalBytes().empty());

    // An out-of-window deposit is dropped (and accounted) — still empty.
    f.SetAnchorCell(0, 0);
    f.QueueDeposit(/*emitter=*/1, /*cx=*/10000, /*cz=*/10000, 0, 500);
    EXPECT_EQ(f.Tick(2), 500u);
    EXPECT_TRUE(f.CanonicalBytes().empty());
    EXPECT_EQ(f.page_count(), 0u);
}

TEST(AetherEmitterDeterminism, ConservationNoSaturation) {
    // Proving signal (a): deposits sized to never clip; before any firing the
    // total equals the deposits exactly, and across every firing the total
    // equals the decay-adjusted expectation exactly (diffusion leaks nothing).
    EnergyFieldState f;
    f.SetAnchorCell(0, 0);

    std::uint64_t deposited = 0;
    std::uint64_t tick = 0;
    for (int i = 0; i < 5; ++i) {
        f.QueueDeposit(7, i * 3 - 6, -i * 2 + 4, 0, 9000 + 100u * i);
        f.QueueDeposit(3, i * 3 - 6, -i * 2 + 4, 0, 1000u + i);
        deposited += 9000 + 100u * i + 1000u + i;
        EXPECT_EQ(f.Tick(++tick), 0u) << "unexpected clipping at tick " << tick;
    }
    ASSERT_LT(tick, kEnergyCadenceTicks) << "test invariant: no firing yet";
    EXPECT_EQ(f.total_raw(), deposited);

    // Across 10 firings: expected = per-cell one-step decay of the pre-fire
    // window; the diffusion sweeps must conserve that value EXACTLY.
    for (int fire = 0; fire < 10; ++fire) {
        while ((tick + 1) % kEnergyCadenceTicks != 0) {
            EXPECT_EQ(f.Tick(++tick), 0u);
        }
        const std::uint64_t expected = ExpectedTotalAfterOneFire(f);
        EXPECT_EQ(f.Tick(++tick), 0u); // the firing tick
        EXPECT_EQ(f.total_raw(), expected) << "leak at firing " << fire;
        EXPECT_EQ(WindowSum(f), expected) << "energy escaped the sealed window";
    }
}

TEST(AetherEmitterDeterminism, SaturationAccounting) {
    // Proving signal (a2): clipped raw is returned and the books balance:
    // sum(cells) == deposits - clipped.
    EnergyFieldState f;
    f.SetAnchorCell(0, 0);
    f.QueueDeposit(1, 0, 0, 0, 40000);
    f.QueueDeposit(2, 0, 0, 0, 40000);
    const std::uint64_t clipped = f.Tick(1);
    EXPECT_EQ(clipped, 80000u - 65535u);
    EXPECT_EQ(f.total_raw(), 80000u - clipped);
    EXPECT_EQ(f.at_cell(0, 0), 65535u);
}

TEST(AetherEmitterDeterminism, DepositOrderIndependence) {
    // Proving signal (b): registration order can never reach the field bytes.
    EnergyFieldState a, b;
    a.SetAnchorCell(0, 0);
    b.SetAnchorCell(0, 0);

    // Same deposits, opposite queue order, same tick.
    a.QueueDeposit(11, 2, 3, 0, 30000);
    a.QueueDeposit(22, 2, 3, 0, 30000);
    a.QueueDeposit(33, -5, 1, 0, 12345);
    b.QueueDeposit(33, -5, 1, 0, 12345);
    b.QueueDeposit(22, 2, 3, 0, 30000);
    b.QueueDeposit(11, 2, 3, 0, 30000);

    EXPECT_EQ(a.Tick(1), b.Tick(1));
    // Saturating pair on one cell: order of the two 30000s cannot matter.
    a.QueueDeposit(44, 2, 3, 0, 30000);
    b.QueueDeposit(44, 2, 3, 0, 30000);
    EXPECT_EQ(a.Tick(2), b.Tick(2));

    for (std::uint64_t t = 3; t <= 40; ++t) {
        a.Tick(t);
        b.Tick(t);
    }
    ASSERT_FALSE(a.CanonicalBytes().empty());
    EXPECT_EQ(a.CanonicalBytes(), b.CanonicalBytes());
}

TEST(AetherEmitterDeterminism, WindowEdgeConservation) {
    // Proving signal (d): deposits hugging the SEALED window edge; the corner
    // cell's outflow has only two in-window neighbours — the residue must stay
    // in the source, never vanish across the seam.
    EnergyFieldState f;
    f.SetAnchorCell(0, 0);
    f.QueueDeposit(1, kWinO, kWinO, 0, 60000);                          // corner
    f.QueueDeposit(2, kWinO + kEnergyWindowCells - 1, kWinO, 0, 60000); // edge
    ASSERT_EQ(f.Tick(1), 0u);

    std::uint64_t tick = 1;
    for (int fire = 0; fire < 6; ++fire) {
        while ((tick + 1) % kEnergyCadenceTicks != 0)
            f.Tick(++tick);
        const std::uint64_t expected = ExpectedTotalAfterOneFire(f);
        f.Tick(++tick);
        EXPECT_EQ(f.total_raw(), expected) << "edge leak at firing " << fire;
    }
}

TEST(AetherEmitterDeterminism, SequentialCatchUpBitEqual) {
    // Page-in catch-up decay must be the SEQUENTIAL
    // per-step loop — bit-equal to stepping the pinned decay one firing at a
    // time (pow-by-squaring truncates once, differs).
    const std::uint16_t seed_value = 51234;
    const int far_cx = 4096; // page far outside any test window

    EnergyFieldState f;
    f.SetAnchorCell(far_cx, far_cx);
    f.QueueDeposit(1, far_cx, far_cx, 0, seed_value);
    ASSERT_EQ(f.Tick(1), 0u);

    // Freeze the page: move the window away, run k firings elsewhere.
    f.SetAnchorCell(0, 0);
    const int k = 23;
    std::uint64_t tick = 1;
    std::uint64_t fires_before = f.fires_completed();
    while (f.fires_completed() < fires_before + k)
        f.Tick(++tick);
    // Frozen page is read AS STORED (reads never mutate).
    EXPECT_EQ(f.at_cell(far_cx, far_cx), seed_value);

    // Re-enter at a NON-firing tick: catch-up only, no new firing.
    f.SetAnchorCell(far_cx, far_cx);
    ASSERT_NE((tick + 1) % kEnergyCadenceTicks, 0u)
        << "test invariant: re-entry tick must not fire";
    f.Tick(++tick);

    std::uint16_t expected = seed_value;
    for (int i = 0; i < k; ++i)
        expected = DecayOnceRef(expected);
    EXPECT_EQ(f.at_cell(far_cx, far_cx), expected);
}

TEST(AetherEmitterDeterminism, SaveResumeEquivalence) {
    // Proving signal (c): save mid-cadence-cycle mid-decay, load, resume ⇒
    // identical canonical bytes to the uninterrupted run (the epoch rebase +
    // normalization contract).
    auto drive = [](EnergyFieldState& f,
                    std::uint64_t from_tick,
                    std::uint64_t to_tick,
                    std::uint64_t tick_offset) {
        for (std::uint64_t t = from_tick; t <= to_tick; ++t) {
            const std::uint64_t logical = t + tick_offset;
            if (logical % 5 == 1) {
                f.QueueDeposit(9,
                               static_cast<int>(logical % 7) - 3,
                               static_cast<int>(logical % 11) - 5,
                               0,
                               2000 + static_cast<std::uint32_t>(logical % 300));
            }
            f.Tick(t);
        }
    };

    const std::uint64_t kSave = 13; // mid-cycle (13 % 8 == 5), mid-decay
    const std::uint64_t kEnd = 200;

    // Uninterrupted run.
    EnergyFieldState a;
    a.SetAnchorCell(0, 0);
    drive(a, 1, kEnd, 0);

    // Interrupted run: same history to kSave, serialize, load at base 0,
    // resume with the same logical deposit stream.
    EnergyFieldState b;
    b.SetAnchorCell(0, 0);
    drive(b, 1, kSave, 0);
    const std::string record = b.SerializeRecord(kSave);

    EnergyFieldState c;
    ASSERT_TRUE(c.DeserializeRecord(record, 0));
    c.SetAnchorCell(0, 0);
    drive(c, 1, kEnd - kSave, kSave);

    ASSERT_FALSE(a.CanonicalBytes().empty());
    EXPECT_EQ(a.CanonicalBytes(), c.CanonicalBytes());
    EXPECT_EQ(a.total_raw(), c.total_raw());
}

TEST(AetherEmitterDeterminism, PagedOutStateDiverges) {
    // Proving signal (e): identical ACTIVE windows, different paged-out state
    // ⇒ different canonical bytes (the whole page set is hashed, not the
    // window).
    const int far_cx = 4096;

    EnergyFieldState a, b;
    // B (only) accrues state in a far region first.
    b.SetAnchorCell(far_cx, far_cx);
    b.QueueDeposit(1, far_cx, far_cx, 0, 30000);
    b.Tick(1);
    a.SetAnchorCell(far_cx, far_cx);
    a.Tick(1);

    // Both then move to the SAME window and receive IDENTICAL deposits.
    a.SetAnchorCell(0, 0);
    b.SetAnchorCell(0, 0);
    for (std::uint64_t t = 2; t <= 30; ++t) {
        a.QueueDeposit(5, 1, 1, 0, 4000);
        b.QueueDeposit(5, 1, 1, 0, 4000);
        a.Tick(t);
        b.Tick(t);
    }
    // Active-window cells agree...
    EXPECT_EQ(a.at_cell(1, 1), b.at_cell(1, 1));
    //...but the canonical bytes MUST differ (B's frozen far page).
    EXPECT_NE(a.CanonicalBytes(), b.CanonicalBytes());
}

TEST(AetherEmitterDeterminism, ExactZeroDecay) {
    // -2: the multiply-shift decay reaches literal 0 — no epsilon tail.
    EnergyFieldState f;
    f.SetAnchorCell(0, 0);
    f.QueueDeposit(1, 0, 0, 0, 65535);
    f.Tick(1);

    std::uint64_t tick = 1;
    bool reached_zero = false;
    for (int fire = 0; fire < 2000 && !reached_zero; ++fire) {
        while ((tick + 1) % kEnergyCadenceTicks != 0)
            f.Tick(++tick);
        f.Tick(++tick);
        reached_zero = (f.total_raw() == 0);
    }
    EXPECT_TRUE(reached_zero) << "decay tail never reached exact zero";
    EXPECT_TRUE(f.CanonicalBytes().empty());
    EXPECT_EQ(f.page_count(), 0u); // all-zero pages dropped
}

TEST(AetherEmitterDeterminism, RecordRejectsGarbage) {
    EnergyFieldState f;
    EXPECT_FALSE(f.DeserializeRecord("", 0));
    EXPECT_FALSE(f.DeserializeRecord("BOGUS 1 4\n", 0));
    EXPECT_FALSE(f.DeserializeRecord("EFS1 0 4\n", 0));                 // bad channels
    EXPECT_FALSE(f.DeserializeRecord("EFS1 1 0\n", 0));                 // bad remaining
    EXPECT_FALSE(f.DeserializeRecord("EFS1 1 99\n", 0));                // bad remaining
    EXPECT_FALSE(f.DeserializeRecord("EFS1 1 4\nP 0 0 999999:1\n", 0)); // idx
    EXPECT_FALSE(f.DeserializeRecord("EFS1 1 4\nP 0 0 1:70000\n", 0));  // val
    EXPECT_FALSE(f.DeserializeRecord("EFS1 1 4\nX 0 0\n", 0));          // tag
}

// AetherDualChannelDeterminism: channel B (polarity)
// under the SAME flag/kernel. The sub-hash covers channel B from v1's first
// activation; the two channels evolve independently through the shared
// pinned kernel.
TEST(AetherDualChannelDeterminism, ChannelsEvolveIndependently) {
    EnergyFieldState f(/*channels=*/2);
    f.SetAnchorCell(0, 0);
    // Same deposit pattern into each channel but at DIFFERENT cells.
    f.QueueDeposit(1, 0, 0, /*channel=*/0, 20000);
    f.QueueDeposit(1, 5, 5, /*channel=*/1, 20000);
    ASSERT_EQ(f.Tick(1), 0u);

    std::uint64_t tick = 1;
    for (int fire = 0; fire < 4; ++fire) {
        while ((tick + 1) % kEnergyCadenceTicks != 0)
            f.Tick(++tick);
        f.Tick(++tick);
    }
    // The kernel is channel-symmetric: channel 1's pattern at (5,5) must equal
    // channel 0's at (0,0), cell-for-cell under the offset (both far from the
    // sealed edge), and neither leaks into the other channel.
    for (int dz = -3; dz <= 3; ++dz) {
        for (int dx = -3; dx <= 3; ++dx) {
            EXPECT_EQ(f.at_cell(dx, dz, 0), f.at_cell(5 + dx, 5 + dz, 1))
                << "kernel not channel-symmetric at (" << dx << ',' << dz << ')';
        }
    }
    EXPECT_EQ(f.at_cell(0, 0, 1), f.at_cell(5, 5, 0)) << "cross-channel leak (symmetric probe)";
}

TEST(AetherDualChannelDeterminism, SubHashCoversChannelB) {
    // Two fields with IDENTICAL channel-0 state; one adds a channel-1 deposit.
    // The canonical bytes MUST differ (v1 covers polarity from day one), and a
    // B-only field is nonempty.
    EnergyFieldState a(2), b(2), b_only(2);
    a.SetAnchorCell(0, 0);
    b.SetAnchorCell(0, 0);
    b_only.SetAnchorCell(0, 0);
    a.QueueDeposit(1, 0, 0, 0, 10000);
    b.QueueDeposit(1, 0, 0, 0, 10000);
    b.QueueDeposit(1, 0, 0, 1, 7);
    b_only.QueueDeposit(1, 0, 0, 1, 7);
    a.Tick(1);
    b.Tick(1);
    b_only.Tick(1);
    EXPECT_NE(a.CanonicalBytes(), b.CanonicalBytes());
    EXPECT_FALSE(b_only.CanonicalBytes().empty());
}

TEST(AetherDualChannelDeterminism, DualChannelSaveResumeEquivalence) {
    // The -4 epoch-rebase contract holds with BOTH channels live: save
    // mid-cycle mid-decay, resume, and the trajectory matches the
    // uninterrupted run byte-for-byte.
    auto drive = [](EnergyFieldState& f,
                    std::uint64_t from_tick,
                    std::uint64_t to_tick,
                    std::uint64_t tick_offset) {
        for (std::uint64_t t = from_tick; t <= to_tick; ++t) {
            const std::uint64_t logical = t + tick_offset;
            if (logical % 4 == 1) {
                f.QueueDeposit(9,
                               static_cast<int>(logical % 5) - 2,
                               0,
                               0,
                               3000 + static_cast<std::uint32_t>(logical % 100));
                f.QueueDeposit(9,
                               0,
                               static_cast<int>(logical % 7) - 3,
                               1,
                               1500 + static_cast<std::uint32_t>(logical % 50));
            }
            f.Tick(t);
        }
    };
    const std::uint64_t kSave = 11, kEnd = 120;

    EnergyFieldState a(2);
    a.SetAnchorCell(0, 0);
    drive(a, 1, kEnd, 0);

    EnergyFieldState b(2);
    b.SetAnchorCell(0, 0);
    drive(b, 1, kSave, 0);
    const std::string record = b.SerializeRecord(kSave);

    EnergyFieldState c; // channel count restores FROM the record header
    ASSERT_TRUE(c.DeserializeRecord(record, 0));
    ASSERT_EQ(c.channels(), 2);
    c.SetAnchorCell(0, 0);
    drive(c, 1, kEnd - kSave, kSave);

    ASSERT_FALSE(a.CanonicalBytes().empty());
    EXPECT_EQ(a.CanonicalBytes(), c.CanonicalBytes());
}

TEST(AetherEmitterDeterminism, MultiChannelIndependence) {
    // v1 record header carries the channel count ( readiness): the
    // two channels evolve independently and both round-trip.
    EnergyFieldState f(/*channels=*/2);
    f.SetAnchorCell(0, 0);
    f.QueueDeposit(1, 0, 0, /*channel=*/0, 10000);
    f.QueueDeposit(1, 0, 0, /*channel=*/1, 20000);
    f.Tick(1);
    EXPECT_EQ(f.at_cell(0, 0, 0), 10000u);
    EXPECT_EQ(f.at_cell(0, 0, 1), 20000u);

    const std::string record = f.SerializeRecord(1);
    EnergyFieldState g;
    ASSERT_TRUE(g.DeserializeRecord(record, 0));
    EXPECT_EQ(g.channels(), 2);
    EXPECT_EQ(g.at_cell(0, 0, 0), 10000u);
    EXPECT_EQ(g.at_cell(0, 0, 1), 20000u);
}

using Luminumbra::world::ActiveRegionLedger;
using Luminumbra::world::ActiveRegionRecord;
using Luminumbra::world::RegionKey;
using Luminumbra::world::RegionState;
using Luminumbra::world::WorldClock;
using EnergyAges = EnergyFieldState::EnergyAges;
using EnergyRecords = EnergyFieldState::RegionRecords;
using EnergyResult = EnergyFieldState::RegionalResult;

EnergyRecords TwoEnergyOwners() {
    return {{{0, 0}, ActiveRegionRecord{}}, {{1, 0}, ActiveRegionRecord{}}};
}
EnergyResult RegionalStep(EnergyFieldState& field,
                          std::uint64_t tick,
                          const EnergyRecords& records,
                          std::span<const RegionKey> due,
                          const EnergyAges& ages) {
    EnergyResult result;
    EXPECT_TRUE(field.TickRegional(tick, records, due, ages, result));
    return result;
}

namespace Emitters = Luminumbra::Systems;
namespace EmitterComp = Luminumbra::Components;
entt::entity RegionalEmitter(entt::registry& registry,
                             Luminumbra::Vec3 position,
                             int radius = 0,
                             std::uint32_t rate = 123) {
    const auto entity = registry.create();
    registry.emplace<EmitterComp::TransformComponent>(entity).position = position;
    auto& emitter = registry.emplace<EmitterComp::FieldEmitterComponent>(entity);
    emitter.rate_raw_per_tick = rate;
    emitter.radius_cells = radius;
    return entity;
}

TEST(RegionalEnergy, EmitterGatherSkipsNonfiniteAndOutOfWorldTransformsBeforeQuantizing) {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();
    const std::array invalid{Luminumbra::Vec3{nan, 0, 0},
                             Luminumbra::Vec3{0, nan, 0},
                             Luminumbra::Vec3{0, 0, nan},
                             Luminumbra::Vec3{infinity, 0, 0},
                             Luminumbra::Vec3{0, -infinity, 0},
                             Luminumbra::Vec3{0, 0, -infinity},
                             Luminumbra::Vec3{std::numeric_limits<float>::max(), 0, 0},
                             Luminumbra::Vec3{0, 0, std::numeric_limits<float>::lowest()},
                             Luminumbra::Vec3{16777216.0f, 0, 0},
                             Luminumbra::Vec3{0, 0, -16777218.0f}};
    for (const auto& position : invalid) {
        entt::registry registry;
        RegionalEmitter(registry, {0, 0, 0});
        RegionalEmitter(registry, position);
        EnergyFieldState field;
        const auto gathered = Emitters::GatherRegionalFieldEmitterDeposits(registry, field);
        EXPECT_TRUE(gathered.accepted);
        EXPECT_EQ(gathered.invalid_emitters, 1u);
        EXPECT_EQ(gathered.queued.emitters, 1u);
        EXPECT_EQ(gathered.queued.deposits, 1u);
        const std::array due{RegionKey{0, 0}};
        RegionalStep(field, 1, TwoEnergyOwners(), due, {});
        EnergyFieldState expected;
        ASSERT_TRUE(expected.QueueRegionalDeposit(0, 0, 0, 0, 123));
        RegionalStep(expected, 1, TwoEnergyOwners(), due, {});
        EXPECT_EQ(field.CanonicalRegionalBytes(), expected.CanonicalRegionalBytes());
        EXPECT_EQ(field.SerializeRegionalRecord(), expected.SerializeRegionalRecord());
    }
}

TEST(RegionalEnergy, EmitterGatherSkipsInvalidRadiusChannelAndOutOfWorldFootprints) {
    entt::registry registry;
    for (const int radius :
         {-1, 32, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()})
        RegionalEmitter(registry, {0, 0, 0}, radius);
    RegionalEmitter(registry, {-16777216.0f, 0, 0}, 1);
    RegionalEmitter(registry, {0, 0, 16777215.0f}, 1);
    const auto bad_channel = RegionalEmitter(registry, {0, 0, 0});
    registry.get<EmitterComp::FieldEmitterComponent>(bad_channel).channel = 1;
    RegionalEmitter(registry, {0, 0, 0}, 31, std::numeric_limits<std::uint32_t>::max());
    EnergyFieldState field;
    const auto gathered = Emitters::GatherRegionalFieldEmitterDeposits(registry, field);
    EXPECT_TRUE(gathered.accepted);
    EXPECT_EQ(gathered.invalid_emitters, 7u);
    EXPECT_EQ(gathered.queued.emitters, 1u);
    EXPECT_EQ(gathered.queued.deposits, 63u * 63u);
}

// A maximum-radius emitter with all rate bits set contributes exactly 63^2
// nonzero deposits. Fill the remainder with centre-only emitters: a small roster
// exercises the actual gather cap without creating half a million entities.
void FillRegionalEmitterCapacity(entt::registry& registry) {
    constexpr std::size_t footprint = 63u * 63u;
    for (std::size_t i = 0; i < EnergyFieldState::kMaxRegionalDeposits / footprint; ++i)
        RegionalEmitter(registry, {0, 0, 0}, 31, std::numeric_limits<std::uint32_t>::max());
    for (std::size_t i = 0; i < EnergyFieldState::kMaxRegionalDeposits % footprint; ++i)
        RegionalEmitter(registry, {0, 0, 0});
}

TEST(RegionalEnergy, EmitterBatchAtCapSucceedsAndOverCapRefusalRecoversWithoutQueuedResidue) {
    entt::registry registry;
    FillRegionalEmitterCapacity(registry);
    EnergyFieldState field;
    const auto at_cap = Emitters::GatherRegionalFieldEmitterDeposits(registry, field);
    ASSERT_TRUE(at_cap.accepted);
    EXPECT_EQ(at_cap.queued.deposits, EnergyFieldState::kMaxRegionalDeposits);
    // No due owners: consume and account the entire accepted queue as clipped.
    const auto consumed = RegionalStep(field, 1, TwoEnergyOwners(), {}, {});
    EXPECT_GT(consumed.clipped_raw, 0u);
    EXPECT_FALSE(field.OwnsRegion({0, 0}));

    // Keep this queue-retention probe inside its owner: cell 21 borders owner 0
    // and exchanges on common phases even before an intrinsic diffusion fire.
    ASSERT_TRUE(field.QueueRegionalDeposit(0, 22, 0, 0, 17));
    const auto extra = RegionalEmitter(registry, {0, 0, 0});
    const auto before = field.SerializeRegionalRecord();
    const auto refused = Emitters::GatherRegionalFieldEmitterDeposits(registry, field);
    EXPECT_FALSE(refused.accepted);
    EXPECT_EQ(refused.queued.deposits, 0u);
    EXPECT_EQ(field.SerializeRegionalRecord(), before);
    EXPECT_FALSE(field.OwnsRegion({0, 0}));
    EXPECT_TRUE(field.OwnsRegion({1, 0})); // prior queue retained, rejected batch absent
    const std::array due{RegionKey{1, 0}};
    const auto recovered = RegionalStep(field, 2, TwoEnergyOwners(), due, {});
    EXPECT_EQ(field.at_cell(22, 0), 17u);
    EXPECT_EQ(field.total_raw(), 17u);
    registry.destroy(extra);
    const auto retry = Emitters::GatherRegionalFieldEmitterDeposits(registry, field);
    EXPECT_TRUE(retry.accepted);
    EXPECT_EQ(retry.queued.deposits, EnergyFieldState::kMaxRegionalDeposits);
    EXPECT_GT(RegionalStep(field, 3, TwoEnergyOwners(), {}, recovered.ages).clipped_raw, 0u);
    EXPECT_EQ(field.total_raw(), 17u);
}

TEST(RegionalEnergy, BatchAdmissionIncludesPriorQueueAndRollsBackInvalidMiddleDeposit) {
    EnergyFieldState field;
    ASSERT_TRUE(field.QueueRegionalDeposit(1, 21, 0, 0, 17));
    const std::array invalid_batch{EnergyFieldState::RegionalDeposit{2, 0, 0, 0, 123},
                                   EnergyFieldState::RegionalDeposit{3, 0, 0, -1, 456}};
    EXPECT_FALSE(field.QueueRegionalDeposits(invalid_batch));
    EXPECT_FALSE(field.OwnsRegion({0, 0}));
    EXPECT_TRUE(field.OwnsRegion({1, 0}));
    entt::registry registry;
    FillRegionalEmitterCapacity(registry);
    EXPECT_FALSE(Emitters::GatherRegionalFieldEmitterDeposits(registry, field).accepted);
    EXPECT_FALSE(field.OwnsRegion({0, 0}));
    const std::array due{RegionKey{1, 0}};
    RegionalStep(field, 1, TwoEnergyOwners(), due, {});
    EXPECT_EQ(field.total_raw(), 17u);
    EXPECT_TRUE(Emitters::GatherRegionalFieldEmitterDeposits(registry, field).accepted);
}

TEST(RegionalEnergy, RefusedTickRetainsGatherAndFollowingValidTickConsumesItExactlyOnce) {
    entt::registry registry;
    RegionalEmitter(registry, {0, 0, 0});
    EnergyFieldState field;
    ASSERT_TRUE(Emitters::GatherRegionalFieldEmitterDeposits(registry, field).accepted);
    EnergyResult output;
    output.clipped_raw = 91;
    const std::array due{RegionKey{0, 0}};
    const EnergyAges invalid_ages{{{0, 0}, WorldClock::kTickLimit - 1}};
    EXPECT_FALSE(field.TickRegional(1, TwoEnergyOwners(), due, invalid_ages, output));
    EXPECT_EQ(output.clipped_raw, 91u);
    EXPECT_TRUE(field.CanonicalRegionalBytes().empty());
    EXPECT_TRUE(field.OwnsRegion({0, 0}));
    const auto recovered = RegionalStep(field, 2, TwoEnergyOwners(), due, {});
    EXPECT_EQ(field.total_raw(), 123u);
    RegionalStep(field, 3, TwoEnergyOwners(), due, recovered.ages);
    EXPECT_EQ(field.total_raw(), 123u);
}

TEST(RegionalEnergy, CentreOwnershipSplitsOnePhysicalPageAndHandlesNegativeCells) {
    EXPECT_EQ(EnergyFieldState::OwnerOfCell(20, 0), (RegionKey{0, 0}));
    EXPECT_EQ(EnergyFieldState::OwnerOfCell(21, 0), (RegionKey{1, 0}));
    EXPECT_EQ(EnergyFieldState::OwnerOfCell(-1, -22), (RegionKey{-1, -2}));
    EXPECT_EQ(EnergyFieldState::OwnerOfCell(-21, -21), (RegionKey{-1, -1}));
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 1 0 4:60000 5:12345\n"));
    ASSERT_EQ(field.page_count(), 1u);
    EXPECT_EQ(field.RegionBytes({0, 0}), "aether_state:v1:1:P1,0,0:4=60000;");
    EXPECT_EQ(field.RegionBytes({1, 0}), "aether_state:v1:1:P1,0,0:5=12345;");
    EXPECT_TRUE(field.OwnsRegion({0, 0}));
    EXPECT_TRUE(field.OwnsRegion({1, 0}));
    EXPECT_FALSE(field.OwnsRegion({2, 0}));
}

TEST(RegionalEnergy, FrozenOwnerCellsAndAgeStayExactOnAMixedOwnerPage) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 1 0 4:60000 5:12345\n"));
    auto records = TwoEnergyOwners();
    records.at({1, 0}).state = RegionState::Frozen;
    records.at({1, 0}).cadence_shift = 2;
    EnergyAges ages{{{0, 0}, 0}, {{1, 0}, 17}};
    const auto frozen = field.RegionBytes({1, 0});
    const std::array due{RegionKey{0, 0}};
    for (std::uint64_t tick = 1; tick <= 40; ++tick) {
        field.QueueDeposit(1, 21, 0, 0, 900); // refused target never changes frozen cells
        auto result = RegionalStep(field, tick, records, due, ages);
        EXPECT_EQ(result.clipped_raw, 900u);
        ages = result.ages;
        EXPECT_EQ(ages.at({0, 0}), tick);
        EXPECT_EQ(ages.at({1, 0}), 17u);
        EXPECT_EQ(field.RegionBytes({1, 0}), frozen);
        for (const auto& work : result.work)
            EXPECT_NE(work.key, (RegionKey{1, 0}));
    }
    EXPECT_EQ(field.at_cell(21, 0), 12345u);
}

TEST(RegionalEnergy, CommonBorderRunsOutsideUnequalInteriorPhasesAndConservesRaw) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 1 0 4:16000\n"));
    auto records = TwoEnergyOwners();
    records.at({0, 0}).state = RegionState::Reduced;
    records.at({0, 0}).cadence_shift = 1;
    records.at({0, 0}).cadence_phase = 1;
    records.at({1, 0}).state = RegionState::Reduced;
    records.at({1, 0}).cadence_shift = 2;
    records.at({1, 0}).cadence_phase = 2;
    const EnergyAges ages{{{0, 0}, 3}, {{1, 0}, 5}};
    const auto initial = field.CanonicalRegionalBytes();
    auto result = RegionalStep(field, 3, records, {}, ages);
    EXPECT_EQ(field.CanonicalRegionalBytes(), initial);
    EXPECT_EQ(result.ages, ages);
    EXPECT_TRUE(result.work.empty());
    // Neither owner's interior phase is due at tick 4. The border still runs.
    result = RegionalStep(field, 4, records, {}, ages);
    EXPECT_EQ(result.ages, ages);
    EXPECT_EQ(field.total_raw(), 16000u);
    EXPECT_GT(field.at_cell(21, 0), 0u);
    EXPECT_EQ(field.at_cell(19, 0), 0u); // no same-owner interior diffusion
    ASSERT_EQ(result.work.size(), 2u);
    EXPECT_EQ(result.work[0].key, (RegionKey{0, 0}));
    EXPECT_EQ(result.work[1].key, (RegionKey{1, 0}));
    EXPECT_EQ(result.work[0].page_updates, 1u);
    EXPECT_EQ(result.work[1].page_updates, 1u);
    const auto after_border = field.CanonicalRegionalBytes();
    result = RegionalStep(field, 5, records, {}, ages);
    EXPECT_EQ(field.CanonicalRegionalBytes(), after_border);
}

TEST(RegionalEnergy, AbsentOwnersAndAbsentPagesHaveSealedEdges) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 1 0 4:16000\n"));
    const EnergyRecords records{{{0, 0}, ActiveRegionRecord{}}};
    const std::array due{RegionKey{0, 0}};
    const EnergyAges ages{{{0, 0}, 7}};
    const auto result = RegionalStep(field, 1000000, records, due, ages);
    EXPECT_EQ(result.ages.at({0, 0}), 8u);
    EXPECT_EQ(field.total_raw(), DecayOnceRef(16000));
    EXPECT_EQ(field.at_cell(21, 0), 0u);
    EXPECT_EQ(field.page_count(), 1u);
    EXPECT_FALSE(field.OwnsRegion({1, 0}));
}

TEST(RegionalEnergy, IntrinsicOneFireRoundingAndNoAbsoluteCatchup) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 0 0 0:1 17:255 34:65535\n"));
    const EnergyRecords records{{{0, 0}, ActiveRegionRecord{}}};
    const std::array due{RegionKey{0, 0}};
    const EnergyAges ages{{{0, 0}, 7}};
    const auto expected = std::uint64_t{DecayOnceRef(1)} + DecayOnceRef(255) + DecayOnceRef(65535);
    const auto result = RegionalStep(field, 3000000, records, due, ages);
    EXPECT_EQ(result.ages.at({0, 0}), 8u);
    EXPECT_EQ(field.total_raw(), expected);
    EXPECT_EQ(field.fires_completed(), 0u);
}

TEST(RegionalEnergy, EmptyEnergyAgeContinuesAcrossSaveAndReopenBeforeLaterDeposit) {
    ActiveRegionLedger ledger;
    ledger.mark_edited({0, 0}, WorldClock{});
    EnergyFieldState field;
    const std::array due{RegionKey{0, 0}};
    for (std::uint64_t tick = 1; tick <= 7; ++tick) {
        auto result = RegionalStep(field, tick, ledger.records(), due, ledger.energy_ages());
        ASSERT_TRUE(ledger.set_energy_ages(result.ages));
    }
    EXPECT_TRUE(field.SerializeRegionalRecord().empty());
    EXPECT_TRUE(field.CanonicalRegionalBytes().empty());
    ASSERT_EQ(ledger.energy_ages().at({0, 0}), 7u);
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), restored, error)) << error;
    EnergyFieldState reopened; // indexed absent EFS; do not reset the ARL age
    field.QueueDeposit(1, 1, 1, 0, 1025);
    reopened.QueueDeposit(1, 1, 1, 0, 1025);
    const auto uninterrupted = RegionalStep(field, 8, ledger.records(), due, ledger.energy_ages());
    const auto resumed =
        RegionalStep(reopened, 800000, restored.records(), due, restored.energy_ages());
    EXPECT_EQ(uninterrupted.ages, resumed.ages);
    EXPECT_EQ(resumed.ages.at({0, 0}), 8u);
    EXPECT_EQ(reopened.total_raw(), DecayOnceRef(1025));
    EXPECT_EQ(reopened.SerializeRegionalRecord(), field.SerializeRegionalRecord());
}

TEST(RegionalEnergy, SaveAndHashesReadFrozenStoredValuesWithoutNormalization) {
    // Build legacy stale pages to prove regional observation bypasses NormalizeAllPages.
    EnergyFieldState field;
    field.SetAnchorCell(4096, 4096);
    field.QueueDeposit(1, 4096, 4096, 0, 51234);
    field.Tick(1);
    field.SetAnchorCell(0, 0);
    field.Tick(800);
    const auto legacy_before = field.CanonicalBytes();
    const auto fires = field.fires_completed();
    const auto next = field.next_fire_tick();
    const auto regional = field.CanonicalRegionalBytes();
    const auto owner = EnergyFieldState::OwnerOfCell(4096, 4096);
    const auto owner_bytes = field.RegionBytes(owner);
    const auto saved = field.SerializeRegionalRecord();
    EXPECT_EQ(saved, "EFS1 1 0\nP 256 256 0:51234\n");
    EXPECT_EQ(field.CanonicalBytes(), legacy_before);
    EXPECT_EQ(field.at_cell(4096, 4096), 51234u);
    EXPECT_EQ(field.fires_completed(), fires);
    EXPECT_EQ(field.next_fire_tick(), next);
    EnergyFieldState reopened;
    ASSERT_TRUE(reopened.DeserializeRegionalRecord(saved));
    EXPECT_EQ(reopened.CanonicalRegionalBytes(), regional);
    EXPECT_EQ(reopened.RegionBytes(owner), owner_bytes);
    EXPECT_EQ(reopened.SerializeRegionalRecord(), saved);
}

TEST(RegionalEnergy, AgeOverflowAndBadDueRefuseAtomicallyIncludingPendingDeposits) {
    EnergyFieldState field;
    const auto records = TwoEnergyOwners();
    const std::array due{RegionKey{0, 0}, RegionKey{1, 0}};
    field.QueueDeposit(1, 1, 1, 0, 2000);
    EnergyResult output;
    output.clipped_raw = 99;
    output.ages = {{{9, 9}, 42}};
    output.work = {{{9, 9}, 1, 2, 3, 4}};
    const EnergyAges bad_ages{{{0, 0}, 4}, {{1, 0}, WorldClock::kTickLimit - 1}};
    EXPECT_FALSE(field.TickRegional(500, records, due, bad_ages, output));
    EXPECT_EQ(output.clipped_raw, 99u);
    EXPECT_EQ(output.ages, (EnergyAges{{{9, 9}, 42}}));
    ASSERT_EQ(output.work.size(), 1u);
    EXPECT_EQ(output.work[0].page_updates, 4u);
    EXPECT_TRUE(field.SerializeRegionalRecord().empty());
    EXPECT_TRUE(field.OwnsRegion({0, 0})); // pending deposit preserved
    const std::array reversed{RegionKey{1, 0}, RegionKey{0, 0}};
    const std::array duplicate{RegionKey{0, 0}, RegionKey{0, 0}};
    const std::array orphan{RegionKey{2, 0}};
    EXPECT_FALSE(field.TickRegional(500, records, reversed, {}, output));
    EXPECT_FALSE(field.TickRegional(500, records, duplicate, {}, output));
    EXPECT_FALSE(field.TickRegional(500, records, orphan, {}, output));
    EXPECT_FALSE(field.TickRegional(WorldClock::kTickLimit, records, due, {}, output));
    auto frozen = records;
    frozen.at({0, 0}).state = RegionState::Frozen;
    EXPECT_FALSE(field.TickRegional(500, frozen, due, {}, output));
    auto result = RegionalStep(field, 500, records, due, {});
    EXPECT_EQ(result.ages.at({0, 0}), 1u);
    EXPECT_EQ(field.at_cell(1, 1), 2000u);
}

TEST(RegionalEnergy, SortedDepositsSaturateWithoutWrappingAndRegistrationOrderIsNeutral) {
    const EnergyRecords records{{{0, 0}, ActiveRegionRecord{}}};
    const std::array due{RegionKey{0, 0}};
    EnergyFieldState a, b;
    a.QueueDeposit(8, 1, 1, 0, UINT32_MAX);
    a.QueueDeposit(3, 1, 1, 0, 7);
    b.QueueDeposit(3, 1, 1, 0, 7);
    b.QueueDeposit(8, 1, 1, 0, UINT32_MAX);
    const auto ra = RegionalStep(a, 1, records, due, {});
    const auto rb = RegionalStep(b, 1, records, due, {});
    EXPECT_EQ(ra.clipped_raw, std::uint64_t{UINT32_MAX} + 7 - 65535);
    EXPECT_EQ(ra.clipped_raw, rb.clipped_raw);
    EXPECT_EQ(a.at_cell(1, 1), 65535u);
    EXPECT_EQ(a.SerializeRegionalRecord(), b.SerializeRegionalRecord());
}

TEST(RegionalEnergy, RegionalDecoderRefusesNoncanonicalPresentRecordsAtomically) {
    EnergyFieldState field;
    const std::string valid = "EFS1 2 0\nP -1 0 30:12 31:345\nP 1 0 4:1\n";
    ASSERT_TRUE(field.DeserializeRegionalRecord(valid));
    const auto before = field.SerializeRegionalRecord();
    const std::vector<std::string> refused{"",
                                           "EFS1 1 0\n",
                                           "EFS1 1 1\nP 0 0 0:1\n",
                                           "EFS1 0 0\nP 0 0 0:1\n",
                                           "EFS1 65 0\nP 0 0 0:1\n",
                                           "EFS1 2147483648 0\nP 0 0 0:1\n",
                                           "EFS1 1 0\nP 0 0\n",
                                           "EFS1 1 0\nP 0 0 0:0\n",
                                           "EFS1 1 0\nP 0 0 0:65536\n",
                                           "EFS1 1 0\nP 0 0 256:1\n",
                                           "EFS1 1 0\nP 0 0 0:1 0:2\n",
                                           "EFS1 1 0\nP 0 0 1:2 0:1\n",
                                           "EFS1 1 0\nP 0 0 0:1\nP 0 0 1:2\n",
                                           "EFS1 1 0\nP 1 0 0:1\nP 0 0 1:2\n",
                                           "EFS1 1 0\nP 0 0 0:1junk\n",
                                           "EFS1 1 0\nP 0 0 -1:1\n",
                                           "EFS1 1 0\nP -0 0 0:1\n",
                                           "EFS1 1 0\nP 0 0 00:1\n",
                                           "EFS1 1 0\nP 0 0 0:1",
                                           "EFS1 1 0\nP 0 0 0:1\n\n",
                                           "EFS1 1 0\nP 2147483647 0 0:1\n",
                                           "EFS1 1 0\nP -43691 0 0:1\n",
                                           "EFS1 1 0 extra\nP 0 0 0:1\n",
                                           "EFS1 1 0\nP 0 0 0:1\ntrailer\n"};
    for (const auto& record : refused) {
        EXPECT_FALSE(field.DeserializeRegionalRecord(record)) << record;
        EXPECT_EQ(field.SerializeRegionalRecord(), before);
    }
    EXPECT_FALSE(field.DeserializeRegionalRecord(
        std::string(EnergyFieldState::kMaxRegionalRecordBytes + 1, 'x')));
    EXPECT_EQ(field.SerializeRegionalRecord(), before);
}

TEST(RegionalEnergy, JointRestoreRequiresAgesForEveryNonzeroOwner) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 1 0 4:1 5:2\n"));
    const auto records = TwoEnergyOwners();
    EXPECT_FALSE(field.ValidateRegionalOwners(records, {{{0, 0}, 7}}));
    EXPECT_FALSE(
        field.ValidateRegionalOwners({{{0, 0}, ActiveRegionRecord{}}}, {{{0, 0}, 7}, {{1, 0}, 2}}));
    EXPECT_FALSE(
        field.ValidateRegionalOwners(records, {{{0, 0}, 7}, {{1, 0}, WorldClock::kTickLimit}}));
    EXPECT_TRUE(field.ValidateRegionalOwners(records, {{{0, 0}, 7}, {{1, 0}, 2}}));
}

TEST(RegionalEnergy, FlagOffFixtureAndLegacyHeaderRemainUnchanged) {
    EnergyFieldState legacy(2);
    legacy.SetAnchorCell(0, 0);
    legacy.QueueDeposit(1, -1, 0, 0, 123);
    legacy.QueueDeposit(2, 0, 0, 1, 456);
    legacy.Tick(1);
    const std::string fixture = "EFS1 2 7\nP -1 0 30:123\nP 0 0 1:456\n";
    EXPECT_EQ(legacy.SerializeRecord(1), fixture);
    EXPECT_EQ(legacy.CanonicalBytes(), "aether_state:v1:2:P-1,0,0:30=123;P0,0,0:1=456;");
    EnergyFieldState restored;
    ASSERT_TRUE(restored.DeserializeRecord(fixture, 1));
    EXPECT_EQ(restored.SerializeRecord(1), fixture);
    EXPECT_FALSE(restored.DeserializeRegionalRecord(fixture));
    EXPECT_EQ(restored.SerializeRecord(1), fixture);
    EnergyFieldState empty;
    EXPECT_EQ(empty.SerializeRecord(0), "EFS1 1 8\n");
    EXPECT_TRUE(empty.SerializeRegionalRecord().empty());
}

TEST(RegionalEnergy, DecodedCapacityRefusesGrowthWithoutConsumingPendingInput) {
    constexpr int channels = 64;
    const auto max_pages =
        EnergyFieldState::kMaxRegionalValueBytes /
        (std::size_t{kEnergyPageCells * kEnergyPageCells} * channels * sizeof(std::uint16_t));
    std::string full = "EFS1 64 0\n";
    EnergyRecords records;
    EnergyAges ages;
    for (std::size_t page = 0; page < max_pages; ++page) {
        full += "P " + std::to_string(page) + " 0 0:1\n";
        const auto owner =
            EnergyFieldState::OwnerOfCell(static_cast<int>(page) * kEnergyPageCells, 0);
        records.try_emplace(owner);
        ages.try_emplace(owner, 1);
    }
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord(full));
    EXPECT_EQ(field.channels(), channels);
    const auto next_cell = static_cast<int>(max_pages) * kEnergyPageCells;
    const auto next_owner = EnergyFieldState::OwnerOfCell(next_cell, 0);
    records.try_emplace(next_owner);
    const std::array due{next_owner};
    ASSERT_TRUE(field.QueueRegionalDeposit(1, next_cell, 0, 0, 777));
    EnergyResult output;
    output.clipped_raw = 42;
    EXPECT_FALSE(field.TickRegional(1, records, due, ages, output));
    EXPECT_EQ(output.clipped_raw, 42u);
    EXPECT_TRUE(output.ages.empty());
    EXPECT_EQ(field.SerializeRegionalRecord(), full);
    EXPECT_TRUE(field.OwnsRegion(next_owner)); // queued input survived capacity refusal
    const auto over_cap = full + "P " + std::to_string(max_pages) + " 0 0:1\n";
    EXPECT_FALSE(field.DeserializeRegionalRecord(over_cap));
    EXPECT_EQ(field.SerializeRegionalRecord(), full);
    EXPECT_TRUE(field.OwnsRegion(next_owner));
}

TEST(RegionalEnergy, RegionalDepositAdmissionChecksChannelWorldAndPendingCapacity) {
    EnergyFieldState field(2);
    EXPECT_FALSE(field.QueueRegionalDeposit(1, 0, 0, -1, 1));
    EXPECT_FALSE(field.QueueRegionalDeposit(1, 0, 0, 2, 1));
    EXPECT_FALSE(field.QueueRegionalDeposit(1, std::numeric_limits<int>::max(), 0, 0, 1));
    EXPECT_FALSE(field.QueueRegionalDeposit(1, 0, std::numeric_limits<int>::min(), 0, 1));
    EXPECT_FALSE(field.OwnsRegion({0, 0}));
    EXPECT_TRUE(field.QueueRegionalDeposit(1, 0, 0, 0, 0));
    EXPECT_FALSE(field.OwnsRegion({0, 0}));
    for (std::size_t i = 0; i < EnergyFieldState::kMaxRegionalDeposits; ++i)
        ASSERT_TRUE(field.QueueRegionalDeposit(i, 0, 0, 0, 1));
    EXPECT_FALSE(field.QueueRegionalDeposit(0, 21, 0, 0, 1));
    EXPECT_TRUE(field.OwnsRegion({0, 0}));
    EXPECT_FALSE(field.OwnsRegion({1, 0}));
    EXPECT_TRUE(field.SerializeRegionalRecord().empty());
    // Direct legacy queue injection is still refused by TickRegional before
    // consuming deposits; the enabled gatherer never uses that admission path.
    field.QueueDeposit(0, 21, 0, 0, 1);
    EnergyResult output;
    output.clipped_raw = 91;
    EXPECT_FALSE(field.TickRegional(1, TwoEnergyOwners(), {}, {}, output));
    EXPECT_EQ(output.clipped_raw, 91u);
    EXPECT_TRUE(field.OwnsRegion({1, 0}));
}

TEST(RegionalEnergy, BorderAcquisitionInitializesZeroAgeWithoutAnInteriorAdvance) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 1 0 4:1000\n"));
    const auto records = TwoEnergyOwners();
    const auto result = RegionalStep(field, 20, records, {}, {{{0, 0}, 3}});
    EXPECT_EQ(result.ages.at({0, 0}), 3u);
    EXPECT_EQ(result.ages.at({1, 0}), 0u);
    EXPECT_GT(field.at_cell(21, 0), 0u);
    EXPECT_EQ(field.total_raw(), 1000u);
    EXPECT_TRUE(field.ValidateRegionalOwners(records, result.ages));
    ASSERT_EQ(result.work.size(), 2u);
    EXPECT_EQ(result.work[0].key, (RegionKey{0, 0}));
    EXPECT_EQ(result.work[0].page_updates, 1u); // existing age, one physical page
    EXPECT_EQ(result.work[1].key, (RegionKey{1, 0}));
    EXPECT_EQ(result.work[1].page_updates, 2u); // one page plus first zero-age register
}

TEST(RegionalEnergy, RegionalMultiChannelStoredContinuationMatchesAtEveryCadence) {
    const auto records = TwoEnergyOwners();
    const std::array due{RegionKey{0, 0}, RegionKey{1, 0}};
    EnergyFieldState uninterrupted(2), reopened(2);
    EnergyAges a, b;
    for (std::uint64_t tick = 1; tick <= 24; ++tick) {
        ASSERT_TRUE(uninterrupted.QueueRegionalDeposit(tick, 20, 0, 0, 300));
        ASSERT_TRUE(reopened.QueueRegionalDeposit(tick, 20, 0, 0, 300));
        ASSERT_TRUE(uninterrupted.QueueRegionalDeposit(tick + 90, 21, 0, 1, 600));
        ASSERT_TRUE(reopened.QueueRegionalDeposit(tick + 90, 21, 0, 1, 600));
        a = RegionalStep(uninterrupted, tick, records, due, a).ages;
        b = RegionalStep(reopened, tick, records, due, b).ages;
        const auto record = reopened.SerializeRegionalRecord();
        ASSERT_TRUE(reopened.DeserializeRegionalRecord(record));
        ASSERT_TRUE(reopened.ValidateRegionalOwners(records, b));
        EXPECT_EQ(reopened.SerializeRegionalRecord(), uninterrupted.SerializeRegionalRecord());
        EXPECT_EQ(reopened.CanonicalRegionalBytes(), uninterrupted.CanonicalRegionalBytes());
        EXPECT_EQ(a, b);
    }
}

} // namespace

TEST(RegionalEnergy, EmptyDueOwnersCountEachPersistentAgeUpdate) {
    EnergyFieldState field;
    const auto records = TwoEnergyOwners();
    const std::array due{RegionKey{0, 0}, RegionKey{1, 0}};
    const auto result = RegionalStep(field, 1, records, due, {});
    EXPECT_TRUE(field.SerializeRegionalRecord().empty());
    EXPECT_EQ(field.page_count(), 0u);
    ASSERT_EQ(result.work.size(), 2u);
    for (std::size_t i = 0; i < due.size(); ++i) {
        EXPECT_EQ(result.work[i].key, due[i]);
        EXPECT_EQ(result.work[i].page_updates, 1u);
        EXPECT_EQ(result.work[i].region_ticks, 0u);
        EXPECT_EQ(result.work[i].entity_updates, 0u);
        EXPECT_EQ(result.work[i].water_cells, 0u);
        EXPECT_EQ(result.ages.at(due[i]), 1u);
    }
}
