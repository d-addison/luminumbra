#include "PhaseScheduler.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace Luminumbra {

bool IsScheduleIdentifier(std::string_view id) {
    return !id.empty() && id.size() <= 128 &&
           std::all_of(id.begin(), id.end(), [](unsigned char byte) {
               return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                      (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.' ||
                      byte == ':';
           });
}

void ExecutionTrace::append_byte(std::uint8_t byte) noexcept {
    m_hash ^= byte;
    m_hash *= 1099511628211ULL;
}

void ExecutionTrace::append(std::string_view text) noexcept {
    const auto size = static_cast<std::uint64_t>(text.size());
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        append_byte(static_cast<std::uint8_t>((size >> shift) & 0xffU));
    }
    for (const unsigned char byte : text) {
        append_byte(byte);
    }
}

void ExecutionTrace::record(std::string_view event,
                            std::string_view phase,
                            std::string_view id) noexcept {
    append(event);
    append(phase);
    append(id);
}

ScheduleResult PhaseScheduler::compile(const std::vector<std::string>& phases,
                                       const std::vector<PhaseTask>& tasks) {
    if (m_executing) {
        return ScheduleResult::Busy;
    }
    if (phases.size() > MaxPhases || tasks.size() > MaxTasks) {
        return ScheduleResult::LimitExceeded;
    }
    std::map<std::string, std::size_t> phase_indices;
    for (std::size_t index = 0; index < phases.size(); ++index) {
        if (!IsScheduleIdentifier(phases[index])) {
            return ScheduleResult::InvalidIdentifier;
        }
        if (!phase_indices.emplace(phases[index], index).second) {
            return ScheduleResult::DuplicateId;
        }
    }
    std::map<std::string, std::size_t> task_indices;
    std::vector<std::size_t> task_phases;
    task_phases.reserve(tasks.size());
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        const auto& task = tasks[index];
        if (!IsScheduleIdentifier(task.id) || !IsScheduleIdentifier(task.phase)) {
            return ScheduleResult::InvalidIdentifier;
        }
        if (task.after.size() > MaxDependencies) {
            return ScheduleResult::LimitExceeded;
        }
        if (!task_indices.emplace(task.id, index).second) {
            return ScheduleResult::DuplicateId;
        }
        const auto phase = phase_indices.find(task.phase);
        if (phase == phase_indices.end()) {
            return ScheduleResult::UnknownPhase;
        }
        task_phases.push_back(phase->second);
    }
    std::vector<std::vector<std::size_t>> dependents(tasks.size());
    std::vector<std::size_t> pending(tasks.size(), 0);
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        for (const auto& dependency : tasks[index].after) {
            if (!IsScheduleIdentifier(dependency)) {
                return ScheduleResult::InvalidIdentifier;
            }
            const auto found = task_indices.find(dependency);
            if (found == task_indices.end()) {
                return ScheduleResult::MissingDependency;
            }
            const auto predecessor = found->second;
            if (task_phases[predecessor] > task_phases[index]) {
                return ScheduleResult::LaterPhaseDependency;
            }
            if (task_phases[predecessor] == task_phases[index]) {
                dependents[predecessor].push_back(index);
                ++pending[index];
            }
        }
    }
    // At most MaxTasks vertices and MaxTasks * MaxDependencies edges; no recursion.
    std::set<std::pair<std::size_t, std::string>> ready;
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        if (pending[index] == 0) {
            ready.emplace(task_phases[index], tasks[index].id);
        }
    }
    std::vector<std::vector<std::string>> order(phases.size());
    std::size_t completed = 0;
    while (!ready.empty()) {
        auto next = ready.extract(ready.begin()).value();
        const auto index = task_indices.at(next.second);
        order[next.first].push_back(std::move(next.second));
        ++completed;
        for (const auto dependent : dependents[index]) {
            --pending[dependent];
            if (pending[dependent] == 0) {
                ready.emplace(task_phases[dependent], tasks[dependent].id);
            }
        }
    }
    if (completed != tasks.size()) {
        return ScheduleResult::Cycle;
    }
    auto phase_names = phases;
    m_phases.swap(phase_names);
    m_order.swap(order);
    return ScheduleResult::Ok;
}

} // namespace Luminumbra
