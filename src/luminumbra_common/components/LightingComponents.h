#pragma once
#include "../../../include/luminumbra/core/Types.h"

namespace Luminumbra::Components {

struct PointLightComponent {
    Vec3 color{1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    f32 radius = 10.0f;
};

} // namespace Luminumbra::Components
