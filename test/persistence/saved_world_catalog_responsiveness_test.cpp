#include "persistence/AsyncSavedWorldCatalog.h"

#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/SyntheticSaveFixture.h"
#include "luminumbra_common/persistence/WorldSaveService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
namespace fs = std::filesystem;
using Luminumbra::Persistence::AsyncSavedWorldCatalog;
using Luminumbra::Persistence::EnumerateSavedWorlds;
using Luminumbra::Persistence::FixturePayload;
using Luminumbra::Persistence::SavedWorldCatalog;
using Luminumbra::Persistence::SyntheticSaveSpec;
using Luminumbra::Persistence::WorldSaveService;
using Luminumbra::Persistence::WriteSyntheticSave;

std::uint32_t FixtureChunksFromEnv(const char* name, std::uint32_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0')
        return fallback;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 100000)
        return fallback;
    return static_cast<std::uint32_t>(value);
}

double ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

SyntheticSaveSpec Spec(const std::string& world_id, std::uint32_t chunks) {
    SyntheticSaveSpec spec;
    spec.world_id = world_id;
    spec.chunk_count = chunks;
    spec.payload = FixturePayload::Noise;
    spec.preset_source = fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json";
    return spec;
}

std::optional<SavedWorldCatalog> Await(AsyncSavedWorldCatalog& scan) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = scan.Poll())
            return result;
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

class SavedWorldCatalogResponsiveness : public testing::Test {
protected:
    std::vector<fs::path> roots;

    fs::path NewRoot(const std::string& tag) {
        auto root = fs::temp_directory_path() /
                    ("catalog_responsiveness_" + tag + "_" +
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
} // namespace

TEST_F(SavedWorldCatalogResponsiveness, LargeFixtureNeverBlocksTheOwningThread) {
    const auto chunks = FixtureChunksFromEnv("LUMIN_CATALOG_FIXTURE_CHUNKS", 256);
    const auto root = NewRoot("large");
    const auto report = WriteSyntheticSave(Spec("catalog-large", chunks), root);
    ASSERT_TRUE(report.ok) << report.error;

    AsyncSavedWorldCatalog catalog;
    const auto scan_start = std::chrono::steady_clock::now();
    catalog.Request([root](std::stop_token stop) { return EnumerateSavedWorlds(root, stop); });
    const double request_ms = ElapsedMs(scan_start);
    EXPECT_LT(request_ms, 1000.0);

    std::optional<SavedWorldCatalog> result;
    double max_poll_ms = 0.0;
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (!result.has_value() && std::chrono::steady_clock::now() < deadline) {
        const auto poll_start = std::chrono::steady_clock::now();
        result = catalog.Poll();
        max_poll_ms = std::max(max_poll_ms, ElapsedMs(poll_start));
        if (!result.has_value())
            std::this_thread::sleep_for(1ms);
    }
    const double validation_ms = ElapsedMs(scan_start);
    ASSERT_TRUE(result.has_value()) << "catalog scan did not finish within 120 s";
    EXPECT_LT(max_poll_ms, 1000.0);
    EXPECT_FALSE(catalog.Poll().has_value());

    EXPECT_TRUE(result->error.empty()) << result->error;
    ASSERT_EQ(result->worlds.size(), 1u);
    EXPECT_TRUE(result->worlds.front().error.empty()) << result->worlds.front().error;
    EXPECT_EQ(result->worlds.front().metadata.worldId, "catalog-large");
    EXPECT_EQ(result->worlds.front().metadata.name, "Synthetic Fixture");

    RecordProperty("chunks", std::to_string(chunks));
    RecordProperty("max_poll_ms", std::to_string(max_poll_ms));
    RecordProperty("request_ms", std::to_string(request_ms));
    RecordProperty("validation_ms", std::to_string(validation_ms));

    const char* receipt_path = std::getenv("LUMIN_CATALOG_RECEIPT");
    if (receipt_path != nullptr && *receipt_path != '\0') {
        std::ofstream receipt(receipt_path, std::ios::trunc);
        receipt << std::fixed << std::setprecision(3) << "{\"schema\":\"catalog_responsiveness.v1\""
                << ",\"chunks\":" << chunks << ",\"max_poll_ms\":" << max_poll_ms
                << ",\"request_ms\":" << request_ms << ",\"validation_ms\":" << validation_ms
                << "}\n";
        EXPECT_TRUE(receipt.good()) << "could not write " << receipt_path;
    }
}

TEST_F(SavedWorldCatalogResponsiveness, CorruptRegionFileIsRefusedAndStaleRequestIsNeverPublished) {
    const auto corrupt_root = NewRoot("corrupt");
    const auto corrupt_report = WriteSyntheticSave(Spec("catalog-corrupt", 8), corrupt_root);
    ASSERT_TRUE(corrupt_report.ok) << corrupt_report.error;

    // Flip one byte in the middle of a region file of the corrupted world.
    fs::path region_file;
    for (const auto& entry :
         fs::directory_iterator(WorldSaveService::region_directory(corrupt_report.save_dir))) {
        if (entry.is_regular_file() && entry.path().extension() == ".lmr") {
            region_file = entry.path();
            break;
        }
    }
    ASSERT_FALSE(region_file.empty());
    std::string bytes;
    {
        std::ifstream input(region_file, std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        bytes = content.str();
    }
    ASSERT_FALSE(bytes.empty());
    bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x5A);
    {
        std::ofstream output(region_file, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good());
    }

    const auto intact_root = NewRoot("intact");
    const auto intact_report = WriteSyntheticSave(Spec("catalog-intact", 8), intact_root);
    ASSERT_TRUE(intact_report.ok) << intact_report.error;

    const auto corrupt = EnumerateSavedWorlds(corrupt_root);
    ASSERT_EQ(corrupt.worlds.size(), 1u);
    const std::string corrupt_error = corrupt.worlds.front().error;
    EXPECT_FALSE(corrupt_error.empty());

    const auto intact = EnumerateSavedWorlds(intact_root);
    ASSERT_EQ(intact.worlds.size(), 1u);
    EXPECT_TRUE(intact.worlds.front().error.empty()) << intact.worlds.front().error;
    EXPECT_NE(corrupt_error, intact.worlds.front().error);

    // A request that is superseded while running must never be published, even after it
    // finishes. Its callback deliberately ignores cancellation and waits for release.
    std::promise<void> started;
    std::promise<void> release;
    auto released = release.get_future().share();
    AsyncSavedWorldCatalog scan;
    scan.Request([&](const std::stop_token&) {
        started.set_value();
        released.wait_for(5s);
        SavedWorldCatalog stale;
        stale.error = "stale";
        return stale;
    });
    ASSERT_EQ(started.get_future().wait_for(5s), std::future_status::ready);
    scan.Request([](const std::stop_token&) {
        SavedWorldCatalog fresh;
        fresh.error = "fresh";
        return fresh;
    });
    release.set_value();
    const auto result = Await(scan);
    if (result.has_value()) {
        EXPECT_EQ(result->error, "fresh");
    } else {
        FAIL() << "Timed out waiting for the fresh catalog result";
    }
    for (int i = 0; i < 50; ++i) {
        const auto later = scan.Poll();
        EXPECT_FALSE(later.has_value()) << "a superseded or duplicate result was published";
        std::this_thread::sleep_for(1ms);
    }
}
