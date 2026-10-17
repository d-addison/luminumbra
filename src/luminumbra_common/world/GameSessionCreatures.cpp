#include "../ai/CreatureBrainSystem.h"
#include "../ai/CreatureCoarse.h"
#include "../ai/CreatureGenome.h"
#include "../ai/CreaturePersistence.h"
#include "../ai/CreatureReproductionSystem.h"
#include "../ai/CreatureSpeciesRegistry.h"
#include "../ai/DecompositionSystem.h"
#include "../components/AlarmComponents.h"
#include "../components/CircadianComponents.h"
#include "../components/CoreComponents.h"
#include "../components/CreatureComponents.h"
#include "../components/ForagingComponents.h"
#include "../components/InstinctComponents.h"
#include "../components/MortalComponents.h"
#include "../components/ThirstComponents.h"
#include "../core/DeterministicRng.h"
#include "../persistence/PlantPersistence.h"
#include "../systems/FoliageRegistryStage.h"
#include "../systems/PhysicsSystem.h"
#include "ActiveRegionLedger.h"
#include "DistantPolicy.h"
#include "GameSession.h"
#include "RegionPopulator.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <entt/entity/registry.hpp>
#include <limits>
#include <luminumbra/core/Types.h>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::world {
namespace {
namespace C = Components;
namespace A = luminumbra::ai;
bool ValidCreaturePosition(const Vec3& position) {
    // Match CreaturePersistence's complete Transform domain, including Y.
    return ActiveRegionLedger::valid_anchor(position) && position.y >= -1048576.0f &&
           position.y < 1048576.0f;
}
std::int64_t Bridge(float value, double scale, std::int64_t low, std::int64_t high) {
    const auto rounded = std::round(static_cast<double>(value) * scale);
    if (!std::isfinite(rounded) || rounded < static_cast<double>(low) ||
        rounded > static_cast<double>(high))
        throw std::invalid_argument("Creature bridge outside domain");
    return static_cast<std::int64_t>(rounded);
}
std::uint32_t Pressure(float value) {
    return static_cast<std::uint32_t>(Bridge(value, 1000000.0, 0, 1000000));
}
std::int64_t Horizontal(float value) {
    return Bridge(value, 1000.0, -16777216000ll, 16777215999ll);
}
std::int64_t OwnedHorizontal(float value, std::int32_t owner) {
    const auto rounded = Horizontal(value);
    const auto low = static_cast<std::int64_t>(owner) * A::kCreatureRegionMm;
    // Rounding alone must not transfer a Full creature into another owner.
    // Park the validated bridge inside the float's owner at sub-mm borders.
    return std::clamp(rounded, low, low + A::kCreatureRegionMm - 1);
}
RegionKey QuantizedPositionOwner(const Vec3& position) {
    if (!ValidCreaturePosition(position))
        throw std::invalid_argument("Creature position outside persistence domain");
    const auto owner = ActiveRegionLedger::region_at(position);
    return A::CreatureRegionAtMillimetres(OwnedHorizontal(position.x, owner.x),
                                          OwnedHorizontal(position.z, owner.z));
}
Vec3 ProjectPosition(std::int64_t x, std::int64_t y, std::int64_t z) {
    const auto owner = A::CreatureRegionAtMillimetres(x, z);
    const auto interior = [](float value, std::int32_t region) {
        const float low = static_cast<float>(region) * 512.0f;
        return std::clamp(
            value, low, std::nextafter(low + 512.0f, -std::numeric_limits<float>::infinity()));
    };
    return {interior(static_cast<float>(static_cast<double>(x) / 1000.0), owner.x),
            std::clamp(static_cast<float>(static_cast<double>(y) / 1000.0),
                       -1048576.0f,
                       std::nextafter(1048576.0f, -std::numeric_limits<float>::infinity())),
            interior(static_cast<float>(static_cast<double>(z) / 1000.0), owner.z)};
}
bool Near(const Vec3& position, const Vec3& anchor, std::uint32_t metres) {
    if (!ActiveRegionLedger::valid_anchor(anchor))
        return false;
    const auto dx = Horizontal(position.x) - Horizontal(anchor.x);
    const auto dz = Horizontal(position.z) - Horizontal(anchor.z);
    const auto radius = static_cast<std::int64_t>(metres) * 1000;
    if (dx < -radius || dx > radius || dz < -radius || dz > radius)
        return false;
    return dx * dx + dz * dz <= radius * radius;
}
std::vector<entt::entity> DurableOrder(entt::registry& registry) {
    std::vector<entt::entity> entities;
    for (auto e : registry.view<C::CreatureComponent,
                                C::TransformComponent,
                                C::CreatureRegionComponent,
                                C::CreatureCoarseComponent>())
        entities.push_back(e);
    std::sort(entities.begin(), entities.end(), [&registry](auto a, auto b) {
        return registry.get<C::CreatureRegionComponent>(a).persistent_id <
               registry.get<C::CreatureRegionComponent>(b).persistent_id;
    });
    return entities;
}
A::CreatureGenome Genome(const C::CreatureGenomeComponent& g) {
    return {g.move_speed,
            g.vigilance,
            g.hunger_threshold,
            g.size_scale,
            g.vision_cos_half_fov,
            g.vision_range,
            g.hearing_range};
}
} // namespace

GameSession::CreatureActivity GameSession::GetCreatureActivity() const {
    if (!m_activeRegionsEnabled)
        return {};
    auto result = m_creatureActivity;
    for (auto e :
         m_registry.view<const C::CreatureRegionComponent, const C::CreatureCoarseComponent>()) {
        const auto& owner = m_registry.get<const C::CreatureRegionComponent>(e);
        const auto found = m_activeRegionLedger.records().find({owner.owner_x, owner.owner_z});
        if (found != m_activeRegionLedger.records().end() &&
            found->second.state == RegionState::Frozen)
            ++result.frozen;
        else if (m_registry.get<const C::CreatureCoarseComponent>(e).tier == C::CreatureTier::Full)
            ++result.full;
        else
            ++result.coarse;
    }
    return result;
}

const CreatureDistantPolicy& GameSession::CreaturePolicy(std::uint16_t species) const {
    if (!m_simulationContent || !m_speciesTable)
        throw std::runtime_error("Creature content is not admitted");
    const auto* definition = m_speciesTable->Find(species);
    if (!definition)
        throw std::invalid_argument("Unresolved creature species");
    const auto found = m_simulationContent->policy.creatures.find(definition->id);
    if (found == m_simulationContent->policy.creatures.end())
        throw std::invalid_argument("Unresolved creature policy");
    return found->second;
}

namespace {
void CaptureCreature(entt::registry& registry, entt::entity e) {
    auto next = registry.get<C::CreatureCoarseComponent>(e);
    const auto& position = registry.get<C::TransformComponent>(e).position;
    const auto key = QuantizedPositionOwner(position);
    next.x_mm = OwnedHorizontal(position.x, key.x);
    next.y_mm = Bridge(position.y, 1000.0, -1048576000ll, 1048575999ll);
    next.z_mm = OwnedHorizontal(position.z, key.z);
    const auto& owner = registry.get<C::CreatureRegionComponent>(e);
    if (key != RegionKey{owner.owner_x, owner.owner_z})
        throw std::invalid_argument("Creature owner mismatch");
    next.hunger_ppm = Pressure(registry.get<C::CreatureComponent>(e).hunger);
    if (const auto* thirst = registry.try_get<C::ThirstComponent>(e))
        next.thirst_ppm = Pressure(thirst->thirst);
    next.needs.clear();
    if (const auto* needs = registry.try_get<C::NeedsComponent>(e))
        for (const auto& need : needs->needs)
            next.needs.push_back({need.name, Pressure(need.pressure)});
    registry.get<C::CreatureCoarseComponent>(e) = std::move(next);
}

void StageCreatureAuthority(entt::registry& registry,
                            entt::entity entity,
                            std::uint64_t id,
                            RegionKey key,
                            std::uint64_t tick,
                            bool physics_controlled) {
    registry.emplace<C::CreatureRegionComponent>(
        entity, C::CreatureRegionComponent{id, key.x, key.z, tick});
    registry.emplace<C::CreatureCoarseComponent>(entity);
    registry.emplace<C::CreatureControllerComponent>(
        entity, C::CreatureControllerComponent{physics_controlled});
    if (physics_controlled && !registry.all_of<C::CreatureLocomotionStateComponent>(entity))
        registry.emplace<C::CreatureLocomotionStateComponent>(entity);
    CaptureCreature(registry, entity);
}
} // namespace
void GameSession::CaptureFullCreature(entt::entity e) {
    CaptureCreature(m_registry, e);
}
void GameSession::StagePopulationCreature(entt::registry& stage,
                                          const CreatureSpawnCommand& command,
                                          std::uint64_t id,
                                          RegionKey key,
                                          std::uint64_t tick,
                                          std::uint64_t draw) const {
    if (!m_speciesTable || !ValidCreaturePosition(command.position))
        throw std::invalid_argument("Invalid population creature");
    const auto* definition = m_speciesTable->Find(command.species_id);
    if (!definition)
        throw std::invalid_argument("Unknown population creature species");
    luminumbra::core::DeterministicRng rng(draw);
    auto genome = A::MutateOffspring(A::CreatureGenome{}, rng, definition->genome_ranges);
    // Sensory genes use the same captured species bands and the founder stream.
    genome = A::BreedSensoryInto(genome, genome, genome, rng, definition->genome_ranges);
    A::CreatureBirthProposal founder;
    founder.genome = genome;
    founder.species_id = command.species_id;
    founder.x = command.position.x;
    founder.y = command.position.y;
    founder.z = command.position.z;
    founder.female = (rng.next_u64() & 1ull) != 0;
    const auto entity = A::CreateCreatureBirthEntity(stage, founder, std::uint32_t{0});
    stage.get<C::CreatureComponent>(entity).is_predator = definition->predator;
    stage.emplace<C::CircadianComponent>(
        entity, C::CircadianComponent{static_cast<std::uint8_t>(definition->nocturnal), 1.0f});
    StageCreatureAuthority(stage, entity, id, key, tick, command.physics_controlled);
}

void GameSession::ImportCoarseCreature(entt::entity e) {
    const auto& coarse = m_registry.get<C::CreatureCoarseComponent>(e);
    PublishCreaturePosition(e,
                            ProjectPosition(coarse.x_mm, coarse.y_mm, coarse.z_mm),
                            A::CreatureRegionAtMillimetres(coarse.x_mm, coarse.z_mm));
    m_registry.get<C::CreatureComponent>(e).hunger =
        static_cast<float>(coarse.hunger_ppm) / 1000000.0f;
    if (auto* thirst = m_registry.try_get<C::ThirstComponent>(e))
        thirst->thirst = static_cast<float>(coarse.thirst_ppm) / 1000000.0f;
    if (auto* needs = m_registry.try_get<C::NeedsComponent>(e))
        for (auto& need : needs->needs)
            for (const auto& saved : coarse.needs)
                if (saved.name == need.name)
                    need.pressure = static_cast<float>(saved.pressure_ppm) / 1000000.0f;
}

bool GameSession::AdmitCreature(entt::entity e, bool physics_controlled) {
    if (!m_activeRegionsEnabled || m_regionFault || !m_registry.valid(e) ||
        !m_registry.all_of<C::CreatureComponent, C::TransformComponent>(e) ||
        m_registry.any_of<C::CreatureRegionComponent,
                          C::CreatureCoarseComponent,
                          C::CreatureControllerComponent>(e))
        return false;
    const auto& cr = m_registry.get<C::CreatureComponent>(e);
    const auto position = m_registry.get<C::TransformComponent>(e).position;
    if (!ValidCreaturePosition(position))
        return false;
    CreaturePolicy(cr.species_id);
    const auto key = QuantizedPositionOwner(position);
    const auto found = m_activeRegionLedger.records().find(key);
    if (found == m_activeRegionLedger.records().end() || found->second.state == RegionState::Frozen)
        return false;
    std::uint64_t id = 0;
    if (!m_activeRegionLedger.allocate_persistent_id(id))
        throw std::runtime_error("Creature identity allocation refused");
    const bool had_locomotion = m_registry.all_of<C::CreatureLocomotionStateComponent>(e);
    try {
        StageCreatureAuthority(m_registry, e, id, key, m_worldClock.tick(), physics_controlled);
        const auto snapshot = A::BuildCreatureSnapshot(m_registry);
        const auto plants = luminumbra::foliage::BuildPlantEntitySnapshot(m_registry, true);
        std::string error;
        if (!A::ValidateCreatureSnapshot(
                snapshot, m_activeRegionLedger, m_groundObjects, plants, error))
            throw std::invalid_argument(error);
        return true;
    } catch (...) {
        m_registry.remove<C::CreatureRegionComponent,
                          C::CreatureCoarseComponent,
                          C::CreatureControllerComponent>(e);
        if (!had_locomotion)
            m_registry.remove<C::CreatureLocomotionStateComponent>(e);
        throw;
    }
}

void GameSession::SynchronizeCreatureLocomotion(entt::entity e) {
    if (!m_physicsSystem)
        throw std::runtime_error("Creature physics unavailable");
    const auto handle = m_registry.get<C::CreaturePhysicsComponent>(e).avatar_index;
    if (!m_physicsSystem->valid_avatar_handle(handle)) {
        ++m_creatureActivity.stale_handles;
        throw std::runtime_error("Stale creature avatar handle");
    }
    const auto velocity = m_physicsSystem->get_avatar_velocity(handle);
    auto& state = m_registry.get<C::CreatureLocomotionStateComponent>(e);
    state = {velocity.x, velocity.y, velocity.z, m_physicsSystem->is_avatar_grounded(handle)};
}

void GameSession::ReconcileCreatureTiers() {
    for (auto e : DurableOrder(m_registry)) {
        const auto& owner = m_registry.get<C::CreatureRegionComponent>(e);
        const auto found = m_activeRegionLedger.records().find({owner.owner_x, owner.owner_z});
        auto& coarse = m_registry.get<C::CreatureCoarseComponent>(e);
        const auto* runtime = m_registry.try_get<C::CreaturePhysicsComponent>(e);
        const auto remove_controller = [this, e, runtime] {
            if (runtime && m_physicsSystem) {
                if (m_physicsSystem->remove_avatar_handle(runtime->avatar_index))
                    ++m_creatureActivity.controllers_removed;
                else
                    ++m_creatureActivity.stale_handles;
            }
            m_registry.remove<C::CreaturePhysicsComponent>(e);
        };
        if (found == m_activeRegionLedger.records().end() ||
            found->second.state == RegionState::Frozen) {
            remove_controller(); // Runtime cleanup cannot sample or demote frozen authority.
            continue;
        }
        const auto& policy = CreaturePolicy(m_registry.get<C::CreatureComponent>(e).species_id);
        const auto position = m_registry.get<C::TransformComponent>(e).position;
        if (QuantizedPositionOwner(position) != RegionKey{owner.owner_x, owner.owner_z})
            throw std::invalid_argument("Creature controller restore owner mismatch");
        bool promote = false, retain = false;
        const auto probe = [&](const Vec3& anchor) {
            promote = promote || Near(position, anchor, policy.promote_distance_m);
            retain =
                retain ||
                Near(position, anchor, static_cast<std::uint32_t>(policy.promote_distance_m) + 64u);
        };
        if (m_replicatedSimulationAnchors) {
            for (const auto& anchor : *m_replicatedSimulationAnchors)
                probe(anchor);
        } else {
            const auto& local = m_activeRegionLedger.local_anchor();
            if (local)
                probe(*local);
        }
        const bool active = found->second.state == RegionState::Active;
        if (coarse.tier == C::CreatureTier::Full &&
            (!active || !policy.promote_distance_m || !retain)) {
            if (runtime)
                SynchronizeCreatureLocomotion(e);
            CaptureFullCreature(e);
            ++m_creatureActivity.demotions;
            coarse.tier = C::CreatureTier::Coarse;
            coarse.transition = C::CreatureTransition::None;
        } else if (coarse.tier == C::CreatureTier::Coarse && active && policy.promote_distance_m &&
                   promote) {
            ImportCoarseCreature(e);
            ++m_creatureActivity.promotions;
            coarse.tier = C::CreatureTier::Full;
            coarse.transition = C::CreatureTransition::None;
        }
        const auto* controller = m_registry.try_get<C::CreatureControllerComponent>(e);
        if (coarse.tier != C::CreatureTier::Full || !controller ||
            !controller->physics_controlled || A::DecompIsDead(m_registry, e)) {
            remove_controller();
            continue;
        }
        const auto* state = m_registry.try_get<C::CreatureLocomotionStateComponent>(e);
        if (!state || !m_physicsSystem)
            throw std::runtime_error("Creature controller continuation unavailable");
        if (!runtime) {
            if (m_creatureControllerPreparation)
                m_creatureControllerPreparation();
            const auto handle = m_physicsSystem->create_avatar_handle(
                m_registry.get<C::TransformComponent>(e).position,
                Vec3(state->velocity_x, state->velocity_y, state->velocity_z),
                state->grounded);
            try {
                m_registry.emplace<C::CreaturePhysicsComponent>(e, handle);
                ++m_creatureActivity.controllers_created;
            } catch (...) {
                m_physicsSystem->remove_avatar_handle(handle);
                throw;
            }
        } else if (!m_physicsSystem->valid_avatar_handle(runtime->avatar_index)) {
            ++m_creatureActivity.stale_handles;
            throw std::runtime_error("Stale creature avatar handle");
        }
    }
}

void GameSession::RunCoarseCreatureTick() {
    std::vector<A::CoarseCreature> previous;
    std::map<std::uint64_t, entt::entity> entities;
    for (auto e : DurableOrder(m_registry)) {
        const auto& cr = m_registry.get<C::CreatureComponent>(e);
        A::CoarseCreature row;
        row.region = m_registry.get<C::CreatureRegionComponent>(e);
        row.coarse = m_registry.get<C::CreatureCoarseComponent>(e);
        row.policy = CreaturePolicy(cr.species_id);
        row.species_id = cr.species_id;
        row.alive = !A::DecompIsDead(m_registry, e);
        row.is_predator = cr.is_predator;
        row.stamina_ppm = Pressure(cr.stamina);
        if (const auto* genome = m_registry.try_get<C::CreatureGenomeComponent>(e)) {
            row.has_genome = true;
            row.female = genome->female;
            row.generation = genome->generation;
            row.mate_hunger_limit_ppm = Pressure(genome->hunger_threshold);
        }
        previous.push_back(std::move(row));
        entities.emplace(previous.back().region.persistent_id, e);
    }
    auto result = A::StepCoarseCreatures(previous,
                                         SortedDueRegions(),
                                         m_activeRegionLedger.records(),
                                         static_cast<std::uint64_t>(StringToSeed(m_metadata.seed)),
                                         m_creatureTickAdmission.tick);
    std::vector<entt::entity> children;
    children.reserve(result.births.size());
    try {
        for (const auto& birth : result.births) {
            const auto child = AdmitCoarseBirth(birth, entities);
            if (child == entt::null)
                throw std::runtime_error("Coarse birth admission refused");
            children.push_back(child);
        }
    } catch (...) {
        for (const auto child : children)
            m_registry.destroy(child);
        throw;
    }
    // The entire read pass completes before any owner, death or birth publishes.
    for (auto& row : result.creatures) {
        const auto e = entities.at(row.region.persistent_id);
        const auto& old = m_registry.get<C::CreatureRegionComponent>(e);
        if (old.region_stamp == row.region.region_stamp)
            continue;
        m_registry.get<C::CreatureCoarseComponent>(e) = std::move(row.coarse);
        m_registry.get<C::CreatureRegionComponent>(e) = row.region;
        ImportCoarseCreature(e);
        ++m_creatureActivity.coarse_updates;
        if (m_registry.get<C::CreatureCoarseComponent>(e).travel_pending)
            ++m_creatureActivity.parked_travel;
    }
    m_creatureActivity.births += children.size();
    m_creatureActivity.deaths += result.deaths.size();
    for (const auto id : result.deaths) {
        const auto e = entities.at(id);
        m_registry.get<C::CreatureComponent>(e).eaten = true;
        if (auto* mortal = m_registry.try_get<C::MortalComponent>(e))
            mortal->dead = 1;
    }
    for (const auto& work : result.work) {
        RecordActiveRegionWork(work);
        m_creatureTickAdmission.processed_owners.insert(work.key);
    }
}

entt::entity GameSession::AdmitCoarseBirth(const A::CoarseBirth& birth,
                                           const std::map<std::uint64_t, entt::entity>& parents) {
    const auto mother = parents.at(birth.mother_id), father = parents.at(birth.father_id);
    if (!m_speciesTable)
        throw std::runtime_error("Creature species unavailable");
    const auto* definition = m_speciesTable->Find(birth.species_id);
    if (!definition)
        throw std::invalid_argument("Unresolved birth species");
    auto rng = luminumbra::core::DeterministicRng(birth.seed);
    auto genome = A::BreedOffspring(Genome(m_registry.get<C::CreatureGenomeComponent>(mother)),
                                    Genome(m_registry.get<C::CreatureGenomeComponent>(father)),
                                    rng,
                                    definition->genome_ranges);
    genome = A::BreedSensoryInto(genome,
                                 Genome(m_registry.get<C::CreatureGenomeComponent>(mother)),
                                 Genome(m_registry.get<C::CreatureGenomeComponent>(father)),
                                 rng,
                                 definition->genome_ranges);
    A::CreatureBirthProposal proposal;
    proposal.genome = genome;
    proposal.species_id = birth.species_id;
    const auto position = ProjectPosition(birth.x_mm, birth.y_mm, birth.z_mm);
    proposal.x = position.x;
    proposal.y = position.y;
    proposal.z = position.z;
    proposal.generation = birth.generation;
    proposal.female = birth.female;
    const auto child = A::CreateCreatureBirthEntity(m_registry, proposal, std::uint32_t{0});
    try {
        if (!AdmitCreature(child)) {
            m_registry.destroy(child);
            return entt::null;
        }
        auto& coarse = m_registry.get<C::CreatureCoarseComponent>(child);
        coarse.tier = C::CreatureTier::Coarse;
        coarse.x_mm = birth.x_mm;
        coarse.y_mm = birth.y_mm;
        coarse.z_mm = birth.z_mm;
        RecordActiveRegionWork({birth.owner, 0ull, 1ull, 0ull, 0ull});
        m_creatureTickAdmission.processed_owners.insert(birth.owner);
        return child;
    } catch (...) {
        m_registry.destroy(child);
        throw;
    }
}

void GameSession::RunFullCreatureBrain() {
    // The brain's kinematic path writes its proposed endpoint. Keep that write
    // in a staging registry until the traversed owners and world bounds pass.
    namespace stage = luminumbra::foliage::stage_detail;
    entt::registry proposed;
    for (auto e : DurableOrder(m_registry)) {
        if (!CreatureFullEligible(e))
            continue;
        if (proposed.create(e) != e)
            throw std::runtime_error("Creature brain staging refused");
        stage::Copy<C::CreatureComponent>(m_registry, e, proposed, e);
        stage::Copy<C::TransformComponent>(m_registry, e, proposed, e);
        stage::Copy<C::CreatureRegionComponent>(m_registry, e, proposed, e);
        stage::Copy<C::CreatureGenomeComponent>(m_registry, e, proposed, e);
        stage::Copy<C::CircadianComponent>(m_registry, e, proposed, e);
        stage::Copy<C::ThirstComponent>(m_registry, e, proposed, e);
        stage::Copy<C::AlarmComponent>(m_registry, e, proposed, e);
        stage::Copy<C::CreaturePhysicsComponent>(m_registry, e, proposed, e);
    }
    A::RunCreatureBrainSystemOnTick(
        proposed,
        static_cast<float>(m_simulationClock.fixed_dt()),
        m_ecologyTuning,
        m_scentField.get(),
        ScentDomainOrigin(true),
        ScentDomainOrigin(false),
        ScentCellSize(),
        true,
        m_speciesTable.get(),
        [this](auto e) { return CreatureFullEligible(e); },
        [this](auto e) { RecordCreatureOperation(e); });
    for (auto e : DurableOrder(m_registry)) {
        if (!proposed.valid(e))
            continue;
        CompleteCreatureMovement(e, proposed.get<C::TransformComponent>(e).position);
        stage::Copy<C::CreatureComponent>(proposed, e, m_registry, e);
        stage::Copy<C::AlarmComponent>(proposed, e, m_registry, e);
    }
}

void GameSession::StepCreaturePhysics() {
    std::vector<std::size_t> handles;
    std::vector<entt::entity> entities;
    for (auto e : DurableOrder(m_registry)) {
        const auto* runtime = m_registry.try_get<C::CreaturePhysicsComponent>(e);
        if (!runtime || !m_creatureTickAdmission.full.contains(e) || A::DecompIsDead(m_registry, e))
            continue;
        if (!m_physicsSystem)
            throw std::runtime_error("Creature physics unavailable");
        const auto& cr = m_registry.get<C::CreatureComponent>(e);
        m_physicsSystem->set_avatar_wish_velocity(runtime->avatar_index, {cr.wish_x, cr.wish_z});
        handles.push_back(runtime->avatar_index);
        entities.push_back(e);
    }
    if (handles.empty())
        return;
    if (!m_physicsSystem || !m_physicsSystem->update_selected_avatars(
                                handles, static_cast<float>(m_simulationClock.fixed_dt())))
        throw std::runtime_error("Selected creature physics step refused");
    for (auto e : entities) {
        const auto handle = m_registry.get<C::CreaturePhysicsComponent>(e).avatar_index;
        CompleteCreatureMovement(e, m_physicsSystem->get_avatar_position(handle));
        SynchronizeCreatureLocomotion(e);
        RecordCreatureOperation(e);
    }
}

// Walk every region crossed by the horizontal segment, including both sides of
// corner crossings. Existence and frozen state are independent of due status.
void GameSession::CompleteCreatureMovement(entt::entity e, Vec3 position) {
    const auto& before = m_creatureTickAdmission.previous.at(e);
    // Out-of-world endpoints are sealed movement, not invalid durable state.
    // Refuse the proposal before installing any Transform or owner change.
    if (!ValidCreaturePosition(position))
        position = before.position;
    auto owner = QuantizedPositionOwner(before.position);
    if (owner != before.owner)
        throw std::invalid_argument("Creature movement source owner mismatch");
    if (position == before.position) {
        PublishCreaturePosition(e, position, owner);
        return;
    }
    const auto target = QuantizedPositionOwner(position);
    // The Full segment determines crossing order. Millimetre projection is only
    // a persistence bridge: rounding endpoints can swap near-corner crossings.
    const double x0 = before.position.x, z0 = before.position.z;
    const double x1 = position.x, z1 = position.z;
    const auto dx = x1 - x0, dz = z1 - z0;
    const int sx = (dx > 0) - (dx < 0), sz = (dz > 0) - (dz < 0);
    auto open = [this](RegionKey key) {
        const auto it = m_activeRegionLedger.records().find(key);
        return it != m_activeRegionLedger.records().end() &&
               it->second.state != RegionState::Frozen;
    };
    while (owner != target) {
        const double bx = (static_cast<double>(owner.x) + (sx > 0 ? 1 : 0)) * 512.0;
        const double bz = (static_cast<double>(owner.z) + (sz > 0 ? 1 : 0)) * 512.0;
        const double tx = dx == 0 || owner.x == target.x ? std::numeric_limits<double>::infinity()
                                                         : (bx - x0) / dx;
        const double tz = dz == 0 || owner.z == target.z ? std::numeric_limits<double>::infinity()
                                                         : (bz - z0) / dz;
        auto next = owner;
        if (tx <= tz)
            next.x += sx;
        if (tz <= tx)
            next.z += sz;
        if (!open(next) || (tx == tz && (!open({next.x, owner.z}) || !open({owner.x, next.z})))) {
            const double fraction = std::min(tx, tz);
            if (!std::isfinite(fraction))
                throw std::runtime_error("Creature traversal has no finite crossing");
            position = before.position + static_cast<float>(std::clamp(fraction, 0.0, 1.0)) *
                                             (position - before.position);
            const float low_x = static_cast<float>(owner.x) * 512.0f;
            const float low_z = static_cast<float>(owner.z) * 512.0f;
            const auto inside = [](float value, float low) {
                const float high =
                    std::min(std::nextafter(low + 512.0f, -std::numeric_limits<float>::infinity()),
                             static_cast<float>(static_cast<double>(low) + 511.999));
                return std::clamp(value, low, high);
            };
            position.x = inside(position.x, low_x);
            position.z = inside(position.z, low_z);
            break;
        }
        owner = next;
    }
    // Keep interpolation and float conversion inside the last admitted owner.
    const auto align_owner = [](float value, std::int32_t coordinate) {
        const float low = static_cast<float>(coordinate) * 512.0f;
        return std::clamp(
            value, low, std::nextafter(low + 512.0f, -std::numeric_limits<float>::infinity()));
    };
    position.x = align_owner(position.x, owner.x);
    position.z = align_owner(position.z, owner.z);
    PublishCreaturePosition(e, position, owner);
}

void GameSession::PublishCreaturePosition(entt::entity e, const Vec3& position, RegionKey owner) {
    // Validate before synchronizing runtime physics or publishing durable values.
    // Kinematic movement, controller movement and coarse restore share this gate.
    if (QuantizedPositionOwner(position) != owner)
        throw std::invalid_argument("Creature publication owner mismatch");
    if (const auto* runtime = m_registry.try_get<C::CreaturePhysicsComponent>(e))
        if (!m_physicsSystem ||
            !m_physicsSystem->set_avatar_position(runtime->avatar_index, position))
            throw std::runtime_error("Creature movement reconciliation refused");
    auto& region = m_registry.get<C::CreatureRegionComponent>(e);
    if (region.owner_x != owner.x || region.owner_z != owner.z) {
        region.owner_x = owner.x;
        region.owner_z = owner.z;
    }
    auto& transform = m_registry.get<C::TransformComponent>(e);
    if (transform.position != position)
        transform.position = position;
}
void GameSession::SealCreatureTickAdmission(std::uint64_t tick) {
    namespace C = Components;
    m_creatureTickAdmission = {};
    auto& admission = m_creatureTickAdmission;
    admission.tick = tick;
    const auto due = SortedDueRegions();
    auto active_due = [this, &due](RegionKey key) {
        const auto it = m_activeRegionLedger.records().find(key);
        return it != m_activeRegionLedger.records().end() &&
               it->second.state == RegionState::Active &&
               std::binary_search(due.begin(), due.end(), key);
    };
    auto positional_due = [this, &due](RegionKey key) {
        const auto it = m_activeRegionLedger.records().find(key);
        return it != m_activeRegionLedger.records().end() &&
               it->second.state != RegionState::Frozen &&
               std::binary_search(due.begin(), due.end(), key);
    };
    for (auto e : m_registry.view<C::TransformComponent>()) {
        const auto& tf = m_registry.get<C::TransformComponent>(e);
        const auto* cr = m_registry.try_get<C::CreatureComponent>(e);
        if (!cr) {
            if (ActiveRegionLedger::valid_anchor(tf.position) &&
                positional_due(ActiveRegionLedger::region_at(tf.position))) {
                admission.positional_sources.insert(e);
                admission.positional_owners.emplace(e, ActiveRegionLedger::region_at(tf.position));
            }
            continue;
        }
        const auto* owner = m_registry.try_get<C::CreatureRegionComponent>(e);
        const auto* coarse = m_registry.try_get<C::CreatureCoarseComponent>(e);
        if (!owner && !coarse && cr->species_id == 0) {
            if (ActiveRegionLedger::valid_anchor(tf.position) &&
                positional_due(ActiveRegionLedger::region_at(tf.position))) {
                admission.transient_grazers.insert(e);
                admission.positional_owners.emplace(e, ActiveRegionLedger::region_at(tf.position));
            }
            continue;
        }
        if (!owner || !coarse || !owner->persistent_id || !m_speciesTable ||
            !m_speciesTable->Find(cr->species_id))
            continue;
        const RegionKey key{owner->owner_x, owner->owner_z};
        admission.previous.emplace(
            e,
            CreatureTickAdmission::Previous{key,
                                            coarse->tier,
                                            owner->persistent_id,
                                            tf.position,
                                            !A::DecompIsDead(m_registry, e)});
        if (owner->region_stamp >= tick)
            continue;
        if (coarse->tier == C::CreatureTier::Full && active_due(key))
            admission.full.insert(e);
        if (coarse->tier != C::CreatureTier::Coarse || !luminumbra::ai::DecompIsDead(m_registry, e))
            continue;
        const auto it = m_activeRegionLedger.records().find(key);
        if (it == m_activeRegionLedger.records().end() || it->second.state == RegionState::Frozen ||
            !std::binary_search(due.begin(), due.end(), key))
            continue;
        const auto shift = it->second.cadence_shift;
        if (owner->region_stamp == 0 || (owner->region_stamp >> shift) < (tick >> shift))
            admission.coarse_corpses.insert(e);
    }
    const auto seal_grid_source = [this, &admission](entt::entity e, int x, int z) {
        if (!m_registry.any_of<C::CreatureComponent, C::TransformComponent>(e) &&
            ScentCellDue(x, z)) {
            admission.positional_sources.insert(e);
            admission.positional_owners.emplace(e, ScentCellOwner(x, z));
        }
    };
    for (auto e : m_registry.view<const C::ForagerComponent>()) {
        const auto& a = m_registry.get<const C::ForagerComponent>(e);
        seal_grid_source(e, a.cell_x, a.cell_z);
    }
    for (auto e : m_registry.view<const C::FoodSourceComponent>()) {
        const auto& f = m_registry.get<const C::FoodSourceComponent>(e);
        seal_grid_source(e, f.cell_x, f.cell_z);
    }
}
bool GameSession::CreatureFullEligible(entt::entity entity) const {
    return m_creatureTickAdmission.full.contains(entity) ||
           m_creatureTickAdmission.positional_sources.contains(entity);
}
bool GameSession::CreatureGrazerEligible(entt::entity entity) const {
    return m_creatureTickAdmission.full.contains(entity) ||
           m_creatureTickAdmission.transient_grazers.contains(entity);
}
void GameSession::RecordCreatureOperation(entt::entity entity) {
    const auto it = m_creatureTickAdmission.previous.find(entity);
    if (it != m_creatureTickAdmission.previous.end()) {
        if (m_creatureTickAdmission.processed.insert(entity).second) {
            if (m_creatureTickAdmission.full.contains(entity))
                ++m_creatureActivity.full_updates;
            else if (m_creatureTickAdmission.coarse_corpses.contains(entity))
                ++m_creatureActivity.coarse_updates;
        }
        m_creatureTickAdmission.processed_owners.insert(it->second.owner);
        RecordActiveRegionWork({it->second.owner, 0ull, 1ull, 0ull, 0ull});
    } else if (m_creatureTickAdmission.transient_grazers.contains(entity) ||
               m_creatureTickAdmission.positional_sources.contains(entity)) {
        const auto key = m_creatureTickAdmission.positional_owners.at(entity);
        m_creatureTickAdmission.processed_owners.insert(key);
        RecordActiveRegionWork({key, 0ull, 1ull, 0ull, 0ull});
    }
}
void GameSession::CompleteCreatureTickAdmission() {
    for (const auto& [entity, previous] : m_creatureTickAdmission.previous) {
        if (!m_registry.valid(entity))
            ++m_creatureActivity.removals;
        else if (previous.alive && previous.tier == C::CreatureTier::Full &&
                 A::DecompIsDead(m_registry, entity))
            ++m_creatureActivity.deaths;
    }
    for (const auto e : m_creatureTickAdmission.processed) {
        if (!m_registry.valid(e))
            continue;
        if (auto* owner = m_registry.try_get<Components::CreatureRegionComponent>(e))
            owner->region_stamp = m_creatureTickAdmission.tick;
    }
    for (const auto key : m_creatureTickAdmission.processed_owners)
        RecordActiveRegionWork({key, 1, 0, 0, 0});
}
bool GameSession::AdmitCreatureBirth(const luminumbra::ai::CreatureBirthProposal& birth) {
    namespace C = Components;
    const auto tick = m_creatureTickAdmission.tick;
    const Vec3 position(birth.x, birth.y, birth.z);
    if (!m_activeRegionsEnabled || m_regionFault || !ValidCreaturePosition(position) ||
        !m_speciesTable || !m_speciesTable->Find(birth.species_id) ||
        !luminumbra::ai::AdmitFullCreatureBirthDestination(
            m_activeRegionLedger, SortedDueRegions(), position))
        return false;
    std::uint64_t id = 0;
    if (!m_activeRegionLedger.allocate_persistent_id(id))
        throw std::runtime_error("Creature identity allocation refused");
    entt::entity child = entt::null;
    try {
        child = luminumbra::ai::CreateCreatureBirthEntity(
            m_registry, birth, m_reproductionTuning.cooldown_ticks);
        const auto key = QuantizedPositionOwner(position);
        m_registry.emplace<C::CreatureRegionComponent>(child, id, key.x, key.z, tick);
        m_registry.emplace<C::CreatureCoarseComponent>(child);
        m_registry.emplace<C::CreatureControllerComponent>(child); // Explicitly kinematic newborn.
        CaptureFullCreature(child);
        // The existing snapshot validation checks all admission bridges and inventory.
        const auto snapshot = luminumbra::ai::BuildCreatureSnapshot(m_registry);
        const auto plants = luminumbra::foliage::BuildPlantEntitySnapshot(m_registry, true);
        std::string error;
        if (!luminumbra::ai::ValidateCreatureSnapshot(
                snapshot, m_activeRegionLedger, m_groundObjects, plants, error)) {
            m_registry.destroy(child);
            return false;
        }
        RecordActiveRegionWork({key, 0, 1, 0, 0});
        m_creatureTickAdmission.processed_owners.insert(key);
        ++m_creatureActivity.births;
        return true;
    } catch (...) {
        if (child != entt::null && m_registry.valid(child))
            m_registry.destroy(child);
        throw;
    }
}

} // namespace Luminumbra::world
