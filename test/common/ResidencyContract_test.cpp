// Spec 018 FR-A + FR-B — locks the two-worlds residency / availability-set contract
// (src/luminumbra_common/core/ResidencyContract.h) so it stops being a dormant header:
// this TU includes it (compiling its static_asserts into the build) and pins the
// behavioural invariants Spec 017-B must implement against. Behaviour-neutral: it asserts
// the contract's shape only — no system is wired here.

#include "core/ResidencyContract.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

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

// FR-A-003 (SHIELD-06) — the world_hash exclusion scope DERIVES from the declared
// partition, table-driven. Pins the projection BYTE-EXACTLY to the historical
// hand-maintained kRenderMeshHashExcludedFields set (so the derivation is
// hash-neutral) and asserts the load-bearing hashed fields classify Sim. The
// completeness half (every serialized key is classified) is enforced in
// production by VerifyChunkFieldResidencyCoverage on the first hashed chunk.
TEST(ResidencyContract, HashScopeDerivesFromPartition) {
    using luminumbra::core::kChunkFieldResidency;

    // The exclusion set — the Render-classified subset of the table must equal
    // EXACTLY these 19 names (order-insensitive). History: the original 14 were the
    // hand-maintained kRenderMeshHashExcludedFields; WATER-17 (Bump A, 2026-07-05)
    // deliberately added water_mesh_generated + water_mesh_dirty_ticks — meshing
    // bookkeeping mutated by the render-side mesh pipeline, whose hashing made the
    // save/load water round-trip impossible (the loaded-boot remesh flips them while
    // the water sim is paused). WATER-08 (Bump B, 2026-07-05) then reclassified the
    // three water FLOAT MIRRORS — water_level_data, water_flow_data,
    // water_sim_terrain_height — to Render ("float mirrors = Render, mm = the only
    // water sim truth"): they are render-side derived views and must not feed
    // world_hash. This literal records that deliberate, reviewed hash-scope move (the
    // canonical baselines already carry it — the Bump B commit re-pinned them).
    const std::set<std::string> legacy_excluded = {
        "mesh_vertices", "mesh_indices",
        "water_mesh_vertices", "water_mesh_indices",
        "pending_mesh_vertices", "pending_mesh_indices",
        "pending_water_mesh_vertices", "pending_water_mesh_indices",
        "mesh_version", "water_mesh_version",
        "pending_mesh_ready", "pending_mesh_failed",
        "current_lod", "pending_lod",
        "water_mesh_generated", "water_mesh_dirty_ticks",
        // WATER-08 Bump B (2026-07-05): the water float mirrors, now Render.
        "water_level_data", "water_flow_data", "water_sim_terrain_height",
    };

    std::set<std::string> derived_excluded;
    std::set<std::string> all_fields;
    for (const auto& entry : kChunkFieldResidency) {
        EXPECT_TRUE(all_fields.insert(entry.field).second)
            << "duplicate field classification: " << entry.field;
        if (!MayFeedWorldHash(entry.residency)) {
            derived_excluded.insert(entry.field);
        }
    }
    EXPECT_EQ(derived_excluded, legacy_excluded)
        << "the derived exclusion scope drifted from the historical hash scope — "
           "that is a world_hash change and must be a deliberate reviewed bump";

    // Load-bearing hashed fields stay Sim.
    for (const char* sim_field : {"state", "state_value", "sdf_data", "heightmap_data",
                                  "material_data", "has_collision", "water_depth_mm",
                                  "water_bed_mm", "water_edge_flux", "has_water_sim"}) {
        bool found = false;
        for (const auto& entry : kChunkFieldResidency) {
            if (std::string(entry.field) == sim_field) {
                found = true;
                EXPECT_TRUE(MayFeedWorldHash(entry.residency))
                    << sim_field << " must classify Sim (it feeds world_hash)";
                break;
            }
        }
        EXPECT_TRUE(found) << sim_field << " missing from kChunkFieldResidency";
    }
}

}  // namespace

