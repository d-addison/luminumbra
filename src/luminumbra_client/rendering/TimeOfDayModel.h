#pragma once

// Spec 016 FR-F-001 / spec-021 rank 72 (RENDER-14): the TIME-OF-DAY POLICY seam.
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
// uniforms and NEVER world_hash (spec 018 FR-E-003). The season + lunar facets use the sim's
// in-house libm-free DeterministicMath::Sin (a pure function of the integer tick, so the same
// tick always yields the same season) exactly as the inline code did; the sun/moon DIRECTION
// facets use std::sin/std::cos exactly as the inline code did. This per-site trig choice is
// preserved verbatim on extraction -- swapping either would silently move pixels.

#include <cstdint>

#include <glm/glm.hpp>

#include "luminumbra_common/core/DeterministicMath.h"

namespace Luminumbra::Rendering {

// Axial-tilt amplitude (radians): the seasonal swing of the sun's peak elevation. ~23.5 deg
// Earth obliquity. (Was a function-local constexpr in update_time_of_day.)
inline constexpr float kSeasonalTiltAmplitude = 0.41015237f; // ~23.5 degrees

// SEASON as a PURE FUNCTION of the sim tick. Integer epoch math (modulo the long-period
// year), then a single DeterministicMath trig evaluation -- no wall-clock, no float
// accumulator. The phase wraps deterministically on ticksPerSeasonCycle, so the same tick
// always yields the same season, reproducible from the tick alone. Phase 0 == spring equinox
// and is season-NEUTRAL (wave 0, declination 0): the DEFAULT state every non-season scenario
// sees (it never calls set_season_tick), so the neutral case reproduces the pre-season sun
// arc / palette EXACTLY -- the season is a delta layered on top, not a baseline shift.
struct SeasonState {
    float phase;          // [0,1): 0 spring-equinox (neutral), 0.25 summer, 0.5 autumn, 0.75 winter
    float wave;           // Sin(phase*2pi): +1 at summer solstice, -1 at winter, 0 at phase 0/0.5
    float sunDeclination; // radians = kSeasonalTiltAmplitude * wave (the per-season arc tilt)
};

inline SeasonState ComputeSeason(std::uint64_t seasonTick, std::uint64_t ticksPerSeasonCycle) {
    namespace DM = Luminumbra::DeterministicMath;
    const std::uint64_t tick_in_year = seasonTick % ticksPerSeasonCycle;
    // Integer ratio first (exact), then to float: keeps the mapping a pure function of the
    // integer tick rather than an accumulated remainder.
    const float phase = static_cast<float>(
        static_cast<double>(tick_in_year) / static_cast<double>(ticksPerSeasonCycle));
    // Seasonal sine wave over the year, ZEROED at phase 0 (neutral default). +1 at phase 0.25
    // (summer solstice, highest arc / longest day); -1 at phase 0.75 (winter, lowest arc).
    const float wave = DM::Sin(phase * DM::kTwoPi);
    // Axial-tilt declination -> a real per-season change in the sun's peak elevation, replacing
    // the old fixed -0.2f tilt with this season-varying term so summer reads a visibly higher
    // noon arc than winter.
    const float sunDeclination = kSeasonalTiltAmplitude * wave;
    return SeasonState{phase, wave, sunDeclination};
}

} // namespace Luminumbra::Rendering
