// end-to-end emergent-ecology demos — compose the whole substrate
// (scent deposit/diffuse/evaporate/clamp + chemotaxis steering + boids locomotion +
// kinematic integration) and assert the EMERGENT outcomes: a predator hunts prey
// down a scent gradient, and a herd flocks together while travelling. Deterministic
// headless "watchable" proof that the systems work together (no GPU/physics).

#include "gtest/gtest.h"

#include "luminumbra_common/ai/Evolution.h"
#include "luminumbra_common/ai/InstinctLocomotionSystem.h"
#include "luminumbra_common/ai/ScentDepositSystem.h"
#include "luminumbra_common/ai/ScentField.h"
#include "luminumbra_common/ai/ScentSteeringSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/DeterministicRng.h"

#include <cmath>
#include <vector>

namespace {
using namespace Luminumbra::Components;
using luminumbra::ai::RunInstinctLocomotionOnTick;
using luminumbra::ai::RunScentDepositOnTick;
using luminumbra::ai::RunScentSteeringOnTick;
using luminumbra::ai::ScentField;

constexpr float kDt = 1.0f / 30.0f;

float Dist(const Luminumbra::Vec3& a, const Luminumbra::Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// Kinematic integrate: apply each agent's locomotion wish to its position.
void IntegrateWishes(entt::registry& r, float dt) {
    auto view = r.view<TransformComponent, const LocomotionIntentComponent>();
    for (auto e : view) {
        auto& tf = view.get<TransformComponent>(e);
        const auto& in = view.get<const LocomotionIntentComponent>(e);
        tf.position.x += in.wish_xz.x * dt;
        tf.position.z += in.wish_xz.y * dt;
    }
}
} // namespace

// Run one scent-hunting episode for a predator with the given genome (move_speed,
// weber_k, scent_strength) and return its fitness = -final distance to prey
// (closer = fitter). Pure + deterministic given the genome.
namespace {
float HuntFitness(const std::vector<float>& genome) {
    ScentField field(32, 32, 1);
    entt::registry r;
    const auto prey = r.create();
    r.emplace<TransformComponent>(prey).position = Luminumbra::Vec3(24.0f, 0.0f, 16.0f);
    {
        auto& s = r.emplace<SensableComponent>(prey);
        s.scent_channel = 0;
        s.scent_deposit = 80.0f;
    }
    const auto pred = r.create();
    r.emplace<TransformComponent>(pred).position = Luminumbra::Vec3(8.0f, 0.0f, 16.0f);
    {
        auto& sn = r.emplace<ScentSenseComponent>(pred);
        sn.channel = 0;
        sn.sign = 1.0f;
        sn.strength = genome[2];
        sn.floor = 1e-4f;
        sn.weber_k = genome[1];
    }
    r.emplace<LocomotionIntentComponent>(pred);
    r.emplace<LocomotionProfile>(pred).move_speed = genome[0];
    for (int t = 0; t < 150; ++t) {
        RunScentDepositOnTick(r, field, 0.0f, 0.0f, 1.0f);
        field.Step(0.25, 4, 0.004);
        field.Clamp(0.0, 1.0e6);
        RunScentSteeringOnTick(r, field, 0.0f, 0.0f, 1.0f);
        IntegrateWishes(r, kDt);
    }
    return -Dist(r.get<TransformComponent>(pred).position,
                 r.get<TransformComponent>(prey).position);
}
} // namespace

// Evolution loop over the ACTUAL hunting sim: a population of predator genomes is
// selected/mutated across generations and gets measurably better at hunting.
TEST(EcologyDemo, EvolvedPredatorsHuntBetter) {
    using luminumbra::ai::EvolveGeneration;
    using luminumbra::ai::GeneBound;
    using luminumbra::core::DeterministicRng;

    DeterministicRng rng(/*seed=*/4242);
    // genome = [move_speed, weber_k, scent_strength]
    const std::vector<GeneBound> bounds = {{2.0f, 12.0f}, {0.5f, 8.0f}, {0.2f, 2.0f}};
    const std::size_t P = 16;
    std::vector<std::vector<float>> pop(P, std::vector<float>(3));
    for (auto& g : pop)
        for (std::size_t i = 0; i < 3; ++i)
            g[i] = rng.next_range(bounds[i].lo, bounds[i].hi);

    auto best_of = [&](const std::vector<std::vector<float>>& p) {
        float best = -1e30f;
        for (const auto& g : p)
            best = std::max(best, HuntFitness(g));
        return best;
    };
    const float gen0_best = best_of(pop);

    for (int gen = 0; gen < 12; ++gen) {
        std::vector<float> fitness(P);
        for (std::size_t i = 0; i < P; ++i)
            fitness[i] = HuntFitness(pop[i]);
        pop = EvolveGeneration(pop,
                               fitness,
                               bounds,
                               /*sigma=*/0.12f,
                               /*elitism=*/2,
                               /*tournament_k=*/3,
                               rng);
    }
    const float evolved_best = best_of(pop);

    // Evolution improved hunting (fitness = -distance, so higher = closer).
    EXPECT_GT(evolved_best, gen0_best);
    // The evolved best is a genuinely good hunter (ends near the prey).
    EXPECT_GT(evolved_best, -4.0f);
}

// A predator with no goal but a nose homes in on a stationary, scent-emitting prey.
TEST(EcologyDemo, PredatorHuntsPreyDownScentGradient) {
    ScentField field(64, 64, 1); // channel 0 = prey scent
    entt::registry r;

    // Stationary prey emitting a strong scent at cell (40,32).
    const auto prey = r.create();
    r.emplace<TransformComponent>(prey).position = Luminumbra::Vec3(40.0f, 0.0f, 32.0f);
    {
        auto& s = r.emplace<SensableComponent>(prey);
        s.scent_channel = 0;
        s.scent_deposit = 80.0f;
        s.faction = 2;
    }

    // Predator far to the west with a hunting nose, no plan/target of its own.
    const auto pred = r.create();
    r.emplace<TransformComponent>(pred).position = Luminumbra::Vec3(20.0f, 0.0f, 32.0f);
    {
        auto& sn = r.emplace<ScentSenseComponent>(pred);
        sn.channel = 0;
        sn.sign = 1.0f; // track toward the source
        sn.strength = 1.0f;
        sn.floor = 1e-4f;
        sn.weber_k = 2.0f;
    }
    r.emplace<LocomotionIntentComponent>(pred);
    r.emplace<LocomotionProfile>(pred).move_speed = 8.0f;
    r.emplace<ActionPlanComponent>(pred); // empty plan -> locomotion idles, nose drives

    const float start =
        Dist(r.get<TransformComponent>(pred).position, r.get<TransformComponent>(prey).position);
    for (int t = 0; t < 400; ++t) {
        RunScentDepositOnTick(r, field, 0.0f, 0.0f, 1.0f);
        field.Step(/*rate=*/0.25, /*iters=*/4, /*evap=*/0.004); // rate is capped at 0.25
        field.Clamp(0.0, 1.0e6);
        RunInstinctLocomotionOnTick(r);                     // empty plan -> wish 0
        RunScentSteeringOnTick(r, field, 0.0f, 0.0f, 1.0f); // nose biases the wish
        IntegrateWishes(r, kDt);
    }
    const float end =
        Dist(r.get<TransformComponent>(pred).position, r.get<TransformComponent>(prey).position);

    EXPECT_LT(end, start * 0.4f); // closed most of the gap by following the scent
    EXPECT_LT(end, 6.0f);         // ended up near the prey
}

// A herd seeking a shared far target stays cohesive (boids) instead of dispersing.
TEST(EcologyDemo, HerdFlocksTogetherWhileTravelling) {
    entt::registry r;
    const auto target = r.create();
    r.emplace<TransformComponent>(target).position = Luminumbra::Vec3(120.0f, 0.0f, 0.0f);

    std::vector<entt::entity> herd;
    for (int i = 0; i < 8; ++i) {
        const auto e = r.create();
        // Scattered start around the origin.
        const float x = static_cast<float>((i % 4) * 3);
        const float z = static_cast<float>((i / 4) * 3) + (i & 1 ? 1.5f : 0.0f);
        r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
        auto& p = r.emplace<LocomotionProfile>(e);
        p.move_speed = 4.0f;
        p.separation_radius = 2.0f;
        p.separation_strength = 0.6f;
        p.flock_radius = 12.0f;
        p.cohesion_strength = 0.5f;
        p.alignment_strength = 0.4f;
        r.emplace<LocomotionIntentComponent>(e);
        auto& plan = r.emplace<ActionPlanComponent>(e);
        Action a;
        a.name = "approach";
        a.target = target;
        plan.plan.push_back(a);
        herd.push_back(e);
    }

    auto spread = [&]() {
        float maxd = 0.0f;
        for (std::size_t i = 0; i < herd.size(); ++i)
            for (std::size_t j = i + 1; j < herd.size(); ++j)
                maxd = std::max(maxd,
                                Dist(r.get<TransformComponent>(herd[i]).position,
                                     r.get<TransformComponent>(herd[j]).position));
        return maxd;
    };
    auto centroid_x = [&]() {
        float s = 0.0f;
        for (auto e : herd)
            s += r.get<TransformComponent>(e).position.x;
        return s / static_cast<float>(herd.size());
    };

    const float start_cx = centroid_x();
    for (int t = 0; t < 300; ++t) {
        RunInstinctLocomotionOnTick(r);
        IntegrateWishes(r, kDt);
    }
    EXPECT_GT(centroid_x(), start_cx + 10.0f); // herd travelled toward the target
    EXPECT_LT(spread(), 30.0f);                // and stayed a cohesive group (didn't scatter)
}
