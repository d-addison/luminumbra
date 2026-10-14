#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Luminumbra::TestSupport {

// Small required inventories for enabled headless fixtures, without presentation assets.
inline void WriteSimulationContentFixture(const std::filesystem::path& root) {
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
