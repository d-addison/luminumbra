#pragma once
#include "../../../include/luminumbra/core/Types.h"

namespace Luminumbra::Components {

/**
 * @brief A tag component attached to entities that generate water (e.g., a spring).
 * The WaterSystem will use this entity's position as a source point.
 */
struct WaterSourceComponent {
    /**
     * @brief The volume of water (in cubic meters) to add to the simulation per second.
     */
    f32 flow_rate = 1.0f;
};

/**
 * @brief Component attached to an entity to make it interact with the water simulation.
 * Used for buoyancy calculations and creating ripples/displacements.
 */
struct WaterInteractorComponent {
    /**
     * @brief A multiplier for the buoyancy force. >1.0 floats higher, <1.0 sinks lower.
     */
    f32 buoyancy_factor = 1.0f;

    /**
     * @brief The percentage of the entity's physics body that is currently submerged.
     * This value is calculated and updated by the WaterSystem each tick.
     */
    f32 submerged_percentage = 0.0f;

    /**
     * @brief The velocity of the water at the entity's position.
     * This value is calculated and updated by the WaterSystem each tick and can be used to apply a drag/push force.
     */
    Vec3 fluid_velocity_at_pos{0.0f};
};

} // namespace Luminumbra::Components