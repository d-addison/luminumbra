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
