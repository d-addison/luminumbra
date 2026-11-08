// AetherEmitterDeterminism provides proving signals for the stateful energy-field
// layer. The kernel constants are public and pinned; the tests
// replicate single ops (one decay step) from those constants where an expected
// value is needed — never from a parallel implementation of the kernel.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/fields/ScalarFieldDiffusion.h"
#include "luminumbra_common/systems/FieldEmitterSystem.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/WorldClock.h"

namespace luminumbra::fields {

struct EnergyFieldStateTestAccess {
    static bool Validated(const EnergyFieldState& field) {
        return field.m_regional_validation.has_value();
    }
    static void Invalidate(EnergyFieldState& field) {
        field.m_regional_validation.reset();
    }
    static void ForgetPageFacts(EnergyFieldState& field, int px, int pz) {
        field.m_pages.at(EnergyFieldState::PackPageKey(px, pz)).regional_summary.reset();
    }
    static bool HasPageFacts(const EnergyFieldState& field, int px, int pz) {
        return field.m_pages.at(EnergyFieldState::PackPageKey(px, pz)).regional_summary.has_value();
    }
    static void TamperPageShape(EnergyFieldState& field, int px, int pz) {
        field.m_pages.at(EnergyFieldState::PackPageKey(px, pz)).values.pop_back();
    }
    static void
    SetOriginChannel(EnergyFieldState& field, std::size_t channel, std::uint16_t value) {
        field.m_pages.at(EnergyFieldState::PackPageKey(0, 0)).values[channel] = value;
        ForgetPageFacts(field, 0, 0);
        Invalidate(field);
    }
    static void Normalize(EnergyFieldState& field) {
        field.NormalizeAllPages();
    }
};

} // namespace luminumbra::fields

namespace {

using luminumbra::fields::EnergyFieldState;
using luminumbra::fields::EnergyFieldStateTestAccess;
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
        const auto amount = static_cast<std::uint32_t>(i);
        f.QueueDeposit(7, i * 3 - 6, -i * 2 + 4, 0, 9000u + 100u * amount);
        f.QueueDeposit(3, i * 3 - 6, -i * 2 + 4, 0, 1000u + amount);
        deposited += 9000u + 100u * amount + 1000u + amount;
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

TEST(AetherEmitterDeterminism, FullWidthDepositsSaturateBothChannelsWithoutWrapping) {
    constexpr std::uint32_t kMaxDeposit = (std::numeric_limits<std::uint32_t>::max)();
    for (const std::uint32_t stored : {0u, 1u, 65534u, 65535u}) {
        for (const std::uint32_t amount : {0u,
                                           1u,
                                           65534u,
                                           65535u,
                                           65536u,
                                           kMaxDeposit - 65535u,
                                           kMaxDeposit - 1u,
                                           kMaxDeposit}) {
            SCOPED_TRACE(stored);
            SCOPED_TRACE(amount);
            EnergyFieldState field(2);
            field.SetAnchorCell(0, 0);
            for (int channel = 0; channel < 2; ++channel) {
                field.QueueDeposit(1, 0, 0, channel, stored);
                field.QueueDeposit(2, 0, 0, channel, amount);
            }
            const std::uint64_t sum = static_cast<std::uint64_t>(stored) + amount;
            const std::uint64_t retained = sum > 65535u ? 65535u : sum;
            EXPECT_EQ(field.Tick(1), 2u * (sum - retained));
            EXPECT_EQ(field.at_cell(0, 0, 0), retained);
            EXPECT_EQ(field.at_cell(0, 0, 1), retained);
            EXPECT_EQ(field.total_raw(), 2u * retained);
            EXPECT_EQ(field.Tick(2), 0u);
            EXPECT_EQ(field.total_raw(), 2u * retained);
        }
    }
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

} // namespace

namespace {
std::string EnergyPages(std::size_t count, int channels = 1) {
    std::string record = "EFS1 " + std::to_string(channels) + " 8\n";
    for (std::size_t page = 0; page < count; ++page)
        record += "P " + std::to_string(page) + " 0 0:1\n";
    return record;
}
} // namespace

TEST(AetherEmitterDeterminism, EnergyCompatibilityChannelsAndPagesAreAdmittedBeforeMutation) {
    using namespace luminumbra::fields;
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRecord("EFS1 16 8\nP 0 0 4095:65535\n", 0));
    const auto original = field.CanonicalBytes();
    EXPECT_FALSE(field.DeserializeRecord("EFS1 17 8\nP 0 0 4096:1\n", 0));
    EXPECT_EQ(field.CanonicalBytes(), original);
    ASSERT_TRUE(field.DeserializeRecord(EnergyPages(kEnergyReadPages), 0));
    EXPECT_EQ(field.page_count(), kEnergyReadPages);
    const auto accepted = field.CanonicalBytes();
    EXPECT_FALSE(field.DeserializeRecord(EnergyPages(kEnergyReadPages + 1), 0));
    EXPECT_EQ(field.CanonicalBytes(), accepted);
}

TEST(AetherEmitterDeterminism, EnergyWriterRefusalIsReadOnlyAndEveryWrittenRecordReloads) {
    using namespace luminumbra::fields;
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRecord(EnergyPages(kEnergyWritePages, kEnergyRecordChannels), 0));
    EnergyFieldState reloaded;
    ASSERT_TRUE(reloaded.DeserializeRecord(field.SerializeRecord(0), 0));
    EXPECT_EQ(reloaded.CanonicalBytes(), field.CanonicalBytes());
    ASSERT_TRUE(field.DeserializeRecord(EnergyPages(kEnergyWritePages + 1), 0));
    const auto original = field.CanonicalBytes();
    EXPECT_THROW(static_cast<void>(field.SerializeRecord(0)), std::length_error);
    EXPECT_EQ(field.CanonicalBytes(), original);
    EnergyFieldState too_many_channels(kEnergyRecordChannels + 1);
    EXPECT_THROW(static_cast<void>(too_many_channels.SerializeRecord(0)), std::length_error);
}

TEST(AetherEmitterDeterminism, EnergyRecordBytesAcceptExactLimitAndRefuseOneMoreBeforeParsing) {
    using namespace luminumbra::fields;
    // Whitespace padding leaves the historical record and cadence unchanged.
    std::string record = "EFS1 16 8";
    record.resize(kEnergyReadRecordBytes - 1, ' ');
    record += '\n';
    EXPECT_TRUE(EnergyFieldState::AdmitRecord(record));
    record += ' ';
    EXPECT_FALSE(EnergyFieldState::AdmitRecord(record));
    EnergyFieldState field;
    EXPECT_FALSE(field.DeserializeRecord(record, 0));
    EXPECT_EQ(field.page_count(), 0u);
}

TEST(AetherEmitterDeterminism, EnergyMalformedControlsAndPartialNumbersFailClosed) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRecord("EFS1 2 8\nP 0 0 0:20\n", 0));
    const auto original = field.CanonicalBytes();
    for (const auto* record : {"EFS1 2 8 ignored\n",
                               "EFS1 2 8\nP 0 0 0:1junk\n",
                               "EFS1 2 8\nP 0 0 0:1:2\n",
                               "EFS1 2 8\nP 0 0 -1:1\n",
                               "EFS1 2 8\nP 0 0 0:1\nP 0 0 1:1\n"}) {
        EXPECT_FALSE(field.DeserializeRecord(record, 0)) << record;
        EXPECT_EQ(field.CanonicalBytes(), original);
    }
    EXPECT_FALSE(field.DeserializeRecord("EFS1 2 8\n", std::numeric_limits<std::uint64_t>::max()));
}

TEST(AetherEmitterDeterminism, LargestEnergyWriterEncodingFitsItsDerivedByteLimitAndReloads) {
    using namespace luminumbra::fields;
    std::string cells;
    for (std::size_t index = 0; index < 256u * kEnergyRecordChannels; ++index)
        cells += " " + std::to_string(index) + ":65535";
    std::string record = "EFS1 16 8\n";
    record.reserve(kEnergyWriteRecordBytes);
    for (std::size_t page = 0; page < kEnergyWritePages; ++page) {
        const int coordinate = std::numeric_limits<int>::min() + static_cast<int>(page);
        record += "P " + std::to_string(coordinate) + " -2147483648" + cells + "\n";
    }
    ASSERT_EQ(record.size(), kEnergyWriteRecordBytes);
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRecord(record, 0));
    EXPECT_EQ(field.SerializeRecord(0), record);
    // The first extra page is refused before the writer could exceed its derived byte maximum.
    record += "P 0 0 0:1\n";
    ASSERT_TRUE(field.DeserializeRecord(record, 0));
    EXPECT_THROW(static_cast<void>(field.SerializeRecord(0)), std::length_error);
}

TEST(AetherEmitterDeterminism, EnergyWriterCountsOnlyPagesRemainingAfterFrozenDecay) {
    using namespace luminumbra::fields;
    for (const bool excess_decays : {false, true}) {
        SCOPED_TRACE(excess_decays);
        // All pages are frozen outside the active window. Exactly the excess
        // page has value 1 in the successful case; one firing decays it to zero.
        std::string record = "EFS1 1 8\n";
        for (std::size_t page = 0; page <= kEnergyWritePages; ++page)
            record += "P " + std::to_string(page + 100) +
                      " 0 0:" + (excess_decays && page == kEnergyWritePages ? "1\n" : "65535\n");
        EnergyFieldState field;
        ASSERT_TRUE(field.DeserializeRecord(record, 0));
        field.SetAnchorCell(0, 0);
        field.Tick(kEnergyCadenceTicks);
        ASSERT_EQ(field.page_count(), kEnergyWritePages + 1);
        const auto before = field.CanonicalBytes();
        if (excess_decays) {
            const auto bytes = field.SerializeRecord(kEnergyCadenceTicks);
            EXPECT_EQ(field.page_count(), kEnergyWritePages);
            EnergyFieldState loaded;
            ASSERT_TRUE(loaded.DeserializeRecord(bytes, 0));
            EXPECT_EQ(loaded.page_count(), kEnergyWritePages);
        } else {
            EXPECT_THROW(static_cast<void>(field.SerializeRecord(kEnergyCadenceTicks)),
                         std::length_error);
            EXPECT_EQ(field.CanonicalBytes(), before);
        }
    }
}

namespace {
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

TEST(RegionalEnergy, OwnershipQueriesOnlyOverlappingStoredValuesWithoutChangingBytes) {
    EnergyFieldState field(3);
    ASSERT_TRUE(field.DeserializeRegionalRecord(
        "EFS1 3 0\nP -2 -2 512:1 563:2\nP 1 1 206:3 257:4\nP 100 100 0:5\n"));
    const auto before = field.SerializeRegionalRecord();
    for (std::int32_t z = -3; z <= 3; ++z) {
        for (std::int32_t x = -3; x <= 3; ++x) {
            const RegionKey owner{x, z};
            SCOPED_TRACE(::testing::Message() << x << ',' << z);
            EXPECT_EQ(field.OwnsRegion(owner), !field.RegionBytes(owner).empty());
        }
    }
    EXPECT_FALSE(field.OwnsRegion({(std::numeric_limits<std::int32_t>::min)(), 0}));
    EXPECT_FALSE(field.OwnsRegion({0, (std::numeric_limits<std::int32_t>::max)()}));
    EXPECT_EQ(field.SerializeRegionalRecord(), before);
    ASSERT_TRUE(field.QueueRegionalDeposit(1, 86, -86, 2, 7));
    EXPECT_TRUE(field.OwnsRegion(EnergyFieldState::OwnerOfCell(86, -86)));
    EXPECT_EQ(field.SerializeRegionalRecord(), before);
    field.ResetEmptyAtTick(0);
    EXPECT_FALSE(field.OwnsRegion({0, 0}));
    EXPECT_FALSE(field.OwnsRegion(EnergyFieldState::OwnerOfCell(86, -86)));

    // Legacy records can contain empty pages and the extreme representable int cells.
    ASSERT_TRUE(field.DeserializeRecord("EFS1 3 " + std::to_string(kEnergyCadenceTicks) +
                                            "\nP -134217728 -134217728 2:1\nP 0 0\n"
                                            "P 134217727 134217727 767:2\n",
                                        0));
    const auto legacy = field.CanonicalBytes();
    for (const int coordinate :
         {(std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)()})
        EXPECT_TRUE(field.OwnsRegion(EnergyFieldState::OwnerOfCell(coordinate, coordinate)));
    EXPECT_FALSE(field.OwnsRegion({0, 0}));
    EXPECT_EQ(field.CanonicalBytes(), legacy);

    if constexpr (sizeof(std::size_t) == 4) {
        // Legacy channel admission rejects counts that could wrap page allocation.
        ASSERT_FALSE(field.DeserializeRecord(
            "EFS1 16777216 " + std::to_string(kEnergyCadenceTicks) + "\nP 0 0\n", 0));
        EXPECT_FALSE(field.OwnsRegion({0, 0}));
        EXPECT_EQ(field.CanonicalBytes(), legacy);
    }
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

TEST(RegionalEnergy, CommonBorderRunsOnSharedScheduledPhaseAndConservesRaw) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 1 0 4:16000\n"));
    auto records = TwoEnergyOwners();
    records.at({0, 0}).state = RegionState::Reduced;
    records.at({0, 0}).cadence_shift = 1;
    records.at({0, 0}).cadence_phase = 1;
    records.at({1, 0}).state = RegionState::Reduced;
    records.at({1, 0}).cadence_shift = 2;
    records.at({1, 0}).cadence_phase = 3;
    const EnergyAges ages{{{0, 0}, 3}, {{1, 0}, 5}};
    const auto initial = field.CanonicalRegionalBytes();
    auto result = RegionalStep(field, 2, records, {}, ages);
    EXPECT_EQ(field.CanonicalRegionalBytes(), initial);
    EXPECT_EQ(result.ages, ages);
    EXPECT_TRUE(result.work.empty());
    // Both owners are scheduled at tick 3, without an intrinsic energy firing.
    result = RegionalStep(field, 3, records, {}, ages);
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
    result = RegionalStep(field, 4, records, {}, ages);
    EXPECT_EQ(field.CanonicalRegionalBytes(), after_border);
}

TEST(RegionalEnergy, UnequalCadencesExchangeExactlyWhenBothNonzeroPhasesAreScheduled) {
    for (const bool separate_pages : {false, true}) {
        const RegionKey left{separate_pages ? 2 : 0, 0};
        const RegionKey right{left.x + 1, 0};
        const int border_cell = separate_pages ? 64 : 21;
        const std::string initial = separate_pages ? "EFS1 1 0\nP 3 0 15:16000\nP 4 0 255:1\n"
                                                   : "EFS1 1 0\nP 1 0 4:16000\n";
        for (const std::uint32_t right_phase : {1u, 2u, 3u}) {
            EnergyRecords records{{left, ActiveRegionRecord{}}, {right, ActiveRegionRecord{}}};
            records.at(left).state = records.at(right).state = RegionState::Reduced;
            records.at(left).cadence_shift = 1;
            records.at(left).cadence_phase = 1;
            records.at(right).cadence_shift = 2;
            records.at(right).cadence_phase = right_phase;
            const EnergyAges ages{{left, 3}, {right, 5}};
            for (std::uint64_t tick = 0; tick < 16; ++tick) {
                SCOPED_TRACE(::testing::Message()
                             << separate_pages << ':' << right_phase << ':' << tick);
                EnergyFieldState field;
                ASSERT_TRUE(field.DeserializeRegionalRecord(initial));
                const auto result = RegionalStep(field, tick, records, {}, ages);
                const bool common = tick % 2 == 1 && tick % 4 == right_phase;
                EXPECT_EQ(field.at_cell(border_cell, 0), common ? 3310u : 0u);
                EXPECT_EQ(field.total_raw(), separate_pages ? 16001u : 16000u);
                EXPECT_EQ(result.ages, ages);
                EXPECT_EQ(result.work.size(), common ? 2u : 0u);
                EXPECT_EQ(result.diffusion_pages, common ? (separate_pages ? 2u : 1u) : 0u);
                if (!common) {
                    EXPECT_EQ(field.SerializeRegionalRecord(), initial);
                }
            }
        }
    }
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
    // Floor(v * 1016 / 1024): 1 -> 0, 255 -> 253, 65535 -> 65023.
    // The four sealed sweeps conserve 0 + 253 + 65023 exactly.
    EXPECT_EQ(field.total_raw(), 65276u);
    EXPECT_EQ(field.fires_completed(), 0u);
}

TEST(RegionalEnergy, EmptyEnergyAgeContinuesAcrossSaveAndReopenBeforeLaterDeposit) {
    ActiveRegionLedger ledger;
    ledger.mark_edited({0, 0}, WorldClock{});
    EnergyFieldState field(2);
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
    EnergyFieldState reopened(2); // indexed absent EFS; do not reset the ARL age
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
                                           "EFS1 2 0\n",
                                           "EFS1 5 0\n",
                                           "EFS2 0 0\n",
                                           "EFS2 2 0\n",
                                           "EFS2 65 0\n",
                                           "EFS2 -1 0\n",
                                           "EFS2 2147483648 0\n",
                                           "EFS2 5 1\n",
                                           "EFS2 05 0\n",
                                           "EFS2 +5 0\n",
                                           "EFS2 5 -0\n",
                                           "EFS2 5 0",
                                           "EFS2 5 0 \n",
                                           "EFS2  5 0\n",
                                           "EFS2 5 0\n\n",
                                           "EFS2 5 0 extra\n",
                                           "EFS2 5 0\nP 0 0 0:1\n",
                                           "EFS3 5 0\n",
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

TEST(RegionalEnergy, EmptyChannelRecordsRestoreOnlyCanonicalNondefaultCounts) {
    for (const int channels : {1, 5, 16, 64}) {
        SCOPED_TRACE(channels);
        EnergyFieldState field(2);
        ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 2 0\nP 0 0 0:100\n"));
        ASSERT_TRUE(field.QueueRegionalDeposit(1, 0, 0, 0, 10));
        ASSERT_TRUE(field.DeserializeRegionalRecord("EFS2 " + std::to_string(channels) + " 0\n"));
        EXPECT_EQ(field.channels(), channels);
        EXPECT_EQ(field.page_count(), 0u);
        EXPECT_FALSE(field.CanonicalRegionalBytes().empty());
        EXPECT_TRUE(EnergyFieldState(2).CanonicalRegionalBytes().empty());
        EXPECT_NE(field.CanonicalRegionalBytes(),
                  EnergyFieldState(channels + 1).CanonicalRegionalBytes());
        EXPECT_TRUE(field.SerializeRegionalRecord().empty());
        EXPECT_FALSE(field.OwnsRegion({0, 0}));
        EXPECT_FALSE(field.ValidateRegionalOwners({}, {{{0, 0}, 5}}));
        const auto records = TwoEnergyOwners();
        const EnergyAges ages{{{0, 0}, 5}, {{1, 0}, 7}};
        ASSERT_TRUE(field.ValidateRegionalOwners(records, ages));
        ASSERT_TRUE(field.QueueRegionalDeposit(2, 0, 0, channels - 1, 777));
        const std::array due{RegionKey{0, 0}};
        const auto result = RegionalStep(field, 1, records, due, ages);
        EXPECT_EQ(result.ages.at({0, 0}), 6u);
        EXPECT_EQ(field.at_cell(0, 0, channels - 1), 777u);
    }
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
    const auto max_pages = EnergyFieldState::kMaxRegionalValueBytes /
                           (std::size_t{kEnergyPageCells} * kEnergyPageCells *
                            static_cast<std::size_t>(channels) * sizeof(std::uint16_t));
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
    EXPECT_FALSE(field.QueueRegionalDeposit(1, (std::numeric_limits<int>::max)(), 0, 0, 1));
    EXPECT_FALSE(field.QueueRegionalDeposit(1, 0, (std::numeric_limits<int>::min)(), 0, 1));
    EXPECT_FALSE(field.OwnsRegion({0, 0}));
    EXPECT_TRUE(field.QueueRegionalDeposit(1, 0, 0, 0, 0));
    EXPECT_FALSE(field.OwnsRegion({0, 0}));
    for (std::size_t i = 0; i < EnergyFieldState::kMaxRegionalDeposits; ++i)
        ASSERT_TRUE(field.QueueRegionalDeposit(i, 0, 0, 0, 1));
    EXPECT_FALSE(field.QueueRegionalDeposit(0, 21, 0, 0, 1));
    EXPECT_TRUE(field.OwnsRegion({0, 0}));
    EXPECT_FALSE(field.OwnsRegion({1, 0}));
    EXPECT_TRUE(field.SerializeRegionalRecord().empty());
    // The shared legacy gatherer remains compatible, but TickRegional still
    // refuses an oversized buffer before consuming any deposits.
    field.QueueDeposit(0, 21, 0, 0, 1);
    EnergyResult output;
    output.clipped_raw = 91;
    EXPECT_FALSE(field.TickRegional(1, TwoEnergyOwners(), {}, {}, output));
    EXPECT_EQ(output.clipped_raw, 91u);
    EXPECT_TRUE(field.OwnsRegion({1, 0}));
}

using RegionalDeposit = EnergyFieldState::RegionalDeposit;

void ExpectBatchContinuation(EnergyFieldState& actual, const EnergyFieldState& prior) {
    auto expected = prior;
    const auto records = TwoEnergyOwners();
    const std::array due{RegionKey{0, 0}, RegionKey{1, 0}};
    const auto result = RegionalStep(actual, 1, records, due, {});
    const auto reference = RegionalStep(expected, 1, records, due, {});
    EXPECT_EQ(actual.SerializeRegionalRecord(), expected.SerializeRegionalRecord());
    EXPECT_EQ(actual.CanonicalRegionalBytes(), expected.CanonicalRegionalBytes());
    EXPECT_EQ(result.clipped_raw, reference.clipped_raw);
    EXPECT_EQ(result.ages, reference.ages);
}

void CheckBatchRefusal(const RegionalDeposit& invalid, const EnergyRecords& records) {
    EnergyFieldState field(2);
    ASSERT_TRUE(field.QueueRegionalDeposit(91, 0, 0, 0, 123));
    const auto prior = field;
    const auto bytes = field.SerializeRegionalRecord();
    // A valid prefix must not leak through a late invalid cell.
    const std::array batch{RegionalDeposit{1, 0, 0, 1, 777}, invalid};
    EXPECT_FALSE(field.QueueRegionalDeposits(batch, records));
    EXPECT_EQ(field.SerializeRegionalRecord(), bytes);
    EXPECT_FALSE(field.OwnsRegion({1, 0}));
    ExpectBatchContinuation(field, prior);
}

TEST(RegionalEnergy, BatchRefusesInvalidFootprintsWithoutConsumingPriorInput) {
    const auto records = TwoEnergyOwners();
    for (const auto& invalid :
         {RegionalDeposit{2, (std::numeric_limits<int>::max)(), 0, 0, 1},
          RegionalDeposit{2, 0, (std::numeric_limits<int>::min)(), 0, 1},
          RegionalDeposit{2, 0, 0, -1, 1},
          RegionalDeposit{2, 0, 0, 2, 1},
          RegionalDeposit{2, 0, 0, 0, std::uint64_t{1} << 32},
          RegionalDeposit{2, 0, 0, 0, (std::numeric_limits<std::uint64_t>::max)()},
          RegionalDeposit{2, 64, 0, 0, 1},
          RegionalDeposit{2, -1, 0, 0, 0}}) {
        ASSERT_NO_FATAL_FAILURE(CheckBatchRefusal(invalid, records));
    }
    for (const auto state : {RegionState::Frozen, static_cast<RegionState>(255)}) {
        auto unavailable = records;
        unavailable.at({1, 0}).state = state;
        ASSERT_NO_FATAL_FAILURE(CheckBatchRefusal({2, 21, 0, 0, 1}, unavailable));
    }
    auto invalid_cadence = records;
    invalid_cadence.at({1, 0}).cadence_shift = 17;
    ASSERT_NO_FATAL_FAILURE(CheckBatchRefusal({2, 21, 0, 0, 1}, invalid_cadence));
    invalid_cadence.at({1, 0}).cadence_shift = 0;
    invalid_cadence.at({1, 0}).cadence_phase = 1;
    ASSERT_NO_FATAL_FAILURE(CheckBatchRefusal({2, 21, 0, 0, 1}, invalid_cadence));
}

void FillPendingBatchQueue(EnergyFieldState& field, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        ASSERT_TRUE(field.QueueRegionalDeposit(index, 0, 0, 0, 1));
    }
}

using Luminumbra::Components::FieldEmitterComponent;
using Luminumbra::Components::TransformComponent;
using Luminumbra::Systems::GatherRegionalFieldEmitterDeposits;

void AddBatchEmitter(entt::registry& registry,
                     const Luminumbra::Vec3& position,
                     const FieldEmitterComponent& emitter) {
    const auto entity = registry.create();
    registry.emplace<TransformComponent>(entity).position = position;
    registry.emplace<FieldEmitterComponent>(entity, emitter);
}

void CheckBatchCapacityRefusal() {
    EnergyFieldState field(2);
    ASSERT_NO_FATAL_FAILURE(
        FillPendingBatchQueue(field, EnergyFieldState::kMaxRegionalDeposits - 1));
    const auto prior = field;
    const auto records = TwoEnergyOwners();
    const std::array batch{RegionalDeposit{1, 21, 0, 0, 777}, RegionalDeposit{2, 22, 0, 1, 888}};
    EXPECT_FALSE(field.QueueRegionalDeposits(batch, records));
    EXPECT_FALSE(field.OwnsRegion({1, 0}));
    EXPECT_TRUE(field.SerializeRegionalRecord().empty());
    entt::registry registry;
    AddBatchEmitter(registry, {480, 0, 24}, {0, 1000, 1});
    EnergyFieldState staged(2);
    EXPECT_EQ(field.regional_deposit_capacity(), 1u);
    const auto stats = GatherRegionalFieldEmitterDeposits(
        registry, staged, records, false, field.regional_deposit_capacity());
    EXPECT_FALSE(stats.accepted);
    EXPECT_FALSE(staged.OwnsRegion({0, 0}));
    ExpectBatchContinuation(field, prior);
    // Refusal did not poison later admission or stepping.
    EXPECT_TRUE(field.QueueRegionalDeposits(batch, records));
}

TEST(RegionalEnergy, BatchCapacityRefusalPreservesEarlierDepositsByteExactly) {
    CheckBatchCapacityRefusal();
}

void CheckBatchBoundAndZeroAmounts() {
    EnergyFieldState field(2);
    const auto records = TwoEnergyOwners();
    std::vector<RegionalDeposit> batch(EnergyFieldState::kMaxRegionalBatchDeposits + 1,
                                       RegionalDeposit{1, 21, 0, 1, 1});
    EXPECT_FALSE(field.QueueRegionalDeposits(batch, records));
    EXPECT_FALSE(field.OwnsRegion({1, 0}));
    batch.pop_back();
    ASSERT_TRUE(field.QueueRegionalDeposits(batch, records));
    const auto result = RegionalStep(field, 1, records, std::array{RegionKey{1, 0}}, {});
    EnergyFieldState singles(2);
    for (const auto& deposit : batch) {
        ASSERT_TRUE(singles.QueueRegionalDeposit(deposit.emitter_id,
                                                 deposit.cx,
                                                 deposit.cz,
                                                 deposit.channel,
                                                 static_cast<std::uint32_t>(deposit.amount)));
    }
    const auto single_result = RegionalStep(singles, 1, records, std::array{RegionKey{1, 0}}, {});
    EXPECT_EQ(field.at_cell(21, 0, 1), singles.at_cell(21, 0, 1));
    EXPECT_EQ(field.total_raw(), singles.total_raw());
    EXPECT_EQ(result.clipped_raw, single_result.clipped_raw);

    field.ResetEmptyAtTick(0);
    ASSERT_NO_FATAL_FAILURE(
        FillPendingBatchQueue(field, EnergyFieldState::kMaxRegionalDeposits - 1));
    const std::array sparse{RegionalDeposit{1, 21, 0, 1, 0}, RegionalDeposit{2, 21, 0, 1, 1}};
    ASSERT_TRUE(field.QueueRegionalDeposits(sparse, records));
    const std::array zero{RegionalDeposit{1, 0, 0, 0, 0}};
    EXPECT_TRUE(field.QueueRegionalDeposits(zero, records));
    EXPECT_TRUE(field.QueueRegionalDeposits({}, {}));
    EXPECT_FALSE(field.QueueRegionalDeposits(sparse, records));
    const auto prior = field;
    ExpectBatchContinuation(field, prior);
}

TEST(RegionalEnergy, BatchBoundAndZeroAmountsUseOnlyRequiredPendingCapacity) {
    CheckBatchBoundAndZeroAmounts();
}

void CheckBatchMatchesSingles() {
    EnergyFieldState field(2), singles(2);
    auto records = TwoEnergyOwners();
    records.at({1, 0}).state = RegionState::Reduced;
    const std::array batch{
        RegionalDeposit{9, 22, 0, 1, 80},
        RegionalDeposit{8, 0, 0, 0, 100},
        RegionalDeposit{2, 21, 0, 1, 60000},
        RegionalDeposit{2, 21, 0, 1, 70000},
        RegionalDeposit{7, 0, 0, 0, (std::numeric_limits<std::uint32_t>::max)()}};
    ASSERT_TRUE(field.QueueRegionalDeposit(1, 0, 0, 1, 123));
    ASSERT_TRUE(singles.QueueRegionalDeposit(1, 0, 0, 1, 123));
    ASSERT_TRUE(field.QueueRegionalDeposits(batch, records));
    for (auto deposit = batch.rbegin(); deposit != batch.rend(); ++deposit) {
        ASSERT_TRUE(singles.QueueRegionalDeposit(deposit->emitter_id,
                                                 deposit->cx,
                                                 deposit->cz,
                                                 deposit->channel,
                                                 static_cast<std::uint32_t>(deposit->amount)));
    }
    ExpectBatchContinuation(field, singles);
    EXPECT_FALSE(field.RegionBytes({0, 0}).empty());
    EXPECT_FALSE(field.RegionBytes({1, 0}).empty());
}

TEST(RegionalEnergy, BatchMatchesSinglesWithDeterministicOwnerAndEmitterOrdering) {
    CheckBatchMatchesSingles();
}

void CheckEmitterRefusal(const Luminumbra::Vec3& position, const FieldEmitterComponent& emitter) {
    entt::registry registry;
    AddBatchEmitter(registry, {480, 0, 24}, {0, 100, 1});
    AddBatchEmitter(registry, position, emitter);
    EnergyFieldState field(2);
    ASSERT_TRUE(field.QueueRegionalDeposit(99, 0, 0, 0, 123));
    const auto prior = field;
    const auto stats = GatherRegionalFieldEmitterDeposits(registry, field, TwoEnergyOwners());
    EXPECT_FALSE(stats.accepted);
    EXPECT_EQ(stats.queued.emitters, 0u);
    EXPECT_EQ(stats.queued.deposits, 0u);
    ExpectBatchContinuation(field, prior);
}

TEST(RegionalEnergy, EmitterBatchValidatesTransformsRadiiChannelsAndCompleteFootprints) {
    const auto infinity = (std::numeric_limits<float>::infinity)();
    const auto nan = (std::numeric_limits<float>::quiet_NaN)();
    for (const auto& position : {Luminumbra::Vec3{infinity, 0, 0},
                                 Luminumbra::Vec3{0, nan, 0},
                                 Luminumbra::Vec3{0, 0, -infinity},
                                 Luminumbra::Vec3{16777216, 0, 0},
                                 Luminumbra::Vec3{-16777216, 0, 0},
                                 Luminumbra::Vec3{0, 0, 0}}) {
        ASSERT_NO_FATAL_FAILURE(CheckEmitterRefusal(position, {0, 100, 1}));
    }
    for (const int radius : {-1, 32, (std::numeric_limits<int>::max)()}) {
        ASSERT_NO_FATAL_FAILURE(CheckEmitterRefusal({0, 0, 0}, {0, 100, radius}));
    }
    ASSERT_NO_FATAL_FAILURE(CheckEmitterRefusal({0, 0, 0}, {-1, 100, 0}));
    ASSERT_NO_FATAL_FAILURE(CheckEmitterRefusal({0, 0, 0}, {2, 100, 0}));
}

void CheckEmitterBatchNeutralityAndClipping() {
    entt::registry registry;
    EnergyFieldState field(3);
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS2 3 0\n"));
    const auto bytes = field.SerializeRegionalRecord();
    const auto empty = GatherRegionalFieldEmitterDeposits(registry, field, {});
    EXPECT_TRUE(empty.accepted);
    EXPECT_EQ(empty.visited_emitters, 0u);
    EXPECT_EQ(field.SerializeRegionalRecord(), bytes);
    AddBatchEmitter(registry, {(std::numeric_limits<float>::infinity)(), 0, 0}, {-1, 0, -1});
    const auto inert = GatherRegionalFieldEmitterDeposits(registry, field, {});
    EXPECT_TRUE(inert.accepted);
    EXPECT_EQ(inert.queued.emitters, 0u);
    EXPECT_EQ(inert.queued.deposits, 0u);
    EXPECT_EQ(field.SerializeRegionalRecord(), bytes);

    registry.clear();
    const Luminumbra::Vec3 position{16777200, 0, 16777200};
    AddBatchEmitter(registry, position, {0, 1000, 1});
    const auto cell =
        static_cast<int>(std::floor(position.x / Luminumbra::Systems::kAetherCellSizeM));
    const auto owner = EnergyFieldState::OwnerOfCell(cell, cell);
    const EnergyRecords records{{owner, ActiveRegionRecord{}}};
    const auto clipped = GatherRegionalFieldEmitterDeposits(registry, field, records, true);
    EXPECT_TRUE(clipped.accepted);
    EXPECT_EQ(clipped.queued.emitters, 1u);
    EXPECT_EQ(clipped.queued.deposits, 4u);
    const auto result = RegionalStep(field, 1, records, std::array{owner}, {});
    EXPECT_EQ(result.clipped_raw, 0u);
    EXPECT_EQ(field.total_raw(), 2500u);
}

TEST(RegionalEnergy, EmitterBatchPreservesInertBytesAndSessionHaloClipping) {
    CheckEmitterBatchNeutralityAndClipping();
}

void CheckEmitterBatchOrdering(bool reverse) {
    entt::registry registry;
    for (int index = 0; index < 64; ++index) {
        registry.emplace<TransformComponent>(registry.create());
    }
    AddBatchEmitter(registry, {480, 0, 24}, {0, 1000, 1});
    AddBatchEmitter(registry, {504, 0, 24}, {1, 100, 0});
    registry.sort<FieldEmitterComponent>([reverse](const auto& a, const auto& b) {
        return reverse ? a.rate_raw_per_tick > b.rate_raw_per_tick
                       : a.rate_raw_per_tick < b.rate_raw_per_tick;
    });
    EnergyFieldState field(2), singles(2);
    const auto stats = GatherRegionalFieldEmitterDeposits(registry, field, TwoEnergyOwners());
    EXPECT_TRUE(stats.accepted);
    EXPECT_EQ(stats.visited_emitters, 2u);
    EXPECT_EQ(stats.queued.emitters, 2u);
    EXPECT_EQ(stats.queued.deposits, 10u);
    Luminumbra::Systems::GatherFieldEmitterDeposits(registry, singles);
    ExpectBatchContinuation(field, singles);
}

TEST(RegionalEnergy, EmitterBatchOrderingIsIndependentOfRegistryStorage) {
    ASSERT_NO_FATAL_FAILURE(CheckEmitterBatchOrdering(false));
    ASSERT_NO_FATAL_FAILURE(CheckEmitterBatchOrdering(true));
}

void CheckEmitterRosterBound(bool use_footprints) {
    entt::registry registry;
    // 134 full 63x63 footprints exceed the batch budget; the inert-looking roster alone does too.
    const auto count = use_footprints ? 134u : EnergyFieldState::kMaxRegionalBatchDeposits + 16;
    for (std::size_t index = 0; index < count; ++index) {
        AddBatchEmitter(registry, {0, 0, 0}, {0, 0xFFFFFFFFu, use_footprints ? 31 : 0});
    }
    EnergyFieldState field(2);
    ASSERT_TRUE(field.QueueRegionalDeposit(99, 0, 0, 0, 123));
    const auto prior = field;
    auto records = TwoEnergyOwners();
    for (int z = -2; z <= 1; ++z) {
        for (int x = -2; x <= 1; ++x) {
            records.try_emplace(RegionKey{x, z});
        }
    }
    const auto stats = GatherRegionalFieldEmitterDeposits(registry, field, records);
    EXPECT_FALSE(stats.accepted);
    EXPECT_EQ(stats.queued.deposits, 0u);
    EXPECT_LE(stats.visited_emitters, EnergyFieldState::kMaxRegionalBatchDeposits + 1);
    ExpectBatchContinuation(field, prior);
}

TEST(RegionalEnergy, EmitterRosterAndCombinedFootprintsAreBoundedBeforeAdmission) {
    ASSERT_NO_FATAL_FAILURE(CheckEmitterRosterBound(false));
    ASSERT_NO_FATAL_FAILURE(CheckEmitterRosterBound(true));
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

TEST(RegionalEnergy, IdleAndBorderTicksExcludeDormantPagesFromBuffers) {
    EnergyFieldState field;
    auto records = TwoEnergyOwners();
    for (auto& [key, record] : records) {
        (void)key;
        record.state = RegionState::Reduced;
        record.cadence_shift = 2;
        record.cadence_phase = 1;
    }
    EnergyAges ages{{{0, 0}, 3}, {{1, 0}, 5}};
    std::string initial = "EFS1 1 0\nP 1 0 4:16000\n";
    // Hundreds of distant frozen pages must never join either working set.
    for (int page = 100; page < 356; ++page) {
        initial += "P " + std::to_string(page) + " 100 0:777\n";
        const auto owner = EnergyFieldState::OwnerOfCell(page * kEnergyPageCells, 1600);
        records[owner].state = RegionState::Frozen;
        ages[owner] = 19;
    }
    ASSERT_TRUE(field.DeserializeRegionalRecord(initial));
    auto result = RegionalStep(field, 3, records, {}, ages);
    EXPECT_EQ(result.staged_pages, 0u);
    EXPECT_EQ(result.diffusion_pages, 0u);
    EXPECT_EQ(result.ages, ages);
    EXPECT_TRUE(result.work.empty());
    EXPECT_EQ(field.SerializeRegionalRecord(), initial);

    // A common border fires even with an empty due set. Its four exact sweeps are
    // 16000/0 -> 15000/1000 -> 14125/1875 -> 13360/2640 -> 12690/3310.
    result = RegionalStep(field, 5, records, {}, ages);
    EXPECT_EQ(result.staged_pages, 1u);
    EXPECT_EQ(result.diffusion_pages, 1u);
    EXPECT_EQ(result.ages, ages);
    ASSERT_EQ(result.work.size(), 2u);
    EXPECT_EQ(result.work[0].page_updates, 1u);
    EXPECT_EQ(result.work[1].page_updates, 1u);
    const auto dormant_at = initial.find("P 100 ");
    ASSERT_NE(dormant_at, std::string::npos);
    EXPECT_EQ(field.SerializeRegionalRecord(),
              "EFS1 1 0\nP 1 0 4:12690 5:3310\n" + initial.substr(dormant_at));
    EXPECT_EQ(field.total_raw(), 16000u + 256u * 777u);
    result = RegionalStep(field, 6, records, {}, ages);
    EXPECT_EQ(result.staged_pages, 0u);
    EXPECT_EQ(result.diffusion_pages, 0u);
    const auto right = field.RegionBytes({1, 0});
    ages[{0, 0}] = 7;
    const std::array due{RegionKey{0, 0}};
    result = RegionalStep(field, 10, records, due, ages);
    EXPECT_EQ(result.staged_pages, 1u);
    EXPECT_EQ(result.diffusion_pages, 1u);
    EXPECT_EQ(result.ages.at({0, 0}), 8u);
    EXPECT_EQ(field.RegionBytes({1, 0}), right);
    EXPECT_EQ(field.total_raw(), DecayOnceRef(12690) + 3310u + 256u * 777u);
}

TEST(RegionalEnergy, CommonBorderStagesBothExistingPhysicalPagesAndSkipsAbsentPages) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 3 0 15:16000\nP 4 0 255:1\n"));
    const EnergyRecords records{{{2, 0}, ActiveRegionRecord{}}, {{3, 0}, ActiveRegionRecord{}}};
    const auto result = RegionalStep(field, 1, records, {}, {{{2, 0}, 3}, {{3, 0}, 5}});
    EXPECT_EQ(result.staged_pages, 2u);
    EXPECT_EQ(result.diffusion_pages, 2u);
    EXPECT_EQ(field.SerializeRegionalRecord(), "EFS1 1 0\nP 3 0 15:12690\nP 4 0 0:3310 255:1\n");

    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 3 0 15:16000\n"));
    const auto sealed = RegionalStep(field, 2, records, {}, {{{2, 0}, 3}});
    EXPECT_EQ(sealed.staged_pages, 0u);
    EXPECT_EQ(sealed.diffusion_pages, 0u);
    EXPECT_EQ(field.SerializeRegionalRecord(), "EFS1 1 0\nP 3 0 15:16000\n");
}

namespace {
void ExpectSameRegionalResult(const EnergyResult& actual, const EnergyResult& expected) {
    EXPECT_EQ(actual.ages, expected.ages);
    EXPECT_EQ(actual.staged_pages, expected.staged_pages);
    EXPECT_EQ(actual.diffusion_pages, expected.diffusion_pages);
    ASSERT_EQ(actual.work.size(), expected.work.size());
    for (std::size_t i = 0; i < actual.work.size(); ++i) {
        EXPECT_EQ(actual.work[i].key, expected.work[i].key);
        EXPECT_EQ(actual.work[i].page_updates, expected.work[i].page_updates);
    }
}

void CheckLegacyEmptyNeighbor(bool ledger_path, bool populate) {
    ActiveRegionLedger ledger;
    ledger.pin({2, 0}, true, WorldClock{});
    ledger.pin({3, 0}, true, WorldClock{});
    EnergyFieldState live, reopened;
    ASSERT_TRUE(live.DeserializeRecord("EFS1 1 8\nP 3 0 15:16000\nP 4 0\n", 0));
    const auto saved = live.SerializeRegionalRecord();
    ASSERT_EQ(saved, "EFS1 1 0\nP 3 0 15:16000\n");
    ASSERT_TRUE(reopened.DeserializeRegionalRecord(saved));
    ASSERT_EQ(live.CanonicalRegionalBytes(), reopened.CanonicalRegionalBytes());
    const EnergyAges ages{{{2, 0}, 3}, {{3, 0}, 5}};
    std::vector<RegionKey> due;
    if (populate) {
        ASSERT_TRUE(live.QueueRegionalDeposit(1, 64, 0, 0, 1));
        ASSERT_TRUE(reopened.QueueRegionalDeposit(1, 64, 0, 0, 1));
        due.push_back({3, 0});
    }
    EnergyResult actual, expected;
    if (ledger_path) {
        ASSERT_TRUE(live.TickRegional(1, ledger, due, ages, actual));
        ASSERT_TRUE(reopened.TickRegional(1, ledger, due, ages, expected));
    } else {
        ASSERT_TRUE(live.TickRegional(1, ledger.records(), due, ages, actual));
        ASSERT_TRUE(reopened.TickRegional(1, ledger.records(), due, ages, expected));
    }
    EXPECT_EQ(live.at_cell(64, 0), populate ? 3311u : 0u);
    EXPECT_EQ(live.total_raw(), populate ? 16001u : 16000u);
    EXPECT_EQ(live.page_count(), populate ? 2u : 1u);
    EXPECT_EQ(actual.staged_pages, populate ? 2u : 0u);
    EXPECT_EQ(actual.diffusion_pages, populate ? 2u : 0u);
    ExpectSameRegionalResult(actual, expected);
    EXPECT_EQ(live.SerializeRegionalRecord(), reopened.SerializeRegionalRecord());
    EXPECT_EQ(live.CanonicalRegionalBytes(), reopened.CanonicalRegionalBytes());
    EXPECT_EQ(live.CanonicalBytes(), reopened.CanonicalBytes());
    if (!populate) {
        EXPECT_EQ(live.SerializeRegionalRecord(), saved);
        EXPECT_EQ(actual.ages, ages);
        EXPECT_TRUE(actual.work.empty());
    }
}
} // namespace

TEST(RegionalEnergy, LegacyEmptyNeighborMatchesReopenedStateAndStagedDepositsOpenTheEdge) {
    for (const bool ledger_path : {false, true}) {
        for (const bool populate : {false, true}) {
            SCOPED_TRACE(::testing::Message() << ledger_path << ':' << populate);
            CheckLegacyEmptyNeighbor(ledger_path, populate);
        }
    }
}

TEST(RegionalEnergy, DepositOnlyTickStagesOnlyItsTargetAndRetainsLegacyEpochs) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRecord("EFS1 1 8\nP 100 100 0:777\n", 0));
    field.SetAnchorCell(0, 0);
    field.Tick(16);
    const auto frozen_owner = EnergyFieldState::OwnerOfCell(1600, 1600);
    EnergyRecords records{{{0, 0}, ActiveRegionRecord{}}, {frozen_owner, ActiveRegionRecord{}}};
    records[frozen_owner].state = RegionState::Frozen;
    const EnergyAges ages{{frozen_owner, 19}};
    const std::array due{RegionKey{0, 0}};
    ASSERT_TRUE(field.QueueRegionalDeposit(1, 0, 0, 0, 123));
    const auto result = RegionalStep(field, 17, records, due, ages);
    EXPECT_EQ(result.staged_pages, 1u);
    EXPECT_EQ(result.diffusion_pages, 0u);
    EXPECT_EQ(field.at_cell(0, 0), 123u);
    EXPECT_EQ(field.at_cell(1600, 1600), 777u);
    EXPECT_EQ(field.CanonicalBytes(), "aether_state:v1:1:P0,0,0:0=123;P100,100,2:0=777;");
}

TEST(RegionalEnergy, NegativeCommonBordersStageBothAxesInCanonicalOrder) {
    for (const bool vertical : {false, true}) {
        EnergyFieldState field;
        const auto source = vertical ? "EFS1 1 0\nP 0 0 255:1\nP 0 -1 240:16000\n"
                                     : "EFS1 1 0\nP -1 -1 255:16000\nP 0 -1 255:1\n";
        ASSERT_TRUE(field.DeserializeRegionalRecord(source));
        const RegionKey first = vertical ? RegionKey{0, -1} : RegionKey{-1, -1};
        const RegionKey second = vertical ? RegionKey{0, 0} : RegionKey{0, -1};
        const EnergyRecords records{{first, ActiveRegionRecord{}}, {second, ActiveRegionRecord{}}};
        const auto result = RegionalStep(field, 1, records, {}, {{first, 3}, {second, 5}});
        EXPECT_EQ(result.staged_pages, 2u);
        EXPECT_EQ(result.diffusion_pages, 2u);
        EXPECT_EQ(field.SerializeRegionalRecord(),
                  vertical ? "EFS1 1 0\nP 0 0 0:3310 255:1\nP 0 -1 240:12690\n"
                           : "EFS1 1 0\nP -1 -1 255:12690\nP 0 -1 240:3310 255:1\n");
        ASSERT_EQ(result.work.size(), 2u);
        EXPECT_EQ(result.work[0].key, first);
        EXPECT_EQ(result.work[1].key, second);
    }
}

TEST(RegionalEnergy, IdleTickDropsLegacyEmptyPagesAndRejectsInvalidMetadataAtomically) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRecord("EFS1 1 8\nP 0 0\n", 0));
    EXPECT_EQ(field.page_count(), 1u);
    const auto empty = RegionalStep(field, 1, {}, {}, {});
    EXPECT_EQ(empty.staged_pages, 0u);
    EXPECT_EQ(empty.diffusion_pages, 0u);
    EXPECT_EQ(field.page_count(), 0u);
    auto records = TwoEnergyOwners();
    EnergyResult out;
    out.staged_pages = 71;
    out.diffusion_pages = 72;
    for (const auto invalid : {17u, 255u}) {
        records.begin()->second.cadence_shift = static_cast<std::uint8_t>(invalid);
        EXPECT_FALSE(field.TickRegional(1, records, {}, {}, out));
        EXPECT_EQ(out.staged_pages, 71u);
        EXPECT_EQ(out.diffusion_pages, 72u);
        EXPECT_EQ(field.page_count(), 0u);
    }
}

TEST(RegionalEnergy, IntrinsicRoundingRetainsSubOutflowResiduesAfterFourSweeps) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 0 0 136:17\n"));
    const EnergyRecords records{{{0, 0}, ActiveRegionRecord{}}};
    const std::array due{RegionKey{0, 0}};
    const auto result = RegionalStep(field, 900000, records, due, {{{0, 0}, 7}});
    // Decay: floor(17 * 1016 / 1024) = 16. First sweep sends floor(16 / 16)
    // to each of four neighbours: centre 12, neighbours 1. All five values
    // are below 16, so the remaining three sweeps move nothing.
    EXPECT_EQ(result.ages.at({0, 0}), 8u);
    EXPECT_EQ(field.SerializeRegionalRecord(), "EFS1 1 0\nP 0 0 120:1 135:1 136:12 137:1 152:1\n");
    EXPECT_EQ(field.total_raw(), 16u);
}

TEST(RegionalEnergy, ScalarEdgeGateSealsExchangeAndEmptyGateKeepsLegacyResults) {
    using luminumbra::fields::ScalarFieldDiffusion;
    ScalarFieldDiffusion legacy(2, 1), empty_gate(2, 1), sealed(2, 1), open(2, 1);
    for (auto* field : {&legacy, &empty_gate, &sealed, &open}) {
        field->set(0, 0, 16.0);
    }
    const auto legacy_report = legacy.diffuse(4, 0.25);
    const auto empty_report = empty_gate.diffuse(4, 0.25, {});
    const auto sealed_report = sealed.diffuse(
        4, 0.25, [](std::size_t, std::size_t, std::size_t, std::size_t) { return false; });
    const auto open_report = open.diffuse(
        4, 0.25, [](std::size_t, std::size_t, std::size_t, std::size_t) { return true; });
    EXPECT_EQ(empty_gate.values(), legacy.values());
    EXPECT_EQ(open.values(), legacy.values());
    EXPECT_EQ(luminumbra::fields::SerializeScalarDiffusionReportJson(empty_report),
              luminumbra::fields::SerializeScalarDiffusionReportJson(legacy_report));
    EXPECT_EQ(luminumbra::fields::SerializeScalarDiffusionReportJson(open_report),
              luminumbra::fields::SerializeScalarDiffusionReportJson(legacy_report));
    EXPECT_EQ(sealed.at(0, 0), 16.0);
    EXPECT_EQ(sealed.at(1, 0), 0.0);
    EXPECT_EQ(sealed_report.final_energy, 16.0);
    // Pairwise flux is one quarter of the difference: 16/0 -> 12/4 ->
    // 10/6 -> 9/7 -> 8.5/7.5, exactly representable in binary.
    EXPECT_EQ(legacy.at(0, 0), 8.5);
    EXPECT_EQ(legacy.at(1, 0), 7.5);
}

namespace {
std::string
RegionalRecordAtByteLimit(EnergyRecords& records,
                          EnergyAges& ages,
                          std::size_t byte_limit = EnergyFieldState::kMaxRegionalRecordBytes) {
    // 64 channels allow five-digit indices. Five-digit values reach the byte
    // cap in fewer than 2048 pages, within both the page and decoded-value budgets.
    constexpr std::size_t channels = 64;
    constexpr std::size_t values_per_page = 256 * channels;
    std::string record = "EFS1 64 0\n";
    record.reserve(byte_limit);
    bool complete = false;
    std::size_t pages = 0;
    for (int px = 0; !complete; ++px) {
        std::string row = "P " + std::to_string(px) + " 0";
        std::vector<std::size_t> value_offsets;
        for (std::size_t index = 0; index < values_per_page; ++index) {
            const auto prefix = " " + std::to_string(index) + ":";
            value_offsets.push_back(row.size() + prefix.size());
            row += prefix;
            row += px == 0 && index == 0 ? "9" : "65535";
            if (record.size() + row.size() + 1 >= byte_limit) {
                auto excess = record.size() + row.size() + 1 - byte_limit;
                // Shorten up to three final values, keeping canonical nonzero tokens.
                for (auto offset = value_offsets.rbegin();
                     excess != 0 && offset != value_offsets.rend();
                     ++offset) {
                    const auto removed = (std::min)(excess, std::size_t{4});
                    row.erase(*offset, removed);
                    excess -= removed;
                }
                EXPECT_EQ(excess, 0u);
                complete = true;
                break;
            }
        }
        record += row;
        record += '\n';
        ++pages;
        for (int x = 0; x < kEnergyPageCells; ++x) {
            const auto owner = EnergyFieldState::OwnerOfCell(px * kEnergyPageCells + x, 0);
            records[owner].state = RegionState::Frozen;
            ages[owner] = 0;
        }
    }
    EXPECT_LE(pages, luminumbra::fields::kEnergyReadPages);
    EXPECT_LE(pages * values_per_page * sizeof(std::uint16_t),
              EnergyFieldState::kMaxRegionalValueBytes);
    EXPECT_EQ(record.size(), byte_limit);
    return record;
}
} // namespace

TEST(RegionalEnergy, RegionalRecordCapCoversTheLegacyReaderRange) {
    // Every record the legacy reader admits must also be admissible and re-savable through the
    // regional paths, so the regional cap may never fall below the legacy reader cap.
    static_assert(EnergyFieldState::kMaxRegionalRecordBytes >=
                  luminumbra::fields::kEnergyReadRecordBytes);
}

namespace {
// One byte over the limit through the width of a single value: the decoder
// refuses it, and the same pages below the limit decode after one digit is removed.
void RefuseWideRecordAndNarrowIt(EnergyFieldState& field, std::string& record) {
    EXPECT_FALSE(field.DeserializeRegionalRecord(record));
    EXPECT_EQ(field.page_count(), 0u);
    const auto wide_value = record.find(":65535");
    ASSERT_NE(wide_value, std::string::npos);
    record.erase(wide_value + 1, 1);
}

void ExpectOverLimitTickRefusedKeepsCallerResult(EnergyFieldState& field,
                                                 const EnergyRecords& records,
                                                 const EnergyAges& ages,
                                                 std::span<const RegionKey> due,
                                                 const std::string& record) {
    EnergyResult result;
    result.ages = {{{9, 9}, 42}};
    result.work = {{{9, 9}, 1, 2, 3, 4}};
    result.clipped_raw = 99;
    result.staged_pages = 123;
    result.diffusion_pages = 456;
    // The decoded budget is unchanged and one deposit is well below its cap.
    EXPECT_FALSE(field.TickRegional(1, records, due, ages, result));
    EXPECT_EQ(result.ages, (EnergyAges{{{9, 9}, 42}}));
    ASSERT_EQ(result.work.size(), 1u);
    EXPECT_EQ(result.work[0].key, (RegionKey{9, 9}));
    EXPECT_EQ(result.work[0].region_ticks, 1u);
    EXPECT_EQ(result.work[0].entity_updates, 2u);
    EXPECT_EQ(result.work[0].water_cells, 3u);
    EXPECT_EQ(result.work[0].page_updates, 4u);
    EXPECT_EQ(result.clipped_raw, 99u);
    EXPECT_EQ(result.staged_pages, 123u);
    EXPECT_EQ(result.diffusion_pages, 456u);
    EXPECT_EQ(field.at_cell(0, 0, 1), 5535u);
    EXPECT_TRUE(field.SerializeRegionalRecord() == record);
}

void ExpectWriterRefusesThenTickFreesBytes(EnergyFieldState& field,
                                           const EnergyRecords& records,
                                           const EnergyAges& ages,
                                           std::span<const RegionKey> due) {
    // The writer independently refuses the identical over-limit state.
    EnergyFieldStateTestAccess::SetOriginChannel(field, 1, 65535);
    EXPECT_THROW(static_cast<void>(field.SerializeRegionalRecord()), std::length_error);
    // Restore channel 1, then free two encoded bytes in channel 2 without
    // copying the fixture. The original queued deposit grows channel 1 by one byte.
    EnergyFieldStateTestAccess::SetOriginChannel(field, 1, 5535);
    EnergyFieldStateTestAccess::SetOriginChannel(field, 2, 535);
    EnergyResult result;
    ASSERT_TRUE(field.TickRegional(1, records, due, ages, result));
    EXPECT_EQ(field.at_cell(0, 0, 1), 65535u);
    EXPECT_EQ(field.at_cell(0, 0, 2), 535u);
    EXPECT_EQ(result.ages.at({0, 0}), 1u);
    EXPECT_EQ(result.clipped_raw, 0u);
    EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
}
} // namespace

TEST(RegionalEnergy, RecordByteBoundariesCoverWriterDecoderAndCachedTick) {
    // Construct and release one large fixture at a time. At each boundary the
    // same decoded pages back the writer, decoder and cached tick assertions.
    for (const int delta : {-1, 0, 1}) {
        SCOPED_TRACE(delta);
        EnergyRecords records;
        EnergyAges ages;
        const auto bytes =
            EnergyFieldState::kMaxRegionalRecordBytes - 1 + static_cast<std::size_t>(delta + 1);
        auto record = RegionalRecordAtByteLimit(records, ages, bytes);
        EnergyFieldState field;
        if (delta == 1) {
            // Refuse a canonical record with exactly the same pages and tokens
            // as the accepted one-byte-shorter record below. Only value width differs.
            ASSERT_NO_FATAL_FAILURE(RefuseWideRecordAndNarrowIt(field, record));
        }
        ASSERT_TRUE(field.DeserializeRegionalRecord(record));
        ASSERT_TRUE(field.SerializeRegionalRecord() == record);
        EXPECT_LE(field.page_count(), luminumbra::fields::kEnergyReadPages);
        EXPECT_LE(field.page_count() * 256u * 64u * sizeof(std::uint16_t),
                  EnergyFieldState::kMaxRegionalValueBytes);
        if (delta == 1) {
            // Restore the removed digit through the deposit path: 5535 -> 65535.
            // The first wide value is channel 1 of cell (0,0), on an existing page.
            ASSERT_TRUE(field.QueueRegionalDeposit(1, 0, 0, 1, 60000));
        }
        records.at({0, 0}).state = RegionState::Active;
        ASSERT_TRUE(field.ValidateRegionalOwners(records, ages));
        ASSERT_TRUE(EnergyFieldStateTestAccess::Validated(field));
        const auto page_count = field.page_count();
        const std::array due{RegionKey{0, 0}};
        if (delta == 1) {
            ASSERT_NO_FATAL_FAILURE(
                ExpectOverLimitTickRefusedKeepsCallerResult(field, records, ages, due, record));
            ASSERT_NO_FATAL_FAILURE(
                ExpectWriterRefusesThenTickFreesBytes(field, records, ages, due));
        } else {
            EnergyResult result;
            ASSERT_TRUE(field.TickRegional(1, records, due, ages, result));
            EXPECT_TRUE(field.SerializeRegionalRecord() == record);
            EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
        }
        EXPECT_EQ(field.page_count(), page_count);
    }
}

TEST(RegionalEnergy, MissingFinalNewlineAtByteLimitRefusesWithoutThrowingOrChangingState) {
    EnergyRecords records;
    EnergyAges ages;
    auto record = RegionalRecordAtByteLimit(records, ages);
    const auto value = record.find(":9");
    ASSERT_NE(value, std::string::npos);
    // The canonical encoding needs one additional byte; the unterminated final
    // line still parses successfully and keeps the input exactly at the limit.
    record.insert(value + 2, 1, '1');
    record.pop_back();
    ASSERT_EQ(record.size(), EnergyFieldState::kMaxRegionalRecordBytes);
    ASSERT_NE(record.back(), '\n');

    EnergyFieldState field(2);
    const std::string saved = "EFS1 2 0\nP 0 0 0:100\n";
    ASSERT_TRUE(field.DeserializeRegionalRecord(saved));
    ASSERT_TRUE(field.QueueRegionalDeposit(1, 0, 0, 0, 1));
    const EnergyRecords owners{{{0, 0}, ActiveRegionRecord{}}};
    ASSERT_TRUE(field.ValidateRegionalOwners(owners, {{{0, 0}, 0}}));
    bool accepted = true;
    EXPECT_NO_THROW(accepted = field.DeserializeRegionalRecord(record));
    EXPECT_FALSE(accepted);
    EXPECT_EQ(field.channels(), 2);
    EXPECT_EQ(field.SerializeRegionalRecord(), saved);
    EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
    const std::array due{RegionKey{0, 0}};
    const auto result = RegionalStep(field, 1, owners, due, {});
    EXPECT_EQ(field.at_cell(0, 0), 101u);
    EXPECT_EQ(result.clipped_raw, 0u);
}

TEST(RegionalEnergy, LegacyMigrationKeepsLegalBoundaryOwnersAndDropsOnlyOutsideCells) {
    struct BoundaryCase {
        const char* legacy_pages;
        const char* kept_pages;
        int cx, cz;
        RegionKey kept_owner, dropped_owner;
    };
    // Cell centres use floor((24*c + 12)/512). The two cells in each
    // boundary page belong to adjacent owners, on opposite sides of the world edge.
    const BoundaryCase cases[]{
        {"P -43691 0 8:111 9:222 10:12345 11:65535\nP 0 0 0:777\n",
         "P -43691 0 10:12345 11:65535\nP 0 0 0:777\n",
         -699051,
         0,
         {-32768, 0},
         {-32769, 0}},
        {"P 0 0 0:777\nP 43690 0 20:12345 21:65535 22:111 23:222\n",
         "P 0 0 0:777\nP 43690 0 20:12345 21:65535\n",
         699050,
         0,
         {32767, 0},
         {32768, 0}},
        {"P 0 0 0:777\nP 0 -43691 128:111 129:222 160:12345 161:65535\n",
         "P 0 0 0:777\nP 0 -43691 160:12345 161:65535\n",
         0,
         -699051,
         {0, -32768},
         {0, -32769}},
        {"P 0 0 0:777\nP 0 43690 320:12345 321:65535 352:111 353:222\n",
         "P 0 0 0:777\nP 0 43690 320:12345 321:65535\n",
         0,
         699050,
         {0, 32767},
         {0, 32768}},
        {"P -43691 -43691 136:111 138:222 168:333 170:12345 171:65535\nP 0 0 0:777\n",
         "P -43691 -43691 170:12345 171:65535\nP 0 0 0:777\n",
         -699051,
         -699051,
         {-32768, -32768},
         {-32769, -32769}},
        {"P 0 0 0:777\nP 43690 43690 340:12345 341:65535 342:111 372:222 374:333\n",
         "P 0 0 0:777\nP 43690 43690 340:12345 341:65535\n",
         699050,
         699050,
         {32767, 32767},
         {32768, 32768}},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.legacy_pages);
        EnergyFieldState field(2);
        const auto legacy = std::string("EFS1 2 3\n") + item.legacy_pages;
        ASSERT_TRUE(EnergyFieldState::AdmitRecord(legacy, true));
        ASSERT_TRUE(field.DeserializeRecord(legacy, 31));
        ActiveRegionLedger ledger({}, WorldClock(31));
        ASSERT_TRUE(field.MigrateLegacyCadence(ledger, WorldClock(31), {}));
        EXPECT_EQ(field.SerializeRegionalRecord(), std::string("EFS1 2 0\n") + item.kept_pages);
        EXPECT_EQ(field.at_cell(item.cx, item.cz, 0), 12345u);
        EXPECT_EQ(field.at_cell(item.cx, item.cz, 1), 65535u);
        EXPECT_EQ(field.total_raw(), 12345u + 65535u + 777u);
        EXPECT_EQ(ledger.energy_ages().at(item.kept_owner), 5u);
        EXPECT_FALSE(ledger.records().contains(item.dropped_owner));
        EnergyFieldState reopened;
        ASSERT_TRUE(reopened.DeserializeRegionalRecord(field.SerializeRegionalRecord()));
        EXPECT_EQ(reopened.CanonicalRegionalBytes(), field.CanonicalRegionalBytes());
    }
}

TEST(RegionalEnergy, LegacyMigrationDropsWhollyOutsidePagesWithoutCoordinateOverflow) {
    for (const bool in_world : {false, true}) {
        const auto legacy = std::string("EFS1 2 8\nP -2147483648 -2147483648 0:111\n") +
                            "P -43692 0 0:222\n" + (in_world ? "P 0 0 0:777\n" : "") +
                            "P 43691 0 0:333\nP 2147483647 2147483647 0:444\n";
        ASSERT_TRUE(EnergyFieldState::AdmitRecord(legacy, true));
        EnergyFieldState field;
        ASSERT_TRUE(field.DeserializeRecord(legacy, 31));
        ActiveRegionLedger ledger({}, WorldClock(31));
        ASSERT_TRUE(field.MigrateLegacyCadence(ledger, WorldClock(31), {}));
        EXPECT_EQ(field.SerializeRegionalRecord(), in_world ? "EFS1 2 0\nP 0 0 0:777\n" : "");
        EXPECT_EQ(field.page_count(), in_world ? 1u : 0u);
        EXPECT_TRUE(field.ValidateRegionalOwners(ledger.records(), ledger.energy_ages()));
    }
}

TEST(RegionalEnergy, LegacyMigrationRefusesExistingAgesAndInvalidCadenceBeforeClipping) {
    const std::string legacy = "EFS1 2 3\nP -43691 0 8:111\nP 0 0 0:777\n";
    for (const std::uint64_t tick : {25u, 31u, 34u}) {
        EnergyFieldState field;
        ASSERT_TRUE(field.DeserializeRecord(legacy, 31));
        ActiveRegionLedger ledger({}, WorldClock(tick));
        if (tick == 31) {
            ledger.mark_edited({0, 0}, WorldClock(tick));
            ASSERT_TRUE(ledger.set_energy_age({0, 0}, 1));
        }
        const auto before = field.CanonicalBytes();
        const auto ledger_before = ledger.encode();
        EXPECT_FALSE(field.MigrateLegacyCadence(ledger, WorldClock(tick), {}));
        EXPECT_EQ(field.CanonicalBytes(), before);
        EXPECT_EQ(ledger.encode(), ledger_before);
    }
    EnergyFieldState invalid_channels(65);
    ActiveRegionLedger ledger;
    EXPECT_FALSE(invalid_channels.MigrateLegacyCadence(ledger, WorldClock(0), {}));
}

TEST(RegionalEnergy, RegionalWriterRefusesIncompatibleLegacyPagesWithoutChangingBytes) {
    for (const auto* record : {"EFS1 1 8\nP 2147483647 0 0:1\n", "EFS1 1 8\nP -43691 0 0:1\n"}) {
        EnergyFieldState field;
        ASSERT_TRUE(field.DeserializeRecord(record, 0));
        const auto before = field.CanonicalBytes();
        EXPECT_THROW(static_cast<void>(field.SerializeRegionalRecord()), std::length_error);
        EXPECT_EQ(field.CanonicalBytes(), before);
        EXPECT_EQ(field.SerializeRecord(0), record);
    }
    const EnergyFieldState excess_channels(65);
    EXPECT_THROW(static_cast<void>(excess_channels.SerializeRegionalRecord()), std::length_error);
}

TEST(RegionalEnergy, AffectedValidationMatchesFullWalkWithThousandsOfDormantPages) {
    EnergyFieldState field;
    auto records = TwoEnergyOwners();
    records.at({1, 0}).state = RegionState::Frozen;
    EnergyAges ages{{{0, 0}, 7}, {{1, 0}, 5}};
    std::string initial = "EFS1 1 8\nP -5 -5\nP 0 0 136:17\nP 1 0 4:16000 5:500\nP 8 8\n";
    for (int page = 100; page < 2148; ++page) {
        initial += "P " + std::to_string(page) + " 100 0:777\n";
        const auto owner = EnergyFieldState::OwnerOfCell(page * kEnergyPageCells, 1600);
        records[owner].state = RegionState::Frozen;
        ages[owner] = 19;
    }
    ASSERT_TRUE(field.DeserializeRecord(initial, 0));
    ASSERT_TRUE(field.QueueRegionalDeposit(1, 0, 0, 0, 300));
    ASSERT_TRUE(field.ValidateRegionalOwners(records, ages));
    auto reference = field;
    const std::array due{RegionKey{0, 0}};
    for (std::uint64_t tick = 1; tick <= 17; ++tick) {
        SCOPED_TRACE(tick);
        // Unset summaries expose any accidental dormant-page walk, including capacity checks.
        for (int page = 100; page < 2148; ++page)
            EnergyFieldStateTestAccess::ForgetPageFacts(field, page, 100);
        EnergyFieldStateTestAccess::Invalidate(reference);
        EnergyResult actual, expected;
        ASSERT_TRUE(field.TickRegional(tick, records, due, ages, actual));
        ASSERT_TRUE(reference.TickRegional(tick, records, due, ages, expected));
        for (int page = 100; page < 2148; ++page) {
            EXPECT_FALSE(EnergyFieldStateTestAccess::HasPageFacts(field, page, 100));
        }
        EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
        EXPECT_LE(actual.staged_pages, 2u);
        EXPECT_EQ(actual.ages, expected.ages);
        EXPECT_EQ(actual.clipped_raw, expected.clipped_raw);
        EXPECT_EQ(actual.staged_pages, expected.staged_pages);
        EXPECT_EQ(actual.diffusion_pages, expected.diffusion_pages);
        ASSERT_EQ(actual.work.size(), expected.work.size());
        for (std::size_t i = 0; i < actual.work.size(); ++i) {
            EXPECT_EQ(actual.work[i].key, expected.work[i].key);
            EXPECT_EQ(actual.work[i].region_ticks, expected.work[i].region_ticks);
            EXPECT_EQ(actual.work[i].entity_updates, expected.work[i].entity_updates);
            EXPECT_EQ(actual.work[i].water_cells, expected.work[i].water_cells);
            EXPECT_EQ(actual.work[i].page_updates, expected.work[i].page_updates);
        }
        ASSERT_TRUE(reference.ValidateRegionalOwners(records, expected.ages));
        EXPECT_EQ(field.page_count(), reference.page_count());
        EXPECT_EQ(field.page_count(), 2050u);
        EXPECT_EQ(field.SerializeRegionalRecord(), reference.SerializeRegionalRecord());
        EXPECT_EQ(field.CanonicalRegionalBytes(), reference.CanonicalRegionalBytes());
        EXPECT_EQ(field.CanonicalBytes(), reference.CanonicalBytes());
        ages = actual.ages;
    }
}

TEST(RegionalEnergy, LedgerLookupsMatchForcedFullScansWithThousandsOfRecordsAndFewDueOwners) {
    ActiveRegionLedger ledger({0, 1, 2});
    for (int x = -32; x < 32; ++x)
        for (int z = -32; z < 32; ++z)
            ledger.pin({x, z}, true, WorldClock{});
    ASSERT_GE(ledger.records().size(), 4096u);
    EnergyFieldState field;
    // Include both axes, negative coordinates, shared and separate physical pages,
    // legacy empty pages, and distant owners whose metadata the scheduler changes.
    ASSERT_TRUE(field.DeserializeRecord(
        "EFS1 1 8\nP -4 -4\nP -1 -1 255:16000\nP 0 0 0:100 136:17000\n"
        "P 0 -1 240:12000\nP 1 0 4:16000 5:500\nP 3 0 15:9000\nP 4 0 255:1\n"
        "P 7 7\nP 41 41 255:6000\n",
        0));
    EnergyAges ages{{{-1, -1}, 3},
                    {{0, -1}, 5},
                    {{0, 0}, 7},
                    {{1, 0}, 5},
                    {{2, 0}, 3},
                    {{3, 0}, 7},
                    {{31, 31}, 5}};
    ASSERT_TRUE(field.ValidateRegionalOwners(ledger.records(), ages));
    auto reference = field;
    const std::array due{RegionKey{0, 0}, RegionKey{3, 0}};
    const std::array<Luminumbra::world::RegionWork, 1> work{{{{0, 0}, 1, 0, 0, 0}}};
    bool border_without_due = false;
    for (std::uint64_t tick = 1; tick <= 17; ++tick) {
        SCOPED_TRACE(tick);
        // Exercise valid state/cadence mutations between ticks without replacing
        // the energy validation marker. Only two owners enter the interior due set.
        ledger.schedule(WorldClock(tick), {}, work, [](RegionKey) { return 42u; });
        const auto selected =
            tick % 3 == 0 ? std::span<const RegionKey>{} : std::span<const RegionKey>{due};
        if (tick % 4 == 0) {
            ASSERT_TRUE(field.QueueRegionalDeposit(1, 65, 0, 0, 700));
            ASSERT_TRUE(reference.QueueRegionalDeposit(1, 65, 0, 0, 700));
        }
        EnergyFieldStateTestAccess::Invalidate(reference);
        EnergyResult actual, expected;
        ASSERT_TRUE(field.TickRegional(tick, ledger, selected, ages, actual));
        // A raw map has no trusted mutation boundary and forces both record
        // validation and the original full-ledger border discovery.
        ASSERT_TRUE(reference.TickRegional(tick, ledger.records(), selected, ages, expected));
        EXPECT_EQ(actual.ages, expected.ages);
        EXPECT_EQ(actual.clipped_raw, expected.clipped_raw);
        EXPECT_EQ(actual.staged_pages, expected.staged_pages);
        EXPECT_EQ(actual.diffusion_pages, expected.diffusion_pages);
        ASSERT_EQ(actual.work.size(), expected.work.size());
        for (std::size_t i = 0; i < actual.work.size(); ++i) {
            EXPECT_EQ(actual.work[i].key, expected.work[i].key);
            EXPECT_EQ(actual.work[i].region_ticks, expected.work[i].region_ticks);
            EXPECT_EQ(actual.work[i].entity_updates, expected.work[i].entity_updates);
            EXPECT_EQ(actual.work[i].water_cells, expected.work[i].water_cells);
            EXPECT_EQ(actual.work[i].page_updates, expected.work[i].page_updates);
        }
        EXPECT_EQ(field.SerializeRegionalRecord(), reference.SerializeRegionalRecord());
        EXPECT_EQ(field.CanonicalRegionalBytes(), reference.CanonicalRegionalBytes());
        EXPECT_EQ(field.CanonicalBytes(), reference.CanonicalBytes());
        EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
        border_without_due =
            border_without_due || (selected.empty() && actual.diffusion_pages != 0);
        ages = actual.ages;
    }
    EXPECT_TRUE(border_without_due);
}

TEST(RegionalEnergy, PhasedLedgerBorderCandidatesMatchReopenedFullScanReference) {
    ActiveRegionLedger ledger({0, 1, 2});
    ledger.pin({0, 0}, true, WorldClock{});
    ledger.pin({1, 0}, true, WorldClock{});
    const std::array<Luminumbra::world::RegionWork, 1> work{{{{0, 0}, 1, 0, 0, 0}}};
    ledger.schedule(WorldClock(1), {}, work);
    ledger.pin({0, 0}, false, WorldClock(1));
    ledger.schedule(WorldClock(2), {}, work);
    ASSERT_EQ(ledger.records().at({0, 0}).state, RegionState::Reduced);
    ASSERT_EQ(ledger.records().at({0, 0}).cadence_shift, 2);
    ASSERT_EQ(ledger.records().at({0, 0}).cadence_phase, 1u);
    ASSERT_EQ(ledger.records().at({1, 0}).cadence_shift, 0);
    const EnergyAges ages{{{0, 0}, 3}, {{1, 0}, 5}};
    ASSERT_TRUE(ledger.set_energy_ages(ages));
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), restored, error)) << error;
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 1 0 4:16000\n"));
    ASSERT_TRUE(field.ValidateRegionalOwners(ledger.records(), ages));
    for (std::uint64_t tick = 3; tick <= 18; ++tick) {
        SCOPED_TRACE(tick);
        const auto before = field.SerializeRegionalRecord();
        EnergyFieldState reference;
        ASSERT_TRUE(reference.DeserializeRegionalRecord(before));
        ASSERT_TRUE(reference.ValidateRegionalOwners(restored.records(), restored.energy_ages()));
        EnergyFieldStateTestAccess::Invalidate(reference);
        EnergyResult actual, expected;
        ASSERT_TRUE(field.TickRegional(tick, ledger, {}, ages, actual));
        ASSERT_TRUE(
            reference.TickRegional(tick, restored.records(), {}, restored.energy_ages(), expected));
        EXPECT_EQ(actual.ages, expected.ages);
        EXPECT_EQ(actual.clipped_raw, expected.clipped_raw);
        EXPECT_EQ(actual.staged_pages, expected.staged_pages);
        EXPECT_EQ(actual.diffusion_pages, expected.diffusion_pages);
        ASSERT_EQ(actual.work.size(), expected.work.size());
        for (std::size_t i = 0; i < actual.work.size(); ++i) {
            EXPECT_EQ(actual.work[i].key, expected.work[i].key);
            EXPECT_EQ(actual.work[i].region_ticks, expected.work[i].region_ticks);
            EXPECT_EQ(actual.work[i].entity_updates, expected.work[i].entity_updates);
            EXPECT_EQ(actual.work[i].water_cells, expected.work[i].water_cells);
            EXPECT_EQ(actual.work[i].page_updates, expected.work[i].page_updates);
        }
        EXPECT_EQ(actual.diffusion_pages, tick % 4 == 1 ? 1u : 0u);
        EXPECT_EQ(field.SerializeRegionalRecord(), reference.SerializeRegionalRecord());
        EXPECT_EQ(field.CanonicalRegionalBytes(), reference.CanonicalRegionalBytes());
        EXPECT_EQ(field.CanonicalBytes(), reference.CanonicalBytes());
        EXPECT_EQ(field.total_raw(), 16000u);
        if (tick % 4 != 1) {
            EXPECT_EQ(field.SerializeRegionalRecord(), before);
        }
    }
}

TEST(RegionalEnergy, FullValidationRejectsTamperedRecordMetadataDespiteCachedValidation) {
    ActiveRegionLedger ledger;
    ledger.pin({0, 0}, true, WorldClock{});
    const RegionKey dormant{100, 100};
    ledger.pin(dormant, true, WorldClock{});
    EnergyFieldState initial;
    ASSERT_TRUE(initial.DeserializeRegionalRecord("EFS1 1 0\nP 0 0 0:100\n"));
    const EnergyAges ages{{{0, 0}, 0}};
    ASSERT_TRUE(initial.ValidateRegionalOwners(ledger.records(), ages));
    const auto saved = initial.SerializeRegionalRecord();
    for (int mutation = 0; mutation < 3; ++mutation) {
        SCOPED_TRACE(mutation);
        auto field = initial;
        auto tampered = ledger;
        // Deliberately bypass the ledger's const-only record interface and leave
        // the energy marker intact. Full admission must still check every record.
        auto& record = const_cast<EnergyRecords&>(tampered.records()).at(dormant);
        if (mutation == 0) {
            record.state = static_cast<RegionState>(255);
        } else if (mutation == 1) {
            record.cadence_shift = 17;
        } else {
            record.cadence_phase = 1;
        }
        EnergyResult result;
        ASSERT_TRUE(field.TickRegional(1, tampered, {}, ages, result));
        EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
        EXPECT_EQ(field.SerializeRegionalRecord(), saved);
        EXPECT_FALSE(field.ValidateRegionalOwners(tampered.records(), result.ages));
        EXPECT_FALSE(EnergyFieldStateTestAccess::Validated(field));
        EXPECT_FALSE(field.ValidateRegionalOwners(tampered.records(), result.ages));
        result.clipped_raw = 91;
        EXPECT_FALSE(field.TickRegional(2, tampered.records(), {}, ages, result));
        EXPECT_EQ(result.clipped_raw, 91u);
        EXPECT_EQ(field.SerializeRegionalRecord(), saved);
        EXPECT_TRUE(field.ValidateRegionalOwners(ledger.records(), ages));
    }
}

TEST(RegionalEnergy, FullValidationRejectsTamperedDormantPagesDespiteCachedValidation) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 0 0 0:100\nP 100 100 0:777\n"));
    const auto dormant = EnergyFieldState::OwnerOfCell(1600, 1600);
    EnergyRecords records{{{0, 0}, ActiveRegionRecord{}}, {dormant, ActiveRegionRecord{}}};
    records[dormant].state = RegionState::Frozen;
    const EnergyAges ages{{{0, 0}, 0}, {dormant, 19}};
    ASSERT_TRUE(field.ValidateRegionalOwners(records, ages));
    const auto saved = field.SerializeRegionalRecord();
    const auto snapshot = field;
    // Leave both markers and page summaries intact to exercise full validation's refresh.
    EnergyFieldStateTestAccess::TamperPageShape(field, 100, 100);
    const std::array due{RegionKey{0, 0}};
    EnergyResult result;
    ASSERT_TRUE(field.TickRegional(1, records, due, ages, result));
    EXPECT_EQ(result.staged_pages, 0u);
    auto restored = field;
    for (auto* state : {&field, &restored}) {
        EXPECT_FALSE(state->ValidateRegionalOwners(records, result.ages));
        EXPECT_FALSE(EnergyFieldStateTestAccess::Validated(*state));
        EXPECT_FALSE(state->ValidateRegionalOwners(records, result.ages));
        EXPECT_FALSE(state->TickRegional(2, records, {}, result.ages, result));
    }

    // A well-formed page tampered to a new owner must fail joint load admission.
    const auto at = saved.find("P 100 100");
    ASSERT_NE(at, std::string::npos);
    auto tampered = saved;
    tampered.replace(at, std::string("P 100 100").size(), "P 102 100");
    restored = snapshot;
    ASSERT_TRUE(restored.DeserializeRegionalRecord(tampered));
    EXPECT_FALSE(EnergyFieldStateTestAccess::Validated(restored));
    EXPECT_FALSE(restored.ValidateRegionalOwners(records, ages));
    EXPECT_FALSE(restored.TickRegional(1, records, due, ages, result));
    ASSERT_TRUE(restored.DeserializeRegionalRecord(saved));
    ASSERT_TRUE(restored.ValidateRegionalOwners(records, ages));
    restored = snapshot;
    EXPECT_TRUE(restored.ValidateRegionalOwners(records, ages));
}

TEST(RegionalEnergy, MutationEntryPointsClearValidationAndRegionalTicksMaintainIt) {
    const EnergyRecords records{{{0, 0}, ActiveRegionRecord{}}};
    const EnergyAges ages{{{0, 0}, 0}};
    EnergyFieldState initial;
    ASSERT_TRUE(initial.DeserializeRegionalRecord("EFS1 1 0\nP 0 0 0:100\n"));
    ASSERT_TRUE(initial.ValidateRegionalOwners(records, ages));
    // All public writers other than TickRegional, plus the normalization boundary.
    const std::vector<std::pair<const char*, std::function<void(EnergyFieldState&)>>> mutations{
        {"anchor",
         [](auto& field) {
             field.SetAnchorCell(0, 0);
         }},
        {"legacy queue",
         [](auto& field) {
             field.QueueDeposit(1, 0, 0, 0, 1);
         }},
        {"regional queue",
         [](auto& field) {
             EXPECT_TRUE(field.QueueRegionalDeposit(1, 0, 0, 0, 1));
         }},
        {"legacy tick",
         [](auto& field) {
             field.Tick(1);
         }},
        {"legacy deserialize",
         [](auto& field) {
             EXPECT_TRUE(field.DeserializeRecord("EFS1 1 8\nP 0 0 0:101\n", 0));
         }},
        {"regional deserialize",
         [](auto& field) {
             EXPECT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 0 0 0:101\n"));
         }},
        {"empty restore",
         [](auto& field) {
             field.ResetEmptyAtTick(9);
         }},
        {"normalize",
         [](auto& field) {
             EnergyFieldStateTestAccess::Normalize(field);
         }},
        {"legacy serialize",
         [](auto& field) {
             static_cast<void>(field.SerializeRecord(0));
         }},
    };
    for (const auto& [name, mutate] : mutations) {
        SCOPED_TRACE(name);
        auto field = initial;
        ASSERT_TRUE(EnergyFieldStateTestAccess::Validated(field));
        mutate(field);
        EXPECT_FALSE(EnergyFieldStateTestAccess::Validated(field));
        EnergyResult result;
        ASSERT_TRUE(field.TickRegional(1, records, {}, ages, result));
        EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
    }
    auto field = initial;
    EXPECT_FALSE(field.DeserializeRecord("bad", 0));
    EXPECT_FALSE(field.DeserializeRegionalRecord("bad"));
    EXPECT_FALSE(field.QueueRegionalDeposit(1, 0, 0, -1, 1));
    EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
    static_cast<void>(field.SerializeRegionalRecord());
    EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
    EnergyResult result;
    const std::array due{RegionKey{0, 0}};
    ASSERT_TRUE(field.TickRegional(1, records, due, ages, result));
    EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
}

TEST(RegionalEnergy, CachedOwnersTrackRemovalAndStillRequireCurrentRecordsAndAges) {
    EnergyFieldState field;
    ASSERT_TRUE(field.DeserializeRegionalRecord("EFS1 1 0\nP 0 0 0:1\nP 100 100 0:777\n"));
    const auto dormant = EnergyFieldState::OwnerOfCell(1600, 1600);
    EnergyRecords records{{{0, 0}, ActiveRegionRecord{}}, {dormant, ActiveRegionRecord{}}};
    records[dormant].state = RegionState::Frozen;
    EnergyAges ages{{{0, 0}, 7}, {dormant, 19}};
    ASSERT_TRUE(field.ValidateRegionalOwners(records, ages));
    auto corrupted = field;
    EnergyFieldStateTestAccess::TamperPageShape(corrupted, 0, 0);
    const std::array due{RegionKey{0, 0}};
    EnergyResult result;
    EXPECT_FALSE(corrupted.TickRegional(1, records, due, ages, result));
    ASSERT_TRUE(field.TickRegional(1, records, due, ages, result));
    EXPECT_EQ(field.page_count(), 1u);
    ages = result.ages;
    ages.erase({0, 0});
    records.erase({0, 0});
    ASSERT_TRUE(field.TickRegional(2, records, {}, ages, result));
    EXPECT_TRUE(EnergyFieldStateTestAccess::Validated(field));
    const auto before = field.SerializeRegionalRecord();
    EXPECT_FALSE(field.TickRegional(3, records, {}, {}, result));
    EXPECT_FALSE(field.TickRegional(3, EnergyRecords{}, {}, {}, result));
    EXPECT_EQ(field.SerializeRegionalRecord(), before);
    const std::array frozen_due{dormant};
    EXPECT_FALSE(field.TickRegional(3, records, frozen_due, ages, result));
}
