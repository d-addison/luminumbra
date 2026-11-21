#include "gtest/gtest.h"

#include "world/BiomeTable.h"
#include "world/MaterialRegistry.h"

#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

namespace {

using Luminumbra::u32;
using Luminumbra::u8;
using Luminumbra::World::BiomeTable;
using Luminumbra::World::MaterialRegistry;
using Luminumbra::World::WorldgenFileReader;

WorldgenFileReader BiomeReader(const std::string& surface) {
    return [document = "{\"biomes\":[{\"id\":0,\"surface\":" + surface +
                       "}]}"](const std::filesystem::path&, std::string& bytes) {
        bytes = document;
        return true;
    };
}

std::string UniformSurface(std::string_view name) {
    const std::string quoted = "\"" + std::string(name) + "\"";
    return "{\"top\":" + quoted + ",\"filler\":" + quoted + ",\"depth\":" + quoted +
           ",\"underwater\":" + quoted + "}";
}

constexpr std::array<std::string_view, 8> kBaseNames{
    "Air", "Stone", "Soil", "Grass", "Sand", "Deepslate", "LuminCrystal", "Water"};

} // namespace

TEST(MaterialRegistryTest, DefaultIdsExactlyMatchMaterialType) {
    const MaterialRegistry registry;
    constexpr std::array<Luminumbra::MaterialType, 8> materials{
        Luminumbra::MaterialType::Air,
        Luminumbra::MaterialType::Stone,
        Luminumbra::MaterialType::Soil,
        Luminumbra::MaterialType::Grass,
        Luminumbra::MaterialType::Sand,
        Luminumbra::MaterialType::Deepslate,
        Luminumbra::MaterialType::LuminCrystal,
        Luminumbra::MaterialType::Water};
    for (std::size_t index = 0; index < kBaseNames.size(); ++index) {
        EXPECT_EQ(registry.Resolve(kBaseNames[index]), std::optional<u8>(static_cast<u8>(index)));
        EXPECT_EQ(static_cast<u32>(materials[index]), index);
    }
    for (std::string_view name : {"", "stone", "Stone ", "test:missing"}) {
        EXPECT_FALSE(registry.Resolve(name).has_value());
    }
}

TEST(MaterialRegistryTest, ExplicitIdsIgnoreRegistrationOrderAndRefuseReassignment) {
    MaterialRegistry first;
    MaterialRegistry second;
    ASSERT_TRUE(first.Register("test:basalt", 8));
    ASSERT_TRUE(first.Register("test:granite", 255));
    ASSERT_TRUE(second.Register("test:granite", 255));
    ASSERT_TRUE(second.Register("test:basalt", 8));
    EXPECT_EQ(first.Resolve("test:basalt"), second.Resolve("test:basalt"));
    EXPECT_EQ(first.Resolve("test:granite"), std::optional<u8>(255));
    EXPECT_FALSE(first.Register("test:basalt", 9));
    EXPECT_FALSE(first.Register("test:other", 8));
    EXPECT_FALSE(first.Register("test:basalt", 8));
    EXPECT_FALSE(first.Resolve("test:other").has_value());
    EXPECT_EQ(first.Resolve("test:basalt"), std::optional<u8>(8));
    ASSERT_TRUE(first.Register("test:other", 9));
}

TEST(MaterialRegistryTest, InvalidNamesAndIdsLeaveSlotsAvailable) {
    MaterialRegistry registry;
    for (std::string_view name : {"",
                                  "basalt",
                                  ":basalt",
                                  "test:",
                                  "test:a:b",
                                  "Test:basalt",
                                  "test:ba salt",
                                  "test:../basalt",
                                  "test:basalt\n"}) {
        EXPECT_FALSE(registry.Register(name, 8)) << name;
        EXPECT_FALSE(registry.Resolve(name).has_value());
    }
    EXPECT_FALSE(registry.Register(std::string("test:a\0b", 8), 8));
    EXPECT_FALSE(registry.Register("test:\xc3\xa9", 8));
    EXPECT_FALSE(registry.Register("test:" + std::string(124, 'a'), 8));
    EXPECT_FALSE(registry.Resolve("test:" + std::string(124, 'a')).has_value());
    for (u32 id = 0; id < 8; ++id) {
        EXPECT_FALSE(registry.Register("test:basalt", id));
        EXPECT_FALSE(registry.Register(kBaseNames[id], 8));
        EXPECT_EQ(registry.Resolve(kBaseNames[id]), std::optional<u8>(static_cast<u8>(id)));
    }
    EXPECT_FALSE(registry.Register("test:basalt", 256));
    EXPECT_FALSE(registry.Register("test:basalt", std::numeric_limits<u32>::max()));
    ASSERT_TRUE(registry.Register("test:basalt", 8));
    const std::string longest = "test:" + std::string(123, 'a');
    ASSERT_TRUE(registry.Register(longest, 9));
    EXPECT_EQ(registry.Resolve(longest), std::optional<u8>(9));
    ASSERT_TRUE(registry.Register("mod.name-1:rock_type.2", 10));
}

TEST(MaterialRegistryTest, FullByteIdSpaceCannotRecycleSlots) {
    MaterialRegistry registry;
    for (u32 id = 8; id <= 255; ++id) {
        ASSERT_TRUE(registry.Register("test:material_" + std::to_string(id), id));
    }
    for (u32 id = 8; id <= 255; ++id) {
        EXPECT_EQ(registry.Resolve("test:material_" + std::to_string(id)),
                  std::optional<u8>(static_cast<u8>(id)));
        EXPECT_FALSE(registry.Register("test:overflow", id));
    }
    EXPECT_FALSE(registry.Resolve("test:overflow").has_value());
}

TEST(MaterialRegistryTest, CustomBiomePaletteResolutionIsOptIn) {
    MaterialRegistry registry;
    ASSERT_TRUE(registry.Register("test:basalt", 8));
    const auto reader = BiomeReader(UniformSurface("test:basalt"));
    const auto legacy = BiomeTable::Load("biomes.json", reader);
    EXPECT_FALSE(legacy.ok());
    EXPECT_TRUE(legacy.empty());
    EXPECT_EQ(legacy.errors().size(), 4u);
    const auto table = BiomeTable::LoadWithRegistry("biomes.json", registry, reader);
    ASSERT_TRUE(table.ok());
    ASSERT_EQ(table.size(), 1u);
    const auto& palette = table.palette_for(0);
    EXPECT_EQ(palette.top, 8);
    EXPECT_EQ(palette.filler, 8);
    EXPECT_EQ(palette.depth, 8);
    EXPECT_EQ(palette.underwater, 8);
    EXPECT_EQ(table.lookup(0, 0, 0, 0, 0), 0);
}

TEST(MaterialRegistryTest, LegacyNameSetAndBasePaletteHashesRemainUnchanged) {
    const MaterialRegistry registry;
    for (std::size_t index = 0; index < kBaseNames.size(); ++index) {
        const auto reader = BiomeReader(UniformSurface(kBaseNames[index]));
        const auto legacy = BiomeTable::Load("biomes.json", reader);
        const auto table = BiomeTable::LoadWithRegistry("biomes.json", registry, reader);
        ASSERT_TRUE(table.ok());
        EXPECT_EQ(table.palette_for(0).top, index);
        EXPECT_EQ(table.palette_for(0).filler, index);
        EXPECT_EQ(table.palette_for(0).depth, index);
        EXPECT_EQ(table.palette_for(0).underwater, index);
        if (kBaseNames[index] == "LuminCrystal") {
            EXPECT_FALSE(legacy.ok());
            EXPECT_TRUE(legacy.empty());
            EXPECT_EQ(legacy.errors().size(), 4u);
        } else {
            ASSERT_TRUE(legacy.ok());
            EXPECT_EQ(legacy.content_hash(), table.content_hash());
        }
    }
}

TEST(MaterialRegistryTest, DefaultPaletteHashMatchesLegacyGolden) {
    const MaterialRegistry registry;
    const auto reader = BiomeReader("{}");
    const auto legacy = BiomeTable::Load("biomes.json", reader);
    const auto table = BiomeTable::LoadWithRegistry("biomes.json", registry, reader);
    ASSERT_TRUE(legacy.ok());
    ASSERT_TRUE(table.ok());
    EXPECT_EQ(legacy.content_hash(), 3037997127034371765ull);
    EXPECT_EQ(table.content_hash(), legacy.content_hash());
}

TEST(MaterialRegistryTest, ShippedTablePaletteBytesAndHashMatchLegacy) {
    MaterialRegistry registry;
    ASSERT_TRUE(registry.Register("test:basalt", 8));
    const auto path = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "data/common/biomes.json";
    const auto legacy = BiomeTable::Load(path);
    const auto table = BiomeTable::LoadWithRegistry(path, registry);
    ASSERT_TRUE(legacy.ok());
    ASSERT_TRUE(table.ok());
    EXPECT_EQ(table.content_hash(), legacy.content_hash());
    EXPECT_EQ(table.warnings(), legacy.warnings());
    ASSERT_EQ(table.size(), legacy.size());
    for (const auto& biome : legacy.biomes()) {
        const auto& palette = table.palette_for(biome.id);
        EXPECT_EQ(palette.top, biome.palette.top);
        EXPECT_EQ(palette.filler, biome.palette.filler);
        EXPECT_EQ(palette.depth, biome.palette.depth);
        EXPECT_EQ(palette.underwater, biome.palette.underwater);
    }
}

TEST(MaterialRegistryTest, HashUsesResolvedPaletteBytesOnly) {
    MaterialRegistry first;
    MaterialRegistry second;
    ASSERT_TRUE(first.Register("test:basalt", 8));
    ASSERT_TRUE(second.Register("test:granite", 8));
    ASSERT_TRUE(second.Register("test:basalt", 9));
    const auto reader = BiomeReader(UniformSurface("test:basalt"));
    const auto a = BiomeTable::LoadWithRegistry("biomes.json", first, reader);
    const auto b = BiomeTable::LoadWithRegistry("biomes.json", second, reader);
    const auto c = BiomeTable::LoadWithRegistry(
        "biomes.json", second, BiomeReader(UniformSurface("test:granite")));
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(c.ok());
    EXPECT_NE(a.content_hash(), b.content_hash());
    EXPECT_EQ(a.content_hash(), c.content_hash());
}

TEST(MaterialRegistryTest, RegistryLoaderRefusesUnknownNamesAndUnreadableOrMalformedInput) {
    const MaterialRegistry registry;
    for (const std::string_view name : {"test:missing", "", "stone"}) {
        const auto table = BiomeTable::LoadWithRegistry(
            "biomes.json", registry, BiomeReader(UniformSurface(name)));
        EXPECT_FALSE(table.ok());
        EXPECT_TRUE(table.empty());
        EXPECT_EQ(table.errors().size(), 4u);
        EXPECT_EQ(table.content_hash(), 0u);
    }
    for (const std::string_view document : {"", "{", "{}", "{\"biomes\":[0]}"}) {
        const auto table = BiomeTable::LoadWithRegistry(
            "biomes.json", registry, [document](const std::filesystem::path&, std::string& bytes) {
                bytes = document;
                return true;
            });
        EXPECT_FALSE(table.ok());
        EXPECT_TRUE(table.empty());
        EXPECT_FALSE(table.errors().empty());
    }
    const auto unreadable = BiomeTable::LoadWithRegistry(
        "biomes.json", registry, [](const std::filesystem::path&, std::string&) { return false; });
    EXPECT_FALSE(unreadable.ok());
    EXPECT_TRUE(unreadable.empty());
    EXPECT_EQ(unreadable.errors().size(), 1u);
    const auto empty = BiomeTable::LoadWithRegistry(
        "biomes.json", registry, [](const std::filesystem::path&, std::string& bytes) {
            bytes = "{\"biomes\":[]}";
            return true;
        });
    EXPECT_TRUE(empty.ok());
    EXPECT_TRUE(empty.empty());
}
