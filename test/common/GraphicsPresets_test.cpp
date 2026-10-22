#include "../support/UniqueTempPath.h"
#include "luminumbra_client/core/FoliageVisualContract.h"
#include "luminumbra_client/rendering/RenderingDefaults.h"
#include "luminumbra_common/core/GraphicsPresets.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <sstream>
#include <string>

namespace luminumbra::core {
namespace {

using Luminumbra::Persistence::WorldSaveService;
using Luminumbra::TestSupport::ScopedTempFile;

class OverlayWriteReset {
public:
    ~OverlayWriteReset() {
        WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    }
};

void ExpectNoOverlayStaging(const std::filesystem::path& destination) {
    auto prefix = destination.filename();
    prefix += ".tmp.";
    for (const auto& entry : std::filesystem::directory_iterator(destination.parent_path()))
        EXPECT_NE(entry.path().filename().native().find(prefix.native()), 0u);
    auto legacy = destination;
    legacy += ".tmp";
    EXPECT_FALSE(std::filesystem::exists(legacy));
}

std::string ReadText(const std::filesystem::path& path) {
    const std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    file.close();
    ASSERT_FALSE(file.fail());
}

nlohmann::json CurrentDocument() {
    return {{"settings_schema_version", kSettingsSchemaVersion},
            {"user",
             {{"graphics",
               {{"base_preset", "ultra"},
                {"preset_revision", kGraphicsPresetRevision},
                {"overrides", nlohmann::json::object()}}}}}};
}

TEST(GraphicsPresets, CompleteUltraSnapshotPreservesShippingPresentation) {
    ResolvedGraphics resolved;
    std::string error;
    ASSERT_TRUE(ResolveGraphics(GraphicsSettings{}, {}, resolved, error)) << error;
    const auto& v = resolved.values;
    EXPECT_DOUBLE_EQ(v.render_scale, 1.0);
    EXPECT_TRUE(v.grass_enabled);
    EXPECT_DOUBLE_EQ(v.grass_density, 1.6);
    EXPECT_DOUBLE_EQ(v.grass_fade_start_m, 48.0);
    EXPECT_DOUBLE_EQ(v.grass_fade_end_m, 92.0);
    EXPECT_EQ(v.grass_candidates_per_column, 8192);
    EXPECT_EQ(v.grass_instance_cap, 262144);
    EXPECT_DOUBLE_EQ(v.far_terrain_radius_m, 3000.0);
    EXPECT_DOUBLE_EQ(v.far_outer_clip_m, 3050.0);
    EXPECT_DOUBLE_EQ(v.far_f1_boundary_m, 768.0);
    EXPECT_DOUBLE_EQ(v.far_inner_clip_m, 176.0);
    EXPECT_EQ(v.shadow_resolution, 2048);
    EXPECT_EQ(v.shadow_cascades, 4);
    EXPECT_EQ(v.shadow_splits_m, (std::array<double, 5>{0.1, 15.0, 40.0, 100.0, 250.0}));
    EXPECT_TRUE(v.tinted_shadows);
    EXPECT_EQ(v.ao_quality, 3);
    EXPECT_EQ(v.cloud_quality, 2);
    EXPECT_EQ(v.volumetric_quality, 1);
    EXPECT_TRUE(v.analytic_aerial);
    EXPECT_TRUE(v.cave_ao);
    EXPECT_DOUBLE_EQ(v.cave_ao_distance_m, 24.0);
    EXPECT_DOUBLE_EQ(v.cave_ao_floor, 0.06);
    EXPECT_DOUBLE_EQ(v.cave_ao_thickness, 1.5);
    EXPECT_EQ(v.cave_ao_steps, 8);
    EXPECT_TRUE(v.god_rays);
    EXPECT_EQ(v.god_ray_samples, 24);
    EXPECT_TRUE(v.taau);
    EXPECT_TRUE(v.gpu_sky_lut);
    EXPECT_FALSE(v.exposure_meter);
    EXPECT_EQ(v.water_ssr_min_steps, 4);
    EXPECT_EQ(v.water_ssr_max_steps, 8);
    EXPECT_EQ(v.water_caustics_resolution, 256);
    EXPECT_EQ(v.ui_blur, UiBlurQuality::Full);
    EXPECT_EQ(v.ui_msaa_samples, 2);
    EXPECT_TRUE(v.live_menu_preview);
    EXPECT_FALSE(resolved.custom);
    EXPECT_TRUE(resolved.session_overrides.empty());
    EXPECT_EQ(resolved.base_preset, GraphicsPreset::Ultra);
    EXPECT_EQ(resolved.preset_revision, 1u);

    // Cross-check shared shipping contracts without constructing GL resources.
    using Luminumbra::Client::ScenarioHarness::FoliageVisualProfile;
    EXPECT_FLOAT_EQ(static_cast<float>(v.grass_density), FoliageVisualProfile::density_scale);
    EXPECT_FLOAT_EQ(static_cast<float>(v.grass_fade_start_m), FoliageVisualProfile::fade_start_m);
    EXPECT_FLOAT_EQ(static_cast<float>(v.grass_fade_end_m), FoliageVisualProfile::fade_end_m);
    EXPECT_EQ(v.volumetric_quality, Luminumbra::Rendering::kDefaultVolumetricQuality);
    const Luminumbra::Rendering::CaveAmbientOcclusion cave;
    EXPECT_FLOAT_EQ(static_cast<float>(v.cave_ao_distance_m), cave.maxDist);
    EXPECT_FLOAT_EQ(static_cast<float>(v.cave_ao_floor), cave.floor);
    EXPECT_FLOAT_EQ(static_cast<float>(v.cave_ao_thickness), cave.thickness);
    EXPECT_EQ(v.cave_ao_steps, cave.steps);
}

TEST(GraphicsPresets, EveryPresetMatchesApprovedValuesIncludingAllUnchangedRows) {
    struct Row {
        double scale;
        double density;
        double fade_start;
        double fade_end;
        double radius;
        int shadows;
        int volumetrics;
        GraphicsPreset preset;
        UiBlurQuality blur;
    };
    constexpr Row rows[]{
        {0.67, 0.4, 24.0, 48.0, 1500.0, 1024, 0, GraphicsPreset::Low, UiBlurQuality::Off},
        {0.80, 0.8, 32.0, 64.0, 2250.0, 1536, 0, GraphicsPreset::Medium, UiBlurQuality::Half},
        {1.0, 1.2, 40.0, 80.0, 3000.0, 2048, 1, GraphicsPreset::High, UiBlurQuality::Full},
        {1.0, 1.6, 48.0, 92.0, 3000.0, 2048, 1, GraphicsPreset::Ultra, UiBlurQuality::Full},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(GraphicsPresetName(row.preset));
        GraphicsSettings settings;
        ASSERT_TRUE(SelectGraphicsPreset(settings, row.preset));
        ResolvedGraphics resolved;
        std::string error;
        ASSERT_TRUE(ResolveGraphics(settings, {}, resolved, error)) << error;
        GraphicsValues expected;
        expected.render_scale = row.scale;
        expected.grass_density = row.density;
        expected.grass_fade_start_m = row.fade_start;
        expected.grass_fade_end_m = row.fade_end;
        expected.far_terrain_radius_m = row.radius;
        expected.far_outer_clip_m = row.radius + 50.0;
        expected.shadow_resolution = row.shadows;
        expected.volumetric_quality = row.volumetrics;
        expected.ui_blur = row.blur;
        EXPECT_EQ(resolved.values, expected);
        EXPECT_FALSE(resolved.custom);
    }
}

TEST(GraphicsPresets, OverridesCustomSessionPrecedenceSelectionAndReset) {
    GraphicsSettings settings;
    ASSERT_TRUE(SelectGraphicsPreset(settings, GraphicsPreset::Low));
    settings.overrides = {{"render_scale", 0.67}};
    ResolvedGraphics resolved;
    std::string error;
    ASSERT_TRUE(ResolveGraphics(settings, {}, resolved, error));
    EXPECT_FALSE(resolved.custom); // Explicit same-as-base is retained, not Custom.
    EXPECT_EQ(settings.overrides.size(), 1u);
    settings.overrides["render_scale"] = 0.8;
    settings.overrides["grass_enabled"] = false;
    settings.overrides["ui_blur"] = std::string("half");
    const GraphicsOverrides session{{"render_scale", 0.9}, {"ui_blur", std::string("full")}};
    ASSERT_TRUE(ResolveGraphics(settings, session, resolved, error));
    EXPECT_TRUE(resolved.custom);
    EXPECT_EQ(resolved.base_preset, GraphicsPreset::Low);
    EXPECT_DOUBLE_EQ(resolved.values.render_scale, 0.9);
    EXPECT_FALSE(resolved.values.grass_enabled);
    EXPECT_EQ(resolved.values.ui_blur, UiBlurQuality::Full);
    EXPECT_EQ(resolved.session_overrides, session);
    EXPECT_EQ(std::get<double>(settings.overrides.at("render_scale")), 0.8);
    const GraphicsOverrides restore_base{
        {"render_scale", 0.67}, {"grass_enabled", true}, {"ui_blur", std::string("off")}};
    ASSERT_TRUE(ResolveGraphics(settings, restore_base, resolved, error));
    EXPECT_DOUBLE_EQ(resolved.values.render_scale, 0.67);
    EXPECT_TRUE(resolved.values.grass_enabled);
    EXPECT_EQ(resolved.values.ui_blur, UiBlurQuality::Off);
    EXPECT_TRUE(resolved.custom); // Session overrides do not erase a saved Custom choice.
    ResetGraphicsOverrides(settings);
    ASSERT_TRUE(ResolveGraphics(settings, {}, resolved, error));
    EXPECT_FALSE(resolved.custom);
    EXPECT_DOUBLE_EQ(resolved.values.render_scale, 0.67);
    ASSERT_TRUE(ResolveGraphics(settings, session, resolved, error));
    EXPECT_TRUE(resolved.custom); // A session-only change is also visibly Custom.
    settings.overrides["grass_density"] = 0.9;
    ASSERT_TRUE(SelectGraphicsPreset(settings, GraphicsPreset::High));
    EXPECT_TRUE(settings.overrides.empty());
    ASSERT_TRUE(ResolveGraphics(settings, {}, resolved, error));
    EXPECT_DOUBLE_EQ(resolved.values.grass_density, 1.2);
    EXPECT_FALSE(resolved.custom);
    EXPECT_FALSE(SelectGraphicsPreset(settings, std::bit_cast<GraphicsPreset>(std::uint8_t{255})));
    EXPECT_EQ(settings.base_preset, GraphicsPreset::High);
    settings.overrides["render_scale"] = 0.75;
    ASSERT_TRUE(SelectGraphicsPreset(settings, GraphicsPreset::High));
    EXPECT_TRUE(settings.overrides.empty());
}

TEST(GraphicsPresets, RejectsInvalidNumbersTypesEnumsRangesAndLockedControlsTransactionally) {
    const GraphicsOverrides invalid[]{
        {{"render_scale", std::numeric_limits<double>::infinity()}},
        {{"render_scale", std::numeric_limits<double>::quiet_NaN()}},
        {{"render_scale", -std::numeric_limits<double>::infinity()}},
        {{"render_scale", 0.49}},
        {{"render_scale", 1.01}},
        {{"render_scale", true}},
        {{"grass_density", 0.0}},
        {{"grass_density", 2.01}},
        {{"grass_fade_start_m", -1.0}},
        {{"grass_fade_end_m", 161.0}},
        {{"grass_fade_start_m", 92.0}},
        {{"far_terrain_radius_m", 767.0}},
        {{"far_terrain_radius_m", 3001.0}},
        {{"far_outer_clip_m", 2000.0}},
        {{"shadow_resolution", 1536.5}},
        {{"shadow_resolution", 1e100}},
        {{"volumetric_quality", 2.0}},
        {{"grass_enabled", 1.0}},
        {{"ui_blur", std::string("invalid")}},
        {{"ui_blur", 1.0}},
        {{"sim.water_high_res", false}},
        {{"active_region_radius", 256.0}},
        {{"live_chunk_radius", 16.0}},
        {{"circadian_amplitude", 0.0}},
    };
    for (const auto& overrides : invalid) {
        SCOPED_TRACE(overrides.begin()->first);
        GraphicsSettings settings;
        settings.overrides = overrides;
        ResolvedGraphics resolved;
        resolved.values.render_scale = 0.75;
        std::string error;
        EXPECT_FALSE(ResolveGraphics(settings, {}, resolved, error));
        EXPECT_FALSE(error.empty());
        EXPECT_DOUBLE_EQ(resolved.values.render_scale, 0.75);
        settings.overrides.clear();
        EXPECT_FALSE(ResolveGraphics(settings, overrides, resolved, error));
        EXPECT_DOUBLE_EQ(resolved.values.render_scale, 0.75);
    }
    GraphicsSettings settings;
    ResolvedGraphics resolved;
    std::string error;
    settings.preset_revision = 2;
    EXPECT_FALSE(ResolveGraphics(settings, {}, resolved, error));
    settings.preset_revision = 1;
    settings.base_preset = std::bit_cast<GraphicsPreset>(std::uint8_t{255});
    EXPECT_FALSE(ResolveGraphics(settings, {}, resolved, error));
}

TEST(GraphicsPresets, SupportedOverrideBoundariesRemainValid) {
    for (const auto scale : {0.5, 1.0}) {
        for (const auto density : {0.1, 2.0}) {
            for (const auto radius : {768.0, 3000.0}) {
                GraphicsSettings settings;
                settings.overrides = {{"render_scale", scale},
                                      {"grass_density", density},
                                      {"grass_fade_start_m", 0.0},
                                      {"grass_fade_end_m", 160.0},
                                      {"far_terrain_radius_m", radius}};
                ResolvedGraphics resolved;
                std::string error;
                ASSERT_TRUE(ResolveGraphics(settings, {}, resolved, error)) << error;
                EXPECT_DOUBLE_EQ(resolved.values.render_scale, scale);
                EXPECT_DOUBLE_EQ(resolved.values.grass_density, density);
                EXPECT_DOUBLE_EQ(resolved.values.far_terrain_radius_m, radius);
                EXPECT_DOUBLE_EQ(resolved.values.far_outer_clip_m, radius + 50.0);
            }
        }
    }
}

TEST(GraphicsPresets, LegacyMigrationRetainsExplicitRenderScaleAndIndividualOwnership) {
    const ScopedTempFile file("graphics_", ".json");
    const std::string legacy = R"({"user":{"video":{"render_scale":0.85,"vsync":false,
        "frame_cap":144,"resolution":"1920x1080","window_mode":"windowed",
        "fov":70,"ui_scale":1.5},"audio":{"music":0.5},"controls":{"Jump":32}}})";
    WriteText(file.Path(), legacy);
    auto config = SystemConfig::LoadLayered("", file.Path().string());
    EXPECT_EQ(config.settings_load_report().state, SettingsLoadState::MigratedLegacy);
    EXPECT_FALSE(config.settings_load_report().needs_first_launch_detection);
    EXPECT_EQ(config.user().graphics.base_preset, GraphicsPreset::Ultra);
    ASSERT_EQ(config.user().graphics.overrides.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(config.user().graphics.overrides.at("render_scale")), 0.85);
    EXPECT_FLOAT_EQ(config.user().render_scale, 0.85f);
    ResolvedGraphics resolved;
    std::string error;
    ASSERT_TRUE(ResolveGraphics(config.user().graphics, {}, resolved, error));
    EXPECT_TRUE(resolved.custom);
    EXPECT_EQ(ReadText(file.Path()), legacy); // Startup never rewrites migration.
    config.OverlayUserFromJsonString(R"({"user":{"video":{"fov":75}}})");
    EXPECT_DOUBLE_EQ(std::get<double>(config.user().graphics.overrides.at("render_scale")), 0.85);
    ASSERT_TRUE(config.SaveUserOverlay(file.Path().string())); // Replace an existing overlay.
    const auto saved = nlohmann::json::parse(ReadText(file.Path()));
    EXPECT_EQ(saved.at("settings_schema_version").get<std::uint32_t>(), kSettingsSchemaVersion);
    EXPECT_FALSE(saved.at("user").at("video").contains("render_scale"));
    const auto reloaded = SystemConfig::LoadLayered("", file.Path().string());
    EXPECT_EQ(reloaded.settings_load_report().state, SettingsLoadState::Loaded);
    EXPECT_EQ(reloaded.user().graphics.overrides, config.user().graphics.overrides);
    EXPECT_FLOAT_EQ(reloaded.user().render_scale, 0.85f);
    EXPECT_FALSE(reloaded.user().vsync);
    EXPECT_EQ(reloaded.user().frame_cap, 144);
    EXPECT_EQ(reloaded.user().resolution, "1920x1080");
    EXPECT_EQ(reloaded.user().window_mode, "windowed");
    EXPECT_FLOAT_EQ(reloaded.user().fov, 75.0f);
    EXPECT_FLOAT_EQ(reloaded.user().ui_scale, 1.5f);
    EXPECT_FLOAT_EQ(reloaded.user().audio_music, 0.5f);
    EXPECT_EQ(reloaded.keybind("Jump", -1), 32);
}

TEST(GraphicsPresets, AbsentAndExplicitDefaultsRemainDistinctFromFirstLaunch) {
    const ScopedTempFile file("graphics_", ".json");
    const auto first = SystemConfig::LoadLayered("", file.Path().string());
    EXPECT_EQ(first.settings_load_report().state, SettingsLoadState::FirstLaunch);
    EXPECT_TRUE(first.settings_load_report().needs_first_launch_detection);
    EXPECT_FALSE(std::filesystem::exists(file.Path()));
    for (const bool explicit_scale : {false, true}) {
        auto document = nlohmann::json::parse(R"({"user":{"video":{}}})");
        if (explicit_scale)
            document["user"]["video"]["render_scale"] = 1.0;
        WriteText(file.Path(), document.dump());
        const auto config = SystemConfig::LoadLayered("", file.Path().string());
        EXPECT_EQ(config.settings_load_report().state, SettingsLoadState::MigratedLegacy);
        EXPECT_FALSE(config.settings_load_report().needs_first_launch_detection);
        EXPECT_EQ(config.user().graphics.overrides.contains("render_scale"), explicit_scale);
        ASSERT_TRUE(config.SaveUserOverlay(file.Path().string()));
        const auto reloaded = SystemConfig::LoadLayered("", file.Path().string());
        EXPECT_EQ(reloaded.user().graphics.overrides.contains("render_scale"), explicit_scale);
        ResolvedGraphics resolved;
        std::string error;
        ASSERT_TRUE(ResolveGraphics(reloaded.user().graphics, {}, resolved, error));
        EXPECT_FALSE(resolved.custom);
    }
}

TEST(GraphicsPresets, MalformedUnreadableAndFutureSettingsRemainUntouchedAndBlockSave) {
    const ScopedTempFile file("graphics_", ".json");
    const std::string documents[]{
        "{broken",
        "[]",
        R"({"user":null})",
        R"({"settings_schema_version":2,"user":{}})",
        R"({"settings_schema_version":18446744073709551615,"user":{}})",
        R"({"settings_schema_version":1.0,"user":{}})",
        R"({"user":{"video":{"render_scale":1e100}}})",
        R"({"user":{"video":{"render_scale":"bad"}}})",
    };
    for (const auto& document : documents) {
        SCOPED_TRACE(document);
        WriteText(file.Path(), document);
        const auto config = SystemConfig::LoadLayered("", file.Path().string());
        EXPECT_EQ(config.settings_load_report().state, SettingsLoadState::Recovery);
        EXPECT_FALSE(config.settings_load_report().needs_first_launch_detection);
        EXPECT_FALSE(config.settings_load_report().recovery_reason.empty());
        EXPECT_FLOAT_EQ(config.user().render_scale, 1.0f);
        EXPECT_TRUE(config.user().graphics.overrides.empty());
        EXPECT_EQ(config.user().graphics.base_preset, GraphicsPreset::Ultra);
        EXPECT_FALSE(config.SaveUserOverlay(file.Path().string()));
        EXPECT_EQ(ReadText(file.Path()), document);
        EXPECT_FALSE(std::filesystem::exists(file.Path().string() + ".tmp"));
        ExpectNoOverlayStaging(file.Path());
    }
    // A directory at the overlay path is portably unreadable as a settings file,
    // even under elevated test accounts where permission-bit tests are unreliable.
    const auto blocked = file.Path().parent_path() / "blocked";
    ASSERT_TRUE(std::filesystem::create_directory(blocked));
    const auto unreadable = SystemConfig::LoadLayered("", blocked.string());
    EXPECT_EQ(unreadable.settings_load_report().state, SettingsLoadState::Recovery);
    EXPECT_FALSE(unreadable.SaveUserOverlay(blocked.string()));
    EXPECT_TRUE(std::filesystem::is_directory(blocked));
}

TEST(GraphicsPresets, SystemsOverridesPreserveRecoveryAndRefuseSavingOriginal) {
    const ScopedTempFile defaults_file("graphics_defaults_", ".json");
    const ScopedTempFile systems_file("graphics_systems_", ".json");
    WriteText(defaults_file.Path(), R"({
        "render":{"circadian":{"params":{"amplitude":0.5}}},
        "user":{"video":{"fov":70,"render_scale":0.8,"vsync":false,"frame_cap":144}}
    })");
    WriteText(systems_file.Path(), R"({
        "sim":{"water_high_res":{"enabled":false}},
        "render":{"moonlight":{"enabled":false,"params":{"strength":0.25,"color":[0.1,0.2,0.3]}}},
        "user":{"video":{"fov":100,"render_scale":0.5,"vsync":true,"frame_cap":60}}
    })");
    const std::string documents[]{
        "{broken",
        R"({"settings_schema_version":2,"user":{}})",
        R"({"settings_schema_version":1,"user":{"graphics":{"base_preset":"ultra","preset_revision":2}}})",
    };
    for (const bool reload_existing : {false, true}) {
        SCOPED_TRACE(reload_existing);
        for (const auto& document : documents) {
            SCOPED_TRACE(document);
            const ScopedTempFile player_file("graphics_", ".json");
            WriteText(player_file.Path(), document);
            auto systems = SystemConfig::Defaults();
            std::string error;
            if (!reload_existing) {
                // The client validates --systems-config before loading the player overlay.
                ASSERT_TRUE(
                    SystemConfig::TryLoadFromFile(systems_file.Path().string(), systems, error))
                    << error;
            }
            auto config = SystemConfig::LoadLayered(defaults_file.Path().string(),
                                                    player_file.Path().string());
            ASSERT_EQ(config.settings_load_report().state, SettingsLoadState::Recovery);
            const auto report = config.settings_load_report();
            ASSERT_FALSE(report.recovery_reason.empty());
            if (reload_existing) {
                // Reload into the loaded config through the public file API.
                ASSERT_TRUE(
                    SystemConfig::TryLoadFromFile(systems_file.Path().string(), config, error))
                    << error;
            } else {
                // Apply the separate systems snapshot after LoadLayered, as the client does.
                config.ReplaceSystemsFrom(systems);
            }
            EXPECT_FALSE(config.enabled(SysKey::SimWaterHighRes));
            EXPECT_FALSE(config.enabled(SysKey::RenderMoonlight));
            EXPECT_FLOAT_EQ(config.param(SysParam::MoonlightStrength, -1.0f), 0.25f);
            EXPECT_EQ(config.param3(SysParam::MoonlightColor, glm::vec3(-1.0f)),
                      glm::vec3(0.1f, 0.2f, 0.3f));
            EXPECT_FLOAT_EQ(config.param(SysParam::CircadianAmplitude, 1.0f), 1.0f);
            EXPECT_EQ(config.settings_load_report().state, report.state);
            EXPECT_EQ(config.settings_load_report().needs_first_launch_detection,
                      report.needs_first_launch_detection);
            EXPECT_EQ(config.settings_load_report().recovery_reason, report.recovery_reason);
            EXPECT_FLOAT_EQ(config.user().fov, 70.0f);
            EXPECT_FLOAT_EQ(config.user().render_scale, 0.8f);
            EXPECT_FALSE(config.user().vsync);
            EXPECT_EQ(config.user().frame_cap, 144);
            EXPECT_DOUBLE_EQ(std::get<double>(config.user().graphics.overrides.at("render_scale")),
                             0.8);
            config.user().fov = 75.0f; // A later Settings edit still must not overwrite the file.
            EXPECT_FALSE(config.SaveUserOverlay(player_file.Path().string()));
            EXPECT_EQ(ReadText(player_file.Path()), document);
            EXPECT_FALSE(std::filesystem::exists(player_file.Path().string() + ".tmp"));
            ExpectNoOverlayStaging(player_file.Path());
        }
    }
}

TEST(GraphicsPresets, SystemsOverridesRetainAllPlayerSettingsAndNonRecoveryLoadStates) {
    const ScopedTempFile systems_file("graphics_systems_", ".json");
    WriteText(systems_file.Path(), R"({"sim":{"water_high_res":{"enabled":false}},
        "user":{"video":{"fov":100,"vsync":true,"frame_cap":60}}})");
    for (const auto state : {SettingsLoadState::FirstLaunch,
                             SettingsLoadState::MigratedLegacy,
                             SettingsLoadState::Loaded}) {
        SCOPED_TRACE(static_cast<int>(state));
        for (const bool reload_existing : {false, true}) {
            SCOPED_TRACE(reload_existing);
            const ScopedTempFile player_file("graphics_", ".json");
            const ScopedTempFile before_file("graphics_before_", ".json");
            const ScopedTempFile after_file("graphics_after_", ".json");
            if (state == SettingsLoadState::MigratedLegacy)
                WriteText(player_file.Path(),
                          R"({"user":{"video":{"fov":70,"render_scale":0.8}}})");
            else if (state == SettingsLoadState::Loaded)
                WriteText(player_file.Path(), CurrentDocument().dump());
            auto config = SystemConfig::LoadLayered("", player_file.Path().string());
            ASSERT_EQ(config.settings_load_report().state, state);
            const auto report = config.settings_load_report();
            config.user().vsync = false;
            config.user().frame_cap = 144;
            config.user().keybinds["Jump"] = 32;
            config.user().graphics.detection = {.observed_hardware = "Example GPU",
                                                .rules_revision = 1,
                                                .recommended_preset = GraphicsPreset::High};
            ASSERT_TRUE(config.SaveUserOverlay(before_file.Path().string()));
            std::string error;
            if (reload_existing) {
                ASSERT_TRUE(
                    SystemConfig::TryLoadFromFile(systems_file.Path().string(), config, error))
                    << error;
            } else {
                auto systems = SystemConfig::Defaults();
                ASSERT_TRUE(
                    SystemConfig::TryLoadFromFile(systems_file.Path().string(), systems, error))
                    << error;
                config.ReplaceSystemsFrom(systems);
            }
            EXPECT_FALSE(config.enabled(SysKey::SimWaterHighRes));
            EXPECT_EQ(config.settings_load_report().state, report.state);
            EXPECT_EQ(config.settings_load_report().needs_first_launch_detection,
                      report.needs_first_launch_detection);
            EXPECT_EQ(config.settings_load_report().recovery_reason, report.recovery_reason);
            ASSERT_TRUE(config.SaveUserOverlay(after_file.Path().string()));
            EXPECT_EQ(ReadText(after_file.Path()), ReadText(before_file.Path()));
        }
    }
}

TEST(GraphicsPresets, DeveloperLayoutRecoveryPreservesVersionedGraphicsAcrossLoadPaths) {
    const ScopedTempFile file("graphics_layout_", ".json");
    auto document = CurrentDocument();
    document["user"]["graphics"]["base_preset"] = "medium";
    document["user"]["graphics"]["overrides"]["render_scale"] = 0.75;
    document["user"]["video"] = {{"vsync", false}, {"frame_cap", 165}};
    document["user"]["developer"] = {
        {"widgets", {{"perf.hud", {{"visible", false}}}}},
        {"bindings", {{"console.toggle", {{"key", 301}, {"mods", 2}}}}},
        {"layouts",
         {{"render.shaders",
           {{"x", 130}, {"y", 90}, {"width", 600}, {"height", 400}, {"open", true}}},
          {"perf.gpu", {{"x", "overflow"}}}}}};
    auto text = document.dump();
    const auto offset = text.find("\"overflow\"");
    ASSERT_NE(offset, std::string::npos);
    text.replace(offset, std::string("\"overflow\"").size(), "1e400");
    WriteText(file.Path(), text);
    for (const int path : {0, 1, 2}) {
        SCOPED_TRACE(path);
        auto config = SystemConfig::Defaults();
        if (path == 0)
            config.OverlayUserFromJsonString(text);
        else if (path == 1)
            config = SystemConfig::FromJsonString(text);
        else
            config = SystemConfig::LoadLayered("", file.Path().string());
        EXPECT_EQ(config.settings_load_report().state, SettingsLoadState::Loaded);
        EXPECT_EQ(config.user().graphics.base_preset, GraphicsPreset::Medium);
        EXPECT_DOUBLE_EQ(std::get<double>(config.user().graphics.overrides.at("render_scale")),
                         0.75);
        EXPECT_FLOAT_EQ(config.user().render_scale, 0.75f);
        EXPECT_FALSE(config.user().vsync);
        EXPECT_EQ(config.user().frame_cap, 165);
        EXPECT_FALSE(config.user().developer.PerfHudVisible());
        ASSERT_TRUE(config.user().developer.bindings.contains("console.toggle"));
        EXPECT_EQ(config.user().developer.bindings.at("console.toggle").key, 301);
        ASSERT_EQ(config.user().developer.layouts.size(), 1u);
        EXPECT_FLOAT_EQ(config.user().developer.layouts.at("render.shaders").x, 130);
        ASSERT_TRUE(config.SaveUserOverlay(file.Path().string()));
        const auto saved = SystemConfig::LoadLayered("", file.Path().string());
        EXPECT_EQ(saved.user().graphics.overrides, config.user().graphics.overrides);
        EXPECT_EQ(saved.user().developer.layouts, config.user().developer.layouts);
        ASSERT_EQ(saved.user().developer.bindings.size(), config.user().developer.bindings.size());
        for (const auto& [id, binding] : config.user().developer.bindings) {
            ASSERT_TRUE(saved.user().developer.bindings.contains(id));
            EXPECT_EQ(saved.user().developer.bindings.at(id).key, binding.key);
            EXPECT_EQ(saved.user().developer.bindings.at(id).mods, binding.mods);
        }
        EXPECT_EQ(saved.user().developer.widgets, config.user().developer.widgets);
        EXPECT_EQ(saved.user().frame_cap, 165);
        EXPECT_FALSE(saved.user().vsync);
        WriteText(file.Path(), text);
    }
}

TEST(GraphicsPresets, InvalidGraphicsRollBackDeveloperPreferencesAndBlockSave) {
    const ScopedTempFile file("graphics_developer_", ".json");
    auto config = SystemConfig::Defaults();
    config.user().developer.widgets["perf.hud"] = true;
    config.user().developer.bindings["console.toggle"] = {290, 0};
    const auto widgets = config.user().developer.widgets;
    const auto bindings = config.user().developer.bindings;
    auto document = CurrentDocument();
    document["user"]["developer"] = {
        {"widgets", {{"perf.hud", {{"visible", false}}}}},
        {"bindings", {{"console.toggle", {{"key", 301}, {"mods", 2}}}}}};
    document["user"]["graphics"]["base_preset"] = "invalid";
    const auto text = document.dump();
    WriteText(file.Path(), text);
    config.OverlayUserFromJsonString(text);
    EXPECT_EQ(config.settings_load_report().state, SettingsLoadState::Recovery);
    EXPECT_EQ(config.user().developer.widgets, widgets);
    ASSERT_EQ(config.user().developer.bindings.size(), bindings.size());
    for (const auto& [id, binding] : bindings) {
        ASSERT_TRUE(config.user().developer.bindings.contains(id));
        EXPECT_EQ(config.user().developer.bindings.at(id).key, binding.key);
        EXPECT_EQ(config.user().developer.bindings.at(id).mods, binding.mods);
    }
    EXPECT_FALSE(config.SaveUserOverlay(file.Path().string()));
    EXPECT_EQ(ReadText(file.Path()), text);
}

TEST(GraphicsPresets, InvalidVersionedGraphicsPreserveWholeOverlayTransaction) {
    const auto document = CurrentDocument();
    auto assert_recovery = [](const nlohmann::json& invalid) {
        auto config = SystemConfig::Defaults();
        config.user().fov = 70.0f;
        config.OverlayUserFromJsonString(invalid.dump());
        EXPECT_EQ(config.settings_load_report().state, SettingsLoadState::Recovery);
        EXPECT_FLOAT_EQ(config.user().fov, 70.0f);
        EXPECT_TRUE(config.user().graphics.overrides.empty());
    };
    for (const auto& value : nlohmann::json::parse(R"([null,true,0,"custom","ULTRA",{}])")) {
        auto invalid = document;
        invalid["user"]["video"]["fov"] = 80;
        invalid["user"]["graphics"]["base_preset"] = value;
        assert_recovery(invalid);
    }
    for (const auto& value : nlohmann::json::parse(R"([null,0,2,1.0,4294967297,-1])")) {
        auto invalid = document;
        invalid["user"]["graphics"]["preset_revision"] = value;
        assert_recovery(invalid);
    }
    for (const auto& value : nlohmann::json::parse(R"([null,true,"0.8",0.49,1.01,1e100,{}])")) {
        auto invalid = document;
        invalid["user"]["graphics"]["overrides"]["render_scale"] = value;
        assert_recovery(invalid);
    }
    auto invalid = document;
    invalid["user"]["graphics"]["overrides"]["sim.water_high_res"] = false;
    assert_recovery(invalid);
    for (const auto& value : nlohmann::json::parse(R"([-1,4294967296,1.0,"1"])")) {
        invalid = document;
        invalid["user"]["graphics"]["detection"]["rules_revision"] = value;
        assert_recovery(invalid);
    }
    invalid = document;
    invalid["user"]["graphics"]["detection"]["recommended_preset"] = "custom";
    assert_recovery(invalid);
}

TEST(GraphicsPresets, SettingsAndMetadataBoundsRecoverWithoutReplacingOriginal) {
    const ScopedTempFile file("graphics_", ".json");
    const std::string oversized(1'048'577, ' ');
    WriteText(file.Path(), oversized);
    auto config = SystemConfig::LoadLayered("", file.Path().string());
    EXPECT_EQ(config.settings_load_report().state, SettingsLoadState::Recovery);
    EXPECT_FALSE(config.SaveUserOverlay(file.Path().string()));
    EXPECT_EQ(ReadText(file.Path()), oversized);
    config = SystemConfig::Defaults();
    config.OverlayUserFromJsonString(oversized);
    EXPECT_EQ(config.settings_load_report().state, SettingsLoadState::Recovery);

    auto document = CurrentDocument();
    document["user"]["graphics"]["detection"]["observed_hardware"] = std::string(4097, 'x');
    WriteText(file.Path(), document.dump());
    const auto invalid_metadata = SystemConfig::LoadLayered("", file.Path().string());
    EXPECT_EQ(invalid_metadata.settings_load_report().state, SettingsLoadState::Recovery);
    EXPECT_FALSE(invalid_metadata.SaveUserOverlay(file.Path().string()));
    EXPECT_EQ(ReadText(file.Path()), document.dump());

    config = SystemConfig::Defaults();
    config.user().graphics.detection.observed_hardware = std::string(4096, 'x');
    ASSERT_TRUE(config.SaveUserOverlay(file.Path().string()));
    const auto reloaded = SystemConfig::LoadLayered("", file.Path().string());
    EXPECT_EQ(reloaded.user().graphics.detection.observed_hardware.size(), 4096u);
    const auto valid = ReadText(file.Path());
    config.user().graphics.detection.observed_hardware.push_back('x');
    EXPECT_FALSE(config.SaveUserOverlay(file.Path().string()));
    EXPECT_EQ(ReadText(file.Path()), valid);
    config.user().graphics.detection.observed_hardware = std::string(1, static_cast<char>(0xff));
    EXPECT_FALSE(config.SaveUserOverlay(file.Path().string()));
    EXPECT_EQ(ReadText(file.Path()), valid);
}

TEST(GraphicsPresets, RoundTripAllOverrideTypesAndDetectionWithoutSessionValues) {
    const ScopedTempFile file("graphics_", ".json");
    auto config = SystemConfig::Defaults();
    auto& settings = config.user().graphics;
    ASSERT_TRUE(SelectGraphicsPreset(settings, GraphicsPreset::Medium));
    settings.overrides = {{"render_scale", 0.75},
                          {"grass_enabled", false},
                          {"grass_density", 0.6},
                          {"grass_fade_start_m", 30.0},
                          {"grass_fade_end_m", 60.0},
                          {"far_terrain_radius_m", 2000.0},
                          {"shadow_resolution", 1024.0},
                          {"volumetric_quality", 1.0},
                          {"ui_blur", std::string("full")}};
    settings.detection = {.observed_hardware = "Example GPU; 8 threads; 1920x1080",
                          .rules_revision = 7,
                          .recommended_preset = GraphicsPreset::High};
    ResolvedGraphics resolved;
    std::string error;
    ASSERT_TRUE(ResolveGraphics(settings, {{"render_scale", 0.5}}, resolved, error));
    ASSERT_TRUE(config.SaveUserOverlay(file.Path().string()));
    const auto first_save = ReadText(file.Path());
    const auto reloaded = SystemConfig::LoadLayered("", file.Path().string());
    EXPECT_EQ(reloaded.user().graphics.base_preset, settings.base_preset);
    EXPECT_EQ(reloaded.user().graphics.preset_revision, settings.preset_revision);
    EXPECT_EQ(reloaded.user().graphics.overrides, settings.overrides);
    EXPECT_EQ(reloaded.user().graphics.detection, settings.detection);
    ASSERT_TRUE(ResolveGraphics(reloaded.user().graphics, {}, resolved, error));
    EXPECT_TRUE(resolved.custom);
    EXPECT_DOUBLE_EQ(resolved.values.render_scale, 0.75);
    EXPECT_DOUBLE_EQ(resolved.values.far_outer_clip_m, 2050.0);
    EXPECT_TRUE(resolved.session_overrides.empty());
    ASSERT_TRUE(reloaded.SaveUserOverlay(file.Path().string()));
    EXPECT_EQ(ReadText(file.Path()), first_save);
}

TEST(GraphicsPresets, SaveFailuresPreserveExistingFileAndCleanTemporaryOutput) {
    const ScopedTempFile file("graphics_", ".json");
    const OverlayWriteReset reset;
    auto config = SystemConfig::Defaults();
    ASSERT_TRUE(config.SaveUserOverlay(file.Path().string()));
    const auto original = ReadText(file.Path());
    // This unowned legacy name must neither block unique staging nor be removed.
    const auto temporary = std::filesystem::path(file.Path().string() + ".tmp");
    ASSERT_TRUE(std::filesystem::create_directory(temporary));
    ASSERT_TRUE(config.SaveUserOverlay(file.Path()));
    EXPECT_EQ(ReadText(file.Path()), original);
    EXPECT_TRUE(std::filesystem::is_directory(temporary));
    ASSERT_TRUE(SelectGraphicsPreset(config.user().graphics, GraphicsPreset::Low));
    // Interrupt the actual shared writer after its owned staging file is flushed.
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_FALSE(config.SaveUserOverlay(file.Path().string()));
    WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_EQ(ReadText(file.Path()), original);
    EXPECT_TRUE(std::filesystem::is_directory(temporary));
    ASSERT_TRUE(std::filesystem::remove(temporary));
    ExpectNoOverlayStaging(file.Path());
    config.user().graphics.overrides["render_scale"] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(config.SaveUserOverlay(file.Path().string()));
    EXPECT_EQ(ReadText(file.Path()), original);
    ExpectNoOverlayStaging(file.Path());
    ResetGraphicsOverrides(config.user().graphics);
    // Force rename failure, and check cleanup with all stream handles closed.
    const auto directory = file.Path().parent_path() / "directory";
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    EXPECT_FALSE(config.SaveUserOverlay(directory.string()));
    EXPECT_FALSE(std::filesystem::exists(directory.string() + ".tmp"));
    ExpectNoOverlayStaging(directory);
    EXPECT_TRUE(std::filesystem::is_directory(directory));
    // A regular file cannot serve as the settings parent directory.
    EXPECT_FALSE(config.SaveUserOverlay((file.Path() / "settings.json").string()));
    EXPECT_EQ(ReadText(file.Path()), original);
    ASSERT_TRUE(config.SaveUserOverlay(file.Path()));
    const auto reloaded = SystemConfig::LoadLayered({}, file.Path());
    EXPECT_EQ(reloaded.user().graphics.base_preset, GraphicsPreset::Low);
    EXPECT_NE(ReadText(file.Path()), original);
    ExpectNoOverlayStaging(file.Path());
}

TEST(GraphicsPresets, EveryPresetLeavesSimulationIdentityAndOtherOwnersUnchanged) {
    const auto packaged_path =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "data/common/systems.json";
    auto config = SystemConfig::LoadFromFile(packaged_path.string());
    const auto hash = config.ComputeConfigSubHash();
    const auto gates = config.SimulationGates();
    Luminumbra::world::GameSession session;
    session.ApplySystemConfig(config);
    const auto session_identity = session.SystemConfigIdentity();
    config.user().vsync = false;
    config.user().frame_cap = 144;
    config.user().fov = 70.0f;
    config.user().ui_scale = 1.5f;
    config.user().window_mode = "windowed";
    config.user().resolution = "1920x1080";
    const ScopedTempFile file("graphics_", ".json");
    for (const auto preset : {GraphicsPreset::Low,
                              GraphicsPreset::Medium,
                              GraphicsPreset::High,
                              GraphicsPreset::Ultra}) {
        ASSERT_TRUE(SelectGraphicsPreset(config.user().graphics, preset));
        ResolvedGraphics resolved;
        std::string error;
        ASSERT_TRUE(ResolveGraphics(config.user().graphics, {}, resolved, error));
        EXPECT_EQ(config.ComputeConfigSubHash(), hash);
        EXPECT_EQ(config.SimulationGates(), gates);
        session.ApplySystemConfig(config);
        EXPECT_EQ(session.SystemConfigIdentity(), session_identity);
        EXPECT_TRUE(config.enabled(SysKey::SimWaterHighRes));
        EXPECT_TRUE(config.enabled(SysKey::SimActiveRegions));
        EXPECT_FLOAT_EQ(config.param(SysParam::CircadianAmplitude, 0.0f), 1.0f);
        EXPECT_FALSE(config.user().vsync);
        EXPECT_EQ(config.user().frame_cap, 144);
        EXPECT_FLOAT_EQ(config.user().fov, 70.0f);
        EXPECT_FLOAT_EQ(config.user().ui_scale, 1.5f);
        EXPECT_EQ(config.user().window_mode, "windowed");
        EXPECT_EQ(config.user().resolution, "1920x1080");
        ASSERT_TRUE(config.SaveUserOverlay(file.Path().string()));
        const auto document = nlohmann::json::parse(ReadText(file.Path()));
        EXPECT_FALSE(document.contains("sim"));
        EXPECT_FALSE(document.contains("render"));
        auto overlay = document;
        overlay["sim"]["water_high_res"]["enabled"] = false;
        overlay["sim"]["active_regions"]["enabled"] = false;
        overlay["render"]["circadian"]["params"]["amplitude"] = 0.0;
        config.OverlayUserFromJsonString(overlay.dump());
        EXPECT_EQ(config.ComputeConfigSubHash(), hash);
        EXPECT_EQ(config.SimulationGates(), gates);
        EXPECT_TRUE(config.enabled(SysKey::SimWaterHighRes));
        EXPECT_TRUE(config.enabled(SysKey::SimActiveRegions));
        EXPECT_FLOAT_EQ(config.param(SysParam::CircadianAmplitude, 0.0f), 1.0f);
    }
}

} // namespace
} // namespace luminumbra::core
