#include <gtest/gtest.h>

#include "../support/LegacyCreatureTickOracle.h"
#include "ai/CircadianSystem.h"
#include "ai/CreatureBrainSystem.h"
#include "ai/CreatureCoarse.h"
#include "ai/CreatureReproductionSystem.h"
#include "ai/DecompositionSystem.h"
#include "ai/EcologyHash.h"
#include "ai/HerdAlarmSystem.h"
#include "ai/LifespanSystem.h"
#include "ai/MigrationSystem.h"
#include "ai/PredatorPackSystem.h"
#include "ai/ScavengingSystem.h"
#include "ai/SteeringConsumer.h"
#include "ai/TerritorySystem.h"
#include "ai/ThirstSystem.h"
#include "ai/WildlifeFoliageSystem.h"
#include "systems/FoliageRegistryStage.h"

#include <array>
#include <bit>
#include <limits>
#include <map>
#include <set>

namespace {
namespace A = luminumbra::ai;
namespace C = Luminumbra::Components;
namespace W = Luminumbra::world;
namespace O = LegacyCreatureTickOracle;
constexpr float kDt = static_cast<float>(1.0 / 30.0);

enum class Block {
    Frozen,
    ReducedOffDue,
    Coarse
};
// Exercise the predicate boundary with real region states, cadence due sets and tiers.
// Positions deliberately overlap across sealed owners to expose source filtering.
struct Cohort {
    entt::registry reg;
    std::map<W::RegionKey, W::ActiveRegionRecord> records;
    std::set<W::RegionKey> due;
    entt::entity Creature(std::uint64_t id, float x = 0, bool predator = false) {
        auto e = O::AddCreature(reg, x, 0, predator);
        reg.emplace<C::CreatureRegionComponent>(e, id, static_cast<std::int32_t>(id), 0, 0);
        reg.emplace<C::CreatureCoarseComponent>(e);
        records[{static_cast<std::int32_t>(id), 0}] = {};
        due.insert({static_cast<std::int32_t>(id), 0});
        return e;
    }
    void BlockEntity(entt::entity e, Block block) {
        auto& owner = reg.get<C::CreatureRegionComponent>(e);
        const W::RegionKey key{owner.owner_x, owner.owner_z};
        if (block == Block::Frozen)
            records.at(key).state = W::RegionState::Frozen;
        if (block == Block::ReducedOffDue) {
            records.at(key).state = W::RegionState::Reduced;
            due.erase(key);
        }
        if (block == Block::Coarse)
            reg.get<C::CreatureCoarseComponent>(e).tier = C::CreatureTier::Coarse;
    }
    W::RegionEntityGate Gate() {
        return [this](auto e) {
            const auto* owner = reg.try_get<C::CreatureRegionComponent>(e);
            const auto* coarse = reg.try_get<C::CreatureCoarseComponent>(e);
            if (!owner || !coarse || coarse->tier != C::CreatureTier::Full)
                return false;
            const W::RegionKey key{owner->owner_x, owner->owner_z};
            const auto it = records.find(key);
            return it != records.end() && it->second.state == W::RegionState::Active &&
                   due.contains(key);
        };
    }
};
template<class F>
void Matrix(F test) {
    for (auto block : {Block::Frozen, Block::ReducedOffDue, Block::Coarse})
        for (int mask = 0; mask != 4; ++mask) {
            SCOPED_TRACE(static_cast<int>(block));
            SCOPED_TRACE(mask);
            test(block, bool(mask & 1), bool(mask & 2));
        }
}
void Brain(Cohort& f, bool substrate = true) {
    A::RunCreatureBrainSystemOnTick(f.reg, kDt, {}, nullptr, 0, 0, 0, substrate, nullptr, f.Gate());
}

TEST(CreatureRegionGates, BrainFrozenActorsAndTargetsRemainUnchanged) {
    Matrix([](Block b, bool source, bool recipient) {
        Cohort f;
        const auto predator = f.Creature(1, 0, true), prey = f.Creature(2, 1);
        if (source)
            f.BlockEntity(predator, b);
        if (recipient)
            f.BlockEntity(prey, b);
        f.reg.get<C::CreatureComponent>(predator).wish_x = -0.0f;
        const auto p = O::Fields(f.reg, predator), q = O::Fields(f.reg, prey);
        Brain(f);
        EXPECT_EQ(f.reg.get<C::CreatureComponent>(prey).eaten, !source && !recipient);
        if (source)
            EXPECT_EQ(O::Fields(f.reg, predator), p);
        if (recipient)
            EXPECT_EQ(O::Fields(f.reg, prey), q);
        if (!source && !recipient)
            EXPECT_LT(f.reg.get<C::CreatureComponent>(predator).hunger, .1f);
    });
}

TEST(CreatureRegionGates, BrainInlineAndSubstrateUseTheSameAdmittedSnapshot) {
    Matrix([](Block b, bool source, bool recipient) {
        Cohort a, c;
        for (auto* f : {&a, &c}) {
            const auto p = f->Creature(1, 0, true), q = f->Creature(2, 1);
            const auto flock = f->Creature(3, 2);
            f->BlockEntity(flock, b);
            if (source)
                f->BlockEntity(p, b);
            if (recipient)
                f->BlockEntity(q, b);
        }
        Brain(a, false);
        Brain(c, true);
        EXPECT_EQ(O::Fields(a.reg), O::Fields(c.reg));
    });
}

TEST(CreatureRegionGates, BrainPersistentIdentityControlsWanderAndNearestTies) {
    std::array<std::array<std::uint32_t, 2>, 2> wishes{};
    for (int reverse = 0; reverse < 2; ++reverse) {
        Cohort f;
        // Recycling changes the generation as well as allocation order.
        const auto discarded = f.reg.create();
        f.reg.destroy(discarded);
        if (reverse)
            (void)f.reg.create(); // Different actor handle and generation for the same identity.
        const auto p = f.Creature(1, 0, true);
        entt::entity low, high;
        if (reverse) {
            high = f.Creature(20, -1);
            low = f.Creature(10, 1);
        } else {
            low = f.Creature(10, 1);
            high = f.Creature(20, -1);
        }
        Brain(f);
        EXPECT_TRUE(f.reg.get<C::CreatureComponent>(low).eaten);
        EXPECT_FALSE(f.reg.get<C::CreatureComponent>(high).eaten);
        f.reg.destroy(low);
        f.reg.destroy(high);
        f.reg.get<C::CreatureComponent>(p).hunger = 0;
        f.reg.get<C::CreatureComponent>(p).energy = 1;
        Brain(f);
        const auto& cr = f.reg.get<C::CreatureComponent>(p);
        EXPECT_EQ(cr.last_action, static_cast<int>(A::CreatureAction::Wander));
        wishes[reverse] = {std::bit_cast<std::uint32_t>(cr.wish_x),
                           std::bit_cast<std::uint32_t>(cr.wish_z)};
    }
    EXPECT_EQ(wishes[0], wishes[1]);
}

void Genome(Cohort& f, entt::entity e, bool female) {
    auto& gn = f.reg.emplace<C::CreatureGenomeComponent>(e);
    gn.female = female;
    gn.age_ticks = 120;
    f.reg.get<C::CreatureComponent>(e).hunger = .05f;
}
TEST(CreatureRegionGates, MateSeekingAndResolveGateBothParentsBeforeBookkeeping) {
    Matrix([](Block b, bool source, bool recipient) {
        Cohort f;
        auto mother = f.Creature(1), father = f.Creature(2, 1);
        Genome(f, mother, true);
        Genome(f, father, false);
        if (source)
            f.BlockEntity(father, b);
        if (recipient)
            f.BlockEntity(mother, b);
        auto before_m = O::Fields(f.reg, mother), before_f = O::Fields(f.reg, father);
        A::ReproductionTuning tuning;
        tuning.courtship_ticks = 1;
        A::RunMateSeekingOnTick(f.reg, tuning, f.Gate());
        int proposals = 0;
        const auto result = A::RunMatingResolveOnTick(
            f.reg, 1, 1337, tuning, nullptr, f.Gate(), {}, [&](const auto&) {
                ++proposals;
                return true;
            });
        EXPECT_EQ(result.born, !source && !recipient ? 1 : 0);
        EXPECT_EQ(proposals, result.born);
        if (source)
            EXPECT_EQ(O::Fields(f.reg, father), before_f);
        if (recipient)
            EXPECT_EQ(O::Fields(f.reg, mother), before_m);
        if (source && !recipient)
            EXPECT_EQ(f.reg.get<C::CreatureGenomeComponent>(mother).courting_ticks, 0u);
    });
}

TEST(CreatureRegionGates, ReproductionSeedsAndBirthOrderIgnoreRegistryOrder) {
    std::vector<std::string> results;
    for (bool reverse : {false, true}) {
        Cohort f;
        std::map<int, entt::entity> entities;
        for (int i : reverse ? std::array{4, 3, 2, 1} : std::array{1, 2, 3, 4}) {
            entities[i] = f.Creature(i, i <= 2 ? 0 : 100);
            Genome(f, entities[i], i % 2 != 0);
        }
        f.reg.get<C::CreatureGenomeComponent>(entities[1]).generation =
            std::numeric_limits<std::uint32_t>::max();
        A::ReproductionTuning tuning;
        tuning.courtship_ticks = 1;
        std::string result;
        EXPECT_EQ(
            A::RunMatingResolveOnTick(
                f.reg,
                17,
                1337,
                tuning,
                nullptr,
                f.Gate(),
                {},
                [&](const auto& birth) {
                    const auto seed = W::RegionRng(1337,
                                                   birth.mother_owner,
                                                   W::RegionRngTag::CreatureReproduction,
                                                   17,
                                                   birth.mother_id)
                                          .draw(birth.father_id);
                    EXPECT_EQ(birth.seed, seed);
                    luminumbra::core::DeterministicRng rng(seed);
                    A::CreatureGenome parent;
                    auto genome = A::BreedOffspring(parent, parent, rng);
                    const auto female = (rng.next_u64() & 1) == 0;
                    genome = A::BreedSensoryInto(genome, parent, parent, rng);
                    EXPECT_EQ(std::bit_cast<std::uint32_t>(birth.genome.move_speed),
                              std::bit_cast<std::uint32_t>(genome.move_speed));
                    EXPECT_EQ(std::bit_cast<std::uint32_t>(birth.genome.hearing_range),
                              std::bit_cast<std::uint32_t>(genome.hearing_range));
                    EXPECT_EQ(birth.female, female);
                    if (birth.mother_id == 1)
                        EXPECT_EQ(birth.generation, std::numeric_limits<std::uint32_t>::max());
                    result += std::to_string(birth.mother_id) + ":" + std::to_string(seed) + ";";
                    return true;
                })
                .born,
            2);
        results.push_back(result);
    }
    EXPECT_EQ(results[0], results[1]);
}

TEST(CreatureRegionGates, BirthAdmissionRejectsAbsentFrozenAndOffDueDestinations) {
    W::ActiveRegionLedger ledger;
    auto& records = const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(ledger.records());
    const Luminumbra::Vec3 position(1, 0, 1);
    const std::vector<W::RegionKey> due{{0, 0}};
    EXPECT_FALSE(A::AdmitFullCreatureBirthDestination(ledger, due, position));
    records[{0, 0}] = {};
    EXPECT_TRUE(A::AdmitFullCreatureBirthDestination(ledger, due, position));
    EXPECT_FALSE(A::AdmitFullCreatureBirthDestination(ledger, {}, position));
    records.at({0, 0}).state = W::RegionState::Frozen;
    EXPECT_FALSE(A::AdmitFullCreatureBirthDestination(ledger, due, position));
    records.at({0, 0}).state = W::RegionState::Reduced;
    EXPECT_FALSE(A::AdmitFullCreatureBirthDestination(ledger, due, position));
    EXPECT_EQ(ledger.next_persistent_id(), 1u);
}

TEST(CreatureRegionGates, FailedBirthAdmissionDoesNotPublishChildOrParentCooldown) {
    Cohort f;
    const auto mother = f.Creature(1), father = f.Creature(2, 1);
    Genome(f, mother, true);
    Genome(f, father, false);
    A::ReproductionTuning tuning;
    tuning.courtship_ticks = 1;
    f.reg.get<C::CreatureGenomeComponent>(mother).courting_ticks =
        std::numeric_limits<std::uint32_t>::max();
    for (bool with_callback : {false, true}) {
        A::CreatureBirthAdmission admission;
        if (with_callback)
            admission = [](const auto&) {
                return false;
            };
        EXPECT_EQ(
            A::RunMatingResolveOnTick(f.reg, 2, 1337, tuning, nullptr, f.Gate(), {}, admission)
                .born,
            0);
        EXPECT_EQ(f.reg.view<C::CreatureComponent>().size(), 2u);
        EXPECT_EQ(f.reg.get<C::CreatureGenomeComponent>(mother).reproduce_cooldown, 0u);
        EXPECT_EQ(f.reg.get<C::CreatureGenomeComponent>(father).reproduce_cooldown, 0u);
        EXPECT_EQ(f.reg.get<C::CreatureGenomeComponent>(mother).courting_ticks,
                  std::numeric_limits<std::uint32_t>::max());
    }
}

TEST(CreatureRegionGates, SteeringAndSurvivalBlendLeaveBlockedWishesBitExact) {
    for (auto b : {Block::Frozen, Block::ReducedOffDue, Block::Coarse}) {
        Cohort f;
        const auto allowed = f.Creature(1), blocked = f.Creature(2);
        for (auto e : {allowed, blocked}) {
            f.reg.get<C::CreatureComponent>(e).last_action =
                static_cast<int>(A::CreatureAction::Drink);
            f.reg.get<C::CreatureComponent>(e).wish_z = -0.0f;
            f.reg.emplace<C::MigratoryComponent>(e).wish_x = 2;
            f.reg.emplace<C::TerritoryBiasComponent>(e).wish_x = 3;
            f.reg.emplace<C::ThirstComponent>(e).wish_x = 4;
            f.reg.emplace<C::ScavengerComponent>(e).wish_x = 5;
        }
        f.BlockEntity(blocked, b);
        const auto before = O::Fields(f.reg, blocked);
        A::RunSteeringConsumerOnTick(f.reg, f.Gate());
        A::RunSurvivalWishBlendOnTick(f.reg, f.Gate());
        EXPECT_EQ(O::Fields(f.reg, blocked), before);
        EXPECT_EQ(f.reg.get<C::CreatureComponent>(allowed).wish_x, 14);
    }
}

TEST(CreatureRegionGates, ThirstGatesActorsAndWaterSources) {
    Matrix([](Block b, bool source, bool recipient) {
        Cohort f;
        const auto actor = f.Creature(1), water = f.Creature(2, .5f);
        f.reg.emplace<C::ThirstComponent>(actor).thirst = .8f;
        f.reg.emplace<C::WaterHoleComponent>(water);
        if (source)
            f.BlockEntity(water, b);
        if (recipient)
            f.BlockEntity(actor, b);
        const auto before = O::Fields(f.reg, actor);
        const auto stats = A::RunThirstOnTick(f.reg, kDt, {}, f.Gate());
        EXPECT_EQ(stats.drinking, !source && !recipient ? 1 : 0);
        if (recipient)
            EXPECT_EQ(O::Fields(f.reg, actor), before);
    });
    // Two equidistant holes: the smaller PersistentId wins despite insertion order.
    Cohort f;
    auto actor = f.Creature(1), high = f.Creature(30, -10), low = f.Creature(20, 10);
    f.reg.emplace<C::ThirstComponent>(actor).thirst = .8f;
    f.reg.emplace<C::WaterHoleComponent>(high).radius = 1;
    f.reg.emplace<C::WaterHoleComponent>(low).radius = 1;
    A::RunThirstOnTick(f.reg, kDt, {}, f.Gate());
    EXPECT_GT(f.reg.get<C::ThirstComponent>(actor).wish_x, 0);
}

TEST(CreatureRegionGates, ScavengingGatesActorsAndCarcassSources) {
    Matrix([](Block b, bool source, bool recipient) {
        Cohort f;
        const auto actor = f.Creature(1, 0, true), corpse = f.Creature(2, .5f);
        f.reg.emplace<C::ScavengerComponent>(actor);
        f.reg.get<C::CreatureComponent>(corpse).eaten = true;
        if (source)
            f.BlockEntity(corpse, b);
        if (recipient)
            f.BlockEntity(actor, b);
        const auto before = O::Fields(f.reg, actor), food = O::Fields(f.reg, corpse);
        EXPECT_EQ(A::RunScavengingOnTick(f.reg, 1, {}, f.Gate()).feeding,
                  !source && !recipient ? 1 : 0);
        EXPECT_EQ(O::Fields(f.reg, corpse), food);
        if (recipient)
            EXPECT_EQ(O::Fields(f.reg, actor), before);
    });
    Cohort f;
    auto actor = f.Creature(1, 0, true), high = f.Creature(30, -10), low = f.Creature(20, 10);
    f.reg.emplace<C::ScavengerComponent>(actor);
    f.reg.get<C::CreatureComponent>(high).eaten = f.reg.get<C::CreatureComponent>(low).eaten = true;
    A::RunScavengingOnTick(f.reg, 1, {}, f.Gate());
    EXPECT_GT(f.reg.get<C::ScavengerComponent>(actor).wish_x, 0);
}

TEST(CreatureRegionGates, AlarmGatesEmittersAndRecipientsBeforeSnapshot) {
    Matrix([](Block b, bool source, bool recipient) {
        Cohort f;
        const auto emitter = f.Creature(1), target = f.Creature(2, 1);
        f.reg.emplace<C::AlarmComponent>(emitter).alarmed = 1;
        f.reg.emplace<C::AlarmComponent>(target);
        if (source)
            f.BlockEntity(emitter, b);
        if (recipient)
            f.BlockEntity(target, b);
        const auto before = O::Fields(f.reg, target), emission = O::Fields(f.reg, emitter);
        A::RunHerdAlarmOnTick(f.reg, kDt, f.Gate());
        EXPECT_EQ(f.reg.get<C::AlarmComponent>(target).level > 0, !source && !recipient);
        if (source)
            EXPECT_EQ(O::Fields(f.reg, emitter), emission);
        if (recipient)
            EXPECT_EQ(O::Fields(f.reg, target), before);
    });
}

TEST(CreatureRegionGates, PackGatesMembersAndPreyAndUsesPersistentTies) {
    Matrix([](Block b, bool source, bool recipient) {
        Cohort f;
        const auto hunter = f.Creature(1, 0, true), prey = f.Creature(2, 10);
        f.reg.emplace<C::PackHunterComponent>(hunter).coord_z = -0.0f;
        if (source)
            f.BlockEntity(prey, b);
        if (recipient)
            f.BlockEntity(hunter, b);
        const auto before = O::Fields(f.reg, hunter);
        const auto stats = A::RunPredatorPackOnTick(f.reg, 1, f.Gate());
        EXPECT_EQ(stats.hunting, !source && !recipient ? 1 : 0);
        if (recipient)
            EXPECT_EQ(O::Fields(f.reg, hunter), before);
    });
    for (bool reverse : {false, true}) {
        Cohort f;
        const auto hunter = f.Creature(1, 0, true);
        const auto ally = f.Creature(2, 0, true);
        f.reg.emplace<C::PackHunterComponent>(hunter);
        f.reg.emplace<C::PackHunterComponent>(ally);
        if (reverse) {
            f.Creature(30, -10);
            f.Creature(20, 10);
        } else {
            f.Creature(20, 10);
            f.Creature(30, -10);
        }
        A::RunPredatorPackOnTick(f.reg, 1, f.Gate());
        EXPECT_EQ(f.reg.get<C::PackHunterComponent>(hunter).in_pack, 1);
        EXPECT_GT(f.reg.get<C::PackHunterComponent>(hunter).coord_x, 0);
        f.BlockEntity(ally, Block::Frozen);
        const auto before = O::Fields(f.reg, ally);
        A::RunPredatorPackOnTick(f.reg, 2, f.Gate());
        EXPECT_EQ(f.reg.get<C::PackHunterComponent>(hunter).in_pack, 0);
        EXPECT_EQ(O::Fields(f.reg, ally), before);
    }
}

TEST(CreatureRegionGates, CircadianTerritoryAndMigrationDoNotTouchBlockedEntities) {
    for (auto b : {Block::Frozen, Block::ReducedOffDue, Block::Coarse}) {
        Cohort f;
        auto allowed = f.Creature(1), blocked = f.Creature(2);
        for (auto e : {allowed, blocked}) {
            f.reg.emplace<C::CircadianComponent>(e);
            f.reg.emplace<C::TerritoryComponent>(e);
            f.reg.emplace<C::MigratoryComponent>(e);
        }
        f.BlockEntity(blocked, b);
        const auto before = O::Fields(f.reg, blocked);
        A::RunCircadianOnTick(f.reg, 0, 1, f.Gate());
        A::RunTerritoryOnTick(f.reg, 1, f.Gate());
        A::RunMigrationOnTick(f.reg, 0, f.Gate());
        EXPECT_EQ(O::Fields(f.reg, blocked), before);
        EXPECT_FALSE(f.reg.all_of<C::TerritoryBiasComponent>(blocked));
        EXPECT_TRUE(f.reg.all_of<C::TerritoryBiasComponent>(allowed));
        EXPECT_LT(f.reg.get<C::CircadianComponent>(allowed).activity, 1);
        EXPECT_GT(f.reg.get<C::MigratoryComponent>(allowed).drive, 0);
    }
}

TEST(CreatureRegionGates, FullAndCoarseAgeCountersHaveDistinctOwners) {
    Cohort f;
    auto full = f.Creature(1), coarse = f.Creature(2);
    for (auto e : {full, coarse}) {
        Genome(f, e, false);
        f.reg.emplace<C::MortalComponent>(e);
    }
    f.BlockEntity(coarse, Block::Coarse);
    A::RunMatingResolveOnTick(f.reg, 1, 1337, {}, nullptr, f.Gate());
    A::RunLifespanOnTick(f.reg, 1, f.Gate());
    EXPECT_EQ(f.reg.get<C::CreatureGenomeComponent>(full).age_ticks, 121u);
    EXPECT_EQ(f.reg.get<C::MortalComponent>(full).age_ticks, 1u);
    EXPECT_EQ(f.reg.get<C::CreatureGenomeComponent>(coarse).age_ticks, 120u);
    EXPECT_EQ(f.reg.get<C::MortalComponent>(coarse).age_ticks, 0u);
    A::CoarseCreature input;
    input.species_id = 123;
    input.region = {2, 0, 0, 0};
    input.coarse.tier = C::CreatureTier::Coarse;
    const std::array previous{input};
    const std::array<W::RegionKey, 1> due{{{0, 0}}};
    const A::CoarseRegionRecords records{{{0, 0}, {}}};
    const auto stepped = A::StepCoarseCreatures(previous, due, records, 1337, 1);
    EXPECT_EQ(stepped.creatures.front().coarse.age_cadences, 1u);
    EXPECT_EQ(f.reg.get<C::CreatureGenomeComponent>(coarse).age_ticks, 120u);
    f.BlockEntity(full, Block::Coarse);
    A::RunMatingResolveOnTick(f.reg, 10000, 1337, {}, nullptr, f.Gate());
    A::RunLifespanOnTick(f.reg, 10000, f.Gate());
    EXPECT_EQ(f.reg.get<C::CreatureGenomeComponent>(full).age_ticks, 121u);
    EXPECT_EQ(f.reg.get<C::MortalComponent>(full).age_ticks, 1u);
    f.reg.get<C::CreatureCoarseComponent>(full).tier = C::CreatureTier::Full;
    A::RunMatingResolveOnTick(f.reg, 10001, 1337, {}, nullptr, f.Gate());
    A::RunLifespanOnTick(f.reg, 10001, f.Gate());
    EXPECT_EQ(f.reg.get<C::CreatureGenomeComponent>(full).age_ticks, 122u);
    EXPECT_EQ(f.reg.get<C::MortalComponent>(full).age_ticks, 2u);
    f.reg.get<C::CreatureComponent>(full).eaten = true;
    A::RunLifespanOnTick(f.reg, 10002, f.Gate());
    EXPECT_EQ(f.reg.get<C::MortalComponent>(full).age_ticks, 2u);
    EXPECT_EQ(f.reg.get<C::MortalComponent>(full).dead, 1u);
}

TEST(CreatureRegionGates, DecayAdvancesOncePerEligibleTierStep) {
    Cohort f;
    auto corpse = f.Creature(1);
    f.reg.get<C::CreatureComponent>(corpse).eaten = true;
    f.reg.emplace<C::DecayComponent>(corpse);
    A::RunDecompositionOnTick(f.reg, 1, f.Gate());
    EXPECT_EQ(f.reg.get<C::DecayComponent>(corpse).decay_ticks, 1u);
    for (auto b : {Block::Frozen, Block::ReducedOffDue, Block::Coarse}) {
        Cohort held;
        auto e = held.Creature(1);
        held.reg.get<C::CreatureComponent>(e).eaten = true;
        held.reg.emplace<C::DecayComponent>(e);
        held.BlockEntity(e, b);
        const auto before = O::Fields(held.reg, e);
        A::RunDecompositionOnTick(held.reg, 10000, held.Gate());
        EXPECT_EQ(O::Fields(held.reg, e), before);
    }
    // The session regression below exercises real stamp/window ownership across transitions.
}

TEST(CreatureRegionGates, WildlifeGatesCreatureSourcesAndPlantRecipientsSeparately) {
    Matrix([](Block b, bool source, bool recipient) {
        Cohort f;
        const auto grazer = f.Creature(1), plant = f.Creature(2, 1);
        f.reg.remove<C::CreatureComponent>(plant);
        f.reg.get<C::CreatureComponent>(grazer).hunger = .8f;
        f.reg.emplace<C::GrazeableComponent>(plant).biomass = .5f;
        if (source)
            f.BlockEntity(grazer, b);
        if (recipient)
            f.BlockEntity(plant, b);
        const auto before = O::Fields(f.reg, plant), animal = O::Fields(f.reg, grazer);
        const auto stats = A::RunWildlifeFoliageOnTick(f.reg, 7, {}, f.Gate(), f.Gate(), {});
        EXPECT_EQ(stats.grazed_plants, !source && !recipient ? 1 : 0);
        if (recipient)
            EXPECT_EQ(O::Fields(f.reg, plant), before);
        if (source || recipient)
            EXPECT_EQ(O::Fields(f.reg, grazer), animal);
        else
            EXPECT_LT(f.reg.get<C::CreatureComponent>(grazer).hunger, .8f);
    });
}

TEST(CreatureRegionGates, WildlifePersistentOrderingSurvivesFoliageStaging) {
    Cohort f;
    auto a = f.Creature(30), b = f.Creature(10);
    const auto plant = f.reg.create();
    f.reg.emplace<C::TransformComponent>(plant);
    f.reg.emplace<C::GrazeableComponent>(plant);
    f.reg.emplace<C::PlantRegionComponent>(plant).persistent_id = 20;
    const W::RegionEntityGate all = [](auto) {
        return true;
    };
    std::set<entt::entity> retained;
    auto stage = luminumbra::foliage::StageFoliageRegistry(f.reg, retained);
    EXPECT_EQ(W::RegionEntityIdentity(stage, a, true), 30u);
    EXPECT_EQ(W::RegionEntityIdentity(stage, b, true), 10u);
    EXPECT_TRUE(stage.all_of<C::CreatureCoarseComponent>(a));
    std::vector<std::uint64_t> order;
    A::RunWildlifeFoliageOnTick(stage, 1, {}, all, all, [&](auto e) {
        order.push_back(W::RegionEntityIdentity(stage, e, true));
    });
    EXPECT_EQ(order, (std::vector<std::uint64_t>{10, 30, 20}));
    luminumbra::foliage::CommitFoliageRegistry(f.reg, stage, retained);
    EXPECT_EQ(f.reg.get<C::GrazeableComponent>(plant).last_grazed_tick, 1u);
}

void ProductionLegacyTick(entt::registry& reg, std::uint64_t tick, std::uint64_t seed) {
    A::RunCreatureBrainSystemOnTick(reg, kDt);
    A::RunMateSeekingOnTick(reg);
    A::RunSteeringConsumerOnTick(reg);
    A::RunThirstOnTick(reg, kDt);
    A::RunScavengingOnTick(reg, tick);
    A::RunSurvivalWishBlendOnTick(reg);
    A::RunMatingResolveOnTick(reg, tick, seed);
    A::RunWildlifeFoliageOnTick(reg, tick);
    A::RunLifespanOnTick(reg, tick);
    A::RunHerdAlarmOnTick(reg, kDt);
    A::RunDecompositionOnTick(reg, tick);
    A::RunCircadianOnTick(reg, static_cast<float>(tick % 36000) / 36000.0f);
    A::RunTerritoryOnTick(reg, tick);
    A::RunPredatorPackOnTick(reg, tick);
    A::RunMigrationOnTick(reg, static_cast<float>(tick % 108000) / 108000.0f);
}
TEST(CreatureRegionGates, FlagOffMatchesIndependentLegacyCreatureOracle) {
    entt::registry actual, expected;
    O::MakeRoster(actual);
    O::MakeRoster(expected);
    const auto initial = actual.view<C::CreatureComponent>().size();
    for (std::uint64_t tick = 1; tick <= 100; ++tick) {
        SCOPED_TRACE(tick);
        if (tick == 50) {
            // Reuse an EnTT generation without depending on a production admission helper.
            for (auto* reg : {&actual, &expected}) {
                const auto victim = static_cast<entt::entity>(6);
                ASSERT_TRUE(reg->valid(victim));
                reg->destroy(victim);
                O::AddCreature(*reg, 600, 0);
            }
        }
        ProductionLegacyTick(actual, tick, 1337);
        O::Tick(expected, tick, 1337);
        EXPECT_EQ(O::Fields(actual), O::Fields(expected));
        EXPECT_EQ(A::ComputeEcologySubHash(actual), O::ComputeEcologySubHash(expected));
    }
    EXPECT_GT(actual.view<C::CreatureComponent>().size(), initial);
    EXPECT_GT(actual.get<C::DecayComponent>(static_cast<entt::entity>(4)).decay_ticks, 0u);
}
} // namespace
