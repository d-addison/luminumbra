#pragma once

#include "luminumbra_common/core/GraphicsPresets.h"

namespace Luminumbra::Rendering {

// Renderer-facing projection. These are presentation values only; no session,
// world, streaming authority, collision or simulation configuration is accepted.
struct GraphicsRuntimeConfig {
    float render_scale = 1.0f;
    float grass_density = 1.6f;
    float grass_fade_start_m = 48.0f;
    float grass_fade_end_m = 92.0f;
    float far_radius_m = 3000.0f;
    float far_outer_clip_m = 3050.0f;
    int shadow_resolution = 2048;
    int volumetric_quality = 1;
    bool grass_enabled = true;
    luminumbra::core::UiBlurQuality ui_blur = luminumbra::core::UiBlurQuality::Full;
};

inline bool operator==(const GraphicsRuntimeConfig& lhs, const GraphicsRuntimeConfig& rhs) {
    return lhs.render_scale == rhs.render_scale && lhs.grass_density == rhs.grass_density &&
           lhs.grass_fade_start_m == rhs.grass_fade_start_m &&
           lhs.grass_fade_end_m == rhs.grass_fade_end_m && lhs.far_radius_m == rhs.far_radius_m &&
           lhs.far_outer_clip_m == rhs.far_outer_clip_m &&
           lhs.shadow_resolution == rhs.shadow_resolution &&
           lhs.volumetric_quality == rhs.volumetric_quality &&
           lhs.grass_enabled == rhs.grass_enabled && lhs.ui_blur == rhs.ui_blur;
}

inline GraphicsRuntimeConfig
GraphicsRuntimeValues(const luminumbra::core::ResolvedGraphics& resolved) {
    const auto& v = resolved.values;
    return {.render_scale = static_cast<float>(v.render_scale),
            .grass_density = static_cast<float>(v.grass_density),
            .grass_fade_start_m = static_cast<float>(v.grass_fade_start_m),
            .grass_fade_end_m = static_cast<float>(v.grass_fade_end_m),
            .far_radius_m = static_cast<float>(v.far_terrain_radius_m),
            .far_outer_clip_m = static_cast<float>(v.far_outer_clip_m),
            .shadow_resolution = v.shadow_resolution,
            .volumetric_quality = v.volumetric_quality,
            .grass_enabled = v.grass_enabled,
            .ui_blur = v.ui_blur};
}

} // namespace Luminumbra::Rendering
