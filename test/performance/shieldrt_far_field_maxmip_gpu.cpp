// Standalone GPU max-mip reduction parity.
//
// The experimental heightfield raymarch uses a max-mip pyramid. GL's
// glGenerateMipmap is a box (average)
// filter — unusable: the conservative ray-above-surface test requires per-cell
// max. This test exercises GPU max-reduction (two compute passes: build
// level 0 = max over each 2x2 block of base samples, then halve+max each coarser
// level) and proves it BYTE-IDENTICAL to the CPU reference BuildHeightMaxMip
// (shieldrt_far_field.h) — max of identical uploaded floats is exact, so the
// GPU and CPU pyramids must match to the bit. This validates the acceleration-
// structure build in isolation without claiming a shipping render integration.
//
// Render-only, GPU-gated (skips headless/software). Label manual;perf;gpu:
//   ctest -L manual -R ShieldRtFarFieldMaxMipGpu --output-on-failure

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
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

// Build level 0 of the max-mip from the base heightfield: each cell = max of its
// 2x2 corner samples. Base is (n x n) row-major; output is ((n-1) x (n-1)).
const char* const kMaxMipLevel0Compute = R"GLSL(
#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) readonly  buffer Base { float base[]; };
layout(std430, binding = 1) writeonly buffer Out  { float outc[]; };
uniform int u_n;      // base samples per side
uniform int u_cells;  // n - 1
void main() {
    int cx = int(gl_GlobalInvocationID.x);
    int cz = int(gl_GlobalInvocationID.y);
    if (cx >= u_cells || cz >= u_cells) return;
    float a = base[cz * u_n + cx];
    float b = base[cz * u_n + cx + 1];
    float c = base[(cz + 1) * u_n + cx];
    float d = base[(cz + 1) * u_n + cx + 1];
    outc[cz * u_cells + cx] = max(max(a, b), max(c, d));
}
)GLSL";

// Coarsen one level: out[(fine+1)/2]^2, each coarse cell = max over the (up to
// 2x2) fine cells it covers. Matches the CPU scatter (nz = cz/2, coarse =
// (cells+1)/2) as a gather.
const char* const kMaxMipReduceCompute = R"GLSL(
#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) readonly  buffer In  { float fin[]; };
layout(std430, binding = 1) writeonly buffer Out { float fout[]; };
uniform int u_fine;
uniform int u_coarse;
void main() {
    int X = int(gl_GlobalInvocationID.x);
    int Z = int(gl_GlobalInvocationID.y);
    if (X >= u_coarse || Z >= u_coarse) return;
    float m = -3.0e38;
    for (int dz = 0; dz < 2; ++dz) {
        int fz = 2 * Z + dz;
        if (fz >= u_fine) continue;
        for (int dx = 0; dx < 2; ++dx) {
            int fx = 2 * X + dx;
            if (fx >= u_fine) continue;
            m = max(m, fin[fz * u_fine + fx]);
        }
    }
    fout[Z * u_coarse + X] = m;
}
)GLSL";

GLuint MakeRWBuffer(GLsizeiptr bytes) {
    GLuint b = 0;
    glGenBuffers(1, &b);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, nullptr, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return b;
}

GLuint Groups(int dim) {
    return static_cast<GLuint>((dim + 7) / 8);
}

} // namespace

TEST(ShieldRtFarFieldMaxMipGpu, GpuMaxReductionMatchesCpuReference) {
    HiddenGlContext ctx("shieldrt_far_field_maxmip_gpu");
    if (!ctx.ready()) {
        GTEST_SKIP() << "no GL context (headless): " << ctx.error();
    }
    const std::string renderer = GlString(GL_RENDERER);
    const bool software = IsSoftwareRenderer(renderer);

    std::string err;
    const GLuint prog0 = CompileComputeProgram(kMaxMipLevel0Compute, err);
    ASSERT_NE(prog0, 0u) << err;
    const GLuint progR = CompileComputeProgram(kMaxMipReduceCompute, err);
    ASSERT_NE(progR, 0u) << err;

    fs::create_directories(ArtifactRoot());

    JobSystem jobs;
    jobs.startup();

    nlohmann::json results = nlohmann::json::array();
    bool all_exact = true;

    // Two presets (flat-ish default + high-relief mountains) exercise a range of
    // height values and odd coarse-dimension boundaries.
    const std::array<const char*, 2> presets{{"default.json", "mountains.json"}};
    for (const char* preset : presets) {
        const fs::path preset_path = SourceRoot() / "worlds/atlas/presets" / preset;
        ASSERT_TRUE(fs::exists(preset_path)) << preset_path.string();
        const TerrainGenParams params = LoadPresetParams(preset_path);
        SHIELD_WorldSystem world(&jobs, nullptr, params, kSeed);
        const std::uint64_t params_hash = ComputeTerrainParamsHash(params, kSeed);

        // Single F1 region (n = 129) keeps the test quick while exercising the
        // odd-dimension halving (128 -> 64 ->... and the (n-1)=128 base cells).
        const HeightField hf =
            BuildHeightFieldFromTiles(world, FarLodTier::F1, kRx0, kRz0, 1, params_hash);
        const HeightMaxMip cpu = BuildHeightMaxMip(hf);

        // Upload base heightfield.
        const GLuint base_buf =
            MakeStorageBuffer(static_cast<GLsizeiptr>(hf.h.size() * sizeof(float)), hf.h.data());

        // Allocate one RW buffer per CPU level (same dims).
        std::vector<GLuint> level_buf(cpu.levels, 0u);
        for (int L = 0; L < cpu.levels; ++L) {
            level_buf[L] =
                MakeRWBuffer(static_cast<GLsizeiptr>(cpu.max_h[L].size() * sizeof(float)));
        }

        // Level 0 from base.
        glUseProgram(prog0);
        glUniform1i(glGetUniformLocation(prog0, "u_n"), hf.n);
        glUniform1i(glGetUniformLocation(prog0, "u_cells"), cpu.dims[0]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, base_buf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, level_buf[0]);
        glDispatchCompute(Groups(cpu.dims[0]), Groups(cpu.dims[0]), 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        // Coarser levels.
        glUseProgram(progR);
        for (int L = 1; L < cpu.levels; ++L) {
            glUniform1i(glGetUniformLocation(progR, "u_fine"), cpu.dims[L - 1]);
            glUniform1i(glGetUniformLocation(progR, "u_coarse"), cpu.dims[L]);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, level_buf[L - 1]);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, level_buf[L]);
            glDispatchCompute(Groups(cpu.dims[L]), Groups(cpu.dims[L]), 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
        glFinish();

        // Read back + compare each level to the CPU reference (exact: max of the
        // same uploaded floats performs no rounding).
        long long total_cells = 0, mismatches = 0;
        double max_abs_diff = 0.0;
        for (int L = 0; L < cpu.levels; ++L) {
            std::vector<float> gpu(cpu.max_h[L].size(), 0.0f);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, level_buf[L]);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                               0,
                               static_cast<GLsizeiptr>(gpu.size() * sizeof(float)),
                               gpu.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            for (std::size_t i = 0; i < gpu.size(); ++i) {
                ++total_cells;
                const double diff =
                    std::abs(static_cast<double>(gpu[i]) - static_cast<double>(cpu.max_h[L][i]));
                if (diff != 0.0) {
                    ++mismatches;
                    max_abs_diff = std::max(max_abs_diff, diff);
                }
            }
        }

        if (mismatches != 0)
            all_exact = false;
        results.push_back({
            {"preset", preset},
            {"base_n", hf.n},
            {"levels", cpu.levels},
            {"total_cells", total_cells},
            {"mismatches", mismatches},
            {"max_abs_diff", max_abs_diff},
        });

        EXPECT_EQ(mismatches, 0) << preset << ": GPU max-mip diverges from CPU reference at "
                                 << mismatches << "/" << total_cells << " cells (max diff "
                                 << max_abs_diff << ")";

        glDeleteBuffers(1, &base_buf);
        for (GLuint b : level_buf)
            glDeleteBuffers(1, &b);
    }

    jobs.shutdown();
    glDeleteProgram(prog0);
    glDeleteProgram(progR);

#ifdef NDEBUG
    const char* build_mode = "release";
#else
    const char* build_mode = "debug";
#endif
    const nlohmann::json report = {
        {"schema", "luminumbra.shieldrt_far_field_maxmip.v1"},
        {"generated_by",
         "shieldrt_far_field_maxmip_gpu (GPU max-reduction vs CPU BuildHeightMaxMip)"},
        {"seed", kSeed},
        {"build_mode", build_mode},
        {"gpu", {{"renderer", renderer}, {"software_renderer", software}}},
        {"all_exact", all_exact},
        {"presets", results},
        {"note",
         "Proves the GPU max-mip reduction (level0 = max over 2x2 base samples, then "
         "halve+max per level) is byte-identical to the CPU BuildHeightMaxMip "
         "reference. glGenerateMipmap (box filter) is unusable for the conservative "
         "ray-above test; this is the net-new acceleration-structure build for the "
         "live far-field pass ( inc2c)."},
    };
    std::ofstream out(ArtifactRoot() / "shieldrt-far-field-maxmip.json");
    ASSERT_TRUE(out);
    out << std::setw(2) << report << "\n";

    if (software) {
        GTEST_SKIP() << "software renderer (" << renderer << "): artifact written for record";
    }
    EXPECT_TRUE(all_exact);
}
