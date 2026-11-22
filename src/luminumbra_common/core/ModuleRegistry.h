#pragma once

#include "PhaseScheduler.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra {

struct ModuleDescriptor {
    std::string id;
    std::vector<std::string> provides;
    std::vector<std::string> requires_services;
    // Optional hooks; context is borrowed and must outlive the registry.
    // A false startup result must release that hook's own partial resources.
    bool (*startup)(void*) noexcept = nullptr;
    void (*shutdown)(void*) noexcept = nullptr;
    void* context = nullptr;
};

enum class ModuleResult {
    Ok,
    Busy,
    LimitExceeded,
    InvalidIdentifier,
    DuplicateId,
    DuplicateService,
    MissingService,
    Cycle,
    StartupFailed,
};

// Host-thread only. Single ownership: copying/moving a live registry is forbidden.
// No services are instantiated here; declarations establish lifecycle ordering.
class ModuleRegistry {
public:
    static constexpr std::size_t MaxModules = PhaseScheduler::MaxTasks;
    // Each module declares at most 256 provided and 256 required services.
    static constexpr std::size_t MaxServices = 256;

    ModuleRegistry() = default;
    ~ModuleRegistry();
    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;
    ModuleRegistry(ModuleRegistry&&) = delete;
    ModuleRegistry& operator=(ModuleRegistry&&) = delete;

    ModuleResult register_module(const ModuleDescriptor& module);
    // Validate the entire graph before any hook. On hook failure, shut down only
    // successfully started modules, in reverse order. Repeated start/stop is safe.
    ModuleResult startup();
    ModuleResult shutdown() noexcept;
    // Exact registered identity; callers requiring live services also check running().
    [[nodiscard]] bool contains(std::string_view id) const noexcept {
        for (const auto& module : m_modules) {
            if (module.id == id) {
                return true;
            }
        }
        return false;
    }
    bool running() const noexcept {
        return m_state == State::Running;
    }
    std::size_t size() const noexcept {
        return m_modules.size();
    }
    std::uint64_t execution_trace_hash() const noexcept {
        return m_trace.hash();
    }

private:
    enum class State {
        Ready,
        Starting,
        Running,
        Stopping
    };
    ModuleResult build_order(std::vector<std::size_t>& order) const;
    void stop_started() noexcept;

    std::vector<ModuleDescriptor> m_modules;
    std::vector<std::size_t> m_started;
    ExecutionTrace m_trace;
    State m_state = State::Ready;
};

} // namespace Luminumbra
