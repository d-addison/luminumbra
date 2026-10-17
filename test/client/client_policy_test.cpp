// Save and world-entry contracts:
//  (1) quit/shutdown saves use the standing eye anchor regardless of stance, so world
//      entry derives the saved feet from the spawn point minus the standing eye height;
//  (2) the render pipeline and world-entry gate share the LUMIN_TREE_IMPOSTORS policy:
//      empty or '0'-prefixed values disable impostors without refusing world entry.
#include <gtest/gtest.h>

#include "FocusedUiElement.h"
#include "app/DeveloperUiPolicy.h"
#include "app/FarmDropSeedInput.h"
#include "app/UiInputRouter.h"
#include "core/BenchmarkPose.h"
#include "luminumbra_common/game/ItemDrop.h"
#include "player/SaveAnchor.h"
#include "rendering/TreeImpostorPolicy.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <set>
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
        int rebindCaptureAction = -1;
        bool codexOpen = false;
        bool photoModeUiShown = false;
    } hud;
};

struct InputGateRuntime {
    bool windowFocused = true;
    bool playerPhotoMode = false;
    bool imguiCapture = false;
    const FocusedUiElement* focused = nullptr;
    const Luminumbra::Client::App::UiInputRouter* router = nullptr;
    int seedKey = 66;
    std::set<std::string> visiblePanels;
    Luminumbra::Client::App::DeveloperUiPolicy developerPolicy;

    bool GameplayKeyAllowed() const {
        return !router || router->GameplayKeyAllowed(seedKey);
    }
    bool DeveloperPanelVisible(const char* id) const {
        return Luminumbra::Client::App::DeveloperUiAllowed(developerPolicy) &&
               visiblePanels.contains(id);
    }

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
                        &InputGateApp::Hud::codexOpen,
                        &InputGateApp::Hud::photoModeUiShown}) {
        app.hud.*member = true;
        EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
        app.hud.*member = false;
    }
    app.hud.rebindCaptureAction = 0;
    EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
    app.hud.rebindCaptureAction = -1;
    for (const char* id : {"client.settings", "perf.gpu", "render.shaders", "world.generation"}) {
        input.visiblePanels.insert(id);
        EXPECT_FALSE(FarmDropSeedInputAllowed(app, input)) << id;
        input.visiblePanels.erase(id);
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

TEST(FarmDropSeedInput, RoutedCaptureRequiresReleaseBeforeDefaultAndReboundSeedDrops) {
    using namespace Luminumbra::Client::App;
    const InputGateApp app;
    for (const int key : {66, 90}) {
        for (int capture = 0; capture < 11; ++capture) {
            SCOPED_TRACE(key);
            SCOPED_TRACE(capture);
            UiInputRouter router;
            UiInputOwnership ownership;
            ownership.in_world = true;
            router.SetOwnership(ownership);
            InputGateRuntime input;
            input.router = &router;
            input.seedKey = key;
            luminumbra::game::FarmDropSeedEdge edge;
            ASSERT_TRUE(FarmDropSeedInputAllowed(app, input));
            EXPECT_FALSE(luminumbra::game::FarmDropSeedPressed(false, true, true, edge));
            auto captured = ownership;
            switch (capture) {
                case 0:
                    captured.console_open = true;
                    break;
                case 1:
                    captured.interactive_panel = true;
                    break;
                case 2:
                    captured.binding_capture = true;
                    break;
                case 3:
                    captured.rml_interaction = true;
                    break;
                case 4:
                    captured.rml_text_input = true;
                    break;
                case 5:
                    captured.imgui_text_input = true;
                    break;
                case 6:
                    captured.imgui_keyboard = true;
                    break;
                case 7:
                    captured.imgui_mouse = true;
                    break;
                case 8:
                    captured.window_focused = false;
                    break;
                case 9:
                    captured.in_world = false;
                    break;
                case 10:
                    captured.rml_debugger = true;
                    captured.rml_interaction = true;
                    break;
            }
            router.SetOwnership(captured);
            router.Key(key, 1, false, false);
            EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
            EXPECT_FALSE(luminumbra::game::FarmDropSeedPressed(
                true, true, FarmDropSeedInputAllowed(app, input), edge));
            router.SetOwnership(ownership);
            EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
            EXPECT_FALSE(luminumbra::game::FarmDropSeedPressed(
                true, true, FarmDropSeedInputAllowed(app, input), edge));
            router.Key(key, 0, false, false);
            ASSERT_TRUE(FarmDropSeedInputAllowed(app, input));
            EXPECT_FALSE(luminumbra::game::FarmDropSeedPressed(
                false, true, FarmDropSeedInputAllowed(app, input), edge));
            router.Key(key, 1, false, false);
            EXPECT_TRUE(luminumbra::game::FarmDropSeedPressed(
                true, true, FarmDropSeedInputAllowed(app, input), edge));
            EXPECT_FALSE(luminumbra::game::FarmDropSeedPressed(
                true, true, FarmDropSeedInputAllowed(app, input), edge));
        }
    }
}

TEST(FarmDropSeedInput, SuppressedDeveloperPanelsLeaveGameplayAvailable) {
    using Luminumbra::Client::App::DeveloperUiPolicy;
    const InputGateApp app;
    for (const char* id : {"client.settings", "perf.gpu", "render.shaders", "world.generation"}) {
        SCOPED_TRACE(id);
        InputGateRuntime input;
        input.visiblePanels.insert(id);
        ASSERT_FALSE(FarmDropSeedInputAllowed(app, input));
        input.developerPolicy.imgui_enabled = false;
        EXPECT_TRUE(FarmDropSeedInputAllowed(app, input));
        input.developerPolicy.imgui_enabled = true;
        for (auto member : {&DeveloperUiPolicy::scenario_active,
                            &DeveloperUiPolicy::timelapse,
                            &DeveloperUiPolicy::scene,
                            &DeveloperUiPolicy::frame_scan,
                            &DeveloperUiPolicy::render_parity,
                            &DeveloperUiPolicy::render_benchmark,
                            &DeveloperUiPolicy::survey,
                            &DeveloperUiPolicy::bake_impostor,
                            &DeveloperUiPolicy::ui_screenshot,
                            &DeveloperUiPolicy::ui_thumbnails}) {
            input.developerPolicy.*member = true;
            EXPECT_TRUE(FarmDropSeedInputAllowed(app, input));
            input.developerPolicy.*member = false;
            EXPECT_FALSE(FarmDropSeedInputAllowed(app, input));
        }
    }
}
