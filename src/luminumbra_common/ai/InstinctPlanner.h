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

InstinctPlanRequest MakeGrovestriderHungerFixture();
InstinctPlan PlanInstincts(const InstinctPlanRequest& request);
std::string SerializeInstinctPlanJson(const InstinctPlan& plan, const std::string& build_preset);
bool InstinctPlannerMeetsBaseline();

} // namespace luminumbra::ai
