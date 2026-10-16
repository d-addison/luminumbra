#pragma once

#include "RenderMeasurement.h"

#include <chrono>
#include <optional>
#include <string_view>

namespace Luminumbra::Client::Measurement {

// Initial saved authority is outside the v4 identity. Only reloads of the world
// created by this capture are allowed, for save/load and menu-entry scripts.
inline void require_workload_world_load(bool capture_active,
                                        std::string_view requested_id,
                                        std::string_view created_world_id) {
    if (capture_active && (created_world_id.empty() || requested_id != created_world_id))
        throw std::invalid_argument("schema v4 refuses --load-world and existing-world loads; "
                                    "only the world created by this capture may be reloaded");
}

struct ActionResult {
    bool succeeded = true;
    int modified_chunks = 0;
};

// The cursor consumes each of the at most 64 script actions once. Callbacks are
// statically dispatched; the frame path does no allocation or type erasure here.
template<class Edit, class Save, class Load, class Pause, class Settings, class Record>
bool dispatch_actions(const std::vector<Traversal::Action>& actions,
                      std::size_t& cursor,
                      int tick,
                      Edit edit,
                      Save save,
                      Load load,
                      Pause pause,
                      Settings settings,
                      Record record) {
    while (cursor < actions.size() && actions[cursor].tick == tick) {
        const auto& action = actions[cursor];
        ActionResult result;
        if (action.name == "dig" || action.name == "fill")
            result = edit(action, action.name == "fill");
        else if (action.name == "save" || action.name == "main_menu")
            result = save(action.name == "main_menu");
        else if (action.name == "reload" || action.name == "enter_world")
            result = load(action.name == "reload");
        else if (action.name == "menu_open" || action.name == "menu_close")
            result = pause(action.name == "menu_open");
        else if (action.name == "settings_open" || action.name == "settings_close")
            result = settings(action.name == "settings_open");
        else
            throw std::invalid_argument("unknown workload action");
        record(action, result);
        ++cursor;
        if (!result.succeeded)
            return false;
    }
    return true;
}

class ReloadInterval {
public:
    using Clock = std::chrono::steady_clock;
    struct Result {
        std::uint64_t request_frame = 0;
        std::uint64_t first_settled_frame = 0;
        double elapsed_ms = 0;
    };

    // Call immediately before requesting a world reload, and observe after
    // present. The request frame is included; frames before it are excluded.
    void request(std::uint64_t frame, Clock::time_point now) {
        requested_ = now;
        request_frame_ = frame;
        result_.reset();
    }
    void observe(std::uint64_t frame, bool settled, Clock::time_point present_end) {
        if (requested_ && !result_ && settled && frame >= request_frame_ &&
            present_end >= *requested_)
            result_ = Result{
                request_frame_,
                frame,
                std::chrono::duration<double, std::milli>(present_end - *requested_).count()};
    }
    const std::optional<Result>& result() const {
        return result_;
    }

private:
    std::optional<Clock::time_point> requested_;
    std::uint64_t request_frame_ = 0;
    std::optional<Result> result_;
};

struct MenuObservation {
    std::string_view state;
    std::string_view document;
    bool paused = false;
};

// Record visible documents after UI Update/Render, not queued requests. The
// fixed precedence matches the renderer when more than one document is visible.
template<class Visible>
MenuObservation observe_menu(bool in_game, bool paused, Visible visible) {
    MenuObservation result{in_game ? "gameplay" : "main_menu", "none", paused};
    for (const char* document : {"hud", "pause", "settings", "main_menu"})
        if (visible(document))
            result.document = document;
    return result;
}

} // namespace Luminumbra::Client::Measurement
