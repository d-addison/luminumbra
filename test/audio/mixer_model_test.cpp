// CPU-only unit tests for the mixer/ducking math in
// src/luminumbra_client/audio/MixerModel.h. The header is dependency-free (no
// miniaudio, no audio device, no GL), so this runs headless in the default
// ctest lane. Covered contract:
//   - the duck envelope hits the floor after the attack time
//   - it recovers to exactly 1.0 after the release time
//   - it is monotone within each phase (non-increasing ducking, non-decreasing
//     releasing)
//   - re-triggering is idempotent and EXTENDS the duck (ref-counted voices;
//     overlapping events hold the floor until the last one ends)
//   - degenerate params snap instead of dividing by zero
//   - the music leg is a no-op at its default floor of 1.0

#include <gtest/gtest.h>

#include "audio/MixerModel.h"

namespace {

using Luminumbra::Client::Audio::DuckGain;
using Luminumbra::Client::Audio::DuckGainAt;
using Luminumbra::Client::Audio::DuckParams;
using Luminumbra::Client::Audio::MixerDucker;

constexpr float kAttack = 0.05f;
constexpr float kRelease = 0.80f;
constexpr float kFloor = 0.5f;

DuckParams TestParams() {
    DuckParams p;
    p.attack_seconds = kAttack;
    p.release_seconds = kRelease;
    p.floor_gain = kFloor;
    return p;
}

// Advance a ducker in fixed small steps totalling `seconds`, asserting the
// per-step monotonicity `dir` (-1 = non-increasing, +1 = non-decreasing).
float AdvanceChecked(MixerDucker& ducker, float seconds, int dir) {
    const float step = 0.004f; // ~250 Hz update, denser than any real frame rate
    float last = ducker.AmbientGain();
    for (float t = 0.0f; t < seconds; t += step) {
        ducker.Advance(step);
        const float g = ducker.AmbientGain();
        if (dir < 0) {
            EXPECT_LE(g, last + 1e-6f) << "gain rose while ducking at t=" << t;
        } else {
            EXPECT_GE(g, last - 1e-6f) << "gain fell while releasing at t=" << t;
        }
        EXPECT_GE(g, kFloor - 1e-6f);
        EXPECT_LE(g, 1.0f + 1e-6f);
        last = g;
    }
    return last;
}

// --- DuckGain (per-bus gain from the shared envelope) -----------------------

TEST(MixerModelDuckGain, EndpointsAndClamping) {
    EXPECT_FLOAT_EQ(DuckGain(0.0f, kFloor), 1.0f);
    EXPECT_FLOAT_EQ(DuckGain(1.0f, kFloor), kFloor);
    EXPECT_FLOAT_EQ(DuckGain(0.5f, kFloor), 1.0f - 0.5f * (1.0f - kFloor));
    // Out-of-range inputs clamp instead of extrapolating.
    EXPECT_FLOAT_EQ(DuckGain(-1.0f, kFloor), 1.0f);
    EXPECT_FLOAT_EQ(DuckGain(2.0f, kFloor), kFloor);
    EXPECT_FLOAT_EQ(DuckGain(1.0f, -0.5f), 0.0f);
    EXPECT_FLOAT_EQ(DuckGain(1.0f, 2.0f), 1.0f);
}

TEST(MixerModelDuckGain, FloorOfOneIsIdentity) {
    // The music bus default (floor 1.0) must be gain 1.0 at EVERY duck level —
    // this is what keeps the music path byte-identical until ducking music is
    // explicitly configured.
    for (float d = 0.0f; d <= 1.0f; d += 0.1f) {
        EXPECT_FLOAT_EQ(DuckGain(d, 1.0f), 1.0f);
    }
}

// --- DuckGainAt (closed-form single-trigger envelope) ------------------------

TEST(MixerModelClosedForm, HitsFloorExactlyAtAttack) {
    const float active = 1.0f; // event sound outlives the attack
    EXPECT_FLOAT_EQ(DuckGainAt(0.0f, active, kAttack, kRelease, kFloor), 1.0f);
    EXPECT_FLOAT_EQ(DuckGainAt(kAttack, active, kAttack, kRelease, kFloor), kFloor);
    // Holds the floor for as long as the event stays active.
    EXPECT_FLOAT_EQ(DuckGainAt(0.5f * active, active, kAttack, kRelease, kFloor), kFloor);
    EXPECT_FLOAT_EQ(DuckGainAt(active - 1e-4f, active, kAttack, kRelease, kFloor), kFloor);
}

TEST(MixerModelClosedForm, MonotoneDuringAttackAndRelease) {
    const float active = 0.3f;
    float last = 1.0f + 1e-6f;
    for (float t = 0.0f; t <= active; t += 0.002f) {
        const float g = DuckGainAt(t, active, kAttack, kRelease, kFloor);
        EXPECT_LE(g, last + 1e-6f) << "attack not monotone at t=" << t;
        last = g;
    }
    last = kFloor - 1e-6f;
    for (float t = active; t <= active + kRelease + 0.05f; t += 0.002f) {
        const float g = DuckGainAt(t, active, kAttack, kRelease, kFloor);
        EXPECT_GE(g, last - 1e-6f) << "release not monotone at t=" << t;
        last = g;
    }
}

TEST(MixerModelClosedForm, RecoversToUnityExactlyAtRelease) {
    const float active = 0.3f;
    EXPECT_LT(DuckGainAt(active + 0.5f * kRelease, active, kAttack, kRelease, kFloor), 1.0f);
    EXPECT_FLOAT_EQ(DuckGainAt(active + kRelease, active, kAttack, kRelease, kFloor), 1.0f);
    EXPECT_FLOAT_EQ(DuckGainAt(active + 10.0f, active, kAttack, kRelease, kFloor), 1.0f);
}

TEST(MixerModelClosedForm, ShortEventReleasesFromPartialDuck) {
    // The event ends mid-attack: the release starts from the PARTIAL duck level,
    // so recovery is proportionally faster than a full release.
    const float active = 0.5f * kAttack; // reached duck01 = 0.5
    const float at_end = DuckGainAt(active, active, kAttack, kRelease, kFloor);
    EXPECT_FLOAT_EQ(at_end, DuckGain(0.5f, kFloor));
    // Fully recovered after HALF the release time (duck01 0.5 -> 0).
    EXPECT_FLOAT_EQ(DuckGainAt(active + 0.5f * kRelease, active, kAttack, kRelease, kFloor), 1.0f);
}

TEST(MixerModelClosedForm, DegenerateParamsSnap) {
    // attack <= 0: instantly at the floor while active.
    EXPECT_FLOAT_EQ(DuckGainAt(1e-6f, 1.0f, 0.0f, kRelease, kFloor), kFloor);
    // release <= 0: instantly back at unity once inactive.
    EXPECT_FLOAT_EQ(DuckGainAt(0.3f + 1e-6f, 0.3f, kAttack, 0.0f, kFloor), 1.0f);
    // negative time clamps to the start.
    EXPECT_FLOAT_EQ(DuckGainAt(-5.0f, 1.0f, kAttack, kRelease, kFloor), 1.0f);
}

// --- MixerDucker (stateful, dt-stepped, ref-counted) --------------------------

TEST(MixerDuckerTest, StartsIdleAtUnity) {
    MixerDucker ducker(TestParams());
    EXPECT_TRUE(ducker.IsIdle());
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), 1.0f);
    ducker.Advance(1.0f); // idle advance stays at unity
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), 1.0f);
}

TEST(MixerDuckerTest, HitsFloorAfterAttackMonotonically) {
    MixerDucker ducker(TestParams());
    ducker.OnEventStart();
    const float g = AdvanceChecked(ducker, 2.0f * kAttack, /*dir=*/-1);
    EXPECT_FLOAT_EQ(g, kFloor);
    EXPECT_FLOAT_EQ(ducker.duck01(), 1.0f);
}

TEST(MixerDuckerTest, RecoversToUnityAfterReleaseMonotonically) {
    MixerDucker ducker(TestParams());
    ducker.OnEventStart();
    ducker.Advance(kAttack); // fully ducked
    ASSERT_FLOAT_EQ(ducker.AmbientGain(), kFloor);
    ducker.OnEventEnd();
    const float g = AdvanceChecked(ducker, kRelease + 0.1f, /*dir=*/+1);
    EXPECT_FLOAT_EQ(g, 1.0f);
    EXPECT_TRUE(ducker.IsIdle());
}

TEST(MixerDuckerTest, HoldsFloorWhileAnyEventActive) {
    MixerDucker ducker(TestParams());
    ducker.OnEventStart();
    ducker.Advance(kAttack);
    ASSERT_FLOAT_EQ(ducker.AmbientGain(), kFloor);
    // No matter how long the event plays, the duck holds (no drift, no decay).
    for (int i = 0; i < 100; ++i) {
        ducker.Advance(0.1f);
        EXPECT_FLOAT_EQ(ducker.AmbientGain(), kFloor);
    }
}

TEST(MixerDuckerTest, RetriggerIsIdempotentAndExtendsDuck) {
    MixerDucker ducker(TestParams());

    // Two overlapping events: ending ONE must not release the duck.
    ducker.OnEventStart();
    ducker.Advance(kAttack);
    ducker.OnEventStart(); // re-trigger while fully ducked: no state corruption
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), kFloor);
    EXPECT_EQ(ducker.active_events(), 2);

    ducker.OnEventEnd();
    ducker.Advance(kRelease); // one voice still active -> the duck holds
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), kFloor);

    // Last voice ends -> normal release.
    ducker.OnEventEnd();
    ducker.Advance(kRelease);
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), 1.0f);
}

TEST(MixerDuckerTest, RetriggerDuringReleaseDucksAgain) {
    MixerDucker ducker(TestParams());
    ducker.OnEventStart();
    ducker.Advance(kAttack);
    ducker.OnEventEnd();
    ducker.Advance(0.5f * kRelease); // partially recovered
    const float mid = ducker.AmbientGain();
    ASSERT_GT(mid, kFloor);
    ASSERT_LT(mid, 1.0f);

    // A new event mid-release ducks back down from wherever the envelope is —
    // and from a partial duck the floor is reached FASTER than a full attack.
    ducker.OnEventStart();
    ducker.Advance(kAttack);
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), kFloor);
}

TEST(MixerDuckerTest, EventEndUnderflowIsSafe) {
    MixerDucker ducker(TestParams());
    ducker.OnEventEnd(); // spurious end with no start: must not wedge the state
    EXPECT_EQ(ducker.active_events(), 0);
    ducker.OnEventStart();
    ducker.Advance(kAttack);
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), kFloor);
}

TEST(MixerDuckerTest, NegativeAndHugeDtAreSafe) {
    MixerDucker ducker(TestParams());
    ducker.OnEventStart();
    ducker.Advance(-1.0f); // negative dt: no-op
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), 1.0f);
    ducker.Advance(1000.0f); // huge dt: saturates at the floor, no overshoot
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), kFloor);
    ducker.OnEventEnd();
    ducker.Advance(1000.0f); // saturates at unity, no overshoot
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), 1.0f);
}

TEST(MixerDuckerTest, ResetClearsDuckAndVoices) {
    MixerDucker ducker(TestParams());
    ducker.OnEventStart();
    ducker.OnEventStart();
    ducker.Advance(kAttack);
    ASSERT_FLOAT_EQ(ducker.AmbientGain(), kFloor);
    ducker.Reset();
    EXPECT_TRUE(ducker.IsIdle());
    EXPECT_EQ(ducker.active_events(), 0);
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), 1.0f);
}

TEST(MixerDuckerTest, MusicLegDisabledByDefault) {
    // Default DuckParams: music floor 1.0 -> the music gain NEVER moves even
    // when the ambient leg is fully ducked (behaviour-preservation contract).
    MixerDucker ducker; // library defaults
    ducker.OnEventStart();
    ducker.Advance(10.0f);
    EXPECT_LT(ducker.AmbientGain(), 1.0f);
    EXPECT_FLOAT_EQ(ducker.MusicGain(), 1.0f);
}

TEST(MixerDuckerTest, MusicLegFollowsConfiguredFloor) {
    DuckParams p = TestParams();
    p.music_floor_gain = 0.7f;
    MixerDucker ducker(p);
    ducker.OnEventStart();
    ducker.Advance(kAttack);
    EXPECT_FLOAT_EQ(ducker.AmbientGain(), kFloor);
    EXPECT_FLOAT_EQ(ducker.MusicGain(), 0.7f);
}

TEST(MixerDuckerTest, SteppedMatchesClosedForm) {
    // The dt-stepped ducker must agree with the closed-form DuckGainAt spec on
    // a full attack -> hold -> release cycle (fixed-step integration of a linear
    // ramp is exact, so the tolerance only absorbs float accumulation).
    const float active = 0.2f;
    MixerDucker ducker(TestParams());
    ducker.OnEventStart();

    const float step = 0.001f;
    float t = 0.0f;
    bool ended = false;
    for (; t <= active + kRelease + 0.1f; t += step) {
        if (!ended && t >= active) {
            ducker.OnEventEnd();
            ended = true;
        }
        ducker.Advance(step);
        const float expected = DuckGainAt(t + step, active, kAttack, kRelease, kFloor);
        // Tolerance = one integration step of attack-rate gain change (the
        // fastest slope), covering phase-boundary off-by-one-step alignment.
        EXPECT_NEAR(ducker.AmbientGain(), expected, 0.02f) << "diverged at t=" << t;
    }
}

} // namespace
