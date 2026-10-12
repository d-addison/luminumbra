#pragma once
#include <entt/entt.hpp>
#include <functional>

namespace Luminumbra::world {
// Empty is the exact legacy full-rate path. Enabled consumers share a pure
// admission predicate for source AND destination entities; never mutate a
// participant or use it as an interacting source when its owner is not due.
using RegionEntityGate = std::function<bool(entt::entity)>;
inline bool AdmitRegionEntity(const RegionEntityGate& gate, entt::entity entity) {
    return !gate || gate(entity);
}
} // namespace Luminumbra::world
