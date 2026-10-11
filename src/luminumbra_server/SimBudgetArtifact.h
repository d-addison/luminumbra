#pragma once

#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include <cstddef>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <utility>

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
}

} // namespace Luminumbra::Server
