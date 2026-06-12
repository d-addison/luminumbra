#include "SHIELD_WorldSystem.h"
#include "entt/entt.hpp"
#include "../world/MarchingCubes.h"
#include <array>
#include <atomic>
#include <cmath>
#include <algorithm> // Required for std::max and std::min
#include <filesystem>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "systems/PhysicsSystem.h"
#include "../core/Log.h"
#include "WaterSystem.h"

constexpr int BASE_WORK_BUDGET_EQUIVALENT = 192;
const int MAX_CHUNKS_TO_PROCESS_PER_FRAME = std::max(1, 
    static_cast<int>(BASE_WORK_BUDGET_EQUIVALENT / (static_cast<float>(Luminumbra::CHUNK_VOLUME) / 4096.0f))
);
const int MAX_COLLISION_MESHES_PER_FRAME = 16;
// Per dispatch, at most this many sorted-prefix hole-fill candidates ride the
// High job lane. The cap keeps "High" meaning near-field holes the player can
// see: during bulk drains (initial load, fast travel) nearly every candidate
// lacks an active mesh, and routing the whole backlog High starves the Normal
// lane that generation batches ride on, delaying neighbor arrival and
// inflating late-neighbor transition remesh churn.
constexpr std::size_t MAX_HIGH_PRIORITY_MESHING_JOBS_PER_DISPATCH = 32;
constexpr size_t MAX_ACTIVE_CHUNKS = 20000;
constexpr size_t STREAMING_MAX_ACTIVE_CHUNKS_BUDGET = 8192;
constexpr int STREAMING_ACTIVATION_INTERVAL_FRAMES = 4;
constexpr int STREAMING_NEAR_VERTICAL_STACK_RADIUS = 4;
constexpr int STREAMING_MID_VERTICAL_STACK_RADIUS = 12;
namespace Luminumbra::Systems {

constexpr float FLOW_CONSTANT = 0.1f;
constexpr float MIN_FLOW_DIFF = 0.001f;
constexpr float MAX_WATER_COMPRESSION = 0.2f;

namespace {

constexpr float kCaveSurfaceCapDepth = 18.0f;
constexpr float kCaveSurfaceFullDepth = 24.0f;

float smoothstep01(float value) {
    const float t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float cave_surface_blend(float terrain_density) {
    const float depth_below_surface = std::max(0.0f, -terrain_density);
    return smoothstep01((depth_below_surface - kCaveSurfaceCapDepth) /
                        (kCaveSurfaceFullDepth - kCaveSurfaceCapDepth));
}

float surface_capped_cave_density(float terrain_density, float raw_cave_noise, const TerrainGenParams& params) {
    const float cave_val = std::clamp((raw_cave_noise + 1.0f) * 0.5f, 0.0f, 1.0f);
    const float cave_density = (cave_val - params.cave_threshold) * params.cave_carve_value;
    const float cap_blend = cave_surface_blend(terrain_density);
    return terrain_density + (cave_density - terrain_density) * cap_blend;
}

float apply_cave_field(float terrain_density, float raw_cave_noise, const TerrainGenParams& params) {
    return std::max(terrain_density, surface_capped_cave_density(terrain_density, raw_cave_noise, params));
}

// Legacy single-material classifier (biome_id == kNoBiome). Kept as the exact
// float-op sequence the pre-biome implementation used so disabled worlds stay
// byte-zero; SurfaceMaterialForColumn dispatches here when no biome applies.
MaterialType classify_material_legacy(float world_y, float final_height) {
    if (world_y < 34.0f && final_height < 36.0f) {
        return MaterialType::Sand;
    }
    const float depth = final_height - world_y;
    if (depth < 1.0f) {
        return MaterialType::Grass;
    }
    if (depth < 5.0f) {
        return MaterialType::Soil;
    }
    return MaterialType::Stone;
}

void clear_completed_job_handle(JobHandle& handle) {
    if (handle.counter && handle.counter->load(std::memory_order_acquire) == 0) {
        handle = {};
    }
}

bool has_active_job(const JobHandle& handle) {
    return handle.counter && handle.counter->load(std::memory_order_acquire) > 0;
}

int horizon_lod_for_ring(int ring_distance, int surface_radius, int collision_radius) {
    if (ring_distance <= collision_radius) {
        return 0;
    }

    const int mid_lod_ring = std::max(collision_radius, (surface_radius * 2) / 3);
    return ring_distance <= mid_lod_ring ? 1 : 2;
}

int horizontal_ring_distance(int dx, int dz) {
    return std::max(std::abs(dx), std::abs(dz));
}

int horizontal_distance_sq(int dx, int dz) {
    return dx * dx + dz * dz;
}

u64 horizontal_chunk_key(int x, int z) {
    return (static_cast<u64>(static_cast<u32>(x)) << 32u) | static_cast<u32>(z);
}

int streaming_radius_for_pressure(std::size_t active_chunks, std::size_t loading_chunks, std::size_t idle_chunks, bool generation_active, bool meshing_active) {
    const std::size_t pending_chunks = loading_chunks + idle_chunks;
    if (active_chunks < 800u && !generation_active) {
        return std::min(RENDER_DISTANCE, 24);
    }
    if (generation_active || meshing_active || pending_chunks > static_cast<std::size_t>(MAX_CHUNKS_TO_PROCESS_PER_FRAME * 8)) {
        return std::min(RENDER_DISTANCE, 20);
    }
    if (active_chunks > STREAMING_MAX_ACTIVE_CHUNKS_BUDGET * 3u / 4u) {
        return std::min(RENDER_DISTANCE, 24);
    }
    return RENDER_DISTANCE;
}

void clear_streaming_state_counts(SHIELD_WorldSystem::StreamingBudgetFrameStats& stats) {
    stats.ready_chunks = 0;
    stats.renderable_chunks = 0;
    stats.idle_chunks = 0;
    stats.loading_chunks = 0;
    stats.meshing_chunks = 0;
}

void replace_chunk_collision(PhysicsSystem& physics_system, Luminumbra::Chunk& chunk) {
    const ChunkID id = chunk.get_id();
    physics_system.remove_chunk_collision(id);
    physics_system.add_chunk_collision(chunk);
    chunk.has_collision.store(true, std::memory_order_release);
}

}

SHIELD_WorldSystem::SHIELD_WorldSystem(JobSystem* job_system, WaterSystem* water_system, const TerrainGenParams& params, int seed)
    : m_job_system(job_system), m_params(params), m_seed(seed), m_water_system(water_system) {
    reinitialize_noise();
}

SHIELD_WorldSystem::~SHIELD_WorldSystem() {
    wait_for_generation_jobs();
    wait_for_meshing_jobs();
}

void SHIELD_WorldSystem::wait_for_generation_jobs() {
    if (m_job_system && m_streaming_state.generation_job_handle.counter) {
        m_job_system->wait(m_streaming_state.generation_job_handle);
    }

    m_streaming_state.generation_job_handle = {};
}

void SHIELD_WorldSystem::wait_for_meshing_jobs() {
    if (m_job_system && m_streaming_state.meshing_job_handle_high.counter) {
        m_job_system->wait(m_streaming_state.meshing_job_handle_high);
    }
    if (m_job_system && m_streaming_state.meshing_job_handle.counter) {
        m_job_system->wait(m_streaming_state.meshing_job_handle);
    }

    process_completed_meshing_jobs();
}

bool SHIELD_WorldSystem::meshing_jobs_active() const {
    return has_active_job(m_streaming_state.meshing_job_handle) ||
           has_active_job(m_streaming_state.meshing_job_handle_high);
}

void SHIELD_WorldSystem::reinitialize_noise() {
    // Terrain height is a pure function of seed/params; drop the cached
    // per-column surface spans whenever either changes.
    m_column_surface_span_cache.clear();

    // FastNoise2 uses a node-based system to build complex generators.

    // 1. Terrain Height Generator (Fractal Simplex Noise)
    auto terrain_noise = FastNoise::New<FastNoise::Simplex>();
    auto terrain_fractal = FastNoise::New<FastNoise::FractalFBm>();
    terrain_fractal->SetSource(terrain_noise);
    terrain_fractal->SetOctaveCount(m_params.octaves);
    terrain_fractal->SetLacunarity(m_params.lacunarity);
    terrain_fractal->SetGain(m_params.persistence);
    m_terrain_generator = terrain_fractal;

    // 2. Cave Generator (3D Perlin Noise)
    auto cave_noise = FastNoise::New<FastNoise::Perlin>();
    m_cave_generator = cave_noise;

    // 3. Island Mask Generator (Low-frequency Simplex)
    auto island_noise = FastNoise::New<FastNoise::Simplex>();
    m_island_mask_generator = island_noise;

    // 4. T-I3-10 shaping control noises (seed registry: +3 continentalness,
    //    +4 erosion, +5 peaks/valleys, +6/+7 domain warp X/Z). Only built when
    //    the preset opts in; legacy worlds never construct these nodes.
    m_continentalness_generator = {};
    m_erosion_generator = {};
    m_peaks_generator = {};
    m_warp_generator = {};
    if (m_params.shaping_enabled) {
        auto continental_fractal = FastNoise::New<FastNoise::FractalFBm>();
        continental_fractal->SetSource(FastNoise::New<FastNoise::Simplex>());
        continental_fractal->SetOctaveCount(3);
        m_continentalness_generator = continental_fractal;

        auto erosion_fractal = FastNoise::New<FastNoise::FractalFBm>();
        erosion_fractal->SetSource(FastNoise::New<FastNoise::Simplex>());
        erosion_fractal->SetOctaveCount(3);
        m_erosion_generator = erosion_fractal;

        auto peaks_fractal = FastNoise::New<FastNoise::FractalRidged>();
        peaks_fractal->SetSource(FastNoise::New<FastNoise::Simplex>());
        peaks_fractal->SetOctaveCount(2);
        m_peaks_generator = peaks_fractal;

        m_warp_generator = FastNoise::New<FastNoise::Simplex>();
    }

    // 4b. T-I4-3 river noise (seed registry: +10). A ridged FBm whose folded
    //     PV near-zero band carves the river channels. Built only when the
    //     preset opts in; legacy worlds never construct it.
    m_river_generator = {};
    if (m_params.rivers_enabled) {
        auto river_fractal = FastNoise::New<FastNoise::FractalRidged>();
        river_fractal->SetSource(FastNoise::New<FastNoise::Simplex>());
        river_fractal->SetOctaveCount(2);
        m_river_generator = river_fractal;
    }

    // 5. T-I4-1 biome climate noises (seed registry: +8 temperature,
    //    +9 humidity) and the biome table. Built/loaded only when the preset
    //    opted in (m_params.biomes_enabled); legacy worlds construct nothing
    //    here, so every height and material path stays bit-identical.
    m_temperature_generator = {};
    m_humidity_generator = {};
    m_biome_table = World::BiomeTable{};
    m_biomes_enabled = false;
    // Stamp the table content hash into params (0 unless a table actually
    // loads) so ComputeTerrainParamsHash mixes it into the far-LOD cache key.
    m_params.biome_table_content_hash = 0;
    if (m_params.biomes_enabled) {
        auto temperature_fractal = FastNoise::New<FastNoise::FractalFBm>();
        temperature_fractal->SetSource(FastNoise::New<FastNoise::Simplex>());
        temperature_fractal->SetOctaveCount(2);
        m_temperature_generator = temperature_fractal;

        auto humidity_fractal = FastNoise::New<FastNoise::FractalFBm>();
        humidity_fractal->SetSource(FastNoise::New<FastNoise::Simplex>());
        humidity_fractal->SetOctaveCount(2);
        m_humidity_generator = humidity_fractal;

        m_biome_table = World::BiomeTable::Load(m_params.biome_table_path);
        if (!m_biome_table.ok() || m_biome_table.empty()) {
            for (const std::string& error : m_biome_table.errors()) {
                LUMINUMBRA_CORE_WARN("biome table load error: {}", error);
            }
            LUMINUMBRA_CORE_WARN(
                "biomes requested but table '{}' failed to load; falling back to legacy single-material classification",
                m_params.biome_table_path);
            // Table failed: disable biomes so the world is still byte-zero
            // (legacy) rather than half-applied.
            m_biomes_enabled = false;
        } else {
            m_biomes_enabled = true;
            m_params.biome_table_content_hash = m_biome_table.content_hash();
        }
    }

    // T-I4-4: structure template pools. Loaded only when the preset opts in;
    // disabled worlds load nothing and contribute a zero content hash (byte-zero
    // drift). The combined content hash is stamped into params so far-LOD cache
    // keys track template changes (ComputeTerrainParamsHash mixes it in).
    m_structure_pools.clear();
    m_structures_enabled = false;
    m_params.structures_content_hash = 0;
    if (m_params.structures_enabled && !m_params.structures_data_dir.empty()) {
        const std::filesystem::path structures_root(m_params.structures_data_dir);
        std::error_code ec;
        std::vector<std::filesystem::path> type_dirs;
        if (std::filesystem::is_directory(structures_root, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(structures_root, ec)) {
                if (entry.is_directory()) {
                    type_dirs.push_back(entry.path());
                }
            }
        }
        // Sort by type name so the combined content hash is order-independent.
        std::sort(type_dirs.begin(), type_dirs.end());
        u64 combined = 14695981039346656037ull; // fnv offset basis
        for (const auto& type_dir : type_dirs) {
            const std::string type = type_dir.filename().string();
            World::StructureTemplatePool pool =
                World::LoadStructureTemplatePool(type_dir, type);
            for (const std::string& warn : pool.warnings) {
                LUMINUMBRA_CORE_WARN("structure pool '{}': {}", type, warn);
            }
            if (!pool.ok()) {
                for (const std::string& error : pool.errors) {
                    LUMINUMBRA_CORE_WARN("structure pool '{}' load error: {}", type, error);
                }
                continue;
            }
            const u64 ch = pool.content_hash;
            const auto* bytes = reinterpret_cast<const unsigned char*>(&ch);
            for (std::size_t i = 0; i < sizeof(ch); ++i) {
                combined ^= static_cast<u64>(bytes[i]);
                combined *= 1099511628211ull;
            }
            m_structure_pools.push_back(std::move(pool));
        }
        if (!m_structure_pools.empty()) {
            m_structures_enabled = true;
            m_params.structures_content_hash = combined;
        }
    }
}

std::optional<World::StructureSite> SHIELD_WorldSystem::LocateStructure(
    const std::string& type, int world_x, int world_z, int search_radius_cells) const {
    for (const World::StructureTemplatePool& pool : m_structure_pools) {
        if (pool.type == type) {
            return World::LocateNearestSite(pool, m_seed, world_x, world_z, search_radius_cells);
        }
    }
    return std::nullopt;
}

float SHIELD_WorldSystem::EvaluateShapingSpline(
    const std::vector<std::array<float, 2>>& points, float input, float fallback) {
    if (points.empty()) {
        return fallback;
    }
    if (input <= points.front()[0]) {
        return points.front()[1];
    }
    if (input >= points.back()[0]) {
        return points.back()[1];
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (input <= points[i][0]) {
            const float span = points[i][0] - points[i - 1][0];
            const float t = span > 0.0f ? (input - points[i - 1][0]) / span : 0.0f;
            return points[i - 1][1] + t * (points[i][1] - points[i - 1][1]);
        }
    }
    return points.back()[1];
}

SHIELD_WorldSystem::ShapedHeightSample SHIELD_WorldSystem::ComputeShapedHeightSample(
    float world_x, float world_z) const {
    ShapedHeightSample sample;

    float sample_x = world_x;
    float sample_z = world_z;
    float base_level = 0.0f;
    float amplitude_multiplier = 1.0f;
    float ridge = 0.0f;

    if (m_params.shaping_enabled) {
        // Domain warp (seed +6 / +7) displaces the BASE detail (and pv)
        // sample coordinates; the control channels read the unwarped point so
        // the macro structure stays stable under the warp.
        const float warp_x = m_params.domain_warp_amplitude * m_warp_generator->GenSingle2D(
            world_x * m_params.domain_warp_frequency,
            world_z * m_params.domain_warp_frequency,
            m_seed + 6);
        const float warp_z = m_params.domain_warp_amplitude * m_warp_generator->GenSingle2D(
            world_x * m_params.domain_warp_frequency,
            world_z * m_params.domain_warp_frequency,
            m_seed + 7);
        sample_x = world_x + warp_x;
        sample_z = world_z + warp_z;

        const float continentalness = m_continentalness_generator->GenSingle2D(
            world_x * m_params.continentalness_frequency,
            world_z * m_params.continentalness_frequency,
            m_seed + 3);
        const float erosion = m_erosion_generator->GenSingle2D(
            world_x * m_params.erosion_frequency,
            world_z * m_params.erosion_frequency,
            m_seed + 4);
        const float peaks_valleys = m_peaks_generator->GenSingle2D(
            sample_x * m_params.peaks_frequency,
            sample_z * m_params.peaks_frequency,
            m_seed + 5);

        base_level = EvaluateShapingSpline(m_params.continental_spline, continentalness, 0.0f);
        amplitude_multiplier = EvaluateShapingSpline(m_params.erosion_spline, erosion, 1.0f);
        // Ridge term: peaks only where erosion is low (eroded land is flat).
        const float erosion_01 = std::clamp((erosion + 1.0f) * 0.5f, 0.0f, 1.0f);
        ridge = EvaluateShapingSpline(m_params.peaks_spline, peaks_valleys, 0.0f)
            * m_params.peaks_amplitude * std::max(0.0f, 1.0f - erosion_01);
    }

    sample.base_noise = m_terrain_generator->GenSingle2D(
        sample_x * m_params.base_frequency,
        sample_z * m_params.base_frequency,
        m_seed);
    // With shaping disabled this is exactly the legacy float-op sequence:
    // height_offset + noise * amplitude (base_level == 0, multiplier == 1,
    // ridge == 0 are not applied at all on the legacy branch).
    float terrain_height;
    if (m_params.shaping_enabled) {
        terrain_height = m_params.height_offset + base_level
            + amplitude_multiplier * (sample.base_noise * m_params.base_amplitude)
            + ridge;
    } else {
        terrain_height = m_params.height_offset + sample.base_noise * m_params.base_amplitude;
    }
    sample.pre_island_height = terrain_height;
    sample.final_height = terrain_height;

    if (m_params.island_mask_enabled) {
        sample.island_applied = true;
        sample.island_noise = m_island_mask_generator->GenSingle2D(
            world_x * m_params.island_mask_frequency,
            world_z * m_params.island_mask_frequency,
            m_seed + 2);
        sample.island_mask = glm::smoothstep(0.1f, 0.25f, sample.island_noise);
        sample.final_height = glm::mix(m_params.height_offset, terrain_height, sample.island_mask);
    }

    // T-I4-3: river carve. Where the +10 PV-band river noise is in the valleys
    // band, lower the final height toward a channel floor below SEA_LEVEL so
    // the existing global water plane (SEA_LEVEL) fills the channel - no
    // WaterSystem changes (critique F5). The carve depth scales with river
    // influence (channel center deepest) and is clamped so a high ridge in the
    // band drops a bounded amount. Applied after the island mask so the channel
    // sits in the final surface, and inside this ONE shared height helper so
    // near chunks and far tiles carve identically at the seam. The branch is
    // skipped entirely when rivers are disabled (byte-zero drift).
    sample.pre_carve_height = sample.final_height;
    if (m_params.rivers_enabled) {
        const float influence = RiverInfluenceFromNoise(world_x, world_z);
        sample.final_height -= RiverCarveAmount(sample.final_height, influence);
    }

    return sample;
}

float SHIELD_WorldSystem::RiverCarveAmount(float final_height, float influence) const {
    // Carve depth at a single column given its river influence [0, 1]. Pure
    // function: lowers the surface toward a per-influence channel floor below
    // SEA_LEVEL, clamped by river_max_carve * influence so a high ridge in the
    // band drops a bounded amount. Zero when not in the river band.
    if (influence <= 0.0f) {
        return 0.0f;
    }
    const float channel_floor = SEA_LEVEL - m_params.river_depth * influence;
    if (final_height <= channel_floor) {
        return 0.0f;
    }
    return std::min(final_height - channel_floor, m_params.river_max_carve * influence);
}

float SHIELD_WorldSystem::GetTerrainHeightAtCoarse(
    float world_x, float world_z, int sample_step) const {
    // Full-res path is byte-identical to GetTerrainHeightAt (the carve is a
    // single point sample), so near chunks and the worldgen gates are
    // unaffected. The coarse anti-alias only runs for step > 1 river worlds.
    if (sample_step <= 1 || !m_params.rivers_enabled) {
        return GetTerrainHeightAt(world_x, world_z);
    }

    const ShapedHeightSample shaped = ComputeShapedHeightSample(world_x, world_z);
    const float h = shaped.pre_carve_height;
    // Average the carve over a 3x3 stencil spanning the sample_step cell so a
    // channel narrower than the step contributes only its coverage fraction of
    // the depth: the isolated full-depth notch that aliased into a near-vertical
    // sliver becomes a shallow, resolution-appropriate dip. The carve is
    // evaluated against the shared pre-carve surface (h) so every stencil tap
    // uses the same reference height; border samples of adjacent coarse tiles
    // land on the same world stencil, so a tile's shared border row still agrees
    // with its neighbor (no new tile-boundary seam).
    const float r = static_cast<float>(sample_step) * 0.5f;
    float carve_sum = 0.0f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const float sx = world_x + static_cast<float>(dx) * r;
            const float sz = world_z + static_cast<float>(dz) * r;
            carve_sum += RiverCarveAmount(h, RiverInfluenceFromNoise(sx, sz));
        }
    }
    return h - carve_sum / 9.0f;
}

float SHIELD_WorldSystem::ComputeShapedHeight(float world_x, float world_z) const {
    return ComputeShapedHeightSample(world_x, world_z).final_height;
}

void SHIELD_WorldSystem::ComputeShapedHeightGrid(
    int base_x, int base_z, int size_x, int size_z, float* out) const {
    // SIMD-batched twin of ComputeShapedHeightSample over an integer-aligned
    // column grid. MUST stay byte-for-byte equal to calling ComputeShapedHeight
    // per column (the batch-vs-scalar parity gtest pins ==), so the per-channel
    // math below mirrors ComputeShapedHeightSample EXACTLY; only the noise reads
    // move from per-point GenSingle2D to the SIMD batch entry points (which
    // produce identical float bits on this build, proven by the parity gate).
    const std::size_t count = static_cast<std::size_t>(size_x) * static_cast<std::size_t>(size_z);
    if (count == 0) {
        return;
    }

    if (!m_params.shaping_enabled) {
        // Defensive: callers only invoke this on the shaping path, but keep the
        // legacy float-op sequence here too so a stray call stays correct.
        std::vector<float> base_noise(count);
        m_terrain_generator->GenUniformGrid2D(
            base_noise.data(), base_x, base_z, size_x, size_z, m_params.base_frequency, m_seed);
        std::vector<float> island_noise;
        if (m_params.island_mask_enabled) {
            island_noise.resize(count);
            m_island_mask_generator->GenUniformGrid2D(
                island_noise.data(), base_x, base_z, size_x, size_z,
                m_params.island_mask_frequency, m_seed + 2);
        }
        for (std::size_t i = 0; i < count; ++i) {
            float h = m_params.height_offset + base_noise[i] * m_params.base_amplitude;
            if (m_params.island_mask_enabled) {
                const float mask = glm::smoothstep(0.1f, 0.25f, island_noise[i]);
                h = glm::mix(m_params.height_offset, h, mask);
            }
            out[i] = h;
        }
        return;
    }

    // --- 1. Unwarped control + warp channels on the integer lattice (SIMD). ---
    std::vector<float> warp_x_grid(count);
    std::vector<float> warp_z_grid(count);
    std::vector<float> continentalness_grid(count);
    std::vector<float> erosion_grid(count);
    m_warp_generator->GenUniformGrid2D(
        warp_x_grid.data(), base_x, base_z, size_x, size_z,
        m_params.domain_warp_frequency, m_seed + 6);
    m_warp_generator->GenUniformGrid2D(
        warp_z_grid.data(), base_x, base_z, size_x, size_z,
        m_params.domain_warp_frequency, m_seed + 7);
    m_continentalness_generator->GenUniformGrid2D(
        continentalness_grid.data(), base_x, base_z, size_x, size_z,
        m_params.continentalness_frequency, m_seed + 3);
    m_erosion_generator->GenUniformGrid2D(
        erosion_grid.data(), base_x, base_z, size_x, size_z,
        m_params.erosion_frequency, m_seed + 4);

    // --- 2. Warp-displaced sample coordinates for the base + peaks channels. ---
    // The warped coordinate is (world + amp*warp) * freq, matching the scalar
    // helper's `sample_x * m_params.<freq>`. GenPositionArray2D samples at
    // (xPos[i] + xOffset, yPos[i] + yOffset); we fold freq into the arrays and
    // pass zero offsets.
    std::vector<float> base_px(count);
    std::vector<float> base_py(count);
    std::vector<float> peaks_px(count);
    std::vector<float> peaks_py(count);
    for (int z = 0; z < size_z; ++z) {
        for (int x = 0; x < size_x; ++x) {
            const std::size_t i = static_cast<std::size_t>(x) +
                                  static_cast<std::size_t>(z) * static_cast<std::size_t>(size_x);
            const float world_x = static_cast<float>(base_x + x);
            const float world_z = static_cast<float>(base_z + z);
            const float warp_x = m_params.domain_warp_amplitude * warp_x_grid[i];
            const float warp_z = m_params.domain_warp_amplitude * warp_z_grid[i];
            const float sample_x = world_x + warp_x;
            const float sample_z = world_z + warp_z;
            base_px[i] = sample_x * m_params.base_frequency;
            base_py[i] = sample_z * m_params.base_frequency;
            peaks_px[i] = sample_x * m_params.peaks_frequency;
            peaks_py[i] = sample_z * m_params.peaks_frequency;
        }
    }

    std::vector<float> base_noise(count);
    std::vector<float> peaks_noise(count);
    m_terrain_generator->GenPositionArray2D(
        base_noise.data(), static_cast<int>(count), base_px.data(), base_py.data(),
        0.0f, 0.0f, m_seed);
    m_peaks_generator->GenPositionArray2D(
        peaks_noise.data(), static_cast<int>(count), peaks_px.data(), peaks_py.data(),
        0.0f, 0.0f, m_seed + 5);

    // --- 3. Optional island mask + river channels (cheap per-column scalar). ---
    std::vector<float> island_noise;
    if (m_params.island_mask_enabled) {
        island_noise.resize(count);
        m_island_mask_generator->GenUniformGrid2D(
            island_noise.data(), base_x, base_z, size_x, size_z,
            m_params.island_mask_frequency, m_seed + 2);
    }

    // --- 4. Combine per column, mirroring ComputeShapedHeightSample exactly. ---
    for (int z = 0; z < size_z; ++z) {
        for (int x = 0; x < size_x; ++x) {
            const std::size_t i = static_cast<std::size_t>(x) +
                                  static_cast<std::size_t>(z) * static_cast<std::size_t>(size_x);
            const float continentalness = continentalness_grid[i];
            const float erosion = erosion_grid[i];
            const float peaks_valleys = peaks_noise[i];

            const float base_level =
                EvaluateShapingSpline(m_params.continental_spline, continentalness, 0.0f);
            const float amplitude_multiplier =
                EvaluateShapingSpline(m_params.erosion_spline, erosion, 1.0f);
            const float erosion_01 = std::clamp((erosion + 1.0f) * 0.5f, 0.0f, 1.0f);
            const float ridge = EvaluateShapingSpline(m_params.peaks_spline, peaks_valleys, 0.0f)
                * m_params.peaks_amplitude * std::max(0.0f, 1.0f - erosion_01);

            float terrain_height = m_params.height_offset + base_level
                + amplitude_multiplier * (base_noise[i] * m_params.base_amplitude)
                + ridge;

            if (m_params.island_mask_enabled) {
                const float mask = glm::smoothstep(0.1f, 0.25f, island_noise[i]);
                terrain_height = glm::mix(m_params.height_offset, terrain_height, mask);
            }

            if (m_params.rivers_enabled) {
                const float world_x = static_cast<float>(base_x + x);
                const float world_z = static_cast<float>(base_z + z);
                const float influence = RiverInfluenceFromNoise(world_x, world_z);
                terrain_height -= RiverCarveAmount(terrain_height, influence);
            }

            out[i] = terrain_height;
        }
    }
}

SHIELD_WorldSystem::ClimateSample SHIELD_WorldSystem::ComputeClimateSample(
    float world_x, float world_z) const {
    ClimateSample climate;

    // continentalness/erosion REUSE the +3/+4 shaping noises sampled at the
    // unwarped column, and peaks/valleys reuses the +5 ridged noise at the
    // warped coords - byte-identical to how ComputeShapedHeightSample reads
    // them, so biome selection and terrain height agree on the same fields.
    // When shaping is off the three control noises are not built; biomes then
    // see a flat (0) control field, which still selects deterministically.
    if (m_params.shaping_enabled) {
        const float warp_x = m_params.domain_warp_amplitude * m_warp_generator->GenSingle2D(
            world_x * m_params.domain_warp_frequency,
            world_z * m_params.domain_warp_frequency,
            m_seed + 6);
        const float warp_z = m_params.domain_warp_amplitude * m_warp_generator->GenSingle2D(
            world_x * m_params.domain_warp_frequency,
            world_z * m_params.domain_warp_frequency,
            m_seed + 7);
        const float sample_x = world_x + warp_x;
        const float sample_z = world_z + warp_z;

        climate.continentalness = m_continentalness_generator->GenSingle2D(
            world_x * m_params.continentalness_frequency,
            world_z * m_params.continentalness_frequency,
            m_seed + 3);
        climate.erosion = m_erosion_generator->GenSingle2D(
            world_x * m_params.erosion_frequency,
            world_z * m_params.erosion_frequency,
            m_seed + 4);
        climate.peaks_valleys = m_peaks_generator->GenSingle2D(
            sample_x * m_params.peaks_frequency,
            sample_z * m_params.peaks_frequency,
            m_seed + 5);
    }

    // Temperature (+8) and humidity (+9) are new 2D climate noises, sampled at
    // the unwarped column so the climate macro-structure is stable.
    climate.temperature = m_temperature_generator->GenSingle2D(
        world_x * m_params.temperature_frequency,
        world_z * m_params.temperature_frequency,
        m_seed + 8);
    climate.humidity = m_humidity_generator->GenSingle2D(
        world_x * m_params.humidity_frequency,
        world_z * m_params.humidity_frequency,
        m_seed + 9);
    return climate;
}

u8 SHIELD_WorldSystem::BiomeIdAt(float world_x, float world_z) const {
    if (!m_biomes_enabled || m_biome_table.empty()) {
        return World::kNoBiome;
    }
    const ClimateSample climate = ComputeClimateSample(world_x, world_z);
    return m_biome_table.lookup(climate.continentalness,
                                climate.erosion,
                                climate.peaks_valleys,
                                climate.temperature,
                                climate.humidity);
}

const World::BiomeReverb& SHIELD_WorldSystem::BiomeReverbAt(float world_x, float world_z) const {
    static const World::BiomeReverb kDefaultReverb{};
    if (!m_biomes_enabled || m_biome_table.empty()) {
        return kDefaultReverb;
    }
    return m_biome_table.reverb_for(BiomeIdAt(world_x, world_z));
}

float SHIELD_WorldSystem::RiverInfluenceFromNoise(float world_x, float world_z) const {
    if (!m_params.rivers_enabled || !m_river_generator) {
        return 0.0f;
    }
    // +10 ridged noise -> Minecraft 1.18 weirdness->PV fold: PV = 1 - |3|r| - 2|.
    // The valleys band [river_pv_min, river_pv_max] selects the river course;
    // the influence ramps from 0 at the band edge to 1 at the band center, so
    // the channel has a soft width/wobble driven by the same noise.
    const float r = m_river_generator->GenSingle2D(
        world_x * m_params.river_frequency,
        world_z * m_params.river_frequency,
        m_seed + 10);
    const float pv = 1.0f - std::abs(3.0f * std::abs(r) - 2.0f);
    if (pv < m_params.river_pv_min || pv > m_params.river_pv_max) {
        return 0.0f;
    }
    const float band = m_params.river_pv_max - m_params.river_pv_min;
    if (band <= 0.0f) {
        return 1.0f;
    }
    // Triangular ramp peaking at the band center (channel thalweg).
    const float t = (pv - m_params.river_pv_min) / band; // 0..1 across the band
    const float influence = 1.0f - std::abs(2.0f * t - 1.0f);
    return std::clamp(influence, 0.0f, 1.0f);
}

float SHIELD_WorldSystem::RiverInfluenceAt(float world_x, float world_z) const {
    return RiverInfluenceFromNoise(world_x, world_z);
}

MaterialType SHIELD_WorldSystem::SurfaceMaterialForColumn(
    float world_y, float final_height, u8 biome_id, bool river_bank) const {
    // No biome (disabled / unmatched): the exact legacy classifier. River banks
    // need a palette, so with no biome they keep the legacy classification
    // (rivers only ship on biome-enabled presets; the bank distinction is a
    // no-op for legacy worlds, preserving byte-zero drift).
    if (!m_biomes_enabled || biome_id == World::kNoBiome || m_biome_table.empty()) {
        return classify_material_legacy(world_y, final_height);
    }

    const World::BiomeSurfacePalette& palette = m_biome_table.palette_for(biome_id);
    // Band selection mirrors the legacy classifier exactly (same thresholds,
    // same float-op sequence) but maps each band to the biome palette: the
    // waterline/sand band -> underwater, the surface skin -> top, the shallow
    // subsurface -> filler, the deep interior -> depth.
    if (world_y < 34.0f && final_height < 36.0f) {
        return static_cast<MaterialType>(palette.underwater);
    }
    const float depth = final_height - world_y;
    if (depth < 1.0f) {
        // T-I4-3: above-water river bank skin uses the filler (muddy bank)
        // rather than the top (grass).
        return static_cast<MaterialType>(river_bank ? palette.filler : palette.top);
    }
    if (depth < 5.0f) {
        return static_cast<MaterialType>(palette.filler);
    }
    return static_cast<MaterialType>(palette.depth);
}

MaterialType SHIELD_WorldSystem::SurfaceVertexMaterial(
    float world_x, float world_z, float terrain_height) const {
    // Byte-exact twin of MarchingCubes::GetTerrainMaterialAt for a surface
    // vertex, given the already-known terrain height. GetTerrainMaterialAt:
    //   1. sample = SampleWorldGenLayers(x, terrain_height - 0.35, z);
    //      at depth 0.35 m the surface-capped cave field never carves (cap
    //      blend == 0 below 18 m), so final_density < 0 -> the sample is solid
    //      and sample.material == SurfaceMaterialForColumn(y=terrain_height-0.35,
    //      final_height, BiomeIdAt, RiverInfluence>0.25). For !rivers_enabled
    //      final_height == terrain_height (the cached heightmap value).
    //   2. if that material is not Air/Water, return it.
    //   3. otherwise reclassify at depth 0.1 m:
    //      SurfaceMaterialForColumn(y=terrain_height-0.1, final_height, ...).
    // Reproducing those two SurfaceMaterialForColumn calls here skips the
    // redundant shaped-height recompute inside SampleWorldGenLayers.
    const u8 biome_id = BiomeIdAt(world_x, world_z);
    const bool river_bank = RiverInfluenceFromNoise(world_x, world_z) > 0.25f;
    const MaterialType solid_material = SurfaceMaterialForColumn(
        terrain_height - 0.35f, terrain_height, biome_id, river_bank);
    if (solid_material != MaterialType::Air && solid_material != MaterialType::Water) {
        return solid_material;
    }
    return SurfaceMaterialForColumn(
        terrain_height - 0.1f, terrain_height, biome_id, river_bank);
}

void SHIELD_WorldSystem::ComputeShapedHeightsAtPositions(
    const float* xs, const float* zs, std::size_t count, float* out) const {
    if (count == 0) {
        return;
    }
    if (!m_params.shaping_enabled) {
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = ComputeShapedHeight(xs[i], zs[i]);
        }
        return;
    }
    // Warp channels (unwarped lattice), then base/peaks at warped coords, then
    // continentalness/erosion at unwarped coords - all via GenPositionArray2D.
    std::vector<float> wx_in(count), wz_in(count);
    for (std::size_t i = 0; i < count; ++i) {
        wx_in[i] = xs[i] * m_params.domain_warp_frequency;
        wz_in[i] = zs[i] * m_params.domain_warp_frequency;
    }
    std::vector<float> warp_x(count), warp_z(count);
    m_warp_generator->GenPositionArray2D(warp_x.data(), static_cast<int>(count),
        wx_in.data(), wz_in.data(), 0.0f, 0.0f, m_seed + 6);
    m_warp_generator->GenPositionArray2D(warp_z.data(), static_cast<int>(count),
        wx_in.data(), wz_in.data(), 0.0f, 0.0f, m_seed + 7);

    std::vector<float> cont_x(count), cont_y(count), eros_x(count), eros_y(count);
    std::vector<float> base_x(count), base_y(count), peaks_x(count), peaks_y(count);
    for (std::size_t i = 0; i < count; ++i) {
        cont_x[i] = xs[i] * m_params.continentalness_frequency;
        cont_y[i] = zs[i] * m_params.continentalness_frequency;
        eros_x[i] = xs[i] * m_params.erosion_frequency;
        eros_y[i] = zs[i] * m_params.erosion_frequency;
        const float sx = xs[i] + m_params.domain_warp_amplitude * warp_x[i];
        const float sz = zs[i] + m_params.domain_warp_amplitude * warp_z[i];
        base_x[i] = sx * m_params.base_frequency;
        base_y[i] = sz * m_params.base_frequency;
        peaks_x[i] = sx * m_params.peaks_frequency;
        peaks_y[i] = sz * m_params.peaks_frequency;
    }
    std::vector<float> continentalness(count), erosion(count), base_noise(count), peaks_noise(count);
    m_continentalness_generator->GenPositionArray2D(continentalness.data(), static_cast<int>(count),
        cont_x.data(), cont_y.data(), 0.0f, 0.0f, m_seed + 3);
    m_erosion_generator->GenPositionArray2D(erosion.data(), static_cast<int>(count),
        eros_x.data(), eros_y.data(), 0.0f, 0.0f, m_seed + 4);
    m_terrain_generator->GenPositionArray2D(base_noise.data(), static_cast<int>(count),
        base_x.data(), base_y.data(), 0.0f, 0.0f, m_seed);
    m_peaks_generator->GenPositionArray2D(peaks_noise.data(), static_cast<int>(count),
        peaks_x.data(), peaks_y.data(), 0.0f, 0.0f, m_seed + 5);

    std::vector<float> island_noise;
    for (std::size_t i = 0; i < count; ++i) {
        const float base_level =
            EvaluateShapingSpline(m_params.continental_spline, continentalness[i], 0.0f);
        const float amplitude_multiplier =
            EvaluateShapingSpline(m_params.erosion_spline, erosion[i], 1.0f);
        const float erosion_01 = std::clamp((erosion[i] + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float ridge = EvaluateShapingSpline(m_params.peaks_spline, peaks_noise[i], 0.0f)
            * m_params.peaks_amplitude * std::max(0.0f, 1.0f - erosion_01);
        float h = m_params.height_offset + base_level
            + amplitude_multiplier * (base_noise[i] * m_params.base_amplitude) + ridge;
        if (m_params.island_mask_enabled) {
            const float in = m_island_mask_generator->GenSingle2D(
                xs[i] * m_params.island_mask_frequency, zs[i] * m_params.island_mask_frequency, m_seed + 2);
            const float mask = glm::smoothstep(0.1f, 0.25f, in);
            h = glm::mix(m_params.height_offset, h, mask);
        }
        if (m_params.rivers_enabled) {
            const float influence = RiverInfluenceFromNoise(xs[i], zs[i]);
            h -= RiverCarveAmount(h, influence);
        }
        out[i] = h;
    }
}

void SHIELD_WorldSystem::ClassifyVertexMaterials(
    const Vec3* positions, std::size_t count, u32* out_materials) const {
    if (count == 0) {
        return;
    }
    // Fallback (legacy / no shaping): per-vertex, byte-unchanged. This mirrors
    // MarchingCubes::GetTerrainMaterialAt exactly via SampleWorldGenLayers, so a
    // disabled-shaping world keeps identical materials.
    if (!m_params.shaping_enabled) {
        for (std::size_t i = 0; i < count; ++i) {
            const Vec3& p = positions[i];
            const WorldGenLayerSample sample =
                SampleWorldGenLayers(p - Vec3(0.0f, 0.25f, 0.0f));
            MaterialType material = sample.material;
            if (material == MaterialType::Air || material == MaterialType::Water) {
                const float th = sample.final_height;
                const u8 biome_id = BiomeIdAt(p.x, p.z);
                const bool river_bank = RiverInfluenceFromNoise(p.x, p.z) > 0.25f;
                material = SurfaceMaterialForColumn(p.y - 0.1f, th, biome_id, river_bank);
            }
            out_materials[i] = static_cast<u32>(material);
        }
        return;
    }

    // --- Batched shaped height + climate for every vertex (x,z). ---
    // GenPositionArray2D samples at (xPos[i] + xOffset, yPos[i] + yOffset); we
    // fold the per-channel frequency into the coordinate arrays (zero offsets),
    // mirroring the scalar helpers' `coord * frequency`.
    std::vector<float> warp_xf(count), warp_zf(count);   // warp coords (unwarped * warp_freq)
    for (std::size_t i = 0; i < count; ++i) {
        warp_xf[i] = positions[i].x * m_params.domain_warp_frequency;
        warp_zf[i] = positions[i].z * m_params.domain_warp_frequency;
    }
    std::vector<float> warp_x(count), warp_z(count);
    m_warp_generator->GenPositionArray2D(warp_x.data(), static_cast<int>(count),
        warp_xf.data(), warp_zf.data(), 0.0f, 0.0f, m_seed + 6);
    m_warp_generator->GenPositionArray2D(warp_z.data(), static_cast<int>(count),
        warp_xf.data(), warp_zf.data(), 0.0f, 0.0f, m_seed + 7);

    // continentalness/erosion read the UNWARPED column; base/peaks read the
    // warp-displaced column (matching ComputeShapedHeightSample).
    std::vector<float> cont_x(count), cont_y(count), eros_x(count), eros_y(count);
    std::vector<float> base_x(count), base_y(count), peaks_x(count), peaks_y(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float wx = positions[i].x;
        const float wz = positions[i].z;
        cont_x[i] = wx * m_params.continentalness_frequency;
        cont_y[i] = wz * m_params.continentalness_frequency;
        eros_x[i] = wx * m_params.erosion_frequency;
        eros_y[i] = wz * m_params.erosion_frequency;
        const float sx = wx + m_params.domain_warp_amplitude * warp_x[i];
        const float sz = wz + m_params.domain_warp_amplitude * warp_z[i];
        base_x[i] = sx * m_params.base_frequency;
        base_y[i] = sz * m_params.base_frequency;
        peaks_x[i] = sx * m_params.peaks_frequency;
        peaks_y[i] = sz * m_params.peaks_frequency;
    }
    std::vector<float> continentalness(count), erosion(count), base_noise(count), peaks_noise(count);
    m_continentalness_generator->GenPositionArray2D(continentalness.data(), static_cast<int>(count),
        cont_x.data(), cont_y.data(), 0.0f, 0.0f, m_seed + 3);
    m_erosion_generator->GenPositionArray2D(erosion.data(), static_cast<int>(count),
        eros_x.data(), eros_y.data(), 0.0f, 0.0f, m_seed + 4);
    m_terrain_generator->GenPositionArray2D(base_noise.data(), static_cast<int>(count),
        base_x.data(), base_y.data(), 0.0f, 0.0f, m_seed);
    m_peaks_generator->GenPositionArray2D(peaks_noise.data(), static_cast<int>(count),
        peaks_x.data(), peaks_y.data(), 0.0f, 0.0f, m_seed + 5);

    // Climate (temperature +8, humidity +9) for the biome lookup, when biomes
    // are enabled. Sampled at the unwarped column.
    std::vector<float> temperature, humidity;
    if (m_biomes_enabled && !m_biome_table.empty()) {
        temperature.resize(count);
        humidity.resize(count);
        std::vector<float> temp_x(count), temp_y(count), hum_x(count), hum_y(count);
        for (std::size_t i = 0; i < count; ++i) {
            temp_x[i] = positions[i].x * m_params.temperature_frequency;
            temp_y[i] = positions[i].z * m_params.temperature_frequency;
            hum_x[i] = positions[i].x * m_params.humidity_frequency;
            hum_y[i] = positions[i].z * m_params.humidity_frequency;
        }
        m_temperature_generator->GenPositionArray2D(temperature.data(), static_cast<int>(count),
            temp_x.data(), temp_y.data(), 0.0f, 0.0f, m_seed + 8);
        m_humidity_generator->GenPositionArray2D(humidity.data(), static_cast<int>(count),
            hum_x.data(), hum_y.data(), 0.0f, 0.0f, m_seed + 9);
    }

    // --- Per-vertex combine + classification (mirrors the scalar path). ---
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3& p = positions[i];

        // Shaped final height at (x,z) - identical math to
        // ComputeShapedHeightSample (no island mask in the shipped shaped
        // presets; include it for completeness when enabled).
        const float base_level =
            EvaluateShapingSpline(m_params.continental_spline, continentalness[i], 0.0f);
        const float amplitude_multiplier =
            EvaluateShapingSpline(m_params.erosion_spline, erosion[i], 1.0f);
        const float erosion_01 = std::clamp((erosion[i] + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float ridge = EvaluateShapingSpline(m_params.peaks_spline, peaks_noise[i], 0.0f)
            * m_params.peaks_amplitude * std::max(0.0f, 1.0f - erosion_01);
        float final_height = m_params.height_offset + base_level
            + amplitude_multiplier * (base_noise[i] * m_params.base_amplitude) + ridge;
        if (m_params.island_mask_enabled) {
            const float island_noise = m_island_mask_generator->GenSingle2D(
                p.x * m_params.island_mask_frequency, p.z * m_params.island_mask_frequency, m_seed + 2);
            const float mask = glm::smoothstep(0.1f, 0.25f, island_noise);
            final_height = glm::mix(m_params.height_offset, final_height, mask);
        }
        if (m_params.rivers_enabled) {
            const float influence = RiverInfluenceFromNoise(p.x, p.z);
            final_height -= RiverCarveAmount(final_height, influence);
        }

        // Biome id (batched climate + the reused +3/+4/+5 shaping noises).
        u8 biome_id = World::kNoBiome;
        if (m_biomes_enabled && !m_biome_table.empty()) {
            biome_id = m_biome_table.lookup(continentalness[i], erosion[i], peaks_noise[i],
                                            temperature[i], humidity[i]);
        }
        const bool river_bank = RiverInfluenceFromNoise(p.x, p.z) > 0.25f;

        // Stage 1: solid-branch classification at depth 0.25 m below the vertex
        // (SampleWorldGenLayers samples P - (0,0.25,0)). The surface-capped cave
        // field cannot carve within 18 m of the surface, so a sample within the
        // mesh band is solid; the cave eval only matters deep, where the marching
        // cubes vertex never sits. Reproduce the cave/solid test exactly.
        const float world_y = p.y - 0.25f;
        const float terrain_density = world_y - final_height;
        float final_density = terrain_density;
        if (m_params.caves_enabled) {
            const float cave_noise = m_cave_generator->GenSingle3D(
                (p.x) * m_params.cave_frequency,
                (world_y) * m_params.cave_frequency,
                (p.z) * m_params.cave_frequency, m_seed + 1);
            final_density = std::max(terrain_density,
                surface_capped_cave_density(terrain_density, cave_noise, m_params));
        }
        MaterialType material = MaterialType::Air;
        if (final_density < 0.0f) {
            material = SurfaceMaterialForColumn(world_y, final_height, biome_id, river_bank);
        }
        if (material != MaterialType::Air && material != MaterialType::Water) {
            out_materials[i] = static_cast<u32>(material);
            continue;
        }
        // Stage 2: GetTerrainMaterialAt fallback at depth 0.1 m below the
        // ORIGINAL vertex y.
        out_materials[i] = static_cast<u32>(
            SurfaceMaterialForColumn(p.y - 0.1f, final_height, biome_id, river_bank));
    }
}

SHIELD_WorldSystem::ColumnSurfaceSpan SHIELD_WorldSystem::compute_column_surface_span(int chunk_x, int chunk_z) const {
    const float base_x = static_cast<float>(chunk_x * CHUNK_SIZE_X);
    const float base_z = static_cast<float>(chunk_z * CHUNK_SIZE_Z);
    const float max_x = base_x + static_cast<float>(CHUNK_SIZE_X);
    const float max_z = base_z + static_cast<float>(CHUNK_SIZE_Z);
    const float center_x = base_x + CHUNK_SIZE_X * 0.5f;
    const float center_z = base_z + CHUNK_SIZE_Z * 0.5f;

    // T-I4-DR-shaping-perf: batch the 5 footprint height samples (center + 4
    // corners) through the SIMD position-array path instead of 5 scalar
    // GenSingle2D sweeps. Byte-identical heights (same shaped helper), but the
    // per-column span cost - the dominant cold-frame streaming cost on shaped
    // presets - drops ~5x.
    const std::array<float, 5> sample_xs{center_x, base_x, max_x, base_x, max_x};
    const std::array<float, 5> sample_zs{center_z, base_z, base_z, max_z, max_z};
    std::array<float, 5> heights{};
    ComputeShapedHeightsAtPositions(sample_xs.data(), sample_zs.data(), 5, heights.data());
    const float center_height = heights[0];
    float min_height = center_height;
    float max_height = center_height;
    for (std::size_t i = 1; i < heights.size(); ++i) {
        min_height = std::min(min_height, heights[i]);
        max_height = std::max(max_height, heights[i]);
    }

    ColumnSurfaceSpan span;
    span.center_y = world_to_chunk_coords(Vec3(center_x, center_height, center_z)).y;
    span.min_y = world_to_chunk_coords(Vec3(center_x, min_height, center_z)).y;
    span.max_y = world_to_chunk_coords(Vec3(center_x, max_height, center_z)).y;
    // T-I4-1: cache the surface biome id for the column (kNoBiome when biomes
    // are disabled). Sampled at the column center, matching center_y.
    span.biome_id = BiomeIdAt(center_x, center_z);
    return span;
}

SHIELD_WorldSystem::ColumnSurfaceSpan SHIELD_WorldSystem::column_surface_span(int chunk_x, int chunk_z) {
    const u64 key = horizontal_chunk_key(chunk_x, chunk_z);
    const auto it = m_column_surface_span_cache.find(key);
    if (it != m_column_surface_span_cache.end()) {
        return it->second;
    }

    const ColumnSurfaceSpan span = compute_column_surface_span(chunk_x, chunk_z);
    m_column_surface_span_cache.emplace(key, span);
    return span;
}

int SHIELD_WorldSystem::get_lod_level_for_distance(float dist) const {
    for (const auto& lod : m_lod_levels) {
        if (dist <= lod.distance) {
            return lod.level;
        }
    }
    // If it's further than our max LOD distance, use the lowest detail level
    return m_lod_levels.back().level;
}

int SHIELD_WorldSystem::get_required_lod_for_chunk(
    const IVec3& coords,
    const Vec3& chunk_center,
    const Vec3& camera_position,
    int current_lod)
{
    // Chunks in the surface band of their column (the chunks that actually
    // contain the terrain isosurface) select LOD from HORIZONTAL distance
    // only, so the surface never crosses a vertical LOD boundary. With 3D
    // distance, vertically stacked surface chunks straddle LOD rings and the
    // surface crossing the horizontal chunk seam is contoured at two
    // different sample steps, opening void slivers that no X/Z transition
    // skirt can cover. Chunks far above/below the surface produce little or
    // no geometry, cannot open surface seams, and keep the cheaper
    // 3D-distance LOD so deep/air columns do not inflate the meshing load.
    // This mirrors the per-ring LOD already used by EnsureSurfaceReadyNear.
    // The band covers the full column surface SPAN (T-I3-2) plus the same
    // +-1 margin as before, so an entire cliff face keeps a single LOD per
    // column (preserving the vertical-seam invariant above). On flat terrain
    // span.min_y == span.max_y == center and this is exactly the old band.
    constexpr int kSurfaceLodBandChunks = 1;
    const ColumnSurfaceSpan span = column_surface_span(coords.x, coords.z);
    float dist;
    if (coords.y >= span.min_y - kSurfaceLodBandChunks &&
        coords.y <= span.max_y + kSurfaceLodBandChunks) {
        dist = glm::distance(
            Vec3(camera_position.x, 0.0f, camera_position.z),
            Vec3(chunk_center.x, 0.0f, chunk_center.z));
    } else {
        dist = glm::distance(camera_position, chunk_center);
    }

    const int band_lod = get_lod_level_for_distance(dist);

    // Asymmetric hysteresis (T-I3-19): a chunk PROMOTES to a finer LOD the
    // moment it enters the finer band (dist <= D, unchanged), but DEMOTES to
    // a coarser LOD only once the camera has receded one full chunk past the
    // band edge it currently occupies (dist > D + margin). Margin = one chunk
    // footprint (CHUNK_SIZE_X = 16 m): chunk centers are quantized to 16 m,
    // so camera dither smaller than a chunk (the oscillation failure mode)
    // can no longer flip a band-edge chunk back and forth, while the margin
    // stays far below the narrowest band width (192 m) so a receding camera
    // demotes after at most one extra chunk of travel. The hysteresis only
    // applies to chunks that already hold a meshed LOD (current_lod >= 0);
    // first-time assignment, the generation/initial-load path and every
    // static-camera (settled) configuration use the raw band thresholds, so
    // steady-state LOD assignment - and therefore settled snapshot/meshing
    // determinism - is byte-identical to the pre-hysteresis behavior.
    if (current_lod >= 0 && band_lod > current_lod) {
        constexpr float kDemoteHysteresisMeters = static_cast<float>(CHUNK_SIZE_X);
        const float hold_distance = m_lod_levels[static_cast<std::size_t>(current_lod)].distance
                                    + kDemoteHysteresisMeters;
        if (dist <= hold_distance) {
            return current_lod;
        }
    }
    return band_lod;
}

int SHIELD_WorldSystem::get_lod_step_for_level(int lod_level) const {
    for (const auto& lod : m_lod_levels) {
        if (lod.level == lod_level) {
            return std::max(1, lod.step);
        }
    }

    return std::max(1, m_lod_levels.back().step);
}

std::vector<IVec3> SHIELD_WorldSystem::GetInitialChunkLoadList(const Vec3& center_pos) const {
    const IVec3 spawn_chunk_coords = world_to_chunk_coords(center_pos);

    const int INITIAL_LOAD_RADIUS = 12;
    int min_y = std::numeric_limits<int>::max();
    int max_y = std::numeric_limits<int>::min();

    struct InitialChunkCandidate {
        IVec3 coords;
        int vertical_rank = 0;
        int ring_distance = 0;
        int horizontal_distance_sq = 0;
    };

    std::vector<InitialChunkCandidate> candidates;
    candidates.reserve(1875);

    for (int dz = -INITIAL_LOAD_RADIUS; dz <= INITIAL_LOAD_RADIUS; ++dz) {
        for (int dx = -INITIAL_LOAD_RADIUS; dx <= INITIAL_LOAD_RADIUS; ++dx) {
            const int chunk_x = spawn_chunk_coords.x + dx;
            const int chunk_z = spawn_chunk_coords.z + dz;
            // Full column surface span (T-I3-2) plus the same +-1 margin the
            // fixed {0, -1, 1} offsets provided on flat terrain, so steep
            // spawn neighborhoods preload their cliff-wall chunks too.
            const ColumnSurfaceSpan span = compute_column_surface_span(chunk_x, chunk_z);

            for (int y = span.min_y - 1; y <= span.max_y + 1; ++y) {
                candidates.push_back({
                    IVec3(chunk_x, y, chunk_z),
                    std::abs(y - span.center_y),
                    horizontal_ring_distance(dx, dz),
                    horizontal_distance_sq(dx, dz)
                });
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const InitialChunkCandidate& a, const InitialChunkCandidate& b) {
        if (a.vertical_rank != b.vertical_rank) {
            return a.vertical_rank < b.vertical_rank;
        }
        if (a.ring_distance != b.ring_distance) {
            return a.ring_distance < b.ring_distance;
        }
        if (a.horizontal_distance_sq != b.horizontal_distance_sq) {
            return a.horizontal_distance_sq < b.horizontal_distance_sq;
        }
        if (a.coords.y != b.coords.y) {
            return a.coords.y < b.coords.y;
        }
        if (a.coords.x != b.coords.x) {
            return a.coords.x < b.coords.x;
        }
        return a.coords.z < b.coords.z;
    });

    std::vector<IVec3> initial_chunks;
    initial_chunks.reserve(candidates.size());
    for (const InitialChunkCandidate& candidate : candidates) {
        initial_chunks.push_back(candidate.coords);
    }

    // Debug: Log initial chunk loading
    const float spawn_terrain_height = GetTerrainHeightAt(center_pos.x, center_pos.z);
    LUMINUMBRA_CORE_WARN("Loading {} chunks - Spawn Y={}, Terrain Y={}, Chunk range Y={}..{}", 
        initial_chunks.size(), center_pos.y, spawn_terrain_height, min_y * CHUNK_SIZE_Y, max_y * CHUNK_SIZE_Y);
    
    return initial_chunks;
}

void SHIELD_WorldSystem::update(entt::registry& registry, const Vec3& camera_position, PhysicsSystem* physics_system) {
    clear_completed_job_handle(m_streaming_state.generation_job_handle);
    process_completed_meshing_jobs();

    m_last_streaming_budget_stats = {};
    m_last_streaming_budget_stats.update_interval_frames = STREAMING_ACTIVATION_INTERVAL_FRAMES;
    m_last_streaming_budget_stats.requested_render_radius = RENDER_DISTANCE;
    m_last_streaming_budget_stats.max_active_chunks_budget = STREAMING_MAX_ACTIVE_CHUNKS_BUDGET;
    m_last_streaming_budget_stats.generation_job_active = has_active_job(m_streaming_state.generation_job_handle);
    m_last_streaming_budget_stats.meshing_job_active = meshing_jobs_active();
    m_last_streaming_budget_stats.active_chunks_before = m_streaming_state.chunks.size();
    clear_streaming_state_counts(m_last_streaming_budget_stats);
    for (auto const& [id, chunk_ptr] : m_streaming_state.chunks) {
        (void)id;
        if (!chunk_ptr) {
            continue;
        }

        const ChunkState state = chunk_ptr->get_state();
        if (state == ChunkState::Ready) {
            ++m_last_streaming_budget_stats.ready_chunks;
            if (!chunk_ptr->mesh_vertices.empty() && !chunk_ptr->mesh_indices.empty()) {
                ++m_last_streaming_budget_stats.renderable_chunks;
            }
        } else if (state == ChunkState::Idle) {
            ++m_last_streaming_budget_stats.idle_chunks;
        } else if (state == ChunkState::Loading) {
            ++m_last_streaming_budget_stats.loading_chunks;
        } else if (state == ChunkState::Meshing) {
            ++m_last_streaming_budget_stats.meshing_chunks;
        }
    }

    // Engine streaming stays fully asynchronous: on a camera discontinuity
    // (teleport, or a per-frame jump forced by a slow renderer driving a
    // wall-clock camera path) the throttled activation pass + async meshing
    // catch the destination near field up over the next few updates. A previous
    // engine-side SYNCHRONOUS catch-up here (ee4f378) pulled the near surface
    // band ready via EnsureSurfaceReadyNear whenever the streaming chunk jumped
    // > 2 chebyshev, but churn workloads (which jump every frame) turned that
    // into a per-frame synchronous meshing spike (chunk_churn p99 8 -> 89 ms,
    // an 11x PerfRegression). It is gone. Capture-driven scenarios that need a
    // guaranteed-renderable near field in the exact frame they screenshot call
    // EnsureSurfaceReadyNear explicitly BEFORE the capture (the API is public
    // and already used by the server boot path and the LodGround/FarLod
    // harness); gameplay teleports stream in normally.

    // Decouple the expensive chunk activation/deactivation logic from the frame rate.
    m_update_tick_counter++;
    if (m_update_tick_counter >= STREAMING_ACTIVATION_INTERVAL_FRAMES) {
        update_chunk_activation(camera_position, physics_system);
        m_update_tick_counter = 0;
    }

    // Step 2: Update the water system using the now-current list of active chunks.
    // This MUST happen before meshing jobs are dispatched.
    if (m_water_system) {
        m_water_system->update(registry, m_streaming_state.chunks);
    }

    // Step 3: Schedule meshing jobs for chunks that need it.
    struct MeshingCandidate {
        std::shared_ptr<Luminumbra::Chunk> chunk;
        int required_lod = 0;
        bool has_active_mesh = false;
        bool terrain_mesh_required = true;
        int vertical_surface_distance = 0;
        float distance_sq = 0.0f;
    };

    std::vector<MeshingCandidate> meshing_candidates;
    meshing_candidates.reserve(MAX_CHUNKS_TO_PROCESS_PER_FRAME * 2);
    std::vector<MeshingWorkItem> chunks_to_mesh_jobs;
    chunks_to_mesh_jobs.reserve(MAX_CHUNKS_TO_PROCESS_PER_FRAME);

    // Snapshot meshed-chunk LODs per horizontal column so the candidate loop
    // below can cheaply detect coarse chunks whose transition skirts went
    // stale because a finer neighbor arrived AFTER this chunk was meshed.
    // Without this, a seam crack opened by a late-arriving finer neighbor
    // persists until the coarse chunk happens to remesh for another reason.
    struct MeshedColumnEntry {
        int y = 0;
        int lod = 0;
    };
    std::unordered_map<u64, std::vector<MeshedColumnEntry>> meshed_columns;
    meshed_columns.reserve(m_streaming_state.chunks.size());
    for (auto const& [id, chunk_ptr] : m_streaming_state.chunks) {
        (void)id;
        if (!chunk_ptr || chunk_ptr->mesh_vertices.empty() || chunk_ptr->mesh_indices.empty()) {
            continue;
        }
        const int lod = chunk_ptr->current_lod.load(std::memory_order_acquire);
        if (lod < 0) {
            continue;
        }
        const IVec3 coords = chunk_ptr->get_coords();
        meshed_columns[horizontal_chunk_key(coords.x, coords.z)].push_back({coords.y, lod});
    }

    // Returns the transition faces this chunk needs against current neighbor
    // LODs that are NOT yet baked into its mesh. Mirrors the neighbor criteria
    // used by dispatch_meshing_jobs so a triggered remesh always converges.
    auto missing_transition_faces = [&](const Luminumbra::Chunk& chunk, int lod_level) -> u8 {
        const IVec3 coords = chunk.get_coords();
        u8 required = Luminumbra::World::MarchingCubes::kNoTransitionFaces;
        auto require_face_if_neighbor_is_finer = [&](int dx, int dz, Luminumbra::World::MarchingCubes::TerrainTransitionFace face) {
            const auto column_it = meshed_columns.find(horizontal_chunk_key(coords.x + dx, coords.z + dz));
            if (column_it == meshed_columns.end()) {
                return;
            }
            for (const MeshedColumnEntry& entry : column_it->second) {
                if (entry.lod < lod_level || (entry.lod != lod_level && entry.y != coords.y)) {
                    required |= static_cast<Luminumbra::World::MarchingCubes::TerrainTransitionFaceMask>(face);
                    return;
                }
            }
        };
        require_face_if_neighbor_is_finer(-1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMinX);
        require_face_if_neighbor_is_finer(1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMaxX);
        require_face_if_neighbor_is_finer(0, -1, Luminumbra::World::MarchingCubes::TransitionFaceMinZ);
        require_face_if_neighbor_is_finer(0, 1, Luminumbra::World::MarchingCubes::TransitionFaceMaxZ);
        return static_cast<u8>(required & static_cast<u8>(~chunk.applied_transition_faces.load(std::memory_order_acquire)));
    };

    // Chunk state logging removed from hot path - too expensive

    for (auto const& [id, chunk_ptr] : m_streaming_state.chunks) {
        (void)id;
        bool needs_meshing = false;
        bool terrain_mesh_required = true;
        int required_lod = -1;

        ChunkState state = chunk_ptr->get_state();
        const int pending_lod = chunk_ptr->pending_lod.load(std::memory_order_acquire);
        if (pending_lod >= 0) {
            continue;
        }
        
        if (state == Luminumbra::ChunkState::Idle) {
            needs_meshing = true;
        } else if (state == Luminumbra::ChunkState::Ready) {
            Vec3 chunk_center = (Vec3(chunk_ptr->get_coords()) + 0.5f) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
            // T-I3-19: pass the meshed LOD so demotions go through the
            // asymmetric hysteresis band (promote at D, demote at D + margin).
            required_lod = get_required_lod_for_chunk(
                chunk_ptr->get_coords(), chunk_center, camera_position,
                chunk_ptr->current_lod.load());

            if (required_lod != chunk_ptr->current_lod.load()) {
                needs_meshing = true;
                terrain_mesh_required = true;
            } else if (required_lod == 0 &&
                       chunk_ptr->has_water_sim.load(std::memory_order_acquire) &&
                       !chunk_ptr->water_mesh_generated.load(std::memory_order_acquire))
            {
                needs_meshing = true;
                terrain_mesh_required = false;
            } else if (get_lod_step_for_level(required_lod) > 1 &&
                       !chunk_ptr->mesh_vertices.empty() && !chunk_ptr->mesh_indices.empty() &&
                       missing_transition_faces(*chunk_ptr, required_lod) != Luminumbra::World::MarchingCubes::kNoTransitionFaces)
            {
                // A finer neighbor arrived after this coarse chunk was meshed:
                // remesh so the now-required boundary transition skirts are
                // baked in, closing the persistent LOD seam crack.
                needs_meshing = true;
                terrain_mesh_required = true;
            }
        }
        
        if (needs_meshing) {
            const Vec3 chunk_center = (Vec3(chunk_ptr->get_coords()) + 0.5f) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
            if (required_lod == -1) {
                // Never-meshed chunks carry current_lod == -1, so this stays
                // the raw band assignment; a previously meshed chunk that
                // re-enters here keeps the same hysteresis as the Ready path.
                required_lod = get_required_lod_for_chunk(
                    chunk_ptr->get_coords(), chunk_center, camera_position,
                    chunk_ptr->current_lod.load());
            }
            // The column-surface cache samples the same chunk positions, so
            // this replaces per-candidate fractal noise evaluations with a
            // hash lookup (terrain height is a pure function of seed/params).
            // Vertical rank is the distance OUTSIDE the column surface span
            // (0 for every chunk the isosurface passes through), so cliff
            // wall chunks drain with the same hole-fill priority as the
            // center surface chunk.
            const ColumnSurfaceSpan span = column_surface_span(chunk_ptr->get_coords().x, chunk_ptr->get_coords().z);
            const int chunk_y = chunk_ptr->get_coords().y;
            const int vertical_surface_distance = chunk_y > span.max_y
                ? chunk_y - span.max_y
                : (chunk_y < span.min_y ? span.min_y - chunk_y : 0);
            const IVec3 delta = chunk_ptr->get_coords() - world_to_chunk_coords(camera_position);
            const float distance_sq = static_cast<float>(horizontal_distance_sq(delta.x, delta.z) + delta.y * delta.y);
            meshing_candidates.push_back({
                chunk_ptr,
                required_lod,
                state == Luminumbra::ChunkState::Ready && !chunk_ptr->mesh_vertices.empty() && !chunk_ptr->mesh_indices.empty(),
                terrain_mesh_required,
                vertical_surface_distance,
                distance_sq
            });
        }
    }

    m_last_streaming_budget_stats.meshing_candidates = meshing_candidates.size();
    std::size_t terrain_meshing_backlog = 0;
    for (const MeshingCandidate& candidate : meshing_candidates) {
        if (candidate.terrain_mesh_required) {
            ++terrain_meshing_backlog;
        }
    }
    const bool meshing_job_active = meshing_jobs_active();
    m_last_streaming_budget_stats.meshing_job_active = meshing_job_active;
    // Scale the per-dispatch meshing batch with the standing terrain backlog:
    // deep backlogs (initial load, fast travel) dispatch larger batches so
    // more of the backlog is in flight per dispatch, while shallow
    // steady-state backlogs keep the small batches that preserve LOD/hole-fill
    // responsiveness. The batch is still the sorted-candidate prefix, so the
    // hole-fill-first ordering and per-chunk LOD selection are unchanged -
    // only how quickly the same work drains. Measured on the 20s
    // EnduranceStreamDrain scenario: max_deferred_age_frames 28 -> 12 and
    // cumulative_deferred_meshing ~15k -> ~5k versus a fixed budget; a 3x cap
    // adds little over 2x while tripling worst-case batch latency, so cap
    // at 2x.
    int meshing_budget = MAX_CHUNKS_TO_PROCESS_PER_FRAME;
    if (terrain_meshing_backlog > static_cast<std::size_t>(MAX_CHUNKS_TO_PROCESS_PER_FRAME)) {
        meshing_budget = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(MAX_CHUNKS_TO_PROCESS_PER_FRAME) * 2u,
            terrain_meshing_backlog / 2u
        ));
        meshing_budget = std::max(meshing_budget, MAX_CHUNKS_TO_PROCESS_PER_FRAME);
    }
    m_last_streaming_budget_stats.meshing_budget = meshing_job_active ? 0 : meshing_budget;

    if (!meshing_candidates.empty() && !meshing_job_active) {
        std::sort(meshing_candidates.begin(), meshing_candidates.end(), [](const MeshingCandidate& a, const MeshingCandidate& b) {
            if (a.has_active_mesh != b.has_active_mesh) {
                return !a.has_active_mesh;
            }
            if (a.vertical_surface_distance != b.vertical_surface_distance) {
                return a.vertical_surface_distance < b.vertical_surface_distance;
            }
            if (a.distance_sq != b.distance_sq) {
                return a.distance_sq < b.distance_sq;
            }
            if (a.required_lod != b.required_lod) {
                return a.required_lod < b.required_lod;
            }
            return a.chunk->get_id() < b.chunk->get_id();
        });

        const std::size_t budget = std::min(
            meshing_candidates.size(),
            static_cast<std::size_t>(m_last_streaming_budget_stats.meshing_budget)
        );
        for (std::size_t i = 0; i < budget; ++i) {
            // The sort places hole-fill candidates (no active mesh) first,
            // nearest surface band first, so the capped High prefix is
            // exactly the near-field holes.
            const bool high_priority = !meshing_candidates[i].has_active_mesh &&
                i < MAX_HIGH_PRIORITY_MESHING_JOBS_PER_DISPATCH;
            chunks_to_mesh_jobs.push_back({
                meshing_candidates[i].chunk,
                meshing_candidates[i].required_lod,
                meshing_candidates[i].terrain_mesh_required,
                high_priority
            });
        }

        m_last_streaming_budget_stats.scheduled_meshing = chunks_to_mesh_jobs.size();
        m_last_streaming_budget_stats.deferred_meshing = meshing_candidates.size() - chunks_to_mesh_jobs.size();
    }

    if (!chunks_to_mesh_jobs.empty() && !meshing_job_active) {
        dispatch_meshing_jobs(chunks_to_mesh_jobs);
    }
    
    // Step 4. Time-slice the creation of expensive physics colliders on the main thread
    if (physics_system) {
        int collision_meshes_created_this_frame = 0;
        for (auto const& [id, chunk_ptr] : m_streaming_state.chunks) {
            if (chunk_ptr->get_state() == ChunkState::Ready && !chunk_ptr->has_collision.load()) {
                // Only create collision for the highest LOD terrain mesh
                if (chunk_ptr->current_lod.load() == 0 && !chunk_ptr->mesh_vertices.empty() && !chunk_ptr->mesh_indices.empty()) {
                    replace_chunk_collision(*physics_system, *chunk_ptr);

                    collision_meshes_created_this_frame++;
                    if (collision_meshes_created_this_frame >= MAX_COLLISION_MESHES_PER_FRAME) {
                        break;
                    }
                }
            }
        }
    }

    m_last_streaming_budget_stats.active_chunks_after = m_streaming_state.chunks.size();
    clear_streaming_state_counts(m_last_streaming_budget_stats);
    for (auto const& [id, chunk_ptr] : m_streaming_state.chunks) {
        (void)id;
        if (!chunk_ptr) {
            continue;
        }

        const ChunkState state = chunk_ptr->get_state();
        if (state == ChunkState::Ready) {
            ++m_last_streaming_budget_stats.ready_chunks;
            if (!chunk_ptr->mesh_vertices.empty() && !chunk_ptr->mesh_indices.empty()) {
                ++m_last_streaming_budget_stats.renderable_chunks;
            }
        } else if (state == ChunkState::Idle) {
            ++m_last_streaming_budget_stats.idle_chunks;
        } else if (state == ChunkState::Loading) {
            ++m_last_streaming_budget_stats.loading_chunks;
        } else if (state == ChunkState::Meshing) {
            ++m_last_streaming_budget_stats.meshing_chunks;
        }
    }

    const std::size_t queue_depth = m_last_streaming_budget_stats.loading_chunks + terrain_meshing_backlog;
    ++m_streaming_telemetry_stats.frames_observed;
    m_streaming_telemetry_stats.last_queue_depth = queue_depth;
    m_streaming_telemetry_stats.peak_queue_depth = std::max(m_streaming_telemetry_stats.peak_queue_depth, queue_depth);
    m_streaming_telemetry_stats.peak_meshing_candidates = std::max(
        m_streaming_telemetry_stats.peak_meshing_candidates,
        m_last_streaming_budget_stats.meshing_candidates
    );
    m_streaming_telemetry_stats.cumulative_scheduled_meshing += m_last_streaming_budget_stats.scheduled_meshing;
    m_streaming_telemetry_stats.cumulative_deferred_meshing += m_last_streaming_budget_stats.deferred_meshing;
    if (m_last_streaming_budget_stats.meshing_candidates > m_last_streaming_budget_stats.scheduled_meshing) {
        ++m_deferred_backlog_age_frames;
    } else {
        m_deferred_backlog_age_frames = 0;
    }
    m_streaming_telemetry_stats.max_deferred_age_frames = std::max(
        m_streaming_telemetry_stats.max_deferred_age_frames,
        m_deferred_backlog_age_frames
    );
}

void SHIELD_WorldSystem::update_chunk_activation(const Vec3& player_pos, PhysicsSystem* physics_system) {
    const IVec3 camera_chunk = world_to_chunk_coords(player_pos);
    struct GenerationCandidate {
        IVec3 coords;
        bool surface = false;
        int ring_distance = 0;
        int horizontal_distance_sq = 0;
        int vertical_rank = 0;
        int target_step = 1;
    };

    const int target_radius = streaming_radius_for_pressure(
        m_streaming_state.chunks.size(),
        m_last_streaming_budget_stats.loading_chunks,
        m_last_streaming_budget_stats.idle_chunks,
        has_active_job(m_streaming_state.generation_job_handle),
        meshing_jobs_active()
    );

    m_last_streaming_budget_stats.target_render_radius = target_radius;
    m_last_streaming_budget_stats.generation_job_active = has_active_job(m_streaming_state.generation_job_handle);
    m_last_streaming_budget_stats.generation_budget = m_last_streaming_budget_stats.generation_job_active
        ? 0
        : MAX_CHUNKS_TO_PROCESS_PER_FRAME;

    std::vector<GenerationCandidate> to_create;
    to_create.reserve(static_cast<std::size_t>((target_radius * 2 + 1) * (target_radius * 2 + 1)));
    std::unordered_set<ChunkID> seen_candidate_ids;
    seen_candidate_ids.reserve(to_create.capacity() * 2u);

    auto add_candidate = [&](const IVec3& coords, bool surface, int ring_distance, int horizontal_dist2, int vertical_rank) {
        // NOTE (T-I3-2): the active-chunk budget is no longer applied here.
        // Enforcing it during enumeration capped candidates in row-major scan
        // order, so when the wanted set exceeded the budget (mountains preset
        // with surface spans) the dropped chunks were a directional bite out
        // of one side of the disc. The budget is applied after the sort below,
        // so the trimmed candidates are always the lowest-priority (farthest
        // ring, deepest vertical rank) ones - a thin rim at the horizon edge.
        const ChunkID id = Chunk::calculate_id(coords);
        if (m_streaming_state.chunks.find(id) != m_streaming_state.chunks.end()) {
            return;
        }
        if (!seen_candidate_ids.insert(id).second) {
            return;
        }

        // Generation intent (T-I3-1): chunks whose required meshing step is
        // coarse (> 1) generate surface-band data only - no interior SDF, no
        // 3D cave grid. Promotion to LOD0 backfills the full SDF via the
        // meshing dispatch, so a conservative step here is only a perf cost.
        const Vec3 chunk_center = (Vec3(coords) + 0.5f) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
        const int required_lod = get_required_lod_for_chunk(coords, chunk_center, player_pos);
        const int target_step = get_lod_step_for_level(required_lod);

        to_create.push_back({coords, surface, ring_distance, horizontal_dist2, vertical_rank, target_step});
    };

    for (int dz = -target_radius; dz <= target_radius; ++dz) {
        for (int dx = -target_radius; dx <= target_radius; ++dx) {
            const int horizontal_dist2 = horizontal_distance_sq(dx, dz);
            if (horizontal_dist2 > target_radius * target_radius) {
                continue;
            }

            const int chunk_x = camera_chunk.x + dx;
            const int chunk_z = camera_chunk.z + dz;
            // 5-point span sample (T-I3-2); the cache persists for the
            // lifetime of seed/params.
            const ColumnSurfaceSpan span = column_surface_span(chunk_x, chunk_z);
            const int ring_distance = horizontal_ring_distance(dx, dz);

            ++m_last_streaming_budget_stats.target_surface_columns;
            // Activate EVERY chunk-Y the column's isosurface passes through,
            // at every ring. Beyond ring 12 the old code streamed exactly one
            // chunk per column; any coarse cell whose surface lay in another
            // chunk-Y had no owner (the coarse mesher's per-cell ownership
            // test drops it) - a permanent horizon hole. Cliff walls between
            // columns (>16 m steps) live in the span interior and were never
            // streamed at any ring. Flat terrain has span size 1, so this
            // costs nothing where the old behavior was already correct.
            // When the full wanted set exceeds the active-chunk budget (the
            // mountains preset at large radii), the post-sort budget
            // truncation below trims the farthest-ring candidates - never
            // the near field.
            for (int y = span.min_y; y <= span.max_y; ++y) {
                add_candidate(
                    IVec3(chunk_x, y, chunk_z),
                    true,
                    ring_distance,
                    horizontal_dist2,
                    std::abs(y - span.center_y)
                );
            }

            if (ring_distance <= STREAMING_NEAR_VERTICAL_STACK_RADIUS) {
                add_candidate(IVec3(chunk_x, span.min_y - 1, chunk_z), false, ring_distance, horizontal_dist2, 1);
                add_candidate(IVec3(chunk_x, span.max_y + 1, chunk_z), false, ring_distance, horizontal_dist2, 1);
            } else if (ring_distance <= STREAMING_MID_VERTICAL_STACK_RADIUS) {
                add_candidate(IVec3(chunk_x, span.min_y - 1, chunk_z), false, ring_distance, horizontal_dist2, 2);
                add_candidate(IVec3(chunk_x, span.max_y + 1, chunk_z), false, ring_distance, horizontal_dist2, 2);
            }
        }
    }

    std::sort(to_create.begin(), to_create.end(), [](const GenerationCandidate& a, const GenerationCandidate& b) {
        if (a.surface != b.surface) {
            return a.surface;
        }
        if (a.ring_distance != b.ring_distance) {
            return a.ring_distance < b.ring_distance;
        }
        if (a.horizontal_distance_sq != b.horizontal_distance_sq) {
            return a.horizontal_distance_sq < b.horizontal_distance_sq;
        }
        if (a.vertical_rank != b.vertical_rank) {
            return a.vertical_rank < b.vertical_rank;
        }
        if (a.coords.y != b.coords.y) {
            return a.coords.y < b.coords.y;
        }
        if (a.coords.x != b.coords.x) {
            return a.coords.x < b.coords.x;
        }
        return a.coords.z < b.coords.z;
    });

    m_last_streaming_budget_stats.generation_candidates = to_create.size();
    for (const GenerationCandidate& candidate : to_create) {
        if (candidate.surface) {
            ++m_last_streaming_budget_stats.surface_generation_candidates;
        } else {
            ++m_last_streaming_budget_stats.vertical_generation_candidates;
        }
    }

    std::vector<ChunkGenerationRequest> generate_now;
    generate_now.reserve(static_cast<std::size_t>(m_last_streaming_budget_stats.generation_budget));
    if (m_last_streaming_budget_stats.generation_budget > 0) {
        // Active-chunk budget, applied to the sorted candidate prefix so the
        // highest-priority (nearest, surface-first) chunks always win the
        // remaining slots.
        const std::size_t budget_headroom =
            m_streaming_state.chunks.size() < STREAMING_MAX_ACTIVE_CHUNKS_BUDGET
                ? STREAMING_MAX_ACTIVE_CHUNKS_BUDGET - m_streaming_state.chunks.size()
                : 0u;
        const std::size_t budget = std::min(
            std::min(to_create.size(), budget_headroom),
            static_cast<std::size_t>(m_last_streaming_budget_stats.generation_budget)
        );
        for (std::size_t i = 0; i < budget; ++i) {
            generate_now.push_back({to_create[i].coords, to_create[i].target_step});
            if (to_create[i].surface) {
                ++m_last_streaming_budget_stats.surface_generation_scheduled;
            } else {
                ++m_last_streaming_budget_stats.vertical_generation_scheduled;
            }
        }
    }

    m_last_streaming_budget_stats.scheduled_generation = generate_now.size();
    m_last_streaming_budget_stats.deferred_generation = to_create.size() - generate_now.size();
    if (!generate_now.empty()) {
        dispatch_generation_jobs(generate_now);
    }

    // 5. Unload chunks that are now out of range.
    std::vector<ChunkID> to_unload;

    // Add a small buffer (hysteresis) to the render distance to prevent rapid
    // loading/unloading of chunks at the very edge of the view distance.
    const int UNLOAD_DISTANCE_XZ = RENDER_DISTANCE + 2;
    const int UNLOAD_DISTANCE_UP = RENDER_DISTANCE_UP + 2;
    const int UNLOAD_DISTANCE_DOWN = RENDER_DISTANCE_DOWN + 2;

    for (const auto& [id, chunk_ptr] : m_streaming_state.chunks) {
        const IVec3 coords = chunk_ptr->get_coords();
        const IVec3 d = coords - camera_chunk;
        if (std::abs(d.x) > UNLOAD_DISTANCE_XZ || std::abs(d.z) > UNLOAD_DISTANCE_XZ) {
            to_unload.push_back(id);
            continue;
        }

        if (d.y <= UNLOAD_DISTANCE_UP && d.y >= -UNLOAD_DISTANCE_DOWN) {
            continue;
        }

        // Vertical-unload exemption (T-I3-2): a chunk inside its column's
        // surface span (+-1 stack margin) holds the terrain isosurface the
        // player can see, regardless of how far above/below the CAMERA it
        // sits. The old camera-relative test evicted mountain summits more
        // than 160 m above a valley camera every activation pass, then the
        // surface scan immediately re-added them - a load/unload churn loop
        // that left permanent holes on tall peaks. Only chunks vertically
        // outside their column's surface band may be evicted by the Y test;
        // the XZ test above is unchanged.
        const ColumnSurfaceSpan span = column_surface_span(coords.x, coords.z);
        const bool inside_surface_band = coords.y >= span.min_y - 1 && coords.y <= span.max_y + 1;
        if (!inside_surface_band) {
            to_unload.push_back(id);
        }
    }

    m_last_streaming_budget_stats.unloaded_chunks = to_unload.size();
    for (ChunkID id : to_unload) {
        if (physics_system) {
            physics_system->remove_chunk_collision(id);
        }
        m_streaming_state.chunks.erase(id);
    }
}

bool SHIELD_WorldSystem::EnsureCollisionReadyNear(const Vec3& world_pos, PhysicsSystem* physics_system, int horizontal_radius) {
    return EnsureSurfaceReadyNear(world_pos, physics_system, horizontal_radius, horizontal_radius);
}

bool SHIELD_WorldSystem::EnsureSurfaceReadyNear(const Vec3& world_pos, PhysicsSystem* physics_system, int surface_radius, int collision_radius) {
    if (!physics_system) {
        return false;
    }

    wait_for_generation_jobs();
    wait_for_meshing_jobs();

    const IVec3 center_chunk = world_to_chunk_coords(world_pos);
    const int radius = std::max(0, surface_radius);
    const int collision_range = std::max(0, collision_radius);
    struct SurfaceHorizonChunk {
        std::shared_ptr<Luminumbra::Chunk> chunk;
        IVec2 offset{0};
        int vertical_rank = 0;
        int lod = 0;
        int step = 1;
    };

    std::vector<SurfaceHorizonChunk> chunks_to_build;
    std::vector<SurfaceHorizonChunk> chunks_to_consider_for_collision;
    std::array<std::size_t, 3> lod_counts{0u, 0u, 0u};
    const std::size_t surface_capacity = static_cast<std::size_t>((radius * 2 + 1) * (radius * 2 + 1) * 5);
    chunks_to_build.reserve(surface_capacity);
    chunks_to_consider_for_collision.reserve(surface_capacity);

    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int chunk_x = center_chunk.x + dx;
            const int chunk_z = center_chunk.z + dz;
            // Full column surface span (T-I3-2) with the same +-1 stack
            // margin the fixed {-1, 0, 1} band provided on flat terrain;
            // steep columns additionally cover every chunk-Y the isosurface
            // passes through so cliff walls are meshed before world enter.
            const ColumnSurfaceSpan span = column_surface_span(chunk_x, chunk_z);
            const int ring_distance = std::max(std::abs(dx), std::abs(dz));
            const int lod = horizon_lod_for_ring(ring_distance, radius, collision_range);
            const int step = get_lod_step_for_level(lod);
            lod_counts[static_cast<std::size_t>(std::clamp(lod, 0, 2))]++;

            for (int chunk_y = span.min_y - 1; chunk_y <= span.max_y + 1; ++chunk_y) {
                const IVec3 coords(chunk_x, chunk_y, chunk_z);
                const ChunkID id = Chunk::calculate_id(coords);

                auto it = m_streaming_state.chunks.find(id);
                if (it == m_streaming_state.chunks.end()) {
                    auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
                    it = m_streaming_state.chunks.emplace(id, std::move(chunk)).first;
                }

                auto& chunk = it->second;
                SurfaceHorizonChunk surface_chunk{
                    chunk,
                    IVec2(dx, dz),
                    std::abs(chunk_y - span.center_y),
                    lod,
                    step
                };
                chunks_to_consider_for_collision.push_back(surface_chunk);
                if (chunk->get_state() != ChunkState::Ready || chunk->current_lod.load() != lod ||
                    chunk->mesh_vertices.empty() || chunk->mesh_indices.empty())
                {
                    chunk->set_state(ChunkState::Loading);
                    chunks_to_build.push_back(surface_chunk);
                }
            }
        }
    }

    std::vector<Luminumbra::Job> build_jobs;
    build_jobs.reserve(chunks_to_build.size());
    for (const SurfaceHorizonChunk& build_chunk : chunks_to_build) {
        build_jobs.emplace_back([this, build_chunk]() {
            const auto& chunk = build_chunk.chunk;
            const bool needs_full_sdf = build_chunk.step <= 1;
            const bool missing_required_data = needs_full_sdf
                ? chunk->sdf_data.empty()
                : (chunk->sdf_data.empty() && chunk->heightmap_data.empty());
            if (missing_required_data) {
                // Chunks restored from a world save arrive with voxel data
                // already populated (possibly carrying player edits);
                // regeneration would clobber those edits. Generation is a
                // pure function of seed/params, so skipping it for any chunk
                // that already has the data its step needs is also a no-op
                // for fresh chunks that merely need a LOD rebuild. Coarse
                // (step > 1) horizon chunks generate the surface band only
                // (T-I3-1): the heightfield mesher and seam fallback never
                // read interior SDF.
                GenerateChunkData(*chunk, build_chunk.step);
            }
            chunk->set_state(ChunkState::Meshing);
            Luminumbra::World::MarchingCubes::PolygoniseTerrain(*this, *chunk, 0.0f, build_chunk.step);
            chunk->applied_transition_faces.store(0, std::memory_order_release);
            chunk->water_mesh_vertices.clear();
            chunk->water_mesh_indices.clear();
            chunk->current_lod.store(build_chunk.lod);
            chunk->mesh_version++;
            chunk->has_collision.store(false);
            chunk->set_state(ChunkState::Ready);
        });
    }

    if (m_job_system && build_jobs.size() > 128u) {
        m_job_system->wait(m_job_system->dispatch_batch(build_jobs));
    } else {
        for (auto& job : build_jobs) {
            job();
        }
    }

    std::unordered_map<u64, const SurfaceHorizonChunk*> surface_by_xz;
    surface_by_xz.reserve(chunks_to_consider_for_collision.size());
    for (const SurfaceHorizonChunk& surface_chunk : chunks_to_consider_for_collision) {
        if (surface_chunk.vertical_rank != 0) {
            continue;
        }
        const IVec3 coords = surface_chunk.chunk->get_coords();
        surface_by_xz[horizontal_chunk_key(coords.x, coords.z)] = &surface_chunk;
    }

    for (const SurfaceHorizonChunk& surface_chunk : chunks_to_consider_for_collision) {
        // Per-cell surface ownership can split a column's coarse mesh across
        // vertically adjacent chunks, so every meshed coarse chunk needs its
        // boundary transition skirts - not just the vertical_rank==0 chunk.
        if (surface_chunk.step <= 1 || surface_chunk.chunk->mesh_vertices.empty()) {
            continue;
        }

        const IVec3 coords = surface_chunk.chunk->get_coords();
        auto transition_faces = Luminumbra::World::MarchingCubes::kNoTransitionFaces;
        auto add_face_if_neighbor_is_finer = [&](int dx, int dz, Luminumbra::World::MarchingCubes::TerrainTransitionFace face) {
            const auto neighbor_it = surface_by_xz.find(horizontal_chunk_key(coords.x + dx, coords.z + dz));
            if (neighbor_it == surface_by_xz.end()) {
                return;
            }

            const SurfaceHorizonChunk& neighbor = *neighbor_it->second;
            const bool neighbor_is_finer = neighbor.lod < surface_chunk.lod;
            const bool vertical_mixed_lod_pair =
                neighbor.lod != surface_chunk.lod &&
                neighbor.chunk->get_coords().y != coords.y;
            if (neighbor_is_finer || vertical_mixed_lod_pair) {
                transition_faces |= static_cast<Luminumbra::World::MarchingCubes::TerrainTransitionFaceMask>(face);
            }
        };

        add_face_if_neighbor_is_finer(-1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMinX);
        add_face_if_neighbor_is_finer(1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMaxX);
        add_face_if_neighbor_is_finer(0, -1, Luminumbra::World::MarchingCubes::TransitionFaceMinZ);
        add_face_if_neighbor_is_finer(0, 1, Luminumbra::World::MarchingCubes::TransitionFaceMaxZ);

        if (transition_faces != Luminumbra::World::MarchingCubes::kNoTransitionFaces) {
            Luminumbra::World::MarchingCubes::AddBoundaryTransitionSkirts(
                *surface_chunk.chunk,
                surface_chunk.step,
                transition_faces
            );
            surface_chunk.chunk->applied_transition_faces.fetch_or(transition_faces, std::memory_order_acq_rel);
        }
    }

    std::size_t collision_count = 0;
    for (const SurfaceHorizonChunk& surface_chunk : chunks_to_consider_for_collision) {
        const auto& chunk = surface_chunk.chunk;
        const IVec2& offset = surface_chunk.offset;
        if (surface_chunk.lod == 0 &&
            surface_chunk.vertical_rank == 0 &&
            std::abs(offset.x) <= collision_range && std::abs(offset.y) <= collision_range &&
            !chunk->has_collision.load())
        {
            replace_chunk_collision(*physics_system, *chunk);
            ++collision_count;
        }
    }

    LUMINUMBRA_CORE_INFO("Initial surface horizon ready: radius={}, surface_chunks={}, rebuilt={}, lod0={}, lod1={}, lod2={}, collision_radius={}, collisions={}",
        radius, chunks_to_consider_for_collision.size(), chunks_to_build.size(),
        lod_counts[0], lod_counts[1], lod_counts[2], collision_range, collision_count);
    return true;
}

float SHIELD_WorldSystem::GetTerrainHeightAt(float world_x, float world_z) const {
    // T-I3-10: delegates to the one shared height implementation.
    return ComputeShapedHeightSample(world_x, world_z).final_height;
}

WorldGenLayerSample SHIELD_WorldSystem::SampleWorldGenLayers(const Vec3& world_pos) const {
    WorldGenLayerSample sample;
    sample.world_pos = world_pos;

    // T-I3-10: heights come from the one shared implementation so this sample
    // path stays exactly consistent with GetTerrainHeightAt and both
    // GenerateChunkData batch loops.
    const ShapedHeightSample height = ComputeShapedHeightSample(world_pos.x, world_pos.z);
    sample.base_noise = height.base_noise;
    sample.base_height = height.pre_island_height;
    sample.final_height = height.final_height;
    sample.island_applied = height.island_applied;
    sample.island_noise = height.island_noise;
    sample.island_mask = height.island_mask;

    sample.terrain_density = world_pos.y - sample.final_height;
    sample.final_density = sample.terrain_density;
    sample.cave_density = sample.terrain_density;

    if (m_params.caves_enabled) {
        sample.caves_applied = true;
        sample.cave_noise = m_cave_generator->GenSingle3D(
            world_pos.x * m_params.cave_frequency,
            world_pos.y * m_params.cave_frequency,
            world_pos.z * m_params.cave_frequency,
            m_seed + 1
        );
        sample.cave_value = std::clamp((sample.cave_noise + 1.0f) * 0.5f, 0.0f, 1.0f);
        sample.cave_density = surface_capped_cave_density(sample.terrain_density, sample.cave_noise, m_params);
        sample.final_density = std::max(sample.terrain_density, sample.cave_density);
    }

    sample.solid = sample.final_density < 0.0f;
    if (!sample.solid) {
        sample.material = MaterialType::Air;
    } else {
        // T-I4-2: biome-aware surface material. When biomes are disabled the
        // biome id is kNoBiome and SurfaceMaterialForColumn reproduces the
        // legacy classifier bit-for-bit. The column biome is resolved from the
        // surface (world_x/world_z), not the sample's depth.
        const u8 biome_id = BiomeIdAt(world_pos.x, world_pos.z);
        // River banks (T-I4-3): a column under meaningful river influence lays
        // its above-water skin as the biome filler. The threshold keeps the
        // bank a thin rim around the channel rather than the whole valley.
        const bool river_bank = RiverInfluenceFromNoise(world_pos.x, world_pos.z) > 0.25f;
        sample.material = SurfaceMaterialForColumn(world_pos.y, sample.final_height, biome_id, river_bank);
    }
    return sample;
}

float SHIELD_WorldSystem::get_density_at_from_precalculated(const Vec3& world_pos, float terrain_height) const {
    float terrain_density = world_pos.y - terrain_height;
    if (m_params.caves_enabled) {
        // <<< FIX: The function returns the value directly.
        float cave_noise = m_cave_generator->GenSingle3D(world_pos.x * m_params.cave_frequency, world_pos.y * m_params.cave_frequency, world_pos.z * m_params.cave_frequency, m_seed + 1);
        terrain_density = apply_cave_field(terrain_density, cave_noise, m_params);
    }
    return terrain_density;
}

float SHIELD_WorldSystem::get_density_at(const Vec3& world_pos) const {
    return SampleWorldGenLayers(world_pos).final_density;
}

std::vector<Luminumbra::Chunk*> SHIELD_WorldSystem::get_renderable_chunks() {
    clear_completed_job_handle(m_streaming_state.generation_job_handle);
    process_completed_meshing_jobs();

    std::vector<Luminumbra::Chunk*> renderable;
    renderable.reserve(m_streaming_state.chunks.size());
    
    int total_chunks = 0;
    int ready_chunks = 0;
    int chunks_with_mesh = 0;
    
    for (auto const& [id, chunk_ptr] : m_streaming_state.chunks) {
        total_chunks++;
        if (chunk_ptr->get_state() == Luminumbra::ChunkState::Ready) {
            ready_chunks++;
            if (!chunk_ptr->mesh_vertices.empty()) {
                chunks_with_mesh++;
                renderable.push_back(chunk_ptr.get());
                
                // Debug logging disabled to prevent segfault from static atomics
            }
        }
    }
    
    // Performance logging removed from hot path - use profiler instead
    
    return renderable;
}

SHIELD_WorldSystem::RuntimeChunkStats SHIELD_WorldSystem::get_runtime_chunk_stats() const {
    RuntimeChunkStats stats;
    stats.total_chunks = m_streaming_state.chunks.size();
    stats.generation_job_active = has_active_job(m_streaming_state.generation_job_handle);
    stats.meshing_job_active = meshing_jobs_active();

    for (const auto& [id, chunk_ptr] : m_streaming_state.chunks) {
        (void)id;
        if (!chunk_ptr) {
            continue;
        }

        switch (chunk_ptr->get_state()) {
            case ChunkState::Unloaded:
                ++stats.unloaded_chunks;
                break;
            case ChunkState::Loading:
                ++stats.loading_chunks;
                break;
            case ChunkState::Idle:
                ++stats.idle_chunks;
                break;
            case ChunkState::Meshing:
                ++stats.meshing_chunks;
                break;
            case ChunkState::Ready:
                ++stats.ready_chunks;
                break;
            case ChunkState::Unloading:
                ++stats.unloading_chunks;
                break;
        }

        if (!chunk_ptr->mesh_vertices.empty() && !chunk_ptr->mesh_indices.empty()) {
            ++stats.renderable_chunks;
        }
        if (chunk_ptr->has_collision.load(std::memory_order_acquire)) {
            ++stats.collision_chunks;
        }

        stats.terrain_vertex_count += chunk_ptr->mesh_vertices.size();
        stats.terrain_index_count += chunk_ptr->mesh_indices.size();
        stats.water_vertex_count += chunk_ptr->water_mesh_vertices.size();
        stats.water_index_count += chunk_ptr->water_mesh_indices.size();
        stats.terrain_payload_bytes +=
            (chunk_ptr->mesh_vertices.size() + chunk_ptr->water_mesh_vertices.size()) * sizeof(VoxelVertex) +
            (chunk_ptr->mesh_indices.size() + chunk_ptr->water_mesh_indices.size()) * sizeof(u32);
        stats.sdf_payload_bytes += chunk_ptr->sdf_data.size() * sizeof(f32);
        stats.heightmap_payload_bytes += chunk_ptr->heightmap_data.size() * sizeof(f32);
        if (chunk_ptr->sdf_data.empty() && !chunk_ptr->heightmap_data.empty()) {
            ++stats.sdf_skipped_chunks;
        }
    }

    return stats;
}

SHIELD_WorldSystem::CameraLocalCoverageStats SHIELD_WorldSystem::get_camera_local_coverage_stats(
    const Vec3& camera_position,
    int horizontal_radius) const
{
    CameraLocalCoverageStats stats;
    stats.camera_position = camera_position;
    stats.camera_chunk = world_to_chunk_coords(camera_position);
    stats.horizontal_radius = std::max(0, horizontal_radius);
    stats.terrain_height_under_camera = GetTerrainHeightAt(camera_position.x, camera_position.z);
    stats.camera_height_above_terrain = camera_position.y - stats.terrain_height_under_camera;
    stats.surface_chunk_under_camera = world_to_chunk_coords(Vec3(
        camera_position.x,
        stats.terrain_height_under_camera,
        camera_position.z
    ));

    const IVec3 center_chunk = stats.camera_chunk;
    for (int dz = -stats.horizontal_radius; dz <= stats.horizontal_radius; ++dz) {
        for (int dx = -stats.horizontal_radius; dx <= stats.horizontal_radius; ++dx) {
            const int chunk_x = center_chunk.x + dx;
            const int chunk_z = center_chunk.z + dz;
            const float sample_x = static_cast<float>(chunk_x * CHUNK_SIZE_X) + CHUNK_SIZE_X * 0.5f;
            const float sample_z = static_cast<float>(chunk_z * CHUNK_SIZE_Z) + CHUNK_SIZE_Z * 0.5f;
            const float terrain_height = GetTerrainHeightAt(sample_x, sample_z);
            const int chunk_y = world_to_chunk_coords(Vec3(sample_x, terrain_height, sample_z)).y;
            const IVec3 coords(chunk_x, chunk_y, chunk_z);
            const bool is_center_surface_chunk =
                coords.x == stats.surface_chunk_under_camera.x &&
                coords.y == stats.surface_chunk_under_camera.y &&
                coords.z == stats.surface_chunk_under_camera.z;

            ++stats.expected_surface_chunks;
            const auto it = m_streaming_state.chunks.find(Chunk::calculate_id(coords));
            if (it == m_streaming_state.chunks.end() || !it->second) {
                ++stats.missing_surface_chunks;
                continue;
            }

            ++stats.present_surface_chunks;
            const auto& chunk = it->second;
            if (is_center_surface_chunk) {
                stats.center_chunk_present = true;
            }

            switch (chunk->get_state()) {
                case ChunkState::Unloaded:
                    ++stats.unloaded_surface_chunks;
                    break;
                case ChunkState::Loading:
                    ++stats.loading_surface_chunks;
                    break;
                case ChunkState::Idle:
                    ++stats.idle_surface_chunks;
                    break;
                case ChunkState::Meshing:
                    ++stats.meshing_surface_chunks;
                    break;
                case ChunkState::Ready:
                    ++stats.ready_surface_chunks;
                    break;
                case ChunkState::Unloading:
                    break;
            }

            if (!chunk->mesh_vertices.empty() && !chunk->mesh_indices.empty()) {
                ++stats.renderable_surface_chunks;
                if (is_center_surface_chunk) {
                    stats.center_chunk_renderable = true;
                }
            }
            if (chunk->has_collision.load(std::memory_order_acquire)) {
                ++stats.collision_surface_chunks;
            }
            if (chunk->pending_lod.load(std::memory_order_acquire) >= 0) {
                ++stats.pending_lod_chunks;
            }

            const int lod = chunk->current_lod.load(std::memory_order_acquire);
            if (lod >= 0 && lod < static_cast<int>(stats.lod_counts.size())) {
                ++stats.lod_counts[static_cast<std::size_t>(lod)];
            } else {
                ++stats.lod_unknown_chunks;
            }
        }
    }

    stats.near_field_renderable =
        stats.expected_surface_chunks > 0 &&
        stats.missing_surface_chunks == 0 &&
        stats.renderable_surface_chunks == stats.expected_surface_chunks;
    return stats;
}

SHIELD_WorldSystem::FrustumSurfaceCoverageStats SHIELD_WorldSystem::get_frustum_surface_coverage_stats(
    const Vec3& camera_position,
    const std::array<Vec4, 6>& frustum_planes,
    float max_distance) const
{
    FrustumSurfaceCoverageStats stats;

    // Positive-vertex AABB/frustum intersection: the box is outside when its
    // most-positive corner against a plane normal is still behind the plane.
    const auto aabb_intersects_frustum = [&frustum_planes](const Vec3& min_corner, const Vec3& max_corner) {
        for (const Vec4& plane : frustum_planes) {
            const Vec3 positive_corner(
                plane.x >= 0.0f ? max_corner.x : min_corner.x,
                plane.y >= 0.0f ? max_corner.y : min_corner.y,
                plane.z >= 0.0f ? max_corner.z : min_corner.z);
            if (plane.x * positive_corner.x + plane.y * positive_corner.y +
                plane.z * positive_corner.z + plane.w < 0.0f) {
                return false;
            }
        }
        return true;
    };

    const IVec3 camera_chunk = world_to_chunk_coords(camera_position);
    const int radius = std::max(0, static_cast<int>(std::ceil(max_distance / static_cast<float>(CHUNK_SIZE_X))));
    const float max_distance_sq = max_distance * max_distance;

    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int chunk_x = camera_chunk.x + dx;
            const int chunk_z = camera_chunk.z + dz;
            const float base_x = static_cast<float>(chunk_x * CHUNK_SIZE_X);
            const float base_z = static_cast<float>(chunk_z * CHUNK_SIZE_Z);
            const float center_x = base_x + CHUNK_SIZE_X * 0.5f;
            const float center_z = base_z + CHUNK_SIZE_Z * 0.5f;

            const float horizontal_dx = center_x - camera_position.x;
            const float horizontal_dz = center_z - camera_position.z;
            if (horizontal_dx * horizontal_dx + horizontal_dz * horizontal_dz > max_distance_sq) {
                continue;
            }

            // Inline 5-point span sample (column center + footprint corners);
            // deliberately independent of the streaming span cache so the
            // gate measures the policy from the outside.
            float min_height = std::numeric_limits<float>::max();
            float max_height = std::numeric_limits<float>::lowest();
            const std::array<std::pair<float, float>, 5> sample_points{{
                {center_x, center_z},
                {base_x, base_z},
                {base_x + CHUNK_SIZE_X, base_z},
                {base_x, base_z + CHUNK_SIZE_Z},
                {base_x + CHUNK_SIZE_X, base_z + CHUNK_SIZE_Z},
            }};
            for (const auto& [px, pz] : sample_points) {
                const float h = GetTerrainHeightAt(px, pz);
                min_height = std::min(min_height, h);
                max_height = std::max(max_height, h);
            }
            const int span_min = world_to_chunk_coords(Vec3(center_x, min_height, center_z)).y;
            const int span_max = world_to_chunk_coords(Vec3(center_x, max_height, center_z)).y;

            bool column_considered = false;
            for (int chunk_y = span_min; chunk_y <= span_max; ++chunk_y) {
                const Vec3 min_corner(base_x, static_cast<float>(chunk_y * CHUNK_SIZE_Y), base_z);
                const Vec3 max_corner = min_corner + Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
                if (!aabb_intersects_frustum(min_corner, max_corner)) {
                    continue;
                }

                column_considered = true;
                ++stats.expected_chunks;
                const auto it = m_streaming_state.chunks.find(Chunk::calculate_id(IVec3(chunk_x, chunk_y, chunk_z)));
                if (it == m_streaming_state.chunks.end() || !it->second) {
                    ++stats.missing_chunks;
                    continue;
                }
                ++stats.present_chunks;
                if (!it->second->mesh_vertices.empty() && !it->second->mesh_indices.empty()) {
                    ++stats.renderable_chunks;
                }
            }
            if (column_considered) {
                ++stats.columns_considered;
            }
        }
    }

    stats.renderable_ratio = stats.expected_chunks > 0
        ? static_cast<double>(stats.renderable_chunks) / static_cast<double>(stats.expected_chunks)
        : 1.0;
    return stats;
}

void SHIELD_WorldSystem::GenerateChunkData(Luminumbra::Chunk& chunk, int target_step) const {
   const IVec3 coords = chunk.get_coords();
   const IVec3 base_pos = coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
   const int size_x = CHUNK_SIZE_X + 1;
   const int size_y = CHUNK_SIZE_Y + 1;
   const int size_z = CHUNK_SIZE_Z + 1;
   const size_t padded_volume = static_cast<size_t>(size_x) * size_y * size_z;

   // T-I3-1 SDF skip: chunks generated for a coarse meshing step (> 1) are
   // only ever meshed by GenerateCoarseHeightfieldTerrain (which samples
   // GetTerrainHeightAt analytically) and by the seam-fallback face patches
   // (which read terrain density derived from heightmap_data). Neither reads
   // the interior 17^3 SDF, so generate only the 17x17 heightmap: no SDF
   // allocation (19.65 KB/chunk) and no 3D cave-noise grid. Cave carving is
   // surface-capped at 18 m depth (kCaveSurfaceCapDepth), so every face SDF
   // value within the seam fallback's +-0.75 near-surface band equals the
   // pure terrain density (y - heightmap) exactly - the heightmap IS the
   // boundary-face band for seam purposes.
   if (target_step > 1) {
       chunk.sdf_data.clear();
       chunk.sdf_data.shrink_to_fit();

       const size_t heightmap_size = static_cast<size_t>(size_x) * size_z;
       chunk.heightmap_data.resize(heightmap_size);

       if (m_params.shaping_enabled) {
           // T-I3-10 / T-I4-DR-shaping-perf: shaped heights come from the one
           // shared height definition, but via the SIMD-batched grid helper
           // (GenUniformGrid2D / GenPositionArray2D) which produces bytes
           // EXACTLY equal to the per-column GenSingle2D scalar helper
           // GetTerrainHeightAt on this build - the batch and scalar paths
           // cannot diverge (batch-vs-scalar parity gtest pins ==). The batched
           // path is ~13x faster than the old per-column GenSingle2D loop.
           ComputeShapedHeightGrid(base_pos.x, base_pos.z, size_x, size_z,
                                   chunk.heightmap_data.data());
           chunk.clear_voxel_data_dirty();
           return;
       }

       std::vector<float> heightmap_noise(heightmap_size);
       std::vector<float> island_mask_noise(heightmap_size);
       m_terrain_generator->GenUniformGrid2D(heightmap_noise.data(), base_pos.x, base_pos.z, size_x, size_z, m_params.base_frequency, m_seed);
       m_island_mask_generator->GenUniformGrid2D(island_mask_noise.data(), base_pos.x, base_pos.z, size_x, size_z, m_params.island_mask_frequency, m_seed + 2);

       for (size_t index = 0; index < heightmap_size; ++index) {
           // Identical combine math to the full path below so heightmap bytes
           // are bit-equal regardless of which generation mode ran.
           float terrain_h = m_params.height_offset + heightmap_noise[index] * m_params.base_amplitude;
           if (m_params.island_mask_enabled) {
               const float island_mask = glm::smoothstep(0.1f, 0.25f, island_mask_noise[index]);
               terrain_h = glm::mix(m_params.height_offset, terrain_h, island_mask);
           }
           chunk.heightmap_data[index] = terrain_h;
       }

       chunk.clear_voxel_data_dirty();
       return;
   }

   // Try GPU generation first
   if (m_gpu_sdf_callback && m_gpu_sdf_callback(coords, m_params, m_seed, chunk.sdf_data)) {
       // GPU generation successful - still need to generate heightmap for physics
       const size_t heightmap_size = static_cast<size_t>(size_x) * size_z;
       chunk.heightmap_data.resize(heightmap_size);
       
       // Generate heightmap from SDF data
       for (int z = 0; z < size_z; ++z) {
           for (int x = 0; x < size_x; ++x) {
               int heightmap_idx = z * size_x + x;
               
               // Find surface by marching down through SDF
               float surface_height = base_pos.y + size_y; // Start from top
               for (int y = size_y - 1; y >= 0; --y) {
                   int sdf_idx = z * (size_x * size_y) + y * size_x + x;
                   if (chunk.sdf_data[sdf_idx] <= 0.0f) {
                       surface_height = base_pos.y + y;
                       break;
                   }
               }
               chunk.heightmap_data[heightmap_idx] = surface_height;
           }
       }
       
       // Generation produces the canonical voxel data; only post-generation
       // edits count as unsaved dirty state.
       chunk.clear_voxel_data_dirty();
       chunk.set_state(ChunkState::Idle);
       return;
   }

   // Fallback to CPU generation
   chunk.sdf_data.resize(padded_volume);

   const size_t heightmap_size = static_cast<size_t>(size_x) * size_z;
   chunk.heightmap_data.resize(heightmap_size);

   // --- Step 1: Generate all noise data for the chunk in large, SIMD-accelerated batches ---
   std::vector<float> heightmap_noise(heightmap_size);
   std::vector<float> island_mask_noise(heightmap_size);
   std::vector<float> cave_noise;
   if (m_params.caves_enabled) {
       cave_noise.resize(padded_volume);
   }
   
   // T-I3-10: with shaping enabled the per-column heights are computed by the
   // one shared scalar helper (GenSingle* only) so they are EXACTLY equal to
   // GetTerrainHeightAt/SampleWorldGenLayers at the same coordinates. The
   // legacy path keeps its SIMD GenUniformGrid2D batches (bit-identical
   // pre-shaping bytes; the 1e-4 snapshot gate covers grid-vs-single drift).
   std::vector<float> shaped_heights;
   if (m_params.shaping_enabled) {
       // T-I4-DR-shaping-perf: SIMD-batched shaped heights, byte-identical to
       // the per-column GenSingle2D scalar helper (parity gtest pins ==).
       shaped_heights.resize(heightmap_size);
       ComputeShapedHeightGrid(base_pos.x, base_pos.z, size_x, size_z,
                               shaped_heights.data());
   } else {
       // FastNoise GenUniformGrid2D populates its buffer in [x][z] layout where x varies fastest
       m_terrain_generator->GenUniformGrid2D(heightmap_noise.data(), base_pos.x, base_pos.z, size_x, size_z, m_params.base_frequency, m_seed);
       m_island_mask_generator->GenUniformGrid2D(island_mask_noise.data(), base_pos.x, base_pos.z, size_x, size_z, m_params.island_mask_frequency, m_seed + 2);
   }

   // FastNoise GenUniformGrid3D populates its buffer in [x][y][z] layout where x varies fastest
   if (m_params.caves_enabled) {
       m_cave_generator->GenUniformGrid3D(cave_noise.data(), base_pos.x, base_pos.y, base_pos.z, size_x, size_y, size_z, m_params.cave_frequency, m_seed + 1);
   }
   
   // --- Step 2: Combine the pre-generated noise to calculate the final SDF values ---
   for (int z = 0; z < size_z; ++z) {
       for (int y = 0; y < size_y; ++y) {
           for (int x = 0; x < size_x; ++x) {
               // For 2D noise buffers in [x][z] layout (x varies fastest):
               size_t index_2d_read = static_cast<size_t>(x) + static_cast<size_t>(z) * size_x;

               // Index for WRITING to our sdf_data and heightmap_data with bounds checking
               size_t sdf_write_idx = static_cast<size_t>(x) + static_cast<size_t>(y) * size_x + static_cast<size_t>(z) * size_x * size_y;
               size_t heightmap_write_idx = static_cast<size_t>(x) + static_cast<size_t>(z) * size_x;
               
               // Bounds checking
               if (sdf_write_idx >= chunk.sdf_data.size()) {
                   LUMINUMBRA_CORE_ERROR("SDF index out of bounds: {} >= {}", sdf_write_idx, chunk.sdf_data.size());
                   continue;
               }
               if (y == 0 && heightmap_write_idx >= chunk.heightmap_data.size()) {
                   LUMINUMBRA_CORE_ERROR("Heightmap index out of bounds: {} >= {}", heightmap_write_idx, chunk.heightmap_data.size());
                   continue;
               }
               
               // A. Calculate final terrain height for this (x,z) column
               float terrain_h;
               if (m_params.shaping_enabled) {
                   terrain_h = shaped_heights[index_2d_read];
               } else {
                   terrain_h = m_params.height_offset + heightmap_noise[index_2d_read] * m_params.base_amplitude;
                   if (m_params.island_mask_enabled) {
                       float island_mask = glm::smoothstep(0.1f, 0.25f, island_mask_noise[index_2d_read]);
                       terrain_h = glm::mix(m_params.height_offset, terrain_h, island_mask);
                   }
               }

               // B. Calculate base terrain density
               float current_world_y = base_pos.y + y;
               float terrain_density = current_world_y - terrain_h;

               // C. Carve caves using the 3D noise buffer
               if (m_params.caves_enabled) {
                   // For 3D noise buffer in [x][y][z] layout (x varies fastest):
                   size_t cave_read_idx = static_cast<size_t>(x) + static_cast<size_t>(y) * size_x + static_cast<size_t>(z) * size_x * size_y;
                   terrain_density = apply_cave_field(terrain_density, cave_noise[cave_read_idx], m_params);
               }
               
               if (y == 0) {
                   chunk.heightmap_data[heightmap_write_idx] = terrain_h;
               }

               // D. Write final density to chunk data using our consistent internal layout.
               // SDF debug logging removed to prevent segfault
               chunk.sdf_data[sdf_write_idx] = terrain_density;
           }
       }
   }

   // Generation produces the canonical voxel data; only post-generation edits
   // count as unsaved dirty state.
   chunk.clear_voxel_data_dirty();
}

JobHandle SHIELD_WorldSystem::dispatch_generation_jobs(const std::vector<IVec3>& chunks_to_generate) {
    // Coordinate-only callers (initial world load, regeneration, tests,
    // persistence) always want full voxel generation.
    std::vector<ChunkGenerationRequest> requests;
    requests.reserve(chunks_to_generate.size());
    for (const IVec3& coords : chunks_to_generate) {
        requests.push_back({coords, 1});
    }
    return dispatch_generation_jobs(requests);
}

JobHandle SHIELD_WorldSystem::dispatch_generation_jobs(const std::vector<ChunkGenerationRequest>& chunks_to_generate) {
    clear_completed_job_handle(m_streaming_state.generation_job_handle);
    if (has_active_job(m_streaming_state.generation_job_handle)) {
        return {};
    }

    std::vector<Luminumbra::Job> jobs;
    for (const auto& request : chunks_to_generate) {
        const IVec3 coords = request.coords;
        // Chunks restored from a world save (or already generated) carry
        // populated voxel data, possibly with player edits; regeneration would
        // clobber those edits. Generation is a pure function of seed/params,
        // so skipping any chunk that already has voxel data (full SDF or the
        // surface-band heightmap) is a no-op for untouched chunks and the
        // load/generation contract for saved ones. A surface-band chunk later
        // promoted to LOD0 gets its full SDF backfilled by the meshing path.
        const auto existing = m_streaming_state.chunks.find(Chunk::calculate_id(coords));
        if (existing != m_streaming_state.chunks.end() && existing->second &&
            (!existing->second->sdf_data.empty() || !existing->second->heightmap_data.empty())) {
            continue;
        }

        auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
        chunk->set_state(Luminumbra::ChunkState::Loading);
        m_streaming_state.chunks[chunk->get_id()] = chunk;

        // Chunk generation job created

        const int target_step = request.target_step;
        jobs.emplace_back([this, chunk, target_step]() {
            GenerateChunkData(*chunk, target_step);
            chunk->set_state(Luminumbra::ChunkState::Idle);
        });
    }
    
    JobHandle handle; // Declare handle outside the if block
    
    if (m_job_system && !jobs.empty()) {
        handle = m_job_system->dispatch_batch(jobs);
        m_streaming_state.generation_job_handle = handle;
    }
    return handle; // Return the handle (will be default-constructed/invalid if no jobs were dispatched)
}

void SHIELD_WorldSystem::dispatch_meshing_jobs(const std::vector<MeshingWorkItem>& chunks_to_mesh) {
    process_completed_meshing_jobs();
    if (meshing_jobs_active()) {
        return;
    }

    // Snapshot meshed-chunk LODs per horizontal column once so the
    // transition-face checks below are hash lookups instead of a full
    // chunk-map scan per face per work item (O(batch * chunks) before,
    // which dominated the dispatch frame once backlog-scaled batches
    // landed). Mirrors the candidate-side snapshot built in update().
    struct DispatchMeshedColumnEntry {
        const Luminumbra::Chunk* chunk = nullptr;
        int y = 0;
        int lod = 0;
    };
    std::unordered_map<u64, std::vector<DispatchMeshedColumnEntry>> meshed_columns;
    meshed_columns.reserve(m_streaming_state.chunks.size());
    for (const auto& [neighbor_id, neighbor] : m_streaming_state.chunks) {
        (void)neighbor_id;
        if (!neighbor || neighbor->mesh_vertices.empty() || neighbor->mesh_indices.empty()) {
            continue;
        }
        const int neighbor_lod = neighbor->current_lod.load(std::memory_order_acquire);
        if (neighbor_lod < 0) {
            continue;
        }
        const IVec3 neighbor_coords = neighbor->get_coords();
        meshed_columns[horizontal_chunk_key(neighbor_coords.x, neighbor_coords.z)].push_back({
            neighbor.get(), neighbor_coords.y, neighbor_lod
        });
    }

    // Near-field hole-fill candidates (no active mesh yet, capped prefix of
    // the sorted batch) ride the High job lane so visible gaps close ahead of
    // bulk meshing, generation, and LOD/water remeshes; everything else stays
    // on the Normal lane. The sort in update() already places the hole-fill
    // prefix first, so this mirrors the existing priority order.
    std::vector<Luminumbra::Job> high_priority_jobs;
    std::vector<Luminumbra::Job> normal_priority_jobs;
    m_streaming_state.meshing_job_chunks.clear();
    m_streaming_state.meshing_job_chunks.reserve(chunks_to_mesh.size());
    for (const MeshingWorkItem& work_item : chunks_to_mesh) {
        auto& chunk = work_item.chunk;
        const int lod_level = work_item.lod_level;
        const bool terrain_mesh_required = work_item.terrain_mesh_required;
        const int step = get_lod_step_for_level(lod_level);
        const bool has_active_mesh = chunk->get_state() == Luminumbra::ChunkState::Ready &&
            !chunk->mesh_vertices.empty() && !chunk->mesh_indices.empty();

        if (!has_active_mesh) {
            chunk->set_state(Luminumbra::ChunkState::Meshing);
        }
        chunk->pending_mesh_vertices.clear();
        chunk->pending_mesh_indices.clear();
        chunk->pending_water_mesh_vertices.clear();
        chunk->pending_water_mesh_indices.clear();
        chunk->pending_mesh_ready.store(false, std::memory_order_release);
        chunk->pending_mesh_failed.store(false, std::memory_order_release);
        chunk->pending_lod.store(lod_level, std::memory_order_release);

        auto transition_faces = Luminumbra::World::MarchingCubes::kNoTransitionFaces;
        if (terrain_mesh_required && step > 1) {
            const IVec3 coords = chunk->get_coords();
            auto add_face_if_neighbor_is_finer = [&](int dx, int dz, Luminumbra::World::MarchingCubes::TerrainTransitionFace face) {
                const auto column_it = meshed_columns.find(horizontal_chunk_key(coords.x + dx, coords.z + dz));
                if (column_it == meshed_columns.end()) {
                    return;
                }
                for (const DispatchMeshedColumnEntry& entry : column_it->second) {
                    if (entry.chunk == chunk.get()) {
                        continue;
                    }
                    if (entry.lod < lod_level || (entry.lod != lod_level && entry.y != coords.y)) {
                        transition_faces |= static_cast<Luminumbra::World::MarchingCubes::TerrainTransitionFaceMask>(face);
                        return;
                    }
                }
            };

            add_face_if_neighbor_is_finer(-1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMinX);
            add_face_if_neighbor_is_finer(1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMaxX);
            add_face_if_neighbor_is_finer(0, -1, Luminumbra::World::MarchingCubes::TransitionFaceMinZ);
            add_face_if_neighbor_is_finer(0, 1, Luminumbra::World::MarchingCubes::TransitionFaceMaxZ);
        }
        m_streaming_state.meshing_job_chunks.push_back({chunk, terrain_mesh_required, transition_faces});

        auto& lane_jobs = work_item.high_priority ? high_priority_jobs : normal_priority_jobs;
        lane_jobs.emplace_back([this, chunk, step, transition_faces, terrain_mesh_required]() {
            try {
                Luminumbra::Chunk scratch(chunk->get_coords());
                scratch.water_level_data = chunk->water_level_data;
                scratch.water_flow_data = chunk->water_flow_data;
                scratch.water_sim_terrain_height = chunk->water_sim_terrain_height;
                scratch.has_water_sim.store(chunk->has_water_sim.load(std::memory_order_acquire), std::memory_order_release);
                scratch.water_mesh_generated.store(false, std::memory_order_release);
                scratch.current_water_resolution.store(chunk->current_water_resolution.load(std::memory_order_acquire), std::memory_order_release);

                bool backfilled_voxel_data = false;
                if (terrain_mesh_required) {
                    if (step <= 1 && chunk->sdf_data.empty()) {
                        // LOD0 promotion of a chunk generated surface-band
                        // only (T-I3-1): build the full voxel field into the
                        // scratch chunk here and stage it for main-thread
                        // publication alongside the mesh, so the previous
                        // coarse mesh stays renderable while pending and the
                        // live chunk's sdf_data is never touched off-thread.
                        GenerateChunkData(scratch, 1);
                        backfilled_voxel_data = true;
                    } else {
                        scratch.sdf_data = chunk->sdf_data;
                        scratch.heightmap_data = chunk->heightmap_data;
                    }

                    // 1. Generate the terrain mesh from the SDF data into a scratch chunk.
                    Luminumbra::World::MarchingCubes::PolygoniseTerrain(*this, scratch, 0.0f, step);
                    Luminumbra::World::MarchingCubes::AddBoundaryTransitionSkirts(scratch, step, transition_faces);
                }
                
                // Terrain mesh generation debug logging removed to prevent segfault
                
                // 2. Generate the water surface mesh if the water system exists
                if (m_water_system) {
                    // Only generate high-detail water mesh for highest LOD terrain
                    if (step <= 1) {
                        Luminumbra::World::MarchingCubes::GenerateWaterMesh(*m_water_system, *this, scratch);
                    } else {
                        scratch.water_mesh_vertices.clear();
                        scratch.water_mesh_indices.clear();
                        scratch.water_mesh_generated.store(false, std::memory_order_release);
                    }
                }

                if (terrain_mesh_required) {
                    chunk->pending_mesh_vertices = std::move(scratch.mesh_vertices);
                    chunk->pending_mesh_indices = std::move(scratch.mesh_indices);
                    if (backfilled_voxel_data) {
                        chunk->pending_sdf_data = std::move(scratch.sdf_data);
                        chunk->pending_heightmap_data = std::move(scratch.heightmap_data);
                    }
                }
                chunk->pending_water_mesh_vertices = std::move(scratch.water_mesh_vertices);
                chunk->pending_water_mesh_indices = std::move(scratch.water_mesh_indices);
                chunk->water_mesh_generated.store(scratch.water_mesh_generated.load(std::memory_order_acquire), std::memory_order_release);
                chunk->pending_mesh_ready.store(true, std::memory_order_release);
                
                // Job completion debug logging removed to prevent segfault
            } catch (const std::exception& e) {
                LUMINUMBRA_CORE_ERROR("MESHING JOB CRASH: Chunk ({},{},{}) failed: {}", 
                    chunk->get_coords().x, chunk->get_coords().y, chunk->get_coords().z, e.what());
                chunk->pending_mesh_failed.store(true, std::memory_order_release);
            } catch (...) {
                LUMINUMBRA_CORE_ERROR("MESHING JOB CRASH: Chunk ({},{},{}) failed with unknown exception", 
                    chunk->get_coords().x, chunk->get_coords().y, chunk->get_coords().z);
                chunk->pending_mesh_failed.store(true, std::memory_order_release);
            }
        });
    }
    if (m_job_system && (!high_priority_jobs.empty() || !normal_priority_jobs.empty())) {
        if (!high_priority_jobs.empty()) {
            m_streaming_state.meshing_job_handle_high =
                m_job_system->dispatch_batch(high_priority_jobs, JobPriority::High);
        }
        if (!normal_priority_jobs.empty()) {
            m_streaming_state.meshing_job_handle = m_job_system->dispatch_batch(normal_priority_jobs);
        }
    } else {
        for (auto& job : high_priority_jobs) {
            job();
        }
        for (auto& job : normal_priority_jobs) {
            job();
        }
        m_streaming_state.meshing_job_handle.counter = std::make_shared<std::atomic<int>>(0);
        process_completed_meshing_jobs();
    }
}

void SHIELD_WorldSystem::process_completed_meshing_jobs() {
    if (!m_streaming_state.meshing_job_handle.counter &&
        !m_streaming_state.meshing_job_handle_high.counter)
    {
        return;
    }

    // Results are published only once BOTH lanes of the dispatch finished, so
    // mesh application keeps the pre-priority-lane all-or-nothing semantics.
    if (meshing_jobs_active()) {
        return;
    }

    for (const auto& job_chunk : m_streaming_state.meshing_job_chunks) {
        const auto& chunk = job_chunk.chunk;
        if (!chunk) {
            continue;
        }

        const int completed_lod = chunk->pending_lod.load(std::memory_order_acquire);
        const bool mesh_ready = chunk->pending_mesh_ready.load(std::memory_order_acquire);
        const bool mesh_failed = chunk->pending_mesh_failed.load(std::memory_order_acquire);

        if (mesh_ready && !mesh_failed) {
            if (job_chunk.terrain_mesh_required) {
                if (!chunk->pending_sdf_data.empty()) {
                    // LOD0-promotion backfill (T-I3-1): publish the full
                    // voxel field generated inside the meshing job. Pure
                    // generation output, not an edit - the dirty flag stays
                    // clear (matching GenerateChunkData's contract).
                    chunk->sdf_data = std::move(chunk->pending_sdf_data);
                    chunk->heightmap_data = std::move(chunk->pending_heightmap_data);
                    chunk->clear_voxel_data_dirty();
                }
                chunk->mesh_vertices = std::move(chunk->pending_mesh_vertices);
                chunk->mesh_indices = std::move(chunk->pending_mesh_indices);
                chunk->current_lod.store(completed_lod, std::memory_order_release);
                chunk->applied_transition_faces.store(job_chunk.transition_faces, std::memory_order_release);
                chunk->mesh_version++;
                chunk->has_collision.store(false, std::memory_order_release);
            }
            chunk->water_mesh_vertices = std::move(chunk->pending_water_mesh_vertices);
            chunk->water_mesh_indices = std::move(chunk->pending_water_mesh_indices);
            chunk->water_mesh_version++;
            chunk->water_mesh_dirty_ticks = 0;
            chunk->set_state(Luminumbra::ChunkState::Ready);
        } else if (chunk->mesh_vertices.empty() || chunk->mesh_indices.empty()) {
            chunk->set_state(Luminumbra::ChunkState::Idle);
        } else {
            chunk->set_state(Luminumbra::ChunkState::Ready);
        }

        chunk->pending_mesh_vertices.clear();
        chunk->pending_mesh_indices.clear();
        chunk->pending_water_mesh_vertices.clear();
        chunk->pending_water_mesh_indices.clear();
        chunk->pending_sdf_data.clear();
        chunk->pending_heightmap_data.clear();
        chunk->pending_mesh_ready.store(false, std::memory_order_release);
        chunk->pending_mesh_failed.store(false, std::memory_order_release);
        chunk->pending_lod.store(-1, std::memory_order_release);
    }

    m_streaming_state.meshing_job_chunks.clear();
    m_streaming_state.meshing_job_handle = {};
    m_streaming_state.meshing_job_handle_high = {};
}

void SHIELD_WorldSystem::set_params(const TerrainGenParams& params) {
    wait_for_generation_jobs();
    wait_for_meshing_jobs();

    m_params = params;
    reinitialize_noise();
}

void SHIELD_WorldSystem::set_seed(int seed) {
    wait_for_generation_jobs();
    wait_for_meshing_jobs();

    m_seed = seed;
    reinitialize_noise();
}

void SHIELD_WorldSystem::clear_world(PhysicsSystem* physics_system) {
    wait_for_generation_jobs();
    wait_for_meshing_jobs();

    if (physics_system) {
        for (const auto& [id, chunk] : m_streaming_state.chunks) {
            physics_system->remove_chunk_collision(id);
        }
    }
    m_streaming_state.chunks.clear();
    LUMINUMBRA_CORE_INFO("World cleared.");
}

void SHIELD_WorldSystem::regenerate_all_chunks(PhysicsSystem* physics_system) {
    wait_for_generation_jobs();
    wait_for_meshing_jobs();

    LUMINUMBRA_CORE_INFO("Regenerating all active chunks...");
    std::vector<IVec3> coords_to_regenerate;
    coords_to_regenerate.reserve(m_streaming_state.chunks.size());
    for(const auto& [id, chunk] : m_streaming_state.chunks) {
        coords_to_regenerate.push_back(chunk->get_coords());
    }
    clear_world(physics_system);
    dispatch_generation_jobs(coords_to_regenerate);
}

void SHIELD_WorldSystem::SetWaterSystem(WaterSystem* water_system) {
    wait_for_meshing_jobs();

    m_water_system = water_system;
}

IVec3 SHIELD_WorldSystem::world_to_chunk_coords(const Vec3& position) {
    return IVec3(
        static_cast<int>(std::floor(position.x / CHUNK_SIZE_X)),
        static_cast<int>(std::floor(position.y / CHUNK_SIZE_Y)),
        static_cast<int>(std::floor(position.z / CHUNK_SIZE_Z))
    );
}

void SHIELD_WorldSystem::SetGPUSDFCallback(std::function<bool(const IVec3&, const TerrainGenParams&, int, std::vector<float>&)> callback) {
    wait_for_generation_jobs();

    m_gpu_sdf_callback = callback;
}

void SHIELD_WorldSystem::wait_for_streaming_jobs() {
    wait_for_generation_jobs();
    wait_for_meshing_jobs();
}

std::vector<std::shared_ptr<Luminumbra::Chunk>> SHIELD_WorldSystem::snapshot_streamed_chunks() const {
    std::vector<std::shared_ptr<Luminumbra::Chunk>> chunks;
    chunks.reserve(m_streaming_state.chunks.size());
    for (const auto& [id, chunk_ptr] : m_streaming_state.chunks) {
        (void)id;
        if (chunk_ptr) {
            chunks.push_back(chunk_ptr);
        }
    }
    return chunks;
}

std::shared_ptr<Luminumbra::Chunk> SHIELD_WorldSystem::find_streamed_chunk(const IVec3& coords) const {
    const auto it = m_streaming_state.chunks.find(Chunk::calculate_id(coords));
    return it != m_streaming_state.chunks.end() ? it->second : nullptr;
}

bool SHIELD_WorldSystem::adopt_streamed_chunk(const std::shared_ptr<Luminumbra::Chunk>& chunk) {
    if (!chunk) {
        return false;
    }
    return m_streaming_state.chunks.emplace(chunk->get_id(), chunk).second;
}

} // namespace Luminumbra::Systems
