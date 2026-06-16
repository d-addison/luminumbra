#pragma once

// SHIELD-RT far-field render pass (T-I6-A3b inc2c).
//
// Productionizes the validated heightfield max-mip raymarch as a fullscreen
// deferred pass. Owns a camera-centered heightfield SSBO (assembled from
// FarLodStore F1 tiles around the camera, rebuilt on region-crossing) and a
// GPU-built max-mip pyramid (the conservative ray-above acceleration structure;
// glGenerateMipmap is a box filter, unusable). The fragment pass reconstructs a
// per-pixel world ray from the inverse view-projection, marches the heightfield
// (overshoot-free hierarchical DDA), and on a hit writes the deferred G-buffer
// (view-space position, octahedral view-space normal + material, albedo/rough,
// metallic/AO) + gl_FragDepth; on a miss it discards. Depth-tested against the
// existing G-buffer so it fills only the far/sky pixels the live + far-mesh
// geometry did not (v1 augments the mesh; replacing it is a later refinement).
//
// Gated by RenderPipeline's compile-time kEnableExperimentalFarFieldGpuRaymarching
// + the --enable-far-field-gpu-raymarch runtime flag. Every increment proven in
// isolation: ShieldRtFarFieldParityGpu (tracer hits ground truth),
// ShieldRtFarFieldGbufferGpu (G-buffer encoding), ShieldRtFarFieldMaxMipGpu (the
// GPU max-reduction == CPU reference).

#include <glad/glad.h>
#include <glm/glm.hpp>

#include "luminumbra_common/core/JobSystem.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Luminumbra::Systems { class SHIELD_WorldSystem; }

namespace Luminumbra::Rendering {

class ShieldRtFarFieldPass {
public:
    // Uniform-array bound for the flattened max-mip layout (chain depth ~10).
    static constexpr int kMaxMipLevels = 24;
    // Camera-centered F1 block radius in 512 m regions (covers the F2 outer range).
    static constexpr int kRegionRadius = 3;

    ShieldRtFarFieldPass();
    ~ShieldRtFarFieldPass();

    ShieldRtFarFieldPass(const ShieldRtFarFieldPass&) = delete;
    ShieldRtFarFieldPass& operator=(const ShieldRtFarFieldPass&) = delete;

    // Compile shaders + allocate the empty VAO. Returns false (and stays !ready)
    // on shader failure. Safe to call once after a GL context exists.
    bool init();
    void shutdown();
    // Wait out any in-flight async heightfield build. MUST be called before the bound
    // world is cleared/destroyed (the build job reads the world by pointer).
    void drain();
    bool ready() const { return m_ready; }

    // inc2c-SCALE step 3: attach the JobSystem so the heightfield assembly (49
    // BuildPristineFarLodTile calls) runs on a worker instead of hitching the GL
    // thread on every region-crossing. When null, update() falls back to the
    // synchronous in-line build (the validated inc2c behaviour).
    void attach_job_system(JobSystem* job_system) { m_job_system = job_system; }

    // Keep the camera-centered heightfield + GPU max-mip current. On a region/params
    // change it dispatches an ASYNC CPU assembly (the prior field keeps rendering
    // until the new one is ready), and integrates a finished build (GL upload + mip
    // reduction) on the GL thread. Cheap no-op when the field already matches. Must
    // run on the GL thread.
    void update(const Systems::SHIELD_WorldSystem& world, const glm::vec3& camera_pos);

    // inc2c-SCALE step 1: blit the source FBO's depth into a pass-owned copy
    // texture so the raymarch frag can sample it WITHOUT a feedback loop on the
    // depth attachment it writes (gl_FragDepth). Call BEFORE render() each frame;
    // it lazily (re)allocates the copy texture/FBO to the viewport. Enables the
    // far-pixel early-out (skip rays where opaque geometry already won the pixel).
    void capture_scene_depth(GLuint src_fbo, int width, int height);

    // Fullscreen raymarch into the CURRENTLY BOUND G-buffer FBO (the caller binds
    // it, sets the 4 draw buffers, and enables depth-test GL_LESS with writes on).
    // No-op until the first successful update() populated the heightfield.
    void render(const glm::mat4& view, const glm::mat4& view_proj,
                const glm::mat4& inv_view_proj, const glm::mat3& normal_view,
                const glm::vec3& eye, const glm::vec2& viewport, float t_max);

    bool has_field() const { return m_has_field; }

private:
    // Pure-CPU heightfield assembly result (no GL, no `this`) — safe to produce on
    // a worker thread and hand back to the GL thread to integrate.
    struct FieldData {
        std::vector<float> heights;
        int n = 0;
        float step = 0.0f, ox = 0.0f, oz = 0.0f;
        int center_rx = 0, center_rz = 0;
        std::uint64_t params_hash = 0;
    };
    // Cross-thread hand-off: the worker fills `data` + sets `ready` under `mutex`;
    // the GL thread polls and moves it out. shared_ptr so an in-flight job outlives
    // the pass if it is destroyed mid-build.
    struct FieldBuild {
        std::mutex mutex;
        bool ready = false;
        FieldData data;
    };

    // Assemble the 7x7-region heightfield (CPU only). Static so the worker lambda
    // cannot touch GL state or `this`.
    static FieldData assemble_field(const Systems::SHIELD_WorldSystem& world,
                                    int center_rx, int center_rz,
                                    std::uint64_t params_hash);
    // Upload the assembled field + build the GPU max-mip (GL thread only). Sets the
    // resident field descriptor + cache key to the integrated region.
    void integrate_field(FieldData& fd);

    bool m_ready = false;
    bool m_has_field = false;

    GLuint m_raymarch_prog = 0;     // vert + frag (fullscreen -> G-buffer)
    GLuint m_maxmip_l0_prog = 0;    // base samples -> level 0 (compute)
    GLuint m_maxmip_reduce_prog = 0;// level L -> level L+1 (compute)
    GLuint m_vao = 0;               // empty VAO for the fullscreen triangle

    GLuint m_base_ssbo = 0;         // heightfield base (n*n floats)
    GLuint m_maxmip_ssbo = 0;       // flattened max-mip pyramid

    // inc2c-SCALE step 1: scene-depth copy for the far-pixel early-out.
    GLuint m_scene_depth_tex = 0;
    GLuint m_scene_depth_fbo = 0;
    int m_depth_w = 0;
    int m_depth_h = 0;
    bool m_have_depth_copy = false;

    // Cached field descriptor (set by integrate_field — the resident field's region).
    int m_center_rx = 0;
    int m_center_rz = 0;
    std::uint64_t m_params_hash = 0;
    bool m_have_cache_key = false;

    // inc2c-SCALE step 3: async-rebuild state (all GL-thread-only except m_shared,
    // which is guarded by its own mutex).
    JobSystem* m_job_system = nullptr;
    std::shared_ptr<FieldBuild> m_shared;  // in-flight build hand-off (null when idle)
    JobHandle m_inflight_handle;
    bool m_building = false;
    int m_inflight_rx = 0;
    int m_inflight_rz = 0;
    std::uint64_t m_inflight_params = 0;

    int m_n = 0;                    // base samples per side
    float m_step = 0.0f;           // metres between samples
    float m_ox = 0.0f, m_oz = 0.0f; // world origin of sample (0,0)
    int m_levels = 0;
    std::array<std::int32_t, kMaxMipLevels> m_mip_offset{};
    std::array<std::int32_t, kMaxMipLevels> m_mip_dim{};
};

}  // namespace Luminumbra::Rendering
