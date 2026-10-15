#pragma once

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"

namespace Luminumbra::Test {
// Fixed synthetic workloads opt out explicitly; shipping defaults are tested separately.
inline Systems::TerrainGenParams SyntheticTerrainParams() {
    Systems::TerrainGenParams params;
    params.surface_breaks_enabled = false;
    params.island_mask_enabled = false;
    params.biomes_enabled = false;
    params.biome_relief_enabled = false;
    params.cliffs_enabled = false;
    params.rivers_enabled = false;
    params.lakes_enabled = false;
    params.structures_enabled = false;
    params.hydro_enabled = false;
    params.biome_table_path.clear();
    params.structures_data_dir.clear();
    return params;
}
} // namespace Luminumbra::Test
