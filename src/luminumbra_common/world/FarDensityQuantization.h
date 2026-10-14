#pragma once

#include <cstdint>

namespace Luminumbra::World {

// Existing far SDF codec. Non-finite input maps to the reserved sentinel;
// finite nonzero input keeps its sign, including values smaller than one unit.
constexpr float kFarLodSdfQuantScale = 256.0f;
constexpr std::int16_t kFarLodSdfInvalid = static_cast<std::int16_t>(-32768);

std::int16_t QuantizeFarLodSdf(float density);
float DequantizeFarLodSdf(std::int16_t density_q);

} // namespace Luminumbra::World
