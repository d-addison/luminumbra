#include "luminumbra_client/ui/GraphicsSettingsState.h"
#include "luminumbra_common/core/SystemConfig.h"

#include <gtest/gtest.h>

#include <limits>

namespace {
using namespace Luminumbra::Client;
using namespace luminumbra::core;

GraphicsEdit Preset(GraphicsPreset preset) {
    GraphicsEdit edit;
    edit.kind = GraphicsEditKind::Preset;
    edit.preset = preset;
    return edit;
}

GraphicsEdit Override(std::string key, GraphicsOverrideValue value) {
    GraphicsEdit edit;
    edit.kind = GraphicsEditKind::Override;
    edit.key = std::move(key);
    edit.value = std::move(value);
    return edit;
}

TEST(GraphicsSettingsState, CustomRetainsBaseAndResetRestoresIt) {
    GraphicsSettings settings;
    ASSERT_TRUE(SelectGraphicsPreset(settings, GraphicsPreset::High));
    settings.overrides["grass_density"] = 1.2;
    EXPECT_EQ(BuildGraphicsSettingsDisplay(settings, {}, "").preset_label, "High");
    settings.overrides["grass_density"] = 0.6;
    GraphicsSettingsState state(settings, {});
    EXPECT_EQ(state.Display().preset_label, "Custom (base: High)");
    ASSERT_TRUE(state.Stage(GraphicsEdit{}));
    const auto& pending = state.Pending();
    if (!pending.has_value()) {
        ADD_FAILURE() << "Staging the reset must queue graphics settings";
        return;
    }
    EXPECT_TRUE(pending->overrides.empty());
    EXPECT_EQ(state.Settings().overrides, settings.overrides); // No commit before renderer success.
    state.Applied(true, 0.0);
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::High);
    EXPECT_EQ(state.Display().preset_label, "High");
    EXPECT_DOUBLE_EQ(state.Display().effective.values.grass_density, 1.2);
}

TEST(GraphicsSettingsState, SelectingEachPresetClearsEveryOverride) {
    for (const auto preset : {GraphicsPreset::Low,
                              GraphicsPreset::Medium,
                              GraphicsPreset::High,
                              GraphicsPreset::Ultra}) {
        GraphicsSettings settings;
        settings.overrides = {
            {"render_scale", 0.5}, {"grass_enabled", false}, {"ui_blur", std::string("off")}};
        GraphicsSettingsState state(settings, {});
        ASSERT_TRUE(state.Stage(Preset(preset)));
        state.Applied(true, 0.0);
        EXPECT_EQ(state.Settings().base_preset, preset);
        EXPECT_TRUE(state.Settings().overrides.empty());
        EXPECT_FALSE(state.Display().effective.custom);
    }
}

TEST(GraphicsSettingsState, InvalidRequestsDoNotReplaceAnAlreadyQueuedTransaction) {
    GraphicsSettingsState state({}, {});
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::Low)));
    for (const auto& edit : {Override("render_scale", 1.01),
                             Override("grass_density", 0.0),
                             Override("grass_fade_start_m", 48.0), // Low ends at 48.
                             Override("grass_fade_end_m", 24.0),
                             Override("far_terrain_radius_m", 767.0),
                             Override("shadow_resolution", 512.0),
                             Override("volumetric_quality", 2.0),
                             Override("ui_blur", std::string("invalid")),
                             Override("render_scale", std::numeric_limits<double>::quiet_NaN()),
                             Override("water_resolution", 8.0),
                             Preset(static_cast<GraphicsPreset>(255))}) {
        EXPECT_FALSE(state.Stage(edit));
        const auto& pending = state.Pending();
        if (!pending.has_value()) {
            ADD_FAILURE() << "An invalid edit must retain the queued graphics settings";
            return;
        }
        EXPECT_EQ(pending->base_preset, GraphicsPreset::Low);
        EXPECT_TRUE(pending->overrides.empty());
        EXPECT_FALSE(state.Display().status.empty());
    }
}

TEST(GraphicsSettingsState, RapidEditsCoalesceAndSaveOnlyAfterSuccessfulApplyAndDebounce) {
    GraphicsSettingsState state({}, {});
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::Medium)));
    ASSERT_TRUE(state.Stage(Override("grass_density", 0.5)));
    EXPECT_FALSE(state.SaveDue(100.0));
    state.Applied(true, 1.0);
    EXPECT_FALSE(state.SaveDue(1.499));
    ASSERT_TRUE(state.Stage(Override("ui_blur", std::string("off"))));
    EXPECT_FALSE(state.SaveDue(100.0));
    state.Applied(true, 1.4);
    EXPECT_FALSE(state.SaveDue(1.899));
    EXPECT_TRUE(state.SaveDue(1.9));
    EXPECT_DOUBLE_EQ(state.Display().effective.values.grass_density, 0.5);
    EXPECT_EQ(state.Display().effective.values.ui_blur, UiBlurQuality::Off);
    state.Saved(false);
    EXPECT_FALSE(state.SaveDue(100.0));
    EXPECT_EQ(state.Display().status, "Save failed; graphics are active for this session");
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Medium);
    state.ScheduleSave(2.0);
    EXPECT_TRUE(state.SaveDue(2.5));
    state.Saved(true);
    EXPECT_EQ(state.Display().status, "Graphics saved");
    EXPECT_FALSE(state.SavePending());
}

TEST(GraphicsSettingsState, ApplyFailureRetainsSavedSelectionAndNeverSchedulesItsSave) {
    GraphicsSettings settings;
    settings.overrides["render_scale"] = 0.75;
    GraphicsSettingsState state(settings, {});
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::Low)));
    state.Applied(false, 1.0);
    EXPECT_FALSE(state.Pending());
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Ultra);
    EXPECT_EQ(state.Settings().overrides, settings.overrides);
    EXPECT_DOUBLE_EQ(state.Display().effective.values.render_scale, 0.75);
    EXPECT_NE(state.Display().status.find("apply failed"), std::string::npos);
    EXPECT_FALSE(state.SaveDue(100.0));
}

TEST(GraphicsSettingsState, DetectApplyUsesFreshInputsClearsOverridesAndReportsHardware) {
    GraphicsSettings settings;
    settings.overrides["render_scale"] = 0.5;
    GraphicsSettingsState state(settings, {});
    GraphicsEdit detect;
    detect.kind = GraphicsEditKind::DetectApply;
    EXPECT_FALSE(state.Stage(detect));
    EXPECT_FALSE(state.Pending());
    EXPECT_EQ(state.Display().status, "Hardware detection unavailable");
    GraphicsHardwareInputs inputs;
    inputs.vendor = "NVIDIA";
    inputs.renderer = "NVIDIA GeForce RTX 5070 Ti";
    inputs.version = "4.6";
    inputs.logicalProcessorCount = 16;
    inputs.dedicatedVideoMemoryBytes = 8ULL * 1024 * 1024 * 1024;
    ASSERT_TRUE(state.Stage(detect, inputs));
    state.Applied(true, 0.0);
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Ultra);
    EXPECT_TRUE(state.Settings().overrides.empty());
    EXPECT_EQ(state.Settings().detection.rules_revision, kGraphicsPresetRulesRevision);
    EXPECT_NE(state.Display().hardware.find("RTX 5070 Ti"), std::string::npos);
    EXPECT_NE(state.Display().hardware.find("CPU threads: 16"), std::string::npos);
    EXPECT_NE(state.Display().hardware.find("VRAM: 8192 MiB"), std::string::npos);
    EXPECT_NE(state.Display().hardware.find("Recommended: Ultra"), std::string::npos);
    inputs.logicalProcessorCount = 4;
    inputs.dedicatedVideoMemoryBytes.reset();
    ASSERT_TRUE(state.Stage(detect, inputs));
    state.Applied(true, 1.0);
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Low);
    EXPECT_NE(state.Display().hardware.find("VRAM: unknown"), std::string::npos);
    const auto before = state.Settings().detection;
    ASSERT_TRUE(state.Stage(detect, inputs));
    state.Applied(false, 2.0);
    EXPECT_EQ(state.Settings().detection, before);
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Low);
}

TEST(GraphicsSettingsState, SavingPriorSuccessfulEditsDoesNotHideALaterApplyFailure) {
    GraphicsSettingsState state({}, {});
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::High)));
    state.Applied(true, 0.0);
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::Low)));
    state.Applied(false, 0.1);
    EXPECT_TRUE(state.SaveDue(0.5));
    state.Saved(true);
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::High);
    EXPECT_NE(state.Display().status.find("apply failed"), std::string::npos);
}

TEST(GraphicsSettingsState, SessionOverridesDisplayEffectiveValuesAndNeverEnterSavedSettings) {
    GraphicsSettings settings;
    ASSERT_TRUE(SelectGraphicsPreset(settings, GraphicsPreset::High));
    const GraphicsOverrides session{{"render_scale", 0.67}, {"volumetric_quality", 0.0}};
    GraphicsSettingsState state(settings, session);
    EXPECT_DOUBLE_EQ(state.Display().effective.values.render_scale, 0.67);
    EXPECT_EQ(state.Display().effective.values.volumetric_quality, 0);
    EXPECT_EQ(state.Display().effective.session_overrides, session);
    EXPECT_NE(state.Display().preset_label.find("Session overrides active"), std::string::npos);
    ASSERT_TRUE(state.Stage(Preset(GraphicsPreset::Ultra)));
    state.Applied(true, 1.0);
    ASSERT_TRUE(state.Stage(GraphicsEdit{}));
    state.Applied(true, 2.0);
    EXPECT_TRUE(state.Settings().overrides.empty());
    EXPECT_DOUBLE_EQ(state.Display().effective.values.render_scale, 0.67);
    ResolvedGraphics saved;
    std::string error;
    ASSERT_TRUE(ResolveGraphics(state.Settings(), {}, saved, error));
    EXPECT_DOUBLE_EQ(saved.values.render_scale, 1.0);
    EXPECT_EQ(saved.values.volumetric_quality, 1);
}

TEST(GraphicsSettingsState, StartupMigrationReportDistinguishesFirstLaunchFromExistingChoices) {
    SystemConfig first;
    EXPECT_TRUE(first.settings_load_report().needs_first_launch_detection);
    SystemConfig legacy;
    legacy.OverlayUserFromJsonString(R"({"user":{"video":{"render_scale":0.8}}})");
    EXPECT_FALSE(legacy.settings_load_report().needs_first_launch_detection);
    EXPECT_EQ(BuildGraphicsSettingsDisplay(legacy.user().graphics, {}, "").preset_label,
              "Custom (base: Ultra)");
    SystemConfig recovery;
    recovery.OverlayUserFromJsonString("{");
    EXPECT_FALSE(recovery.settings_load_report().needs_first_launch_detection);
    const auto display = BuildGraphicsSettingsDisplay(
        recovery.user().graphics, {}, recovery.settings_load_report().recovery_reason);
    EXPECT_FALSE(display.status.empty());
    EXPECT_EQ(display.preset_label, "Ultra");
}
} // namespace
