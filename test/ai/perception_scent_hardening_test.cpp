// ADVERSARIAL determinism + producer->consumer contract hardening for the
// STIGMERGY + sensing substrate: ScentField (deposit/diffuse/evaporate/gradient),
// ScentDepositSystem (WRITE side), ScentSteeringSystem (READ side -> wish), and
// PerceptionSystem (vision cone + hearing audiogram -> awareness).
//
// Sibling happy-path tests cover the golden path. This file hunts the gaps:
// unit/coordinate-space mismatches between producer and consumer, threshold EDGES,
// coincident/zero-distance degeneracies, order-independence, gating no-ops,
// run==replay EXACT equality, and NaN/inf/degenerate-input survival.

#include <cmath>
#include <vector>

#include "gtest/gtest.h"

#include "luminumbra_common/ai/Perception.h"
#include "luminumbra_common/ai/PerceptionSystem.h"
#include "luminumbra_common/ai/ScentDepositSystem.h"
#include "luminumbra_common/ai/ScentField.h"
#include "luminumbra_common/ai/ScentSteeringSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"

namespace {
namespace Comp = ::Luminumbra::Components;

using Comp::AwarenessComponent;
using Comp::LocomotionIntentComponent;
using Comp::LocomotionProfile;
using Comp::PerceptionComponent;
using Comp::ScentSenseComponent;
using Comp::SensableComponent;
using Comp::TransformComponent;
using luminumbra::ai::AwarenessState;
using luminumbra::ai::HeardLoudness;
using luminumbra::ai::HearingProfile;
using luminumbra::ai::InVisionCone;
using luminumbra::ai::PerceivedLoudness;
using luminumbra::ai::PitchSensitivity;
using luminumbra::ai::RunPerceptionSystemOnTick;
using luminumbra::ai::RunScentDepositOnTick;
using luminumbra::ai::RunScentSteeringOnTick;
using luminumbra::ai::ScentField;
using luminumbra::ai::WorldToCell;

constexpr float kDt = 1.0f / 30.0f;

// --- shared spawn helpers (id-ordered traversal is canonical) ---------------

entt::entity MakeEmitter(entt::registry& r, float x, float z, int channel, float amount) {
    const auto e = r.create();
    r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
    auto& s = r.emplace<SensableComponent>(e);
    s.scent_channel = channel;
    s.scent_deposit = amount;
    return e;
}

entt::entity MakeSniffer(entt::registry& r, float x, float z, float sign, int channel = 0) {
    const auto e = r.create();
    r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
    auto& s = r.emplace<ScentSenseComponent>(e);
    s.channel = channel;
    s.sign = sign;
    s.strength = 1.0f;
    s.floor = 1e-4f;
    s.weber_k = 5.0f;
    r.emplace<LocomotionIntentComponent>(e);
    auto& p = r.emplace<LocomotionProfile>(e);
    p.move_speed = 3.0f;
    return e;
}

entt::entity MakePerceiver(entt::registry& r, float x, float z, float fx, float fz,
                           std::uint32_t faction = 1) {
    const auto e = r.create();
    r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
    auto& p = r.emplace<PerceptionComponent>(e);
    p.vision_cos_half_fov = 0.5f;
    p.vision_range = 30.0f;
    p.facing_x = fx;
    p.facing_z = fz;
    p.faction = faction;
    r.emplace<AwarenessComponent>(e);
    return e;
}

entt::entity MakeSensable(entt::registry& r, float x, float z, std::uint32_t faction, float loud,
                          float pitch) {
    const auto e = r.create();
    r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
    auto& s = r.emplace<SensableComponent>(e);
    s.faction = faction;
    s.noise_loudness = loud;
    s.noise_pitch = pitch;
    return e;
}

// Flatten a whole ScentField to a vector for byte-exact run==replay comparison.
std::vector<double> FlattenField(const ScentField& f) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(f.channels()) * f.width() * f.height());
    for (int c = 0; c < f.channels(); ++c)
        for (int z = 0; z < f.height(); ++z)
            for (int x = 0; x < f.width(); ++x) out.push_back(f.Sample(c, x, z));
    return out;
}

} // namespace

// ===========================================================================
// ScentField: gradient stencil + coordinate-space contract
// ===========================================================================

// The gradient writes (gx,gz) pointing UP-gradient toward stronger scent. A raw
// single-cell deposit (no diffusion) must give a usable central difference from a
// neighbor cell: this nails the +X/-X/+Z/-Z sign mapping the steering side relies
// on (a producer->consumer sign flip here silently steers creatures the wrong way).
TEST(ScentFieldHardening, GradientSignMapsAxesCorrectlyWithoutDiffusion) {
    ScentField f(11, 11, 1);
    f.Deposit(0, 5, 5, 100.0);
    float gx = 0.0f, gz = 0.0f;
    // West of source (x=4): up-gradient must point +X toward x=5.
    EXPECT_GT(f.Gradient(0, 4, 5, gx, gz), 0.0);
    EXPECT_GT(gx, 0.0f);
    EXPECT_NEAR(gz, 0.0f, 1e-6f);
    // East of source (x=6): up-gradient must point -X.
    f.Gradient(0, 6, 5, gx, gz);
    EXPECT_LT(gx, 0.0f);
    // North/South on the Z axis: the SECOND component must carry the z gradient,
    // not leak into gx (catches an x/z stencil swap).
    EXPECT_GT(f.Gradient(0, 5, 4, gx, gz), 0.0);
    EXPECT_GT(gz, 0.0f);
    EXPECT_NEAR(gx, 0.0f, 1e-6f);
    f.Gradient(0, 5, 6, gx, gz);
    EXPECT_LT(gz, 0.0f);
}

// At the exact source cell of a symmetric deposit the central difference is zero
// on both axes (neighbors equal). The gradient magnitude must be exactly 0 — not
// a NaN, not a tiny noise value (a flat field must read flat).
TEST(ScentFieldHardening, GradientAtSymmetricPeakIsExactlyZero) {
    ScentField f(11, 11, 1);
    f.Deposit(0, 5, 5, 50.0);
    f.Step(0.25, 2, 0.0); // symmetric spread about (5,5)
    float gx = 1.0f, gz = 1.0f;
    const double mag = f.Gradient(0, 5, 5, gx, gz);
    EXPECT_DOUBLE_EQ(mag, 0.0);
    EXPECT_FLOAT_EQ(gx, 0.0f);
    EXPECT_FLOAT_EQ(gz, 0.0f);
}

// Gradient at a corner cell must use the edge-CLAMP stencil (no out-of-bounds
// read, no crash) and stay finite. The interior neighbor toward the source must
// still produce the correct up-gradient sign.
TEST(ScentFieldHardening, GradientAtCornerClampsAndStaysFinite) {
    ScentField f(8, 8, 1);
    f.Deposit(0, 4, 4, 100.0);
    f.Step(0.25, 3, 0.0); // 0.25 = the max stable diffusion rate (Step rejects > 0.25)
    float gx = 0.0f, gz = 0.0f;
    const double mag = f.Gradient(0, 0, 0, gx, gz); // corner toward interior source
    EXPECT_TRUE(std::isfinite(gx));
    EXPECT_TRUE(std::isfinite(gz));
    EXPECT_TRUE(std::isfinite(mag));
    // Source is to the +X/+Z interior, so up-gradient at the corner heads that way.
    EXPECT_GE(gx, 0.0f);
    EXPECT_GE(gz, 0.0f);
}

// Out-of-bounds gradient is a defined no-op: zero direction, zero magnitude.
TEST(ScentFieldHardening, GradientOutOfBoundsIsZeroNoOp) {
    ScentField f(6, 6, 1);
    f.Deposit(0, 3, 3, 100.0);
    f.Step(0.25, 2, 0.0); // 0.25 = the max stable diffusion rate (Step rejects > 0.25)
    float gx = 7.0f, gz = -7.0f;
    EXPECT_DOUBLE_EQ(f.Gradient(0, -1, 3, gx, gz), 0.0);
    EXPECT_FLOAT_EQ(gx, 0.0f);
    EXPECT_FLOAT_EQ(gz, 0.0f);
    EXPECT_DOUBLE_EQ(f.Gradient(0, 3, 99, gx, gz), 0.0);
    EXPECT_DOUBLE_EQ(f.Gradient(5, 3, 3, gx, gz), 0.0); // bad channel
}

// GradientSteer must NEVER emit a NaN unit direction from a flat field. A flat
// field yields zero gradient -> the normalize 1/mag would be 1/0; the guard must
// return 0 confidence and a zero direction instead.
TEST(ScentFieldHardening, GradientSteerOnFlatFieldDoesNotNaN) {
    ScentField f(10, 10, 1); // completely empty -> flat zero field
    float dx = 9.0f, dz = 9.0f;
    const float conf = f.GradientSteer(0, 5, 5, 1.0f, 0.0f, 1.0f, dx, dz);
    EXPECT_FLOAT_EQ(conf, 0.0f);
    EXPECT_FALSE(std::isnan(dx));
    EXPECT_FALSE(std::isnan(dz));
    EXPECT_FLOAT_EQ(dx, 0.0f);
    EXPECT_FLOAT_EQ(dz, 0.0f);
}

// The steer direction is a UNIT vector regardless of absolute concentration; the
// confidence is Weber-normalized m/(m+k) in [0,1). Verify both the unit-length
// invariant and the confidence formula at a known gradient.
TEST(ScentFieldHardening, GradientSteerEmitsUnitDirectionAndWeberConfidence) {
    ScentField f(11, 11, 1);
    f.Deposit(0, 5, 5, 100.0);
    f.Step(0.25, 2, 0.0);
    float gx = 0.0f, gz = 0.0f;
    const double mag = f.Gradient(0, 4, 5, gx, gz);
    ASSERT_GT(mag, 0.0);
    const float k = 5.0f;
    float dx = 0.0f, dz = 0.0f;
    const float conf = f.GradientSteer(0, 4, 5, 1.0f, 0.0f, k, dx, dz);
    EXPECT_NEAR(std::sqrt(dx * dx + dz * dz), 1.0f, 1e-3f);
    const float m = static_cast<float>(mag);
    EXPECT_NEAR(conf, m / (m + k), 1e-4f);
}

// floor edge: the cutoff is `mag < floor` (strict). A gradient magnitude exactly
// AT the floor must NOT be rejected (it commits). Construct a known single-cell
// gradient so the magnitude is exact.
TEST(ScentFieldHardening, GradientSteerFloorEdgeIsInclusiveAtFloor) {
    ScentField f(11, 11, 1);
    // Single-cell deposit, no diffusion: at (4,5) gx = sample(5,5)-sample(3,5) = A.
    const double amount = 7.0;
    f.Deposit(0, 5, 5, amount);
    float gx = 0.0f, gz = 0.0f;
    const double mag = f.Gradient(0, 4, 5, gx, gz); // == amount on +X axis
    ASSERT_NEAR(mag, amount, 1e-9);
    const float exact = static_cast<float>(mag);
    float dx = 0.0f, dz = 0.0f;
    // floor exactly == magnitude: `mag < floor` is false -> committed.
    const float conf = f.GradientSteer(0, 4, 5, 1.0f, exact, 1.0f, dx, dz);
    EXPECT_GT(conf, 0.0f);
    EXPECT_GT(dx, 0.0f);
    // A floor a hair ABOVE the magnitude rejects.
    float dx2 = 5.0f, dz2 = 5.0f;
    const float conf2 = f.GradientSteer(0, 4, 5, 1.0f, std::nextafter(exact, 1e30f), 1.0f, dx2, dz2);
    EXPECT_FLOAT_EQ(conf2, 0.0f);
    EXPECT_FLOAT_EQ(dx2, 0.0f);
    EXPECT_FLOAT_EQ(dz2, 0.0f);
}

// GradientSteer with a non-positive weber_k must not divide by zero: the header
// falls back to k=1. Confidence stays finite and in [0,1).
TEST(ScentFieldHardening, GradientSteerNonPositiveKFallsBackFinite) {
    ScentField f(11, 11, 1);
    f.Deposit(0, 5, 5, 100.0);
    f.Step(0.25, 2, 0.0);
    float dx = 0.0f, dz = 0.0f;
    const float conf = f.GradientSteer(0, 4, 5, 1.0f, 0.0f, /*k=*/0.0f, dx, dz);
    EXPECT_TRUE(std::isfinite(conf));
    EXPECT_GE(conf, 0.0f);
    EXPECT_LT(conf, 1.0f);
}

// ===========================================================================
// ScentField: decay / diffusion determinism + idempotency under re-run
// ===========================================================================

// Pure evaporation (iters 0) is an EXACT per-cell multiply by keep = 1-evap. Two
// half-decays must equal one quarter-keep exactly (run==replay determinism over
// repeated Steps, no diffusion smoothing in the way).
TEST(ScentFieldHardening, PureEvaporationIsExactRepeatableMultiply) {
    ScentField a(4, 4, 1), b(4, 4, 1);
    a.Deposit(0, 1, 1, 64.0);
    b.Deposit(0, 1, 1, 64.0);
    a.Step(0.0, 0, 0.5); // x0.5
    a.Step(0.0, 0, 0.5); // x0.5 -> x0.25 total
    b.Step(0.0, 0, 0.5); // single does NOT match; verify the two-step path
    EXPECT_DOUBLE_EQ(a.Sample(0, 1, 1), 16.0);
    EXPECT_DOUBLE_EQ(b.Sample(0, 1, 1), 32.0);
}

// A full deposit/Step/Clamp sequence must be byte-exact across two independent
// runs (the field's only sources of state are deterministic). Compare the whole
// flattened field, not just an accumulation, to catch per-cell divergence.
TEST(ScentFieldHardening, FieldSequenceIsByteExactAcrossRuns) {
    auto run = []() {
        ScentField f(16, 16, 3);
        f.Deposit(0, 3, 4, 20.0);
        f.Deposit(1, 8, 9, 15.0);
        f.Deposit(2, 5, 5, 30.0);
        for (int i = 0; i < 6; ++i) {
            f.Step(0.18, 2, 0.05);
            f.Clamp(0.0, 1000.0);
        }
        return FlattenField(f);
    };
    const auto a = run();
    const auto b = run();
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i], b[i]) << "cell " << i;
}

// Diffusion must be order-INDEPENDENT of how deposits were laid: depositing A then
// B vs B then A into the same cells, then stepping identically, must produce a
// byte-identical field (add_impulse is commutative; a non-commutative write order
// would be a determinism bug).
TEST(ScentFieldHardening, DepositOrderDoesNotChangeField) {
    ScentField f1(12, 12, 1), f2(12, 12, 1);
    f1.Deposit(0, 3, 3, 10.0);
    f1.Deposit(0, 8, 8, 25.0);
    f2.Deposit(0, 8, 8, 25.0);
    f2.Deposit(0, 3, 3, 10.0);
    for (int i = 0; i < 4; ++i) {
        f1.Step(0.2, 2, 0.03);
        f2.Step(0.2, 2, 0.03);
    }
    const auto a = FlattenField(f1);
    const auto b = FlattenField(f2);
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i], b[i]) << "cell " << i;
}

// Evaporation = 1.0 fully clears every cell after a single step (keep = 0). Edge
// of the [0,1] clamp; also confirms evap > 1 is clamped to 1 (full clear), not a
// negative keep that would flip sign.
TEST(ScentFieldHardening, EvaporationOneAndAboveFullyClears) {
    ScentField f(5, 5, 1);
    f.Deposit(0, 2, 2, 100.0);
    f.Step(0.0, 0, 1.0);
    EXPECT_DOUBLE_EQ(f.Sample(0, 2, 2), 0.0);
    ScentField g(5, 5, 1);
    g.Deposit(0, 2, 2, 100.0);
    g.Step(0.0, 0, 5.0); // clamped to keep=0
    EXPECT_DOUBLE_EQ(g.Sample(0, 2, 2), 0.0);
}

// Negative evaporation is clamped to 0 (keep = 1): a pure step must be a no-op,
// never an amplification.
TEST(ScentFieldHardening, NegativeEvaporationIsClampedToNoOp) {
    ScentField f(5, 5, 1);
    f.Deposit(0, 2, 2, 42.0);
    f.Step(0.0, 0, -3.0); // keep clamps to 1 -> unchanged
    EXPECT_DOUBLE_EQ(f.Sample(0, 2, 2), 42.0);
}

// Total scent must be non-increasing under diffuse+evaporate (diffusion is
// conservative; evaporation only removes). Catches any smoothing pass that would
// inject energy.
TEST(ScentFieldHardening, StepNeverIncreasesTotalScent) {
    ScentField f(10, 10, 1);
    f.Deposit(0, 5, 5, 100.0);
    auto total = [&]() {
        double s = 0.0;
        for (int z = 0; z < 10; ++z)
            for (int x = 0; x < 10; ++x) s += f.Sample(0, x, z);
        return s;
    };
    double prev = total();
    for (int i = 0; i < 8; ++i) {
        f.Step(0.2, 2, 0.05);
        const double now = total();
        EXPECT_LE(now, prev + 1e-6);
        prev = now;
    }
}

// Channels stay isolated through a full Step+Clamp cycle (diffusion/evaporation
// must not bleed channel 0 into channel 1).
TEST(ScentFieldHardening, ChannelsStayIsolatedThroughStep) {
    ScentField f(8, 8, 2);
    f.Deposit(0, 4, 4, 100.0);
    for (int i = 0; i < 5; ++i) {
        f.Step(0.2, 2, 0.02);
        f.Clamp(0.0, 1e9);
    }
    double ch1 = 0.0;
    for (int z = 0; z < 8; ++z)
        for (int x = 0; x < 8; ++x) ch1 += f.Sample(1, x, z);
    EXPECT_DOUBLE_EQ(ch1, 0.0);
}

// ===========================================================================
// ScentDepositSystem: producer (world coords) -> field cell mapping
// ===========================================================================

// The deposit producer floors world XZ to a cell via WorldToCell. A deposit at a
// NEGATIVE world coordinate must land in the negative-floored cell, which is
// out-of-bounds for a [0,W) field and therefore a silent no-op (not wrapped to
// cell 0 by a truncating cast). Verifies the producer respects the field bounds.
TEST(ScentDepositHardening, NegativeWorldCoordFloorsOutOfBoundsNoOp) {
    ScentField f(20, 20, 1);
    entt::registry r;
    MakeEmitter(r, -0.5f, 5.0f, 0, 10.0f); // floors to cell x = -1 -> OOB
    const auto stats = RunScentDepositOnTick(r, f, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(stats.emitters, 1u); // it was considered an emitter...
    EXPECT_DOUBLE_EQ(f.Sample(0, 0, 0), 0.0); // ...but did NOT smear into cell 0
}

// origin offset: the producer subtracts origin before flooring. An emitter at
// world (10,10) with origin (8,8) and cell_size 2 must deposit into cell (1,1).
// Catches an origin/cell-size mis-application (the classic world<->grid mismatch).
TEST(ScentDepositHardening, OriginAndCellSizeMapWorldToCorrectCell) {
    ScentField f(20, 20, 1);
    entt::registry r;
    MakeEmitter(r, 10.0f, 10.0f, 0, 5.0f);
    RunScentDepositOnTick(r, f, /*origin_x=*/8.0f, /*origin_z=*/8.0f, /*cell_size=*/2.0f);
    EXPECT_GE(f.Sample(0, 1, 1), 5.0); // (10-8)/2 = 1
    EXPECT_DOUBLE_EQ(f.Sample(0, 5, 5), 0.0);
}

// cell_size <= 0 is a guarded no-op (no divide-by-zero in WorldToCell, no deposit).
TEST(ScentDepositHardening, NonPositiveCellSizeIsNoOp) {
    ScentField f(20, 20, 1);
    entt::registry r;
    MakeEmitter(r, 5.0f, 5.0f, 0, 10.0f);
    const auto stats = RunScentDepositOnTick(r, f, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(stats.emitters, 0u);
    EXPECT_DOUBLE_EQ(f.Sample(0, 5, 5), 0.0);
}

// Gating no-op: a registry with entities that carry NO SensableComponent (or only
// Transform) deposits nothing and reports zero emitters — the canonical roster is
// untouched.
TEST(ScentDepositHardening, NonSensableEntitiesAreUntouchedGatingNoOp) {
    ScentField f(20, 20, 1);
    entt::registry r;
    const auto plain = r.create();
    r.emplace<TransformComponent>(plain).position = Luminumbra::Vec3(5.0f, 0.0f, 5.0f);
    const auto stats = RunScentDepositOnTick(r, f, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(stats.emitters, 0u);
    EXPECT_TRUE(FlattenField(f) == std::vector<double>(20 * 20, 0.0));
}

// Deposit is independent of spawn/iteration order: two emitters laying into the
// same cell must produce the same total whether spawned A-then-B or B-then-A.
TEST(ScentDepositHardening, DepositIsSpawnOrderIndependent) {
    auto totalAt = [](bool reversed) {
        ScentField f(20, 20, 1);
        entt::registry r;
        if (!reversed) {
            MakeEmitter(r, 5.0f, 5.0f, 0, 3.0f);
            MakeEmitter(r, 5.0f, 5.0f, 0, 7.0f);
        } else {
            MakeEmitter(r, 5.0f, 5.0f, 0, 7.0f);
            MakeEmitter(r, 5.0f, 5.0f, 0, 3.0f);
        }
        RunScentDepositOnTick(r, f, 0.0f, 0.0f, 1.0f);
        return f.Sample(0, 5, 5);
    };
    EXPECT_EQ(totalAt(false), totalAt(true));
}

// run==replay for the deposit producer: two identical synthetic registries +
// fields produce byte-identical fields.
TEST(ScentDepositHardening, ProducerIsRunReplayExact) {
    auto run = []() {
        ScentField f(24, 24, 2);
        entt::registry r;
        MakeEmitter(r, 3.5f, 4.5f, 0, 12.0f);
        MakeEmitter(r, 18.2f, 9.1f, 1, 6.0f);
        MakeEmitter(r, 7.0f, 7.0f, 0, 4.0f);
        for (int i = 0; i < 4; ++i) RunScentDepositOnTick(r, f, 0.0f, 0.0f, 1.5f);
        return FlattenField(f);
    };
    const auto a = run();
    const auto b = run();
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i], b[i]) << "cell " << i;
}

// ===========================================================================
// ScentSteeringSystem: field (consumer) -> wish bias contract
// ===========================================================================

// The deposit producer and the steering consumer MUST share one world<->cell
// mapping. Deposit a trail through the deposit system, step the field, then have a
// sniffer at a known world point read it: the wish must bias TOWARD the source
// along the producer's coordinate frame (catches a producer/consumer origin or
// cell-size mismatch).
TEST(ScentSteeringHardening, ConsumerSharesProducerCoordinateFrame) {
    const float origin = 0.0f, cell = 1.0f;
    ScentField f(20, 20, 1);
    entt::registry rdep;
    MakeEmitter(rdep, 12.0f, 10.0f, 0, 200.0f); // source at world (12,10)
    RunScentDepositOnTick(rdep, f, origin, origin, cell);
    f.Step(0.25, 3, 0.0);

    entt::registry r;
    const auto a = MakeSniffer(r, 8.0f, 10.0f, +1.0f); // west of source -> track +X
    RunScentSteeringOnTick(r, f, origin, origin, cell);
    const auto& w = r.get<LocomotionIntentComponent>(a).wish_xz;
    EXPECT_GT(w.x, 0.0f);
    EXPECT_NEAR(w.y, 0.0f, 1e-3f);
}

// The steering bias is ADDED to the existing wish then clamped to move_speed. A
// pre-existing wish at the budget plus a scent bias must not exceed the budget
// (the producer of the wish is the seek system; this consumer must respect the
// shared move_speed budget — the unit here is m/s, not a unit direction).
TEST(ScentSteeringHardening, BiasedWishNeverExceedsMoveSpeedBudget) {
    auto f = ([] {
        ScentField fld(20, 20, 1);
        fld.Deposit(0, 10, 10, 300.0);
        fld.Step(0.25, 3, 0.0);
        return fld;
    })();
    entt::registry r;
    const auto a = MakeSniffer(r, 5.0f, 10.0f, +1.0f);
    auto& intent = r.get<LocomotionIntentComponent>(a);
    intent.wish_xz = Luminumbra::Vec2(3.0f, 0.0f); // already at the 3 m/s budget
    RunScentSteeringOnTick(r, f, 0.0f, 0.0f, 1.0f);
    const auto& w = r.get<LocomotionIntentComponent>(a).wish_xz;
    EXPECT_LE(std::sqrt(w.x * w.x + w.y * w.y), 3.0f + 1e-4f);
}

// Gating no-op: an inactive channel (channel < 0) must leave the wish EXACTLY as
// it was (not zero it, not nudge it) — the consumer only touches active sniffers.
TEST(ScentSteeringHardening, InactiveChannelLeavesWishExactlyUnchanged) {
    ScentField f(20, 20, 1);
    f.Deposit(0, 10, 10, 100.0);
    f.Step(0.25, 3, 0.0);
    entt::registry r;
    const auto a = MakeSniffer(r, 5.0f, 10.0f, +1.0f, /*channel=*/-1);
    auto& intent = r.get<LocomotionIntentComponent>(a);
    intent.wish_xz = Luminumbra::Vec2(0.7f, -0.3f);
    const auto stats = RunScentSteeringOnTick(r, f, 0.0f, 0.0f, 1.0f);
    const auto& w = r.get<LocomotionIntentComponent>(a).wish_xz;
    EXPECT_FLOAT_EQ(w.x, 0.7f);
    EXPECT_FLOAT_EQ(w.y, -0.3f);
    EXPECT_EQ(stats.steered, 0u);
}

// A sniffer standing on a flat (zero) field must leave its wish untouched and
// produce no NaN — the consumer's normalize path must be guarded end to end.
TEST(ScentSteeringHardening, FlatFieldLeavesWishUntouchedNoNaN) {
    ScentField f(20, 20, 1); // empty
    entt::registry r;
    const auto a = MakeSniffer(r, 5.0f, 5.0f, +1.0f);
    auto& intent = r.get<LocomotionIntentComponent>(a);
    intent.wish_xz = Luminumbra::Vec2(1.0f, 2.0f);
    RunScentSteeringOnTick(r, f, 0.0f, 0.0f, 1.0f);
    const auto& w = r.get<LocomotionIntentComponent>(a).wish_xz;
    EXPECT_FALSE(std::isnan(w.x));
    EXPECT_FALSE(std::isnan(w.y));
    EXPECT_FLOAT_EQ(w.x, 1.0f);
    EXPECT_FLOAT_EQ(w.y, 2.0f);
}

// Steering is spawn-order independent: two sniffers at distinct cells get the same
// bias regardless of which was created first (id-ordered traversal is canonical,
// and the two are independent so the result must match exactly).
TEST(ScentSteeringHardening, SteeringIsSpawnOrderIndependent) {
    auto biasFor = [](bool reversed) {
        ScentField f(20, 20, 1);
        f.Deposit(0, 10, 10, 100.0);
        f.Step(0.25, 3, 0.0);
        entt::registry r;
        entt::entity probe;
        if (!reversed) {
            MakeSniffer(r, 14.0f, 10.0f, +1.0f);
            probe = MakeSniffer(r, 6.0f, 10.0f, +1.0f);
        } else {
            probe = MakeSniffer(r, 6.0f, 10.0f, +1.0f);
            MakeSniffer(r, 14.0f, 10.0f, +1.0f);
        }
        RunScentSteeringOnTick(r, f, 0.0f, 0.0f, 1.0f);
        return r.get<LocomotionIntentComponent>(probe).wish_xz.x;
    };
    EXPECT_EQ(biasFor(false), biasFor(true));
}

// A creature sensing its OWN deposit channel: deposit + sense on the same cell of
// the same channel. The sniffer sits exactly on the symmetric peak, so the
// gradient there is zero and it must NOT self-propel (no phantom bias from its own
// scent at its own cell).
TEST(ScentSteeringHardening, SnifferOnOwnDepositPeakGetsZeroBias) {
    ScentField f(20, 20, 1);
    f.Deposit(0, 10, 10, 100.0);
    f.Step(0.25, 2, 0.0); // symmetric about (10,10)
    entt::registry r;
    const auto a = MakeSniffer(r, 10.0f, 10.0f, +1.0f); // exactly on the peak cell
    const auto stats = RunScentSteeringOnTick(r, f, 0.0f, 0.0f, 1.0f);
    const auto& w = r.get<LocomotionIntentComponent>(a).wish_xz;
    EXPECT_FLOAT_EQ(w.x, 0.0f);
    EXPECT_FLOAT_EQ(w.y, 0.0f);
    EXPECT_EQ(stats.steered, 0u);
}

// run==replay for the steering consumer over many ticks on a moving field.
TEST(ScentSteeringHardening, ConsumerIsRunReplayExact) {
    auto run = []() {
        ScentField f(20, 20, 1);
        f.Deposit(0, 10, 10, 100.0);
        entt::registry r;
        const auto a = MakeSniffer(r, 4.0f, 11.0f, +1.0f);
        float acc = 0.0f;
        for (int i = 0; i < 10; ++i) {
            f.Step(0.2, 2, 0.02);
            auto& intent = r.get<LocomotionIntentComponent>(a);
            intent.wish_xz = Luminumbra::Vec2(0.0f, 0.0f);
            RunScentSteeringOnTick(r, f, 0.0f, 0.0f, 1.0f);
            acc += intent.wish_xz.x + intent.wish_xz.y;
        }
        return acc;
    };
    EXPECT_EQ(run(), run());
}

// ===========================================================================
// Perception primitives: vision cone radians/degrees + half-vs-full angle edges
// ===========================================================================

// The cone test consumes cos(HALF-angle). A target just INSIDE the half-FOV is
// seen; just OUTSIDE is rejected. Use 59/61 deg against a 60deg half-FOV so the
// result is robust to float rounding of cos at the exact boundary.
TEST(PerceptionHardening, VisionConeHalfFovInsideVsOutside) {
    const float cos_half = 0.5f; // half-FOV 60deg (120deg total)
    const float in = 59.0f * 0.017453292519943295f;
    EXPECT_TRUE(InVisionCone(0, 0, 1, 0, cos_half, 30.0f, std::cos(in) * 10.0f,
                             std::sin(in) * 10.0f));
    const float out = 61.0f * 0.017453292519943295f;
    EXPECT_FALSE(InVisionCone(0, 0, 1, 0, cos_half, 30.0f, std::cos(out) * 10.0f,
                              std::sin(out) * 10.0f));
}

// Dead-ahead boundary: a target straight along the facing axis has dot == dist
// EXACTLY (dz = 0), so for cos_half just below 1 it is always inside, and a tighter
// cone toward an off-axis target rejects it. Verifies the >= comparison without
// float-cos fragility.
TEST(PerceptionHardening, VisionConeDeadAheadIsAlwaysInside) {
    // Facing +X, target straight ahead: dot = dist -> dot >= cos_half*dist for any
    // cos_half <= 1, so always inside regardless of how tight the cone.
    EXPECT_TRUE(InVisionCone(0, 0, 1, 0, /*cos_half=*/0.999f, 30.0f, 10.0f, 0.0f));
    EXPECT_TRUE(InVisionCone(0, 0, 1, 0, /*cos_half=*/0.5f, 30.0f, 10.0f, 0.0f));
}

// cos_half_fov is a HALF-angle, not full: a target at 90deg off-axis must be
// REJECTED by a 120deg (half=60deg) cone — catches a half-vs-full-angle confusion
// where the consumer would treat the value as a full-FOV cosine.
TEST(PerceptionHardening, VisionConeRejectsTargetBeyondHalfAngle) {
    EXPECT_FALSE(InVisionCone(0, 0, 1, 0, /*cos_half=*/0.5f, 30.0f, /*tx=*/0.0f, /*tz=*/10.0f));
}

// cos_half_fov = -1 is full 360: anything in range is visible, including directly
// behind. cos_half = 1 is a blind-ahead-only ray: a target off-axis is rejected.
TEST(PerceptionHardening, VisionConeFullAndDegenerateAngleExtremes) {
    EXPECT_TRUE(InVisionCone(0, 0, 1, 0, /*cos_half=*/-1.0f, 30.0f, -10.0f, 0.0f)); // behind, 360
    EXPECT_TRUE(InVisionCone(0, 0, 1, 0, 1.0f, 30.0f, 10.0f, 0.0f));  // dead ahead, ray
    EXPECT_FALSE(InVisionCone(0, 0, 1, 0, 1.0f, 30.0f, 10.0f, 0.01f)); // a hair off-axis, ray
}

// Coincident target (dist ~ 0) is always visible regardless of facing/FOV — the
// dist2 < 1e-8 guard must fire before any normalize.
TEST(PerceptionHardening, VisionConeCoincidentTargetAlwaysVisible) {
    EXPECT_TRUE(InVisionCone(5, 5, 0, 1, /*cos_half=*/1.0f, 30.0f, 5.0f, 5.0f));
}

// Range edge: the cone uses dist2 > range*range (strict). A target EXACTLY at
// range is still in range; just beyond is out.
TEST(PerceptionHardening, VisionConeRangeBoundary) {
    EXPECT_TRUE(InVisionCone(0, 0, 1, 0, -1.0f, 10.0f, 10.0f, 0.0f));  // exactly at range
    EXPECT_FALSE(InVisionCone(0, 0, 1, 0, -1.0f, 10.0f, 10.001f, 0.0f)); // just beyond
}

// ===========================================================================
// Perception primitives: hearing audiogram falloff edges
// ===========================================================================

// PerceivedLoudness range edge: at dist == range, loudness is 0 (>= range cutoff);
// just inside range it is a small positive value. A zero/negative range is silent.
TEST(PerceptionHardening, PerceivedLoudnessRangeEdge) {
    EXPECT_FLOAT_EQ(PerceivedLoudness(24.0f, 1.0f, 24.0f), 0.0f); // exactly at range -> 0
    EXPECT_GT(PerceivedLoudness(23.99f, 1.0f, 24.0f), 0.0f);
    EXPECT_FLOAT_EQ(PerceivedLoudness(5.0f, 1.0f, 0.0f), 0.0f);   // no range -> silent
    EXPECT_FLOAT_EQ(PerceivedLoudness(5.0f, 0.0f, 24.0f), 0.0f);  // silent source
}

// PitchSensitivity triangular falloff: 1 at the peak, linearly to 0 at +/-
// bandwidth, exactly 0 beyond. Verify the band edges precisely.
TEST(PerceptionHardening, PitchSensitivityTriangleEdges) {
    HearingProfile ear;
    ear.peak_pitch = 0.5f;
    ear.bandwidth = 0.25f;
    EXPECT_FLOAT_EQ(PitchSensitivity(ear, 0.5f), 1.0f);          // at peak
    EXPECT_NEAR(PitchSensitivity(ear, 0.625f), 0.5f, 1e-5f);     // halfway out
    EXPECT_FLOAT_EQ(PitchSensitivity(ear, 0.75f), 0.0f);        // at band edge -> 0
    EXPECT_FLOAT_EQ(PitchSensitivity(ear, 0.9f), 0.0f);         // beyond band -> 0
    EXPECT_FLOAT_EQ(PitchSensitivity(ear, 0.25f), 0.0f);       // lower band edge -> 0
    EXPECT_NEAR(PitchSensitivity(ear, 0.375f), 0.5f, 1e-5f);   // halfway out, lower side
}

// bandwidth <= 0 is a degenerate "only the exact peak is audible" ear: the peak
// pitch reads 1, any other pitch reads 0 (no divide-by-zero from 1 - d/bandwidth).
TEST(PerceptionHardening, PitchSensitivityZeroBandwidthIsPeakOnly) {
    HearingProfile ear;
    ear.peak_pitch = 0.4f;
    ear.bandwidth = 0.0f;
    EXPECT_FLOAT_EQ(PitchSensitivity(ear, 0.4f), 1.0f);
    EXPECT_FLOAT_EQ(PitchSensitivity(ear, 0.41f), 0.0f);
    EXPECT_FALSE(std::isnan(PitchSensitivity(ear, 0.41f)));
}

// HeardLoudness composes distance x gain x pitch-sensitivity and applies the
// threshold (strict >). A perceived value AT the threshold is NOT heard; a hair
// above IS. Catches an inclusive/exclusive threshold flip.
TEST(PerceptionHardening, HeardLoudnessThresholdIsStrict) {
    HearingProfile ear;
    ear.range = 10.0f;
    ear.peak_pitch = 0.5f;
    ear.bandwidth = 1.0f;
    ear.gain = 1.0f;
    // At dist 0, pitch at peak: base = loudness * (1 - 0) = loudness; perceived =
    // loudness * 1 * 1 = loudness. Set threshold exactly = loudness -> NOT heard.
    ear.threshold = 0.4f;
    EXPECT_FLOAT_EQ(HeardLoudness(0.0f, 0.4f, 0.5f, ear), 0.0f); // perceived == threshold
    EXPECT_GT(HeardLoudness(0.0f, 0.41f, 0.5f, ear), 0.0f);      // just above threshold
}

// A sound outside the audible band contributes nothing even at point-blank range
// (pitch-sensitivity zero -> HeardLoudness 0). The audiogram is the gate.
TEST(PerceptionHardening, HeardLoudnessOutsideBandIsSilentAtZeroRange) {
    HearingProfile ear;
    ear.range = 20.0f;
    ear.peak_pitch = 0.2f;
    ear.bandwidth = 0.1f;
    EXPECT_FLOAT_EQ(HeardLoudness(0.0f, 1.0f, /*pitch=*/0.9f, ear), 0.0f);
}

// ===========================================================================
// PerceptionSystem: ECS fusion contract + determinism + gating
// ===========================================================================

// Gating no-op: a registry whose entities carry no Perception/Awareness components
// produces zero perceivers and changes nothing (canonical roster is untouched).
TEST(PerceptionSystemHardening, NoPerceiversIsGatingNoOp) {
    entt::registry r;
    MakeSensable(r, 5.0f, 0.0f, /*faction=*/2, /*loud=*/1.0f, /*pitch=*/0.5f); // sensable only
    const auto stats = RunPerceptionSystemOnTick(r, kDt);
    EXPECT_EQ(stats.perceivers, 0u);
    EXPECT_EQ(stats.sensing, 0u);
    EXPECT_EQ(stats.engaged, 0u);
}

// Same-faction targets are ignored (the consumer filters on faction != perceiver
// faction). A loud same-faction neighbor dead ahead must not raise awareness.
TEST(PerceptionSystemHardening, SameFactionNeverSensed) {
    entt::registry r;
    const auto p = MakePerceiver(r, 0, 0, 1, 0, /*faction=*/3);
    MakeSensable(r, 5.0f, 0.0f, /*faction=*/3, /*loud=*/1.0f, /*pitch=*/0.5f);
    for (int i = 0; i < 40; ++i) RunPerceptionSystemOnTick(r, kDt);
    const auto& aw = r.get<AwarenessComponent>(p).awareness;
    EXPECT_EQ(aw.state, AwarenessState::Unaware);
    EXPECT_FALSE(aw.has_last_known);
}

// A perceiver must not sense ITSELF even when it carries a SensableComponent of a
// different-looking faction field (the `s.id == e` self-skip must fire). Give the
// perceiver a Sensable too.
TEST(PerceptionSystemHardening, PerceiverDoesNotSenseItself) {
    entt::registry r;
    const auto p = MakePerceiver(r, 0, 0, 1, 0, /*faction=*/1);
    // Same entity is also loudly sensable; only OTHER entities should count.
    auto& s = r.emplace<SensableComponent>(p);
    s.faction = 2; // different from its perceiver faction
    s.noise_loudness = 1.0f;
    s.noise_pitch = 0.5f;
    auto& ear = r.get<PerceptionComponent>(p).ear;
    ear.range = 30.0f;
    for (int i = 0; i < 40; ++i) RunPerceptionSystemOnTick(r, kDt);
    EXPECT_EQ(r.get<AwarenessComponent>(p).awareness.state, AwarenessState::Unaware);
}

// last_known is recorded in WORLD coordinates of the sensed target (the producer
// feeds the awareness consumer a world point, not a relative offset). A predator
// engaging prey at world (10,4) must remember (10,4).
TEST(PerceptionSystemHardening, LastKnownIsWorldPositionOfTarget) {
    entt::registry r;
    const auto p = MakePerceiver(r, 2, 1, 1, 0, /*faction=*/1); // observer not at origin
    MakeSensable(r, 10.0f, 4.0f, /*faction=*/2, /*loud=*/0.0f, /*pitch=*/0.5f); // visible ahead
    for (int i = 0; i < 60; ++i) RunPerceptionSystemOnTick(r, kDt);
    const auto& aw = r.get<AwarenessComponent>(p).awareness;
    ASSERT_TRUE(aw.has_last_known);
    EXPECT_FLOAT_EQ(aw.last_known_x, 10.0f); // world X of the TARGET, not the delta
    EXPECT_FLOAT_EQ(aw.last_known_z, 4.0f);
}

// The strongest sense wins and the strongest TARGET is chosen. With two prey, the
// closer (stronger vision) one must be the remembered last-known.
TEST(PerceptionSystemHardening, StrongestTargetIsChosen) {
    entt::registry r;
    const auto p = MakePerceiver(r, 0, 0, 1, 0, /*faction=*/1);
    MakeSensable(r, 25.0f, 0.0f, /*faction=*/2, /*loud=*/0.0f, /*pitch=*/0.5f); // far, weak vision
    MakeSensable(r, 5.0f, 0.0f, /*faction=*/2, /*loud=*/0.0f, /*pitch=*/0.5f);  // near, strong vision
    for (int i = 0; i < 40; ++i) RunPerceptionSystemOnTick(r, kDt);
    const auto& aw = r.get<AwarenessComponent>(p).awareness;
    ASSERT_TRUE(aw.has_last_known);
    EXPECT_FLOAT_EQ(aw.last_known_x, 5.0f); // the nearer (stronger) target
}

// Awareness decays and FORGETS once the meter drains below suspicious: a target
// seen then removed must return the perceiver to Unaware and clear last-known.
TEST(PerceptionSystemHardening, AwarenessDecaysAndForgetsAfterTargetGone) {
    entt::registry r;
    const auto p = MakePerceiver(r, 0, 0, 1, 0, /*faction=*/1);
    const auto prey = MakeSensable(r, 5.0f, 0.0f, /*faction=*/2, /*loud=*/0.0f, /*pitch=*/0.5f);
    for (int i = 0; i < 40; ++i) RunPerceptionSystemOnTick(r, kDt);
    ASSERT_TRUE(r.get<AwarenessComponent>(p).awareness.has_last_known);
    r.destroy(prey); // target gone
    for (int i = 0; i < 400; ++i) RunPerceptionSystemOnTick(r, kDt);
    const auto& aw = r.get<AwarenessComponent>(p).awareness;
    EXPECT_EQ(aw.state, AwarenessState::Unaware);
    EXPECT_FALSE(aw.has_last_known); // memory cleared on full forget
}

// Order-independence: spawning the perceiver before vs after the prey must yield
// the identical awareness meter (id-ordered traversal is canonical; the system
// must not depend on creation order).
TEST(PerceptionSystemHardening, PerceiverResultIsSpawnOrderIndependent) {
    auto meterFor = [](bool perceiverFirst) {
        entt::registry r;
        entt::entity p;
        if (perceiverFirst) {
            p = MakePerceiver(r, 0, 0, 1, 0, /*faction=*/1);
            MakeSensable(r, 8.0f, 2.0f, /*faction=*/2, /*loud=*/0.5f, /*pitch=*/0.5f);
        } else {
            MakeSensable(r, 8.0f, 2.0f, /*faction=*/2, /*loud=*/0.5f, /*pitch=*/0.5f);
            p = MakePerceiver(r, 0, 0, 1, 0, /*faction=*/1);
        }
        auto& ear = r.get<PerceptionComponent>(p).ear;
        ear.range = 30.0f;
        for (int i = 0; i < 30; ++i) RunPerceptionSystemOnTick(r, kDt);
        return r.get<AwarenessComponent>(p).awareness.meter;
    };
    EXPECT_EQ(meterFor(true), meterFor(false));
}

// run==replay EXACT over many ticks with a populated roster (multiple perceivers,
// multiple sensables, mixed factions). Flatten meters to a vector and require
// bit-exact equality across two independent runs.
TEST(PerceptionSystemHardening, PopulatedRosterIsRunReplayExact) {
    auto run = []() {
        entt::registry r;
        std::vector<entt::entity> percs;
        percs.push_back(MakePerceiver(r, 0, 0, 1, 0, /*faction=*/1));
        percs.push_back(MakePerceiver(r, -5, 3, 0, 1, /*faction=*/1));
        percs.push_back(MakePerceiver(r, 8, -2, -1, 0, /*faction=*/4));
        MakeSensable(r, 6.0f, 1.0f, /*faction=*/2, /*loud=*/0.6f, /*pitch=*/0.5f);
        MakeSensable(r, -3.0f, 7.0f, /*faction=*/2, /*loud=*/0.3f, /*pitch=*/0.4f);
        MakeSensable(r, 10.0f, -1.0f, /*faction=*/2, /*loud=*/0.9f, /*pitch=*/0.6f);
        for (int i = 0; i < 50; ++i) RunPerceptionSystemOnTick(r, kDt);
        std::vector<float> out;
        for (auto e : percs) {
            const auto& aw = r.get<AwarenessComponent>(e).awareness;
            out.push_back(aw.meter);
            out.push_back(aw.last_known_x);
            out.push_back(aw.last_known_z);
        }
        return out;
    };
    const auto a = run();
    const auto b = run();
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_FLOAT_EQ(a[i], b[i]) << "slot " << i;
}

// A target precisely on top of the perceiver (coincident) is visible via the
// cone's coincident guard and drives awareness up — no NaN from the zero-distance
// normalize inside the system.
TEST(PerceptionSystemHardening, CoincidentTargetDrivesAwarenessNoNaN) {
    entt::registry r;
    const auto p = MakePerceiver(r, 3, 3, 1, 0, /*faction=*/1);
    MakeSensable(r, 3.0f, 3.0f, /*faction=*/2, /*loud=*/0.0f, /*pitch=*/0.5f); // exactly on top
    for (int i = 0; i < 30; ++i) RunPerceptionSystemOnTick(r, kDt);
    const auto& aw = r.get<AwarenessComponent>(p).awareness;
    EXPECT_FALSE(std::isnan(aw.meter));
    EXPECT_GT(aw.meter, 0.0f); // coincident -> vision clarity ~1 -> awareness rises
}
