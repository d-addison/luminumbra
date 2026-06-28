// Spec 018 FR-A + FR-B — locks the two-worlds residency / availability-set contract
// (src/luminumbra_common/core/ResidencyContract.h) so it stops being a dormant header:
// this TU includes it (compiling its static_asserts into the build) and pins the
// behavioural invariants Spec 017-B must implement against. Behaviour-neutral: it asserts
// the contract's shape only — no system is wired here.

#include "core/ResidencyContract.h"

#include <gtest/gtest.h>

namespace {

using luminumbra::core::AvailabilityKey;
using luminumbra::core::IsResidentFn;
using luminumbra::core::MayFeedWorldHash;
using luminumbra::core::RenderResidency;
using luminumbra::core::ResidencyClass;
using luminumbra::core::SimResidency;

// FR-A-002 — only Sim residency may feed world_hash; Render never may.
TEST(ResidencyContract, OnlySimMayFeedWorldHash) {
    EXPECT_TRUE(MayFeedWorldHash(ResidencyClass::Sim));
    EXPECT_FALSE(MayFeedWorldHash(ResidencyClass::Render));
    // The tag types carry the same fact at compile time.
    EXPECT_EQ(SimResidency::kClass, ResidencyClass::Sim);
    EXPECT_EQ(RenderResidency::kClass, ResidencyClass::Render);
    EXPECT_TRUE(MayFeedWorldHash(SimResidency::kClass));
    EXPECT_FALSE(MayFeedWorldHash(RenderResidency::kClass));
}

// FR-B-001 — the AvailabilityKey default is the byte-stable empty baseline: an
// unconfigured world leaves config_sub_hash at the 0 sentinel and two fresh keys
// compare equal (so membership is reproducible across runs).
TEST(ResidencyContract, DefaultKeyIsEmptyBaselineAndStable) {
    constexpr AvailabilityKey a{};
    constexpr AvailabilityKey b{};
    EXPECT_EQ(a.config_sub_hash, 0u) << "all-sim-default config must leave the empty sentinel";
    EXPECT_EQ(a.tick, 0);
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

// FR-B-001 — AvailabilityKey identity is exactly (seed, preset, config, tick, chunk xyz).
// Perturbing ANY field must break equality, or the activation queue could alias two
// distinct tick/position states onto one availability decision.
TEST(ResidencyContract, EveryFieldParticipatesInIdentity) {
    const AvailabilityKey base{};

    auto differs = [&](AvailabilityKey k) {
        EXPECT_TRUE(base != k);
        EXPECT_FALSE(base == k);
    };

    { AvailabilityKey k = base; k.seed = 1;            differs(k); }
    { AvailabilityKey k = base; k.preset_id = 1;       differs(k); }
    { AvailabilityKey k = base; k.config_sub_hash = 1; differs(k); }
    { AvailabilityKey k = base; k.tick = 1;            differs(k); }
    { AvailabilityKey k = base; k.chunk_x = 1;         differs(k); }
    { AvailabilityKey k = base; k.chunk_y = 1;         differs(k); }
    { AvailabilityKey k = base; k.chunk_z = 1;         differs(k); }

    // Two independently-built keys with identical fields are equal (no hidden state).
    AvailabilityKey lhs{}, rhs{};
    lhs.seed = rhs.seed = 7;
    lhs.preset_id = rhs.preset_id = 3;
    lhs.config_sub_hash = rhs.config_sub_hash = 42;
    lhs.tick = rhs.tick = 99;
    lhs.chunk_x = rhs.chunk_x = -4;
    lhs.chunk_y = rhs.chunk_y = 2;
    lhs.chunk_z = rhs.chunk_z = -8;
    EXPECT_TRUE(lhs == rhs);
}

// FR-B-002 — membership is a PURE function of AvailabilityKey. The contract fixes the
// SHAPE (IsResidentFn) here; Spec 017-B supplies the implementation. This pins the
// signature so a consumer cannot smuggle wall-clock / scheduling inputs through it.
TEST(ResidencyContract, IsResidentSignatureIsPureOfKey) {
    IsResidentFn fn = [](const AvailabilityKey& key) -> bool {
        // A trivially pure stand-in: residency keyed only on the deterministic tick.
        return key.tick >= 0;
    };
    AvailabilityKey k{};
    k.tick = 5;
    EXPECT_TRUE(fn(k));
    k.tick = -1;
    EXPECT_FALSE(fn(k));
}

}  // namespace
