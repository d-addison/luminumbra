#pragma once

#include "RenderMeasurement.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

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

// Allocate at most Limit elements before warmup. Appending a fixed record never
// allocates; each frame and each transition is visited once. Refuse overflow
// without dropping evidence.
template<class T, std::size_t Limit>
class ObservationRecords {
    static_assert(Limit <= std::numeric_limits<std::size_t>::max() / sizeof(T));

public:
    constexpr explicit ObservationRecords(std::size_t capacity)
        : records_(checked_capacity(capacity)) {}
    constexpr ObservationRecords(const ObservationRecords&) = default;
    constexpr ObservationRecords& operator=(const ObservationRecords&) = default;
    // Transfer the count with its storage. The source becomes an empty buffer
    // which refuses append until assigned another preallocated buffer.
    constexpr ObservationRecords(ObservationRecords&& other) noexcept
        : records_(std::move(other.records_))
        , size_(std::exchange(other.size_, 0)) {
        other.records_.clear();
    }
    constexpr ObservationRecords& operator=(ObservationRecords&& other) noexcept {
        if (this != &other) {
            records_ = std::move(other.records_);
            size_ = std::exchange(other.size_, 0);
            other.records_.clear();
        }
        return *this;
    }
    constexpr void append(const T& record) {
        if (size_ >= records_.size())
            throw std::runtime_error("workload observations exceed bounds");
        // Check the actual storage access as well as the logical count, and
        // publish the record only after assignment has succeeded.
        records_.at(size_) = record;
        ++size_;
    }
    constexpr std::size_t size() const {
        return size_;
    }
    constexpr std::size_t remaining() const {
        return records_.size() - size_;
    }
    constexpr const T& at(std::size_t index) const {
        if (index >= size_)
            throw std::out_of_range("missing workload observation");
        return records_.at(index);
    }

private:
    static constexpr std::size_t checked_capacity(std::size_t capacity) {
        // Both the fixed capture limit and the allocator's element/byte bound
        // are checked before constructing storage, without multiplying sizes.
        if (capacity > Limit || capacity > std::vector<T>{}.max_size())
            throw std::invalid_argument("workload observation capacity exceeds bounds");
        return capacity;
    }
    std::vector<T> records_;
    std::size_t size_ = 0;
};

struct CameraObservation {
    int tick = 0;
    std::array<double, 3> position{};
    double yaw = 0, pitch = 0;
};
struct ActionObservation {
    int tick = 0;
    std::string_view name;
    bool succeeded = false;
    int modified_chunks = 0;
};
struct TransitionObservation {
    std::array<int, 2> region{};
    int from = 0, to = 0;
};
struct WorkloadObservation {
    int tick = 0;
    double presentation_seconds = 0;
    std::uint64_t frame = 0;
    std::string_view state, ui_document;
    bool paused = false;
    std::array<int, 2> region{};
    std::size_t transitions_begin = 0, transitions_end = 0;
    std::string_view far_requested, far_effective;
    std::optional<int> cave_requested, cave_effective;
    bool underground = false, camera_in_air = false;
    std::uint64_t foliage_build_generation = 0, terrain_uploads = 0, stream_queue_depth = 0;
    std::uint64_t stream_generation_batches = 0, stream_meshing_batches = 0;
    std::uint64_t stream_pending_lod_chunks = 0;
    bool stream_promotion_pending = false;
    bool region_scheduler_fault = false;
    std::uint64_t terrain_upload_candidates = 0, water_upload_candidates = 0;
    bool foliage_pending = false, world_ready = false, settled = false;
};
// Bound transition storage independently of frame count (at most 65536 per
// capture); reject excess before iterating any transition in the refused frame.
inline constexpr std::size_t kMaxCaptureTransitions = 65536;
using TransitionRecords = ObservationRecords<TransitionObservation, kMaxCaptureTransitions>;

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
