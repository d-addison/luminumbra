#include "luminumbra/world/GenerationProfile.h"
#include "luminumbra/world/World.h"

namespace Luminumbra::World {

GenerationProfile::GenerationProfile() {
    // Define default settings for a biome
    BiomeProfile default_profile;
    default_profile.base_terrain.noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    default_profile.base_terrain.noise.fractal_type = FNL_FRACTAL_FBM;
    default_profile.base_terrain.noise.frequency = 0.001f;
    default_profile.base_terrain.noise.octaves = 2;
    default_profile.terrain_variance = 10.0f;

    default_profile.mountains.noise.noise_type = FNL_NOISE_OPENSIMPLEX2;
    default_profile.mountains.noise.fractal_type = FNL_FRACTAL_FBM;
    default_profile.mountains.noise.frequency = 0.0015f;
    default_profile.mountains.noise.octaves = 5;
    default_profile.mountains.amplitude = 30.0f;

    default_profile.caves.noise.noise_type = FNL_NOISE_PERLIN;
    default_profile.caves.noise.fractal_type = FNL_FRACTAL_RIDGED;
    default_profile.caves.noise.frequency = 0.02f;
    default_profile.caves.noise.octaves = 2;
    default_profile.caves.amplitude = 10.0f;

    // Assign this default profile to all biome types
    for (int i = 0; i <= static_cast<int>(BiomeType::SKY_VOID); ++i) {
        biome_profiles[static_cast<BiomeType>(i)] = default_profile;
    }

    // --- You can now override specific biomes for unique looks ---
    BiomeProfile& crystal_groves = biome_profiles.at(BiomeType::CRYSTAL_GROVES);
    crystal_groves.base_terrain.noise.frequency = 0.008f;
    crystal_groves.terrain_variance = 60.0f; // More jagged
    crystal_groves.mountains.amplitude = 150.0f;
    BiomeProfile& sunken_hollows = biome_profiles.at(BiomeType::SUNKEN_HOLLOWS);
    sunken_hollows.base_height = -20.0f; // Lower base altitude
    sunken_hollows.base_terrain.noise.frequency = 0.002f; // More subtle terrain
    sunken_hollows.terrain_variance = 20.0f; // Less variance
    sunken_hollows.mountains.amplitude = 50.0f; // More gentle hills
    sunken_hollows.caves.noise.frequency = 0.01f; // More frequent caves
    sunken_hollows.caves.amplitude = 20.0f; // More pronounced caves
    BiomeProfile& canopy_bridges = biome_profiles.at(BiomeType::CANOPY_BRIDGES);
    canopy_bridges.base_height = 50.0f; // Higher altitude
    canopy_bridges.base_terrain.noise.frequency = 0.005f; // More subtle terrain
    canopy_bridges.terrain_variance = 15.0f; // Less variance
    canopy_bridges.mountains.amplitude = 30.0f; // More gentle hills
    canopy_bridges.caves.noise.frequency = 0.02f; // Less frequent caves
    canopy_bridges.caves.amplitude = 10.0f; // Less pronounced caves
    BiomeProfile& sky_void = biome_profiles.at(BiomeType::SKY_VOID);
    sky_void.base_height = 100.0f; // Very high altitude
    sky_void.base_terrain.noise.frequency = 0.01f; // More subtle terrain
    sky_void.terrain_variance = 5.0f; // Very little variance
    sky_void.mountains.amplitude = 10.0f; // Very gentle hills
    sky_void.caves.noise.frequency = 0.05f; // Very infrequent
    sky_void.caves.amplitude = 5.0f; // Very subtle caves
}

void GenerationProfile::applyChanges(World& world) {
    // Implementation for the regenerate button
    world.regenerate();
}

} // namespace Luminumbra::World