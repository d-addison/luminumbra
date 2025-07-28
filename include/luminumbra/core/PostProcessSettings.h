#pragma once

namespace Luminumbra::Core {

struct PostProcessSettings {
    float bloomThreshold = 1.0f;
    float bloomIntensity = 0.5f;
    float dofFocalDistance = 10.0f;
    float dofFocalRange = 5.0f;
    float godRaysDensity = 0.5f;
    float exposure = 1.0f;
    bool enableBloom = true;
    bool enableDof = true;
    bool enableGodRays = true;
};

} // namespace Luminumbra::Core