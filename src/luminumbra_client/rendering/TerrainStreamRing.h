#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>

namespace Luminumbra::Rendering {

// Fixed allocator for independent {indirect, origin} buffer pairs. Each allocation
// starts at byte offset zero in its own pair, so command/baseInstance bytes stay
// unchanged. Bound: four shadow cascades + G-buffer, over three frames in flight.
// More delayed GPU work is handled by orphaning, never by waiting or growing the ring.
class TerrainStreamRing {
public:
    static constexpr std::size_t kSubmissionsPerFrame = 5;
    static constexpr std::size_t kFramesInFlight = 3;
    static constexpr std::size_t kSlotCount = kSubmissionsPerFrame * kFramesInFlight;
    static constexpr std::size_t kMaxCommands = 65536; // live terrain pool allocation bound

    struct Allocation {
        std::size_t slot = 0;
        bool orphan = false;
    };
    struct Diagnostics {
        std::size_t submissions = 0;
        std::size_t same_frame_slot_reuses = 0;
        std::size_t orphanings = 0;
    };

    void begin_frame() noexcept {
        m_frame_used.fill(false);
        m_diagnostics = {};
    }

    std::size_t cursor() const noexcept {
        return m_cursor;
    }

    // Replays restore scheduling only. GPU ownership must NEVER be rolled back.
    void restore_cursor(std::size_t saved_cursor) noexcept {
        m_cursor = saved_cursor % kSlotCount;
    }

    // Completion must cover the previous consumer of this pair. Missing fences,
    // timeout and failed polls all pass false, requiring replacement storage.
    Allocation acquire(bool completion_signalled, bool replay = false) noexcept {
        const Allocation allocation{.slot = m_cursor,
                                    .orphan = m_live[m_cursor] && !completion_signalled};
        if (!replay) {
            ++m_diagnostics.submissions;
            m_diagnostics.same_frame_slot_reuses += m_frame_used[m_cursor] ? 1u : 0u;
            m_diagnostics.orphanings += allocation.orphan ? 1u : 0u;
            m_frame_used[m_cursor] = true;
        }
        m_live[m_cursor] = true;
        m_cursor = (m_cursor + 1u) % kSlotCount;
        return allocation;
    }

    const Diagnostics& diagnostics() const noexcept {
        return m_diagnostics;
    }

    static std::size_t capacity(std::size_t commands) {
        if (commands > kMaxCommands)
            throw std::length_error("Terrain stream command limit exceeded");
        const std::size_t base = std::max<std::size_t>(commands, 256u);
        return base + std::min(base / 2u, kMaxCommands - base);
    }

    static std::size_t
    storage_bytes(std::size_t commands, std::size_t stride, std::size_t byte_limit) {
        if (commands > kMaxCommands || stride == 0 || commands > byte_limit / stride)
            throw std::length_error("Terrain stream storage limit exceeded");
        return commands * stride;
    }

private:
    std::size_t m_cursor = 0;
    std::array<bool, kSlotCount> m_live{};
    std::array<bool, kSlotCount> m_frame_used{};
    Diagnostics m_diagnostics{};
};

} // namespace Luminumbra::Rendering
