// CPU-only discovery. stdout is reserved for the complete canonical document.
#include "StationDiscoveryArguments.h"

#include <array>
#include <atomic>
#include <bit>
#include <cfenv>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <FastSIMD/FastSIMD.h>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#include "core/BoundedConsoleSink.h"
#include "core/FilesystemPath.h"
#include "core/Log.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/BiomeTable.h"
#include "world/StationDiscovery.h"
#include "world/StructurePlacement.h"
#include "world/TerrainPresetLoader.h"

namespace {
using nlohmann::json;
using namespace Luminumbra::world;
namespace fs = std::filesystem;
using Luminumbra::Filesystem::GenericUtf8PathString;
using Luminumbra::Filesystem::PathFromUtf8;
using Luminumbra::StationDiscoveryCli::ArgumentChar;
using Luminumbra::StationDiscoveryCli::ArgumentPath;
using Luminumbra::StationDiscoveryCli::ArgumentText;
using Luminumbra::StationDiscoveryCli::UsageError;

class WarningSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    bool Observed() const {
        return m_observed.load();
    }

protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        if (message.level >= spdlog::level::warn) {
            m_observed.store(true);
        }
    }
    void flush_() override {}

private:
    std::atomic_bool m_observed{false};
};

struct Options {
    fs::path preset, data_root;
    int seed = 0;
    std::string kind;
    StationQuery query;
};

template<typename T>
T Integer(const ArgumentChar* text) {
    const auto value = ArgumentText(text);
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec == std::errc::invalid_argument || parsed.ptr != value.data() + value.size()) {
        throw UsageError("expected an integer");
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        throw std::runtime_error("integer outside supported range");
    }
    return result;
}
Options ParseOptions(int argc, ArgumentChar** argv) {
    Options options;
    const std::map<std::string, int> arities{{"--preset-json", 1},
                                             {"--data-root", 1},
                                             {"--seed", 1},
                                             {"--kind", 1},
                                             {"--origin-mm", 2},
                                             {"--radius-m", 1},
                                             {"--y-range-m", 2},
                                             {"--view-distance-m", 1},
                                             {"--limit", 1}};
    std::set<std::string> seen;
    for (int i = 1; i < argc;) {
        const auto key = ArgumentText(argv[i++]);
        const auto found = arities.find(key);
        if (found == arities.end() || !seen.insert(key).second) {
            throw UsageError("unknown or duplicate argument: " + key);
        }
        if (argc - i < found->second) {
            throw UsageError("missing argument value: " + key);
        }
        if (key == "--preset-json") {
            options.preset = ArgumentPath(argv[i]);
        } else if (key == "--data-root") {
            options.data_root = ArgumentPath(argv[i]);
        } else if (key == "--seed") {
            options.seed = Integer<int>(argv[i]);
        } else if (key == "--kind") {
            options.kind = ArgumentText(argv[i]);
        } else if (key == "--origin-mm") {
            options.query.origin_mm = {Integer<std::int64_t>(argv[i]),
                                       Integer<std::int64_t>(argv[i + 1])};
        } else if (key == "--radius-m") {
            options.query.radius_m = Integer<int>(argv[i]);
        } else if (key == "--y-range-m") {
            options.query.y_range_m = {Integer<int>(argv[i]), Integer<int>(argv[i + 1])};
        } else if (key == "--view-distance-m") {
            options.query.view_distance_m = Integer<int>(argv[i]);
        } else if (key == "--limit") {
            options.query.limit = Integer<int>(argv[i]);
        }
        i += found->second;
    }
    if (seen.size() != arities.size() || options.preset.empty() || options.data_root.empty()) {
        throw UsageError("all discovery arguments are required");
    }
    if (options.kind == "mouth") {
        options.query.kind = StationKind::Mouth;
    } else if (options.kind == "arch") {
        options.query.kind = StationKind::Arch;
    } else if (options.kind == "horizon") {
        options.query.kind = StationKind::Horizon;
    } else if (options.kind == "coverage") {
        options.query.kind = StationKind::Coverage;
    } else {
        throw UsageError("kind must be mouth, arch, horizon or coverage");
    }
    // Numeric bounds are input refusals (exit 1), syntactic mistakes are exit 2.
    if (options.seed < 0 || options.seed > 2147483615) {
        throw std::runtime_error("seed outside supported range");
    }
    const auto error = ValidateStationQuery(options.query);
    if (!error.empty()) {
        throw std::runtime_error(error);
    }
    return options;
}

std::string ReadBytes(const fs::path& path) {
    if (!fs::is_regular_file(fs::symlink_status(path))) {
        throw std::runtime_error("input is missing, nonregular or a symlink: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open input: " + path.string());
    }
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    if (input.bad()) {
        throw std::runtime_error("cannot read input: " + path.string());
    }
    return bytes;
}
void CheckNumbers(const json& value) {
    if (value.is_number()) {
        const double number = value.get<double>();
        if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
            throw std::runtime_error("nonfinite or unsupported input number");
        }
    }
    if (value.is_structured()) {
        for (const auto& child : value) {
            CheckNumbers(child);
        }
    }
}
json StrictJson(const std::string& bytes) {
    std::vector<std::set<std::string>> keys;
    auto callback = [&keys](int depth, json::parse_event_t event, json& parsed) {
        if (depth > 128) {
            throw std::runtime_error("input JSON nesting exceeds 128");
        }
        if (event == json::parse_event_t::object_start) {
            keys.emplace_back();
        } else if (event == json::parse_event_t::key) {
            if (!keys.back().insert(parsed.get<std::string>()).second) {
                throw std::runtime_error("duplicate JSON field");
            }
        } else if (event == json::parse_event_t::object_end) {
            keys.pop_back();
        }
        return true;
    };
    json value = json::parse(bytes, callback);
    CheckNumbers(value);
    return value;
}

void CheckIntegerField(const json& object,
                       const char* key,
                       double minimum = std::numeric_limits<int>::min(),
                       double maximum = std::numeric_limits<int>::max()) {
    if (object.contains(key) &&
        (!object.at(key).is_number_integer() || object.at(key).get<double>() < minimum ||
         object.at(key).get<double>() > maximum)) {
        throw std::runtime_error(std::string("integer input outside bounds: ") + key);
    }
}
void CheckPresetShape(const json& preset) {
    CheckIntegerField(preset, "schema_rev");
    const auto& generation = preset.at("generation_params");
    const auto& terrain = generation.at("terrain");
    CheckIntegerField(terrain, "octaves", 1, 32);
    for (const auto* key : {"shaping", "hydro"}) {
        if (terrain.contains(key) && !terrain.at(key).is_object()) {
            throw std::runtime_error("optional terrain block must be an object");
        }
    }
    if (generation.contains("biomes") && !generation.at("biomes").is_object()) {
        throw std::runtime_error("biomes must be an object");
    }
    if (generation.contains("biomes") && generation.at("biomes").contains("table")) {
        const auto table = PathFromUtf8(generation.at("biomes").at("table").get<std::string>());
        if (table.is_absolute() || table.has_root_name()) {
            throw std::runtime_error("biome table must name a relative dependency");
        }
    }
    if (terrain.contains("hydro")) {
        // The public sampler builds a halo per iteration; unbounded counts can
        // overflow allocation arithmetic before a discovery sample completes.
        CheckIntegerField(terrain.at("hydro"), "iterations", 0, 512);
    }
    if (terrain.contains("shaping")) {
        const auto& shaping = terrain.at("shaping");
        for (const auto* key : {"continental_spline", "erosion_spline", "peaks_spline"}) {
            if (!shaping.contains(key)) {
                continue;
            }
            const auto& spline = shaping.at(key);
            if (!spline.is_array()) {
                throw std::runtime_error("spline must be an array");
            }
            for (const auto& pair : spline) {
                if (!pair.is_array() || pair.size() != 2 || !pair[0].is_number() ||
                    !pair[1].is_number()) {
                    throw std::runtime_error("invalid spline pair refused");
                }
            }
        }
    }
}

void CheckHydroDomain(const Options& options, const Luminumbra::Systems::TerrainGenParams& params) {
    if (!params.hydro_enabled) {
        return;
    }
    // Keep every binary32 world/cell quotient within +/-2^62, reserving
    // headroom for bilinear neighbours, region origins, halos and cache deltas.
    constexpr float minimum_cell_size = 0x1p-39f; // 2^23 metres / 2^62 cells
    const float cs = params.hydro_cell_size_m;
    if (!std::isfinite(cs) || cs < minimum_cell_size || params.hydro_iterations < 0 ||
        params.hydro_iterations > 512) {
        throw std::runtime_error("hydro cell size or iterations outside supported domain");
    }
    for (float value : {params.hydro_talus_height,
                        params.hydro_rain_per_sweep,
                        params.hydro_solubility,
                        params.hydro_sediment_capacity,
                        params.hydro_max_offset}) {
        if (!std::isfinite(value) || value < 0) {
            throw std::runtime_error("hydro parameter must be finite and nonnegative");
        }
    }
    for (float value :
         {params.hydro_thermal_rate, params.hydro_deposition, params.hydro_evaporation}) {
        if (!std::isfinite(value) || value < 0 || value > 1) {
            throw std::runtime_error("hydro rate outside finite unit interval");
        }
    }
    // Mirror SampleHydroOffsetMeters' 64-cell regions and iterations+8 halo.
    // Validate all query/pose/stencil samples AND the no-hydro bake positions
    // before constructing the world. Those positions feed the terrain samplers.
    constexpr std::int64_t region_cells = 64;
    const std::int64_t halo = params.hydro_iterations + 8;
    constexpr double bound = static_cast<double>(kStationHorizontalBoundMm) / 1000;
    const auto& query = options.query;
    const bool feature = query.kind == StationKind::Mouth || query.kind == StationKind::Arch;
    const double reach = query.radius_m + (feature ? query.view_distance_m + 8 : 0) + 0.5;
    const auto region_origin = [](std::int64_t cell) {
        return (cell / region_cells - (cell % region_cells < 0 ? 1 : 0)) * region_cells;
    };
    for (auto origin_mm : query.origin_mm) {
        const double origin = static_cast<double>(origin_mm) / 1000;
        const float lower = static_cast<float>(origin - reach) / cs;
        const float upper = static_cast<float>(origin + reach) / cs;
        constexpr double cell_bound = 0x1p62;
        if (!std::isfinite(lower) || !std::isfinite(upper) || lower < -cell_bound ||
            upper > cell_bound) {
            throw std::runtime_error("hydro query outside cell coordinate domain");
        }
        const auto first = static_cast<std::int64_t>(std::floor(lower));
        const auto last = static_cast<std::int64_t>(std::floor(upper));
        const auto padded_first = region_origin(first) - halo;
        const auto padded_last = region_origin(last + 1) + region_cells - 1 + halo;
        const float first_m = static_cast<float>(padded_first) * cs;
        const float last_m = static_cast<float>(padded_last) * cs;
        if (!std::isfinite(first_m) || !std::isfinite(last_m) || first_m < -bound ||
            last_m > bound) {
            throw std::runtime_error("hydro region and halo outside horizontal domain");
        }
    }
}

void CheckStructureVector(const json& object, const char* key, int minimum) {
    if (!object.contains(key)) {
        return;
    }
    const auto& values = object.at(key);
    if (!values.is_array() || values.size() != 3) {
        throw std::runtime_error("structure integer vector must have three components");
    }
    for (const auto& value : values) {
        if (!value.is_number_integer() || value.get<double>() < minimum ||
            value.get<double>() > std::numeric_limits<int>::max()) {
            throw std::runtime_error("structure integer vector outside bounds");
        }
    }
}

void CheckStructureInput(const json& data, bool placement) {
    if (placement) {
        CheckIntegerField(data, "spacing", 1);
        CheckIntegerField(data, "separation", 0);
        CheckIntegerField(data, "salt", 0, std::numeric_limits<std::uint32_t>::max());
        if (data.value("density", 1.0) < 0 || data.value("density", 1.0) > 1) {
            throw std::runtime_error("structure placement clamping refused");
        }
        return;
    }
    if (data.contains("boxes")) {
        for (const auto& box : data.at("boxes")) {
            CheckStructureVector(box, "min", std::numeric_limits<int>::min());
            CheckStructureVector(box, "size", 1);
            CheckIntegerField(box, "material", 0, std::numeric_limits<std::uint8_t>::max());
            // An exclusive box maximum is consumed later as an int too.
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto minimum = box.contains("min") ? box.at("min")[axis].get<int>() : 0;
                const auto size = box.contains("size") ? box.at("size")[axis].get<int>() : 1;
                if (static_cast<std::int64_t>(minimum) + size > std::numeric_limits<int>::max()) {
                    throw std::runtime_error("structure box maximum outside integer domain");
                }
            }
        }
    }
    if (data.contains("sockets")) {
        for (const auto& socket : data.at("sockets")) {
            CheckStructureVector(socket, "position", std::numeric_limits<int>::min());
        }
    }
}

// SHA-256 of exact bytes, local to input provenance; no session/hash codec changes.
std::string Sha256(const std::string& bytes) {
    constexpr std::array<std::uint32_t, 64> constants{
        {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
         0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
         0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
         0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
         0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
         0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
         0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
         0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
         0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
         0xc67178f2}};
    std::array<std::uint32_t, 8> hash{{0x6a09e667,
                                       0xbb67ae85,
                                       0x3c6ef372,
                                       0xa54ff53a,
                                       0x510e527f,
                                       0x9b05688c,
                                       0x1f83d9ab,
                                       0x5be0cd19}};
    std::vector<unsigned char> padded(bytes.begin(), bytes.end());
    const auto bits = static_cast<std::uint64_t>(padded.size()) * 8;
    padded.push_back(0x80);
    while (padded.size() % 64 != 56) {
        padded.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<unsigned char>(bits >> shift));
    }
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            for (std::size_t j = 0; j < 4; ++j) {
                words[i] = (words[i] << 8) | padded[offset + i * 4 + j];
            }
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto a = words[i - 15], b = words[i - 2];
            const auto s0 = std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3);
            const auto s1 = std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto work = hash;
        for (std::size_t i = 0; i < 64; ++i) {
            const auto s1 = std::rotr(work[4], 6) ^ std::rotr(work[4], 11) ^ std::rotr(work[4], 25);
            const auto choice = (work[4] & work[5]) ^ (~work[4] & work[6]);
            const auto t1 = work[7] + s1 + choice + constants[i] + words[i];
            const auto s0 = std::rotr(work[0], 2) ^ std::rotr(work[0], 13) ^ std::rotr(work[0], 22);
            const auto majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
            const auto t2 = s0 + majority;
            for (std::size_t j = 7; j > 0; --j) {
                work[j] = work[j - 1];
            }
            work[4] += t1;
            work[0] = t1 + t2;
        }
        for (std::size_t i = 0; i < 8; ++i) {
            hash[i] += work[i];
        }
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto word : hash) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            result += hex[(word >> shift) & 15];
        }
    }
    return result;
}

fs::path DependencyPath(const fs::path& root, const fs::path& relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name()) {
        throw std::runtime_error("dependency must have a relative name");
    }
    fs::path path = root;
    for (const auto& component : relative) {
        if (component == ".." || component == ".") {
            throw std::runtime_error("dependency traversal refused");
        }
        path /= component;
        if (fs::is_symlink(fs::symlink_status(path))) {
            throw std::runtime_error("dependency symlink refused");
        }
    }
    return path;
}

using Snapshot = std::map<std::string, std::string>;
// Inspect only resolved selectors here: the preset loader itself consumes
// dependencies, so their bytes and integer domains must be checked before it runs.
Snapshot CaptureInputs(const Options& options, const json& resolved_preset) {
    Snapshot files{{"preset.json", ReadBytes(options.preset)}};
    const auto capture = [&files, &options](const fs::path& relative) {
        files.emplace("data/" + GenericUtf8PathString(relative),
                      ReadBytes(DependencyPath(options.data_root, relative)));
    };
    const auto& generation = resolved_preset.at("generation_params");
    const auto table = generation.at("biomes").at("table").get<std::string>();
    if (!table.empty()) {
        capture(PathFromUtf8(table));
    }
    if (generation.at("features").value("structures_enabled", false)) {
        const auto root = DependencyPath(options.data_root, "common/structures");
        if (!fs::is_directory(root)) {
            throw std::runtime_error("missing structure directory");
        }
        for (const auto& type : fs::directory_iterator(root)) {
            if (type.is_symlink()) {
                throw std::runtime_error("structure symlink refused");
            }
            if (!type.is_directory()) {
                continue;
            }
            // Match the loader's explicit optional-file lookup, including on
            // case-insensitive filesystems where the directory spelling differs.
            const auto placement = type.path() / "placement.json";
            if (fs::exists(placement)) {
                capture(placement.lexically_relative(options.data_root));
            }
            for (const auto& piece : fs::directory_iterator(type.path())) {
                if (piece.path().extension() == ".json" &&
                    piece.path().filename() != "placement.json") {
                    capture(piece.path().lexically_relative(options.data_root));
                }
            }
        }
    }
    for (const auto& [name, bytes] : files) {
        const auto data = StrictJson(bytes);
        if (name.starts_with("data/common/structures/")) {
            // Case aliases can cause the same file to be read as placement and
            // piece data. Check both integer domains; valid files use disjoint keys.
            CheckStructureInput(data, true);
            CheckStructureInput(data, false);
        }
    }
    return files;
}
std::string InputIdentity(const Snapshot& files) {
    json manifest = json::object();
    for (const auto& [name, bytes] : files) {
        manifest[name] = Sha256(bytes);
    }
    // Compact, sorted UTF-8 JSON with no trailing LF, matching the Python contract.
    return Sha256(manifest.dump(-1, ' ', false));
}
void ValidateDependencies(const Options& options,
                          const Luminumbra::Systems::TerrainGenParams& params,
                          const Snapshot& files) {
    if (params.biomes_enabled) {
        const auto table =
            Luminumbra::World::BiomeTable::Load(PathFromUtf8(params.biome_table_path));
        if (!table.ok() || table.empty() || !table.warnings().empty()) {
            throw std::runtime_error("biome table errors or warnings refused");
        }
        const auto relative =
            PathFromUtf8(params.biome_table_path).lexically_relative(options.data_root);
        const auto data = StrictJson(files.at("data/" + GenericUtf8PathString(relative)));
        for (const auto& biome : data.at("biomes")) {
            for (const auto* block : {"vegetation", "reverb"}) {
                if (!biome.contains(block)) {
                    continue;
                }
                const auto& values = biome.at(block);
                for (const auto* key : {"density", "wet", "dry", "decay"}) {
                    if (values.contains(key)) {
                        const float value = values.at(key).get<float>();
                        if (value < 0 || (std::string(key) != "decay" && value > 1)) {
                            throw std::runtime_error("biome clamping refused");
                        }
                    }
                }
            }
        }
    }
    if (params.structures_enabled) {
        const auto root = DependencyPath(options.data_root, "common/structures");
        std::size_t pools = 0;
        for (const auto& type : fs::directory_iterator(root)) {
            if (!type.is_directory()) {
                continue;
            }
            ++pools;
            const auto pool = Luminumbra::World::LoadStructureTemplatePool(
                type.path(), GenericUtf8PathString(type.path().filename()));
            if (!pool.ok() || !pool.warnings.empty()) {
                throw std::runtime_error("structure pool errors or warnings refused");
            }
        }
        if (pools == 0) {
            throw std::runtime_error("empty structure catalogue refused");
        }
    }
}
void CheckNumerics() {
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    throw std::runtime_error("unsupported fast-math numerics");
#else
    if (!std::numeric_limits<float>::is_iec559 || std::numeric_limits<float>::digits != 24 ||
        !std::numeric_limits<double>::is_iec559 || std::numeric_limits<double>::digits != 53 ||
        std::fegetround() != FE_TONEAREST) {
        throw std::runtime_error("unsupported floating-point profile");
    }
#if defined(__x86_64__) || defined(_M_X64)
    // Require nearest rounding and preserve denormals (FTZ/DAZ both disabled).
    if ((_mm_getcsr() & ((1u << 15) | (1u << 6) | (3u << 13))) != 0) {
        throw std::runtime_error("unsupported SIMD floating-point environment");
    }
    if ((FastSIMD::COMPILED_SIMD_LEVELS & FastSIMD::Level_AVX2) == 0 ||
        FastSIMD::CPUMaxSIMDLevel() < FastSIMD::Level_AVX2) {
        throw std::runtime_error("AVX2 world queries unavailable");
    }
#else
    throw std::runtime_error("unsupported world-query ISA");
#endif
#endif
}
json Document(const Options& options,
              const std::string& identity,
              const StationDiscoveryResult& result) {
    json candidates = json::array();
    for (const auto& candidate : result.candidates) {
        const auto& pose = candidate.pose;
        candidates.push_back({{"id", candidate.id},
                              {"anchor_mm", candidate.anchor_mm},
                              {"pose",
                               {{"x", pose.x},
                                {"y", pose.y},
                                {"z", pose.z},
                                {"yaw", pose.yaw},
                                {"pitch", pose.pitch}}}});
    }
    const std::string revision = LUMINUMBRA_STATION_ENGINE_REVISION;
    if (revision.size() != 40 ||
        revision.find_first_not_of("0123456789abcdef") != std::string::npos) {
        throw std::runtime_error("engine revision is unavailable");
    }
    const auto& query = options.query;
    return {{"schema", "luminumbra.station-discovery.v1"},
            {"engine_revision", revision},
            {"inputs_sha256", identity},
            {"algorithm", "sampled-portals.v1"},
            {"numeric_profile", "canonical-worldgen.v1"},
            {"certified", false},
            {"seed", options.seed},
            {"query",
             {{"kind", options.kind},
              {"origin_mm", query.origin_mm},
              {"radius_m", query.radius_m},
              {"y_range_m", query.y_range_m},
              {"view_distance_m", query.view_distance_m},
              {"limit", query.limit}}},
            {"candidates", candidates}};
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
#if !defined(_WIN32)
    // Protect stdout and the custom stderr logger before either can write.
    BoundedConsoleSink::IgnoreDefaultSigpipe();
#endif
    // Register before Log's lazy initializer, avoiding stdout and log-file side effects.
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    sink->set_pattern("%v");
    auto warnings = std::make_shared<WarningSink>();
    const std::array<spdlog::sink_ptr, 2> sinks{{sink, warnings}};
    auto logger = std::make_shared<spdlog::logger>("LUMINUMBRA", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    spdlog::register_logger(logger);
    try {
#ifdef _WIN32
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
            throw std::runtime_error("cannot set binary stdout mode");
        }
#endif
        auto options = ParseOptions(argc, argv);
        CheckNumerics();
        options.data_root = Luminumbra::Filesystem::LexicallyNormalPath(
            Luminumbra::Filesystem::AbsolutePath(options.data_root));
        if (!fs::is_directory(options.data_root)) {
            throw std::runtime_error("data root must be a directory");
        }
        const auto preset_bytes = ReadBytes(options.preset);
        const auto preset = StrictJson(preset_bytes);
        if (!preset.is_object()) {
            throw std::runtime_error("preset must be an object");
        }
        CheckPresetShape(preset);
        const auto resolved_preset = ResolveTerrainPresetDefaults(preset);
        const auto files = CaptureInputs(options, resolved_preset);
        if (files.at("preset.json") != preset_bytes) {
            throw std::runtime_error("preset changed while loading");
        }
        const auto loaded = LoadTerrainPresetFromJson(preset, options.data_root, "preset.json");
        if (!loaded.ok || !loaded.warnings.empty()) {
            throw std::runtime_error("preset errors, warnings or clamping refused");
        }
        const auto& params = loaded.params;
        CheckHydroDomain(options, params);
        if (params.octaves < 1 || params.octaves > 32 ||
            (params.surface_breaks_enabled &&
             (params.feature_cell_size <= 0 || params.max_feature_radius < 3 ||
              params.surface_break_density < 0 || params.surface_break_density > 1))) {
            throw std::runtime_error("unsupported terrain query parameters");
        }
        ValidateDependencies(options, params, files);
        Luminumbra::Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, options.seed);
        const auto result = DiscoverStations(world, options.query);
        if (!result.ok) {
            throw std::runtime_error(result.error);
        }
        if (CaptureInputs(options, resolved_preset) != files) {
            throw std::runtime_error("input identity changed during discovery");
        }
        if (warnings->Observed()) {
            throw std::runtime_error("input or world-query warnings refused");
        }
        const std::string document =
            Document(options, InputIdentity(files), result).dump(-1, ' ', false) + "\n";
        std::cout << document;
        std::cout.flush();
        if (!std::cout) {
            throw std::runtime_error("cannot write station discovery stdout");
        }
        return 0;
    } catch (const UsageError& error) {
        std::cerr << "station discovery usage: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "station discovery refused: " << error.what() << '\n';
        return 1;
    }
}
