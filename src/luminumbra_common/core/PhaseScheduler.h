#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra {

// Identifiers use ASCII letters, digits, '_', '-', '.', ':' and at most 128 bytes.
bool IsScheduleIdentifier(std::string_view id);

enum class ScheduleResult {
    Ok,
    Busy,
    LimitExceeded,
    InvalidIdentifier,
    DuplicateId,
    UnknownPhase,
    MissingDependency,
    LaterPhaseDependency,
    Cycle,
};

struct PhaseTask {
    std::string id;
    std::string phase;
    std::vector<std::string> after;
};

// Diagnostic trace only: FNV-1a-64 over length-prefixed event/phase/id bytes.
// It is independent of authority, save and replay identities.
class ExecutionTrace {
public:
    void record(std::string_view event, std::string_view phase, std::string_view id) noexcept;
    std::uint64_t hash() const noexcept {
        return m_hash;
    }

private:
    void append(std::string_view text) noexcept;
    void append_byte(std::uint8_t byte) noexcept;
    std::uint64_t m_hash = 14695981039346656037ULL;
};

// Host-thread only. Phase order is explicit; ready tasks use ascending ASCII id.
// Compilation is transactional. Execution allocates nothing and visits only the
// selected phase's tasks; a typed visitor supplies the actual system dispatch.
class PhaseScheduler {
public:
    PhaseScheduler() = default;
    PhaseScheduler(const PhaseScheduler&) = delete;
    PhaseScheduler& operator=(const PhaseScheduler&) = delete;
    PhaseScheduler(PhaseScheduler&&) = delete;
    PhaseScheduler& operator=(PhaseScheduler&&) = delete;

    // Bounds cover all stored phases, tasks, dependency edges and identifier bytes.
    static constexpr std::size_t MaxPhases = 64;
    static constexpr std::size_t MaxTasks = 1024;
    static constexpr std::size_t MaxDependencies = 256;

    ScheduleResult compile(const std::vector<std::string>& phases,
                           const std::vector<PhaseTask>& tasks);

    // Const diagnostic; references remain valid until the next successful compile.
    const std::vector<std::vector<std::string>>& ordered_ids() const {
        return m_order;
    }

    template<typename Visitor>
    ScheduleResult execute(std::size_t phase_index, Visitor&& visitor) {
        if (m_executing) {
            return ScheduleResult::Busy;
        }
        if (phase_index >= m_order.size()) {
            return ScheduleResult::UnknownPhase;
        }
        struct ExecutionGuard {
            bool& active;
            explicit ExecutionGuard(bool& flag)
                : active(flag) {
                active = true;
            }
            ~ExecutionGuard() {
                active = false;
            }
        } guard(m_executing);
        for (const auto& id : m_order[phase_index]) {
            m_trace.record("phase", m_phases[phase_index], id);
            visitor(std::string_view(id));
        }
        return ScheduleResult::Ok;
    }

    std::uint64_t execution_trace_hash() const noexcept {
        return m_trace.hash();
    }

private:
    std::vector<std::string> m_phases;
    std::vector<std::vector<std::string>> m_order;
    ExecutionTrace m_trace;
    bool m_executing = false;
};

} // namespace Luminumbra
