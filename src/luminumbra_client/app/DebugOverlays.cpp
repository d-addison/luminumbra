#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "app/DebugOverlays.h"

#include "audio/IAudioManager.h"
#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/game/CodexView.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "player/PlayerController.h"
#include "rendering/Camera.h"
#include "rendering/ExposureModel.h"
#include "rendering/RenderPipeline.h"
#include "rendering/Shader.h"
#include "ui/Rml_UIManager.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

namespace Luminumbra::Client::App {

using namespace Luminumbra::Client::ScenarioHarness;

namespace {

//  map the sun's elevation (radians, >0 above horizon) to a photographic
// scene luminance [0,1] for photo scoring. Night (sun below horizon) is dark (~0.06);
// low/golden-hour sun lands near the ideal (~0.5-0.7); midday is bright but not blown
// (~0.85). CLIENT render-derived feedback ONLY — never sim / world_hash — so libm
// (std::sin/std::pow) is fine here (unlike the pure photo headers, which stay libm-free).
inline float SceneLuminanceFromSunElevation(float sun_elev_rad) {
    const float e = std::sin(sun_elev_rad); // [-1,1], fraction of the way above horizon
    if (e <= 0.0f) {
        // Twilight → night: a small floor that dims toward midnight (e == -1).
        return luminumbra::game::Clamp01(0.06f + 0.10f * (1.0f + e));
    }
    return luminumbra::game::Clamp01(0.15f + 0.70f * std::pow(e, 0.3f));
}

// feature: gather the in-frustum creature subjects for a photo-mode
// CAPTURE. STRICTLY a read-only observer — it takes a CONST registry + CONST camera,
// projects each creature's world position into NDC via the camera's view*proj, and
// derives a deterministic species/luminance proxy (: no luminance component to
// read, so a constant scene-luminance + the predator-role species proxy; no new sim
// component). It mutates NOTHING, so it cannot perturb world_hash (the sim-isolation
// gate test pins this on the pure path).
std::vector<luminumbra::game::PhotoSubjectView>
GatherPhotoSubjects(const entt::registry& reg,
                    const Luminumbra::Rendering::Camera& camera,
                    int width,
                    int height,
                    float scene_luminance = 0.6f) {
    std::vector<luminumbra::game::PhotoSubjectView> views;
    if (width <= 0 || height <= 0)
        return views;

    const glm::mat4 proj = glm::perspective(glm::radians(camera.Zoom),
                                            static_cast<float>(width) / static_cast<float>(height),
                                            camera.GetNearPlane(),
                                            camera.GetFarPlane());
    const glm::mat4 view_proj = proj * camera.GetViewMatrix();

    auto cr_view = reg.view<const Luminumbra::Components::CreatureComponent,
                            const Luminumbra::Components::TransformComponent>();
    for (const entt::entity e : cr_view) {
        const auto& tf = cr_view.get<const Luminumbra::Components::TransformComponent>(e);
        const auto& cr = cr_view.get<const Luminumbra::Components::CreatureComponent>(e);

        const glm::vec3 world(tf.position.x, tf.position.y, tf.position.z);
        const glm::vec4 clip = view_proj * glm::vec4(world, 1.0f);

        luminumbra::game::PhotoSubjectView pv;
        pv.in_frustum = (clip.w > 0.0f);
        if (pv.in_frustum) {
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            pv.in_frustum = (ndc.x >= -1.0f && ndc.x <= 1.0f && ndc.y >= -1.0f && ndc.y <= 1.0f);
            pv.ndc_x = ndc.x;
            pv.ndc_y = ndc.y;
        }
        const glm::vec3 cam_to = world - camera.Position;
        const float dist = glm::length(cam_to);
        pv.distance_m = dist > 0.01f ? dist : 0.01f;
        pv.size_m = 1.0f; // ~creature footprint
        // Apparent footprint falls off with distance (a far subject fills less frame).
        pv.size = luminumbra::game::Clamp01(pv.size_m / (pv.distance_m * 0.5f + 1.0f));
        //  scene luminance is now driven by the sun (time-of-day), passed in
        // from the render pipeline's sun elevation, so golden hour rewards and night
        // punishes. No per-creature luminance component exists, so all subjects share the
        // scene's light this frame (a far better proxy than the old hardcoded 0.6).
        pv.light = scene_luminance;
        // Real per-creature species identity (set at spawn from the archetype) keys the
        // codex; fall back to the predator/prey role proxy only for unspecified (0)
        // creatures so the codex can fill with actual species rather than two buckets.
        pv.species_id =
            cr.species_id != 0 ? static_cast<int>(cr.species_id) : (cr.is_predator ? 1 : 2);
        //  carry the subject's current behaviour so the principal subject's
        // action reaches the capture's ObservationMetadata (behaviour objectives).
        pv.subject_action = cr.last_action;
        views.push_back(pv);
    }
    return views;
}

// Codex toggle/populate, the HUD<->viewfinder swap, the objective tracker,
// and the farming HUD (verbatim; the outer interactive-play guard lives in
// UpdatePhotoModeAndHud).
void UpdateCodexObjectivesFarmHud(ClientAppContext& app,
                                  Luminumbra::Client::PlayerController* playerController,
                                  Luminumbra::Rendering::Camera* camera,
                                  Luminumbra::Client::Rml_UIManager* uiManager,
                                  Luminumbra::Client::IAudioManager* audioManager,
                                  Luminumbra::world::GameSession* gameSession) {
    // Codex browse overlay (client-only). Its toggle takes precedence
    // over the HUD/viewfinder swap below and cannot open while the
    // viewfinder is active. Closing it resyncs the hud-swap state.
    if (playerController->consume_codex_toggle() && !app.hud.photoMode.active) {
        app.hud.codexOpen = !app.hud.codexOpen;
        if (audioManager)
            audioManager->PlayOneShot2D(app.hud.codexOpen ? "ui_codex_open" : "ui_codex_close");
        if (uiManager) {
            uiManager->RequestLoadDocument(app.hud.codexOpen ? "codex.rml" : "hud.rml");
            app.hud.photoModeUiShown = false;
            app.hud.codexSig.clear(); // force a repopulate on next open
        }
    }

    //  swap the in-game overlay between the HUD and the photo-mode viewfinder
    // when photo mode toggles (the capture loop + lens nudges below already
    // exist).
    if (!app.hud.codexOpen && uiManager) {
        if (app.hud.photoMode.active && !app.hud.photoModeUiShown) {
            uiManager->RequestLoadDocument("photo_mode.rml");
            app.hud.photoModeUiShown = true;
        } else if (!app.hud.photoMode.active && app.hud.photoModeUiShown) {
            uiManager->RequestLoadDocument("hud.rml");
            app.hud.photoModeUiShown = false;
        }
    }

    // While the codex is open, populate it from the live codex+registry
    // (throttled by app.hud.codexSig: only rebuild rows when discovery state
    // changes). Skips the HUD objective update below.
    if (app.hud.codexOpen && uiManager && uiManager->GetContext()) {
        if (auto* doc = uiManager->GetContext()->GetDocument("codex")) {
            const luminumbra::game::CodexView cv =
                luminumbra::game::BuildCodexView(app.hud.creatureSpecies, app.hud.photoCodex);
            const int pct = static_cast<int>(cv.completeness * 100.0f + 0.5f);
            std::string sig = std::to_string(cv.discovered_count) + "/" +
                              std::to_string(cv.total_species) + "|" + std::to_string(pct);
            if (sig != app.hud.codexSig) {
                app.hud.codexSig = sig;
                if (auto* h = doc->GetElementById("codex_completion")) {
                    h->SetInnerRML(std::to_string(cv.discovered_count) + " / " +
                                   std::to_string(cv.total_species) + " discovered \xC2\xB7 " +
                                   std::to_string(pct) + "%");
                }
                if (auto* list = doc->GetElementById("codex_list")) {
                    std::string rows_rml;
                    for (const auto& r : cv.rows) {
                        std::string stars;
                        for (int s = 0; s < 5; ++s)
                            stars += (s < r.stars) ? "\xE2\x98\x85" : "\xE2\x98\x86";
                        const std::string cls =
                            r.discovered ? "codex-row codex-found" : "codex-row codex-locked";
                        const std::string name = r.discovered ? r.display_name : "? ? ?";
                        rows_rml += "<div class=\"" + cls +
                                    "\">"
                                    "<span class=\"codex-name\">" +
                                    name +
                                    "</span>"
                                    "<span class=\"codex-stars\">" +
                                    stars +
                                    "</span>"
                                    "</div>";
                    }
                    list->SetInnerRML(rows_rml);
                }
            }
        }
    }

    //  lazily build the starter objective chain keyed on the
    // default world's first creature, then surface the current goal +
    // progress on the HUD. Throttled by app.hud.objHudSig so the DOM is only
    // written when the current objective or its progress changes.
    if (!app.hud.objectivesInit) {
        app.hud.objectives = luminumbra::game::DefaultObjectives(
            static_cast<int>(Luminumbra::Components::CreatureSpeciesId16("grovestrider")));
        app.hud.objectivesInit = true;
    }
    if (!app.hud.codexOpen && !app.hud.photoMode.active && uiManager && uiManager->GetContext()) {
        if (auto* hud = uiManager->GetContext()->GetDocument("hud")) {
            const luminumbra::game::Objective* cur =
                app.hud.objectives.next_incomplete(app.hud.photoCodex);
            const std::uint32_t done = app.hud.objectives.completed_count(app.hud.photoCodex);
            // Everything maps to sound: a newly-completed goal rings the
            // success chime once (edge-triggered on the completed count).
            static std::uint32_t s_objDoneLast = 0;
            if (done > s_objDoneLast && audioManager)
                audioManager->PlayOneShot2D("objective_complete",
                                            Luminumbra::Client::BusId::Events);
            s_objDoneLast = done;
            std::string title = "All goals complete";
            float progress = 1.0f;
            if (cur) {
                title = cur->title;
                progress = luminumbra::game::EvaluateObjective(*cur, app.hud.photoCodex).progress;
            }
            const int pct = static_cast<int>(progress * 100.0f + 0.5f);
            // Onboarding hint shows until the first capture lands a species.
            const bool show_tutorial = app.hud.photoCodex.species_count() == 0;
            std::string sig = std::to_string(done) + "/" +
                              std::to_string(app.hud.objectives.size()) + "|" + title + "|" +
                              std::to_string(pct) + "|" + (show_tutorial ? "t1" : "t0");
            if (sig != app.hud.objHudSig) {
                app.hud.objHudSig = sig;
                if (auto* e = hud->GetElementById("obj_title"))
                    e->SetInnerRML(title);
                if (auto* e = hud->GetElementById("obj_progress"))
                    e->SetInnerRML(std::to_string(pct) + "%");
                if (auto* e = hud->GetElementById("obj_count"))
                    e->SetInnerRML(std::to_string(done) + " / " +
                                   std::to_string(app.hud.objectives.size()) + " goals");
                if (auto* e = hud->GetElementById("tutorial_hint"))
                    e->SetClass("hidden", !show_tutorial);
            }
        }
    }
    //  farming HUD — seed/harvest inventory + the crop the
    // player is facing (stage + quality + a harvest hint). Shown once the
    // player is near a crop or has farmed, so it never clutters a non-farming
    // session. Signature-gated like the objective tracker; render-only (reads
    // sim state).
    if (!app.hud.codexOpen && !app.hud.photoMode.active && uiManager && uiManager->GetContext() &&
        gameSession && camera) {
        if (auto* hud = uiManager->GetContext()->GetDocument("hud")) {
            namespace FC = Luminumbra::Components;
            const auto& freg = gameSession->GetRegistry();
            const glm::vec3 ffwd =
                glm::normalize(glm::vec3(camera->Front.x, 0.0f, camera->Front.z));
            const glm::vec3 aimXZ = glm::vec3(camera->Position) + ffwd * 3.0f;
            const Luminumbra::Vec3 aimv(aimXZ.x, camera->Position.y, aimXZ.z);
            const entt::entity crop =
                luminumbra::foliage::FarmingController::NearestPlant(freg, aimv, 3.0f);
            // Show once farming is in play: a crop in reach, a seed spent /
            // harvest made, or the player has cycled the species picker (so
            // it's discoverable).
            const bool farmed = app.hud.farming.seeds != 5 || app.hud.farming.harvests > 0;
            const bool show = (crop != entt::null) || farmed || app.hud.farmSelectedSpecies != 0;
            std::string cropText = "no crop in reach";
            if (crop != entt::null && freg.all_of<FC::PlantGrowthComponent>(crop)) {
                const auto& g = freg.get<FC::PlantGrowthComponent>(crop);
                static const char* kStage[] = {
                    "seed", "sprout", "juvenile", "mature", "flowering", "fruiting"};
                const int s = g.stage < 6 ? static_cast<int>(g.stage) : 5;
                // Optional species name (reverse-map the stable id; default
                // "crop").
                std::string sname = "crop";
                if (app.hud.farmSpeciesLoaded) {
                    for (const auto& t : app.hud.farmSpecies.all())
                        if (luminumbra::foliage::SpeciesId16(t.id) == g.species_id) {
                            sname = t.id;
                            break;
                        }
                }
                cropText = sname + " - " + kStage[s] + " - quality " +
                           std::to_string(static_cast<int>(g.quality));
                if (g.stage >= static_cast<std::uint8_t>(FC::PlantStage::Mature))
                    cropText += " - ready (J)";
            }
            // Selected planting species (V cycles it) shown so the player knows
            // what F plants.
            std::string selName = "wheat";
            if (app.hud.farmSpeciesLoaded && !app.hud.farmSpecies.all().empty())
                selName = app.hud.farmSpecies
                              .all()[static_cast<std::size_t>(app.hud.farmSelectedSpecies) %
                                     app.hud.farmSpecies.all().size()]
                              .id;
            const std::string inv = selName + " - " + std::to_string(app.hud.farming.seeds) +
                                    " seeds - " + std::to_string(app.hud.farming.harvests) +
                                    " harvested";
            const std::string sig = (show ? "1" : "0") + inv + "|" + cropText;
            if (sig != app.hud.farmHudSig) {
                app.hud.farmHudSig = sig;
                if (auto* e = hud->GetElementById("farming_panel"))
                    e->SetClass("hidden", !show);
                if (auto* e = hud->GetElementById("farm_inv"))
                    e->SetInnerRML(inv);
                if (auto* e = hud->GetElementById("farm_crop"))
                    e->SetInnerRML(cropText);
            }
        }
    }
}

// Photo-mode environment scrub, lens/exposure nudges, viewfinder readouts,
// and the shutter capture (verbatim; same guard note as above).
void UpdatePhotoCapture(ClientAppContext& app,
                        GLFWwindow* window,
                        const std::string& root_path_str,
                        Luminumbra::Client::PlayerController* playerController,
                        Luminumbra::Rendering::Camera* camera,
                        Luminumbra::Client::Rml_UIManager* uiManager,
                        Luminumbra::Client::IAudioManager* audioManager,
                        Luminumbra::world::GameSession* gameSession,
                        Luminumbra::Rendering::RenderPipeline& renderPipeline) {
    // photo-mode ENVIRONMENT scrub state (time-of-day + weather).
    // Render-only overrides of the visual day clock + weather overlay; they
    // NEVER touch the sim clock or WeatherSystem (world_hash unaffected).
    static bool s_photoEnvEngaged = false;
    static float s_photoTod = 0.5f;
    static int s_photoWeatherIdx = 0;
    static bool s_photoWeatherActive = false;
    if (!app.hud.photoMode.active && s_photoEnvEngaged) {
        // Left photo mode: release the day-clock hold (live clock resumes); the
        // per-frame sim weather push restores driven weather next frame.
        s_photoEnvEngaged = false;
        s_photoWeatherActive = false;
        renderPipeline.set_time_of_day_hold(false);
        //  rendering: drop the manual exposure override so
        // the automatic time-of-day exposure curve resumes.
        renderPipeline.set_exposure_override(-1.0f);
    }
    if (app.hud.photoMode.active) {
        // Apply lens nudges (aperture stops + focus metres), clamped
        // to sane photographic ranges.
        app.hud.photoMode.lens.aperture_f += playerController->consume_aperture_nudge();
        if (app.hud.photoMode.lens.aperture_f < 1.0f)
            app.hud.photoMode.lens.aperture_f = 1.0f;
        if (app.hud.photoMode.lens.aperture_f > 32.0f)
            app.hud.photoMode.lens.aperture_f = 32.0f;
        app.hud.photoMode.lens.focus_distance_m += playerController->consume_focus_nudge();
        if (app.hud.photoMode.lens.focus_distance_m < 0.2f)
            app.hud.photoMode.lens.focus_distance_m = 0.2f;
        if (app.hud.photoMode.lens.focus_distance_m > 200.0f)
            app.hud.photoMode.lens.focus_distance_m = 200.0f;

        //  manual exposure — shutter speed + ISO nudges
        // applied multiplicatively in stops. + shutter stop = FASTER
        // (less light, shorter time); + ISO stop = higher sensitivity.
        const float shutter_stops = playerController->consume_shutter_speed_nudge();
        if (shutter_stops != 0.0f) {
            app.hud.photoMode.lens.shutter_s *= std::pow(2.0f, -shutter_stops);
            if (app.hud.photoMode.lens.shutter_s < 1.0f / 4000.0f)
                app.hud.photoMode.lens.shutter_s = 1.0f / 4000.0f;
            if (app.hud.photoMode.lens.shutter_s > 30.0f)
                app.hud.photoMode.lens.shutter_s = 30.0f;
        }
        const float iso_stops = playerController->consume_iso_nudge();
        if (iso_stops != 0.0f) {
            app.hud.photoMode.lens.iso *= std::pow(2.0f, iso_stops);
            if (app.hud.photoMode.lens.iso < 50.0f)
                app.hud.photoMode.lens.iso = 50.0f;
            if (app.hud.photoMode.lens.iso > 25600.0f)
                app.hud.photoMode.lens.iso = 25600.0f;
        }

        //  rendering ( /  ): the lens now drives
        // the RENDER exposure. Map the (just-nudged) lens EV to an exposure
        // multiplier and push it; it OVERRIDES the analytic TOD exposure curve
        // so stopping down darkens and opening up brightens the frame —
        // the photographer exposes for the light. Render-only; never
        // world_hash.
        renderPipeline.set_exposure_override(
            Luminumbra::Rendering::ManualExposureMultiplier(app.hud.photoMode.lens));

        //  /0.2: TIME-OF-DAY scrub (K/L) + WEATHER cycle (T).
        // On entry, seed the scrub from the live clock and HOLD it (so the
        // per-frame auto-advance stops); each frame push the scrubbed values to
        // the render pipeline. Render-only — the sim is never touched.
        if (!s_photoEnvEngaged) {
            s_photoEnvEngaged = true;
            s_photoTod = renderPipeline.get_time_of_day();
            renderPipeline.set_time_of_day_hold(true);
        }
        s_photoTod += playerController->consume_tod_nudge();
        s_photoTod -= std::floor(s_photoTod); // wrap to [0,1)
        renderPipeline.set_time_of_day(s_photoTod);
        if (const int wc = playerController->consume_weather_cycle(); wc != 0) {
            s_photoWeatherActive = true;
            s_photoWeatherIdx = (((s_photoWeatherIdx + wc) % 5) + 5) % 5;
        }
        if (s_photoWeatherActive) {
            using WT = Luminumbra::Rendering::WeatherType;
            static const WT kW[5] = {WT::None, WT::Fog, WT::Rain, WT::Snow, WT::Storm};
            renderPipeline.set_weather(kW[s_photoWeatherIdx], s_photoWeatherIdx == 0 ? 0.0f : 0.7f);
        }

        //  live-bind the viewfinder readouts to the current lens.
        if (uiManager && uiManager->GetContext()) {
            if (auto* doc = uiManager->GetContext()->GetDocument("photo_mode")) {
                char rbuf[24];
                if (auto* e = doc->GetElementById("ro_aperture")) {
                    std::snprintf(rbuf, sizeof(rbuf), "f/%.1f", app.hud.photoMode.lens.aperture_f);
                    e->SetInnerRML(rbuf);
                }
                if (auto* e = doc->GetElementById("ro_focus")) {
                    std::snprintf(
                        rbuf, sizeof(rbuf), "%.1fm", app.hud.photoMode.lens.focus_distance_m);
                    e->SetInnerRML(rbuf);
                }
                //  shutter + ISO + a live EV meter so the
                // player sees whether they are exposing for the light.
                if (auto* e = doc->GetElementById("ro_shutter")) {
                    const float ss = app.hud.photoMode.lens.shutter_s;
                    if (ss >= 1.0f)
                        std::snprintf(rbuf, sizeof(rbuf), "%.1fs", ss);
                    else
                        std::snprintf(
                            rbuf, sizeof(rbuf), "1/%d", static_cast<int>(1.0f / ss + 0.5f));
                    e->SetInnerRML(rbuf);
                }
                if (auto* e = doc->GetElementById("ro_iso")) {
                    std::snprintf(rbuf,
                                  sizeof(rbuf),
                                  "ISO %d",
                                  static_cast<int>(app.hud.photoMode.lens.iso + 0.5f));
                    e->SetInnerRML(rbuf);
                }
                if (auto* e = doc->GetElementById("ro_ev")) {
                    const float live_lum =
                        SceneLuminanceFromSunElevation(renderPipeline.get_sun_elevation_rad());
                    const float lens_ev = luminumbra::game::ExposureValue(app.hud.photoMode.lens);
                    const float target_ev =
                        6.0f + live_lum * 9.0f;          // kSceneEvMin + lum*kSceneEvSpan
                    const float d = lens_ev - target_ev; // + = under (dark), - = over (blown)
                    const char* tag = (d > 0.5f) ? " dark" : (d < -0.5f) ? " bright" : " ok";
                    std::snprintf(rbuf, sizeof(rbuf), "%+.1f EV%s", d, tag);
                    e->SetInnerRML(rbuf);
                }
                // time-of-day phase (golden-hour cue for the photographer)
                // + the active weather preset.
                if (auto* e = doc->GetElementById("ro_tod")) {
                    const float elev = renderPipeline.get_sun_elevation_rad();
                    const char* ph = (elev > 0.6f)     ? "midday"
                                     : (elev > 0.12f)  ? "day"
                                     : (elev > -0.08f) ? "golden"
                                                       : "night";
                    e->SetInnerRML(ph);
                }
                if (auto* e = doc->GetElementById("ro_weather")) {
                    static const char* kWN[5] = {"clear", "fog", "rain", "snow", "storm"};
                    e->SetInnerRML(s_photoWeatherActive ? kWN[s_photoWeatherIdx] : "live");
                }
            }
        }

        if (playerController->consume_shutter_request()) {
            // The core action gets its sound: a soft camera shutter on every
            // capture.
            if (audioManager)
                audioManager->PlayOneShot2D("camera_shutter");
            int cap_w = 0, cap_h = 0;
            glfwGetFramebufferSize(window, &cap_w, &cap_h);
            //  scene luminance follows the sun so the
            // player must expose for the light (golden hour rewards,
            // night punishes). Render-derived; never feeds world_hash.
            const float scene_lum =
                SceneLuminanceFromSunElevation(renderPipeline.get_sun_elevation_rad());
            // Build the shot from the live frame (CONST registry read).
            const std::vector<luminumbra::game::PhotoSubjectView> subjects =
                GatherPhotoSubjects(gameSession->GetRegistry(), *camera, cap_w, cap_h, scene_lum);
            //  stamp the capture's time-of-day; BuildShotInput
            // fills the principal subject's behaviour + scene luminance.
            luminumbra::game::ObservationMetadata obs;
            obs.time_of_day = renderPipeline.get_time_of_day();
            const luminumbra::game::ShotInput shot = luminumbra::game::BuildShotInput(
                subjects, app.hud.photoMode.lens, scene_lum, 0.5f, 1.0f, obs);
            // Was this species already in the codex BEFORE the capture? A
            // subject-bearing shot of a never-seen species is a DISCOVERY.
            const bool had_subject = !shot.composition.subjects.empty();
            const bool was_known =
                had_subject && app.hud.photoCodex.discovered(shot.main_species_id);
            const luminumbra::game::ShotVerdict verdict =
                luminumbra::game::CaptureShot(app.hud.photoCodex, shot);
            app.hud.photoMode.last_total = verdict.total;
            app.hud.photoMode.last_stars = verdict.stars;
            ++app.hud.photoMode.captures;

            const bool is_discovery = had_subject && !was_known;
            const std::string species_name =
                had_subject ? app.hud.creatureSpecies.DisplayName(
                                  static_cast<std::uint16_t>(shot.main_species_id))
                            : std::string("no subject");

            //  reveal the star-verdict panel on the overlay (filled to
            //  last_stars). name the subject + flag a first-time DISCOVERY so
            //  the codex
            // fill is felt at the moment of capture.
            if (uiManager && uiManager->GetContext()) {
                if (auto* doc = uiManager->GetContext()->GetDocument("photo_mode")) {
                    if (auto* panel = doc->GetElementById("verdict-panel"))
                        panel->SetClass("hidden", false);
                    if (auto* heading = doc->GetElementById("verdict_heading")) {
                        heading->SetInnerRML(had_subject ? species_name : "captured");
                    }
                    if (auto* note = doc->GetElementById("verdict_note")) {
                        if (is_discovery) {
                            note->SetInnerRML("New species discovered! \xE2\x98\x85 " +
                                              std::to_string(app.hud.photoCodex.species_count()) +
                                              " in codex");
                            note->SetClass("verdict-discovery", true);
                        } else if (had_subject) {
                            note->SetInnerRML("Codex updated \xC2\xB7 best shot kept");
                            note->SetClass("verdict-discovery", false);
                        } else {
                            note->SetInnerRML("no subject in frame");
                            note->SetClass("verdict-discovery", false);
                        }
                    }
                    if (auto* st = doc->GetElementById("verdict_stars")) {
                        std::string stars_rml;
                        for (int si = 0; si < 5; ++si) {
                            stars_rml += (si < app.hud.photoMode.last_stars)
                                             ? "<span class=\"verdict-star "
                                               "verdict-star-on\">\xE2\x98\x85</span>"
                                             : "<span "
                                               "class=\"verdict-star\">\xE2\x98\x85</"
                                               "span>";
                        }
                        st->SetInnerRML(stars_rml);
                    }
                }
            }

            // Everything maps to sound: a first-time codex fill rings the
            // discovery chime at the moment of capture (2D, UI-felt).
            if (is_discovery && audioManager)
                audioManager->PlayOneShot2D("discovery", Luminumbra::Client::BusId::Events);

            // Persist the framebuffer (PPM) + a verdict sidecar.
            std::error_code _photo_ec;
            const std::filesystem::path photo_dir = std::filesystem::path(root_path_str) / "photos";
            std::filesystem::create_directories(photo_dir, _photo_ec);
            const std::string stamp = "photo-" + std::to_string(app.hud.photoMode.captures);
            if (cap_w > 0 && cap_h > 0) {
                std::vector<unsigned char> px(static_cast<std::size_t>(cap_w) *
                                              static_cast<std::size_t>(cap_h) * 3u);
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, cap_w, cap_h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                WritePixelBufferPpm(photo_dir / (stamp + ".ppm"), cap_w, cap_h, px);
                // Also drop a small TGA thumbnail where the gallery can load it
                // (RmlUi decodes TGA only). The gallery enumerates these on
                // open.
                WriteCaptureThumbnailTga(
                    std::filesystem::path(root_path_str) / "data" / "ui" / "captures" /
                        ("cap_" + std::to_string(app.hud.photoMode.captures) + ".tga"),
                    cap_w,
                    cap_h,
                    px,
                    512);
            }
            luminumbra::game::PhotoSidecar side;
            side.stamp = stamp;
            side.verdict = verdict;
            side.species_id = shot.main_species_id;
            side.lens = app.hud.photoMode.lens;
            side.observation = shot.observation; // behaviour/time/light context
            std::ofstream sidecar(photo_dir / (stamp + ".photo.json"),
                                  std::ios::binary | std::ios::trunc);
            if (sidecar) {
                const std::string json = luminumbra::game::SerializePhotoSidecar(side);
                sidecar.write(json.data(), static_cast<std::streamsize>(json.size()));
            }
            LUMINUMBRA_CORE_INFO("Photo captured: {} stars (total {}), saved {}",
                                 verdict.stars,
                                 verdict.total,
                                 stamp);
        }
    }
}

} // namespace

// --- feature: photo-mode capture loop ---
// Runs AFTER render_frame, on the RENDER side, against a CONST
// registry: it reads camera + creature state, scores via the landed
// PhotoSession scorers, and on shutter persists a PPM + verdict
// sidecar into photos/. It NEVER ticks the sim or mutates the
// registry, so determinism cannot regress (the sim-isolation gate
// test pins the pure path). The interactive-only guard keeps it out
// of the automated scenario/gate runs.
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
                           const ScenarioHarness::RuntimeScenarioConfig& scenario_config) {
    if (playerController && currentState == GameState::IN_GAME && !scenario_config.active() &&
        !app.hud.paused) {
        app.hud.photoMode.active = playerController->photo_mode_active();
        UpdateCodexObjectivesFarmHud(
            app, playerController, camera, uiManager, audioManager, gameSession);
        UpdatePhotoCapture(app,
                           window,
                           root_path_str,
                           playerController,
                           camera,
                           uiManager,
                           audioManager,
                           gameSession,
                           renderPipeline);
    }
}

void DrawCreatureNameplates(ClientAppContext& app,
                            GLFWwindow* window,
                            Luminumbra::Rendering::Camera* camera,
                            Luminumbra::world::GameSession* gameSession) {
    //  Minecraft-style floating ID nameplates above each creature's head. Projects
    // the world position to screen via the camera and draws a label (id + sex + generation)
    // into the ImGui foreground list, so it shows in the live view AND in the demo capture
    // (this runs even during the timelapse, unlike the gated debug windows below).
    if (app.overlay.imgui_enabled && app.capture.timelapse_creatures && camera && gameSession) {
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw > 0 && fbh > 0) {
            const float aspect = static_cast<float>(fbw) / static_cast<float>(fbh);
            const glm::mat4 vp =
                glm::perspective(glm::radians(camera->Zoom), aspect, 0.1f, 10000.0f) *
                camera->GetViewMatrix();
            auto* dl = ImGui::GetForegroundDrawList();
            auto& reg = gameSession->GetRegistry();
            auto view = reg.view<const Luminumbra::Components::CreatureComponent,
                                 const Luminumbra::Components::TransformComponent>();
            for (auto e : view) {
                const auto& cr = view.get<const Luminumbra::Components::CreatureComponent>(e);
                const auto& tf = view.get<const Luminumbra::Components::TransformComponent>(e);
                const glm::vec4 clip =
                    vp * glm::vec4(tf.position.x, tf.position.y + 2.4f, tf.position.z, 1.0f);
                if (clip.w <= 0.05f)
                    continue; // behind the camera
                const float sx = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(fbw);
                const float sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * static_cast<float>(fbh);
                char label[48];
                const unsigned idx = static_cast<unsigned>(entt::to_entity(e));
                if (cr.eaten) {
                    std::snprintf(label, sizeof(label), "#%u dead", idx);
                } else if (cr.is_predator) {
                    std::snprintf(label, sizeof(label), "#%u PRED", idx);
                } else if (const auto* gn =
                               reg.try_get<Luminumbra::Components::CreatureGenomeComponent>(e)) {
                    std::snprintf(label,
                                  sizeof(label),
                                  "#%u %c g%u",
                                  idx,
                                  gn->female ? 'F' : 'M',
                                  gn->generation);
                } else {
                    std::snprintf(label, sizeof(label), "#%u", idx);
                }
                const ImVec2 sz = ImGui::CalcTextSize(label);
                const ImVec2 at(sx - sz.x * 0.5f, sy - sz.y);
                dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                            IM_COL32(0, 0, 0, 200),
                            label); // shadow
                dl->AddText(at, IM_COL32(255, 255, 255, 235), label);
            }
        }
    }
}

void DrawFrameStatusOverlays(ClientAppContext& app,
                             GameState currentState,
                             const ScenarioHarness::RuntimeScenarioConfig& scenario_config,
                             Luminumbra::Client::PlayerController* playerController) {
    if (app.overlay.imgui_enabled && playerController && app.capture.timelapse_frames == 0) {
        playerController->RenderDebugUI();
    }
    // Always-on time-scale indicator (when not real-time) so slow-mo / fast-forward
    // / pause is obvious at a glance.
    if (app.overlay.imgui_enabled && app.capture.timeScale != 1.0f &&
        app.capture.timelapse_frames == 0) {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 60.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.5f);
        if (ImGui::Begin("##timescale",
                         nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoMove)) {
            if (app.capture.timeScale == 0.0f)
                ImGui::Text("|| PAUSED  (\\ to resume)");
            else
                ImGui::Text("TIME  x%.2f", app.capture.timeScale);
        }
        ImGui::End();
    }
    //  minimal crop HUD — seed/harvest inventory + the farming verb hints.
    if (app.overlay.imgui_enabled && currentState == GameState::IN_GAME &&
        !scenario_config.active() && app.capture.timelapse_frames == 0) {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 92.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        if (ImGui::Begin("##farmhud",
                         nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoMove)) {
            ImGui::Text("FARM  seeds:%d  harvests:%d  yield:%.1f",
                        app.hud.farming.seeds,
                        app.hud.farming.harvests,
                        app.hud.farming.total_yield);
            ImGui::TextDisabled("F plant   G water   H fertilize   J harvest");
        }
        ImGui::End();
    }
}

void DrawGpuProfilerOverlay(ClientAppContext& app,
                            GameState currentState,
                            Luminumbra::Rendering::RenderPipeline& renderPipeline,
                            Luminumbra::Client::Rml_UIManager* uiManager) {
    // Live GPU profiler : per-pass GPU-ms + draw/instance counts read from the render
    // pipeline's GL_TIMESTAMP timer ring (render-side only; never hashed). The first real
    // interactive readout for the engine optimization pass.
    if (app.overlay.imgui_enabled && app.overlay.show_gpu_profiler &&
        currentState == GameState::IN_GAME && app.capture.timelapse_frames == 0) {
        const auto& gp = renderPipeline.get_last_render_pass_stats();
        ImGui::SetNextWindowPos(ImVec2(10.0f, 120.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        if (ImGui::Begin("GPU Profiler ",
                         &app.overlay.show_gpu_profiler,
                         ImGuiWindowFlags_AlwaysAutoResize)) {
            const double total_ms = gp.shadow_gpu_ms + gp.gbuffer_gpu_ms + gp.ssao_gpu_ms +
                                    gp.ssao_blur_gpu_ms + gp.lighting_gpu_ms + gp.water_gpu_ms +
                                    gp.skybox_gpu_ms + gp.particle_gpu_ms + gp.foliage_gpu_ms +
                                    gp.aerial_gpu_ms + gp.final_blit_gpu_ms;
            if (!gp.gpu_timers_supported) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                   "GPU timers unsupported on this context");
            }
            const ImVec4 over(1.0f, 0.45f, 0.45f, 1.0f);
            const ImVec4 okc(0.70f, 0.85f, 0.70f, 1.0f);
            ImGui::TextColored(total_ms > 3.333 ? over : okc,
                               "GPU frame total: %6.3f ms   (budget 3.333 ms @ 300fps)",
                               total_ms);
            const double ui_ms = uiManager ? uiManager->GetLastUiFrameMs() : 0.0;
            ImGui::Text("UI submit (CPU): %6.3f ms", ui_ms);
            ImGui::Separator();
            auto row = [&](const char* name, double ms) {
                ImGui::TextColored(ms > 1.0 ? over : okc, "  %-11s %7.3f ms", name, ms);
            };
            row("gbuffer", gp.gbuffer_gpu_ms);
            row("shadow", gp.shadow_gpu_ms);
            row("lighting", gp.lighting_gpu_ms);
            row("foliage", gp.foliage_gpu_ms);
            row("particle", gp.particle_gpu_ms);
            row("water", gp.water_gpu_ms);
            row("ssao", gp.ssao_gpu_ms + gp.ssao_blur_gpu_ms);
            row("aerial", gp.aerial_gpu_ms);
            row("skybox", gp.skybox_gpu_ms);
            row("final_blit", gp.final_blit_gpu_ms);
            ImGui::Separator();
            ImGui::Text("terrain: %5zu draws  %5zu chunks  %7zu tris",
                        gp.terrain_draws,
                        gp.terrain_visible_chunks,
                        gp.terrain_indices_drawn / 3);
            ImGui::Text(
                "far-LOD: %5zu draws  %7zu tris", gp.far_region_draws, gp.far_indices_drawn / 3);
            ImGui::Text("foliage: %5zu draws  %7zu instances",
                        gp.foliage_draws,
                        gp.foliage_instances_drawn);
            ImGui::Text(
                "particle: %5zu draws  %7zu instances", gp.particle_draws, gp.particles_drawn);
            ImGui::Text("shadow: %5zu draws", gp.shadow_draws);
            ImGui::Text("water: %5zu draws", gp.water_draws);
        }
        ImGui::End();
    }
}

void SpawnDebugGlassPanes(ClientAppContext& app,
                          GameState currentState,
                          Luminumbra::world::GameSession* gameSession,
                          Luminumbra::Rendering::RenderPipeline& renderPipeline) {
    // --debug-glass-pane — stage three stained-glass
    // panes on the terrain near spawn (one-time). Render-only capture subject.
    if (app.overlay.debug_glass_panes && !app.overlay.glass_panes_spawned &&
        currentState == GameState::IN_GAME && gameSession) {
        if (auto* gws = gameSession->GetWorldSystem()) {
            app.overlay.glass_panes_spawned = true;
            std::vector<Luminumbra::Rendering::GlassPaneItem> panes;
            const glm::vec3 pane_tints[3] = {
                {0.95f, 0.25f, 0.25f}, {0.25f, 0.85f, 0.35f}, {0.30f, 0.45f, 0.95f}};
            for (int pi = 0; pi < 3; ++pi) {
                const float px = 12.0f + 5.0f * static_cast<float>(pi);
                const float pz = 14.0f;
                const float py = gws->GetTerrainHeightAt(px, pz);
                Luminumbra::Rendering::GlassPaneItem pane;
                glm::mat4 pm(1.0f);
                pm = glm::translate(pm, glm::vec3(px, py, pz));
                // Lean the panes back ~50 deg so the near-noon sun projects a
                // real footprint (a vertical pane under an overhead sun casts
                // only a sliver - the first capture attempt's lesson).
                pm = glm::rotate(pm, glm::radians(-50.0f), glm::vec3(1.0f, 0.0f, 0.0f));
                pm = glm::scale(pm, glm::vec3(4.0f, 5.0f, 1.0f)); // 4 m wide, 5 m tall
                pane.model = pm;
                pane.tint = pane_tints[pi];
                pane.thickness = 1.0f;
                panes.push_back(pane);
            }
            renderPipeline.set_glass_panes(std::move(panes));
            LUMINUMBRA_CORE_INFO("Debug glass panes staged (3 stained-glass tints) near spawn");
        }
    }
}

void UpdateShaderTools(ClientAppContext& app,
                       GameState currentState,
                       Luminumbra::Rendering::RenderPipeline& renderPipeline) {
    // live shader authoring (crawl  + walk: watcher + panel ).
    // Render-only end to end: shaders/uniforms never feed the sim or world_hash.
    if (currentState == GameState::IN_GAME && app.capture.timelapse_frames == 0) {
        // Crawl: reload-all requested by  (executed here, on the GL thread).
        if (app.overlay.request_shader_reload) {
            app.overlay.request_shader_reload = false;
            renderPipeline.reload_all_shaders();
        }
        // Walk (-3): opt-in once/sec mtime poll over the roster's source
        // files; a changed file triggers that shader's rollback-safe Reload.
        if (app.overlay.shader_auto_reload) {
            const double watch_now = glfwGetTime();
            if (watch_now - app.overlay.shader_watch_last_poll >= 1.0) {
                app.overlay.shader_watch_last_poll = watch_now;
                renderPipeline.enumerate_shaders(
                    [&](const char* sh_name, Luminumbra::Rendering::Shader* sh) {
                        if (!sh)
                            return;
                        bool changed = false;
                        for (const std::string& p : {sh->VertexPath(), sh->FragmentPath()}) {
                            if (p.empty())
                                continue;
                            std::error_code ec;
                            const auto t = std::filesystem::last_write_time(p, ec);
                            if (ec)
                                continue;
                            auto it = app.overlay.shader_watch_mtimes.find(p);
                            if (it != app.overlay.shader_watch_mtimes.end() && it->second != t) {
                                changed = true;
                            }
                            app.overlay.shader_watch_mtimes[p] = t;
                        }
                        if (changed) {
                            LUMINUMBRA_CORE_INFO("Shader source changed on disk -> reloading {}",
                                                 sh_name);
                            sh->Reload();
                        }
                    });
            }
        }
        // Walk (-4): the dev shader panel — roster status, per-shader
        // reload, and LIVE uniform editing via glProgramUniform (GL 4.5 DSA).
        // Honesty note (-5): passes re-set most uniforms per draw; an
        // edit persists only for uniforms a pass never sets (u_dev_*).
        if (app.overlay.imgui_enabled && app.overlay.show_shader_panel) {
            ImGui::SetNextWindowPos(ImVec2(10.0f, 420.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(440.0f, 520.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Shaders ", &app.overlay.show_shader_panel)) {
                ImGui::TextWrapped(
                    "Live shader authoring (): edit res/shaders/ in any editor, "
                    "reload hot-swaps rollback-safe. Uniform edits persist only for "
                    "uniforms a pass does not re-set per frame (u_dev_* convention).");
                if (ImGui::Button("Reload All "))
                    app.overlay.request_shader_reload = true;
                ImGui::SameLine();
                ImGui::Checkbox("Auto-reload on file change", &app.overlay.shader_auto_reload);
                ImGui::Separator();
                renderPipeline.enumerate_shaders([&](const char* sh_name,
                                                     Luminumbra::Rendering::Shader* sh) {
                    if (!sh)
                        return;
                    ImGui::PushID(sh_name);
                    if (ImGui::CollapsingHeader(sh_name)) {
                        const bool sh_ok = sh->IsValid();
                        ImGui::TextColored(sh_ok ? ImVec4(0.70f, 0.85f, 0.70f, 1.0f)
                                                 : ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                                           sh_ok ? "valid" : "INVALID (prior program kept)");
                        if (!sh->Diagnostic().empty()) {
                            ImGui::TextWrapped("%s", sh->Diagnostic().c_str());
                        }
                        if (ImGui::Button("Reload"))
                            sh->Reload();
                        const GLuint prog = sh->Id();
                        GLint uniform_count = 0;
                        glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &uniform_count);
                        for (GLint u = 0; u < uniform_count; ++u) {
                            char uname[128];
                            GLsizei ulen = 0;
                            GLint usize = 0;
                            GLenum utype = 0;
                            glGetActiveUniform(prog,
                                               static_cast<GLuint>(u),
                                               sizeof(uname),
                                               &ulen,
                                               &usize,
                                               &utype,
                                               uname);
                            if (usize != 1)
                                continue; // arrays: not editable here
                            const GLint loc = glGetUniformLocation(prog, uname);
                            if (loc < 0)
                                continue;
                            ImGui::PushID(u);
                            const bool looks_color =
                                std::string_view(uname).find("olor") != std::string_view::npos;
                            switch (utype) {
                                case GL_FLOAT: {
                                    float v = 0.0f;
                                    glGetUniformfv(prog, loc, &v);
                                    if (ImGui::DragFloat(uname, &v, 0.01f))
                                        glProgramUniform1f(prog, loc, v);
                                    break;
                                }
                                case GL_FLOAT_VEC2: {
                                    float v[2] = {};
                                    glGetUniformfv(prog, loc, v);
                                    if (ImGui::DragFloat2(uname, v, 0.01f))
                                        glProgramUniform2fv(prog, loc, 1, v);
                                    break;
                                }
                                case GL_FLOAT_VEC3: {
                                    float v[3] = {};
                                    glGetUniformfv(prog, loc, v);
                                    const bool edited =
                                        looks_color
                                            ? ImGui::ColorEdit3(uname, v, ImGuiColorEditFlags_Float)
                                            : ImGui::DragFloat3(uname, v, 0.01f);
                                    if (edited)
                                        glProgramUniform3fv(prog, loc, 1, v);
                                    break;
                                }
                                case GL_FLOAT_VEC4: {
                                    float v[4] = {};
                                    glGetUniformfv(prog, loc, v);
                                    const bool edited =
                                        looks_color
                                            ? ImGui::ColorEdit4(uname, v, ImGuiColorEditFlags_Float)
                                            : ImGui::DragFloat4(uname, v, 0.01f);
                                    if (edited)
                                        glProgramUniform4fv(prog, loc, 1, v);
                                    break;
                                }
                                case GL_INT: {
                                    GLint v = 0;
                                    glGetUniformiv(prog, loc, &v);
                                    if (ImGui::DragInt(uname, &v))
                                        glProgramUniform1i(prog, loc, v);
                                    break;
                                }
                                case GL_BOOL: {
                                    GLint v = 0;
                                    glGetUniformiv(prog, loc, &v);
                                    bool b = v != 0;
                                    if (ImGui::Checkbox(uname, &b))
                                        glProgramUniform1i(prog, loc, b ? 1 : 0);
                                    break;
                                }
                                default:
                                    break; // samplers/matrices: display-only, skip
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::PopID();
                });
            }
            ImGui::End();
        }
    }
}

void DrawSettingsWindow(ClientAppContext& app,
                        GLFWwindow* window,
                        Luminumbra::Rendering::Camera* camera,
                        Luminumbra::Client::IAudioManager* audioManager,
                        luminumbra::core::SystemConfig& systemConfig,
                        WindowState& windowState) {
    // Settings menu ( to toggle; frees the cursor). Render-only; user.* is never
    // hashed. Changes apply live and "Save" persists them to the per-user overlay.
    // The polished RML settings screen (settings.rml) is the follow-on.
    if (app.overlay.imgui_enabled && app.hud.show_settings && camera &&
        app.capture.timelapse_frames == 0) {
        ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Settings ")) {
            luminumbra::core::UserSettings& us = systemConfig.user();
            if (ImGui::SliderFloat(
                    "Look sensitivity", &us.mouse_sensitivity, 0.01f, 1.0f, "%.3f")) {
                camera->MouseSensitivity = us.mouse_sensitivity; // applied live
            }
            if (ImGui::SliderFloat("FOV", &us.fov, 30.0f, 110.0f, "%.0f deg")) {
                camera->Zoom = us.fov;
            }
            if (ImGui::Checkbox("VSync", &us.vsync)) {
                glfwSwapInterval(us.vsync ? 1 : 0);
            }
            ImGui::Separator();
            ImGui::SliderFloat("Time scale", &app.capture.timeScale, 0.0f, 8.0f, "%.2fx");
            ImGui::SameLine();
            if (ImGui::SmallButton("1x"))
                app.capture.timeScale = 1.0f;
            ImGui::SameLine();
            if (ImGui::SmallButton(app.capture.timeScale == 0.0f ? "Resume" : "Pause"))
                app.capture.timeScale = (app.capture.timeScale == 0.0f) ? 1.0f : 0.0f;
            ImGui::TextDisabled("engine time: [ slower   ] faster   \\ reset");
            ImGui::Separator();
            {
                // Window mode — applied live via ApplyWindowMode (no-op on capture-pinned
                // runs).
                const char* modes[] = {"windowed", "borderless", "fullscreen"};
                int cur = 1; // default borderless
                for (int i = 0; i < 3; ++i)
                    if (us.window_mode == modes[i])
                        cur = i;
                if (ImGui::Combo("Window mode", &cur, modes, 3)) {
                    us.window_mode = modes[cur];
                    const WindowMode m = ParseWindowMode(us.window_mode, windowState.mode);
                    ApplyWindowMode(window, windowState, m);
                }
            }
            {
                // Resolution — applied live in windowed mode (borderless/fullscreen use
                // the monitor's resolution); suppressed on capture-pinned gate runs.
                const char* resos[] = {
                    "1280x720", "1920x1080", "2560x1440", "3440x1440", "3840x1600", "3840x2160"};
                int rcur = -1;
                for (int i = 0; i < 6; ++i)
                    if (us.resolution == resos[i])
                        rcur = i;
                if (ImGui::Combo("Resolution", &rcur, resos, 6) && rcur >= 0) {
                    us.resolution = resos[rcur];
                    const std::string& r = us.resolution;
                    const auto xp = r.find('x');
                    if (xp != std::string::npos) {
                        const int rw = std::atoi(r.substr(0, xp).c_str());
                        const int rh = std::atoi(r.substr(xp + 1).c_str());
                        if (rw > 0 && rh > 0) {
                            windowState.windowedWidth = rw;
                            windowState.windowedHeight = rh;
                            if (us.window_mode == "windowed" && !windowState.capture_pinned)
                                glfwSetWindowSize(window, rw, rh);
                        }
                    }
                }
            }
            if (ImGui::SliderFloat("Master volume", &us.audio_master, 0.0f, 1.0f, "%.2f")) {
                if (audioManager)
                    audioManager->SetMasterVolume(us.audio_master); // applied live
            }
            if (ImGui::SliderFloat("Music volume", &us.audio_music, 0.0f, 1.0f, "%.2f")) {
                if (audioManager)
                    audioManager->SetMusicVolume(us.audio_music); // applied live (music bus)
            }
            if (ImGui::SliderFloat("SFX volume", &us.audio_sfx, 0.0f, 1.0f, "%.2f")) {
                if (audioManager)
                    audioManager->SetSfxVolume(us.audio_sfx); // applied live (sfx bus)
            }
            if (ImGui::CollapsingHeader("Controls (keyboard)")) {
                for (const auto& def : Luminumbra::Client::kInputActionDefs) {
                    const int idx = static_cast<int>(def.action);
                    const int kc = systemConfig.keybind(def.name, def.default_key);
                    const char* kn = glfwGetKeyName(kc, 0);
                    char btn[48];
                    if (app.hud.rebindCaptureAction == idx)
                        std::snprintf(btn, sizeof(btn), "press a key...##%s", def.name);
                    else if (kn)
                        std::snprintf(btn, sizeof(btn), "%s##%s", kn, def.name);
                    else
                        std::snprintf(btn, sizeof(btn), "key %d##%s", kc, def.name);
                    ImGui::Text("%-12s", def.name);
                    ImGui::SameLine(150);
                    if (ImGui::Button(btn))
                        app.hud.rebindCaptureAction = idx;
                }
                ImGui::TextDisabled("click a binding, then press a key (Esc cancels)");
            }
            if (ImGui::Button("Save settings")) {
                const std::string path = luminumbra::core::SystemConfig::DefaultUserOverlayPath();
                const bool ok = systemConfig.SaveUserOverlay(path);
                LUMINUMBRA_CORE_INFO("Settings {} ({})", ok ? "saved" : "save FAILED", path);
            }
            ImGui::TextDisabled("user.* — client-only, never hashed");
        }
        ImGui::End();
    }
}

} // namespace Luminumbra::Client::App
