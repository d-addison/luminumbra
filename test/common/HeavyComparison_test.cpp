#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <system_error>

#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "luminumbra_server/HeavyComparison.h"

namespace {
using Luminumbra::Server::AuthoritativeStateEqual;
using Luminumbra::Server::CaptureGroundObjects;
using Luminumbra::Server::CaptureScentBits;
using Luminumbra::Server::GroundObjectDifference;
using Luminumbra::Server::HeavyHashes;
using Luminumbra::Server::HeavySubsystem;
using Luminumbra::Server::HeavySubsystemTable;
using Luminumbra::Server::MeshEqual;
using Luminumbra::Server::ScentDifference;
using Luminumbra::world::GameSession;
using Luminumbra::world::GroundObject;
using Luminumbra::world::GroundObjectStore;

HeavyHashes MakeBase(bool active_regions) {
    HeavyHashes h;
    h.world_hash = "world";
    h.sub.terrain = "terrain";
    h.sub.mesh = "mesh";
    h.sub.water = "water";
    h.sub.entities = "entities";
    h.sub.wind = "wind";
    h.sub.weather = "weather";
    h.sub.aether = "aether";
    h.sub.aether_state = "aether_state";
    h.scent_hash = "scents";
    h.scent_bits = {0, 0x3ff0000000000000ULL};
    h.active_regions = active_regions;
    h.clock_bytes = "clock";
    h.ledger_bytes = "ledger";
    return h;
}

const HeavySubsystem* FindRow(std::span<const HeavySubsystem> table, const std::string& name) {
    for (const auto& row : table) {
        if (name == row.name) {
            return &row;
        }
    }
    return nullptr;
}

// Compile-time guard: these structured bindings only compile while the structs have exactly
// this many members, so adding a hashed field without revisiting HeavySubsystemTable (and this
// function) fails the build instead of passing silently.
void ExpectFieldArityMatchesTable(const HeavyHashes& hashes, std::size_t table_rows) {
    [[maybe_unused]] const auto& [world_hash,
                                  sub,
                                  scent_hash,
                                  scent_bits,
                                  active_regions,
                                  clock_bytes,
                                  ledger_bytes,
                                  ground_objects,
                                  ground_notifications] = hashes;
    [[maybe_unused]] const auto& [terrain,
                                  mesh,
                                  water,
                                  entities,
                                  wind,
                                  weather,
                                  aether,
                                  aether_state] = hashes.sub;
    constexpr std::size_t kTopLevelFields = 9;
    constexpr std::size_t kSubFields = 8;
    // One table row per leaf field; `sub` is a container of the kSubFields rows.
    EXPECT_EQ(table_rows, kTopLevelFields - 1 + kSubFields);
}

TEST(HeavyComparison, TableCoversEveryHashField) {
    ExpectFieldArityMatchesTable(HeavyHashes{}, HeavySubsystemTable().size());
    const auto table = HeavySubsystemTable();
    const std::set<std::string> expected{"terrain",
                                         "water",
                                         "entities",
                                         "ground_objects",
                                         "ground_notifications",
                                         "active_regions",
                                         "clock",
                                         "ledger",
                                         "wind",
                                         "weather",
                                         "aether",
                                         "aether_state",
                                         "mesh",
                                         "scents",
                                         "scent_grid",
                                         "world_hash"};
    std::set<std::string> names;
    for (const auto& row : table) {
        names.insert(row.name);
        if (row.compared) {
            EXPECT_EQ(row.exempt_reason, nullptr) << row.name;
        } else {
            ASSERT_NE(row.exempt_reason, nullptr) << row.name;
            EXPECT_NE(std::string(row.exempt_reason), std::string()) << row.name;
        }
    }
    EXPECT_EQ(names, expected);
    EXPECT_EQ(names.size(), table.size());
    EXPECT_EQ(table.size(), expected.size());
}

TEST(HeavyComparison, EveryRowMutatesAndCompares) {
    const auto table = HeavySubsystemTable();
    const HeavyHashes base = MakeBase(true);
    EXPECT_TRUE(AuthoritativeStateEqual(base, base));
    EXPECT_TRUE(MeshEqual(base, base));

    for (const auto& row : table) {
        SCOPED_TRACE(row.name);
        ASSERT_NE(row.mutate, nullptr);
        HeavyHashes mutated = base;
        row.mutate(mutated);
        if (row.compared) {
            EXPECT_FALSE(AuthoritativeStateEqual(base, mutated));
        } else {
            EXPECT_TRUE(AuthoritativeStateEqual(base, mutated));
        }
    }
}

TEST(HeavyComparison, InactiveRegionsSkipsOptionalRows) {
    const auto table = HeavySubsystemTable();
    const HeavyHashes base = MakeBase(false);
    EXPECT_TRUE(AuthoritativeStateEqual(base, base));

    for (const auto& row : table) {
        SCOPED_TRACE(row.name);
        HeavyHashes mutated = base;
        row.mutate(mutated);
        if (row.compared && !row.requires_active_regions) {
            EXPECT_FALSE(AuthoritativeStateEqual(base, mutated));
        } else {
            EXPECT_TRUE(AuthoritativeStateEqual(base, mutated));
        }
    }
}

TEST(HeavyComparison, InactiveRegionsStillComparesAlwaysRows) {
    const HeavyHashes base = MakeBase(false);
    for (const char* name :
         {"terrain", "water", "entities", "ground_objects", "ground_notifications"}) {
        SCOPED_TRACE(name);
        const HeavySubsystem* row = FindRow(HeavySubsystemTable(), name);
        ASSERT_NE(row, nullptr);
        HeavyHashes mutated = base;
        row->mutate(mutated);
        EXPECT_FALSE(AuthoritativeStateEqual(base, mutated));
    }
}

TEST(HeavyComparison, OneSideActiveIsNotEqual) {
    const HeavyHashes active = MakeBase(true);
    const HeavyHashes inactive = MakeBase(false);
    EXPECT_FALSE(AuthoritativeStateEqual(active, inactive));
    EXPECT_FALSE(AuthoritativeStateEqual(inactive, active));
}

void CheckScentHashComparison() {
    for (const bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        const HeavyHashes base = MakeBase(active_regions);
        HeavyHashes changed = base;
        changed.scent_hash += "x";
        EXPECT_EQ(changed.scent_bits, base.scent_bits);
        EXPECT_EQ(changed.world_hash, base.world_hash);
        EXPECT_EQ(AuthoritativeStateEqual(base, changed), !active_regions);
        EXPECT_EQ(AuthoritativeStateEqual(changed, base), !active_regions);
        EXPECT_EQ(ScentDifference(base, changed), active_regions ? "scents" : "");
        EXPECT_EQ(ScentDifference(changed, base), active_regions ? "scents" : "");
    }
}

void CheckLiveScentGridPerturbation() {
    luminumbra::ai::ScentField field(3, 2, 4);
    field.RestoreCell(3, 2, 1, 8.0);
    const auto before = CaptureScentBits(field);
    ASSERT_EQ(before.size(), 24u);
    EXPECT_EQ(before.back(), std::bit_cast<std::uint64_t>(8.0));
    field.RestoreCell(3, 2, 1, std::bit_cast<double>(before.back() ^ std::uint64_t{1}));
    const auto after = CaptureScentBits(field);
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i + 1 < before.size(); ++i) {
        EXPECT_EQ(before[i], after[i]);
    }
    EXPECT_EQ(before.back() ^ after.back(), 1u);
    for (const bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        HeavyHashes base = MakeBase(active_regions);
        base.scent_bits = before;
        HeavyHashes changed = base;
        changed.scent_bits = after;
        EXPECT_EQ(changed.scent_hash, base.scent_hash);
        EXPECT_EQ(changed.ledger_bytes, base.ledger_bytes);
        EXPECT_EQ(AuthoritativeStateEqual(base, changed), !active_regions);
        EXPECT_EQ(AuthoritativeStateEqual(changed, base), !active_regions);
        EXPECT_EQ(ScentDifference(base, changed), active_regions ? "scent_grid" : "");
        EXPECT_EQ(ScentDifference(changed, base), active_regions ? "scent_grid" : "");
    }
}

TEST(HeavyComparison, ScentHashComparesOnlyWithActiveRegions) {
    CheckScentHashComparison();
}

TEST(HeavyComparison, LiveScentGridDetectsOneBitPerturbationIndependentlyOfHash) {
    CheckLiveScentGridPerturbation();
}

class HeavyScentSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        root_ = Luminumbra::TestSupport::CreateUniqueTempDirectory("heavy_scent_");
        const fs::path source(LUMINUMBRA_SOURCE_ROOT);
        const auto presets = root_ / "worlds/atlas/presets";
        fs::create_directories(presets);
        fs::copy_file(source / "worlds/atlas/presets/default.json", presets / "default.json");
        Luminumbra::TestSupport::CopyWorldgenTestData(source, root_);
        jobs_.startup(1);
    }

    void TearDown() override {
        jobs_.shutdown();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    void Configure(GameSession& session) {
        session.SetRootPath(root_.string() +
                            static_cast<char>(std::filesystem::path::preferred_separator));
        session.SetJobSystem(&jobs_);
        session.SetActiveRegionsEnabled(true);
        session.SetWaterHighResEnabled(false);
        session.SetWeatherRainEnabled(false);
        session.SetRegionSchedulerConfig({std::numeric_limits<std::uint64_t>::max(), 10000, 2});
    }

private:
    std::filesystem::path root_;
    Luminumbra::JobSystem jobs_;
};

HeavyHashes CaptureSessionState(GameSession& session) {
    HeavyHashes h;
    h.active_regions = session.ActiveRegionsEnabled();
    h.clock_bytes = session.GetWorldClock().canonical_bytes();
    h.ledger_bytes = session.GetActiveRegionLedger().canonical_bytes();
    h.ground_objects = CaptureGroundObjects(session.GetGroundObjects());
    h.ground_notifications = session.CaptureGroundNotifications().canonical_bytes();
    h.scent_hash = session.ComputeScentSubHash();
    h.scent_bits = CaptureScentBits(*session.GetScentField());
    auto* world = session.GetWorldSystem();
    world->wait_for_streaming_jobs();
    Luminumbra::WorldStreamingState state;
    for (const auto& chunk : world->snapshot_streamed_chunks()) {
        state.insert_chunk(chunk);
    }
    h.sub = Luminumbra::Persistence::ComputeWorldStreamingStateSubHashes(
        state, world->GetWaterSimWindowCursor());
    h.sub.wind = session.GetWindFieldSystem()->ComputeWindSubHash();
    h.sub.weather = session.GetWeatherSystem()->ComputeWeatherSubHash();
    h.sub.aether = session.GetAetherFieldSystem()->ComputeAetherSubHash();
    h.sub.aether_state = session.ComputeAetherStateSubHash();
    if (!h.sub.aether_state.empty()) {
        h.sub.aether =
            Luminumbra::Persistence::StableChecksum(h.sub.aether + "|state:" + h.sub.aether_state);
    }
    return h;
}

entt::entity AddScentEmitter(GameSession& session) {
    namespace C = Luminumbra::Components;
    auto& registry = session.GetRegistry();
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity).position = session.GetMetadata().spawnPoint;
    auto& emitter = registry.emplace<C::SensableComponent>(entity);
    emitter.scent_channel = 3;
    emitter.scent_deposit = 8.0f;
    return entity;
}

bool AdvanceScentSession(GameSession& session) {
    return session.TickSimulation(session.GetSimulationClock().fixed_dt()) == 1u &&
           !session.GetRegionSchedulerFault();
}

void ExpectSessionStateEqual(const HeavyHashes& a, const HeavyHashes& b) {
    EXPECT_EQ(a.scent_bits, b.scent_bits);
    EXPECT_EQ(a.scent_hash, b.scent_hash);
    EXPECT_TRUE(ScentDifference(a, b).empty());
    EXPECT_TRUE(AuthoritativeStateEqual(a, b));
    EXPECT_TRUE(AuthoritativeStateEqual(b, a));
}

void ExpectScentChanged(const HeavyHashes& before, const HeavyHashes& after) {
    EXPECT_NE(before.scent_bits, after.scent_bits);
    EXPECT_NE(before.scent_hash, after.scent_hash);
}

TEST_F(HeavyScentSessionTest, SavedScentMatchesReopenedSessionAndContinuedSimulation) {
    GameSession original, reopened;
    Configure(original);
    ASSERT_TRUE(original.CreateWorld("Heavy scent", "1337", "default"));
    original.SetReplicatedSimulationAnchors({});
    original.PinActiveRegion(
        Luminumbra::world::ActiveRegionLedger::region_at(original.GetMetadata().spawnPoint), true);
    const auto emitter = AddScentEmitter(original);
    ASSERT_TRUE(AdvanceScentSession(original));
    original.GetRegistry().destroy(emitter);
    ASSERT_TRUE(AdvanceScentSession(original));
    ASSERT_FALSE(original.GetFieldPages().scent_cells().empty());
    ASSERT_TRUE(original.SaveWorld());
    ASSERT_TRUE(
        Luminumbra::Persistence::WorldSaveService::validate_save(original.GetWorldSaveDir()));
    const auto saved = CaptureSessionState(original);
    ASSERT_FALSE(saved.scent_hash.empty());
    ASSERT_EQ(saved.scent_bits.size(), 65536u);

    Configure(reopened);
    ASSERT_TRUE(reopened.LoadWorld(original.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    ExpectSessionStateEqual(saved, CaptureSessionState(reopened));

    AddScentEmitter(original);
    AddScentEmitter(reopened);
    for (int tick = 0; tick < 3; ++tick) {
        SCOPED_TRACE(tick);
        ASSERT_TRUE(AdvanceScentSession(original));
        ASSERT_TRUE(AdvanceScentSession(reopened));
        ExpectSessionStateEqual(CaptureSessionState(original), CaptureSessionState(reopened));
    }
    ExpectScentChanged(saved, CaptureSessionState(original));
}

GroundObject MakeGroundObject() {
    return GroundObject{.id = 7,
                        .type = 2,
                        .x_mm = 110,
                        .y_mm = 220,
                        .z_mm = 330,
                        .spawn_tick = 11,
                        .active_age = 3,
                        .despawn_after = 20,
                        .payload = 42,
                        .flags = GroundObject::kSettled};
}

TEST(HeavyComparison, GroundObjectProjectionIsCanonicalAndIdenticalStatePasses) {
    GroundObjectStore original, loaded;
    const GroundObject first = MakeGroundObject();
    GroundObject second = first;
    second.id = 9;
    ASSERT_TRUE(original.insert(second));
    ASSERT_TRUE(original.insert(first));
    ASSERT_TRUE(loaded.insert(first));
    ASSERT_TRUE(loaded.insert(second));

    HeavyHashes a = MakeBase(true);
    HeavyHashes b = a;
    a.ground_objects = CaptureGroundObjects(original);
    b.ground_objects = CaptureGroundObjects(loaded);
    ASSERT_EQ(a.ground_objects.size(), 2u);
    EXPECT_EQ(a.ground_objects[0], first);
    EXPECT_EQ(a.ground_objects[1], second);
    EXPECT_TRUE(AuthoritativeStateEqual(a, b));
    EXPECT_TRUE(GroundObjectDifference(a, b).empty());
}

TEST(HeavyComparison, EveryDurableGroundObjectFieldMutatesAndCompares) {
    const GroundObject object = MakeGroundObject();
    // Guard against adding durable fields without updating the mutation coverage/diagnostics.
    [[maybe_unused]] const auto& [id,
                                  type,
                                  x_mm,
                                  y_mm,
                                  z_mm,
                                  spawn_tick,
                                  active_age,
                                  despawn_after,
                                  payload,
                                  flags] = object;
    struct Mutation {
        const char* field;
        void (*mutate)(GroundObject&);
    };
    const std::array<Mutation, 10> mutations{{
        {"id",
         [](GroundObject& o) {
             ++o.id;
         }},
        {"type",
         [](GroundObject& o) {
             ++o.type;
         }},
        {"x_mm",
         [](GroundObject& o) {
             ++o.x_mm;
         }},
        {"y_mm",
         [](GroundObject& o) {
             ++o.y_mm;
         }},
        {"z_mm",
         [](GroundObject& o) {
             ++o.z_mm;
         }},
        {"spawn_tick",
         [](GroundObject& o) {
             ++o.spawn_tick;
         }},
        {"active_age",
         [](GroundObject& o) {
             ++o.active_age;
         }},
        {"despawn_after",
         [](GroundObject& o) {
             ++o.despawn_after;
         }},
        {"payload",
         [](GroundObject& o) {
             ++o.payload;
         }},
        {"flags",
         [](GroundObject& o) {
             o.flags = 0;
         }},
    }};
    // Pure store fixtures: no world boot, terrain generation, persistence IO or resim ticks.
    GroundObjectStore original;
    ASSERT_TRUE(original.insert(object));
    for (bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        HeavyHashes base = MakeBase(active_regions);
        base.ground_objects = CaptureGroundObjects(original);
        EXPECT_TRUE(AuthoritativeStateEqual(base, base));
        for (const auto& mutation : mutations) {
            SCOPED_TRACE(mutation.field);
            GroundObject changed = object;
            mutation.mutate(changed);
            GroundObjectStore other;
            ASSERT_TRUE(other.insert(changed));
            HeavyHashes mutated = base;
            mutated.ground_objects = CaptureGroundObjects(other);
            // Even equal ledger bytes and an equal composite hash cannot mask the object change.
            EXPECT_EQ(base.ledger_bytes, mutated.ledger_bytes);
            EXPECT_EQ(base.world_hash, mutated.world_hash);
            EXPECT_FALSE(AuthoritativeStateEqual(base, mutated));
            EXPECT_FALSE(AuthoritativeStateEqual(mutated, base));
            EXPECT_EQ(GroundObjectDifference(base, mutated),
                      std::string("ground_objects[id=7].") + mutation.field);
        }
    }
}

TEST(HeavyComparison, GroundObjectPresenceMutatesAndCompares) {
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject()));
    HeavyHashes empty = MakeBase(true);
    HeavyHashes populated = empty;
    populated.ground_objects = CaptureGroundObjects(store);
    EXPECT_FALSE(AuthoritativeStateEqual(empty, populated));
    EXPECT_FALSE(AuthoritativeStateEqual(populated, empty));
    EXPECT_EQ(GroundObjectDifference(empty, populated), "ground_objects.size");
    EXPECT_EQ(GroundObjectDifference(populated, empty), "ground_objects.size");
}

TEST(HeavyComparison, GroundNotificationsCompareIndependentlyOfObjectsAndComposite) {
    const Luminumbra::world::GroundNotifications notifications{
        2, {{4, -1, 0, "ground.spawned", "first"}, {3, 2, 1, "ground.despawned", "second"}}};
    HeavyHashes base = MakeBase(true);
    base.ground_notifications = notifications.canonical_bytes();
    for (const bool cursor_only : {false, true}) {
        auto changed = notifications;
        if (cursor_only) {
            ++changed.next_sequence;
        } else {
            changed.events.front().payload += "x";
        }
        HeavyHashes mutated = base;
        mutated.ground_notifications = changed.canonical_bytes();
        EXPECT_EQ(base.world_hash, mutated.world_hash);
        EXPECT_EQ(base.ground_objects, mutated.ground_objects);
        EXPECT_FALSE(AuthoritativeStateEqual(base, mutated));
        EXPECT_FALSE(AuthoritativeStateEqual(mutated, base));
    }
    EXPECT_TRUE(AuthoritativeStateEqual(base, base));
}

TEST(HeavyComparison, MeshEqualTracksMeshOnly) {
    const HeavyHashes base = MakeBase(true);
    EXPECT_TRUE(MeshEqual(base, base));

    HeavyHashes mesh_changed = base;
    mesh_changed.sub.mesh += "x";
    EXPECT_FALSE(MeshEqual(base, mesh_changed));

    for (const auto& row : HeavySubsystemTable()) {
        if (std::string(row.name) == "mesh") {
            continue;
        }
        SCOPED_TRACE(row.name);
        HeavyHashes other = base;
        row.mutate(other);
        EXPECT_TRUE(MeshEqual(base, other));
    }
}
} // namespace
