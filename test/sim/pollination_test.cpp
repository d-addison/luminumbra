// sim.pollination — neighbouring plants CROSS-POLLINATE so a field's genetics DRIFT.
// Deterministic (id-ordered, pair-symmetric RNG seeded purely from offset 19 + sorted
// ids + tick; Sqrt is the only transcendental). Gated by PollinationTag: a world with
// none — and a world with no plants — writes nothing (canonical world_hash baseline
// byte-identical).
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#include <entt/entt.hpp>

#include "components/CoreComponents.h"
#include "components/PlantComponents.h"
#include "components/PollinationComponents.h"
#include "core/DeterministicRng.h"
#include "systems/PollinationSystem.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::foliage::PollinateCross;
using luminumbra::foliage::PollinationStats;
using luminumbra::foliage::RunPollinationOnTick;

// A genome whose genes are all the same constant — easy to reason about blends.
Comp::PlantGenomeComponent uniformGenome(float v) {
    Comp::PlantGenomeComponent g;
    for (auto& gene : g.genes)
        gene = v;
    return g;
}

// Spawn a pollination participant at (x,z). `stage` decides whether it can DONATE
// pollen (Flowering/Fruiting) — receivers can be any stage. Returns the entity.
entt::entity spawnPlant(entt::registry& r,
                        float x,
                        float z,
                        const Comp::PlantGenomeComponent& genome,
                        Comp::PlantStage stage) {
    auto e = r.create();
    auto& tf = r.emplace<Comp::TransformComponent>(e);
    tf.position.x = x;
    tf.position.y = 0.0f;
    tf.position.z = z;
    r.emplace<Comp::PlantGenomeComponent>(e, genome);
    auto& gr = r.emplace<Comp::PlantGrowthComponent>(e);
    gr.stage = static_cast<std::uint8_t>(stage);
    r.emplace<Comp::PollinationComponent>(e);
    r.emplace<Comp::PollinationTag>(e);
    return e;
}

int plantCount(entt::registry& r) {
    int n = 0;
    for (auto e : r.view<Comp::PlantGenomeComponent>()) {
        (void)e;
        ++n;
    }
    return n;
}

// ---- gating / no-op ----

// No plants at all -> the system reports zero and creates/changes nothing.
TEST(Pollination, EmptyRosterNoOp) {
    entt::registry r;
    const auto stats = RunPollinationOnTick(r, /*tick*/ 1);
    EXPECT_EQ(stats.considered, 0);
    EXPECT_EQ(stats.donors, 0);
    EXPECT_EQ(stats.crossed, 0);
    EXPECT_EQ(plantCount(r), 0);
}

// A plant WITHOUT the PollinationTag opt-in is never considered (gating marker).
TEST(Pollination, UntaggedPlantIgnored) {
    entt::registry r;
    // Two reproductive plants adjacent, but neither carries PollinationTag.
    auto mk = [&](float x) {
        auto e = r.create();
        auto& tf = r.emplace<Comp::TransformComponent>(e);
        tf.position.x = x;
        r.emplace<Comp::PlantGenomeComponent>(e, uniformGenome(0.5f));
        auto& gr = r.emplace<Comp::PlantGrowthComponent>(e);
        gr.stage = static_cast<std::uint8_t>(Comp::PlantStage::Flowering);
        r.emplace<Comp::PollinationComponent>(e); // has result slot but no tag
    };
    mk(0.0f);
    mk(1.0f);
    const auto stats = RunPollinationOnTick(r, /*tick*/ 1);
    EXPECT_EQ(stats.considered, 0);
    EXPECT_EQ(stats.crossed, 0);
}

// ---- core behaviour: a cross produces a MIX between the two parents ----

// Two adjacent plants of differing uniform genomes: the flowering one pollinates the
// other, whose next_genome lands strictly BETWEEN the two parents (not equal to either).
TEST(Pollination, NeighbourCrossProducesMix) {
    entt::registry r;
    const float lo = 0.2f, hi = 0.8f;
    // Receiver is a non-donor stage; donor is flowering and 1m away (within reach).
    auto receiver = spawnPlant(r, 0.0f, 0.0f, uniformGenome(lo), Comp::PlantStage::Juvenile);
    spawnPlant(r, 1.0f, 0.0f, uniformGenome(hi), Comp::PlantStage::Flowering);

    const auto stats = RunPollinationOnTick(r, /*tick*/ 5);
    EXPECT_EQ(stats.crossed, 1);
    EXPECT_GE(stats.donors, 1);

    const auto& pc = r.get<Comp::PollinationComponent>(receiver);
    EXPECT_TRUE(pc.pollinated);
    EXPECT_EQ(pc.last_pollen_tick, 5u);
    // Each mixed gene is a blend of lo & hi (BlendCrossover lies in [lo,hi]) plus a
    // small bounded mutation. It must differ from BOTH pure parents on at least one
    // gene, and stay inside valid [0,1] bounds.
    bool differs_from_lo = false, differs_from_hi = false;
    for (float g : pc.next_genome.genes) {
        EXPECT_GE(g, 0.0f);
        EXPECT_LE(g, 1.0f);
        if (g != lo)
            differs_from_lo = true;
        if (g != hi)
            differs_from_hi = true;
    }
    EXPECT_TRUE(differs_from_lo) << "mix must not equal the receiver parent exactly";
    EXPECT_TRUE(differs_from_hi) << "mix must not equal the donor parent exactly";
}

// An ISOLATED plant (no donor within reach) is left UNCHANGED.
TEST(Pollination, IsolatedPlantUnchanged) {
    entt::registry r;
    auto solo = spawnPlant(r, 0.0f, 0.0f, uniformGenome(0.3f), Comp::PlantStage::Juvenile);
    // A flowering donor far beyond kPollinationRadius (6m): no cross.
    spawnPlant(r, 100.0f, 0.0f, uniformGenome(0.9f), Comp::PlantStage::Flowering);

    const auto stats = RunPollinationOnTick(r, /*tick*/ 5);
    EXPECT_EQ(stats.crossed, 0);
    const auto& pc = r.get<Comp::PollinationComponent>(solo);
    EXPECT_FALSE(pc.pollinated);
    EXPECT_EQ(pc.crosses, 0u);
}

// A plant with no reproductive neighbour (the only other plant is a non-donor stage)
// is unchanged even when adjacent.
TEST(Pollination, NonDonorNeighbourDoesNotPollinate) {
    entt::registry r;
    auto a = spawnPlant(r, 0.0f, 0.0f, uniformGenome(0.3f), Comp::PlantStage::Juvenile);
    auto b = spawnPlant(r, 1.0f, 0.0f, uniformGenome(0.7f), Comp::PlantStage::Juvenile);
    const auto stats = RunPollinationOnTick(r, /*tick*/ 5);
    EXPECT_EQ(stats.crossed, 0);
    EXPECT_EQ(stats.donors, 0);
    EXPECT_FALSE(r.get<Comp::PollinationComponent>(a).pollinated);
    EXPECT_FALSE(r.get<Comp::PollinationComponent>(b).pollinated);
}

// ---- symmetry / order-independence of the cross ----

// PollinateCross is pair-symmetric: A<-B and B<-A draw the IDENTICAL blend (sorted-id
// seed + sorted-id parent order), so a mutual cross drifts both plants to the same point.
TEST(Pollination, CrossIsSymmetricAndOrderIndependent) {
    const Comp::PlantGenomeComponent A = uniformGenome(0.2f);
    const Comp::PlantGenomeComponent B = uniformGenome(0.8f);
    const std::uint64_t idA = 10, idB = 25, tick = 7;

    // Receiver A, donor B  vs  receiver B, donor A  -> identical mixed genome.
    const auto ab = PollinateCross(A, B, idA, idB, tick);
    const auto ba = PollinateCross(B, A, idB, idA, tick);
    for (std::size_t i = 0; i < ab.genes.size(); ++i) {
        EXPECT_FLOAT_EQ(ab.genes[i], ba.genes[i]) << "gene " << i << " asymmetric";
    }
}

TEST(Pollination, ConfiguredMutationRateChangesTheDeterministicCross) {
    const Comp::PlantGenomeComponent a = uniformGenome(0.2f);
    const Comp::PlantGenomeComponent b = uniformGenome(0.8f);
    const auto unmutated = PollinateCross(a, b, 10, 25, 7, 99, 0.0f);
    const auto mutated = PollinateCross(a, b, 10, 25, 7, 99, 0.25f);

    bool differs = false;
    for (std::size_t i = 0; i < unmutated.genes.size(); ++i) {
        EXPECT_GE(mutated.genes[i], 0.0f);
        EXPECT_LE(mutated.genes[i], 1.0f);
        differs = differs || unmutated.genes[i] != mutated.genes[i];
    }
    EXPECT_TRUE(differs);
}

// Through the SYSTEM: two mutually-flowering adjacent plants each receive the SAME
// mixed genome from the other (symmetric pair).
TEST(Pollination, MutualPairConvergesToSameMix) {
    entt::registry r;
    auto a = spawnPlant(r, 0.0f, 0.0f, uniformGenome(0.25f), Comp::PlantStage::Flowering);
    auto b = spawnPlant(r, 1.0f, 0.0f, uniformGenome(0.75f), Comp::PlantStage::Flowering);
    const auto stats = RunPollinationOnTick(r, /*tick*/ 9);
    EXPECT_EQ(stats.crossed, 2);
    const auto& pa = r.get<Comp::PollinationComponent>(a).next_genome;
    const auto& pb = r.get<Comp::PollinationComponent>(b).next_genome;
    for (std::size_t i = 0; i < pa.genes.size(); ++i) {
        EXPECT_FLOAT_EQ(pa.genes[i], pb.genes[i]) << "gene " << i;
    }
}

// ---- determinism: run == replay ----

// Identical setup + identical tick -> identical mixed genomes (down to the bit).
TEST(Pollination, RunEqualsReplay) {
    auto run = [] {
        entt::registry r;
        // A small field with a couple of flowering donors and a few receivers, plus
        // a non-uniform wind so the wind-biased path is exercised too.
        spawnPlant(r, 0.0f, 0.0f, uniformGenome(0.10f), Comp::PlantStage::Flowering);
        auto rcv1 = spawnPlant(r, 1.5f, 0.0f, uniformGenome(0.40f), Comp::PlantStage::Juvenile);
        spawnPlant(r, 3.0f, 0.0f, uniformGenome(0.90f), Comp::PlantStage::Fruiting);
        auto rcv2 = spawnPlant(r, 2.0f, 1.0f, uniformGenome(0.55f), Comp::PlantStage::Sprout);
        RunPollinationOnTick(r, /*tick*/ 13, ::Luminumbra::Vec2(1.0f, 0.0f));
        const auto& g1 = r.get<Comp::PollinationComponent>(rcv1).next_genome;
        const auto& g2 = r.get<Comp::PollinationComponent>(rcv2).next_genome;
        std::vector<float> out;
        for (float g : g1.genes)
            out.push_back(g);
        for (float g : g2.genes)
            out.push_back(g);
        return out;
    };
    EXPECT_EQ(run(), run());
}

// Wind biases which neighbour donates: a downwind donor reaches a receiver that an
// equidistant donor (no wind) would, but an UPWIND donor at the same distance can be
// pushed out of reach. We verify the wind-biased reach actually changes the outcome.
TEST(Pollination, WindExtendsDownwindReach) {
    // Donor at the reach edge along +X: 6.5m away (beyond the 6m still-air radius).
    // With wind blowing +X (toward the receiver), the effective distance shrinks so
    // the cross now happens.
    auto crossedWith = [](::Luminumbra::Vec2 wind) {
        entt::registry r;
        // Donor at x=0, receiver downwind at x=6.5 (>6m still-air radius).
        // (spawn donor first so id order is donor<receiver; irrelevant to reach.)
        // Use systems' own radius constant via behaviour, not hardcode.
        // Donor flowering, receiver juvenile.
        // NB: 6.5 > kPollinationRadius(6) so still-air => no cross.
        // +X wind of strength 1: along = 6.5, eff = 6.5 - 0.5*6.5 = 3.25 < 6 => cross.
        (void)r;
        entt::registry reg;
        // donor
        {
            auto e = reg.create();
            auto& tf = reg.emplace<Comp::TransformComponent>(e);
            tf.position.x = 0.0f;
            reg.emplace<Comp::PlantGenomeComponent>(e, Comp::PlantGenomeComponent{});
            auto& gr = reg.emplace<Comp::PlantGrowthComponent>(e);
            gr.stage = static_cast<std::uint8_t>(Comp::PlantStage::Flowering);
            reg.emplace<Comp::PollinationComponent>(e);
            reg.emplace<Comp::PollinationTag>(e);
        }
        // receiver downwind
        {
            auto e = reg.create();
            auto& tf = reg.emplace<Comp::TransformComponent>(e);
            tf.position.x = 6.5f;
            reg.emplace<Comp::PlantGenomeComponent>(e, Comp::PlantGenomeComponent{});
            auto& gr = reg.emplace<Comp::PlantGrowthComponent>(e);
            gr.stage = static_cast<std::uint8_t>(Comp::PlantStage::Juvenile);
            reg.emplace<Comp::PollinationComponent>(e);
            reg.emplace<Comp::PollinationTag>(e);
        }
        return RunPollinationOnTick(reg, /*tick*/ 4, wind).crossed;
    };
    EXPECT_EQ(crossedWith(::Luminumbra::Vec2(0.0f, 0.0f)), 0) // still air: out of reach
        << "6.5m apart exceeds the still-air pollination radius";
    EXPECT_EQ(crossedWith(::Luminumbra::Vec2(1.0f, 0.0f)), 1) // downwind: in reach
        << "downwind pollen should reach further";
}

// Candidate identity is independent of ownership and of recycled handle generations.
TEST(Pollination, SimulationDonorTiesUseAllIdentityBitsInBothRegionModes) {
    using Gate = Luminumbra::world::RegionConsumerGate;
    for (const bool regional : {false, true}) {
        entt::registry registry;
        const auto recycled = registry.create();
        registry.destroy(recycled);
        const auto low =
            spawnPlant(registry, 99, 100, uniformGenome(0.2f), Comp::PlantStage::Flowering);
        const auto high =
            spawnPlant(registry, 101, 100, uniformGenome(0.8f), Comp::PlantStage::Flowering);
        const auto receiver =
            spawnPlant(registry, 100, 100, uniformGenome(0.5f), Comp::PlantStage::Juvenile);
        ASSERT_GT(entt::to_integral(low), entt::to_integral(high));
        constexpr auto low_id = std::uint64_t{1} << 40;
        constexpr auto high_id = std::uint64_t{1} << 48;
        registry.emplace<Comp::SimulationIdentityComponent>(low, low_id);
        registry.emplace<Comp::SimulationIdentityComponent>(high, high_id);
        registry.emplace<Comp::SimulationIdentityComponent>(receiver, 7u);
        const std::array due{Luminumbra::world::RegionKey{0, 0}};
        std::vector<std::uint64_t> owners;
        const auto gate = regional ? Gate(registry, due, owners, Gate::PlantIdentity::Simulation)
                                   : Gate(Gate::PlantIdentity::Simulation);
        const auto stats = RunPollinationOnTick(registry, 13, {}, 99, 0.0f, gate);
        EXPECT_EQ(stats.crossed, 3);
        EXPECT_EQ(registry.get<Comp::PollinationComponent>(receiver).next_genome.genes,
                  PollinateCross(uniformGenome(0.5f), uniformGenome(0.2f), 7, low_id, 13, 99, 0.0f)
                      .genes);
        // A closer donor still beats a smaller identity.
        registry.get<Comp::TransformComponent>(high).position.x = 100.5f;
        RunPollinationOnTick(registry, 14, {}, 99, 0.0f, gate);
        EXPECT_EQ(registry.get<Comp::PollinationComponent>(receiver).next_genome.genes,
                  PollinateCross(uniformGenome(0.5f), uniformGenome(0.8f), 7, high_id, 14, 99, 0.0f)
                      .genes);
    }
}

TEST(Pollination, SimulationCrossPreservesCanonicalParentArgumentOrder) {
    using Gate = Luminumbra::world::RegionConsumerGate;
    for (const bool regional : {false, true}) {
        for (const float mutation_frac : {0.0f, 0.05f, 0.25f}) {
            SCOPED_TRACE(regional);
            SCOPED_TRACE(mutation_frac);
            entt::registry registry;
            const auto high =
                spawnPlant(registry, 100, 100, uniformGenome(0.8f), Comp::PlantStage::Flowering);
            const auto low =
                spawnPlant(registry, 101, 100, uniformGenome(0.2f), Comp::PlantStage::Flowering);
            constexpr auto high_id = std::uint64_t{1} << 50;
            registry.emplace<Comp::SimulationIdentityComponent>(high, high_id);
            registry.emplace<Comp::SimulationIdentityComponent>(low, 17u);
            const std::array due{Luminumbra::world::RegionKey{0, 0}};
            std::vector<std::uint64_t> owners;
            const auto gate = regional
                                  ? Gate(registry, due, owners, Gate::PlantIdentity::Simulation)
                                  : Gate(Gate::PlantIdentity::Simulation);
            // Independent oracle: ascending stable IDs order both seed inputs and genomes.
            // Breeding must use the caller's mutation fraction, not BreedPlants' default.
            auto rng = luminumbra::core::DeterministicRng::seeded(
                luminumbra::foliage::kPollinationSeedOffset ^ 99u ^ 7u, 17u, high_id);
            const auto expected = luminumbra::foliage::BreedPlants(
                uniformGenome(0.2f), uniformGenome(0.8f), rng, mutation_frac);
            ASSERT_EQ(RunPollinationOnTick(registry, 7, {}, 99, mutation_frac, gate).crossed, 2);
            EXPECT_EQ(registry.get<Comp::PollinationComponent>(high).next_genome.genes,
                      expected.genes);
            EXPECT_EQ(registry.get<Comp::PollinationComponent>(low).next_genome.genes,
                      expected.genes);
        }
    }
}

TEST(Pollination, SimulationAdmissionRefusesInvalidIdentitiesBeforeAnyCross) {
    using Gate = Luminumbra::world::RegionConsumerGate;
    for (const int failure : {0, 1, 2, 3}) {
        entt::registry registry;
        const auto first =
            spawnPlant(registry, 0, 0, uniformGenome(0.2f), Comp::PlantStage::Flowering);
        const auto second =
            spawnPlant(registry, 1, 0, uniformGenome(0.8f), Comp::PlantStage::Flowering);
        registry.emplace<Comp::SimulationIdentityComponent>(first, 1u);
        if (failure != 0) {
            const auto id =
                failure == 1
                    ? 0u
                    : (failure == 2 ? 1u
                                    : Luminumbra::world::ActiveRegionLedger::kPersistentIdLimit);
            registry.emplace<Comp::SimulationIdentityComponent>(second, id);
        }
        const auto gate = Gate(Gate::PlantIdentity::Simulation);
        EXPECT_THROW(RunPollinationOnTick(registry, 7, {}, 0, 0.05f, gate), std::runtime_error);
        EXPECT_THROW(luminumbra::foliage::RunPollinationRegional(registry, 7, gate),
                     std::runtime_error);
        for (const auto entity : {first, second}) {
            const auto& pollen = registry.get<Comp::PollinationComponent>(entity);
            EXPECT_FALSE(pollen.pollinated);
            EXPECT_EQ(pollen.crosses, 0u);
            EXPECT_EQ(pollen.last_pollen_tick, 0u);
        }
    }
}

TEST(Pollination, SimulationVisitCountExcludesUnrelatedResidentsAndNoDonorsIsNoOp) {
    using Gate = Luminumbra::world::RegionConsumerGate;
    entt::registry registry;
    const auto first =
        spawnPlant(registry, 100, 100, uniformGenome(0.2f), Comp::PlantStage::Juvenile);
    const auto second =
        spawnPlant(registry, 101, 100, uniformGenome(0.8f), Comp::PlantStage::Juvenile);
    registry.emplace<Comp::SimulationIdentityComponent>(first, 1u);
    registry.emplace<Comp::SimulationIdentityComponent>(second, 2u);
    const std::array due{Luminumbra::world::RegionKey{0, 0}};
    std::vector<std::uint64_t> owners;
    const Gate before(registry, due, owners, Gate::PlantIdentity::Simulation);
    EXPECT_EQ(RunPollinationOnTick(registry, 1, {}, 0, 0.05f, before).crossed, 0);
    const auto visits = before.owner_visits();
    EXPECT_EQ(visits, 3u); // One due owner, two participating plants.
    for (int index = 0; index < 4096; ++index)
        registry.emplace<Comp::TransformComponent>(registry.create());
    const Gate after(registry, due, owners, Gate::PlantIdentity::Simulation);
    EXPECT_EQ(RunPollinationOnTick(registry, 2, {}, 0, 0.05f, after).crossed, 0);
    EXPECT_EQ(after.owner_visits(), visits);
    const Gate none_due(registry, {}, owners, Gate::PlantIdentity::Simulation);
    EXPECT_EQ(RunPollinationOnTick(registry, 3, {}, 0, 0.05f, none_due).considered, 0);
    entt::registry empty;
    EXPECT_EQ(RunPollinationOnTick(empty, 4, {}, 0, 0.05f, Gate(Gate::PlantIdentity::Simulation))
                  .considered,
              0);
}

TEST(Pollination, RegionalElapsedMatchesIndependentTickSequence) {
    namespace F = luminumbra::foliage;
    namespace W = Luminumbra::world;
    using Gate = W::RegionConsumerGate;
    for (const bool simulation : {false, true}) {
        SCOPED_TRACE(simulation);
        entt::registry regional, reference;
        const std::array positions{Luminumbra::Vec2(-1, -1),
                                   Luminumbra::Vec2(1, -1),
                                   Luminumbra::Vec2(-1, 1),
                                   Luminumbra::Vec2(1, 1)};
        const std::array owners{
            W::RegionKey{-1, -1}, W::RegionKey{0, -1}, W::RegionKey{-1, 0}, W::RegionKey{0, 0}};
        std::array<entt::entity, 4> entities{};
        for (auto* registry : {&regional, &reference}) {
            for (std::size_t index = 0; index < positions.size(); ++index) {
                const auto entity = spawnPlant(*registry,
                                               positions[index].x,
                                               positions[index].y,
                                               uniformGenome(0.2f * static_cast<float>(index + 1)),
                                               Comp::PlantStage::Flowering);
                entities[index] = entity;
                const auto id = static_cast<std::uint64_t>(10 - index);
                registry->emplace<Comp::PlantRegionComponent>(
                    entity, Comp::PlantRegionComponent{id, owners[index].x, owners[index].z, 0u});
                registry->emplace<Comp::SimulationIdentityComponent>(entity, id);
                if (index == 0) {
                    registry->get<Comp::PlantGrowthComponent>(entity).planted_tick = 99;
                    registry->get<Comp::PollinationComponent>(entity).crosses =
                        std::numeric_limits<std::uint32_t>::max() - 1u;
                }
            }
        }
        const auto identity =
            simulation ? Gate::PlantIdentity::Simulation : Gate::PlantIdentity::Legacy;
        const std::array due{owners[0], owners[1], owners[3]};
        const std::map<W::RegionKey, std::uint32_t> windows{
            {owners[0], 5}, {owners[1], 3}, {owners[2], 65536}, {owners[3], 0}};
        std::vector<std::uint64_t> scratch;
        const Gate gate(regional, due, scratch, identity, W::RegionElapsedTicks(windows));
        const auto result = F::RunPollinationRegional(regional, 100, gate, {1, 0}, 123);
        PollinationStats expected;
        for (std::uint64_t tick = 96; tick <= 100; ++tick) {
            std::vector<W::RegionKey> active;
            if (tick >= 99)
                active.push_back(owners[0]);
            if (tick >= 98)
                active.push_back(owners[1]);
            std::vector<std::uint64_t> step_scratch;
            const Gate step_gate(reference, active, step_scratch, identity);
            const auto step = RunPollinationOnTick(reference, tick, {1, 0}, 123, 0.05f, step_gate);
            expected.considered += step.considered;
            expected.donors += step.donors;
            expected.crossed += step.crossed;
            expected.candidate_tests += step.candidate_tests;
        }
        EXPECT_EQ(result.considered, 5);
        EXPECT_EQ(result.considered, expected.considered);
        EXPECT_EQ(result.donors, expected.donors);
        EXPECT_EQ(result.crossed, 4);
        EXPECT_EQ(result.crossed, expected.crossed);
        EXPECT_EQ(result.candidate_tests, expected.candidate_tests);
        for (const auto entity : entities) {
            const auto& actual = regional.get<Comp::PollinationComponent>(entity);
            const auto& oracle = reference.get<Comp::PollinationComponent>(entity);
            EXPECT_EQ(actual.pollinated, oracle.pollinated);
            EXPECT_EQ(actual.crosses, oracle.crosses);
            EXPECT_EQ(actual.last_pollen_tick, oracle.last_pollen_tick);
            EXPECT_EQ(actual.next_genome.genes, oracle.next_genome.genes);
        }
        EXPECT_EQ(regional.get<Comp::PollinationComponent>(entities[0]).crosses,
                  std::numeric_limits<std::uint32_t>::max());
        // One collection and one elapsed lookup per admitted plant, independent of history.
        EXPECT_EQ(gate.owner_visits(), 10u);
    }
}

TEST(Pollination, RegionalWindowBoundsZeroAndLegacyEntryPoint) {
    namespace F = luminumbra::foliage;
    namespace W = Luminumbra::world;
    for (const std::uint32_t elapsed : {0u, 1u, 36001u, 65536u, 65537u}) {
        SCOPED_TRACE(elapsed);
        entt::registry registry;
        const auto receiver =
            spawnPlant(registry, 100, 100, uniformGenome(0.2f), Comp::PlantStage::Seed);
        const auto donor =
            spawnPlant(registry, 101, 100, uniformGenome(0.8f), Comp::PlantStage::Flowering);
        // Unrelated residents must not enter the replay scans.
        for (int index = 0; index < 32; ++index)
            (void)registry.create();
        const std::array due{W::RegionKey{0, 0}};
        const std::map<W::RegionKey, std::uint32_t> windows{{due[0], elapsed}};
        const W::RegionConsumerGate gate(registry, due, W::RegionElapsedTicks(windows));
        if (elapsed > 65536) {
            EXPECT_THROW(F::RunPollinationRegional(registry, 65536, gate), std::invalid_argument);
            EXPECT_EQ(registry.get<Comp::PollinationComponent>(receiver).crosses, 0u);
        } else {
            const auto result = F::RunPollinationRegional(registry, 65536, gate);
            EXPECT_EQ(result.crossed, static_cast<int>(elapsed));
            const auto& pollen = registry.get<Comp::PollinationComponent>(receiver);
            EXPECT_EQ(pollen.crosses, elapsed);
            EXPECT_EQ(pollen.last_pollen_tick, elapsed == 0 ? 0u : 65536u);
            if (elapsed != 0) {
                const auto expected = F::PollinateCross(uniformGenome(0.2f),
                                                        uniformGenome(0.8f),
                                                        F::PlantSimulationId(registry, receiver),
                                                        F::PlantSimulationId(registry, donor),
                                                        65536);
                EXPECT_EQ(pollen.next_genome.genes, expected.genes);
            }
        }
        const auto before = registry.get<Comp::PollinationComponent>(receiver).crosses;
        EXPECT_EQ(RunPollinationOnTick(registry, 65537, {}, 0, 0.05f, gate).crossed, 1);
        EXPECT_EQ(registry.get<Comp::PollinationComponent>(receiver).crosses, before + 1u);
    }
    entt::registry registry;
    const auto plant = spawnPlant(registry, 100, 100, uniformGenome(0.5f), Comp::PlantStage::Seed);
    const std::array due{W::RegionKey{0, 0}};
    const std::map<W::RegionKey, std::uint32_t> windows{{due[0], 3}};
    const W::RegionConsumerGate gate(registry, due, W::RegionElapsedTicks(windows));
    EXPECT_THROW(F::RunPollinationRegional(registry, 1, gate), std::invalid_argument);
    EXPECT_EQ(registry.get<Comp::PollinationComponent>(plant).crosses, 0u);
    EXPECT_EQ(F::RunPollinationRegional(registry, 3, gate).considered, 3);
    EXPECT_EQ(F::RunPollinationRegional(registry, 3, gate).crossed, 0);
    registry.get<Comp::PlantGrowthComponent>(plant).planted_tick = 4;
    EXPECT_EQ(F::RunPollinationRegional(registry, 3, gate).considered, 0);
    registry.clear();
    EXPECT_EQ(F::RunPollinationRegional(registry, 3, gate).considered, 0);
    spawnPlant(registry, 100, 100, uniformGenome(0.5f), Comp::PlantStage::Seed);
    EXPECT_EQ(F::RunPollinationRegional(registry, 0, {}).considered, 1);
    EXPECT_EQ(F::RunPollinationRegional(registry, std::numeric_limits<std::uint64_t>::max(), {})
                  .considered,
              1);
}

} // namespace
