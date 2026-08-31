// CreatureProcgen: pure genome -> body-proportion build (the creature analogue
// of PlantProcgen). Pins the contract the procedural-creature silhouettes rely on:
// deterministic, bounded (no degenerate slivers/giants), distinct genes -> distinct
// builds, and the overall size multiplies every axis. No GL, no rng.
#include <gtest/gtest.h>

#include "systems/CreatureProcgen.h"

namespace {

using luminumbra::creature::ComputeCreatureBuild;
using luminumbra::creature::CreatureBuild;
using luminumbra::creature::CreatureBuildGenome;

TEST(CreatureProcgen, DeterministicAndBounded) {
    CreatureBuildGenome g;
    g.height = 0.7f;
    g.girth = 0.3f;
    g.length = 0.9f;
    g.size = 1.1f;
    const CreatureBuild a = ComputeCreatureBuild(g);
    const CreatureBuild b = ComputeCreatureBuild(g);
    EXPECT_FLOAT_EQ(a.scale_x, b.scale_x);
    EXPECT_FLOAT_EQ(a.scale_y, b.scale_y);
    EXPECT_FLOAT_EQ(a.scale_z, b.scale_z);

    // Even at gene extremes the build stays in a sane band (times the size multiplier).
    CreatureBuildGenome lo;
    lo.height = 0.0f;
    lo.girth = 0.0f;
    lo.length = 0.0f;
    lo.size = 1.0f;
    CreatureBuildGenome hi;
    hi.height = 1.0f;
    hi.girth = 1.0f;
    hi.length = 1.0f;
    hi.size = 1.0f;
    const CreatureBuild bl = ComputeCreatureBuild(lo);
    const CreatureBuild bh = ComputeCreatureBuild(hi);
    for (float v : {bl.scale_x, bl.scale_y, bl.scale_z, bh.scale_x, bh.scale_y, bh.scale_z}) {
        EXPECT_GT(v, 0.5f);
        EXPECT_LT(v, 1.6f);
    }
    // The tall/stocky/long genome is strictly bigger on every axis than the short/lean one.
    EXPECT_GT(bh.scale_x, bl.scale_x);
    EXPECT_GT(bh.scale_y, bl.scale_y);
    EXPECT_GT(bh.scale_z, bl.scale_z);
}

TEST(CreatureProcgen, AxesAreIndependent) {
    CreatureBuildGenome base;
    base.height = 0.5f;
    base.girth = 0.5f;
    base.length = 0.5f;
    CreatureBuildGenome tall = base;
    tall.height = 1.0f;
    CreatureBuildGenome wide = base;
    wide.girth = 1.0f;

    const CreatureBuild b0 = ComputeCreatureBuild(base);
    const CreatureBuild bt = ComputeCreatureBuild(tall);
    const CreatureBuild bw = ComputeCreatureBuild(wide);

    // Raising height changes Y only; raising girth changes X only.
    EXPECT_GT(bt.scale_y, b0.scale_y);
    EXPECT_FLOAT_EQ(bt.scale_x, b0.scale_x);
    EXPECT_FLOAT_EQ(bt.scale_z, b0.scale_z);
    EXPECT_GT(bw.scale_x, b0.scale_x);
    EXPECT_FLOAT_EQ(bw.scale_y, b0.scale_y);
}

TEST(CreatureProcgen, SizeMultipliesEveryAxis) {
    CreatureBuildGenome g;
    g.height = 0.5f;
    g.girth = 0.5f;
    g.length = 0.5f;
    g.size = 1.0f;
    const CreatureBuild b1 = ComputeCreatureBuild(g);
    g.size = 2.0f;
    const CreatureBuild b2 = ComputeCreatureBuild(g);
    EXPECT_FLOAT_EQ(b2.scale_x, b1.scale_x * 2.0f);
    EXPECT_FLOAT_EQ(b2.scale_y, b1.scale_y * 2.0f);
    EXPECT_FLOAT_EQ(b2.scale_z, b1.scale_z * 2.0f);

    // A non-positive size is treated as 1.0 (never zero/negative scale).
    g.size = 0.0f;
    const CreatureBuild b0 = ComputeCreatureBuild(g);
    EXPECT_FLOAT_EQ(b0.scale_x, b1.scale_x);
}

} // namespace
