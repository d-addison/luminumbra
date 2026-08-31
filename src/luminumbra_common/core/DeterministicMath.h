#pragma once

//  determinism contract: deterministic transcendental wrappers.
//
// PURPOSE. The lockstep world tick and replay require that
// simulation math produce BIT-IDENTICAL results across machines, compilers, and
// C runtime libraries. libm's sin/cos/atan2/etc. are NOT specified to the last
// ULP and differ between implementations (glibc vs musl vs MSVCRT vs MinGW) --
// Factorio shipped their own trig for exactly this reason (research Area 2,
// takeaway 6). These wrappers replace libm transcendentals in sim paths with
// implementations built from ONLY the IEEE-754 basic operations (+ - * / and a
// single hardware sqrt), which Bruce Dawson establishes ARE bit-stable under a
// fixed rounding mode on any IEEE platform.
//
// CONTRACT.
//   * The result of every function here is a pure function of its float bits.
//   * Implementations use float (binary32) intermediates only -- no double
//     widening, no libm calls (except the IEEE-deterministic sqrtf, see Sqrt).
//   * -ffp-contract=off (pinned on luminumbra_common in CMakeLists.txt) forbids
//     FMA contraction, so the multiply-add chains below evaluate term by term
//     identically everywhere.
//   * Accuracy is SECONDARY to reproducibility. These are minimax polynomial
//     approximations; max error is bounded (documented per function) and well
//     within sim tolerances, but they are NOT a precision-grade libm.
//   * Golden bit patterns for a sweep of inputs are locked by
//     test/common/DeterministicMath_test.cpp (ctest target
//     deterministic_math_test). Any change to a result bit pattern is a
//     contract break and must move those goldens deliberately.
//
// SCOPE / MIGRATION. This task LANDS the wrappers + the lint that bans raw libm
// transcendentals in sim code; it does NOT migrate existing call sites (that
// would churn the world_hash). New sim code (/13 onward) must call these;
// the SimDeterminismLint validator mode enforces it going forward. The only
// transcendental presently on a sim path is sqrt (WaterSystem / InstinctSystem /
// AnimationRuntime distance math, glm::distance in PhysicsSystem) -- sqrt is
// IEEE-deterministic so those sites are correct as-is and are documented in the
// progress log, not rewritten here.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace Luminumbra::DeterministicMath {

// ---------------------------------------------------------------------------
// Constants (binary32). Authored as the nearest float to the true value so the
// reduction arithmetic below is reproducible.
// ---------------------------------------------------------------------------
inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 6.28318530717958647692f;
inline constexpr float kHalfPi = 1.57079632679489661923f;
inline constexpr float kInvTwoPi = 0.15915494309189533577f; // 1 / (2*pi)

// ---------------------------------------------------------------------------
// Bit helpers (used by tests and by the float-domain branchless paths).
// ---------------------------------------------------------------------------
inline std::uint32_t BitsOf(float v) {
    std::uint32_t bits;
    static_assert(sizeof(bits) == sizeof(v), "float is not 32-bit");
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

inline float FromBits(std::uint32_t bits) {
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------
// Sqrt. IEEE-754 specifies square root as a CORRECTLY ROUNDED basic operation
// (like + - * /), so the hardware/std sqrtf result is identical on every IEEE
// platform under the default round-to-nearest mode. We therefore route through
// std::sqrt directly: re-implementing it would only RISK divergence, not remove
// it. Routing through this wrapper keeps sqrt out of the lint's libm ban while
// documenting WHY it is the one transcendental that is already deterministic.
inline float Sqrt(float x) {
    return std::sqrt(x);
}

// ---------------------------------------------------------------------------
// Range reduction for sin/cos. Reduce x into the symmetric interval
// [-pi, pi] using a float-domain k = round(x / (2*pi)); x - k*(2*pi). The
// subtraction is exact term-by-term under -ffp-contract=off. This is a single
// reduction step: it is accurate for the |x| <= a few thousand radians that sim
// code produces (angles, phases); it is NOT a Payne-Hanek reducer for huge
// arguments. Sim code that needs a bounded angle should pre-wrap to [0, 2*pi).
inline float ReduceToPi(float x) {
    // round-half-to-even via the standard "add/sub a big constant" trick would
    // be FP-mode sensitive; use nearbyint-free explicit rounding in float.
    const float k =
        static_cast<float>(static_cast<std::int64_t>(x * kInvTwoPi + (x >= 0.0f ? 0.5f : -0.5f)));
    return x - k * kTwoPi;
}

// Sine minimax polynomial valid on the QUARTER interval [-pi/2, pi/2], where a
// low-order odd polynomial is most accurate. Horner form; max abs error ~1e-6
// over [-pi/2, pi/2] (it is only ever evaluated there, after folding below).
// Exactly reproducible: fixed-order float multiply/adds under -ffp-contract=off.
inline float SinPoly(float x) {
    const float x2 = x * x;
    // s(x) = x * (s1 + x2*(s3 + x2*(s5 + x2*s7)))
    const float s1 = 0.99999660f;
    const float s3 = -0.16664824f;
    const float s5 = 0.00830629f;
    const float s7 = -0.00018363f;
    float p = s7;
    p = p * x2 + s5;
    p = p * x2 + s3;
    p = p * x2 + s1;
    return x * p;
}

// Cosine minimax polynomial valid on the QUARTER interval [-pi/2, pi/2]. Even
// Horner form; max abs error ~1.2e-6 over that interval.
inline float CosPoly(float x) {
    const float x2 = x * x;
    // c(x) = c0 + x2*(c2 + x2*(c4 + x2*c6))
    const float c0 = 0.99999440f;
    const float c2 = -0.49999040f;
    const float c4 = 0.04154732f;
    const float c6 = -0.00133926f;
    float p = c6;
    p = p * x2 + c4;
    p = p * x2 + c2;
    p = p * x2 + c0;
    return p;
}

// SinReduced: argument already in [-pi, pi]. Fold into [-pi/2, pi/2] using
// sin(r) = sin(pi - r) for r > pi/2 and sin(r) = -sin(-pi - r) for r < -pi/2,
// so the high-accuracy quarter-interval polynomial covers the whole period.
// All branches are exact float adds; the comparison selects the fold.
inline float SinReduced(float r) {
    if (r > kHalfPi) {
        return SinPoly(kPi - r);
    }
    if (r < -kHalfPi) {
        return SinPoly(-kPi - r);
    }
    return SinPoly(r);
}

inline float Sin(float x) {
    return SinReduced(ReduceToPi(x));
}

inline float Cos(float x) {
    // Reduce to [-pi, pi], then fold into [-pi/2, pi/2] using cos symmetry:
    // cos(r) = -cos(pi - r) for r > pi/2, cos(r) = -cos(pi + r) for r < -pi/2.
    // Keeps the cosine polynomial inside its high-accuracy quarter interval
    // (max abs error ~1.2e-6) instead of compounding error via a pi/2 shift.
    const float r = ReduceToPi(x);
    if (r > kHalfPi) {
        return -CosPoly(kPi - r);
    }
    if (r < -kHalfPi) {
        return -CosPoly(kPi + r);
    }
    return CosPoly(r);
}

// ---------------------------------------------------------------------------
// atan / atan2. atan on [-1,1] via a minimax odd polynomial; atan2 built from
// it with exact quadrant/branch selection (all comparisons + the / are IEEE
// basic ops). Max abs error of the core poly ~1.2e-4 rad. Built so atan2(0,0)
// returns 0 deterministically rather than an implementation-defined NaN/errno.
inline float AtanUnit(float x) {
    // |x| <= 1 assumed by callers below.
    const float x2 = x * x;
    // a(x) = x * (a1 + x2*(a3 + x2*(a5 + x2*a7)))
    const float a1 = 0.99997726f;
    const float a3 = -0.33262347f;
    const float a5 = 0.19354346f;
    const float a7 = -0.11643287f;
    const float a9 = 0.05265332f;
    const float a11 = -0.01172120f;
    float p = a11;
    p = p * x2 + a9;
    p = p * x2 + a7;
    p = p * x2 + a5;
    p = p * x2 + a3;
    p = p * x2 + a1;
    return x * p;
}

inline float Atan(float x) {
    // Fold |x| > 1 into the unit interval: atan(x) = pi/2 - atan(1/x).
    const float ax = x < 0.0f ? -x : x;
    if (ax <= 1.0f) {
        return AtanUnit(x);
    }
    const float r = kHalfPi - AtanUnit(1.0f / ax);
    return x < 0.0f ? -r : r;
}

inline float Atan2(float y, float x) {
    // Deterministic quadrant resolution. Each branch returns a value built from
    // Atan plus exact constant offsets; no libm, no errno, no signed-zero
    // surprises.
    if (x > 0.0f) {
        return Atan(y / x);
    }
    if (x < 0.0f) {
        if (y >= 0.0f) {
            return Atan(y / x) + kPi;
        }
        return Atan(y / x) - kPi;
    }
    // x == 0
    if (y > 0.0f) {
        return kHalfPi;
    }
    if (y < 0.0f) {
        return -kHalfPi;
    }
    return 0.0f; // atan2(0,0) defined as 0 for sim determinism.
}

} // namespace Luminumbra::DeterministicMath
