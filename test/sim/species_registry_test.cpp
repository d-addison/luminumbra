//  data-driven SpeciesRegistry — species are JSON data (genome ranges /
// annual-perennial / lifespan / render archetype); the engine stays generic. Loader + deterministic
// genome sampling from a species' per-gene ranges.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "luminumbra_common/components/CropLifecycleComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/core/DeterministicRng.h"
#include "luminumbra_common/foliage/SpeciesRegistry.h"
#include "luminumbra_common/systems/FarmingSystem.h" // MakePlantFromSpecies

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
        DeterministicRng rng =
            DeterministicRng::seeded(F::SpeciesGeneIndex("GrowthRate") + 1, 7, 3);
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
    EXPECT_FALSE(wheat->perennial); // annual crop
    EXPECT_TRUE(oak->perennial);    // perennial tree
    EXPECT_GT(oak->lifespan_ticks, wheat->lifespan_ticks);
}

// Content: the shipped crops span SEASONAL niches so different crops thrive in different seasons
// (the genome ideal_temp + the annual env-temperature swing make this emergent). maize/sungourd
// lean WARM (HeatTolerance > ColdTolerance), frostberry/moonpetal lean COOL. Guards the data.
TEST(SpeciesRegistry, ShippedCropsSpanSeasonalNiches) {
    F::SpeciesRegistry reg;
    std::vector<std::string> errors;
    const std::filesystem::path dir =
        std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "data" / "common" / "foliage" / "species";
    const std::size_t n = reg.LoadFromDirectory(dir, errors);
    EXPECT_GE(n, 6u) << (errors.empty() ? "" : errors.front()); // oak, wheat + the 4 new crops
    using G = C::PlantGene;
    auto leansWarm = [](const F::SpeciesTemplate* t) {
        const auto h = static_cast<std::size_t>(G::HeatTolerance);
        const auto c = static_cast<std::size_t>(G::ColdTolerance);
        return (t->gene_lo[h] + t->gene_hi[h]) > (t->gene_lo[c] + t->gene_hi[c]);
    };
    for (const char* warm : {"maize", "sungourd"}) {
        const auto* t = reg.Find(warm);
        ASSERT_NE(t, nullptr) << warm << " crop missing";
        EXPECT_TRUE(leansWarm(t)) << warm << " should be a warm-season crop";
    }
    for (const char* cool : {"frostberry", "moonpetal"}) {
        const auto* t = reg.Find(cool);
        ASSERT_NE(t, nullptr) << cool << " crop missing";
        EXPECT_FALSE(leansWarm(t)) << cool << " should be a cool-season crop";
    }
}

//   wiring: MakePlantFromSpecies is the single bridge from a data-driven species
// template to a live PlantTag entity — it samples the genome, plants the seed, and stamps the crop
// lifecycle, deterministically.
TEST(SpeciesRegistry, MakePlantFromSpeciesSpawnsAndStampsLifecycle) {
    F::SpeciesRegistry reg;
    std::string err;
    ASSERT_TRUE(reg.AddFromJsonText(kWheat, err)) << err;
    const auto* w = reg.Find("wheat");
    ASSERT_NE(w, nullptr);

    const std::uint16_t sid = F::SpeciesId16("wheat");
    EXPECT_NE(sid, 0) << "0 is the unspecified sentinel; a real species must not collide with it";
    EXPECT_EQ(sid, F::SpeciesId16("wheat")) << "SpeciesId16 is stable for a given id";
    EXPECT_NE(sid, F::SpeciesId16("oak")) << "distinct ids hash distinctly (no collision here)";

    auto spawn = [&] {
        entt::registry r;
        DeterministicRng rng = DeterministicRng::seeded(101, 5, 9);
        const entt::entity e =
            F::MakePlantFromSpecies(r, ::Luminumbra::Vec3{1.0f, 2.0f, 3.0f}, *w, rng, 42);
        return std::make_pair(std::move(r), e);
    };

    auto [r1, e1] = spawn();
    ASSERT_TRUE(e1 != entt::null);
    ASSERT_TRUE(r1.all_of<C::PlantTag>(e1));
    ASSERT_TRUE(r1.all_of<C::PlantGrowthComponent>(e1));
    ASSERT_TRUE(r1.all_of<C::PlantGenomeComponent>(e1));
    ASSERT_TRUE(r1.all_of<C::CropLifecycleComponent>(e1));
    ASSERT_TRUE(r1.all_of<C::TransformComponent>(e1));

    const auto& g1 = r1.get<C::PlantGrowthComponent>(e1);
    EXPECT_EQ(g1.species_id, sid);
    EXPECT_EQ(g1.planted_tick, 42u);
    const auto& cl1 = r1.get<C::CropLifecycleComponent>(e1);
    EXPECT_FALSE(cl1.perennial);         // wheat is annual
    EXPECT_EQ(cl1.lifespan_ticks, 900u); // from the template
    EXPECT_EQ(cl1.species_id, sid);
    const auto& tf1 = r1.get<C::TransformComponent>(e1);
    EXPECT_FLOAT_EQ(tf1.position.x, 1.0f);
    EXPECT_FLOAT_EQ(tf1.position.y, 2.0f);
    EXPECT_FLOAT_EQ(tf1.position.z, 3.0f);

    // Determinism: same seed -> identical sampled genome (run==replay).
    auto [r2, e2] = spawn();
    const auto& gen1 = r1.get<C::PlantGenomeComponent>(e1);
    const auto& gen2 = r2.get<C::PlantGenomeComponent>(e2);
    EXPECT_EQ(gen1.genes, gen2.genes);
}

//  the player-facing FarmingController loop (seed -> tend -> harvest), all on
// the deterministic FarmingSystem verbs. Client-agnostic, so unit-testable here.
TEST(SpeciesRegistry, FarmingControllerLoop) {
    namespace C = ::Luminumbra::Components;
    F::SpeciesRegistry reg;
    std::string err;
    ASSERT_TRUE(reg.AddFromJsonText(kWheat, err)) << err;
    const auto* w = reg.Find("wheat");
    ASSERT_NE(w, nullptr);

    entt::registry r;
    DeterministicRng rng = DeterministicRng::seeded(7, 1, 1);
    F::FarmingController fc;
    EXPECT_EQ(fc.seeds, 5);

    // Seed consumes a seed + spawns a PlantTag plant.
    const entt::entity e = fc.Seed(r, ::Luminumbra::Vec3{0.0f, 0.0f, 0.0f}, *w, rng, 10);
    ASSERT_TRUE(e != entt::null);
    EXPECT_EQ(fc.seeds, 4);
    EXPECT_TRUE(r.all_of<C::PlantTag>(e));

    // NearestPlant: found within reach, null outside.
    EXPECT_TRUE(F::FarmingController::NearestPlant(r, ::Luminumbra::Vec3{1.0f, 0.0f, 1.0f}, 5.0f) ==
                e);
    EXPECT_TRUE(F::FarmingController::NearestPlant(
                    r, ::Luminumbra::Vec3{100.0f, 0.0f, 100.0f}, 5.0f) == entt::null);

    // Water raises the tended husbandry bonus.
    const auto tended0 = r.get<C::PlantGrowthComponent>(e).tended;
    EXPECT_TRUE(fc.Water(r, e));
    EXPECT_GT(r.get<C::PlantGrowthComponent>(e).tended, tended0);

    // Harvesting an immature plant fails; no inventory change.
    EXPECT_FALSE(fc.Harvest(r, e).harvestable);
    EXPECT_EQ(fc.harvests, 0);

    // Force mature + quality, then harvest -> banks yield + returned seeds, removes the annual
    // plant.
    r.get<C::PlantGrowthComponent>(e).stage = static_cast<std::uint8_t>(C::PlantStage::Fruiting);
    r.get<C::PlantGrowthComponent>(e).quality = 80;
    const int seedsBefore = fc.seeds;
    const auto hr = fc.Harvest(r, e);
    EXPECT_TRUE(hr.harvestable);
    EXPECT_EQ(fc.harvests, 1);
    EXPECT_GT(fc.total_yield, 0.0f);
    EXPECT_GE(fc.seeds, seedsBefore); // harvest returns seeds
    EXPECT_FALSE(r.valid(e));         // annual plant removed
}

//  perennial reset: harvesting a PERENNIAL plant regrows it (reset to vegetative + generation
// bump) instead of destroying it; an annual is consumed (covered in FarmingControllerLoop).
TEST(SpeciesRegistry, FarmingHarvestPerennialRegrows) {
    namespace C = ::Luminumbra::Components;
    entt::registry r;
    F::FarmingController fc;
    const entt::entity e = r.create();
    r.emplace<C::PlantTag>(e);
    r.emplace<C::PlantGenomeComponent>(e);
    auto& g = r.emplace<C::PlantGrowthComponent>(e);
    g.stage = static_cast<std::uint8_t>(C::PlantStage::Fruiting);
    g.quality = 70;
    auto& cl = r.emplace<C::CropLifecycleComponent>(e);
    cl.perennial = true;

    const auto hr = fc.Harvest(r, e);
    EXPECT_TRUE(hr.harvestable);
    EXPECT_TRUE(r.valid(e)) << "perennial regrows, not destroyed";
    EXPECT_EQ(r.get<C::PlantGrowthComponent>(e).stage,
              static_cast<std::uint8_t>(C::PlantStage::Sprout));
    EXPECT_EQ(r.get<C::CropLifecycleComponent>(e).generations, 1);
    EXPECT_EQ(fc.harvests, 1);
}

} // namespace
