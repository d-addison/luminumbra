#pragma once

#include "../RenderPipeline.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

class Camera;
class Shader;

// ===========================================================================
// T-I5a-1: GPU particle framework.
//
// A transparent, forward-lit, billboarded particle system slotted AFTER the
// SkyboxPass and BEFORE the final blit. It blends emissive particles into the
// lit HDR (RGBA16F) lighting target, reading the G-buffer depth for
// soft-particle alpha fade (and for spawn-region clip).
//
// DESIGN (pinned, design-decisions §3):
//  - FIXED-CAPACITY, persistent-mapped instance buffer (the same
//    glBufferStorage + GL_MAP_PERSISTENT|COHERENT pattern as the T-I4-16
//    ChunkGeometryPool). NOT compute / transform feedback.
//  - Global pool: 65,536 instances, ring-recycled (oldest evicted on overflow).
//  - 256 concurrent emitters max.
//  - Instance stride 24 B:
//        pos        3 * f32  (12 B)
//        size       1 * f32  ( 4 B)
//        color      rgba8    ( 4 B)
//        atlasLayer u16      ( 2 B)
//        rotation   f16      ( 2 B)
//  - Instanced draw: 4 verts/instance (gl_VertexID quad expansion in the vertex
//    shader); no geometry shader (the Shader class is vert+frag only). The
//    billboard expansion + emissive shading that previously lived in
//    magical_particles.{geom,frag} is re-homed onto this framework.
//
// DETERMINISM SURFACE (critique F2, CRITICAL):
//  - The EMITTER SCHEDULE is sim-deterministic. An EmitterDescriptor
//    {id, type, origin-region, spawn-rate, rng_seed, enable} is a pure function
//    of world state at a tick; rng_seed is derived deterministically from world
//    state (see derive_emitter_seed). The descriptor SET is the snapshot
//    surface for the determinism gate.
//  - Particle MOTION (per-particle position/velocity/age) is RENDER-ONLY. It is
//    driven by render time, never enters world_hash, and is never snapshotted.
//  - ONE-WAY RULE: this subsystem reads sim/world state but NEVER writes back
//    into any sim/world_hash input.
// ===========================================================================
class ParticlePass {
public:
    // --- Pinned capacities (design-decisions §3). ---
    static constexpr std::size_t kMaxInstances = 65536;  // global ring pool
    static constexpr std::size_t kMaxEmitters = 256;     // concurrent emitters
    // Instance stride: pos(12) + size(4) + color(4) + atlasLayer(2) + rot(1) +
    // aspect(1). T-I5a-DR-atmospheric-visuals: the final 2-byte slot, formerly a
    // single f16 rotation, now packs an snorm8 rotation (angle/pi in [-1,1]) plus
    // a unorm8 streak ELONGATION (so rain renders as a stretched velocity-aligned
    // streak, not a round dot). The 24-byte stride + 24-byte static_assert are
    // unchanged, so the ParticleDeterminism InstanceRecordIs24Bytes contract holds.
    static constexpr std::size_t kInstanceStride = 24;
    // Streak aspect quantization: aspect in [1, ~16] -> unorm8 (aspect/16*255).
    static constexpr float kMaxStreakAspect = 16.0f;
    // Double-buffer the persistent mapping so the CPU writes frame N+1 while the
    // GPU may still be reading frame N (avoids a coherent-write hazard).
    static constexpr std::size_t kRingFrames = 2;

    // Packed 24-byte instance record (matches the GL vertex-attribute layout).
    // Trivially copyable; written straight into the persistent mapping.
#pragma pack(push, 1)
    struct InstanceRecord {
        float    pos[3];      // world position
        float    size;        // billboard half-extent (world units)
        uint8_t  color[4];    // rgba8 (a = emissive opacity scale)
        uint16_t atlas_layer; // array-texture layer
        int8_t   rotation;    // snorm8 rotation (angle/pi in [-1,1])
        uint8_t  streak;      // unorm8 streak aspect (aspect/kMaxStreakAspect)
    };
#pragma pack(pop)
    static_assert(sizeof(InstanceRecord) == kInstanceStride,
                  "particle instance must be 24 bytes (design-decisions §3)");

    // Engine-generic curve over normalized life [0,1] sampled at N control
    // points (the emitter DATA owns the values; the engine only knows the
    // schema). Linear interpolation between control points.
    static constexpr std::size_t kCurvePoints = 5;
    struct Curve {
        std::array<float, kCurvePoints> points{{1.0f, 1.0f, 1.0f, 1.0f, 1.0f}};
        float sample(float t) const;
    };

    enum class BlendMode : uint8_t {
        Additive = 0, // emissive glow (default for magical particles)
        AlphaBlend,   // standard transparency
    };

    // Emitter DATA loaded from data/common/particles/*.json. The engine knows
    // only this schema; the values are game content.
    struct EmitterData {
        std::string name;
        uint32_t type = 0;             // shader shape selector (0..3)
        float spawn_rate = 0.0f;       // particles / second
        float lifetime = 1.0f;         // seconds
        glm::vec3 origin{0.0f};        // emission region centre (world)
        glm::vec3 origin_extent{0.0f}; // half-extents of the spawn box
        glm::vec3 base_velocity{0.0f};
        float velocity_jitter = 0.0f;
        Curve size_curve;              // size over normalized life
        Curve r_curve;                 // colour r/g/b/a over normalized life
        Curve g_curve;
        Curve b_curve;
        Curve a_curve;
        uint16_t atlas_layer = 0;
        BlendMode blend = BlendMode::Additive;
        // T-I5a-4 (B2 precipitation): wind-advection + streak + splash extensions.
        // wind_response scales how much the per-frame wind velocity (set via
        // set_wind) is applied to this emitter's particles -- rain/snow slant with
        // wind; magical emitters default to 0 (unaffected). streak_aspect drives
        // the billboard elongation along the velocity direction (>1 = a rain
        // streak; 1 = a round flake). impact_splash converts a particle that
        // descends past impact_plane_y into a short splash burst (depth/ground
        // impact). All render-only -- NONE of these touch world_hash.
        float wind_response = 0.0f;
        float streak_aspect = 1.0f;
        bool impact_splash = false;
        float impact_plane_y = 0.0f;
        bool loaded = false;
    };

    // Sim-deterministic emitter descriptor: the snapshot surface for the gate.
    // POD, trivially serializable byte-for-byte. NOTHING here depends on render
    // time or particle motion.
#pragma pack(push, 1)
    struct EmitterDescriptor {
        uint32_t id = 0;          // stable emitter id
        uint32_t type = 0;        // shape selector
        int32_t  origin_region[3]{}; // quantized world region (mm) -> integer
        uint32_t spawn_rate_milli = 0; // spawn_rate * 1000, quantized to integer
        uint64_t rng_seed = 0;    // derived deterministically from world state
        uint32_t enable = 0;      // 0/1
    };
#pragma pack(pop)
    static_assert(sizeof(EmitterDescriptor) == 36,
                  "emitter descriptor must stay a fixed-size POD for byte-equal snapshots");

    ParticlePass();
    ~ParticlePass();

    void init_shader(const std::filesystem::path& root_path);
    void init_buffers();
    void destroy_buffers();
    void reset_shader();

    const std::unique_ptr<Shader>& shader() const { return m_shader; }
    u32 vao() const { return m_vao; }
    u32 instance_buffer(std::size_t ring) const { return m_instance_vbo[ring % kRingFrames]; }
    bool has_emitters() const { return !m_active_emitters.empty(); }
    std::size_t live_particle_count() const { return m_live_count; }

    // --- Emitter lifecycle (game-data driven). ---
    // Loads an emitter descriptor from data/common/particles/<file>. Returns the
    // emitter id, or kInvalidEmitter on failure.
    static constexpr uint32_t kInvalidEmitter = 0xFFFFFFFFu;
    uint32_t add_emitter(const std::filesystem::path& json_path, const glm::vec3& world_origin);
    void clear_emitters();

    // T-I5a-DR-storm-motion-v4: RE-CENTER an existing emitter's spawn region on a
    // new base origin (render-only). `base_origin` is the same un-offset world
    // anchor that add_emitter takes -- the emitter's data.origin (e.g. the rain
    // column's [0,22,0] height offset) is re-applied internally so the spawn box
    // keeps its authored height above the anchor. The precipitation scenario calls
    // this every frame with the LIVE camera position so the rain volume FOLLOWS
    // the moving camera: new drops always spawn AROUND/ABOVE the viewer and fall
    // straight past it, while in-flight drops keep their existing trajectories.
    // No-op for an unknown id. Touches only render state -- never world_hash.
    void set_emitter_origin(uint32_t emitter_id, const glm::vec3& base_origin);

    // T-I5a-4 (B2): registers a SPLASH emitter (a zero-spawn-rate burst template)
    // that impact_splash particles trigger when they reach the impact plane. The
    // emitter's spawn_rate is forced to 0 so it produces nothing on its own --
    // only impact events spawn from it. Returns its emitter id (or kInvalidEmitter).
    // Render-only; the splash template carries NO descriptor enable (spawn_rate 0).
    uint32_t add_splash_emitter(const std::filesystem::path& json_path);

    // T-I5a-4 (B2): per-frame WIND velocity (world-space; horizontal XZ carried in
    // x/z, y usually 0) sampled by the client from the A2 wind field / replicated
    // weather at the camera. Applied to particle motion scaled by each emitter's
    // wind_response so rain/snow SLANT in storms. RENDER-ONLY -- never hashed, and
    // this subsystem writes nothing back into sim/world_hash (one-way, critique F2).
    void set_wind(const glm::vec3& wind_velocity) { m_wind_velocity = wind_velocity; }
    const glm::vec3& wind_velocity() const { return m_wind_velocity; }

    // Rebuilds the sim-deterministic emitter descriptor set for the supplied
    // world tick. rng_seed is derived from {world_seed, tick, emitter id}. This
    // is the ONLY surface snapshotted by the determinism gate.
    void rebuild_emitter_descriptors(uint64_t world_seed, uint64_t world_tick);
    const std::vector<EmitterDescriptor>& emitter_descriptors() const { return m_descriptors; }
    // FNV-1a hash of the descriptor set bytes (stable, order-preserving).
    uint64_t emitter_descriptor_hash() const;

    // Derives the deterministic per-emitter RNG seed from world state. PUBLIC so
    // the determinism gate can assert the derivation independently.
    static uint64_t derive_emitter_seed(uint64_t world_seed, uint64_t world_tick, uint32_t emitter_id);

    // Advances RENDER-ONLY particle motion by dt and refills the persistent
    // mapping for this frame. Spawn counts use the deterministic per-emitter
    // seed so even the render-side spawn pattern is reproducible (it still never
    // feeds world_hash). A no-op (zero GL writes) when no emitters exist.
    void update(float dt);

    // Blends the live particles into the lighting HDR target. Reads the
    // G-buffer depth for soft-particle fade. No-op when no live particles.
    void execute(RenderPipeline& pipeline, const Camera& camera);

private:
    struct ActiveEmitter {
        EmitterData data;
        uint32_t id = 0;
        glm::vec3 world_origin{0.0f};
        uint64_t rng_seed = 0;
        double spawn_accumulator = 0.0; // fractional particles carried frame to frame
        uint64_t rng_state = 0;         // xorshift state, render-only
    };

    // Render-only particle (motion state lives here, NEVER snapshotted).
    struct Particle {
        glm::vec3 pos{0.0f};
        glm::vec3 vel{0.0f};
        float age = 0.0f;
        float lifetime = 1.0f;
        uint32_t emitter_index = 0;
        bool alive = false;
    };

    void spawn_from_emitter(ActiveEmitter& emitter, float dt);
    void map_instances_for_frame();
    // T-I5a-4: spawn a small splash burst at world_pos using the registered
    // splash template emitter (no-op when none registered). Render-only.
    void spawn_splash_burst(const glm::vec3& world_pos, int count, ActiveEmitter& splash_template);

    std::unique_ptr<Shader> m_shader;
    u32 m_vao = 0;
    std::array<u32, kRingFrames> m_instance_vbo{};
    std::array<InstanceRecord*, kRingFrames> m_instance_ptr{};
    std::size_t m_ring_cursor = 0;

    std::vector<ActiveEmitter> m_active_emitters;
    std::vector<EmitterDescriptor> m_descriptors;

    // Ring buffer of live particles (oldest evicted on overflow).
    std::vector<Particle> m_particles;
    std::size_t m_ring_head = 0; // next write slot
    std::size_t m_live_count = 0;

    // Per-frame instance count actually written to the mapping.
    std::size_t m_frame_instance_count = 0;
    uint32_t m_next_emitter_id = 0;

    // T-I5a-4 (B2): per-frame wind velocity (render-only) + splash template index.
    glm::vec3 m_wind_velocity{0.0f};
    static constexpr std::size_t kNoSplashEmitter = static_cast<std::size_t>(-1);
    std::size_t m_splash_emitter_index = kNoSplashEmitter;
};

} // namespace Luminumbra::Rendering
