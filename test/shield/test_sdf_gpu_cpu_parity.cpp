#include "gtest/gtest.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"

#include <array>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

struct SdfParityCase {
    int seed;
    IVec3 chunk_coords;
};

TerrainGenParams MakeParityParams() {
    TerrainGenParams params;
    params.base_frequency = 0.004f;
    params.base_amplitude = 8.0f;
    params.octaves = 6;
    params.persistence = 0.5f;
    params.lacunarity = 2.2f;
    params.height_offset = 8.0f;
    params.caves_enabled = true;
    params.cave_frequency = 0.04f;
    params.cave_threshold = 0.4f;
    params.cave_carve_value = 2.0f;
    params.island_mask_enabled = false;
    return params;
}

std::vector<SdfParityCase> MakeParityCases() {
    constexpr std::array<int, 3> seeds = {42, 1337, 12345};
    const std::array<IVec3, 3> chunks = {
        IVec3{0, 0, 0},
        IVec3{1, 0, -1},
        IVec3{-2, 1, 3},
    };

    std::vector<SdfParityCase> cases;
    cases.reserve(seeds.size() * chunks.size());
    for (int seed : seeds) {
        for (const IVec3& chunk_coords : chunks) {
            cases.push_back({seed, chunk_coords});
        }
    }
    return cases;
}

std::string CaseName(const SdfParityCase& test_case) {
    std::ostringstream stream;
    stream << "seed=" << test_case.seed
           << " chunk=(" << test_case.chunk_coords.x
           << "," << test_case.chunk_coords.y
           << "," << test_case.chunk_coords.z << ")";
    return stream.str();
}

std::vector<float> GenerateCpuSdf(const TerrainGenParams& params, int seed, const IVec3& chunk_coords) {
    SHIELD_WorldSystem world_system(nullptr, nullptr, params, seed);
    Chunk chunk(chunk_coords);
    world_system.GenerateChunkData(chunk);
    return chunk.sdf_data;
}

std::vector<float> GenerateGpuSdf(const TerrainGenParams& params, int seed, const IVec3& chunk_coords) {
    (void)params;
    (void)seed;
    (void)chunk_coords;
    return {};
}

void ExpectSdfNear(const std::vector<float>& cpu_sdf, const std::vector<float>& gpu_sdf, const std::string& case_name) {
    ASSERT_EQ(cpu_sdf.size(), gpu_sdf.size()) << case_name;
    for (size_t i = 0; i < cpu_sdf.size(); ++i) {
        ASSERT_TRUE(std::isfinite(cpu_sdf[i])) << case_name << " CPU SDF index " << i;
        ASSERT_TRUE(std::isfinite(gpu_sdf[i])) << case_name << " GPU SDF index " << i;
        EXPECT_NEAR(cpu_sdf[i], gpu_sdf[i], 1e-5f) << case_name << " SDF index " << i;
    }
}

} // namespace

TEST(SdfGpuCpuParityTest, KnownSeedsAndChunksMatch) {
    const TerrainGenParams params = MakeParityParams();
    const std::vector<SdfParityCase> cases = MakeParityCases();
    ASSERT_EQ(cases.size(), 9u);

    GTEST_SKIP() << "GPU SDF generation is not exposed to the test build yet.";

    for (const SdfParityCase& test_case : cases) {
        const std::string case_name = CaseName(test_case);
        const std::vector<float> cpu_sdf = GenerateCpuSdf(params, test_case.seed, test_case.chunk_coords);
        const std::vector<float> gpu_sdf = GenerateGpuSdf(params, test_case.seed, test_case.chunk_coords);
        ExpectSdfNear(cpu_sdf, gpu_sdf, case_name);
    }
}
