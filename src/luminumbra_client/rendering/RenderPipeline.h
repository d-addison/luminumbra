#pragma once

#include "../../include/luminumbra/core/Types.h"
#include "FrameBufferObject.h" // FrameBufferObject (extracted)
#include "GBuffer.h"           // GBuffer (extracted)
#include "Mesh.h"
#include "RenderContext.h"          // pass contract (make_ssao_context returns by value)
#include "RenderFrameTypes.h"       // light/weather/cloud PODs (extracted)
#include "RenderInputs.h"           //  GlassPaneItem (the glass draw list member)
#include "RenderResourceRegistry.h" // render resource registry (value member)
#include "ShadowMap.h"              // ShadowMap (extracted)
#include "SkyAtmosphereLut.h"       //  Hillaire 2020 scattering LUTs
#include "StaticModelTex.h"         // StaticModelTex hoisted to namespace scope
#include "TerrainSubmit.h"          // SubmitTerrainChunksFn + TerrainSubmitStats
#include "core/AssetManager.h"
#include "core/IsolationConfig.h" // isolation/layer render mode (backdrop + spawn-suppression)
#include "luminumbra_common/components/LightingComponents.h"
#include "luminumbra_common/world/Chunk.h"
#include "passes/ChunkGeometryPool.h"
#include "passes/SsaoData.h" // SSAOData (extracted; still read here for stats)
#include "passes/WaterfallPass.h"
#include <array>
#include <filesystem>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Forward declarations
namespace Luminumbra {
class Chunk;
class JobSystem;
} // namespace Luminumbra
namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
struct TerrainGenParams;
} // namespace Luminumbra::Systems
namespace Luminumbra::Rendering {
class Shader;
class Camera;
class ShadowPass;
class GBufferPass;
class SsaoPass;
class LightingPass;
class WaterPass;
class SkyboxPass;
class ParticlePass;
class FoliagePass;
class PlantProcgenPass;
class FarLodSystem;
class GroundDecalPass;
class DebugViewPass;
class FinalBlitPass;
class AerialPass;
class GodRaysPass;
class TaauPass;
class GlassOitPass;
class FroxelPass;
class LuminanceMeterPass;
struct ScentFieldRenderMirror;
} // namespace Luminumbra::Rendering

namespace Luminumbra::Rendering {

// DirectionalLight + PointLight moved to RenderFrameTypes.h (included above).
//  Definitions unchanged.

// GBuffer + ShadowMap moved to GBuffer.h / ShadowMap.h
// (included above) so passes + PassGlHelpers reference them without this
// god-object. Definitions unchanged.

// a live chunk's terrain geometry now lives inside the shared
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
    //  pool slice. Packs {block_index, vertex/index slot offsets}.
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

// FrameBufferObject moved to FrameBufferObject.h (included above). Definition unchanged.

// SSAOData moved to passes/SsaoData.h (included above) so
// SsaoPass owns it without including this god-object. Definition is unchanged;
// RenderPipeline still reads ssao resources for inventory/VRAM stats.

// enum class WeatherType moved to RenderFrameTypes.h (included above). Definition
//  unchanged.

// WeatherRenderState moved to RenderFrameTypes.h (included above). Definition unchanged.

// CloudRenderState moved to RenderFrameTypes.h (included above). Definition unchanged.

// LightningRenderState (+ kMaxBoltSegmentPoints) moved to RenderFrameTypes.h (included
//  above). Definition unchanged.

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
        // chunks. Recorded inside the gbuffer GPU timer window.
        size_t far_region_draws = 0;
        size_t far_indices_drawn = 0;
        // Skinned (animated) meshes drawn in the G-buffer pass.
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
        //  instanced particle draws (one per ParticlePass submit) and
        // the total particle instances drawn this frame. Both stay 0 when no
        // emitters are active (the pass is a no-op), keeping existing visual
        // gates byte-stable.
        size_t particle_draws = 0;
        size_t particles_drawn = 0;
        // instanced foliage scatter draws (one per FoliagePass
        // submit) and the total scatter instances drawn this frame. Both stay 0
        // when foliage is disabled (the pass is a no-op), keeping existing
        // visual gates byte-stable.
        size_t foliage_draws = 0;
        size_t foliage_instances_drawn = 0;
        size_t final_blits = 0;
        // Per-pass GPU timings sampled from a GL_TIMESTAMP query ring
        // (frame N publishes the timings recorded at frame ). Values stay
        // 0.0 when timers are unsupported or no sample has resolved yet.
        bool gpu_timers_supported = false;
        double shadow_gpu_ms = 0.0;
        double gbuffer_gpu_ms = 0.0;
        double ssao_gpu_ms = 0.0;
        double ssao_blur_gpu_ms = 0.0;
        double lighting_gpu_ms = 0.0;
        double water_gpu_ms = 0.0;
        double skybox_gpu_ms = 0.0;
        double particle_gpu_ms = 0.0; //  ParticlePass GPU timer (≤ 0.8 ms budget)
        // FoliagePass GPU timer. The FoliageInstancing gate bounds
        // this against the pinned release budget (documented design). 0.0 when foliage is
        // disabled / no instances.
        double foliage_gpu_ms = 0.0;
        //  analytic aerial-perspective term (a fullscreen pass wiring
        // volumetric_lighting.frag). Budget ≤ 0.3 ms (documented design).
        double aerial_gpu_ms = 0.0;
        //  the lighting-pass GPU time on the last frame the cloud cast
        // shadow was ACTIVE (the projected coverage sample is per-fragment inside
        // the lighting pass — no separate pass to time). cloud_shadow_added_ms is
        // the incremental cost vs the clouds-off lighting baseline, the number the
        // CloudShadow gate bounds against the ≤ 0.4 ms budget (documented design, ).
        double cloud_shadow_gpu_ms = 0.0; // lighting pass ms with clouds on
        // the lighting-pass GPU time on the last frame the lightning
        // light-pulse was ACTIVE (the full-scene flash + bolt rasterization are both
        // per-fragment inside the lighting pass). The PerfRegression gate bounds this
        // against the transient ≤ 0.5 ms budget (documented design).
        double lightning_pulse_gpu_ms = 0.0;
        double final_blit_gpu_ms = 0.0;
        //  sky scattering LUT precompute timings (CPU build + GL upload).
        // sky_full_precompute_ms is the startup one-shot (budget ≤ 8.0 ms on
        // release); sky_view_refresh_ms is the last per-frame sky-view recompute
        // when the sun moved past the refresh threshold (budget ≤ 0.2 ms),
        // staying 0.0 on frames with no refresh.
        double sky_full_precompute_ms = 0.0;
        double sky_view_refresh_ms = 0.0;
        //  CPU-side per-phase submit cost (std::chrono, this
        // frame, NOT 2-frame-delayed like the GPU timers). The frame is
        // CPU-submit-bound, so this localizes WHERE the CPU time goes — the GPU
        // pass-timer sum cannot. cpu_gbuffer includes chunks+props+farlod+foliage
        // geometry; cpu_static_prop is the static-mesh sub-cost within it.
        double cpu_prepare_ms = 0.0;     // snapshots + GPU resource mgmt + culling + farlod
        double cpu_shadow_ms = 0.0;      // shadow pass CPU submit
        double cpu_gbuffer_ms = 0.0;     // g-buffer pass CPU submit (chunks + props + geo)
        double cpu_static_prop_ms = 0.0; // static-mesh submit within the g-buffer pass
        double cpu_post_ms = 0.0;        // ssao + lighting + skybox + water + particles + final
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
        bool terrain_texture_array_ok = false;
        bool material_lut_ok = false;
        size_t terrain_texture_fallback_layers = 0;
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
    void render_frame(entt::registry& registry,
                      Systems::SHIELD_WorldSystem& world_system,
                      const Camera& camera,
                      float deltaTime,
                      bool wireframe = false);

    // push the deterministic Aether scalar field to the lighting-pass
    // emissive tap (one-way sim->render bridge, called per frame from the client).
    // `cells` is the row-major extent*extent field; (world_origin_x/z) is the
    // grid's world-space origin; cell_size_m maps world XZ -> texel. Lazily
    // creates the R32F texture. Pass an empty `cells` (or never call it) to leave
    // the field inactive -> the lighting pass adds no glow (pixel-identical).
    void update_aether_field(const std::vector<float>& cells,
                             float world_origin_x,
                             float world_origin_z,
                             int extent,
                             float cell_size_m);
    //  ( -8): the RG32F dual tap — R = energy, G =
    // Lumin/Umbra polarity [-1, 1]. Polarity tints the glow ONLY while a dual
    // upload is live; single-channel uploads (and no uploads) keep the glow
    // color untouched (pixel-identical).
    void update_aether_field_dual(const std::vector<float>& energy_cells,
                                  const std::vector<float>& polarity_cells,
                                  float world_origin_x,
                                  float world_origin_z,
                                  int extent,
                                  float cell_size_m);
    // grade the aether glow. Defaults equal the GLSL
    // initializers (pixel-identical untouched); render-only, never world_hash.
    void set_aether_glow(const glm::vec3& color, float intensity) {
        m_aetherGlowColor = color;
        m_aetherGlowIntensity = intensity;
    }
    //  ( -6): emissive-material modulation by the local
    // aether field — emissive output scales by (1 + aether * modulation).
    // Default 0.0 == multiply by exactly 1.0 == pixel-identical even with an
    // active field tap; render-only, never world_hash.
    void set_aether_material_modulation(float modulation) {
        m_aetherMaterialModulation = std::max(0.0f, modulation);
    }
    // render-only snow ground cover [0,1] (0 = untouched).
    void set_snow_cover(float cover01) {
        m_snowCover = std::clamp(cover01, 0.0f, 1.0f);
    }
    // Reallocates ALL screen-sized render targets (G-buffer, SSAO, lighting/post
    // chain) to the new framebuffer size, preserving formats; the far-LOD path
    // and passes consume the new sizes through the shared state. A no-op when the
    // size is unchanged or degenerate (0). Each real reallocation bumps
    // resize_generation so callers (the WindowModeStress gate) can assert
    // targets were actually rebuilt.
    void on_resize(u32 new_width, u32 new_height);

    // set the internal render scale (SystemConfig user.render_scale).
    // Clamped to [0.5, 1.0]; 1.0 is byte-identical (internal==output). Call BEFORE startup
    // to seed the default -- the LUMIN_RENDER_SCALE env knob still WINS (startup applies it
    // after this, for the A/B capture path). Also safe at runtime (a settings change): when
    // already started it reallocates the scaled intermediates like on_resize (the output-res
    // TAAU/backbuffer targets are untouched -- only the internal extent moved).
    void set_render_scale(float scale);

    //   (ADDITIVE offscreen render-target redirect for the
    // create-world live preview diorama). When an offscreen target is set, the
    // FINAL BLIT of render_frame writes its lit color into the given FBO (color
    // attachment 0, sized fbo_w x fbo_h) INSTEAD of the default framebuffer 0.
    // Every internal pass (G-buffer/SSAO/lighting) is unchanged; callers that
    // want a small, budget-holding preview also call on_resize(fbo_w, fbo_h) for
    // the preview's lifetime so the internal passes match the preview dims. The
    // blit is a filtered copy from the lighting FBO (m_screen_*) to the preview
    // FBO. clear_offscreen_target restores the default-0 path (byte-identical
    // to the legacy behaviour). Render-only; nothing is hashed.
    void set_offscreen_target(u32 fbo, u32 fbo_w, u32 fbo_h);
    void clear_offscreen_target();
    bool has_offscreen_target() const {
        return m_offscreen_target_active;
    }
    // Far-LOD on/off. The worldgen PREVIEW disables it around its render_frame: the preview
    // swaps + frees its candidate world between frames (WorldgenPreview::swap_pending_into_
    // live) while far-LOD tile-build jobs run on worker threads holding a reference to it —
    // a destroyed-world use-after-free (the create-screen panning crash). The bounded preview
    // diorama is covered by its live chunks; skipping far-LOD means no such jobs are ever
    // dispatched for the transient preview world. Restored after the preview's frame.
    void set_far_lod_enabled(bool enabled) {
        m_far_lod_enabled = enabled;
    }

    // Worldgen-preview far-field: pin far-LOD streaming + the inner fragment
    // discard to the FIXED diorama centre (instead of the orbiting preview
    // camera) so the preview gets a STABLE distant vista with no orbit churn.
    // inner_radius_m is the live-slice radius the far mesh is hidden behind.
    // Forwarded to the FarLodSystem (no-op if far-LOD isn't constructed). The
    // preview sets this each frame it renders and clears it after, so normal game
    // frames never carry a stale anchor.
    void set_far_lod_preview_anchor(const glm::vec3& center, float inner_radius_m);
    void clear_far_lod_preview_anchor();

    u32 screen_width() const {
        return m_screen_width;
    }
    u32 screen_height() const {
        return m_screen_height;
    }
    float render_scale() const {
        return m_render_scale;
    }
    u32 internal_width() const {
        return m_internal_width;
    }
    u32 internal_height() const {
        return m_internal_height;
    }
    const glm::mat4& prev_view_proj() const {
        return m_prev_view_proj;
    } //  TAAU motion vectors
    float prev_time() const {
        return m_prev_time;
    } //  TAAU: prev-frame wind wall-clock
    void set_taau_enabled(bool e) {
        m_taau_enabled = e;
    } // render.taau (client wires from SystemConfig)
    //  route sky-view LUT (init + refresh) through GPU compute. render.sky_lut_gpu, default OFF.
    void set_sky_lut_gpu_enabled(bool e) {
        m_sky_lut.set_gpu_skyview_enabled(e);
    }
    const glm::vec2& taau_jitter_ndc() const {
        return m_taau_jitter_ndc;
    } // sub-pixel projection jitter for the G-buffer
    // Count of render-target reallocations since startup (one per real
    // on_resize). Surfaced as telemetry so the resize-stress gate can verify
    // targets were rebuilt during the mid-run mode toggles.
    u64 resize_generation() const {
        return m_resize_generation;
    }
    void clear_all_chunk_data(); // Force clear all cached chunk render data
    const MeshUploadFrameStats& get_last_mesh_upload_stats() const {
        return m_last_mesh_upload_stats;
    }
    const RenderPassFrameStats& get_last_render_pass_stats() const {
        return m_last_render_pass_stats;
    }
    const std::vector<RenderPassMetadata>& get_last_render_pass_metadata() const {
        return m_last_render_pass_metadata;
    }
    // the ORDERED stage ids render_frame actually dispatched last frame --
    // the runtime golden the declarative RenderGraph (BuildLuminumbraFrameGraph) is gated against
    // (schedule == this), so the declaration can never silently drift from the shipping order.
    // Render-only observability; never hashed (the server never renders).
    const std::vector<std::string>& frame_stage_trace() const {
        return m_frame_stage_trace;
    }
    RuntimeRenderStats get_runtime_render_stats() const;
    RenderResourceRegistryStats get_resource_registry_stats() const;
    std::vector<ShaderHealthEntry> get_shader_health() const;

    //  (live shader authoring, -1/2): the shader ROSTER — visit
    // every live Shader the pipeline owns (name + instance; instance may be null,
    // matching health's "not initialized"). Shader health, reload-all, the dev
    // shader panel, and the auto-reload watcher all consume this one enumeration.
    void enumerate_shaders(const std::function<void(const char*, Shader*)>& visit) const;
    struct ShaderReloadReport {
        int attempted = 0;
        int reloaded = 0;                  // swapped to a freshly compiled program
        int kept = 0;                      // Reload failed -> previous good program kept (rollback)
        std::vector<std::string> failures; // "name: diagnostic" per kept shader
    };
    // Hot-reload every roster shader from res/shaders/ (the  crawl). Render-only.
    ShaderReloadReport reload_all_shaders();

    // replace the glass-pane draw list (translucent
    // occluders for the shadow tint cascade;  OIT reuses the same items).
    // Render-only — panes never feed the sim or world_hash.
    void set_glass_panes(std::vector<GlassPaneItem> panes) {
        m_glass_pane_items = std::move(panes);
    }

    //  (/,  ): GPU auto-exposure metering. OFF by
    // default — the analytic AutoExposureForElevation curve stays the auto source
    // ( is satisfied by it); ON routes the mean-log-luminance readback
    // (luminance_meter stage -> AsyncReadbackRing -> the damped servo in
    // prepare_frame) into the auto half of SelectRenderExposure. Manual photo EV
    // overrides either. Render-only; never world_hash.
    void set_auto_exposure_metered(bool on) {
        m_auto_exposure_metered = on;
    }
    bool auto_exposure_metered() const {
        return m_auto_exposure_metered;
    }

    //  rendering (,  ): froxel volumetric quality.
    // 0 = analytic aerial only (the default — byte-identical to pre-froxel);
    // 1 = the froxel participating-media volume composes with the aerial.
    // Render-only; owner-ratified default at the visual checkpoint.
    void set_volumetric_quality(int q) {
        m_volumetric_quality = q;
    }
    int volumetric_quality() const {
        return m_volumetric_quality;
    }
    RenderHealthSnapshot get_render_health_snapshot(bool drain_gl_errors = false) const;
    // framescan: read-only access to the G-buffer attachments for the
    // what's-in-frame scan tool. The normal/material attachment (RGBA8) carries
    // the material id in its ALPHA byte (g_buffer.frag writes alpha =
    // MaterialID/255, so the byte == MaterialID). The FrameScan tool reads this
    // id attachment + the back color buffer to compute per-material coverage and
    // luminance.: this only exposes existing GL texture ids, it never
    // writes sim state or feeds world_hash. Defined in the.cpp because GBufferPass
    // is forward-declared here.
    const GBuffer& gbuffer() const;
    // SSAO parity: run the ORIGINAL pipeline-sourced SSAO+blur
    // GL sequence (golden A-leg) then the ctx-sourced SsaoPass seam (B-leg) into the
    // pass-owned blur FBO in the same frame, reading back R16F between legs, for each
    // ssao_quality 0..3. memcmp==0 catches a call-site ctx mis-population. Writes a
    // ssao_parity.txt verdict under out_dir; returns false on GL/IO error or mismatch.
    bool capture_ssao_parity(const std::filesystem::path& out_dir, const Camera& camera);
    //   (the  unlock): the in-process WHOLE-FRAME A/B. Re-dispatch the
    // full 23-stage sequence TWICE over the frame prepare_frame already built
    // (dispatch_stages is bit-idempotent per prepared frame), each leg final-blitted into
    // its own offscreen target, then FLIP the readbacks in-process. The score is EXACTLY
    // 0.0 or dispatch is not idempotent — the deterministic whole-frame gate this engine
    // never had (cross-run FLIP floors at ~0.057 noise; --smoke never renders). The
    //  execution migration compares old-vs-new dispatch paths through this same
    // entry point. Writes frame_parity_{a,b}.ppm + frame_parity.json under out_dir.
    // Returns false on GL/IO error, TAAU-on (history ping-pong breaks idempotence — Codex
    // ), or a nonzero score.
    bool capture_frame_parity(const Camera& camera, const std::filesystem::path& out_dir);
    // / closure gate: render the same prepared frame at native scale twice
    // (the scale-1 seam must be bit-exact), then at 0.67 internal scale and compare the
    // upscaled output against the native reference with the in-process FLIP metric.
    // The method restores the caller's render scale and offscreen target before returning.
    // Writes upscale_seam_{reference,scale1,scale067}.ppm and
    // upscale_seam_parity.json under out_dir. TAAU must be disabled.
    bool capture_upscale_seam_parity(const Camera& camera, const std::filesystem::path& out_dir);
    // Generated caustics texture id (0 when unavailable). Exposed for the
    // runtime scenario harness caustics-animation probe.
    u32 water_caustics_texture() const;
    void set_time_of_day(float normalized_time);
    // Current normalized day phase [0,1) (the value last pushed via set_time_of_day /
    // advanced by update_time_of_day)., read-only: exposed for the
    // photo-mode capture so a shot can record the time-of-day it was taken at (
    // ObservationMetadata). Never feeds the sim / world_hash.
    float get_time_of_day() const {
        return m_timeOfDay;
    }
    // hold the day clock at its current value (photo-mode TOD scrub) so
    // update_time_of_day stops auto-advancing; set_time_of_day still moves it. Render-only.
    void set_time_of_day_hold(bool hold) {
        m_timeOfDayHold = hold;
    }
    // drive the day clock from the AUTHORITATIVE sim tick —
    // tod = TimeOfDayFromTick(tick, day_length) (TimeOfDayModel.h). Called per frame
    // from live play; suppresses that frame's wall-clock advance so TOD is a pure
    // function of the tick (same tick -> same sun, independent of frame pacing).
    // Precedence unchanged: the photo-mode hold wins here, and scenario/scene pins
    // (set_time_of_day) run AFTER this in the frame, overwriting it. Paths that
    // never call this keep the legacy wall-clock advance byte-identically.
    void set_time_of_day_tick(std::uint64_t sim_tick);
    void set_day_length_ticks(std::uint64_t ticks) {
        m_dayLengthTicks = ticks == 0 ? 1 : ticks;
    }
    // SEASON / celestial model. The season phase is a PURE FUNCTION
    // of the authoritative TICK COUNT (integer epoch math; DeterministicMath for
    // the sun-path trig) -- never wall-clock, never a free-running float
    // accumulator (regression review). It is: the client pushes the
    // replicated sim tick here each frame; the season modulates the sun ARC
    // (declination / day length) and a biome material/foliage PALETTE tint ON TOP
    // of the existing time-of-day, and adds NOTHING to world_hash. One-way :
    // nothing here writes back into the sim.
    void set_season_tick(std::uint64_t tick);
    std::uint64_t get_season_tick() const {
        return m_seasonTick;
    }
    // Season phase in [0,1): 0 == summer solstice, 0.5 == winter solstice. Pure
    // function of the tick count (see kTicksPerSeasonCycle).
    float get_season_phase() const {
        return m_seasonPhase;
    }
    //  rendering: force the lunar illumination [0,1] (1 = full moon, bright
    // navigable night; ~0 = new moon, dark night). <0 (set via -1) restores the automatic
    // tick-derived lunar cycle. Render-only; never world_hash.
    void set_moon_illumination(float illum) {
        m_moonIllumOverride = illum;
    }
    float get_moon_illumination() const {
        return m_moonIllumination;
    }
    //  rendering ( /  ): photo-mode MANUAL exposure override.
    // When photo mode is active the player's lens (aperture/shutter/ISO -> EV) drives the
    // render exposure directly (see ExposureModel.h), OVERRIDING the analytic time-of-day
    // curve so the photographer exposes for the light. >0 forces the multiplier;
    // <0 (the default) restores the automatic TOD exposure. Render-only; never world_hash.
    void set_exposure_override(float mult) {
        m_exposureOverride = mult;
    }
    float get_exposure_override() const {
        return m_exposureOverride;
    }
    // Seasonal solar declination offset applied to the sun arc this frame
    // (positive raises the arc / lengthens the day in summer; negative lowers it
    // in winter). Tick-derived, deterministic.
    float get_season_sun_declination() const {
        return m_seasonSunDeclination;
    }
    // Sun elevation above the horizon this frame, radians (>0 == above horizon).
    // The season modulates its peak, so the season sweep reads distinct bands.
    float get_sun_elevation_rad() const {
        return m_sunElevationRad;
    }
    // Per-frame season palette tint (multiplied into the sun/ambient warmth):
    // warmer (R>B) toward summer, cooler (B>R) toward winter. Render-only.
    glm::vec3 get_season_palette_tint() const {
        return m_seasonPaletteTint;
    }
    // Long-period season cycle length in ticks (a full "year"). The phase wraps
    // on this; integer epoch math keeps it a pure tick function.
    static constexpr std::uint64_t kTicksPerSeasonCycle = 432000ull; // 4 h at 30 Hz
    // Runtime weather control (engine-generic). Intensity is clamped to
    // [0, 1]; WeatherType::None or intensity 0 disables the overlay entirely.
    void set_weather(WeatherType type, float intensity);
    WeatherType get_weather_type() const {
        return m_weather_type;
    }
    float get_weather_intensity() const {
        return m_weather_intensity;
    }
    // push the SIM-DRIVEN weather render state (one-way, ). The
    // client samples the replicated WeatherSystem at the camera each frame and
    // calls this; the overlay + wetness response read m_weather_state. Also sets
    // m_weather_type/intensity so the overlay's zero-work gate still fires when
    // there is no precipitation (driven clear == overlay off).
    void set_weather_state(const WeatherRenderState& state);
    const WeatherRenderState& get_weather_state() const {
        return m_weather_state;
    }

    // render-only cloud layer control. set_cloud_state enables the
    // wind-advected cloud dome + (optionally) the projected cast shadow and sets
    // the weather-derived coverage/biome/plane parameters. The wind scroll offset
    // is advanced internally each frame from the pushed wind direction * a tick-
    // derived phase (advance_cloud_phase, called inside update_time_of_day) so the
    // clouds drift deterministically with the wind; callers supply the static
    // coverage parameters here. One-way: the cloud field never feeds the sim.
    void set_cloud_state(const CloudRenderState& state);
    const CloudRenderState& get_cloud_state() const {
        return m_cloud_state;
    }

    // Render-optimization (cloud-raymarch-optimization): cloud/sky-dome render
    // quality. 0 = full (legacy full-res dome draw, byte-identical to before);
    // 1 = half (dome raymarched into a 1/2-per-axis FBO + depth-masked upsample
    // composite, the ~-3 ms structural win); 2 = quarter (1/4 per axis). The
    // half/quarter path is render-only (no world_hash impact). The pipeline starts
    // at 0; the client selects 2 unless LUMIN_CLOUD_QUALITY overrides it. Lazily
    // (re)allocates the reduced-res FBO; safe to call before or after startup.
    void set_cloud_quality(int quality);
    int get_cloud_quality() const {
        return m_cloud_quality;
    }

    // Render-optimization (ssao-gtao): AO algorithm/quality. 0 = legacy 64-sample
    // hemisphere SSAO (pipeline initial value); 1 = GTAO Low (8 spp) full-res;
    // 2 = GTAO High (18 spp) full-res; 3 = GTAO High at HALF-RES + joint-bilateral
    // depth-aware upsample (client default). Render-only (no world_hash impact).
    // The client applies LUMIN_SSAO_QUALITY when present.
    void set_ssao_quality(int quality) {
        m_ssao_quality = (quality < 0) ? 0 : (quality > 3 ? 3 : quality);
    }
    int get_ssao_quality() const {
        return m_ssao_quality;
    }

    // render-only lightning control. set_lightning_state pushes the
    // full-scene light pulse + bolt polyline for the current frame; the LightingPass
    // injects the pulse and rasterizes the bolt. Pass an inactive state (default) to
    // turn it off (zero added lighting cost). One-way : never fed to the sim.
    void set_lightning_state(const LightningRenderState& state);
    const LightningRenderState& get_lightning_state() const {
        return m_lightning_state;
    }
    // Lightning light-pulse GPU cost (ms) from the lighting-pass timer on the LAST
    // frame the pulse was active; 0.0 otherwise. The PerfRegression gate bounds this
    // against the transient ≤ 0.5 ms budget (documented design).
    double lightning_pulse_gpu_ms() const {
        return m_last_render_pass_stats.lightning_pulse_gpu_ms;
    }
    // Cloud-shadow GPU cost (ms) from the per-pass timer pair bracketing the
    // lighting pass on the LAST frame the cloud shadow was active; 0.0 otherwise.
    // The CloudShadow gate compares the clouds-on vs clouds-off lighting timing to
    // bound the ADDED per-fragment sample cost against the ≤ 0.4 ms budget .
    double cloud_shadow_gpu_ms() const {
        return m_last_render_pass_stats.cloud_shadow_gpu_ms;
    }

    //  atmosphere control: data-driven aerial-perspective / atmospheric
    // depth so the far field can be dialed crisp <-> realistic <-> dramatic from
    // one set of parameters. Render-only (never hashed); wired into
    // execute_aerial_pass and shared with the far-field path.
    struct AtmosphereParams {
        // Exponential extinction per metre — distance at which haze reaches ~63%
        // opacity is 1/density. Higher = thicker/closer haze (dramatic); lower =
        // crisp far view (Distant-Horizons-like).
        float aerial_density = 0.0016f;
        // Distance clamp for the fog term (m); matches the extended far horizon
        // (kF2OuterRangeMeters ~3000 m) so far terrain hazes fully into the sky
        // before the render edge instead of stopping short as a dark band.
        float aerial_max_distance = 3000.0f;
        // HDR scale of the shared sky in-scatter colour composited as haze.
        float inscatter_strength = 60.0f;
        // 0 = raw sky-view hue (bluer, crisp/aerial), 1 = fully warmed land veil
        // (b clamped <= g, avoids blue-tinting distant ground). Tunable so the
        // look ranges from clear-blue distance to warm hazy depth.
        float warmth = 1.0f;
    };
    void set_atmosphere_params(const AtmosphereParams& p) {
        m_atmosphere = p;
    }
    const AtmosphereParams& atmosphere_params() const {
        return m_atmosphere;
    }

    //  isolation/layer mode: set from the scenario config (CLI). Default
    // {All, Scene} is a no-op (render byte-stable). The SkyboxPass reads the
    // backdrop to flat-fill the background; the scenario harness reads the layer
    // mask to spawn-suppress non-selected content.
    void set_isolation_config(const Client::ScenarioHarness::IsolationConfig& cfg) {
        m_isolation_config = cfg;
    }
    const Client::ScenarioHarness::IsolationConfig& isolation_config() const {
        return m_isolation_config;
    }

    // --- Far-LOD region rendering ---
    // Far tile builds run on the attached JobSystem's Normal lane; without an
    // attached job system the far-LOD path stays inert.
    void attach_farlod_job_system(JobSystem* job_system);
    // Drains in-flight far tile builds (they sample the world system). MUST
    // run before the currently bound world is destroyed/recreated.
    void prepare_world_swap();
    FarLodSystem* farlod() {
        return m_farlod.get();
    }
    const FarLodSystem* farlod() const {
        return m_farlod.get();
    }

    // --- Particle framework. ---
    // The particle pass owns the fixed-capacity persistent-mapped instance pool,
    // the emitter set, and the sim-deterministic emitter-descriptor snapshot
    // surface. Exposed so the scenario harness can load fixture emitters and
    // snapshot/assert the descriptor set for the ParticleEmitterDeterminism gate.
    ParticlePass* particles() {
        return m_particle_pass.get();
    }
    const ParticlePass* particles() const {
        return m_particle_pass.get();
    }

    // --- Foliage scatter (, ). ---
    // The foliage pass owns the fixed-capacity persistent-mapped scatter
    // instance pool + the deterministic placement hash. Exposed so the scenario
    // harness can load the scatter set, push per-chunk placement inputs + the
    // wind bridge, and snapshot the instance set for the FoliageInstancing gate.
    FoliagePass* foliage() {
        return m_foliage_pass.get();
    }
    const FoliagePass* foliage() const {
        return m_foliage_pass.get();
    }

    // --- Procedural plants (, behind render.plant_procgen). ---
    // The procgen plant pass owns a dedicated VAO/VBO/EBO + a small dedicated
    // shader writing the deferred G-buffer. The client bakes one combined
    // world-space plant mesh and pushes it via set_plants; the pass draws it in
    // the geometry pass. OFF by default (zero GL work) so render is byte-stable.
    PlantProcgenPass* plant_procgen() {
        return m_plant_procgen_pass.get();
    }
    const PlantProcgenPass* plant_procgen() const {
        return m_plant_procgen_pass.get();
    }
    // push the one-way scent snapshot for the pheromone ground decal.
    // Defined in the.cpp (GroundDecalPass is forward-declared here). Render-only.
    void UpdateScentDecals(const ScentFieldRenderMirror& mirror);
    // Render-only G-buffer debug visualizer (DebugViewPass::Mode; 0 = off, normal render).
    // Diagnostic only: never touches sim state or world_hash. Default-OFF.
    void set_debug_view(int mode);

    // Vast procedural-tree palette: register a runtime-built mesh under a synthetic key into the
    // instanced static-mesh cache (forwards to GBufferPass). Lets the world scatter thousands of
    // instances of a small palette of procedural trees through the existing instanced + LOD path.
    void register_procgen_mesh(const std::string& key, std::unique_ptr<Mesh> mesh);
    [[nodiscard]] bool has_procgen_mesh(const std::string& key) const;
    // Render-only accessor for the current sun TRAVEL direction (the lighting
    // pass's m_sun.direction — points away from the sun, i.e. the direction the
    // light travels). The unit direction TO the sun is the negation. Exposed so
    // the procgen plant bake can feed phototropism the scene's real sun.
    glm::vec3 sun_direction() const {
        return m_sun.direction;
    }

    // --- Waterfalls (, ). ---
    // World-deterministic site detection (river course x steep height drop),
    // computed once per world and CACHED here (camera/frame independent —
    // regression review). The dressing (sheet shader,  spray, plunge foam, roar) is
    // render-only and never hashed; the SITES are a pure function of the
    // generated world (same seed -> same sites for every replay). Returns the
    // cached site set for `world`, detecting on first use.
    const std::vector<WaterfallSite>& waterfall_sites(const Systems::SHIELD_WorldSystem& world,
                                                      const WaterfallDetectParams& params = {}) {
        return m_waterfall_pass->waterfall_sites(world, params);
    }
    WaterfallSiteCache& waterfall_cache() {
        return m_waterfall_pass->waterfall_cache();
    }

    // build the live waterfall DRESSING for `world` (call once
    // after the world is entered). Queries waterfall_sites(world), bakes one
    // vertical world-space quad per site into the pass-owned VAO/VBO (drawn by
    // the falling-sheet shader in render_frame), records the per-site crest/foot Y
    // for the shader's height-down-the-fall normalization, and emits an  spray
    // emitter at each plunge foot (capped to bound particle cost).
    // dressing on a world-deterministic site set — never hashed (one-way).
    void prepare_waterfalls(const Systems::SHIELD_WorldSystem& world);

private:
    // Extracted render pass classes. Passes own their GL resources
    // (FBOs/textures/shaders); the pipeline keeps orchestration order, shared
    // state, stats collection, and GPU timer issue/collect calls.
    // ShadowPass friend removed (-T10): reads from RenderContext + ShadowPassInput +
    // make_terrain_submitter. GBufferPass friend removed (-T11): converted pass has 0 pipeline
    // refs (RenderContext + GBufferPassInput). SsaoPass friend removed (-T02): SsaoPass now reads
    // from RenderContext. LightingPass friend removed (-T12): RenderContext seam (cascade fixup
    // hoisted to make_lighting_context). WaterPass friend removed (-T16): reads from RenderContext
    // + WaterPassInput. SkyboxPass friend removed (-T04): RenderContext seam (opaque-copy relocated
    // to the call site). ParticlePass + FoliagePass friends removed (-T18/T17): read from
    // RenderContext. PlantProcgenPass friend removed (-T14): reads from RenderContext.
    // ===: the RenderPipeline friend list is EMPTY — every render
    // === pass is decoupled from the god-object behind the RenderContext seam.

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

        bool has_terrain_mesh() const {
            return terrain_vertex_count > 0 && terrain_index_count > 0;
        }
        bool has_water_mesh() const {
            return water_vertex_count > 0 && water_index_count > 0;
        }
    };

    struct ChunkMeshPayload {
        u32 mesh_version = 0;
        std::vector<VoxelVertex> vertices;
        std::vector<u32> indices;

        bool has_mesh() const {
            return !vertices.empty() && !indices.empty();
        }
    };

    // build the SSAO pass contract from pipeline state.
    // Shared by the render_frame call site and capture_ssao_parity so the gated
    // ctx is exactly the production ctx.
    RenderContext make_ssao_context(const Camera& camera);
    // the PlantProcgen pass contract (screen size + the
    // per-frame wall-clock snapshot). Camera is a separate execute arg.
    RenderContext make_plant_context();
    //  (/T17): Particle + Foliage pass contracts.
    RenderContext make_particle_context(const Camera& camera);
    RenderContext make_foliage_context(const Camera& camera);
    // Water pass contract (draw list built at the call site).
    RenderContext make_water_context(const Camera& camera);
    // GBuffer pass contract (frame state; submit callback +
    // far-LOD + static-model lane + impostors travel via GBufferPassInput).
    RenderContext make_gbuffer_context(const Camera& camera, const glm::vec4 frustum_planes[6]);
    //  ( / ): Lighting + Skybox pass contracts.
    // make_lighting_context also runs the hoisted shadow-cascade fixup (CPU-only).
    RenderContext make_lighting_context(const Camera& camera);
    RenderContext make_skybox_context(const Camera& camera);
    // pilot-pass seam contracts — DebugView (the rendering pilot pass) +
    // GroundDecal + the inline aerial/god-rays/TAAU post-passes. Render-only diagnostics/
    // dressing; every field is frame state adopted wrap-existing, never feeds world_hash.
    RenderContext make_debug_view_context(const Camera& camera);
    RenderContext make_ground_decal_context(const Camera& camera);
    RenderContext make_aerial_context(const Camera& camera);
    RenderContext make_god_rays_context(const Camera& camera);
    RenderContext make_taau_context();
    RenderContext make_glass_oit_context(const Camera& camera);
    RenderContext make_froxel_context(const Camera& camera);
    RenderContext make_waterfall_context(const Camera& camera);
    RenderContext make_luminance_meter_context();

    std::vector<ChunkMeshSnapshot>
    build_chunk_snapshots(const std::vector<Chunk*>& renderable_chunks) const;

    //   (the  unlock): render_frame is split into prepare_frame (ALL
    // per-frame CPU mutation — clock snapshot, time-of-day advance, light gathering,
    // chunk snapshots, GPU resource management, TAAU jitter, frustum, particle motion)
    // and dispatch_stages (the pure 23-stage GPU dispatch over the prepared state).
    // dispatch_stages is IDEMPOTENT per prepared frame — running it twice produces
    // bit-identical pixels — which is what the in-process whole-frame A/B harness
    // (capture_frame_parity) and the  executor migration gate on.
    struct FramePrepared {
        entt::registry* registry = nullptr;
        Systems::SHIELD_WorldSystem* world_system = nullptr;
        float delta_time = 0.0f;
        bool wireframe = false;
        glm::mat4 projection{1.0f};
        glm::mat4 view{1.0f};
        glm::vec4 frustum_planes[6]{};
        std::vector<ChunkMeshSnapshot> renderable_chunk_snapshots;
        bool valid = false;
    };
    void prepare_frame(entt::registry& registry,
                       Systems::SHIELD_WorldSystem& world_system,
                       const Camera& camera,
                       float deltaTime,
                       bool wireframe);
    void dispatch_stages(const Camera& camera);

    using StageExecutorFn = void (RenderPipeline::*)(const Camera&);
    // node name -> extracted stage body, authored order.
    // Coverage (every declared node maps to an executor) is enforced on every real
    // frame by the drift guard in get_render_health_snapshot — the strongest pin:
    // it runs in the shipping binary, not just a test fixture.
    static const std::vector<std::pair<std::string, StageExecutorFn>>& stage_executor_table();

    //   ( execution migration): each frame-graph node's body,
    // moved VERBATIM out of the dispatch script. Every member owns its own trace
    // record, runtime guard, and GL pre/post state; each reads only FramePrepared
    // + pipeline members (the dispatch-idempotence contract). The names match
    // BuildLuminumbraFrameGraph's node names 1:1 — the executor table maps
    // node name -> member and the drift guard keeps declaration/dispatch in sync.
    void execute_stage_shadow(const Camera& camera);
    void execute_stage_gbuffer(const Camera& camera);
    void execute_stage_plant_procgen(const Camera& camera);
    void execute_stage_ground_decals(const Camera& camera);
    void execute_stage_ssao(const Camera& camera);
    void execute_stage_ssao_blur(const Camera& camera);
    void execute_stage_lighting(const Camera& camera);
    void execute_stage_depth_blit_to_lighting(const Camera& camera);
    void execute_stage_skybox(const Camera& camera);
    void execute_stage_opaque_snapshot(const Camera& camera);
    void execute_stage_water(const Camera& camera);
    void execute_stage_waterfall(const Camera& camera);
    void execute_stage_glass_oit_accum(const Camera& camera);
    void execute_stage_glass_oit_resolve(const Camera& camera);
    void execute_stage_weather_opaque_snapshot(const Camera& camera);
    void execute_stage_weather_overlay(const Camera& camera);
    void execute_stage_froxel_inject(const Camera& camera);
    void execute_stage_froxel_integrate(const Camera& camera);
    void execute_stage_aerial(const Camera& camera);
    void execute_stage_god_rays(const Camera& camera);
    void execute_stage_foliage(const Camera& camera);
    void execute_stage_taau_resolve(const Camera& camera);
    void execute_stage_luminance_meter(const Camera& camera);
    void execute_stage_particles(const Camera& camera);
    void execute_stage_lightning_overlay(const Camera& camera);
    void execute_stage_final_blit(const Camera& camera);
    void execute_stage_debug_view(const Camera& camera);

    void ensure_terrain_culling_hierarchy(const std::vector<ChunkMeshSnapshot>& renderable_chunks);
    void manage_chunk_gpu_resources(const std::vector<ChunkMeshSnapshot>& renderable_chunks,
                                    const Camera& camera);
    bool copy_terrain_mesh_payload(const ChunkMeshSnapshot& chunk, ChunkMeshPayload& payload) const;
    void upload_chunk_mesh(const ChunkMeshSnapshot& chunk, const ChunkMeshPayload& payload);
    void unload_chunk_resources(ChunkID chunk_id);

    void manage_water_gpu_resources(const std::vector<ChunkMeshSnapshot>& renderable_chunks,
                                    const Camera& camera);
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
        Particle,
        Foliage, // Instanced foliage scatter pass.
        Aerial,  // Analytic aerial-perspective fullscreen pass.
        FinalBlit,
        Count,
    };
    static constexpr size_t kGpuTimerPassCount = static_cast<size_t>(GpuTimerPass::Count);
    // Ring of 3 frame slots so frame N polls the queries issued at frame
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
    // append a stage id to m_frame_stage_trace as render_frame dispatches it. Pure CPU
    // append (no GL, never hashed); called unconditionally at each stage's authored slot so the
    // trace is the full canonical order regardless of which conditional stages did GL work.
    void record_frame_stage(const char* name) {
        m_frame_stage_trace.emplace_back(name);
    }

    std::vector<glm::mat4> get_light_space_matrices(const Camera& camera);

    void update_time_of_day(float deltaTime);
    float m_timeOfDay = 0.5f;     // Start at sunrise
    bool m_timeOfDayHold = false; // freeze auto-advance for photo-mode TOD scrub
    float m_dayDurationSeconds = 60.0f;
    // tick-authority state. m_todTickDriven marks "the sim tick fed TOD this
    // frame" so update_time_of_day skips the wall-clock advance; cleared every frame.
    std::uint64_t m_dayLengthTicks = 1800; // == TimeOfDayModel kDefaultDayLengthTicks
    bool m_todTickDriven = false;

    u32 m_screen_width = 0;
    u32 m_screen_height = 0;

    //  render-scale seam. The scene is rendered at m_internal_* (= output x
    // m_render_scale) into the scaled G-buffer/lighting/SSAO intermediates, then upscaled
    // to output (m_screen_*) for post/UI/backbuffer; the shadow atlas, TAAU history and
    // the backbuffer stay at output resolution. user.render_scale configures the
    // scale; at 1.0 m_internal_* == m_screen_* (lround(N*1.0f) == N).
    float m_render_scale = 1.0f;
    u32 m_internal_width = 0;
    u32 m_internal_height = 0;
    //  (Group K): one glfwGetTime snapshot per frame (set at render_frame
    // top), fed to RenderContext.time_seconds for every converted pass.
    float m_wall_clock_time = 0.0f;
    //  TAAU: previous-frame UNJITTERED view-projection, used by the G-buffer to write
    // screen-space motion vectors (current screen pos - reprojected previous screen pos).
    // Advanced at render_frame end; identity on frame 0 (the shader's w<=0 guard => zero motion).
    glm::mat4 m_prev_view_proj = glm::mat4(1.0f);
    float m_prev_time = 0.0f; //  TAAU: prev-frame wind wall-clock (instanced vert prev-pos)
    // Render-target reallocation counter. Bumped once per
    // real on_resize so the resize-stress gate can assert targets were rebuilt.
    u64 m_resize_generation = 0;
    //  offscreen render-target redirect for the live preview.
    // When active, render_frame's final blit targets m_offscreen_target_fbo at
    // m_offscreen_target_w/h instead of framebuffer 0. Default inactive == the
    // legacy default-0 blit (byte-identical).
    bool m_offscreen_target_active = false;
    bool m_far_lod_enabled = true; // worldgen preview disables far-LOD (transient-world UAF)
    u32 m_offscreen_target_fbo = 0;
    u32 m_offscreen_target_w = 0;
    u32 m_offscreen_target_h = 0;
    std::filesystem::path m_root_path;

    DirectionalLight m_sun;
    glm::vec3 m_moonDirection;
    // Moon TOWARD-LIGHT direction (same convention as m_sun.direction: the value
    // is used directly as the L vector in the lighting pass and as the cascade
    // light direction at night). The moon is the anti-sun, overhead at midnight.
    // Distinct from m_moonDirection (the moon TRAVEL dir consumed by SkyboxPass
    // to place the moon disc), which is intentionally left unchanged.
    glm::vec3 m_moonLightDir{0.0f, -1.0f, 0.0f};
    // Moon elevation factor: dot(m_moonLightDir,(0,-1,0)); >0 when the moon is
    // above the horizon. Drives the day->moon cascade switch.
    float m_moonUpFactor = 0.0f;
    glm::vec3 m_skyAmbientColor;
    //  controllable atmosphere (aerial perspective) parameters.
    AtmosphereParams m_atmosphere{};
    // continuous day->twilight->night factor derived
    // from the sun's elevation, smoothly 1 (sun high) -> 0 (sun below horizon).
    // The sky dome reads this so its brightness/tint tracks time-of-day with
    // the SAME elevation signal (sun_up_factor) that drives the ambient day/night
    // blend (the direct-sun color is now transmittance-coupled — ), instead
    // of the clamped m_sun.intensity (which saturates to 1 while the sun is still low,
    // leaving the dusk dome stuck at full midday and the night dome bright).
    float m_skyDayFactor = 1.0f;
    //  rendering: deterministic time-of-day exposure (eye adaptation),
    // computed in update_time_of_day as a pure function of the sun elevation and fed to
    // the lighting pass via RenderContext.exposure. DAY == the prior static LUMIN_GRADE
    // exposure (noon image preserved); lifts at night for navigability; dips through the
    // golden-hour band for contrast/mood. Render-only — never world_hash.
    float m_scene_exposure = 1.12f;
    //  rendering: photo-mode manual exposure override. >0 forces the render
    // exposure (mapped from the lens EV by ExposureModel::ManualExposureMultiplier) over
    // m_scene_exposure; <0 (default) selects the automatic TOD exposure. See
    // ExposureModel::SelectRenderExposure — the precedence applied at ctx assembly.
    // Render-only; never world_hash.
    float m_exposureOverride = -1.0f;
    // 1.0 when the render camera is below a water surface (drives the aerial pass's
    // underwater murk). Set per-frame in render_frame from WaterLevelAt.
    float m_underwater_factor = 0.0f;
    // SEASON state, all DERIVED from m_seasonTick (a pure function
    // of the authoritative sim tick -- no wall-clock, no float accumulator). The
    // phase/declination/tint are recomputed inside update_time_of_day from the
    // integer tick, so they are reproducible from tick alone and never hashed.
    std::uint64_t m_seasonTick = 0;
    float m_seasonPhase = 0.0f; // [0,1): 0 summer, 0.5 winter
    //  rendering: LUNAR PHASE -> the "two night modes". A render-only lunar
    // cycle (pure function of the tick, like the season; never world_hash) sets how much the
    // moon lights the night: ~1 full moon (bright, navigable, crisp moon shadows), down to a
    // starlight floor at new moon (dark night that wants a torch). m_moonIllumOverride>=0
    // (LUMIN_MOON env / set_moon_illumination) forces a value for photo/testing.
    float m_moonIllumination = 0.85f;
    float m_moonIllumOverride = -1.0f;
    //  rendering : the moon's DEDICATED radiance (cool key colour), fed to the
    // lighting pass via RenderContext.moon_radiance -> u_moonRadiance instead of a hardcoded shader
    // const, so the moon is tunable independent of the sun. Default == the prior shader kMoonColor
    // (byte-identical until deliberately re-calibrated). This is a fixed pipeline value.
    glm::vec3 m_moonRadiance = glm::vec3(0.40f, 0.52f, 0.92f);
    static constexpr std::uint64_t kTicksPerLunarCycle =
        54000ull;                        // 30 min @ 30 Hz (a lunar "month")
    float m_seasonSunDeclination = 0.0f; // radians, seasonal arc tilt
    float m_sunElevationRad = 0.0f;      // sun elevation this frame (radians)
    glm::vec3 m_seasonPaletteTint{1.0f}; // warm(summer)/cool(winter) palette tint
    WeatherType m_weather_type = WeatherType::None;
    float m_weather_intensity = 0.0f;
    // SIM-DRIVEN weather render state (one-way from WeatherSystem).
    WeatherRenderState m_weather_state;
    // render-only cloud layer state + the wind-advection scroll
    // phase. m_cloud_phase accumulates the tick-derived deltaTime so the scroll
    // offset (= wind_dir * wind_strength * phase) advances with the wind; it is a
    // pure render accumulator (never hashed, one-way). advance_cloud_phase runs
    // inside update_time_of_day on the same deltaTime that drives the sun.
    CloudRenderState m_cloud_state;
    float m_cloud_phase = 0.0f;
    void advance_cloud_phase(float deltaTime);
    // Render-optimization (cloud-raymarch-optimization, ): reduced-res sky
    // dome target + composite. m_cloud_quality 0 keeps the legacy full-res dome
    // draw (these stay 0/unused). Quality 1/2 renders the dome into m_halfres_cloud
    // at 1/2 or 1/4 per axis, then m_cloud_composite_shader upsamples + depth-masks
    // it into the lighting FBO. Render-only; never hashed.
    int m_cloud_quality = 0;
    int m_ssao_quality = 0; // render-optimization (ssao-gtao): 0 legacy SSAO, 1 GTAO High
    struct HalfResCloudTarget {
        u32 fbo = 0;
        u32 color_texture = 0; // RGBA16F reduced-res sky dome
        u32 width = 0;
        u32 height = 0;
        int scale = 0; // axis divisor the buffer is sized for (1=>off, 2, 4)
    };
    HalfResCloudTarget m_halfres_cloud;
    std::unique_ptr<Shader> m_cloud_composite_shader;
    // (Re)allocate m_halfres_cloud for the current screen size + m_cloud_quality.
    // No-op when quality 0 (releases any existing target). Idempotent if already
    // sized correctly.
    void init_halfres_cloud();
    void destroy_halfres_cloud();
    // render-only lightning pulse + bolt state for the current frame.
    LightningRenderState m_lightning_state;
    std::unique_ptr<FarLodSystem> m_farlod;
    std::unique_ptr<GBufferPass> m_gbuffer_pass;
    std::unique_ptr<ShadowPass> m_shadow_pass;
    std::unique_ptr<SsaoPass> m_ssao_pass;
    std::unique_ptr<LightingPass> m_lighting_pass;
    std::unique_ptr<WaterPass> m_water_pass;
    std::unique_ptr<SkyboxPass> m_skybox_pass;
    std::unique_ptr<ParticlePass> m_particle_pass;          //
    std::unique_ptr<FoliagePass> m_foliage_pass;            //
    std::unique_ptr<PlantProcgenPass> m_plant_procgen_pass; //  (flag-gated)
    std::unique_ptr<GroundDecalPass> m_ground_decal_pass;   //   pheromone trail (default-OFF)
    std::unique_ptr<DebugViewPass>
        m_debug_view_pass; // render-only G-buffer debug overlay (default-OFF)
    std::unique_ptr<FinalBlitPass> m_final_blit_pass; // -T01: first pass on the RenderContext seam
    // The aerial/god-rays/TAAU post-passes take a RenderContext& (frame state
    // from ctx; only the pass-owned shaders + TAAU history stay members) so
    // they sit on the same seam as the other pass classes.
    std::unique_ptr<AerialPass> m_aerial_pass;
    std::unique_ptr<GodRaysPass> m_god_rays_pass;
    std::unique_ptr<TaauPass> m_taau_pass;
    std::unique_ptr<GlassOitPass> m_glass_oit_pass;
    std::unique_ptr<FroxelPass> m_froxel_pass;
    std::unique_ptr<WaterfallPass> m_waterfall_pass;
    std::unique_ptr<LuminanceMeterPass> m_luminance_meter_pass;
    RenderResourceRegistry m_render_registry; // typed render-resource handles (adopt/lookup)
    Client::ScenarioHarness::IsolationConfig m_isolation_config; //  (default {All,Scene} = no-op)

    //  Hillaire 2020 atmospheric scattering. The LUTs are built once at
    // startup and the sky-view LUT refreshed when the sun moves; the skybox pass
    // samples the sky-view LUT, the lighting pass + aerial pass read the SAME
    // transmittance/multi-scatter pair (coherent sun/sky/ambient/fog palette).
    // The aerial pass is the analytic aerial-perspective fullscreen term wiring
    // the previously dormant volumetric_lighting.frag. Render-only (documented design).
    SkyAtmosphereLut m_sky_lut;
    //  TAAU resolve (render.taau, default OFF). Motion-reprojected temporal AA over the lit HDR
    // color with a 3x3 neighborhood-clamp anti-ghost; ping-pong history. Flag OFF -> the pass never
    // runs and the lighting color blits through unchanged (byte-identical default render).
    bool m_taau_enabled = false;
    glm::vec2 m_taau_jitter_ndc =
        glm::vec2(0.0f);        // current-frame sub-pixel projection jitter (NDC); (0,0) when OFF
    unsigned m_taau_frame = 0u; // Halton sequence index
    // Screen-space crepuscular rays (god rays). Additive pass over the lit scene when
    // the sun is above the horizon + on screen. Render-only.
    double m_sky_full_precompute_ms = 0.0;
    double m_sky_view_refresh_ms = 0.0; // last sky-view refresh cost (0 = none this frame)
    // Sky-derived scattering ambient (the sky-view hemisphere integral); folded
    // into m_skyAmbientColor so lighting/ambient share the LUT transmittance.
    glm::vec3 m_skyScatterAmbient{0.0f};
    void init_sky_lut();

public:
    const SkyAtmosphereLut& sky_lut() const {
        return m_sky_lut;
    }

private:
    std::unordered_map<ChunkID, ChunkRenderData> m_chunk_render_data;
    std::unordered_map<ChunkID, WaterRenderData> m_water_render_data;
    std::vector<ChunkRenderData> m_free_chunk_render_slots;
    std::vector<WaterRenderData> m_free_water_render_slots;

    // shared bucketed persistent-mapped pool backing all live terrain
    // chunk geometry, plus the per-frame MDI scratch buffers (an indirect
    // command buffer and a chunk-origin SSBO, double/triple-buffered to avoid
    // stalling on the GPU still reading last frame's commands). The G-buffer and
    // shadow passes both submit live terrain through draw_chunks_mdi.
    ChunkGeometryPool m_chunk_geometry_pool;

    struct DrawElementsIndirectCommand {
        GLuint count;         // index count
        GLuint instanceCount; // 1
        GLuint firstIndex;    // index_offset
        GLuint baseVertex;    // vertex_offset
        GLuint baseInstance;  // gl_DrawID fallback / origin index
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
    std::vector<std::string> m_frame_stage_trace; //  ( ): last frame's dispatch order

    // the prepared per-frame state dispatch_stages reads (see prepare_frame).
    FramePrepared m_frame_prepared;
    // the lighting ctx built by execute_stage_lighting, reused by the
    // opaque-snapshot and lightning-overlay stages (its fields are frame-stable —
    // the pre-split code built it once at function scope for exactly this reuse).
    RenderContext m_frame_lighting_ctx;
    //  CPU per-phase markers — members because they now span the
    // prepare/dispatch boundary (set in prepare_frame/dispatch_stages, read in the
    // render_frame epilogue's stats block).
    std::chrono::steady_clock::time_point m_cpu_frame_t0{};
    std::chrono::steady_clock::time_point m_cpu_frame_prep{};
    std::chrono::steady_clock::time_point m_cpu_frame_shadow{};
    std::chrono::steady_clock::time_point m_cpu_frame_gbuf{};
    // the harness's SECOND dispatch of a frame suppresses GPU timer-query
    // records (the ring slot records once per frame; timers are observability only).
    bool m_gpu_timers_suppressed = false;

    u32 m_screen_quad_vao = 0;
    u32 m_screen_quad_vbo = 0;

    // the glass-pane draw list (shadow tint occluders;
    // 's OIT glass reuses the same items) + the shared unit-quad VAO.
    std::vector<GlassPaneItem> m_glass_pane_items;
    u32 m_glass_quad_vao = 0;
    u32 m_glass_quad_vbo = 0;

    // The GPU auto-exposure meter pass owns its 1-workgroup compute reduction,
    // 1-float SSBO, and readback ring. Pipeline state retains only the enable
    // switch and damped exposure servo consumed by lighting.
    bool m_auto_exposure_metered = false;
    float m_metered_exposure = 1.0f;
    bool m_metered_valid = false;

    // Froxel quality remains pipeline orchestration state; the pass owns both
    // compute programs and volumes and reads this value through RenderContext.
    int m_volumetric_quality = 0;

    u32 m_terrainTextureArray = 0;
    // Per-material triplanar normal-map array. Same layer order as the
    // albedo array; layer indices come from the material LUT normal_layer
    // column. RGBA8 tangent-space (OpenGL convention) normal maps.
    u32 m_terrainNormalArray = 0;
    // terrain PBR (roughness-map): per-material triplanar ROUGHNESS-map array. Same layer
    // order as albedo/normal; linear RGBA8 (roughness in.r). Sampled in
    // g_buffer.frag to drive per-texel roughness (was a flat per-material scalar
    // from materials.json). m_terrainRoughnessValid is 0 when any layer fell back
    // (then the shader keeps the scalar). Render-only — no world_hash impact.
    u32 m_terrainRoughnessArray = 0;
    int m_terrainRoughnessValid = 0;
    u32 m_materialLUT = 0;
    // Aether scalar field as an R32F texture for the lighting-pass
    // emissive tap. Updated per frame from the sim field (one-way bridge).
    // m_aetherFieldActive gates the glow so a no-aether world stays pixel-identical
    // (RenderHealth-neutral until a world enables the field).
    u32 m_aetherFieldTexture = 0;
    glm::vec2 m_aetherFieldWorldOrigin{0.0f};
    float m_aetherFieldCellSize = 24.0f;
    int m_aetherFieldExtent = 0;
    bool m_aetherFieldActive = false;
    // the glow grade (defaults == the lighting_pass.frag initializers).
    glm::vec3 m_aetherGlowColor{0.30f, 0.55f, 0.95f};
    float m_aetherGlowIntensity = 2.0f;
    float m_aetherMaterialModulation = 0.0f; // 0 = pixel-identical
    bool m_aetherPolarityActive = false;     // dual-tap live?
    bool m_aetherFieldTextureIsDual = false; // current alloc format
    // render-only snow ground cover (0 = byte-identical default).
    float m_snowCover = 0.0f;
    size_t m_terrain_texture_fallback_layers = 0;
    // Resolution the terrain albedo/normal arrays are allocated at.: raised
    // 256 -> 1024. The repo already ships full AmbientCG 2K CC0 PBR sets per material
    // (Rock028/Ground048/Grass003/Ground087/Gravel040) but the runtime had been
    // loading 256px.ltex plates that threw away the near-ground micro-detail the
    // visual-fidelity  fidelity floor wants. The 1024.ltex are regenerated from the 2K Color/
    // NormalGL PNGs (16x the texels). VRAM: 1024^2 x 5 layers x 4 B x 2 arrays = 42 MB
    // (trivial on the 16 GB RTX 5070 Ti target).
    static constexpr int kTerrainTextureResolution = 1024;

    // Emissive intensity LUT scale. The RGBA8 material LUT stores
    // emissive_intensity normalized by this ceiling; the lighting pass rescales.
    // Authored intensities run 0..~4 this iteration; 8 leaves headroom.
    static constexpr float kEmissiveLutScale = 8.0f;

    // --- Skinned/creature UV-mapped textures ---
    // GL_TEXTURE_2D_ARRAY of UV-sampled creature textures; layer 0 = albedo,
    // layer 1 = tangent-space normal. Sampled by the skinned-mesh G-buffer path
    // (skinned_mesh.vert + g_buffer.frag u_skinnedTextures). Separate from the
    // terrain triplanar arrays (different sampling model). The engine names no
    // creature here: the texture *set* is supplied by the caller (the scenario
    // harness reads paths from the game archetype JSON — ).
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
    u32 skinned_texture_array() const {
        return m_skinnedTextureArray;
    }
    int skinned_albedo_layer() const {
        return m_skinnedAlbedoLayer;
    }
    int skinned_normal_layer() const {
        return m_skinnedNormalLayer;
    }
    // Generic, data-driven texture-set loader. Uploads the albedo (.ltex) into
    // layer 0 and the tangent-space normal (.ltex) into layer 1 of the skinned
    // array, returning the layer indices through albedo_layer_out/normal_layer_out.
    // A missing/mismatched file keeps that layer's flat fallback. Returns true if
    // the albedo loaded (the mesh is then UV-textured). Paths are caller-supplied,
    // so the engine carries no creature/asset names.
    bool load_skinned_texture_set(const std::filesystem::path& albedo_path,
                                  const std::filesystem::path& normal_path,
                                  int& albedo_layer_out,
                                  int& normal_layer_out);

    // ---  static-model UV texture lane ---
    // A dedicated GL_TEXTURE_2D_ARRAY (separate from the 256x256 skinned/creature
    // array) holding per-static-model albedo+normal layers sampled by the mesh's
    // own UVs. Used to give the tree parts (trunk/branch/leaves) real bark/leaf
    // textures instead of the world-projected terrain triplanar. Per-meshPath
    // layer + alpha-test lookup is consumed by GBufferPass::geometry_pass_static_meshes.
    // StaticModelTex hoisted to namespace scope (StaticModelTex.h);
    // alias keeps RenderPipeline::StaticModelTex (ImpostorBake.cpp:100) compatible.
    using StaticModelTex = Luminumbra::Rendering::StaticModelTex;

private:
    u32 m_staticModelTextureArray = 0;
    static constexpr int kStaticModelTextureResolution = 512;
    static constexpr int kStaticModelTextureLayers = 8;
    int m_staticModelNextLayer = 0; // next free layer pair to fill
    std::unordered_map<std::string, StaticModelTex> m_staticModelTextures; // meshPath -> layers
    void init_static_model_texture_array();

public:
    // Far-field tree impostors (default ON; LUMIN_TREE_IMPOSTORS=0 disables). The atlas is
    // baked once at init (BakeTreeImpostorAtlasToTextures) and the GBuffer LOD3 path draws one
    // camera-facing quad per far tree sampling it. Read-only accessors for GBufferPass.
    bool tree_impostor_enabled() const {
        return m_treeImpostorsEnabled;
    }
    u32 tree_impostor_albedo() const {
        return m_treeImpostorAlbedo;
    }
    u32 tree_impostor_normal() const {
        return m_treeImpostorNormal;
    }
    int tree_impostor_grid() const {
        return m_treeImpostorGrid;
    }
    float tree_impostor_radius() const {
        return m_treeImpostorRadius;
    }
    float tree_impostor_sphere_y() const {
        return m_treeImpostorSphereY;
    }

private:
    bool m_treeImpostorsEnabled = false;
    u32 m_treeImpostorAlbedo = 0;
    u32 m_treeImpostorNormal = 0;
    int m_treeImpostorGrid = 0;
    float m_treeImpostorRadius = 0.0f;
    float m_treeImpostorSphereY = 0.0f;
    // Loads albedo+normal.ltex into the next free layer pair; returns false +
    // keeps the flat fallback on failure. Layer indices come back via the outs.
    bool load_static_model_texture_set(const std::filesystem::path& albedo_path,
                                       const std::filesystem::path& normal_path,
                                       int& albedo_layer_out,
                                       int& normal_layer_out);
    // Loads the tree-part textures and populates m_staticModelTextures (data-driven
    // from data/models/trees/tree_textures.json; silently no-ops if absent).
    void register_static_model_textures();

public:
    u32 static_model_texture_array() const {
        return m_staticModelTextureArray;
    }
    const StaticModelTex* static_model_tex(const std::string& mesh_path) const {
        auto it = m_staticModelTextures.find(mesh_path);
        if (it != m_staticModelTextures.end())
            return &it->second;
        // VAST-FOREST: the procedural tree palette (procgen://tree_N_{leaf,bark}[.lodN])
        // has no JSON entry, so its leaf/bark submeshes fell through to the world
        // triplanar GRASS texture with NO alpha cutout -> flat green solid blobs.
        // Reuse the baked tree-part textures by part suffix: leaves -> the alpha-tested
        // leaf plate (cutout foliage cards), bark -> the branch plate. Render-only.
        if (mesh_path.rfind("procgen://tree", 0) == 0) {
            const char* baked = nullptr;
            if (mesh_path.find("_leaf") != std::string::npos)
                baked = "data/models/trees/tree_small_02_leaves.lmesh";
            else if (mesh_path.find("_bark") != std::string::npos)
                baked = "data/models/trees/tree_small_02_branches.lmesh";
            if (baked) {
                auto b = m_staticModelTextures.find(baked);
                if (b != m_staticModelTextures.end())
                    return &b->second;
            }
        }
        return nullptr;
    }

private:
private:
    // Per-material LUT columns parsed from data/common/materials.json
    // (texture_layer / normal_layer / tiling — documented design, owned by ).
    // Indexed by material id; defaults mean "untextured / flat" so unknown ids
    // and the crystal/water render kinds keep the G-buffer base color.
    struct MaterialTextureLut {
        std::array<int, 256> texture_layer;        // -1 = untextured
        std::array<int, 256> normal_layer;         // -1 = flat
        std::array<float, 256> tiling;             // world-units per repeat (>0)
        std::array<float, 256> emissive_intensity; // 0 = non-emissive ()
        std::array<float, 256> roughness;          // 0..1, default 0.85 ()
        std::array<bool, 256> roughness_set;       // material declared roughness
        // terrain PBR: per-material metallic, data-driven from materials.json (was
        // hardcoded per-id in init_material_lut, leaving the JSON `metallic`
        // column dead). 0..1; absent -> the authored row0.r fallback is kept.
        std::array<float, 256> metallic;    // 0..1, default 0.0
        std::array<bool, 256> metallic_set; // material declared metallic
        //  per-material albedo multiplier applied to the
        // baked (textured) G-buffer albedo. Default 1.0 (unscaled). Calibrates a
        // physically-bright photographic texture down to a natural lit tone when
        // the irradiance chain would otherwise clip it past the ACES knee (the
        // sun-bright near-sea-level sand-flat). Render-only.
        std::array<float, 256> albedo_scale; // >0, default 1.0
        //  dusty- palette: per-material warm albedo TINT (render-only
        // content). Multiplied onto the baked textured albedo alongside
        // albedo_scale in the g_buffer triplanar branch. Default [1,1,1] is a
        // byte-identical no-op. Distinct from the post-process LUMIN_GRADE /
        // LUMIN_ATMOS stages: this nudges the base surface color (khaki/ochre)
        // so the warmth survives relighting. Keep small + luminance-preserving.
        std::array<glm::vec3, 256> albedo_tint; // default [1,1,1] (no-op)
        int terrain_layer_count = 0;            // distinct albedo layers loaded
        MaterialTextureLut() {
            texture_layer.fill(-1);
            normal_layer.fill(-1);
            tiling.fill(4.0f);
            emissive_intensity.fill(0.0f);
            roughness.fill(0.85f);
            roughness_set.fill(false);
            metallic.fill(0.0f);
            metallic_set.fill(false);
            albedo_scale.fill(1.0f);
            albedo_tint.fill(glm::vec3(1.0f));
        }
    };
    MaterialTextureLut m_material_texture_lut;
    // Parses materials.json texture_layer/normal_layer/tiling columns into
    // m_material_texture_lut. Missing file/columns leave defaults (untextured).
    void load_material_texture_lut();

    void init_terrain_textures();
    void init_material_lut();

    // --- Texture-array residency manager ---
    // Imports.ltex assets into GL_TEXTURE_2D_ARRAY objects bucketed by size
    struct LtexCpuImage {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 0;
        uint16_t mip_count = 0;
        std::vector<unsigned char> bytes; // full mip chain, level 0 first
    };

    // Loads a.ltex file from disk into a CPU image (full mip chain). Returns
    // false on any header/size error.
    bool load_ltex_cpu_image(const std::filesystem::path& path, LtexCpuImage& out) const;

    std::vector<PointLight> m_point_lights_this_frame;
    const int MAX_POINT_LIGHTS = 32;

    void gather_lights(entt::registry& registry, const glm::vec3& camera_pos);

    bool m_started = false;

    // Hierarchical frustum culling system
    struct AABB {
        glm::vec3 min;
        glm::vec3 max;

        AABB() = default;
        AABB(const glm::vec3& min, const glm::vec3& max)
            : min(min)
            , max(max) {}
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
        CullingNode(const AABB& bounds)
            : bounds(bounds) {}
    };

    class HierarchicalCuller {
    public:
        void BuildHierarchy(const std::vector<ChunkMeshSnapshot>& chunks);
        void CullRecursive(const glm::vec4 frustum_planes[6],
                           CullingNode* node,
                           std::vector<const ChunkCullEntry*>& visible);
        void CullHierarchical(const glm::vec4 frustum_planes[6],
                              std::vector<const ChunkCullEntry*>& visible);
        void Clear();

        std::unique_ptr<CullingNode> m_root; // Made public for access

    private:
        static constexpr int MAX_CHUNKS_PER_NODE = 8;
        static constexpr int MAX_DEPTH = 4;

        void
        BuildRecursive(CullingNode* node, const std::vector<ChunkCullEntry>& chunks, int depth);
        bool AABBFrustumCulled(const AABB& aabb, const glm::vec4 frustum_planes[6]);
    };

    HierarchicalCuller m_hierarchicalCuller;

    // builds and issues glMultiDrawElementsIndirect for the supplied
    // visible live chunks (one command per pool-resident chunk, grouped by pool
    // block -> one MDI call per block). The chunk world origin reaches the
    // vertex shader through the instanced aOrigin attribute (binding 1) indexed
    // by each command's baseInstance; the caller's shader must declare that
    // attribute and set u_useInstanceOrigin = 1. Returns draw + index totals.
    // Declared here (not with the other MDI helpers above) because it takes a
    // vector of ChunkCullEntry, which is defined just above.
    void draw_chunks_mdi(const std::vector<const ChunkCullEntry*>& visible_chunks,
                         std::size_t& out_draws,
                         std::size_t& out_indices);
    //  (Codex-signed-off terrain-submit seam): a callback wrapping the
    // pipeline-owned CullHierarchical + draw_chunks_mdi, shared by GBuffer + Shadow
    // so neither needs friend access for terrain submission. Returns counts; the
    // caller folds them into pass stats.
    SubmitTerrainChunksFn make_terrain_submitter();

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
