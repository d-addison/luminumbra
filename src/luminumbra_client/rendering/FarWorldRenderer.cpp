#include "FarWorldRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Luminumbra::Rendering {
using namespace World;

FarWorldSelection SelectFarWorld(FarWorldMode requested,
                                 FarWorldPurpose purpose,
                                 FarWorldRefusal prerequisite,
                                 bool enabled) noexcept {
    FarWorldSelection result;
    result.requested = requested;
    if (requested == FarWorldMode::Ladder) {
        result.refusal = prerequisite;
        if (prerequisite == FarWorldRefusal::None)
            result.effective = FarWorldMode::Ladder;
    }
    if (purpose != FarWorldPurpose::World) {
        result.effective = FarWorldMode::Legacy;
        result.refusal = purpose == FarWorldPurpose::Preview ? FarWorldRefusal::PreviewOverride
                                                             : FarWorldRefusal::OffscreenSuppressed;
    }
    result.advance = enabled && purpose != FarWorldPurpose::OffscreenPreview;
    result.submit = result.advance;
    return result;
}

FarWorldSubmitStats SubmitSelectedFarWorld(const FarWorldSelection& selection,
                                           const FarWorldLegacySubmitFn& legacy,
                                           const FarWorldSubmitFn& ladder,
                                           const FarWorldPrepareDraw& prepare) {
    if (!selection.submit)
        return {};
    if (selection.effective == FarWorldMode::Legacy)
        return legacy ? legacy() : FarWorldSubmitStats{};
    return ladder ? ladder(prepare) : FarWorldSubmitStats{};
}

namespace {
// A wanted scan is at most 8 cells per axis per tier: R/E <= 2+1/32,
// plus the two tangent guards. This also bounds ComputeFarWantedSet's vector.
static_assert([] {
    for (const auto& tier : kFarTierTable)
        if (2u * (tier.outer_radius_meters + tier.brick_edge_meters) >= 5u * tier.tile_edge_meters)
            return false;
    return kFarTierCount * 8u * 8u <= 512u;
}());
bool CompleteQueue(const FarVolumeQueueLimits& queue) noexcept {
    const auto& compilation = queue.compilation;
    return queue.descriptor_slots > 0 && queue.descriptor_slots <= 512 && queue.result_slots > 0 &&
           queue.result_slots <= 512 && compilation.authority.max_height_samples > 0 &&
           compilation.authority.max_density_samples > 0 &&
           compilation.generation.max_candidate_bricks > 0 &&
           compilation.generation.max_surface_bricks > 0 && compilation.mesh.max_cells > 0 &&
           compilation.mesh.max_sample_evaluations > 0 && compilation.mesh.max_vertices > 0 &&
           compilation.mesh.max_indices > 0 && queue.max_converted_bytes > 0;
}
} // namespace

bool FarWorldExperimentalLimits::Complete() const noexcept {
    if (!frame.builds || !frame.uploads || !frame.upload_bytes || !max_fixture_bytes ||
        !build_headroom_bytes || !std::isfinite(spans.min_y) || !std::isfinite(spans.max_y) ||
        !std::isfinite(spans.max_extent_meters) || spans.min_y >= spans.max_y ||
        spans.max_extent_meters <= 0 || max_fixture_bytes > 256ull * 1024 * 1024 ||
        frame.builds > 512 || frame.uploads > 512 ||
        frame.upload_bytes == std::numeric_limits<std::uint64_t>::max())
        return false;
    for (std::size_t i = 0; i < kFarTierCount; ++i) {
        if (!budgets.bytes[i] || budgets.bytes[i] == std::numeric_limits<std::uint64_t>::max() ||
            !residency.entry_slots[i] || residency.entry_slots[i] > 512 ||
            residency.entry_slots[i] > residency.queues[i].descriptor_slots ||
            !CompleteQueue(residency.queues[i]))
            return false;
    }
    return true;
}

FarWorldExperimentalLimits FarWorldDevelopmentLimits() noexcept {
    FarWorldExperimentalLimits limits;
    limits.budgets.bytes.fill(256ull * 1024 * 1024);
    limits.residency.entry_slots.fill(64);
    for (auto& queue : limits.residency.queues)
        queue = {64,
                 2,
                 {{131 * 131, 20'000'000},
                  {131'072, 16'384},
                  {1'048'576, 3'000'000, 300'000, 1'000'000}},
                 16 * 1024 * 1024};
    limits.frame = {2, 2, 16 * 1024 * 1024};
    limits.spans = {-8192, 8192, 2048};
    limits.max_fixture_bytes = 8 * 1024 * 1024;
    limits.build_headroom_bytes = 64 * 1024 * 1024;
    return limits;
}

FarWorldRenderer::FarWorldRenderer(FarWorldMode requested)
    : requested_(requested) {}
FarWorldRenderer::~FarWorldRenderer() {
    Shutdown();
}
void FarWorldRenderer::CheckOwner() const {
    if (std::this_thread::get_id() != owner_ || submitting_)
        throw std::logic_error("FarWorldRenderer owner-thread/re-entry violation");
}
void FarWorldRenderer::SetRequested(FarWorldMode requested) {
    CheckOwner();
    if (requested_ == requested)
        return;
    Drain();
    requested_ = requested;
}
void FarWorldRenderer::InstallBackend(std::unique_ptr<FarVolumeGpuDriver> driver) {
    CheckOwner();
    Drain();
    driver_ = std::move(driver);
}
FarWorldRefusal
FarWorldRenderer::BindFixture(JobSystem& jobs,
                              std::shared_ptr<const FarWorldPristineFixture> fixture,
                              std::uint64_t world_epoch,
                              std::uint64_t bake_generation,
                              const FarWorldExperimentalLimits& limits) {
    CheckOwner();
    Drain();
    if (!limits.Complete())
        return refusal_ = FarWorldRefusal::MissingLimits;
    if (!fixture || !world_epoch || !fixture->content_identity() ||
        (fixture->caves() != FarCaveMode::BandLimited &&
         fixture->caves() != FarCaveMode::BoxFiltered) ||
        (fixture->numeric() != FarVolumeNumericProfile::RawFloat &&
         fixture->numeric() != FarVolumeNumericProfile::LegacySigned16))
        return refusal_ = FarWorldRefusal::ImmutableAuthorityUnavailable;
    if (!driver_ || !driver_->ValidContext())
        return refusal_ = FarWorldRefusal::BackendUnavailable;
    if (fixture->owned_bytes() > limits.max_fixture_bytes)
        return refusal_ = FarWorldRefusal::AdmissionFailed;
    try {
        memory_ = std::make_unique<FarVolumeTierMemory>(limits.budgets);
        integration_charge_.emplace(*memory_->Pool(kFarTierCount));
        // Fixed ledgers, transactional copies, ordering/wanted scratch, owner
        // objects and the immutable fixture are charged in addition to residency.
        const auto bytes = sizeof(*this) + sizeof(FarVolumeResidency) +
                           sizeof(FarVolumeTierMemory) + kScratchSlots * sizeof(FarTileKey) +
                           fixture->owned_bytes();
        if (!integration_charge_->TryCharge(bytes))
            throw std::bad_alloc();
        residency_ = std::make_unique<FarVolumeResidency>(jobs, *memory_, limits.residency);
        fixture_ = std::move(fixture);
        limits_ = limits;
        world_epoch_ = world_epoch;
        bake_generation_ = bake_generation;
        stopping_ = false;
        return refusal_ = FarWorldRefusal::None;
    } catch (const std::bad_alloc&) {
        Drain();
        return refusal_ = FarWorldRefusal::AdmissionFailed;
    }
}
FarWorldSelection FarWorldRenderer::Selection(FarWorldPurpose purpose,
                                              bool enabled) const noexcept {
    auto prerequisite = refusal_;
    if (prerequisite == FarWorldRefusal::None && (!driver_ || !driver_->ValidContext()))
        prerequisite = FarWorldRefusal::BackendUnavailable;
    return SelectFarWorld(requested_, purpose, prerequisite, enabled);
}

FarVolumeAuthorityProducer FarWorldRenderer::Producer() const {
    // Captures only the owned immutable fixture. No world pointer or owner-thread
    // callback is reachable from the asynchronous authority.
    return [fixture = fixture_](const FarVolumeWantedRequest&, FarVolumeMemoryPool&) {
        return FarVolumeAuthority{
            fixture->content_identity(),
            fixture->caves(),
            fixture->numeric(),
            [fixture](std::int64_t x, std::int64_t z) { return fixture->Height(x, z); },
            [fixture](const FarVolumePosition& p, float h, std::uint32_t s, FarCaveMode caves) {
                return fixture->Sample(p, h, s, caves);
            }};
    };
}

FarWorldRefusal FarWorldRenderer::MakeRequests(const std::array<double, 3>& camera,
                                               std::span<const FarVolumeCoverageSpan> extensions,
                                               std::size_t& count) {
    if (extensions.size() > 16 || !std::isfinite(camera[1]) ||
        std::abs(camera[1]) > kFarWantedCameraLimitMeters)
        return FarWorldRefusal::SpanRejected;
    // The fixed tier table bounds the scan to fewer than 512 candidates. Reserve
    // the complete charged scratch so ComputeFarWantedSet never grows its buffer.
    std::vector<FarTileKey> wanted;
    wanted.reserve(kScratchSlots);
    if (ComputeFarWantedSet(camera[0], camera[2], wanted) != FarWantedError::None ||
        wanted.size() > kScratchSlots)
        return FarWorldRefusal::SpanRejected;
    count = 0;
    for (const auto& key : wanted) {
        const auto outcome =
            ExtendLedger(key, static_cast<float>(camera[1]), extensions, requests_[count]);
        if (outcome != FarWorldRefusal::None)
            return outcome;
        ++count;
    }
    return FarWorldRefusal::None;
}

FarWorldRefusal FarWorldRenderer::ExtendLedger(const FarTileKey& key,
                                               float camera_y,
                                               std::span<const FarVolumeCoverageSpan> extensions,
                                               FarVolumeWantedRequest& request) const {
    const auto old = std::find_if(ledgers_.begin(), ledgers_.end(), [&key](const auto& l) {
        return l.accepted && l.accepted->identity.key == key;
    });
    FarVolumeSpanLedger span;
    if (old != ledgers_.end())
        span = old->span;
    else if (!span.Reset(key))
        return FarWorldRefusal::SpanRejected;
    const auto previous_generation = span.generation();
    if (!span.Extend({camera_y, camera_y}, limits_.spans))
        return FarWorldRefusal::SpanRejected;
    for (const auto& extension : extensions)
        if (!span.Extend(extension, limits_.spans))
            return FarWorldRefusal::SpanRejected;
    const auto intent = span.Intent();
    if (!intent)
        return FarWorldRefusal::SpanRejected;
    const auto growth = span.generation() - previous_generation;
    const auto* previous = old != ledgers_.end() ? &*old : nullptr;
    const auto generation =
        previous && previous->accepted ? previous->accepted->identity.span_generation : 0;
    if (growth > std::numeric_limits<std::uint64_t>::max() - generation)
        return FarWorldRefusal::SpanRejected;
    request = {{world_epoch_,
                fixture_->content_identity(),
                static_cast<std::uint8_t>(fixture_->caves()),
                key,
                bake_generation_,
                generation + growth},
               *intent,
               previous && previous->accepted && growth == 0
                   ? previous->accepted->build_headroom_bytes
                   : limits_.build_headroom_bytes};
    return FarWorldRefusal::None;
}

bool FarWorldRenderer::ValidRequest(const FarVolumeWantedRequest& request,
                                    FarVolumeSpanLedger& span) const {
    if (request.identity.world_epoch != world_epoch_ ||
        request.identity.content_hash != fixture_->content_identity() ||
        request.identity.cave_policy != static_cast<std::uint8_t>(fixture_->caves()) ||
        request.identity.bake_generation != bake_generation_ ||
        request.build_headroom_bytes > limits_.build_headroom_bytes)
        return false;
    return span.Reset(request.identity.key) &&
           (!request.intent.extra_span || span.Extend(*request.intent.extra_span, limits_.spans)) &&
           span.Intent() == request.intent;
}

FarWorldRefusal
FarWorldRenderer::ValidateAndRecordRequests(std::span<const FarVolumeWantedRequest> requests) {
    // Both entry points stage the complete manifest here. Residency validates
    // its capacities/duplicates before mutation; history commits only after
    // ReplaceWanted succeeds. Fixed staging storage is charged at binding.
    pending_ledgers_ = ledgers_;
    for (const auto& request : requests) {
        FarVolumeSpanLedger span;
        if (!ValidRequest(request, span))
            return FarWorldRefusal::InvalidRequests;
        auto slot =
            std::find_if(pending_ledgers_.begin(), pending_ledgers_.end(), [&](const auto& l) {
                return l.accepted && l.accepted->identity.key == request.identity.key;
            });
        if (slot != pending_ledgers_.end()) {
            const auto& accepted = slot->accepted;
            if (!accepted)
                return FarWorldRefusal::InvalidRequests;
            const auto& old = *accepted;
            if (request.identity.span_generation < old.identity.span_generation ||
                (request.identity == old.identity &&
                 (request.intent != old.intent ||
                  request.build_headroom_bytes != old.build_headroom_bytes)))
                return FarWorldRefusal::InvalidRequests;
            if (old.intent.extra_span &&
                (!request.intent.extra_span ||
                 request.intent.extra_span->min_y > old.intent.extra_span->min_y ||
                 request.intent.extra_span->max_y < old.intent.extra_span->max_y))
                return FarWorldRefusal::SpanRejected;
        } else {
            slot = std::find_if(pending_ledgers_.begin(),
                                pending_ledgers_.end(),
                                [](const auto& l) { return !l.accepted; });
            if (slot == pending_ledgers_.end())
                return FarWorldRefusal::SpanHistoryExhausted;
        }
        *slot = {request, span};
    }
    return FarWorldRefusal::None;
}

bool FarWorldRenderer::PrepareFrame(std::uint64_t frame,
                                    const std::array<double, 3>& camera,
                                    std::span<const FarVolumeCoverageSpan> extensions) {
    CheckOwner();
    if (!residency_ || Selection(FarWorldPurpose::World).effective != FarWorldMode::Ladder)
        return false;
    if (advanced_frame_ && frame <= *advanced_frame_)
        return false;
    std::size_t count = 0;
    try {
        const auto outcome = MakeRequests(camera, extensions, count);
        if (outcome != FarWorldRefusal::None) {
            refusal_ = outcome;
            return false;
        }
    } catch (const std::bad_alloc&) {
        refusal_ = FarWorldRefusal::AdmissionFailed;
        return false;
    }
    return AdvanceFrame(frame, camera, std::span(requests_.data(), count));
}

bool FarWorldRenderer::AdvanceFrame(std::uint64_t frame,
                                    const std::array<double, 3>& camera,
                                    std::span<const FarVolumeWantedRequest> requests) {
    CheckOwner();
    if (!residency_ || stopping_ ||
        Selection(FarWorldPurpose::World).effective != FarWorldMode::Ladder ||
        (advanced_frame_ && frame <= *advanced_frame_))
        return false;
    advanced_frame_ = frame;
    if (requests.size() > kScratchSlots || !std::all_of(camera.begin(), camera.end(), [](double v) {
            return std::isfinite(v) && std::abs(v) <= kFarWantedCameraLimitMeters;
        })) {
        refusal_ = FarWorldRefusal::InvalidRequests;
        return false;
    }
    const auto outcome = ValidateAndRecordRequests(requests);
    if (outcome != FarWorldRefusal::None) {
        refusal_ = outcome;
        return false;
    }
    if (!residency_->ReplaceWanted(requests)) {
        refusal_ = FarWorldRefusal::InvalidRequests;
        return false;
    }
    ledgers_ = pending_ledgers_;
    last_frame_ = residency_->AdvanceFrame(
        camera[0], camera[2], limits_.frame, Producer(), [this](const auto& mesh) {
            return UploadFarVolumeGpu(mesh, *driver_);
        });
    return true;
}

bool MakeFarWorldDrawCommand(const FarVolumeDrawRecord& record,
                             const std::array<double, 3>& camera,
                             std::int32_t index_count,
                             FarWorldDrawCommand& command) noexcept {
    if (!record.resource || index_count <= 0 || index_count % 3 != 0 ||
        record.origin_meters.y != record.request.span.min_y)
        return false;
    const std::array<double, 3> origin{static_cast<double>(record.origin_meters.x),
                                       static_cast<double>(record.origin_meters.y),
                                       static_cast<double>(record.origin_meters.z)};
    FarWorldDrawCommand candidate;
    for (std::size_t i = 0; i < origin.size(); ++i) {
        if (!std::isfinite(camera[i]) ||
            std::abs(origin[i]) > kFarVolumeExactCoordinateLimitMeters ||
            std::abs(camera[i]) > kFarWantedCameraLimitMeters)
            return false;
        candidate.relative_origin[i] = static_cast<float>(origin[i] - camera[i]);
        candidate.world_origin[i] = static_cast<float>(origin[i]);
    }
    FarWorldCellOrigin cell_origin;
    if (!MakeFarWorldCellOrigin(origin, cell_origin))
        return false;
    candidate.origin_chunk = {cell_origin.key.x, cell_origin.key.y, cell_origin.key.z};
    candidate.origin_remainder = cell_origin.remainder;
    candidate.tile_edge_meters = static_cast<float>(FarTileEdgeMeters(record.identity.key.tier));
    candidate.arrival_eligible = record.arrival.eligible;
    candidate.finer_outer_radius_meters =
        static_cast<float>(record.arrival.finer_outer_radius_meters);
    for (std::size_t i = 0; i < record.arrival.children.size(); ++i) {
        const auto& child = record.arrival.children[i];
        if (std::abs(static_cast<double>(child.min_y)) > kFarVolumeExactCoordinateLimitMeters ||
            std::abs(static_cast<double>(child.max_y)) > kFarVolumeExactCoordinateLimitMeters)
            return false;
        // Both integer endpoints and the origin lie in +/-2^23 metres, so their
        // differences are integers within +/-2^24 and exactly representable.
        candidate.child_local_y_spans[i] = {
            static_cast<float>(static_cast<double>(child.min_y) - origin[1]),
            static_cast<float>(static_cast<double>(child.max_y) - origin[1])};
    }
    candidate.index_count = index_count;
    candidate.horizontal_outer_clip_meters = record.horizontal_outer_clip_meters;
    candidate.view_ray_bias_meters = record.view_ray_bias_meters;
    command = candidate;
    return true;
}

FarWorldSubmitStats FarWorldRenderer::Submit(const std::array<double, 3>& camera,
                                             const FarWorldPrepareDraw& prepare) {
    CheckOwner();
    FarWorldSubmitStats stats;
    if (!residency_ || stopping_ || !prepare ||
        Selection(FarWorldPurpose::World).effective != FarWorldMode::Ladder)
        return stats;
    if (!ownership_.complete() || !advanced_frame_ || ownership_.frame() != *advanced_frame_) {
        ++stats.refused;
        return stats;
    }
    if (!ownership_gpu_charge_) {
        auto charge = memory_->ReserveGpu(kFarTierCount, kFarWorldOwnershipGpuBytes);
        if (!charge) {
            ++stats.refused;
            return stats;
        }
        ownership_gpu_charge_.emplace(std::move(*charge));
    }
    if (!driver_->BindLiveOwnership(ownership_.cells())) {
        driver_->ReleaseLiveOwnership();
        ownership_gpu_charge_.reset();
        ++stats.refused;
        return stats;
    }
    struct Visit {
        FarWorldRenderer& owner;
        const std::array<double, 3>& camera;
        const FarWorldPrepareDraw& prepare;
        FarWorldSubmitStats& stats;
    } context{*this, camera, prepare, stats};
    submitting_ = true;
    try {
        residency_->VisitDraws(
            [](void* opaque, const FarVolumeDrawRecord& record) {
                auto& v = *static_cast<Visit*>(opaque);
                const auto* allocation =
                    dynamic_cast<const FarVolumeGlAllocation*>(record.resource);
                FarWorldDrawCommand command;
                if (!allocation || !MakeFarWorldDrawCommand(
                                       record, v.camera, allocation->index_count(), command)) {
                    ++v.stats.refused;
                    return;
                }
                command.live_cell_count =
                    static_cast<std::int32_t>(v.owner.ownership_.cells().size());
                if (!v.prepare(command) || !DrawFarVolumeGpu(*record.resource, *v.owner.driver_)) {
                    ++v.stats.refused;
                    return;
                }
                ++v.stats.draws;
                v.stats.indices += static_cast<std::size_t>(command.index_count);
            },
            &context);
    } catch (...) {
        driver_->UnbindLiveOwnership();
        submitting_ = false;
        throw;
    }
    driver_->UnbindLiveOwnership();
    submitting_ = false;
    return stats;
}

void FarWorldRenderer::Drain() {
    stopping_ = true;
    // Stop submission and unbind before any queue mutation or GPU retirement.
    // Drawing restores the caller's bindings, but retirement also handles a
    // caller that inspected/bound one of the allocation's native objects.
    if (residency_ && driver_ && driver_->ValidContext()) {
        driver_->BindVertexArray(0);
        driver_->BindBuffer(FarVolumeBufferKind::Vertex, 0);
    }
    // Drain every callback BEFORE releasing immutable authority and charges.
    if (residency_)
        residency_->CancelAndDrain();
    residency_.reset();
    if (driver_)
        driver_->ReleaseLiveOwnership();
    fixture_.reset();
    ownership_gpu_charge_.reset();
    ownership_.Begin(0);
    integration_charge_.reset();
    memory_.reset();
    ledgers_ = {};
    pending_ledgers_ = {};
    requests_ = {};
    advanced_frame_.reset();
    last_frame_ = {};
    world_epoch_ = 0;
    refusal_ = FarWorldRefusal::ImmutableAuthorityUnavailable;
}
void FarWorldRenderer::PrepareWorldSwap() {
    CheckOwner();
    Drain();
}
void FarWorldRenderer::Shutdown() {
    CheckOwner();
    Drain();
    driver_.reset();
}

} // namespace Luminumbra::Rendering
