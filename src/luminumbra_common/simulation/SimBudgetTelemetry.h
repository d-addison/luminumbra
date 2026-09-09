#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace luminumbra::simulation {

// Observational only. Never serialize into a save, simulation hash or LREC1 checkpoint.
// Stage identities and work units are declared in docs/sim-budget-telemetry.md.
enum class SimBudgetStage : std::uint8_t {
    ServerPhysics,
    Animation,
    Instinct,
    Perception,
    Scent,
    Locomotion,
    Creatures,
    Wind,
    Weather,
    Aether,
    Energy,
    Irrigation,
    Soil,
    Plants,
    Pollination,
    Disease,
    Crops,
    Fire,
    Grazing,
    Lifespan,
    Alarm,
    Decay,
    Circadian,
    Territory,
    Packs,
    Migration,
    Events,
    ServerStreaming,
    ServerWater,
    Count
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(SimBudgetStage::Count)>
    kSimBudgetStageNames = {"server_physics", "animation",  "instinct",    "perception",
                            "scent",          "locomotion", "creatures",   "wind",
                            "weather",        "aether",     "energy",      "irrigation",
                            "soil",           "plants",     "pollination", "disease",
                            "crops",          "fire",       "grazing",     "lifespan",
                            "alarm",          "decay",      "circadian",   "territory",
                            "packs",          "migration",  "events",      "server_streaming",
                            "server_water"};

struct SimBudgetSample {
    std::uint64_t tick = 0;
    std::uint64_t work = 0;
    double duration_ms = 0.0;
};

struct SimBudgetSummary {
    std::uint64_t work_total = 0;
    std::size_t samples = 0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
};

class SimBudgetTelemetry {
public:
    using Samples = std::array<std::vector<SimBudgetSample>, kSimBudgetStageNames.size()>;

    void SetEnabled(bool enabled) {
        enabled_ = enabled;
        Clear();
    }
    [[nodiscard]] bool Enabled() const {
        return enabled_;
    }
    void Clear() {
        for (auto& stage : samples_)
            stage.clear();
    }
    [[nodiscard]] const Samples& SamplesByStage() const {
        return samples_;
    }

    void Record(SimBudgetStage stage, std::uint64_t tick, std::uint64_t work, double duration_ms) {
        if (enabled_)
            samples_[static_cast<std::size_t>(stage)].push_back({tick, work, duration_ms});
    }

    // Deliberately excludes all wall-clock values, including in replay validation.
    [[nodiscard]] bool WorkMatches(const SimBudgetTelemetry& other) const {
        for (std::size_t stage = 0; stage < samples_.size(); ++stage) {
            const auto& left = samples_[stage];
            const auto& right = other.samples_[stage];
            if (left.size() != right.size())
                return false;
            for (std::size_t i = 0; i < left.size(); ++i)
                if (left[i].tick != right[i].tick || left[i].work != right[i].work)
                    return false;
        }
        return true;
    }

    [[nodiscard]] SimBudgetSummary Summarize(SimBudgetStage stage) const {
        SimBudgetSummary result;
        std::vector<double> durations;
        const auto& samples = samples_[static_cast<std::size_t>(stage)];
        durations.reserve(samples.size());
        for (const auto& sample : samples) {
            result.work_total += sample.work;
            durations.push_back(sample.duration_ms);
        }
        result.samples = samples.size();
        if (samples.empty())
            return result;
        std::sort(durations.begin(), durations.end());
        // tools/perf/perf.py: linear interpolation at (n - 1) * fraction.
        const auto percentile = [&durations](double fraction) {
            const double position = static_cast<double>(durations.size() - 1) * fraction;
            const auto lower = static_cast<std::size_t>(std::floor(position));
            const auto upper = static_cast<std::size_t>(std::ceil(position));
            const double weight = position - static_cast<double>(lower);
            return durations[lower] * (1.0 - weight) + durations[upper] * weight;
        };
        result.p50_ms = percentile(0.50);
        result.p95_ms = percentile(0.95);
        result.p99_ms = percentile(0.99);
        result.max_ms = durations.back();
        return result;
    }

    // A cursor measures the existing ordered stages without changing their scopes.
    // Count callbacks execute only when enabled, before the stage's timer starts.
    class TickScope {
    public:
        TickScope(SimBudgetTelemetry& telemetry, std::uint64_t tick)
            : telemetry_(telemetry.Enabled() ? &telemetry : nullptr)
            , tick_(tick) {}
        TickScope(const TickScope&) = delete;
        TickScope& operator=(const TickScope&) = delete;
        ~TickScope() {
            Finish();
        }

        template<typename Counter>
        void Next(SimBudgetStage stage, Counter count) {
            if (!telemetry_)
                return;
            Finish();
            stage_ = stage;
            work_ = count();
            start_ = std::chrono::steady_clock::now();
            active_ = true;
        }
        void Finish() {
            if (!active_)
                return;
            const double elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
                    .count();
            telemetry_->Record(stage_, tick_, work_, elapsed);
            active_ = false;
        }

    private:
        SimBudgetTelemetry* telemetry_;
        std::uint64_t tick_;
        SimBudgetStage stage_ = SimBudgetStage::Animation;
        std::uint64_t work_ = 0;
        std::chrono::steady_clock::time_point start_{};
        bool active_ = false;
    };

private:
    bool enabled_ = false;
    Samples samples_;
};

} // namespace luminumbra::simulation
