#pragma once

#include <glm/glm.hpp>

namespace Luminumbra::Client::Player {

// Eye-height conventions shared by PlayerController and the saved-world spawn anchor.
inline constexpr float kStandingEyeHeightFactor = 0.95f;
inline constexpr float kCrouchingEyeHeightFactor = 0.9f;

inline float StandingEyeHeight(float standing_height) {
    return standing_height * kStandingEyeHeightFactor;
}

// The position a save records as the world spawn point for a walking player. It is
// the STANDING eye position over the feet regardless of the current stance, because
// world entry always derives the feet as (spawn point - standing eye height). Saving
// the live camera position while crouched would put the reloaded feet 0.9 x 0.9 m
// lower than the saved feet (the crouched eye sits lower than the standing eye).
inline glm::vec3 SavedSpawnAnchorForFeet(const glm::vec3& feet, float standing_height) {
    return feet + glm::vec3(0.0f, StandingEyeHeight(standing_height), 0.0f);
}

// Inverse used at world entry: feet derived from a saved spawn anchor.
inline glm::vec3 FeetFromSpawnAnchor(const glm::vec3& anchor, float standing_height) {
    return anchor - glm::vec3(0.0f, StandingEyeHeight(standing_height), 0.0f);
}

} // namespace Luminumbra::Client::Player
