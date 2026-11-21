#pragma once

#include "SimCommandBuffer.h"

#include <cstdint>
#include <new>
#include <utility>

namespace Luminumbra::scripting {

// A runtime adapter receives only this call-local staging and fuel API.
// It must meter interpreter work through consume(); this host does not execute
// or interrupt a VM. Each attempted command also consumes one unit of fuel.
class SimScriptCall {
public:
    explicit SimScriptCall(std::uint64_t fuel) noexcept
        : m_fuel(fuel) {}
    SimScriptCall(const SimScriptCall&) = delete;
    SimScriptCall& operator=(const SimScriptCall&) = delete;

    [[nodiscard]] bool consume(std::uint64_t units) noexcept {
        if (m_staged.status() != SimScriptStatus::Succeeded) {
            return false;
        }
        if (units > m_fuel) {
            m_staged.discard(SimScriptStatus::BudgetExceeded);
            return false;
        }
        m_fuel -= units;
        return true;
    }
    [[nodiscard]] bool stage(const SimCommand& command) noexcept {
        return consume(1) && m_staged.stage(command);
    }
    [[nodiscard]] std::uint64_t remaining_fuel() const noexcept {
        return m_fuel;
    }
    [[nodiscard]] const SimCommandBuffer& staged() const noexcept {
        return m_staged;
    }

private:
    SimCommandBuffer m_staged;
    std::uint64_t m_fuel;
};

class SimScriptHost {
public:
    // Publish into a caller-owned committed journal only after successful return.
    // No callback exceptions escape and no destination storage is allocated.
    // Runtime adapters must report caught VM errors/OOM as the matching status.
    template<typename Callback>
    [[nodiscard]] static SimScriptStatus
    Run(Callback&& callback, std::uint64_t fuel, SimCommandBuffer& committed) noexcept {
        SimScriptCall call(fuel);
        SimScriptStatus result = SimScriptStatus::RuntimeError;
        try {
            result = std::forward<Callback>(callback)(call);
        } catch (const std::bad_alloc&) {
            return SimScriptStatus::MemoryExceeded;
        } catch (...) {
            return SimScriptStatus::RuntimeError;
        }
        if (call.staged().status() != SimScriptStatus::Succeeded) {
            return call.staged().status();
        }
        if (result != SimScriptStatus::Succeeded) {
            return result;
        }
        if (committed.status() != SimScriptStatus::Succeeded) {
            return committed.status();
        }
        return committed.append_atomic(call.staged()) ? SimScriptStatus::Succeeded
                                                      : SimScriptStatus::CommandLimit;
    }
};

} // namespace Luminumbra::scripting
