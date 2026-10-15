#include "../support/FarEditedCacheFixture.h"
#include "core/Crc32.h"
#include "core/FilesystemPath.h"
#include "nlohmann/json.hpp"
#include "gtest/gtest.h"
#include <map>
#include <stdexcept>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Persistence;
using namespace Luminumbra::TestSupport;
namespace fs = std::filesystem;

class FarEditedCacheTest : public testing::Test {
protected:
    void SetUp() override {
        root = CreateUniqueTempDirectory("far_cache_");
        policy = FarCachePolicy();
        policy->after_commit = [this](const FarCacheReport& report) {
            SnapshotIndex index;
            EXPECT_TRUE(
                read_snapshot_index(destination.empty() ? root : destination, index, nullptr));
            EXPECT_EQ(index.generation, report.committed_generation);
            EXPECT_TRUE(report.authoritative_success);
            reports.push_back(report);
        };
    }
    void TearDown() override {
        set_snapshot_step_hook_for_testing(nullptr);
        WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
        std::error_code ec;
        fs::remove_all(root, ec);
        if (!destination.empty())
            fs::remove_all(destination, ec);
    }
    bool Save() {
        FarEditedCacheSaveScope scope(root, policy);
        return WorldSaveService{}.save_dirty_chunks(state, root).saved;
    }
    FarCacheError Acquire(std::uint64_t generation, std::shared_ptr<const FarCommittedCache>& out) {
        return AcquireFarCommittedCache(
            {root, {policy->selection}}, generation, policy->limits, out);
    }
    FarEditedCatalog Catalog() {
        FarEditedCatalog catalog;
        EXPECT_EQ(ReadFarEditedCatalog(root, policy->limits.catalog, catalog),
                  FarCatalogError::None);
        return catalog;
    }
    fs::path root, destination;
    std::shared_ptr<FarEditedCachePolicy> policy;
    WorldStreamingState state;
    std::vector<FarCacheReport> reports;
};

TEST_F(FarEditedCacheTest, FirstIncrementalAndZeroDirtySaveRetainUnchangedReceipts) {
    auto chunk = FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    ASSERT_EQ(reports.size(), 1u);
    ASSERT_TRUE(reports.back().derived_success);
    EXPECT_FALSE(reports.back().derived_due);
    auto first = Catalog();
    ASSERT_EQ(first.tiles.size(), 20u);
    EXPECT_EQ(first.tiles.front().source_generation, 1u);
    const auto bytes = FarCacheRead(root / "far/catalog.json");
    ASSERT_TRUE(Save());
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports.back().rebuilt_tiles, 0u);
    EXPECT_EQ(FarCacheRead(root / "far/catalog.json"), bytes);
    chunk->sdf_data[1 + 17 * (1 + 17)] = -5;
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(Save());
    ASSERT_EQ(reports.size(), 3u);
    EXPECT_GT(reports.back().rebuilt_tiles, 0u);
    std::shared_ptr<const FarCommittedCache> cache;
    ASSERT_EQ(Acquire(3, cache), FarCacheError::None);
    ASSERT_TRUE(cache);
    EXPECT_EQ(cache->report.rebuilt_tiles, 0u);
    EXPECT_FALSE(cache->report.derived_due);
    ASSERT_EQ(cache->source->Lattices().size(), 1u);
    EXPECT_EQ(cache->source->Lattices()[0].density[1 + 17 * (1 + 17)], -5);
}
TEST_F(FarEditedCacheTest, OneFinalizerAcrossNestedWritersAndMetadataOnlySave) {
    auto chunk = FarCacheEditedChunk(state);
    FarEditedCacheSaveScope scope(root, policy);
    SaveTransaction transaction(root, 0, {}, &WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(WorldSaveService{}.save_world(state, root));
    ASSERT_TRUE(WorldSaveService{}.save_dirty_chunks(state, root).saved);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_TRUE(reports.empty());
    ASSERT_TRUE(transaction.commit(nullptr));
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_TRUE(reports.back().derived_success);
    const auto catalog = FarCacheRead(root / "far/catalog.json");
    // A singleton writer alone also registers and runs reconciliation.
    ASSERT_TRUE(WorldSaveService::save_metadata("{\"container_version\":2}\n", root));
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports.back().rebuilt_tiles, 0u);
    EXPECT_EQ(FarCacheRead(root / "far/catalog.json"), catalog);
}
TEST_F(FarEditedCacheTest, NestedWritersReuseFinalizerAtPoolCapacity) {
    auto chunk = FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    reports.clear();
    FarEditedCacheSaveScope scope(root, policy);
    SaveTransaction transaction(root, 0, {}, &WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(RegisterFarEditedCacheFinalizer(transaction, policy));
    const auto pool = policy->limits.source.memory;
    ASSERT_TRUE(pool);
    FarVolumeMemoryLease blocker(*pool);
    ASSERT_TRUE(blocker.TryCharge(pool->limit() - pool->used()));
    const auto attempts = pool->attempts();
    const auto refusals = pool->refusals();
    ASSERT_TRUE(WorldSaveService{}.save_dirty_chunks(state, root).saved);
    ASSERT_TRUE(WorldSaveService::save_metadata("{\"container_version\":2}\n", root));
    EXPECT_EQ(pool->used(), pool->limit());
    EXPECT_EQ(pool->attempts(), attempts);
    EXPECT_EQ(pool->refusals(), refusals);
    EXPECT_TRUE(reports.empty());
    blocker.Reset();
    ASSERT_TRUE(transaction.commit(nullptr));
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_TRUE(reports.back().authoritative_success);
    EXPECT_TRUE(reports.back().derived_success);
    EXPECT_EQ(reports.back().rebuilt_tiles, 0u);
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
}

TEST_F(FarEditedCacheTest, ZeroDirtySaveReusesCaptureAndDecodesEachOverlayOncePerValidation) {
    auto chunk = FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    ASSERT_FALSE(chunk->is_voxel_data_dirty());
    const auto catalog = Catalog();
    ASSERT_FALSE(catalog.tiles.empty());
    const auto bytes = FarCacheRead(root / "far/catalog.json");
    const auto worlds = WorldSaveService::decode_count_for_testing();
    const auto regions = WorldSaveService::edited_source_region_decode_count_for_testing();
    const auto overlays = FarOverlayDecodeCountForTesting();
    ASSERT_TRUE(Save());
    // One ordinary baseline validation, one prepared source capture, and one overlay pass
    // each for baseline validation and finalization. Zero-dirty publication decodes nothing.
    EXPECT_EQ(WorldSaveService::decode_count_for_testing() - worlds, 1u);
    EXPECT_EQ(WorldSaveService::edited_source_region_decode_count_for_testing() - regions, 1u);
    EXPECT_EQ(FarOverlayDecodeCountForTesting() - overlays, 2u * catalog.tiles.size());
    EXPECT_EQ(FarCacheRead(root / "far/catalog.json"), bytes);
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports.back().rebuilt_tiles, 0u);
    const auto acquire_overlays = FarOverlayDecodeCountForTesting();
    std::shared_ptr<const FarCommittedCache> cache;
    ASSERT_EQ(Acquire(2, cache), FarCacheError::None);
    ASSERT_TRUE(cache);
    EXPECT_EQ(FarOverlayDecodeCountForTesting() - acquire_overlays, catalog.tiles.size());
    EXPECT_EQ(cache->tiles.size(), catalog.tiles.size());
    EXPECT_EQ(cache->report.rebuilt_tiles, 0u);
}

TEST_F(FarEditedCacheTest, DirectoryEntryAdmissionPrecedesOverlayDecodes) {
    auto chunk = FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(root, index, nullptr));
    const auto decodes = FarOverlayDecodeCountForTesting();
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::verify_derived_members(
        root, index, &errors, &InspectFarEditedCatalogDigest, 0));
    EXPECT_EQ(FarOverlayDecodeCountForTesting(), decodes);
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("entry limit"), std::string::npos);
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
}

TEST_F(FarEditedCacheTest, ExplicitDestinationAndSaveAsUseDestinationGenerationAndPaths) {
    auto chunk = FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    ASSERT_TRUE(Save());
    const auto source_catalog = FarCacheRead(root / "far/catalog.json");
    destination = CreateUniqueTempDirectory("far_cache_destination_");
    {
        FarEditedCacheSaveScope scope(destination, policy);
        ASSERT_TRUE(WorldSaveService{}.save_world(state, destination));
    }
    FarEditedCatalog copied;
    ASSERT_EQ(ReadFarEditedCatalog(destination, policy->limits.catalog, copied),
              FarCatalogError::None);
    ASSERT_EQ(copied.tiles.size(), 20u);
    EXPECT_EQ(copied.tiles.front().source_generation, 1u);
    EXPECT_EQ(FarCacheRead(root / "far/catalog.json"), source_catalog);
    EXPECT_NE(FarCacheRead(root / "chunks/region/snapshot.json"),
              FarCacheRead(destination / "chunks/region/snapshot.json"));
    std::shared_ptr<const FarCommittedCache> cache;
    ASSERT_EQ(
        AcquireFarCommittedCache({destination, {policy->selection}}, 1, policy->limits, cache),
        FarCacheError::None);
    ASSERT_TRUE(cache);
    EXPECT_EQ(cache->source->Identity().generation, 1u);
    EXPECT_EQ(cache->report.rebuilt_tiles, 0u);
    EXPECT_TRUE(chunk->needs_authority());
}
TEST_F(FarEditedCacheTest, EarlierDiskOnlyEditsSurviveLaterNeighborEdit) {
    auto earlier = FarCacheEditedChunk(state);
    const auto earlier_id = earlier->get_id();
    ASSERT_TRUE(Save());
    state.clear();
    earlier.reset();
    auto neighbor = FarCacheEditedChunk(state, {1, 0, 0});
    neighbor->sdf_data[1 + 17 * (1 + 17)] = -9;
    ASSERT_TRUE(Save());
    std::shared_ptr<const FarCommittedCache> cache;
    ASSERT_EQ(Acquire(2, cache), FarCacheError::None);
    ASSERT_TRUE(cache);
    ASSERT_EQ(cache->source->Inventory().size(), 2u);
    bool found = false;
    for (std::size_t i = 0; i < cache->source->Inventory().size(); ++i) {
        if (cache->source->Inventory()[i].chunk_id == earlier_id) {
            found = true;
            EXPECT_EQ(cache->source->Lattices()[i].density[1 + 17 * (1 + 17)], -3);
        }
    }
    EXPECT_TRUE(found);
    EXPECT_FALSE(cache->tiles.empty());
    // Compare complete canonical output with a fresh full bake of both disk edits.
    auto force = policy->limits;
    force.content_identity = 30;
    const auto capture = force.capture_source;
    force.capture_source = [capture](const auto& image) {
        auto source = capture(image);
        source.content_identity = 30;
        return source;
    };
    std::shared_ptr<const FarCommittedCache> rebuilt;
    ASSERT_EQ(AcquireFarCommittedCache({root, {policy->selection}}, 2, force, rebuilt),
              FarCacheError::None);
    ASSERT_TRUE(rebuilt);
    ASSERT_EQ(rebuilt->tiles.size(), cache->tiles.size());
    for (std::size_t i = 0; i < cache->tiles.size(); ++i) {
        std::string a, b;
        ASSERT_EQ(EncodeFarOverlay(cache->tiles[i], a), FarOverlayError::None);
        ASSERT_EQ(EncodeFarOverlay(rebuilt->tiles[i], b), FarOverlayError::None);
        EXPECT_EQ(a, b);
    }
}
TEST_F(FarEditedCacheTest, LastEditRemovalClearsObsoleteOverridesAndRetainsSpans) {
    FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    const auto before = Catalog();
    state.clear();
    {
        FarEditedCacheSaveScope scope(root, policy);
        SaveTransaction transaction(root, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(RegisterFarEditedCacheFinalizer(transaction, policy));
        ASSERT_TRUE(WorldSaveService::remove_member(root, "chunks/region/r.0.0.lmr"));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_TRUE(reports.back().derived_success);
    EXPECT_FALSE(reports.back().derived_due);
    const auto after = Catalog();
    ASSERT_EQ(after.tiles.size(), before.tiles.size());
    for (std::size_t i = 0; i < after.tiles.size(); ++i) {
        EXPECT_TRUE(after.tiles[i].dependencies.empty());
        EXPECT_LE(after.tiles[i].edited_span.min_y, before.tiles[i].edited_span.min_y);
        EXPECT_GE(after.tiles[i].edited_span.max_y, before.tiles[i].edited_span.max_y);
        const auto decoded = ReadFarOverlay(root, after.tiles[i].key);
        ASSERT_EQ(decoded.error, FarOverlayError::None);
        EXPECT_TRUE(decoded.tile.bricks.empty());
    }
}
TEST_F(FarEditedCacheTest, MissingStaleAndUnreceiptedValidCacheReconstructWithoutWriting) {
    FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    const auto original = Catalog();
    ASSERT_FALSE(original.tiles.empty());
    const auto& receipt = original.tiles.front();
    const auto path = root / receipt.path;
    const auto bytes = FarCacheRead(path);
    for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        ASSERT_TRUE(WorldSaveService::durable_write_file(path, bytes));
        if (mode == 0)
            fs::remove(path);
        if (mode == 1) {
            auto tile = ReadFarOverlay(root, receipt.key);
            ASSERT_EQ(tile.error, FarOverlayError::None);
            tile.tile.generation = 0;
            ASSERT_TRUE(WriteFarOverlay(root, tile.tile));
        }
        if (mode == 2)
            fs::remove(snapshot_index_path(root));
        const auto before = FarCacheRead(path);
        std::shared_ptr<const FarCommittedCache> cache;
        ASSERT_EQ(Acquire(mode == 2 ? 0 : 1, cache), FarCacheError::None);
        ASSERT_TRUE(cache);
        EXPECT_TRUE(cache->report.derived_due);
        EXPECT_GT(cache->report.rebuilt_tiles, 0u);
        EXPECT_EQ(FarCacheRead(path), before);
        if (mode == 2)
            EXPECT_TRUE(cache->source->LegacyAuthority());
    }
}
TEST_F(FarEditedCacheTest, StaleCorruptionAndFutureCatalogOrOverlayRefuseOpen) {
    FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    const auto catalog = Catalog();
    ASSERT_FALSE(catalog.tiles.empty());
    const auto path = root / catalog.tiles.front().path;
    const auto overlay = FarCacheRead(path);
    const auto catalog_bytes = FarCacheRead(root / "far/catalog.json");
    // Even with a stale index fingerprint, structural corruption is an error.
    ASSERT_TRUE(WorldSaveService::durable_write_file(path, "bad"));
    EXPECT_FALSE(WorldSaveService::validate_save(root));
    std::shared_ptr<const FarCommittedCache> cache;
    EXPECT_NE(Acquire(1, cache), FarCacheError::None);
    EXPECT_FALSE(cache);
    ASSERT_TRUE(WorldSaveService::durable_write_file(path, overlay));
    auto future = nlohmann::json::parse(catalog_bytes);
    future["schema"] = "luminumbra.far_catalog.v2";
    ASSERT_TRUE(WorldSaveService::durable_write_file(root / "far/catalog.json", future.dump()));
    EXPECT_FALSE(WorldSaveService::validate_save(root));
    future = nlohmann::json::parse(catalog_bytes);
    future["tiles"][0]["source_generation"] = 9;
    ASSERT_TRUE(WorldSaveService::durable_write_file(root / "far/catalog.json", future.dump()));
    EXPECT_FALSE(WorldSaveService::validate_save(root));
    ASSERT_TRUE(WorldSaveService::durable_write_file(root / "far/catalog.json", catalog_bytes));
    auto decoded = DecodeFarOverlay(overlay, catalog.tiles.front().key);
    ASSERT_EQ(decoded.error, FarOverlayError::None);
    decoded.tile.generation = 9;
    ASSERT_TRUE(WriteFarOverlay(root, decoded.tile));
    EXPECT_FALSE(WorldSaveService::validate_save(root));
    // A future codec version must refuse even without an index.
    fs::remove(snapshot_index_path(root));
    std::string future_overlay = overlay;
    ASSERT_GT(future_overlay.size(), 5u);
    future_overlay[4] = 2;
    ASSERT_TRUE(WorldSaveService::durable_write_file(path, future_overlay));
    EXPECT_FALSE(WorldSaveService::validate_save(root));
}
TEST_F(FarEditedCacheTest, HaloOnlyDependencyChangeMakesTileDue) {
    auto chunk = FarCacheEditedChunk(state, {0, 17, 0}); // y=272, beyond the y=256 capture support.
    ASSERT_TRUE(Save());
    const auto first = Catalog();
    const auto found = std::find_if(first.tiles.begin(), first.tiles.end(), [](const auto& r) {
        return r.key == FarTileKey{5, 0, 0};
    });
    ASSERT_NE(found, first.tiles.end());
    ASSERT_FALSE(found->dependencies.empty());
    EXPECT_EQ(found->dependencies.front().footprint.min.y, 272);
    const auto before = found->overlay_fingerprint;
    chunk->sdf_data[1 + 17 * (1 + 17)] = -12;
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(Save());
    const auto second = Catalog();
    const auto next = std::find_if(second.tiles.begin(), second.tiles.end(), [](const auto& r) {
        return r.key == FarTileKey{5, 0, 0};
    });
    ASSERT_NE(next, second.tiles.end());
    EXPECT_EQ(next->source_generation, 2u);
    EXPECT_NE(next->overlay_fingerprint, before);
}

void OverlayU32(std::string& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<char>((value >> (8 * i)) & 255u);
}
void OverlayCrc(std::string& bytes) {
    Core::Crc32Accumulator crc;
    crc.Update(bytes.data(), bytes.size() - 4);
    OverlayU32(bytes, bytes.size() - 4, crc.Value());
}
TEST_F(FarEditedCacheTest, EveryOverlayCodecRefusalPropagatesThroughOpen) {
    FarOverlayTile tile;
    tile.key = {1, 0, 0};
    FarOverlayBrick a, b;
    b.by = 1;
    tile.bricks = {a, b};
    std::string bytes;
    ASSERT_EQ(EncodeFarOverlay(tile, bytes), FarOverlayError::None);
    ASSERT_TRUE(WorldSaveService{}.save_world(WorldStreamingState{}, root));
    std::vector<std::pair<std::string, FarOverlayError>> failures;
    const auto add = [&](FarOverlayError expected,
                         const std::function<void(std::string&)>& mutate,
                         bool crc = true) {
        auto candidate = bytes;
        mutate(candidate);
        if (crc)
            OverlayCrc(candidate);
        failures.emplace_back(std::move(candidate), expected);
    };
    add(FarOverlayError::BadMagic, [](auto& s) { s[0] = 'X'; });
    add(FarOverlayError::Truncated, [](auto& s) { s.resize(2); }, false);
    add(FarOverlayError::CrcMismatch, [](auto& s) { s.back() ^= 1; }, false);
    add(FarOverlayError::FutureVersion, [](auto& s) { s[4] = 2; });
    add(FarOverlayError::CorruptVersion, [](auto& s) { s[4] = 0; });
    add(FarOverlayError::ReservedBits, [](auto& s) { s[6] = 1; });
    add(FarOverlayError::InvalidTier, [](auto& s) { s[8] = 0; });
    add(FarOverlayError::KeyMismatch, [](auto& s) { s[12] = 1; });
    add(FarOverlayError::BrickOutOfFootprint, [](auto& s) { OverlayU32(s, 44, 32); });
    add(FarOverlayError::BricksNotSorted, [](auto& s) { OverlayU32(s, 44 + 391 + 4, 0); });
    add(FarOverlayError::InvalidBrickState, [](auto& s) { s[44 + 12] = 3; });
    add(FarOverlayError::ForbiddenDensity, [](auto& s) {
        s[60] = 0;
        s[61] = static_cast<char>(128);
    });
    add(FarOverlayError::LimitExceeded, [](auto& s) { OverlayU32(s, 36, kFsv1MaxBricks + 1); });
    add(FarOverlayError::SizeMismatch, [](auto& s) { OverlayU32(s, 36, 3); });
    add(FarOverlayError::TrailingBytes, [](auto& s) { s.push_back(0); }, false);
    const auto path = FarOverlayPath(root, tile.key);
    for (const auto& [candidate, expected] : failures) {
        EXPECT_EQ(DecodeFarOverlay(candidate, tile.key).error, expected);
        ASSERT_TRUE(WorldSaveService::durable_write_file(path, candidate));
        EXPECT_FALSE(WorldSaveService::validate_save(root)) << FarOverlayErrorName(expected);
    }
    fs::remove(path);
    fs::create_directory(path);
    EXPECT_FALSE(WorldSaveService::validate_save(root));
    fs::remove_all(path);
    ASSERT_TRUE(WorldSaveService::durable_write_file(root / "far/v1.33554433.0.lmr", bytes));
    EXPECT_FALSE(WorldSaveService::validate_save(root));
}

struct PublicationFailurePlan {
    std::string step;
    bool armed = false;
};
PublicationFailurePlan& PublicationFailure() {
    static PublicationFailurePlan plan;
    return plan;
}
void FailOverlayPublication(const char* step, const fs::path& path) {
    auto& plan = PublicationFailure();
    if (plan.armed && step == plan.step && path.parent_path().filename() == "far" &&
        path.extension() == ".lmr") {
        plan.armed = false;
        throw std::runtime_error("publication interrupted");
    }
}
TEST_F(FarEditedCacheTest, OptionalFailureBeforeAndAfterRenamePreservesChunksAndZeroDirtyRetry) {
    auto chunk = FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    for (const auto* step : {"before-replace", "after-replace"}) {
        const auto before = FarCacheRead(root / "far/catalog.json");
        chunk->sdf_data[1 + 17 * (1 + 17)] -= 1;
        chunk->mark_voxel_data_dirty();
        PublicationFailure().step = step;
        PublicationFailure().armed = true;
        set_snapshot_step_hook_for_testing(&FailOverlayPublication);
        ASSERT_TRUE(Save());
        set_snapshot_step_hook_for_testing(nullptr);
        EXPECT_FALSE(PublicationFailure().armed);
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        ASSERT_FALSE(reports.empty());
        EXPECT_TRUE(reports.back().authoritative_success);
        EXPECT_TRUE(reports.back().derived_due);
        EXPECT_FALSE(reports.back().derived_success);
        EXPECT_EQ(FarCacheRead(root / "far/catalog.json"), before);
        ASSERT_TRUE(Save());
        EXPECT_TRUE(reports.back().derived_success);
        EXPECT_FALSE(reports.back().derived_due);
        EXPECT_GT(reports.back().rebuilt_tiles, 0u);
    }
}

struct LateValidationFailurePlan {
    bool armed = false, hit = false, corrupted = false;
};
LateValidationFailurePlan& LateValidationFailure() {
    static LateValidationFailurePlan plan;
    return plan;
}
void InvalidatePublishedCatalog(const char* step, const fs::path& path) {
    auto& plan = LateValidationFailure();
    if (plan.armed && std::string_view(step) == "after-replace" &&
        path.parent_path().filename() == "far" && path.filename() == "catalog.json") {
        plan.armed = false;
        plan.hit = true;
        // Return normally: Publish succeeds, then transaction-level validation must restore it.
        plan.corrupted = WorldSaveService::durable_write_file(path, "invalid catalog");
    }
}
TEST_F(FarEditedCacheTest, LateValidationRollbackAbortsLegacyAdoptionAndPreservesReopen) {
    auto chunk = FarCacheEditedChunk(state);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, root));
    FarOverlayTile legacy;
    legacy.key = {1, 0, 0};
    legacy.generation = 99;
    ASSERT_TRUE(WriteFarOverlay(root, legacy));
    const auto legacy_path = FarOverlayPath(root, legacy.key);
    const auto legacy_bytes = FarCacheRead(legacy_path);
    const auto region_path = WorldSaveService::region_file_path(root, 0, 0);
    const auto region_bytes = FarCacheRead(region_path);
    ASSERT_TRUE(WorldSaveService::validate_save(root));
    ASSERT_FALSE(snapshot_index_exists(root));
    chunk->sdf_data[1 + 17 * (1 + 17)] = -8;
    chunk->mark_voxel_data_dirty();
    LateValidationFailure() = {true, false, false};
    set_snapshot_step_hook_for_testing(&InvalidatePublishedCatalog);
    const bool saved = Save();
    set_snapshot_step_hook_for_testing(nullptr);
    ASSERT_TRUE(LateValidationFailure().hit);
    ASSERT_TRUE(LateValidationFailure().corrupted);
    EXPECT_FALSE(saved);
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_FALSE(snapshot_index_exists(root));
    EXPECT_FALSE(journal_pending(root));
    EXPECT_FALSE(fs::exists(root / "far/catalog.json"));
    EXPECT_EQ(FarCacheRead(legacy_path), legacy_bytes);
    EXPECT_EQ(FarCacheRead(region_path), region_bytes);
    const auto restored = ReadFarOverlay(root, legacy.key);
    ASSERT_EQ(restored.error, FarOverlayError::None);
    EXPECT_EQ(restored.tile.generation, 99u);
    // Reopening retains generation-zero legacy authority, with no conflicting generation-one index.
    EXPECT_TRUE(WorldSaveService::validate_save(root));
    std::shared_ptr<const FarCommittedCache> reopened;
    ASSERT_EQ(Acquire(0, reopened), FarCacheError::None);
    ASSERT_TRUE(reopened);
    EXPECT_TRUE(reopened->source->LegacyAuthority());
    EXPECT_EQ(reopened->source->Identity().generation, 0u);
    ASSERT_EQ(reopened->source->Lattices().size(), 1u);
    EXPECT_EQ(reopened->source->Lattices()[0].density[1 + 17 * (1 + 17)], -3);
    ASSERT_TRUE(Save());
    EXPECT_TRUE(WorldSaveService::validate_save(root));
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.back().committed_generation, 1u);
    const auto adopted = ReadFarOverlay(root, legacy.key);
    ASSERT_EQ(adopted.error, FarOverlayError::None);
    EXPECT_EQ(adopted.tile.generation, 1u);
}
TEST_F(FarEditedCacheTest, LateValidationRollbackReportsDerivedDueAfterIndexedCommit) {
    auto chunk = FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    reports.clear();
    const auto catalog = Catalog();
    ASSERT_FALSE(catalog.tiles.empty());
    const auto catalog_bytes = FarCacheRead(root / "far/catalog.json");
    std::map<std::string, std::string> overlay_bytes;
    for (const auto& tile : catalog.tiles)
        overlay_bytes.emplace(tile.path, FarCacheRead(root / tile.path));
    SnapshotIndex before;
    ASSERT_TRUE(read_snapshot_index(root, before, nullptr));
    chunk->sdf_data[1 + 17 * (1 + 17)] = -8;
    chunk->mark_voxel_data_dirty();
    LateValidationFailure() = {true, false, false};
    set_snapshot_step_hook_for_testing(&InvalidatePublishedCatalog);
    const bool saved = Save();
    set_snapshot_step_hook_for_testing(nullptr);
    ASSERT_TRUE(LateValidationFailure().hit);
    ASSERT_TRUE(LateValidationFailure().corrupted);
    ASSERT_TRUE(saved);
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_TRUE(reports.back().authoritative_success);
    EXPECT_FALSE(reports.back().derived_success);
    EXPECT_TRUE(reports.back().derived_due);
    EXPECT_EQ(reports.back().refusal, FarCacheError::PublicationFailure);
    EXPECT_EQ(reports.back().rebuilt_tiles, 0u);
    EXPECT_EQ(reports.back().committed_generation, 2u);
    EXPECT_EQ(FarCacheRead(root / "far/catalog.json"), catalog_bytes);
    for (const auto& [path, bytes] : overlay_bytes)
        EXPECT_EQ(FarCacheRead(root / path), bytes);
    SnapshotIndex after;
    ASSERT_TRUE(read_snapshot_index(root, after, nullptr));
    EXPECT_EQ(after.generation, 2u);
    for (const auto& member : before.members) {
        if (member.cls != MemberClass::Derived)
            continue;
        const auto found = std::find_if(after.members.begin(),
                                        after.members.end(),
                                        [&](const auto& m) { return m.path == member.path; });
        ASSERT_NE(found, after.members.end());
        EXPECT_EQ(found->cls, MemberClass::Derived);
        EXPECT_EQ(found->generation, member.generation);
        EXPECT_EQ(found->present, member.present);
        EXPECT_EQ(found->bytes, member.bytes);
        EXPECT_EQ(found->fnv64, member.fnv64);
    }
    EXPECT_FALSE(journal_pending(root));
    EXPECT_TRUE(WorldSaveService::validate_save(root));
    std::shared_ptr<const FarCommittedCache> reopened;
    ASSERT_EQ(Acquire(2, reopened), FarCacheError::None);
    ASSERT_TRUE(reopened);
    EXPECT_TRUE(reopened->report.derived_due);
    ASSERT_EQ(reopened->source->Lattices().size(), 1u);
    EXPECT_EQ(reopened->source->Lattices()[0].density[1 + 17 * (1 + 17)], -8);
    ASSERT_TRUE(Save());
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_TRUE(reports.back().derived_success);
    EXPECT_FALSE(reports.back().derived_due);
    EXPECT_EQ(reports.back().refusal, FarCacheError::None);
    EXPECT_EQ(reports.back().committed_generation, 3u);
    EXPECT_GT(reports.back().rebuilt_tiles, 0u);
    EXPECT_TRUE(WorldSaveService::validate_save(root));
}

using CacheTree = std::map<std::string, std::string>;
CacheTree ReadCacheTree(const fs::path& root) {
    CacheTree tree;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file())
            tree.emplace(entry.path().lexically_relative(root).generic_string(),
                         FarCacheRead(entry.path()));
    return tree;
}
struct CacheCrashPlan {
    fs::path live, copied, member;
    std::string step;
    bool hit = false, interrupt_rollback = false;
};
CacheCrashPlan*& ActiveCacheCrashPlan() {
    static CacheCrashPlan* plan = nullptr;
    return plan;
}
void CaptureCacheCrash(const char* step, const fs::path& path) {
    auto* plan = ActiveCacheCrashPlan();
    if (!plan || plan->hit || step != plan->step || path != plan->member)
        return;
    plan->hit = true;
    if (plan->interrupt_rollback)
        throw std::runtime_error("rollback interrupted");
    fs::remove_all(plan->copied);
    fs::copy(plan->live, plan->copied, fs::copy_options::recursive);
}
class CacheCrashScope {
public:
    explicit CacheCrashScope(CacheCrashPlan& plan) {
        ActiveCacheCrashPlan() = &plan;
        set_snapshot_step_hook_for_testing(&CaptureCacheCrash);
    }
    ~CacheCrashScope() {
        set_snapshot_step_hook_for_testing(nullptr);
        ActiveCacheCrashPlan() = nullptr;
    }
    CacheCrashScope(const CacheCrashScope&) = delete;
    CacheCrashScope& operator=(const CacheCrashScope&) = delete;
};
TEST_F(FarEditedCacheTest, OverlayCatalogAndIndexCrashImagesRecoverAndRollbackIsIdempotent) {
    auto chunk = FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    const auto catalog = Catalog();
    ASSERT_FALSE(catalog.tiles.empty());
    const auto baseline = ReadCacheTree(root);
    const auto backup =
        root.parent_path() / (Filesystem::GenericPathString(root.filename()) + "_baseline");
    const auto interrupted =
        root.parent_path() / (Filesystem::GenericPathString(root.filename()) + "_interrupted");
    fs::copy(root, backup, fs::copy_options::recursive);
    for (const auto& member : {catalog.tiles.front().path,
                               std::string("far/catalog.json"),
                               std::string(kSnapshotIndexRelativePath)}) {
        for (const auto* step : {"before-replace", "after-replace"}) {
            SCOPED_TRACE(member + " " + step);
            fs::remove_all(root);
            fs::copy(backup, root, fs::copy_options::recursive);
            chunk->sdf_data[1 + 17 * (1 + 17)] = -8;
            chunk->mark_voxel_data_dirty();
            CacheCrashPlan plan;
            plan.live = root;
            plan.copied = interrupted;
            plan.member = root / member;
            plan.step = step;
            {
                CacheCrashScope hook(plan);
                ASSERT_TRUE(Save());
            }
            ASSERT_TRUE(plan.hit);
            const bool published =
                member == kSnapshotIndexRelativePath && std::string_view(step) == "after-replace";
            if (!published) {
                CacheCrashPlan rollback;
                rollback.step = "rollback-entry";
                rollback.member = snapshot_index_path(interrupted);
                rollback.interrupt_rollback = true;
                {
                    CacheCrashScope hook(rollback);
                    EXPECT_EQ(recover_pending_journal(interrupted, nullptr),
                              RecoveryOutcome::Failed);
                }
                EXPECT_TRUE(rollback.hit);
                EXPECT_TRUE(journal_pending(interrupted));
            }
            EXPECT_EQ(recover_pending_journal(interrupted, nullptr),
                      published ? RecoveryOutcome::DiscardedStale : RecoveryOutcome::RolledBack);
            EXPECT_EQ(ReadCacheTree(interrupted), published ? ReadCacheTree(root) : baseline);
            EXPECT_EQ(recover_pending_journal(interrupted, nullptr), RecoveryOutcome::NothingToDo);
            EXPECT_TRUE(WorldSaveService::validate_save(interrupted));
        }
    }
    fs::remove_all(backup);
    fs::remove_all(interrupted);
}
TEST_F(FarEditedCacheTest, AbortDoesNotNotifyAcknowledgeOrPublishPreparedAuthority) {
    auto chunk = FarCacheEditedChunk(state);
    FarEditedCacheSaveScope scope(root, policy);
    SaveTransaction tx(root, 0, {}, &WorldSaveService::member_digest);
    ASSERT_TRUE(tx.begin(nullptr));
    ASSERT_TRUE(WorldSaveService{}.save_dirty_chunks(state, root).saved);
    std::shared_ptr<const EditedSourceImage> prepared;
    ASSERT_EQ(CapturePreparedEditedSource(tx, {policy->selection}, policy->limits.source, prepared),
              EditedSourceError::None);
    ASSERT_TRUE(prepared);
    FarCacheReconciliation previous;
    EXPECT_EQ(ReconcileFarEditedCache(*prepared, {}, policy->limits, previous),
              FarCacheError::InvalidSource);
    tx.abort(nullptr);
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_FALSE(prepared->RendererEligible());
    EXPECT_FALSE(prepared->PublicationReceipt());
    EXPECT_FALSE(fs::exists(root / "far/catalog.json"));
}
TEST_F(FarEditedCacheTest, EveryAdmissionRefusalPreservesOutputAndLifetimeCharges) {
    FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    std::shared_ptr<const FarCommittedCache> cache;
    ASSERT_EQ(Acquire(1, cache), FarCacheError::None);
    ASSERT_TRUE(cache);
    const auto used = policy->limits.source.memory->used();
    auto previous = cache;
    EXPECT_EQ(Acquire(2, cache), FarCacheError::GenerationMismatch);
    EXPECT_EQ(cache, previous);
    EXPECT_EQ(policy->limits.source.memory->used(), used);
    for (int mode = 0; mode < 5; ++mode) {
        auto limits = policy->limits;
        if (mode == 0)
            limits.max_pipeline_bytes = 1;
        if (mode == 1)
            limits.max_cache_bytes = 1;
        if (mode == 2)
            limits.max_reconcile_probes = 0;
        if (mode == 3)
            limits.source.max_image_bytes = 1;
        if (mode == 4)
            limits.source.max_directory_entries = 0;
        EXPECT_NE(AcquireFarCommittedCache({root, {policy->selection}}, 1, limits, cache),
                  FarCacheError::None);
        EXPECT_EQ(cache, previous);
        EXPECT_EQ(policy->limits.source.memory->used(), used);
    }
    cache.reset();
    EXPECT_EQ(policy->limits.source.memory->used(), used);
    previous.reset();
    EXPECT_EQ(policy->limits.source.memory->used(), 0u);
}
TEST_F(FarEditedCacheTest, AcquiredCacheOwnsDecodedDataAfterEditAndClose) {
    auto chunk = FarCacheEditedChunk(state);
    ASSERT_TRUE(Save());
    std::shared_ptr<const FarCommittedCache> cache;
    ASSERT_EQ(Acquire(1, cache), FarCacheError::None);
    ASSERT_TRUE(cache);
    ASSERT_FALSE(cache->tiles.empty());
    std::string bytes;
    ASSERT_EQ(EncodeFarOverlay(cache->tiles.back(), bytes), FarOverlayError::None);
    chunk->sdf_data.assign(kCacheLatticeSamples, 99);
    state.clear();
    chunk.reset();
    fs::remove_all(root);
    std::string retained;
    ASSERT_EQ(EncodeFarOverlay(cache->tiles.back(), retained), FarOverlayError::None);
    EXPECT_EQ(retained, bytes);
    ASSERT_FALSE(cache->source->Lattices().empty());
    EXPECT_EQ(cache->source->Lattices()[0].density[1 + 17 * (1 + 17)], -3);
    EXPECT_GT(policy->limits.source.memory->used(), 0u);
    cache.reset();
    EXPECT_EQ(policy->limits.source.memory->used(), 0u);
}
TEST_F(FarEditedCacheTest, CaptureAndBakeFailureCommitAuthorityAndKeepRetryDue) {
    auto chunk = FarCacheEditedChunk(state);
    const auto producer = policy->limits.capture_source;
    policy->limits.capture_source = {};
    ASSERT_TRUE(Save());
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_TRUE(reports.back().derived_due);
    EXPECT_EQ(reports.back().refusal, FarCacheError::CaptureFailure);
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    policy->limits.capture_source = producer;
    policy->limits.bake.max_density_samples = 0;
    ASSERT_TRUE(Save());
    EXPECT_TRUE(reports.back().derived_due);
    policy->limits.bake.max_density_samples = 2'000'000;
    ASSERT_TRUE(Save());
    EXPECT_FALSE(reports.back().derived_due);
}
TEST_F(FarEditedCacheTest, LegacyAbsenceAdoptsWithoutFabricatedEditedContent) {
    std::shared_ptr<const FarCommittedCache> cache;
    ASSERT_EQ(Acquire(0, cache), FarCacheError::None);
    ASSERT_TRUE(cache);
    EXPECT_TRUE(cache->source->LegacyAuthority());
    EXPECT_TRUE(cache->source->Inventory().empty());
    EXPECT_TRUE(cache->tiles.empty());
    EXPECT_FALSE(cache->catalog.present);
    EXPECT_FALSE(fs::exists(root / "far"));
    EXPECT_FALSE(snapshot_index_exists(root));
    ASSERT_TRUE(Save());
    EXPECT_TRUE(snapshot_index_exists(root));
    EXPECT_FALSE(fs::exists(root / "far"));
}
TEST_F(FarEditedCacheTest, LegacyUnreceiptedGenerationsAreRebuiltOrRemovedBeforeAdoption) {
    FarCacheEditedChunk(state);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, root));
    FarOverlayTile legacy;
    legacy.key = {1, 0, 0};
    legacy.generation = 99;
    ASSERT_TRUE(WriteFarOverlay(root, legacy));
    std::shared_ptr<const FarCommittedCache> reconstructed;
    ASSERT_EQ(Acquire(0, reconstructed), FarCacheError::None);
    ASSERT_TRUE(reconstructed);
    EXPECT_TRUE(reconstructed->report.derived_due);
    ASSERT_TRUE(Save());
    EXPECT_TRUE(WorldSaveService::validate_save(root));
    auto installed = ReadFarOverlay(root, legacy.key);
    ASSERT_EQ(installed.error, FarOverlayError::None);
    EXPECT_EQ(installed.tile.generation, 1u);
    // Repeat validated legacy adoption with a failed optional producer.
    ASSERT_TRUE(WorldSaveService::forget_snapshot_index_for_testing(root));
    ASSERT_TRUE(WriteFarOverlay(root, legacy));
    policy->limits.capture_source = {};
    ASSERT_TRUE(Save());
    EXPECT_TRUE(reports.back().authoritative_success);
    EXPECT_TRUE(reports.back().derived_due);
    EXPECT_FALSE(fs::exists(FarOverlayPath(root, legacy.key)));
    EXPECT_FALSE(fs::exists(root / "far/catalog.json"));
    EXPECT_TRUE(WorldSaveService::validate_save(root));
}
TEST_F(FarEditedCacheTest, CacheCreationAndRemovalLeaveSimulationHashesAndLegacyBytesEqual) {
    FarCacheEditedChunk(state);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, root));
    const auto region_path = WorldSaveService::region_file_path(root, 0, 0);
    const auto region = FarCacheRead(region_path);
    const auto manifest = FarCacheRead(WorldSaveService::world_manifest_path(root));
    const auto hash = WorldSaveService{}.world_hash(state);
    const auto digest =
        WorldSaveService::region_simulation_digest(root, {0, 0}, state.snapshot_chunks());
    ASSERT_FALSE(snapshot_index_exists(root));
    ASSERT_TRUE(Save());
    EXPECT_EQ(FarCacheRead(region_path), region);
    EXPECT_EQ(FarCacheRead(WorldSaveService::world_manifest_path(root)), manifest);
    EXPECT_EQ(WorldSaveService{}.world_hash(state), hash);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root, {0, 0}, state.snapshot_chunks()),
              digest);
    fs::remove_all(root / "far");
    EXPECT_EQ(WorldSaveService{}.world_hash(state), hash);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root, {0, 0}, state.snapshot_chunks()),
              digest);
    EXPECT_TRUE(WorldSaveService::validate_save(root));
}
} // namespace
