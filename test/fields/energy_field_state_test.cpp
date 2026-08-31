// AetherEmitterDeterminism provides proving signals for the stateful energy-field
// layer. The kernel constants are public and pinned; the tests
// replicate single ops (one decay step) from those constants where an expected
// value is needed — never from a parallel implementation of the kernel.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "luminumbra_common/fields/EnergyFieldState.h"

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
    // Codex r2 correction: page-in catch-up decay must be the SEQUENTIAL
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

} // namespace
