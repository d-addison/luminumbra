#include "InstinctLocomotionSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "../components/CoreComponents.h"
#include "../components/InstinctComponents.h"
#include "../core/DeterministicMath.h"

namespace luminumbra::ai {
namespace {

struct AgentPos {
    entt::entity id;
    float x;
    float z;
    float wx;
    float wz;
};

// Only candidate selection uses double precision. Steering retains the original
// float operations and id order, including its strict sqrt-based distance test.
class NeighborGrid {
public:
    NeighborGrid(const std::vector<AgentPos>& snapshot, float largest_radius)
        : cell_size_(std::min(static_cast<double>(largest_radius), 1024.0))
        , candidate_limit_(CandidateLimit(snapshot.size())) {
        if (!(cell_size_ > 0.0)) {
            return;
        }
        for (std::size_t index = 0; index < snapshot.size(); ++index) {
            const auto& position = snapshot[index];
            Cell cell{};
            if (CellCoordinate(static_cast<double>(position.x), cell.first) &&
                CellCoordinate(static_cast<double>(position.z), cell.second)) {
                cells_[cell].push_back(index);
            } else {
                // Keep unindexable entries in every query rather than dropping
                // finite neighbors near the representable cell-key limits.
                unindexed_.push_back(index);
            }
        }
    }

    bool Query(float x, float z, float radius, std::vector<std::size_t>& candidates) const {
        // Below sqrt(FLT_MIN), squared distances can underflow. Use the exact
        // scan there, and for non-finite inputs or an unusable grid.
        constexpr float kMinGridRadius = 0x1p-63f;
        if (!(cell_size_ > 0.0) || !std::isfinite(x) || !std::isfinite(z) ||
            !std::isfinite(radius) || radius < kMinGridRadius) {
            return false;
        }
        // Round the query radius outwards by eight binary32 epsilons to
        // cover subtraction, squared-distance and sqrt rounding at the strict
        // cutoff. Query only cells overlapping that conservative radius; every
        // candidate still goes through the unchanged float distance test.
        const double reach = static_cast<double>(radius) * (1.0 + 0x1p-20);
        Cell lower{}, upper{};
        if (!CellCoordinate(static_cast<double>(x) - reach, lower.first) ||
            !CellCoordinate(static_cast<double>(z) - reach, lower.second) ||
            !CellCoordinate(static_cast<double>(x) + reach, upper.first) ||
            !CellCoordinate(static_cast<double>(z) + reach, upper.second)) {
            return false;
        }
        const auto width = static_cast<std::int64_t>(upper.first) - lower.first + 1;
        const auto height = static_cast<std::int64_t>(upper.second) - lower.second + 1;
        // Cap cell size above, and bound traversal here: enormous radii must not
        // collapse the whole grid or request an unbounded number of lookups.
        constexpr std::int64_t kMaxQueryCells = 64;
        if (width > kMaxQueryCells || height > kMaxQueryCells || width * height > kMaxQueryCells) {
            return false;
        }
        // Count disjoint buckets before copying any indices. Above the sorting
        // budget, fall back without candidate allocation, gathering or sorting,
        // and visit each snapshot entry exactly once.
        std::array<const std::vector<std::size_t>*, static_cast<std::size_t>(kMaxQueryCells)>
            query_cells{};
        std::size_t query_cell_count = 0;
        std::size_t candidate_count = unindexed_.size();
        if (candidate_count > candidate_limit_) {
            return false;
        }
        for (std::int64_t cell_x = lower.first; cell_x <= upper.first; ++cell_x) {
            for (std::int64_t cell_z = lower.second; cell_z <= upper.second; ++cell_z) {
                const Cell key{static_cast<std::int32_t>(cell_x),
                               static_cast<std::int32_t>(cell_z)};
                const auto found = cells_.find(key);
                if (found != cells_.end()) {
                    candidate_count += found->second.size();
                    if (candidate_count > candidate_limit_) {
                        return false;
                    }
                    query_cells[query_cell_count] = &found->second;
                    ++query_cell_count;
                }
            }
        }
        candidates.reserve(candidate_count);
        candidates.assign(unindexed_.begin(), unindexed_.end());
        for (std::size_t query_cell_index = 0; query_cell_index < query_cell_count;
             ++query_cell_index) {
            const auto& entries = *query_cells[query_cell_index];
            candidates.insert(candidates.end(), entries.begin(), entries.end());
        }
        std::sort(candidates.begin(), candidates.end());
        return true;
    }

private:
    using Cell = std::pair<std::int32_t, std::int32_t>;

    static std::size_t CandidateLimit(std::size_t count) {
        // A fixed density fraction alone still permits O(N log N) sorting.
        // Limit M to N / max(2, ceil(log2 N)), so M log2 M <= N and gathering
        // never exceeds half the snapshot. Integer arithmetic avoids rounding
        // and overflow, including for an empty or singleton snapshot.
        std::size_t sort_levels = 0;
        for (auto remaining = count; remaining > 1; remaining = remaining / 2 + remaining % 2) {
            ++sort_levels;
        }
        return count / std::max(std::size_t{2}, sort_levels);
    }

    bool CellCoordinate(double position, std::int32_t& coordinate) const {
        const double scaled = std::floor(position / cell_size_);
        // Check the range in double before any integer conversion.
        constexpr auto kMinCell = std::numeric_limits<std::int32_t>::min() + 1;
        constexpr auto kMaxCell = std::numeric_limits<std::int32_t>::max() - 1;
        if (!std::isfinite(scaled) || scaled < static_cast<double>(kMinCell) ||
            scaled > static_cast<double>(kMaxCell)) {
            return false;
        }
        coordinate = static_cast<std::int32_t>(scaled);
        return true;
    }

    double cell_size_;
    std::size_t candidate_limit_;
    std::map<Cell, std::vector<std::size_t>> cells_;
    std::vector<std::size_t> unindexed_;
};

template<typename Visitor>
void VisitNeighbors(const NeighborGrid& grid,
                    const std::vector<AgentPos>& snapshot,
                    float x,
                    float z,
                    float radius,
                    std::vector<std::size_t>& candidates,
                    InstinctLocomotionTickStats& stats,
                    Visitor visit) {
    if (grid.Query(x, z, radius, candidates)) {
        for (const auto index : candidates) {
            ++stats.neighbor_candidates_examined;
            visit(snapshot[index]);
        }
    } else {
        ++stats.neighbor_exhaustive_queries;
        for (const auto& neighbor : snapshot) {
            ++stats.neighbor_candidates_examined;
            visit(neighbor);
        }
    }
}

} // namespace

InstinctLocomotionTickStats
RunInstinctLocomotionOnTick(entt::registry& registry,
                            const Luminumbra::world::RegionEntityGate& gate,
                            const Luminumbra::world::RegionEntityWork& completed) {
    using namespace Luminumbra::world;
    using Luminumbra::Components::ActionPlanComponent;
    using Luminumbra::Components::LocomotionIntentComponent;
    using Luminumbra::Components::LocomotionPathComponent;
    using Luminumbra::Components::LocomotionProfile;
    using Luminumbra::Components::TransformComponent;

    InstinctLocomotionTickStats stats;

    // Collect agents and visit them in a deterministic id order so the outcome
    // never depends on registry storage internals (mirrors InstinctSystem's
    // sort discipline). entt entity ids are creation-ordered integers.
    std::vector<entt::entity> agents;
    {
        auto view =
            registry.view<ActionPlanComponent, const TransformComponent, const LocomotionProfile>();
        for (auto entity : view) {
            agents.push_back(entity);
        }
    }
    std::sort(agents.begin(), agents.end(), [](entt::entity lhs, entt::entity rhs) {
        return entt::to_integral(lhs) < entt::to_integral(rhs);
    });

    FilterRegionEntities(registry, agents, gate);

    // snapshot of all locomotion agents (position + PRIOR-tick heading),
    // in deterministic id order, for the Reynolds flocking terms below
    // (separation / cohesion / alignment). Built ONCE per tick BEFORE the per-agent
    // wish is recomputed, so alignment reads last tick's velocities — there is no
    // intra-tick read-after-write coupling and the result is order-independent.
    // Shipping profiles enable separation, cohesion and alignment by default.
    std::vector<AgentPos> neighbors;
    neighbors.reserve(agents.size());
    float largest_radius = 0.0f;
    for (auto entity : agents) {
        const auto& tf = registry.get<const TransformComponent>(entity);
        const auto* prev = registry.try_get<LocomotionIntentComponent>(entity);
        const float pwx = prev ? prev->wish_xz.x : 0.0f;
        const float pwz = prev ? prev->wish_xz.y : 0.0f;
        neighbors.push_back({entity, tf.position.x, tf.position.z, pwx, pwz});
        const auto& neighbor_profile = registry.get<const LocomotionProfile>(entity);
        for (const float radius :
             {neighbor_profile.separation_radius, neighbor_profile.flock_radius}) {
            if (std::isfinite(radius) && radius > largest_radius) {
                largest_radius = radius;
            }
        }
    }
    // `agents` is already id-sorted, so `neighbors` is too (deterministic scan).
    const NeighborGrid grid(neighbors, largest_radius);
    std::vector<std::size_t> candidates;

    for (auto entity : agents) {
        if (completed)
            completed(entity);
        ++stats.agents_seen;

        auto& plan = registry.get<ActionPlanComponent>(entity);
        const auto& tf = registry.get<const TransformComponent>(entity);
        const auto& profile = registry.get<const LocomotionProfile>(entity);

        auto& intent = registry.get_or_emplace<LocomotionIntentComponent>(entity);
        intent.wish_xz = Luminumbra::Vec2(0.0f);
        intent.arrived = false;

        // No current action (empty plan or index past the end) -> hold/idle.
        if (plan.plan.empty() || plan.current_action_index >= plan.plan.size()) {
            ++stats.agents_idle;
            continue;
        }

        const Luminumbra::EntityID target = plan.plan[plan.current_action_index].target;
        if (target == entt::null || !registry.valid(target) ||
            !registry.all_of<TransformComponent>(target) || !AdmitRegionEntity(gate, target)) {
            ++stats.agents_idle;
            continue;
        }

        const auto& target_tf = registry.get<const TransformComponent>(target);
        const bool flee = plan.plan[plan.current_action_index].flee;

        //  path-following: when the agent carries a LocomotionPathComponent
        // with a remaining waypoint (and is not fleeing), seek the WAYPOINT instead
        // of the action target directly. On waypoint arrival the index advances;
        // once the route is exhausted the agent resumes the plain action-target
        // seek so it still finishes onto its goal. No component -> byte-identical.
        auto* path = registry.try_get<LocomotionPathComponent>(entity);
        const bool following = !flee && path != nullptr && path->index < path->waypoints.size();
        const float goal_x = following ? path->waypoints[path->index].x : target_tf.position.x;
        const float goal_z = following ? path->waypoints[path->index].y : target_tf.position.z;

        // Horizontal vector to the goal (XZ plane; Y handled by the character's
        // ground-stick in physics). Float-only; Sqrt is IEEE-deterministic.
        const float dx = goal_x - tf.position.x;
        const float dz = goal_z - tf.position.z;
        const float dist = Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);

        const float slow = profile.slow_radius > profile.arrival_radius ? profile.slow_radius
                                                                        : profile.arrival_radius;

        if (flee) {
            //  flee/avoidance: move AWAY from the (threat) target until
            // beyond slow_radius (the "safe" distance), then the action completes.
            const float safe = slow > 0.0f ? slow : profile.arrival_radius;
            if (dist >= safe) {
                intent.arrived = true; // safe: stop fleeing, advance the plan
                ++stats.agents_arrived;
                ++plan.current_action_index;
                plan.time_in_current_action = 0.0f;
                continue;
            }
            // Full-speed flight directly away. Degenerate dist~0 (agent on top of
            // the threat) picks a deterministic +X so the wish is never NaN.
            float adx = dx, adz = dz;
            if (dist < 1e-4f) {
                adx = -1.0f;
                adz = 0.0f;
            } // away = -dir; -(-1)=+X
            const float ainv = 1.0f / Luminumbra::DeterministicMath::Sqrt(adx * adx + adz * adz);
            intent.wish_xz = Luminumbra::Vec2(-adx * ainv * profile.move_speed,
                                              -adz * ainv * profile.move_speed);
        } else {
            if (dist <= profile.arrival_radius) {
                if (following) {
                    // Reached this waypoint: advance the route (next tick seeks the
                    // next waypoint, or the action target once the path runs out).
                    // The PLAN is not advanced here — the action completes only on
                    // arrival at its real target, after the path is exhausted.
                    ++path->index;
                    continue;
                }
                // Arrived at the action target: hold and advance the plan.
                intent.arrived = true;
                ++stats.agents_arrived;
                ++plan.current_action_index;
                plan.time_in_current_action = 0.0f;
                continue;
            }
            // Seek + arrival ramp (Reynolds): full cruise outside slow_radius,
            // linear ramp toward zero between slow_radius and arrival_radius.
            float speed = profile.move_speed;
            if (dist < slow && slow > 0.0f) {
                speed = profile.move_speed * (dist / slow);
            }
            const float inv = 1.0f / dist; // dist > arrival_radius >= 0 -> safe
            intent.wish_xz = Luminumbra::Vec2(dx * inv * speed, dz * inv * speed);
        }

        //  obstacle/crowd avoidance (Reynolds separation), DATA-FLAGGED.
        // When separation is enabled, push away from nearby agents so they do not
        // pile onto a shared target at scale. Deterministic: neighbors are scanned
        // in id order; float-only; Sqrt is IEEE-deterministic. An explicit zero
        // strength skips this contribution; shipping profiles enable it.
        if (profile.separation_strength > 0.0f && profile.separation_radius > 0.0f) {
            const float sep_r = profile.separation_radius;
            float push_x = 0.0f;
            float push_z = 0.0f;
            VisitNeighbors(grid,
                           neighbors,
                           tf.position.x,
                           tf.position.z,
                           sep_r,
                           candidates,
                           stats,
                           [&](const AgentPos& nb) {
                               if (nb.id == entity) {
                                   return;
                               }
                               const float nx = tf.position.x - nb.x;
                               const float nz = tf.position.z - nb.z;
                               const float nd =
                                   Luminumbra::DeterministicMath::Sqrt(nx * nx + nz * nz);
                               if (nd > 0.0f && nd < sep_r) {
                                   // Linear falloff: closer neighbors push harder. Normalize the
                                   // away direction (1/nd) then weight by (1 - nd/sep_r).
                                   const float w = (sep_r - nd) / (sep_r * nd);
                                   push_x += nx * w;
                                   push_z += nz * w;
                               }
                           });
            // Blend the avoidance into the seek wish, scaled to the cruise speed so
            // it is commensurate with the seek term, then clamp the combined wish to
            // move_speed so avoidance never exceeds the agent's locomotion budget.
            intent.wish_xz.x += push_x * profile.separation_strength * profile.move_speed;
            intent.wish_xz.y += push_z * profile.separation_strength * profile.move_speed;
            const float wmag = Luminumbra::DeterministicMath::Sqrt(
                intent.wish_xz.x * intent.wish_xz.x + intent.wish_xz.y * intent.wish_xz.y);
            if (wmag > profile.move_speed && wmag > 0.0f) {
                const float s = profile.move_speed / wmag;
                intent.wish_xz.x *= s;
                intent.wish_xz.y *= s;
            }
        }

        //  flocking/herding: cohesion (steer toward the local group's center
        // of mass) + alignment (match the group's mean heading). Both use
        // flock_radius and last tick's neighbor headings (snapshotted above), so the
        // result is deterministic and order-independent. Explicit zero strength
        // skips its contribution. Composed on top of seek + separation;
        // the combined wish is then clamped to the move_speed budget.
        if ((profile.cohesion_strength > 0.0f || profile.alignment_strength > 0.0f) &&
            profile.flock_radius > 0.0f) {
            const float fr = profile.flock_radius;
            float cx = 0.0f, cz = 0.0f; // sum of neighbor positions (cohesion)
            float hx = 0.0f, hz = 0.0f; // sum of neighbor headings (alignment)
            int count = 0;
            VisitNeighbors(grid,
                           neighbors,
                           tf.position.x,
                           tf.position.z,
                           fr,
                           candidates,
                           stats,
                           [&](const AgentPos& nb) {
                               if (nb.id == entity)
                                   return;
                               const float nx = nb.x - tf.position.x;
                               const float nz = nb.z - tf.position.z;
                               const float nd =
                                   Luminumbra::DeterministicMath::Sqrt(nx * nx + nz * nz);
                               if (nd > 0.0f && nd < fr) {
                                   cx += nb.x;
                                   cz += nb.z;
                                   hx += nb.wx;
                                   hz += nb.wz;
                                   ++count;
                               }
                           });
            if (count > 0) {
                const float invn = 1.0f / static_cast<float>(count);
                if (profile.cohesion_strength > 0.0f) {
                    // Toward the neighbor centroid (unit) * strength * cruise speed.
                    const float ccx = cx * invn - tf.position.x;
                    const float ccz = cz * invn - tf.position.z;
                    const float cl = Luminumbra::DeterministicMath::Sqrt(ccx * ccx + ccz * ccz);
                    if (cl > 0.0f) {
                        const float cs = profile.cohesion_strength * profile.move_speed / cl;
                        intent.wish_xz.x += ccx * cs;
                        intent.wish_xz.y += ccz * cs;
                    }
                }
                if (profile.alignment_strength > 0.0f) {
                    // Toward the mean neighbor heading (unit) * strength * cruise speed.
                    const float mhx = hx * invn;
                    const float mhz = hz * invn;
                    const float al = Luminumbra::DeterministicMath::Sqrt(mhx * mhx + mhz * mhz);
                    if (al > 0.0f) {
                        const float as = profile.alignment_strength * profile.move_speed / al;
                        intent.wish_xz.x += mhx * as;
                        intent.wish_xz.y += mhz * as;
                    }
                }
                const float wmag = Luminumbra::DeterministicMath::Sqrt(
                    intent.wish_xz.x * intent.wish_xz.x + intent.wish_xz.y * intent.wish_xz.y);
                if (wmag > profile.move_speed && wmag > 0.0f) {
                    const float s = profile.move_speed / wmag;
                    intent.wish_xz.x *= s;
                    intent.wish_xz.y *= s;
                }
            }
        }
        ++stats.agents_steered;
    }

    return stats;
}

} // namespace luminumbra::ai
