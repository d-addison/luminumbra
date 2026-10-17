#pragma once

#include <algorithm>
#include <cmath>

namespace Luminumbra::Rendering {

// Shared render-thread transition: recreate must publish complete scene targets
// or retain the old group and return false. History is reset exactly once after
// a successful live change. Callables are statically typed and never retained.
template<class Recreate, class ResetHistory>
bool ApplySceneRenderScale(float& current,
                           float requested,
                           bool started,
                           Recreate&& recreate,
                           ResetHistory&& reset_history) {
    if (!std::isfinite(requested))
        return false;
    const float scale = std::clamp(requested, 0.5f, 1.0f);
    if (current == scale)
        return true;
    if (started && !recreate(scale))
        return false;
    current = scale;
    if (started)
        reset_history();
    return true;
}

} // namespace Luminumbra::Rendering
