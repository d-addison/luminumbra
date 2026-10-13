#include "FarDensityQuantization.h"

#include <algorithm>
#include <cmath>

namespace Luminumbra::World {

std::int16_t QuantizeFarLodSdf(float density) {
    if (!std::isfinite(density)) {
        return kFarLodSdfInvalid;
    }
    const float scaled = std::clamp(density * kFarLodSdfQuantScale, -32767.0f, 32767.0f);
    std::int16_t quantized = static_cast<std::int16_t>(std::lround(scaled));
    if (density < 0.0f && quantized == 0) {
        quantized = -1;
    } else if (density > 0.0f && quantized == 0) {
        quantized = 1;
    }
    return quantized;
}

float DequantizeFarLodSdf(std::int16_t density_q) {
    return static_cast<float>(density_q) / kFarLodSdfQuantScale;
}

} // namespace Luminumbra::World
