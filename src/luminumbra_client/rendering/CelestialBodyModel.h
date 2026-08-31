#pragma once

#include <cstdint>

#include <glm/glm.hpp>

#include "TimeOfDayModel.h"

//  Tier 1: the celestial-body SEAM. Sun and moon become two
// evaluated instances of one primitive shape instead of ad-hoc pipeline math —
// the composability groundwork for data-driven bodies (a second moon, a bright
// planet) WITHOUT changing today's lighting: Tier 1 is plumbing over the
//  TimeOfDayModel primitives, called VERBATIM (the sun's unqualified
// sin vs the moon's std::sin asymmetry is preserved inside those primitives —
// this header adds NO trig of its own). Bit-identical by construction; pinned by
// CelestialBodyModel.SunMoonSeamBitExactAgainstPrimitives.
//
// Render-only: celestial render state never feeds world_hash.
// A SIM-authoritative celestial quantity must route through the deterministic
// tick path.
namespace Luminumbra::Rendering {

struct CelestialBodyParams {
    enum class RadianceModel {
        TransmittanceCoupledSun, // magnitude follows the atmosphere transmittance ( )
        AuthoredNightFill,       // authored night key (the moon: no in-scatter model at night)
    };
    enum class ShadowRole {
        PrimaryCascade, // keys the CSM (the sun by day; the moon re-keys it at night)
        None,
    };
    RadianceModel radiance = RadianceModel::TransmittanceCoupledSun;
    ShadowRole shadow = ShadowRole::PrimaryCascade;
    bool has_lunar_phase = false;
};

struct CelestialBodyState {
    glm::vec3 travel_direction{0.0f}; // the disc's travel dir (sun: SunGeometry.direction)
    glm::vec3 light_direction{0.0f};  // TOWARD-light dir the lighting/shadow passes consume
    float up_factor = 0.0f;
    float elevation_rad = 0.0f;
    float day_factor = 0.0f;          // sun: SunGeometry.sunIntensity (0..1)
    float sky_dome_day_factor = 0.0f; // sun: the wider twilight envelope
    float illumination = 1.0f;        // lunar phase (bodies with has_lunar_phase)
};

// The evaluated frame: today exactly two configured instances. Tier 2 grows this
// into a list the pipeline iterates.
struct CelestialFrame {
    CelestialBodyState sun;
    CelestialBodyState moon;
    // The raw sun geometry, exposed so downstream consumers that keyed off its
    // intermediate fields (angleRad drives the moon phase geometry; tiltZ the
    // moon light z) keep reading the exact same values.
    SunGeometry sun_geometry;
};

inline CelestialFrame EvaluateCelestialBodies(float timeOfDay,
                                              float sunDeclination,
                                              std::uint64_t seasonTick,
                                              float moonForcedOverride,
                                              std::uint64_t ticksPerLunarCycle) {
    CelestialFrame frame;
    // VERBATIM primitive calls — the seam adds no math (-2).
    frame.sun_geometry = ComputeSunGeometry(timeOfDay, sunDeclination);
    frame.sun.travel_direction = frame.sun_geometry.direction;
    frame.sun.light_direction = frame.sun_geometry.direction; // the pass consumes it as L
    frame.sun.up_factor = frame.sun_geometry.upFactor;
    frame.sun.elevation_rad = frame.sun_geometry.elevationRad;
    frame.sun.day_factor = frame.sun_geometry.sunIntensity;
    frame.sun.sky_dome_day_factor = frame.sun_geometry.skyDomeDayFactor;
    frame.sun.illumination = 1.0f;

    const MoonGeometry moon = ComputeMoonGeometry(
        frame.sun_geometry.angleRad, frame.sun_geometry.tiltZ, frame.sun_geometry.direction);
    frame.moon.travel_direction = moon.direction;
    frame.moon.light_direction = moon.lightDir;
    frame.moon.up_factor = moon.upFactor;
    frame.moon.elevation_rad = 0.0f; // the moon path never derived one
    frame.moon.day_factor = 0.0f;    // the moon is the night key
    frame.moon.sky_dome_day_factor = 0.0f;
    frame.moon.illumination =
        ComputeMoonIllumination(seasonTick, moonForcedOverride, ticksPerLunarCycle);
    return frame;
}

} // namespace Luminumbra::Rendering
