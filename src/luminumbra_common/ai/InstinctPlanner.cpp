#include "InstinctPlanner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace luminumbra::ai {
namespace {

constexpr const char* kDecisionContract = "deterministic_priority_then_cost";

double Clamp01(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

double Round4(double value) {
    return std::round(value * 10000.0) / 10000.0;
}

double NeedPressure(const std::vector<InstinctNeed>& needs, const std::string& name) {
    for (const auto& need : needs) {
        if (need.name == name) {
            return Clamp01(need.pressure);
        }
    }
    return 0.0;
}

std::string ReasonFor(const InstinctOpportunity& opportunity, double pressure) {
    std::ostringstream out;
    out << "answers " << opportunity.need << " pressure ";
    out << std::fixed << std::setprecision(2) << pressure;
    out << " with " << opportunity.action << " at " << opportunity.target;
    return out.str();
}

std::uint32_t HashText(std::uint32_t hash, const std::string& text) {
    for (const unsigned char ch : text) {
        hash ^= ch;
        hash *= 16777619u;
    }
    return hash;
}

std::string BuildChecksum(const InstinctPlan& plan) {
    std::uint32_t hash = 2166136261u;
    hash = HashText(hash, plan.actor_id);
    hash = HashText(hash, plan.archetype);
    hash = HashText(hash, plan.decision_contract);
    for (const auto& candidate : plan.candidates) {
        hash = HashText(hash, candidate.id);
        hash = HashText(hash, candidate.action);
        hash = HashText(hash, candidate.target);
        hash = HashText(hash, candidate.need);
        hash =
            HashText(hash, std::to_string(static_cast<int>(std::round(candidate.score * 10000.0))));
    }

    std::ostringstream out;
    out << "fnv1a32:";
    out << std::hex << std::setw(8) << std::setfill('0') << hash;
    return out.str();
}

std::string JsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20u) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch);
                    out << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

void JsonString(std::ostringstream& out,
                const std::string& key,
                const std::string& value,
                bool comma = true) {
    out << "\"" << key << "\":\"" << JsonEscape(value) << "\"";
    if (comma) {
        out << ",";
    }
}

void JsonNumber(std::ostringstream& out, const std::string& key, double value, bool comma = true) {
    out << "\"" << key << "\":" << std::fixed << std::setprecision(4) << value;
    if (comma) {
        out << ",";
    }
}

} // namespace

InstinctPlan PlanInstincts(const InstinctPlanRequest& request) {
    InstinctPlan plan;
    plan.actor_id = request.actor_id;
    plan.archetype = request.archetype;
    plan.decision_contract = kDecisionContract;
    plan.candidates.reserve(request.opportunities.size());

    for (const auto& opportunity : request.opportunities) {
        const double pressure = NeedPressure(request.needs, opportunity.need);
        const double score = (pressure * Clamp01(opportunity.satisfaction) * 2.0) +
                             (Clamp01(opportunity.urgency) * 1.5) -
                             (Clamp01(opportunity.risk) * 1.2) - (opportunity.distance * 0.35) -
                             (opportunity.stamina_cost * 0.4);

        InstinctCandidate candidate;
        candidate.id = opportunity.id;
        candidate.action = opportunity.action;
        candidate.target = opportunity.target;
        candidate.need = opportunity.need;
        candidate.score = Round4(score);
        candidate.need_pressure = Round4(pressure);
        candidate.reason = ReasonFor(opportunity, pressure);
        plan.candidates.push_back(candidate);
    }

    std::stable_sort(
        plan.candidates.begin(), plan.candidates.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }
            if (lhs.need != rhs.need) {
                return lhs.need < rhs.need;
            }
            if (lhs.action != rhs.action) {
                return lhs.action < rhs.action;
            }
            return lhs.target < rhs.target;
        });

    for (std::size_t index = 0; index < plan.candidates.size(); ++index) {
        plan.candidates[index].rank = static_cast<int>(index + 1u);
    }

    plan.selected_index = plan.candidates.empty() ? -1 : 0;
    plan.checksum = BuildChecksum(plan);
    // Generic pass semantics: a non-empty deterministic ranking
    // with the top candidate selected. Content-specific expectations live in
    // the game-data `expected` block consumed by the gate.
    plan.passed = plan.selected_index == 0 && !plan.candidates.empty();
    return plan;
}

std::string SerializeInstinctPlanJson(const InstinctPlan& plan, const std::string& build_preset) {
    const InstinctCandidate* selected = nullptr;
    if (plan.selected_index >= 0 &&
        static_cast<std::size_t>(plan.selected_index) < plan.candidates.size()) {
        selected = &plan.candidates[static_cast<std::size_t>(plan.selected_index)];
    }

    std::ostringstream out;
    out << "{";
    JsonString(out, "schema", "luminumbra.ai.instinct_planner.v1");
    out << "\"passed\":" << (plan.passed ? "true" : "false") << ",";
    JsonString(out, "build_preset", build_preset);
    out << "\"planner\":{";
    JsonString(out, "source", "src/luminumbra_common/ai/InstinctPlanner.cpp");
    JsonString(out, "header", "src/luminumbra_common/ai/InstinctPlanner.h");
    JsonString(out, "serializer", "SerializeInstinctPlanJson");
    JsonString(out, "validation_api", "InstinctPlannerMeetsBaseline");
    JsonString(out, "decision_contract", plan.decision_contract, false);
    out << "},";
    out << "\"fixture\":{";
    JsonString(out, "actor_id", plan.actor_id);
    JsonString(out, "archetype", plan.archetype);
    JsonString(out, "dominant_need", selected == nullptr ? "" : selected->need);
    JsonString(out, "selected_action", selected == nullptr ? "" : selected->action);
    JsonString(out, "selected_target", selected == nullptr ? "" : selected->target);
    JsonNumber(out, "selected_score", selected == nullptr ? 0.0 : selected->score);
    out << "\"candidate_count\":" << plan.candidates.size() << ",";
    JsonString(out, "checksum", plan.checksum, false);
    out << "},";
    out << "\"candidates\":[";
    for (std::size_t index = 0; index < plan.candidates.size(); ++index) {
        const auto& candidate = plan.candidates[index];
        out << "{";
        out << "\"rank\":" << candidate.rank << ",";
        JsonString(out, "id", candidate.id);
        JsonString(out, "need", candidate.need);
        JsonString(out, "action", candidate.action);
        JsonString(out, "target", candidate.target);
        JsonNumber(out, "score", candidate.score);
        JsonNumber(out, "need_pressure", candidate.need_pressure);
        JsonString(out, "reason", candidate.reason, false);
        out << "}";
        if (index + 1u < plan.candidates.size()) {
            out << ",";
        }
    }
    out << "]";
    out << "}";
    return out.str();
}

} // namespace luminumbra::ai
