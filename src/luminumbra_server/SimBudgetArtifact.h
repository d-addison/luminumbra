#pragma once

#include "luminumbra_common/simulation/SimBudgetProfile.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::Server {

inline nlohmann::json RegionDurationJson(
    const std::optional<luminumbra::simulation::RegionTelemetry::Distribution>& duration) {
    if (!duration)
        return nullptr;
    return {{"p50", duration->p50},
            {"p95", duration->p95},
            {"p99", duration->p99},
            {"maximum", duration->maximum}};
}

inline nlohmann::json RegionGaugeJson(const luminumbra::simulation::RegionTelemetry::Gauge& gauge) {
    return {{"current", gauge.current}, {"maximum", gauge.maximum}};
}

inline nlohmann::json RegionTelemetryJson(const luminumbra::simulation::RegionTelemetry& first,
                                          const luminumbra::simulation::RegionTelemetry& replay) {
    using R = luminumbra::simulation::RegionTelemetry;
    const auto& totals = first.Totals();
    nlohmann::json fault = nullptr;
    if (const auto tick = first.FaultTick(); tick)
        fault = {{"tick", *tick}, {"message", first.FaultMessage()}};
    const auto first_tick = first.FirstTick();
    const auto last_tick = first.LastTick();
    nlohmann::json result = {
        {"schema", "luminumbra.sim_budget.regions.v1"},
        {"samples", first.Samples()},
        {"first_tick", first_tick ? nlohmann::json(*first_tick) : nlohmann::json(nullptr)},
        {"last_tick", last_tick ? nlohmann::json(*last_tick) : nlohmann::json(nullptr)},
        {"complete", first.Complete()},
        {"dropped_samples", first.DroppedSamples()},
        {"duration_ms", RegionDurationJson(first.SchedulerDuration())},
        {"scheduler",
         {{"ledger_records", RegionGaugeJson(first.ledger_records)},
          {"active", RegionGaugeJson(first.active)},
          {"reduced", RegionGaugeJson(first.reduced)},
          {"frozen", RegionGaugeJson(first.frozen)},
          {"due_total", first.due_total},
          {"accepted_work_total", first.accepted_work_total},
          {"work_limit", first.work_limit},
          {"pressure_max", first.pressure_max},
          {"rejected_anchors", first.rejected_anchors},
          {"rejected_work", first.rejected_work},
          {"rejected_capacity", first.rejected_capacity}}},
        {"creatures",
         {{"full", first.creatures_full},
          {"coarse", first.creatures_coarse},
          {"frozen", first.creatures_frozen},
          {"promotions", totals[R::Promotions]},
          {"demotions", totals[R::Demotions]},
          {"full_updates", totals[R::FullUpdates]},
          {"coarse_updates", totals[R::CoarseUpdates]},
          {"births", totals[R::Births]},
          {"deaths", totals[R::Deaths]},
          {"removals", totals[R::Removals]},
          {"parked_travel", totals[R::ParkedTravel]}}},
        {"population",
         {{"successes", totals[R::PopulationSuccesses]},
          {"failures", totals[R::PopulationFailures]},
          {"pending_activations", first.pending_activations}}},
        {"controllers",
         {{"created", totals[R::ControllersCreated]},
          {"removed", totals[R::ControllersRemoved]},
          {"stale_handle_rejections", totals[R::StaleHandles]}}},
        {"persistence",
         {{"parked_bytes", RegionGaugeJson(first.parked_bytes)},
          {"saves_total", first.saves_total},
          {"failures_total", first.save_failures_total},
          {"save_samples", first.SaveSamples()},
          {"save_duration_ms", RegionDurationJson(first.SaveDuration())}}},
        {"water",
         {{"reserved", first.water_reserved},
          {"used", first.water_used},
          {"unused", first.water_unused},
          {"unallocated", first.water_unallocated},
          {"completed_cells", first.water_completed_cells},
          {"bound_known", false},
          {"bound", nullptr}}},
        {"replay",
         {{"trace_digest", first.TraceDigest()},
          {"work_replay_match", replay.Enabled() && first.WorkMatches(replay)}}},
        {"fault", std::move(fault)}};
    // Fixed fields, u64 counters and bounded ASCII fault text fit even with indentation.
    // Refuse a future accidental schema expansion before adding it to an artifact.
    if (result.dump(8).size() > R::kMaxBlockBytes)
        throw std::length_error("Regional telemetry block exceeds its byte cap");
    return result;
}

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
    if (first.Regions().Enabled())
        artifact["sim_budget"]["regions"] = RegionTelemetryJson(first.Regions(), replay.Regions());
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
