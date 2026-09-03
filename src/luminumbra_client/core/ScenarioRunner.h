#pragma once

// ScenarioRunner: the seam between the interactive frame loop in
// main_client.cpp and the QA scenario driving/capture machinery
// (luminumbra_client_qa). main() holds a nullable runner created ONLY when a
// `--scenario` run is active (RuntimeScenarioConfig::active()); with a null
// runner the frame loop takes exactly the non-scenario branches it always
// took, and with a live runner each hook fires at the exact code position the
// moved scenario block previously occupied:
//
//   onLoopTop              - the loop-top readiness watchdog (world readiness
//                            timeout -> exit 4).
//   onGameStateInGame      - the IN_GAME scenario driving chain (persistence
//                            phases + the per-scenario camera/scene drivers).
//                            Returns how the case should continue.
//   onPreRenderWorldSweep  - the world_visual_sweep synchronous capture
//                            matrix (runs before the per-frame render).
//   onPreRenderCapturePins - the per-frame capture render pins (fixed
//                            time-of-day for the visual smokes, the season
//                            sweep's pin schedule).
//   onPostRenderCapture    - the per-scenario capture/pixel-analysis blocks
//                            that read the just-rendered back buffer, plus the
//                            periodic runtime-state write and the timed-run
//                            completion check.
//   onShutdown             - the scenario result artifacts written after the
//                            frame loop exits (streaming telemetry, the
//                            recorder analyses, the incomplete-timed-run
//                            failure path).
//
// The hooks see main()'s frame state through ScenarioFrameContext: a narrow
// struct of references into the exact locals/globals the moved code already
// used, so the moved bodies stay verbatim.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct GLFWwindow;

namespace Luminumbra {
class JobSystem;
namespace world {
class GameSession;
}
namespace Rendering {
class Camera;
class RenderPipeline;
} // namespace Rendering
} // namespace Luminumbra

namespace Luminumbra::Client::App {
struct ClientAppContext;
class RuntimeStateRecorder;
class RuntimeScenarioFrameRecorder;
struct RuntimeReadinessReport;
} // namespace Luminumbra::Client::App

namespace Luminumbra::Client::ScenarioHarness {

struct RuntimeScenarioConfig;

// Narrow view of main()'s frame-loop state the scenario hooks read/write.
// Field names deliberately match the main_client.cpp locals/globals the moved
// code referenced (g_camera, g_app, ...) so the hook bodies stay verbatim.
struct ScenarioFrameContext {
    GLFWwindow* window = nullptr;
    const std::filesystem::path& root_dir;
    const std::string& root_path_str;
    RuntimeScenarioConfig& scenario_config;
    App::RuntimeStateRecorder& runtime_state_recorder;
    App::RuntimeScenarioFrameRecorder& lod_ground_frame_recorder;
    Luminumbra::JobSystem& jobSystem;
    std::unique_ptr<Luminumbra::world::GameSession>& gameSession;
    Luminumbra::Rendering::RenderPipeline& renderPipeline;
    std::unique_ptr<Luminumbra::Rendering::Camera>& g_camera;
    App::ClientAppContext& g_app;
    const std::string& scenario_world_type;
    bool& scenario_failed;
    std::string& scenario_failure_reason;
    bool& scenario_ready;
    std::uint64_t& scenario_frame_count;
    std::chrono::steady_clock::time_point& scenario_play_started_at;
    App::RuntimeReadinessReport& last_readiness_report;
    int& exit_code;
};

class ScenarioRunner {
public:
    // How the IN_GAME switch case should continue after the scenario chain.
    enum class InGameDrive {
        kFallThrough, // no scenario branch fired: run the non-scenario
                      // profile-fly / player-controller branches as today
        kHandled,     // a scenario branch drove this frame (skip those)
        kBreakCase,   // persistence phase ran + requested shutdown: break out
                      // of the IN_GAME case before physics/sim tick
    };

    virtual ~ScenarioRunner() = default;

    virtual void onLoopTop() = 0;
    virtual InGameDrive onGameStateInGame(float deltaTime) = 0;
    virtual void onPreRenderWorldSweep() = 0;
    virtual void onPreRenderCapturePins() = 0;
    virtual void onPostRenderCapture(float deltaTime) = 0;
    virtual void onShutdown() = 0;
};

std::unique_ptr<ScenarioRunner> CreateScenarioRunner(const ScenarioFrameContext& context);

} // namespace Luminumbra::Client::ScenarioHarness
