#pragma once

#include "luminumbra_common/world/FarHorizon.h"

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
    SpanHistoryExhausted
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
FarWorldSelection SelectFarWorld(FarWorldMode requested,
                                 FarWorldPurpose purpose,
                                 FarWorldRefusal prerequisite,
                                 bool enabled = true) noexcept;

// Exactly one scheduler at the owner frame-preparation point. Passing callbacks
// directly keeps this seam independent of GL and avoids owned callback storage.
template<class LegacyAdvance, class LadderAdvance>
void AdvanceSelectedFarWorld(const FarWorldSelection& selection,
                             LegacyAdvance&& legacy,
                             LadderAdvance&& ladder) {
    if (!selection.advance)
        return;
    if (selection.effective == FarWorldMode::Legacy)
        legacy();
    else
        ladder();
}

// Values only. The backend consumes the allocation synchronously; a pass never
// receives or retains a borrowed resource pointer. Origin subtraction happens
// in double before narrowing. No CPU frustum or estimated-height culling.
struct FarWorldDrawCommand {
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
using FarWorldPrepareDraw = std::function<bool(const FarWorldDrawCommand&)>;
struct FarWorldSubmitStats {
    std::size_t draws = 0, indices = 0, refused = 0;
};
using FarWorldSubmitFn = std::function<FarWorldSubmitStats(const FarWorldPrepareDraw&)>;
using FarWorldLegacySubmitFn = std::function<FarWorldSubmitStats()>;
// Exactly one branch runs. Preview overrides never change the stored request.
FarWorldSubmitStats SubmitSelectedFarWorld(const FarWorldSelection& selection,
                                           const FarWorldLegacySubmitFn& legacy,
                                           const FarWorldSubmitFn& ladder,
                                           const FarWorldPrepareDraw& prepare);

} // namespace Luminumbra::Rendering
