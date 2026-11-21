#include "ModuleRegistry.h"

#include "EngineContracts.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace Luminumbra {

ModuleRegistry::~ModuleRegistry() {
    shutdown();
}

ModuleResult ModuleRegistry::register_module(const ModuleDescriptor& module) {
    if (m_state != State::Ready) {
        return ModuleResult::Busy;
    }
    if (m_modules.size() >= MaxModules || module.provides.size() > MaxServices ||
        module.requires_services.size() > MaxServices) {
        return ModuleResult::LimitExceeded;
    }
    if (!IsScheduleIdentifier(module.id)) {
        return ModuleResult::InvalidIdentifier;
    }
    if (std::any_of(m_modules.begin(), m_modules.end(), [&](const auto& entry) {
            return entry.id == module.id;
        })) {
        return ModuleResult::DuplicateId;
    }
    std::set<std::string> provided;
    for (const auto& service : module.provides) {
        if (!IsScheduleIdentifier(service)) {
            return ModuleResult::InvalidIdentifier;
        }
        if (!provided.insert(service).second) {
            return ModuleResult::DuplicateService;
        }
    }
    for (const auto& service : module.requires_services) {
        if (!IsScheduleIdentifier(service)) {
            return ModuleResult::InvalidIdentifier;
        }
    }
    for (const auto& entry : m_modules) {
        for (const auto& service : entry.provides) {
            if (provided.contains(service)) {
                return ModuleResult::DuplicateService;
            }
        }
    }
    m_modules.push_back(module);
    return ModuleResult::Ok;
}

ModuleResult ModuleRegistry::build_order(std::vector<std::size_t>& order) const {
    std::map<std::string, std::size_t> providers;
    std::map<std::string, std::size_t> module_indices;
    for (std::size_t index = 0; index < m_modules.size(); ++index) {
        const auto& module = m_modules[index];
        module_indices.emplace(module.id, index);
        for (const auto& service : module.provides) {
            providers.emplace(service, index);
        }
    }
    std::vector<PhaseTask> tasks;
    std::vector<Contracts::SubsystemContract> contracts;
    for (const auto& module : m_modules) {
        std::set<std::string> dependencies;
        for (const auto& required : module.requires_services) {
            const auto provider = providers.find(required);
            if (provider == providers.end()) {
                return ModuleResult::MissingService;
            }
            dependencies.insert(m_modules[provider->second].id);
        }
        std::vector<std::string> after(dependencies.begin(), dependencies.end());
        contracts.push_back({module.id, "module_registry", after});
        tasks.push_back({module.id, "Startup", std::move(after)});
    }
    PhaseScheduler scheduler;
    if (scheduler.compile({"Startup"}, tasks) != ScheduleResult::Ok) {
        return ModuleResult::Cycle;
    }
    const auto& ids = scheduler.ordered_ids().front();
    const Contracts::LifecycleGraph graph(std::move(contracts));
    if (!graph.validate_startup_order(ids).ok) {
        return ModuleResult::Cycle;
    }
    order.reserve(ids.size());
    for (const auto& id : ids) {
        order.push_back(module_indices.at(id));
    }
    return ModuleResult::Ok;
}

ModuleResult ModuleRegistry::startup() {
    if (m_state == State::Running) {
        return ModuleResult::Ok;
    }
    if (m_state != State::Ready) {
        return ModuleResult::Busy;
    }
    std::vector<std::size_t> order;
    const auto result = build_order(order);
    if (result != ModuleResult::Ok) {
        return result;
    }
    // Allocate all bookkeeping before invoking hooks; no allocation can interrupt rollback.
    m_started.reserve(order.size());
    m_state = State::Starting;
    for (const auto index : order) {
        const auto& module = m_modules[index];
        m_trace.record("startup", "Startup", module.id);
        if (module.startup != nullptr && !module.startup(module.context)) {
            stop_started();
            return ModuleResult::StartupFailed;
        }
        m_started.push_back(index);
    }
    m_state = State::Running;
    return ModuleResult::Ok;
}

void ModuleRegistry::stop_started() noexcept {
    m_state = State::Stopping;
    while (!m_started.empty()) {
        const auto& module = m_modules[m_started.back()];
        m_trace.record("shutdown", "Shutdown", module.id);
        if (module.shutdown != nullptr) {
            module.shutdown(module.context);
        }
        m_started.pop_back();
    }
    m_state = State::Ready;
}

ModuleResult ModuleRegistry::shutdown() noexcept {
    if (m_state == State::Ready) {
        return ModuleResult::Ok;
    }
    if (m_state != State::Running) {
        return ModuleResult::Busy;
    }
    stop_started();
    return ModuleResult::Ok;
}

} // namespace Luminumbra
