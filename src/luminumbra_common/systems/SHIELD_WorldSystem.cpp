#include "SHIELD_WorldSystem.h"
#include "../core/JobWatchdog.h" // opt-in named-phase wedge reporter for unbounded waits
#include "../core/Log.h"
#include "../world/FarLodStore.h"
#include "../world/HydraulicErosion.h" //
#include "../world/MarchingCubes.h"
#include "WaterSystem.h"
#include "entt/entt.hpp"
#include "systems/PhysicsSystem.h"
#include <algorithm> // Required for std::max and std::min
#include <array>
#include <atomic>
#include <chrono> // runtime telemetry ( implementation note): per-sub-phase streaming timing
#include <cmath>
#include <cstdlib> // std::llabs (hydro cache eviction distance)
#include <cstring> // std::memcpy for the deterministic water-state hash
#include <filesystem>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <thread> // opt-in job-wedge watchdog monitor thread
#include <unordered_map>
#include <unordered_set>

constexpr int BASE_WORK_BUDGET_EQUIVALENT = 320;
const int MAX_CHUNKS_TO_PROCESS_PER_FRAME =
    std::max(1,
             static_cast<int>(BASE_WORK_BUDGET_EQUIVALENT /
                              (static_cast<float>(Luminumbra::CHUNK_VOLUME) / 4096.0f)));
const int MAX_COLLISION_MESHES_PER_FRAME = 16;
// Per dispatch, at most this many sorted-prefix hole-fill candidates ride the
// High job lane. The cap keeps "High" meaning near-field holes the player can
// see: during bulk drains (initial load, fast travel) nearly every candidate
// lacks an active mesh, and routing the whole backlog High starves the Normal
// lane that generation batches ride on, delaying neighbor arrival and
// inflating late-neighbor transition remesh churn.
constexpr std::size_t MAX_HIGH_PRIORITY_MESHING_JOBS_PER_DISPATCH = 32;
constexpr size_t STREAMING_MAX_ACTIVE_CHUNKS_BUDGET = 8192;
constexpr int STREAMING_ACTIVATION_INTERVAL_FRAMES = 4;
constexpr int STREAMING_NEAR_VERTICAL_STACK_RADIUS = 4;
constexpr int STREAMING_MID_VERTICAL_STACK_RADIUS = 12;
constexpr int STREAMING_MULTI_ANCHOR_MIN_RADIUS = 6;
constexpr std::size_t STREAMING_APPROX_CHUNKS_PER_SURFACE_COLUMN = 3;

// Full LOD0 SDF lattice size. A chunk whose sdf_data has EXACTLY this size is
// full-resolution sim truth; anything else (empty = surface-band/coarse
// generated, wrong-sized = malformed, ) needs the promotion lane to
// (re)generate the full field before a unit-step polygonise may read it.
constexpr std::size_t kFullSdfLattice = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                        (Luminumbra::CHUNK_SIZE_Y + 1) *
                                        (Luminumbra::CHUNK_SIZE_Z + 1);

//  (activation queue ): the FIXED activation pipeline latency K, in sim
// ticks. A batch dispatched at tick D becomes sim-visible at D+K — a pure
// function of the dispatch schedule, identical across worker counts and
// machines (never measured/adaptive: that would re-couple availability to
// timing and desync lockstep peers). K=8 (~267 ms at 30 Hz) covers the
// measured whole-drain p99 of ~240 ms with margin; a due-but-unfinished batch
// BLOCKS activation (wall-clock cost), it never slips (schedule change).
constexpr std::int64_t kActivationPipelineLatencyTicks = 8;

//  5b: deterministic dispatch backpressure — FIFO depth budgets
// (main-thread pure state, never job timing). Chosen so steady-state dispatch
// cadence matches the barrier era exactly: generation dispatches at most once
// per activation pass (every 4 ticks) and a batch lives K=8 ticks → depth
// alternates 1-2, so 3 never binds; meshing can dispatch every tick → depth
// reaches K, so K+1 never binds in steady state. The caps exist to bound the
// pipe if cadence assumptions ever break, not to throttle it.
constexpr std::size_t kMaxGenerationBatchesInFlight = 3;
constexpr std::size_t kMaxMeshingBatchesInFlight =
    static_cast<std::size_t>(kActivationPipelineLatencyTicks) + 1;

// SIMD over-read guard for FastNoise2's
// GenPositionArray2D. That entry point's tail does an UNCONDITIONAL full-width
// SIMD load of the input position arrays (FS_Load_f32(&xPosArray[index]) in
// vendor/fastnoise/.../Generator.inl:260) and a masked store of only the valid
// lanes. When the element count is smaller than the SIMD width (AVX512 = 16
// floats) -- e.g. the 5-point column-span footprint -- that load reads up to 15
// floats PAST a count-sized std::vector; if the allocation abuts an unmapped
// page the load faults with 0xC0000005 (the intermittent headless streaming
// crash). Padding every input/output array handed to GenPositionArray2D up to a
// multiple of this width keeps the tail load/store inside mapped memory. 16
// covers AVX512 (and every narrower level); the padding lanes are never read
// back into results, so heights/materials for indices [0,count) are byte-
// identical and the world_hash is unchanged.
constexpr std::size_t kNoiseSimdWidth = 16;
constexpr std::size_t PadToNoiseSimd(std::size_t count) {
    return count + (kNoiseSimdWidth - 1);
}

namespace Luminumbra::Systems {

namespace {

template<typename T>
FastNoise::SmartNode<T> NewWorldNoise() {
    // Keep world generation on one x86 SIMD implementation. Auto-selection
    // otherwise changes persisted terrain when the same build moves between
    // AVX2 and AVX-512 hosts.
    return FastNoise::New<T>(FastSIMD::Level_AVX2);
}

constexpr float kCaveSurfaceCapDepth = 18.0f;
constexpr float kCaveSurfaceFullDepth = 24.0f;
// surface-break feature width above the cap. The blend ramps from a
// per-column effective_cap to effective_cap + this, so when effective_cap drops
// to 0 inside a feature footprint the cave noise reaches the surface.
constexpr float kCaveSurfaceCapBand = kCaveSurfaceFullDepth - kCaveSurfaceCapDepth;

// distinct placement salt for surface-break dolines/cave-mouths, kept
// separate from every structure/biome stream (StructurePlacement salts).
constexpr u32 kSurfaceBreakSalt = 0xA3CA7E5u;

float smoothstep01(float value) {
    const float t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

//  generalized cap blend: the depth at which the cave field starts to win is
// the PER-COLUMN effective_cap (18 everywhere except inside a feature footprint).
// Byte-identical to the original cave_surface_blend(td) when effective_cap == 18.
float cave_surface_blend(float terrain_density, float effective_cap) {
    const float depth_below_surface = std::max(0.0f, -terrain_density);
    return smoothstep01((depth_below_surface - effective_cap) / kCaveSurfaceCapBand);
}

float surface_capped_cave_density(float terrain_density,
                                  float raw_cave_noise,
                                  const TerrainGenParams& params,
                                  float effective_cap) {
    const float cave_val = std::clamp((raw_cave_noise + 1.0f) * 0.5f, 0.0f, 1.0f);
    const float cave_density = (cave_val - params.cave_threshold) * params.cave_carve_value;
    const float cap_blend = cave_surface_blend(terrain_density, effective_cap);
    return terrain_density + (cave_density - terrain_density) * cap_blend;
}

// CSG smooth subtraction via EXPONENTIAL smin (associative + commutative -> order-
// free across SIMD lanes and chunk seams). k <= 0 => hard max (crisp). Subtraction
// of carve C from field F is max(F, -C); the soft form is -smin(-F, C, k) folded
// into max via the exp identity. Implemented directly as the associative
// exp combine so disabling (k<=0) is the exact hard max.
float exp_smax(float a, float b, float k) {
    if (k <= 0.0f) {
        return std::max(a, b);
    }
    // -k * log2(exp2(-a/k) + exp2(-b/k)) is the exp-smin; smax(a,b)=-smin(-a,-b).
    const float res = std::exp2(a / k) + std::exp2(b / k);
    return k * std::log2(res);
}

//  apply: combine caves with the per-column cap, then CSG-subtract the
// analytic sinkhole carve. max(F, -carve) opens the funnel (carve>0 -> -carve<0 ->
// where terrain is solid/negative this can flip it positive == air). Order-free.
// When effective_cap==18 and feature_carve==0 and carve_smoothness<=0 this is
// byte-identical to the original apply_cave_field.
float apply_cave_field(float terrain_density,
                       float raw_cave_noise,
                       const TerrainGenParams& params,
                       float effective_cap,
                       float feature_carve) {
    const float caves = std::max(
        terrain_density,
        surface_capped_cave_density(terrain_density, raw_cave_noise, params, effective_cap));
    if (feature_carve <= 0.0f) {
        return caves;
    }
    return exp_smax(caves, -feature_carve, params.carve_smoothness);
}

// ----  deterministic placement primitives (mirror StructurePlacement) ----
// All-unsigned; no int*prime UB. Same magic constants as the GLSL port so CPU and
// GPU produce bit-identical placement.
constexpr u64 kSbFnvOffsetBasis = 14695981039346656037ull;
constexpr u64 kSbFnvPrime = 1099511628211ull;

void SbFnvMix32(u64& hash, u32 value) {
    for (int i = 0; i < 4; ++i) {
        hash ^= static_cast<u64>((value >> (i * 8)) & 0xFFu);
        hash *= kSbFnvPrime;
    }
}

// Per-cell stream seed: world seed + salt + signed cell coords (two's-complement
// reinterpreted as u32, matching the GLSL int->uint bit reinterpretation).
u64 SbCellSeed(int world_seed, u32 salt, int cell_x, int cell_z) {
    u64 h = kSbFnvOffsetBasis;
    SbFnvMix32(h, static_cast<u32>(world_seed));
    SbFnvMix32(h, salt);
    SbFnvMix32(h, static_cast<u32>(cell_x));
    SbFnvMix32(h, static_cast<u32>(cell_z));
    return h;
}

struct SbSplitMix64 {
    u64 state;
    explicit SbSplitMix64(u64 seed)
        : state(seed) {}
    u64 next() {
        state += 0x9E3779B97F4A7C15ull;
        u64 z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float next_unit() {
        return static_cast<float>(next() >> 11) * (1.0f / 9007199254740992.0f);
    }
};

// IQ signed-distance primitives (negative inside). Used to carve sinkholes.
// sdVerticalCapsule: cenote shaft, a vertical capsule of radius r and height h
// rising from p0 = base. Here p is the point relative to the capsule base.
float sdVerticalCapsule(float px, float py, float pz, float h, float r) {
    const float cy = py - std::clamp(py, 0.0f, h);
    return std::sqrt(px * px + cy * cy + pz * pz) - r;
}

// sdCappedCone (IQ): a funnel. h = half-height, r1 = bottom radius, r2 = top
// radius. p is relative to the cone center. We use it inverted (wide at top) for a
// doline funnel by passing r1 < r2.
float sdCappedCone(float px, float py, float pz, float h, float r1, float r2) {
    const float qx = std::sqrt(px * px + pz * pz);
    const float k1x = r2, k1y = h;
    const float k2x = r2 - r1, k2y = 2.0f * h;
    const float cax = qx - std::min(qx, (py < 0.0f) ? r1 : r2);
    const float cay = std::abs(py) - h;
    const float k2dot = k2x * k2x + k2y * k2y;
    const float t =
        std::clamp(((k1x - qx) * k2x + (k1y - py) * k2y) / std::max(1e-6f, k2dot), 0.0f, 1.0f);
    const float cbx = qx - k1x + k2x * t;
    const float cby = py - k1y + k2y * t;
    const float s = (cbx < 0.0f && cay < 0.0f) ? -1.0f : 1.0f;
    return s * std::sqrt(std::min(cax * cax + cay * cay, cbx * cbx + cby * cby));
}

// Decoded surface-break feature (one accepted doline cell).
struct SurfaceBreakFeature {
    bool valid = false;
    float center_x = 0.0f;
    float center_z = 0.0f;
    float radius = 0.0f; // surface footprint radius (m), < max_feature_radius
    float depth = 0.0f;  // funnel depth (m)
    bool shaft = false;  // true => vertical capsule (cenote), false => cone funnel
};

// Decode the feature (if any) authored in doline cell (cx, cz). Pure fn of seed +
// cell coords. The same decode runs on CPU and GPU.
SurfaceBreakFeature
DecodeSurfaceBreakCell(int seed, int cx, int cz, const TerrainGenParams& params) {
    SurfaceBreakFeature f;
    SbSplitMix64 rng(SbCellSeed(seed, kSurfaceBreakSalt, cx, cz));
    const float accept = rng.next_unit();
    if (accept >= params.surface_break_density) {
        return f; // rejected -> no feature in this cell
    }
    const float cs = params.feature_cell_size;
    // Jittered center inside the cell (keep a margin so the footprint stays in the
    // 3x3 scan: center jitter is full-cell but radius < cell guarantees support
    // reaches at most into the immediate neighbor cells).
    const float jx = rng.next_unit();
    const float jz = rng.next_unit();
    f.center_x = (static_cast<float>(cx) + jx) * cs;
    f.center_z = (static_cast<float>(cz) + jz) * cs;
    // Power-law diameter (truncated Pareto, beta ~ 2.5): D = Dmin * (1-u)^(-1/(b-1)).
    const float u = rng.next_unit();
    constexpr float kBeta = 2.5f;
    constexpr float kDmin = 6.0f; // metres
    const float kDmax = std::min(120.0f, 2.0f * params.max_feature_radius);
    float diameter = kDmin * std::pow(std::max(1e-4f, 1.0f - u), -1.0f / (kBeta - 1.0f));
    diameter = std::clamp(diameter, kDmin, kDmax);
    f.radius = std::min(0.5f * diameter, params.max_feature_radius * 0.999f);
    // Depth ~ 0.2..0.5 * D.
    const float depth_frac = 0.2f + 0.3f * rng.next_unit();
    f.depth = depth_frac * diameter;
    f.shaft = rng.next_unit() < 0.25f; // a quarter are deep cenote shafts
    f.valid = true;
    return f;
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

// (clear_completed_job_handle removed by  : batch handles
// now live in the lane FIFOs and retire when their batch publishes.)

bool has_active_job(const JobHandle& handle) {
    return handle.counter && handle.counter->load(std::memory_order_acquire) > 0;
}

// LOD selection for a surface horizon ring. `lod0_radius` is the ring distance
// (inclusive) that renders at full-SDF LOD0; rings beyond it fall to LOD1 then
// LOD2 by the same 2/3-of-surface_radius threshold as before. Historically the
// LOD0 boundary was tied to the COLLISION radius (rings that get colliders are
// the rings rendered at full detail). The preview decouples them: it wants a
// larger full-detail RENDER slice (caves/overhangs) than its collision radius,
// so it passes an explicit lod0_radius without paying for gameplay collision.
int horizon_lod_for_ring(int ring_distance, int surface_radius, int lod0_radius) {
    if (ring_distance <= lod0_radius) {
        return 0;
    }

    const int mid_lod_ring = std::max(lod0_radius, (surface_radius * 2) / 3);
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

int streaming_radius_for_pressure(std::size_t active_chunks,
                                  std::size_t loading_chunks,
                                  std::size_t idle_chunks,
                                  bool generation_active,
                                  bool meshing_active,
                                  std::size_t anchor_count) {
    const std::size_t pending_chunks = loading_chunks + idle_chunks;
    int radius = RENDER_DISTANCE;
    if (active_chunks < 800u && !generation_active) {
        radius = std::min(radius, 24);
    } else if (generation_active || meshing_active ||
               pending_chunks > static_cast<std::size_t>(MAX_CHUNKS_TO_PROCESS_PER_FRAME * 8)) {
        radius = std::min(radius, 20);
    } else if (active_chunks > STREAMING_MAX_ACTIVE_CHUNKS_BUDGET * 3u / 4u) {
        radius = std::min(radius, 24);
    }

    if (anchor_count > 1u) {
        // Server-scale multi-anchor sessions cannot keep a full client render
        // disc around every avatar. Bound each anchor's wanted radius by the
        // global chunk budget so 20+ spread-out players get fair local AOIs
        // instead of an unbounded union backlog.
        constexpr double kPi = 3.14159265358979323846;
        const double columns_per_anchor =
            static_cast<double>(STREAMING_MAX_ACTIVE_CHUNKS_BUDGET) /
            static_cast<double>(anchor_count * STREAMING_APPROX_CHUNKS_PER_SURFACE_COLUMN);
        const int budget_radius =
            static_cast<int>(std::floor(std::sqrt(std::max(1.0, columns_per_anchor / kPi))));
        radius = std::min(
            radius, std::clamp(budget_radius, STREAMING_MULTI_ANCHOR_MIN_RADIUS, RENDER_DISTANCE));
    }

    return radius;
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

} // namespace

SHIELD_WorldSystem::SHIELD_WorldSystem(JobSystem* job_system,
                                       WaterSystem* water_system,
                                       const TerrainGenParams& params,
                                       int seed)
    : m_job_system(job_system)
    , m_params(params)
    , m_seed(seed)
    , m_water_system(water_system) {
    reinitialize_noise();
}

SHIELD_WorldSystem::~SHIELD_WorldSystem() {
    wait_for_generation_jobs();
    // Drain promotion workers with RAW waits — teardown must not publish or
    // dispatch stage-B jobs, only guarantee no worker still touches chunks.
    if (m_job_system && m_streaming_state.promotion_job_handle_high.counter) {
        m_job_system->wait(m_streaming_state.promotion_job_handle_high);
    }
    if (m_job_system && m_streaming_state.promotion_job_handle.counter) {
        m_job_system->wait(m_streaming_state.promotion_job_handle);
    }
    wait_for_meshing_jobs();
}

void SHIELD_WorldSystem::wait_for_generation_jobs() {
    if (m_job_system) {
        for (auto& batch : m_streaming_state.generation_batches) {
            if (batch.handle.counter) {
                m_job_system->wait(batch.handle);
            }
        }
    }
    // main-thread generation publication (Loading→Idle
    // flips + FIFO pops). Force: an explicit drain is a legal full-publish.
    publish_completed_generation_jobs(/*force=*/true);
}

bool SHIELD_WorldSystem::publish_front_generation_batch(bool force) {
    // Publish EXACTLY the front batch if its jobs have all completed AND (in
    // the non-force regime) its due tick has arrived. Flips are ON THE MAIN
    // THREAD (the gen job only stages data + raises pending_generation_ready
    // the chunk state machine is main-thread-owned). A chunk whose job
    // never completed (crash) keeps Loading and never re-candidates — the
    // same terminal behavior the old in-job flip had on a crash; its batch
    // pops with the rest once the batch counter drains (a crashed job still
    // decrements it).
    if (m_streaming_state.generation_batches.empty()) {
        return false;
    }
    auto& front = m_streaming_state.generation_batches.front();
    if (!force && front.due_tick >= 0) {
        // activation queue: a tick-stamped (server) batch publishes EXCLUSIVELY via
        // activate_due / the explicit force drains — never via a per-frame
        // hook. A hook publishing a due batch at update-start would land the
        // state flips BEFORE the tick's candidate pass while activate_due
        // lands them after, making the scheduler's view depend on whether
        // workers happened to be drained at that instant (wall-clock — the
        // release-moving run!=replay this rule fixed).
        return false;
    }
    if (has_active_job(front.handle)) {
        return false; // FIFO head still running — later batches wait their turn
    }
    for (auto& chunk : front.chunks) {
        if (!chunk) {
            continue;
        }
        if (chunk->pending_generation_ready.load(std::memory_order_acquire)) {
            chunk->pending_generation_ready.store(false, std::memory_order_release);
            if (chunk->get_state() == Luminumbra::ChunkState::Loading) {
                chunk->set_state(Luminumbra::ChunkState::Idle);
            }
        }
    }
    m_streaming_state.generation_batches.pop_front();
    return true;
}

void SHIELD_WorldSystem::publish_completed_generation_jobs(bool force) {
    // Drain every publishable front batch, in FIFO order (publication order
    // == dispatch order — deterministic).
    while (publish_front_generation_batch(force)) {}
}

void SHIELD_WorldSystem::wait_for_meshing_jobs() {
    if (m_job_system) {
        for (auto& batch : m_streaming_state.meshing_batches) {
            if (batch.handle_high.counter) {
                m_job_system->wait(batch.handle_high);
            }
            if (batch.handle.counter) {
                m_job_system->wait(batch.handle);
            }
        }
    }

    process_completed_meshing_jobs(/*force=*/true);
}

bool SHIELD_WorldSystem::meshing_jobs_active() const {
    for (const auto& batch : m_streaming_state.meshing_batches) {
        if (has_active_job(batch.handle) || has_active_job(batch.handle_high)) {
            return true;
        }
    }
    return false;
}

bool SHIELD_WorldSystem::generation_jobs_active() const {
    for (const auto& batch : m_streaming_state.generation_batches) {
        if (has_active_job(batch.handle)) {
            return true;
        }
    }
    return false;
}

bool SHIELD_WorldSystem::meshing_batch_outstanding() const {
    return !m_streaming_state.meshing_batches.empty();
}

bool SHIELD_WorldSystem::generation_batch_outstanding() const {
    return !m_streaming_state.generation_batches.empty();
}

bool SHIELD_WorldSystem::sim_available_lod0(const ::Luminumbra::Chunk& chunk) {
    // Barrier-era definition: the LOD0 render mesh has published. The
    // activation queue redefines this to tick-keyed availability at the
    // barrier-swap increment (activation queue ) — one function, all consumers.
    return chunk.current_lod.load(std::memory_order_acquire) == 0 && !chunk.mesh_vertices.empty() &&
           !chunk.mesh_indices.empty();
}

bool SHIELD_WorldSystem::promotion_jobs_active() const {
    return has_active_job(m_streaming_state.promotion_job_handle) ||
           has_active_job(m_streaming_state.promotion_job_handle_high);
}

bool SHIELD_WorldSystem::promotion_pipeline_pending() const {
    return promotion_jobs_active() || !m_streaming_state.promotion_job_chunks.empty() ||
           !m_streaming_state.pending_promotion_mesh.empty();
}

// ---  shadow instrumentation (activation queue step 2, ) ---
// Latency shadow for the activation-queue K design: how many SIM TICKS elapse
// between a streaming dispatch and the chunk becoming sim-visible, measured
// under the real barrier. Main-thread only, never hashed, inert at tick -1.

void SHIELD_WorldSystem::shadow_note_generation_dispatch(ChunkID id) {
    if (m_shadow_current_tick < 0) {
        return;
    }
    if (m_shadow_generation_dispatch_tick.emplace(id, m_shadow_current_tick).second) {
        ++m_shadow_gen_dispatches;
    }
}

void SHIELD_WorldSystem::shadow_note_promotion_dispatch(ChunkID id) {
    if (m_shadow_current_tick < 0) {
        return;
    }
    if (m_shadow_promotion_dispatch_tick.emplace(id, m_shadow_current_tick).second) {
        ++m_shadow_promo_dispatches;
    }
}

void SHIELD_WorldSystem::shadow_note_ready(ChunkID id) {
    if (m_shadow_current_tick < 0) {
        return;
    }
    const auto it = m_shadow_generation_dispatch_tick.find(id);
    if (it == m_shadow_generation_dispatch_tick.end()) {
        return; // boot-path / save-adopted chunk, or a re-mesh — not a first activation
    }
    m_shadow_gen_latency_samples.push_back(m_shadow_current_tick - it->second);
    m_shadow_generation_dispatch_tick.erase(it);
}

void SHIELD_WorldSystem::shadow_note_promotion_published(ChunkID id) {
    if (m_shadow_current_tick < 0) {
        return;
    }
    const auto it = m_shadow_promotion_dispatch_tick.find(id);
    if (it == m_shadow_promotion_dispatch_tick.end()) {
        return;
    }
    m_shadow_promo_latency_samples.push_back(m_shadow_current_tick - it->second);
    m_shadow_promotion_dispatch_tick.erase(it);
}

void SHIELD_WorldSystem::shadow_note_evicted(ChunkID id) {
    m_shadow_generation_dispatch_tick.erase(id);
    m_shadow_promotion_dispatch_tick.erase(id);
}

SHIELD_WorldSystem::ActivationShadowReport SHIELD_WorldSystem::activation_shadow_report() const {
    ActivationShadowReport report;
    report.generation_to_ready_ticks = m_shadow_gen_latency_samples;
    report.promotion_to_publish_ticks = m_shadow_promo_latency_samples;
    report.generation_dispatches = m_shadow_gen_dispatches;
    report.promotion_dispatches = m_shadow_promo_dispatches;
    report.still_pending =
        m_shadow_generation_dispatch_tick.size() + m_shadow_promotion_dispatch_tick.size();
    return report;
}

void SHIELD_WorldSystem::quiesce_streaming_jobs_for_save() {
    if (!m_job_system) {
        return;
    }
    for (auto& batch : m_streaming_state.generation_batches) {
        if (batch.handle.counter) {
            m_job_system->wait(batch.handle);
        }
    }
    if (m_streaming_state.promotion_job_handle_high.counter) {
        m_job_system->wait(m_streaming_state.promotion_job_handle_high);
    }
    if (m_streaming_state.promotion_job_handle.counter) {
        m_job_system->wait(m_streaming_state.promotion_job_handle);
    }
    for (auto& batch : m_streaming_state.meshing_batches) {
        if (batch.handle_high.counter) {
            m_job_system->wait(batch.handle_high);
        }
        if (batch.handle.counter) {
            m_job_system->wait(batch.handle);
        }
    }
}

void SHIELD_WorldSystem::activate_due(std::int64_t tick) {
    // activation queue : publish, in FIFO order, exactly the batches due at or
    // before `tick`. A due-but-unfinished batch BLOCKS on its jobs (K
    // under-covering is a wall-clock cost, never a schedule change); a batch
    // with due_tick -1 (no tick source at dispatch) publishes when drained.
    // Until the barrier swap this is exercised only by the
    // ActivationQueueSemantics gtest; the per-tick barrier force-drains
    // everything regardless of due ticks.

    // Generation lane.
    while (!m_streaming_state.generation_batches.empty()) {
        auto& front = m_streaming_state.generation_batches.front();
        const bool due =
            front.due_tick < 0 ? !has_active_job(front.handle) : front.due_tick <= tick;
        if (!due) {
            break;
        }
        if (m_job_system && front.handle.counter) {
            m_job_system->wait(front.handle);
        }
        publish_front_generation_batch(/*force=*/true);
    }

    // Promotion lane (depth 1): publish sim truth + dispatch stage B when due.
    if (promotion_pipeline_pending()) {
        const std::int64_t promo_due = m_streaming_state.promotion_due_tick;
        const bool due = promo_due < 0 ? !promotion_jobs_active() : promo_due <= tick;
        if (due) {
            if (m_job_system) {
                if (m_streaming_state.promotion_job_handle_high.counter) {
                    m_job_system->wait(m_streaming_state.promotion_job_handle_high);
                }
                if (m_streaming_state.promotion_job_handle.counter) {
                    m_job_system->wait(m_streaming_state.promotion_job_handle);
                }
            }
            process_completed_promotion_jobs(/*force=*/true);
            if (!promotion_pipeline_pending()) {
                m_streaming_state.promotion_due_tick = -1;
            }
        }
    }

    // Meshing lane.
    while (!m_streaming_state.meshing_batches.empty()) {
        auto& front = m_streaming_state.meshing_batches.front();
        const bool due = front.due_tick < 0
                             ? !(has_active_job(front.handle) || has_active_job(front.handle_high))
                             : front.due_tick <= tick;
        if (!due) {
            break;
        }
        if (m_job_system) {
            if (front.handle_high.counter) {
                m_job_system->wait(front.handle_high);
            }
            if (front.handle.counter) {
                m_job_system->wait(front.handle);
            }
        }
        publish_front_meshing_batch(/*force=*/true);
    }
}

void SHIELD_WorldSystem::wait_for_promotion_jobs() {
    if (m_job_system && m_streaming_state.promotion_job_handle_high.counter) {
        m_job_system->wait(m_streaming_state.promotion_job_handle_high);
    }
    if (m_job_system && m_streaming_state.promotion_job_handle.counter) {
        m_job_system->wait(m_streaming_state.promotion_job_handle);
    }
    process_completed_promotion_jobs(/*force=*/true);
}

void SHIELD_WorldSystem::process_completed_promotion_jobs(bool force) {
    // Stage-A publish: only once BOTH promotion batches have fully drained
    // (mirrors process_completed_meshing_jobs' both-lanes gate) AND, in the
    // non-force regime, the lane's due tick has arrived — publication order
    // is the dispatch order, which is the deterministic candidate-sort
    // order, so run == replay.
    // activation queue: a tick-stamped promotion pipeline publishes exclusively via
    // activate_due / force drains (see publish_front_generation_batch).
    const bool promotion_due = force || m_streaming_state.promotion_due_tick < 0;
    if (promotion_due && !m_streaming_state.promotion_job_chunks.empty() &&
        !promotion_jobs_active()) {
        for (auto& job_chunk : m_streaming_state.promotion_job_chunks) {
            auto& chunk = job_chunk.chunk;
            if (!chunk) {
                continue;
            }
            const bool ready = chunk->pending_promotion_ready.load(std::memory_order_acquire);
            const bool failed = chunk->pending_promotion_failed.load(std::memory_order_acquire);
            if (ready && !failed && !chunk->pending_sdf_data.empty()) {
                // The LOD0-promotion publish (, moved here from
                // process_completed_meshing_jobs by ): pure
                // generation output, not an edit — the dirty flag stays
                // clear (matching GenerateChunkData's contract).
                chunk->sdf_data = std::move(chunk->pending_sdf_data);
                chunk->heightmap_data = std::move(chunk->pending_heightmap_data);
                // publish the promoted structure material channel
                // alongside the SDF (empty -> empty, lazy alloc preserved).
                chunk->material_data = std::move(chunk->pending_material_data);
                chunk->mark_sdf_generated_current_params();
                chunk->clear_voxel_data_dirty();
                shadow_note_promotion_published(chunk->get_id()); //  shadow
                m_streaming_state.pending_promotion_mesh.push_back(job_chunk);
            } else {
                // Stage-A failure: revert the transient Meshing state (the
                // meshing lane's failure reversion, verbatim) so the chunk
                // re-candidates on a later pass.
                if (chunk->mesh_vertices.empty() || chunk->mesh_indices.empty()) {
                    chunk->set_state(Luminumbra::ChunkState::Idle);
                } else {
                    chunk->set_state(Luminumbra::ChunkState::Ready);
                }
                chunk->pending_lod.store(-1, std::memory_order_release);
            }
            chunk->pending_sdf_data.clear();
            chunk->pending_heightmap_data.clear();
            chunk->pending_material_data.clear();
            chunk->pending_promotion_ready.store(false, std::memory_order_release);
            chunk->pending_promotion_failed.store(false, std::memory_order_release);
        }
        m_streaming_state.promotion_job_chunks.clear();
        m_streaming_state.promotion_job_handle = {};
        m_streaming_state.promotion_job_handle_high = {};
    }

    // Stage-B dispatch: render meshes for published promotions, down the
    // ordinary meshing lane. The items re-classify as non-promotion there
    // (their live sdf_data is full now), so there is no recursion.
    //  5b: gate by FIFO depth (deterministic), not raw job activity.
    if (!m_streaming_state.pending_promotion_mesh.empty() &&
        m_streaming_state.meshing_batches.size() < kMaxMeshingBatchesInFlight) {
        std::vector<MeshingWorkItem> stage_b;
        stage_b.reserve(m_streaming_state.pending_promotion_mesh.size());
        for (const auto& item : m_streaming_state.pending_promotion_mesh) {
            if (!item.chunk) {
                continue;
            }
            // Deterministic orphan guard: a chunk evicted mid-pipeline must
            // not be resurrected by its render-mesh dispatch (the eviction
            // pass is main-thread deterministic, so this skip is too).
            const auto it = m_streaming_state.chunks.find(item.chunk->get_id());
            if (it == m_streaming_state.chunks.end() || it->second.get() != item.chunk.get()) {
                continue;
            }
            stage_b.push_back(
                {item.chunk, item.lod_level, /*terrain_mesh_required=*/true, item.high_priority});
        }
        m_streaming_state.pending_promotion_mesh.clear();
        if (!stage_b.empty()) {
            dispatch_meshing_jobs(stage_b);
        }
    }
}

void SHIELD_WorldSystem::dispatch_promotion_jobs(
    const std::vector<MeshingWorkItem>& chunks_to_promote) {
    process_completed_promotion_jobs(/*force=*/false);
    if (promotion_jobs_active()) {
        // One promotion batch in flight at a time (the meshing lane's
        // semantics); refused items re-candidate on a later pass, exactly as
        // a refused meshing dispatch would.
        return;
    }

    std::vector<Luminumbra::Job> high_priority_jobs;
    std::vector<Luminumbra::Job> normal_priority_jobs;
    m_streaming_state.promotion_job_chunks.clear();
    m_streaming_state.promotion_job_chunks.reserve(chunks_to_promote.size());
    for (const MeshingWorkItem& work_item : chunks_to_promote) {
        auto& chunk = work_item.chunk;
        // Same state trajectory as a meshing dispatch: a chunk with an active
        // (coarse) mesh stays Ready so it keeps rendering while the promotion
        // is pending; a meshless chunk flips to Meshing.
        const bool has_active_mesh = chunk->get_state() == Luminumbra::ChunkState::Ready &&
                                     !chunk->mesh_vertices.empty() && !chunk->mesh_indices.empty();
        if (!has_active_mesh) {
            chunk->set_state(Luminumbra::ChunkState::Meshing);
        }
        chunk->pending_sdf_data.clear();
        chunk->pending_heightmap_data.clear();
        chunk->pending_material_data.clear();
        chunk->pending_promotion_ready.store(false, std::memory_order_release);
        chunk->pending_promotion_failed.store(false, std::memory_order_release);
        chunk->pending_lod.store(work_item.lod_level, std::memory_order_release);
        m_streaming_state.promotion_job_chunks.push_back(
            {chunk, work_item.lod_level, work_item.high_priority});
        shadow_note_promotion_dispatch(chunk->get_id()); //  shadow

        auto& lane_jobs = work_item.high_priority ? high_priority_jobs : normal_priority_jobs;
        lane_jobs.emplace_back([this, chunk]() {
            try {
                const auto worldgen_scope = acquire_worldgen_sample_scope(); //
                // Build the full LOD0 voxel field into a scratch chunk and
                // stage it — the live chunk's sdf_data is never touched off
                // the main thread (a concurrent far-LOD sampler must never
                // observe a half-written field).
                Luminumbra::Chunk scratch(chunk->get_coords());
                GenerateChunkData(scratch, 1);
                chunk->pending_sdf_data = std::move(scratch.sdf_data);
                chunk->pending_heightmap_data = std::move(scratch.heightmap_data);
                chunk->pending_material_data = std::move(scratch.material_data);
                chunk->pending_promotion_ready.store(true, std::memory_order_release);
            } catch (const std::exception& e) {
                LUMINUMBRA_CORE_ERROR("PROMOTION JOB CRASH: Chunk ({},{},{}) failed: {}",
                                      chunk->get_coords().x,
                                      chunk->get_coords().y,
                                      chunk->get_coords().z,
                                      e.what());
                chunk->pending_promotion_failed.store(true, std::memory_order_release);
            } catch (...) {
                LUMINUMBRA_CORE_ERROR(
                    "PROMOTION JOB CRASH: Chunk ({},{},{}) failed with unknown exception",
                    chunk->get_coords().x,
                    chunk->get_coords().y,
                    chunk->get_coords().z);
                chunk->pending_promotion_failed.store(true, std::memory_order_release);
            }
        });
    }
    m_promotion_batches_dispatched++;
    m_promotion_chunks_dispatched += m_streaming_state.promotion_job_chunks.size();
    // tick-keyed activation stamp (inert until the swap).
    m_streaming_state.promotion_due_tick =
        m_current_sim_tick >= 0 ? m_current_sim_tick + kActivationPipelineLatencyTicks : -1;

    if (m_job_system && (!high_priority_jobs.empty() || !normal_priority_jobs.empty())) {
        if (!high_priority_jobs.empty()) {
            m_streaming_state.promotion_job_handle_high =
                m_job_system->dispatch_batch(high_priority_jobs, JobPriority::High);
        }
        if (!normal_priority_jobs.empty()) {
            m_streaming_state.promotion_job_handle =
                m_job_system->dispatch_batch(normal_priority_jobs);
        }
    } else {
        for (auto& job : high_priority_jobs) {
            job();
        }
        for (auto& job : normal_priority_jobs) {
            job();
        }
        process_completed_promotion_jobs(/*force=*/true);
    }
}

void SHIELD_WorldSystem::reinitialize_noise(const TerrainGenParams* params,
                                            std::optional<int> seed) {
    // QUIESCE off-main-thread worldgen samplers for the rebuild.
    // Every sampling job holds the shared side of the epoch gate for its
    // duration; taking the exclusive side here waits out in-flight samplers
    // and blocks new ones until the generator set is consistent again — the
    // proper fix for the worldgen-preview pan crash (the assign-last ordering
    // below stays as belt-and-braces, no longer the correctness mechanism).
    m_worldgen_writer_pending.store(true, std::memory_order_release);
    struct WriterIntentReset {
        std::atomic<bool>& pending;
        ~WriterIntentReset() {
            pending.store(false, std::memory_order_release);
        }
    } writer_intent_reset{m_worldgen_writer_pending};
    std::unique_lock<std::shared_mutex> worldgen_epoch(m_worldgen_epoch_mutex);
    // Publish parameters and seed under the same epoch as their generators.
    // Preview readers must retain the previous complete world until they release it.
    if (params) {
        m_params = *params;
    }
    if (seed) {
        m_seed = *seed;
    }

    // Terrain height is a pure function of seed/params; drop the cached
    // per-column surface spans whenever either changes.
    m_column_surface_span_cache.clear();

    // FastNoise2 uses a node-based system to build complex generators.

    // 1. Terrain Height Generator (Fractal Simplex Noise)
    auto terrain_noise = NewWorldNoise<FastNoise::Simplex>();
    auto terrain_fractal = NewWorldNoise<FastNoise::FractalFBm>();
    terrain_fractal->SetSource(terrain_noise);
    terrain_fractal->SetOctaveCount(m_params.octaves);
    terrain_fractal->SetLacunarity(m_params.lacunarity);
    terrain_fractal->SetGain(m_params.persistence);
    m_terrain_generator = terrain_fractal;

    // 2. Cave Generator (3D Perlin Noise) — the cheese BODY field.
    auto cave_noise = NewWorldNoise<FastNoise::Perlin>();
    m_cave_generator = cave_noise;
    // 2b.  noise-router SPAGHETTI tunnel field (a second, decorrelated Perlin).
    // The noise router is the sole cave implementation.

    m_spaghetti_generator = NewWorldNoise<FastNoise::Perlin>();
    // 2c. Worley CHEESE caverns: cellular / edge field -> big ROUNDED rooms (vs the
    // Perlin body's blobs). Index0Div1 returns / in [0,1]: small at cell centres
    // (carve = open room), ->1 at cell walls (stone). Used whenever caves are enabled.
    auto worley = NewWorldNoise<FastNoise::CellularDistance>();
    worley->SetDistanceIndex0(0);
    worley->SetDistanceIndex1(2); //  and
    worley->SetReturnType(FastNoise::CellularDistance::ReturnType::Index0Div1);
    m_worley_generator = worley;

    // 3. Island Mask Generator (Low-frequency Simplex)
    auto island_noise = NewWorldNoise<FastNoise::Simplex>();
    m_island_mask_generator = island_noise;

    // 4. Shaping control noises (seed registry: +3 continentalness,
    // +4 erosion, +5 peaks/valleys, +6/+7 domain warp X/Z).
    // The epoch lock above quiesces samplers while rebuilding. Assign each
    // fully configured node last, retaining the defensive old-to-new ordering.

    auto continental_fractal = NewWorldNoise<FastNoise::FractalFBm>();
    continental_fractal->SetSource(NewWorldNoise<FastNoise::Simplex>());
    continental_fractal->SetOctaveCount(3);
    m_continentalness_generator = continental_fractal;

    auto erosion_fractal = NewWorldNoise<FastNoise::FractalFBm>();
    erosion_fractal->SetSource(NewWorldNoise<FastNoise::Simplex>());
    erosion_fractal->SetOctaveCount(3);
    m_erosion_generator = erosion_fractal;

    auto peaks_fractal = NewWorldNoise<FastNoise::FractalRidged>();
    peaks_fractal->SetSource(NewWorldNoise<FastNoise::Simplex>());
    peaks_fractal->SetOctaveCount(2);
    m_peaks_generator = peaks_fractal;

    m_warp_generator = NewWorldNoise<FastNoise::Simplex>();

    // 4b.  river noise (seed registry: +10). A ridged FBm whose folded
    //     PV near-zero band carves the river channels. Built only when the
    //     preset opts in; legacy worlds never construct it. (Same never-null race fix:
    //     assign in the enabled branch, null only in the else.)
    if (m_params.rivers_enabled) {
        auto river_fractal = NewWorldNoise<FastNoise::FractalRidged>();
        river_fractal->SetSource(NewWorldNoise<FastNoise::Simplex>());
        river_fractal->SetOctaveCount(2);
        m_river_generator = river_fractal;
    } else {
        m_river_generator = {};
    }

    // 5.  biome climate noises (seed registry: +8 temperature,
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
        auto temperature_fractal = NewWorldNoise<FastNoise::FractalFBm>();
        temperature_fractal->SetSource(NewWorldNoise<FastNoise::Simplex>());
        temperature_fractal->SetOctaveCount(2);
        m_temperature_generator = temperature_fractal;

        auto humidity_fractal = NewWorldNoise<FastNoise::FractalFBm>();
        humidity_fractal->SetSource(NewWorldNoise<FastNoise::Simplex>());
        humidity_fractal->SetOctaveCount(2);
        m_humidity_generator = humidity_fractal;

        m_biome_table = World::BiomeTable::Load(m_params.biome_table_path);
        if (!m_biome_table.ok() || m_biome_table.empty()) {
            for (const std::string& error : m_biome_table.errors()) {
                LUMINUMBRA_CORE_WARN("biome table load error: {}", error);
            }
            LUMINUMBRA_CORE_WARN("biomes requested but table '{}' failed to load; falling back to "
                                 "legacy single-material classification",
                                 m_params.biome_table_path);
            // Table failed: disable biomes so the world is still byte-zero
            // (legacy) rather than half-applied.
            m_biomes_enabled = false;
        } else {
            m_biomes_enabled = true;
            m_params.biome_table_content_hash = m_biome_table.content_hash();
        }
    }

    // structure template pools. Loaded only when the preset opts in;
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
            World::StructureTemplatePool pool = World::LoadStructureTemplatePool(type_dir, type);
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

float SHIELD_WorldSystem::EvaluateShapingSpline(const std::vector<std::array<float, 2>>& points,
                                                float input,
                                                float fallback) {
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

SHIELD_WorldSystem::ShapedHeightSample
SHIELD_WorldSystem::ComputeShapedHeightSample(float world_x, float world_z) const {
    return ComputeShapedHeightSampleImpl(world_x, world_z, /*apply_hydro=*/true);
}

SHIELD_WorldSystem::ShapedHeightSample SHIELD_WorldSystem::ComputeShapedHeightSampleImpl(
    float world_x, float world_z, bool apply_hydro) const {
    ShapedHeightSample sample;

    // Sampling scopes hold the shared epoch while reinitialization takes the
    // exclusive epoch. Shaping nodes are always built; local references pin
    // their lifetimes throughout this sample without an algorithm fallback.
    const auto warp_gen = m_warp_generator;
    const auto continental_gen = m_continentalness_generator;
    const auto erosion_gen = m_erosion_generator;
    const auto peaks_gen = m_peaks_generator;

    // Domain warp (seed +6 / +7) displaces the BASE detail (and pv)
    // sample coordinates; the control channels read the unwarped point so
    // the macro structure stays stable under the warp.
    const float warp_x = m_params.domain_warp_amplitude *
                         warp_gen->GenSingle2D(world_x * m_params.domain_warp_frequency,
                                               world_z * m_params.domain_warp_frequency,
                                               m_seed + 6);
    const float warp_z = m_params.domain_warp_amplitude *
                         warp_gen->GenSingle2D(world_x * m_params.domain_warp_frequency,
                                               world_z * m_params.domain_warp_frequency,
                                               m_seed + 7);
    const float sample_x = world_x + warp_x;
    const float sample_z = world_z + warp_z;

    const float continentalness =
        continental_gen->GenSingle2D(world_x * m_params.continentalness_frequency,
                                     world_z * m_params.continentalness_frequency,
                                     m_seed + 3);
    const float erosion = erosion_gen->GenSingle2D(
        world_x * m_params.erosion_frequency, world_z * m_params.erosion_frequency, m_seed + 4);
    const float peaks_valleys = peaks_gen->GenSingle2D(
        sample_x * m_params.peaks_frequency, sample_z * m_params.peaks_frequency, m_seed + 5);

    const float base_level =
        EvaluateShapingSpline(m_params.continental_spline, continentalness, 0.0f);
    const float amplitude_multiplier =
        EvaluateShapingSpline(m_params.erosion_spline, erosion, 1.0f);
    // Ridge term: peaks only where erosion is low (eroded land is flat).
    const float erosion_01 = std::clamp((erosion + 1.0f) * 0.5f, 0.0f, 1.0f);
    float ridge = EvaluateShapingSpline(m_params.peaks_spline, peaks_valleys, 0.0f) *
                  m_params.peaks_amplitude * std::max(0.0f, 1.0f - erosion_01);

    //  per-biome morphology: scale the ridge by the (continuous)
    // temperature field — cold ground (alpine) gets taller/rugged peaks, warm
    // lowlands gentler. Only the ridge term; smooth so no seams. Byte-identical
    // to the batched path in ComputeShapedHeightGrid.
    if (m_params.biome_relief_enabled && m_temperature_generator) {
        const float temp =
            m_temperature_generator->GenSingle2D(world_x * m_params.temperature_frequency,
                                                 world_z * m_params.temperature_frequency,
                                                 m_seed + 8);
        const float cold01 = std::clamp(0.5f - 0.5f * temp, 0.0f, 1.0f);
        ridge *= 1.0f + m_params.biome_relief_strength * (2.0f * cold01 - 1.0f);
    }

    sample.base_noise = m_terrain_generator->GenSingle2D(
        sample_x * m_params.base_frequency, sample_z * m_params.base_frequency, m_seed);
    float terrain_height = m_params.height_offset + base_level +
                           amplitude_multiplier * (sample.base_noise * m_params.base_amplitude) +
                           ridge;
    //  cliffs: terrace the shaped height inside cliff zones (byte-identical
    // to ComputeShapedHeightGrid). No-op when cliffs disabled / outside a zone.
    terrain_height = CliffTerracedHeight(world_x, world_z, terrain_height);
    sample.pre_island_height = terrain_height;
    sample.final_height = terrain_height;

    if (m_params.island_mask_enabled) {
        sample.island_applied = true;
        sample.island_noise =
            m_island_mask_generator->GenSingle2D(world_x * m_params.island_mask_frequency,
                                                 world_z * m_params.island_mask_frequency,
                                                 m_seed + 2);
        sample.island_mask = glm::smoothstep(0.1f, 0.25f, sample.island_noise);
        sample.final_height = glm::mix(m_params.height_offset, terrain_height, sample.island_mask);
    }

    // river carve. Where the +10 PV-band river noise is in the valleys
    // band, lower the final height toward a channel floor below SEA_LEVEL so
    // the existing global water plane (SEA_LEVEL) fills the channel - no
    // WaterSystem changes (regression review). The carve depth scales with river
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

    //  lake basins. Where the lake field is high, pull the surface toward a
    // floor below SEA_LEVEL so the global water plane fills it (mirrors the river
    // carve; lake_max_carve gates it to low terrain only). Skipped (byte-zero) when
    // lakes are disabled. Kept byte-identical to ComputeShapedHeightGrid.
    if (m_params.lakes_enabled) {
        const float lake_influence = LakeInfluenceFromNoise(world_x, world_z);
        const float lake_surface = LakeSurfaceLevel(world_x, world_z);
        sample.final_height -= LakeCarveAmount(sample.final_height, lake_influence, lake_surface);
    }

    // sinkhole/cave-mouth RIM depression. Dips the heightfield inside a
    // doline footprint so the feature reads at distance/coarse-LOD (the SDF carve
    // alone is invisible on the SDF-ignoring far path). Folded into ALL THREE
    // height paths byte-identically. No-op (0) when surface_breaks disabled.
    sample.final_height -= SurfaceBreakRimDepression(world_x, world_z);

    // hydraulic/thermal relief (decision a). Added LAST so the baked
    // drainage/talus sits in the final surface EVERY height consumer reads
    // (collision/spawn/water/far-LOD/mesh). The per-region bake samples the
    // NO-hydro height (apply_hydro=false) so this never recurses. Skipped (byte-
    // identical legacy path) when hydro is disabled -> world_hash unchanged.
    if (apply_hydro && m_params.hydro_enabled) {
        sample.final_height += SampleHydroOffsetMeters(world_x, world_z);
    }

    return sample;
}

namespace {
// hydraulic-relief region geometry. A region is kHydroRegionCells erosion
// cells per side; offsets are indexed by GLOBAL erosion cell so bilinear sampling
// crosses region boundaries seamlessly (each region's interior is, by the hydraulic erosion kernel
// halo-independence proof, byte-identical to a single global bake). The halo
// margin keeps the interior strictly independent of terrain beyond the halo.
constexpr int kHydroRegionCells = 64;
constexpr int kHydroHaloMargin = 8;
// Cap on resident baked regions. Each entry is kHydroRegionCells^2 floats
// (16 KB), so 2048 regions bound the cache at 32 MB while covering a
// 32x32-region (~16x16 km) working set — far beyond the far-LOD prefetch
// reach, so a live session never evicts what it is still reading. Bakes are
// pure recompute-on-load, so eviction can only cost a re-bake, never change
// a sampled offset.
constexpr std::size_t kHydroCacheMaxRegions = 2048;

std::int64_t HydroFloorDiv(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    const std::int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
} // namespace

float SHIELD_WorldSystem::SampleHydroOffsetMeters(float world_x, float world_z) const {
    const float cs = m_params.hydro_cell_size_m;
    if (cs <= 0.0f) {
        return 0.0f;
    }
    // World -> fractional GLOBAL erosion-cell coordinates.
    const float gxf = world_x / cs;
    const float gzf = world_z / cs;
    const std::int64_t gx0 = static_cast<std::int64_t>(std::floor(gxf));
    const std::int64_t gz0 = static_cast<std::int64_t>(std::floor(gzf));
    const float fx = gxf - static_cast<float>(gx0);
    const float fz = gzf - static_cast<float>(gz0);

    // Bake one region's interior offset grid. PURE function of (region, seed,
    // params); runs OUTSIDE any lock so concurrent chunk-gen threads never
    // serialize on the (expensive) bake -- a duplicate concurrent bake is
    // byte-identical, so try_emplace keeps whichever lands first.
    auto bake_region = [this, cs](std::int64_t rx, std::int64_t rz) -> std::vector<float> {
        const int halo = m_params.hydro_iterations + kHydroHaloMargin;
        const int m = kHydroRegionCells + 2 * halo;
        const std::size_t cells = static_cast<std::size_t>(m) * static_cast<std::size_t>(m);
        // Build the padded grid's world coords, then sample the NO-hydro base
        // heights in ONE SIMD batch (apply_hydro=false: no recursion, and the
        // GenPositionArray2D path is ~10x faster than per-cell GenSingle2D --
        // keeps the bake off the far-LOD settle critical path). Byte-identical to
        // the scalar path (position-array parity gate), so the offset is unchanged.
        std::vector<float> xs(cells, 0.0f);
        std::vector<float> zs(cells, 0.0f);
        for (int pz = 0; pz < m; ++pz) {
            for (int px = 0; px < m; ++px) {
                const std::size_t idx = static_cast<std::size_t>(pz) * static_cast<std::size_t>(m) +
                                        static_cast<std::size_t>(px);
                xs[idx] = static_cast<float>(rx * kHydroRegionCells + (px - halo)) * cs;
                zs[idx] = static_cast<float>(rz * kHydroRegionCells + (pz - halo)) * cs;
            }
        }
        std::vector<float> heights(cells, 0.0f);
        ComputeShapedHeightsAtPositions(xs.data(),
                                        zs.data(),
                                        cells,
                                        heights.data(),
                                        /*apply_hydro=*/false);
        World::HydroErosionParams p;
        p.iterations = m_params.hydro_iterations;
        p.talus_height = m_params.hydro_talus_height;
        p.thermal_rate = m_params.hydro_thermal_rate;
        p.rain_per_sweep = m_params.hydro_rain_per_sweep;
        p.solubility = m_params.hydro_solubility;
        p.deposition = m_params.hydro_deposition;
        p.evaporation = m_params.hydro_evaporation;
        p.sediment_capacity = m_params.hydro_sediment_capacity;
        p.max_offset = m_params.hydro_max_offset;
        std::vector<float> offset;
        World::BakeHydraulicErosion(heights, kHydroRegionCells, halo, p, offset);
        return offset;
    };

    // Fetch one global cell's baked offset. Warm-cache reads take a SHARED lock
    // (concurrent across chunk-gen jobs); a cold region is baked OUTSIDE the lock
    // then inserted under the exclusive lock. std::map node storage keeps cached
    // values stable; recompute-on-load (not persisted), deterministic.
    auto offset_at = [this, &bake_region](std::int64_t gx, std::int64_t gz) -> float {
        const std::int64_t rx = HydroFloorDiv(gx, kHydroRegionCells);
        const std::int64_t rz = HydroFloorDiv(gz, kHydroRegionCells);
        const int lx = static_cast<int>(gx - rx * kHydroRegionCells);
        const int lz = static_cast<int>(gz - rz * kHydroRegionCells);
        const std::pair<std::int64_t, std::int64_t> key(rx, rz);
        const std::size_t local =
            static_cast<std::size_t>(lz) * static_cast<std::size_t>(kHydroRegionCells) +
            static_cast<std::size_t>(lx);
        {
            std::shared_lock<std::shared_mutex> rlock(m_hydro_mutex);
            auto it = m_hydro_cache.find(key);
            if (it != m_hydro_cache.end()) {
                return it->second[local];
            }
        }
        std::vector<float> baked = bake_region(rx, rz); // outside any lock
        std::unique_lock<std::shared_mutex> wlock(m_hydro_mutex);
        auto it = m_hydro_cache.try_emplace(key, std::move(baked)).first;
        // Bound the cache: past the cap, evict the region FARTHEST (Chebyshev)
        // from the one just requested — the just-requested key approximates the
        // current activity centre, so hot neighbourhoods survive and only stale
        // far-behind regions fall out. Safe under the exclusive lock: readers
        // copy a single float under a lock and never retain references. Purely
        // a cost bound; a re-read of an evicted region re-bakes byte-identically.
        if (m_hydro_cache.size() > kHydroCacheMaxRegions) {
            auto victim = m_hydro_cache.end();
            std::int64_t worst = -1;
            for (auto cit = m_hydro_cache.begin(); cit != m_hydro_cache.end(); ++cit) {
                const std::int64_t d = std::max(std::llabs(cit->first.first - key.first),
                                                std::llabs(cit->first.second - key.second));
                if (d > worst) {
                    worst = d;
                    victim = cit;
                }
            }
            if (victim != m_hydro_cache.end() && victim != it) {
                m_hydro_cache.erase(victim);
            }
        }
        return it->second[local];
    };

    const float o00 = offset_at(gx0, gz0);
    const float o10 = offset_at(gx0 + 1, gz0);
    const float o01 = offset_at(gx0, gz0 + 1);
    const float o11 = offset_at(gx0 + 1, gz0 + 1);
    const float a = o00 + (o10 - o00) * fx;
    const float b = o01 + (o11 - o01) * fx;
    return a + (b - a) * fz;
}

void SHIELD_WorldSystem::PrefetchHydroRegions(float cx, float cz, float radius_m) const {
    // Warm the hydraulic-erosion region cache AHEAD of chunk-gen demand on
    // background jobs, so the (expensive) per-region bake never lands on the
    // gen/main critical path when the camera flies into new terrain. Each region
    // bakes ONCE; the inflight set stops re-dispatching it before its job lands,
    // and the cache check stops re-dispatching baked regions.
    const float cs = m_params.hydro_cell_size_m;
    if (!m_params.hydro_enabled || m_job_system == nullptr || cs <= 0.0f) {
        return;
    }
    const float region_m = static_cast<float>(kHydroRegionCells) * cs; // 512 m
    const std::int64_t reach = static_cast<std::int64_t>(std::ceil(radius_m / region_m)) + 1;
    const std::int64_t crx =
        HydroFloorDiv(static_cast<std::int64_t>(std::floor(cx / cs)), kHydroRegionCells);
    const std::int64_t crz =
        HydroFloorDiv(static_cast<std::int64_t>(std::floor(cz / cs)), kHydroRegionCells);
    std::vector<Job> jobs;
    for (std::int64_t dz = -reach; dz <= reach; ++dz) {
        for (std::int64_t dx = -reach; dx <= reach; ++dx) {
            const std::pair<std::int64_t, std::int64_t> rkey(crx + dx, crz + dz);
            {
                std::shared_lock<std::shared_mutex> rl(m_hydro_mutex);
                if (m_hydro_cache.find(rkey) != m_hydro_cache.end()) {
                    continue; // already baked
                }
            }
            {
                std::lock_guard<std::mutex> g(m_hydro_prefetch_mutex);
                if (!m_hydro_prefetch_inflight.insert(rkey).second) {
                    continue; // already queued
                }
            }
            const float sample_x =
                static_cast<float>(rkey.first * kHydroRegionCells) * cs + region_m * 0.5f;
            const float sample_z =
                static_cast<float>(rkey.second * kHydroRegionCells) * cs + region_m * 0.5f;
            jobs.emplace_back([this, rkey, sample_x, sample_z]() {
                SampleHydroOffsetMeters(sample_x, sample_z); // triggers + caches the region bake
                std::lock_guard<std::mutex> g(m_hydro_prefetch_mutex);
                m_hydro_prefetch_inflight.erase(rkey);
            });
        }
    }
    if (!jobs.empty()) {
        m_job_system->dispatch_batch(jobs); // fire-and-forget background bakes
    }
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

float SHIELD_WorldSystem::LakeInfluenceFromNoise(float world_x, float world_z) const {
    // Lake field: a smooth FBM (reusing the continentalness generator at seed +11)
    // so lakes are blobby basins. Influence ramps 0->1 above lake_threshold. Pure;
    // 0 when lakes are disabled.
    if (!m_params.lakes_enabled) {
        return 0.0f;
    }
    const float v = m_continentalness_generator->GenSingle2D(
        world_x * m_params.lake_frequency, world_z * m_params.lake_frequency, m_seed + 11);
    if (v < m_params.lake_threshold) {
        return 0.0f;
    }
    const float span = std::max(1e-4f, 1.0f - m_params.lake_threshold);
    return std::clamp((v - m_params.lake_threshold) / span, 0.0f, 1.0f);
}

float SHIELD_WorldSystem::ContinentalBaseHeight(float world_x, float world_z) const {
    // Smooth regional base: height_offset + the continental spline of the
    // continentalness control noise (seed +3) ONLY — no ridge, no base detail
    // noise. Low-frequency, so a lake's surface (derived from this) reads ~flat
    // over its extent.

    const float continentalness =
        m_continentalness_generator->GenSingle2D(world_x * m_params.continentalness_frequency,
                                                 world_z * m_params.continentalness_frequency,
                                                 m_seed + 3);
    return m_params.height_offset +
           EvaluateShapingSpline(m_params.continental_spline, continentalness, 0.0f);
}

float SHIELD_WorldSystem::LakeSurfaceLevel(float world_x, float world_z) const {
    // the lake surface must be near-constant (flat) over a lake's extent AND
    // continuous across the coarse-cell boundaries. The old code snapped to the
    // NEAREST 768 m node (std::round), so a lake straddling a cell boundary saw two
    // different surface levels and STEPPED at the seam. Instead, sample
    // ContinentalBaseHeight at the FOUR surrounding 768 m grid nodes and BILINEARLY
    // interpolate: the result is continuous everywhere (no step), still very flat
    // within a basin (the continental base is low-frequency, so the four nodes of a
    // single lake's enclosing cell are nearly equal). The carve (LakeCarveAmount via
    // WaterLevelAt) and the water mesh both call this, so basin floor and surface
    // stay consistent — the basin floor remains BELOW the surface because the carve
    // references the SAME interpolated value. Pure fn of (x,z,params,seed),
    // evaluated identically in every path.
    constexpr float kLakeCellMeters = 768.0f;
    const float gx = world_x / kLakeCellMeters;
    const float gz = world_z / kLakeCellMeters;
    const float fx0 = std::floor(gx);
    const float fz0 = std::floor(gz);
    const float tx = gx - fx0; // [0,1) within the cell
    const float tz = gz - fz0;
    const float x0 = fx0 * kLakeCellMeters;
    const float z0 = fz0 * kLakeCellMeters;
    const float x1 = x0 + kLakeCellMeters;
    const float z1 = z0 + kLakeCellMeters;
    const float h00 = ContinentalBaseHeight(x0, z0);
    const float h10 = ContinentalBaseHeight(x1, z0);
    const float h01 = ContinentalBaseHeight(x0, z1);
    const float h11 = ContinentalBaseHeight(x1, z1);
    const float a = h00 + (h10 - h00) * tx;
    const float b = h01 + (h11 - h01) * tx;
    const float surface = a + (b - a) * tz;
    return surface - m_params.lake_bank_offset;
}

float SHIELD_WorldSystem::WaterLevelAt(float world_x, float world_z) const {
    // Sea level everywhere, RAISED to the local (flat) lake surface inside a lake
    // basin so perched lakes sit at their basin elevation. max with SEA_LEVEL
    // keeps lakes that dip below sea level merged with the ocean.
    float level = SEA_LEVEL;
    if (m_params.lakes_enabled && LakeInfluenceFromNoise(world_x, world_z) > 0.0f) {
        level = std::max(level, LakeSurfaceLevel(world_x, world_z));
    }
    return level;
}

float SHIELD_WorldSystem::LakeCarveAmount(float final_height,
                                          float influence,
                                          float lake_surface) const {
    // Carve a basin whose floor sits lake_depth below the LOCAL lake surface (which
    // is at the basin's elevation, not SEA_LEVEL — that is what lets a lake form on
    // a mountain/valley). lake_max_carve * influence clamps the drop so the rim
    // (low influence) stays above the surface and holds the water. Zero outside.
    if (influence <= 0.0f) {
        return 0.0f;
    }
    const float lake_floor = lake_surface - m_params.lake_depth * influence;
    if (final_height <= lake_floor) {
        return 0.0f;
    }
    return std::min(final_height - lake_floor, m_params.lake_max_carve * influence);
}

float SHIELD_WorldSystem::CliffTerracedHeight(float world_x, float world_z, float height) const {
    if (!m_params.cliffs_enabled) {
        return height;
    }
    const float mask_noise = m_continentalness_generator->GenSingle2D(
        world_x * m_params.cliff_frequency, world_z * m_params.cliff_frequency, m_seed + 12);
    const float mask = std::clamp((mask_noise - m_params.cliff_threshold) / 0.15f, 0.0f, 1.0f);
    if (mask <= 0.0f) {
        return height;
    }
    // Snap toward flat benches (treads) joined by steep risers (cliff faces): the
    // fractional part of height/step is held flat until 0.55 then ramps sharply to
    // the next bench, so the surface reads as terraced mesa/canyon walls.
    const float step = std::max(1.0f, m_params.cliff_step);
    const float t = height / step;
    const float base = std::floor(t);
    const float frac = t - base;
    const float riser = glm::smoothstep(0.55f, 0.95f, frac);
    const float terraced = (base + riser) * step;
    return height + (terraced - height) * mask;
}

//  shared surface-break sampler. Scans the fixed 3x3 doline-cell neighborhood
// around world_pos, decodes each cell deterministically, and combines features with
// order-free ops (min cap, max carve). For each feature it runs an
// interior-proximity probe (one extra cave-noise read at y = surface - capDepth) so
// the cap is only lifted where the cave field is ALREADY carved -> never a blind pit.
// the single cave-carve composition point (MC-1.18 noise-router, on an SDF).
// Compose cheese BODY rooms, Worley caverns, and SPAGHETTI tunnels at
// the zero-crossing EDGE of a second Perlin (abs(noise) < thickness => air), composed via
// the surface cap + a hard max so isolated cheese bubbles become a connected network.
float SHIELD_WorldSystem::EvaluateCaveDensity(const Vec3& wp,
                                              float terrain_density,
                                              float effective_cap,
                                              float feature_carve,
                                              const float* precomputed_cheese) const {
    // All three noise terms are multiplied by zero above the surface cap.
    // Preserve the analytic surface-break carve, but avoid sampling fields that
    // cannot affect this density. This keeps the noise-router result bit-identical.
    if (cave_surface_blend(terrain_density, effective_cap) == 0.0f) {
        if (feature_carve <= 0.0f)
            return terrain_density;
        return exp_smax(terrain_density, -feature_carve, m_params.carve_smoothness);
    }
    // Cheese BODY field shared by scalar and batch samplers.
    // The hot batched-chunk path passes its precomputed batch value to skip a re-sample.
    const float cheese = precomputed_cheese
                             ? *precomputed_cheese
                             : m_cave_generator->GenSingle3D(wp.x * m_params.cave_frequency,
                                                             wp.y * m_params.cave_frequency,
                                                             wp.z * m_params.cave_frequency,
                                                             m_seed + 1);
    float density =
        apply_cave_field(terrain_density, cheese, m_params, effective_cap, feature_carve);

    // SPAGHETTI tunnels: |noise| small => near a tunnel centerline => air. Seed +20 keeps
    // the tunnel field decorrelated from the cheese field (m_seed + 1).
    const float sp = m_spaghetti_generator->GenSingle3D(wp.x * m_params.spaghetti_frequency,
                                                        wp.y * m_params.spaghetti_frequency,
                                                        wp.z * m_params.spaghetti_frequency,
                                                        m_seed + 20);
    const float thickness = std::max(1e-4f, m_params.spaghetti_thickness);
    const float edge = thickness - std::abs(sp); // >0 inside the tunnel, peak at the centerline
    if (edge > 0.0f) {
        const float tunnel_density = (edge / thickness) * m_params.cave_carve_value;
        // Respect the per-column surface cap exactly like the cheese field (no surface breaches).
        const float cap_blend = cave_surface_blend(terrain_density, effective_cap);
        const float capped = terrain_density + (tunnel_density - terrain_density) * cap_blend;
        density = std::max(density, capped);
    }

    // WORLEY CHEESE caverns: big ROUNDED rooms at cell interiors. / (Index0Div1) is small
    // at a cell centre -> carve an open room; ->1 at the cell wall -> stone. Low frequency =>
    // dramatic chambers. Composed via the same surface cap + max as the cheese/spaghetti terms.

    const float w = m_worley_generator->GenSingle3D(wp.x * m_params.worley_frequency,
                                                    wp.y * m_params.worley_frequency,
                                                    wp.z * m_params.worley_frequency,
                                                    m_seed + 21);
    const float room = m_params.worley_threshold - w; // >0 inside a room (w below threshold)
    if (room > 0.0f) {
        const float room_density =
            (room / std::max(1e-4f, m_params.worley_threshold)) * m_params.cave_carve_value;
        const float cap_blend = cave_surface_blend(terrain_density, effective_cap);
        const float capped = terrain_density + (room_density - terrain_density) * cap_blend;
        density = std::max(density, capped);
    }

    return density;
}

SHIELD_WorldSystem::SurfaceBreakInfo
SHIELD_WorldSystem::FindLargestSurfaceBreak(float near_x, float near_z, float scan_radius_m) const {
    SurfaceBreakInfo best;
    if (!m_params.surface_breaks_enabled)
        return best;
    const float cs = m_params.feature_cell_size;
    if (cs <= 0.0f)
        return best;
    const int cells = static_cast<int>(std::ceil(scan_radius_m / cs)) + 1;
    const int bcx = static_cast<int>(std::floor(near_x / cs));
    const int bcz = static_cast<int>(std::floor(near_z / cs));
    const float r2 = scan_radius_m * scan_radius_m;
    for (int dz = -cells; dz <= cells; ++dz) {
        for (int dx = -cells; dx <= cells; ++dx) {
            const SurfaceBreakFeature f =
                DecodeSurfaceBreakCell(m_seed, bcx + dx, bcz + dz, m_params);
            if (!f.valid)
                continue;
            const float ddx = f.center_x - near_x, ddz = f.center_z - near_z;
            if (ddx * ddx + ddz * ddz > r2)
                continue;
            if (f.radius > best.radius) { // prefer the biggest (most dramatic) opening
                best.found = true;
                best.x = f.center_x;
                best.z = f.center_z;
                best.radius = f.radius;
                best.depth = f.depth;
                best.shaft = f.shaft;
            }
        }
    }
    return best;
}

SHIELD_WorldSystem::SurfaceBreakSample
SHIELD_WorldSystem::sample_surface_breaks(const Vec3& world_pos, float surface_h) const {
    SurfaceBreakSample out{kCaveSurfaceCapDepth, 0.0f};
    if (!m_params.surface_breaks_enabled) {
        return out; // byte-identical disabled path
    }
    const float cs = m_params.feature_cell_size;
    if (cs <= 0.0f) {
        return out;
    }
    const int base_cx = static_cast<int>(std::floor(world_pos.x / cs));
    const int base_cz = static_cast<int>(std::floor(world_pos.z / cs));

    float min_cap = kCaveSurfaceCapDepth; // lower = more exposed
    float max_carve = 0.0f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = base_cx + dx;
            const int cz = base_cz + dz;
            const SurfaceBreakFeature f = DecodeSurfaceBreakCell(m_seed, cx, cz, m_params);
            if (!f.valid) {
                continue;
            }
            const float ddx = world_pos.x - f.center_x;
            const float ddz = world_pos.z - f.center_z;
            const float dist2 = ddx * ddx + ddz * ddz;
            if (dist2 >= f.radius * f.radius) {
                continue; // outside this feature's finite support
            }
            const float dist = std::sqrt(dist2);
            const float t = 1.0f - (dist / f.radius); // 1 at center -> 0 at rim

            // CAVE MOUTH: lift the cap only if the interior-proximity probe shows the
            // cave noise is already carved at depth under this column. The probe is a
            // single extra GenSingle3D at y = surface - capDepth, classified with the
            // same threshold the cave field uses.
            const float probe_y = surface_h - kCaveSurfaceCapDepth;
            const float cave_noise =
                m_cave_generator->GenSingle3D(world_pos.x * m_params.cave_frequency,
                                              probe_y * m_params.cave_frequency,
                                              world_pos.z * m_params.cave_frequency,
                                              m_seed + 1);
            const float cave_val = std::clamp((cave_noise + 1.0f) * 0.5f, 0.0f, 1.0f);
            const bool interior_carved = cave_val > m_params.cave_threshold;
            if (interior_carved) {
                // Smoothly drop the cap from 18 toward entrance_min_cap across the
                // footprint (deeper toward center). Min combine == order-free.
                const float cap_here =
                    kCaveSurfaceCapDepth +
                    (m_params.entrance_min_cap - kCaveSurfaceCapDepth) * smoothstep01(t);
                min_cap = std::min(min_cap, cap_here);
            }

            // SINKHOLE carve (always, gated by footprint): a funnel/shaft whose mouth
            // sits at the surface and reaches `depth` down. We carve in the SDF where
            // the point is within the inverted cone/capsule. The carve magnitude is the
            // negative signed distance (positive inside the funnel), clamped >= 0.
            // Point relative to the funnel: x/z relative to center; y relative to the
            // surface (downward positive => use surface - world_pos.y so the funnel
            // opens at the surface and deepens downward).
            const float ry = surface_h - world_pos.y; // metres below surface
            float sd;
            if (f.shaft) {
                // Vertical capsule from surface (y=0 at surface) to depth.
                sd = sdVerticalCapsule(dist, ry, 0.0f, f.depth, f.radius * 0.45f);
            } else {
                // Inverted capped cone: wide at the surface (top), narrow at the floor.
                // Cone center is at half-depth below the surface.
                const float h = f.depth * 0.5f;
                const float cone_y = ry - h; // shift so cone spans [0, depth]
                sd = sdCappedCone(dist, cone_y, 0.0f, h, f.radius * 0.15f, f.radius);
            }
            const float carve_here = std::max(0.0f, -sd);
            max_carve = std::max(max_carve, carve_here);
        }
    }
    out.effective_cap = min_cap;
    out.carve = max_carve;
    return out;
}

//  2D rim depression for the height paths (coarse/far visibility). A shallow
// bowl that DIPS the surface inside a doline footprint so the feature reads even on
// the SDF-ignoring coarse path. Pure 2D (no cave probe) so it is cheap to fold into
// every height consumer; returns metres to subtract from the surface height.
float SHIELD_WorldSystem::SurfaceBreakRimDepression(float world_x, float world_z) const {
    if (!m_params.surface_breaks_enabled) {
        return 0.0f;
    }
    const float cs = m_params.feature_cell_size;
    if (cs <= 0.0f) {
        return 0.0f;
    }
    const int base_cx = static_cast<int>(std::floor(world_x / cs));
    const int base_cz = static_cast<int>(std::floor(world_z / cs));
    float dip = 0.0f; // metres to lower the surface (max combine == order-free)
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const SurfaceBreakFeature f =
                DecodeSurfaceBreakCell(m_seed, base_cx + dx, base_cz + dz, m_params);
            if (!f.valid) {
                continue;
            }
            const float ddx = world_x - f.center_x;
            const float ddz = world_z - f.center_z;
            const float dist2 = ddx * ddx + ddz * ddz;
            if (dist2 >= f.radius * f.radius) {
                continue;
            }
            const float dist = std::sqrt(dist2);
            const float t = 1.0f - (dist / f.radius);
            // A smooth bowl: up to ~0.35*depth at center, 0 at rim. Kept shallower
            // than the SDF carve so the near-field 3D throat still dominates close up.
            const float bowl = 0.35f * f.depth * smoothstep01(t);
            dip = std::max(dip, bowl);
        }
    }
    return dip;
}

float SHIELD_WorldSystem::GetTerrainHeightAtCoarse(float world_x,
                                                   float world_z,
                                                   int sample_step) const {
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
    float result = h - carve_sum / 9.0f;
    //  far-field fidelity: the coarse reconstruction (pre_carve + river
    // stencil) otherwise MISSES the lake carve the near path applies after
    // pre_carve_height, so lakes would pop in at the LOD seam. Apply the lake carve
    // here too (one cheap point-sample of the smooth lake field) so lakes read
    // consistently into the distance.
    if (m_params.lakes_enabled) {
        const float lake_surface = LakeSurfaceLevel(world_x, world_z);
        result -= LakeCarveAmount(result, LakeInfluenceFromNoise(world_x, world_z), lake_surface);
    }
    //  rim depression — same single point-sample the near/grid paths apply, so
    // dolines dip the surface consistently into the far field (matches the lake
    // carve handling above).
    result -= SurfaceBreakRimDepression(world_x, world_z);
    //  far/amplified hydro seam fix: the near path adds the baked hydraulic
    // erosion offset (ComputeShapedHeightSampleImpl), so omitting it here left a
    // faint near/far drainage STEP at the live/far LOD boundary. Add the SAME
    // offset on the coarse/far path so near and far agree. SampleHydroOffsetMeters
    // is deterministic + recompute-on-load (per-region bake, cached), so run==replay
    // holds; the hitch is hidden by PrefetchHydroRegions warming the far regions
    // ahead of the bake (FarLodSystem + the far-field pass). No-op when hydro is
    // disabled (the guard skips the bake entirely, matching the near path).
    if (m_params.hydro_enabled) {
        result += SampleHydroOffsetMeters(world_x, world_z);
    }
    return result;
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

    // --- 1. Unwarped control + warp channels on the integer lattice (SIMD). ---
    std::vector<float> warp_x_grid(count);
    std::vector<float> warp_z_grid(count);
    std::vector<float> continentalness_grid(count);
    std::vector<float> erosion_grid(count);
    m_warp_generator->GenUniformGrid2D(warp_x_grid.data(),
                                       base_x,
                                       base_z,
                                       size_x,
                                       size_z,
                                       m_params.domain_warp_frequency,
                                       m_seed + 6);
    m_warp_generator->GenUniformGrid2D(warp_z_grid.data(),
                                       base_x,
                                       base_z,
                                       size_x,
                                       size_z,
                                       m_params.domain_warp_frequency,
                                       m_seed + 7);
    m_continentalness_generator->GenUniformGrid2D(continentalness_grid.data(),
                                                  base_x,
                                                  base_z,
                                                  size_x,
                                                  size_z,
                                                  m_params.continentalness_frequency,
                                                  m_seed + 3);
    m_erosion_generator->GenUniformGrid2D(erosion_grid.data(),
                                          base_x,
                                          base_z,
                                          size_x,
                                          size_z,
                                          m_params.erosion_frequency,
                                          m_seed + 4);

    // --- 2. Warp-displaced sample coordinates for the base + peaks channels. ---
    // The warped coordinate is (world + amp*warp) * freq, matching the scalar
    // helper's `sample_x * m_params.<freq>`. GenPositionArray2D samples at
    // (xPos[i] + xOffset, yPos[i] + yOffset); we fold freq into the arrays and
    // pass zero offsets.
    // SIMD-pad the GenPositionArray2D input AND
    // output arrays so the full-width tail load/store (which runs even when count
    // is not a multiple of the SIMD width -- count = size_x*size_z is rarely a
    // multiple of 16) cannot over-read/over-write past the count-sized vectors.
    // Padding is never consumed (only [0,count) is read back) -> hash-neutral.
    const std::size_t noise_pad = PadToNoiseSimd(count);
    std::vector<float> base_px(noise_pad);
    std::vector<float> base_py(noise_pad);
    std::vector<float> peaks_px(noise_pad);
    std::vector<float> peaks_py(noise_pad);
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

    std::vector<float> base_noise(noise_pad);
    std::vector<float> peaks_noise(noise_pad);
    m_terrain_generator->GenPositionArray2D(base_noise.data(),
                                            static_cast<int>(count),
                                            base_px.data(),
                                            base_py.data(),
                                            0.0f,
                                            0.0f,
                                            m_seed);
    m_peaks_generator->GenPositionArray2D(peaks_noise.data(),
                                          static_cast<int>(count),
                                          peaks_px.data(),
                                          peaks_py.data(),
                                          0.0f,
                                          0.0f,
                                          m_seed + 5);

    // --- 3. Optional island mask + river channels (cheap per-column scalar). ---
    std::vector<float> island_noise;
    if (m_params.island_mask_enabled) {
        island_noise.resize(count);
        m_island_mask_generator->GenUniformGrid2D(island_noise.data(),
                                                  base_x,
                                                  base_z,
                                                  size_x,
                                                  size_z,
                                                  m_params.island_mask_frequency,
                                                  m_seed + 2);
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
            float ridge = EvaluateShapingSpline(m_params.peaks_spline, peaks_valleys, 0.0f) *
                          m_params.peaks_amplitude * std::max(0.0f, 1.0f - erosion_01);

            //  per-biome morphology — byte-identical to ComputeShapedHeightSampleImpl.
            if (m_params.biome_relief_enabled && m_temperature_generator) {
                const float world_x = static_cast<float>(base_x + x);
                const float world_z = static_cast<float>(base_z + z);
                const float temp =
                    m_temperature_generator->GenSingle2D(world_x * m_params.temperature_frequency,
                                                         world_z * m_params.temperature_frequency,
                                                         m_seed + 8);
                const float cold01 = std::clamp(0.5f - 0.5f * temp, 0.0f, 1.0f);
                ridge *= 1.0f + m_params.biome_relief_strength * (2.0f * cold01 - 1.0f);
            }

            float terrain_height =
                m_params.height_offset + base_level +
                amplitude_multiplier * (base_noise[i] * m_params.base_amplitude) + ridge;

            //  cliffs — byte-identical to ComputeShapedHeightSampleImpl.
            terrain_height = CliffTerracedHeight(
                static_cast<float>(base_x + x), static_cast<float>(base_z + z), terrain_height);

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

            // Lake carve — byte-identical to ComputeShapedHeightSampleImpl.
            if (m_params.lakes_enabled) {
                const float world_x = static_cast<float>(base_x + x);
                const float world_z = static_cast<float>(base_z + z);
                const float lake_influence = LakeInfluenceFromNoise(world_x, world_z);
                const float lake_surface = LakeSurfaceLevel(world_x, world_z);
                terrain_height -= LakeCarveAmount(terrain_height, lake_influence, lake_surface);
            }

            //  rim depression — byte-identical to ComputeShapedHeightSampleImpl.
            terrain_height -= SurfaceBreakRimDepression(static_cast<float>(base_x + x),
                                                        static_cast<float>(base_z + z));

            out[i] = terrain_height;
        }
    }

    // hydraulic relief post-pass (decision a). Adds the same baked
    // offset the scalar path adds in ComputeShapedHeightSampleImpl, so batch and
    // scalar stay byte-identical (parity gate). Skipped when hydro is disabled
    // -> batch output byte-identical to the legacy path (world_hash unchanged).
    if (m_params.hydro_enabled) {
        for (int z = 0; z < size_z; ++z) {
            for (int x = 0; x < size_x; ++x) {
                const std::size_t i =
                    static_cast<std::size_t>(x) +
                    static_cast<std::size_t>(z) * static_cast<std::size_t>(size_x);
                out[i] += SampleHydroOffsetMeters(static_cast<float>(base_x + x),
                                                  static_cast<float>(base_z + z));
            }
        }
    }
}

SHIELD_WorldSystem::ClimateSample SHIELD_WorldSystem::ComputeClimateSample(float world_x,
                                                                           float world_z) const {
    ClimateSample climate;

    // continentalness/erosion REUSE the +3/+4 shaping noises sampled at the
    // unwarped column, and peaks/valleys reuses the +5 ridged noise at the
    // warped coords - byte-identical to how ComputeShapedHeightSample reads
    // them, so biome selection and terrain height agree on the same fields.

    const float warp_x = m_params.domain_warp_amplitude *
                         m_warp_generator->GenSingle2D(world_x * m_params.domain_warp_frequency,
                                                       world_z * m_params.domain_warp_frequency,
                                                       m_seed + 6);
    const float warp_z = m_params.domain_warp_amplitude *
                         m_warp_generator->GenSingle2D(world_x * m_params.domain_warp_frequency,
                                                       world_z * m_params.domain_warp_frequency,
                                                       m_seed + 7);
    const float sample_x = world_x + warp_x;
    const float sample_z = world_z + warp_z;

    climate.continentalness =
        m_continentalness_generator->GenSingle2D(world_x * m_params.continentalness_frequency,
                                                 world_z * m_params.continentalness_frequency,
                                                 m_seed + 3);
    climate.erosion = m_erosion_generator->GenSingle2D(
        world_x * m_params.erosion_frequency, world_z * m_params.erosion_frequency, m_seed + 4);
    climate.peaks_valleys = m_peaks_generator->GenSingle2D(
        sample_x * m_params.peaks_frequency, sample_z * m_params.peaks_frequency, m_seed + 5);

    // Temperature (+8) and humidity (+9) are new 2D climate noises, sampled at
    // the unwarped column so the climate macro-structure is stable.
    climate.temperature =
        m_temperature_generator->GenSingle2D(world_x * m_params.temperature_frequency,
                                             world_z * m_params.temperature_frequency,
                                             m_seed + 8);
    climate.humidity = m_humidity_generator->GenSingle2D(
        world_x * m_params.humidity_frequency, world_z * m_params.humidity_frequency, m_seed + 9);
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
        world_x * m_params.river_frequency, world_z * m_params.river_frequency, m_seed + 10);
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

MaterialType SHIELD_WorldSystem::SurfaceMaterialForColumn(float world_y,
                                                          float final_height,
                                                          u8 biome_id,
                                                          bool river_bank) const {
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
        // above-water river bank skin uses the filler (muddy bank)
        // rather than the top (grass).
        return static_cast<MaterialType>(river_bank ? palette.filler : palette.top);
    }
    if (depth < 5.0f) {
        return static_cast<MaterialType>(palette.filler);
    }
    return static_cast<MaterialType>(palette.depth);
}

MaterialType SHIELD_WorldSystem::SurfaceVertexMaterial(float world_x,
                                                       float world_z,
                                                       float terrain_height) const {
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
    //      SurfaceMaterialForColumn(y=terrain_height-0.1, final_height,...).
    // Reproducing those two SurfaceMaterialForColumn calls here skips the
    // redundant shaped-height recompute inside SampleWorldGenLayers.
    const u8 biome_id = BiomeIdAt(world_x, world_z);
    const bool river_bank = RiverInfluenceFromNoise(world_x, world_z) > 0.25f;
    const MaterialType solid_material =
        SurfaceMaterialForColumn(terrain_height - 0.35f, terrain_height, biome_id, river_bank);
    if (solid_material != MaterialType::Air && solid_material != MaterialType::Water) {
        return solid_material;
    }
    return SurfaceMaterialForColumn(terrain_height - 0.1f, terrain_height, biome_id, river_bank);
}

void SHIELD_WorldSystem::ComputeShapedHeightsAtPositions(
    const float* xs, const float* zs, std::size_t count, float* out, bool apply_hydro) const {
    if (count == 0) {
        return;
    }

    // Warp channels (unwarped lattice), then base/peaks at warped coords, then
    // continentalness/erosion at unwarped coords - all via GenPositionArray2D.
    // every array handed to GenPositionArray2D is
    // SIMD-padded (PadToNoiseSimd) so the entry point's full-width tail load/store
    // stays in mapped memory even when count < SIMD width. Loops still touch only
    // [0,count); padding lanes are never read into results (hash-neutral).
    const std::size_t pad = PadToNoiseSimd(count);
    std::vector<float> wx_in(pad), wz_in(pad);
    for (std::size_t i = 0; i < count; ++i) {
        wx_in[i] = xs[i] * m_params.domain_warp_frequency;
        wz_in[i] = zs[i] * m_params.domain_warp_frequency;
    }
    std::vector<float> warp_x(pad), warp_z(pad);
    m_warp_generator->GenPositionArray2D(
        warp_x.data(), static_cast<int>(count), wx_in.data(), wz_in.data(), 0.0f, 0.0f, m_seed + 6);
    m_warp_generator->GenPositionArray2D(
        warp_z.data(), static_cast<int>(count), wx_in.data(), wz_in.data(), 0.0f, 0.0f, m_seed + 7);

    std::vector<float> cont_x(pad), cont_y(pad), eros_x(pad), eros_y(pad);
    std::vector<float> base_x(pad), base_y(pad), peaks_x(pad), peaks_y(pad);
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
    std::vector<float> continentalness(pad), erosion(pad), base_noise(pad), peaks_noise(pad);
    m_continentalness_generator->GenPositionArray2D(continentalness.data(),
                                                    static_cast<int>(count),
                                                    cont_x.data(),
                                                    cont_y.data(),
                                                    0.0f,
                                                    0.0f,
                                                    m_seed + 3);
    m_erosion_generator->GenPositionArray2D(erosion.data(),
                                            static_cast<int>(count),
                                            eros_x.data(),
                                            eros_y.data(),
                                            0.0f,
                                            0.0f,
                                            m_seed + 4);
    m_terrain_generator->GenPositionArray2D(base_noise.data(),
                                            static_cast<int>(count),
                                            base_x.data(),
                                            base_y.data(),
                                            0.0f,
                                            0.0f,
                                            m_seed);
    m_peaks_generator->GenPositionArray2D(peaks_noise.data(),
                                          static_cast<int>(count),
                                          peaks_x.data(),
                                          peaks_y.data(),
                                          0.0f,
                                          0.0f,
                                          m_seed + 5);

    std::vector<float> island_noise;
    for (std::size_t i = 0; i < count; ++i) {
        const float base_level =
            EvaluateShapingSpline(m_params.continental_spline, continentalness[i], 0.0f);
        const float amplitude_multiplier =
            EvaluateShapingSpline(m_params.erosion_spline, erosion[i], 1.0f);
        const float erosion_01 = std::clamp((erosion[i] + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float ridge = EvaluateShapingSpline(m_params.peaks_spline, peaks_noise[i], 0.0f) *
                            m_params.peaks_amplitude * std::max(0.0f, 1.0f - erosion_01);
        float h = m_params.height_offset + base_level +
                  amplitude_multiplier * (base_noise[i] * m_params.base_amplitude) + ridge;
        if (m_params.island_mask_enabled) {
            const float in =
                m_island_mask_generator->GenSingle2D(xs[i] * m_params.island_mask_frequency,
                                                     zs[i] * m_params.island_mask_frequency,
                                                     m_seed + 2);
            const float mask = glm::smoothstep(0.1f, 0.25f, in);
            h = glm::mix(m_params.height_offset, h, mask);
        }
        if (m_params.rivers_enabled) {
            const float influence = RiverInfluenceFromNoise(xs[i], zs[i]);
            h -= RiverCarveAmount(h, influence);
        }
        // add the baked hydro offset so this matches GetTerrainHeightAt
        // (apply_hydro=false on the erosion bake's own base samples -> no recursion).
        if (apply_hydro && m_params.hydro_enabled) {
            h += SampleHydroOffsetMeters(xs[i], zs[i]);
        }
        out[i] = h;
    }
}

void SHIELD_WorldSystem::ClassifyVertexMaterials(const Vec3* positions,
                                                 std::size_t count,
                                                 u32* out_materials) const {
    if (count == 0) {
        return;
    }

    // --- Batched shaped height + climate for every vertex (x,z). ---
    // GenPositionArray2D samples at (xPos[i] + xOffset, yPos[i] + yOffset); we
    // fold the per-channel frequency into the coordinate arrays (zero offsets),
    // mirroring the scalar helpers' `coord * frequency`.
    // SIMD-pad every GenPositionArray2D buffer (see
    // PadToNoiseSimd) so the full-width tail load/store cannot over-read past the
    // count-sized vectors. Hash-neutral: only indices [0,count) are consumed.
    const std::size_t pad = PadToNoiseSimd(count);
    std::vector<float> warp_xf(pad), warp_zf(pad); // warp coords (unwarped * warp_freq)
    for (std::size_t i = 0; i < count; ++i) {
        warp_xf[i] = positions[i].x * m_params.domain_warp_frequency;
        warp_zf[i] = positions[i].z * m_params.domain_warp_frequency;
    }
    std::vector<float> warp_x(pad), warp_z(pad);
    m_warp_generator->GenPositionArray2D(warp_x.data(),
                                         static_cast<int>(count),
                                         warp_xf.data(),
                                         warp_zf.data(),
                                         0.0f,
                                         0.0f,
                                         m_seed + 6);
    m_warp_generator->GenPositionArray2D(warp_z.data(),
                                         static_cast<int>(count),
                                         warp_xf.data(),
                                         warp_zf.data(),
                                         0.0f,
                                         0.0f,
                                         m_seed + 7);

    // continentalness/erosion read the UNWARPED column; base/peaks read the
    // warp-displaced column (matching ComputeShapedHeightSample).
    std::vector<float> cont_x(pad), cont_y(pad), eros_x(pad), eros_y(pad);
    std::vector<float> base_x(pad), base_y(pad), peaks_x(pad), peaks_y(pad);
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
    std::vector<float> continentalness(pad), erosion(pad), base_noise(pad), peaks_noise(pad);
    m_continentalness_generator->GenPositionArray2D(continentalness.data(),
                                                    static_cast<int>(count),
                                                    cont_x.data(),
                                                    cont_y.data(),
                                                    0.0f,
                                                    0.0f,
                                                    m_seed + 3);
    m_erosion_generator->GenPositionArray2D(erosion.data(),
                                            static_cast<int>(count),
                                            eros_x.data(),
                                            eros_y.data(),
                                            0.0f,
                                            0.0f,
                                            m_seed + 4);
    m_terrain_generator->GenPositionArray2D(base_noise.data(),
                                            static_cast<int>(count),
                                            base_x.data(),
                                            base_y.data(),
                                            0.0f,
                                            0.0f,
                                            m_seed);
    m_peaks_generator->GenPositionArray2D(peaks_noise.data(),
                                          static_cast<int>(count),
                                          peaks_x.data(),
                                          peaks_y.data(),
                                          0.0f,
                                          0.0f,
                                          m_seed + 5);

    // Climate (temperature +8, humidity +9) for the biome lookup, when biomes
    // are enabled. Sampled at the unwarped column.
    std::vector<float> temperature, humidity;
    if (m_biomes_enabled && !m_biome_table.empty()) {
        temperature.resize(pad);
        humidity.resize(pad);
        std::vector<float> temp_x(pad), temp_y(pad), hum_x(pad), hum_y(pad);
        for (std::size_t i = 0; i < count; ++i) {
            temp_x[i] = positions[i].x * m_params.temperature_frequency;
            temp_y[i] = positions[i].z * m_params.temperature_frequency;
            hum_x[i] = positions[i].x * m_params.humidity_frequency;
            hum_y[i] = positions[i].z * m_params.humidity_frequency;
        }
        m_temperature_generator->GenPositionArray2D(temperature.data(),
                                                    static_cast<int>(count),
                                                    temp_x.data(),
                                                    temp_y.data(),
                                                    0.0f,
                                                    0.0f,
                                                    m_seed + 8);
        m_humidity_generator->GenPositionArray2D(humidity.data(),
                                                 static_cast<int>(count),
                                                 hum_x.data(),
                                                 hum_y.data(),
                                                 0.0f,
                                                 0.0f,
                                                 m_seed + 9);
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
        const float ridge = EvaluateShapingSpline(m_params.peaks_spline, peaks_noise[i], 0.0f) *
                            m_params.peaks_amplitude * std::max(0.0f, 1.0f - erosion_01);
        float final_height = m_params.height_offset + base_level +
                             amplitude_multiplier * (base_noise[i] * m_params.base_amplitude) +
                             ridge;
        if (m_params.island_mask_enabled) {
            const float island_noise =
                m_island_mask_generator->GenSingle2D(p.x * m_params.island_mask_frequency,
                                                     p.z * m_params.island_mask_frequency,
                                                     m_seed + 2);
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
            biome_id = m_biome_table.lookup(
                continentalness[i], erosion[i], peaks_noise[i], temperature[i], humidity[i]);
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
            const Vec3 wp(p.x, world_y, p.z);
            const SurfaceBreakSample sb = sample_surface_breaks(wp, final_height);
            // single composition point (cheese + noise-router spaghetti tunnels).
            final_density = EvaluateCaveDensity(wp, terrain_density, sb.effective_cap, sb.carve);
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

SHIELD_WorldSystem::ColumnSurfaceSpan
SHIELD_WorldSystem::compute_column_surface_span(int chunk_x, int chunk_z) const {
    const float base_x = static_cast<float>(chunk_x * CHUNK_SIZE_X);
    const float base_z = static_cast<float>(chunk_z * CHUNK_SIZE_Z);
    const float max_x = base_x + static_cast<float>(CHUNK_SIZE_X);
    const float max_z = base_z + static_cast<float>(CHUNK_SIZE_Z);
    const float center_x = base_x + CHUNK_SIZE_X * 0.5f;
    const float center_z = base_z + CHUNK_SIZE_Z * 0.5f;

    // batch the 5 footprint height samples (center + 4
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
    // cache the surface biome id for the column (kNoBiome when biomes
    // are disabled). Sampled at the column center, matching center_y.
    span.biome_id = BiomeIdAt(center_x, center_z);
    return span;
}

SHIELD_WorldSystem::ColumnSurfaceSpan SHIELD_WorldSystem::column_surface_span(int chunk_x,
                                                                              int chunk_z) {
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

int SHIELD_WorldSystem::get_required_lod_for_chunk(const IVec3& coords,
                                                   const Vec3& chunk_center,
                                                   const Vec3& camera_position,
                                                   int current_lod) {
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
    // The band covers the full column surface SPAN plus the same
    // +-1 margin as before, so an entire cliff face keeps a single LOD per
    // column (preserving the vertical-seam invariant above). On flat terrain
    // span.min_y == span.max_y == center and this is exactly the old band.
    constexpr int kSurfaceLodBandChunks = 1;
    const ColumnSurfaceSpan span = column_surface_span(coords.x, coords.z);
    float dist;
    if (coords.y >= span.min_y - kSurfaceLodBandChunks &&
        coords.y <= span.max_y + kSurfaceLodBandChunks) {
        dist = glm::distance(Vec3(camera_position.x, 0.0f, camera_position.z),
                             Vec3(chunk_center.x, 0.0f, chunk_center.z));
    } else {
        dist = glm::distance(camera_position, chunk_center);
    }

    const int band_lod = get_lod_level_for_distance(dist);

    // Asymmetric hysteresis: a chunk PROMOTES to a finer LOD the
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
        const float hold_distance =
            m_lod_levels[static_cast<std::size_t>(current_lod)].distance + kDemoteHysteresisMeters;
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
            // Full column surface span plus the same +-1 margin the
            // fixed {0, -1, 1} offsets provided on flat terrain, so steep
            // spawn neighborhoods preload their cliff-wall chunks too.
            const ColumnSurfaceSpan span = compute_column_surface_span(chunk_x, chunk_z);

            for (int y = span.min_y - 1; y <= span.max_y + 1; ++y) {
                candidates.push_back({IVec3(chunk_x, y, chunk_z),
                                      std::abs(y - span.center_y),
                                      horizontal_ring_distance(dx, dz),
                                      horizontal_distance_sq(dx, dz)});
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
    }

    std::sort(candidates.begin(),
              candidates.end(),
              [](const InitialChunkCandidate& a, const InitialChunkCandidate& b) {
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
                         initial_chunks.size(),
                         center_pos.y,
                         spawn_terrain_height,
                         min_y * CHUNK_SIZE_Y,
                         max_y * CHUNK_SIZE_Y);

    return initial_chunks;
}

void SHIELD_WorldSystem::update(entt::registry& registry,
                                const Vec3& camera_position,
                                PhysicsSystem* physics_system) {
    // Single-anchor convenience overload — forwards to the multi-anchor path with one
    // anchor (byte-identical streaming/world_hash to the historical single-anchor code).
    update(registry, std::vector<Vec3>{camera_position}, physics_system);
}

void SHIELD_WorldSystem::update(entt::registry& registry,
                                const std::vector<Vec3>& anchor_positions,
                                PhysicsSystem* physics_system) {
    // runtime telemetry ( implementation note): split the 300ms+ streaming spike by sub-phase.
    m_dbg_stream = {};
    auto _dbg_prev = std::chrono::steady_clock::now();
    auto _dbg_split = [&](double& slot) {
        const auto _n = std::chrono::steady_clock::now();
        slot += std::chrono::duration<double, std::milli>(_n - _dbg_prev).count();
        _dbg_prev = _n;
    };
    //  5b: the per-frame publish hooks run in the NON-FORCE regime —
    // on the server (tick source present) a batch publishes only at its due
    // tick, so the per-tick schedule belongs exclusively to activate_due; on
    // the client (due -1) these publish when drained, exactly as before.
    publish_completed_generation_jobs(/*force=*/false);
    process_completed_meshing_jobs(/*force=*/false);
    process_completed_promotion_jobs(/*force=*/false);
    _dbg_split(m_dbg_stream.process_completed);

    // Hydro prefetch: warm the erosion-region cache around each anchor AHEAD of
    // chunk-gen on background jobs, so re-enabled hydro never bakes on the gen/main
    // critical path (the fix for the laggy-while-flying with hydro on). No-op when
    // hydro is disabled. 768 m covers the live ring (~512 m) plus a region of lead.
    for (const Vec3& anchor : anchor_positions) {
        PrefetchHydroRegions(anchor.x, anchor.z, 768.0f);
    }

    m_last_streaming_budget_stats = {};
    m_last_streaming_budget_stats.update_interval_frames = STREAMING_ACTIVATION_INTERVAL_FRAMES;
    m_last_streaming_budget_stats.requested_render_radius = RENDER_DISTANCE;
    m_last_streaming_budget_stats.max_active_chunks_budget = STREAMING_MAX_ACTIVE_CHUNKS_BUDGET;
    // telemetry mirrors the publication-keyed signals the
    // scheduler now reads (not the wall-clock job counters).
    m_last_streaming_budget_stats.generation_job_active = generation_batch_outstanding();
    m_last_streaming_budget_stats.meshing_job_active = meshing_batch_outstanding();
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
    _dbg_split(
        m_dbg_stream.telemetry); // runtime telemetry: hydro prefetch + telemetry count loop 1

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
    bool activation_ran_this_tick = false;
    m_update_tick_counter++;
    if (m_update_tick_counter >= STREAMING_ACTIVATION_INTERVAL_FRAMES) {
        // activation may elide itself (return false) when the residency set is provably unchanged;
        // only count it as "ran" when it actually did work, so the meshing gate stays accurate.
        activation_ran_this_tick = update_chunk_activation(anchor_positions, physics_system);
        m_update_tick_counter = 0;
    }
    _dbg_split(m_dbg_stream.activation); // runtime telemetry

    // Step 2: Update the water system using the now-current list of active chunks.
    // This MUST happen before meshing jobs are dispatched.
    if (m_water_system) {
        m_water_system->update(registry, m_streaming_state.chunks);
    }
    _dbg_split(m_dbg_stream.water); // runtime telemetry

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

    //  streaming elision GATE. Run the Step-2/3 meshing-candidate pass only when something
    // the pass cares about changed since it last fully drained. RUN signals are deterministic +
    // main-thread-observed (NO job-completion timing — the attempt-#1 trap): a dirty-generation
    // delta (a chunk insert/erase / synchronous rebuild bumped it), an EXACT anchor-vector change
    // (LOD keys on continuous distance, so any sub-chunk anchor motion can flip a band — no
    // threshold, no quantization), a chunk-count delta, an activation tick, or a previous pass that
    // did not reach quiescence. On a settled static pose all are false, so the O(N) meshed_columns
    // build + candidate scan + sort + dispatch are skipped entirely. Step 4 (collision) and the
    // telemetry scans run unchanged every tick.
    const bool anchor_changed = (anchor_positions != m_last_anchor_positions);
    const bool count_changed = (m_streaming_state.chunks.size() != m_last_chunk_count);
    const bool generation_dirty = (m_dirty_generation != m_last_serviced_generation);
    const bool streaming_dirty = generation_dirty || anchor_changed || count_changed ||
                                 activation_ran_this_tick || !m_last_pass_drained;
    m_last_anchor_positions = anchor_positions;
    m_last_chunk_count = m_streaming_state.chunks.size();
    // Hoisted out of the gated block: the queue-depth telemetry below (after the gate) reads it.
    // Recomputed FRESH inside every non-elided pass (never a stale cross-frame dispatch input).
    std::size_t terrain_meshing_backlog = 0;
    if (streaming_dirty) {
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
            auto require_face_if_neighbor_is_finer =
                [&](int dx, int dz, Luminumbra::World::MarchingCubes::TerrainTransitionFace face) {
                    const auto column_it =
                        meshed_columns.find(horizontal_chunk_key(coords.x + dx, coords.z + dz));
                    if (column_it == meshed_columns.end()) {
                        return;
                    }
                    for (const MeshedColumnEntry& entry : column_it->second) {
                        if (entry.lod < lod_level ||
                            (entry.lod != lod_level && entry.y != coords.y)) {
                            required |= static_cast<
                                Luminumbra::World::MarchingCubes::TerrainTransitionFaceMask>(face);
                            return;
                        }
                    }
                };
            require_face_if_neighbor_is_finer(
                -1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMinX);
            require_face_if_neighbor_is_finer(
                1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMaxX);
            require_face_if_neighbor_is_finer(
                0, -1, Luminumbra::World::MarchingCubes::TransitionFaceMinZ);
            require_face_if_neighbor_is_finer(
                0, 1, Luminumbra::World::MarchingCubes::TransitionFaceMaxZ);
            return static_cast<u8>(required & static_cast<u8>(~chunk.applied_transition_faces.load(
                                                  std::memory_order_acquire)));
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

            // Closest anchor to this chunk (multi-anchor LOD: the finest detail any anchor
            // demands wins). One anchor -> that anchor, identical to the historical
            // camera_position.
            Vec3 closest_anchor = anchor_positions.empty() ? Vec3(0.0f) : anchor_positions[0];
            if (anchor_positions.size() > 1) {
                const IVec3 lod_chunk = chunk_ptr->get_coords();
                float best = 1e30f;
                for (const Vec3& a : anchor_positions) {
                    const IVec3 d = lod_chunk - world_to_chunk_coords(a);
                    const float ds =
                        static_cast<float>(horizontal_distance_sq(d.x, d.z) + d.y * d.y);
                    if (ds < best) {
                        best = ds;
                        closest_anchor = a;
                    }
                }
            }

            if (state == Luminumbra::ChunkState::Idle) {
                needs_meshing = true;
            } else if (state == Luminumbra::ChunkState::Ready) {
                Vec3 chunk_center = (Vec3(chunk_ptr->get_coords()) + 0.5f) *
                                    Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
                // pass the meshed LOD so demotions go through the
                // asymmetric hysteresis band (promote at D, demote at D + margin).
                required_lod = get_required_lod_for_chunk(chunk_ptr->get_coords(),
                                                          chunk_center,
                                                          closest_anchor,
                                                          chunk_ptr->current_lod.load());

                if (required_lod != chunk_ptr->current_lod.load()) {
                    needs_meshing = true;
                    terrain_mesh_required = true;
                } else if (required_lod == 0 &&
                           chunk_ptr->has_water_sim.load(std::memory_order_acquire) &&
                           !chunk_ptr->water_mesh_generated.load(std::memory_order_acquire)) {
                    needs_meshing = true;
                    terrain_mesh_required = false;
                } else if (get_lod_step_for_level(required_lod) > 1 &&
                           !chunk_ptr->mesh_vertices.empty() && !chunk_ptr->mesh_indices.empty() &&
                           missing_transition_faces(*chunk_ptr, required_lod) !=
                               Luminumbra::World::MarchingCubes::kNoTransitionFaces) {
                    // A finer neighbor arrived after this coarse chunk was meshed:
                    // remesh so the now-required boundary transition skirts are
                    // baked in, closing the persistent LOD seam crack.
                    needs_meshing = true;
                    terrain_mesh_required = true;
                }
            }

            if (needs_meshing) {
                const Vec3 chunk_center = (Vec3(chunk_ptr->get_coords()) + 0.5f) *
                                          Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
                if (required_lod == -1) {
                    // Never-meshed chunks carry current_lod == -1, so this stays
                    // the raw band assignment; a previously meshed chunk that
                    // re-enters here keeps the same hysteresis as the Ready path.
                    required_lod = get_required_lod_for_chunk(chunk_ptr->get_coords(),
                                                              chunk_center,
                                                              closest_anchor,
                                                              chunk_ptr->current_lod.load());
                }
                // The column-surface cache samples the same chunk positions, so
                // this replaces per-candidate fractal noise evaluations with a
                // hash lookup (terrain height is a pure function of seed/params).
                // Vertical rank is the distance OUTSIDE the column surface span
                // (0 for every chunk the isosurface passes through), so cliff
                // wall chunks drain with the same hole-fill priority as the
                // center surface chunk.
                const ColumnSurfaceSpan span =
                    column_surface_span(chunk_ptr->get_coords().x, chunk_ptr->get_coords().z);
                const int chunk_y = chunk_ptr->get_coords().y;
                const int vertical_surface_distance =
                    chunk_y > span.max_y ? chunk_y - span.max_y
                                         : (chunk_y < span.min_y ? span.min_y - chunk_y : 0);
                // Meshing priority by distance to the CLOSEST anchor (multi-anchor). One
                // anchor -> identical to the historical camera-relative distance.
                const IVec3 mc_coords = chunk_ptr->get_coords();
                float distance_sq = 1e30f;
                for (const Vec3& a : anchor_positions) {
                    const IVec3 delta = mc_coords - world_to_chunk_coords(a);
                    distance_sq =
                        std::min(distance_sq,
                                 static_cast<float>(horizontal_distance_sq(delta.x, delta.z) +
                                                    delta.y * delta.y));
                }
                meshing_candidates.push_back({chunk_ptr,
                                              required_lod,
                                              state == Luminumbra::ChunkState::Ready &&
                                                  !chunk_ptr->mesh_vertices.empty() &&
                                                  !chunk_ptr->mesh_indices.empty(),
                                              terrain_mesh_required,
                                              vertical_surface_distance,
                                              distance_sq});
            }
        }

        m_last_streaming_budget_stats.meshing_candidates = meshing_candidates.size();
        terrain_meshing_backlog = 0;
        for (const MeshingCandidate& candidate : meshing_candidates) {
            if (candidate.terrain_mesh_required) {
                ++terrain_meshing_backlog;
            }
        }
        //  5b: the meshing dispatch gate is the FIFO DEPTH BUDGET —
        // deterministic main-thread state, never job timing. Promotion
        // backpressure decoupled from meshing (its own pipeline-pending guard in
        // dispatch_meshing_jobs routes promotions; a promotion pipeline in flight
        // must not stall meshing for its whole due window).
        const bool meshing_job_active =
            m_streaming_state.meshing_batches.size() >= kMaxMeshingBatchesInFlight;
        m_last_streaming_budget_stats.meshing_job_active = meshing_job_active;
        // Scale the per-dispatch meshing batch with the standing terrain backlog:
        // deep backlogs (initial load, fast travel) dispatch larger batches so
        // more of the backlog is in flight per dispatch, while shallow
        // steady-state backlogs keep the small batches that preserve LOD/hole-fill
        // responsiveness. The batch is still the sorted-candidate prefix, so the
        // hole-fill-first ordering and per-chunk LOD selection are unchanged -
        // only how quickly the same work drains. Measured on the 20s
        // EnduranceStreamDrain scenario: max_deferred_age_frames 28 -> 12 and
        // cumulative_deferred_meshing ~15k -> ~5k versus a fixed budget.
        // residency push (owner: "parts not loaded" must resolve fast + "up the caps"
        // for the RTX 5070 Ti target): the deep-backlog cap is raised from 2x to 4x
        // and the base budget bumped, so initial-load / fast-travel backlogs drain in
        // far fewer frames (meshing runs on JobSystem workers; the main thread is
        // still bounded by the per-frame upload cap, so worst-case main-thread cost is
        // governed by uploads, not this dispatch batch size). Steady-state shallow
        // backlogs keep the base budget for LOD/hole-fill responsiveness.
        int meshing_budget = MAX_CHUNKS_TO_PROCESS_PER_FRAME;
        if (terrain_meshing_backlog > static_cast<std::size_t>(MAX_CHUNKS_TO_PROCESS_PER_FRAME)) {
            meshing_budget = static_cast<int>(std::min<std::size_t>(
                static_cast<std::size_t>(MAX_CHUNKS_TO_PROCESS_PER_FRAME) * 4u,
                terrain_meshing_backlog / 2u));
            meshing_budget = std::max(meshing_budget, MAX_CHUNKS_TO_PROCESS_PER_FRAME);
        }
        m_last_streaming_budget_stats.meshing_budget = meshing_job_active ? 0 : meshing_budget;

        if (!meshing_candidates.empty() && !meshing_job_active) {
            std::sort(meshing_candidates.begin(),
                      meshing_candidates.end(),
                      [](const MeshingCandidate& a, const MeshingCandidate& b) {
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

            const std::size_t budget =
                std::min(meshing_candidates.size(),
                         static_cast<std::size_t>(m_last_streaming_budget_stats.meshing_budget));
            for (std::size_t i = 0; i < budget; ++i) {
                // The sort places hole-fill candidates (no active mesh) first,
                // nearest surface band first, so the capped High prefix is
                // exactly the near-field holes.
                const bool high_priority = !meshing_candidates[i].has_active_mesh &&
                                           i < MAX_HIGH_PRIORITY_MESHING_JOBS_PER_DISPATCH;
                chunks_to_mesh_jobs.push_back({meshing_candidates[i].chunk,
                                               meshing_candidates[i].required_lod,
                                               meshing_candidates[i].terrain_mesh_required,
                                               high_priority});
            }

            m_last_streaming_budget_stats.scheduled_meshing = chunks_to_mesh_jobs.size();
            m_last_streaming_budget_stats.deferred_meshing =
                meshing_candidates.size() - chunks_to_mesh_jobs.size();
        }

        if (!chunks_to_mesh_jobs.empty() && !meshing_job_active) {
            dispatch_meshing_jobs(chunks_to_mesh_jobs);
        }

        // Post-pass QUIESCENCE test (decides whether a FUTURE tick may elide — NOT whether THIS
        // pass ran). The world is settled iff this pass produced no candidates, left nothing
        // deferred, and no gen/mesh jobs are in flight. Job-active state is read ONLY here; on the
        // per-tick-quiesced hashed paths it is deterministic. Advancing m_last_serviced_generation
        // only at quiescence means a budget-deferred remesh (deferred_meshing>0) or any produced
        // candidate keeps the gate sticky-open next tick, so chained seam/transition propagation
        // always converges.
        const bool produced_work =
            !meshing_candidates.empty() || m_last_streaming_budget_stats.deferred_meshing > 0;
        // quiescence keys on the publication-keyed outstanding
        // signals (main-thread events), not wall-clock job counters — the last
        // scheduler read to be de-timed ahead of the barrier removal.
        const bool quiescent = !produced_work && !meshing_batch_outstanding() &&
                               !promotion_pipeline_pending() && !generation_batch_outstanding();
        m_last_pass_drained = quiescent;
        if (quiescent) {
            m_last_serviced_generation = m_dirty_generation;
        }
    } // streaming_dirty gate
    _dbg_split(
        m_dbg_stream.meshing_pass); // runtime telemetry: Step 3 meshing-candidate pass + dispatch

    // Step 4. Time-slice the creation of expensive physics colliders on the main thread.
    //  the eligibility scan is O(N) over every streamed chunk. Gate it on
    // m_collision_pass_dirty so a SETTLED world skips it entirely. The flag is set at the two
    // has_collision=false reset sites (remesh/LOD0 promotion + synchronous rebuild) and cleared
    // here once a scan drains all eligible chunks without hitting the per-frame cap. The scan body
    // is unchanged, so the set of colliders created (and has_collision, which feeds world_hash) is
    // identical to before — only redundant settled-tick walks are removed.
    if (physics_system && m_collision_pass_dirty) {
        int collision_meshes_created_this_frame = 0;
        bool hit_frame_cap = false;
        for (auto const& [id, chunk_ptr] : m_streaming_state.chunks) {
            if (chunk_ptr->get_state() == ChunkState::Ready && !chunk_ptr->has_collision.load()) {
                // Only create collision once the chunk is sim-available at
                // LOD0 ( : predicate centralized — collision is
                // built from the HEIGHTMAP; sim_available_lod0 is the single
                // place the availability definition lives).
                if (sim_available_lod0(*chunk_ptr)) {
                    replace_chunk_collision(*physics_system, *chunk_ptr);

                    collision_meshes_created_this_frame++;
                    if (collision_meshes_created_this_frame >= MAX_COLLISION_MESHES_PER_FRAME) {
                        hit_frame_cap = true;
                        break;
                    }
                }
            }
        }
        // Hitting the cap means eligible chunks may remain past the break, so stay dirty for the
        // next tick. Otherwise the loop visited every chunk and created colliders for all eligible
        // ones, so there is no collision work left until something resets has_collision again.
        m_collision_pass_dirty = hit_frame_cap;
    }
    _dbg_split(m_dbg_stream.collision); // runtime telemetry

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

    const std::size_t queue_depth =
        m_last_streaming_budget_stats.loading_chunks + terrain_meshing_backlog;
    ++m_streaming_telemetry_stats.frames_observed;
    m_streaming_telemetry_stats.last_queue_depth = queue_depth;
    m_streaming_telemetry_stats.peak_queue_depth =
        std::max(m_streaming_telemetry_stats.peak_queue_depth, queue_depth);
    //  record this frame's depth into the trailing ring
    // and derive the SETTLED floor (min over the last activation window). Chunk
    // activation runs only every STREAMING_ACTIVATION_INTERVAL_FRAMES frames, so
    // generation/loading arrives in periodic batches; the raw last_queue_depth
    // catches whichever phase of that cycle the snapshot frame falls in. The
    // window minimum is 0 iff the pipeline reaches empty within each cycle
    // (bounded + fully draining) and stays nonzero only for a standing backlog
    // that never empties (genuinely unbounded). Telemetry only; not hashed.
    m_recent_queue_depths[m_recent_queue_depth_cursor] = queue_depth;
    m_recent_queue_depth_cursor = (m_recent_queue_depth_cursor + 1u) % m_recent_queue_depths.size();
    if (m_recent_queue_depth_count < m_recent_queue_depths.size()) {
        ++m_recent_queue_depth_count;
    }
    std::size_t settled_queue_depth = queue_depth;
    for (std::size_t i = 0; i < m_recent_queue_depth_count; ++i) {
        settled_queue_depth = std::min(settled_queue_depth, m_recent_queue_depths[i]);
    }
    m_streaming_telemetry_stats.settled_queue_depth = settled_queue_depth;
    m_streaming_telemetry_stats.peak_meshing_candidates =
        std::max(m_streaming_telemetry_stats.peak_meshing_candidates,
                 m_last_streaming_budget_stats.meshing_candidates);
    m_streaming_telemetry_stats.cumulative_scheduled_meshing +=
        m_last_streaming_budget_stats.scheduled_meshing;
    m_streaming_telemetry_stats.cumulative_deferred_meshing +=
        m_last_streaming_budget_stats.deferred_meshing;
    if (m_last_streaming_budget_stats.meshing_candidates >
        m_last_streaming_budget_stats.scheduled_meshing) {
        ++m_deferred_backlog_age_frames;
    } else {
        m_deferred_backlog_age_frames = 0;
    }
    m_streaming_telemetry_stats.max_deferred_age_frames = std::max(
        m_streaming_telemetry_stats.max_deferred_age_frames, m_deferred_backlog_age_frames);
}

bool SHIELD_WorldSystem::update_chunk_activation(const std::vector<Vec3>& anchors,
                                                 PhysicsSystem* physics_system) {
    if (anchors.empty()) {
        return false; // no anchors -> nothing to stream around (caller guarantees >= 1 in practice)
    }
    // Per-anchor chunk coordinates (the wanted-set is the UNION of each anchor's disc;
    // eviction below keeps a chunk if it is in range of ANY anchor). One anchor ->
    // identical to the historical single-anchor path.
    std::vector<IVec3> camera_chunks;
    camera_chunks.reserve(anchors.size());
    for (const Vec3& a : anchors) {
        camera_chunks.push_back(world_to_chunk_coords(a));
    }

    //  implementation note (streaming residual): ELIDE the whole pass when the residency set is
    // provably unchanged. The wanted disc is chunk-granular and keyed only on the anchor CHUNK
    // coords, so sub-chunk camera motion never changes it. If the anchor chunks are identical to
    // last activation, no world mutation happened (dirty_generation), and the last activation
    // created NOTHING (scheduled+deferred == 0 -> every wanted chunk was already resident), then
    // re-running would enumerate the same disc (all already resident -> create 0) and evict 0
    // (anchor static). So the pass is a pure no-op and we skip its O(radius^2) enumeration + O(N)
    // eviction scan. Signals are deterministic main-thread state (NEVER job-activity timing), so
    // residency — and world_hash — are byte-identical to running it. This is the dominant per-tick
    // CPU cost while stationary at high render distance.
    if (m_activation_has_run && m_dirty_generation == m_last_activation_dirty_generation &&
        m_last_activation_pending == 0 && camera_chunks == m_last_activation_camera_chunks) {
        return false;
    }
    struct GenerationCandidate {
        IVec3 coords;
        bool surface = false;
        int ring_distance = 0;
        int horizontal_distance_sq = 0;
        int vertical_rank = 0;
        int target_step = 1;
    };

    int target_radius = streaming_radius_for_pressure(
        m_streaming_state.chunks.size(),
        m_last_streaming_budget_stats.loading_chunks,
        m_last_streaming_budget_stats.idle_chunks,
        // publication-keyed signals — the wanted radius is
        // now a pure function of main-thread events, never job-counter timing.
        generation_batch_outstanding(),
        // promotion work counts as meshing-lane pressure (it was
        // meshing-lane work before the decoupling). Byte-neutral on the
        // per-tick-quiesced server paths — both terms read false there.
        meshing_batch_outstanding() || promotion_pipeline_pending(),
        anchors.size());
    if (m_streaming_radius_cap > 0) {
        // Debug/test cap (see debug_set_streaming_radius_cap): still a pure
        // function of main-thread state, so capped runs stay deterministic.
        target_radius = std::min(target_radius, m_streaming_radius_cap);
    }

    m_last_streaming_budget_stats.target_render_radius = target_radius;
    m_last_streaming_budget_stats.generation_job_active = generation_batch_outstanding();
    int generation_budget = MAX_CHUNKS_TO_PROCESS_PER_FRAME;
    if (anchors.size() > 1u) {
        generation_budget = static_cast<int>(
            std::min<std::size_t>(static_cast<std::size_t>(MAX_CHUNKS_TO_PROCESS_PER_FRAME) * 2u,
                                  static_cast<std::size_t>(MAX_CHUNKS_TO_PROCESS_PER_FRAME) +
                                      static_cast<std::size_t>(MAX_CHUNKS_TO_PROCESS_PER_FRAME) *
                                          (anchors.size() - 1u) / 8u));
    }
    //  5b: generation dispatch backpressure = FIFO depth budget
    // (deterministic; the telemetry field above keeps outstanding semantics).
    m_last_streaming_budget_stats.generation_budget =
        m_streaming_state.generation_batches.size() >= kMaxGenerationBatchesInFlight
            ? 0
            : generation_budget;

    std::vector<GenerationCandidate> to_create;
    to_create.reserve(static_cast<std::size_t>((target_radius * 2 + 1) * (target_radius * 2 + 1)));
    //  map ChunkID -> its index in to_create (was a plain seen-set). A chunk
    // reached from multiple anchors is deduped AND its priority metrics are upgraded
    // to the CLOSEST anchor's (see add_candidate) so per-anchor near-fields are fair.
    std::unordered_map<ChunkID, std::size_t> candidate_index;
    candidate_index.reserve(to_create.capacity() * 2u);

    auto add_candidate = [&](const IVec3& coords,
                             bool surface,
                             int ring_distance,
                             int horizontal_dist2,
                             int vertical_rank,
                             const Vec3& anchor_pos) {
        // NOTE: the active-chunk budget is no longer applied here.
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

        // Generation intent: chunks whose required meshing step is
        // coarse (> 1) generate surface-band data only - no interior SDF, no
        // 3D cave grid. Promotion to LOD0 backfills the full SDF via the
        // meshing dispatch, so a conservative step here is only a perf cost.
        const Vec3 chunk_center =
            (Vec3(coords) + 0.5f) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
        const int required_lod = get_required_lod_for_chunk(coords, chunk_center, anchor_pos);
        const int target_step = get_lod_step_for_level(required_lod);

        //  when a chunk is wanted by more than
        // one anchor, keep the CLOSEST anchor's priority metrics (smallest ring /
        // horizontal distance, finest LOD step) instead of the v1 first-anchor-wins.
        // ring_distance/horizontal_distance_sq/target_step are all monotone in the
        // distance to the SAME enumerating anchor, so the per-field min picks the
        // closest anchor consistently. This makes each anchor's near-field sort to the
        // front of to_create, so the post-sort active-chunk budget truncates only the
        // shared far rim -- no near anchor can starve a far one under union pressure.
        // RESIDENCY-ONLY: streaming sets which chunks are resident, never their content,
        // so world_hash is unaffected; a single anchor never hits the merge path -> the
        // single-anchor stream/hash stays byte-identical.
        auto it = candidate_index.find(id);
        if (it != candidate_index.end()) {
            GenerationCandidate& existing = to_create[it->second];
            existing.ring_distance = std::min(existing.ring_distance, ring_distance);
            existing.horizontal_distance_sq =
                std::min(existing.horizontal_distance_sq, horizontal_dist2);
            existing.vertical_rank = std::min(existing.vertical_rank, vertical_rank);
            existing.target_step = std::min(existing.target_step, target_step);
            return;
        }
        candidate_index.emplace(id, to_create.size());
        to_create.push_back(
            {coords, surface, ring_distance, horizontal_dist2, vertical_rank, target_step});
    };

    // UNION the wanted-set across every anchor. candidate_index (in add_candidate)
    // dedupes a chunk reached from multiple anchors and upgrades it to the CLOSEST
    // anchor's priority metrics; a chunk wanted by ANY anchor is enumerated.
    // One anchor -> the historical single-disc scan, unchanged.
    for (std::size_t ai = 0; ai < anchors.size(); ++ai) {
        const IVec3 camera_chunk = camera_chunks[ai];
        const Vec3& anchor_pos = anchors[ai];
        for (int dz = -target_radius; dz <= target_radius; ++dz) {
            for (int dx = -target_radius; dx <= target_radius; ++dx) {
                const int horizontal_dist2 = horizontal_distance_sq(dx, dz);
                if (horizontal_dist2 > target_radius * target_radius) {
                    continue;
                }

                const int chunk_x = camera_chunk.x + dx;
                const int chunk_z = camera_chunk.z + dz;
                // 5-point span sample; the cache persists for the
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
                    add_candidate(IVec3(chunk_x, y, chunk_z),
                                  true,
                                  ring_distance,
                                  horizontal_dist2,
                                  std::abs(y - span.center_y),
                                  anchor_pos);
                }

                if (ring_distance <= STREAMING_NEAR_VERTICAL_STACK_RADIUS) {
                    add_candidate(IVec3(chunk_x, span.min_y - 1, chunk_z),
                                  false,
                                  ring_distance,
                                  horizontal_dist2,
                                  1,
                                  anchor_pos);
                    add_candidate(IVec3(chunk_x, span.max_y + 1, chunk_z),
                                  false,
                                  ring_distance,
                                  horizontal_dist2,
                                  1,
                                  anchor_pos);
                } else if (ring_distance <= STREAMING_MID_VERTICAL_STACK_RADIUS) {
                    add_candidate(IVec3(chunk_x, span.min_y - 1, chunk_z),
                                  false,
                                  ring_distance,
                                  horizontal_dist2,
                                  2,
                                  anchor_pos);
                    add_candidate(IVec3(chunk_x, span.max_y + 1, chunk_z),
                                  false,
                                  ring_distance,
                                  horizontal_dist2,
                                  2,
                                  anchor_pos);
                }
            }
        }
    }

    std::sort(to_create.begin(),
              to_create.end(),
              [](const GenerationCandidate& a, const GenerationCandidate& b) {
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
        const std::size_t budget =
            std::min(std::min(to_create.size(), budget_headroom),
                     static_cast<std::size_t>(m_last_streaming_budget_stats.generation_budget));
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
        for (const ChunkGenerationRequest& request : generate_now) {
            shadow_note_generation_dispatch(::Luminumbra::Chunk::calculate_id(request.coords));
        }
    }

    // 5. Unload chunks that are now out of range.
    std::vector<ChunkID> to_unload;

    // Add a small buffer (hysteresis) to the render distance to prevent rapid
    // loading/unloading of chunks at the very edge of the view distance.
    const int UNLOAD_DISTANCE_XZ = target_radius + 2;
    const int UNLOAD_DISTANCE_UP = RENDER_DISTANCE_UP + 2;
    const int UNLOAD_DISTANCE_DOWN = RENDER_DISTANCE_DOWN + 2;

    for (const auto& [id, chunk_ptr] : m_streaming_state.chunks) {
        const IVec3 coords = chunk_ptr->get_coords();
        // Multi-anchor range test: keep the chunk if it is in full range of ANY anchor
        // (XZ within the hysteresis disc AND vertically within that anchor's band). One
        // anchor -> identical to the historical camera-relative test.
        bool xz_in_range_any = false;
        bool vert_in_band_any = false;
        for (const IVec3& cc : camera_chunks) {
            const IVec3 d = coords - cc;
            if (std::abs(d.x) > UNLOAD_DISTANCE_XZ || std::abs(d.z) > UNLOAD_DISTANCE_XZ) {
                continue; // outside this anchor's XZ disc
            }
            xz_in_range_any = true;
            if (d.y <= UNLOAD_DISTANCE_UP && d.y >= -UNLOAD_DISTANCE_DOWN) {
                vert_in_band_any = true;
                break;
            }
        }
        if (vert_in_band_any) {
            continue; // in full range of some anchor
        }
        if (!xz_in_range_any) {
            to_unload.push_back(id); // XZ-far from every anchor
            continue;
        }
        // XZ in range of some anchor but vertically outside all bands -> surface-band test.

        // Vertical-unload exemption: a chunk inside its column's
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
        shadow_note_evicted(id);
    }

    //  implementation note: record the elision signals for next tick. m_last_activation_pending ==
    //  0
    // next time (with a static anchor + no world mutation) means this pass made the world fully
    // resident, so the next activation can be skipped.
    m_activation_has_run = true;
    m_last_activation_camera_chunks = camera_chunks;
    m_last_activation_dirty_generation = m_dirty_generation;
    m_last_activation_pending = m_last_streaming_budget_stats.scheduled_generation +
                                m_last_streaming_budget_stats.deferred_generation;
    return true;
}

bool SHIELD_WorldSystem::EnsureCollisionReadyNear(const Vec3& world_pos,
                                                  PhysicsSystem* physics_system,
                                                  int horizontal_radius) {
    return EnsureSurfaceReadyNear(world_pos, physics_system, horizontal_radius, horizontal_radius);
}

bool SHIELD_WorldSystem::EnsureSurfaceReadyNear(const Vec3& world_pos,
                                                PhysicsSystem* physics_system,
                                                int surface_radius,
                                                int collision_radius,
                                                int render_lod0_radius) {
    if (!physics_system) {
        return false;
    }

    // Diagnostic breadcrumbs ( implementation note): EnsureSurfaceReadyNear blocks on
    // three unbounded job waits during the interactive "CONSTRUCTING WORLD GEOMETRY"
    // load. If one wedges (the intermittent freeze), the LAST line printed names the
    // exact phase that hung. Render-only logging; no world_hash impact. Cheap: this
    // runs at world-load/teleport, not per-frame.
    const auto _esrn_t0 = std::chrono::steady_clock::now();
    // ALL THREE unbounded waits in this function run under the
    // opt-in LUMINUMBRA_JOB_WATCHDOG named-phase reporter (previously only the
    // collision-build batch below was wrapped, so a generation/meshing wedge still
    // hung silently). Observability only — hash-neutral, OFF in determinism gates.
    const bool job_watchdog = Luminumbra::Core::JobWatchdogEnabled();
    LUMINUMBRA_CORE_INFO("EnsureSurfaceReadyNear: draining generation jobs (near {:.0f},{:.0f})...",
                         world_pos.x,
                         world_pos.z);
    Luminumbra::Core::WaitWithJobWatchdog(job_watchdog,
                                          "EnsureSurfaceReadyNear/generation-drain",
                                          [this]() { wait_for_generation_jobs(); });
    LUMINUMBRA_CORE_INFO("EnsureSurfaceReadyNear: draining meshing jobs...");
    Luminumbra::Core::WaitWithJobWatchdog(job_watchdog,
                                          "EnsureSurfaceReadyNear/meshing-drain",
                                          [this]() { wait_for_meshing_jobs(); });
    // settle the two-stage promotion pipeline too (publish staged
    // sim truth, dispatch + drain the stage-B render meshes) so the surface
    // band below observes fully settled chunks, exactly as before the split.
    Luminumbra::Core::WaitWithJobWatchdog(
        job_watchdog, "EnsureSurfaceReadyNear/promotion-drain", [this]() {
            wait_for_promotion_jobs();
            wait_for_meshing_jobs();
        });

    const IVec3 center_chunk = world_to_chunk_coords(world_pos);
    const int radius = std::max(0, surface_radius);
    const int collision_range = std::max(0, collision_radius);
    //  radius, decoupled from collision (preview fidelity implementation note).
    // render_lod0_radius < 0 (the default, every game caller) preserves the
    // historical behaviour exactly: the full-detail LOD0 ring boundary == the
    // collision radius, so the game's LOD selection — and world_hash — is
    // unchanged. A caller (the create-world preview) may opt into a LARGER
    // full-SDF render slice than its collision radius so the visible near rings
    // render caves/overhangs instead of coarse heightmap LODs, WITHOUT building
    // any extra gameplay collision (collision still gates on collision_range
    // below). Clamped to [0, radius] so it never exceeds the built surface disc.
    const int lod0_range =
        (render_lod0_radius < 0) ? collision_range : std::clamp(render_lod0_radius, 0, radius);
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
    const std::size_t surface_capacity =
        static_cast<std::size_t>((radius * 2 + 1) * (radius * 2 + 1) * 5);
    chunks_to_build.reserve(surface_capacity);
    chunks_to_consider_for_collision.reserve(surface_capacity);

    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int chunk_x = center_chunk.x + dx;
            const int chunk_z = center_chunk.z + dz;
            // Full column surface span with the same +-1 stack
            // margin the fixed {-1, 0, 1} band provided on flat terrain;
            // steep columns additionally cover every chunk-Y the isosurface
            // passes through so cliff walls are meshed before world enter.
            const ColumnSurfaceSpan span = column_surface_span(chunk_x, chunk_z);
            const int ring_distance = std::max(std::abs(dx), std::abs(dz));
            const int lod = horizon_lod_for_ring(ring_distance, radius, lod0_range);
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
                    chunk, IVec2(dx, dz), std::abs(chunk_y - span.center_y), lod, step};
                chunks_to_consider_for_collision.push_back(surface_chunk);
                if (chunk->get_state() != ChunkState::Ready || chunk->current_lod.load() != lod ||
                    chunk->mesh_vertices.empty() || chunk->mesh_indices.empty()) {
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
            const auto worldgen_scope = acquire_worldgen_sample_scope(); //
            const auto& chunk = build_chunk.chunk;
            const bool needs_full_sdf = build_chunk.step <= 1;
            // Validation guard: a non-empty but wrong-sized full SDF (for example,
            // from a malformed or unsupported save) would reach the unit-step
            // PolygoniseTerrain, which assumes a full
            // (CHUNK_SIZE+1)^3 lattice and reads OUT OF BOUNDS -> heap corruption that can
            // clobber job-completion state and wedge the "CONSTRUCTING WORLD GEOMETRY" load.
            // Detect it and clear the buffer so it REGENERATES a correct lattice below
            // (generation is a pure function of seed/params). Hash-neutral for valid worlds:
            // a well-formed full SDF is always exactly kFullSdfLattice, so this never fires.
            const bool has_malformed_sdf =
                !chunk->sdf_data.empty() && chunk->sdf_data.size() != kFullSdfLattice;
            if (has_malformed_sdf) {
                const IVec3 cc = chunk->get_coords();
                LUMINUMBRA_CORE_WARN(
                    "EnsureSurfaceReadyNear: chunk ({},{},{}) has malformed "
                    "SDF (size {} != {}); regenerating a full lattice before meshing",
                    cc.x,
                    cc.y,
                    cc.z,
                    chunk->sdf_data.size(),
                    kFullSdfLattice);
                chunk->sdf_data.clear();
            }
            const bool missing_required_data =
                needs_full_sdf ? chunk->sdf_data.empty()
                               : (chunk->sdf_data.empty() && chunk->heightmap_data.empty());
            if (has_malformed_sdf) {
                // A malformed non-empty lattice must not fall back to the
                // coarse heightfield: rebuild exact SDF authority first.
                GenerateChunkData(*chunk);
            } else if (missing_required_data) {
                // Chunks restored from a world save arrive with voxel data
                // already populated (possibly carrying player edits);
                // regeneration would clobber those edits. Generation is a
                // pure function of seed/params, so skipping it for any chunk
                // that already has the data its step needs is also a no-op
                // for fresh chunks that merely need a LOD rebuild. Coarse
                // (step > 1) horizon chunks generate the surface band only
                // the heightfield mesher and seam fallback never
                // read interior SDF.
                GenerateChunkData(*chunk, build_chunk.step);
            }
            chunk->set_state(ChunkState::Meshing);
            Luminumbra::World::MarchingCubes::PolygoniseTerrain(
                *this, *chunk, 0.0f, build_chunk.step);
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
        //  ( step 3 — the world-load hang ROOT FIX): the
        // load path never waits on one monolithic batch again. The build set
        // is dispatched in BOUNDED sub-batches; each wait is watchdog-named
        // with batch index + running progress, so (a) the worst single wait
        // is one sub-batch (not thousands of jobs), (b) a wedge is localized
        // to a 64-chunk neighbourhood with monotone progress visible in the
        // log, and (c) the load's wall time has a computable per-batch bound
        // the WorldLoadBounded gate asserts. Opt-in wedge watchdog
        // (LUMINUMBRA_JOB_WATCHDOG=1): observability only — hash-neutral, OFF
        // in determinism gates.
        constexpr std::size_t kBootBuildSubBatch = 64u;
        const std::size_t total_jobs = build_jobs.size();
        const std::size_t batch_count = (total_jobs + kBootBuildSubBatch - 1) / kBootBuildSubBatch;
        LUMINUMBRA_CORE_INFO("EnsureSurfaceReadyNear: dispatching {} surface-build jobs in {} "
                             "bounded sub-batches...",
                             total_jobs,
                             batch_count);
        std::vector<Luminumbra::Job> sub_batch;
        sub_batch.reserve(kBootBuildSubBatch);
        std::size_t completed = 0;
        for (std::size_t begin = 0; begin < total_jobs; begin += kBootBuildSubBatch) {
            const std::size_t end = std::min(begin + kBootBuildSubBatch, total_jobs);
            sub_batch.assign(
                std::make_move_iterator(build_jobs.begin() + static_cast<std::ptrdiff_t>(begin)),
                std::make_move_iterator(build_jobs.begin() + static_cast<std::ptrdiff_t>(end)));
            const Luminumbra::JobHandle handle = m_job_system->dispatch_batch(sub_batch);
            std::string phase =
                "EnsureSurfaceReadyNear/surface-build batch " +
                std::to_string(begin / kBootBuildSubBatch + 1) + "/" + std::to_string(batch_count) +
                " (" + std::to_string(completed) + "/" + std::to_string(total_jobs) +
                " built, near chunk " + std::to_string(center_chunk.x) + "," +
                std::to_string(center_chunk.y) + "," + std::to_string(center_chunk.z) + ")";
            Luminumbra::Core::WaitWithJobWatchdog(
                job_watchdog, std::move(phase), [this, &handle]() { m_job_system->wait(handle); });
            completed = end;
        }
    } else {
        for (auto& job : build_jobs) {
            job();
        }
    }
    LUMINUMBRA_CORE_INFO(
        "EnsureSurfaceReadyNear: surface ready (took {:.0f}ms)",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _esrn_t0)
            .count());

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
        auto add_face_if_neighbor_is_finer =
            [&](int dx, int dz, Luminumbra::World::MarchingCubes::TerrainTransitionFace face) {
                const auto neighbor_it =
                    surface_by_xz.find(horizontal_chunk_key(coords.x + dx, coords.z + dz));
                if (neighbor_it == surface_by_xz.end()) {
                    return;
                }

                const SurfaceHorizonChunk& neighbor = *neighbor_it->second;
                const bool neighbor_is_finer = neighbor.lod < surface_chunk.lod;
                const bool vertical_mixed_lod_pair =
                    neighbor.lod != surface_chunk.lod && neighbor.chunk->get_coords().y != coords.y;
                if (neighbor_is_finer || vertical_mixed_lod_pair) {
                    transition_faces |=
                        static_cast<Luminumbra::World::MarchingCubes::TerrainTransitionFaceMask>(
                            face);
                }
            };

        add_face_if_neighbor_is_finer(-1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMinX);
        add_face_if_neighbor_is_finer(1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMaxX);
        add_face_if_neighbor_is_finer(0, -1, Luminumbra::World::MarchingCubes::TransitionFaceMinZ);
        add_face_if_neighbor_is_finer(0, 1, Luminumbra::World::MarchingCubes::TransitionFaceMaxZ);

        if (transition_faces != Luminumbra::World::MarchingCubes::kNoTransitionFaces) {
            Luminumbra::World::MarchingCubes::AddBoundaryTransitionSkirts(
                *surface_chunk.chunk, surface_chunk.step, transition_faces);
            surface_chunk.chunk->applied_transition_faces.fetch_or(transition_faces,
                                                                   std::memory_order_acq_rel);
        }
    }

    std::size_t collision_count = 0;
    for (const SurfaceHorizonChunk& surface_chunk : chunks_to_consider_for_collision) {
        const auto& chunk = surface_chunk.chunk;
        const IVec2& offset = surface_chunk.offset;
        if (surface_chunk.lod == 0 && surface_chunk.vertical_rank == 0 &&
            std::abs(offset.x) <= collision_range && std::abs(offset.y) <= collision_range &&
            !chunk->has_collision.load()) {
            replace_chunk_collision(*physics_system, *chunk);
            ++collision_count;
        }
    }

    LUMINUMBRA_CORE_INFO("Initial surface horizon ready: radius={}, surface_chunks={}, rebuilt={}, "
                         "lod0={}, lod1={}, lod2={}, collision_radius={}, collisions={}",
                         radius,
                         chunks_to_consider_for_collision.size(),
                         chunks_to_build.size(),
                         lod_counts[0],
                         lod_counts[1],
                         lod_counts[2],
                         collision_range,
                         collision_count);
    // This synchronous rebuild can mutate a settled world (carve /
    // teleport / boot) WITHOUT a chunk-count or anchor delta, so re-open the candidate-pass gate
    // explicitly.
    ++m_dirty_generation;
    //  the rebuild reset has_collision=false on its remeshed chunks (some on worker
    // threads, now joined) and synchronously collided only the in-range subset, so re-open the
    // gated collision scan to backfill colliders for any out-of-range LOD0 chunks. Main-thread
    // store.
    m_collision_pass_dirty = true;
    return true;
}

float SHIELD_WorldSystem::GetTerrainHeightAt(float world_x, float world_z) const {
    // delegates to the one shared height implementation.
    return ComputeShapedHeightSample(world_x, world_z).final_height;
}

SHIELD_WorldSystem::WaterStateHash SHIELD_WorldSystem::debug_water_state_hash() const {
    // Fold the live water-sim state across resident chunks, sorted by chunk id so the result is
    // order-independent.: FNV-1a over the FIXED-POINT int32 bits of water_depth_mm +
    // water_bed_mm (millimetres) — integers are bit-identical across compilers/CPUs so host==peer
    // holds, and the float water_level/flow arrays are now  and are NO LONGER hashed.
    std::vector<ChunkID> ids;
    ids.reserve(m_streaming_state.chunks.size());
    for (const auto& [id, c] : m_streaming_state.chunks) {
        if (c && c->has_water_sim.load(std::memory_order_acquire))
            ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    WaterStateHash out;
    out.water_chunks = ids.size();
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    for (ChunkID id : ids) {
        const auto& c = m_streaming_state.chunks.at(id);
        mix(static_cast<std::uint64_t>(id));
        for (const std::int32_t d : c->water_depth_mm)
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(d)));
        for (const std::int32_t b : c->water_bed_mm)
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b)));
    }
    out.hash = h;
    return out;
}

bool SHIELD_WorldSystem::debug_water_mass_ok() const {
    return m_water_system ? m_water_system->dbg_mass_ok() : true;
}

int SHIELD_WorldSystem::debug_water_seam_wet_pairs() const {
    return m_water_system ? m_water_system->dbg_seam_wet_pairs() : 0;
}

std::int64_t SHIELD_WorldSystem::debug_max_water_depth_mm() const {
    std::int64_t mx = 0;
    for (const auto& [id, c] : m_streaming_state.chunks) {
        if (c && c->has_water_sim.load(std::memory_order_acquire)) {
            for (const std::int32_t d : c->water_depth_mm)
                if (d > mx)
                    mx = d;
        }
    }
    return mx;
}

// Total water VOLUME proxy (sum of depth_mm) over cells within radius_m of a world XZ point —
// the ground-truth measure of whether a region's water is actually draining (falling) or not.
std::int64_t SHIELD_WorldSystem::debug_water_volume_near(const Vec3& center, float radius_m) const {
    const float r2 = radius_m * radius_m;
    std::int64_t sum = 0;
    for (const auto& [id, c] : m_streaming_state.chunks) {
        if (!c || !c->has_water_sim.load(std::memory_order_acquire))
            continue;
        const int res = static_cast<int>(c->current_water_resolution.load());
        if (res <= 1 || static_cast<int>(c->water_depth_mm.size()) != res * res)
            continue;
        const IVec3 cc = c->get_coords();
        const float cw_x = static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(res);
        const float cw_z = static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(res);
        for (int z = 0; z < res; ++z) {
            for (int x = 0; x < res; ++x) {
                const float wx = cc.x * CHUNK_SIZE_X + (x + 0.5f) * cw_x;
                const float wz = cc.z * CHUNK_SIZE_Z + (z + 0.5f) * cw_z;
                const float dx = wx - center.x, dz = wz - center.z;
                if (dx * dx + dz * dz <= r2)
                    sum += c->water_depth_mm[z * res + x];
            }
        }
    }
    return sum;
}

// Total standing water on LAND (cells whose bed sits clearly above sea level) — isolates rain-fed
// puddles/ponds from the sea, which the global max-depth probe is pinned to. Sum of depth_mm.
std::int64_t SHIELD_WorldSystem::debug_land_water_volume_mm() const {
    std::int64_t sum = 0;
    for (const auto& [id, c] : m_streaming_state.chunks) {
        if (!c || !c->has_water_sim.load(std::memory_order_acquire))
            continue;
        const std::size_t nb = c->water_bed_mm.size();
        if (c->water_depth_mm.size() != nb)
            continue;
        for (std::size_t i = 0; i < nb; ++i) {
            if (c->water_bed_mm[i] > 500)
                sum += c->water_depth_mm[i]; // bed > 0.5 m above sea => land
        }
    }
    return sum;
}

// Lowest LAND cell (bed above sea) in a carvable chunk — the natural valley floor where rainfall
// collects. Returns its world pos (surface = bed) + bed height; false if no land chunk is streamed.
bool SHIELD_WorldSystem::debug_lowest_land_pos(Vec3& pos_out, float& bed_m_out) const {
    constexpr std::size_t kFullSdf =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    std::int32_t lowest = 0x7fffffff;
    bool found = false;
    for (const auto& [id, c] : m_streaming_state.chunks) {
        if (!c || !c->has_water_sim.load(std::memory_order_acquire))
            continue;
        if (c->sdf_data.size() != kFullSdf)
            continue;
        const int res = static_cast<int>(c->current_water_resolution.load());
        if (res <= 1 || static_cast<int>(c->water_bed_mm.size()) != res * res)
            continue;
        const IVec3 cc = c->get_coords();
        const float cw_x = static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(res);
        const float cw_z = static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(res);
        for (int z = 0; z < res; ++z)
            for (int x = 0; x < res; ++x) {
                const std::int32_t b = c->water_bed_mm[z * res + x];
                if (b > 2000 &&
                    b < lowest) { // above sea (valley floor, not coast) and the lowest so far
                    lowest = b;
                    const float wx = cc.x * CHUNK_SIZE_X + (x + 0.5f) * cw_x;
                    const float wz = cc.z * CHUNK_SIZE_Z + (z + 0.5f) * cw_z;
                    pos_out = Vec3(wx, static_cast<float>(b) / 1000.0f, wz);
                    bed_m_out = static_cast<float>(b) / 1000.0f;
                    found = true;
                }
            }
    }
    return found;
}

// Find a SHORELINE worth filming: a deep-water cell (in a carvable full-sdf chunk) that sits right
// next to genuinely DRY land (terrain well above the water surface). Returns the water cell as the
// carve start, the unit direction from the water toward the dry bank, the water surface height, and
// the bank height. Picks the tallest dry bank found (most dramatic dry->flooded contrast). False if
// no such shoreline is streamed (e.g. open sea surrounded by sea). This is what makes a convincing
// "carve the bank, water floods the dry side" shot — the naive deepest-water cell is mid-ocean.
bool SHIELD_WorldSystem::debug_find_shoreline(Vec3& water_pos_out,
                                              float& to_land_x,
                                              float& to_land_z,
                                              float& water_surf_out,
                                              float& bank_height_out) const {
    constexpr std::size_t kFullSdf =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    float best_bank = -1e9f;
    bool found = false;
    for (const auto& [id, c] : m_streaming_state.chunks) {
        if (!c || !c->has_water_sim.load(std::memory_order_acquire))
            continue;
        if (c->sdf_data.size() != kFullSdf)
            continue;
        const int res = static_cast<int>(c->current_water_resolution.load());
        if (res <= 1 || static_cast<int>(c->water_depth_mm.size()) != res * res ||
            static_cast<int>(c->water_bed_mm.size()) != res * res)
            continue;
        const IVec3 cc = c->get_coords();
        const float cw_x = static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(res);
        const float cw_z = static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(res);
        for (int z = 0; z < res; ++z) {
            for (int x = 0; x < res; ++x) {
                if (c->water_depth_mm[z * res + x] < 2500)
                    continue; // need real depth (>=2.5 m)
                const float wx = cc.x * CHUNK_SIZE_X + (x + 0.5f) * cw_x;
                const float wz = cc.z * CHUNK_SIZE_Z + (z + 0.5f) * cw_z;
                const float surf = static_cast<float>(c->water_bed_mm[z * res + x] +
                                                      c->water_depth_mm[z * res + x]) /
                                   1000.0f;
                for (int a = 0; a < 8; ++a) {
                    const float ang = static_cast<float>(a) * 0.7853981634f;
                    const float ox = std::cos(ang), oz = std::sin(ang);
                    const float th = GetTerrainHeightAt(wx + ox * 8.0f, wz + oz * 8.0f);
                    const float bank = th - surf; // dry bank height above the water
                    if (bank > best_bank && bank > 2.5f) {
                        best_bank = bank;
                        water_pos_out = Vec3(wx, surf, wz);
                        to_land_x = ox;
                        to_land_z = oz;
                        water_surf_out = surf;
                        bank_height_out = bank;
                        found = true;
                    }
                }
            }
        }
    }
    return found;
}

// Render-only: force every simulated water chunk to regenerate its surface mesh next frame.
// Used by capture tooling so a draining body updates its visible surface every frame instead of
// on the coalesced WATER_MESH_DIRTY_TICK_INTERVAL cadence. Does NOT touch the hashed sim state.
void SHIELD_WorldSystem::debug_force_water_remesh() {
    for (const auto& [id, c] : m_streaming_state.chunks) {
        if (c && c->has_water_sim.load(std::memory_order_acquire)) {
            c->water_mesh_generated.store(false);
            c->water_mesh_dirty_ticks = 0;
        }
    }
}

// World position (surface_y = bed+depth) of the deepest wet water cell across all simulated
// chunks — used by capture tooling to anchor a camera/dig on a real river or lake. Returns the
// origin and *depth_mm_out=0 if no water is simulated.
Vec3 SHIELD_WorldSystem::debug_deepest_water_pos(std::int64_t* depth_mm_out) const {
    std::int64_t best = 0;
    Vec3 best_pos(0.0f, 0.0f, 0.0f);
    // Only full-res (lod-0) chunks are CARVABLE (EditTerrainVoxel's exact test), so anchoring on
    // one guarantees the breach lands — coarse far chunks carry water but no editable sdf.
    constexpr std::size_t kFullSdf =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    for (const auto& [id, c] : m_streaming_state.chunks) {
        if (!c || !c->has_water_sim.load(std::memory_order_acquire))
            continue;
        if (c->sdf_data.size() != kFullSdf)
            continue;
        const int res = static_cast<int>(c->current_water_resolution.load());
        if (res <= 1 || static_cast<int>(c->water_depth_mm.size()) != res * res ||
            static_cast<int>(c->water_bed_mm.size()) != res * res)
            continue;
        const IVec3 cc = c->get_coords();
        const float cw_x = static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(res);
        const float cw_z = static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(res);
        for (int z = 0; z < res; ++z) {
            for (int x = 0; x < res; ++x) {
                const std::int32_t d = c->water_depth_mm[z * res + x];
                if (d <= best)
                    continue;
                best = d;
                const float wx = cc.x * CHUNK_SIZE_X + (x + 0.5f) * cw_x;
                const float wz = cc.z * CHUNK_SIZE_Z + (z + 0.5f) * cw_z;
                const float surf_y = static_cast<float>(c->water_bed_mm[z * res + x] + d) / 1000.0f;
                best_pos = Vec3(wx, surf_y, wz);
            }
        }
    }
    if (depth_mm_out)
        *depth_mm_out = best;
    return best_pos;
}

//  terraform the water bed (dig/dam) — delegates to the WaterSystem.
int SHIELD_WorldSystem::EditTerrainBed(const Vec3& world_pos,
                                       std::int32_t delta_mm,
                                       float radius_m) {
    return m_water_system ? m_water_system->EditTerrainBed(world_pos, delta_mm, radius_m) : 0;
}

// configure the finite-hydrology cycle (no perpetual source + rain + evaporation).
void SHIELD_WorldSystem::SetWaterHydrology(bool finite,
                                           std::int32_t rain_mm_per_tick,
                                           std::int32_t evap_mm_per_tick) {
    if (m_water_system)
        m_water_system->SetHydrology(finite, rain_mm_per_tick, evap_mm_per_tick);
}

// weather-driven per-cell rain passthrough (null = OFF).
void SHIELD_WorldSystem::SetWaterWeatherRain(const Systems::WeatherSystem* weather,
                                             std::int32_t scale_mm) {
    if (m_water_system)
        m_water_system->SetWeatherRain(weather, scale_mm);
}

// water-source diagnostics passthrough.
std::int64_t SHIELD_WorldSystem::debug_water_sources_seen() const {
    return m_water_system ? m_water_system->debug_sources_seen() : 0;
}
std::int64_t SHIELD_WorldSystem::debug_water_source_injected_mm() const {
    return m_water_system ? m_water_system->debug_source_injected_mm() : 0;
}
std::vector<IVec3> SHIELD_WorldSystem::debug_water_grid_chunk_coords(std::size_t max_count) const {
    std::vector<std::pair<ChunkID, IVec3>> found;
    for (const auto& [id, c] : m_streaming_state.chunks) {
        if (c && c->has_water_sim.load(std::memory_order_relaxed)) {
            found.emplace_back(id, c->get_coords());
        }
    }
    std::sort(
        found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<IVec3> out;
    for (std::size_t i = 0; i < found.size() && i < max_count; ++i)
        out.push_back(found[i].second);
    return out;
}

// epoch passthrough (0 when no water system).
std::uint64_t SHIELD_WorldSystem::water_epoch() const {
    return m_water_system ? m_water_system->water_epoch() : 0;
}

// live water surface from the float mirror on the y=0 column chunk.
float SHIELD_WorldSystem::live_water_surface_at(float world_x, float world_z) const {
    const float terrain = GetTerrainHeightAt(world_x, world_z);
    const IVec3 cc = world_to_chunk_coords(Vec3(world_x, 0.5f, world_z));
    const std::shared_ptr<Chunk> c = find_streamed_chunk(cc);
    if (!c || !c->has_water_sim.load(std::memory_order_relaxed))
        return terrain;
    const int res = c->current_water_resolution.load(std::memory_order_relaxed);
    if (res <= 1 || static_cast<int>(c->water_level_data.size()) != res * res)
        return terrain;
    const float lx = world_x - static_cast<float>(cc.x * CHUNK_SIZE_X);
    const float lz = world_z - static_cast<float>(cc.z * CHUNK_SIZE_Z);
    const int sx = std::clamp(static_cast<int>((lx / CHUNK_SIZE_X) * res), 0, res - 1);
    const int sz = std::clamp(static_cast<int>((lz / CHUNK_SIZE_Z) * res), 0, res - 1);
    return c->water_level_data[sz * res + sx];
}

bool SHIELD_WorldSystem::debug_water_grid_at(float world_x, float world_z) const {
    // Mirrors the injection loop's precondition (has_water_sim + complete FLOAT
    // grid). EMPIRICAL ground truth (the water-source staging hunt): water grids live on
    // the 2.5D COLUMN's y=0 chunk regardless of the terrain sign — probe there.
    const IVec3 cc = world_to_chunk_coords(Vec3(world_x, 0.5f, world_z));
    const std::shared_ptr<Chunk> c = find_streamed_chunk(cc);
    if (!c || !c->has_water_sim.load(std::memory_order_relaxed))
        return false;
    const int res = c->current_water_resolution.load(std::memory_order_relaxed);
    return res > 1 && static_cast<int>(c->water_level_data.size()) == res * res;
}

// boot-settle mode passthrough (lifts the live-play water caps during Boot only).
void SHIELD_WorldSystem::SetWaterBootSettleMode(bool on) {
    if (m_water_system)
        m_water_system->SetBootSettleMode(on);
}

// loaded-boot water pause passthrough (see WaterSystem::SetBootPaused).
void SHIELD_WorldSystem::SetWaterBootPaused(bool on) {
    if (m_water_system)
        m_water_system->SetBootPaused(on);
}

// session water-sim resolution passthrough (sim.water_high_res; set once pre-tick).
void SHIELD_WorldSystem::SetWaterSimResolution(int cells_per_side) {
    if (m_water_system)
        m_water_system->SetSimResolution(cells_per_side);
}

// boot-time save-migration passthrough over the loaded chunk map (see
// WaterSystem::MigrateChunksToSimResolution).
std::size_t SHIELD_WorldSystem::MigrateWaterSimResolution() {
    return m_water_system ? m_water_system->MigrateChunksToSimResolution(m_streaming_state.chunks)
                          : 0u;
}

int SHIELD_WorldSystem::debug_water_sim_resolution() const {
    return m_water_system ? m_water_system->sim_resolution() : 0;
}

std::size_t SHIELD_WorldSystem::debug_water_chunks_off_resolution() const {
    if (!m_water_system)
        return 0u;
    const int session_res = m_water_system->sim_resolution();
    std::size_t off = 0;
    for (const auto& [id, c] : m_streaming_state.chunks) {
        (void)id;
        if (c && c->has_water_sim.load(std::memory_order_relaxed) &&
            c->current_water_resolution.load(std::memory_order_relaxed) != session_res) {
            ++off;
        }
    }
    return off;
}

// rotating sim-window cursor persistence seam (see WaterSystem accessors).
std::size_t SHIELD_WorldSystem::GetWaterSimWindowCursor() const {
    return m_water_system ? m_water_system->GetSimWindowCursor() : 0u;
}
void SHIELD_WorldSystem::SetWaterSimWindowCursor(std::size_t cursor) {
    if (m_water_system)
        m_water_system->SetSimWindowCursor(cursor);
}

// PLAYER-FACING terraform: carve/fill the VOXEL terrain in-world,
// then couple the water to the new bed. Edits the signed density field (sdf_data) of
// every streamed chunk the sphere overlaps. Positive density is air (same convention
// as CarveSphereIntoChunk / MarchingCubes), so a DIG raises each in-range sample to at
// least (radius - distance) [carve to air] and a FILL lowers it to at most -(radius -
// distance) [pack solid]. Boundary (+1) samples are edited identically in every chunk
// that shares them — the world-space sample math is position-only — so a carve spanning
// a chunk seam stays watertight after remesh. Each touched chunk is marked voxel-dirty +
// LOD-invalidated so the existing surface-rebuild path remeshes it (generation is skipped
// for chunks that already carry sdf data, so the edit survives), then EnsureSurfaceReadyNear
// rebuilds the mesh + collider band and the water bed is resampled (dig drains, fill dams).
// Deterministic: the edit is a pure function of (world_pos, radius_m, fill); sdf_data is
// hashed + persisted directly (WorldPersistenceRoundtrip), and sqrt is IEEE-754
// correctly-rounded — the same determinism contract the green persistence carve relies on.
// Returns the number of chunks actually modified.
int SHIELD_WorldSystem::EditTerrainVoxel(const Vec3& world_pos,
                                         float radius_m,
                                         bool fill,
                                         PhysicsSystem* physics_system) {
    if (radius_m <= 0.0f)
        return 0;
    constexpr int size_x = CHUNK_SIZE_X + 1;
    constexpr int size_y = CHUNK_SIZE_Y + 1;
    constexpr int size_z = CHUNK_SIZE_Z + 1;
    constexpr std::size_t expected = static_cast<std::size_t>(size_x) *
                                     static_cast<std::size_t>(size_y) *
                                     static_cast<std::size_t>(size_z);

    // Chunk-coord AABB the sphere can touch (carve may span several chunks).
    const IVec3 lo = world_to_chunk_coords(world_pos - Vec3(radius_m, radius_m, radius_m));
    const IVec3 hi = world_to_chunk_coords(world_pos + Vec3(radius_m, radius_m, radius_m));
    int edited = 0;
    for (int cz = lo.z; cz <= hi.z; ++cz)
        for (int cy = lo.y; cy <= hi.y; ++cy)
            for (int cx = lo.x; cx <= hi.x; ++cx) {
                const auto chunk = find_streamed_chunk(IVec3(cx, cy, cz));
                if (!chunk || chunk->sdf_data.size() != expected)
                    continue;
                const IVec3 base =
                    chunk->get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
                bool changed = false;
                for (int z = 0; z < size_z; ++z)
                    for (int y = 0; y < size_y; ++y)
                        for (int x = 0; x < size_x; ++x) {
                            const float dx = static_cast<float>(base.x + x) - world_pos.x;
                            const float dy = static_cast<float>(base.y + y) - world_pos.y;
                            const float dz = static_cast<float>(base.z + z) - world_pos.z;
                            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                            if (dist > radius_m)
                                continue;
                            const std::size_t idx =
                                static_cast<std::size_t>(x) +
                                static_cast<std::size_t>(y) * static_cast<std::size_t>(size_x) +
                                static_cast<std::size_t>(z) * static_cast<std::size_t>(size_x) *
                                    static_cast<std::size_t>(size_y);
                            const float mag = radius_m - dist;
                            if (!fill) {
                                // DIG → carve to air (raise toward +mag).
                                if (mag > chunk->sdf_data[idx]) {
                                    chunk->sdf_data[idx] = mag;
                                    if (!chunk->material_data.empty() &&
                                        idx < chunk->material_data.size())
                                        chunk->material_data[idx] = 0u;
                                    changed = true;
                                }
                            } else {
                                // FILL → pack solid (lower toward -mag).
                                if (-mag < chunk->sdf_data[idx]) {
                                    chunk->sdf_data[idx] = -mag;
                                    changed = true;
                                }
                            }
                        }
                if (changed) {
                    chunk->mark_voxel_data_dirty();
                    bump_far_lod_authority_revision(chunk->get_coords());
                    chunk->current_lod.store(-1, std::memory_order_release);
                    ++edited;
                }
            }
    if (edited > 0) {
        // Remesh + rebuild colliders for the edited band (rings in chunk units).
        const int rings = static_cast<int>(radius_m / static_cast<float>(CHUNK_SIZE_X)) + 2;
        EnsureSurfaceReadyNear(world_pos, physics_system, rings, rings);
        // Couple the water: a dig lowers the bed (drain), a fill raises it (dam). The bed
        // delta tracks the carve depth at the sphere center (~radius below the surface).
        const std::int32_t delta_mm =
            static_cast<std::int32_t>((fill ? 1.0f : -1.0f) * radius_m * 1000.0f);
        EditTerrainBed(world_pos, delta_mm, radius_m);
    }
    return edited;
}

WorldGenLayerSample SHIELD_WorldSystem::SampleWorldGenLayers(const Vec3& world_pos) const {
    WorldGenLayerSample sample;
    sample.world_pos = world_pos;

    // heights come from the one shared implementation so this sample
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
        sample.cave_noise = m_cave_generator->GenSingle3D(world_pos.x * m_params.cave_frequency,
                                                          world_pos.y * m_params.cave_frequency,
                                                          world_pos.z * m_params.cave_frequency,
                                                          m_seed + 1);
        sample.cave_value = std::clamp((sample.cave_noise + 1.0f) * 0.5f, 0.0f, 1.0f);
        const SurfaceBreakSample sb = sample_surface_breaks(world_pos, sample.final_height);
        sample.cave_density =
            surface_capped_cave_density(sample.terrain_density,
                                        sample.cave_noise,
                                        m_params,
                                        sb.effective_cap); // cheese component (diagnostic)
        // final density routes through the single composition point so it includes
        // the noise-router spaghetti tunnels and Worley caverns.
        sample.final_density =
            EvaluateCaveDensity(world_pos, sample.terrain_density, sb.effective_cap, sb.carve);
    }

    sample.solid = sample.final_density < 0.0f;
    if (!sample.solid) {
        sample.material = MaterialType::Air;
    } else {
        // biome-aware surface material. When biomes are disabled the
        // biome id is kNoBiome and SurfaceMaterialForColumn reproduces the
        // legacy classifier bit-for-bit. The column biome is resolved from the
        // surface (world_x/world_z), not the sample's depth.
        const u8 biome_id = BiomeIdAt(world_pos.x, world_pos.z);
        // River banks: a column under meaningful river influence lays
        // its above-water skin as the biome filler. The threshold keeps the
        // bank a thin rim around the channel rather than the whole valley.
        const bool river_bank = RiverInfluenceFromNoise(world_pos.x, world_pos.z) > 0.25f;
        sample.material =
            SurfaceMaterialForColumn(world_pos.y, sample.final_height, biome_id, river_bank);
    }
    return sample;
}

float SHIELD_WorldSystem::get_density_at_from_precalculated(const Vec3& world_pos,
                                                            float terrain_height) const {
    float terrain_density = world_pos.y - terrain_height;
    if (m_params.caves_enabled) {
        // single composition point (cheese + noise-router spaghetti).
        const SurfaceBreakSample sb = sample_surface_breaks(world_pos, terrain_height);
        terrain_density =
            EvaluateCaveDensity(world_pos, terrain_density, sb.effective_cap, sb.carve);
    }
    return terrain_density;
}

float SHIELD_WorldSystem::get_density_at(const Vec3& world_pos) const {
    return SampleWorldGenLayers(world_pos).final_density;
}

std::vector<Luminumbra::Chunk*> SHIELD_WorldSystem::get_renderable_chunks() {
    // Client render-path observation: publish drained-and-due front batches
    // (main-thread, idempotent; non-force — see the 5b regime note).
    publish_completed_generation_jobs(/*force=*/false);
    process_completed_meshing_jobs(/*force=*/false);

    std::vector<Luminumbra::Chunk*> renderable;
    renderable.reserve(m_streaming_state.chunks.size());

    for (auto const& [id, chunk_ptr] : m_streaming_state.chunks) {
        if (chunk_ptr->get_state() == Luminumbra::ChunkState::Ready) {
            if (!chunk_ptr->mesh_vertices.empty()) {
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
    stats.generation_job_active = generation_jobs_active();
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
            (chunk_ptr->mesh_vertices.size() + chunk_ptr->water_mesh_vertices.size()) *
                sizeof(VoxelVertex) +
            (chunk_ptr->mesh_indices.size() + chunk_ptr->water_mesh_indices.size()) * sizeof(u32);
        stats.sdf_payload_bytes += chunk_ptr->sdf_data.size() * sizeof(f32);
        stats.heightmap_payload_bytes += chunk_ptr->heightmap_data.size() * sizeof(f32);
        if (chunk_ptr->sdf_data.empty() && !chunk_ptr->heightmap_data.empty()) {
            ++stats.sdf_skipped_chunks;
        }
    }

    return stats;
}

SHIELD_WorldSystem::CameraLocalCoverageStats
SHIELD_WorldSystem::get_camera_local_coverage_stats(const Vec3& camera_position,
                                                    int horizontal_radius) const {
    CameraLocalCoverageStats stats;
    stats.camera_position = camera_position;
    stats.camera_chunk = world_to_chunk_coords(camera_position);
    stats.horizontal_radius = std::max(0, horizontal_radius);
    stats.terrain_height_under_camera = GetTerrainHeightAt(camera_position.x, camera_position.z);
    stats.camera_height_above_terrain = camera_position.y - stats.terrain_height_under_camera;
    stats.surface_chunk_under_camera = world_to_chunk_coords(
        Vec3(camera_position.x, stats.terrain_height_under_camera, camera_position.z));

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
            const bool is_center_surface_chunk = coords.x == stats.surface_chunk_under_camera.x &&
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

    stats.near_field_renderable = stats.expected_surface_chunks > 0 &&
                                  stats.missing_surface_chunks == 0 &&
                                  stats.renderable_surface_chunks == stats.expected_surface_chunks;
    return stats;
}

SHIELD_WorldSystem::FrustumSurfaceCoverageStats
SHIELD_WorldSystem::get_frustum_surface_coverage_stats(const Vec3& camera_position,
                                                       const std::array<Vec4, 6>& frustum_planes,
                                                       float max_distance) const {
    FrustumSurfaceCoverageStats stats;

    // Positive-vertex AABB/frustum intersection: the box is outside when its
    // most-positive corner against a plane normal is still behind the plane.
    const auto aabb_intersects_frustum = [&frustum_planes](const Vec3& min_corner,
                                                           const Vec3& max_corner) {
        for (const Vec4& plane : frustum_planes) {
            const Vec3 positive_corner(plane.x >= 0.0f ? max_corner.x : min_corner.x,
                                       plane.y >= 0.0f ? max_corner.y : min_corner.y,
                                       plane.z >= 0.0f ? max_corner.z : min_corner.z);
            if (plane.x * positive_corner.x + plane.y * positive_corner.y +
                    plane.z * positive_corner.z + plane.w <
                0.0f) {
                return false;
            }
        }
        return true;
    };

    const IVec3 camera_chunk = world_to_chunk_coords(camera_position);
    const int radius =
        std::max(0, static_cast<int>(std::ceil(max_distance / static_cast<float>(CHUNK_SIZE_X))));
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
                const auto it = m_streaming_state.chunks.find(
                    Chunk::calculate_id(IVec3(chunk_x, chunk_y, chunk_z)));
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
                                 ? static_cast<double>(stats.renderable_chunks) /
                                       static_cast<double>(stats.expected_chunks)
                                 : 1.0;
    return stats;
}

void SHIELD_WorldSystem::StampStructuresIntoChunk(Luminumbra::Chunk& chunk,
                                                  const IVec3& base_pos) const {
    // Guard: only the full-res path (sdf_data populated). The step>1 coarse path
    // leaves sdf_data empty and carries no structure material (documented gap).
    if (!m_structures_enabled || m_structure_pools.empty() || chunk.sdf_data.empty()) {
        return;
    }

    const int size_x = CHUNK_SIZE_X + 1;
    const int size_y = CHUNK_SIZE_Y + 1;
    const int size_z = CHUNK_SIZE_Z + 1;
    const size_t padded_volume = static_cast<size_t>(size_x) * size_y * size_z;
    if (chunk.sdf_data.size() != padded_volume) {
        return; // defensive: mismatched lattice, do not stamp
    }

    // Chunk voxel-coordinate bounds (the padded lattice spans [base, base+SIZE]
    // inclusive; voxels owned by this chunk for stamping are [base, base+SIZE]
    // so shared boundary voxels are written consistently by each neighbour).
    const int min_x = base_pos.x;
    const int min_y = base_pos.y;
    const int min_z = base_pos.z;

    for (const World::StructureTemplatePool& pool : m_structure_pools) {
        if (!pool.ok()) {
            continue;
        }
        // Enumerate sites over the chunk X/Z AABB padded by the pool footprint,
        // so a structure whose voxels straddle the chunk border is seen here.
        const int pad = std::max(0, pool.footprint_radius);
        const std::vector<World::StructureSite> sites =
            World::SitesInArea(pool,
                               m_seed,
                               min_x - pad,
                               min_z - pad,
                               base_pos.x + CHUNK_SIZE_X + pad + 1,
                               base_pos.z + CHUNK_SIZE_Z + pad + 1);

        for (const World::StructureSite& enumerated : sites) {
            // Drop the site to a SINGLE integer floor of the surface height,
            // computed once per site so every chunk/path that touches this
            // structure agrees on its Y (determinism: identical across paths).
            const float surface = GetTerrainHeightAt(static_cast<float>(enumerated.origin.x),
                                                     static_cast<float>(enumerated.origin.z));
            if (surface < SEA_LEVEL) {
                continue; // no half-submerged structures (deterministic skip)
            }
            const int floor_y = static_cast<int>(std::floor(surface));

            World::StructureSite site = enumerated;
            site.origin.y = floor_y;
            const std::vector<World::StructureVoxel> voxels = World::AssembleStructure(pool, site);

            for (const World::StructureVoxel& voxel : voxels) {
                // Only write voxels that fall inside this chunk's lattice. The
                // padded enumeration makes neighbouring chunks each own a
                // disjoint subset of a straddling structure (boundary-complete,
                // no double-stamp, order-independent).
                const int lx = voxel.position.x - min_x;
                const int ly = voxel.position.y - min_y;
                const int lz = voxel.position.z - min_z;
                if (lx < 0 || lx >= size_x || ly < 0 || ly >= size_y || lz < 0 || lz >= size_z) {
                    continue;
                }
                const size_t index = static_cast<size_t>(lx) + static_cast<size_t>(ly) * size_x +
                                     static_cast<size_t>(lz) * size_x * size_y;

                // Lazily allocate the material channel on first stamp (0 = Air
                // sentinel). Empty for non-structure chunks => byte-identical.
                if (chunk.material_data.empty()) {
                    chunk.material_data.assign(padded_volume, 0u);
                }
                chunk.sdf_data[index] = -1.0f; // solid
                chunk.material_data[index] = voxel.material;
            }
        }
    }
}

void SHIELD_WorldSystem::GenerateChunkData(Luminumbra::Chunk& chunk, int target_step) const {
    const IVec3 coords = chunk.get_coords();
    const IVec3 base_pos = coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    const int size_x = CHUNK_SIZE_X + 1;
    const int size_y = CHUNK_SIZE_Y + 1;
    const int size_z = CHUNK_SIZE_Z + 1;
    const size_t padded_volume = static_cast<size_t>(size_x) * size_y * size_z;

    //  SDF skip: chunks generated for a coarse meshing step (> 1) are
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

        // The grid helper uses the same shaped height definition as scalar sampling.
        ComputeShapedHeightGrid(
            base_pos.x, base_pos.z, size_x, size_z, chunk.heightmap_data.data());
        chunk.mark_sdf_generated_current_params();
        chunk.clear_voxel_data_dirty();
        return;
    }

    // Generate the authoritative CPU SDF and matching heightmap.
    chunk.sdf_data.resize(padded_volume);

    const size_t heightmap_size = static_cast<size_t>(size_x) * size_z;
    chunk.heightmap_data.resize(heightmap_size);

    // --- Step 1: Generate all noise data for the chunk in large, SIMD-accelerated batches ---
    std::vector<float> cave_noise;
    if (m_params.caves_enabled) {
        cave_noise.resize(padded_volume);
    }

    // SIMD-batched heights share the scalar sampler's unconditional shaping.
    std::vector<float> shaped_heights(heightmap_size);
    ComputeShapedHeightGrid(base_pos.x, base_pos.z, size_x, size_z, shaped_heights.data());

    // FastNoise GenUniformGrid3D populates its buffer in [x][y][z] layout where x varies fastest
    if (m_params.caves_enabled) {
        m_cave_generator->GenUniformGrid3D(cave_noise.data(),
                                           base_pos.x,
                                           base_pos.y,
                                           base_pos.z,
                                           size_x,
                                           size_y,
                                           size_z,
                                           m_params.cave_frequency,
                                           m_seed + 1);
    }

    // --- Step 2: Combine the pre-generated noise to calculate the final SDF values ---
    for (int z = 0; z < size_z; ++z) {
        for (int y = 0; y < size_y; ++y) {
            for (int x = 0; x < size_x; ++x) {
                // For 2D noise buffers in [x][z] layout (x varies fastest):
                size_t index_2d_read = static_cast<size_t>(x) + static_cast<size_t>(z) * size_x;

                // Index for WRITING to our sdf_data and heightmap_data with bounds checking
                size_t sdf_write_idx = static_cast<size_t>(x) + static_cast<size_t>(y) * size_x +
                                       static_cast<size_t>(z) * size_x * size_y;
                size_t heightmap_write_idx =
                    static_cast<size_t>(x) + static_cast<size_t>(z) * size_x;

                // Bounds checking
                if (sdf_write_idx >= chunk.sdf_data.size()) {
                    LUMINUMBRA_CORE_ERROR(
                        "SDF index out of bounds: {} >= {}", sdf_write_idx, chunk.sdf_data.size());
                    continue;
                }
                if (y == 0 && heightmap_write_idx >= chunk.heightmap_data.size()) {
                    LUMINUMBRA_CORE_ERROR("Heightmap index out of bounds: {} >= {}",
                                          heightmap_write_idx,
                                          chunk.heightmap_data.size());
                    continue;
                }

                // A. Calculate final terrain height for this (x,z) column
                const float terrain_h = shaped_heights[index_2d_read];

                // B. Calculate base terrain density
                float current_world_y = static_cast<float>(base_pos.y + y);
                float terrain_density = current_world_y - terrain_h;

                // C. Carve caves using the 3D noise buffer
                if (m_params.caves_enabled) {
                    // For 3D noise buffer in [x][y][z] layout (x varies fastest):
                    size_t cave_read_idx = static_cast<size_t>(x) +
                                           static_cast<size_t>(y) * size_x +
                                           static_cast<size_t>(z) * size_x * size_y;
                    const Vec3 cave_world_pos(static_cast<float>(base_pos.x + x),
                                              current_world_y,
                                              static_cast<float>(base_pos.z + z));
                    const SurfaceBreakSample sb = sample_surface_breaks(cave_world_pos, terrain_h);
                    // Pass the precomputed batch cheese noise through the single composition point;
                    // the hot path skips a re-sample while the noise router adds tunnels per voxel.
                    terrain_density = EvaluateCaveDensity(cave_world_pos,
                                                          terrain_density,
                                                          sb.effective_cap,
                                                          sb.carve,
                                                          &cave_noise[cave_read_idx]);
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

    // stamp authored structure voxels into the populated SDF (solid
    // density + per-voxel material) before the dirty flag is cleared. No-op when
    // structures are disabled or sdf_data is empty (coarse step>1 path).
    StampStructuresIntoChunk(chunk, base_pos);

    // Generation produces the canonical voxel data; only post-generation edits
    // count as unsaved dirty state.
    chunk.mark_sdf_generated_current_params();
    chunk.clear_voxel_data_dirty();
}

JobHandle
SHIELD_WorldSystem::dispatch_generation_jobs(const std::vector<IVec3>& chunks_to_generate) {
    // Coordinate-only callers (initial world load, regeneration, tests,
    // persistence) always want full voxel generation.
    std::vector<ChunkGenerationRequest> requests;
    requests.reserve(chunks_to_generate.size());
    for (const IVec3& coords : chunks_to_generate) {
        requests.push_back({coords, 1});
    }
    return dispatch_generation_jobs(requests);
}

JobHandle SHIELD_WorldSystem::dispatch_generation_jobs(
    const std::vector<ChunkGenerationRequest>& chunks_to_generate) {
    // dispatch APPENDS a batch to the lane FIFO (the old
    // refuse-while-active head guard is gone — the scheduler's budget gate
    // already prevents scheduling-path double-dispatch, boot-path callers
    // dispatch-then-wait, and multi-batch flight is the queue's design).
    StreamingState::GenerationBatch batch;
    std::vector<Luminumbra::Job> jobs;
    for (const auto& request : chunks_to_generate) {
        const IVec3 coords = request.coords;
        // Chunks restored from a world save (or already generated) carry
        // populated voxel data, possibly with player edits; regeneration would
        // clobber those edits. Generation is a pure function of seed/params,
        // so skipping any chunk that already has voxel data (full SDF or the
        // surface-band heightmap) is a no-op for untouched chunks and the
        // load/generation contract for saved ones. A surface-band chunk later
        // promoted to LOD0 gets its full SDF via the  promotion lane.
        const auto existing = m_streaming_state.chunks.find(Chunk::calculate_id(coords));
        if (existing != m_streaming_state.chunks.end() && existing->second &&
            (!existing->second->sdf_data.empty() || !existing->second->heightmap_data.empty())) {
            continue;
        }

        auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
        chunk->set_state(Luminumbra::ChunkState::Loading);
        m_streaming_state.chunks[chunk->get_id()] = chunk;

        const int target_step = request.target_step;
        chunk->pending_generation_ready.store(false, std::memory_order_release);
        batch.chunks.push_back(chunk);
        jobs.emplace_back([this, chunk, target_step]() {
            const auto worldgen_scope = acquire_worldgen_sample_scope(); //
            GenerateChunkData(*chunk, target_step);
            // stage completion only — the MAIN thread flips
            // Loading→Idle in publish_completed_generation_jobs, so chunk
            // lifecycle is never a worker-timing side effect.
            chunk->pending_generation_ready.store(true, std::memory_order_release);
        });
    }

    if (m_job_system && !jobs.empty()) {
        batch.handle = m_job_system->dispatch_batch(jobs);
        // tick-keyed activation stamp (inert until the
        // barrier swap; -1 with no tick source = publish-when-drained).
        batch.due_tick =
            m_current_sim_tick >= 0 ? m_current_sim_tick + kActivationPipelineLatencyTicks : -1;
        const JobHandle handle = batch.handle;
        m_streaming_state.generation_batches.push_back(std::move(batch));
        return handle;
    }
    return {}; // no batch dispatched (all requests skipped, or no job system)
}

void SHIELD_WorldSystem::dispatch_meshing_jobs(const std::vector<MeshingWorkItem>& chunks_to_mesh) {
    process_completed_meshing_jobs(/*force=*/false);
    //  5b: refuse by FIFO depth (deterministic), not by raw job
    // activity. Refused items re-candidate on a later pass, as always.
    if (m_streaming_state.meshing_batches.size() >= kMaxMeshingBatchesInFlight) {
        return;
    }

    //  ( step 1): LOD0 promotions of chunks without a full
    // voxel field ride the sim-truth PROMOTION lane — stage A generates the
    // field, the main thread publishes it, and only then is the render mesh
    // dispatched back through here (stage B, re-classifying as non-promotion
    // because the live sdf_data is full by then). Meshing never generates.
    // Classification happens HERE on the main thread (sdf_data is main-thread-
    // owned between dispatch and job start, so the value matches what the old
    // in-job test read).: a non-empty but WRONG-SIZED
    // sdf_data (corrupt save / stale coarse producer) must never be copied
    // into the unit-step polygonise — its corner indexing assumes the full
    // (CHUNK+1)^3 lattice and would read out of bounds; it re-routes through
    // the promotion lane to regenerate (mirrors the EnsureSurfaceReadyNear
    // guard; generation is a pure function of seed/params, so this is
    // hash-neutral for valid worlds).
    std::vector<MeshingWorkItem> promotion_items;
    std::vector<MeshingWorkItem> mesh_items;
    mesh_items.reserve(chunks_to_mesh.size());
    for (const MeshingWorkItem& work_item : chunks_to_mesh) {
        const int step = get_lod_step_for_level(work_item.lod_level);
        const bool has_malformed_sdf = work_item.terrain_mesh_required &&
                                       !work_item.chunk->sdf_data.empty() &&
                                       work_item.chunk->sdf_data.size() != kFullSdfLattice;
        const bool needs_sim_truth = work_item.terrain_mesh_required && step <= 1 &&
                                     work_item.chunk->sdf_data.size() != kFullSdfLattice;
        if (has_malformed_sdf) {
            LUMINUMBRA_CORE_WARN(
                "Meshing promotion: chunk ({},{},{}) has malformed sdf_data "
                "(size {} != full lattice {}) — regenerating instead of meshing it",
                work_item.chunk->get_coords().x,
                work_item.chunk->get_coords().y,
                work_item.chunk->get_coords().z,
                work_item.chunk->sdf_data.size(),
                kFullSdfLattice);
        }
        ((needs_sim_truth || has_malformed_sdf) ? promotion_items : mesh_items)
            .push_back(work_item);
    }
    // Guard: only hand items to the promotion lane when its whole pipeline is
    // idle. This makes the stage-B re-entry into dispatch_meshing_jobs
    // structurally impossible mid-dispatch (the publish inside
    // dispatch_promotion_jobs' head can only ever see empty state from here),
    // rather than relying on the caller-side scheduling gate alone. Refused
    // items re-candidate on a later pass — the meshing lane's semantics.
    if (!promotion_items.empty() && !promotion_pipeline_pending()) {
        dispatch_promotion_jobs(promotion_items);
    }
    if (mesh_items.empty()) {
        return;
    }

    // Snapshot meshed-chunk LODs per horizontal column once so the
    // transition-face checks below are hash lookups instead of a full
    // chunk-map scan per face per work item (O(batch * chunks) before,
    // which dominated the dispatch frame once backlog-scaled batches
    // landed). Mirrors the candidate-side snapshot built in update.
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
        meshed_columns[horizontal_chunk_key(neighbor_coords.x, neighbor_coords.z)].push_back(
            {neighbor.get(), neighbor_coords.y, neighbor_lod});
    }

    // Near-field hole-fill candidates (no active mesh yet, capped prefix of
    // the sorted batch) ride the High job lane so visible gaps close ahead of
    // bulk meshing, generation, and LOD/water remeshes; everything else stays
    // on the Normal lane. The sort in update already places the hole-fill
    // prefix first, so this mirrors the existing priority order.
    std::vector<Luminumbra::Job> high_priority_jobs;
    std::vector<Luminumbra::Job> normal_priority_jobs;
    // build this dispatch's batch record locally; it is
    // appended to the lane FIFO at the dispatch below. The due stamp (5a-3)
    // is inert until the barrier swap; -1 = publish-when-drained (client).
    StreamingState::MeshingBatch mesh_batch;
    mesh_batch.due_tick =
        m_current_sim_tick >= 0 ? m_current_sim_tick + kActivationPipelineLatencyTicks : -1;
    mesh_batch.chunks.reserve(mesh_items.size());
    for (const MeshingWorkItem& work_item : mesh_items) {
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
            auto add_face_if_neighbor_is_finer =
                [&](int dx, int dz, Luminumbra::World::MarchingCubes::TerrainTransitionFace face) {
                    const auto column_it =
                        meshed_columns.find(horizontal_chunk_key(coords.x + dx, coords.z + dz));
                    if (column_it == meshed_columns.end()) {
                        return;
                    }
                    for (const DispatchMeshedColumnEntry& entry : column_it->second) {
                        if (entry.chunk == chunk.get()) {
                            continue;
                        }
                        if (entry.lod < lod_level ||
                            (entry.lod != lod_level && entry.y != coords.y)) {
                            transition_faces |= static_cast<
                                Luminumbra::World::MarchingCubes::TerrainTransitionFaceMask>(face);
                            return;
                        }
                    }
                };

            add_face_if_neighbor_is_finer(
                -1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMinX);
            add_face_if_neighbor_is_finer(
                1, 0, Luminumbra::World::MarchingCubes::TransitionFaceMaxX);
            add_face_if_neighbor_is_finer(
                0, -1, Luminumbra::World::MarchingCubes::TransitionFaceMinZ);
            add_face_if_neighbor_is_finer(
                0, 1, Luminumbra::World::MarchingCubes::TransitionFaceMaxZ);
        }
        mesh_batch.chunks.push_back({chunk, terrain_mesh_required, transition_faces});

        auto& lane_jobs = work_item.high_priority ? high_priority_jobs : normal_priority_jobs;
        lane_jobs.emplace_back([this, chunk, step, transition_faces, terrain_mesh_required]() {
            try {
                const auto worldgen_scope = acquire_worldgen_sample_scope(); //
                Luminumbra::Chunk scratch(chunk->get_coords());
                scratch.water_level_data = chunk->water_level_data;
                scratch.water_flow_data = chunk->water_flow_data;
                scratch.water_sim_terrain_height = chunk->water_sim_terrain_height;
                scratch.has_water_sim.store(chunk->has_water_sim.load(std::memory_order_acquire),
                                            std::memory_order_release);
                scratch.water_mesh_generated.store(false, std::memory_order_release);
                scratch.current_water_resolution.store(
                    chunk->current_water_resolution.load(std::memory_order_acquire),
                    std::memory_order_release);

                if (terrain_mesh_required) {
                    if (step <= 1 && chunk->sdf_data.size() != kFullSdfLattice) {
                        //  tripwire: meshing NEVER generates sim
                        // truth. A unit-step item without a full live voxel
                        // field must have been routed through the promotion
                        // lane at dispatch — reaching here is a regression of
                        // the activation queue decoupling, not a recoverable state.
                        LUMINUMBRA_CORE_ERROR(
                            "MESHING JOB: chunk ({},{},{}) reached the render-only meshing "
                            "lane without full sim truth (sdf size {} != {}) — the  "
                            "promotion routing is broken; failing the mesh",
                            chunk->get_coords().x,
                            chunk->get_coords().y,
                            chunk->get_coords().z,
                            chunk->sdf_data.size(),
                            kFullSdfLattice);
                        chunk->pending_mesh_failed.store(true, std::memory_order_release);
                        return;
                    }
                    scratch.sdf_data = chunk->sdf_data;
                    scratch.heightmap_data = chunk->heightmap_data;
                    // carry the live material channel so stage-B
                    // promotion meshes classify authored structure voxels
                    // exactly as the old fused backfill+mesh job did (its
                    // scratch had materials stamped by GenerateChunkData).
                    // Also fixes the latent authored-material loss on plain
                    // remeshes, which copied only sdf+heightmap. Mesh bytes
                    // are world_hash-excluded either way (empty -> empty,
                    //  lazy alloc preserved).
                    scratch.material_data = chunk->material_data;

                    // 1. Generate the terrain mesh from the SDF data into a scratch chunk.
                    Luminumbra::World::MarchingCubes::PolygoniseTerrain(*this, scratch, 0.0f, step);
                    Luminumbra::World::MarchingCubes::AddBoundaryTransitionSkirts(
                        scratch, step, transition_faces);
                }

                // Terrain mesh generation debug logging removed to prevent segfault

                // 2. Generate the water surface mesh if the water system exists
                if (m_water_system) {
                    // Only generate high-detail water mesh for highest LOD terrain
                    if (step <= 1) {
                        Luminumbra::World::MarchingCubes::GenerateWaterMesh(
                            *m_water_system, *this, scratch);
                    } else {
                        scratch.water_mesh_vertices.clear();
                        scratch.water_mesh_indices.clear();
                        scratch.water_mesh_generated.store(false, std::memory_order_release);
                    }
                }

                if (terrain_mesh_required) {
                    chunk->pending_mesh_vertices = std::move(scratch.mesh_vertices);
                    chunk->pending_mesh_indices = std::move(scratch.mesh_indices);
                }
                chunk->pending_water_mesh_vertices = std::move(scratch.water_mesh_vertices);
                chunk->pending_water_mesh_indices = std::move(scratch.water_mesh_indices);
                chunk->water_mesh_generated.store(
                    scratch.water_mesh_generated.load(std::memory_order_acquire),
                    std::memory_order_release);
                chunk->pending_mesh_ready.store(true, std::memory_order_release);

                // Job completion debug logging removed to prevent segfault
            } catch (const std::exception& e) {
                LUMINUMBRA_CORE_ERROR("MESHING JOB CRASH: Chunk ({},{},{}) failed: {}",
                                      chunk->get_coords().x,
                                      chunk->get_coords().y,
                                      chunk->get_coords().z,
                                      e.what());
                chunk->pending_mesh_failed.store(true, std::memory_order_release);
            } catch (...) {
                LUMINUMBRA_CORE_ERROR(
                    "MESHING JOB CRASH: Chunk ({},{},{}) failed with unknown exception",
                    chunk->get_coords().x,
                    chunk->get_coords().y,
                    chunk->get_coords().z);
                chunk->pending_mesh_failed.store(true, std::memory_order_release);
            }
        });
    }
    if (m_job_system && (!high_priority_jobs.empty() || !normal_priority_jobs.empty())) {
        if (!high_priority_jobs.empty()) {
            mesh_batch.handle_high =
                m_job_system->dispatch_batch(high_priority_jobs, JobPriority::High);
        }
        if (!normal_priority_jobs.empty()) {
            mesh_batch.handle = m_job_system->dispatch_batch(normal_priority_jobs);
        }
        m_streaming_state.meshing_batches.push_back(std::move(mesh_batch));
    } else {
        for (auto& job : high_priority_jobs) {
            job();
        }
        for (auto& job : normal_priority_jobs) {
            job();
        }
        m_streaming_state.meshing_batches.push_back(std::move(mesh_batch));
        process_completed_meshing_jobs(/*force=*/true);
    }
}

bool SHIELD_WorldSystem::publish_front_meshing_batch(bool force) {
    // Publish EXACTLY the front batch once BOTH its job lanes have drained
    // (the pre-priority-lane all-or-nothing semantics, per batch) AND (in the
    // non-force regime) its due tick has arrived.
    if (m_streaming_state.meshing_batches.empty()) {
        return false;
    }
    auto& front_batch = m_streaming_state.meshing_batches.front();
    if (!force && front_batch.due_tick >= 0) {
        // activation queue: tick-stamped batches publish exclusively via activate_due /
        // force drains (see publish_front_generation_batch).
        return false;
    }
    if (has_active_job(front_batch.handle) || has_active_job(front_batch.handle_high)) {
        return false;
    }

    // Results are published only once BOTH lanes of this dispatch finished,
    // keeping the pre-priority-lane all-or-nothing semantics per batch.
    for (const auto& job_chunk : front_batch.chunks) {
        const auto& chunk = job_chunk.chunk;
        if (!chunk) {
            continue;
        }

        const int completed_lod = chunk->pending_lod.load(std::memory_order_acquire);
        const bool mesh_ready = chunk->pending_mesh_ready.load(std::memory_order_acquire);
        const bool mesh_failed = chunk->pending_mesh_failed.load(std::memory_order_acquire);

        if (mesh_ready && !mesh_failed) {
            if (job_chunk.terrain_mesh_required) {
                //  ( step 1): the LOD0-promotion backfill
                // publish that used to live here moved to
                // process_completed_promotion_jobs — the meshing lane is
                // render-only and never writes sim truth.
                chunk->mesh_vertices = std::move(chunk->pending_mesh_vertices);
                chunk->mesh_indices = std::move(chunk->pending_mesh_indices);
                chunk->current_lod.store(completed_lod, std::memory_order_release);
                chunk->applied_transition_faces.store(job_chunk.transition_faces,
                                                      std::memory_order_release);
                chunk->mesh_version++;
                chunk->has_collision.store(false, std::memory_order_release);
                //  a remesh / LOD0 promotion just cleared this chunk's collider, so
                // re-open the gated collision scan (Step 4) to rebuild it. Main-thread store.
                m_collision_pass_dirty = true;
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
        if (chunk->get_state() == Luminumbra::ChunkState::Ready) {
            shadow_note_ready(chunk->get_id()); //  shadow: first-activation latency
        }

        chunk->pending_mesh_vertices.clear();
        chunk->pending_mesh_indices.clear();
        chunk->pending_water_mesh_vertices.clear();
        chunk->pending_water_mesh_indices.clear();
        // pending_sdf/heightmap/material_data are promotion-lane
        // staging now — the meshing lane neither writes nor clears them.
        chunk->pending_mesh_ready.store(false, std::memory_order_release);
        chunk->pending_mesh_failed.store(false, std::memory_order_release);
        chunk->pending_lod.store(-1, std::memory_order_release);
    }

    m_streaming_state.meshing_batches.pop_front();
    return true;
}

void SHIELD_WorldSystem::process_completed_meshing_jobs(bool force) {
    // Drain every publishable front batch, in FIFO order (publication order
    // == dispatch order — deterministic; stops at the first still-running or
    // not-yet-due batch so later batches never publish ahead of an earlier one).
    while (publish_front_meshing_batch(force)) {}
}

void SHIELD_WorldSystem::set_params(const TerrainGenParams& params) {
    // the full sequenced drain — promotion stage-A jobs run
    // GenerateChunkData concurrently, so generator state must not be mutated
    // until the whole pipeline (including stage B) has settled.
    wait_for_streaming_jobs();

    reinitialize_noise(&params);
}

void SHIELD_WorldSystem::set_seed(int seed) {
    wait_for_streaming_jobs();

    reinitialize_noise(nullptr, seed);
}

void SHIELD_WorldSystem::clear_world(PhysicsSystem* physics_system) {
    wait_for_streaming_jobs();

    if (physics_system) {
        for (const auto& [id, chunk] : m_streaming_state.chunks) {
            physics_system->remove_chunk_collision(id);
        }
    }
    m_streaming_state.chunks.clear();
    m_shadow_generation_dispatch_tick.clear();
    m_shadow_promotion_dispatch_tick.clear();
    LUMINUMBRA_CORE_INFO("World cleared.");
}

void SHIELD_WorldSystem::regenerate_all_chunks(PhysicsSystem* physics_system) {
    wait_for_streaming_jobs();

    LUMINUMBRA_CORE_INFO("Regenerating all active chunks...");
    std::vector<IVec3> coords_to_regenerate;
    coords_to_regenerate.reserve(m_streaming_state.chunks.size());
    for (const auto& [id, chunk] : m_streaming_state.chunks) {
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
    return IVec3(static_cast<int>(std::floor(position.x / CHUNK_SIZE_X)),
                 static_cast<int>(std::floor(position.y / CHUNK_SIZE_Y)),
                 static_cast<int>(std::floor(position.z / CHUNK_SIZE_Z)));
}

void SHIELD_WorldSystem::wait_for_streaming_jobs() {
    //  ( step 1): the SEQUENCED drain. Sim truth for LOD0
    // promotions is generated on the promotion lane and published before its
    // render mesh is dispatched (stage B) — and the whole two-stage pipeline
    // still settles inside ONE call, so every per-tick observation point
    // (availability digest, collision pass, water init, world_hash) sees
    // exactly the same settled state per tick as the old fused pipeline.
    wait_for_generation_jobs();
    wait_for_meshing_jobs();   // publish any in-flight render meshes; frees the lanes
    wait_for_promotion_jobs(); // publish staged sim truth + dispatch stage B
    wait_for_meshing_jobs();   // publish stage-B render meshes — same call, same tick
    if (promotion_pipeline_pending()) {
        LUMINUMBRA_CORE_ERROR(
            "wait_for_streaming_jobs: promotion pipeline still pending after the "
            "sequenced drain — the activation queue same-tick settlement invariant is broken");
    }
}

std::vector<std::shared_ptr<Luminumbra::Chunk>>
SHIELD_WorldSystem::snapshot_streamed_chunks() const {
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

void SHIELD_WorldSystem::bump_far_lod_authority_revision(const IVec3& chunk_coords) {
    const auto floor_div = [](int value, int divisor) {
        const int quotient = value / divisor;
        const int remainder = value % divisor;
        return remainder < 0 ? quotient - 1 : quotient;
    };
    constexpr int kChunksPerRegion = World::kFarLodRegionSizeMeters / CHUNK_SIZE_X;
    const int home_rx = floor_div(chunk_coords.x, kChunksPerRegion);
    const int home_rz = floor_div(chunk_coords.z, kChunksPerRegion);
    const int local_x = static_cast<int>(static_cast<std::int64_t>(chunk_coords.x) -
                                         static_cast<std::int64_t>(home_rx) * kChunksPerRegion);
    const int local_z = static_cast<int>(static_cast<std::int64_t>(chunk_coords.z) -
                                         static_cast<std::int64_t>(home_rz) * kChunksPerRegion);

    std::array<int, 2> affected_x{home_rx, home_rx};
    std::array<int, 2> affected_z{home_rz, home_rz};
    std::size_t x_count = 1;
    std::size_t z_count = 1;
    if (local_x == 0)
        affected_x[x_count++] = home_rx - 1;
    else if (local_x == kChunksPerRegion - 1)
        affected_x[x_count++] = home_rx + 1;
    if (local_z == 0)
        affected_z[z_count++] = home_rz - 1;
    else if (local_z == kChunksPerRegion - 1)
        affected_z[z_count++] = home_rz + 1;

    const u64 revision = m_far_lod_authority_revision.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    std::lock_guard<std::mutex> lock(m_far_lod_region_revision_mutex);
    for (std::size_t z = 0; z < z_count; ++z) {
        for (std::size_t x = 0; x < x_count; ++x) {
            m_far_lod_region_revisions[{affected_x[x], affected_z[z]}] = revision;
        }
    }
}

u64 SHIELD_WorldSystem::far_lod_region_authority_revision(i32 rx, i32 rz) const {
    std::lock_guard<std::mutex> lock(m_far_lod_region_revision_mutex);
    const auto found = m_far_lod_region_revisions.find({rx, rz});
    return found == m_far_lod_region_revisions.end() ? 0u : found->second;
}

void SHIELD_WorldSystem::notify_far_lod_authority_durable(const IVec3& chunk_coords) {
    bump_far_lod_authority_revision(chunk_coords);
}

std::shared_ptr<const FarLodSdfSnapshot>
SHIELD_WorldSystem::capture_far_lod_sdf_snapshot(i32 rx, i32 rz) const {
    constexpr int kRegionChunkSpan = World::kFarLodRegionSizeMeters / CHUNK_SIZE_X;
    constexpr std::size_t kFullSdfLatticeSize = static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                                static_cast<std::size_t>(CHUNK_SIZE_Y + 1) *
                                                static_cast<std::size_t>(CHUNK_SIZE_Z + 1);

    const std::int64_t min_chunk_x64 = static_cast<std::int64_t>(rx) * kRegionChunkSpan - 1;
    const std::int64_t max_chunk_x64 =
        static_cast<std::int64_t>(rx) * kRegionChunkSpan + kRegionChunkSpan;
    const std::int64_t min_chunk_z64 = static_cast<std::int64_t>(rz) * kRegionChunkSpan - 1;
    const std::int64_t max_chunk_z64 =
        static_cast<std::int64_t>(rz) * kRegionChunkSpan + kRegionChunkSpan;
    if (min_chunk_x64 < Chunk::kPackedMinXz || max_chunk_x64 > Chunk::kPackedMaxXz ||
        min_chunk_z64 < Chunk::kPackedMinXz || max_chunk_z64 > Chunk::kPackedMaxXz) {
        return nullptr;
    }
    const int min_chunk_x = static_cast<int>(min_chunk_x64);
    const int max_chunk_x = static_cast<int>(max_chunk_x64);
    const int min_chunk_z = static_cast<int>(min_chunk_z64);
    const int max_chunk_z = static_cast<int>(max_chunk_z64);

    auto snapshot = std::make_shared<FarLodSdfSnapshot>();
    snapshot->capture_epoch = m_far_lod_capture_epoch.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    snapshot->params_hash = World::ComputeTerrainParamsHash(m_params, m_seed);
    snapshot->authority_revision = m_far_lod_authority_revision.load(std::memory_order_acquire);
    snapshot->region_authority_revision = far_lod_region_authority_revision(rx, rz);
    snapshot->entries.reserve(m_streaming_state.chunks.size());

    // This capture runs after the owner thread's normal streaming publication
    // point. Copying here is the ownership boundary: workers receive only the
    // owned value vectors below, never a Chunk or a borrowed vector span.
    for (const auto& [id, chunk] : m_streaming_state.chunks) {
        (void)id;
        if (!chunk || chunk->sdf_data.size() != kFullSdfLatticeSize ||
            (!chunk->material_data.empty() && chunk->material_data.size() != kFullSdfLatticeSize)) {
            continue;
        }
        const IVec3& coords = chunk->get_coords();
        if (coords.x < min_chunk_x || coords.x > max_chunk_x || coords.z < min_chunk_z ||
            coords.z > max_chunk_z) {
            continue;
        }

        FarLodSdfSnapshotEntry entry;
        entry.coords = coords;
        entry.provenance = chunk->sdf_provenance();
        entry.voxel_revision = chunk->voxel_revision();
        entry.authority_durable = !chunk->is_voxel_data_dirty();
        entry.sdf_data = chunk->sdf_data;
        entry.material_data = chunk->material_data;
        snapshot->entries.push_back(std::move(entry));
    }

    std::sort(snapshot->entries.begin(),
              snapshot->entries.end(),
              [](const FarLodSdfSnapshotEntry& lhs, const FarLodSdfSnapshotEntry& rhs) {
                  if (lhs.coords.z != rhs.coords.z)
                      return lhs.coords.z < rhs.coords.z;
                  if (lhs.coords.x != rhs.coords.x)
                      return lhs.coords.x < rhs.coords.x;
                  return lhs.coords.y < rhs.coords.y;
              });
    return snapshot;
}

bool SHIELD_WorldSystem::is_far_lod_sdf_snapshot_current(const FarLodSdfSnapshot& snapshot) const {
    if (snapshot.params_hash != World::ComputeTerrainParamsHash(m_params, m_seed) ||
        snapshot.authority_revision !=
            m_far_lod_authority_revision.load(std::memory_order_acquire)) {
        return false;
    }

    // Eviction is safe because snapshot owns its bytes. A still-resident
    // chunk with a changed local revision/provenance, however, supersedes it.
    for (const FarLodSdfSnapshotEntry& entry : snapshot.entries) {
        const auto current = find_streamed_chunk(entry.coords);
        if (current && (current->voxel_revision() != entry.voxel_revision ||
                        current->sdf_provenance() != entry.provenance)) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<Luminumbra::Chunk>
SHIELD_WorldSystem::find_streamed_chunk(const IVec3& coords) const {
    const auto it = m_streaming_state.chunks.find(Chunk::calculate_id(coords));
    return it != m_streaming_state.chunks.end() ? it->second : nullptr;
}

bool SHIELD_WorldSystem::adopt_streamed_chunk(const std::shared_ptr<Luminumbra::Chunk>& chunk) {
    if (!chunk) {
        return false;
    }
    //   lifecycle rule (streaming-owned, NOT the persistence
    // codec's business): a save captured between generation-job completion and
    // the main-thread Loading→Idle publish (the save quiesce is deliberately
    // non-publishing) carries Loading WITH populated voxel data. The data is
    // complete generation output, so the adoptee enters as Idle — a
    // stuck-Loading adoptee would never re-candidate for anything.
    if (chunk->get_state() == Luminumbra::ChunkState::Loading &&
        (!chunk->sdf_data.empty() || !chunk->heightmap_data.empty())) {
        chunk->set_state(Luminumbra::ChunkState::Idle);
    }
    const bool adopted = m_streaming_state.chunks.emplace(chunk->get_id(), chunk).second;
    if (adopted && chunk->sdf_provenance() == ChunkSdfProvenance::LoadedOrEdited) {
        bump_far_lod_authority_revision(chunk->get_coords());
    }
    return adopted;
}

} // namespace Luminumbra::Systems
