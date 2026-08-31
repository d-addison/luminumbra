// Config schema owning-CONSTANT cross-check ( , second half).
//
// The existing  gate (tools/config_codegen.py --check, the ConfigSchemaCheck
// frontier gate) proves the GENERATED registry header is fresh vs ConfigSchema.json and
// that residency is schema-declared. It does NOT prove that each schema DEFAULT still
// equals the live C++ constant it is supposed to MIRROR. That second half is the hazard
// this test closes: when a sim system is ENABLED but a param is left UNSET,
// ComputeConfigSubHash serializes the schema default (SystemConfig.cpp), while the
// system's behaviour uses the owning-struct fallback (the Resolve* functions in
// ai/EcologyTuningConfig.h + ai/SimTuningConfig.h pass `t.<member>` as the fallback). If
// the schema default silently drifts from that member, the config sub-hash no longer
// faithfully identifies the parameters the sim actually ran with — a determinism split
// the header-freshness gate cannot see.
//
// Cross-check design (non-tautological): the "live" value is read from a
// DEFAULT-CONSTRUCTED tuning struct — an INDEPENDENT compiled source in another header —
// never re-typed as a literal here. Edit a tuning constant without updating the schema
// and BuildLiveConstants moves while the schema default does not, so this test fails.
//
// Each mirrored param now names its owning constant in the schema ("constant" +
// "constant_system"); render-only (excluded, never hashed) params are honestly
// annotated "constant": null and
// listed in ExemptParams — a param in NEITHER partition fails the test (fail-closed).
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ai/EcologyTuningConfig.h" // EcologyTuning (via CreatureBrainSystem.h)
#include "ai/SimTuningConfig.h" // WildlifeFoliage/Thirst/Scavenging/Foraging/Reproduction tunings
#include "systems/PollinationSystem.h"

namespace {

// The float equivalence ComputeConfigSubHash (which serializes pm.default_scalar as a
// float) and SystemConfig::param (which returns a float) actually operate under: cast
// the schema's JSON double to float and compare. Schema defaults are authored as the SAME
// literals as the owning struct members, so exact equality is the intent; kDriftEps only
// absorbs decimal->double->float double-rounding, orders of magnitude below any real drift
// (the smallest checked default is 0.006 and real drifts are whole-value).
constexpr float kDriftEps = 1e-6f;

bool DefaultsMatch(double schema_default, float live_constant) {
    const float s = static_cast<float>(schema_default);
    const float scale = std::max({1.0f, std::fabs(s), std::fabs(live_constant)});
    return std::fabs(s - live_constant) <= kDriftEps * scale;
}

// param enum name -> the LIVE compiled constant, read from a default-constructed tuning
// struct (the exact value the Resolve* fallback feeds the sim when the param is unset).
// Heterogeneous member types (uint32 tick fields, double foraging weights) are cast to
// float — the same float the hash path serializes.
std::map<std::string, float> BuildLiveConstants() {
    using namespace luminumbra::ai;
    const EcologyTuning eco{};
    const WildlifeFoliageTuning wf{};
    const ThirstTuning thirst{};
    const ScavengingTuning scav{};
    const ForagingParams forage{};
    const ReproductionTuning repro{};
    return {
        {"PlantMutationRate", luminumbra::foliage::kPollinationMutationFrac},
        {"EcoEnergyDrain", eco.energy_drain_per_second},
        {"EcoEnergyRestRecover", eco.energy_rest_recover},
        {"EcoEnergySleepRecover", eco.energy_sleep_recover},
        {"EcoHungerGrowth", eco.hunger_growth_per_second},
        {"EcoHungerGrazeSate", eco.hunger_graze_sate},
        {"EcoStaminaRestRecover", eco.stamina_rest_recover},
        {"EcoStaminaMoveDrain", eco.stamina_move_drain},
        {"EcoHerdWeight", eco.herd_weight},
        {"EcoAlignmentWeight", eco.alignment_weight},
        {"EcoCatchRadius", eco.catch_radius},
        {"EcoCatchSatiation", eco.catch_satiation},
        {"EcoFlockNeighborRadius", eco.flock_neighbor_radius},
        {"EcoFlockSeparationRadius", eco.flock_separation_radius},
        {"EcoFlockCohesionWeight", eco.flock_cohesion_weight},
        {"EcoFlockSeparationWeight", eco.flock_separation_weight},
        {"WfGrazeRadius", wf.graze_radius},
        {"WfGrazePerCreature", wf.graze_per_creature},
        {"WfRegrowPerTick", wf.regrow_per_tick},
        {"WfFeedPerGraze", wf.feed_per_graze},
        {"ThirstRiseRate", thirst.rise_rate},
        {"ThirstDrinkRate", thirst.drink_rate},
        {"ThirstSeekThreshold", thirst.seek_threshold},
        {"ScavHungerThreshold", scav.hunger_threshold},
        {"ScavFeedRadius", scav.feed_radius},
        {"ScavFeedRate", scav.feed_rate},
        {"ReproMaturityTicks", static_cast<float>(repro.maturity_ticks)},
        {"ReproCooldownTicks", static_cast<float>(repro.cooldown_ticks)},
        {"ReproHealthyStamina", repro.healthy_stamina},
        {"ReproMateSeekRadius", repro.mate_seek_radius},
        {"ReproCourtshipRadius", repro.courtship_radius},
        {"ReproCourtshipTicks", static_cast<float>(repro.courtship_ticks)},
        {"ReproSpawnRadius", repro.spawn_radius},
        {"ForagingDeposit", static_cast<float>(forage.deposit)},
        {"ForagingTrailWeight", static_cast<float>(forage.trail_weight)},
        {"ForagingGoalWeight", static_cast<float>(forage.goal_weight)},
    };
}

// Params with no live compiled constant reachable from this (common) test: render-only
// params (excluded from every hash; their defaults are inline literals in the
// luminumbra_client TU, not linked here).
// This test OWNS the checked/exempt partition — a JSON flag could be mis-set, so we do not
// trust one.
std::set<std::string> ExemptParams() {
    return {
        "MoonlightStrength",
        "MoonlightColor",     // render.moonlight (excluded)
        "CircadianAmplitude", // render.circadian (excluded)
        "SpawnHerdCount",
        "SpawnPredatorSpeed",
        "SpawnPreySpeed",
        "SpawnInitialHunger",
        "ColonyAntCount",
        "ColonyFoodAmount",
    };
}

nlohmann::json LoadSchema() {
    const std::string path =
        std::string(LUMINUMBRA_SOURCE_ROOT) + "/src/luminumbra_common/core/ConfigSchema.json";
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << "cannot open ConfigSchema.json at " << path;
    std::stringstream ss;
    ss << in.rdbuf();
    return nlohmann::json::parse(ss.str());
}

// The single authoritative check, factored so the "matching PASSES" and "drift DETECTED"
// tests exercise the SAME logic. Returns one human-readable message per problem; empty ==
// every param is either a live constant that matches its schema default, or exempt.
std::vector<std::string> FindDrifts(const nlohmann::json& schema,
                                    const std::map<std::string, float>& live,
                                    const std::set<std::string>& exempt) {
    std::vector<std::string> issues;
    for (const auto& p : schema.at("params")) {
        const std::string name = p.at("enum").get<std::string>();
        const bool is_live = live.count(name) != 0;
        const bool is_exempt = exempt.count(name) != 0;

        if (is_live && is_exempt) {
            issues.push_back(name + ": param is both live-checked AND exempt");
            continue;
        }
        if (is_live) {
            // Schema half of: a mirrored param must NAME its owning constant.
            if (!(p.contains("constant") && p.at("constant").is_string() &&
                  !p.at("constant").get<std::string>().empty())) {
                issues.push_back(name + ": missing owning-constant ref (\"constant\") in schema");
            } else if (!(p.contains("constant_system") && p.at("constant_system").is_string())) {
                issues.push_back(name + ": missing \"constant_system\" in schema");
            } else if (!p.at("default").is_number()) {
                issues.push_back(name + ": non-scalar default cannot be cross-checked");
            } else if (!DefaultsMatch(p.at("default").get<double>(), live.at(name))) {
                std::ostringstream m;
                m << name << ": DRIFT schema default=" << p.at("default").get<double>()
                  << " != live " << p.at("constant").get<std::string>() << '=' << live.at(name);
                issues.push_back(m.str());
            }
        } else if (is_exempt) {
            if (!(p.contains("constant") && p.at("constant").is_null())) {
                issues.push_back(name + ": exempt param must annotate \"constant\": null");
            }
        } else {
            issues.push_back(name +
                             ": no constant cross-check wired — add it to BuildLiveConstants() "
                             "(if it mirrors a tuning-struct constant) or ExemptParams() (with a "
                             "documented reason). Fail-closed by design.");
        }
    }
    return issues;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines)
        out += "  - " + l + "\n";
    return out;
}

// -------------------------------------------------------------------------------------

// Every mirrored schema default equals the live compiled constant, and every param is
// accounted for (live-checked or documented-exempt). This is the PASS proof.
TEST(ConfigConstantDrift, SchemaDefaultsMatchLiveConstants) {
    const nlohmann::json schema = LoadSchema();
    const auto live = BuildLiveConstants();
    const auto exempt = ExemptParams();

    // Reverse coverage: every name this test wires must actually exist in the schema
    // (guards a typo / stale rename in BuildLiveConstants or ExemptParams).
    std::set<std::string> schema_names;
    for (const auto& p : schema.at("params"))
        schema_names.insert(p.at("enum").get<std::string>());
    for (const auto& kv : live)
        EXPECT_TRUE(schema_names.count(kv.first)) << "live-map param not in schema: " << kv.first;
    for (const auto& name : exempt)
        EXPECT_TRUE(schema_names.count(name)) << "exempt param not in schema: " << name;

    const auto issues = FindDrifts(schema, live, exempt);
    EXPECT_TRUE(issues.empty()) << "config schema default != owning C++ constant:\n"
                                << Join(issues);
}

// FAIL-CLOSED proof #1: an intentionally-drifted schema default is DETECTED by the SAME
// FindDrifts logic the pass test uses (mutate the in-memory schema, not the live constant).
TEST(ConfigConstantDrift, IntentionalDriftIsDetected) {
    nlohmann::json schema = LoadSchema();
    bool drifted = false;
    for (auto& p : schema.at("params")) {
        if (p.at("enum").get<std::string>() == "EcoEnergyDrain") {
            p["default"] = p.at("default").get<double>() + 0.5; // 0.006 -> 0.506 (real drift)
            drifted = true;
            break;
        }
    }
    ASSERT_TRUE(drifted) << "EcoEnergyDrain must exist to drift it";

    const auto issues = FindDrifts(schema, BuildLiveConstants(), ExemptParams());
    ASSERT_FALSE(issues.empty()) << "a drifted default MUST be detected (fail-closed)";
    const bool named = std::any_of(issues.begin(), issues.end(), [](const std::string& s) {
        return s.find("EcoEnergyDrain") != std::string::npos &&
               s.find("DRIFT") != std::string::npos;
    });
    EXPECT_TRUE(named) << "the detected issue must name the drifted param";
}

// FAIL-CLOSED proof #2: a param present in the schema but wired into NEITHER partition
// (someone added a config param without declaring what constant it mirrors) is flagged.
TEST(ConfigConstantDrift, UnwiredParamIsFailClosed) {
    const nlohmann::json schema = LoadSchema();
    auto live = BuildLiveConstants();
    live.erase("EcoEnergyDrain");       // simulate "forgot to wire the cross-check"
    const auto exempt = ExemptParams(); // and it is not exempt either

    const auto issues = FindDrifts(schema, live, exempt);
    const bool flagged = std::any_of(issues.begin(), issues.end(), [](const std::string& s) {
        return s.find("EcoEnergyDrain") != std::string::npos &&
               s.find("no constant cross-check") != std::string::npos;
    });
    EXPECT_TRUE(flagged) << "an unwired schema param must fail the check";
}

// The comparator mirrors the float equivalence the hash path uses, across the
// heterogeneous member types, and rejects real drift.
TEST(ConfigConstantDrift, DriftComparatorMatchesFloatEquivalence) {
    using namespace luminumbra::ai;
    EXPECT_TRUE(DefaultsMatch(0.006, EcologyTuning{}.energy_drain_per_second));
    EXPECT_TRUE(DefaultsMatch(90.0, static_cast<float>(ReproductionTuning{}.maturity_ticks)));
    EXPECT_TRUE(DefaultsMatch(1.0, static_cast<float>(ForagingParams{}.deposit)));
    EXPECT_FALSE(DefaultsMatch(0.006, 0.007f)); // ~16% drift
    EXPECT_FALSE(DefaultsMatch(0.5, EcologyTuning{}.energy_drain_per_second));
    EXPECT_FALSE(DefaultsMatch(0.006, 0.0f));
}

} // namespace
