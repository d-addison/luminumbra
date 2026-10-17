#include "FarWorldHandoffDiagnostics.h"
#include "luminumbra_client/rendering/FarVolumeHandoff.h"
#include "luminumbra_client/rendering/FarVolumeResidency.h"
#include "world/FarVolumeTile.h"
#include "world/FarWantedSet.h"
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <utility>

namespace Luminumbra::Rendering {
void CaptureFarWorldHandoffDiagnostic(FarVolumeHandoff& owner,
                                      std::uint64_t frame,
                                      const FarVolumeFrameResult& upload,
                                      FarWorldHandoffDiagnosticSnapshot& out) {
    const auto frame_upload = upload;
    out = {};
    out.captured = true;
    out.frame = frame;
    out.state = owner.State();
    out.upload = frame_upload;
    out.memory = owner.Memory();
    struct Context {
        FarVolumeHandoff& owner;
        FarWorldHandoffDiagnosticSnapshot& out;
    } context{owner, out};
    owner.VisitObservations(
        [](void* opaque, const World::FarTileKey& key, const FarHandoffTileObservation& value) {
            auto& c = *static_cast<Context*>(opaque);
            if (c.out.tile_count == c.out.tiles.size()) {
                c.out.complete = false;
                return;
            }
            c.out.tiles[c.out.tile_count++] = {
                key,
                value,
                c.owner.CoarseMayYield(key, FarHandoffCoverageView::Serving),
                c.owner.CoarseMayYield(key, FarHandoffCoverageView::Target)};
        },
        &context);
    owner.VisitServingDraws(
        [](void* opaque, const FarVolumeDrawRecord& record) {
            auto& c = *static_cast<Context*>(opaque);
            if (c.out.draw_count == c.out.draws.size()) {
                c.out.complete = false;
                return;
            }
            auto& draw = c.out.draws[c.out.draw_count++];
            draw = {record.identity, record.request, record.arrival, {}};
            if (draw.arrival.eligible)
                for (std::int64_t z = 0; z < 2; ++z)
                    for (std::int64_t x = 0; x < 2; ++x) {
                        const auto child =
                            c.owner.Observe({std::uint8_t(record.identity.key.tier - 1),
                                             record.identity.key.tx * 2 + x,
                                             record.identity.key.tz * 2 + z});
                        if (!child || !child->serving) {
                            c.out.complete = false;
                            return;
                        }
                        draw.child_identities[std::size_t(x + 2 * z)] = child->serving->identity;
                    }
        },
        &context);
}
bool FarWorldHandoffTargetReady(const FarWorldHandoffDiagnosticSnapshot& snapshot) noexcept {
    if (!snapshot.captured || !snapshot.complete || !snapshot.state.target_serial)
        return false;
    std::size_t targets = 0;
    for (std::size_t i = 0; i < snapshot.tile_count; ++i) {
        const auto& tile = snapshot.tiles[i].observation;
        if (tile.target) {
            ++targets;
            if (!World::IsFarTileCovering(tile.target_coverage) || !tile.target_water_complete)
                return false;
        }
    }
    return targets != 0;
}
nlohmann::json FarWorldHandoffDiagnosticJson(const FarWorldHandoffDiagnosticSnapshot& s) {
    const auto identity = [](const World::FarBuildIdentity& id) {
        return nlohmann::json{{"world_epoch", id.world_epoch},
                              {"content_hash", id.content_hash},
                              {"cave_policy", id.cave_policy},
                              {"tier", id.key.tier},
                              {"tx", id.key.tx},
                              {"tz", id.key.tz},
                              {"bake_generation", id.bake_generation},
                              {"span_generation", id.span_generation}};
    };
    const auto span = [](const World::FarVolumeSpan& value) {
        return nlohmann::json::array({value.min_y, value.max_y});
    };
    nlohmann::json result{{"schema", "luminumbra.far_handoff.v1"},
                          {"frame", s.frame},
                          {"captured", s.captured},
                          {"complete", s.complete},
                          {"world_epoch", s.state.world_epoch},
                          {"committed_generation", s.state.committed_generation},
                          {"serving_committed_generation", s.state.serving_committed_generation},
                          {"serving_serial", s.state.serving_serial},
                          {"target_serial", s.state.target_serial},
                          {"awaiting_initial_fallback", s.state.awaiting_initial_fallback},
                          {"awaiting_replacement_fallback", s.state.awaiting_replacement_fallback},
                          {"promotions", s.state.promotions},
                          {"promotion_reason", int(s.state.last_promotion)},
                          {"upload_attempts", s.upload.upload_attempts},
                          {"attempted_upload_bytes", s.upload.upload_bytes},
                          {"successful_upload_bytes", s.upload.successful_upload_bytes},
                          {"submitted_draws", s.submission.draws},
                          {"submitted_indices", s.submission.indices},
                          {"submitted_water_indices", s.submission.water_indices},
                          {"submission_refusals", s.submission.refused},
                          {"target_ready", FarWorldHandoffTargetReady(s)},
                          {"tiles", nlohmann::json::array()},
                          {"draws", nlohmann::json::array()},
                          {"memory", nlohmann::json::array()}};
    for (const auto& m : s.memory)
        result["memory"].push_back({{"limit", m.limit},
                                    {"cpu_bytes", m.bytes_cpu},
                                    {"gpu_bytes", m.bytes_gpu},
                                    {"total_bytes", m.bytes_total},
                                    {"peak_bytes", m.peak_total},
                                    {"admission_refusals", m.admission_refusals}});
    for (std::size_t i = 0; i < s.tile_count; ++i) {
        const auto& t = s.tiles[i];
        const auto& o = t.observation;
        nlohmann::json tile{{"tier", t.key.tier},
                            {"tx", t.key.tx},
                            {"tz", t.key.tz},
                            {"target_eligibility", int(o.target_eligibility)},
                            {"serving_covering", World::IsFarTileCovering(o.serving_coverage)},
                            {"target_covering", World::IsFarTileCovering(o.target_coverage)},
                            {"serving_state", int(o.serving_coverage.state)},
                            {"target_state", int(o.target_coverage.state)},
                            {"serving_water_complete", o.serving_water_complete},
                            {"target_water_complete", o.target_water_complete},
                            {"serving_water_known_dry", o.serving_water_known_dry},
                            {"target_water_known_dry", o.target_water_known_dry},
                            {"serving_terrain_indices", o.serving_terrain_indices},
                            {"serving_water_indices", o.serving_water_indices},
                            {"target_terrain_indices", o.target_terrain_indices},
                            {"target_water_indices", o.target_water_indices},
                            {"serving_built_span", span(o.serving_built.span)},
                            {"target_built_span", span(o.target_built.span)},
                            {"serving_arrival", t.serving_arrival},
                            {"target_arrival", t.target_arrival},
                            {"ready_cpu_bytes", o.ready_cpu_bytes},
                            {"serving_gpu_bytes", o.serving_gpu_bytes},
                            {"staged_gpu_bytes", o.staged_gpu_bytes},
                            {"ready_upload_bytes", o.ready_upload_bytes},
                            {"serving_source_payload_bytes", o.serving_source_payload_bytes},
                            {"target_source_payload_bytes", o.target_source_payload_bytes},
                            {"serving_older_than_target",
                             o.serving && o.target && o.serving->identity != o.target->identity}};
        for (const bool serving : {true, false}) {
            const auto& request = serving ? o.serving : o.target;
            const auto name = serving ? "serving" : "target";
            tile[name] = nullptr;
            if (request) {
                tile[name] = identity(request->identity);
                tile[name]["requested_span"] = nullptr;
                if (request->intent.extra_span)
                    tile[name]["requested_span"] = nlohmann::json::array(
                        {request->intent.extra_span->min_y, request->intent.extra_span->max_y});
            }
        }
        result["tiles"].push_back(std::move(tile));
        auto& added = result["tiles"].back();
        added["target_build_failure"] = nullptr;
        if (o.target_build_failure)
            added["target_build_failure"] = int(*o.target_build_failure);
        added["target_upload_failure"] = nullptr;
        if (o.target_upload_failure)
            added["target_upload_failure"] = int(*o.target_upload_failure);
    }
    for (std::size_t i = 0; i < s.draw_count; ++i) {
        const auto& d = s.draws[i];
        auto draw = identity(d.identity);
        draw["built_span"] = span(d.request.span);
        draw["arrival_eligible"] = d.arrival.eligible;
        draw["submitted"] = d.submitted;
        draw["children"] = nlohmann::json::array();
        if (d.arrival.eligible)
            for (std::size_t j = 0; j < 4; ++j) {
                auto child = identity(d.child_identities[j]);
                child["built_span"] = span(d.arrival.children[j]);
                draw["children"].push_back(std::move(child));
            }
        result["draws"].push_back(std::move(draw));
    }
    return result;
}
} // namespace Luminumbra::Rendering
