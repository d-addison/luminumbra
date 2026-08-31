#pragma once

// the TIME-OF-DAY POLICY seam.
//
// RenderPipeline::update_time_of_day was a ~270-line monolith mixing PURE render-derived
// math (season phase, sun/moon geometry, day-factors, palette tint, lunar phase, auto
// exposure) with side-effecting ASSEMBLY (advancing the day clock, refreshing the sky-view
// LUT, sampling the atmosphere, advancing the cloud scroll, writing pipeline members). This
// header extracts the PURE facets as small, deterministic, unit-testable free functions --
// the SAME split already applied to the direct-sun magnitude (SunLightModel.h) and the
// manual-exposure mapping (ExposureModel.h). The pipeline keeps the side-effecting assembly
// and now CALLS these functions, so the gate that unit-tests them exercises the SAME code
// the shipping frame runs -- there is no second copy to drift.
//
// DETERMINISM. Every function here is render-only: its outputs feed lighting / sky / exposure
// uniforms and NEVER world_hash. The season + lunar facets use the sim's
// in-house libm-free DeterministicMath::Sin (a pure function of the integer tick, so the same
// tick always yields the same season) exactly as the inline code did; the sun/moon DIRECTION
// facets use std::sin/std::cos exactly as the inline code did. This per-site trig choice is
// preserved verbatim on extraction -- swapping either would silently move pixels.

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include "luminumbra_common/core/DeterministicMath.h"

namespace Luminumbra::Rendering {

// Axial-tilt amplitude (radians): the seasonal swing of the sun's peak elevation. ~23.5 deg
// Earth obliquity. (Was a function-local constexpr in update_time_of_day.)
inline constexpr float kSeasonalTiltAmplitude = 0.41015237f; // ~23.5 degrees

// SEASON as a PURE FUNCTION of the sim tick. Integer epoch math (modulo the long-period
// year), then a single DeterministicMath trig evaluation -- no wall-clock, no float
// accumulator. The phase wraps deterministically on ticksPerSeasonCycle, so the same tick
// always yields the same season, reproducible from the tick alone.  == spring equinox
// and is season-NEUTRAL (, declination 0): the DEFAULT state every non-season scenario
// sees (it never calls set_season_tick), so the neutral case reproduces the pre-season sun
// arc / palette EXACTLY -- the season is a delta layered on top, not a baseline shift.
struct SeasonState {
    float phase;          // [0,1): 0 spring-equinox (neutral), 0.25 summer, 0.5 autumn, 0.75 winter
    float wave;           // Sin(phase*2pi): +1 at summer solstice, -1 at winter, 0 at .5
    float sunDeclination; // radians = kSeasonalTiltAmplitude * wave (the per-season arc tilt)
};

inline SeasonState ComputeSeason(std::uint64_t seasonTick, std::uint64_t ticksPerSeasonCycle) {
    namespace DM = Luminumbra::DeterministicMath;
    const std::uint64_t tick_in_year = seasonTick % ticksPerSeasonCycle;
    // Integer ratio first (exact), then to float: keeps the mapping a pure function of the
    // integer tick rather than an accumulated remainder.
    const float phase = static_cast<float>(static_cast<double>(tick_in_year) /
                                           static_cast<double>(ticksPerSeasonCycle));
    // Seasonal sine  the year, ZEROED at  (neutral default). +1 at .25
    // (summer solstice, highest arc / longest day); -1 at .75 (winter, lowest arc).
    const float wave = DM::Sin(phase * DM::kTwoPi);
    // Axial-tilt declination -> a real per-season change in the sun's peak elevation, replacing
    // the old fixed -0.2f tilt with this season-varying term so summer reads a visibly higher
    // noon arc than winter.
    const float sunDeclination = kSeasonalTiltAmplitude * wave;
    return SeasonState{phase, wave, sunDeclination};
}

// SUN GEOMETRY + derived day-factors, a pure function of (timeOfDay, sunDeclination).
//
// TRIG NOTE (byte-fragile — preserved verbatim from the inline code): the sun DIRECTION uses
// UNQUALIFIED sin/cos (global::sin/::cos from <cmath>) exactly as update_time_of_day did --
// NOT std::sin -- whereas the moon direction (ComputeMoonGeometry) uses std::sin/std::cos. This
// per-site choice is intentional and load-bearing; unifying it would silently move the sun path.
// (This header has no using-directives, so unqualified sin/cos here resolves to the same global
// sin overload the pipeline TU used -> the extraction is bit-for-bit identical.)
struct SunGeometry {
    float angleRad;      // timeOfDay * 2pi (also drives the moon phase downstream)
    float tiltZ;         // DM::Sin(sunDeclination) - 0.2: the light-dir z (carries the season)
    glm::vec3 direction; // normalized sun-travel dir; ~(0,-1,tiltZ) overhead at noon (timeOfDay 0)
    float upFactor;      // dot(direction,(0,-1,0)): +1 overhead, 0 horizon, <0 below
    float elevationRad;  // asin(clamp(upFactor)): sun elevation, domain-clamped
    float sunIntensity;  // 0->1 DAY-FACTOR smoothstep(-0.1,0.15,upFactor):
                         // skybox/foliage/water/ambient blends
    float skyDomeDayFactor; // sky dome day/twilight/night blend smoothstep(-0.22,0.34,upFactor):
                            // wider so twilight stays warm
};

inline SunGeometry ComputeSunGeometry(float timeOfDay, float sunDeclination) {
    namespace DM = Luminumbra::DeterministicMath;
    SunGeometry g;
    g.angleRad = timeOfDay * 2.0f * glm::pi<float>();
    // The -0.2f baseline keeps the season-neutral (declination 0) arc EXACTLY as before seasons.
    g.tiltZ = DM::Sin(sunDeclination) - 0.2f;
    // Unqualified sin/cos (global::sin/::cos) — verbatim from the inline code; do NOT
    // std::-qualify.
    g.direction = glm::normalize(glm::vec3(sin(g.angleRad), -cos(g.angleRad), g.tiltZ));
    g.upFactor = glm::dot(g.direction, glm::vec3(0.0f, -1.0f, 0.0f));
    g.elevationRad = std::asin(glm::clamp(g.upFactor, -1.0f, 1.0f));
    // A 0->1 day-factor: saturates to 1 once the sun clears ~0.15 elevation.
    g.sunIntensity = glm::smoothstep(-0.1f, 0.15f, g.upFactor);
    // Wider band keyed off the RAW elevation: ~0.5-0.6 at the horizon (twilight stays lit + warm),
    // full day while the sun is up, collapsing to 0 only well below the horizon (dawn/dusk glow).
    g.skyDomeDayFactor = glm::smoothstep(-0.22f, 0.34f, g.upFactor);
    return g;
}

// SEASON PALETTE tint: a small luminance-preserving hue shift that gives the biome palette a
// per-season feel. WINTER (wave < 0) leans WARM, SUMMER (wave > 0) leans COOL, reinforcing the
// seasonal sun path. Luminance-normalized so it rotates HUE only -- the calibrated noon
// luminance the LodGround/RenderHealth baselines depend on does not move. Render-only.
inline glm::vec3 SeasonPaletteTint(float seasonWave) {
    constexpr float kSeasonTintStrength = 0.06f; // +/- per-channel hue swing
    glm::vec3 tint(
        1.0f - kSeasonTintStrength * seasonWave, 1.0f, 1.0f + kSeasonTintStrength * seasonWave);
    // Preserve luminance: renormalize the tint so it only rotates hue. (Luma weights kept as a
    // verbatim manual sum -- do NOT refactor to a dot, which would reassociate the adds.)
    const float tint_lum = tint.r * 0.2126f + tint.g * 0.7152f + tint.b * 0.0722f;
    if (tint_lum > 1e-6f) {
        tint /= tint_lum;
    }
    return tint;
}

// MOON GEOMETRY: the moon orbits OPPOSITE the sun. lightDir is the moon's TOWARD-LIGHT vector in
// the same convention the lighting pass uses for the sun; it is overhead (0,-1,tiltZ) at midnight
// (sun angle PI) -- exactly when the sun is below the horizon -- so it can re-key the shadow
// cascade onto the moon at night. direction (= -sunDirection) is the moon TRAVEL dir for the disc.
//
// TRIG NOTE: the moon uses std::sin/std::cos (the FLOAT overload), distinct from the sun's
// unqualified::sin (see ComputeSunGeometry) -- preserved verbatim from the inline code.
struct MoonGeometry {
    glm::vec3 direction; // -sunDirection (moon travel dir, for placing the disc)
    glm::vec3 lightDir;  // toward-moon-light dir; overhead at midnight
    float upFactor;      // dot(lightDir,(0,-1,0)): >0 when the moon is up
};

inline MoonGeometry
ComputeMoonGeometry(float sunAngleRad, float sunTiltZ, const glm::vec3& sunDirection) {
    MoonGeometry m;
    m.direction = -sunDirection;
    m.lightDir = glm::normalize(glm::vec3(-std::sin(sunAngleRad), std::cos(sunAngleRad), sunTiltZ));
    m.upFactor = glm::dot(m.lightDir, glm::vec3(0.0f, -1.0f, 0.0f));
    return m;
}

// Starlight floor: a new moon is dark but never pitch-zero (a deep new-moon night wants a torch).
inline constexpr float kNewMoonFloor = 0.08f;

// LUNAR PHASE illumination as a pure function of the tick -> the "two night modes" (bright moonlit
// vs dark new-moon nights). Deterministic lunar cycle: 1 (full) -> kNewMoonFloor (new) -> 1.
inline float LunarIllumination(std::uint64_t seasonTick, std::uint64_t ticksPerLunarCycle) {
    const std::uint64_t tick_in_lunar = seasonTick % ticksPerLunarCycle;
    const float lunar_t = static_cast<float>(static_cast<double>(tick_in_lunar) /
                                             static_cast<double>(ticksPerLunarCycle));
    const float full =
        0.5f + 0.5f * std::cos(lunar_t * 2.0f * glm::pi<float>()); // 1 full -> 0 new -> 1
    return glm::mix(kNewMoonFloor, 1.0f, full);
}

// Resolve the moon illumination: a forced override (>= 0, from LUMIN_MOON or set_moon_illumination)
// wins, clamped to [0,1]; otherwise the deterministic lunar cycle. The env read stays in the
// pipeline (a client-config concern); this is the pure selection + math.
inline float ComputeMoonIllumination(std::uint64_t seasonTick,
                                     float forcedOverride,
                                     std::uint64_t ticksPerLunarCycle) {
    if (forcedOverride >= 0.0f) {
        return glm::clamp(forcedOverride, 0.0f, 1.0f);
    }
    return LunarIllumination(seasonTick, ticksPerLunarCycle);
}

// TIME AUTHORITY — time-of-day as a PURE FUNCTION of the
// authoritative sim tick, retiring the wall-clock day-advance for live play. Integer
// modulo first (exact), then one divide — the same pure-function-of-the-tick shape as
// ComputeSeason. The default day length reproduces the legacy pacing EXACTLY:
// m_dayDurationSeconds (60 s) at the canonical 30 Hz tick rate = 1800 ticks/day, so
// the switch from wall-clock to tick authority is visually pacing-identical.
// Render-only: the tick flows sim -> render one way; TOD never feeds world_hash
//.
inline constexpr std::uint64_t kDefaultDayLengthTicks = 1800; // 60 s/day @ 30 Hz

inline float TimeOfDayFromTick(std::uint64_t simTick, std::uint64_t dayLengthTicks) {
    if (dayLengthTicks == 0) {
        dayLengthTicks = 1;
    }
    const std::uint64_t tick_in_day = simTick % dayLengthTicks;
    return static_cast<float>(static_cast<double>(tick_in_day) /
                              static_cast<double>(dayLengthTicks));
}

} // namespace Luminumbra::Rendering
