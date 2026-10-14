// Save and world-entry contracts:
//  (1) quit/shutdown saves use the standing eye anchor regardless of stance, so world
//      entry derives the saved feet from the spawn point minus the standing eye height;
//  (2) the render pipeline and world-entry gate share the LUMIN_TREE_IMPOSTORS policy:
//      empty or '0'-prefixed values disable impostors without refusing world entry.
#include <gtest/gtest.h>

#include "FocusedUiElement.h"
#include "app/FarmDropSeedInput.h"
#include "core/BenchmarkPose.h"
#include "player/SaveAnchor.h"
#include "rendering/TreeImpostorPolicy.h"

#include <fstream>
#include <iterator>
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
using Luminumbra::Client::App::FarmDropSeedInputAllowed;
using Luminumbra::Client::Test::FocusedUiElement;

struct InputGateApp {
    struct Hud {
        bool paused = false;
        bool show_settings = false;
        int rebindCaptureAction = -1;
        bool codexOpen = false;
        bool photoModeUiShown = false;
    } hud;
    struct Overlay {
        bool show_gpu_profiler = false;
        bool show_shader_panel = false;
        bool show_worldgen_viewer = false;
    } overlay;
};

struct InputGateRuntime {
    bool windowFocused = true;
    bool playerPhotoMode = false;
    bool imguiCapture = false;
    const FocusedUiElement* focused = nullptr;

    bool WindowFocused() const {
        return windowFocused;
    }
    bool PlayerPhotoModeActive() const {
        return playerPhotoMode;
    }
    bool ImGuiCapturesKeyboard() const {
        return imguiCapture;
    }
    const FocusedUiElement* GetFocusElement() const {
        return focused;
    }
};
} // namespace

TEST(FarmDropSeedInput, PassiveFocusAllowsGameplayThroughProductionGate) {
    const InputGateApp app;
    InputGateRuntime input;
    const FocusedUiElement context{"#root"};
    const FocusedUiElement hud{"body", &context};
    const FocusedUiElement hint{"span", &hud};
    const FocusedUiElement button{"button", &hud};
    for (const auto* focused :
         {static_cast<const FocusedUiElement*>(nullptr), &context, &hud, &hint, &button}) {
        input.focused = focused;
        // The old non-null-focus gate refuses all four non-null passive cases.
        EXPECT_TRUE(FarmDropSeedInputAllowed(app, input)) << (focused ? focused->tag : "no focus");
    }
}

TEST(FarmDropSeedInput, ControlsRemainCapturedIncludingReadonlyAndInputButtons) {
    const InputGateApp app;
    InputGateRuntime input;
    const FocusedUiElement hud{"body"};
    for (const char* tag : {"input", "textarea", "select"}) {
        FocusedUiElement control{tag, &hud};
        input.focused = &control;
        EXPECT_FALSE(FarmDropSeedInputAllowed(app, input)) << tag;
        if (control.tag != "select") {
            control.readOnly = true;
            ASSERT_TRUE(control.HasAttribute("readonly"));
            EXPECT_FALSE(FarmDropSeedInputAllowed(app, input)) << "readonly " << tag;
        }
    }
    const FocusedUiElement inputButton{"input", &hud, std::nullopt, false, "button"};
    ASSERT_EQ(inputButton.GetAttribute<std::string>("type", ""), "button");
    input.focused = &inputButton;
    EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));

    for (const char* value : {"", "true", "plaintext-only"}) {
        const FocusedUiElement editor{"div", &hud, value};
        const FocusedUiElement text{"span", &editor};
        input.focused = &text;
        EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
        const FocusedUiElement readOnly{"span", &editor, "false"};
        input.focused = &readOnly;
        EXPECT_TRUE(FarmDropSeedInputAllowed(app, input));
    }
}

TEST(FarmDropSeedInput, EveryWindowHudOverlayAndExternalCaptureGuardBlocksGameplay) {
    InputGateApp app;
    InputGateRuntime input;
    const FocusedUiElement hud{"body"};
    input.focused = &hud;
    ASSERT_TRUE(FarmDropSeedInputAllowed(app, input));
    for (auto member : {&InputGateApp::Hud::paused,
                        &InputGateApp::Hud::show_settings,
                        &InputGateApp::Hud::codexOpen,
                        &InputGateApp::Hud::photoModeUiShown}) {
        app.hud.*member = true;
        EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
        app.hud.*member = false;
    }
    app.hud.rebindCaptureAction = 0;
    EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
    app.hud.rebindCaptureAction = -1;
    for (auto member : {&InputGateApp::Overlay::show_gpu_profiler,
                        &InputGateApp::Overlay::show_shader_panel,
                        &InputGateApp::Overlay::show_worldgen_viewer}) {
        app.overlay.*member = true;
        EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
        app.overlay.*member = false;
    }
    input.windowFocused = false;
    EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
    input.windowFocused = true;
    for (auto member : {&InputGateRuntime::playerPhotoMode, &InputGateRuntime::imguiCapture}) {
        input.*member = true;
        EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
        input.*member = false;
    }
    EXPECT_TRUE(FarmDropSeedInputAllowed(app, input));
}

TEST(FarmDropSeedInput, GlfwAdapterDelegatesToTheTestedProductionGate) {
    // Also guard the wiring: retaining the tested policy while restoring the old
    // non-null-focus expression in the GLFW adapter must fail this regression.
    std::ifstream file(std::string(LUMINUMBRA_SOURCE_ROOT) +
                       "/src/luminumbra_client/app/InputCallbacks.cpp");
    ASSERT_TRUE(file.is_open());
    const std::string source{std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>()};
    const auto begin = source.find("bool FarmDropSeedInputAllowed(GLFWwindow* window) {");
    ASSERT_NE(begin, std::string::npos);
    const auto end = source.find("\n}", begin);
    ASSERT_NE(end, std::string::npos);
    const auto adapter = source.substr(begin, end - begin);
    EXPECT_NE(adapter.find("return FarmDropSeedInputAllowed(app, "
                           "FarmDropSeedRuntimeInput{window, player, ui});"),
              std::string::npos);
    EXPECT_EQ(adapter.find("GetFocusElement"), std::string::npos);
}
