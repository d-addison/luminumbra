//  plant persistence projection — a planted field serializes to the engine's
// deterministic EntityRegistrySnapshot and reloads byte-exact (genome + growth/soil/disease/
// pollination/lifecycle sim truth). Empty roster -> empty snapshot (no-plant save byte-identical).
#include <gtest/gtest.h>

#include <iterator>
#include <limits>
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
#include "luminumbra_common/world/WorldStreamingState.h"

#include "support/UniqueTempPath.h"
#include <filesystem>
#include <fstream>
#include <ios>
#include <system_error>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

#if !defined(_WIN32)
TEST(PlantPersistence, EmptyRosterRemovalFlushesParentEvenAfterFailedFlush) {
    namespace SS = ::Luminumbra::Persistence;
    const auto directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("plant-unlink-");
    struct Cleanup {
        const std::filesystem::path& path;
        ~Cleanup() {
            SS::WorldSaveService::set_directory_flush_failure_for_testing(-1);
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    const auto path = SS::WorldSaveService::plant_entities_path(directory);
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    ASSERT_TRUE(
        SS::WorldSaveService::save_plant_entities(F::BuildPlantEntitySnapshot(plants), directory));
    const Luminumbra::Ecs::EntityRegistrySnapshot empty;
    for (int attempt = 0; attempt < 2; ++attempt) {
        SS::WorldSaveService::set_directory_flush_failure_for_testing(0);
        std::vector<std::string> errors;
        EXPECT_FALSE(SS::WorldSaveService::save_plant_entities(empty, directory, &errors));
        EXPECT_FALSE(std::filesystem::exists(path));
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_EQ(errors.front(),
                  "directory flush failed (test seam): " + path.parent_path().string());
    }
    EXPECT_TRUE(SS::WorldSaveService::save_plant_entities(empty, directory));
}
#endif

TEST(PlantPersistence, InvalidLastPlantLeavesRegistryAndEntitySequenceUnchanged) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 500);
    barePlant(source, 2.0f, 3, 600);
    const auto original = F::BuildPlantEntitySnapshot(source);
    const std::vector<nlohmann::json> invalid_numbers = {1e38, -1, 256, 1.5, "3", nullptr, true};
    for (const auto& invalid : invalid_numbers) {
        SCOPED_TRACE(invalid.dump());
        auto snapshot = original;
        for (auto& component : snapshot.entities.back().components) {
            if (component.type == "PlantGrowth")
                component.data["stage"] = invalid;
        }
        entt::registry target;
        const auto existing = barePlant(target, 4.0f, 3, 700);
        const auto before =
            E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)),
                  before);
        EXPECT_EQ(entt::to_integral(target.create()), entt::to_integral(existing) + 1u);
    }
}

TEST(PlantPersistence, EveryNumericAndBooleanProjectionIsValidatedBeforeMutation) {
    entt::registry source;
    const auto plant = barePlant(source, 1.0f, 3, 500);
    source.emplace<C::SoilFeederComponent>(plant);
    source.emplace<C::PlantHealthComponent>(plant);
    source.emplace<C::PollinationComponent>(plant);
    source.emplace<C::CropLifecycleComponent>(plant);
    const auto original = F::BuildPlantEntitySnapshot(source);
    for (std::size_t component_index = 0;
         component_index < original.entities.front().components.size();
         ++component_index) {
        const auto& data = original.entities.front().components[component_index].data;
        for (auto field = data.begin(); field != data.end(); ++field) {
            for (const bool missing : {false, true}) {
                SCOPED_TRACE(original.entities.front().components[component_index].type);
                SCOPED_TRACE(field.key());
                SCOPED_TRACE(missing);
                auto snapshot = original;
                auto& changed = snapshot.entities.front().components[component_index].data;
                if (missing)
                    changed.erase(field.key());
                else if (field.value().is_array())
                    changed[field.key()] = nlohmann::json::array({"invalid"});
                else
                    changed[field.key()] = "invalid";
                entt::registry target;
                EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
                EXPECT_TRUE(target.view<C::PlantTag>().empty());
                EXPECT_EQ(entt::to_integral(target.create()), 0u);
            }
        }
    }
    for (const auto& type : {"Transform",
                             "PlantGenome",
                             "PlantGrowth",
                             "SoilFeeder",
                             "PlantHealth",
                             "Pollination",
                             "CropLifecycle"}) {
        auto snapshot = original;
        const auto& components = original.entities.front().components;
        const auto found = std::find_if(
            components.begin(), components.end(), [&](const auto& c) { return c.type == type; });
        ASSERT_NE(found, components.end());
        snapshot.entities.front().components.push_back(*found);
        entt::registry target;
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_EQ(entt::to_integral(target.create()), 0u);
    }
}

TEST(PlantPersistence, NumericRangeChecksPrecedeEveryProjection) {
    entt::registry source;
    const auto plant = barePlant(source, 1.0f, 3, 500);
    source.emplace<C::SoilFeederComponent>(plant);
    source.emplace<C::PlantHealthComponent>(plant);
    source.emplace<C::PollinationComponent>(plant);
    source.emplace<C::CropLifecycleComponent>(plant);
    const auto original = F::BuildPlantEntitySnapshot(source);
    for (std::size_t i = 0; i < original.entities.front().components.size(); ++i) {
        const auto& component = original.entities.front().components[i];
        for (auto field = component.data.begin(); field != component.data.end(); ++field) {
            if (field->is_boolean())
                continue;
            const std::vector<nlohmann::json> invalid =
                field->is_array()
                    ? std::vector<nlohmann::json>{nlohmann::json::array({1e38 * 10.0}),
                                                  nlohmann::json::array({nullptr})}
                : field->is_number_float()
                    ? std::vector<nlohmann::json>{1e38 * 10.0,
                                                  std::numeric_limits<double>::infinity()}
                    : std::vector<nlohmann::json>{
                          -1, 1e38, std::numeric_limits<std::uint64_t>::max()};
            for (const auto& value : invalid) {
                // uint64 max is a valid boundary for the tick fields, checked separately.
                if (value.is_number_unsigned() &&
                    (field.key() == "planted_tick" || field.key() == "last_tick" ||
                     field.key() == "last_pollen_tick"))
                    continue;
                SCOPED_TRACE(component.type + "." + field.key());
                auto snapshot = original;
                snapshot.entities.front().components[i].data[field.key()] = value;
                entt::registry target;
                EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
                EXPECT_EQ(entt::to_integral(target.create()), 0u);
            }
        }
    }
}

TEST(PlantPersistence, DiskRefusalsLeaveOutputAndExistingFilesUnchanged) {
    namespace P = ::Luminumbra::Persistence;
    const auto dir = Luminumbra::TestSupport::CreateUniqueTempDirectory("plant-admission-");
    struct Cleanup {
        const std::filesystem::path& path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{dir};
    entt::registry source;
    barePlant(source, 1.0f, 3, 500);
    const auto original = F::BuildPlantEntitySnapshot(source);
    ASSERT_TRUE(P::WorldSaveService::save_plant_entities(original, dir));
    auto invalid = original;
    for (auto& component : invalid.entities.front().components) {
        if (component.type == "PlantGrowth")
            component.data["stage"] = 1e38;
    }
    std::vector<std::string> errors;
    EXPECT_FALSE(P::WorldSaveService::save_plant_entities(invalid, dir, &errors));
    EXPECT_FALSE(errors.empty());
    E::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(P::WorldSaveService::load_plant_entities(loaded, dir));
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    const auto path = P::WorldSaveService::plant_entities_path(dir);
    const auto invalid_bytes = E::SerializeEntityRegistrySnapshotJson(invalid);
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(path, invalid_bytes));
    EXPECT_FALSE(P::WorldSaveService::load_plant_entities(loaded, dir, &errors));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    std::ifstream input(path, std::ios::binary);
    const std::string disk_bytes{std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()};
    EXPECT_EQ(disk_bytes, invalid_bytes);
}

TEST(PlantPersistence, ComponentDomainsAreCheckedBeforeAnyPlantIsAdopted) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 500);
    const auto last = barePlant(source, 2.0f, 3, 600);
    source.emplace<C::PlantHealthComponent>(last);
    source.emplace<C::PollinationComponent>(last);
    const auto original = F::BuildPlantEntitySnapshot(source);
    struct InvalidField {
        std::string type;
        std::string field;
        nlohmann::json value;
    };
    const std::vector<InvalidField> invalid = {
        {"PlantGrowth", "stage", C::kPlantStageCount},
        {"PlantGrowth", "stage", 255},
        {"PlantGrowth", "quality", 101},
        {"PlantHealth", "state", C::kPlantDiseaseStateCount},
        {"PlantHealth", "infection", C::kDiseaseUnitScale + 1u},
        {"PlantHealth", "resistance", C::kDiseaseUnitScale + 1u}};
    const auto expect_refused = [](const E::EntityRegistrySnapshot& snapshot) {
        entt::registry target;
        const auto existing = barePlant(target, 4.0f, 3, 700);
        const auto before =
            E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target));
        EXPECT_FALSE(F::ValidatePlantEntitySnapshot(snapshot));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)),
                  before);
        EXPECT_EQ(entt::to_integral(target.create()), entt::to_integral(existing) + 1u);
    };
    for (const auto& field : invalid) {
        SCOPED_TRACE(field.type + "." + field.field);
        auto snapshot = original;
        for (auto& component : snapshot.entities.back().components) {
            if (component.type == field.type)
                component.data[field.field] = field.value;
        }
        expect_refused(snapshot);
    }
    // Check every adopted gene in both the current and pending pollination genome.
    // The tiny excursions would round back into [0,1] if checked after float conversion.
    for (const auto& type : {"PlantGenome", "Pollination"}) {
        for (std::size_t gene = 0; gene < C::kPlantGeneCount; ++gene) {
            for (const double value : {-0.01, 1.01, 1e20, -1e-50, 1.0 + 1e-12}) {
                SCOPED_TRACE(type);
                SCOPED_TRACE(gene);
                SCOPED_TRACE(value);
                auto snapshot = original;
                for (auto& component : snapshot.entities.back().components) {
                    if (component.type == type)
                        component
                            .data[component.type == "PlantGenome" ? "genes" : "next_genome"][gene] =
                            value;
                }
                expect_refused(snapshot);
            }
        }
    }
}

TEST(PlantPersistence, EveryPlantRequiresAllCoreComponentsBeforeRegistryMutation) {
    entt::registry source;
    barePlant(source, 1.0f, 3, 500);
    barePlant(source, 2.0f, 3, 600);
    const auto original = F::BuildPlantEntitySnapshot(source);
    for (unsigned mask = 0; mask < 7; ++mask) {
        SCOPED_TRACE(mask);
        auto snapshot = original;
        auto& components = snapshot.entities.back().components;
        std::erase_if(components, [mask](const auto& component) {
            return (component.type == "Transform" && (mask & 1u) == 0) ||
                   (component.type == "PlantGenome" && (mask & 2u) == 0) ||
                   (component.type == "PlantGrowth" && (mask & 4u) == 0);
        });
        entt::registry target;
        const auto existing = barePlant(target, 4.0f, 3, 700);
        const auto before =
            E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target));
        EXPECT_FALSE(F::ValidatePlantEntitySnapshot(snapshot));
        EXPECT_FALSE(F::ApplyPlantEntitySnapshot(target, snapshot));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(F::BuildPlantEntitySnapshot(target)),
                  before);
        EXPECT_EQ(entt::to_integral(target.create()), entt::to_integral(existing) + 1u);
    }
}

TEST(PlantPersistence, RepresentableMaximaAndHistoricalPartialGenomesStillLoad) {
    entt::registry source;
    const auto plant = barePlant(source, 1.0f, 3, 500);
    source.emplace<C::SoilFeederComponent>(plant);
    source.emplace<C::PlantHealthComponent>(plant);
    source.emplace<C::PollinationComponent>(plant);
    source.emplace<C::CropLifecycleComponent>(plant);
    auto snapshot = F::BuildPlantEntitySnapshot(source);
    for (auto& component : snapshot.entities.front().components) {
        if (component.type == "Transform")
            component.data["x"] = std::numeric_limits<float>::max();
        if (component.type == "PlantGenome")
            component.data["genes"] = nlohmann::json::array({0.25f});
        if (component.type == "PlantGrowth") {
            component.data["stage"] = C::kPlantStageCount - 1u;
            component.data["quality"] = 100;
            component.data["species_id"] = std::numeric_limits<std::uint16_t>::max();
            component.data["growth_points"] = std::numeric_limits<std::uint32_t>::max();
            component.data["last_tick"] = std::numeric_limits<std::uint64_t>::max();
        }
        if (component.type == "PlantHealth") {
            component.data["infection"] = C::kDiseaseUnitScale;
            component.data["resistance"] = C::kDiseaseUnitScale;
            component.data["state"] = C::kPlantDiseaseStateCount - 1u;
        }
        if (component.type == "Pollination")
            component.data["next_genome"] = nlohmann::json::array({0.0, 1.0});
    }
    snapshot.entities.front().components.push_back({"FuturePlantExtension", {{"opaque", true}}});
    snapshot.entities.front().components.push_back({"FuturePlantExtension", nullptr});
    entt::registry target;
    ASSERT_TRUE(F::ApplyPlantEntitySnapshot(target, snapshot));
    const auto restored = *target.view<C::PlantTag>().begin();
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(restored).last_tick,
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(target.get<C::PlantGenomeComponent>(restored).genes[0], 0.25f);
    EXPECT_EQ(target.get<C::PlantGenomeComponent>(restored).genes[1], 0.0f);
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(restored).stage, C::kPlantStageCount - 1u);
    EXPECT_EQ(target.get<C::PlantGrowthComponent>(restored).quality, 100);
    EXPECT_EQ(target.get<C::PlantHealthComponent>(restored).infection, C::kDiseaseUnitScale);
    EXPECT_EQ(target.get<C::PlantHealthComponent>(restored).resistance, C::kDiseaseUnitScale);
    EXPECT_EQ(target.get<C::PlantHealthComponent>(restored).state, C::kPlantDiseaseStateCount - 1u);
    EXPECT_EQ(target.get<C::PollinationComponent>(restored).next_genome.genes[0], 0.0f);
    EXPECT_EQ(target.get<C::PollinationComponent>(restored).next_genome.genes[1], 1.0f);
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

namespace {
class PlantSaveRecovery : public testing::Test {
private:
    std::filesystem::path m_directory;

protected:
    const std::filesystem::path& Directory() const {
        return m_directory;
    }
    void SetUp() override {
        m_directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("plant_save_recovery_");
    }
    void TearDown() override {
        Luminumbra::Persistence::WorldSaveService::set_interrupt_before_region_replace_for_testing(
            false);
        std::error_code error;
        std::filesystem::remove_all(m_directory, error);
    }
};
} // namespace

TEST_F(PlantSaveRecovery, AbandonedStagingDoesNotBlockLoadsAndIsReclaimedOnEverySave) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto snapshot = F::BuildPlantEntitySnapshot(plants);
    const E::EntityRegistrySnapshot empty;
    const auto abandoned = Directory() / "plant-entities.json.tmp.abandoned";
    const auto unrelated = Directory() / "unrelated.tmp.abandoned";
    for (const bool existing_roster : {false, true}) {
        for (const bool empty_roster : {false, true}) {
            SCOPED_TRACE(existing_roster);
            SCOPED_TRACE(empty_roster);
            ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(
                existing_roster ? snapshot : empty, Directory()));
            ASSERT_TRUE(SS::WorldSaveService::write_file_durably(abandoned, "{partial roster"));
            ASSERT_TRUE(SS::WorldSaveService::write_file_durably(unrelated, "keep"));
            E::EntityRegistrySnapshot loaded;
            ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory()));
            EXPECT_EQ(loaded.entities.empty(), !existing_roster);
            Luminumbra::WorldStreamingState chunks;
            std::vector<std::string> errors;
            SS::WorldSaveService{}.load_world(chunks, Directory(), errors);
            EXPECT_TRUE(errors.empty());
            ASSERT_TRUE(SS::WorldSaveService::validate_save(Directory()));
            ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(empty_roster ? empty : snapshot,
                                                                  Directory()));
            EXPECT_FALSE(fs::exists(abandoned));
            EXPECT_TRUE(fs::exists(unrelated));
            ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory()));
            EXPECT_EQ(loaded.entities.empty(), empty_roster);
        }
    }
}

TEST_F(PlantSaveRecovery, InterruptedReplacementPreservesRosterAndRetryLeavesNoRegionDebris) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    entt::registry plants;
    const auto plant = barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(original, Directory()));
    plants.get<C::PlantGrowthComponent>(plant).growth_points = 42;
    const auto changed = F::BuildPlantEntitySnapshot(plants);
    SS::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool saved = SS::WorldSaveService::save_plant_entities(changed, Directory());
    SS::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(saved);
    E::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory()));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded),
              E::SerializeEntityRegistrySnapshotJson(original));
    ASSERT_TRUE(SS::WorldSaveService::validate_save(Directory()));
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(changed, Directory()));
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory()));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded),
              E::SerializeEntityRegistrySnapshotJson(changed));
    for (const auto& entry : fs::recursive_directory_iterator(Directory()))
        EXPECT_EQ(entry.path().filename().string().find(".tmp."), std::string::npos);
    const auto unknown = SS::WorldSaveService::region_directory(Directory()) / "unknown.tmp";
    ASSERT_TRUE(SS::WorldSaveService::write_file_durably(unknown, "unknown artifact"));
    EXPECT_FALSE(SS::WorldSaveService::validate_save(Directory()));
}

TEST_F(PlantSaveRecovery, MissingIsCleanButInvalidPathsAndPayloadsPreserveTheOutput) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    auto loaded = original;
    std::vector<std::string> errors;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
    EXPECT_TRUE(loaded.entities.empty());
    EXPECT_TRUE(errors.empty());
    loaded = original;
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(
        loaded, Directory() / std::string(512, 'x'), &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    const auto path = SS::WorldSaveService::plant_entities_path(Directory());
    fs::create_directories(path);
    errors.clear();
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    fs::remove(path);
    for (const auto& bytes :
         {std::string{}, std::string("{"), expected.substr(0, expected.size() / 2)}) {
        SCOPED_TRACE(bytes);
        ASSERT_TRUE(SS::WorldSaveService::write_file_durably(path, bytes));
        errors.clear();
        EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    }
}

#if !defined(_WIN32)
TEST_F(PlantSaveRecovery, SymlinksIncludingDeviceAndDanglingTargetsAreRefusedBeforeReading) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    const auto path = SS::WorldSaveService::plant_entities_path(Directory());
    fs::create_directories(path.parent_path());
    const auto valid_target = Directory() / "valid-roster.json";
    ASSERT_TRUE(SS::WorldSaveService::write_file_durably(valid_target, expected));
    for (const auto& target : {valid_target, Directory() / "absent", fs::path("/dev/zero")}) {
        SCOPED_TRACE(target);
        fs::create_symlink(target, path);
        auto loaded = original;
        std::vector<std::string> errors;
        ASSERT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
        Luminumbra::WorldStreamingState chunks;
        errors.clear();
        EXPECT_FALSE(SS::WorldSaveService{}.load_world(chunks, Directory(), errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_TRUE(fs::is_symlink(path));
        fs::remove(path);
    }
}

TEST_F(PlantSaveRecovery, FifoWithValidPayloadIsRefusedBeforeOpeningAStream) {
    namespace SS = ::Luminumbra::Persistence;
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto original = F::BuildPlantEntitySnapshot(plants);
    const auto expected = E::SerializeEntityRegistrySnapshotJson(original);
    const auto path = SS::WorldSaveService::plant_entities_path(Directory());
    std::filesystem::create_directories(path.parent_path());
    ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
    // Keep a nonblocking reader alive so a finite valid payload survives the
    // writer's close. Even the unsafe stream reader reaches EOF without hanging.
    const int keeper = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
    ASSERT_GE(keeper, 0);
    struct CloseReader {
        int descriptor;
        ~CloseReader() {
            ::close(descriptor);
        }
    } close_reader{keeper};
    const int writer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
    ASSERT_GE(writer, 0);
    const auto written = ::write(writer, expected.data(), expected.size());
    const int closed = ::close(writer);
    ASSERT_EQ(written, static_cast<ssize_t>(expected.size()));
    ASSERT_EQ(closed, 0);
    auto loaded = original;
    std::vector<std::string> errors;
    EXPECT_FALSE(SS::WorldSaveService::load_plant_entities(loaded, Directory(), &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded), expected);
    Luminumbra::WorldStreamingState chunks;
    errors.clear();
    EXPECT_FALSE(SS::WorldSaveService{}.load_world(chunks, Directory(), errors));
    EXPECT_FALSE(errors.empty());
}
#endif

TEST(PlantPersistence, EmptyRosterRemovalErrorIsReportedAndCanRetry) {
    namespace SS = ::Luminumbra::Persistence;
    namespace fs = std::filesystem;
    const auto dir = Luminumbra::TestSupport::CreateUniqueTempDirectory("plant_remove_error_");
    const auto path = SS::WorldSaveService::plant_entities_path(dir);
    entt::registry plants;
    barePlant(plants, 1.0f, 0, 5);
    const auto snapshot = F::BuildPlantEntitySnapshot(plants);
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(snapshot, dir));
    ASSERT_TRUE(fs::is_regular_file(path));
#ifdef _WIN32
    // MSVC remove can delete read-only files. A CRT reader denies delete sharing
    // while allowing the validation reader to inspect the existing valid roster.
    std::ifstream held_roster(path, std::ios::binary);
    ASSERT_TRUE(held_roster);
#else
    const auto file_permissions = fs::status(path).permissions();
    const auto parent_permissions = fs::status(path.parent_path()).permissions();
    // POSIX checks the parent, while validation can still read the valid roster.
    fs::permissions(path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    fs::permissions(path.parent_path(),
                    fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read |
                        fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec);
#endif
    const E::EntityRegistrySnapshot empty;
    std::vector<std::string> errors;
    const bool removed = SS::WorldSaveService::save_plant_entities(empty, dir, &errors);
#ifdef _WIN32
    held_roster.close();
    EXPECT_FALSE(held_roster.fail());
#else
    fs::permissions(path.parent_path(), parent_permissions);
    if (fs::exists(path))
        fs::permissions(path, file_permissions);
#endif
    EXPECT_FALSE(removed);
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("failed to remove plant entities file"), std::string::npos);
    EXPECT_TRUE(fs::exists(path));
    E::EntityRegistrySnapshot retained;
    ASSERT_TRUE(SS::WorldSaveService::load_plant_entities(retained, dir));
    EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(retained),
              E::SerializeEntityRegistrySnapshotJson(snapshot));
    errors.clear();
    ASSERT_TRUE(SS::WorldSaveService::save_plant_entities(empty, dir, &errors));
    EXPECT_FALSE(fs::exists(path));
    // Invalid occupied directories must also fail without deleting their contents.
    fs::create_directories(path);
    {
        std::ofstream keep(path / "keep");
        keep << "occupied";
    }
    EXPECT_FALSE(SS::WorldSaveService::save_plant_entities(empty, dir, &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_TRUE(fs::exists(path / "keep"));
    fs::remove_all(path);
    errors.clear();
    EXPECT_TRUE(SS::WorldSaveService::save_plant_entities(empty, dir, &errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_FALSE(fs::exists(path));
    fs::remove_all(dir);
}
