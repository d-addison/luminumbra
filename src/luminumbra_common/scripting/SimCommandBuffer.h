#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Luminumbra::scripting {

enum class SimCommandKind : std::uint8_t {
    Spawn,
    Destroy,
    SetComponent,
    EmitEvent,
    DepositField
};

// Runtime-neutral record. Payload encoding and target/key interpretation belong
// to the eventual command consumer; this record is not a save or wire format.
struct SimCommand {
    SimCommandKind kind = SimCommandKind::Spawn;
    std::uint64_t target = 0;
    std::uint64_t key = 0;
    // Each command carries at most 64 bytes; no borrowed script memory survives.
    std::array<std::byte, 64> payload{};
    std::size_t payload_size = 0;
};

enum class SimScriptStatus : std::uint8_t {
    Succeeded,
    RuntimeError,
    BudgetExceeded,
    MemoryExceeded,
    CommandLimit,
    InvalidCommand
};

// A bounded journal, independent of ECS, VM, ticks, persistence and networking.
// Stage failure poisons this call's entire journal. Publication appends one
// complete journal, preserving the destination on every refusal. The future
// simulation consumer must validate and prepare a whole batch before effects.
class SimCommandBuffer {
public:
    // Hard bound per journal: 256 commands, each with at most 64 payload bytes.
    static constexpr std::size_t kMaxCommands = 256;

    [[nodiscard]] bool stage(const SimCommand& command) noexcept;
    [[nodiscard]] bool append_atomic(const SimCommandBuffer& staged) noexcept;
    void discard(SimScriptStatus reason) noexcept;

    [[nodiscard]] SimScriptStatus status() const noexcept {
        return m_status;
    }
    [[nodiscard]] std::span<const SimCommand> commands() const noexcept {
        return {m_commands.data(), m_size};
    }

private:
    std::array<SimCommand, kMaxCommands> m_commands{};
    std::size_t m_size = 0;
    SimScriptStatus m_status = SimScriptStatus::Succeeded;
};

inline bool SimCommandBuffer::stage(const SimCommand& command) noexcept {
    if (m_status != SimScriptStatus::Succeeded) {
        return false;
    }
    if (command.kind > SimCommandKind::DepositField ||
        command.payload_size > command.payload.size()) {
        discard(SimScriptStatus::InvalidCommand);
        return false;
    }
    if (m_size == kMaxCommands) {
        discard(SimScriptStatus::CommandLimit);
        return false;
    }
    auto& stored = m_commands[m_size];
    stored = command;
    // Clear unused payload bytes so caller padding cannot become command data.
    std::fill(stored.payload.begin() + static_cast<std::ptrdiff_t>(stored.payload_size),
              stored.payload.end(),
              std::byte{});
    ++m_size;
    return true;
}

inline bool SimCommandBuffer::append_atomic(const SimCommandBuffer& staged) noexcept {
    if (m_status != SimScriptStatus::Succeeded || staged.status() != SimScriptStatus::Succeeded ||
        staged.m_size > kMaxCommands - m_size) {
        return false;
    }
    // At most 256 trivial record copies; capacity is checked before any write.
    std::copy_n(staged.m_commands.begin(),
                staged.m_size,
                m_commands.begin() + static_cast<std::ptrdiff_t>(m_size));
    m_size += staged.m_size;
    return true;
}

inline void SimCommandBuffer::discard(SimScriptStatus reason) noexcept {
    m_size = 0;
    // A failure is sticky, including when a caller mistakenly reports success.
    if (m_status == SimScriptStatus::Succeeded) {
        m_status = reason == SimScriptStatus::Succeeded ? SimScriptStatus::RuntimeError : reason;
    }
}

} // namespace Luminumbra::scripting
