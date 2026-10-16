// per-biome environmental-audio reverb flow gate.
// Validates that the shipped biomes.json reverb params are CONSUMED (parsed
// into BiomeTable) and that reverb_for(active biome id) returns the authored
// profile - the data->engine flow the EnvironmentalAudioSystem drives.
#include "gtest/gtest.h"

#include "world/BiomeTable.h"

#include "../support/UniqueTempPath.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

namespace {

namespace fs = std::filesystem;
using namespace Luminumbra::World;

fs::path BiomesJson() {
    return fs::path(LUMINUMBRA_SOURCE_ROOT) / "data" / "common" / "biomes.json";
}

} // namespace

TEST(BiomeReverbTest, RawNulRefusesWholeTableAndCleanBytesLoadIdentically) {
    std::ifstream source(BiomesJson(), std::ios::binary);
    ASSERT_TRUE(source.is_open());
    std::ostringstream buffer;
    buffer << source.rdbuf();
    ASSERT_FALSE(source.bad());
    const std::string bytes = buffer.str();
    const auto expected = BiomeTable::Load(BiomesJson());
    ASSERT_TRUE(expected.ok());
    const Luminumbra::TestSupport::ScopedTempFile file("biome_bytes_", ".json");
    const auto write = [&](const std::string& content) {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << content;
        output.close();
        ASSERT_TRUE(output.good());
    };
    write(bytes);
    const auto clean = BiomeTable::Load(file.Path());
    ASSERT_TRUE(clean.ok());
    EXPECT_EQ(clean.errors(), expected.errors());
    EXPECT_EQ(clean.warnings(), expected.warnings());
    EXPECT_EQ(clean.content_hash(), expected.content_hash());
    ASSERT_EQ(clean.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& actual = clean.biomes()[i];
        const auto& original = expected.biomes()[i];
        EXPECT_EQ(actual.id, original.id);
        EXPECT_EQ(actual.name, original.name);
        for (const auto member : {&BiomeDefinition::continentalness,
                                  &BiomeDefinition::erosion,
                                  &BiomeDefinition::peaks_valleys,
                                  &BiomeDefinition::temperature,
                                  &BiomeDefinition::humidity}) {
            EXPECT_FLOAT_EQ((actual.*member).min, (original.*member).min);
            EXPECT_FLOAT_EQ((actual.*member).max, (original.*member).max);
        }
        EXPECT_EQ(actual.palette.top, original.palette.top);
        EXPECT_EQ(actual.palette.filler, original.palette.filler);
        EXPECT_EQ(actual.palette.depth, original.palette.depth);
        EXPECT_EQ(actual.palette.underwater, original.palette.underwater);
        EXPECT_EQ(actual.reverb.preset, original.reverb.preset);
        EXPECT_FLOAT_EQ(actual.reverb.wet, original.reverb.wet);
        EXPECT_FLOAT_EQ(actual.reverb.dry, original.reverb.dry);
        EXPECT_FLOAT_EQ(actual.reverb.decay, original.reverb.decay);
        EXPECT_FLOAT_EQ(actual.vegetation.density, original.vegetation.density);
        EXPECT_EQ(actual.vegetation.scatter, original.vegetation.scatter);
    }
    for (const auto& invalid :
         {bytes + '\0',
          bytes + '\0' + "garbage",
          bytes + '\x1a' + '\0' + "garbage",
          '\0' + bytes,
          bytes.substr(0, bytes.size() / 2) + '\0' + bytes.substr(bytes.size() / 2)}) {
        write(invalid);
        const auto refused = BiomeTable::Load(file.Path());
        EXPECT_FALSE(refused.ok());
        EXPECT_TRUE(refused.empty());
        EXPECT_EQ(refused.content_hash(), 0u);
        ASSERT_EQ(refused.errors().size(), 1u);
        EXPECT_NE(refused.errors().front().find("raw NUL"), std::string::npos);
        EXPECT_TRUE(refused.warnings().empty());
    }
    write(bytes);
    const auto restored = BiomeTable::Load(file.Path());
    ASSERT_TRUE(restored.ok());
    EXPECT_EQ(restored.content_hash(), clean.content_hash());
    EXPECT_EQ(restored.size(), clean.size());
}

TEST(BiomeReverbTest, ShippedTableParsesPerBiomeReverb) {
    BiomeTable table = BiomeTable::Load(BiomesJson());
    ASSERT_TRUE(table.ok()) << (table.errors().empty() ? "" : table.errors().front());
    ASSERT_FALSE(table.empty());

    // Every authored biome must carry a reverb profile with sane numeric ranges
    // and a non-empty preset label.
    for (const BiomeDefinition& biome : table.biomes()) {
        const BiomeReverb& reverb = table.reverb_for(biome.id);
        EXPECT_FALSE(reverb.preset.empty()) << "biome '" << biome.name << "' reverb preset empty";
        EXPECT_GE(reverb.wet, 0.0f);
        EXPECT_LE(reverb.wet, 1.0f);
        EXPECT_GE(reverb.dry, 0.0f);
        EXPECT_LE(reverb.dry, 1.0f);
        EXPECT_GE(reverb.decay, 0.0f);
    }
}

TEST(BiomeReverbTest, ActiveBiomeReverbDiffersAcrossBiomes) {
    BiomeTable table = BiomeTable::Load(BiomesJson());
    ASSERT_TRUE(table.ok());
    ASSERT_GE(table.size(), 2u);

    // The authored table varies reverb wetness between biomes (e.g. the open
    // mountain/arid biomes are drier than the muffled basin). So at least two
    // biomes must yield distinct reverb wet values - proof the per-biome reverb
    // is data-driven, not a single global constant.
    bool found_distinct = false;
    const float first_wet = table.reverb_for(table.biomes().front().id).wet;
    for (const BiomeDefinition& biome : table.biomes()) {
        if (table.reverb_for(biome.id).wet != first_wet) {
            found_distinct = true;
            break;
        }
    }
    EXPECT_TRUE(found_distinct) << "per-biome reverb is constant across all biomes";
}

TEST(BiomeReverbTest, UnknownBiomeReturnsDefaultReverb) {
    BiomeTable table = BiomeTable::Load(BiomesJson());
    ASSERT_TRUE(table.ok());
    // kNoBiome and any out-of-range id resolve to the default profile (so a
    // caller never needs a separate guard).
    const BiomeReverb& none = table.reverb_for(kNoBiome);
    EXPECT_FALSE(none.preset.empty());
    EXPECT_GE(none.wet, 0.0f);
    EXPECT_LE(none.wet, 1.0f);
}
