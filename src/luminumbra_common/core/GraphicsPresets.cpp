#include "core/GraphicsPresets.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

namespace luminumbra::core {
namespace {

// Explicit revision snapshot: changing struct defaults cannot alter saved revision 1.
constexpr GraphicsValues kUltraRevision1{
    .render_scale = 1.0,
    .grass_density = 1.6,
    .grass_fade_start_m = 48.0,
    .grass_fade_end_m = 92.0,
    .far_terrain_radius_m = 3000.0,
    .far_outer_clip_m = 3050.0,
    .far_f1_boundary_m = 768.0,
    .far_inner_clip_m = 176.0,
    .cave_ao_distance_m = 24.0,
    .cave_ao_floor = 0.06,
    .cave_ao_thickness = 1.5,
    .shadow_splits_m = {0.1, 15.0, 40.0, 100.0, 250.0},
    .shadow_resolution = 2048,
    .volumetric_quality = 1,
    .grass_candidates_per_column = 8192,
    .grass_instance_cap = 262144,
    .shadow_cascades = 4,
    .ao_quality = 3,
    .cloud_quality = 2,
    .cave_ao_steps = 8,
    .god_ray_samples = 24,
    .water_ssr_min_steps = 4,
    .water_ssr_max_steps = 8,
    .water_caustics_resolution = 256,
    .ui_msaa_samples = 2,
    .grass_enabled = true,
    .ui_blur = UiBlurQuality::Full,
    .tinted_shadows = true,
    .analytic_aerial = true,
    .cave_ao = true,
    .god_rays = true,
    .taau = true,
    .gpu_sky_lut = true,
    .exposure_meter = false,
    .live_menu_preview = true,
};

// Ultra preserves packaged gameplay; High changes only grass.
constexpr auto kRevision1 = [] {
    std::array values{kUltraRevision1, kUltraRevision1, kUltraRevision1, kUltraRevision1};
    values[0].render_scale = 0.67;
    values[0].grass_density = 0.4;
    values[0].grass_fade_start_m = 24.0;
    values[0].grass_fade_end_m = 48.0;
    values[0].far_terrain_radius_m = 1500.0;
    values[0].far_outer_clip_m = 1550.0;
    values[0].shadow_resolution = 1024;
    values[0].volumetric_quality = 0;
    values[0].ui_blur = UiBlurQuality::Off;
    values[1].render_scale = 0.80;
    values[1].grass_density = 0.8;
    values[1].grass_fade_start_m = 32.0;
    values[1].grass_fade_end_m = 64.0;
    values[1].far_terrain_radius_m = 2250.0;
    values[1].far_outer_clip_m = 2300.0;
    values[1].shadow_resolution = 1536;
    values[1].volumetric_quality = 0;
    values[1].ui_blur = UiBlurQuality::Half;
    values[2].grass_density = 1.2;
    values[2].grass_fade_start_m = 40.0;
    values[2].grass_fade_end_m = 80.0;
    return values;
}();

// Compare every resolved field without serialization or allocation.
auto GraphicsValueMembers(const GraphicsValues& values) {
    return std::tie(values.render_scale,
                    values.grass_density,
                    values.grass_fade_start_m,
                    values.grass_fade_end_m,
                    values.far_terrain_radius_m,
                    values.far_outer_clip_m,
                    values.far_f1_boundary_m,
                    values.far_inner_clip_m,
                    values.cave_ao_distance_m,
                    values.cave_ao_floor,
                    values.cave_ao_thickness,
                    values.shadow_splits_m,
                    values.shadow_resolution,
                    values.volumetric_quality,
                    values.grass_candidates_per_column,
                    values.grass_instance_cap,
                    values.shadow_cascades,
                    values.ao_quality,
                    values.cloud_quality,
                    values.cave_ao_steps,
                    values.god_ray_samples,
                    values.water_ssr_min_steps,
                    values.water_ssr_max_steps,
                    values.water_caustics_resolution,
                    values.ui_msaa_samples,
                    values.grass_enabled,
                    values.ui_blur,
                    values.tinted_shadows,
                    values.analytic_aerial,
                    values.cave_ao,
                    values.god_rays,
                    values.taau,
                    values.gpu_sky_lut,
                    values.exposure_meter,
                    values.live_menu_preview);
}

bool ApplyGraphicsOverrides(const GraphicsOverrides& overrides,
                            GraphicsValues& values,
                            std::string& error) {
    for (const auto& [key, value] : overrides) {
        if (key == "grass_enabled") {
            if (const auto* enabled = std::get_if<bool>(&value)) {
                values.grass_enabled = *enabled;
                continue;
            }
        } else if (key == "ui_blur") {
            if (const auto* quality = std::get_if<std::string>(&value)) {
                if (*quality == "off" || *quality == "half" || *quality == "full") {
                    values.ui_blur = *quality == "off"    ? UiBlurQuality::Off
                                     : *quality == "half" ? UiBlurQuality::Half
                                                          : UiBlurQuality::Full;
                    continue;
                }
            }
        } else if (const auto* number = std::get_if<double>(&value);
                   number && std::isfinite(*number)) {
            // Explicit supported ranges, validated before any runtime conversion.
            if (key == "render_scale" && *number >= 0.5 && *number <= 1.0) {
                values.render_scale = *number;
                continue;
            }
            if (key == "grass_density" && *number >= 0.1 && *number <= 2.0) {
                values.grass_density = *number;
                continue;
            }
            if (key == "grass_fade_start_m" && *number >= 0.0 && *number <= 160.0) {
                values.grass_fade_start_m = *number;
                continue;
            }
            if (key == "grass_fade_end_m" && *number >= 1.0 && *number <= 160.0) {
                values.grass_fade_end_m = *number;
                continue;
            }
            if (key == "far_terrain_radius_m" && *number >= 768.0 && *number <= 3000.0) {
                values.far_terrain_radius_m = *number;
                values.far_outer_clip_m = *number + 50.0;
                continue;
            }
            if (key == "shadow_resolution" &&
                (*number == 1024.0 || *number == 1536.0 || *number == 2048.0)) {
                values.shadow_resolution = static_cast<int>(*number);
                continue;
            }
            if (key == "volumetric_quality" && (*number == 0.0 || *number == 1.0)) {
                values.volumetric_quality = static_cast<int>(*number);
                continue;
            }
        }
        // Bound diagnostics for callers supplying an arbitrary in-memory key.
        error = "invalid graphics override: " + key.substr(0, 64);
        return false;
    }
    if (values.grass_fade_start_m >= values.grass_fade_end_m) {
        error = "grass fade start must be below fade end";
        return false;
    }
    return true;
}

} // namespace

bool operator==(const GraphicsValues& left, const GraphicsValues& right) {
    return GraphicsValueMembers(left) == GraphicsValueMembers(right);
}

bool operator==(const GraphicsDetectionMetadata& left, const GraphicsDetectionMetadata& right) {
    return std::tie(left.observed_hardware, left.rules_revision, left.recommended_preset) ==
           std::tie(right.observed_hardware, right.rules_revision, right.recommended_preset);
}

std::string_view GraphicsPresetName(GraphicsPreset preset) {
    switch (preset) {
        case GraphicsPreset::Low:
            return "low";
        case GraphicsPreset::Medium:
            return "medium";
        case GraphicsPreset::High:
            return "high";
        case GraphicsPreset::Ultra:
            return "ultra";
    }
    return {};
}

bool ParseGraphicsPreset(std::string_view name, GraphicsPreset& preset) {
    for (const auto candidate : {GraphicsPreset::Low,
                                 GraphicsPreset::Medium,
                                 GraphicsPreset::High,
                                 GraphicsPreset::Ultra}) {
        if (name == GraphicsPresetName(candidate)) {
            preset = candidate;
            return true;
        }
    }
    return false;
}

bool SelectGraphicsPreset(GraphicsSettings& settings, GraphicsPreset preset) {
    if (GraphicsPresetName(preset).empty())
        return false;
    settings.base_preset = preset;
    settings.preset_revision = kGraphicsPresetRevision;
    ResetGraphicsOverrides(settings);
    return true;
}

void ResetGraphicsOverrides(GraphicsSettings& settings) {
    settings.overrides.clear();
}

bool ResolveGraphics(const GraphicsSettings& settings,
                     const GraphicsOverrides& session_overrides,
                     ResolvedGraphics& result,
                     std::string& error) {
    if (settings.preset_revision != kGraphicsPresetRevision ||
        GraphicsPresetName(settings.base_preset).empty()) {
        error = "unsupported graphics preset or revision";
        return false;
    }
    const auto& base = kRevision1[static_cast<std::size_t>(settings.base_preset)];
    ResolvedGraphics resolved{.values = base,
                              .base_preset = settings.base_preset,
                              .preset_revision = settings.preset_revision,
                              .custom = false,
                              .session_overrides = {}};
    if (!ApplyGraphicsOverrides(settings.overrides, resolved.values, error))
        return false;
    resolved.custom = resolved.values != base;
    if (!ApplyGraphicsOverrides(session_overrides, resolved.values, error))
        return false;
    resolved.custom = resolved.custom || resolved.values != base;
    // Validation restricts this copy to the nine supported keys and scalar values.
    resolved.session_overrides = session_overrides;
    result = std::move(resolved);
    error.clear();
    return true;
}

} // namespace luminumbra::core
