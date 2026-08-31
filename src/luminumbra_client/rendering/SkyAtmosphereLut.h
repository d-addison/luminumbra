#pragma once

//  Hillaire 2020 precomputed atmospheric-scattering LUTs.
//
// Three LUTs, computed once at startup on the CPU (deterministic, no GPU
// dependency for the math) and uploaded to RGB16F GL textures:
//   - transmittance   256x64: optical-depth transmittance for a view ray
//                                 parameterized by (sun-zenith cos, altitude).
//   - multi-scatter    32x32: isotropic multiple-scattering term (Hillaire's
//                                 1-bounce approximation of the infinite series).
//   - sky-view        192x108: the sky dome radiance integral for the current
//                                 sun direction (RECOMPUTED only when the sun
//                                 moves past a small threshold).
//
// Atmosphere params are PINNED (documented design, Earth-like): Rayleigh
// beta_R = (5.802, 13.558, 33.1)e-6 /m, scale height 8000 m; Mie beta_M =
// 3.996e-6 /m, scale height 1200 m, phase g = 0.76; ground albedo 0.1; planet
// radius 6360 km, atmosphere top 6460 km.
//
// The same transmittance/multi-scatter pair feeds the sun-disc color, the
// sky-view integral (ambient), and the analytic aerial-perspective term in the
// lighting pass, so sun/sky/ambient/fog share ONE transmittance and the low-sun
// palette (pinks/purples/oranges) stays coherent.
//
// This is render-only: it touches nothing in world_hash (documented design).

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

#include "AsyncReadbackRing.h"

namespace Luminumbra::Rendering {

class SkyAtmosphereLut {
public:
    // PINNED LUT dimensions (documented design).
    static constexpr int kTransmittanceWidth = 256; // mu (cos sun-zenith)
    static constexpr int kTransmittanceHeight = 64; // altitude
    static constexpr int kMultiScatterWidth = 32;
    static constexpr int kMultiScatterHeight = 32;
    static constexpr int kSkyViewWidth = 192;
    static constexpr int kSkyViewHeight = 108;

    // PINNED Earth-like atmosphere parameters (SI: metres, /metre).
    static constexpr float kPlanetRadiusM = 6360000.0f;
    static constexpr float kAtmosphereTopM = 6460000.0f;
    static constexpr float kRayleighScaleHeightM = 8000.0f;
    static constexpr float kMieScaleHeightM = 1200.0f;
    static constexpr float kMiePhaseG = 0.76f;
    static constexpr float kGroundAlbedo = 0.1f;
    static constexpr float kMieScatteringPerM = 3.996e-6f;
    // Mie extinction ~= scattering / single-scatter albedo (~0.9).
    static constexpr float kMieAbsorptionPerM = 4.4e-6f - 3.996e-6f;

    static glm::vec3 rayleigh_scattering_per_m() {
        return glm::vec3(5.802e-6f, 13.558e-6f, 33.1e-6f);
    }

    SkyAtmosphereLut() = default;
    ~SkyAtmosphereLut();

    SkyAtmosphereLut(const SkyAtmosphereLut&) = delete;
    SkyAtmosphereLut& operator=(const SkyAtmosphereLut&) = delete;

    // Builds the transmittance + multi-scatter LUTs (sun-independent) and the
    // initial sky-view LUT for sun_dir_world (the toward-sun unit vector).
    // Allocates and uploads the three RGB16F textures. Returns true on success.
    // out_full_precompute_ms receives the wall-clock cost of the whole build
    // (the startup one-shot, budget <= 8.0 ms on release).
    bool initialize(const glm::vec3& sun_dir_world, double* out_full_precompute_ms = nullptr);

    // Recomputes ONLY the sky-view LUT for a new sun direction when it has moved
    // past kSunRefreshCosThreshold since the last refresh. A no-op (returns
    // false) when the sun has not moved enough. out_refresh_ms receives the
    // wall-clock cost when a refresh happened (budget <= 0.2 ms on release).
    bool refresh_sky_view(const glm::vec3& sun_dir_world, double* out_refresh_ms = nullptr);

    void destroy();
    bool ready() const {
        return m_transmittance_tex != 0 && m_multiscatter_tex != 0 && m_skyview_tex != 0;
    }

    // Route refresh_sky_view through a GPU compute shader instead of the CPU
    // march. render.sky_lut_gpu defaults OFF. The GPU path samples the uploaded
    // transmittance/multiscatter textures and writes the sky-view texture; a
    // compile/link failure explicitly falls back to the CPU march.
    void set_gpu_skyview_enabled(bool e) {
        m_use_gpu_skyview = e;
    }

    // Pre-compile the GPU compute programs before the timed initialize so the
    // one-time driver shader-compile cost is not counted against the precompute budget. No-op when
    // the GPU path is off or already warmed. Call once before initialize.
    void prewarm_gpu_compute();

    GLuint transmittance_texture() const {
        return m_transmittance_tex;
    }
    GLuint multiscatter_texture() const {
        return m_multiscatter_tex;
    }
    GLuint sky_view_texture() const {
        return m_skyview_tex;
    }

    // Sun direction the sky-view LUT was last computed for (toward-sun).
    const glm::vec3& sky_view_sun_dir() const {
        return m_skyview_sun_dir;
    }

    // CPU evaluation of the transmittance LUT toward the sun for a ground-level
    // viewer, returning the RGB transmittance the sun-disc color and the lighting
    // pass sky-ambient use (so the engine side can share the SAME transmittance
    // as the shaders). sun_cos_zenith is dot(sun_dir, up).
    glm::vec3 sun_transmittance(float sun_cos_zenith) const;

    // CPU integral of the sky-view LUT over the hemisphere = the sky irradiance
    // ambient color (the lighting pass u_skyAmbientColor sky-scattering term).
    glm::vec3 sky_ambient() const {
        return m_sky_ambient;
    }

    // ---  rendering magnitude getters ---------------------------------
    // The atmosphere drives HUE today; rendering also couples BRIGHTNESS. These name the
    // UN-NORMALIZED magnitudes the coupling needs (update_time_of_day currently normalizes
    // the transmittance and only partially blends the ambient, discarding magnitude). They
    // forward to the existing getters — the data already exists; what is new is the unit
    // contract the coupling task reads against.
    //
    // sun_irradiance_rgb: the raw ground-viewer transmittance toward the sun = dimensionless
    // atmospheric EXTINCTION in [0,1] per channel (NOT absolute irradiance).  multiplies
    // this by a calibrated kSolarRenderScale (chosen so noon matches today) for sun radiance.
    glm::vec3 sun_irradiance_rgb(float sun_cos_zenith) const {
        return sun_transmittance(sun_cos_zenith);
    }
    // sky_unit_irradiance_rgb: the hemisphere sky-view irradiance integral at full magnitude,
    // computed for a UNIT sun (no solar radiance factor in the single-scatter source term).
    //  multiplies by a calibrated kSkyAmbientRenderScale; do NOT raw-upload (under-lights).
    glm::vec3 sky_unit_irradiance_rgb() const {
        return m_sky_ambient;
    }

    // The sun must move past this cosine delta for a sky-view refresh to fire
    // (~0.8 degree of arc). Keeps the per-frame refresh cost amortized.
    static constexpr float kSunRefreshCosThreshold = 0.0001f;

private:
    // CPU LUT pixel buffers (RGB float, row-major).
    std::vector<glm::vec3> m_transmittance_cpu;
    std::vector<glm::vec3> m_multiscatter_cpu;
    std::vector<glm::vec3> m_skyview_cpu;

    GLuint m_transmittance_tex = 0;
    GLuint m_multiscatter_tex = 0;
    GLuint m_skyview_tex = 0;

    glm::vec3 m_skyview_sun_dir{0.0f, 1.0f, 0.0f};
    glm::vec3 m_sky_ambient{0.0f};
    bool m_base_built = false;

    //  GPU-compute sky-view path (default OFF). Lazily-compiled compute program +
    // an SSBO (3 floats/texel) that doubles as the PBO upload source and the ambient readback.
    bool m_use_gpu_skyview = false;
    GLuint m_skyview_compute_prog = 0;
    GLuint m_transmittance_compute_prog = 0; // Initializes LUTs once on the GPU.
    GLuint m_multiscatter_compute_prog = 0;
    GLuint m_skyview_ssbo = 0;
    // the ambient readback rides the asynchronous-readback ring — no
    // blocking GL readback primitives remain on this path.
    Luminumbra::Rendering::AsyncReadbackRing m_skyview_readback;

    void build_transmittance_cpu();
    void build_multiscatter_cpu();
    void build_sky_view_cpu(const glm::vec3& sun_dir_world);
    // Returns true and fills the sky-view texture via compute; false if GPU resources
    // could not be created (caller falls back to build_sky_view_cpu).
    bool build_sky_view_gpu(const glm::vec3& sun_dir_world);
    bool ensure_skyview_compute_resources();
    // build ALL three LUTs (transmittance + multiscatter + sky-view) on the GPU
    // at startup so the one-shot precompute drops under the 8 ms budget. Returns false on any GPU
    // resource failure (caller falls back to the full CPU build).
    bool build_sky_lut_gpu_init(const glm::vec3& sun_dir_world);

    // Sampling helpers over the CPU buffers (used during the multi-scatter and
    // sky-view builds so all three LUTs share one transmittance source).
    glm::vec3 sample_transmittance(float altitude_m, float cos_zenith) const;

    void upload_texture(
        GLuint& tex, int width, int height, const std::vector<glm::vec3>& cpu, const char* label);
    void update_texture(GLuint tex, int width, int height, const std::vector<glm::vec3>& cpu);
};

} // namespace Luminumbra::Rendering
