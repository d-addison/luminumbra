#pragma once

#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

// Existing batches begin before traversal; batches inserted during traversal
// must also begin before selecting an ID or appending either instance stream.
template<class Batches>
auto& BeginStaticPropBatch(Batches& batches, std::uint64_t key) {
    auto [entry, inserted] = batches.try_emplace(key);
    if (inserted)
        entry->second.selection.Begin();
    return entry->second;
}

// Model matrices are translation * rotation * scale (no shear). Column lengths
// include reflected axes, so negative scales cannot shrink the culling radius.
inline float InstanceAxisScale(const glm::mat4& model) {
    return std::max({glm::length(glm::vec3(model[0])),
                     glm::length(glm::vec3(model[1])),
                     glm::length(glm::vec3(model[2]))});
}

inline glm::vec4
TransformInstanceSphere(const glm::mat4& model, const glm::vec4& sphere, float padding) {
    return glm::vec4(glm::vec3(model * glm::vec4(glm::vec3(sphere), 1.0f)),
                     sphere.w * InstanceAxisScale(model) * padding);
}

inline bool InstanceSphereCulled(const glm::vec4& sphere, const glm::vec4 (&planes)[6]) {
    for (const auto& plane : planes) {
        if (glm::dot(glm::vec4(glm::vec3(sphere), 1.0f), plane) < -sphere.w)
            return true;
    }
    return false;
}

} // namespace Luminumbra::Rendering
