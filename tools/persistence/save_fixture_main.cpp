// luminumbra_save_fixture: writes a deterministic synthetic saved world (no worldgen).
//
// Usage: luminumbra_save_fixture --out <root> [--chunks N] [--seed S] [--edits K]
//                                [--payload noise|flat] [--world-id ID] [--preset <path>]
//                                [--profile historical]
//
// Exit codes: 0 success (one JSON line on stdout), 1 write failure, 2 usage error.

#include "luminumbra_common/persistence/SyntheticSaveFixture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <type_traits>

namespace {
namespace Persistence = Luminumbra::Persistence;

constexpr int kUsageExit = 2;
constexpr int kFailureExit = 1;
constexpr std::array<const char*, 8> kFlags = {
    "--out", "--chunks", "--seed", "--edits", "--payload", "--world-id", "--preset", "--profile"};

void PrintUsage() {
    std::cerr << "usage: luminumbra_save_fixture --out <root> [--chunks N] [--seed S]\n"
                 "       [--edits K] [--payload noise|flat] [--world-id ID] [--preset <path>]\n"
                 "       [--profile historical]\n";
}

std::optional<std::uint64_t> ParseUnsigned(const std::string& text) {
    try {
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
            return std::nullopt;
        std::size_t used = 0;
        const auto value = std::stoull(text, &used);
        if (used != text.size())
            return std::nullopt;
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> values;
    for (int i = 1; i < argc; i += 2) {
        const std::string flag = argv[i];
        if (std::find(kFlags.begin(), kFlags.end(), flag) == kFlags.end()) {
            std::cerr << "unknown flag: " << flag << "\n";
            PrintUsage();
            return kUsageExit;
        }
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << flag << "\n";
            PrintUsage();
            return kUsageExit;
        }
        values[flag] = argv[i + 1];
    }

    const auto profile = values.find("--profile");
    if (profile != values.end() && profile->second != "historical") {
        std::cerr << "unknown profile: " << profile->second << "\n";
        PrintUsage();
        return kUsageExit;
    }

    Persistence::SyntheticSaveSpec spec;
    if (profile != values.end()) {
        // Historical defaults; explicit flags below override them.
        spec.chunk_count = 5433;
        spec.payload = Persistence::FixturePayload::Noise;
        spec.edit_count = 0;
        spec.seed = 1337;
    }
    const auto out = values.find("--out");
    if (out == values.end()) {
        std::cerr << "--out is required\n";
        PrintUsage();
        return kUsageExit;
    }
    // Parses an optional numeric flag into target; false means a usage error was printed.
    auto number = [&values](const char* flag, auto& target) -> bool {
        const auto it = values.find(flag);
        if (it == values.end())
            return true;
        const auto parsed = ParseUnsigned(it->second);
        const bool is_seed = std::string(flag) == "--seed";
        if (!parsed || (!is_seed && *parsed > UINT32_MAX)) {
            std::cerr << "invalid value for " << flag << ": " << it->second << "\n";
            PrintUsage();
            return false;
        }
        target = static_cast<std::remove_reference_t<decltype(target)>>(*parsed);
        return true;
    };
    if (!number("--chunks", spec.chunk_count) || !number("--seed", spec.seed) ||
        !number("--edits", spec.edit_count)) {
        return kUsageExit;
    }
    if (const auto it = values.find("--payload"); it != values.end()) {
        if (it->second == "noise") {
            spec.payload = Persistence::FixturePayload::Noise;
        } else if (it->second == "flat") {
            spec.payload = Persistence::FixturePayload::Flat;
        } else {
            std::cerr << "invalid value for --payload: " << it->second << "\n";
            PrintUsage();
            return kUsageExit;
        }
    }
    if (const auto it = values.find("--world-id"); it != values.end())
        spec.world_id = it->second;
    const auto preset = values.find("--preset");
    if (preset == values.end()) {
        std::cerr << "--preset is required: without an embedded preset the world is not "
                     "catalog-valid\n";
        PrintUsage();
        return kUsageExit;
    }
    spec.preset_source = preset->second;

    const auto report = Persistence::WriteSyntheticSave(spec, out->second);
    if (!report.ok) {
        std::cerr << "save fixture failed: " << report.error << "\n";
        return kFailureExit;
    }
    const nlohmann::json line = {
        {"schema", "luminumbra.save_fixture_report.v1"},
        {"world_id", spec.world_id},
        {"save_dir", report.save_dir.generic_string()},
        {"seed", spec.seed},
        {"chunks", report.chunks_written},
        {"edits", spec.edit_count},
        {"payload", spec.payload == Persistence::FixturePayload::Noise ? "noise" : "flat"},
        {"region_files", report.region_files},
        {"region_bytes", report.region_bytes},
    };
    std::cout << line.dump() << "\n";
    return 0;
}
