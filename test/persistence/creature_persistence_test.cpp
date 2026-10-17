#include <gtest/gtest.h>

#include "ai/CreaturePersistence.h"
#include "ai/DecompositionSystem.h"
#include "ai/LifespanSystem.h"
#include "ai/StimulusChannels.h"
#include "components/AlarmComponents.h"
#include "components/CircadianComponents.h"
#include "components/CombustionComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/DecayComponents.h"
#include "components/FieldEmitterComponents.h"
#include "components/ForagingComponents.h"
#include "components/GrazeableComponent.h"
#include "components/InstinctComponents.h"
#include "components/IrrigationComponents.h"
#include "components/MigratoryComponents.h"
#include "components/MortalComponents.h"
#include "components/PackHunterComponents.h"
#include "components/PlantComponents.h"
#include "components/ScavengerComponent.h"
#include "components/TerritoryComponents.h"
#include "components/ThirstComponents.h"
#include "core/WaterComponents.h"
#include "persistence/PlantPersistence.h"
#include "world/ActiveRegionLedger.h"
#include "world/GroundObjectStore.h"
#include "world/WorldClock.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <entt/entt.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
namespace A = luminumbra::ai;
namespace C = Luminumbra::Components;
namespace F = luminumbra::foliage;
using Luminumbra::world::ActiveRegionLedger;
using Luminumbra::world::GroundObjectStore;
using Luminumbra::world::WorldClock;

entt::entity Creature(entt::registry& r, std::uint64_t id) {
    const auto e = r.create();
    r.emplace<C::TransformComponent>(e);
    r.emplace<C::CreatureComponent>(e).species_id = 1;
    r.emplace<C::CreatureRegionComponent>(e).persistent_id = id;
    r.emplace<C::CreatureCoarseComponent>(e);
    return e;
}
void Plant(entt::registry& r, std::uint64_t id) {
    const auto e = r.create();
    r.emplace<C::TransformComponent>(e);
    r.emplace<C::PlantTag>(e);
    r.emplace<C::PlantGenomeComponent>(e);
    r.emplace<C::PlantGrowthComponent>(e);
    r.emplace<C::PlantRegionComponent>(e).persistent_id = id;
}
ActiveRegionLedger Ledger(std::uint64_t last_id) {
    const WorldClock clock(10);
    ActiveRegionLedger ledger({}, clock);
    ledger.mark_edited({0, 0}, clock);
    for (std::uint64_t i = 0, id = 0; i < last_id; ++i)
        EXPECT_TRUE(ledger.allocate_persistent_id(id));
    return ledger;
}
template<class T>
void Nondefault(T& v) {
    if constexpr (std::is_floating_point_v<T>)
        v = static_cast<T>(0.375);
    else if constexpr (std::is_same_v<T, bool>)
        v = true;
    else if constexpr (std::is_integral_v<T>)
        v = 1;
    else if constexpr (std::is_enum_v<T>)
        v = static_cast<T>(1);
    else if constexpr (std::is_same_v<T, std::string>)
        v = "sample";
}
void EveryComponent(entt::registry& r, entt::entity e, entt::entity target) {
    {
        auto& c = r.get_or_emplace<C::TransformComponent>(e);
        Nondefault(c.position.x);
        Nondefault(c.position.y);
        Nondefault(c.position.z);
        Nondefault(c.rotation.w);
        Nondefault(c.rotation.x);
        Nondefault(c.rotation.y);
        Nondefault(c.rotation.z);
        Nondefault(c.scale.x);
        Nondefault(c.scale.y);
        Nondefault(c.scale.z);
    }
    {
        auto& c = r.get_or_emplace<C::CreatureRegionComponent>(e);
        Nondefault(c.persistent_id);
        Nondefault(c.owner_x);
        Nondefault(c.owner_z);
        Nondefault(c.region_stamp);
    }
    {
        auto& c = r.get_or_emplace<C::CreatureCoarseComponent>(e);
        Nondefault(c.tier);
        Nondefault(c.transition);
        Nondefault(c.x_mm);
        Nondefault(c.y_mm);
        Nondefault(c.z_mm);
        Nondefault(c.hunger_ppm);
        Nondefault(c.thirst_ppm);
        Nondefault(c.needs);
        Nondefault(c.age_cadences);
        Nondefault(c.reproduce_cadences);
        Nondefault(c.reproduce_cooldown_cadences);
        Nondefault(c.courting_cadences);
        Nondefault(c.travel_pending);
        Nondefault(c.destination_x);
        Nondefault(c.destination_z);
    }
    {
        auto& c = r.get_or_emplace<C::CreatureLocomotionStateComponent>(e);
        Nondefault(c.velocity_x);
        Nondefault(c.velocity_y);
        Nondefault(c.velocity_z);
        Nondefault(c.grounded);
    }
    {
        auto& c = r.get_or_emplace<C::CreatureComponent>(e);
        Nondefault(c.is_predator);
        Nondefault(c.species_id);
        Nondefault(c.hunger);
        Nondefault(c.stamina);
        Nondefault(c.energy);
        Nondefault(c.move_speed);
        Nondefault(c.last_action);
        Nondefault(c.wish_x);
        Nondefault(c.wish_z);
        Nondefault(c.eaten);
    }
    {
        auto& c = r.get_or_emplace<C::CreatureGenomeComponent>(e);
        Nondefault(c.move_speed);
        Nondefault(c.vigilance);
        Nondefault(c.hunger_threshold);
        Nondefault(c.size_scale);
        Nondefault(c.vision_cos_half_fov);
        Nondefault(c.vision_range);
        Nondefault(c.hearing_range);
        Nondefault(c.age_ticks);
        Nondefault(c.reproduce_cooldown);
        Nondefault(c.generation);
        Nondefault(c.female);
        Nondefault(c.courting_ticks);
    }
    {
        auto& c = r.get_or_emplace<C::MortalComponent>(e);
        Nondefault(c.age_ticks);
        Nondefault(c.lifespan_ticks);
        Nondefault(c.dead);
    }
    {
        auto& c = r.get_or_emplace<C::DecayComponent>(e);
        Nondefault(c.decay_ticks);
        Nondefault(c.decay_duration);
        Nondefault(c.nutrient_released_milli);
        Nondefault(c.fully_decomposed);
    }
    {
        auto& c = r.get_or_emplace<C::ThirstComponent>(e);
        Nondefault(c.thirst);
        Nondefault(c.wish_x);
        Nondefault(c.wish_z);
        Nondefault(c.drinking);
        Nondefault(c.water_proximity);
    }
    {
        auto& c = r.get_or_emplace<C::CircadianComponent>(e);
        Nondefault(c.nocturnal);
        Nondefault(c.activity);
    }
    {
        auto& c = r.get_or_emplace<C::AlarmComponent>(e);
        Nondefault(c.level);
        Nondefault(c.alarmed);
    }
    {
        auto& c = r.get_or_emplace<C::PackHunterComponent>(e);
        Nondefault(c.coord_x);
        Nondefault(c.coord_z);
        Nondefault(c.in_pack);
    }
    {
        auto& c = r.get_or_emplace<C::TerritoryComponent>(e);
        Nondefault(c.home_x);
        Nondefault(c.home_z);
        Nondefault(c.radius);
        Nondefault(c.established);
    }
    {
        auto& c = r.get_or_emplace<C::TerritoryBiasComponent>(e);
        Nondefault(c.wish_x);
        Nondefault(c.wish_z);
    }
    {
        auto& c = r.get_or_emplace<C::MigratoryComponent>(e);
        Nondefault(c.drive);
        Nondefault(c.wish_x);
        Nondefault(c.wish_z);
    }
    {
        auto& c = r.get_or_emplace<C::ScavengerComponent>(e);
        Nondefault(c.wish_x);
        Nondefault(c.wish_z);
        Nondefault(c.feeding);
    }
    {
        auto& c = r.get_or_emplace<C::NeedsComponent>(e);
        Nondefault(c.needs);
    }
    {
        auto& c = r.get_or_emplace<C::SensesComponent>(e);
        c.nearest_food_source = target;
        c.nearest_water_source = target;
        c.nearest_threat = target;
        Nondefault(c.threat_distance);
    }
    {
        auto& c = r.get_or_emplace<C::OpportunityComponent>(e);
        Nondefault(c.id);
        Nondefault(c.action);
        Nondefault(c.target);
        Nondefault(c.need);
        Nondefault(c.satisfaction);
        Nondefault(c.urgency);
        Nondefault(c.risk);
        Nondefault(c.stamina_cost);
        Nondefault(c.radius);
    }
    {
        auto& c = r.get_or_emplace<C::ActionPlanComponent>(e);
        Nondefault(c.plan);
        Nondefault(c.current_action_index);
        Nondefault(c.time_in_current_action);
    }
    { r.get_or_emplace<C::InstinctAgent>(e); }
    {
        auto& c = r.get_or_emplace<C::LocomotionProfile>(e);
        Nondefault(c.move_speed);
        Nondefault(c.arrival_radius);
        Nondefault(c.slow_radius);
        Nondefault(c.separation_radius);
        Nondefault(c.separation_strength);
        Nondefault(c.flock_radius);
        Nondefault(c.cohesion_strength);
        Nondefault(c.alignment_strength);
    }
    {
        auto& c = r.get_or_emplace<C::SensableComponent>(e);
        Nondefault(c.scent_channel);
        Nondefault(c.scent_deposit);
        Nondefault(c.noise_loudness);
        Nondefault(c.noise_pitch);
        Nondefault(c.faction);
    }
    {
        auto& c = r.get_or_emplace<C::PerceptionComponent>(e);
        Nondefault(c.vision_cos_half_fov);
        Nondefault(c.vision_range);
        Nondefault(c.facing_x);
        Nondefault(c.facing_z);
        Nondefault(c.ear.range);
        Nondefault(c.ear.peak_pitch);
        Nondefault(c.ear.bandwidth);
        Nondefault(c.ear.gain);
        Nondefault(c.ear.threshold);
        Nondefault(c.faction);
    }
    {
        auto& c = r.get_or_emplace<C::AwarenessComponent>(e);
        Nondefault(c.awareness.meter);
        Nondefault(c.awareness.state);
        Nondefault(c.awareness.has_last_known);
        Nondefault(c.awareness.last_known_x);
        Nondefault(c.awareness.last_known_z);
        Nondefault(c.awareness.time_since_signal);
        Nondefault(c.params.gain);
        Nondefault(c.params.decay);
        Nondefault(c.params.suspicious_at);
        Nondefault(c.params.alert_at);
        Nondefault(c.params.engaged_at);
    }
    {
        auto& c = r.get_or_emplace<C::LocomotionIntentComponent>(e);
        Nondefault(c.wish_xz.x);
        Nondefault(c.wish_xz.y);
        Nondefault(c.arrived);
    }
    {
        auto& c = r.get_or_emplace<C::ScentSenseComponent>(e);
        Nondefault(c.channel);
        Nondefault(c.sign);
        Nondefault(c.strength);
        Nondefault(c.floor);
        Nondefault(c.weber_k);
    }
    {
        auto& c = r.get_or_emplace<C::LocomotionPathComponent>(e);
        Nondefault(c.waypoints);
        Nondefault(c.index);
    }
    {
        auto& c = r.get_or_emplace<C::StimulusSubscriptionComponent>(e);
        Nondefault(c.subscriptions);
    }
    {
        auto& c = r.get_or_emplace<C::InstinctAgentComponent>(e);
        Nondefault(c.actor_id);
        Nondefault(c.archetype);
        Nondefault(c.replan_interval_ticks);
        Nondefault(c.last_planned_tick);
        Nondefault(c.plans_executed);
        Nondefault(c.current_plan);
    }
    {
        auto& c = r.get_or_emplace<C::ForagerComponent>(e);
        Nondefault(c.cell_x);
        Nondefault(c.cell_z);
        Nondefault(c.home_x);
        Nondefault(c.home_z);
        Nondefault(c.carrying_food);
        Nondefault(c.deliveries);
    }
    {
        auto& c = r.get_or_emplace<C::FoodSourceComponent>(e);
        Nondefault(c.cell_x);
        Nondefault(c.cell_z);
        Nondefault(c.amount);
    }
    {
        auto& c = r.get_or_emplace<C::FieldEmitterComponent>(e);
        Nondefault(c.channel);
        Nondefault(c.rate_raw_per_tick);
        Nondefault(c.radius_cells);
    }
    { r.get_or_emplace<C::PendingDeletion>(e); }
    {
        auto& c = r.get_or_emplace<C::GrazeableComponent>(e);
        Nondefault(c.biomass);
        Nondefault(c.last_grazed_tick);
    }
    {
        auto& c = r.get_or_emplace<C::WaterHoleComponent>(e);
        Nondefault(c.radius);
    }
    {
        auto& c = r.get_or_emplace<C::CombustibleComponent>(e);
        Nondefault(c.fuel_milli);
        Nondefault(c.moisture_milli);
        Nondefault(c.burn_state);
        Nondefault(c.burn_ticks_remaining);
        Nondefault(c.ignition_radius);
    }
    {
        auto& c = r.get_or_emplace<C::IgnitionSourceComponent>(e);
        Nondefault(c.radius);
        Nondefault(c.intensity);
    }
    {
        auto& c = r.get_or_emplace<C::WaterSourceComponent>(e);
        Nondefault(c.output);
    }
    {
        auto& c = r.get_or_emplace<C::WaterFlowSourceComponent>(e);
        Nondefault(c.flow_rate);
    }
    auto& owner = r.get<C::CreatureRegionComponent>(e);
    owner.persistent_id = 1;
    owner.owner_x = owner.owner_z = 0;
    auto& coarse = r.get<C::CreatureCoarseComponent>(e);
    coarse.needs = {{"hunger", 123456}, {"curiosity", 234567}};
    r.get<C::ScentSenseComponent>(e).sign = -1.0f;
    r.get<C::TransformComponent>(e).position.z = std::bit_cast<float>(0x80000000u);
    r.get<C::CreatureComponent>(e).wish_z = std::bit_cast<float>(0x80000000u);
    r.get<C::NeedsComponent>(e).needs = {{"hunger", 0.3125f, 0.0009765625f}};
    auto& plan = r.get<C::ActionPlanComponent>(e);
    plan.plan = {{"move", target, 0.5f, true}, {"eat", entt::null, 0.25f, false}};
    r.get<C::LocomotionPathComponent>(e).waypoints = {{1.25f, -2.5f}, {3.75f, -4.5f}};
    r.get<C::StimulusSubscriptionComponent>(e).subscriptions = {
        {A::StimulusChannel::Weather, "safety", 0.125f}};
    auto& instinct = r.get<C::InstinctAgentComponent>(e);
    instinct.current_plan = {"actor", "archetype", "contract", {}, 0, "checksum", true};
    instinct.current_plan.candidates = {
        {1, "food", "eat", "plant", "hunger", 0.12500000000000003, 0.37500000000000006, "ranked"}};
    r.get<C::DecayComponent>(e).decay_duration = 100;
    r.get<C::DecayComponent>(e).fully_decomposed = 0;
    r.get<C::MortalComponent>(e).lifespan_ticks = 30;
    // Runtime handles must not leak into the record.
    r.emplace<C::CreaturePhysicsComponent>(e).avatar_index = 9876;
}

template<class T>
void ExpectValue(const T& actual, const T& expected) {
    if constexpr (std::is_floating_point_v<T>) {
        using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
        EXPECT_EQ(std::bit_cast<Bits>(actual), std::bit_cast<Bits>(expected));
    } else {
        EXPECT_EQ(actual, expected);
    }
}

// Independent source-field expectations: these never pass through the persistence codec.
void ExpectEveryField(const entt::registry& original,
                      entt::entity source,
                      const entt::registry& restored,
                      entt::entity destination) {
    {
        SCOPED_TRACE("TransformComponent");
        const auto& expected = original.get<C::TransformComponent>(source);
        const auto& actual = restored.get<C::TransformComponent>(destination);
        ExpectValue(actual.position.x, expected.position.x);
        ExpectValue(actual.position.y, expected.position.y);
        ExpectValue(actual.position.z, expected.position.z);
        ExpectValue(actual.rotation.w, expected.rotation.w);
        ExpectValue(actual.rotation.x, expected.rotation.x);
        ExpectValue(actual.rotation.y, expected.rotation.y);
        ExpectValue(actual.rotation.z, expected.rotation.z);
        ExpectValue(actual.scale.x, expected.scale.x);
        ExpectValue(actual.scale.y, expected.scale.y);
        ExpectValue(actual.scale.z, expected.scale.z);
    }
    {
        SCOPED_TRACE("CreatureRegionComponent");
        const auto& expected = original.get<C::CreatureRegionComponent>(source);
        const auto& actual = restored.get<C::CreatureRegionComponent>(destination);
        ExpectValue(actual.persistent_id, expected.persistent_id);
        ExpectValue(actual.owner_x, expected.owner_x);
        ExpectValue(actual.owner_z, expected.owner_z);
        ExpectValue(actual.region_stamp, expected.region_stamp);
    }
    {
        SCOPED_TRACE("CreatureCoarseComponent");
        const auto& expected = original.get<C::CreatureCoarseComponent>(source);
        const auto& actual = restored.get<C::CreatureCoarseComponent>(destination);
        ExpectValue(actual.tier, expected.tier);
        ExpectValue(actual.transition, expected.transition);
        ExpectValue(actual.x_mm, expected.x_mm);
        ExpectValue(actual.y_mm, expected.y_mm);
        ExpectValue(actual.z_mm, expected.z_mm);
        ExpectValue(actual.hunger_ppm, expected.hunger_ppm);
        ExpectValue(actual.thirst_ppm, expected.thirst_ppm);
        ExpectValue(actual.age_cadences, expected.age_cadences);
        ExpectValue(actual.reproduce_cadences, expected.reproduce_cadences);
        ExpectValue(actual.reproduce_cooldown_cadences, expected.reproduce_cooldown_cadences);
        ExpectValue(actual.courting_cadences, expected.courting_cadences);
        ExpectValue(actual.travel_pending, expected.travel_pending);
        ExpectValue(actual.destination_x, expected.destination_x);
        ExpectValue(actual.destination_z, expected.destination_z);
    }
    {
        SCOPED_TRACE("CreatureLocomotionStateComponent");
        const auto& expected = original.get<C::CreatureLocomotionStateComponent>(source);
        const auto& actual = restored.get<C::CreatureLocomotionStateComponent>(destination);
        ExpectValue(actual.velocity_x, expected.velocity_x);
        ExpectValue(actual.velocity_y, expected.velocity_y);
        ExpectValue(actual.velocity_z, expected.velocity_z);
        ExpectValue(actual.grounded, expected.grounded);
    }
    {
        SCOPED_TRACE("CreatureComponent");
        const auto& expected = original.get<C::CreatureComponent>(source);
        const auto& actual = restored.get<C::CreatureComponent>(destination);
        ExpectValue(actual.is_predator, expected.is_predator);
        ExpectValue(actual.species_id, expected.species_id);
        ExpectValue(actual.hunger, expected.hunger);
        ExpectValue(actual.stamina, expected.stamina);
        ExpectValue(actual.energy, expected.energy);
        ExpectValue(actual.move_speed, expected.move_speed);
        ExpectValue(actual.last_action, expected.last_action);
        ExpectValue(actual.wish_x, expected.wish_x);
        ExpectValue(actual.wish_z, expected.wish_z);
        ExpectValue(actual.eaten, expected.eaten);
    }
    {
        SCOPED_TRACE("CreatureGenomeComponent");
        const auto& expected = original.get<C::CreatureGenomeComponent>(source);
        const auto& actual = restored.get<C::CreatureGenomeComponent>(destination);
        ExpectValue(actual.move_speed, expected.move_speed);
        ExpectValue(actual.vigilance, expected.vigilance);
        ExpectValue(actual.hunger_threshold, expected.hunger_threshold);
        ExpectValue(actual.size_scale, expected.size_scale);
        ExpectValue(actual.vision_cos_half_fov, expected.vision_cos_half_fov);
        ExpectValue(actual.vision_range, expected.vision_range);
        ExpectValue(actual.hearing_range, expected.hearing_range);
        ExpectValue(actual.age_ticks, expected.age_ticks);
        ExpectValue(actual.reproduce_cooldown, expected.reproduce_cooldown);
        ExpectValue(actual.generation, expected.generation);
        ExpectValue(actual.female, expected.female);
        ExpectValue(actual.courting_ticks, expected.courting_ticks);
    }
    {
        SCOPED_TRACE("MortalComponent");
        const auto& expected = original.get<C::MortalComponent>(source);
        const auto& actual = restored.get<C::MortalComponent>(destination);
        ExpectValue(actual.age_ticks, expected.age_ticks);
        ExpectValue(actual.lifespan_ticks, expected.lifespan_ticks);
        ExpectValue(actual.dead, expected.dead);
    }
    {
        SCOPED_TRACE("DecayComponent");
        const auto& expected = original.get<C::DecayComponent>(source);
        const auto& actual = restored.get<C::DecayComponent>(destination);
        ExpectValue(actual.decay_ticks, expected.decay_ticks);
        ExpectValue(actual.decay_duration, expected.decay_duration);
        ExpectValue(actual.nutrient_released_milli, expected.nutrient_released_milli);
        ExpectValue(actual.fully_decomposed, expected.fully_decomposed);
    }
    {
        SCOPED_TRACE("ThirstComponent");
        const auto& expected = original.get<C::ThirstComponent>(source);
        const auto& actual = restored.get<C::ThirstComponent>(destination);
        ExpectValue(actual.thirst, expected.thirst);
        ExpectValue(actual.wish_x, expected.wish_x);
        ExpectValue(actual.wish_z, expected.wish_z);
        ExpectValue(actual.drinking, expected.drinking);
        ExpectValue(actual.water_proximity, expected.water_proximity);
    }
    {
        SCOPED_TRACE("CircadianComponent");
        const auto& expected = original.get<C::CircadianComponent>(source);
        const auto& actual = restored.get<C::CircadianComponent>(destination);
        ExpectValue(actual.nocturnal, expected.nocturnal);
        ExpectValue(actual.activity, expected.activity);
    }
    {
        SCOPED_TRACE("AlarmComponent");
        const auto& expected = original.get<C::AlarmComponent>(source);
        const auto& actual = restored.get<C::AlarmComponent>(destination);
        ExpectValue(actual.level, expected.level);
        ExpectValue(actual.alarmed, expected.alarmed);
    }
    {
        SCOPED_TRACE("PackHunterComponent");
        const auto& expected = original.get<C::PackHunterComponent>(source);
        const auto& actual = restored.get<C::PackHunterComponent>(destination);
        ExpectValue(actual.coord_x, expected.coord_x);
        ExpectValue(actual.coord_z, expected.coord_z);
        ExpectValue(actual.in_pack, expected.in_pack);
    }
    {
        SCOPED_TRACE("TerritoryComponent");
        const auto& expected = original.get<C::TerritoryComponent>(source);
        const auto& actual = restored.get<C::TerritoryComponent>(destination);
        ExpectValue(actual.home_x, expected.home_x);
        ExpectValue(actual.home_z, expected.home_z);
        ExpectValue(actual.radius, expected.radius);
        ExpectValue(actual.established, expected.established);
    }
    {
        SCOPED_TRACE("TerritoryBiasComponent");
        const auto& expected = original.get<C::TerritoryBiasComponent>(source);
        const auto& actual = restored.get<C::TerritoryBiasComponent>(destination);
        ExpectValue(actual.wish_x, expected.wish_x);
        ExpectValue(actual.wish_z, expected.wish_z);
    }
    {
        SCOPED_TRACE("MigratoryComponent");
        const auto& expected = original.get<C::MigratoryComponent>(source);
        const auto& actual = restored.get<C::MigratoryComponent>(destination);
        ExpectValue(actual.drive, expected.drive);
        ExpectValue(actual.wish_x, expected.wish_x);
        ExpectValue(actual.wish_z, expected.wish_z);
    }
    {
        SCOPED_TRACE("ScavengerComponent");
        const auto& expected = original.get<C::ScavengerComponent>(source);
        const auto& actual = restored.get<C::ScavengerComponent>(destination);
        ExpectValue(actual.wish_x, expected.wish_x);
        ExpectValue(actual.wish_z, expected.wish_z);
        ExpectValue(actual.feeding, expected.feeding);
    }
    {
        SCOPED_TRACE("SensesComponent");
        const auto& expected = original.get<C::SensesComponent>(source);
        const auto& actual = restored.get<C::SensesComponent>(destination);
        ExpectValue(actual.threat_distance, expected.threat_distance);
    }
    {
        SCOPED_TRACE("OpportunityComponent");
        const auto& expected = original.get<C::OpportunityComponent>(source);
        const auto& actual = restored.get<C::OpportunityComponent>(destination);
        ExpectValue(actual.id, expected.id);
        ExpectValue(actual.action, expected.action);
        ExpectValue(actual.target, expected.target);
        ExpectValue(actual.need, expected.need);
        ExpectValue(actual.satisfaction, expected.satisfaction);
        ExpectValue(actual.urgency, expected.urgency);
        ExpectValue(actual.risk, expected.risk);
        ExpectValue(actual.stamina_cost, expected.stamina_cost);
        ExpectValue(actual.radius, expected.radius);
    }
    {
        SCOPED_TRACE("ActionPlanComponent");
        const auto& expected = original.get<C::ActionPlanComponent>(source);
        const auto& actual = restored.get<C::ActionPlanComponent>(destination);
        ExpectValue(actual.current_action_index, expected.current_action_index);
        ExpectValue(actual.time_in_current_action, expected.time_in_current_action);
    }
    {
        SCOPED_TRACE("LocomotionProfile");
        const auto& expected = original.get<C::LocomotionProfile>(source);
        const auto& actual = restored.get<C::LocomotionProfile>(destination);
        ExpectValue(actual.move_speed, expected.move_speed);
        ExpectValue(actual.arrival_radius, expected.arrival_radius);
        ExpectValue(actual.slow_radius, expected.slow_radius);
        ExpectValue(actual.separation_radius, expected.separation_radius);
        ExpectValue(actual.separation_strength, expected.separation_strength);
        ExpectValue(actual.flock_radius, expected.flock_radius);
        ExpectValue(actual.cohesion_strength, expected.cohesion_strength);
        ExpectValue(actual.alignment_strength, expected.alignment_strength);
    }
    {
        SCOPED_TRACE("SensableComponent");
        const auto& expected = original.get<C::SensableComponent>(source);
        const auto& actual = restored.get<C::SensableComponent>(destination);
        ExpectValue(actual.scent_channel, expected.scent_channel);
        ExpectValue(actual.scent_deposit, expected.scent_deposit);
        ExpectValue(actual.noise_loudness, expected.noise_loudness);
        ExpectValue(actual.noise_pitch, expected.noise_pitch);
        ExpectValue(actual.faction, expected.faction);
    }
    {
        SCOPED_TRACE("PerceptionComponent");
        const auto& expected = original.get<C::PerceptionComponent>(source);
        const auto& actual = restored.get<C::PerceptionComponent>(destination);
        ExpectValue(actual.vision_cos_half_fov, expected.vision_cos_half_fov);
        ExpectValue(actual.vision_range, expected.vision_range);
        ExpectValue(actual.facing_x, expected.facing_x);
        ExpectValue(actual.facing_z, expected.facing_z);
        ExpectValue(actual.ear.range, expected.ear.range);
        ExpectValue(actual.ear.peak_pitch, expected.ear.peak_pitch);
        ExpectValue(actual.ear.bandwidth, expected.ear.bandwidth);
        ExpectValue(actual.ear.gain, expected.ear.gain);
        ExpectValue(actual.ear.threshold, expected.ear.threshold);
        ExpectValue(actual.faction, expected.faction);
    }
    {
        SCOPED_TRACE("AwarenessComponent");
        const auto& expected = original.get<C::AwarenessComponent>(source);
        const auto& actual = restored.get<C::AwarenessComponent>(destination);
        ExpectValue(actual.awareness.meter, expected.awareness.meter);
        ExpectValue(actual.awareness.state, expected.awareness.state);
        ExpectValue(actual.awareness.has_last_known, expected.awareness.has_last_known);
        ExpectValue(actual.awareness.last_known_x, expected.awareness.last_known_x);
        ExpectValue(actual.awareness.last_known_z, expected.awareness.last_known_z);
        ExpectValue(actual.awareness.time_since_signal, expected.awareness.time_since_signal);
        ExpectValue(actual.params.gain, expected.params.gain);
        ExpectValue(actual.params.decay, expected.params.decay);
        ExpectValue(actual.params.suspicious_at, expected.params.suspicious_at);
        ExpectValue(actual.params.alert_at, expected.params.alert_at);
        ExpectValue(actual.params.engaged_at, expected.params.engaged_at);
    }
    {
        SCOPED_TRACE("LocomotionIntentComponent");
        const auto& expected = original.get<C::LocomotionIntentComponent>(source);
        const auto& actual = restored.get<C::LocomotionIntentComponent>(destination);
        ExpectValue(actual.wish_xz.x, expected.wish_xz.x);
        ExpectValue(actual.wish_xz.y, expected.wish_xz.y);
        ExpectValue(actual.arrived, expected.arrived);
    }
    {
        SCOPED_TRACE("ScentSenseComponent");
        const auto& expected = original.get<C::ScentSenseComponent>(source);
        const auto& actual = restored.get<C::ScentSenseComponent>(destination);
        ExpectValue(actual.channel, expected.channel);
        ExpectValue(actual.sign, expected.sign);
        ExpectValue(actual.strength, expected.strength);
        ExpectValue(actual.floor, expected.floor);
        ExpectValue(actual.weber_k, expected.weber_k);
    }
    {
        SCOPED_TRACE("LocomotionPathComponent");
        const auto& expected = original.get<C::LocomotionPathComponent>(source);
        const auto& actual = restored.get<C::LocomotionPathComponent>(destination);
        ExpectValue(actual.index, expected.index);
    }
    {
        SCOPED_TRACE("InstinctAgentComponent");
        const auto& expected = original.get<C::InstinctAgentComponent>(source);
        const auto& actual = restored.get<C::InstinctAgentComponent>(destination);
        ExpectValue(actual.actor_id, expected.actor_id);
        ExpectValue(actual.archetype, expected.archetype);
        ExpectValue(actual.replan_interval_ticks, expected.replan_interval_ticks);
        ExpectValue(actual.last_planned_tick, expected.last_planned_tick);
        ExpectValue(actual.plans_executed, expected.plans_executed);
    }
    {
        SCOPED_TRACE("ForagerComponent");
        const auto& expected = original.get<C::ForagerComponent>(source);
        const auto& actual = restored.get<C::ForagerComponent>(destination);
        ExpectValue(actual.cell_x, expected.cell_x);
        ExpectValue(actual.cell_z, expected.cell_z);
        ExpectValue(actual.home_x, expected.home_x);
        ExpectValue(actual.home_z, expected.home_z);
        ExpectValue(actual.carrying_food, expected.carrying_food);
        ExpectValue(actual.deliveries, expected.deliveries);
    }
    {
        SCOPED_TRACE("FoodSourceComponent");
        const auto& expected = original.get<C::FoodSourceComponent>(source);
        const auto& actual = restored.get<C::FoodSourceComponent>(destination);
        ExpectValue(actual.cell_x, expected.cell_x);
        ExpectValue(actual.cell_z, expected.cell_z);
        ExpectValue(actual.amount, expected.amount);
    }
    {
        SCOPED_TRACE("FieldEmitterComponent");
        const auto& expected = original.get<C::FieldEmitterComponent>(source);
        const auto& actual = restored.get<C::FieldEmitterComponent>(destination);
        ExpectValue(actual.channel, expected.channel);
        ExpectValue(actual.rate_raw_per_tick, expected.rate_raw_per_tick);
        ExpectValue(actual.radius_cells, expected.radius_cells);
    }
    {
        SCOPED_TRACE("GrazeableComponent");
        const auto& expected = original.get<C::GrazeableComponent>(source);
        const auto& actual = restored.get<C::GrazeableComponent>(destination);
        ExpectValue(actual.biomass, expected.biomass);
        ExpectValue(actual.last_grazed_tick, expected.last_grazed_tick);
    }
    {
        SCOPED_TRACE("WaterHoleComponent");
        const auto& expected = original.get<C::WaterHoleComponent>(source);
        const auto& actual = restored.get<C::WaterHoleComponent>(destination);
        ExpectValue(actual.radius, expected.radius);
    }
    {
        SCOPED_TRACE("CombustibleComponent");
        const auto& expected = original.get<C::CombustibleComponent>(source);
        const auto& actual = restored.get<C::CombustibleComponent>(destination);
        ExpectValue(actual.fuel_milli, expected.fuel_milli);
        ExpectValue(actual.moisture_milli, expected.moisture_milli);
        ExpectValue(actual.burn_state, expected.burn_state);
        ExpectValue(actual.burn_ticks_remaining, expected.burn_ticks_remaining);
        ExpectValue(actual.ignition_radius, expected.ignition_radius);
    }
    {
        SCOPED_TRACE("IgnitionSourceComponent");
        const auto& expected = original.get<C::IgnitionSourceComponent>(source);
        const auto& actual = restored.get<C::IgnitionSourceComponent>(destination);
        ExpectValue(actual.radius, expected.radius);
        ExpectValue(actual.intensity, expected.intensity);
    }
    {
        SCOPED_TRACE("WaterSourceComponent");
        const auto& expected = original.get<C::WaterSourceComponent>(source);
        const auto& actual = restored.get<C::WaterSourceComponent>(destination);
        ExpectValue(actual.output, expected.output);
    }
    {
        SCOPED_TRACE("WaterFlowSourceComponent");
        const auto& expected = original.get<C::WaterFlowSourceComponent>(source);
        const auto& actual = restored.get<C::WaterFlowSourceComponent>(destination);
        ExpectValue(actual.flow_rate, expected.flow_rate);
    }
    EXPECT_TRUE(restored.all_of<C::InstinctAgent>(destination));
    EXPECT_TRUE(restored.all_of<C::PendingDeletion>(destination));
    EXPECT_EQ(restored.get<C::CreatureControllerComponent>(destination).physics_controlled,
              original.all_of<C::CreaturePhysicsComponent>(source));
    const auto reference = [&](entt::entity actual, entt::entity expected) {
        if (expected == entt::null) {
            EXPECT_EQ(actual, entt::entity{entt::null});
        } else {
            ASSERT_TRUE(restored.valid(actual));
            EXPECT_EQ(restored.get<C::CreatureRegionComponent>(actual).persistent_id,
                      original.get<C::CreatureRegionComponent>(expected).persistent_id);
        }
    };
    const auto& es = original.get<C::SensesComponent>(source);
    const auto& as = restored.get<C::SensesComponent>(destination);
    reference(as.nearest_food_source, es.nearest_food_source);
    reference(as.nearest_water_source, es.nearest_water_source);
    reference(as.nearest_threat, es.nearest_threat);
    const auto& ec = original.get<C::CreatureCoarseComponent>(source).needs;
    const auto& ac = restored.get<C::CreatureCoarseComponent>(destination).needs;
    ASSERT_EQ(ac.size(), ec.size());
    for (std::size_t i = 0; i < ec.size(); ++i) {
        ExpectValue(ac[i].name, ec[i].name);
        ExpectValue(ac[i].pressure_ppm, ec[i].pressure_ppm);
    }
    const auto& en = original.get<C::NeedsComponent>(source).needs;
    const auto& an = restored.get<C::NeedsComponent>(destination).needs;
    ASSERT_EQ(an.size(), en.size());
    for (std::size_t i = 0; i < en.size(); ++i) {
        ExpectValue(an[i].name, en[i].name);
        ExpectValue(an[i].pressure, en[i].pressure);
        ExpectValue(an[i].growth_per_tick, en[i].growth_per_tick);
    }
    const auto& ep = original.get<C::ActionPlanComponent>(source).plan;
    const auto& ap = restored.get<C::ActionPlanComponent>(destination).plan;
    ASSERT_EQ(ap.size(), ep.size());
    for (std::size_t i = 0; i < ep.size(); ++i) {
        ExpectValue(ap[i].name, ep[i].name);
        reference(ap[i].target, ep[i].target);
        ExpectValue(ap[i].duration, ep[i].duration);
        ExpectValue(ap[i].flee, ep[i].flee);
    }
    const auto& ew = original.get<C::LocomotionPathComponent>(source).waypoints;
    const auto& aw = restored.get<C::LocomotionPathComponent>(destination).waypoints;
    ASSERT_EQ(aw.size(), ew.size());
    for (std::size_t i = 0; i < ew.size(); ++i) {
        ExpectValue(aw[i].x, ew[i].x);
        ExpectValue(aw[i].y, ew[i].y);
    }
    const auto& eu = original.get<C::StimulusSubscriptionComponent>(source).subscriptions;
    const auto& au = restored.get<C::StimulusSubscriptionComponent>(destination).subscriptions;
    ASSERT_EQ(au.size(), eu.size());
    for (std::size_t i = 0; i < eu.size(); ++i) {
        ExpectValue(au[i].channel, eu[i].channel);
        ExpectValue(au[i].need, eu[i].need);
        ExpectValue(au[i].gain, eu[i].gain);
    }
    const auto& ei = original.get<C::InstinctAgentComponent>(source).current_plan;
    const auto& ai = restored.get<C::InstinctAgentComponent>(destination).current_plan;
    ExpectValue(ai.actor_id, ei.actor_id);
    ExpectValue(ai.archetype, ei.archetype);
    ExpectValue(ai.decision_contract, ei.decision_contract);
    ExpectValue(ai.selected_index, ei.selected_index);
    ExpectValue(ai.checksum, ei.checksum);
    ExpectValue(ai.passed, ei.passed);
    ASSERT_EQ(ai.candidates.size(), ei.candidates.size());
    for (std::size_t i = 0; i < ei.candidates.size(); ++i) {
        ExpectValue(ai.candidates[i].rank, ei.candidates[i].rank);
        ExpectValue(ai.candidates[i].id, ei.candidates[i].id);
        ExpectValue(ai.candidates[i].action, ei.candidates[i].action);
        ExpectValue(ai.candidates[i].target, ei.candidates[i].target);
        ExpectValue(ai.candidates[i].need, ei.candidates[i].need);
        ExpectValue(ai.candidates[i].score, ei.candidates[i].score);
        ExpectValue(ai.candidates[i].need_pressure, ei.candidates[i].need_pressure);
        ExpectValue(ai.candidates[i].reason, ei.candidates[i].reason);
    }
}

TEST(CreaturePersistence, EveryComponentAndEveryFieldRoundTripsBitExactly) {
    entt::registry original;
    const auto first = Creature(original, 1);
    const auto target = Creature(original, 2);
    EveryComponent(original, first, target);
    const auto snapshot = A::BuildCreatureSnapshot(original);
    const auto& components = snapshot.entities.front().components;
    ASSERT_EQ(components.at("Transform").at("data").size(), 10u) << "Transform";
    ASSERT_EQ(components.at("Region").at("data").size(), 4u) << "Region";
    ASSERT_EQ(components.at("Coarse").at("data").size(), 15u) << "Coarse";
    ASSERT_EQ(components.at("LocomotionState").at("data").size(), 4u) << "LocomotionState";
    ASSERT_EQ(components.at("Creature").at("data").size(), 10u) << "Creature";
    ASSERT_EQ(components.at("Genome").at("data").size(), 12u) << "Genome";
    ASSERT_EQ(components.at("Mortal").at("data").size(), 3u) << "Mortal";
    ASSERT_EQ(components.at("Decay").at("data").size(), 4u) << "Decay";
    ASSERT_EQ(components.at("Thirst").at("data").size(), 5u) << "Thirst";
    ASSERT_EQ(components.at("Circadian").at("data").size(), 2u) << "Circadian";
    ASSERT_EQ(components.at("Alarm").at("data").size(), 2u) << "Alarm";
    ASSERT_EQ(components.at("PackHunter").at("data").size(), 3u) << "PackHunter";
    ASSERT_EQ(components.at("Territory").at("data").size(), 4u) << "Territory";
    ASSERT_EQ(components.at("TerritoryBias").at("data").size(), 2u) << "TerritoryBias";
    ASSERT_EQ(components.at("Migratory").at("data").size(), 3u) << "Migratory";
    ASSERT_EQ(components.at("Scavenger").at("data").size(), 3u) << "Scavenger";
    ASSERT_EQ(components.at("Needs").at("data").size(), 1u) << "Needs";
    ASSERT_EQ(components.at("Senses").at("data").size(), 4u) << "Senses";
    ASSERT_EQ(components.at("Opportunity").at("data").size(), 9u) << "Opportunity";
    ASSERT_EQ(components.at("ActionPlan").at("data").size(), 3u) << "ActionPlan";
    ASSERT_EQ(components.at("InstinctAgent").at("data").size(), 0u) << "InstinctAgent";
    ASSERT_EQ(components.at("LocomotionProfile").at("data").size(), 8u) << "LocomotionProfile";
    ASSERT_EQ(components.at("Sensable").at("data").size(), 5u) << "Sensable";
    ASSERT_EQ(components.at("Perception").at("data").size(), 10u) << "Perception";
    ASSERT_EQ(components.at("Awareness").at("data").size(), 11u) << "Awareness";
    ASSERT_EQ(components.at("LocomotionIntent").at("data").size(), 3u) << "LocomotionIntent";
    ASSERT_EQ(components.at("ScentSense").at("data").size(), 5u) << "ScentSense";
    ASSERT_EQ(components.at("LocomotionPath").at("data").size(), 2u) << "LocomotionPath";
    ASSERT_EQ(components.at("StimulusSubscription").at("data").size(), 1u)
        << "StimulusSubscription";
    ASSERT_EQ(components.at("InstinctAgentState").at("data").size(), 6u) << "InstinctAgentState";
    ASSERT_EQ(components.at("Forager").at("data").size(), 6u) << "Forager";
    ASSERT_EQ(components.at("FoodSource").at("data").size(), 3u) << "FoodSource";
    ASSERT_EQ(components.at("FieldEmitter").at("data").size(), 3u) << "FieldEmitter";
    ASSERT_EQ(components.at("PendingDeletion").at("data").size(), 0u) << "PendingDeletion";
    ASSERT_EQ(components.at("Grazeable").at("data").size(), 2u) << "Grazeable";
    ASSERT_EQ(components.at("WaterHole").at("data").size(), 1u) << "WaterHole";
    ASSERT_EQ(components.at("Combustible").at("data").size(), 5u) << "Combustible";
    ASSERT_EQ(components.at("IgnitionSource").at("data").size(), 2u) << "IgnitionSource";
    ASSERT_EQ(components.at("WaterSource").at("data").size(), 1u) << "WaterSource";
    ASSERT_EQ(components.at("WaterFlowSource").at("data").size(), 1u) << "WaterFlowSource";
    ASSERT_EQ(components.at("Controller").at("data").size(), 1u);
    ASSERT_EQ(components.size(), 41u);
    EXPECT_FALSE(components.contains("CreaturePhysics"));
    EXPECT_EQ(components.at("Creature").at("data").at("wish_z_bits"), 0x80000000u);
    const auto bytes = A::SerializeCreatureSnapshot(snapshot);
    EXPECT_EQ(bytes.find("9876"), std::string::npos);
    A::CreatureSnapshot loaded;
    std::string error;
    ASSERT_TRUE(A::LoadCreatureSnapshot(bytes, loaded, error)) << error;
    ASSERT_TRUE(A::ValidateCreatureSnapshot(loaded, Ledger(2), {}, {}, error)) << error;
    entt::registry restored;
    // Change all raw entity handles so preserving references requires actual remapping.
    for (int i = 0; i < 7; ++i)
        ASSERT_TRUE(restored.valid(restored.create()));
    A::ApplyCreatureSnapshot(restored, loaded);
    entt::entity restored_first = entt::null;
    for (auto e : restored.view<C::CreatureRegionComponent>())
        if (restored.get<C::CreatureRegionComponent>(e).persistent_id == 1)
            restored_first = e;
    ASSERT_NE(restored_first, entt::entity{entt::null});
    ExpectEveryField(original, first, restored, restored_first);
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored)), bytes);
    EXPECT_FALSE(restored.view<C::CreaturePhysicsComponent>().begin() !=
                 restored.view<C::CreaturePhysicsComponent>().end());
    for (auto e : restored.view<C::CreatureRegionComponent, C::SensesComponent>()) {
        const auto ref = restored.get<C::SensesComponent>(e).nearest_food_source;
        EXPECT_NE(ref, target);
        EXPECT_EQ(restored.get<C::CreatureRegionComponent>(ref).persistent_id, 2u);
    }
    for (std::uint64_t tick = 11; tick <= 23; ++tick) {
        A::RunLifespanOnTick(original, tick);
        A::RunLifespanOnTick(restored, tick);
        A::RunDecompositionOnTick(original, tick);
        A::RunDecompositionOnTick(restored, tick);
        EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored)),
                  A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(original)));
    }
}

TEST(CreaturePersistence, PresenceIsIndependentAndMinimalCreatureDoesNotGainOptions) {
    entt::registry original;
    const auto e = Creature(original, 1);
    original.emplace<C::InstinctAgent>(e);
    original.emplace<C::ActionPlanComponent>(e); // present empty plan
    auto bytes = A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(original));
    entt::registry restored;
    A::CreatureSnapshot loaded;
    std::string error;
    ASSERT_TRUE(A::LoadCreatureSnapshot(bytes, loaded, error));
    A::ApplyCreatureSnapshot(restored, loaded);
    const auto out = *restored.view<C::CreatureComponent>().begin();
    EXPECT_TRUE((restored.all_of<C::InstinctAgent, C::ActionPlanComponent>(out)));
    EXPECT_FALSE(
        (restored.any_of<C::NeedsComponent, C::CreatureGenomeComponent, C::SensesComponent>(out)));
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored)), bytes);
    original.remove<C::InstinctAgent>(e);
    EXPECT_NE(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(original)), bytes);
}

TEST(CreaturePersistence, DurablePlantReferencesResolveAndUnsupportedOrWrongKindRefuses) {
    entt::registry r;
    const auto e = Creature(r, 1);
    Plant(r, 2);
    const auto plant = *r.view<C::PlantTag>().begin();
    r.emplace<C::SensesComponent>(e).nearest_food_source = plant;
    auto snapshot = A::BuildCreatureSnapshot(r);
    const auto plants = F::BuildPlantEntitySnapshot(r, true);
    std::string error;
    EXPECT_TRUE(A::ValidateCreatureSnapshot(snapshot, Ledger(2), {}, plants, error)) << error;
    entt::registry restored;
    for (int i = 0; i < 7; ++i)
        ASSERT_TRUE(restored.valid(restored.create()));
    F::ApplyPlantEntitySnapshot(restored, plants);
    A::ApplyCreatureSnapshot(restored, snapshot);
    const auto actor = *restored.view<C::SensesComponent>().begin();
    const auto remapped = restored.get<C::SensesComponent>(actor).nearest_food_source;
    EXPECT_NE(remapped, plant);
    EXPECT_TRUE(restored.all_of<C::PlantTag>(remapped));
    EXPECT_EQ(restored.get<C::PlantRegionComponent>(remapped).persistent_id,
              r.get<C::PlantRegionComponent>(plant).persistent_id);
    EXPECT_FALSE(A::ValidateCreatureSnapshot(snapshot, Ledger(2), {}, {}, error));
    snapshot.entities[0].components["Senses"]["data"]["nearest_food_source"]["kind"] = "creature";
    A::CreatureSnapshot loaded;
    EXPECT_FALSE(A::LoadCreatureSnapshot(A::SerializeCreatureSnapshot(snapshot), loaded, error));
    snapshot.entities[0].components["Senses"]["data"]["nearest_food_source"] = {{"kind", "ground"},
                                                                                {"id", 2}};
    EXPECT_FALSE(A::LoadCreatureSnapshot(A::SerializeCreatureSnapshot(snapshot), loaded, error));
    r.get<C::SensesComponent>(e).nearest_food_source = r.create();
    EXPECT_THROW(A::BuildCreatureSnapshot(r), std::invalid_argument);
    const auto stale = r.get<C::SensesComponent>(e).nearest_food_source;
    r.destroy(stale);
    EXPECT_THROW(A::BuildCreatureSnapshot(r), std::invalid_argument);
}

TEST(CreaturePersistence, ForwardAndCyclicReferencesUseTwoPassRestore) {
    entt::registry r;
    const auto a = Creature(r, 1), b = Creature(r, 2);
    r.emplace<C::SensesComponent>(a).nearest_threat = b;
    r.emplace<C::SensesComponent>(b).nearest_threat = a;
    const auto bytes = A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(r));
    A::CreatureSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(A::LoadCreatureSnapshot(bytes, snapshot, error));
    entt::registry restored;
    A::ApplyCreatureSnapshot(restored, snapshot);
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored)), bytes);
    snapshot.entities[0].components["Senses"]["data"]["nearest_threat"]["id"] = 99;
    const auto before = A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(r));
    EXPECT_THROW(A::ApplyCreatureSnapshot(r, snapshot), std::invalid_argument);
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(r)), before);
    A::CreatureSnapshot untouched = A::BuildCreatureSnapshot(r);
    EXPECT_FALSE(A::LoadCreatureSnapshot(A::SerializeCreatureSnapshot(snapshot), untouched, error));
    EXPECT_EQ(A::SerializeCreatureSnapshot(untouched), before);
    entt::registry fresh;
    EXPECT_THROW(A::ApplyCreatureSnapshot(fresh, snapshot), std::invalid_argument);
    EXPECT_TRUE(A::BuildCreatureSnapshot(fresh).entities.empty());
}

TEST(CreaturePersistence, SharedAllocatorRefusesGroundAndPlantCollisionsAndUnallocatedIds) {
    entt::registry r;
    Creature(r, 1);
    auto snapshot = A::BuildCreatureSnapshot(r);
    GroundObjectStore ground;
    Luminumbra::world::GroundObject object;
    object.id = 1;
    object.type = 1;
    ASSERT_TRUE(ground.insert(object));
    std::string error;
    EXPECT_FALSE(A::ValidateCreatureSnapshot(snapshot, Ledger(1), ground, {}, error));
    entt::registry p;
    Plant(p, 1);
    EXPECT_FALSE(A::ValidateCreatureSnapshot(
        snapshot, Ledger(1), {}, F::BuildPlantEntitySnapshot(p, true), error));
    EXPECT_FALSE(A::ValidateCreatureSnapshot(snapshot, Ledger(0), {}, {}, error));
    snapshot.entities[0].components["Region"]["data"]["region_stamp"] = 11;
    EXPECT_FALSE(A::ValidateCreatureSnapshot(snapshot, Ledger(1), {}, {}, error));
}

TEST(CreaturePersistence, UnknownVersionsKeysTypesBoundsAndNonfiniteBitsRefuseAtomically) {
    entt::registry r;
    const auto e = Creature(r, 1);
    r.emplace<C::NeedsComponent>(e).needs = {{"hunger", 0.25f, 0.125f}};
    const auto snapshot = A::BuildCreatureSnapshot(r);
    const auto baseline = A::SerializeCreatureSnapshot(snapshot);
    std::vector<nlohmann::json> invalid;
    const auto parsed = nlohmann::json::parse(baseline);
    const auto add = [&](const auto& change) {
        auto j = parsed;
        change(j);
        invalid.push_back(std::move(j));
    };
    add([](auto& j) { j["schema"] = "luminumbra.creature_entities.v2"; });
    add([](auto& j) { j["future"] = 1; });
    add([](auto& j) { j["entities"][0]["id"] = 0; });
    add([](auto& j) {
        j["entities"][0]["components"]["Future"] = {{"version", 1},
                                                    {"data", nlohmann::json::object()}};
    });
    add([](auto& j) { j["entities"][0]["components"]["Creature"]["version"] = 2; });
    add([](auto& j) { j["entities"][0]["components"].erase("Coarse"); });
    add([](auto& j) {
        j["entities"][0]["components"]["Controller"]["data"]["physics_controlled"] = 1;
    });
    add([](auto& j) { j["entities"][0]["components"]["Creature"]["data"]["species_id"] = 65537; });
    add([](auto& j) { j["entities"][0]["components"]["Creature"]["data"]["hunger_bits"] = -1; });
    add([](auto& j) { j["entities"][0]["components"]["Creature"]["data"]["hunger_bits"] = 1.0; });
    add([](auto& j) {
        j["entities"][0]["components"]["Creature"]["data"]["hunger_bits"] = 0x7f800000u;
    });
    add([](auto& j) {
        j["entities"][0]["components"]["Creature"]["data"]["hunger_bits"] = 0x7fc00000u;
    });
    add([](auto& j) {
        j["entities"][0]["components"]["Creature"]["data"]["hunger_bits"] =
            std::bit_cast<std::uint32_t>(1.25f);
    });
    add([](auto& j) { j["entities"][0]["components"]["Creature"]["data"]["eaten"] = 1; });
    add([](auto& j) { j["entities"][0]["components"]["Creature"]["data"]["future"] = 1; });
    add([](auto& j) { j["entities"][0]["components"]["Coarse"]["data"]["tier"] = 2; });
    add([](auto& j) {
        j["entities"][0]["components"]["Needs"]["data"]["needs"][0]["name"] =
            std::string(A::kMaxCreatureString + 1, 'a');
    });
    add([](auto& j) {
        auto& v = j["entities"][0]["components"]["Needs"]["data"]["needs"];
        const auto item = v[0];
        for (std::size_t i = 0; i < A::kMaxCreatureVector; ++i)
            v.push_back(item);
    });
    add([](auto& j) {
        const auto item = j["entities"][0];
        j["entities"].push_back(item);
    });
    for (const auto& j : invalid) {
        SCOPED_TRACE(j.dump().substr(0, 180));
        A::CreatureSnapshot out = snapshot;
        std::string error;
        EXPECT_FALSE(A::LoadCreatureSnapshot(j.dump(), out, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(A::SerializeCreatureSnapshot(out), baseline);
    }
    std::string duplicate = baseline;
    duplicate.insert(1, "\"schema\":\"luminumbra.creature_entities.v1\",");
    std::string error;
    A::CreatureSnapshot out;
    EXPECT_FALSE(A::LoadCreatureSnapshot(duplicate, out, error));
    EXPECT_FALSE(A::LoadCreatureSnapshot(std::string(A::kMaxCreatureBytes + 1, ' '), out, error));
    EXPECT_FALSE(
        A::LoadCreatureSnapshot(std::string(40, '[') + "0" + std::string(40, ']'), out, error));
}

TEST(CreaturePersistence, CompleteProjectionDetectsFieldsMissingFromLegacyEcologyHash) {
    entt::registry r;
    const auto e = Creature(r, 1);
    r.emplace<C::CreatureGenomeComponent>(e);
    const auto baseline = A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(r));
    r.get<C::CreatureGenomeComponent>(e).reproduce_cooldown = 7;
    EXPECT_NE(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(r)), baseline);
    r.get<C::CreatureGenomeComponent>(e).reproduce_cooldown = 0;
    r.get<C::TransformComponent>(e).rotation.x = 0.25f;
    EXPECT_NE(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(r)), baseline);
    r.get<C::TransformComponent>(e).rotation.x = 0;
    r.get<C::CreatureComponent>(e).last_action = 6;
    EXPECT_NE(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(r)), baseline);
    EXPECT_FALSE(A::CreatureRegionProjection(r, {0, 0}).empty());
    EXPECT_TRUE(A::CreatureRegionProjection(r, {1, 0}).empty());
}
TEST(CreaturePersistence, EmptyMarkersRequireEmptyObjectsAndPreservePresence) {
    for (const auto* marker : {"InstinctAgent", "PendingDeletion"}) {
        entt::registry source;
        const auto e = Creature(source, 1);
        source.emplace<C::InstinctAgent>(e);
        source.emplace<C::PendingDeletion>(e);
        auto snapshot = A::BuildCreatureSnapshot(source);
        entt::registry restored;
        A::ApplyCreatureSnapshot(restored, snapshot);
        const auto target = *restored.view<C::CreatureComponent>().begin();
        EXPECT_TRUE(restored.all_of<C::InstinctAgent>(target));
        EXPECT_TRUE(restored.all_of<C::PendingDeletion>(target));
        for (const auto& invalid : {nlohmann::json::array(), nlohmann::json{{"extra", 1}}}) {
            snapshot.entities[0].components[marker]["data"] = invalid;
            std::string error;
            A::CreatureSnapshot out;
            EXPECT_FALSE(
                A::LoadCreatureSnapshot(A::SerializeCreatureSnapshot(snapshot), out, error));
            entt::registry untouched;
            EXPECT_THROW(A::ApplyCreatureSnapshot(untouched, snapshot), std::invalid_argument);
        }
    }
}

TEST(CreaturePersistence, ControllerParticipationIsSeparateFromRuntimeHandleAndVelocityPresence) {
    entt::registry source;
    const auto e = Creature(source, 1);
    source.emplace<C::CreatureLocomotionStateComponent>(e).velocity_y = -1.25f;
    const auto direct = A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(source));
    source.emplace<C::CreaturePhysicsComponent>(e).avatar_index = 1234;
    const auto controlled = A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(source));
    EXPECT_NE(controlled, direct);
    for (const bool physics : {false, true}) {
        A::CreatureSnapshot loaded;
        std::string error;
        ASSERT_TRUE(A::LoadCreatureSnapshot(physics ? controlled : direct, loaded, error)) << error;
        entt::registry restored;
        A::ApplyCreatureSnapshot(restored, loaded);
        const auto target = *restored.view<C::CreatureComponent>().begin();
        EXPECT_EQ(restored.get<C::CreatureControllerComponent>(target).physics_controlled, physics);
        EXPECT_FALSE(restored.all_of<C::CreaturePhysicsComponent>(target));
        ExpectValue(restored.get<C::CreatureLocomotionStateComponent>(target).velocity_y, -1.25f);
        EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored)),
                  physics ? controlled : direct);
    }
    auto missing = A::BuildCreatureSnapshot(source);
    missing.entities[0].components.erase("LocomotionState");
    A::CreatureSnapshot out;
    std::string error;
    EXPECT_FALSE(A::LoadCreatureSnapshot(A::SerializeCreatureSnapshot(missing), out, error));
}

TEST(CreaturePersistence, LegacyControllerAbsenceLoadsAndNextSaveStampsNonparticipation) {
    for (const bool velocity : {false, true}) {
        for (const bool missing_block : {false, true}) {
            SCOPED_TRACE(velocity);
            SCOPED_TRACE(missing_block);
            entt::registry original;
            const auto e = Creature(original, 1);
            if (velocity)
                original.emplace<C::CreatureLocomotionStateComponent>(e).velocity_y = -0.375f;
            auto legacy = A::BuildCreatureSnapshot(original);
            if (missing_block)
                legacy.entities[0].components.erase("Controller");
            else
                legacy.entities[0].components["Controller"]["data"].erase("physics_controlled");
            const nlohmann::json document{
                {"schema", A::kCreatureSchema},
                {"entities", {{{"id", 1}, {"components", legacy.entities[0].components}}}}};
            A::CreatureSnapshot loaded;
            std::string error;
            ASSERT_TRUE(A::LoadCreatureSnapshot(document.dump(), loaded, error)) << error;
            ASSERT_TRUE(A::ValidateCreatureSnapshot(loaded, Ledger(1), {}, {}, error)) << error;
            // Direct callers must receive the same adoption and canonical comparison behavior.
            ASSERT_TRUE(A::ValidateCreatureSnapshot(legacy, Ledger(1), {}, {}, error)) << error;
            entt::registry restored;
            A::ApplyCreatureSnapshot(restored, legacy);
            const auto out = *restored.view<C::CreatureComponent>().begin();
            ASSERT_TRUE(restored.all_of<C::CreatureControllerComponent>(out));
            EXPECT_FALSE(restored.get<C::CreatureControllerComponent>(out).physics_controlled);
            EXPECT_FALSE(restored.all_of<C::CreaturePhysicsComponent>(out));
            EXPECT_EQ(restored.all_of<C::CreatureLocomotionStateComponent>(out), velocity);
            if (velocity)
                EXPECT_EQ(restored.get<C::CreatureLocomotionStateComponent>(out).velocity_y,
                          -0.375f);
            const auto saved = A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored));
            EXPECT_EQ(nlohmann::json::parse(saved)["entities"][0]["components"]["Controller"],
                      (nlohmann::json{{"version", 1}, {"data", {{"physics_controlled", false}}}}));
            EXPECT_EQ(A::SerializeCreatureSnapshot(loaded), saved);
            EXPECT_EQ(A::SerializeCreatureSnapshot(legacy), saved);
        }
    }
}

TEST(CreaturePersistence, EmitterRadiusRefusesExtremeValuesAndAcceptsLastNonzeroRing) {
    entt::registry source;
    const auto e = Creature(source, 1);
    source.emplace<C::FieldEmitterComponent>(e).rate_raw_per_tick = 0xffffffffu;
    for (const auto radius : {A::kMaxCreatureEmitterRadius,
                              A::kMaxCreatureEmitterRadius + 1,
                              std::numeric_limits<int>::max()}) {
        SCOPED_TRACE(radius);
        source.get<C::FieldEmitterComponent>(e).radius_cells = A::kMaxCreatureEmitterRadius;
        auto snapshot = A::BuildCreatureSnapshot(source);
        snapshot.entities[0].components["FieldEmitter"]["data"]["radius_cells"] = radius;
        A::CreatureSnapshot loaded;
        std::string error;
        const bool accepted = radius == A::kMaxCreatureEmitterRadius;
        EXPECT_EQ(A::LoadCreatureSnapshot(A::SerializeCreatureSnapshot(snapshot), loaded, error),
                  accepted)
            << error;
        if (accepted) {
            entt::registry restored;
            A::ApplyCreatureSnapshot(restored, loaded);
            const auto target = *restored.view<C::FieldEmitterComponent>().begin();
            EXPECT_EQ(restored.get<C::FieldEmitterComponent>(target).radius_cells, radius);
        }
        source.get<C::FieldEmitterComponent>(e).radius_cells = radius;
        if (!accepted)
            EXPECT_THROW(A::BuildCreatureSnapshot(source), std::invalid_argument);
    }
}

class RestoreSignals {
    int calls = 0;

public:
    int CallCount() const {
        return calls;
    }
    void Observe(entt::registry&, entt::entity) {
        ++calls;
    }
};

TEST(CreaturePersistence, RefusalPreservesEntityAllocationGenerationsAndEmitsNoSignals) {
    entt::registry source;
    const auto first = Creature(source, 1), second = Creature(source, 2);
    source.emplace<C::SensesComponent>(first).nearest_threat = second;
    const auto valid = A::BuildCreatureSnapshot(source);
    for (const bool dangling : {false, true}) {
        entt::registry destination, control;
        for (auto* r : {&destination, &control}) {
            ASSERT_TRUE(r->valid(r->create()));
            const auto recycled = r->create();
            r->destroy(recycled);
        }
        RestoreSignals signals;
        destination.on_construct<C::TransformComponent>().connect<&RestoreSignals::Observe>(
            signals);
        destination.on_destroy<C::TransformComponent>().connect<&RestoreSignals::Observe>(signals);
        destination.on_construct<C::CreatureComponent>().connect<&RestoreSignals::Observe>(signals);
        destination.on_destroy<C::CreatureComponent>().connect<&RestoreSignals::Observe>(signals);
        auto invalid = valid;
        if (dangling)
            invalid.entities[0].components["Senses"]["data"]["nearest_threat"]["id"] = 99;
        else
            invalid.entities[1].components["Transform"]["data"]["scale_z_bits"] = 0x7f800000u;
        EXPECT_THROW(A::ApplyCreatureSnapshot(destination, invalid), std::invalid_argument);
        EXPECT_EQ(signals.CallCount(), 0);
        for (int i = 0; i < 8; ++i)
            EXPECT_EQ(destination.create(), control.create());
        destination.on_construct<C::TransformComponent>().disconnect<&RestoreSignals::Observe>(
            signals);
        destination.on_destroy<C::TransformComponent>().disconnect<&RestoreSignals::Observe>(
            signals);
        destination.on_construct<C::CreatureComponent>().disconnect<&RestoreSignals::Observe>(
            signals);
        destination.on_destroy<C::CreatureComponent>().disconnect<&RestoreSignals::Observe>(
            signals);
    }
}

TEST(CreaturePersistence, ExistingIdentitiesRequireMatchingKindsAndValidIdDomains) {
    for (const bool plant : {false, true}) {
        for (const auto id : {std::uint64_t{0}, std::uint64_t{2}, std::uint64_t{1} << 63}) {
            for (const bool correct_kind : {false, true}) {
                entt::registry source, destination;
                Creature(source, 1);
                auto snapshot = A::BuildCreatureSnapshot(source);
                snapshot.entities[0].components["Senses"] = {
                    {"version", 1},
                    {"data",
                     {{"nearest_food_source", {{"kind", plant ? "plant" : "creature"}, {"id", id}}},
                      {"nearest_water_source", {{"kind", "null"}, {"id", 0}}},
                      {"nearest_threat", {{"kind", "null"}, {"id", 0}}},
                      {"threat_distance_bits", 0}}}};
                const auto target = destination.create();
                if (plant) {
                    destination.emplace<C::PlantRegionComponent>(target).persistent_id = id;
                    if (correct_kind)
                        destination.emplace<C::PlantTag>(target);
                } else {
                    destination.emplace<C::CreatureRegionComponent>(target).persistent_id = id;
                    if (correct_kind)
                        destination.emplace<C::CreatureComponent>(target).species_id = 1;
                }
                if (correct_kind && id == 2) {
                    A::ApplyCreatureSnapshot(destination, snapshot);
                    const auto actor = *destination.view<C::SensesComponent>().begin();
                    EXPECT_EQ(destination.get<C::SensesComponent>(actor).nearest_food_source,
                              target);
                } else {
                    EXPECT_THROW(A::ApplyCreatureSnapshot(destination, snapshot),
                                 std::invalid_argument);
                    EXPECT_THROW(A::BuildCreatureSnapshot(destination), std::invalid_argument);
                }
            }
        }
    }
}

TEST(CreaturePersistence, ParserBudgetsRefuseLargeCollectionsBeforeMaterializingThem) {
    const auto refuse = [](const std::string& bytes, const char* budget) {
        ASSERT_LT(bytes.size(), A::kMaxCreatureBytes);
        A::CreatureSnapshot out;
        std::string error;
        EXPECT_FALSE(A::LoadCreatureSnapshot(bytes, out, error));
        EXPECT_NE(error.find(budget), std::string::npos) << error;
        EXPECT_TRUE(out.entities.empty());
    };
    std::string many_values = "{\"entities\":[";
    for (int i = 0; i < 1000000; ++i)
        many_values += "0,";
    many_values.back() = ']';
    many_values += '}';
    refuse(many_values, "array count limit");
    refuse("{\"entities\":[{\"vector\":" +
               many_values.substr(many_values.find('['),
                                  many_values.size() - many_values.find('[') - 1) +
               "}]}",
           "array count limit");
    std::string many_keys = "{";
    for (int i = 0; i < 100000; ++i)
        many_keys += "\"k" + std::to_string(i) + "\":0,";
    many_keys.back() = '}';
    refuse(many_keys, "object key limit");
    refuse(std::string(33, '[') + "0" + std::string(33, ']'), "nesting limit");
}

TEST(CreaturePersistence, MaximumWaypointVectorsKeepTheLegacyNodeBudget) {
    entt::registry registry;
    const auto e = Creature(registry, 1);
    registry.emplace<C::LocomotionPathComponent>(e).waypoints.resize(A::kMaxCreatureVector,
                                                                     Luminumbra::Vec2(0.0f));
    const auto prototype = A::BuildCreatureSnapshot(registry).entities.front();
    const auto nodes = [](const nlohmann::json& value) {
        std::size_t result = 0;
        std::vector<const nlohmann::json*> pending{&value};
        while (!pending.empty()) {
            const auto* current = pending.back();
            pending.pop_back();
            result += 1 + (current->is_object() ? current->size() : 0);
            if (current->is_structured())
                for (const auto& child : *current)
                    pending.push_back(&child);
        }
        return result;
    };
    // Each stamped record has 1,405 nodes including its wrapper; the document has five.
    ASSERT_EQ(nodes(prototype.components) + 4, 1405u);
    ASSERT_EQ(A::kMaxCreatureNodes, 4194304u);
    ASSERT_EQ(A::kCreatureControllerStampNodes, 8u);
    for (const bool stamped : {false, true}) {
        auto input_prototype = prototype;
        if (!stamped)
            input_prototype.components.erase("Controller");
        for (const std::size_t count : {3000u, 3003u, 3200u}) {
            SCOPED_TRACE(stamped);
            SCOPED_TRACE(count);
            A::CreatureSnapshot snapshot;
            snapshot.entities.reserve(count);
            std::string bytes = "{\"entities\":[";
            for (std::size_t i = 1; i <= count; ++i) {
                auto record = input_prototype;
                record.id = i;
                record.components["Region"]["data"]["persistent_id"] = i;
                if (i != 1)
                    bytes += ',';
                bytes += nlohmann::json{{"id", i}, {"components", record.components}}.dump();
                snapshot.entities.push_back(std::move(record));
            }
            bytes += "],\"schema\":\"";
            bytes += A::kCreatureSchema;
            bytes += "\"}\n";
            EXPECT_LT(bytes.size(), 23u * 1024u * 1024u);
            ASSERT_LT(bytes.size(), A::kMaxCreatureBytes);
            const auto total_nodes = 5 + count * (stamped ? 1405 : 1397);
            const bool fits = count == 3000;
            EXPECT_EQ(total_nodes <= A::CreatureNodeLimit(count, stamped ? count * 8 : 0), fits);
            if (fits) {
                EXPECT_NO_THROW(A::SerializeCreatureSnapshot(snapshot));
            } else {
                EXPECT_THROW(A::SerializeCreatureSnapshot(snapshot), std::invalid_argument);
            }
            snapshot.entities.clear();
            snapshot.entities.shrink_to_fit();
            A::CreatureSnapshot loaded;
            std::string error;
            if (fits) {
                ASSERT_TRUE(A::LoadCreatureSnapshot(bytes, loaded, error)) << error;
                ASSERT_EQ(loaded.entities.size(), count);
                for (std::size_t i = 0; i < loaded.entities.size(); ++i) {
                    const auto& record = loaded.entities[i];
                    EXPECT_EQ(record.id, i + 1);
                    EXPECT_EQ(record.components["LocomotionPath"]["data"]["waypoints"].size(),
                              A::kMaxCreatureVector);
                    EXPECT_EQ(record.components["Controller"]["data"]["physics_controlled"], false);
                }
                const auto saved = A::SerializeCreatureSnapshot(loaded);
                EXPECT_EQ(saved.size(), bytes.size() + (stamped ? 0 : count * 63));
                if (stamped)
                    EXPECT_EQ(saved, bytes);
            } else {
                EXPECT_FALSE(A::LoadCreatureSnapshot(bytes, loaded, error));
                EXPECT_NE(error.find("creature node limit"), std::string::npos) << error;
                EXPECT_TRUE(loaded.entities.empty());
            }
        }
    }
}

TEST(CreaturePersistence, ControllerStampAllowanceIsBoundedByActualRecordCount) {
    entt::registry registry;
    Creature(registry, 1);
    const auto snapshot = A::BuildCreatureSnapshot(registry);
    const auto canonical = A::SerializeCreatureSnapshot(snapshot);
    auto bytes = canonical;
    // Whitespace counts toward the byte budget. One record earns only one stamp's allowance.
    bytes.resize(A::CreatureByteLimit(1, A::kCreatureControllerStampBytes), ' ');
    A::CreatureSnapshot loaded;
    std::string error;
    ASSERT_TRUE(A::LoadCreatureSnapshot(bytes, loaded, error)) << error;
    EXPECT_EQ(A::SerializeCreatureSnapshot(loaded), canonical);
    bytes.push_back(' ');
    ASSERT_LT(bytes.size(), A::kMaxStampedCreatureBytes);
    EXPECT_FALSE(A::LoadCreatureSnapshot(bytes, loaded, error));
    EXPECT_NE(error.find("creature byte limit"), std::string::npos) << error;
    EXPECT_EQ(A::SerializeCreatureSnapshot(loaded), canonical);
    // Refuse input above the absolute ceiling before attempting even the bounded SAX parse.
    bytes.assign(A::kMaxStampedCreatureBytes + 1, ' ');
    EXPECT_FALSE(A::LoadCreatureSnapshot(bytes, loaded, error));
    EXPECT_NE(error.find("creature byte limit"), std::string::npos) << error;
    EXPECT_EQ(A::SerializeCreatureSnapshot(loaded), canonical);
}

TEST(CreaturePersistence, LegacyWithoutControllerKeepsExactByteBoundary) {
    entt::registry registry;
    Creature(registry, 1);
    auto record = A::BuildCreatureSnapshot(registry).entities.front();
    record.components.erase("Controller");
    const nlohmann::json document{
        {"schema", A::kCreatureSchema},
        {"entities", {{{"id", record.id}, {"components", record.components}}}}};
    auto bytes = document.dump() + "\n";
    bytes.resize(A::kMaxCreatureBytes, ' ');
    A::CreatureSnapshot loaded;
    std::string error;
    ASSERT_TRUE(A::LoadCreatureSnapshot(bytes, loaded, error)) << error;
    const auto canonical = A::SerializeCreatureSnapshot(loaded);
    bytes.push_back(' ');
    ASSERT_EQ(bytes.size(), 67108865u);
    EXPECT_FALSE(A::LoadCreatureSnapshot(bytes, loaded, error));
    EXPECT_NE(error.find("creature byte limit"), std::string::npos) << error;
    EXPECT_EQ(A::SerializeCreatureSnapshot(loaded), canonical);
}

TEST(CreaturePersistence, PresentControllerEarnsOnlyItsCompactBytes) {
    for (const bool missing_field : {false, true}) {
        entt::registry registry;
        const auto e = Creature(registry, 1);
        registry.emplace<C::CreatureLocomotionStateComponent>(e);
        registry.emplace<C::CreatureControllerComponent>(e).physics_controlled = true;
        auto record = A::BuildCreatureSnapshot(registry).entities.front();
        if (missing_field)
            record.components["Controller"]["data"].erase("physics_controlled");
        const auto controller_bytes =
            sizeof(R"(,"Controller":)") - 1 + record.components["Controller"].dump().size();
        ASSERT_LT(controller_bytes, A::kCreatureControllerStampBytes);
        const nlohmann::json document{
            {"schema", A::kCreatureSchema},
            {"entities", {{{"id", record.id}, {"components", record.components}}}}};
        auto bytes = document.dump() + "\n";
        bytes.resize(A::CreatureByteLimit(1, controller_bytes), ' ');
        A::CreatureSnapshot loaded;
        std::string error;
        ASSERT_TRUE(A::LoadCreatureSnapshot(bytes, loaded, error)) << error;
        const auto canonical = A::SerializeCreatureSnapshot(loaded);
        bytes.push_back(' ');
        EXPECT_FALSE(A::LoadCreatureSnapshot(bytes, loaded, error));
        EXPECT_NE(error.find("creature byte limit"), std::string::npos) << error;
        EXPECT_EQ(A::SerializeCreatureSnapshot(loaded), canonical);
    }
}

TEST(CreaturePersistence, NearLimitLegacyNeedsNeverLoadThenFailStampedSave) {
    entt::registry registry;
    const auto e = Creature(registry, 1);
    registry.emplace<C::NeedsComponent>(e).needs = {{std::string(1024, 'a'), 0.0f, 0.0f},
                                                    {std::string(1024, 'b'), 0.0f, 0.0f},
                                                    {std::string(940, 'c'), 0.0f, 0.0f}};
    auto prototype = A::BuildCreatureSnapshot(registry).entities.front();
    prototype.components.erase("Controller");
    for (const std::size_t name_size : {940u, 955u}) {
        SCOPED_TRACE(name_size);
        prototype.components["Needs"]["data"]["needs"][2]["name"] = std::string(name_size, 'c');
        std::string bytes = "{\"entities\":[";
        bytes.reserve(A::kMaxStampedCreatureBytes);
        for (std::size_t i = 1; i <= A::kMaxCreatures; ++i) {
            if (i != 1)
                bytes += ',';
            prototype.components["Region"]["data"]["persistent_id"] = i;
            bytes += nlohmann::json{{"id", i}, {"components", prototype.components}}.dump();
        }
        bytes += "],\"schema\":\"";
        bytes += A::kCreatureSchema;
        bytes += "\"}\n";
        ASSERT_EQ(bytes.size(), 66873718u + A::kMaxCreatures * (name_size - 940));
        A::CreatureSnapshot loaded;
        std::string error;
        if (name_size == 940) {
            ASSERT_TRUE(A::LoadCreatureSnapshot(bytes, loaded, error)) << error;
            std::string saved;
            ASSERT_NO_THROW(saved = A::SerializeCreatureSnapshot(loaded));
            EXPECT_EQ(saved.size(), bytes.size() + A::kMaxCreatures * 63);
            EXPECT_LE(saved.size(), A::CreatureByteLimit(A::kMaxCreatures, A::kMaxCreatures * 63));
        } else {
            ASSERT_EQ(bytes.size(), 67119478u);
            EXPECT_FALSE(A::LoadCreatureSnapshot(bytes, loaded, error));
            EXPECT_NE(error.find("creature byte limit"), std::string::npos) << error;
            EXPECT_TRUE(loaded.entities.empty());
        }
    }
}

TEST(CreaturePersistence, MinimalRecordHasPinnedIntegerBitEncoding) {
    entt::registry registry;
    Creature(registry, 1);
    const auto expected = nlohmann::json::parse(R"({
      "schema":"luminumbra.creature_entities.v1",
      "entities":[{"id":1,"components":{
        "Controller":{"version":1,"data":{"physics_controlled":false}},
        "Coarse":{"version":1,"data":{"tier":0,"transition":0,"x_mm":0,"y_mm":0,"z_mm":0,
          "hunger_ppm":0,"thirst_ppm":0,"needs":[],"age_cadences":0,"reproduce_cadences":0,
          "reproduce_cooldown_cadences":0,"courting_cadences":0,"travel_pending":false,
          "destination_x":0,"destination_z":0}},
        "Creature":{"version":1,"data":{"is_predator":false,"species_id":1,"hunger_bits":0,
          "stamina_bits":1065353216,"energy_bits":1065353216,"move_speed_bits":1077936128,
          "last_action":0,"wish_x_bits":0,"wish_z_bits":0,"eaten":false}},
        "Region":{"version":1,"data":{"persistent_id":1,"owner_x":0,"owner_z":0,"region_stamp":0}},
        "Transform":{"version":1,"data":{"position_x_bits":0,"position_y_bits":0,"position_z_bits":0,
          "rotation_w_bits":1065353216,"rotation_x_bits":0,"rotation_y_bits":0,"rotation_z_bits":0,
          "scale_x_bits":1065353216,"scale_y_bits":1065353216,"scale_z_bits":1065353216}}
      }}]
    })");
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(registry)),
              expected.dump() + "\n");
}

} // namespace
