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

// --- Waterfall visual (, ) ---

void WriteWaterfallVisualAnalysis(const std::filesystem::path& artifact_dir,
                                  const std::string& waterfall_screenshot,
                                  const WaterfallVisualResult& result) {
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();

    // Determinism (regression review): same world -> byte-equal sites, same seed (a
    // separate world) -> the same site set/hash. This is the load-bearing gate.
    const bool determinism_passed = result.determinism_byte_equal && result.same_seed_same_sites &&
                                    result.site_hash_run_a == result.site_hash_run_b &&
                                    result.site_count > 0;

    // Dressing: when a capture was taken it must show the falling sheet body,
    // the plunge/crest foam, and the spray plume. When GL is unavailable the
    // dressing assertion is vacuously satisfied (audio/visual is optional
    // dressing; the determinism contract still gates).
    const bool dressing_passed =
        !result.capture_written ||
        (result.cascade_pixels > 0 && result.foam_pixels > 0 && result.spray_pixels > 0);

    const bool passed = determinism_passed && dressing_passed && gl_debug.errors == 0;

    nlohmann::json artifact = {
        {"schema", "luminumbra.waterfall_visual.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"waterfall_screenshot", waterfall_screenshot},
        {"world_seed", result.world_seed},
        {"site_count", result.site_count},
        {"determinism",
         {{"passed", determinism_passed},
          {"site_hash_run_a", result.site_hash_run_a},
          {"site_hash_run_b", result.site_hash_run_b},
          {"byte_equal", result.determinism_byte_equal},
          {"same_seed_same_sites", result.same_seed_same_sites},
          {"contract",
           "pure_function(world river course x heightfield); cached per region; never hashed"}}},
        {"best_site",
         {{"drop_height", result.best_drop_height}, {"steepness", result.best_steepness}}},
        {"dressing",
         {{"passed", dressing_passed},
          {"capture_written", result.capture_written},
          {"cascade_pixels", result.cascade_pixels},
          {"foam_pixels", result.foam_pixels},
          {"spray_pixels", result.spray_pixels},
          {"sheet_present", result.cascade_pixels > 0},
          {"foam_present", result.foam_pixels > 0},
          {"spray_present", result.spray_pixels > 0}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "waterfall-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
