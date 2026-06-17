#pragma once

#include "../../include/luminumbra/core/Types.h"
#include <glad/glad.h>
#include <array>
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <utility>
#include <glm/glm.hpp>
#include "Mesh.h"
#include "SkyAtmosphereLut.h" // T-I5a-6: Hillaire 2020 scattering LUTs
#include "WaterfallDetect.h"  // T-I5b-4: world-deterministic waterfall sites
#include <map>
#include "core/AssetManager.h"
#include "core/IsolationConfig.h"  // T-I6: isolation/layer render mode (backdrop + spawn-suppression)
#include <filesystem>
#include "luminumbra_common/components/LightingComponents.h"
#include "luminumbra_common/world/Chunk.h"

// Forward declarations
namespace Luminumbra { class Chunk; class JobSystem; }
namespace Luminumbra::Systems { class SHIELD_WorldSystem; struct TerrainGenParams; }
namespace Luminumbra::Rendering { class Shader; class Camera; class ShadowPass; class GBufferPass; class SsaoPass; class LightingPass; class WaterPass; class SkyboxPass; class ParticlePass; class FoliagePass; class FarLodSystem; class ShieldRtFarFieldPass; }

namespace Luminumbra::Rendering {

struct DirectionalLight {
    glm::vec3 direction = glm::normalize(glm::vec3(0.5f, -1.0f, -0.5f));
    glm::vec3 color = glm::vec3(1.0f, 0.95f, 0.85f);
    float intensity = 1.0f;
};

struct PointLight {
    glm::vec3 position;
    float radius; // Using std140 layout padding for future UBO compatibility
    glm::vec3 color;
    float intensity;
};

struct GBuffer {
    u32 fbo_id = 0;
    u32 position_texture = 0;
    u32 normal_texture = 0;
    u32 albedo_texture = 0;
    u32 material_texture = 0;
    u32 depth_texture = 0; 
};

struct ShadowMap {
    u32 fbo_id = 0;
    u32 depth_texture_array = 0;
    u32 resolution = 2048;
    static constexpr int CASCADE_COUNT = 4;
    std::vector<glm::mat4> light_space_matrices;
    std::vector<float> cascade_splits;
};

// T-I4-16: a live chunk's terrain geometry now lives inside the shared
// bucketed persistent-mapped geometry pool (ChunkGeometryPool) instead of a
// dedicated VAO/VBO/EBO per chunk. ChunkRenderData keeps the per-chunk
// LIFECYCLE bookkeeping (mark-and-sweep TTL, mesh_version, capacity for the
// distance-budgeted upload selection in manage_chunk_gpu_resources) and now
// records the pool slice the chunk occupies. The legacy vao/vbo/ebo fields are
// retained ONLY for the still-per-chunk water path (WaterRenderData mirrors
// this layout); terrain leaves them 0 and the resource-registry stats count
// pool blocks instead (see get_resource_registry_stats). pool_handle == kInvalid
// means "not pool-resident".
struct ChunkRenderData {
    u32 vao_id = 0;
    u32 vbo_id = 0;
    u32 ebo_id = 0;
    u32 element_count = 0;
    u32 mesh_version = 0;
    u32 vertex_capacity = 0;
    u32 index_capacity = 0;
    u32 frames_since_inactive = 0;
    // T-I4-16 pool slice. Packs {block_index, vertex/index slot offsets}.
    static constexpr u32 kInvalidPoolHandle = 0xFFFFFFFFu;
    u32 pool_handle = kInvalidPoolHandle;
};

struct WaterRenderData {
    u32 vao_id = 0;
    u32 vbo_id = 0;
    u32 ebo_id = 0;
    u32 element_count = 0;
    u32 mesh_version = 0;
    u32 vertex_capacity = 0;
    u32 index_capacity = 0;
    u32 frames_since_inactive = 0;
};

struct FrameBufferObject {
    u32 fbo_id = 0;
    u32 color_texture = 0;
    u32 opaque_color_texture = 0;
    u32 depth_texture = 0;
};

struct SSAOData {
    GLuint fbo = 0, blurFBO = 0;
    GLuint ssaoColorBuffer = 0, ssaoColorBufferBlur = 0;
    GLuint noiseTexture = 0;
    std::vector<glm::vec3> kernel;
    std::unique_ptr<Shader> ssaoShader;
    std::unique_ptr<Shader> blurShader;
};

// Engine-generic runtime weather state (T-I2-17b). Default Off: the weather
// overlay issues zero GL work unless a weather type with intensity > 0 is set.
enum class WeatherType {
    None = 0,
    Rain,
    Snow,
    Fog,
    Storm,
};

// T-I5a-3 (B1): SIM-DRIVEN weather render state. This is the one-way (critique
// F2) bridge from the replicated WeatherSystem state to the render overlay +
// wetness response: the client samples WeatherSystem at the camera each frame and
// pushes this POD via RenderPipeline::set_weather_state. The render side READS it
// and writes NOTHING back into the sim. All fields are derived weather quantities;
// the overlay's u_rainIntensity/u_snowIntensity/u_fogDensity/u_stormIntensity/
// u_windDirection/u_windStrength uniforms are fed from here instead of the legacy
// set_weather debug mapping. driven=false falls back to the legacy debug path so
// existing set_weather callers (and the None default) are unchanged.
struct WeatherRenderState {
    bool driven = false;          // true once a sim weather state has been pushed
    float rain_intensity = 0.0f;  // [0, 1]
    float snow_intensity = 0.0f;  // [0, 1]
    float fog_density = 0.0f;     // [0, 1]
    float storm_intensity = 0.0f; // [0, 1]
    float wetness = 0.0f;         // [0, 1] local precipitation -> material wetness
    glm::vec3 wind_direction = glm::vec3(1.0f, 0.0f, 0.0f); // normalized XZ wind
    float wind_strength = 0.0f;   // [0, 1] wind magnitude (scaled)
};

// T-I5a-8 (C3): RENDER-ONLY cloud layer state. The cloud coverage field + its
// projected cast shadow are a pure function of (replicated weather state + sim
// tick + wind) — one-way, never read back into the sim or world_hash (critique
// F2). The client pushes this each frame via set_cloud_state; the SkyboxPass
// renders the wind-advected sky-dome cloud layer and the LightingPass projects
// the SAME coverage field to cast crawling terrain shadows. The scroll offset is
// the wind direction * a tick-derived phase, so the clouds drift deterministically
// with the large-scale wind and the dome/shadow stay registered.
struct CloudRenderState {
    bool enabled = false;             // master toggle (false == zero added cost)
    bool shadow_enabled = false;      // project the coverage into the lighting pass
    glm::vec2 scroll_offset = glm::vec2(0.0f); // wind * tick-phase, world metres
    float coverage_amount = 0.45f;    // [0,1] weather sky-cover fraction
    float biome_variation = 0.0f;     // biome coverage bias (e.g. wetter == cloudier)
    float plane_height = 900.0f;      // world Y of the cloud sheet
    float shadow_strength = 0.0f;     // [0,1] max sun darkening under a cloud core
    glm::vec3 sun_travel_dir = glm::vec3(0.0f, -1.0f, 0.0f); // for shadow projection
};

// T-I5a-5 (B3): RENDER-ONLY lightning state for a single captured frame. A strike
// is a deterministic SIM world event (Systems::StrikeEvent, in the `weather`
// world_hash sub-hash); this is the ONE-WAY (critique F2) render response the
// client pushes for the frame(s) the bolt is visible: a full-scene LIGHT PULSE
// injected through the lighting pass + a screen-space BOLT polyline rasterized in
// the same pass (no new GL objects). Nothing here is hashed or written back to sim.
//
//  - pulse_intensity  scales a full-scene additive luminance spike (the 1-to-few-
//    frame flash); 0 == the zero-cost OFF path (no added lighting work).
//  - pulse_color      the flash tint (cool white-blue by default).
//  - strike_ndc       the strike ground point projected to NDC [-1,1] (for a mild
//    radial brightening centred on the strike).
//  - bolt_points_ndc  the bolt polyline (main channel + branches, flattened with
//    NaN-x separators) in NDC; the lighting frag adds bright pixels near any
//    segment so the capture shows a thin high-gradient structure.
inline constexpr int kMaxBoltSegmentPoints = 96; // GLSL uniform array cap
struct LightningRenderState {
    bool active = false;                    // master toggle (false == zero added cost)
    float pulse_intensity = 0.0f;           // [0,~3] full-scene additive flash strength
    glm::vec3 pulse_color = glm::vec3(0.72f, 0.82f, 1.0f); // cool flash tint
    glm::vec2 strike_ndc = glm::vec2(0.0f); // strike point in NDC (radial centre)
    float bolt_width_ndc = 0.004f;          // bolt core half-width in NDC units
    float bolt_glow_ndc = 0.018f;           // bolt glow falloff radius in NDC units
    // T-I5a-DR-storm-motion-v2: GROUND-IMPACT bloom at the bolt touchdown point so
    // the strike visibly CONNECTS to terrain. ground_ndc is the projected terminus;
    // ground_flash scales the radial impact glow (0 = off, keeps gates byte-stable).
    glm::vec2 ground_ndc = glm::vec2(0.0f, -1.0f);
    float ground_flash = 0.0f;
    // T-I5a-DR-storm-motion-v3: DARK STORM CLOUD anchor. The owner saw the bolt
    // "appear from thin air". cloud_anchor_ndc is the screen point at the TOP of the
    // bolt (where it should emerge from the cloud base); cloud_darkness scales a
    // dark, billowing cloud mass the overlay paints around that anchor so the bolt
    // visibly STEMS FROM a cloud and the flash lights that cloud from within.
    // 0 == no cloud overlay (keeps the WeatherVisual strike gate byte-stable).
    glm::vec2 cloud_anchor_ndc = glm::vec2(0.0f, 0.85f);
    float cloud_darkness = 0.0f;
    // Flattened NDC polyline points. A point with x <= -2.0 is a PEN-UP separator
    // between disjoint polylines (main channel / each branch). Drawn as connected
    // segments between consecutive non-separator points.
    std::vector<glm::vec2> bolt_points_ndc;
};

// T-I4-16: bucketed persistent-mapped geometry pool for live terrain chunks.
//
// Replaces the one-VBO/EBO/VAO-per-chunk model with a small set of large,
// immutable, persistently+coherently mapped GL buffers ("blocks"). Each block
// owns a vertex buffer, an index buffer, and a VAO with the VoxelVertex
// attribute layout pre-bound, and acts as ONE glMultiDrawElementsIndirect
// bucket. A chunk's geometry is sub-allocated as a contiguous vertex slice and
// a contiguous index slice inside a single block (a free-list suballocator per
// block, best-fit). Uploads memcpy straight into the persistent mapping (no
// glBufferSubData / driver staging copy). Draw submission builds one indirect
// command per visible chunk (firstIndex/baseVertex into the block) plus a
// per-draw chunk origin, and issues one MDI per block that has visible chunks.
//
// GL floor: glBufferStorage + GL_MAP_PERSISTENT_BIT (GL 4.4) and
// glMultiDrawElementsIndirect (GL 4.3). The engine requests a 4.5 core context
// (main_client.cpp), so both are guaranteed; no runtime capability fallback.
class ChunkGeometryPool {
public:
    static constexpr u32 kInvalid = 0xFFFFFFFFu;

    // One per-chunk allocation record. block_index selects the GL block; the
    // vertex/index slices are expressed in ELEMENTS (vertices / indices), so the
    // draw command derives baseVertex = vertex_offset and firstIndex =
    // index_offset directly.
    struct Allocation {
        u32 block_index = 0;
        u32 vertex_offset = 0;   // first vertex (baseVertex)
        u32 vertex_count = 0;    // vertices written
        u32 vertex_slot = 0;     // capacity of the reserved vertex slot
        u32 index_offset = 0;    // first index (firstIndex)
        u32 index_count = 0;     // indices written (drawn element_count)
        u32 index_slot = 0;      // capacity of the reserved index slot
        bool live = false;
    };

    // GL handles + persistent pointers + free lists for one pool block.
    struct Block {
        u32 vao = 0;
        u32 vbo = 0;
        u32 ebo = 0;
        VoxelVertex* vertex_ptr = nullptr; // persistent+coherent mapping
        u32* index_ptr = nullptr;
        u32 vertex_capacity = 0;           // vertices
        u32 index_capacity = 0;            // indices
        u32 vertex_high_water = 0;         // bump allocator frontier
        u32 index_high_water = 0;
        // Free slices returned by freed chunks, reused best-fit before bumping.
        std::vector<std::pair<u32, u32>> free_vertex_slices; // {offset, size}
        std::vector<std::pair<u32, u32>> free_index_slices;
    };

    ChunkGeometryPool() = default;

    // Allocates a slice for {vertex_count, index_count}, growing the pool with a
    // fresh block if no existing block can host it. Writes the geometry into the
    // persistent mapping. Returns a handle, or kInvalid on failure (e.g. a mesh
    // larger than a whole block). label_seed feeds GL debug labels.
    u32 allocate(const VoxelVertex* vertices, u32 vertex_count,
                 const u32* indices, u32 index_count, const char* label_seed);
    // Overwrites an existing allocation in place when the new geometry fits the
    // reserved slots; otherwise frees and reallocates. Returns the (possibly
    // new) handle.
    u32 update(u32 handle, const VoxelVertex* vertices, u32 vertex_count,
               const u32* indices, u32 index_count, const char* label_seed);
    void free(u32 handle);

    const Allocation& allocation(u32 handle) const { return m_allocations[handle]; }
    const std::vector<Block>& blocks() const { return m_blocks; }
    std::size_t block_count() const { return m_blocks.size(); }
    std::size_t live_allocation_count() const { return m_live_count; }

    // Sum of reserved vertex/index slot capacities across live allocations
    // (feeds the runtime VRAM estimate, matching the old per-chunk accounting).
    void resident_capacity(std::size_t& vertices, std::size_t& indices) const;

    // Releases all GL objects + mappings. Safe to call with no current chunks.
    void destroy();
    bool empty() const { return m_blocks.empty(); }

private:
    u32 acquire_handle();
    void release_handle(u32 handle);
    bool reserve_in_block(Block& block, u32 vertex_count, u32 index_count,
                          u32& vertex_offset, u32& vertex_slot,
                          u32& index_offset, u32& index_slot);
    u32 add_block(u32 min_vertices, u32 min_indices, const char* label_seed);

    std::vector<Block> m_blocks;
    std::vector<Allocation> m_allocations;
    std::vector<u32> m_free_handles;
    std::size_t m_live_count = 0;
};

class RenderPipeline {
public:
    struct MeshUploadFrameStats {
        size_t snapshot_count = 0;
        size_t terrain_upload_candidates = 0;
        size_t terrain_uploads = 0;
        size_t terrain_payload_copies = 0;
        size_t terrain_payload_bytes = 0;
        size_t terrain_uploads_deferred = 0;
        size_t terrain_new_upload_candidates = 0;
        size_t terrain_stale_upload_candidates = 0;
        size_t terrain_new_uploads_selected = 0;
        size_t terrain_stale_uploads_selected = 0;
        size_t terrain_new_uploads_deferred = 0;
        size_t terrain_stale_uploads_deferred = 0;
        size_t terrain_deferred_nearer_than_selected = 0;
        float terrain_nearest_candidate_distance_sq = 0.0f;
        float terrain_farthest_selected_distance_sq = 0.0f;
        float terrain_nearest_deferred_distance_sq = 0.0f;
        size_t terrain_slots_created = 0;
        size_t terrain_slots_reused = 0;
        size_t terrain_slots_grown = 0;
        size_t terrain_upload_failures = 0;
        size_t water_upload_candidates = 0;
        size_t water_uploads = 0;
        size_t water_payload_copies = 0;
        size_t water_payload_bytes = 0;
        size_t water_uploads_deferred = 0;
        size_t water_new_upload_candidates = 0;
        size_t water_stale_upload_candidates = 0;
        size_t water_new_uploads_selected = 0;
        size_t water_stale_uploads_selected = 0;
        size_t water_new_uploads_deferred = 0;
        size_t water_stale_uploads_deferred = 0;
        size_t water_deferred_nearer_than_selected = 0;
        float water_nearest_candidate_distance_sq = 0.0f;
        float water_farthest_selected_distance_sq = 0.0f;
        float water_nearest_deferred_distance_sq = 0.0f;
        size_t water_slots_created = 0;
        size_t water_slots_reused = 0;
        size_t water_slots_grown = 0;
        size_t water_upload_failures = 0;
    };

    struct RenderPassFrameStats {
        size_t snapshot_count = 0;
        size_t culling_hierarchy_rebuilds = 0;
        size_t culling_hierarchy_chunks = 0;
        size_t terrain_visible_chunks = 0;
        size_t terrain_draws = 0;
        size_t terrain_indices_drawn = 0;
        // Far-LOD region meshes drawn in the G-buffer pass after the live
        // chunks (T-I3-9). Recorded inside the gbuffer GPU timer window.
        size_t far_region_draws = 0;
        size_t far_indices_drawn = 0;
        // Skinned (animated) meshes drawn in the G-buffer pass (T-I3-16).
        size_t skinned_draws = 0;
        size_t skinned_indices_drawn = 0;
        std::array<size_t, ShadowMap::CASCADE_COUNT> shadow_cascade_visible_chunks{};
        std::array<size_t, ShadowMap::CASCADE_COUNT> shadow_cascade_draws{};
        size_t shadow_draws = 0;
        size_t shadow_indices_drawn = 0;
        size_t ssao_draws = 0;
        size_t ssao_blur_draws = 0;
        size_t lighting_draws = 0;
        size_t water_draws = 0;
        size_t water_indices_drawn = 0;
        size_t skybox_draws = 0;
        // T-I5a-1: instanced particle draws (one per ParticlePass submit) and
        // the total particle instances drawn this frame. Both stay 0 when no
        // emitters are active (the pass is a no-op), keeping existing visual
        // gates byte-stable.
        size_t particle_draws = 0;
        size_t particles_drawn = 0;
        // T-I5b-1 (F1): instanced foliage scatter draws (one per FoliagePass
        // submit) and the total scatter instances drawn this frame. Both stay 0
        // when foliage is disabled (the pass is a no-op), keeping existing
        // visual gates byte-stable.
        size_t foliage_draws = 0;
        size_t foliage_instances_drawn = 0;
        size_t final_blits = 0;
        // Per-pass GPU timings sampled from a GL_TIMESTAMP query ring
        // (frame N publishes the timings recorded at frame N-2). Values stay
        // 0.0 when timers are unsupported or no sample has resolved yet.
        bool gpu_timers_supported = false;
        double shadow_gpu_ms = 0.0;
        double gbuffer_gpu_ms = 0.0;
        double ssao_gpu_ms = 0.0;
        double ssao_blur_gpu_ms = 0.0;
        double lighting_gpu_ms = 0.0;
        double water_gpu_ms = 0.0;
        double skybox_gpu_ms = 0.0;
        double particle_gpu_ms = 0.0; // T-I5a-1: ParticlePass GPU timer (≤ 0.8 ms budget)
        // T-I5b-1 (F1): FoliagePass GPU timer. The FoliageInstancing gate bounds
        // this against the pinned release budget (design §7). 0.0 when foliage is
        // disabled / no instances.
        double foliage_gpu_ms = 0.0;
        // T-I5a-6: analytic aerial-perspective term (a fullscreen pass wiring
        // volumetric_lighting.frag). Budget ≤ 0.3 ms (design §7).
        double aerial_gpu_ms = 0.0;
        // T-I5a-8: the lighting-pass GPU time on the last frame the cloud cast
        // shadow was ACTIVE (the projected coverage sample is per-fragment inside
        // the lighting pass — no separate pass to time). cloud_shadow_added_ms is
        // the incremental cost vs the clouds-off lighting baseline, the number the
        // CloudShadow gate bounds against the ≤ 0.4 ms budget (design §7, F3).
        double cloud_shadow_gpu_ms = 0.0;       // lighting pass ms with clouds on
        // T-I5a-5 (B3): the lighting-pass GPU time on the last frame the lightning
        // light-pulse was ACTIVE (the full-scene flash + bolt rasterization are both
        // per-fragment inside the lighting pass). The PerfRegression gate bounds this
        // against the transient ≤ 0.5 ms budget (design §7).
        double lightning_pulse_gpu_ms = 0.0;
        double final_blit_gpu_ms = 0.0;
        // T-I5a-6: sky scattering LUT precompute timings (CPU build + GL upload).
        // sky_full_precompute_ms is the startup one-shot (budget ≤ 8.0 ms on
        // release); sky_view_refresh_ms is the last per-frame sky-view recompute
        // when the sun moved past the refresh threshold (budget ≤ 0.2 ms),
        // staying 0.0 on frames with no refresh.
        double sky_full_precompute_ms = 0.0;
        double sky_view_refresh_ms = 0.0;
    };

    struct RenderPassMetadata {
        std::string name;
        std::vector<std::string> inputs;
        std::vector<std::string> outputs;
        u32 width = 0;
        u32 height = 0;
        std::string clear;
        std::string load_store;
        size_t draw_count = 0;
        size_t dispatch_count = 0;
    };

    struct RenderResourceRegistryStats {
        size_t framebuffers = 0;
        size_t textures = 0;
        size_t renderbuffers = 0;
        size_t buffers = 0;
        size_t vertex_arrays = 0;
        size_t shader_programs = 0;
        size_t terrain_slots = 0;
        size_t water_slots = 0;
        bool empty_after_shutdown = false;
    };

    struct ShaderHealthEntry {
        std::string name;
        bool ok = false;
        std::string diagnostic;
    };

    struct RuntimeRenderStats {
        size_t terrain_gpu_chunks = 0;
        size_t water_gpu_chunks = 0;
        size_t free_terrain_slots = 0;
        size_t free_water_slots = 0;
        size_t terrain_vertex_capacity = 0;
        size_t terrain_index_capacity = 0;
        size_t water_vertex_capacity = 0;
        size_t water_index_capacity = 0;
        size_t estimated_vram_bytes = 0;
        bool started = false;
        bool geometry_shader_ok = false;
        bool lighting_shader_ok = false;
        bool skybox_shader_ok = false;
        bool shadow_shader_ok = false;
        bool ssao_shader_ok = false;
        bool ssao_blur_shader_ok = false;
        bool water_shader_ok = false;
        bool instanced_static_mesh_shader_ok = false;
        bool gpu_sdf_initialized = false;
        bool gpu_sdf_compile_time_enabled = false;
        bool gpu_sdf_runtime_requested = false;
        bool gpu_sdf_runtime_allowed = false;
        bool gpu_sdf_callback_registered = false;
        bool gpu_sdf_cpu_fallback_active = true;
        bool terrain_texture_array_ok = false;
        bool material_lut_ok = false;
        size_t terrain_texture_fallback_layers = 0;
        // --- Texture-array residency (T-I4-6) ---
        // Resident .ltex texture bytes (mip chains included) across all
        // size-class arrays, the configured budget, and array/layer counts.
        // Surfaced in render telemetry; nothing samples the layers yet
        // (T-I4-7 wires shaders to the LUT layer indices).
        size_t texture_resident_bytes = 0;
        size_t texture_resident_budget_bytes = 0;
        bool texture_resident_within_budget = true;
        size_t texture_residency_array_count = 0;
        size_t texture_residency_layer_count = 0;
    };

    struct GpuSdfRuntimeToggleState {
        bool compile_time_enabled = false;
        bool runtime_requested = false;
        bool runtime_allowed = false;
        bool callback_registered = false;
        bool cpu_fallback_active = true;
    };

    struct RenderHealthSnapshot {
        RuntimeRenderStats runtime;
        RenderResourceRegistryStats resources;
        std::vector<ShaderHealthEntry> shaders;
        std::vector<RenderPassMetadata> passes;
        size_t gl_debug_errors = 0;
        bool started = false;
        bool passed = false;
        std::vector<std::string> failures;
    };

    RenderPipeline();
    ~RenderPipeline();

    bool startup(u32 screen_width, u32 screen_height, const std::filesystem::path& root_path);
    void shutdown();
    void render_frame(entt::registry& registry, Systems::SHIELD_WorldSystem& world_system, const Camera& camera, float deltaTime, bool wireframe = false);

    // T-I6-A1d: push the deterministic Aetheric scalar field to the lighting-pass
    // emissive tap (one-way sim->render bridge, called per frame from the client).
    // `cells` is the row-major extent*extent field; (world_origin_x/z) is the
    // grid's world-space origin; cell_size_m maps world XZ -> texel. Lazily
    // creates the R32F texture. Pass an empty `cells` (or never call it) to leave
    // the field inactive -> the lighting pass adds no glow (pixel-identical).
    void update_aether_field(const std::vector<float>& cells, float world_origin_x,
                             float world_origin_z, int extent, float cell_size_m);
    // Reallocates ALL screen-sized render targets (G-buffer, SSAO, lighting/post
    // chain) to the new framebuffer size, preserving formats; the far-LOD path
    // and passes consume the new sizes through the shared state. A no-op when the
    // size is unchanged or degenerate (0). Each real reallocation bumps
    // resize_generation() so callers (the WindowModeStress gate) can assert
    // targets were actually rebuilt (T-I4-DR-window-modes).
    void on_resize(u32 new_width, u32 new_height);
    u32 screen_width() const { return m_screen_width; }
    u32 screen_height() const { return m_screen_height; }
    // Count of render-target reallocations since startup (one per real
    // on_resize). Surfaced as telemetry so the resize-stress gate can verify
    // targets were rebuilt during the mid-run mode toggles.
    u64 resize_generation() const { return m_resize_generation; }
    void clear_all_chunk_data(); // Force clear all cached chunk render data
    const MeshUploadFrameStats& get_last_mesh_upload_stats() const { return m_last_mesh_upload_stats; }
    const RenderPassFrameStats& get_last_render_pass_stats() const { return m_last_render_pass_stats; }
    const std::vector<RenderPassMetadata>& get_last_render_pass_metadata() const { return m_last_render_pass_metadata; }
    RuntimeRenderStats get_runtime_render_stats() const;
    RenderResourceRegistryStats get_resource_registry_stats() const;
    // Texture-array residency layer lookup by name (T-I4-6). Returns the
    // {array, layer} index of a resident .ltex texture, or false if the name
    // is not resident. T-I4-7 wires these layer indices through the material
    // LUT; nothing samples them yet.
    bool find_resident_texture_layer(const std::string& name, size_t& out_array_index, uint32_t& out_layer) const;
    size_t texture_resident_bytes() const { return m_texture_residency.resident_bytes; }
    static constexpr size_t texture_resident_budget_bytes() { return kTextureResidentBudgetBytes; }
    std::vector<ShaderHealthEntry> get_shader_health() const;
    RenderHealthSnapshot get_render_health_snapshot(bool drain_gl_errors = false) const;
    // Generated caustics texture id (0 when unavailable). Exposed for the
    // runtime scenario harness caustics-animation probe (T-I2-16).
    u32 water_caustics_texture() const;
    void set_time_of_day(float normalized_time);
    // T-I5a-7 (C2): SEASON / celestial model. The season phase is a PURE FUNCTION
    // of the authoritative TICK COUNT (integer epoch math; DeterministicMath for
    // the sun-path trig) -- never wall-clock, never a free-running float
    // accumulator (critique F7). It is RENDER-DERIVED: the client pushes the
    // replicated sim tick here each frame; the season modulates the sun ARC
    // (declination / day length) and a biome material/foliage PALETTE tint ON TOP
    // of the existing time-of-day, and adds NOTHING to world_hash. One-way (F2):
    // nothing here writes back into the sim.
    void set_season_tick(std::uint64_t tick);
    std::uint64_t get_season_tick() const { return m_seasonTick; }
    // Season phase in [0,1): 0 == summer solstice, 0.5 == winter solstice. Pure
    // function of the tick count (see kTicksPerSeasonCycle).
    float get_season_phase() const { return m_seasonPhase; }
    // Seasonal solar declination offset applied to the sun arc this frame
    // (positive raises the arc / lengthens the day in summer; negative lowers it
    // in winter). Tick-derived, deterministic.
    float get_season_sun_declination() const { return m_seasonSunDeclination; }
    // Sun elevation above the horizon this frame, radians (>0 == above horizon).
    // The season modulates its peak, so the season sweep reads distinct bands.
    float get_sun_elevation_rad() const { return m_sunElevationRad; }
    // Per-frame season palette tint (multiplied into the sun/ambient warmth):
    // warmer (R>B) toward summer, cooler (B>R) toward winter. Render-only.
    glm::vec3 get_season_palette_tint() const { return m_seasonPaletteTint; }
    // Long-period season cycle length in ticks (a full "year"). The phase wraps
    // on this; integer epoch math keeps it a pure tick function.
    static constexpr std::uint64_t kTicksPerSeasonCycle = 432000ull; // 4 h at 30 Hz
    // Runtime weather control (engine-generic). Intensity is clamped to
    // [0, 1]; WeatherType::None or intensity 0 disables the overlay entirely.
    void set_weather(WeatherType type, float intensity);
    WeatherType get_weather_type() const { return m_weather_type; }
    float get_weather_intensity() const { return m_weather_intensity; }
    // T-I5a-3 (B1): push the SIM-DRIVEN weather render state (one-way, F2). The
    // client samples the replicated WeatherSystem at the camera each frame and
    // calls this; the overlay + wetness response read m_weather_state. Also sets
    // m_weather_type/intensity so the overlay's zero-work gate still fires when
    // there is no precipitation (driven clear == overlay off).
    void set_weather_state(const WeatherRenderState& state);
    const WeatherRenderState& get_weather_state() const { return m_weather_state; }

    // T-I5a-8 (C3): render-only cloud layer control. set_cloud_state enables the
    // wind-advected cloud dome + (optionally) the projected cast shadow and sets
    // the weather-derived coverage/biome/plane parameters. The wind scroll offset
    // is advanced internally each frame from the pushed wind direction * a tick-
    // derived phase (advance_cloud_phase, called inside update_time_of_day) so the
    // clouds drift deterministically with the wind; callers supply the static
    // coverage parameters here. One-way: the cloud field never feeds the sim.
    void set_cloud_state(const CloudRenderState& state);
    const CloudRenderState& get_cloud_state() const { return m_cloud_state; }

    // T-I5a-5 (B3): render-only lightning control. set_lightning_state pushes the
    // full-scene light pulse + bolt polyline for the current frame; the LightingPass
    // injects the pulse and rasterizes the bolt. Pass an inactive state (default) to
    // turn it off (zero added lighting cost). One-way (F2): never fed to the sim.
    void set_lightning_state(const LightningRenderState& state);
    const LightningRenderState& get_lightning_state() const { return m_lightning_state; }
    // Lightning light-pulse GPU cost (ms) from the lighting-pass timer on the LAST
    // frame the pulse was active; 0.0 otherwise. The PerfRegression gate bounds this
    // against the transient ≤ 0.5 ms budget (design §7).
    double lightning_pulse_gpu_ms() const { return m_last_render_pass_stats.lightning_pulse_gpu_ms; }
    // Cloud-shadow GPU cost (ms) from the per-pass timer pair bracketing the
    // lighting pass on the LAST frame the cloud shadow was active; 0.0 otherwise.
    // The CloudShadow gate compares the clouds-on vs clouds-off lighting timing to
    // bound the ADDED per-fragment sample cost against the ≤ 0.4 ms budget (F3).
    double cloud_shadow_gpu_ms() const { return m_last_render_pass_stats.cloud_shadow_gpu_ms; }
    
    // GPU SDF integration
    void set_gpu_sdf_runtime_enabled(bool enabled);
    GpuSdfRuntimeToggleState get_gpu_sdf_runtime_toggle_state() const;
    // T-I6-A3b SHIELD-RT far-field raymarch runtime opt-in (gated additionally by
    // the compile-time kEnableExperimentalFarFieldGpuRaymarching).
    void set_far_field_raymarch_enabled(bool enabled) { m_far_field_runtime_requested = enabled; }
    // T-I6 isolation/layer mode: set from the scenario config (CLI). Default
    // {All, Scene} is a no-op (render byte-stable). The SkyboxPass reads the
    // backdrop to flat-fill the background; the scenario harness reads the layer
    // mask to spawn-suppress non-selected content.
    void set_isolation_config(const Client::ScenarioHarness::IsolationConfig& cfg) { m_isolation_config = cfg; }
    const Client::ScenarioHarness::IsolationConfig& isolation_config() const { return m_isolation_config; }
    void SetupGPUSDFIntegration(Systems::SHIELD_WorldSystem& world_system);

    // --- Far-LOD region rendering (T-I3-9) ---
    // Far tile builds run on the attached JobSystem's Normal lane; without an
    // attached job system the far-LOD path stays inert.
    void attach_farlod_job_system(JobSystem* job_system);
    // Drains in-flight far tile builds (they sample the world system). MUST
    // run before the currently bound world is destroyed/recreated.
    void prepare_world_swap();
    // Drains only the SHIELD-RT far-field heightfield build (its worker reads the
    // world by pointer). MUST run before clear_world at teardown.
    void drain_far_field_builds();
    FarLodSystem* farlod() { return m_farlod.get(); }
    const FarLodSystem* farlod() const { return m_farlod.get(); }

    // --- Particle framework (T-I5a-1). ---
    // The particle pass owns the fixed-capacity persistent-mapped instance pool,
    // the emitter set, and the sim-deterministic emitter-descriptor snapshot
    // surface. Exposed so the scenario harness can load fixture emitters and
    // snapshot/assert the descriptor set for the ParticleEmitterDeterminism gate.
    ParticlePass* particles() { return m_particle_pass.get(); }
    const ParticlePass* particles() const { return m_particle_pass.get(); }

    // --- Foliage scatter (T-I5b-1, F1). ---
    // The foliage pass owns the fixed-capacity persistent-mapped scatter
    // instance pool + the deterministic placement hash. Exposed so the scenario
    // harness can load the scatter set, push per-chunk placement inputs + the A2
    // wind bridge, and snapshot the instance set for the FoliageInstancing gate.
    FoliagePass* foliage() { return m_foliage_pass.get(); }
    const FoliagePass* foliage() const { return m_foliage_pass.get(); }

    // --- Waterfalls (T-I5b-4, W1). ---
    // World-deterministic site detection (river course x steep height drop),
    // computed once per world and CACHED here (camera/frame independent —
    // critique F5). The dressing (sheet shader, A1 spray, plunge foam, roar) is
    // render-only and never hashed; the SITES are a pure function of the
    // generated world (same seed -> same sites for every replay). Returns the
    // cached site set for `world`, detecting on first use.
    const std::vector<WaterfallSite>& waterfall_sites(
        const Systems::SHIELD_WorldSystem& world,
        const WaterfallDetectParams& params = {}) {
        return m_waterfall_sites.sites_for(world, params);
    }
    WaterfallSiteCache& waterfall_cache() { return m_waterfall_sites; }

private:
    // Extracted render pass classes (T-I2-11). Passes own their GL resources
    // (FBOs/textures/shaders); the pipeline keeps orchestration order, shared
    // state, stats collection, and GPU timer issue/collect calls.
    friend class ShadowPass;
    friend class GBufferPass;
    friend class SsaoPass;
    friend class LightingPass;
    friend class WaterPass;
    friend class SkyboxPass;
    friend class ParticlePass;
    friend class FoliagePass;

    struct ChunkMeshSnapshot {
        ChunkID id = 0;
        IVec3 coords{};
        const Chunk* source_chunk = nullptr;
        u32 mesh_version = 0;
        u32 water_mesh_version = 0;
        size_t terrain_vertex_count = 0;
        size_t terrain_index_count = 0;
        size_t water_vertex_count = 0;
        size_t water_index_count = 0;

        bool has_terrain_mesh() const { return terrain_vertex_count > 0 && terrain_index_count > 0; }
        bool has_water_mesh() const { return water_vertex_count > 0 && water_index_count > 0; }
    };

    struct ChunkMeshPayload {
        u32 mesh_version = 0;
        std::vector<VoxelVertex> vertices;
        std::vector<u32> indices;

        bool has_mesh() const { return !vertices.empty() && !indices.empty(); }
    };

    std::vector<ChunkMeshSnapshot> build_chunk_snapshots(const std::vector<Chunk*>& renderable_chunks) const;

    void ensure_terrain_culling_hierarchy(const std::vector<ChunkMeshSnapshot>& renderable_chunks);
    void manage_chunk_gpu_resources(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera);
    bool copy_terrain_mesh_payload(const ChunkMeshSnapshot& chunk, ChunkMeshPayload& payload) const;
    void upload_chunk_mesh(const ChunkMeshSnapshot& chunk, const ChunkMeshPayload& payload);
    void unload_chunk_resources(ChunkID chunk_id);

    void manage_water_gpu_resources(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera);
    bool copy_water_mesh_payload(const ChunkMeshSnapshot& chunk, ChunkMeshPayload& payload) const;
    void upload_water_mesh(const ChunkMeshSnapshot& chunk, const ChunkMeshPayload& payload);
    void unload_water_resources(ChunkID chunk_id);

    void init_shaders();
    void init_screen_quad();
    void cleanup_gpu_resources();

    // --- Per-pass GPU timers (GL_TIMESTAMP query pairs) ---
    enum class GpuTimerPass : size_t {
        Shadow = 0,
        GBuffer,
        Ssao,
        SsaoBlur,
        Lighting,
        Water,
        Skybox,
        Particle, // T-I5a-1
        Foliage,  // T-I5b-1: instanced foliage scatter pass
        Aerial,    // T-I5a-6: analytic aerial-perspective fullscreen pass
        FarFieldRaymarch, // T-I6-A3b: experimental SHIELD-RT far-field raymarch
        FinalBlit,
        Count,
    };
    static constexpr size_t kGpuTimerPassCount = static_cast<size_t>(GpuTimerPass::Count);
    // Ring of 3 frame slots so frame N polls the queries issued at frame N-2
    // without ever stalling on GL_QUERY_RESULT_AVAILABLE.
    static constexpr size_t kGpuTimerFrameRing = 3;

    struct GpuTimerFrameSlot {
        std::array<GLuint, kGpuTimerPassCount> begin_queries{};
        std::array<GLuint, kGpuTimerPassCount> end_queries{};
        std::array<bool, kGpuTimerPassCount> issued{};
    };

    struct GpuPassTimers {
        bool supported = false;
        bool labeled = false;
        bool first_sample_logged = false;
        u64 frame_index = 0;
        std::array<GpuTimerFrameSlot, kGpuTimerFrameRing> slots{};
        std::array<double, kGpuTimerPassCount> last_gpu_ms{};
    };
    GpuPassTimers m_gpu_timers;

    void init_gpu_pass_timers();
    void destroy_gpu_pass_timers();
    void begin_gpu_pass_timer(GpuTimerPass pass);
    void end_gpu_pass_timer(GpuTimerPass pass);
    void collect_gpu_pass_timers();
    void finish_gpu_pass_timer_frame();

    void refresh_render_pass_metadata();

    std::vector<glm::mat4> get_light_space_matrices(const Camera& camera);

    void update_time_of_day(float deltaTime);
    float m_timeOfDay = 0.5f; // Start at sunrise
    float m_dayDurationSeconds = 60.0f;

    u32 m_screen_width = 0;
    u32 m_screen_height = 0;
    // Render-target reallocation counter (T-I4-DR-window-modes). Bumped once per
    // real on_resize so the resize-stress gate can assert targets were rebuilt.
    u64 m_resize_generation = 0;
    std::filesystem::path m_root_path;

    DirectionalLight m_sun;
    glm::vec3 m_moonDirection;
    glm::vec3 m_skyAmbientColor;
    // T-I4-DR-tod-sky-balance: continuous day->twilight->night factor derived
    // from the sun's elevation, smoothly 1 (sun high) -> 0 (sun below horizon).
    // The sky dome reads this so its brightness/tint tracks time-of-day with
    // the SAME elevation signal that drives sun.color/ambient, instead of the
    // clamped m_sun.intensity (which saturates to 1 while the sun is still low,
    // leaving the dusk dome stuck at full midday and the night dome bright).
    float m_skyDayFactor = 1.0f;
    // T-I5a-7 (C2): SEASON state, all DERIVED from m_seasonTick (a pure function
    // of the authoritative sim tick -- no wall-clock, no float accumulator). The
    // phase/declination/tint are recomputed inside update_time_of_day from the
    // integer tick, so they are reproducible from tick alone and never hashed.
    std::uint64_t m_seasonTick = 0;
    float m_seasonPhase = 0.0f;          // [0,1): 0 summer, 0.5 winter
    float m_seasonSunDeclination = 0.0f; // radians, seasonal arc tilt
    float m_sunElevationRad = 0.0f;      // sun elevation this frame (radians)
    glm::vec3 m_seasonPaletteTint{1.0f}; // warm(summer)/cool(winter) palette tint
    WeatherType m_weather_type = WeatherType::None;
    float m_weather_intensity = 0.0f;
    // T-I5a-3 (B1): SIM-DRIVEN weather render state (one-way from WeatherSystem).
    WeatherRenderState m_weather_state;
    // T-I5a-8 (C3): render-only cloud layer state + the wind-advection scroll
    // phase. m_cloud_phase accumulates the tick-derived deltaTime so the scroll
    // offset (= wind_dir * wind_strength * phase) advances with the wind; it is a
    // pure render accumulator (never hashed, one-way). advance_cloud_phase runs
    // inside update_time_of_day on the same deltaTime that drives the sun.
    CloudRenderState m_cloud_state;
    float m_cloud_phase = 0.0f;
    void advance_cloud_phase(float deltaTime);
    // T-I5a-5 (B3): render-only lightning pulse + bolt state for the current frame.
    LightningRenderState m_lightning_state;
    std::unique_ptr<FarLodSystem> m_farlod;
    std::unique_ptr<GBufferPass> m_gbuffer_pass;
    std::unique_ptr<ShadowPass> m_shadow_pass;
    std::unique_ptr<SsaoPass> m_ssao_pass;
    std::unique_ptr<LightingPass> m_lighting_pass;
    std::unique_ptr<WaterPass> m_water_pass;
    std::unique_ptr<SkyboxPass> m_skybox_pass;
    std::unique_ptr<ParticlePass> m_particle_pass; // T-I5a-1
    std::unique_ptr<FoliagePass> m_foliage_pass;   // T-I5b-1
    std::unique_ptr<ShieldRtFarFieldPass> m_shieldrt_far_pass; // T-I6-A3b (flag-gated)
    JobSystem* m_job_system = nullptr;             // attached pre-startup; forwarded to passes built in startup()
    bool m_far_field_runtime_requested = false;    // --enable-far-field-gpu-raymarch
    Client::ScenarioHarness::IsolationConfig m_isolation_config; // T-I6 (default {All,Scene} = no-op)
    WaterfallSiteCache m_waterfall_sites;          // T-I5b-4 (render-only, cached)

    // T-I5a-6: Hillaire 2020 atmospheric scattering. The LUTs are built once at
    // startup and the sky-view LUT refreshed when the sun moves; the skybox pass
    // samples the sky-view LUT, the lighting pass + aerial pass read the SAME
    // transmittance/multi-scatter pair (coherent sun/sky/ambient/fog palette).
    // m_aerial_shader is the analytic aerial-perspective fullscreen term wiring
    // the previously dormant volumetric_lighting.frag. Render-only (design §2).
    SkyAtmosphereLut m_sky_lut;
    std::unique_ptr<Shader> m_aerial_shader;
    // T-I5b-4 (W1): the animated falling-sheet shader (waterfall.frag) the live
    // pipeline draws over detected waterfall sites. Render-only dressing.
    std::unique_ptr<Shader> m_waterfall_shader;
    double m_sky_full_precompute_ms = 0.0;
    double m_sky_view_refresh_ms = 0.0; // last sky-view refresh cost (0 = none this frame)
    // Sky-derived scattering ambient (the sky-view hemisphere integral); folded
    // into m_skyAmbientColor so lighting/ambient share the LUT transmittance.
    glm::vec3 m_skyScatterAmbient{0.0f};
    void init_sky_lut();
    void execute_aerial_pass(const Camera& camera);
public:
    const SkyAtmosphereLut& sky_lut() const { return m_sky_lut; }
private:

    std::unordered_map<ChunkID, ChunkRenderData> m_chunk_render_data;
    std::unordered_map<ChunkID, WaterRenderData> m_water_render_data;
    std::vector<ChunkRenderData> m_free_chunk_render_slots;
    std::vector<WaterRenderData> m_free_water_render_slots;

    // T-I4-16: shared bucketed persistent-mapped pool backing all live terrain
    // chunk geometry, plus the per-frame MDI scratch buffers (an indirect
    // command buffer and a chunk-origin SSBO, double/triple-buffered to avoid
    // stalling on the GPU still reading last frame's commands). The G-buffer and
    // shadow passes both submit live terrain through draw_chunks_mdi().
    ChunkGeometryPool m_chunk_geometry_pool;

    struct DrawElementsIndirectCommand {
        GLuint count;          // index count
        GLuint instanceCount;  // 1
        GLuint firstIndex;     // index_offset
        GLuint baseVertex;     // vertex_offset
        GLuint baseInstance;   // gl_DrawID fallback / origin index
    };

    static constexpr std::size_t kMdiRingFrames = 3;
    struct MdiFrameBuffers {
        GLuint indirect_buffer = 0; // GL_DRAW_INDIRECT_BUFFER
        // Per-draw chunk origin (vec4), consumed as an instanced vertex
        // attribute (binding 1, divisor 1) indexed by each command's
        // baseInstance. A plain array buffer, not an SSBO -- this makes
        // per-draw origin selection work without GLSL 4.6 gl_BaseInstance.
        GLuint origin_buffer = 0;
        std::size_t command_capacity = 0; // commands the buffers can hold
    };
    std::array<MdiFrameBuffers, kMdiRingFrames> m_mdi_frames;
    std::size_t m_mdi_frame_cursor = 0;
    // Reusable CPU scratch so the per-frame submit allocates nothing steady-state.
    std::vector<DrawElementsIndirectCommand> m_mdi_command_scratch;
    std::vector<glm::vec4> m_mdi_origin_scratch;

    void init_mdi_buffers();
    void destroy_mdi_buffers();
    void ensure_mdi_capacity(MdiFrameBuffers& frame, std::size_t commands);
    // draw_chunks_mdi is declared after ChunkCullEntry (it takes a vector of
    // those), further down in this class.

    MeshUploadFrameStats m_last_mesh_upload_stats;
    RenderPassFrameStats m_last_render_pass_stats;
    std::vector<RenderPassMetadata> m_last_render_pass_metadata;

    u32 m_screen_quad_vao = 0;
    u32 m_screen_quad_vbo = 0;

    u32 m_terrainTextureArray = 0;
    // Per-material triplanar normal-map array (T-I4-7). Same layer order as the
    // albedo array; layer indices come from the material LUT normal_layer
    // column. RGBA8 tangent-space (OpenGL convention) normal maps.
    u32 m_terrainNormalArray = 0;
    u32 m_materialLUT = 0;
    // T-I6-A1d: Aetheric scalar field as an R32F texture for the lighting-pass
    // emissive tap. Updated per frame from the sim field (one-way bridge).
    // m_aetherFieldActive gates the glow so a no-aether world stays pixel-identical
    // (RenderHealth-neutral until a world enables the field).
    u32 m_aetherFieldTexture = 0;
    glm::vec2 m_aetherFieldWorldOrigin{0.0f};
    float m_aetherFieldCellSize = 24.0f;
    int m_aetherFieldExtent = 0;
    bool m_aetherFieldActive = false;
    size_t m_terrain_texture_fallback_layers = 0;
    // Resolution the terrain albedo/normal arrays are allocated at. T-I6: raised
    // 256 -> 1024. The repo already ships full AmbientCG 2K CC0 PBR sets per material
    // (Rock028/Ground048/Grass003/Ground087/Gravel040) but the runtime had been
    // loading 256px .ltex plates that threw away the near-ground micro-detail the
    // BF4/BF1 fidelity floor wants. The 1024 .ltex are regenerated from the 2K Color/
    // NormalGL PNGs (16x the texels). VRAM: 1024^2 x 5 layers x 4 B x 2 arrays = 42 MB
    // (trivial on the 16 GB RTX 5070 Ti target).
    static constexpr int kTerrainTextureResolution = 1024;

    // Emissive intensity LUT scale (T-I4-9). The RGBA8 material LUT stores
    // emissive_intensity normalized by this ceiling; the lighting pass rescales.
    // Authored intensities run 0..~4 this iteration; 8 leaves headroom.
    static constexpr float kEmissiveLutScale = 8.0f;

    // --- Skinned/creature UV-mapped textures (T-I4-8) ---
    // GL_TEXTURE_2D_ARRAY of UV-sampled creature textures; layer 0 = albedo,
    // layer 1 = tangent-space normal. Sampled by the skinned-mesh G-buffer path
    // (skinned_mesh.vert + g_buffer.frag u_skinnedTextures). Separate from the
    // terrain triplanar arrays (different sampling model). The engine names no
    // creature here: the texture *set* is supplied by the caller (the scenario
    // harness reads paths from the game archetype JSON — T-I4-DR-split-lint).
    u32 m_skinnedTextureArray = 0;
    static constexpr int kSkinnedTextureResolution = 256;
    // Layer indices within m_skinnedTextureArray (-1 = absent).
    int m_skinnedAlbedoLayer = -1;
    int m_skinnedNormalLayer = -1;
    // Allocates the skinned texture array with flat fallback layers (mid-grey
    // albedo / up-normal) so the skinned mesh is always drawable even before a
    // texture set is loaded.
    void init_skinned_texture_array();
    // Accessor for GBufferPass (friend) skinned-pass binding.
public:
    u32 skinned_texture_array() const { return m_skinnedTextureArray; }
    int skinned_albedo_layer() const { return m_skinnedAlbedoLayer; }
    int skinned_normal_layer() const { return m_skinnedNormalLayer; }
    // Generic, data-driven texture-set loader. Uploads the albedo (.ltex) into
    // layer 0 and the tangent-space normal (.ltex) into layer 1 of the skinned
    // array, returning the layer indices through albedo_layer_out/normal_layer_out.
    // A missing/mismatched file keeps that layer's flat fallback. Returns true if
    // the albedo loaded (the mesh is then UV-textured). Paths are caller-supplied,
    // so the engine carries no creature/asset names.
    bool load_skinned_texture_set(const std::filesystem::path& albedo_path,
                                  const std::filesystem::path& normal_path,
                                  int& albedo_layer_out, int& normal_layer_out);
private:

    // Per-material LUT columns parsed from data/common/materials.json
    // (texture_layer / normal_layer / tiling — design §3, owned by T-I4-7).
    // Indexed by material id; defaults mean "untextured / flat" so unknown ids
    // and the crystal/water render kinds keep the G-buffer base color.
    struct MaterialTextureLut {
        std::array<int, 256> texture_layer;        // -1 = untextured
        std::array<int, 256> normal_layer;         // -1 = flat
        std::array<float, 256> tiling;             // world-units per repeat (>0)
        std::array<float, 256> emissive_intensity; // 0 = non-emissive (T-I4-9)
        std::array<float, 256> roughness;          // 0..1, default 0.85 (T-I4-10)
        std::array<bool, 256> roughness_set;       // material declared roughness
        // T-I5b-5-water-backlog: per-material albedo multiplier applied to the
        // baked (textured) G-buffer albedo. Default 1.0 (unscaled). Calibrates a
        // physically-bright photographic texture down to a natural lit tone when
        // the irradiance chain would otherwise clip it past the ACES knee (the
        // sun-bright near-sea-level sand-flat). Render-only.
        std::array<float, 256> albedo_scale;       // >0, default 1.0
        int terrain_layer_count = 0;               // distinct albedo layers loaded
        MaterialTextureLut() {
            texture_layer.fill(-1);
            normal_layer.fill(-1);
            tiling.fill(4.0f);
            emissive_intensity.fill(0.0f);
            roughness.fill(0.85f);
            roughness_set.fill(false);
            albedo_scale.fill(1.0f);
        }
    };
    MaterialTextureLut m_material_texture_lut;
    // Parses materials.json texture_layer/normal_layer/tiling columns into
    // m_material_texture_lut. Missing file/columns leave defaults (untextured).
    void load_material_texture_lut();

    void init_terrain_textures();
    void init_material_lut();

    // --- Texture-array residency manager (T-I4-6) ---
    // Imports .ltex assets into GL_TEXTURE_2D_ARRAY objects bucketed by size
    // class {width, height, channels}. Each distinct size class gets its own
    // array; textures of that class become layers. Layer lookup by name feeds
    // the material LUT layer indices (consumed by T-I4-7). Texture arrays were
    // chosen over bindless deliberately (design-decisions §10; research Area 1
    // — bindless is AMD-fragile/Intel-absent on GL).
    //
    // 96 MB resident-texture budget this iteration (design-decisions §10).
    static constexpr size_t kTextureResidentBudgetBytes = 96u * 1024u * 1024u;

    struct LtexCpuImage {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 0;
        uint16_t mip_count = 0;
        std::vector<unsigned char> bytes; // full mip chain, level 0 first
    };

    struct TextureResidencyArray {
        u32 texture_id = 0;        // GL_TEXTURE_2D_ARRAY
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 0;
        uint16_t mip_count = 0;
        uint32_t layer_count = 0;  // populated layers
        uint32_t layer_capacity = 0;
        size_t resident_bytes = 0; // sum of uploaded mip bytes
        std::string size_class_key; // e.g. "16x16x4"
    };

    struct TextureResidencyLayer {
        size_t array_index = 0;
        uint32_t layer = 0;
    };

    struct TextureResidencyManager {
        std::vector<TextureResidencyArray> arrays;
        std::unordered_map<std::string, TextureResidencyLayer> layer_by_name;
        size_t resident_bytes = 0;
    };
    TextureResidencyManager m_texture_residency;

    // Loads a .ltex file from disk into a CPU image (full mip chain). Returns
    // false on any header/size error.
    bool load_ltex_cpu_image(const std::filesystem::path& path, LtexCpuImage& out) const;
    // Imports a .ltex asset, allocating/growing a size-class array as needed and
    // uploading its mip chain into a fresh layer. Records layer-by-name lookup
    // and resident-byte accounting. Respects the resident budget (an over-budget
    // upload is rejected and logged). Returns the layer name's resident state.
    bool upload_ltex_to_residency(const std::string& name, const std::filesystem::path& path);
    // Loads the committed iteration-4 .ltex test assets into residency arrays.
    void init_texture_residency();
    void destroy_texture_residency();

    // --- GPU SDF Generation ---
    struct GPUSDFSystem {
        GLuint compute_program = 0;
        GLuint sdf_buffer = 0;
        GLuint terrain_noise_texture = 0;
        GLuint cave_noise_texture = 0;
        GLuint island_mask_texture = 0;
        bool initialized = false;
        bool runtime_requested = false;
        bool callback_registered = false;
        
        // Async compute fence for non-blocking operation
        GLsync compute_fence = nullptr;
    };
    GPUSDFSystem m_gpu_sdf;
    
    void init_gpu_sdf_system();
    void generate_noise_textures();
    bool generate_chunk_sdf_gpu(const glm::ivec3& chunk_coords, const ::Luminumbra::Systems::TerrainGenParams& params, int seed, std::vector<float>& out_sdf);
    void cleanup_gpu_sdf_system();

    std::vector<PointLight> m_point_lights_this_frame;
    const int MAX_POINT_LIGHTS = 32;

    void gather_lights(entt::registry& registry);

    bool m_started = false;
    
    // Hierarchical frustum culling system
    struct AABB {
        glm::vec3 min;
        glm::vec3 max;
        
        AABB() = default;
        AABB(const glm::vec3& min, const glm::vec3& max) : min(min), max(max) {}
    };

    struct ChunkCullEntry {
        ChunkID id = 0;
        IVec3 coords{};
        AABB bounds;
    };
    
    struct CullingNode {
        AABB bounds;
        std::vector<ChunkCullEntry> chunks;
        std::unique_ptr<CullingNode> children[4]; // Quadtree (X-Z plane)
        bool is_leaf = true;
        
        CullingNode() = default;
        CullingNode(const AABB& bounds) : bounds(bounds) {}
    };
    
    class HierarchicalCuller {
    public:
        void BuildHierarchy(const std::vector<ChunkMeshSnapshot>& chunks);
        void CullRecursive(const glm::vec4 frustum_planes[6], CullingNode* node, std::vector<const ChunkCullEntry*>& visible);
        void CullHierarchical(const glm::vec4 frustum_planes[6], std::vector<const ChunkCullEntry*>& visible);
        void Clear();
        
        std::unique_ptr<CullingNode> m_root; // Made public for access
        
    private:
        static constexpr int MAX_CHUNKS_PER_NODE = 8;
        static constexpr int MAX_DEPTH = 4;
        
        void BuildRecursive(CullingNode* node, const std::vector<ChunkCullEntry>& chunks, int depth);
        bool AABBFrustumCulled(const AABB& aabb, const glm::vec4 frustum_planes[6]);
    };
    
    HierarchicalCuller m_hierarchicalCuller;

    // T-I4-16: builds and issues glMultiDrawElementsIndirect for the supplied
    // visible live chunks (one command per pool-resident chunk, grouped by pool
    // block -> one MDI call per block). The chunk world origin reaches the
    // vertex shader through the instanced aOrigin attribute (binding 1) indexed
    // by each command's baseInstance; the caller's shader must declare that
    // attribute and set u_useInstanceOrigin = 1. Returns draw + index totals.
    // Declared here (not with the other MDI helpers above) because it takes a
    // vector of ChunkCullEntry, which is defined just above.
    void draw_chunks_mdi(const std::vector<const ChunkCullEntry*>& visible_chunks,
                         std::size_t& out_draws, std::size_t& out_indices);

    struct TerrainCullingCache {
        u64 chunk_set_signature = 0;
        size_t chunk_count = 0;
        bool valid = false;
    } m_terrainCullingCache;

    // Frustum culling cache
    struct FrustumCache {
        glm::vec4 planes[6];
        glm::vec3 lastCameraPos;
        glm::vec3 lastCameraFront;
        float lastZoom = 0.0f;
        bool valid = false;
    } m_frustumCache;
};

} // namespace Luminumbra::Rendering
