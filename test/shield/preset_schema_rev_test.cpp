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

bool Accepts(std::int64_t revision, const char* provenance) {
    const auto result =
        Luminumbra::world::LoadTerrainPresetFromJson(ValidPreset(revision), {}, provenance);
    return result.ok && result.errors.empty();
}

} // namespace

int main() {
    using Luminumbra::world::kMinTerrainPresetSchemaRevision;
    using Luminumbra::world::kTerrainPresetSchemaRevision;
    using Luminumbra::world::LoadTerrainPresetFromJson;

    // The revision presets are authored against today.
    if (!Accepts(kTerrainPresetSchemaRevision, "current")) {
        std::cerr << "current preset schema revision was rejected\n";
        return 1;
    }

    if (kTerrainPresetSchemaRevision != 6 || kMinTerrainPresetSchemaRevision != 6) {
        std::cerr << "the sole explicit preset revision must be 6\n";
        return 1;
    }

    // Retirement rejects every formerly supported explicit revision, including
    // the previously shipped archipelago revision and the previous current revision.
    for (std::int64_t revision : {-1, 0, 1, 2, 3, 4, 5}) {
        const auto rejected = LoadTerrainPresetFromJson(ValidPreset(revision), {}, "too-old");
        if (rejected.ok || !Contains(rejected.errors, "found " + std::to_string(revision))) {
            std::cerr << "a retired preset revision was not rejected\n";
            return 1;
        }
    }

    // Above the current revision: a typo, or content authored against a future schema.
    // This is the case the guard exists for - before it, any integer was accepted.
    {
        constexpr std::int64_t kTooNew = kTerrainPresetSchemaRevision + 1;
        const auto rejected = LoadTerrainPresetFromJson(ValidPreset(kTooNew), {}, "too-new");
        if (rejected.ok || !Contains(rejected.errors, "found " + std::to_string(kTooNew))) {
            std::cerr << "a revision above the current schema was not rejected\n";
            return 1;
        }
    }

    // A missing key is accepted: schema_rev is validated when declared, not required.
    // Requiring it would be a new contract on every in-memory fixture.
    {
        nlohmann::json missing = ValidPreset(kTerrainPresetSchemaRevision);
        missing.erase("schema_rev");
        const auto accepted = LoadTerrainPresetFromJson(missing, {}, "missing");
        if (!accepted.ok || !accepted.errors.empty()) {
            std::cerr << "a preset without schema_rev should still load\n";
            return 1;
        }
    }

    // A non-integer schema_rev is rejected.
    {
        nlohmann::json bad = ValidPreset(kTerrainPresetSchemaRevision);
        bad["schema_rev"] = "five";
        const auto rejected = LoadTerrainPresetFromJson(bad, {}, "non-integer");
        if (rejected.ok || !Contains(rejected.errors, "must be an integer")) {
            std::cerr << "a non-integer schema_rev was not rejected\n";
            return 1;
        }
    }

    return 0;
}
