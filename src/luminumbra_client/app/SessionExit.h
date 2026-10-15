#pragma once

#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/world/GameSession.h"

namespace Luminumbra::Client {

// Keep the frame's fault decision independent of GLFW so headless hosts can
// exercise the same quit request. The caller owns the window and shutdown loop.
template<typename RequestQuit>
bool PollSessionFault(world::GameSession& session, RequestQuit request_quit) {
    if (const auto& fault = session.GetRegionSchedulerFault()) {
        LUMINUMBRA_CORE_ERROR("Region scheduler fault at tick {}: {}; saving and quitting",
                              fault->tick,
                              fault->message);
        request_quit();
        return true;
    }
    return false;
}

// The client drains world readers before calling this and clears the world
// afterwards. Preserve its existing state -> spawn anchor -> metadata order.
template<typename UpdateSpawn, typename MarkSaved>
void SaveSessionOnExit(world::GameSession* session,
                       UpdateSpawn update_spawn,
                       MarkSaved mark_saved) {
    if (session && session->SaveWorldState()) {
        update_spawn();
        if (session->SaveWorld())
            mark_saved();
    }
}

} // namespace Luminumbra::Client
