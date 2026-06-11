#include "SHIELD_WorldSystem.h"
#include "entt/entt.hpp"
#include "../world/MarchingCubes.h"
#include <array>
#include <atomic>
#include <cmath>
#include <algorithm> // Required for std::max and std::min
#include <limits>
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

MaterialType classify_material(const WorldGenLayerSample& sample) {
    if (!sample.solid) {
        return MaterialType::Air;
    }

    if (sample.world_pos.y < 34.0f && sample.final_height < 36.0f) {
        return MaterialType::Sand;
    }

    const float depth = sample.final_height - sample.world_pos.y;
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
}

SHIELD_WorldSystem::ColumnSurfaceSpan SHIELD_WorldSystem::compute_column_surface_span(int chunk_x, int chunk_z) const {
    const float base_x = static_cast<float>(chunk_x * CHUNK_SIZE_X);
    const float base_z = static_cast<float>(chunk_z * CHUNK_SIZE_Z);
    const float max_x = base_x + static_cast<float>(CHUNK_SIZE_X);
    const float max_z = base_z + static_cast<float>(CHUNK_SIZE_Z);
    const float center_x = base_x + CHUNK_SIZE_X * 0.5f;
    const float center_z = base_z + CHUNK_SIZE_Z * 0.5f;

    const float center_height = GetTerrainHeightAt(center_x, center_z);
    float min_height = center_height;
    float max_height = center_height;
    const std::array<std::pair<float, float>, 4> corners{{
        {base_x, base_z}, {max_x, base_z}, {base_x, max_z}, {max_x, max_z}
    }};
    for (const auto& [corner_x, corner_z] : corners) {
        const float corner_height = GetTerrainHeightAt(corner_x, corner_z);
        min_height = std::min(min_height, corner_height);
        max_height = std::max(max_height, corner_height);
    }

    ColumnSurfaceSpan span;
    span.center_y = world_to_chunk_coords(Vec3(center_x, center_height, center_z)).y;
    span.min_y = world_to_chunk_coords(Vec3(center_x, min_height, center_z)).y;
    span.max_y = world_to_chunk_coords(Vec3(center_x, max_height, center_z)).y;
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
    const Vec3& camera_position)
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
    return get_lod_level_for_distance(dist);
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
            required_lod = get_required_lod_for_chunk(chunk_ptr->get_coords(), chunk_center, camera_position);

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
                required_lod = get_required_lod_for_chunk(chunk_ptr->get_coords(), chunk_center, camera_position);
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
    const float noise_value = m_terrain_generator->GenSingle2D(
        world_x * m_params.base_frequency,
        world_z * m_params.base_frequency,
        m_seed
    );
    float terrain_height = m_params.height_offset + noise_value * m_params.base_amplitude;
    if (m_params.island_mask_enabled) {
        const float island_value = m_island_mask_generator->GenSingle2D(
            world_x * m_params.island_mask_frequency,
            world_z * m_params.island_mask_frequency,
            m_seed + 2
        );
        const float island_mask = glm::smoothstep(0.1f, 0.25f, island_value);
        terrain_height = glm::mix(m_params.height_offset, terrain_height, island_mask);
    }
    return terrain_height;
}

WorldGenLayerSample SHIELD_WorldSystem::SampleWorldGenLayers(const Vec3& world_pos) const {
    WorldGenLayerSample sample;
    sample.world_pos = world_pos;

    sample.base_noise = m_terrain_generator->GenSingle2D(
        world_pos.x * m_params.base_frequency,
        world_pos.z * m_params.base_frequency,
        m_seed
    );
    sample.base_height = m_params.height_offset + sample.base_noise * m_params.base_amplitude;
    sample.final_height = sample.base_height;

    if (m_params.island_mask_enabled) {
        sample.island_applied = true;
        sample.island_noise = m_island_mask_generator->GenSingle2D(
            world_pos.x * m_params.island_mask_frequency,
            world_pos.z * m_params.island_mask_frequency,
            m_seed + 2
        );
        sample.island_mask = glm::smoothstep(0.1f, 0.25f, sample.island_noise);
        sample.final_height = glm::mix(m_params.height_offset, sample.base_height, sample.island_mask);
    }

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
    sample.material = classify_material(sample);
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
   
   // FastNoise GenUniformGrid2D populates its buffer in [x][z] layout where x varies fastest
   m_terrain_generator->GenUniformGrid2D(heightmap_noise.data(), base_pos.x, base_pos.z, size_x, size_z, m_params.base_frequency, m_seed);
   m_island_mask_generator->GenUniformGrid2D(island_mask_noise.data(), base_pos.x, base_pos.z, size_x, size_z, m_params.island_mask_frequency, m_seed + 2);
   
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
               float terrain_h = m_params.height_offset + heightmap_noise[index_2d_read] * m_params.base_amplitude;
               if (m_params.island_mask_enabled) {
                   float island_mask = glm::smoothstep(0.1f, 0.25f, island_mask_noise[index_2d_read]);
                   terrain_h = glm::mix(m_params.height_offset, terrain_h, island_mask);
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
