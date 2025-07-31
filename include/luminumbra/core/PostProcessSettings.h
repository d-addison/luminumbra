#pragma once

namespace Luminumbra::Core {

struct PostProcessSettings {
    float bloomThreshold = 1.0f;
    float bloomIntensity = 0.5f;
    float dofFocalDistance = 10.0f;
    float dofFocalRange = 5.0f;
    float godRaysDensity = 0.5f;
    float godRaysWeight = 0.5f; // Weight for god rays effect
    // Global settings
    float exposure = 1.0f;
    float gamma = 2.2f;
    float saturation = 1.0f;
    float contrast = 1.0f;
    float brightness = 1.0f;
    bool enableBloom = true;
    bool enableDof = false;
    bool enableGodRays = false;
    bool enableColorGrading = true;
};

} // namespace Luminumbra::Core