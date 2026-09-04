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

double SmoothStep01(double value) {
    const double t = std::clamp(value, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

Luminumbra::Vec3 CalculateLodGroundCameraPosition(const RuntimeScenarioConfig& config,
                                                  Luminumbra::world::GameSession* game_session,
                                                  double elapsed_seconds) {
    const Luminumbra::Vec3 spawn =
        game_session ? game_session->GetMetadata().spawnPoint : Luminumbra::Vec3(0.0f);
    auto* world_system = game_session ? game_session->GetWorldSystem() : nullptr;
    const double duration = static_cast<double>(std::max(1, config.timed_run_seconds));
    const double normalized = std::clamp(elapsed_seconds / duration, 0.0, 1.0);
    const double travel = SmoothStep01((normalized - 0.12) / 0.58);

    constexpr double kPi = 3.14159265358979323846;
    const float x = spawn.x + static_cast<float>(travel * 240.0);
    const float z = spawn.z + static_cast<float>(std::sin(travel * kPi * 1.5) * 64.0);
    const float terrain_height = world_system ? world_system->GetTerrainHeightAt(x, z) : spawn.y;
    const float y = terrain_height + 180.0f;
    return Luminumbra::Vec3(x, y, z);
}

Luminumbra::Vec3 CalculateLodGroundFocusPosition(Luminumbra::world::GameSession* game_session,
                                                 const Luminumbra::Vec3& camera_position,
                                                 double elapsed_seconds) {
    auto* world_system = game_session ? game_session->GetWorldSystem() : nullptr;
    const float yaw_degrees = -72.0f + static_cast<float>(std::sin(elapsed_seconds * 0.35) * 10.0);
    const float yaw_radians = glm::radians(yaw_degrees);
    const Luminumbra::Vec3 forward(std::cos(yaw_radians), 0.0f, std::sin(yaw_radians));
    const Luminumbra::Vec3 focus_xz = camera_position + forward * 120.0f;
    const float terrain_height = world_system
                                     ? world_system->GetTerrainHeightAt(focus_xz.x, focus_xz.z)
                                     : camera_position.y - 180.0f;
    return Luminumbra::Vec3(focus_xz.x, terrain_height, focus_xz.z);
}

void ApplyLodGroundCameraPath(const RuntimeScenarioConfig& config,
                              Luminumbra::world::GameSession* game_session,
                              Luminumbra::Rendering::Camera* camera,
                              double elapsed_seconds) {
    if (!camera || !game_session) {
        return;
    }

    camera->Position = CalculateLodGroundCameraPosition(config, game_session, elapsed_seconds);
    const Luminumbra::Vec3 focus =
        CalculateLodGroundFocusPosition(game_session, camera->Position, elapsed_seconds);
    const glm::vec3 direction = glm::normalize(focus - camera->Position);
    camera->Yaw = glm::degrees(std::atan2(direction.z, direction.x));
    camera->Pitch = glm::degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
    camera->updateCameraVectors();
}

} // namespace Luminumbra::Client::ScenarioHarness
