#pragma once

// Deterministic real-valued genetic-algorithm operators for the emergent ecology
// model. A genome is a small vector
// of real traits (move_speed, boids weights, flee threshold, FOV/vision range,
// hearing audiogram fields, scent sensitivity/deposit). The standard, reproducible
// recipe (GA practice): TOURNAMENT selection + GAUSSIAN mutation + optional blend
// crossover + ELITISM, modest population, many generations.
//
// PURE + DETERMINISTIC: every stochastic draw comes from a caller-supplied seeded
// DeterministicRng (no wall-clock/global RNG), the Gaussian is the libm-free
// Irwin-Hall one, and elitism sorts by (fitness desc, index asc) so ties are
// stable. Same seed + same inputs -> identical next generation (run==replay).
// Reproduction systems fold their resulting state into the world hash.

#include <algorithm>
#include <cstddef>
#include <vector>

#include "../core/DeterministicMath.h"
#include "../core/DeterministicRng.h"

namespace luminumbra::ai {

// Inclusive trait range; genes are clamped here after mutation/crossover.
struct GeneBound {
    float lo = 0.0f;
    float hi = 1.0f;
};

inline float ClampGene(float v, const GeneBound& b) {
    return v < b.lo ? b.lo : (v > b.hi ? b.hi : v);
}

// Per-gene Gaussian mutation: gene += N(0,1) * sigma_frac * (hi-lo), then clamp.
// sigma_frac is the mutation step as a FRACTION of each gene's range (5-10% typical).
// sigma_frac <= 0 is a no-op.
inline void GaussianMutate(std::vector<float>& genome,
                           const std::vector<GeneBound>& bounds,
                           float sigma_frac,
                           luminumbra::core::DeterministicRng& rng) {
    const std::size_t n = std::min(genome.size(), bounds.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (sigma_frac > 0.0f) {
            const float range = bounds[i].hi - bounds[i].lo;
            genome[i] += rng.next_gaussian() * sigma_frac * range;
        }
        genome[i] = ClampGene(genome[i], bounds[i]);
    }
}

// Arithmetic/blend crossover: child[i] = lerp(a[i], b[i], u), u ~ U[0,1) per gene.
// The child always lies within [min(a,b), max(a,b)] per gene (no clamp needed if
// parents are in-bounds).
inline std::vector<float> BlendCrossover(const std::vector<float>& a,
                                         const std::vector<float>& b,
                                         luminumbra::core::DeterministicRng& rng) {
    const std::size_t n = std::min(a.size(), b.size());
    std::vector<float> child(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float u = rng.next_unit();
        child[i] = a[i] + (b[i] - a[i]) * u;
    }
    return child;
}

// Tournament selection: sample `k` contestants uniformly (with replacement) and
// return the index of the fittest (ties -> lowest index, deterministic). k<=1 is a
// single random pick. Higher k -> stronger selection pressure.
inline std::size_t TournamentSelect(const std::vector<float>& fitness,
                                    int k,
                                    luminumbra::core::DeterministicRng& rng) {
    const std::size_t n = fitness.size();
    if (n == 0)
        return 0;
    const int rounds = k < 1 ? 1 : k;
    std::size_t best = static_cast<std::size_t>(rng.next_u64() % n);
    for (int r = 1; r < rounds; ++r) {
        const std::size_t c = static_cast<std::size_t>(rng.next_u64() % n);
        if (fitness[c] > fitness[best])
            best = c;
    }
    return best;
}

// Produce the next generation from `pop` + per-genome `fitness`:
//   * ELITISM: the top `elitism` genomes (by fitness desc, index asc) carry over
//     unchanged — guarantees the best solution never regresses.
//   * the rest: parentA = tournament, parentB = tournament, child = blend-crossover,
//     then Gaussian mutate + clamp.
// Output size equals pop.size. Deterministic for a given rng state + inputs.
inline std::vector<std::vector<float>> EvolveGeneration(const std::vector<std::vector<float>>& pop,
                                                        const std::vector<float>& fitness,
                                                        const std::vector<GeneBound>& bounds,
                                                        float sigma_frac,
                                                        int elitism,
                                                        int tournament_k,
                                                        luminumbra::core::DeterministicRng& rng) {
    const std::size_t n = pop.size();
    std::vector<std::vector<float>> next;
    if (n == 0)
        return next;
    next.reserve(n);

    // Elite indices: stable sort by fitness desc, then index asc.
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (fitness[a] != fitness[b])
            return fitness[a] > fitness[b];
        return a < b;
    });
    const std::size_t elite = std::min(static_cast<std::size_t>(elitism < 0 ? 0 : elitism), n);
    for (std::size_t e = 0; e < elite; ++e)
        next.push_back(pop[order[e]]);

    while (next.size() < n) {
        const std::size_t pa = TournamentSelect(fitness, tournament_k, rng);
        const std::size_t pb = TournamentSelect(fitness, tournament_k, rng);
        std::vector<float> child = BlendCrossover(pop[pa], pop[pb], rng);
        GaussianMutate(child, bounds, sigma_frac, rng);
        next.push_back(std::move(child));
    }
    return next;
}

} // namespace luminumbra::ai
