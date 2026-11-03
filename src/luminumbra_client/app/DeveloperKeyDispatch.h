#pragma once

#include "app/UiInputRouter.h"
#include "debug/DeveloperConsole.h"

namespace Luminumbra::Client::App {
// Dispatch resolved developer shortcuts after UI ownership is refreshed. The caller
// handles capture, toggle and escape routes, then refreshes ownership after dispatch.
inline KeyRoute DispatchDeveloperKey(Debug::DeveloperConsole* console,
                                     UiInputRouter* router,
                                     int key,
                                     int action,
                                     int mods,
                                     unsigned scope) {
    const Debug::KeyChord chord{key, mods & 15};
    const auto shortcut = console ? console->Bindings().Match(chord, action, scope) : std::nullopt;
    const bool toggle = console && console->Bindings().Get("console.toggle") == chord;
    const auto route =
        router ? router->Key(key, action, toggle, shortcut.has_value()) : KeyRoute::Gameplay;
    if (route == KeyRoute::DeveloperShortcut && shortcut && console)
        console->InvokeAction(*shortcut);
    return route;
}
} // namespace Luminumbra::Client::App
