#include <array>
#include <cstddef>
#include <gtest/gtest.h>
#include <set>
#include <span>
#include <string>

#include "luminumbra_server/HeavyComparison.h"

namespace {
using Luminumbra::Server::AuthoritativeStateEqual;
using Luminumbra::Server::CaptureGroundObjects;
using Luminumbra::Server::GroundObjectDifference;
using Luminumbra::Server::HeavyHashes;
using Luminumbra::Server::HeavySubsystem;
using Luminumbra::Server::HeavySubsystemTable;
using Luminumbra::Server::MeshEqual;
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
                                  active_regions,
                                  clock_bytes,
                                  ledger_bytes,
                                  ground_objects] = hashes;
    [[maybe_unused]] const auto& [terrain,
                                  mesh,
                                  water,
                                  entities,
                                  wind,
                                  weather,
                                  aether,
                                  aether_state] = hashes.sub;
    constexpr std::size_t kTopLevelFields = 7;
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
                                         "active_regions",
                                         "clock",
                                         "ledger",
                                         "wind",
                                         "weather",
                                         "aether",
                                         "aether_state",
                                         "mesh",
                                         "scents",
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
    for (const char* name : {"terrain", "water", "entities", "ground_objects"}) {
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
