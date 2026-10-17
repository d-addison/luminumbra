#pragma once

// The frame loop's menu-branch rendering extracted verbatim from
// main_client.cpp: the --ui-thumbs backdrop-thumbnail capture, the scenic
// menu-backdrop world render, the create-world live preview diorama
// (candidate rebuild + orbit/zoom controls), the RmlUi menu render, and the
// --ui-screenshot batch capture. main() calls RenderMenuScreens from the
// frame loop's non-IN_GAME branch, at the exact point the inline code ran.

#include "app/ClientAppContext.h"
#include "app/MenuPreviewInput.h"

#include <string>

struct GLFWwindow;

namespace Luminumbra::Client {
class Rml_UIManager;
class WorldgenPreview;
} // namespace Luminumbra::Client
namespace Luminumbra::Rendering {
class Camera;
class RenderPipeline;
} // namespace Luminumbra::Rendering
namespace Luminumbra::world {
class GameSession;
}

namespace Luminumbra::Client::App {

void RenderMenuScreens(ClientAppContext& app,
                       GLFWwindow* window,
                       float deltaTime,
                       const std::string& root_path_str,
                       Luminumbra::Rendering::Camera* camera,
                       Luminumbra::world::GameSession* gameSession,
                       Luminumbra::Rendering::RenderPipeline& renderPipeline,
                       Luminumbra::Client::Rml_UIManager* uiManager,
                       Luminumbra::Client::WorldgenPreview* worldgenPreview,
                       MenuPreviewState& previewState,
                       const UiInputRouter& inputRouter);

} // namespace Luminumbra::Client::App
