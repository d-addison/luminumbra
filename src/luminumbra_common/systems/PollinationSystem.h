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
//   * id-ordered traversal (sorted by entt::to_integral) for receivers AND the
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
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "PlantGrowthSystem.h" // BreedPlants, ExpressGenome, namespace luminumbra::foliage, Comp alias

#include "../components/CoreComponents.h"
#include "../components/PlantComponents.h"
#include "../components/PollinationComponents.h"
#include "../core/DeterministicMath.h" // Sqrt (libm-free / IEEE-deterministic)
#include "../core/DeterministicRng.h"

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
    int considered = 0; // plants with the opt-in examined this tick
    int donors = 0;     // plants that were flowering/fruiting (eligible to donate)
    int crossed = 0;    // receivers that received pollen this tick
};

// A plant donates pollen only once it has reached a reproductive stage.
inline bool IsPollenDonor(const Comp::PlantGrowthComponent& g) {
    return g.stage == static_cast<std::uint8_t>(Comp::PlantStage::Flowering) ||
           g.stage == static_cast<std::uint8_t>(Comp::PlantStage::Fruiting);
}

// Wind-biased effective distance from donor `d` to receiver `r`. `wind_xz` is the
// wind direction*strength in the XZ plane (need not be normalized). With zero wind
// this is plain Euclidean XZ distance.
inline float PollinationEffectiveDistance(const ::Luminumbra::Vec3& d,
                                          const ::Luminumbra::Vec3& r,
                                          const ::Luminumbra::Vec2& wind_xz) {
    const float dx = r.x - d.x;
    const float dz = r.z - d.z;
    const float dist = ::Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);

    // Project (r - d) onto the wind direction. Normalize the wind with the
    // deterministic Sqrt; zero wind => zero bias.
    const float wlen =
        ::Luminumbra::DeterministicMath::Sqrt(wind_xz.x * wind_xz.x + wind_xz.y * wind_xz.y);
    if (wlen <= 0.0f)
        return dist;
    const float wx = wind_xz.x / wlen;
    const float wz = wind_xz.y / wlen;     // wind_xz.y is the Z component
    const float along = dx * wx + dz * wz; // >0 => receiver is downwind of donor
    float eff = dist - kPollinationWindReach * along;
    return eff < 0.0f ? 0.0f : eff;
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

// Advance pollination by one fixed tick. Pure function of registry state + the tick
// (the RNG seed). Returns telemetry counts. `wind_xz` biases donor reach (defaults
// to still air). `world_seed` lets distinct worlds diverge.
inline PollinationStats RunPollinationOnTick(entt::registry& reg,
                                             std::uint64_t tick,
                                             ::Luminumbra::Vec2 wind_xz = ::Luminumbra::Vec2(0.0f),
                                             std::uint64_t world_seed = 0,
                                             float mutation_frac = kPollinationMutationFrac) {
    PollinationStats stats;

    auto view = reg.view<Comp::PollinationTag,
                         Comp::PlantGenomeComponent,
                         Comp::PlantGrowthComponent,
                         Comp::TransformComponent,
                         Comp::PollinationComponent>();

    // id-ordered participant ids so donor-selection ties + apply order are deterministic.
    std::vector<entt::entity> ents;
    for (auto e : view)
        ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });

    //  (read): for each receiver pick its best donor (smallest wind-biased
    // effective distance within reach; ties -> lowest donor id). Snapshot the cross.
    struct Cross {
        entt::entity receiver;
        std::uint64_t receiver_id;
        std::uint64_t donor_id;
        Comp::PlantGenomeComponent receiver_genome;
        Comp::PlantGenomeComponent donor_genome;
    };
    std::vector<Cross> crosses;

    for (auto r : ents) {
        ++stats.considered;
        const std::uint64_t rid = static_cast<std::uint64_t>(entt::to_integral(r));
        const auto& r_tf = view.get<Comp::TransformComponent>(r);
        const auto& r_genome = view.get<Comp::PlantGenomeComponent>(r);

        entt::entity best_donor = entt::null;
        std::uint64_t best_donor_id = 0;
        float best_eff = kPollinationRadius; // strictly within reach to qualify
        bool found = false;

        for (auto d : ents) { // already id-ordered -> deterministic tie-break
            if (d == r)
                continue;
            const std::uint64_t did = static_cast<std::uint64_t>(entt::to_integral(d));
            const auto& d_growth = view.get<Comp::PlantGrowthComponent>(d);
            if (!IsPollenDonor(d_growth))
                continue; // only reproductive plants donate

            const auto& d_tf = view.get<Comp::TransformComponent>(d);
            const float eff = PollinationEffectiveDistance(d_tf.position, r_tf.position, wind_xz);
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

    // Count donors for telemetry (independent of whether they pollinated anyone).
    for (auto d : ents) {
        if (IsPollenDonor(view.get<Comp::PlantGrowthComponent>(d)))
            ++stats.donors;
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

} // namespace luminumbra::foliage
