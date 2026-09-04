#include "LightningBolt.h"

#include <algorithm>

namespace Luminumbra::Rendering {

namespace {

// splitmix64 finalizer -> a deterministic stream from a 64-bit key. Identical
// scheme to the sim-side strike RNG, kept local so this render util has no sim
// dependency. Pure integer arithmetic: reproducible bolt for a given seed.
std::uint64_t Mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Deterministic float in [0, 1) from a 64-bit key (top 24 bits / 2^24).
float UnitFloat(std::uint64_t key) {
    const std::uint32_t mantissa = static_cast<std::uint32_t>(Mix64(key) >> 40);
    return static_cast<float>(mantissa) * (1.0f / 16777216.0f);
}

// Deterministic float in [-1, 1).
float SignedFloat(std::uint64_t key) {
    return UnitFloat(key) * 2.0f - 1.0f;
}

} // namespace

LightningBoltGeometry BuildLightningBolt(float ground_x,
                                         float ground_y,
                                         float ground_z,
                                         float magnitude,
                                         std::uint64_t strike_seed,
                                         const LightningBoltParams& params) {
    LightningBoltGeometry bolt;
    bolt.ground_point = glm::vec3(ground_x, ground_y, ground_z);
    bolt.magnitude = std::clamp(magnitude, 0.0f, 1.0f);

    const glm::vec3 top(ground_x, ground_y + params.cloud_base_height, ground_z);
    const glm::vec3 bottom(ground_x, ground_y, ground_z);

    // Start with the straight cloud->ground segment, then midpoint-displace it.
    // Each subdivision inserts a displaced midpoint between every adjacent pair;
    // the lateral displacement (perpendicular-ish to the descent) shrinks by
    // jaggedness_falloff per level so the bolt is jagged at the top and tightens
    // toward the terminus. Endpoints are pinned (cloud base + ground point).
    std::vector<glm::vec3> pts{top, bottom};
    float disp = params.jaggedness * (0.6f + 0.4f * bolt.magnitude);
    std::uint64_t key = strike_seed ^ 0xB017B017B017B017ull;

    for (int level = 0; level < params.subdivisions; ++level) {
        std::vector<glm::vec3> next;
        next.reserve(pts.size() * 2 - 1);
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            const glm::vec3 a = pts[i];
            const glm::vec3 b = pts[i + 1];
            next.push_back(a);
            glm::vec3 mid = 0.5f * (a + b);
            // Lateral jitter in world XZ (the bolt descends in Y); a small Y jitter
            // too so segments are not perfectly co-planar.
            mid.x += SignedFloat(key) * disp;
            key = Mix64(key);
            mid.z += SignedFloat(key) * disp;
            key = Mix64(key);
            mid.y += SignedFloat(key) * disp * 0.15f;
            key = Mix64(key);
            next.push_back(mid);
        }
        next.push_back(pts.back());
        pts.swap(next);
        disp *= params.jaggedness_falloff;
    }
    bolt.main_channel = std::move(pts);

    // Branches: fork a short, downward-trending offshoot from a few interior main-
    // channel vertices. Deterministic count + attach points + directions.
    const int interior = static_cast<int>(bolt.main_channel.size());
    int branches_made = 0;
    for (int i = interior / 4; i < interior - 2 && branches_made < params.max_branches; i += 3) {
        if (UnitFloat(key) >= params.branch_chance) {
            key = Mix64(key);
            continue;
        }
        key = Mix64(key);
        const glm::vec3 root = bolt.main_channel[static_cast<std::size_t>(i)];
        // Branch direction: continue downward but veer sideways.
        glm::vec3 dir(SignedFloat(key), -0.6f - 0.4f * UnitFloat(key), SignedFloat(Mix64(key)));
        key = Mix64(key ^ 0xABCDEF1234567890ull);
        const float len = (18.0f + 30.0f * UnitFloat(key)) * (0.5f + 0.5f * bolt.magnitude);
        key = Mix64(key);
        std::vector<glm::vec3> branch;
        branch.push_back(root);
        glm::vec3 cur = root;
        const int seg = 3;
        for (int s = 0; s < seg; ++s) {
            cur += dir * (len / static_cast<float>(seg));
            cur.x += SignedFloat(key) * 6.0f;
            key = Mix64(key);
            cur.z += SignedFloat(key) * 6.0f;
            key = Mix64(key);
            branch.push_back(cur);
        }
        bolt.branches.push_back(std::move(branch));
        ++branches_made;
    }

    return bolt;
}

} // namespace Luminumbra::Rendering
