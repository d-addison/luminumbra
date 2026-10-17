#include <gtest/gtest.h>

#include "../support/LegacyCreatureTickOracle.h"
#include "ai/ForagingSystem.h"
#include "ai/InstinctLocomotionSystem.h"
#include "ai/InstinctSystem.h"
#include "ai/PerceptionSystem.h"
#include "ai/ScentDepositSystem.h"
#include "ai/ScentSteeringSystem.h"
#include "systems/FieldEmitterSystem.h"
#include "world/ActiveRegionLedger.h"
#include <array>
#include <bit>
#include <map>
#include <set>

namespace {
namespace A = luminumbra::ai;
namespace C = Luminumbra::Components;
namespace W = Luminumbra::world;
namespace O = LegacyCreatureTickOracle;
namespace E = luminumbra::fields;

// Sealed owners deliberately differ from positions: interactions must consult
// the admission cohort, including when two participants occupy the same cell.
enum class Block {
    Frozen,
    ReducedOffDue,
    Coarse,
    ReducedDue,
    Absent
};
struct Cohort {
    entt::registry reg;
    std::map<W::RegionKey, W::ActiveRegionRecord> records;
    std::set<W::RegionKey> due;
    entt::entity Entity(std::uint64_t id, float x = 4, float z = 4) {
        const auto e = O::AddCreature(reg, x, z);
        reg.emplace<C::CreatureRegionComponent>(e, id, static_cast<int>(id), 0, std::uint64_t{0});
        reg.emplace<C::CreatureCoarseComponent>(e);
        records[{static_cast<int>(id), 0}] = {};
        due.insert({static_cast<int>(id), 0});
        return e;
    }
    void BlockEntity(entt::entity e, Block why) {
        const auto& owner = reg.get<C::CreatureRegionComponent>(e);
        const W::RegionKey key{owner.owner_x, owner.owner_z};
        if (why == Block::Frozen)
            records.at(key).state = W::RegionState::Frozen;
        if (why == Block::ReducedOffDue || why == Block::ReducedDue)
            records.at(key).state = W::RegionState::Reduced;
        if (why == Block::ReducedOffDue)
            due.erase(key);
        if (why == Block::Coarse)
            reg.get<C::CreatureCoarseComponent>(e).tier = C::CreatureTier::Coarse;
        if (why == Block::Absent)
            records.erase(key);
    }
    W::RegionEntityGate Gate() {
        // Capture once, as the session does; writes and stamp changes cannot admit newborns.
        std::set<entt::entity> admitted;
        for (auto e : reg.view<C::CreatureRegionComponent, C::CreatureCoarseComponent>()) {
            const auto& owner = reg.get<C::CreatureRegionComponent>(e);
            const auto found = records.find({owner.owner_x, owner.owner_z});
            if (reg.get<C::CreatureCoarseComponent>(e).tier == C::CreatureTier::Full &&
                found != records.end() && found->second.state == W::RegionState::Active &&
                due.contains(found->first))
                admitted.insert(e);
        }
        return [admitted](auto e) {
            return admitted.contains(e);
        };
    }
};
template<class F>
void Matrix(F test) {
    for (const auto b :
         {Block::Frozen, Block::ReducedOffDue, Block::Coarse, Block::ReducedDue, Block::Absent})
        for (int mask = 0; mask < 4; ++mask) {
            SCOPED_TRACE(static_cast<int>(b));
            SCOPED_TRACE(mask);
            test(b, bool(mask & 1), bool(mask & 2));
        }
}
std::string Fields(const entt::registry& r, entt::entity e) {
    return O::Fields(r, e) + O::SecondaryFields(r, e);
}
void Agent(Cohort& f, entt::entity e, entt::entity target) {
    O::AddSecondaryAgent(f.reg, e, target);
}

TEST(CreatureSecondaryRegionGates, InstinctGatesNeedGrowthStimuliPlanningAndOpportunities) {
    Matrix([](Block b, bool actor_blocked, bool source_blocked) {
        for (bool substrate : {false, true}) {
            Cohort f;
            const auto actor = f.Entity(1), source = f.Entity(2, 5);
            Agent(f, actor, source);
            O::AddOpportunity(f.reg, source);
            if (actor_blocked)
                f.BlockEntity(actor, b);
            if (source_blocked)
                f.BlockEntity(source, b);
            const auto before = Fields(f.reg, actor), before_source = Fields(f.reg, source);
            A::StimulusContext ctx;
            ctx.tick = 1800;
            const A::StimulusChannelRegistry stimuli(ctx);
            const auto stats = A::RunInstinctSystemOnTick(f.reg, 7, &stimuli, substrate, f.Gate());
            EXPECT_EQ(stats.agents_seen, actor_blocked ? 0u : 1u);
            EXPECT_EQ(stats.stimulus_subscribers_applied, actor_blocked ? 0u : 1u);
            if (actor_blocked) {
                EXPECT_EQ(Fields(f.reg, actor), before);
            } else {
                EXPECT_GT(f.reg.get<C::NeedsComponent>(actor).needs.front().pressure, .41f);
                EXPECT_EQ(f.reg.get<C::ActionPlanComponent>(actor).plan.empty(), source_blocked);
                if (!source_blocked) {
                    EXPECT_EQ(f.reg.get<C::ActionPlanComponent>(actor).plan.front().target, source);
                }
            }
            EXPECT_EQ(Fields(f.reg, source), before_source);
        }
    });
    Cohort f;
    const auto actor = f.Entity(1);
    f.reg.emplace<C::InstinctAgentComponent>(actor);
    f.reg.emplace<C::NeedsComponent>(actor).needs = {{"food", .5f, .1f}};
    f.BlockEntity(actor, Block::Frozen);
    A::RunInstinctSystemOnTick(f.reg, 1, nullptr, true, f.Gate());
    EXPECT_FALSE(f.reg.all_of<C::ActionPlanComponent>(actor));
}

TEST(CreatureSecondaryRegionGates, InstinctPersistentTiesMatchInlineAndSubstrate) {
    for (bool reverse : {false, true})
        for (bool substrate : {false, true}) {
            Cohort f;
            const auto actor = f.Entity(1);
            const auto a = f.Entity(reverse ? 30 : 20, 5), b = f.Entity(reverse ? 20 : 30, 5);
            Agent(f, actor, a);
            O::AddOpportunity(f.reg, a, "same");
            O::AddOpportunity(f.reg, b, "same");
            A::RunInstinctSystemOnTick(f.reg, 1, nullptr, substrate, f.Gate());
            auto target = f.reg.get<C::ActionPlanComponent>(actor).plan.front().target;
            EXPECT_EQ(f.reg.get<C::CreatureRegionComponent>(target).persistent_id, 20u);
            // Distinct scores behind the same authored ID must resolve the real winner.
            f.reg.get<C::OpportunityComponent>(reverse ? a : b).satisfaction = 1;
            A::RunInstinctSystemOnTick(f.reg, 10, nullptr, substrate, f.Gate());
            target = f.reg.get<C::ActionPlanComponent>(actor).plan.front().target;
            EXPECT_EQ(f.reg.get<C::CreatureRegionComponent>(target).persistent_id, 30u);
        }
}

TEST(CreatureSecondaryRegionGates, LocomotionGatesActorsTargetsNeighborsAndPathProgress) {
    Matrix([](Block b, bool actor_blocked, bool target_blocked) {
        Cohort f;
        const auto actor = f.Entity(1), target = f.Entity(2, 4);
        Agent(f, actor, target);
        auto& path = f.reg.get<C::LocomotionPathComponent>(actor);
        path.waypoints = {{4, 4}};
        if (actor_blocked)
            f.BlockEntity(actor, b);
        if (target_blocked)
            f.BlockEntity(target, b);
        const auto before = Fields(f.reg, actor);
        A::RunInstinctLocomotionOnTick(f.reg, f.Gate());
        if (actor_blocked) {
            EXPECT_EQ(Fields(f.reg, actor), before);
        } else {
            EXPECT_EQ(path.index, target_blocked ? 0u : 1u);
            EXPECT_EQ(f.reg.get<C::ActionPlanComponent>(actor).current_action_index, 0u);
            EXPECT_EQ(f.reg.get<C::ActionPlanComponent>(actor).time_in_current_action, .25f);
            EXPECT_EQ(f.reg.get<C::LocomotionIntentComponent>(actor).wish_xz.x, 0);
        }
    });
    for (const auto b : {Block::Frozen, Block::ReducedOffDue, Block::Coarse}) {
        Cohort f;
        const auto actor = f.Entity(1), target = f.Entity(2, 15), neighbor = f.Entity(3, 4, 5);
        Agent(f, actor, target);
        Agent(f, neighbor, target);
        f.reg.remove<C::LocomotionPathComponent>(actor);
        f.BlockEntity(neighbor, b);
        A::RunInstinctLocomotionOnTick(f.reg, f.Gate());
        EXPECT_FLOAT_EQ(f.reg.get<C::LocomotionIntentComponent>(actor).wish_xz.y, 0);
        // Eligible neighbor is a real steering opportunity.
        f.records[{3, 0}] = {};
        f.due.insert({3, 0});
        f.reg.get<C::CreatureCoarseComponent>(neighbor).tier = C::CreatureTier::Full;
        A::RunInstinctLocomotionOnTick(f.reg, f.Gate());
        EXPECT_NE(f.reg.get<C::LocomotionIntentComponent>(actor).wish_xz.y, 0);
    }
}

TEST(CreatureSecondaryRegionGates, PerceptionGatesSensablesAndPerceivers) {
    Matrix([](Block b, bool actor_blocked, bool source_blocked) {
        Cohort f;
        const auto actor = f.Entity(1), source = f.Entity(2, 5);
        f.reg.emplace<C::PerceptionComponent>(actor);
        f.reg.emplace<C::AwarenessComponent>(actor).awareness.meter = .4f;
        f.reg.emplace<C::SensableComponent>(source).faction = 1;
        if (actor_blocked)
            f.BlockEntity(actor, b);
        if (source_blocked)
            f.BlockEntity(source, b);
        const auto before = Fields(f.reg, actor);
        A::RunPerceptionSystemOnTick(f.reg, .1f, f.Gate());
        if (actor_blocked) {
            EXPECT_EQ(Fields(f.reg, actor), before);
        } else if (source_blocked) {
            EXPECT_LT(f.reg.get<C::AwarenessComponent>(actor).awareness.meter, .4f);
        } else {
            EXPECT_GT(f.reg.get<C::AwarenessComponent>(actor).awareness.meter, .4f);
            EXPECT_EQ(f.reg.get<C::AwarenessComponent>(actor).awareness.last_known_x, 5);
        }
    });
}

TEST(CreatureSecondaryRegionGates, ScentDepositRequiresEligibleActorAndCell) {
    Matrix([](Block b, bool actor_blocked, bool cell_blocked) {
        Cohort f;
        const auto e = f.Entity(1);
        auto& s = f.reg.emplace<C::SensableComponent>(e);
        s.scent_channel = 0;
        s.scent_deposit = 5;
        if (actor_blocked)
            f.BlockEntity(e, b);
        A::ScentField field(8, 8);
        field.BindDepositGate([=](int, int) { return !cell_blocked; });
        const auto stats = A::RunScentDepositOnTick(f.reg, field, 0, 0, 1, f.Gate());
        EXPECT_EQ(stats.emitters, actor_blocked || cell_blocked ? 0u : 1u);
        EXPECT_EQ(field.Sample(0, 4, 4), actor_blocked || cell_blocked ? 0 : 5);
    });
}

TEST(CreatureSecondaryRegionGates, ScentSteeringLeavesBlockedIntentBitExact) {
    for (auto b :
         {Block::Frozen, Block::ReducedOffDue, Block::Coarse, Block::ReducedDue, Block::Absent}) {
        Cohort f;
        const auto e = f.Entity(1), target = f.Entity(2);
        Agent(f, e, target);
        A::ScentField field(8, 8);
        field.Deposit(0, 5, 4, 10);
        f.BlockEntity(e, b);
        const auto before = Fields(f.reg, e);
        A::RunScentSteeringOnTick(f.reg, field, 0, 0, 1, f.Gate());
        EXPECT_EQ(Fields(f.reg, e), before);
        f.records[{1, 0}] = {};
        f.due.insert({1, 0});
        f.reg.get<C::CreatureCoarseComponent>(e).tier = C::CreatureTier::Full;
        EXPECT_EQ(A::RunScentSteeringOnTick(f.reg, field, 0, 0, 1, f.Gate()).steered, 1u);
    }
}

TEST(CreatureSecondaryRegionGates, ForagingGatesActorsFoodAndDestinationCells) {
    Matrix([](Block b, bool actor_blocked, bool food_blocked) {
        Cohort f;
        const auto ant = f.Entity(1), food = f.Entity(2);
        auto& a = f.reg.emplace<C::ForagerComponent>(ant);
        a.cell_x = 4;
        a.cell_z = 4;
        auto& fc = f.reg.emplace<C::FoodSourceComponent>(food);
        fc.cell_x = 5;
        fc.cell_z = 4;
        fc.amount = 2;
        if (actor_blocked)
            f.BlockEntity(ant, b);
        if (food_blocked)
            f.BlockEntity(food, b);
        const auto before = Fields(f.reg, ant), source_before = Fields(f.reg, food);
        A::ScentField field(8, 8, 4);
        A::RunForagingOnTick(f.reg, field, {}, f.Gate());
        EXPECT_EQ(a.carrying_food, !actor_blocked && !food_blocked);
        if (actor_blocked) {
            EXPECT_EQ(Fields(f.reg, ant), before);
            EXPECT_EQ(field.Sample(3, 4, 4), 0);
        }
        if (food_blocked) {
            EXPECT_EQ(Fields(f.reg, food), source_before);
        }
    });
    for (int sealed : {0, 1, 2})
        for (bool carrying : {false, true}) {
            Cohort f;
            const auto ant = f.Entity(1), food = f.Entity(2);
            auto& a = f.reg.emplace<C::ForagerComponent>(ant);
            a.cell_x = 4;
            a.cell_z = 4;
            a.home_x = 5;
            a.home_z = 4;
            a.carrying_food = carrying;
            auto& fc = f.reg.emplace<C::FoodSourceComponent>(food);
            fc.cell_x = 5;
            fc.cell_z = 4;
            A::ScentField field(8, 8, 4);
            field.Deposit(carrying ? 3 : 2, 5, 4, 100);
            field.BindDepositGate([=](int x, int z) {
                return sealed == 2 || (sealed == 0 ? x != 4 || z != 4 : x != 5 || z != 4);
            });
            const auto before = Fields(f.reg, ant), source_before = Fields(f.reg, food);
            const A::ScentField::EdgeGate edge = [=](int, int, int, int) {
                return sealed != 2;
            };
            A::RunForagingOnTick(f.reg, field, {}, f.Gate(), {}, edge);
            EXPECT_EQ(Fields(f.reg, ant), before);
            EXPECT_EQ(Fields(f.reg, food), source_before);
            EXPECT_EQ(field.Sample(carrying ? 2 : 3, 4, 4), 0);
        }
}

TEST(CreatureSecondaryRegionGates, FieldEmitterGatesSourceAndEveryRecipientCell) {
    Matrix([](Block b, bool actor_blocked, bool cell_blocked) {
        Cohort f;
        const auto e = f.Entity(1, 24, 24);
        auto& em = f.reg.emplace<C::FieldEmitterComponent>(e);
        em.rate_raw_per_tick = 128;
        em.radius_cells = 1;
        if (actor_blocked)
            f.BlockEntity(e, b);
        E::EnergyFieldState field;
        const auto stats = Luminumbra::Systems::GatherFieldEmitterDeposits(
            f.reg, field, f.Gate(), [=](int x, int z) {
                return !cell_blocked || (x == 1 && z == 1);
            });
        const std::map<W::RegionKey, W::ActiveRegionRecord> records{{{0, 0}, {}}};
        const std::array<W::RegionKey, 1> due{{{0, 0}}};
        E::EnergyFieldState::RegionalResult out;
        ASSERT_TRUE(field.TickRegional(1, records, due, {}, out));
        EXPECT_EQ(stats.deposits, actor_blocked ? 0u : cell_blocked ? 1u : 9u);
        EXPECT_EQ(field.at_cell(1, 1), actor_blocked ? 0u : 128u);
        EXPECT_EQ(field.at_cell(2, 1), actor_blocked || cell_blocked ? 0u : 64u);
    });
    Cohort f;
    const auto e = f.Entity(1);
    auto& em = f.reg.emplace<C::FieldEmitterComponent>(e);
    em.rate_raw_per_tick = 1;
    em.radius_cells = 32;
    E::EnergyFieldState field;
    EXPECT_EQ(Luminumbra::Systems::GatherFieldEmitterDeposits(f.reg, field, f.Gate()).deposits, 0u);
}

TEST(CreatureSecondaryRegionGates, PersistentEmitterIdentityMakesDepositsRegistryIndependent) {
    auto run = [](bool reverse) {
        Cohort f;
        for (auto id : reverse ? std::array{20ull, 10ull} : std::array{10ull, 20ull}) {
            const auto e = f.Entity(id, 24, 24);
            auto& em = f.reg.emplace<C::FieldEmitterComponent>(e);
            em.rate_raw_per_tick = id == 10ull ? 65535u : 128u;
            em.radius_cells = 1;
        }
        std::vector<std::uint64_t> processed;
        E::EnergyFieldState field;
        Luminumbra::Systems::GatherFieldEmitterDeposits(f.reg, field, f.Gate(), {}, [&](auto e) {
            processed.push_back(f.reg.get<C::CreatureRegionComponent>(e).persistent_id);
        });
        EXPECT_EQ(processed, (std::vector<std::uint64_t>{10ull, 20ull}));
        // Read the actual queue: saturated field sums cannot distinguish IDs.
        // Runtime handles are 0/1 here, while persistent identities are 10/20.
        const auto queued_ids = field.PendingEmitterIdsForTesting();
        std::vector<std::uint64_t> expected_ids(9, 10ull);
        expected_ids.insert(expected_ids.end(), 9, 20ull);
        EXPECT_EQ(queued_ids, expected_ids);
        const std::map<W::RegionKey, W::ActiveRegionRecord> records{{{0, 0}, {}}};
        const std::array<W::RegionKey, 1> due{{{0, 0}}};
        E::EnergyFieldState::RegionalResult out;
        EXPECT_TRUE(field.TickRegional(1, records, due, {}, out));
        return field.CanonicalRegionalBytes();
    };
    EXPECT_EQ(run(false), run(true));
}

TEST(CreatureSecondaryRegionGates, FrozenReferenceHoldsThenResumesTheSameAction) {
    Cohort f;
    const auto actor = f.Entity(1), target = f.Entity(2);
    Agent(f, actor, target);
    f.reg.get<C::LocomotionPathComponent>(actor).waypoints = {{4, 4}};
    f.BlockEntity(target, Block::Frozen);
    const auto plan = f.reg.get<C::ActionPlanComponent>(actor);
    for (int tick = 0; tick < 3; ++tick)
        A::RunInstinctLocomotionOnTick(f.reg, f.Gate());
    EXPECT_EQ(f.reg.get<C::ActionPlanComponent>(actor).plan.front().target, target);
    EXPECT_EQ(f.reg.get<C::ActionPlanComponent>(actor).time_in_current_action,
              plan.time_in_current_action);
    EXPECT_EQ(f.reg.get<C::LocomotionPathComponent>(actor).index, 0u);
    f.records[{2, 0}].state = W::RegionState::Active;
    A::RunInstinctLocomotionOnTick(f.reg, f.Gate());
    EXPECT_EQ(f.reg.get<C::LocomotionPathComponent>(actor).index, 1u);
    A::RunInstinctLocomotionOnTick(f.reg, f.Gate());
    EXPECT_EQ(f.reg.get<C::ActionPlanComponent>(actor).current_action_index, 1u);
}

TEST(CreatureSecondaryRegionGates, FlagOffSecondarySystemsMatchLegacyReferences) {
    entt::registry actual, expected;
    for (auto* r : {&actual, &expected}) {
        const auto actor = O::AddCreature(*r, 4, 4), target = O::AddCreature(*r, 6, 4);
        O::AddSecondaryAgent(*r, actor, target);
        O::AddOpportunity(*r, target);
        auto& food = r->emplace<C::FoodSourceComponent>(target);
        food.cell_x = 5;
        food.cell_z = 4;
        r->emplace<C::SensableComponent>(target).faction = 1;
    }
    A::ScentField a(16, 16, 4), b(16, 16, 4);
    E::EnergyFieldState ea(2), eb(2);
    ea.SetAnchorCell(0, 0);
    eb.SetAnchorCell(0, 0);
    for (std::uint64_t tick = 1; tick <= 30; ++tick) {
        SCOPED_TRACE(tick);
        A::StimulusContext context;
        context.tick = tick;
        const A::StimulusChannelRegistry stimuli(context);
        A::RunInstinctSystemOnTick(actual, tick, &stimuli);
        O::RunInstinctSystemOnTick(expected, tick, &stimuli);
        A::RunPerceptionSystemOnTick(actual, .1f);
        O::RunPerceptionSystemOnTick(expected, .1f);
        A::RunScentDepositOnTick(actual, a, 0, 0, 1);
        O::RunScentDepositOnTick(expected, b, 0, 0, 1);
        A::RunForagingOnTick(actual, a);
        O::RunForagingOnTick(expected, b);
        a.Step(.25, 4, .05);
        b.Step(.25, 4, .05);
        a.Clamp(1e-9, 1e6);
        b.Clamp(1e-9, 1e6);
        A::RunInstinctLocomotionOnTick(actual);
        O::RunInstinctLocomotionOnTick(expected);
        A::RunScentSteeringOnTick(actual, a, 0, 0, 1);
        O::RunScentSteeringOnTick(expected, b, 0, 0, 1);
        Luminumbra::Systems::GatherFieldEmitterDeposits(actual, ea);
        O::GatherFieldEmitterDeposits(expected, eb);
        ea.Tick(tick);
        eb.Tick(tick);
        EXPECT_EQ(O::SecondaryFields(actual), O::SecondaryFields(expected));
        EXPECT_EQ(O::ScentBytes(a), O::ScentBytes(b));
        EXPECT_EQ(ea.CanonicalBytes(), eb.CanonicalBytes());
        if (tick == 15) {
            // Remove and reuse a live generation in both independent trajectories.
            for (auto* r : {&actual, &expected}) {
                const auto old = static_cast<entt::entity>(1);
                r->destroy(old);
                const auto replacement = O::AddCreature(*r, 6, 4);
                O::AddOpportunity(*r, replacement);
            }
        }
    }
}
} // namespace
