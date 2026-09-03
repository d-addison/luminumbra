#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "app/MenuScreens.h"

#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "nlohmann/json.hpp"
#include "rendering/Camera.h"
#include "rendering/RenderPipeline.h"
#include "ui/Rml_UIManager.h"
#include "world/WorldgenOverride.h"
#include "world/WorldgenPreview.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Luminumbra::Client::App {

using namespace Luminumbra::Client::ScenarioHarness;

// The body below is the frame loop's menu branch, moved verbatim; only the
// former globals now come in through the context and parameters.
void RenderMenuScreens(ClientAppContext& app,
                       GLFWwindow* window,
                       float deltaTime,
                       const std::string& root_path_str,
                       Luminumbra::Rendering::Camera* camera,
                       Luminumbra::world::GameSession* gameSession,
                       Luminumbra::Rendering::RenderPipeline& renderPipeline,
                       Luminumbra::Client::Rml_UIManager* uiManager,
                       Luminumbra::Client::WorldgenPreview* worldgenPreview,
                       MenuPreviewState& previewState) {
    if (app.capture.ui_thumbs > 0 && app.menu.menu_backdrop_active && camera && gameSession &&
        gameSession->GetWorldSystem() && app.capture.ui_thumbs_index < app.capture.ui_thumbs) {
        // capture N clean landscape thumbnails (no UI) at varied yaw + time-of-day.
        const float yaw = 40.0f + static_cast<float>(app.capture.ui_thumbs_index) * 47.0f;
        const float tod = 0.16f + 0.025f * static_cast<float>(app.capture.ui_thumbs_index % 5);
        camera->Yaw = yaw;
        camera->Pitch = -14.0f; // look down to frame the lit landscape, not just sky
        camera->updateCameraVectors();
        renderPipeline.set_time_of_day(tod);
        renderPipeline.render_frame(gameSession->GetRegistry(),
                                    *gameSession->GetWorldSystem(),
                                    *camera,
                                    deltaTime,
                                    app.overlay.wireframe_mode);
        if (app.capture.ui_thumbs_settle < 28) {
            ++app.capture.ui_thumbs_settle;
        } else {
            int vw = 0, vh = 0;
            glfwGetFramebufferSize(window, &vw, &vh);
            if (vw > 0 && vh > 0) {
                std::vector<unsigned char> px(static_cast<std::size_t>(vw) *
                                              static_cast<std::size_t>(vh) * 3u);
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                const std::filesystem::path shot =
                    app.capture.ui_thumbs_dir /
                    ("thumb_" + std::to_string(app.capture.ui_thumbs_index) + ".ppm");
                WritePixelBufferPpm(shot, vw, vh, px);
                LUMINUMBRA_CORE_INFO("UI thumb written -> {} ({}x{})", shot.string(), vw, vh);
            }
            ++app.capture.ui_thumbs_index;
            app.capture.ui_thumbs_settle = 0;
            if (app.capture.ui_thumbs_index >= app.capture.ui_thumbs)
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

        // render the live scenic world behind the menu, with a slow auto-orbit, at
        // golden hour. The menu UI bodies are transparent (game_theme.rcss) so the world
        // shows through. render_frame draws to the back buffer BEFORE the UI pass.
        // Suppressed while the create-world preview owns the world render.
        if (!previewActive && app.menu.menu_backdrop_active && camera && gameSession &&
            gameSession->GetWorldSystem()) {
            // Gentle yaw oscillation around the lit-valley heading (95°) for a living, slow
            // parallax that never rotates away into dark/back-lit terrain.
            app.menu.menu_backdrop_yaw += deltaTime; // phase accumulator (seconds)
            camera->Yaw = 95.0f + 6.0f * std::sin(app.menu.menu_backdrop_yaw * 0.12f);
            camera->updateCameraVectors();
            renderPipeline.set_time_of_day(0.24f); // dusk golden hour (matches dusk.json)
            renderPipeline.render_frame(gameSession->GetRegistry(),
                                        *gameSession->GetWorldSystem(),
                                        *camera,
                                        deltaTime,
                                        app.overlay.wireframe_mode);
        }

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
                int seedVal = 4242;
                if (sig != previewState.lastSig) {
                    previewState.lastSig = sig;
                    // prove a knob/param drag actually drives a
                    // diorama rebuild — one line per resolved candidate sig so
                    // a dragged knob is visibly firing the host rebuild branch.
                    LUMINUMBRA_CORE_INFO("Worldgen preview rebuild: sig={}", sig);
                    try {
                        const std::filesystem::path base_path =
                            std::filesystem::path(root_path_str) / "worlds" / "atlas" / "presets" /
                            (pv.worldType + ".json");
                        std::ifstream in(base_path);
                        if (in) {
                            nlohmann::json base;
                            in >> base;
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
                            nlohmann::json resolved =
                                hasKnobs
                                    ? Luminumbra::Client::BuildKnobResolvedPreset(base, pv.params)
                                          .json
                                    : Luminumbra::Client::BuildCustomPreset(base, pv.params).json;
                            const std::filesystem::path data_root =
                                std::filesystem::path(root_path_str) / "data";
                            worldgenPreview->set_candidate(resolved, data_root, seedVal);
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
                worldgenPreview->set_weather(w);
                worldgenPreview->set_time_of_day(pv.tod);
                if (uiManager->ConsumeWorldCreationResetView())
                    worldgenPreview->reset_view();

                // Orbit/zoom when the cursor is over the WORLD backdrop, i.e.
                // not over the create panel or a control. RmlUi reports the
                // hovered element; the bare backdrop is the document body
                // (id "world_creation"), so a null/body hover == over the world.
                double cx = 0.0, cy = 0.0;
                glfwGetCursorPos(window, &cx, &cy);
                const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                Rml::Element* hover =
                    uiManager->GetContext() ? uiManager->GetContext()->GetHoverElement() : nullptr;
                const bool overWorld = (hover == nullptr) || (hover->GetId() == "world_creation");
                if (lmb && overWorld && !previewState.dragging) {
                    previewState.dragging = true;
                    previewState.lastCursorX = cx;
                    previewState.lastCursorY = cy;
                } else if (!lmb) {
                    previewState.dragging = false;
                }
                if (previewState.dragging) {
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
                }
                app.menu.menu_scroll_accum = 0.0;

                // Debounced rebuild, then render the candidate world FULL-SCREEN
                // to the backbuffer (no offscreen FBO, no per-frame pipeline
                // resize). render_frame clears + fills the backbuffer; the UI
                // pass draws the frosted frame/vignette on top.
                worldgenPreview->tick(deltaTime);
                worldgenPreview->render_to_backbuffer(renderPipeline, deltaTime);
            } else {
                worldgenPreview->set_active(false);
                worldgenPreview->clear_precipitation(renderPipeline); // no lingering preview rain
                previewState.lastSig.clear();
                previewState.dragging = false;
                app.menu.menu_scroll_accum = 0.0; // drop stale wheel deltas from other menus
            }
        }

        if (uiManager) {
            uiManager->Render();
        }
        // --ui-screenshot: settle layout/fonts/textures, capture the back buffer (now
        // holding the UI over the menu backdrop), then advance to the next batched screen
        // (one window session captures them all). Mirrors --scene-config.
        if (!app.capture.ui_screens.empty() &&
            app.capture.ui_screen_index < app.capture.ui_screens.size()) {
            // HEADLESS PREVIEW-DIORAMA CAPTURE: for world_creation under --preview-live,
            // the live diorama builds on a background worker, so the fixed 30-frame settle
            // would capture a black backdrop before world_ready. Instead, wait (bounded
            // by a wall-clock timeout so we never hang) for the candidate world to build +
            // a short post-ready settle (far-LOD/foliage/precipitation drawn), THEN
            // capture.
            static std::chrono::steady_clock::time_point s_preview_wait_start{};
            static bool s_preview_wait_armed = false;
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
                if (world_ready)
                    ++app.capture.ui_preview_settle_after_ready;
                const double waited_s = std::chrono::duration<double>(
                                            std::chrono::steady_clock::now() - s_preview_wait_start)
                                            .count();
                constexpr double kPreviewTimeoutSeconds =
                    45.0; // hard bound -> never hangs headlessly
                constexpr int kPreviewSettleAfterReady =
                    40; // post-ready frames for far-LOD/foliage/particles
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
                ++app.capture.ui_screenshot_settle;
            } else {
                ready_to_capture = true;
            }
            if (ready_to_capture) {
                s_preview_wait_armed = false; // re-arm for the next screen (batched runs)
                int vw = 0, vh = 0;
                glfwGetFramebufferSize(window, &vw, &vh);
                if (vw > 0 && vh > 0) {
                    std::vector<unsigned char> px(static_cast<std::size_t>(vw) *
                                                  static_cast<std::size_t>(vh) * 3u);
                    glReadBuffer(GL_BACK);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                    const std::filesystem::path shot =
                        app.capture.ui_screenshot_dir /
                        ("ui-" + app.capture.ui_screens[app.capture.ui_screen_index] + ".ppm");
                    WritePixelBufferPpm(shot, vw, vh, px);
                    LUMINUMBRA_CORE_INFO(
                        "UI screenshot written -> {} ({}x{})", shot.string(), vw, vh);
                }
                // Advance to the next screen, or finish.
                ++app.capture.ui_screen_index;
                if (app.capture.ui_screen_index < app.capture.ui_screens.size()) {
                    app.capture.ui_screenshot_screen =
                        app.capture.ui_screens[app.capture.ui_screen_index];
                    app.capture.ui_screenshot_settle = 0;
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
