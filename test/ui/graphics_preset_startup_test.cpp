#include "../support/UniqueTempPath.h"
#include "app/GraphicsStartup.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
using namespace luminumbra::core;
using Luminumbra::Client::SelectStartupGraphicsPreset;

class GraphicsPresetStartup : public testing::Test {
protected:
    Luminumbra::TestSupport::ScopedTempFile overlay{"graphics_startup_", ".json"};

    std::filesystem::path DefaultsPath() const {
        return overlay.Path().parent_path() / "systems.json";
    }

    SystemConfig Load() const {
        return SystemConfig::LoadLayered(DefaultsPath(), overlay.Path());
    }

    static void Write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << text;
        output.close();
        ASSERT_FALSE(output.fail());
    }

    static std::string Read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        EXPECT_TRUE(input.is_open());
        std::ostringstream bytes;
        bytes << input.rdbuf();
        EXPECT_FALSE(input.bad());
        return bytes.str();
    }

    static void Start(SystemConfig& config, bool detect_requested = false) {
        // Missing identity and VRAM deliberately exercise conservative hardware detection.
        const GraphicsHardwareInputs inputs;
        std::string error;
        ASSERT_TRUE(SelectStartupGraphicsPreset(config, detect_requested, inputs, error)) << error;
        EXPECT_TRUE(error.empty());
    }

    static void ExpectSameGraphics(const GraphicsSettings& actual,
                                   const GraphicsSettings& expected) {
        EXPECT_EQ(actual.base_preset, expected.base_preset);
        EXPECT_EQ(actual.preset_revision, expected.preset_revision);
        EXPECT_EQ(actual.overrides, expected.overrides);
        EXPECT_EQ(actual.detection, expected.detection);
    }
};

TEST_F(GraphicsPresetStartup, FirstLaunchDetectsAndPersistsThenReloadKeepsTheSelection) {
    auto config = Load();
    ASSERT_EQ(config.settings_load_report().state, SettingsLoadState::FirstLaunch);
    ASSERT_TRUE(config.settings_load_report().needs_first_launch_detection);
    ASSERT_NO_FATAL_FAILURE(Start(config));
    EXPECT_EQ(config.user().graphics.base_preset, GraphicsPreset::Low);
    EXPECT_TRUE(config.user().graphics.overrides.empty());
    EXPECT_EQ(config.user().graphics.preset_revision, kGraphicsPresetRevision);
    EXPECT_EQ(config.user().graphics.detection.rules_revision, kGraphicsPresetRulesRevision);
    EXPECT_EQ(config.user().graphics.detection.recommended_preset, GraphicsPreset::Low);
    EXPECT_FALSE(config.user().graphics.detection.observed_hardware.empty());
    EXPECT_FALSE(std::filesystem::exists(overlay.Path()));
    ASSERT_TRUE(config.SaveUserOverlay(overlay.Path()));

    auto reloaded = Load();
    ASSERT_EQ(reloaded.settings_load_report().state, SettingsLoadState::Loaded);
    ASSERT_FALSE(reloaded.settings_load_report().needs_first_launch_detection);
    const GraphicsHardwareInputs faster{.vendor = "NVIDIA",
                                        .renderer = "NVIDIA GeForce RTX 5070 Ti",
                                        .version = "4.6",
                                        .logicalProcessorCount = 16,
                                        .dedicatedVideoMemoryBytes = 8ULL * 1024 * 1024 * 1024,
                                        .framebufferWidth = 1920,
                                        .framebufferHeight = 1080};
    ASSERT_EQ(ClassifyGraphicsPreset(faster).preset, GraphicsPreset::Ultra);
    std::string error;
    ASSERT_TRUE(SelectStartupGraphicsPreset(reloaded, false, faster, error));
    ExpectSameGraphics(reloaded.user().graphics, config.user().graphics);
}

TEST_F(GraphicsPresetStartup, NormalStartPreservesSavedRevisionOverridesAndDetection) {
    for (const auto revision : {1U, kGraphicsPresetRevision}) {
        SCOPED_TRACE(revision);
        SystemConfig saved;
        auto& graphics = saved.user().graphics;
        ASSERT_TRUE(SelectGraphicsPreset(graphics, GraphicsPreset::High));
        graphics.preset_revision = revision;
        graphics.overrides = {{"render_scale", 0.75}, {"ui_blur", std::string("half")}};
        graphics.detection = {.observed_hardware = "saved hardware",
                              .rules_revision = kGraphicsPresetRulesRevision,
                              .recommended_preset = GraphicsPreset::Ultra};
        ASSERT_TRUE(saved.SaveUserOverlay(overlay.Path()));
        const auto before = Read(overlay.Path());
        auto config = Load();
        ASSERT_FALSE(config.settings_load_report().needs_first_launch_detection);
        ASSERT_NO_FATAL_FAILURE(Start(config));
        ExpectSameGraphics(config.user().graphics, graphics);
        EXPECT_EQ(Read(overlay.Path()), before);
        ASSERT_TRUE(config.SaveUserOverlay(overlay.Path()));
        ExpectSameGraphics(Load().user().graphics, graphics);
    }
}

TEST_F(GraphicsPresetStartup, LegacySelectionMigratesWithoutAutomaticRedetection) {
    const std::string legacy = R"({"user":{"video":{"render_scale":0.8}}})";
    ASSERT_NO_FATAL_FAILURE(Write(overlay.Path(), legacy));
    auto config = Load();
    ASSERT_EQ(config.settings_load_report().state, SettingsLoadState::MigratedLegacy);
    const auto before = config.user().graphics;
    ASSERT_NO_FATAL_FAILURE(Start(config));
    ExpectSameGraphics(config.user().graphics, before);
    EXPECT_EQ(Read(overlay.Path()), legacy);
    ASSERT_TRUE(config.SaveUserOverlay(overlay.Path()));
    ExpectSameGraphics(Load().user().graphics, before);
}

TEST_F(GraphicsPresetStartup, DamagedPrimaryFallsBackToDefaultsAndRemainsProtected) {
    SystemConfig defaults;
    ASSERT_TRUE(SelectGraphicsPreset(defaults.user().graphics, GraphicsPreset::Medium));
    defaults.user().graphics.overrides = {{"ui_blur", std::string("off")}};
    ASSERT_TRUE(defaults.SaveUserOverlay(DefaultsPath()));
    const auto defaults_bytes = Read(DefaultsPath());
    for (const auto* damaged : {"",
                                "{",
                                "[]",
                                "{\"user\":{",
                                R"({"user":null})",
                                R"({"settings_schema_version":999,"user":{}})"}) {
        SCOPED_TRACE(damaged);
        ASSERT_NO_FATAL_FAILURE(Write(overlay.Path(), damaged));
        auto config = Load();
        ASSERT_EQ(config.settings_load_report().state, SettingsLoadState::Recovery);
        ASSERT_FALSE(config.settings_load_report().needs_first_launch_detection);
        EXPECT_FALSE(config.settings_load_report().recovery_reason.empty());
        ASSERT_NO_FATAL_FAILURE(Start(config));
        ExpectSameGraphics(config.user().graphics, defaults.user().graphics);
        EXPECT_EQ(Read(overlay.Path()), damaged);
        EXPECT_FALSE(config.SaveUserOverlay(overlay.Path()));
        EXPECT_EQ(Read(overlay.Path()), damaged);
        EXPECT_EQ(Read(DefaultsPath()), defaults_bytes);
        ASSERT_NO_FATAL_FAILURE(Start(config, true));
        EXPECT_EQ(config.user().graphics.base_preset, GraphicsPreset::Low);
        EXPECT_FALSE(config.SaveUserOverlay(overlay.Path()));
        EXPECT_EQ(Read(overlay.Path()), damaged);
    }
}

TEST_F(GraphicsPresetStartup, UnreadablePrimaryIsRecoveryAndIsNeverReplaced) {
    ASSERT_TRUE(std::filesystem::create_directory(overlay.Path()));
    auto config = Load();
    ASSERT_EQ(config.settings_load_report().state, SettingsLoadState::Recovery);
    const auto before = config.user().graphics;
    ASSERT_NO_FATAL_FAILURE(Start(config));
    ExpectSameGraphics(config.user().graphics, before);
    EXPECT_FALSE(config.SaveUserOverlay(overlay.Path()));
    EXPECT_TRUE(std::filesystem::is_directory(overlay.Path()));
    EXPECT_TRUE(std::filesystem::is_empty(overlay.Path()));
}

TEST_F(GraphicsPresetStartup, ExplicitDetectionReplacesAnExistingChoiceAndCanBeSaved) {
    SystemConfig saved;
    ASSERT_TRUE(SelectGraphicsPreset(saved.user().graphics, GraphicsPreset::High));
    saved.user().graphics.overrides = {{"render_scale", 0.75}};
    ASSERT_TRUE(saved.SaveUserOverlay(overlay.Path()));
    auto config = Load();
    ASSERT_NO_FATAL_FAILURE(Start(config, true));
    EXPECT_EQ(config.user().graphics.base_preset, GraphicsPreset::Low);
    EXPECT_TRUE(config.user().graphics.overrides.empty());
    ASSERT_TRUE(config.SaveUserOverlay(overlay.Path()));
    ExpectSameGraphics(Load().user().graphics, config.user().graphics);
}
} // namespace
