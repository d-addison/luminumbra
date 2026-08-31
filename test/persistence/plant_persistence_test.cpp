//  plant persistence projection — a planted field serializes to the engine's
// deterministic EntityRegistrySnapshot and reloads byte-exact (genome + growth/soil/disease/
// pollination/lifecycle sim truth). Empty roster -> empty snapshot (no-plant save byte-identical).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CropLifecycleComponents.h"
#include "luminumbra_common/components/DiseaseComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/components/PollinationComponents.h"
#include "luminumbra_common/components/SoilComponents.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/WorldSaveService.h"

#include <filesystem>

namespace {

namespace C = ::Luminumbra::Components;
namespace F = luminumbra::foliage;
namespace E = ::Luminumbra::Ecs;

// A plant with just the required components (tag + genome + growth + transform).
entt::entity barePlant(entt::registry& r, float x, std::uint8_t stage, std::uint32_t growth) {
    auto e = r.create();
    r.emplace<C::PlantTag>(e);
    r.emplace<C::TransformComponent>(e).position = Luminumbra::Vec3(x, 1.5f, -x);
    auto& gn = r.emplace<C::PlantGenomeComponent>(e);
    for (std::size_t i = 0; i < gn.genes.size(); ++i)
        gn.genes[i] = 0.1f + 0.1f * static_cast<float>((i + static_cast<std::size_t>(x)) % 8);
    auto& g = r.emplace<C::PlantGrowthComponent>(e);
    g.species_id = static_cast<std::uint16_t>(x);
    g.stage = stage;
    g.quality = 73;
    g.growth_points = growth;
    g.stress_points = growth / 4u;
    g.tended = 12;
    g.planted_tick = 100;
    g.last_tick = 100 + growth;
    return e;
}

TEST(PlantPersistence, EmptyRosterIsEmptySnapshot) {
    entt::registry r;
    EXPECT_TRUE(F::BuildPlantEntitySnapshot(r).entities.empty());
}

TEST(PlantPersistence, RoundtripPreservesAllPlantState) {
    entt::registry a;
    // plant 0: minimal. plant 1: + soil + health. plant 2: + pollination + lifecycle.
    barePlant(a, 0.0f, static_cast<std::uint8_t>(C::PlantStage::Seed), 500);

    const auto p1 = barePlant(a, 1.0f, static_cast<std::uint8_t>(C::PlantStage::Mature), 12000);
    a.emplace<C::SoilFeederComponent>(p1, C::SoilFeederComponent{800, 4200});
    auto& h1 = a.emplace<C::PlantHealthComponent>(p1);
    h1.infection = 640;
    h1.resistance = 120;
    h1.state = static_cast<std::uint8_t>(C::PlantDiseaseState::Infected);
    h1.infected_ticks = 33;

    const auto p2 = barePlant(a, 2.0f, static_cast<std::uint8_t>(C::PlantStage::Fruiting), 18500);
    auto& pc2 = a.emplace<C::PollinationComponent>(p2);
    pc2.pollinated = true;
    pc2.last_pollen_tick = 900;
    pc2.crosses = 4;
    for (std::size_t i = 0; i < pc2.next_genome.genes.size(); ++i)
        pc2.next_genome.genes[i] = 0.05f * static_cast<float>(i + 1);
    auto& lc2 = a.emplace<C::CropLifecycleComponent>(p2);
    lc2.perennial = true;
    lc2.ripe_ticks = 77;
    lc2.lifespan_ticks = 1500;
    lc2.generations = 3;
    lc2.species_id = 2;

    // Build -> serialize -> load -> apply into a FRESH registry -> re-serialize must be byte-exact.
    const std::string json = E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(a));
    E::EntityRegistrySnapshot loaded;
    std::vector<std::string> errs;
    ASSERT_TRUE(E::LoadEntityRegistrySnapshotJson(json, loaded, errs))
        << (errs.empty() ? "" : errs.front());

    entt::registry b;
    F::ApplyPlantEntitySnapshot(b, loaded);
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(b)), json)
        << "planted field must reload byte-exact";

    // Spot-check that reconstructed component values match (not just the serialized bytes).
    EXPECT_EQ(b.view<C::PlantTag>().size(), 3u);
    bool sawInfected = false, sawPerennial = false;
    for (auto e : b.view<C::PlantTag>()) {
        if (auto* h = b.try_get<C::PlantHealthComponent>(e); h && h->infection == 640) {
            sawInfected = true;
            EXPECT_EQ(h->infected_ticks, 33u);
        }
        if (auto* lc = b.try_get<C::CropLifecycleComponent>(e); lc && lc->perennial) {
            sawPerennial = true;
            EXPECT_EQ(lc->generations, 3u);
        }
    }
    EXPECT_TRUE(sawInfected) << "blighted plant's health survived the roundtrip";
    EXPECT_TRUE(sawPerennial) << "perennial lifecycle survived the roundtrip";
}

TEST(PlantPersistence, GenesSurviveFullPrecision) {
    entt::registry a;
    auto e = barePlant(a, 0.0f, 0, 0);
    auto& gn = a.get<C::PlantGenomeComponent>(e);
    gn.genes = {0.123456791f,
                0.987654328f,
                0.333333343f,
                0.0009765625f,
                0.5f,
                0.7071067691f,
                0.0f,
                0.999999940f};
    const auto saved = gn.genes;

    const std::string json = E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(a));
    E::EntityRegistrySnapshot loaded;
    std::vector<std::string> errs;
    ASSERT_TRUE(E::LoadEntityRegistrySnapshotJson(json, loaded, errs));
    entt::registry b;
    F::ApplyPlantEntitySnapshot(b, loaded);
    ASSERT_EQ(b.view<C::PlantTag>().size(), 1u);
    for (auto pe : b.view<C::PlantTag>())
        EXPECT_EQ(b.get<C::PlantGenomeComponent>(pe).genes, saved) << "genes survive bit-exact";
}

//  disk path: WorldSaveService writes/reads plant-entities.json byte-exact.
TEST(PlantPersistence, DiskSaveLoadByteExact) {
    namespace fs = std::filesystem;
    namespace SS = ::Luminumbra::Persistence;
    entt::registry a;
    barePlant(a, 0.0f, static_cast<std::uint8_t>(C::PlantStage::Seed), 500);
    const auto p1 = barePlant(a, 1.0f, static_cast<std::uint8_t>(C::PlantStage::Mature), 12000);
    a.emplace<C::SoilFeederComponent>(p1, C::SoilFeederComponent{800, 4200});
    const auto snap = F::BuildPlantEntitySnapshot(a);
    const std::string json = E::SerializeEntityRegistrySnapshotJson(snap);

    const fs::path dir = fs::temp_directory_path() / "lumin_plant_persist_disk";
    fs::remove_all(dir);
    std::vector<std::string> errs;
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(snap, dir, &errs))
        << (errs.empty() ? "" : errs.front());
    E::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, dir, &errs));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), json) << "disk save->load byte-exact";
    fs::remove_all(dir);
}

// A no-plant save writes NO file and loads as a clean (empty) miss -> byte-identical no-plant save.
TEST(PlantPersistence, EmptyRosterWritesNoFile) {
    namespace fs = std::filesystem;
    namespace SS = ::Luminumbra::Persistence;
    const fs::path dir = fs::temp_directory_path() / "lumin_plant_persist_empty";
    fs::remove_all(dir);
    std::vector<std::string> errs;
    E::EntityRegistrySnapshot empty;
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(empty, dir, &errs));
    EXPECT_FALSE(fs::exists(SS::WorldSaveService::plant_entities_path(dir)))
        << "no-plant save writes no plant-entities.json";
    E::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, dir, &errs)); // clean miss
    EXPECT_TRUE(loaded.entities.empty());
    fs::remove_all(dir);
}

} // namespace
