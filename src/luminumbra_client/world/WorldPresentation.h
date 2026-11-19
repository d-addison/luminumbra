#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Luminumbra::Client {

// Presentation-only deadlines delay entry, never simulation time or authority.
inline constexpr unsigned kWorldPresentationFrameDeadline = 1800;
inline constexpr auto kWorldPresentationWallDeadline = std::chrono::seconds(120);
inline constexpr unsigned kWorldPresentationMinimumFrames = 3;

// A published empty mesh is complete. Vertex counts cannot distinguish it from
// unfinished terrain; Ready state and a known LOD can.
constexpr bool NearTerrainPrepared(std::size_t expected,
                                   std::size_t ready,
                                   std::size_t pending,
                                   std::size_t unknown_lod) {
    return expected > 0 && ready == expected && pending == 0 && unknown_lod == 0;
}

constexpr bool InitialWaterPrepared(bool prepared, std::size_t deferred_uploads) {
    return prepared && deferred_uploads == 0;
}

struct WorldPresentationReadiness {
    bool terrain = false;
    bool horizon = false;
    bool water = false;
    bool foliage = false;
    bool uploads = false;
    bool streaming = false;

    [[nodiscard]] constexpr std::uint32_t missing() const {
        return (terrain ? 0u : 1u) | (horizon ? 0u : 2u) | (water ? 0u : 4u) | (foliage ? 0u : 8u) |
               (uploads ? 0u : 16u) | (streaming ? 0u : 32u);
    }
    [[nodiscard]] constexpr bool ready() const {
        return missing() == 0;
    }
};

enum class WorldPresentationState {
    Waiting,
    Ready,
    TimedOut
};

// Clock-free policy: the owner supplies elapsed steady-clock time and one
// observation per rendered frame. A new view shares the original deadline;
// timeout latches until world replacement.
class WorldPresentationGate {
public:
    constexpr WorldPresentationState observe(const WorldPresentationReadiness& readiness,
                                             std::chrono::steady_clock::duration elapsed) {
        if (m_state != WorldPresentationState::Waiting) {
            return m_state;
        }
        ++m_frames;
        ++m_view_frames;
        m_missing = readiness.missing();
        if (readiness.ready() && m_view_frames >= kWorldPresentationMinimumFrames) {
            m_state = WorldPresentationState::Ready;
        } else if (m_frames >= kWorldPresentationFrameDeadline ||
                   elapsed >= kWorldPresentationWallDeadline) {
            m_state = WorldPresentationState::TimedOut;
        }
        return m_state;
    }
    constexpr void restart_for_view() {
        if (!timed_out()) {
            m_state = m_frames < kWorldPresentationFrameDeadline ? WorldPresentationState::Waiting
                                                                 : WorldPresentationState::TimedOut;
            m_view_frames = 0;
            m_missing = 0;
        }
    }
    [[nodiscard]] constexpr bool waiting() const {
        return m_state == WorldPresentationState::Waiting;
    }
    [[nodiscard]] constexpr bool timed_out() const {
        return m_state == WorldPresentationState::TimedOut;
    }
    // Automated readiness failures survive presentation deadlines. Normal play
    // can proceed on a presentation timeout without declaring automation ready.
    [[nodiscard]] constexpr bool permits_entry(bool automated, bool runtime_ready) const {
        return !waiting() && (!automated || (!timed_out() && runtime_ready));
    }
    [[nodiscard]] std::string missing_parts() const {
        constexpr std::array names{
            "terrain", "horizon", "water", "foliage", "uploads", "streaming"};
        std::string result;
        // Six fixed parts; allocate diagnostics only when requested on timeout.
        for (unsigned i = 0; i < names.size(); ++i) {
            if ((m_missing & (1u << i)) != 0) {
                if (!result.empty()) {
                    result += ',';
                }
                result += names[i];
            }
        }
        return result.empty() ? "minimum_frames" : result;
    }

private:
    unsigned m_frames = 0;
    unsigned m_view_frames = 0;
    std::uint32_t m_missing = 0;
    WorldPresentationState m_state = WorldPresentationState::Waiting;
};

} // namespace Luminumbra::Client
