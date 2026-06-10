#include "../../src/luminumbra_common/ai/InstinctPlanner.h"

#include <cmath>
#include <string>

bool LuminumbraInstinctPlannerGateTest() {
    const auto request = luminumbra::ai::MakeGrovestriderHungerFixture();
    const auto plan = luminumbra::ai::PlanInstincts(request);
    if (!luminumbra::ai::InstinctPlannerMeetsBaseline()) {
        return false;
    }
    if (!plan.passed || plan.candidates.size() < 4u) {
        return false;
    }
    if (plan.candidates.front().need != "hunger") {
        return false;
    }
    if (plan.candidates.front().action != "forage" || plan.candidates.front().target != "mossberry_grove") {
        return false;
    }
    if (std::fabs(plan.candidates.front().score - 2.1366) > 0.0001) {
        return false;
    }

    const std::string json = luminumbra::ai::SerializeInstinctPlanJson(plan, "debug");
    return json.find("\"schema\":\"luminumbra.ai.instinct_planner.v1\"") != std::string::npos &&
           json.find("\"selected_target\":\"mossberry_grove\"") != std::string::npos &&
           json.find("\"decision_contract\":\"deterministic_priority_then_cost\"") != std::string::npos;
}
