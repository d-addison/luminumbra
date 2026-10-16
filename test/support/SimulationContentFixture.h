#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Luminumbra::TestSupport {

// Small required inventories and readable optional-policy parent for enabled headless fixtures.
inline void WriteSimulationContentFixture(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "data/common/sim");
    std::ofstream biomes(root / "data/common/biomes.json", std::ios::binary | std::ios::trunc);
    biomes
        << R"({"schema":"luminumbra.biome_table.v1","biomes":[{"id":0,"name":"fixture_biome","surface":{"top":"Grass","filler":"Soil","depth":"Stone","underwater":"Sand"}}]})"
        << '\n';
    biomes.close();
    if (!biomes.good())
        throw std::runtime_error("Could not write simulation biome fixture.");
    for (const auto* domain : {"creatures", "foliage"}) {
        const auto directory = root / "data/common" / domain / "species";
        std::filesystem::create_directories(directory);
        const auto* name =
            std::string_view(domain) == "creatures" ? "fixture_creature" : "fixture_plant";
        std::ofstream output(directory / (std::string(name) + ".json"),
                             std::ios::binary | std::ios::trunc);
        output << "{\"id\":\"" << name << "\"}\n";
        output.close();
        if (!output.good())
            throw std::runtime_error("Could not write simulation content fixture.");
    }
}

} // namespace Luminumbra::TestSupport
