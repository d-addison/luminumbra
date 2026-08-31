#include "SkyAtmosphereLut.h"

#include "passes/PassGlHelpers.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

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
    return t0 < 0.0f ? t1: t0;
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

//  GPU-compute port of build_sky_view_cpu. One invocation per sky-view texel
// (192x108), marching kMarchSamples=30 single+multi scatter, sampling the already-uploaded
// transmittance/multiscatter textures with the SAME nearest indexing as the CPU helpers. Writes
// L (vec3) into an std430 float[] SSBO (3 per texel) so it can be uploaded to the RGB16F sky-view
// texture via a PIXEL_UNPACK_BUFFER (RGB16F is not image-store-able) and read back for the CPU
// ambient reduction. The math mirrors the CPU reference line-for-line.
const char* kSkyViewComputeSrc = R"GLSL(
#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) writeonly buffer SkyViewOut { float vals[]; };
uniform sampler2D u_transmittance;
uniform sampler2D u_multiscatter;
uniform vec3 u_sunDir; // toward-sun, world

const float PI = 3.14159265358979323846;
const float PLANET_R = 6360000.0;
const float ATMO_TOP = 6460000.0;
const float RAYLEIGH_H = 8000.0;
const float MIE_H = 1200.0;
const float MIE_G = 0.76;
const float MIE_SCA = 3.996e-6;
const float MIE_ABS = 4.4e-6 - 3.996e-6;
const vec3  BETA_R = vec3(5.802e-6, 13.558e-6, 33.1e-6);
const float EPS = 10.0;
const int SKYW = 192; const int SKYH = 108;
const int TW = 256;   const int TH = 64;
const int MSW = 32;   const int MSH = 32;
const int MARCH = 30;

float raySphere(vec3 o, vec3 d, float r) {
  float b = dot(o, d);
  float c = dot(o, o) - r * r;
  if (c > 0.0 && b > 0.0) return -1.0;
  float disc = b * b - c;
  if (disc < 0.0) return -1.0;
  float sq = sqrt(disc);
  float t0 = -b - sq; float t1 = -b + sq;
  if (t1 < 0.0) return -1.0;
  return t0 < 0.0 ? t1: t0;
}
vec2 densityRatio(float alt) { return vec2(exp(-alt / RAYLEIGH_H), exp(-alt / MIE_H)); }
float rayleighPhase(float ct) { return 3.0 / (16.0 * PI) * (1.0 + ct * ct); }
float miePhase(float ct, float g) {
  float g2 = g * g;
  float num = (1.0 - g2) * (1.0 + ct * ct);
  float den = (2.0 + g2) * pow(1.0 + g2 - 2.0 * g * ct, 1.5);
  return 3.0 / (8.0 * PI) * num / max(den, 1e-6);
}
vec3 sampleTransmittance(float alt, float cz) {
  float top = ATMO_TOP - PLANET_R;
  float v = clamp(alt / top, 0.0, 1.0);
  float u = clamp((cz + 1.0) * 0.5, 0.0, 1.0);
  int x = min(TW - 1, int(u * float(TW)));
  int y = min(TH - 1, int(v * float(TH)));
  return texelFetch(u_transmittance, ivec2(x, y), 0).rgb;
}
void main() {
  ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
  if (gid.x >= SKYW || gid.y >= SKYH) return;
  vec3 up = vec3(0.0, 1.0, 0.0);
  vec3 origin = vec3(0.0, PLANET_R + EPS, 0.0);
  vec3 sunHoriz = vec3(u_sunDir.x, 0.0, u_sunDir.z);
  float shl = length(sunHoriz);
  vec3 forward = shl > 1e-4 ? sunHoriz / shl: vec3(0.0, 0.0, 1.0);
  vec3 right = vec3(-forward.z, 0.0, forward.x);
  vec3 betaR = BETA_R;
  vec3 betaMsca = vec3(MIE_SCA);
  vec3 betaRext = BETA_R;
  vec3 betaMext = vec3(MIE_SCA + MIE_ABS);

  float vv = (float(gid.y) + 0.5) / float(SKYH);
  float viewZen = vv * PI;
  float cosView = cos(viewZen);
  float sinView = sin(viewZen);
  float uu = (float(gid.x) + 0.5) / float(SKYW);
  float az = uu * 2.0 * PI;
  vec3 viewDir = up * cosView + (forward * cos(az) + right * sin(az)) * sinView;

  float tMax = raySphere(origin, viewDir, ATMO_TOP);
  float tPlanet = raySphere(origin, viewDir, PLANET_R);
  if (tPlanet > 0.0) tMax = min(tMax, tPlanet);
  vec3 L = vec3(0.0);
  if (tMax > 0.0) {
    float dt = tMax / float(MARCH);
    float cosTheta = dot(viewDir, u_sunDir);
    float phaseR = rayleighPhase(cosTheta);
    float phaseM = miePhase(cosTheta, MIE_G);
    vec3 throughput = vec3(1.0);
    for (int s = 0; s < MARCH; ++s) {
      vec3 p = origin + viewDir * (dt * (float(s) + 0.5));
      float alt = max(0.0, length(p) - PLANET_R);
      vec2 dens = densityRatio(alt);
      vec3 sigmaSr = betaR * dens.x;
      vec3 sigmaSm = betaMsca * dens.y;
      vec3 sigmaE = betaRext * dens.x + betaMext * dens.y;
      vec3 pUp = normalize(p);
      float cosSun = dot(pUp, u_sunDir);
      vec3 tSun = sampleTransmittance(alt, cosSun);
      vec3 single = (sigmaSr * phaseR + sigmaSm * phaseM) * tSun;
      float msU = clamp((cosSun + 1.0) * 0.5, 0.0, 1.0);
      float msV = clamp(alt / (ATMO_TOP - PLANET_R), 0.0, 1.0);
      int msx = min(MSW - 1, int(msU * float(MSW)));
      int msy = min(MSH - 1, int(msV * float(MSH)));
      vec3 ms = texelFetch(u_multiscatter, ivec2(msx, msy), 0).rgb;
      vec3 multi = (sigmaSr + sigmaSm) * ms;
      vec3 stepTr = exp(-sigmaE * dt);
      vec3 inScatter = single + multi;
      vec3 integ = (inScatter - inScatter * stepTr) / max(sigmaE, vec3(1e-9));
      L += throughput * integ;
      throughput *= stepTr;
    }
  }
  int idx = (gid.y * SKYW + gid.x) * 3;
  vals[idx + 0] = L.r; vals[idx + 1] = L.g; vals[idx + 2] = L.b;
}
)GLSL";

// GPU-compute port of build_transmittance_cpu (256x64, 40 samples, no LUT input).
const char* kTransmittanceComputeSrc = R"GLSL(
#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) writeonly buffer Out { float vals[]; };
const float PLANET_R = 6360000.0; const float ATMO_TOP = 6460000.0;
const float RAYLEIGH_H = 8000.0;  const float MIE_H = 1200.0;
const float MIE_SCA = 3.996e-6;   const float MIE_ABS = 4.4e-6 - 3.996e-6;
const vec3  BETA_R = vec3(5.802e-6, 13.558e-6, 33.1e-6);
const int TW = 256; const int TH = 64; const int SAMPLES = 40;
float raySphere(vec3 o, vec3 d, float r) {
  float b = dot(o, d); float c = dot(o, o) - r * r;
  if (c > 0.0 && b > 0.0) return -1.0;
  float disc = b * b - c; if (disc < 0.0) return -1.0;
  float sq = sqrt(disc); float t0 = -b - sq; float t1 = -b + sq;
  if (t1 < 0.0) return -1.0; return t0 < 0.0 ? t1: t0;
}
vec2 densityRatio(float a) { return vec2(exp(-a / RAYLEIGH_H), exp(-a / MIE_H)); }
void main() {
  ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
  if (gid.x >= TW || gid.y >= TH) return;
  vec3 betaMext = vec3(MIE_SCA + MIE_ABS);
  float top = ATMO_TOP - PLANET_R;
  float v = (float(gid.y) + 0.5) / float(TH); float altitude = v * top; float r = PLANET_R + altitude;
  float u = (float(gid.x) + 0.5) / float(TW); float mu = u * 2.0 - 1.0;
  vec3 origin = vec3(0.0, r, 0.0);
  vec3 dir = vec3(sqrt(max(0.0, 1.0 - mu * mu)), mu, 0.0);
  vec3 result;
  float tTop = raySphere(origin, dir, ATMO_TOP);
  if (tTop <= 0.0) { result = vec3(1.0); }
  else {
    float dt = tTop / float(SAMPLES); vec3 od = vec3(0.0);
    for (int s = 0; s < SAMPLES; ++s) {
      vec3 p = origin + dir * (dt * (float(s) + 0.5));
      float alt = max(0.0, length(p) - PLANET_R);
      vec2 d = densityRatio(alt);
      od += (BETA_R * d.x + betaMext * d.y) * dt;
    }
    result = exp(-od);
  }
  int idx = (gid.y * TW + gid.x) * 3;
  vals[idx + 0] = result.r; vals[idx + 1] = result.g; vals[idx + 2] = result.b;
}
)GLSL";

// GPU-compute port of build_multiscatter_cpu (32x32, 16 dirs x 20 samples; samples
// the transmittance texture). Hillaire isotropic 1-bounce, folded as the analytic series L2/(1-fms).
const char* kMultiscatterComputeSrc = R"GLSL(
#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) writeonly buffer Out { float vals[]; };
uniform sampler2D u_transmittance;
const float PI = 3.14159265358979323846;
const float PLANET_R = 6360000.0; const float ATMO_TOP = 6460000.0;
const float RAYLEIGH_H = 8000.0;  const float MIE_H = 1200.0;
const float MIE_SCA = 3.996e-6;   const float MIE_ABS = 4.4e-6 - 3.996e-6;
const vec3  BETA_R = vec3(5.802e-6, 13.558e-6, 33.1e-6);
const int MSW = 32; const int MSH = 32; const int TW = 256; const int TH = 64;
const int DIRS = 16; const int MARCH = 20;
float raySphere(vec3 o, vec3 d, float r) {
  float b = dot(o, d); float c = dot(o, o) - r * r;
  if (c > 0.0 && b > 0.0) return -1.0;
  float disc = b * b - c; if (disc < 0.0) return -1.0;
  float sq = sqrt(disc); float t0 = -b - sq; float t1 = -b + sq;
  if (t1 < 0.0) return -1.0; return t0 < 0.0 ? t1: t0;
}
vec2 densityRatio(float a) { return vec2(exp(-a / RAYLEIGH_H), exp(-a / MIE_H)); }
vec3 sampleTransmittance(float alt, float cz) {
  float top = ATMO_TOP - PLANET_R;
  float v = clamp(alt / top, 0.0, 1.0); float u = clamp((cz + 1.0) * 0.5, 0.0, 1.0);
  int x = min(TW - 1, int(u * float(TW))); int y = min(TH - 1, int(v * float(TH)));
  return texelFetch(u_transmittance, ivec2(x, y), 0).rgb;
}
void main() {
  ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
  if (gid.x >= MSW || gid.y >= MSH) return;
  vec3 betaMsca = vec3(MIE_SCA); vec3 betaRext = BETA_R; vec3 betaMext = vec3(MIE_SCA + MIE_ABS);
  float top = ATMO_TOP - PLANET_R; float uniformPhase = 1.0 / (4.0 * PI);
  float v = (float(gid.y) + 0.5) / float(MSH); float altitude = v * top; float r = PLANET_R + altitude;
  float u = (float(gid.x) + 0.5) / float(MSW); float muSun = u * 2.0 - 1.0;
  vec3 sunDir = vec3(sqrt(max(0.0, 1.0 - muSun * muSun)), muSun, 0.0);
  vec3 origin = vec3(0.0, r, 0.0);
  vec3 L2nd = vec3(0.0); vec3 fms = vec3(0.0);
  for (int dd = 0; dd < DIRS; ++dd) {
    float fd = (float(dd) + 0.5) / float(DIRS);
    float cosT = 1.0 - 2.0 * fd; float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = fd * PI * (3.0 - sqrt(5.0)) * float(dd);
    vec3 dir = vec3(sinT * cos(phi), cosT, sinT * sin(phi));
    float tMax = raySphere(origin, dir, ATMO_TOP);
    float tPlanet = raySphere(origin, dir, PLANET_R);
    if (tPlanet > 0.0) tMax = min(tMax, tPlanet);
    if (tMax <= 0.0) continue;
    float dt = tMax / float(MARCH);
    vec3 throughput = vec3(1.0);
    for (int s = 0; s < MARCH; ++s) {
      vec3 p = origin + dir * (dt * (float(s) + 0.5));
      float alt = max(0.0, length(p) - PLANET_R);
      vec2 dens = densityRatio(alt);
      vec3 sigmaS = BETA_R * dens.x + betaMsca * dens.y;
      vec3 sigmaE = betaRext * dens.x + betaMext * dens.y;
      vec3 up = normalize(p); float cosSun = dot(up, sunDir);
      vec3 tSun = sampleTransmittance(alt, cosSun);
      vec3 stepTr = exp(-sigmaE * dt);
      vec3 scattered = sigmaS * uniformPhase * tSun;
      vec3 integ = (scattered - scattered * stepTr) / max(sigmaE, vec3(1e-9));
      L2nd += throughput * integ;
      vec3 integF = (sigmaS - sigmaS * stepTr) / max(sigmaE, vec3(1e-9));
      fms += throughput * integF;
      throughput *= stepTr;
    }
  }
  float inv = 1.0 / float(DIRS);
  L2nd *= inv * 4.0 * PI * uniformPhase;
  fms *= inv * 4.0 * PI * uniformPhase;
  vec3 denom = max(vec3(1.0) - fms, vec3(1e-3));
  vec3 result = L2nd / denom;
  int idx = (gid.y * MSW + gid.x) * 3;
  vals[idx + 0] = result.r; vals[idx + 1] = result.g; vals[idx + 2] = result.b;
}
)GLSL";

// Compile + link a compute program from source. Returns 0 on failure (caller falls back to CPU).
GLuint compile_compute(const char* src) {
    GLuint s = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, s);
    glLinkProgram(p);
    glDeleteShader(s);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(p); return 0; }
    return p;
}

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

    //  FIX: build the dome in a SUN-RELATIVE horizontal frame so the LUT's
    // azimuth column u directly matches what the skybox/aerial shaders sample
    // (az = acos(dot(view_horiz, sun_horiz)) -> u). Previously this loop built
    // view_dir on the WORLD x/z axes (the comment claimed a sun frame, the code
    // used world axes), so the warm toward-sun column landed at the sun's WORLD
    // azimuth while the shader read u from the sun-RELATIVE angle. With the sun at
    // an arbitrary world azimuth (e.g. ~141 deg at the pinned t=0.04) the warm
    // band was rotated away from the toward-sun ray and the horizon read blue.
    // Sun-aligned horizontal basis: forward points at the sun's horizontal
    // bearing, right is the orthogonal horizontal axis.
    glm::vec3 sun_horiz(sun_dir_world.x, 0.0f, sun_dir_world.z);
    const float sun_horiz_len = glm::length(sun_horiz);
    glm::vec3 forward = sun_horiz_len > 1e-4f ? sun_horiz / sun_horiz_len: glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 right(-forward.z, 0.0f, forward.x); // 90 deg CCW about +Y

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
            const float azimuth = u * 2.0f * glm::pi<float>(); // 0 = toward sun
            // View ray in the sun-relative horizontal frame: azimuth 0 points at
            // the sun's bearing, so column u mirrors the shader's sun-relative az.
            const glm::vec3 view_dir =
                up * cos_view + (forward * std::cos(azimuth) + right * std::sin(azimuth)) * sin_view;

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
    m_sky_ambient = ambient_weight > 0.0f ? ambient_accum / ambient_weight * glm::pi<float>(): glm::vec3(0.0f);
    (void)sun_cos_zenith;
}

// --- GPU sky-view ( , render.sky_lut_gpu) -----------------------

bool SkyAtmosphereLut::ensure_skyview_compute_resources() {
    if (m_skyview_compute_prog == 0) {
        m_skyview_compute_prog = compile_compute(kSkyViewComputeSrc);
        if (m_skyview_compute_prog == 0) return false;
    }
    if (m_skyview_ssbo == 0) {
        glGenBuffers(1, &m_skyview_ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_skyview_ssbo);
        // Sized to the LARGEST LUT (sky-view 192x108 > transmittance 256x64 > multiscatter 32x32),
        // so the same SSBO is reused as the dispatch target + PBO upload source for all three.
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     static_cast<GLsizeiptr>(sizeof(float) * 3u * kSkyViewWidth * kSkyViewHeight),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    return m_skyview_ssbo != 0;
}

namespace {
// Allocate an empty RGB16F texture (GPU init fills it via glTexSubImage2D from the PBO).
void allocate_empty_lut_texture(GLuint& tex, int width, int height, const char* label) {
    if (tex != 0) return;
    glGenTextures(1, &tex);
    PassGl::label_gl_object(GL_TEXTURE, tex, label);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}
}  // namespace

void SkyAtmosphereLut::prewarm_gpu_compute() {
    if (!m_use_gpu_skyview) return;
    ensure_skyview_compute_resources();  // compiles sky-view prog + allocates the shared SSBO
    if (m_transmittance_compute_prog == 0) m_transmittance_compute_prog = compile_compute(kTransmittanceComputeSrc);
    if (m_multiscatter_compute_prog == 0) m_multiscatter_compute_prog = compile_compute(kMultiscatterComputeSrc);
}

bool SkyAtmosphereLut::build_sky_lut_gpu_init(const glm::vec3& sun_dir_world) {
    if (!ensure_skyview_compute_resources()) return false;
    if (m_transmittance_compute_prog == 0) {
        m_transmittance_compute_prog = compile_compute(kTransmittanceComputeSrc);
        if (m_transmittance_compute_prog == 0) return false;
    }
    if (m_multiscatter_compute_prog == 0) {
        m_multiscatter_compute_prog = compile_compute(kMultiscatterComputeSrc);
        if (m_multiscatter_compute_prog == 0) return false;
    }
    allocate_empty_lut_texture(m_transmittance_tex, kTransmittanceWidth, kTransmittanceHeight, "sky.lut.transmittance");
    allocate_empty_lut_texture(m_multiscatter_tex, kMultiScatterWidth, kMultiScatterHeight, "sky.lut.multiscatter");
    allocate_empty_lut_texture(m_skyview_tex, kSkyViewWidth, kSkyViewHeight, "sky.lut.skyview");
    if (m_transmittance_tex == 0 || m_multiscatter_tex == 0 || m_skyview_tex == 0) return false;

    auto dispatch_and_upload = [&](GLuint prog, int w, int h, GLuint tex) {
        glUseProgram(prog);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_skyview_ssbo);
        glDispatchCompute(static_cast<GLuint>((w + 7) / 8), static_cast<GLuint>((h + 7) / 8), 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_skyview_ssbo);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_FLOAT, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        // One-shot init: a full barrier before the next stage reuses the shared SSBO (correctness
        // over throughput — this runs once at startup).
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
    };

    // 1. Transmittance (no input). 2. Multiscatter (samples transmittance). 3. Sky-view + ambient.
    dispatch_and_upload(m_transmittance_compute_prog, kTransmittanceWidth, kTransmittanceHeight, m_transmittance_tex);

    glUseProgram(m_multiscatter_compute_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_transmittance_tex);
    glUniform1i(glGetUniformLocation(m_multiscatter_compute_prog, "u_transmittance"), 0);
    dispatch_and_upload(m_multiscatter_compute_prog, kMultiScatterWidth, kMultiScatterHeight, m_multiscatter_tex);

    glUseProgram(0);
    return build_sky_view_gpu(sun_dir_world);  // dispatch + upload + ambient readback
}

bool SkyAtmosphereLut::build_sky_view_gpu(const glm::vec3& sun_dir_world) {
    // Needs the transmittance/multiscatter textures (the compute samples them) and the sky-view
    // texture (the compute writes it); all three are uploaded by the CPU initialize. Refuse if
    // any is missing or the compute program/SSBO can't be created -> caller falls back to CPU.
    if (m_transmittance_tex == 0 || m_multiscatter_tex == 0 || m_skyview_tex == 0) return false;
    if (!ensure_skyview_compute_resources()) return false;

    glUseProgram(m_skyview_compute_prog);
    glUniform3f(glGetUniformLocation(m_skyview_compute_prog, "u_sunDir"),
                sun_dir_world.x, sun_dir_world.y, sun_dir_world.z);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_transmittance_tex);
    glUniform1i(glGetUniformLocation(m_skyview_compute_prog, "u_transmittance"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_multiscatter_tex);
    glUniform1i(glGetUniformLocation(m_skyview_compute_prog, "u_multiscatter"), 1);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_skyview_ssbo);

    glDispatchCompute(static_cast<GLuint>((kSkyViewWidth + 7) / 8),
                      static_cast<GLuint>((kSkyViewHeight + 7) / 8), 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT);

    // RGB16F is not image-store-able, so upload the SSBO into the sky-view texture through a
    // PIXEL_UNPACK_BUFFER: 3 tightly-packed floats per texel match GL_RGB/GL_FLOAT exactly.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_skyview_ssbo);
    glBindTexture(GL_TEXTURE_2D, m_skyview_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kSkyViewWidth, kSkyViewHeight, GL_RGB, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    // the blocking glGetBufferSubData ambient readback is
    // retired onto the asynchronous-readback ring — submit + a BOUNDED zero-timeout poll, so a
    // wedged GPU can no longer stall this call indefinitely (on timeout the
    // caller falls back to the CPU sky-view path). Same-invocation consumption
    // is retained because m_sky_ambient must match the LUT this call built and
    // the path is the default-OFF debug-stall fix ("the readback stall is
    // acceptable for the use case"); a fully deferred stale-safe consumer is
    // the implementation note if this ever ships default-ON.
    const std::size_t skyview_bytes =
        sizeof(float) * 3u * static_cast<std::size_t>(kSkyViewWidth) * kSkyViewHeight;
    m_skyview_cpu.assign(static_cast<std::size_t>(kSkyViewWidth) * kSkyViewHeight, glm::vec3(0.0f));
    bool readback_ok = false;
    if (m_skyview_readback.ensure(skyview_bytes) && m_skyview_readback.begin()) {
        m_skyview_readback.copy_region(m_skyview_ssbo, 0, 0, skyview_bytes);
        m_skyview_readback.submit();
        glFlush();
        const void* mapped = nullptr;
        std::size_t mapped_bytes = 0;
        const auto readback_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!(readback_ok = m_skyview_readback.consume(&mapped, &mapped_bytes))) {
            if (std::chrono::steady_clock::now() >= readback_deadline) {
                break;
            }
            std::this_thread::yield();
        }
        if (readback_ok && mapped_bytes >= skyview_bytes) {
            std::memcpy(m_skyview_cpu.data(), mapped, skyview_bytes);
        } else {
            readback_ok = false;
        }
    }
    glUseProgram(0);
    if (!readback_ok) {
        return false;  // caller falls back to the CPU sky-view path
    }

    glm::vec3 ambient_accum(0.0f);
    float ambient_weight = 0.0f;
    for (int y = 0; y < kSkyViewHeight; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kSkyViewHeight);
        const float view_zenith = v * glm::pi<float>();
        const float cos_view = std::cos(view_zenith);
        if (cos_view <= 0.0f) continue;
        const float sin_view = std::sin(view_zenith);
        for (int x = 0; x < kSkyViewWidth; ++x) {
            const glm::vec3 L = m_skyview_cpu[static_cast<std::size_t>(y) * kSkyViewWidth + x];
            ambient_accum += L * cos_view * sin_view;
            ambient_weight += cos_view * sin_view;
        }
    }
    m_skyview_sun_dir = sun_dir_world;
    m_sky_ambient = ambient_weight > 0.0f ? ambient_accum / ambient_weight * glm::pi<float>(): glm::vec3(0.0f);
    return true;
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

    // build all three LUTs on the GPU when render.sky_lut_gpu is on (the CPU
    // integration is ~32 ms at startup, over the 8 ms budget). Falls back to the full CPU build if
    // any GPU resource fails.
    bool gpu_init_done = false;
    if (m_use_gpu_skyview) {
        gpu_init_done = build_sky_lut_gpu_init(glm::normalize(sun_dir_world));
    }
    if (!gpu_init_done) {
        build_transmittance_cpu();
        build_multiscatter_cpu();
        build_sky_view_cpu(glm::normalize(sun_dir_world));

        upload_texture(m_transmittance_tex, kTransmittanceWidth, kTransmittanceHeight, m_transmittance_cpu, "sky.lut.transmittance");
        upload_texture(m_multiscatter_tex, kMultiScatterWidth, kMultiScatterHeight, m_multiscatter_cpu, "sky.lut.multiscatter");
        upload_texture(m_skyview_tex, kSkyViewWidth, kSkyViewHeight, m_skyview_cpu, "sky.lut.skyview");
    }

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
    //  GPU compute path when render.sky_lut_gpu is on (it writes the texture +
    // ambient itself); fall back to the CPU march if the path is off or its resources fail.
    bool gpu_done = false;
    if (m_use_gpu_skyview) {
        gpu_done = build_sky_view_gpu(sun);
    }
    if (!gpu_done) {
        build_sky_view_cpu(sun);
        update_texture(m_skyview_tex, kSkyViewWidth, kSkyViewHeight, m_skyview_cpu);
    }
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
    //  GPU sky-view resources.
    if (m_skyview_compute_prog) { glDeleteProgram(m_skyview_compute_prog); m_skyview_compute_prog = 0; }
    if (m_transmittance_compute_prog) { glDeleteProgram(m_transmittance_compute_prog); m_transmittance_compute_prog = 0; }
    if (m_multiscatter_compute_prog) { glDeleteProgram(m_multiscatter_compute_prog); m_multiscatter_compute_prog = 0; }
    if (m_skyview_ssbo) { glDeleteBuffers(1, &m_skyview_ssbo); m_skyview_ssbo = 0; }
    m_base_built = false;
}

} // namespace Luminumbra::Rendering
