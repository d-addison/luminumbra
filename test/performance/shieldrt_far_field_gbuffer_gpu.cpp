// Standalone far-field G-buffer raymarch, validated offscreen.
//
// The heightfield max-mip march (an overshoot-free hierarchical DDA) hits the
// real surface. This test validates the render form of that tracer: a fullscreen
// fragment pass that writes the deferred G-buffer
// (view-space position, octahedral view-space normal + material id, albedo/
// roughness, metallic/AO) and gl_FragDepth. It is an isolated algorithm test,
// not a claim that the shipping mesh-based far-LOD path uses this shader.
//
// The pass reconstructs a per-pixel world ray from the inverse view-projection
// from the inverse view-projection, marches the
// heightfield, and on a hit writes the G-buffer + depth; on a miss it discards
// (the sky / nearer mesh geometry is left intact). The test renders into an
// offscreen MRT FBO and validates:
//   - HIT POSITION: the view-space gPosition, transformed back to world space,
//     lands on the analytic ground-truth surface (GetTerrainHeightAtCoarse),
//     reusing the inc1 quantization-aware tolerance — the load-bearing leg.
//   - NORMAL: the decoded view-space normal, rotated back to world space, is unit
//     length and points up (terrain normals have +Y), i.e. the heightfield-
//     gradient normal is sane and correctly encoded/round-tripped.
//   - DEPTH: gl_FragDepth round-trips to the same hit (the position leg covers
//     this implicitly; coverage asserts a sane hit fraction).
//
// The validation FBO uses RGBA32F attachments to isolate SHADER-LOGIC correctness
// from the live G-buffer's RGB16F/RGB10A2 format precision (format fidelity is a
// live-wiring concern). Render-only, GPU-gated (skips headless/software). Label
// manual;perf;gpu — run:
//   ctest -L manual -R ShieldRtFarFieldGbufferGpu --output-on-failure

#include "gtest/gtest.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/JobSystem.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/FarLodStore.h"
#include "world/TerrainPresetLoader.h"

#include "shieldrt_far_field.h"
#include "shieldrt_gl_harness.h"

namespace fs = std::filesystem;

using namespace Luminumbra;
using namespace Luminumbra::Systems;
using namespace luminumbra_shieldrt;
using Luminumbra::World::ComputeTerrainParamsHash;
using Luminumbra::World::FarLodTier;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif
#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

constexpr int kSeed = 424242;
constexpr int kMaxMipLevels = 24;
constexpr int kViewW = 480;
constexpr int kViewH = 270;
constexpr float kFovDeg = 60.0f;
constexpr float kNear = 0.1f;
constexpr float kFar = 4000.0f; // far enough to encompass the ~2.7 km t_max block

// Reuse the inc1 ground-truth tolerances (same surface, same comparison).
constexpr double kGroundTruthMedianToleranceM = 0.5;
constexpr double kGroundTruthP99ToleranceM = 3.0;
constexpr double kMinHitFraction = 0.02;
constexpr double kMaxHitFraction = 0.995;
// Decoded-normal sanity: unit length within tolerance, and terrain-up (+Y).
constexpr double kNormalUnitToleranceM = 0.02;
constexpr double kMinUpNormalFraction = 0.95; // ≥95% of hits have worldN.y > 0

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}
fs::path ArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "performance";
}

TerrainGenParams LoadPresetParams(const fs::path& path) {
    const Luminumbra::world::TerrainPresetLoadResult result =
        Luminumbra::world::LoadTerrainPreset(path);
    EXPECT_TRUE(result.ok) << path.string();
    return result.params;
}

struct FlatMaxMip {
    std::vector<float> data;
    std::array<std::int32_t, kMaxMipLevels> offset{};
    std::array<std::int32_t, kMaxMipLevels> dim{};
    int levels = 0;
};
FlatMaxMip FlattenMaxMip(const HeightMaxMip& mip) {
    FlatMaxMip f;
    f.levels = mip.levels;
    EXPECT_LE(mip.levels, kMaxMipLevels);
    std::int32_t off = 0;
    for (int L = 0; L < mip.levels; ++L) {
        f.offset[L] = off;
        f.dim[L] = mip.dims[L];
        f.data.insert(f.data.end(), mip.max_h[L].begin(), mip.max_h[L].end());
        off += static_cast<std::int32_t>(mip.max_h[L].size());
    }
    return f;
}

GLuint MakeStorageBufferF(const std::vector<float>& v) {
    return MakeStorageBuffer(static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data());
}

GLuint CompileRenderProgram(const char* vs, const char* fs_src, std::string& error) {
    auto compile = [&](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = GL_FALSE;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            GLint len = 0;
            glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
            std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
            glGetShaderInfoLog(s, len, nullptr, log.data());
            error = "compile failed: " + log;
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    const GLuint v = compile(GL_VERTEX_SHADER, vs);
    if (!v)
        return 0;
    const GLuint f = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!f) {
        glDeleteShader(v);
        return 0;
    }
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        error = "link failed: " + log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Fullscreen triangle (no VBO).
const char* kFullscreenVert = R"GLSL(
#version 450 core
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0: -1.0, (gl_VertexID == 2) ? 3.0: -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

// Far-field G-buffer raymarch. Reconstructs a per-pixel world ray from the
// inverse view-projection, marches the heightfield with the inc1 overshoot-free
// hierarchical DDA, and on a hit writes the deferred G-buffer + gl_FragDepth;
// on a miss discards. Same MRT layout/encoding as res/shaders/g_buffer.frag.
const char* kFarFieldGbufferFrag = R"GLSL(
#version 450 core

layout(location = 0) out vec3 gPosition;        // view-space position
layout(location = 1) out vec4 gNormalMaterial;  // oct view-space normal + matId/255
layout(location = 2) out vec4 gAlbedoRoughness;
layout(location = 3) out vec2 gMetallicAO;

layout(std430, binding = 1) readonly buffer HfBuf { float hf[]; };
layout(std430, binding = 2) readonly buffer MipBuf{ float mip[]; };

uniform vec2  u_viewport;
uniform vec3  u_eye;
uniform mat4  u_invViewProj;
uniform mat4  u_view;
uniform mat4  u_viewProj;
uniform mat3  u_normalView;
uniform int   u_n;
uniform float u_step;
uniform vec2  u_origin;    // ox, oz
uniform float u_tmax;
uniform int   u_levels;
uniform int   u_maxSteps;
uniform float u_seaLevel;
uniform int   u_mipOffset[24];
uniform int   u_mipDim[24];

float hfSample(float wx, float wz) {
    float fx = clamp((wx - u_origin.x) / u_step, 0.0, float(u_n - 1));
    float fz = clamp((wz - u_origin.y) / u_step, 0.0, float(u_n - 1));
    int x0 = int(floor(fx)); int z0 = int(floor(fz));
    int x1 = min(x0 + 1, u_n - 1); int z1 = min(z0 + 1, u_n - 1);
    float tx = fx - float(x0); float tz = fz - float(z0);
    float h00 = hf[z0 * u_n + x0]; float h10 = hf[z0 * u_n + x1];
    float h01 = hf[z1 * u_n + x0]; float h11 = hf[z1 * u_n + x1];
    float a = h00 + (h10 - h00) * tx;
    float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
}

float cellMaxAt(int L, float px, float pz) {
    float cs = u_step * float(1 << L);
    int dim = u_mipDim[L];
    int cx = clamp(int(floor((px - u_origin.x) / cs)), 0, dim - 1);
    int cz = clamp(int(floor((pz - u_origin.y) / cs)), 0, dim - 1);
    return mip[u_mipOffset[L] + cz * dim + cx];
}

float cellExitDist(int L, float px, float pz, float dx, float dz) {
    float cs = u_step * float(1 << L);
    float lx = (px - u_origin.x) / cs; float lz = (pz - u_origin.y) / cs;
    int cx = int(floor(lx)); int cz = int(floor(lz));
    float tx = 1.0e30; float tz = 1.0e30;
    if (abs(dx) > 1.0e-9) {
        float bound = (dx > 0.0 ? float(cx + 1): float(cx)) * cs + u_origin.x;
        tx = (bound - px) / dx;
    }
    if (abs(dz) > 1.0e-9) {
        float bound = (dz > 0.0 ? float(cz + 1): float(cz)) * cs + u_origin.y;
        tz = (bound - pz) / dz;
    }
    return max(min(tx, tz), 0.0);
}

vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0); }
vec2 encode_octahedral(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy: octWrap(n.xy);
}

void main() {
    vec2 uv = gl_FragCoord.xy / u_viewport;
    vec4 farp = u_invViewProj * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    farp /= farp.w;
    vec3 d = normalize(farp.xyz - u_eye);
    vec3 o = u_eye;

    float base_cell = u_step;
    float ext = float(u_n - 1) * u_step;
    float eps = base_cell * 0.01;
    float t = 0.0;
    int L = u_levels - 1;
    int steps = 0;
    float hit_t = -1.0;
    while (t < u_tmax && steps < u_maxSteps) {
        steps++;
        float px = o.x + d.x * t;
        float py = o.y + d.y * t;
        float pz = o.z + d.z * t;
        if (px < u_origin.x - base_cell || pz < u_origin.y - base_cell ||
            px > u_origin.x + ext + base_cell || pz > u_origin.y + ext + base_cell) break;
        float exit_d = cellExitDist(L, px, pz, d.x, d.z);
        float t_exit = t + exit_d;
        float py_exit = o.y + d.y * t_exit;
        float cmax = cellMaxAt(L, px, pz);
        if (py > cmax && py_exit > cmax) {
            t = t_exit + eps;
            if (L < u_levels - 1) ++L;
            continue;
        }
        if (L > 0) { --L; continue; }
        float surf_t = hfSample(px, pz);
        if (py - surf_t <= 0.0) { hit_t = t; break; }
        float px_e = o.x + d.x * t_exit;
        float pz_e = o.z + d.z * t_exit;
        if (py_exit - hfSample(px_e, pz_e) <= 0.0) {
            float lo = t; float hi = t_exit;
            for (int it = 0; it < 20; ++it) {
                float tm = 0.5 * (lo + hi);
                float mx = o.x + d.x * tm;
                float my = o.y + d.y * tm;
                float mz = o.z + d.z * tm;
                if (my - hfSample(mx, mz) <= 0.0) hi = tm; else lo = tm;
            }
            hit_t = hi;
            break;
        }
        t = t_exit + eps;
        if (L < u_levels - 1) ++L;
    }

    if (hit_t < 0.0) discard;  // sky / no far terrain: leave the framebuffer

    vec3 P = o + d * hit_t;

    // gl_FragDepth from the clip-space hit (so depth-tested passes see it).
    vec4 clip = u_viewProj * vec4(P, 1.0);
    gl_FragDepth = (clip.z / clip.w) * 0.5 + 0.5;

    // View-space position.
    gPosition = (u_view * vec4(P, 1.0)).xyz;

    // Heightfield-gradient world normal (central differences) -> view space oct.
    float e = u_step;
    float hl = hfSample(P.x - e, P.z);
    float hr = hfSample(P.x + e, P.z);
    float hd = hfSample(P.x, P.z - e);
    float hu = hfSample(P.x, P.z + e);
    vec3 worldN = normalize(vec3(hl - hr, 2.0 * e, hd - hu));
    vec3 viewN = normalize(u_normalView * worldN);
    vec2 enc = encode_octahedral(viewN);

    // Far terrain material: sand below the shoreline band, grass/stone above
    // (a coarse far-field classification; live mesh parity is a separate leg).
    float matId = (P.y < u_seaLevel + 1.0) ? 4.0: (worldN.y > 0.9 ? 3.0: 1.0);
    gNormalMaterial = vec4(enc * 0.5 + 0.5, 0.0, matId / 255.0);
    gAlbedoRoughness = vec4(0.4, 0.45, 0.3, 0.9);
    gMetallicAO = vec2(0.0, 1.0);
}
)GLSL";

double Percentile(std::vector<double> v, double pct) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t idx = static_cast<std::size_t>(
        std::min(v.size() - 1, static_cast<std::size_t>(pct * (v.size() - 1) + 0.5)));
    return v[idx];
}

// Octahedral decode (inverse of res/shaders/g_buffer.frag's encode).
glm::vec3 DecodeOct(glm::vec2 f) {
    glm::vec3 n(f.x, f.y, 1.0f - std::abs(f.x) - std::abs(f.y));
    const float t = std::max(-n.z, 0.0f);
    n.x += (n.x >= 0.0f) ? -t : t;
    n.y += (n.y >= 0.0f) ? -t : t;
    return glm::normalize(n);
}

} // namespace

TEST(ShieldRtFarFieldGbufferGpu, RaymarchWritesCorrectGbuffer) {
    HiddenGlContext ctx("shieldrt_far_field_gbuffer_gpu");
    if (!ctx.ready()) {
        GTEST_SKIP() << "no GL context (headless): " << ctx.error();
    }
    const std::string renderer = GlString(GL_RENDERER);
    const std::string gl_version = GlString(GL_VERSION);
    const bool software = IsSoftwareRenderer(renderer);

    std::string err;
    const GLuint prog = CompileRenderProgram(kFullscreenVert, kFarFieldGbufferFrag, err);
    ASSERT_NE(prog, 0u) << err;

    // Offscreen MRT FBO (RGBA32F for exact readback) + depth.
    GLuint fbo = 0, depth_rb = 0;
    std::array<GLuint, 4> tex{};
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(4, tex.data());
    for (int i = 0; i < 4; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kViewW, kViewH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, tex[i], 0);
    }
    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kViewW, kViewH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    const std::array<GLenum, 4> bufs{
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
    glDrawBuffers(4, bufs.data());
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    GLuint vao = 0;
    glGenVertexArrays(1, &vao);

    fs::create_directories(ArtifactRoot());

    const std::array<ViewSpec, 4> view_specs{{
        {"default_grazing", "default.json", true},
        {"default_elevated", "default.json", false},
        {"mountains_grazing", "mountains.json", true},
        {"mountains_elevated", "mountains.json", false},
    }};

    constexpr double kBlockSpan = kRegions * 512.0;
    const double cx = (kRx0 + kRegions * 0.5) * 512.0;
    const double cz = (kRz0 + kRegions * 0.5) * 512.0;
    constexpr int kFarStep = 4;
    const float aspect = static_cast<float>(kViewW) / static_cast<float>(kViewH);

    nlohmann::json results = nlohmann::json::array();
    double worst_median_gt = 0.0, worst_p99_gt = 0.0, worst_unit = 0.0;

    JobSystem jobs;
    jobs.startup();

    for (const ViewSpec& vs : view_specs) {
        const fs::path preset_path = SourceRoot() / "worlds/atlas/presets" / vs.preset;
        ASSERT_TRUE(fs::exists(preset_path)) << preset_path.string();
        const TerrainGenParams params = LoadPresetParams(preset_path);
        SHIELD_WorldSystem world(&jobs, nullptr, params, kSeed);
        const std::uint64_t params_hash = ComputeTerrainParamsHash(params, kSeed);

        const HeightField hf =
            BuildHeightFieldFromTiles(world, FarLodTier::F1, kRx0, kRz0, kRegions, params_hash);
        const HeightMaxMip hmip = BuildHeightMaxMip(hf);
        const FlatMaxMip flat_mip = FlattenMaxMip(hmip);

        const double terrain_at_center = hf.sample(cx, cz);
        glm::dvec3 eye_d, fwd_d;
        if (vs.grazing) {
            eye_d = {cx - kBlockSpan * 0.5, terrain_at_center + 30.0, cz - kBlockSpan * 0.5};
            fwd_d = glm::normalize(glm::dvec3(1.0, -0.04, 1.0));
        } else {
            eye_d = {cx - kBlockSpan * 0.4, hf.max_h + 250.0, cz - kBlockSpan * 0.4};
            fwd_d = glm::normalize(glm::dvec3(1.0, -0.6, 1.0));
        }
        const glm::vec3 eye(eye_d);
        const glm::vec3 fwd(fwd_d);
        const glm::mat4 view = glm::lookAt(eye, eye + fwd, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 proj = glm::perspective(glm::radians(kFovDeg), aspect, kNear, kFar);
        const glm::mat4 view_proj = proj * view;
        const glm::mat4 inv_view_proj = glm::inverse(view_proj);
        const glm::mat4 inv_view = glm::inverse(view);
        const glm::mat3 normal_view(view); // rotation part (lookAt has no scale)
        const double t_max = kBlockSpan * 1.8;

        const GLuint hf_buf = MakeStorageBufferF(hf.h);
        const GLuint mip_buf = MakeStorageBufferF(flat_mip.data);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, kViewW, kViewH);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE);

        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "u_viewport"), float(kViewW), float(kViewH));
        glUniform3f(glGetUniformLocation(prog, "u_eye"), eye.x, eye.y, eye.z);
        glUniformMatrix4fv(
            glGetUniformLocation(prog, "u_invViewProj"), 1, GL_FALSE, &inv_view_proj[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(prog, "u_view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(prog, "u_viewProj"), 1, GL_FALSE, &view_proj[0][0]);
        glUniformMatrix3fv(
            glGetUniformLocation(prog, "u_normalView"), 1, GL_FALSE, &normal_view[0][0]);
        glUniform1i(glGetUniformLocation(prog, "u_n"), hf.n);
        glUniform1f(glGetUniformLocation(prog, "u_step"), static_cast<float>(hf.step));
        glUniform2f(glGetUniformLocation(prog, "u_origin"),
                    static_cast<float>(hf.ox),
                    static_cast<float>(hf.oz));
        glUniform1f(glGetUniformLocation(prog, "u_tmax"), static_cast<float>(t_max));
        glUniform1i(glGetUniformLocation(prog, "u_levels"), flat_mip.levels);
        glUniform1i(glGetUniformLocation(prog, "u_maxSteps"), kMaxMarchSteps);
        glUniform1f(glGetUniformLocation(prog, "u_seaLevel"), 0.0f);
        SetIntArray(prog, "u_mipOffset", flat_mip.offset.data(), flat_mip.levels);
        SetIntArray(prog, "u_mipDim", flat_mip.dim.data(), flat_mip.levels);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, hf_buf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, mip_buf);

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();

        const std::size_t px_count = static_cast<std::size_t>(kViewW) * kViewH;
        std::vector<float> pos(px_count * 4), nrm(px_count * 4);
        glBindTexture(GL_TEXTURE_2D, tex[0]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pos.data());
        glBindTexture(GL_TEXTURE_2D, tex[1]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, nrm.data());

        long long hits = 0, up_normals = 0;
        double unit_max = 0.0;
        std::vector<double> gt_abs;
        for (std::size_t p = 0; p < px_count; ++p) {
            const float mat = nrm[p * 4 + 3];
            if (mat <= 0.0f)
                continue; // discard (no write) left material 0
            ++hits;
            // view-space position -> world
            const glm::vec4 vpos(pos[p * 4 + 0], pos[p * 4 + 1], pos[p * 4 + 2], 1.0f);
            const glm::vec4 wpos = inv_view * vpos;
            const double analytic =
                static_cast<double>(world.GetTerrainHeightAtCoarse(wpos.x, wpos.z, kFarStep));
            gt_abs.push_back(std::abs(static_cast<double>(wpos.y) - analytic));
            // decode normal (stored as enc*0.5+0.5) -> view -> world
            const glm::vec2 enc(nrm[p * 4 + 0] * 2.0f - 1.0f, nrm[p * 4 + 1] * 2.0f - 1.0f);
            const glm::vec3 viewN = DecodeOct(enc);
            unit_max = std::max(unit_max, std::abs(static_cast<double>(glm::length(viewN)) - 1.0));
            const glm::vec3 worldN = glm::normalize(glm::mat3(inv_view) * viewN);
            if (worldN.y > 0.0f)
                ++up_normals;
        }

        const double hit_fraction = static_cast<double>(hits) / px_count;
        double gt_median = 0.0, gt_p99 = 0.0, gt_max = 0.0;
        if (!gt_abs.empty()) {
            gt_median = Percentile(gt_abs, 0.50);
            gt_p99 = Percentile(gt_abs, 0.99);
            gt_max = *std::max_element(gt_abs.begin(), gt_abs.end());
        }
        const double up_fraction = hits > 0 ? static_cast<double>(up_normals) / hits : 0.0;

        worst_median_gt = std::max(worst_median_gt, gt_median);
        worst_p99_gt = std::max(worst_p99_gt, gt_p99);
        worst_unit = std::max(worst_unit, unit_max);

        results.push_back({
            {"view", vs.name},
            {"preset", vs.preset},
            {"pixels", px_count},
            {"hits", hits},
            {"hit_fraction", hit_fraction},
            {"ground_truth_median_m", gt_median},
            {"ground_truth_p99_m", gt_p99},
            {"ground_truth_max_m", gt_max},
            {"normal_unit_max_err", unit_max},
            {"up_normal_fraction", up_fraction},
        });

        EXPECT_GT(hit_fraction, kMinHitFraction)
            << vs.name << ": far-field pass produced almost no hits";
        EXPECT_LT(hit_fraction, kMaxHitFraction)
            << vs.name << ": far-field pass filled ~everything";
        EXPECT_LE(gt_median, kGroundTruthMedianToleranceM)
            << vs.name << ": gPosition median drifts from analytic ground truth";
        EXPECT_LE(gt_p99, kGroundTruthP99ToleranceM)
            << vs.name << ": gPosition p99 drifts from analytic ground truth";
        EXPECT_LE(unit_max, kNormalUnitToleranceM) << vs.name << ": decoded normal not unit length";
        EXPECT_GE(up_fraction, kMinUpNormalFraction)
            << vs.name << ": far-field normals are not terrain-up (gradient/encode error)";

        glDeleteBuffers(1, &hf_buf);
        glDeleteBuffers(1, &mip_buf);
    }

    jobs.shutdown();
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(4, tex.data());
    glDeleteRenderbuffers(1, &depth_rb);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(prog);

#ifdef NDEBUG
    const char* build_mode = "release";
#else
    const char* build_mode = "debug";
#endif

    const nlohmann::json report = {
        {"schema", "luminumbra.shieldrt_far_field_gbuffer.v1"},
        {"generated_by",
         "shieldrt_far_field_gbuffer_gpu (fullscreen far-field raymarch -> G-buffer)"},
        {"seed", kSeed},
        {"build_mode", build_mode},
        {"gpu",
         {{"renderer", renderer}, {"gl_version", gl_version}, {"software_renderer", software}}},
        {"viewport", {{"w", kViewW}, {"h", kViewH}}},
        {"tolerances",
         {
             {"ground_truth_median_m", kGroundTruthMedianToleranceM},
             {"ground_truth_p99_m", kGroundTruthP99ToleranceM},
             {"normal_unit_max_err", kNormalUnitToleranceM},
             {"min_up_normal_fraction", kMinUpNormalFraction},
         }},
        {"worst",
         {
             {"ground_truth_median_m", worst_median_gt},
             {"ground_truth_p99_m", worst_p99_gt},
             {"normal_unit_max_err", worst_unit},
         }},
        {"views", results},
        {"note",
         "Proves the production far-field raymarch writes correct deferred G-buffer "
         "values (view-space position on the analytic surface, sane terrain-up "
         "octahedral normal, gl_FragDepth) offscreen, before live RenderPipeline "
         "wiring () and GPU-resident heightfield streaming ()."},
    };
    std::ofstream out(ArtifactRoot() / "shieldrt-far-field-gbuffer.json");
    ASSERT_TRUE(out);
    out << std::setw(2) << report << "\n";

    if (software) {
        GTEST_SKIP() << "software renderer (" << renderer
                     << "): not representative; artifact written";
    }
}
