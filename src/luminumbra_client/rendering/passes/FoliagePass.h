#pragma once

#include "../AsyncReadbackRing.h"
#include "../RenderContext.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

namespace detail {

inline constexpr std::uint64_t pack_foliage_chunk_key(std::int32_t chunk_x,
                                                       std::int32_t chunk_z) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk_x)) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk_z)) << 32);
}

template <typename Cache, typename Chunks, typename KeyFn>
void prune_foliage_cache(Cache& cache, const Chunks& chunks, KeyFn key_fn) {
    std::unordered_set<std::uint64_t> live_keys;
    live_keys.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        live_keys.insert(key_fn(chunk));
    }

    for (auto it = cache.begin(); it != cache.end();) {
        if (live_keys.find(it->first) == live_keys.end()) {
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace detail

class Camera;
class Shader;

// ===========================================================================
// instanced foliage scatter + wind response..
//
// One instanced scatter system covering grass/tufts, pebbles/gravel, clutter
// (twigs/shells) and canopy, drawn per visible chunk from a fixed-capacity,
// persistent-mapped instance pool (the SAME glBufferStorage +
// GL_MAP_PERSISTENT|COHERENT pattern as the  ParticlePass /  chunk
// pool). NOT compute / transform feedback.
//
// PLACEMENT (PINNED, documented design): a DETERMINISTIC PURE FUNCTION of
// (chunk coords, biome id, slope, moisture, instance index) via a splitmix64
// hash. NO global RNG, NO world-seed offset is consumed (foliage is render-only
// and per-chunk, so no +14 seed is taken — documented design). Density is driven by the
// biome table's vegetation/cover block, modulated by slope and moisture.
//
// WIND (documented design): each instance carries a per-instance wind
// displacement sampled CPU-side from the  WindFieldSystem (the one-way
// replicated render bridge — same pattern  clouds used). The vertex shader
// bends the card tip by that displacement scaled by a per-ARCHETYPE sway flag
// (grass/canopy wave; pebbles/clutter do not). Distance-faded against the
// far-LOD horizon (no foliage in far tiles).
//
// ONE-WAY RULE (regression review): this subsystem READS sim/world state (biome,
// height, wind) but NEVER writes back into any sim/world_hash input. The
// scatter is regenerated per frame from the deterministic hash; nothing here is
// snapshotted into world_hash. The placement hash IS the determinism surface
// the FoliageInstancing gate asserts.
// ===========================================================================
class FoliagePass {
public:
    // --- Pinned capacities. ---
    static constexpr std::size_t kMaxInstances = 262144; // global scatter pool
    // Instance stride: pos(12) + size(8) + color(4) + sway(8) + phase(2) + facing(2).
    static constexpr std::size_t kInstanceStride = 36;
    // Double-buffer the persistent mapping (CPU writes frame N+1 while the GPU
    // may still read frame N).
    static constexpr std::size_t kRingFrames = 2;
    // Per-chunk scatter cap (placement evaluates this many candidate slots per
    // chunk; density + slope/moisture decide which actually emit).
    //  (defect 2): raised 256 -> 2048 so daytime ground
    // reads as real grass COVER rather than a few sparse tufts. The placement is
    // still a pure per-chunk hash (idx in [0,candidates)), so determinism + the
    //  contract are preserved; only the candidate count per chunk grows.
    // 2048 -> 4096 candidate slots. The per-frame CPU rebuild that once capped this
    // is removed by the scatter cache (#1, rebuild only on change). Density is then capped
    // by the BIOME contract, not perf: the FoliageInstancing gate requires coverage to
    // track the biome's vegetation density (within a band), so 8192 (coverage saturates to
    // 1.0, ignoring biome) FAILS — desert would carpet too. 4096 -> ~0.73 coverage tracks
    // the plains 0.3 density (in-band). "Continuous turf" in a scene is therefore the
    // biome's vegetation density (data/common/biomes.json), not this raw cap.
    static constexpr std::size_t kMaxCandidatesPerChunk =
        8192; // grass overhaul: 2x denser per-chunk
              // carpet (paired with the tighter near
              // fade so the global cap still fits).

    // Packed 36-byte instance record (matches the GL vertex-attribute layout).
#pragma pack(push, 1)
    struct InstanceRecord {
        float pos[3];     // world ground anchor
        float size[2];    // half-width, height (world units)
        uint8_t color[4]; // rgba8 (a = per-archetype sway flag scale 0..255)
        float sway[2];    // per-instance wind displacement at the tip (world XZ)
        uint16_t phase;   // f16 sway phase offset (radians)
        uint16_t facing;  // f16 card yaw in the XZ plane (radians)
    };
#pragma pack(pop)
    static_assert(sizeof(InstanceRecord) == kInstanceStride, "foliage instance must be 36 bytes");

    // Per-archetype scatter parameters (game content; loaded from
    // data/common/foliage/*.json). The engine knows only this schema.
    struct ArchetypeData {
        std::string name;
        glm::vec3 color{0.35f, 0.55f, 0.2f}; // albedo tint
        float half_width = 0.12f;            // card half width (world units)
        float height = 0.4f;                 // card height (world units)
        bool sways = true;                   // grass/canopy true; pebbles false
        float density_weight = 1.0f;         // share of the biome density budget
        bool loaded = false;
    };

    // A loaded foliage SET: a small ordered list of archetypes the scatter picks
    // from per instance (deterministically, by the placement hash). Loaded from
    // data/common/foliage/scatter_set.json.
    FoliagePass();
    ~FoliagePass();

    void init_shader(const std::filesystem::path& root_path);
    void init_buffers();
    void destroy_buffers();
    void reset_shader();

    //  #4: GPU grass scatter (compute). Compiles res/shaders/grass_scatter.comp
    // and allocates the SSBOs. On any failure m_gpu_scatter stays false and the
    // pass transparently uses the CPU rebuild loop (graceful degradation -- the
    // working path is never lost)..
    void init_compute(const std::filesystem::path& root_path);
    void destroy_compute();
    bool gpu_scatter_active() const {
        return m_gpu_scatter;
    }

    // --- GPU scatter tuning (mirrors the CPU placement constants). ---
    static constexpr int kSurfaceGrid = 8;                     // cells/side
    static constexpr int kSurfaceGridVerts = kSurfaceGrid + 1; // 9 -> 81 samples/chunk
    static constexpr std::size_t kWordsPerBlade = 9;           // 36-byte record = 9 u32
    // m_count_ssbo stores one append counter followed by a DrawArraysIndirectCommand.
    static constexpr std::size_t kGrassDrawCommandOffsetBytes = sizeof(u32);

    const std::unique_ptr<Shader>& shader() const {
        return m_shader;
    }
    u32 vao() const {
        return m_vao;
    }
    u32 instance_buffer(std::size_t ring) const {
        return m_instance_vbo[ring % kRingFrames];
    }
    bool enabled() const {
        return m_enabled;
    }
    void set_enabled(bool on) {
        m_enabled = on;
    }
    std::size_t frame_instance_count() const {
        return m_frame_instance_count;
    }

    // Loads the scatter archetype set from data/common/foliage/scatter_set.json.
    // On failure the pass stays empty (no foliage). Returns true on success.
    bool load_scatter_set(const std::filesystem::path& json_path);
    std::size_t archetype_count() const {
        return m_archetypes.size();
    }
    const ArchetypeData& archetype(std::size_t i) const {
        return m_archetypes[i];
    }

    // --- Distance fade (gate: no foliage beyond the live ring). ---
    void set_fade_distances(float start_m, float end_m) {
        m_fade_start_m = start_m;
        m_fade_end_m = end_m;
    }
    float fade_start_m() const {
        return m_fade_start_m;
    }
    float fade_end_m() const {
        return m_fade_end_m;
    }

    //  #1b-lush: render-only density multiplier for SHOWCASE/photo scenes (owner:
    // lush-per-preset, default untouched). Scales both the candidate count and the
    // accept fraction, so a scene can reach near-continuous turf WITHOUT raising the
    // biome's vegetation density (which folds into the biome content-hash / params
    // marker — determinism-adjacent). Default 1.0 == byte-identical to the biome-tracked
    // density; the FoliageInstancing gate (which runs the default) is unaffected.
    void set_density_scale(float scale) {
        m_density_scale = scale > 0.0f ? scale : 1.0f;
        ++m_chunk_cache_gen;
    }
    float density_scale() const {
        return m_density_scale;
    }

    // --- Per-frame wind bridge (one-way). The caller pushes the camera-region
    // wind vector sampled from the  wind field; per-instance sway is the wind
    // projected at the instance (cheap distance-attenuated copy).. ---
    void set_wind(const glm::vec2& wind_xz) {
        m_wind_xz = wind_xz;
    }
    glm::vec2 wind() const {
        return m_wind_xz;
    }
    // the GPU scatter path reads the generated blades back to the CPU
    // (m_instances) ONLY so the FoliageInstancing gate's instance_hash works.
    // That readback is a synchronous glGetBufferSubData -> a ~5 ms CPU stall on
    // the hot path. execute draws straight from the SSBO via glDrawArraysIndirect,
    // so normal play / the budget benchmark disable the readback (default ON keeps
    // the gate exact)..
    void set_readback_enabled(bool e) {
        m_readback_enabled = e;
    }
    // the gate must be able to tell a real instance count from the
    // play-mode kMaxInstances marker (readback OFF publishes the marker).
    bool readback_enabled() const {
        return m_readback_enabled;
    }
    void set_sway_strength(float amplitude, float speed) {
        m_sway_amplitude = amplitude;
        m_sway_speed = speed;
    }

    // One visible chunk's placement input. The caller (RenderPipeline) supplies
    // per-chunk biome id + density + surface samples; the pass scatters
    // instances deterministically inside the chunk footprint.
    struct ChunkScatter {
        glm::ivec2 chunk_xz{0, 0}; // chunk coords (X,Z) — placement hash input
        glm::vec3 origin{0.0f};    // world origin of the chunk column footprint
        float extent_m = 32.0f;    // chunk footprint side length (world units)
        u8 biome_id = 255;         // placement hash input
        float density = 0.0f;      // biome vegetation density [0,1]
    };

    // Surface query callback: returns the terrain surface world Y + a slope
    // estimate [0,1] (0 flat.. 1 steep) + moisture [0,1] at a world (x,z).
    // Supplied by the caller so the pass never depends on the world system.
    struct SurfaceSample {
        float height = 0.0f;
        float slope = 0.0f;    // [0,1]
        float moisture = 0.0f; // [0,1]
        bool valid = true;     // false skips the candidate (e.g. underwater)
    };
    using SurfaceQuery = SurfaceSample (*)(void* ctx, float world_x, float world_z);

    // Rebuilds the instance set for the supplied visible chunks. PURE function
    // of the chunk inputs + the surface query (no RNG). Fills the persistent
    // mapping for this frame..
    void rebuild_instances(const std::vector<ChunkScatter>& chunks,
                           SurfaceQuery query,
                           void* query_ctx,
                           const glm::vec3& camera_pos);

    // Draws the live foliage instances into the lit HDR target (ctx.lit_scene).
    // Reads scene depth for occlusion. No-op (returns 0) when no instances or
    // disabled. -T17: reads frame state from the RenderContext seam
    // (sun/ambient/moon/cloud/time/lit_scene) instead of RenderPipeline; RETURNS
    // the instance count drawn so the call site owns the stat bump.
    std::size_t execute(const RenderContext& ctx, const Camera& camera);

    // --- Gate hooks (FoliageInstancing). All PURE; never touch GL. ---
    // The deterministic placement hash, exposed so the gate can assert the
    // scatter is reproducible (same inputs -> same hash) independently.
    static uint64_t placement_hash(int chunk_x, int chunk_z, u8 biome_id, uint32_t instance_index);
    // FNV-1a over the live instance record bytes (stable, order-preserving):
    // the determinism surface for the gate.
    uint64_t instance_hash() const;
    // Count of instances whose anchor lies within `radius_m` of `center` — the
    // gate's coverage-density probe (instances within the live ring).
    std::size_t instances_within(const glm::vec3& center, float radius_m) const;
    // Max tip sway displacement magnitude across live instances this frame
    // (calm vs windy differs — the gate's wind-response probe).
    float max_sway_displacement() const;
    // Count of live instances beyond `radius_m` (the gate asserts this is 0 once
    // the fade end is inside the live ring).
    std::size_t instances_beyond(const glm::vec3& center, float radius_m) const;
    // Read-back of the CPU-side instance set (for the gate density/fade probes).
    const std::vector<InstanceRecord>& instances() const {
        return m_instances;
    }

private:
    void map_instances_for_frame();
    //  #4: GPU scatter rebuild (dispatch compute -> SSBO, read back into
    // m_instances for the gate/cache surface). Returns false if it could not run
    // (caller then falls back to the CPU loop).
    bool rebuild_instances_gpu(const std::vector<ChunkScatter>& chunks,
                               SurfaceQuery query,
                               void* query_ctx,
                               const glm::vec3& camera_pos);
    // drain the most-recent COMPLETED async blade readback
    // into m_instances (stale-safe). Called every frame from rebuild_instances so
    // the gate's instance_hash/coverage probes stay populated independent of the
    // scatter-cache elision..
    void poll_foliage_readback();

    //  implementation note (foliage streaming-burst amortization): build (or fetch the cached)
    // CAMERA-INDEPENDENT instance records for one chunk. The records (position/size/color/phase/
    // facing) are a pure function of chunk_xz + the static terrain surface query + density_scale +
    // archetypes, so they are computed ONCE per chunk and reused as the camera moves. The per-frame
    // rebuild then just copies these with the cheap camera distance-fade cull applied and a fresh
    // wind sway — eliminating the ~660ms re-query when moving. sway is baked as 0 here and set at
    // copy time so the output matches the uncached loop exactly..
    const std::vector<InstanceRecord>&
    build_or_get_chunk_records(const ChunkScatter& chunk, SurfaceQuery query, void* query_ctx);

    std::unique_ptr<Shader> m_shader;
    u32 m_vao = 0;

    // GPU scatter resources ( #4). m_gpu_scatter gates the whole path; when
    // false the CPU loop runs. m_gpu_active is true once a GPU build populated
    // m_blade_ssbo this session (execute then draws from it directly through
    // the command stored in m_count_ssbo).
    u32 m_compute_prog = 0;
    u32 m_chunk_ssbo = 0;
    u32 m_surf_ssbo = 0;
    u32 m_blade_ssbo = 0;
    u32 m_count_ssbo = 0;
    u32 m_arch_ssbo = 0;
    bool m_gpu_scatter = false;
    bool m_gpu_active = false;
    bool m_readback_enabled = true; // gate needs CPU readback; play/benchmark disable it
    // the gate-only blade readback routes through this async
    // ring instead of a synchronous glGetBufferSubData, so it never blocks the
    // frame. m_instances holds the LAST-COMPLETED result (replaced only when the
    // ring delivers a newer one) -> it is never re-emptied, keeping instance_hash
    // stable + non-empty once primed (an empty hash would break the gate). Lazily
    // allocated on first readback use, so the play path (readback disabled) pays
    // nothing..
    AsyncReadbackRing m_readback_ring;
    std::array<u32, kRingFrames> m_instance_vbo{};
    std::array<InstanceRecord*, kRingFrames> m_instance_ptr{};
    std::size_t m_ring_cursor = 0;

    std::vector<ArchetypeData> m_archetypes;
    // CPU-side mirror of the instances built this frame (gate read-back source).
    std::vector<InstanceRecord> m_instances;
    std::size_t m_frame_instance_count = 0;

    bool m_enabled = false;
    glm::vec2 m_wind_xz{0.0f, 0.0f};
    float m_sway_amplitude = 0.25f;
    float m_sway_speed = 1.6f;
    float m_fade_start_m = 96.0f;
    float m_fade_end_m = 160.0f;

    //  scatter cache: rebuild_instances is called EVERY frame, but the instance set
    // only changes when the visible chunk-set, the camera chunk (the coarse per-chunk
    // fade cull), or the wind changes. Skip the per-frame CPU rebuild + GPU upload when
    // the signature is unchanged — the ring buffer + frame_instance_count from the last
    // build are reused (execute redraws the same VBO). This removes the per-frame CPU
    // cost that capped scatter density. Determinism-neutral (render-only).
    std::uint64_t m_last_scatter_sig = 0;
    bool m_scatter_built = false;
    // denser default foliage (owner: fuller ground cover). GPU-scattered
    // blades are cheap, so a modest lift fills the dusk/low-sun fields without a
    // meaningful perf cost. Deeper grass work (moonlit grass, dusk brightness,
    // -grove shading) is the  foliage substrate.
    float m_density_scale = 1.35f; // #1b-lush: showcase density multiplier (1.0 = baseline)

    //  implementation note: per-chunk CAMERA-INDEPENDENT instance cache (see
    //  build_or_get_chunk_records).
    // Keyed by packed chunk_xz. Each entry stores the generation it was built at; when
    // m_chunk_cache_gen bumps (density scale / archetypes changed) the entry is stale and rebuilt
    // on next use. Bounded by pruning chunks absent from the current renderable set once the cache
    // grows past a soft cap.
    struct CachedChunkRecords {
        std::uint64_t gen = 0;
        std::vector<InstanceRecord> records;
    };
    std::unordered_map<std::uint64_t, CachedChunkRecords> m_chunk_cache;
    std::uint64_t m_chunk_cache_gen = 1;
    //  implementation note: the GPU scatter path (rebuild_instances_gpu, the path that actually
    //  runs
    // in normal play) sampled the per-chunk SURFACE GRID (kSurfaceGridVerts^2 GetTerrainHeightAt
    // calls) on the CPU for EVERY renderable chunk on every rebuild — ~1s when moving. The grid is
    // a pure function of chunk_xz + the static terrain, so cache it per chunk (keyed by packed
    // chunk_xz) and rebuild only a budgeted few new chunks per frame. Keyed identically to
    // m_chunk_cache.
    std::unordered_map<std::uint64_t, std::vector<glm::vec4>> m_surf_grid_cache;
    //  implementation note: when MOVING fast, many chunks stream into the renderable set in one
    //  frame,
    // and building their (uncached) records all at once re-ran hundreds of SurfaceQuery calls -> a
    // ~960ms hitch. Budget the per-frame chunk-record BUILDS; chunks over budget contribute no
    // foliage this frame and build over the next few frames (the foliage fades in —, so
    // no determinism impact). While a build backlog exists the scatter-cache elision is suppressed
    // so the rebuild keeps draining it even when the camera is still.
    bool m_foliage_build_backlog = false;
};

} // namespace Luminumbra::Rendering
