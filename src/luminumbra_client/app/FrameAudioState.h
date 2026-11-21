#pragma once

#include "audio/IAudioManager.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Luminumbra::Client::App {

// Client presentation cues only; never serialized or included in authority hashes.
struct PendingThunder {
    std::uint64_t strike_tick = 0;
    double fired_at_seconds = 0.0;
    float distance_m = 0.0f;
    float magnitude = 0.0f;
};

// App-owned history for one world presentation. Reset on replacement, clear or menu exit.
struct FrameAudioState {
    std::vector<PendingThunder> pendingThunder;
    bool waterfallRoarOn = false;
    glm::vec3 waterfallRoarPosition{0.0f};
    bool footInitialized = false;
    glm::vec3 footLastPos{0.0f};
    float footDist = 0.0f;
    float envTimer = 0.0f;
    float callTimer = 0.0f;
    float sleepTimer = 0.0f;
    float feedTimer = 0.0f;
    float colonyTimer = 0.0f;
    float thunderTimer = 0.0f;
    bool rainOn = false;
    bool waterOn = false;
    int sunUp = -1; // -1 uninitialized, 0 below horizon, 1 above
    std::unordered_map<std::uint32_t, std::pair<glm::vec2, float>> stride;

    // Stop owned loops before forgetting their flags. Null audio needs only the state reset.
    void Reset(Luminumbra::Client::IAudioManager* audioManager = nullptr) {
        if (audioManager) {
            if (rainOn)
                audioManager->StopAmbientLoop("ambient_rain");
            if (waterOn)
                audioManager->StopAmbientLoop("ambient_stream");
            if (waterfallRoarOn)
                audioManager->StopAmbientLoop("waterfall_roar");
        }
        *this = FrameAudioState{};
    }

    void AdvanceTimers(float deltaTime) {
        envTimer += deltaTime;
        callTimer += deltaTime;
        sleepTimer += deltaTime;
        feedTimer += deltaTime;
        colonyTimer += deltaTime;
    }

    // Called only on eligible live frames, preserving pause/scenario/audio-disabled gating.
    bool PlayerFootstep(const glm::vec3& position, float deltaTime) {
        if (!footInitialized) {
            footLastPos = position;
            footInitialized = true;
        }
        const float dx = position.x - footLastPos.x, dz = position.z - footLastPos.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        footLastPos = position;
        const float speed = deltaTime > 0.0f ? horizontal / deltaTime : 0.0f;
        if (speed < 0.8f || speed > 25.0f) {
            footDist = 0.0f;
        } else {
            footDist += horizontal;
            if (footDist >= 1.9f) {
                footDist = 0.0f;
                return true;
            }
        }
        return false;
    }

    void BeginCreatureFrame() {
        // Preserve the existing 512-entry clear threshold at the frame boundary.
        // A frame can add at most one entry per audible creature before the next clear.
        if (stride.size() > 512)
            stride.clear();
    }

    bool CreatureFootfall(std::uint32_t key, const glm::vec2& position, float strideLength) {
        auto it = stride.find(key);
        if (it == stride.end()) {
            stride.emplace(key, std::make_pair(position, 0.0f));
            return false;
        }
        const float moved = glm::distance(position, it->second.first);
        it->second.first = position;
        if (moved > 5.0f)
            return false;
        it->second.second += moved;
        if (it->second.second >= strideLength) {
            it->second.second -= strideLength;
            return true;
        }
        return false;
    }
};

} // namespace Luminumbra::Client::App
