// scent/pheromone stigmergy field coverage — deposit, diffusion,
// evaporation, gradient sensing, channel isolation, and determinism. Pure (no
// ECS/world); this is the substrate for tracking/hunting/ant-trail behaviors.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/ScentField.h"

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
