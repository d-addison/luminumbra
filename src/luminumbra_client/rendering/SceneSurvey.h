#pragma once

#include <filesystem>

struct GLFWwindow;

namespace Luminumbra::world {
class GameSession;
}

namespace Luminumbra::Rendering {

class RenderPipeline;
class Camera;

// ===========================================================================
// SceneSurvey — autonomous "tour the world and screenshot every scene type".
//
// The owner ask: the engine should be able to get scene screenshots on its own
// (a waterfall, a rocky cliff, a grass field, a lake) instead of being stuck at
// the spawn pose. This DISCOVERS points of interest in the already-generated
// world using only public, deterministic world queries:
//   * waterfalls  -> WaterfallDetect (river/lake-outlet drops)
//   * cliffs/rock -> steepest terrain slope (central-difference gradient scan)
//   * grass field -> a flat, above-water cell
//   * lake/water  -> a cell where WaterLevelAt > terrain
// then for each POI it teleports the camera + streaming anchor there, streams +
// settles the region (EnsureSurfaceReadyNear + wait_for_streaming_jobs + a few
// rendered settle frames), and writes a labelled screenshot (.ppm) + frame-scan
// (.json) plus a survey index. Blocking headless capture; issues no sim writes
//. Pure glue over existing engine primitives.
// ===========================================================================
void RunSceneSurvey(GLFWwindow* window,
                    world::GameSession& session,
                    RenderPipeline& pipeline,
                    Camera& camera,
                    const std::filesystem::path& out_dir,
                    const std::filesystem::path& materials_json,
                    bool wireframe);

} // namespace Luminumbra::Rendering
