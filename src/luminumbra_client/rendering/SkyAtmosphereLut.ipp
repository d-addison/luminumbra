#include "SkyAtmosphereLut.h"

#include "passes/PassGlHelpers.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Luminumbra::Rendering {

namespace {

// Ray / sphere intersection: distance to the atmosphere top (or the planet) for
// a ray from origin (planet-centred frame) along dir. Returns -1 if no hit.
float ray_sphere_nearest(const glm::vec3& origin, const glm::vec3& dir, float radius) {
    const float b = glm::dot(origin, dir);
    const float c = glm::dot(origin, origin) - radius * radius;
    if (c > 0.0f && b > 0.0f) return -1.0f;
    const float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    const float sqrt_disc = std::sqrt(disc);
    const float t0 = -b - sqrt_disc;
    const float t1 = -b + sqrt_disc;
    if (t1 < 0.0f) return -1.0f;
    return t0 < 0.0f ? t1 : t0;
}

// Density-ratio of Rayleigh / Mie at a given altitude (metres above ground).
glm::vec2 density_ratio(float altitude_m) {
    const float r = std::exp(-altitude_m / SkyAtmosphereLut::kRayleighScaleHeightM);
    const float m = std::exp(-altitude_m / SkyAtmosphereLut::kMieScaleHeightM);
    return glm::vec2(r, m);
}

float rayleigh_phase(float cos_theta) {
    return 3.0f / (16.0f * glm::pi<float>()) * (1.0f + cos_theta * cos_theta);
}

float mie_phase(float cos_theta, float g) {
    const float g2 = g * g;
    const float num = (1.0f - g2) * (1.0f + cos_theta * cos_theta);
    const float denom = (2.0f + g2) * std::pow(1.0f + g2 - 2.0f * g * cos_theta, 1.5f);
    return 3.0f / (8.0f * glm::pi<float>()) * num / std::max(denom, 1e-6f);
}

constexpr float kEpsilonM = 10.0f; // lift the viewer off the exact ground sphere

} // namespace

SkyAtmosphereLut::~SkyAtmosphereLut() {
    destroy();
}

// --- Transmittance LUT ------------------------------------------------------
// Parameterized: u (x) = cos(sun-zenith) remapped [-1,1]->[0,1];
//                v (y) = altitude [0, top] -> [0,1].
// Each texel integrates the optical depth from the sample altitude toward the
// sun direction out to the atmosphere top, returning exp(-tau) per channel.
void SkyAtmosphereLut::build_transmittance_cpu() {
    const glm::vec3 beta_r = rayleigh_scattering_per_m();
    const glm::vec3 beta_m_ext = glm::vec3(kMieScatteringPerM + kMieAbsorptionPerM);
    const float top = kAtmosphereTopM - kPlanetRadiusM;

    m_transmittance_cpu.assign(static_cast<std::size_t>(kTransmittanceWidth) * kTransmittanceHeight, glm::vec3(0.0f));
    constexpr int kSamples = 40;

    for (int y = 0; y < kTransmittanceHeight; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kTransmittanceHeight);
        const float altitude = v * top;
        const float r = kPlanetRadiusM + altitude;
        for (int x = 0; x < kTransmittanceWidth; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kTransmittanceWidth);
            const float mu = u * 2.0f - 1.0f; // cos(zenith)
            // Viewer at (0, r, 0); sun direction with zenith cosine mu.
            const glm::vec3 origin(0.0f, r, 0.0f);
            const glm::vec3 dir(std::sqrt(std::max(0.0f, 1.0f - mu * mu)), mu, 0.0f);

            const float t_top = ray_sphere_nearest(origin, dir, kAtmosphereTopM);
            if (t_top <= 0.0f) {
                m_transmittance_cpu[static_cast<std::size_t>(y) * kTransmittanceWidth + x] = glm::vec3(1.0f);
                continue;
            }
            const float dt = t_top / static_cast<float>(kSamples);
            glm::vec3 optical_depth(0.0f);
            for (int s = 0; s < kSamples; ++s) {
                const glm::vec3 p = origin + dir * (dt * (static_cast<float>(s) + 0.5f));
                const float alt = glm::length(p) - kPlanetRadiusM;
                const glm::vec2 d = density_ratio(std::max(0.0f, alt));
                optical_depth += (beta_r * d.x + beta_m_ext * d.y) * dt;
            }
            m_transmittance_cpu[static_cast<std::size_t>(y) * kTransmittanceWidth + x] =
                glm::exp(-optical_depth);
        }
    }
}

glm::vec3 SkyAtmosphereLut::sample_transmittance(float altitude_m, float cos_zenith) const {
    const float top = kAtmosphereTopM - kPlanetRadiusM;
    const float v = glm::clamp(altitude_m / top, 0.0f, 1.0f);
    const float u = glm::clamp((cos_zenith + 1.0f) * 0.5f, 0.0f, 1.0f);
    const int x = std::min(kTransmittanceWidth - 1, static_cast<int>(u * kTransmittanceWidth));
    const int y = std::min(kTransmittanceHeight - 1, static_cast<int>(v * kTransmittanceHeight));
    return m_transmittance_cpu[static_cast<std::size_t>(y) * kTransmittanceWidth + x];
}

glm::vec3 SkyAtmosphereLut::sun_transmittance(float sun_cos_zenith) const {
    if (m_transmittance_cpu.empty()) return glm::vec3(1.0f);
    return sample_transmittance(kEpsilonM, sun_cos_zenith);
}

// --- Multiple-scattering LUT ------------------------------------------------
// Hillaire's isotropic 1-bounce approximation. Parameterized over (cos sun
// zenith, altitude). Integrates uniform-sphere directions to estimate the
// second-order in-scattering, folded as an analytic infinite series 1/(1-f).
void SkyAtmosphereLut::build_multiscatter_cpu() {
    const glm::vec3 beta_r = rayleigh_scattering_per_m();
    const glm::vec3 beta_m_sca = glm::vec3(kMieScatteringPerM);
    const glm::vec3 beta_r_ext = beta_r;
    const glm::vec3 beta_m_ext = glm::vec3(kMieScatteringPerM + kMieAbsorptionPerM);
    const float top = kAtmosphereTopM - kPlanetRadiusM;

    m_multiscatter_cpu.assign(static_cast<std::size_t>(kMultiScatterWidth) * kMultiScatterHeight, glm::vec3(0.0f));

    constexpr int kDirSamples = 16;     // sphere directions
    constexpr int kMarchSamples = 20;   // along each direction
    const float uniform_phase = 1.0f / (4.0f * glm::pi<float>());

    for (int y = 0; y < kMultiScatterHeight; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kMultiScatterHeight);
        const float altitude = v * top;
        const float r = kPlanetRadiusM + altitude;
        for (int x = 0; x < kMultiScatterWidth; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kMultiScatterWidth);
            const float mu_sun = u * 2.0f - 1.0f;
            const glm::vec3 sun_dir(std::sqrt(std::max(0.0f, 1.0f - mu_sun * mu_sun)), mu_sun, 0.0f);
            const glm::vec3 origin(0.0f, r, 0.0f);

            glm::vec3 L2nd(0.0f);   // single-scatter gathered over sphere
            glm::vec3 fms(0.0f);    // multi-scatter feedback factor

            for (int d = 0; d < kDirSamples; ++d) {
                // Spread sample directions over the sphere (Fibonacci-ish).
                const float fd = (static_cast<float>(d) + 0.5f) / static_cast<float>(kDirSamples);
                const float cos_t = 1.0f - 2.0f * fd;
                const float sin_t = std::sqrt(std::max(0.0f, 1.0f - cos_t * cos_t));
                const float phi = fd * glm::pi<float>() * (3.0f - std::sqrt(5.0f)) * static_cast<float>(d);
                const glm::vec3 dir(sin_t * std::cos(phi), cos_t, sin_t * std::sin(phi));

                float t_max = ray_sphere_nearest(origin, dir, kAtmosphereTopM);
                const float t_planet = ray_sphere_nearest(origin, dir, kPlanetRadiusM);
                if (t_planet > 0.0f) t_max = std::min(t_max, t_planet);
                if (t_max <= 0.0f) continue;
                const float dt = t_max / static_cast<float>(kMarchSamples);

                glm::vec3 throughput(1.0f);
                for (int s = 0; s < kMarchSamples; ++s) {
                    const glm::vec3 p = origin + dir * (dt * (static_cast<float>(s) + 0.5f));
                    const float alt = glm::length(p) - kPlanetRadiusM;
                    const glm::vec2 dens = density_ratio(std::max(0.0f, alt));
                    const glm::vec3 sigma_s = beta_r * dens.x + beta_m_sca * dens.y;
                    const glm::vec3 sigma_e = beta_r_ext * dens.x + beta_m_ext * dens.y;

                    const glm::vec3 up = glm::normalize(p);
                    const float cos_sun = glm::dot(up, sun_dir);
                    const glm::vec3 t_sun = sample_transmittance(std::max(0.0f, alt), cos_sun);

                    const glm::vec3 step_tr = glm::exp(-sigma_e * dt);
                    // Uniform-phase single scatter contributing to the feedback.
                    const glm::vec3 scattered = sigma_s * uniform_phase * t_sun;
                    const glm::vec3 integ = (scattered - scattered * step_tr) / glm::max(sigma_e, glm::vec3(1e-9f));
                    L2nd += throughput * integ;
                    // Multi-scatter feedback: integral of scattering * throughput.
                    const glm::vec3 integ_f = (sigma_s - sigma_s * step_tr) / glm::max(sigma_e, glm::vec3(1e-9f));
                    fms += throughput * integ_f;
                    throughput *= step_tr;
                }
            }
            const float inv = 1.0f / static_cast<float>(kDirSamples);
            L2nd *= inv * 4.0f * glm::pi<float>() * uniform_phase;
            fms *= inv * 4.0f * glm::pi<float>() * uniform_phase;
            // Analytic infinite series sum: L2 / (1 - fms).
            const glm::vec3 denom = glm::max(glm::vec3(1.0f) - fms, glm::vec3(1e-3f));
            m_multiscatter_cpu[static_cast<std::size_t>(y) * kMultiScatterWidth + x] = L2nd / denom;
        }
    }
}

// --- Sky-view LUT -----------------------------------------------------------
// Latitude-longitude sky dome for the current sun direction. x = azimuth around
// the sun, y = view zenith. Each texel ray-marches single + multi scatter.
// Sun-dependent: recomputed on sun motion.
void SkyAtmosphereLut::build_sky_view_cpu(const glm::vec3& sun_dir_world) {
    const glm::vec3 beta_r = rayleigh_scattering_per_m();
    const glm::vec3 beta_m_sca = glm::vec3(kMieScatteringPerM);
    const glm::vec3 beta_r_ext = beta_r;
    const glm::vec3 beta_m_ext = glm::vec3(kMieScatteringPerM + kMieAbsorptionPerM);

    m_skyview_cpu.assign(static_cast<std::size_t>(kSkyViewWidth) * kSkyViewHeight, glm::vec3(0.0f));

    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 origin(0.0f, kPlanetRadiusM + kEpsilonM, 0.0f);
    const float sun_cos_zenith = glm::clamp(glm::dot(sun_dir_world, up), -1.0f, 1.0f);

    constexpr int kMarchSamples = 30;
    glm::vec3 ambient_accum(0.0f);
    float ambient_weight = 0.0f;

    for (int y = 0; y < kSkyViewHeight; ++y) {
        // v in [0,1] -> view zenith angle [0 (up), pi (down)].
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kSkyViewHeight);
        const float view_zenith = v * glm::pi<float>();
        const float cos_view = std::cos(view_zenith);
        const float sin_view = std::sin(view_zenith);
        for (int x = 0; x < kSkyViewWidth; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kSkyViewWidth);
            const float azimuth = u * 2.0f * glm::pi<float>();
            // Build the view direction in a frame where the sun azimuth is 0.
            const glm::vec3 view_dir(sin_view * std::cos(azimuth), cos_view, sin_view * std::sin(azimuth));

            float t_max = ray_sphere_nearest(origin, view_dir, kAtmosphereTopM);
            const float t_planet = ray_sphere_nearest(origin, view_dir, kPlanetRadiusM);
            if (t_planet > 0.0f) t_max = std::min(t_max, t_planet);
            if (t_max <= 0.0f) {
                continue;
            }
            const float dt = t_max / static_cast<float>(kMarchSamples);

            const float cos_theta = glm::dot(view_dir, sun_dir_world);
            const float phase_r = rayleigh_phase(cos_theta);
            const float phase_m = mie_phase(cos_theta, kMiePhaseG);

            glm::vec3 L(0.0f);
            glm::vec3 throughput(1.0f);
            for (int s = 0; s < kMarchSamples; ++s) {
                const glm::vec3 p = origin + view_dir * (dt * (static_cast<float>(s) + 0.5f));
                const float alt = glm::length(p) - kPlanetRadiusM;
                const glm::vec2 dens = density_ratio(std::max(0.0f, alt));
                const glm::vec3 sigma_s_r = beta_r * dens.x;
                const glm::vec3 sigma_s_m = beta_m_sca * dens.y;
                const glm::vec3 sigma_e = beta_r_ext * dens.x + beta_m_ext * dens.y;

                const glm::vec3 p_up = glm::normalize(p);
                const float cos_sun = glm::dot(p_up, sun_dir_world);
                const glm::vec3 t_sun = sample_transmittance(std::max(0.0f, alt), cos_sun);

                // Single scatter (phase-weighted) + isotropic multi-scatter.
                const glm::vec3 single = (sigma_s_r * phase_r + sigma_s_m * phase_m) * t_sun;
                const float ms_u = glm::clamp((cos_sun + 1.0f) * 0.5f, 0.0f, 1.0f);
                const float ms_v = glm::clamp(std::max(0.0f, alt) / (kAtmosphereTopM - kPlanetRadiusM), 0.0f, 1.0f);
                const int msx = std::min(kMultiScatterWidth - 1, static_cast<int>(ms_u * kMultiScatterWidth));
                const int msy = std::min(kMultiScatterHeight - 1, static_cast<int>(ms_v * kMultiScatterHeight));
                const glm::vec3 ms = m_multiscatter_cpu.empty()
                    ? glm::vec3(0.0f)
                    : m_multiscatter_cpu[static_cast<std::size_t>(msy) * kMultiScatterWidth + msx];
                const glm::vec3 multi = (sigma_s_r + sigma_s_m) * ms;

                const glm::vec3 step_tr = glm::exp(-sigma_e * dt);
                const glm::vec3 in_scatter = single + multi;
                const glm::vec3 integ = (in_scatter - in_scatter * step_tr) / glm::max(sigma_e, glm::vec3(1e-9f));
                L += throughput * integ;
                throughput *= step_tr;
            }

            m_skyview_cpu[static_cast<std::size_t>(y) * kSkyViewWidth + x] = L;

            // Hemisphere ambient: average upper-hemisphere radiance weighted by
            // cosine of the view zenith (irradiance integral).
            if (cos_view > 0.0f) {
                ambient_accum += L * cos_view * sin_view;
                ambient_weight += cos_view * sin_view;
            }
        }
    }

    m_skyview_sun_dir = sun_dir_world;
    m_sky_ambient = ambient_weight > 0.0f ? ambient_accum / ambient_weight * glm::pi<float>() : glm::vec3(0.0f);
    (void)sun_cos_zenith;
}

// --- GL upload --------------------------------------------------------------

void SkyAtmosphereLut::upload_texture(GLuint& tex, int width, int height,
                                      const std::vector<glm::vec3>& cpu, const char* label) {
    glGenTextures(1, &tex);
    PassGl::label_gl_object(GL_TEXTURE, tex, label);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, cpu.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void SkyAtmosphereLut::update_texture(GLuint tex, int width, int height, const std::vector<glm::vec3>& cpu) {
    if (tex == 0) return;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_FLOAT, cpu.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool SkyAtmosphereLut::initialize(const glm::vec3& sun_dir_world, double* out_full_precompute_ms) {
    const auto t0 = std::chrono::steady_clock::now();

    build_transmittance_cpu();
    build_multiscatter_cpu();
    build_sky_view_cpu(glm::normalize(sun_dir_world));

    upload_texture(m_transmittance_tex, kTransmittanceWidth, kTransmittanceHeight, m_transmittance_cpu, "sky.lut.transmittance");
    upload_texture(m_multiscatter_tex, kMultiScatterWidth, kMultiScatterHeight, m_multiscatter_cpu, "sky.lut.multiscatter");
    upload_texture(m_skyview_tex, kSkyViewWidth, kSkyViewHeight, m_skyview_cpu, "sky.lut.skyview");

    m_base_built = true;
    const auto t1 = std::chrono::steady_clock::now();
    if (out_full_precompute_ms) {
        *out_full_precompute_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    return ready();
}

bool SkyAtmosphereLut::refresh_sky_view(const glm::vec3& sun_dir_world, double* out_refresh_ms) {
    if (!m_base_built) return false;
    const glm::vec3 sun = glm::normalize(sun_dir_world);
    // Only refresh when the sun has moved past the threshold (cosine of arc).
    if (glm::dot(sun, m_skyview_sun_dir) >= 1.0f - kSunRefreshCosThreshold) {
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    build_sky_view_cpu(sun);
    update_texture(m_skyview_tex, kSkyViewWidth, kSkyViewHeight, m_skyview_cpu);
    const auto t1 = std::chrono::steady_clock::now();
    if (out_refresh_ms) {
        *out_refresh_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    return true;
}

void SkyAtmosphereLut::destroy() {
    if (m_transmittance_tex) { glDeleteTextures(1, &m_transmittance_tex); m_transmittance_tex = 0; }
    if (m_multiscatter_tex) { glDeleteTextures(1, &m_multiscatter_tex); m_multiscatter_tex = 0; }
    if (m_skyview_tex) { glDeleteTextures(1, &m_skyview_tex); m_skyview_tex = 0; }
    m_base_built = false;
}

} // namespace Luminumbra::Rendering
