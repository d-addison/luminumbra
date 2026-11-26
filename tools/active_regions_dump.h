#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>

namespace Luminumbra::Tools {

// Command exit codes; usage errors use 2. Invalid inputs never produce a document.
enum class ActiveRegionDumpStatus : int {
    Ok = 0,
    Absent = 3,
    Corrupt = 4,
    FutureVersion = 5,
    IoError = 6
};

struct ActiveRegionDumpResult {
    ActiveRegionDumpStatus status = ActiveRegionDumpStatus::IoError;
    std::string document;
    std::string error;
};

// Read an explicit ledger file, without loading a world or changing its source.
// JSON schema luminumbra.active_regions_dump.v1 uses ordered records and exact
// integer JSON numbers. Sparse extension registers are null when absent (zero
// energy age is present). extension_envelope_hex retains the validated original
// extension length/TLV bytes, including the frozen digest projection declaration.
// State values follow RegionState: 0 active, 1 reduced, 2 frozen.
// No frozen digest calculation, scheduler, recovery or migration is invoked here.
[[nodiscard]] ActiveRegionDumpResult DumpActiveRegions(const std::filesystem::path& path);

// stdout receives one complete JSON line only after successful validation.
int ActiveRegionsDumpCommand(const std::filesystem::path& path,
                             std::ostream& output,
                             std::ostream& errors);

} // namespace Luminumbra::Tools
