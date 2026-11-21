#pragma once

#include "luminumbra_common/world/FarHorizon.h"
#include "luminumbra_common/world/FarWantedSet.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace Luminumbra::Rendering {

enum class FarWorldMode {
    Legacy,
    Ladder
};
enum class FarWorldPurpose {
    World,
    Preview,
    OffscreenPreview
};
enum class FarWorldRefusal {
    None,
    ImmutableAuthorityUnavailable,
    MissingLimits,
    BackendUnavailable,
    AdmissionFailed,
    InvalidRequests,
    SpanRejected,
    PreviewOverride,
    OffscreenSuppressed,
    SpanHistoryExhausted,
    ShaderProgramUnavailable,
    InvalidCavePolicy,
    CavePolicyMismatch,
    WaterUnavailable,
    WaterSpanRefused
};

constexpr FarWorldMode RequestedFarWorldMode(bool enabled) noexcept {
    return enabled ? FarWorldMode::Ladder : FarWorldMode::Legacy;
}
constexpr float FarWorldPlane(FarWorldMode mode) noexcept {
    return mode == FarWorldMode::Ladder ? World::kFarPlaneMeters : World::kLegacyFarPlaneMeters;
}
struct FarWorldSelection {
    FarWorldMode requested = FarWorldMode::Legacy;
    FarWorldMode effective = FarWorldMode::Legacy;
    FarWorldRefusal refusal = FarWorldRefusal::None;
    bool advance = true, submit = true;
};
// Retained default/preview compatibility. Requested Ladder may still select
// Legacy after refusal; effective Ladder never receives a Legacy runtime owner.
template<class LegacyOwner>
constexpr LegacyOwner* LegacyFarWorldCompatibility(const FarWorldSelection& selection,
                                                   LegacyOwner* owner) noexcept {
    return selection.effective == FarWorldMode::Legacy ? owner : nullptr;
}

// The preview's historical enable hook belongs only to compatibility. Selected
// Ladder suppression uses the submission override, preserving its frame/policy.
constexpr FarWorldSelection ApplyLegacyFarWorldEnable(FarWorldSelection selection,
                                                      bool enabled) noexcept {
    if (selection.effective == FarWorldMode::Legacy && !enabled) {
        selection.advance = false;
        selection.submit = false;
    }
    return selection;
}
FarWorldSelection SelectFarWorld(FarWorldMode requested,
                                 FarWorldPurpose purpose,
                                 FarWorldRefusal prerequisite,
                                 bool enabled = true) noexcept;

// The Ladder callback republishes selection after preparation. A refusal must
// advance effective Legacy in this frame, while a successful Ladder frame never
// advances Legacy. Callbacks run at most once each, without owned storage or GL.
template<class LegacyAdvance, class LadderAdvance>
void AdvanceSelectedFarWorld(const FarWorldSelection& selection,
                             LegacyAdvance&& legacy,
                             LadderAdvance&& ladder) {
    if (!selection.advance)
        return;
    if (selection.effective == FarWorldMode::Legacy) {
        legacy();
        return;
    }
    ladder();
    if (selection.advance && selection.effective == FarWorldMode::Legacy)
        legacy();
}

// Values only. The backend consumes the allocation synchronously; a pass never
// receives or retains a borrowed resource pointer. Origin subtraction happens
// in double before narrowing. No CPU frustum or estimated-height culling.
struct FarWorldDrawCommand {
    World::FarBuildIdentity diagnostic_identity{};
    std::array<std::int64_t, 2> diagnostic_span{};
    std::uint64_t diagnostic_water_indices = 0;
    std::array<float, 3> relative_origin{}, world_origin{};
    double horizontal_outer_clip_meters = 0, view_ray_bias_meters = 0;
    std::int32_t index_count = 0;
    float tile_edge_meters = 0, finer_outer_radius_meters = 0;
    bool arrival_eligible = false;
    // Child built spans minus this draw's integer Y origin, exactly representable.
    std::array<std::array<float, 2>, 4> child_local_y_spans{};
    std::array<std::int32_t, 3> origin_chunk{};
    std::array<float, 3> origin_remainder{};
    std::int32_t live_cell_count = 0;
};
// Producer metadata lives only for this synchronous submission scope. The GL
// backend consumes each prepared command once; scope exit removes stale metadata.
// The capture, when present, outlives this scope.
class GBufferDiagnosticCapture;
class FarWorldDiagnosticDraw {
public:
    explicit FarWorldDiagnosticDraw(GBufferDiagnosticCapture* capture) noexcept;
    ~FarWorldDiagnosticDraw();
    FarWorldDiagnosticDraw(const FarWorldDiagnosticDraw&) = delete;
    FarWorldDiagnosticDraw& operator=(const FarWorldDiagnosticDraw&) = delete;
    void prepare(const FarWorldDrawCommand& command) noexcept;
    static bool Write(std::int32_t index_count) noexcept;

private:
    GBufferDiagnosticCapture* capture_;
    FarWorldDiagnosticDraw* previous_;
    FarWorldDrawCommand command_{};
    bool prepared_ = false;
};

using FarWorldPrepareDraw = std::function<bool(const FarWorldDrawCommand&)>;
struct FarWorldSubmitStats {
    std::size_t draws = 0, indices = 0, refused = 0;
    std::uint64_t water_indices = 0;
};
using FarWorldSubmitFn = std::function<FarWorldSubmitStats(const FarWorldPrepareDraw&)>;
using FarWorldLegacySubmitFn = std::function<FarWorldSubmitStats()>;
// Exactly one branch runs. Preview overrides never change the stored request.
FarWorldSubmitStats SubmitSelectedFarWorld(const FarWorldSelection& selection,
                                           const FarWorldLegacySubmitFn& legacy,
                                           const FarWorldSubmitFn& ladder,
                                           const FarWorldPrepareDraw& prepare);

} // namespace Luminumbra::Rendering
