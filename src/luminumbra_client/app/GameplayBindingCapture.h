#pragma once

#include <optional>
#include <utility>

namespace Luminumbra::Client::App {
struct CapturedGameplayBinding {
    int actionIndex;
    int key;
};

// Gameplay bindings store one integer key, so modifier keys complete capture too.
// GLFW event values: press = 1, Escape = 256. Repeats/releases leave capture pending.
inline std::optional<CapturedGameplayBinding>
CaptureGameplayBinding(int& pendingAction, int key, int action) {
    if (pendingAction < 0 || action != 1)
        return std::nullopt;
    const int capturedAction = std::exchange(pendingAction, -1);
    if (key == 256)
        return std::nullopt;
    return CapturedGameplayBinding{capturedAction, key};
}
} // namespace Luminumbra::Client::App
