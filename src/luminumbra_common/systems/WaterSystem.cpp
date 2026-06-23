#define GLM_ENABLE_EXPERIMENTAL

#include "WaterSystem.h"
#include "SHIELD_WorldSystem.h"
#include "../world/Chunk.h"
#include "../core/WaterComponents.h"
#include "../components/CoreComponents.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_set> // spec 008 follow-up: scope water snapshots to simmed chunks + neighbors
#include <glm/gtx/compatibility.hpp>
#include "../core/Log.h"

namespace Luminumbra::Systems {

// --- Constants for Simulation ---
constexpr float FLOW_CONSTANT = 0.1f;
constexpr float MIN_FLOW_DIFF = 0.001f;
constexpr float MAX_WATER_COMPRESSION = 0.2f;
constexpr int WATER_MESH_DIRTY_TICK_INTERVAL = 15; // spec 009: responsive water mesh (render-only, not
                                                   // hashed) so a filling river updates its surface ~0.5s
                                                   // instead of 2s — visible flowing water, not stale mesh.
// spec 008 follow-up (streaming-burst amortization): cap how many chunks first-time-initialize
// their water sim grid per tick. Each init samples WaterLevelAt + GetTerrainHeightAt for every
// cell (resolution^2), so a moving camera that streams in many water chunks at once initialized
// ALL of them in one tick -> a 300ms+ spike (research: time-slice the streaming integration). The
// deferred chunks keep has_water_sim=false and init over the next ticks, draining the backlog. The
// cap is a DETERMINISTIC count (same active-chunk iteration order on host==peer), and each chunk's
// init result is order-independent (pure worldgen samples), so run==replay / host==peer hold.
constexpr int MAX_WATER_INITS_PER_TICK = 6;
// spec 008 follow-up: cap how many active water chunks are SIMULATED per tick. The sim dispatches
// the woken chunks to workers then m_job_system->wait()s on completion (+ snapshot/integrate copies),
// all O(chunks_to_sim); moving into water-heavy terrain woke hundreds at once -> ~450ms main-thread
// block. We simulate a deterministic ROTATING window (sorted by chunk id, advanced each tick) so the
// per-tick work is bounded and every chunk still sims over a few ticks. Deterministic — guarded by
// the WaterDeterminism live-water gate.
constexpr std::size_t MAX_WATER_SIMS_PER_TICK = 64;
// spec 008 follow-up: cap how many water chunks RESIZE their sim grid per tick. Each resize re-samples
// GetTerrainHeightAt per cell (~30us, up to 1024 at Ultra), so a burst of LOD-boundary crossings while
// moving spiked the adaptive loop to ~180ms. Same deterministic rotating-window amortization (sorted by
// chunk id) as the init/sim caps — bounded per-tick cost, every chunk still reaches its target res over
// a few ticks. Guarded by the WaterDeterminism live-water gate.
constexpr std::size_t MAX_WATER_RESIZES_PER_TICK = 1;

// --- Spec 009: fixed-point virtual-pipes (Mei) flowing-water solver ----------------------------
// Integer-only so host==peer is BIT-exact (the water-state hash FNV-1a's the raw int32 bits). Depth
// and flux are in MILLIMETRES (per unit cell area -> flux and depth share mm units). The whole hashed
// path is integer: no float/libm/RNG/wall-clock. Mass is exact (one shared int32 flux subtracted from
// one cell, added to its neighbour). See docs/specs/009-flowing-water-terraforming/spec.md.
constexpr std::int64_t MM_PER_M    = 1000;
constexpr std::int32_t MIN_FLOW_MM = 2;     // sub-2mm surface diffs produce no flux (kills limit-cycle jitter)
constexpr std::int64_t K_ACCEL     = 256;   // gain: q += (K_ACCEL*dSurf_mm) >> FLOW_SHIFT  (dSurf 1m -> +62mm)
constexpr int          FLOW_SHIFT  = 12;
constexpr std::int64_t FRICTION_NUM = 200, FRICTION_SHIFT = 8; // q = (q*200)>>8 ~ 0.78 (strong damping -> stable, modest steady flow)
constexpr std::int32_t RIVER_DISCHARGE_MM = 24; // mm/tick injected at a river-channel source cell
constexpr float        RIVER_SOURCE_THRESHOLD = 0.45f; // RiverInfluenceAt >= this => source cell
constexpr std::int32_t EVAP_MM = 1;         // cull <1mm films on non-source cells (deterministic)

// Spec 009 Phase 3: the HASHED water-sim grid runs at ONE FIXED resolution for ALL simulated chunks,
// a DETERMINISTIC function of worldgen only (NOT camera distance). This is required for (a) host==peer —
// a camera-driven resolution made the hashed mm state differ per peer — and (b) cross-chunk flux, which
// needs neighbour chunks' boundary cells to align cell-for-cell. High (16 -> 2m cells) resolves a river
// channel. Visual mesh LOD, if wanted, stays a separate RENDER-ONLY concern. Tunable here.
constexpr int WATER_SIM_RESOLUTION = static_cast<int>(WaterDetailLevel::Medium); // 8x8 (4m cells).
// Perf: High (16) was ~4x and spiked the moving water phase to ~130ms; Medium is ~4x cheaper (~32ms
// worst) and still uniform+deterministic. Cross-chunk flux REQUIRES a uniform resolution (boundary
// cells must align), so a river-aware High-only-on-river-chunks variant would need seam resampling —
// a later refinement if 4m river cells read too coarse.

namespace {

int GetWaterResolution(const Chunk& chunk) {
    return chunk.current_water_resolution.load(std::memory_order_relaxed);
}

size_t GetWaterCellCount(int resolution) {
    return static_cast<size_t>(resolution) * static_cast<size_t>(resolution);
}

bool HasCompleteWaterGrid(const Chunk& chunk, int resolution) {
    if (!chunk.has_water_sim.load(std::memory_order_relaxed) || resolution <= 1) {
        return false;
    }

    const size_t cell_count = GetWaterCellCount(resolution);
    return chunk.water_level_data.size() >= cell_count
        && chunk.water_flow_data.size() >= cell_count
        && chunk.water_sim_terrain_height.size() >= cell_count;
}

int FloorDiv(int value, int divisor) {
    int quotient = value / divisor;
    int remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
        --quotient;
    }
    return quotient;
}

int PositiveMod(int value, int divisor) {
    int result = value % divisor;
    return result < 0 ? result + divisor : result;
}

// Spec 009 Slice-1: integer virtual-pipes step for ONE chunk (INTERNAL edges only — cross-chunk flux
// is Phase 3). Injects deterministic river sources, runs flux -> K outflow-clamp -> symmetric apply on
// the int32 depth/bed/edge_flux, applies the sea/edge sink + evaporation, and regenerates the float
// water_level_data mirror for the mesher. Accumulates per-tick source/sink mm into out_src/out_sink for
// the mass-conservation invariant (AC-2). Everything that feeds the hash is integer + row-major fixed
// order -> bit-exact host==peer / run==replay.
void StepChunkWaterFixed(Chunk& c, SHIELD_WorldSystem& shield,
                         std::int64_t& out_src, std::int64_t& out_sink) {
    const int res = GetWaterResolution(c);
    if (res <= 1) return;
    const int n = res * res;
    if (static_cast<int>(c.water_depth_mm.size()) != n ||
        static_cast<int>(c.water_bed_mm.size()) != n) return;
    if (static_cast<int>(c.water_edge_flux.size()) != 2 * n) c.water_edge_flux.assign(2 * n, 0);

    std::vector<std::int32_t>& depth = c.water_depth_mm;
    std::vector<std::int32_t>& bed   = c.water_bed_mm;
    std::vector<std::int32_t>& flux  = c.water_edge_flux;
    const IVec3 cc = c.get_coords();
    const float cw_x = static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(res);
    const float cw_z = static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(res);
    const std::int32_t sea_mm = 0; // SEA_LEVEL == 0
    auto IDX = [res](int x, int z) { return z * res + x; };

    const std::vector<std::int32_t> depth_before = depth; // for the activity delta + remesh signal

    // --- Phase 0: SOURCES (deterministic, pure function of position) ---
    for (int z = 0; z < res; ++z) for (int x = 0; x < res; ++x) {
        const float wx = cc.x * CHUNK_SIZE_X + (x + 0.5f) * cw_x;
        const float wz = cc.z * CHUNK_SIZE_Z + (z + 0.5f) * cw_z;
        if (shield.RiverInfluenceAt(wx, wz) >= RIVER_SOURCE_THRESHOLD) {
            depth[IDX(x, z)] += RIVER_DISCHARGE_MM;
            out_src += RIVER_DISCHARGE_MM;
        }
    }

    // --- Phase 1: FLUX on +X and +Z internal edges, from the read-only surface snapshot ---
    std::vector<std::int64_t> surf(n);
    for (int i = 0; i < n; ++i) surf[i] = static_cast<std::int64_t>(bed[i]) + depth[i];
    auto compute_edge = [&](int i, int j, int eidx) {
        const std::int64_t dSurf = surf[i] - surf[j];
        std::int64_t q = static_cast<std::int64_t>(flux[eidx]) + ((K_ACCEL * dSurf) >> FLOW_SHIFT);
        q = (q * FRICTION_NUM) >> FRICTION_SHIFT;
        if (dSurf < MIN_FLOW_MM && dSurf > -MIN_FLOW_MM) q = 0;
        if (q >  2000000000LL) q =  2000000000LL;
        if (q < -2000000000LL) q = -2000000000LL;
        flux[eidx] = static_cast<std::int32_t>(q);
    };
    for (int z = 0; z < res; ++z) for (int x = 0; x < res; ++x) {
        const int i = IDX(x, z);
        if (x + 1 < res) compute_edge(i, IDX(x + 1, z), 2 * i + 0); else flux[2 * i + 0] = 0;
        if (z + 1 < res) compute_edge(i, IDX(x, z + 1), 2 * i + 1); else flux[2 * i + 1] = 0;
    }

    // --- Phase 2: K outflow-clamp per cell (Sigma_out <= depth -> non-negative + mass-exact) ---
    // A cell's outgoing edges are the ones where IT is the higher (source) side: +X/+Z of i when the
    // flux is positive, and +X/+Z of the left/up neighbour when their flux is negative. Each edge has
    // exactly ONE source cell, so per-cell clamps touch disjoint edge sets -> order-independent.
    for (int z = 0; z < res; ++z) for (int x = 0; x < res; ++x) {
        const int i = IDX(x, z);
        std::int32_t* e[4]; std::int32_t sgn[4]; int ne = 0;
        std::int64_t out = 0;
        auto add_out = [&](std::int32_t* ptr, std::int32_t s) {
            const std::int64_t o = static_cast<std::int64_t>(*ptr) * s; // outflow magnitude if >0
            if (o > 0) { out += o; e[ne] = ptr; sgn[ne] = s; ++ne; }
        };
        if (x + 1 < res) add_out(&flux[2 * i + 0], +1);
        if (z + 1 < res) add_out(&flux[2 * i + 1], +1);
        if (x - 1 >= 0)  add_out(&flux[2 * IDX(x - 1, z) + 0], -1);
        if (z - 1 >= 0)  add_out(&flux[2 * IDX(x, z - 1) + 1], -1);
        const std::int64_t d = depth[i];
        if (out > d) {
            for (int k = 0; k < ne; ++k) {
                const std::int64_t mag = static_cast<std::int64_t>(*e[k]) * sgn[k]; // positive
                const std::int64_t scaled = (mag * d) / out;                        // floor toward 0
                *e[k] = static_cast<std::int32_t>(scaled * sgn[k]);
            }
        }
    }

    // --- Phase 3: APPLY (symmetric: same int subtracted from P, added to N -> exact, order-free) ---
    for (int z = 0; z < res; ++z) for (int x = 0; x < res; ++x) {
        const int i = IDX(x, z);
        if (x + 1 < res) { const std::int32_t q = flux[2 * i + 0]; depth[i] -= q; depth[IDX(x + 1, z)] += q; }
        if (z + 1 < res) { const std::int32_t q = flux[2 * i + 1]; depth[i] -= q; depth[IDX(x, z + 1)] += q; }
    }

    // --- Sinks (sea/edge) + evaporation + defensive clamp + float render mirror ---
    std::int32_t max_delta = 0;
    const bool have_mirror = (static_cast<int>(c.water_level_data.size()) == n);
    for (int z = 0; z < res; ++z) for (int x = 0; x < res; ++x) {
        const int i = IDX(x, z);
        if (depth[i] < 0) depth[i] = 0; // defensive; the K-clamp should already guarantee this
        if (bed[i] <= sea_mm) { // ocean is an infinite SINK: settle the surface at sea level
            const std::int32_t target = sea_mm - bed[i]; // depth giving surface == sea level (>= 0)
            if (depth[i] > target) { out_sink += (depth[i] - target); depth[i] = target; }
        } else if (depth[i] > 0 && depth[i] < EVAP_MM) { // cull sub-mm films on dry land
            out_sink += depth[i]; depth[i] = 0;
        }
        std::int32_t dl = depth[i] - depth_before[i]; if (dl < 0) dl = -dl;
        if (dl > max_delta) max_delta = dl;
        if (have_mirror) c.water_level_data[i] = static_cast<float>(bed[i] + depth[i]) / static_cast<float>(MM_PER_M);
    }
    c.max_water_delta_last_tick = static_cast<float>(max_delta) / static_cast<float>(MM_PER_M);
    if (max_delta > 1) {
        c.water_mesh_dirty_ticks++;
        if (c.water_mesh_dirty_ticks >= WATER_MESH_DIRTY_TICK_INTERVAL) {
            c.water_mesh_generated.store(false);
            c.water_mesh_dirty_ticks = 0;
        }
    }
}

} // namespace

WaterSystem::WaterSystem(JobSystem* job_system, SHIELD_WorldSystem* shield_system)
    : m_job_system(job_system), m_shield_system(shield_system) {}

void WaterSystem::set_camera_entity(EntityID camera_entity) {
    m_camera_entity = camera_entity;
}

Vec3 WaterSystem::get_camera_position(entt::registry& registry) const {
    if (m_camera_entity != entt::null && registry.valid(m_camera_entity)) {
        if (const auto* transform = registry.try_get<const Components::TransformComponent>(m_camera_entity)) {
            return transform->position;
        }
    }

    auto camera_view = registry.view<const Components::TransformComponent, const Components::ActiveCameraComponent>();
    if (camera_view.begin() != camera_view.end()) {
        return camera_view.get<const Components::TransformComponent>(camera_view.front()).position;
    }

    return Vec3(0.0f);
}

void WaterSystem::update(entt::registry& registry, const std::unordered_map<ChunkID, std::shared_ptr<Chunk>>& active_chunks) {
    m_active_chunks = &active_chunks;
    if (m_active_chunks->empty()) return;

    // --- ADAPTIVE WATER GRID SYSTEM INTEGRATION ---
    Vec3 camera_position = get_camera_position(registry);

    // Apply adaptive resolution to active water chunks. spec 008 follow-up: each resolution change
    // re-samples GetTerrainHeightAt per cell (~30us, up to 1024 cells at Ultra), so a burst of chunks
    // crossing LOD boundaries at once when moving spiked this loop to ~180ms. Collect the chunks that
    // actually need a resize, then apply a DETERMINISTIC per-tick budget (sorted by chunk id, rotating
    // window via m_water_resize_cursor) — identical amortization to the init/sim caps. Deferred chunks
    // keep their current resolution one more tick and resize over the next few ticks; the selection is
    // timing-independent (id-sorted), so run==replay / host==peer hold (WaterDeterminism gate).
    struct ResizeRequest { Chunk* chunk; ChunkID id; WaterDetailLevel level; };
    std::vector<ResizeRequest> resize_requests;
    for (const auto& active_chunk : active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (chunk_ptr && chunk_ptr->has_water_sim.load()) {
            Vec3 chunk_center = Vec3(chunk_ptr->get_coords()) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z) + Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z) * 0.5f;
            float camera_distance = glm::length(camera_position - chunk_center);

            // Check if player is interacting with water in this chunk (simplified check)
            bool has_player_interaction = camera_distance < 10.0f; // Player very close = likely interacting

            // Calculate required detail level
            WaterDetailLevel required_detail = CalculateRequiredDetail(*chunk_ptr, camera_distance, has_player_interaction);

            // Only the chunks whose grid resolution must actually change are resize candidates
            // (Off is left to the existing path — never disabled here, preserving prior behaviour).
            if (required_detail != WaterDetailLevel::Off &&
                static_cast<int>(required_detail) != chunk_ptr->current_water_resolution.load()) {
                resize_requests.push_back({chunk_ptr.get(), chunk_ptr->get_id(), required_detail});
            }
        }
    }
    if (!resize_requests.empty()) {
        std::sort(resize_requests.begin(), resize_requests.end(),
                  [](const ResizeRequest& a, const ResizeRequest& b) { return a.id < b.id; });
        const std::size_t total = resize_requests.size();
        m_water_resize_cursor %= total;
        const std::size_t budget = std::min<std::size_t>(MAX_WATER_RESIZES_PER_TICK, total);
        for (std::size_t i = 0; i < budget; ++i) {
            const ResizeRequest& req = resize_requests[(m_water_resize_cursor + i) % total];
            ResizeSimulationGrid(*req.chunk, req.level);
        }
        m_water_resize_cursor = (m_water_resize_cursor + budget) % total;
    }

    auto source_view = registry.view<const Components::TransformComponent, const Components::WaterSourceComponent>();

    int water_inits_this_tick = 0;
    for (const auto& active_chunk : active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (!chunk_ptr) {
            continue;
        }

        if (!chunk_ptr->has_water_sim.load()) {
            // spec 008 follow-up: time-slice first-time water init so a burst of newly-streamed
            // water chunks doesn't spike the tick. Deferred chunks stay un-inited (has_water_sim
            // false) and are picked up over the next ticks.
            if (water_inits_this_tick >= MAX_WATER_INITS_PER_TICK) {
                continue;
            }
            ++water_inits_this_tick;
            // Spec 009 Phase 3: init directly at the fixed camera-independent sim resolution (no later
            // camera-driven resize -> the hashed grid is uniform + stable on every peer).
            int initial_resolution = WATER_SIM_RESOLUTION;
            chunk_ptr->current_water_resolution.store(initial_resolution);
            
            const size_t sim_size = initial_resolution * initial_resolution;
            chunk_ptr->water_level_data.assign(sim_size, SEA_LEVEL);
            chunk_ptr->water_flow_data.assign(sim_size, Vec2(0.0f));
            chunk_ptr->water_sim_terrain_height.resize(sim_size);
            chunk_ptr->water_rest_level.assign(sim_size, SEA_LEVEL);
            // Spec 009: fixed-point flowing-water state (HASHED). bed = terrain, depth = standing water
            // above bed (lakes/sea start filled, dry land + perched river channels start dry and fill
            // from the river sources). edge flux starts at rest.
            chunk_ptr->water_depth_mm.assign(sim_size, 0);
            chunk_ptr->water_bed_mm.assign(sim_size, 0);
            chunk_ptr->water_edge_flux.assign(2 * sim_size, 0);
            const IVec3 c_coords = chunk_ptr->get_coords();
            const float cell_width_x = CHUNK_SIZE_X / (float)initial_resolution;
            const float cell_width_z = CHUNK_SIZE_Z / (float)initial_resolution;

            for (int z = 0; z < initial_resolution; ++z) {
                for (int x = 0; x < initial_resolution; ++x) {
                    float world_x = c_coords.x * CHUNK_SIZE_X + (x + 0.5f) * cell_width_x;
                    float world_z = c_coords.z * CHUNK_SIZE_Z + (z + 0.5f) * cell_width_z;
                    const int cell = z * initial_resolution + x;
                    // Seed the resting water surface from the worldgen: sea level
                    // everywhere, raised to the local lake surface inside basins so
                    // perched lakes start (and, via the rest-level clamp below, stay)
                    // filled at their basin elevation.
                    const float rest = m_shield_system->WaterLevelAt(world_x, world_z);
                    const float terrain = m_shield_system->GetTerrainHeightAt(world_x, world_z);
                    chunk_ptr->water_level_data[cell] = rest;
                    chunk_ptr->water_rest_level[cell] = rest;
                    chunk_ptr->water_sim_terrain_height[cell] = terrain;
                    // Spec 009 fixed-point seed (mm): bed = terrain; depth = standing water above bed.
                    chunk_ptr->water_bed_mm[cell] = static_cast<std::int32_t>(std::lround(terrain * MM_PER_M));
                    const long depth0 = std::lround(static_cast<double>(rest - terrain) * MM_PER_M);
                    chunk_ptr->water_depth_mm[cell] = static_cast<std::int32_t>(depth0 > 0 ? depth0 : 0);
                }
            }
            chunk_ptr->has_water_sim.store(true);
            chunk_ptr->water_mesh_generated.store(false);
            chunk_ptr->water_mesh_dirty_ticks = 0;
        }
    }

    for (auto entity : source_view) {
        auto& transform = source_view.get<const Components::TransformComponent>(entity);
        auto& source = source_view.get<const Components::WaterSourceComponent>(entity);
        IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords(transform.position);
        auto it = m_active_chunks->find(Chunk::calculate_id(chunk_coords));
        if (it != m_active_chunks->end() && it->second) {
            Chunk& chunk = *it->second;
            const int resolution = GetWaterResolution(chunk);
            if (!HasCompleteWaterGrid(chunk, resolution)) {
                continue;
            }

            IVec3 local_pos = IVec3(transform.position) - (chunk_coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z));
            int sim_x = (local_pos.x * resolution) / CHUNK_SIZE_X;
            int sim_z = (local_pos.z * resolution) / CHUNK_SIZE_Z;
            int index = std::clamp(sim_z, 0, resolution - 1) * resolution + std::clamp(sim_x, 0, resolution - 1);
            float cell_area = (static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(resolution))
                * (static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(resolution));
            float water_added = source.flow_rate * SECONDS_PER_TICK / cell_area;
            if (water_added > 0.0f) {
                chunk.water_level_data[index] += water_added;
                // WAKE UP: An internal event occurred in this chunk.
                chunk.is_water_sleeping.store(false, std::memory_order_relaxed);
            }
        }
    }

    // --- Step 2: Wake up sleeping chunks that are adjacent to active ones (propagation) ---
    for (const auto& active_chunk : *m_active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (!chunk_ptr) {
            continue;
        }

        if (chunk_ptr->is_water_sleeping.load(std::memory_order_relaxed)) {
            IVec3 self_coords = chunk_ptr->get_coords();
            const IVec3 neighbor_offsets[] = {{0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}};
            for (const auto& offset : neighbor_offsets) {
                IVec3 neighbor_coords = self_coords + offset;
                auto it = m_active_chunks->find(Chunk::calculate_id(neighbor_coords));
                if (it != m_active_chunks->end() && it->second && !it->second->is_water_sleeping.load(std::memory_order_relaxed)) {
                    // WAKE UP: A neighbor is active, so this chunk must also become active.
                    chunk_ptr->is_water_sleeping.store(false, std::memory_order_relaxed);
                    break; 
                }
            }
        }
    }
    
    // --- Step 3: Collect only the active chunks for simulation ---
    std::vector<Chunk*> chunks_to_sim;
    chunks_to_sim.reserve(m_active_chunks->size());
    for (const auto& active_chunk : *m_active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (!chunk_ptr) {
            continue;
        }

        if (chunk_ptr->has_water_sim.load() && !chunk_ptr->is_water_sleeping.load(std::memory_order_relaxed)) {
            chunks_to_sim.push_back(chunk_ptr.get());
        }
    }

    // spec 008 follow-up: bound the per-tick sim work to MAX_WATER_SIMS_PER_TICK via a DETERMINISTIC
    // rotating window. Sort by chunk id (order independent of the unordered_map's iteration), then take
    // a window starting at m_water_sim_cursor and advance it — so over a few ticks every active chunk
    // sims, but no single tick blocks on hundreds of chunks. The un-simulated chunks stay awake and are
    // picked up by the rotation next tick. Cursor evolution is a pure function of the active-set size
    // sequence -> run==replay / host==peer identical (verified by the WaterDeterminism gate).
    if (chunks_to_sim.size() > MAX_WATER_SIMS_PER_TICK) {
        std::sort(chunks_to_sim.begin(), chunks_to_sim.end(),
                  [](const Chunk* a, const Chunk* b) { return a->get_id() < b->get_id(); });
        const std::size_t total = chunks_to_sim.size();
        m_water_sim_cursor %= total;
        std::vector<Chunk*> window;
        window.reserve(MAX_WATER_SIMS_PER_TICK);
        for (std::size_t i = 0; i < MAX_WATER_SIMS_PER_TICK; ++i) {
            window.push_back(chunks_to_sim[(m_water_sim_cursor + i) % total]);
        }
        m_water_sim_cursor = (m_water_sim_cursor + MAX_WATER_SIMS_PER_TICK) % total;
        chunks_to_sim.swap(window);
    }

    // --- Step 4: Spec 009 — INTEGER virtual-pipes. Per chunk: internal-edge flux + sources/sinks
    // (StepChunkWaterFixed). Then a CROSS-CHUNK owner-edge shared-flux pass so rivers/lakes are
    // CONTINUOUS + mass-conserved across chunk seams (Phase 3). Mass invariant (AC-2): over the simmed
    // set Σdepth changes by exactly (Σsource − Σsink); the cross-chunk pass is pure symmetric transfer
    // WITHIN the set -> nets to zero in Σdepth. FIXED order (id-sorted) so cross-chunk apply is deterministic.
    std::sort(chunks_to_sim.begin(), chunks_to_sim.end(),
              [](const Chunk* a, const Chunk* b) { return a->get_id() < b->get_id(); });
    std::unordered_set<const Chunk*> sim_set(chunks_to_sim.begin(), chunks_to_sim.end());

    std::int64_t depth_before = 0;
    for (Chunk* chunk : chunks_to_sim) {
        for (std::int32_t d : chunk->water_depth_mm) depth_before += d;
    }
    std::int64_t src_mm = 0, sink_mm = 0;
    for (Chunk* chunk : chunks_to_sim) {
        StepChunkWaterFixed(*chunk, *m_shield_system, src_mm, sink_mm);
    }

    // Cross-chunk owner-edge shared flux. Each chunk OWNS its +X (east) and +Z (north) boundary edges;
    // it reads the neighbour's POST-internal bed+depth, computes the same pipe flux as an internal edge,
    // clamps to the SOURCE (higher) cell's available depth (sequential -> never negative), and applies it
    // SYMMETRICALLY across the seam (A -= q; B += q -> mass conserved). Both chunks must be in the sim set
    // so the transfer stays inside the Σdepth sum. The boundary flux persists in the owner's edge_flux slot
    // (the +X slot of x=res-1 / +Z slot of z=res-1, which the internal step leaves at 0). Integer + fixed
    // id-sorted order -> bit-exact host==peer / run==replay.
    int seam_wet_pairs = 0; // CONTINUITY proof: seam cell-pairs with water on BOTH sides of the border
    for (Chunk* A : chunks_to_sim) {
        const int res = GetWaterResolution(*A);
        if (res <= 1) continue;
        const int nA = res * res;
        if (static_cast<int>(A->water_depth_mm.size()) != nA ||
            static_cast<int>(A->water_edge_flux.size()) != 2 * nA) continue;
        const IVec3 ac = A->get_coords();
        const bool have_mirror_A = (static_cast<int>(A->water_level_data.size()) == nA);
        auto step_face = [&](Chunk* B, bool isX) {
            if (GetWaterResolution(*B) != res) return; // uniform-res only -> cells align cell-for-cell
            if (static_cast<int>(B->water_depth_mm.size()) != nA) return;
            const bool have_mirror_B = (static_cast<int>(B->water_level_data.size()) == nA);
            for (int t = 0; t < res; ++t) {
                int ai, bi, slot;
                if (isX) { ai = t * res + (res - 1); bi = t * res + 0; slot = 2 * ai + 0; }
                else     { ai = (res - 1) * res + t; bi = 0 * res + t; slot = 2 * ai + 1; }
                const std::int64_t surfA = static_cast<std::int64_t>(A->water_bed_mm[ai]) + A->water_depth_mm[ai];
                const std::int64_t surfB = static_cast<std::int64_t>(B->water_bed_mm[bi]) + B->water_depth_mm[bi];
                const std::int64_t dSurf = surfA - surfB;
                std::int64_t q = static_cast<std::int64_t>(A->water_edge_flux[slot]) + ((K_ACCEL * dSurf) >> FLOW_SHIFT);
                q = (q * FRICTION_NUM) >> FRICTION_SHIFT;
                if (dSurf < MIN_FLOW_MM && dSurf > -MIN_FLOW_MM) q = 0;
                if (q >  2000000000LL) q =  2000000000LL;
                if (q < -2000000000LL) q = -2000000000LL;
                if (q > 0) { if (q >  A->water_depth_mm[ai]) q =  A->water_depth_mm[ai]; }
                else if (q < 0) { if (-q > B->water_depth_mm[bi]) q = -static_cast<std::int64_t>(B->water_depth_mm[bi]); }
                A->water_edge_flux[slot] = static_cast<std::int32_t>(q);
                A->water_depth_mm[ai] -= static_cast<std::int32_t>(q);
                B->water_depth_mm[bi] += static_cast<std::int32_t>(q);
                if (A->water_depth_mm[ai] > 0 && B->water_depth_mm[bi] > 0) ++seam_wet_pairs;
                if (q != 0) {
                    A->is_water_sleeping.store(false, std::memory_order_relaxed);
                    B->is_water_sleeping.store(false, std::memory_order_relaxed);
                    if (have_mirror_A) A->water_level_data[ai] = static_cast<float>(A->water_bed_mm[ai] + A->water_depth_mm[ai]) / static_cast<float>(MM_PER_M);
                    if (have_mirror_B) B->water_level_data[bi] = static_cast<float>(B->water_bed_mm[bi] + B->water_depth_mm[bi]) / static_cast<float>(MM_PER_M);
                }
            }
        };
        if (auto itE = m_active_chunks->find(Chunk::calculate_id(ac + IVec3(1, 0, 0)));
            itE != m_active_chunks->end() && itE->second && sim_set.count(itE->second.get())) step_face(itE->second.get(), true);
        if (auto itN = m_active_chunks->find(Chunk::calculate_id(ac + IVec3(0, 0, 1)));
            itN != m_active_chunks->end() && itN->second && sim_set.count(itN->second.get())) step_face(itN->second.get(), false);
    }

    std::int64_t depth_after = 0;
    for (Chunk* chunk : chunks_to_sim) {
        for (std::int32_t d : chunk->water_depth_mm) depth_after += d;
    }
    m_dbg_last_source_mm = src_mm;
    m_dbg_last_sink_mm = sink_mm;
    m_dbg_mass_ok = ((depth_after - depth_before) == (src_mm - sink_mm));
    m_dbg_seam_wet_pairs = seam_wet_pairs;

    // --- Step 5: Update sleep states for chunks that were just simulated ---
    const int TICKS_TO_SLEEP = 120;       // ~4 seconds at 30 ticks per second
    const float SLEEP_THRESHOLD = 0.0001f; // The max water level change to be considered "calm"

    for (Chunk* chunk : chunks_to_sim) {
        if (chunk->max_water_delta_last_tick < SLEEP_THRESHOLD) {
            chunk->ticks_below_threshold++;
            if (chunk->ticks_below_threshold >= TICKS_TO_SLEEP) {
                // GO TO SLEEP: Chunk has been calm for long enough.
                chunk->is_water_sleeping.store(true, std::memory_order_relaxed);
            }
        } else {
            // STAY AWAKE: Chunk is active, reset its calm counter.
            chunk->ticks_below_threshold = 0;
            chunk->is_water_sleeping.store(false, std::memory_order_relaxed);
        }
    }
}

void WaterSystem::dispatch_simulation_jobs(const std::vector<Chunk*>& chunks_to_simulate) {
    if (!m_active_chunks) {
        return;
    }

    std::unordered_map<ChunkID, WaterChunkSnapshot> water_snapshots;

    // spec 008 follow-up: build snapshots ONLY for the chunks we will simulate + their 4 XZ neighbors
    // (the only chunks the sim reads, via find_neighbor). The previous loop snapshotted EVERY active
    // chunk — 3 vector copies per chunk over the WHOLE active set — which cost ~345ms when moving into
    // water-heavy terrain regardless of the sim cap. Scoping to needed chunks is determinism-NEUTRAL:
    // the simulated chunks get the identical inputs (their + their neighbors' snapshots) they got
    // before; the unused snapshots of non-neighbor chunks were never read. Bounded by the sim cap.
    std::unordered_set<ChunkID> needed;
    needed.reserve(chunks_to_simulate.size() * 5u);
    for (const Chunk* c : chunks_to_simulate) {
        if (!c) continue;
        const IVec3 co = c->get_coords();
        needed.insert(c->get_id());
        needed.insert(Chunk::calculate_id(co + IVec3(0, 0, 1)));
        needed.insert(Chunk::calculate_id(co + IVec3(0, 0, -1)));
        needed.insert(Chunk::calculate_id(co + IVec3(1, 0, 0)));
        needed.insert(Chunk::calculate_id(co + IVec3(-1, 0, 0)));
    }
    water_snapshots.reserve(needed.size());
    for (const ChunkID id : needed) {
        const auto it = m_active_chunks->find(id);
        if (it == m_active_chunks->end() || !it->second) {
            continue;
        }
        const Chunk& chunk = *it->second;
        const int resolution = GetWaterResolution(chunk);
        if (!HasCompleteWaterGrid(chunk, resolution)) {
            continue;
        }

        const size_t cell_count = GetWaterCellCount(resolution);
        WaterChunkSnapshot snapshot;
        snapshot.id = chunk.get_id();
        snapshot.coords = chunk.get_coords();
        snapshot.resolution = resolution;
        snapshot.water_levels.assign(chunk.water_level_data.begin(), chunk.water_level_data.begin() + cell_count);
        snapshot.flow_data.assign(chunk.water_flow_data.begin(), chunk.water_flow_data.begin() + cell_count);
        snapshot.terrain_height.assign(chunk.water_sim_terrain_height.begin(), chunk.water_sim_terrain_height.begin() + cell_count);
        water_snapshots.emplace(snapshot.id, std::move(snapshot));
    }

    struct WaterSimulationTask {
        Chunk* chunk = nullptr;
        const WaterChunkSnapshot* snapshot = nullptr;
        WaterSimNeighbors neighbors;
        WaterChunkSimulationOutput output;
    };

    std::vector<WaterSimulationTask> tasks;
    tasks.reserve(chunks_to_simulate.size());

    for (Chunk* chunk : chunks_to_simulate) {
        if (!chunk) {
            continue;
        }

        auto snapshot_it = water_snapshots.find(chunk->get_id());
        if (snapshot_it == water_snapshots.end()) {
            continue;
        }

        const WaterChunkSnapshot& snapshot = snapshot_it->second;
        WaterSimulationTask task;
        task.chunk = chunk;
        task.snapshot = &snapshot;
        task.output.water_levels.resize(GetWaterCellCount(snapshot.resolution));
        task.output.flow_data.resize(GetWaterCellCount(snapshot.resolution));

        auto find_neighbor = [&](int dx, int dz) -> const WaterChunkSnapshot* {
            IVec3 neighbor_coords = snapshot.coords + IVec3(dx, 0, dz);
            auto active_it = m_active_chunks->find(Chunk::calculate_id(neighbor_coords));
            if (active_it == m_active_chunks->end() || !active_it->second) {
                return nullptr;
            }

            auto neighbor_snapshot_it = water_snapshots.find(active_it->second->get_id());
            return neighbor_snapshot_it != water_snapshots.end() ? &neighbor_snapshot_it->second : nullptr;
        };

        task.neighbors.north = find_neighbor(0, 1);
        task.neighbors.south = find_neighbor(0, -1);
        task.neighbors.east  = find_neighbor(1, 0);
        task.neighbors.west  = find_neighbor(-1, 0);

        tasks.push_back(std::move(task));
    }

    // spec 008 follow-up (streaming-burst amortization): run the per-chunk water sim INLINE on the
    // main thread instead of dispatching to the shared JobSystem and blocking on wait(). Each task
    // reads only immutable snapshots and writes its OWN output (no cross-chunk shared state), so
    // sequential execution is BYTE-IDENTICAL to the parallel batch — run==replay / host==peer hold
    // (guarded by the WaterDeterminism live-water gate). The dispatch+wait was the moving-water
    // killer: 18 trivial water jobs took ~1300ms because wait() blocked behind the flood of
    // streaming/meshing jobs already queued on the same JobSystem (classic head-of-line blocking).
    // The water compute itself is only a few ms (4 neighbour reads + a few flops per cell), so
    // inlining removes the stall entirely with no determinism or correctness change.
    for (WaterSimulationTask& task : tasks) {
        simulate_chunk_water(*task.snapshot, task.neighbors, task.output);
    }

    for (WaterSimulationTask& task : tasks) {
        task.chunk->water_level_data = std::move(task.output.water_levels);
        // Pin to the worldgen rest level: the flow sim may ripple a cell ABOVE its
        // rest surface but never drain it below, so perched lakes (rest > sea level)
        // stay filled at their basin elevation instead of flowing downhill/out.
        const std::vector<f32>& rest = task.chunk->water_rest_level;
        std::vector<f32>& lvl = task.chunk->water_level_data;
        if (rest.size() == lvl.size()) {
            for (std::size_t i = 0; i < lvl.size(); ++i) {
                if (lvl[i] < rest[i]) lvl[i] = rest[i];
            }
        }
        task.chunk->water_flow_data = std::move(task.output.flow_data);
        task.chunk->max_water_delta_last_tick = task.output.max_delta;
        if (task.output.max_delta > 1.0e-4f && task.chunk->water_mesh_generated.load(std::memory_order_relaxed)) {
            task.chunk->water_mesh_dirty_ticks++;
        }
        if (task.chunk->water_mesh_dirty_ticks >= WATER_MESH_DIRTY_TICK_INTERVAL) {
            task.chunk->water_mesh_generated.store(false);
            task.chunk->water_mesh_dirty_ticks = 0;
        }
    }
}

void WaterSystem::simulate_chunk_water(const WaterChunkSnapshot& snapshot, const WaterSimNeighbors& neighbors, WaterChunkSimulationOutput& output) {
    const int resolution = snapshot.resolution;
    if (resolution <= 1) {
        return;
    }

    const size_t cell_count = GetWaterCellCount(resolution);
    output.water_levels.resize(cell_count);
    output.flow_data.resize(cell_count);

    float max_delta_this_tick = 0.0f;
    const IVec2 neighbors_offset[4] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};

    auto sample_neighbor_edge = [&](const WaterChunkSnapshot* neighbor, int x, int z, int dx, int dz) -> float {
        if (!neighbor || neighbor->resolution <= 1) {
            return SEA_LEVEL;
        }

        const int neighbor_resolution = neighbor->resolution;
        if (neighbor->water_levels.size() < GetWaterCellCount(neighbor_resolution)) {
            return SEA_LEVEL;
        }

        int sample_x = 0;
        int sample_z = 0;

        if (dx < 0) {
            sample_x = neighbor_resolution - 1;
            sample_z = std::clamp(static_cast<int>(((z + 0.5f) / resolution) * neighbor_resolution), 0, neighbor_resolution - 1);
        } else if (dx > 0) {
            sample_x = 0;
            sample_z = std::clamp(static_cast<int>(((z + 0.5f) / resolution) * neighbor_resolution), 0, neighbor_resolution - 1);
        } else if (dz < 0) {
            sample_x = std::clamp(static_cast<int>(((x + 0.5f) / resolution) * neighbor_resolution), 0, neighbor_resolution - 1);
            sample_z = neighbor_resolution - 1;
        } else {
            sample_x = std::clamp(static_cast<int>(((x + 0.5f) / resolution) * neighbor_resolution), 0, neighbor_resolution - 1);
            sample_z = 0;
        }

        return neighbor->water_levels[sample_z * neighbor_resolution + sample_x];
    };

    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            const int idx_center = z * resolution + x;
            const float h_center = snapshot.water_levels[idx_center];
            
            float total_outflow = 0.0f;
            float total_inflow = 0.0f;
            Vec2 new_flow_vector = {0.0f, 0.0f};

            for(int i = 0; i < 4; ++i) {
                int nx = x + neighbors_offset[i].x;
                int nz = z + neighbors_offset[i].y;
                float h_neighbor = SEA_LEVEL;

                if (nx >= 0 && nx < resolution && nz >= 0 && nz < resolution) {
                    h_neighbor = snapshot.water_levels[nz * resolution + nx];
                } else if (nx < 0) {
                    h_neighbor = sample_neighbor_edge(neighbors.west, x, z, -1, 0);
                } else if (nx >= resolution) {
                    h_neighbor = sample_neighbor_edge(neighbors.east, x, z, 1, 0);
                } else if (nz < 0) {
                    h_neighbor = sample_neighbor_edge(neighbors.south, x, z, 0, -1);
                } else if (nz >= resolution) {
                    h_neighbor = sample_neighbor_edge(neighbors.north, x, z, 0, 1);
                }

                float diff = h_center - h_neighbor;
                
                if (diff > MIN_FLOW_DIFF) {
                    float outflow = diff * FLOW_CONSTANT;
                    total_outflow += outflow;
                    new_flow_vector += Vec2(neighbors_offset[i]) * outflow;
                } else if (-diff > MIN_FLOW_DIFF) {
                    float inflow = -diff * FLOW_CONSTANT;
                    total_inflow += inflow;
                }
            }
            
            const float terrain_h = snapshot.terrain_height[idx_center];
            const float water_depth = h_center - terrain_h;

            if (water_depth > 0) {
                 total_outflow = std::min(total_outflow, water_depth * MAX_WATER_COMPRESSION);
            } else {
                 total_outflow = 0;
            }
            
            float final_height = h_center - total_outflow + total_inflow;
            output.water_levels[idx_center] = final_height;
            
            // <<< OPTIMIZATION: Measure water activity >>>
            const float delta = std::abs(final_height - h_center);
            if (delta > max_delta_this_tick) {
                max_delta_this_tick = delta;
            }
            
            output.flow_data[idx_center] = glm::mix(snapshot.flow_data[idx_center], new_flow_vector, 0.5f);
        }
    }
    
    output.max_delta = max_delta_this_tick;
}

f32 WaterSystem::get_water_level_at(float world_x, float world_z) const {
    if (!m_active_chunks) return SEA_LEVEL;

    IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords({world_x, 0, world_z});
    auto it = m_active_chunks->find(Chunk::calculate_id(chunk_coords));
    if (it == m_active_chunks->end() || !it->second || !it->second->has_water_sim.load()) {
        return SEA_LEVEL;
    }

    const Chunk& chunk = *it->second;
    const int resolution = GetWaterResolution(chunk);
    if (!HasCompleteWaterGrid(chunk, resolution)) {
        return SEA_LEVEL;
    }
    
    Vec3 base_pos = Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
    float local_x = world_x - base_pos.x;
    float local_z = world_z - base_pos.z;
    
    float sim_xf = (local_x / CHUNK_SIZE_X) * resolution - 0.5f;
    float sim_zf = (local_z / CHUNK_SIZE_Z) * resolution - 0.5f;

    int x0 = static_cast<int>(sim_xf);
    int z0 = static_cast<int>(sim_zf);
    
    x0 = std::clamp(x0, 0, resolution - 2);
    z0 = std::clamp(z0, 0, resolution - 2);

    float tx = sim_xf - x0;
    float tz = sim_zf - z0;

    float h00 = chunk.water_level_data[z0 * resolution + x0];
    float h10 = chunk.water_level_data[z0 * resolution + (x0 + 1)];
    float h01 = chunk.water_level_data[(z0 + 1) * resolution + x0];
    float h11 = chunk.water_level_data[(z0 + 1) * resolution + (x0 + 1)];

    float h_z0 = glm::mix(h00, h10, tx);
    float h_z1 = glm::mix(h01, h11, tx);

    return glm::mix(h_z0, h_z1, tz);
}

Vec2 WaterSystem::get_water_flow_at(float world_x, float world_z) const {
    if (!m_active_chunks) {
        return Vec2(0.0f);
    }

    IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords({world_x, 0.0f, world_z});
    auto it = m_active_chunks->find(Chunk::calculate_id(chunk_coords));

    if (it == m_active_chunks->end() || !it->second || !it->second->has_water_sim.load()) {
        return Vec2(0.0f);
    }

    const Chunk& chunk = *it->second;
    const int resolution = GetWaterResolution(chunk);
    if (!HasCompleteWaterGrid(chunk, resolution)) {
        return Vec2(0.0f);
    }

    Vec3 base_pos = Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
    float local_x = world_x - base_pos.x;
    float local_z = world_z - base_pos.z;
    
    float sim_xf = (local_x / CHUNK_SIZE_X) * resolution;
    float sim_zf = (local_z / CHUNK_SIZE_Z) * resolution;

    int x0 = static_cast<int>(sim_xf);
    int z0 = static_cast<int>(sim_zf);
    float tx = sim_xf - x0;
    float tz = sim_zf - z0;
    
    x0 = std::clamp(x0, 0, resolution - 2);
    z0 = std::clamp(z0, 0, resolution - 2);
    int x1 = x0 + 1;
    int z1 = z0 + 1;

    const auto& flow_data = chunk.water_flow_data;
    const Vec2& f00 = flow_data[z0 * resolution + x0];
    const Vec2& f10 = flow_data[z0 * resolution + x1];
    const Vec2& f01 = flow_data[z1 * resolution + x0];
    const Vec2& f11 = flow_data[z1 * resolution + x1];

    Vec2 f_z0 = glm::lerp(f00, f10, tx);
    Vec2 f_z1 = glm::lerp(f01, f11, tx);
    
    return glm::lerp(f_z0, f_z1, tz);
}

void WaterSystem::apply_displacement(const Vec3& world_pos, f32 volume) {
    if (!m_active_chunks) return;
    
    const int radius = 2;
    const float total_cells = (2 * radius + 1) * (2 * radius + 1);
    
    IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords(world_pos);
    auto source_it = m_active_chunks->find(Chunk::calculate_id(chunk_coords));
    if (source_it == m_active_chunks->end() || !source_it->second) {
        return;
    }

    const int source_resolution = GetWaterResolution(*source_it->second);
    if (!HasCompleteWaterGrid(*source_it->second, source_resolution)) {
        return;
    }

    Vec3 base_chunk_pos = Vec3(chunk_coords * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z));
    float local_x = world_pos.x - base_chunk_pos.x;
    float local_z = world_pos.z - base_chunk_pos.z;
    int center_sim_x = static_cast<int>((local_x / CHUNK_SIZE_X) * source_resolution);
    int center_sim_z = static_cast<int>((local_z / CHUNK_SIZE_Z) * source_resolution);

    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            int current_sim_x = center_sim_x + dx;
            int current_sim_z = center_sim_z + dz;

            IVec3 target_chunk_coords = chunk_coords;
            target_chunk_coords.x += FloorDiv(current_sim_x, source_resolution);
            target_chunk_coords.z += FloorDiv(current_sim_z, source_resolution);

            int source_cell_x = PositiveMod(current_sim_x, source_resolution);
            int source_cell_z = PositiveMod(current_sim_z, source_resolution);

            auto it = m_active_chunks->find(Chunk::calculate_id(target_chunk_coords));
            if (it != m_active_chunks->end() && it->second) {
                Chunk& target_chunk = *it->second;
                const int target_resolution = GetWaterResolution(target_chunk);
                if (!HasCompleteWaterGrid(target_chunk, target_resolution)) {
                    continue;
                }

                int final_sim_x = std::clamp(static_cast<int>(((source_cell_x + 0.5f) / source_resolution) * target_resolution), 0, target_resolution - 1);
                int final_sim_z = std::clamp(static_cast<int>(((source_cell_z + 0.5f) / source_resolution) * target_resolution), 0, target_resolution - 1);
                
                float distance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
                float falloff = 1.0f - std::min(distance / (radius + 1.0f), 1.0f);
                falloff = falloff * falloff; // Cosine-like falloff

                const float target_cell_area = (static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(target_resolution))
                    * (static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(target_resolution));
                const float base_height_delta = volume / target_cell_area;
                int index = final_sim_z * target_resolution + final_sim_x;
                target_chunk.water_level_data[index] += (base_height_delta / total_cells) * falloff;

                // WAKE UP: An external event disturbed this chunk.
                target_chunk.is_water_sleeping.store(false, std::memory_order_relaxed);
            }
        }
    }
}

// Spec 009 Phase 2 — TERRAFORM the water bed (dig / dam). Adjusts the integer bed height
// (water_bed_mm) by delta_mm for every sim cell within radius_m of world_pos (delta<0 = dig a channel,
// delta>0 = raise a dam), then WAKES those chunks so the fixed-point solver re-routes next tick. The
// solver drains/pools FOR FREE because its flux reads only Δ(bed+depth) — no special drain path. The
// edit is deterministic (integer delta, applied in the sim tick; replicate the edit as a command for
// host==peer). The VOXEL/SDF terrain + its render mesh are a separate concern; this couples WATER to a
// bed change. Returns the number of cells edited.
int WaterSystem::EditTerrainBed(const Vec3& world_pos, std::int32_t delta_mm, float radius_m) {
    if (!m_active_chunks || delta_mm == 0) return 0;
    const float r2 = radius_m * radius_m;
    int edited_total = 0;
    for (const auto& kv : *m_active_chunks) {
        const std::shared_ptr<Chunk>& cptr = kv.second;
        if (!cptr) continue;
        Chunk& c = *cptr;
        const int res = GetWaterResolution(c);
        if (res <= 1 || static_cast<int>(c.water_bed_mm.size()) != res * res) continue;
        const IVec3 cc = c.get_coords();
        const float cw_x = static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(res);
        const float cw_z = static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(res);
        bool edited = false;
        for (int z = 0; z < res; ++z) {
            for (int x = 0; x < res; ++x) {
                const float wx = cc.x * CHUNK_SIZE_X + (x + 0.5f) * cw_x;
                const float wz = cc.z * CHUNK_SIZE_Z + (z + 0.5f) * cw_z;
                const float dx = wx - world_pos.x;
                const float dz = wz - world_pos.z;
                if (dx * dx + dz * dz <= r2) {
                    c.water_bed_mm[z * res + x] += delta_mm;
                    edited = true;
                    ++edited_total;
                }
            }
        }
        if (edited) {
            c.is_water_sleeping.store(false, std::memory_order_relaxed);
            c.water_mesh_generated.store(false);
            c.water_mesh_dirty_ticks = 0;
        }
    }
    return edited_total;
}

// --- ADAPTIVE WATER GRID SYSTEM IMPLEMENTATION ---

WaterDetailLevel WaterSystem::CalculateRequiredDetail(const Chunk& chunk, float camera_distance, bool has_player_interaction) {
    // Spec 009 Phase 3: the HASHED sim grid is CAMERA-INDEPENDENT — every simulated water chunk runs at
    // the single fixed WATER_SIM_RESOLUTION. A camera-driven resolution made the hashed mm water state
    // differ per peer (host!=peer) and misaligned cross-chunk boundary cells. Uniform resolution is
    // required so neighbour chunks exchange flux cell-for-cell. (Visual mesh LOD, if ever wanted, is a
    // separate render-only concern — it must NOT drive this hashed grid.)
    (void)chunk; (void)camera_distance; (void)has_player_interaction;
    return static_cast<WaterDetailLevel>(WATER_SIM_RESOLUTION);
}

void WaterSystem::ResizeSimulationGrid(Chunk& chunk, WaterDetailLevel new_level) {
    int new_resolution = static_cast<int>(new_level);
    int current_resolution = chunk.current_water_resolution.load();
    
    if (new_resolution == current_resolution) {
        return; // No change needed
    }
    
    if (new_resolution <= 0) { // Disable simulation
        chunk.water_level_data.clear();
        chunk.water_flow_data.clear();
        chunk.water_sim_terrain_height.clear();
        chunk.has_water_sim.store(false);
        chunk.water_mesh_generated.store(false);
        chunk.water_mesh_dirty_ticks = 0;
        chunk.current_water_resolution.store(0);
        return;
    }
    
    // Store old data for interpolation
    std::vector<f32> old_water_levels = chunk.water_level_data;
    std::vector<Vec2> old_flow_data = chunk.water_flow_data;
    const bool can_interpolate = current_resolution > 1
        && old_water_levels.size() >= GetWaterCellCount(current_resolution)
        && old_flow_data.size() >= GetWaterCellCount(current_resolution);
    
    // Resize arrays
    const size_t new_sim_size = new_resolution * new_resolution;
    chunk.water_level_data.resize(new_sim_size);
    chunk.water_flow_data.resize(new_sim_size);
    chunk.water_sim_terrain_height.resize(new_sim_size);
    
    // Regenerate terrain height data for new resolution
    const IVec3 c_coords = chunk.get_coords();
    const float cell_width_x = CHUNK_SIZE_X / (float)new_resolution;
    const float cell_width_z = CHUNK_SIZE_Z / (float)new_resolution;
    
    for (int z = 0; z < new_resolution; ++z) {
        for (int x = 0; x < new_resolution; ++x) {
            int idx = z * new_resolution + x;
            float world_x = c_coords.x * CHUNK_SIZE_X + (x + 0.5f) * cell_width_x;
            float world_z = c_coords.z * CHUNK_SIZE_Z + (z + 0.5f) * cell_width_z;
            chunk.water_sim_terrain_height[idx] = m_shield_system->GetTerrainHeightAt(world_x, world_z);
            
            // Bilinear interpolation from old grid
            if (can_interpolate) {
                float old_x = (x / (float)(new_resolution - 1)) * (current_resolution - 1);
                float old_z = (z / (float)(new_resolution - 1)) * (current_resolution - 1);
                
                int x0 = (int)std::floor(old_x);
                int z0 = (int)std::floor(old_z);
                int x1 = std::min(x0 + 1, current_resolution - 1);
                int z1 = std::min(z0 + 1, current_resolution - 1);
                
                float fx = old_x - x0;
                float fz = old_z - z0;
                
                // Sample old grid
                float w00 = old_water_levels[z0 * current_resolution + x0];
                float w10 = old_water_levels[z0 * current_resolution + x1];
                float w01 = old_water_levels[z1 * current_resolution + x0];
                float w11 = old_water_levels[z1 * current_resolution + x1];
                
                // Bilinear interpolation
                float w0 = w00 * (1.0f - fx) + w10 * fx;
                float w1 = w01 * (1.0f - fx) + w11 * fx;
                chunk.water_level_data[idx] = w0 * (1.0f - fz) + w1 * fz;
                
                // Similar interpolation for flow data
                Vec2 f00 = old_flow_data[z0 * current_resolution + x0];
                Vec2 f10 = old_flow_data[z0 * current_resolution + x1];
                Vec2 f01 = old_flow_data[z1 * current_resolution + x0];
                Vec2 f11 = old_flow_data[z1 * current_resolution + x1];
                
                Vec2 f0 = f00 * (1.0f - fx) + f10 * fx;
                Vec2 f1 = f01 * (1.0f - fx) + f11 * fx;
                chunk.water_flow_data[idx] = f0 * (1.0f - fz) + f1 * fz;
            } else {
                // Initialize with default values
                chunk.water_level_data[idx] = SEA_LEVEL;
                chunk.water_flow_data[idx] = Vec2(0.0f);
            }
        }
    }
    
    // Spec 009: resize + re-seed the fixed-point mm arrays (the HASHED sim state) to the new
    // resolution — otherwise StepChunkWaterFixed's size-guard bails and the chunk's water FREEZES
    // (the camera-near LOD-up made the mm arrays mismatch -> rivers rendered dry near the player).
    // bed = terrain (mm); depth = the just-resampled float surface mirror minus terrain (mm, >=0);
    // flux reset. Deterministic (pure function of terrain + the resampled surface).
    // NOTE: water resolution is camera-driven, so the hashed mm state is camera-dependent — fine for
    // single-player/run==replay, but TRUE host==peer needs a camera-independent sim resolution
    // (decouple sim grid from visual LOD) — spec 009 NFR-DET / Phase 3 architectural follow-up.
    chunk.water_depth_mm.assign(new_sim_size, 0);
    chunk.water_bed_mm.assign(new_sim_size, 0);
    chunk.water_edge_flux.assign(2 * new_sim_size, 0);
    for (int z = 0; z < new_resolution; ++z) {
        for (int x = 0; x < new_resolution; ++x) {
            const int idx = z * new_resolution + x;
            const float terr = chunk.water_sim_terrain_height[idx];
            chunk.water_bed_mm[idx] = static_cast<std::int32_t>(std::lround(terr * static_cast<float>(MM_PER_M)));
            const long d = std::lround((chunk.water_level_data[idx] - terr) * static_cast<float>(MM_PER_M));
            chunk.water_depth_mm[idx] = d > 0 ? static_cast<std::int32_t>(d) : 0;
        }
    }

    chunk.has_water_sim.store(true);
    chunk.water_mesh_generated.store(false);
    chunk.water_mesh_dirty_ticks = 0;
    chunk.current_water_resolution.store(new_resolution);
}

} // namespace Luminumbra::Systems
