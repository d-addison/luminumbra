// CI evidence only. No platform pins: compare the full staged artifact across lanes.
#include <FastNoise/FastNoise.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#include "core/AmbientDiagnosticBuild.h"
#include "core/EngineVersion.h"
#include "luminumbra_common/core/DeterministicMath.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/AmbientDiagnosticCapture.h"
#include "luminumbra_common/systems/AmbientNoiseDispatch.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

namespace {

using Json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;
using Luminumbra::DeterministicMath::BitsOf;
using Luminumbra::Systems::AetherFieldSystem;
using Luminumbra::Systems::WeatherSystem;
using Luminumbra::Systems::WindFieldSystem;
constexpr int kSeed = 424242;
constexpr std::uint64_t kLastTick = 64;
constexpr std::size_t kMaxStageBytes = 4096 * 6 * sizeof(float);
constexpr std::size_t kMaxArtifactBytes = 16 * 1024 * 1024;
const Luminumbra::Vec3 kAnchor(8.0f, 100.0f, 8.0f);

std::string Hex(std::uint64_t value, int width) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::hex << std::setw(width) << std::setfill('0') << value;
    return out.str();
}

void AppendLE(Bytes& bytes, std::uint64_t value, unsigned int width) {
    for (unsigned int i = 0; i < width; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xffu));
    }
}

void AppendFloat(Bytes& bytes, float value) {
    AppendLE(bytes, BitsOf(value), 4);
}

std::string Hash(const Bytes& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return Hex(hash, 16);
}

struct Stage {
    std::string encoding;
    Bytes bytes;
};

struct GraphNodes {
    const FastNoise::Generator* fractal = nullptr;
    const FastNoise::Generator* simplex = nullptr;
};

struct FractalParameters {
    float gain = 0.0f;
    float weighted_strength = 0.0f;
    float lacunarity = 0.0f;
    float bounding = 0.0f;
    int octaves = 0;
};

// Form pointers to inherited protected members in a derived test accessor, then
// apply those base-class member pointers to the real graph. No casts to a fake
// derived object, vendor edits, duplicate source nodes or graph mutations.
class FractalProbeAccess : public FastNoise::FractalFBm {
public:
    static const FastNoise::Generator& Source(const FastNoise::Generator& generator) {
        const auto& node = dynamic_cast<const FastNoise::FractalFBm&>(generator);
        return *(node.*(&FractalProbeAccess::mSource)).base;
    }

    static FractalParameters Parameters(const FastNoise::Generator& generator) {
        const auto& node = dynamic_cast<const FastNoise::FractalFBm&>(generator);
        const auto& gain = node.*(&FractalProbeAccess::mGain);
        const auto& strength = node.*(&FractalProbeAccess::mWeightedStrength);
        if (gain.base.get() != nullptr || strength.base.get() != nullptr) {
            throw std::runtime_error("ambient probe requires constant gain and strength");
        }
        return {gain.constant,
                strength.constant,
                node.*(&FractalProbeAccess::mLacunarity),
                node.*(&FractalProbeAccess::mFractalBounding),
                node.*(&FractalProbeAccess::mOctaves)};
    }
};

class Capture final : public Luminumbra::Systems::AmbientDiagnosticCapture {
public:
    [[nodiscard]] bool WantsTick(std::uint64_t tick) const override {
        return tick == 0 || tick == kLastTick;
    }

    void Graph(std::string_view name,
               const FastNoise::Generator& fractal,
               const FastNoise::Generator& simplex) {
        graphs.emplace(std::string(name), GraphNodes{&fractal, &simplex});
    }

    void Floats(std::uint64_t tick,
                std::string_view name,
                const float* values,
                std::size_t count) override {
        Stage& stage = stages[{tick, std::string(name)}];
        stage.encoding = "f32le";
        if (count > kMaxStageBytes / sizeof(float) ||
            stage.bytes.size() > kMaxStageBytes - count * sizeof(float)) {
            throw std::runtime_error("ambient float stage exceeds fixed capture bound");
        }
        for (std::size_t i = 0; i < count; ++i) {
            AppendFloat(stage.bytes, values[i]);
        }
    }

    void Sweep(std::uint64_t tick, int iteration, const float* values, std::size_t count) override {
        Floats(tick, "aether.diffusion." + std::to_string(iteration + 1), values, count);
    }

    void ByteStage(std::uint64_t tick, std::string_view name, Bytes bytes) {
        if (bytes.size() > kMaxStageBytes) {
            throw std::runtime_error("ambient byte stage exceeds fixed capture bound");
        }
        stages.emplace(std::make_pair(tick, std::string(name)), Stage{"bytes", std::move(bytes)});
    }

    std::vector<float> ReadFloats(std::uint64_t tick, std::string_view name) const {
        const Bytes& bytes = stages.at({tick, std::string(name)}).bytes;
        std::vector<float> result;
        result.reserve(bytes.size() / sizeof(float));
        for (std::size_t i = 0; i < bytes.size(); i += sizeof(float)) {
            std::uint32_t bits = 0;
            for (unsigned int j = 0; j < 4; ++j) {
                bits |= static_cast<std::uint32_t>(bytes[i + j]) << (8u * j);
            }
            result.push_back(Luminumbra::DeterministicMath::FromBits(bits));
        }
        return result;
    }

    // Probe the SAME source node, ISA, batch coordinates and seed as the field.
    // Vendor arithmetic stays untouched. The last accumulated sum is the actual
    // captured FBm output; the first is its single binary32 product. Also expose
    // fused/non-fused reconstructions to identify additional accumulation drift.
    void Probe(std::uint64_t tick,
               const std::string& graph_name,
               const std::string& prefix,
               std::vector<float> x,
               std::vector<float> z,
               int seed,
               const std::vector<float>& actual) {
        if (x.size() != z.size() || x.size() != actual.size() || x.empty() || x.size() > 4096) {
            throw std::runtime_error("invalid ambient probe geometry");
        }
        const std::size_t count = x.size();
        // FastNoise reads a full vector even for the two base samples.
        x.resize(std::max<std::size_t>(count, 16), 0.0f);
        z.resize(std::max<std::size_t>(count, 16), 0.0f);
        const GraphNodes& graph = graphs.at(graph_name);
        const FractalParameters parameters = FractalProbeAccess::Parameters(*graph.fractal);
        if (parameters.gain <= 0.0f || parameters.lacunarity <= 0.0f ||
            parameters.weighted_strength != 0.0f || parameters.octaves != 2) {
            throw std::runtime_error("ambient graph no longer fits two-octave probe");
        }
        const float amp0 = parameters.bounding;
        const float amp1 = amp0 * parameters.gain;
        Bytes seeds;
        AppendLE(seeds, static_cast<std::uint32_t>(seed), 4);
        AppendLE(seeds, static_cast<std::uint32_t>(seed + 1), 4);
        ByteStage(tick, prefix + ".octave_seeds", std::move(seeds));
        Samples(tick,
                prefix + ".parameters",
                {parameters.gain, parameters.lacunarity, parameters.weighted_strength, amp0, amp1});
        Samples(tick, prefix + ".position_offsets", {0.0f, 0.0f});
        std::vector<float> raw0(count);
        std::vector<float> raw1(count);
        graph.simplex->GenPositionArray2D(
            raw0.data(), static_cast<int>(count), x.data(), z.data(), 0.0f, 0.0f, seed);
        Floats(tick, prefix + ".octave0.x", x.data(), count);
        Floats(tick, prefix + ".octave0.z", z.data(), count);
        for (std::size_t i = 0; i < count; ++i) {
            x[i] *= parameters.lacunarity;
            z[i] *= parameters.lacunarity;
        }
        graph.simplex->GenPositionArray2D(
            raw1.data(), static_cast<int>(count), x.data(), z.data(), 0.0f, 0.0f, seed + 1);
        Floats(tick, prefix + ".octave1.x", x.data(), count);
        Floats(tick, prefix + ".octave1.z", z.data(), count);
        Floats(tick, prefix + ".octave0.simplex", raw0.data(), count);
        Floats(tick, prefix + ".octave1.simplex", raw1.data(), count);
        std::vector<float> sum0(count);
        std::vector<float> unfused(count);
        std::vector<float> fused(count);
        for (std::size_t i = 0; i < count; ++i) {
            sum0[i] = raw0[i] * amp0;
            // Volatile fixes a rounding boundary in diagnostic replay only.
            // common_tests does not inherit the engine's private FP options.
            const volatile float product = raw1[i] * amp1;
            unfused[i] = sum0[i] + product;
            fused[i] = std::fma(raw1[i], amp1, sum0[i]);
        }
        Floats(tick, prefix + ".sum0.product_replay", sum0.data(), count);
        Floats(tick, prefix + ".sum1.actual", actual.data(), count);
        Floats(tick, prefix + ".sum1.unfused_replay", unfused.data(), count);
        Floats(tick, prefix + ".sum1.fused_replay", fused.data(), count);
    }

    Json StageJson() const {
        Json result = Json::array();
        for (const auto& [key, stage] : stages) {
            const std::size_t width = stage.encoding == "f32le" ? 4 : 1;
            Json bits = Json::array();
            for (std::size_t i = 0; i < stage.bytes.size(); i += width) {
                std::uint64_t word = 0;
                for (std::size_t j = 0; j < width; ++j) {
                    word |= static_cast<std::uint64_t>(stage.bytes[i + j]) << (8u * j);
                }
                bits.push_back(Hex(word, static_cast<int>(width * 2)));
            }
            result.push_back({{"tick", key.first},
                              {"stage", key.second},
                              {"encoding", stage.encoding},
                              {"count", bits.size()},
                              {"fnv1a64", Hash(stage.bytes)},
                              {"bits", std::move(bits)}});
        }
        return result;
    }

    std::map<std::string, GraphNodes> graphs;
    std::map<std::pair<std::uint64_t, std::string>, Stage> stages;
    Json receipts = Json::array();
};

template<typename T>
Bytes Metadata(const luminumbra::fields::FieldGrid<T>& grid, int seed, std::uint64_t tick) {
    Bytes bytes;
    AppendLE(bytes, static_cast<std::uint32_t>(seed), 8);
    AppendLE(bytes, tick, 8);
    AppendLE(bytes, static_cast<std::uint32_t>(grid.extent_cells()), 8);
    AppendFloat(bytes, grid.cell_size_m());
    AppendLE(bytes, static_cast<std::uint64_t>(grid.origin_cell_x()), 8);
    AppendLE(bytes, static_cast<std::uint64_t>(grid.origin_cell_z()), 8);
    return bytes;
}

void VectorStage(Capture& capture,
                 std::uint64_t tick,
                 std::string_view name,
                 const std::vector<float>& values) {
    capture.Floats(tick, name, values.data(), values.size());
}

} // namespace

namespace Luminumbra::Systems {

// The only installer/read access for the private diagnostic seam. Thread-local
// scope includes construction and is reset even if a test assertion throws.
class AmbientDiagnosticAccess {
public:
    explicit AmbientDiagnosticAccess(AmbientDiagnosticCapture& capture) {
        WindFieldSystem::s_diagnostic_capture = &capture;
        AetherFieldSystem::s_diagnostic_capture = &capture;
    }
    ~AmbientDiagnosticAccess() {
        WindFieldSystem::s_diagnostic_capture = nullptr;
        AetherFieldSystem::s_diagnostic_capture = nullptr;
    }
    AmbientDiagnosticAccess(const AmbientDiagnosticAccess&) = delete;
    AmbientDiagnosticAccess& operator=(const AmbientDiagnosticAccess&) = delete;

    static void Snapshot(Capture& capture,
                         const WindFieldSystem& wind,
                         const WeatherSystem& weather,
                         const AetherFieldSystem& aether) {
        const std::uint64_t tick = wind.m_last_tick;
        if (tick == 0) {
            for (const auto& [name, node] :
                 std::initializer_list<std::pair<std::string_view, const FastNoise::Generator*>>{
                     {"wind.direction", wind.m_direction_noise.get()},
                     {"weather.pressure", weather.m_pressure_noise.get()},
                     {"weather.temperature", weather.m_temperature_noise.get()},
                     {"weather.humidity", weather.m_humidity_noise.get()},
                     {"aether.emission", aether.m_emission_noise.get()}}) {
                capture.Graph(name, *node, FractalProbeAccess::Source(*node));
            }
        }
        capture.ByteStage(tick, "wind.metadata", Metadata(wind.m_grid, wind.m_wind_seed, tick));
        capture.ByteStage(
            tick, "weather.metadata", Metadata(weather.m_precip, weather.m_weather_seed, tick));
        capture.ByteStage(
            tick, "aether.metadata", Metadata(aether.m_grid, aether.m_aether_seed, tick));
        VectorStage(capture, tick, "wind.cell.x", wind.m_scratch_px);
        VectorStage(capture, tick, "wind.cell.z", wind.m_scratch_pz);
        VectorStage(capture, tick, "wind.cell.noise", wind.m_scratch_cell_noise);
        for (const auto& cell : wind.m_grid.cells()) {
            for (const auto& layer : cell.layer) {
                capture.Samples(tick, "wind.final_layers", {layer.x, layer.y});
            }
        }
        VectorStage(capture, tick, "weather.x", weather.m_scratch_px);
        VectorStage(capture, tick, "weather.z", weather.m_scratch_pz);
        VectorStage(capture, tick, "weather.raw_pressure", weather.m_scratch_pressure);
        VectorStage(capture, tick, "weather.raw_temperature", weather.m_scratch_temperature);
        VectorStage(capture, tick, "weather.raw_humidity", weather.m_scratch_humidity);
        capture.ByteStage(tick, "weather.categories", weather.m_category.cells());
        VectorStage(capture, tick, "weather.precipitation", weather.m_precip.cells());
        // Identical byte order to ComputeWeatherSubHash, without struct padding.
        Bytes storms;
        AppendLE(storms, static_cast<std::uint64_t>(weather.m_storm_cells.size()), 8);
        capture.Floats(tick, "weather.storm_float_fields", nullptr, 0);
        for (const auto& cell : weather.m_storm_cells) {
            capture.Samples(tick,
                            "weather.storm_float_fields",
                            {cell.center_world.x,
                             cell.center_world.y,
                             cell.velocity.x,
                             cell.velocity.y,
                             cell.intensity});
            for (float value : {cell.center_world.x,
                                cell.center_world.y,
                                cell.velocity.x,
                                cell.velocity.y,
                                cell.intensity}) {
                AppendFloat(storms, value);
            }
            AppendLE(storms, cell.spawn_tick, 8);
            AppendLE(storms, cell.lifetime_ticks, 8);
            AppendLE(storms, cell.seed_salt, 8);
        }
        capture.ByteStage(tick, "weather.storms", std::move(storms));
        Bytes strikes;
        AppendLE(strikes, static_cast<std::uint32_t>(weather.m_lightning_seed), 8);
        AppendLE(strikes, static_cast<std::uint64_t>(weather.m_strikes.size()), 8);
        capture.Floats(tick, "weather.strike_float_fields", nullptr, 0);
        for (const auto& strike : weather.m_strikes) {
            capture.Samples(tick,
                            "weather.strike_float_fields",
                            {strike.world_x, strike.world_z, strike.magnitude});
            AppendLE(strikes, strike.strike_tick, 8);
            AppendFloat(strikes, strike.world_x);
            AppendFloat(strikes, strike.world_z);
            AppendFloat(strikes, strike.magnitude);
            AppendLE(strikes, strike.storm_salt, 8);
        }
        capture.ByteStage(tick, "weather.strikes", std::move(strikes));
        VectorStage(capture, tick, "aether.x", aether.m_scratch_px);
        VectorStage(capture, tick, "aether.z", aether.m_scratch_pz);
        VectorStage(capture, tick, "aether.final", aether.m_grid.cells());
        capture.Probe(tick,
                      "wind.direction",
                      "wind.base.fbm",
                      capture.ReadFloats(tick, "wind.base.x"),
                      capture.ReadFloats(tick, "wind.base.z"),
                      wind.m_wind_seed,
                      capture.ReadFloats(tick, "wind.base.noise"));
        capture.Probe(tick,
                      "wind.direction",
                      "wind.cell.fbm",
                      wind.m_scratch_px,
                      wind.m_scratch_pz,
                      wind.m_wind_seed,
                      wind.m_scratch_cell_noise);
        capture.Probe(tick,
                      "weather.pressure",
                      "weather.pressure.fbm",
                      weather.m_scratch_px,
                      weather.m_scratch_pz,
                      weather.m_weather_seed,
                      weather.m_scratch_pressure);
        capture.Probe(tick,
                      "weather.temperature",
                      "weather.temperature.fbm",
                      weather.m_scratch_px,
                      weather.m_scratch_pz,
                      weather.m_weather_seed + 101,
                      weather.m_scratch_temperature);
        capture.Probe(tick,
                      "weather.humidity",
                      "weather.humidity.fbm",
                      weather.m_scratch_px,
                      weather.m_scratch_pz,
                      weather.m_weather_seed + 211,
                      weather.m_scratch_humidity);
        capture.Probe(tick,
                      "aether.emission",
                      "aether.emission.fbm",
                      aether.m_scratch_px,
                      aether.m_scratch_pz,
                      aether.m_aether_seed,
                      capture.ReadFloats(tick, "aether.raw_emission"));
        capture.receipts.push_back({{"tick", tick},
                                    {"wind", wind.ComputeWindSubHash()},
                                    {"weather", weather.ComputeWeatherSubHash()},
                                    {"aether", aether.ComputeAetherSubHash()}});
    }
};

} // namespace Luminumbra::Systems

namespace {

Json CompileCommands() {
    const auto path =
        std::filesystem::path(AmbientDiagnosticBuild::kBinaryRoot) / "compile_commands.json";
    if (std::filesystem::file_size(path) > 32 * 1024 * 1024) {
        throw std::runtime_error("compile database exceeds diagnostic input bound");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("ambient diagnostic requires exported compile commands");
    }
    const Json database = Json::parse(input);
    Json commands = Json::object();
    const std::array<std::string_view, 4> names = {"WindFieldSystem.cpp",
                                                   "WeatherSystem.cpp",
                                                   "AetherFieldSystem.cpp",
                                                   "FastSIMD_Level_AVX2.cpp"};
    for (const auto& entry : database) {
        const auto name =
            std::filesystem::path(entry.at("file").get<std::string>()).filename().string();
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            if (commands.contains(name)) {
                throw std::runtime_error("ambiguous ambient translation unit in compile database");
            }
            commands[name] = entry;
        }
    }
    if (commands.size() != names.size()) {
        throw std::runtime_error("ambient compile command evidence is incomplete");
    }
    return commands;
}

Json Mxcsr() {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    const unsigned int value = _mm_getcsr();
    return {{"raw", Hex(value, 8)},
            {"rounding", (value >> 13u) & 3u},
            {"ftz", (value & (1u << 15u)) != 0},
            {"daz", (value & (1u << 6u)) != 0}};
#else
    return {{"available", false}};
#endif
}

TEST(AmbientStageDiagnostic, WritesBoundedCanonicalEvidence) {
    const char* enabled = std::getenv("LUMINUMBRA_AMBIENT_DIAGNOSTIC");
    if (enabled == nullptr || std::string_view(enabled) != "1") {
        std::cout << "Ambient staged capture is opt-in: LUMINUMBRA_AMBIENT_DIAGNOSTIC=1\n";
        return;
    }
    Capture capture;
    Json identity = {{"revision", luminumbra::core::kEngineGitSha},
                     {"compiler", AmbientDiagnosticBuild::kCompiler},
                     {"compiler_version", AmbientDiagnosticBuild::kCompilerVersion},
                     {"compile_commands", CompileCommands()},
                     {"mxcsr_before", Mxcsr()},
                     {"ceiling", static_cast<int>(Luminumbra::Systems::kAmbientNoiseMaxSIMDLevel)},
                     {"cpu_simd", static_cast<int>(FastSIMD::CPUMaxSIMDLevel())}};
#if defined(_MSC_FULL_VER)
    identity["compiler_full_version"] = std::to_string(_MSC_FULL_VER);
#elif defined(__VERSION__)
    identity["compiler_full_version"] = __VERSION__;
#endif
    {
        Luminumbra::Systems::AmbientDiagnosticAccess access(capture);
        WindFieldSystem wind(kSeed);
        WeatherSystem weather(kSeed);
        AetherFieldSystem aether(kSeed);
        capture.Samples(0, "inputs.region_anchor", {0.0f, 0.0f, 0.0f});
        capture.ByteStage(0, "inputs.wind_coupling", Bytes{0});
        identity["mxcsr_tick0"] = Mxcsr();
        Luminumbra::Systems::AmbientDiagnosticAccess::Snapshot(capture, wind, weather, aether);
        for (std::uint64_t tick = 1; tick <= kLastTick; ++tick) {
            wind.Update(tick, kAnchor);
            weather.Update(tick, kAnchor, &wind);
            aether.Update(tick, kAnchor, &wind);
        }
        capture.Samples(kLastTick, "inputs.region_anchor", {kAnchor.x, kAnchor.y, kAnchor.z});
        capture.ByteStage(kLastTick, "inputs.wind_coupling", Bytes{1});
        identity["mxcsr_tick64"] = Mxcsr();
        Luminumbra::Systems::AmbientDiagnosticAccess::Snapshot(capture, wind, weather, aether);
        Json nodes = Json::object();
        ASSERT_EQ(capture.graphs.size(), 5u);
        for (const auto& [name, graph] : capture.graphs) {
            const int fractal_level = static_cast<int>(graph.fractal->GetSIMDLevel());
            const int simplex_level = static_cast<int>(graph.simplex->GetSIMDLevel());
            EXPECT_EQ(fractal_level, simplex_level) << name;
            nodes[name + ".fbm"] = fractal_level;
            nodes[name + ".simplex"] = simplex_level;
        }
        identity["selected_isa"] = std::move(nodes);
    }
    // GitHub supplies the full tested revision; retain the compiled engine identity too.
    if (const char* revision = std::getenv("GITHUB_SHA"); revision != nullptr) {
        identity["engine_revision"] = luminumbra::core::kEngineGitSha;
        identity["revision"] = revision;
        EXPECT_EQ(std::string_view(revision).substr(0, luminumbra::core::kEngineGitSha.size()),
                  luminumbra::core::kEngineGitSha);
    }
    identity["mxcsr_after"] = Mxcsr();
    for (const std::uint64_t tick : {std::uint64_t{0}, kLastTick}) {
        EXPECT_EQ(capture.stages.at({tick, "wind.final_layers"}).bytes.size(),
                  4096u * 6u * sizeof(float));
        EXPECT_EQ(capture.stages.at({tick, "weather.categories"}).bytes.size(), 4096u);
        for (int iteration = 1; iteration <= Luminumbra::Systems::kAetherDiffuseIterations;
             ++iteration) {
            EXPECT_EQ(capture.stages.at({tick, "aether.diffusion." + std::to_string(iteration)})
                          .bytes.size(),
                      4096u * sizeof(float));
        }
    }
    // Same sequence without the observer, preserving all six existing receipts.
    WindFieldSystem wind(kSeed);
    WeatherSystem weather(kSeed);
    AetherFieldSystem aether(kSeed);
    EXPECT_EQ(capture.receipts[0]["wind"], wind.ComputeWindSubHash());
    EXPECT_EQ(capture.receipts[0]["weather"], weather.ComputeWeatherSubHash());
    EXPECT_EQ(capture.receipts[0]["aether"], aether.ComputeAetherSubHash());
    for (std::uint64_t tick = 1; tick <= kLastTick; ++tick) {
        wind.Update(tick, kAnchor);
        weather.Update(tick, kAnchor, &wind);
        aether.Update(tick, kAnchor, &wind);
    }
    EXPECT_EQ(capture.receipts[1]["wind"], wind.ComputeWindSubHash());
    EXPECT_EQ(capture.receipts[1]["weather"], weather.ComputeWeatherSubHash());
    EXPECT_EQ(capture.receipts[1]["aether"], aether.ComputeAetherSubHash());
    const Json artifact = {{"schema", "luminumbra.ambient-stage-diagnostic.v1"},
                           {"seed", kSeed},
                           {"identity", std::move(identity)},
                           {"receipts", capture.receipts},
                           {"stages", capture.StageJson()}};
    const std::string serialized = artifact.dump(2) + '\n';
    ASSERT_LE(serialized.size(), kMaxArtifactBytes);
    const auto directory =
        std::filesystem::path(AmbientDiagnosticBuild::kBinaryRoot) / "test-artifacts" / "ambient";
    std::filesystem::create_directories(directory);
    const auto path = directory / "stages.json";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open()) << path;
    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    output.close();
    ASSERT_TRUE(output.good()) << path;
    RecordProperty("ambient_stage_artifact", "test-artifacts/ambient/stages.json");
}

} // namespace
