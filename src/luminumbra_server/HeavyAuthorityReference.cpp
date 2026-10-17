#include "HeavyAuthorityReference.h"

#include "luminumbra_common/components/AlarmComponents.h"
#include "luminumbra_common/components/CircadianComponents.h"
#include "luminumbra_common/components/CombustionComponents.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/CropLifecycleComponents.h"
#include "luminumbra_common/components/DecayComponents.h"
#include "luminumbra_common/components/DiseaseComponents.h"
#include "luminumbra_common/components/FieldEmitterComponents.h"
#include "luminumbra_common/components/ForagingComponents.h"
#include "luminumbra_common/components/GrazeableComponent.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/components/IrrigationComponents.h"
#include "luminumbra_common/components/MigratoryComponents.h"
#include "luminumbra_common/components/MortalComponents.h"
#include "luminumbra_common/components/PackHunterComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/components/PollinationComponents.h"
#include "luminumbra_common/components/ScavengerComponent.h"
#include "luminumbra_common/components/SoilComponents.h"
#include "luminumbra_common/components/TerritoryComponents.h"
#include "luminumbra_common/components/ThirstComponents.h"
#include "luminumbra_common/core/WaterComponents.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/simulation/SimulationEventBus.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"

#include "luminumbra_common/core/BoundedFileReader.h"
#include "luminumbra_common/core/FilesystemPath.h"

#include <lz4.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Luminumbra::Server {
namespace {
namespace C = Components;
namespace W = world;
namespace A = luminumbra::ai;

// This inventory is deliberately separate from persistence component registration. Field order
// is local to reference v1. No object representation, padding or runtime handle is encoded.
template<class T>
struct Inventory;
template<>
struct Inventory<C::CreatureRegionComponent> {
    static auto Fields(const C::CreatureRegionComponent& v) {
        return std::tie(v.persistent_id, v.owner_x, v.owner_z, v.region_stamp);
    }
};
template<>
struct Inventory<C::CreatureCoarseNeed> {
    static auto Fields(const C::CreatureCoarseNeed& v) {
        return std::tie(v.name, v.pressure_ppm);
    }
};
template<>
struct Inventory<C::CreatureCoarseComponent> {
    static auto Fields(const C::CreatureCoarseComponent& v) {
        return std::tie(v.tier,
                        v.transition,
                        v.x_mm,
                        v.y_mm,
                        v.z_mm,
                        v.hunger_ppm,
                        v.thirst_ppm,
                        v.needs,
                        v.age_cadences,
                        v.reproduce_cadences,
                        v.reproduce_cooldown_cadences,
                        v.courting_cadences,
                        v.travel_pending,
                        v.destination_x,
                        v.destination_z);
    }
};
template<>
struct Inventory<C::CreatureLocomotionStateComponent> {
    static auto Fields(const C::CreatureLocomotionStateComponent& v) {
        return std::tie(v.velocity_x, v.velocity_y, v.velocity_z, v.grounded);
    }
};
template<>
struct Inventory<C::CreatureComponent> {
    static auto Fields(const C::CreatureComponent& v) {
        return std::tie(v.species_id,
                        v.is_predator,
                        v.eaten,
                        v.hunger,
                        v.stamina,
                        v.energy,
                        v.move_speed,
                        v.last_action,
                        v.wish_x,
                        v.wish_z);
    }
};
template<>
struct Inventory<C::CreatureGenomeComponent> {
    static auto Fields(const C::CreatureGenomeComponent& v) {
        return std::tie(v.generation,
                        v.female,
                        v.age_ticks,
                        v.reproduce_cooldown,
                        v.courting_ticks,
                        v.move_speed,
                        v.vigilance,
                        v.hunger_threshold,
                        v.size_scale,
                        v.vision_cos_half_fov,
                        v.vision_range,
                        v.hearing_range);
    }
};
template<>
struct Inventory<C::MortalComponent> {
    static auto Fields(const C::MortalComponent& v) {
        return std::tie(v.dead, v.age_ticks, v.lifespan_ticks);
    }
};
template<>
struct Inventory<C::DecayComponent> {
    static auto Fields(const C::DecayComponent& v) {
        return std::tie(
            v.fully_decomposed, v.decay_ticks, v.decay_duration, v.nutrient_released_milli);
    }
};
template<>
struct Inventory<C::ThirstComponent> {
    static auto Fields(const C::ThirstComponent& v) {
        return std::tie(v.thirst, v.drinking, v.water_proximity, v.wish_x, v.wish_z);
    }
};
template<>
struct Inventory<C::CircadianComponent> {
    static auto Fields(const C::CircadianComponent& v) {
        return std::tie(v.nocturnal, v.activity);
    }
};
template<>
struct Inventory<C::AlarmComponent> {
    static auto Fields(const C::AlarmComponent& v) {
        return std::tie(v.alarmed, v.level);
    }
};
template<>
struct Inventory<C::PackHunterComponent> {
    static auto Fields(const C::PackHunterComponent& v) {
        return std::tie(v.in_pack, v.coord_x, v.coord_z);
    }
};
template<>
struct Inventory<C::TerritoryComponent> {
    static auto Fields(const C::TerritoryComponent& v) {
        return std::tie(v.established, v.home_x, v.home_z, v.radius);
    }
};
template<>
struct Inventory<C::TerritoryBiasComponent> {
    static auto Fields(const C::TerritoryBiasComponent& v) {
        return std::tie(v.wish_x, v.wish_z);
    }
};
template<>
struct Inventory<C::MigratoryComponent> {
    static auto Fields(const C::MigratoryComponent& v) {
        return std::tie(v.drive, v.wish_x, v.wish_z);
    }
};
template<>
struct Inventory<C::ScavengerComponent> {
    static auto Fields(const C::ScavengerComponent& v) {
        return std::tie(v.feeding, v.wish_x, v.wish_z);
    }
};
template<>
struct Inventory<C::Need> {
    static auto Fields(const C::Need& v) {
        return std::tie(v.name, v.pressure, v.growth_per_tick);
    }
};
template<>
struct Inventory<C::NeedsComponent> {
    static auto Fields(const C::NeedsComponent& v) {
        return std::tie(v.needs);
    }
};
template<>
struct Inventory<C::SensesComponent> {
    static auto Fields(const C::SensesComponent& v) {
        return std::tie(
            v.nearest_threat, v.nearest_food_source, v.nearest_water_source, v.threat_distance);
    }
};
template<>
struct Inventory<C::OpportunityComponent> {
    static auto Fields(const C::OpportunityComponent& v) {
        return std::tie(v.id,
                        v.action,
                        v.target,
                        v.need,
                        v.radius,
                        v.satisfaction,
                        v.urgency,
                        v.risk,
                        v.stamina_cost);
    }
};
template<>
struct Inventory<C::Action> {
    static auto Fields(const C::Action& v) {
        return std::tie(v.name, v.target, v.flee, v.duration);
    }
};
template<>
struct Inventory<C::ActionPlanComponent> {
    static auto Fields(const C::ActionPlanComponent& v) {
        return std::tie(v.current_action_index, v.time_in_current_action, v.plan);
    }
};
template<>
struct Inventory<C::LocomotionProfile> {
    static auto Fields(const C::LocomotionProfile& v) {
        return std::tie(v.move_speed,
                        v.arrival_radius,
                        v.slow_radius,
                        v.separation_radius,
                        v.separation_strength,
                        v.flock_radius,
                        v.cohesion_strength,
                        v.alignment_strength);
    }
};
template<>
struct Inventory<C::SensableComponent> {
    static auto Fields(const C::SensableComponent& v) {
        return std::tie(
            v.faction, v.scent_channel, v.scent_deposit, v.noise_loudness, v.noise_pitch);
    }
};
template<>
struct Inventory<C::PerceptionComponent> {
    static auto Fields(const C::PerceptionComponent& v) {
        return std::tie(
            v.faction, v.vision_cos_half_fov, v.vision_range, v.facing_x, v.facing_z, v.ear);
    }
};
template<>
struct Inventory<A::HearingProfile> {
    static auto Fields(const A::HearingProfile& v) {
        return std::tie(v.range, v.peak_pitch, v.bandwidth, v.gain, v.threshold);
    }
};
template<>
struct Inventory<C::AwarenessComponent> {
    static auto Fields(const C::AwarenessComponent& v) {
        return std::tie(v.awareness, v.params);
    }
};
template<>
struct Inventory<A::Awareness> {
    static auto Fields(const A::Awareness& v) {
        return std::tie(v.state,
                        v.meter,
                        v.has_last_known,
                        v.last_known_x,
                        v.last_known_z,
                        v.time_since_signal);
    }
};
template<>
struct Inventory<A::AwarenessParams> {
    static auto Fields(const A::AwarenessParams& v) {
        return std::tie(v.gain, v.decay, v.suspicious_at, v.alert_at, v.engaged_at);
    }
};
template<>
struct Inventory<C::LocomotionIntentComponent> {
    static auto Fields(const C::LocomotionIntentComponent& v) {
        return std::tie(v.arrived, v.wish_xz);
    }
};
template<>
struct Inventory<C::ScentSenseComponent> {
    static auto Fields(const C::ScentSenseComponent& v) {
        return std::tie(v.channel, v.sign, v.strength, v.floor, v.weber_k);
    }
};
template<>
struct Inventory<C::LocomotionPathComponent> {
    static auto Fields(const C::LocomotionPathComponent& v) {
        return std::tie(v.index, v.waypoints);
    }
};
template<>
struct Inventory<C::StimulusSubscription> {
    static auto Fields(const C::StimulusSubscription& v) {
        return std::tie(v.channel, v.need, v.gain);
    }
};
template<>
struct Inventory<C::StimulusSubscriptionComponent> {
    static auto Fields(const C::StimulusSubscriptionComponent& v) {
        return std::tie(v.subscriptions);
    }
};
template<>
struct Inventory<C::InstinctAgentComponent> {
    static auto Fields(const C::InstinctAgentComponent& v) {
        return std::tie(v.actor_id,
                        v.archetype,
                        v.replan_interval_ticks,
                        v.last_planned_tick,
                        v.plans_executed,
                        v.current_plan);
    }
};
template<>
struct Inventory<A::InstinctPlan> {
    static auto Fields(const A::InstinctPlan& v) {
        return std::tie(v.actor_id,
                        v.archetype,
                        v.decision_contract,
                        v.selected_index,
                        v.passed,
                        v.checksum,
                        v.candidates);
    }
};
template<>
struct Inventory<A::InstinctCandidate> {
    static auto Fields(const A::InstinctCandidate& v) {
        return std::tie(
            v.rank, v.id, v.action, v.target, v.need, v.score, v.need_pressure, v.reason);
    }
};
template<>
struct Inventory<C::ForagerComponent> {
    static auto Fields(const C::ForagerComponent& v) {
        return std::tie(v.cell_x, v.cell_z, v.home_x, v.home_z, v.carrying_food, v.deliveries);
    }
};
template<>
struct Inventory<C::FoodSourceComponent> {
    static auto Fields(const C::FoodSourceComponent& v) {
        return std::tie(v.cell_x, v.cell_z, v.amount);
    }
};
template<>
struct Inventory<C::FieldEmitterComponent> {
    static auto Fields(const C::FieldEmitterComponent& v) {
        return std::tie(v.channel, v.rate_raw_per_tick, v.radius_cells);
    }
};
template<>
struct Inventory<C::GrazeableComponent> {
    static auto Fields(const C::GrazeableComponent& v) {
        return std::tie(v.biomass, v.last_grazed_tick);
    }
};
template<>
struct Inventory<C::WaterHoleComponent> {
    static auto Fields(const C::WaterHoleComponent& v) {
        return std::tie(v.radius);
    }
};
template<>
struct Inventory<C::CombustibleComponent> {
    static auto Fields(const C::CombustibleComponent& v) {
        return std::tie(v.burn_state,
                        v.burn_ticks_remaining,
                        v.fuel_milli,
                        v.moisture_milli,
                        v.ignition_radius);
    }
};
template<>
struct Inventory<C::IgnitionSourceComponent> {
    static auto Fields(const C::IgnitionSourceComponent& v) {
        return std::tie(v.radius, v.intensity);
    }
};
template<>
struct Inventory<C::WaterSourceComponent> {
    static auto Fields(const C::WaterSourceComponent& v) {
        return std::tie(v.output);
    }
};
template<>
struct Inventory<C::WaterFlowSourceComponent> {
    static auto Fields(const C::WaterFlowSourceComponent& v) {
        return std::tie(v.flow_rate);
    }
};
template<>
struct Inventory<C::PlantRegionComponent> {
    static auto Fields(const C::PlantRegionComponent& v) {
        return std::tie(v.persistent_id, v.owner_x, v.owner_z, v.region_stamp);
    }
};
template<>
struct Inventory<C::PlantGenomeComponent> {
    static auto Fields(const C::PlantGenomeComponent& v) {
        return std::tie(v.genes);
    }
};
template<>
struct Inventory<C::PlantGrowthComponent> {
    static auto Fields(const C::PlantGrowthComponent& v) {
        return std::tie(v.species_id,
                        v.stage,
                        v.quality,
                        v.tended,
                        v.growth_points,
                        v.stress_points,
                        v.planted_tick,
                        v.last_tick);
    }
};
template<>
struct Inventory<C::PlantHealthComponent> {
    static auto Fields(const C::PlantHealthComponent& v) {
        return std::tie(v.state, v.infection, v.resistance, v.infected_ticks);
    }
};
template<>
struct Inventory<C::SoilFeederComponent> {
    static auto Fields(const C::SoilFeederComponent& v) {
        return std::tie(v.uptake, v.absorbed);
    }
};
template<>
struct Inventory<C::PollinationComponent> {
    static auto Fields(const C::PollinationComponent& v) {
        return std::tie(v.pollinated, v.last_pollen_tick, v.crosses, v.next_genome);
    }
};
template<>
struct Inventory<C::CropLifecycleComponent> {
    static auto Fields(const C::CropLifecycleComponent& v) {
        return std::tie(v.perennial, v.ripe_ticks, v.lifespan_ticks, v.generations, v.species_id);
    }
};
template<>
struct Inventory<Vec2> {
    static auto Fields(const Vec2& v) {
        return std::tie(v.x, v.y);
    }
};
template<>
struct Inventory<Vec3> {
    static auto Fields(const Vec3& v) {
        return std::tie(v.x, v.y, v.z);
    }
};
template<>
struct Inventory<Quat> {
    static auto Fields(const Quat& v) {
        return std::tie(v.w, v.x, v.y, v.z);
    }
};
template<>
struct Inventory<IVec3> {
    static auto Fields(const IVec3& v) {
        return std::tie(v.x, v.y, v.z);
    }
};
template<>
struct Inventory<W::RegionKey> {
    static auto Fields(const W::RegionKey& v) {
        return std::tie(v.x, v.z);
    }
};
template<>
struct Inventory<W::FieldPageKey> {
    static auto Fields(const W::FieldPageKey& v) {
        return std::tie(v.x, v.z);
    }
};
template<>
struct Inventory<W::ScentCellKey> {
    static auto Fields(const W::ScentCellKey& v) {
        return std::tie(v.x, v.z);
    }
};
template<>
struct Inventory<W::FieldPage> {
    static auto Fields(const W::FieldPage& v) {
        return std::tie(v.mask, v.cells);
    }
};
template<>
struct Inventory<W::FoliageFieldPages::Page> {
    static auto Fields(const W::FoliageFieldPages::Page& v) {
        return std::tie(v.mask, v.cells);
    }
};
template<>
struct Inventory<W::RegionalStorm> {
    static auto Fields(const W::RegionalStorm& v) {
        return std::tie(v.owner,
                        v.spawn_tick,
                        v.seed_salt,
                        v.center_x,
                        v.center_z,
                        v.velocity_x,
                        v.velocity_z,
                        v.intensity,
                        v.lifetime_ticks,
                        v.active_age);
    }
};
template<>
struct Inventory<W::RegionalStrike> {
    static auto Fields(const W::RegionalStrike& v) {
        return std::tie(
            v.owner, v.strike_tick, v.storm_salt, v.world_x, v.world_z, v.magnitude, v.active_age);
    }
};
template<>
struct Inventory<W::GroundObject> {
    static auto Fields(const W::GroundObject& v) {
        return std::tie(v.id,
                        v.type,
                        v.x_mm,
                        v.y_mm,
                        v.z_mm,
                        v.spawn_tick,
                        v.active_age,
                        v.despawn_after,
                        v.payload,
                        v.flags);
    }
};
template<>
struct Inventory<W::RegionSchedulerConfig> {
    static auto Fields(const W::RegionSchedulerConfig& v) {
        return std::tie(v.work_limit, v.hold_ticks, v.reduced_shift);
    }
};
template<>
struct Inventory<W::ActiveRegionRecord> {
    static auto Fields(const W::ActiveRegionRecord& v) {
        return std::tie(v.visited,
                        v.edited,
                        v.pinned,
                        v.populated,
                        v.state,
                        v.cadence_shift,
                        v.cadence_phase,
                        v.wake_pending,
                        v.first_active,
                        v.last_proximity,
                        v.last_edit,
                        v.last_ticked,
                        v.frozen_at,
                        v.last_decision,
                        v.pressure,
                        v.over_hold,
                        v.under_hold,
                        v.water_cursor,
                        v.frozen_digest);
    }
};
template<>
struct Inventory<W::ActivationIntent> {
    static auto Fields(const W::ActivationIntent& v) {
        return std::tie(v.creation_pending, v.wake_cause);
    }
};
template<>
struct Inventory<W::PendingRegionActivation> {
    static auto Fields(const W::PendingRegionActivation& v) {
        return std::tie(v.key, v.activation_tick, v.cause);
    }
};
template<>
struct Inventory<W::SimulationContentIdentity> {
    static auto Fields(const W::SimulationContentIdentity& v) {
        return std::tie(v.encoding_version,
                        v.defaults_version,
                        v.algorithm_version,
                        v.effective_policy_digest,
                        v.creature_definitions_digest,
                        v.plant_definitions_digest,
                        v.population_digest);
    }
};
template<>
struct Inventory<W::CapturedSpeciesDefinition> {
    static auto Fields(const W::CapturedSpeciesDefinition& v) {
        return std::tie(v.name, v.species_id, v.bytes);
    }
};
template<>
struct Inventory<W::CreatureDistantPolicy> {
    static auto Fields(const W::CreatureDistantPolicy& v) {
        return std::tie(v.coarse_need_rate_ppm,
                        v.coarse_travel_m_per_cadence,
                        v.promote_distance_m,
                        v.lifespan_cadences,
                        v.reproduce_cadences);
    }
};
template<>
struct Inventory<W::PlantDistantPolicy> {
    static auto Fields(const W::PlantDistantPolicy& v) {
        return std::tie(v.max_cadence_shift, v.resume_cap_ticks);
    }
};
template<>
struct Inventory<luminumbra::simulation::ActivationDeliveryKey> {
    static auto Fields(const luminumbra::simulation::ActivationDeliveryKey& v) {
        return std::tie(v.activation_tick, v.x, v.z);
    }
};
template<>
struct Inventory<luminumbra::simulation::SimulationEvent> {
    static auto Fields(const luminumbra::simulation::SimulationEvent& v) {
        return std::tie(v.tick, v.lane, v.sequence, v.topic, v.payload, v.activation);
    }
};
template<>
struct Inventory<luminumbra::fields::EnergyFieldState::PendingDeposit> {
    static auto Fields(const luminumbra::fields::EnergyFieldState::PendingDeposit& v) {
        return std::tie(v.cell_key, v.channel, v.emitter_id, v.amount);
    }
};
template<>
struct Inventory<World::PlayerAvatar> {
    static auto Fields(const World::PlayerAvatar& v) {
        return std::tie(v.player_id, v.position, v.facing, v.velocity);
    }
};
template<>
struct Inventory<A::EcologyTuning> {
    static auto Fields(const A::EcologyTuning& v) {
        return std::tie(v.energy_drain_per_second,
                        v.energy_rest_recover,
                        v.energy_sleep_recover,
                        v.hunger_growth_per_second,
                        v.hunger_graze_sate,
                        v.stamina_rest_recover,
                        v.stamina_move_drain,
                        v.herd_weight,
                        v.alignment_weight,
                        v.catch_radius,
                        v.catch_satiation,
                        v.flock_neighbor_radius,
                        v.flock_separation_radius,
                        v.flock_cohesion_weight,
                        v.flock_separation_weight);
    }
};
template<>
struct Inventory<A::WildlifeFoliageTuning> {
    static auto Fields(const A::WildlifeFoliageTuning& v) {
        return std::tie(v.graze_radius, v.graze_per_creature, v.regrow_per_tick, v.feed_per_graze);
    }
};
template<>
struct Inventory<A::ThirstTuning> {
    static auto Fields(const A::ThirstTuning& v) {
        return std::tie(v.rise_rate, v.drink_rate, v.seek_threshold);
    }
};
template<>
struct Inventory<A::ScavengingTuning> {
    static auto Fields(const A::ScavengingTuning& v) {
        return std::tie(v.hunger_threshold, v.feed_radius, v.feed_rate);
    }
};
template<>
struct Inventory<A::ForagingParams> {
    static auto Fields(const A::ForagingParams& v) {
        return std::tie(v.deposit, v.trail_weight, v.goal_weight);
    }
};
template<>
struct Inventory<A::ReproductionTuning> {
    static auto Fields(const A::ReproductionTuning& v) {
        return std::tie(v.maturity_ticks,
                        v.cooldown_ticks,
                        v.courtship_ticks,
                        v.healthy_stamina,
                        v.mate_seek_radius,
                        v.courtship_radius,
                        v.spawn_radius);
    }
};
template<>
struct Inventory<Systems::TerrainGenParams> {
    static auto Fields(const Systems::TerrainGenParams& v) {
        return std::tie(v.base_frequency,
                        v.base_amplitude,
                        v.octaves,
                        v.persistence,
                        v.lacunarity,
                        v.height_offset,
                        v.caves_enabled,
                        v.cave_frequency,
                        v.cave_threshold,
                        v.cave_carve_value,
                        v.spaghetti_frequency,
                        v.spaghetti_thickness,
                        v.worley_frequency,
                        v.worley_threshold,
                        v.surface_breaks_enabled,
                        v.surface_break_density,
                        v.feature_cell_size,
                        v.max_feature_radius,
                        v.carve_smoothness,
                        v.entrance_min_cap,
                        v.island_mask_enabled,
                        v.island_mask_frequency,
                        v.continentalness_frequency,
                        v.erosion_frequency,
                        v.peaks_frequency,
                        v.peaks_amplitude,
                        v.domain_warp_amplitude,
                        v.domain_warp_frequency,
                        v.continental_spline,
                        v.erosion_spline,
                        v.peaks_spline,
                        v.biomes_enabled,
                        v.temperature_frequency,
                        v.humidity_frequency,
                        v.biome_relief_enabled,
                        v.biome_relief_strength,
                        v.cliffs_enabled,
                        v.cliff_frequency,
                        v.cliff_threshold,
                        v.cliff_step,
                        v.rivers_enabled,
                        v.river_frequency,
                        v.river_pv_min,
                        v.river_pv_max,
                        v.river_depth,
                        v.river_max_carve,
                        v.lakes_enabled,
                        v.lake_frequency,
                        v.lake_threshold,
                        v.lake_depth,
                        v.lake_max_carve,
                        v.lake_bank_offset,
                        v.biome_table_content_hash,
                        v.structures_enabled,
                        v.structures_content_hash,
                        v.hydro_enabled,
                        v.hydro_iterations,
                        v.hydro_cell_size_m,
                        v.hydro_talus_height,
                        v.hydro_thermal_rate,
                        v.hydro_rain_per_sweep,
                        v.hydro_solubility,
                        v.hydro_deposition,
                        v.hydro_evaporation,
                        v.hydro_sediment_capacity,
                        v.hydro_max_offset);
    }
};

// Resolved generation data participates independently of its production content digests.
template<>
struct Inventory<World::BiomeClimateRange> {
    static auto Fields(const World::BiomeClimateRange& v) {
        return std::tie(v.min, v.max);
    }
};
template<>
struct Inventory<World::BiomeSurfacePalette> {
    static auto Fields(const World::BiomeSurfacePalette& v) {
        return std::tie(v.top, v.filler, v.depth, v.underwater);
    }
};
template<>
struct Inventory<World::BiomeDefinition> {
    static auto Fields(const World::BiomeDefinition& v) {
        return std::tie(v.id,
                        v.name,
                        v.continentalness,
                        v.erosion,
                        v.peaks_valleys,
                        v.temperature,
                        v.humidity,
                        v.palette);
    }
};
template<>
struct Inventory<World::StructureSocket> {
    static auto Fields(const World::StructureSocket& v) {
        return std::tie(v.name, v.position);
    }
};
template<>
struct Inventory<World::StructureBox> {
    static auto Fields(const World::StructureBox& v) {
        return std::tie(v.min, v.size, v.material);
    }
};
template<>
struct Inventory<World::StructurePiece> {
    static auto Fields(const World::StructurePiece& v) {
        return std::tie(v.name, v.boxes, v.sockets);
    }
};
template<>
struct Inventory<World::StructureTemplatePool> {
    static auto Fields(const World::StructureTemplatePool& v) {
        return std::tie(
            v.type, v.spacing, v.separation, v.salt, v.density, v.pieces, v.footprint_radius);
    }
};

class Writer {
public:
    explicit Writer(const entt::registry* registry = nullptr)
        : registry_(registry) {}
    static_assert(sizeof(int) == 4 && sizeof(float) == 4 && sizeof(double) == 8);
    std::string bytes;
    void Integer(std::uint64_t value, unsigned width) {
        for (unsigned byte = 0; byte < width; ++byte)
            bytes.push_back(static_cast<char>((value >> (byte * 8)) & 255u));
    }
    template<class... T>
    void Fields(const T&... values) {
        (Put(values), ...);
    }
    template<class T>
    void Put(const T& value) {
        if constexpr (std::is_same_v<T, entt::entity>) {
            Reference(value);
        } else if constexpr (std::is_same_v<T, bool>) {
            Integer(value ? 1u : 0u, 1);
        } else if constexpr (std::is_integral_v<T>) {
            Integer(static_cast<std::uint64_t>(value), static_cast<unsigned>(sizeof(T)));
        } else if constexpr (std::is_enum_v<T>) {
            Put(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_same_v<T, float>) {
            Put(std::bit_cast<std::uint32_t>(value));
        } else if constexpr (std::is_same_v<T, double>) {
            Put(std::bit_cast<std::uint64_t>(value));
        } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
            const std::string_view text(value);
            Put(static_cast<std::uint64_t>(text.size()));
            bytes.append(text);
        } else if constexpr (requires { value.has_value(); }) {
            Put(value.has_value());
            if (value)
                Put(*value);
        } else if constexpr (requires {
                                 value.first;
                                 value.second;
                             }) {
            Fields(value.first, value.second);
        } else if constexpr (requires {
                                 value.begin();
                                 value.end();
                                 value.size();
                             }) {
            Put(static_cast<std::uint64_t>(value.size()));
            for (const auto& member : value)
                Put(member);
        } else {
            std::apply([this](const auto&... members) { this->Fields(members...); },
                       Inventory<T>::Fields(value));
        }
    }
    template<class T>
    void Component(const entt::registry& registry, entt::entity entity, std::string_view name) {
        Put(name);
        Put(std::uint32_t{1});
        const auto* component = registry.try_get<T>(entity);
        Put(component != nullptr);
        if (component)
            Put(*component);
    }
    template<class T>
    void Tag(const entt::registry& registry, entt::entity entity, std::string_view name) {
        Fields(name, std::uint32_t{1}, registry.all_of<T>(entity));
    }

private:
    const entt::registry* registry_;
    void Reference(entt::entity entity) {
        if (entity == entt::null) {
            Fields(std::uint8_t{0}, std::uint64_t{0});
            return;
        }
        if (!registry_ || !registry_->valid(entity))
            throw std::runtime_error("Invalid Heavy entity reference");
        if (const auto* creature = registry_->try_get<C::CreatureRegionComponent>(entity))
            Fields(std::uint8_t{1}, creature->persistent_id);
        else if (const auto* plant = registry_->try_get<C::PlantRegionComponent>(entity))
            Fields(std::uint8_t{2}, plant->persistent_id);
        else
            throw std::runtime_error("Heavy reference has no durable identity");
    }
};

std::string Creatures(const entt::registry& registry) {
    std::map<std::uint64_t, entt::entity> ordered;
    for (const auto entity : registry.view<const C::CreatureComponent>()) {
        const auto* owner = registry.try_get<C::CreatureRegionComponent>(entity);
        if (!owner || !owner->persistent_id ||
            !ordered.emplace(owner->persistent_id, entity).second)
            throw std::runtime_error("Invalid Heavy creature identity");
    }
    Writer writer(&registry);
    writer.Put(static_cast<std::uint64_t>(ordered.size()));
    for (const auto& [id, entity] : ordered) {
        writer.Put(id);
        const auto* transform = registry.try_get<C::TransformComponent>(entity);
        writer.Put(transform != nullptr);
        if (transform)
            writer.Fields(transform->position, transform->rotation, transform->scale);
        // Runtime controller allocation is excluded; durable participation survives teardown.
        const auto* controller = registry.try_get<C::CreatureControllerComponent>(entity);
        writer.Fields(std::string_view{"Controller"},
                      std::uint32_t{1},
                      registry.all_of<C::CreaturePhysicsComponent>(entity) ||
                          (controller && controller->physics_controlled));
        writer.Component<C::CreatureRegionComponent>(registry, entity, "CreatureRegionComponent");
        writer.Component<C::CreatureCoarseComponent>(registry, entity, "CreatureCoarseComponent");
        writer.Component<C::CreatureLocomotionStateComponent>(
            registry, entity, "CreatureLocomotionStateComponent");
        writer.Component<C::CreatureComponent>(registry, entity, "CreatureComponent");
        writer.Component<C::CreatureGenomeComponent>(registry, entity, "CreatureGenomeComponent");
        writer.Component<C::MortalComponent>(registry, entity, "MortalComponent");
        writer.Component<C::DecayComponent>(registry, entity, "DecayComponent");
        writer.Component<C::ThirstComponent>(registry, entity, "ThirstComponent");
        writer.Component<C::CircadianComponent>(registry, entity, "CircadianComponent");
        writer.Component<C::AlarmComponent>(registry, entity, "AlarmComponent");
        writer.Component<C::PackHunterComponent>(registry, entity, "PackHunterComponent");
        writer.Component<C::TerritoryComponent>(registry, entity, "TerritoryComponent");
        writer.Component<C::TerritoryBiasComponent>(registry, entity, "TerritoryBiasComponent");
        writer.Component<C::MigratoryComponent>(registry, entity, "MigratoryComponent");
        writer.Component<C::ScavengerComponent>(registry, entity, "ScavengerComponent");
        writer.Component<C::NeedsComponent>(registry, entity, "NeedsComponent");
        writer.Component<C::SensesComponent>(registry, entity, "SensesComponent");
        writer.Component<C::OpportunityComponent>(registry, entity, "OpportunityComponent");
        writer.Component<C::ActionPlanComponent>(registry, entity, "ActionPlanComponent");
        writer.Component<C::LocomotionProfile>(registry, entity, "LocomotionProfile");
        writer.Component<C::SensableComponent>(registry, entity, "SensableComponent");
        writer.Component<C::PerceptionComponent>(registry, entity, "PerceptionComponent");
        writer.Component<C::AwarenessComponent>(registry, entity, "AwarenessComponent");
        writer.Component<C::LocomotionIntentComponent>(
            registry, entity, "LocomotionIntentComponent");
        writer.Component<C::ScentSenseComponent>(registry, entity, "ScentSenseComponent");
        writer.Component<C::LocomotionPathComponent>(registry, entity, "LocomotionPathComponent");
        writer.Component<C::StimulusSubscriptionComponent>(
            registry, entity, "StimulusSubscriptionComponent");
        writer.Component<C::InstinctAgentComponent>(registry, entity, "InstinctAgentComponent");
        writer.Component<C::ForagerComponent>(registry, entity, "ForagerComponent");
        writer.Component<C::FoodSourceComponent>(registry, entity, "FoodSourceComponent");
        writer.Component<C::FieldEmitterComponent>(registry, entity, "FieldEmitterComponent");
        writer.Component<C::GrazeableComponent>(registry, entity, "GrazeableComponent");
        writer.Component<C::WaterHoleComponent>(registry, entity, "WaterHoleComponent");
        writer.Component<C::CombustibleComponent>(registry, entity, "CombustibleComponent");
        writer.Component<C::IgnitionSourceComponent>(registry, entity, "IgnitionSourceComponent");
        writer.Component<C::WaterSourceComponent>(registry, entity, "WaterSourceComponent");
        writer.Component<C::WaterFlowSourceComponent>(registry, entity, "WaterFlowSourceComponent");
        writer.Tag<C::InstinctAgent>(registry, entity, "InstinctAgent");
        writer.Tag<C::PendingDeletion>(registry, entity, "PendingDeletion");
    }
    return std::move(writer.bytes);
}
std::string Plants(const entt::registry& registry) {
    std::map<std::uint64_t, entt::entity> ordered;
    for (const auto entity : registry.view<const C::PlantTag>()) {
        const auto* owner = registry.try_get<C::PlantRegionComponent>(entity);
        if (!owner || !owner->persistent_id ||
            !ordered.emplace(owner->persistent_id, entity).second)
            throw std::runtime_error("Invalid Heavy plant identity");
    }
    Writer writer(&registry);
    writer.Put(static_cast<std::uint64_t>(ordered.size()));
    for (const auto& [id, entity] : ordered) {
        writer.Put(id);
        const auto* transform = registry.try_get<C::TransformComponent>(entity);
        writer.Put(transform != nullptr);
        // Plant orientation and mesh scale are presentation; position is simulation authority.
        if (transform)
            writer.Put(transform->position);
        writer.Component<C::PlantRegionComponent>(registry, entity, "PlantRegionComponent");
        writer.Component<C::PlantGenomeComponent>(registry, entity, "PlantGenomeComponent");
        writer.Component<C::PlantGrowthComponent>(registry, entity, "PlantGrowthComponent");
        writer.Component<C::SoilFeederComponent>(registry, entity, "SoilFeederComponent");
        writer.Component<C::PlantHealthComponent>(registry, entity, "PlantHealthComponent");
        writer.Component<C::PollinationComponent>(registry, entity, "PollinationComponent");
        writer.Component<C::CropLifecycleComponent>(registry, entity, "CropLifecycleComponent");
        writer.Component<C::MortalComponent>(registry, entity, "MortalComponent");
        writer.Component<C::GrazeableComponent>(registry, entity, "GrazeableComponent");
        writer.Component<C::CombustibleComponent>(registry, entity, "CombustibleComponent");
        writer.Component<C::IgnitionSourceComponent>(registry, entity, "IgnitionSourceComponent");
        writer.Component<C::WaterSourceComponent>(registry, entity, "WaterSourceComponent");
        writer.Component<C::WaterFlowSourceComponent>(registry, entity, "WaterFlowSourceComponent");
        writer.Tag<C::PollinationTag>(registry, entity, "PollinationTag");
    }
    return std::move(writer.bytes);
}

// Offline bounds are checked before allocating file, record, JSON-container or field storage.
// A limit refusal aborts Heavy; it never supplies an empty or partial observation.
constexpr std::uint64_t kReferenceFnvOffset = 14695981039346656037ull;

void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Allocation-free lexical budget pass. JSON syntax and duplicate keys are checked afterwards.
// Count commas as well as container starts: each collection is bounded before DOM allocation,
// including arrays of tiny scalars and objects whose values are themselves empty containers.
void CheckJsonBudget(std::string_view bytes) {
    Require(bytes.size() <= HeavyReferenceDecodeLimits::kRecordBytes, "Heavy JSON byte limit");
    std::array<std::size_t, HeavyReferenceDecodeLimits::kJsonDepth> separators{};
    std::array<char, HeavyReferenceDecodeLimits::kJsonDepth> containers{};
    std::size_t depth = 0, nodes = 0;
    bool quoted = false, escaped = false, atom = false;
    for (const char byte : bytes) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (byte == '\\')
                escaped = true;
            else if (byte == '"')
                quoted = false;
            continue;
        }
        if (byte == '"') {
            quoted = true;
            atom = false;
            Require(++nodes <= HeavyReferenceDecodeLimits::kJsonNodes, "Heavy JSON node limit");
        } else if (byte == '[' || byte == '{') {
            atom = false;
            Require(depth < HeavyReferenceDecodeLimits::kJsonDepth, "Heavy JSON nesting limit");
            containers[depth] = byte;
            separators[depth++] = 0;
            Require(++nodes <= HeavyReferenceDecodeLimits::kJsonNodes, "Heavy JSON node limit");
        } else if (byte == ']' || byte == '}') {
            atom = false;
            Require(depth != 0, "Heavy JSON unmatched close");
            Require(containers[depth - 1] == (byte == ']' ? '[' : '{'),
                    "Heavy JSON mismatched close");
            --depth;
        } else if (byte == ',') {
            atom = false;
            Require(depth != 0, "Heavy JSON separator outside container");
            Require(++separators[depth - 1] < HeavyReferenceDecodeLimits::kCollection,
                    "Heavy JSON collection limit");
        } else if (byte == ':' || byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            atom = false;
        } else if (!atom) {
            atom = true;
            Require(++nodes <= HeavyReferenceDecodeLimits::kJsonNodes, "Heavy JSON node limit");
        }
    }
    Require(!quoted && depth == 0, "Heavy JSON truncated container or string");
}

nlohmann::json ReferenceJson(std::string_view bytes) {
    CheckJsonBudget(bytes);
    std::array<std::set<std::string>, HeavyReferenceDecodeLimits::kJsonDepth + 1> keys;
    const auto callback =
        [&keys](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
            Require(depth >= 0 &&
                        static_cast<std::size_t>(depth) <= HeavyReferenceDecodeLimits::kJsonDepth,
                    "Heavy JSON parser depth");
            const auto level = static_cast<std::size_t>(depth);
            if (event == nlohmann::json::parse_event_t::object_start) {
                Require(level < HeavyReferenceDecodeLimits::kJsonDepth, "Heavy JSON object depth");
                keys[level + 1].clear();
            } else if (event == nlohmann::json::parse_event_t::key) {
                Require(keys[level].insert(value.get<std::string>()).second,
                        "Heavy JSON duplicate key");
            }
            return true;
        };
    try {
        return nlohmann::json::parse(bytes.begin(), bytes.end(), callback);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Heavy malformed durable JSON");
    }
}

void ReadReferenceBytes(Filesystem::RegularFileHandle& file,
                        char* destination,
                        std::size_t count,
                        const char* failure = "Heavy truncated LMR payload") {
    // Fixed 64 KiB read requests also fit the Windows DWORD API on every platform.
    std::size_t completed = 0;
    while (completed < count) {
        const auto requested = std::min(count - completed, std::size_t{64u * 1024u});
        const auto received = file.read(destination + completed, requested);
        Require(received != 0, failure);
        completed += received;
    }
}

bool ReferenceFileExists(const std::filesystem::path& path) {
    const auto status = std::filesystem::symlink_status(path);
    if (status.type() == std::filesystem::file_type::not_found)
        return false;
    Require(std::filesystem::is_regular_file(status), "Heavy invalid durable member type");
    return true;
}

std::string ReadBounded(const std::filesystem::path& path, std::size_t limit) {
    Filesystem::RegularFileHandle input(path);
    Require(input.size() <= limit, "Heavy file byte limit");
    std::string bytes(static_cast<std::size_t>(input.size()), '\0');
    ReadReferenceBytes(
        input, bytes.data(), bytes.size(), "Heavy durable member changed or truncated");
    char probe = 0;
    Require(input.read(&probe, 1) == 0, "Heavy durable member changed or truncated");
    return bytes;
}

std::uint64_t ReferenceUnsigned(const nlohmann::json& value) {
    Require(value.is_number_unsigned(), "Heavy expected unsigned integer");
    return value.get<std::uint64_t>();
}

template<class T>
T ReferenceInteger(const nlohmann::json& value) {
    static_assert(std::is_integral_v<T>);
    Require(value.is_number_integer(), "Heavy expected integer");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        Require(number <= static_cast<std::uint64_t>(std::numeric_limits<T>::max()),
                "Heavy integer overflow");
        return static_cast<T>(number);
    }
    const auto number = value.get<std::int64_t>();
    Require(number >= static_cast<std::int64_t>(std::numeric_limits<T>::lowest()) &&
                number <= static_cast<std::int64_t>(std::numeric_limits<T>::max()),
            "Heavy integer overflow");
    return static_cast<T>(number);
}

float ReferenceFloat(const nlohmann::json& value) {
    Require(value.is_number(), "Heavy expected scalar");
    const auto number = value.get<double>();
    Require(std::isfinite(number) && std::abs(number) <= std::numeric_limits<float>::max(),
            "Heavy nonfinite or overflowing scalar");
    return static_cast<float>(number);
}

template<class T>
std::vector<T> ReferenceArray(const nlohmann::json& value, std::size_t limit) {
    Require(value.is_array() && value.size() <= limit, "Heavy field collection limit");
    std::vector<T> result;
    result.reserve(value.size());
    for (const auto& scalar : value) {
        if constexpr (std::is_floating_point_v<T>)
            result.push_back(ReferenceFloat(scalar));
        else
            result.push_back(ReferenceInteger<T>(scalar));
    }
    return result;
}

int ReferenceFloorRegion(int coordinate) {
    constexpr int span = 32;
    return coordinate / span - (coordinate % span < 0 ? 1 : 0);
}

bool ReferenceRegionPath(std::string_view path, int& region_x, int& region_z) {
    constexpr std::string_view prefix = "chunks/region/r.";
    constexpr std::string_view suffix = ".lmr";
    if (path.size() <= prefix.size() + suffix.size() || !path.starts_with(prefix) ||
        !path.ends_with(suffix))
        return false;
    const auto coordinates =
        path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    const auto dot = coordinates.find('.');
    if (dot == std::string_view::npos)
        return false;
    const auto x = std::from_chars(coordinates.data(), coordinates.data() + dot, region_x);
    const auto z = std::from_chars(
        coordinates.data() + dot + 1, coordinates.data() + coordinates.size(), region_z);
    return x.ec == std::errc{} && x.ptr == coordinates.data() + dot && z.ec == std::errc{} &&
           z.ptr == coordinates.data() + coordinates.size() &&
           path == std::string(prefix) + std::to_string(region_x) + "." + std::to_string(region_z) +
                       std::string(suffix);
}

constexpr std::array<std::string_view, 9> kReferenceSingletons{
    "world_info.json",
    "aether_state.efs",
    "chunks/region/world-manifest.json",
    "chunks/region/plant-entities.json",
    "chunks/region/creature-entities.json",
    "chunks/region/ground-objects.json",
    "chunks/region/field-pages.fpg",
    "chunks/region/active-regions.arl",
    "chunks/region/simulation-pending.spn"};

struct ReferenceMember {
    bool present = false;
    std::uint64_t bytes = 0;
    std::uint64_t hash = 0;
};

std::map<std::string, ReferenceMember> ReferenceIndex(const std::filesystem::path& directory) {
    const auto document = ReferenceJson(ReadBounded(directory / "chunks/region/snapshot.json",
                                                    HeavyReferenceDecodeLimits::kIndexBytes));
    Require(document.at("schema") == "luminumbra.snapshot.v1" &&
                ReferenceUnsigned(document.at("generation")) > 0,
            "Heavy unsupported snapshot index");
    (void)ReferenceUnsigned(document.at("tick"));
    (void)ReferenceUnsigned(document.at("txn_id"));
    const auto& anchors = document.at("anchors");
    Require(anchors.is_array(), "Heavy invalid snapshot anchors");
    for (const auto& anchor : anchors) {
        Require(anchor.is_array() && anchor.size() == 3, "Heavy invalid snapshot anchor");
        for (const auto& scalar : anchor)
            (void)ReferenceFloat(scalar);
    }
    const auto& members = document.at("members");
    Require(members.is_array() && members.size() <= HeavyReferenceDecodeLimits::kMembers,
            "Heavy snapshot member limit");
    std::map<std::string, ReferenceMember> result;
    std::string previous;
    for (const auto& item : members) {
        const auto name = item.at("path").get<std::string>();
        int region_x = 0, region_z = 0;
        const bool region = ReferenceRegionPath(name, region_x, region_z);
        Require(region ||
                    std::find(kReferenceSingletons.begin(), kReferenceSingletons.end(), name) !=
                        kReferenceSingletons.end(),
                "Heavy invalid snapshot path");
        Require(previous.empty() || previous < name, "Heavy unordered snapshot members");
        previous = name;
        Require(item.at("class") == "authoritative" || item.at("class") == "derived",
                "Heavy invalid snapshot member class");
        Require(item.at("state") == "present" || item.at("state") == "absent",
                "Heavy invalid snapshot member state");
        (void)ReferenceUnsigned(item.at("generation"));
        const auto hex = item.at("fnv64").get<std::string>();
        std::uint64_t hash = 0;
        const auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), hash, 16);
        Require(hex.size() == 16 && parsed.ec == std::errc{} &&
                    parsed.ptr == hex.data() + hex.size(),
                "Heavy invalid snapshot checksum");
        result.emplace(name,
                       ReferenceMember{item.at("state") == "present",
                                       ReferenceUnsigned(item.at("bytes")),
                                       hash});
    }
    return result;
}

void ReferenceFold(std::uint64_t& hash, std::string_view bytes) {
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
}

std::uint64_t ReadLittle(std::string_view bytes, std::size_t offset, std::size_t width) {
    Require(width <= sizeof(std::uint64_t) && offset <= bytes.size() &&
                width <= bytes.size() - offset,
            "Heavy truncated LMR header");
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index)
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index]))
                 << (index * 8);
    return value;
}

std::shared_ptr<Chunk>
ReferenceChunk(std::string_view bytes, ChunkID id, int region_x, int region_z) {
    const auto snapshot = ReferenceJson(bytes);
    Require(snapshot.at("schema") == "luminumbra.persistence.world_state_snapshot.v1" &&
                snapshot.at("order_contract") == "chunk_id_ascending" &&
                ReferenceUnsigned(snapshot.at("chunk_count")) == 1,
            "Heavy invalid chunk snapshot");
    const auto& chunks = snapshot.at("chunks");
    Require(chunks.is_array() && chunks.size() == 1, "Heavy invalid chunk record count");
    const auto& record = chunks.at(0);
    const auto& coordinates = record.at("coords");
    const IVec3 coords{ReferenceInteger<int>(coordinates.at("x")),
                       ReferenceInteger<int>(coordinates.at("y")),
                       ReferenceInteger<int>(coordinates.at("z"))};
    constexpr int minimum_coordinate = -(1 << 20), maximum_coordinate = 1 << 20;
    Require(coords.x >= minimum_coordinate && coords.x < maximum_coordinate &&
                coords.y >= 2 * minimum_coordinate && coords.y < 2 * maximum_coordinate &&
                coords.z >= minimum_coordinate && coords.z < maximum_coordinate &&
                ReferenceFloorRegion(coords.x) == region_x &&
                ReferenceFloorRegion(coords.z) == region_z,
            "Heavy invalid chunk coordinates");
    Require(Chunk::calculate_id(coords) == id && ReferenceUnsigned(record.at("chunk_id")) == id,
            "Heavy chunk identity mismatch");
    auto chunk = std::make_shared<Chunk>(coords);
    constexpr std::size_t lattice =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    chunk->sdf_data = ReferenceArray<float>(record.at("sdf_data"), lattice);
    Require(chunk->sdf_data.empty() || chunk->sdf_data.size() == lattice,
            "Heavy invalid SDF lattice");
    chunk->heightmap_data = ReferenceArray<float>(record.at("heightmap_data"), lattice);
    chunk->material_data = record.contains("material_data")
                               ? ReferenceArray<u8>(record.at("material_data"), lattice)
                               : std::vector<u8>{};
    constexpr std::size_t max_water_cells = 32u * 32u;
    chunk->water_depth_mm =
        ReferenceArray<std::int32_t>(record.at("water_depth_mm"), max_water_cells);
    chunk->water_bed_mm = ReferenceArray<std::int32_t>(record.at("water_bed_mm"), max_water_cells);
    chunk->water_edge_flux =
        ReferenceArray<std::int32_t>(record.at("water_edge_flux"), 2 * max_water_cells);
    const auto& water = record.at("water_state");
    chunk->has_water_sim.store(water.at("has_water_sim").get<bool>());
    chunk->current_water_resolution.store(
        ReferenceInteger<int>(water.at("current_water_resolution")));
    chunk->is_water_sleeping.store(water.at("is_water_sleeping").get<bool>());
    chunk->ticks_below_threshold = ReferenceInteger<int>(water.at("ticks_below_threshold"));
    chunk->max_water_delta_last_tick = ReferenceFloat(water.at("max_water_delta_last_tick"));
    const int resolution = chunk->current_water_resolution.load();
    Require(resolution == 4 || resolution == 8 || resolution == 16 || resolution == 32,
            "Heavy invalid water resolution");
    const auto cells = static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
    Require(!chunk->has_water_sim.load() ||
                (chunk->water_depth_mm.size() == cells && chunk->water_bed_mm.size() == cells &&
                 chunk->water_edge_flux.size() == 2 * cells),
            "Heavy incomplete water authority");
    Require(chunk->has_water_sim.load() ||
                (chunk->water_depth_mm.empty() && chunk->water_bed_mm.empty() &&
                 chunk->water_edge_flux.empty()),
            "Heavy uninitialized water storage");
    for (const auto depth : chunk->water_depth_mm)
        Require(depth >= 0, "Heavy negative water depth");
    Require(chunk->ticks_below_threshold >= 0 && chunk->max_water_delta_last_tick >= 0,
            "Heavy invalid water continuation");
    chunk->mark_sdf_loaded_or_edited();
    chunk->set_state(ChunkState::Idle);
    return chunk;
}

ReferenceMember ReadReferenceRegion(const std::filesystem::path& path,
                                    int region_x,
                                    int region_z,
                                    std::map<ChunkID, std::shared_ptr<Chunk>>& disk,
                                    std::uint64_t& retained_bytes) {
    Filesystem::RegularFileHandle input(path);
    const auto file_bytes = input.size();
    Require(file_bytes >= 8 && file_bytes <= HeavyReferenceDecodeLimits::kRegionBytes,
            "Heavy LMR byte limit");
    std::string header(8, '\0');
    ReadReferenceBytes(input, header.data(), header.size());
    Require(header.substr(0, 4) == "LMR1" && ReadLittle(header, 4, 2) == 2,
            "Heavy invalid LMR header");
    const auto count = static_cast<std::size_t>(ReadLittle(header, 6, 2));
    constexpr std::size_t record_header_bytes = 18;
    Require(count <= HeavyReferenceDecodeLimits::kRegionRecords &&
                count <= (file_bytes - 8) / record_header_bytes,
            "Heavy truncated LMR manifest");
    std::string manifest(count * record_header_bytes, '\0');
    ReadReferenceBytes(input, manifest.data(), manifest.size());
    struct Record {
        std::uint64_t offset;
        std::uint32_t raw_bytes, compressed_bytes;
        std::uint8_t flags;
    };
    std::map<std::pair<std::uint8_t, ChunkID>, Record> records;
    std::uint64_t offset = 8 + manifest.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto start = index * record_header_bytes;
        const auto id = ReadLittle(manifest, start, 8);
        const auto lod = static_cast<std::uint8_t>(ReadLittle(manifest, start + 8, 1));
        const auto flags = static_cast<std::uint8_t>(ReadLittle(manifest, start + 9, 1));
        const auto raw = static_cast<std::uint32_t>(ReadLittle(manifest, start + 10, 4));
        const auto compressed = static_cast<std::uint32_t>(ReadLittle(manifest, start + 14, 4));
        Require(lod <= 2 && (flags & ~3u) == 0 && raw > 0 &&
                    raw <= HeavyReferenceDecodeLimits::kRecordBytes && compressed > 0 &&
                    compressed <= HeavyReferenceDecodeLimits::kRecordBytes &&
                    compressed <= file_bytes - offset,
                "Heavy invalid LMR record bounds");
        Require(records.emplace(std::pair{lod, id}, Record{offset, raw, compressed, flags}).second,
                "Heavy duplicate LMR record");
        offset += compressed;
    }
    Require(offset == file_bytes, "Heavy trailing LMR bytes");
    ReferenceMember fingerprint;
    for (const auto& [key, record] : records) {
        if (key.first != 0)
            continue; // Far geometry is derived and is outside the authoritative lod-0 fingerprint.
        if ((record.flags & 1u) != 0) {
            Require(disk.size() < HeavyReferenceDecodeLimits::kCollection &&
                        record.raw_bytes <=
                            HeavyReferenceDecodeLimits::kRetainedChunkBytes - retained_bytes,
                    "Heavy retained chunk storage limit");
            retained_bytes += record.raw_bytes;
        }
        input.seek(record.offset);
        std::string compressed(record.compressed_bytes, '\0');
        ReadReferenceBytes(input, compressed.data(), compressed.size());
        if (!fingerprint.present) {
            fingerprint.present = true;
            fingerprint.hash = kReferenceFnvOffset;
        }
        Writer framed;
        framed.Fields(key.second, record.flags, record.raw_bytes, record.compressed_bytes);
        ReferenceFold(fingerprint.hash, framed.bytes);
        ReferenceFold(fingerprint.hash, compressed);
        fingerprint.bytes += framed.bytes.size() + compressed.size();
        std::string payload(record.raw_bytes, '\0');
        const int produced = LZ4_decompress_safe(compressed.data(),
                                                 payload.data(),
                                                 static_cast<int>(record.compressed_bytes),
                                                 static_cast<int>(record.raw_bytes));
        Require(produced >= 0 && static_cast<std::uint32_t>(produced) == record.raw_bytes,
                "Heavy invalid LZ4 record");
        const auto chunk = ReferenceChunk(payload, key.second, region_x, region_z);
        if ((record.flags & 1u) != 0)
            Require(disk.emplace(key.second, chunk).second, "Heavy duplicate durable chunk");
    }
    // Validate the tail even when only derived LOD records occupy it, then probe growth.
    input.seek(file_bytes - 1);
    char tail = 0;
    ReadReferenceBytes(input, &tail, 1);
    Require(input.read(&tail, 1) == 0, "Heavy LMR grew during observation");
    return fingerprint;
}

void MatchReferenceMember(const ReferenceMember& expected, const ReferenceMember& actual) {
    Require(
        expected.present == actual.present &&
            (!actual.present || (expected.bytes == actual.bytes && expected.hash == actual.hash)),
        "Heavy committed member mismatch");
}

ReferenceMember ReferenceFileFingerprint(const std::filesystem::path& path) {
    ReferenceMember result;
    if (!ReferenceFileExists(path))
        return result;
    Filesystem::RegularFileHandle input(path);
    Require(input.size() <= HeavyReferenceDecodeLimits::kRecordBytes, "Heavy member byte limit");
    result = {true, input.size(), kReferenceFnvOffset};
    std::array<char, 64u * 1024u> buffer{};
    std::uint64_t remaining = result.bytes;
    while (remaining > 0) {
        const auto take =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        ReadReferenceBytes(input, buffer.data(), take, "Heavy truncated indexed member");
        ReferenceFold(result.hash, std::string_view(buffer.data(), take));
        remaining -= take;
    }
    char probe = 0;
    Require(input.read(&probe, 1) == 0, "Heavy indexed member grew");
    return result;
}

std::map<ChunkID, std::shared_ptr<Chunk>> ReferenceDisk(const std::filesystem::path& directory) {
    std::map<ChunkID, std::shared_ptr<Chunk>> disk;
    if (directory.empty())
        return disk;
    Require(!std::filesystem::exists(std::filesystem::symlink_status(directory / "journal")),
            "Heavy requires committed authority");
    const bool indexed = ReferenceFileExists(directory / "chunks/region/snapshot.json");
    const auto members =
        indexed ? ReferenceIndex(directory) : std::map<std::string, ReferenceMember>{};
    std::map<std::string, ReferenceMember> actual;
    std::set<std::string> region_paths;
    const auto region_directory = directory / "chunks/region";
    if (std::filesystem::exists(region_directory)) {
        for (const auto& entry : std::filesystem::directory_iterator(region_directory)) {
            if (entry.path().extension() != ".lmr")
                continue;
            Require(region_paths.size() < HeavyReferenceDecodeLimits::kMembers,
                    "Heavy region member limit");
            region_paths.insert("chunks/region/" +
                                Filesystem::GenericPathString(entry.path().filename()));
        }
    }
    std::uint64_t retained_bytes = 0;
    for (const auto& name : region_paths) {
        int region_x = 0, region_z = 0;
        Require(ReferenceRegionPath(name, region_x, region_z), "Heavy invalid region filename");
        actual.emplace(
            name, ReadReferenceRegion(directory / name, region_x, region_z, disk, retained_bytes));
    }
    if (!indexed) {
        Require(region_paths.empty(), "Heavy durable authority requires a committed index");
        return disk;
    }
    for (const auto name : kReferenceSingletons)
        actual.emplace(name, ReferenceFileFingerprint(directory / name));
    for (const auto& [name, expected] : members) {
        const auto found = actual.find(name);
        MatchReferenceMember(expected, found == actual.end() ? ReferenceMember{} : found->second);
    }
    for (const auto& [name, member] : actual)
        Require(!member.present || members.contains(name), "Heavy unindexed authority member");
    return disk;
}

std::string Chunks(const HeavyReferenceInputs& inputs) {
    std::map<ChunkID, std::shared_ptr<Chunk>> disk;
    try {
        disk = ReferenceDisk(inputs.directory);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Heavy invalid durable record fields");
    }
    if (!inputs.chunks.durable.empty() &&
        (inputs.directory.empty() ||
         !ReferenceFileExists(inputs.directory / "chunks/region/snapshot.json")))
        throw std::runtime_error("Heavy durable inventory requires a snapshot index");
    for (const auto& coords : inputs.chunks.durable) {
        const auto found = disk.find(Chunk::calculate_id(coords));
        if (found == disk.end() || found->second->get_coords() != coords)
            throw std::runtime_error("Heavy expected durable record is missing");
    }
    std::map<ChunkID, std::shared_ptr<const Chunk>> selected;
    const auto select = [&selected](const auto& candidates) {
        for (const auto& chunk : candidates) {
            if (!chunk || chunk->get_state() == ChunkState::Loading)
                continue;
            if (chunk->sdf_provenance() != ChunkSdfProvenance::LoadedOrEdited &&
                !chunk->has_water_authority() && !chunk->is_water_state_dirty())
                continue;
            selected.emplace(chunk->get_id(), chunk);
        }
    };
    select(inputs.chunks.live);
    select(inputs.chunks.parked);
    selected.insert(disk.begin(), disk.end());
    Writer writer;
    std::map<W::RegionKey, std::map<ChunkID, std::shared_ptr<const Chunk>>> owners;
    const auto floor_region = [](int coordinate) {
        constexpr int span = Persistence::WorldSaveService::kRegionChunkSpan;
        return coordinate / span - (coordinate % span < 0 ? 1 : 0);
    };
    for (const auto& [id, chunk] : selected) {
        const auto coords = chunk->get_coords();
        owners[{floor_region(coords.x), floor_region(coords.z)}].emplace(id, chunk);
    }
    writer.Put(static_cast<std::uint64_t>(owners.size()));
    for (const auto& [owner, chunks] : owners) {
        writer.Fields(owner, static_cast<std::uint64_t>(chunks.size()));
        for (const auto& [id, chunk] : chunks) {
            writer.Fields(id,
                          chunk->get_coords(),
                          chunk->sdf_data,
                          chunk->heightmap_data,
                          chunk->material_data,
                          chunk->water_depth_mm,
                          chunk->water_bed_mm,
                          chunk->water_edge_flux,
                          chunk->has_water_sim.load(),
                          chunk->current_water_resolution.load(),
                          chunk->is_water_sleeping.load(),
                          chunk->ticks_below_threshold,
                          chunk->max_water_delta_last_tick);
        }
    }
    return std::move(writer.bytes);
}
std::string Ledger(const W::ActiveRegionLedger& ledger) {
    Writer writer;
    writer.Fields(ledger.tick(),
                  ledger.config(),
                  ledger.local_anchor(),
                  ledger.next_persistent_id(),
                  ledger.content_identity(),
                  ledger.records(),
                  ledger.water_debts(),
                  ledger.energy_ages(),
                  ledger.completed_work(),
                  ledger.activation_intents(),
                  ledger.pending_activations());
    return std::move(writer.bytes);
}
std::string Pages(const W::FieldPageState& pages) {
    Writer writer;
    writer.Fields(pages.pages(),
                  pages.storms(),
                  pages.strikes(),
                  pages.scent_cells(),
                  pages.soil().pages(),
                  pages.irrigation().pages(),
                  pages.weather_clocks());
    return std::move(writer.bytes);
}
std::string Energy(const luminumbra::fields::EnergyFieldState* energy) {
    Writer writer;
    // Absent and an empty single-channel layer describe the same stored values.
    writer.Put(energy ? energy->channels() : 1);
    std::uint64_t count = 0;
    if (energy)
        energy->VisitStoredRegionalPages([&count](auto, auto) { ++count; });
    writer.Put(count);
    if (energy) {
        energy->VisitStoredRegionalPages(
            [&writer](auto key, auto values) { writer.Fields(key, values); });
        auto deposits = energy->PendingDeposits();
        std::sort(deposits.begin(), deposits.end(), [](const auto& left, const auto& right) {
            return std::tie(left.cell_key, left.channel, left.emitter_id, left.amount) <
                   std::tie(right.cell_key, right.channel, right.emitter_id, right.amount);
        });
        writer.Put(deposits);
    } else {
        writer.Put(std::uint64_t{0});
    }
    return std::move(writer.bytes);
}
} // namespace

std::string HeavyAuthorityReference::Difference(const HeavyAuthorityReference& other) const {
    for (const auto& [name, bytes] : domains) {
        const auto found = other.domains.find(name);
        if (found == other.domains.end() || found->second != bytes)
            return name;
    }
    for (const auto& [name, bytes] : other.domains) {
        (void)bytes;
        if (!domains.contains(name))
            return name;
    }
    return {};
}
HeavyAuthorityReference HeavyAuthorityReference::Capture(const HeavyReferenceInputs& inputs) {
    HeavyAuthorityReference result;
    auto& domains = result.domains;
    domains["chunks"] = Chunks(inputs);
    domains["creatures"] = Creatures(inputs.registry);
    domains["plants"] = Plants(inputs.registry);
    domains["regions"] = Ledger(inputs.ledger);
    domains["pages"] = Pages(inputs.fields);
    domains["energy"] = Energy(inputs.energy);
    Writer clock;
    clock.Fields(inputs.clock.tick(),
                 inputs.clock.calendar().dayLengthTicks,
                 inputs.clock.calendar().daysPerYear);
    clock.Put(inputs.ambient_anchor);
    domains["clock"] = std::move(clock.bytes);
    Writer items;
    items.Put(inputs.ground.objects());
    domains["items"] = std::move(items.bytes);
    Writer events;
    const auto snapshot = inputs.events ? inputs.events->snapshot()
                                        : luminumbra::simulation::OrderedEventBus::Snapshot{};
    events.Fields(snapshot.next_sequence, snapshot.events);
    domains["pending"] = std::move(events.bytes);
    std::map<std::uint32_t, World::PlayerAvatar> avatars;
    for (const auto& avatar : inputs.avatars)
        if (!avatars.emplace(avatar.player_id, avatar).second)
            throw std::runtime_error("Duplicate Heavy avatar identity");
    Writer avatar_fields;
    avatar_fields.Put(avatars);
    domains["avatars"] = std::move(avatar_fields.bytes);
    return result;
}
HeavyAuthorityReference
HeavyAuthorityReference::Capture(const world::GameSession& session,
                                 std::span<const World::PlayerAvatar> avatars) {
    const auto observation = session.ObserveSimulationAuthority();
    if (!session.ActiveRegionsEnabled() || !session.IsSimulationTickBoundary() ||
        session.GetRegionSchedulerFault())
        throw std::runtime_error("Heavy requires a healthy enabled simulation boundary");
    const auto world_authority = session.ObserveWorldAuthority();
    if (!world_authority)
        throw std::runtime_error("Heavy requires world authority");
    auto result = Capture(HeavyReferenceInputs{.directory = observation.durable_directory.empty()
                                                                ? session.GetWorldSaveDir()
                                                                : observation.durable_directory,
                                               .chunks = {.live = world_authority->live,
                                                          .parked = world_authority->parked,
                                                          .durable = world_authority->durable},
                                               .registry = session.GetRegistry(),
                                               .clock = session.GetWorldClock(),
                                               .ledger = session.GetActiveRegionLedger(),
                                               .ground = session.GetGroundObjects(),
                                               .fields = session.GetFieldPages(),
                                               .energy = session.GetEnergyFieldState(),
                                               .events = &session.GetSimulationEventBus(),
                                               .avatars = avatars,
                                               .ambient_anchor = observation.ambient_anchor});
    Writer configuration;
    configuration.Fields(session.GetMetadata().seed,
                         world_authority->seed,
                         world_authority->params,
                         world_authority->biomes_enabled,
                         world_authority->biomes,
                         world_authority->structures_enabled,
                         world_authority->structure_pools,
                         observation.population_contract,
                         observation.population_configuration,
                         observation.population.has_value(),
                         session.ActiveRegionsEnabled(),
                         session.AetherStateEnabled(),
                         observation.water_high_res,
                         observation.weather_rain,
                         observation.weather_rain_scale_mm,
                         observation.weather_events,
                         observation.ecology,
                         observation.wildlife_foliage,
                         observation.thirst,
                         observation.scavenging,
                         observation.reproduction,
                         observation.foraging,
                         observation.circadian_amplitude,
                         observation.plant_mutation_rate);
    if (observation.population)
        configuration.Fields(observation.population->contract,
                             observation.population->configuration,
                             observation.population->ground_types);
    if (!session.GetSimulationContent())
        throw std::runtime_error("Heavy requires admitted simulation content");
    const auto& content = *session.GetSimulationContent();
    configuration.Fields(content.identity,
                         content.creature_definitions,
                         content.plant_definitions,
                         content.policy.defaults_version,
                         content.policy.creatures,
                         content.policy.plants,
                         content.policy.plant_ids);
    result.domains["identity"] = std::move(configuration.bytes);
    return result;
}
} // namespace Luminumbra::Server
