// game.light_tools: a PURE, DETERMINISTIC light-QUALITY scorer for the
// photography game loop (photography de-risk: no render/camera/GL/entt/rng/wall-clock).
// These tests pin the rubric: a LOW sun scores higher golden_hour than harsh noon; a
// back-lit subject at a low sun scores high rim_light (and a high sun kills it);
// cloud cover raises softness and lowers contrast; ambient softens; every axis +
// total stay in [0,1]; and the scorer is a pure function (same scene -> same score,
// run==replay). ScoreLight operates on a plain value struct.
#include <gtest/gtest.h>

#include "game/LightTools.h"

namespace {

using luminumbra::game::LightScene;
using luminumbra::game::LightScore;
using luminumbra::game::ScoreLight;

// Build a scene with explicit fields (named for readability at call sites).
LightScene
makeScene(float elevation, float facing, float cloud, float ambient, float azimuth = 0.0f) {
    LightScene s;
    s.sun_elevation01 = elevation;
    s.subject_facing01 = facing;
    s.cloud_cover01 = cloud;
    s.ambient01 = ambient;
    s.sun_azimuth01 = azimuth;
    return s;
}

// ---- GOLDEN HOUR ----

// A low sun (near the horizon, dawn/dusk) scores higher golden_hour than the same
// scene at harsh overhead noon.
TEST(LightTools, LowSunBeatsNoonGoldenHour) {
    const LightScene low =
        makeScene(/*elev=*/0.02f, /*facing=*/0.0f, /*cloud=*/0.1f, /*ambient=*/0.2f);
    const LightScene noon =
        makeScene(/*elev=*/0.50f, /*facing=*/0.0f, /*cloud=*/0.1f, /*ambient=*/0.2f);
    EXPECT_GT(ScoreLight(low).golden_hour, ScoreLight(noon).golden_hour);
}

// Golden hour is monotonically non-increasing as the sun climbs from horizon to noon.
TEST(LightTools, GoldenHourMonotoneInElevation) {
    float prev = ScoreLight(makeScene(0.0f, 0.0f, 0.0f, 0.0f)).golden_hour;
    for (int i = 1; i <= 10; ++i) {
        const float elev = 0.05f * static_cast<float>(i); // 0.05 .. 0.50
        const float g = ScoreLight(makeScene(elev, 0.0f, 0.0f, 0.0f)).golden_hour;
        EXPECT_LE(g, prev + 1e-6f) << "elevation=" << elev;
        prev = g;
    }
}

// Heavy cloud cover mutes the warm golden glow vs a clear sky at the same low sun.
TEST(LightTools, CloudMutesGoldenHour) {
    const LightScene clear = makeScene(0.02f, 0.0f, /*cloud=*/0.0f, 0.2f);
    const LightScene overcast = makeScene(0.02f, 0.0f, /*cloud=*/1.0f, 0.2f);
    EXPECT_GT(ScoreLight(clear).golden_hour, ScoreLight(overcast).golden_hour);
}

// ---- RIM LIGHT ----

// A back-lit subject (high facing) at a LOW sun scores high rim_light; a front-lit
// subject in the same scene scores lower.
TEST(LightTools, BackLitLowSunScoresHighRim) {
    const LightScene back = makeScene(0.05f, /*facing=*/1.0f, 0.1f, 0.2f);
    const LightScene front = makeScene(0.05f, /*facing=*/0.0f, 0.1f, 0.2f);
    EXPECT_GT(ScoreLight(back).rim_light, ScoreLight(front).rim_light);
    // A grazing back-lit subject should read as a strong rim.
    EXPECT_GT(ScoreLight(back).rim_light, 0.5f);
}

// A high overhead sun produces no rim however the subject faces — the low-sun gate
// suppresses it.
TEST(LightTools, HighSunKillsRim) {
    const LightScene low = makeScene(0.05f, /*facing=*/1.0f, 0.1f, 0.2f);
    const LightScene high = makeScene(0.55f, /*facing=*/1.0f, 0.1f, 0.2f);
    EXPECT_GT(ScoreLight(low).rim_light, ScoreLight(high).rim_light);
}

// ---- SOFTNESS ----

// Cloud cover raises softness (overcast = giant softbox).
TEST(LightTools, CloudRaisesSoftness) {
    const LightScene clear = makeScene(0.3f, 0.0f, /*cloud=*/0.0f, 0.2f);
    const LightScene overcast = makeScene(0.3f, 0.0f, /*cloud=*/1.0f, 0.2f);
    EXPECT_GT(ScoreLight(overcast).softness, ScoreLight(clear).softness);
}

// Ambient fill also raises softness.
TEST(LightTools, AmbientRaisesSoftness) {
    const LightScene dim = makeScene(0.3f, 0.0f, 0.3f, /*ambient=*/0.0f);
    const LightScene filled = makeScene(0.3f, 0.0f, 0.3f, /*ambient=*/1.0f);
    EXPECT_GT(ScoreLight(filled).softness, ScoreLight(dim).softness);
}

// ---- CONTRAST ----

// Cloud cover LOWERS contrast (diffuse light flattens shadows).
TEST(LightTools, CloudLowersContrast) {
    const LightScene clear = makeScene(0.05f, 0.0f, /*cloud=*/0.0f, 0.1f);
    const LightScene overcast = makeScene(0.05f, 0.0f, /*cloud=*/1.0f, 0.1f);
    EXPECT_GT(ScoreLight(clear).contrast, ScoreLight(overcast).contrast);
}

// A clear-sky low sun gives higher contrast than a clear-sky high sun (raking
// shadows vs flat overhead light).
TEST(LightTools, LowSunRaisesContrast) {
    const LightScene low = makeScene(0.02f, 0.0f, 0.0f, 0.1f);
    const LightScene high = makeScene(0.50f, 0.0f, 0.0f, 0.1f);
    EXPECT_GT(ScoreLight(low).contrast, ScoreLight(high).contrast);
}

// High ambient fill washes contrast out.
TEST(LightTools, AmbientLowersContrast) {
    const LightScene crisp = makeScene(0.05f, 0.0f, 0.0f, /*ambient=*/0.0f);
    const LightScene flat = makeScene(0.05f, 0.0f, 0.0f, /*ambient=*/1.0f);
    EXPECT_GT(ScoreLight(crisp).contrast, ScoreLight(flat).contrast);
}

// ---- bounds ----

// Every axis and the total stay within [0,1] across a sweep of strong, ordinary,
// and degenerate (out-of-range) scenes.
TEST(LightTools, ScoresAreClampedToUnitRange) {
    const LightScene scenes[] = {
        makeScene(0.02f, 1.0f, 0.1f, 0.2f),  // golden, back-lit, crisp
        makeScene(0.50f, 0.5f, 0.5f, 0.5f),  // ordinary noon
        makeScene(1.0f, 1.0f, 1.0f, 1.0f),   // saturated
        makeScene(-0.5f, -1.0f, 2.0f, 5.0f), // out-of-range / degenerate
    };
    for (const auto& s : scenes) {
        const LightScore sc = ScoreLight(s);
        for (float v : {sc.golden_hour, sc.rim_light, sc.softness, sc.contrast, sc.total}) {
            EXPECT_GE(v, 0.0f);
            EXPECT_LE(v, 1.0f);
        }
    }
}

// A great-light scene (low golden sun, back-lit, clear) totals higher than a bland
// flat-light one (overhead, front-lit, hazy).
TEST(LightTools, GreatLightBeatsBlandTotal) {
    const LightScene great =
        makeScene(/*elev=*/0.03f, /*facing=*/0.9f, /*cloud=*/0.1f, /*ambient=*/0.2f);
    const LightScene bland =
        makeScene(/*elev=*/0.50f, /*facing=*/0.1f, /*cloud=*/0.6f, /*ambient=*/0.8f);
    EXPECT_GT(ScoreLight(great).total, ScoreLight(bland).total);
}

// ---- determinism: run == replay ----

// The same LightScene yields a BIT-IDENTICAL LightScore on every evaluation (pure,
// libm-free, no rng/wall-clock). This is the run==replay contract for the scorer.
TEST(LightTools, RunEqualsReplay) {
    const LightScene scene = makeScene(/*elev=*/0.137f,
                                       /*facing=*/0.642f,
                                       /*cloud=*/0.281f,
                                       /*ambient=*/0.413f,
                                       /*azimuth=*/0.75f);
    const LightScore a = ScoreLight(scene);
    const LightScore b = ScoreLight(scene);
    // Exact bit equality (==), not approximate: determinism, not tolerance.
    EXPECT_EQ(a.golden_hour, b.golden_hour);
    EXPECT_EQ(a.rim_light, b.rim_light);
    EXPECT_EQ(a.softness, b.softness);
    EXPECT_EQ(a.contrast, b.contrast);
    EXPECT_EQ(a.total, b.total);
}

} // namespace
