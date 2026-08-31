#pragma once

// Far-LOD region scheduler + render residency.
//
// Extends the visible horizon past the live chunk ring by streaming far-LOD
// region tiles (FarLodStore, ) and drawing one merged heightfield mesh
// per (tier, region) inside the G-buffer pass AFTER the live chunks:
// - Ring-diff wanted set: every region whose footprint comes within the F2
//   outer range (1536 m) of the camera is wanted; tier F1 (4 m) inside 768 m,
//   F2 (8 m) beyond. Regions fully covered by the live chunk ring are skipped
//   (live wins); partially-covered regions ARE drawn underneath the live
//   terrain - a small depth bias plus polygon offset keeps the live geometry
//   in front where the two surfaces coincide, and the far surface fills any
//   live-side gaps instead of opening a sky/void band at the boundary (the
//   Distant-Horizons failure mode the FarLodHorizon seam gate watches).
// - Tiles build on the JobSystem Normal lane (pristine tiles are pure
//   functions of (seed, params); edited tiles load from the LMR1 store when a
//   save directory is attached); the main thread uploads finished meshes.
// - Residency: 128 MB byte budget with least-recently-used eviction; regions
//   leaving the wanted set free their GL buffers immediately. (Raised from 64 MB
//   for: hydraulic relief adds far-LOD geometry to the gameplay presets;
//   128 MB is trivial for the 16 GB RTX 5070 Ti target — owner "up the caps".)

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/world/FarLodStore.h"

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
struct FarLodSdfSnapshot;
} // namespace Luminumbra::Systems

namespace Luminumbra::Rendering {

class Shader;

struct FarLodWorkerBuildOutcome {
    bool ok = false;
    bool changed = false;
    std::string error;
    World::FarLodTile tile;
    World::FarLodRegionMesh mesh;
};

// Pure CPU worker seam used by the asynchronous scheduler and focused tests.
// All streamed voxel bytes arrive through the immutable owner-thread snapshot.
FarLodWorkerBuildOutcome BuildFarLodWorkerTile(const Systems::SHIELD_WorldSystem& world,
                                               const Systems::FarLodSdfSnapshot& snapshot,
                                               World::FarLodTier tier,
                                               int rx,
                                               int rz,
                                               const std::filesystem::path& save_dir);

class FarLodSystem {
public:
    // Pinned numbers (the deterministic runtime contract section 4).
    static constexpr float kF1OuterRangeMeters = 768.0f;
    // Extended far horizon (playtest: no visible render edge from mountaintops).
    // F2 streams to ~3000 m; the camera FAR_PLANE (3200 m) clears it + margin.
    static constexpr float kF2OuterRangeMeters = 3000.0f;
    static constexpr std::size_t kResidentBudgetBytes = 384ull * 1024ull * 1024ull;
    // Live chunk ring horizontal reach (RENDER_DISTANCE chunks): regions
    // fully inside this disc are owned by live chunks and never drawn far.
    static constexpr float kLiveRingRadiusMeters = 512.0f;
    // Fragment-level live/far handoff: far-mesh fragments closer than this
    // (the guaranteed-renderable LOD0 ring minus one chunk of overlap) are
    // discarded in the G-buffer shader so the under-terrain far fill cannot
    // show through live LOD seam cracks at close range, while everything
    // beyond the live ring keeps far coverage (no gap band: the overlap chunk
    // is drawn by BOTH paths and depth resolves it).
    static constexpr float kFarClipInnerRadiusMeters = 176.0f;
    // far geometry is clipped at the geometry
    // level outside this radius. The camera far plane (Camera.h FAR_PLANE) is
    // 1000 m but far regions stream to 1536 m; far triangles near the far-plane/
    // frustum-edge corner rasterized as the horizon sky-sliver (a tall thin
    // terrain streak crossing into the sky). Clipping just inside removes them
    // with no visible loss - nothing past the 1000 m far plane was drawable.
    static constexpr float kFarClipOuterRadiusMeters = 3050.0f;
    // Far meshes sit slightly below the live surface so live geometry always
    // wins where the two coincide (quantization can lift far samples at most
    // 1/32 m above the analytic surface).
    static constexpr float kFarDepthBiasMeters = 0.125f;
    // a flat far-water sheet is drawn at the global
    // waterline over far regions whose tiles carry water-present samples (river
    // channels + seabeds beyond the live ring). The sheet uses a dedicated
    // material id so the G-buffer pass tints it with the deep-water albedo
    // family WITHOUT the live water.frag pipeline (no reflections/caustics far
    // out). It sits a hair below the waterline so coincident far terrain at the
    // shoreline keeps winning the depth test, matching the terrain depth bias.
    static constexpr u32 kFarWaterMaterialId = 200u;
    static constexpr float kFarWaterDepthBiasMeters = 0.0625f;
    // Per-frame integration caps keep upload hitches bounded. NOTE (  implementation note):
    // raising the upload cap to 16 was tried to force the dense mountains far-ring to converge
    // before the debug horizon-gate capture, but (a) it did NOT fix debug (the real limit there
    // is worker build-COMPLETION throughput, ~8/frame, not the upload cap), (b) the ring already
    // converges at 6 in the RELEASE lane, and (c) 16 main-thread far-tile uploads/frame caused a
    // visible hitch when flying through fill-in. So it stays at 6 — the deliberate smoothness
    // value. Mountains residency is a release-lane property; the gate fix + diagnostics stand.
    static constexpr std::size_t kMaxBuildDispatchesPerFrame = 8;
    static constexpr std::size_t kMaxUploadsPerFrame = 6;

    struct FrameStats {
        bool enabled = false;
        std::size_t regions_wanted = 0;
        std::size_t regions_resident = 0;
        // Wanted regions with no resident mesh yet (the FarLodHorizon gate
        // requires this to settle to 0 out to 1536 m).
        std::size_t regions_missing = 0;
        std::size_t regions_building = 0;
        std::size_t resident_bytes = 0; // tile payload + GPU mesh bytes
        std::size_t region_draws = 0;
        std::size_t indices_drawn = 0;
        // far water sheet draw/index counts (subset of
        // the far draws; surfaced so the horizon gate can assert continuity).
        std::size_t water_sheet_draws = 0;
        std::size_t water_sheet_indices = 0;
        std::size_t builds_completed_total = 0;
        std::size_t evictions_total = 0;
        //   diagnostics (per-frame, reset each update): distinguish a
        // build-throttle/evict-thrash bottleneck from BuildPristineFarLodTile empty-mesh
        // failures when the mountains-preset horizon gate reports persistent missing regions.
        std::size_t builds_dispatched =
            0; // build jobs dispatched this frame (<= kMaxBuildDispatchesPerFrame)
        std::size_t builds_integrated_ok = 0; // completed builds integrated to resident this frame
        std::size_t builds_integrated_failed =
            0; // completed builds rejected this frame (empty mesh / epoch mismatch)
        std::size_t stale_results_rejected = 0;
        std::size_t authority_build_failures = 0;
        std::size_t builds_failed_total =
            0; // cumulative rejected builds (nonzero => empty-mesh failures, hypothesis b)
        std::size_t evictions_this_frame = 0; // regions evicted this frame (wanted-set + budget)
        std::size_t pending_depth = 0;        // build jobs in flight at end of update()
    };

    FarLodSystem();
    ~FarLodSystem();

    void attach_job_system(JobSystem* job_system) {
        m_job_system = job_system;
    }
    // Optional persistence root: when set, edited (authoritative) far tiles
    // load from the LMR1 store before falling back to pristine generation.
    void set_save_dir(std::filesystem::path save_dir) {
        m_save_dir = std::move(save_dir);
    }
    void set_enabled(bool enabled) {
        m_enabled = enabled;
    }
    bool enabled() const {
        return m_enabled;
    }

    // Worldgen-preview far-field anchor (render-only). The preview is an external
    // orbit camera looking at a FIXED diorama centre over a small bounded near
    // slice. In normal (first-person) mode the wanted region set, the camera-
    // region cull, and the inner fragment discard are all CAMERA-relative — for
    // an orbiting camera that produces continuous tile churn and a moving void
    // disc. set_preview_anchor pins all of that to the diorama centre instead:
    //   * update streams the wanted set around `center` (a FIXED tile set, no
    //     orbit churn) while eviction stays relative to the same anchor;
    //   * draw_gbuffer drops the camera-region skip + the camera-relative
    //     radial clips and instead discards far fragments whose world XZ is
    //     within `inner_radius_m` of the centre (the live slice owns that space),
    //     so the coarse far mesh cannot poke through the fine live slice.
    // The crash-safety drain (prepare_world_swap before the bound world is freed)
    // is unchanged. clear_preview_anchor returns to first-person streaming.
    void set_preview_anchor(const glm::vec3& center, float inner_radius_m) {
        m_preview_mode = true;
        m_preview_anchor = center;
        m_preview_inner_radius = inner_radius_m;
    }
    void clear_preview_anchor() {
        m_preview_mode = false;
    }
    bool preview_anchored() const {
        return m_preview_mode;
    }

    // Waits for in-flight tile builds (they sample the world system) and
    // drops the world binding. MUST run before the bound world is destroyed.
    void prepare_world_swap();
    // prepare_world_swap + frees every GL resource.
    void shutdown();

    // Ring-diff scheduling + finished-build integration + eviction. Called
    // once per rendered frame from RenderPipeline::render_frame.
    void update(const Systems::SHIELD_WorldSystem& world_system, const glm::vec3& camera_position);

    // Draws resident far-region meshes with the ALREADY BOUND terrain
    // geometry shader (VoxelVertex layout is identical). Region-AABB frustum
    // culling; depth-biased so overlapping live chunks win. Returns draw and
    // index counts for RenderPassFrameStats.
    void draw_gbuffer(Shader& geometry_shader,
                      const glm::mat4& view,
                      const glm::vec4 frustum_planes[6],
                      std::size_t& draws_out,
                      std::size_t& indices_out);

    const FrameStats& stats() const {
        return m_stats;
    }
    std::size_t resident_gpu_buffer_count() const {
        return m_residents.size() * 3u;
    }
    std::size_t resident_vertex_array_count() const {
        return m_residents.size();
    }

private:
    struct ResidentRegion {
        World::FarLodTier tier = World::FarLodTier::F1;
        int rx = 0;
        int rz = 0;
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        u32 element_count = 0;
        // optional flat water sheet for this region
        // (present only when the tile carries water-flagged samples).
        GLuint water_vao = 0;
        GLuint water_vbo = 0;
        GLuint water_ebo = 0;
        u32 water_element_count = 0;
        glm::vec3 aabb_min{0.0f};
        glm::vec3 aabb_max{0.0f};
        std::size_t resident_bytes = 0;
        u64 last_wanted_frame = 0;
        u64 region_authority_revision = 0;
        bool persistence_pending = false;
    };

    struct BuildResult {
        u64 epoch = 0;
        // Immutable owner-thread capture used by this build.  Keeping the
        // shared value through completion lets integration reject results from
        // a changed SDF authority without ever exposing mutable Chunk data to a
        // worker.
        std::shared_ptr<const Systems::FarLodSdfSnapshot> sdf_snapshot;
        u64 capture_epoch = 0;
        u64 params_hash = 0;
        u64 authority_revision = 0;
        u64 region_authority_revision = 0;
        bool authority_build_failed = false;
        bool tile_changed = false;
        bool persistence_allowed = true;
        std::filesystem::path save_dir;
        World::FarLodTile tile;
        World::FarLodTier tier = World::FarLodTier::F1;
        int rx = 0;
        int rz = 0;
        float min_height = 0.0f;
        float max_height = 0.0f;
        std::size_t tile_bytes = 0;
        World::FarLodRegionMesh mesh;
        // flat water sheet geometry (VoxelVertex
        // layout, material id kFarWaterMaterialId, normals up) covering the
        // water-flagged samples at the global waterline. Empty when the tile
        // is fully dry.
        World::FarLodRegionMesh water_mesh;
    };

    struct SharedBuildState {
        std::mutex mutex;
        std::vector<BuildResult> completed;
    };

    static u64 region_key(int rx, int rz);
    void bind_world(const Systems::SHIELD_WorldSystem& world_system);
    void wait_for_builds();
    void release_region(ResidentRegion& region);
    void integrate_completed_builds();
    std::size_t total_resident_bytes() const;

    JobSystem* m_job_system = nullptr;
    bool m_enabled = true;
    std::filesystem::path m_save_dir;

    const Systems::SHIELD_WorldSystem* m_world = nullptr;
    // camera position from the last update,
    // used by draw_gbuffer to skip the far region the camera sits inside (its
    // near triangles straddle the camera and rasterize into the horizon
    // sky-sliver; the live ring + neighbor regions cover its footprint).
    glm::vec3 m_last_camera_position{0.0f};
    // Worldgen-preview far-field anchor (see set_preview_anchor). When
    // m_preview_mode is set, streaming + the inner discard pin to m_preview_anchor
    // (the fixed diorama centre) instead of the orbiting camera.
    bool m_preview_mode = false;
    glm::vec3 m_preview_anchor{0.0f};
    float m_preview_inner_radius = 0.0f;
    u64 m_params_hash = 0;
    // Build-result generation: results from a previous world binding are
    // discarded on integration.
    u64 m_epoch = 0;
    u64 m_frame = 0;

    std::shared_ptr<SharedBuildState> m_shared = std::make_shared<SharedBuildState>();
    std::vector<JobHandle> m_inflight_handles;
    std::unordered_map<u64, World::FarLodTier> m_pending;
    std::unordered_map<u64, ResidentRegion> m_residents;

    FrameStats m_stats;
};

} // namespace Luminumbra::Rendering
