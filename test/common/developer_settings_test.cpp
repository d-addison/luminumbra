#include "luminumbra_common/core/SystemConfig.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

using luminumbra::core::DeveloperSettings;
using luminumbra::core::SystemConfig;

namespace {
class DeveloperPersistence : public testing::Test {
protected:
    std::filesystem::path directory;
    void SetUp() override {
        static std::atomic<unsigned> serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory =
            std::filesystem::path(LUMINUMBRA_TEST_ARTIFACT_DIR) /
            ("developer-settings-" + std::to_string(stamp) + "-" + std::to_string(serial++));
        std::filesystem::create_directories(directory);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};
TEST_F(DeveloperPersistence, OldSettingsDefaultsAndFalseRoundTrip) {
    auto cfg = SystemConfig::FromJsonString(
        R"({"user":{"video":{"fov":70,"vsync":true},"audio":{"master":0.5},"controls":{"Jump":90}}})");
    EXPECT_TRUE(cfg.user().developer.PerfHudVisible());
    cfg.user().developer.widgets["perf.hud"] = false;
    cfg.user().developer.bindings["console.toggle"] = {301, 2};
    cfg.user().developer.bindings["perf.gpu"] = {-1, 0};
    const auto path = directory / "settings.json";
    ASSERT_TRUE(cfg.SaveUserOverlay(path.string()));
    const auto loaded = SystemConfig::LoadFromFile(path.string());
    EXPECT_FALSE(loaded.user().developer.PerfHudVisible());
    EXPECT_FLOAT_EQ(loaded.user().fov, 70);
    EXPECT_TRUE(loaded.user().vsync);
    EXPECT_FLOAT_EQ(loaded.user().audio_master, 0.5f);
    EXPECT_EQ(loaded.keybind("Jump", 32), 90);
    EXPECT_EQ(loaded.user().developer.bindings.at("console.toggle").key, 301);
    EXPECT_EQ(loaded.user().developer.bindings.at("console.toggle").mods, 2);
    EXPECT_EQ(loaded.user().developer.bindings.at("perf.gpu").key, -1);
}
TEST_F(DeveloperPersistence, PerfSwitchRoundTripDoesNotChangeOtherSettingsOrHash) {
    auto cfg = SystemConfig::FromJsonString(
        R"({"sim":{"plant_growth":{"enabled":true}},"user":{"video":{"fov":80},"audio":{"master":0.4},"controls":{"Jump":90}}})");
    const auto hash = cfg.ComputeConfigSubHash();
    const auto path = directory / "perf-switch.json";
    EXPECT_TRUE(cfg.user().developer.PerfHudVisible());
    for (const bool visible : {false, true, false}) {
        cfg.user().developer.widgets["perf.hud"] = visible;
        ASSERT_TRUE(cfg.SaveUserOverlay(path.string()));
        const auto loaded = SystemConfig::LoadFromFile(path.string());
        cfg.user().developer = loaded.user().developer;
        EXPECT_EQ(cfg.user().developer.PerfHudVisible(), visible);
        EXPECT_EQ(cfg.ComputeConfigSubHash(), hash);
        EXPECT_FLOAT_EQ(cfg.user().fov, 80);
        EXPECT_FLOAT_EQ(cfg.user().audio_master, 0.4f);
        EXPECT_EQ(cfg.keybind("Jump", 32), 90);
    }
}

TEST_F(DeveloperPersistence, IndependentMalformedRecordsAndBoundedUnknownRecords) {
    auto cfg = SystemConfig::FromJsonString(
        R"({"user":{"developer":{"schema_version":1,"widgets":{"perf.hud":{"visible":false},"bad":{"visible":1},"future.widget":{"visible":true}},"bindings":{"console.toggle":{"key":290,"mods":0},"bad":{"key":"F1","mods":0},"modifier":{"key":340,"mods":1},"oversized":{"key":18446744073709551615,"mods":0},"invalid.mods":{"key":300,"mods":16},"future.action":{"key":302,"mods":2}}}}})");
    EXPECT_FALSE(cfg.user().developer.PerfHudVisible());
    EXPECT_EQ(cfg.user().developer.widgets.size(), 2u);
    EXPECT_EQ(cfg.user().developer.bindings.size(), 2u);
    ASSERT_TRUE(cfg.SaveUserOverlay((directory / "unknown.json").string()));
    const auto loaded = SystemConfig::LoadFromFile((directory / "unknown.json").string());
    EXPECT_TRUE(loaded.user().developer.widgets.at("future.widget"));
    EXPECT_EQ(loaded.user().developer.bindings.at("future.action").key, 302);
    std::string records;
    for (int i = 0; i < 400; ++i) {
        if (i)
            records += ',';
        records += "\"future." + std::to_string(i) + "\":{\"visible\":true}";
    }
    cfg.OverlayUserFromJsonString("{\"user\":{\"developer\":{\"widgets\":{" + records + "}}}}");
    EXPECT_EQ(cfg.user().developer.widgets.size(), DeveloperSettings::kRecordLimit);
    // Existing and newly supplied recognized records survive a full unknown-id budget.
    cfg.OverlayUserFromJsonString(
        R"({"user":{"developer":{"widgets":{"world.generation":{"visible":true}}}}})");
    EXPECT_TRUE(cfg.user().developer.widgets.at("world.generation"));
    EXPECT_FALSE(cfg.user().developer.PerfHudVisible());
    ASSERT_TRUE(cfg.SaveUserOverlay((directory / "bounded.json").string()));
    const auto bounded = SystemConfig::LoadFromFile((directory / "bounded.json").string());
    EXPECT_TRUE(bounded.user().developer.widgets.at("world.generation"));
    EXPECT_FALSE(bounded.user().developer.PerfHudVisible());

    std::string actions;
    for (int i = 0; i < 400; ++i) {
        if (i)
            actions += ',';
        actions += "\"future." + std::to_string(i) + "\":{\"key\":300,\"mods\":0}";
    }
    cfg.OverlayUserFromJsonString("{\"user\":{\"developer\":{\"bindings\":{" + actions + "}}}}");
    EXPECT_EQ(cfg.user().developer.bindings.size(), DeveloperSettings::kRecordLimit);
    cfg.OverlayUserFromJsonString(
        R"({"user":{"developer":{"bindings":{"shader.reload":{"key":303,"mods":2}}}}})");
    EXPECT_EQ(cfg.user().developer.bindings.at("shader.reload").key, 303);
    EXPECT_EQ(cfg.user().developer.bindings.at("console.toggle").key, 290);
    cfg.OverlayUserFromJsonString("{\"user\":{\"developer\":{\"widgets\":{\"" +
                                  std::string(129, 'x') + "\":{\"visible\":true}}}}}");
    EXPECT_EQ(cfg.user().developer.widgets.size(), DeveloperSettings::kRecordLimit);
}
TEST_F(DeveloperPersistence, UnsignedOverflowAndNegativeValuesKeepDefaultBinding) {
    for (const auto* key :
         {"18446744073709551615", "18446744073709551614", "349", "-2", "-9223372036854775808"}) {
        const auto cfg = SystemConfig::FromJsonString(
            std::string(R"({"user":{"developer":{"bindings":{"console.toggle":{"key":)") + key +
            R"(,"mods":0}}}}})");
        EXPECT_FALSE(cfg.user().developer.bindings.contains("console.toggle")) << key;
    }
    for (const auto* mods : {"18446744073709551615", "16", "-1", "-9223372036854775808"}) {
        const auto cfg = SystemConfig::FromJsonString(
            std::string(
                R"({"user":{"developer":{"bindings":{"console.toggle":{"key":290,"mods":)") +
            mods + R"(}}}}})");
        EXPECT_FALSE(cfg.user().developer.bindings.contains("console.toggle")) << mods;
    }
    const auto cfg = SystemConfig::FromJsonString(
        R"({"user":{"developer":{"bindings":{"console.toggle":{"key":-1,"mods":0},"perf.gpu":{"key":348,"mods":15}}}}})");
    EXPECT_EQ(cfg.user().developer.bindings.at("console.toggle").key, -1);
    EXPECT_EQ(cfg.user().developer.bindings.at("perf.gpu").key, 348);
    EXPECT_EQ(cfg.user().developer.bindings.at("perf.gpu").mods, 15);
    auto overlay = SystemConfig::Defaults();
    overlay.user().developer.bindings["console.toggle"] = {290, 0};
    overlay.OverlayUserFromJsonString(
        R"({"user":{"developer":{"bindings":{"console.toggle":{"key":18446744073709551615,"mods":0}}}}})");
    EXPECT_EQ(overlay.user().developer.bindings.at("console.toggle").key, 290);
}

TEST_F(DeveloperPersistence, MergePrecedenceAndHashExclusion) {
    const auto defaults = directory / "defaults.json", overlay = directory / "overlay.json";
    {
        std::ofstream file(defaults);
        file
            << R"({"sim":{"plant_growth":{"enabled":true}},"user":{"video":{"fov":65},"developer":{"widgets":{"perf.hud":{"visible":true},"perf.gpu":{"visible":false}}}}})";
    }
    {
        std::ofstream file(overlay);
        file
            << R"({"sim":{"plant_growth":{"enabled":false}},"user":{"developer":{"widgets":{"perf.hud":{"visible":false}},"bindings":{"console.toggle":{"key":301,"mods":2}}}}})";
    }
    const auto baseline = SystemConfig::LoadFromFile(defaults.string());
    auto cfg = SystemConfig::LoadLayered(defaults.string(), overlay.string());
    EXPECT_FLOAT_EQ(cfg.user().fov, 65);
    EXPECT_FALSE(cfg.user().developer.PerfHudVisible());
    EXPECT_FALSE(cfg.user().developer.widgets.at("perf.gpu"));
    EXPECT_EQ(cfg.ComputeConfigSubHash(), baseline.ComputeConfigSubHash());
    cfg.user().developer.widgets["world.generation"] = true;
    cfg.user().developer.bindings["console.toggle"] = {-1, 0};
    EXPECT_EQ(cfg.ComputeConfigSubHash(), baseline.ComputeConfigSubHash());
    cfg.OverlayUserFromJsonString("malformed");
    EXPECT_FALSE(cfg.user().developer.PerfHudVisible());
    EXPECT_TRUE(SystemConfig::Defaults().ComputeConfigSubHash().empty());
}
TEST_F(DeveloperPersistence, SaveFailureRetainsPreferencesAndSupportsRetry) {
    auto cfg = SystemConfig::Defaults();
    cfg.user().developer.widgets["perf.hud"] = false;
    const auto blocker = directory / "blocker";
    {
        std::ofstream file(blocker);
        file << "file";
    }
    EXPECT_FALSE(cfg.SaveUserOverlay((blocker / "settings.json").string()));
    EXPECT_FALSE(cfg.user().developer.PerfHudVisible());
    ASSERT_TRUE(cfg.SaveUserOverlay((directory / "retry.json").string()));
    EXPECT_FALSE(SystemConfig::LoadFromFile((directory / "retry.json").string())
                     .user()
                     .developer.PerfHudVisible());
}
} // namespace
