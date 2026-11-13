#pragma once

// lightning BOLT geometry (, one-way per regression review).
//
// A strike is a deterministic SIM world event (Systems::StrikeEvent, folded into
// the `weather` world_hash sub-hash). The bolt POLYLINE drawn for it is a pure
// render artifact: a midpoint-displacement fractal line from a cloud-base height
// down to the strike ground point, SEEDED from the strike event so the same strike
// always draws the same bolt (reproducible capture for the gate). This file builds
// the world-space polyline; nothing here is hashed or read back into the sim.
//
// The bolt is rasterized inside the lighting pass as a screen-space distance-to-
// polyline test (no new GL objects): the caller projects these world points to NDC
// and pushes them as uniforms. See LightingPass / lighting_pass.frag.

#include "LiveStrikeComposer.h"

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

// A generated bolt: an ordered main-channel polyline (cloud base -> ground) plus a
// few short branch polylines forking off the main channel. World-space metres.
struct LightningBoltGeometry {
    std::vector<glm::vec3> main_channel;          // >= 2 points, top -> ground
    std::vector<std::vector<glm::vec3>> branches; // optional forks (may be empty)
    glm::vec3 ground_point = glm::vec3(0.0f);     // strike terminus (world)
    float magnitude = 0.0f;                       // [0,1] strike strength (copied)
};

// Project the generated world geometry without re-anchoring or reshaping it in
// screen space. Perspective clip.w is positive view depth, independent of the
// chosen near/far planes and depth convention. Default generated bolts have 33 channel points and
// at most three four-point branches. ComposeLiveStrikes applies the fixed upload budget next.
inline ProjectedLiveStrike ProjectLightningBolt(const LightningBoltGeometry& bolt,
                                                const glm::mat4& view_projection,
                                                float pulse) {
    const auto project = [&](const glm::vec3& point) -> ProjectedBoltPoint {
        const glm::vec4 clip = view_projection * glm::vec4(point, 1.0f);
        if (!std::isfinite(clip.w) || !(clip.w > 0.0f)) {
            return kLiveBoltPenUp;
        }
        const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y)) {
            return kLiveBoltPenUp;
        }
        const float inverse_depth = 1.0f / clip.w;
        if (!std::isfinite(inverse_depth)) {
            return kLiveBoltPenUp;
        }
        return {ndc, inverse_depth};
    };
    const auto project_stroke = [&](const std::vector<glm::vec3>& stroke) {
        std::vector<ProjectedBoltPoint> points;
        points.reserve(stroke.size());
        for (const auto& point : stroke) {
            points.push_back(project(point));
        }
        return points;
    };
    ProjectedLiveStrike projected;
    projected.main_channel = project_stroke(bolt.main_channel);
    projected.branches.reserve(bolt.branches.size());
    for (const auto& branch : bolt.branches) {
        projected.branches.push_back(project_stroke(branch));
    }
    projected.strike_ndc = project(bolt.ground_point);
    projected.pulse = pulse;
    return projected;
}

// Parameters controlling the fractal subdivision. Defaults give a readable,
// believable bolt at the gate's framing; all deterministic.
struct LightningBoltParams {
    float cloud_base_height = 220.0f; // world Y the bolt descends from
    int subdivisions = 5;             // midpoint-displacement passes (2^n segments)
    float jaggedness = 26.0f;         // initial lateral displacement (world metres)
    float jaggedness_falloff = 0.55f; // displacement scale per subdivision level
    int max_branches = 3;             // upper bound on forks
    float branch_chance = 0.5f;       // per eligible vertex
};

// Build the deterministic bolt for a strike. ground_x/ground_z is the strike
// terminus (world XZ); ground_y is the terrain/sea height there. strike_seed is a
// deterministic salt from the StrikeEvent (e.g. storm_salt mixed with the tick) so
// the polyline is reproducible. NO wall-clock, NO std::random.
LightningBoltGeometry BuildLightningBolt(float ground_x,
                                         float ground_y,
                                         float ground_z,
                                         float magnitude,
                                         std::uint64_t strike_seed,
                                         const LightningBoltParams& params = {});

} // namespace Luminumbra::Rendering
