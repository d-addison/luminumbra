#pragma once

#include "core/FoliageVisualContract.h"
#include <nlohmann/json.hpp>

namespace Luminumbra::Client::ScenarioHarness {

// Controlled foliage observations. Independent rebuild determinism, rendered
// displacement and correlated GPU timing are separate, still-open qualifications.
struct FoliageInstancingResult {
    std::uint64_t world_seed = 0;
    FoliagePhaseEvidence calm;
    FoliagePhaseEvidence windy;
    float density_multiplier = 1.0f;
    std::size_t instances_within_ring = 0;
    std::size_t instances_total = 0;
    double measured_density = 0.0; // Calibrated count ratio, not emitted candidate fraction.
    double biome_density = 0.0;
    double biome_density_band = 0.6;
    std::size_t instances_beyond_fade = 0;
    double fade_start_m = 0.0;
    double fade_end_m = 0.0;
    double live_ring_radius_m = 0.0;
    double foliage_gpu_ms = 0.0; // Retained overlay value; source frame is unavailable.
    bool gpu_timers_supported = false;
    std::size_t foliage_draws = 0;
    std::size_t foliage_instances_drawn = 0;
};

inline nlohmann::json FoliagePhaseReport(const FoliagePhaseEvidence& sample) {
    if (!sample.sampled) {
        return {{"status", "unavailable"}, {"sampled", false}};
    }
    return {{"status", "sampled"},
            {"sampled", true},
            {"phase", sample.phase == 1 ? "calm" : "windy"},
            {"capture_frame", sample.capture_frame},
            {"simulation_tick", sample.simulation_tick},
            {"build_generation", sample.build_generation},
            {"instance_source", sample.from_gpu_readback ? "gpu_readback" : "cpu_scatter"},
            {"instance_generation", sample.instance_generation},
            {"readback_generation",
             sample.from_gpu_readback ? nlohmann::json(sample.instance_generation)
                                      : nlohmann::json(nullptr)},
            {"readback_consumed_frame",
             sample.from_gpu_readback ? nlohmann::json(sample.available_frame)
                                      : nlohmann::json(nullptr)},
            {"build_frame", sample.build_frame},
            {"instance_available_frame", sample.available_frame},
            {"instance_matches_drawn_build",
             FoliageInstanceMatches(sample.build_generation,
                                    sample.instance_generation,
                                    sample.build_frame,
                                    sample.available_frame,
                                    sample.capture_frame)},
            {"instance_snapshot_hash", sample.instance_hash},
            {"shader_time_seconds", sample.shader_time_seconds},
            {"camera",
             {{"position", sample.camera},
              {"terrain_height_m", sample.terrain_height_m},
              {"yaw_degrees", sample.yaw_degrees},
              {"pitch_degrees", sample.pitch_degrees},
              {"vertical_fov_degrees", sample.fov_degrees}}},
            {"time_of_day", sample.time_of_day},
            {"fade_start_m", sample.fade_start_m},
            {"fade_end_m", sample.fade_end_m},
            {"density_scale", sample.density_scale},
            {"wind_input_xz", sample.wind_input},
            {"maximum_instance_wind_magnitude", sample.maximum_instance_wind},
            {"width", sample.width},
            {"height", sample.height},
            {"screenshot", sample.screenshot}};
}

// Pure producer policy shared by the native writer and GL-free result tests.
inline nlohmann::json BuildFoliageInstancingReport(const FoliageInstancingResult& result,
                                                   std::uint64_t gl_errors) {
    const bool controls =
        FoliageControlsMatch(result.calm, result.windy, result.density_multiplier);
    const bool density = FoliageCoveragePasses(result.instances_within_ring,
                                               result.measured_density,
                                               result.biome_density,
                                               result.biome_density_band);
    const bool fade = result.instances_beyond_fade == 0 &&
                      result.fade_start_m == FoliageVisualProfile::fade_start_m &&
                      result.fade_end_m == FoliageVisualProfile::fade_end_m;
    const bool functional =
        controls && density && fade && result.foliage_draws > 0 &&
        result.foliage_instances_drawn >= FoliageVisualProfile::minimum_instances && gl_errors == 0;
    const bool timer_value = result.gpu_timers_supported && std::isfinite(result.foliage_gpu_ms) &&
                             result.foliage_gpu_ms > 0.0;
    return {
        {"schema", "luminumbra.foliage_instancing.v2"},
        {"profile", FoliageVisualProfile::id},
        {"passed", false},
        {"qualification",
         {{"status", "incomplete"},
          {"missing",
           {"independent rebuild determinism",
            "rendered geometric wind response",
            "source-frame-correlated GPU samples"}},
          {"visual_approved", false}}},
        {"functional_control", {{"passed", functional}, {"final_state_matches_profile", controls}}},
        {"profile_settings",
         {{"fade_start_m", FoliageVisualProfile::fade_start_m},
          {"fade_end_m", FoliageVisualProfile::fade_end_m},
          {"base_density_scale", FoliageVisualProfile::density_scale},
          {"requested_density_multiplier", result.density_multiplier},
          {"camera_changed_from_v1", true},
          {"minimum_instances", FoliageVisualProfile::minimum_instances}}},
        {"phases",
         {{"calm", FoliagePhaseReport(result.calm)}, {"windy", FoliagePhaseReport(result.windy)}}},
        {"determinism",
         {{"status", "unevaluated"},
          {"passed", nullptr},
          {"reason",
           "Snapshot hashes describe two different wind phases; no independent reconstruction was "
           "compared."},
          {"world_seed", result.world_seed},
          {"global_rng", false},
          {"world_hash_written", false}}},
        {"coverage_density",
         {{"passed", density},
          {"instances_within_ring", result.instances_within_ring},
          {"instances_total", result.instances_total},
          {"minimum_instances", FoliageVisualProfile::minimum_instances},
          {"calibrated_count_ratio", result.measured_density},
          {"normalizer", 873813.0},
          {"biome_density", result.biome_density},
          {"density_band", result.biome_density_band},
          {"meaning",
           "Historical count calibration; not candidate emission fraction or screen coverage."}}},
        {"distance_fade",
         {{"passed", fade},
          {"instances_beyond_fade", result.instances_beyond_fade},
          {"fade_start_m", result.fade_start_m},
          {"fade_end_m", result.fade_end_m},
          {"live_ring_radius_m", result.live_ring_radius_m}}},
        {"wind_input_response",
         {{"passed", controls},
          {"units", "wind-field input units"},
          {"meaning", "Per-instance raw wind magnitude, not vertex or tip displacement."}}},
        {"rendered_motion",
         {{"status", "unevaluated"},
          {"tip_displacement_m", nullptr},
          {"reason", "Ordered stills and input readback do not measure rendered vertex motion."}}},
        {"gpu_timer",
         {{"status", timer_value ? "unqualified_observation" : "unavailable"},
          {"foliage_gpu_ms",
           timer_value ? nlohmann::json(result.foliage_gpu_ms) : nlohmann::json(nullptr)},
          {"budget_ms", FoliageVisualProfile::gpu_budget_ms},
          {"within_budget", nullptr},
          {"observed_within_budget",
           timer_value
               ? nlohmann::json(result.foliage_gpu_ms <= FoliageVisualProfile::gpu_budget_ms)
               : nlohmann::json(nullptr)},
          {"supported", result.gpu_timers_supported},
          {"source_frame", nullptr},
          {"reason", "Retained timer value has no source-frame identity or unique sample count."}}},
        {"render_pass",
         {{"foliage_draws", result.foliage_draws},
          {"foliage_instances_drawn", result.foliage_instances_drawn}}}};
}

} // namespace Luminumbra::Client::ScenarioHarness
