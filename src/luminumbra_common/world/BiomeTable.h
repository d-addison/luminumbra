#pragma once

// Biome selection table (T-I4-1, design-decisions.md section 2).
//
// The engine knows ONLY the lookup: biome_id = BiomeTable::lookup(
//   continentalness, erosion, pv, temperature, humidity). The climate ranges
// and surface material palettes are GAME DATA loaded from
// data/common/biomes.json (engine/game split: the engine never names a
// concrete biome). The five climate dimensions REUSE the existing +3/+4/+5
// shaping control noises (continentalness/erosion/peaks-valleys) and add the
// +8 temperature / +9 humidity climate noises.
//
// Selection contract:
//   - climate ranges are [min, max) in the normalized noise domain [-1, 1];
//     an absent dimension matches the full span;
//   - overlapping ranges resolve FIRST-MATCH in declaration order (documented);
//   - a column matching no biome row yields kNoBiome (255), and the caller
//     falls back to the legacy single-material classifier.
//
// Determinism: ComputeContentHash() is an fnv1a64 over the CANONICALIZED table
// (id, climate ranges quantized to a fixed integer grid, palette material ids)
// in declaration order. T-I4-2 mixes this hash into ComputeTerrainParamsHash so
// pristine far-LOD tiles self-invalidate when the table content changes.

#include "../../../include/luminumbra/core/Types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::World {

// Sentinel: the column matched no authored biome.
constexpr u8 kNoBiome = 255u;

// One climate dimension range, [min, max) in the normalized noise domain.
struct BiomeClimateRange {
    float min = -1.0f;
    float max = 1.0f;

    bool contains(float value) const {
        // Inclusive-min, exclusive-max, except the domain ceiling 1.0 is
        // inclusive so a value sitting exactly at the top of the noise range
        // still resolves a biome.
        return value >= min && (value < max || (max >= 1.0f && value <= max));
    }
};

// Surface material palette: which MaterialType id is laid at the top of the
// column, just below it (filler), deeper (depth), and below the waterline.
struct BiomeSurfacePalette {
    u8 top = static_cast<u8>(MaterialType::Grass);
    u8 filler = static_cast<u8>(MaterialType::Soil);
    u8 depth = static_cast<u8>(MaterialType::Stone);
    u8 underwater = static_cast<u8>(MaterialType::Sand);
};

// T-I4-5: per-biome environmental-audio reverb. Consumed by the client
// EnvironmentalAudioSystem to set the listener's reverb profile by biome. The
// preset name is game-authored (the engine treats it as an opaque label); the
// numeric wet/dry/decay drive the audio environment directly.
struct BiomeReverb {
    std::string preset = "open_field";
    float wet = 0.2f;
    float dry = 0.8f;
    float decay = 1.0f;
};

// T-I5b-1 (F1): per-biome vegetation/cover. Parsed in iteration 4
// (parsed-not-consumed); now CONSUMED render-side by the FoliagePass scatter
// density. The scatter names are game-authored opaque labels (the engine only
// reads `density`); like reverb, vegetation is RENDER/CLIENT-only and is
// DELIBERATELY NOT mixed into compute_content_hash() (the content hash gates the
// terrain far-LOD cache; foliage is render-only and must not invalidate tiles or
// perturb world_hash).
struct BiomeVegetation {
    float density = 0.0f; // [0,1] cover fraction driving the foliage scatter
    std::vector<std::string> scatter; // opaque archetype labels (game content)
};

struct BiomeDefinition {
    u8 id = kNoBiome;
    std::string name;
    BiomeClimateRange continentalness;
    BiomeClimateRange erosion;
    BiomeClimateRange peaks_valleys;
    BiomeClimateRange temperature;
    BiomeClimateRange humidity;
    BiomeSurfacePalette palette;
    BiomeReverb reverb;
    BiomeVegetation vegetation;
};

class BiomeTable {
public:
    // Loads and parses data/common/biomes.json. On any structural error the
    // result is empty() and errors() carries human-readable messages. Unknown
    // keys (including the parsed-not-consumed vegetation/reverb blocks) emit
    // warnings, never errors (loader discipline matches TerrainPresetLoader).
    static BiomeTable Load(const std::filesystem::path& table_path);

    bool ok() const { return m_ok; }
    bool empty() const { return m_biomes.empty(); }
    std::size_t size() const { return m_biomes.size(); }
    const std::vector<BiomeDefinition>& biomes() const { return m_biomes; }
    const std::vector<std::string>& errors() const { return m_errors; }
    const std::vector<std::string>& warnings() const { return m_warnings; }

    // First-match biome lookup. Returns kNoBiome (255) when no row matches.
    u8 lookup(float continentalness,
              float erosion,
              float peaks_valleys,
              float temperature,
              float humidity) const;

    // Palette for a biome id; returns the default palette for kNoBiome / any
    // unknown id so callers never need a separate guard.
    const BiomeSurfacePalette& palette_for(u8 biome_id) const;

    // T-I4-5: reverb profile for a biome id; returns the default profile for
    // kNoBiome / any unknown id so callers never need a separate guard.
    const BiomeReverb& reverb_for(u8 biome_id) const;

    // T-I5b-1: vegetation/cover for a biome id; returns the default (zero
    // density) for kNoBiome / any unknown id. Render-only (foliage scatter).
    const BiomeVegetation& vegetation_for(u8 biome_id) const;

    // fnv1a64 over the canonicalized table content (see header note).
    u64 content_hash() const { return m_content_hash; }

private:
    std::vector<BiomeDefinition> m_biomes;
    // id -> index into m_biomes (255 entries, kNoBiome means absent). Built at
    // load time so palette_for is O(1).
    std::array<u8, 256> m_id_to_index{};
    BiomeSurfacePalette m_default_palette{};
    BiomeReverb m_default_reverb{};
    BiomeVegetation m_default_vegetation{};
    std::vector<std::string> m_errors;
    std::vector<std::string> m_warnings;
    u64 m_content_hash = 0;
    bool m_ok = false;

    void rebuild_index();
    u64 compute_content_hash() const;
};

} // namespace Luminumbra::World
