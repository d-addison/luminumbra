#pragma once

// ForagingSystem — ant-trail foraging via stigmergy (Deneubourg double-bridge /
// Dorigo ACO). Each tick every ForagerComponent:
//   1. DEPOSITS pheromone on BOTH legs of its trip — an outbound ant lays the HOME trail (channel
//   3);
//      a laden returning ant lays the FOOD trail (channel 2). This is the "double trail".
//   2. STEPS one grid cell, choosing the neighbour that maximises a blend of the OPPOSITE channel's
//      pheromone (follow the trail the other leg laid) and a deterministic pull toward its goal
//      (nearest food when outbound, the nest when laden) so it makes progress before any trail
//      exists.
//   3. PICKS UP at a food source / DELIVERS at the nest, flipping carrying state.
// Because a SHORTER route is completed more often per unit time, its trail is reinforced faster
// than evaporation (ScentField::Step) erases it, so foragers converge on the shorter path. Zero
// evaporation removes the contrast (saturation) — the classic double-bridge result.
//
// DETERMINISM (sim path): id-ordered foragers + id-ordered food sources; integer grid cells; a
// FIXED 4-neighbour scan order with first-wins tie-break; no RNG / wall-clock / libm. The field
// read/write is the solver's deterministic add_impulse / at. OPT-IN: no ForagerComponent ->
// no-op, so the canonical roster's world_hash is byte-identical (same discipline as
// scent/creatures/plants).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "entt/entt.hpp"

#include "../components/ForagingComponents.h"
#include "ScentField.h"

namespace luminumbra::ai {

// ScentField channel layout for foraging (the 4-channel field is 0 prey / 1 predator / 2 food-trail
// / 3 home-trail). Outbound ants lay the HOME trail and follow the FOOD trail; laden ants lay the
// FOOD trail and follow the HOME trail.
inline constexpr int kFoodTrailChannel = 2;
inline constexpr int kHomeTrailChannel = 3;

struct ForagingParams {
    double deposit = 1.0;      // pheromone laid per step
    double trail_weight = 8.0; // attraction to the opposite-channel trail
    double goal_weight = 1.0;  // deterministic pull toward the goal (0 = pure trail following)
};

struct ForagingStats {
    std::uint64_t foragers = 0;
    std::uint64_t pickups = 0;
    std::uint64_t deliveries = 0; // food delivered to nests THIS tick
};

// Advance every forager one cell. Pure function of registry + field state (+ params).
inline ForagingStats
RunForagingOnTick(entt::registry& reg, ScentField& field, const ForagingParams& p = {}) {
    namespace Comp = ::Luminumbra::Components;
    ForagingStats stats;

    std::vector<entt::entity> ants;
    {
        auto view = reg.view<Comp::ForagerComponent>();
        for (auto e : view)
            ants.push_back(e);
    }
    if (ants.empty())
        return stats; // opt-in: no foragers -> byte-identical no-op
    std::sort(ants.begin(), ants.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    // id-ordered food source cells (with a live handle so pickups can deplete them).
    struct Food {
        entt::entity e;
        std::int32_t x, z;
    };
    std::vector<Food> foods;
    {
        auto fv = reg.view<Comp::FoodSourceComponent>();
        std::vector<entt::entity> fe(fv.begin(), fv.end());
        std::sort(fe.begin(), fe.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        for (auto e : fe) {
            const auto& f = reg.get<Comp::FoodSourceComponent>(e);
            foods.push_back({e, f.cell_x, f.cell_z});
        }
    }

    static const int kDX[4] = {1, -1, 0, 0}; // FIXED scan order -> deterministic tie-break
    static const int kDZ[4] = {0, 0, 1, -1};

    for (auto e : ants) {
        ++stats.foragers;
        auto& a = reg.get<Comp::ForagerComponent>(e);

        // 1. Deposit-on-both-trips: lay the trail leading back toward where we came from.
        const int layCh = a.carrying_food ? kFoodTrailChannel : kHomeTrailChannel;
        field.Deposit(layCh, a.cell_x, a.cell_z, p.deposit);

        // 2. Goal cell: laden -> nest; outbound -> nearest live food (Manhattan, first-wins
        // tie-break).
        std::int32_t goalX = a.cell_x, goalZ = a.cell_z;
        if (a.carrying_food) {
            goalX = a.home_x;
            goalZ = a.home_z;
        } else {
            std::int64_t best = -1;
            for (const auto& f : foods) {
                const std::int64_t d = std::llabs(static_cast<long long>(f.x) - a.cell_x) +
                                       std::llabs(static_cast<long long>(f.z) - a.cell_z);
                if (best < 0 || d < best) {
                    best = d;
                    goalX = f.x;
                    goalZ = f.z;
                }
            }
        }

        // 3. Step: maximise trail_weight * opposite-channel pheromone + goal_weight * goal
        // progress.
        const int followCh = a.carrying_food ? kHomeTrailChannel : kFoodTrailChannel;
        const std::int64_t curGoalDist = std::llabs(static_cast<long long>(goalX) - a.cell_x) +
                                         std::llabs(static_cast<long long>(goalZ) - a.cell_z);
        int bestDir = -1;
        double bestScore = 0.0;
        for (int d = 0; d < 4; ++d) {
            const int nx = a.cell_x + kDX[d];
            const int nz = a.cell_z + kDZ[d];
            const double trail = field.Sample(followCh, nx, nz);
            const std::int64_t gd = std::llabs(static_cast<long long>(goalX) - nx) +
                                    std::llabs(static_cast<long long>(goalZ) - nz);
            const double goalTerm = static_cast<double>(curGoalDist - gd); // +1 closer, -1 farther
            const double score = p.trail_weight * trail + p.goal_weight * goalTerm;
            if (bestDir < 0 || score > bestScore) {
                bestScore = score;
                bestDir = d;
            }
        }
        if (bestDir >= 0) {
            a.cell_x += kDX[bestDir];
            a.cell_z += kDZ[bestDir];
            // Keep the forager ON the grid. An unclamped step can walk a cell off the field
            // (e.g. a nest/food placed near an edge), and a downstream render mirror would then
            // map it to a far-out-of-bounds world coordinate and sample terrain there (crash /
            // garbage). Clamp to the valid cell range — deterministic integer math.
            if (a.cell_x < 0)
                a.cell_x = 0;
            else if (a.cell_x >= field.width())
                a.cell_x = field.width() - 1;
            if (a.cell_z < 0)
                a.cell_z = 0;
            else if (a.cell_z >= field.height())
                a.cell_z = field.height() - 1;
        }

        // 4. Arrival transitions.
        if (!a.carrying_food) {
            for (const auto& f : foods) {
                if (f.x == a.cell_x && f.z == a.cell_z) {
                    auto& fc = reg.get<Comp::FoodSourceComponent>(f.e);
                    if (fc.amount > 0) {
                        --fc.amount;
                        a.carrying_food = true;
                        ++stats.pickups;
                    }
                    break;
                }
            }
        } else if (a.cell_x == a.home_x && a.cell_z == a.home_z) {
            a.carrying_food = false;
            ++a.deliveries;
            ++stats.deliveries;
        }
    }
    return stats;
}

} // namespace luminumbra::ai
