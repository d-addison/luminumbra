// the grovestrider hunger fixture is GAME DATA
// (data/common/archetypes/grovestrider.json); this gate builds the planner
// request from that file and validates the plan against the file's
// `expected` block. The engine planner stays content-free — this test also
// asserts the engine source no longer embeds the game nouns (inverting the
// old gate, which required "mossberry_grove" inside InstinctPlanner.cpp).

#include "../../src/luminumbra_common/ai/InstinctPlanner.h"

#include "nlohmann/json.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

namespace {

std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// Relocated baseline validation (was engine-side InstinctPlannerMeetsBaseline
// with hardcoded game strings): the plan must match the game-data `expected`
// block exactly.
bool InstinctPlannerMeetsBaseline(const luminumbra::ai::InstinctPlan& plan,
                                  const nlohmann::json& expected) {
    if (!plan.passed || plan.selected_index != 0) {
        return false;
    }
    if (plan.decision_contract != expected.at("decision_contract").get<std::string>()) {
        return false;
    }
    if (plan.candidates.size() != expected.at("candidate_count").get<std::size_t>()) {
        return false;
    }
    if (plan.checksum != expected.at("checksum").get<std::string>()) {
        return false;
    }

    const auto& selected = plan.candidates.front();
    if (selected.need != expected.at("dominant_need").get<std::string>() ||
        selected.action != expected.at("selected_action").get<std::string>() ||
        selected.target != expected.at("selected_target").get<std::string>()) {
        return false;
    }
    if (std::fabs(selected.score - expected.at("selected_score").get<double>()) > 0.0001) {
        return false;
    }

    const nlohmann::json& expected_candidates = expected.at("candidates");
    if (expected_candidates.size() != plan.candidates.size()) {
        return false;
    }
    for (std::size_t i = 0; i < plan.candidates.size(); ++i) {
        const auto& candidate = plan.candidates[i];
        const nlohmann::json& row = expected_candidates[i];
        if (candidate.rank != row.at("rank").get<int>() ||
            candidate.id != row.at("id").get<std::string>() ||
            candidate.need != row.at("need").get<std::string>() ||
            candidate.action != row.at("action").get<std::string>() ||
            candidate.target != row.at("target").get<std::string>()) {
            return false;
        }
        if (std::fabs(candidate.score - row.at("score").get<double>()) > 0.0001) {
            return false;
        }
    }
    return true;
}

} // namespace

bool LuminumbraInstinctPlannerGateTest() {
    const std::filesystem::path source_root(LUMINUMBRA_SOURCE_ROOT);
    const std::filesystem::path archetype_path =
        source_root / "data" / "common" / "archetypes" / "grovestrider.json";

    const std::string archetype_text = ReadFileText(archetype_path);
    if (archetype_text.empty()) {
        return false;
    }

    nlohmann::json archetype;
    try {
        archetype = nlohmann::json::parse(archetype_text);
    } catch (...) {
        return false;
    }
    if (archetype.value("schema", "") != "luminumbra.game.archetype.v1") {
        return false;
    }

    // Build the planner request purely from game data.
    luminumbra::ai::InstinctPlanRequest request;
    request.actor_id = archetype.at("actor_id").get<std::string>();
    request.archetype = archetype.at("archetype").get<std::string>();
    for (const auto& need : archetype.at("needs")) {
        request.needs.push_back(
            {need.at("name").get<std::string>(), need.at("pressure").get<double>()});
    }
    for (const auto& opportunity : archetype.at("opportunities")) {
        request.opportunities.push_back({
            opportunity.at("id").get<std::string>(),
            opportunity.at("action").get<std::string>(),
            opportunity.at("target").get<std::string>(),
            opportunity.at("need").get<std::string>(),
            opportunity.at("satisfaction").get<double>(),
            opportunity.at("urgency").get<double>(),
            opportunity.at("distance").get<double>(),
            opportunity.at("risk").get<double>(),
            opportunity.at("stamina_cost").get<double>(),
        });
    }

    const auto plan = luminumbra::ai::PlanInstincts(request);
    const nlohmann::json& expected = archetype.at("expected");
    if (!InstinctPlannerMeetsBaseline(plan, expected)) {
        return false;
    }
    const auto& selected = plan.candidates.front();

    // Engine/game split: the planner source must be free of the game nouns
    // that used to live in MakeGrovestriderHungerFixture.
    for (const char* engine_file : {"ai/InstinctPlanner.cpp",
                                    "ai/InstinctPlanner.h",
                                    "ai/InstinctSystem.cpp",
                                    "ai/InstinctSystem.h"}) {
        const std::string text =
            ReadFileText(source_root / "src" / "luminumbra_common" / engine_file);
        if (text.empty()) {
            return false;
        }
        for (const char* noun : {"grovestrider",
                                 "Grovestrider",
                                 "mossberry",
                                 "glowcap",
                                 "thunder_hollow",
                                 "stream_reeds"}) {
            if (text.find(noun) != std::string::npos) {
                return false;
            }
        }
    }

    const std::string json = luminumbra::ai::SerializeInstinctPlanJson(plan, "debug");
    return json.find("\"schema\":\"luminumbra.ai.instinct_planner.v1\"") != std::string::npos &&
           json.find("\"selected_target\":\"" + selected.target + "\"") != std::string::npos &&
           json.find("\"decision_contract\":\"deterministic_priority_then_cost\"") !=
               std::string::npos;
}
