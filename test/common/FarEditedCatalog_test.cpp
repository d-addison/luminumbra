#include "../support/UniqueTempPath.h"
#include "nlohmann/json.hpp"
#include "persistence/SaveTransaction.h"
#include "persistence/WorldSaveService.h"
#include "world/Chunk.h"
#include "world/FarAuthorityOverlay.h"
#include "world/FarEditedCatalog.h"
#include "world/WorldStreamingState.h"
#include "gtest/gtest.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <ios>
#include <limits>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Persistence;
using Json = nlohmann::json;

FarEditedCatalog Catalog() {
    FarEditedCatalog catalog;
    catalog.present = true;
    FarCatalogReceipt r;
    r.key = {1, -1, 0};
    r.path = "far/v1.-1.0.lmr";
    r.source_generation = 4;
    r.overlay_bytes = kFsv1HeaderBytes + kFsv1TrailerBytes;
    r.overlay_fingerprint = 13;
    r.terrain_identity = 17;
    r.content_identity = 29;
    r.edited_span = {-16, 32};
    FarCatalogDependency d;
    d.member_path = "chunks/region/r.-1.0.lmr";
    d.chunk_id = Chunk::calculate_id({-1, 0, 0});
    d.member_generation = 3;
    d.member_bytes = 4096;
    d.member_digest = 11;
    d.chunk_digest = 12;
    d.footprint = {{-16, 0, 0}, {0, 16, 16}};
    r.dependencies.push_back(d);
    catalog.tiles.push_back(r);
    return catalog;
}
TEST(FarEditedCatalog, DeterministicRoundTripRetainsReceiptDependenciesAndSpan) {
    auto catalog = Catalog();
    std::string bytes, second;
    ASSERT_EQ(EncodeFarEditedCatalog(catalog, {}, bytes), FarCatalogError::None);
    ASSERT_EQ(EncodeFarEditedCatalog(catalog, {}, second), FarCatalogError::None);
    EXPECT_EQ(bytes, second);
    FarEditedCatalog decoded;
    ASSERT_EQ(DecodeFarEditedCatalog(bytes, {}, decoded), FarCatalogError::None);
    ASSERT_EQ(decoded.tiles.size(), 1u);
    ASSERT_EQ(decoded.tiles[0].dependencies.size(), 1u);
    EXPECT_TRUE(SameFarCatalogDependency(decoded.tiles[0].dependencies[0],
                                         catalog.tiles[0].dependencies[0]));
    EXPECT_EQ(decoded.tiles[0].edited_span, catalog.tiles[0].edited_span);
    EXPECT_FALSE(decoded.tiles[0].PublicationVerifiedFor(4));
    ASSERT_EQ(EncodeFarEditedCatalog(decoded, {}, second), FarCatalogError::None);
    EXPECT_EQ(second, bytes);
}
TEST(FarEditedCatalog, EveryCodecRefusalPreservesPreviousOutput) {
    std::string bytes;
    ASSERT_EQ(EncodeFarEditedCatalog(Catalog(), {}, bytes), FarCatalogError::None);
    const auto original = Json::parse(bytes);
    std::vector<Json> malformed;
    auto add = [&](const std::function<void(Json&)>& mutate) {
        auto value = original;
        mutate(value);
        malformed.push_back(std::move(value));
    };
    add([](auto& j) { j["schema"] = "luminumbra.far_catalog.v2"; });
    add([](auto& j) { j["extra"] = 0; });
    add([](auto& j) { j["tiles"] = 3; });
    add([](auto& j) { j["tiles"].push_back(j["tiles"][0]); });
    add([](auto& j) {
        auto next = j["tiles"][0];
        next["tx"] = -2;
        next["path"] = "far/v1.-2.0.lmr";
        j["tiles"].push_back(next);
    });
    add([](auto& j) { j["tiles"][0]["tier"] = 257; });
    add([](auto& j) { j["tiles"][0]["tx"] = 1.0; });
    add([](auto& j) { j["tiles"][0]["tx"] = std::numeric_limits<std::uint64_t>::max(); });
    add([](auto& j) { j["tiles"][0]["path"] = "far/v1.0.0.lmr"; });
    add([](auto& j) { j["tiles"][0]["source_generation"] = -1; });
    add([](auto& j) { j["tiles"][0]["overlay_bytes"] = 1; });
    add([](auto& j) { j["tiles"][0]["numeric"] = 0; });
    add([](auto& j) { j["tiles"][0]["caves"] = 256; });
    add([](auto& j) { j["tiles"][0]["span"] = {0, 1}; });
    add([](auto& j) { j["tiles"][0]["span"] = {16, -16}; });
    add([](auto& j) { j["tiles"][0]["span"] = {0}; });
    add([](auto& j) { j["tiles"][0]["dependencies"] = nullptr; });
    add([](auto& j) {
        auto& d = j["tiles"][0]["dependencies"];
        d.push_back(d[0]);
    });
    add([](auto& j) { j["tiles"][0]["dependencies"][0]["member"] = "world_info.json"; });
    add([](auto& j) { j["tiles"][0]["dependencies"][0]["member"] = "chunks/region/r.0.0.lmr"; });
    add([](auto& j) { j["tiles"][0]["dependencies"][0]["chunk"] = 0; });
    add([](auto& j) { j["tiles"][0]["dependencies"][0]["generation"] = 5; });
    add([](auto& j) { j["tiles"][0]["dependencies"][0]["bytes"] = 0; });
    add([](auto& j) { j["tiles"][0]["dependencies"][0]["min"] = {-15, 0, 0}; });
    add([](auto& j) { j["tiles"][0]["dependencies"][0].erase("digest"); });
    const auto root = TestSupport::CreateUniqueTempDirectory("far_catalog_refusal_");
    ASSERT_TRUE(WorldSaveService{}.save_world(WorldStreamingState{}, root));
    for (const auto& j : malformed) {
        auto output = Catalog();
        EXPECT_NE(DecodeFarEditedCatalog(j.dump(), {}, output), FarCatalogError::None) << j.dump();
        ASSERT_EQ(output.tiles.size(), 1u);
        EXPECT_EQ(output.tiles[0].source_generation, 4u);
        ASSERT_TRUE(WorldSaveService::durable_write_file(root / "far/catalog.json", j.dump()));
        EXPECT_FALSE(WorldSaveService::validate_save(root)) << j.dump();
    }
    auto output = Catalog();
    EXPECT_EQ(DecodeFarEditedCatalog("{", {}, output), FarCatalogError::Malformed);
    EXPECT_EQ(
        DecodeFarEditedCatalog(
            R"({"schema":"luminumbra.far_catalog.v1","schema":"luminumbra.far_catalog.v1","tiles":[]})",
            {},
            output),
        FarCatalogError::Malformed);
    FarCatalogLimits limits;
    limits.max_tiles = 0;
    EXPECT_EQ(DecodeFarEditedCatalog(bytes, limits, output), FarCatalogError::LimitExceeded);
    limits = {};
    limits.max_dependencies = 0;
    EXPECT_EQ(DecodeFarEditedCatalog(bytes, limits, output), FarCatalogError::LimitExceeded);
    limits = {};
    limits.max_bytes = bytes.size() - 1;
    EXPECT_EQ(DecodeFarEditedCatalog(bytes, limits, output), FarCatalogError::LimitExceeded);
    std::filesystem::remove(root / "far/catalog.json");
    std::filesystem::create_directory(root / "far/catalog.json");
    EXPECT_EQ(ReadFarEditedCatalog(root, {}, output), FarCatalogError::IoError);
    ASSERT_EQ(output.tiles.size(), 1u);
    EXPECT_EQ(output.tiles[0].source_generation, 4u);
    EXPECT_FALSE(WorldSaveService::validate_save(root));
    std::filesystem::remove_all(root);
}
void ReplaceCatalogAtHashOpen(const char* step, const std::filesystem::path& path) {
    if (std::string_view(step) != "hash-opened" || path.filename() != "catalog.json")
        return;
    // A second-open fingerprint would consume different bytes from the validated document.
    const auto size = std::filesystem::file_size(path);
    ASSERT_LT(size, 256u);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << std::string(static_cast<std::size_t>(size), 'x');
    output.flush();
    ASSERT_TRUE(output.good());
}
TEST(FarEditedCatalog, BothDigestModesFingerprintOnlyTheValidatedBytes) {
    const auto root = TestSupport::CreateUniqueTempDirectory("far_catalog_digest_");
    const std::string bytes = R"({"schema":"luminumbra.far_catalog.v1","tiles":[]})";
    struct ResetHook {
        ~ResetHook() {
            set_snapshot_step_hook_for_testing(nullptr);
        }
    };
    const ResetHook reset;
    for (const auto digest : {&FarEditedCatalogDigest, &InspectFarEditedCatalogDigest}) {
        ASSERT_TRUE(WorldSaveService::durable_write_file(root / "far/catalog.json", bytes));
        set_snapshot_step_hook_for_testing(&ReplaceCatalogAtHashOpen);
        MemberDigest actual;
        std::string error;
        EXPECT_TRUE(digest(root, "far/catalog.json", actual, error)) << error;
        EXPECT_TRUE(actual.present);
        EXPECT_EQ(actual.bytes, bytes.size());
        EXPECT_EQ(actual.fnv64, fnv1a64_bytes(bytes.data(), bytes.size()));
        set_snapshot_step_hook_for_testing(nullptr);
    }
    std::filesystem::remove_all(root);
}
TEST(FarEditedCatalog, AbsenceDoesNotFabricateContentAndReadOnlyInspectionReportsRecovery) {
    const auto root = TestSupport::CreateUniqueTempDirectory("far_catalog_");
    FarEditedCatalog catalog = Catalog();
    ASSERT_EQ(ReadFarEditedCatalog(root, {}, catalog), FarCatalogError::None);
    EXPECT_FALSE(catalog.present);
    EXPECT_TRUE(catalog.tiles.empty());
    std::filesystem::create_directories(root / "journal");
    DerivedMemberState state = DerivedMemberState::Missing;
    std::string error;
    EXPECT_FALSE(WorldSaveService::inspect_derived_member(
        root, "far/catalog.json", {}, state, error, &FarEditedCatalogDigest));
    EXPECT_EQ(state, DerivedMemberState::RecoveryPending);
    EXPECT_TRUE(std::filesystem::exists(root / "journal"));
    std::filesystem::remove_all(root);
}
} // namespace
