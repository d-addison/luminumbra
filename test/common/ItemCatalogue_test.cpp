#include "../support/UniqueTempPath.h"
#include "luminumbra_common/game/ItemCatalogue.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <vector>

namespace {
using luminumbra::game::ItemCatalogue;
using luminumbra::game::ItemTypeId;
using Json = nlohmann::json;
Json CatalogueDocument() {
    return {
        {"schema", ItemCatalogue::kSchema},
        {"items", Json::array({{{"id", "farming-seed"}, {"despawn_after", 900}, {"payload", 0}}})}};
}
std::string Bytes(const Json& document) {
    return document.dump() + "\n";
}

TEST(ItemCatalogueTest, PinnedTypeVectorsAndRealCollision) {
    EXPECT_EQ(ItemTypeId(""), 2166136261u);
    EXPECT_EQ(ItemTypeId("a"), 3826002220u);
    EXPECT_EQ(ItemTypeId("farming-seed"), 516719959u);
    EXPECT_EQ(ItemTypeId("costarring"), 1582148253u);
    EXPECT_EQ(ItemTypeId("liquid"), 1582148253u);
    EXPECT_EQ(ItemTypeId("fxdsatwp"), 0u);
}
TEST(ItemCatalogueTest, FindByNameAndTypeAndUnsignedBoundaries) {
    ItemCatalogue catalogue;
    std::string error;
    auto document = CatalogueDocument();
    document["items"][0]["despawn_after"] = UINT32_MAX;
    document["items"][0]["payload"] = UINT32_MAX;
    ASSERT_TRUE(ItemCatalogue::parse(Bytes(document), catalogue, error)) << error;
    const auto* seed = catalogue.FindByName("farming-seed");
    ASSERT_NE(seed, nullptr);
    EXPECT_EQ(catalogue.FindByType(516719959u), seed);
    EXPECT_EQ(seed->despawn_after, UINT32_MAX);
    EXPECT_EQ(seed->payload, UINT32_MAX);
    EXPECT_EQ(catalogue.FindByName("Farming-seed"), nullptr);
    EXPECT_EQ(catalogue.FindByType(0), nullptr);
    EXPECT_FALSE(catalogue.ValidateGameAdmission(error));
    document["items"][0]["payload"] = 0;
    document["items"][0]["despawn_after"] = 0;
    ASSERT_TRUE(ItemCatalogue::parse(Bytes(document), catalogue, error));
    EXPECT_TRUE(catalogue.ValidateGameAdmission(error));
    EXPECT_EQ(catalogue.FindByName("farming-seed")->despawn_after, 0u);
}
TEST(ItemCatalogueTest, StrictRefusalsLeavePreviouslyParsedStateUnchanged) {
    ItemCatalogue catalogue;
    std::string error;
    const auto valid = CatalogueDocument();
    ASSERT_TRUE(ItemCatalogue::parse(Bytes(valid), catalogue, error));
    std::vector<std::string> invalid = {
        "",
        "{}\n",
        "[]\n",
        Bytes(valid) + "{}\n",
        Bytes(valid).substr(0, Bytes(valid).size() - 1),
        "\xef\xbb\xbf" + Bytes(valid),
        "{\r\n}\r\n",
        "{\"schema\":\"luminumbra.game.items.v1\",\"schema\":\"luminumbra.game.items.v1\","
        "\"items\":[]}\n",
        "{\"schema\":\"luminumbra.game.items.v1\",\"items\":[{\"id\":\"a\",\"payload\":0,"
        "\"payload\":0,\"despawn_after\":1}]}\n"};
    for (const char* key : {"schema", "items"}) {
        auto document = valid;
        document.erase(key);
        invalid.push_back(Bytes(document));
    }
    for (const char* key : {"id", "payload", "despawn_after"}) {
        auto document = valid;
        document["items"][0].erase(key);
        invalid.push_back(Bytes(document));
    }
    for (const Json& value : {Json(true),
                              Json(false),
                              Json(1.5),
                              Json("1"),
                              Json(-1),
                              Json(nullptr),
                              Json(4294967296ull)}) {
        for (const char* key : {"payload", "despawn_after"}) {
            auto document = valid;
            document["items"][0][key] = value;
            invalid.push_back(Bytes(document));
        }
    }
    for (const auto& name : {"", "A", "a_b", "-a", "a-", "a--b", "fxdsatwp"}) {
        auto document = valid;
        document["items"][0]["id"] = name;
        invalid.push_back(Bytes(document));
    }
    for (const std::string& name :
         {std::string(65, 'a'), std::string("a\0b", 3), std::string("caf\xc3\xa9")}) {
        auto document = valid;
        document["items"][0]["id"] = name;
        invalid.push_back(Bytes(document));
    }
    auto document = valid;
    document["unknown"] = 1;
    invalid.push_back(Bytes(document));
    document = valid;
    document["schema"] = "luminumbra.game.items.v2";
    invalid.push_back(Bytes(document));
    document = valid;
    document["items"][0]["type"] = 516719959u;
    invalid.push_back(Bytes(document));
    document = valid;
    document["items"].push_back(document["items"][0]);
    invalid.push_back(Bytes(document));
    document = valid;
    document["items"].push_back({{"id", "a"}, {"payload", 0}, {"despawn_after", 0}});
    invalid.push_back(Bytes(document));
    document = valid;
    document["items"] = Json::array();
    for (const char* name : {"costarring", "liquid"})
        document["items"].push_back({{"id", name}, {"payload", 0}, {"despawn_after", 0}});
    invalid.push_back(Bytes(document));
    auto decimal = Bytes(valid);
    decimal.replace(decimal.find("900"), 3, "900.0");
    invalid.push_back(decimal);
    auto exponent = Bytes(valid);
    exponent.replace(exponent.find("900"), 3, "9e2");
    invalid.push_back(exponent);
    auto bad_utf8 = Bytes(valid);
    bad_utf8.insert(bad_utf8.find("farming-seed"), 1, '\xff');
    invalid.push_back(bad_utf8);
    invalid.push_back(std::string(ItemCatalogue::kMaxBytes + 1, ' '));
    for (const auto& bytes : invalid) {
        EXPECT_FALSE(ItemCatalogue::parse(bytes, catalogue, error)) << bytes.substr(0, 180);
        EXPECT_FALSE(error.empty());
        ASSERT_NE(catalogue.FindByName("farming-seed"), nullptr);
        EXPECT_EQ(catalogue.FindByType(516719959u)->despawn_after, 900u);
    }
}
TEST(ItemCatalogueTest, OptionalRenderIsStrictAndRequiresReferenceResolution) {
    ItemCatalogue catalogue;
    std::string error;
    auto document = CatalogueDocument();
    document["items"][0]["render"] = {{"model_id", "farming-seed"}, {"material_id", 2}};
    ASSERT_TRUE(ItemCatalogue::parse(Bytes(document), catalogue, error));
    EXPECT_FALSE(catalogue.ValidateGameAdmission(error));
    for (const Json& render :
         {Json(nullptr),
          Json::object(),
          Json{{"model_id", "farming-seed"}},
          Json{{"model_id", "farming-seed"}, {"material_id", true}},
          Json{{"model_id", "farming-seed"}, {"material_id", 2}, {"extra", 0}}}) {
        document["items"][0]["render"] = render;
        EXPECT_FALSE(ItemCatalogue::parse(Bytes(document), catalogue, error));
        EXPECT_EQ(catalogue.FindByName("farming-seed")->render->material_id, 2u);
    }
}
TEST(ItemCatalogueTest, EntryLimitAndRequiredSeedAdmission) {
    ItemCatalogue catalogue;
    std::string error;
    auto document = CatalogueDocument();
    document["items"] = Json::array();
    ASSERT_TRUE(ItemCatalogue::parse(Bytes(document), catalogue, error));
    EXPECT_FALSE(catalogue.ValidateGameAdmission(error));
    for (std::size_t i = 0; i < ItemCatalogue::kMaxItems; ++i) {
        const auto digits = std::to_string(i);
        document["items"].push_back(
            {{"id", "entry-" + std::string(4 - digits.size(), '0') + digits},
             {"payload", 0},
             {"despawn_after", 0}});
    }
    ASSERT_TRUE(ItemCatalogue::parse(Bytes(document), catalogue, error)) << error;
    EXPECT_NE(catalogue.FindByName("entry-4095"), nullptr);
    document["items"].push_back({{"id", "entry-4096"}, {"payload", 0}, {"despawn_after", 0}});
    EXPECT_FALSE(ItemCatalogue::parse(Bytes(document), catalogue, error));
    EXPECT_NE(error.find("limit"), std::string::npos);
}
TEST(ItemCatalogueTest, LoaderRefusesAbsentNonregularAndMalformedContentTransactionally) {
    const auto root = Luminumbra::TestSupport::CreateUniqueTempDirectory("item_catalogue_");
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } cleanup{root};
    ItemCatalogue catalogue;
    std::string error;
    const auto path = root / "items.json";
    std::ofstream(path, std::ios::binary) << Bytes(CatalogueDocument());
    ASSERT_TRUE(ItemCatalogue::load(path, catalogue, error));
    EXPECT_FALSE(ItemCatalogue::load(root / "absent.json", catalogue, error));
    EXPECT_FALSE(ItemCatalogue::load(root, catalogue, error));
    std::ofstream(path, std::ios::binary) << "{}\n";
    EXPECT_FALSE(ItemCatalogue::load(path, catalogue, error));
    EXPECT_EQ(catalogue.FindByName("farming-seed")->type, 516719959u);
}
} // namespace
