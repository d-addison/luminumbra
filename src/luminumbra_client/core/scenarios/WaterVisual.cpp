#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>

#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h"
#include "core/scenarios/ScenarioCommon.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h" //  species base_color -> creature tint
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CreatureProcgen.h" //  genome -> body-proportion build
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "rendering/Camera.h"
#include "rendering/LightningBolt.h"
#include "rendering/passes/FoliagePass.h"
#include "rendering/passes/ParticlePass.h"
// lockstep transport seam (engine-generic; ILockstepTransport +
// LoopbackTransport + LockstepSession). Named SendFrame/TryReceiveFrame to dodge
// the <windows.h> SendMessage macro (see LockstepSession.h note).
#include "luminumbra_common/net/LockstepSession.h"
//  (AU1): atmosphere audio telemetry. The harness sweeps the replicated
// weather/wind state through the REAL EnvironmentalAudioSystem atmosphere model +
// the AudioPropagationSystem ambience bed and emits the AtmosphereAudio artifact.
// Client-side dressing only -- no world_hash, no visual-gate dependency.
#include "audio/AudioPropagationSystem.h"
#include "audio/EnvironmentalAudioSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace Luminumbra::Client::ScenarioHarness {

namespace {

// Fills in both camera framings for the water visual scenario:
// - camera_position: the original top-down view (pitch ~ -73 deg) used for
//   the main capture, the caustics-animation samples, and all v1 pixel gates.
// - reflection_camera_position: a grazing view (pitch ~ -18 deg)
//   facing the azimuth with the most open water beyond the focus. Looking
//   almost straight down the fresnel term bottoms out and reflections are
//   invisible, so the SSR/sky-correlation gate captures from this framing
//   late in the run instead.
void FrameWaterVisualCameras(Luminumbra::Systems::SHIELD_WorldSystem* world_system,
                             WaterVisualCameraTarget& target) {
    const Luminumbra::Vec3 camera_offset(0.0f, 80.0f, 24.0f);
    target.camera_position = target.focus + camera_offset;
    target.camera_terrain_height =
        world_system->GetTerrainHeightAt(target.camera_position.x, target.camera_position.z);
    target.camera_position.y =
        std::max(target.camera_position.y, target.camera_terrain_height + 32.0f);

    constexpr float kReflectionDistance = 56.0f;
    constexpr float kReflectionHeight = 18.0f;
    constexpr int kAzimuthCount = 16;
    constexpr float kProbeStep = 16.0f;
    constexpr int kProbeSamples = 14; // out to ~224 m beyond the focus

    float best_score = -1.0f;
    Luminumbra::Vec3 best_dir(0.0f, 0.0f, 1.0f);
    for (int a = 0; a < kAzimuthCount; ++a) {
        constexpr float kTwoPi = 6.28318530718f;
        const float angle = (kTwoPi * static_cast<float>(a)) / static_cast<float>(kAzimuthCount);
        const Luminumbra::Vec3 dir(std::cos(angle), 0.0f, std::sin(angle));
        float score = 0.0f;
        for (int s = 1; s <= kProbeSamples; ++s) {
            const Luminumbra::Vec3 probe =
                target.focus + dir * (kProbeStep * static_cast<float>(s));
            const float water_depth =
                Luminumbra::SEA_LEVEL - world_system->GetTerrainHeightAt(probe.x, probe.z);
            if (water_depth > 1.0f) {
                score += 1.0f;
            }
        }
        if (score > best_score) {
            best_score = score;
            best_dir = dir;
        }
    }

    target.reflection_camera_position = target.focus - best_dir * kReflectionDistance +
                                        Luminumbra::Vec3(0.0f, kReflectionHeight, 0.0f);
    const float reflection_terrain = world_system->GetTerrainHeightAt(
        target.reflection_camera_position.x, target.reflection_camera_position.z);
    target.reflection_camera_position.y =
        std::max(target.reflection_camera_position.y, reflection_terrain + 12.0f);

    // locate the nearest shallow-tint (0.8-1.6 m), foam-band
    // (0.25-0.6 m) and deep (>= 3 m) water-surface points around the focus.
    // All are projected into the top-down capture to gate the depth tint
    // gradient and the shoreline foam band. The 64 m search radius stays
    // inside the top-down camera footprint.
    constexpr int kDepthSearchRadius = 64;
    constexpr int kDepthSearchStep = 2;
    float best_shallow_distance_sq = std::numeric_limits<float>::max();
    float best_foam_distance_sq = std::numeric_limits<float>::max();
    float best_deep_distance_sq = std::numeric_limits<float>::max();
    for (int dz = -kDepthSearchRadius; dz <= kDepthSearchRadius; dz += kDepthSearchStep) {
        for (int dx = -kDepthSearchRadius; dx <= kDepthSearchRadius; dx += kDepthSearchStep) {
            const float x = target.focus.x + static_cast<float>(dx);
            const float z = target.focus.z + static_cast<float>(dz);
            const float water_depth =
                Luminumbra::SEA_LEVEL - world_system->GetTerrainHeightAt(x, z);
            const float distance_sq = static_cast<float>(dx * dx + dz * dz);
            if (water_depth >= 0.8f && water_depth <= 1.6f &&
                distance_sq < best_shallow_distance_sq) {
                best_shallow_distance_sq = distance_sq;
                target.shallow_point = Luminumbra::Vec3(x, Luminumbra::SEA_LEVEL, z);
                target.shallow_point_found = true;
            }
            if (water_depth >= 0.25f && water_depth <= 0.6f &&
                distance_sq < best_foam_distance_sq) {
                best_foam_distance_sq = distance_sq;
                target.foam_point = Luminumbra::Vec3(x, Luminumbra::SEA_LEVEL, z);
                target.foam_point_found = true;
            }
            if (water_depth >= 3.0f && distance_sq < best_deep_distance_sq) {
                best_deep_distance_sq = distance_sq;
                target.deep_point = Luminumbra::Vec3(x, Luminumbra::SEA_LEVEL, z);
                target.deep_point_found = true;
            }
        }
    }
}

} // namespace

WaterVisualCameraTarget FindWaterVisualCameraTarget(Luminumbra::world::GameSession* game_session) {
    WaterVisualCameraTarget target;
    if (!game_session || !game_session->GetWorldSystem()) {
        return target;
    }

    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;

    const auto renderable_chunks = world_system->get_renderable_chunks();
    float best_mesh_score = 0.0f;
    for (const Luminumbra::Chunk* chunk : renderable_chunks) {
        if (!chunk || chunk->water_mesh_vertices.empty() || chunk->water_mesh_indices.empty()) {
            continue;
        }

        const Luminumbra::Vec3 chunk_origin =
            Luminumbra::Vec3(chunk->get_coords() * Luminumbra::IVec3(Luminumbra::CHUNK_SIZE_X,
                                                                     Luminumbra::CHUNK_SIZE_Y,
                                                                     Luminumbra::CHUNK_SIZE_Z));
        Luminumbra::Vec3 accumulated_position(0.0f);
        const std::size_t sample_count =
            std::min<std::size_t>(chunk->water_mesh_vertices.size(), 256u);
        const std::size_t stride =
            std::max<std::size_t>(1u, chunk->water_mesh_vertices.size() / sample_count);
        std::size_t collected = 0;
        for (std::size_t i = 0; i < chunk->water_mesh_vertices.size() && collected < sample_count;
             i += stride) {
            accumulated_position += chunk_origin + chunk->water_mesh_vertices[i].position;
            ++collected;
        }
        if (collected == 0) {
            continue;
        }

        const Luminumbra::Vec3 focus = accumulated_position / static_cast<float>(collected);
        const float distance = glm::length(Luminumbra::Vec3(focus.x, spawn.y, focus.z) -
                                           Luminumbra::Vec3(spawn.x, spawn.y, spawn.z));
        int open_water_samples = 0;
        float min_water_depth = std::numeric_limits<float>::max();
        for (int oz = -2; oz <= 2; ++oz) {
            for (int ox = -2; ox <= 2; ++ox) {
                const float sample_x = focus.x + static_cast<float>(ox * 16);
                const float sample_z = focus.z + static_cast<float>(oz * 16);
                const float water_depth =
                    Luminumbra::SEA_LEVEL - world_system->GetTerrainHeightAt(sample_x, sample_z);
                min_water_depth = std::min(min_water_depth, water_depth);
                if (water_depth > 1.0f) {
                    ++open_water_samples;
                }
            }
        }
        if (open_water_samples < 20) {
            continue;
        }
        const float score =
            static_cast<float>(std::min<std::size_t>(chunk->water_mesh_indices.size(), 20000u)) *
                0.01f +
            static_cast<float>(open_water_samples) * 250.0f +
            std::max(0.0f, min_water_depth) * 5.0f - distance * 0.02f;
        if (!target.found || score > best_mesh_score) {
            target.found = true;
            target.focus = focus + Luminumbra::Vec3(0.0f, 0.1f, 0.0f);
            target.terrain_height = world_system->GetTerrainHeightAt(focus.x, focus.z);
            target.supporting_water_samples = open_water_samples;
            best_mesh_score = score;
        }
    }

    if (target.found) {
        FrameWaterVisualCameras(world_system, target);
        return target;
    }

    constexpr int kSearchRadius = 512;
    constexpr int kSearchStep = 16;
    constexpr float kMinWaterDepth = 0.5f;
    float best_score = 0.0f;

    for (int dz = -kSearchRadius; dz <= kSearchRadius; dz += kSearchStep) {
        for (int dx = -kSearchRadius; dx <= kSearchRadius; dx += kSearchStep) {
            const float x = spawn.x + static_cast<float>(dx);
            const float z = spawn.z + static_cast<float>(dz);
            const float terrain_height = world_system->GetTerrainHeightAt(x, z);
            const float water_depth = Luminumbra::SEA_LEVEL - terrain_height;
            if (water_depth < kMinWaterDepth) {
                continue;
            }

            int supporting_water_samples = 0;
            for (int oz = -2; oz <= 2; ++oz) {
                for (int ox = -2; ox <= 2; ++ox) {
                    const float sx = x + static_cast<float>(ox * kSearchStep);
                    const float sz = z + static_cast<float>(oz * kSearchStep);
                    if (world_system->GetTerrainHeightAt(sx, sz) < Luminumbra::SEA_LEVEL - 0.25f) {
                        ++supporting_water_samples;
                    }
                }
            }
            if (supporting_water_samples < 20) {
                continue;
            }

            const float distance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
            const float score = static_cast<float>(supporting_water_samples) * 100.0f +
                                water_depth * 10.0f - distance * 0.02f;
            if (!target.found || score > best_score) {
                target.found = true;
                target.focus = Luminumbra::Vec3(x, Luminumbra::SEA_LEVEL + 0.1f, z);
                target.terrain_height = terrain_height;
                target.supporting_water_samples = supporting_water_samples;
                best_score = score;
            }
        }
    }

    if (!target.found) {
        return target;
    }

    FrameWaterVisualCameras(world_system, target);
    return target;
}

void AimCameraAt(Luminumbra::Rendering::Camera* camera, const Luminumbra::Vec3& focus) {
    if (!camera) {
        return;
    }

    const glm::vec3 direction = glm::normalize(focus - camera->Position);
    camera->Yaw = glm::degrees(std::atan2(direction.z, direction.x));
    camera->Pitch = glm::degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
    camera->updateCameraVectors();
}

void ApplyWaterVisualCamera(Luminumbra::Rendering::Camera* camera,
                            const WaterVisualCameraTarget& target) {
    if (!camera || !target.found) {
        return;
    }

    camera->Position = target.camera_position;
    AimCameraAt(camera, target.focus);
}

void ApplyWaterReflectionCamera(Luminumbra::Rendering::Camera* camera,
                                const WaterVisualCameraTarget& target) {
    if (!camera || !target.found) {
        return;
    }

    camera->Position = target.reflection_camera_position;
    AimCameraAt(camera, target.focus);
}

} // namespace Luminumbra::Client::ScenarioHarness
