#include <gtest/gtest.h>

#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/SyntheticSaveFixture.h"
#include "luminumbra_common/persistence/WorldSaveService.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace Luminumbra;
using Persistence::FixturePayload;
using Persistence::SyntheticSaveSpec;
using Persistence::WorldSaveService;
using Persistence::WriteSyntheticSave;

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}
std::map<std::string, std::string> DiskBytes(const fs::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto key = entry.path().lexically_relative(root).generic_string();
        result.emplace(key, entry.is_directory() ? "<directory>" : Read(entry.path()));
    }
    return result;
}
std::map<std::string, std::string> RegionBytes(const fs::path& save_dir) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::directory_iterator(WorldSaveService::region_directory(save_dir))) {
        if (entry.is_regular_file() && entry.path().extension() == ".lmr")
            result.emplace(entry.path().filename().generic_string(), Read(entry.path()));
    }
    return result;
}

class SyntheticSaveFixture : public testing::Test {
protected:
    std::vector<fs::path> roots;

    fs::path NewRoot(const std::string& tag) {
        auto root = fs::temp_directory_path() /
                    ("synthetic_save_fixture_" + tag + "_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        roots.push_back(root);
        return root;
    }
    void TearDown() override {
        for (const auto& root : roots) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
};

SyntheticSaveSpec Spec(const std::string& world_id, std::uint32_t chunks) {
    SyntheticSaveSpec spec;
    spec.world_id = world_id;
    spec.chunk_count = chunks;
    return spec;
}

TEST_F(SyntheticSaveFixture, CoordinateSpiralMatchesDocumentedPrefix) {
    const std::pair<int, int> columns[13] = {{0, 0},
                                             {1, 0},
                                             {1, 1},
                                             {0, 1},
                                             {-1, 1},
                                             {-1, 0},
                                             {-1, -1},
                                             {0, -1},
                                             {1, -1},
                                             {2, -1},
                                             {2, 0},
                                             {2, 1},
                                             {2, 2}};
    const auto coords = Persistence::SyntheticChunkCoords(26);
    ASSERT_EQ(coords.size(), 26u);
    for (int j = 0; j < 13; ++j) {
        for (int half = 0; half < 2; ++half) {
            const auto& c = coords[static_cast<std::size_t>(2 * j + half)];
            EXPECT_EQ(c.x, columns[j].first) << "column " << j;
            EXPECT_EQ(c.y, half) << "column " << j;
            EXPECT_EQ(c.z, columns[j].second) << "column " << j;
        }
    }
}

TEST_F(SyntheticSaveFixture, CoordinatesAreUniqueAtHistoricalCount) {
    const auto coords = Persistence::SyntheticChunkCoords(5433);
    ASSERT_EQ(coords.size(), 5433u);
    std::set<std::tuple<int, int, int>> unique;
    for (const auto& c : coords)
        unique.insert(std::make_tuple(c.x, c.y, c.z));
    EXPECT_EQ(unique.size(), 5433u);
}

TEST_F(SyntheticSaveFixture, WritesCatalogValidWorld) {
    const auto root = NewRoot("catalog");
    SyntheticSaveSpec spec = Spec("catalog_world", 24);
    spec.preset_source = fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json";
    const auto report = WriteSyntheticSave(spec, root);
    ASSERT_TRUE(report.ok) << report.error;
    EXPECT_EQ(report.chunks_written, 24u);
    EXPECT_GT(report.region_files, 0u);

    const auto inspected = Persistence::InspectSavedWorld(root, spec.world_id);
    EXPECT_EQ(inspected.error, "");
    EXPECT_TRUE(WorldSaveService::validate_save(report.save_dir));
    EXPECT_TRUE(fs::exists(WorldSaveService::world_manifest_path(report.save_dir)));

    for (const auto& entry : fs::recursive_directory_iterator(report.save_dir))
        EXPECT_NE(entry.path().filename(), "fixture.json") << entry.path();
    EXPECT_FALSE(fs::exists(root / "worlds/saves" / (spec.world_id + ".fixture.json")));
}

TEST_F(SyntheticSaveFixture, RepeatRunsAreByteIdentical) {
    for (const auto payload : {FixturePayload::Noise, FixturePayload::Flat}) {
        SyntheticSaveSpec spec = Spec("repeat", 24);
        spec.edit_count = 3;
        spec.payload = payload;
        const auto first_root = NewRoot("repeat_a");
        const auto second_root = NewRoot("repeat_b");
        const auto first = WriteSyntheticSave(spec, first_root);
        const auto second = WriteSyntheticSave(spec, second_root);
        ASSERT_TRUE(first.ok) << first.error;
        ASSERT_TRUE(second.ok) << second.error;
        const auto first_bytes = DiskBytes(first.save_dir);
        EXPECT_FALSE(first_bytes.empty());
        EXPECT_EQ(first_bytes, DiskBytes(second.save_dir))
            << "payload " << static_cast<int>(payload);
    }
}

TEST_F(SyntheticSaveFixture, SeedChangesBytes) {
    const auto noise_one_root = NewRoot("seed_noise_1");
    const auto noise_two_root = NewRoot("seed_noise_2");
    SyntheticSaveSpec noise_one = Spec("seeded", 8);
    SyntheticSaveSpec noise_two = Spec("seeded", 8);
    noise_one.seed = 1;
    noise_two.seed = 2;
    const auto a = WriteSyntheticSave(noise_one, noise_one_root);
    const auto b = WriteSyntheticSave(noise_two, noise_two_root);
    ASSERT_TRUE(a.ok) << a.error;
    ASSERT_TRUE(b.ok) << b.error;
    EXPECT_NE(RegionBytes(a.save_dir), RegionBytes(b.save_dir));

    const auto flat_one_root = NewRoot("seed_flat_1");
    const auto flat_two_root = NewRoot("seed_flat_2");
    SyntheticSaveSpec flat_one = Spec("seeded", 8);
    SyntheticSaveSpec flat_two = Spec("seeded", 8);
    flat_one.payload = FixturePayload::Flat;
    flat_two.payload = FixturePayload::Flat;
    flat_one.seed = 1;
    flat_two.seed = 2;
    const auto c = WriteSyntheticSave(flat_one, flat_one_root);
    const auto d = WriteSyntheticSave(flat_two, flat_two_root);
    ASSERT_TRUE(c.ok) << c.error;
    ASSERT_TRUE(d.ok) << d.error;
    EXPECT_EQ(RegionBytes(c.save_dir), RegionBytes(d.save_dir));
}

TEST_F(SyntheticSaveFixture, RefusesBadSpecAndExistingSave) {
    const auto bad_root = NewRoot("bad_spec");
    SyntheticSaveSpec zero_chunks = Spec("ok_id", 0);
    SyntheticSaveSpec too_many_edits = Spec("ok_id", 4);
    too_many_edits.edit_count = 5;
    SyntheticSaveSpec traversal = Spec("../x", 4);
    for (const auto& spec : {zero_chunks, too_many_edits, traversal}) {
        const auto report = WriteSyntheticSave(spec, bad_root);
        EXPECT_FALSE(report.ok);
        EXPECT_FALSE(report.error.empty());
    }

    const auto root = NewRoot("existing");
    const auto first = WriteSyntheticSave(Spec("twice", 8), root);
    ASSERT_TRUE(first.ok) << first.error;
    const auto before = DiskBytes(first.save_dir);
    SyntheticSaveSpec again = Spec("twice", 8);
    again.seed = 99;
    const auto second = WriteSyntheticSave(again, root);
    EXPECT_FALSE(second.ok);
    EXPECT_FALSE(second.error.empty());
    EXPECT_EQ(DiskBytes(first.save_dir), before);
}

} // namespace
