#pragma once

// Per-frame audio region extracted verbatim from main_client.cpp's frame
// loop: the 3D listener follow, material-keyed player footsteps, and the
// living-world ambience (weather-reactive rain/streams/waterfall roar,
// creature calls/sleep/feeding/colony/locomotion, and the dawn/dusk cues).
// Render/audio-only — it reads sim state and never mutates it, so determinism
// and the visual gates are untouched.

#include "app/ClientAppContext.h"
#include "core/GameState.h"
#include "core/RuntimeScenarioConfig.h"

namespace Luminumbra::Client {
class IAudioManager;
class EnvironmentalAudioSystem;
} // namespace Luminumbra::Client
namespace Luminumbra::Rendering {
class Camera;
class RenderPipeline;
} // namespace Luminumbra::Rendering
namespace Luminumbra::world {
class GameSession;
}
namespace luminumbra::core {
class SystemConfig;
}

namespace Luminumbra::Client::App {

// Runs the whole frame-audio region at the exact point the inline code ran:
// after the UI update, before the game-state switch. All parameters are the
// same objects the inline code reached for (the audio manager and session are
// main()-locals; the camera is the core singleton).
void UpdateFrameAudio(ClientAppContext& app,
                      GameState currentState,
                      float deltaTime,
                      Luminumbra::Client::IAudioManager* audioManager,
                      Luminumbra::Client::EnvironmentalAudioSystem* envAudio,
                      Luminumbra::Rendering::Camera* camera,
                      luminumbra::core::SystemConfig& systemConfig,
                      Luminumbra::world::GameSession* gameSession,
                      Luminumbra::Rendering::RenderPipeline& renderPipeline,
                      const ScenarioHarness::RuntimeScenarioConfig& scenario_config);

} // namespace Luminumbra::Client::App
