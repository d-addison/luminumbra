// Save and world-entry contracts:
//  (1) quit/shutdown saves use the standing eye anchor regardless of stance, so world
//      entry derives the saved feet from the spawn point minus the standing eye height;
//  (2) the render pipeline and world-entry gate share the LUMIN_TREE_IMPOSTORS policy:
//      empty or '0'-prefixed values disable impostors without refusing world entry.
#include <gtest/gtest.h>

#include "core/BenchmarkPose.h"
#include "core/TimelapseVista.h"
#include "player/SaveAnchor.h"
#include "rendering/TreeImpostorPolicy.h"

#include <cmath>
#include <limits>
#include <optional>
#include <string>

using namespace Luminumbra::Client::Player;
using Luminumbra::Rendering::TreeImpostorsRequested;

namespace {
constexpr float kStanding = 1.8f;
constexpr float kCrouch = 0.9f;
} // namespace

TEST(SaveAnchor, StandingSaveRoundTripsFeet) {
    const glm::vec3 feet(12.0f, 17.152279f, 8.0f);
    const glm::vec3 anchor = SavedSpawnAnchorForFeet(feet, kStanding);
    EXPECT_FLOAT_EQ(anchor.y, feet.y + kStanding * 0.95f);
    const glm::vec3 reloaded = FeetFromSpawnAnchor(anchor, kStanding);
    EXPECT_FLOAT_EQ(reloaded.y, feet.y);
    EXPECT_FLOAT_EQ(reloaded.x, feet.x);
    EXPECT_FLOAT_EQ(reloaded.z, feet.z);
}

TEST(SaveAnchor, CrouchedSaveDoesNotLowerReloadedFeet) {
    const glm::vec3 feet(12.0f, 17.152279f, 8.0f);
    // The live camera while crouched sits at feet + 0.9 * crouch height (PlayerController).
    const glm::vec3 crouched_camera =
        feet + glm::vec3(0.0f, kCrouch * kCrouchingEyeHeightFactor, 0.0f);
    // Old behaviour: the save recorded the crouched camera; reload subtracted the standing eye.
    const glm::vec3 old_reload = FeetFromSpawnAnchor(crouched_camera, kStanding);
    EXPECT_NEAR(feet.y - old_reload.y, kStanding * 0.95f - kCrouch * 0.9f, 1e-5f); // 0.9 m defect
    // New behaviour: the anchor is stance-independent, so the reload restores the same feet.
    const glm::vec3 anchor = SavedSpawnAnchorForFeet(feet, kStanding);
    EXPECT_FLOAT_EQ(FeetFromSpawnAnchor(anchor, kStanding).y, feet.y);
}

TEST(TreeImpostorPolicy, UnsetEnablesImpostors) {
    EXPECT_TRUE(TreeImpostorsRequested(std::nullopt));
}

TEST(TreeImpostorPolicy, DocumentedDisableValuesDisableEverywhere) {
    EXPECT_FALSE(TreeImpostorsRequested(std::string("0")));
    EXPECT_FALSE(TreeImpostorsRequested(std::string("")));
    EXPECT_FALSE(TreeImpostorsRequested(std::string("0abc")));
    EXPECT_TRUE(TreeImpostorsRequested(std::string("1")));
    EXPECT_TRUE(TreeImpostorsRequested(std::string("yes")));
}

namespace {
using Luminumbra::BenchmarkPose::SelectStreamingPosition;
using Luminumbra::BenchmarkPose::StreamingInputs;

StreamingInputs BaseInputs() {
    StreamingInputs in{};
    in.fixed_cam_pos = glm::vec3(1.0f, 2.0f, 3.0f);
    in.camera_pos = glm::vec3(30.0f, 40.0f, 50.0f);
    in.player_pos = glm::vec3(-5.0f, -6.0f, -7.0f);
    return in;
}
} // namespace

TEST(BenchmarkPose, FixedCameraWinsOverEverything) {
    StreamingInputs in = BaseInputs();
    in.fixed_cam = true;
    in.benchmark_active = true;
    in.has_camera = true;
    in.cam_anchored = true;
    in.has_player = true;
    EXPECT_EQ(SelectStreamingPosition(in), in.fixed_cam_pos);
}

TEST(BenchmarkPose, BenchmarkWithCameraUsesDefaultPose) {
    StreamingInputs in = BaseInputs();
    in.benchmark_active = true;
    in.has_camera = true;
    in.has_player = true;
    EXPECT_EQ(SelectStreamingPosition(in), glm::vec3(8.0f, 56.0f, 8.0f));
}

TEST(BenchmarkPose, BenchmarkWithoutCameraFallsBackToPlayer) {
    StreamingInputs in = BaseInputs();
    in.benchmark_active = true;
    in.has_camera = false;
    in.has_player = true;
    EXPECT_EQ(SelectStreamingPosition(in), in.player_pos);
}

TEST(BenchmarkPose, InactiveBenchmarkAnchoredUsesCamera) {
    StreamingInputs in = BaseInputs();
    in.has_camera = true;
    in.cam_anchored = true;
    in.has_player = true;
    EXPECT_EQ(SelectStreamingPosition(in), in.camera_pos);
}

TEST(BenchmarkPose, UnanchoredWithPlayerUsesPlayer) {
    StreamingInputs in = BaseInputs();
    in.has_camera = true;
    in.has_player = true;
    EXPECT_EQ(SelectStreamingPosition(in), in.player_pos);
}

TEST(BenchmarkPose, UnanchoredWithoutPlayerUsesCamera) {
    StreamingInputs in = BaseInputs();
    in.has_camera = true;
    in.has_player = false;
    EXPECT_EQ(SelectStreamingPosition(in), in.camera_pos);
}

TEST(BenchmarkPose, ConstantsMatchRenderBenchmarkDefaults) {
    namespace BP = Luminumbra::BenchmarkPose;
    EXPECT_FLOAT_EQ(BP::kPosX, 8.0f);
    EXPECT_FLOAT_EQ(BP::kPosY, 56.0f);
    EXPECT_FLOAT_EQ(BP::kPosZ, 8.0f);
    EXPECT_FLOAT_EQ(BP::kYaw, 35.0f);
    EXPECT_FLOAT_EQ(BP::kPitch, -6.0f);
    EXPECT_FLOAT_EQ(BP::kTimeOfDay, 0.04f);
}

namespace {
namespace TV = Luminumbra::TimelapseVista;
constexpr int kDefaultSettle = 45; // kTimelapseSettleFrames in ClientAppContext.h
} // namespace

TEST(TimelapseVista, SettleFramesStayWithinDefaultAndOneMinute) {
    EXPECT_EQ(TV::ClampSettleFrames(kDefaultSettle, kDefaultSettle), kDefaultSettle);
    EXPECT_EQ(TV::ClampSettleFrames(0, kDefaultSettle), kDefaultSettle);
    EXPECT_EQ(TV::ClampSettleFrames(-5, kDefaultSettle), kDefaultSettle);
    EXPECT_EQ(TV::ClampSettleFrames(600, kDefaultSettle), 600);
    EXPECT_EQ(TV::ClampSettleFrames(TV::kMaxSettleFrames, kDefaultSettle), TV::kMaxSettleFrames);
    EXPECT_EQ(TV::ClampSettleFrames(TV::kMaxSettleFrames + 1, kDefaultSettle),
              TV::kMaxSettleFrames);
    EXPECT_EQ(TV::ClampSettleFrames(std::numeric_limits<int>::max(), kDefaultSettle),
              TV::kMaxSettleFrames);
}

TEST(TimelapseVista, PanParsesFiniteValuesAndClampsToTwoDegrees) {
    EXPECT_EQ(TV::ParsePanDegrees("0.5"), std::optional<float>(0.5f));
    EXPECT_EQ(TV::ParsePanDegrees("-1.25"), std::optional<float>(-1.25f));
    EXPECT_EQ(TV::ParsePanDegrees("0"), std::optional<float>(0.0f));
    EXPECT_EQ(TV::ParsePanDegrees("2"), std::optional<float>(TV::kMaxPanDegreesPerFrame));
    EXPECT_EQ(TV::ParsePanDegrees("45"), std::optional<float>(TV::kMaxPanDegreesPerFrame));
    EXPECT_EQ(TV::ParsePanDegrees("-45"), std::optional<float>(-TV::kMaxPanDegreesPerFrame));
    EXPECT_EQ(TV::ParsePanDegrees("1e30"), std::optional<float>(TV::kMaxPanDegreesPerFrame));
}

TEST(TimelapseVista, PanRefusesEmptyGarbageAndNonFiniteInput) {
    for (const char* text : {"", "abc", "nan", "NaN", "-nan", "inf", "-inf", "infinity", "1e999"}) {
        SCOPED_TRACE(text);
        EXPECT_EQ(TV::ParsePanDegrees(text), std::nullopt);
    }
}

TEST(TimelapseVista, FramingLooksTowardTheSunsetSideJustBelowTheHorizon) {
    const glm::vec3 spawn(100.0f, 40.0f, -20.0f);
    const auto framing = TV::VistaFraming(spawn);
    // Low camera: well under the 16-30 m overlooks of the rain/drain framings.
    EXPECT_FLOAT_EQ(framing.position.y - spawn.y, 10.0f);
    const glm::vec3 d = glm::normalize(framing.target - framing.position);
    EXPECT_LT(d.x, -0.9f); // the sun sets toward -x
    const float pitch = glm::degrees(std::asin(d.y));
    EXPECT_LT(pitch, 0.0f);
    EXPECT_GT(pitch, -10.0f);
    // The framing follows the spawn point, not world coordinates.
    const auto moved = TV::VistaFraming(spawn + glm::vec3(5.0f, -3.0f, 7.0f));
    EXPECT_FLOAT_EQ(moved.position.x - framing.position.x, 5.0f);
    EXPECT_FLOAT_EQ(moved.target.y - framing.target.y, -3.0f);
    EXPECT_FLOAT_EQ(moved.target.z - framing.target.z, 7.0f);
}

TEST(TimelapseVista, PanAccumulatesPerCapturedFrameAndZeroKeepsTheFramingFixed) {
    EXPECT_FLOAT_EQ(TV::PanYawOffset(0.0f, 0), 0.0f);
    EXPECT_FLOAT_EQ(TV::PanYawOffset(0.0f, 900), 0.0f);
    EXPECT_FLOAT_EQ(TV::PanYawOffset(0.5f, 0), 0.0f);
    EXPECT_FLOAT_EQ(TV::PanYawOffset(0.5f, 10), 5.0f);
    EXPECT_FLOAT_EQ(TV::PanYawOffset(-2.0f, 90), -180.0f);
}

TEST(TimelapseVista, VistaTakesPrecedenceOverScenarioFramings) {
    EXPECT_FALSE(TV::ScenarioMayFrameCamera(true));
    EXPECT_TRUE(TV::ScenarioMayFrameCamera(false));
}
