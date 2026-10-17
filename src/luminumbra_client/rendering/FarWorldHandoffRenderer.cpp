#include "FarWorldHandoffRenderer.h"
#include "core/JobSystem.h"
#include "luminumbra_client/rendering/FarVolumeGpuBackend.h"
#include "luminumbra_client/rendering/FarVolumeHandoff.h"
#include "luminumbra_client/rendering/FarVolumeResidency.h"
#include "luminumbra_client/rendering/FarWorldHandoffDiagnostics.h"
#include "luminumbra_client/rendering/FarWorldOwnership.h"
#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_client/rendering/FarWorldSelection.h"
#include "world/FarTierTable.h"
#include "world/FarVolume.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeTierMemory.h"
#include "world/FarWantedSet.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
class HandoffMemoryDomain {
public:
    explicit HandoffMemoryDomain(const World::FarTierBudgets& budgets)
        : memory_(budgets)
        , charge_(*memory_.Pool(World::kFarTierCount)) {
        if (!charge_.TryCharge(sizeof(*this) + 2 * sizeof(void*)))
            throw World::FarVolumeMemoryRefused();
    }
    World::FarVolumeTierMemory& Memory() noexcept {
        return memory_;
    }

private:
    World::FarVolumeTierMemory memory_;
    World::FarVolumeMemoryLease charge_;
};
} // namespace
class FarWorldHandoffRenderer::Guard {
public:
    explicit Guard(FarWorldHandoffRenderer& owner)
        : owner_(owner) {
        owner.CheckOwner();
        owner.mutating_ = true;
    }
    ~Guard() {
        owner_.mutating_ = false;
    }

private:
    FarWorldHandoffRenderer& owner_;
};
FarWorldHandoffRenderer::~FarWorldHandoffRenderer() {
    try {
        Shutdown();
    } catch (...) {
        std::terminate();
    }
}
void FarWorldHandoffRenderer::CheckOwner() const {
    if (std::this_thread::get_id() != owner_ || submitting_ || mutating_)
        throw std::logic_error("Far-world handoff ownership/reentrancy");
}
void FarWorldHandoffRenderer::Unbind() noexcept {
    if (driver_ && driver_->ValidContext()) {
        driver_->UnbindLiveOwnership();
        driver_->BindVertexArray(0);
        driver_->BindBuffer(FarVolumeBufferKind::Vertex, 0);
    }
}
void FarWorldHandoffRenderer::InstallBackend(std::unique_ptr<FarVolumeGpuDriver> driver) {
    Guard const guard(*this);
    Drain();
    driver_ = std::move(driver);
}
bool FarWorldHandoffRenderer::Bind(JobSystem& jobs,
                                   std::uint64_t epoch,
                                   World::FarCaveMode caves,
                                   World::FarVolumeNumericProfile numeric,
                                   const World::FarTierBudgets& budgets,
                                   const FarVolumeResidencyLimits& limits,
                                   std::pmr::memory_resource* upstream) {
    Guard const guard(*this);
    if (handoff_ || !driver_ || !driver_->ValidContext() || epoch == 0 ||
        (caves != World::FarCaveMode::BandLimited && caves != World::FarCaveMode::BoxFiltered) ||
        (numeric != World::FarVolumeNumericProfile::RawFloat &&
         numeric != World::FarVolumeNumericProfile::LegacySigned16))
        return false;
    for (const auto bytes : budgets.bytes)
        if (bytes == 0)
            return false;
    // Preflight the bootstrap domain before allocating its pools/control block.
    if (budgets.bytes.back() <
        sizeof(HandoffMemoryDomain) + 2 * sizeof(void*) + sizeof(*this) + sizeof(FarVolumeHandoff))
        return false;
    try {
        auto domain = std::make_shared<HandoffMemoryDomain>(budgets);
        memory_ = std::shared_ptr<World::FarVolumeTierMemory>(domain, &domain->Memory());
        auto& charge = charge_.emplace(*memory_->Pool(World::kFarTierCount));
        if (!charge.TryCharge(sizeof(*this)))
            throw World::FarVolumeMemoryRefused();
        World::FarVolumeMemoryLease control(*memory_->Pool(World::kFarTierCount));
        if (!control.TryCharge(sizeof(FarVolumeHandoff)))
            throw World::FarVolumeMemoryRefused();
        handoff_ = std::make_unique<FarVolumeHandoff>(
            jobs, *memory_, limits, std::move(control), upstream);
    } catch (...) {
        Drain();
        return false;
    }
    world_epoch_ = epoch;
    caves_ = caves;
    numeric_ = numeric;
    return true;
}
std::shared_ptr<World::FarVolumeMemoryPool>
FarWorldHandoffRenderer::MemoryPool(std::size_t tier) const {
    CheckOwner();
    return memory_ && memory_->Pool(tier)
               ? std::shared_ptr<World::FarVolumeMemoryPool>(memory_, memory_->Pool(tier))
               : nullptr;
}
bool FarWorldHandoffRenderer::StageTarget(const FarHandoffManifest& manifest) {
    Guard const guard(*this);
    if (!handoff_ || manifest.world_epoch != world_epoch_ || manifest.caves != caves_ ||
        manifest.numeric != numeric_)
        return false;
    Unbind();
    if (!handoff_->StageTarget(manifest))
        return false;
    const auto upload = diagnostic_.upload;
    CaptureFarWorldHandoffDiagnostic(*handoff_, frame_.value_or(0), upload, diagnostic_);
    return true;
}
bool FarWorldHandoffRenderer::AdvanceFrame(std::uint64_t frame,
                                           const std::array<double, 3>& camera,
                                           const FarVolumeFrameLimits& limits) {
    Guard const guard(*this);
    if (!handoff_ || (frame_ && frame <= *frame_))
        return false;
    for (const auto coordinate : camera)
        if (!std::isfinite(coordinate) ||
            std::abs(coordinate) > World::kFarVolumeExactCoordinateLimitMeters)
            return false;
    Unbind();
    const auto result =
        handoff_->AdvanceFrame(camera[0], camera[2], limits, [this](const auto& mesh) {
            return UploadFarVolumeGpu(mesh, *driver_);
        });
    handoff_->TryPromote();
    frame_ = frame;
    CaptureFarWorldHandoffDiagnostic(*handoff_, frame, result, diagnostic_);
    return true;
}
void FarWorldHandoffRenderer::BeginLiveOwnership(std::uint64_t frame, std::size_t limit) {
    CheckOwner();
    ownership_.Begin(frame, limit);
}
bool FarWorldHandoffRenderer::AddLiveUpload(const FarWorldLiveUpload& upload) {
    CheckOwner();
    return ownership_.Add(upload);
}
bool FarWorldHandoffRenderer::FinishLiveOwnership() {
    CheckOwner();
    return ownership_.Finish();
}
FarWorldSubmitStats FarWorldHandoffRenderer::Submit(const std::array<double, 3>& camera,
                                                    const FarWorldPrepareDraw& prepare) {
    Guard const guard(*this);
    FarWorldSubmitStats stats;
    diagnostic_.submission = {};
    for (std::size_t i = 0; i < diagnostic_.draw_count; ++i)
        diagnostic_.draws[i].submitted = false;
    class RecordSubmission {
    public:
        RecordSubmission(FarWorldHandoffDiagnosticSnapshot& diagnostic, FarWorldSubmitStats& stats)
            : diagnostic_(diagnostic)
            , stats_(stats) {}
        ~RecordSubmission() {
            diagnostic_.submission = stats_;
        }

    private:
        FarWorldHandoffDiagnosticSnapshot& diagnostic_;
        FarWorldSubmitStats& stats_;
    } const record_submission{diagnostic_, stats};
    if (!handoff_ || !prepare)
        return stats;
    if (!frame_ || !ownership_.complete() || ownership_.frame() != *frame_) {
        ++stats.refused;
        return stats;
    }
    if (!ownership_charge_) {
        auto lease = memory_->ReserveGpu(World::kFarTierCount, kFarWorldOwnershipGpuBytes);
        if (!lease) {
            ++stats.refused;
            return stats;
        }
        ownership_charge_.emplace(std::move(*lease));
    }
    std::uint32_t vao = 0, vbo = 0;
    driver_->GetVertexArrayBinding(vao);
    driver_->GetVertexBufferBinding(vbo);
    class Restore {
    public:
        Restore(FarVolumeGpuDriver& driver, bool& submitting, std::uint32_t vao, std::uint32_t vbo)
            : driver_(driver)
            , submitting_(submitting)
            , vao_(vao)
            , vbo_(vbo) {}
        ~Restore() {
            driver_.UnbindLiveOwnership();
            driver_.BindVertexArray(vao_);
            driver_.BindBuffer(FarVolumeBufferKind::Vertex, vbo_);
            submitting_ = false;
        }

    private:
        FarVolumeGpuDriver& driver_;
        bool& submitting_;
        std::uint32_t vao_, vbo_;
    } const restore{*driver_, submitting_, vao, vbo};
    submitting_ = true;
    if (!driver_->BindLiveOwnership(ownership_.cells())) {
        driver_->ReleaseLiveOwnership();
        ownership_charge_.reset();
        ++stats.refused;
        return stats;
    }
    struct Context {
        FarWorldHandoffRenderer& owner;
        const std::array<double, 3>& camera;
        const FarWorldPrepareDraw& prepare;
        FarWorldSubmitStats& stats;
    } context{*this, camera, prepare, stats};
    handoff_->VisitServingDraws(
        [](void* opaque, const FarVolumeDrawRecord& record) {
            auto& c = *static_cast<Context*>(opaque);
            const auto* allocation = dynamic_cast<const FarVolumeGlAllocation*>(record.resource);
            FarWorldDrawCommand command;
            if (!allocation ||
                !MakeFarWorldDrawCommand(record, c.camera, allocation->index_count(), command)) {
                ++c.stats.refused;
                return;
            }
            command.live_cell_count = static_cast<std::int32_t>(c.owner.ownership_.cells().size());
            if (!c.prepare(command) || !DrawFarVolumeGpu(*allocation, *c.owner.driver_)) {
                ++c.stats.refused;
                return;
            }
            ++c.stats.draws;
            for (std::size_t i = 0; i < c.owner.diagnostic_.draw_count; ++i)
                if (c.owner.diagnostic_.draws[i].identity == record.identity)
                    c.owner.diagnostic_.draws[i].submitted = true;
            c.stats.indices += static_cast<std::size_t>(command.index_count);
            c.stats.water_indices += record.water_indices;
        },
        &context);
    return stats;
}
const FarVolumeHandoff* FarWorldHandoffRenderer::Handoff() const {
    CheckOwner();
    return handoff_.get();
}
const FarWorldHandoffDiagnosticSnapshot& FarWorldHandoffRenderer::Diagnostic() const {
    CheckOwner();
    return diagnostic_;
}
void FarWorldHandoffRenderer::PrepareWorldSwap() {
    Guard const guard(*this);
    Drain();
}
void FarWorldHandoffRenderer::Drain() {
    Unbind();
    if (handoff_)
        handoff_->CancelAndDrain();
    handoff_.reset();
    if (driver_)
        driver_->ReleaseLiveOwnership();
    ownership_charge_.reset();
    ownership_.Begin(0);
    diagnostic_ = {};
    charge_.reset();
    memory_.reset();
    frame_.reset();
    world_epoch_ = 0;
}
void FarWorldHandoffRenderer::Shutdown() {
    Guard const guard(*this);
    Drain();
    driver_.reset();
}
} // namespace Luminumbra::Rendering
