#pragma once

// I9-ECO: herd flocking (boids) for the creature brain. A PURE, libm-free, order-independent
// steering helper: given a creature and its same-role neighbours, it returns a steer vector
// blending COHESION (toward the local group centroid) and SEPARATION (away from neighbours
// that are too close, stronger the closer they are). The CreatureBrainSystem blends this into
// the action heading so prey flee as a coherent herd (and predators can pack) instead of each
// moving in isolation. Deterministic: pure function of the inputs, DeterministicMath only,
// commutative accumulation -> independent of neighbour order (run==replay).

#include "../core/DeterministicMath.h"

#include <utility>
#include <vector>

namespace luminumbra::ai {

namespace dm = ::Luminumbra::DeterministicMath;

struct FlockParams {
    float neighbor_radius = 12.0f;    // cohesion considers same-role neighbours within this
    float separation_radius = 3.0f;   // separation kicks in below this spacing
    float cohesion_weight = 0.6f;     // pull toward the group centroid
    float separation_weight = 1.4f;   // push off crowding (dominates up close)
};

// A steering vector in the XZ plane (NOT normalized; the caller blends + renormalizes).
struct FlockSteer {
    float x = 0.0f;
    float z = 0.0f;
};

// neighbours: same-role creature positions (x, z) EXCLUDING self.
inline FlockSteer ComputeFlockSteer(float sx, float sz,
                                    const std::vector<std::pair<float, float>>& neighbors,
                                    const FlockParams& p = {}) {
    FlockSteer steer;
    if (neighbors.empty()) return steer;

    // Cohesion: centroid of neighbours within the cohesion radius.
    float cx = 0.0f, cz = 0.0f;
    int cohesion_count = 0;
    float sepx = 0.0f, sepz = 0.0f;
    for (const auto& [nx, nz] : neighbors) {
        const float dx = nx - sx, dz = nz - sz;
        const float dist = dm::Sqrt(dx * dx + dz * dz);
        if (dist <= p.neighbor_radius) {
            cx += nx;
            cz += nz;
            ++cohesion_count;
        }
        if (dist > 1.0e-5f && dist < p.separation_radius) {
            // Away from this neighbour, weighted by how close it is (1 at touching -> 0 at radius).
            const float inv = 1.0f / dist;
            const float crowd = 1.0f - dist / p.separation_radius;
            sepx += (sx - nx) * inv * crowd;
            sepz += (sz - nz) * inv * crowd;
        }
    }

    if (cohesion_count > 0) {
        const float invn = 1.0f / static_cast<float>(cohesion_count);
        const float toward_x = cx * invn - sx;
        const float toward_z = cz * invn - sz;
        const float len = dm::Sqrt(toward_x * toward_x + toward_z * toward_z);
        if (len > 1.0e-5f) {
            const float k = p.cohesion_weight / len;
            steer.x += toward_x * k;
            steer.z += toward_z * k;
        }
    }
    steer.x += sepx * p.separation_weight;
    steer.z += sepz * p.separation_weight;
    return steer;
}

}  // namespace luminumbra::ai
