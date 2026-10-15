#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace luminumbra::simulation {

// Observations only: no member is persistence, scheduling or hash authority.
class RegionTelemetry {
public:
    static constexpr std::size_t kMaxSamples = 108000;
    static constexpr std::size_t kMaxSaveSamples = 4096;
    static constexpr std::size_t kMaxFaultBytes = 256;
    static constexpr std::size_t kMaxBlockBytes = 16383;
    // Export sorts observations in place and retains only scalar summaries.
    static constexpr std::size_t kSampleStorageBytes =
        (kMaxSamples + kMaxSaveSamples) * sizeof(double);
    static constexpr std::size_t kHeavySampleStorageBytes = 2 * kSampleStorageBytes;
    static_assert(kHeavySampleStorageBytes < 2 * 1024 * 1024);
    struct SampleStorageUsage {
        std::size_t current = 0, peak = 0;
    };
    [[nodiscard]] static SampleStorageUsage SampleStorageUsageForTesting();
    static void ResetSampleStoragePeakForTesting();

    enum Activity : std::size_t {
        Promotions,
        Demotions,
        FullUpdates,
        CoarseUpdates,
        Births,
        Deaths,
        Removals,
        ParkedTravel,
        ControllersCreated,
        ControllersRemoved,
        StaleHandles,
        PopulationSuccesses,
        PopulationFailures,
        ActivityCount
    };
    using Activities = std::array<std::uint64_t, ActivityCount>;
    struct Gauge {
        std::uint64_t current = 0, maximum = 0;
        void Observe(std::uint64_t value);
    };
    struct Distribution {
        double p50 = 0.0, p95 = 0.0, p99 = 0.0, maximum = 0.0;
    };
    struct SchedulerObservation {
        std::uint64_t ledger_records = 0, active = 0, reduced = 0, frozen = 0, pressure = 0;
    };

    RegionTelemetry() = default;
    RegionTelemetry(const RegionTelemetry& other);
    RegionTelemetry& operator=(const RegionTelemetry& other);
    RegionTelemetry(RegionTelemetry&&) noexcept = default;
    RegionTelemetry& operator=(RegionTelemetry&&) noexcept = default;
    void SetEnabled(bool enabled);
    void Clear();
    [[nodiscard]] bool Enabled() const {
        return enabled_ && (storage_ || summaries_);
    }
    // Scalar export; no retained-array copy or percentile scratch allocation.
    [[nodiscard]] RegionTelemetry Capture() const;
    void SetActivityBaseline(const Activities& values) {
        previous_ = values;
    }
    void ObserveActivity(const Activities& values);
    void RecordScheduler(std::uint64_t tick,
                         double duration_ms,
                         const SchedulerObservation& observation,
                         std::uint64_t due,
                         std::uint64_t accepted_work,
                         std::uint64_t configured_limit,
                         std::uint64_t anchors_rejected,
                         std::uint64_t work_rejected,
                         std::uint64_t capacity_rejected,
                         std::string_view schedule_digest);
    void RecordWater(std::uint64_t reserved,
                     std::uint64_t used,
                     std::uint64_t unused,
                     std::uint64_t unallocated,
                     std::uint64_t completed_cells);
    void RecordSave(bool success, double duration_ms);
    void ObserveParkedBytes(std::uint64_t bytes);
    void ObserveGauges(std::uint64_t full_count,
                       std::uint64_t coarse_count,
                       std::uint64_t frozen_count,
                       std::uint64_t pending,
                       std::uint64_t parked);
    void RecordFault(std::uint64_t tick, std::string_view message);
    [[nodiscard]] std::optional<Distribution> SchedulerDuration() const;
    [[nodiscard]] std::optional<Distribution> SaveDuration() const;
    [[nodiscard]] bool WorkMatches(const RegionTelemetry& other) const;
    [[nodiscard]] std::string TraceDigest() const;
    [[nodiscard]] std::size_t RetainedSampleBytes() const;

    [[nodiscard]] std::uint64_t Samples() const {
        return samples_;
    }
    [[nodiscard]] std::uint64_t DroppedSamples() const {
        return dropped_samples_;
    }
    [[nodiscard]] bool Complete() const {
        return complete_;
    }
    [[nodiscard]] std::optional<std::uint64_t> FirstTick() const {
        return first_tick_;
    }
    [[nodiscard]] std::optional<std::uint64_t> LastTick() const {
        return last_tick_;
    }
    [[nodiscard]] std::size_t SaveSamples() const {
        return save_samples_;
    }
    [[nodiscard]] const Activities& Totals() const {
        return totals_;
    }
    [[nodiscard]] std::optional<std::uint64_t> FaultTick() const {
        return fault_tick_;
    }
    [[nodiscard]] std::string_view FaultMessage() const {
        return {fault_message_.data(), fault_message_size_};
    }

    Gauge ledger_records, active, reduced, frozen, parked_bytes;
    std::uint64_t due_total = 0, accepted_work_total = 0, work_limit = 0, pressure_max = 0;
    std::uint64_t rejected_anchors = 0, rejected_work = 0, rejected_capacity = 0;
    std::uint64_t creatures_full = 0, creatures_coarse = 0, creatures_frozen = 0;
    std::uint64_t pending_activations = 0, saves_total = 0, save_failures_total = 0;
    std::uint64_t water_reserved = 0, water_used = 0, water_unused = 0;
    std::uint64_t water_unallocated = 0, water_completed_cells = 0;

private:
    struct Storage {
        Storage();
        Storage(const Storage& other);
        ~Storage();
        std::array<double, kMaxSamples> scheduler{};
        std::array<double, kMaxSaveSamples> saves{};
    };
    struct Summaries {
        std::optional<Distribution> scheduler, saves;
    };
    static std::optional<Distribution> Summarize(double* values, std::size_t size);
    void CopyObservations(const RegionTelemetry& other);
    void Drop();
    void Add(std::uint64_t& total, std::uint64_t value);
    void Trace(std::uint64_t value);
    bool enabled_ = false, complete_ = true;
    std::uint64_t samples_ = 0, dropped_samples_ = 0;
    std::optional<std::uint64_t> first_tick_, last_tick_, fault_tick_;
    std::size_t save_samples_ = 0, fault_message_size_ = 0;
    std::array<char, kMaxFaultBytes> fault_message_{};
    Activities previous_{}, totals_{};
    std::uint64_t trace_ = 14695981039346656037ull;
    std::unique_ptr<Storage> storage_;
    std::unique_ptr<Summaries> summaries_;
};

} // namespace luminumbra::simulation
