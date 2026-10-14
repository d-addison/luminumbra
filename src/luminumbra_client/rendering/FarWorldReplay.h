#pragma once

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace Luminumbra::Rendering {
enum class ReplayObjectKind {
    VertexArray,
    Framebuffer,
    LayerFramebuffer,
    Program
};
// First-mutation journal shared by production and fake accessors. A second
// cascade must not replace the pre-replay snapshot with partially replayed state.
template<class State>
class ReplayObjectJournal {
public:
    template<class Read>
    void Preserve(std::uint32_t name, Read read) {
        for (const auto& entry : entries_)
            if (entry.first == name)
                return;
        entries_.emplace_back(name, read());
    }
    template<class Write>
    void Restore(Write write) noexcept {
        static_assert(std::is_nothrow_invocable_v<Write&, std::uint32_t, const State&>);
        for (const auto& entry : entries_)
            write(entry.first, entry.second);
        entries_.clear();
    }

private:
    std::vector<std::pair<std::uint32_t, State>> entries_;
};
// GL-free seam: production captures all touched GL state; tests implement it
// with plain values. Restoration must not throw, including during unwinding.
class ReplayStateAccess {
public:
    virtual ~ReplayStateAccess() = default;
    virtual void Save() = 0;
    virtual void Restore() noexcept = 0;
    virtual void PreserveObject(ReplayObjectKind kind, std::uint32_t name) = 0;
};
inline thread_local ReplayStateAccess* active_replay_state = nullptr;
inline void PreserveReplayObject(ReplayObjectKind kind, std::uint32_t name) {
    if (active_replay_state)
        active_replay_state->PreserveObject(kind, name);
}
class ReplayGlGuard {
public:
    explicit ReplayGlGuard(ReplayStateAccess& access)
        : access_(access) {
        access_.Save();
        previous_ = std::exchange(active_replay_state, &access_);
    }
    ~ReplayGlGuard() {
        active_replay_state = previous_;
        access_.Restore();
    }
    ReplayGlGuard(const ReplayGlGuard&) = delete;
    ReplayGlGuard& operator=(const ReplayGlGuard&) = delete;

private:
    ReplayStateAccess& access_;
    ReplayStateAccess* previous_ = nullptr;
};
template<class Restore>
class ReplayRollback {
public:
    explicit ReplayRollback(Restore restore)
        : restore_(std::move(restore)) {
        static_assert(std::is_nothrow_invocable_v<Restore&>);
    }
    ~ReplayRollback() {
        restore_();
    }
    ReplayRollback(const ReplayRollback&) = delete;
    ReplayRollback& operator=(const ReplayRollback&) = delete;

private:
    Restore restore_;
};
// Save completes before any mutation. Both guards cover begin AND dispatch,
// and the ON CPU state is restored before entry GL state on either exit.
template<class Save, class Begin, class Run>
void RunFarWorldReplay(ReplayStateAccess& gl, Save save, Begin begin, Run run) {
    auto restore = save();
    ReplayGlGuard bindings(gl);
    ReplayRollback<decltype(restore)> rollback(std::move(restore));
    begin();
    run();
}

struct FarReplayCamera {
    std::array<float, 16> view{}, projection{};
    float zoom = 0, near_plane = 0, far_plane = 0;
    std::uint32_t width = 0, height = 0, internal_width = 0, internal_height = 0;
    std::uint32_t target = 0, target_width = 0, target_height = 0;
    bool offscreen = false;
};
enum class FarReplayRefusal {
    None,
    NotStarted,
    Unprepared,
    AlreadyReplaying,
    ViewChanged,
    ProjectionChanged,
    DimensionsChanged,
    TaauHistoryChanged
};
inline FarReplayRefusal CheckFarReplay(bool started,
                                       bool prepared,
                                       bool replaying,
                                       const FarReplayCamera& on,
                                       const FarReplayCamera& now,
                                       bool history_matches = true) noexcept {
    if (!started)
        return FarReplayRefusal::NotStarted;
    if (!prepared)
        return FarReplayRefusal::Unprepared;
    if (replaying)
        return FarReplayRefusal::AlreadyReplaying;
    if (on.view != now.view)
        return FarReplayRefusal::ViewChanged;
    if (on.width != now.width || on.height != now.height ||
        on.internal_width != now.internal_width || on.internal_height != now.internal_height ||
        on.target != now.target || on.target_width != now.target_width ||
        on.target_height != now.target_height || on.offscreen != now.offscreen)
        return FarReplayRefusal::DimensionsChanged;
    if (on.projection != now.projection || on.zoom != now.zoom || on.near_plane != now.near_plane ||
        on.far_plane != now.far_plane)
        return FarReplayRefusal::ProjectionChanged;
    if (!history_matches)
        return FarReplayRefusal::TaauHistoryChanged;
    return FarReplayRefusal::None;
}
// Reset/invalidation is represented by valid. Texture identity is retained so
// a resize or replacement cannot quietly replay against different history.
struct TaauHistoryState {
    std::array<std::uint32_t, 2> textures{};
    int write_index = 0;
    bool valid = false;
    bool operator==(const TaauHistoryState&) const = default;
};
struct TaauResolvePlan {
    std::uint32_t read_texture = 0, output_texture = 0;
    bool history_valid = false, commit_history = false;
};
inline TaauResolvePlan
PlanTaauResolve(const TaauHistoryState& input, std::uint32_t scratch, bool replay) noexcept {
    return {input.textures[1 - input.write_index],
            replay ? scratch : input.textures[input.write_index],
            input.valid,
            !replay};
}
struct FarReplayEffects {
    bool replay = false;
    bool Timers() const noexcept {
        return !replay;
    }
    bool ExposureReadback() const noexcept {
        return !replay;
    }
    bool Evidence() const noexcept {
        return !replay;
    }
};
} // namespace Luminumbra::Rendering
