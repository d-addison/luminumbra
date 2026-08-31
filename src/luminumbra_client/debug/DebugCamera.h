#pragma once

// ============================================================================
// DebugCamera — deterministic feature LOCATOR + camera FRAMER for visual tests.
// ============================================================================
//
// The gap this closes: visual tests (--frame-scan / --timelapse / --ui-screenshot)
// could pin a FIXED camera (g_fixed_cam_pos/yaw/pitch) but had no reproducible way
// to AIM that camera AT a specific world feature — a cave, a doline, a landmark.
// My ad-hoc downward density probes kept landing in OPEN AIR (above-surface sky or
// the lip of a depression) instead of inside a real, enclosed cave, so the screenshot
// framed nothing. These pure helpers fix that:
//
//   (a) FindEnclosedCave  — scans the deterministic SDF for a TRUE enclosed cave-air
//       pocket (air below, SOLID overhead within a few metres, solid surrounding it —
//       i.e. a roofed cavern, not open sky or a surface dip), and returns a camera
//       placement just inside the opening looking INTO the cavity.
//   (b) FrameFeature      — given any feature world-position + a rough radius, computes
//       a sensible camera pose (distance / azimuth / pitch) that frames it.
//   (c) FindDoline        — thin wrapper over SHIELD_WorldSystem::FindLargestSurfaceBreak,
//       returning a FrameFeature pose aimed down into the cave mouth.
//
// DETERMINISM CONTRACT: these are PURE READS of the deterministic SDF / placement.
// They NEVER mutate sim state and NEVER feed world_hash. They only produce a camera
// pose, which is render-only. Calling them is byte-identical to not calling them.
//
// All distances are metres in world space (Y up). Yaw/pitch are degrees in the same
// convention as the engine Camera (yaw 0 = +X..., handled internally so the caller
// just copies pos/yaw/pitch into g_fixed_cam_*).

#include <glm/glm.hpp>

#include <optional>

namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
}

namespace Luminumbra::Debug {

// A located + framed camera pose. Drop straight into the fixed-cam globals:
//   g_fixed_cam_pos = pose.pos; g_fixed_cam_yaw = pose.yaw; g_fixed_cam_pitch = pose.pitch;
struct DebugCamPose {
    glm::vec3 pos{0.0f};
    float     yaw   = 0.0f;   // degrees, engine Camera convention
    float     pitch = 0.0f;   // degrees, engine Camera convention
    // The world point the camera is aimed at (the feature). Handy for logging / a
    // secondary "look target" if the caller drives a look-at camera instead.
    glm::vec3 target{0.0f};
};

// ---------------------------------------------------------------------------
// (b) FrameFeature — pure geometry. Given a feature centre + its rough radius,
// pick a camera distance that fits the feature in frame at a cinematic 3/4 angle
// and return the pose looking at it. `azimuth_deg` rotates the camera around the
// feature (deterministic default 135 => looks toward -X/-Z, sun roughly behind-left);
// `pitch_down_deg` tilts the view downward (default gives a slightly elevated look).
// `distance_scale` multiplies the auto-fit distance (1.0 = snug, >1 = pull back).
// This touches NOTHING but math; always succeeds.
// ---------------------------------------------------------------------------
DebugCamPose FrameFeature(const glm::vec3& feature_world_pos,
                          float feature_radius,
                          float azimuth_deg     = 135.0f,
                          float pitch_down_deg  = 18.0f,
                          float distance_scale  = 1.0f);

// ---------------------------------------------------------------------------
// (a) FindEnclosedCave — locate a real, roofed cave-air pocket near `near_world`
// within `search_radius_m`, and return a camera pose JUST OUTSIDE/INSIDE the
// opening looking INTO the void.
//
// "Enclosed" test at a candidate air sample p (air = get_density_at(p) >= 0; the
// worldgen density is (y - height) + cave carve, so negative = solid terrain):
//   * SOLID overhead: at least one solid sample within `roof_probe_m` straight up
//     (so it's roofed — not open sky / a surface depression),
//   * an OPENING to look through: a horizontal direction in which the cave-air run
//     is at least `min_cavity_m` before hitting solid (so there's a void to frame),
//   * solid floor below within a short drop (so the camera has something to stand on
//     and the pocket isn't a thin sliver).
//
// The scan is a deterministic ring/lattice walk (fixed iteration order, fixed step),
// so the SAME world always returns the SAME cave — reproducible screenshots. It
// prefers the LARGEST cavity found (longest clear horizontal run) within the budget.
//
// Returns nullopt when caves are disabled / none qualifies in range. Render-only:
// every call is a pure read of the deterministic SDF.
// ---------------------------------------------------------------------------
std::optional<DebugCamPose>
FindEnclosedCave(const Systems::SHIELD_WorldSystem& world,
                 const glm::vec3& near_world,
                 float search_radius_m = 256.0f);

// ---------------------------------------------------------------------------
// (c) FindDoline — reuse the engine's deterministic surface-break (doline / cenote)
// locator and frame a camera looking DOWN into the cave mouth. Returns nullopt when
// surface breaks are disabled or none are in range. Pure read.
// ---------------------------------------------------------------------------
std::optional<DebugCamPose>
FindDoline(const Systems::SHIELD_WorldSystem& world,
           const glm::vec3& near_world,
           float search_radius_m = 500.0f);

} // namespace Luminumbra::Debug
