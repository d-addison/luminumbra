#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace Luminumbra::TestSupport {

using WorkCounts = std::map<std::string, std::uint64_t>;

inline void PeakWork(WorkCounts& peak, const WorkCounts& observed) {
    for (const auto& [name, work] : observed)
        peak[name] = std::max(peak[name], work);
}

inline nlohmann::json WorkBudgetData() {
    std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                        "test/fixtures/deterministic-work-budgets.json");
    const auto data = nlohmann::json::parse(input);
    if (data.at("schema") != "luminumbra.deterministic_work_budgets.v1")
        throw std::runtime_error("Unknown deterministic work budget schema");
    return data.at("limits");
}

inline WorkCounts WorkLimits(const std::string& scenario) {
    WorkCounts limits;
    const auto data = WorkBudgetData();
    for (const auto& [name, value] : data.at(scenario).items()) {
        if (!value.is_number_unsigned())
            throw std::runtime_error("Work limits must be unsigned integers: " + name);
        limits.emplace(name, value.get<std::uint64_t>());
    }
    if (limits.empty())
        throw std::runtime_error("Empty work budget: " + scenario);
    return limits;
}

// Shared by the measured fixtures and injected negative cases. Complete key
// coverage is mandatory; missing measurements cannot silently become zero.
inline ::testing::AssertionResult
CompareWork(const WorkCounts& observed, const WorkCounts& limits, std::ostream& report) {
    bool passed = observed.size() == limits.size();
    for (const auto& [name, limit] : limits) {
        const auto found = observed.find(name);
        report << name << ": observed=";
        if (found == observed.end()) {
            report << "missing";
            passed = false;
        } else {
            report << found->second;
            passed = passed && found->second <= limit;
        }
        report << " limit=" << limit << '\n';
    }
    if (!passed)
        return ::testing::AssertionFailure() << "Deterministic work budget exceeded or incomplete";
    return ::testing::AssertionSuccess();
}

inline ::testing::AssertionResult CheckWork(const WorkCounts& observed,
                                            const std::string& scenario) {
    std::ostringstream report;
    const auto verdict = CompareWork(observed, WorkLimits(scenario), report);
    if (!verdict)
        return ::testing::AssertionFailure() << verdict.message() << '\n' << report.str();
    // Successful native tests retain the same observed/limit evidence in CTest logs.
    ::testing::Test::RecordProperty("work_budget_" + scenario, report.str());
    return ::testing::AssertionSuccess();
}

} // namespace Luminumbra::TestSupport
