#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app/FrameAudio.h"

#include "audio/AudioPropagationSystem.h"   // ComputeWaterfallRoar (static)
#include "audio/EnvironmentalAudioSystem.h" // day/night beds + biome/weather reverb
#include "audio/IAudioManager.h"
#include "core/Log.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/ForagingComponents.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "rendering/Camera.h"
#include "rendering/RenderPipeline.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Luminumbra::Client::App {

namespace {

// 3D listener follow + environmental-audio feed (verbatim region 1).
void UpdateListenerEnvironment(GameState currentState,
                               float deltaTime,
                               Luminumbra::Client::IAudioManager* audioManager,
                               Luminumbra::Client::EnvironmentalAudioSystem* envAudio,
                               Luminumbra::Rendering::Camera* camera,
                               Luminumbra::world::GameSession* gameSession,
                               Luminumbra::Rendering::RenderPipeline& renderPipeline) {
    // 3D audio LISTENER follows the player so footsteps / ambient bed / creature sounds
    // spatialize correctly (without this the listener sits at the origin and positional audio
    // is near-silent).
    if (currentState == GameState::IN_GAME && audioManager && camera) {
        audioManager->SetListenerTransform(camera->Position, camera->Front, camera->Up);
        // point the audio-occlusion raycaster at the LIVE physics world so
        // geometry between a sound and the listener muffles it (real raycasts, not the
        // distance-only fallback). Re-set each frame -> correct across world (re)load/unload;
        // nullptr when there is no physics -> occlusion falls back to distance-only.
        if (auto* mam = dynamic_cast<Luminumbra::Client::MiniaudioManager*>(audioManager)) {
            mam->SetPhysicsSystem(gameSession ? gameSession->GetPhysicsSystem() : nullptr);
        }
        // feed the sun elevation (sin; the sun vector points AWAY from
        // the sun — same convention as the dawn/dusk cues below) + tick the
        // day/night bed crossfade (internally throttled to 10 Hz).
        envAudio->SetSunElevation(-renderPipeline.sun_direction().y);
        envAudio->Update(camera->Position, static_cast<float>(deltaTime));
    }
}

// Material-keyed player footsteps (verbatim region 2).
void UpdatePlayerFootsteps(ClientAppContext& app,
                           GameState currentState,
                           float deltaTime,
                           Luminumbra::Client::IAudioManager* audioManager,
                           Luminumbra::Rendering::Camera* camera,
                           Luminumbra::world::GameSession* gameSession,
                           const ScenarioHarness::RuntimeScenarioConfig& scenario_config) {
    // Player FOOTSTEPS (interactive audio): when the player walks, play a footstep keyed to the
    // surface material under them (stone vs grass/soil/etc.), at a distance-based cadence so it
    // tracks speed. Render/audio-only; live play only (never in scenario captures, so
    // determinism and the visual gates are untouched). Stride length / speed gates are tunable
    // by ear.
    if (currentState == GameState::IN_GAME && audioManager && camera && !app.hud.paused &&
        !scenario_config.active() && gameSession && gameSession->GetWorldSystem()) {
        static glm::vec3 s_footLastPos = camera->Position;
        static float s_footDist = 0.0f;
        const glm::vec3 fp = camera->Position;
        const float fdx = fp.x - s_footLastPos.x, fdz = fp.z - s_footLastPos.z;
        const float fhoriz = std::sqrt(fdx * fdx + fdz * fdz);
        s_footLastPos = fp;
        const float fspeed = deltaTime > 0.0f ? fhoriz / static_cast<float>(deltaTime) : 0.0f;
        if (fspeed < 0.8f || fspeed > 25.0f) {
            s_footDist = 0.0f; // standing still, or a teleport/respawn jump -> reset
        } else {
            s_footDist += fhoriz;
            if (s_footDist >= 1.9f) { // stride length (m)
                s_footDist = 0.0f;
                auto* fws = gameSession->GetWorldSystem();
                const float fth = fws->GetTerrainHeightAt(fp.x, fp.z);
                // Material-based footstep: the surface under the player picks the sound.
                const char* fev = "footstep_grass";
                switch (fws->SurfaceVertexMaterial(fp.x, fp.z, fth)) {
                    case Luminumbra::MaterialType::Stone:
                    case Luminumbra::MaterialType::Deepslate:
                        fev = "footstep_stone";
                        break;
                    case Luminumbra::MaterialType::Soil:
                        fev = "footstep_soil";
                        break;
                    case Luminumbra::MaterialType::Sand:
                        fev = "footstep_sand";
                        break;
                    case Luminumbra::MaterialType::Water:
                        fev = "footstep_water";
                        break;
                    case Luminumbra::MaterialType::LuminCrystal:
                        fev = "footstep_crystal";
                        break;
                    case Luminumbra::MaterialType::Grass:
                    default:
                        fev = "footstep_grass";
                        break;
                }
                audioManager->PlayOneShot(fev, glm::vec3(fp.x, fth, fp.z));
            }
        }
    }
}

// Living-world ambience: rain/stream/waterfall/wind beds, creature
// calls/sleep/feeding/colony/locomotion, and the dawn/dusk cues (verbatim
// region 3).
void UpdateLivingWorldAudio(ClientAppContext& app,
                            GameState currentState,
                            float deltaTime,
                            Luminumbra::Client::IAudioManager* audioManager,
                            Luminumbra::Client::EnvironmentalAudioSystem* envAudio,
                            Luminumbra::Rendering::Camera* camera,
                            luminumbra::core::SystemConfig& systemConfig,
                            Luminumbra::world::GameSession* gameSession,
                            Luminumbra::Rendering::RenderPipeline& renderPipeline,
                            const ScenarioHarness::RuntimeScenarioConfig& scenario_config) {
    // LIVING-WORLD AUDIO: weather-reactive RAIN + occasional CREATURE CALLS. Periodic in live
    // play; render/audio-only (reads sim state, never mutates -> determinism + gates
    // untouched).
    if (currentState == GameState::IN_GAME && audioManager && camera && !app.hud.paused &&
        !scenario_config.active() && gameSession) {
        static float s_envTimer = 0.0f, s_callTimer = 0.0f, s_sleepTimer = 0.0f;
        static float s_feedTimer = 0.0f, s_colonyTimer = 0.0f;
        static bool s_rainOn = false, s_waterOn = false;
        s_envTimer += static_cast<float>(deltaTime);
        s_callTimer += static_cast<float>(deltaTime);
        s_sleepTimer += static_cast<float>(deltaTime);
        s_feedTimer += static_cast<float>(deltaTime);
        s_colonyTimer += static_cast<float>(deltaTime);
        const glm::vec3 pc = camera->Position;
        if (s_envTimer >= 0.5f) {
            s_envTimer = 0.0f;
            // Rain: tie ambient_rain to the live weather precipitation at the player
            // (hysteresis so it doesn't flutter at a storm-cell edge). Same field the foliage
            // growth reads.
            if (auto* weather = gameSession->GetWeatherSystem()) {
                const float precip = weather->PrecipitationAt(Luminumbra::Vec3(pc.x, pc.y, pc.z));
                if (!s_rainOn && precip > 0.18f) {
                    audioManager->PlayAmbientLoop("ambient_rain", pc, 1.0e6f);
                    s_rainOn = true;
                } else if (s_rainOn && precip < 0.08f) {
                    audioManager->StopAmbientLoop("ambient_rain");
                    s_rainOn = false;
                }
                // thunder now follows the SIM strike schedule when
                // the live-weather bridge is on — each queued strike cue fires after
                // its physical sound delay (distance / 343 m/s), volume by
                // magnitude/distance. The legacy flat 22 s timer remains ONLY as the
                // fallback when the bridge is off (no schedule consumer running).
                if (systemConfig.enabled(luminumbra::core::SysKey::RenderLiveWeather)) {
                    const double now_s = glfwGetTime();
                    for (auto it = app.audio.pendingThunder.begin();
                         it != app.audio.pendingThunder.end();) {
                        const double delay_s = it->distance_m / 343.0; // speed of sound
                        if (now_s - it->fired_at_seconds >= delay_s) {
                            audioManager->PlayOneShot2D("thunder",
                                                        Luminumbra::Client::BusId::Events);
                            it = app.audio.pendingThunder.erase(it);
                        } else {
                            ++it;
                        }
                    }
                } else {
                    static float s_thunderTimer = 0.0f;
                    if (precip > 0.40f) {
                        s_thunderTimer += 0.5f; // this branch runs once per 0.5 s tick above
                        if (s_thunderTimer >= 22.0f) {
                            s_thunderTimer = 0.0f;
                            audioManager->PlayOneShot2D("thunder",
                                                        Luminumbra::Client::BusId::Events);
                        }
                    } else {
                        s_thunderTimer = 0.0f;
                    }
                }
            }
            // Water: a gentle stream bed when standing water is within ~14 m (a ring probe of
            // the water surface vs terrain). Fades in/out as you approach / leave a river or
            // lake.
            if (auto* ws2 = gameSession->GetWorldSystem()) {
                static const float off[5][2] = {{0, 0}, {14, 0}, {-14, 0}, {0, 14}, {0, -14}};
                bool nearWater = false;
                for (const auto& o : off) {
                    const float wx = pc.x + o[0], wz = pc.z + o[1];
                    if (ws2->WaterLevelAt(wx, wz) > ws2->GetTerrainHeightAt(wx, wz) + 0.4f) {
                        nearWater = true;
                        break;
                    }
                }
                if (nearWater && !s_waterOn) {
                    audioManager->PlayAmbientLoop("ambient_stream", pc, 1.0e6f);
                    s_waterOn = true;
                } else if (!nearWater && s_waterOn) {
                    audioManager->StopAmbientLoop("ambient_stream");
                    s_waterOn = false;
                }
                // waterfall ROAR — the nearest DETECTED waterfall
                // site within earshot drives a positional ambient loop through the
                // previously never-called ComputeWaterfallRoar. The
                // detector's cached sites are the SAME set the render sheets use, so
                // what you hear is what you see. Pure reads; client-only.
                {
                    static bool s_roarOn = false;
                    const auto& falls = renderPipeline.waterfall_sites(*ws2);
                    const Luminumbra::Rendering::WaterfallSite* best = nullptr;
                    float best_d2 = 400.0f * 400.0f;
                    for (const auto& site : falls) {
                        const glm::vec3 d = site.crest - pc;
                        const float d2 = glm::dot(d, d);
                        if (d2 < best_d2) {
                            best_d2 = d2;
                            best = &site;
                        }
                    }
                    if (best != nullptr) {
                        const auto roar =
                            Luminumbra::Client::AudioPropagationSystem::ComputeWaterfallRoar(
                                best->crest, best->drop_height, pc);
                        if (roar.audible) {
                            if (!s_roarOn) {
                                audioManager->PlayAmbientLoop(
                                    "waterfall_roar", best->crest, 400.0f);
                                s_roarOn = true;
                            }
                            audioManager->SetAmbientVolume("waterfall_roar", roar.volume);
                        } else if (s_roarOn) {
                            audioManager->StopAmbientLoop("waterfall_roar");
                            s_roarOn = false;
                        }
                    } else if (s_roarOn) {
                        audioManager->StopAmbientLoop("waterfall_roar");
                        s_roarOn = false;
                    }
                }
            }
            // Wind GUSTS: the wind bed never stops, but its volume breathes with the live
            // wind-field magnitude so a gust is actually felt (calm still whispers).
            if (auto* weather2 = gameSession->GetWeatherSystem()) {
                const auto wsmp = weather2->SampleAt(Luminumbra::Vec3(pc.x, pc.y, pc.z));
                const float windMag =
                    std::sqrt(wsmp.wind.x * wsmp.wind.x + wsmp.wind.y * wsmp.wind.y);
                const float swell = 0.5f + std::min(windMag / 6.0f, 1.0f) * 1.1f; // [0.5.. 1.6]
                audioManager->SetAmbientVolume("ambient_wind", swell);
            }
            // biome reverb base (idempotent), then the weather reverb
            // shift — order matters: UpdateAtmosphere layers on the last base.
            if (auto* wsr = gameSession->GetWorldSystem()) {
                const auto& br = wsr->BiomeReverbAt(pc.x, pc.z);
                envAudio->ApplyBiomeReverb(br.preset, br.wet, br.dry, br.decay);
            }
            if (auto* weather3 = gameSession->GetWeatherSystem()) {
                const auto smp = weather3->SampleAt(Luminumbra::Vec3(pc.x, pc.y, pc.z));
                // WeatherSample.wind is a Vec2 in the world XZ plane:.y -> z.
                envAudio->UpdateAtmosphere(glm::vec3(smp.wind.x, 0.0f, smp.wind.y),
                                           smp.precip_intensity,
                                           smp.storm_intensity);
            }
        }
        // Occasional call from the nearest LIVE creature (<50 m) so the world has voices.
        if (s_callTimer >= 11.0f) {
            s_callTimer = 0.0f;
            const auto& reg = gameSession->GetRegistry();
            auto cview = reg.view<const Luminumbra::Components::CreatureComponent,
                                  const Luminumbra::Components::TransformComponent>();
            entt::entity best = entt::null;
            float bestD = 50.0f * 50.0f;
            glm::vec3 bestPos(0.0f);
            std::uint16_t bestSpecies = 0;
            for (auto e : cview) {
                const auto& cc = cview.get<const Luminumbra::Components::CreatureComponent>(e);
                if (cc.eaten)
                    continue;
                const auto& tf = cview.get<const Luminumbra::Components::TransformComponent>(e);
                const float dx = tf.position.x - pc.x, dz = tf.position.z - pc.z;
                const float d2 = dx * dx + dz * dz;
                if (d2 < bestD) {
                    bestD = d2;
                    best = e;
                    bestSpecies = cc.species_id;
                    bestPos = glm::vec3(tf.position.x, tf.position.y, tf.position.z);
                }
            }
            if (best != entt::null) {
                // Per-species voice: map the species id -> "creature_<id>_call". Every species
                // has a call event in the bank; an unspecified/legacy id falls back to
                // grovestrider.
                std::string ev = "creature_grovestrider_call";
                for (const auto& sp : app.hud.creatureSpecies.all()) {
                    if (sp.species_id() == bestSpecies) {
                        ev = "creature_" + sp.id + "_call";
                        break;
                    }
                }
                audioManager->PlayOneShot(ev, bestPos);
            } else {
                s_callTimer = 8.0f; // nobody near -> check again soon
            }
        }
        // a soft sleeping breath from the nearest SLEEPING creature (<18 m) every
        // ~6 s, so a creature bedded down for the night reads as alive, not frozen. last_action
        // == Sleep (CreatureAction::Sleep = 5). Render-only; one breath at a time stays subtle.
        if (s_sleepTimer >= 6.0f) {
            s_sleepTimer = 0.0f;
            const auto& reg = gameSession->GetRegistry();
            auto sview = reg.view<const Luminumbra::Components::CreatureComponent,
                                  const Luminumbra::Components::TransformComponent>();
            entt::entity best = entt::null;
            float bestD = 18.0f * 18.0f;
            glm::vec3 bestPos(0.0f);
            for (auto e : sview) {
                const auto& cc = sview.get<const Luminumbra::Components::CreatureComponent>(e);
                if (cc.eaten || cc.last_action != 5 /* CreatureAction::Sleep */)
                    continue;
                const auto& tf = sview.get<const Luminumbra::Components::TransformComponent>(e);
                const float dx = tf.position.x - pc.x, dz = tf.position.z - pc.z;
                const float d2 = dx * dx + dz * dz;
                if (d2 < bestD) {
                    bestD = d2;
                    best = e;
                    bestPos = glm::vec3(tf.position.x, tf.position.y, tf.position.z);
                }
            }
            if (best != entt::null)
                audioManager->PlayOneShot("creature_sleep", bestPos);
            else
                s_sleepTimer = 4.0f; // none asleep nearby -> re-check sooner
        }
        // per-action audio: the nearest GRAZING creature (<20 m) emits a
        // soft feed/chew every ~5 s (a drink/sip instead if it is feeding right at the water's
        // edge). last_action == Graze (CreatureAction::Graze = 1). Render-only.
        if (s_feedTimer >= 5.0f) {
            s_feedTimer = 0.0f;
            const auto& reg = gameSession->GetRegistry();
            auto* ws3 = gameSession->GetWorldSystem();
            auto gview = reg.view<const Luminumbra::Components::CreatureComponent,
                                  const Luminumbra::Components::TransformComponent>();
            entt::entity best = entt::null;
            float bestD = 20.0f * 20.0f;
            glm::vec3 bestPos(0.0f);
            for (auto e : gview) {
                const auto& cc = gview.get<const Luminumbra::Components::CreatureComponent>(e);
                if (cc.eaten || cc.last_action != 1 /* CreatureAction::Graze */)
                    continue;
                const auto& tf = gview.get<const Luminumbra::Components::TransformComponent>(e);
                const float dx = tf.position.x - pc.x, dz = tf.position.z - pc.z;
                const float d2 = dx * dx + dz * dz;
                if (d2 < bestD) {
                    bestD = d2;
                    best = e;
                    bestPos = glm::vec3(tf.position.x, tf.position.y, tf.position.z);
                }
            }
            if (best != entt::null) {
                // Drinking proxy: if the grazer is at the water's edge, it sips instead of
                // chews.
                const bool atWater =
                    ws3 && ws3->WaterLevelAt(bestPos.x, bestPos.z) >
                               ws3->GetTerrainHeightAt(bestPos.x, bestPos.z) + 0.2f;
                audioManager->PlayOneShot(atWater ? "creature_drink" : "creature_feed", bestPos);
            } else {
                s_feedTimer = 3.0f; // nobody grazing nearby -> re-check sooner
            }
        }
        // colony bed: a faint chitter from the nearest forager NEST (<25 m)
        // every ~5 s, so an active ant colony reads as alive. Keyed on the colony's shared home
        // cell (every ForagerComponent carries it). Render-only.
        if (s_colonyTimer >= 5.0f) {
            s_colonyTimer = 0.0f;
            const auto& reg = gameSession->GetRegistry();
            auto* ws4 = gameSession->GetWorldSystem();
            auto nview = reg.view<const Luminumbra::Components::ForagerComponent>();
            bool haveNest = false;
            glm::vec3 nestPos(0.0f);
            float bestD = 25.0f * 25.0f;
            for (auto e : nview) {
                const auto& fg = nview.get<const Luminumbra::Components::ForagerComponent>(e);
                const float wx = gameSession->ScentCellToWorldX(fg.home_x);
                const float wz = gameSession->ScentCellToWorldZ(fg.home_z);
                const float dx = wx - pc.x, dz = wz - pc.z;
                const float d2 = dx * dx + dz * dz;
                if (d2 < bestD) {
                    bestD = d2;
                    haveNest = true;
                    const float wy = (ws4 ? ws4->GetTerrainHeightAt(wx, wz) : 0.0f) + 0.3f;
                    nestPos = glm::vec3(wx, wy, wz);
                }
            }
            if (haveNest)
                audioManager->PlayOneShot("creature_colony", nestPos);
            else
                s_colonyTimer = 3.0f;
        }
        // Creature LOCOMOTION sound: grounded creatures near the player tick a soft footfall
        // as they travel (stride-accumulated from real movement, so cadence tracks speed);
        // fliers (corvid/heron/finch/moth) get wingbeats at a longer interval; the small
        // skink gets a light skitter. Render-only.
        {
            static std::unordered_set<std::uint16_t> s_fliers, s_light;
            if (s_fliers.empty()) {
                for (const char* f : {"ashen_corvid", "dusk_heron", "glimmer_finch", "lumen_moth"})
                    s_fliers.insert(Luminumbra::Components::CreatureSpeciesId16(f));
                s_light.insert(Luminumbra::Components::CreatureSpeciesId16("ember_skink"));
            }
            static std::unordered_map<std::uint32_t, std::pair<glm::vec2, float>> s_stride;
            if (s_stride.size() > 512)
                s_stride.clear(); // bound: render-only bookkeeping
            const auto& reg = gameSession->GetRegistry();
            auto fview = reg.view<const Luminumbra::Components::CreatureComponent,
                                  const Luminumbra::Components::TransformComponent>();
            for (auto e : fview) {
                const auto& cc = fview.get<const Luminumbra::Components::CreatureComponent>(e);
                if (cc.eaten)
                    continue;
                const auto& tf = fview.get<const Luminumbra::Components::TransformComponent>(e);
                const float dx = tf.position.x - pc.x, dz = tf.position.z - pc.z;
                if (dx * dx + dz * dz > 35.0f * 35.0f)
                    continue; // only the audible ones
                // Pick the gait sound + stride length by species class.
                const char* gait = "creature_grovestrider_footstep";
                float kStride = 1.7f;
                if (s_fliers.count(cc.species_id)) {
                    gait = "creature_wingbeat";
                    kStride = 3.0f;
                } else if (s_light.count(cc.species_id)) {
                    gait = "creature_footstep_light";
                    kStride = 1.0f;
                }
                const glm::vec2 cur(tf.position.x, tf.position.z);
                const auto key = static_cast<std::uint32_t>(entt::to_integral(e));
                auto it = s_stride.find(key);
                if (it == s_stride.end()) {
                    s_stride.emplace(key, std::make_pair(cur, 0.0f));
                    continue;
                }
                const float moved = glm::distance(cur, it->second.first);
                it->second.first = cur;
                if (moved > 5.0f)
                    continue; // ignore teleport-sized jumps (re-anchor/respawn)
                it->second.second += moved;
                if (it->second.second >= kStride) {
                    it->second.second -= kStride;
                    audioManager->PlayOneShot(
                        gait, glm::vec3(tf.position.x, tf.position.y, tf.position.z));
                }
            }
        }
        // Day/night: a soft cue as the sun crosses the horizon — brightening at dawn, settling
        // at dusk. Sun elevation = -sun_direction.y (the vector points away from the sun).
        // The state only flips once clearly past the horizon, so it fires once per transition.
        {
            static int s_sunUp = -1; // -1 uninit, 0 below horizon, 1 above
            const float elev = -renderPipeline.sun_direction().y;
            const int up = elev > 0.0f ? 1 : 0;
            if (s_sunUp == -1) {
                s_sunUp = up;
            } else if (up != s_sunUp) {
                if (up == 1 && elev > 0.03f) {
                    audioManager->PlayOneShot2D("time_dawn", Luminumbra::Client::BusId::Events);
                    audioManager->PlayMusic("music_exploration");
                    s_sunUp = 1;
                } else if (up == 0 && elev < -0.03f) {
                    audioManager->PlayOneShot2D("time_dusk", Luminumbra::Client::BusId::Events);
                    audioManager->PlayMusic("music_dusk");
                    s_sunUp = 0;
                }
            }
        }
    }
}

} // namespace

void UpdateFrameAudio(ClientAppContext& app,
                      GameState currentState,
                      float deltaTime,
                      Luminumbra::Client::IAudioManager* audioManager,
                      Luminumbra::Client::EnvironmentalAudioSystem* envAudio,
                      Luminumbra::Rendering::Camera* camera,
                      luminumbra::core::SystemConfig& systemConfig,
                      Luminumbra::world::GameSession* gameSession,
                      Luminumbra::Rendering::RenderPipeline& renderPipeline,
                      const ScenarioHarness::RuntimeScenarioConfig& scenario_config) {
    UpdateListenerEnvironment(
        currentState, deltaTime, audioManager, envAudio, camera, gameSession, renderPipeline);
    UpdatePlayerFootsteps(
        app, currentState, deltaTime, audioManager, camera, gameSession, scenario_config);
    UpdateLivingWorldAudio(app,
                           currentState,
                           deltaTime,
                           audioManager,
                           envAudio,
                           camera,
                           systemConfig,
                           gameSession,
                           renderPipeline,
                           scenario_config);
}

} // namespace Luminumbra::Client::App
