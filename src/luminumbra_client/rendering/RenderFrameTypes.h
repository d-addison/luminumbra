#pragma once

//  (, struct extraction): the per-frame RENDER LIGHT/WEATHER PODs
// moved out of RenderPipeline.h so passes (Skybox, Lighting, Foliage, Particle,
// Aerial,...) and the RenderContext seam can reference them by value/pointer
// without pulling the RenderPipeline god-object. Definitions are VERBATIM — the
// four isolated per-pass recipes (RenderFrameTypes/RenderFrameState/
// RenderLightTypes/RenderLightState) overlapped on these types (ODR conflict) and
// are consolidated into this ONE shared header. Layout is unchanged.

#include "../../include/luminumbra/core/Types.h"

#include <glm/glm.hpp>
#include <vector>

namespace Luminumbra::Rendering {

struct DirectionalLight {
    glm::vec3 direction = glm::normalize(glm::vec3(0.5f, -1.0f, -0.5f));
    glm::vec3 color = glm::vec3(1.0f, 0.95f, 0.85f);
    float intensity = 1.0f;
};

struct PointLight {
    glm::vec3 position;
    float radius; // Using std140 layout padding for future UBO compatibility
    glm::vec3 color;
    float intensity;
};

// Engine-generic runtime weather state. Default Off: the weather
// overlay issues zero GL work unless a weather type with intensity > 0 is set.
enum class WeatherType {
    None = 0,
    Rain,
    Snow,
    Fog,
    Storm,
};

// SIM-DRIVEN weather render state. This is the one-way (critique
// ) bridge from the replicated WeatherSystem state to the render overlay +
// wetness response: the client samples WeatherSystem at the camera each frame and
// pushes this POD via RenderPipeline::set_weather_state. The render side READS it
// and writes NOTHING back into the sim. All fields are derived weather quantities;
// the overlay's u_rainIntensity/u_snowIntensity/u_fogDensity/u_stormIntensity/
// u_windDirection/u_windStrength uniforms are fed from here instead of the legacy
// set_weather debug mapping. driven=false falls back to the legacy debug path so
// existing set_weather callers (and the None default) are unchanged.
struct WeatherRenderState {
    bool driven = false;          // true once a sim weather state has been pushed
    float rain_intensity = 0.0f;  // [0, 1]
    float snow_intensity = 0.0f;  // [0, 1]
    float fog_density = 0.0f;     // [0, 1]
    float storm_intensity = 0.0f; // [0, 1]
    float wetness = 0.0f;         // [0, 1] local precipitation -> material wetness
    glm::vec3 wind_direction = glm::vec3(1.0f, 0.0f, 0.0f); // normalized XZ wind
    float wind_strength = 0.0f;                             // [0, 1] wind magnitude (scaled)
};

// cloud layer state. The cloud coverage field + its
// projected cast shadow are a pure function of (replicated weather state + sim
// tick + wind) — one-way, never read back into the sim or world_hash (critique
// ). The client pushes this each frame via set_cloud_state; the SkyboxPass
// renders the wind-advected sky-dome cloud layer and the LightingPass projects
// the SAME coverage field to cast crawling terrain shadows. The scroll offset is
// the wind direction * a tick-derived phase, so the clouds drift deterministically
// with the large-scale wind and the dome/shadow stay registered.
struct CloudRenderState {
    bool enabled = false;                      // master toggle (false == zero added cost)
    bool shadow_enabled = false;               // project the coverage into the lighting pass
    glm::vec2 scroll_offset = glm::vec2(0.0f); // wind * tick-phase, world metres
    float coverage_amount = 0.45f;             // [0,1] weather sky-cover fraction
    float biome_variation = 0.0f;              // biome coverage bias (e.g. wetter == cloudier)
    float plane_height = 900.0f;               // world Y of the cloud sheet
    float shadow_strength = 0.0f;              // [0,1] max sun darkening under a cloud core
    glm::vec3 sun_travel_dir = glm::vec3(0.0f, -1.0f, 0.0f); // for shadow projection
};

// lightning state for a single captured frame. A strike
// is a deterministic SIM world event (Systems::StrikeEvent, in the `weather`
// world_hash sub-hash); this is the ONE-WAY (regression review) render response the
// client pushes for the frame(s) the bolt is visible: a full-scene LIGHT PULSE
// injected through the lighting pass + a screen-space BOLT polyline rasterized in
// the same pass (no new GL objects). Nothing here is hashed or written back to sim.
//
//  - pulse_intensity  scales a full-scene additive luminance spike (the 1-to-few-
//    frame flash); 0 == the zero-cost OFF path (no added lighting work).
//  - pulse_color      the flash tint (cool white-blue by default).
//  - strike_ndc       the strike ground point projected to NDC [-1,1] (for a mild
//    radial brightening centred on the strike).
//  - bolt_points_ndc  the bolt polyline (main channel + branches, flattened with
//    NaN-x separators) in NDC; the lighting frag adds bright pixels near any
//    segment so the capture shows a thin high-gradient structure.
inline constexpr int kMaxBoltSegmentPoints = 96; // GLSL uniform array cap
struct LightningRenderState {
    bool active = false;          // master toggle (false == zero added cost)
    float pulse_intensity = 0.0f; // [0,~3] full-scene additive flash strength
    glm::vec3 pulse_color = glm::vec3(0.72f, 0.82f, 1.0f); // cool flash tint
    glm::vec2 strike_ndc = glm::vec2(0.0f);                // strike point in NDC (radial centre)
    float bolt_width_ndc = 0.004f;                         // bolt core half-width in NDC units
    float bolt_glow_ndc = 0.018f;                          // bolt glow falloff radius in NDC units
    //  GROUND-IMPACT bloom at the bolt touchdown point so
    // the strike visibly CONNECTS to terrain. ground_ndc is the projected terminus;
    // ground_flash scales the radial impact glow (0 = off, keeps gates byte-stable).
    glm::vec2 ground_ndc = glm::vec2(0.0f, -1.0f);
    float ground_flash = 0.0f;
    //  DARK STORM CLOUD anchor. The owner saw the bolt
    // "appear from thin air". cloud_anchor_ndc is the screen point at the TOP of the
    // bolt (where it should emerge from the cloud base); cloud_darkness scales a
    // dark, billowing cloud mass the overlay paints around that anchor so the bolt
    // visibly STEMS FROM a cloud and the flash lights that cloud from within.
    // 0 == no cloud overlay (keeps the WeatherVisual strike gate byte-stable).
    glm::vec2 cloud_anchor_ndc = glm::vec2(0.0f, 0.85f);
    float cloud_darkness = 0.0f;
    // Flattened NDC polyline points. A point with x <= -2.0 is a PEN-UP separator
    // between disjoint polylines (main channel / each branch). Drawn as connected
    // segments between consecutive non-separator points.
    std::vector<glm::vec2> bolt_points_ndc;
};

} // namespace Luminumbra::Rendering
