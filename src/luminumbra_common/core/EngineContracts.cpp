#include "EngineContracts.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace Luminumbra::Contracts {

std::string_view ToString(LifecycleState state) {
    switch (state) {
        case LifecycleState::Uninitialized:
            return "uninitialized";
        case LifecycleState::Starting:
            return "starting";
        case LifecycleState::Running:
            return "running";
        case LifecycleState::Stopping:
            return "stopping";
        case LifecycleState::Stopped:
            return "stopped";
        case LifecycleState::Failed:
            return "failed";
    }
    return "unknown";
}

void ContractResult::add_error(std::string error) {
    ok = false;
    errors.push_back(std::move(error));
}

LifecycleGraph::LifecycleGraph(std::vector<SubsystemContract> contracts)
    : m_contracts(std::move(contracts)) {
    for (const auto& contract : m_contracts) {
        m_by_name.emplace(contract.name, contract);
    }
}

ContractResult LifecycleGraph::validate_startup_order(const std::vector<std::string>& order) const {
    ContractResult result;
    std::unordered_set<std::string> seen;

    for (const std::string& name : order) {
        auto contract_it = m_by_name.find(name);
        if (contract_it == m_by_name.end()) {
            result.add_error("unknown subsystem in startup order: " + name);
            continue;
        }

        if (!seen.insert(name).second) {
            result.add_error("duplicate subsystem in startup order: " + name);
            continue;
        }

        for (const std::string& dependency : contract_it->second.depends_on) {
            if (seen.find(dependency) == seen.end()) {
                result.add_error(name + " starts before dependency " + dependency);
            }
        }
    }

    for (const auto& contract : m_contracts) {
        if (seen.find(contract.name) == seen.end()) {
            result.add_error("missing subsystem in startup order: " + contract.name);
        }
    }

    return result;
}

ContractResult
LifecycleGraph::validate_shutdown_order(const std::vector<std::string>& order) const {
    ContractResult result;
    std::unordered_set<std::string> stopped;
    std::unordered_set<std::string> all_names;

    for (const auto& contract : m_contracts) {
        all_names.insert(contract.name);
    }

    for (const std::string& name : order) {
        if (all_names.find(name) == all_names.end()) {
            result.add_error("unknown subsystem in shutdown order: " + name);
            continue;
        }

        if (!stopped.insert(name).second) {
            result.add_error("duplicate subsystem in shutdown order: " + name);
            continue;
        }

        for (const auto& contract : m_contracts) {
            if (std::find(contract.depends_on.begin(), contract.depends_on.end(), name) !=
                    contract.depends_on.end() &&
                stopped.find(contract.name) == stopped.end()) {
                result.add_error(name + " shuts down before dependent " + contract.name);
            }
        }
    }

    for (const auto& contract : m_contracts) {
        if (stopped.find(contract.name) == stopped.end()) {
            result.add_error("missing subsystem in shutdown order: " + contract.name);
        }
    }

    return result;
}

std::vector<SubsystemContract> KnownRuntimeSubsystemContracts() {
    return {
        {"job_system", "runtime", {}},
        {"physics_system", "game_session", {}},
        {"world_system", "game_session", {"job_system", "physics_system"}},
        {"water_system", "game_session", {"job_system", "world_system"}},
        // instinct planning runs on the fixed simulation tick after
        // the world systems exist (NeedsComponent migration landed with this
        // contract entry).
        {"instinct_system", "game_session", {"world_system"}},
        {"renderer", "client", {"world_system"}},
        {"audio", "client", {}},
        {"ui", "client", {"renderer"}},
        {"gameplay", "client", {"world_system", "physics_system", "renderer"}},
    };
}

LifecycleTracker::LifecycleTracker(std::string name)
    : m_name(std::move(name)) {}

bool LifecycleTracker::mark_starting() {
    return transition(LifecycleState::Uninitialized, LifecycleState::Starting) ||
           transition(LifecycleState::Stopped, LifecycleState::Starting);
}

bool LifecycleTracker::mark_running() {
    return transition(LifecycleState::Starting, LifecycleState::Running);
}

bool LifecycleTracker::mark_stopping() {
    return transition(LifecycleState::Running, LifecycleState::Stopping);
}

bool LifecycleTracker::mark_stopped() {
    return transition(LifecycleState::Stopping, LifecycleState::Stopped);
}

void LifecycleTracker::mark_failed(std::string reason) {
    m_state = LifecycleState::Failed;
    m_failure_reason = std::move(reason);
}

bool LifecycleTracker::transition(LifecycleState expected, LifecycleState next) {
    if (m_state != expected) {
        std::ostringstream message;
        message << m_name << " cannot transition from " << ToString(m_state) << " to "
                << ToString(next) << "; expected " << ToString(expected);
        m_failure_reason = message.str();
        return false;
    }

    m_state = next;
    m_failure_reason.clear();
    return true;
}

int PendingJobCount(const JobHandle& handle) {
    if (!handle.counter) {
        return 0;
    }
    const int pending = handle.counter->load(std::memory_order_acquire);
    return pending > 0 ? pending : 0;
}

bool IsJobComplete(const JobHandle& handle) {
    return PendingJobCount(handle) == 0;
}

OwnedJobTracker::OwnedJobTracker(std::string owner)
    : m_owner(std::move(owner)) {}

void OwnedJobTracker::track(JobHandle handle) {
    if (handle.counter || handle.completion) {
        m_handles.push_back(std::move(handle));
    }
}

void OwnedJobTracker::prune_completed() {
    m_handles.erase(std::remove_if(m_handles.begin(),
                                   m_handles.end(),
                                   [](const JobHandle& handle) { return IsJobComplete(handle); }),
                    m_handles.end());
}

void OwnedJobTracker::drain(JobSystem& job_system) {
    for (const JobHandle& handle : m_handles) {
        job_system.wait(handle);
    }
    prune_completed();
}

std::size_t OwnedJobTracker::outstanding_count() const {
    return static_cast<std::size_t>(
        std::count_if(m_handles.begin(), m_handles.end(), [](const JobHandle& handle) {
            return !IsJobComplete(handle);
        }));
}

ResourceHandle ResourceGenerationTable::create() {
    std::uint32_t slot_index = ResourceHandle::InvalidSlot;

    if (!m_free_slots.empty()) {
        slot_index = m_free_slots.back();
        m_free_slots.pop_back();
    } else {
        slot_index = static_cast<std::uint32_t>(m_slots.size());
        m_slots.push_back({});
    }

    Slot& slot = m_slots[slot_index];
    slot.active = true;
    ++m_active_count;
    return ResourceHandle{slot_index, slot.generation};
}

bool ResourceGenerationTable::destroy(ResourceHandle handle) {
    if (!is_valid(handle)) {
        return false;
    }

    Slot& slot = m_slots[handle.slot];
    slot.active = false;
    slot.generation =
        slot.generation == std::numeric_limits<std::uint32_t>::max() ? 1 : slot.generation + 1;
    m_free_slots.push_back(handle.slot);
    --m_active_count;
    return true;
}

bool ResourceGenerationTable::is_valid(ResourceHandle handle) const {
    if (!handle.valid_shape()) {
        return false;
    }
    if (handle.slot >= m_slots.size()) {
        return false;
    }

    const Slot& slot = m_slots[handle.slot];
    return slot.active && slot.generation == handle.generation;
}

} // namespace Luminumbra::Contracts
