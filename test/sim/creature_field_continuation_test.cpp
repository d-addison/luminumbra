#include <gtest/gtest.h>

#include "../support/LegacyCreatureTickOracle.h"
#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_common/ai/CreaturePersistence.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/fields/FieldPageState.h"
#include "luminumbra_common/world/GameSession.h"
#include <bit>
#include <filesystem>
#include <limits>
#include <map>

namespace {
namespace A = luminumbra::ai;
namespace C = Luminumbra::Components;
namespace W = Luminumbra::world;
namespace O = LegacyCreatureTickOracle;
namespace fs = std::filesystem;

struct SessionFixture {
    fs::path root = Luminumbra::TestSupport::CreateUniqueTempDirectory("creature_fields_");
    Luminumbra::JobSystem jobs;
    SessionFixture() {
        jobs.startup(1);
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        Luminumbra::TestSupport::WriteSimulationContentFixture(root);
        fs::remove(root / "data/common/biomes.json");
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    }
    ~SessionFixture() {
        jobs.shutdown();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void Configure(W::GameSession& s) {
        s.SetRootPath(Luminumbra::Filesystem::GenericUtf8PathString(root) + "/");
        s.SetJobSystem(&jobs);
        s.SetActiveRegionsEnabled(true);
        s.SetAetherStateEnabled(true);
        s.SetRegionSchedulerConfig(
            {std::numeric_limits<std::uint64_t>::max(), 10000u, std::uint8_t{2}});
    }
    void Create(W::GameSession& s) {
        Configure(s);
        ASSERT_TRUE(s.CreateWorld("Creature fields", "1337", "default"));
        s.SetReplicatedSimulationAnchors({});
    }
};
W::ActiveRegionLedger& Ledger(W::GameSession& s) {
    return const_cast<W::ActiveRegionLedger&>(s.GetActiveRegionLedger());
}
entt::entity Creature(W::GameSession& s, const Luminumbra::Vec3& pos) {
    auto& ledger = Ledger(s);
    std::uint64_t id = 0;
    if (!ledger.allocate_persistent_id(id))
        throw std::runtime_error("Fixture identity exhausted");
    s.NotifyGroundObjectEdit(pos);
    const auto e = s.GetRegistry().create();
    auto& r = s.GetRegistry();
    r.emplace<C::TransformComponent>(e).position = pos;
    auto& cr = r.emplace<C::CreatureComponent>(e);
    cr.species_id = C::CreatureSpeciesId16("fixture_creature");
    cr.move_speed = 0;
    const auto key = W::ActiveRegionLedger::region_at(pos);
    r.emplace<C::CreatureRegionComponent>(e, id, key.x, key.z, s.GetWorldClock().tick());
    auto& c = r.emplace<C::CreatureCoarseComponent>(e);
    c.x_mm = static_cast<std::int64_t>(std::round(double(pos.x) * 1000));
    c.y_mm = static_cast<std::int64_t>(std::round(double(pos.y) * 1000));
    c.z_mm = static_cast<std::int64_t>(std::round(double(pos.z) * 1000));
    r.emplace<C::CreatureControllerComponent>(e);
    return e;
}
entt::entity Find(W::GameSession& s, std::uint64_t id) {
    for (auto e : s.GetRegistry().view<C::CreatureRegionComponent>())
        if (s.GetRegistry().get<C::CreatureRegionComponent>(e).persistent_id == id)
            return e;
    return entt::null;
}
void Tick(W::GameSession& s) {
    ASSERT_EQ(s.TickSimulation(s.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(s.GetRegionSchedulerFault());
}
void Freeze(W::GameSession& s, W::RegionKey key) {
    auto& r =
        const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(Ledger(s).records()).at(key);
    r.state = W::RegionState::Frozen;
    r.frozen_at = s.GetWorldClock().tick();
    r.frozen_digest = 123;
    r.cadence_shift = 2;
    r.cadence_phase = W::ActiveRegionLedger::phase(key, 2);
    r.wake_pending = false;
    r.under_hold = 0;
    r.over_hold = 0;
}
std::string Snapshot(W::GameSession& s) {
    return A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(s.GetRegistry()));
}
void Pair(W::GameSession& s) {
    const auto a = Creature(s, {8, 0, 8}), b = Creature(s, {10, 0, 8});
    auto& r = s.GetRegistry();
    O::AddSecondaryAgent(r, a, b, s.ScentWorldToCellX(8), s.ScentWorldToCellZ(8));
    O::AddOpportunity(r, b);
    auto& food = r.emplace<C::FoodSourceComponent>(b);
    food.cell_x = s.ScentWorldToCellX(10);
    food.cell_z = s.ScentWorldToCellZ(8);
    r.emplace<C::SensableComponent>(b).faction = 1;
    r.get<C::LocomotionPathComponent>(a).waypoints = {{8, 8}, {9, 8}};
}

TEST(CreatureFieldContinuation, SavedReferencesProduceTheSamePlansAndTargets) {
    SessionFixture f;
    W::GameSession original, loaded;
    ASSERT_NO_FATAL_FAILURE(f.Create(original));
    Pair(original);
    Tick(original);
    ASSERT_TRUE(original.SaveWorld());
    f.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    loaded.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(Snapshot(original), Snapshot(loaded));
    for (std::uint64_t tick = 0; tick < 5; ++tick) {
        Tick(original);
        Tick(loaded);
        EXPECT_EQ(Snapshot(original), Snapshot(loaded));
        const auto target =
            loaded.GetRegistry().get<C::ActionPlanComponent>(Find(loaded, 1)).plan.front().target;
        EXPECT_EQ(loaded.GetRegistry().get<C::CreatureRegionComponent>(target).persistent_id, 2u);
    }
    // Resolve the same saved references into a registry with a different allocation history.
    entt::registry reordered;
    const auto unused = reordered.create();
    reordered.destroy(unused);
    const auto runtime_entity = reordered.create();
    reordered.emplace<C::TransformComponent>(runtime_entity).position = {12, 34, 56};
    A::ApplyCreatureSnapshot(reordered, A::BuildCreatureSnapshot(original.GetRegistry()));
    ASSERT_TRUE(reordered.valid(runtime_entity));
    EXPECT_EQ(reordered.get<C::TransformComponent>(runtime_entity).position,
              Luminumbra::Vec3(12, 34, 56));
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(reordered)),
              Snapshot(original));
}

TEST(CreatureFieldContinuation, ScentAndEnergyContinueAcrossSaveLoadWithCreatureEmitters) {
    SessionFixture f;
    W::GameSession original, loaded;
    ASSERT_NO_FATAL_FAILURE(f.Create(original));
    Pair(original);
    for (int i = 0; i < 3; ++i)
        Tick(original);
    ASSERT_FALSE(original.GetFieldPages().scent_cells().empty());
    ASSERT_FALSE(original.GetEnergyFieldState()->CanonicalRegionalBytes().empty());
    ASSERT_TRUE(original.SaveWorld());
    f.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    loaded.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(original.ComputeScentSubHash(), loaded.ComputeScentSubHash());
    EXPECT_EQ(original.ComputeAetherStateSubHash(), loaded.ComputeAetherStateSubHash());
    for (int i = 0; i < 8; ++i) {
        Tick(original);
        Tick(loaded);
        EXPECT_EQ(Snapshot(original), Snapshot(loaded));
        EXPECT_EQ(original.GetFieldPages().scent_cells(), loaded.GetFieldPages().scent_cells());
        EXPECT_EQ(original.GetEnergyFieldState()->CanonicalRegionalBytes(),
                  loaded.GetEnergyFieldState()->CanonicalRegionalBytes());
        EXPECT_EQ(Ledger(original).energy_ages(), Ledger(loaded).energy_ages());
    }
}

TEST(CreatureFieldContinuation, FrozenActorsCannotChangePagesAcrossOwnerBoundaries) {
    for (auto state : {W::RegionState::Frozen, W::RegionState::Reduced, W::RegionState::Active}) {
        SessionFixture f;
        W::GameSession s;
        ASSERT_NO_FATAL_FAILURE(f.Create(s));
        const auto source = Creature(s, {-1, 0, 8}), recipient = Creature(s, {1, 0, 8});
        auto& r = s.GetRegistry();
        O::AddSecondaryAgent(r, source, recipient, s.ScentWorldToCellX(-1), s.ScentWorldToCellZ(8));
        auto& food = r.emplace<C::FoodSourceComponent>(recipient);
        food.cell_x = s.ScentWorldToCellX(1);
        food.cell_z = s.ScentWorldToCellZ(8);
        food.amount = 10;
        // Warm the real scheduler, then remove all field authority to isolate new writes.
        Tick(s);
        r.get<C::FoodSourceComponent>(recipient).amount = 10;
        s.GetEnergyFieldState()->ResetEmptyAtTick(s.GetWorldClock().tick());
        const_cast<W::FieldPageState&>(s.GetFieldPages()).clear();
        for (int ch = 0; ch < 4; ++ch)
            for (int z = 0; z < s.ScentFieldCells(); ++z)
                for (int x = 0; x < s.ScentFieldCells(); ++x)
                    s.GetScentField()->RestoreCell(ch, x, z, 0);
        const auto key = W::RegionKey{-1, 0};
        auto& record =
            const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(Ledger(s).records()).at(key);
        if (state == W::RegionState::Frozen)
            Freeze(s, key);
        if (state == W::RegionState::Reduced) {
            record.state = state;
            record.cadence_shift = 2;
            record.cadence_phase = 3;
        }
        if (state == W::RegionState::Active)
            r.get<C::CreatureCoarseComponent>(source).tier = C::CreatureTier::Coarse;
        const auto before = O::SecondaryFields(r, source);
        Tick(s);
        EXPECT_EQ(O::SecondaryFields(r, source), before);
        EXPECT_TRUE(s.GetFieldPages().scent_cells().empty());
        EXPECT_TRUE(s.GetEnergyFieldState()->CanonicalRegionalBytes().empty());
        EXPECT_EQ(r.get<C::FoodSourceComponent>(recipient).amount, 10);
        // Full Active control demonstrates a source with recipients on both sides.
        auto& active =
            const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(Ledger(s).records()).at(key);
        active.state = W::RegionState::Active;
        active.cadence_shift = 0;
        active.cadence_phase = 0;
        active.frozen_at = 0;
        active.frozen_digest = 0;
        r.get<C::CreatureCoarseComponent>(source).tier = C::CreatureTier::Full;
        Tick(s);
        EXPECT_FALSE(s.GetFieldPages().scent_cells().empty());
        EXPECT_GT(s.GetEnergyFieldState()->at_cell(0, 0), 0u);
    }
}

TEST(CreatureFieldContinuation, PriorScentSurvivesEmitterFreezeWithoutNewDeposits) {
    SessionFixture f;
    W::GameSession original, loaded;
    ASSERT_NO_FATAL_FAILURE(f.Create(original));
    const auto emitter = Creature(original, {-1, 0, 8});
    auto& s = original.GetRegistry().emplace<C::SensableComponent>(emitter);
    s.scent_channel = 0;
    s.scent_deposit = 10;
    original.NotifyGroundObjectEdit({1, 0, 8});
    Tick(original);
    const auto before = original.GetFieldPages().scent_cells();
    ASSERT_FALSE(before.empty());
    Freeze(original, {-1, 0});
    ASSERT_TRUE(original.SaveWorld());
    f.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    loaded.SetReplicatedSimulationAnchors({});
    bool active_history = false;
    for (const auto& [cell, value] : before)
        if (W::FieldPageState::region_of_scent_cell(cell) == W::RegionKey{0, 0} &&
            std::bit_cast<double>(value[0]) > 1e-8)
            active_history = true;
    ASSERT_TRUE(active_history);
    for (int i = 0; i < 3; ++i) {
        Tick(original);
        Tick(loaded);
    }
    const auto& after = original.GetFieldPages().scent_cells();
    EXPECT_EQ(after, loaded.GetFieldPages().scent_cells());
    bool decayed = false;
    for (const auto& [cell, value] : before) {
        if (W::FieldPageState::region_of_scent_cell(cell) == W::RegionKey{-1, 0}) {
            EXPECT_EQ(after.at(cell), value);
        } else if (std::bit_cast<double>(value[0]) > 1e-8 &&
                   std::bit_cast<double>(after.at(cell)[0]) < std::bit_cast<double>(value[0])) {
            decayed = true;
        }
    }
    EXPECT_TRUE(decayed);
}
} // namespace
