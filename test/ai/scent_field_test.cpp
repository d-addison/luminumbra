// scent/pheromone stigmergy field coverage — deposit, diffusion,
// evaporation, gradient sensing, channel isolation, and determinism. Pure (no
// ECS/world); this is the substrate for tracking/hunting/ant-trail behaviors.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/ScentField.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {
using luminumbra::ai::ScentField;
} // namespace

TEST(ScentField, DepositRaisesOnlyTheTargetCell) {
    ScentField f(8, 8, /*channels=*/1);
    f.Deposit(0, 4, 4, 10.0);
    EXPECT_DOUBLE_EQ(f.Sample(0, 4, 4), 10.0);
    EXPECT_DOUBLE_EQ(f.Sample(0, 0, 0), 0.0);
    EXPECT_DOUBLE_EQ(f.Sample(0, 5, 4), 0.0); // neighbor untouched before diffusion
}

TEST(ScentField, DiffusionSpreadsToNeighbors) {
    ScentField f(8, 8, 1);
    f.Deposit(0, 4, 4, 100.0);
    f.Step(/*diffusion_rate=*/0.2, /*iters=*/1, /*evaporation=*/0.0);
    EXPECT_LT(f.Sample(0, 4, 4), 100.0); // center gave scent to neighbors
    EXPECT_GT(f.Sample(0, 5, 4), 0.0);   // a neighbor now carries some
    EXPECT_GT(f.Sample(0, 4, 5), 0.0);
}

TEST(ScentField, EvaporationDecaysTowardZero) {
    ScentField f(4, 4, 1);
    f.Deposit(0, 1, 1, 50.0);
    const double before = f.Sample(0, 1, 1);
    f.Step(/*diffusion_rate=*/0.0, /*iters=*/0, /*evaporation=*/0.5); // pure decay
    EXPECT_NEAR(f.Sample(0, 1, 1), before * 0.5, 1e-9);
    for (int i = 0; i < 40; ++i)
        f.Step(0.0, 0, 0.5);
    EXPECT_LT(f.Sample(0, 1, 1), 1e-6); // stale trail fades away
}

TEST(ScentField, GradientPointsTowardTheSource) {
    // The tracking/hunting primitive: a deposit creates a gradient that a hunter
    // follows UP toward the source from either side.
    ScentField f(10, 10, 1);
    f.Deposit(0, 5, 5, 100.0);
    f.Step(0.25, 2, 0.0); // spread a little so there's a smooth gradient

    float gx = 0.0f, gz = 0.0f;
    const double mleft = f.Gradient(0, 3, 5, gx, gz);
    EXPECT_GT(mleft, 0.0);
    EXPECT_GT(gx, 0.0f); // west of source -> up-gradient points +X (toward x=5)
    EXPECT_NEAR(gz, 0.0f, 1e-3f);

    const double mright = f.Gradient(0, 7, 5, gx, gz);
    EXPECT_GT(mright, 0.0);
    EXPECT_LT(gx, 0.0f); // east of source -> up-gradient points -X (toward x=5)

    const double msouth = f.Gradient(0, 5, 3, gx, gz);
    EXPECT_GT(msouth, 0.0);
    EXPECT_GT(gz, 0.0f); // south of source -> up-gradient points +Z (toward z=5)
}

TEST(ScentField, GradientSteerTracksTowardAndFleesFromSource) {
    ScentField f(10, 10, 1);
    f.Deposit(0, 5, 5, 100.0);
    f.Step(0.25, 2, 0.0);
    float dx = 0.0f, dz = 0.0f;
    // West of the source: steering TOWARD (sign +1) points +X (a hunter tracking up).
    const float conf = f.GradientSteer(0, 3, 5, /*sign=*/1.0f, /*floor=*/1e-4f, /*k=*/5.0f, dx, dz);
    EXPECT_GT(conf, 0.0f);
    EXPECT_GT(dx, 0.0f);
    EXPECT_NEAR(dz, 0.0f, 1e-3f);
    // Unit direction.
    EXPECT_NEAR(std::sqrt(dx * dx + dz * dz), 1.0f, 1e-3f);
    // Fleeing (sign -1) inverts the heading (prey running from the scent).
    float fx = 0.0f, fz = 0.0f;
    f.GradientSteer(0, 3, 5, -1.0f, 1e-4f, 5.0f, fx, fz);
    EXPECT_LT(fx, 0.0f);
}

TEST(ScentField, GradientSteerFloorCutsOffColdTrail) {
    ScentField f(10, 10, 1);
    f.Deposit(0, 5, 5, 100.0);
    f.Step(0.25, 2, 0.0);
    float dx = 1.0f, dz = 1.0f;
    // A high floor rejects the weak gradient far from the source (cold-trail cutoff).
    const float conf = f.GradientSteer(0, 0, 0, 1.0f, /*floor=*/1e6f, 5.0f, dx, dz);
    EXPECT_FLOAT_EQ(conf, 0.0f);
    EXPECT_FLOAT_EQ(dx, 0.0f); // direction zeroed when below floor
    EXPECT_FLOAT_EQ(dz, 0.0f);
}

TEST(ScentField, ClampBoundsEveryCellToMinMax) {
    ScentField f(5, 5, 1);
    f.Deposit(0, 2, 2, 100.0);
    f.Clamp(/*tau_min=*/1.0, /*tau_max=*/50.0);
    EXPECT_DOUBLE_EQ(f.Sample(0, 2, 2), 50.0); // capped at tau_max (buildup bound)
    EXPECT_DOUBLE_EQ(f.Sample(0, 0, 0), 1.0);  // floored at tau_min (>0 -> no dead cells)
}

TEST(ScentField, ChannelsAreIndependent) {
    ScentField f(6, 6, /*channels=*/3);
    f.Deposit(0, 2, 2, 10.0); // prey scent only
    EXPECT_DOUBLE_EQ(f.Sample(0, 2, 2), 10.0);
    EXPECT_DOUBLE_EQ(f.Sample(1, 2, 2), 0.0); // predator channel unaffected
    EXPECT_DOUBLE_EQ(f.Sample(2, 2, 2), 0.0); // food-trail channel unaffected
}

TEST(ScentField, IsDeterministicAcrossRuns) {
    auto run = []() {
        ScentField f(12, 12, 2);
        f.Deposit(0, 3, 4, 20.0);
        f.Deposit(0, 8, 9, 15.0);
        f.Deposit(1, 5, 5, 30.0);
        for (int i = 0; i < 5; ++i)
            f.Step(0.18, 2, 0.05);
        double acc = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int z = 0; z < 12; ++z)
                for (int x = 0; x < 12; ++x)
                    acc += f.Sample(c, x, z);
        return acc;
    };
    const double a = run();
    const double b = run();
    EXPECT_EQ(a, b); // bit-identical accumulation across identical runs
}

//  wind-advection: a pure +X wind shifts the deposited blob downwind (+X). With an integer
// wind the semi-Lagrangian backtrace lands exactly on source cells, so the blob translates cleanly.
TEST(ScentField, WindAdvectionDriftsScentDownwind) {
    ScentField f(20, 16, 1);
    f.Deposit(0, 4, 8, 100.0);
    // No diffusion / no evaporation: isolate the advection. Wind = (+2, 0) cells/step.
    f.Step(/*rate=*/0.0, /*iters=*/0, /*evap=*/0.0, /*wind_cx=*/2.0, /*wind_cz=*/0.0);
    EXPECT_NEAR(f.Sample(0, 6, 8), 100.0, 1e-9); // blob arrived 2 cells downwind (+X)
    EXPECT_NEAR(f.Sample(0, 4, 8), 0.0, 1e-9);   // original cell now samples empty upwind
}

// Default (zero) wind is byte-identical to the no-wind 3-arg Step — so the canonical scent tick
// (which passes 0 until tuned) is unchanged and needs no re-pin.
TEST(ScentField, ZeroWindStepMatchesNoWindStep) {
    ScentField a(16, 16, 2), b(16, 16, 2);
    a.Deposit(0, 5, 6, 30.0);
    b.Deposit(0, 5, 6, 30.0);
    a.Deposit(1, 9, 9, 12.0);
    b.Deposit(1, 9, 9, 12.0);
    for (int i = 0; i < 4; ++i) {
        a.Step(0.2, 2, 0.05);           // no wind args (legacy call)
        b.Step(0.2, 2, 0.05, 0.0, 0.0); // explicit zero wind
    }
    for (int c = 0; c < 2; ++c)
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x)
                EXPECT_DOUBLE_EQ(a.Sample(c, x, z), b.Sample(c, x, z));
}

// Advection (incl. fractional/bilinear wind) is deterministic across identical runs (run==replay).
TEST(ScentField, WindAdvectionIsDeterministic) {
    auto run = []() {
        ScentField f(18, 18, 1);
        f.Deposit(0, 4, 4, 25.0);
        for (int i = 0; i < 6; ++i)
            f.Step(0.18, 2, 0.05, 1.5, -0.5);
        double acc = 0.0;
        for (int z = 0; z < 18; ++z)
            for (int x = 0; x < 18; ++x)
                acc += f.Sample(0, x, z);
        return acc;
    };
    EXPECT_EQ(run(), run());
}

// Hunt-enabling property: advection carries prey scent FURTHER downwind than diffusion alone, so a
// predator downwind detects scent it otherwise could not — the cue to then move up-wind to the
// prey.
TEST(ScentField, WindCarriesScentFartherDownwindThanDiffusionAlone) {
    ScentField wind(24, 12, 1), still(24, 12, 1);
    wind.Deposit(0, 4, 6, 100.0);
    still.Deposit(0, 4, 6, 100.0);
    for (int i = 0; i < 8; ++i) {
        wind.Step(0.2, 2, 0.02, /*wind_cx=*/1.0, /*wind_cz=*/0.0);
        still.Step(0.2, 2, 0.02, 0.0, 0.0);
    }
    // Far downwind (+X of the source), the advected field carries measurably more scent.
    EXPECT_GT(wind.Sample(0, 14, 6), still.Sample(0, 14, 6));
    EXPECT_GT(wind.Sample(0, 14, 6), 0.0);
}

TEST(ScentField, RegionalFrozenEdgeSealsDepositsAdvectionDiffusionAndClamp) {
    ScentField field(8, 4, 1);
    field.RestoreCell(0, 4, 2, 80.0);
    const ScentField::CellGate due = [](int x, int) {
        return x < 4;
    };
    const ScentField::EdgeGate sealed = [](int x, int, int nx, int) {
        return x < 4 && nx < 4;
    };
    field.BindDepositGate(due);
    field.Deposit(0, 4, 2, 20.0);
    field.Step(0.25, 4, 0.1, -1.0, 0.0, due, sealed);
    field.Clamp(1e-9, 1e6, due);
    EXPECT_DOUBLE_EQ(field.Sample(0, 4, 2), 80.0);
    EXPECT_DOUBLE_EQ(field.Sample(0, 5, 2), 0.0);
    EXPECT_DOUBLE_EQ(field.Sample(0, 3, 2), 1e-9);
}

TEST(ScentField, RegionalCommonBorderExchangesWithoutInteriorEvaporation) {
    ScentField field(4, 2, 1);
    field.RestoreCell(0, 1, 0, 40.0);
    const ScentField::CellGate neither_due = [](int, int) {
        return false;
    };
    const ScentField::EdgeGate border = [](int x, int, int nx, int) {
        return (x == 1 && nx == 2) || (x == 2 && nx == 1);
    };
    field.Step(0.25, 1, 0.1, 0.0, 0.0, neither_due, border);
    EXPECT_DOUBLE_EQ(field.Sample(0, 1, 0), 30.0);
    EXPECT_DOUBLE_EQ(field.Sample(0, 2, 0), 10.0);
}

TEST(ScentField, UngatedAndOpenGatesMatchHandTracedLegacyBits) {
    // Row-major input [0,16;32,48], rate 1/4, one iteration, keep 3/4.
    // No wind: horizontal fluxes -4,-4 then vertical -8,-8 give [12,20;28,36].
    // Smoothing accumulators [12,16;20,24] / (3/4), then evaporation * (3/4),
    // give exactly [12,16;20,24].
    // Wind (1/2,-1/2): bilinear backtraces give [16,24;32,40]. Horizontal fluxes
    // -2,-2 then vertical -4,-4 give [22,26;30,34]. Smoothing accumulators
    // [18,20;22,24] / (3/4), then evaporation * (3/4), give [18,20;22,24].
    const ScentField::CellGate all_cells = [](int, int) {
        return true;
    };
    const ScentField::EdgeGate all_edges = [](int, int, int, int) {
        return true;
    };
    for (const bool with_wind : {false, true}) {
        SCOPED_TRACE(with_wind);
        ScentField legacy_call(2, 2);
        legacy_call.Deposit(0, 1, 0, 16.0);
        legacy_call.Deposit(0, 0, 1, 32.0);
        legacy_call.Deposit(0, 1, 1, 48.0);
        auto empty_gates = legacy_call;
        auto open_gates = legacy_call;
        const double wind_x = with_wind ? 0.5 : 0.0;
        const double wind_z = with_wind ? -0.5 : 0.0;
        if (with_wind) {
            legacy_call.Step(0.25, 1, 0.25, wind_x, wind_z);
        } else {
            legacy_call.Step(0.25, 1, 0.25);
        }
        empty_gates.Step(0.25, 1, 0.25, wind_x, wind_z, {}, {});
        open_gates.Step(0.25, 1, 0.25, wind_x, wind_z, all_cells, all_edges);
        const std::array<double, 4> expected =
            with_wind ? std::array{18.0, 20.0, 22.0, 24.0} : std::array{12.0, 16.0, 20.0, 24.0};
        for (int z = 0; z < 2; ++z) {
            for (int x = 0; x < 2; ++x) {
                const auto bits =
                    std::bit_cast<std::uint64_t>(expected[static_cast<std::size_t>(z * 2 + x)]);
                EXPECT_EQ(std::bit_cast<std::uint64_t>(legacy_call.Sample(0, x, z)), bits);
                EXPECT_EQ(std::bit_cast<std::uint64_t>(empty_gates.Sample(0, x, z)), bits);
                EXPECT_EQ(std::bit_cast<std::uint64_t>(open_gates.Sample(0, x, z)), bits);
            }
        }
        // Clamp, then pure evaporation: [19,19;20,23] or [19,20;22,23], halved.
        legacy_call.Clamp(19.0, 23.0);
        empty_gates.Clamp(19.0, 23.0, {});
        open_gates.Clamp(19.0, 23.0, all_cells);
        legacy_call.Step(0.0, 0, 0.5);
        empty_gates.Step(0.0, 0, 0.5, 0.0, 0.0, {}, {});
        open_gates.Step(0.0, 0, 0.5, 0.0, 0.0, all_cells, all_edges);
        const std::array<double, 4> decayed =
            with_wind ? std::array{9.5, 10.0, 11.0, 11.5} : std::array{9.5, 9.5, 10.0, 11.5};
        for (int z = 0; z < 2; ++z) {
            for (int x = 0; x < 2; ++x) {
                const auto bits =
                    std::bit_cast<std::uint64_t>(decayed[static_cast<std::size_t>(z * 2 + x)]);
                EXPECT_EQ(std::bit_cast<std::uint64_t>(legacy_call.Sample(0, x, z)), bits);
                EXPECT_EQ(std::bit_cast<std::uint64_t>(empty_gates.Sample(0, x, z)), bits);
                EXPECT_EQ(std::bit_cast<std::uint64_t>(open_gates.Sample(0, x, z)), bits);
            }
        }
    }
}

TEST(ScentField, GatedDiffusionRetainsLegacyRateValidation) {
    const ScentField::EdgeGate all_edges = [](int, int, int, int) {
        return true;
    };
    for (const double invalid_rate : {0.5, std::numeric_limits<double>::infinity()}) {
        ScentField legacy_call(2, 2);
        ScentField gated(2, 2);
        EXPECT_THROW(legacy_call.Step(invalid_rate, 1, 0.0), std::invalid_argument);
        EXPECT_THROW(gated.Step(invalid_rate, 1, 0.0, 0.0, 0.0, {}, all_edges),
                     std::invalid_argument);
    }
}
