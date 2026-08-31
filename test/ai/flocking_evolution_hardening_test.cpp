// ADVERSARIAL determinism-hardening for the boids flocking helper, the GA
// (Evolution) operators, and grid A*. These tests hunt the bugs the happy-path
// suites (flocking_test / evolution_test / astar_grid_test) MISS:
//   * producer->consumer field-meaning mismatches and threshold EDGES,
//   * order-INDEPENDENCE under non-id / shuffled accumulation order
//     (float addition is non-associative -> a SUM-order-dependent steer is a bug
//      vs the header's "commutative accumulation -> independent of neighbour
//      order (run==replay)" contract),
//   * coincident / zero-distance degeneracies (separation -> inf, cohesion -> NaN),
//   * run==replay EXACT float equality, and NaN/inf survival.
//
// Registered into frontier_gates_test (include root../src), so the canonical
// "luminumbra_common/ai/..." include form is used (matches evolution_test.cpp
// and astar_grid_test.cpp). Components are unused here (pure helpers), so no
// Luminumbra::Components alias is needed.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/AStarGrid.h"
#include "luminumbra_common/ai/Evolution.h"
#include "luminumbra_common/ai/Flocking.h"
#include "luminumbra_common/core/DeterministicRng.h"

#include "../support/SeededShuffle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace {

using luminumbra::ai::BlendCrossover;
using luminumbra::ai::ComputeFlockSteer;
using luminumbra::ai::EvolveGeneration;
using luminumbra::ai::FindPath;
using luminumbra::ai::FlockParams;
using luminumbra::ai::FlockSteer;
using luminumbra::ai::GaussianMutate;
using luminumbra::ai::GeneBound;
using luminumbra::ai::GridCoord;
using luminumbra::ai::NavGrid;
using luminumbra::ai::TournamentSelect;
using luminumbra::core::DeterministicRng;

using NList = std::vector<std::pair<float, float>>;

// A hand-picked neighbour set (all inside the default cohesion radius 12 and a
// spread that crosses the separation radius 3) whose float accumulation diverges
// between forward and reverse order under the production summation. Verified
// offline: fwd vs reverse steer differs in the last ULPs on BOTH x and z.
NList DivergingSet() {
    return {{2.13f, 0.37f},
            {-1.77f, 1.11f},
            {2.59f, -2.41f},
            {0.43f, 1.97f},
            {-1.19f, -2.73f},
            {2.31f, 1.55f},
            {-0.77f, 0.83f},
            {1.93f, -0.61f},
            {2.66f, -1.83f},
            {-0.91f, 2.27f},
            {1.41f, 2.66f},
            {-2.31f, -0.49f},
            {0.83f, -2.55f},
            {2.07f, 0.91f},
            {-1.55f, 1.73f},
            {0.29f, -1.37f}};
}

std::vector<GeneBound> Bounds4() {
    return {{-1, 1}, {-1, 1}, {-1, 1}, {-1, 1}};
}

NavGrid FromAscii(const std::vector<std::string>& rows) {
    const int h = static_cast<int>(rows.size());
    const int w = h > 0 ? static_cast<int>(rows[0].size()) : 0;
    std::vector<std::uint8_t> cells(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0u);
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x)
            cells[static_cast<std::size_t>(z) * w + x] = (rows[z][x] == '#') ? 0u : 1u;
    return NavGrid(w, h, std::move(cells));
}

int PathCost(const std::vector<GridCoord>& p) {
    int c = 0;
    for (std::size_t i = 1; i < p.size(); ++i) {
        const int dx = p[i].x - p[i - 1].x, dz = p[i].z - p[i - 1].z;
        c += (dx != 0 && dz != 0) ? 14 : 10;
    }
    return c;
}

// ---------------------------------------------------------------------------
// FLOCKING — order independence (the header's load-bearing determinism claim).
// ---------------------------------------------------------------------------

// Order-independent accumulation is required because run==replay must remain
// exact even when callers provide equivalent neighbour permutations.
TEST(FlockHardening, ForwardVsReverseOrderIdentical) {
    NList fwd = DivergingSet();
    NList rev(fwd.rbegin(), fwd.rend());
    const FlockSteer a = ComputeFlockSteer(0.0f, 0.0f, fwd);
    const FlockSteer b = ComputeFlockSteer(0.0f, 0.0f, rev);
    EXPECT_FLOAT_EQ(a.x, b.x) << "steer must not depend on neighbour ordering";
    EXPECT_FLOAT_EQ(a.z, b.z) << "steer must not depend on neighbour ordering";
}

// Same divergence under a rotation (a different permutation than reverse) — pins
// it as a general order sensitivity, not a fluke of one swap.
TEST(FlockHardening, RotatedOrderIdentical) {
    NList base = DivergingSet();
    NList rot = base;
    std::rotate(rot.begin(), rot.begin() + 1, rot.end()); // offset that diverges fwd
    const FlockSteer a = ComputeFlockSteer(0.0f, 0.0f, base);
    const FlockSteer b = ComputeFlockSteer(0.0f, 0.0f, rot);
    EXPECT_FLOAT_EQ(a.x, b.x);
    EXPECT_FLOAT_EQ(a.z, b.z);
}

// A second, independent diverging configuration off a non-origin self position.
TEST(FlockHardening, OrderIndependentOffOrigin) {
    NList fwd = DivergingSet();
    NList rev(fwd.rbegin(), fwd.rend());
    const FlockSteer a = ComputeFlockSteer(0.4f, -0.6f, fwd);
    const FlockSteer b = ComputeFlockSteer(0.4f, -0.6f, rev);
    EXPECT_FLOAT_EQ(a.x, b.x);
    EXPECT_FLOAT_EQ(a.z, b.z);
}

// run==replay: calling the SAME helper with the SAME inputs twice is byte-exact.
TEST(FlockHardening, RunEqualsReplaySameOrder) {
    NList n = DivergingSet();
    const FlockSteer a = ComputeFlockSteer(0.4f, -0.6f, n);
    const FlockSteer b = ComputeFlockSteer(0.4f, -0.6f, n);
    EXPECT_EQ(a.x, b.x);
    EXPECT_EQ(a.z, b.z);
}

// ORDER-INDEPENDENCE via a DETERMINISTIC SEEDED SHUFFLE (test-rigor  / ).
// RunEqualsReplaySameOrder above re-runs the SAME order and so cannot catch the order
// dependence the boids bug (a8b689c) was. The reverse/rotate tests above use ad-hoc
// permutations; this closes that gap with the integer-SEEDED Fisher-Yates  wants,
// over the VERIFIED order-diverging set, asserting byte-identical steer.
TEST(FlockHardening, SeededShuffleOrderIdentical) {
    NList base = DivergingSet();
    NList shuffled = Luminumbra::TestSupport::SeededShuffled(base, /*seed=*/0x51EED99u);
    bool reordered = false; // guard : the seed must actually permute.
    for (std::size_t i = 0; i < base.size(); ++i)
        if (shuffled[i] != base[i]) {
            reordered = true;
            break;
        }
    ASSERT_TRUE(reordered) << "seed must actually permute the neighbour order";

    const FlockSteer a = ComputeFlockSteer(0.4f, -0.6f, base);
    const FlockSteer b = ComputeFlockSteer(0.4f, -0.6f, shuffled);
    EXPECT_FLOAT_EQ(a.x, b.x) << "steer must not depend on (seeded-shuffled) neighbour order";
    EXPECT_FLOAT_EQ(a.z, b.z) << "steer must not depend on (seeded-shuffled) neighbour order";

    // A second, independent seed -> a different permutation -> same steer.
    NList shuffled2 = Luminumbra::TestSupport::SeededShuffled(base, /*seed=*/0xA17EED3u);
    const FlockSteer c = ComputeFlockSteer(0.4f, -0.6f, shuffled2);
    EXPECT_FLOAT_EQ(a.x, c.x);
    EXPECT_FLOAT_EQ(a.z, c.z);
}

// ---------------------------------------------------------------------------
// FLOCKING — degenerate / edge inputs.
// ---------------------------------------------------------------------------

// Zero-distance coincident neighbour: separation must NOT divide by zero
// (the dist>1e-5 guard), and cohesion toward a centroid sitting on self must NOT
// produce NaN (the len>1e-5 guard) -> a finite, here ZERO, steer.
TEST(FlockHardening, CoincidentNeighborIsFiniteZero) {
    NList n = {{0.0f, 0.0f}};
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, n);
    EXPECT_TRUE(std::isfinite(s.x)) << "separation div-by-zero leaked to inf/NaN";
    EXPECT_TRUE(std::isfinite(s.z));
    EXPECT_FLOAT_EQ(s.x, 0.0f);
    EXPECT_FLOAT_EQ(s.z, 0.0f);
}

// Many coincident neighbours stacked on self: still finite (cohesion centroid is
// self, separation all guarded out).
TEST(FlockHardening, ManyCoincidentNeighborsFinite) {
    NList n(20, {2.5f, -1.5f}); // all identical, none on self
    const FlockSteer s = ComputeFlockSteer(2.5f, -1.5f, n);
    EXPECT_TRUE(std::isfinite(s.x));
    EXPECT_TRUE(std::isfinite(s.z));
}

// A neighbour an epsilon away (inside the 1e-5 separation guard band but nonzero):
// the separation term must stay finite, not explode through the 1/dist factor.
TEST(FlockHardening, NearZeroDistanceSeparationFinite) {
    NList n = {{1.0e-4f, 0.0f}}; // dist ~1e-4 > 1e-5 guard -> inv ~1e4
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, n);
    EXPECT_TRUE(std::isfinite(s.x));
    EXPECT_TRUE(std::isfinite(s.z));
    EXPECT_LT(s.x, 0.0f) << "an almost-on-top neighbour on +x pushes toward -x";
}

// A neighbour EXACTLY at the separation radius contributes no separation push
// (crowd = 1 - dist/sep = 0), and the strict < means it is excluded anyway.
// It is still inside the cohesion radius, so the net steer is pure cohesion (+x).
TEST(FlockHardening, NeighborAtSeparationRadiusNoSeparation) {
    FlockParams p; // sep radius 3, neighbour radius 12
    NList n = {{3.0f, 0.0f}};
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, n, p);
    EXPECT_TRUE(std::isfinite(s.x));
    EXPECT_GT(s.x, 0.0f) << "exactly at sep radius -> no push, cohesion pulls toward it";
    EXPECT_NEAR(s.z, 0.0f, 1e-6f);
}

// A neighbour EXACTLY at the cohesion (neighbor) radius is INCLUDED (the producer
// uses <=). Producer->consumer edge: a consumer assuming a strict < boundary
// would disagree. Pins the inclusive contract.
TEST(FlockHardening, NeighborExactlyAtCohesionRadiusIncluded) {
    FlockParams p;
    p.neighbor_radius = 5.0f;
    p.separation_radius = 1.0f; // keep the neighbour out of separation
    NList at = {{5.0f, 0.0f}};  // dist == neighbor_radius -> included
    NList just_out = {{5.0001f, 0.0f}};
    const FlockSteer s_at = ComputeFlockSteer(0.0f, 0.0f, at, p);
    const FlockSteer s_out = ComputeFlockSteer(0.0f, 0.0f, just_out, p);
    EXPECT_GT(s_at.x, 0.0f) << "<= radius: the boundary neighbour pulls cohesion";
    EXPECT_FLOAT_EQ(s_out.x, 0.0f) << "just beyond radius contributes nothing";
}

// A neighbour just INSIDE the cohesion radius but with a single member: cohesion
// pulls directly toward it (centroid == that neighbour).
TEST(FlockHardening, SingleNeighborCohesionPointsAtIt) {
    FlockParams p;
    p.separation_radius = 1.0f; // neighbour at dist 6 is well outside sep
    NList n = {{0.0f, 6.0f}};
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, n, p);
    EXPECT_NEAR(s.x, 0.0f, 1e-6f);
    EXPECT_GT(s.z, 0.0f) << "cohesion toward the only neighbour on +z";
}

// Empty neighbour list -> exact zero (gating no-op: no neighbours means no steer,
// never a NaN from a 1/0 centroid).
TEST(FlockHardening, EmptyNeighborsZeroNoNaN) {
    const FlockSteer s = ComputeFlockSteer(7.0f, -3.0f, {});
    EXPECT_EQ(s.x, 0.0f);
    EXPECT_EQ(s.z, 0.0f);
}

// Zero cohesion weight but nonzero separation: only the push survives, finite.
TEST(FlockHardening, ZeroCohesionWeightLeavesSeparation) {
    FlockParams p;
    p.cohesion_weight = 0.0f;
    NList n = {{1.0f, 0.0f}}; // inside sep radius
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, n, p);
    EXPECT_TRUE(std::isfinite(s.x));
    EXPECT_LT(s.x, 0.0f) << "separation pushes off the close neighbour";
}

// Symmetric ring of neighbours: cohesion centroid is self and separations cancel
// -> a (near) zero net steer, and crucially FINITE (no NaN from the zero centroid
// direction). Documents the balanced-herd degeneracy.
TEST(FlockHardening, SymmetricRingCancelsToFiniteNearZero) {
    NList n = {{2.0f, 0.0f}, {-2.0f, 0.0f}, {0.0f, 2.0f}, {0.0f, -2.0f}};
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, n);
    EXPECT_TRUE(std::isfinite(s.x));
    EXPECT_TRUE(std::isfinite(s.z));
    EXPECT_NEAR(s.x, 0.0f, 1e-5f);
    EXPECT_NEAR(s.z, 0.0f, 1e-5f);
}

// Separation strictly dominates over cohesion up close (weights 1.4 vs 0.6): a
// lone too-close neighbour yields a net push AWAY, not toward. Producer->consumer
// sign contract.
TEST(FlockHardening, SeparationOutweighsCohesionUpClose) {
    NList n = {{0.5f, 0.0f}}; // very close on +x, inside sep radius
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, n);
    EXPECT_LT(s.x, 0.0f) << "net steer must be away from a crowding neighbour";
}

// ---------------------------------------------------------------------------
// EVOLUTION (GA) — reproducibility, edges, gating no-ops.
// ---------------------------------------------------------------------------

// run==replay of a bred genome: BlendCrossover is a pure function of
// (parents, rng stream). Same seed -> identical child, ULP-exact.
TEST(EvoHardening, BlendCrossoverRunEqualsReplay) {
    std::vector<float> a = {0.1f, -0.4f, 0.7f, -0.9f};
    std::vector<float> b = {-0.2f, 0.8f, -0.3f, 0.5f};
    auto run = [&]() {
        DeterministicRng rng(20260619u);
        return BlendCrossover(a, b, rng);
    };
    const std::vector<float> c1 = run();
    const std::vector<float> c2 = run();
    ASSERT_EQ(c1.size(), c2.size());
    for (std::size_t i = 0; i < c1.size(); ++i)
        EXPECT_FLOAT_EQ(c1[i], c2[i]) << "bred gene " << i << " not reproducible";
}

// ORACLE (test-rigor ): INDEPENDENT-RECOMPUTE + GOLDEN-LITERAL for BlendCrossover.
// BlendCrossoverRunEqualsReplay above only proves the operator is reproducible (zero
// correctness signal). This recomputes the expected child by HAND from the documented
// formula child[i] = a[i] + (b[i]-a[i])*u with u=rng.next_unit per gene (Evolution.h:
// 57-60), driving a SECOND identically-seeded rng in the SAME draw order, then pins the
// result to frozen golden literals. A sign/operator flip in the lerp breaks BOTH the
// recompute (different code path) and the golden (fixed numbers) -> RED (mutation-pass).
TEST(EvoHardening, BlendCrossoverOracle) {
    std::vector<float> a = {0.1f, -0.4f, 0.7f, -0.9f};
    std::vector<float> b = {-0.2f, 0.8f, -0.3f, 0.5f};
    const std::uint64_t kSeed = 0x0A11CE99ull;

    DeterministicRng prod(kSeed);
    const std::vector<float> child = BlendCrossover(a, b, prod);

    // Independent recompute with a second, identically-seeded rng, SAME draw order.
    DeterministicRng oracle(kSeed);
    std::array<float, 4> expect{};
    for (std::size_t i = 0; i < 4; ++i) {
        const float u = oracle.next_unit();
        expect[i] = a[i] + (b[i] - a[i]) * u;
    }
    ASSERT_EQ(child.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(child[i], expect[i]) << "bred gene " << i << " != hand recompute";

    // GOLDEN LITERALS (frozen on THIS toolchain). A draw-ORDER regression that the
    // recompute would track in lockstep is ALSO caught here (fixed numbers).
    EXPECT_NEAR(child[0], -0.04411011f, 1e-5f);
    EXPECT_NEAR(child[1], 0.62961972f, 1e-5f);
    EXPECT_NEAR(child[2], 0.22541648f, 1e-5f);
    EXPECT_NEAR(child[3], 0.33877230f, 1e-5f);
}

// run==replay of a FULL next generation, EXACT (operator==): every gene of every
// child must match across two identical runs.
TEST(EvoHardening, EvolveGenerationByteExactReplay) {
    auto run = [&]() {
        DeterministicRng rng(424242u);
        std::vector<std::vector<float>> pop = {{0.1f, 0.2f, 0.3f, 0.4f},
                                               {-0.5f, 0.6f, -0.7f, 0.8f},
                                               {0.9f, -0.1f, 0.2f, -0.3f},
                                               {0.0f, 0.0f, 0.0f, 0.0f},
                                               {0.4f, 0.4f, -0.4f, -0.4f},
                                               {-0.9f, 0.9f, 0.1f, -0.1f}};
        std::vector<float> fitness = {3.0f, 1.0f, 5.0f, 2.0f, 4.0f, 0.0f};
        return EvolveGeneration(pop, fitness, Bounds4(), 0.12f, 2, 3, rng);
    };
    EXPECT_EQ(run(), run());
}

// Mutation rate at the sigma_frac == 0 EDGE: a pure no-op on an in-bounds genome
// (only clamp runs, which leaves an in-bounds genome untouched).
TEST(EvoHardening, ZeroSigmaIsExactNoop) {
    DeterministicRng rng(5u);
    std::vector<float> g = {0.25f, -0.5f, 0.75f, -0.125f};
    const std::vector<float> before = g;
    GaussianMutate(g, Bounds4(), 0.0f, rng);
    EXPECT_EQ(g, before);
}

// Negative sigma_frac is also treated as a no-op (sigma_frac > 0 gate): documents
// the gate edge so a future sign flip is caught.
TEST(EvoHardening, NegativeSigmaIsNoop) {
    DeterministicRng rng(6u);
    std::vector<float> g = {0.25f, -0.5f, 0.75f, -0.125f};
    const std::vector<float> before = g;
    GaussianMutate(g, Bounds4(), -0.1f, rng);
    EXPECT_EQ(g, before);
}

// Zero sigma must NOT consume the RNG stream (the >0 gate skips next_gaussian):
// a draw taken right after a zero-sigma mutate must equal the draw with no mutate
// in between. Guards the "gating no-op leaves state untouched" contract.
TEST(EvoHardening, ZeroSigmaDoesNotAdvanceRng) {
    DeterministicRng ref(99u);
    const float expected = ref.next_unit();

    DeterministicRng rng(99u);
    std::vector<float> g = {0.1f, 0.2f, 0.3f, 0.4f};
    GaussianMutate(g, Bounds4(), 0.0f, rng); // must not touch rng
    const float got = rng.next_unit();
    EXPECT_FLOAT_EQ(got, expected) << "zero-sigma mutate must not advance the RNG";
}

// Mutation reproducibility at a LARGE sigma (1.0): genome += N*1*range still
// clamps into bounds AND is bit-identical across two seeded runs.
TEST(EvoHardening, LargeSigmaMutateReproducibleAndInBounds) {
    auto bounds = Bounds4();
    auto run = [&]() {
        DeterministicRng rng(13u);
        std::vector<float> g = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int it = 0; it < 50; ++it)
            GaussianMutate(g, bounds, 1.0f, rng);
        return g;
    };
    const std::vector<float> a = run();
    const std::vector<float> b = run();
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i], b[i]);
        EXPECT_GE(a[i], bounds[i].lo);
        EXPECT_LE(a[i], bounds[i].hi);
        EXPECT_TRUE(std::isfinite(a[i]));
    }
}

// Mutation must clamp a genome that STARTS out of bounds (producer/consumer
// contract: ClampGene runs unconditionally). An initially-OOB gene comes back
// in-range even at sigma 0.
TEST(EvoHardening, OutOfBoundsGenomeIsClampedIn) {
    DeterministicRng rng(8u);
    std::vector<float> g = {5.0f, -5.0f, 0.0f, 9.0f}; // 3 genes OOB for [-1,1]
    GaussianMutate(g, Bounds4(), 0.0f, rng);
    EXPECT_FLOAT_EQ(g[0], 1.0f);
    EXPECT_FLOAT_EQ(g[1], -1.0f);
    EXPECT_FLOAT_EQ(g[2], 0.0f);
    EXPECT_FLOAT_EQ(g[3], 1.0f);
}

// BlendCrossover child stays within [min(a,b), max(a,b)] per gene EXACTLY at the
// u-edges: with parents in-bounds the child is in-bounds with no clamp. Tested
// over many draws including the smallest/largest u the RNG can emit.
TEST(EvoHardening, CrossoverChildWithinParentRangeManyDraws) {
    DeterministicRng rng(31u);
    std::vector<float> a = {-1.0f, 1.0f, -0.3f, 0.6f};
    std::vector<float> b = {1.0f, -1.0f, 0.9f, -0.6f};
    for (int it = 0; it < 500; ++it) {
        const std::vector<float> c = BlendCrossover(a, b, rng);
        for (std::size_t i = 0; i < c.size(); ++i) {
            const float lo = std::min(a[i], b[i]);
            const float hi = std::max(a[i], b[i]);
            EXPECT_GE(c[i], lo);
            EXPECT_LE(c[i], hi);
        }
    }
}

// Crossover of identical parents returns those genes EXACTLY for every u (lerp
// with a==b collapses to a). No drift from the blend.
TEST(EvoHardening, CrossoverIdenticalParentsIsIdentity) {
    DeterministicRng rng(17u);
    std::vector<float> a = {0.123f, -0.456f, 0.789f, -0.321f};
    for (int it = 0; it < 100; ++it) {
        const std::vector<float> c = BlendCrossover(a, a, rng);
        ASSERT_EQ(c.size(), a.size());
        for (std::size_t i = 0; i < a.size(); ++i)
            EXPECT_FLOAT_EQ(c[i], a[i]);
    }
}

// BlendCrossover with mismatched parent lengths yields a child of the SHORTER
// length (std::min) — pins the contract so a consumer never reads past the end.
TEST(EvoHardening, CrossoverMismatchedLengthsTakesShorter) {
    DeterministicRng rng(23u);
    std::vector<float> a = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
    std::vector<float> b = {-0.1f, -0.2f};
    const std::vector<float> c = BlendCrossover(a, b, rng);
    EXPECT_EQ(c.size(), 2u);
    for (std::size_t i = 0; i < c.size(); ++i) {
        EXPECT_GE(c[i], std::min(a[i], b[i]));
        EXPECT_LE(c[i], std::max(a[i], b[i]));
    }
}

// Tournament k <= 1 is a single uniform pick (no selection pressure): with all
// equal fitness it still returns a valid in-range index, deterministically.
TEST(EvoHardening, TournamentKOneIsSinglePick) {
    DeterministicRng rng(3u);
    std::vector<float> fitness = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int it = 0; it < 50; ++it) {
        const std::size_t pick = TournamentSelect(fitness, 1, rng);
        EXPECT_LT(pick, fitness.size());
    }
}

// Tournament k == 0 is clamped to one round (rounds = k<1?1:k): must behave like
// k==1 and never index out of range / loop zero times.
TEST(EvoHardening, TournamentKZeroClampsToOneRound) {
    auto draw = [](int k) {
        DeterministicRng rng(55u);
        std::vector<float> fitness = {1.0f, 2.0f, 3.0f, 4.0f};
        std::vector<std::size_t> picks;
        for (int it = 0; it < 20; ++it)
            picks.push_back(TournamentSelect(fitness, k, rng));
        return picks;
    };
    EXPECT_EQ(draw(0), draw(1)) << "k=0 must be identical to k=1 (one round)";
}

// Tournament reproducibility: same seed + fitness -> identical pick sequence.
TEST(EvoHardening, TournamentReproducible) {
    auto draw = []() {
        DeterministicRng rng(7777u);
        std::vector<float> fitness = {0.2f, 0.9f, 0.1f, 0.5f, 0.7f};
        std::vector<std::size_t> picks;
        for (int it = 0; it < 100; ++it)
            picks.push_back(TournamentSelect(fitness, 3, rng));
        return picks;
    };
    EXPECT_EQ(draw(), draw());
}

// Tournament tie-break: equal fitness -> the LOWEST index wins (the `>` strict
// compare never replaces an equal). With two clear winners tied at the top, the
// lower index must be favoured over the run.
TEST(EvoHardening, TournamentTieBreaksToLowerIndex) {
    DeterministicRng rng(2u);
    std::vector<float> fitness = {9.0f, 9.0f, 0.0f, 0.0f}; // indices 0,1 tie at top
    int got0 = 0, got1 = 0;
    for (int it = 0; it < 400; ++it) {
        const std::size_t p = TournamentSelect(fitness, 4, rng);
        if (p == 0u)
            ++got0;
        if (p == 1u)
            ++got1;
    }
    // When both 0 and 1 appear among contestants, 0 must win the tie (strict >).
    EXPECT_GT(got0, got1) << "ties must resolve to the lower index";
}

// Elitism carries the top genomes UNCHANGED and preserves population size, even
// when the best is NOT at index 0 (ordering by fitness desc, index asc).
TEST(EvoHardening, ElitismCarriesTopTwoUnchanged) {
    DeterministicRng rng(4u);
    std::vector<std::vector<float>> pop = {{0.1f, 0.1f, 0.1f, 0.1f},
                                           {0.9f, 0.8f, 0.7f, 0.6f},
                                           {0.2f, 0.2f, 0.2f, 0.2f},
                                           {0.5f, 0.4f, 0.3f, 0.2f}};
    std::vector<float> fitness = {1.0f, 99.0f, 2.0f, 50.0f}; // best=1, second=3
    auto next = EvolveGeneration(pop, fitness, Bounds4(), 0.1f, 2, 2, rng);
    ASSERT_EQ(next.size(), pop.size());
    EXPECT_EQ(next[0], pop[1]) << "fittest elite carried byte-for-byte";
    EXPECT_EQ(next[1], pop[3]) << "second elite carried byte-for-byte";
}

// Elitism larger than the population is clamped (std::min): elitism=10 on a pop
// of 4 must still return exactly 4 genomes and not crash / over-copy.
TEST(EvoHardening, ElitismExceedingPopIsClamped) {
    DeterministicRng rng(9u);
    std::vector<std::vector<float>> pop = {{0.1f, 0.1f, 0.1f, 0.1f},
                                           {0.2f, 0.2f, 0.2f, 0.2f},
                                           {0.3f, 0.3f, 0.3f, 0.3f},
                                           {0.4f, 0.4f, 0.4f, 0.4f}};
    std::vector<float> fitness = {1.0f, 2.0f, 3.0f, 4.0f};
    auto next = EvolveGeneration(pop, fitness, Bounds4(), 0.1f, 10, 2, rng);
    EXPECT_EQ(next.size(), pop.size());
}

// Negative elitism is treated as zero (no elites) and still fills the population.
TEST(EvoHardening, NegativeElitismIsZero) {
    DeterministicRng rng(11u);
    std::vector<std::vector<float>> pop = {
        {0.1f, 0.1f, 0.1f, 0.1f}, {0.2f, 0.2f, 0.2f, 0.2f}, {0.3f, 0.3f, 0.3f, 0.3f}};
    std::vector<float> fitness = {1.0f, 2.0f, 3.0f};
    auto next = EvolveGeneration(pop, fitness, Bounds4(), 0.1f, -5, 2, rng);
    EXPECT_EQ(next.size(), pop.size());
}

// Empty population -> empty next generation (no crash, no div-by-zero).
TEST(EvoHardening, EmptyPopulationYieldsEmpty) {
    DeterministicRng rng(1u);
    std::vector<std::vector<float>> pop;
    std::vector<float> fitness;
    auto next = EvolveGeneration(pop, fitness, Bounds4(), 0.1f, 1, 2, rng);
    EXPECT_TRUE(next.empty());
}

// Single-genome population with elitism: the lone elite is preserved and size 1.
TEST(EvoHardening, SingletonPopulationPreservesElite) {
    DeterministicRng rng(12u);
    std::vector<std::vector<float>> pop = {{0.3f, -0.3f, 0.6f, -0.6f}};
    std::vector<float> fitness = {1.0f};
    auto next = EvolveGeneration(pop, fitness, Bounds4(), 0.1f, 1, 2, rng);
    ASSERT_EQ(next.size(), 1u);
    EXPECT_EQ(next[0], pop[0]);
}

// ---------------------------------------------------------------------------
// A* — determinism under tie-rich grids, edges, unreachable goals.
// ---------------------------------------------------------------------------

// An open grid is FULL of equal-cost paths (lots of f-score ties). The header
// promises a tie-break by ascending cell index -> a single stable path. Two
// calls must return the identical sequence (run==replay on a tie-rich board).
TEST(AStarHardening, TieRichOpenGridIsStable) {
    auto grid = FromAscii({"........",
                           "........",
                           "........",
                           "........",
                           "........",
                           "........",
                           "........",
                           "........"});
    auto a = FindPath(grid, {0, 0}, {7, 7});
    auto b = FindPath(grid, {0, 0}, {7, 7});
    ASSERT_FALSE(a.empty());
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        EXPECT_TRUE(a[i] == b[i]) << "tie-broken path diverged at step " << i;
}

// Optimality on the open grid: corner-to-corner cost must equal the octile
// optimum 7*14 = 98 (pure diagonal), not a longer detour.
TEST(AStarHardening, OpenGridDiagonalCostIsOptimal) {
    auto grid = FromAscii({"........",
                           "........",
                           "........",
                           "........",
                           "........",
                           "........",
                           "........",
                           "........"});
    auto p = FindPath(grid, {0, 0}, {7, 7});
    ASSERT_FALSE(p.empty());
    EXPECT_EQ(PathCost(p), 7 * 14);
    EXPECT_TRUE((p.front() == GridCoord{0, 0}));
    EXPECT_TRUE((p.back() == GridCoord{7, 7}));
}

// Symmetric reverse: forward and reversed-endpoint searches yield the SAME total
// cost (the cost field is symmetric even if the exact cells differ).
TEST(AStarHardening, ReverseSearchSameCost) {
    auto grid = FromAscii({"..........",
                           ".##.###.#.",
                           "....#...#.",
                           ".#.##.##..",
                           "..#....#..",
                           ".##.##.#..",
                           ".........."});
    auto fwd = FindPath(grid, {0, 0}, {9, 6});
    auto rev = FindPath(grid, {9, 6}, {0, 0});
    ASSERT_FALSE(fwd.empty());
    ASSERT_FALSE(rev.empty());
    EXPECT_EQ(PathCost(fwd), PathCost(rev)) << "octile cost must be symmetric";
}

// Unreachable goal (fully walled) -> empty path, not a partial/garbage one.
TEST(AStarHardening, UnreachableGoalIsEmpty) {
    auto enclosed = FromAscii({".....", ".###.", ".#.#.", ".###.", "....."});
    auto p = FindPath(enclosed, {0, 0}, {2, 2});
    EXPECT_TRUE(p.empty());
}

// Out-of-bounds endpoints -> empty (treated as blocked), no crash.
TEST(AStarHardening, OutOfBoundsEndpointsEmpty) {
    auto grid = FromAscii({"...", "...", "..."});
    EXPECT_TRUE(FindPath(grid, {-1, 0}, {2, 2}).empty());
    EXPECT_TRUE(FindPath(grid, {0, 0}, {3, 0}).empty());
    EXPECT_TRUE(FindPath(grid, {0, 0}, {0, 3}).empty());
}

// Start == goal -> singleton path of exactly that cell (no spurious neighbours).
TEST(AStarHardening, StartEqualsGoalSingleton) {
    auto grid = FromAscii({"...", "...", "..."});
    auto p = FindPath(grid, {1, 1}, {1, 1});
    ASSERT_EQ(p.size(), 1u);
    EXPECT_TRUE((p.front() == GridCoord{1, 1}));
}

// 1x1 grid, start==goal at (0,0): the smallest possible board still returns the
// singleton (degenerate dimension survival).
TEST(AStarHardening, OneByOneGridSingleton) {
    auto grid = FromAscii({"."});
    auto p = FindPath(grid, {0, 0}, {0, 0});
    ASSERT_EQ(p.size(), 1u);
    EXPECT_TRUE((p.front() == GridCoord{0, 0}));
}

// Corner-cut prevention: a diagonal between two orthogonally-blocked corners must
// NOT clip through; with no other route the goal is unreachable -> empty.
TEST(AStarHardening, NoDiagonalCornerCut) {
    auto grid = FromAscii({".#", "#."});
    auto p = FindPath(grid, {0, 0}, {1, 1});
    EXPECT_TRUE(p.empty()) << "must not cut the blocked corner";
}

// Every returned step is an 8-neighbour move onto a walkable cell, and the path
// is contiguous start..goal (no teleports through walls). Stress board.
TEST(AStarHardening, PathContiguousAndWalkable) {
    auto grid = FromAscii({"..........",
                           ".##.###.#.",
                           "....#...#.",
                           ".#.##.##..",
                           "..#....#..",
                           ".##.##.#..",
                           ".........."});
    auto p = FindPath(grid, {0, 0}, {9, 6});
    ASSERT_FALSE(p.empty());
    EXPECT_TRUE((p.front() == GridCoord{0, 0}));
    EXPECT_TRUE((p.back() == GridCoord{9, 6}));
    for (std::size_t i = 1; i < p.size(); ++i) {
        const int dx = p[i].x - p[i - 1].x, dz = p[i].z - p[i - 1].z;
        EXPECT_LE(dx * dx, 1);
        EXPECT_LE(dz * dz, 1);
        EXPECT_FALSE(dx == 0 && dz == 0) << "no zero-length step";
        EXPECT_TRUE(grid.walkable(p[i].x, p[i].z));
    }
}

} // namespace
