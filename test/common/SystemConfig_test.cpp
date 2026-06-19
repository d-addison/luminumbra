// §1a SystemConfig substrate — RED-first tests derived from spec
// `.forge/specs/system-config/spec.md` Acceptance Criteria AC-SC-001..005.
// These reference luminumbra::core::SystemConfig, which does not exist yet:
// the test is expected to FAIL TO COMPILE until SystemConfig.{h,cpp} land (RED).
#include <gtest/gtest.h>

#include <string>

#include <glm/glm.hpp>

#include "luminumbra_common/core/SystemConfig.h"

using luminumbra::core::SysKey;
using luminumbra::core::SysParam;
using luminumbra::core::SystemConfig;

namespace {

// AC-SC-001 — missing/empty config yields all-defaults, no crash.
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

// AC-SC-002 — param parse + defaults round-trip; unknown keys ignored.
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

// AC-SC-003 — config sub-hash is empty at all-defaults (sub-hash set byte-identical).
TEST(SystemConfig, ConfigSubHashEmptyAtDefaults) {
    EXPECT_EQ(SystemConfig::Defaults().ComputeConfigSubHash(), std::string{});
}

// AC-SC-004 — sim non-default flag/param moves the sub-hash; render flags never do.
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

// AC-SC-005 — enabled() is a cheap, deterministic hot-path query (structural O(1);
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

}  // namespace
