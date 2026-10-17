//  SystemConfig substrate — contract tests derived from spec
// Acceptance Criteria ..005.
// These reference luminumbra::core::SystemConfig, which does not exist yet:
// the test keeps the generated schema and runtime registry aligned.
#include "../support/SystemConfigVariants.h"
#include "../support/UniqueTempPath.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../support/UniqueTempPath.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <iomanip>

#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_common/core/FramePacing.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h" // StableChecksum

using luminumbra::core::SysKey;
using luminumbra::core::SysParam;
using luminumbra::core::SystemConfig;

namespace {

constexpr const char* kSimulationGateNames[]{
#define SIM_GATE_Sim(NAME) NAME,
#define SIM_GATE_Render(NAME)
#define SIM_GATE_NAME(ENUM, SECTION, JSON_SECTION, NAME, RESIDENCY) SIM_GATE_##SECTION(NAME)
    LUMIN_CONFIG_KEY_TABLE(SIM_GATE_NAME)
#undef SIM_GATE_NAME
#undef SIM_GATE_Render
#undef SIM_GATE_Sim
};

TEST(SystemConfig, SimulationArtifactReportsEveryRegistryGate) {
    const auto gates = SystemConfig::Defaults().SimulationGates();
    std::size_t count = 0;
    for (const char* name : kSimulationGateNames) {
        SCOPED_TRACE(name);
        ++count;
        ASSERT_TRUE(gates.contains(name));
        EXPECT_TRUE(gates.at(name));
    }
    EXPECT_EQ(gates.size(), count);
}

TEST(SystemConfig, SimulationArtifactReportsEveryExplicitOptOut) {
    const auto gates = SystemConfig::Defaults().SimulationGates();
    for (const char* name : kSimulationGateNames) {
        SCOPED_TRACE(name);
        nlohmann::json document;
        document["sim"][name]["enabled"] = false;
        Luminumbra::world::GameSession session;
        session.ApplySystemConfig(SystemConfig::FromJsonString(document.dump()));
        const auto effective = session.EffectiveSimulationGates();
        EXPECT_EQ(effective.size(), gates.size());
        EXPECT_FALSE(effective.at(name));
    }
}

std::string ConfigIdentity(const SystemConfig& config) {
    Luminumbra::world::GameSession session;
    session.ApplySystemConfig(config);
    return session.SystemConfigIdentity();
}

TEST(SystemConfig, SimulationIdentityPinsShippingDefaults) {
    // Exact little-endian scalar serialization, independently computed with stdlib Python.
    // See docs/development/system-config-identity.md for the complete computation.
    EXPECT_EQ(ConfigIdentity(SystemConfig::Defaults()), "60ab3c242d54ce89");
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                      "data/common/systems.json";
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(ConfigIdentity(SystemConfig::LoadFromFile(path.string())), "60ab3c242d54ce89");
    EXPECT_EQ(luminumbra::core::SimulationIdentityValues{}.ComputeIdentity(),
              ConfigIdentity(SystemConfig::Defaults()));
}

TEST(SystemConfig, SimulationIdentityCoversEveryAppliedInput) {
    const auto baseline = ConfigIdentity(SystemConfig::Defaults());
    for (const auto& [name, identity] : Luminumbra::TestSupport::EffectiveIdentityMismatches()) {
        SCOPED_TRACE(name);
        EXPECT_NE(identity, baseline);
    }
}

TEST(SystemConfig, SimulationIdentityUsesResolvedValuesAndIgnoresPresentation) {
    const auto baseline = ConfigIdentity(SystemConfig::Defaults());
    const auto explicit_defaults = SystemConfig::FromJsonString(R"({
        "user":{"video":{"fov":90}},
        "render":{"moonlight":{"enabled":false,"params":{"strength":0.75}}},
        "sim":{"ecology":{"params":{"herd_weight":0.8,"energy_drain_per_second":0.006}},
               "plant_growth":{"params":{"mutation_rate":0.05}}}})");
    EXPECT_EQ(ConfigIdentity(explicit_defaults), baseline);
    const auto first = SystemConfig::FromJsonString(R"({"sim":{
        "ecology":{"params":{"herd_weight":0.9,"catch_radius":2.5}},
        "plant_growth":{"params":{"mutation_rate":0.2}}}})");
    const auto reordered = SystemConfig::FromJsonString(R"({"sim":{
        "plant_growth":{"params":{"mutation_rate":0.2}},
        "ecology":{"params":{"catch_radius":2.5,"herd_weight":0.9}}}})");
    EXPECT_EQ(ConfigIdentity(first), ConfigIdentity(reordered));
    const auto disabled = SystemConfig::FromJsonString(R"({"sim":{"ecology":{"enabled":false}}})");
    const auto ignored = SystemConfig::FromJsonString(
        R"({"sim":{"ecology":{"enabled":false,"params":{"herd_weight":0.9}}}})");
    EXPECT_EQ(ConfigIdentity(disabled), ConfigIdentity(ignored));
    const auto positive_zero = SystemConfig::FromJsonString(
        R"({"sim":{"plant_growth":{"params":{"mutation_rate":0.0}}}})");
    const auto negative_zero = SystemConfig::FromJsonString(
        R"({"sim":{"plant_growth":{"params":{"mutation_rate":-0.0}}}})");
    EXPECT_NE(ConfigIdentity(positive_zero), ConfigIdentity(negative_zero));
}

// missing/empty config yields all-defaults, no crash.
TEST(SystemConfig, ShippingDefaultsAllOn) {
    const SystemConfig cfg = SystemConfig::Defaults();
    for (unsigned key = 0; key < static_cast<unsigned>(SysKey::Count); ++key) {
        const auto id = static_cast<SysKey>(key);
        EXPECT_TRUE(cfg.enabled(id)) << key;
    }
    EXPECT_TRUE(cfg.enabled(SysKey::SimPlantGrowth));
    EXPECT_TRUE(cfg.enabled(SysKey::SimErosion));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderMoonlight));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderTreeWind));
    EXPECT_FLOAT_EQ(cfg.param(SysParam::PlantMutationRate, 0.123f), 0.123f);
    EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, 0.5f), 0.5f);
}

TEST(SystemConfig, DefaultsRequestFarWorldLadder) {
    const SystemConfig cfg = SystemConfig::Defaults();
    EXPECT_TRUE(cfg.enabled(SysKey::SimPlantGrowth));
    EXPECT_TRUE(cfg.enabled(SysKey::SimErosion));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderMoonlight));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderTreeWind));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderFarWorldLadder));
    EXPECT_FLOAT_EQ(cfg.param(SysParam::PlantMutationRate, 0.123f), 0.123f);
    EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, 0.5f), 0.5f);
}

TEST(SystemConfig, MissingFileIsDefaults) {
    const SystemConfig cfg = SystemConfig::LoadFromFile("this/path/does/not/exist.json");
    for (unsigned key = 0; key < static_cast<unsigned>(SysKey::Count); ++key)
        EXPECT_TRUE(cfg.enabled(static_cast<SysKey>(key))) << key;
    EXPECT_TRUE(cfg.enabled(SysKey::RenderMoonlight));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderFarWorldLadder));
    EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, 0.7f), 0.7f);
}

TEST(SystemConfig, ExplicitOverrideLoadsHistoricalConfiguration) {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "fixtures/historical-defaults-off.json";
    SystemConfig config;
    std::string error = "previous error";
    ASSERT_TRUE(SystemConfig::TryLoadFromFile(path.string(), config, error)) << error;
    EXPECT_TRUE(error.empty());
#define EXPECT_HISTORICAL_KEY(ENUM, SECTION, JSON_SECTION, JSON_NAME, RESIDENCY)                   \
    if (std::string(JSON_SECTION) == "sim") {                                                      \
        EXPECT_FALSE(config.enabled(SysKey::ENUM)) << JSON_NAME;                                   \
    }
    LUMIN_CONFIG_KEY_TABLE(EXPECT_HISTORICAL_KEY)
#undef EXPECT_HISTORICAL_KEY
    EXPECT_TRUE(config.ComputeConfigSubHash().empty());
    EXPECT_TRUE(config.enabled(SysKey::RenderFarWorldLadder));
    EXPECT_FALSE(config.enabled(SysKey::RenderCircadian));
    EXPECT_NE(ConfigIdentity(config), ConfigIdentity(SystemConfig::Defaults()));
    EXPECT_EQ(ConfigIdentity(config), ConfigIdentity(SystemConfig::LoadFromFile(path.string())));
}

TEST(SystemConfig, ExplicitOverrideRefusesInvalidFilesWithoutChangingSnapshot) {
    const auto directory =
        Luminumbra::TestSupport::CreateUniqueTempDirectory("lumin-systemconfig-override-");
    const auto path = directory / "override.json";
    std::error_code remove_error;
    SystemConfig config =
        SystemConfig::FromJsonString(R"({"sim":{"water_high_res":{"enabled":false}}})");
    const auto identity = ConfigIdentity(config);
    std::string error;
    EXPECT_FALSE(SystemConfig::TryLoadFromFile(path.string(), config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(ConfigIdentity(config), identity);
    for (const char* text : {"",
                             "{",
                             "[]",
                             "null",
                             "{} trailing",
                             R"({"render":{"moonlight":{"params":{"color":[1,"bad",3]}}}})"}) {
        SCOPED_TRACE(text);
        {
            std::ofstream file(path);
            ASSERT_TRUE(file.good());
            file << text;
        }
        EXPECT_FALSE(SystemConfig::TryLoadFromFile(path.string(), config, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(ConfigIdentity(config), identity);
    }
    std::filesystem::remove_all(directory, remove_error);
}

TEST(SystemConfig, EmptyAndBlankJsonAreDefaults) {
    for (const char* text : {"",
                             "{}",
                             "   ",
                             "not json at all",
                             R"({"render":{"far_world_ladder":{}}})",
                             R"({"render":{"far_world_ladder":{"enabled":null}}})",
                             R"({"render":{"far_world_ladder":{"enabled":"false"}}})"}) {
        const SystemConfig cfg = SystemConfig::FromJsonString(text);
        for (unsigned key = 0; key < static_cast<unsigned>(SysKey::Count); ++key)
            EXPECT_TRUE(cfg.enabled(static_cast<SysKey>(key)))
                << "input: " << text << " key: " << key;
        EXPECT_TRUE(cfg.enabled(SysKey::SimPlantGrowth)) << "input: " << text;
        EXPECT_TRUE(cfg.enabled(SysKey::RenderFarWorldLadder)) << "input: " << text;
    }
}

TEST(SystemConfig, RawNulRefusesWholeConfiguration) {
    const std::string bytes =
        R"({"sim":{"plant_growth":{"enabled":true,"params":{"mutation_rate":0.08}}}})";
    const auto clean = SystemConfig::FromJsonString(bytes);
    ASSERT_TRUE(clean.enabled(SysKey::SimPlantGrowth));
    EXPECT_FLOAT_EQ(clean.param(SysParam::PlantMutationRate, -1.0f), 0.08f);
    const Luminumbra::TestSupport::ScopedTempFile file("config_bytes_", ".json");
    const auto write = [&](const std::string& content) {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << content;
        output.close();
        ASSERT_TRUE(output.good());
    };
    for (const auto& invalid : {bytes + '\0',
                                bytes + '\0' + "garbage",
                                bytes + '\x1a' + '\0' + "garbage",
                                '\0' + bytes}) {
        write(invalid);
        const auto refused_file = SystemConfig::LoadFromFile(file.Path().string());
        EXPECT_FALSE(refused_file.enabled(SysKey::SimPlantGrowth));
        EXPECT_EQ(refused_file.ComputeConfigSubHash(),
                  SystemConfig::Defaults().ComputeConfigSubHash());
        EXPECT_FLOAT_EQ(refused_file.param(SysParam::PlantMutationRate, -1.0f), -1.0f);
        const auto refused = SystemConfig::FromJsonString(invalid);
        EXPECT_FALSE(refused.enabled(SysKey::SimPlantGrowth));
        EXPECT_EQ(refused.ComputeConfigSubHash(), SystemConfig::Defaults().ComputeConfigSubHash());
        EXPECT_FLOAT_EQ(refused.param(SysParam::PlantMutationRate, -1.0f), -1.0f);
    }
    write(bytes);
    EXPECT_EQ(SystemConfig::LoadFromFile(file.Path().string()).ComputeConfigSubHash(),
              clean.ComputeConfigSubHash());
    EXPECT_EQ(SystemConfig::FromJsonString(bytes).ComputeConfigSubHash(),
              clean.ComputeConfigSubHash());
}

// param parse + defaults round-trip; unknown keys ignored.
TEST(SystemConfig, RoundTripNamedValuesAndFallbacks) {
    const std::string json = R"({
      "render": {
        "moonlight": { "enabled": true, "params": { "strength": 0.42, "color": [0.6, 0.7, 1.0] } }
      },
      "sim": {
        "plant_growth": { "enabled": false, "params": { "mutation_rate": 0.08 } }
      }
    })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    EXPECT_TRUE(cfg.enabled(SysKey::RenderMoonlight));
    EXPECT_FALSE(cfg.enabled(SysKey::SimPlantGrowth));
    EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, -1.0f), 0.42f);
    EXPECT_FLOAT_EQ(cfg.param(SysParam::PlantMutationRate, -1.0f), 0.08f);
    const glm::vec3 color = cfg.param3(SysParam::MoonlightColor, glm::vec3(0.0f));
    EXPECT_FLOAT_EQ(color.r, 0.6f);
    EXPECT_FLOAT_EQ(color.g, 0.7f);
    EXPECT_FLOAT_EQ(color.b, 1.0f);
    // unnamed param falls back
    EXPECT_FLOAT_EQ(cfg.param(SysParam::PlantMutationRate, 9.0f), 0.08f);
}

TEST(SystemConfig, UnknownKeysIgnoredNoThrow) {
    const std::string json = R"({
      "render": { "moonlight": { "enabled": true }, "bogus_system": { "enabled": true } },
      "sim": { "not_a_real_key": { "params": { "x": 1.0 } } },
      "weird_top_level": 42
    })";
    SystemConfig cfg = SystemConfig::Defaults();
    ASSERT_NO_THROW({ cfg = SystemConfig::FromJsonString(json); });
    EXPECT_TRUE(cfg.enabled(SysKey::RenderMoonlight));
}

// Shipping simulation defaults contribute to the config sub-hash.
TEST(SystemConfig, ConfigSubHashPresentAtDefaults) {
    EXPECT_FALSE(SystemConfig::Defaults().ComputeConfigSubHash().empty());
}

// sim non-default flag/param moves the sub-hash; render flags never do.
TEST(SystemConfig, RenderFlagDoesNotMoveConfigSubHash) {
    nlohmann::json on, off;
    // Exercise every render gate with opposite values and identical sim settings.
    // Keep render parameters different too, to cover both exclusion contracts.
#define CHECK_RENDER_Sim(ENUM, NAME)
#define CHECK_RENDER_Render(ENUM, NAME)                                                            \
    on["render"][NAME]["enabled"] = true;                                                          \
    off["render"][NAME]["enabled"] = false;
#define CHECK_RENDER_KEY(ENUM, SECTION, JSON_SECTION, NAME, RESIDENCY)                             \
    CHECK_RENDER_##SECTION(ENUM, NAME)
    LUMIN_CONFIG_KEY_TABLE(CHECK_RENDER_KEY)
#undef CHECK_RENDER_KEY
#undef CHECK_RENDER_Render
#undef CHECK_RENDER_Sim
    on["render"]["moonlight"]["params"]["strength"] = 9.9;
    off["render"]["moonlight"]["params"]["strength"] = 0.1;
    const auto enabled = SystemConfig::FromJsonString(on.dump());
    const auto disabled = SystemConfig::FromJsonString(off.dump());
    EXPECT_TRUE(enabled.enabled(SysKey::RenderMoonlight));
    EXPECT_FALSE(disabled.enabled(SysKey::RenderMoonlight));
    EXPECT_TRUE(enabled.enabled(SysKey::RenderTreeWind));
    EXPECT_FALSE(disabled.enabled(SysKey::RenderTreeWind));
    EXPECT_FALSE(enabled.ComputeConfigSubHash().empty());
    EXPECT_EQ(enabled.ComputeConfigSubHash(), disabled.ComputeConfigSubHash());
}

TEST(SystemConfig, SimFlagMovesConfigSubHash) {
    const std::string json = R"({ "sim": { "plant_growth": { "enabled": false } } })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    EXPECT_NE(cfg.ComputeConfigSubHash(), SystemConfig::Defaults().ComputeConfigSubHash());
}

TEST(SystemConfig, SimParamMovesConfigSubHash) {
    const std::string enabled_only = R"({ "sim": { "plant_growth": { "enabled": true } } })";
    const std::string with_param =
        R"({ "sim": { "plant_growth": { "enabled": true, "params": { "mutation_rate": 0.2 } } } })";
    const auto h0 = SystemConfig::FromJsonString(enabled_only).ComputeConfigSubHash();
    const auto h1 = SystemConfig::FromJsonString(with_param).ComputeConfigSubHash();
    EXPECT_NE(h0, std::string{});
    EXPECT_NE(h1, std::string{});
    EXPECT_NE(h0, h1);
}

TEST(SystemConfig, ConfigSubHashIsOrderIndependentAndStable) {
    // Same non-default sim set, different JSON key order -> identical hash.
    const std::string a = R"({ "sim": {
        "plant_growth": { "enabled": true, "params": { "mutation_rate": 0.2 } },
        "erosion": { "enabled": true } } })";
    const std::string b = R"({ "sim": {
        "erosion": { "enabled": true },
        "plant_growth": { "params": { "mutation_rate": 0.2 }, "enabled": true } } })";
    const auto ha = SystemConfig::FromJsonString(a).ComputeConfigSubHash();
    const auto hb = SystemConfig::FromJsonString(b).ComputeConfigSubHash();
    EXPECT_FALSE(ha.empty());
    EXPECT_EQ(ha, hb);
}

//   /  — the SCHEMA-GENERATED kKeys/kParams registries must produce a
// config:v1: serialization that is BYTE-IDENTICAL to the hand-authored contract. We reconstruct
// the canonical serialization independently here (same prefix, same setprecision(17), same float
// literals as the schema defaults) and assert ComputeConfigSubHash ==
// StableChecksum(reconstructed). This pins BOTH the serialization format AND each generated default
// value, so a drift in either (e.g. a regenerated header with a changed default, or a serializer
// edit) fails this test rather than silently moving an enabled-system hash. Defaults are never
// exercised by `--smoke` (which enables no sim system → empty hash), so this is the real guard for
// the schema-codegen migration.
std::string ExpectedSubHash(const std::string& canonical_body) {
    std::ostringstream bytes;
    bytes << "config:v1:" << std::setprecision(17) << canonical_body;
    return Luminumbra::Persistence::StableChecksum(bytes.str());
}

// Pin the serialization contract independently of shipping activation defaults.
SystemConfig WithAllOtherSimFlagsOff(const std::string& text) {
    nlohmann::json json;
#define DISABLE_KEY(ENUM, SECTION, JSON_SECTION, JSON_NAME, RESIDENCY)                             \
    if (std::string(JSON_SECTION) == "sim")                                                        \
        json[JSON_SECTION][JSON_NAME]["enabled"] = false;
    LUMIN_CONFIG_KEY_TABLE(DISABLE_KEY)
#undef DISABLE_KEY
    json.merge_patch(nlohmann::json::parse(text));
    return SystemConfig::FromJsonString(json.dump());
}

TEST(SystemConfig, ConfigSubHashEmptyWithAllSimFlagsDisabled) {
    EXPECT_EQ(WithAllOtherSimFlagsOff("{}").ComputeConfigSubHash(), std::string{});
}

TEST(SystemConfig, ConfigSubHashByteIdenticalSnapshot) {
    // Single sim key with one scalar param at its compiled default.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "plant_growth:en=1;mutation_rate=" << 0.05f << ';';
        const auto cfg =
            WithAllOtherSimFlagsOff(R"({ "sim": { "plant_growth": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // Sim key with NO owned params still emits the enabled marker and nothing after it.
    {
        const auto cfg =
            WithAllOtherSimFlagsOff(R"({ "sim": { "erosion": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash("erosion:en=1;"));
    }
    // Sim key owning multiple scalar params: every owned default is emitted in kParams order.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "foraging:en=1;deposit=" << 1.0f
             << ";trail_weight=" << 8.0f << ";goal_weight=" << 1.0f << ';';
        const auto cfg =
            WithAllOtherSimFlagsOff(R"({ "sim": { "foraging": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // Multiple enabled sim keys serialize in kKeys (schema) order: plant_growth precedes erosion.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "plant_growth:en=1;mutation_rate=" << 0.05f
             << ";erosion:en=1;";
        const auto cfg = WithAllOtherSimFlagsOff(
            R"({ "sim": { "erosion": { "enabled": true },
                          "plant_growth": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // An explicitly-set non-default param overrides the generated default in the same format.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "plant_growth:en=1;mutation_rate=" << 0.2f << ';';
        const auto cfg = WithAllOtherSimFlagsOff(
            R"({ "sim": { "plant_growth": { "enabled": true, "params": { "mutation_rate": 0.2 } } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // Explicitly disabling all sim flags retains the empty compatibility baseline.
    EXPECT_EQ(WithAllOtherSimFlagsOff("{}").ComputeConfigSubHash(), std::string{});
}

// enabled is a cheap, deterministic hot-path query (structural O(1);
// functional smoke that it is side-effect-free and stable across many calls).
TEST(SystemConfig, EnabledHotPathStable) {
    const SystemConfig cfg =
        SystemConfig::FromJsonString(R"({ "sim": { "erosion": { "enabled": true } } })");
    bool acc = false;
    for (int i = 0; i < 1'000'000; ++i) {
        acc ^= cfg.enabled(SysKey::SimErosion);
    }
    // 1e6 XORs of `true` -> false; the point is no crash/alloc and a stable answer.
    EXPECT_FALSE(acc);
    EXPECT_TRUE(cfg.enabled(SysKey::SimErosion));
}

// ---- Addendum A: user.* settings extension (-101..105) ----

// -101 — user.video/audio round-trip; unnamed fields keep defaults.
TEST(SystemConfig, UserVideoAudioRoundTrip) {
    const std::string json = R"({
      "user": {
        "video": { "resolution": "3840x1600", "window_mode": "fullscreen", "vsync": false,
                   "fov": 70.0, "render_scale": 0.85, "mouse_sensitivity": 0.22 },
        "audio": { "master": 0.8, "music": 0.5 }
      }
    })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    const auto& u = cfg.user();
    EXPECT_EQ(u.resolution, "3840x1600");
    EXPECT_EQ(u.window_mode, "fullscreen");
    EXPECT_FALSE(u.vsync);
    EXPECT_FLOAT_EQ(u.fov, 70.0f);
    EXPECT_FLOAT_EQ(u.render_scale, 0.85f);
    EXPECT_FLOAT_EQ(u.mouse_sensitivity, 0.22f);
    EXPECT_FLOAT_EQ(u.audio_master, 0.8f);
    EXPECT_FLOAT_EQ(u.audio_music, 0.5f);
    EXPECT_FLOAT_EQ(u.audio_sfx, 1.0f); // unnamed -> default
}

// Default look sensitivity is 25% of the prior 0.1 (2026-06-18).
// Shipping VSync defaults ON; saved user settings remain authoritative.
TEST(SystemConfig, UserVideoDefaults) {
    const auto config = SystemConfig::Defaults();
    const auto& u = config.user();
    EXPECT_FLOAT_EQ(u.mouse_sensitivity, 0.025f);
    EXPECT_TRUE(u.vsync);
    EXPECT_EQ(u.frame_cap, 0);
}

TEST(SystemConfig, FramePacingPersistsEverySupportedChoice) {
    for (const int cap : {0, 30, 60, 120, 144, 165, 240}) {
        for (const bool vsync : {false, true}) {
            SCOPED_TRACE(cap);
            SCOPED_TRACE(vsync);
            const Luminumbra::TestSupport::ScopedTempFile file("frame_pacing_", ".json");
            const nlohmann::json document = {
                {"user", {{"video", {{"vsync", vsync}, {"frame_cap", cap}}}}}};
            const auto config = SystemConfig::FromJsonString(document.dump());
            EXPECT_EQ(config.user().vsync, vsync);
            EXPECT_EQ(config.user().frame_cap, cap);
            ASSERT_TRUE(config.SaveUserOverlay(file.Path().string()));
            const auto reloaded = SystemConfig::LoadLayered("", file.Path().string());
            EXPECT_EQ(reloaded.user().vsync, vsync);
            EXPECT_EQ(reloaded.user().frame_cap, cap);
            EXPECT_EQ(reloaded.ComputeConfigSubHash(),
                      SystemConfig::Defaults().ComputeConfigSubHash());
        }
    }
}

TEST(SystemConfig, InvalidFramePacingValuesKeepDefaultsOrPreviousOverlay) {
    const auto invalid_caps = nlohmann::json::parse(
        R"([null,true,false,"30","60","165","unlimited",30.0,60.0,60.5,165.0,
            -1,1,29,31,61,164,166,300,[],{},4294967326,4294967356,4294967461,
            18446744073709551615,-9223372036854775808])");
    for (const auto& cap : invalid_caps) {
        SCOPED_TRACE(cap.dump());
        const nlohmann::json document = {{"user", {{"video", {{"frame_cap", cap}}}}}};
        EXPECT_EQ(SystemConfig::FromJsonString(document.dump()).user().frame_cap, 0);
        auto config = SystemConfig::Defaults();
        config.OverlayUserFromJsonString(R"({"user":{"video":{"frame_cap":144}}})");
        config.OverlayUserFromJsonString(document.dump());
        EXPECT_EQ(config.user().frame_cap, 144);
    }
    const auto invalid_vsync = nlohmann::json::parse(R"([null,0,1,"on","off",[],{}])");
    for (const auto& vsync : invalid_vsync) {
        SCOPED_TRACE(vsync.dump());
        const nlohmann::json document = {{"user", {{"video", {{"vsync", vsync}}}}}};
        EXPECT_TRUE(SystemConfig::FromJsonString(document.dump()).user().vsync);
        auto config = SystemConfig::Defaults();
        config.OverlayUserFromJsonString(R"({"user":{"video":{"vsync":false}}})");
        config.OverlayUserFromJsonString(document.dump());
        EXPECT_FALSE(config.user().vsync);
    }
    auto config = SystemConfig::Defaults();
    config.OverlayUserFromJsonString(R"({"user":{"video":{"vsync":false,"frame_cap":240}}})");
    config.OverlayUserFromJsonString(R"({"user":{"video":{"unknown_setting":120}}})");
    config.OverlayUserFromJsonString(R"({"user":{"video":{}}})");
    EXPECT_FALSE(config.user().vsync);
    EXPECT_EQ(config.user().frame_cap, 240);
}

TEST(SystemConfig, InvalidInMemoryFrameCapSavesAsUnlimited) {
    const Luminumbra::TestSupport::ScopedTempFile file("frame_pacing_", ".json");
    auto config = SystemConfig::Defaults();
    config.user().frame_cap = 61;
    ASSERT_TRUE(config.SaveUserOverlay(file.Path().string()));
    EXPECT_EQ(SystemConfig::LoadFromFile(file.Path().string()).user().frame_cap, 0);
}

// -105 — keybind map resolves action->key; missing action -> fallback.
TEST(SystemConfig, UserControlsKeybindRoundTrip) {
    const std::string json = R"({
      "user": { "controls": { "Move_Forward": 87, "Jump": 32 } }
    })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    EXPECT_EQ(cfg.keybind("Move_Forward", -1), 87);
    EXPECT_EQ(cfg.keybind("Jump", -1), 32);
    EXPECT_EQ(cfg.keybind("Crouch", 67), 67); // unbound -> fallback (compiled default)
}

// -102 — user.* never moves the config sub-hash.
TEST(SystemConfig, UserSettingsDoNotHash) {
    const std::string json = R"({
      "user": { "video": { "fov": 120.0, "vsync": false },
                "controls": { "Jump": 32 } }
    })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    EXPECT_EQ(cfg.ComputeConfigSubHash(), SystemConfig::Defaults().ComputeConfigSubHash());
}

// -103 — SaveUserOverlay round-trips and writes ONLY user.* (no sim/render leakage).
TEST(SystemConfig, UserOverlaySaveReload) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "lumin_systemconfig_test";
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); // non-throwing pre-clean
    const std::filesystem::path path = dir / "settings.json";

    SystemConfig cfg = SystemConfig::FromJsonString(R"({
      "sim": { "erosion": { "enabled": true } },
      "user": { "video": { "fov": 95.0, "window_mode": "windowed" },
                "controls": { "Sprint": 340 } }
    })");
    ASSERT_TRUE(cfg.SaveUserOverlay(path.string()));

    // The written file must contain only user.* — no sim/render keys. Scope the stream so
    // its handle is closed before cleanup (Windows can't delete an open file).
    std::string text;
    {
        std::ifstream in(path);
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    EXPECT_NE(text.find("\"user\""), std::string::npos);
    EXPECT_EQ(text.find("\"sim\""), std::string::npos);
    EXPECT_EQ(text.find("\"render\""), std::string::npos);

    const SystemConfig reloaded = SystemConfig::LoadFromFile(path.string());
    EXPECT_FLOAT_EQ(reloaded.user().fov, 95.0f);
    EXPECT_EQ(reloaded.user().window_mode, "windowed");
    EXPECT_EQ(reloaded.keybind("Sprint", -1), 340);
    EXPECT_TRUE(reloaded.enabled(SysKey::SimErosion)); // overlay carries no sim flags

    std::filesystem::remove_all(dir, rm_ec); // non-throwing teardown
}

// -104 — overlay merge precedence: user.* from overlay, sim/render from defaults;
// a sim.* in the overlay is ignored.
TEST(SystemConfig, OverlayMergePrecedence) {
    SystemConfig cfg = SystemConfig::FromJsonString(R"({
      "sim": { "plant_growth": { "enabled": true } },
      "user": { "video": { "fov": 45.0 } }
    })");
    EXPECT_TRUE(cfg.enabled(SysKey::SimPlantGrowth));
    EXPECT_FLOAT_EQ(cfg.user().fov, 45.0f);

    // Overlay only changes user.*; its sim flag must be ignored.
    cfg.OverlayUserFromJsonString(R"({
      "sim": { "plant_growth": { "enabled": false } },
      "user": { "video": { "fov": 100.0 } }
    })");
    EXPECT_TRUE(cfg.enabled(SysKey::SimPlantGrowth)); // unchanged by overlay
    EXPECT_FLOAT_EQ(cfg.user().fov, 100.0f);          // overlay wins for user.*
}

TEST(SystemConfig, MalformedOverlayKeepsSettings) {
    SystemConfig cfg = SystemConfig::FromJsonString(R"({ "user": { "video": { "fov": 60.0 } } })");
    cfg.OverlayUserFromJsonString("}{ not json");
    EXPECT_FLOAT_EQ(cfg.user().fov, 60.0f); // unchanged
}

TEST(SystemConfig, EnabledMissingTrueFalseAndMalformed) {
    for (const auto& entry : {"{}",
                              "{\"enabled\":true}",
                              "{\"enabled\":false}",
                              "{\"enabled\":0}",
                              "{\"enabled\":null}",
                              "false"}) {
        auto json = nlohmann::json::parse(entry);
        nlohmann::json document = {{"sim", {{"erosion", json}}}};
        const auto cfg = SystemConfig::FromJsonString(document.dump());
        const bool expected = !(json.is_object() && json.contains("enabled") &&
                                json["enabled"].is_boolean() && !json["enabled"].get<bool>());
        EXPECT_EQ(cfg.enabled(SysKey::SimErosion), expected) << entry;
    }
}

TEST(SystemConfig, PackagedFlagsAgreeWithCompiledDefaults) {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                      "data/common/systems.json";
    ASSERT_TRUE(std::filesystem::exists(path));
    std::ifstream file(path);
    const auto document = nlohmann::json::parse(file);
#define CHECK_PACKAGED_FLAG(name, section, json_section, json_name, residency)                     \
    EXPECT_EQ(document.at(json_section).at(json_name).at("enabled").get<bool>(),                   \
              SystemConfig::Defaults().enabled(SysKey::name))                                      \
        << json_name;
    LUMIN_CONFIG_KEY_TABLE(CHECK_PACKAGED_FLAG)
#undef CHECK_PACKAGED_FLAG
    const auto packaged = SystemConfig::LoadFromFile(path.string());
    for (unsigned key = 0; key < static_cast<unsigned>(SysKey::Count); ++key)
        EXPECT_EQ(packaged.enabled(static_cast<SysKey>(key)),
                  SystemConfig::Defaults().enabled(static_cast<SysKey>(key)))
            << key;
}

TEST(SystemConfig, SavedFalseVsyncRemainsAuthoritative) {
    auto cfg = SystemConfig::Defaults();
    cfg.OverlayUserFromJsonString(R"({"user":{"video":{"vsync":false}},
        "sim":{"erosion":{"enabled":false}},"render":{"taau":{"enabled":false}}})");
    EXPECT_FALSE(cfg.user().vsync);
    EXPECT_TRUE(cfg.enabled(SysKey::SimErosion));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderTaau));
}

TEST(SystemConfig, FarWorldCaveFieldDefaultsAndExactNumericCodes) {
    using namespace Luminumbra::Rendering;
    using Luminumbra::World::FarCaveMode;
    const auto defaults = SystemConfig::Defaults();
    EXPECT_TRUE(defaults.enabled(SysKey::RenderFarWorldLadder));
    EXPECT_EQ(ResolveFarWorldCaveField(
                  defaults.param(SysParam::FarWorldCaveField, kDefaultFarWorldCaveFieldCode)),
              FarCaveMode::BandLimited);
    // The compiled schema default and the selector fallback must agree.
    constexpr float schema_defaults[]{
#define SCHEMA_SCALAR_DEFAULT(ENUM, OWNER, NAME, VEC, DEFAULT, X, Y, Z) DEFAULT,
        LUMIN_CONFIG_PARAM_TABLE(SCHEMA_SCALAR_DEFAULT)
#undef SCHEMA_SCALAR_DEFAULT
    };
    EXPECT_EQ(schema_defaults[static_cast<std::size_t>(SysParam::FarWorldCaveField)],
              kDefaultFarWorldCaveFieldCode);
    for (const auto& [code, expected] : {std::pair{"0", FarCaveMode::BandLimited},
                                         std::pair{"-0.0", FarCaveMode::BandLimited},
                                         std::pair{"1", FarCaveMode::BoxFiltered},
                                         std::pair{"1.0", FarCaveMode::BoxFiltered}}) {
        const auto cfg = SystemConfig::FromJsonString(
            std::string(
                R"({"render":{"far_world_ladder":{"enabled":true,"params":{"cave_field":)") +
            code + "}}}}");
        EXPECT_TRUE(cfg.enabled(SysKey::RenderFarWorldLadder));
        EXPECT_EQ(ResolveFarWorldCaveField(cfg.param(SysParam::FarWorldCaveField, -1)), expected);
    }
}

TEST(SystemConfig, InvalidCaveCodesCannotRoundOrUnderflowToValidEnumCodes) {
    using namespace Luminumbra::Rendering;
    for (const auto code : {"-1",
                            "2",
                            "255",
                            "0.5",
                            "1.5",
                            "1.00000001",
                            "0.99999999",
                            "1e-46",
                            "-1e-46",
                            "1e39",
                            "-1e39"}) {
        SCOPED_TRACE(code);
        const auto cfg = SystemConfig::FromJsonString(
            std::string(
                R"({"render":{"far_world_ladder":{"enabled":true,"params":{"cave_field":)") +
            code + "}}}}");
        EXPECT_TRUE(cfg.enabled(SysKey::RenderFarWorldLadder));
        EXPECT_FALSE(ResolveFarWorldCaveField(
            cfg.param(SysParam::FarWorldCaveField, kDefaultFarWorldCaveFieldCode)));
    }
}

TEST(SystemConfig, ExplicitNonNumericCaveFieldsRefuseLadderWithoutChangingOtherParameters) {
    using namespace Luminumbra::Rendering;
    for (const auto value : {"true", "false", R"("BoxFiltered")", "null", "[]", "{}"}) {
        SCOPED_TRACE(value);
        const auto cfg = SystemConfig::FromJsonString(
            std::string(
                R"({"render":{"far_world_ladder":{"enabled":true,"params":{"cave_field":)") +
            value + R"(}},"moonlight":{"params":{"strength":)" + value + "}}}}");
        const auto requested = ResolveFarWorldCaveField(
            cfg.param(SysParam::FarWorldCaveField, kDefaultFarWorldCaveFieldCode));
        EXPECT_FALSE(requested); // Also selects startup's invalid-code warning.
        EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, .75f), .75f);
        FarWorldRenderer renderer(RequestedFarWorldMode(cfg.enabled(SysKey::RenderFarWorldLadder)));
        renderer.SetRequestedCavePolicy(requested);
        const auto selection = renderer.Selection(FarWorldPurpose::World);
        EXPECT_EQ(selection.refusal, FarWorldRefusal::InvalidCavePolicy);
        EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
        EXPECT_FALSE(renderer.CavePolicy(selection).effective);
        Luminumbra::JobSystem jobs;
        EXPECT_EQ(renderer.BindFixture(jobs, {}, 1, 1, FarWorldDevelopmentLimits()),
                  FarWorldRefusal::InvalidCavePolicy);
    }
    const auto absent = SystemConfig::FromJsonString(
        R"({"render":{"far_world_ladder":{"enabled":true,"params":{}}}})");
    EXPECT_EQ(ResolveFarWorldCaveField(
                  absent.param(SysParam::FarWorldCaveField, kDefaultFarWorldCaveFieldCode)),
              Luminumbra::World::FarCaveMode::BandLimited);
}

TEST(SystemConfig, CaveFieldIsRenderOnlyAndIgnoredByUserOverlay) {
    using namespace Luminumbra::Rendering;
    using Luminumbra::World::FarCaveMode;
    const auto sim = SystemConfig::FromJsonString(R"({"sim":{"erosion":{"enabled":true}}})");
    for (const auto enabled : {"false", "true"}) {
        for (const auto code : {"0", "1", "0.5"}) {
            auto cfg = SystemConfig::FromJsonString(
                std::string(
                    R"({"sim":{"erosion":{"enabled":true}},"render":{"far_world_ladder":{"enabled":)") +
                enabled + R"(,"params":{"cave_field":)" + code + "}}}}");
            EXPECT_EQ(cfg.ComputeConfigSubHash(), sim.ComputeConfigSubHash());
            cfg.OverlayUserFromJsonString(R"({"render":{"far_world_ladder":{
                "enabled":true,"params":{"cave_field":1}}}})");
            if (std::string(code) == "0") {
                EXPECT_EQ(ResolveFarWorldCaveField(cfg.param(SysParam::FarWorldCaveField, -1)),
                          FarCaveMode::BandLimited);
            } else if (std::string(code) == "1") {
                EXPECT_EQ(ResolveFarWorldCaveField(cfg.param(SysParam::FarWorldCaveField, -1)),
                          FarCaveMode::BoxFiltered);
            } else {
                EXPECT_FALSE(ResolveFarWorldCaveField(cfg.param(SysParam::FarWorldCaveField, -1)));
            }
            EXPECT_EQ(cfg.enabled(SysKey::RenderFarWorldLadder), std::string(enabled) == "true");
        }
    }
}

} // namespace
