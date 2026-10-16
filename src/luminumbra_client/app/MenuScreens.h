#pragma once

// The frame loop's menu rendering, extracted from
// main_client.cpp: the --ui-thumbs backdrop-thumbnail capture, the scenic
// menu-backdrop world render, the create-world live preview diorama
// (candidate rebuild + orbit/zoom controls), the RmlUi menu render, and the
// --ui-screenshot batch capture. main() calls RenderMenuScreens from the
// frame loop's non-IN_GAME branch, at the exact point the inline code ran.

#include "app/ClientAppContext.h"
#include "app/MenuSceneSchedule.h"

#include <chrono>
#include <cstdint>
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

// One full-size RGBA8 texture, bounded by GL_MAX_TEXTURE_SIZE. Release while
// the window context is current; the destructor is then a no-op at shutdown.
class MenuSceneImage {
public:
    MenuSceneImage() = default;
    ~MenuSceneImage();
    MenuSceneImage(const MenuSceneImage&) = delete;
    MenuSceneImage& operator=(const MenuSceneImage&) = delete;
    void release_scene_image();
    bool ensure_scene_image(int width, int height);
    void copy_scene_image(bool capture) const;

private:
    unsigned int m_scene_texture = 0;
    unsigned int m_scene_framebuffer = 0;
    int m_scene_width = 0, m_scene_height = 0;
};

// Create-world preview orbit/drag bookkeeping (formerly main()-locals): the
// last candidate signature so params are only re-derived when the form
// actually changed, plus the cursor-drag state for the turntable orbit.
struct MenuPreviewState {
    std::string lastSig;
    bool dragging = false;
    double lastCursorX = 0.0, lastCursorY = 0.0;
    MenuSceneSchedule schedule;
    MenuCameraRevision camera_revision;
    std::uint64_t preview_revision = 0;
    float last_tod = -1;
    std::string last_weather;
    std::chrono::steady_clock::time_point last_menu_frame{};
    MenuSceneImage scene_image;
};

void RenderMenuScreens(ClientAppContext& app,
                       GLFWwindow* window,
                       float deltaTime,
                       const std::string& root_path_str,
                       Luminumbra::Rendering::Camera* camera,
                       Luminumbra::world::GameSession* gameSession,
                       Luminumbra::Rendering::RenderPipeline& renderPipeline,
                       Luminumbra::Client::Rml_UIManager* uiManager,
                       Luminumbra::Client::WorldgenPreview* worldgenPreview,
                       MenuPreviewState& previewState);

} // namespace Luminumbra::Client::App
