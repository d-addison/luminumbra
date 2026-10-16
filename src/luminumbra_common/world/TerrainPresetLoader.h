#pragma once

// the one canonical world-preset parser. Replaces the four duplicate
// parsers that previously lived in GameSession.cpp,
// test_worldgen_layer_snapshots.cpp, runtime_world_visual_validation_test.cpp
// and initial_world_loading_perf_test.cpp.
//
// Consumed parameters land in Systems::TerrainGenParams. Shaping is always
// applied; an absent terrain.shaping block uses the default shaping parameters.
// Biome and feature blocks retain their content controls. Unknown keys warn;
// retired algorithm selectors are errors.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "../systems/SHIELD_WorldSystem.h" // Systems::TerrainGenParams
#include "WorldgenFileReader.h"

namespace Luminumbra::world {

// Explicit preset revisions must match the current algorithm contract.
inline constexpr std::int64_t kTerrainPresetSchemaRevision = 7;
inline constexpr std::int64_t kMinTerrainPresetSchemaRevision = 6;

// generation_params.terrain.shaping — reserved keys pinned by
// (spline points
// are monotone piecewise-linear control points, [input, output] pairs).
struct TerrainShapingPreset {
    bool present = false; // block existed in the preset file
    float continentalness_frequency = 0.0008f;
    float erosion_frequency = 0.0015f;
    float peaks_frequency = 0.004f;
    float peaks_amplitude = 90.0f;
    float domain_warp_amplitude = 30.0f;
    float domain_warp_frequency = 0.006f;
    std::vector<std::array<float, 2>> continental_spline;
    std::vector<std::array<float, 2>> erosion_spline;
    std::vector<std::array<float, 2>> peaks_spline;
};

// generation_params.biomes. A preset opts INTO biomes by naming a
// table: "biomes": {"table": "common/biomes.json"}. The table path is relative
// to the data/ root and resolved to an absolute path against the preset's
// location at load time (presets live at <root>/worlds/atlas/presets/, data at
// <root>/data/). Revision 7 supplies the shipped table when omitted; revision 6
// keeps historical omission semantics. An explicit empty table always disables biomes.
struct TerrainBiomesPreset {
    bool present = false;
    bool enabled = true;                      // a non-empty "table" was supplied
    std::string table = "common/biomes.json"; // verbatim relative path from the preset
    std::string resolved_table_path;          // absolute UTF-8 path handed to TerrainGenParams
    float temperature_frequency = 0.005f;
    float humidity_frequency = 0.005f;
    bool relief_enabled = true; //  temperature-driven ridge scaling
    float relief_strength = 0.45f;
};

// generation_params.features flags beyond the cave params consumed through
// TerrainGenParams.
struct TerrainFeaturesPreset {
    bool present = false;
    bool rivers_enabled = true;
    bool structures_enabled = true;
};

// generation_params.terrain.hydro: hydraulic/thermal relief. A preset
// opts in with "hydro": {"enabled": true,...}. Defaults mirror
// TerrainGenParams' hydro_* defaults so a bare {"enabled": true} works.
struct TerrainHydroPreset {
    bool present = false;
    bool enabled = true;
    int iterations = 24;
    float cell_size_m = 8.0f;
    float talus_height = 1.2f;
    float thermal_rate = 0.5f;
    float rain_per_sweep = 0.02f;
    float solubility = 0.10f;
    float deposition = 0.10f;
    float evaporation = 0.20f;
    float sediment_capacity = 0.40f;
    float max_offset = 24.0f;
};

struct TerrainPresetExtras {
    TerrainShapingPreset shaping;
    TerrainBiomesPreset biomes;
    TerrainFeaturesPreset features;
    TerrainHydroPreset hydro;
};

struct TerrainPresetLoadResult {
    bool ok = false;
    // FNV-1a-64 of canonical resolved preset JSON and a sorted manifest of
    // logical data_root-relative paths, exact-byte file hashes and loader outcomes.
    std::string preset_content_hash;
    // params.worldgen_dependencies retains the loaded objects and manifest that
    // this identity describes, including absent/rejected-input fallback decisions.
    Systems::TerrainGenParams params;
    TerrainPresetExtras extras;
    std::vector<std::string> errors;
    std::vector<std::string> warnings; // unknown-key reports (also logged)
};

// Fill omitted feature choices using the declared revision (unversioned presets use
// historical revision 6). Explicit false and an empty biome table remain authoritative.
nlohmann::json ResolveTerrainPresetDefaults(const nlohmann::json& preset);

// Identity bytes: materialized revision defaults, current schema revision, sorted
// object keys, compact JSON, ASCII escapes and locale-independent binary64 numbers.
// No filesystem paths derived from data_root or provenance enter this form.
std::string CanonicalResolvedTerrainPresetJson(const nlohmann::json& preset);
// Identity with an empty dependency manifest (for dependency-free presets/fixtures).
// Runtime callers must use the loader's preset_content_hash to bind resolved files.
std::string TerrainPresetContentHash(const nlohmann::json& preset);

// Loads and validates a world preset JSON file. Validation contract matches
// the historical GameSession parser: generation_params, terrain and features
// must be objects, the six terrain noise fields must be numeric and
// caves_enabled/cave_frequency must be present and typed. On any error the
// result carries ok=false and human-readable messages; params/extras are only
// meaningful when ok=true. Absent optional worldgen inputs bind fallback markers;
// present but unreadable inputs are errors.
TerrainPresetLoadResult LoadTerrainPreset(const std::filesystem::path& preset_path);

// Explicit CLI preset override, validated against the runtime data root. Failure leaves
// json_text unchanged; successful text is passed to GameSession::CreateWorld.
bool LoadTerrainPresetOverride(const std::filesystem::path& preset_path,
                               const std::filesystem::path& data_root,
                               std::string& json_text,
                               std::string& error);

//   (ADDITIVE in-memory seam — no struct/behavior change). Parses
// an ALREADY-PARSED preset JSON with an EXPLICIT data root, so a host that holds
// candidate preset JSON in memory (the create-world live preview) gets identical
// TerrainGenParams WITHOUT round-tripping through a temp file (the temp-file
// route resolves the biome/structure table paths against the wrong data root —
// they are resolved relative to <data_root> here). `data_root` is the absolute
// path to the `data/` directory (the same root the on-disk loader derives as
// four parents up from the preset file + "data"). `provenance` only labels the
// unknown-key warnings. LoadTerrainPreset(path) reads+parses the file and
// DELEGATES here with the file-derived data root. Identity binds the selected
// biome table and enabled structure placement/piece files through portable paths.
// The status reader queries links themselves and permits deterministic I/O failure coverage.
TerrainPresetLoadResult LoadTerrainPresetFromJson(
    const nlohmann::json& data,
    const std::filesystem::path& data_root,
    const std::string& provenance = "<memory>",
    const World::WorldgenFileStatusReader& read_status = World::ReadWorldgenSymlinkStatus);

} // namespace Luminumbra::world
