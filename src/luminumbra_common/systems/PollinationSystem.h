#pragma once

// sim.pollination — DETERMINISTIC cross-pollination of neighbouring plants so a
// field's genetics DRIFT over seasons. A flowering / fruiting plant ("donor")
// pollinates nearby plants; the receiver's NEXT-generation genome becomes a
// deterministic BLEND (the existing PlantGrowthSystem BreedPlants crossover) of
// the two parents. WIND biases which neighbour donates — pollen carries further
// DOWNWIND (the effective distance shrinks along the wind vector, exactly the way
// fire spreads downwind) — but the blend RNG itself is seeded from the SORTED pair
// of ids so the cross A<->B is symmetric and order-independent.
//
// DETERMINISM (this feeds world_hash):
//   * persistent-id traversal (legacy plants follow in handle order) for receivers AND the
//     donor scan, so donor selection ties resolve to the lowest id deterministically.
//   * the per-cross RNG is seeded ONLY from integers — DeterministicRng::seeded(
//     kPollinationSeedOffset, lower_id, higher_id, tick) — pair-symmetric (sorted
//     ids) so A receiving from B and B receiving from A draw the IDENTICAL blend.
//   * distances use Luminumbra::DeterministicMath::Sqrt (the one IEEE-deterministic
//     transcendental); NO libm exp/log/pow, NO wall-clock, NO std::random.
//   * two-phase: a READ pass snapshots every (receiver, donor) cross, then an APPLY
//     pass writes the new PollinationComponent state — the view is never mutated
//     mid-iteration and apply order is id-deterministic.
//
// GATING: a plant participates only if it carries PollinationTag (the opt-in) plus
// the plant genome / growth / transform it needs. A registry with NO PollinationTag
// and in particular a world with no plants — performs ZERO work and writes NOTHING,
// so the canonical NetworkStateHash baseline stays byte-identical.
//
// ADDITIVE: the result is stored in a NEW PollinationComponent (next_genome); the
// live PlantGenomeComponent is never written here. (See PollinationComponents.h.)
//
// SEED OFFSET REGISTRY: wind+11, weather+12/13, aether+14, plant+15,
// creature-reproduction+16 are taken. POLLINATION CLAIMS +19.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include <entt/entity/registry.hpp>

#include "PlantGrowthSystem.h" // BreedPlants, ExpressGenome, namespace luminumbra::foliage, Comp alias

#include "../components/CoreComponents.h"
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h"
#include "../core/DeterministicMath.h" // Sqrt (libm-free / IEEE-deterministic)
#include "../core/DeterministicRng.h"
#include "../simulation/DeterministicUniformGrid.h"

#include "RegionConsumerGate.h"
#include "RegionalPlantTicks.h"

namespace luminumbra::foliage {

// The component types live in the capital-L Luminumbra::Components namespace.
namespace Comp = ::Luminumbra::Components;

// Distinct world-seed offset for the pollination blend stream (see registry above).
inline constexpr std::uint64_t kPollinationSeedOffset = 19ull;

// Pollen reach (metres) of a donor under STILL air. Wind extends/contracts this
// directionally (see kPollinationWindReach). Plants beyond this never cross.
inline constexpr float kPollinationRadius = 6.0f;

// How strongly wind biases reach. The effective distance from donor D to receiver R
// is the geometric distance MINUS this fraction of the component of (R - D) along
// the wind direction: a receiver downwind of the donor is "closer" (longer reach),
// upwind is "further" (shorter reach). Pure projection arithmetic — deterministic.
inline constexpr float kPollinationWindReach = 0.5f;

// Mutation step applied during the blend (matches BreedPlants' default). Kept small
// so drift is gradual across seasons rather than a jump.
inline constexpr float kPollinationMutationFrac = 0.05f;

struct PollinationStats {
    int considered = 0;                // plants with the opt-in examined this tick
    int donors = 0;                    // plants that were flowering/fruiting (eligible to donate)
    int crossed = 0;                   // receivers that received pollen this tick
    std::uint64_t candidate_tests = 0; // broad-phase source inspections (or fallback pairs)
};

// A plant donates pollen only once it has reached a reproductive stage.
inline bool IsPollenDonor(const Comp::PlantGrowthComponent& g) {
    return g.stage == static_cast<std::uint8_t>(Comp::PlantStage::Flowering) ||
           g.stage == static_cast<std::uint8_t>(Comp::PlantStage::Fruiting);
}

struct PollinationWind {
    ::Luminumbra::Vec2 direction{0.0f};
    bool active = false;
    bool indexable = true;
};

inline PollinationWind NormalizePollinationWind(const ::Luminumbra::Vec2& wind_xz) {
    const float wlen =
        ::Luminumbra::DeterministicMath::Sqrt(wind_xz.x * wind_xz.x + wind_xz.y * wind_xz.y);
    if (wlen <= 0.0f) {
        return {};
    }
    // Preserve the original divisions, including exceptional float behaviour.
    // Subnormal squared lengths can enlarge the normalized vector appreciably;
    // use the original scan in that domain instead of assuming a unit vector.
    return {{wind_xz.x / wlen, wind_xz.y / wlen}, true, std::isfinite(wlen) && wlen >= 1.0e-18f};
}

inline float PollinationDistanceWithWind(const ::Luminumbra::Vec3& d,
                                         const ::Luminumbra::Vec3& r,
                                         const PollinationWind& wind) {
    const float dx = r.x - d.x;
    const float dz = r.z - d.z;
    const float dist = ::Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);

    if (!wind.active)
        return dist;
    const float wx = wind.direction.x;
    const float wz = wind.direction.y;     // wind_xz.y is the Z component
    const float along = dx * wx + dz * wz; // >0 => receiver is downwind of donor
    float eff = dist - kPollinationWindReach * along;
    return eff < 0.0f ? 0.0f : eff;
}

// Wind-biased effective distance; unnormalized wind uses the original pair math.
inline float PollinationEffectiveDistance(const ::Luminumbra::Vec3& d,
                                          const ::Luminumbra::Vec3& r,
                                          const ::Luminumbra::Vec2& wind_xz) {
    return PollinationDistanceWithWind(d, r, NormalizePollinationWind(wind_xz));
}

struct PollinationSpatialScratch {
    simulation::DeterministicUniformGrid grid;
    std::vector<simulation::DeterministicUniformGrid::Point> points;
    std::vector<entt::entity> donors;
};

inline bool PollinationGridPoint(const ::Luminumbra::Vec3& position,
                                 simulation::DeterministicUniformGrid::Point& point) {
    // A deliberately bounded integer projection. Other float inputs retain the
    // original scan, including NaNs and subtraction/square overflow behaviour.
    constexpr float coordinate_limit = 536870912.0f;
    if (!std::isfinite(position.x) || !std::isfinite(position.z) ||
        std::abs(position.x) > coordinate_limit || std::abs(position.z) > coordinate_limit) {
        return false;
    }
    point = {static_cast<std::int64_t>(std::floor(position.x)),
             static_cast<std::int64_t>(std::floor(position.z))};
    return true;
}

// Build the pair seed from the SORTED ids so the cross is symmetric/order-independent.
inline luminumbra::core::DeterministicRng PollinationPairRng(std::uint64_t id_a,
                                                             std::uint64_t id_b,
                                                             std::uint64_t tick,
                                                             std::uint64_t world_seed = 0) {
    const std::uint64_t lo = id_a < id_b ? id_a : id_b;
    const std::uint64_t hi = id_a < id_b ? id_b : id_a;
    // seeded mixes (a,b,c); fold tick + world_seed into `a` so all four integers
    // contribute. Pair-symmetric because (lo, hi) are sorted.
    return luminumbra::core::DeterministicRng::seeded(
        kPollinationSeedOffset ^ world_seed ^ tick, lo, hi);
}

// The deterministic, pair-symmetric cross of two plant genomes. Public so tests (and
// callers) can assert symmetry directly without spinning a registry.
inline Comp::PlantGenomeComponent PollinateCross(const Comp::PlantGenomeComponent& receiver,
                                                 const Comp::PlantGenomeComponent& donor,
                                                 std::uint64_t receiver_id,
                                                 std::uint64_t donor_id,
                                                 std::uint64_t tick,
                                                 std::uint64_t world_seed = 0,
                                                 float mutation_frac = kPollinationMutationFrac) {
    luminumbra::core::DeterministicRng rng =
        PollinationPairRng(receiver_id, donor_id, tick, world_seed);
    // Order the parents by id too, so the BlendCrossover draw order (and thus the
    // result) is identical regardless of who is the receiver vs the donor.
    const bool receiver_first = receiver_id < donor_id;
    const Comp::PlantGenomeComponent& a = receiver_first ? receiver : donor;
    const Comp::PlantGenomeComponent& b = receiver_first ? donor : receiver;
    return BreedPlants(a, b, rng, mutation_frac);
}

// Collect admitted participants in identity order with storage bounded by the view.
inline std::vector<entt::entity>
CollectPollinationParticipants(entt::registry& reg,
                               const ::Luminumbra::world::RegionConsumerGate& gate) {
    auto view = reg.view<Comp::PollinationTag,
                         Comp::PlantGenomeComponent,
                         Comp::PlantGrowthComponent,
                         Comp::TransformComponent,
                         Comp::PollinationComponent>();

    // id-ordered participant ids so donor-selection ties + apply order are deterministic.
    std::vector<entt::entity> ents;
    ents.reserve(view.size_hint()); // Bounded by the smallest participating component pool.
    for (auto e : view) {
        if (gate.Allows(e)) {
            ents.push_back(e);
        }
    }
    if (gate.UsesSimulationPlantIdentity()) {
        OrderPlantsBySimulationIdentity(reg, ents, gate.PlantIdentityScratch());
    } else {
        std::sort(ents.begin(), ents.end(), [&reg, &gate](entt::entity a, entt::entity b) {
            return gate.UsesPersistentPlantIdentity() ? PlantIdentityLess(reg, a, b)
                                                      : entt::to_integral(a) < entt::to_integral(b);
        });
    }
    return ents;
}

namespace pollination_detail {

struct Cross {
    entt::entity receiver;
    std::uint64_t receiver_id;
    std::uint64_t donor_id;
    Comp::PlantGenomeComponent receiver_genome;
    Comp::PlantGenomeComponent donor_genome;
};

inline PollinationStats Run(entt::registry& reg,
                            std::span<const entt::entity> ents,
                            std::uint64_t tick,
                            const ::Luminumbra::Vec2& wind_xz,
                            std::uint64_t world_seed,
                            float mutation_frac,
                            const ::Luminumbra::world::RegionConsumerGate& gate,
                            std::vector<Cross>& crosses) {
    PollinationStats stats;
    auto view = reg.view<Comp::PollinationTag,
                         Comp::PlantGenomeComponent,
                         Comp::PlantGrowthComponent,
                         Comp::TransformComponent,
                         Comp::PollinationComponent>();
    if (ents.empty()) {
        return stats;
    }
    auto* scratch_pointer = reg.ctx().find<PollinationSpatialScratch>();
    if (scratch_pointer == nullptr) {
        scratch_pointer = &reg.ctx().emplace<PollinationSpatialScratch>();
    }
    auto& scratch = *scratch_pointer;
    scratch.points.clear();
    scratch.donors.clear();
    // Scratch is retained by this registry and bounded by admitted participants.
    scratch.points.reserve(ents.size());
    scratch.donors.reserve(ents.size());
    const auto wind = NormalizePollinationWind(wind_xz);
    bool indexed = wind.indexable;
    for (const auto entity : ents) {
        simulation::DeterministicUniformGrid::Point point;
        indexed =
            PollinationGridPoint(view.get<Comp::TransformComponent>(entity).position, point) &&
            indexed;
        if (IsPollenDonor(view.get<Comp::PlantGrowthComponent>(entity))) {
            scratch.points.push_back(point);
            scratch.donors.push_back(entity);
            ++stats.donors;
        }
    }
    if (scratch.donors.empty()) {
        for (const auto entity : ents) {
            (void)entity;
            ++stats.considered;
        }
        return stats;
    }
    if (indexed) {
        scratch.grid.Build(scratch.points, 16);
    }

    //  (read): for each receiver pick its best donor (smallest wind-biased
    // effective distance within reach; ties -> lowest donor id). Snapshot the cross.
    crosses.clear();
    crosses.reserve(ents.size()); // At most one cross per admitted receiver.

    const auto participant_id = [&reg, &gate](entt::entity entity) {
        if (gate.UsesSimulationPlantIdentity())
            return RequirePlantSimulationId(reg, entity);
        return gate.UsesPersistentPlantIdentity()
                   ? PlantSimulationId(reg, entity)
                   : static_cast<std::uint64_t>(entt::to_integral(entity));
    };
    for (auto r : ents) {
        ++stats.considered;
        const std::uint64_t rid = participant_id(r);
        const auto& r_tf = view.get<Comp::TransformComponent>(r);
        const auto& r_genome = view.get<Comp::PlantGenomeComponent>(r);

        entt::entity best_donor = entt::null;
        std::uint64_t best_donor_id = 0;
        float best_eff = kPollinationRadius; // strictly within reach to qualify
        bool found = false;

        std::span<const std::size_t> candidates;
        if (indexed) {
            simulation::DeterministicUniformGrid::Point center;
            (void)PollinationGridPoint(r_tf.position, center);
            // Exact still-air reach is 6; wind expands it to 6/(1-0.5)=12.
            // One integer metre covers floor projection and float boundary
            // rounding (normalization's squared length is normal here).
            static_assert(kPollinationRadius == 6.0f && kPollinationWindReach == 0.5f,
                          "Update the pollination query extents with the reach constants");
            const std::int64_t extent = wind.active ? 13 : 7;
            candidates = scratch.grid.Query({center.x - extent, center.z - extent},
                                            {center.x + extent, center.z + extent});
        }
        const auto count = indexed ? candidates.size() : scratch.donors.size();
        for (std::size_t candidate = 0; candidate < count; ++candidate) {
            const auto d = scratch.donors[indexed ? candidates[candidate] : candidate];
            if (!indexed) {
                ++stats.candidate_tests;
            }
            if (d == r)
                continue;
            const std::uint64_t did = participant_id(d);

            const auto& d_tf = view.get<Comp::TransformComponent>(d);
            const float eff = PollinationDistanceWithWind(d_tf.position, r_tf.position, wind);
            if (eff > kPollinationRadius)
                continue; // out of (still-air) reach
            // Strictly closer wins; equal effective distance -> lower id (ents order
            // means the first-seen equal candidate already has the lower id).
            if (!found || eff < best_eff) {
                found = true;
                best_eff = eff;
                best_donor = d;
                best_donor_id = did;
            }
        }

        if (!found)
            continue; // ISOLATED plant: no donor in reach -> unchanged.

        Cross c;
        c.receiver = r;
        c.receiver_id = rid;
        c.donor_id = best_donor_id;
        c.receiver_genome = r_genome;
        c.donor_genome = view.get<Comp::PlantGenomeComponent>(best_donor);
        crosses.push_back(c);
    }

    if (indexed) {
        stats.candidate_tests = scratch.grid.CandidateTests();
    }

    //  (apply): write the mixed next-generation genome onto each receiver's
    // PollinationComponent in id-deterministic order. NOTHING on existing components
    // is touched.
    for (const Cross& c : crosses) {
        auto& pc = view.get<Comp::PollinationComponent>(c.receiver);
        pc.next_genome = PollinateCross(c.receiver_genome,
                                        c.donor_genome,
                                        c.receiver_id,
                                        c.donor_id,
                                        tick,
                                        world_seed,
                                        mutation_frac);
        pc.pollinated = true;
        pc.last_pollen_tick = tick;
        if (pc.crosses < 0xFFFFFFFFu)
            ++pc.crosses;
        ++stats.crossed;
    }

    return stats;
}

} // namespace pollination_detail

// Legacy one-tick entry point deliberately ignores elapsed windows.
inline PollinationStats
RunPollinationOnTick(entt::registry& reg,
                     std::uint64_t tick,
                     ::Luminumbra::Vec2 wind_xz = ::Luminumbra::Vec2(0.0f),
                     std::uint64_t world_seed = 0,
                     float mutation_frac = kPollinationMutationFrac,
                     const ::Luminumbra::world::RegionConsumerGate& gate = {}) {
    const auto participants = CollectPollinationParticipants(reg, gate);
    std::vector<pollination_detail::Cross> crosses;
    return pollination_detail::Run(
        reg, participants, tick, wind_xz, world_seed, mutation_frac, gate, crosses);
}

// Sample wind/genomes/stages at due time. At each chronological virtual tick both
// donor and receiver must have an overlapping, nonzero owner window and be born.
// Each crossing uses that virtual tick for RNG and last_pollen_tick, increments
// crosses once (saturating), and contributes one event to the returned totals.
inline PollinationStats
RunPollinationRegional(entt::registry& reg,
                       std::uint64_t due_tick,
                       const ::Luminumbra::world::RegionConsumerGate& gate,
                       ::Luminumbra::Vec2 wind_xz = ::Luminumbra::Vec2(0.0f),
                       std::uint64_t world_seed = 0,
                       float mutation_frac = kPollinationMutationFrac) {
    const auto participants = CollectPollinationParticipants(reg, gate);
    plant_elapsed_detail::RegionalPlantTicks schedule(reg, participants, gate, due_tick);
    PollinationStats total;
    std::vector<pollination_detail::Cross> crosses;
    schedule.Run([&](std::span<const entt::entity> active, std::uint64_t tick, std::uint32_t) {
        const auto stats = pollination_detail::Run(
            reg, active, tick, wind_xz, world_seed, mutation_frac, gate, crosses);
        plant_elapsed_detail::AddCount(total.considered, stats.considered);
        plant_elapsed_detail::AddCount(total.donors, stats.donors);
        plant_elapsed_detail::AddCount(total.crossed, stats.crossed);
        total.candidate_tests += stats.candidate_tests;
    });
    return total;
}

} // namespace luminumbra::foliage
