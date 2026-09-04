#include "../../src/luminumbra_common/world/TerrainPresetLoader.h"

#include <iostream>
#include <string>

#include "nlohmann/json.hpp"

namespace {

nlohmann::json ValidPreset(std::int64_t schema_revision) {
    return {{"schema_rev", schema_revision},
            {"generation_params",
             {{"terrain",
               {{"base_frequency", 0.01},
                {"base_amplitude", 32.0},
                {"octaves", 4},
                {"persistence", 0.5},
                {"lacunarity", 2.0},
                {"height_offset", 64.0}}},
              {"features", {{"caves_enabled", true}, {"cave_frequency", 0.02}}}}}};
}

bool Contains(const std::vector<std::string>& messages, const std::string& text) {
    for (const std::string& message : messages) {
        if (message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    using Luminumbra::world::kTerrainPresetSchemaRevision;
    using Luminumbra::world::LoadTerrainPresetFromJson;

    const auto accepted =
        LoadTerrainPresetFromJson(ValidPreset(kTerrainPresetSchemaRevision), {}, "accepted");
    if (!accepted.ok || !accepted.errors.empty()) {
        std::cerr << "current preset schema revision was rejected\n";
        return 1;
    }

    constexpr std::int64_t kRejectedRevision = kTerrainPresetSchemaRevision - 1;
    const auto rejected = LoadTerrainPresetFromJson(ValidPreset(kRejectedRevision), {}, "rejected");
    if (rejected.ok ||
        !Contains(rejected.errors, "expected " + std::to_string(kTerrainPresetSchemaRevision)) ||
        !Contains(rejected.errors, "found " + std::to_string(kRejectedRevision))) {
        std::cerr << "mismatched preset schema revision was not rejected with expected and found "
                     "revisions\n";
        return 1;
    }

    return 0;
}
