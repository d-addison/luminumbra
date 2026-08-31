#pragma once

//  herd flocking (boids) for the creature brain. A PURE, libm-free, order-independent
// steering helper: given a creature and its same-role neighbours, it returns a steer vector
// blending COHESION (toward the local group centroid) and SEPARATION (away from neighbours
// that are too close, stronger the closer they are). The CreatureBrainSystem blends this into
// the action heading so prey flee as a coherent herd (and predators can pack) instead of each
// moving in isolation. Deterministic: pure function of the inputs, DeterministicMath only,
// commutative accumulation -> independent of neighbour order (run==replay).

#include "../core/DeterministicMath.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace luminumbra::ai {

namespace dm = ::Luminumbra::DeterministicMath;

struct FlockParams {
    float neighbor_radius = 12.0f; // cohesion + alignment consider same-role neighbours within this
    float separation_radius = 3.0f; // separation kicks in below this spacing
    float cohesion_weight = 0.6f;   // pull toward the group centroid
    float separation_weight = 1.4f; // push off crowding (dominates up close)
    float alignment_weight = 0.0f;  // match the group's mean heading (3rd Reynolds term; 0 = off)
};

// A steering vector in the XZ plane (NOT normalized; the caller blends + renormalizes).
struct FlockSteer {
    float x = 0.0f;
    float z = 0.0f;
};

// ORDER-INDEPENDENCE (the header's load-bearing determinism claim). IEEE-754 float addition
// is NOT associative, so accumulating the cohesion/separation sums in the caller's neighbour
// order would make the last ULPs depend on that order — breaking run==replay if the gather
// order ever varies (and the spatial-grid query in CreatureBrainSystem deliberately does NOT
// gather in a fixed order). Rather than sort the neighbours into a canonical order every call
// (an O(n log n) tax on the hot tick), we accumulate the four reduction sums in FIXED-POINT
// int64 at `kFlockFixedScale`. Integer addition IS associative + commutative, so the sums are
// a pure function of the neighbour SET regardless of visitation order — order-independent by
// construction, with no per-call sort + vector copy. Each per-neighbour term is computed in
// float exactly as before (so a single term is bit-identical); only the REDUCTION moves to
// fixed point. Scale 2^20 keeps centroid precision (~1e-6 at radius 12) and cannot overflow
// int64 even at crowded N (max separation term ~1e5 * 2^20 ~ 1e11, * thousands of neighbours
// stays well under 9.2e18).
inline constexpr double kFlockFixedScale = 1048576.0; // 2^20

inline std::int64_t ToFixed(float v) {
    return static_cast<std::int64_t>(static_cast<double>(v) * kFlockFixedScale +
                                     (v >= 0.0f ? 0.5 : -0.5));
}

inline float FromFixed(std::int64_t v) {
    return static_cast<float>(static_cast<double>(v) / kFlockFixedScale);
}

// neighbours: same-role creature positions (x, z) EXCLUDING self.
// neighbor_headings (optional): per-neighbour heading vectors (wx, wz), index-aligned with
// `neighbors`, used ONLY for the alignment term when `p.alignment_weight > 0`. Pass nullptr (or
// leave alignment_weight at 0) to skip alignment entirely — the steer is then byte-identical to the
// cohesion+separation-only result, so existing callers and goldens are unaffected. Like cohesion,
// the heading sum is reduced in int64 fixed point so it is order-independent w.r.t. the grid
// gather.
inline FlockSteer
ComputeFlockSteer(float sx,
                  float sz,
                  const std::vector<std::pair<float, float>>& neighbors,
                  const FlockParams& p = {},
                  const std::vector<std::pair<float, float>>* neighbor_headings = nullptr) {
    FlockSteer steer;
    if (neighbors.empty())
        return steer;

    const bool do_align = p.alignment_weight > 0.0f && neighbor_headings != nullptr &&
                          neighbor_headings->size() == neighbors.size();

    // Cohesion: centroid of neighbours within the cohesion radius (sums in fixed point).
    // Separation: push off neighbours below the separation radius (sums in fixed point).
    // Alignment: mean heading of neighbours within the cohesion radius (sums in fixed point).
    std::int64_t cx_fp = 0, cz_fp = 0;
    int cohesion_count = 0;
    std::int64_t sepx_fp = 0, sepz_fp = 0;
    std::int64_t ahx_fp = 0, ahz_fp = 0;
    for (std::size_t i = 0; i < neighbors.size(); ++i) {
        const float nx = neighbors[i].first, nz = neighbors[i].second;
        const float dx = nx - sx, dz = nz - sz;
        const float dist = dm::Sqrt(dx * dx + dz * dz);
        if (dist <= p.neighbor_radius) {
            cx_fp += ToFixed(nx);
            cz_fp += ToFixed(nz);
            ++cohesion_count;
            if (do_align) {
                ahx_fp += ToFixed((*neighbor_headings)[i].first);
                ahz_fp += ToFixed((*neighbor_headings)[i].second);
            }
        }
        if (dist > 1.0e-5f && dist < p.separation_radius) {
            // Away from this neighbour, weighted by how close it is (1 at touching -> 0 at radius).
            const float inv = 1.0f / dist;
            const float crowd = 1.0f - dist / p.separation_radius;
            sepx_fp += ToFixed((sx - nx) * inv * crowd);
            sepz_fp += ToFixed((sz - nz) * inv * crowd);
        }
    }

    if (cohesion_count > 0) {
        const float invn = 1.0f / static_cast<float>(cohesion_count);
        const float cx = FromFixed(cx_fp);
        const float cz = FromFixed(cz_fp);
        const float toward_x = cx * invn - sx;
        const float toward_z = cz * invn - sz;
        const float len = dm::Sqrt(toward_x * toward_x + toward_z * toward_z);
        if (len > 1.0e-5f) {
            const float k = p.cohesion_weight / len;
            steer.x += toward_x * k;
            steer.z += toward_z * k;
        }
        if (do_align) {
            // Toward the unit mean neighbour heading * alignment_weight (mirrors cohesion's
            // unit-normalize-then-weight, so the term is commensurate with cohesion/separation).
            const float mhx = FromFixed(ahx_fp) * invn;
            const float mhz = FromFixed(ahz_fp) * invn;
            const float al = dm::Sqrt(mhx * mhx + mhz * mhz);
            if (al > 1.0e-5f) {
                const float ak = p.alignment_weight / al;
                steer.x += mhx * ak;
                steer.z += mhz * ak;
            }
        }
    }
    steer.x += FromFixed(sepx_fp) * p.separation_weight;
    steer.z += FromFixed(sepz_fp) * p.separation_weight;
    return steer;
}

} // namespace luminumbra::ai
