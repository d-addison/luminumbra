#pragma once
#include "core/FoliageVisualContract.h"
#include "rendering/FoliageEvidence.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>

namespace Luminumbra::Client::ScenarioHarness {
inline nlohmann::json BuildFoliageQualification(const Rendering::FoliageQualificationEvidence& e,
                                                const FoliagePhaseEvidence& calm,
                                                const FoliagePhaseEvidence& windy) {
    const auto& r = e.rebuild;
    const bool rebuild = calm.from_gpu_readback && windy.from_gpu_readback && e.frozen_inputs &&
                         r.completed && r.output_cleared && r.byte_equal && r.input_hash &&
                         r.first_generation && r.first_generation < r.second_generation &&
                         r.second_generation == calm.build_generation && r.first_frame &&
                         r.first_frame < r.second_frame && r.second_frame == calm.build_frame &&
                         r.first_count >= FoliageVisualProfile::minimum_instances &&
                         r.first_count == r.second_count && r.first_hash &&
                         r.first_hash == r.second_hash && r.second_hash == calm.instance_hash;
    nlohmann::json determinism = {
        {"status", r.completed ? "evaluated" : "unevaluated"},
        {"passed", rebuild},
        {"scope", "same-device frozen-input independent GPU dispatch; not cross-host determinism"},
        {"input_hash", r.input_hash},
        {"first_generation", r.first_generation},
        {"second_generation", r.second_generation},
        {"first_frame", r.first_frame},
        {"second_frame", r.second_frame},
        {"first_count", r.first_count},
        {"second_count", r.second_count},
        {"first_hash", r.first_hash},
        {"second_hash", r.second_hash},
        {"output_cleared", r.output_cleared},
        {"byte_equal", r.byte_equal},
        {"completed", r.completed},
        {"first_artifact", "foliage-rebuild-first.bin"},
        {"second_artifact", "foliage-rebuild-second.bin"},
        {"stride_bytes", 36},
        {"global_rng", false},
        {"world_hash_written", false}};
    nlohmann::json phases = nlohmann::json::array();
    bool motion = true;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& v = e.vertices[i];
        const auto& p = i == 0 ? calm : windy;
        motion = motion && v.vertices.size() == 768 && v.blade_heights.size() == 64 &&
                 v.sways.size() == 64 && v.phase == i + 1 && v.source_frame == p.capture_frame &&
                 v.source_frame > 0 && v.available_frame >= v.source_frame &&
                 v.generation == p.build_generation && v.instance_hash == p.instance_hash &&
                 std::uint64_t(v.first_instance) + 64 <= r.first_count && v.shader_time == 1.0f &&
                 p.shader_time_seconds == v.shader_time;
        const auto expected = FoliageExpectedViewProjection(p);
        for (std::size_t k = 0; k < 16; ++k)
            motion = motion && std::isfinite(v.view_projection[k]) &&
                     std::abs(expected[k] - v.view_projection[k]) <= 0.002;
        phases.push_back({{"phase", v.phase},
                          {"source_frame", v.source_frame},
                          {"available_frame", v.available_frame},
                          {"generation", v.generation},
                          {"instance_hash", v.instance_hash},
                          {"first_instance", v.first_instance},
                          {"shader_time_seconds", v.shader_time},
                          {"view_projection", v.view_projection},
                          {"vertices", v.vertices},
                          {"blade_heights", v.blade_heights},
                          {"sways", v.sways}});
    }
    const auto& a = e.vertices[0];
    const auto& b = e.vertices[1];
    motion = motion && a.first_instance == b.first_instance && a.blade_heights == b.blade_heights &&
             a.sways == b.sways && a.view_projection == b.view_projection;
    for (const float value : a.view_projection)
        motion = motion && std::isfinite(value);
    double max_root = 0, max_tip = 0, max_control = 0;
    std::size_t moving_tips = 0, controls = 0, visible_sway = 0;
    if (motion) {
        for (std::size_t blade = 0; blade < 64; ++blade) {
            const double height = a.blade_heights[blade];
            motion = motion && std::isfinite(height) && height > 0 && height <= 10;
            if (!a.sways[blade])
                ++controls;
            bool visible = false;
            for (std::size_t j = 0; j < 12; ++j) {
                const auto& va = a.vertices[blade * 12 + j];
                const auto& vb = b.vertices[blade * 12 + j];
                const bool tip = j % 6 == 2 || j % 6 == 4 || j % 6 == 5;
                for (std::size_t c = 0; c < 8; ++c)
                    motion = motion && std::isfinite(va[c]) && std::isfinite(vb[c]);
                motion = motion && va[3] == (tip ? 1 : 0) && vb[3] == va[3];
                for (const auto* vertex : {&va, &vb}) {
                    for (std::size_t row = 0; row < 4; ++row) {
                        double projected = a.view_projection[12 + row];
                        for (std::size_t col = 0; col < 3; ++col)
                            projected += double(a.view_projection[col * 4 + row]) * (*vertex)[col];
                        motion = motion && std::abs(projected - (*vertex)[4 + row]) <= 0.002;
                    }
                }
                const double dx = double(vb[0]) - va[0], dy = double(vb[1]) - va[1],
                             dz = double(vb[2]) - va[2];
                const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (!tip)
                    max_root = std::max(max_root, distance);
                else if (!a.sways[blade])
                    max_control = std::max(max_control, distance);
                else {
                    max_tip = std::max(max_tip, distance);
                    if (distance > 0.00001)
                        ++moving_tips;
                    motion = motion && distance > 0.00001 && distance <= height * 0.45 + 0.0001 &&
                             std::abs(dy) <= 0.00001;
                }
                if (tip && va[7] > 0 && std::abs(va[4]) < va[7] && std::abs(va[5]) < va[7] &&
                    va[6] > 0 && va[6] < va[7] && vb[7] > 0 && std::abs(vb[4]) < vb[7] &&
                    std::abs(vb[5]) < vb[7] && vb[6] > 0 && vb[6] < vb[7])
                    visible = true;
            }
            if (visible && a.sways[blade])
                ++visible_sway;
        }
    }
    motion = motion && max_root <= 0.00001 && max_control <= 0.00001 && controls > 0 &&
             visible_sway >= 8 && moving_tips >= 48;
    nlohmann::json rendered = {
        {"status", phases[0]["vertices"].empty() ? "unevaluated" : "evaluated"},
        {"passed", motion},
        {"scope", "actual rasterized draw vertex outputs; not occlusion or visual approval"},
        {"format", "world_xyz,height_t,clip_xyzw.float32"},
        {"windy_instance_artifact", "foliage-windy.bin"},
        {"vertices_per_instance", 12},
        {"instances_per_phase", 64},
        {"rasterizer_discard", false},
        {"maximum_root_displacement_m", max_root},
        {"maximum_tip_displacement_m", max_tip},
        {"maximum_control_displacement_m", max_control},
        {"visible_swaying_instances", visible_sway},
        {"non_swaying_controls", controls},
        {"phases", phases}};
    std::array<std::size_t, 2> counts{};
    std::set<std::uint64_t> source_frames;
    nlohmann::json samples = nlohmann::json::array();
    bool gpu = e.gpu.size() == 64 && !e.gl_vendor.empty() && !e.gl_renderer.empty() &&
               !e.gl_version.empty();
    double max_ms = 0, total_ms = 0;
    std::uint64_t previous_frame = 0;
    for (std::size_t i = 0; i < e.gpu.size(); ++i) {
        const auto& q = e.gpu[i];
        const bool phase_valid = q.phase == 1 || q.phase == 2;
        const auto& p = q.phase == 1 ? calm : windy;
        const bool valid =
            phase_valid && q.query_id == i + 1 && q.source_frame > previous_frame &&
            q.source_frame >= p.available_frame && q.source_frame < p.capture_frame &&
            q.generation == p.build_generation && q.instance_generation == p.instance_generation &&
            q.instance_count == r.first_count && q.shader_time == p.shader_time_seconds &&
            std::isfinite(q.milliseconds) && q.milliseconds > 0;
        gpu = gpu && valid && source_frames.insert(q.source_frame).second;
        if (phase_valid)
            ++counts[q.phase - 1];
        previous_frame = q.source_frame;
        if (std::isfinite(q.milliseconds) && q.milliseconds > 0) {
            max_ms = std::max(max_ms, q.milliseconds);
            total_ms += q.milliseconds;
        }
        samples.push_back(
            {{"query_id", q.query_id},
             {"source_frame", q.source_frame},
             {"phase", q.phase},
             {"generation", q.generation},
             {"instance_generation", q.instance_generation},
             {"instance_count", q.instance_count},
             {"shader_time_seconds", q.shader_time},
             {"milliseconds",
              q.milliseconds > 0 && std::isfinite(q.milliseconds) ? nlohmann::json(q.milliseconds)
                                                                  : nlohmann::json(nullptr)}});
    }
    gpu = gpu && counts[0] == 32 && counts[1] == 32;
    const bool budget = gpu && max_ms <= FoliageVisualProfile::gpu_budget_ms;
    nlohmann::json timer = {
        {"status", gpu ? "source_correlated" : "unavailable"},
        {"qualified", gpu},
        {"within_budget", budget},
        {"budget_ms", FoliageVisualProfile::gpu_budget_ms},
        {"budget_statistic", "maximum of 32 ordinary draw frames per phase"},
        {"maximum_ms", gpu ? nlohmann::json(max_ms) : nlohmann::json(nullptr)},
        {"mean_ms", gpu ? nlohmann::json(total_ms / 64) : nlohmann::json(nullptr)},
        {"instrumented_vertex_frames_excluded", true},
        {"samples", samples},
        {"gl_vendor", e.gl_vendor},
        {"gl_renderer", e.gl_renderer},
        {"gl_version", e.gl_version}};
    std::vector<std::string> missing;
    if (!rebuild)
        missing.emplace_back("independent rebuild determinism");
    if (!motion)
        missing.emplace_back("rendered geometric wind response");
    if (!gpu)
        missing.emplace_back("source-frame-correlated GPU samples");
    return {{"determinism", determinism},
            {"rendered_motion", rendered},
            {"gpu_timer", timer},
            {"qualification",
             {{"status", missing.empty() ? "complete" : "incomplete"},
              {"missing", missing},
              {"visual_approved", false}}},
            {"passed", rebuild && motion && budget}};
}
} // namespace Luminumbra::Client::ScenarioHarness
