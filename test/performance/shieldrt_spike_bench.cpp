//   spike benchmark (EVIDENCE ONLY — one benchmark round).
//
// Compares two candidate far-field rendering source representations against the
// SAME terrain region, both driven from FarLodStore tiles:
//
//   (A) Heightfield ray-marching of the packed FarLodStore tiles directly,
//       accelerated by a max-mip pyramid over the tile heights (a max-mip is the
//       conservative acceleration structure for the "ray above terrain" test:
//       within a cell a ray is guaranteed to be above the surface while its Y
//       exceeds the cell's MAX height, so we descend the quadtree and DDA-step
//       by the safe vertical/horizontal margin). This is the iq-style terrain
//       march with quadtree-min descent the research (Area 3, takeaway 5) calls
//       the cheaper baseline for terrain-dominated far fields.
//
//   (B) Sphere tracing a mip-mapped SDF volume built from the same terrain
//       region. Coarse mips MUST be conservative lower bounds on the true
//       distance (min-style filtering) or sphere tracing silently overshoots the
//       surface (research Area 3, takeaway 3). We build BOTH a conservative
//       (min-filtered) mip chain AND a naive (average-filtered) mip chain and
//       count the surface MISSES the naive chain produces — the spike's required
//       correctness evidence.
//
// Measures, per representative far-field view (default + mountains presets,
// horizon-grazing and elevated rays):
//   - per-ray step counts (mean) for A, B-conservative, B-naive
//   - total ms for a 320x180 ray grid (median of 3 runs)
//   - memory bytes for each structure
//   - naive-mip surface-miss count (correctness)
//
// CPU prototype on purpose: this measures the *traversal cost shape and the
// conservative-mip correctness criterion*, which are representation-level facts
// independent of CPU-vs-GPU. The research notes a production build would be
// GPU-resident; that is an  concern, not what this round decides.
//
// Render-only: touches NO world_hash / determinism contract (research takeaway
// 7). Registered in ctest under a manual label (runtime can exceed 60s in Debug).

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/JobSystem.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/FarLodStore.h"
#include "world/TerrainPresetLoader.h"

// Shared far-field data structures + builders (heightfield/SDF/views). The GPU
// micro-profile (shieldrt_tracer_profile_gpu.cpp) consumes the SAME header so
// both benches measure byte-identical structures over the same terrain.
#include "shieldrt_far_field.h"

namespace fs = std::filesystem;

using namespace Luminumbra;
using namespace Luminumbra::Systems;
using namespace luminumbra_shieldrt;
using Luminumbra::World::BuildPristineFarLodTile;
using Luminumbra::World::ComputeTerrainParamsHash;
using Luminumbra::World::DequantizeFarLodHeight;
using Luminumbra::World::FarLodSampleStepMeters;
using Luminumbra::World::FarLodTier;
using Luminumbra::World::FarLodTile;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif
#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

constexpr int kSeed = 424242;
constexpr int kRunsPerView = 3; // median of 3

struct ScopedJobSystem {
    ScopedJobSystem() {
        jobs.startup();
    }
    ~ScopedJobSystem() {
        jobs.shutdown();
    }
    JobSystem jobs;
};

struct Timer {
    using Clock = std::chrono::steady_clock;
    Timer()
        : start(Clock::now()) {}
    double elapsed_ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    Clock::time_point start;
};

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

double Median3(std::array<double, kRunsPerView> v) {
    std::sort(v.begin(), v.end());
    return v[1];
}

struct MarchResult {
    bool hit = false;
    int steps = 0;
    double t = 0.0;
};

// Quadtree-accelerated heightfield march. At each step we read the coarsest mip
// cell whose max-height the ray is currently above and advance to the far edge
// of that cell (a safe skip — the ray cannot intersect terrain inside it). When
// the ray's Y is at/below the finest cell's max we step finely and test the
// bilinear surface for the crossing. Step budget is fixed (Claybook policy).
MarchResult MarchHeightfield(const HeightField& hf,
                             const HeightMaxMip& mip,
                             const Vec3d& origin,
                             const Vec3d& dir,
                             double t_max) {
    MarchResult r;
    double t = 0.0;
    const double base_cell = hf.step; // finest cell size in meters
    double prev_diff = origin.y - hf.sample(origin.x, origin.z);
    while (t < t_max && r.steps < kMaxMarchSteps) {
        ++r.steps;
        const double px = origin.x + dir.x * t;
        const double py = origin.y + dir.y * t;
        const double pz = origin.z + dir.z * t;

        // Out of the field domain in XZ -> miss (far field bounded).
        if (px < hf.ox - base_cell || pz < hf.oz - base_cell ||
            px > hf.ox + hf.extent() + base_cell || pz > hf.oz + hf.extent() + base_cell) {
            return r;
        }

        // Choose a mip level: the coarser the level whose max-height we clear,
        // the bigger the safe horizontal skip. Find the coarsest level whose
        // cell max is below py.
        int level = 0;
        double cell_max = py + 1.0;
        for (int L = mip.levels - 1; L >= 0; --L) {
            const double lvl_cell = base_cell * static_cast<double>(1 << L);
            const int dim = mip.dims[L];
            int cx = static_cast<int>(std::floor((px - hf.ox) / lvl_cell));
            int cz = static_cast<int>(std::floor((pz - hf.oz) / lvl_cell));
            cx = std::clamp(cx, 0, dim - 1);
            cz = std::clamp(cz, 0, dim - 1);
            const float mx = mip.max_h[L][static_cast<std::size_t>(cz) * dim + cx];
            if (py > mx) { // safely above this whole cell
                level = L;
                cell_max = mx;
                break;
            }
        }

        const double surf = hf.sample(px, pz);
        const double diff = py - surf;
        if (diff <= 0.0) {
            // Crossed the surface this step: refine the hit (bisection on t).
            double lo = std::max(0.0, t - base_cell);
            double hi = t;
            for (int it = 0; it < 12; ++it) {
                const double tm = 0.5 * (lo + hi);
                const double mx = origin.x + dir.x * tm;
                const double my = origin.y + dir.y * tm;
                const double mz = origin.z + dir.z * tm;
                if (my - hf.sample(mx, mz) <= 0.0)
                    hi = tm;
                else
                    lo = tm;
            }
            r.hit = true;
            r.t = hi;
            return r;
        }
        prev_diff = diff;

        // Advance: safe skip = the horizontal span of the chosen mip cell along
        // the ray, clamped so we never overshoot the vertical clearance margin.
        const double lvl_cell = base_cell * static_cast<double>(1 << level);
        double advance = lvl_cell;
        if (level == 0) {
            // Near the surface: limit the step to the vertical clearance so we
            // do not tunnel through a thin ridge (heightfield-safe step).
            const double v_margin = py - cell_max;
            const double slope_guard = std::max(0.25, std::abs(dir.y));
            advance = std::min(advance, std::max(base_cell * 0.5, v_margin / slope_guard));
        }
        t += std::max(advance, base_cell * 0.5);
    }
    return r;
}

// Nearest-sample distance read at a given mip level.
float SampleMip(const SdfMipChain& c, int L, double wx, double wy, double wz) {
    const double s = c.level_step(L);
    int x = static_cast<int>(std::floor((wx - c.ox) / s));
    int y = static_cast<int>(std::floor((wy - c.oy) / s));
    int z = static_cast<int>(std::floor((wz - c.oz) / s));
    x = std::clamp(x, 0, c.nx[L] - 1);
    y = std::clamp(y, 0, c.ny[L] - 1);
    z = std::clamp(z, 0, c.nz[L] - 1);
    return c
        .levels[L][static_cast<std::size_t>(x) +
                   c.nx[L] * (static_cast<std::size_t>(y) + c.ny[L] * static_cast<std::size_t>(z))];
}

// Sphere trace a mip SDF (coarse-mip-first: pick a mip level by distance so far
// rays read cheap coarse levels — Claybook policy). Returns hit + step count.
// `truth` (the dense heightfield) lets us detect overshoot: an SDF step that
// strides THROUGH the true surface because the sampled distance exceeded the
// real clearance — the sphere-tracing correctness failure.
//
// We classify each overshoot by the mip level of the step that caused it:
//   - mip_overshot: the offending step read a COARSE level (L>0) — this is the
//     mip-filtering error the spike is about (naive average mips overshoot).
//   - base_overshot: the offending step read the base level (L==0) — this is
//     base grid-resolution tunneling on a thin grazing ridge, a property of any
//     sampled-grid SDF independent of the mip-filtering question.
struct SphereTraceResult {
    bool hit = false;
    int steps = 0;
    double t = 0.0;
    bool mip_overshot = false;
    bool base_overshot = false;
};

SphereTraceResult SphereTrace(const SdfMipChain& c,
                              const HeightField& truth,
                              const Vec3d& origin,
                              const Vec3d& dir,
                              double t_max) {
    SphereTraceResult r;
    double t = 0.0;
    const double surf_eps = c.base_step * 0.5;
    const int max_level = static_cast<int>(c.levels.size()) - 1;
    auto true_clearance = [&](double tt) {
        const double px = origin.x + dir.x * tt;
        const double py = origin.y + dir.y * tt;
        const double pz = origin.z + dir.z * tt;
        return py - truth.sample(px, pz);
    };
    while (t < t_max && r.steps < kMaxMarchSteps) {
        ++r.steps;
        const double px = origin.x + dir.x * t;
        const double py = origin.y + dir.y * t;
        const double pz = origin.z + dir.z * t;
        if (px < c.ox || pz < c.oz || px > c.ox + truth.extent() || pz > c.oz + truth.extent()) {
            return r;
        }
        // Coarse-mip-first: deeper (coarser) level the farther we are along t.
        const int L =
            std::clamp(static_cast<int>(std::log2(1.0 + t / (32.0 * c.base_step))), 0, max_level);
        const double d = SampleMip(c, L, px, py, pz);
        if (d < surf_eps) {
            r.hit = true;
            r.t = t;
            return r;
        }
        // Take the sphere-tracing step, then check whether it strode through the
        // true surface. If the clearance flipped from clearly-above to clearly-
        // below across this single step, the read distance `d` overshot.
        const double clear_before = true_clearance(t);
        const double step = std::max(static_cast<double>(d), c.base_step * 0.25);
        const double t_next = t + step;
        const double clear_after = true_clearance(t_next);
        if (clear_before > surf_eps && clear_after < -c.base_step) {
            if (L > 0)
                r.mip_overshot = true;
            else
                r.base_overshot = true;
            // A correct tracer would have stopped at the crossing; report the hit
            // there but flag the overshoot for the correctness tally.
            r.hit = true;
            r.t = t_next;
            return r;
        }
        t = t_next;
    }
    return r;
}

} // namespace

TEST(ShieldRtSpike, HeightfieldMarchVsSdfSphereTrace) {
    fs::create_directories(ArtifactRoot());

    // 3 region block (~1536 m, the far-tile horizon) at the  (4 m) tier, the
    // far band the research targets. Origin offset away from spawn so we sample
    // genuine far terrain.
    constexpr i32 kRx0 = 4, kRz0 = 4, kRegions = 3;
    constexpr double kBlockSpan = kRegions * 512.0;
    const double cx = (kRx0 + kRegions * 0.5) * 512.0;
    const double cz = (kRz0 + kRegions * 0.5) * 512.0;

    struct ViewSpec {
        const char* name;
        const char* preset;
        bool grazing;
    };
    const std::array<ViewSpec, 4> view_specs{{
        {"default_grazing", "default.json", true},
        {"default_elevated", "default.json", false},
        {"mountains_grazing", "mountains.json", true},
        {"mountains_elevated", "mountains.json", false},
    }};

    nlohmann::json results = nlohmann::json::array();
    std::vector<std::string> miss_lines;
    double max_variance_pct = 0.0;

    ScopedJobSystem jobs;

    for (const ViewSpec& vs : view_specs) {
        const fs::path preset_path = SourceRoot() / "worlds/atlas/presets" / vs.preset;
        ASSERT_TRUE(fs::exists(preset_path)) << preset_path.string();
        const TerrainGenParams params = LoadPresetParams(preset_path);
        SHIELD_WorldSystem world(&jobs.jobs, nullptr, params, kSeed);
        const u64 params_hash = ComputeTerrainParamsHash(params, kSeed);

        // Build the shared far heightfield once per view (NOT timed — this is the
        // generation cost, separate from the per-frame raymarch cost we compare).
        const HeightField hf =
            BuildHeightFieldFromTiles(world, FarLodTier::F1, kRx0, kRz0, kRegions, params_hash);

        // Path A structure.
        const HeightMaxMip hmip = BuildHeightMaxMip(hf);

        // Path B structures. Voxel step = 2x the heightfield step keeps the SDF
        // volume tractable in Debug while staying a faithful far-field grid.
        const double voxel_step = hf.step * 2.0;
        const SdfVolume sdf = BuildSdfVolume(hf, voxel_step);
        const SdfMipChain sdf_cons = BuildSdfMips(sdf, /*conservative=*/true);
        const SdfMipChain sdf_naive = BuildSdfMips(sdf, /*conservative=*/false);

        // Camera: looking across the block toward its center. Grazing rays sit
        // just above the surface and skim the horizon; elevated rays look down.
        const double terrain_at_center = hf.sample(cx, cz);
        View view;
        view.name = vs.name;
        view.preset = vs.preset;
        view.grazing = vs.grazing;
        if (vs.grazing) {
            view.eye = {cx - kBlockSpan * 0.5, terrain_at_center + 30.0, cz - kBlockSpan * 0.5};
            view.forward = Normalize({1.0, -0.04, 1.0}); // near-horizontal
        } else {
            view.eye = {cx - kBlockSpan * 0.4, hf.max_h + 250.0, cz - kBlockSpan * 0.4};
            view.forward = Normalize({1.0, -0.6, 1.0}); // looking down
        }
        const std::vector<Vec3d> dirs = BuildRayDirs(view);
        const double t_max = kBlockSpan * 1.8;

        // --- Path A timing (median of 3) ---
        std::array<double, kRunsPerView> a_ms{};
        long long a_steps_total = 0;
        long long a_hits = 0;
        for (int run = 0; run < kRunsPerView; ++run) {
            long long steps = 0, hits = 0;
            Timer t;
            for (const Vec3d& d : dirs) {
                const MarchResult m = MarchHeightfield(hf, hmip, view.eye, d, t_max);
                steps += m.steps;
                if (m.hit)
                    ++hits;
            }
            a_ms[run] = t.elapsed_ms();
            a_steps_total = steps;
            a_hits = hits;
        }

        // --- Path B conservative timing (median of 3) ---
        std::array<double, kRunsPerView> bc_ms{};
        long long bc_steps_total = 0, bc_hits = 0, bc_mip_miss = 0, bc_base_miss = 0;
        for (int run = 0; run < kRunsPerView; ++run) {
            long long steps = 0, hits = 0, mip = 0, base = 0;
            Timer t;
            for (const Vec3d& d : dirs) {
                const SphereTraceResult s = SphereTrace(sdf_cons, hf, view.eye, d, t_max);
                steps += s.steps;
                if (s.hit)
                    ++hits;
                if (s.mip_overshot)
                    ++mip;
                if (s.base_overshot)
                    ++base;
            }
            bc_ms[run] = t.elapsed_ms();
            bc_steps_total = steps;
            bc_hits = hits;
            bc_mip_miss = mip;
            bc_base_miss = base;
        }

        // --- Path B naive timing + miss count (median of 3) ---
        std::array<double, kRunsPerView> bn_ms{};
        long long bn_steps_total = 0, bn_hits = 0, bn_mip_miss = 0, bn_base_miss = 0;
        for (int run = 0; run < kRunsPerView; ++run) {
            long long steps = 0, hits = 0, mip = 0, base = 0;
            Timer t;
            for (const Vec3d& d : dirs) {
                const SphereTraceResult s = SphereTrace(sdf_naive, hf, view.eye, d, t_max);
                steps += s.steps;
                if (s.hit)
                    ++hits;
                if (s.mip_overshot)
                    ++mip;
                if (s.base_overshot)
                    ++base;
            }
            bn_ms[run] = t.elapsed_ms();
            bn_steps_total = steps;
            bn_hits = hits;
            bn_mip_miss = mip;
            bn_base_miss = base;
        }

        const std::size_t total_rays = dirs.size();
        const double a_med = Median3(a_ms);
        const double bc_med = Median3(bc_ms);
        const double bn_med = Median3(bn_ms);

        auto variance_pct = [](const std::array<double, kRunsPerView>& v) {
            const double mn = *std::min_element(v.begin(), v.end());
            const double mx = *std::max_element(v.begin(), v.end());
            return mn > 0.0 ? (mx - mn) / mn * 100.0 : 0.0;
        };
        const double view_var =
            std::max({variance_pct(a_ms), variance_pct(bc_ms), variance_pct(bn_ms)});
        max_variance_pct = std::max(max_variance_pct, view_var);

        const std::string miss_line =
            std::string(vs.name) +
            ": naive-mip surface misses (overshoot) = " + std::to_string(bn_mip_miss) + " of " +
            std::to_string(total_rays) +
            " rays; conservative-mip misses = " + std::to_string(bc_mip_miss) +
            " (base-grid tunneling, both chains: naive=" + std::to_string(bn_base_miss) +
            " conservative=" + std::to_string(bc_base_miss) + ")";
        miss_lines.push_back(miss_line);

        results.push_back({
            {"view", vs.name},
            {"preset", vs.preset},
            {"grazing", vs.grazing},
            {"rays", total_rays},
            {"ray_grid", {{"w", kRayGridW}, {"h", kRayGridH}}},
            {"heightfield",
             {
                 {"samples_per_side", hf.n},
                 {"step_m", hf.step},
                 {"min_h", hf.min_h},
                 {"max_h", hf.max_h},
                 {"bytes", hf.bytes()},
             }},
            {"path_a_heightfield_march",
             {
                 {"max_mip_levels", hmip.levels},
                 {"accel_bytes", hmip.bytes()},
                 {"total_bytes", hf.bytes() + hmip.bytes()},
                 {"ms_runs", {a_ms[0], a_ms[1], a_ms[2]}},
                 {"ms_median", a_med},
                 {"mean_steps_per_ray", static_cast<double>(a_steps_total) / total_rays},
                 {"hits", a_hits},
                 {"variance_pct", variance_pct(a_ms)},
             }},
            {"path_b_sdf_sphere_trace",
             {
                 {"voxel_step_m", voxel_step},
                 {"volume_dims", {{"nx", sdf.nx}, {"ny", sdf.ny}, {"nz", sdf.nz}}},
                 {"base_volume_bytes", sdf.bytes()},
                 {"conservative",
                  {
                      {"mip_levels", sdf_cons.levels.size()},
                      {"total_bytes", sdf_cons.bytes()},
                      {"ms_runs", {bc_ms[0], bc_ms[1], bc_ms[2]}},
                      {"ms_median", bc_med},
                      {"mean_steps_per_ray", static_cast<double>(bc_steps_total) / total_rays},
                      {"hits", bc_hits},
                      {"surface_misses_mip", bc_mip_miss},
                      {"surface_misses_base_grid", bc_base_miss},
                      {"variance_pct", variance_pct(bc_ms)},
                  }},
                 {"naive",
                  {
                      {"mip_levels", sdf_naive.levels.size()},
                      {"total_bytes", sdf_naive.bytes()},
                      {"ms_runs", {bn_ms[0], bn_ms[1], bn_ms[2]}},
                      {"ms_median", bn_med},
                      {"mean_steps_per_ray", static_cast<double>(bn_steps_total) / total_rays},
                      {"hits", bn_hits},
                      {"surface_misses_mip", bn_mip_miss},
                      {"surface_misses_base_grid", bn_base_miss},
                      {"variance_pct", variance_pct(bn_ms)},
                  }},
             }},
        });

        // The far field MUST actually be hit by a substantial fraction of rays,
        // or the benchmark is measuring empty sky.
        EXPECT_GT(a_hits, static_cast<long long>(total_rays / 20))
            << vs.name << ": heightfield march barely hit terrain";
    }

#ifdef NDEBUG
    const char* build_mode = "release";
#else
    const char* build_mode = "debug";
#endif

    const bool contended = max_variance_pct > 25.0;
    const nlohmann::json report = {
        {"schema", "luminumbra.shieldrt_spike.v1"},
        {"seed", kSeed},
        {"build_mode", build_mode},
        {"ray_grid", {{"w", kRayGridW}, {"h", kRayGridH}}},
        {"runs_per_view", kRunsPerView},
        {"median_policy", "median of 3 runs per path per view"},
        {"contended_machine", contended},
        {"max_wallclock_variance_pct", max_variance_pct},
        {"correctness_note",
         "surface_misses_mip = sphere-trace steps that strode THROUGH the surface "
         "while reading a COARSE mip level (the mip-filtering error the spike is "
         "about): naive (average) mips overshoot, conservative (min-magnitude) "
         "mips must report ~0. surface_misses_base_grid = base-resolution "
         "tunneling on thin grazing ridges, a property of any sampled-grid SDF "
         "independent of mip filtering (reported but not the spike's variable)."},
        {"views", results},
        {"naive_mip_miss_lines", miss_lines},
    };
    std::ofstream out(ArtifactRoot() / "shieldrt_spike.json");
    ASSERT_TRUE(out);
    out << std::setw(2) << report << "\n";

    // Correctness criterion: across all views the naive (average) mip chain must
    // produce STRICTLY MORE mip-attributable surface misses than the conservative
    // (min-magnitude) chain — the spike's required demonstration of mip overshoot.
    long long total_naive_miss = 0, total_cons_miss = 0;
    for (const auto& v : results) {
        total_naive_miss +=
            v["path_b_sdf_sphere_trace"]["naive"]["surface_misses_mip"].get<long long>();
        total_cons_miss +=
            v["path_b_sdf_sphere_trace"]["conservative"]["surface_misses_mip"].get<long long>();
    }
    EXPECT_GT(total_naive_miss, total_cons_miss)
        << "naive mips must demonstrably overshoot more than conservative mips";
    EXPECT_GT(total_naive_miss, 0) << "spike requires demonstrating naive-mip surface misses";
}
