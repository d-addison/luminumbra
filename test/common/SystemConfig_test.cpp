//  SystemConfig substrate — contract tests derived from spec
// Acceptance Criteria ..005.
// These reference luminumbra::core::SystemConfig, which does not exist yet:
// the test keeps the generated schema and runtime registry aligned.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <glm/glm.hpp>
#include <iomanip>

#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h" // StableChecksum

using luminumbra::core::SysKey;
using luminumbra::core::SysParam;
using luminumbra::core::SystemConfig;

namespace {

// missing/empty config yields all-defaults, no crash.
TEST(SystemConfig, DefaultsAllOff) {
    const SystemConfig cfg = SystemConfig::Defaults();
    EXPECT_FALSE(cfg.enabled(SysKey::SimPlantGrowth));
    EXPECT_FALSE(cfg.enabled(SysKey::SimErosion));
    EXPECT_FALSE(cfg.enabled(SysKey::RenderMoonlight));
    EXPECT_FALSE(cfg.enabled(SysKey::RenderTreeWind));
    EXPECT_FLOAT_EQ(cfg.param(SysParam::PlantMutationRate, 0.123f), 0.123f);
    EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, 0.5f), 0.5f);
}

TEST(SystemConfig, MissingFileIsDefaults) {
    const SystemConfig cfg = SystemConfig::LoadFromFile("this/path/does/not/exist.json");
    EXPECT_FALSE(cfg.enabled(SysKey::RenderMoonlight));
    EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, 0.7f), 0.7f);
}

TEST(SystemConfig, EmptyAndBlankJsonAreDefaults) {
    for (const char* text : {"", "{}", "   ", "not json at all"}) {
        const SystemConfig cfg = SystemConfig::FromJsonString(text);
        EXPECT_FALSE(cfg.enabled(SysKey::SimPlantGrowth)) << "input: " << text;
    }
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

// config sub-hash is empty at all-defaults (sub-hash set byte-identical).
TEST(SystemConfig, ConfigSubHashEmptyAtDefaults) {
    EXPECT_EQ(SystemConfig::Defaults().ComputeConfigSubHash(), std::string{});
}

// sim non-default flag/param moves the sub-hash; render flags never do.
TEST(SystemConfig, RenderFlagDoesNotMoveConfigSubHash) {
    const std::string json = R"({
      "render": { "moonlight": { "enabled": true, "params": { "strength": 9.9 } },
                  "tree_wind": { "enabled": true } }
    })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    EXPECT_EQ(cfg.ComputeConfigSubHash(), std::string{});
}

TEST(SystemConfig, SimFlagMovesConfigSubHash) {
    const std::string json = R"({ "sim": { "plant_growth": { "enabled": true } } })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    EXPECT_NE(cfg.ComputeConfigSubHash(), std::string{});
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

TEST(SystemConfig, ConfigSubHashByteIdenticalSnapshot) {
    // Single sim key with one scalar param at its compiled default.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "plant_growth:en=1;mutation_rate=" << 0.05f << ';';
        const auto cfg =
            SystemConfig::FromJsonString(R"({ "sim": { "plant_growth": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // Sim key with NO owned params still emits the enabled marker and nothing after it.
    {
        const auto cfg =
            SystemConfig::FromJsonString(R"({ "sim": { "erosion": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash("erosion:en=1;"));
    }
    // Sim key owning multiple scalar params: every owned default is emitted in kParams order.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "foraging:en=1;deposit=" << 1.0f
             << ";trail_weight=" << 8.0f << ";goal_weight=" << 1.0f << ';';
        const auto cfg =
            SystemConfig::FromJsonString(R"({ "sim": { "foraging": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // Multiple enabled sim keys serialize in kKeys (schema) order: plant_growth precedes erosion.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "plant_growth:en=1;mutation_rate=" << 0.05f
             << ";erosion:en=1;";
        const auto cfg = SystemConfig::FromJsonString(
            R"({ "sim": { "erosion": { "enabled": true },
                          "plant_growth": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // An explicitly-set non-default param overrides the generated default in the same format.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "plant_growth:en=1;mutation_rate=" << 0.2f << ';';
        const auto cfg = SystemConfig::FromJsonString(
            R"({ "sim": { "plant_growth": { "enabled": true, "params": { "mutation_rate": 0.2 } } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // All-default (no sim enabled) is the empty baseline — byte-identical to today.
    EXPECT_EQ(SystemConfig::Defaults().ComputeConfigSubHash(), std::string{});
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

// Owner request 2026-06-18: default look sensitivity is 25% of the prior 0.1, and VSync
// defaults OFF (preserve uncapped 300fps until the player opts in).
TEST(SystemConfig, UserVideoDefaults) {
    const auto& u = SystemConfig::Defaults().user();
    EXPECT_FLOAT_EQ(u.mouse_sensitivity, 0.025f);
    EXPECT_FALSE(u.vsync);
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
    EXPECT_EQ(cfg.ComputeConfigSubHash(), std::string{});
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
    EXPECT_FALSE(reloaded.enabled(SysKey::SimErosion)); // overlay carries no sim flags

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

} // namespace
