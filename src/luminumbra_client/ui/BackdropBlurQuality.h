#pragma once

#include "luminumbra_common/core/GraphicsPresets.h"

namespace Luminumbra::Client {

// Runtime input for backdrop filters. Apply at draw time so changing quality
// also affects already-compiled authored radii. Ordinary blur/drop shadows are
// independent. The settings/filter bridge owns consuming this input.
class BackdropBlurQuality {
    luminumbra::core::UiBlurQuality m_quality = luminumbra::core::UiBlurQuality::Full;

public:
    void set(luminumbra::core::UiBlurQuality quality) {
        m_quality = quality;
    }

    float radius(float authored_radius) const {
        switch (m_quality) {
            case luminumbra::core::UiBlurQuality::Off:
                return 0.0f;
            case luminumbra::core::UiBlurQuality::Half:
                return authored_radius * 0.5f;
            case luminumbra::core::UiBlurQuality::Full:
                return authored_radius;
        }
        return authored_radius;
    }
};

} // namespace Luminumbra::Client
