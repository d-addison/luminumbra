#pragma once

#include "RenderMeasurement.h"
#include "luminumbra_common/core/Environment.h"
#include <nlohmann/json.hpp>

namespace Luminumbra::Client::Measurement {

inline nlohmann::json optional_value(const std::optional<double>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

inline void add_far_world_reporting(nlohmann::json& report, SampleRing& ring) {
    auto& block = report["far_world"];
    block["frames"] = nlohmann::json::array();
    nlohmann::json summary = {{"requested_mode", nlohmann::json::object()},
                              {"effective_mode", nlohmann::json::object()},
                              {"refusal", nlohmann::json::object()},
                              {"submission_refusal", nlohmann::json::object()},
                              {"suppressed_frames", 0},
                              {"unsettled_frames", 0}};
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto& sample = ring.at(ring.first() + i);
        const auto& far = sample.far_world;
        block["frames"].push_back(
            {{"frame", sample.frame},
             {"requested_mode", far.requested_mode},
             {"effective_mode", far.effective_mode},
             {"refusal", far.refusal},
             {"submission_refusal", far.submission_refusal},
             {"suppressed", far.suppressed},
             {"coverage_settled", far.coverage_settled},
             {"streaming",
              {{"generation_pending", far.generation_pending},
               {"meshing_pending", far.meshing_pending},
               {"generation_outstanding", far.generation_outstanding},
               {"meshing_outstanding", far.meshing_outstanding},
               {"terrain_uploads_pending", far.terrain_uploads_pending}}},
             {"content_identity", std::to_string(far.content_identity)},
             {"settings",
              {{"gns_compiled", far.gns_compiled},
               {"active_regions", far.active_regions},
               {"water_cells_per_side", far.water_cells_per_side},
               {"grass_density_scale",
                far.grass_density_scale ? nlohmann::json(*far.grass_density_scale)
                                        : nlohmann::json(nullptr)}}}});
        for (const auto& [field, value] :
             {std::pair{"requested_mode", far.requested_mode},
              std::pair{"effective_mode", far.effective_mode},
              std::pair{"refusal", far.refusal},
              std::pair{"submission_refusal", far.submission_refusal}}) {
            auto& count = summary[field][value];
            count = count.is_null() ? 1 : count.get<std::size_t>() + 1;
        }
        summary["suppressed_frames"] =
            summary["suppressed_frames"].get<std::size_t>() + far.suppressed;
        summary["unsettled_frames"] =
            summary["unsettled_frames"].get<std::size_t>() + !far.settled();
    }
    block["summary"] = std::move(summary);
}

inline void add_distributions(nlohmann::json& report, SampleRing& ring) {
    const std::array<const char*, 5> names = {
        "frame_wall_ms", "cpu_submit_ms", "present_ms", "gpu_frame_ms", "gpu_pass_sum_ms"};
    std::array<std::vector<double>, 5> metrics;
    std::array<std::vector<double>, kPassCount> passes;
    report["frame_samples"] = nlohmann::json::array();
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const auto& sample = ring.at(ring.first() + i);
        nlohmann::json row = {{"frame", sample.frame}, {"gpu_frame", sample.frame}};
        const std::array<std::optional<double>, 5> values = {
            sample.wall, sample.cpu, sample.present, sample.gpu, sample.passSum};
        for (std::size_t metric = 0; metric < names.size(); ++metric) {
            row[names[metric]] = optional_value(values[metric]);
            const auto& value = values[metric];
            if (value)
                metrics[metric].push_back(*value);
        }
        for (std::size_t pass = 0; pass < kPassCount; ++pass) {
            row["passes"][kPassNames[pass]] = {{"issued", sample.issued[pass]},
                                               {"ms", optional_value(sample.passes[pass])}};
            const auto& value = sample.passes[pass];
            if (value)
                passes[pass].push_back(*value);
            else if (!sample.issued[pass] && sample.gpu)
                passes[pass].push_back(0.0); // known no dispatch, not an unavailable query
        }
        report["frame_samples"].push_back(std::move(row));
    }
    const auto mean = [&](const std::vector<double>& values) -> nlohmann::json {
        if (values.size() != ring.size() || values.empty())
            return nullptr;
        double sum = 0.0;
        for (double value : values)
            sum += value;
        return sum / static_cast<double>(values.size());
    };
    for (std::size_t metric = 0; metric < names.size(); ++metric) {
        auto& block = report["distribution"][names[metric]];
        block = {{"unit", "ms"},
                 {"sample_count", metrics[metric].size()},
                 {"unavailable_count", ring.size() - metrics[metric].size()}};
        const std::array<const char*, 5> statistics = {"p50", "p95", "p99", "max", "mad"};
        if (metrics[metric].empty()) {
            for (const char* name : statistics)
                block[name] = nullptr;
        } else {
            const auto stats = summarize(metrics[metric]);
            for (std::size_t stat = 0; stat < statistics.size(); ++stat)
                block[statistics[stat]] = stats[stat];
        }
        report["avg"][names[metric]] = mean(metrics[metric]);
    }
    for (std::size_t pass = 0; pass < kPassCount; ++pass)
        report["avg_ms"][kPassNames[pass]] = mean(passes[pass]);
    report["avg_ms"]["total"] = report["avg"]["gpu_pass_sum_ms"];
    report["avg_ms"]["ssao_total"] =
        report["avg_ms"]["ssao"].is_number() && report["avg_ms"]["ssao_blur"].is_number()
            ? nlohmann::json(report["avg_ms"]["ssao"].get<double>() +
                             report["avg_ms"]["ssao_blur"].get<double>())
            : nlohmann::json(nullptr);
}

inline nlohmann::json render_overrides(int argc, char** argv) {
    nlohmann::json result = nlohmann::json::object();
    // Treat every non-workload CLI option as an override requiring an exact declaration.
    const std::vector<std::string> flags = {
        "--no-audio", "--auto-create-world", "--auto-enter-world", "--no-menu-backdrop"};
    const std::vector<std::string> options = {"--render-benchmark",
                                              "--render-benchmark-schema",
                                              "--render-benchmark-frames",
                                              "--render-benchmark-warmup",
                                              "--render-benchmark-screenshot",
                                              "--capture-size",
                                              "--load-world",
                                              "--perf-profile",
                                              "--traversal",
                                              "--declare-render-overrides",
                                              "--world-preset",
                                              "--cam-pos",
                                              "--cam-yaw",
                                              "--cam-pitch",
                                              "--window-mode"};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (std::find(flags.begin(), flags.end(), arg) != flags.end())
            continue;
        const bool hasValue = i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0;
        if (std::find(options.begin(), options.end(), arg) != options.end()) {
            if (hasValue)
                ++i;
            continue;
        }
        result["cli:" + arg] = hasValue ? std::string(argv[++i]) : "true";
    }
    for (const char* name : {"LUMIN_GL_DEBUG",
                             "LUMIN_RENDER_SCALE",
                             "LUMIN_TREE_IMPOSTORS",
                             "LUMIN_ATMOS",
                             "LUMIN_MOON",
                             "LUMIN_CLOUD_QUALITY",
                             "LUMIN_SSAO_QUALITY",
                             "LUMIN_MOON_WRAP_FLOOR",
                             "LUMIN_GRADE",
                             "LUMIN_CAVE_AO",
                             "LUMIN_SCENT_DECAL",
                             "LUMIN_RHI",
                             "LUMIN_FRAME_SCAN_SETTLE",
                             "LUMINUMBRA_VISUAL_SWEEP",
                             "LUMINUMBRA_VISUAL_SWEEP_WINTER",
                             "LUMINUMBRA_ATMOS_MOTION_CAPTURE",
                             "LUMINUMBRA_JOB_THROTTLE"}) {
        if (const auto value = Core::ReadEnvironment(name))
            result[std::string("env:") + name] = *value;
    }
    return result;
}
} // namespace Luminumbra::Client::Measurement
