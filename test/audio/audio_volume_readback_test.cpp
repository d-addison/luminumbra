//  implementation note: CPU-only unit tests for the occlusion
// read-back math — MiniaudioManager::ComputeFinalVolume in
// src/luminumbra_client/audio/MiniaudioManager.h.
//
// AudioSpatialCluster now COMPUTES per-sound spatial attenuation including a
// physics-raycast occlusion term, and MiniaudioManager::Update reads it back
// onto each 3D voice's ma_sound volume so a sound blocked by geometry gets
// quieter. The volume fold is factored into ComputeFinalVolume(base, busGain,
// spatialAttenuation, occlusion01) — a pure static helper (no audio device, no
// miniaudio link needed to call it) — so the proving_signal ("more occlusion =>
// lower volume, monotonically; occlusion 0 => unchanged") can be pinned CPU-only.
//
// The helper is header-inline, so this TU includes MiniaudioManager.h (which
// transitively pulls in miniaudio.h in declaration-mode, nlohmann/json, glm) but
// links NOTHING from the audio subsystem — it never constructs a MiniaudioManager,
// only calls the static method. Runs in the default headless ctest lane.
//
// Contract covered:
//   * occlusion 0 => result == base*busGain*spatialAttenuation (unchanged)
//   * strictly DECREASING in occlusion (over a positive base*busGain*sa)
//   * result clamped to [0, base*busGain] for ALL inputs, incl. out-of-range
//     spatialAttenuation/occlusion and negative gains

#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>

#include "audio/MiniaudioManager.h"

namespace {

using Luminumbra::Client::MiniaudioManager;

constexpr float kEps = 1e-5f;

} // namespace

// occlusion 0 leaves the voice at base*busGain*spatialAttenuation — i.e. exactly
// the pre-read-back volume. spatialAttenuation is kept in [0,1] here (its live
// domain) so this and the clamp bound below are jointly consistent.
TEST(AudioVolumeReadback, OcclusionZeroEqualsBaseTimesBusTimesSpatial) {
    struct Case {
        float base, bus, sa;
    };
    const Case cases[] = {
        {1.0f, 1.0f, 1.0f},
        {0.5f, 0.8f, 0.75f},
        {0.9f, 0.6f, 1.0f},
        {0.25f, 1.0f, 0.0f}, // silent source stays silent
        {0.0f, 0.7f, 0.5f},  // zero base stays zero
    };
    for (const auto& c : cases) {
        const float expected = c.base * c.bus * c.sa;
        const float got = MiniaudioManager::ComputeFinalVolume(c.base, c.bus, c.sa, 0.0f);
        EXPECT_NEAR(got, expected, kEps)
            << "occlusion 0 must not change the volume (base=" << c.base << ", bus=" << c.bus
            << ", sa=" << c.sa << ")";
    }
}

// More occlusion => strictly quieter. Held over a strictly-positive
// base*busGain*spatialAttenuation so the sequence is genuinely decreasing rather
// than a flat run of zeros.
TEST(AudioVolumeReadback, StrictlyDecreasingInOcclusion) {
    const float base = 0.8f, bus = 0.6f, sa = 1.0f;
    const float occlusions[] = {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f};

    float prev = MiniaudioManager::ComputeFinalVolume(base, bus, sa, occlusions[0]);
    for (std::size_t i = 1; i < std::size(occlusions); ++i) {
        const float cur = MiniaudioManager::ComputeFinalVolume(base, bus, sa, occlusions[i]);
        EXPECT_LT(cur, prev) << "volume must drop as occlusion rises (occ " << occlusions[i - 1]
                             << " -> " << occlusions[i] << ": " << prev << " -> " << cur << ")";
        prev = cur;
    }

    //...and monotone (non-increasing) is the strongest weaker statement — also
    // assert the fully-occluded voice is still audible (never dead silent) and
    // strictly below the fully-clear one.
    const float clear = MiniaudioManager::ComputeFinalVolume(base, bus, sa, 0.0f);
    const float blocked = MiniaudioManager::ComputeFinalVolume(base, bus, sa, 1.0f);
    EXPECT_GT(blocked, 0.0f) << "occlusion must not fully silence a sound";
    EXPECT_LT(blocked, clear) << "fully blocked must be quieter than fully clear";
}

// The result is bounded to [0, base*busGain] for EVERY input, including
// out-of-range spatialAttenuation/occlusion and negative gains. base/bus are held
// non-negative wherever the upper bound is asserted (a negative ceiling would make
// the bound itself nonsensical); negatives are checked only for the >= 0 floor.
TEST(AudioVolumeReadback, ClampsToZeroBaseBusRange) {
    const float bases[] = {0.0f, 0.3f, 1.0f, 2.0f};
    const float buses[] = {0.0f, 0.5f, 1.0f};
    const float sas[] = {-1.0f, 0.0f, 0.5f, 1.0f, 5.0f};  // incl. out-of-range
    const float occs[] = {-2.0f, 0.0f, 0.3f, 1.0f, 3.0f}; // incl. out-of-range

    for (float base : bases)
        for (float bus : buses)
            for (float sa : sas)
                for (float occ : occs) {
                    const float v = MiniaudioManager::ComputeFinalVolume(base, bus, sa, occ);
                    const float ceiling = base * bus; // base,bus >= 0 here
                    EXPECT_GE(v, 0.0f) << "volume floor (base=" << base << ", bus=" << bus
                                       << ", sa=" << sa << ", occ=" << occ << ")";
                    EXPECT_LE(v, ceiling + kEps)
                        << "volume ceiling base*bus=" << ceiling << " (base=" << base
                        << ", bus=" << bus << ", sa=" << sa << ", occ=" << occ << ")";
                }
}

// Negative gains never produce a negative (or NaN-ish) volume — they floor at 0.
TEST(AudioVolumeReadback, NegativeGainsFloorAtZero) {
    EXPECT_FLOAT_EQ(MiniaudioManager::ComputeFinalVolume(-1.0f, 1.0f, 1.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(MiniaudioManager::ComputeFinalVolume(1.0f, -1.0f, 1.0f, 0.0f), 0.0f);
    EXPECT_GE(MiniaudioManager::ComputeFinalVolume(-0.5f, -0.5f, 0.5f, 0.5f), 0.0f);
}

// Out-of-range spatialAttenuation is clamped to 1.0, so it can never boost a voice
// above its base*busGain ceiling (occlusion 0).
TEST(AudioVolumeReadback, SpatialAttenuationClampedToUnity) {
    const float base = 0.5f, bus = 0.8f;
    const float ceiling = base * bus; // 0.4
    const float boosted = MiniaudioManager::ComputeFinalVolume(base, bus, 5.0f, 0.0f);
    EXPECT_NEAR(boosted, ceiling, kEps)
        << "spatialAttenuation > 1 must clamp to the base*bus ceiling, not amplify";
}
