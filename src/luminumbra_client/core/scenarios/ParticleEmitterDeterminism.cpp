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

// --- Particle emitter determinism smoke ---

void WriteParticleEmitterDeterminismAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& particle_screenshot,
    const ParticleDeterminismResult& result,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass) {
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    // GPU timer budget only asserted when the timer ring resolved a sample; an
    // unsupported/unresolved timer reports 0.0 ms and is treated as within budget
    // (the byte-equal determinism assertion is the load-bearing check).
    const bool gpu_within_budget = result.particle_pass_gpu_ms <= result.particle_pass_budget_ms;
    const bool passed = result.byte_equal && result.descriptor_count > 0 &&
                        result.descriptor_hash_run_a == result.descriptor_hash_run_b &&
                        render_pass.particle_draws > 0 && gpu_within_budget && gl_debug.errors == 0;

    nlohmann::json artifact = {{"schema", "luminumbra.particle_emitter_determinism.v1"},
                               {"timestamp_utc", TimestampUtc()},
                               {"passed", passed},
                               {"particle_screenshot", particle_screenshot},
                               {"determinism",
                                {{"byte_equal", result.byte_equal},
                                 {"descriptor_count", result.descriptor_count},
                                 {"world_seed", result.world_seed},
                                 {"world_tick", result.world_tick},
                                 {"descriptor_hash_run_a", result.descriptor_hash_run_a},
                                 {"descriptor_hash_run_b", result.descriptor_hash_run_b},
                                 // Documents the snapshot surface: ONLY the emitter descriptor set
                                 // is snapshotted; per-particle motion is render-only and excluded.
                                 {"snapshot_surface", "emitter_descriptor_set"},
                                 {"motion_snapshotted", false}}},
                               {"gpu_timer",
                                {{"particle_pass_gpu_ms", result.particle_pass_gpu_ms},
                                 {"budget_ms", result.particle_pass_budget_ms},
                                 {"within_budget", gpu_within_budget},
                                 {"supported", render_pass.gpu_timers_supported}}},
                               {"render_pass",
                                {{"particle_draws", render_pass.particle_draws},
                                 {"particles_drawn", render_pass.particles_drawn},
                                 {"skybox_draws", render_pass.skybox_draws}}},
                               {"gl_debug",
                                {{"messages", gl_debug.messages},
                                 {"errors", gl_debug.errors},
                                 {"warnings", gl_debug.warnings},
                                 {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "particle-emitter-determinism-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
