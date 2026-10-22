#pragma once

#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/world/GameSession.h"

namespace Luminumbra::Client {

inline bool SessionPersistenceAllowed(bool capture_active,
                                      int exit_code,
                                      bool scenario_failed,
                                      bool content_refused) {
    return !capture_active || (exit_code == 0 && !scenario_failed && !content_refused);
}

// Keep the frame's fault decision independent of GLFW so headless hosts can
// exercise the same quit request. The caller owns the window and shutdown loop.
template<typename RequestQuit>
void PollSessionFault(world::GameSession& session, RequestQuit request_quit) {
    if (const auto& fault = session.GetRegionSchedulerFault()) {
        LUMINUMBRA_CORE_ERROR("Region scheduler fault at tick {}: {}; saving and quitting",
                              fault->tick,
                              fault->message);
        request_quit();
    }
}

// The client drains world readers before calling this and clears the world
// afterwards. Capture the spawn anchor before the complete save.
// A refused capture must leave durable authority untouched, even if an older
// in-memory session is still loaded after a rejected reload.
template<typename UpdateSpawn, typename MarkSaved>
void SaveSessionOnExit(world::GameSession* session,
                       bool persistence_allowed,
                       UpdateSpawn update_spawn,
                       MarkSaved mark_saved) {
    if (persistence_allowed && session) {
        update_spawn();
        if (session->SaveWorld())
            mark_saved();
    }
}

} // namespace Luminumbra::Client
