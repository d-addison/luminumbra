#pragma once

#include "luminumbra_common/simulation/SimBudgetProfile.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::Server {

// Additive artifact extension. Calling this with a disabled collector is byte-neutral.
inline void AppendSimBudgetArtifact(nlohmann::json& artifact,
                                    const luminumbra::simulation::SimBudgetTelemetry& first,
                                    const luminumbra::simulation::SimBudgetTelemetry& replay) {
    if (!first.Enabled())
        return;
    using namespace luminumbra::simulation;
    nlohmann::json stages = nlohmann::json::array();
    for (std::size_t i = 0; i < kSimBudgetStageNames.size(); ++i) {
        const auto summary = first.Summarize(static_cast<SimBudgetStage>(i));
        nlohmann::json work = nlohmann::json::array();
        for (const auto& sample : first.SamplesByStage()[i])
            work.push_back({{"tick", sample.tick}, {"work", sample.work}});
        stages.push_back({{"name", kSimBudgetStageNames[i]},
                          {"samples", summary.samples},
                          {"work_total", summary.work_total},
                          {"work_trace", std::move(work)},
                          {"duration_ms",
                           summary.samples == 0 ? nlohmann::json(nullptr)
                                                : nlohmann::json{{"p50", summary.p50_ms},
                                                                 {"p95", summary.p95_ms},
                                                                 {"p99", summary.p99_ms},
                                                                 {"maximum", summary.max_ms}}}});
    }
    artifact["sim_budget"] = {
        {"schema", "luminumbra.sim_budget.v2"},
        {"work_replay_match", replay.Enabled() && first.WorkMatches(replay)},
        {"stages", std::move(stages)},
    };
    nlohmann::json counters = nlohmann::json::array();
    for (std::size_t i = 0; i < kSimWorkNames.size(); ++i) {
        nlohmann::json trace = nlohmann::json::array();
        for (const auto& sample : first.WorkItems()[i])
            trace.push_back({{"tick", sample.tick}, {"work", sample.work}});
        counters.push_back({{"name", kSimWorkNames[i]}, {"work_trace", std::move(trace)}});
    }
    artifact["sim_budget"]["work_items"] = {{"schema", "luminumbra.sim_work.v1"},
                                            {"counters", std::move(counters)}};
}

// Evaluates a budget profile against the collector's stage ticks. The engine emits no residency
// samples yet, so every profile residency class is reported as unevaluated. Disabled collectors
// yield a passing verdict without evaluation.
inline luminumbra::simulation::BudgetVerdict
EvaluateSimBudgetProfile(const luminumbra::simulation::SimBudgetProfile& profile,
                         const luminumbra::simulation::SimBudgetTelemetry& telemetry) {
    using namespace luminumbra::simulation;
    if (!telemetry.Enabled())
        return BudgetVerdict{};
    std::vector<StageTick> ticks;
    const auto& by_stage = telemetry.SamplesByStage();
    for (std::size_t i = 0; i < kSimBudgetStageNames.size(); ++i) {
        for (const auto& sample : by_stage[i]) {
            StageTick tick;
            tick.stage = std::string(kSimBudgetStageNames[i]);
            tick.tick = sample.tick;
            tick.work = sample.work;
            ticks.push_back(std::move(tick));
        }
    }
    const std::vector<ResidencySample> no_residency;
    return EvaluateSimBudget(profile, ticks, no_residency);
}

// Additive enforcement block under artifact["sim_budget"]["enforcement"]. Does nothing (and
// returns a passing verdict) when the collector is disabled or AppendSimBudgetArtifact did not
// write a sim_budget block. Returns the verdict that was written.
inline luminumbra::simulation::BudgetVerdict
AppendSimBudgetEnforcement(nlohmann::json& artifact,
                           const luminumbra::simulation::SimBudgetProfile& profile,
                           std::uint64_t profile_bytes,
                           const luminumbra::simulation::SimBudgetTelemetry& first) {
    using luminumbra::simulation::BudgetViolation;
    if (!first.Enabled() || !artifact.contains("sim_budget"))
        return luminumbra::simulation::BudgetVerdict{};
    const luminumbra::simulation::BudgetVerdict verdict = EvaluateSimBudgetProfile(profile, first);
    nlohmann::json violations = nlohmann::json::array();
    for (const auto& violation : verdict.violations) {
        const bool is_stage = violation.kind == BudgetViolation::Kind::Stage;
        violations.push_back({{"kind", is_stage ? "stage" : "residency"},
                              {"name", violation.name},
                              {"tick", violation.tick},
                              {"observed", violation.observed},
                              {"limit", violation.limit}});
    }
    artifact["sim_budget"]["enforcement"] = {
        {"schema", "luminumbra.sim_budget_enforcement.v1"},
        {"profile_bytes", profile_bytes},
        {"no_violation", verdict.no_violation},
        {"coverage_complete", verdict.coverage_complete},
        {"violations", std::move(violations)},
        {"unevaluated", verdict.unevaluated},
    };
    return verdict;
}

} // namespace Luminumbra::Server
