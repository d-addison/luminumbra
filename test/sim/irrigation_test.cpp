// sim.irrigation: DETERMINISTIC soil-MOISTURE / water-diffusion grid that
// feeds plant moisture. A WaterSourceComponent (dug channel / spring) deposits
// moisture into its cell; the grid DIFFUSES it to neighbours and DRAINS every
// cell toward a dry baseline each tick. Deterministic: integer fixed-point store,
// id-ordered traversal, per-cell deposit summed (order-independent), out-of-place
// integer diffusion; NO rng / wall-clock / libm. Gated by WaterSourceComponent: a
// registry with none leaves an at-baseline grid untouched (no-op), so the
// canonical NetworkStateHash baseline stays byte-identical.
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include <entt/entt.hpp>

#include "components/CoreComponents.h"
#include "components/IrrigationComponents.h"
#include "systems/IrrigationSystem.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::foliage::IrrigationGrid;
using luminumbra::foliage::RunIrrigationOnTick;
using luminumbra::foliage::MoistureAt;
using luminumbra::foliage::kMoistureBaseline;
using luminumbra::foliage::kMoistureMax;
using luminumbra::foliage::kMoistureDrainPerTick;

// Grid anchored at the origin with 1m cells, so world (x,z) maps directly to cell
// (floor(x), floor(z)).
constexpr float kOX = 0.0f;
constexpr float kOZ = 0.0f;
constexpr float kCell = 1.0f;

// Spawn a water source at (x,z) with a given per-tick output strength.
entt::entity spawnSource(entt::registry& r, float x, float z, std::uint16_t output = 1000) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    auto& s = r.emplace<Comp::WaterSourceComponent>(e);
    s.output = output;
    return e;
}

// ---- gating: empty roster / no opt-in ----

// No sources at all -> an at-baseline grid stays exactly at baseline (true no-op).
TEST(Irrigation, EmptyRosterStaysBaseline) {
    entt::registry r;
    IrrigationGrid grid(8, 8); // initialized dry (baseline)
    const auto stats = RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);
    EXPECT_EQ(stats.sources, 0);
    EXPECT_EQ(stats.deposited, 0);
    EXPECT_EQ(stats.drained, 0);
    EXPECT_TRUE(grid.all_at(kMoistureBaseline))
        << "no water sources => no deposit; an at-baseline grid stays at baseline";
}

// A transform-only entity WITHOUT WaterSourceComponent is ignored entirely.
TEST(Irrigation, EntityWithoutSourceIsIgnored) {
    entt::registry r;
    IrrigationGrid grid(8, 8);
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = 2.0f;
    tf.position.z = 2.0f;
    const auto stats = RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);
    EXPECT_EQ(stats.sources, 0) << "a non-source entity is not a participant";
    EXPECT_EQ(stats.deposited, 0);
    EXPECT_TRUE(grid.all_at(kMoistureBaseline));
}

// ---- core: a source raises local moisture ----

TEST(Irrigation, SourceRaisesLocalMoisture) {
    entt::registry r;
    IrrigationGrid grid(8, 8);
    spawnSource(r, 4.0f, 4.0f, 2000);

    EXPECT_EQ(MoistureAt(grid, 4.0f, 4.0f, kOX, kOZ, kCell), kMoistureBaseline);

    const auto stats = RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);
    EXPECT_EQ(stats.sources, 1);
    EXPECT_GT(stats.deposited, 0);

    EXPECT_GT(MoistureAt(grid, 4.0f, 4.0f, kOX, kOZ, kCell), kMoistureBaseline)
        << "the source's cell is wetter than the dry baseline";
}

// Two springs on one cell wet it more than a single spring of the same output.
TEST(Irrigation, MoreSourcesWetCellMore) {
    entt::registry oneReg;
    IrrigationGrid oneGrid(4, 4);
    spawnSource(oneReg, 1.0f, 1.0f, 1000);
    RunIrrigationOnTick(oneReg, oneGrid, kOX, kOZ, kCell);
    const std::int32_t one = MoistureAt(oneGrid, 1.0f, 1.0f, kOX, kOZ, kCell);

    entt::registry manyReg;
    IrrigationGrid manyGrid(4, 4);
    spawnSource(manyReg, 1.0f, 1.0f, 1000);
    spawnSource(manyReg, 1.2f, 1.3f, 1000); // same cell (1,1)
    RunIrrigationOnTick(manyReg, manyGrid, kOX, kOZ, kCell);
    const std::int32_t many = MoistureAt(manyGrid, 1.0f, 1.0f, kOX, kOZ, kCell);

    EXPECT_GT(many, one) << "two springs on one cell deposit (and retain) more";
}

// Deposit is order-INDEPENDENT: summing per-cell means the final level does not
// depend on source id ordering (two sources, same cell, same total).
TEST(Irrigation, DepositOrderIndependent) {
    auto run = [](bool reversed) {
        entt::registry r;
        IrrigationGrid grid(4, 4);
        if (!reversed) {
            spawnSource(r, 1.0f, 1.0f, 700);
            spawnSource(r, 1.5f, 1.5f, 1300);
        } else {
            spawnSource(r, 1.5f, 1.5f, 1300);
            spawnSource(r, 1.0f, 1.0f, 700);
        }
        RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);
        return MoistureAt(grid, 1.0f, 1.0f, kOX, kOZ, kCell);
    };
    EXPECT_EQ(run(false), run(true));
}

// ---- diffusion: moisture spreads to neighbours ----

TEST(Irrigation, MoistureDiffusesToNeighbours) {
    entt::registry r;
    IrrigationGrid grid(8, 8);
    spawnSource(r, 4.0f, 4.0f, 4000); // strong source so excess exceeds the share denom

    // A neighbour starts dry; after a few ticks the spreading wets it.
    EXPECT_EQ(MoistureAt(grid, 5.0f, 4.0f, kOX, kOZ, kCell), kMoistureBaseline);
    for (int t = 0; t < 4; ++t) RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);

    EXPECT_GT(MoistureAt(grid, 5.0f, 4.0f, kOX, kOZ, kCell), kMoistureBaseline)
        << "moisture migrated from the source cell into an adjacent cell";
    EXPECT_GT(MoistureAt(grid, 4.0f, 5.0f, kOX, kOZ, kCell), kMoistureBaseline);
}

// ---- drain: fallow cells relax toward baseline ----

// A wet cell with NO source drains back toward the dry baseline over ticks.
TEST(Irrigation, FallowCellDrainsTowardBaseline) {
    entt::registry r; // empty: pure drain (no source feeding)
    IrrigationGrid grid(4, 4);
    grid.SetCell(2, 2, 2000); // a puddle, no neighbours wet
    const std::int32_t start = grid.AtCell(2, 2);

    std::int32_t prev = start;
    for (int t = 0; t < 50; ++t) {
        RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);
        const std::int32_t now = grid.AtCell(2, 2);
        EXPECT_LE(now, prev) << "an unfed wet cell monotonically dries (drain + spread out)";
        prev = now;
    }
    EXPECT_LT(prev, start) << "the cell dried toward baseline";
    EXPECT_GE(prev, kMoistureBaseline) << "drain never undershoots the dry baseline";
}

// Over enough ticks an unfed grid returns ALL the way to the dry baseline.
TEST(Irrigation, FullDryDownToBaseline) {
    entt::registry r;
    IrrigationGrid grid(2, 2);
    grid.Reset(1500);
    // drain step per tick + diffusion redistribution; plenty of headroom.
    const int ticks = (1500 / kMoistureDrainPerTick) + 16;
    for (int t = 0; t < ticks; ++t) RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);
    EXPECT_TRUE(grid.all_at(kMoistureBaseline))
        << "with no source, every cell drains fully to the dry baseline";
}

// ---- MoistureAt query stability + bounds ----

TEST(Irrigation, MoistureAtIsStableAndBounded) {
    entt::registry r;
    IrrigationGrid grid(8, 8);
    spawnSource(r, 4.0f, 4.0f, 3000);
    for (int t = 0; t < 6; ++t) RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);

    // Repeated queries return the same value (pure read, no mutation).
    const std::int32_t a = MoistureAt(grid, 4.0f, 4.0f, kOX, kOZ, kCell);
    const std::int32_t b = MoistureAt(grid, 4.0f, 4.0f, kOX, kOZ, kCell);
    EXPECT_EQ(a, b);
    // Out-of-grid query is a defined baseline (not UB).
    EXPECT_EQ(MoistureAt(grid, -100.0f, -100.0f, kOX, kOZ, kCell), kMoistureBaseline);
    EXPECT_EQ(MoistureAt(grid, 1000.0f, 1000.0f, kOX, kOZ, kCell), kMoistureBaseline);
    // All in-grid values stay within [baseline, max].
    for (int z = 0; z < grid.height(); ++z)
        for (int x = 0; x < grid.width(); ++x) {
            EXPECT_GE(grid.AtCell(x, z), kMoistureBaseline);
            EXPECT_LE(grid.AtCell(x, z), kMoistureMax);
        }
}

// A continuously-fed grid never exceeds saturation (max) even over many ticks.
TEST(Irrigation, BoundedUnderSustainedInput) {
    entt::registry r;
    IrrigationGrid grid(1, 1);
    spawnSource(r, 0.0f, 0.0f, std::numeric_limits<std::uint16_t>::max());
    for (int t = 0; t < 100; ++t) RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);
    EXPECT_EQ(grid.AtCell(0, 0), kMoistureMax - kMoistureDrainPerTick)
        << "each sustained deposit saturates before the fixed drain step";
}

// ---- determinism: run == replay ----

TEST(Irrigation, SystemDeterministicRunEqualsReplay) {
    auto run = [] {
        entt::registry r;
        IrrigationGrid grid(6, 6);
        spawnSource(r, 1.0f, 1.0f, 900);
        spawnSource(r, 4.0f, 2.0f, 1500);
        spawnSource(r, 1.0f, 1.0f, 600); // second source same cell
        std::vector<std::int32_t> trail;
        for (int t = 0; t < 12; ++t) {
            const auto st = RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);
            trail.push_back(static_cast<std::int32_t>(st.deposited));
            trail.push_back(static_cast<std::int32_t>(st.drained));
            for (int z = 0; z < grid.height(); ++z)
                for (int x = 0; x < grid.width(); ++x) trail.push_back(grid.AtCell(x, z));
        }
        return trail;
    };
    EXPECT_EQ(run(), run());
}

// Diffusion is conservative away from the boundary/drain: with drain disabled-equivalent
// (single tick, no drain effect on freshly-deposited excess beyond one step), the total
// moisture-above-baseline after one tick equals deposited minus the single drain step
// applied to wet cells. We assert the looser invariant that the grid total never exceeds
// the cumulative deposit (no moisture is created by diffusion).
TEST(Irrigation, DiffusionDoesNotCreateMoisture) {
    entt::registry r;
    IrrigationGrid grid(5, 5);
    spawnSource(r, 2.0f, 2.0f, 3000);

    std::int64_t cumulativeDeposit = 0;
    for (int t = 0; t < 8; ++t) {
        const auto st = RunIrrigationOnTick(r, grid, kOX, kOZ, kCell);
        cumulativeDeposit += st.deposited;
        std::int64_t total = 0;
        for (int z = 0; z < grid.height(); ++z)
            for (int x = 0; x < grid.width(); ++x)
                total += grid.AtCell(x, z) - kMoistureBaseline;
        EXPECT_LE(total, cumulativeDeposit)
            << "grid moisture-above-baseline never exceeds the moisture put in";
    }
}

} // namespace
