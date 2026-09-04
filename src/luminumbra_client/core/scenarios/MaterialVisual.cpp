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

// True when the column at (x, z) sits on a steep high-altitude rim. The
// marching-cubes skin follows the height graph (depth ~0), so soil/stone
// (depth 1-5 / >= 5 per classify_material) only become visible where vertex
// interpolation error is large: along the rims of grass-topped cliffs, where
// the height field drops several meters within one cell. The cave field is
// surface-capped (kCaveSurfaceCapDepth = 18) and never opens to the sky, so
// rims are the only reliable natural stone/soil exposure. Rims face upward
// and catch the near-zenith noon sun.
bool ColumnOnSteepRim(Luminumbra::Systems::SHIELD_WorldSystem* world_system, float x, float z) {
    const float column_height = world_system->GetTerrainHeightAt(x, z);
    if (column_height < 37.0f) {
        return false;
    }
    constexpr float kStep = 6.0f;
    constexpr float kMinDrop = 6.0f;
    const float drops[4] = {
        column_height - world_system->GetTerrainHeightAt(x + kStep, z),
        column_height - world_system->GetTerrainHeightAt(x - kStep, z),
        column_height - world_system->GetTerrainHeightAt(x, z + kStep),
        column_height - world_system->GetTerrainHeightAt(x, z - kStep),
    };
    for (const float drop : drops) {
        if (drop >= kMinDrop) {
            return true;
        }
    }
    return false;
}

// Camera/focus composition shared by candidate evaluation and the final
// target: camera seaward of the beach looking up-slope, focus partway up the
// highland flank.
struct MaterialVantage {
    Luminumbra::Vec3 camera{0.0f};
    Luminumbra::Vec3 focus{0.0f};
};

MaterialVantage ComposeMaterialVantage(Luminumbra::Systems::SHIELD_WorldSystem* world_system,
                                       const Luminumbra::Vec3& beach,
                                       const Luminumbra::Vec3& highland) {
    Luminumbra::Vec3 slope_dir(highland.x - beach.x, 0.0f, highland.z - beach.z);
    const float slope_len = std::sqrt(slope_dir.x * slope_dir.x + slope_dir.z * slope_dir.z);
    if (slope_len > 0.01f) {
        slope_dir.x /= slope_len;
        slope_dir.z /= slope_len;
    } else {
        slope_dir = Luminumbra::Vec3(0.0f, 0.0f, 1.0f);
    }

    MaterialVantage vantage;
    vantage.focus = Luminumbra::Vec3(beach.x + slope_dir.x * slope_len * 0.45f,
                                     beach.y + (highland.y - beach.y) * 0.35f,
                                     beach.z + slope_dir.z * slope_len * 0.45f);
    vantage.camera = beach - slope_dir * 46.0f;
    vantage.camera.y = beach.y + 22.0f;
    const float camera_terrain =
        world_system->GetTerrainHeightAt(vantage.camera.x, vantage.camera.z);
    vantage.camera.y = std::max(vantage.camera.y, camera_terrain + 14.0f);
    return vantage;
}

// The camera must actually see the flank instead of staring into an
// intervening hillside (which captures as a near-black unlit wall or a
// texture-magnified flat close-up). Check the center ray to the focus plus
// rays swung +/-35 degrees toward the frame edges; each samples the
// camera-side stretch of the segment and requires clearance above the height
// field. The height field is the cave-uncarved graph top, so a clear path
// here is a conservative occlusion proxy.
bool MaterialVantageHasLineOfSight(Luminumbra::Systems::SHIELD_WorldSystem* world_system,
                                   const MaterialVantage& vantage) {
    const Luminumbra::Vec3 ray = vantage.focus - vantage.camera;
    constexpr float kEdgeAngles[] = {-0.61f, 0.0f, 0.61f}; // ~35 degrees
    for (const float angle : kEdgeAngles) {
        const float cos_a = std::cos(angle);
        const float sin_a = std::sin(angle);
        const Luminumbra::Vec3 swung(
            ray.x * cos_a - ray.z * sin_a, ray.y, ray.x * sin_a + ray.z * cos_a);
        // Side rays do not end on the flank, so only their nearer stretch must
        // be clear; the center ray is enforced almost to the focus.
        const float max_t = angle == 0.0f ? 0.7f : 0.5f;
        for (int step = 0; step <= 7; ++step) {
            const float t = static_cast<float>(step) * 0.1f;
            if (t > max_t) {
                break;
            }
            const Luminumbra::Vec3 pos = vantage.camera + swung * t;
            if (pos.y < world_system->GetTerrainHeightAt(pos.x, pos.z) + 2.0f) {
                return false;
            }
        }
    }
    return true;
}

// Material visual targets reuse the water target shape: a focus point, a
// raised camera position, and a count of supporting samples that prove the
// surrounding area really is the expected material.
//
// The vantage is a composite designed to show several terrain materials in a
// single capture (one frame, one ROI): a beach (sand band per
// GetTerrainMaterialAt: dry terrain near sea level) in the foreground with a
// grass-topped highland (terrain comfortably above the y<34/terrain<36 sand
// band) rising behind it. The camera sits seaward of the beach, raised, and
// aims up-slope at the highland so the bottom-3/5 analysis ROI contains
// foreground sand, the highland's cliff flank (soil/stone exposure), and the
// grass top.
WaterVisualCameraTarget
FindMaterialVisualCameraTarget(Luminumbra::world::GameSession* game_session) {
    WaterVisualCameraTarget target;
    if (!game_session || !game_session->GetWorldSystem()) {
        return target;
    }

    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;

    constexpr float kBeachMinHeight = 0.25f; // above SEA_LEVEL offset
    constexpr float kBeachMaxHeight = 12.0f; // comfortably inside the <36 beach band
    // classify_material assigns Grass only above the sand band (terrain >= 36
    // with depth < 1); 38 keeps a margin so noise jitter cannot flip the top
    // back into the sand classification.
    constexpr float kGrassMinTerrain = 38.0f;
    constexpr int kSearchRadius = 512;
    constexpr int kSearchStep = 8;
    float best_score = -std::numeric_limits<float>::max();
    std::size_t in_band_candidates = 0;

    Luminumbra::Vec3 best_beach{0.0f};
    Luminumbra::Vec3 best_highland{0.0f};
    int best_grass_support = 0;
    int best_stone_support = 0;

    for (int dz = -kSearchRadius; dz <= kSearchRadius; dz += kSearchStep) {
        for (int dx = -kSearchRadius; dx <= kSearchRadius; dx += kSearchStep) {
            const float x = spawn.x + static_cast<float>(dx);
            const float z = spawn.z + static_cast<float>(dz);
            const float terrain_height = world_system->GetTerrainHeightAt(x, z);
            const float height_above_sea = terrain_height - Luminumbra::SEA_LEVEL;
            if (height_above_sea < kBeachMinHeight || height_above_sea > kBeachMaxHeight) {
                continue;
            }
            ++in_band_candidates;

            int supporting_sand_samples = 0;
            for (int oz = -2; oz <= 2; ++oz) {
                for (int ox = -2; ox <= 2; ++ox) {
                    const float sx = x + static_cast<float>(ox * 6);
                    const float sz = z + static_cast<float>(oz * 6);
                    const float sample_height =
                        world_system->GetTerrainHeightAt(sx, sz) - Luminumbra::SEA_LEVEL;
                    if (sample_height >= kBeachMinHeight && sample_height <= kBeachMaxHeight) {
                        ++supporting_sand_samples;
                    }
                }
            }
            if (supporting_sand_samples < 8) {
                continue;
            }

            // Highland scan: walk rings of directions around the beach point
            // and keep the tallest sample; the grass gate needs terrain that
            // actually rises above the sand band within camera range.
            float highland_height = -std::numeric_limits<float>::max();
            Luminumbra::Vec3 highland_pos{0.0f};
            for (int ring = 1; ring <= 4; ++ring) {
                const float radius = static_cast<float>(ring) * 28.0f;
                for (int dir = 0; dir < 8; ++dir) {
                    const float angle = static_cast<float>(dir) * 0.78539816f; // pi/4
                    const float hx = x + std::cos(angle) * radius;
                    const float hz = z + std::sin(angle) * radius;
                    const float h = world_system->GetTerrainHeightAt(hx, hz);
                    if (h > highland_height) {
                        highland_height = h;
                        highland_pos = Luminumbra::Vec3(hx, h, hz);
                    }
                }
            }
            if (highland_height < kGrassMinTerrain) {
                continue;
            }

            // Grass support: flat-top samples around the highland that stay
            // above the grass floor keep depth < 1 across the visible cap.
            int grass_support = 0;
            for (int oz = -1; oz <= 1; ++oz) {
                for (int ox = -1; ox <= 1; ++ox) {
                    const float gx = highland_pos.x + static_cast<float>(ox * 10);
                    const float gz = highland_pos.z + static_cast<float>(oz * 10);
                    if (world_system->GetTerrainHeightAt(gx, gz) >= kGrassMinTerrain) {
                        ++grass_support;
                    }
                }
            }
            if (grass_support < 4) {
                continue;
            }

            // Stone/soil rim exposure: sample around the highland cap for
            // steep rim columns. No sun-alignment term: rims face upward and
            // are zenith-lit regardless of flank orientation.
            int stone_support = 0;
            for (int oz = -2; oz <= 2; ++oz) {
                for (int ox = -2; ox <= 2; ++ox) {
                    const float sx = highland_pos.x + static_cast<float>(ox * 8);
                    const float sz = highland_pos.z + static_cast<float>(oz * 8);
                    if (ColumnOnSteepRim(world_system, sx, sz)) {
                        ++stone_support;
                    }
                }
            }
            if (stone_support < 1) {
                continue;
            }

            const float distance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
            const float score = static_cast<float>(supporting_sand_samples) * 220.0f +
                                static_cast<float>(grass_support) * 150.0f +
                                static_cast<float>(stone_support) * 60.0f +
                                std::min(highland_height, 70.0f) * 4.0f - distance * 0.05f;
            if (target.found && score <= best_score) {
                continue;
            }

            const Luminumbra::Vec3 beach(x, terrain_height, z);
            if (!MaterialVantageHasLineOfSight(
                    world_system, ComposeMaterialVantage(world_system, beach, highland_pos))) {
                continue;
            }

            target.found = true;
            best_beach = beach;
            best_highland = highland_pos;
            best_grass_support = grass_support;
            best_stone_support = stone_support;
            target.terrain_height = terrain_height;
            target.supporting_water_samples = supporting_sand_samples;
            best_score = score;
        }
    }

    LUMINUMBRA_CORE_INFO("Material visual target scan: in_band_candidates={}, found={}, "
                         "beach=({:.1f},{:.1f},{:.1f}), highland=({:.1f},{:.1f},{:.1f}), "
                         "sand_support={}, grass_support={}, stone_support={}",
                         in_band_candidates,
                         target.found,
                         best_beach.x,
                         best_beach.y,
                         best_beach.z,
                         best_highland.x,
                         best_highland.y,
                         best_highland.z,
                         target.supporting_water_samples,
                         best_grass_support,
                         best_stone_support);

    if (!target.found) {
        return target;
    }

    // Camera seaward of the beach looking up-slope: focus partway up the
    // highland flank so the frame stacks foreground beach sand in the lower
    // ROI, the slope (grass/soil/stone) in the middle, and keeps the horizon
    // and sky above the analysis ROI.
    const MaterialVantage vantage = ComposeMaterialVantage(world_system, best_beach, best_highland);
    target.focus = vantage.focus;
    target.camera_position = vantage.camera;
    target.camera_terrain_height =
        world_system->GetTerrainHeightAt(target.camera_position.x, target.camera_position.z);
    return target;
}

} // namespace Luminumbra::Client::ScenarioHarness
