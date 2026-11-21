#include "../support/GameSessionTestWorld.h"
#include "../support/RegionFixtureCapture.h"
#include "../support/RegionFixtureCapturePublication.h"

#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace P = Luminumbra::Persistence;
using Luminumbra::TestSupport::PublishCapture;

TEST(RegionFixtureCapture, PublicationRefusesExistingDestinationAndCleansStaging) {
    const Luminumbra::TestSupport::ScopedTempFile file("capture_publish_", ".json");
    const auto& destination = file.Path();
    const std::string original = "previous capture\n";
    {
        std::ofstream output(destination, std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << original;
    }
    try {
        PublishCapture(destination, "replacement capture\n");
        FAIL() << "publication must refuse an existing destination";
    } catch (const fs::filesystem_error& error) {
        EXPECT_NE(std::string(error.what()).find("destination must be new"), std::string::npos);
    }
    std::ifstream input(destination, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    EXPECT_EQ(contents.str(), original);
    // The destination is the only remaining entry; no reserved staging directory.
    for (const auto& entry : fs::directory_iterator(destination.parent_path())) {
        EXPECT_EQ(entry.path(), destination);
    }
}

TEST(RegionFixtureCapture, PublicationCreatesNewDestinationAndCleansStaging) {
    const Luminumbra::TestSupport::ScopedTempFile file("capture_publish_", ".json");
    const auto& destination = file.Path();
    const std::string payload = "new capture\n";
    PublishCapture(destination, payload);
    std::ifstream input(destination, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    EXPECT_EQ(contents.str(), payload);
    for (const auto& entry : fs::directory_iterator(destination.parent_path())) {
        EXPECT_EQ(entry.path(), destination);
    }
}

TEST(RegionFixtureCapture, PublicationFailuresLeaveNoStagingOrDestination) {
    const Luminumbra::TestSupport::ScopedTempFile file("capture_publish_", ".json");
    const auto& destination = file.Path();
    // A missing parent refuses staging without leaving a directory behind.
    EXPECT_THROW(PublishCapture(destination / "missing/capture.json", "capture\n"),
                 fs::filesystem_error);
    EXPECT_TRUE(fs::is_empty(destination.parent_path()));
    // An existing directory is also an occupied publication name.
    fs::create_directory(destination);
    EXPECT_THROW(PublishCapture(destination, "capture\n"), fs::filesystem_error);
    EXPECT_TRUE(fs::is_empty(destination));
    for (const auto& entry : fs::directory_iterator(destination.parent_path())) {
        EXPECT_EQ(entry.path(), destination);
    }
}

TEST(RegionFixtureCapture, WorldCaptureMatchesSavedMembersAndIndependentHashes) {
    // Both fixed scenarios: at most two injected chunks, one plant, seven ticks,
    // four members of at most 1 MiB, and one worker; no streamed terrain generation.
    for (const bool plant : {false, true}) {
        SCOPED_TRACE(plant);
        Luminumbra::TestSupport::RegionFixtureWorld root;
        Luminumbra::world::GameSession session, loaded;
        const auto capture =
            Luminumbra::TestSupport::CaptureRegionFixture(root, session, loaded, plant);
        const Luminumbra::TestSupport::ScopedTempFile file("capture_world_", ".json");
        PublishCapture(file.Path(), capture.dump() + '\n');
        std::ifstream input(file.Path(), std::ios::binary);
        ASSERT_TRUE(input.is_open());
        const auto artifact = nlohmann::json::parse(input);
        input.close();
        const auto save = root.path() / "capture-save";
        P::SnapshotIndex index;
        ASSERT_TRUE(P::read_snapshot_index(save, index, nullptr));
        const bool indexed = index.generation != 0;
        const auto& members = artifact.at("members");
        ASSERT_EQ(members.size(), indexed ? 4U : (plant ? 3U : 2U));
        constexpr std::array names{"chunks/region/plant-entities.json",
                                   "chunks/region/r.0.0.lmr",
                                   "chunks/region/snapshot.json",
                                   "chunks/region/world-manifest.json"};
        for (const auto* name : names) {
            SCOPED_TRACE(name);
            if ((!plant && !indexed && std::string_view(name).ends_with("plant-entities.json")) ||
                (!indexed && std::string_view(name).ends_with("snapshot.json"))) {
                EXPECT_FALSE(members.contains(name));
                continue;
            }
            ASSERT_TRUE(members.contains(name));
            const auto& member = members.at(name);
            const auto size = member.at("bytes").get<std::size_t>();
            ASSERT_GT(size, 0U);
            ASSERT_LE(size, 1U << 20);
            const auto& encoded = member.at("hex").get_ref<const std::string&>();
            ASSERT_EQ(encoded.size(), size * 2);
            std::string bytes(size, '\0');
            for (std::size_t offset = 0; offset < size; ++offset) {
                unsigned int value = 0;
                const auto* first = encoded.data() + offset * 2;
                const auto parsed = std::from_chars(first, first + 2, value, 16);
                ASSERT_EQ(parsed.ec, std::errc{});
                ASSERT_EQ(parsed.ptr, first + 2);
                bytes[offset] = static_cast<char>(static_cast<unsigned char>(value));
            }
            EXPECT_EQ(bytes, Luminumbra::TestSupport::ReadClockTestFile(save / name));
            EXPECT_EQ(member.at("fnv64"), P::StableChecksum(bytes));
            if (std::string_view(name).ends_with(".json")) {
                EXPECT_TRUE(nlohmann::json::parse(bytes).is_object());
            }
        }
        // Decode the same region bytes admitted by LoadWorldStateFrom, then
        // compare their content with the reopened world's only durable chunk.
        std::vector<std::shared_ptr<Luminumbra::Chunk>> chunks;
        ASSERT_TRUE(P::WorldSaveService::read_region_chunks(
            P::WorldSaveService::region_file_path(save, 0, 0), chunks));
        ASSERT_EQ(chunks.size(), 1U);
        const auto reopened = loaded.GetWorldSystem()->snapshot_streamed_chunks();
        ASSERT_EQ(reopened.size(), 1U);
        EXPECT_EQ(chunks.front()->get_id(), reopened.front()->get_id());
        EXPECT_EQ(chunks.front()->heightmap_data, reopened.front()->heightmap_data);
        EXPECT_EQ(session.GetWorldSystem()->snapshot_streamed_chunks().size(), plant ? 2U : 1U);
        EXPECT_FALSE(fs::exists(P::WorldSaveService::region_file_path(save, 1, 0)));
        // Use the existing session oracle, independently of the capture helpers.
        const auto& hashes = artifact.at("hashes");
        EXPECT_EQ(hashes.at("live_subhashes"),
                  Luminumbra::TestSupport::SessionWorldSubHashes(session));
        EXPECT_EQ(hashes.at("loaded_subhashes"),
                  Luminumbra::TestSupport::SessionWorldSubHashes(loaded));
        EXPECT_EQ(hashes.at("live_world_hash"), Luminumbra::TestSupport::SessionWorldHash(session));
        EXPECT_EQ(hashes.at("loaded_world_hash"),
                  Luminumbra::TestSupport::SessionWorldHash(loaded));
        if (plant) {
            EXPECT_NE(hashes.at("live_world_hash"), hashes.at("loaded_world_hash"));
        }
        for (const auto& entry : fs::directory_iterator(file.Path().parent_path())) {
            EXPECT_EQ(entry.path(), file.Path());
        }
    }
}

} // namespace
