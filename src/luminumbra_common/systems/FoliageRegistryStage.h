#pragma once

#include "../components/CombustionComponents.h"
#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/CropLifecycleComponents.h"
#include "../components/DiseaseComponents.h"
#include "../components/GrazeableComponent.h"
#include "../components/IrrigationComponents.h"
#include "../components/MortalComponents.h"
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h"
#include "../components/SoilComponents.h"
#include "../core/WaterComponents.h"
#include <entt/entt.hpp>
#include <set>
#include <stdexcept>
#include <vector>

namespace luminumbra::foliage {
namespace stage_detail {
namespace C = ::Luminumbra::Components;
template<class T>
void Copy(const entt::registry& from,
          entt::entity source,
          entt::registry& to,
          entt::entity target) {
    if (const auto* component = from.try_get<const T>(source))
        to.emplace_or_replace<T>(target, *component);
}
inline void CopyFoliage(const entt::registry& from,
                        entt::entity source,
                        entt::registry& to,
                        entt::entity target) {
    Copy<C::TransformComponent>(from, source, to, target);
    Copy<C::PlantTag>(from, source, to, target);
    Copy<C::PlantRegionComponent>(from, source, to, target);
    Copy<C::PlantGenomeComponent>(from, source, to, target);
    Copy<C::PlantGrowthComponent>(from, source, to, target);
    Copy<C::SoilFeederComponent>(from, source, to, target);
    Copy<C::PlantHealthComponent>(from, source, to, target);
    Copy<C::CropLifecycleComponent>(from, source, to, target);
    Copy<C::PollinationComponent>(from, source, to, target);
    Copy<C::PollinationTag>(from, source, to, target);
    Copy<C::CombustibleComponent>(from, source, to, target);
    Copy<C::IgnitionSourceComponent>(from, source, to, target);
    Copy<C::WaterSourceComponent>(from, source, to, target);
    Copy<C::WaterFlowSourceComponent>(from, source, to, target);
    Copy<C::GrazeableComponent>(from, source, to, target);
    Copy<C::MortalComponent>(from, source, to, target);
    Copy<C::CreatureComponent>(from, source, to, target);
}
} // namespace stage_detail
// Staging preserves existing handles. Fresh births receive live handles only at
// commit; their simulation identity is the already allocated persistent ID.
// All unrelated components on retained entities remain in the original registry.
inline entt::registry StageFoliageRegistry(const entt::registry& original,
                                           std::set<entt::entity>& retained) {
    namespace C = ::Luminumbra::Components;
    entt::registry stage;
    for (auto e : original.view<const C::TransformComponent>()) {
        if (!original.any_of<C::PlantTag,
                             C::CombustibleComponent,
                             C::IgnitionSourceComponent,
                             C::WaterSourceComponent,
                             C::GrazeableComponent,
                             C::MortalComponent,
                             C::CreatureComponent>(e))
            continue;
        if (stage.create(e) != e)
            throw std::runtime_error("Foliage handle staging refused");
        stage_detail::CopyFoliage(original, e, stage, e);
        retained.insert(e);
    }
    return stage;
}
inline void CommitFoliageRegistry(entt::registry& original,
                                  const entt::registry& stage,
                                  const std::set<entt::entity>& retained) {
    for (const auto e : retained) {
        if (stage.valid(e))
            stage_detail::CopyFoliage(stage, e, original, e);
        else
            original.destroy(e);
    }
    for (auto e : stage.view<const ::Luminumbra::Components::PlantTag>())
        if (!retained.contains(e)) {
            const auto live = original.create();
            stage_detail::CopyFoliage(stage, e, original, live);
        }
}
} // namespace luminumbra::foliage
