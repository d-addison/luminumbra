#include "../support/UniqueTempPath.h"
#include "luminumbra_server/modes/Modes.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
namespace fs = std::filesystem;

class BenchArtifactMode : public testing::Test {
protected:
    const Luminumbra::TestSupport::ScopedTempFile file{"bench_artifact_", ".json"};
    ServerCliOptions options;

    void SetUp() override {
        options.seed = "1337";
        options.ticks = 4;
        options.artifact_path = file.Path().string();
    }

    void ExpectArtifact(const char* schema) const {
        ASSERT_TRUE(fs::is_regular_file(file.Path()));
        std::ifstream input(file.Path());
        ASSERT_TRUE(input.is_open());
        const auto artifact = nlohmann::json::parse(input, nullptr, false);
        ASSERT_FALSE(artifact.is_discarded());
        EXPECT_EQ(artifact.at("passed"), true);
        EXPECT_EQ(artifact.at("schema"), schema);
    }

    void ExpectDirectoryUnchanged() const {
        ASSERT_TRUE(fs::is_directory(file.Path().parent_path()));
        EXPECT_TRUE(fs::is_empty(file.Path().parent_path()));
        EXPECT_FALSE(fs::exists(file.Path()));
    }
};

TEST_F(BenchArtifactMode, AetherDirectoryOutputReturnsFailure) {
    options.artifact_path = file.Path().parent_path().string();
    ASSERT_TRUE(fs::is_empty(file.Path().parent_path()));
    EXPECT_EQ(RunAetherBench(options), 1);
    ExpectDirectoryUnchanged();
}

TEST_F(BenchArtifactMode, AetherWritableOutputReturnsSuccess) {
    ASSERT_EQ(RunAetherBench(options), 0);
    ExpectArtifact("luminumbra.aether_field_determinism.v1");
}

TEST_F(BenchArtifactMode, WindDirectoryOutputReturnsFailure) {
    options.artifact_path = file.Path().parent_path().string();
    ASSERT_TRUE(fs::is_empty(file.Path().parent_path()));
    EXPECT_EQ(RunWindBench(options), 1);
    ExpectDirectoryUnchanged();
}

TEST_F(BenchArtifactMode, WindWritableOutputReturnsSuccess) {
    ASSERT_EQ(RunWindBench(options), 0);
    ExpectArtifact("luminumbra.wind_field_determinism.v1");
}

#ifdef __linux__
// Unlike a directory, this device opens successfully and refuses the payload.
TEST_F(BenchArtifactMode, AetherFullDeviceReturnsFailure) {
    options.artifact_path = "/dev/full";
    std::ofstream output(options.artifact_path);
    ASSERT_TRUE(output.is_open());
    output.close();
    EXPECT_EQ(RunAetherBench(options), 1);
}

TEST_F(BenchArtifactMode, WindFullDeviceReturnsFailure) {
    options.artifact_path = "/dev/full";
    std::ofstream output(options.artifact_path);
    ASSERT_TRUE(output.is_open());
    output.close();
    EXPECT_EQ(RunWindBench(options), 1);
}
#endif

} // namespace
