// far-field GROUND-TRUTH parity gate.
//
// heightfield tracer validation pinned the heightfield max-mip march as the far-field tracer.
// Before that tracer is wired into the live G-buffer, this gate proves it is CORRECT: every ray it
// reports as a hit must land on the real terrain surface, not on a degenerate early-out (the
// failure mode the heightfield tracer validation profile caught in the SDF path, where coarse mips
// collapsed to ~0 and false-hit the sky).
//
// Two correctness legs (-SPEC MAJOR #9 — the analytic leg is load-bearing):
//
//   (1) SELF-CONSISTENCY: the GPU march's hit point must lie ON the bilinear
//       FarLodStore heightfield it marches (|hit_h - hf.sample(hit_xz)| small).
//       This proves the tracer found a genuine surface crossing rather than
//       reporting a spurious far hit.
//
//   (2) GROUND TRUTH: the hit height must match the analytic terrain generator
//       `SHIELD_WorldSystem::GetTerrainHeightAtCoarse(x, z, 4)` at the hit XZ —
//       the exact function that produced the tile node samples — within a
//       quantization-aware tolerance (height_q is 1/16 m; the F1 grid is 4 m, so
//       between nodes the bilinear interpolation of quantized samples deviates
//       from the continuous coarse surface by a small, bounded amount).
//
// The same GPU kernel as heightfield tracer validation (shared algorithm), extended to emit the
// refined hit distance t per ray. Render-only: NO world_hash / determinism contract. GPU- gated
// like the profile (skips headless; writes artifact + skips asserts on a software renderer). Label
// manual;perf;gpu — run:
//   ctest -L manual -R ShieldRtFarFieldParityGpu --output-on-failure

#include "gtest/gtest.h"

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
constexpr int kLocalSizeX = 64;
constexpr int kMaxMipLevels = 24;

// Self-consistency: the GPU march (float) hit height vs the CPU bilinear (double)
// of the same heightfield. The 20-step bisection refines t to << 1 mm; observed
// worst is ~0.09 m (float slack at km-scale world coords). A degenerate/overshoot
// hit misses this by hundreds of metres (the bug this gate caught was ~1360 m).
constexpr double kSelfConsistencyToleranceM = 0.5;

// Ground truth: bilinear-of-quantized-4m-nodes vs the continuous coarse analytic
// surface at the same XZ. Bounds quantization (1/16 m) + 4 m-grid interpolation
// error. Observed median ~0.19 m, p99 ~1.32 m, max ~4.6 m on steep ridges. The
// asserted bounds sit ~2.5x above observed — tight enough that a raymarch or
// representation regression trips them (the overshoot bug gave median 766 m /
// p99 1315 m), loose enough that grid discretization does not. Observed values
// recorded in the artifact for provenance.
constexpr double kGroundTruthMedianToleranceM = 0.5;
constexpr double kGroundTruthP99ToleranceM = 3.0;

// Fraction of a view's rays that must hit (rejects an all-miss degenerate tracer;
// the upper bound rejects an all-hit one).
constexpr double kMinHitFraction = 0.02;
constexpr double kMaxHitFraction = 0.995;

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

// PRODUCTION heightfield max-mip march (overshoot-free hierarchical DDA),
// emitting the refined hit distance t per ray (-1.0 on miss).
//
// At the current mip level L it steps to the NEXT CELL BOUNDARY along the ray
// (never by a fixed cell size from mid-cell — the overshoot bug that the
// parity gate caught in the spike's port). Because py is linear in t, if the ray
// is above the cell's max height at BOTH the entry and the cell-exit t, it is
// above the whole cell (linear minimum is at an endpoint) and the cell is safely
// skipped; on a skip it climbs a level for bigger empty-space jumps. If it is not
// safely above, it descends a level; at the finest level a remaining cross is
// resolved by bisection between the cell-entry and cell-exit t.
const char* const kHeightfieldMarchDepthCompute = R"GLSL(
#version 450 core
layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer Dirs  { vec4  dirs[]; };
layout(std430, binding = 1) readonly buffer HfBuf { float hf[]; };
layout(std430, binding = 2) readonly buffer MipBuf{ float mip[]; };
layout(std430, binding = 3) writeonly buffer Out  { float outt[]; };

uniform vec3  u_eye;
uniform int   u_n;
uniform float u_step;
uniform vec2  u_origin;   // ox, oz
uniform float u_tmax;
uniform int   u_rayCount;
uniform int   u_levels;
uniform int   u_maxSteps;
uniform int   u_mipOffset[24];
uniform int   u_mipDim[24];

float hfSample(float wx, float wz) {
    float fx = clamp((wx - u_origin.x) / u_step, 0.0, float(u_n - 1));
    float fz = clamp((wz - u_origin.y) / u_step, 0.0, float(u_n - 1));
    int x0 = int(floor(fx));
    int z0 = int(floor(fz));
    int x1 = min(x0 + 1, u_n - 1);
    int z1 = min(z0 + 1, u_n - 1);
    float tx = fx - float(x0);
    float tz = fz - float(z0);
    float h00 = hf[z0 * u_n + x0];
    float h10 = hf[z0 * u_n + x1];
    float h01 = hf[z1 * u_n + x0];
    float h11 = hf[z1 * u_n + x1];
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

// Distance along the ray (XZ) from world (px,pz) to the exit boundary of the
// level-L cell it sits in. Always > 0.
float cellExitDist(int L, float px, float pz, float dx, float dz) {
    float cs = u_step * float(1 << L);
    float lx = (px - u_origin.x) / cs;
    float lz = (pz - u_origin.y) / cs;
    int cx = int(floor(lx));
    int cz = int(floor(lz));
    float tx = 1.0e30;
    float tz = 1.0e30;
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

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(u_rayCount)) return;
    vec3 o = u_eye;
    vec3 d = dirs[i].xyz;
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
            px > u_origin.x + ext + base_cell || pz > u_origin.y + ext + base_cell) {
            break;
        }
        float exit_d = cellExitDist(L, px, pz, d.x, d.z);
        float t_exit = t + exit_d;
        float py_exit = o.y + d.y * t_exit;
        float cmax = cellMaxAt(L, px, pz);
        // Above the whole cell over [t, t_exit] (py linear -> endpoints suffice)?
        if (py > cmax && py_exit > cmax) {
            t = t_exit + eps;
            if (L < u_levels - 1) ++L;   // climb for bigger empty-space jumps
            continue;
        }
        if (L > 0) { --L; continue; }    // terrain may be here: refine
        // Finest level: resolve any crossing within [t, t_exit] by bisection.
        float surf_t = hfSample(px, pz);
        if (py - surf_t <= 0.0) { hit_t = t; break; }
        float px_e = o.x + d.x * t_exit;
        float pz_e = o.z + d.z * t_exit;
        if (py_exit - hfSample(px_e, pz_e) <= 0.0) {
            float lo = t;
            float hi = t_exit;
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
        // No crossing in this finest cell: advance past it, climb.
        t = t_exit + eps;
        if (L < u_levels - 1) ++L;
    }
    outt[i] = hit_t;
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

} // namespace

TEST(ShieldRtFarFieldParityGpu, HeightfieldMarchHitsAnalyticGroundTruth) {
    HiddenGlContext ctx("shieldrt_far_field_parity_gpu");
    if (!ctx.ready()) {
        GTEST_SKIP() << "no GL context (headless): " << ctx.error();
    }

    const std::string renderer = GlString(GL_RENDERER);
    const std::string gl_version = GlString(GL_VERSION);
    const bool software = IsSoftwareRenderer(renderer);

    std::string compile_error;
    const GLuint prog = CompileComputeProgram(kHeightfieldMarchDepthCompute, compile_error);
    ASSERT_NE(prog, 0u) << compile_error;

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
    constexpr int kFarStep = 4; // F1 tier sample step (meters)

    nlohmann::json results = nlohmann::json::array();
    double worst_self = 0.0;
    double worst_median_gt = 0.0;
    double worst_p99_gt = 0.0;

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

        const double terrain_at_center = hf.sample(cx, cz);
        View view;
        view.name = vs.name;
        view.preset = vs.preset;
        view.grazing = vs.grazing;
        if (vs.grazing) {
            view.eye = {cx - kBlockSpan * 0.5, terrain_at_center + 30.0, cz - kBlockSpan * 0.5};
            view.forward = Normalize({1.0, -0.04, 1.0});
        } else {
            view.eye = {cx - kBlockSpan * 0.4, hf.max_h + 250.0, cz - kBlockSpan * 0.4};
            view.forward = Normalize({1.0, -0.6, 1.0});
        }
        const std::vector<Vec3d> dirs = BuildRayDirs(view);
        const int ray_count = static_cast<int>(dirs.size());
        const double t_max = kBlockSpan * 1.8;

        std::vector<float> dir4(static_cast<std::size_t>(ray_count) * 4, 0.0f);
        for (int i = 0; i < ray_count; ++i) {
            dir4[i * 4 + 0] = static_cast<float>(dirs[i].x);
            dir4[i * 4 + 1] = static_cast<float>(dirs[i].y);
            dir4[i * 4 + 2] = static_cast<float>(dirs[i].z);
        }
        const GLuint dirs_buf =
            MakeStorageBuffer(static_cast<GLsizeiptr>(dir4.size() * sizeof(float)), dir4.data());
        const GLuint hf_buf =
            MakeStorageBuffer(static_cast<GLsizeiptr>(hf.h.size() * sizeof(float)), hf.h.data());
        const FlatMaxMip flat_mip = FlattenMaxMip(hmip);
        const GLuint mip_buf = MakeStorageBuffer(
            static_cast<GLsizeiptr>(flat_mip.data.size() * sizeof(float)), flat_mip.data.data());
        const GLuint out_buf =
            MakeStorageBuffer(static_cast<GLsizeiptr>(ray_count) * sizeof(float), nullptr);

        glUseProgram(prog);
        glUniform3f(glGetUniformLocation(prog, "u_eye"),
                    static_cast<float>(view.eye.x),
                    static_cast<float>(view.eye.y),
                    static_cast<float>(view.eye.z));
        glUniform1i(glGetUniformLocation(prog, "u_n"), hf.n);
        glUniform1f(glGetUniformLocation(prog, "u_step"), static_cast<float>(hf.step));
        glUniform2f(glGetUniformLocation(prog, "u_origin"),
                    static_cast<float>(hf.ox),
                    static_cast<float>(hf.oz));
        glUniform1f(glGetUniformLocation(prog, "u_tmax"), static_cast<float>(t_max));
        glUniform1i(glGetUniformLocation(prog, "u_rayCount"), ray_count);
        glUniform1i(glGetUniformLocation(prog, "u_levels"), flat_mip.levels);
        glUniform1i(glGetUniformLocation(prog, "u_maxSteps"), kMaxMarchSteps);
        SetIntArray(prog, "u_mipOffset", flat_mip.offset.data(), flat_mip.levels);
        SetIntArray(prog, "u_mipDim", flat_mip.dim.data(), flat_mip.levels);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, dirs_buf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, hf_buf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, mip_buf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, out_buf);

        const GLuint groups = static_cast<GLuint>((ray_count + kLocalSizeX - 1) / kLocalSizeX);
        glDispatchCompute(groups, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        glFinish();

        std::vector<float> hit_t(static_cast<std::size_t>(ray_count), -1.0f);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, out_buf);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                           0,
                           static_cast<GLsizeiptr>(hit_t.size() * sizeof(float)),
                           hit_t.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        // Per-ray correctness against ground truth.
        long long hits = 0;
        double self_max = 0.0;
        std::vector<double> gt_abs;
        gt_abs.reserve(static_cast<std::size_t>(ray_count));
        for (int i = 0; i < ray_count; ++i) {
            if (hit_t[i] < 0.0f)
                continue;
            ++hits;
            const double t = static_cast<double>(hit_t[i]);
            const double hx = view.eye.x + dirs[i].x * t;
            const double hy = view.eye.y + dirs[i].y * t;
            const double hz = view.eye.z + dirs[i].z * t;

            // Leg 1: hit lies on the bilinear FarLodStore surface it marched.
            const double self_delta = std::abs(hy - hf.sample(hx, hz));
            self_max = std::max(self_max, self_delta);

            // Leg 2: hit height vs the analytic generator that produced the tiles.
            const double analytic = static_cast<double>(world.GetTerrainHeightAtCoarse(
                static_cast<float>(hx), static_cast<float>(hz), kFarStep));
            gt_abs.push_back(std::abs(hy - analytic));
        }

        const double hit_fraction = static_cast<double>(hits) / ray_count;
        double gt_median = 0.0, gt_p99 = 0.0, gt_max = 0.0;
        if (!gt_abs.empty()) {
            gt_median = Percentile(gt_abs, 0.50);
            gt_p99 = Percentile(gt_abs, 0.99);
            gt_max = *std::max_element(gt_abs.begin(), gt_abs.end());
        }

        worst_self = std::max(worst_self, self_max);
        worst_median_gt = std::max(worst_median_gt, gt_median);
        worst_p99_gt = std::max(worst_p99_gt, gt_p99);

        results.push_back({
            {"view", vs.name},
            {"preset", vs.preset},
            {"grazing", vs.grazing},
            {"rays", ray_count},
            {"hits", hits},
            {"hit_fraction", hit_fraction},
            {"self_consistency_max_m", self_max},
            {"ground_truth_median_m", gt_median},
            {"ground_truth_p99_m", gt_p99},
            {"ground_truth_max_m", gt_max},
        });

        EXPECT_GT(hit_fraction, kMinHitFraction) << vs.name << ": march barely hit terrain";
        EXPECT_LT(hit_fraction, kMaxHitFraction)
            << vs.name << ": march hit ~everything (degenerate)";
        EXPECT_LE(self_max, kSelfConsistencyToleranceM)
            << vs.name << ": march hit is off its own heightfield surface (overshoot/degenerate)";
        EXPECT_LE(gt_median, kGroundTruthMedianToleranceM)
            << vs.name << ": median hit height drifts from analytic ground truth";
        EXPECT_LE(gt_p99, kGroundTruthP99ToleranceM)
            << vs.name << ": p99 hit height drifts from analytic ground truth";

        for (GLuint b : {dirs_buf, hf_buf, mip_buf, out_buf})
            glDeleteBuffers(1, &b);
    }

    jobs.shutdown();
    glDeleteProgram(prog);

#ifdef NDEBUG
    const char* build_mode = "release";
#else
    const char* build_mode = "debug";
#endif

    const nlohmann::json report = {
        {"schema", "luminumbra.shieldrt_far_field_parity.v1"},
        {"generated_by",
         "shieldrt_far_field_parity_gpu (heightfield max-mip march vs analytic ground truth)"},
        {"seed", kSeed},
        {"build_mode", build_mode},
        {"gpu",
         {{"renderer", renderer}, {"gl_version", gl_version}, {"software_renderer", software}}},
        {"far_tier_step_m", kFarStep},
        {"tolerances",
         {
             {"self_consistency_m", kSelfConsistencyToleranceM},
             {"ground_truth_median_m", kGroundTruthMedianToleranceM},
             {"ground_truth_p99_m", kGroundTruthP99ToleranceM},
         }},
        {"worst",
         {
             {"self_consistency_m", worst_self},
             {"ground_truth_median_m", worst_median_gt},
             {"ground_truth_p99_m", worst_p99_gt},
         }},
        {"views", results},
        {"note",
         "Validates the standalone heightfield max-mip march against the real surface. Leg 1 "
         "(self-consistency) catches the SDF-style degenerate false-hit; leg 2 "
         "(ground truth vs GetTerrainHeightAtCoarse) is the load-bearing "
         "correctness assertion within a quantization-aware tolerance."},
    };
    std::ofstream out(ArtifactRoot() / "shieldrt-far-field-parity.json");
    ASSERT_TRUE(out);
    out << std::setw(2) << report << "\n";

    if (software) {
        GTEST_SKIP() << "software renderer (" << renderer
                     << "): float-precision parity not representative; artifact written for record";
    }
}
