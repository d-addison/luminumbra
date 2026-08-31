//  E5: sensory-trait DIVERGENCE under selection + scale determinism. Proves the
// sensory genes are a directional substrate (predator cones narrow, prey cones widen under opposing
// selection) and that the creature brain tick is deterministic at the 20-48 agent scale.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CreatureBrainSystem.h"
#include "ai/CreatureGenome.h"
#include "ai/CreatureTelemetry.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::BreedSensoryInto;
using luminumbra::ai::ComputeSensoryMeans;
using luminumbra::ai::CreatureGenome;
using luminumbra::ai::RunCreatureBrainSystemOnTick;

// Evolve a 16-genome population for `gens` generations, each generation keeping the 8 most extreme
// in the selected direction (narrow = highest cos cone, wide = lowest) and breeding them to refill.
// Returns the final mean vision_cos_half_fov. Deterministic (seeded rng).
double evolveMeanCos(bool selectNarrow, int gens) {
    std::vector<CreatureGenome> pop;
    for (int i = 0; i < 16; ++i) {
        CreatureGenome g;
        g.vision_cos_half_fov = 0.3f + 0.6f * static_cast<float>(i) / 15.0f; // spread [0.3, 0.9]
        pop.push_back(g);
    }
    auto rng = luminumbra::core::DeterministicRng::seeded(selectNarrow ? 11u : 22u, 0, 0);
    for (int gen = 0; gen < gens; ++gen) {
        std::sort(pop.begin(), pop.end(), [&](const CreatureGenome& a, const CreatureGenome& b) {
            return selectNarrow ? a.vision_cos_half_fov > b.vision_cos_half_fov
                                : a.vision_cos_half_fov < b.vision_cos_half_fov;
        });
        pop.resize(8); // survivors
        std::vector<CreatureGenome> next = pop;
        for (int i = 0; i < 8; ++i)
            next.push_back(BreedSensoryInto(CreatureGenome{}, pop[i], pop[(i + 1) % 8], rng));
        pop = next;
    }
    double s = 0.0;
    for (const auto& g : pop)
        s += g.vision_cos_half_fov;
    return s / static_cast<double>(pop.size());
}

// Opposing selection makes predator and prey vision cones DIVERGE across generations.
TEST(CreatureDivergence, SensoryGenesDivergeUnderSelection) {
    const double start = 0.6; // mean of the initial [0.3, 0.9] spread
    const double predMean =
        evolveMeanCos(/*selectNarrow=*/true, 8); // predators: narrow cone (high cos)
    const double preyMean = evolveMeanCos(/*selectNarrow=*/false, 8); // prey: wide cone (low cos)
    EXPECT_GT(predMean, start) << "selecting narrow cones raises the predator mean cos";
    EXPECT_LT(preyMean, start) << "selecting wide cones lowers the prey mean cos";
    EXPECT_GT(predMean - preyMean, 0.2) << "predator/prey cones diverge under opposing selection";
}

// Telemetry reads the mean sensory genes split by role.
TEST(CreatureDivergence, TelemetryMeansSplitByRole) {
    entt::registry r;
    auto spawn = [&](bool predator, float cos) {
        auto e = r.create();
        r.emplace<Comp::TransformComponent>(e);
        auto& cr = r.emplace<Comp::CreatureComponent>(e);
        cr.is_predator = predator;
        auto& gn = r.emplace<Comp::CreatureGenomeComponent>(e);
        gn.vision_cos_half_fov = cos;
    };
    spawn(true, 0.90f);
    spawn(true, 0.80f); // narrow-coned predators
    spawn(false, 0.30f);
    spawn(false, 0.40f); // wide-coned prey
    const auto pred = ComputeSensoryMeans(r, true);
    const auto prey = ComputeSensoryMeans(r, false);
    EXPECT_EQ(pred.count, 2u);
    EXPECT_EQ(prey.count, 2u);
    EXPECT_NEAR(pred.vision_cos_half_fov, 0.85, 1e-6);
    EXPECT_NEAR(prey.vision_cos_half_fov, 0.35, 1e-6);
    EXPECT_GT(pred.vision_cos_half_fov, prey.vision_cos_half_fov);
}

// Scale + determinism: the creature brain tick over 40 agents is a pure function (run == replay).
TEST(CreatureDivergence, BrainScaleDeterminismFortyAgents) {
    auto run = [] {
        entt::registry r;
        for (int i = 0; i < 40; ++i) {
            auto e = r.create();
            auto& tf = r.emplace<Comp::TransformComponent>(e);
            // Deterministic varied positions (golden-angle scatter), no RNG.
            const float a = 2.39996323f * static_cast<float>(i);
            tf.position = Luminumbra::Vec3(20.0f * Luminumbra::DeterministicMath::Cos(a),
                                           0.0f,
                                           20.0f * Luminumbra::DeterministicMath::Sin(a));
            auto& cr = r.emplace<Comp::CreatureComponent>(e);
            cr.is_predator = (i % 3 == 0); // ~1/3 predators
            cr.hunger = 0.5f;
            cr.stamina = 1.0f;
        }
        for (int t = 0; t < 5; ++t)
            RunCreatureBrainSystemOnTick(r, 1.0f / 30.0f);
        std::vector<float> out;
        std::vector<entt::entity> es(r.view<Comp::CreatureComponent>().begin(),
                                     r.view<Comp::CreatureComponent>().end());
        std::sort(es.begin(), es.end(), [](entt::entity a, entt::entity b) {
            return entt::to_integral(a) < entt::to_integral(b);
        });
        for (auto e : es) {
            const auto& cr = r.get<Comp::CreatureComponent>(e);
            out.push_back(cr.wish_x);
            out.push_back(cr.wish_z);
        }
        return out;
    };
    EXPECT_EQ(run(), run());
}

} // namespace
