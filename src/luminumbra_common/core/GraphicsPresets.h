#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace luminumbra::core {

// Revisions are immutable: retain old tables when introducing a new revision.
inline constexpr std::uint32_t kGraphicsPresetRevision = 1;
enum class GraphicsPreset : std::uint8_t {
    Low,
    Medium,
    High,
    Ultra
};
enum class UiBlurQuality : std::uint8_t {
    Off,
    Half,
    Full
};

[[nodiscard]] std::string_view GraphicsPresetName(GraphicsPreset preset);
[[nodiscard]] bool ParseGraphicsPreset(std::string_view name, GraphicsPreset& preset);

// Only presentation controls belong here. Existing request/refusal policies,
// authored content and other UI filters remain with their current owners. In particular there are
// no simulation, live residency, water resolution, content, transport or display-choice controls.
struct GraphicsValues {
    double render_scale = 1.0;
    double grass_density = 1.6;
    double grass_fade_start_m = 48.0;
    double grass_fade_end_m = 92.0;
    double far_terrain_radius_m = 3000.0;
    // Derived with wanted radius; never independently overridden.
    double far_outer_clip_m = 3050.0;
    // The remaining presentation rows preserve packaged behavior.
    double far_f1_boundary_m = 768.0;
    double far_inner_clip_m = 176.0;
    double cave_ao_distance_m = 24.0;
    double cave_ao_floor = 0.06;
    double cave_ao_thickness = 1.5;
    std::array<double, 5> shadow_splits_m = {0.1, 15.0, 40.0, 100.0, 250.0};
    int shadow_resolution = 2048;
    int volumetric_quality = 1;
    int grass_candidates_per_column = 8192;
    int grass_instance_cap = 262144;
    int shadow_cascades = 4;
    // Half-resolution GTAO; zero means legacy SSAO, not off.
    int ao_quality = 3;
    // Quarter-resolution per axis.
    int cloud_quality = 2;
    int cave_ao_steps = 8;
    int god_ray_samples = 24;
    int water_ssr_min_steps = 4;
    int water_ssr_max_steps = 8;
    int water_caustics_resolution = 256;
    int ui_msaa_samples = 2;
    bool grass_enabled = true;
    UiBlurQuality ui_blur = UiBlurQuality::Full;
    bool tinted_shadows = true;
    bool analytic_aerial = true;
    bool cave_ao = true;
    bool god_rays = true;
    bool taau = true;
    bool gpu_sky_lut = true;
    bool exposure_meter = false;
    bool live_menu_preview = true;
};

[[nodiscard]] bool operator==(const GraphicsValues& left, const GraphicsValues& right);

// Numeric values use double so validation precedes any narrowing conversion.
// Accepted keys/ranges are documented by ApplyGraphicsOverrides in the source.
using GraphicsOverrideValue = std::variant<bool, double, std::string>;
using GraphicsOverrides = std::map<std::string, GraphicsOverrideValue>;

struct GraphicsDetectionMetadata {
    std::string observed_hardware;
    std::uint32_t rules_revision = 0; // Zero means no detection has run.
    GraphicsPreset recommended_preset = GraphicsPreset::Low;
};

[[nodiscard]] bool operator==(const GraphicsDetectionMetadata& left,
                              const GraphicsDetectionMetadata& right);

struct GraphicsSettings {
    GraphicsOverrides overrides;
    GraphicsDetectionMetadata detection;
    std::uint32_t preset_revision = kGraphicsPresetRevision;
    GraphicsPreset base_preset = GraphicsPreset::Ultra;
};

// Invalid enum values are rejected without modifying the saved selection.
[[nodiscard]] bool SelectGraphicsPreset(GraphicsSettings& settings, GraphicsPreset preset);
void ResetGraphicsOverrides(GraphicsSettings& settings);

struct ResolvedGraphics {
    GraphicsValues values;
    GraphicsPreset base_preset = GraphicsPreset::Ultra;
    std::uint32_t preset_revision = kGraphicsPresetRevision;
    bool custom = false;
    // The caller can display session-only effective values; never persisted.
    GraphicsOverrides session_overrides;
};

// Pure, transactional resolution: preset/revision -> saved overrides -> session
// CLI/environment overrides (already ordered by the caller's environment precedence).
// On error, result is unchanged. No environment reads, I/O or renderer calls.
// Resolve when settings change and cache values; this is not a per-frame operation.
[[nodiscard]] bool ResolveGraphics(const GraphicsSettings& settings,
                                   const GraphicsOverrides& session_overrides,
                                   ResolvedGraphics& result,
                                   std::string& error);

} // namespace luminumbra::core
