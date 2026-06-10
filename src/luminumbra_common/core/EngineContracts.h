#pragma once

#include "JobSystem.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Luminumbra::Contracts {

enum class LifecycleState {
    Uninitialized,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

std::string_view ToString(LifecycleState state);

struct ContractResult {
    bool ok = true;
    std::vector<std::string> errors;

    void add_error(std::string error);
};

struct SubsystemContract {
    std::string name;
    std::string owner;
    std::vector<std::string> depends_on;
};

class LifecycleGraph {
public:
    explicit LifecycleGraph(std::vector<SubsystemContract> contracts);

    ContractResult validate_startup_order(const std::vector<std::string>& order) const;
    ContractResult validate_shutdown_order(const std::vector<std::string>& order) const;

private:
    std::vector<SubsystemContract> m_contracts;
    std::unordered_map<std::string, SubsystemContract> m_by_name;
};

std::vector<SubsystemContract> KnownRuntimeSubsystemContracts();

class LifecycleTracker {
public:
    explicit LifecycleTracker(std::string name);

    bool mark_starting();
    bool mark_running();
    bool mark_stopping();
    bool mark_stopped();
    void mark_failed(std::string reason);

    LifecycleState state() const { return m_state; }
    bool ready() const { return m_state == LifecycleState::Running; }
    const std::string& name() const { return m_name; }
    const std::string& failure_reason() const { return m_failure_reason; }

private:
    bool transition(LifecycleState expected, LifecycleState next);

    std::string m_name;
    LifecycleState m_state = LifecycleState::Uninitialized;
    std::string m_failure_reason;
};

int PendingJobCount(const JobHandle& handle);
bool IsJobComplete(const JobHandle& handle);

class OwnedJobTracker {
public:
    explicit OwnedJobTracker(std::string owner);

    void track(JobHandle handle);
    void prune_completed();
    void drain(JobSystem& job_system);
    std::size_t outstanding_count() const;
    const std::string& owner() const { return m_owner; }

private:
    std::string m_owner;
    std::vector<JobHandle> m_handles;
};

struct ResourceHandle {
    static constexpr std::uint32_t InvalidSlot = UINT32_MAX;

    std::uint32_t slot = InvalidSlot;
    std::uint32_t generation = 0;

    bool valid_shape() const { return slot != InvalidSlot && generation != 0; }
};

class ResourceGenerationTable {
public:
    ResourceHandle create();
    bool destroy(ResourceHandle handle);
    bool is_valid(ResourceHandle handle) const;
    std::size_t active_count() const { return m_active_count; }

private:
    struct Slot {
        std::uint32_t generation = 1;
        bool active = false;
    };

    std::vector<Slot> m_slots;
    std::vector<std::uint32_t> m_free_slots;
    std::size_t m_active_count = 0;
};

} // namespace Luminumbra::Contracts
