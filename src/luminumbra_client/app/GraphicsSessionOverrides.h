#pragma once

#include "luminumbra_common/core/GraphicsPresets.h"

#include <algorithm>
#include <optional>
#include <string_view>

namespace Luminumbra::Client {

// Host-parsed overrides are session-only. Preserve the existing order: the
// benchmark profile wins over saved graphics, then the environment scale wins
// over the profile. Explicit CLI volumetrics wins over the selected preset.
inline luminumbra::core::GraphicsOverrides
GraphicsSessionOverrides(std::string_view profile,
                         std::optional<float> environment_scale,
                         std::optional<int> volumetrics) {
    luminumbra::core::GraphicsOverrides overrides;
    if (!profile.empty())
        overrides["render_scale"] = profile == "quality" ? 1.0 : 0.67;
    if (environment_scale && *environment_scale > 0.0f)
        overrides["render_scale"] = static_cast<double>(std::clamp(*environment_scale, 0.5f, 1.0f));
    if (volumetrics)
        overrides["volumetric_quality"] = *volumetrics > 0 ? 1.0 : 0.0;
    return overrides;
}

} // namespace Luminumbra::Client
