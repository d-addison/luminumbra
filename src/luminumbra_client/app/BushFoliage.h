#pragma once

#include <array>
#include <cmath>
#include <glm/glm.hpp>

namespace Luminumbra::Client::App {

// A full UV quad for the existing procedural leaf cutout. Inputs are a finite
// centre, unit outward normal and positive half extents from the palette lobe.
inline std::array<glm::vec3, 4> BuildBushLeafCard(const glm::vec3& center,
                                                  const glm::vec3& normal,
                                                  float half_width,
                                                  float half_height) {
    const glm::vec3 reference = std::abs(normal.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 right = glm::normalize(glm::cross(reference, normal)) * half_width;
    const glm::vec3 up = glm::normalize(glm::cross(normal, right)) * half_height;
    return {center - right - up, center + right - up, center + right + up, center - right + up};
}

} // namespace Luminumbra::Client::App
