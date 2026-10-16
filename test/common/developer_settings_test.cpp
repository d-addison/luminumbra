#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/core/LogDispatcherSink.h"
#include "luminumbra_common/core/SystemConfig.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>

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
TEST_F(DeveloperPersistence, DirectConstructionMatchesFactoryDefaultsAndInitializesRecords) {
    SystemConfig direct;
    SystemConfig value{};
    const auto defaults = SystemConfig::Defaults();
    for (auto* cfg : {&direct, &value}) {
        EXPECT_EQ(cfg->ComputeConfigSubHash(), defaults.ComputeConfigSubHash());
        EXPECT_EQ(cfg->SimulationGates(), defaults.SimulationGates());
        EXPECT_TRUE(cfg->user().developer.widgets.empty());
        EXPECT_TRUE(cfg->user().developer.bindings.empty());
        EXPECT_TRUE(cfg->user().developer.layouts.empty());
        cfg->user().developer.widgets["perf.hud"] = false;
        cfg->user().developer.bindings["console.toggle"] = {301, 2};
        cfg->user().developer.layouts["console.toggle"] = {70, 40, 820, 650, false};
        EXPECT_FALSE(cfg->user().developer.PerfHudVisible());
        EXPECT_EQ(cfg->user().developer.bindings.at("console.toggle").key, 301);
        EXPECT_FALSE(cfg->user().developer.layouts.at("console.toggle").open);
        EXPECT_EQ(cfg->ComputeConfigSubHash(), defaults.ComputeConfigSubHash());
    }
}

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
    auto defaults_config = SystemConfig::Defaults();
    const auto default_hash = defaults_config.ComputeConfigSubHash();
    EXPECT_FALSE(default_hash.empty());
    defaults_config.user().developer = cfg.user().developer;
    EXPECT_EQ(defaults_config.ComputeConfigSubHash(), default_hash);
    nlohmann::json disabled;
    for (const auto& [name, enabled] : defaults_config.SimulationGates()) {
        EXPECT_TRUE(enabled);
        disabled["sim"][name]["enabled"] = false;
    }
    auto disabled_config = SystemConfig::FromJsonString(disabled.dump());
    EXPECT_TRUE(disabled_config.ComputeConfigSubHash().empty());
    disabled_config.user().developer = cfg.user().developer;
    EXPECT_TRUE(disabled_config.ComputeConfigSubHash().empty());
}
TEST_F(DeveloperPersistence, EffectModulesDiscardDuplicateVisibilityBindings) {
    auto cfg = SystemConfig::FromJsonString(R"({"user":{"developer":{"bindings":{
        "render.debug_views":{"key":-1,"mods":0},
        "render.wireframe":{"key":-1,"mods":0},
        "runtime.time":{"key":-1,"mods":0},
        "render.debug-cycle":{"key":295,"mods":0},
        "future.action":{"key":303,"mods":2}
    }}}})");
    ASSERT_EQ(cfg.user().developer.bindings.size(), 2u);
    EXPECT_TRUE(cfg.user().developer.bindings.contains("render.debug-cycle"));
    EXPECT_TRUE(cfg.user().developer.bindings.contains("future.action"));
    cfg.user().developer.bindings["runtime.time"] = {-1, 0};
    const auto path = directory / "effect-bindings.json";
    ASSERT_TRUE(cfg.SaveUserOverlay(path.string()));
    const auto loaded = SystemConfig::LoadFromFile(path.string());
    EXPECT_EQ(loaded.user().developer.bindings.size(), 2u);
}

TEST_F(DeveloperPersistence, PanelLayoutsRoundTripWithOpenStateAndNoHashChanges) {
    auto cfg = SystemConfig::FromJsonString(R"({"sim":{"plant_growth":{"enabled":false}}})");
    const auto hash = cfg.ComputeConfigSubHash();
    const luminumbra::core::DeveloperPanelLayout shaders{125, 80, 640, 460, true};
    const luminumbra::core::DeveloperPanelLayout profiler{310, 150, 720, 540, false};
    cfg.user().developer.layouts["render.shaders"] = shaders;
    cfg.user().developer.layouts["perf.gpu"] = profiler;
    cfg.user().developer.layouts["console.toggle"] = {70, 40, 820, 650, false};
    cfg.user().developer.widgets["render.shaders"] = true;
    EXPECT_EQ(cfg.ComputeConfigSubHash(), hash);
    const auto path = directory / "layout.json";
    ASSERT_TRUE(cfg.SaveUserOverlay(path.string()));
    auto loaded = SystemConfig::FromJsonString(R"({"sim":{"plant_growth":{"enabled":false}}})");
    std::ifstream file(path);
    std::ostringstream json;
    json << file.rdbuf();
    loaded.OverlayUserFromJsonString(json.str());
    EXPECT_EQ(loaded.user().developer.layouts.at("render.shaders"), shaders);
    EXPECT_EQ(loaded.user().developer.layouts.at("perf.gpu"), profiler);
    EXPECT_FALSE(loaded.user().developer.layouts.at("console.toggle").open);
    EXPECT_TRUE(loaded.user().developer.widgets.at("render.shaders"));
    EXPECT_EQ(loaded.ComputeConfigSubHash(), hash);
    loaded.OverlayUserFromJsonString(R"({"user":{"video":{"fov":78}}})");
    EXPECT_EQ(loaded.user().developer.layouts.at("render.shaders"), shaders);
}

TEST_F(DeveloperPersistence, AbsentLayoutsUseDefaultsAndMalformedEntriesLogWithoutRefusingFile) {
    const auto old = SystemConfig::FromJsonString(R"({"user":{"video":{"fov":75}}})");
    EXPECT_TRUE(old.user().developer.layouts.empty());
    EXPECT_FLOAT_EQ(old.user().fov, 75);
    std::ostringstream messages;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages);
    auto dispatcher = Log::GetDispatcherSink();
    dispatcher->Attach(sink);
    const auto path = directory / "mixed-layout.json";
    {
        std::ofstream file(path);
        file << R"({"user":{"video":{"fov":82},"controls":{"Jump":90},"developer":{"layouts":{
            "render.shaders":{"x":130,"y":90,"width":600,"height":400,"open":true},
            "perf.gpu":{"x":20,"y":40,"width":-1,"height":500,"open":true},
            "wrong-type":{"x":"left","y":0,"width":600,"height":400,"open":false},
            "missing-field":{"x":0,"y":0,"width":600,"open":false},
            "wrong-open":{"x":0,"y":0,"width":600,"height":400,"open":1},
            "overflow":{"x":1e400,"y":0,"width":600,"height":400,"open":true},
            "zero-size":{"x":0,"y":0,"width":0,"height":400,"open":true},
            "wrong-record":[]
        }}}})";
    }
    auto cfg = SystemConfig::LoadFromFile(path.string());
    cfg.OverlayUserFromJsonString(R"({"user":{"developer":{"layouts":[]}}})");
    cfg.OverlayUserFromJsonString(R"({"user":{"developer":{"layouts":{"render.shaders":null}}}})");
    dispatcher->Detach(sink);
    ASSERT_EQ(cfg.user().developer.layouts.size(), 1u);
    EXPECT_TRUE(cfg.user().developer.layouts.at("render.shaders").open);
    EXPECT_FLOAT_EQ(cfg.user().developer.layouts.at("render.shaders").x, 130);
    EXPECT_FLOAT_EQ(cfg.user().fov, 82);
    EXPECT_EQ(cfg.keybind("Jump", 32), 90);
    for (const auto* id : {"perf.gpu",
                           "wrong-type",
                           "missing-field",
                           "wrong-open",
                           "overflow",
                           "zero-size",
                           "wrong-record",
                           "render.shaders"})
        EXPECT_NE(
            messages.str().find(std::string("Ignoring malformed developer layout entry: ") + id),
            std::string::npos);
    EXPECT_NE(messages.str().find("Ignoring malformed developer layouts object"),
              std::string::npos);
}

TEST_F(DeveloperPersistence, LayoutNumericOverflowLogsAndPreservesSiblingsInBothParsers) {
    const std::string json = R"({"\u0075ser":{
        "video":{"fov":82},"audio":{"master":0.4},"controls":{"Jump":90},
        "developer":{"widgets":{"perf.hud":{"visible":false}},
        "bindings":{"console.toggle":{"key":301,"mods":2}},"layouts":{
            "console.toggle":{"x":50,"y":60,"width":820,"height":650,"open":false},
            "overflow.x":{"x":1e400,"y":0,"width":600,"height":400,"open":true},
            "overflow.y":{"x":0,"y":-1e400,"width":600,"height":400,"open":true},
            "overflow.width":{"x":0,"y":0,"width":1e400,"height":400,"open":true},
            "overflow.height":{"x":0,"y":0,"width":600,"height":1e400,"open":true},
            "overflow.open":{"x":0,"y":0,"width":600,"height":400,"open":1e400},
            "overflow.extra":{"x":0,"y":0,"width":600,"height":400,"open":true,
                              "extra":{"nested":[1e400]}},
            "overflow.record":1e400,
            "perf.gpu":{"x":1e400,"y":-1e400,"width":600,"height":400,"open":true},
            "render.shaders":{"x":130,"y":90,"width":600,"height":400,"open":true}
        }}}})";
    const luminumbra::core::DeveloperPanelLayout previous{20, 30, 640, 480, false};
    for (const bool overlay : {false, true}) {
        SCOPED_TRACE(overlay ? "OverlayUserFromJsonString" : "FromJsonString");
        auto cfg = SystemConfig::Defaults();
        if (overlay)
            cfg.user().developer.layouts["perf.gpu"] = previous;
        std::ostringstream messages;
        const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages);
        auto dispatcher = Log::GetDispatcherSink();
        dispatcher->Attach(sink);
        EXPECT_NO_THROW({
            if (overlay)
                cfg.OverlayUserFromJsonString(json);
            else
                cfg = SystemConfig::FromJsonString(json);
        });
        dispatcher->Detach(sink);

        ASSERT_EQ(cfg.user().developer.layouts.size(), overlay ? 3u : 2u);
        EXPECT_FLOAT_EQ(cfg.user().developer.layouts.at("render.shaders").x, 130);
        EXPECT_FALSE(cfg.user().developer.layouts.at("console.toggle").open);
        if (overlay) {
            EXPECT_EQ(cfg.user().developer.layouts.at("perf.gpu"), previous);
        }
        EXPECT_FLOAT_EQ(cfg.user().fov, 82);
        EXPECT_FLOAT_EQ(cfg.user().audio_master, 0.4f);
        EXPECT_EQ(cfg.keybind("Jump", 32), 90);
        EXPECT_FALSE(cfg.user().developer.PerfHudVisible());
        ASSERT_TRUE(cfg.user().developer.bindings.contains("console.toggle"));
        EXPECT_EQ(cfg.user().developer.bindings.at("console.toggle").key, 301);
        for (const auto* id : {"overflow.x",
                               "overflow.y",
                               "overflow.width",
                               "overflow.height",
                               "overflow.open",
                               "overflow.extra",
                               "overflow.record",
                               "perf.gpu"})
            EXPECT_NE(messages.str().find(
                          std::string("Ignoring malformed developer layout entry: ") + id),
                      std::string::npos);
    }
}

TEST_F(DeveloperPersistence, MalformedLayoutsPreserveSiblingSettingsInBothParsers) {
    for (const auto* layouts :
         {R"([1e400])",
          R"(1e400)",
          R"([{"nested":[1e400,-1e400]}])",
          R"({"perf/gpu~future":{"x":0,"y":0,"width":600,"height":400,"open":true,
                                    "extra":{"nested":[1e400]}}})",
          R"("invalid")",
          R"([])",
          R"(42)",
          R"(false)",
          R"(null)"}) {
        SCOPED_TRACE(layouts);
        const auto json = std::string(R"({"user":{"developer":{"layouts":)") + layouts +
                          R"(},"video":{"fov":82}}})";
        for (const bool overlay : {false, true}) {
            SCOPED_TRACE(overlay ? "OverlayUserFromJsonString" : "FromJsonString");
            auto cfg = SystemConfig::Defaults();
            std::ostringstream messages;
            const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages);
            auto dispatcher = Log::GetDispatcherSink();
            dispatcher->Attach(sink);
            EXPECT_NO_THROW({
                if (overlay)
                    cfg.OverlayUserFromJsonString(json);
                else
                    cfg = SystemConfig::FromJsonString(json);
            });
            dispatcher->Detach(sink);
            EXPECT_FLOAT_EQ(cfg.user().fov, 82);
            EXPECT_TRUE(cfg.user().developer.layouts.empty());
            EXPECT_NE(messages.str().find("Ignoring malformed developer layout"),
                      std::string::npos);
        }
    }
}

TEST_F(DeveloperPersistence, LayoutRecoveryDoesNotSwallowOverflowOutsideLayouts) {
    for (const auto* json :
         {R"({"sim":{"plant_growth":{"params":{"mutation_rate":1e400}}}})",
          R"({"render":{"moonlight":{"params":{"strength":1e400}}}})",
          R"({"user":{"video":{"fov":1e400}}})",
          R"({"user":{"audio":{"master":1e400}}})",
          R"({"user":{"controls":{"Jump":1e400}}})",
          R"({"user":{"developer":{"bindings":{"console.toggle":{"key":1e400,"mods":0}}}}})",
          R"({"user":{"developer":{"widgets":{"perf.hud":{"visible":1e400}}}}})",
          R"({"other":{"user":{"developer":{"layouts":{"bad":{"x":1e400}}}}}})",
          R"({"user":[{"developer":{"layouts":{"bad":{"x":1e400}}}}]})",
          R"({"user":{"developer":[{"layouts":[1e400]}]}})",
          R"({"user":{"developer":{"layouts":[1e400]},"video":{"fov":1e400}}})",
          R"({"user":{"developer":{"layouts":{"bad":1e400}},"video":{"fov":1e400}}})"}) {
        SCOPED_TRACE(json);
        EXPECT_THROW(static_cast<void>(SystemConfig::FromJsonString(json)),
                     nlohmann::json::out_of_range);
        auto cfg = SystemConfig::Defaults();
        EXPECT_THROW(cfg.OverlayUserFromJsonString(json), nlohmann::json::out_of_range);
    }
}

TEST_F(DeveloperPersistence, LayoutRecoveryDoesNotSwallowOtherSectionTypeErrors) {
    const std::string json = R"({
        "user":{"developer":{"layouts":{"bad":{"x":1e400}}}},
        "render":{"moonlight":{"params":{"color":["invalid",0.7,1.0]}}}
    })";
    EXPECT_THROW(static_cast<void>(SystemConfig::FromJsonString(json)), nlohmann::json::type_error);
}

TEST_F(DeveloperPersistence, LayoutRecordsAreBoundedAndKnownPanelsSurviveUnknownEntries) {
    std::string records;
    for (int i = 0; i < 400; ++i) {
        if (i)
            records += ',';
        records += "\"future." + std::to_string(i) +
                   "\":{\"x\":10,\"y\":20,\"width\":300,\"height\":200,\"open\":false}";
    }
    auto cfg =
        SystemConfig::FromJsonString("{\"user\":{\"developer\":{\"layouts\":{" + records + "}}}}");
    ASSERT_EQ(cfg.user().developer.layouts.size(), DeveloperSettings::kRecordLimit);
    cfg.OverlayUserFromJsonString(R"({"user":{"developer":{"layouts":{
        "render.shaders":{"x":30,"y":50,"width":600,"height":400,"open":true}
    }}}})");
    EXPECT_TRUE(cfg.user().developer.layouts.at("render.shaders").open);
    ASSERT_TRUE(cfg.SaveUserOverlay((directory / "bounded-layout.json").string()));
    const auto loaded = SystemConfig::LoadFromFile((directory / "bounded-layout.json").string());
    EXPECT_EQ(loaded.user().developer.layouts.size(), DeveloperSettings::kRecordLimit);
    EXPECT_TRUE(loaded.user().developer.layouts.at("render.shaders").open);
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
