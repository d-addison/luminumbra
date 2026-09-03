// Isolation/layer render mode: unit tests for the pure config parse
// (core/IsolationConfig.h is dependency-free, so this needs no GL/common libs).
// Pins the CLI -> layer-mask/backdrop mapping, the default-is-noop byte-stability
// guarantee, and unknown-token handling. This tests the gating logic directly
// without substituting a synthetic rendering path for the production path.

#include "gtest/gtest.h"

#include "core/IsolationConfig.h"

using namespace Luminumbra::Client::ScenarioHarness;
namespace L = Luminumbra::Client::ScenarioHarness::IsolationLayer;

TEST(IsolationConfig, DefaultIsNoop) {
    IsolationConfig cfg; // {All, Scene}
    EXPECT_EQ(cfg.layer_mask, L::All);
    EXPECT_EQ(cfg.backdrop, BackdropMode::Scene);
    EXPECT_FALSE(cfg.active()); // the no-op default keeps the render path byte-stable
    // ParseIsolationConfig("", "") must also be the no-op default.
    const IsolationConfig parsed = ParseIsolationConfig("", "");
    EXPECT_EQ(parsed.layer_mask, L::All);
    EXPECT_EQ(parsed.backdrop, BackdropMode::Scene);
    EXPECT_FALSE(parsed.active());
}

TEST(IsolationConfig, ParseSingleAndMultiLayer) {
    EXPECT_EQ(ParseIsolationLayers("foliage"), L::Foliage);
    EXPECT_EQ(ParseIsolationLayers("foliage,particles"), L::Foliage | L::Particles);
    EXPECT_EQ(ParseIsolationLayers("terrain,water,foliage"), L::Terrain | L::Water | L::Foliage);
    // case-insensitive + whitespace-tolerant + aliases
    EXPECT_EQ(ParseIsolationLayers(" Foliage , Particles "), L::Foliage | L::Particles);
    EXPECT_EQ(ParseIsolationLayers("grass"), L::Foliage);      // alias
    EXPECT_EQ(ParseIsolationLayers("far_field"), L::FarField); // alias
    EXPECT_EQ(ParseIsolationLayers("creatures"), L::Skinned);  // alias
}

TEST(IsolationConfig, EmptyAndUnknownTokensYieldNoIsolation) {
    EXPECT_EQ(ParseIsolationLayers(""), L::All);
    EXPECT_EQ(ParseIsolationLayers("   "), L::All);
    EXPECT_EQ(ParseIsolationLayers(","), L::All);
    // only-unknown tokens -> All (no valid selection -> render everything, NOT 0/nothing)
    EXPECT_EQ(ParseIsolationLayers("banana,xyzzy"), L::All);
    // a valid + an unknown -> just the valid bit (unknown ignored)
    EXPECT_EQ(ParseIsolationLayers("foliage,banana"), L::Foliage);
}

TEST(IsolationConfig, IsolatedConfigIsActive) {
    EXPECT_TRUE(ParseIsolationConfig("foliage", "").active());
    EXPECT_TRUE(ParseIsolationConfig("", "greenscreen").active()); // backdrop alone is isolation
    EXPECT_TRUE(ParseIsolationConfig("foliage", "void").active());
}

TEST(IsolationConfig, ParseBackdropModes) {
    EXPECT_EQ(ParseBackdropMode("void"), BackdropMode::Void);
    EXPECT_EQ(ParseBackdropMode("greenscreen"), BackdropMode::Greenscreen);
    EXPECT_EQ(ParseBackdropMode("green"), BackdropMode::Greenscreen); // alias
    EXPECT_EQ(ParseBackdropMode("checker"), BackdropMode::Checker);
    EXPECT_EQ(ParseBackdropMode("transparent"), BackdropMode::Transparent);
    EXPECT_EQ(ParseBackdropMode(" VOID "), BackdropMode::Void); // case/space
    EXPECT_EQ(ParseBackdropMode(""), BackdropMode::Scene);      // default
    EXPECT_EQ(ParseBackdropMode("scene"), BackdropMode::Scene);
    EXPECT_EQ(ParseBackdropMode("nonsense"), BackdropMode::Scene); // unknown -> default
}

TEST(IsolationConfig, RendersBitHelper) {
    const IsolationConfig only_foliage = ParseIsolationConfig("foliage", "void");
    EXPECT_TRUE(only_foliage.renders(L::Foliage));
    EXPECT_FALSE(only_foliage.renders(L::Terrain));
    EXPECT_FALSE(only_foliage.renders(L::Particles));
    IsolationConfig def;
    EXPECT_TRUE(def.renders(L::Terrain)); // default renders everything
    EXPECT_TRUE(def.renders(L::Foliage));
}

TEST(IsolationConfig, BackdropModeNameRoundTrips) {
    for (auto m : {BackdropMode::Scene,
                   BackdropMode::Void,
                   BackdropMode::Greenscreen,
                   BackdropMode::Checker,
                   BackdropMode::Transparent}) {
        EXPECT_EQ(ParseBackdropMode(BackdropModeName(m)), m);
    }
}
