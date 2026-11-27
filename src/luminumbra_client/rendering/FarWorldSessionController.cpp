#include "FarWorldSessionController.h"
#include "FarWorldHandoffDiagnostics.h"
#include "core/Log.h"
#include "world/FarWorldCapturePlan.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace Luminumbra::Rendering {
using namespace World;
using Producer = FarWorldAuthorityProducer;
using Plan = FarWorldCapturePlan;
namespace {
bool Finite(std::uint64_t value) {
    return value != 0 && value != std::numeric_limits<std::uint64_t>::max();
}
Producer::FrozenResult PlanFailure(const Plan::Result& result) {
    if (!result.source.ok())
        return result.source;
    using E = Plan::Error;
    using C = FarFrozenCaptureError;
    switch (result.error) {
        case E::None:
            return {};
        case E::InvalidLimits:
            return {Producer::Error::InvalidLimits};
        case E::InvalidWanted:
        case E::InvalidSpan:
            return {Producer::Error::InvalidWindow};
        case E::Capacity:
            return {Producer::Error::IndexCapacity};
        case E::SourceChanged:
            return {Producer::Error::None, C::IdentityMismatch};
        case E::SourceRefused:
        case E::Inactive:
            return {Producer::Error::AuthorityUnavailable};
        case E::WorkLimit:
            return {Producer::Error::None, C::WorkLimit};
        case E::AllocationFailure:
            return {Producer::Error::AllocationFailure};
    }
    return {Producer::Error::AuthorityUnavailable};
}
} // namespace
bool FarWorldSessionQualificationLimits::Complete() const noexcept {
    const auto& read = capture.read;
    if (!rendering.Complete() || !tiles || tiles > Producer::kSlots || !source.index_records ||
        source.index_records > Producer::kSlots || source.memory || !capture.clock.now ||
        !capture.deadline_frames || !refresh_frames || !sampling.max_regions || sampling.work ||
        sampling.deadline.now || !Finite(source.copy_bytes) || !Finite(source.query_columns) ||
        source.copy_bytes > rendering.budgets.bytes.back() || !Finite(capture.clock.max_ticks) ||
        !Finite(refresh_ticks) || !Finite(ticks_per_second) || !Finite(planning_operations) ||
        !Finite(admission_operations) || !Finite(operations_per_pump) ||
        !Finite(sampling.max_bytes) || !Finite(sampling.max_height_samples) ||
        !Finite(sampling.max_density_samples) || !Finite(sampling.max_water_samples) ||
        !Finite(transaction_samples.height) || !Finite(transaction_samples.density) ||
        !Finite(transaction_samples.water))
        return false;
    for (const auto value : {read.members,
                             read.records,
                             read.headers,
                             read.probes,
                             read.compressed_member_bytes,
                             read.compressed_total_bytes,
                             read.decoded_member_bytes,
                             read.decoded_total_bytes,
                             read.control_bytes})
        if (!Finite(value))
            return false;
    const auto& spans = rendering.spans;
    constexpr auto guard = kFarVolumeExactCoordinateLimitMeters - 1024;
    return read.compressed_member_bytes <= read.compressed_total_bytes &&
           read.decoded_member_bytes <= read.decoded_total_bytes && spans.min_y > -guard &&
           spans.max_y < guard;
}
struct FarWorldSessionController::Impl {
    explicit Impl(const std::shared_ptr<FarVolumeMemoryPool>& pool)
        : plan(pool) {}
    FarWorldSessionQualificationLimits limits;
    Producer::FrozenPolicy policy;
    std::shared_ptr<Producer> producer;
    std::shared_ptr<const Systems::SHIELD_WorldSystem> world;
    Plan plan;
    std::optional<FarFrozenCaptureWork> sampling;
    FarFrozenCaptureDeadline deadline;
    // Fixed 512-slot controls and transaction scratch, admitted with Impl before
    // allocation. No container grows with world size or publication count.
    struct Ledger {
        FarTileKey key;
        FarVolumeSpan span;
        std::uint64_t generation = 0;
    };
    std::array<Ledger, Producer::kSlots> history{}, pending_history{};
    std::array<FarSessionHandoffTile, Producer::kSlots> tiles{};
    std::array<FarTileKey, Producer::kSlots> successor{}, retained{};
    std::size_t history_count = 0, pending_count = 0, tile_count = 0, successor_count = 0;
    bool queued = false;
    std::optional<std::uint64_t> frame;
    std::uint64_t start_frame = 0, epoch = 0, source_operations = 0;
    // Constant-size terminal measurement receipt. Source progress survives a
    // refusal/cancellation; sampling counts must be observed before unwinding.
    Producer::CaptureProgress measured_source;
    FarFrozenCaptureWork::Counts measured_samples;
    std::uint64_t start_ticks = 0, elapsed_ticks = 0;
    std::uint64_t source_frames = 0, preparation_frames = 0;
    bool receipt_pending = false;
    void Record(const FarWorldRefreshWork& work,
                FarWorldRefusal refusal,
                const Producer::FrozenResult& failure) {
        if (!receipt_pending)
            return;
        receipt_pending = false;
        if (sampling)
            measured_samples = sampling->counts();
        const auto& clock = limits.capture.clock;
        const auto end = clock.now(clock.context);
        elapsed_ticks = end >= start_ticks ? end - start_ticks : 0;
        // Shipping construction is restricted to the diagnostics host opt-in;
        // explicit qualification harnesses use the same terminal receipts.
        LUMINUMBRA_CORE_INFO(
            "Far ladder diagnostics refresh: reads={} columns={} members={} "
            "compressed_bytes={} decoded_bytes={} source_ops={} planning_ops={} "
            "admission_ops={} height_samples={} density_samples={} water_samples={} "
            "source_frames={} preparation_frames={} elapsed_ticks={} refusal={} "
            "source_error={} capture_error={}",
            measured_source.reads,
            measured_source.columns,
            measured_source.members,
            work.source_read_bytes,
            work.source_decode_work,
            source_operations,
            work.capture_work,
            work.admission_work,
            measured_samples.height,
            measured_samples.density,
            measured_samples.water,
            source_frames,
            preparation_frames,
            elapsed_ticks,
            FarWorldRefusalName(refusal),
            static_cast<int>(failure.source),
            static_cast<int>(failure.capture));
    }
};
FarWorldSessionController::FarWorldSessionController(FarWorldRenderer& renderer)
    : renderer_(renderer) {}
FarWorldSessionController::~FarWorldSessionController() {
    Cancel();
}
void FarWorldSessionController::CheckOwner() const {
    if (owner_ != std::this_thread::get_id())
        throw std::logic_error("FarWorldSessionController owner-thread violation");
}
FarWorldRefusal
FarWorldSessionController::Configure(JobSystem& jobs,
                                     std::uint64_t epoch,
                                     const Producer::FrozenPolicy& policy,
                                     const FarWorldSessionQualificationLimits& limits) {
    CheckOwner();
    // Completeness precedes binding, pool admission and all heap allocation.
    if (!limits.Complete())
        return FarWorldRefusal::MissingLimits;
    if (impl_)
        return FarWorldRefusal::InvalidRequests;
    const auto bound =
        renderer_.BindSession(jobs, epoch, policy.caves, policy.numeric, limits.rendering);
    if (bound != FarWorldRefusal::None)
        return bound;
    auto pool = renderer_.SessionMemoryPool(kFarTierCount);
    if (!pool)
        return FarWorldRefusal::AdmissionFailed;
    FarVolumeMemoryLease control(*pool);
    if (!control.TryCharge(sizeof(*this)))
        return FarWorldRefusal::AdmissionFailed;
    try {
        auto state = std::allocate_shared<Impl>(FarVolumePoolAllocator<Impl>(pool), pool);
        state->limits = limits;
        state->policy = policy;
        state->epoch = epoch;
        impl_ = std::move(state);
        pool_ = std::move(pool);
        charge_ = std::move(control);
        return FarWorldRefusal::None;
    } catch (const std::bad_alloc&) {
        return FarWorldRefusal::AdmissionFailed;
    }
}
FarWorldAuthorityConfig FarWorldSessionController::SourceConfig() const {
    CheckOwner();
    if (!impl_)
        return {};
    auto source = impl_->limits.source;
    source.memory = pool_;
    return {source, impl_->limits.capture};
}
FarWorldRefusal
FarWorldSessionController::Attach(const std::shared_ptr<Producer>& producer,
                                  const std::shared_ptr<const Systems::SHIELD_WorldSystem>& world) {
    CheckOwner();
    if (!impl_)
        return FarWorldRefusal::MissingLimits;
    if (!producer || !world || impl_->producer || producer->retired() ||
        producer->frozen_active() || producer->capture_status().active)
        return FarWorldRefusal::ImmutableAuthorityUnavailable;
    const auto actual = producer->source_limits();
    const auto& expected = impl_->limits.source;
    if (actual.memory != pool_ || actual.index_records != expected.index_records ||
        actual.copy_bytes != expected.copy_bytes ||
        actual.query_columns != expected.query_columns ||
        !producer->configure_capture(impl_->limits.capture))
        return FarWorldRefusal::MissingLimits;
    impl_->producer = producer;
    impl_->world = world;
    return FarWorldRefusal::None;
}
bool FarWorldSessionController::Request(std::span<const FarTileKey> keys) {
    CheckOwner();
    if (!impl_ || !impl_->producer || keys.empty() || keys.size() > impl_->limits.tiles)
        return false;
    // Validate before replacing the one successor. Canonical sort is bounded by
    // 512 keys, never the resident world or producer index.
    for (const auto& key : keys) {
        FarVolumeTileKey volume;
        Persistence::WorldSaveService::AuthorityWindow window;
        if (!ToFarVolumeTileKey(key, volume) || !Producer::source_window(volume, window))
            return false;
    }
    std::copy(keys.begin(), keys.end(), impl_->successor.begin());
    auto end = impl_->successor.begin() + static_cast<std::ptrdiff_t>(keys.size());
    std::sort(impl_->successor.begin(), end);
    end = std::unique(impl_->successor.begin(), end);
    impl_->successor_count = static_cast<std::size_t>(end - impl_->successor.begin());
    impl_->queued = true;
    return true;
}
void FarWorldSessionController::Refuse(FarWorldRefusal refusal,
                                       const Producer::FrozenResult& failure) {
    refusal_ = refusal == FarWorldRefusal::None ? FarWorldRefusal::AdmissionFailed : refusal;
    impl_->Record(work_, refusal_, failure);
    impl_->plan.Cancel();
    if (impl_->producer)
        impl_->producer->cancel_capture();
    const bool bound = renderer_.world_epoch() == impl_->epoch &&
                       renderer_.SessionMemoryPool(kFarTierCount) == pool_;
    if (bound)
        renderer_.CancelAdmission();
    impl_->sampling.reset();
    failure_ = failure;
    phase_ = Phase::Refused;
    if (bound)
        renderer_.SetSessionRefresh(refusal_, failure_, work_);
}
void FarWorldSessionController::Begin() {
    auto& state = *impl_;
    state.plan.Reset();
    state.sampling.reset();
    work_ = {};
    state.source_operations = 0;
    state.measured_source = {};
    state.measured_samples = {};
    state.source_frames = state.preparation_frames = 0;
    state.receipt_pending = true;
    state.start_frame = state.frame.value_or(0);
    const auto& clock = state.limits.capture.clock;
    const auto now = clock.now(clock.context);
    state.start_ticks = now;
    if (state.limits.refresh_ticks > std::numeric_limits<std::uint64_t>::max() - now) {
        Refuse(FarWorldRefusal::MissingLimits, {Producer::Error::InvalidLimits});
        return;
    }
    state.deadline = {clock.now, clock.context, now + state.limits.refresh_ticks};
    state.sampling.emplace(state.limits.transaction_samples, nullptr, state.deadline);
    if (state.plan.SetWanted({state.successor.data(), state.successor_count}) !=
        Plan::Error::None) {
        Refuse(FarWorldRefusal::SpanRejected, {Producer::Error::InvalidWindow});
        return;
    }
    if (!state.producer->request_capture(state.plan.source_windows())) {
        const auto error = state.producer->capture_status().error;
        const Producer::FrozenResult failure{
            error != Producer::Error::None ? error : Producer::Error::InvalidWindow};
        Refuse(FarWorldProducerRefusal(failure), failure);
        return;
    }
    state.queued = false;
    phase_ = Phase::Source;
}
void FarWorldSessionController::PlanAdmission() {
    auto& state = *impl_;
    const auto image = state.plan.accepted();
    if (!state.sampling.has_value() || !image || image->tiles.empty() ||
        image->tiles.size() > state.limits.tiles) {
        Refuse(FarWorldRefusal::InvalidRequests);
        return;
    }
    auto& transaction = *state.sampling;
    state.pending_history = state.history;
    state.pending_count = state.history_count;
    state.tile_count = image->tiles.size();
    // At most 512 wanted and 512 retained keys; binary lookup, then one bounded
    // sort. This runs once at the planning/admission boundary, never per sample.
    for (std::size_t index = 0; index < image->tiles.size(); ++index) {
        const auto& tile = image->tiles[index];
        auto span = tile.request.span;
        const auto history_end =
            state.history.begin() + static_cast<std::ptrdiff_t>(state.history_count);
        const auto previous = std::lower_bound(
            state.history.begin(),
            history_end,
            tile.key,
            [](const Impl::Ledger& entry, const FarTileKey& key) { return entry.key < key; });
        std::uint64_t generation = 1;
        if (previous != history_end && previous->key == tile.key) {
            span.min_y = std::min(span.min_y, previous->span.min_y);
            span.max_y = std::max(span.max_y, previous->span.max_y);
            generation = previous->generation;
            if (span != previous->span) {
                if (generation == std::numeric_limits<std::uint64_t>::max()) {
                    Refuse(FarWorldRefusal::SpanRejected);
                    return;
                }
                ++generation;
            }
        }
        const auto& bounds = state.limits.rendering.spans;
        if (double(span.min_y) < bounds.min_y || double(span.max_y) > bounds.max_y ||
            double(span.max_y) - double(span.min_y) > bounds.max_extent_meters) {
            Refuse(FarWorldRefusal::SpanRejected);
            return;
        }
        if (previous != history_end && previous->key == tile.key) {
            state.pending_history[static_cast<std::size_t>(previous - state.history.begin())] = {
                tile.key, span, generation};
        } else {
            if (state.pending_count == state.pending_history.size()) {
                Refuse(FarWorldRefusal::SpanHistoryExhausted);
                return;
            }
            state.pending_history[state.pending_count++] = {tile.key, span, generation};
        }
        auto capture = tile.request;
        capture.span = span;
        state.tiles[index] = {{{state.epoch,
                                0,
                                static_cast<std::uint8_t>(state.policy.caves),
                                tile.key,
                                image->committed_generation,
                                generation},
                               // Coverage intents have an inclusive high endpoint; keep the
                               // discovered half-open span without growing its top brick.
                               {capture.key,
                                FarVolumeCoverageSpan{static_cast<float>(span.min_y),
                                                      static_cast<float>(span.max_y - 1)}},
                               state.limits.rendering.build_headroom_bytes,
                               {},
                               {}},
                              capture};
    }
    std::sort(
        state.pending_history.begin(),
        state.pending_history.begin() + static_cast<std::ptrdiff_t>(state.pending_count),
        [](const Impl::Ledger& left, const Impl::Ledger& right) { return left.key < right.key; });
    auto sampling = state.limits.sampling;
    sampling.work = &transaction;
    sampling.deadline = state.deadline;
    const auto result = renderer_.StartSessionTarget(state.producer,
                                                     state.world,
                                                     {state.tiles.data(), state.tile_count},
                                                     state.policy,
                                                     sampling,
                                                     state.limits.transaction_samples);
    if (result != FarHandoffAdmissionStatus::Pending) {
        Refuse(renderer_.SessionAdmissionRefusal(), renderer_.SessionCaptureFailure());
        return;
    }
    phase_ = Phase::Admission;
}
FarWorldSessionController::Phase FarWorldSessionController::Pump(std::uint64_t frame) {
    CheckOwner();
    if (!impl_ || !impl_->producer)
        return phase_;
    auto& state = *impl_;
    if (state.frame && frame <= *state.frame)
        return phase_;
    state.frame = frame;
    if (renderer_.world_epoch() != state.epoch || state.producer->retired() ||
        renderer_.SessionMemoryPool(kFarTierCount) != pool_) {
        Refuse(FarWorldRefusal::ImmutableAuthorityUnavailable,
               {Producer::Error::AuthorityUnavailable});
        return phase_;
    }
    // Dirty publication queues at most the last wanted set. It does not pump,
    // tick, replace source images or disturb either active sampling cursor.
    state.queued = state.queued || state.producer->needs_capture_boundary();
    if (phase_ == Phase::Idle || phase_ == Phase::Complete || phase_ == Phase::Refused) {
        if (!state.queued)
            return phase_;
        RetireIdleHistory();
        Begin();
    }
    if (phase_ == Phase::Refused)
        return phase_;
    if (frame - state.start_frame >= state.limits.refresh_frames || state.deadline.expired()) {
        Refuse(FarWorldRefusal::AdmissionFailed, {Producer::Error::DeadlineExceeded});
        return phase_;
    }
    if (phase_ != Phase::Source && phase_ != Phase::Waiting) {
        const auto status = state.producer->capture_status();
        if (status.error != Producer::Error::None || status.paused) {
            const Producer::FrozenResult failure{status.error != Producer::Error::None
                                                     ? status.error
                                                     : Producer::Error::AuthorityUnavailable};
            Refuse(FarWorldProducerRefusal(failure), failure);
            return phase_;
        }
        if (const auto* source = state.producer->capture(); source && source->deadline.expired()) {
            Refuse(FarWorldRefusal::AdmissionFailed, {Producer::Error::SourceReadExpired});
            return phase_;
        }
    }
    if (phase_ != Phase::Waiting)
        ++state.preparation_frames;
    if (phase_ == Phase::Source) {
        ++state.source_frames;
        const auto& clock = state.limits.capture.clock;
        const auto before = clock.now(clock.context);
        const auto copied = state.producer->counters().copied_bytes;
        // Source assembly shares the aggregate admission allowance. Each
        // synchronous read remains independently bounded by the injected read
        // limits; evidence/record merging is resumable one entry per unit.
        const auto remaining = state.limits.admission_operations - state.source_operations;
        if (!remaining) {
            Refuse(FarWorldRefusal::AdmissionFailed,
                   {Producer::Error::None, FarFrozenCaptureError::WorkLimit});
            return phase_;
        }
        state.source_operations +=
            state.producer->pump_capture(std::min(remaining, state.limits.operations_per_pump));
        const auto after = clock.now(clock.context);
        work_.source_copied_bytes += state.producer->counters().copied_bytes - copied;
        const auto progress = state.producer->capture_progress();
        state.measured_source = progress;
        work_.source_read_bytes = progress.compressed_bytes;
        work_.source_decode_work = progress.decoded_bytes;
        work_.source_ms += after >= before ? double(after - before) * 1000.0 /
                                                 double(state.limits.ticks_per_second)
                                           : 0;
        const auto status = state.producer->capture_status();
        if (status.active && status.error == Producer::Error::None && !status.paused) {
            if (state.deadline.expired()) {
                Refuse(FarWorldRefusal::AdmissionFailed, {Producer::Error::DeadlineExceeded});
                return phase_;
            }
            renderer_.SetSessionRefresh(refusal_, failure_, work_);
            return phase_;
        }
        if (status.error != Producer::Error::None || status.paused || !state.producer->capture()) {
            const Producer::FrozenResult failure{status.error != Producer::Error::None
                                                     ? status.error
                                                     : Producer::Error::AuthorityUnavailable};
            Refuse(FarWorldProducerRefusal(failure), failure);
            return phase_;
        }
        if (!state.sampling.has_value()) {
            Refuse(FarWorldRefusal::MissingLimits, {Producer::Error::InvalidLimits});
            return phase_;
        }
        auto sampling = state.limits.sampling;
        sampling.work = &*state.sampling;
        sampling.deadline = state.deadline;
        const auto started = state.plan.Start(
            state.producer,
            state.world,
            state.policy,
            sampling,
            {state.limits.tiles, state.limits.planning_operations, state.limits.rendering.spans});
        if (!started.ok()) {
            const auto failure = PlanFailure(started);
            Refuse(FarWorldProducerRefusal(failure), failure);
            return phase_;
        }
        phase_ = Phase::Planning;
    } else if (phase_ == Phase::Planning) {
        const auto result = state.plan.Pump(state.limits.operations_per_pump);
        work_.capture_work += result.work;
        if (!result.ok()) {
            const auto failure = PlanFailure(result);
            Refuse(FarWorldProducerRefusal(failure), failure);
            return phase_;
        }
        if (result.complete)
            PlanAdmission();
    } else if (phase_ == Phase::Admission) {
        const auto remaining =
            state.limits.admission_operations - state.source_operations - work_.admission_work;
        if (!remaining) {
            Refuse(FarWorldRefusal::AdmissionFailed,
                   {Producer::Error::None, FarFrozenCaptureError::WorkLimit});
            return phase_;
        }
        const auto result =
            renderer_.PumpAdmission(std::min(remaining, state.limits.operations_per_pump));
        work_.admission_work += result.work;
        work_.admission_height_observations += result.height_observations;
        if (result.status == FarHandoffAdmissionStatus::Refused) {
            Refuse(renderer_.SessionAdmissionRefusal(), renderer_.SessionCaptureFailure());
            return phase_;
        }
        if (result.status == FarHandoffAdmissionStatus::Complete) {
            state.history = state.pending_history;
            state.history_count = state.pending_count;
            state.plan.Reset();
            phase_ = Phase::Waiting;
            refusal_ = FarWorldRefusal::None;
            failure_ = {};
            RetireIdleHistory();
        } else if (result.status != FarHandoffAdmissionStatus::Pending) {
            Refuse(FarWorldRefusal::ImmutableAuthorityUnavailable,
                   {Producer::Error::AuthorityUnavailable});
            return phase_;
        }
    } else if (phase_ == Phase::Waiting) {
        const auto* diagnostic = renderer_.SessionDiagnostic();
        if (diagnostic && FarWorldHandoffTargetReady(*diagnostic))
            phase_ = Phase::Complete;
    }
    if (phase_ != Phase::Refused && state.deadline.expired()) {
        Refuse(FarWorldRefusal::AdmissionFailed, {Producer::Error::DeadlineExceeded});
        return phase_;
    }
    if (phase_ == Phase::Complete)
        state.Record(work_, FarWorldRefusal::None, {});
    // This constant-size projection includes synchronous read/copy elapsed cost.
    renderer_.SetSessionRefresh(refusal_, failure_, work_);
    return phase_;
}
std::size_t FarWorldSessionController::RetireIdleHistory() {
    CheckOwner();
    if (!impl_ || renderer_.world_epoch() != impl_->epoch ||
        renderer_.SessionMemoryPool(kFarTierCount) != pool_ || phase_ == Phase::Planning ||
        phase_ == Phase::Admission ||
        renderer_.AdmissionStatus() == FarHandoffAdmissionStatus::Pending)
        return 0;
    const auto retired = renderer_.RetireIdleHistory();
    const auto* diagnostic = renderer_.SessionDiagnostic();
    if (!diagnostic || !diagnostic->captured || !diagnostic->complete ||
        diagnostic->tile_count > impl_->retained.size())
        return retired;
    auto& state = *impl_;
    for (std::size_t index = 0; index < diagnostic->tile_count; ++index)
        state.retained[index] = diagnostic->tiles[index].key;
    const auto end = state.retained.begin() + static_cast<std::ptrdiff_t>(diagnostic->tile_count);
    std::sort(state.retained.begin(), end);
    std::size_t kept = 0;
    for (std::size_t index = 0; index < state.history_count; ++index) {
        if (std::binary_search(state.retained.begin(), end, state.history[index].key))
            state.history[kept++] = state.history[index];
    }
    state.history_count = kept;
    return retired;
}
bool FarWorldSessionController::Retains(const FarTileKey& key) const {
    CheckOwner();
    if (!impl_)
        return false;
    const auto end = impl_->history.begin() + static_cast<std::ptrdiff_t>(impl_->history_count);
    const auto found = std::lower_bound(
        impl_->history.begin(), end, key, [](const Impl::Ledger& entry, const FarTileKey& item) {
            return entry.key < item;
        });
    return found != end && found->key == key;
}
void FarWorldSessionController::Cancel() {
    CheckOwner();
    if (impl_) {
        impl_->Record(work_,
                      FarWorldRefusal::ImmutableAuthorityUnavailable,
                      {Producer::Error::AuthorityUnavailable});
        impl_->plan.Cancel();
        if (impl_->producer)
            impl_->producer->cancel_capture();
        if (renderer_.world_epoch() == impl_->epoch &&
            renderer_.SessionMemoryPool(kFarTierCount) == pool_)
            renderer_.CancelAdmission();
        impl_.reset();
    }
    charge_ = {};
    pool_.reset();
    phase_ = Phase::Idle;
    refusal_ = FarWorldRefusal::None;
    failure_ = {};
    work_ = {};
}
FarWorldSessionController::Phase FarWorldSessionController::phase() const {
    CheckOwner();
    return phase_;
}
const FarWorldRefreshWork& FarWorldSessionController::work() const {
    CheckOwner();
    return work_;
}
Producer::FrozenResult FarWorldSessionController::failure() const {
    CheckOwner();
    return failure_;
}
} // namespace Luminumbra::Rendering
