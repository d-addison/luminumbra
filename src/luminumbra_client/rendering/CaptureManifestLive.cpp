#include "CaptureManifestLive.h"

#include "RenderPipeline.h"
#include "core/EngineVersion.h"
#include "luminumbra_common/core/GraphicsPresets.h"
#include "luminumbra_common/world/GameSession.h"

#include <string>

namespace Luminumbra::Rendering {

CaptureManifestInput ObserveCaptureManifest(const world::GameSession& session,
                                            const RenderPipeline& pipeline,
                                            int width,
                                            int height,
                                            const luminumbra::core::ResolvedGraphics& graphics,
                                            bool wireframe,
                                            bool ui_overlay) {
    CaptureManifestInput input;
    input.build_id = {std::string(luminumbra::core::GetEngineVersionString()),
                      "engine_version_string_empty"};
    input.asset_hash = {session.PresetContentHash(), "session_preset_content_identity_unavailable"};
    input.asset_hash_scope = "terrain_preset_and_resolved_dependency_content_identity";
    // The linked programs discard their expanded/define-injected sources. Disk
    // bytes can differ after reload failure; hashing those would invent provenance.
    // The roster also omits embedded compute/bake shaders. No source I/O here.
    input.shader_hash = {
        "", "compiled_sources_not_retained_including_embedded_compute_and_bake_shaders"};
    // The server composite includes authoritative sub-hashes; the client only
    // exposes a chunk-snapshot hash that requires a whole-world scan. Do not label
    // that different quantity as the authoritative world_hash or mutate via a fence.
    input.world_hash = {"", "client_has_no_cached_authoritative_world_hash"};
    input.simulation_tick = session.GetSimulationTickCount();
    input.world_seed = session.GetMetadata().seed;
    input.width = width;
    input.height = height;
    const auto* camera = pipeline.capture_camera();
    if (!camera || !SetCaptureManifestCamera(input, *camera)) {
        // Refuse unprepared or mismatched observations before any artifact write.
        input.near_plane = 0.0f;
        return input;
    }

    const auto& values = graphics.values;
    const auto runtime = pipeline.graphics_runtime_config();
    // Fixed roster, below the builder's 128-setting bound. Runtime diagnostics
    // replace resolved values wherever the pipeline exposes live overrides.
    input.render_settings = {
        {"quality_preset", std::string(luminumbra::core::GraphicsPresetName(graphics.base_preset))},
        {"preset_revision", std::uint64_t(graphics.preset_revision)},
        {"custom", graphics.custom},
        {"render_scale", double(runtime.render_scale)},
        {"internal_width", std::uint64_t(pipeline.internal_width())},
        {"internal_height", std::uint64_t(pipeline.internal_height())},
        {"grass_density", double(runtime.grass_density)},
        {"grass_enabled", runtime.grass_enabled},
        {"grass_fade_start_m", double(runtime.grass_fade_start_m)},
        {"grass_fade_end_m", double(runtime.grass_fade_end_m)},
        {"grass_candidates_per_column", std::int64_t(values.grass_candidates_per_column)},
        {"grass_instance_cap", std::int64_t(values.grass_instance_cap)},
        {"far_radius_m", double(runtime.far_radius_m)},
        {"far_outer_clip_m", double(runtime.far_outer_clip_m)},
        {"far_f1_boundary_m", values.far_f1_boundary_m},
        {"far_inner_clip_m", values.far_inner_clip_m},
        {"shadow_resolution", std::int64_t(runtime.shadow_resolution)},
        {"shadow_cascades", std::int64_t(values.shadow_cascades)},
        {"tinted_shadows", values.tinted_shadows},
        {"volumetric_quality", std::int64_t(runtime.volumetric_quality)},
        {"ao_quality", std::int64_t(pipeline.get_ssao_quality())},
        {"cloud_quality", std::int64_t(pipeline.get_cloud_quality())},
        {"cave_ao", values.cave_ao},
        {"cave_ao_distance_m", values.cave_ao_distance_m},
        {"cave_ao_floor", values.cave_ao_floor},
        {"cave_ao_thickness", values.cave_ao_thickness},
        {"cave_ao_steps", std::int64_t(values.cave_ao_steps)},
        {"god_rays", values.god_rays},
        {"god_ray_samples", std::int64_t(values.god_ray_samples)},
        {"water_ssr_min_steps", std::int64_t(values.water_ssr_min_steps)},
        {"water_ssr_max_steps", std::int64_t(values.water_ssr_max_steps)},
        {"water_caustics_resolution", std::int64_t(values.water_caustics_resolution)},
        {"analytic_aerial", values.analytic_aerial},
        {"taau", pipeline.taau_enabled()},
        {"taau_history_valid", pipeline.taau_history_valid()},
        {"taau_jitter_ndc_x", double(pipeline.taau_jitter_ndc().x)},
        {"taau_jitter_ndc_y", double(pipeline.taau_jitter_ndc().y)},
        {"gpu_sky_lut", pipeline.sky_lut_gpu_enabled()},
        {"exposure_meter", pipeline.auto_exposure_metered()},
        {"exposure_override", double(pipeline.get_exposure_override())},
        {"time_of_day", double(pipeline.get_time_of_day())},
        {"season_tick", pipeline.get_season_tick()},
        {"season_phase", double(pipeline.get_season_phase())},
        {"moon_illumination", double(pipeline.get_moon_illumination())},
        {"weather_type", std::int64_t(pipeline.get_weather_type())},
        {"weather_intensity", double(pipeline.get_weather_intensity())},
        {"wireframe", wireframe},
        {"ui_overlay", ui_overlay},
        {"ui_msaa_samples", std::int64_t(values.ui_msaa_samples)},
        {"ui_blur", std::int64_t(runtime.ui_blur)},
        {"live_menu_preview", values.live_menu_preview}};
    for (std::size_t index = 0; index < values.shadow_splits_m.size(); ++index)
        input.render_settings.emplace("shadow_split_" + std::to_string(index) + "_m",
                                      values.shadow_splits_m[index]);
    const auto& atmosphere = pipeline.atmosphere_params();
    input.render_settings.emplace("aerial_density", double(atmosphere.aerial_density));
    input.render_settings.emplace("aerial_max_distance", double(atmosphere.aerial_max_distance));
    input.render_settings.emplace("inscatter_strength", double(atmosphere.inscatter_strength));
    input.render_settings.emplace("atmosphere_warmth", double(atmosphere.warmth));
    const auto& weather = pipeline.get_weather_state();
    input.render_settings.emplace("weather_driven", weather.driven);
    input.render_settings.emplace("rain_intensity", double(weather.rain_intensity));
    input.render_settings.emplace("snow_intensity", double(weather.snow_intensity));
    input.render_settings.emplace("fog_density", double(weather.fog_density));
    input.render_settings.emplace("storm_intensity", double(weather.storm_intensity));
    input.render_settings.emplace("wetness", double(weather.wetness));
    input.render_settings.emplace("wind_strength", double(weather.wind_strength));
    input.render_settings.emplace("wind_direction_x", double(weather.wind_direction.x));
    input.render_settings.emplace("wind_direction_y", double(weather.wind_direction.y));
    input.render_settings.emplace("wind_direction_z", double(weather.wind_direction.z));
    const auto& clouds = pipeline.get_cloud_state();
    input.render_settings.emplace("clouds_enabled", clouds.enabled);
    input.render_settings.emplace("cloud_shadows_enabled", clouds.shadow_enabled);
    input.render_settings.emplace("cloud_scroll_x", double(clouds.scroll_offset.x));
    input.render_settings.emplace("cloud_scroll_y", double(clouds.scroll_offset.y));
    input.render_settings.emplace("cloud_coverage", double(clouds.coverage_amount));
    input.render_settings.emplace("cloud_biome_variation", double(clouds.biome_variation));
    input.render_settings.emplace("cloud_plane_height", double(clouds.plane_height));
    input.render_settings.emplace("cloud_shadow_strength", double(clouds.shadow_strength));
    const auto& lightning = pipeline.get_lightning_state();
    input.render_settings.emplace("lightning_active", lightning.active);
    input.render_settings.emplace("lightning_pulse_intensity", double(lightning.pulse_intensity));
    input.render_settings.emplace("lightning_bolt_width_ndc", double(lightning.bolt_width_ndc));
    input.render_settings.emplace("lightning_bolt_glow_ndc", double(lightning.bolt_glow_ndc));
    input.render_settings.emplace("lightning_ground_flash", double(lightning.ground_flash));
    input.render_settings.emplace("lightning_cloud_darkness", double(lightning.cloud_darkness));
    input.render_settings.emplace("isolation_layer_mask",
                                  std::uint64_t(pipeline.isolation_config().layer_mask));
    input.render_settings.emplace("isolation_backdrop",
                                  std::int64_t(pipeline.isolation_config().backdrop));
    input.render_settings.emplace("tree_impostor_enabled", pipeline.tree_impostor_enabled());
    const auto sun = pipeline.sun_direction();
    input.render_settings.emplace("sun_direction_x", double(sun.x));
    input.render_settings.emplace("sun_direction_y", double(sun.y));
    input.render_settings.emplace("sun_direction_z", double(sun.z));
    input.render_settings.emplace("previous_render_time", double(pipeline.prev_time()));
    return input;
}

} // namespace Luminumbra::Rendering
