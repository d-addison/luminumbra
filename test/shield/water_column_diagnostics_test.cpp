#include "luminumbra/core/Types.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <initializer_list>
#include <limits>
#include <memory>

TEST(WaterColumnDiagnostics, CarvabilityUsesBedTerrainAndRejectsMissingOrPartialData) {
    using Luminumbra::Chunk;
    using Luminumbra::IVec3;
    using Luminumbra::Vec3;
    using Luminumbra::Systems::SHIELD_WorldSystem;
    using Luminumbra::Systems::TerrainGenParams;
    for (const int invalid : {0, 1, 2, 3, 4, 5}) {
        SCOPED_TRACE(invalid);
        TerrainGenParams params;
        params.hydro_enabled = false;
        params.biomes_enabled = false;
        params.height_offset = 40.0f;
        params.base_amplitude = 0.0f;
        params.peaks_amplitude = 0.0f;
        params.continental_spline = {{-1.0f, 0.0f}, {1.0f, 0.0f}};
        params.island_mask_enabled = false;
        params.cliffs_enabled = false;
        params.rivers_enabled = false;
        params.lakes_enabled = false;
        params.surface_breaks_enabled = false;
        params.structures_enabled = false;
        SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
        Vec3 position;
        float bed = 0.0f;
        EXPECT_FALSE(world.debug_lowest_land_pos(position, bed));
        std::int64_t depth = -1;
        (void)world.debug_deepest_water_pos(&depth);
        EXPECT_EQ(depth, 0);

        auto owner = std::make_shared<Chunk>(IVec3(0, 0, 0));
        owner->has_water_sim.store(true);
        owner->current_water_resolution.store(4);
        owner->water_depth_mm.assign(16, 3000);
        owner->water_bed_mm.assign(16, 24000);
        if (invalid == 3)
            owner->water_bed_mm.assign(16, std::numeric_limits<std::int32_t>::max());
        if (invalid == 4)
            owner->current_water_resolution.store(std::numeric_limits<int>::max());
        if (invalid == 5)
            owner->water_bed_mm.pop_back();
        ASSERT_TRUE(world.adopt_streamed_chunk(owner));
        ASSERT_TRUE(owner->sdf_data.empty());
        if (invalid != 1) {
            auto terrain = std::make_shared<Chunk>(IVec3(0, 1, 0));
            // The editable lattice is bounded to one padded 17*17*17 chunk.
            constexpr std::size_t samples = std::size_t{17} * 17 * 17;
            terrain->sdf_data.assign(samples - (invalid == 2 ? 1 : 0), 1.0f);
            ASSERT_TRUE(world.adopt_streamed_chunk(terrain));
        }
        const bool valid = invalid == 0;
        EXPECT_EQ(world.debug_lowest_land_pos(position, bed), valid);
        if (valid) {
            EXPECT_FLOAT_EQ(bed, 24.0f);
        }
        (void)world.debug_deepest_water_pos(&depth);
        EXPECT_EQ(depth, valid ? 3000 : 0);
        float to_land_x = 0.0f, to_land_z = 0.0f, surface = 0.0f, bank = 0.0f;
        EXPECT_EQ(world.debug_find_shoreline(position, to_land_x, to_land_z, surface, bank), valid);
        if (valid) {
            EXPECT_FLOAT_EQ(surface, 27.0f);
            EXPECT_GT(bank, 2.5f);
        }
    }
}
