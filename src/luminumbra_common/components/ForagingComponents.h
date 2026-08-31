#pragma once

// ant-trail FORAGING via stigmergy — the components for the Deneubourg
// double-bridge model. A forager shuttles between a NEST and a FOOD source, depositing pheromone on
// BOTH legs into two ScentField channels (an outbound ant lays the trail-to-home; a laden returning
// ant lays the trail-to-food), and follows the OPPOSITE channel's gradient. Because a shorter route
// is traversed more often per unit time, its trail is reinforced faster than evaporation erases it,
// so the colony self-organises onto the shorter path (ACO / Dorigo). Evaporation is what makes the
// selection happen — with zero evaporation every trail saturates and no preference emerges.
//
// DETERMINISM: the forager carries its authoritative GRID cell (integers), so the whole foraging
// sim is integer/id-ordered/libm-free and snapshot-free; ForagingSystem advances it one cell/tick.
// These are pure data, in their own header (no wide EnTT recompile blast radius), and are an
// OPT-IN: a world with no ForagerComponent runs ForagingSystem as a no-op, so the canonical
// roster's world_hash is byte-identical.

#include <cstdint>

namespace Luminumbra::Components {

// A foraging agent. Its (cell_x, cell_z) is the authoritative position on the ScentField grid; a
// render/transform owner may mirror it, but the sim moves THIS.
struct ForagerComponent {
    std::int32_t cell_x = 0; // current grid cell on the ScentField
    std::int32_t cell_z = 0;
    std::int32_t home_x = 0; // nest cell (drop-off; return target while carrying)
    std::int32_t home_z = 0;
    bool carrying_food = false;   // false = outbound (seeking food), true = laden (returning home)
    std::uint32_t deliveries = 0; // food units delivered to the nest (telemetry / selection signal)
};

// A food source on the grid. `amount` decrements as foragers pick up (deterministic depletion); a
// source at 0 is exhausted and no longer offers food.
struct FoodSourceComponent {
    std::int32_t cell_x = 0;
    std::int32_t cell_z = 0;
    std::int32_t amount = 1000000; // remaining units (effectively inexhaustible by default)
};

} // namespace Luminumbra::Components
