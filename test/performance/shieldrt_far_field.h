//  far-field shared builders ( spike +  GPU profile).
//
// The  CPU spike (shieldrt_spike_bench.cpp) and the  GPU
// micro-profile (shieldrt_tracer_profile_gpu.cpp) BOTH compare the same two
// candidate far-field source representations against the SAME FarLodStore-derived
// terrain region:
//
//   (A) heightfield max-mip pyramid + quadtree-min-descent march, and
//   (B) a mip-mapped SDF volume (conservative min-magnitude mips) + sphere trace.
//
// To keep the two benches honest — same terrain, same camera rig, same ray grid,
// byte-identical acceleration structures — the data structures and builders live
// here, in ONE place. The CPU spike owns the traversal kernels and the
// conservative-mip correctness tally; the GPU profile re-implements the same
// kernels as compute shaders and times them on real hardware (GL timestamp
// queries). Neither bench touches world_hash / the determinism contract.
//
// Header-only, no gtest dependency, depends only on luminumbra_common worldgen.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "systems/SHIELD_WorldSystem.h"
#include "world/FarLodStore.h"

namespace luminumbra_shieldrt {

constexpr int kRayGridW = 320;
constexpr int kRayGridH = 180;
constexpr int kMaxMarchSteps = 512;

struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;
};

inline Vec3d Normalize(const Vec3d& v) {
    const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 0.0)
        return v;
    return {v.x / len, v.y / len, v.z / len};
}

// ---------------------------------------------------------------------------
// A dense, region-local height field assembled from FarLodStore tiles. The far
// representation in the engine IS a packed heightfield (4 m F1 / 8 m F2 samples)
// both raymarch paths sample exactly this data so the comparison is apples to
// apples. The grid is (n x n) samples at `step` meters, origin at (ox, oz).
// ---------------------------------------------------------------------------
struct HeightField {
    int n = 0;                 // samples per side
    double step = 0.0;         // meters between samples
    double ox = 0.0, oz = 0.0; // world origin of sample (0,0)
    std::vector<float> h;      // row-major n*n
    double min_h = 0.0, max_h = 0.0;

    double world_x(int i) const {
        return ox + i * step;
    }
    double world_z(int j) const {
        return oz + j * step;
    }
    double extent() const {
        return (n - 1) * step;
    }

    // Bilinear height sample in world XZ, clamped to the field domain.
    double sample(double wx, double wz) const {
        double fx = (wx - ox) / step;
        double fz = (wz - oz) / step;
        fx = std::clamp(fx, 0.0, static_cast<double>(n - 1));
        fz = std::clamp(fz, 0.0, static_cast<double>(n - 1));
        const int x0 = static_cast<int>(std::floor(fx));
        const int z0 = static_cast<int>(std::floor(fz));
        const int x1 = std::min(x0 + 1, n - 1);
        const int z1 = std::min(z0 + 1, n - 1);
        const double tx = fx - x0;
        const double tz = fz - z0;
        const double h00 = h[z0 * n + x0];
        const double h10 = h[z0 * n + x1];
        const double h01 = h[z1 * n + x0];
        const double h11 = h[z1 * n + x1];
        const double a = h00 + (h10 - h00) * tx;
        const double b = h01 + (h11 - h01) * tx;
        return a + (b - a) * tz;
    }

    std::size_t bytes() const {
        return h.size() * sizeof(float);
    }
};

// Build a region-local height field by stitching FarLodStore tiles. We use the
// requested tier and assemble a `regions x regions` block. The samples come
// straight from BuildPristineFarLodTile so this is the real engine far-LOD data.
inline HeightField BuildHeightFieldFromTiles(const Luminumbra::Systems::SHIELD_WorldSystem& world,
                                             Luminumbra::World::FarLodTier tier,
                                             std::int32_t rx0,
                                             std::int32_t rz0,
                                             int regions,
                                             std::uint64_t params_hash) {
    const int step = Luminumbra::World::FarLodSampleStepMeters(tier);
    const int per_region = 512 / step;
    HeightField hf;
    hf.step = static_cast<double>(step);
    hf.n = per_region * regions + 1;
    hf.ox = static_cast<double>(rx0) * 512.0;
    hf.oz = static_cast<double>(rz0) * 512.0;
    hf.h.assign(static_cast<std::size_t>(hf.n) * hf.n, 0.0f);

    for (int rz = 0; rz < regions; ++rz) {
        for (int rx = 0; rx < regions; ++rx) {
            const Luminumbra::World::FarLodTile tile = Luminumbra::World::BuildPristineFarLodTile(
                world, tier, rx0 + rx, rz0 + rz, params_hash);
            const int sps = static_cast<int>(tile.samples_per_side);
            for (int z = 0; z < sps; ++z) {
                for (int x = 0; x < sps; ++x) {
                    const int gx = rx * per_region + x;
                    const int gz = rz * per_region + z;
                    if (gx >= hf.n || gz >= hf.n)
                        continue;
                    const float height =
                        Luminumbra::World::DequantizeFarLodHeight(tile.height_q[z * sps + x]);
                    hf.h[static_cast<std::size_t>(gz) * hf.n + gx] = height;
                }
            }
        }
    }

    hf.min_h = std::numeric_limits<double>::max();
    hf.max_h = std::numeric_limits<double>::lowest();
    for (float v : hf.h) {
        hf.min_h = std::min(hf.min_h, static_cast<double>(v));
        hf.max_h = std::max(hf.max_h, static_cast<double>(v));
    }
    return hf;
}

// ---------------------------------------------------------------------------
// PATH A acceleration structure: heightfield max-mip pyramid.
//
// A max-mip pyramid stores, per coarse cell, the MAX terrain height under it.
// This is the conservative structure for the ray-above-surface test: while a
// ray's Y exceeds a cell's max height it is guaranteed not to have hit terrain,
// so it can skip the whole cell.
// ---------------------------------------------------------------------------
struct HeightMaxMip {
    int levels = 0;
    int base_n = 0;
    double step = 0.0;
    double ox = 0.0, oz = 0.0;
    std::vector<int> dims;                 // dims[L] = cell count per side
    std::vector<std::vector<float>> max_h; // per level, row-major

    std::size_t bytes() const {
        std::size_t b = 0;
        for (const auto& lvl : max_h)
            b += lvl.size() * sizeof(float);
        return b;
    }
};

inline HeightMaxMip BuildHeightMaxMip(const HeightField& hf) {
    HeightMaxMip mip;
    mip.base_n = hf.n;
    mip.step = hf.step;
    mip.ox = hf.ox;
    mip.oz = hf.oz;
    int cells = hf.n - 1;
    std::vector<float> level0(static_cast<std::size_t>(cells) * cells);
    for (int cz = 0; cz < cells; ++cz) {
        for (int cx = 0; cx < cells; ++cx) {
            const float a = hf.h[cz * hf.n + cx];
            const float b = hf.h[cz * hf.n + cx + 1];
            const float c = hf.h[(cz + 1) * hf.n + cx];
            const float d = hf.h[(cz + 1) * hf.n + cx + 1];
            level0[static_cast<std::size_t>(cz) * cells + cx] =
                std::max(std::max(a, b), std::max(c, d));
        }
    }
    mip.dims.push_back(cells);
    mip.max_h.push_back(std::move(level0));

    while (cells > 1) {
        const int coarse = (cells + 1) / 2;
        std::vector<float> next(static_cast<std::size_t>(coarse) * coarse,
                                std::numeric_limits<float>::lowest());
        const std::vector<float>& prev = mip.max_h.back();
        for (int cz = 0; cz < cells; ++cz) {
            for (int cx = 0; cx < cells; ++cx) {
                const int nz = cz / 2, nx = cx / 2;
                float& dst = next[static_cast<std::size_t>(nz) * coarse + nx];
                dst = std::max(dst, prev[static_cast<std::size_t>(cz) * cells + cx]);
            }
        }
        mip.dims.push_back(coarse);
        mip.max_h.push_back(std::move(next));
        cells = coarse;
    }
    mip.levels = static_cast<int>(mip.max_h.size());
    return mip;
}

// ---------------------------------------------------------------------------
// PATH B structures: sparse SDF volume built from the heightfield + mip chains.
// ---------------------------------------------------------------------------
struct SdfVolume {
    int nx = 0, ny = 0, nz = 0;
    double step = 0.0;
    double ox = 0.0, oy = 0.0, oz = 0.0;
    std::vector<float> d; // signed distance, row-major (x + nx*(y + ny*z))

    std::size_t idx(int x, int y, int z) const {
        return static_cast<std::size_t>(x) +
               nx * (static_cast<std::size_t>(y) + ny * static_cast<std::size_t>(z));
    }
    std::size_t bytes() const {
        return d.size() * sizeof(float);
    }
};

// True conservative SDF from the heightfield: signed vertical gap scaled by the
// local Lipschitz factor so |sdf| <= true distance everywhere.
inline double HeightfieldSdf(const HeightField& hf, double wx, double wy, double wz) {
    const double surf = hf.sample(wx, wz);
    const double e = hf.step;
    const double dhx = (hf.sample(wx + e, wz) - hf.sample(wx - e, wz)) / (2.0 * e);
    const double dhz = (hf.sample(wx, wz + e) - hf.sample(wx, wz - e)) / (2.0 * e);
    const double lip = std::sqrt(1.0 + dhx * dhx + dhz * dhz);
    return (wy - surf) / lip; // negative below surface
}

inline SdfVolume BuildSdfVolume(const HeightField& hf, double voxel_step) {
    SdfVolume v;
    v.step = voxel_step;
    v.ox = hf.ox;
    v.oz = hf.oz;
    v.oy = hf.min_h - voxel_step * 2.0;
    const double span_xz = hf.extent();
    const double span_y = (hf.max_h - hf.min_h) + voxel_step * 4.0;
    v.nx = static_cast<int>(std::ceil(span_xz / voxel_step)) + 1;
    v.nz = v.nx;
    v.ny = std::max(2, static_cast<int>(std::ceil(span_y / voxel_step)) + 1);
    v.d.assign(static_cast<std::size_t>(v.nx) * v.ny * v.nz, 0.0f);
    for (int z = 0; z < v.nz; ++z) {
        const double wz = v.oz + z * voxel_step;
        for (int y = 0; y < v.ny; ++y) {
            const double wy = v.oy + y * voxel_step;
            for (int x = 0; x < v.nx; ++x) {
                const double wx = v.ox + x * voxel_step;
                v.d[v.idx(x, y, z)] = static_cast<float>(HeightfieldSdf(hf, wx, wy, wz));
            }
        }
    }
    return v;
}

struct SdfMipChain {
    bool conservative = false;
    std::vector<int> nx, ny, nz;
    double base_step = 0.0;
    double ox = 0.0, oy = 0.0, oz = 0.0;
    std::vector<std::vector<float>> levels; // [L] row-major

    std::size_t bytes() const {
        std::size_t b = 0;
        for (const auto& l : levels)
            b += l.size() * sizeof(float);
        return b;
    }
    double level_step(int L) const {
        return base_step * static_cast<double>(1 << L);
    }
};

// Build a mip chain. conservative=true -> coarse value is the signed distance of
// SMALLEST magnitude among the 8 children minus the half-diagonal of the coarse
// cell, guaranteeing it stays a lower bound on the true distance across the whole
// coarse cell. conservative=false -> plain average (the naive mip).
inline SdfMipChain BuildSdfMips(const SdfVolume& base, bool conservative) {
    SdfMipChain c;
    c.conservative = conservative;
    c.base_step = base.step;
    c.ox = base.ox;
    c.oy = base.oy;
    c.oz = base.oz;
    c.nx.push_back(base.nx);
    c.ny.push_back(base.ny);
    c.nz.push_back(base.nz);
    c.levels.push_back(base.d);

    int lx = base.nx, ly = base.ny, lz = base.nz;
    while (lx > 1 || ly > 1 || lz > 1) {
        const int cx = std::max(1, (lx + 1) / 2);
        const int cy = std::max(1, (ly + 1) / 2);
        const int cz = std::max(1, (lz + 1) / 2);
        const std::vector<float>& prev = c.levels.back();
        const int plx = lx, ply = ly;
        auto at = [&](int x, int y, int z) -> float {
            x = std::min(x, lx - 1);
            y = std::min(y, ly - 1);
            z = std::min(z, lz - 1);
            return prev[static_cast<std::size_t>(x) +
                        plx * (static_cast<std::size_t>(y) + ply * static_cast<std::size_t>(z))];
        };
        std::vector<float> next(static_cast<std::size_t>(cx) * cy * cz);
        const double coarse_cell = c.level_step(static_cast<int>(c.levels.size()));
        const double half_diag = 0.5 * coarse_cell * std::sqrt(3.0);
        for (int z = 0; z < cz; ++z) {
            for (int y = 0; y < cy; ++y) {
                for (int x = 0; x < cx; ++x) {
                    float vals[8];
                    int k = 0;
                    for (int dz = 0; dz < 2; ++dz)
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                                vals[k++] = at(2 * x + dx, 2 * y + dy, 2 * z + dz);
                    float out;
                    if (conservative) {
                        float best = vals[0];
                        for (int i = 1; i < 8; ++i)
                            if (std::abs(vals[i]) < std::abs(best))
                                best = vals[i];
                        const float sign = best < 0.0f ? -1.0f : 1.0f;
                        const float mag =
                            std::max(0.0f, std::abs(best) - static_cast<float>(half_diag));
                        out = sign * mag;
                    } else {
                        double avg = 0.0;
                        for (int i = 0; i < 8; ++i)
                            avg += vals[i];
                        out = static_cast<float>(avg / 8.0);
                    }
                    next[static_cast<std::size_t>(x) +
                         cx * (static_cast<std::size_t>(y) + cy * static_cast<std::size_t>(z))] =
                        out;
                }
            }
        }
        c.nx.push_back(cx);
        c.ny.push_back(cy);
        c.nz.push_back(cz);
        c.levels.push_back(std::move(next));
        lx = cx;
        ly = cy;
        lz = cz;
    }
    return c;
}

// ---------------------------------------------------------------------------
// View / camera setup. A 320x180 ray grid fired from a far-field vantage toward
// the terrain region.
// ---------------------------------------------------------------------------
struct View {
    std::string name;
    std::string preset;
    Vec3d eye;
    Vec3d forward;
    double fov_deg = 60.0;
    bool grazing = false;
};

inline std::vector<Vec3d> BuildRayDirs(const View& v) {
    std::vector<Vec3d> dirs;
    dirs.reserve(static_cast<std::size_t>(kRayGridW) * kRayGridH);
    const Vec3d fwd = Normalize(v.forward);
    const Vec3d world_up{0.0, 1.0, 0.0};
    Vec3d right = Normalize({fwd.z * world_up.y - fwd.y * world_up.z,
                             fwd.x * world_up.z - fwd.z * world_up.x,
                             fwd.y * world_up.x - fwd.x * world_up.y});
    Vec3d up{right.y * fwd.z - right.z * fwd.y,
             right.z * fwd.x - right.x * fwd.z,
             right.x * fwd.y - right.y * fwd.x};
    const double aspect = static_cast<double>(kRayGridW) / kRayGridH;
    const double th = std::tan(v.fov_deg * 0.5 * 3.14159265358979 / 180.0);
    for (int py = 0; py < kRayGridH; ++py) {
        const double ndc_y = (1.0 - 2.0 * (py + 0.5) / kRayGridH) * th;
        for (int px = 0; px < kRayGridW; ++px) {
            const double ndc_x = (2.0 * (px + 0.5) / kRayGridW - 1.0) * th * aspect;
            Vec3d d{fwd.x + right.x * ndc_x + up.x * ndc_y,
                    fwd.y + right.y * ndc_x + up.y * ndc_y,
                    fwd.z + right.z * ndc_x + up.z * ndc_y};
            dirs.push_back(Normalize(d));
        }
    }
    return dirs;
}

// The four representative far-field views the spike + GPU profile both use, plus
// the shared 3-region (~1536 m) F1 block geometry.
struct ViewSpec {
    const char* name;
    const char* preset;
    bool grazing;
};

inline constexpr std::int32_t kRx0 = 4;
inline constexpr std::int32_t kRz0 = 4;
inline constexpr int kRegions = 3;

} // namespace luminumbra_shieldrt
