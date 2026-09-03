#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../core/JobSystem.h"
#include "../world/BiomeTable.h"
#include "../world/Chunk.h"
#include "../world/FarLodStore.h"
#include "../world/StructurePlacement.h"
#include "FastNoise/FastNoise.h"
#include "entt/entt.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Luminumbra::Systems {

class PhysicsSystem;
class WaterSystem;
class WeatherSystem; // Weather-driven rain passthrough.
struct TerrainGenParams {
    float base_frequency = 0.01f;
    float base_amplitude = 50.0f;
    int octaves = 4;
    float persistence = 0.5f;
    float lacunarity = 2.0f;
    float height_offset = 0.0f;

    bool caves_enabled = true;
    float cave_frequency = 0.02f;
    float cave_threshold = 0.7f;
    float cave_carve_value = 2.0f;
    // cave STYLE. 0 = legacy single 3D-Perlin BODY threshold (cheese-only,
    // byte-identical to all pre-existing worlds). 1 = noise-router (Minecraft-1.18-style):
    // the legacy cheese rooms PLUS spaghetti winding tunnels carved at the zero-crossing
    // EDGE of a second Perlin (abs(noise) < thickness => air) so caves become connected
    // networks, not isolated bubbles. Opt-in per preset; legacy stays byte-identical.
    int cave_style = 0;
    float spaghetti_frequency = 0.04f; // tunnel noise scale (higher = tighter winding)
    float spaghetti_thickness = 0.08f; // |noise| < this carves a tunnel (wider = fatter)
    float worley_frequency = 0.016f;   // CHEESE cavern cell scale (lower = bigger rooms)
    float worley_threshold =
        0.62f; // / < this => open room (cell interior); higher = more/bigger caverns

    // ---  surface-breaking caves / sinkholes / cave-mouths (default-off) ---
    // Makes the 18 m surface cap (kCaveSurfaceCapDepth) a PER-COLUMN field so the
    // EXISTING cave noise reaches the surface ONLY inside hashed feature
    // footprints, plus a bounded analytic sinkhole carve. Placement is a pure
    // unsigned fn of integer doline-cell coords + seed (SplitMix64 + CellSeed with
    // a distinct salt), combined with caves via commutative+associative ops only
    // (hard max / exp-smin) so chunk seams and the SIMD/scalar paths cannot
    // diverge. The carve is VISUAL/terrain-field only (the integer sim reads the
    // resulting density classification, never the float carve math), same rule as
    // procedural trees. When surface_breaks_enabled == false every path takes the
    // ORIGINAL float-op sequence (sample_surface_breaks returns {18,0} and the
    // helpers fall through), so the output is byte-identical -> world_hash
    // unchanged. World_hash-affecting when enabled -> deliberate re-pin.
    //   feature_cell_size  -- doline cell grid pitch (~128 m); a 3x3 neighbor scan
    //                         suffices because max_feature_radius < feature_cell_size.
    //   max_feature_radius -- finite support clamp on a feature's surface footprint.
    //   carve_smoothness   -- exp-smin k for the cone/capsule lip (0 => hard max).
    //   entrance_min_cap   -- floor the per-column cap drops to inside a feature
    //                         (0 => cave noise fully exposed; small positive keeps a thin lip).
    bool surface_breaks_enabled = false;
    float surface_break_density = 0.0f; // Bernoulli accept prob per doline cell
    float feature_cell_size = 128.0f;   // doline cell pitch (m)
    float max_feature_radius = 60.0f;   // MUST be < feature_cell_size (asserted)
    float carve_smoothness = 0.0f;      // exp-smin k (0 => hard max)
    float entrance_min_cap = 0.0f;      // metres the cap floors to inside a feature

    bool island_mask_enabled = false;
    float island_mask_frequency = 0.004f;

    // ---  terrain shaping (default-off) ---
    // With shaping_enabled == false every height path is bit-identical to the
    // pre-shaping implementation (legacy regression hashes in
    // test_worldgen_layer_snapshots.cpp prove it). When enabled, three 2D
    // control channels modulate the base FBM detail channel:
    //   continentalness (m_seed + 3) -> continental_spline -> base elevation
    //   erosion         (m_seed + 4) -> erosion_spline     -> amplitude mult
    //   peaks/valleys   (m_seed + 5) -> peaks_spline       -> ridge term
    // plus a 2-channel simplex domain warp (m_seed + 6 / + 7) applied to the
    // BASE noise (and pv) sample coordinates. Seed offsets are pinned by the
    //  seed registry (the deterministic runtime contract section 2).
    // Splines are monotone piecewise-linear [input, output] control points
    // evaluated with plain lerp + endpoint clamping (no smoothstep, so the
    // scalar and batch paths cannot diverge).
    bool shaping_enabled = false;
    float continentalness_frequency = 0.0008f;
    float erosion_frequency = 0.0015f;
    float peaks_frequency = 0.004f;
    float peaks_amplitude = 90.0f;
    float domain_warp_amplitude = 30.0f;
    float domain_warp_frequency = 0.006f;
    std::vector<std::array<float, 2>> continental_spline;
    std::vector<std::array<float, 2>> erosion_spline;
    std::vector<std::array<float, 2>> peaks_spline;

    // ---  biome selection (default-off) ---
    // Biomes are opt-in via the preset block "biomes": {"table": "..."}. When
    // biomes_enabled is false the world system never builds the temperature
    // (+8) / humidity (+9) climate noises and never loads a biome table, so
    // every height AND material path is bit-identical to the pre-biome
    // implementation (the worldgen snapshot/hash fixtures prove byte-zero
    // drift). biome_table_path is relative to data/ (e.g. "common/biomes.json").
    //  mixes the loaded table's content hash into ComputeTerrainParamsHash
    // so pristine far-LOD tiles self-invalidate on a table content change.
    bool biomes_enabled = false;
    std::string biome_table_path;
    float temperature_frequency = 0.005f;
    float humidity_frequency = 0.005f;
    // Per-biome morphology: when enabled, the peaks/ridge term is scaled
    // by the (continuous) temperature field — cold/alpine ground gets taller, more
    // rugged peaks; warm lowlands stay gentler — so biomes differ in SHAPE, not
    // just material. Smooth (climate noise is continuous) so there are no seams,
    // and it only touches the ridge term (base relief unchanged) to keep the
    // hypsometric/spectral realism gate in band. Disabled -> byte-zero drift.
    bool biome_relief_enabled = false;
    float biome_relief_strength = 0.45f; // ridge scale span: warm *(1-s).. cold *(1+s)

    // Cliffs / escarpments: in cliff-zones (a smooth noise mask, seed
    // +12) the shaped height snaps toward flat benches with steep risers, so the
    // marching-cubes surface forms mesa/canyon cliff faces. Pure heightfield ->
    // no holes / floating geometry, fully deterministic. Blended by the mask so
    // cliff zones sit naturally amid normal foothills. Disabled -> byte-zero drift.
    bool cliffs_enabled = false;
    float cliff_frequency = 0.0011f; // cliff-zone mask feature scale
    float cliff_threshold = 0.4f; // mask value above which terracing engages (matches default.json)
    float cliff_step = 11.0f;     // metres per bench (cliff face height)

    // ---  PV-band rivers (default-off) ---
    // Chunk-local river carve on the +10 ridged noise (seed registry). The
    // folded PV value PV = 1 - |3*|r| - 2| (Minecraft 1.18 weirdness->PV) of
    // the +10 noise selects the rivers where it falls in the VALLEYS band
    // [river_pv_min, river_pv_max] = [-1.0, -0.85] (research Area 1); the same
    // +10 noise modulates width/wobble. Carving lowers the terrain floor below
    // SEA_LEVEL so the EXISTING global water plane fills the channel (no
    // WaterSystem changes; regression review). Bank material is the biome filler.
    // Applied inside ComputeShapedHeight so near chunks AND far tiles agree at
    // the seam. With rivers_enabled false the height path is bit-identical to
    // pre-river generation (byte-zero drift).
    bool rivers_enabled = false;
    float river_frequency = 0.0016f;
    float river_pv_min = -1.0f;    // valleys band lower edge (folded PV)
    float river_pv_max = -0.85f;   // valleys band upper edge (folded PV)
    float river_depth = 8.0f;      // metres the channel floor sits below SEA_LEVEL
    float river_max_carve = 60.0f; // clamp on terrain lowered into the channel
    // Lakes (elevation lakes): where the lake field (smooth FBM, seed +11) exceeds
    // lake_threshold, the terrain is carved into a basin BELOW the LOCAL surface
    // level (ContinentalBaseHeight - lake_bank_offset), not below SEA_LEVEL — so a
    // basin on a mountain or in a valley gets a lake at THAT elevation (alpine
    // tarns, valley pools), held by its higher rim. WaterLevelAt returns that local
    // surface for the water mesh + the WaterSystem rest level (which pins the lake
    // so the flow sim can't drain it). Sea-level oceans still form wherever the
    // continental base dips below SEA_LEVEL. Disabled -> byte-zero drift.
    bool lakes_enabled = false;
    float lake_frequency = 0.0009f; // ~1100 m feature scale
    float lake_threshold = 0.52f;   // lake field value above which a lake forms (moderate)
    float lake_depth = 5.0f;        // metres the basin floor sits below the lake surface
    float lake_max_carve = 22.0f;   // clamp on terrain lowered into the basin
    float lake_bank_offset = 2.0f;  // lake surface sits this far below the local land level (banks)
    // fnv1a64 of the canonicalized biome table content, stamped by the world
    // system when it loads the table (0 when biomes are disabled or the table
    // failed to load). ComputeTerrainParamsHash mixes this in so pristine
    // far-LOD tiles self-invalidate when the table content changes even though
    // the path is unchanged.
    u64 biome_table_content_hash = 0;
    // structures. When enabled, the world system places jigsaw
    // structures (data/common/structures/<type>/) through the normal edit path
    // during chunk generation. structures_content_hash is the fnv1a64 of the
    // loaded template pools' content; ComputeTerrainParamsHash mixes it in (only
    // when enabled) so pristine far tiles self-invalidate on a template change.
    // Disabled (the default preset) => byte-zero drift, identical hashes.
    bool structures_enabled = false;
    u64 structures_content_hash = 0;
    // Absolute path to data/common/structures (resolved by the preset loader,
    // mirrors biome_table_path). The world system loads every <type>/ pool here
    // when structures_enabled.
    std::string structures_data_dir;

    // hydraulic/thermal RELIEF. When enabled, a deterministic per-region
    // erosion bake (HydraulicErosion) carves drainage/talus into the analytic
    // surface; ComputeShapedHeightSample adds the baked offset so EVERY height
    // consumer (collision/spawn/water/far-LOD/mesh) sees the eroded surface
    // (decision a). Distinct from the analytic `erosion` CONTROL noise (seed +4,
    // erosion_spline) which only modulates amplitude -- this is an absolute
    // height offset in metres. Disabled (the default preset) => byte-zero drift,
    // identical hashes (ComputeShapedHeightSample/Grid skip the lookup, marker
    // 0x06 is not mixed). World_hash-affecting when enabled -> deliberate bump.
    bool hydro_enabled = false;
    int hydro_iterations = 24;       // erosion sweeps (also the halo cell radius)
    float hydro_cell_size_m = 8.0f;  // erosion grid resolution (coarse macro relief)
    float hydro_talus_height = 1.2f; // thermal stable per-cell delta (m, at cell size)
    float hydro_thermal_rate = 0.5f;
    float hydro_rain_per_sweep = 0.02f;
    float hydro_solubility = 0.10f;
    float hydro_deposition = 0.10f;
    float hydro_evaporation = 0.20f;
    float hydro_sediment_capacity = 0.40f;
    float hydro_max_offset = 24.0f; // clamp |offset| (m)
};

// Immutable value data handed to far-LOD workers.  The capture owns these
// vectors and never exposes a mutable streamed Chunk beyond the owner thread.
struct FarLodSdfSnapshotEntry {
    IVec3 coords{};
    ChunkSdfProvenance provenance = ChunkSdfProvenance::GeneratedCurrentParams;
    u32 voxel_revision = 0;
    bool authority_durable = true;
    std::vector<f32> sdf_data;
    std::vector<u8> material_data;

    World::FarLodSdfSnapshot as_reduction_snapshot() const {
        World::FarLodSdfSnapshot snapshot;
        snapshot.coords = coords;
        snapshot.revision = voxel_revision;
        snapshot.source_kind = provenance == ChunkSdfProvenance::LoadedOrEdited
                                   ? World::FarLodBrickSourceKind::Authoritative
                                   : World::FarLodBrickSourceKind::RegenerableCache;
        snapshot.sdf_data = sdf_data;
        snapshot.material_data = material_data;
        return snapshot;
    }
};

struct FarLodSdfSnapshot {
    u64 capture_epoch = 0;
    u64 params_hash = 0;
    u64 authority_revision = 0;
    u64 region_authority_revision = 0;
    std::vector<FarLodSdfSnapshotEntry> entries;
};

struct WorldGenLayerSample {
    Vec3 world_pos{0.0f};

    float base_noise = 0.0f;
    float base_height = 0.0f;

    float island_noise = 0.0f;
    float island_mask = 1.0f;
    float final_height = 0.0f;

    float terrain_density = 0.0f;
    float cave_noise = 0.0f;
    float cave_value = 0.0f;
    float cave_density = 0.0f;
    float final_density = 0.0f;

    bool island_applied = false;
    bool caves_applied = false;
    bool solid = false;
    MaterialType material = MaterialType::Air;
};

struct ChunkLOD {
    int level;      // The LOD identifier (0 = highest detail)
    int step;       // The step size for Marching Cubes (1, 2, 4, etc.)
    float distance; // The maximum camera distance at which this LOD is used
};

class WaterSystem;

class SHIELD_WorldSystem {
public:
    struct StreamingBudgetFrameStats {
        int update_interval_frames = 0;
        int requested_render_radius = 0;
        int target_render_radius = 0;
        int generation_budget = 0;
        int meshing_budget = 0;
        std::size_t max_active_chunks_budget = 0;
        std::size_t active_chunks_before = 0;
        std::size_t active_chunks_after = 0;
        std::size_t ready_chunks = 0;
        std::size_t renderable_chunks = 0;
        std::size_t idle_chunks = 0;
        std::size_t loading_chunks = 0;
        std::size_t meshing_chunks = 0;
        std::size_t target_surface_columns = 0;
        std::size_t generation_candidates = 0;
        std::size_t surface_generation_candidates = 0;
        std::size_t vertical_generation_candidates = 0;
        std::size_t scheduled_generation = 0;
        std::size_t surface_generation_scheduled = 0;
        std::size_t vertical_generation_scheduled = 0;
        std::size_t deferred_generation = 0;
        std::size_t meshing_candidates = 0;
        std::size_t scheduled_meshing = 0;
        std::size_t deferred_meshing = 0;
        std::size_t unloaded_chunks = 0;
        bool generation_job_active = false;
        bool meshing_job_active = false;
    };

    struct RuntimeChunkStats {
        std::size_t total_chunks = 0;
        std::size_t unloaded_chunks = 0;
        std::size_t loading_chunks = 0;
        std::size_t idle_chunks = 0;
        std::size_t meshing_chunks = 0;
        std::size_t ready_chunks = 0;
        std::size_t unloading_chunks = 0;
        std::size_t renderable_chunks = 0;
        std::size_t collision_chunks = 0;
        std::size_t terrain_vertex_count = 0;
        std::size_t terrain_index_count = 0;
        std::size_t water_vertex_count = 0;
        std::size_t water_index_count = 0;
        std::size_t terrain_payload_bytes = 0;
        // Voxel-field storage telemetry: resident SDF/heightmap
        // bytes plus the count of chunks generated surface-band-only (empty
        // SDF), proving the step>1 SDF skip is active and sizing its savings.
        std::size_t sdf_payload_bytes = 0;
        std::size_t heightmap_payload_bytes = 0;
        std::size_t sdf_skipped_chunks = 0;
        bool generation_job_active = false;
        bool meshing_job_active = false;
    };

    struct StreamingTelemetryStats {
        std::size_t peak_queue_depth = 0;
        std::size_t peak_meshing_candidates = 0;
        std::size_t cumulative_scheduled_meshing = 0;
        std::size_t cumulative_deferred_meshing = 0;
        uint64_t max_deferred_age_frames = 0;
        std::size_t last_queue_depth = 0;
        //  the SETTLED backlog floor — the minimum
        // queue depth observed across the trailing activation window
        // (STREAMING_ACTIVATION_INTERVAL_FRAMES). Chunk activation runs only
        // every Nth frame (decoupled from the frame rate), so generation/loading
        // arrives in periodic batches that drain over the next few frames. The
        // raw last_queue_depth is a single mid-cycle snapshot: it reads the
        // in-flight generation batch (~one ring of loading chunks) whenever the
        // run's final frame happens to land on or just after an activation tick,
        // even though the pipeline reaches EMPTY every cycle. settled_queue_depth
        // is the trailing-window minimum, so it is 0 iff the backlog genuinely
        // drains to empty within each activation period (bounded + draining) and
        // is nonzero only if a standing backlog never empties (truly unbounded).
        // Pure telemetry: never fed into world_hash.
        std::size_t settled_queue_depth = 0;
        uint64_t frames_observed = 0;
    };

    struct CameraLocalCoverageStats {
        Vec3 camera_position{0.0f};
        IVec3 camera_chunk{0};
        IVec3 surface_chunk_under_camera{0};
        int horizontal_radius = 0;
        float terrain_height_under_camera = 0.0f;
        float camera_height_above_terrain = 0.0f;
        std::size_t expected_surface_chunks = 0;
        std::size_t present_surface_chunks = 0;
        std::size_t missing_surface_chunks = 0;
        std::size_t unloaded_surface_chunks = 0;
        std::size_t loading_surface_chunks = 0;
        std::size_t idle_surface_chunks = 0;
        std::size_t meshing_surface_chunks = 0;
        std::size_t ready_surface_chunks = 0;
        std::size_t renderable_surface_chunks = 0;
        std::size_t collision_surface_chunks = 0;
        std::size_t pending_lod_chunks = 0;
        std::array<std::size_t, 3> lod_counts{0u, 0u, 0u};
        std::size_t lod_unknown_chunks = 0;
        bool center_chunk_present = false;
        bool center_chunk_renderable = false;
        bool near_field_renderable = false;
    };

    // Sim-side frustum surface coverage (, player_view_smoke gate):
    // for every horizontal column within max_distance of the camera whose
    // 5-point surface span (column center + 4 footprint corners) intersects
    // the view frustum, is the owning chunk streamed and renderable? The
    // span is sampled directly from GetTerrainHeightAt (pure function of
    // seed/params), so the metric is independent of the streaming policy it
    // measures. frustum_planes are inward-facing (ax+by+cz+d >= 0 inside),
    // e.g. Gribb-Hartmann extraction from the camera view-projection.
    struct FrustumSurfaceCoverageStats {
        std::size_t columns_considered = 0;
        std::size_t expected_chunks = 0;
        std::size_t present_chunks = 0;
        std::size_t missing_chunks = 0;
        std::size_t renderable_chunks = 0;
        double renderable_ratio = 1.0;
    };
    FrustumSurfaceCoverageStats
    get_frustum_surface_coverage_stats(const Vec3& camera_position,
                                       const std::array<Vec4, 6>& frustum_planes,
                                       float max_distance) const;

    SHIELD_WorldSystem(JobSystem* job_system,
                       WaterSystem* water_system,
                       const TerrainGenParams& params,
                       int seed);
    ~SHIELD_WorldSystem();

    // Generates voxel data for a chunk. target_step is the meshing step this
    // generation must support: step <= 1 (default) generates the full 17^3
    // SDF + heightmap (LOD0 marching cubes, collision, edits, persistence);
    // step > 1 generates ONLY the 17x17 heightmap, leaving sdf_data empty and
    // skipping the 3D cave-noise grid. Such newly generated coarse chunks use
    // the heightfield fallback; a resident exact full SDF is instead meshed at
    // its requested coarse stride. All existing callers default to full
    // generation.
    void GenerateChunkData(::Luminumbra::Chunk& chunk, int target_step = 1) const;
    // stamp authored structure voxels into a freshly generated FULL-RES
    // chunk (sdf_data populated, step <= 1). Enumerates every structure site
    // whose footprint can reach this chunk (chunk AABB padded by each pool's
    // footprint_radius), drops each site to a single integer floor of the
    // surface (computed once per site, identical across all chunks/paths), skips
    // sub-SEA_LEVEL sites, then for each in-bounds assembled voxel sets
    // sdf_data = -1 (solid) and lazily-allocated material_data = voxel.material.
    // No-op (and no allocation) when structures are disabled or sdf_data is empty
    // (coarse step>1 path), so pristine/structures-off worlds stay byte-identical.
    // base_pos is the chunk's world-voxel origin (chunk.get_coords * CHUNK_SIZE).
    void StampStructuresIntoChunk(::Luminumbra::Chunk& chunk, const IVec3& base_pos) const;
    // stream around MULTIPLE anchors (multi-player / multi-camera). The
    // wanted-set is the UNION of each anchor's disc; the shared active-chunk budget and
    // eviction use distance-to-CLOSEST-anchor. The single-anchor overload below forwards
    // to this, so existing callers + single-anchor behaviour are byte-identical (one
    // anchor -> one disc, identical eviction). Streaming sets RESIDENCY only, not chunk
    // content, so world_hash is unaffected for a given anchor set.
    void update(entt::registry& registry,
                const std::vector<Vec3>& anchor_positions,
                PhysicsSystem* physics_system);
    void
    update(entt::registry& registry, const Vec3& camera_position, PhysicsSystem* physics_system);
    std::vector<::Luminumbra::Chunk*> get_renderable_chunks();
    float get_density_at(const Vec3& world_pos) const;
    float GetTerrainHeightAt(float world_x, float world_z) const;
    // the single cave-carve composition point. Samples the cheese BODY noise
    // (byte-identical to the legacy path) and, in noise-router style (cave_style==1), adds
    // spaghetti EDGE tunnels, composed via the existing order-free smax. Called identically
    // by every density site (mesh / collision / query) so they cannot disagree.
    float EvaluateCaveDensity(const Vec3& world_pos,
                              float terrain_density,
                              float effective_cap,
                              float feature_carve,
                              const float* precomputed_cheese = nullptr) const;
    //  locate the LARGEST doline / surface-break (cave mouth) within
    // `scan_radius_m` of a point. Pure read of the deterministic placement; used to aim a
    // camera at a dramatic cave opening (dolines follow a power-law, so most are small).
    // found=false when surface breaks are disabled or none in range.
    struct SurfaceBreakInfo {
        bool found = false;
        float x = 0.0f, z = 0.0f; // world centre of the doline
        float radius = 0.0f;      // surface footprint radius (m)
        float depth = 0.0f;       // funnel depth (m)
        bool shaft = false;       // true => vertical cenote shaft
    };
    SurfaceBreakInfo FindLargestSurfaceBreak(float near_x, float near_z, float scan_radius_m) const;
    // Elevation-aware water surface level at a column: the local lake surface
    // inside a lake basin (held above the carved floor by its rim), or SEA_LEVEL
    // elsewhere. The WaterSystem seeds + pins its per-cell rest level from this so
    // lakes sit at their basin elevation (mountain/valley) and never drain. Pure.
    float WaterLevelAt(float world_x, float world_z) const;
    // coarse-LOD height. Identical to
    // GetTerrainHeightAt for step <= 1 (full-res byte-identical). For step > 1
    // the river carve - and ONLY the carve - is anti-aliased over the
    // sample_step footprint so a sub-step-width channel produces a shallow dip
    // proportional to its coverage instead of an isolated full-depth notch. A
    // point-sampled narrow channel otherwise aliases into a one-sample pit at
    // 4 m / 8 m far-LOD steps, and the far/coarse heightfield mesher then emits
    // a tall near-vertical sliver triangle across it (the FarLodHorizon defect).
    // The base (un-carved) terrain is low-frequency and already band-limited, so
    // only the high-frequency carve needs this; the perimeter skirts mask the
    // residual full-res-ring vs coarse-tile seam, as documented in the mesher.
    float GetTerrainHeightAtCoarse(float world_x, float world_z, int sample_step) const;
    WorldGenLayerSample SampleWorldGenLayers(const Vec3& world_pos) const;

    // ---  biome selection ---
    // Whether biomes are active (preset opted in AND the table loaded).
    bool biomes_enabled() const {
        return m_biomes_enabled;
    }
    const World::BiomeTable& biome_table() const {
        return m_biome_table;
    }

    // ---  structures ---
    bool structures_enabled() const {
        return m_structures_enabled;
    }
    const std::vector<World::StructureTemplatePool>& structure_pools() const {
        return m_structure_pools;
    }
    // Nearest structure site of the given type to (world_x, world_z) within the
    // search radius (cells), or nullopt. Deterministic; pure function of the
    // world seed + the type's grid. Used by gates and locate(type, near).
    std::optional<World::StructureSite> LocateStructure(const std::string& type,
                                                        int world_x,
                                                        int world_z,
                                                        int search_radius_cells = 2) const;
    // Per-column biome id at the surface (u8, 255 = none). Pure function of
    // (seed, params): samples the five climate dimensions (continentalness,
    // erosion, peaks/valleys reuse the +3/+4/+5 shaping noises; temperature
    // +8, humidity +9) and resolves the first matching biome row. Returns
    // kNoBiome (255) when biomes are disabled or no row matches, so callers
    // fall back to the legacy single-material classifier.
    u8 BiomeIdAt(float world_x, float world_z) const;

    // environmental-audio reverb profile for the biome at a column.
    // Returns the active biome's reverb when biomes are enabled, else the
    // default profile. Pure function of (seed, params, table).
    const World::BiomeReverb& BiomeReverbAt(float world_x, float world_z) const;

    //  biome-aware surface material. Given a solid surface sample (its
    // world Y, the column's final terrain height, and the column biome id from
    // BiomeIdAt), returns the palette material for the band the sample sits in:
    //   below the waterline -> palette.underwater
    //   depth < 1 m (top)   -> palette.top
    //   depth < 5 m (filler)-> palette.filler
    //   deeper (depth)      -> palette.depth
    // With biome_id == kNoBiome (biomes disabled or unmatched) this reproduces
    // the legacy Sand/Grass/Soil/Stone classifier BIT-FOR-BIT, so disabled
    // worlds keep byte-zero drift. The plains palette maps to exactly the
    // legacy materials, so a plains column is also unchanged.
    // river_bank: when true, an above-water surface skin that would be
    // the biome `top` is laid as the biome `filler` instead - the exposed muddy
    // bank along a carved river channel.
    MaterialType SurfaceMaterialForColumn(float world_y,
                                          float final_height,
                                          u8 biome_id,
                                          bool river_bank = false) const;

    // surface-skin material for a column whose final
    // terrain height is already known (e.g. the coarse heightfield mesher just
    // read it from the cached heightmap). Returns BYTE-IDENTICAL results to
    // GetTerrainMaterialAt(Vec3(world_x, terrain_height - 0.1, world_z)) for a
    // surface vertex, but WITHOUT re-evaluating the shaped height (which the
    // caller already has) - it reproduces that helper's exact two-stage band
    // selection (solid-branch classification at depth 0.35 m, then the
    // Air/Water reclassification at depth 0.1 m) using the supplied height.
    // Valid for the live coarse mesher's surface samples, where caves never
    // carve (the 18 m surface cap makes the 0.35 m-deep sample always solid).
    MaterialType SurfaceVertexMaterial(float world_x, float world_z, float terrain_height) const;

    // SIMD-batched material classification for a list of
    // isosurface vertex world positions (the LOD0 marching-cubes mesher emits
    // hundreds per chunk). out[i] receives the SAME material id as
    // MarchingCubes::GetTerrainMaterialAt(positions[i]) would return - the
    // shaped height and the climate channels are evaluated through FastNoise's
    // SIMD GenPositionArray2D batch entry points (bit-identical to the per-point
    // GenSingle2D scalar helper on this build, proven by the parity gate),
    // collapsing the per-vertex ComputeShapedHeightSample + ComputeClimateSample
    // cost (~30 us each scalar) into one batched pass. The cave channel and the
    // final band selection stay per-vertex (cheap). positions are in WORLD
    // space. With shaping disabled this falls back to per-vertex classification
    // so legacy worlds are byte-unchanged.
    void
    ClassifyVertexMaterials(const Vec3* positions, std::size_t count, u32* out_materials) const;

    // SIMD-batched shaped final heights at an arbitrary
    // list of world (x,z) positions. out[i] == ComputeShapedHeight(xs[i], zs[i])
    // byte-for-byte (uses GenPositionArray2D, proven bit-identical to GenSingle2D
    // on this build). Used to batch the per-column surface-span corner samples
    // and exercised directly by the position-array parity gate.
    // apply_hydro: when true (default) and hydro is enabled, the baked
    // erosion offset is added so this matches GetTerrainHeightAt at every position
    // (keeps the surface-span corners consistent with collision/SDF). The erosion
    // bake passes false to sample the NO-hydro base height (and to avoid recursion)
    // -- this is also the fast SIMD path the bake uses to build its base grid.
    void ComputeShapedHeightsAtPositions(const float* xs,
                                         const float* zs,
                                         std::size_t count,
                                         float* out,
                                         bool apply_hydro = true) const;

    // river influence [0, 1] at a column - how strongly the +10 PV-band
    // river carve applies (0 = no river, 1 = channel center). 0 when rivers are
    // disabled. Pure function of (seed, params); used by the RiverPresence gate.
    float RiverInfluenceAt(float world_x, float world_z) const;

    static IVec3 world_to_chunk_coords(const Vec3& position);

    // --- API for WorldGenViewer ---
    const TerrainGenParams& get_params() const {
        return m_params;
    }
    //  (far-LOD): read-only seed accessor so the far-LOD scheduler can
    // key its pristine tile cache (seed, params_hash) without re-deriving the
    // seed. Minimal insertion - no height/noise path is touched.
    int get_seed() const {
        return m_seed;
    }
    void set_params(const TerrainGenParams& params);
    void set_seed(int seed);
    void regenerate_all_chunks(PhysicsSystem* physics_system);
    void clear_world(PhysicsSystem* physics_system);
    void SetWaterSystem(WaterSystem* water_system);
    std::vector<IVec3> GetInitialChunkLoadList(const Vec3& center_pos) const; // <<< NEW
    JobHandle dispatch_generation_jobs(const std::vector<IVec3>& chunks_to_generate);
    // Generation request carrying the meshing step the chunk is being
    // generated for, so far-ring (step > 1) chunks can skip the interior SDF
    // and 3D cave-noise grid (see GenerateChunkData target_step).
    struct ChunkGenerationRequest {
        IVec3 coords{0};
        int target_step = 1;
    };
    JobHandle
    dispatch_generation_jobs(const std::vector<ChunkGenerationRequest>& chunks_to_generate);
    bool EnsureCollisionReadyNear(const Vec3& world_pos,
                                  PhysicsSystem* physics_system,
                                  int horizontal_radius = 1);
    // render_lod0_radius (default -1): the ring distance that renders at full-SDF
    // LOD0, decoupled from collision_radius. -1 keeps the historical behaviour
    // (LOD0 boundary == collision_radius) so the GAME path and world_hash are
    // unchanged. A caller may pass a larger value than collision_radius to render
    // a wider full-detail near slice (caves/overhangs) without building collision
    // for it — used by the create-world preview.
    bool EnsureSurfaceReadyNear(const Vec3& world_pos,
                                PhysicsSystem* physics_system,
                                int surface_radius,
                                int collision_radius,
                                int render_lod0_radius = -1);
    const StreamingBudgetFrameStats& get_last_streaming_budget_stats() const {
        return m_last_streaming_budget_stats;
    }

    // Per-sub-phase timing from the latest update, retained as runtime telemetry
    // for localizing streaming stalls.
    struct DbgStreamTimings {
        double process_completed = 0.0;
        double telemetry = 0.0;
        double activation = 0.0;
        double water = 0.0;
        double meshing_pass = 0.0;
        double collision = 0.0;
    };
    const DbgStreamTimings& dbg_stream_timings() const {
        return m_dbg_stream;
    }
    const StreamingTelemetryStats& get_streaming_telemetry_stats() const {
        return m_streaming_telemetry_stats;
    }
    const std::vector<ChunkLOD>& get_lod_levels() const {
        return m_lod_levels;
    }
    RuntimeChunkStats get_runtime_chunk_stats() const;
    CameraLocalCoverageStats get_camera_local_coverage_stats(const Vec3& camera_position,
                                                             int horizontal_radius) const;
    float get_density_at_from_precalculated(const Vec3& world_pos, float terrain_height) const;

    //  implementation note: a deterministic, order-independent hash of the LIVE water-sim state
    // (water_level_data + water_flow_data folded across resident chunks, sorted by chunk id) plus
    // the count of chunks carrying a water sim. Test/telemetry hook for the live-water determinism
    // gate — the existing fixture gates never tick live water, so they cannot catch a non-determ-
    // inistic water-sim change (e.g. an async integration). Two independent sessions ticked with
    // the same seed+anchors must produce identical hash sequences.
    struct WaterStateHash {
        std::uint64_t hash = 0;
        std::size_t water_chunks = 0;
    };
    WaterStateHash debug_water_state_hash() const;
    //  gate hooks:  mass invariant held last tick;  the max live water depth (mm)
    // across resident chunks (> 0 once a river has filled). Debug/test only.
    [[nodiscard]] bool debug_water_mass_ok() const;
    [[nodiscard]] std::int64_t debug_max_water_depth_mm() const;
    [[nodiscard]] Vec3 debug_deepest_water_pos(
        std::int64_t* depth_mm_out = nullptr) const; // anchor capture on real water
    void debug_force_water_remesh(); // render-only: refresh all water surfaces next frame (capture
                                     // tooling)
    [[nodiscard]] std::int64_t debug_water_volume_near(const Vec3& center,
                                                       float radius_m) const; // drain ground-truth
    [[nodiscard]] std::int64_t
    debug_land_water_volume_mm() const; // rain-fed water on land (excludes the sea)
    [[nodiscard]] bool
    debug_lowest_land_pos(Vec3& pos_out,
                          float& bed_m_out) const; // valley floor where rain collects
    [[nodiscard]] bool debug_find_shoreline(Vec3& water_pos_out,
                                            float& to_land_x,
                                            float& to_land_z,
                                            float& water_surf_out,
                                            float& bank_height_out) const; // filmable shoreline
    [[nodiscard]] int debug_water_seam_wet_pairs() const; //   cross-chunk continuity

    // terraform the water bed (dig delta<0 / dam delta>0) within radius_m of
    // world_pos; the fixed-point solver then drains/pools. Returns cells edited.
    int EditTerrainBed(const Vec3& world_pos, std::int32_t delta_mm, float radius_m);

    // Finite hydrology removes the perpetual river source (drainable
    // water), rain_mm_per_tick adds rainfall (caller scales by weather precip), evap_mm_per_tick
    // recedes ponds.
    void
    SetWaterHydrology(bool finite, std::int32_t rain_mm_per_tick, std::int32_t evap_mm_per_tick);

    // wire weather-driven per-cell rain into the water solver
    // (precipitation integer-quantized at the boundary). null = OFF, byte-identical.
    // The session owner gates this on sim.hydrology_weather (default-OFF).
    void SetWaterWeatherRain(const Systems::WeatherSystem* weather, std::int32_t scale_mm);

    // water-source diagnostics passthrough (never hashed): source entities seen by the
    // injection loop / total mm injected — localizes a do-nothing spring.
    std::int64_t debug_water_sources_seen() const;
    std::int64_t debug_water_source_injected_mm() const;
    // water-source diagnostics: does the chunk containing this world position carry a
    // complete live water grid (the injection precondition)?
    bool debug_water_grid_at(float world_x, float world_z) const;
    // water-source diagnostics: the coords of the first N gridded chunks (id-sorted) —
    // ground truth for where the water grids actually live.
    std::vector<IVec3> debug_water_grid_chunk_coords(std::size_t max_count) const;

    // Debug/test only: cap the streaming wanted-set radius (chunks). 0 = uncapped
    // (production default, byte-identical). Deterministic: main-thread state read
    // only inside chunk activation; set once before the first update(). Shrinks
    // the resident disc — and with it the water-sim region and the eviction
    // hysteresis, which derives from the same radius — so the live-water
    // determinism gates pay a bounded fill instead of the full RENDER_DISTANCE
    // fill on every tick.
    void debug_set_streaming_radius_cap(int cap_chunks) {
        m_streaming_radius_cap = cap_chunks;
    }

    //  ( T.1): the water epoch (terraform bed edits increment it) —
    // folded into the waterfall site-cache key for bounded re-surveys.
    std::uint64_t water_epoch() const;
    // the LIVE water surface at a world position from the render float
    // mirror (one-way derived from the mm truth — legal after derived-state reclassification), read
    // off the 2.5D column's y=0 chunk. Returns terrain height when no grid/cell is live (i.e. "no
    // standing live water here").
    float live_water_surface_at(float world_x, float world_z) const;

    // boot-settle mode: lifts the live-play per-tick water init/sim caps for the
    // duration of the server BOOT water settle so the settle can complete its init work and
    // drain the flood transient. Boot-only; live play is untouched. See
    // WaterSystem::SetBootSettleMode for the full rationale.
    void SetWaterBootSettleMode(bool on);

    // loaded-boot water pause: a session booted from a save must not advance
    // water during Boot (the restored mid-flow state is authoritative). See
    // WaterSystem::SetBootPaused.
    void SetWaterBootPaused(bool on);

    // session water-sim resolution passthrough (sim.water_high_res): the owner sets it
    // ONCE, before the first update — see WaterSystem::SetSimResolution.
    void SetWaterSimResolution(int cells_per_side);
    // boot-time save migration: resize every loaded water chunk to the session
    // resolution in ONE pass, before the first live tick (see
    // WaterSystem::MigrateChunksToSimResolution). Returns chunks resized.
    std::size_t MigrateWaterSimResolution();
    // debug/test: the session water-sim resolution (0 when no water system), and the
    // count of loaded water chunks whose stored resolution differs from the session's
    // (0 after a converged migration — mixed resolutions wall off the seam pass).
    [[nodiscard]] int debug_water_sim_resolution() const;
    [[nodiscard]] std::size_t debug_water_chunks_off_resolution() const;

    // the rotating water sim-window cursor: evolution-relevant sim state,
    // persisted with the world and restored on load. See WaterSystem::GetSimWindowCursor.
    [[nodiscard]] std::size_t GetWaterSimWindowCursor() const;
    void SetWaterSimWindowCursor(std::size_t cursor);

    // PLAYER-FACING terraform: carve (fill=false) or fill (fill=true) a
    // sphere of radius_m into the voxel terrain at world_pos, remesh + rebuild colliders, and
    // couple the water bed (dig drains, fill dams). Deterministic + persisted (edits sdf_data).
    // Returns the number of chunks modified.
    int EditTerrainVoxel(const Vec3& world_pos,
                         float radius_m,
                         bool fill,
                         PhysicsSystem* physics_system);

    // --- Persistence integration (runtime save/load, ) ---
    // Blocks until in-flight generation and meshing jobs complete so chunk
    // voxel/mesh data is stable for hashing and serialization.
    void wait_for_streaming_jobs();
    //  ( step 1): drains the sim-truth promotion lane ONLY —
    // waits any in-flight promotion generation jobs, publishes their staged
    // voxel fields on this (main) thread, and dispatches the render-mesh
    // stage-B batch WITHOUT waiting for it. Lets callers (and tests) observe
    // sim truth going live independently of any render-mesh publish.
    void wait_for_promotion_jobs();
    //   (activation queue): stability WITHOUT publication. Drains every
    // streaming lane (generation, promotion, meshing) with RAW handle waits —
    // no pending→live movement, no stage-B dispatch, no state flips — so the
    // caller (the SAVE path) serializes stable bytes without becoming an
    // accidental activation event. Publication stays exclusively owned by the
    // per-tick barrier today and by the activation queue after the swap
    // (smoke autosaves at ticks 30/60/90 must not activate ahead of D+K).
    // Handles and outstanding flags are left intact: the next legitimate
    // publish point observes and publishes exactly as it would have.
    void quiesce_streaming_jobs_for_save();
    //  ( implementation note): the worldgen-epoch gate. Every
    // OFF-MAIN-THREAD worldgen sampling job (generation, promotion, meshing,
    // boot surface builds, and the client's far-LOD tile builds) holds the
    // SHARED side for the duration of one job; reinitialize_noise (set_params
    // / set_seed — the create-world preview's knob path) holds the EXCLUSIVE
    // side. A generator rebuild therefore QUIESCES in-flight samplers and
    // blocks new ones instead of racing them — replacing the assign-last
    // point-guard as the correctness mechanism (the ordering stays as
    // belt-and-braces). Per-JOB granularity: one lock per chunk/tile job,
    // negligible. Sampling jobs never take the exclusive side, so there is no
    // ordering inversion.
    [[nodiscard]] std::shared_lock<std::shared_mutex> acquire_worldgen_sample_scope() const {
        for (;;) {
            while (m_worldgen_writer_pending.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::shared_lock<std::shared_mutex> scope(m_worldgen_epoch_mutex);
            if (!m_worldgen_writer_pending.load(std::memory_order_acquire)) {
                return scope;
            }
        }
    }
    //  telemetry: lifetime totals of promotion-lane dispatches
    // (batches, chunks). Static smoke runs are expected to record ZERO (the
    // resident set is constant post-boot); moving runs exercise the lane.
    struct PromotionDispatchTotals {
        std::uint64_t batches = 0;
        std::uint64_t chunks = 0;
    };
    PromotionDispatchTotals promotion_dispatch_totals() const {
        return {m_promotion_batches_dispatched, m_promotion_chunks_dispatched};
    }

    //  shadow instrumentation (activation queue step 2, ): measures
    // the ACTUAL streaming-pipeline latencies in SIM TICKS that the activation
    // queue's fixed pipeline-latency K must cover. Observability only — never
    // hashed, inert unless the host drives the tick (the headless server
    // runner does under --avail-trace; the client never calls this, so
    // m_shadow_current_tick stays -1 and every recording site early-outs).
    //   (activation queue): the sim-tick entry point. The server runner
    // calls this EVERY tick; the tick stamps batch due_ticks at dispatch
    // (due = dispatch + kActivationPipelineLatencyTicks) and drives
    // activate_due after the swap. The client never calls it (tick stays -1:
    // batches carry due_tick -1 = publish-when-drained, today's semantics).
    void begin_tick(std::int64_t sim_tick) {
        m_current_sim_tick = sim_tick;
    }
    std::int64_t current_sim_tick() const {
        return m_current_sim_tick;
    }
    //  (activation queue ): tick-keyed activation. Publishes, in FIFO
    // order, exactly the batches whose due_tick is at or before `tick` —
    // BLOCKING on a due-but-unfinished batch's jobs (K under-covering is a
    // wall-clock cost, never a schedule change) — plus the promotion lane
    // when due. Batches with due_tick -1 (dispatched with no tick, i.e. the
    // client) publish when drained, exactly as today. Replaces the per-tick
    // wait_for_streaming_jobs barrier at the 5b swap; until then it is
    // exercised by the ActivationQueueSemantics gtest only.
    void activate_due(std::int64_t tick);
    void begin_tick_shadow(std::int64_t sim_tick) {
        m_shadow_current_tick = sim_tick;
    }
    struct ActivationShadowReport {
        // gen-dispatch tick -> first Ready tick, one sample per activated chunk
        std::vector<std::int64_t> generation_to_ready_ticks;
        // promotion-dispatch tick -> sim-truth publish tick
        std::vector<std::int64_t> promotion_to_publish_ticks;
        std::uint64_t generation_dispatches = 0; // chunks entered the shadow
        std::uint64_t promotion_dispatches = 0;
        std::uint64_t still_pending = 0; // dispatched, never activated by report time
    };
    ActivationShadowReport activation_shadow_report() const;
    // Shared-ownership snapshot of every streamed chunk (save path).
    std::vector<std::shared_ptr<::Luminumbra::Chunk>> snapshot_streamed_chunks() const;
    // Owner-thread capture for a 32x32 chunk far region plus a one-chunk X/Z
    // halo. Only exact full SDF lattices are copied into the immutable result.
    std::shared_ptr<const FarLodSdfSnapshot> capture_far_lod_sdf_snapshot(i32 rx, i32 rz) const;
    // Eviction does not stale a copied snapshot; changed SDF authority or
    // worldgen parameters do.
    bool is_far_lod_sdf_snapshot_current(const FarLodSdfSnapshot& snapshot) const;
    u64 far_lod_authority_revision() const {
        return m_far_lod_authority_revision.load(std::memory_order_acquire);
    }
    u64 far_lod_region_authority_revision(i32 rx, i32 rz) const;
    // Phase-A persistence transition: a successful authoritative chunk rewrite
    // changes the durable source observed by far workers even when voxel bytes
    // themselves were edited earlier. Bump the generation so any worker that
    // read the pre-commit LMR1 image is rejected at owner-thread integration.
    void notify_far_lod_authority_durable(const IVec3& chunk_coords);
    std::shared_ptr<::Luminumbra::Chunk> find_streamed_chunk(const IVec3& coords) const;
    // Adopts an externally loaded chunk when its slot is empty. Returns false
    // (without clobbering the streamed chunk) when a chunk with the same id
    // is already active.
    bool adopt_streamed_chunk(const std::shared_ptr<::Luminumbra::Chunk>& chunk);

private:
    struct StreamingState {
        std::unordered_map<ChunkID, std::shared_ptr<::Luminumbra::Chunk>> chunks;
        //   (activation queue): per-lane BATCH FIFOs. Each streaming
        // dispatch appends a batch record {job handles, its chunks, due_tick};
        // publication pops FRONT batches in FIFO order (deterministic
        // publication order = dispatch order). Under the per-tick barrier the
        // depth never exceeds 1 and every batch publishes the tick it was
        // dispatched, so this is byte-identical structure-only change; the
        // barrier swap gives due_tick = dispatch_tick + K its meaning.
        struct GenerationBatch {
            JobHandle handle;
            std::vector<std::shared_ptr<::Luminumbra::Chunk>> chunks;
            std::int64_t due_tick = -1;
        };
        std::deque<GenerationBatch> generation_batches;
        // Meshing work is split across two job lanes per dispatch: hole-fill
        // candidates (no active mesh yet) ride the High job lane so visible
        // gaps close ahead of bulk LOD/water remeshes on the Normal lane.
        struct MeshingJobChunk {
            std::shared_ptr<::Luminumbra::Chunk> chunk;
            bool terrain_mesh_required = true;
            u8 transition_faces = 0;
        };
        struct MeshingBatch {
            JobHandle handle;      // Normal lane
            JobHandle handle_high; // High lane
            std::vector<MeshingJobChunk> chunks;
            std::int64_t due_tick = -1;
        };
        std::deque<MeshingBatch> meshing_batches;
        //  ( step 1): the sim-truth promotion lane. LOD0
        // promotions of surface-band-only chunks generate their full voxel
        // field in a PROMOTION generation job (stage A, these handles); the
        // main thread publishes the staged sim truth in
        // process_completed_promotion_jobs and only then dispatches the
        // render-mesh batch (stage B) down the ordinary meshing lane —
        // meshing never writes sim truth.
        JobHandle promotion_job_handle;
        JobHandle promotion_job_handle_high;
        struct PromotionJobChunk {
            std::shared_ptr<::Luminumbra::Chunk> chunk;
            int lod_level = 0;
            bool high_priority = false;
        };
        std::vector<PromotionJobChunk> promotion_job_chunks; // stage A in flight
        std::vector<PromotionJobChunk>
            pending_promotion_mesh; // sim truth live, stage B not yet dispatched
        // promotion stays a depth-1 lane (its dispatch cadence is
        // low; stage B rides the meshing FIFO); due_tick joins it for the
        // activate_due scheduling shell.
        std::int64_t promotion_due_tick = -1;
    };

    StreamingState m_streaming_state;
    // the worldgen-epoch gate (see acquire_worldgen_sample_scope).
    mutable std::shared_mutex m_worldgen_epoch_mutex;
    mutable std::atomic<bool> m_worldgen_writer_pending{false};
    mutable std::atomic<u64> m_far_lod_capture_epoch{0};
    std::atomic<u64> m_far_lod_authority_revision{0};
    mutable std::mutex m_far_lod_region_revision_mutex;
    std::map<std::pair<i32, i32>, u64> m_far_lod_region_revisions;
    void bump_far_lod_authority_revision(const IVec3& chunk_coords);
    //  telemetry (main-thread, never hashed).
    std::uint64_t m_promotion_batches_dispatched = 0;
    std::uint64_t m_promotion_chunks_dispatched = 0;
    // the current sim tick (-1 = no tick source, i.e. the
    // client). Set by begin_tick from the server runner each tick.
    std::int64_t m_current_sim_tick = -1;
    //  shadow state (main-thread, never hashed; -1 tick = shadow off).
    std::int64_t m_shadow_current_tick = -1;
    std::unordered_map<ChunkID, std::int64_t> m_shadow_generation_dispatch_tick;
    std::unordered_map<ChunkID, std::int64_t> m_shadow_promotion_dispatch_tick;
    std::vector<std::int64_t> m_shadow_gen_latency_samples;
    std::vector<std::int64_t> m_shadow_promo_latency_samples;
    std::uint64_t m_shadow_gen_dispatches = 0;
    std::uint64_t m_shadow_promo_dispatches = 0;
    void shadow_note_generation_dispatch(ChunkID id);
    void shadow_note_promotion_dispatch(ChunkID id);
    void shadow_note_ready(ChunkID id);
    void shadow_note_promotion_published(ChunkID id);
    void shadow_note_evicted(ChunkID id);
    StreamingBudgetFrameStats m_last_streaming_budget_stats;
    // debug_set_streaming_radius_cap: 0 = uncapped (production behaviour).
    int m_streaming_radius_cap = 0;
    DbgStreamTimings m_dbg_stream; // runtime telemetry
    StreamingTelemetryStats m_streaming_telemetry_stats;
    uint64_t m_deferred_backlog_age_frames = 0;
    //  trailing per-frame queue-depth ring used to
    // derive StreamingTelemetryStats::settled_queue_depth (the min over the last
    // activation window). Sized to one full activation period so the window
    // always spans an activation tick + its drain. Telemetry only.
    std::array<std::size_t, 8> m_recent_queue_depths{};
    std::size_t m_recent_queue_depth_count = 0;
    std::size_t m_recent_queue_depth_cursor = 0;

    //  streaming elision: skip the O(N) Step-2/3 meshing-candidate pass on FULLY SETTLED
    // ticks. The decision to RUN the pass keys ONLY on deterministic, main-thread-observed signals
    // (a dirty-generation delta, EXACT anchor-vector inequality, chunk-count delta, an activation
    // tick, or a not-yet-drained previous pass) — NEVER on job-completion timing (the attempt-#1
    // determinism trap). Job-active state is consulted ONLY to decide whether the world has reached
    // quiescence (so a future tick MAY elide), which on the per-tick-quiesced hashed paths is
    // itself deterministic. `m_dirty_generation` is bumped at the chunk insert/erase +
    // synchronous-rebuild sites that mutate a settled world.
    std::uint64_t m_dirty_generation = 0;
    std::uint64_t m_last_serviced_generation = 0;
    std::vector<Vec3> m_last_anchor_positions;

    // Elide update_chunk_activation's O(radius^2)
    // wanted-set enumeration + O(N) eviction scan when the residency set is PROVABLY unchanged —
    // the anchor chunk coords are identical, no world mutation since (dirty_generation), and the
    // last activation created NOTHING (scheduled+deferred == 0, i.e. every wanted chunk was already
    // resident). All signals are deterministic main-thread state (NOT job-activity timing — the
    // attempt-#1 trap), so the skip is residency-equivalent and run==replay / host==peer stay
    // byte-identical. This is the dominant per-tick cost while stationary at high render distance.
    bool m_activation_has_run = false;
    std::vector<IVec3> m_last_activation_camera_chunks;
    std::uint64_t m_last_activation_dirty_generation = 0;
    std::size_t m_last_activation_pending =
        0; // scheduled_generation + deferred_generation last pass
    std::size_t m_last_chunk_count = 0;
    bool m_last_pass_drained = false; // sticky: false forces the next tick's candidate pass to run

    // Gate the O(N) per-tick collision-creation scan so
    // a SETTLED world (every Ready/LOD0 chunk already has its collider) skips the scan entirely
    // instead of walking all chunks every tick. The flag is a PRECISE, main-thread, deterministic
    // work signal: set true at exactly the two sites that reset has_collision=false (remesh / LOD0
    // promotion in process_completed_meshing_jobs, and the synchronous surface-horizon rebuild) —
    // NOT job-activity state (cf. the attempt-#1 trap above). Cleared only when a scan visits every
    // chunk without hitting the per-frame cap (i.e. it drained all eligible work). Starts true so
    // the first tick scans. The scan body itself is byte-identical to before, so collision
    // eligibility/order — and thus has_collision, which feeds world_hash — is unchanged; only
    // redundant settled-tick scans go.
    bool m_collision_pass_dirty = true;

    const std::vector<ChunkLOD> m_lod_levels = {
        {0, 1, 192.0f}, // LOD 0: Full detail up to 192 meters (~12 chunks)
        {1, 2, 384.0f}, // LOD 1: Half resolution up to 384 meters (~24 chunks)
        {2, 4, 640.0f}  // LOD 2: Quarter resolution beyond the near visual range
    };
    int get_lod_level_for_distance(float dist) const;
    int get_lod_step_for_level(int lod_level) const;

    // Required streaming LOD for a chunk: horizontal-only distance for
    // surface-band chunks (keeps the terrain surface on one LOD per column
    // so vertical LOD seams cannot open), 3D distance for deep/air chunks.
    // current_lod: the chunk's already-meshed LOD, or -1 if the
    // chunk has never been meshed. When >= 0, demotions (finer -> coarser)
    // apply an asymmetric hysteresis margin so chunks dwelling on a band
    // edge do not oscillate; promotions and first-time assignment are
    // unaffected (see implementation comment).
    int get_required_lod_for_chunk(const IVec3& coords,
                                   const Vec3& chunk_center,
                                   const Vec3& camera_position,
                                   int current_lod = -1);

    // Chunk-Y span of the terrain surface for a horizontal column.
    // Sampled at 5 points - the column center plus its 4 footprint corners -
    // so steep columns (mountains preset: >16 m height variation across one
    // chunk) report every chunk-Y their isosurface passes through, not just
    // the center sample. Corner samples are shared with the neighboring
    // columns, so adjacent spans overlap at shared corners and the cliff
    // wall between columns of different surface height is always inside one
    // of the two spans. center_y preserves the old single-point sample for
    // ordering (drain center-out) and collision selection.
    struct ColumnSurfaceSpan {
        int min_y = 0;
        int max_y = 0;
        int center_y = 0;
        // cached surface biome id for the column (u8, 255 = none).
        // Filled from BiomeIdAt when biomes are enabled, kNoBiome otherwise.
        u8 biome_id = 255u;
    };
    // Pure 5-point sampling (no cache) - usable from const initial-load paths.
    ColumnSurfaceSpan compute_column_surface_span(int chunk_x, int chunk_z) const;
    // Cached for the lifetime of the current seed/params (terrain height is
    // deterministic).
    ColumnSurfaceSpan column_surface_span(int chunk_x, int chunk_z);
    std::unordered_map<u64, ColumnSurfaceSpan> m_column_surface_span_cache;

    // --- Helper Functions ---
    // Returns true if it actually ran the activation pass (false if it was elided because the
    // wanted residency set is provably unchanged).  implementation note (streaming-residual).
    bool update_chunk_activation(const std::vector<Vec3>& anchors, PhysicsSystem* physics_system);
    // Signature updated to use shared_ptr
    struct MeshingWorkItem {
        std::shared_ptr<::Luminumbra::Chunk> chunk;
        int lod_level = 0;
        bool terrain_mesh_required = true;
        // Near-field hole-fill work rides the High job lane (see
        // MAX_HIGH_PRIORITY_MESHING_JOBS_PER_DISPATCH in the.cpp).
        bool high_priority = false;
    };
    void dispatch_meshing_jobs(const std::vector<MeshingWorkItem>& chunks_to_mesh);
    void process_completed_meshing_jobs(bool force);
    bool meshing_jobs_active() const;
    //  promotion lane (see StreamingState). dispatch_promotion_jobs
    // runs stage A (sim-truth generation into staging);
    // process_completed_promotion_jobs publishes staged sim truth on the main
    // thread and dispatches stage B. promotion_pipeline_pending covers the
    // whole pipeline (jobs in flight OR unpublished results OR stage B not yet
    // dispatched) for the scheduling gates that today read meshing activity.
    void dispatch_promotion_jobs(const std::vector<MeshingWorkItem>& chunks_to_promote);
    void process_completed_promotion_jobs(bool force);
    // MAIN-THREAD generation publication — flips completed
    // (pending_generation_ready) batch chunks Loading→Idle and clears the
    // batch bookkeeping + outstanding flag. Called from
    // wait_for_generation_jobs (barrier / mutate-site paths) and the
    // update-start observation (client path); becomes an activate_due
    // responsibility at the barrier swap.
    //  5b: `force` distinguishes the two publication regimes. force
    // = true (the explicit drains: wait_for_*, boot, hash, mutate sites)
    // publishes any drained batch regardless of due tick — legal only at
    // deterministic full-drain points. force = false (the per-frame client
    // hooks + dispatch heads) publishes a batch only once it is BOTH drained
    // AND due (due_tick -1 = client batch = due when drained), so on the
    // server the per-tick schedule belongs exclusively to activate_due.
    void publish_completed_generation_jobs(bool force);
    bool publish_front_generation_batch(bool force);
    bool publish_front_meshing_batch(bool force);
    bool promotion_jobs_active() const;
    bool promotion_pipeline_pending() const;
    //   (+5a-2): publication-keyed "a batch is outstanding" —
    // the lane FIFOs are appended at dispatch and popped at the main-thread
    // publish, so these are pure functions of main-thread events (unlike the
    // wall-clock *_jobs_active counter reads, which stay raw inside the
    // wait/publish machinery only).
    bool meshing_batch_outstanding() const;
    bool generation_batch_outstanding() const;
    // Raw counter read (machinery only): any generation batch's jobs in flight.
    bool generation_jobs_active() const;
    // Simulation-availability predicate for LOD0 consumers. Collision geometry
    // is built from the heightmap, while eligibility is keyed to publication of
    // the corresponding LOD0 render mesh. Keeping the predicate centralized
    // makes activation queue ownership explicit and tick-keyed.
    static bool sim_available_lod0(const ::Luminumbra::Chunk& chunk);
    void wait_for_generation_jobs();
    void wait_for_meshing_jobs();
    void reinitialize_noise();

    // Shared terrain-height implementation.
    // Every terrain-height consumer (GetTerrainHeightAt, SampleWorldGenLayers,
    // GenerateChunkData full + step>1 batch loops, and the column-span cache
    // through GetTerrainHeightAt) derives its height from this helper so the
    // scalar and batch paths cannot diverge. All noise reads use GenSingle2D
    // (never batch SIMD Gen* grids) when shaping is enabled, making the batch
    // heightmap bytes EXACTLY equal to the scalar value at the same world
    // coordinate. With shaping disabled the helper reproduces the legacy
    // float-op sequence bit-for-bit, while GenerateChunkData keeps its
    // GenUniformGrid2D fast path (legacy heights/hashes untouched, covered by
    // the existing max_sdf_sample_error < 1e-4 snapshot gate).
    struct ShapedHeightSample {
        float base_noise = 0.0f;        // detail FBM (at warped coords when shaping)
        float pre_island_height = 0.0f; // combined height before the island mask
        float island_noise = 0.0f;
        float island_mask = 1.0f;
        float final_height = 0.0f;     // after the river carve
        float pre_carve_height = 0.0f; // final surface before the river carve
        bool island_applied = false;
    };
    ShapedHeightSample ComputeShapedHeightSample(float world_x, float world_z) const;
    float ComputeShapedHeight(float world_x, float world_z) const;

    // SIMD-batched shaped heights for a chunk-aligned
    // (size_x * size_z) column grid whose origin is the integer world position
    // (base_x, base_z). Writes size_x*size_z final heights into out (row-major,
    // x fastest) that are BYTE-IDENTICAL to calling ComputeShapedHeight at each
    // column - every noise channel is evaluated through FastNoise's SIMD batch
    // entry points (GenUniformGrid2D for the unwarped continentalness/erosion/
    // warp channels; GenPositionArray2D for the warp-displaced base detail and
    // peaks channels), which produce the same float bits as the per-point
    // GenSingle2D scalar helper on this build (proven by the batch-vs-scalar
    // parity gtest, which now also exercises the SIMD path). The slow per-column
    // GenSingle2D loop cost ~26 us/column; the batched path is ~2 us/column.
    // Only valid when m_params.shaping_enabled; callers gate on that.
    void ComputeShapedHeightGrid(int base_x, int base_z, int size_x, int size_z, float* out) const;

    // hydraulic/thermal relief (decision a). The analytic height is
    // computed by...Impl(apply_hydro=false); ComputeShapedHeightSample adds the
    // baked offset when hydro_enabled, so every consumer walks the eroded
    // surface. SampleHydroOffsetMeters baked per kHydroRegionMeters region
    // (deterministic, cached, recompute-on-load); the bake samples the NO-hydro
    // height (apply_hydro=false) to avoid recursion. World_hash-affecting when
    // enabled. Halo == hydro_iterations (halo-independent interior, hydraulic erosion kernel).
    ShapedHeightSample
    ComputeShapedHeightSampleImpl(float world_x, float world_z, bool apply_hydro) const;
    float SampleHydroOffsetMeters(float world_x, float world_z) const;

    // the five normalized climate dimensions consumed by the biome
    // lookup. continentalness/erosion/peaks_valleys REUSE the +3/+4/+5 shaping
    // control noises (sampled at the unwarped column, exactly as
    // ComputeShapedHeightSample reads them) so terrain and biomes agree;
    // temperature/humidity are the new +8/+9 climate noises. Only meaningful
    // when m_biomes_enabled.
    struct ClimateSample {
        float continentalness = 0.0f;
        float erosion = 0.0f;
        float peaks_valleys = 0.0f;
        float temperature = 0.0f;
        float humidity = 0.0f;
    };
    ClimateSample ComputeClimateSample(float world_x, float world_z) const;

    // river influence [0, 1] from the +10 noise folded into PV space,
    // ramped across the valleys band. 0 outside the band / rivers disabled.
    // Shared by ComputeShapedHeightSample (carve) and RiverInfluenceAt (gate).
    float RiverInfluenceFromNoise(float world_x, float world_z) const;
    // carve depth at one column for a given river
    // influence [0, 1] (pure; 0 outside the band). Factored out so the coarse
    // anti-aliased sampler can re-evaluate the carve over a footprint stencil.
    float RiverCarveAmount(float final_height, float influence) const;
    // Lakes: lake influence [0,1] from the smooth lake field (seed +11),
    // and the carve toward a sub-SEA_LEVEL floor. Mirror the river helpers so the
    // scalar + batched height paths stay byte-identical. 0 when lakes disabled.
    float LakeInfluenceFromNoise(float world_x, float world_z) const;
    // Carve toward a basin floor below `lake_surface` (the LOCAL lake level), so a
    // lake forms at its basin elevation, not at SEA_LEVEL. 0 when not in a lake.
    float LakeCarveAmount(float final_height, float influence, float lake_surface) const;
    // Smooth regional base elevation (height_offset + continental spline only, no
    // ridge/detail) — the level a lake's surface nestles at. Low-frequency so a
    // lake reads ~flat over its extent. Pure.
    float ContinentalBaseHeight(float world_x, float world_z) const;
    // FLAT lake surface: ContinentalBaseHeight sampled at a coarse-snapped position
    // so the surface is constant over a lake (a smoothly-varying surface reads as a
    // concave/tilted lake). The carve + water mesh both use this so basin + surface
    // agree. Pure.
    float LakeSurfaceLevel(float world_x, float world_z) const;
    //  terrace the height into cliff benches inside cliff-zones (smooth
    // mask, seed +12). Pure; returns `height` unchanged when cliffs disabled or
    // outside a cliff zone. Shared by the scalar + batched paths (byte-identical).
    float CliffTerracedHeight(float world_x, float world_z, float height) const;
    // 2D rim depression (metres to LOWER the surface) from the nearest
    // surface-break feature, so dolines DIP the heightfield even on the
    // SDF-ignoring coarse/far path. Pure fn of (world_x, world_z, seed); returns 0
    // when surface_breaks disabled. Folded into all three height paths
    // (ComputeShapedHeightSampleImpl, ComputeShapedHeightGrid, GetTerrainHeightAtCoarse)
    // so near/far/coarse stay byte-identical.
    float SurfaceBreakRimDepression(float world_x, float world_z) const;
    // the ONE shared surface-break sampler. Returns the per-column effective
    // cap (18 outside any feature, lowered toward entrance_min_cap inside a
    // cave-mouth/doline where the interior-proximity probe shows the cave noise is
    // already carved) and the analytic sinkhole carve (>=0) at world_pos. All four
    // CPU cave call sites route through this so the float-op sequence is
    // byte-identical across paths and chunk seams (absolute world coords only).
    // Returns {kCaveSurfaceCapDepth, 0} when surface_breaks_enabled is false (the
    // disabled path stays byte-identical to the pre-A3 implementation).
    struct SurfaceBreakSample {
        float effective_cap;
        float carve;
    };
    SurfaceBreakSample sample_surface_breaks(const Vec3& world_pos, float surface_h) const;
    // Monotone piecewise-linear spline over sorted [input, output] control
    // points: endpoint-clamped, plain lerp between neighbors, `fallback` when
    // the point list is empty.
    static float EvaluateShapingSpline(const std::vector<std::array<float, 2>>& points,
                                       float input,
                                       float fallback);

    int m_update_tick_counter = 0;

    // --- Dependencies ---
    JobSystem* m_job_system;
    TerrainGenParams m_params;
    int m_seed;

    // Noise states for procedural generation
    FastNoise::SmartNode<FastNoise::Generator> m_terrain_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_cave_generator;
    FastNoise::SmartNode<FastNoise::Generator>
        m_spaghetti_generator; // noise-router tunnels, built when cave_style == 1
    FastNoise::SmartNode<FastNoise::Generator>
        m_worley_generator; // Worley cheese caverns, built when cave_style == 1
    FastNoise::SmartNode<FastNoise::Generator> m_island_mask_generator;
    //  shaping control noises (only built when shaping_enabled; seed
    // offsets +3/+4/+5 for continentalness/erosion/peaks, the single warp
    // simplex is sampled with seeds +6 and +7 for the X/Z warp channels).
    FastNoise::SmartNode<FastNoise::Generator> m_continentalness_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_erosion_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_peaks_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_warp_generator;
    //  climate noises (seed registry: +8 temperature, +9 humidity).
    // Only built when biomes are enabled; legacy worlds never construct them.
    FastNoise::SmartNode<FastNoise::Generator> m_temperature_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_humidity_generator;
    //  river noise (seed registry: +10). Only built when rivers enabled.
    FastNoise::SmartNode<FastNoise::Generator> m_river_generator;

    //  biome table (game data). Loaded from m_params.biome_table_path on
    // (re)init when biomes are enabled; empty/disabled otherwise.
    World::BiomeTable m_biome_table;
    bool m_biomes_enabled = false;

    //  structure template pools (game data). Loaded from
    // m_params.structures_data_dir when structures are enabled; the combined
    // content hash is stamped into m_params.structures_content_hash so the far
    // cache key tracks template changes. Public accessors expose the deterministic
    // placement query so callers (and gates) can locate sites.
    std::vector<World::StructureTemplatePool> m_structure_pools;
    bool m_structures_enabled = false;

    WaterSystem* m_water_system;

    // per-region baked hydraulic-relief offset cache. Keyed by integer
    // region (rx,rz) on a kHydroRegionMeters grid; each entry is the cropped
    // interior offset (row-major, kHydroRegionCells^2). Lazily baked on first
    // lookup (pure function of region+seed+params -> recompute-on-load, NOT
    // persisted, like wind/aether). `mutable` + a mutex because the height path
    // is queried from multithreaded chunk-gen jobs; the bake is deterministic so
    // a duplicate concurrent bake would be identical, the mutex only guards the
    // map. Empty/unused when hydro is disabled. BOUNDED: past
    // kHydroCacheMaxRegions entries the farthest region from the latest request
    // is evicted (see offset_at), so long exploration cannot grow this without
    // limit; eviction only ever costs a byte-identical re-bake.
    mutable std::map<std::pair<std::int64_t, std::int64_t>, std::vector<float>> m_hydro_cache;
    // shared_mutex: warm-cache lookups take a SHARED lock (concurrent across the
    // multithreaded chunk-gen jobs); only the one-time per-region bake takes the
    // exclusive lock, and the bake itself runs OUTSIDE any lock (pure function).
    mutable std::shared_mutex m_hydro_mutex;
    // Hydro PREFETCH (re-enable erosion without the on-demand bake hitch): regions
    // currently queued for a background bake, so PrefetchHydroRegions doesn't
    // re-dispatch the same region every tick before its job lands.
    // mutable: PrefetchHydroRegions is const ( — the far-LOD scheduler holds a
    // const SHIELD_WorldSystem* and warms far regions ahead of the bake). It only
    // touches the deterministic, recompute-on-load hydro caches (already mutable),
    // so const-correctness is preserved (no world_hash-affecting state changes).
    mutable std::set<std::pair<std::int64_t, std::int64_t>> m_hydro_prefetch_inflight;
    mutable std::mutex m_hydro_prefetch_mutex;

public:
    // Bake the hydraulic-erosion regions covering a disc around (cx,cz) AHEAD of
    // demand, on background jobs, so when chunk-gen samples the height the region
    // is already warm in the cache (no main-thread / gen-critical-path bake stall —
    // the cause of the laggy-while-flying with hydro on). No-op when hydro is off.
    // const: only mutates the deterministic recompute-on-load hydro caches (mutable),
    // so the far-LOD scheduler (which holds a const SHIELD_WorldSystem*) can warm far
    // regions ahead of BuildPristineFarLodTile.
    void PrefetchHydroRegions(float cx, float cz, float radius_m) const;
};

} // namespace Luminumbra::Systems
