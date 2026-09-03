#define GLM_ENABLE_EXPERIMENTAL

#include "WaterSystem.h"
#include "../components/CoreComponents.h"
#include "../core/Log.h"
#include "../core/WaterComponents.h"
#include "../world/Chunk.h"
#include "SHIELD_WorldSystem.h"
#include "WeatherSystem.h" // Weather-driven per-cell rain.
#include <algorithm>
#include <chrono> // (water-kernel perf) sub-phase wall timers — telemetry only, never hashed
#include <cmath>
#include <glm/gtx/compatibility.hpp>
#include <numeric>

namespace Luminumbra::Systems {

constexpr int WATER_MESH_DIRTY_TICK_INTERVAL =
    15; // responsive water mesh (render-only, not
        // hashed) so a filling river updates its surface ~0.5s
        // instead of 2s — visible flowing water, not stale mesh.
//  implementation note (streaming-burst amortization): cap how many chunks first-time-initialize
// their water sim grid per tick. Each init samples WaterLevelAt + GetTerrainHeightAt for every
// cell (resolution^2), so a moving camera that streams in many water chunks at once initialized
// ALL of them in one tick -> a 300ms+ spike (research: time-slice the streaming integration). The
// deferred chunks keep has_water_sim=false and init over the next ticks, draining the backlog. The
// cap is a DETERMINISTIC count (same active-chunk iteration order on host==peer), and each chunk's
// init result is order-independent (pure worldgen samples), so run==replay / host==peer hold.
constexpr int MAX_WATER_INITS_PER_TICK = 6;
//  implementation note: cap how much active water is SIMULATED per tick. The sim dispatches
// the woken chunks to workers then m_job_system->waits on completion (+ snapshot/integrate
// copies), all O(chunks_to_sim); moving into water-heavy terrain woke hundreds at once -> ~450ms
// main-thread block. We simulate a deterministic ROTATING window (sorted by chunk id, advanced each
// tick) so the per-tick work is bounded and every chunk still sims over a few ticks. Deterministic
// guarded by the WaterDeterminism live-water gate.
//  the budget is expressed in CELLS, not chunks, because per-tick sim cost scales with
// cell count (the kernel is O(resolution^2) per chunk). At the uniform Medium resolution every
// chunk is 64 cells, so 4096 cells == the historical 64-chunk window EXACTLY — the derived
// window length, cursor evolution and hash trajectory are unchanged. A future higher uniform
// resolution automatically shrinks the chunk window to keep per-tick cell work constant.
constexpr std::size_t MAX_WATER_CELLS_PER_TICK = 4096;
//  implementation note: cap how many water chunks RESIZE their sim grid per tick. Each resize
//  re-samples
// GetTerrainHeightAt per cell (~30us, up to 1024 at Ultra), so a burst of LOD-boundary crossings
// while moving spiked the adaptive loop to ~180ms. Same deterministic rotating-window amortization
// (sorted by chunk id) as the init/sim caps — bounded per-tick cost, every chunk still reaches its
// target res over a few ticks. Guarded by the WaterDeterminism live-water gate.
constexpr std::size_t MAX_WATER_RESIZES_PER_TICK = 1;

// ---: fixed-point virtual-pipes (Mei) flowing-water solver ----------------------------
// Integer-only so host==peer is BIT-exact (the water-state hash FNV-1a's the raw int32 bits). Depth
// and flux are in MILLIMETRES (per unit cell area -> flux and depth share mm units). The whole
// hashed path is integer: no float/libm/RNG/wall-clock. Mass is exact (one shared int32 flux
// subtracted from one cell, added to its neighbour).
constexpr std::int64_t MM_PER_M = 1000;
constexpr std::int32_t MIN_FLOW_MM =
    2; // sub-2mm surface diffs produce no flux (kills limit-cycle jitter)
constexpr std::int64_t K_ACCEL =
    256; // gain: q += (K_ACCEL*dSurf_mm) >> FLOW_SHIFT  (dSurf 1m -> +62mm)
constexpr int FLOW_SHIFT = 12;
constexpr std::int64_t
    FRICTION_NUM = 200,
    FRICTION_SHIFT = 8; // q = (q*200)>>8 ~ 0.78 (strong damping -> stable, modest steady flow)
constexpr std::int32_t RIVER_DISCHARGE_MM = 24; // mm/tick injected at a river-channel source cell
constexpr float RIVER_SOURCE_THRESHOLD = 0.45f; // RiverInfluenceAt >= this => source cell
constexpr std::int32_t EVAP_MM = 1; // cull <1mm films on non-source cells (deterministic)

//  the HASHED water-sim grid runs at ONE FIXED resolution for ALL simulated chunks,
// a DETERMINISTIC function of worldgen only (NOT camera distance). This is required for (a)
// host==peer — a camera-driven resolution made the hashed mm state differ per peer — and (b)
// cross-chunk flux, which needs neighbour chunks' boundary cells to align cell-for-cell.
// Chunks are 16 m across, so Medium (8x8) is 2 m cells and High (16x16 -> 1 m cells)
// resolves a river channel. Visual mesh LOD, if wanted, stays a separate  concern.
// The resolution lives in m_sim_resolution (default Medium); sim.water_high_res
// raises a session to High ONCE at construction via SetSimResolution — never mid-run.
// Perf: High (16) was ~4x and spiked the moving water phase to ~130ms; Medium is ~4x cheaper (~32ms
// worst) and still uniform+deterministic. Cross-chunk flux REQUIRES a uniform resolution (boundary
// cells must align). A mixed-resolution variant would require explicit seam resampling.

// Per-tick rotating sim-window length in CHUNKS, derived from the cell budget at the single
// uniform sim resolution. Medium (8x8 = 64 cells/chunk) -> 4096/64 = 64 chunks, identical to the
// historical fixed 64-chunk window, so the persisted waterSimCursor semantics and the hashed
// trajectory are untouched; only a resolution change re-derives it (High: 4096/256 = 16 chunks).
// Derived at runtime from m_sim_resolution in update() — at the Medium default the
// values (64 cells/chunk, 64-chunk window) and arithmetic are identical to the old
// constexpr pair, so the default trajectory is byte-identical.

namespace {

constexpr std::size_t DeriveSimWindowChunks(std::size_t cells_per_chunk) {
    return std::max<std::size_t>(std::size_t{1}, MAX_WATER_CELLS_PER_TICK / cells_per_chunk);
}

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
    return chunk.water_level_data.size() >= cell_count &&
           chunk.water_flow_data.size() >= cell_count &&
           chunk.water_sim_terrain_height.size() >= cell_count;
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

//  integer virtual-pipes step for ONE chunk (INTERNAL edges only — cross-chunk flux
// is ). Injects deterministic river sources, runs flux -> K outflow-clamp -> symmetric apply on
// the int32 depth/bed/edge_flux, applies the sea/edge sink + evaporation, and regenerates the float
// water_level_data mirror for the mesher. Accumulates per-tick source/sink mm into out_src/out_sink
// for the mass-conservation invariant. Everything that feeds the hash is integer + row-major
// fixed order -> bit-exact host==peer / run==replay.
// (water-kernel perf) out_depth_before/out_depth_after: in-kernel mass accumulators replacing
// the two whole-window Σdepth passes update() used to run around the kernel loop. Per chunk they
// accumulate Σ(depth at kernel entry) — the depth_before snapshot — and Σ(depth at kernel exit),
// read in the final per-cell pass where both arrays are already hot. These feed ONLY the
// debug_water_mass_ok bookkeeping (never hashed, never persisted), and integer addition is
// associative/commutative, so the totals are bit-exact regardless of summation order. The
// cross-chunk seam pass that runs between the kernels and the old post-pass is a symmetric
// transfer WITHIN the sim window (A -= q; B += q), so it nets to zero in Σdepth — the
// kernel-exit total equals the old post-seam total exactly. A chunk that early-returns here
// contributed equally to both of the old sums (delta zero), and contributes zero to both
// accumulators — the compared difference is identical.
void StepChunkWaterFixed(Chunk& c,
                         SHIELD_WorldSystem& shield,
                         std::int64_t& out_src,
                         std::int64_t& out_sink,
                         std::int64_t& out_depth_before,
                         std::int64_t& out_depth_after,
                         bool finite_hydrology,
                         std::int32_t rain_mm,
                         std::int32_t evap_mm,
                         const WeatherSystem* weather_rain = nullptr,
                         std::int32_t weather_scale_mm = 0) {
    const int res = GetWaterResolution(c);
    if (res <= 1)
        return;
    const int n = res * res;
    if (static_cast<int>(c.water_depth_mm.size()) != n ||
        static_cast<int>(c.water_bed_mm.size()) != n)
        return;
    if (static_cast<int>(c.water_edge_flux.size()) != 2 * n)
        c.water_edge_flux.assign(2 * n, 0);

    std::vector<std::int32_t>& depth = c.water_depth_mm;
    std::vector<std::int32_t>& bed = c.water_bed_mm;
    std::vector<std::int32_t>& flux = c.water_edge_flux;
    const IVec3 cc = c.get_coords();
    const float cw_x = static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(res);
    const float cw_z = static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(res);
    const std::int32_t sea_mm = 0; // SEA_LEVEL == 0
    auto IDX = [res](int x, int z) {
        return z * res + x;
    };

    // (Step 3) reuse scratch storage across calls (single-threaded, non-recursive sim) — fully
    // overwritten each call, so byte-identical. assign keeps capacity, dropping the per-tick
    // alloc.
    thread_local std::vector<std::int32_t> tl_depth_before;
    tl_depth_before.assign(depth.begin(), depth.end()); // for the activity delta + remesh signal
    const std::vector<std::int32_t>& depth_before = tl_depth_before;

    // --- : SOURCES (deterministic, pure function of position) ---
    //  FINITE HYDROLOGY: when finite_hydrology is on, water is a CONSERVED quantity — there is
    // NO perpetual river source. Bodies are fed by RAIN and emptied by drainage/evaporation, so a
    // basin you drain stays drained and depressions fill when it rains (real-life cycle). The
    // classic behaviour (perpetual river springs) is the finite_hydrology==false default.
    if (!finite_hydrology) {
        // (Step 2) The river source is a PURE function of cell position, so cache it once instead
        // of re-running the per-cell noise (RiverInfluenceAt) every tick. The size-guard rebuilds
        // the mask lazily on first sim / after a resize / for persistence-loaded chunks (which skip
        // the init loop) — same cell-centre formula, same threshold/discharge, same row order, so
        // the consumed result is BYTE-IDENTICAL to the per-tick computation (run==replay /
        // host==peer).
        if (static_cast<int>(c.water_src_mm.size()) != n) {
            c.water_src_mm.assign(n, 0);
            for (int z = 0; z < res; ++z)
                for (int x = 0; x < res; ++x) {
                    const float wx = cc.x * CHUNK_SIZE_X + (x + 0.5f) * cw_x;
                    const float wz = cc.z * CHUNK_SIZE_Z + (z + 0.5f) * cw_z;
                    if (shield.RiverInfluenceAt(wx, wz) >= RIVER_SOURCE_THRESHOLD) {
                        c.water_src_mm[IDX(x, z)] = RIVER_DISCHARGE_MM;
                    }
                }
        }
        for (int i = 0; i < n; ++i) {
            depth[i] += c.water_src_mm[i]; // += 0 for non-source cells (byte-identical)
            out_src += c.water_src_mm[i];
        }
    }
    // RAIN: deterministic uniform input (mm/tick; the caller scales it by the weather
    // precipitation). It lands on every column; the flux below carries it to the low spots, so
    // rainfall pools into puddles -> ponds -> lakes and runs off slopes toward the sea.
    if (rain_mm > 0) {
        for (int i = 0; i < n; ++i)
            depth[i] += rain_mm;
        out_src += static_cast<std::int64_t>(rain_mm) * n;
    }
    //  ==: WEATHER-DRIVEN per-cell rain. Precipitation is
    // sampled at the cell CENTRE and integer-quantized AT THE BOUNDARY (the only
    // float->int crossing; row-order iteration -> deterministic), then joins the mm
    // domain like the uniform rain above. weather_rain == null (the default) touches
    // nothing — byte-identical to pre-. The weather state is the one the weather
    // core produced earlier THIS tick (fixed 0-tick phase, the scent/wind convention).
    if (weather_rain != nullptr && weather_scale_mm > 0) {
        for (int z = 0; z < res; ++z) {
            for (int x = 0; x < res; ++x) {
                const float wx = cc.x * CHUNK_SIZE_X + (x + 0.5f) * cw_x;
                const float wz = cc.z * CHUNK_SIZE_Z + (z + 0.5f) * cw_z;
                const float p = weather_rain->PrecipitationAt(Vec3(wx, 0.0f, wz));
                const std::int32_t r =
                    static_cast<std::int32_t>(p * static_cast<float>(weather_scale_mm) + 0.5f);
                if (r > 0) {
                    depth[IDX(x, z)] += r;
                    out_src += r;
                }
            }
        }
    }

    // --- : FLUX on +X and +Z internal edges, from the read-only surface snapshot ---
    // (Step 3) thread_local scratch, resize-without-shrink; all n entries written below, so
    // byte-identical to a fresh value-initialized vector.
    thread_local std::vector<std::int64_t> surf;
    surf.resize(n);
    for (int i = 0; i < n; ++i)
        surf[i] = static_cast<std::int64_t>(bed[i]) + depth[i];
    auto compute_edge = [&](int i, int j, int eidx) {
        const std::int64_t dSurf = surf[i] - surf[j];
        std::int64_t q = static_cast<std::int64_t>(flux[eidx]) + ((K_ACCEL * dSurf) >> FLOW_SHIFT);
        q = (q * FRICTION_NUM) >> FRICTION_SHIFT;
        if (dSurf < MIN_FLOW_MM && dSurf > -MIN_FLOW_MM)
            q = 0;
        if (q > 2000000000LL)
            q = 2000000000LL;
        if (q < -2000000000LL)
            q = -2000000000LL;
        flux[eidx] = static_cast<std::int32_t>(q);
    };
    for (int z = 0; z < res; ++z)
        for (int x = 0; x < res; ++x) {
            const int i = IDX(x, z);
            if (x + 1 < res)
                compute_edge(i, IDX(x + 1, z), 2 * i + 0);
            else
                flux[2 * i + 0] = 0;
            if (z + 1 < res)
                compute_edge(i, IDX(x, z + 1), 2 * i + 1);
            else
                flux[2 * i + 1] = 0;
        }

    // --- : K outflow-clamp per cell (Sigma_out <= depth -> non-negative + mass-exact) ---
    // A cell's outgoing edges are the ones where IT is the higher (source) side: +X/+Z of i when
    // the flux is positive, and +X/+Z of the left/up neighbour when their flux is negative. Each
    // edge has exactly ONE source cell, so per-cell clamps touch disjoint edge sets ->
    // order-independent.
    for (int z = 0; z < res; ++z)
        for (int x = 0; x < res; ++x) {
            const int i = IDX(x, z);
            std::int32_t* e[4];
            std::int32_t sgn[4];
            int ne = 0;
            std::int64_t out = 0;
            auto add_out = [&](std::int32_t* ptr, std::int32_t s) {
                const std::int64_t o =
                    static_cast<std::int64_t>(*ptr) * s; // outflow magnitude if >0
                if (o > 0) {
                    out += o;
                    e[ne] = ptr;
                    sgn[ne] = s;
                    ++ne;
                }
            };
            if (x + 1 < res)
                add_out(&flux[2 * i + 0], +1);
            if (z + 1 < res)
                add_out(&flux[2 * i + 1], +1);
            if (x - 1 >= 0)
                add_out(&flux[2 * IDX(x - 1, z) + 0], -1);
            if (z - 1 >= 0)
                add_out(&flux[2 * IDX(x, z - 1) + 1], -1);
            const std::int64_t d = depth[i];
            if (out > d) {
                for (int k = 0; k < ne; ++k) {
                    const std::int64_t mag = static_cast<std::int64_t>(*e[k]) * sgn[k]; // positive
                    const std::int64_t scaled = (mag * d) / out; // floor toward 0
                    *e[k] = static_cast<std::int32_t>(scaled * sgn[k]);
                }
            }
        }

    // --- : APPLY (symmetric: same int subtracted from P, added to N -> exact, order-free) ---
    for (int z = 0; z < res; ++z)
        for (int x = 0; x < res; ++x) {
            const int i = IDX(x, z);
            if (x + 1 < res) {
                const std::int32_t q = flux[2 * i + 0];
                depth[i] -= q;
                depth[IDX(x + 1, z)] += q;
            }
            if (z + 1 < res) {
                const std::int32_t q = flux[2 * i + 1];
                depth[i] -= q;
                depth[IDX(x, z + 1)] += q;
            }
        }

    // --- Sinks (sea/edge) + evaporation + defensive clamp + float render mirror ---
    std::int32_t max_delta = 0;
    std::int64_t sum_before = 0; // (water-kernel perf) in-kernel mass accumulators (see header
    std::int64_t sum_after = 0;  // comment) — depth_before[i] is read here anyway for max_delta.
    const bool have_mirror = (static_cast<int>(c.water_level_data.size()) == n);
    for (int z = 0; z < res; ++z)
        for (int x = 0; x < res; ++x) {
            const int i = IDX(x, z);
            if (depth[i] < 0)
                depth[i] = 0;       // defensive; the K-clamp should already guarantee this
            if (bed[i] <= sea_mm) { // ocean is an infinite SINK: settle the surface at sea level
                const std::int32_t target =
                    sea_mm - bed[i]; // depth giving surface == sea level (>= 0)
                if (depth[i] > target) {
                    out_sink += (depth[i] - target);
                    depth[i] = target;
                }
            } else {
                //  EVAPORATION (finite hydrology): above-sea standing water loses evap_mm/tick, so
                // without rain ponds slowly recede — the drying half of the cycle. (Caller keeps it
                // gentle.)
                if (evap_mm > 0 && depth[i] > 0) {
                    const std::int32_t e = depth[i] < evap_mm ? depth[i] : evap_mm;
                    depth[i] -= e;
                    out_sink += e;
                }
                if (depth[i] > 0 && depth[i] < EVAP_MM) { // cull sub-mm films on dry land
                    out_sink += depth[i];
                    depth[i] = 0;
                }
            }
            std::int32_t dl = depth[i] - depth_before[i];
            if (dl < 0)
                dl = -dl;
            if (dl > max_delta)
                max_delta = dl;
            sum_before += depth_before[i];
            sum_after += depth[i];
            if (have_mirror)
                c.water_level_data[i] =
                    static_cast<float>(bed[i] + depth[i]) / static_cast<float>(MM_PER_M);
        }
    out_depth_before += sum_before;
    out_depth_after += sum_after;
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
    : m_job_system(job_system)
    , m_shield_system(shield_system) {}

void WaterSystem::set_camera_entity(EntityID camera_entity) {
    m_camera_entity = camera_entity;
}

Vec3 WaterSystem::get_camera_position(entt::registry& registry) const {
    if (m_camera_entity != entt::null && registry.valid(m_camera_entity)) {
        if (const auto* transform =
                registry.try_get<const Components::TransformComponent>(m_camera_entity)) {
            return transform->position;
        }
    }

    auto camera_view =
        registry
            .view<const Components::TransformComponent, const Components::ActiveCameraComponent>();
    if (camera_view.begin() != camera_view.end()) {
        return camera_view.get<const Components::TransformComponent>(camera_view.front()).position;
    }

    return Vec3(0.0f);
}

void WaterSystem::update(entt::registry& registry,
                         const std::unordered_map<ChunkID, std::shared_ptr<Chunk>>& active_chunks) {
    // (water-kernel perf) sub-phase wall timers — RUNTIME TELEMETRY ONLY (never hashed),
    // the _dbg_split pattern from SHIELD_WorldSystem::update.
    m_dbg_water = {};
    auto dbg_prev = std::chrono::steady_clock::now();
    auto dbg_split = [&](double& slot) {
        const auto now = std::chrono::steady_clock::now();
        slot += std::chrono::duration<double, std::milli>(now - dbg_prev).count();
        dbg_prev = now;
    };

    m_active_chunks = &active_chunks;
    if (m_active_chunks->empty())
        return;
    // a session booted from a save pauses water for the whole Boot (see
    // SetBootPaused) — no init, no wake/sleep mutation, no stepping. The loaded
    // mid-flow water state stays bit-exact until live ticks resume it.
    if (m_boot_paused)
        return;

    // --- ADAPTIVE WATER GRID SYSTEM INTEGRATION ---
    // Apply adaptive resolution to active water chunks.  implementation note: each resolution
    // change re-samples GetTerrainHeightAt per cell (~30us, up to 1024 cells at Ultra), so a burst
    // of chunks crossing LOD boundaries at once when moving spiked this loop to ~180ms. Collect the
    // chunks that actually need a resize, then apply a DETERMINISTIC per-tick budget (sorted by
    // chunk id, rotating window via m_water_resize_cursor) — identical amortization to the init/sim
    // caps. Deferred chunks keep their current resolution one more tick and resize over the next
    // few ticks; the selection is timing-independent (id-sorted), so run==replay / host==peer hold
    // (WaterDeterminism gate).
    //
    // (water-kernel perf) This scan and the first-time-INIT scan below used to be two separate
    // full passes over the active-chunk map. Their predicates are disjoint (resize candidates
    // require has_water_sim==true, init candidates require it false) and the map is not mutated
    // between the old passes (a resize touches chunk contents, never map membership, and only
    // chunks already has_water_sim==true), so ONE pass fills both lists with the exact same
    // membership AND the same capped first-N init selection (same map, same iteration order) —
    // byte-identical, half the map-walk cost. The old per-chunk camera-distance math is dropped:
    // CalculateRequiredDetail deterministically ignores its distance/interaction arguments (the
    // hashed sim grid is camera-independent — see its body), so the computed detail level is
    // unchanged.
    struct ResizeRequest {
        Chunk* chunk;
        ChunkID id;
        WaterDetailLevel level;
    };
    std::vector<ResizeRequest> resize_requests;
    // (water-perf-200fps init follow-on) First-time water INIT is the dominant water-frame cost:
    // per cell it samples the worldgen (WaterLevelAt + GetTerrainHeightAt), time-sliced to
    // MAX_WATER_INITS_PER_TICK chunks/tick. Collect THIS tick's chunks in the SAME
    // selection order + cap as before, then SEED THEM IN PARALLEL. Each chunk writes ONLY its own
    // arrays from read-only worldgen queries — already thread-safe (the streamer generates chunks
    // on these same workers and terrain is byte-identical run-to-run) — so parallel seeding is
    // BYTE-IDENTICAL to sequential. run==replay / host==peer hold (validated by --smoke).
    std::vector<Chunk*> to_init;
    bool init_cap_reached = false;
    for (const auto& active_chunk : active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (!chunk_ptr) {
            continue;
        }
        if (chunk_ptr->has_water_sim.load()) {
            const WaterDetailLevel required_detail =
                CalculateRequiredDetail(*chunk_ptr, 0.0f, false);
            // Only the chunks whose grid resolution must actually change are resize candidates
            // (Off is left to the existing path — never disabled here, preserving prior behaviour).
            if (required_detail != WaterDetailLevel::Off &&
                static_cast<int>(required_detail) != chunk_ptr->current_water_resolution.load()) {
                resize_requests.push_back({chunk_ptr.get(), chunk_ptr->get_id(), required_detail});
            }
        } else if (!init_cap_reached) {
            //  boot-settle mode: no init cap — seed the ENTIRE pending backlog this
            // call so the boot settle can reach its fixed point (see SetBootSettleMode).
            if (!m_boot_settle_mode &&
                static_cast<int>(to_init.size()) >= MAX_WATER_INITS_PER_TICK) {
                init_cap_reached = true; // same cap as the old water_inits_this_tick counter
            } else {
                to_init.push_back(chunk_ptr.get());
            }
        }
    }
    if (!resize_requests.empty()) {
        std::sort(resize_requests.begin(),
                  resize_requests.end(),
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

    auto source_view =
        registry
            .view<const Components::TransformComponent, const Components::WaterSourceComponent>();

    // (worst-frame fix) The dominant moving cost is INIT re-sampling worldgen per cell
    // (~16ms/frame, profiled). The terrain half (GetTerrainHeightAt) is BYTE-IDENTICAL to the
    // heightmap the mesher already computed into heightmap_data (proven by the EXPECT_EQ parity
    // gate, commit 8348e0e9), so REUSE it — a cheap array read instead of a ~125us multi-octave
    // sample. Read it HERE on the MAIN THREAD: an active chunk's LIVE heightmap_data is only ever
    // (re)written by the main-thread LOD publish (process_completed_meshing_jobs); off-thread
    // meshing writes scratch/pending, never the live array. So this read cannot race, and the
    // workers below get a PRIVATE copy (never touch heightmap_data) — unlike the old worker-side
    // read. When shaping is disabled, workers fall back to the analytic sampler, preserving
    // identical water-bed values.
    const int hm_stride = CHUNK_SIZE_X + 1; // heightmap is (CHUNK_SIZE_X+1)^2, x-fastest
    // The heightmap-node reuse below requires each water-cell CENTRE to land on an
    // integer heightmap node: centre = step*x + step/2 with step = CHUNK_SIZE_X /
    // resolution, so step must divide evenly AND be even (Medium: step 2 -> node
    // 2x+1, the historical fast path). At High (step 1) centres sit on half-metre
    // positions with no node — every chunk takes the pure-sampler fallback, which
    // yields the SAME bits (the parity-gate argument below), just slower.
    const int hm_step = (m_sim_resolution > 0 && CHUNK_SIZE_X % m_sim_resolution == 0)
                            ? CHUNK_SIZE_X / m_sim_resolution
                            : 0;
    const bool reuse_heightmap =
        m_shield_system->get_params().shaping_enabled && hm_step > 0 && hm_step % 2 == 0;
    std::vector<std::vector<float>> terrain_seed(
        to_init.size()); // [i] empty => worker samples the sampler
    if (reuse_heightmap) {
        const int res = m_sim_resolution;
        for (std::size_t i = 0; i < to_init.size(); ++i) {
            const Chunk* cp = to_init[i];
            // DETERMINISM (cold-first-run fix): reuse the heightmap ONLY at the finest LOD
            // (current_lod == 0), where it is byte-identical to GetTerrainHeightAt (the EXPECT_EQ
            // parity gate). A chunk that is water-initialized while still at a COARSE / not-yet-
            // promoted LOD carries a heightmap that does NOT match the sampler, and WHETHER it has
            // reached LOD0 by init time is streaming-timing-dependent — so the old size-only gate
            // seeded a timing-dependent water_bed_mm (cold run-1 != warm run-2; --smoke water
            // sub-hash flaked). For any non-LOD0 chunk we leave terrain_seed[i] empty so
            // seed_chunk_water falls back to the pure sampler, which yields the SAME bits the LOD0
            // heightmap would — so water_bed_mm is deterministic regardless of streaming timing,
            // while LOD0 chunks (the common settled case) keep the cheap heightmap-reuse fast path.
            if (cp->current_lod.load(std::memory_order_acquire) != 0)
                continue;
            if (static_cast<int>(cp->heightmap_data.size()) != hm_stride * hm_stride)
                continue;
            terrain_seed[i].resize(static_cast<std::size_t>(res) * res);
            for (int z = 0; z < res; ++z) {
                for (int x = 0; x < res; ++x) {
                    // Water cell centre is chunk-local (step*x + step/2, step*z + step/2) — an
                    // integer heightmap node (2x+1 at Medium's step 2), and heightmap[node] ==
                    // GetTerrainHeightAt(node) to the BIT (parity gate) -> byte-exact.
                    terrain_seed[i][static_cast<std::size_t>(z) * res + x] =
                        cp->heightmap_data[(hm_step * x + hm_step / 2) +
                                           (hm_step * z + hm_step / 2) * hm_stride];
                }
            }
        }
    }

    auto seed_chunk_water = [this](Chunk* cp, const std::vector<float>& terrain) {
        //  init directly at the fixed camera-independent sim resolution (no later
        // camera-driven resize -> the hashed grid is uniform + stable on every peer).
        const int initial_resolution = m_sim_resolution;
        cp->current_water_resolution.store(initial_resolution);
        const size_t sim_size = static_cast<size_t>(initial_resolution) * initial_resolution;
        cp->water_level_data.assign(sim_size, SEA_LEVEL);
        cp->water_flow_data.assign(sim_size, Vec2(0.0f));
        cp->water_sim_terrain_height.resize(sim_size);
        cp->water_rest_level.assign(sim_size, SEA_LEVEL);
        // fixed-point flowing-water state (HASHED). bed = terrain, depth = standing water
        // above bed; lakes/sea start filled, dry land + perched channels fill from the sources.
        cp->water_depth_mm.assign(sim_size, 0);
        cp->water_bed_mm.assign(sim_size, 0);
        cp->water_edge_flux.assign(2 * sim_size, 0);
        const IVec3 c_coords = cp->get_coords();
        const float cell_width_x = CHUNK_SIZE_X / (float)initial_resolution;
        const float cell_width_z = CHUNK_SIZE_Z / (float)initial_resolution;
        const bool have_terrain =
            (terrain.size() == sim_size); // main thread pre-read the heightmap
        for (int z = 0; z < initial_resolution; ++z) {
            for (int x = 0; x < initial_resolution; ++x) {
                float world_x = c_coords.x * CHUNK_SIZE_X + (x + 0.5f) * cell_width_x;
                float world_z = c_coords.z * CHUNK_SIZE_Z + (z + 0.5f) * cell_width_z;
                const int cell = z * initial_resolution + x;
                // Seed the resting surface from worldgen: sea level, raised to the local lake
                // surface inside basins so perched lakes start (and stay) filled.
                const float rest = m_shield_system->WaterLevelAt(world_x, world_z);
                // Reuse the mesher's heightmap (byte-identical) when the main thread provided it;
                // else sample GetTerrainHeightAt directly — same bits, so water_bed_mm is identical
                // either way.
                const float terrain_h = have_terrain
                                            ? terrain[cell]
                                            : m_shield_system->GetTerrainHeightAt(world_x, world_z);
                cp->water_level_data[cell] = rest;
                cp->water_rest_level[cell] = rest;
                cp->water_sim_terrain_height[cell] = terrain_h;
                cp->water_bed_mm[cell] =
                    static_cast<std::int32_t>(std::lround(terrain_h * MM_PER_M));
                const long depth0 = std::lround(static_cast<double>(rest - terrain_h) * MM_PER_M);
                cp->water_depth_mm[cell] = static_cast<std::int32_t>(depth0 > 0 ? depth0 : 0);
            }
        }
        cp->has_water_sim.store(true);
        cp->water_mesh_generated.store(false);
        cp->water_mesh_dirty_ticks = 0;
    };
    // (worst-frame fix) Seed INLINE on the main thread — NOT dispatch_batch + wait. With the
    // terrain half now a free heightmap read, the per-chunk compute is cheap (WaterLevelAt +
    // integer fill). The old parallel path's wait was HEAD-OF-LINE blocked behind the flooded
    // streaming/meshing job queue (the same pathology the water-SIM hit — see moving-lag notes: "18
    // trivial jobs took ~1300ms because wait blocked behind the streaming flood"), so most of its
    // ~16ms was QUEUE WAIT, not work. Inline pays only the (now small) compute and skips the wait.
    // Each chunk writes only its own arrays -> byte-identical to the parallel version -> world_hash
    // unchanged (no re-pin).
    //
    //  boot-settle mode: the bulk backlog (thousands of chunks, mostly coarse-LOD so
    // the heightmap fast path doesn't apply and each pays the full sampler) IS worth the
    // parallel path — at boot the queue is post-residency-settle idle, so the head-of-line
    // hazard above does not apply. Each chunk writes only its own arrays from read-only
    // worldgen queries (the streamer generates chunks on these same workers), so parallel is
    // byte-identical to sequential — the ORDER chunks are seeded in never changes the bits.
    if (m_boot_settle_mode && to_init.size() > 8 && m_job_system) {
        std::vector<Job> seed_jobs;
        seed_jobs.reserve(to_init.size());
        for (std::size_t i = 0; i < to_init.size(); ++i) {
            Chunk* cp = to_init[i];
            const std::vector<float>* terrain = &terrain_seed[i];
            seed_jobs.emplace_back(
                [&seed_chunk_water, cp, terrain]() { seed_chunk_water(cp, *terrain); });
        }
        m_job_system->wait(m_job_system->dispatch_batch(seed_jobs));
    } else {
        for (std::size_t i = 0; i < to_init.size(); ++i) {
            seed_chunk_water(to_init[i], terrain_seed[i]);
        }
    }
    dbg_split(m_dbg_water.init);

    for (auto entity : source_view) {
        auto& transform = source_view.get<const Components::TransformComponent>(entity);
        auto& source = source_view.get<const Components::WaterSourceComponent>(entity);
        ++m_debug_sources_seen; // water-source diagnostics
        IVec3 chunk_coords = SHIELD_WorldSystem::world_to_chunk_coords(transform.position);
        auto it = m_active_chunks->find(Chunk::calculate_id(chunk_coords));
        // water-source FINDING (empirical): water grids live on the 2.5D COLUMN's y=0
        // chunk, but a spring standing on sub-sea-level terrain (river beds carve
        // below 0) resolves to the y=-1 chunk and silently no-ops. Fall back to
        // the column's y=0 chunk when the direct chunk carries no water sim.
        if (it == m_active_chunks->end() || !it->second ||
            !it->second->has_water_sim.load(std::memory_order_relaxed)) {
            IVec3 col = chunk_coords;
            col.y = 0;
            it = m_active_chunks->find(Chunk::calculate_id(col));
        }
        if (it != m_active_chunks->end() && it->second) {
            Chunk& chunk = *it->second;
            const int resolution = GetWaterResolution(chunk);
            if (!HasCompleteWaterGrid(chunk, resolution)) {
                continue;
            }

            IVec3 local_pos = IVec3(transform.position) -
                              (chunk_coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z));
            int sim_x = (local_pos.x * resolution) / CHUNK_SIZE_X;
            int sim_z = (local_pos.z * resolution) / CHUNK_SIZE_Z;
            int index = std::clamp(sim_z, 0, resolution - 1) * resolution +
                        std::clamp(sim_x, 0, resolution - 1);
            float cell_area = (static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(resolution)) *
                              (static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(resolution));
            float water_added = source.flow_rate * SECONDS_PER_TICK / cell_area;
            // water-source: the injection routes through the INTEGER mm
            // domain — the hashed sim truth — quantized ONCE at this boundary
            // (metres -> mm, round-half-up); the float mirror regenerates FROM mm
            // (surface = bed + depth, the same one-way formula the solver uses).
            // The old code added metres into the float mirror directly, a
            // sim-relevant float mutation the mm solver never saw.
            const std::int32_t add_mm = static_cast<std::int32_t>(water_added * 1000.0f + 0.5f);
            if (add_mm > 0 && index < static_cast<int>(chunk.water_depth_mm.size())) {
                chunk.water_depth_mm[index] += add_mm;
                m_debug_source_injected_mm += add_mm; // water-source diagnostics
                if (index < static_cast<int>(chunk.water_level_data.size()) &&
                    index < static_cast<int>(chunk.water_bed_mm.size())) {
                    chunk.water_level_data[index] =
                        static_cast<float>(chunk.water_bed_mm[index] +
                                           chunk.water_depth_mm[index]) /
                        1000.0f;
                }
                // WAKE UP: An internal event occurred in this chunk.
                chunk.is_water_sleeping.store(false, std::memory_order_relaxed);
            }
        }
    }

    // --- Step 2: Wake up sleeping chunks that are adjacent to active ones (propagation) ---
    // wake propagation is TWO-PHASE — decide against the PRE-PASS sleep
    // snapshot, then apply. The old single pass woke chunks in unordered_map
    // iteration order while READING the flags it was mutating, so a multi-hop wake
    // cascade within one tick depended on the map's insertion history — which
    // differs between a progressively-streamed session and a save-adopted one
    // (run==replay held, save/load and host!=peer did not: the heavy oracle's
    // resim leg diverged at its first tick on exactly this). Deferred application
    // makes the pass order-independent; a cascade now propagates one neighbour
    // hop per tick, deterministically.
    // (water-kernel perf) The wake pass and the Step-3 sim-collection pass used to be two
    // separate full walks over the active-chunk map. They are merged into ONE walk: wake
    // decisions still read only PRE-PASS sleep flags (every store is deferred to after the
    // scan, exactly as before, so the two-phase order-independence argument above is intact),
    // and sim membership is computed from (pre-pass flag, woken-this-pass) — algebraically
    // identical to the old second pass's post-application read (post_sleep = pre_sleep &&
    // !woken; nothing else mutates the flag between the old passes). Same map, same iteration
    // order, no mutation during the scan -> the collected vector is element-for-element
    // identical (its order is anyway erased by the id-sort below). Halves the map-walk cost.
    //
    // when rain is falling, keep EVERY water chunk awake — rain must land on dry, otherwise
    // sleeping, land so puddles/ponds form in the low spots (sleeping chunks are skipped and
    // never get rain). Without rain this is the normal "simulate only active chunks" fast path.
    // weather-driven rain keeps chunks awake the same way (a storm cell may
    // rain on any chunk even when the uniform rate is zero).
    const bool rain_active =
        m_rain_mm_per_tick > 0 || (m_weather_rain != nullptr && m_weather_rain_scale_mm > 0);
    std::vector<Chunk*> chunks_to_wake;
    std::vector<Chunk*> chunks_to_sim;
    chunks_to_sim.reserve(m_active_chunks->size());
    for (const auto& active_chunk : *m_active_chunks) {
        const auto& chunk_ptr = active_chunk.second;
        if (!chunk_ptr) {
            continue;
        }

        const bool sleeping_pre = chunk_ptr->is_water_sleeping.load(std::memory_order_relaxed);
        bool woken = false;
        if (sleeping_pre) {
            IVec3 self_coords = chunk_ptr->get_coords();
            const IVec3 neighbor_offsets[] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}};
            for (const auto& offset : neighbor_offsets) {
                IVec3 neighbor_coords = self_coords + offset;
                auto it = m_active_chunks->find(Chunk::calculate_id(neighbor_coords));
                if (it != m_active_chunks->end() && it->second &&
                    !it->second->is_water_sleeping.load(std::memory_order_relaxed)) {
                    // WAKE UP (deferred): a neighbor is active per the pre-pass state.
                    chunks_to_wake.push_back(chunk_ptr.get());
                    woken = true;
                    break;
                }
            }
        }
        if (chunk_ptr->has_water_sim.load() && (rain_active || !sleeping_pre || woken)) {
            chunks_to_sim.push_back(chunk_ptr.get());
        }
    }
    for (Chunk* chunk : chunks_to_wake) {
        chunk->is_water_sleeping.store(false, std::memory_order_relaxed);
    }

    //  implementation note: bound the per-tick sim work to the MAX_WATER_CELLS_PER_TICK cell
    //  budget via a DETERMINISTIC
    // rotating window of WATER_SIM_WINDOW_CHUNKS chunks (budget / cells-per-chunk at the uniform
    // resolution). Sort by chunk id (order independent of the unordered_map's iteration), then
    // take a window starting at m_water_sim_cursor and advance it — so over a few ticks every
    // active chunk sims, but no single tick blocks on hundreds of chunks. The un-simulated chunks
    // stay awake and are picked up by the rotation next tick. The cursor stays in CHUNK-INDEX
    // space (persisted as world_info.json waterSimCursor) and its evolution is a pure function
    // of the active-set size sequence -> run==replay / host==peer identical (verified by the
    // WaterDeterminism gate).
    //  boot-settle mode: no rotating window — sim EVERY awake chunk each call so
    // ticks_below_threshold advances every settle iteration and the sleep threshold (120
    // calm ticks) is reachable within a bounded settle (see SetBootSettleMode).
    const std::size_t water_cells_per_chunk = GetWaterCellCount(m_sim_resolution);
    const std::size_t water_sim_window_chunks = DeriveSimWindowChunks(water_cells_per_chunk);
    m_dbg_awake_water_chunks = chunks_to_sim.size();
    if (!m_boot_settle_mode && chunks_to_sim.size() > water_sim_window_chunks) {
        std::sort(chunks_to_sim.begin(), chunks_to_sim.end(), [](const Chunk* a, const Chunk* b) {
            return a->get_id() < b->get_id();
        });
        const std::size_t total = chunks_to_sim.size();
        m_water_sim_cursor %= total;
        std::vector<Chunk*> window;
        window.reserve(water_sim_window_chunks);
        for (std::size_t i = 0; i < water_sim_window_chunks; ++i) {
            window.push_back(chunks_to_sim[(m_water_sim_cursor + i) % total]);
        }
        m_water_sim_cursor = (m_water_sim_cursor + water_sim_window_chunks) % total;
        chunks_to_sim.swap(window);
    }
    m_dbg_cells_simmed = chunks_to_sim.size() * water_cells_per_chunk;

    // --- Step 4:  — INTEGER virtual-pipes. Per chunk: internal-edge flux + sources/sinks
    // (StepChunkWaterFixed). Then a CROSS-CHUNK owner-edge shared-flux pass so rivers/lakes are
    // CONTINUOUS + mass-conserved across chunk seams. Mass invariant: over the simmed
    // set Σdepth changes by exactly (Σsource − Σsink); the cross-chunk pass is pure symmetric
    // transfer WITHIN the set -> nets to zero in Σdepth. FIXED order (id-sorted) so cross-chunk
    // apply is deterministic.
    std::sort(chunks_to_sim.begin(), chunks_to_sim.end(), [](const Chunk* a, const Chunk* b) {
        return a->get_id() < b->get_id();
    });
    // (water-kernel perf) The seam pass only transfers to a neighbour that is IN the sim
    // window (both sides must be inside the Σdepth mass sum). The window is id-sorted and
    // every element came from the active-chunk map (non-null, unique ids), so a binary search
    // over it is exactly equivalent to the old active-map find + unordered_set membership
    // check — same pointer or nothing — without building a per-tick unordered_set or hashing
    // into the big map.
    auto find_in_window = [&chunks_to_sim](ChunkID id) -> Chunk* {
        const auto it = std::lower_bound(chunks_to_sim.begin(),
                                         chunks_to_sim.end(),
                                         id,
                                         [](const Chunk* c, ChunkID v) { return c->get_id() < v; });
        return (it != chunks_to_sim.end() && (*it)->get_id() == id) ? *it : nullptr;
    };
    dbg_split(m_dbg_water.bookkeeping);

    // (water-kernel perf) The two whole-window Σdepth passes that bracketed this loop are now
    // in-kernel accumulators (see StepChunkWaterFixed's header comment for the bit-exactness
    // argument — they feed only the debug mass invariant below, never the hash).
    std::int64_t depth_before = 0, depth_after = 0;
    std::int64_t src_mm = 0, sink_mm = 0;
    for (Chunk* chunk : chunks_to_sim) {
        StepChunkWaterFixed(*chunk,
                            *m_shield_system,
                            src_mm,
                            sink_mm,
                            depth_before,
                            depth_after,
                            m_finite_hydrology,
                            m_rain_mm_per_tick,
                            m_evap_mm_per_tick,
                            m_weather_rain,
                            m_weather_rain_scale_mm);
    }
    dbg_split(m_dbg_water.sim);

    // Cross-chunk owner-edge shared flux. Each chunk OWNS its +X (east) and +Z (north) boundary
    // edges; it reads the neighbour's POST-internal bed+depth, computes the same pipe flux as an
    // internal edge, clamps to the SOURCE (higher) cell's available depth (sequential -> never
    // negative), and applies it SYMMETRICALLY across the seam (A -= q; B += q -> mass conserved).
    // Both chunks must be in the sim set so the transfer stays inside the Σdepth sum. The boundary
    // flux persists in the owner's edge_flux slot (the +X slot of x=res-1 / +Z slot of z=res-1,
    // which the internal step leaves at 0). Integer + fixed id-sorted order -> bit-exact host==peer
    // / run==replay.
    int seam_wet_pairs =
        0; // CONTINUITY proof: seam cell-pairs with water on BOTH sides of the border
    for (Chunk* A : chunks_to_sim) {
        const int res = GetWaterResolution(*A);
        if (res <= 1)
            continue;
        const int nA = res * res;
        if (static_cast<int>(A->water_depth_mm.size()) != nA ||
            static_cast<int>(A->water_edge_flux.size()) != 2 * nA)
            continue;
        const IVec3 ac = A->get_coords();
        const bool have_mirror_A = (static_cast<int>(A->water_level_data.size()) == nA);
        auto step_face = [&](Chunk* B, bool isX) {
            if (GetWaterResolution(*B) != res)
                return; // uniform-res only -> cells align cell-for-cell
            if (static_cast<int>(B->water_depth_mm.size()) != nA)
                return;
            const bool have_mirror_B = (static_cast<int>(B->water_level_data.size()) == nA);
            for (int t = 0; t < res; ++t) {
                int ai, bi, slot;
                if (isX) {
                    ai = t * res + (res - 1);
                    bi = t * res + 0;
                    slot = 2 * ai + 0;
                } else {
                    ai = (res - 1) * res + t;
                    bi = 0 * res + t;
                    slot = 2 * ai + 1;
                }
                const std::int64_t surfA =
                    static_cast<std::int64_t>(A->water_bed_mm[ai]) + A->water_depth_mm[ai];
                const std::int64_t surfB =
                    static_cast<std::int64_t>(B->water_bed_mm[bi]) + B->water_depth_mm[bi];
                const std::int64_t dSurf = surfA - surfB;
                std::int64_t q = static_cast<std::int64_t>(A->water_edge_flux[slot]) +
                                 ((K_ACCEL * dSurf) >> FLOW_SHIFT);
                q = (q * FRICTION_NUM) >> FRICTION_SHIFT;
                if (dSurf < MIN_FLOW_MM && dSurf > -MIN_FLOW_MM)
                    q = 0;
                if (q > 2000000000LL)
                    q = 2000000000LL;
                if (q < -2000000000LL)
                    q = -2000000000LL;
                if (q > 0) {
                    if (q > A->water_depth_mm[ai])
                        q = A->water_depth_mm[ai];
                } else if (q < 0) {
                    if (-q > B->water_depth_mm[bi])
                        q = -static_cast<std::int64_t>(B->water_depth_mm[bi]);
                }
                A->water_edge_flux[slot] = static_cast<std::int32_t>(q);
                A->water_depth_mm[ai] -= static_cast<std::int32_t>(q);
                B->water_depth_mm[bi] += static_cast<std::int32_t>(q);
                if (A->water_depth_mm[ai] > 0 && B->water_depth_mm[bi] > 0)
                    ++seam_wet_pairs;
                if (q != 0) {
                    A->is_water_sleeping.store(false, std::memory_order_relaxed);
                    B->is_water_sleeping.store(false, std::memory_order_relaxed);
                    if (have_mirror_A)
                        A->water_level_data[ai] =
                            static_cast<float>(A->water_bed_mm[ai] + A->water_depth_mm[ai]) /
                            static_cast<float>(MM_PER_M);
                    if (have_mirror_B)
                        B->water_level_data[bi] =
                            static_cast<float>(B->water_bed_mm[bi] + B->water_depth_mm[bi]) /
                            static_cast<float>(MM_PER_M);
                }
            }
        };
        if (Chunk* east = find_in_window(Chunk::calculate_id(ac + IVec3(1, 0, 0))))
            step_face(east, true);
        if (Chunk* north = find_in_window(Chunk::calculate_id(ac + IVec3(0, 0, 1))))
            step_face(north, false);
    }
    dbg_split(m_dbg_water.seam);

    m_dbg_last_source_mm = src_mm;
    m_dbg_last_sink_mm = sink_mm;
    m_dbg_mass_ok = ((depth_after - depth_before) == (src_mm - sink_mm));
    m_dbg_seam_wet_pairs = seam_wet_pairs;

    // --- Step 5: Update sleep states for chunks that were just simulated ---
    const int TICKS_TO_SLEEP = 120;        // ~4 seconds at 30 ticks per second
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
    dbg_split(m_dbg_water.bookkeeping);
}

f32 WaterSystem::get_water_level_at(float world_x, float world_z) const {
    if (!m_active_chunks)
        return SEA_LEVEL;

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
    if (!m_active_chunks)
        return;

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

                int final_sim_x =
                    std::clamp(static_cast<int>(((source_cell_x + 0.5f) / source_resolution) *
                                                target_resolution),
                               0,
                               target_resolution - 1);
                int final_sim_z =
                    std::clamp(static_cast<int>(((source_cell_z + 0.5f) / source_resolution) *
                                                target_resolution),
                               0,
                               target_resolution - 1);

                float distance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
                float falloff = 1.0f - std::min(distance / (radius + 1.0f), 1.0f);
                falloff = falloff * falloff; // Cosine-like falloff

                const float target_cell_area =
                    (static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(target_resolution)) *
                    (static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(target_resolution));
                const float base_height_delta = volume / target_cell_area;
                int index = final_sim_z * target_resolution + final_sim_x;
                // fixed-point water: the displacement routes through the INTEGER mm
                // domain, quantized ONCE at this boundary (round-half-away); depth
                // clamps at zero (no negative water); the float mirror regenerates
                // FROM mm (one-way — the old code added metres into the mirror
                // directly, a sim-relevant float mutation the mm solver never saw).
                // A sub-half-mm splash quantizes to 0 and disturbs nothing.
                const float add_m = (base_height_delta / total_cells) * falloff;
                const std::int32_t add_mm =
                    static_cast<std::int32_t>(add_m * 1000.0f + (add_m >= 0.0f ? 0.5f : -0.5f));
                if (add_mm != 0 && index < static_cast<int>(target_chunk.water_depth_mm.size())) {
                    std::int32_t& d = target_chunk.water_depth_mm[index];
                    d = std::max(0, d + add_mm);
                    if (index < static_cast<int>(target_chunk.water_level_data.size()) &&
                        index < static_cast<int>(target_chunk.water_bed_mm.size())) {
                        target_chunk.water_level_data[index] =
                            static_cast<float>(target_chunk.water_bed_mm[index] + d) / 1000.0f;
                    }
                    // WAKE UP: An external event disturbed this chunk.
                    target_chunk.is_water_sleeping.store(false, std::memory_order_relaxed);
                }
            }
        }
    }
}

// TERRAFORM the water bed (dig / dam). Adjusts the integer bed height
// (water_bed_mm) by delta_mm for every sim cell within radius_m of world_pos (delta<0 = dig a
// channel, delta>0 = raise a dam), then WAKES those chunks so the fixed-point solver re-routes next
// tick. The solver drains/pools FOR FREE because its flux reads only Δ(bed+depth) — no special
// drain path. The edit is deterministic (integer delta, applied in the sim tick; replicate the edit
// as a command for host==peer). The VOXEL/SDF terrain + its render mesh are a separate concern;
// this couples WATER to a bed change. Returns the number of cells edited.
int WaterSystem::EditTerrainBed(const Vec3& world_pos, std::int32_t delta_mm, float radius_m) {
    if (!m_active_chunks || delta_mm == 0)
        return 0;
    const float r2 = radius_m * radius_m;
    int edited_total = 0;
    for (const auto& kv : *m_active_chunks) {
        const std::shared_ptr<Chunk>& cptr = kv.second;
        if (!cptr)
            continue;
        Chunk& c = *cptr;
        const int res = GetWaterResolution(c);
        if (res <= 1 || static_cast<int>(c.water_bed_mm.size()) != res * res)
            continue;
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
    // a bed edit that touched cells advances the WATER EPOCH so the
    // render-side waterfall survey re-runs once (bounded) against the new terrain.
    if (edited_total > 0) {
        ++m_water_epoch;
    }
    return edited_total;
}

// --- ADAPTIVE WATER GRID SYSTEM IMPLEMENTATION ---

WaterDetailLevel WaterSystem::CalculateRequiredDetail(const Chunk& chunk,
                                                      float camera_distance,
                                                      bool has_player_interaction) {
    //  the HASHED sim grid is CAMERA-INDEPENDENT — every simulated water chunk runs at
    // the single fixed session resolution (m_sim_resolution). A camera-driven resolution made the
    // hashed mm water state differ per peer (host!=peer) and misaligned cross-chunk boundary cells.
    // Uniform resolution is required so neighbour chunks exchange flux cell-for-cell. (Visual mesh
    // LOD, if ever wanted, is a separate render-only concern — it must NOT drive this hashed grid.)
    (void)chunk;
    (void)camera_distance;
    (void)has_player_interaction;
    return static_cast<WaterDetailLevel>(m_sim_resolution);
}

void WaterSystem::SetSimResolution(int cells_per_side) {
    // One uniform grid for the whole session: the resolution must divide the chunk so
    // seam cells align cell-for-cell (see SetSimResolution's header contract). Reject
    // anything else and keep the current resolution — the caller wires this once from
    // sim.water_high_res before the first update, so a rejected value simply leaves
    // the byte-identical Medium default in place.
    if (cells_per_side <= 1 || CHUNK_SIZE_X % cells_per_side != 0 ||
        CHUNK_SIZE_Z % cells_per_side != 0) {
        return;
    }
    m_sim_resolution = cells_per_side;
}

std::size_t WaterSystem::MigrateChunksToSimResolution(
    const std::unordered_map<ChunkID, std::shared_ptr<Chunk>>& chunks) {
    const auto target_level = static_cast<WaterDetailLevel>(m_sim_resolution);
    std::size_t migrated = 0;
    for (const auto& entry : chunks) {
        Chunk* cp = entry.second.get();
        // Only chunks that already carry live water sim state migrate — resizing a
        // chunk without one would fabricate water where the session never seeded it.
        if (cp == nullptr || !cp->has_water_sim.load(std::memory_order_relaxed)) {
            continue;
        }
        if (cp->current_water_resolution.load(std::memory_order_relaxed) == m_sim_resolution) {
            continue;
        }
        ResizeSimulationGrid(*cp, target_level);
        ++migrated;
    }
    return migrated;
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
    const bool can_interpolate = current_resolution > 1 &&
                                 old_water_levels.size() >= GetWaterCellCount(current_resolution) &&
                                 old_flow_data.size() >= GetWaterCellCount(current_resolution);
    // fixed-point water: the HASHED depth interpolates NATIVELY in the mm
    // domain from the OLD mm truth — integer bilinear with 1/256 fixed-point
    // weights — killing the old float->mm feedback (the mirror was interpolated
    // in float, then mm re-derived from it via lround, so the Render-class float
    // mirror sat on the sim-truth path). The mirror now regenerates FROM mm.
    std::vector<std::int32_t> old_depth_mm = chunk.water_depth_mm;
    const bool can_interpolate_mm =
        can_interpolate && old_depth_mm.size() >= GetWaterCellCount(current_resolution);

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
            chunk.water_sim_terrain_height[idx] =
                m_shield_system->GetTerrainHeightAt(world_x, world_z);

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

    //  / fixed-point water: resize + re-seed the fixed-point mm arrays (the HASHED sim
    // state) to the new resolution — otherwise StepChunkWaterFixed's size-guard bails and the
    // chunk's water FREEZES. bed = terrain (mm, resampled); depth = INTEGER BILINEAR of the OLD
    // depth_mm (1/256 fixed-point weights — pure integer, no float on the sim-truth path); the
    // float surface mirror then regenerates FROM mm (one-way), replacing the old
    // interpolate-float-then-lround feedback. Flux reset. Deterministic.
    // The resolution is camera-INDEPENDENT (see CalculateRequiredDetail): resizes fire
    // only when a chunk's stored resolution disagrees with the session's — i.e. the
    // boot-time save migration (MigrateChunksToSimResolution) and the capped live
    // convergence path for chunks adopted mid-run.
    chunk.water_depth_mm.assign(new_sim_size, 0);
    chunk.water_bed_mm.assign(new_sim_size, 0);
    chunk.water_edge_flux.assign(2 * new_sim_size, 0);
    for (int z = 0; z < new_resolution; ++z) {
        for (int x = 0; x < new_resolution; ++x) {
            const int idx = z * new_resolution + x;
            const float terr = chunk.water_sim_terrain_height[idx];
            chunk.water_bed_mm[idx] =
                static_cast<std::int32_t>(std::lround(terr * static_cast<float>(MM_PER_M)));
            if (can_interpolate_mm) {
                // Fixed-point bilinear over the OLD mm depths. Weights quantize the
                // fractional position to 1/256; the +32768 is round-half-up of the
                // 16-bit weight product. Integer end to end.
                const float old_x =
                    (new_resolution > 1)
                        ? (x / static_cast<float>(new_resolution - 1)) * (current_resolution - 1)
                        : 0.0f;
                const float old_z =
                    (new_resolution > 1)
                        ? (z / static_cast<float>(new_resolution - 1)) * (current_resolution - 1)
                        : 0.0f;
                const int x0 = static_cast<int>(std::floor(old_x));
                const int z0 = static_cast<int>(std::floor(old_z));
                const int x1 = std::min(x0 + 1, current_resolution - 1);
                const int z1 = std::min(z0 + 1, current_resolution - 1);
                const std::int64_t fxq =
                    static_cast<std::int64_t>(std::lround((old_x - x0) * 256.0f));
                const std::int64_t fzq =
                    static_cast<std::int64_t>(std::lround((old_z - z0) * 256.0f));
                const std::int64_t d00 = old_depth_mm[z0 * current_resolution + x0];
                const std::int64_t d10 = old_depth_mm[z0 * current_resolution + x1];
                const std::int64_t d01 = old_depth_mm[z1 * current_resolution + x0];
                const std::int64_t d11 = old_depth_mm[z1 * current_resolution + x1];
                const std::int64_t num = (256 - fxq) * (256 - fzq) * d00 + fxq * (256 - fzq) * d10 +
                                         (256 - fxq) * fzq * d01 + fxq * fzq * d11 + 32768;
                const std::int64_t d = num >> 16;
                chunk.water_depth_mm[idx] = d > 0 ? static_cast<std::int32_t>(d) : 0;
            } else {
                // No old mm truth (fresh/legacy chunk): fall back to the resampled
                // float surface minus terrain — the pre-fixed-point water seeding, unchanged.
                const long d = std::lround((chunk.water_level_data[idx] - terr) *
                                           static_cast<float>(MM_PER_M));
                chunk.water_depth_mm[idx] = d > 0 ? static_cast<std::int32_t>(d) : 0;
            }
            // The mirror regenerates FROM mm (one-way): surface = bed + depth.
            chunk.water_level_data[idx] =
                static_cast<float>(chunk.water_bed_mm[idx] + chunk.water_depth_mm[idx]) /
                static_cast<float>(MM_PER_M);
        }
    }

    chunk.has_water_sim.store(true);
    chunk.water_mesh_generated.store(false);
    chunk.water_mesh_dirty_ticks = 0;
    chunk.current_water_resolution.store(new_resolution);
}

} // namespace Luminumbra::Systems
