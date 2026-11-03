#pragma once

#include <cstddef>
#include <optional>
#include <utility>

namespace Luminumbra::Client {

enum class WorldEntryStage {
    Validate,
    DrainReaders,
    OpenWorld,
    RestoreState,
    PrepareWaterfalls,
    AdoptWaterfalls,
    PrepareChunks,
    GenerateChunks,
    PrepareHorizon,
    Activate,
    Count
};

enum class WorldEntryState {
    Idle,
    Requested,
    LoadingPainted,
    Running,
    Ready,
    Failed,
    Cancelling,
    Cancelled
};
enum class WorldEntryResult {
    Pending,
    Complete,
    Failed
};

inline const char* WorldEntryStageName(WorldEntryStage stage) {
    switch (stage) {
        case WorldEntryStage::Validate:
            return "Validate content";
        case WorldEntryStage::DrainReaders:
            return "Drain world readers";
        case WorldEntryStage::OpenWorld:
            return "Open world";
        case WorldEntryStage::RestoreState:
            return "Restore saved state";
        case WorldEntryStage::PrepareWaterfalls:
            return "Prepare waterfall geometry";
        case WorldEntryStage::AdoptWaterfalls:
            return "Upload waterfall geometry";
        case WorldEntryStage::PrepareChunks:
            return "Prepare initial chunks";
        case WorldEntryStage::GenerateChunks:
            return "Dispatch world geometry";
        case WorldEntryStage::PrepareHorizon:
            return "Prepare spawn horizon";
        case WorldEntryStage::Activate:
            return "Enter world";
        case WorldEntryStage::Count:
            return "World ready";
    }
    return "World entry";
}

// One owned request, no clock or GL. The host acknowledges presentation only
// AFTER swapping the loading frame. Each advance invokes at most one stage;
// Pending retries that same stage, never allowing completion order to reorder
// authoritative steps. Cancellation completes only after the host drains owners.
template<typename Request>
class WorldEntry {
public:
    WorldEntry() = default;
    WorldEntry(const WorldEntry&) = delete;
    WorldEntry& operator=(const WorldEntry&) = delete;

    bool request(Request value) {
        if (active())
            return false;
        m_request = std::move(value);
        m_stage = WorldEntryStage::Validate;
        m_state = WorldEntryState::Requested;
        m_cancel_pressed = false;
        return true;
    }
    void loading_painted() {
        if (m_state == WorldEntryState::Requested)
            m_state = WorldEntryState::LoadingPainted;
    }
    void cancel() {
        if (active())
            m_state = WorldEntryState::Cancelling;
    }
    // Event callbacks feed presses here. A release must not erase a press
    // delivered in the same event pump after a synchronous stage returns.
    void cancel_input(bool pressed) {
        if (active() && pressed)
            m_cancel_pressed = true;
    }
    template<typename Run, typename Drain>
    void advance(Run&& run, Drain&& drain) {
        if (std::exchange(m_cancel_pressed, false))
            cancel();
        if (m_state == WorldEntryState::Cancelling) {
            if (drain() == WorldEntryResult::Complete) {
                m_request.reset();
                m_state = WorldEntryState::Cancelled;
            }
            return;
        }
        if ((m_state != WorldEntryState::LoadingPainted && m_state != WorldEntryState::Running) ||
            !m_request.has_value())
            return;
        m_state = WorldEntryState::Running;
        const auto result = run(m_stage, *m_request);
        if (result == WorldEntryResult::Failed) {
            m_request.reset();
            m_state = WorldEntryState::Failed;
        } else if (result == WorldEntryResult::Complete) {
            m_stage = static_cast<WorldEntryStage>(static_cast<std::size_t>(m_stage) + 1);
            if (m_stage == WorldEntryStage::Count) {
                m_request.reset();
                m_state = WorldEntryState::Ready;
            }
        }
    }
    bool active() const {
        return m_state == WorldEntryState::Requested ||
               m_state == WorldEntryState::LoadingPainted || m_state == WorldEntryState::Running ||
               m_state == WorldEntryState::Cancelling;
    }
    WorldEntryState state() const {
        return m_state;
    }
    WorldEntryStage stage() const {
        return m_stage;
    }
    float progress() const {
        return static_cast<float>(m_stage) / static_cast<float>(WorldEntryStage::Count);
    }

private:
    std::optional<Request> m_request;
    WorldEntryState m_state = WorldEntryState::Idle;
    WorldEntryStage m_stage = WorldEntryStage::Validate;
    bool m_cancel_pressed = false;
};

} // namespace Luminumbra::Client
