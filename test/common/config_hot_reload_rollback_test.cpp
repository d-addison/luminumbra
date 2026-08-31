// Generated shared-constant header + config-side hot-reload ROLLBACK
// (  / ).
//
// Two halves, one file:
//
//   (a) Codegen fidelity: the GENERATED shared constant
//       luminumbra::core::config_constants::kMoonlightStrength / kMoonlightColor
//       (src/luminumbra_common/core/ConfigConstants.gen.h, emitted by
//       tools/config_codegen.py --emit-constants) must equal the config DEFAULT authored
//       in ConfigSchema.json. The schema JSON is read here as the INDEPENDENT source of
//       truth, so a regenerated header with a changed default — or a stale header — fails
//       this test even without the configure-time --check-constants gate.
//
//   (b/c) Config-side rollback: today there is a shader-side rollback predicate
//       (Shader::Reload,  : build a NEW program, validate it, ADOPT only on
//       success, else KEEP the previous good program) but no config-side twin. This test
//       provides the config-side mirror. `ConfigHotReloader` holds a last-good SystemConfig
//       and applies an overlay by building a NEW candidate, validating it, and swapping only
//       on success — on ANY validation failure the previous good config is retained. A bad
//       overlay is REJECTED (rollback); a good overlay APPLIES.
//
// Scope note: the reloader lives in this test, not in SystemConfig.{h,cpp}, because this
// change may only touch the codegen + emitted header + this test. The runtime home for the
// predicate (a SystemConfig::TryReload keeping a last-good snapshot) is called out under
// ORCHESTRATOR-WIRE. The structural mirror of Shader::Reload is faithful regardless.
//
// Deterministic + headless: no GPU, no clock, no filesystem writes; the only external read
// is ConfigSchema.json via LUMINUMBRA_SOURCE_ROOT (same pattern as config_constant_drift_test).
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <glm/glm.hpp>

#include "luminumbra_common/core/ConfigConstants.gen.h" // generated shared constants
#include "luminumbra_common/core/SystemConfig.h"

using luminumbra::core::SysKey;
using luminumbra::core::SysParam;
using luminumbra::core::SystemConfig;
namespace kc = luminumbra::core::config_constants;

namespace {

nlohmann::json LoadSchema() {
    const std::string path =
        std::string(LUMINUMBRA_SOURCE_ROOT) + "/src/luminumbra_common/core/ConfigSchema.json";
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << "cannot open ConfigSchema.json at " << path;
    std::stringstream ss;
    ss << in.rdbuf();
    return nlohmann::json::parse(ss.str());
}

// Read the JSON default for a params[].enum out of the schema (independent source of truth).
const nlohmann::json& SchemaParamDefault(const nlohmann::json& schema,
                                         const std::string& enum_name) {
    for (const auto& p : schema.at("params")) {
        if (p.at("enum").get<std::string>() == enum_name)
            return p.at("default");
    }
    ADD_FAILURE() << "schema param not found: " << enum_name;
    static const nlohmann::json kNull;
    return kNull;
}

// ---- config-side rollback predicate (mirror of Shader::Reload / ValidateReflectedLayout) ----

struct ValidationResult {
    bool ok = false;
    std::string diagnostic;
};

// The config "expected layout": moonlight strength is a finite non-negative scale and each
// color component is a finite normalized channel in [0,1]. A candidate that violates this
// contract is the config analogue of a reflected-layout mismatch. Resolved values fall back
// to the GENERATED defaults when unset, so an all-default / moonlight-absent config is valid.
ValidationResult ValidateConfig(const SystemConfig& c) {
    const float strength = c.param(SysParam::MoonlightStrength, kc::kMoonlightStrength);
    if (!std::isfinite(strength) || strength < 0.0f) {
        return {false,
                "render.moonlight.strength must be finite and >= 0 (got " +
                    std::to_string(strength) + ")"};
    }
    const glm::vec3 color = c.param3(SysParam::MoonlightColor, kc::kMoonlightColor);
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(color[i]) || color[i] < 0.0f || color[i] > 1.0f) {
            return {false,
                    "render.moonlight.color component " + std::to_string(i) +
                        " out of [0,1] (got " + std::to_string(color[i]) + ")"};
        }
    }
    return {true, {}};
}

// build a NEW candidate config from the overlay text, validate it, and ADOPT it only
// on success; on ANY validation failure keep the previous good config (rollback). This is the
// structural twin of Shader::Reload — the candidate is built into a SEPARATE object and the
// last-good is never mutated until validation passes.
class ConfigHotReloader {
public:
    explicit ConfigHotReloader(SystemConfig initial)
        : m_good(std::move(initial)) {}

    const SystemConfig& current() const {
        return m_good;
    }

    bool TryReload(const std::string& json_text, std::string* diagnostic = nullptr) {
        SystemConfig candidate = SystemConfig::FromJsonString(json_text); // build a NEW config
        const ValidationResult vr = ValidateConfig(candidate);
        if (!vr.ok) {
            if (diagnostic)
                *diagnostic = vr.diagnostic;
            return false; // rollback: m_good untouched — never adopt an invalid config
        }
        m_good = std::move(candidate); // adopt the validated candidate
        return true;
    }

private:
    SystemConfig m_good;
};

// -------------------------------------------------------------------------------------
// (a) codegen fidelity — the generated moonlight constant equals the config default.

TEST(ConfigHotReloadRollback, GeneratedMoonlightConstantMatchesSchemaDefault) {
    const nlohmann::json schema = LoadSchema();

    const nlohmann::json& strength = SchemaParamDefault(schema, "MoonlightStrength");
    ASSERT_TRUE(strength.is_number()) << "MoonlightStrength default must be a scalar";
    EXPECT_FLOAT_EQ(kc::kMoonlightStrength, static_cast<float>(strength.get<double>()));

    const nlohmann::json& color = SchemaParamDefault(schema, "MoonlightColor");
    ASSERT_TRUE(color.is_array() && color.size() == 3) << "MoonlightColor default must be a vec3";
    EXPECT_FLOAT_EQ(kc::kMoonlightColor.r, static_cast<float>(color[0].get<double>()));
    EXPECT_FLOAT_EQ(kc::kMoonlightColor.g, static_cast<float>(color[1].get<double>()));
    EXPECT_FLOAT_EQ(kc::kMoonlightColor.b, static_cast<float>(color[2].get<double>()));
    // The exposed component scalars (shader-twin fodder) agree with the assembled vec3.
    EXPECT_FLOAT_EQ(kc::kMoonlightColorR, kc::kMoonlightColor.r);
    EXPECT_FLOAT_EQ(kc::kMoonlightColorG, kc::kMoonlightColor.g);
    EXPECT_FLOAT_EQ(kc::kMoonlightColorB, kc::kMoonlightColor.b);
}

// The generated constant is exactly what an unset config resolves to at runtime: the
// SystemConfig fallback path and the schema-authored default are one and the same value.
TEST(ConfigHotReloadRollback, GeneratedConstantIsTheRuntimeMoonlightDefault) {
    const SystemConfig defaults = SystemConfig::Defaults();
    EXPECT_FALSE(defaults.enabled(SysKey::RenderMoonlight));
    EXPECT_FLOAT_EQ(defaults.param(SysParam::MoonlightStrength, kc::kMoonlightStrength),
                    kc::kMoonlightStrength);
    const glm::vec3 color = defaults.param3(SysParam::MoonlightColor, kc::kMoonlightColor);
    EXPECT_FLOAT_EQ(color.r, kc::kMoonlightColor.r);
    EXPECT_FLOAT_EQ(color.g, kc::kMoonlightColor.g);
    EXPECT_FLOAT_EQ(color.b, kc::kMoonlightColor.b);
}

// The validator accepts the default (generated-constant) config and rejects out-of-contract
// values — the predicate the rollback decision hinges on.
TEST(ConfigHotReloadRollback, ValidatorAcceptsDefaultsRejectsOutOfRange) {
    EXPECT_TRUE(ValidateConfig(SystemConfig::Defaults()).ok);
    EXPECT_TRUE(ValidateConfig(SystemConfig::FromJsonString(
                                   R"({ "render": { "moonlight": { "enabled": true,
                        "params": { "strength": 0.5, "color": [0.6, 0.7, 1.0] } } } })"))
                    .ok);
    // negative strength
    EXPECT_FALSE(ValidateConfig(SystemConfig::FromJsonString(
                                    R"({ "render": { "moonlight": { "enabled": true,
                         "params": { "strength": -1.0 } } } })"))
                     .ok);
    // color channel out of [0,1]
    EXPECT_FALSE(ValidateConfig(SystemConfig::FromJsonString(
                                    R"({ "render": { "moonlight": { "enabled": true,
                         "params": { "color": [0.6, 0.7, 5.0] } } } })"))
                     .ok);
}

// -------------------------------------------------------------------------------------
// (b) a bad overlay is REJECTED and the prior good config is retained (rollback).

TEST(ConfigHotReloadRollback, BadOverlayRejectedPriorConfigRetained) {
    // Last-good: moonlight enabled with valid, non-default values.
    ConfigHotReloader reloader(SystemConfig::FromJsonString(
        R"({ "render": { "moonlight": { "enabled": true,
            "params": { "strength": 0.5, "color": [0.6, 0.7, 1.0] } } } })"));
    ASSERT_TRUE(ValidateConfig(reloader.current()).ok);
    ASSERT_FLOAT_EQ(reloader.current().param(SysParam::MoonlightStrength, kc::kMoonlightStrength),
                    0.5f);

    // Bad overlay #1: blue channel out of [0,1] -> fails validation -> rollback.
    std::string diag;
    EXPECT_FALSE(reloader.TryReload(
        R"({ "render": { "moonlight": { "enabled": true,
            "params": { "strength": 0.5, "color": [0.6, 0.7, 5.0] } } } })",
        &diag));
    EXPECT_FALSE(diag.empty()) << "a rejected reload must report a diagnostic";

    // The prior good config is retained: the invalid candidate was never adopted.
    EXPECT_FLOAT_EQ(reloader.current().param(SysParam::MoonlightStrength, kc::kMoonlightStrength),
                    0.5f);
    const glm::vec3 color =
        reloader.current().param3(SysParam::MoonlightColor, kc::kMoonlightColor);
    EXPECT_FLOAT_EQ(color.b, 1.0f); // NOT 5.0 — rolled back

    // Bad overlay #2: negative strength -> also rejected, still the last-good is kept.
    EXPECT_FALSE(reloader.TryReload(
        R"({ "render": { "moonlight": { "enabled": true, "params": { "strength": -3.0 } } } })"));
    EXPECT_FLOAT_EQ(reloader.current().param(SysParam::MoonlightStrength, kc::kMoonlightStrength),
                    0.5f);
    EXPECT_TRUE(ValidateConfig(reloader.current()).ok) << "retained config stays valid";
}

// -------------------------------------------------------------------------------------
// (c) a good overlay APPLIES (the candidate is adopted).

TEST(ConfigHotReloadRollback, GoodOverlayApplies) {
    ConfigHotReloader reloader(SystemConfig::Defaults());
    // starts at the generated default
    EXPECT_FLOAT_EQ(reloader.current().param(SysParam::MoonlightStrength, kc::kMoonlightStrength),
                    kc::kMoonlightStrength);

    std::string diag;
    EXPECT_TRUE(reloader.TryReload(
        R"({ "render": { "moonlight": { "enabled": true,
            "params": { "strength": 0.8, "color": [0.5, 0.6, 0.9] } } } })",
        &diag));
    EXPECT_TRUE(diag.empty());

    EXPECT_TRUE(reloader.current().enabled(SysKey::RenderMoonlight));
    EXPECT_FLOAT_EQ(reloader.current().param(SysParam::MoonlightStrength, kc::kMoonlightStrength),
                    0.8f);
    const glm::vec3 color =
        reloader.current().param3(SysParam::MoonlightColor, kc::kMoonlightColor);
    EXPECT_FLOAT_EQ(color.r, 0.5f);
    EXPECT_FLOAT_EQ(color.g, 0.6f);
    EXPECT_FLOAT_EQ(color.b, 0.9f);
}

// A good overlay after a rejected one still applies: rollback leaves the reloader usable.
TEST(ConfigHotReloadRollback, GoodOverlayAppliesAfterRollback) {
    ConfigHotReloader reloader(SystemConfig::FromJsonString(
        R"({ "render": { "moonlight": { "enabled": true, "params": { "strength": 0.5 } } } })"));

    EXPECT_FALSE(reloader.TryReload(
        R"({ "render": { "moonlight": { "enabled": true, "params": { "strength": -1.0 } } } })"));
    EXPECT_FLOAT_EQ(reloader.current().param(SysParam::MoonlightStrength, kc::kMoonlightStrength),
                    0.5f); // rolled back

    EXPECT_TRUE(reloader.TryReload(
        R"({ "render": { "moonlight": { "enabled": true, "params": { "strength": 0.9 } } } })"));
    EXPECT_FLOAT_EQ(reloader.current().param(SysParam::MoonlightStrength, kc::kMoonlightStrength),
                    0.9f); // adopted
}

} // namespace
