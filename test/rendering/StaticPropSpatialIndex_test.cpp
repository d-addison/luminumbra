#include "luminumbra_client/rendering/StaticPropSpatialIndex.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
using Planes = std::array<std::array<float, 4>, 6>;

struct Instance {
    std::array<float, 3> position{};
    std::array<std::array<float, 4>, 4> spheres{};
    std::array<float, 4> atlas{};
    std::array<float, 16> matrix{};
    std::array<float, 3> tint{};
    bool authored = false;
    bool tree = false;
    bool leaf = false;
    bool removed = false;
    unsigned available = 15;
};
struct Draw {
    std::size_t id = 0;
    int lod = 0;
    std::array<float, 16> matrix{};
    std::array<float, 3> tint{};
};

bool Visible(const std::array<float, 4>& sphere, const Planes& planes) {
    for (const auto& plane : planes) {
        // Same float operations and strict inequality as the production sphere test.
        const float dot =
            sphere[0] * plane[0] + sphere[1] * plane[1] + sphere[2] * plane[2] + plane[3];
        if (dot < -sphere[3])
            return false;
    }
    return true;
}

std::vector<Draw> DrawList(const std::vector<Instance>& instances,
                           const std::vector<std::size_t>& ids,
                           const std::array<float, 3>& camera,
                           const Planes& planes,
                           bool atlasOn,
                           bool supported) {
    std::vector<Draw> draws;
    for (const auto id : ids) {
        const auto& instance = instances[id];
        const float x = instance.position[0] - camera[0];
        const float y = instance.position[1] - camera[1];
        const float z = instance.position[2] - camera[2];
        int lod =
            SelectTreeLod(std::sqrt(x * x + y * y + z * z),
                          instance.authored ? AuthoredTreeLodConfig(atlasOn) : TreeLodConfig{});
        if (instance.tree && !instance.authored)
            lod = std::min(lod, 2);
        const bool impostor = atlasOn && instance.authored && instance.tree && lod == 3;
        if (impostor && supported) {
            if (instance.leaf && Visible(instance.atlas, planes))
                draws.push_back({id, 3, instance.matrix, instance.tint});
            continue;
        }
        if (impostor)
            lod = 2;
        if ((instance.available & (1u << lod)) == 0) {
            if (instance.authored) {
                while (lod > 0 && (instance.available & (1u << lod)) == 0)
                    --lod;
            } else {
                lod = 0;
            }
        }
        if ((instance.available & (1u << lod)) != 0 && !instance.removed &&
            Visible(instance.spheres[lod], planes))
            draws.push_back({id, lod, instance.matrix, instance.tint});
    }
    return draws;
}

std::vector<StaticPropSpatialIndex::Item> Bounds(const std::vector<Instance>& instances) {
    std::vector<StaticPropSpatialIndex::Item> items;
    for (const auto& instance : instances) {
        StaticPropSpatialIndex::Item item;
        item.position = instance.position;
        for (int requested = 0; requested < kTreeLodCount; ++requested) {
            int resolved = requested;
            if ((instance.available & (1u << resolved)) == 0) {
                if (instance.authored) {
                    while (resolved > 0 && (instance.available & (1u << resolved)) == 0)
                        --resolved;
                } else {
                    resolved = 0;
                }
            }
            if ((instance.available & (1u << resolved)) != 0) {
                const auto& sphere = instance.spheres[resolved];
                item.lodBounds[requested].Include({sphere[0], sphere[1], sphere[2]}, sphere[3]);
            }
        }
        if (instance.authored && instance.tree) {
            item.lodBounds[3].Include(item.lodBounds[2]);
            if (instance.leaf)
                item.lodBounds[3].Include({instance.atlas[0], instance.atlas[1], instance.atlas[2]},
                                          instance.atlas[3]);
        }
        items.push_back(item);
    }
    return items;
}

void ExpectSame(const std::vector<Draw>& expected, const std::vector<Draw>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(expected[index].id, actual[index].id);
        EXPECT_EQ(expected[index].lod, actual[index].lod);
        EXPECT_EQ(0,
                  std::memcmp(expected[index].matrix.data(),
                              actual[index].matrix.data(),
                              sizeof(expected[index].matrix)));
        EXPECT_EQ(0,
                  std::memcmp(expected[index].tint.data(),
                              actual[index].tint.data(),
                              sizeof(expected[index].tint)));
    }
}

Planes BoxPlanes(const std::array<float, 3>& center, float extent) {
    return {{{1, 0, 0, extent - center[0]},
             {-1, 0, 0, extent + center[0]},
             {0, 1, 0, extent - center[1]},
             {0, -1, 0, extent + center[1]},
             {0, 0, 1, extent - center[2]},
             {0, 0, -1, extent + center[2]}}};
}

TEST(StaticPropSpatialIndex, SeededDrawListMatchesFullScanAcrossAtlasAndCoverageStates) {
    std::uint32_t state = 71953;
    auto random = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    };
    for (const std::size_t population : {0u, 1u, 31u, 32u, 33u, 2048u}) {
        std::vector<Instance> instances(population);
        for (auto& instance : instances) {
            for (auto& coordinate : instance.position)
                coordinate = (random() - 0.5f) * 4000.0f;
            instance.authored = random() > 0.5f;
            instance.tree = random() > 0.25f;
            instance.leaf = random() > 0.5f;
            instance.removed = random() > 0.8f;
            instance.available = static_cast<unsigned>(random() * 16.0f);
            for (auto& sphere : instance.spheres) {
                for (std::size_t axis = 0; axis < 3; ++axis)
                    sphere[axis] = instance.position[axis] + (random() - 0.5f) * 80.0f;
                sphere[3] = random() * 60.0f;
            }
            instance.atlas = instance.spheres[0];
            for (auto& element : instance.matrix)
                element = random() * 2.0f - 1.0f;
            for (auto& element : instance.tint)
                element = random();
        }
        if (population > 32) {
            instances[0].position[0] = std::numeric_limits<float>::quiet_NaN();
            instances[1].position[1] = std::numeric_limits<float>::infinity();
        }
        StaticPropSpatialIndex index;
        index.Build(Bounds(instances));
        std::vector<std::size_t> full(population);
        std::iota(full.begin(), full.end(), std::size_t{0});
        for (unsigned pose = 0; pose < 40; ++pose) {
            const std::array<float, 3> camera{(random() - 0.5f) * 4000.0f,
                                              (random() - 0.5f) * 4000.0f,
                                              (random() - 0.5f) * 4000.0f};
            auto planes = BoxPlanes(camera, 40.0f + random() * 900.0f);
            // Rotate the planes around Y to cover oblique camera poses too.
            const float angle = random() * 6.0f;
            for (auto& plane : planes) {
                const float x = plane[0], z = plane[2];
                plane[0] = x * std::cos(angle) - z * std::sin(angle);
                plane[2] = x * std::sin(angle) + z * std::cos(angle);
            }
            for (const bool atlasOn : {false, true}) {
                const auto& candidates = index.Query(camera, planes, atlasOn);
                ASSERT_TRUE(std::is_sorted(candidates.begin(), candidates.end()));
                for (const bool supported : {false, true})
                    ExpectSame(DrawList(instances, full, camera, planes, atlasOn, supported),
                               DrawList(instances, candidates, camera, planes, atlasOn, supported));
            }
        }
    }
}

TEST(StaticPropSpatialIndex, DistanceRejectsInvisibleLodsWithoutCullingSelectedSphere) {
    StaticPropSpatialIndex::Item item;
    item.position = {1000, 0, 0};
    item.lodBounds[0].Include({0, 0, 0}, 1); // near LOD visible, far LOD outside
    item.lodBounds[1] = item.lodBounds[0];
    item.lodBounds[2].Include({1000, 0, 0}, 1);
    item.lodBounds[3] = item.lodBounds[2];
    StaticPropSpatialIndex index;
    index.Build({item});
    EXPECT_TRUE(index.Query({0, 0, 0}, BoxPlanes({0, 0, 0}, 10), true).empty());
    EXPECT_EQ(index.work().instances, 0u);
    EXPECT_EQ(index.Query({1000, 0, 0}, BoxPlanes({0, 0, 0}, 10), true).size(), 1u);
}

TEST(StaticPropSpatialIndex, BoundarySpheresUnknownBoundsAndRebuildsRemainConservative) {
    for (const float distance : {0.0f, 30.0f, 50.0f, 65.0f, 140.0f, 320.0f, 620.0f}) {
        for (const float offset : {-0.00001f, 0.0f, 0.00001f}) {
            StaticPropSpatialIndex::Item item;
            item.position = {distance + offset, 0, 0};
            for (auto& bound : item.lodBounds)
                bound.Include({10.0f, 0, 0}, 1.0f);
            StaticPropSpatialIndex index;
            index.Build({item});
            EXPECT_EQ(index.Query({0, 0, 0}, BoxPlanes({0, 0, 0}, 9), true).size(), 1u);
            // A nonfinite sphere must reach the original comparisons.
            for (auto& bound : item.lodBounds)
                bound.Include({std::numeric_limits<float>::quiet_NaN(), 0, 0}, 1);
            index.Build({item});
            EXPECT_EQ(index.Query({0, 0, 0}, BoxPlanes({100, 100, 100}, 1), false).size(), 1u);
            index.Build({});
            EXPECT_TRUE(index.Query({0, 0, 0}, {}, true).empty());
        }
    }
}

TEST(StaticPropSpatialIndex, NonUnitPlanesAndNonfiniteInputsRetainLegacyCandidates) {
    StaticPropSpatialIndex::Item item;
    item.position = {100, 0, 0};
    for (auto& bound : item.lodBounds)
        bound.Include({100, 0, 0}, 2);
    StaticPropSpatialIndex index;
    index.Build({item});
    Planes planes{};
    planes[0] = {0.001f, 0, 0, -1.0f};
    EXPECT_EQ(index.Query({0, 0, 0}, planes, true).size(), 1u);
    planes[0] = {0, 0, 0, -1.0f};
    EXPECT_EQ(index.Query({0, 0, 0}, planes, true).size(), 1u);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    planes[0] = {nan, 0, 0, -10000.0f};
    EXPECT_EQ(index.Query({0, 0, 0}, planes, true).size(), 1u);
    EXPECT_EQ(index.Query({nan, 0, 0}, planes, true).size(), 1u);
    item.position = {nan, 0, 0};
    index.Build({item});
    EXPECT_EQ(index.Query({0, 0, 0}, planes, true).size(), 1u);
    for (auto& bound : item.lodBounds) {
        bound = {};
        bound.Include({1e30f, -1e30f, 1e30f}, 1e29f);
    }
    index.Build({item});
    EXPECT_EQ(index.Query({0, 0, 0}, {}, false).size(), 1u);
    for (auto& bound : item.lodBounds) {
        bound = {};
        bound.Include({1e38f, 1e38f, 0}, 1);
    }
    index.Build({item});
    planes = {};
    planes[0] = {4, -4, 0, -1e30f};
    EXPECT_EQ(index.Query({0, 0, 0}, planes, false).size(), 1u);
    index.Build({StaticPropSpatialIndex::Item{}});
    EXPECT_TRUE(index.Query({0, 0, 0}, {}, false).empty());
}

TEST(StaticPropSpatialIndex, InstanceVisitsFollowVisibleCellsAsPopulationGrows) {
    for (const std::size_t population : {4096u, 65536u}) {
        std::vector<StaticPropSpatialIndex::Item> items(population);
        for (std::size_t id = 0; id < population; ++id) {
            const float x = static_cast<float>(id) * 100.0f;
            items[id].position = {x, 0, 0};
            for (auto& bound : items[id].lodBounds)
                bound.Include({x, 0, 0}, 1);
        }
        StaticPropSpatialIndex index;
        index.Build(items);
        const auto& candidates = index.Query({0, 0, 0}, BoxPlanes({0, 0, 0}, 10), false);
        ASSERT_FALSE(candidates.empty());
        EXPECT_EQ(candidates.front(), 0u);
        EXPECT_LE(index.work().instances, 32u);
        EXPECT_LT(index.work().cells, 32u);
        EXPECT_EQ(index.work().instances, candidates.size());
        EXPECT_TRUE(index.Query({-1000, 0, 0}, BoxPlanes({-1000, 0, 0}, 10), false).empty());
        EXPECT_EQ(index.work().instances, 0u);
    }
}

TEST(StaticPropStreamSelection, IdenticalListsDoNotRewriteBytesAndChangesRetainOrder) {
    StaticPropStreamSelection selection;
    std::vector<std::size_t> bytes;
    auto update = [&](const std::vector<std::size_t>& ids) {
        std::size_t writes = 0;
        selection.Begin();
        for (const auto id : ids) {
            if (selection.Select(id)) {
                bytes.resize(selection.size() - 1);
                bytes.push_back(id);
                ++writes;
            }
        }
        if (selection.Finish())
            bytes.resize(selection.size());
        EXPECT_EQ(bytes, ids);
        return writes;
    };
    EXPECT_EQ(update({}), 0u);
    EXPECT_EQ(update({1, 3, 5}), 3u);
    EXPECT_EQ(update({1, 3, 5}), 0u);
    EXPECT_EQ(update({1, 5}), 1u);
    EXPECT_EQ(update({1, 5, 7}), 1u);
    EXPECT_EQ(update({1}), 0u);
    EXPECT_EQ(update({3, 1}), 2u);
    EXPECT_EQ(update({}), 0u);
    EXPECT_EQ(update({}), 0u);
    EXPECT_EQ(update({3, 1}), 2u);
    selection.Begin();
    EXPECT_FALSE(selection.Select(3));
    EXPECT_TRUE(selection.Select(9));
    // Simulate interruption between selection and a failed matrix/tint append.
    EXPECT_EQ(update({3, 9}), 2u);
    EXPECT_EQ(update({3, 9}), 0u);
    const auto savedSelection = selection;
    const auto savedBytes = bytes;
    EXPECT_EQ(update({9, 3}), 2u);
    selection = savedSelection;
    bytes = savedBytes;
    EXPECT_EQ(update({3, 9}), 0u);
}
} // namespace
