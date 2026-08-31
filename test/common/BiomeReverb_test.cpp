// per-biome environmental-audio reverb flow gate.
// Validates that the shipped biomes.json reverb params are CONSUMED (parsed
// into BiomeTable) and that reverb_for(active biome id) returns the authored
// profile - the data->engine flow the EnvironmentalAudioSystem drives.
#include "gtest/gtest.h"

#include "world/BiomeTable.h"

#include <filesystem>

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
