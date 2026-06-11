#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../world/Chunk.h"
#include "../core/JobSystem.h"
#include <array>
#include <cstddef>
#include <unordered_map>
#include <memory>
#include <string>
#include <optional>
#include <vector>
#include <functional>
#include "entt/entt.hpp"
#include "FastNoise/FastNoise.h"
#include "../world/BiomeTable.h"
#include "../world/StructurePlacement.h"

namespace Luminumbra::Systems {

class PhysicsSystem;
class WaterSystem;
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

    bool island_mask_enabled = false;
    float island_mask_frequency = 0.004f;

    // --- T-I3-10 terrain shaping (default-off) ---
    // With shaping_enabled == false every height path is bit-identical to the
    // pre-shaping implementation (legacy regression hashes in
    // test_worldgen_layer_snapshots.cpp prove it). When enabled, three 2D
    // control channels modulate the base FBM detail channel:
    //   continentalness (m_seed + 3) -> continental_spline -> base elevation
    //   erosion         (m_seed + 4) -> erosion_spline     -> amplitude mult
    //   peaks/valleys   (m_seed + 5) -> peaks_spline       -> ridge term
    // plus a 2-channel simplex domain warp (m_seed + 6 / + 7) applied to the
    // BASE noise (and pv) sample coordinates. Seed offsets are pinned by the
    // iteration-3 seed registry (design-decisions.md section 2).
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

    // --- T-I4-1 biome selection (default-off) ---
    // Biomes are opt-in via the preset block "biomes": {"table": "..."}. When
    // biomes_enabled is false the world system never builds the temperature
    // (+8) / humidity (+9) climate noises and never loads a biome table, so
    // every height AND material path is bit-identical to the pre-biome
    // implementation (the worldgen snapshot/hash fixtures prove byte-zero
    // drift). biome_table_path is relative to data/ (e.g. "common/biomes.json").
    // T-I4-2 mixes the loaded table's content hash into ComputeTerrainParamsHash
    // so pristine far-LOD tiles self-invalidate on a table content change.
    bool biomes_enabled = false;
    std::string biome_table_path;
    float temperature_frequency = 0.005f;
    float humidity_frequency = 0.005f;

    // --- T-I4-3 PV-band rivers (default-off) ---
    // Chunk-local river carve on the +10 ridged noise (seed registry). The
    // folded PV value PV = 1 - |3*|r| - 2| (Minecraft 1.18 weirdness->PV) of
    // the +10 noise selects the rivers where it falls in the VALLEYS band
    // [river_pv_min, river_pv_max] = [-1.0, -0.85] (research Area 1); the same
    // +10 noise modulates width/wobble. Carving lowers the terrain floor below
    // SEA_LEVEL so the EXISTING global water plane fills the channel (no
    // WaterSystem changes; critique F5). Bank material is the biome filler.
    // Applied inside ComputeShapedHeight so near chunks AND far tiles agree at
    // the seam. With rivers_enabled false the height path is bit-identical to
    // pre-river generation (byte-zero drift).
    bool rivers_enabled = false;
    float river_frequency = 0.0016f;
    float river_pv_min = -1.0f;   // valleys band lower edge (folded PV)
    float river_pv_max = -0.85f;  // valleys band upper edge (folded PV)
    float river_depth = 8.0f;     // metres the channel floor sits below SEA_LEVEL
    float river_max_carve = 60.0f; // clamp on terrain lowered into the channel
    // fnv1a64 of the canonicalized biome table content, stamped by the world
    // system when it loads the table (0 when biomes are disabled or the table
    // failed to load). ComputeTerrainParamsHash mixes this in so pristine
    // far-LOD tiles self-invalidate when the table content changes even though
    // the path is unchanged (design-decisions section 2).
    u64 biome_table_content_hash = 0;
    // T-I4-4: structures. When enabled, the world system places jigsaw
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
        // Voxel-field storage telemetry (T-I3-1): resident SDF/heightmap
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

    // Sim-side frustum surface coverage (T-I3-3, player_view_smoke gate):
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
    FrustumSurfaceCoverageStats get_frustum_surface_coverage_stats(
        const Vec3& camera_position,
        const std::array<Vec4, 6>& frustum_planes,
        float max_distance) const;

    SHIELD_WorldSystem(JobSystem* job_system, WaterSystem* water_system, const TerrainGenParams& params, int seed);
    ~SHIELD_WorldSystem();

    // Generates voxel data for a chunk. target_step is the meshing step this
    // generation must support: step <= 1 (default) generates the full 17^3
    // SDF + heightmap (LOD0 marching cubes, collision, edits, persistence);
    // step > 1 generates ONLY the 17x17 heightmap (the coarse heightfield
    // mesher and its boundary seam fallback never read interior SDF), leaving
    // sdf_data empty and skipping the 3D cave-noise grid entirely. All
    // existing callers default to full generation.
    void GenerateChunkData(::Luminumbra::Chunk& chunk, int target_step = 1) const;
    void update(entt::registry& registry, const Vec3& camera_position, PhysicsSystem* physics_system);
    std::vector<::Luminumbra::Chunk*> get_renderable_chunks();
    float get_density_at(const Vec3& world_pos) const;
    float GetTerrainHeightAt(float world_x, float world_z) const;
    // T-I4-DR-river-seam-sliver: coarse-LOD height. Identical to
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

    // --- T-I4-1 biome selection ---
    // Whether biomes are active (preset opted in AND the table loaded).
    bool biomes_enabled() const { return m_biomes_enabled; }
    const World::BiomeTable& biome_table() const { return m_biome_table; }

    // --- T-I4-4 structures ---
    bool structures_enabled() const { return m_structures_enabled; }
    const std::vector<World::StructureTemplatePool>& structure_pools() const {
        return m_structure_pools;
    }
    // Nearest structure site of the given type to (world_x, world_z) within the
    // search radius (cells), or nullopt. Deterministic; pure function of the
    // world seed + the type's grid. Used by gates and locate(type, near).
    std::optional<World::StructureSite> LocateStructure(
        const std::string& type, int world_x, int world_z, int search_radius_cells = 2) const;
    // Per-column biome id at the surface (u8, 255 = none). Pure function of
    // (seed, params): samples the five climate dimensions (continentalness,
    // erosion, peaks/valleys reuse the +3/+4/+5 shaping noises; temperature
    // +8, humidity +9) and resolves the first matching biome row. Returns
    // kNoBiome (255) when biomes are disabled or no row matches, so callers
    // fall back to the legacy single-material classifier.
    u8 BiomeIdAt(float world_x, float world_z) const;

    // T-I4-2 biome-aware surface material. Given a solid surface sample (its
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
    // river_bank (T-I4-3): when true, an above-water surface skin that would be
    // the biome `top` is laid as the biome `filler` instead - the exposed muddy
    // bank along a carved river channel (design-decisions section 4).
    MaterialType SurfaceMaterialForColumn(float world_y,
                                          float final_height,
                                          u8 biome_id,
                                          bool river_bank = false) const;

    // T-I4-3: river influence [0, 1] at a column - how strongly the +10 PV-band
    // river carve applies (0 = no river, 1 = channel center). 0 when rivers are
    // disabled. Pure function of (seed, params); used by the RiverPresence gate.
    float RiverInfluenceAt(float world_x, float world_z) const;

    static IVec3 world_to_chunk_coords(const Vec3& position);

    // --- API for WorldGenViewer ---
    const TerrainGenParams& get_params() const { return m_params; }
    // T-I3-9 (far-LOD): read-only seed accessor so the far-LOD scheduler can
    // key its pristine tile cache (seed, params_hash) without re-deriving the
    // seed. Minimal insertion - no height/noise path is touched.
    int get_seed() const { return m_seed; }
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
    JobHandle dispatch_generation_jobs(const std::vector<ChunkGenerationRequest>& chunks_to_generate);
    bool EnsureCollisionReadyNear(const Vec3& world_pos, PhysicsSystem* physics_system, int horizontal_radius = 1);
    bool EnsureSurfaceReadyNear(const Vec3& world_pos, PhysicsSystem* physics_system, int surface_radius, int collision_radius);
    const StreamingBudgetFrameStats& get_last_streaming_budget_stats() const { return m_last_streaming_budget_stats; }
    const StreamingTelemetryStats& get_streaming_telemetry_stats() const { return m_streaming_telemetry_stats; }
    const std::vector<ChunkLOD>& get_lod_levels() const { return m_lod_levels; }
    RuntimeChunkStats get_runtime_chunk_stats() const;
    CameraLocalCoverageStats get_camera_local_coverage_stats(const Vec3& camera_position, int horizontal_radius) const;
    float get_density_at_from_precalculated(const Vec3& world_pos, float terrain_height) const;
    
    // GPU SDF generation integration
    void SetGPUSDFCallback(std::function<bool(const IVec3&, const TerrainGenParams&, int, std::vector<float>&)> callback);

    // --- Persistence integration (runtime save/load, T-I2-12) ---
    // Blocks until in-flight generation and meshing jobs complete so chunk
    // voxel/mesh data is stable for hashing and serialization.
    void wait_for_streaming_jobs();
    // Shared-ownership snapshot of every streamed chunk (save path).
    std::vector<std::shared_ptr<::Luminumbra::Chunk>> snapshot_streamed_chunks() const;
    std::shared_ptr<::Luminumbra::Chunk> find_streamed_chunk(const IVec3& coords) const;
    // Adopts an externally loaded chunk when its slot is empty. Returns false
    // (without clobbering the streamed chunk) when a chunk with the same id
    // is already active.
    bool adopt_streamed_chunk(const std::shared_ptr<::Luminumbra::Chunk>& chunk);

private:
    std::function<bool(const IVec3&, const TerrainGenParams&, int, std::vector<float>&)> m_gpu_sdf_callback;

private:
    struct StreamingState {
        std::unordered_map<ChunkID, std::shared_ptr<::Luminumbra::Chunk>> chunks;
        JobHandle generation_job_handle;
        // Meshing work is split across two batches per dispatch: hole-fill
        // candidates (no active mesh yet) ride the High job lane so visible
        // gaps close ahead of bulk LOD/water remeshes on the Normal lane.
        JobHandle meshing_job_handle;
        JobHandle meshing_job_handle_high;
        struct MeshingJobChunk {
            std::shared_ptr<::Luminumbra::Chunk> chunk;
            bool terrain_mesh_required = true;
            u8 transition_faces = 0;
        };
        std::vector<MeshingJobChunk> meshing_job_chunks;
    };

    StreamingState m_streaming_state;
    StreamingBudgetFrameStats m_last_streaming_budget_stats;
    StreamingTelemetryStats m_streaming_telemetry_stats;
    uint64_t m_deferred_backlog_age_frames = 0;

    const std::vector<ChunkLOD> m_lod_levels = {
        {0, 1, 192.0f},  // LOD 0: Full detail up to 192 meters (~12 chunks)
        {1, 2, 384.0f},  // LOD 1: Half resolution up to 384 meters (~24 chunks)
        {2, 4, 640.0f}   // LOD 2: Quarter resolution beyond the near visual range
    };
    int get_lod_level_for_distance(float dist) const;
    int get_lod_step_for_level(int lod_level) const;

    // Required streaming LOD for a chunk: horizontal-only distance for
    // surface-band chunks (keeps the terrain surface on one LOD per column
    // so vertical LOD seams cannot open), 3D distance for deep/air chunks.
    // current_lod (T-I3-19): the chunk's already-meshed LOD, or -1 if the
    // chunk has never been meshed. When >= 0, demotions (finer -> coarser)
    // apply an asymmetric hysteresis margin so chunks dwelling on a band
    // edge do not oscillate; promotions and first-time assignment are
    // unaffected (see implementation comment).
    int get_required_lod_for_chunk(
        const IVec3& coords,
        const Vec3& chunk_center,
        const Vec3& camera_position,
        int current_lod = -1);

    // Chunk-Y span of the terrain surface for a horizontal column (T-I3-2).
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
        // T-I4-1: cached surface biome id for the column (u8, 255 = none).
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
    void update_chunk_activation(const Vec3& player_pos, PhysicsSystem* physics_system);
    // Signature updated to use shared_ptr
    struct MeshingWorkItem {
        std::shared_ptr<::Luminumbra::Chunk> chunk;
        int lod_level = 0;
        bool terrain_mesh_required = true;
        // Near-field hole-fill work rides the High job lane (see
        // MAX_HIGH_PRIORITY_MESHING_JOBS_PER_DISPATCH in the .cpp).
        bool high_priority = false;
    };
    void dispatch_meshing_jobs(const std::vector<MeshingWorkItem>& chunks_to_mesh);
    void process_completed_meshing_jobs();
    bool meshing_jobs_active() const;
    void wait_for_generation_jobs();
    void wait_for_meshing_jobs();
    void reinitialize_noise();

    // --- T-I3-10: the ONE shared height implementation ---
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
        float final_height = 0.0f;  // after the river carve
        float pre_carve_height = 0.0f; // final surface before the river carve
        bool island_applied = false;
    };
    ShapedHeightSample ComputeShapedHeightSample(float world_x, float world_z) const;
    float ComputeShapedHeight(float world_x, float world_z) const;

    // T-I4-1: the five normalized climate dimensions consumed by the biome
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

    // T-I4-3: river influence [0, 1] from the +10 noise folded into PV space,
    // ramped across the valleys band. 0 outside the band / rivers disabled.
    // Shared by ComputeShapedHeightSample (carve) and RiverInfluenceAt (gate).
    float RiverInfluenceFromNoise(float world_x, float world_z) const;
    // T-I4-DR-river-seam-sliver: carve depth at one column for a given river
    // influence [0, 1] (pure; 0 outside the band). Factored out so the coarse
    // anti-aliased sampler can re-evaluate the carve over a footprint stencil.
    float RiverCarveAmount(float final_height, float influence) const;
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
    FastNoise::SmartNode<FastNoise::Generator> m_island_mask_generator;
    // T-I3-10 shaping control noises (only built when shaping_enabled; seed
    // offsets +3/+4/+5 for continentalness/erosion/peaks, the single warp
    // simplex is sampled with seeds +6 and +7 for the X/Z warp channels).
    FastNoise::SmartNode<FastNoise::Generator> m_continentalness_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_erosion_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_peaks_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_warp_generator;
    // T-I4-1 climate noises (seed registry: +8 temperature, +9 humidity).
    // Only built when biomes are enabled; legacy worlds never construct them.
    FastNoise::SmartNode<FastNoise::Generator> m_temperature_generator;
    FastNoise::SmartNode<FastNoise::Generator> m_humidity_generator;
    // T-I4-3 river noise (seed registry: +10). Only built when rivers enabled.
    FastNoise::SmartNode<FastNoise::Generator> m_river_generator;

    // T-I4-1 biome table (game data). Loaded from m_params.biome_table_path on
    // (re)init when biomes are enabled; empty/disabled otherwise.
    World::BiomeTable m_biome_table;
    bool m_biomes_enabled = false;

    // T-I4-4 structure template pools (game data). Loaded from
    // m_params.structures_data_dir when structures are enabled; the combined
    // content hash is stamped into m_params.structures_content_hash so the far
    // cache key tracks template changes. Public accessors expose the deterministic
    // placement query so callers (and gates) can locate sites.
    std::vector<World::StructureTemplatePool> m_structure_pools;
    bool m_structures_enabled = false;

    WaterSystem* m_water_system;
};

} // namespace Luminumbra::Systems
