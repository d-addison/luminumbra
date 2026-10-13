#include "FarWorldDiagnostics.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace Luminumbra::Rendering {
const char* FarWorldModeName(FarWorldMode mode) noexcept {
    return mode == FarWorldMode::Ladder ? "ladder" : "legacy";
}
const char* FarWorldRefusalName(FarWorldRefusal refusal) noexcept {
    switch (refusal) {
#define FW_REASON(name)                                                                            \
    case FarWorldRefusal::name:                                                                    \
        return #name
        FW_REASON(None);
        FW_REASON(ImmutableAuthorityUnavailable);
        FW_REASON(MissingLimits);
        FW_REASON(BackendUnavailable);
        FW_REASON(AdmissionFailed);
        FW_REASON(InvalidRequests);
        FW_REASON(SpanRejected);
        FW_REASON(PreviewOverride);
        FW_REASON(OffscreenSuppressed);
        FW_REASON(SpanHistoryExhausted);
        FW_REASON(ShaderProgramUnavailable);
        FW_REASON(InvalidCavePolicy);
        FW_REASON(CavePolicyMismatch);
        FW_REASON(WaterUnavailable);
        FW_REASON(WaterSpanRefused);
#undef FW_REASON
    }
    return "Unknown";
}
const char* FarWorldSubmissionRefusalName(FarWorldSubmissionRefusal refusal) noexcept {
    switch (refusal) {
        case FarWorldSubmissionRefusal::None:
            return "None";
        case FarWorldSubmissionRefusal::LiveOwnershipIncomplete:
            return "LiveOwnershipIncomplete";
        case FarWorldSubmissionRefusal::LogicalStoreAdmission:
            return "LogicalStoreAdmission";
        case FarWorldSubmissionRefusal::BackendUnavailable:
            return "BackendUnavailable";
        case FarWorldSubmissionRefusal::DrawRejected:
            return "DrawRejected";
    }
    return "Unknown";
}
namespace {
nlohmann::json IdentityJson(const World::FarBuildIdentity& identity) {
    return {{"world_epoch", identity.world_epoch},
            {"content_identity", identity.content_hash},
            {"cave_policy", identity.cave_policy},
            {"key", {identity.key.tier, identity.key.tx, identity.key.tz}},
            {"bake_generation", identity.bake_generation},
            {"span_generation", identity.span_generation}};
}
const char* StateName(World::FarTileState state) {
    switch (state) {
        case World::FarTileState::Wanted:
            return "Wanted";
        case World::FarTileState::Building:
            return "Building";
        case World::FarTileState::Ready:
            return "Ready";
        case World::FarTileState::Uploaded:
            return "Uploaded";
        case World::FarTileState::Empty:
            return "Empty";
        case World::FarTileState::Rejected:
            return "Rejected";
    }
    return "Unknown";
}
} // namespace
nlohmann::json FarWorldDiagnosticJson(const FarWorldDiagnosticSnapshot& s) {
    nlohmann::json tiers = nlohmann::json::array(), tiles = nlohmann::json::array();
    for (std::size_t i = 0; i < s.tiers.size(); ++i) {
        const auto& t = s.tiers[i];
        const auto& v = t.resident;
        tiers.push_back({{"tier", i + 1},
                         {"wanted", v.wanted},
                         {"pending", v.pending},
                         {"building", v.building},
                         {"ready", v.ready},
                         {"settled", v.settled},
                         {"uploaded_pairs", v.uploaded_pairs},
                         {"empty_pairs", v.empty_pairs},
                         {"current_covering", v.covering},
                         {"rejected", v.request_rejected},
                         {"budget_evicted", v.budget_evicted},
                         {"evicted_range_total", t.evicted_range},
                         {"evicted_budget_total", t.evicted_budget},
                         {"stale_rebuilds_total", t.stale_rebuilds},
                         {"retained_ready_cpu_bytes", v.ready_cpu_bytes},
                         {"retained_gpu_logical_store_bytes", v.retained_gpu_bytes},
                         {"pool_cpu_bytes", v.memory.bytes_cpu},
                         {"pool_gpu_logical_store_bytes", v.memory.bytes_gpu},
                         {"pool_sampled_total_bytes", v.memory.bytes_total},
                         {"pool_limit_bytes", v.memory.limit},
                         {"pool_peak_bytes", v.memory.peak_total},
                         {"admission_refusals", v.memory.admission_refusals},
                         {"draws", t.draws},
                         {"indices", t.indices},
                         {"draw_refusals", t.draw_refusals},
                         {"water_known_dry_tiles", v.water_known_dry_tiles},
                         {"water_wet_tiles", v.water_wet_tiles},
                         {"retained_water_indices", v.retained_water_indices},
                         {"submitted_water_indices", v.submitted_water_indices}});
    }
    for (std::size_t i = 0; i < std::min(s.tile_count, s.tiles.size()); ++i) {
        const auto& t = s.tiles[i];
        tiles.push_back({{"desired", IdentityJson(t.desired)},
                         {"built", IdentityJson(t.built)},
                         {"has_pair", t.has_pair},
                         {"water_complete", t.water_complete},
                         {"water_known_dry", t.water_known_dry},
                         {"terrain_indices", t.terrain_indices},
                         {"water_indices", t.water_indices},
                         {"state", StateName(t.coverage.state)},
                         {"current", t.coverage.current},
                         {"span_ok", t.coverage.span_ok},
                         {"eligibility", static_cast<int>(t.eligibility)},
                         {"requested_span",
                          t.intent.extra_span ? nlohmann::json{t.intent.extra_span->min_y,
                                                               t.intent.extra_span->max_y}
                                              : nlohmann::json(nullptr)},
                         {"built_span", {t.request.span.min_y, t.request.span.max_y}}});
    }
    return {{"requested_mode", FarWorldModeName(s.selection.requested)},
            {"effective_mode", FarWorldModeName(s.selection.effective)},
            {"refusal", FarWorldRefusalName(s.selection.refusal)},
            {"submission_refusal", FarWorldSubmissionRefusalName(s.submission_refusal)},
            {"suppression", !s.selection.submit ? "SubmissionOverride" : "None"},
            {"selected", s.selection.effective == FarWorldMode::Ladder},
            {"world_epoch", s.world_epoch},
            {"frame_id", s.frame},
            {"content_identity", s.content_identity},
            {"bake_generation", s.bake_generation},
            {"far_plane_m", s.far_plane},
            {"requested_cave_policy", s.requested_cave_policy
                                          ? nlohmann::json(static_cast<int>(*s.requested_cave_policy))
                                          : nlohmann::json(nullptr)},
            {"bound_cave_policy", s.bound_cave_policy
                                      ? nlohmann::json(static_cast<int>(*s.bound_cave_policy))
                                      : nlohmann::json(nullptr)},
            {"numeric_policy", static_cast<int>(s.numeric)},
            {"captured", s.captured},
            {"complete", s.complete},
            {"advancement_occurred", s.advanced},
            {"submission_occurred", s.submission_occurred},
            {"draws", s.draws},
            {"indices", s.indices},
            {"ladder_water_available", s.ladder_water_available},
            {"water_capability", static_cast<int>(s.water_capability)},
            {"memory_sampling", "asynchronous_diagnostic_not_atomic_admission"},
            {"gpu_bytes_kind", "logical_stores"},
            {"camera", s.camera},
            {"frame_upload",
             {{"attempts", s.upload.upload_attempts},
              {"debited_bytes", s.upload.upload_bytes},
              {"successful_bytes", s.upload.successful_upload_bytes},
              {"horizon_refused", s.upload.horizon_refused}}},
            {"tiers", tiers},
            {"tiles", tiles}};
}
FarWorldStationRefusal CheckFarWorldStation(const FarWorldDiagnosticSnapshot& s,
                                            std::span<const World::FarTileKey> expected,
                                            double seconds) noexcept {
    using R = FarWorldStationRefusal;
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 90)
        return R::Timeout;
    if (!s.captured || !s.complete)
        return R::IncompleteSnapshot;
    if (s.selection.requested != FarWorldMode::Ladder ||
        s.selection.effective != FarWorldMode::Ladder)
        return R::WrongMode;
    if (s.selection.refusal != FarWorldRefusal::None)
        return R::RendererRefused;
    if (!s.selection.submit)
        return R::SubmissionSuppressed;
    if (s.far_plane != World::kFarPlaneMeters)
        return R::WrongPlane;
    if (!s.world_epoch || !s.frame || !s.content_identity || !s.requested_cave_policy ||
        !s.bound_cave_policy || s.requested_cave_policy != s.bound_cave_policy ||
        (*s.bound_cave_policy != World::FarCaveMode::BandLimited &&
         *s.bound_cave_policy != World::FarCaveMode::BoxFiltered))
        return R::InvalidBinding;
    if (!s.advanced)
        return R::NoAdvancement;
    if (s.submission_refusal != FarWorldSubmissionRefusal::None)
        return R::SubmissionRefused;
    if (expected.empty() || s.tile_count != expected.size() || s.tile_count > s.tiles.size())
        return R::ExpectedSetMismatch;
    if (s.upload.horizon_refused)
        return R::HorizonRefused;
    for (const auto& tier : s.tiers)
        if (tier.resident.request_rejected || tier.resident.budget_evicted || tier.draw_refusals)
            return R::RejectedTiles;
    if (!s.ladder_water_available ||
        (s.water_capability != FarWorldWaterCapability::ExplicitlyDry &&
         s.water_capability != FarWorldWaterCapability::Wet))
        return R::WaterIncomplete;
    bool mixed = false;
    std::uint64_t wet_tiles = 0, submitted_water = 0;
    std::array<std::uint64_t, World::kFarTierCount> dry_counts{}, water_counts{}, terrain_counts{};
    for (const auto& key : expected) {
        const auto end = s.tiles.begin() + static_cast<std::ptrdiff_t>(s.tile_count);
        const auto found =
            std::find_if(s.tiles.begin(), end, [&](const auto& t) { return t.desired.key == key; });
        if (found == end)
            return R::ExpectedSetMismatch;
        if (!found->has_pair)
            return R::MissingPair;
        if (found->built != found->desired)
            return R::StaleGeneration;
        if (found->desired.world_epoch != s.world_epoch ||
            found->desired.content_hash != s.content_identity ||
            found->desired.bake_generation != s.bake_generation ||
            found->desired.cave_policy != static_cast<std::uint8_t>(*s.bound_cave_policy))
            return R::StaleBinding;
        if (!World::IsFarTileCovering(found->coverage) ||
            found->request.span.min_y >= found->request.span.max_y ||
            (found->intent.extra_span &&
             (!std::isfinite(found->intent.extra_span->min_y) ||
              !std::isfinite(found->intent.extra_span->max_y) ||
              found->intent.extra_span->min_y > found->intent.extra_span->max_y ||
              found->request.span.min_y > found->intent.extra_span->min_y ||
              found->request.span.max_y < found->intent.extra_span->max_y)))
            return R::SpanRefused;
        if (!found->water_complete || found->water_known_dry != (found->water_indices == 0) ||
            (found->coverage.state == World::FarTileState::Empty &&
             (found->water_indices || found->terrain_indices)) ||
            (found->coverage.state == World::FarTileState::Uploaded && !found->water_indices &&
             !found->terrain_indices))
            return R::WaterIncomplete;
        if (key.tier < 1 || key.tier > World::kFarTierCount || found->water_indices % 6 != 0 ||
            found->terrain_indices % 3 != 0)
            return R::WaterIncomplete;
        const auto number = key.tier - 1;
        dry_counts[number] += found->water_known_dry;
        water_counts[number] += found->water_indices;
        terrain_counts[number] += found->terrain_indices;
        mixed = mixed || found->coverage.state == World::FarTileState::Uploaded;
    }
    std::size_t total_draws = 0, total_indices = 0;
    for (std::size_t i = 0; i < s.tiers.size(); ++i) {
        const auto count = static_cast<std::size_t>(std::count_if(
            expected.begin(), expected.end(), [i](const auto& key) { return key.tier == i + 1; }));
        const auto& tier = s.tiers[i];
        if (tier.resident.wanted != count || tier.resident.covering != count ||
            tier.resident.settled != count || tier.resident.pending || tier.resident.building ||
            tier.resident.ready || tier.draws != tier.resident.uploaded_pairs ||
            tier.indices % 3 != 0 || tier.indices / 3 < tier.draws ||
            (tier.draws == 0) != (tier.indices == 0))
            return R::TierCounters;
        if (tier.resident.water_known_dry_tiles + tier.resident.water_wet_tiles != count ||
            tier.resident.submitted_water_indices > tier.indices ||
            tier.resident.submitted_water_indices > tier.resident.retained_water_indices ||
            tier.resident.submitted_water_indices % 3 != 0)
            return R::WaterIncomplete;
        if (tier.resident.water_known_dry_tiles != dry_counts[i] ||
            tier.resident.retained_water_indices != water_counts[i] ||
            tier.resident.submitted_water_indices != water_counts[i] ||
            tier.indices != water_counts[i] + terrain_counts[i])
            return R::WaterIncomplete;
        wet_tiles += tier.resident.water_wet_tiles;
        submitted_water += tier.resident.submitted_water_indices;
        total_draws += tier.draws;
        total_indices += tier.indices;
    }
    if (total_draws != s.draws || total_indices != s.indices)
        return R::DrawTotals;
    if (mixed && !(s.submission_occurred && s.draws > 0 && s.indices > 0))
        return R::MissingSubmission;
    if ((s.water_capability == FarWorldWaterCapability::ExplicitlyDry &&
         (wet_tiles || submitted_water)) ||
        (s.water_capability == FarWorldWaterCapability::Wet && (!wet_tiles || !submitted_water)))
        return R::WaterIncomplete;
    return R::None;
}
bool FarWorldStationReady(const FarWorldDiagnosticSnapshot& snapshot,
                          std::span<const World::FarTileKey> expected,
                          double seconds) noexcept {
    return CheckFarWorldStation(snapshot, expected, seconds) == FarWorldStationRefusal::None;
}
} // namespace Luminumbra::Rendering
