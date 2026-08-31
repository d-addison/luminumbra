// deterministic GA operators coverage — mutation bounds, crossover range,
// tournament pressure, elitism, reproducibility, and end-to-end convergence
// (selection actually optimizes a fitness). Pure; world_hash-neutral.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/Evolution.h"
#include "luminumbra_common/core/DeterministicRng.h"

#include <vector>

namespace {
using luminumbra::ai::BlendCrossover;
using luminumbra::ai::EvolveGeneration;
using luminumbra::ai::GaussianMutate;
using luminumbra::ai::GeneBound;
using luminumbra::ai::TournamentSelect;
using luminumbra::core::DeterministicRng;

std::vector<GeneBound> Bounds4() {
    return {{-1, 1}, {-1, 1}, {-1, 1}, {-1, 1}};
}
} // namespace

TEST(Evolution, GaussianMutateStaysInBoundsAndZeroSigmaIsNoop) {
    DeterministicRng rng(1);
    auto bounds = Bounds4();
    std::vector<float> g = {0.0f, 0.9f, -0.9f, 0.5f};
    const std::vector<float> before = g;
    GaussianMutate(g, bounds, 0.0f, rng); // sigma 0 -> unchanged (still clamped)
    EXPECT_EQ(g, before);
    for (int it = 0; it < 1000; ++it) {
        GaussianMutate(g, bounds, 0.2f, rng);
        for (std::size_t i = 0; i < g.size(); ++i) {
            EXPECT_GE(g[i], bounds[i].lo);
            EXPECT_LE(g[i], bounds[i].hi);
        }
    }
}

TEST(Evolution, BlendCrossoverChildWithinParentRange) {
    DeterministicRng rng(2);
    std::vector<float> a = {0.0f, 1.0f, -1.0f, 0.2f};
    std::vector<float> b = {1.0f, 0.0f, 1.0f, 0.8f};
    for (int it = 0; it < 200; ++it) {
        auto c = BlendCrossover(a, b, rng);
        for (std::size_t i = 0; i < c.size(); ++i) {
            const float lo = std::min(a[i], b[i]);
            const float hi = std::max(a[i], b[i]);
            EXPECT_GE(c[i], lo - 1e-5f);
            EXPECT_LE(c[i], hi + 1e-5f);
        }
    }
}

TEST(Evolution, TournamentSelectionFavorsHigherFitness) {
    DeterministicRng rng(3);
    std::vector<float> fitness = {0.0f, 0.0f, 0.0f, 10.0f}; // index 3 dominates
    int picked_best = 0;
    for (int it = 0; it < 200; ++it)
        if (TournamentSelect(fitness, /*k=*/3, rng) == 3u)
            ++picked_best;
    EXPECT_GT(picked_best, 100); // a clear winner is selected the majority of the time
}

TEST(Evolution, ElitismCarriesBestUnchangedAndKeepsSize) {
    DeterministicRng rng(4);
    std::vector<std::vector<float>> pop = {{0.1f, 0.1f, 0.1f, 0.1f},
                                           {0.2f, 0.2f, 0.2f, 0.2f},
                                           {0.9f, 0.8f, 0.7f, 0.6f},
                                           {0.3f, 0.3f, 0.3f, 0.3f}};
    std::vector<float> fitness = {1.0f, 2.0f, 99.0f, 3.0f}; // index 2 best
    auto next = EvolveGeneration(pop,
                                 fitness,
                                 Bounds4(),
                                 0.1f,
                                 /*elitism=*/1,
                                 /*tournament_k=*/2,
                                 rng);
    ASSERT_EQ(next.size(), pop.size());
    EXPECT_EQ(next[0], pop[2]); // the elite is carried over byte-for-byte
}

TEST(Evolution, EvolveGenerationIsReproducible) {
    auto run = []() {
        DeterministicRng rng(2024);
        std::vector<std::vector<float>> pop = {{0.1f, 0.2f, 0.3f, 0.4f},
                                               {0.5f, 0.6f, 0.7f, 0.8f},
                                               {-0.1f, -0.2f, -0.3f, -0.4f},
                                               {0.0f, 0.0f, 0.0f, 0.0f}};
        std::vector<float> fitness = {1.0f, 4.0f, 2.0f, 3.0f};
        return EvolveGeneration(pop, fitness, Bounds4(), 0.1f, 1, 2, rng);
    };
    EXPECT_EQ(run(), run()); // identical seed + inputs -> identical next generation
}

TEST(Evolution, SelectionConvergesTowardATargetGenome) {
    // Fitness = -sum (gene - target)^2; GA should climb toward the target.
    DeterministicRng rng(777);
    const std::vector<float> target = {0.5f, -0.3f, 0.8f, -0.6f};
    auto bounds = Bounds4();
    auto fit = [&](const std::vector<float>& g) {
        float s = 0.0f;
        for (std::size_t i = 0; i < g.size(); ++i) {
            const float d = g[i] - target[i];
            s += d * d;
        }
        return -s;
    };

    const std::size_t P = 24;
    std::vector<std::vector<float>> pop(P, std::vector<float>(4));
    for (auto& g : pop)
        for (int i = 0; i < 4; ++i)
            g[static_cast<std::size_t>(i)] = rng.next_range(-1.0f, 1.0f);

    auto best_fitness = [&]() {
        float best = -1e30f;
        for (auto& g : pop)
            best = std::max(best, fit(g));
        return best;
    };
    const float initial_best = best_fitness();

    for (int gen = 0; gen < 60; ++gen) {
        std::vector<float> fitness(P);
        for (std::size_t i = 0; i < P; ++i)
            fitness[i] = fit(pop[i]);
        pop = EvolveGeneration(pop,
                               fitness,
                               bounds,
                               0.12f,
                               /*elitism=*/2,
                               /*tournament_k=*/3,
                               rng);
    }
    const float final_best = best_fitness();

    EXPECT_GT(final_best, initial_best); // selection improved the population
    EXPECT_GT(final_best, -0.05f);       // converged close to the target (fitness -> 0)
}
