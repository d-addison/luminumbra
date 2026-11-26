#include <gtest/gtest.h>

#include "ai/EcologyHash.h"
#include "simulation/ParticipantProjection.h"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {
namespace C = Luminumbra::Components;
namespace S = Luminumbra::simulation;

// Independent member-pointer oracle: these lists deliberately do not use the
// production field visitor. Dropping any projected field must fail its mutation.
template<typename T>
struct TestFields;
template<typename Value, typename Visitor, typename... MemberPointers>
void Members(Value& value, const Visitor& visitor, MemberPointers... members) {
    (visitor(value.*members), ...);
}
template<>
struct TestFields<C::TransformComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::TransformComponent::position);
    }
};
template<>
struct TestFields<C::PlantTag> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        (void)value;
        (void)visitor;
    }
};
template<>
struct TestFields<C::PlantGenomeComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::PlantGenomeComponent::genes);
    }
};
template<>
struct TestFields<C::PlantGrowthComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::PlantGrowthComponent::species_id,
                &C::PlantGrowthComponent::stage,
                &C::PlantGrowthComponent::quality,
                &C::PlantGrowthComponent::growth_points,
                &C::PlantGrowthComponent::stress_points,
                &C::PlantGrowthComponent::tended,
                &C::PlantGrowthComponent::planted_tick,
                &C::PlantGrowthComponent::last_tick);
    }
};
template<>
struct TestFields<C::SoilFeederComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::SoilFeederComponent::uptake, &C::SoilFeederComponent::absorbed);
    }
};
template<>
struct TestFields<C::PlantHealthComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::PlantHealthComponent::infection,
                &C::PlantHealthComponent::resistance,
                &C::PlantHealthComponent::state,
                &C::PlantHealthComponent::infected_ticks);
    }
};
template<>
struct TestFields<C::PollinationComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::PollinationComponent::pollinated,
                &C::PollinationComponent::last_pollen_tick,
                &C::PollinationComponent::crosses,
                &C::PollinationComponent::next_genome);
    }
};
template<>
struct TestFields<C::CropLifecycleComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::CropLifecycleComponent::perennial,
                &C::CropLifecycleComponent::ripe_ticks,
                &C::CropLifecycleComponent::lifespan_ticks,
                &C::CropLifecycleComponent::generations,
                &C::CropLifecycleComponent::species_id);
    }
};
template<>
struct TestFields<C::PlantRegionComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::PlantRegionComponent::owner_x,
                &C::PlantRegionComponent::owner_z,
                &C::PlantRegionComponent::region_stamp);
    }
};
template<>
struct TestFields<C::PollinationTag> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        (void)value;
        (void)visitor;
    }
};
template<>
struct TestFields<C::CombustibleComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::CombustibleComponent::fuel_milli,
                &C::CombustibleComponent::moisture_milli,
                &C::CombustibleComponent::burn_state,
                &C::CombustibleComponent::burn_ticks_remaining,
                &C::CombustibleComponent::ignition_radius);
    }
};
template<>
struct TestFields<C::MortalComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::MortalComponent::age_ticks,
                &C::MortalComponent::lifespan_ticks,
                &C::MortalComponent::dead);
    }
};
template<>
struct TestFields<C::GrazeableComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::GrazeableComponent::biomass,
                &C::GrazeableComponent::last_grazed_tick);
    }
};
template<>
struct TestFields<C::WaterSourceComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::WaterSourceComponent::output);
    }
};
template<>
struct TestFields<C::IgnitionSourceComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::IgnitionSourceComponent::radius,
                &C::IgnitionSourceComponent::intensity);
    }
};
template<>
struct TestFields<C::FlowingWaterSourceComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::FlowingWaterSourceComponent::flow_rate);
    }
};
template<>
struct TestFields<C::DecayComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::DecayComponent::decay_ticks,
                &C::DecayComponent::decay_duration,
                &C::DecayComponent::nutrient_released_milli,
                &C::DecayComponent::fully_decomposed);
    }
};
template<>
struct TestFields<C::FieldEmitterComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::FieldEmitterComponent::channel,
                &C::FieldEmitterComponent::rate_raw_per_tick,
                &C::FieldEmitterComponent::radius_cells);
    }
};
template<>
struct TestFields<C::CreatureComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::CreatureComponent::is_predator,
                &C::CreatureComponent::species_id,
                &C::CreatureComponent::hunger,
                &C::CreatureComponent::stamina,
                &C::CreatureComponent::energy,
                &C::CreatureComponent::move_speed,
                &C::CreatureComponent::last_action,
                &C::CreatureComponent::wish_x,
                &C::CreatureComponent::wish_z,
                &C::CreatureComponent::eaten);
    }
};
template<>
struct TestFields<C::CreatureGenomeComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::CreatureGenomeComponent::move_speed,
                &C::CreatureGenomeComponent::vigilance,
                &C::CreatureGenomeComponent::hunger_threshold,
                &C::CreatureGenomeComponent::size_scale,
                &C::CreatureGenomeComponent::vision_cos_half_fov,
                &C::CreatureGenomeComponent::vision_range,
                &C::CreatureGenomeComponent::hearing_range,
                &C::CreatureGenomeComponent::age_ticks,
                &C::CreatureGenomeComponent::reproduce_cooldown,
                &C::CreatureGenomeComponent::generation,
                &C::CreatureGenomeComponent::female,
                &C::CreatureGenomeComponent::courting_ticks);
    }
};
template<>
struct TestFields<C::CreaturePhysicsComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        (void)value;
        (void)visitor;
    }
};
template<>
struct TestFields<C::ThirstComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::ThirstComponent::thirst,
                &C::ThirstComponent::wish_x,
                &C::ThirstComponent::wish_z,
                &C::ThirstComponent::drinking,
                &C::ThirstComponent::water_proximity);
    }
};
template<>
struct TestFields<C::WaterHoleComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::WaterHoleComponent::radius);
    }
};
template<>
struct TestFields<C::CircadianComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(
            value, visitor, &C::CircadianComponent::nocturnal, &C::CircadianComponent::activity);
    }
};
template<>
struct TestFields<C::AlarmComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::AlarmComponent::level, &C::AlarmComponent::alarmed);
    }
};
template<>
struct TestFields<C::PackHunterComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::PackHunterComponent::coord_x,
                &C::PackHunterComponent::coord_z,
                &C::PackHunterComponent::in_pack);
    }
};
template<>
struct TestFields<C::TerritoryComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::TerritoryComponent::home_x,
                &C::TerritoryComponent::home_z,
                &C::TerritoryComponent::radius,
                &C::TerritoryComponent::established);
    }
};
template<>
struct TestFields<C::TerritoryBiasComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(
            value, visitor, &C::TerritoryBiasComponent::wish_x, &C::TerritoryBiasComponent::wish_z);
    }
};
template<>
struct TestFields<C::MigratoryComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::MigratoryComponent::drive,
                &C::MigratoryComponent::wish_x,
                &C::MigratoryComponent::wish_z);
    }
};
template<>
struct TestFields<C::ScavengerComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::ScavengerComponent::wish_x,
                &C::ScavengerComponent::wish_z,
                &C::ScavengerComponent::feeding);
    }
};
template<>
struct TestFields<C::ForagerComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::ForagerComponent::cell_x,
                &C::ForagerComponent::cell_z,
                &C::ForagerComponent::home_x,
                &C::ForagerComponent::home_z,
                &C::ForagerComponent::carrying_food,
                &C::ForagerComponent::deliveries);
    }
};
template<>
struct TestFields<C::FoodSourceComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::FoodSourceComponent::cell_x,
                &C::FoodSourceComponent::cell_z,
                &C::FoodSourceComponent::amount);
    }
};
template<>
struct TestFields<C::NeedsComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::NeedsComponent::needs);
    }
};
template<>
struct TestFields<C::SensesComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::SensesComponent::nearest_food_source,
                &C::SensesComponent::nearest_water_source,
                &C::SensesComponent::nearest_threat,
                &C::SensesComponent::threat_distance);
    }
};
template<>
struct TestFields<C::OpportunityComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::OpportunityComponent::id,
                &C::OpportunityComponent::action,
                &C::OpportunityComponent::target,
                &C::OpportunityComponent::need,
                &C::OpportunityComponent::satisfaction,
                &C::OpportunityComponent::urgency,
                &C::OpportunityComponent::risk,
                &C::OpportunityComponent::stamina_cost,
                &C::OpportunityComponent::radius);
    }
};
template<>
struct TestFields<C::ActionPlanComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::ActionPlanComponent::plan,
                &C::ActionPlanComponent::current_action_index,
                &C::ActionPlanComponent::time_in_current_action);
    }
};
template<>
struct TestFields<C::InstinctAgent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        (void)value;
        (void)visitor;
    }
};
template<>
struct TestFields<C::LocomotionProfile> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::LocomotionProfile::move_speed,
                &C::LocomotionProfile::arrival_radius,
                &C::LocomotionProfile::slow_radius,
                &C::LocomotionProfile::separation_radius,
                &C::LocomotionProfile::separation_strength,
                &C::LocomotionProfile::flock_radius,
                &C::LocomotionProfile::cohesion_strength,
                &C::LocomotionProfile::alignment_strength);
    }
};
template<>
struct TestFields<C::SensableComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::SensableComponent::scent_channel,
                &C::SensableComponent::scent_deposit,
                &C::SensableComponent::noise_loudness,
                &C::SensableComponent::noise_pitch,
                &C::SensableComponent::faction);
    }
};
template<>
struct TestFields<C::PerceptionComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::PerceptionComponent::vision_cos_half_fov,
                &C::PerceptionComponent::vision_range,
                &C::PerceptionComponent::facing_x,
                &C::PerceptionComponent::facing_z,
                &C::PerceptionComponent::ear,
                &C::PerceptionComponent::faction);
    }
};
template<>
struct TestFields<C::AwarenessComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::AwarenessComponent::awareness, &C::AwarenessComponent::params);
    }
};
template<>
struct TestFields<C::LocomotionIntentComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::LocomotionIntentComponent::wish_xz,
                &C::LocomotionIntentComponent::arrived);
    }
};
template<>
struct TestFields<C::ScentSenseComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::ScentSenseComponent::channel,
                &C::ScentSenseComponent::sign,
                &C::ScentSenseComponent::strength,
                &C::ScentSenseComponent::floor,
                &C::ScentSenseComponent::weber_k);
    }
};
template<>
struct TestFields<C::LocomotionPathComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::LocomotionPathComponent::waypoints,
                &C::LocomotionPathComponent::index);
    }
};
template<>
struct TestFields<C::StimulusSubscriptionComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::StimulusSubscriptionComponent::subscriptions);
    }
};
template<>
struct TestFields<C::InstinctAgentComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::InstinctAgentComponent::actor_id,
                &C::InstinctAgentComponent::archetype,
                &C::InstinctAgentComponent::replan_interval_ticks,
                &C::InstinctAgentComponent::last_planned_tick,
                &C::InstinctAgentComponent::plans_executed,
                &C::InstinctAgentComponent::current_plan);
    }
};
template<>
struct TestFields<C::WaterInteractorComponent> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::WaterInteractorComponent::buoyancy_factor,
                &C::WaterInteractorComponent::submerged_percentage,
                &C::WaterInteractorComponent::fluid_velocity_at_pos);
    }
};
template<>
struct TestFields<C::PendingDeletion> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        (void)value;
        (void)visitor;
    }
};
template<>
struct TestFields<C::Need> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value, visitor, &C::Need::name, &C::Need::pressure, &C::Need::growth_per_tick);
    }
};
template<>
struct TestFields<C::Action> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::Action::name,
                &C::Action::target,
                &C::Action::duration,
                &C::Action::flee);
    }
};
template<>
struct TestFields<C::StimulusSubscription> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &C::StimulusSubscription::channel,
                &C::StimulusSubscription::need,
                &C::StimulusSubscription::gain);
    }
};
template<>
struct TestFields<::luminumbra::ai::HearingProfile> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &::luminumbra::ai::HearingProfile::range,
                &::luminumbra::ai::HearingProfile::peak_pitch,
                &::luminumbra::ai::HearingProfile::bandwidth,
                &::luminumbra::ai::HearingProfile::gain,
                &::luminumbra::ai::HearingProfile::threshold);
    }
};
template<>
struct TestFields<::luminumbra::ai::Awareness> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &::luminumbra::ai::Awareness::meter,
                &::luminumbra::ai::Awareness::state,
                &::luminumbra::ai::Awareness::has_last_known,
                &::luminumbra::ai::Awareness::last_known_x,
                &::luminumbra::ai::Awareness::last_known_z,
                &::luminumbra::ai::Awareness::time_since_signal);
    }
};
template<>
struct TestFields<::luminumbra::ai::AwarenessParams> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &::luminumbra::ai::AwarenessParams::gain,
                &::luminumbra::ai::AwarenessParams::decay,
                &::luminumbra::ai::AwarenessParams::suspicious_at,
                &::luminumbra::ai::AwarenessParams::alert_at,
                &::luminumbra::ai::AwarenessParams::engaged_at);
    }
};
template<>
struct TestFields<::luminumbra::ai::InstinctCandidate> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &::luminumbra::ai::InstinctCandidate::rank,
                &::luminumbra::ai::InstinctCandidate::id,
                &::luminumbra::ai::InstinctCandidate::action,
                &::luminumbra::ai::InstinctCandidate::target,
                &::luminumbra::ai::InstinctCandidate::need,
                &::luminumbra::ai::InstinctCandidate::score,
                &::luminumbra::ai::InstinctCandidate::need_pressure,
                &::luminumbra::ai::InstinctCandidate::reason);
    }
};
template<>
struct TestFields<::luminumbra::ai::InstinctPlan> {
    template<typename Value, typename Visitor>
    static void Visit(Value& value, const Visitor& visitor) {
        Members(value,
                visitor,
                &::luminumbra::ai::InstinctPlan::actor_id,
                &::luminumbra::ai::InstinctPlan::archetype,
                &::luminumbra::ai::InstinctPlan::decision_contract,
                &::luminumbra::ai::InstinctPlan::candidates,
                &::luminumbra::ai::InstinctPlan::selected_index,
                &::luminumbra::ai::InstinctPlan::checksum,
                &::luminumbra::ai::InstinctPlan::passed);
    }
};

template<typename T>
void Seed(T& value) {
    if constexpr (requires { value.push_back(typename T::value_type{}); } &&
                  !std::is_same_v<T, std::string>) {
        value.emplace_back();
        Seed(value.back());
    } else if constexpr (!std::is_arithmetic_v<T> && !std::is_enum_v<T> &&
                         !std::is_same_v<T, std::string> && !std::is_same_v<T, Luminumbra::Vec2> &&
                         !std::is_same_v<T, Luminumbra::Vec3>) {
        if constexpr (requires {
                          value.begin();
                          value.end();
                      }) {
            for (auto& element : value)
                Seed(element);
        } else {
            TestFields<T>::Visit(value, [](auto& field) { Seed(field); });
        }
    }
}

template<typename T, typename Check>
void MutateEach(T& value, const Check& check) {
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>) {
        const auto before = value;
        if constexpr (std::is_same_v<T, entt::entity>)
            value = static_cast<entt::entity>(1);
        else if constexpr (std::is_same_v<T, bool>)
            value = !value;
        else if constexpr (std::is_enum_v<T>)
            value = static_cast<T>(1);
        else if constexpr (std::is_same_v<T, std::string>)
            value += "x";
        else if constexpr (std::is_floating_point_v<T>)
            value = value > T{0} ? value - T{0.125} : T{0.125};
        else
            value = value == std::numeric_limits<T>::max() ? T{0} : static_cast<T>(value + 1);
        check();
        value = before;
    } else if constexpr (std::is_same_v<T, Luminumbra::Vec2>) {
        MutateEach(value.x, check);
        MutateEach(value.y, check);
    } else if constexpr (std::is_same_v<T, Luminumbra::Vec3>) {
        MutateEach(value.x, check);
        MutateEach(value.y, check);
        MutateEach(value.z, check);
    } else if constexpr (requires {
                             value.begin();
                             value.end();
                         }) {
        for (auto& element : value)
            MutateEach(element, check);
        if constexpr (requires {
                          value.emplace_back();
                          value.pop_back();
                      }) {
            value.emplace_back();
            check();
            value.pop_back();
        }
    } else {
        TestFields<T>::Visit(value, [&](auto& field) { MutateEach(field, check); });
    }
}

template<typename T>
void CheckFields(bool plant = false) {
    entt::registry registry;
    const auto entity = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(entity, 1u);
    if constexpr (!std::is_same_v<T, C::CreatureComponent>)
        registry.emplace<C::CreatureComponent>(entity);
    // Reference mutations resolve entity 1 to stable ID 2, independent of handles.
    const auto target = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(target, 2u);
    if (plant)
        registry.emplace<C::PlantTag>(entity);
    registry.emplace<T>(entity);
    if constexpr (!std::is_empty_v<T>)
        Seed(registry.get<T>(entity));
    S::ParticipantProjectionScratch scratch;
    const auto hash = [&] {
        return plant ? S::ComputePlantProjection(registry, 100, scratch)
                     : S::ComputeEcologyProjection(registry, scratch);
    };
    const auto baseline = hash();
    ASSERT_FALSE(baseline.empty());
    std::size_t mutations = 0;
    if constexpr (!std::is_empty_v<T>) {
        MutateEach(registry.get<T>(entity), [&] {
            EXPECT_NE(hash(), baseline) << "mutation " << mutations;
            ++mutations;
        });
    }
    registry.remove<T>(entity);
    EXPECT_NE(hash(), baseline) << "presence";
}
TEST(ParticipantProjectionFields, PlantTag) {
    CheckFields<C::PlantTag>();
}
TEST(ParticipantProjectionFields, PlantGenomeComponent) {
    CheckFields<C::PlantGenomeComponent>();
}
TEST(ParticipantProjectionFields, PlantGrowthComponent) {
    CheckFields<C::PlantGrowthComponent>();
}
TEST(ParticipantProjectionFields, SoilFeederComponent) {
    CheckFields<C::SoilFeederComponent>();
}
TEST(ParticipantProjectionFields, PlantHealthComponent) {
    CheckFields<C::PlantHealthComponent>();
}
TEST(ParticipantProjectionFields, PollinationComponent) {
    CheckFields<C::PollinationComponent>();
}
TEST(ParticipantProjectionFields, CropLifecycleComponent) {
    CheckFields<C::CropLifecycleComponent>();
}
TEST(ParticipantProjectionFields, PlantRegionComponent) {
    CheckFields<C::PlantRegionComponent>();
}
TEST(ParticipantProjectionFields, PollinationTag) {
    CheckFields<C::PollinationTag>();
}
TEST(ParticipantProjectionFields, CombustibleComponent) {
    CheckFields<C::CombustibleComponent>();
}
TEST(ParticipantProjectionFields, MortalComponent) {
    CheckFields<C::MortalComponent>();
}
TEST(ParticipantProjectionFields, GrazeableComponent) {
    CheckFields<C::GrazeableComponent>();
}
TEST(ParticipantProjectionFields, WaterSourceComponent) {
    CheckFields<C::WaterSourceComponent>();
}
TEST(ParticipantProjectionFields, IgnitionSourceComponent) {
    CheckFields<C::IgnitionSourceComponent>();
}
TEST(ParticipantProjectionFields, FlowingWaterSourceComponent) {
    CheckFields<C::FlowingWaterSourceComponent>();
}
TEST(ParticipantProjectionFields, DecayComponent) {
    CheckFields<C::DecayComponent>();
}
TEST(ParticipantProjectionFields, FieldEmitterComponent) {
    CheckFields<C::FieldEmitterComponent>();
}
TEST(ParticipantProjectionFields, CreatureComponent) {
    CheckFields<C::CreatureComponent>();
}
TEST(ParticipantProjectionFields, CreatureGenomeComponent) {
    CheckFields<C::CreatureGenomeComponent>();
}
TEST(ParticipantProjectionFields, CreaturePhysicsComponent) {
    CheckFields<C::CreaturePhysicsComponent>();
}
TEST(ParticipantProjectionFields, ThirstComponent) {
    CheckFields<C::ThirstComponent>();
}
TEST(ParticipantProjectionFields, WaterHoleComponent) {
    CheckFields<C::WaterHoleComponent>();
}
TEST(ParticipantProjectionFields, CircadianComponent) {
    CheckFields<C::CircadianComponent>();
}
TEST(ParticipantProjectionFields, AlarmComponent) {
    CheckFields<C::AlarmComponent>();
}
TEST(ParticipantProjectionFields, PackHunterComponent) {
    CheckFields<C::PackHunterComponent>();
}
TEST(ParticipantProjectionFields, TerritoryComponent) {
    CheckFields<C::TerritoryComponent>();
}
TEST(ParticipantProjectionFields, TerritoryBiasComponent) {
    CheckFields<C::TerritoryBiasComponent>();
}
TEST(ParticipantProjectionFields, MigratoryComponent) {
    CheckFields<C::MigratoryComponent>();
}
TEST(ParticipantProjectionFields, ScavengerComponent) {
    CheckFields<C::ScavengerComponent>();
}
TEST(ParticipantProjectionFields, ForagerComponent) {
    CheckFields<C::ForagerComponent>();
}
TEST(ParticipantProjectionFields, FoodSourceComponent) {
    CheckFields<C::FoodSourceComponent>();
}
TEST(ParticipantProjectionFields, NeedsComponent) {
    CheckFields<C::NeedsComponent>();
}
TEST(ParticipantProjectionFields, SensesComponent) {
    CheckFields<C::SensesComponent>();
}
TEST(ParticipantProjectionFields, OpportunityComponent) {
    CheckFields<C::OpportunityComponent>();
}
TEST(ParticipantProjectionFields, ActionPlanComponent) {
    CheckFields<C::ActionPlanComponent>();
}
TEST(ParticipantProjectionFields, InstinctAgent) {
    CheckFields<C::InstinctAgent>();
}
TEST(ParticipantProjectionFields, LocomotionProfile) {
    CheckFields<C::LocomotionProfile>();
}
TEST(ParticipantProjectionFields, SensableComponent) {
    CheckFields<C::SensableComponent>();
}
TEST(ParticipantProjectionFields, PerceptionComponent) {
    CheckFields<C::PerceptionComponent>();
}
TEST(ParticipantProjectionFields, AwarenessComponent) {
    CheckFields<C::AwarenessComponent>();
}
TEST(ParticipantProjectionFields, LocomotionIntentComponent) {
    CheckFields<C::LocomotionIntentComponent>();
}
TEST(ParticipantProjectionFields, ScentSenseComponent) {
    CheckFields<C::ScentSenseComponent>();
}
TEST(ParticipantProjectionFields, LocomotionPathComponent) {
    CheckFields<C::LocomotionPathComponent>();
}
TEST(ParticipantProjectionFields, StimulusSubscriptionComponent) {
    CheckFields<C::StimulusSubscriptionComponent>();
}
TEST(ParticipantProjectionFields, InstinctAgentComponent) {
    CheckFields<C::InstinctAgentComponent>();
}
TEST(ParticipantProjectionFields, WaterInteractorComponent) {
    CheckFields<C::WaterInteractorComponent>();
}
TEST(PlantProjectionFields, TransformComponent) {
    CheckFields<C::TransformComponent>(true);
}
TEST(PlantProjectionFields, PlantGenomeComponent) {
    CheckFields<C::PlantGenomeComponent>(true);
}
TEST(PlantProjectionFields, PlantGrowthComponent) {
    CheckFields<C::PlantGrowthComponent>(true);
}
TEST(PlantProjectionFields, SoilFeederComponent) {
    CheckFields<C::SoilFeederComponent>(true);
}
TEST(PlantProjectionFields, PlantHealthComponent) {
    CheckFields<C::PlantHealthComponent>(true);
}
TEST(PlantProjectionFields, PollinationComponent) {
    CheckFields<C::PollinationComponent>(true);
}
TEST(PlantProjectionFields, CropLifecycleComponent) {
    CheckFields<C::CropLifecycleComponent>(true);
}
TEST(PlantProjectionFields, PlantRegionComponent) {
    CheckFields<C::PlantRegionComponent>(true);
}
TEST(PlantProjectionFields, PollinationTag) {
    CheckFields<C::PollinationTag>(true);
}
TEST(PlantProjectionFields, CombustibleComponent) {
    CheckFields<C::CombustibleComponent>(true);
}
TEST(PlantProjectionFields, MortalComponent) {
    CheckFields<C::MortalComponent>(true);
}
TEST(PlantProjectionFields, GrazeableComponent) {
    CheckFields<C::GrazeableComponent>(true);
}
TEST(PlantProjectionFields, WaterSourceComponent) {
    CheckFields<C::WaterSourceComponent>(true);
}
TEST(PlantProjectionFields, IgnitionSourceComponent) {
    CheckFields<C::IgnitionSourceComponent>(true);
}
TEST(PlantProjectionFields, FlowingWaterSourceComponent) {
    CheckFields<C::FlowingWaterSourceComponent>(true);
}
TEST(PlantProjectionFields, DecayComponent) {
    CheckFields<C::DecayComponent>(true);
}
TEST(PlantProjectionFields, FieldEmitterComponent) {
    CheckFields<C::FieldEmitterComponent>(true);
}

TEST(ParticipantProjectionFields, TransformPosition) {
    CheckFields<C::TransformComponent>();
}

TEST(ParticipantProjection, EmptyCursorAndRolePresence) {
    entt::registry registry;
    S::ParticipantProjectionScratch scratch;
    EXPECT_TRUE(S::ComputePlantProjection(registry, 1, scratch).empty());
    const auto consumed = S::ComputePlantProjection(registry, 2, scratch);
    EXPECT_FALSE(consumed.empty());
    EXPECT_NE(consumed, S::ComputePlantProjection(registry, 3, scratch));
    EXPECT_TRUE(S::ComputeEcologyProjection(registry, scratch).empty());
    const auto entity = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(entity, 1u);
    registry.emplace<C::PlantTag>(entity);
    const auto plant = S::ComputePlantProjection(registry, 2, scratch);
    EXPECT_NE(plant, consumed);
    EXPECT_TRUE(S::ComputeEcologyProjection(registry, scratch).empty());
    registry.remove<C::PlantTag>(entity);
    EXPECT_EQ(S::ComputePlantProjection(registry, 2, scratch), consumed);
    registry.emplace<C::CreatureComponent>(entity);
    const auto alive = S::ComputeEcologyProjection(registry, scratch);
    registry.emplace<C::PendingDeletion>(entity);
    EXPECT_NE(alive, S::ComputeEcologyProjection(registry, scratch));
}

TEST(ParticipantProjection, HandlesHolesGenerationsAndPhysicsPoolSlotsAreExcluded) {
    const auto run = [](bool churn) {
        entt::registry registry;
        if (churn) {
            const auto hole = registry.create();
            registry.destroy(hole);
            for (int index = 0; index < 23; ++index)
                registry.emplace<C::TransformComponent>(registry.create());
        }
        std::array<entt::entity, 2> creatures{};
        for (unsigned index = 0; index < 2; ++index) {
            const unsigned semantic = churn ? 1u - index : index;
            const auto entity = registry.create();
            creatures[semantic] = entity;
            registry.emplace<C::SimulationIdentityComponent>(entity, semantic + 1u);
            registry.emplace<C::CreatureComponent>(entity).hunger =
                static_cast<float>(semantic) * 0.25f;
            registry.emplace<C::CreaturePhysicsComponent>(entity).avatar_index = churn ? 100u : 2u;
        }
        registry.emplace<C::SensesComponent>(creatures[0]).nearest_threat = creatures[1];
        registry.emplace<C::ActionPlanComponent>(creatures[0])
            .plan.push_back(
                C::Action{.name = "seek", .target = creatures[1], .duration = 1.0f, .flee = false});
        S::ParticipantProjectionScratch scratch;
        return S::ComputeEcologyProjection(registry, scratch);
    };
    EXPECT_EQ(run(false), run(true));
}

TEST(ParticipantProjection, FullWidthIdsOrderBothRostersAndIdentityMutationsAreVisible) {
    const auto run = [](bool reverse) {
        entt::registry registry;
        constexpr std::array ids{std::uint64_t{1},
                                 std::uint64_t{257},
                                 (std::uint64_t{1} << 32) + 1,
                                 std::uint64_t{1} << 62};
        for (std::size_t index = 0; index < ids.size(); ++index) {
            const auto semantic = reverse ? ids.size() - 1 - index : index;
            const auto entity = registry.create();
            registry.emplace<C::SimulationIdentityComponent>(entity, ids[semantic]);
            registry.emplace<C::CreatureComponent>(entity).last_action = static_cast<int>(semantic);
            registry.emplace<C::PlantTag>(entity);
            registry.emplace<C::PlantGrowthComponent>(entity).growth_points =
                static_cast<std::uint32_t>(semantic);
        }
        S::ParticipantProjectionScratch scratch;
        const auto ecology = S::ComputeEcologyProjection(registry, scratch);
        const auto plants = S::ComputePlantProjection(registry, std::uint64_t{1} << 63, scratch);
        for (std::size_t index = 0; index < ids.size(); ++index)
            EXPECT_EQ(scratch.entries[index].id, ids[index]);
        const auto entity = scratch.entries[0].entity;
        registry.get<C::SimulationIdentityComponent>(entity).id = 2;
        EXPECT_NE(ecology, S::ComputeEcologyProjection(registry, scratch));
        EXPECT_NE(plants, S::ComputePlantProjection(registry, std::uint64_t{1} << 63, scratch));
        return std::pair{ecology, plants};
    };
    EXPECT_EQ(run(false), run(true));
}

TEST(ParticipantProjection, EveryReferenceDistinguishesNullInvalidAndStableTargets) {
    entt::registry registry;
    const auto agent = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(agent, 1u);
    registry.emplace<C::CreatureComponent>(agent);
    auto& senses = registry.emplace<C::SensesComponent>(agent);
    auto& plan = registry.emplace<C::ActionPlanComponent>(agent);
    plan.plan.emplace_back();
    const auto first = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(first, 2u);
    const auto second = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(second, 3u);
    const auto invalid = registry.create();
    registry.destroy(invalid);
    S::ParticipantProjectionScratch scratch;
    for (auto* reference : {&senses.nearest_food_source,
                            &senses.nearest_water_source,
                            &senses.nearest_threat,
                            &plan.plan[0].target}) {
        std::array<std::string, 4> hashes;
        const std::array<entt::entity, 4> targets{entt::null, invalid, first, second};
        for (std::size_t index = 0; index < targets.size(); ++index) {
            *reference = targets[index];
            hashes[index] = S::ComputeEcologyProjection(registry, scratch);
        }
        for (std::size_t left = 0; left < hashes.size(); ++left)
            for (std::size_t right = left + 1; right < hashes.size(); ++right)
                EXPECT_NE(hashes[left], hashes[right]);
        *reference = entt::null;
    }
    const auto missing = registry.create();
    senses.nearest_threat = missing;
    EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
}

TEST(ParticipantProjection, IdentifiedReferenceTargetsProjectPositionWithoutAnotherRole) {
    entt::registry registry;
    const auto agent = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(agent, 1u);
    registry.emplace<C::CreatureComponent>(agent);
    const auto target = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(target, 2u);
    auto& transform = registry.emplace<C::TransformComponent>(target);
    registry.emplace<C::SensesComponent>(agent).nearest_food_source = target;
    registry.emplace<C::ActionPlanComponent>(agent).plan.push_back(
        C::Action{.name = "seek", .target = target, .duration = 1.0f, .flee = false});
    S::ParticipantProjectionScratch scratch;
    const auto baseline = S::ComputeEcologyProjection(registry, scratch);
    EXPECT_EQ(scratch.participant_visits, 2u);
    transform.position.x = 1.0f;
    EXPECT_NE(baseline, S::ComputeEcologyProjection(registry, scratch));
    EXPECT_EQ(scratch.participant_visits, 2u);
}

TEST(ParticipantProjection, OrderingOfListsAndNestedPlannerStateIsVisible) {
    entt::registry registry;
    const auto entity = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(entity, 1u);
    auto& plan = registry.emplace<C::ActionPlanComponent>(entity);
    plan.plan = {C::Action{.name = "one"}, C::Action{.name = "two"}};
    S::ParticipantProjectionScratch scratch;
    const auto first = S::ComputeEcologyProjection(registry, scratch);
    std::swap(plan.plan[0], plan.plan[1]);
    EXPECT_NE(first, S::ComputeEcologyProjection(registry, scratch));
    registry.remove<C::ActionPlanComponent>(entity);
    auto& agent = registry.emplace<C::InstinctAgentComponent>(entity);
    agent.current_plan.candidates.resize(2);
    agent.current_plan.candidates[0].id = "one";
    agent.current_plan.candidates[1].id = "two";
    const auto ranked = S::ComputeEcologyProjection(registry, scratch);
    std::swap(agent.current_plan.candidates[0], agent.current_plan.candidates[1]);
    EXPECT_NE(ranked, S::ComputeEcologyProjection(registry, scratch));
}

TEST(ParticipantProjection, InvalidIdentitiesAndCursorsRefuseWithoutRepair) {
    entt::registry registry;
    const auto entity = registry.create();
    registry.emplace<C::CreatureComponent>(entity);
    S::ParticipantProjectionScratch scratch;
    EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
    EXPECT_FALSE(registry.all_of<C::SimulationIdentityComponent>(entity));
    auto& identity = registry.emplace<C::SimulationIdentityComponent>(entity);
    for (const auto id :
         {std::uint64_t{0}, std::uint64_t{1} << 63, std::numeric_limits<std::uint64_t>::max()}) {
        identity.id = id;
        EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
        EXPECT_EQ(identity.id, id);
    }
    identity.id = 7;
    const auto duplicate = registry.create();
    registry.emplace<C::CreatureComponent>(duplicate);
    registry.emplace<C::SimulationIdentityComponent>(duplicate, 7u);
    EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
    registry.destroy(duplicate);
    registry.emplace<C::PlantTag>(entity);
    for (const auto cursor : {std::uint64_t{0}, std::uint64_t{7}, (std::uint64_t{1} << 63) + 1})
        EXPECT_THROW((void)S::ComputePlantProjection(registry, cursor, scratch), std::domain_error);
    EXPECT_FALSE(S::ComputePlantProjection(registry, std::uint64_t{1} << 63, scratch).empty());
}

TEST(ParticipantProjection, FiniteFloatsSignedZeroAndCountedUtf8) {
    entt::registry registry;
    const auto entity = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(entity, 1u);
    auto& creature = registry.emplace<C::CreatureComponent>(entity);
    S::ParticipantProjectionScratch scratch;
    const auto zero = S::ComputeEcologyProjection(registry, scratch);
    creature.wish_x = -0.0f;
    EXPECT_EQ(zero, S::ComputeEcologyProjection(registry, scratch));
    for (const float bad : {std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
        creature.wish_x = bad;
        EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
        EXPECT_FALSE(std::isfinite(creature.wish_x));
    }
    creature.wish_x = 0;
    auto& opportunity = registry.emplace<C::OpportunityComponent>(entity);
    for (const auto& text : {std::string("\x80"),
                             std::string("\xc2"),
                             std::string("\xc2x"),
                             std::string("\xe0\x80\x80"),
                             std::string("\xed\xa0\x80"),
                             std::string("\xf4\x90\x80\x80")}) {
        opportunity.id = text;
        EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
    }
    for (const auto& text : {std::string("\xc2\xa3"),
                             std::string("\xe2\x82\xac"),
                             std::string("\xf0\x9f\x90\x80"),
                             std::string("a\0b", 3)}) {
        opportunity.id = text;
        EXPECT_FALSE(S::ComputeEcologyProjection(registry, scratch).empty());
    }
    opportunity.id = "ab";
    opportunity.action = "c";
    const auto split = S::ComputeEcologyProjection(registry, scratch);
    opportunity.id = "a";
    opportunity.action = "bc";
    EXPECT_NE(split, S::ComputeEcologyProjection(registry, scratch));
}

TEST(ParticipantProjection, SimulationDomainsRefuseBeforeReturningAHash) {
    entt::registry registry;
    const auto entity = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(entity, 1u);
    auto& creature = registry.emplace<C::CreatureComponent>(entity);
    S::ParticipantProjectionScratch scratch;
    for (const float invalid : {-0.125f, 1.125f}) {
        creature.hunger = invalid;
        EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
        EXPECT_EQ(creature.hunger, invalid);
    }
    creature.hunger = 0;
    auto& genome = registry.emplace<C::CreatureGenomeComponent>(entity);
    for (const float invalid : {-1.125f, 1.125f}) {
        genome.vision_cos_half_fov = invalid;
        EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
    }
    genome.vision_cos_half_fov = 0;
    auto& plant_genome = registry.emplace<C::PlantGenomeComponent>(entity);
    for (const float invalid : {-0.125f, 1.125f}) {
        plant_genome.genes[0] = invalid;
        EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
    }
    plant_genome.genes[0] = 0;
    auto& awareness = registry.emplace<C::AwarenessComponent>(entity);
    for (const int invalid : {-1, 5}) {
        awareness.awareness.state = static_cast<luminumbra::ai::AwarenessState>(invalid);
        EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
    }
    awareness.awareness.state = luminumbra::ai::AwarenessState::Unaware;
    auto& subscriptions = registry.emplace<C::StimulusSubscriptionComponent>(entity);
    subscriptions.subscriptions.emplace_back();
    subscriptions.subscriptions[0].channel = static_cast<luminumbra::ai::StimulusChannel>(6);
    EXPECT_THROW((void)S::ComputeEcologyProjection(registry, scratch), std::domain_error);
    subscriptions.subscriptions[0].channel = luminumbra::ai::StimulusChannel::Weather;
    EXPECT_FALSE(S::ComputeEcologyProjection(registry, scratch).empty());
}

TEST(ParticipantProjection, VisitsParticipantsOnceAndReusesScratchWithUnrelatedResidents) {
    entt::registry registry;
    for (int index = 0; index < 4096; ++index) {
        const auto resident = registry.create();
        registry.emplace<C::TransformComponent>(resident);
        registry.emplace<C::PendingDeletion>(resident);
    }
    const auto entity = registry.create();
    registry.emplace<C::SimulationIdentityComponent>(entity, 1u);
    registry.emplace<C::CreatureComponent>(entity);
    registry.emplace<C::AlarmComponent>(entity);
    registry.emplace<C::ThirstComponent>(entity);
    S::ParticipantProjectionScratch scratch;
    const auto baseline = S::ComputeEcologyProjection(registry, scratch);
    EXPECT_EQ(scratch.participant_visits, 1u);
    EXPECT_EQ(scratch.source_visits, 4u);
    const auto* entries = scratch.entries.data();
    const auto* radix = scratch.radix.data();
    EXPECT_EQ(baseline, S::ComputeEcologyProjection(registry, scratch));
    EXPECT_EQ(scratch.participant_visits, 1u);
    EXPECT_EQ(scratch.source_visits, 4u);
    EXPECT_EQ(scratch.entries.data(), entries);
    EXPECT_EQ(scratch.radix.data(), radix);
    registry.destroy(entity);
    EXPECT_TRUE(S::ComputeEcologyProjection(registry, scratch).empty());
    EXPECT_EQ(scratch.participant_visits, 0u);
    EXPECT_EQ(scratch.source_visits, 0u);
}

TEST(ParticipantProjection, LegacyHashRemainsByteIdenticalWithCandidateState) {
    entt::registry registry;
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity);
    auto& creature = registry.emplace<C::CreatureComponent>(entity);
    const auto legacy = luminumbra::ai::ComputeLegacyEcologySubHash(registry);
    // Independent exact text for the legacy default creature; no identity needed.
    EXPECT_EQ(legacy, Luminumbra::Persistence::StableChecksum("ecology:v2:1:0,0,0,0,0,0,1,0,1,0;"));
    registry.emplace<C::SimulationIdentityComponent>(entity, 42u);
    creature.is_predator = true;
    registry.emplace<C::TerritoryComponent>(entity).radius = 700.0f;
    S::ParticipantProjectionScratch scratch;
    EXPECT_FALSE(S::ComputeEcologyProjection(registry, scratch).empty());
    EXPECT_EQ(legacy, luminumbra::ai::ComputeLegacyEcologySubHash(registry));
}

TEST(ParticipantProjection, ShippingSelectionIsDormant) {
    ASSERT_FALSE(S::UseCompleteSimulationProjections());
    entt::registry registry;
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity);
    registry.emplace<C::CreatureComponent>(entity);
    EXPECT_EQ(luminumbra::ai::ComputeEcologySubHash(registry),
              luminumbra::ai::ComputeLegacyEcologySubHash(registry));
}
} // namespace
