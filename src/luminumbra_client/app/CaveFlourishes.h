#pragma once

// The one-time world-entry cave-flourish region extracted verbatim from
// main_client.cpp's frame loop: the background doline/enclosed-cave scan batch
// (dispatched once to the JobSystem), the --debug-goto feature framing, and
// the lumin-crystal point-light consume. Client-only decoration — the crystal
// lights never feed any hash.

#include "app/ClientAppContext.h"

namespace Luminumbra::Rendering {
class Camera;
}
namespace Luminumbra::world {
class GameSession;
}

namespace Luminumbra::Client::App {

// Runs the flourish region at the exact frame-loop point the inline code ran:
// inside the interactive-play guard (IN_GAME, unpaused, no scenario), before
// the forager mirror. The camera pointer is the core singleton (may be null
// until a world exists).
void UpdateCaveFlourishes(ClientAppContext& app,
                          Luminumbra::JobSystem& jobSystem,
                          Luminumbra::world::GameSession& gameSession,
                          Luminumbra::Rendering::Camera* camera);

// TEARDOWN CONTRACT: the scan jobs hold a raw SHIELD_WorldSystem* — every
// world transition (CreateWorld / session reset) MUST drain the in-flight
// batch first, exactly as before the extraction.
void DrainBackgroundWorldScan(Luminumbra::JobSystem& jobs);

} // namespace Luminumbra::Client::App
