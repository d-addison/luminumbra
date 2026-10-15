#include "RegionTelemetry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>

namespace luminumbra::simulation {
namespace {
std::atomic<std::size_t> sample_bytes{0}, peak_sample_bytes{0};
void NoteSampleAllocation() {
    const auto current =
        sample_bytes.fetch_add(RegionTelemetry::kSampleStorageBytes, std::memory_order_relaxed) +
        RegionTelemetry::kSampleStorageBytes;
    auto peak = peak_sample_bytes.load(std::memory_order_relaxed);
    while (peak < current &&
           !peak_sample_bytes.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {}
}
} // namespace

RegionTelemetry::Storage::Storage() {
    static_assert(sizeof(Storage) == kSampleStorageBytes);
    NoteSampleAllocation();
}
RegionTelemetry::Storage::Storage(const Storage& other)
    : scheduler(other.scheduler)
    , saves(other.saves) {
    NoteSampleAllocation();
}
RegionTelemetry::Storage::~Storage() {
    sample_bytes.fetch_sub(kSampleStorageBytes, std::memory_order_relaxed);
}
RegionTelemetry::SampleStorageUsage RegionTelemetry::SampleStorageUsageForTesting() {
    return {sample_bytes.load(std::memory_order_relaxed),
            peak_sample_bytes.load(std::memory_order_relaxed)};
}
void RegionTelemetry::ResetSampleStoragePeakForTesting() {
    peak_sample_bytes.store(sample_bytes.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
}

void RegionTelemetry::Gauge::Observe(std::uint64_t value) {
    current = value;
    maximum = std::max(maximum, value);
}
RegionTelemetry::RegionTelemetry(const RegionTelemetry& other) {
    *this = other;
}
RegionTelemetry& RegionTelemetry::operator=(const RegionTelemetry& other) {
    if (this == &other)
        return *this;
    RegionTelemetry copy;
    copy.CopyObservations(other);
    if (other.storage_)
        copy.storage_ = std::make_unique<Storage>(*other.storage_);
    if (other.summaries_)
        copy.summaries_ = std::make_unique<Summaries>(*other.summaries_);
    *this = std::move(copy);
    return *this;
}
void RegionTelemetry::CopyObservations(const RegionTelemetry& other) {
    enabled_ = other.enabled_;
    complete_ = other.complete_;
    samples_ = other.samples_;
    dropped_samples_ = other.dropped_samples_;
    first_tick_ = other.first_tick_;
    last_tick_ = other.last_tick_;
    fault_tick_ = other.fault_tick_;
    save_samples_ = other.save_samples_;
    fault_message_size_ = other.fault_message_size_;
    fault_message_ = other.fault_message_;
    previous_ = other.previous_;
    totals_ = other.totals_;
    trace_ = other.trace_;
    ledger_records = other.ledger_records;
    active = other.active;
    reduced = other.reduced;
    frozen = other.frozen;
    parked_bytes = other.parked_bytes;
    due_total = other.due_total;
    accepted_work_total = other.accepted_work_total;
    work_limit = other.work_limit;
    pressure_max = other.pressure_max;
    rejected_anchors = other.rejected_anchors;
    rejected_work = other.rejected_work;
    rejected_capacity = other.rejected_capacity;
    creatures_full = other.creatures_full;
    creatures_coarse = other.creatures_coarse;
    creatures_frozen = other.creatures_frozen;
    pending_activations = other.pending_activations;
    saves_total = other.saves_total;
    save_failures_total = other.save_failures_total;
    water_reserved = other.water_reserved;
    water_used = other.water_used;
    water_unused = other.water_unused;
    water_unallocated = other.water_unallocated;
    water_completed_cells = other.water_completed_cells;
}
RegionTelemetry RegionTelemetry::Capture() const {
    RegionTelemetry result;
    result.CopyObservations(*this);
    if (Enabled())
        result.summaries_ =
            std::make_unique<Summaries>(Summaries{SchedulerDuration(), SaveDuration()});
    return result;
}
void RegionTelemetry::SetEnabled(bool enabled) {
    if (enabled && storage_) {
        Clear();
        return;
    }
    RegionTelemetry fresh;
    if (enabled)
        fresh.storage_ = std::make_unique<Storage>();
    fresh.enabled_ = enabled;
    *this = std::move(fresh);
}
void RegionTelemetry::Clear() {
    const bool was_enabled = Enabled();
    if (summaries_) {
        SetEnabled(was_enabled);
        return;
    }
    auto retained = std::move(storage_);
    *this = RegionTelemetry{};
    enabled_ = was_enabled;
    storage_ = std::move(retained);
}
void RegionTelemetry::Drop() {
    complete_ = false;
    if (dropped_samples_ != std::numeric_limits<std::uint64_t>::max())
        ++dropped_samples_;
}
void RegionTelemetry::Add(std::uint64_t& total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        total = std::numeric_limits<std::uint64_t>::max();
        Drop();
    } else
        total += value;
}
void RegionTelemetry::Trace(std::uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
        trace_ ^= (value >> (byte * 8)) & 255u;
        trace_ *= 1099511628211ull;
    }
}
void RegionTelemetry::ObserveActivity(const Activities& values) {
    if (!Enabled())
        return;
    Trace(2);
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values[index] < previous_[index]) {
            Drop();
        } else {
            const auto delta = values[index] - previous_[index];
            Add(totals_[index], delta);
            // Restoring a session reconstructs runtime controllers without changing authority.
            if (index != ControllersCreated && index != ControllersRemoved && index != StaleHandles)
                Trace(delta);
        }
    }
    previous_ = values;
}
void RegionTelemetry::RecordScheduler(std::uint64_t tick,
                                      double duration_ms,
                                      const SchedulerObservation& observation,
                                      std::uint64_t due,
                                      std::uint64_t accepted_work,
                                      std::uint64_t configured_limit,
                                      std::uint64_t anchors_rejected,
                                      std::uint64_t work_rejected,
                                      std::uint64_t capacity_rejected,
                                      std::string_view schedule_digest) {
    if (!Enabled())
        return;
    if (!storage_ || samples_ == kMaxSamples || tick == 0 || schedule_digest.size() != 16 ||
        !std::isfinite(duration_ms) || duration_ms < 0.0 ||
        (last_tick_ && (tick <= *last_tick_ || tick - *last_tick_ != 1))) {
        Drop();
        return;
    }
    storage_->scheduler[static_cast<std::size_t>(samples_)] = duration_ms;
    ++samples_;
    if (!first_tick_)
        first_tick_ = tick;
    last_tick_ = tick;
    ledger_records.Observe(observation.ledger_records);
    active.Observe(observation.active);
    reduced.Observe(observation.reduced);
    frozen.Observe(observation.frozen);
    pressure_max = std::max(pressure_max, observation.pressure);
    work_limit = configured_limit;
    Add(due_total, due);
    Add(accepted_work_total, accepted_work);
    Add(rejected_anchors, anchors_rejected);
    Add(rejected_work, work_rejected);
    Add(rejected_capacity, capacity_rejected);
    Trace(1);
    Trace(tick);
    Trace(due);
    Trace(accepted_work);
    Trace(configured_limit);
    Trace(anchors_rejected);
    Trace(work_rejected);
    Trace(capacity_rejected);
    Trace(schedule_digest.size());
    for (const char value : schedule_digest) {
        trace_ ^= static_cast<unsigned char>(value);
        trace_ *= 1099511628211ull;
    }
}
void RegionTelemetry::RecordWater(std::uint64_t reserved,
                                  std::uint64_t used,
                                  std::uint64_t unused,
                                  std::uint64_t unallocated,
                                  std::uint64_t completed_cells) {
    if (!Enabled())
        return;
    Add(water_reserved, reserved);
    Add(water_used, used);
    Add(water_unused, unused);
    Add(water_unallocated, unallocated);
    Add(water_completed_cells, completed_cells);
    Trace(3);
    Trace(reserved);
    Trace(used);
    Trace(unused);
    Trace(unallocated);
    Trace(completed_cells);
}
void RegionTelemetry::RecordSave(bool success, double duration_ms) {
    if (!Enabled())
        return;
    if (!storage_) {
        Drop();
        return;
    }
    Add(saves_total, 1);
    if (!success)
        Add(save_failures_total, 1);
    if (save_samples_ == kMaxSaveSamples || !std::isfinite(duration_ms) || duration_ms < 0.0) {
        Drop();
        return;
    }
    storage_->saves[save_samples_++] = duration_ms;
}
void RegionTelemetry::ObserveParkedBytes(std::uint64_t bytes) {
    if (Enabled())
        parked_bytes.Observe(bytes);
}
void RegionTelemetry::ObserveGauges(std::uint64_t full_count,
                                    std::uint64_t coarse_count,
                                    std::uint64_t frozen_count,
                                    std::uint64_t pending,
                                    std::uint64_t parked) {
    if (!Enabled())
        return;
    creatures_full = full_count;
    creatures_coarse = coarse_count;
    creatures_frozen = frozen_count;
    pending_activations = pending;
    ObserveParkedBytes(parked);
}
void RegionTelemetry::RecordFault(std::uint64_t tick, std::string_view message) {
    if (!Enabled() || fault_tick_)
        return;
    fault_tick_ = tick;
    if (message.size() > kMaxFaultBytes)
        Drop();
    fault_message_size_ = std::min(message.size(), kMaxFaultBytes);
    for (std::size_t index = 0; index < fault_message_size_; ++index) {
        const auto value = static_cast<unsigned char>(message[index]);
        fault_message_[index] = value >= 32 && value < 127 ? static_cast<char>(value) : '?';
    }
}
std::optional<RegionTelemetry::Distribution> RegionTelemetry::Summarize(double* values,
                                                                        std::size_t size) {
    if (!size)
        return std::nullopt;
    // Duration order is observational; ticks and work are stored separately.
    std::sort(values, values + size);
    const auto percentile = [values, size](double fraction) {
        const double position = static_cast<double>(size - 1) * fraction;
        const auto lower = static_cast<std::size_t>(std::floor(position));
        const auto upper = static_cast<std::size_t>(std::ceil(position));
        const double weight = position - static_cast<double>(lower);
        return values[lower] + (values[upper] - values[lower]) * weight;
    };
    return Distribution{percentile(0.50), percentile(0.95), percentile(0.99), values[size - 1]};
}
std::optional<RegionTelemetry::Distribution> RegionTelemetry::SchedulerDuration() const {
    if (summaries_)
        return summaries_->scheduler;
    return storage_ ? Summarize(storage_->scheduler.data(), static_cast<std::size_t>(samples_))
                    : std::nullopt;
}
std::optional<RegionTelemetry::Distribution> RegionTelemetry::SaveDuration() const {
    if (summaries_)
        return summaries_->saves;
    return storage_ ? Summarize(storage_->saves.data(), save_samples_) : std::nullopt;
}
bool RegionTelemetry::WorkMatches(const RegionTelemetry& other) const {
    return Enabled() == other.Enabled() &&
           (!Enabled() || (complete_ && other.complete_ && samples_ == other.samples_ &&
                           first_tick_ == other.first_tick_ && last_tick_ == other.last_tick_ &&
                           trace_ == other.trace_));
}
std::string RegionTelemetry::TraceDigest() const {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = digits[(trace_ >> ((15 - index) * 4)) & 15u];
    return result;
}
std::size_t RegionTelemetry::RetainedSampleBytes() const {
    return storage_ ? sizeof(Storage) : 0;
}
} // namespace luminumbra::simulation
