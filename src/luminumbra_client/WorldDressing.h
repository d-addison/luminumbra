#pragma once

// world-dressing PLACEMENT COMPUTATION, extracted from the
// two one-time first-IN_GAME-frame bring-up blocks in main_client.cpp (the
// tree/rock/bush scatter and the ambient-wildlife spawn loop, together ~30 s of
// main-thread candidate probing in a debug build). The loops here are the legacy
// inline loops MOVED VERBATIM — same RNG construction, same seed derivation from
// the spawn anchor, same iteration order, same rejection tests — so the placement
// output is byte-identical to what the inline code produced. The world/species
// queries are injected as callbacks, which makes the computation pure and
// unit-testable (a test binds synthetic terrain; the client binds
// SHIELD_WorldSystem + the species registry) and lets it run on a background
// JobSystem worker (all bound queries are pure, thread-safe worldgen reads — the
// same sampling the meshing workers already run concurrently).
//
// RNG-STREAM PRESERVATION IS THE LOAD-BEARING CONSTRAINT. The tree, rock, and
// bush loops share ONE sequential SplitMix64 stream (rocks continue the stream
// after the trees, bushes after the rocks — main_client's comments always said
// so), so the whole scatter runs as ONE sequential computation and the shared
// stream state threads through the three Compute* calls explicitly. Never split
// the loops per-cell.
//
// placements are client decoration seeded from the spawn anchor and
// never feed world_hash (the inline code never did either). NO GL and NO entt
// here — the placement structs carry everything the main-thread consume needs to
// do the (cheap) GL upload / EnTT / physics registrations afterwards.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Luminumbra::Client {

// The EXACT world/species queries the legacy inline loops made, one callback per
// query shape. Every callback must be a pure, thread-safe read (they are invoked
// from a background job).
struct WorldDressingCallbacks {
    // ws->GetTerrainHeightAt(x, z): the analytic heightmap surface (pure, valid
    // before streaming — the reason trees could appear on the first ready frame).
    std::function<float(float, float)> terrain_height;
    // ws->WaterLevelAt(x, z): sea + perched-lake water level at a column.
    std::function<float(float, float)> water_level;
    // ws->get_density_at({x, y, z}): SDF read for the roofed-cave reject.
    // DENSITY CONVENTION ( root cause): SOLID = < 0; air = >= 0.
    std::function<float(float, float, float)> density_at;
    // biomes_enabled ? biome_table.vegetation_for(BiomeIdAt(x, z)).density
    // 0.3f — the per-biome vegetation density gating shrubs.
    std::function<float(float, float)> vegetation_density;
    // biome_table.name_for(BiomeIdAt(x, z)): the wildlife species-selection key.
    std::function<std::string(float, float)> biome_name;
    // Resolved index into the species roster for (biome, pick): the client binds
    // SelectForBiome plus the legacy pick-modulo-roster fallback; the placement
    // stores only the index (the consume side re-derives colors/flags from it).
    // Pure — SelectForBiome draws no RNG.
    std::function<int(const std::string&, std::size_t)> species_for_biome;
};

// Inputs resolved on the main thread at dispatch time. The scatter knobs
// themselves (reach/cell/caps/densities) stay as named constants inside the
// moved loops — they are part of the verbatim computation, not configuration.
struct WorldDressingParams {
    float anchor_x = 0.0f; // spawn anchor: seeds the scatter RNG + centres every loop
    float anchor_z = 0.0f;
    // Procgen palette sizes, pinned BEFORE dispatch (the palette builders are
    // GL-side and cheap; none of them touches the scatter RNG stream). The
    // placement stores palette_index = position-hash % count.
    int tree_palette_count = 0;
    int rock_palette_count = 0; // 0 = the legacy code skipped the ENTIRE rock loop
    int bush_palette_count = 0; // 0 = the legacy code skipped the ENTIRE bush loop
    // Ambient wildlife: computed only when interactive play + a species roster +
    // a successfully loaded rig were all present at dispatch time.
    bool compute_wildlife = false;
    int herd_count = 12; // render.creature_spawn SpawnHerdCount (config-resolved)
};

// One tree candidate that PASSED every rejection test, in loop order.
// palette_index is -1 when the tree palette was empty: the candidate still
// counted toward the cap and the log (legacy `placed`), but nothing is emitted.
struct TreePlacement {
    glm::vec3 position{0.0f};                   // (x, terrain h, z)
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // yaw about +Y
    float eff_scale = 1.0f;          // s * maturityScale * geneticSize (genome stream applied)
    std::int32_t palette_index = -1; // procgen://tree_<idx>
};

// One placed rock, in loop order. Position/scale carry the settle-into-ground
// offset and the Y-squash jitter already applied (they were RNG draws).
struct RockPlacement {
    glm::vec3 position{0.0f}; // y = h - 0.35 * s
    glm::vec3 scale{1.0f};    // (s, s * yJitter, s)
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    std::int32_t palette_index = 0; // procgen://rock_<idx>
};

// One placed shrub, in loop order (same shape as rocks; y = h - 0.12 * s).
struct BushPlacement {
    glm::vec3 position{0.0f};
    glm::vec3 scale{1.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    std::int32_t palette_index = 0; // procgen://bush_<idx>
};

// One wildlife spawn-loop emission, in loop order. A water-cell candidate emits
// a (capped) drinking-spot WaterHole instead of a creature; both kinds live in
// the SAME vector so the main-thread consume replays the legacy entity-creation
// order exactly. Creature fields carry every RNG draw the legacy loop made.
struct CreaturePlacement {
    enum class Kind : std::uint8_t {
        Creature,
        WaterHole
    };
    Kind kind = Kind::Creature;
    // Both kinds. Creature: (wx, ground + 1.2, wz) — the settle-onto-ground spawn
    // point (also the physics avatar spawn). WaterHole: (wx, waterLevel, wz).
    glm::vec3 position{0.0f};
    // Creature-only fields (defaulted/ignored for WaterHole):
    float yaw = 0.0f;               // spawn heading (angleAxis about +Y)
    std::int32_t species_index = 0; // resolved roster index (species_for_biome)
    float size = 1.0f;              // overall genome size multiplier
    glm::vec3 build_scale{1.0f};    // ComputeCreatureBuild proportions (x/y/z)
    float thirst = 0.0f;            // seeded initial thirst
    float anim_phase = 0.0f;        // unit draw; consume sets player.time = phase * 2.0
    bool female = false;            // alternate M/F (loop index parity)
};

// The full computation output — what the background job fills in.
struct WorldDressingResult {
    std::vector<TreePlacement> trees;
    std::vector<RockPlacement> rocks;
    std::vector<BushPlacement> bushes;
    std::vector<CreaturePlacement> wildlife; // empty when compute_wildlife is false
};

// The legacy scatter-RNG seed derivation (moved verbatim): the shared SplitMix64
// stream state all three scatter loops advance, seeded from the spawn anchor.
std::uint64_t InitialScatterRngState(float anchor_x, float anchor_z);

// The three scatter loops. `rng` is the SHARED stream state (in/out): call them
// in the legacy order — trees, then rocks, then bushes — threading the same
// state through, or the layout diverges from the legacy inline code.
std::vector<TreePlacement> ComputeTreePlacements(const WorldDressingParams& params,
                                                 const WorldDressingCallbacks& cbs,
                                                 std::uint64_t& rng);
std::vector<RockPlacement> ComputeRockPlacements(const WorldDressingParams& params,
                                                 const WorldDressingCallbacks& cbs,
                                                 std::uint64_t& rng);
std::vector<BushPlacement> ComputeBushPlacements(const WorldDressingParams& params,
                                                 const WorldDressingCallbacks& cbs,
                                                 std::uint64_t& rng);

// The ambient-wildlife candidate loop (its own DeterministicRng stream, seeded
// 0xFA0FA0/4242/1 exactly as the legacy loop did). NOT the EnTT/physics/skeleton
// work — that stays on the main thread at consume time.
std::vector<CreaturePlacement> ComputeWildlifePlacements(const WorldDressingParams& params,
                                                         const WorldDressingCallbacks& cbs);

// Convenience: the full legacy sequence (trees -> rocks -> bushes -> wildlife) as
// ONE sequential computation — the body of the single background job.
WorldDressingResult ComputeWorldDressing(const WorldDressingParams& params,
                                         const WorldDressingCallbacks& cbs);

} // namespace Luminumbra::Client
