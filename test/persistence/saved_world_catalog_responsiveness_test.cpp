#include "persistence/AsyncSavedWorldCatalog.h"

#include "../support/IndexedSaveFixture.h"
#include "core/FilesystemPath.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/SyntheticSaveFixture.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "world/FarLodStore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
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

std::string ReadBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good());
}

std::map<fs::path, std::string> SavedBytes(const fs::path& directory) {
    std::map<fs::path, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file())
            result.emplace(entry.path().lexically_relative(directory), ReadBytes(entry.path()));
    }
    return result;
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

TEST_F(SavedWorldCatalogResponsiveness, IndexedCatalogNeverDecodesColdChunksOrChangesSaveBytes) {
    namespace P = Luminumbra::Persistence;
    constexpr std::uint32_t chunks = 256;
    const auto root = NewRoot("indexed");
    const auto report = WriteSyntheticSave(Spec("catalog-indexed", chunks), root);
    ASSERT_TRUE(report.ok) << report.error;
    ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(report.save_dir, [] { return true; }));
    const auto bytes = SavedBytes(report.save_dir);

    // Replace the exact-record cache entries at every id with different bytes. This makes
    // the inspected save cold without a cache-reset seam or a timing-dependent eviction.
    auto other_spec = Spec("other-records", chunks);
    ++other_spec.seed;
    const auto other = WriteSyntheticSave(other_spec, NewRoot("other"));
    ASSERT_TRUE(other.ok) << other.error;

    const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
    for (int pass = 0; pass < 3; ++pass) {
        const auto catalog = EnumerateSavedWorlds(root, fs::path(LUMINUMBRA_SOURCE_ROOT));
        ASSERT_TRUE(catalog.error.empty()) << catalog.error;
        ASSERT_EQ(catalog.worlds.size(), 1u);
        EXPECT_TRUE(catalog.worlds.front().error.empty()) << catalog.worlds.front().error;
        EXPECT_EQ(catalog.worlds.front().metadata.worldId, "catalog-indexed");
        const auto inspected = P::InspectSavedWorld(root,
                                                    fs::path(LUMINUMBRA_SOURCE_ROOT),
                                                    "catalog-indexed",
                                                    {},
                                                    P::SavedWorldValidation::Catalog);
        EXPECT_TRUE(inspected.error.empty()) << inspected.error;
        EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes);
        EXPECT_EQ(SavedBytes(report.save_dir), bytes);
    }

    // Prove the cold-cache premise and keep complete inspection a payload-validation oracle.
    const auto complete =
        P::InspectSavedWorld(root, fs::path(LUMINUMBRA_SOURCE_ROOT), "catalog-indexed");
    ASSERT_TRUE(complete.error.empty()) << complete.error;
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + chunks);
    Luminumbra::WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService{}.load_world(loaded, report.save_dir, errors));
    const auto hash = WorldSaveService{}.world_hash(loaded);
    EXPECT_FALSE(WorldSaveService{}.save_dirty_chunks(loaded, report.save_dir, &errors).saved);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(SavedBytes(report.save_dir), bytes);
    Luminumbra::WorldStreamingState reloaded;
    ASSERT_TRUE(WorldSaveService{}.load_world(reloaded, report.save_dir, errors));
    EXPECT_EQ(WorldSaveService{}.world_hash(reloaded), hash);
}

TEST_F(SavedWorldCatalogResponsiveness, CatalogKeepsCompleteLoadAndWriterRefusalReasons) {
    namespace P = Luminumbra::Persistence;
    const auto root = NewRoot("refusal_reasons");
    const auto report = WriteSyntheticSave(Spec("fixture", 1), root);
    ASSERT_TRUE(report.ok) << report.error;
    ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(report.save_dir, [] { return true; }));
    const auto region = WorldSaveService::region_file_path(report.save_dir, 0, 0);
    const auto index_path = P::snapshot_index_path(report.save_dir);
    const auto original_region = ReadBytes(region);
    const auto original_index = ReadBytes(index_path);
    const auto suffix = ": " + Luminumbra::Filesystem::GenericUtf8PathString(region);
    ASSERT_GT(original_region.size(), 28u);

    for (int fault = 0; fault < 6; ++fault) {
        SCOPED_TRACE(fault);
        auto damaged = original_region;
        std::string expected;
        std::string catalog_expected;
        ASSERT_NO_FATAL_FAILURE(WriteBytes(index_path, original_index));
        if (fault == 0) {
            damaged[26] = 0;
            damaged[27] = 0;
            damaged[28] = 0;
            expected = "LZ4 decompression of a region record failed";
            catalog_expected = "inconsistent snapshot: chunks/region/r.0.0.lmr";
        } else if (fault == 1) {
            damaged.resize(4);
            expected = "truncated region file header" + suffix;
        } else if (fault == 2) {
            damaged.pop_back();
            expected = "region record payload is truncated: payload lengths do not match file "
                       "size" +
                       suffix;
        } else if (fault == 3) {
            damaged[4] = 1;
            expected = WorldSaveService::kObsoleteWorldMessage;
        } else if (fault == 4) {
            damaged[4] = 3;
            expected = "unsupported future LMR1 container version 3" + suffix;
        } else {
            P::SnapshotIndex index;
            std::vector<std::string> errors;
            ASSERT_TRUE(P::read_snapshot_index(report.save_dir, index, &errors));
            bool changed = false;
            for (auto& member : index.members) {
                if (member.path == "chunks/region/r.0.0.lmr") {
                    member.fnv64 ^= 1;
                    changed = true;
                }
            }
            ASSERT_TRUE(changed);
            ASSERT_NO_FATAL_FAILURE(WriteBytes(index_path, P::encode_snapshot_index(index)));
            expected = "inconsistent snapshot: chunks/region/r.0.0.lmr";
        }
        ASSERT_NO_FATAL_FAILURE(WriteBytes(region, damaged));
        const auto bytes = SavedBytes(report.save_dir);
        const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
        const auto catalog = EnumerateSavedWorlds(root, fs::path(LUMINUMBRA_SOURCE_ROOT));
        ASSERT_EQ(catalog.worlds.size(), 1u);
        EXPECT_EQ(catalog.worlds.front().error,
                  catalog_expected.empty() ? expected : catalog_expected);
        EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes);

        for (const bool retain : {false, true}) {
            Luminumbra::WorldStreamingState loaded;
            loaded.get_or_create_chunk({9, 0, 9});
            const std::unordered_set<Luminumbra::ChunkID> retain_none;
            std::vector<std::string> errors;
            EXPECT_FALSE(WorldSaveService{}.load_world(
                loaded, report.save_dir, errors, {}, retain ? nullptr : &retain_none));
            ASSERT_FALSE(errors.empty());
            EXPECT_EQ(errors.front(), expected);
            EXPECT_TRUE(loaded.empty());
            std::vector<std::string> validation_errors;
            EXPECT_FALSE(WorldSaveService::validate_save(report.save_dir, &validation_errors));
            EXPECT_EQ(validation_errors, errors);
        }
        EXPECT_EQ(SavedBytes(report.save_dir), bytes);
    }
}

TEST_F(SavedWorldCatalogResponsiveness, IndexedCatalogStillValidatesUnhashedFarLodPayloads) {
    namespace P = Luminumbra::Persistence;
    namespace W = Luminumbra::World;
    const auto root = NewRoot("far_payload");
    const auto report = WriteSyntheticSave(Spec("fixture", 1), root);
    ASSERT_TRUE(report.ok) << report.error;
    ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(report.save_dir, [] { return true; }));
    const auto region = WorldSaveService::region_file_path(report.save_dir, 0, 0);
    const WorldSaveService::ContainerRecord far{
        .id = W::FarLodStore::tile_record_id(W::FarLodTier::F1, 0, 0),
        .lod_level = 1,
        .flags = 0,
        .payload = "invalid far payload"};
    ASSERT_TRUE(WorldSaveService::upsert_container_records(region, {far}));
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::validate_save(report.save_dir, &errors));
    ASSERT_FALSE(errors.empty());
    const auto catalog = EnumerateSavedWorlds(root, fs::path(LUMINUMBRA_SOURCE_ROOT));
    ASSERT_EQ(catalog.worlds.size(), 1u);
    EXPECT_EQ(catalog.worlds.front().error, errors.front());
}

TEST_F(SavedWorldCatalogResponsiveness, CatalogAdmissionHandlesMissingPartialAndCancelledInput) {
    namespace P = Luminumbra::Persistence;
    const auto root = NewRoot("admission");
    std::vector<std::string> errors;
    EXPECT_TRUE(WorldSaveService::inspect_save(root, &errors));
    EXPECT_TRUE(errors.empty());
    const auto report = WriteSyntheticSave(Spec("fixture", 1), root);
    ASSERT_TRUE(report.ok) << report.error;
    for (const bool indexed : {false, true}) {
        if (indexed) {
            ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(report.save_dir, [] { return true; }));
        }
        std::stop_source cancellation;
        cancellation.request_stop();
        errors.clear();
        EXPECT_FALSE(
            WorldSaveService::inspect_save(report.save_dir, &errors, cancellation.get_token()));
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_EQ(errors.front(), "Saved-world validation cancelled.");
        const auto metadata = report.save_dir / "world_info.json";
        const auto original = ReadBytes(metadata);
        for (const auto& partial : {std::string{}, std::string{"{"}}) {
            ASSERT_NO_FATAL_FAILURE(WriteBytes(metadata, partial));
            std::vector<std::string> complete_errors;
            errors.clear();
            EXPECT_FALSE(WorldSaveService::inspect_save(report.save_dir, &errors));
            EXPECT_FALSE(WorldSaveService::validate_save(report.save_dir, &complete_errors));
            EXPECT_EQ(errors, complete_errors);
            EXPECT_FALSE(errors.empty());
        }
        ASSERT_NO_FATAL_FAILURE(WriteBytes(metadata, original));
    }
    const auto index = P::snapshot_index_path(report.save_dir);
    ASSERT_NO_FATAL_FAILURE(WriteBytes(index, "{"));
    errors.clear();
    EXPECT_FALSE(WorldSaveService::inspect_save(report.save_dir, &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors.back(), "inconsistent snapshot: corrupt snapshot index");
    fs::remove(index);
    fs::create_directory(index);
    errors.clear();
    EXPECT_FALSE(WorldSaveService::inspect_save(report.save_dir, &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors.front(), "unreadable or oversized snapshot index");
    EXPECT_EQ(errors.back(), "inconsistent snapshot: corrupt snapshot index");
}

TEST_F(SavedWorldCatalogResponsiveness, LargeFixtureNeverBlocksTheOwningThread) {
    const auto chunks = FixtureChunksFromEnv("LUMIN_CATALOG_FIXTURE_CHUNKS", 256);
    const auto root = NewRoot("large");
    const auto report = WriteSyntheticSave(Spec("catalog-large", chunks), root);
    ASSERT_TRUE(report.ok) << report.error;

    AsyncSavedWorldCatalog catalog;
    const auto scan_start = std::chrono::steady_clock::now();
    catalog.Request([root](const std::stop_token& stop) {
        return EnumerateSavedWorlds(root, fs::path(LUMINUMBRA_SOURCE_ROOT), stop);
    });
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
    if (!result.has_value())
        FAIL() << "catalog scan did not finish within 120 s";
    const SavedWorldCatalog& scanned = *result;
    EXPECT_LT(max_poll_ms, 1000.0);
    EXPECT_FALSE(catalog.Poll().has_value());

    EXPECT_TRUE(scanned.error.empty()) << scanned.error;
    ASSERT_EQ(scanned.worlds.size(), 1u);
    EXPECT_TRUE(scanned.worlds.front().error.empty()) << scanned.worlds.front().error;
    EXPECT_EQ(scanned.worlds.front().metadata.worldId, "catalog-large");
    EXPECT_EQ(scanned.worlds.front().metadata.name, "Synthetic Fixture");

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

    const auto corrupt = EnumerateSavedWorlds(corrupt_root, fs::path(LUMINUMBRA_SOURCE_ROOT));
    ASSERT_EQ(corrupt.worlds.size(), 1u);
    const std::string corrupt_error = corrupt.worlds.front().error;
    EXPECT_FALSE(corrupt_error.empty());

    const auto intact = EnumerateSavedWorlds(intact_root, fs::path(LUMINUMBRA_SOURCE_ROOT));
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
