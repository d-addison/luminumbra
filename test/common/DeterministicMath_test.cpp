//  determinism contract: golden bit-pattern lock for the deterministic
// transcendental wrappers (src/luminumbra_common/core/DeterministicMath.h).
//
// These wrappers exist so the lockstep world tick and replay
// produce bit-identical floating point across machines/compilers/
// libms. This test pins the EXACT IEEE binary32 bit pattern each function emits
// for a sweep of inputs. The expected values were generated from the
// implementation itself under -ffp-contract=off (the flag pinned on
// luminumbra_common in CMakeLists.txt) and committed as goldens: any change to
// a result bit pattern is a determinism-contract break and must move these
// constants DELIBERATELY in the same commit, exactly like the  pose checksum.
//
// The assertions compare raw uint32 bit patterns (not float ==) so a one-ULP
// drift -- the entire class of bug this contract guards against -- fails loudly
// instead of silently passing a tolerance check.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "luminumbra_common/core/DeterministicMath.h"

namespace dm = Luminumbra::DeterministicMath;

namespace {

float F(std::uint32_t bits) {
    return dm::FromBits(bits);
}
std::uint32_t B(float v) {
    return dm::BitsOf(v);
}

struct UnaryGolden {
    std::uint32_t in;
    std::uint32_t out;
};

struct BinaryGolden {
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t out;
};

// --- Sin golden sweep (input bits -> result bits) -------------------------
const UnaryGolden kSinGolden[] = {
    {0x00000000u, 0x00000000u}, // Sin(0)
    {0x3f000000u, 0x3ef5773fu}, // Sin(0.5)
    {0x3f800000u, 0x3f576aa5u}, // Sin(1)
    {0x3fc90fdau, 0x3f7ffff7u}, // Sin(pi/2)
    {0x40490fdbu, 0x00000000u}, // Sin(pi)
    {0xbf800000u, 0xbf576aa5u}, // Sin(-1)
    {0xc016cbe4u, 0xbf3504feu}, // Sin(-3pi/4)
    {0x40c90fdbu, 0x00000000u}, // Sin(2pi)
    {0x41200000u, 0xbf0b44f6u}, // Sin(10)
    {0xc1200000u, 0x3f0b44f6u}, // Sin(-10)
    {0x42c80000u, 0xbf01a156u}, // Sin(100)
    {0x3dfcd680u, 0x3dfc3201u}, // Sin(0.123456)
};

// --- Cos golden sweep -----------------------------------------------------
const UnaryGolden kCosGolden[] = {
    {0x00000000u, 0x3f7fffa2u}, // Cos(0)
    {0x3f000000u, 0x3f60a899u}, // Cos(0.5)
    {0x3f800000u, 0x3f0a4b56u}, // Cos(1)
    {0x3fc90fdau, 0xba60c800u}, // Cos(pi/2) ~ 0
    {0x40490fdbu, 0xbf7fffa2u}, // Cos(pi)
    {0xbf800000u, 0x3f0a4b56u}, // Cos(-1)
    {0xc016cbe4u, 0xbf350286u}, // Cos(-3pi/4)
    {0x40c90fdbu, 0x3f7fffa2u}, // Cos(2pi)
    {0x41200000u, 0xbf56cc7cu}, // Cos(10)
    {0xc1200000u, 0xbf56cc7cu}, // Cos(-10) == Cos(10) (even)
    {0x42c80000u, 0x3f5cc016u}, // Cos(100)
    {0x3dfcd680u, 0x3f7e0cd8u}, // Cos(0.123456)
};

// --- Sqrt golden sweep (IEEE-correctly-rounded; perfect squares exact) ----
const UnaryGolden kSqrtGolden[] = {
    {0x00000000u, 0x00000000u}, // Sqrt(0)
    {0x3f800000u, 0x3f800000u}, // Sqrt(1) = 1
    {0x40000000u, 0x3fb504f3u}, // Sqrt(2)
    {0x40800000u, 0x40000000u}, // Sqrt(4) = 2
    {0x3e800000u, 0x3f000000u}, // Sqrt(0.25) = 0.5
    {0x42c80000u, 0x41200000u}, // Sqrt(100) = 10
    {0x40200000u, 0x3fca62c2u}, // Sqrt(2.5)
    {0x358637bdu, 0x3a83126fu}, // Sqrt(1e-6)
    {0x47f12000u, 0x43afae79u}, // Sqrt(123456)
};

// --- Atan2 golden sweep (y bits, x bits -> result bits) -------------------
const BinaryGolden kAtan2Golden[] = {
    {0x00000000u, 0x00000000u, 0x00000000u}, // Atan2(0,0) = 0 (defined)
    {0x3f800000u, 0x00000000u, 0x3fc90fdbu}, // Atan2(1,0) = pi/2
    {0x00000000u, 0x3f800000u, 0x00000000u}, // Atan2(0,1) = 0
    {0xbf800000u, 0x00000000u, 0xbfc90fdbu}, // Atan2(-1,0) = -pi/2
    {0x00000000u, 0xbf800000u, 0x40490fdbu}, // Atan2(0,-1) = pi
    {0x3f800000u, 0x3f800000u, 0x3f490fbeu}, // Atan2(1,1)
    {0xbf800000u, 0x3f800000u, 0xbf490fbeu}, // Atan2(-1,1)
    {0x3f800000u, 0xbf800000u, 0x4016cbecu}, // Atan2(1,-1)
    {0xbf800000u, 0xbf800000u, 0xc016cbecu}, // Atan2(-1,-1)
    {0x40400000u, 0x40800000u, 0x3f24bc97u}, // Atan2(3,4)
    {0xc0400000u, 0x40800000u, 0xbf24bc97u}, // Atan2(-3,4)
    {0x3f000000u, 0x40000000u, 0x3e7adbd1u}, // Atan2(0.5,2)
};

TEST(DeterministicMath, SinGoldenBitPatterns) {
    for (const auto& g : kSinGolden) {
        const float x = F(g.in);
        EXPECT_EQ(B(dm::Sin(x)), g.out) << "Sin(" << x << ") bit pattern drifted from golden";
    }
}

TEST(DeterministicMath, CosGoldenBitPatterns) {
    for (const auto& g : kCosGolden) {
        const float x = F(g.in);
        EXPECT_EQ(B(dm::Cos(x)), g.out) << "Cos(" << x << ") bit pattern drifted from golden";
    }
}

TEST(DeterministicMath, SqrtGoldenBitPatterns) {
    for (const auto& g : kSqrtGolden) {
        const float x = F(g.in);
        EXPECT_EQ(B(dm::Sqrt(x)), g.out) << "Sqrt(" << x << ") bit pattern drifted from golden";
    }
}

TEST(DeterministicMath, Atan2GoldenBitPatterns) {
    for (const auto& g : kAtan2Golden) {
        const float y = F(g.a);
        const float x = F(g.b);
        EXPECT_EQ(B(dm::Atan2(y, x)), g.out)
            << "Atan2(" << y << "," << x << ") bit pattern drifted from golden";
    }
}

// Accuracy guard: the wrappers are reproducibility-first, but must still be
// close enough to libm that sim behavior is sane. These bounds are LOOSE (they
// only catch a broken polynomial, not ULP drift -- the golden tests own that).
TEST(DeterministicMath, ApproximatesLibmWithinBound) {
    // Bounds are LOOSE BY DESIGN (2e-3). The contract is reproducibility, NOT
    // precision. With quarter-interval folding the sine path holds <1e-6 abs
    // error; the cosine fold peaks near ~8.6e-4. This guard only catches a
    // FUNDAMENTALLY broken polynomial; ULP-exact reproducibility -- what the
    // lockstep oracle relies on -- is owned by the golden tests above.
    for (float x = -12.0f; x <= 12.0f; x += 0.013f) {
        EXPECT_NEAR(dm::Sin(x), std::sin(x), 2.0e-3f) << "x=" << x;
        EXPECT_NEAR(dm::Cos(x), std::cos(x), 2.0e-3f) << "x=" << x;
    }
    for (float y = -4.0f; y <= 4.0f; y += 0.31f) {
        for (float x = -4.0f; x <= 4.0f; x += 0.31f) {
            if (x == 0.0f && y == 0.0f)
                continue;
            EXPECT_NEAR(dm::Atan2(y, x), std::atan2(y, x), 2.0e-3f) << "y=" << y << " x=" << x;
        }
    }
    for (float x = 0.0f; x <= 1000.0f; x += 3.7f) {
        EXPECT_NEAR(dm::Sqrt(x), std::sqrt(x), 1.0e-3f) << "x=" << x;
    }
}

// Determinism guard: calling twice yields identical bits (no hidden state, no
// errno-path, no signed-zero surprise).
TEST(DeterministicMath, Idempotent) {
    for (float x = -10.0f; x <= 10.0f; x += 0.137f) {
        EXPECT_EQ(B(dm::Sin(x)), B(dm::Sin(x)));
        EXPECT_EQ(B(dm::Cos(x)), B(dm::Cos(x)));
        EXPECT_EQ(B(dm::Sqrt(x < 0 ? -x : x)), B(dm::Sqrt(x < 0 ? -x : x)));
        EXPECT_EQ(B(dm::Atan2(x, 1.0f)), B(dm::Atan2(x, 1.0f)));
    }
}

} // namespace
