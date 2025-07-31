#pragma once

#include "FastNoiseLite.h"
#include "luminumbra/world/Biome.h"
#include <vector>
#include <map>
#include <memory>

namespace Luminumbra::World {

class World;

// A single layer of noise (e.g., for continents, mountains, caves)
struct NoiseLayer {
    bool enabled = true;
    float amplitude = 1.0f;
    fnl_state noise = fnlCreateState();
};

// All parameters that define the shape of a single biome
struct BiomeProfile {
    // Primary terrain shape
    NoiseLayer base_terrain;
    // Layer for adding mountainous features
    NoiseLayer mountains;
    // 3D noise for carving caves
    NoiseLayer caves;

    float base_height = 10.0f;
    float terrain_variance = 30.0f;
    float cliff_factor = 0.0f; // How sharp/sheer the terrain is
};

// A full profile for the entire world, mapping each biome type to its settings
class GenerationProfile {
public:
    GenerationProfile(); // Constructor to set default values

    std::map<BiomeType, BiomeProfile> biome_profiles;

    // Global settings
    float water_level = 18.0f;
    float island_fade_start = -200.0f;
    float island_fade_end = -150.0f;

    // Method to regenerate the entire world after changes
    void applyChanges(World& world);
};

} // namespace Luminumbra::World