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
#include <unordered_set>

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

// Material-keyed player footsteps.
void UpdatePlayerFootsteps(ClientAppContext& app,
                           FrameAudioState& audio,
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
        const glm::vec3 fp = camera->Position;
        if (audio.PlayerFootstep(fp, deltaTime)) {
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

// Living-world ambience: rain/stream/waterfall/wind beds, creature
// calls/sleep/feeding/colony/locomotion, and the dawn/dusk cues.
void UpdateLivingWorldAudio(ClientAppContext& app,
                            FrameAudioState& audio,
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
        audio.AdvanceTimers(deltaTime);
        const glm::vec3 pc = camera->Position;
        if (audio.envTimer >= 0.5f) {
            audio.envTimer = 0.0f;
            // Rain: tie ambient_rain to the live weather precipitation at the player
            // (hysteresis so it doesn't flutter at a storm-cell edge). Same field the foliage
            // growth reads.
            if (auto* weather = gameSession->GetWeatherSystem()) {
                const float precip = weather->PrecipitationAt(Luminumbra::Vec3(pc.x, pc.y, pc.z));
                if (!audio.rainOn && precip > 0.18f) {
                    audioManager->PlayAmbientLoop("ambient_rain", pc, 1.0e6f);
                    audio.rainOn = true;
                } else if (audio.rainOn && precip < 0.08f) {
                    audioManager->StopAmbientLoop("ambient_rain");
                    audio.rainOn = false;
                }
                // thunder now follows the SIM strike schedule when
                // the live-weather bridge is on — each queued strike cue fires after
                // its physical sound delay (distance / 343 m/s), volume by
                // magnitude/distance. The legacy flat 22 s timer remains ONLY as the
                // fallback when the bridge is off (no schedule consumer running).
                if (systemConfig.enabled(luminumbra::core::SysKey::RenderLiveWeather)) {
                    const double now_s = glfwGetTime();
                    for (auto it = audio.pendingThunder.begin();
                         it != audio.pendingThunder.end();) {
                        const double delay_s = it->distance_m / 343.0; // speed of sound
                        if (now_s - it->fired_at_seconds >= delay_s) {
                            audioManager->PlayOneShot2D(
                                "thunder",
                                Luminumbra::Client::BusId::Events,
                                Luminumbra::Client::ThunderCueGain(it->magnitude, it->distance_m));
                            it = audio.pendingThunder.erase(it);
                        } else {
                            ++it;
                        }
                    }
                } else {
                    if (precip > 0.40f) {
                        audio.thunderTimer += 0.5f; // this branch runs once per 0.5 s tick above
                        if (audio.thunderTimer >= 22.0f) {
                            audio.thunderTimer = 0.0f;
                            audioManager->PlayOneShot2D("thunder",
                                                        Luminumbra::Client::BusId::Events);
                        }
                    } else {
                        audio.thunderTimer = 0.0f;
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
                if (nearWater && !audio.waterOn) {
                    audioManager->PlayAmbientLoop("ambient_stream", pc, 1.0e6f);
                    audio.waterOn = true;
                } else if (!nearWater && audio.waterOn) {
                    audioManager->StopAmbientLoop("ambient_stream");
                    audio.waterOn = false;
                }
                // The nearest live baked sheet drives the roar, using the last
                // prepared frame's water/support factors without another survey.
                {
                    auto& roarOn = audio.waterfallRoarOn;
                    auto& roarPosition = audio.waterfallRoarPosition;
                    const auto source = renderPipeline.waterfall_roar_source(pc);
                    const auto* best = source.site;
                    if (best != nullptr) {
                        const auto roar =
                            Luminumbra::Client::AudioPropagationSystem::ComputeWaterfallRoar(
                                best->crest, best->drop_height, pc);
                        if (roar.audible) {
                            if (!roarOn || roarPosition != best->crest) {
                                if (roarOn) {
                                    audioManager->StopAmbientLoop("waterfall_roar");
                                }
                                audioManager->PlayAmbientLoop(
                                    "waterfall_roar", best->crest, 400.0f);
                                roarOn = true;
                                roarPosition = best->crest;
                            }
                            audioManager->SetAmbientVolume("waterfall_roar",
                                                           source.gain(roar.volume));
                        } else if (roarOn) {
                            audioManager->StopAmbientLoop("waterfall_roar");
                            roarOn = false;
                        }
                    } else if (roarOn) {
                        audioManager->StopAmbientLoop("waterfall_roar");
                        roarOn = false;
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
        if (audio.callTimer >= 11.0f) {
            audio.callTimer = 0.0f;
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
                audio.callTimer = 8.0f; // nobody near -> check again soon
            }
        }
        // a soft sleeping breath from the nearest SLEEPING creature (<18 m) every
        // ~6 s, so a creature bedded down for the night reads as alive, not frozen. last_action
        // == Sleep (CreatureAction::Sleep = 5). Render-only; one breath at a time stays subtle.
        if (audio.sleepTimer >= 6.0f) {
            audio.sleepTimer = 0.0f;
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
                audio.sleepTimer = 4.0f; // none asleep nearby -> re-check sooner
        }
        // per-action audio: the nearest GRAZING creature (<20 m) emits a
        // soft feed/chew every ~5 s (a drink/sip instead if it is feeding right at the water's
        // edge). last_action == Graze (CreatureAction::Graze = 1). Render-only.
        if (audio.feedTimer >= 5.0f) {
            audio.feedTimer = 0.0f;
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
                audio.feedTimer = 3.0f; // nobody grazing nearby -> re-check sooner
            }
        }
        // colony bed: a faint chitter from the nearest forager NEST (<25 m)
        // every ~5 s, so an active ant colony reads as alive. Keyed on the colony's shared home
        // cell (every ForagerComponent carries it). Render-only.
        if (audio.colonyTimer >= 5.0f) {
            audio.colonyTimer = 0.0f;
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
                audio.colonyTimer = 3.0f;
        }
        // Creature LOCOMOTION sound: grounded creatures near the player tick a soft footfall
        // as they travel (stride-accumulated from real movement, so cadence tracks speed);
        // fliers (corvid/heron/finch/moth) get wingbeats at a longer interval; the small
        // skink gets a light skitter. Render-only.
        {
            static const std::unordered_set<std::uint16_t> fliers{
                Luminumbra::Components::CreatureSpeciesId16("ashen_corvid"),
                Luminumbra::Components::CreatureSpeciesId16("dusk_heron"),
                Luminumbra::Components::CreatureSpeciesId16("glimmer_finch"),
                Luminumbra::Components::CreatureSpeciesId16("lumen_moth")};
            static const std::unordered_set<std::uint16_t> light{
                Luminumbra::Components::CreatureSpeciesId16("ember_skink")};
            audio.BeginCreatureFrame();
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
                if (fliers.count(cc.species_id)) {
                    gait = "creature_wingbeat";
                    kStride = 3.0f;
                } else if (light.count(cc.species_id)) {
                    gait = "creature_footstep_light";
                    kStride = 1.0f;
                }
                const glm::vec2 cur(tf.position.x, tf.position.z);
                const auto key = static_cast<std::uint32_t>(entt::to_integral(e));
                if (audio.CreatureFootfall(key, cur, kStride)) {
                    audioManager->PlayOneShot(
                        gait, glm::vec3(tf.position.x, tf.position.y, tf.position.z));
                }
            }
        }
        // Day/night: a soft cue as the sun crosses the horizon — brightening at dawn, settling
        // at dusk. Sun elevation = -sun_direction.y (the vector points away from the sun).
        // The state only flips once clearly past the horizon, so it fires once per transition.
        {
            const float elev = -renderPipeline.sun_direction().y;
            const int up = elev > 0.0f ? 1 : 0;
            if (audio.sunUp == -1) {
                audio.sunUp = up;
            } else if (up != audio.sunUp) {
                if (up == 1 && elev > 0.03f) {
                    audioManager->PlayOneShot2D("time_dawn", Luminumbra::Client::BusId::Events);
                    audioManager->PlayMusic("music_exploration");
                    audio.sunUp = 1;
                } else if (up == 0 && elev < -0.03f) {
                    audioManager->PlayOneShot2D("time_dusk", Luminumbra::Client::BusId::Events);
                    audioManager->PlayMusic("music_dusk");
                    audio.sunUp = 0;
                }
            }
        }
    }
}

} // namespace

void UpdateFrameAudio(ClientAppContext& app,
                      FrameAudioState& audio,
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
        app, audio, currentState, deltaTime, audioManager, camera, gameSession, scenario_config);
    UpdateLivingWorldAudio(app,
                           audio,
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
