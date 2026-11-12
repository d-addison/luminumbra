// Independent planned-contract oracle. This TU deliberately uses no production
// codec/projection field list: later mutation tests must cover this inventory.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>

namespace {

std::string ReadContract(const char* relative_path) {
    std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / relative_path);
    EXPECT_TRUE(input.is_open()) << relative_path;
    // Contract documents are bounded to 128 KiB; reaching the bound is a failure.
    std::string contents(128U * 1024U, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    EXPECT_TRUE(input.eof()) << relative_path;
    EXPECT_FALSE(input.bad()) << relative_path;
    contents.resize(static_cast<std::size_t>(input.gcount()));
    std::erase(contents, '\r');
    return contents;
}

TEST(SimulationIdentityContract, PreparationRemainsPlannedAcrossGuides) {
    for (const char* path : {"docs/distant-world.md",
                             "docs/active-regions.md",
                             "docs/development/system-config-identity.md"}) {
        const auto document = ReadContract(path);
        EXPECT_NE(document.find("PLANNED for v0.3.3 (in preparation), issue #629"),
                  std::string::npos)
            << path;
        EXPECT_NE(document.find("luminumbra.plants.v2"), std::string::npos) << path;
        EXPECT_NE(document.find("kAuthorityRulesRevision"), std::string::npos) << path;
        EXPECT_NE(document.find("issue #137"), std::string::npos) << path;
    }
}

TEST(SimulationIdentityContract, SharedCursorRefusalAndSessionScopeAreExplicit) {
    const auto document = ReadContract("docs/distant-world.md");
    for (const std::string_view requirement :
         {"IDs are in `[1, 2^63)`, next cursor in `[1, 2^63]`",
          "issued IDs are never recycled, including after extinction",
          "render-only dressing consumes no IDs",
          "including an empty roster with the consumed shared cursor",
          "Existing saves with v1, a sidecar, an unknown version or no v2 envelope refuse",
          "no converter exists",
          "any saved ledger allocator TLV must equal the envelope cursor",
          "at most `2^20` plant records and 17 component kinds per record",
          "mixed owned/unowned plants are valid",
          "retaining the greater live/saved cursor",
          "session-only until issue #137",
          "wildlife recreated after reopening is a new incarnation with new IDs",
          "then-current authority revision plus one",
          "TCP on Windows/POSIX, loopback, Steam, GNS and replay"}) {
        EXPECT_NE(document.find(requirement), std::string::npos) << requirement;
    }
}

TEST(SimulationIdentityContract, IndependentPlantFieldInventoryIsComplete) {
    constexpr std::string_view expected = R"(| Component | Fields required by codec and projection |
|---|---|
| `TransformComponent` | `position.x`, `position.y`, `position.z` |
| `PlantGenomeComponent` | `genes[0..7]` in `PlantGene` order |
| `PlantGrowthComponent` | `species_id`, `stage`, `quality`, `growth_points`, `stress_points`, `tended`, `planted_tick`, `last_tick` |
| `SoilFeederComponent` | `uptake`, `absorbed` |
| `PlantHealthComponent` | `infection`, `resistance`, `state`, `infected_ticks` |
| `PollinationComponent` | `pollinated`, `last_pollen_tick`, `crosses`, `next_genome.genes[0..7]` |
| `CropLifecycleComponent` | `perennial`, `ripe_ticks`, `lifespan_ticks`, `generations`, `species_id` |
| `PlantRegionComponent` | `owner_x`, `owner_z`, `region_stamp`; identity moves to the common record field |
| `PollinationTag` | presence only |
| `CombustibleComponent` | `fuel_milli`, `moisture_milli`, `burn_state`, `burn_ticks_remaining`, `ignition_radius` |
| `MortalComponent` | `age_ticks`, `lifespan_ticks`, `dead` |
| `GrazeableComponent` | `biomass`, `last_grazed_tick` |
| `WaterSourceComponent` | `output` |
| `IgnitionSourceComponent` | `radius`, `intensity` |
| `FlowingWaterSourceComponent` | `flow_rate` |
| `DecayComponent` | `decay_ticks`, `decay_duration`, `nutrient_released_milli`, `fully_decomposed` |
| `FieldEmitterComponent` | `channel`, `rate_raw_per_tick`, `radius_cells` |)";
    const auto document = ReadContract("docs/distant-world.md");
    const auto start = document.find("| Component | Fields required by codec and projection |");
    ASSERT_NE(start, std::string::npos);
    const auto end = document.find("\n\n### Planned ecology projection inventory", start);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(std::string_view(document).substr(start, end - start), expected);
}

TEST(SimulationIdentityContract, EcologyProjectionAndPersistenceBoundaryAreExplicit) {
    const auto document = ReadContract("docs/distant-world.md");
    for (const std::string_view requirement :
         {"PLANNED `ecology:v3`",
          "this hash inventory does not implement creature persistence",
          "Creature and genome",
          "Mortality, decay and needs",
          "Social and movement state",
          "Perception and planning",
          "Sources and participation",
          "References and exclusions",
          "distinct null/invalid markers",
          "independent completeness oracle",
          "a separate mutation regression for every field and component presence"}) {
        EXPECT_NE(document.find(requirement), std::string::npos) << requirement;
    }
}

} // namespace
