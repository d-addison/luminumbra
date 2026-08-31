#include "DebugCamera.h"

#include "../../luminumbra_common/systems/SHIELD_WorldSystem.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace Luminumbra::Debug {

namespace {

// Engine Camera convention (Camera.h::updateCameraVectors):
//   front.x = cos(yaw)cos(pitch); front.y = sin(pitch); front.z = sin(yaw)cos(pitch).
// Invert it: given a desired (normalized) look direction, recover yaw/pitch in DEGREES.
void DirToYawPitch(const glm::vec3& dir_in, float& yaw_deg, float& pitch_deg) {
    glm::vec3 dir = dir_in;
    const float len = glm::length(dir);
    if (len < 1e-6f) { yaw_deg = 0.0f; pitch_deg = 0.0f; return; }
    dir /= len;
    pitch_deg = glm::degrees(std::asin(std::clamp(dir.y, -1.0f, 1.0f)));
    yaw_deg   = glm::degrees(std::atan2(dir.z, dir.x));
    // Mirror the engine's pitch clamp so the chosen pose is reachable / stable.
    pitch_deg = std::clamp(pitch_deg, -89.0f, 89.0f);
}

inline float Density(const Systems::SHIELD_WorldSystem& world, const glm::vec3& p) {
    return world.get_density_at(Luminumbra::Vec3(p.x, p.y, p.z));
}
// DENSITY CONVENTION (authoritative: the mesher) — final_density = (y - height)
// composed with the cave carve, so NEGATIVE = inside solid terrain and >= 0 = air/
// void; MarchingCubes classifies the SOLID corner as val < isolevel(0). These
// helpers originally shipped INVERTED (air = < 0, 2026-06-26), which made every
// locator hunt rock shelves instead of cave voids and, copied into the foliage/tree
// roof probes, rejected every open-sky column (the bare-world regression).
inline bool IsAir(const Systems::SHIELD_WorldSystem& world, const glm::vec3& p) {
    return Density(world, p) >= 0.0f;
}
inline bool IsSolid(const Systems::SHIELD_WorldSystem& world, const glm::vec3& p) {
    return Density(world, p) < 0.0f;
}

// March from `start` along `dir` (unit) while air, in `step` increments, up to
// `max_dist`. Returns the cave-air run length before solid (or max_dist if it never
// hits solid within budget). Fixed iteration order => deterministic.
float AirRunLength(const Systems::SHIELD_WorldSystem& world,
                   const glm::vec3& start, const glm::vec3& dir,
                   float step, float max_dist) {
    float d = step;
    for (; d <= max_dist; d += step) {
        if (IsSolid(world, start + dir * d)) return d - step;
    }
    return max_dist;
}

} // namespace

// ----------------------------------------------------------------------------
DebugCamPose FrameFeature(const glm::vec3& feature_world_pos,
                          float feature_radius,
                          float azimuth_deg,
                          float pitch_down_deg,
                          float distance_scale) {
    DebugCamPose pose;
    pose.target = feature_world_pos;

    // Auto-fit distance: pull back enough that a sphere of `feature_radius` fits in a
    // ~50deg vertical FOV with margin, with a sane floor for tiny features.
    const float r = std::max(feature_radius, 1.0f);
    const float fit = r / std::tan(glm::radians(25.0f));        // half-FOV ~25deg
    float distance = std::max(fit * 1.6f, r + 4.0f) * std::max(distance_scale, 0.05f);

    // Place the camera on a ring around the feature, raised by the down-pitch angle.
    const float az    = glm::radians(azimuth_deg);
    const float pitch = glm::radians(std::clamp(pitch_down_deg, -89.0f, 89.0f));
    const glm::vec3 offset{
        std::cos(az) * std::cos(pitch),
        std::sin(pitch),                 // +pitch_down => camera ABOVE the feature
        std::sin(az) * std::cos(pitch),
    };
    pose.pos = feature_world_pos + offset * distance;

    // Aim back at the feature.
    DirToYawPitch(feature_world_pos - pose.pos, pose.yaw, pose.pitch);
    return pose;
}

// ----------------------------------------------------------------------------
std::optional<DebugCamPose>
FindEnclosedCave(const Systems::SHIELD_WorldSystem& world,
                 const glm::vec3& near_world,
                 float search_radius_m) {
    // Deterministic lattice: fixed horizontal ring radii x fixed azimuths, and at each
    // column a fixed downward column of probe depths. Same world => same first/best hit.
    constexpr float kHStep        = 4.0f;    // ring radius increment (m)
    constexpr int   kAzCount      = 16;      // azimuth samples per ring
    constexpr float kColTop       = 6.0f;    // start probing this far ABOVE surface
    constexpr float kColBottom    = -64.0f;  // ...down to this far below surface
    constexpr float kColStep      = 2.0f;    // vertical probe spacing (m)
    constexpr float kRoofProbeM   = 8.0f;    // solid must exist within this overhead
    constexpr float kRoofStep     = 1.0f;
    constexpr float kFloorProbeM  = 8.0f;    // solid floor within this drop
    constexpr float kMinCavityM   = 4.0f;    // a usable void must be at least this wide
    constexpr float kHorizStep    = 1.0f;    // horizontal march resolution

    // 8 cardinal/diagonal horizontal directions to test for an opening (deterministic).
    constexpr int kDirN = 8;
    const glm::vec3 hdirs[kDirN] = {
        { 1, 0, 0}, { 0, 0, 1}, {-1, 0, 0}, { 0, 0,-1},
        { 0.70710678f, 0, 0.70710678f}, {-0.70710678f, 0, 0.70710678f},
        { 0.70710678f, 0,-0.70710678f}, {-0.70710678f, 0,-0.70710678f},
    };

    bool      have_best = false;
    float     best_cavity = kMinCavityM;     // maximize the clear horizontal run
    glm::vec3 best_air{0.0f};
    glm::vec3 best_dir{1, 0, 0};

    // ring 0 (the centre column) then expanding rings.
    for (float ring = 0.0f; ring <= search_radius_m; ring += kHStep) {
        const int az_count = (ring < 0.5f) ? 1 : kAzCount;
        for (int ai = 0; ai < az_count; ++ai) {
            const float az = (2.0f * glm::pi<float>() * ai) / static_cast<float>(kAzCount);
            const float wx = near_world.x + std::cos(az) * ring;
            const float wz = near_world.z + std::sin(az) * ring;
            const float surf = world.GetTerrainHeightAt(wx, wz);

            // Walk DOWN the column looking for the first qualifying enclosed air pocket.
            for (float dy = kColTop; dy >= kColBottom; dy -= kColStep) {
                const glm::vec3 p{wx, surf + dy, wz};
                if (!IsAir(world, p)) continue;

                // ROOF: solid overhead within kRoofProbeM (rejects open sky / surface dips).
                bool roofed = false;
                for (float up = kRoofStep; up <= kRoofProbeM; up += kRoofStep) {
                    if (IsSolid(world, p + glm::vec3(0, up, 0))) { roofed = true; break; }
                }
                if (!roofed) continue;

                // FLOOR: solid below within a short drop (so it's a room, not a thin gap
                // and the camera has ground beneath it).
                bool floored = false;
                for (float dn = kColStep; dn <= kFloorProbeM; dn += kRoofStep) {
                    if (IsSolid(world, p - glm::vec3(0, dn, 0))) { floored = true; break; }
                }
                if (!floored) continue;

                // OPENING: find the horizontal direction with the LONGEST clear cave-air
                // run (that's the axis we want to look down — the cavity).
                float    pocket_best = 0.0f;
                glm::vec3 pocket_dir = hdirs[0];
                for (int di = 0; di < kDirN; ++di) {
                    const float run = AirRunLength(world, p, hdirs[di], kHorizStep,
                                                   std::max(kMinCavityM * 3.0f, 24.0f));
                    if (run > pocket_best) { pocket_best = run; pocket_dir = hdirs[di]; }
                }
                if (pocket_best < kMinCavityM) continue; // sliver, not a room

                if (pocket_best > best_cavity) {
                    have_best   = true;
                    best_cavity = pocket_best;
                    best_air    = p;
                    best_dir    = pocket_dir;
                }
                // One qualifying pocket per column is enough; move outward.
                break;
            }
        }
        // Early-out: once we've found a generously large cavity, stop expanding (keeps the
        // scan bounded AND deterministic — the first ring that yields a big room wins).
        if (have_best && best_cavity >= 16.0f) break;
    }

    if (!have_best) return std::nullopt;

    // Place the camera a little back from the air sample, biased toward the opening's
    // ENTRANCE so it looks INTO the void (not at the wall behind it). Step back along
    // -best_dir to the cavity mouth, then nudge up off the floor to eye height.
    const float back = std::min(best_cavity * 0.5f, 6.0f);
    glm::vec3 cam = best_air - best_dir * back + glm::vec3(0.0f, 1.6f, 0.0f);
    // Guard: if that nudge pushed the camera into solid, fall back to the air sample.
    if (IsSolid(world, cam)) cam = best_air + glm::vec3(0.0f, 1.0f, 0.0f);

    const glm::vec3 look_target = best_air + best_dir * std::min(best_cavity, 12.0f);

    DebugCamPose pose;
    pose.pos    = cam;
    pose.target = look_target;
    DirToYawPitch(look_target - cam, pose.yaw, pose.pitch);
    return pose;
}

// ----------------------------------------------------------------------------
std::optional<DebugCamPose>
FindDoline(const Systems::SHIELD_WorldSystem& world,
           const glm::vec3& near_world,
           float search_radius_m) {
    const auto sb = world.FindLargestSurfaceBreak(near_world.x, near_world.z, search_radius_m);
    if (!sb.found) return std::nullopt;

    // The funnel mouth: surface height at the doline centre, looking down into the shaft.
    const float surf = world.GetTerrainHeightAt(sb.x, sb.z);
    const glm::vec3 mouth{sb.x, surf, sb.z};

    // Frame from above the rim, tilted steeply down into the opening. A cenote SHAFT
    // wants a near-top-down look; a wide funnel a flatter 3/4 look.
    const float pitch_down = sb.shaft ? 55.0f : 35.0f;
    DebugCamPose pose = FrameFeature(mouth,
                                     std::max(sb.radius, 4.0f),
                                     /*azimuth_deg=*/135.0f,
                                     /*pitch_down_deg=*/pitch_down,
                                     /*distance_scale=*/1.0f);
    // Aim at the throat (a few metres into the funnel) so the dark opening, not the rim
    // grass, is centred.
    const glm::vec3 throat{sb.x, surf - std::min(sb.depth * 0.4f, 8.0f), sb.z};
    pose.target = throat;
    DirToYawPitch(throat - pose.pos, pose.yaw, pose.pitch);
    return pose;
}

} // namespace Luminumbra::Debug
