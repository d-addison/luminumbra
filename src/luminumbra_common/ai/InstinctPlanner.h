#pragma once

#include <string>
#include <vector>

namespace luminumbra::ai {

struct InstinctNeed {
    std::string name;
    double pressure = 0.0;
};

struct InstinctOpportunity {
    std::string id;
    std::string action;
    std::string target;
    std::string need;
    double satisfaction = 0.0;
    double urgency = 0.0;
    double distance = 0.0;
    double risk = 0.0;
    double stamina_cost = 0.0;
};

struct InstinctCandidate {
    int rank = 0;
    std::string id;
    std::string action;
    std::string target;
    std::string need;
    double score = 0.0;
    double need_pressure = 0.0;
    std::string reason;
};

struct InstinctPlanRequest {
    std::string actor_id;
    std::string archetype;
    std::vector<InstinctNeed> needs;
    std::vector<InstinctOpportunity> opportunities;
};

struct InstinctPlan {
    std::string actor_id;
    std::string archetype;
    std::string decision_contract;
    std::vector<InstinctCandidate> candidates;
    int selected_index = -1;
    std::string checksum;
    bool passed = false;
};

// the planner is pure engine — fixtures and pass expectations are
// game data (data/common/archetypes/*.json carries an `expected` block the
// gate consumes). plan.passed is generic: a non-empty deterministic ranking
// with the top candidate selected.
InstinctPlan PlanInstincts(const InstinctPlanRequest& request);
std::string SerializeInstinctPlanJson(const InstinctPlan& plan, const std::string& build_preset);

} // namespace luminumbra::ai
