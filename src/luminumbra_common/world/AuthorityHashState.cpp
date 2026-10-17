#include "AuthorityHashState.h"

#include "GroundObjectStore.h"
#include "ai/CreaturePersistence.h"
#include "fields/EnergyFieldState.h"
#include "fields/FieldPageState.h"
#include "persistence/PlantPersistence.h"
#include "persistence/SimulationPendingState.h"
#include "persistence/WorldPersistenceRoundtrip.h"
#include "systems/FoliageRegistryStage.h"

#include <bit>
#include <exception>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Luminumbra::world {
namespace {
void Append(std::string& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
}
void Frame(std::string& bytes, std::string_view tag, std::string_view value) {
    Append(bytes, tag.size(), 4);
    bytes.append(tag);
    Append(bytes, value.size(), 8);
    bytes.append(value);
}
RegionKey Owner(const IVec3& coords) {
    int x = 0, z = 0;
    Persistence::WorldSaveService::region_coords_for_chunk(coords, x, z);
    return {x, z};
}
// Both capture paths pass already owner-filtered projections through this recipe.
Persistence::RegionDigestInputs RegionalContentInputs(const AuthorityHashInputs& inputs,
                                                      RegionKey key,
                                                      const Ecs::EntityRegistrySnapshot* plants,
                                                      std::string fields,
                                                      const std::string& energy,
                                                      std::string creatures) {
    std::string ground = inputs.ground.region_bytes(key);
    if (plants && !plants->entities.empty())
        ground += "|plants:" + Ecs::SerializeEntityRegistrySnapshotJson(*plants);
    fields += energy;
    const auto debt = inputs.ledger.water_debts().find(key);
    if (debt != inputs.ledger.water_debts().end()) {
        fields += "|water_debt:";
        Append(fields, debt->second, 8);
    }
    const auto age = inputs.ledger.energy_ages().find(key);
    if (age != inputs.ledger.energy_ages().end()) {
        fields += "|energy_age:";
        Append(fields, age->second, 8);
    }
    return {.live = {},
            .parked = {},
            .durable = {},
            .ground_object_bytes = std::move(ground),
            .field_page_bytes = std::move(fields),
            .creature_bytes = std::move(creatures)};
}
} // namespace

std::uint64_t CaptureRegionAuthorityDigest(const AuthorityHashInputs& inputs, RegionKey key) {
    // Select before copying/encoding: unrelated plants must not be serialized for a freeze.
    namespace C = Luminumbra::Components;
    entt::registry regional_plants;
    for (const auto e : inputs.registry.view<const C::PlantTag, const C::PlantRegionComponent>()) {
        const auto& owner = inputs.registry.get<C::PlantRegionComponent>(e);
        if (owner.owner_x != key.x || owner.owner_z != key.z)
            continue;
        const auto staged = regional_plants.create();
        luminumbra::foliage::stage_detail::CopyFoliage(inputs.registry, e, regional_plants, staged);
    }
    const auto plants = luminumbra::foliage::BuildPlantEntitySnapshot(regional_plants, true);
    auto region =
        RegionalContentInputs(inputs,
                              key,
                              &plants,
                              inputs.fields.region_bytes(key),
                              inputs.energy ? inputs.energy->RegionBytes(key) : std::string{},
                              luminumbra::ai::CreatureRegionProjection(inputs.registry, key));
    region.live = inputs.chunks.live;
    region.parked = inputs.chunks.parked;
    region.durable = inputs.chunks.durable;
    return Persistence::WorldSaveService::region_simulation_digest(inputs.directory, key, region);
}

std::string AuthorityHashState::FoldEcology(const std::string& ecology) const {
    std::string bytes = "authority_ecology:v1:";
    Append(bytes, 2, 4);
    Frame(bytes, "ecology", ecology);
    Frame(bytes, "authority", m_authority);
    return Persistence::StableChecksum(bytes);
}

const std::string& AuthorityHashState::AmbientBytes(std::uint8_t channels, bool weather) const {
    return m_ambient.at({channels, weather});
}

std::string
AuthorityHashState::AmbientHash(std::uint8_t channels, bool weather, const Vec3& anchor) const {
    auto bytes = AmbientBytes(channels, weather);
    bytes += "|ambient_anchor:";
    for (const float coordinate : {anchor.x, anchor.z})
        Append(bytes, std::bit_cast<std::uint32_t>(coordinate), 4);
    return Persistence::StableChecksum(bytes);
}

AuthorityHashCapture CaptureAuthorityHashState(const AuthorityHashInputs& inputs) {
    try {
        AuthorityHashState state;
        const auto chunks = Persistence::WorldSaveService::capture_authoritative_chunks(
            inputs.directory, inputs.chunks);
        const auto creatures = luminumbra::ai::BuildCreatureSnapshot(inputs.registry);
        for (const auto& [id, object] : inputs.ground.objects())
            if (id >= inputs.ledger.next_persistent_id() ||
                !inputs.ledger.records().contains(GroundObjectStore::region_of(object)))
                throw std::runtime_error("Invalid ground object authority owner or identity");
        namespace C = Luminumbra::Components;
        for (const auto e : inputs.registry.view<const C::PlantTag>())
            if (!inputs.registry.all_of<C::PlantGrowthComponent,
                                        C::PlantGenomeComponent,
                                        C::TransformComponent,
                                        C::PlantRegionComponent>(e))
                throw std::runtime_error("Unadmitted plant authority");
        const auto plants = luminumbra::foliage::BuildPlantEntitySnapshot(inputs.registry, true);
        std::set<std::uint64_t> creature_ids;
        for (const auto& creature : creatures.entities)
            creature_ids.insert(creature.id);
        std::string error;
        if (!luminumbra::foliage::ValidatePlantEntitySnapshot(
                plants, inputs.ledger, inputs.ground, error, creature_ids) ||
            !luminumbra::ai::ValidateCreatureSnapshot(
                creatures, inputs.ledger, inputs.ground, plants, error))
            throw std::runtime_error(error);
        const auto creature_regions = luminumbra::ai::CreatureRegionProjections(creatures);
        std::map<RegionKey, Ecs::EntityRegistrySnapshot> plant_regions;
        for (const auto& plant : plants.entities) {
            bool owned = false;
            for (const auto& component : plant.components) {
                if (component.type != "PlantRegion")
                    continue;
                const RegionKey key{component.data.at("owner_x").get<std::int32_t>(),
                                    component.data.at("owner_z").get<std::int32_t>()};
                auto& part = plant_regions[key];
                part.persistent_identity = plants.persistent_identity;
                part.entities.push_back(plant);
                owned = true;
                break;
            }
            if (!owned)
                throw std::runtime_error("Missing plant owner in authority capture");
        }
        auto field_regions = inputs.fields.region_projections();
        for (const auto& [key, bytes] : field_regions) {
            (void)bytes;
            if (!inputs.ledger.records().contains(key))
                throw std::runtime_error("Missing field authority owner");
        }
        if (inputs.energy && !inputs.energy->ValidateRegionalOwners(inputs.ledger.records(),
                                                                    inputs.ledger.energy_ages()))
            throw std::runtime_error("Invalid regional energy authority");
        const auto energy_regions = inputs.energy ? inputs.energy->RegionalProjections()
                                                  : std::map<RegionKey, std::string>{};
        std::map<RegionKey, WorldStreamingState> regional_chunks;
        WorldStreamingState all_chunks;
        std::set<RegionKey> owners;
        for (const auto& chunk : chunks) {
            const auto key = Owner(chunk->get_coords());
            regional_chunks[key].insert_chunk(chunk);
            all_chunks.insert_chunk(chunk);
            owners.insert(key);
        }
        for (const auto& key : inputs.ground.regions())
            owners.insert(key);
        for (const auto& [key, part] : plant_regions) {
            (void)part;
            owners.insert(key);
        }
        for (const auto& [key, bytes] : creature_regions) {
            (void)bytes;
            owners.insert(key);
        }
        for (const auto& [key, bytes] : field_regions)
            if (!bytes.empty())
                owners.insert(key);
        for (const auto& [key, bytes] : energy_regions) {
            (void)bytes;
            owners.insert(key);
        }
        for (const auto& [key, debt] : inputs.ledger.water_debts()) {
            (void)debt;
            owners.insert(key);
        }
        for (const auto& [key, age] : inputs.ledger.energy_ages()) {
            (void)age;
            owners.insert(key);
        }
        const auto empty_avatar_snapshot =
            Ecs::SerializeEntityRegistrySnapshotJson(Ecs::EntityRegistrySnapshot{});
        const std::string_view avatar_bytes = inputs.avatar_bytes == empty_avatar_snapshot
                                                  ? std::string_view{}
                                                  : std::string_view{inputs.avatar_bytes};
        std::string chunk_envelope = "authority_chunks:v1:";
        Append(chunk_envelope, 3, 4);
        Frame(chunk_envelope, "identity", inputs.identity_bytes);
        Frame(chunk_envelope, "avatars", avatar_bytes);
        std::string contents;
        Append(contents, owners.size(), 8);
        for (const auto key : owners) {
            if (key.x < -32768 || key.x >= 32768 || key.z < -32768 || key.z >= 32768)
                throw std::runtime_error("Invalid authority owner");
            const auto plant = plant_regions.find(key);
            const auto creature = creature_regions.find(key);
            const auto regional = RegionalContentInputs(
                inputs,
                key,
                plant == plant_regions.end() ? nullptr : &plant->second,
                field_regions[key],
                energy_regions.contains(key) ? energy_regions.at(key) : std::string{},
                creature == creature_regions.end() ? std::string{} : creature->second);
            const auto digest = Persistence::WorldSaveService::compose_region_simulation_digest(
                regional_chunks[key],
                regional.ground_object_bytes,
                regional.field_page_bytes,
                regional.creature_bytes);
            state.m_regionContent.emplace(key, digest);
            const auto record = inputs.ledger.records().find(key);
            const auto contribution = record != inputs.ledger.records().end() &&
                                              record->second.state == RegionState::Frozen
                                          ? record->second.frozen_digest
                                          : digest;
            state.m_regionContributions.emplace(key, contribution);
            Append(contents, static_cast<std::uint32_t>(key.x), 4);
            Append(contents, static_cast<std::uint32_t>(key.z), 4);
            Append(contents, contribution, 8);
        }
        Frame(chunk_envelope, "owners", contents);
        state.m_chunkHash = Persistence::StableChecksum(chunk_envelope);
        const auto diagnostics = Persistence::ComputeAuthoritativeChunkSubHashes(all_chunks);
        state.m_terrainHash = diagnostics.terrain;
        state.m_waterHash = diagnostics.water;
        state.m_creatures = creatures.entities.empty()
                                ? std::string{}
                                : luminumbra::ai::SerializeCreatureSnapshot(creatures);
        state.m_plants = plants.entities.empty() ? std::string{}
                                                 : Ecs::SerializeEntityRegistrySnapshotJson(plants);
        state.m_energy = inputs.energy ? inputs.energy->CanonicalRegionalBytes() : std::string{};
        if (!inputs.fields.scent_cells().empty()) {
            state.m_scent = "regional_scent:v2:";
            for (const auto& [cell, values] : inputs.fields.scent_cells()) {
                Append(state.m_scent, static_cast<std::uint32_t>(cell.x), 4);
                Append(state.m_scent, static_cast<std::uint32_t>(cell.z), 4);
                for (const auto value : values)
                    Append(state.m_scent, value, 8);
            }
        }
        Persistence::SimulationPendingState pending;
        if (inputs.pending_events) {
            pending.events = inputs.pending_events->snapshot();
            if (!pending.validate_activations(inputs.ledger))
                throw std::runtime_error("Invalid pending activation authority");
        }
        if (inputs.energy) {
            pending.energy_channels = inputs.energy->channels();
            pending.deposits = inputs.energy->PendingDeposits();
        }
        state.m_pendingEvents = pending.event_bytes();
        state.m_pendingDeposits = pending.deposit_bytes();
        if (!pending.deposits.empty())
            Frame(state.m_energy, "pending_deposits", state.m_pendingDeposits);
        const auto ground_bytes = inputs.ground.canonical_bytes();
        const auto field_bytes = inputs.fields.canonical_bytes();
        state.m_authority = "simulation_authority:v1:";
        Append(state.m_authority, 10, 4);
        Frame(state.m_authority, "clock", inputs.clock_bytes);
        const auto ledger_bytes = inputs.ledger.canonical_bytes();
        Frame(state.m_authority, "ledger", ledger_bytes);
        // The legacy ledger codec omits an empty ledger, including its configuration.
        // Carry those settings once even before any owner has been admitted.
        std::string empty_configuration;
        if (ledger_bytes.empty()) {
            const auto& config = inputs.ledger.config();
            Append(empty_configuration, config.work_limit, 8);
            Append(empty_configuration, config.hold_ticks, 4);
            Append(empty_configuration, config.reduced_shift, 1);
        }
        Frame(state.m_authority, "empty_ledger_configuration", empty_configuration);
        Frame(state.m_authority, "ground", ground_bytes);
        Frame(state.m_authority, "fields", field_bytes);
        Frame(state.m_authority, "energy", state.m_energy);
        Frame(state.m_authority, "creatures", state.m_creatures);
        Frame(state.m_authority, "plants", state.m_plants);
        Frame(state.m_authority, "pending_events", state.m_pendingEvents);
        Frame(state.m_authority, "pending_deposits", state.m_pendingDeposits);
        // Named domains localize authority without adding ComposeWorldHash slots.
        std::string regions = "authority_regions:v1:";
        Append(regions, 4, 4);
        Frame(regions, "clock", inputs.clock_bytes);
        Frame(regions, "ledger", ledger_bytes);
        Frame(regions, "empty_ledger_configuration", empty_configuration);
        Frame(regions, "pending_events", state.m_pendingEvents);
        std::string pages = "authority_pages:v1:";
        Append(pages, 3, 4);
        Frame(pages, "fields", field_bytes);
        Frame(pages, "energy", state.m_energy);
        Frame(pages, "pending_deposits", state.m_pendingDeposits);
        auto& sub = state.m_subHashes;
        sub.active_regions = true;
        sub.terrain = state.m_terrainHash;
        sub.water = state.m_waterHash;
        sub.entities = Persistence::StableChecksum(avatar_bytes.empty() ? empty_avatar_snapshot
                                                                        : inputs.avatar_bytes);
        sub.regions = Persistence::StableChecksum(regions);
        sub.creatures = Persistence::StableChecksum(state.m_creatures);
        sub.items = Persistence::StableChecksum(ground_bytes);
        sub.pages = Persistence::StableChecksum(pages);
        const auto hash_nonempty = [](const std::string& bytes) {
            return bytes.empty() ? std::string{} : Persistence::StableChecksum(bytes);
        };
        sub.aether_state = hash_nonempty(state.m_energy);
        sub.scents = hash_nonempty(state.m_scent);
        sub.plants = hash_nonempty(state.m_plants);
        sub.ecology = state.FoldEcology(
            state.m_creatures.empty()
                ? std::string{}
                : Persistence::StableChecksum("ecology:creatures:v3:" + state.m_creatures));
        state.m_ambient.emplace(std::make_pair(std::uint8_t{3}, false),
                                inputs.fields.ambient_bytes(3, false));
        state.m_ambient.emplace(std::make_pair(std::uint8_t{12}, true),
                                inputs.fields.ambient_bytes(12, true));
        state.m_ambient.emplace(std::make_pair(std::uint8_t{16}, false),
                                inputs.fields.ambient_bytes(16, false));
        return {std::move(state), {}};
    } catch (const std::exception& error) {
        return {std::nullopt, error.what()};
    } catch (...) {
        return {std::nullopt, "Unknown authority capture failure"};
    }
}
} // namespace Luminumbra::world
