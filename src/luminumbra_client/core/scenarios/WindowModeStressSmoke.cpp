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

// --- window_mode_stress_smoke ---

std::vector<WindowModeStressStep> BuildWindowModeStressSequence() {
    // windowed -> borderless (larger) -> a couple of resolutions -> exclusive
    // (native-like) -> back to the pinned capture size. The intermediate sizes
    // intentionally differ from the pinned size so every step reallocates the
    // non-pinned targets. The final step restores the pinned 1280x720 so the
    // Smoke-equivalent capture is at the gate-pinned size.
    return {
        {"windowed_1280x720", kCapturePinnedWidth, kCapturePinnedHeight, false, 0, 0, 0, 0, 0},
        {"borderless_1920x1080", 1920, 1080, false, 0, 0, 0, 0, 0},
        {"resolution_1600x900", 1600, 900, false, 0, 0, 0, 0, 0},
        {"resolution_1024x768", 1024, 768, false, 0, 0, 0, 0, 0},
        {"fullscreen_2560x1440", 2560, 1440, false, 0, 0, 0, 0, 0},
        {"windowed_1366x768", 1366, 768, false, 0, 0, 0, 0, 0},
        {"restore_pinned_1280x720",
         kCapturePinnedWidth,
         kCapturePinnedHeight,
         false,
         0,
         0,
         0,
         0,
         0},
    };
}

void WriteWindowModeStressAnalysis(const std::filesystem::path& artifact_dir,
                                   double duration_seconds,
                                   WindowMode requested_window_mode,
                                   const std::vector<WindowModeStressStep>& steps,
                                   const WindowModeStressCapture& final_capture) {
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();

    // Every step that changed the framebuffer size must have bumped the resize
    // generation (targets actually reallocated) and left zero GL errors.
    bool all_steps_ok = !steps.empty();
    std::uint64_t size_changing_steps = 0;
    std::uint64_t reallocating_steps = 0;
    nlohmann::json steps_json = nlohmann::json::array();
    for (const WindowModeStressStep& step : steps) {
        const bool generation_bumped = step.resize_generation_after > step.resize_generation_before;
        const bool realloc_consistent = step.size_changed ? generation_bumped : true;
        const bool targets_match =
            step.targets_width_after == step.width && step.targets_height_after == step.height;
        const bool step_ok = realloc_consistent && targets_match && step.gl_errors_after == 0;
        if (step.size_changed) {
            ++size_changing_steps;
            if (generation_bumped)
                ++reallocating_steps;
        }
        if (!step_ok)
            all_steps_ok = false;
        steps_json.push_back({{"label", step.label},
                              {"width", step.width},
                              {"height", step.height},
                              {"size_changed", step.size_changed},
                              {"resize_generation_before", step.resize_generation_before},
                              {"resize_generation_after", step.resize_generation_after},
                              {"gl_errors_after", step.gl_errors_after},
                              {"targets_width_after", step.targets_width_after},
                              {"targets_height_after", step.targets_height_after},
                              {"passed", step_ok}});
    }

    // Final state must be restored to the pinned capture size.
    const bool final_pinned = !steps.empty() &&
                              steps.back().targets_width_after == kCapturePinnedWidth &&
                              steps.back().targets_height_after == kCapturePinnedHeight;

    // The pinned capture must be at the pinned size and contain a real rendered
    // scene (Smoke-equivalent: ROI present and not an all-dark/empty frame).
    constexpr double kMaxDarkRatio = 0.97;
    const double dark_ratio = final_capture.pixels.roi_pixels > 0
                                  ? static_cast<double>(final_capture.pixels.dark_pixels) /
                                        static_cast<double>(final_capture.pixels.roi_pixels)
                                  : 1.0;
    const bool capture_pinned_size =
        final_capture.width == kCapturePinnedWidth && final_capture.height == kCapturePinnedHeight;
    const bool capture_smoke_ok =
        capture_pinned_size && final_capture.pixels.roi_pixels > 0 && dark_ratio < kMaxDarkRatio;

    const bool passed = all_steps_ok && size_changing_steps > 0 &&
                        reallocating_steps == size_changing_steps && final_pinned &&
                        capture_smoke_ok && gl_debug.errors == 0;

    nlohmann::json artifact = {
        {"schema", "luminumbra.window_mode_stress.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"duration_seconds", duration_seconds},
        {"requested_window_mode", WindowModeName(requested_window_mode)},
        {"capture_pin",
         CapturePinMetadata(requested_window_mode, final_capture.width, final_capture.height)},
        {"steps", steps_json},
        {"aggregates",
         {{"size_changing_steps", size_changing_steps},
          {"reallocating_steps", reallocating_steps},
          {"final_targets_pinned", final_pinned},
          {"final_dark_ratio", dark_ratio}}},
        {"final_capture",
         {{"file", final_capture.file},
          {"width", final_capture.width},
          {"height", final_capture.height},
          {"pixels", ScreenshotPixelStatsToJson(final_capture.pixels)}}},
        {"thresholds", {{"max_dark_ratio", kMaxDarkRatio}}},
        {"gl_debug", {{"errors", gl_debug.errors}, {"warnings", gl_debug.warnings}}}};

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "window-mode-stress-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
