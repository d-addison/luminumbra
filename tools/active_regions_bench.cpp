// Standalone observational fixture. stdout contains one complete JSON document.
#include "active_regions_bench.h"
#include "StationDiscoveryArguments.h"

#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/Log.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace {
namespace Measure = Luminumbra::RegionMeasurement;
namespace Arguments = Luminumbra::StationDiscoveryCli;
using nlohmann::json;

json SchedulerJson(const Measure::LedgerObservation& sample) {
    return {{"elapsed_ms", sample.elapsed_ms},
            {"records", sample.records},
            {"populated_records", sample.populated_records},
            {"empty_unowned_records", sample.empty_unowned_records},
            {"encoded_bytes", sample.encoded_bytes},
            {"record_value_bytes", sample.record_value_bytes},
            {"priority_comparisons", sample.priority_comparisons},
            {"due", sample.due},
            {"transitions", sample.transitions},
            {"evicted", sample.evicted},
            {"digest", sample.digest}};
}

json SaveJson(const Measure::SaveObservation& sample) {
    const auto& work = sample.work;
    return {{"elapsed_ms", sample.elapsed_ms},
            {"chunks", sample.chunks},
            {"payload_value_bytes", sample.payload_value_bytes},
            {"ledger_records", sample.ledger_records},
            {"ledger_encoded_bytes", sample.ledger_encoded_bytes},
            {"snapshot_copies", sample.snapshot_copies},
            {"world_decodes", work.world_decodes},
            {"chunk_decodes", work.chunk_decodes},
            {"owners_serialized", work.owners_serialized},
            {"clean_siblings_serialized", work.clean_siblings_serialized},
            {"owners_rewritten", work.owners_rewritten},
            {"regions_rewritten", work.regions_rewritten},
            {"bytes_compressed", work.bytes_compressed}};
}

int Run(int argc, Arguments::ArgumentChar** argv) {
    std::filesystem::path root, work_dir;
    std::uint32_t records = 8, chunks = 4;
    bool have_root = false, have_work = false, have_records = false, have_chunks = false;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing option value");
        }
        const auto flag = Arguments::ArgumentText(argv[index]);
        if (flag == "--root" && !have_root) {
            root = Arguments::ArgumentPath(argv[index + 1]);
            have_root = true;
        } else if (flag == "--work-dir" && !have_work) {
            work_dir = Arguments::ArgumentPath(argv[index + 1]);
            have_work = true;
        } else if (flag == "--records" && !have_records) {
            records = Measure::ParseSize(Arguments::ArgumentText(argv[index + 1]));
            have_records = true;
        } else if (flag == "--chunks" && !have_chunks) {
            chunks = Measure::ParseSize(Arguments::ArgumentText(argv[index + 1]));
            have_chunks = true;
        } else {
            throw std::invalid_argument("unknown or repeated option");
        }
    }
    Measure::ValidateSizes(records, chunks);
    if (!have_root || root.empty() || !have_work || work_dir.empty()) {
        throw std::invalid_argument("--root and a fresh --work-dir are required");
    }
    root = Luminumbra::Filesystem::AbsolutePath(root);
    work_dir = Luminumbra::Filesystem::AbsolutePath(work_dir);
    // GameSession's legacy runtime-root string is locale encoded on Windows.
    // Resolve native argv paths first, then use an ASCII relative runtime root.
    // This process owns its cwd; saves retain their absolute native path.
    std::filesystem::current_path(root);
    // Reserve the leaf atomically. Failure retains any partial fixture for inspection.
    if (!std::filesystem::create_directory(work_dir)) {
        throw std::invalid_argument("work directory already exists");
    }
    // The engine logger is created lazily at trace level and ignores the global
    // spdlog level, so create it now and silence it before anything can log.
    // stdout must carry only the JSON document.
    spdlog::set_level(spdlog::level::off);
    Log::GetCoreLogger()->set_level(spdlog::level::off);
    json result{{"schema", "luminumbra.active_region_scale_samples.v1"},
                {"records_requested", records},
                {"chunks_requested", chunks},
                {"scheduler", json::object()},
                {"saves", json::object()}};
    {
        auto ledger = Measure::MakeLedger(records);
        for (std::size_t index = 0; index < Measure::kSchedulerPhases.size(); ++index) {
            result["scheduler"][Measure::kSchedulerPhases[index]] =
                SchedulerJson(Measure::MeasurePhase(ledger, index));
        }
    }
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    Luminumbra::world::GameSession session;
    session.SetRootPath("./");
    session.SetSaveRootPath(work_dir);
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetWaterHighResEnabled(false);
    session.SetWeatherRainEnabled(false);
    if (!session.CreateWorld("Region measurements", "1337", "default")) {
        throw std::runtime_error("measurement world creation refused");
    }
    const auto payloads = Measure::PopulateSave(session, chunks, records);
    for (std::size_t index = 0; index < Measure::kSavePhases.size(); ++index) {
        if (index == 2) {
            payloads.front()->sdf_data.front() -= 0.25f;
            payloads.front()->mark_voxel_data_dirty();
        }
        result["saves"][Measure::kSavePhases[index]] =
            SaveJson(Measure::MeasureSave(session, chunks));
    }
    std::cout << result.dump() << '\n' << std::flush;
    if (!std::cout) {
        throw std::runtime_error("measurement output failed");
    }
    return 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        return Run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Measurement refused: " << error.what() << '\n';
        return 1;
    }
}
