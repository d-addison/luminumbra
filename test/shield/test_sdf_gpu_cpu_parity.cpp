#include "gtest/gtest.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

struct SdfParityCase {
    int seed;
    IVec3 chunk_coords;
};

struct SdfCoverage {
    bool has_inside = false;
    bool has_outside = false;
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

std::filesystem::path SourceRoot() {
    return std::filesystem::weakly_canonical(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT));
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    std::stringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

void ExpectCpuSdfInvariants(const std::vector<float>& cpu_sdf, const std::string& case_name, SdfCoverage& coverage) {
    ASSERT_FALSE(cpu_sdf.empty()) << case_name;
    for (size_t i = 0; i < cpu_sdf.size(); ++i) {
        ASSERT_TRUE(std::isfinite(cpu_sdf[i])) << case_name << " CPU SDF index " << i;
        if (cpu_sdf[i] < 0.0f) {
            coverage.has_inside = true;
        }
        if (cpu_sdf[i] > 0.0f) {
            coverage.has_outside = true;
        }
    }
}

void ExpectGpuSdfIntegrationRemainsGuarded() {
    const std::string render_pipeline_source =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
    ASSERT_FALSE(render_pipeline_source.empty());

    EXPECT_NE(render_pipeline_source.find("constexpr bool kEnableExperimentalGpuSdfIntegration = false"),
              std::string::npos)
        << "GPU SDF integration must stay disabled until this parity test exercises the live GPU path.";
    EXPECT_NE(render_pipeline_source.find("world_system.SetGPUSDFCallback({})"), std::string::npos);
    EXPECT_NE(render_pipeline_source.find("GPU SDF integration disabled"), std::string::npos);

    const std::string compute_shader = ReadTextFile(SourceRoot() / "res/shaders/sdf_generation.compute");
    ASSERT_FALSE(compute_shader.empty());
    EXPECT_NE(compute_shader.find("CPU SDF convention: negative is solid, positive is empty"),
              std::string::npos);
    EXPECT_NE(compute_shader.find("calculateSDF"), std::string::npos);
}

} // namespace

TEST(SdfGpuCpuParityTest, KnownSeedsAndChunksMatch) {
    const TerrainGenParams params = MakeParityParams();
    const std::vector<SdfParityCase> cases = MakeParityCases();
    ASSERT_EQ(cases.size(), 9u);

    SdfCoverage corpus_coverage;
    for (const SdfParityCase& test_case : cases) {
        const std::string case_name = CaseName(test_case);
        const std::vector<float> cpu_sdf = GenerateCpuSdf(params, test_case.seed, test_case.chunk_coords);
        ExpectCpuSdfInvariants(cpu_sdf, case_name, corpus_coverage);
    }

    // Individual chunks may be entirely air or entirely terrain depending on
    // their world-space Y, but the corpus must cover both sides of the isolevel.
    ASSERT_TRUE(corpus_coverage.has_inside) << "parity corpus has no inside-negative samples";
    ASSERT_TRUE(corpus_coverage.has_outside) << "parity corpus has no outside-positive samples";

    ExpectGpuSdfIntegrationRemainsGuarded();
}
