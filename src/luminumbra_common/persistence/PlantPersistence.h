#pragma once

//  plant PERSISTENCE projection. Projects every PlantTag entity's SIM TRUTH
// (genome + integer growth/soil/disease/pollination/lifecycle state + position) into the engine's
// generic, deterministic EntityRegistrySnapshot (ecs/EntitySnapshot.h) and reconstructs it.
// Geometry is VISUAL-ONLY and is NOT persisted — only the small integer/genome sim state, so a
// saved world reloads its crops (stage, genetics, husbandry, health, pollination cross,
// annual/perennial cycle) byte-exact. There is no runtime component registry, so each component
// type is projected and rebuilt explicitly by name (the EntitySnapshot `data` is free-form JSON per
// component).
//
// DETERMINISM: id-ordered entities, fixed component set + ascending type names (SortEntityRegistry-
// Snapshot), full nlohmann float precision for genes. An EMPTY roster yields an EMPTY snapshot, so
// the no-plant save path stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"
#include "../components/CropLifecycleComponents.h"
#include "../components/DiseaseComponents.h"
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h"
#include "../components/SoilComponents.h"
#include "../ecs/EntitySnapshot.h"

namespace luminumbra::foliage {

namespace pp_detail {
inline nlohmann::json
GenesToJson(const std::array<float, ::Luminumbra::Components::kPlantGeneCount>& g) {
    nlohmann::json a = nlohmann::json::array();
    for (float v : g)
        a.push_back(v);
    return a;
}
inline void GenesFromJson(const nlohmann::json& a,
                          std::array<float, ::Luminumbra::Components::kPlantGeneCount>& g) {
    for (std::size_t i = 0; i < g.size() && i < a.size(); ++i)
        g[i] = a[i].get<float>();
}
} // namespace pp_detail

// Project all plant entities into a deterministic snapshot. Plants need (at minimum) PlantTag +
// growth + genome + transform; soil/health/pollination/lifecycle are optional (per-plant opt-in).
[[nodiscard]] inline ::Luminumbra::Ecs::EntityRegistrySnapshot
BuildPlantEntitySnapshot(const entt::registry& reg) {
    namespace C = ::Luminumbra::Components;
    ::Luminumbra::Ecs::EntityRegistrySnapshot snap;
    auto view = reg.view<const C::PlantTag,
                         const C::PlantGrowthComponent,
                         const C::PlantGenomeComponent,
                         const C::TransformComponent>();
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end());
    for (auto e : ents) {
        ::Luminumbra::Ecs::EntitySnapshotRecord rec;
        rec.entity_id = static_cast<std::uint64_t>(entt::to_integral(e));
        rec.name = "plant";

        const auto& tf = reg.get<const C::TransformComponent>(e);
        rec.components.push_back(
            {"Transform", {{"x", tf.position.x}, {"y", tf.position.y}, {"z", tf.position.z}}});

        const auto& gn = reg.get<const C::PlantGenomeComponent>(e);
        rec.components.push_back({"PlantGenome", {{"genes", pp_detail::GenesToJson(gn.genes)}}});

        const auto& g = reg.get<const C::PlantGrowthComponent>(e);
        rec.components.push_back({"PlantGrowth",
                                  {{"species_id", g.species_id},
                                   {"stage", g.stage},
                                   {"quality", g.quality},
                                   {"growth_points", g.growth_points},
                                   {"stress_points", g.stress_points},
                                   {"tended", g.tended},
                                   {"planted_tick", g.planted_tick},
                                   {"last_tick", g.last_tick}}});

        if (const auto* sf = reg.try_get<const C::SoilFeederComponent>(e)) {
            rec.components.push_back(
                {"SoilFeeder", {{"uptake", sf->uptake}, {"absorbed", sf->absorbed}}});
        }
        if (const auto* h = reg.try_get<const C::PlantHealthComponent>(e)) {
            rec.components.push_back({"PlantHealth",
                                      {{"infection", h->infection},
                                       {"resistance", h->resistance},
                                       {"state", h->state},
                                       {"infected_ticks", h->infected_ticks}}});
        }
        if (const auto* pc = reg.try_get<const C::PollinationComponent>(e)) {
            rec.components.push_back(
                {"Pollination",
                 {{"pollinated", pc->pollinated},
                  {"last_pollen_tick", pc->last_pollen_tick},
                  {"crosses", pc->crosses},
                  {"next_genome", pp_detail::GenesToJson(pc->next_genome.genes)}}});
        }
        if (const auto* lc = reg.try_get<const C::CropLifecycleComponent>(e)) {
            rec.components.push_back({"CropLifecycle",
                                      {{"perennial", lc->perennial},
                                       {"ripe_ticks", lc->ripe_ticks},
                                       {"lifespan_ticks", lc->lifespan_ticks},
                                       {"generations", lc->generations},
                                       {"species_id", lc->species_id}}});
        }
        snap.entities.push_back(std::move(rec));
    }
    ::Luminumbra::Ecs::SortEntityRegistrySnapshot(snap);
    return snap;
}

// Reconstruct plant entities from a snapshot into `reg` (creates fresh entities). Unknown component
// types are skipped. Deterministic (snapshot is id-ordered).
inline void ApplyPlantEntitySnapshot(entt::registry& reg,
                                     const ::Luminumbra::Ecs::EntityRegistrySnapshot& snap) {
    namespace C = ::Luminumbra::Components;
    for (const auto& rec : snap.entities) {
        if (rec.name != "plant")
            continue;
        const auto e = reg.create();
        reg.emplace<C::PlantTag>(e);
        for (const auto& comp : rec.components) {
            const nlohmann::json& d = comp.data;
            if (comp.type == "Transform") {
                auto& tf = reg.emplace<C::TransformComponent>(e);
                tf.position = ::Luminumbra::Vec3(
                    d.at("x").get<float>(), d.at("y").get<float>(), d.at("z").get<float>());
            } else if (comp.type == "PlantGenome") {
                auto& gn = reg.emplace<C::PlantGenomeComponent>(e);
                pp_detail::GenesFromJson(d.at("genes"), gn.genes);
            } else if (comp.type == "PlantGrowth") {
                auto& g = reg.emplace<C::PlantGrowthComponent>(e);
                g.species_id = d.at("species_id").get<std::uint16_t>();
                g.stage = d.at("stage").get<std::uint8_t>();
                g.quality = d.at("quality").get<std::uint8_t>();
                g.growth_points = d.at("growth_points").get<std::uint32_t>();
                g.stress_points = d.at("stress_points").get<std::uint32_t>();
                g.tended = d.at("tended").get<std::uint8_t>();
                g.planted_tick = d.at("planted_tick").get<std::uint64_t>();
                g.last_tick = d.at("last_tick").get<std::uint64_t>();
            } else if (comp.type == "SoilFeeder") {
                auto& sf = reg.emplace<C::SoilFeederComponent>(e);
                sf.uptake = d.at("uptake").get<std::uint16_t>();
                sf.absorbed = d.at("absorbed").get<std::uint32_t>();
            } else if (comp.type == "PlantHealth") {
                auto& h = reg.emplace<C::PlantHealthComponent>(e);
                h.infection = d.at("infection").get<std::uint16_t>();
                h.resistance = d.at("resistance").get<std::uint16_t>();
                h.state = d.at("state").get<std::uint8_t>();
                h.infected_ticks = d.at("infected_ticks").get<std::uint32_t>();
            } else if (comp.type == "Pollination") {
                // Restore the opt-in TAG too (a marker carries no data, so it isn't serialized): a
                // persisted PollinationComponent means the plant participated in cross-pollination,
                // and the pollination tick views PollinationTag — without it a reloaded field would
                // stop drifting. Co-emplaced with the component (they are always added together at
                // spawn).
                reg.emplace<C::PollinationTag>(e);
                auto& pc = reg.emplace<C::PollinationComponent>(e);
                pc.pollinated = d.at("pollinated").get<bool>();
                pc.last_pollen_tick = d.at("last_pollen_tick").get<std::uint64_t>();
                pc.crosses = d.at("crosses").get<std::uint32_t>();
                pp_detail::GenesFromJson(d.at("next_genome"), pc.next_genome.genes);
            } else if (comp.type == "CropLifecycle") {
                auto& lc = reg.emplace<C::CropLifecycleComponent>(e);
                lc.perennial = d.at("perennial").get<bool>();
                lc.ripe_ticks = d.at("ripe_ticks").get<std::uint32_t>();
                lc.lifespan_ticks = d.at("lifespan_ticks").get<std::uint32_t>();
                lc.generations = d.at("generations").get<std::uint16_t>();
                lc.species_id = d.at("species_id").get<std::uint16_t>();
            }
        }
    }
}

} // namespace luminumbra::foliage
