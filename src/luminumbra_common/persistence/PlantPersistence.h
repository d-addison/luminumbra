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
#include <bit>
#include <cstdint>
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
#include "../core/WaterComponents.h"
#include "../ecs/EntitySnapshot.h"
#include "../world/GroundObjectStore.h"
#include <cmath>
#include <set>

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
        rec.name = "plant";
        if (persistent_identity) {
            const auto& owner = reg.get<const C::PlantRegionComponent>(e);
            rec.entity_id = owner.persistent_id;
            rec.components.push_back({"PlantRegion",
                                      {{"owner_x", owner.owner_x},
                                       {"owner_z", owner.owner_z},
                                       {"region_stamp", owner.region_stamp}}});
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
        if (persistent_identity) {
            if (reg.all_of<C::PollinationTag>(e))
                rec.components.push_back({"PollinationTag", nlohmann::json::object()});
            if (const auto* c = reg.try_get<const C::CombustibleComponent>(e))
                rec.components.push_back({"Combustible",
                                          {{"fuel_milli", c->fuel_milli},
                                           {"moisture_milli", c->moisture_milli},
                                           {"burn_state", c->burn_state},
                                           {"burn_ticks_remaining", c->burn_ticks_remaining},
                                           {"ignition_radius", c->ignition_radius}}});
            if (const auto* c = reg.try_get<const C::MortalComponent>(e))
                rec.components.push_back({"Mortal",
                                          {{"age_ticks", c->age_ticks},
                                           {"lifespan_ticks", c->lifespan_ticks},
                                           {"dead", c->dead}}});
            if (const auto* c = reg.try_get<const C::GrazeableComponent>(e))
                rec.components.push_back(
                    {"Grazeable",
                     {{"biomass", c->biomass}, {"last_grazed_tick", c->last_grazed_tick}}});
            if (const auto* c = reg.try_get<const C::WaterSourceComponent>(e))
                rec.components.push_back({"WaterSource", {{"output", c->output}}});
            if (const auto* c = reg.try_get<const C::WaterFlowSourceComponent>(e))
                rec.components.push_back(
                    {"WaterFlowSource",
                     {{"flow_rate_bits", std::bit_cast<std::uint32_t>(c->flow_rate)}}});
            if (const auto* c = reg.try_get<const C::IgnitionSourceComponent>(e))
                rec.components.push_back(
                    {"IgnitionSource", {{"radius", c->radius}, {"intensity", c->intensity}}});
        }
        snap.entities.push_back(std::move(rec));
    }
    ::Luminumbra::Ecs::SortEntityRegistrySnapshot(snap);
    return snap;
}

inline void ApplyPlantEntitySnapshot(entt::registry& reg,
                                     const ::Luminumbra::Ecs::EntityRegistrySnapshot& snap);

// Validate enabled authority before creating any entities. Never assign new IDs to
// loaded records: a missing identity/owner is corruption, not fresh admission.
inline bool ValidatePlantEntitySnapshot(const ::Luminumbra::Ecs::EntityRegistrySnapshot& snap,
                                        const ::Luminumbra::world::ActiveRegionLedger& ledger,
                                        const ::Luminumbra::world::GroundObjectStore& ground,
                                        std::string& error) {
    try {
        if (!snap.entities.empty() && !snap.persistent_identity)
            throw std::invalid_argument("plant identity mode missing");
        std::set<std::uint64_t> ids;
        for (const auto& [id, object] : ground.objects())
            ids.insert(id);
        for (const auto& rec : snap.entities) {
            if (rec.name != "plant" || !rec.entity_id ||
                rec.entity_id >= ledger.next_persistent_id() || !ids.insert(rec.entity_id).second)
                throw std::invalid_argument("duplicate or invalid shared plant ID");
            std::set<std::string> types;
            const nlohmann::json *owner = nullptr, *transform = nullptr, *growth = nullptr;
            for (const auto& c : rec.components) {
                const std::set<std::string> allowed{"Transform",
                                                    "PlantGenome",
                                                    "PlantGrowth",
                                                    "PlantRegion",
                                                    "SoilFeeder",
                                                    "PlantHealth",
                                                    "Pollination",
                                                    "PollinationTag",
                                                    "CropLifecycle",
                                                    "Combustible",
                                                    "Mortal",
                                                    "Grazeable",
                                                    "WaterSource",
                                                    "WaterFlowSource",
                                                    "IgnitionSource"};
                if (!allowed.contains(c.type))
                    throw std::invalid_argument("unknown enabled plant component");
                if (!types.insert(c.type).second)
                    throw std::invalid_argument("duplicate plant component");
                if (c.type == "PlantRegion")
                    owner = &c.data;
                if (c.type == "Transform")
                    transform = &c.data;
                if (c.type == "PlantGrowth")
                    growth = &c.data;
            }
            if (!owner || !transform || !growth || !types.contains("PlantGenome"))
                throw std::invalid_argument("incomplete persistent plant");
            const ::Luminumbra::Vec3 pos(transform->at("x").get<float>(),
                                         transform->at("y").get<float>(),
                                         transform->at("z").get<float>());
            const ::Luminumbra::world::RegionKey key{owner->at("owner_x").get<std::int32_t>(),
                                                     owner->at("owner_z").get<std::int32_t>()};
            for (const char* coord : {"owner_x", "owner_z"}) {
                const auto& v = owner->at(coord);
                if (!v.is_number_integer() || v.get<std::int64_t>() < -32768 ||
                    v.get<std::int64_t>() >= 32768)
                    throw std::invalid_argument("invalid plant owner coordinate");
            }
            const auto stamp = owner->at("region_stamp").get<std::uint64_t>();
            if (!::Luminumbra::world::ActiveRegionLedger::valid_anchor(pos) ||
                ::Luminumbra::world::ActiveRegionLedger::region_at(pos) != key ||
                !ledger.records().contains(key) || stamp > ledger.header_tick() ||
                growth->at("last_tick").get<std::uint64_t>() != stamp)
                throw std::invalid_argument("invalid plant owner or stamp");
            if (growth->at("stage").get<unsigned>() >= ::Luminumbra::Components::kPlantStageCount ||
                growth->at("quality").get<unsigned>() > 100)
                throw std::invalid_argument("invalid plant growth state");
            for (const auto& c : rec.components) {
                // Reject narrowing and negative encodings for every persisted integer.
                const auto integer = [&c](const char* name, std::uint64_t limit) {
                    const auto& v = c.data.at(name);
                    if (!v.is_number_integer() ||
                        (v.is_number_integer() && !v.is_number_unsigned() &&
                         v.get<std::int64_t>() < 0) ||
                        v.get<std::uint64_t>() > limit)
                        throw std::invalid_argument("invalid plant integer");
                };
                const auto finite = [&c](const char* name, float low, float high) {
                    const float v = c.data.at(name).get<float>();
                    if (!std::isfinite(v) || v < low || v > high)
                        throw std::invalid_argument("invalid plant scalar");
                };
                if (c.type == "PlantRegion")
                    integer("region_stamp", ledger.header_tick());
                if (c.type == "PlantGrowth") {
                    integer("species_id", 65535);
                    integer("stage", 5);
                    integer("quality", 100);
                    integer("growth_points", 0xffffffffu);
                    integer("stress_points", 0xffffffffu);
                    integer("tended", 255);
                    integer("last_tick", ledger.header_tick());
                    integer("planted_tick", stamp);
                }
                if (c.type == "PlantHealth") {
                    integer("infection", 1000);
                    integer("resistance", 1000);
                    integer("state", 3);
                    integer("infected_ticks", 0xffffffffu);
                }
                if (c.type == "SoilFeeder") {
                    integer("uptake", 65535);
                    integer("absorbed", 0xffffffffu);
                }
                if (c.type == "Pollination") {
                    integer("last_pollen_tick", ledger.header_tick());
                    integer("crosses", 0xffffffffu);
                    if (!c.data.at("pollinated").is_boolean())
                        throw std::invalid_argument("invalid pollination flag");
                }
                if (c.type == "CropLifecycle") {
                    integer("ripe_ticks", 0xffffffffu);
                    integer("lifespan_ticks", 0xffffffffu);
                    integer("generations", 65535);
                    integer("species_id", 65535);
                    if (!c.data.at("perennial").is_boolean())
                        throw std::invalid_argument("invalid perennial flag");
                }
                if (c.type == "Combustible") {
                    integer("fuel_milli", 1000);
                    integer("moisture_milli", 1000);
                    integer("burn_state", 2);
                    integer("burn_ticks_remaining", 0xffffffffu);
                    finite("ignition_radius", 0, 16777216);
                }
                if (c.type == "Mortal") {
                    integer("age_ticks", 0xffffffffu);
                    integer("lifespan_ticks", 0xffffffffu);
                    integer("dead", 1);
                }
                if (c.type == "Grazeable") {
                    finite("biomass", 0, 1);
                    integer("last_grazed_tick", 0xffffffffu);
                }
                if (c.type == "WaterSource")
                    integer("output", 65535);
                if (c.type == "WaterFlowSource") {
                    integer("flow_rate_bits", 0xffffffffu);
                    const auto flow =
                        std::bit_cast<float>(c.data.at("flow_rate_bits").get<std::uint32_t>());
                    if (!std::isfinite(flow) || flow < 0)
                        throw std::invalid_argument("invalid physical water source flow");
                }
                if (c.type == "IgnitionSource") {
                    finite("radius", 0, 16777216);
                    finite("intensity", 0, 16777216);
                }
                if (c.type == "PlantGenome" || c.type == "Pollination") {
                    const auto& genes =
                        c.data.at(c.type == "PlantGenome" ? "genes" : "next_genome");
                    if (genes.size() != ::Luminumbra::Components::kPlantGeneCount)
                        throw std::invalid_argument("invalid plant genome size");
                    for (const auto& g : genes) {
                        const auto v = g.get<float>();
                        if (!std::isfinite(v) || v < 0 || v > 1)
                            throw std::invalid_argument("invalid plant gene");
                    }
                }
            }
        }
        // Decode and reproject before adoption: unknown/missing payload fields,
        // narrowing and duplicate opt-in components cannot survive this oracle.
        if (!snap.entities.empty()) {
            entt::registry decoded;
            ApplyPlantEntitySnapshot(decoded, snap);
            if (::Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(BuildPlantEntitySnapshot(
                    decoded, true)) != ::Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(snap))
                throw std::invalid_argument("noncanonical plant payload");
        }
        return true;
    } catch (const std::exception& e) {
        error = std::string("Corrupt plant authority: ") + e.what();
        return false;
    }
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
            if (comp.type == "PollinationTag" && snap.persistent_identity) {
                reg.emplace<C::PollinationTag>(e);
            } else if (comp.type == "Combustible" && snap.persistent_identity) {
                auto& c = reg.emplace<C::CombustibleComponent>(e);
                c.fuel_milli = d.at("fuel_milli").get<std::uint16_t>();
                c.moisture_milli = d.at("moisture_milli").get<std::uint16_t>();
                c.burn_state = d.at("burn_state").get<std::uint8_t>();
                c.burn_ticks_remaining = d.at("burn_ticks_remaining").get<std::uint32_t>();
                c.ignition_radius = d.at("ignition_radius").get<float>();
            } else if (comp.type == "Mortal" && snap.persistent_identity) {
                auto& c = reg.emplace<C::MortalComponent>(e);
                c.age_ticks = d.at("age_ticks").get<std::uint32_t>();
                c.lifespan_ticks = d.at("lifespan_ticks").get<std::uint32_t>();
                c.dead = d.at("dead").get<std::uint8_t>();
            } else if (comp.type == "Grazeable" && snap.persistent_identity) {
                auto& c = reg.emplace<C::GrazeableComponent>(e);
                c.biomass = d.at("biomass").get<float>();
                c.last_grazed_tick = d.at("last_grazed_tick").get<std::uint32_t>();
            } else if (comp.type == "WaterSource" && snap.persistent_identity) {
                reg.emplace<C::WaterSourceComponent>(e, d.at("output").get<std::uint16_t>());
            } else if (comp.type == "WaterFlowSource" && snap.persistent_identity) {
                reg.emplace<C::WaterFlowSourceComponent>(
                    e, std::bit_cast<float>(d.at("flow_rate_bits").get<std::uint32_t>()));
            } else if (comp.type == "IgnitionSource" && snap.persistent_identity) {
                reg.emplace<C::IgnitionSourceComponent>(
                    e, d.at("radius").get<float>(), d.at("intensity").get<float>());
            } else if (comp.type == "PlantRegion" && snap.persistent_identity) {
                auto& owner = reg.emplace<C::PlantRegionComponent>(e);
                owner.persistent_id = rec.entity_id;
                owner.owner_x = d.at("owner_x").get<std::int32_t>();
                owner.owner_z = d.at("owner_z").get<std::int32_t>();
                owner.region_stamp = d.at("region_stamp").get<std::uint64_t>();
            } else if (comp.type == "Transform") {
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
                // spawn). Enabled snapshots persist the marker independently.
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
}

} // namespace luminumbra::foliage
