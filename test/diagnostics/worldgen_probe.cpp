// Worldgen timing probe. Gtest-free diagnostic that times chunk generation,
// terrain meshing, height queries, job-system scaling, and libm call costs
// through public engine APIs. Writes a JSON report and prints a text summary.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#include "nlohmann/json.hpp"

#include "core/JobSystem.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"
#include "world/TerrainPresetLoader.h"

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define LUM_PROBE_NOINLINE __declspec(noinline)
#else
#define LUM_PROBE_NOINLINE __attribute__((noinline))
#endif

constexpr const char* kSchemaId = "luminumbra.worldgen_probe.v1";
constexpr int kSeed = 424242;
constexpr int kDefaultChunks = 24;
constexpr int kSmokeChunks = 2;
constexpr int kGridSide = 32;
constexpr std::size_t kFullLibmCalls = 1000000u;
constexpr std::size_t kSmokeLibmCalls = 1000u;
constexpr std::size_t kFullShapedReps = 8u;
constexpr std::size_t kSmokeShapedReps = 2u;
constexpr std::size_t kScalingRequests[] = {1u, 2u, 4u, 0u}; // 0 = startup() default

using Clock = std::chrono::steady_clock;

// Sinks keep computed results observable so the optimizer cannot discard them.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile float g_probe_sink_f = 0.0f;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile double g_probe_sink_d = 0.0;

struct Options {
    std::string json_path;
    int chunks = kDefaultChunks;
    bool chunks_set = false;
    bool smoke = false;
};

struct Stats {
    std::size_t samples = 0;
    double min_ms = 0.0;
    double median_ms = 0.0;
    double max_ms = 0.0;
    double total_ms = 0.0;
};

double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Stats ComputeStats(std::vector<double> samples_ms) {
    Stats stats;
    stats.samples = samples_ms.size();
    if (samples_ms.empty()) {
        return stats;
    }
    for (const double sample : samples_ms) {
        stats.total_ms += sample;
    }
    std::sort(samples_ms.begin(), samples_ms.end());
    stats.min_ms = samples_ms.front();
    stats.max_ms = samples_ms.back();
    const std::size_t mid = samples_ms.size() / 2u;
    stats.median_ms = (samples_ms.size() % 2u == 0u)
                          ? 0.5 * (samples_ms[mid - 1u] + samples_ms[mid])
                          : samples_ms[mid];
    return stats;
}

nlohmann::json StatsToJson(const Stats& stats) {
    return {
        {"samples", stats.samples},
        {"min_ms", stats.min_ms},
        {"median_ms", stats.median_ms},
        {"max_ms", stats.max_ms},
        {"total_ms", stats.total_ms},
    };
}

std::string CompilerId() {
#if defined(__VERSION__)
    return std::string(__VERSION__);
#elif defined(_MSC_FULL_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

std::string TrimBrand(const char* raw) {
    const std::string text(raw);
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "unknown";
    }
    const std::size_t last = text.find_last_not_of(' ');
    return text.substr(first, last - first + 1u);
}

std::string CpuBrand() {
#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, static_cast<int>(0x80000000u));
    if (static_cast<unsigned int>(regs[0]) < 0x80000004u) {
        return "unknown";
    }
    char raw[49] = {};
    for (int leaf = 0; leaf < 3; ++leaf) {
        __cpuid(regs, static_cast<int>(0x80000002u + static_cast<unsigned int>(leaf)));
        std::memcpy(raw + leaf * 16, regs, sizeof(regs));
    }
    return TrimBrand(raw);
#elif defined(__x86_64__) || defined(__i386__)
    if (__get_cpuid_max(0x80000000u, nullptr) < 0x80000004u) {
        return "unknown";
    }
    char raw[49] = {};
    for (unsigned int leaf = 0; leaf < 3u; ++leaf) {
        unsigned int eax = 0;
        unsigned int ebx = 0;
        unsigned int ecx = 0;
        unsigned int edx = 0;
        __cpuid(0x80000002u + leaf, eax, ebx, ecx, edx);
        const unsigned int words[4] = {eax, ebx, ecx, edx};
        std::memcpy(raw + leaf * 16u, words, sizeof(words));
    }
    return TrimBrand(raw);
#else
    return "unknown";
#endif
}

void PrintUsage(std::ostream& out) {
    out << "usage: worldgen_probe --json PATH [--chunks N] [--smoke]\n";
}

bool ParseArgs(int argc, char** argv, Options& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--smoke") {
            options.smoke = true;
        } else if (arg == "--json") {
            if (i + 1 >= argc) {
                error = "--json requires a path";
                return false;
            }
            options.json_path = argv[++i];
        } else if (arg == "--chunks") {
            if (i + 1 >= argc) {
                error = "--chunks requires a count";
                return false;
            }
            try {
                options.chunks = std::stoi(argv[++i]);
            } catch (const std::exception&) {
                error = "--chunks requires an integer";
                return false;
            }
            if (options.chunks <= 0) {
                error = "--chunks must be positive";
                return false;
            }
            options.chunks_set = true;
        } else {
            error = "unknown argument: " + arg;
            return false;
        }
    }
    if (options.json_path.empty()) {
        error = "--json PATH is required";
        return false;
    }
    if (options.smoke && !options.chunks_set) {
        options.chunks = kSmokeChunks;
    }
    return true;
}

TerrainGenParams LoadPreset(const std::filesystem::path& path) {
    const auto result = Luminumbra::world::LoadTerrainPreset(path);
    if (!result.ok) {
        std::string message = "preset load failed: " + path.string();
        for (const std::string& error : result.errors) {
            message += "; " + error;
        }
        throw std::runtime_error(message);
    }
    return result.params;
}

bool HasSurfaceContent(const Chunk& chunk) {
    bool has_solid = false;
    bool has_air = false;
    for (const float value : chunk.sdf_data) {
        if (value < 0.0f) {
            has_solid = true;
        } else {
            has_air = true;
        }
        if (has_solid && has_air) {
            return true;
        }
    }
    return false;
}

std::size_t MeshTriangleCount(const Chunk& chunk) {
    return chunk.mesh_indices.size() / 3u;
}

// Times one per-call height query across the grid and returns per-call samples.
std::vector<double> TimeHeightGrid(const SHIELD_WorldSystem& world,
                                   const std::vector<float>& xs,
                                   const std::vector<float>& zs) {
    std::vector<double> samples;
    samples.reserve(xs.size());
    float acc = 0.0f;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const Clock::time_point start = Clock::now();
        acc += world.GetTerrainHeightAt(xs[i], zs[i]);
        samples.push_back(MsSince(start));
    }
    g_probe_sink_f = acc;
    return samples;
}

// libm inputs: varied per index so calls are not constant-folded.
float LibmArg(std::size_t i) {
    return 0.25f + static_cast<float>(i & 1023u) * 0.0009765625f;
}

float LibmArgAlt(std::size_t i) {
    return 1.5f + static_cast<float>((i * 7u) & 1023u) * 0.001953125f;
}

LUM_PROBE_NOINLINE float ProbeSinf(float x) {
    return std::sin(x);
}

LUM_PROBE_NOINLINE float ProbeCosf(float x) {
    return std::cos(x);
}

LUM_PROBE_NOINLINE float ProbePowf(float x, float y) {
    return std::pow(x, y);
}

LUM_PROBE_NOINLINE float ProbeExpf(float x) {
    return std::exp(x);
}

LUM_PROBE_NOINLINE float ProbeSqrtf(float x) {
    return std::sqrt(x);
}

LUM_PROBE_NOINLINE float ProbeAtan2f(float y, float x) {
    return std::atan2(y, x);
}

LUM_PROBE_NOINLINE float ProbeFloorf(float x) {
    return std::floor(x);
}

LUM_PROBE_NOINLINE long ProbeLroundf(float x) {
    return std::lround(x);
}

LUM_PROBE_NOINLINE float ProbeFmaf(float a, float b, float c) {
    return std::fma(a, b, c);
}

LUM_PROBE_NOINLINE float ProbeHypotf(float a, float b) {
    return std::hypot(a, b);
}

template<typename Body>
void TimeLibmLoop(nlohmann::json& out, const char* name, std::size_t calls, Body&& body) {
    double acc = 0.0;
    const Clock::time_point start = Clock::now();
    for (std::size_t i = 0; i < calls; ++i) {
        acc += body(i);
    }
    const double total_ms = MsSince(start);
    g_probe_sink_d = acc;
    out[name] = {
        {"calls", calls},
        {"total_ms", total_ms},
        {"ns_per_call", total_ms * 1.0e6 / static_cast<double>(calls)},
    };
}

nlohmann::json RunLibmSuite(std::size_t calls) {
    nlohmann::json functions = nlohmann::json::object();
    TimeLibmLoop(functions, "sinf", calls, [](std::size_t i) {
        return static_cast<double>(ProbeSinf(LibmArg(i)));
    });
    TimeLibmLoop(functions, "cosf", calls, [](std::size_t i) {
        return static_cast<double>(ProbeCosf(LibmArg(i)));
    });
    TimeLibmLoop(functions, "powf", calls, [](std::size_t i) {
        return static_cast<double>(ProbePowf(LibmArg(i), LibmArgAlt(i)));
    });
    TimeLibmLoop(functions, "expf", calls, [](std::size_t i) {
        return static_cast<double>(ProbeExpf(LibmArg(i) - 0.75f));
    });
    TimeLibmLoop(functions, "sqrtf", calls, [](std::size_t i) {
        return static_cast<double>(ProbeSqrtf(LibmArg(i)));
    });
    TimeLibmLoop(functions, "atan2f", calls, [](std::size_t i) {
        return static_cast<double>(ProbeAtan2f(LibmArg(i), LibmArgAlt(i)));
    });
    TimeLibmLoop(functions, "floorf", calls, [](std::size_t i) {
        return static_cast<double>(ProbeFloorf(LibmArg(i) * 10.0f));
    });
    TimeLibmLoop(functions, "lroundf", calls, [](std::size_t i) {
        return static_cast<double>(ProbeLroundf(LibmArg(i) * 1000.0f));
    });
    TimeLibmLoop(functions, "fmaf", calls, [](std::size_t i) {
        return static_cast<double>(ProbeFmaf(LibmArg(i), LibmArgAlt(i), 0.5f));
    });
    TimeLibmLoop(functions, "hypotf", calls, [](std::size_t i) {
        return static_cast<double>(ProbeHypotf(LibmArg(i), LibmArgAlt(i)));
    });
    return {
        {"calls", calls},
        {"functions", functions},
    };
}

nlohmann::json RunScaling(const SHIELD_WorldSystem& world, const std::vector<IVec3>& coords) {
    nlohmann::json entries = nlohmann::json::array();
    for (const std::size_t request : kScalingRequests) {
        JobSystem scaled;
        scaled.startup(request);
        const std::size_t worker_count = scaled.get_runtime_stats().worker_count;

        std::vector<std::unique_ptr<Chunk>> targets;
        targets.reserve(coords.size());
        std::vector<Job> jobs;
        jobs.reserve(coords.size());
        for (const IVec3& coord : coords) {
            targets.push_back(std::make_unique<Chunk>(coord));
        }
        for (const std::unique_ptr<Chunk>& target : targets) {
            Chunk* raw = target.get();
            jobs.push_back([&world, raw]() { world.GenerateChunkData(*raw, 1); });
        }

        const Clock::time_point start = Clock::now();
        const JobHandle handle = scaled.dispatch_batch(jobs);
        scaled.wait(handle);
        const double wall_ms = MsSince(start);
        scaled.shutdown();

        nlohmann::json requested =
            request == 0u ? nlohmann::json("default") : nlohmann::json(request);
        entries.push_back({
            {"requested_workers", requested},
            {"worker_count", worker_count},
            {"chunks", coords.size()},
            {"wall_ms", wall_ms},
        });
    }
    return entries;
}

nlohmann::json RunProbe(const Options& options) {
    const std::filesystem::path preset_path =
        std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json";
    const TerrainGenParams params = LoadPreset(preset_path);

    JobSystem jobs;
    jobs.startup();
    const std::size_t default_worker_count = jobs.get_runtime_stats().worker_count;

    SHIELD_WorldSystem world(&jobs, nullptr, params, kSeed);
    const float spawn_x = 8.0f;
    const float spawn_z = 8.0f;
    const float terrain_height = world.GetTerrainHeightAt(spawn_x, spawn_z);
    if (!std::isfinite(terrain_height)) {
        throw std::runtime_error("non-finite terrain height at spawn");
    }
    const Vec3 spawn(spawn_x, terrain_height + 1.95f, spawn_z);

    const std::vector<IVec3> candidates = world.GetInitialChunkLoadList(spawn);
    const std::size_t wanted = static_cast<std::size_t>(options.chunks);
    std::vector<IVec3> selected;
    for (const IVec3& coord : candidates) {
        if (selected.size() >= wanted) {
            break;
        }
        Chunk probe(coord);
        world.GenerateChunkData(probe, 1);
        if (HasSurfaceContent(probe)) {
            selected.push_back(coord);
        }
    }
    if (selected.empty()) {
        throw std::runtime_error("no chunks with surface content near spawn");
    }

    // (a) full-detail generation on the selected chunks.
    std::vector<std::unique_ptr<Chunk>> generated;
    generated.reserve(selected.size());
    std::vector<double> gen1_ms;
    gen1_ms.reserve(selected.size());
    for (const IVec3& coord : selected) {
        auto chunk = std::make_unique<Chunk>(coord);
        const Clock::time_point start = Clock::now();
        world.GenerateChunkData(*chunk, 1);
        gen1_ms.push_back(MsSince(start));
        generated.push_back(std::move(chunk));
    }
    // Observation only: libm cost measured immediately after worldgen work.
    const nlohmann::json libm_after_generation =
        RunLibmSuite(options.smoke ? kSmokeLibmCalls : kFullLibmCalls);

    // (b) coarse (step 4) generation on the same chunks.
    std::vector<double> gen4_ms;
    gen4_ms.reserve(selected.size());
    for (const IVec3& coord : selected) {
        Chunk coarse(coord);
        const Clock::time_point start = Clock::now();
        world.GenerateChunkData(coarse, 4);
        gen4_ms.push_back(MsSince(start));
    }

    // (c) meshing of the full-detail chunks from (a).
    std::vector<double> polygonise_ms;
    polygonise_ms.reserve(generated.size());
    std::size_t mesh_triangles = 0;
    for (const std::unique_ptr<Chunk>& chunk : generated) {
        const Clock::time_point start = Clock::now();
        Luminumbra::World::MarchingCubes::PolygoniseTerrain(world, *chunk, 0.0f, 1);
        polygonise_ms.push_back(MsSince(start));
        mesh_triangles += MeshTriangleCount(*chunk);
    }
    nlohmann::json polygonise_json = StatsToJson(ComputeStats(polygonise_ms));
    polygonise_json["mesh_triangles"] = mesh_triangles;

    // (d) height queries on a fixed grid: cold instance versus warm instance.
    std::vector<float> xs;
    std::vector<float> zs;
    xs.reserve(static_cast<std::size_t>(kGridSide * kGridSide));
    zs.reserve(static_cast<std::size_t>(kGridSide * kGridSide));
    for (int j = 0; j < kGridSide; ++j) {
        for (int i = 0; i < kGridSide; ++i) {
            xs.push_back(spawn_x + static_cast<float>(i - 16) * 2.0f);
            zs.push_back(spawn_z + static_cast<float>(j - 16) * 2.0f);
        }
    }
    const SHIELD_WorldSystem cold_world(&jobs, nullptr, params, kSeed);
    const std::vector<double> cold_ms = TimeHeightGrid(cold_world, xs, zs);
    const SHIELD_WorldSystem warm_world(&jobs, nullptr, params, kSeed);
    (void)TimeHeightGrid(warm_world, xs, zs); // priming pass, not reported
    const std::vector<double> warm_ms = TimeHeightGrid(warm_world, xs, zs);

    nlohmann::json height_cold = StatsToJson(ComputeStats(cold_ms));
    height_cold["grid"] = "32x32";
    height_cold["first_call_ms"] = cold_ms.empty() ? 0.0 : cold_ms.front();
    nlohmann::json height_warm = StatsToJson(ComputeStats(warm_ms));
    height_warm["grid"] = "32x32";

    // (e) batched shaped heights over the same grid.
    const std::size_t shaped_reps = options.smoke ? kSmokeShapedReps : kFullShapedReps;
    std::vector<float> shaped_out(xs.size(), 0.0f);
    std::vector<double> shaped_ms;
    shaped_ms.reserve(shaped_reps);
    for (std::size_t rep = 0; rep < shaped_reps; ++rep) {
        const Clock::time_point start = Clock::now();
        world.ComputeShapedHeightsAtPositions(
            xs.data(), zs.data(), xs.size(), shaped_out.data(), true);
        shaped_ms.push_back(MsSince(start));
    }
    float shaped_acc = 0.0f;
    for (const float value : shaped_out) {
        shaped_acc += value;
    }
    g_probe_sink_f = shaped_acc;
    nlohmann::json shaped_json = StatsToJson(ComputeStats(shaped_ms));
    shaped_json["grid"] = "32x32";
    shaped_json["positions"] = xs.size();
    shaped_json["reps"] = shaped_reps;

    // (f) generation scaling across explicit and default worker counts.
    nlohmann::json scaling = RunScaling(world, selected);

    // (g) libm micro-loops.
    const nlohmann::json libm = RunLibmSuite(options.smoke ? kSmokeLibmCalls : kFullLibmCalls);

    nlohmann::json report = {
        {"schema", kSchemaId},
        {"mode", options.smoke ? "smoke" : "full"},
        {"compiler", CompilerId()},
        {"cpu", CpuBrand()},
        {"hardware_concurrency", std::thread::hardware_concurrency()},
        {"default_worker_count", default_worker_count},
        {"seed", kSeed},
        {"preset", preset_path.filename().string()},
        {"spawn", {{"x", spawn.x}, {"y", spawn.y}, {"z", spawn.z}}},
        {"requested_chunks", options.chunks},
        {"selected_chunks", selected.size()},
        {"initial_chunk_list_size", candidates.size()},
        {"phases",
         {
             {"generate_step1", StatsToJson(ComputeStats(gen1_ms))},
             {"generate_step4", StatsToJson(ComputeStats(gen4_ms))},
             {"polygonise", polygonise_json},
             {"height_query_cold", height_cold},
             {"height_query_warm", height_warm},
             {"shaped_heights", shaped_json},
         }},
        {"scaling", scaling},
        {"libm", libm},
        {"libm_after_generation", libm_after_generation},
    };
    return report;
}

void PrintStatsRow(const std::string& name, const nlohmann::json& stats) {
    std::cout << std::left << std::setw(22) << name << std::right << std::setw(9)
              << stats.at("samples").get<std::size_t>() << std::setw(13)
              << stats.at("min_ms").get<double>() << std::setw(13)
              << stats.at("median_ms").get<double>() << std::setw(13)
              << stats.at("max_ms").get<double>() << std::setw(13)
              << stats.at("total_ms").get<double>() << "\n";
}

void PrintTable(const nlohmann::json& report) {
    std::cout << "worldgen_probe " << report.at("mode").get<std::string>() << "  "
              << report.at("cpu").get<std::string>() << "\n";
    std::cout << "compiler: " << report.at("compiler").get<std::string>() << "\n";
    std::cout << "hardware_concurrency=" << report.at("hardware_concurrency").get<unsigned>()
              << " default_worker_count=" << report.at("default_worker_count").get<std::size_t>()
              << " selected_chunks=" << report.at("selected_chunks").get<std::size_t>() << "\n\n";

    std::cout << std::fixed << std::setprecision(3);
    std::cout << std::left << std::setw(22) << "phase" << std::right << std::setw(9) << "samples"
              << std::setw(13) << "min_ms" << std::setw(13) << "median_ms" << std::setw(13)
              << "max_ms" << std::setw(13) << "total_ms" << "\n";
    const nlohmann::json& phases = report.at("phases");
    for (const auto& item : phases.items()) {
        PrintStatsRow(item.key(), item.value());
    }

    std::cout << "\n"
              << std::left << std::setw(22) << "scaling" << std::right << std::setw(12)
              << "requested" << std::setw(14) << "worker_count" << std::setw(12) << "chunks"
              << std::setw(13) << "wall_ms" << "\n";
    for (const nlohmann::json& entry : report.at("scaling")) {
        const nlohmann::json& requested = entry.at("requested_workers");
        const std::string requested_text = requested.is_string()
                                               ? requested.get<std::string>()
                                               : std::to_string(requested.get<std::size_t>());
        std::cout << std::left << std::setw(22) << "startup" << std::right << std::setw(12)
                  << requested_text << std::setw(14) << entry.at("worker_count").get<std::size_t>()
                  << std::setw(12) << entry.at("chunks").get<std::size_t>() << std::setw(13)
                  << entry.at("wall_ms").get<double>() << "\n";
    }

    auto print_libm = [](const char* label, const nlohmann::json& libm) {
        std::cout << "\n" << label << " (calls=" << libm.at("calls").get<std::size_t>() << ")\n";
        std::cout << std::left << std::setw(22) << "function" << std::right << std::setw(14)
                  << "total_ms" << std::setw(16) << "ns_per_call" << "\n";
        for (const auto& item : libm.at("functions").items()) {
            std::cout << std::left << std::setw(22) << item.key() << std::right << std::setw(14)
                      << item.value().at("total_ms").get<double>() << std::setw(16)
                      << item.value().at("ns_per_call").get<double>() << "\n";
        }
    };
    print_libm("libm", report.at("libm"));
    print_libm("libm_after_generation (observation)", report.at("libm_after_generation"));
    std::cout << std::defaultfloat;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!ParseArgs(argc, argv, options, error)) {
        std::cerr << error << "\n";
        PrintUsage(std::cerr);
        return 1;
    }

    try {
        const nlohmann::json report = RunProbe(options);

        std::ofstream output(options.json_path);
        if (!output) {
            std::cerr << "cannot open " << options.json_path << " for writing\n";
            return 2;
        }
        output << std::setw(2) << report << "\n";
        output.close();
        if (!output) {
            std::cerr << "failed writing " << options.json_path << "\n";
            return 2;
        }

        PrintTable(report);
    } catch (const std::exception& ex) {
        std::cerr << "worldgen_probe internal error: " << ex.what() << "\n";
        return 2;
    } catch (...) {
        std::cerr << "worldgen_probe internal error: unknown exception\n";
        return 2;
    }
    return 0;
}
