#include "gtest/gtest.h"

#include "luminumbra_common/network/NetworkStateHash.h"

#include "../support/EntitySnapshotFixture.h"

#include <filesystem>
#include <string>

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

// Helper gates compiled into this executable from their own translation
// units; the lua-api-manifest and aetheric-diffusion gates run at static
// initialization in theirs, so linking them is the assertion.
bool LuminumbraInstinctPlannerGateTest();

namespace luminumbra::network::test {
bool NetworkLoopbackAuthorityGateExercisesFixture();
}

namespace {

std::filesystem::path NetworkArtifactDir() {
    return std::filesystem::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "network";
}

} // namespace

TEST(NetworkStateHash, FixtureIsDeterministicAndMeetsBaseline) {
    const auto report = luminumbra::network::BuildNetworkStateHashFixture(
        Luminumbra::TestSupport::BuildEntitySnapshotFixture(), "debug");
    EXPECT_TRUE(report.passed);
    EXPECT_TRUE(report.deterministicReplay);
    EXPECT_TRUE(report.monotonicTicks);
    EXPECT_GE(report.tickCount, 5u);
    EXPECT_FALSE(report.worldHash.empty());
    EXPECT_FALSE(report.durableEntityIds.empty());
    EXPECT_EQ(report.finalStateHash, report.replayFinalStateHash);
    EXPECT_TRUE(luminumbra::network::NetworkStateHashMeetsBaseline(report));

    const std::string json = luminumbra::network::SerializeNetworkStateHashJson(report);
    EXPECT_NE(json.find("luminumbra.network.state_hash.v1"), std::string::npos);
    EXPECT_NE(json.find("authoritative_sorted_state_per_tick"), std::string::npos);
    EXPECT_NE(json.find("tick_ascending_sorted_state_fields"), std::string::npos);
    EXPECT_NE(json.find("fnv1a_64_canonical_state_string"), std::string::npos);
}

TEST(NetworkStateHash, WritesAnalysisArtifact) {
    const std::filesystem::path artifact_path = NetworkArtifactDir() / "network-state-hash.json";
    ASSERT_TRUE(luminumbra::network::WriteNetworkStateHashArtifact(
        artifact_path.string(), Luminumbra::TestSupport::BuildEntitySnapshotFixture(), "debug"));
    ASSERT_TRUE(std::filesystem::exists(artifact_path));
}

TEST(NetworkStateHash, LoopbackAuthorityFixtureStillMeetsBaseline) {
    EXPECT_TRUE(luminumbra::network::test::NetworkLoopbackAuthorityGateExercisesFixture());
}

TEST(InstinctPlannerGate, GrovestriderHungerFixtureMeetsBaseline) {
    EXPECT_TRUE(LuminumbraInstinctPlannerGateTest());
}
