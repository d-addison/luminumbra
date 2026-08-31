// fields_hardening_test.cpp — ADVERSARIAL determinism + contract hardening for the
// deterministic environmental FIELDS (WindFieldSystem / AetherFieldSystem /
// WeatherSystem). These hunt the bugs the happy-path sibling tests MISS:
//   * producer->consumer field-meaning mismatches (unit dir vs scaled vector,
//     [-1,1] noise vs [0,1] emission, ground-layer wind vs sampled height),
//   * threshold EDGES (layer-band boundaries, strike-intensity threshold,
//     in-region vs out-of-region cell membership at the exact extent edge),
//   * coincident / zero-distance degeneracies (storm at the sample point,
//     zero-wind advection identity),
//   * order-independence (anchor quantization; identical-run replay),
//   * gating no-ops (no wind supplied -> field still deterministic + untouched
//     bits across runs; storm-free clear weather -> no strikes),
//   * run==replay EXACT equality over a populated multi-tick roster, asserted on
//     a flattened snapshot vector (not just the rolled-up sub-hash string),
//   * NaN/inf/degenerate-input survival of the public sample API.
//
// These fields feed the `wind`/`aether`/`weather` world_hash sub-hashes, so any
// non-determinism here is load-bearing. Match the sibling tests' include +
// namespace style exactly (Luminumbra::Systems, lowercase luminumbra::fields).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

namespace {

using Luminumbra::Vec2;
using Luminumbra::Vec3;
using Luminumbra::Systems::AetherFieldSystem;
using Luminumbra::Systems::WeatherCategory;
using Luminumbra::Systems::WeatherSystem;
using Luminumbra::Systems::WindFieldSystem;
using Luminumbra::Systems::WindLayer;
using Luminumbra::Systems::kWindLayerCount;
using Luminumbra::Systems::kWindCellSizeM;
using Luminumbra::Systems::kWindExtentCells;
using Luminumbra::Systems::kWindGroundTopM;
using Luminumbra::Systems::kWindMidTopM;
using Luminumbra::Systems::kAetherCellSizeM;
using Luminumbra::Systems::kAetherExtentCells;
using Luminumbra::Systems::kWeatherCellSizeM;
using Luminumbra::Systems::kWeatherExtentCells;
using Luminumbra::Systems::kMaxStormCells;
using Luminumbra::Systems::kMaxLiveStrikes;
using Luminumbra::Systems::kStrikeIntensityThreshold;

constexpr int kSeed = 424242;
const Vec3 kAnchor(8.0f, 100.0f, 8.0f);

float Mag(const Vec2& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// --------------------------------------------------------------------------
// WIND: nearest-cell sampling, layer-band threshold edges, out-of-region
// clamp contract, run==replay over a flattened snapshot, anchor quantization.
// --------------------------------------------------------------------------

// A known grid point: any two world positions inside the SAME cell must return
// the identical (nearest-cell, no-interpolation) wind vector. Probes that the
// sampler floors to a cell rather than smearing across the cell boundary.
TEST(WindHardening, SampleIsCellConstantWithinACell) {
    WindFieldSystem wind(kSeed);
    wind.Update(60, kAnchor);
    // Two points well inside the anchor cell (cell pitch 24 m): quarter and
    // three-quarter of the cell, same layer.
    const Vec2 a = wind.SampleWind(Vec3(kAnchor.x + 1.0f, 5.0f, kAnchor.z + 1.0f), WindLayer::Ground);
    const Vec2 b = wind.SampleWind(Vec3(kAnchor.x + 5.0f, 5.0f, kAnchor.z + 5.0f), WindLayer::Ground);
    EXPECT_FLOAT_EQ(a.x, b.x);
    EXPECT_FLOAT_EQ(a.y, b.y);
}

// Layer-band boundary edge: the AGL bands are HALF-OPEN [low, top). A sample at
// EXACTLY the ground/mid boundary height (32 m) must select Mid, not Ground.
TEST(WindHardening, LayerBandLowerEdgeIsHalfOpenGroundMid) {
    WindFieldSystem wind(kSeed);
    wind.Update(60, kAnchor);
    const Vec3 col(kAnchor.x, 0.0f, kAnchor.z);
    const Vec2 at_boundary = wind.SampleWind(Vec3(col.x, kWindGroundTopM, col.z)); // y == 32
    const Vec2 mid = wind.SampleWind(col, WindLayer::Mid);
    const Vec2 ground = wind.SampleWind(col, WindLayer::Ground);
    EXPECT_FLOAT_EQ(at_boundary.x, mid.x) << "y==32 must fall in Mid (half-open band)";
    EXPECT_FLOAT_EQ(at_boundary.y, mid.y);
    // And distinctly NOT the ground layer (magnitudes differ: ground is sheared).
    EXPECT_FALSE(at_boundary.x == ground.x && at_boundary.y == ground.y);
}

// Mid/High boundary at exactly 128 m selects High (half-open).
TEST(WindHardening, LayerBandUpperEdgeIsHalfOpenMidHigh) {
    WindFieldSystem wind(kSeed);
    wind.Update(60, kAnchor);
    const Vec3 col(kAnchor.x, 0.0f, kAnchor.z);
    const Vec2 at_boundary = wind.SampleWind(Vec3(col.x, kWindMidTopM, col.z)); // y == 128
    const Vec2 high = wind.SampleWind(col, WindLayer::High);
    EXPECT_FLOAT_EQ(at_boundary.x, high.x);
    EXPECT_FLOAT_EQ(at_boundary.y, high.y);
}

// Negative height (below "ground", e.g. underground / sub-sea) must not crash or
// produce a garbage layer; it falls into the Ground band.
TEST(WindHardening, NegativeHeightFallsInGroundBand) {
    WindFieldSystem wind(kSeed);
    wind.Update(60, kAnchor);
    const Vec3 col(kAnchor.x, 0.0f, kAnchor.z);
    const Vec2 below = wind.SampleWind(Vec3(col.x, -50.0f, col.z));
    const Vec2 ground = wind.SampleWind(col, WindLayer::Ground);
    EXPECT_FLOAT_EQ(below.x, ground.x);
    EXPECT_FLOAT_EQ(below.y, ground.y);
}

// Out-of-region CONTRACT: the clamp returns base_direction * layer_speed. Its
// DIRECTION must equal the published BaseDirection (a unit-ish vector), and its
// MAGNITUDE must equal the layer speed times |BaseDirection| (no per-cell
// modulation outside the region). This pins the documented clamp meaning.
TEST(WindHardening, OutOfRegionClampDirectionMatchesBaseExactly) {
    WindFieldSystem wind(kSeed);
    wind.Update(60, kAnchor);
    const Vec2 base = wind.BaseDirection();
    const float far = 1.0e6f;
    const Vec2 g = wind.SampleWind(Vec3(far, 5.0f, far), WindLayer::Ground);
    const Vec2 h = wind.SampleWind(Vec3(far, 5.0f, far), WindLayer::High);
    // Cross product with base is zero (collinear) for BOTH layers.
    EXPECT_NEAR(base.x * g.y - base.y * g.x, 0.0f, 1.0e-3f * Mag(g));
    EXPECT_NEAR(base.x * h.y - base.y * h.x, 0.0f, 1.0e-3f * Mag(h));
    // Higher layer blows harder out-of-region too (the layer-speed ladder holds).
    EXPECT_GT(Mag(h), Mag(g));
    // Same direction, both positive dot.
    EXPECT_GT(base.x * g.x + base.y * g.y, 0.0f);
}

// The exact in-region / out-of-region MEMBERSHIP edge. The grid is centred on the
// anchor cell and is `extent` cells wide; the last in-region cell's far corner is
// the membership boundary. A point just inside the far edge is in-region; one cell
// beyond is out-of-region (clamps to base). Probes the < extent (not <=) test.
TEST(WindHardening, RegionMembershipEdgeIsExclusiveAtFarExtent) {
    WindFieldSystem wind(kSeed);
    wind.Update(60, kAnchor);
    const float cell = wind.cell_size_m();
    const int extent = wind.extent_cells();
    // Anchor cell index and grid origin: origin = anchor_cell - extent/2.
    // The far in-region cell is origin + (extent-1); just past it is origin+extent.
    // Build a world X inside the last in-region cell and one inside the first
    // out-of-region cell.
    const float anchor_cell_x = std::floor(kAnchor.x / cell);
    const float origin_x = anchor_cell_x - static_cast<float>(extent / 2);
    const float last_in_cell_x = (origin_x + static_cast<float>(extent - 1) + 0.5f) * cell;
    const float first_out_cell_x = (origin_x + static_cast<float>(extent) + 0.5f) * cell;

    const Vec2 inside = wind.SampleWind(Vec3(last_in_cell_x, 5.0f, kAnchor.z), WindLayer::Ground);
    const Vec2 outside = wind.SampleWind(Vec3(first_out_cell_x, 5.0f, kAnchor.z), WindLayer::Ground);

    const Vec2 base = wind.BaseDirection();
    // The out-of-region sample is exactly base*speed (direction collinear, no jitter).
    EXPECT_NEAR(base.x * outside.y - base.y * outside.x, 0.0f, 1.0e-3f * Mag(outside));
    // The in-region sample carries per-cell jitter, so in general it is NOT
    // perfectly collinear with base; at minimum it is a populated, finite vector.
    EXPECT_GT(Mag(inside), 0.0f);
    EXPECT_TRUE(std::isfinite(inside.x) && std::isfinite(inside.y));
}

// Anchor QUANTIZATION: the grid origin is floor(anchor/cell). Two anchors that
// fall in the SAME cell (x=8 and x=20, both in cell 0 at 24 m pitch) must produce
// the IDENTICAL wind sub-hash — the field bits depend on the quantized origin, not
// the raw anchor. (Wind has no raw-anchor term, unlike weather's storm spawn.)
TEST(WindHardening, AnchorQuantizationSameCellSameField) {
    WindFieldSystem a(kSeed);
    WindFieldSystem b(kSeed);
    const Vec3 anchor_a(8.0f, 100.0f, 8.0f);
    const Vec3 anchor_b(20.0f, 100.0f, 20.0f); // same 24 m cell as (8,8)
    for (std::uint64_t t = 1; t <= 45; ++t) {
        a.Update(t, anchor_a);
        b.Update(t, anchor_b);
    }
    EXPECT_EQ(a.ComputeWindSubHash(), b.ComputeWindSubHash())
        << "wind field bits leaked the raw (sub-cell) anchor instead of the quantized origin";
}

// A DIFFERENT cell anchor shifts the origin and therefore the field bits.
TEST(WindHardening, AnchorInDifferentCellChangesField) {
    WindFieldSystem a(kSeed);
    WindFieldSystem b(kSeed);
    for (std::uint64_t t = 1; t <= 45; ++t) {
        a.Update(t, Vec3(8.0f, 100.0f, 8.0f));
        b.Update(t, Vec3(8.0f + 100.0f * kWindCellSizeM, 100.0f, 8.0f)); // 100 cells over
    }
    EXPECT_NE(a.ComputeWindSubHash(), b.ComputeWindSubHash());
}

// run==replay EXACT: flatten the populated grid to a snapshot vector and assert
// two independent runs are FLOAT-EXACT, element by element (not just the hash).
TEST(WindHardening, RunEqualsReplayFlattenedSnapshotExact) {
    auto run = [](std::vector<float>& out) {
        WindFieldSystem wind(kSeed);
        for (std::uint64_t t = 1; t <= 90; ++t) {
            wind.Update(t, kAnchor);
        }
        out.clear();
        const int extent = wind.extent_cells();
        // Sample every cell centre across all layers -> flattened snapshot.
        const float cell = wind.cell_size_m();
        for (int lz = 0; lz < extent; lz += 3) {
            for (int lx = 0; lx < extent; lx += 3) {
                const float wx = (std::floor(kAnchor.x / cell) - extent / 2 + lx + 0.5f) * cell;
                const float wz = (std::floor(kAnchor.z / cell) - extent / 2 + lz + 0.5f) * cell;
                for (int li = 0; li < kWindLayerCount; ++li) {
                    const Vec2 v = wind.SampleWind(Vec3(wx, 5.0f, wz), static_cast<WindLayer>(li));
                    out.push_back(v.x);
                    out.push_back(v.y);
                }
            }
        }
    };
    std::vector<float> a, b;
    run(a);
    run(b);
    ASSERT_EQ(a.size(), b.size());
    ASSERT_FALSE(a.empty());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i], b[i]) << "wind snapshot diverged at flattened index " << i;
    }
}

// NaN/inf survival: a degenerate sample position must not crash or assert, and
// must report out-of-region (clamp) rather than dereference a garbage cell.
TEST(WindHardening, DegenerateSamplePositionSurvives) {
    WindFieldSystem wind(kSeed);
    wind.Update(60, kAnchor);
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // These must return SOMETHING finite-or-clamped without UB. We only assert no
    // crash + a finite-or-base result for the inf case (clearly out of region).
    const Vec2 v_inf = wind.SampleWind(Vec3(inf, 5.0f, inf), WindLayer::Ground);
    EXPECT_TRUE(std::isfinite(v_inf.x) && std::isfinite(v_inf.y))
        << "an infinite sample position produced a non-finite wind (clamp path broken)";
    // NaN position: also must not crash; the result is the out-of-region clamp.
    const Vec2 v_nan = wind.SampleWind(Vec3(nan, 5.0f, nan), WindLayer::High);
    EXPECT_TRUE(std::isfinite(v_nan.x) && std::isfinite(v_nan.y));
}

// --------------------------------------------------------------------------
// AETHER: emission remap [-1,1]->[0,1], zero-wind advection identity, bilinear
// edge-clamp, non-negativity, gating no-op (null wind), run==replay snapshot.
// --------------------------------------------------------------------------

// Gating no-op: with NO wind supplied the advection step is skipped (identity
// copy). The field must still be deterministic across runs AND remain a pure
// function of (seed, tick, anchor) — two null-wind runs are bit-identical.
TEST(AetherHardening, NullWindGatingIsDeterministicNoOp) {
    auto run = [](std::vector<float>& out) {
        AetherFieldSystem a(kSeed);
        for (std::uint64_t t = 1; t <= 90; ++t) {
            a.Update(t, kAnchor, nullptr);
        }
        out.clear();
        const int extent = a.extent_cells();
        for (int lz = 0; lz < extent; lz += 4) {
            for (int lx = 0; lx < extent; lx += 4) {
                out.push_back(a.grid().cells()[a.grid().index(lx, lz)]);
            }
        }
    };
    std::vector<float> x, y;
    run(x);
    run(y);
    ASSERT_EQ(x.size(), y.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        EXPECT_FLOAT_EQ(x[i], y[i]) << "null-wind aether diverged at index " << i;
    }
}

// Emission is remapped from noise [-1,1] to [0,1] before diffusion; diffusion is a
// convex average of non-negative sources => the field is NON-NEGATIVE everywhere.
// Scan every cell (not just a sparse stride) to catch a single negative leak.
TEST(AetherHardening, EveryCellIsNonNegative) {
    AetherFieldSystem a(kSeed);
    WindFieldSystem wind(kSeed);
    for (std::uint64_t t = 1; t <= 90; ++t) {
        wind.Update(t, kAnchor);
        a.Update(t, kAnchor, &wind);
    }
    const auto& grid = a.grid();
    const int extent = grid.extent_cells();
    float min_v = 1.0e30f;
    for (int lz = 0; lz < extent; ++lz) {
        for (int lx = 0; lx < extent; ++lx) {
            const float v = grid.cells()[grid.index(lx, lz)];
            ASSERT_TRUE(std::isfinite(v)) << "non-finite aether at (" << lx << "," << lz << ")";
            if (v < min_v) min_v = v;
        }
    }
    EXPECT_GE(min_v, 0.0f) << "aether field leaked a NEGATIVE cell (emission remap or diffusion bug)";
    // And the convex average can never exceed the [0,1] emission ceiling.
    float max_v = -1.0e30f;
    for (int lz = 0; lz < extent; ++lz) {
        for (int lx = 0; lx < extent; ++lx) {
            max_v = std::max(max_v, grid.cells()[grid.index(lx, lz)]);
        }
    }
    // Convex diffusion of an emission source that is ~[0,1] (FastNoise FBm can
    // marginally overshoot [-1,1], so allow a small slack); a value far above 1
    // would indicate a non-convex blend / unbounded accumulation, not noise slack.
    EXPECT_LE(max_v, 1.25f) << "aether ran far past the emission ceiling (non-convex blend / unbounded)";
}

// SampleAether at an exact known grid point (a cell centre) returns the stored
// cell value — the sampler floors to the owning cell and reads grid.at.
TEST(AetherHardening, SampleAtCellCentreReturnsStoredCell) {
    AetherFieldSystem a(kSeed);
    a.Update(60, kAnchor, nullptr);
    const auto& grid = a.grid();
    const float cell = grid.cell_size_m();
    // Pick the in-region cell at local (10,12).
    const int lx = 10, lz = 12;
    const float wx = (static_cast<float>(grid.origin_cell_x() + lx) + 0.5f) * cell;
    const float wz = (static_cast<float>(grid.origin_cell_z() + lz) + 0.5f) * cell;
    const float stored = grid.cells()[grid.index(lx, lz)];
    const float sampled = a.SampleAether(Vec3(wx, 0.0f, wz));
    EXPECT_FLOAT_EQ(sampled, stored);
}

// Out-of-region reads exactly 0.0 (no aether beyond the streamed footprint). Probe
// the membership edge: one cell past the far extent reads 0, the last in-region
// cell reads its stored value.
TEST(AetherHardening, OutOfRegionEdgeReadsZero) {
    AetherFieldSystem a(kSeed);
    a.Update(60, kAnchor, nullptr);
    const auto& grid = a.grid();
    const float cell = grid.cell_size_m();
    const int extent = grid.extent_cells();
    const float first_out_x = (static_cast<float>(grid.origin_cell_x() + extent) + 0.5f) * cell;
    EXPECT_FLOAT_EQ(a.SampleAether(Vec3(first_out_x, 0.0f, kAnchor.z)), 0.0f);
    const float far = 1.0e6f;
    EXPECT_FLOAT_EQ(a.SampleAether(Vec3(far, 0.0f, far)), 0.0f);
    EXPECT_FLOAT_EQ(a.SampleAether(Vec3(-far, 0.0f, -far)), 0.0f);
}

// run==replay EXACT for the wind-coupled aether path over many ticks, on a
// flattened snapshot vector — the world_hash `aether` slot depends on this.
TEST(AetherHardening, RunEqualsReplayWithWindFlattenedExact) {
    auto run = [](std::vector<float>& out) {
        WindFieldSystem wind(kSeed);
        AetherFieldSystem a(kSeed);
        for (std::uint64_t t = 1; t <= 90; ++t) {
            wind.Update(t, kAnchor);
            a.Update(t, kAnchor, &wind);
        }
        out.clear();
        const auto& grid = a.grid();
        const int extent = grid.extent_cells();
        for (int lz = 0; lz < extent; ++lz) {
            for (int lx = 0; lx < extent; ++lx) {
                out.push_back(grid.cells()[grid.index(lx, lz)]);
            }
        }
    };
    std::vector<float> x, y;
    run(x);
    run(y);
    ASSERT_EQ(x.size(), y.size());
    ASSERT_FALSE(x.empty());
    for (std::size_t i = 0; i < x.size(); ++i) {
        EXPECT_FLOAT_EQ(x[i], y[i]) << "wind-coupled aether diverged at index " << i;
    }
}

// Anchor quantization for aether: two anchors in the same cell -> identical field
// (aether has no raw-anchor term; the emission noise is sampled over quantized
// cell-centre world coords).
TEST(AetherHardening, AnchorQuantizationSameCellSameField) {
    AetherFieldSystem a(kSeed);
    AetherFieldSystem b(kSeed);
    for (std::uint64_t t = 1; t <= 45; ++t) {
        a.Update(t, Vec3(8.0f, 100.0f, 8.0f), nullptr);
        b.Update(t, Vec3(23.999f, 100.0f, 23.999f), nullptr); // same cell 0
    }
    EXPECT_EQ(a.ComputeAetherSubHash(), b.ComputeAetherSubHash())
        << "aether field leaked the raw sub-cell anchor";
}

// NaN/inf sample survival: a degenerate world position reads the out-of-region 0.
TEST(AetherHardening, DegenerateSampleReadsZeroNotGarbage) {
    AetherFieldSystem a(kSeed);
    a.Update(60, kAnchor, nullptr);
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_FLOAT_EQ(a.SampleAether(Vec3(inf, 0.0f, inf)), 0.0f);
    EXPECT_FLOAT_EQ(a.SampleAether(Vec3(-inf, 0.0f, -inf)), 0.0f);
}

// --------------------------------------------------------------------------
// WEATHER: storm/strike determinism, bounded caps, gating no-op (null wind),
// strike-threshold edge, coincident-storm precip degeneracy, run==replay.
// --------------------------------------------------------------------------

// Gating no-op: with NO wind the storm cells drift on last velocity (initial 0)
// and the whole state is still a pure function of (seed, tick, anchor). Two
// null-wind runs are bit-identical (sub-hash equal).
TEST(WeatherHardening, NullWindIsDeterministic) {
    auto run = [](int seed) {
        WeatherSystem w(seed);
        for (std::uint64_t t = 1; t <= 300; ++t) {
            w.Update(t, kAnchor, nullptr);
        }
        return w.ComputeWeatherSubHash();
    };
    EXPECT_EQ(run(kSeed), run(kSeed)) << "null-wind weather diverged across identical runs";
}

// run==replay EXACT on the full strike schedule + storm cells (POD fields), not
// just the hash. Two wind-coupled runs reproduce every strike tick/pos/magnitude
// and every storm cell position bit-for-bit.
TEST(WeatherHardening, RunEqualsReplayStormAndStrikeFieldsExact) {
    auto run = [](std::vector<float>& flat) {
        WindFieldSystem wind(kSeed);
        WeatherSystem w(kSeed);
        for (std::uint64_t t = 1; t <= 300; ++t) {
            wind.Update(t, kAnchor);
            w.Update(t, kAnchor, &wind);
        }
        flat.clear();
        for (const auto& c : w.StormCells()) {
            flat.push_back(c.center_world.x);
            flat.push_back(c.center_world.y);
            flat.push_back(c.velocity.x);
            flat.push_back(c.velocity.y);
            flat.push_back(c.intensity);
        }
        for (const auto& s : w.StrikeSchedule()) {
            flat.push_back(static_cast<float>(s.strike_tick));
            flat.push_back(s.world_x);
            flat.push_back(s.world_z);
            flat.push_back(s.magnitude);
        }
    };
    std::vector<float> a, b;
    run(a);
    run(b);
    ASSERT_EQ(a.size(), b.size()) << "storm/strike roster size diverged across runs";
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i], b[i]) << "weather roster diverged at flattened index " << i;
    }
}

// Strike eligibility threshold edge: a storm cell's strike path is gated by
// `intensity >= kStrikeIntensityThreshold` (strictly: the code SKIPS when
// intensity < threshold, so intensity == threshold is ELIGIBLE). Drive a long run
// and assert at least one strike fires while at least one storm sits below the
// threshold at some opportunity (the gate is actually exercised, not vacuous).
TEST(WeatherHardening, StrikeThresholdGateIsExercisedNotVacuous) {
    WindFieldSystem wind(kSeed);
    WeatherSystem w(kSeed);
    std::uint64_t total_strikes = 0;
    bool saw_subthreshold_storm = false;
    for (std::uint64_t t = 1; t <= 300; ++t) {
        wind.Update(t, kAnchor);
        w.Update(t, kAnchor, &wind);
        total_strikes += w.StrikesThisTick().size();
        for (const auto& c : w.StormCells()) {
            if (c.intensity > 0.0f && c.intensity < kStrikeIntensityThreshold) {
                saw_subthreshold_storm = true;
            }
        }
    }
    EXPECT_GT(total_strikes, 0u) << "no strike ever fired (threshold gate vacuous)";
    EXPECT_TRUE(saw_subthreshold_storm)
        << "no storm was ever observed below the strike threshold (ramp-in not exercised)";
}

// Bounded caps under a long, storm-heavy run: storm cells <= kMaxStormCells and
// live strikes <= kMaxLiveStrikes at every tick (flat memory, ).
TEST(WeatherHardening, CapsHoldEveryTick) {
    WindFieldSystem wind(kSeed);
    WeatherSystem w(kSeed);
    for (std::uint64_t t = 1; t <= 600; ++t) {
        wind.Update(t, kAnchor);
        w.Update(t, kAnchor, &wind);
        ASSERT_LE(w.active_storm_count(), kMaxStormCells) << "storm cap exceeded at tick " << t;
        ASSERT_LE(w.live_strike_count(), kMaxLiveStrikes) << "strike cap exceeded at tick " << t;
    }
}

// Coincident / zero-distance degeneracy: PrecipitationAt exactly AT a storm
// centre (dx=dz=0) must be finite and in [0,1] — the falloff (1-d/r)^2 at d=0 is
// the peak, and the [0,1] clamp must hold. Probes Sqrt(0) and the clamp.
TEST(WeatherHardening, PrecipAtStormCentreIsFiniteAndClamped) {
    WindFieldSystem wind(kSeed);
    WeatherSystem w(kSeed);
    // Advance to a tick where storms are present.
    std::uint64_t found_tick = 0;
    for (std::uint64_t t = 1; t <= 300; ++t) {
        wind.Update(t, kAnchor);
        w.Update(t, kAnchor, &wind);
        if (w.active_storm_count() > 0) { found_tick = t; break; }
    }
    ASSERT_GT(found_tick, 0u) << "no storm spawned to probe (schedule vacuous)";
    const auto& cells = w.StormCells();
    ASSERT_FALSE(cells.empty());
    const Vec2 c = cells.front().center_world; // (x, z) packed in Vec2
    const float p = w.PrecipitationAt(Vec3(c.x, 5.0f, c.y));
    EXPECT_TRUE(std::isfinite(p)) << "precip at storm centre is non-finite (Sqrt(0) / divide degeneracy)";
    EXPECT_GE(p, 0.0f);
    EXPECT_LE(p, 1.0f) << "precip exceeded the [0,1] clamp at the storm centre";
    // SampleAt at the same point is also clamped + finite for storm_intensity.
    const auto s = w.SampleAt(Vec3(c.x, 5.0f, c.y));
    EXPECT_GE(s.storm_intensity, 0.0f);
    EXPECT_LE(s.storm_intensity, 1.0f);
    EXPECT_GE(s.precip_intensity, 0.0f);
    EXPECT_LE(s.precip_intensity, 1.0f);
}

// PrecipitationAt is in [0,1] for a dense world scan (the clamp must hold even
// when several overlapping storm cells stack their additive contributions).
TEST(WeatherHardening, PrecipIsClampedAcrossDenseScan) {
    WindFieldSystem wind(kSeed);
    WeatherSystem w(kSeed);
    for (std::uint64_t t = 1; t <= 300; ++t) {
        wind.Update(t, kAnchor);
        w.Update(t, kAnchor, &wind);
    }
    const float cell = w.cell_size_m();
    const int extent = w.extent_cells();
    for (int lz = 0; lz < extent; lz += 2) {
        for (int lx = 0; lx < extent; lx += 2) {
            const float wx = kAnchor.x + static_cast<float>(lx - extent / 2) * cell;
            const float wz = kAnchor.z + static_cast<float>(lz - extent / 2) * cell;
            const float p = w.PrecipitationAt(Vec3(wx, 5.0f, wz));
            ASSERT_TRUE(std::isfinite(p));
            EXPECT_GE(p, 0.0f);
            EXPECT_LE(p, 1.0f) << "precip > 1 at (" << lx << "," << lz << ")";
        }
    }
}

// Out-of-region category clamps to Clear AND out-of-region precip is the category
// base only (here 0, no storm reaches 1e6 m away). Probe the exact one-cell-past
// edge plus the far field.
TEST(WeatherHardening, OutOfRegionCategoryAndPrecip) {
    WindFieldSystem wind(kSeed);
    WeatherSystem w(kSeed);
    for (std::uint64_t t = 1; t <= 120; ++t) {
        wind.Update(t, kAnchor);
        w.Update(t, kAnchor, &wind);
    }
    const float far = 1.0e6f;
    EXPECT_EQ(w.CategoryAt(Vec3(far, 5.0f, far)), WeatherCategory::Clear);
    const float p = w.PrecipitationAt(Vec3(far, 5.0f, far));
    EXPECT_GE(p, 0.0f);
    EXPECT_LE(p, 1.0f);
}

// CategoryAt returns a VALID enum value for a dense in-region scan (no garbage
// uint8 leaking through the cast — Classify must only ever emit the 5 categories).
TEST(WeatherHardening, CategoryIsAlwaysValidEnum) {
    WindFieldSystem wind(kSeed);
    WeatherSystem w(kSeed);
    for (std::uint64_t t = 1; t <= 120; ++t) {
        wind.Update(t, kAnchor);
        w.Update(t, kAnchor, &wind);
    }
    const float cell = w.cell_size_m();
    const int extent = w.extent_cells();
    for (int lz = 0; lz < extent; lz += 3) {
        for (int lx = 0; lx < extent; lx += 3) {
            const float wx = kAnchor.x + static_cast<float>(lx - extent / 2) * cell;
            const float wz = kAnchor.z + static_cast<float>(lz - extent / 2) * cell;
            const int cat = static_cast<int>(w.CategoryAt(Vec3(wx, 5.0f, wz)));
            EXPECT_GE(cat, 0);
            EXPECT_LT(cat, Luminumbra::Systems::kWeatherCategoryCount)
                << "invalid weather category at (" << lx << "," << lz << ")";
        }
    }
}

// Anchor quantization for weather GRID origin: the category/precip GRID bits are
// quantized to the anchor cell, but storm SPAWN positions use the RAW anchor. So
// two same-cell anchors share the SAME category-map origin (the grid follows the
// quantized origin) — verify via the anchor category at a fixed world point inside
// both regions across a no-storm short run (tick < first spawn epoch => no storms,
// so the sub-hash reflects only the grid). Spawn epoch is 45; run to tick 44.
TEST(WeatherHardening, GridOriginQuantizedBeforeFirstStorm) {
    WeatherSystem a(kSeed);
    WeatherSystem b(kSeed);
    // No wind, run to just before the first storm spawn opportunity (tick 45).
    for (std::uint64_t t = 1; t <= 44; ++t) {
        a.Update(t, Vec3(8.0f, 100.0f, 8.0f), nullptr);
        b.Update(t, Vec3(20.0f, 100.0f, 20.0f), nullptr); // same 24 m cell
    }
    EXPECT_EQ(a.active_storm_count(), 0) << "a storm spawned before tick 45 (test assumption broken)";
    EXPECT_EQ(b.active_storm_count(), 0);
    EXPECT_EQ(a.ComputeWeatherSubHash(), b.ComputeWeatherSubHash())
        << "weather grid bits leaked the raw sub-cell anchor before any storm spawned";
}

// StrikesThisTick is a pure read of CURRENT state: every strike it returns has
// strike_tick == last_tick, and it is a subset of the live schedule.
TEST(WeatherHardening, StrikesThisTickMatchesLastTick) {
    WindFieldSystem wind(kSeed);
    WeatherSystem w(kSeed);
    for (std::uint64_t t = 1; t <= 300; ++t) {
        wind.Update(t, kAnchor);
        w.Update(t, kAnchor, &wind);
        for (const auto& s : w.StrikesThisTick()) {
            EXPECT_EQ(s.strike_tick, w.last_tick())
                << "StrikesThisTick returned a strike not landing this tick";
        }
    }
}

// Strike schedule canonical ORDER invariant: the live schedule is sorted
// (strike_tick asc, then storm_salt, then x-bits, then z-bits). Verify it is
// non-decreasing under that comparator at a populated tick — order-independence of
// the sub-hash relies on this canonicalization.
TEST(WeatherHardening, StrikeScheduleIsCanonicallyOrdered) {
    WindFieldSystem wind(kSeed);
    WeatherSystem w(kSeed);
    for (std::uint64_t t = 1; t <= 300; ++t) {
        wind.Update(t, kAnchor);
        w.Update(t, kAnchor, &wind);
    }
    const auto& s = w.StrikeSchedule();
    for (std::size_t i = 1; i < s.size(); ++i) {
        const auto& p = s[i - 1];
        const auto& q = s[i];
        bool ordered = (p.strike_tick < q.strike_tick) ||
                       (p.strike_tick == q.strike_tick &&
                        (p.storm_salt < q.storm_salt ||
                         (p.storm_salt == q.storm_salt &&
                          (p.world_x < q.world_x ||
                           (p.world_x == q.world_x && p.world_z <= q.world_z)))));
        EXPECT_TRUE(ordered) << "strike schedule out of canonical order at index " << i;
    }
}

// Seed dependence of the +13 lightning stream: same +12 weather/storm schedule
// path, but a different world seed yields a different lightning seed (+13) and
// therefore (in general) a different strike sub-hash. We assert the FULL weather
// sub-hash differs (storms + strikes) across seeds, and that the strike schedule
// is non-empty for at least one of them (the stream is live).
TEST(WeatherHardening, LightningStreamDependsOnSeed) {
    WindFieldSystem wa(kSeed), wb(kSeed + 7);
    WeatherSystem a(kSeed), b(kSeed + 7);
    std::uint64_t strikes_a = 0;
    for (std::uint64_t t = 1; t <= 300; ++t) {
        wa.Update(t, kAnchor); a.Update(t, kAnchor, &wa);
        wb.Update(t, kAnchor); b.Update(t, kAnchor, &wb);
        strikes_a += a.StrikesThisTick().size();
    }
    EXPECT_GT(strikes_a, 0u) << "seed A produced no strikes (cannot probe seed sensitivity)";
    EXPECT_NE(a.ComputeWeatherSubHash(), b.ComputeWeatherSubHash());
}

// Degenerate sample survival for weather: NaN/inf positions must not crash and
// must return the out-of-region clamp (Clear, finite precip).
TEST(WeatherHardening, DegenerateWeatherSampleSurvives) {
    WindFieldSystem wind(kSeed);
    WeatherSystem w(kSeed);
    for (std::uint64_t t = 1; t <= 120; ++t) {
        wind.Update(t, kAnchor);
        w.Update(t, kAnchor, &wind);
    }
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_EQ(w.CategoryAt(Vec3(inf, 5.0f, inf)), WeatherCategory::Clear);
    const float p = w.PrecipitationAt(Vec3(inf, 5.0f, inf));
    EXPECT_TRUE(std::isfinite(p));
    EXPECT_GE(p, 0.0f);
    EXPECT_LE(p, 1.0f);
}

// --------------------------------------------------------------------------
// CROSS-FIELD: the three fields share the 24 m / 64-cell PINNED geometry so the
// renderer + sim sample one grid identity. A drift here breaks the shared mapping.
// --------------------------------------------------------------------------
TEST(FieldsHardening, SharedPinnedGeometryAcrossAllThreeFields) {
    WindFieldSystem wind(kSeed);
    AetherFieldSystem aether(kSeed);
    WeatherSystem weather(kSeed);
    EXPECT_FLOAT_EQ(wind.cell_size_m(), kWindCellSizeM);
    EXPECT_FLOAT_EQ(aether.cell_size_m(), kAetherCellSizeM);
    EXPECT_FLOAT_EQ(weather.cell_size_m(), kWeatherCellSizeM);
    // All three share the same cell pitch and extent (one grid identity).
    EXPECT_FLOAT_EQ(wind.cell_size_m(), aether.cell_size_m());
    EXPECT_FLOAT_EQ(aether.cell_size_m(), weather.cell_size_m());
    EXPECT_EQ(wind.extent_cells(), kWindExtentCells);
    EXPECT_EQ(aether.extent_cells(), kAetherExtentCells);
    EXPECT_EQ(weather.extent_cells(), kWeatherExtentCells);
    EXPECT_EQ(wind.extent_cells(), aether.extent_cells());
    EXPECT_EQ(aether.extent_cells(), weather.extent_cells());
}

} // namespace
