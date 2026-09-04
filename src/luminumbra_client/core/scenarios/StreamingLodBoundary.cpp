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

void WriteStreamingTelemetry(
    const std::filesystem::path& artifact_dir,
    const std::string& scenario,
    double duration_seconds,
    const Luminumbra::Systems::SHIELD_WorldSystem::StreamingTelemetryStats& stats) {
    const double drain_rate_per_s =
        duration_seconds > 0.0
            ? static_cast<double>(stats.cumulative_scheduled_meshing) / duration_seconds
            : 0.0;
    const bool deferred_age_bounded =
        stats.frames_observed == 0 || stats.max_deferred_age_frames < stats.frames_observed;
    //  judge "did the streaming backlog drain" against
    // the SETTLED queue depth (the trailing-window minimum), not the raw final-
    // frame snapshot. Chunk activation is decoupled from the frame rate (it runs
    // every STREAMING_ACTIVATION_INTERVAL_FRAMES frames), so generation/loading
    // arrives in periodic batches and the raw last_queue_depth reads an in-flight
    // batch (~one ring of loading chunks) whenever the run's final frame lands on
    // or just after an activation tick — a single-frame phase artifact, not a
    // standing backlog. The settled depth is 0 iff the pipeline reaches empty
    // within each activation cycle (bounded + fully draining) and stays nonzero
    // only for a backlog that never empties (genuinely unbounded). This makes the
    // gate measure boundedness instead of which frame the 20 s window happened to
    // stop on. world_hash is untouched (telemetry only).
    const std::size_t settled_queue_depth = stats.settled_queue_depth;
    const bool backlog_bounded = settled_queue_depth == 0 && deferred_age_bounded;

    nlohmann::json artifact = {
        {"schema", "luminumbra.streaming_telemetry.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"scenario", scenario},
        {"duration_seconds", duration_seconds},
        {"frames_observed", stats.frames_observed},
        {"peak_queue_depth", stats.peak_queue_depth},
        {"peak_meshing_candidates", stats.peak_meshing_candidates},
        {"cumulative_scheduled_meshing", stats.cumulative_scheduled_meshing},
        {"cumulative_deferred_meshing", stats.cumulative_deferred_meshing},
        {"max_deferred_age_frames", stats.max_deferred_age_frames},
        {"drain_rate_per_s", drain_rate_per_s},
        {"backlog_bounded", backlog_bounded},
        // final_queue_depth is the SETTLED floor (the value the gate checks);
        // last_frame_queue_depth preserves the raw single-frame snapshot for
        // transparency / debugging of the activation-cycle phase.
        {"final_queue_depth", settled_queue_depth},
        {"last_frame_queue_depth", stats.last_queue_depth}};

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "streaming-telemetry.json");
    output << std::setw(2) << artifact << '\n';
}

float LodBoundaryDistance(Luminumbra::Systems::SHIELD_WorldSystem* world_system) {
    if (world_system && !world_system->get_lod_levels().empty()) {
        return world_system->get_lod_levels().front().distance;
    }
    return 192.0f;
}

void ApplyLodBoundaryOscillationCamera(Luminumbra::world::GameSession* game_session,
                                       Luminumbra::Rendering::Camera* camera,
                                       double elapsed_seconds) {
    if (!camera || !game_session) {
        return;
    }

    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kDriftAmplitudeMeters = 6.0;
    constexpr double kDriftPeriodSeconds = 4.0;
    const float x =
        spawn.x + static_cast<float>(kDriftAmplitudeMeters *
                                     std::sin(elapsed_seconds * 2.0 * kPi / kDriftPeriodSeconds));
    const float z = spawn.z;
    const float terrain_height = world_system ? world_system->GetTerrainHeightAt(x, z) : spawn.y;
    camera->Position = Luminumbra::Vec3(x, terrain_height + 80.0f, z);

    const float boundary_distance = LodBoundaryDistance(world_system);
    const float focus_x = x + boundary_distance;
    const float focus_height =
        world_system ? world_system->GetTerrainHeightAt(focus_x, z) : terrain_height;
    AimCameraAt(camera, Luminumbra::Vec3(focus_x, focus_height, z));
}

void LodBoundaryTransitionRecorder::record_frame(
    Luminumbra::Systems::SHIELD_WorldSystem* world_system) {
    if (!world_system) {
        return;
    }

    ++m_frames_observed;
    for (const Luminumbra::Chunk* chunk : world_system->get_renderable_chunks()) {
        if (!chunk) {
            continue;
        }
        const int lod = chunk->current_lod.load(std::memory_order_acquire);
        const Luminumbra::ChunkID id = chunk->get_id();
        const auto [it, inserted] = m_last_lod.try_emplace(id, lod);
        if (!inserted && it->second != lod) {
            ++m_transitions[id];
            it->second = lod;
        }
    }
}

void WriteLodBoundaryOscillationAnalysis(const std::filesystem::path& artifact_dir,
                                         double duration_seconds,
                                         float boundary_distance,
                                         const LodBoundaryTransitionRecorder& recorder) {
    // Bounds for the asymmetric LOD hysteresis implementation. Per-second
    // rates keep the gate stable across run lengths and reject excessive
    // remeshing when a camera dwells near a band boundary.
    constexpr std::uint64_t kOscillatingTransitionThreshold = 4;
    constexpr double kBaselineMaxTransitionsPerChunkPerSecond = 0.75;
    constexpr std::uint64_t kBaselineMaxOscillatingChunks = 240;
    constexpr double kBaselineMaxTotalTransitionsPerSecond = 115.0;

    std::uint64_t max_transitions_per_chunk = 0;
    std::uint64_t total_transitions = 0;
    std::uint64_t oscillating_chunk_count = 0;
    for (const auto& [id, transitions] : recorder.transitions()) {
        (void)id;
        max_transitions_per_chunk = std::max(max_transitions_per_chunk, transitions);
        total_transitions += transitions;
        if (transitions > kOscillatingTransitionThreshold) {
            ++oscillating_chunk_count;
        }
    }

    const double max_transitions_per_chunk_per_s =
        duration_seconds > 0.0 ? static_cast<double>(max_transitions_per_chunk) / duration_seconds
                               : 0.0;
    const double total_transitions_per_s =
        duration_seconds > 0.0 ? static_cast<double>(total_transitions) / duration_seconds : 0.0;

    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    const bool passed =
        gl_debug.errors == 0 &&
        max_transitions_per_chunk_per_s <= kBaselineMaxTransitionsPerChunkPerSecond &&
        oscillating_chunk_count <= kBaselineMaxOscillatingChunks &&
        total_transitions_per_s <= kBaselineMaxTotalTransitionsPerSecond;

    nlohmann::json artifact = {
        {"schema", "luminumbra.lod_boundary_oscillation.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"passed", passed},
        {"duration_seconds", duration_seconds},
        {"boundary_distance", boundary_distance},
        {"frames_observed", recorder.frames_observed()},
        {"chunks_observed", recorder.chunks_observed()},
        {"max_transitions_per_chunk", max_transitions_per_chunk},
        {"max_transitions_per_chunk_per_s", max_transitions_per_chunk_per_s},
        {"oscillating_transition_threshold", kOscillatingTransitionThreshold},
        {"oscillating_chunk_count", oscillating_chunk_count},
        {"total_transitions", total_transitions},
        {"total_transitions_per_s", total_transitions_per_s},
        {"known_oscillation_baseline",
         {{"max_transitions_per_chunk_per_s", kBaselineMaxTransitionsPerChunkPerSecond},
          {"max_oscillating_chunk_count", kBaselineMaxOscillatingChunks},
          {"max_total_transitions_per_s", kBaselineMaxTotalTransitionsPerSecond}}},
        {"gl_debug",
         {{"messages", gl_debug.messages},
          {"errors", gl_debug.errors},
          {"warnings", gl_debug.warnings},
          {"notifications", gl_debug.notifications}}}};

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "lod-boundary-oscillation.json");
    output << std::setw(2) << artifact << '\n';
}

void ApplyLodSeamArrivalCamera(const RuntimeScenarioConfig& config,
                               Luminumbra::world::GameSession* game_session,
                               Luminumbra::Rendering::Camera* camera,
                               double elapsed_seconds) {
    if (!camera || !game_session) {
        return;
    }

    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;
    const double duration = static_cast<double>(std::max(1, config.timed_run_seconds));
    const double progress = std::clamp(elapsed_seconds / duration, 0.0, 1.0);

    constexpr float kStartDistanceMeters = 350.0f;
    constexpr float kEndDistanceMeters = 40.0f;
    const float distance = kStartDistanceMeters - static_cast<float>(progress) *
                                                      (kStartDistanceMeters - kEndDistanceMeters);
    const float x = spawn.x + distance;
    const float z = spawn.z;
    const float terrain_height = world_system ? world_system->GetTerrainHeightAt(x, z) : spawn.y;
    camera->Position = Luminumbra::Vec3(x, terrain_height + 180.0f, z);

    const float focus_x = x - 120.0f;
    const float focus_height =
        world_system ? world_system->GetTerrainHeightAt(focus_x, z) : terrain_height;
    AimCameraAt(camera, Luminumbra::Vec3(focus_x, focus_height, z));
}

void LodSeamArrivalRecorder::record_frame(Luminumbra::Systems::SHIELD_WorldSystem* world_system) {
    if (!world_system) {
        return;
    }

    ++m_frames_observed;
    std::size_t pending = 0;
    for (const Luminumbra::Chunk* chunk : world_system->get_renderable_chunks()) {
        if (chunk && chunk->pending_lod.load(std::memory_order_acquire) >= 0) {
            ++pending;
        }
    }
    m_last_pending_lod = pending;
    m_pending_lod_high_water = std::max(m_pending_lod_high_water, pending);
}

void WriteLodSeamArrivalAnalysis(const std::filesystem::path& artifact_dir,
                                 double duration_seconds,
                                 const std::vector<LodGroundVisualCapture>& captures,
                                 const LodSeamArrivalRecorder& recorder) {
    // Pixel-count ceilings scale with the capture area ( capture-native
    // update the baseline; identity at the 1280x720 tuning base). Critical: without scaling
    // these would false-fail at native res, where a benign frame has ~6.67x more
    // pixels. The companion ratios are resolution-independent.
    const std::uint64_t kMaxDarkVoidPixels = static_cast<std::uint64_t>(
        ScalePinnedArea(18000, kCapturePinnedWidth, kCapturePinnedHeight));
    const std::uint64_t kMaxNearBlackPixels = static_cast<std::uint64_t>(
        ScalePinnedArea(4000, kCapturePinnedWidth, kCapturePinnedHeight));
    const std::uint64_t kMaxBackgroundBluePixels = static_cast<std::uint64_t>(
        ScalePinnedArea(22000, kCapturePinnedWidth, kCapturePinnedHeight));
    constexpr double kMaxDarkVoidRatio = 0.020;
    constexpr double kMaxNearBlackRatio = 0.0065;
    constexpr double kMaxBackgroundBlueRatio = 0.025;
    // Seam crack sliver clusters (>= 12 connected void pixels, RGB <= 10) are
    // the user-visible defect even at near-black ratios far below the area
    // threshold (pre-fix captures: 505-675 void pixels per frame forming
    // wedge-shaped holes, at near-black ratios of only 0.0002-0.0013).
    // Column-aligned LOD selection plus stale-skirt repair keeps the void
    // cluster count at zero; legitimately dark geometry (RGB 17-28) is not
    // counted.
    constexpr std::uint64_t kMaxNearBlackClusterCount = 0;

    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    bool passed = captures.size() >= 4 && gl_debug.errors == 0;
    nlohmann::json captures_json = nlohmann::json::array();
    for (const LodGroundVisualCapture& capture : captures) {
        const bool capture_passed =
            capture.pixels.dark_void_pixels <= kMaxDarkVoidPixels &&
            capture.pixels.dark_void_ratio <= kMaxDarkVoidRatio &&
            capture.pixels.near_black_pixels <= kMaxNearBlackPixels &&
            capture.pixels.near_black_ratio <= kMaxNearBlackRatio &&
            capture.pixels.background_blue_pixels <= kMaxBackgroundBluePixels &&
            capture.pixels.background_blue_ratio <= kMaxBackgroundBlueRatio &&
            capture.pixels.near_black_cluster_count <= kMaxNearBlackClusterCount;
        if (!capture_passed) {
            passed = false;
        }
        captures_json.push_back({{"role", capture.role},
                                 {"file", capture.file},
                                 {"enforced", true},
                                 {"passed", capture_passed},
                                 {"pixels", LodHolePixelStatsToJson(capture.pixels)}});
    }

    nlohmann::json artifact = {{"schema", "luminumbra.lod_seam_arrival.v1"},
                               {"timestamp_utc", TimestampUtc()},
                               {"passed", passed},
                               {"duration_seconds", duration_seconds},
                               {"frames_observed", recorder.frames_observed()},
                               {"pending_lod_high_water", recorder.pending_lod_high_water()},
                               {"final_pending_lod", recorder.last_pending_lod()},
                               {"thresholds",
                                {{"max_dark_void_pixels", kMaxDarkVoidPixels},
                                 {"max_dark_void_ratio", kMaxDarkVoidRatio},
                                 {"max_near_black_pixels", kMaxNearBlackPixels},
                                 {"max_near_black_ratio", kMaxNearBlackRatio},
                                 {"max_background_blue_pixels", kMaxBackgroundBluePixels},
                                 {"max_background_blue_ratio", kMaxBackgroundBlueRatio},
                                 {"max_near_black_cluster_count", kMaxNearBlackClusterCount},
                                 {"min_near_black_cluster_px", kMinNearBlackClusterPx}}},
                               {"captures", captures_json},
                               {"gl_debug",
                                {{"messages", gl_debug.messages},
                                 {"errors", gl_debug.errors},
                                 {"warnings", gl_debug.warnings},
                                 {"notifications", gl_debug.notifications}}}};

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "lod-seam-arrival.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
