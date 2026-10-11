#include <cstddef>
#include <gtest/gtest.h>
#include <set>
#include <span>
#include <string>

#include "luminumbra_server/HeavyComparison.h"

namespace {
using Luminumbra::Server::AuthoritativeStateEqual;
using Luminumbra::Server::HeavyHashes;
using Luminumbra::Server::HeavySubsystem;
using Luminumbra::Server::HeavySubsystemTable;
using Luminumbra::Server::MeshEqual;

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

TEST(HeavyComparison, TableCoversEveryHashField) {
    const auto table = HeavySubsystemTable();
    const std::set<std::string> expected{"terrain",
                                         "water",
                                         "entities",
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
    for (const char* name : {"terrain", "water", "entities"}) {
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
