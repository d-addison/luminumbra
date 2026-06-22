// I9-FOLIAGE Phase 5A: data-driven SpeciesRegistry — species are JSON data (genome ranges /
// annual-perennial / lifespan / render archetype); the engine stays generic. Loader + deterministic
// genome sampling from a species' per-gene ranges.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/core/DeterministicRng.h"
#include "luminumbra_common/foliage/SpeciesRegistry.h"

namespace {
namespace F = luminumbra::foliage;
namespace C = ::Luminumbra::Components;
using luminumbra::core::DeterministicRng;

const char* kWheat = R"({
  "id": "wheat", "render_archetype": "grass_crop", "perennial": false, "lifespan_ticks": 900,
  "genes": { "GrowthRate": [0.6, 0.9], "MaxScale": [0.15, 0.35], "Yield": [0.55, 0.90] }
})";

TEST(SpeciesRegistry, ParsesFieldsAndGeneRanges) {
    F::SpeciesRegistry reg;
    std::string err;
    ASSERT_TRUE(reg.AddFromJsonText(kWheat, err)) << err;
    const auto* w = reg.Find("wheat");
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->render_archetype, "grass_crop");
    EXPECT_FALSE(w->perennial);
    EXPECT_EQ(w->lifespan_ticks, 900u);
    const std::size_t gr = static_cast<std::size_t>(C::PlantGene::GrowthRate);
    EXPECT_FLOAT_EQ(w->gene_lo[gr], 0.6f);
    EXPECT_FLOAT_EQ(w->gene_hi[gr], 0.9f);
    // An unspecified gene defaults to the full [0,1] range.
    const std::size_t cold = static_cast<std::size_t>(C::PlantGene::ColdTolerance);
    EXPECT_FLOAT_EQ(w->gene_lo[cold], 0.0f);
    EXPECT_FLOAT_EQ(w->gene_hi[cold], 1.0f);
}

TEST(SpeciesRegistry, MissingIdIsRejected) {
    F::SpeciesRegistry reg;
    std::string err;
    EXPECT_FALSE(reg.AddFromJsonText(R"({"perennial": true})", err));
    EXPECT_FALSE(err.empty());
}

TEST(SpeciesRegistry, SampleGenomeStaysInRangeAndIsDeterministic) {
    F::SpeciesRegistry reg;
    std::string err;
    ASSERT_TRUE(reg.AddFromJsonText(kWheat, err)) << err;
    const auto* w = reg.Find("wheat");
    ASSERT_NE(w, nullptr);

    auto sample = [&] {
        DeterministicRng rng = DeterministicRng::seeded(F::SpeciesGeneIndex("GrowthRate") + 1, 7, 3);
        return F::SpeciesRegistry::SampleGenome(*w, rng);
    };
    const auto a = sample();
    const auto b = sample();
    EXPECT_EQ(a.genes, b.genes) << "same seed -> identical genome (run==replay)";
    // Every gene within its declared [lo,hi].
    for (std::size_t i = 0; i < a.genes.size(); ++i) {
        EXPECT_GE(a.genes[i], w->gene_lo[i]);
        EXPECT_LE(a.genes[i], w->gene_hi[i]);
    }
}

TEST(SpeciesRegistry, LoadsShippedSpeciesFromDirectory) {
    F::SpeciesRegistry reg;
    std::vector<std::string> errors;
    const std::filesystem::path dir =
        std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "data" / "common" / "foliage" / "species";
    const std::size_t n = reg.LoadFromDirectory(dir, errors);
    ASSERT_GE(n, 2u) << (errors.empty() ? "" : errors.front());
    const auto* wheat = reg.Find("wheat");
    const auto* oak = reg.Find("oak");
    ASSERT_NE(wheat, nullptr);
    ASSERT_NE(oak, nullptr);
    EXPECT_FALSE(wheat->perennial);              // annual crop
    EXPECT_TRUE(oak->perennial);                 // perennial tree
    EXPECT_GT(oak->lifespan_ticks, wheat->lifespan_ticks);
}

}  // namespace
