#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "app/ClientAppContext.h"
#include "app/FrameAttribution.h"
#include "app/MenuSceneSchedule.h"
#include "app/MenuScreens.h"
#include "app/UiInputRouter.h"

#include "core/Log.h"
#include "luminumbra_common/persistence/SavedInputLimits.h"
#include "luminumbra_common/persistence/SavedJsonAdmission.h"
#include "luminumbra_common/world/GameSession.h"
#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"
#include "rendering/Camera.h"
#include "rendering/PixelIo.h"
#include "rendering/RenderPipeline.h"
#include "ui/Rml_UIManager.h"
#include "ui/UiLayout.h"
#include "world/WorldgenOverride.h"
#include "world/WorldgenPreview.h"

#include <RmlUi/Core/Element.h>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::Client::App {

using namespace Luminumbra::Client::ScenarioHarness;

namespace {
using Luminumbra::Rendering::WritePixelBufferPpm;

bool ReadPreviewPreset(const std::filesystem::path& path, nlohmann::json& preset) {
    bool missing = false;
    return Persistence::ReadSavedJson(
               path, Persistence::SavedInputLimits::kPresetBytes, preset, missing, false) &&
           !missing;
}

MenuCameraRevision::State MenuCameraState(const Luminumbra::Rendering::Camera& camera) {
    return {camera.Position.x,
            camera.Position.y,
            camera.Position.z,
            camera.Front.x,
            camera.Front.y,
            camera.Front.z,
            camera.Up.x,
            camera.Up.y,
            camera.Up.z,
            camera.WorldUp.x,
            camera.WorldUp.y,
            camera.WorldUp.z,
            camera.Yaw,
            camera.Pitch,
            camera.Zoom};
}

// Readback overload for the menu capture paths; the shared four-argument writer
// owns PPM validation and I/O error reporting.
bool WritePixelBufferPpm(const std::filesystem::path& shot, int width, int height) {
    const auto rgba_bytes = menu_image_bytes(width, height);
    if (!rgba_bytes)
        return false;
    try {
        // One tightly packed RGB8 image, bounded by the checked RGBA8 size.
        std::vector<unsigned char> pixels((*rgba_bytes / 4) * 3);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        return WritePixelBufferPpm(shot, width, height, pixels);
    } catch (const std::exception& error) {
        LUMINUMBRA_CORE_ERROR("Menu capture output failed: {}", error.what());
        return false;
    }
}

bool WriteUiScreenshot(const std::filesystem::path& shot,
                       GLFWwindow* window,
                       Luminumbra::Client::Rml_UIManager* uiManager,
                       const std::string& screen) {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    try {
        // Validate and serialize the snapshot before either output is opened.
        // Both single and batch CLI captures use this path; normal frames do not.
        // The backbuffer contains RmlUi; ImGui is composited after this readback.
        const auto layout = UI::CollectUiLayout(
            uiManager ? uiManager->GetContext() : nullptr, nullptr, screen, width, height);
        const auto bytes = UI::SerializeUiLayout(layout);
        auto sidecar = shot;
        sidecar.replace_extension(".layout.json");
        if (!WritePixelBufferPpm(shot, width, height))
            return false;
        LUMINUMBRA_CORE_INFO("UI screenshot written -> {} ({}x{})", shot.string(), width, height);
        // Metadata is last. A failed sidecar refuses the capture without advancing
        // the batch; the successfully written screenshot remains available.
        if (!Luminumbra::Rendering::WriteCaptureText(sidecar, bytes)) {
            LUMINUMBRA_CORE_ERROR("UI layout output failed: {}", sidecar.string());
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        LUMINUMBRA_CORE_ERROR("UI layout capture failed: {}", error.what());
        return false;
    }
}
} // namespace

MenuSceneImage::~MenuSceneImage() {
    release_scene_image();
}

void MenuSceneImage::release_scene_image() {
    if (m_scene_framebuffer)
        glDeleteFramebuffers(1, &m_scene_framebuffer);
    if (m_scene_texture)
        glDeleteTextures(1, &m_scene_texture);
    m_scene_framebuffer = 0;
    m_scene_texture = 0;
    m_scene_width = 0;
    m_scene_height = 0;
}

bool MenuSceneImage::ensure_scene_image(int width, int height) {
    if (m_scene_framebuffer && width == m_scene_width && height == m_scene_height)
        return true;
    release_scene_image();
    if (!menu_image_bytes(width, height))
        return false;
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    if (width > max_size || height > max_size)
        return false;
    GLint previous_texture = 0, previous_draw = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    glGenTextures(1, &m_scene_texture);
    if (!m_scene_texture)
        return false;
    glBindTexture(GL_TEXTURE_2D, m_scene_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &m_scene_framebuffer);
    if (!m_scene_framebuffer) {
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
        release_scene_image();
        return false;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scene_framebuffer);
    glFramebufferTexture2D(
        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_scene_texture, 0);
    const bool complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    if (!complete) {
        release_scene_image();
        return false;
    }
    m_scene_width = width;
    m_scene_height = height;
    return true;
}

void MenuSceneImage::copy_scene_image(bool capture) const {
    GLint previous_read = 0, previous_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    const bool srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB) == GL_TRUE;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB); // copy the exact displayed bytes in both directions
    glBindFramebuffer(GL_READ_FRAMEBUFFER, capture ? 0 : m_scene_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, capture ? m_scene_framebuffer : 0);
    glBlitFramebuffer(0,
                      0,
                      m_scene_width,
                      m_scene_height,
                      0,
                      0,
                      m_scene_width,
                      m_scene_height,
                      GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw));
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    if (srgb)
        glEnable(GL_FRAMEBUFFER_SRGB);
}

namespace {
bool MenuThumbnailsPending(const ClientAppContext& app) {
    return app.capture.ui_thumbs > 0 && app.capture.ui_thumbs_index < app.capture.ui_thumbs;
}

void FailMenuCapture(ClientAppContext& app,
                     GLFWwindow* window,
                     Luminumbra::Client::WorldgenPreview* worldgenPreview,
                     MenuSceneState& previewState,
                     const char* reason) {
    const bool thumbnails_pending = MenuThumbnailsPending(app);
    const bool screenshot_pending = app.capture.ui_screen_index < app.capture.ui_screens.size();

    previewState.capture_failed = true;
    LUMINUMBRA_CORE_ERROR(
        "Menu capture failed: {} thumbnail={} index={} screen='{}' "
        "candidate_source_failed={} build_failed={} last_error='{}'",
        reason,
        thumbnails_pending,
        thumbnails_pending ? static_cast<std::size_t>(app.capture.ui_thumbs_index)
                           : app.capture.ui_screen_index,
        screenshot_pending ? app.capture.ui_screens[app.capture.ui_screen_index] : "",
        previewState.candidate_source_failed,
        worldgenPreview && worldgenPreview->last_build_failed(),
        worldgenPreview ? worldgenPreview->last_error() : std::string("<null preview>"));
    glfwSetWindowShouldClose(window, GLFW_TRUE);
}

bool PrepareMenuCapture(ClientAppContext& app,
                        GLFWwindow* window,
                        Luminumbra::Client::Rml_UIManager* uiManager,
                        Luminumbra::Client::WorldgenPreview* worldgenPreview,
                        MenuSceneState& previewState,
                        MenuCaptureGate::Time menu_now,
                        int& capture_width,
                        int& capture_height) {
    const bool thumbnails_pending = MenuThumbnailsPending(app);
    const bool screenshot_pending = app.capture.ui_screen_index < app.capture.ui_screens.size();
    const bool capturing = thumbnails_pending || screenshot_pending;
    const auto fail_capture = [&](const char* reason) {
        FailMenuCapture(app, window, worldgenPreview, previewState, reason);
    };
    if (previewState.capture_failed)
        return false;
    if (capturing) {
        const bool armed = previewState.capture_gate.arm(
            menu_now,
            thumbnails_pending ? static_cast<std::size_t>(app.capture.ui_thumbs_index)
                               : app.capture.ui_screen_index,
            thumbnails_pending);
        if (armed) {
            app.capture.ui_screenshot_settle = 0;
            app.capture.ui_preview_settle_after_ready = 0;
            app.capture.ui_thumbs_settle = 0;
            previewState.preview_wait_armed = false;
            previewState.capture_key.reset();
        }
        if (previewState.capture_gate.decide(menu_now, false, false, false) ==
            MenuCaptureGate::Result::TimedOut) {
            fail_capture("45-second deadline expired before a settled image");
            return false;
        }
        glfwGetFramebufferSize(window, &capture_width, &capture_height);
        const auto document = uiManager ? uiManager->document_generation() : 0;
        if (armed || capture_width != previewState.capture_width ||
            capture_height != previewState.capture_height ||
            document != previewState.capture_document) {
            app.capture.ui_screenshot_settle = 0;
            app.capture.ui_preview_settle_after_ready = 0;
            app.capture.ui_thumbs_settle = 0;
            previewState.capture_width = capture_width;
            previewState.capture_height = capture_height;
            previewState.capture_document = document;
            // LoadDocument appends the sole application document after closing
            // the previous one (debugger documents remain underneath). Inspect
            // that last document in O(1), without scanning the context per item.
            auto* context = uiManager ? uiManager->GetContext() : nullptr;
            const int documents = context ? context->GetNumDocuments() : 0;
            auto* requested = !thumbnails_pending && documents > 0
                                  ? context->GetDocument(documents - 1)
                                  : nullptr;
            previewState.capture_document_ready =
                requested && requested->IsVisible() &&
                requested->GetSourceURL().ends_with(
                    "/" + app.capture.ui_screens[app.capture.ui_screen_index] + ".rml");
        }
    } else {
        previewState.capture_gate.reset();
        previewState.preview_wait_armed = false;
    }
    return true;
}

bool MenuCaptureContentReady(const MenuSceneState& previewState,
                             const ClientAppContext& app,
                             const Luminumbra::Client::WorldgenPreview* worldgenPreview,
                             bool expects_preview,
                             bool scenic,
                             bool previewActive) {
    const bool content_ready =
        previewState.capture_document_ready &&
        (!app.menu.menu_backdrop_active || scenic || previewActive) &&
        (!expects_preview ||
         (previewActive && worldgenPreview && worldgenPreview->world_ready() &&
          worldgenPreview->candidate_ready() && !worldgenPreview->last_build_failed() &&
          !previewState.candidate_source_failed));
    return content_ready;
}
} // namespace

// Scene samples are retained independently of the per-frame UI composition.
void RenderMenuScreens(ClientAppContext& app,
                       GLFWwindow* window,
                       float deltaTime,
                       const std::string& root_path_str,
                       Luminumbra::Rendering::Camera* camera,
                       Luminumbra::world::GameSession* gameSession,
                       Luminumbra::Rendering::RenderPipeline& renderPipeline,
                       Luminumbra::Client::Rml_UIManager* uiManager,
                       Luminumbra::Client::WorldgenPreview* worldgenPreview,
                       MenuSceneState& previewState,
                       const UiInputRouter& inputRouter) {
    const bool previewMouseAllowed = previewState.UpdateInputOwnership(inputRouter);
    const auto menu_now = std::chrono::steady_clock::now();
    const float menu_delta =
        previewState.last_menu_frame.time_since_epoch().count() == 0
            ? deltaTime
            : std::chrono::duration<float>(menu_now - previewState.last_menu_frame).count();
    previewState.last_menu_frame = menu_now;
    const bool thumbnails_pending = MenuThumbnailsPending(app);
    int capture_width = 0, capture_height = 0;
    if (!PrepareMenuCapture(app,
                            window,
                            uiManager,
                            worldgenPreview,
                            previewState,
                            menu_now,
                            capture_width,
                            capture_height))
        return;
    const auto fail_capture = [&](const char* reason) {
        FailMenuCapture(app, window, worldgenPreview, previewState, reason);
    };
    if (app.capture.ui_thumbs > 0 && app.menu.menu_backdrop_active && camera && gameSession &&
        gameSession->GetWorldSystem() && app.capture.ui_thumbs_index < app.capture.ui_thumbs) {
        if (capture_width <= 0 || capture_height <= 0) {
            app.capture.ui_thumbs_settle = 0;
            return;
        }
        // capture N clean landscape thumbnails (no UI) at varied yaw + time-of-day.
        const float yaw = 40.0f + static_cast<float>(app.capture.ui_thumbs_index) * 47.0f;
        const float tod = 0.16f + 0.025f * static_cast<float>(app.capture.ui_thumbs_index % 5);
        camera->Yaw = yaw;
        camera->Pitch = -14.0f; // look down to frame the lit landscape, not just sky
        camera->updateCameraVectors();
        const MenuSceneKey thumbnail_key{
            .width = capture_width,
            .height = capture_height,
            .document = previewState.capture_document,
            .preview = 0,
            .preview_world = 0,
            .renderer_target = renderPipeline.resize_generation(),
            .camera = previewState.camera_revision.observe(MenuCameraState(*camera)),
            .settings = {.render_scale = renderPipeline.render_scale(),
                         .exposure_override = renderPipeline.get_exposure_override(),
                         .cloud_quality = renderPipeline.get_cloud_quality(),
                         .ssao_quality = renderPipeline.get_ssao_quality(),
                         .volumetric_quality = renderPipeline.volumetric_quality(),
                         .taau = renderPipeline.taau_enabled(),
                         .sky_lut_gpu = renderPipeline.sky_lut_gpu_enabled(),
                         .metered_exposure = renderPipeline.auto_exposure_metered(),
                         .graphics = renderPipeline.graphics_runtime_config()},
            .preview_active = false,
            .wireframe = app.overlay.wireframe_mode};
        if (previewState.capture_key != thumbnail_key) {
            app.capture.ui_thumbs_settle = 0;
            previewState.capture_key = thumbnail_key;
        }
        renderPipeline.set_time_of_day(tod);
        const Attribution::PhaseScope scene_phase(Attribution::Phase::MenuScene);
        renderPipeline.render_frame(gameSession->GetRegistry(),
                                    *gameSession->GetWorldSystem(),
                                    *camera,
                                    deltaTime,
                                    app.overlay.wireframe_mode);
        if (app.capture.ui_thumbs_settle < 28) {
            ++app.capture.ui_thumbs_settle;
        } else {
            const auto decision = previewState.capture_gate.decide(
                std::chrono::steady_clock::now(), true, true, true);
            if (decision == MenuCaptureGate::Result::TimedOut) {
                fail_capture("45-second thumbnail deadline expired");
                return;
            }
            if (decision == MenuCaptureGate::Result::Ready) {
                const std::filesystem::path shot =
                    app.capture.ui_thumbs_dir /
                    ("thumb_" + std::to_string(app.capture.ui_thumbs_index) + ".ppm");
                if (!WritePixelBufferPpm(shot, capture_width, capture_height)) {
                    fail_capture("thumbnail output failed");
                    return;
                }
                LUMINUMBRA_CORE_INFO(
                    "UI thumb written -> {} ({}x{})", shot.string(), capture_width, capture_height);
            } else {
                return;
            }
            ++app.capture.ui_thumbs_index;
            app.capture.ui_thumbs_settle = 0;
            if (app.capture.ui_thumbs_index >= app.capture.ui_thumbs &&
                app.capture.ui_screen_index >= app.capture.ui_screens.size())
                glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    } else { // Main Menu, etc.
        //  is the create-world live preview active this frame?
        // (world_creation.rml loaded + #preview_pane present + sized). When it
        // is, the candidate world IS the backdrop — we render ONE world (the
        // candidate, full-screen) and SUPPRESS the separate menu-vista backdrop,
        // so the create screen pays for a single render with no FBO/resize churn.
        Luminumbra::Client::Rml_UIManager::PreviewState pv;
        bool previewActive = false;
        if (uiManager && worldgenPreview) {
            pv = uiManager->GetWorldCreationPreviewState();
            previewActive = pv.active; // full-screen diorama; no bounded pane rect
        }

        bool interactive = false;
        //  LIVE WORLD-PREVIEW DIORAMA. When the create-world
        // screen is up, feed the candidate params/weather/tod and render the
        // candidate world FULL-SCREEN to the backbuffer (the "framed hole" the
        // create panel frames). The menu UI bodies are transparent so the world
        // shows through; the #preview_pane frames the primary viewing area.
        // Mouse drag/scroll over the pane orbits/zooms the turntable camera.
        if (uiManager && worldgenPreview) {
            if (previewActive) {
                worldgenPreview->set_active(true);

                // Re-derive the candidate world ONLY when the form changed.
                // BuildCustomPreset diffs the form against the base preset;
                // we hand the resolved JSON + data root to the in-memory
                // loader seam so biome/structure tables resolve correctly.
                std::string sig = pv.worldType;
                for (const auto& p : pv.params) {
                    sig += '|';
                    sig += p.path;
                    sig += '=';
                    sig += p.value;
                }
                const int seedVal = 4242;
                if (sig != previewState.lastSig) {
                    previewState.candidate_source_failed = true;
                    previewState.lastSig = sig;
                    ++previewState.preview_revision;
                    // prove a knob/param drag actually drives a
                    // diorama rebuild — one line per resolved candidate sig so
                    // a dragged knob is visibly firing the host rebuild branch.
                    LUMINUMBRA_CORE_INFO("Worldgen preview rebuild: sig={}", sig);
                    try {
                        const std::filesystem::path base_path =
                            std::filesystem::path(root_path_str) / "worlds" / "atlas" / "presets" /
                            (pv.worldType + ".json");
                        nlohmann::json base;
                        if (ReadPreviewPreset(base_path, base)) {
                            //  resolve the live diorama through
                            // the engine KnobLayer when the form carries knobs
                            // (knobs -> response curves + overlay overrides), so
                            // sliding a knob regenerates the preview.
                            bool hasKnobs = false;
                            for (const auto& p : pv.params)
                                if (p.type == "knob" || p.path.rfind("knob.", 0) == 0) {
                                    hasKnobs = true;
                                    break;
                                }
                            const nlohmann::json resolved =
                                hasKnobs
                                    ? Luminumbra::Client::BuildKnobResolvedPreset(base, pv.params)
                                          .json
                                    : Luminumbra::Client::BuildCustomPreset(base, pv.params).json;
                            const std::filesystem::path data_root =
                                std::filesystem::path(root_path_str) / "data";
                            previewState.candidate_source_failed =
                                !worldgenPreview->set_candidate(resolved, data_root, seedVal);
                        }
                    } catch (const std::exception& e) {
                        LUMINUMBRA_CORE_WARN("Preview candidate build failed: {}", e.what());
                    }
                }

                // Live look controls.
                using PW = Luminumbra::Client::WorldgenPreview::Weather;
                // Headless capture (--preview-weather) can force a weather chip so the
                // diorama spawns precipitation even though no UI pill was clicked; falls
                // back to the DOM-selected pill for the interactive create screen.
                const std::string weatherSel =
                    (app.capture.ui_preview_live && !app.capture.ui_preview_weather.empty())
                        ? app.capture.ui_preview_weather
                        : pv.weather;
                PW w = PW::Clear;
                if (weatherSel == "rain")
                    w = PW::Rain;
                else if (weatherSel == "snow")
                    w = PW::Snow;
                else if (weatherSel == "fog")
                    w = PW::Fog;
                else if (weatherSel == "storm")
                    w = PW::Storm;
                if (previewState.last_weather != weatherSel || previewState.last_tod != pv.tod) {
                    previewState.last_weather = weatherSel;
                    previewState.last_tod = pv.tod;
                    ++previewState.preview_revision;
                }
                worldgenPreview->set_weather(w);
                worldgenPreview->set_time_of_day(pv.tod);
                if (uiManager->ConsumeWorldCreationResetView()) {
                    worldgenPreview->reset_view();
                    ++previewState.preview_revision;
                    interactive = true;
                }

                // Orbit/zoom when the cursor is over the WORLD backdrop, i.e.
                // not over the create panel or a control. RmlUi reports the
                // hovered element; the bare backdrop is the document body
                // (id "world_creation"), so a null/body hover == over the world.
                double cx = 0.0, cy = 0.0;
                if (previewMouseAllowed)
                    glfwGetCursorPos(window, &cx, &cy);
                const bool lmb = previewMouseAllowed &&
                                 glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                Rml::Element* hover =
                    uiManager->GetContext() ? uiManager->GetContext()->GetHoverElement() : nullptr;
                const bool overWorld = previewMouseAllowed &&
                                       ((hover == nullptr) || (hover->GetId() == "world_creation"));
                if (lmb && overWorld && !previewState.dragging) {
                    previewState.dragging = true;
                    previewState.lastCursorX = cx;
                    previewState.lastCursorY = cy;
                } else if (!lmb) {
                    previewState.dragging = false;
                }
                if (previewState.dragging) {
                    interactive = true;
                    ++previewState.preview_revision;
                    const float dx = static_cast<float>(cx - previewState.lastCursorX);
                    const float dy = static_cast<float>(cy - previewState.lastCursorY);
                    previewState.lastCursorX = cx;
                    previewState.lastCursorY = cy;
                    // Drag right -> spin right; drag down -> tilt down.
                    worldgenPreview->orbit(dx * 0.35f, -dy * 0.35f);
                }

                // Scroll wheel over the world zooms the diorama in/out. The
                // menu scroll callback accrues the delta; consume + reset it
                // here (only applied when the cursor is over the world).
                if (overWorld && app.menu.menu_scroll_accum != 0.0) {
                    worldgenPreview->zoom(static_cast<float>(app.menu.menu_scroll_accum));
                    interactive = true;
                    ++previewState.preview_revision;
                }
                app.menu.menu_scroll_accum = 0.0;

                // Debounced rebuild, then render the candidate world FULL-SCREEN
                // to the backbuffer (no offscreen FBO, no per-frame pipeline
                // resize). render_frame clears + fills the backbuffer; the UI
                // pass draws the frosted frame/vignette on top.
                worldgenPreview->tick(menu_delta);
            } else {
                worldgenPreview->set_active(false);
                worldgenPreview->clear_precipitation(renderPipeline); // no lingering preview rain
                previewState.lastSig.clear();
                previewState.dragging = false;
                app.menu.menu_scroll_accum = 0.0; // drop stale wheel deltas from other menus
            }
        }

        const bool scenic = !previewActive && app.menu.menu_backdrop_active && camera &&
                            gameSession && gameSession->GetWorldSystem();
        bool scene_rendered = false;
        if (scenic || previewActive) {
            const Attribution::PhaseScope scene_phase(Attribution::Phase::MenuScene);
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            const bool has_image = previewState.scene_image.ensure_scene_image(width, height);
            if (!has_image)
                previewState.schedule.invalidate();
            const auto camera_state = [&] {
                return MenuCameraState(*camera);
            };
            const MenuSceneKey key{
                .width = width,
                .height = height,
                .document = uiManager ? uiManager->document_generation() : 0,
                .preview = previewState.preview_revision,
                .preview_world = previewActive ? worldgenPreview->rebuild_generation() : 0,
                .renderer_target = renderPipeline.resize_generation(),
                .camera = scenic ? previewState.camera_revision.observe(camera_state()) : 0,
                .settings = {.render_scale = renderPipeline.render_scale(),
                             .exposure_override = renderPipeline.get_exposure_override(),
                             .cloud_quality = renderPipeline.get_cloud_quality(),
                             .ssao_quality = renderPipeline.get_ssao_quality(),
                             .volumetric_quality = renderPipeline.volumetric_quality(),
                             .taau = renderPipeline.taau_enabled(),
                             .sky_lut_gpu = renderPipeline.sky_lut_gpu_enabled(),
                             .metered_exposure = renderPipeline.auto_exposure_metered(),
                             .graphics = renderPipeline.graphics_runtime_config()},
                .preview_active = previewActive,
                .wireframe = app.overlay.wireframe_mode};
            if (app.capture.ui_screen_index < app.capture.ui_screens.size()) {
                if (!previewState.capture_key || key != *previewState.capture_key) {
                    app.capture.ui_screenshot_settle = 0;
                    app.capture.ui_preview_settle_after_ready = 0;
                    previewState.capture_key = key;
                }
            }
            // The camera phase advances with all elapsed menu time, including
            // frames which only replay the cached image. Dusk remains at 0.24.
            if (scenic) {
                app.menu.menu_backdrop_yaw += menu_delta;
                camera->Yaw = 95.0f + 6.0f * std::sin(app.menu.menu_backdrop_yaw * 0.12f);
                camera->updateCameraVectors();
                previewState.camera_revision.animated(camera_state());
            }
            const auto now =
                std::chrono::duration_cast<MenuSceneSchedule::Time>(menu_now.time_since_epoch());
            const bool refresh = previewState.schedule.update(now, key, interactive);
            const bool drawable = width > 0 && height > 0;
            if (drawable && (refresh || !has_image)) {
                const float render_delta = previewState.schedule.render_delta_seconds();
                bool rendered = false;
                if (previewActive) {
                    rendered = worldgenPreview->render_to_backbuffer(renderPipeline, render_delta);
                } else {
                    renderPipeline.set_time_of_day(0.24f);
                    renderPipeline.render_frame(gameSession->GetRegistry(),
                                                *gameSession->GetWorldSystem(),
                                                *camera,
                                                render_delta,
                                                app.overlay.wireframe_mode);
                    rendered = true;
                }
                if (rendered) {
                    scene_rendered = true;
                    // Capture BEFORE RmlUi so hover, text, and animations remain live.
                    if (has_image)
                        previewState.scene_image.copy_scene_image(true);
                    previewState.schedule.rendered();
                    if (!has_image)
                        previewState.schedule.invalidate();
                } else if (has_image && previewState.schedule.valid()) {
                    previewState.scene_image.copy_scene_image(false);
                }
            } else if (previewState.schedule.valid()) {
                previewState.scene_image.copy_scene_image(false);
            }
        } else {
            previewState.schedule.invalidate();
            if (previewState.capture_key) {
                app.capture.ui_screenshot_settle = 0;
                app.capture.ui_preview_settle_after_ready = 0;
                previewState.capture_key.reset();
            }
        }

        if (uiManager) {
            uiManager->Render();
        }
        // --ui-screenshot: settle layout/fonts/textures, capture the back buffer (now
        // holding the UI over the menu backdrop), then advance to the next batched screen
        // (one window session captures them all). Mirrors --scene-config.
        if (!thumbnails_pending && !app.capture.ui_screens.empty() &&
            app.capture.ui_screen_index < app.capture.ui_screens.size()) {
            // HEADLESS PREVIEW-DIORAMA CAPTURE: for world_creation under --preview-live,
            // the live diorama builds on a background worker, so the fixed 30-sample settle
            // would capture a black backdrop before world_ready. Instead, wait (bounded
            // by a wall-clock timeout so we never hang) for the candidate world to build +
            // a short post-ready settle (far-LOD/foliage/precipitation drawn), THEN
            // capture.
            auto& s_preview_wait_start = previewState.preview_wait_start;
            auto& s_preview_wait_armed = previewState.preview_wait_armed;
            const bool preview_live_screen =
                app.capture.ui_preview_live &&
                app.capture.ui_screens[app.capture.ui_screen_index] == "world_creation";
            bool ready_to_capture = false;
            if (preview_live_screen) {
                if (!s_preview_wait_armed) {
                    s_preview_wait_armed = true;
                    s_preview_wait_start = std::chrono::steady_clock::now();
                    app.capture.ui_preview_settle_after_ready = 0;
                }
                const bool world_ready = worldgenPreview && worldgenPreview->world_ready();
                if (!world_ready || !previewActive)
                    app.capture.ui_preview_settle_after_ready = 0;
                else if (scene_rendered)
                    ++app.capture.ui_preview_settle_after_ready;
                const double waited_s = std::chrono::duration<double>(
                                            std::chrono::steady_clock::now() - s_preview_wait_start)
                                            .count();
                constexpr double kPreviewTimeoutSeconds =
                    45.0; // hard bound -> never hangs headlessly
                constexpr int kPreviewSettleAfterReady =
                    40; // successful post-ready scene renders for far-LOD/foliage/particles
                const bool settled = world_ready && app.capture.ui_preview_settle_after_ready >=
                                                        kPreviewSettleAfterReady;
                const bool timed_out = waited_s >= kPreviewTimeoutSeconds;
                ready_to_capture = settled || timed_out;
                if (ready_to_capture) {
                    // One-line, self-diagnosing readiness report so a black/timed-out
                    // capture tells us WHICH failure happened (no rebuild signal vs build
                    // failed vs world built but render black) in a single run.
                    LUMINUMBRA_CORE_INFO(
                        "Preview-live capture gate: world_ready={} settle_after_ready={} "
                        "waited={:.1f}s timed_out={} "
                        "build_failed={} last_error='{}'",
                        world_ready,
                        app.capture.ui_preview_settle_after_ready,
                        waited_s,
                        timed_out,
                        worldgenPreview ? worldgenPreview->last_build_failed() : true,
                        worldgenPreview ? worldgenPreview->last_error()
                                        : std::string("<null preview>"));
                }
            } else if (app.capture.ui_screenshot_settle < 30) {
                // UI-only screens still settle layout/fonts every frame. A scenic or
                // preview screen needs actual scene updates; cached replays do no work.
                if (scene_rendered || !(scenic || previewActive))
                    ++app.capture.ui_screenshot_settle;
            } else {
                ready_to_capture = true;
            }
            // Neither a missing preview nor an old world retained after refusal
            // can satisfy this request. Recovery may settle within the original
            // deadline; readiness loss never extends it or admits a cached sample.
            const bool expects_preview =
                app.capture.ui_screens[app.capture.ui_screen_index] == "world_creation";
            const bool content_ready = MenuCaptureContentReady(
                previewState, app, worldgenPreview, expects_preview, scenic, previewActive);
            const bool drawable = capture_width > 0 && capture_height > 0;
            if (!content_ready || !drawable) {
                app.capture.ui_screenshot_settle = 0;
                app.capture.ui_preview_settle_after_ready = 0;
                ready_to_capture = false;
            }
            const auto decision = previewState.capture_gate.decide(
                std::chrono::steady_clock::now(), ready_to_capture, drawable, content_ready);
            if (decision == MenuCaptureGate::Result::TimedOut) {
                fail_capture("45-second screenshot deadline expired before a settled image");
                return;
            }
            ready_to_capture = decision == MenuCaptureGate::Result::Ready;
            if (ready_to_capture) {
                s_preview_wait_armed = false; // re-arm for the next screen (batched runs)
                const std::filesystem::path shot =
                    app.capture.ui_screenshot_dir /
                    ("ui-" + app.capture.ui_screens[app.capture.ui_screen_index] + ".ppm");
                if (!WriteUiScreenshot(shot,
                                       window,
                                       uiManager,
                                       app.capture.ui_screens[app.capture.ui_screen_index])) {
                    fail_capture("screenshot output failed");
                    return;
                }
                // Advance to the next screen, or finish.
                ++app.capture.ui_screen_index;
                if (app.capture.ui_screen_index < app.capture.ui_screens.size()) {
                    app.capture.ui_screenshot_screen =
                        app.capture.ui_screens[app.capture.ui_screen_index];
                    app.capture.ui_screenshot_settle = 0;
                    app.capture.ui_preview_settle_after_ready = 0;
                    previewState.capture_key.reset();
                    if (uiManager)
                        uiManager->RequestLoadDocument(
                            app.capture.ui_screens[app.capture.ui_screen_index] + ".rml");
                } else {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            }
        }
    }
}

} // namespace Luminumbra::Client::App
