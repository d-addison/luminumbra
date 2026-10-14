#include "core/RuntimeScenarioHarness.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Luminumbra::Client::ScenarioHarness::FarLodHorizonStationCapture;
using Luminumbra::Client::ScenarioHarness::WriteFarLodHorizonAnalysis;

std::vector<FarLodHorizonStationCapture> MakeCaptures() {
    FarLodHorizonStationCapture origin{};
    origin.station.name = "origin";
    origin.file = "origin.ppm";
    // At a 512 m grid intersection the 3000 m disc intersects 132 regions:
    // four quadrants, each with 6 + 6 + 6 + 6 + 5 + 4 nearest-point pairs.
    origin.regions_wanted = 132;
    origin.regions_resident = 132;
    origin.resident_bytes = 4096;
    origin.region_draws = 1;
    origin.far_indices_drawn = 3;

    FarLodHorizonStationCapture translated = origin;
    translated.station.name = "translated";
    translated.file = "translated.ppm";
    translated.camera_world_x = -512.0f;
    translated.camera_world_z = 1024.0f;
    return {origin, translated};
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr
            << "Usage: far_ladder_oracle_writer_fixture <artifact-dir> <complete|incomplete>\n";
        return EXIT_FAILURE;
    }
    const std::string mode = argv[2];
    if (mode != "complete" && mode != "incomplete") {
        std::cerr << "Unknown fixture mode: " << mode << '\n';
        return EXIT_FAILURE;
    }

    try {
        std::vector<FarLodHorizonStationCapture> captures = MakeCaptures();
        if (mode == "incomplete") {
            captures.back().regions_missing = 1;
            --captures.back().regions_resident;
        }
        // File-only production writer call: no window, GL context, or smoke run.
        // These synthetic captures do not establish readiness within 90 seconds.
        WriteFarLodHorizonAnalysis(std::filesystem::path(argv[1]),
                                   "mountains",
                                   0.0,
                                   captures,
                                   captures.size(),
                                   0.0,
                                   0.0,
                                   false,
                                   false);
    } catch (const std::exception& error) {
        std::cerr << "Writer fixture failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
