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
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CombustionComponents.h"
#include "../components/CoreComponents.h"
#include "../components/CropLifecycleComponents.h"
#include "../components/DiseaseComponents.h"
#include "../components/GrazeableComponent.h"
#include "../components/IrrigationComponents.h"
#include "../components/MortalComponents.h"
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h"
#include "../components/SoilComponents.h"
#include "../ecs/EntitySnapshot.h"
#include "../world/GroundObjectStore.h"

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
// Test representation before get<T>: JSON numeric conversion otherwise silently
// narrows integers and can invoke undefined behaviour for a large floating value.
template<typename T>
inline bool FitsNumber(const nlohmann::json& value) {
    if constexpr (std::is_floating_point_v<T>) {
        if (!value.is_number())
            return false;
        const double number = value.get<double>();
        return std::isfinite(number) && number >= -std::numeric_limits<T>::max() &&
               number <= std::numeric_limits<T>::max();
    } else {
        if (value.is_number_unsigned())
            return value.get<std::uint64_t>() <= std::numeric_limits<T>::max();
        if (!value.is_number_integer())
            return false;
        const auto number = value.get<std::int64_t>();
        return number >= 0 && static_cast<std::uint64_t>(number) <= std::numeric_limits<T>::max();
    }
}

inline bool ValidGenes(const nlohmann::json& genes) {
    if (!genes.is_array())
        return false;
    // Historical snapshots may contain a partial genome; missing genes stay zero.
    for (std::size_t i = 0; i < genes.size() && i < ::Luminumbra::Components::kPlantGeneCount;
         ++i) {
        if (!FitsNumber<float>(genes[i]) || genes[i].get<double>() < 0.0 ||
            genes[i].get<double>() > 1.0)
            return false;
    }
    return true;
}

inline bool ValidComponent(const ::Luminumbra::Ecs::EntityComponentSnapshot& component) {
    namespace C = ::Luminumbra::Components;
    const auto& data = component.data;
    const auto& type = component.type;
    if (type == "Transform")
        return FitsNumber<float>(data.at("x")) && FitsNumber<float>(data.at("y")) &&
               FitsNumber<float>(data.at("z"));
    if (type == "PlantGenome")
        return ValidGenes(data.at("genes"));
    if (type == "PlantGrowth")
        return FitsNumber<std::uint16_t>(data.at("species_id")) &&
               FitsNumber<std::uint8_t>(data.at("stage")) &&
               data.at("stage").get<std::uint8_t>() < C::kPlantStageCount &&
               FitsNumber<std::uint8_t>(data.at("quality")) &&
               data.at("quality").get<std::uint8_t>() <= 100 &&
               FitsNumber<std::uint32_t>(data.at("growth_points")) &&
               FitsNumber<std::uint32_t>(data.at("stress_points")) &&
               FitsNumber<std::uint8_t>(data.at("tended")) &&
               FitsNumber<std::uint64_t>(data.at("planted_tick")) &&
               FitsNumber<std::uint64_t>(data.at("last_tick"));
    if (type == "SoilFeeder")
        return FitsNumber<std::uint16_t>(data.at("uptake")) &&
               FitsNumber<std::uint32_t>(data.at("absorbed"));
    if (type == "PlantHealth")
        return FitsNumber<std::uint16_t>(data.at("infection")) &&
               data.at("infection").get<std::uint16_t>() <= C::kDiseaseUnitScale &&
               FitsNumber<std::uint16_t>(data.at("resistance")) &&
               data.at("resistance").get<std::uint16_t>() <= C::kDiseaseUnitScale &&
               FitsNumber<std::uint8_t>(data.at("state")) &&
               data.at("state").get<std::uint8_t>() < C::kPlantDiseaseStateCount &&
               FitsNumber<std::uint32_t>(data.at("infected_ticks"));
    if (type == "Pollination")
        return data.at("pollinated").is_boolean() &&
               FitsNumber<std::uint64_t>(data.at("last_pollen_tick")) &&
               FitsNumber<std::uint32_t>(data.at("crosses")) && ValidGenes(data.at("next_genome"));
    if (type == "CropLifecycle")
        return data.at("perennial").is_boolean() &&
               FitsNumber<std::uint32_t>(data.at("ripe_ticks")) &&
               FitsNumber<std::uint32_t>(data.at("lifespan_ticks")) &&
               FitsNumber<std::uint16_t>(data.at("generations")) &&
               FitsNumber<std::uint16_t>(data.at("species_id"));
    return true; // Unknown components remain forward-compatible and are skipped.
}

inline bool ExactFloat(const nlohmann::json& value) {
    return value.is_number_float() && FitsNumber<float>(value) &&
           static_cast<double>(value.get<float>()) == value.get<double>();
}

inline bool ScalarInRange(const nlohmann::json& value, float maximum) {
    return ExactFloat(value) && value.get<double>() >= 0 && value.get<double>() <= maximum;
}

inline bool ValidOwnerCoordinate(const nlohmann::json& value) {
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>() < 32768;
    return value.is_number_integer() && value.get<std::int64_t>() >= -32768 &&
           value.get<std::int64_t>() < 32768;
}

inline bool ValidEnabledComponent(const ::Luminumbra::Ecs::EntityComponentSnapshot& component) {
    namespace C = ::Luminumbra::Components;
    const auto& data = component.data;
    const auto& type = component.type;
    constexpr std::array<std::pair<std::string_view, std::size_t>, 14> fields = {
        {{"Transform", 3},
         {"PlantGenome", 1},
         {"PlantGrowth", 8},
         {"SoilFeeder", 2},
         {"PlantHealth", 4},
         {"Pollination", 4},
         {"CropLifecycle", 5},
         {"PlantRegion", 3},
         {"PollinationTag", 0},
         {"Combustible", 5},
         {"Mortal", 3},
         {"Grazeable", 2},
         {"WaterSource", 1},
         {"IgnitionSource", 2}}};
    const auto found = std::find_if(
        fields.begin(), fields.end(), [&](const auto& entry) { return entry.first == type; });
    if (found == fields.end() || !data.is_object() || data.size() != found->second)
        return false;
    if (type == "Transform")
        return ExactFloat(data.at("x")) && ExactFloat(data.at("y")) && ExactFloat(data.at("z"));
    if (type == "PlantRegion")
        return ValidOwnerCoordinate(data.at("owner_x")) &&
               ValidOwnerCoordinate(data.at("owner_z")) &&
               FitsNumber<std::uint64_t>(data.at("region_stamp"));
    if (type == "PollinationTag")
        return data.is_object() && data.empty();
    if (type == "Combustible")
        return FitsNumber<std::uint16_t>(data.at("fuel_milli")) &&
               data.at("fuel_milli").get<std::uint16_t>() <= 1000 &&
               FitsNumber<std::uint16_t>(data.at("moisture_milli")) &&
               data.at("moisture_milli").get<std::uint16_t>() <= 1000 &&
               FitsNumber<std::uint8_t>(data.at("burn_state")) &&
               data.at("burn_state").get<std::uint8_t>() <
                   static_cast<std::uint8_t>(C::BurnState::Count) &&
               FitsNumber<std::uint32_t>(data.at("burn_ticks_remaining")) &&
               ScalarInRange(data.at("ignition_radius"), 16777216.0f);
    if (type == "Mortal")
        return FitsNumber<std::uint32_t>(data.at("age_ticks")) &&
               FitsNumber<std::uint32_t>(data.at("lifespan_ticks")) &&
               FitsNumber<std::uint8_t>(data.at("dead")) && data.at("dead").get<unsigned>() <= 1;
    if (type == "Grazeable")
        return ScalarInRange(data.at("biomass"), 1.0f) &&
               FitsNumber<std::uint32_t>(data.at("last_grazed_tick"));
    if (type == "WaterSource")
        return FitsNumber<std::uint16_t>(data.at("output"));
    if (type == "IgnitionSource")
        return ScalarInRange(data.at("radius"), 16777216.0f) &&
               ScalarInRange(data.at("intensity"), 16777216.0f);
    if (type == "PlantGenome" || type == "Pollination") {
        const auto& genes = data.at(type == "PlantGenome" ? "genes" : "next_genome");
        return ValidComponent(component) && genes.size() == C::kPlantGeneCount &&
               std::all_of(
                   genes.begin(), genes.end(), [](const auto& gene) { return ExactFloat(gene); });
    }
    return ValidComponent(component);
}

inline const nlohmann::json* ComponentData(const ::Luminumbra::Ecs::EntitySnapshotRecord& record,
                                           std::string_view type) {
    const auto found = std::find_if(record.components.begin(),
                                    record.components.end(),
                                    [&](const auto& c) { return c.type == type; });
    return found == record.components.end() ? nullptr : &found->data;
}

// Called only after the fixed core and owner payloads have passed component admission.
inline bool OwnerMatchesTransform(const ::Luminumbra::Ecs::EntitySnapshotRecord& record) {
    using Ledger = ::Luminumbra::world::ActiveRegionLedger;
    const auto* transform = ComponentData(record, "Transform");
    const auto* owner = ComponentData(record, "PlantRegion");
    const auto* growth = ComponentData(record, "PlantGrowth");
    if (!transform || !owner || !growth)
        return false;
    const ::Luminumbra::Vec3 position{transform->at("x").get<float>(),
                                      transform->at("y").get<float>(),
                                      transform->at("z").get<float>()};
    const ::Luminumbra::world::RegionKey key{owner->at("owner_x").get<std::int32_t>(),
                                             owner->at("owner_z").get<std::int32_t>()};
    const auto stamp = owner->at("region_stamp").get<std::uint64_t>();
    return Ledger::valid_anchor(position) && Ledger::region_at(position) == key &&
           stamp <= growth->at("last_tick").get<std::uint64_t>() &&
           growth->at("planted_tick").get<std::uint64_t>() <= stamp;
}

inline void ProjectEnabledComponents(const entt::registry& reg,
                                     entt::entity entity,
                                     ::Luminumbra::Ecs::EntitySnapshotRecord& record) {
    namespace C = ::Luminumbra::Components;
    if (reg.all_of<C::PollinationTag>(entity))
        record.components.push_back({"PollinationTag", nlohmann::json::object()});
    if (const auto* value = reg.try_get<const C::CombustibleComponent>(entity))
        record.components.push_back({"Combustible",
                                     {{"fuel_milli", value->fuel_milli},
                                      {"moisture_milli", value->moisture_milli},
                                      {"burn_state", value->burn_state},
                                      {"burn_ticks_remaining", value->burn_ticks_remaining},
                                      {"ignition_radius", value->ignition_radius}}});
    if (const auto* value = reg.try_get<const C::MortalComponent>(entity))
        record.components.push_back({"Mortal",
                                     {{"age_ticks", value->age_ticks},
                                      {"lifespan_ticks", value->lifespan_ticks},
                                      {"dead", value->dead}}});
    if (const auto* value = reg.try_get<const C::GrazeableComponent>(entity))
        record.components.push_back(
            {"Grazeable",
             {{"biomass", value->biomass}, {"last_grazed_tick", value->last_grazed_tick}}});
    if (const auto* value = reg.try_get<const C::WaterSourceComponent>(entity))
        record.components.push_back({"WaterSource", {{"output", value->output}}});
    if (const auto* value = reg.try_get<const C::IgnitionSourceComponent>(entity))
        record.components.push_back(
            {"IgnitionSource", {{"radius", value->radius}, {"intensity", value->intensity}}});
}
} // namespace pp_detail

// Project all plant entities into a deterministic snapshot. Plants need (at minimum) PlantTag +
// growth + genome + transform; soil/health/pollination/lifecycle are optional (per-plant opt-in).
[[nodiscard]] inline ::Luminumbra::Ecs::EntityRegistrySnapshot
BuildPlantEntitySnapshot(const entt::registry& reg, bool persistent_identity = false) {
    namespace C = ::Luminumbra::Components;
    ::Luminumbra::Ecs::EntityRegistrySnapshot snap;
    snap.persistent_identity = persistent_identity;
    auto view = reg.view<const C::PlantTag,
                         const C::PlantGrowthComponent,
                         const C::PlantGenomeComponent,
                         const C::TransformComponent>();
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end());
    for (auto e : ents) {
        ::Luminumbra::Ecs::EntitySnapshotRecord rec;
        rec.entity_id = static_cast<std::uint64_t>(entt::to_integral(e));
        rec.legacy_entity_id = rec.entity_id;
        rec.name = "plant";
        if (persistent_identity) {
            if (const auto* owner = reg.try_get<const C::PlantRegionComponent>(e)) {
                rec.entity_id = owner->persistent_id;
                rec.components.push_back({"PlantRegion",
                                          {{"owner_x", owner->owner_x},
                                           {"owner_z", owner->owner_z},
                                           {"region_stamp", owner->region_stamp}}});
            }
            pp_detail::ProjectEnabledComponents(reg, e, rec);
        }

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

// Read-only admission for the complete roster. No registry operation runs until
// every plant passes, including plants after the first valid record.
// Generic persistence admits partial records but still checks every supplied
// component. Runtime adoption additionally requires the complete core set.
inline bool ValidatePlantEntitySnapshot(const ::Luminumbra::Ecs::EntityRegistrySnapshot& snap,
                                        bool require_core_components = true) {
    try {
        std::set<std::uint64_t> ids;
        for (const auto& record : snap.entities) {
            if (snap.persistent_identity &&
                (record.name != "plant" || record.entity_id == 0 ||
                 record.entity_id >= ::Luminumbra::world::ActiveRegionLedger::kPersistentIdLimit ||
                 !ids.insert(record.entity_id).second))
                return false;
            if (record.name != "plant")
                continue;
            // Fixed shipping component set; unknown extension payloads are ignored.
            constexpr std::array<std::string_view, 14> known = {"Transform",
                                                                "PlantGenome",
                                                                "PlantGrowth",
                                                                "SoilFeeder",
                                                                "PlantHealth",
                                                                "Pollination",
                                                                "CropLifecycle",
                                                                "PlantRegion",
                                                                "PollinationTag",
                                                                "Combustible",
                                                                "Mortal",
                                                                "Grazeable",
                                                                "WaterSource",
                                                                "IgnitionSource"};
            std::array<bool, known.size()> seen{};
            for (const auto& component : record.components) {
                const auto found = std::find(known.begin(), known.end(), component.type);
                if (found == known.end()) {
                    if (snap.persistent_identity)
                        return false;
                    continue;
                }
                const auto index = static_cast<std::size_t>(found - known.begin());
                if (!snap.persistent_identity && index >= 7) {
                    if (component.type == "PlantRegion")
                        return false;
                    continue;
                }
                if (seen[index] ||
                    !(snap.persistent_identity ? pp_detail::ValidEnabledComponent(component)
                                               : pp_detail::ValidComponent(component)))
                    return false;
                seen[index] = true;
            }
            if ((require_core_components || snap.persistent_identity) &&
                (!seen[0] || !seen[1] || !seen[2]))
                return false;
            if (snap.persistent_identity && (!seen[7] || !pp_detail::OwnerMatchesTransform(record)))
                return false;
        }
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

inline bool HasPersistentPlantIdentity(const entt::registry& reg) {
    const auto plants = reg.view<const ::Luminumbra::Components::PlantTag,
                                 const ::Luminumbra::Components::PlantRegionComponent>();
    return plants.begin() != plants.end();
}

// Error-returning enabled projection; an incomplete tagged plant is never silently omitted.
inline bool BuildPlantEntitySnapshot(const entt::registry& reg,
                                     ::Luminumbra::Ecs::EntityRegistrySnapshot& out,
                                     std::string& error) {
    namespace C = ::Luminumbra::Components;
    for (const auto entity : reg.view<const C::PlantTag>()) {
        if (!reg.all_of<C::TransformComponent,
                        C::PlantGenomeComponent,
                        C::PlantGrowthComponent,
                        C::PlantRegionComponent>(entity)) {
            error = "Corrupt plant authority: incomplete persistent plant.";
            return false;
        }
    }
    auto candidate = BuildPlantEntitySnapshot(reg, true);
    if (!ValidatePlantEntitySnapshot(candidate)) {
        error = "Corrupt plant authority: invalid identity or component value.";
        return false;
    }
    out = std::move(candidate);
    return true;
}

inline bool ValidatePlantEntitySnapshot(const ::Luminumbra::Ecs::EntityRegistrySnapshot& snap,
                                        const ::Luminumbra::world::ActiveRegionLedger& ledger,
                                        const ::Luminumbra::world::GroundObjectStore& ground,
                                        std::string& error) {
    if (!ValidatePlantEntitySnapshot(snap)) {
        error = "Corrupt plant authority: invalid identity or component value.";
        return false;
    }
    if (!snap.persistent_identity)
        return true;
    for (const auto& record : snap.entities) {
        if (record.entity_id >= ledger.next_persistent_id() || ground.find(record.entity_id)) {
            error = "Corrupt plant authority: duplicate or unallocated plant ID.";
            return false;
        }
        const auto* owner = pp_detail::ComponentData(record, "PlantRegion");
        if (!owner) {
            error = "Corrupt plant authority: incomplete persistent plant.";
            return false;
        }
        const ::Luminumbra::world::RegionKey key{owner->at("owner_x").get<std::int32_t>(),
                                                 owner->at("owner_z").get<std::int32_t>()};
        if (!ledger.records().contains(key) ||
            owner->at("region_stamp").get<std::uint64_t>() > ledger.header_tick()) {
            error = "Corrupt plant authority: orphaned owner or future stamp.";
            return false;
        }
    }
    return true;
}

namespace pp_detail {
inline void ApplyEnabledComponent(entt::registry& reg,
                                  entt::entity entity,
                                  const ::Luminumbra::Ecs::EntityComponentSnapshot& component,
                                  std::uint64_t persistent_id) {
    namespace C = ::Luminumbra::Components;
    const auto& data = component.data;
    if (component.type == "PlantRegion")
        reg.emplace<C::PlantRegionComponent>(entity,
                                             persistent_id,
                                             data.at("owner_x").get<std::int32_t>(),
                                             data.at("owner_z").get<std::int32_t>(),
                                             data.at("region_stamp").get<std::uint64_t>());
    else if (component.type == "PollinationTag")
        reg.emplace<C::PollinationTag>(entity);
    else if (component.type == "Combustible")
        reg.emplace<C::CombustibleComponent>(entity,
                                             data.at("fuel_milli").get<std::uint16_t>(),
                                             data.at("moisture_milli").get<std::uint16_t>(),
                                             data.at("burn_state").get<std::uint8_t>(),
                                             data.at("burn_ticks_remaining").get<std::uint32_t>(),
                                             data.at("ignition_radius").get<float>());
    else if (component.type == "Mortal")
        reg.emplace<C::MortalComponent>(entity,
                                        data.at("age_ticks").get<std::uint32_t>(),
                                        data.at("lifespan_ticks").get<std::uint32_t>(),
                                        data.at("dead").get<std::uint8_t>());
    else if (component.type == "Grazeable")
        reg.emplace<C::GrazeableComponent>(entity,
                                           data.at("biomass").get<float>(),
                                           data.at("last_grazed_tick").get<std::uint32_t>());
    else if (component.type == "WaterSource")
        reg.emplace<C::WaterSourceComponent>(entity, data.at("output").get<std::uint16_t>());
    else if (component.type == "IgnitionSource")
        reg.emplace<C::IgnitionSourceComponent>(
            entity, data.at("radius").get<float>(), data.at("intensity").get<float>());
}
} // namespace pp_detail

// Reconstruct plant entities from a snapshot into `reg` (creates fresh entities). Unknown component
// types are skipped. Returns false without registry mutation for invalid input.
// Deterministic (snapshot is id-ordered).
inline bool ApplyPlantEntitySnapshot(entt::registry& reg,
                                     const ::Luminumbra::Ecs::EntityRegistrySnapshot& snap) {
    if (!ValidatePlantEntitySnapshot(snap))
        return false;
    namespace C = ::Luminumbra::Components;
    if (snap.persistent_identity) {
        // Save/load only: bounded by the admitted roster, never run on the tick path.
        std::set<std::uint64_t> existing_ids;
        for (const auto entity : reg.view<const C::PlantRegionComponent>())
            existing_ids.insert(reg.get<const C::PlantRegionComponent>(entity).persistent_id);
        for (const auto& record : snap.entities) {
            if (existing_ids.contains(record.entity_id))
                return false;
        }
    }
    for (const auto& rec : snap.entities) {
        if (rec.name != "plant")
            continue;
        const auto e = reg.create();
        reg.emplace<C::PlantTag>(e);
        for (const auto& comp : rec.components) {
            const nlohmann::json& d = comp.data;
            if (snap.persistent_identity)
                pp_detail::ApplyEnabledComponent(reg, e, comp, rec.entity_id);
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
                if (!snap.persistent_identity)
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
    return true;
}

} // namespace luminumbra::foliage
