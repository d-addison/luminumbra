#pragma once

// Gameplay/capture presentation and registered developer widget contents.
// The registry owns widget windows, visibility and focus. Shader watching and
// queued reload servicing remain independent render-thread operations.

#include "app/ClientAppContext.h"
#include "app/WindowModeControls.h"
#include "core/GameState.h"
#include "core/RuntimeScenarioConfig.h"
#include "debug/DeveloperConsole.h"

struct GLFWwindow;

namespace Luminumbra::Client {
class IAudioManager;
class PlayerController;
class Rml_UIManager;
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

// Photo-mode capture loop + codex/objectives/farming HUD. Runs AFTER
// render_frame against a CONST registry; it never ticks the sim or mutates
// the registry, so determinism cannot regress. Interactive play only.
void UpdatePhotoModeAndHud(ClientAppContext& app,
                           GameState currentState,
                           GLFWwindow* window,
                           const std::string& root_path_str,
                           Luminumbra::Client::PlayerController* playerController,
                           Luminumbra::Rendering::Camera* camera,
                           Luminumbra::Client::Rml_UIManager* uiManager,
                           Luminumbra::Client::IAudioManager* audioManager,
                           Luminumbra::world::GameSession* gameSession,
                           Luminumbra::Rendering::RenderPipeline& renderPipeline,
                           const ScenarioHarness::RuntimeScenarioConfig& scenario_config);

// Floating creature ID nameplates for the ecology demo capture (drawn into
// the ImGui foreground list, so they show during the timelapse too).
void DrawCreatureNameplates(ClientAppContext& app,
                            GLFWwindow* window,
                            Luminumbra::Rendering::Camera* camera,
                            Luminumbra::world::GameSession* gameSession);

// Gameplay status entry point retained for callers without a session.
void DrawFrameStatusOverlays(ClientAppContext& app,
                             GameState currentState,
                             const ScenarioHarness::RuntimeScenarioConfig& scenario_config,
                             Luminumbra::Client::PlayerController* playerController);

// Registered widget contents; visibility/window ownership belongs to the registry.
void DrawFarmDiagnosticsContents(ClientAppContext& app);
void DrawGpuProfilerContents(Luminumbra::Rendering::RenderPipeline& renderPipeline,
                             Luminumbra::Client::Rml_UIManager* uiManager);

// --debug-glass-pane: stage the three stained-glass panes near spawn once.
void SpawnDebugGlassPanes(ClientAppContext& app,
                          GameState currentState,
                          Luminumbra::world::GameSession* gameSession,
                          Luminumbra::Rendering::RenderPipeline& renderPipeline);

// Render-thread reload requests and the independent opt-in shader watcher.
void UpdateShaderTools(ClientAppContext& app,
                       GameState currentState,
                       Luminumbra::Rendering::RenderPipeline& renderPipeline);

void DrawShaderContents(ClientAppContext& app,
                        Luminumbra::Rendering::RenderPipeline& renderPipeline,
                        Debug::DeveloperConsole& console);

// Settings contents (render-only; user.* is never hashed).
void DrawSettingsContents(ClientAppContext& app,
                          GLFWwindow* window,
                          Luminumbra::Rendering::Camera* camera,
                          Luminumbra::Client::IAudioManager* audioManager,
                          luminumbra::core::SystemConfig& systemConfig,
                          WindowState& windowState,
                          Debug::DeveloperConsole& console);

} // namespace Luminumbra::Client::App
