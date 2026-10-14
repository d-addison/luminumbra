#pragma once

#include <cstdio>
#include <optional>
#include <string>

namespace Luminumbra::Rendering {

inline constexpr int kDefaultVolumetricQuality = 1;
inline constexpr float kDefaultAetherMaterialModulation = 1.0f;

inline bool ScentDecalEnabled(const std::optional<std::string>& environment) {
    return environment.value_or("1") != "0";
}

struct CaveAmbientOcclusion {
    float enabled = 1.0f;
    float maxDist = 24.0f;
    float floor = 0.06f;
    float thickness = 1.5f;
    int steps = 8;
};

inline CaveAmbientOcclusion
ResolveCaveAmbientOcclusion(const std::optional<std::string>& environment) {
    CaveAmbientOcclusion result;
    if (!environment)
        return result;

    // "enabled,maxDist,floor,steps,thickness"
    std::sscanf(environment->c_str(),
                "%f,%f,%f,%d,%f",
                &result.enabled,
                &result.maxDist,
                &result.floor,
                &result.steps,
                &result.thickness);
    return result;
}

} // namespace Luminumbra::Rendering
