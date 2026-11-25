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

#include "../fixtures/AvatarContinuationWorld.h"
#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
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

using Luminumbra::Server::HeavyOwnerCapture;
using Luminumbra::Server::HeavyOwnerDifference;
using Luminumbra::Server::HeavyOwnerFields;
using Luminumbra::Server::HeavyOwnerScheduler;

struct OwnerMutation {
    const char* field;
    void (*mutate)(HeavyOwnerCapture&);
};

// Independent mutation list: it does not call the comparison's scalar projections.
// The arity guards below fail compilation when a real encoding gains a member.

const std::array<OwnerMutation, 138>& OwnerMutationTable() {
    static const std::array<OwnerMutation, 138> table{{
        {"owner.version",
         [](HeavyOwnerCapture& h) {
             ++h.owners.version;
         }},
        {"owner.resident_chunks[0].value",
         [](HeavyOwnerCapture& h) {
             ++h.owners.resident_chunks[0];
         }},
        {"owner.resident_chunks.size",
         [](HeavyOwnerCapture& h) {
             h.owners.resident_chunks.clear();
         }},
        {"owner.ambient.present",
         [](HeavyOwnerCapture& h) {
             h.owners.ambient.reset();
         }},
        {"owner.ambient.tick",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             ++h.owners.ambient->tick;
         }},
        {"owner.ambient.anchor.x",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->anchor.x = -0.0f;
         }},
        {"owner.ambient.anchor.y",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->anchor.y = -0.0f;
         }},
        {"owner.ambient.anchor.z",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->anchor.z = -0.0f;
         }},
        {"owner.ambient.constructor_state",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->constructor_state = !h.owners.ambient->constructor_state;
         }},
        {"owner.ambient.storms[0].center_world.x",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->storms[0].center_world.x = -0.0f;
         }},
        {"owner.ambient.storms[0].center_world.y",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->storms[0].center_world.y = -0.0f;
         }},
        {"owner.ambient.storms[0].velocity.x",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->storms[0].velocity.x = -0.0f;
         }},
        {"owner.ambient.storms[0].velocity.y",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->storms[0].velocity.y = -0.0f;
         }},
        {"owner.ambient.storms[0].intensity",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->storms[0].intensity = -0.0f;
         }},
        {"owner.ambient.storms[0].spawn_tick",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             ++h.owners.ambient->storms[0].spawn_tick;
         }},
        {"owner.ambient.storms[0].lifetime_ticks",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             ++h.owners.ambient->storms[0].lifetime_ticks;
         }},
        {"owner.ambient.storms[0].seed_salt",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             ++h.owners.ambient->storms[0].seed_salt;
         }},
        {"owner.ambient.strikes[0].strike_tick",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             ++h.owners.ambient->strikes[0].strike_tick;
         }},
        {"owner.ambient.strikes[0].world_x",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->strikes[0].world_x = -0.0f;
         }},
        {"owner.ambient.strikes[0].world_z",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->strikes[0].world_z = -0.0f;
         }},
        {"owner.ambient.strikes[0].magnitude",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->strikes[0].magnitude = -0.0f;
         }},
        {"owner.ambient.strikes[0].storm_salt",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             ++h.owners.ambient->strikes[0].storm_salt;
         }},
        {"owner.ambient.wind[0].center_world.x",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->wind[0].center_world.x = -0.0f;
         }},
        {"owner.ambient.wind[0].center_world.y",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->wind[0].center_world.y = -0.0f;
         }},
        {"owner.ambient.wind[0].radius_m",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->wind[0].radius_m = -0.0f;
         }},
        {"owner.ambient.wind[0].velocity.x",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->wind[0].velocity.x = -0.0f;
         }},
        {"owner.ambient.wind[0].velocity.y",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->wind[0].velocity.y = -0.0f;
         }},
        {"owner.ambient.wind[0].intensity",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->wind[0].intensity = -0.0f;
         }},
        {"owner.ambient.storms.size",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->storms.clear();
         }},
        {"owner.ambient.strikes.size",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->strikes.clear();
         }},
        {"owner.ambient.wind.size",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.ambient) {
                 ADD_FAILURE() << "Expected populated ambient capture";
                 return;
             }
             h.owners.ambient->wind.clear();
         }},
        {"owner.world_seed",
         [](HeavyOwnerCapture& h) {
             ++h.owners.world_seed;
         }},
        {"owner.preset_identity",
         [](HeavyOwnerCapture& h) {
             h.owners.preset_identity += "x";
         }},
        {"owner.slot_count",
         [](HeavyOwnerCapture& h) {
             ++h.owners.slot_count;
         }},
        {"owner.bodies[0].physics.suspended",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].state.suspended = !h.owners.bodies[0].state.suspended;
         }},
        {"owner.bodies[0].physics.was_active",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].state.was_active = !h.owners.bodies[0].state.was_active;
         }},
        {"owner.bodies[0].physics.retained_velocity.x",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].state.retained_velocity.x = -0.0f;
         }},
        {"owner.bodies[0].physics.retained_velocity.y",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].state.retained_velocity.y = -0.0f;
         }},
        {"owner.bodies[0].physics.retained_velocity.z",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].state.retained_velocity.z = -0.0f;
         }},
        {"owner.bodies[0].physics.retained_angular_velocity.x",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].state.retained_angular_velocity.x = -0.0f;
         }},
        {"owner.bodies[0].physics.retained_angular_velocity.y",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].state.retained_angular_velocity.y = -0.0f;
         }},
        {"owner.bodies[0].physics.retained_angular_velocity.z",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].state.retained_angular_velocity.z = -0.0f;
         }},
        {"owner.bodies[0].physics.registration_slot",
         [](HeavyOwnerCapture& h) {
             ++h.owners.bodies[0].state.registration_slot;
         }},
        {"owner.free_slots.size",
         [](HeavyOwnerCapture& h) {
             h.owners.free_slots.clear();
         }},
        {"owner.free_slots[0].value",
         [](HeavyOwnerCapture& h) {
             ++h.owners.free_slots[0];
         }},
        {"owner.simulation_terrain.bodies.size",
         [](HeavyOwnerCapture& h) {
             h.owners.simulation_terrain.bodies.clear();
         }},
        {"owner.simulation_terrain.bodies[0].value",
         [](HeavyOwnerCapture& h) {
             ++h.owners.simulation_terrain.bodies[0];
         }},
        {"owner.simulation_terrain.columns.size",
         [](HeavyOwnerCapture& h) {
             h.owners.simulation_terrain.columns.clear();
         }},
        {"owner.simulation_terrain.columns[0].id",
         [](HeavyOwnerCapture& h) {
             ++h.owners.simulation_terrain.columns[0].id;
         }},
        {"owner.simulation_terrain.columns[0].heights[0]",
         [](HeavyOwnerCapture& h) {
             h.owners.simulation_terrain.columns[0].heights[0] = -0.0f;
         }},
        {"owner.simulation_tick",
         [](HeavyOwnerCapture& h) {
             ++h.owners.simulation_tick;
         }},
        {"owner.stream_tick",
         [](HeavyOwnerCapture& h) {
             ++h.owners.stream_tick;
         }},
        {"owner.moving_anchor",
         [](HeavyOwnerCapture& h) {
             h.owners.moving_anchor = !h.owners.moving_anchor;
         }},
        {"owner.water_smoke",
         [](HeavyOwnerCapture& h) {
             h.owners.water_smoke = !h.owners.water_smoke;
         }},
        {"owner.origin.x",
         [](HeavyOwnerCapture& h) {
             h.owners.origin.x = -0.0f;
         }},
        {"owner.origin.y",
         [](HeavyOwnerCapture& h) {
             h.owners.origin.y = -0.0f;
         }},
        {"owner.origin.z",
         [](HeavyOwnerCapture& h) {
             h.owners.origin.z = -0.0f;
         }},
        {"owner.anchor.x",
         [](HeavyOwnerCapture& h) {
             h.owners.anchor.x = -0.0f;
         }},
        {"owner.anchor.y",
         [](HeavyOwnerCapture& h) {
             h.owners.anchor.y = -0.0f;
         }},
        {"owner.anchor.z",
         [](HeavyOwnerCapture& h) {
             h.owners.anchor.z = -0.0f;
         }},
        {"owner.avatars[0].avatar.player_id",
         [](HeavyOwnerCapture& h) {
             ++h.owners.avatars[0].avatar.player_id;
         }},
        {"owner.avatars[0].avatar.position.x",
         [](HeavyOwnerCapture& h) {
             h.owners.avatars[0].avatar.position.x = -0.0f;
         }},
        {"owner.avatars[0].avatar.position.y",
         [](HeavyOwnerCapture& h) {
             h.owners.avatars[0].avatar.position.y = -0.0f;
         }},
        {"owner.avatars[0].avatar.position.z",
         [](HeavyOwnerCapture& h) {
             h.owners.avatars[0].avatar.position.z = -0.0f;
         }},
        {"owner.avatars[0].avatar.velocity.x",
         [](HeavyOwnerCapture& h) {
             h.owners.avatars[0].avatar.velocity.x = -0.0f;
         }},
        {"owner.avatars[0].avatar.velocity.y",
         [](HeavyOwnerCapture& h) {
             h.owners.avatars[0].avatar.velocity.y = -0.0f;
         }},
        {"owner.avatars[0].avatar.velocity.z",
         [](HeavyOwnerCapture& h) {
             h.owners.avatars[0].avatar.velocity.z = -0.0f;
         }},
        {"owner.avatars[0].avatar.facing",
         [](HeavyOwnerCapture& h) {
             h.owners.avatars[0].avatar.facing = -0.0f;
         }},
        {"owner.bodies[0].owner_id",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].owner_id += 1;
         }},
        {"owner.bodies[0].anchor.x",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].anchor.x = -0.0f;
         }},
        {"owner.bodies[0].anchor.y",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].anchor.y = -0.0f;
         }},
        {"owner.bodies[0].anchor.z",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies[0].anchor.z = -0.0f;
         }},
        {"owner.player.owner_id",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.player) {
                 ADD_FAILURE() << "Expected populated capture";
                 return;
             }
             (*h.owners.player).owner_id += 1;
         }},
        {"owner.player.anchor.x",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.player) {
                 ADD_FAILURE() << "Expected populated capture";
                 return;
             }
             (*h.owners.player).anchor.x = -0.0f;
         }},
        {"owner.player.anchor.y",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.player) {
                 ADD_FAILURE() << "Expected populated capture";
                 return;
             }
             (*h.owners.player).anchor.y = -0.0f;
         }},
        {"owner.player.anchor.z",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.player) {
                 ADD_FAILURE() << "Expected populated capture";
                 return;
             }
             (*h.owners.player).anchor.z = -0.0f;
         }},
        {"owner.terrain[0].chunk_id",
         [](HeavyOwnerCapture& h) {
             ++h.owners.terrain[0].chunk_id;
         }},
        {"owner.terrain[0].body_id",
         [](HeavyOwnerCapture& h) {
             ++h.owners.terrain[0].body_id;
         }},
        {"scheduler.active_regions",
         [](HeavyOwnerCapture& h) {
             h.scheduler.active_regions = !h.scheduler.active_regions;
         }},
        {"scheduler.ledger_tick",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.ledger_tick;
         }},
        {"scheduler.ledger_header_tick",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.ledger_header_tick;
         }},
        {"scheduler.next_persistent_id",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.next_persistent_id;
         }},
        {"scheduler.water_cursor",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.water_cursor;
         }},
        {"scheduler.clock.tick",
         [](HeavyOwnerCapture& h) {
             h.scheduler.clock.set_tick(1);
         }},
        {"scheduler.clock.calendar.dayLengthTicks",
         [](HeavyOwnerCapture& h) {
             auto calendar = h.scheduler.clock.calendar();
             ++calendar.dayLengthTicks;
             h.scheduler.clock = Luminumbra::world::WorldClock(0, calendar);
         }},
        {"scheduler.clock.calendar.daysPerYear",
         [](HeavyOwnerCapture& h) {
             auto calendar = h.scheduler.clock.calendar();
             ++calendar.daysPerYear;
             h.scheduler.clock = Luminumbra::world::WorldClock(0, calendar);
         }},
        {"scheduler.clock.calendar.daylightNumerator",
         [](HeavyOwnerCapture& h) {
             auto calendar = h.scheduler.clock.calendar();
             --calendar.daylightNumerator;
             h.scheduler.clock = Luminumbra::world::WorldClock(0, calendar);
         }},
        {"scheduler.clock.calendar.daylightDenominator",
         [](HeavyOwnerCapture& h) {
             auto calendar = h.scheduler.clock.calendar();
             calendar.daylightDenominator += 2;
             h.scheduler.clock = Luminumbra::world::WorldClock(0, calendar);
         }},
        {"scheduler.clock.calendar.lunarCycleDays",
         [](HeavyOwnerCapture& h) {
             auto calendar = h.scheduler.clock.calendar();
             ++calendar.lunarCycleDays;
             h.scheduler.clock = Luminumbra::world::WorldClock(0, calendar);
         }},
        {"scheduler.config.work_limit",
         [](HeavyOwnerCapture& h) {
             --h.scheduler.config.work_limit;
         }},
        {"scheduler.config.hold_ticks",
         [](HeavyOwnerCapture& h) {
             --h.scheduler.config.hold_ticks;
         }},
        {"scheduler.config.reduced_shift",
         [](HeavyOwnerCapture& h) {
             --h.scheduler.config.reduced_shift;
         }},
        {"scheduler.local_anchor.x",
         [](HeavyOwnerCapture& h) {
             if (!h.scheduler.local_anchor) {
                 ADD_FAILURE() << "Expected populated capture";
                 return;
             }
             h.scheduler.local_anchor->x = -0.0f;
         }},
        {"scheduler.local_anchor.y",
         [](HeavyOwnerCapture& h) {
             if (!h.scheduler.local_anchor) {
                 ADD_FAILURE() << "Expected populated capture";
                 return;
             }
             h.scheduler.local_anchor->y = -0.0f;
         }},
        {"scheduler.local_anchor.z",
         [](HeavyOwnerCapture& h) {
             if (!h.scheduler.local_anchor) {
                 ADD_FAILURE() << "Expected populated capture";
                 return;
             }
             h.scheduler.local_anchor->z = -0.0f;
         }},
        {"scheduler.records[0,0].visited",
         [](HeavyOwnerCapture& h) {
             h.scheduler.records.begin()->second.visited =
                 !h.scheduler.records.begin()->second.visited;
         }},
        {"scheduler.records[0,0].edited",
         [](HeavyOwnerCapture& h) {
             h.scheduler.records.begin()->second.edited =
                 !h.scheduler.records.begin()->second.edited;
         }},
        {"scheduler.records[0,0].pinned",
         [](HeavyOwnerCapture& h) {
             h.scheduler.records.begin()->second.pinned =
                 !h.scheduler.records.begin()->second.pinned;
         }},
        {"scheduler.records[0,0].populated",
         [](HeavyOwnerCapture& h) {
             h.scheduler.records.begin()->second.populated =
                 !h.scheduler.records.begin()->second.populated;
         }},
        {"scheduler.records[0,0].first_active",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.first_active;
         }},
        {"scheduler.records[0,0].last_proximity",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.last_proximity;
         }},
        {"scheduler.records[0,0].last_edit",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.last_edit;
         }},
        {"scheduler.records[0,0].last_ticked",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.last_ticked;
         }},
        {"scheduler.records[0,0].frozen_at",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.frozen_at;
         }},
        {"scheduler.records[0,0].last_save",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.last_save;
         }},
        {"scheduler.records[0,0].state",
         [](HeavyOwnerCapture& h) {
             h.scheduler.records.begin()->second.state = Luminumbra::world::RegionState::Frozen;
         }},
        {"scheduler.records[0,0].cadence_shift",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.cadence_shift;
         }},
        {"scheduler.records[0,0].cadence_phase",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.cadence_phase;
         }},
        {"scheduler.records[0,0].pressure",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.pressure;
         }},
        {"scheduler.records[0,0].over_hold",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.over_hold;
         }},
        {"scheduler.records[0,0].under_hold",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.under_hold;
         }},
        {"scheduler.records[0,0].last_decision",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.last_decision;
         }},
        {"scheduler.records[0,0].water_cursor",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.water_cursor;
         }},
        {"scheduler.records[0,0].frozen_digest",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.records.begin()->second.frozen_digest;
         }},
        {"scheduler.records[0,0].wake_pending",
         [](HeavyOwnerCapture& h) {
             h.scheduler.records.begin()->second.wake_pending =
                 !h.scheduler.records.begin()->second.wake_pending;
         }},
        {"scheduler.records[0,0].key.x",
         [](HeavyOwnerCapture& h) {
             auto node = h.scheduler.records.extract(h.scheduler.records.begin());
             ++node.key().x;
             h.scheduler.records.insert(std::move(node));
         }},
        {"scheduler.records[0,0].key.z",
         [](HeavyOwnerCapture& h) {
             auto node = h.scheduler.records.extract(h.scheduler.records.begin());
             ++node.key().z;
             h.scheduler.records.insert(std::move(node));
         }},
        {"scheduler.energy_ages[0,0].key.x",
         [](HeavyOwnerCapture& h) {
             auto node = h.scheduler.energy_ages.extract(h.scheduler.energy_ages.begin());
             ++node.key().x;
             h.scheduler.energy_ages.insert(std::move(node));
         }},
        {"scheduler.energy_ages[0,0].key.z",
         [](HeavyOwnerCapture& h) {
             auto node = h.scheduler.energy_ages.extract(h.scheduler.energy_ages.begin());
             ++node.key().z;
             h.scheduler.energy_ages.insert(std::move(node));
         }},
        {"scheduler.energy_ages[0,0].value",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.energy_ages.begin()->second;
         }},
        {"scheduler.water_debts[0,0].key.x",
         [](HeavyOwnerCapture& h) {
             auto node = h.scheduler.water_debts.extract(h.scheduler.water_debts.begin());
             ++node.key().x;
             h.scheduler.water_debts.insert(std::move(node));
         }},
        {"scheduler.water_debts[0,0].key.z",
         [](HeavyOwnerCapture& h) {
             auto node = h.scheduler.water_debts.extract(h.scheduler.water_debts.begin());
             ++node.key().z;
             h.scheduler.water_debts.insert(std::move(node));
         }},
        {"scheduler.water_debts[0,0].value",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.water_debts.begin()->second;
         }},
        {"scheduler.completed_work[0,0].key.x",
         [](HeavyOwnerCapture& h) {
             auto node = h.scheduler.completed_work.extract(h.scheduler.completed_work.begin());
             ++node.key().x;
             h.scheduler.completed_work.insert(std::move(node));
         }},
        {"scheduler.completed_work[0,0].key.z",
         [](HeavyOwnerCapture& h) {
             auto node = h.scheduler.completed_work.extract(h.scheduler.completed_work.begin());
             ++node.key().z;
             h.scheduler.completed_work.insert(std::move(node));
         }},
        {"scheduler.completed_work[0,0].value",
         [](HeavyOwnerCapture& h) {
             ++h.scheduler.completed_work.begin()->second;
         }},
        {"owner.avatars.size",
         [](HeavyOwnerCapture& h) {
             h.owners.avatars.clear();
         }},
        {"owner.bodies.size",
         [](HeavyOwnerCapture& h) {
             h.owners.bodies.clear();
         }},
        {"owner.terrain.size",
         [](HeavyOwnerCapture& h) {
             h.owners.terrain.clear();
         }},
        {"owner.player.present",
         [](HeavyOwnerCapture& h) {
             h.owners.player.reset();
         }},
        {"scheduler.records.size",
         [](HeavyOwnerCapture& h) {
             h.scheduler.records.clear();
         }},
        {"scheduler.energy_ages.size",
         [](HeavyOwnerCapture& h) {
             h.scheduler.energy_ages.clear();
         }},
        {"scheduler.water_debts.size",
         [](HeavyOwnerCapture& h) {
             h.scheduler.water_debts.clear();
         }},
        {"scheduler.completed_work.size",
         [](HeavyOwnerCapture& h) {
             h.scheduler.completed_work.clear();
         }},
        {"scheduler.local_anchor.present",
         [](HeavyOwnerCapture& h) {
             h.scheduler.local_anchor.reset();
         }},
        {"owner.avatars[0].physics.size",
         [](HeavyOwnerCapture& h) {
             --h.owners.avatars[0].physics.size;
         }},
        {"owner.bodies[0].physics.size",
         [](HeavyOwnerCapture& h) {
             --h.owners.bodies[0].state.size;
         }},
        {"owner.player.physics.size",
         [](HeavyOwnerCapture& h) {
             if (!h.owners.player) {
                 ADD_FAILURE() << "Expected populated capture";
                 return;
             }
             --h.owners.player->state.size;
         }},
    }};
    return table;
}

void CheckOwnerEncodingArity(const HeavyOwnerCapture& h) {
    [[maybe_unused]] const auto& [captured, owners, scheduler] = h;
    [[maybe_unused]] const auto& [version,
                                  resident_chunks,
                                  ambient,
                                  world_seed,
                                  preset_identity,
                                  simulation_tick,
                                  stream_tick,
                                  moving_anchor,
                                  water_smoke,
                                  origin,
                                  anchor,
                                  avatars,
                                  bodies,
                                  player,
                                  slot_count,
                                  free_slots,
                                  simulation_terrain,
                                  terrain] = owners;
    if (!ambient) {
        ADD_FAILURE() << "Expected populated ambient capture";
        return;
    }
    [[maybe_unused]] const auto& [ambient_tick,
                                  ambient_anchor,
                                  constructor_state,
                                  storms,
                                  strikes,
                                  wind] = *ambient;
    [[maybe_unused]] const auto& [storm_center,
                                  storm_velocity,
                                  storm_intensity,
                                  spawn_tick,
                                  lifetime_ticks,
                                  seed_salt] = storms.front();
    [[maybe_unused]] const auto& [strike_tick, world_x, world_z, magnitude, storm_salt] =
        strikes.front();
    [[maybe_unused]] const auto& [wind_center, radius_m, wind_velocity, wind_intensity] =
        wind.front();
    [[maybe_unused]] const auto& [avatar, physics] = owners.avatars.front();
    [[maybe_unused]] const auto& [id, position, facing, velocity] = avatar;
    [[maybe_unused]] const auto& [bytes, size] = physics;
    [[maybe_unused]] const auto& [owner_id, body_anchor, state] = owners.bodies.front();
    [[maybe_unused]] const auto& [body_bytes,
                                  body_size,
                                  suspended,
                                  retained_velocity,
                                  retained_angular_velocity,
                                  was_active,
                                  registration_slot] = state;
    if (!player) {
        ADD_FAILURE() << "Expected populated player";
        return;
    }
    [[maybe_unused]] const auto& [player_id, player_anchor, player_state] = *player;
    [[maybe_unused]] const auto& [simulation_bodies, columns] = simulation_terrain;
    [[maybe_unused]] const auto& [column_id, heights] = columns.front();
    [[maybe_unused]] const auto& [chunk_id, body_id] = owners.terrain.front();
    [[maybe_unused]] const auto& [active,
                                  clock,
                                  tick,
                                  header_tick,
                                  next_id,
                                  water_cursor,
                                  config,
                                  local_anchor,
                                  records,
                                  energy,
                                  debts,
                                  completed] = scheduler;
    [[maybe_unused]] const auto& [work_limit, hold_ticks, reduced_shift] = config;
    [[maybe_unused]] const auto& [day, year, numerator, denominator, lunar] = clock.calendar();
    [[maybe_unused]] const auto& [rx, rz] = records.begin()->first;
    [[maybe_unused]] const auto& [visited,
                                  edited,
                                  pinned,
                                  populated,
                                  first_active,
                                  last_proximity,
                                  last_edit,
                                  last_ticked,
                                  frozen_at,
                                  last_save,
                                  region_state,
                                  cadence_shift,
                                  cadence_phase,
                                  pressure,
                                  over_hold,
                                  under_hold,
                                  last_decision,
                                  cursor,
                                  digest,
                                  wake_pending] = records.begin()->second;
    EXPECT_EQ(HeavyOwnerFields(owners).size(), 13u);
    EXPECT_EQ(HeavyOwnerFields(*ambient).size(), 5u);
    EXPECT_EQ(HeavyOwnerFields(storms.front()).size(), 8u);
    EXPECT_EQ(HeavyOwnerFields(strikes.front()).size(), 5u);
    EXPECT_EQ(HeavyOwnerFields(wind.front()).size(), 6u);
    EXPECT_EQ(HeavyOwnerFields(avatar).size(), 8u);
    EXPECT_EQ(HeavyOwnerFields(state).size(), 9u);
    EXPECT_EQ(HeavyOwnerFields(*player).size(), 4u);
    EXPECT_EQ(HeavyOwnerFields(owners.bodies.front()).size(), 4u);
    EXPECT_EQ(HeavyOwnerFields(owners.terrain.front()).size(), 2u);
    EXPECT_EQ(HeavyOwnerFields(records.begin()->second).size(), 20u);
    EXPECT_EQ(HeavyOwnerFields(config).size(), 3u);
    EXPECT_EQ(HeavyOwnerFields(clock.calendar()).size(), 5u);
    EXPECT_EQ(HeavyOwnerFields(scheduler).size(), 5u);
    // 13 descriptor scalars + identity string + 9 body metadata + 7 allocator/terrain
    // + 2 resident + 1 ambient presence + 24 ambient scalars + 3 ambient lengths.
    // 8 avatar + 2*4 body/player + 2 terrain + 5 scheduler +
    // 6 clock/calendar + 3 config + 3 local anchor + 20 record + 8 map key
    // coordinates + 3 sparse values + 9 sizes/presence + 3 physics lengths.
    EXPECT_EQ(OwnerMutationTable().size(),
              13u + 1u + 9u + 7u + 2u + 1u + 24u + 3u + 8u + 8u + 2u + 5u + 6u + 3u + 3u + 20u +
                  8u + 3u + 9u + 3u);
}

void PopulateOwnerCapture(HeavyOwnerCapture& h) {
    h.owners.version = 2;
    h.owners.resident_chunks = {0, 7};
    h.owners.ambient =
        Luminumbra::Systems::AmbientContinuation{.tick = 17,
                                                 .anchor = {1.0f, 2.0f, 3.0f},
                                                 .constructor_state = false,
                                                 .storms = {{.center_world = {4.0f, 5.0f},
                                                             .velocity = {6.0f, 7.0f},
                                                             .intensity = 0.5f,
                                                             .spawn_tick = 11,
                                                             .lifetime_ticks = 19,
                                                             .seed_salt = 23}},
                                                 .strikes = {{.strike_tick = 29,
                                                              .world_x = 8.0f,
                                                              .world_z = 9.0f,
                                                              .magnitude = 0.75f,
                                                              .storm_salt = 31}},
                                                 .wind = {{.center_world = {10.0f, 11.0f},
                                                           .radius_m = 12.0f,
                                                           .velocity = {13.0f, 14.0f},
                                                           .intensity = 0.25f}}};
    Luminumbra::TestSupport::AvatarContinuationWorld world;
    auto& physics = world.physics();
    h.owners.avatars.emplace_back();
    const auto index = physics.create_avatar_character(Luminumbra::Vec3(0.0f));
    ASSERT_TRUE(world.capture_avatar_owner(index, h.owners.avatars.front().physics));
    h.owners.bodies.emplace_back();
    const auto body =
        physics.create_dynamic_sphere(Luminumbra::Vec3(10.0f), Luminumbra::Vec3(0.0f), 0.5f);
    ASSERT_TRUE(physics.capture_body_owner(body, h.owners.bodies.front().state));
    h.owners.player.emplace();
    ASSERT_TRUE(physics.create_player_controller(Luminumbra::Vec3(20.0f)));
    ASSERT_TRUE(world.capture_player_owner(h.owners.player->state));
    h.owners.terrain.emplace_back();
    h.owners.free_slots.push_back(0);
    h.owners.simulation_terrain.bodies.push_back(0);
    h.owners.simulation_terrain.columns.emplace_back();
    h.scheduler.local_anchor.emplace(0.0f);
    const Luminumbra::world::RegionKey key{};
    h.scheduler.records.emplace(key, Luminumbra::world::ActiveRegionRecord{});
    h.scheduler.energy_ages.emplace(key, 0);
    h.scheduler.water_debts.emplace(key, 1);
    h.scheduler.completed_work.emplace(key, 0);
    h.captured = true;
}

TEST(HeavyComparison, OwnerFieldsMutateIndependentlyAndRespectProfileGate) {
    HeavyOwnerCapture base;
    PopulateOwnerCapture(base);
    ASSERT_TRUE(base.captured);
    CheckOwnerEncodingArity(base);
    EXPECT_TRUE(HeavyOwnerDifference(true, base, base).empty());
    std::set<std::string> names;
    for (const auto& row : OwnerMutationTable()) {
        SCOPED_TRACE(row.field);
        EXPECT_TRUE(names.insert(row.field).second);
        auto changed = base;
        row.mutate(changed);
        EXPECT_EQ(HeavyOwnerDifference(true, base, changed), row.field);
        EXPECT_FALSE(HeavyOwnerDifference(true, changed, base).empty());
        EXPECT_TRUE(HeavyOwnerDifference(false, base, changed).empty());
        EXPECT_TRUE(HeavyOwnerDifference(false, changed, base).empty());
    }
    // The exact-bit projection must distinguish a subnormal from zero too.
    auto changed = base;
    changed.owners.avatars.front().avatar.facing = std::bit_cast<float>(1u);
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "owner.avatars[0].avatar.facing");
    changed.owners.stream_tick = 1;
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "owner.stream_tick");
}

TEST(HeavyComparison, EveryEncodedPhysicsByteIsComparedForEveryOwnerKind) {
    HeavyOwnerCapture base;
    PopulateOwnerCapture(base);
    ASSERT_TRUE(base.captured);
    struct Payload {
        const char* path;
        std::span<std::uint8_t> bytes;
        std::uint32_t& size;
    };
    std::size_t mutated_bytes = 0;
    std::size_t encoded_bytes = 0;
    for (std::size_t kind = 0; kind < 3; ++kind) {
        auto changed = base;
        if (!changed.owners.player) {
            ADD_FAILURE() << "Expected populated capture";
            return;
        }
        const std::array<Payload, 3> payloads{{
            {"owner.avatars[0].physics",
             changed.owners.avatars[0].physics.bytes,
             changed.owners.avatars[0].physics.size},
            {"owner.bodies[0].physics",
             changed.owners.bodies[0].state.bytes,
             changed.owners.bodies[0].state.size},
            {"owner.player.physics",
             changed.owners.player->state.bytes,
             changed.owners.player->state.size},
        }};
        const auto& [path, bytes, size] = payloads[kind];
        ASSERT_GT(size, 0u);
        ASSERT_LT(size, bytes.size());
        encoded_bytes += size;
        for (std::size_t i = 0; i < size; ++i) {
            SCOPED_TRACE(path);
            SCOPED_TRACE(i);
            bytes[i] ^= 1u;
            const auto field = std::string(path) + ".bytes[" + std::to_string(i) + "]";
            EXPECT_EQ(HeavyOwnerDifference(true, base, changed), field);
            EXPECT_EQ(HeavyOwnerDifference(true, changed, base), field);
            EXPECT_TRUE(HeavyOwnerDifference(false, base, changed).empty());
            bytes[i] ^= 1u;
            ++mutated_bytes;
        }
        // Unused payload storage is outside the encoding; the length itself is covered above.
        bytes[size] ^= 1u;
        EXPECT_TRUE(HeavyOwnerDifference(true, base, changed).empty());
        size = static_cast<std::uint32_t>(bytes.size() + 1);
        EXPECT_EQ(HeavyOwnerDifference(true, base, changed), std::string(path) + ".invalid_size");
        EXPECT_EQ(HeavyOwnerDifference(true, changed, base), std::string(path) + ".invalid_size");
    }
    EXPECT_EQ(mutated_bytes,
              encoded_bytes); // Derived from the real owner encoding, including growth.
    std::array<std::uint8_t, Luminumbra::World::kMaxAvatarOwnerBytes> encoded{};
    std::size_t written = 0;
    ASSERT_TRUE(
        Luminumbra::World::EncodeAvatarOwner(base.owners.avatars.front(), encoded, written));
    // Eight scalar fields, plus the version and nested length, then all physics bytes.
    EXPECT_EQ(written,
              (HeavyOwnerFields(base.owners.avatars.front().avatar).size() + 2) * 4 +
                  base.owners.avatars.front().physics.size);
}

TEST(HeavyComparison, OwnerEmptyCapturesAndRefusals) {
    HeavyOwnerCapture empty;
    EXPECT_TRUE(HeavyOwnerDifference(false, empty, empty).empty());
    EXPECT_EQ(HeavyOwnerDifference(true, empty, empty), "owner.capture_refused");
    auto accepted = empty;
    accepted.captured = true;
    EXPECT_TRUE(HeavyOwnerDifference(true, accepted, accepted).empty());
    EXPECT_EQ(HeavyOwnerDifference(true, empty, accepted), "owner.capture_refused");
    EXPECT_EQ(HeavyOwnerDifference(true, accepted, empty), "owner.capture_refused");
    EXPECT_TRUE(HeavyOwnerDifference(false, empty, accepted).empty());
    auto full = accepted;
    full.owners.player.emplace();
    EXPECT_EQ(HeavyOwnerDifference(true, accepted, full), "owner.player.present");
    EXPECT_EQ(HeavyOwnerDifference(true, full, accepted), "owner.player.present");
    full = accepted;
    full.scheduler.local_anchor.emplace(0.0f);
    EXPECT_EQ(HeavyOwnerDifference(true, accepted, full), "scheduler.local_anchor.present");
    EXPECT_EQ(HeavyOwnerDifference(true, full, accepted), "scheduler.local_anchor.present");
}

TEST(HeavyComparison, OwnerAmbientPresenceIncludesEmptyRecords) {
    HeavyOwnerCapture absent;
    absent.captured = true;
    auto present = absent;
    present.owners.ambient.emplace();
    EXPECT_EQ(HeavyOwnerDifference(true, absent, present), "owner.ambient.present");
    EXPECT_EQ(HeavyOwnerDifference(true, present, absent), "owner.ambient.present");
    EXPECT_TRUE(HeavyOwnerDifference(true, present, present).empty());
    EXPECT_TRUE(HeavyOwnerDifference(false, absent, present).empty());
    EXPECT_TRUE(HeavyOwnerDifference(false, present, absent).empty());
}

TEST(HeavyComparison, OwnerExtensionSequencesCompareLastEntryAndStoredOrderAtCapacity) {
    HeavyOwnerCapture base;
    base.captured = true;
    base.owners.resident_chunks.resize(Luminumbra::Persistence::kOwnerContinuationResidentChunks);
    for (std::size_t i = 0; i < base.owners.resident_chunks.size(); ++i) {
        base.owners.resident_chunks[i] = static_cast<Luminumbra::ChunkID>(i);
    }
    auto& state = base.owners.ambient.emplace();
    state.storms.resize(Luminumbra::Systems::kMaxStormCells);
    state.storms.back().seed_salt = 7;
    state.strikes.resize(Luminumbra::Systems::kMaxLiveStrikes);
    state.strikes.back().storm_salt = 11;
    state.wind.resize(Luminumbra::Systems::kMaxStormCells);
    state.wind.back().intensity = 0.5f;
    auto changed = base;
    ASSERT_TRUE(changed.owners.ambient);
    const auto check_sequence = [&base,
                                 &changed](auto& records, const char* prefix, const char* field) {
        SCOPED_TRACE(prefix);
        const auto last = records.back();
        records.back() = records.front();
        const auto last_path =
            std::string(prefix) + "[" + std::to_string(records.size() - 1) + "]." + field;
        EXPECT_EQ(HeavyOwnerDifference(true, base, changed), last_path);
        EXPECT_EQ(HeavyOwnerDifference(true, changed, base), last_path);
        records.back() = last;
        std::swap(records.front(), records.back());
        const auto first_path = std::string(prefix) + "[0]." + field;
        EXPECT_EQ(HeavyOwnerDifference(true, base, changed), first_path);
        EXPECT_EQ(HeavyOwnerDifference(true, changed, base), first_path);
        EXPECT_TRUE(HeavyOwnerDifference(false, base, changed).empty());
        std::swap(records.front(), records.back());
        EXPECT_TRUE(HeavyOwnerDifference(true, base, changed).empty());
    };
    check_sequence(changed.owners.resident_chunks, "owner.resident_chunks", "value");
    if (!changed.owners.ambient) {
        FAIL() << "Expected populated ambient capture";
    }
    auto& ambient = *changed.owners.ambient;
    check_sequence(ambient.storms, "owner.ambient.storms", "seed_salt");
    check_sequence(ambient.strikes, "owner.ambient.strikes", "storm_salt");
    check_sequence(ambient.wind, "owner.ambient.wind", "intensity");
}

TEST(HeavyComparison, OwnerAmbientFloatProjectionsPreserveExactBits) {
    HeavyOwnerCapture base;
    base.captured = true;
    auto& state = base.owners.ambient.emplace();
    state.storms.emplace_back();
    state.strikes.emplace_back();
    state.wind.emplace_back();
    auto changed = base;
    if (!changed.owners.ambient) {
        FAIL() << "Expected populated ambient capture";
    }
    auto& ambient = *changed.owners.ambient;
    const std::array<std::pair<const char*, float*>, 17> fields{{
        {"owner.ambient.anchor.x", &ambient.anchor.x},
        {"owner.ambient.anchor.y", &ambient.anchor.y},
        {"owner.ambient.anchor.z", &ambient.anchor.z},
        {"owner.ambient.storms[0].center_world.x", &ambient.storms[0].center_world.x},
        {"owner.ambient.storms[0].center_world.y", &ambient.storms[0].center_world.y},
        {"owner.ambient.storms[0].velocity.x", &ambient.storms[0].velocity.x},
        {"owner.ambient.storms[0].velocity.y", &ambient.storms[0].velocity.y},
        {"owner.ambient.storms[0].intensity", &ambient.storms[0].intensity},
        {"owner.ambient.strikes[0].world_x", &ambient.strikes[0].world_x},
        {"owner.ambient.strikes[0].world_z", &ambient.strikes[0].world_z},
        {"owner.ambient.strikes[0].magnitude", &ambient.strikes[0].magnitude},
        {"owner.ambient.wind[0].center_world.x", &ambient.wind[0].center_world.x},
        {"owner.ambient.wind[0].center_world.y", &ambient.wind[0].center_world.y},
        {"owner.ambient.wind[0].radius_m", &ambient.wind[0].radius_m},
        {"owner.ambient.wind[0].velocity.x", &ambient.wind[0].velocity.x},
        {"owner.ambient.wind[0].velocity.y", &ambient.wind[0].velocity.y},
        {"owner.ambient.wind[0].intensity", &ambient.wind[0].intensity},
    }};
    for (const auto& [path, scalar] : fields) {
        SCOPED_TRACE(path);
        for (const auto bits : {0x80000000u, 1u, 0x7fc00001u}) {
            *scalar = std::bit_cast<float>(bits);
            EXPECT_EQ(HeavyOwnerDifference(true, base, changed), path);
            EXPECT_EQ(HeavyOwnerDifference(true, changed, base), path);
            EXPECT_TRUE(HeavyOwnerDifference(false, base, changed).empty());
            EXPECT_TRUE(HeavyOwnerDifference(true, changed, changed).empty());
        }
        *scalar = 0.0f;
    }
}

TEST(HeavyComparison, OwnerSchedulerCaptureIncludesEmptyLedgerRegisters) {
    using namespace Luminumbra::world;
    const WorldClock clock(9);
    const ActiveRegionLedger empty({.work_limit = 10, .hold_ticks = 2, .reduced_shift = 3}, clock);
    auto capture = Luminumbra::Server::CaptureHeavyOwnerScheduler(true, clock, empty, 17);
    EXPECT_TRUE(capture.active_regions);
    EXPECT_EQ(capture.clock.tick(), 9u);
    EXPECT_EQ(capture.ledger_tick, 9u);
    EXPECT_EQ(capture.ledger_header_tick, 9u);
    EXPECT_EQ(capture.next_persistent_id, 1u);
    EXPECT_EQ(capture.water_cursor, 17u);
    EXPECT_EQ(capture.config, empty.config());
    EXPECT_FALSE(capture.local_anchor.has_value());
    EXPECT_TRUE(capture.records.empty());
    EXPECT_TRUE(capture.energy_ages.empty());
    EXPECT_TRUE(capture.water_debts.empty());
    EXPECT_TRUE(capture.completed_work.empty());
    ActiveRegionLedger ledger(empty);
    ledger.set_local_anchor(Luminumbra::Vec3(0.0f));
    const RegionKey key{};
    ledger.activate_owned(key, clock);
    std::uint64_t id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(id));
    ASSERT_TRUE(ledger.set_energy_age(key, 3));
    ASSERT_TRUE(ledger.set_water_debts({{key, 4}}));
    const std::array<RegionWork, 1> work{{{.key = key,
                                           .region_ticks = 1,
                                           .entity_updates = 2,
                                           .water_cells = 3,
                                           .page_updates = 4}}};
    ledger.publish_completed_work(work);
    capture = Luminumbra::Server::CaptureHeavyOwnerScheduler(false, clock, ledger, 18);
    EXPECT_FALSE(capture.active_regions);
    const auto& ledger_anchor = ledger.local_anchor();
    if (!capture.local_anchor || !ledger_anchor) {
        ADD_FAILURE() << "Expected captured scheduler anchor";
        return;
    }
    EXPECT_EQ(*capture.local_anchor, *ledger_anchor);
    EXPECT_EQ(capture.records, ledger.records());
    EXPECT_EQ(capture.next_persistent_id, ledger.next_persistent_id());
    EXPECT_EQ(capture.energy_ages, ledger.energy_ages());
    EXPECT_EQ(capture.water_debts, ledger.water_debts());
    EXPECT_EQ(capture.completed_work, ledger.completed_work());
}

TEST(HeavyComparison, OwnerLaterEntriesAndMaximumPayloadAreComparedInOrder) {
    HeavyOwnerCapture base;
    PopulateOwnerCapture(base);
    ASSERT_TRUE(base.captured);
    base.owners.avatars.push_back(base.owners.avatars.front());
    base.owners.bodies.push_back(base.owners.bodies.front());
    base.owners.terrain.push_back(base.owners.terrain.front());
    auto changed = base;
    changed.owners.avatars.back().avatar.facing = -0.0f;
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "owner.avatars[1].avatar.facing");
    changed = base;
    changed.owners.bodies.back().state.bytes[0] ^= 1u;
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "owner.bodies[1].physics.bytes[0]");
    changed = base;
    ++changed.owners.terrain.back().body_id;
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "owner.terrain[1].body_id");
    const Luminumbra::world::RegionKey key{3, -2};
    base.scheduler.records.emplace(key, Luminumbra::world::ActiveRegionRecord{});
    base.scheduler.energy_ages.emplace(key, 0);
    base.scheduler.water_debts.emplace(key, 1);
    base.scheduler.completed_work.emplace(key, 0);
    changed = base;
    changed.scheduler.records.at(key).wake_pending = true;
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "scheduler.records[3,-2].wake_pending");
    changed = base;
    ++changed.scheduler.energy_ages.at(key);
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "scheduler.energy_ages[3,-2].value");
    changed = base;
    ++changed.scheduler.water_debts.at(key);
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "scheduler.water_debts[3,-2].value");
    changed = base;
    ++changed.scheduler.completed_work.at(key);
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "scheduler.completed_work[3,-2].value");
    changed.scheduler.records.at(key).wake_pending = true;
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "scheduler.records[3,-2].wake_pending");
    auto& state = base.owners.avatars.front().physics;
    state.size = static_cast<std::uint32_t>(state.bytes.size());
    changed = base;
    changed.owners.avatars.front().physics.bytes.back() ^= 1u;
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed),
              "owner.avatars[0].physics.bytes[" + std::to_string(state.size - 1) + "]");
    EXPECT_TRUE(HeavyOwnerDifference(false, base, changed).empty());
}

TEST(HeavyComparison, UnavailableOwnerCapturePreservesDiagnosticOutput) {
    GameSession unavailable({.owner_continuation = true});
    std::string bytes = "unchanged";
    EXPECT_FALSE(unavailable.CaptureOwnerContinuation(bytes));
    EXPECT_EQ(bytes, "unchanged");
}

TEST(HeavyComparison, SimulationTerrainSamplesAndAllocatorOrderAreComparedExactly) {
    HeavyOwnerCapture base;
    base.captured = true;
    base.owners.free_slots = {1, 2};
    base.owners.simulation_terrain.bodies = {3, 4};
    base.owners.simulation_terrain.columns.resize(2);
    auto changed = base;
    std::swap(changed.owners.free_slots[0], changed.owners.free_slots[1]);
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "owner.free_slots[0].value");
    changed = base;
    std::swap(changed.owners.simulation_terrain.bodies[0],
              changed.owners.simulation_terrain.bodies[1]);
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed),
              "owner.simulation_terrain.bodies[0].value");
    changed = base;
    ++changed.owners.simulation_terrain.columns.back().id;
    EXPECT_EQ(HeavyOwnerDifference(true, base, changed), "owner.simulation_terrain.columns[1].id");
    changed = base;
    for (std::size_t column = 0; column < changed.owners.simulation_terrain.columns.size();
         ++column) {
        auto& heights = changed.owners.simulation_terrain.columns[column].heights;
        for (std::size_t sample = 0; sample < heights.size(); ++sample) {
            SCOPED_TRACE(column);
            SCOPED_TRACE(sample);
            heights[sample] = -0.0f;
            const auto field = "owner.simulation_terrain.columns[" + std::to_string(column) +
                               "].heights[" + std::to_string(sample) + "]";
            EXPECT_EQ(HeavyOwnerDifference(true, base, changed), field);
            EXPECT_EQ(HeavyOwnerDifference(true, changed, base), field);
            EXPECT_TRUE(HeavyOwnerDifference(false, base, changed).empty());
            heights[sample] = 0.0f;
        }
    }
}
} // namespace

namespace {
TEST(HeavyComparison, ScentOnlyDivergenceComparesWhenActiveRegionsEnabled) {
    const HeavySubsystem* row = FindRow(HeavySubsystemTable(), "scents");
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(row->compared);
    EXPECT_TRUE(row->requires_active_regions);
    EXPECT_EQ(row->exempt_reason, nullptr);
    ASSERT_NE(row->mutate, nullptr);

    for (bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        const HeavyHashes base = MakeBase(active_regions);
        HeavyHashes other = base;
        EXPECT_TRUE(AuthoritativeStateEqual(base, other));
        EXPECT_TRUE(ScentDifference(base, other).empty());

        row->mutate(other);
        EXPECT_NE(base.scent_hash, other.scent_hash);
        // Equal ledger bytes and an equal composite hash cannot mask durable scent divergence.
        EXPECT_EQ(base.ledger_bytes, other.ledger_bytes);
        EXPECT_EQ(base.world_hash, other.world_hash);
        if (active_regions) {
            EXPECT_FALSE(AuthoritativeStateEqual(base, other));
            EXPECT_FALSE(AuthoritativeStateEqual(other, base));
            EXPECT_EQ(ScentDifference(base, other), "scents");
            EXPECT_EQ(ScentDifference(other, base), "scents");
        } else {
            EXPECT_TRUE(AuthoritativeStateEqual(base, other));
            EXPECT_TRUE(AuthoritativeStateEqual(other, base));
            EXPECT_TRUE(ScentDifference(base, other).empty());
            EXPECT_TRUE(ScentDifference(other, base).empty());
        }
    }
}
} // namespace
