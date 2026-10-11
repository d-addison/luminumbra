#pragma once

#include <glm/glm.hpp>

namespace Luminumbra::BenchmarkPose {

// Default render-benchmark camera pose and time of day.
inline constexpr float kPosX = 8.0f;
inline constexpr float kPosY = 56.0f;
inline constexpr float kPosZ = 8.0f;
inline constexpr float kYaw = 35.0f;
inline constexpr float kPitch = -6.0f;
inline constexpr float kTimeOfDay = 0.04f;

struct StreamingInputs {
    bool fixed_cam;
    glm::vec3 fixed_cam_pos;
    bool benchmark_active;
    bool has_camera;
    glm::vec3 camera_pos; // read only when has_camera
    bool cam_anchored;
    bool has_player;
    glm::vec3 player_pos;
};

// Precedence: fixed_cam -> (benchmark_active && has_camera) -> cam_anchored ->
// (has_player ? player_pos : camera_pos).
inline glm::vec3 SelectStreamingPosition(const StreamingInputs& in) {
    if (in.fixed_cam) {
        return in.fixed_cam_pos;
    }
    if (in.benchmark_active && in.has_camera) {
        return glm::vec3(kPosX, kPosY, kPosZ);
    }
    if (in.cam_anchored) {
        return in.camera_pos;
    }
    return in.has_player ? in.player_pos : in.camera_pos;
}

} // namespace Luminumbra::BenchmarkPose
