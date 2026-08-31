// Residency classification tests. These pin the production table that separates
// authoritative simulation fields from render-only fields.

#include "core/ResidencyContract.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace {

using luminumbra::core::MayFeedWorldHash;
using luminumbra::core::RenderResidency;
using luminumbra::core::ResidencyClass;
using luminumbra::core::SimResidency;

// only Sim residency may feed world_hash; Render never may.
TEST(ResidencyContract, OnlySimMayFeedWorldHash) {
    EXPECT_TRUE(MayFeedWorldHash(ResidencyClass::Sim));
    EXPECT_FALSE(MayFeedWorldHash(ResidencyClass::Render));
    // The tag types carry the same fact at compile time.
    EXPECT_EQ(SimResidency::kClass, ResidencyClass::Sim);
    EXPECT_EQ(RenderResidency::kClass, ResidencyClass::Render);
    EXPECT_TRUE(MayFeedWorldHash(SimResidency::kClass));
    EXPECT_FALSE(MayFeedWorldHash(RenderResidency::kClass));
}

// the world_hash exclusion scope DERIVES from the declared
// partition, table-driven. Pins the projection BYTE-EXACTLY to the historical
// hand-maintained kRenderMeshHashExcludedFields set (so the derivation is
// hash-neutral) and asserts the load-bearing hashed fields classify Sim. The
// completeness half (every serialized key is classified) is enforced in
// production by VerifyChunkFieldResidencyCoverage on the first hashed chunk.
TEST(ResidencyContract, HashScopeDerivesFromPartition) {
    using luminumbra::core::kChunkFieldResidency;

    // The exclusion set — the Render-classified subset of the table must equal
    // EXACTLY these 19 names (order-insensitive). History: the original 14 were the
    // hand-maintained kRenderMeshHashExcludedFields;  (authoritative-state change, 2026-07-05)
    // deliberately added water_mesh_generated + water_mesh_dirty_ticks — meshing
    // bookkeeping mutated by the render-side mesh pipeline, whose hashing made the
    // save/load water round-trip impossible (the loaded-boot remesh flips them while
    // the water sim is paused).  (derived-state reclassification, 2026-07-05) then reclassified the
    // three water FLOAT MIRRORS — water_level_data, water_flow_data,
    // water_sim_terrain_height — to Render ("float mirrors = Render, mm = the only
    // water sim truth"): they are render-side derived views and must not feed
    // world_hash. This literal records that deliberate, reviewed hash-scope move (the
    // canonical baselines already carry it — the derived-state reclassification commit re-pinned
    // them).
    const std::set<std::string> legacy_excluded = {
        "mesh_vertices",
        "mesh_indices",
        "water_mesh_vertices",
        "water_mesh_indices",
        "pending_mesh_vertices",
        "pending_mesh_indices",
        "pending_water_mesh_vertices",
        "pending_water_mesh_indices",
        "mesh_version",
        "water_mesh_version",
        "pending_mesh_ready",
        "pending_mesh_failed",
        "current_lod",
        "pending_lod",
        "water_mesh_generated",
        "water_mesh_dirty_ticks",
        //  derived-state reclassification (2026-07-05): the water float mirrors, now Render.
        "water_level_data",
        "water_flow_data",
        "water_sim_terrain_height",
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
    for (const char* sim_field : {"state",
                                  "state_value",
                                  "sdf_data",
                                  "heightmap_data",
                                  "material_data",
                                  "has_collision",
                                  "water_depth_mm",
                                  "water_bed_mm",
                                  "water_edge_flux",
                                  "has_water_sim"}) {
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

} // namespace
