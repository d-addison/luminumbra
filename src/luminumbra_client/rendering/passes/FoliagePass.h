#pragma once

#include "../RenderPipeline.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

class Camera;
class Shader;

// ===========================================================================
// T-I5b-1 (F1): instanced foliage scatter + wind response. RENDER-ONLY.
//
// One instanced scatter system covering grass/tufts, pebbles/gravel, clutter
// (twigs/shells) and canopy, drawn per visible chunk from a fixed-capacity,
// persistent-mapped instance pool (the SAME glBufferStorage +
// GL_MAP_PERSISTENT|COHERENT pattern as the A1 ParticlePass / T-I4-16 chunk
// pool). NOT compute / transform feedback.
//
// PLACEMENT (PINNED, design-decisions §2): a DETERMINISTIC PURE FUNCTION of
// (chunk coords, biome id, slope, moisture, instance index) via a splitmix64
// hash. NO global RNG, NO world-seed offset is consumed (foliage is render-only
// and per-chunk, so no +14 seed is taken — design §1). Density is driven by the
// biome table's vegetation/cover block (the iter-4 parsed-not-consumed hook,
// now consumed render-side), modulated by slope and moisture.
//
// WIND (design-decisions §2): each instance carries a per-instance wind
// displacement sampled CPU-side from the A2 WindFieldSystem (the one-way
// replicated render bridge — same pattern C3 clouds used). The vertex shader
// bends the card tip by that displacement scaled by a per-ARCHETYPE sway flag
// (grass/canopy wave; pebbles/clutter do not). Distance-faded against the
// far-LOD horizon (no foliage in far tiles).
//
// ONE-WAY RULE (critique F2): this subsystem READS sim/world state (biome,
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
    static constexpr std::size_t kMaxCandidatesPerChunk = 256;

    // Packed 36-byte instance record (matches the GL vertex-attribute layout).
#pragma pack(push, 1)
    struct InstanceRecord {
        float    pos[3];   // world ground anchor
        float    size[2];  // half-width, height (world units)
        uint8_t  color[4]; // rgba8 (a = per-archetype sway flag scale 0..255)
        float    sway[2];  // per-instance wind displacement at the tip (world XZ)
        uint16_t phase;    // f16 sway phase offset (radians)
        uint16_t facing;   // f16 card yaw in the XZ plane (radians)
    };
#pragma pack(pop)
    static_assert(sizeof(InstanceRecord) == kInstanceStride,
                  "foliage instance must be 36 bytes");

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

    const std::unique_ptr<Shader>& shader() const { return m_shader; }
    u32 vao() const { return m_vao; }
    u32 instance_buffer(std::size_t ring) const { return m_instance_vbo[ring % kRingFrames]; }
    bool enabled() const { return m_enabled; }
    void set_enabled(bool on) { m_enabled = on; }
    std::size_t frame_instance_count() const { return m_frame_instance_count; }

    // Loads the scatter archetype set from data/common/foliage/scatter_set.json.
    // On failure the pass stays empty (no foliage). Returns true on success.
    bool load_scatter_set(const std::filesystem::path& json_path);
    std::size_t archetype_count() const { return m_archetypes.size(); }
    const ArchetypeData& archetype(std::size_t i) const { return m_archetypes[i]; }

    // --- Distance fade (gate: no foliage beyond the live ring). ---
    void set_fade_distances(float start_m, float end_m) {
        m_fade_start_m = start_m;
        m_fade_end_m = end_m;
    }
    float fade_start_m() const { return m_fade_start_m; }
    float fade_end_m() const { return m_fade_end_m; }

    // --- Per-frame wind bridge (one-way). The caller pushes the camera-region
    // wind vector sampled from the A2 wind field; per-instance sway is the wind
    // projected at the instance (cheap distance-attenuated copy). RENDER-ONLY. ---
    void set_wind(const glm::vec2& wind_xz) { m_wind_xz = wind_xz; }
    glm::vec2 wind() const { return m_wind_xz; }
    void set_sway_strength(float amplitude, float speed) {
        m_sway_amplitude = amplitude;
        m_sway_speed = speed;
    }

    // One visible chunk's placement input. The caller (RenderPipeline) supplies
    // per-chunk biome id + density + surface samples; the pass scatters
    // instances deterministically inside the chunk footprint.
    struct ChunkScatter {
        glm::ivec2 chunk_xz{0, 0};   // chunk coords (X,Z) — placement hash input
        glm::vec3 origin{0.0f};      // world origin of the chunk column footprint
        float extent_m = 32.0f;      // chunk footprint side length (world units)
        u8 biome_id = 255;           // placement hash input
        float density = 0.0f;        // biome vegetation density [0,1]
    };

    // Surface query callback: returns the terrain surface world Y + a slope
    // estimate [0,1] (0 flat .. 1 steep) + moisture [0,1] at a world (x,z).
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
    // mapping for this frame. RENDER-ONLY.
    void rebuild_instances(const std::vector<ChunkScatter>& chunks,
                           SurfaceQuery query, void* query_ctx,
                           const glm::vec3& camera_pos);

    // Draws the live foliage instances into the lit HDR target. Reads scene
    // depth for occlusion. No-op when no instances or disabled.
    void execute(RenderPipeline& pipeline, const Camera& camera);

    // --- Gate hooks (FoliageInstancing). All PURE; never touch GL. ---
    // The deterministic placement hash, exposed so the gate can assert the
    // scatter is reproducible (same inputs -> same hash) independently.
    static uint64_t placement_hash(int chunk_x, int chunk_z, u8 biome_id,
                                    uint32_t instance_index);
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
    const std::vector<InstanceRecord>& instances() const { return m_instances; }

private:
    void map_instances_for_frame();

    std::unique_ptr<Shader> m_shader;
    u32 m_vao = 0;
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
};

} // namespace Luminumbra::Rendering
