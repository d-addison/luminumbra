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
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CoreComponents.h"
#include "../components/CropLifecycleComponents.h"
#include "../components/DiseaseComponents.h"
#include "../components/GrazeableComponent.h"
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h"
#include "../components/SoilComponents.h"
#include "../ecs/EntitySnapshot.h"
#include "../world/Chunk.h"

namespace luminumbra::foliage {

namespace pp_detail {
// Packed chunks span [-2^20,2^20) in X/Z and [-2^21,2^21) in Y.
// Chunk width/height convert those ranges to simulation coordinates. These
// limits are exclusive at the positive end and inclusive at the negative end.
inline constexpr float kPlantHorizontalPositionLimit =
    static_cast<float>(::Luminumbra::Chunk::kPackedMaxXz + 1) * ::Luminumbra::CHUNK_SIZE_X;
inline constexpr float kPlantVerticalPositionLimit =
    static_cast<float>(::Luminumbra::Chunk::kPackedMaxY + 1) * ::Luminumbra::CHUNK_SIZE_Y;
static_assert(::Luminumbra::CHUNK_SIZE_X == ::Luminumbra::CHUNK_SIZE_Z);

inline bool ValidPosition(const nlohmann::json& value, float limit) {
    if (!value.is_number())
        return false;
    const double number = value.get<double>();
    // Check the adopted float too: a double just below the upper end can round
    // to the unsupported boundary during reconstruction.
    return std::isfinite(number) && number >= -limit && number < limit &&
           static_cast<float>(number) < limit;
}
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
    if (type == "Transform") {
        return ValidPosition(data.at("x"), kPlantHorizontalPositionLimit) &&
               ValidPosition(data.at("y"), kPlantVerticalPositionLimit) &&
               ValidPosition(data.at("z"), kPlantHorizontalPositionLimit);
    }
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
    if (type == "Grazeable")
        return FitsNumber<float>(data.at("biomass")) && data.at("biomass").get<double>() >= 0.0 &&
               data.at("biomass").get<double>() <= 1.0 &&
               FitsNumber<std::uint32_t>(data.at("last_grazed_tick"));
    return true; // Unknown components remain forward-compatible and are skipped.
}

inline std::optional<entt::entity>
SavedPlantEntity(const ::Luminumbra::Ecs::EntitySnapshotRecord& record) {
    if (!record.has_entity_id || record.entity_id > std::numeric_limits<entt::id_type>::max())
        return std::nullopt;
    const auto entity = static_cast<entt::entity>(record.entity_id);
    // Historical records without a usable identity retain fresh-ID loading.
    if (entity == entt::null || entity == entt::tombstone)
        return std::nullopt;
    return entity;
}

// V1 components have free-form payloads and unknown types are ignored by older
// readers. Keep allocator history on the first plant, without changing the ECS
// schema or the empty-roster/no-file contract. Legacy rosters omit this component.
inline constexpr std::string_view kAllocatorComponent = "PlantAllocator";
struct PlantAllocator {
    std::vector<entt::entity> packed;
    std::size_t live = 0;
    entt::entity next = entt::entity{};
};

inline const nlohmann::json*
FindAllocator(const ::Luminumbra::Ecs::EntityRegistrySnapshot& snapshot) {
    for (const auto& record : snapshot.entities) {
        if (record.name == "plant") {
            for (const auto& component : record.components) {
                if (component.type == kAllocatorComponent)
                    return &component.data;
            }
        }
    }
    return nullptr;
}

inline bool ReadAllocator(const ::Luminumbra::Ecs::EntityRegistrySnapshot& snapshot,
                          PlantAllocator& allocator) {
    const nlohmann::json* data = nullptr;
    for (const auto& record : snapshot.entities) {
        if (record.name != "plant")
            continue;
        for (const auto& component : record.components) {
            if (component.type != kAllocatorComponent)
                continue;
            if (data)
                return false;
            data = &component.data;
        }
    }
    if (!data)
        return true;
    using Traits = entt::entt_traits<entt::entity>;
    const auto& packed = data->at("packed");
    // EnTT has at most entity_mask usable slots. Allocate only from the actual
    // bounded array, never from a declared count or an entity's numeric value.
    if (!packed.is_array() || packed.size() > Traits::entity_mask ||
        !::Luminumbra::Ecs::snapshot_detail::ReadUnsignedInteger(data->at("live"),
                                                                 allocator.live) ||
        allocator.live > packed.size())
        return false;
    std::vector<entt::id_type> slots;
    for (const auto& value : packed) {
        entt::id_type id = 0;
        if (!::Luminumbra::Ecs::snapshot_detail::ReadUnsignedInteger(value, id))
            return false;
        const auto entity = static_cast<entt::entity>(id);
        if (entity == entt::null || entity == entt::tombstone)
            return false;
        allocator.packed.push_back(entity);
        slots.push_back(entt::to_entity(entity));
    }
    std::sort(slots.begin(), slots.end());
    if (std::adjacent_find(slots.begin(), slots.end()) != slots.end())
        return false;
    entt::id_type next_slot = 0;
    for (const auto slot : slots) {
        if (slot != next_slot)
            break;
        ++next_slot;
    }
    allocator.next = Traits::construct(next_slot, 0);
    std::vector<entt::entity> live(allocator.packed.begin(),
                                   allocator.packed.begin() +
                                       static_cast<std::ptrdiff_t>(allocator.live));
    std::sort(live.begin(), live.end());
    // Every plant must have an exact live ID when allocator metadata is present.
    for (const auto& record : snapshot.entities) {
        if (record.name == "plant") {
            const auto entity = SavedPlantEntity(record);
            if (!entity || !std::binary_search(live.begin(), live.end(), *entity))
                return false;
        }
    }
    return true;
}

inline bool AllocatorResidentsMatch(const entt::registry& registry,
                                    const ::Luminumbra::Ecs::EntityRegistrySnapshot& snapshot,
                                    const PlantAllocator& allocator,
                                    bool replace_plants) {
    using C = ::Luminumbra::Components::PlantTag;
    std::vector<entt::entity> plants;
    for (const auto& record : snapshot.entities) {
        if (record.name == "plant") {
            if (const auto entity = SavedPlantEntity(record))
                plants.push_back(*entity);
        }
    }
    std::sort(plants.begin(), plants.end());
    std::vector<entt::entity> expected;
    for (std::size_t index = 0; index < allocator.live; ++index) {
        const auto entity = allocator.packed[index];
        if (!std::binary_search(plants.begin(), plants.end(), entity))
            expected.push_back(entity);
    }
    std::sort(expected.begin(), expected.end());
    std::size_t residents = 0;
    for (const auto [entity] : registry.storage<entt::entity>()->each()) {
        if (registry.all_of<C>(entity)) {
            if (!replace_plants)
                return false;
        } else {
            if (!std::binary_search(expected.begin(), expected.end(), entity))
                return false;
            ++residents;
        }
    }
    // Non-plants are not serialized here. Refuse missing/extra generations rather
    // than inventing live placeholders or destroying another subsystem's entities.
    return residents == expected.size();
}
} // namespace pp_detail

// Project all plant entities into a deterministic snapshot. Plants need (at minimum) PlantTag +
// growth + genome + transform; soil/health/pollination/lifecycle/grazing are optional.
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
        if (const auto* grazing = reg.try_get<const C::GrazeableComponent>(e)) {
            rec.components.push_back(
                {"Grazeable",
                 {{"biomass", grazing->biomass}, {"last_grazed_tick", grazing->last_grazed_tick}}});
        }
        snap.entities.push_back(std::move(rec));
    }
    if (!snap.entities.empty()) {
        const auto* storage = reg.storage<entt::entity>();
        nlohmann::json packed = nlohmann::json::array();
        for (std::size_t index = 0; index < storage->size(); ++index)
            packed.push_back(entt::to_integral(storage->data()[index]));
        snap.entities.front().components.push_back(
            {std::string(pp_detail::kAllocatorComponent),
             {{"packed", std::move(packed)}, {"live", storage->free_list()}}});
    }
    ::Luminumbra::Ecs::SortEntityRegistrySnapshot(snap);
    return snap;
}

// Read-only admission for the complete roster. No registry operation runs until
// every plant passes, including plants after the first valid record.
inline bool ValidatePlantEntitySnapshot(const ::Luminumbra::Ecs::EntityRegistrySnapshot& snap) {
    try {
        // At most one slot per input record; storage is bounded by the roster.
        std::vector<entt::id_type> slots;
        for (const auto& record : snap.entities) {
            if (record.name != "plant")
                continue;
            if (record.has_entity_id &&
                record.entity_id > std::numeric_limits<entt::id_type>::max())
                return false;
            if (const auto entity = pp_detail::SavedPlantEntity(record))
                slots.push_back(entt::to_entity(*entity));
            // Fixed shipping component set; unknown extension payloads are ignored.
            constexpr std::array<std::string_view, 8> known = {"Transform",
                                                               "PlantGenome",
                                                               "PlantGrowth",
                                                               "SoilFeeder",
                                                               "PlantHealth",
                                                               "Pollination",
                                                               "CropLifecycle",
                                                               "Grazeable"};
            std::array<bool, known.size()> seen{};
            for (const auto& component : record.components) {
                const auto found = std::find(known.begin(), known.end(), component.type);
                if (found == known.end())
                    continue;
                const auto index = static_cast<std::size_t>(found - known.begin());
                if (seen[index] || !pp_detail::ValidComponent(component))
                    return false;
                seen[index] = true;
            }
            if (!seen[0] || !seen[1] || !seen[2])
                return false;
        }
        // Different generations of the same slot cannot coexist either.
        std::sort(slots.begin(), slots.end());
        pp_detail::PlantAllocator allocator;
        return std::adjacent_find(slots.begin(), slots.end()) == slots.end() &&
               pp_detail::ReadAllocator(snap, allocator);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

// A roster application replaces plants by default. Explicit append admission must
// compare all live slots; replacement admission exempts only current PlantTags.
// EnTT's create(hint) silently falls back to a fresh ID on collision.
inline bool CanApplyPlantEntitySnapshot(const entt::registry& reg,
                                        const ::Luminumbra::Ecs::EntityRegistrySnapshot& snap,
                                        bool replace_plants = true) {
    if (!ValidatePlantEntitySnapshot(snap))
        return false;
    for (const auto& record : snap.entities) {
        if (record.name != "plant")
            continue;
        if (const auto entity = pp_detail::SavedPlantEntity(record)) {
            const auto current = entt::entt_traits<entt::entity>::construct(
                entt::to_entity(*entity), reg.current(*entity));
            if (reg.valid(current) &&
                !(replace_plants && reg.all_of<::Luminumbra::Components::PlantTag>(current)))
                return false;
        }
    }
    if (pp_detail::FindAllocator(snap)) {
        pp_detail::PlantAllocator allocator;
        return pp_detail::ReadAllocator(snap, allocator) &&
               pp_detail::AllocatorResidentsMatch(reg, snap, allocator, replace_plants);
    }
    return true;
}

// Replace the plant roster with saved IDs (legacy records get fresh IDs). Pass
// replace_plants=false to append instead. Foreign entities are preserved; unknown
// components are skipped. Invalid input refuses before any registry mutation.
inline bool ApplyPlantEntitySnapshot(entt::registry& reg,
                                     const ::Luminumbra::Ecs::EntityRegistrySnapshot& snap,
                                     bool replace_plants = true) {
    if (!CanApplyPlantEntitySnapshot(reg, snap, replace_plants))
        return false;
    namespace C = ::Luminumbra::Components;
    pp_detail::PlantAllocator allocator;
    const bool restore_allocator = pp_detail::FindAllocator(snap) != nullptr;
    if (restore_allocator && !pp_detail::ReadAllocator(snap, allocator))
        return false;
    if (replace_plants) {
        const auto plants = reg.view<C::PlantTag>();
        // One destruction per current plant; no front erasure or foreign teardown.
        const std::vector<entt::entity> removed(plants.begin(), plants.end());
        for (const auto entity : removed)
            reg.destroy(entity);
    }
    if (restore_allocator) {
        // Rebuild entity storage only: foreign component pools keep their exact
        // live IDs. EnTT 3.15's swap-only storage keeps dead generations in its
        // packed tail, and free_list() is the live-prefix boundary.
        auto& storage = reg.storage<entt::entity>();
        storage.clear();
        storage.reserve(allocator.packed.size());
        for (const auto entity : allocator.packed)
            storage.generate(entity);
        storage.free_list(allocator.live);
        // Registry create() never discards allocated slots, so the first absent
        // slot is the same next fresh ID as continuing (no start_from callers).
        storage.start_from(allocator.next);
    }
    // Reserve saved slots first so fresh legacy IDs cannot consume a later hint.
    for (const bool restore_ids : {true, false}) {
        for (const auto& rec : snap.entities) {
            if (rec.name != "plant")
                continue;
            const auto saved_entity = pp_detail::SavedPlantEntity(rec);
            if (saved_entity.has_value() != restore_ids)
                continue;
            const auto e = restore_allocator && saved_entity
                               ? *saved_entity
                               : (saved_entity ? reg.create(*saved_entity) : reg.create());
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
                    // Restore the opt-in TAG too (a marker carries no data, so it isn't
                    // serialized): a persisted PollinationComponent means the plant participated in
                    // cross-pollination, and the pollination tick views PollinationTag — without it
                    // a reloaded field would stop drifting. Co-emplaced with the component (they
                    // are always added together at spawn).
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
                } else if (comp.type == "Grazeable") {
                    auto& grazing = reg.emplace<C::GrazeableComponent>(e);
                    grazing.biomass = d.at("biomass").get<float>();
                    grazing.last_grazed_tick = d.at("last_grazed_tick").get<std::uint32_t>();
                }
            }
        }
    }
    return true;
}

} // namespace luminumbra::foliage
