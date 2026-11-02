#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Luminumbra::Client::Measurement {

inline constexpr std::size_t kMaxFrames = 18000;
inline int benchmark_frame_count(int argc, char* argv[]) {
    constexpr std::string_view flag = "--render-benchmark-frames";
    std::string_view value = "120";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == flag) {
            value = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view{};
            break;
        }
        if (arg.size() > flag.size() && arg.substr(0, flag.size()) == flag &&
            arg[flag.size()] == '=') {
            value = arg.substr(flag.size() + 1);
            break;
        }
    }
    if (value.empty())
        throw std::invalid_argument("Render benchmark requires 1..18000 measured frames");
    int count = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
    if (error != std::errc{} || end != value.data() + value.size() || count < 1 ||
        count > static_cast<int>(kMaxFrames))
        throw std::invalid_argument("Render benchmark requires 1..18000 measured frames");
    return count;
}

inline constexpr std::size_t kPassCount = 11;
inline constexpr std::array<const char*, kPassCount> kPassNames = {"shadow",
                                                                   "gbuffer",
                                                                   "ssao",
                                                                   "ssao_blur",
                                                                   "lighting",
                                                                   "water",
                                                                   "skybox",
                                                                   "particle",
                                                                   "foliage",
                                                                   "aerial",
                                                                   "final_blit"};

// Mirror tools/perf/perf.py: linear interpolation at (n - 1) * fraction.
inline double percentile(std::vector<double> values, double fraction) {
    if (values.empty() || fraction < 0.0 || fraction > 1.0 || !std::isfinite(fraction))
        throw std::invalid_argument("percentile requires samples and a fraction in [0, 1]");
    std::sort(values.begin(), values.end());
    const double position = static_cast<double>(values.size() - 1) * fraction;
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper)
        return values[lower];
    const double weight = position - static_cast<double>(lower);
    return values[lower] * (1.0 - weight) + values[upper] * weight;
}

inline std::array<double, 5> summarize(const std::vector<double>& values) {
    if (values.empty() || std::any_of(values.begin(), values.end(), [](double value) {
            return !std::isfinite(value) || value < 0.0;
        }))
        throw std::invalid_argument("statistics require finite non-negative samples");
    const double median = percentile(values, 0.5);
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (const double value : values)
        deviations.push_back(std::abs(value - median));
    return {median,
            percentile(values, 0.95),
            percentile(values, 0.99),
            *std::max_element(values.begin(), values.end()),
            percentile(deviations, 0.5)};
}

// Names point only to static enum-name literals. No per-frame strings or allocations.
struct FarFrameSample {
    const char* requested_mode = "unknown";
    const char* effective_mode = "unknown";
    const char* refusal = "Unknown";
    const char* submission_refusal = "Unknown";
    bool suppressed = false, coverage_settled = false;
    bool gns_compiled = false, active_regions = false;
    int water_cells_per_side = 0;
    std::optional<float> grass_density_scale;
    std::uint64_t content_identity = 0;
    std::size_t generation_pending = 0, meshing_pending = 0, terrain_uploads_pending = 0;
    bool generation_outstanding = false, meshing_outstanding = false;
};

inline bool far_frame_settled(const FarFrameSample& sample) noexcept {
    return sample.coverage_settled && sample.generation_pending == 0 &&
           sample.meshing_pending == 0 && sample.terrain_uploads_pending == 0 &&
           !sample.generation_outstanding && !sample.meshing_outstanding;
}

struct FrameSample {
    std::uint64_t frame = 0;
    std::optional<double> wall;
    std::optional<double> cpu;
    std::optional<double> present;
    std::optional<double> gpu;
    std::optional<double> passSum;
    std::array<std::optional<double>, kPassCount> passes{};
    std::array<bool, kPassCount> issued{};
    FarFrameSample far_world;
};

// One bounded measured window. Refuse overflow instead of dropping a measured frame.
// Lookup checks the originating id before a delayed GPU result may attach to a slot.
class SampleRing {
public:
    explicit SampleRing(std::size_t capacity)
        : slots_(checked_capacity(capacity)) {}

private:
    static std::size_t checked_capacity(std::size_t capacity) {
        // At most 18000 fixed-size samples; check before vector allocation.
        if (capacity == 0 || capacity > kMaxFrames ||
            capacity > std::numeric_limits<std::size_t>::max() / sizeof(FrameSample))
            throw std::invalid_argument("measured window must contain 1..18000 frames");
        return capacity;
    }

public:
    FrameSample& append(std::uint64_t frame) {
        if (count_ == slots_.size())
            throw std::runtime_error("measured sample ring exhausted");
        if (count_ && frame != first_ + count_)
            throw std::invalid_argument("non-contiguous measured frame");
        if (!count_)
            first_ = frame;
        auto& slot = slots_[frame % slots_.size()];
        slot = {};
        slot.frame = frame;
        ++count_;
        return slot;
    }
    FrameSample& at(std::uint64_t frame) {
        if (frame < first_ || frame - first_ >= count_)
            throw std::out_of_range("GPU result has no originating frame");
        auto& slot = slots_[frame % slots_.size()];
        if (slot.frame != frame)
            throw std::out_of_range("GPU result would overwrite another frame");
        return slot;
    }
    std::size_t size() const {
        return count_;
    }
    std::uint64_t first() const {
        return first_;
    }

private:
    std::vector<FrameSample> slots_;
    std::uint64_t first_ = 0;
    std::size_t count_ = 0;
};

// Separate opt-in queries leave the legacy timer ring and v2 reporting untouched.
// A full ring drains its oldest frame before reuse; it never replaces a pending id.
template<class Backend>
class GpuQueryRing {
public:
    explicit GpuQueryRing(SampleRing& samples, Backend backend = {})
        : samples_(samples)
        , backend_(std::move(backend)) {}
    void initialize() {
        supported_ = backend_.supported();
        if (supported_)
            for (auto& slot : slots_)
                backend_.create(slot.queries.data(), slot.queries.size());
    }
    void begin_frame(std::uint64_t frame) {
        for (auto& slot : slots_)
            resolve(slot, false);
        auto& slot = slots_[frame % slots_.size()];
        resolve(slot, true);
        slot.frame = frame;
        slot.pending = supported_;
        slot.begun = {};
        slot.ended = {};
        slot.repeated = {};
        slot.accumulated = {};
        slot.accumulated_issued = {};
        slot.invalid = {};
        active_ = &slot;
        if (supported_)
            backend_.stamp(slot.queries[0]);
    }
    void begin_pass(std::size_t pass) {
        if (supported_ && active_) {
            active_->repeated.at(pass) = active_->repeated.at(pass) || active_->begun.at(pass);
            active_->begun.at(pass) = true;
            backend_.stamp(active_->queries.at(2 + pass * 2));
        }
    }
    void end_pass(std::size_t pass) {
        if (supported_ && active_) {
            active_->repeated.at(pass) = active_->repeated.at(pass) || active_->ended.at(pass);
            active_->ended.at(pass) = true;
            backend_.stamp(active_->queries.at(3 + pass * 2));
        }
    }
    // A reload action can render multiple complete passes inside one measured
    // frame. Resolve each segment before reusing its fixed query ids; retain
    // every segment's duration and any failure in O(kPassCount) storage.
    void checkpoint_passes() {
        if (supported_ && active_)
            accumulate_passes(*active_);
    }
    void end_frame() {
        if (supported_ && active_)
            backend_.stamp(active_->queries[1]);
        active_ = nullptr;
    }
    void drain() {
        for (auto& slot : slots_)
            resolve(slot, true);
    }
    void shutdown() {
        drain();
        if (supported_)
            for (auto& slot : slots_)
                backend_.destroy(slot.queries.data(), slot.queries.size());
        supported_ = false;
    }

private:
    struct Slot {
        std::array<std::uint32_t, 2 + 2 * kPassCount> queries{};
        std::array<bool, kPassCount> begun{};
        std::array<bool, kPassCount> ended{};
        std::array<bool, kPassCount> repeated{};
        std::array<double, kPassCount> accumulated{};
        std::array<bool, kPassCount> accumulated_issued{};
        std::array<bool, kPassCount> invalid{};
        std::uint64_t frame = 0;
        bool pending = false;
    };
    void resolve(Slot& slot, bool wait) {
        if (!slot.pending)
            return;
        if (!wait && !backend_.available(slot.queries[1]))
            return;
        auto& sample = samples_.at(slot.frame);
        sample.gpu = read_interval(slot, 0);
        accumulate_passes(slot);
        double sum = 0.0;
        bool complete = sample.gpu.has_value();
        for (std::size_t pass = 0; pass < slot.accumulated.size(); ++pass) {
            sample.issued[pass] = slot.accumulated_issued[pass];
            if (!sample.issued[pass])
                continue;
            if (!slot.invalid[pass])
                sample.passes[pass] = slot.accumulated[pass];
            complete = complete && sample.passes[pass].has_value();
            if (sample.passes[pass])
                sum += *sample.passes[pass];
        }
        if (complete)
            sample.passSum = sum;
        slot.pending = false;
    }
    std::optional<double> read_interval(const Slot& slot, std::size_t offset) {
        const auto begin = backend_.result(slot.queries[offset]);
        const auto end = backend_.result(slot.queries[offset + 1]);
        if (end < begin)
            return std::nullopt;
        return static_cast<double>(end - begin) / 1.0e6;
    }
    void accumulate_passes(Slot& slot) {
        for (std::size_t pass = 0; pass < slot.begun.size(); ++pass) {
            if (!slot.begun[pass] && !slot.ended[pass])
                continue;
            slot.accumulated_issued[pass] = true;
            if (slot.begun[pass] && slot.ended[pass] && !slot.repeated[pass]) {
                const auto duration = read_interval(slot, 2 + pass * 2);
                if (duration) {
                    slot.accumulated[pass] += *duration;
                    slot.invalid[pass] =
                        slot.invalid[pass] || !std::isfinite(slot.accumulated[pass]);
                } else
                    slot.invalid[pass] = true;
            } else
                slot.invalid[pass] = true;
        }
        slot.begun = {};
        slot.ended = {};
        slot.repeated = {};
    }
    SampleRing& samples_;
    Backend backend_;
    std::array<Slot, 8> slots_{};
    Slot* active_ = nullptr;
    bool supported_ = false;
};

struct CameraSample {
    std::array<double, 3> position{};
    double yaw = 0.0;
    double pitch = 0.0;
    bool operator==(const CameraSample&) const = default;
};

// Strict, ordered text manifest; no implicit defaults or ignored trailing fields.
// V1 advances one simulation tick per frame. V2 advances a script tick per frame;
// gameplay simulation pauses during menus, while the scripted camera keeps its clock.
struct Traversal {
    struct Action {
        int tick = 0;
        std::string name;
        std::array<double, 3> position{};
        double radius = 0.0;
    };
    std::string kind = "traversal";
    double duration = 0.0;
    int tickRate = 0;
    double speed = 0.0;
    std::uint32_t seed = 0;
    std::string preset;
    int presetRevision = 0;
    std::string presetIdentity;
    std::string contentIdentity;
    int expectedTicks = 0;
    std::vector<CameraSample> path;
    std::vector<int> pointTicks;
    std::vector<Action> actions;
    // At most 18001 poses, allocated only after the tick bound is checked.
    std::vector<CameraSample> steps;

    static Traversal read(std::istream& in) {
        Traversal result;
        const auto key = [&](const char* expected) {
            std::string actual;
            if (!(in >> actual) || actual != expected)
                throw std::invalid_argument(std::string("traversal expected ") + expected);
        };
        std::string version;
        in >> version;
        const bool scripted = version == "luminumbra.traversal.v2";
        if (!scripted && version != "luminumbra.traversal.v1")
            throw std::invalid_argument("unsupported traversal version");
        if (scripted) {
            key("workload");
            in >> result.kind;
            if (result.kind != "cave" && result.kind != "edit" &&
                result.kind != "region_crossing" && result.kind != "save_load" &&
                result.kind != "menu_entry")
                throw std::invalid_argument("unsupported scripted workload");
        }
        key("duration_seconds");
        in >> result.duration;
        key("tick_rate");
        in >> result.tickRate;
        key("speed_mps");
        in >> result.speed;
        // Parse wide and reject signed/wrapped uint32 seeds.
        std::int64_t seedValue = -1;
        key("seed");
        in >> seedValue;
        key("preset");
        in >> result.preset;
        key("preset_revision");
        in >> result.presetRevision;
        key("preset_identity");
        in >> result.presetIdentity;
        key("content_identity");
        in >> result.contentIdentity;
        key("expected_ticks");
        in >> result.expectedTicks;
        key("cold_cache");
        std::string coldCache;
        in >> coldCache;
        key("points");
        int count = 0;
        in >> count;
        if (!in || count < 2 || count > 1024 ||
            (result.presetRevision < 6 || result.presetRevision > 8) ||
            result.presetIdentity.size() != 16 ||
            result.presetIdentity.find_first_not_of("0123456789abcdef") != std::string::npos ||
            result.contentIdentity.empty() || result.contentIdentity.size() > 20 ||
            result.contentIdentity.find_first_not_of("0123456789") != std::string::npos ||
            coldCache != "true" || seedValue < 0 || seedValue > UINT32_MAX ||
            result.tickRate != 30 || !std::isfinite(result.duration) || result.duration <= 0 ||
            !std::isfinite(result.speed) || result.speed <= 0 || result.expectedTicks < 1 ||
            result.expectedTicks > static_cast<int>(kMaxFrames) ||
            std::abs(result.duration * result.tickRate - result.expectedTicks) > 1e-9 ||
            result.preset.empty() ||
            result.preset.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") !=
                std::string::npos)
            throw std::invalid_argument("invalid traversal manifest");
        result.seed = static_cast<std::uint32_t>(seedValue);
        for (int i = 0; i < count; ++i) {
            CameraSample point;
            key("point");
            if (scripted) {
                int tick = -1;
                in >> tick;
                if (!in || tick < 0 || tick > result.expectedTicks ||
                    (i == 0 ? tick != 0 : tick <= result.pointTicks.back()))
                    throw std::invalid_argument("invalid camera key tick");
                result.pointTicks.push_back(tick);
            }
            in >> point.position[0] >> point.position[1] >> point.position[2] >> point.yaw >>
                point.pitch;
            if (!in || !std::isfinite(point.yaw) || !std::isfinite(point.pitch) ||
                std::abs(point.pitch) >= 90.0)
                throw std::invalid_argument("invalid traversal camera");
            for (const double coordinate : point.position)
                if (!std::isfinite(coordinate) || std::abs(coordinate) > 32000.0)
                    throw std::invalid_argument("traversal leaves qualified coordinates");
            if (!scripted && !result.path.empty() && distance(result.path.back(), point) <= 0.0)
                throw std::invalid_argument("zero-length traversal segment");
            result.path.push_back(point);
        }
        if (scripted) {
            if (result.pointTicks.back() != result.expectedTicks)
                throw std::invalid_argument("camera keys must cover every script tick");
            key("actions");
            int actionCount = -1;
            in >> actionCount;
            // Scripts contain at most 64 actions and 1024 camera keys.
            if (!in || actionCount < 0 || actionCount > 64)
                throw std::invalid_argument("invalid action count");
            for (int i = 0; i < actionCount; ++i) {
                Action action;
                key("action");
                in >> action.tick >> action.name >> action.position[0] >> action.position[1] >>
                    action.position[2] >> action.radius;
                const bool edit = action.name == "dig" || action.name == "fill";
                if (!in || action.tick < 0 || action.tick >= result.expectedTicks ||
                    (!result.actions.empty() && action.tick < result.actions.back().tick) ||
                    !std::isfinite(action.radius) ||
                    (edit ? action.radius <= 0 || action.radius > 16 : action.radius != 0))
                    throw std::invalid_argument("invalid scripted action");
                for (double coordinate : action.position)
                    if (!std::isfinite(coordinate) || std::abs(coordinate) > 32000 ||
                        (!edit && coordinate != 0))
                        throw std::invalid_argument("invalid action position");
                if (action.tick == 0 && (result.kind != "cave" || action.name != "dig"))
                    throw std::invalid_argument("only cave preparation runs at tick zero");
                result.actions.push_back(std::move(action));
            }
            std::vector<std::string> names;
            for (const auto& action : result.actions)
                names.push_back(action.name);
            const bool valid =
                (result.kind == "cave" && !names.empty() &&
                 std::all_of(result.actions.begin(),
                             result.actions.end(),
                             [](const Action& a) { return a.tick == 0 && a.name == "dig"; })) ||
                (result.kind == "edit" && names == std::vector<std::string>{"dig", "fill"}) ||
                (result.kind == "region_crossing" && names.empty()) ||
                (result.kind == "save_load" &&
                 names == std::vector<std::string>{"save", "reload"}) ||
                (result.kind == "menu_entry" && names == std::vector<std::string>{"menu_open",
                                                                                  "settings_open",
                                                                                  "settings_close",
                                                                                  "menu_close",
                                                                                  "main_menu",
                                                                                  "enter_world"});
            if (!valid)
                throw std::invalid_argument("actions do not match workload");
            for (std::size_t i = 1; i < result.actions.size(); ++i)
                if (result.kind != "cave" && result.actions[i].tick <= result.actions[i - 1].tick)
                    throw std::invalid_argument("actions require distinct ticks");
        }
        std::string trailing;
        if (in >> trailing)
            throw std::invalid_argument("unknown traversal field");
        double length = 0.0;
        for (std::size_t i = 1; i < result.path.size(); ++i)
            length += distance(result.path[i - 1], result.path[i]);
        if (!scripted && length + 1e-9 < result.duration * result.speed)
            throw std::invalid_argument("traversal path shorter than duration times speed");
        if (scripted) {
            std::vector<CameraSample> expandedSteps;
            expandedSteps.reserve(static_cast<std::size_t>(result.expectedTicks) + 1);
            for (int tick = 0; tick <= result.expectedTicks; ++tick)
                expandedSteps.push_back(result.sample(tick));
            result.steps = std::move(expandedSteps);
        }
        return result;
    }
    CameraSample sample(int tick) const {
        if (tick < 0 || tick > expectedTicks)
            throw std::out_of_range("traversal tick outside manifest");
        if (!steps.empty())
            return steps[static_cast<std::size_t>(tick)];
        if (!pointTicks.empty()) {
            const auto upper = std::lower_bound(pointTicks.begin() + 1, pointTicks.end(), tick);
            const auto index = static_cast<std::size_t>(upper - pointTicks.begin());
            const double fraction = static_cast<double>(tick - pointTicks[index - 1]) /
                                    (pointTicks[index] - pointTicks[index - 1]);
            CameraSample result;
            for (std::size_t axis = 0; axis < 3; ++axis)
                result.position[axis] =
                    path[index - 1].position[axis] +
                    (path[index].position[axis] - path[index - 1].position[axis]) * fraction;
            result.yaw = path[index - 1].yaw + (path[index].yaw - path[index - 1].yaw) * fraction;
            result.pitch =
                path[index - 1].pitch + (path[index].pitch - path[index - 1].pitch) * fraction;
            return result;
        }
        double remaining = speed * static_cast<double>(tick) / tickRate;
        for (std::size_t i = 1; i < path.size(); ++i) {
            const double length = distance(path[i - 1], path[i]);
            if (remaining <= length || i + 1 == path.size()) {
                const double fraction = std::clamp(remaining / length, 0.0, 1.0);
                CameraSample result;
                for (std::size_t axis = 0; axis < 3; ++axis)
                    result.position[axis] = path[i - 1].position[axis] * (1.0 - fraction) +
                                            path[i].position[axis] * fraction;
                result.yaw = path[i - 1].yaw * (1.0 - fraction) + path[i].yaw * fraction;
                result.pitch = path[i - 1].pitch * (1.0 - fraction) + path[i].pitch * fraction;
                return result;
            }
            remaining -= length;
        }
        throw std::logic_error("empty traversal");
    }

private:
    static double distance(const CameraSample& a, const CameraSample& b) {
        double squared = 0.0;
        for (std::size_t axis = 0; axis < 3; ++axis)
            squared +=
                (a.position[axis] - b.position[axis]) * (a.position[axis] - b.position[axis]);
        return std::sqrt(squared);
    }
};
} // namespace Luminumbra::Client::Measurement
