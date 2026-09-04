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
#if defined(LUMINUMBRA_SYNTHETIC_MESH_FIXTURE)
#include "rendering/synthetic_mesh_fixture.h"
#endif
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
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace Luminumbra::Client::ScenarioHarness {

#if defined(LUMINUMBRA_SYNTHETIC_MESH_FIXTURE)
namespace {

// The renderer bakes tree impostors during startup, before ScenarioRunner is
// constructed. A QA-library static therefore installs the temporary runtime
// overlay before main() resolves its runtime root. Real asset checkouts remain
// untouched because SyntheticMeshFixture is inactive when every payload exists.
const std::unique_ptr<luminumbra::test::SyntheticMeshFixture> kSyntheticFoliageAssets = [] {
    try {
        return std::make_unique<luminumbra::test::SyntheticMeshFixture>(LUMINUMBRA_SOURCE_ROOT);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Synthetic foliage fixture setup failed: %s\n", error.what());
        return std::unique_ptr<luminumbra::test::SyntheticMeshFixture>{};
    }
}();

} // namespace
#endif

// --- Foliage instancing smoke ( / ) ---
// (FoliageSurfaceQuery itself moved to rendering/FoliageSurface.cpp — the
// shipping client's in-game scatter and the worldgen preview share it.)

void WriteFoliageInstancingAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& foliage_screenshot,
    const FoliageInstancingResult& result,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass) {
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();

    // Coverage density tracks the biome table within a band (fixed seeds). The
    // measured density is the fraction of the per-chunk candidate budget that
    // actually emitted; it should land near the biome density it scaled from.
    const double density_delta = std::abs(result.measured_density - result.biome_density);
    const bool density_passed =
        result.instances_within_ring > 0 && density_delta <= result.biome_density_band;

    // Distance-fade: NO foliage beyond the live ring / fade end.
    const bool fade_passed = result.instances_beyond_fade == 0;

    // Wind sway responds: the windy max tip displacement must exceed calm by a
    // clear margin (calm is ~0). The placement hash is identical across the two
    // phases, so the ONLY change is the wind bridge -> the sway delta isolates it.
    constexpr double kMinSwayDelta = 0.02; // metres of extra tip displacement
    const bool sway_passed = result.sway_responds &&
                             (result.windy_max_sway - result.calm_max_sway) >= kMinSwayDelta &&
                             result.windy_max_sway > result.calm_max_sway;

    // GPU budget enforced where the timer resolved a sample; informational on
    // debug (debug ~10x slower —  /  precedent).
    const bool gpu_within_budget =
        !result.gpu_timers_supported || result.foliage_gpu_ms <= result.foliage_budget_ms;

    // Determinism: the instance-set hash is byte-equal across two rebuilds.
    const bool determinism_passed =
        result.hash_byte_equal && result.instance_hash_run_a == result.instance_hash_run_b;

    const bool passed = render_pass.foliage_draws > 0 && determinism_passed && density_passed &&
                        fade_passed && sway_passed && gpu_within_budget && gl_debug.errors == 0;

    nlohmann::json artifact = {
        {"schema", "luminumbra.foliage_instancing.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"foliage_screenshot", foliage_screenshot},
        {"determinism",
         {{"passed", determinism_passed},
          {"world_seed", result.world_seed},
          {"instance_hash_run_a", result.instance_hash_run_a},
          {"instance_hash_run_b", result.instance_hash_run_b},
          {"hash_byte_equal", result.hash_byte_equal},
          {"placement", "pure_hash(chunk_coords, biome_id, slope, moisture, instance_index)"},
          {"global_rng", false},
          {"seed_offset_consumed", false},
          {"world_hash_written", false}}},
        {"coverage_density",
         {{"passed", density_passed},
          {"instances_within_ring", result.instances_within_ring},
          {"instances_total", result.instances_total},
          {"measured_density", result.measured_density},
          {"biome_density", result.biome_density},
          {"density_delta", density_delta},
          {"density_band", result.biome_density_band}}},
        {"distance_fade",
         {{"passed", fade_passed},
          {"instances_beyond_fade", result.instances_beyond_fade},
          {"fade_start_m", result.fade_start_m},
          {"fade_end_m", result.fade_end_m},
          {"live_ring_radius_m", result.live_ring_radius_m}}},
        {"wind_sway",
         {{"passed", sway_passed},
          {"calm_max_sway", result.calm_max_sway},
          {"windy_max_sway", result.windy_max_sway},
          {"sway_delta", result.windy_max_sway - result.calm_max_sway},
          {"min_sway_delta", kMinSwayDelta}}},
        {"gpu_timer",
         {{"foliage_gpu_ms", result.foliage_gpu_ms},
          {"budget_ms", result.foliage_budget_ms},
          {"within_budget", gpu_within_budget},
          {"supported", result.gpu_timers_supported}}},
        {"render_pass",
         {{"foliage_draws", result.foliage_draws},
          {"foliage_instances_drawn", result.foliage_instances_drawn},
          {"skybox_draws", render_pass.skybox_draws}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::ofstream output(artifact_dir / "foliage-instancing-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
