// Regressions from the independent review of candidate 2c32211:
//  (1) saving while crouched reloaded the player 0.81 m below the saved feet because both
//      quit/shutdown saves recorded the live camera position while world entry derives the
//      feet from the spawn point minus the STANDING eye height;
//  (2) the LUMIN_TREE_IMPOSTORS switch was interpreted differently by the render pipeline
//      (empty or '0'-prefixed disables) and the world-entry gate (only exactly "0"), so a
//      documented disable value could refuse world entry.
#include <gtest/gtest.h>

#include "player/SaveAnchor.h"
#include "rendering/TreeImpostorPolicy.h"

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
