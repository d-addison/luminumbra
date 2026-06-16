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

#include <array>
#include <cstdint>
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
    bool ready() const { return m_ready; }

    // Rebuild the camera-centered heightfield + GPU max-mip when the camera
    // crosses into a new F1 region (or the bound world/params change). Cheap
    // no-op otherwise. Must run on the GL thread (it uploads + dispatches).
    void update(const Systems::SHIELD_WorldSystem& world, const glm::vec3& camera_pos);

    // Fullscreen raymarch into the CURRENTLY BOUND G-buffer FBO (the caller binds
    // it, sets the 4 draw buffers, and enables depth-test GL_LESS with writes on).
    // No-op until the first successful update() populated the heightfield.
    void render(const glm::mat4& view, const glm::mat4& view_proj,
                const glm::mat4& inv_view_proj, const glm::mat3& normal_view,
                const glm::vec3& eye, const glm::vec2& viewport, float t_max);

    bool has_field() const { return m_has_field; }

private:
    void rebuild_field(const Systems::SHIELD_WorldSystem& world, int center_rx, int center_rz);

    bool m_ready = false;
    bool m_has_field = false;

    GLuint m_raymarch_prog = 0;     // vert + frag (fullscreen -> G-buffer)
    GLuint m_maxmip_l0_prog = 0;    // base samples -> level 0 (compute)
    GLuint m_maxmip_reduce_prog = 0;// level L -> level L+1 (compute)
    GLuint m_vao = 0;               // empty VAO for the fullscreen triangle

    GLuint m_base_ssbo = 0;         // heightfield base (n*n floats)
    GLuint m_maxmip_ssbo = 0;       // flattened max-mip pyramid

    // Cached field descriptor (set by rebuild_field).
    int m_center_rx = 0;
    int m_center_rz = 0;
    std::uint64_t m_params_hash = 0;
    bool m_have_cache_key = false;

    int m_n = 0;                    // base samples per side
    float m_step = 0.0f;           // metres between samples
    float m_ox = 0.0f, m_oz = 0.0f; // world origin of sample (0,0)
    int m_levels = 0;
    std::array<std::int32_t, kMaxMipLevels> m_mip_offset{};
    std::array<std::int32_t, kMaxMipLevels> m_mip_dim{};
};

}  // namespace Luminumbra::Rendering
