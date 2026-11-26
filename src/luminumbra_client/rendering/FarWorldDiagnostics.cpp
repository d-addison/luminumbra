#include "FarWorldDiagnostics.h"
#include "FarWorldHandoffDiagnostics.h"
#include "luminumbra_client/app/FarBudgetMeasurement.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <nlohmann/json.hpp>

namespace Luminumbra::Rendering {
namespace {
// Explicit little-endian FNV-1a words; no object bytes or implementation-defined hash.
void ManifestWord(std::uint64_t& hash, std::uint64_t word) noexcept {
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash = (hash ^ (word & 255)) * 1099511628211ULL;
        word >>= 8;
    }
}
struct ManifestFold {
    std::uint64_t sum = 0, mixed = 0;
    bool valid = true;
    void Add(const FarWorldTileDiagnostic& tile) noexcept {
        const auto& id = tile.desired;
        valid = valid && id.content_hash != 0 && tile.has_pair &&
                World::IsFarTileCovering(tile.coverage) &&
                tile.request.span.min_y < tile.request.span.max_y;
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto word : {id.world_epoch,
                                id.content_hash,
                                std::uint64_t(id.cave_policy),
                                std::uint64_t(id.key.tier),
                                std::uint64_t(id.key.tx),
                                std::uint64_t(id.key.tz),
                                id.bake_generation,
                                id.span_generation,
                                std::uint64_t(tile.numeric),
                                std::uint64_t(tile.intent.key.tier),
                                std::uint64_t(tile.intent.key.x),
                                std::uint64_t(tile.intent.key.z),
                                std::uint64_t(tile.intent.extra_span.has_value()),
                                std::uint64_t(tile.request.key.tier),
                                std::uint64_t(tile.request.key.x),
                                std::uint64_t(tile.request.key.z),
                                tile.request.field_identity,
                                std::uint64_t(tile.request.span.min_y),
                                std::uint64_t(tile.request.span.max_y)}) {
            ManifestWord(hash, word);
        }
        if (tile.intent.extra_span) {
            for (const float value :
                 {tile.intent.extra_span->min_y, tile.intent.extra_span->max_y}) {
                valid = valid && std::isfinite(value);
                // Canonicalize signed zero; all other finite floats keep their exact bits.
                ManifestWord(hash, std::bit_cast<std::uint32_t>(value == 0 ? 0.0f : value));
            }
        }
        // Canonical multiset fold: unsigned sum and rotated XOR are order independent.
        sum += hash;
        mixed ^= std::rotl(hash, 23);
    }
    std::uint64_t Finish(const FarWorldDiagnosticSnapshot& snapshot) const noexcept {
        if (!valid || !snapshot.complete || !snapshot.tile_count || !snapshot.latest_target_ready)
            return 0;
        std::uint64_t hash = 14695981039346656037ULL;
        // Domain/version word is specific to this diagnostic manifest, not a tile fingerprint.
        for (const auto word : {std::uint64_t{0x46574d414e494601},
                                snapshot.world_epoch,
                                snapshot.bake_generation,
                                snapshot.target_serial,
                                std::uint64_t(snapshot.numeric),
                                std::uint64_t(snapshot.tile_count),
                                sum,
                                mixed})
            ManifestWord(hash, word);
        return hash == 0 ? 1 : hash;
    }
};
nlohmann::json FarWorldRefreshWorkJson(const std::optional<FarWorldRefreshWork>& work) {
    if (!work)
        return nullptr;
    return {{"source_read_bytes", work->source_read_bytes},
            {"source_copied_bytes", work->source_copied_bytes},
            {"source_decode_work", work->source_decode_work},
            {"capture_work", work->capture_work},
            {"admission_work", work->admission_work},
            {"admission_height_observations", work->admission_height_observations},
            {"source_ms", work->source_ms}};
}
} // namespace
FarWorldRefusal
FarWorldProducerRefusal(const World::FarWorldAuthorityProducer::FrozenResult& result) noexcept {
    using E = World::FarWorldAuthorityProducer::Error;
    using C = World::FarFrozenCaptureError;
    using R = FarWorldRefusal;
    switch (result.source) {
        case E::None:
            break;
        case E::InvalidLimits:
            return R::MissingLimits;
        case E::InvalidWindow:
            return R::SpanRejected;
        case E::IndexCapacity:
        case E::CopyLimit:
        case E::AllocationFailure:
        case E::DeadlineExceeded:
        case E::CommittedReadRefused:
        case E::SourceReadExpired:
            return R::AdmissionFailed;
        case E::AuthorityUnavailable:
            return R::ImmutableAuthorityUnavailable;
        case E::WaterUnavailable:
            return R::WaterUnavailable;
    }
    if (result.source != E::None) {
        return R::InvalidRequests;
    }
    switch (result.capture) {
        case C::None:
            return R::None;
        case C::InvalidSource:
        case C::Unavailable:
        case C::NonFiniteSample:
        case C::IdentityMismatch:
            return R::ImmutableAuthorityUnavailable;
        case C::InvalidRegion:
        case C::CoordinateRange:
            return R::SpanRejected;
        case C::WorkLimit:
        case C::ByteLimit:
        case C::AllocationFailure:
        case C::DeadlineExceeded:
            return R::AdmissionFailed;
    }
    return R::InvalidRequests;
}
void ProjectFarWorldSessionDiagnostic(const FarWorldHandoffDiagnosticSnapshot& in,
                                      World::FarVolumeNumericProfile numeric,
                                      FarWorldDiagnosticSnapshot& out) noexcept {
    out.authority_kind = FarWorldAuthorityKind::Session;
    out.content_identity = 0;
    out.numeric = numeric;
    out.bake_generation = in.state.committed_generation;
    out.serving_serial = in.state.serving_serial;
    out.serving_committed_generation = in.state.serving_committed_generation;
    out.target_serial = in.state.target_serial;
    out.latest_target_ready = in.tile_count <= in.tiles.size() && FarWorldHandoffTargetReady(in);
    out.advanced = out.advanced && in.frame == out.frame;
    out.complete = out.complete && in.captured && in.complete &&
                   out.world_epoch == in.state.world_epoch && in.tile_count <= in.tiles.size() &&
                   in.draw_count <= in.draws.size();
    out.ladder_water_available = true;
    out.water_capability = FarWorldWaterCapability::ExplicitlyDry;
    // A Legacy fallback keeps the caller's totals; only Ladder submission comes from the handoff.
    const bool ladder = out.selection.effective == FarWorldMode::Ladder;
    const bool submitted = ladder && out.selection.submit && in.frame == out.frame;
    if (ladder) {
        out.draws = submitted ? in.submission.draws : 0;
        out.indices = submitted ? in.submission.indices : 0;
        out.submitted_water_indices = submitted ? in.submission.water_indices : 0;
        out.submission_occurred = out.draws != 0;
        if (submitted && in.submission.refused) {
            out.submission_refusal = FarWorldSubmissionRefusal::DrawRejected;
        }
    }
    out.tile_count = 0;
    ManifestFold manifest;
    out.tiers = {};
    for (std::size_t i = 0; i < out.tiers.size(); ++i) {
        out.tiers[i].resident.memory = in.memory[i];
    }
    // Each value view has at most 512 entries. No resource is borrowed or copied.
    for (std::size_t i = 0; i < std::min(in.tile_count, in.tiles.size()); ++i) {
        const auto& value = in.tiles[i].observation;
        if (!value.target) {
            continue;
        }
        const auto& target = *value.target;
        const auto tier = target.identity.key.tier;
        if (tier < 1 || tier > World::kFarTierCount || out.tile_count == out.tiles.size()) {
            out.complete = false;
            continue;
        }
        auto& tile = out.tiles[out.tile_count++];
        tile.desired = target.identity;
        tile.accepted_content_identity = target.identity.content_hash;
        tile.numeric = numeric;
        tile.intent = target.intent;
        tile.request = value.target_built;
        tile.coverage = value.target_coverage;
        tile.eligibility = value.target_eligibility;
        tile.has_pair = tile.coverage.state == World::FarTileState::Uploaded ||
                        tile.coverage.state == World::FarTileState::Empty;
        if (World::IsFarTileCovering(tile.coverage)) {
            tile.built = tile.desired;
        }
        tile.water_complete = value.target_water_complete;
        tile.water_known_dry = value.target_water_known_dry;
        tile.terrain_indices = value.target_terrain_indices;
        tile.water_indices = value.target_water_indices;
        manifest.Add(tile);
        auto& counters = out.tiers[tier - 1].resident;
        ++counters.wanted;
        switch (tile.eligibility) {
            case FarVolumeEligibility::Wanted:
                break;
            case FarVolumeEligibility::Pending:
                ++counters.pending;
                break;
            case FarVolumeEligibility::Building:
                ++counters.building;
                break;
            case FarVolumeEligibility::Ready:
                ++counters.ready;
                break;
            case FarVolumeEligibility::Settled:
                ++counters.settled;
                break;
            case FarVolumeEligibility::RequestRejected:
                ++counters.request_rejected;
                break;
            case FarVolumeEligibility::BudgetEvicted:
                ++counters.budget_evicted;
                break;
        }
        counters.covering += World::IsFarTileCovering(tile.coverage);
        counters.empty_pairs += tile.has_pair && tile.coverage.state == World::FarTileState::Empty;
        counters.uploaded_pairs +=
            tile.has_pair && tile.coverage.state == World::FarTileState::Uploaded;
        counters.ready_cpu_bytes += value.ready_cpu_bytes;
        counters.retained_gpu_bytes += value.staged_gpu_bytes + value.serving_gpu_bytes;
        if (tile.water_complete) {
            counters.water_known_dry_tiles += tile.water_known_dry;
            counters.water_wet_tiles += !tile.water_known_dry;
            counters.retained_water_indices += tile.water_indices;
            if (!tile.water_known_dry) {
                out.water_capability = FarWorldWaterCapability::Wet;
            }
        }
    }
    out.manifest_fingerprint = manifest.Finish(out);
    for (std::size_t i = 0; i < std::min(in.draw_count, in.draws.size()); ++i) {
        const auto& draw = in.draws[i];
        if (submitted && draw.submitted && draw.identity.key.tier >= 1 &&
            draw.identity.key.tier <= World::kFarTierCount) {
            ++out.tiers[draw.identity.key.tier - 1].draws;
        }
    }
    // Component totals become submission evidence only when every drawable target
    // pair in that tier was actually submitted from the latest serving cohort.
    for (std::size_t i = 0; i < out.tile_count; ++i) {
        const auto& tile = out.tiles[i];
        auto& tier = out.tiers[tile.desired.key.tier - 1];
        if (out.serving_serial == out.target_serial && tier.draws == tier.resident.uploaded_pairs &&
            tile.coverage.state == World::FarTileState::Uploaded) {
            tier.indices += static_cast<std::size_t>(tile.terrain_indices + tile.water_indices);
            tier.resident.submitted_water_indices += tile.water_indices;
        }
    }
}
bool FarWorldReportChanged(const std::optional<FarWorldReportingState>& previous,
                           const FarWorldReportingState& current) noexcept {
    return !previous || previous->selection.requested != current.selection.requested ||
           previous->selection.effective != current.selection.effective ||
           previous->selection.refusal != current.selection.refusal ||
           previous->selection.submit != current.selection.submit ||
           previous->submission_refusal != current.submission_refusal;
}

FarWorldReportingState FarWorldReportState(const FarWorldDiagnosticSnapshot& s,
                                           bool terrain_submission) noexcept {
    FarWorldReportingState result{
        .selection = s.selection,
        .submission_refusal = terrain_submission ? s.submission_refusal
                                                 : FarWorldSubmissionRefusal::IsolationSuppressed,
        .coverage_settled = s.captured && s.complete && s.advanced &&
                            s.selection.effective == FarWorldMode::Ladder &&
                            !s.upload.horizon_refused && s.ladder_water_available};
    std::size_t remaining = s.tile_count;
    result.coverage_settled =
        result.coverage_settled && remaining > 0 && remaining <= s.tiles.size();
    // Five fixed tiers: no tile traversal or allocation in per-frame reporting.
    for (const auto& tier : s.tiers) {
        const auto& resident = tier.resident;
        if (resident.wanted > remaining)
            result.coverage_settled = false;
        else
            remaining -= resident.wanted;
        result.coverage_settled =
            result.coverage_settled && resident.wanted == resident.settled &&
            resident.wanted == resident.covering && !resident.pending && !resident.building &&
            !resident.ready && !resident.request_rejected && !resident.budget_evicted &&
            !tier.draw_refusals && resident.water_known_dry_tiles <= resident.wanted &&
            resident.water_wet_tiles == resident.wanted - resident.water_known_dry_tiles;
    }
    result.coverage_settled =
        result.coverage_settled && remaining == 0 &&
        (s.authority_kind == FarWorldAuthorityKind::Fixture ||
         (s.latest_target_ready && s.target_serial != 0 && s.serving_serial == s.target_serial));
    if (s.authority_kind == FarWorldAuthorityKind::Session) {
        result.coverage_settled = result.coverage_settled && s.source_failure.ok() &&
                                  s.serving_committed_generation == s.bake_generation;
    }
    return result;
}

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
        case FarWorldSubmissionRefusal::IsolationSuppressed:
            return "IsolationSuppressed";
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
                         {"accepted_content_identity", t.accepted_content_identity},
                         {"built_field_identity", t.request.field_identity},
                         {"numeric_policy", static_cast<int>(t.numeric)},
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
    return {{"authority_kind",
             s.authority_kind == FarWorldAuthorityKind::Session ? "session" : "fixture"},
            {"serving_serial", s.serving_serial},
            {"target_serial", s.target_serial},
            {"committed_generation", s.bake_generation},
            {"serving_committed_generation", s.serving_committed_generation},
            {"manifest_fingerprint", s.manifest_fingerprint},
            {"refresh_work", FarWorldRefreshWorkJson(s.refresh_work)},
            {"submitted_water_indices", s.submitted_water_indices},
            {"latest_target_ready", s.latest_target_ready},
            {"source_error", static_cast<int>(s.source_failure.source)},
            {"capture_error", static_cast<int>(s.source_failure.capture)},
            {"requested_mode", FarWorldModeName(s.selection.requested)},
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
            {"requested_cave_policy",
             s.requested_cave_policy ? nlohmann::json(static_cast<int>(*s.requested_cave_policy))
                                     : nlohmann::json(nullptr)},
            {"bound_cave_policy",
             s.bound_cave_policy ? nlohmann::json(static_cast<int>(*s.bound_cave_policy))
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
    if (!s.world_epoch || !s.frame ||
        (s.authority_kind == FarWorldAuthorityKind::Fixture && !s.content_identity) ||
        (s.authority_kind != FarWorldAuthorityKind::Fixture &&
         s.authority_kind != FarWorldAuthorityKind::Session) ||
        (s.numeric != World::FarVolumeNumericProfile::RawFloat &&
         s.numeric != World::FarVolumeNumericProfile::LegacySigned16) ||
        !s.requested_cave_policy || !s.bound_cave_policy ||
        s.requested_cave_policy != s.bound_cave_policy ||
        (*s.bound_cave_policy != World::FarCaveMode::BandLimited &&
         *s.bound_cave_policy != World::FarCaveMode::BoxFiltered))
        return R::InvalidBinding;
    if (s.authority_kind == FarWorldAuthorityKind::Session &&
        (!s.target_serial || s.serving_serial != s.target_serial || !s.latest_target_ready ||
         !s.source_failure.ok())) {
        return R::StaleGeneration;
    }
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
    // One flag for each of the snapshot's at most 512 tile slots.
    std::array<bool, kFarWorldDiagnosticTiles> matched{};
    for (const auto& key : expected) {
        const auto end = s.tiles.begin() + static_cast<std::ptrdiff_t>(s.tile_count);
        const auto found =
            std::find_if(s.tiles.begin(), end, [&](const auto& t) { return t.desired.key == key; });
        if (found == end)
            return R::ExpectedSetMismatch;
        const auto slot = static_cast<std::size_t>(found - s.tiles.begin());
        if (matched[slot]) {
            return R::ExpectedSetMismatch;
        }
        matched[slot] = true;
        if (!found->has_pair)
            return R::MissingPair;
        if (found->built != found->desired)
            return R::StaleGeneration;
        if (found->desired.world_epoch != s.world_epoch ||
            (s.authority_kind == FarWorldAuthorityKind::Fixture
                 ? found->desired.content_hash != s.content_identity
                 : !found->accepted_content_identity ||
                       found->desired.content_hash != found->accepted_content_identity ||
                       found->numeric != s.numeric ||
                       found->request.field_identity != found->accepted_content_identity ||
                       found->request.key.tier != key.tier || found->request.key.x != key.tx ||
                       found->request.key.z != key.tz) ||
            found->desired.bake_generation != s.bake_generation ||
            found->desired.cave_policy != static_cast<std::uint8_t>(*s.bound_cave_policy))
            return R::StaleBinding;
        if (s.authority_kind == FarWorldAuthorityKind::Session) {
            if (key.tier < 1 || key.tier > World::kFarTierCount) {
                return R::SpanRefused;
            }
            const auto edge = World::kFarTierTable[key.tier - 1].brick_edge_meters;
            const auto& span = found->request.span;
            if (span.min_y < -World::kFarVolumeExactCoordinateLimitMeters ||
                span.max_y > World::kFarVolumeExactCoordinateLimitMeters ||
                span.min_y % edge != 0 || span.max_y % edge != 0) {
                return R::SpanRefused;
            }
        }
        if (!World::IsFarTileCovering(found->coverage) ||
            found->request.span.min_y >= found->request.span.max_y ||
            (found->intent.extra_span &&
             (!std::isfinite(found->intent.extra_span->min_y) ||
              !std::isfinite(found->intent.extra_span->max_y) ||
              found->intent.extra_span->min_y > found->intent.extra_span->max_y ||
              static_cast<double>(found->request.span.min_y) > found->intent.extra_span->min_y ||
              static_cast<double>(found->request.span.max_y) < found->intent.extra_span->max_y)))
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
        const auto number = static_cast<std::size_t>(key.tier) - 1;
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
    if (s.authority_kind == FarWorldAuthorityKind::Session &&
        submitted_water != s.submitted_water_indices) {
        return R::WaterIncomplete;
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

namespace Luminumbra::Client::Measurement {
FarBudgetObservation ProjectFarBudget(const Rendering::FarWorldDiagnosticSnapshot& snapshot,
                                      bool submission_allowed) noexcept {
    FarBudgetObservation result;
    result.render_frame = snapshot.frame;
    result.world_epoch = snapshot.world_epoch;
    result.target_serial = snapshot.target_serial;
    result.bake_generation = snapshot.bake_generation;
    result.content_identity = snapshot.content_identity;
    result.accepted = snapshot.captured && snapshot.complete && snapshot.world_epoch != 0 &&
                      snapshot.tile_count != 0 && snapshot.tile_count <= snapshot.tiles.size() &&
                      snapshot.source_failure.ok() &&
                      (snapshot.authority_kind == Rendering::FarWorldAuthorityKind::Fixture ||
                       snapshot.target_serial != 0);
    const auto reporting = Rendering::FarWorldReportState(snapshot, submission_allowed);
    result.current_coverage =
        reporting.coverage_settled && !Rendering::FarWorldReportSuppressed(reporting);
    const bool usable = snapshot.captured && snapshot.complete && snapshot.advanced &&
                        snapshot.selection.effective == Rendering::FarWorldMode::Ladder &&
                        !Rendering::FarWorldReportSuppressed(reporting) &&
                        snapshot.ladder_water_available && !snapshot.upload.horizon_refused;
    std::array<std::size_t, World::kFarTierCount> counts{};
    for (std::size_t i = 0; i < result.tiers.size(); ++i) {
        result.tiers[i].observation = snapshot.tiers[i];
        result.tiers[i].work = snapshot.upload.tiers[i];
        result.tiers[i].current_coverage = true;
    }
    // The diagnostic value view has at most 512 slots. No world/residency
    // lookup, allocation, callback or additional snapshot is performed here.
    std::uint64_t content = 0;
    for (std::size_t i = 0; i < std::min(snapshot.tile_count, snapshot.tiles.size()); ++i) {
        const auto& tile = snapshot.tiles[i];
        const auto& identity = tile.desired;
        if (identity.key.tier < 1 || identity.key.tier > result.tiers.size()) {
            result.accepted = false;
            continue;
        }
        const auto slot = static_cast<std::size_t>(identity.key.tier - 1);
        ++counts[slot];
        // Specified FNV-1a over little-endian uint64 words, XOR across tiles:
        // diagnostic fingerprints are independent of visitor/ledger ordering.
        std::uint64_t hash = 14695981039346656037ULL;
        const auto fold = [&](std::uint64_t word) {
            for (unsigned byte = 0; byte < 8; ++byte) {
                hash ^= word & 255U;
                hash *= 1099511628211ULL;
                word >>= 8;
            }
        };
        fold(identity.key.tier);
        fold(static_cast<std::uint64_t>(identity.key.tx));
        fold(static_cast<std::uint64_t>(identity.key.tz));
        fold(identity.content_hash);
        content ^= hash;
        fold(identity.world_epoch);
        fold(identity.bake_generation);
        fold(identity.span_generation);
        fold(identity.cave_policy);
        fold(static_cast<std::uint64_t>(snapshot.numeric));
        fold(tile.intent.extra_span.has_value());
        if (tile.intent.extra_span) {
            fold(std::bit_cast<std::uint32_t>(tile.intent.extra_span->min_y));
            fold(std::bit_cast<std::uint32_t>(tile.intent.extra_span->max_y));
        }
        result.target_cohort ^= hash;
        const bool binding_matches =
            identity.world_epoch == snapshot.world_epoch &&
            identity.bake_generation == snapshot.bake_generation && identity.content_hash != 0 &&
            snapshot.bound_cave_policy &&
            identity.cave_policy == static_cast<std::uint8_t>(*snapshot.bound_cave_policy) &&
            (snapshot.authority_kind == Rendering::FarWorldAuthorityKind::Fixture
                 ? identity.content_hash == snapshot.content_identity
                 : identity.content_hash == tile.accepted_content_identity);
        result.accepted = result.accepted && binding_matches;
        result.tiers[slot].current_coverage = result.tiers[slot].current_coverage &&
                                              binding_matches && tile.has_pair &&
                                              tile.built == identity && tile.water_complete &&
                                              World::IsFarTileCovering(tile.coverage);
    }
    if (!result.content_identity)
        result.content_identity = content;
    result.accepted = result.accepted && result.content_identity != 0 && result.target_cohort != 0;
    for (std::size_t i = 0; i < result.tiers.size(); ++i) {
        auto& tier = result.tiers[i];
        const auto& resident = tier.observation.resident;
        result.accepted = result.accepted && counts[i] == tier.observation.resident.wanted;
        tier.current_coverage =
            tier.current_coverage && usable && counts[i] != 0 && counts[i] == resident.wanted &&
            counts[i] == resident.covering && counts[i] == resident.settled && !resident.pending &&
            !resident.building && !resident.ready && !resident.request_rejected &&
            !resident.budget_evicted && !tier.observation.draw_refusals;
    }
    for (const auto& tier : result.tiers)
        result.current_coverage = result.current_coverage && tier.current_coverage;
    if (!result.accepted) {
        result.current_coverage = false;
        for (auto& tier : result.tiers)
            tier.current_coverage = false;
    }
    return result;
}
} // namespace Luminumbra::Client::Measurement
