//  (, -7): StimulusChannel::Aether -- the composite
// energy environment reaching the instinct planner. Asserts:
//   * APPEND-ONLY REGISTRY: Aether landed as slot 5 and kStimulusChannelCount
//     moved to 6 WITHOUT touching slots 0..4 (names + values pinned here so a
//     reorder/reuse regression fails loudly).
//   * NEUTRAL WHEN UNSET: no caller-supplied aether_level (< 0 sentinel) =>
//     the channel samples EXACTLY 0.0 at every tick -- aether has NO
//     tick-derived fallback (it is WORLD state: seed+anchor ambience or
//     gameplay deposits, never a function of the tick alone), so "no sample
//     supplied" means "no energy".
//   * CLAMPED PASSTHROUGH: a supplied level is returned bit-exactly inside
//     [0, 1] and clamped outside it (a saturated stateful cell normalized past
//     1.0 stays a bounded stimulus).
//   * PURE: same context => bit-identical samples across repeated calls and
//     across registry instances (run==replay for the channel itself).
// The canonical roster subscribes to nothing, so none of this perturbs
// world_hash (the lazy-registry INERT property stimulus_channel_gate_test
// pins); this file only exercises the channel's pure-function surface.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/StimulusChannels.h"

#include <cstdint>

namespace {

using luminumbra::ai::kStimulusChannelCount;
using luminumbra::ai::StimulusChannel;
using luminumbra::ai::StimulusChannelName;
using luminumbra::ai::StimulusChannelRegistry;
using luminumbra::ai::StimulusContext;

} // namespace

// --- Append-only registry shape: slot 5, count 6, prior names untouched. ---
TEST(AetherStimulusDeterminism, ChannelNameAndCountAppendOnly) {
    EXPECT_EQ(kStimulusChannelCount, 6);
    EXPECT_EQ(static_cast<std::uint8_t>(StimulusChannel::Aether), 5u);
    EXPECT_STREQ(StimulusChannelName(StimulusChannel::Aether), "aether");
    // Slots 0..4 are pinned (append-only discipline: never reorder/reuse).
    EXPECT_STREQ(StimulusChannelName(StimulusChannel::Weather), "weather");
    EXPECT_STREQ(StimulusChannelName(StimulusChannel::Temperature), "temperature");
    EXPECT_STREQ(StimulusChannelName(StimulusChannel::TimeOfDay), "time_of_day");
    EXPECT_STREQ(StimulusChannelName(StimulusChannel::Season), "season");
    EXPECT_STREQ(StimulusChannelName(StimulusChannel::LightLevel), "light_level");
}

// --- Unset level -> the deterministic neutral 0 (no tick-derived fallback). ---
TEST(AetherStimulusDeterminism, UnsetLevelSamplesNeutralZero) {
    StimulusContext ctx; // aether_level left at the -1 sentinel
    ctx.tick = 4321;     // a non-trivial tick: MUST NOT leak into the channel
    const StimulusChannelRegistry reg(ctx);
    EXPECT_EQ(reg.Sample(StimulusChannel::Aether), 0.0f);

    // Unlike LightLevel/TimeOfDay there is deliberately no day-curve fallback:
    // a different tick yields the SAME neutral 0 (aether is world state, not
    // tick state -- see the aether_level contract in StimulusChannels.h).
    StimulusContext midnight;
    midnight.tick = 0;
    const StimulusChannelRegistry reg0(midnight);
    EXPECT_EQ(reg0.Sample(StimulusChannel::Aether), 0.0f);
}

// --- Supplied level -> bit-exact passthrough inside [0,1], clamped outside. ---
TEST(AetherStimulusDeterminism, SetLevelIsClampedPassthrough) {
    const auto sample_at = [](float level) {
        StimulusContext ctx;
        ctx.tick = 1800; // fixed tick: irrelevant to the channel by contract
        ctx.aether_level = level;
        const StimulusChannelRegistry reg(ctx);
        return reg.Sample(StimulusChannel::Aether);
    };

    // In-range levels pass through bit-exactly (==, not approximate).
    EXPECT_EQ(sample_at(0.0f), 0.0f); // an explicit zero is a valid sample
    EXPECT_EQ(sample_at(0.37f), 0.37f);
    EXPECT_EQ(sample_at(1.0f), 1.0f);
    // Over-unity (a saturated stateful cell normalized past full scale) clamps.
    EXPECT_EQ(sample_at(2.5f), 1.0f);
}

// --- Purity: same context => same value, every call and every instance. ---
TEST(AetherStimulusDeterminism, PureSameContextSameValue) {
    StimulusContext ctx;
    ctx.tick = 2718;
    ctx.aether_level = 0.625f; // exactly representable: the equality is bit-exact
    const StimulusChannelRegistry a(ctx);
    const StimulusChannelRegistry b(ctx);

    const float first = a.Sample(StimulusChannel::Aether);
    EXPECT_EQ(first, 0.625f);
    for (int i = 0; i < 8; ++i) {
        // Sampling never mutates registry/context state, so every re-sample of
        // either instance reproduces the first value bit-exactly.
        EXPECT_EQ(a.Sample(StimulusChannel::Aether), first);
        EXPECT_EQ(b.Sample(StimulusChannel::Aether), first);
    }
}
