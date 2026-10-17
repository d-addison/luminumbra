#pragma once

#include "luminumbra_common/world/RegionDiagnostics.h"

#include <cstddef>

struct GLFWwindow;
enum class GameState;

namespace Luminumbra::Client {
class PlayerController;
namespace ScenarioHarness {
struct RuntimeScenarioConfig;
}
} // namespace Luminumbra::Client

namespace Luminumbra::Client::App {
struct ClientAppContext;
inline constexpr char kRegionPanelWindowTitle[] = "Regions [F4]";

// F4 opens the production overlay. Previous/next, row count and signed-key order
// change only display selection. The future console can own this same state.
struct RegionPanelState {
    bool open = false;
    std::size_t page = 0;
    std::size_t rows_per_page = 64;
    bool descending = false;
};

// Releases panel input ownership after F4, a title close or a session transition.
// Also discards queued/held ImGui input before restoring callbacks and cursor mode.
void CloseRegionPanelInput(GLFWwindow* window);

// Requires only an active ImGui frame, no renderer or GL context. Returns whether the
// window was submitted. Closing, sorting and paging affect only RegionPanelState.
// Hash age is completed simulation ticks since the last successful host request;
// unavailable hashes and an unknown water bound are never refreshed by drawing.
bool DrawRegionPanel(const world::RegionPanelSnapshot& snapshot, RegionPanelState& state);

// Session-aware DebugOverlays adapter; builds the snapshot only while open.
// The existing four-argument status-overlay entry point remains available.
void DrawFrameStatusOverlays(ClientAppContext& app,
                             GameState currentState,
                             const ScenarioHarness::RuntimeScenarioConfig& scenario_config,
                             PlayerController* playerController,
                             const world::GameSession* gameSession);
} // namespace Luminumbra::Client::App
