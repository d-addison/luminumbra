#pragma once

// Candidate canonical simulation projections for issue #629. Shipping remains on
// the legacy projections until the common activation and authority change.
#include "components/AlarmComponents.h"
#include "components/CircadianComponents.h"
#include "components/CombustionComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/CropLifecycleComponents.h"
#include "components/DecayComponents.h"
#include "components/DiseaseComponents.h"
#include "components/FieldEmitterComponents.h"
#include "components/ForagingComponents.h"
#include "components/GrazeableComponent.h"
#include "components/InstinctComponents.h"
#include "components/IrrigationComponents.h"
#include "components/MigratoryComponents.h"
#include "components/MortalComponents.h"
#include "components/PackHunterComponents.h"
#include "components/PlantComponents.h"
#include "components/PollinationComponents.h"
#include "components/ScavengerComponent.h"
#include "components/SimulationIdentityComponent.h"
#include "components/SoilComponents.h"
#include "components/TerritoryComponents.h"
#include "components/ThirstComponents.h"
#include "core/WaterComponents.h"
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::simulation {

[[nodiscard]] constexpr bool UseCompleteSimulationProjections() noexcept {
    return false;
}

namespace projection {
namespace C = ::Luminumbra::Components;

// Canonical field inventory for projections and the forthcoming v2 codec.
// Identity belongs to the record, never to PlantRegion; avatar pool indices and visual
// transform rotation/scale are excluded. Component presence is always explicit.
// A component schema changes only with a new version and projection version.
template<typename T>
struct Fields;
template<>
struct Fields<C::TransformComponent> {
    static constexpr std::string_view name = "TransformComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("position", value.position);
    }
};
template<>
struct Fields<C::PlantTag> {
    static constexpr std::string_view name = "PlantTag";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        (void)value;
        (void)visitor;
    }
};
template<>
struct Fields<C::PlantGenomeComponent> {
    static constexpr std::string_view name = "PlantGenomeComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("genes", value.genes);
    }
};
template<>
struct Fields<C::PlantGrowthComponent> {
    static constexpr std::string_view name = "PlantGrowthComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("species_id", value.species_id);
        visitor("stage", value.stage);
        visitor("quality", value.quality);
        visitor("growth_points", value.growth_points);
        visitor("stress_points", value.stress_points);
        visitor("tended", value.tended);
        visitor("planted_tick", value.planted_tick);
        visitor("last_tick", value.last_tick);
    }
};
template<>
struct Fields<C::SoilFeederComponent> {
    static constexpr std::string_view name = "SoilFeederComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("uptake", value.uptake);
        visitor("absorbed", value.absorbed);
    }
};
template<>
struct Fields<C::PlantHealthComponent> {
    static constexpr std::string_view name = "PlantHealthComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("infection", value.infection);
        visitor("resistance", value.resistance);
        visitor("state", value.state);
        visitor("infected_ticks", value.infected_ticks);
    }
};
template<>
struct Fields<C::PollinationComponent> {
    static constexpr std::string_view name = "PollinationComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("pollinated", value.pollinated);
        visitor("last_pollen_tick", value.last_pollen_tick);
        visitor("crosses", value.crosses);
        visitor("next_genome", value.next_genome);
    }
};
template<>
struct Fields<C::CropLifecycleComponent> {
    static constexpr std::string_view name = "CropLifecycleComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("perennial", value.perennial);
        visitor("ripe_ticks", value.ripe_ticks);
        visitor("lifespan_ticks", value.lifespan_ticks);
        visitor("generations", value.generations);
        visitor("species_id", value.species_id);
    }
};
template<>
struct Fields<C::PlantRegionComponent> {
    static constexpr std::string_view name = "PlantRegionComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("owner_x", value.owner_x);
        visitor("owner_z", value.owner_z);
        visitor("region_stamp", value.region_stamp);
    }
};
template<>
struct Fields<C::PollinationTag> {
    static constexpr std::string_view name = "PollinationTag";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        (void)value;
        (void)visitor;
    }
};
template<>
struct Fields<C::CombustibleComponent> {
    static constexpr std::string_view name = "CombustibleComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("fuel_milli", value.fuel_milli);
        visitor("moisture_milli", value.moisture_milli);
        visitor("burn_state", value.burn_state);
        visitor("burn_ticks_remaining", value.burn_ticks_remaining);
        visitor("ignition_radius", value.ignition_radius);
    }
};
template<>
struct Fields<C::MortalComponent> {
    static constexpr std::string_view name = "MortalComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("age_ticks", value.age_ticks);
        visitor("lifespan_ticks", value.lifespan_ticks);
        visitor("dead", value.dead);
    }
};
template<>
struct Fields<C::GrazeableComponent> {
    static constexpr std::string_view name = "GrazeableComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("biomass", value.biomass);
        visitor("last_grazed_tick", value.last_grazed_tick);
    }
};
template<>
struct Fields<C::WaterSourceComponent> {
    static constexpr std::string_view name = "WaterSourceComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("output", value.output);
    }
};
template<>
struct Fields<C::IgnitionSourceComponent> {
    static constexpr std::string_view name = "IgnitionSourceComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("radius", value.radius);
        visitor("intensity", value.intensity);
    }
};
template<>
struct Fields<C::FlowingWaterSourceComponent> {
    static constexpr std::string_view name = "FlowingWaterSourceComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("flow_rate", value.flow_rate);
    }
};
template<>
struct Fields<C::DecayComponent> {
    static constexpr std::string_view name = "DecayComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("decay_ticks", value.decay_ticks);
        visitor("decay_duration", value.decay_duration);
        visitor("nutrient_released_milli", value.nutrient_released_milli);
        visitor("fully_decomposed", value.fully_decomposed);
    }
};
template<>
struct Fields<C::FieldEmitterComponent> {
    static constexpr std::string_view name = "FieldEmitterComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("channel", value.channel);
        visitor("rate_raw_per_tick", value.rate_raw_per_tick);
        visitor("radius_cells", value.radius_cells);
    }
};
template<>
struct Fields<C::CreatureComponent> {
    static constexpr std::string_view name = "CreatureComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("is_predator", value.is_predator);
        visitor("species_id", value.species_id);
        visitor("hunger", value.hunger);
        visitor("stamina", value.stamina);
        visitor("energy", value.energy);
        visitor("move_speed", value.move_speed);
        visitor("last_action", value.last_action);
        visitor("wish_x", value.wish_x);
        visitor("wish_z", value.wish_z);
        visitor("eaten", value.eaten);
    }
};
template<>
struct Fields<C::CreatureGenomeComponent> {
    static constexpr std::string_view name = "CreatureGenomeComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("move_speed", value.move_speed);
        visitor("vigilance", value.vigilance);
        visitor("hunger_threshold", value.hunger_threshold);
        visitor("size_scale", value.size_scale);
        visitor("vision_cos_half_fov", value.vision_cos_half_fov);
        visitor("vision_range", value.vision_range);
        visitor("hearing_range", value.hearing_range);
        visitor("age_ticks", value.age_ticks);
        visitor("reproduce_cooldown", value.reproduce_cooldown);
        visitor("generation", value.generation);
        visitor("female", value.female);
        visitor("courting_ticks", value.courting_ticks);
    }
};
template<>
struct Fields<C::CreaturePhysicsComponent> {
    static constexpr std::string_view name = "CreaturePhysicsComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        (void)value;
        (void)visitor;
    }
};
template<>
struct Fields<C::ThirstComponent> {
    static constexpr std::string_view name = "ThirstComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("thirst", value.thirst);
        visitor("wish_x", value.wish_x);
        visitor("wish_z", value.wish_z);
        visitor("drinking", value.drinking);
        visitor("water_proximity", value.water_proximity);
    }
};
template<>
struct Fields<C::WaterHoleComponent> {
    static constexpr std::string_view name = "WaterHoleComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("radius", value.radius);
    }
};
template<>
struct Fields<C::CircadianComponent> {
    static constexpr std::string_view name = "CircadianComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("nocturnal", value.nocturnal);
        visitor("activity", value.activity);
    }
};
template<>
struct Fields<C::AlarmComponent> {
    static constexpr std::string_view name = "AlarmComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("level", value.level);
        visitor("alarmed", value.alarmed);
    }
};
template<>
struct Fields<C::PackHunterComponent> {
    static constexpr std::string_view name = "PackHunterComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("coord_x", value.coord_x);
        visitor("coord_z", value.coord_z);
        visitor("in_pack", value.in_pack);
    }
};
template<>
struct Fields<C::TerritoryComponent> {
    static constexpr std::string_view name = "TerritoryComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("home_x", value.home_x);
        visitor("home_z", value.home_z);
        visitor("radius", value.radius);
        visitor("established", value.established);
    }
};
template<>
struct Fields<C::TerritoryBiasComponent> {
    static constexpr std::string_view name = "TerritoryBiasComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("wish_x", value.wish_x);
        visitor("wish_z", value.wish_z);
    }
};
template<>
struct Fields<C::MigratoryComponent> {
    static constexpr std::string_view name = "MigratoryComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("drive", value.drive);
        visitor("wish_x", value.wish_x);
        visitor("wish_z", value.wish_z);
    }
};
template<>
struct Fields<C::ScavengerComponent> {
    static constexpr std::string_view name = "ScavengerComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("wish_x", value.wish_x);
        visitor("wish_z", value.wish_z);
        visitor("feeding", value.feeding);
    }
};
template<>
struct Fields<C::ForagerComponent> {
    static constexpr std::string_view name = "ForagerComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("cell_x", value.cell_x);
        visitor("cell_z", value.cell_z);
        visitor("home_x", value.home_x);
        visitor("home_z", value.home_z);
        visitor("carrying_food", value.carrying_food);
        visitor("deliveries", value.deliveries);
    }
};
template<>
struct Fields<C::FoodSourceComponent> {
    static constexpr std::string_view name = "FoodSourceComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("cell_x", value.cell_x);
        visitor("cell_z", value.cell_z);
        visitor("amount", value.amount);
    }
};
template<>
struct Fields<C::NeedsComponent> {
    static constexpr std::string_view name = "NeedsComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("needs", value.needs);
    }
};
template<>
struct Fields<C::SensesComponent> {
    static constexpr std::string_view name = "SensesComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("nearest_food_source", value.nearest_food_source);
        visitor("nearest_water_source", value.nearest_water_source);
        visitor("nearest_threat", value.nearest_threat);
        visitor("threat_distance", value.threat_distance);
    }
};
template<>
struct Fields<C::OpportunityComponent> {
    static constexpr std::string_view name = "OpportunityComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("id", value.id);
        visitor("action", value.action);
        visitor("target", value.target);
        visitor("need", value.need);
        visitor("satisfaction", value.satisfaction);
        visitor("urgency", value.urgency);
        visitor("risk", value.risk);
        visitor("stamina_cost", value.stamina_cost);
        visitor("radius", value.radius);
    }
};
template<>
struct Fields<C::ActionPlanComponent> {
    static constexpr std::string_view name = "ActionPlanComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("plan", value.plan);
        visitor("current_action_index", value.current_action_index);
        visitor("time_in_current_action", value.time_in_current_action);
    }
};
template<>
struct Fields<C::InstinctAgent> {
    static constexpr std::string_view name = "InstinctAgent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        (void)value;
        (void)visitor;
    }
};
template<>
struct Fields<C::LocomotionProfile> {
    static constexpr std::string_view name = "LocomotionProfile";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("move_speed", value.move_speed);
        visitor("arrival_radius", value.arrival_radius);
        visitor("slow_radius", value.slow_radius);
        visitor("separation_radius", value.separation_radius);
        visitor("separation_strength", value.separation_strength);
        visitor("flock_radius", value.flock_radius);
        visitor("cohesion_strength", value.cohesion_strength);
        visitor("alignment_strength", value.alignment_strength);
    }
};
template<>
struct Fields<C::SensableComponent> {
    static constexpr std::string_view name = "SensableComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("scent_channel", value.scent_channel);
        visitor("scent_deposit", value.scent_deposit);
        visitor("noise_loudness", value.noise_loudness);
        visitor("noise_pitch", value.noise_pitch);
        visitor("faction", value.faction);
    }
};
template<>
struct Fields<C::PerceptionComponent> {
    static constexpr std::string_view name = "PerceptionComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("vision_cos_half_fov", value.vision_cos_half_fov);
        visitor("vision_range", value.vision_range);
        visitor("facing_x", value.facing_x);
        visitor("facing_z", value.facing_z);
        visitor("ear", value.ear);
        visitor("faction", value.faction);
    }
};
template<>
struct Fields<C::AwarenessComponent> {
    static constexpr std::string_view name = "AwarenessComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("awareness", value.awareness);
        visitor("params", value.params);
    }
};
template<>
struct Fields<C::LocomotionIntentComponent> {
    static constexpr std::string_view name = "LocomotionIntentComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("wish_xz", value.wish_xz);
        visitor("arrived", value.arrived);
    }
};
template<>
struct Fields<C::ScentSenseComponent> {
    static constexpr std::string_view name = "ScentSenseComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("channel", value.channel);
        visitor("sign", value.sign);
        visitor("strength", value.strength);
        visitor("floor", value.floor);
        visitor("weber_k", value.weber_k);
    }
};
template<>
struct Fields<C::LocomotionPathComponent> {
    static constexpr std::string_view name = "LocomotionPathComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("waypoints", value.waypoints);
        visitor("index", value.index);
    }
};
template<>
struct Fields<C::StimulusSubscriptionComponent> {
    static constexpr std::string_view name = "StimulusSubscriptionComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("subscriptions", value.subscriptions);
    }
};
template<>
struct Fields<C::InstinctAgentComponent> {
    static constexpr std::string_view name = "InstinctAgentComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("actor_id", value.actor_id);
        visitor("archetype", value.archetype);
        visitor("replan_interval_ticks", value.replan_interval_ticks);
        visitor("last_planned_tick", value.last_planned_tick);
        visitor("plans_executed", value.plans_executed);
        visitor("current_plan", value.current_plan);
    }
};
template<>
struct Fields<C::WaterInteractorComponent> {
    static constexpr std::string_view name = "WaterInteractorComponent";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("buoyancy_factor", value.buoyancy_factor);
        visitor("submerged_percentage", value.submerged_percentage);
        visitor("fluid_velocity_at_pos", value.fluid_velocity_at_pos);
    }
};
template<>
struct Fields<C::PendingDeletion> {
    static constexpr std::string_view name = "PendingDeletion";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        (void)value;
        (void)visitor;
    }
};
template<>
struct Fields<C::Need> {
    static constexpr std::string_view name = "Need";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("name", value.name);
        visitor("pressure", value.pressure);
        visitor("growth_per_tick", value.growth_per_tick);
    }
};
template<>
struct Fields<C::Action> {
    static constexpr std::string_view name = "Action";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("name", value.name);
        visitor("target", value.target);
        visitor("duration", value.duration);
        visitor("flee", value.flee);
    }
};
template<>
struct Fields<C::StimulusSubscription> {
    static constexpr std::string_view name = "StimulusSubscription";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("channel", value.channel);
        visitor("need", value.need);
        visitor("gain", value.gain);
    }
};
template<>
struct Fields<::luminumbra::ai::HearingProfile> {
    static constexpr std::string_view name = "HearingProfile";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("range", value.range);
        visitor("peak_pitch", value.peak_pitch);
        visitor("bandwidth", value.bandwidth);
        visitor("gain", value.gain);
        visitor("threshold", value.threshold);
    }
};
template<>
struct Fields<::luminumbra::ai::Awareness> {
    static constexpr std::string_view name = "Awareness";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("meter", value.meter);
        visitor("state", value.state);
        visitor("has_last_known", value.has_last_known);
        visitor("last_known_x", value.last_known_x);
        visitor("last_known_z", value.last_known_z);
        visitor("time_since_signal", value.time_since_signal);
    }
};
template<>
struct Fields<::luminumbra::ai::AwarenessParams> {
    static constexpr std::string_view name = "AwarenessParams";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("gain", value.gain);
        visitor("decay", value.decay);
        visitor("suspicious_at", value.suspicious_at);
        visitor("alert_at", value.alert_at);
        visitor("engaged_at", value.engaged_at);
    }
};
template<>
struct Fields<::luminumbra::ai::InstinctCandidate> {
    static constexpr std::string_view name = "InstinctCandidate";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("rank", value.rank);
        visitor("id", value.id);
        visitor("action", value.action);
        visitor("target", value.target);
        visitor("need", value.need);
        visitor("score", value.score);
        visitor("need_pressure", value.need_pressure);
        visitor("reason", value.reason);
    }
};
template<>
struct Fields<::luminumbra::ai::InstinctPlan> {
    static constexpr std::string_view name = "InstinctPlan";
    static constexpr std::uint16_t version = 1;
    template<typename Value, typename Visitor>
    static void Visit(Value& value, Visitor&& visitor) {
        visitor("actor_id", value.actor_id);
        visitor("archetype", value.archetype);
        visitor("decision_contract", value.decision_contract);
        visitor("candidates", value.candidates);
        visitor("selected_index", value.selected_index);
        visitor("checksum", value.checksum);
        visitor("passed", value.passed);
    }
};

template<typename... Types>
struct ComponentList {
    template<typename Visitor>
    static void Visit(Visitor&& visitor) {
        (visitor.template operator()<Types>(), ...);
    }
};

// Seventeen admitted plant kinds; PlantTag is the record's role, not a codec
// component. Transform carries position only, matching the plant save contract.
using PlantComponents = ComponentList<C::TransformComponent,
                                      C::PlantGenomeComponent,
                                      C::PlantGrowthComponent,
                                      C::SoilFeederComponent,
                                      C::PlantHealthComponent,
                                      C::PollinationComponent,
                                      C::CropLifecycleComponent,
                                      C::PlantRegionComponent,
                                      C::PollinationTag,
                                      C::CombustibleComponent,
                                      C::MortalComponent,
                                      C::GrazeableComponent,
                                      C::WaterSourceComponent,
                                      C::IgnitionSourceComponent,
                                      C::FlowingWaterSourceComponent,
                                      C::DecayComponent,
                                      C::FieldEmitterComponent>;
using ParticipantComponents = ComponentList<C::TransformComponent,
                                            C::PlantTag,
                                            C::PlantGenomeComponent,
                                            C::PlantGrowthComponent,
                                            C::SoilFeederComponent,
                                            C::PlantHealthComponent,
                                            C::PollinationComponent,
                                            C::CropLifecycleComponent,
                                            C::PlantRegionComponent,
                                            C::PollinationTag,
                                            C::CombustibleComponent,
                                            C::MortalComponent,
                                            C::GrazeableComponent,
                                            C::WaterSourceComponent,
                                            C::IgnitionSourceComponent,
                                            C::FlowingWaterSourceComponent,
                                            C::DecayComponent,
                                            C::FieldEmitterComponent,
                                            C::CreatureComponent,
                                            C::CreatureGenomeComponent,
                                            C::CreaturePhysicsComponent,
                                            C::ThirstComponent,
                                            C::WaterHoleComponent,
                                            C::CircadianComponent,
                                            C::AlarmComponent,
                                            C::PackHunterComponent,
                                            C::TerritoryComponent,
                                            C::TerritoryBiasComponent,
                                            C::MigratoryComponent,
                                            C::ScavengerComponent,
                                            C::ForagerComponent,
                                            C::FoodSourceComponent,
                                            C::NeedsComponent,
                                            C::SensesComponent,
                                            C::OpportunityComponent,
                                            C::ActionPlanComponent,
                                            C::InstinctAgent,
                                            C::LocomotionProfile,
                                            C::SensableComponent,
                                            C::PerceptionComponent,
                                            C::AwarenessComponent,
                                            C::LocomotionIntentComponent,
                                            C::ScentSenseComponent,
                                            C::LocomotionPathComponent,
                                            C::StimulusSubscriptionComponent,
                                            C::InstinctAgentComponent,
                                            C::WaterInteractorComponent,
                                            C::PendingDeletion>;
} // namespace projection

struct ParticipantProjectionEntry {
    std::uint64_t id = 0;
    entt::entity entity = entt::null;
};

// Reused checkpoint scratch. Entries are bounded by the identified participant
// pool size (one entry per live participant). No chunk/page traversal, world
// snapshot, identity allocation or repair occurs.
struct ParticipantProjectionScratch {
    std::vector<ParticipantProjectionEntry> entries;
    std::vector<ParticipantProjectionEntry> radix;
    // Const observational oracle: number of records whose fields were read.
    std::size_t participant_visits = 0;
    // Identity/role pool members inspected, including excluded plants.
    std::size_t source_visits = 0;
};

// Throws on invalid/duplicate/missing identity or a non-finite simulation value.
// References encode null, invalid, or a valid stable ID. Lists retain their
// simulation order. Integers are little endian; floats are IEEE binary32 (and
// planner doubles binary64), with both zeros encoded as positive zero.
// A plant roster includes the shared cursor, even after extinction.
[[nodiscard]] std::string ComputePlantProjection(const entt::registry& registry,
                                                 std::uint64_t next_id,
                                                 ParticipantProjectionScratch& scratch);
[[nodiscard]] std::string ComputeEcologyProjection(const entt::registry& registry,
                                                   ParticipantProjectionScratch& scratch);

// Checkpoint adapters retain thread-local scratch across sessions/checkpoints;
// no session or registry pointer is retained. Call only at the authoritative
// checkpoint boundary while the registry is quiescent.
[[nodiscard]] std::string ComputeCandidatePlantSubHash(const entt::registry& registry,
                                                       std::uint64_t next_id);
[[nodiscard]] std::string ComputeCandidateEcologySubHash(const entt::registry& registry);

} // namespace Luminumbra::simulation
