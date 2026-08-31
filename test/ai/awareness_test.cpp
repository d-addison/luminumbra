// perception-fusion awareness state machine coverage — buildup, decay,
// searching after signal loss, last-known memory, threshold progression, purity.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/Awareness.h"

namespace {
using luminumbra::ai::Awareness;
using luminumbra::ai::AwarenessParams;
using luminumbra::ai::AwarenessState;
using luminumbra::ai::UpdateAwareness;

constexpr float kDt = 1.0f / 30.0f;
} // namespace

TEST(Awareness, StrongSignalEscalatesUnawareToEngaged) {
    AwarenessParams p;
    Awareness a;
    EXPECT_EQ(a.state, AwarenessState::Unaware);
    // Steady strong signal: should climb through the states to Engaged.
    bool saw_suspicious = false, saw_alert = false;
    for (int i = 0; i < 60; ++i) {
        a = UpdateAwareness(a, /*signal=*/1.0f, 7.0f, 3.0f, kDt, p);
        if (a.state == AwarenessState::Suspicious)
            saw_suspicious = true;
        if (a.state == AwarenessState::Alert)
            saw_alert = true;
    }
    EXPECT_TRUE(saw_suspicious);
    EXPECT_TRUE(saw_alert);
    EXPECT_EQ(a.state, AwarenessState::Engaged);
    EXPECT_TRUE(a.has_last_known);
    EXPECT_FLOAT_EQ(a.last_known_x, 7.0f);
    EXPECT_FLOAT_EQ(a.last_known_z, 3.0f);
}

TEST(Awareness, LosingSignalWhileAwareEntersSearchingThenForgets) {
    AwarenessParams p;
    Awareness a;
    // Build up to Engaged on a target at (5,5).
    for (int i = 0; i < 60; ++i)
        a = UpdateAwareness(a, 1.0f, 5.0f, 5.0f, kDt, p);
    ASSERT_EQ(a.state, AwarenessState::Engaged);

    // Signal lost: should drop to Searching (still aware, heading to last-known).
    a = UpdateAwareness(a, 0.0f, 0.0f, 0.0f, kDt, p);
    EXPECT_EQ(a.state, AwarenessState::Searching);
    EXPECT_TRUE(a.has_last_known);
    EXPECT_FLOAT_EQ(a.last_known_x, 5.0f); // remembers where the target was
    EXPECT_GT(a.time_since_signal, 0.0f);

    // Keep losing it: the meter decays through Suspicious to Unaware (forgets).
    for (int i = 0; i < 200; ++i)
        a = UpdateAwareness(a, 0.0f, 0.0f, 0.0f, kDt, p);
    EXPECT_EQ(a.state, AwarenessState::Unaware);
    EXPECT_FALSE(a.has_last_known);
    EXPECT_FLOAT_EQ(a.meter, 0.0f);
}

TEST(Awareness, MeterIsClampedToUnitInterval) {
    AwarenessParams p;
    Awareness a;
    for (int i = 0; i < 1000; ++i)
        a = UpdateAwareness(a, 5.0f, 1.0f, 1.0f, kDt, p);
    EXPECT_LE(a.meter, 1.0f);
    a = UpdateAwareness(a, 0.0f, 0.0f, 0.0f, 100.0f, p); // huge dt of decay
    EXPECT_GE(a.meter, 0.0f);
}

TEST(Awareness, FaintSignalReachesSuspiciousNotAlert) {
    AwarenessParams p;
    Awareness a;
    // Faint but steady signal: equilibrium meter = gain*sig/(... ) stays low.
    for (int i = 0; i < 100; ++i)
        a = UpdateAwareness(a, 0.05f, 2.0f, 2.0f, kDt, p);
    EXPECT_GE(a.meter, p.suspicious_at - 0.05f);
    EXPECT_LT(a.meter, p.alert_at); // never escalates to alert on a faint trace
    EXPECT_TRUE(a.state == AwarenessState::Suspicious || a.state == AwarenessState::Unaware);
}

TEST(Awareness, UpdateIsPure) {
    AwarenessParams p;
    Awareness base;
    base.meter = 0.4f;
    const Awareness r1 = UpdateAwareness(base, 0.3f, 1.0f, 2.0f, kDt, p);
    const Awareness r2 = UpdateAwareness(base, 0.3f, 1.0f, 2.0f, kDt, p);
    EXPECT_EQ(r1.meter, r2.meter);
    EXPECT_EQ(r1.state, r2.state);
    EXPECT_EQ(base.meter, 0.4f); // input unchanged
}
