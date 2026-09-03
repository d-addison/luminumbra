#pragma once

// The frame loop's overlay regions extracted verbatim from main_client.cpp:
// the photo-mode capture loop with its codex/objectives/farming HUD, the
// creature ID nameplates, the ImGui debug windows (time-scale indicator, crop
// HUD, settings), the live per-pass GPU profiler, the shader-authoring tools
// (reload requests, mtime watcher, dev panel), and the one-time glass-pane
// capture subject. All render-side, read-only observers of sim state; main()
// calls each function at the exact point the inline region ran.

#include "app/ClientAppContext.h"
#include "app/WindowModeControls.h"
#include "core/GameState.h"
#include "core/RuntimeScenarioConfig.h"

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

// Player debug UI, the always-on time-scale indicator, and the minimal crop
// HUD.
void DrawFrameStatusOverlays(ClientAppContext& app,
                             GameState currentState,
                             const ScenarioHarness::RuntimeScenarioConfig& scenario_config,
                             Luminumbra::Client::PlayerController* playerController);

// Live per-pass GPU profiler overlay (GL_TIMESTAMP timer ring readout).
void DrawGpuProfilerOverlay(ClientAppContext& app,
                            GameState currentState,
                            Luminumbra::Rendering::RenderPipeline& renderPipeline,
                            Luminumbra::Client::Rml_UIManager* uiManager);

// --debug-glass-pane: stage the three stained-glass panes near spawn once.
void SpawnDebugGlassPanes(ClientAppContext& app,
                          GameState currentState,
                          Luminumbra::world::GameSession* gameSession,
                          Luminumbra::Rendering::RenderPipeline& renderPipeline);

// Live shader authoring: reload-all requests, the opt-in once/sec mtime
// watcher, and the dev shader panel with live uniform editing.
void UpdateShaderTools(ClientAppContext& app,
                       GameState currentState,
                       Luminumbra::Rendering::RenderPipeline& renderPipeline);

// The ImGui settings menu (render-only; user.* is never hashed).
void DrawSettingsWindow(ClientAppContext& app,
                        GLFWwindow* window,
                        Luminumbra::Rendering::Camera* camera,
                        Luminumbra::Client::IAudioManager* audioManager,
                        luminumbra::core::SystemConfig& systemConfig,
                        WindowState& windowState);

} // namespace Luminumbra::Client::App
