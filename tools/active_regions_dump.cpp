#include "active_regions_dump.h"

#include "luminumbra_common/core/CaptureContentPaths.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <ostream>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef LUMINUMBRA_ACTIVE_REGIONS_DUMP_ENTRY
#include <iostream>
#endif

namespace Luminumbra::Tools {
namespace {
using nlohmann::json;
using world::ActiveRegionLedger;

std::string Hex(std::string_view bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    // Called only on a decoded envelope: 2 * kMaxFileBytes fits 32-bit size_t.
    result.reserve(bytes.size() * 2);
    for (const char character : bytes) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

json Register(const std::map<world::RegionKey, std::uint64_t>& values,
              const world::RegionKey& key) {
    const auto found = values.find(key);
    return found == values.end() ? json(nullptr) : json(found->second);
}

std::string Document(const ActiveRegionLedger& ledger, std::string_view bytes) {
    json anchor = nullptr;
    const auto& position = ledger.local_anchor();
    if (position) {
        anchor = json::array({position->x, position->y, position->z});
    }
    // Decode validated these fixed-layout offsets and the complete TLV envelope.
    // Retain its bytes rather than re-encoding historical versions/projections.
    const auto extension_begin = ActiveRegionLedger::kHeaderBytes +
                                 ledger.records().size() * ActiveRegionLedger::kRecordBytes;
    const auto extensions = bytes.substr(extension_begin, bytes.size() - 8 - extension_begin);
    const auto version = static_cast<unsigned char>(bytes[4]) |
                         (static_cast<unsigned>(static_cast<unsigned char>(bytes[5])) << 8);
    const json header = {{"schema", "luminumbra.active_regions_dump.v1"},
                         {"source_version", version},
                         {"source_bytes", bytes.size()},
                         {"tick", ledger.tick()},
                         {"header_tick", ledger.header_tick()},
                         {"config",
                          {{"work_limit", ledger.config().work_limit},
                           {"hold_ticks", ledger.config().hold_ticks},
                           {"reduced_shift", ledger.config().reduced_shift}}},
                         {"local_anchor", anchor},
                         {"next_persistent_id", ledger.next_persistent_id()},
                         {"extension_envelope_hex", Hex(extensions)}};
    std::string result = header.dump();
    result.pop_back();
    result += ",\"records\":[";
    bool first = true;
    // At most kMaxRegions records, each below 2048 bytes (fixed keys, bools and
    // <= 20 decimal digits per integer). Header is below 4096 + 2*kMaxFileBytes.
    // Serialize one at a time instead of retaining a second whole-ledger JSON tree.
    for (const auto& [key, record] : ledger.records()) {
        if (!first) {
            result += ',';
        }
        first = false;
        const json entry = {{"x", key.x},
                            {"z", key.z},
                            {"visited", record.visited},
                            {"edited", record.edited},
                            {"pinned", record.pinned},
                            {"populated", record.populated},
                            {"wake_pending", record.wake_pending},
                            {"state", static_cast<unsigned>(record.state)},
                            {"cadence_shift", record.cadence_shift},
                            {"cadence_phase", record.cadence_phase},
                            {"first_active", record.first_active},
                            {"last_proximity", record.last_proximity},
                            {"last_edit", record.last_edit},
                            {"last_ticked", record.last_ticked},
                            {"frozen_at", record.frozen_at},
                            {"last_save", record.last_save},
                            {"last_decision", record.last_decision},
                            {"pressure", record.pressure},
                            {"over_hold", record.over_hold},
                            {"under_hold", record.under_hold},
                            {"water_cursor", record.water_cursor},
                            {"frozen_digest", record.frozen_digest},
                            {"energy_age", Register(ledger.energy_ages(), key)},
                            {"water_debt", Register(ledger.water_debts(), key)},
                            {"completed_work", Register(ledger.completed_work(), key)}};
        result += entry.dump();
    }
    result += "]}\n";
    return result;
}

ActiveRegionDumpResult Read(const std::filesystem::path& path) {
    Filesystem::VerifiedFileInput input(path);
    const auto size = input.size();
    std::array<char, 65536> buffer{}; // At most 64 KiB per verified handle read.
    if (size > ActiveRegionLedger::kMaxFileBytes) {
        // The codec identifies a future version before applying its file cap.
        // Preserve that distinction even for a future file exceeding today's cap,
        // without allocating or decoding its body. Six bytes cannot decode as valid.
        const auto count = input.read(buffer);
        ActiveRegionLedger candidate;
        std::string error;
        ActiveRegionLedger::decode(
            std::string_view(buffer.data(), std::min(count, std::size_t{6})), candidate, error);
        return {error == ActiveRegionLedger::kFutureMessage ? ActiveRegionDumpStatus::FutureVersion
                                                            : ActiveRegionDumpStatus::Corrupt,
                {},
                std::move(error)};
    }
    std::string bytes;
    // Allocation is capped by the production file bound (~104 MiB); reads add
    // at most one 64 KiB growth probe beyond the captured size.
    // Size and contents come from the same verified regular-file handle on all platforms.
    bytes.reserve(static_cast<std::size_t>(size));
    while (bytes.size() < size) {
        const auto count = input.read(buffer);
        if (count == 0 || count > size - bytes.size()) {
            return {ActiveRegionDumpStatus::IoError, {}, "Ledger size changed while reading."};
        }
        bytes.append(buffer.data(), count);
    }
    if (input.read(buffer) != 0) {
        return {ActiveRegionDumpStatus::IoError, {}, "Ledger size changed while reading."};
    }
    ActiveRegionLedger ledger;
    std::string error;
    if (!ActiveRegionLedger::decode(bytes, ledger, error)) {
        const auto status = error == ActiveRegionLedger::kFutureMessage
                                ? ActiveRegionDumpStatus::FutureVersion
                                : ActiveRegionDumpStatus::Corrupt;
        return {status, {}, std::move(error)};
    }
    return {ActiveRegionDumpStatus::Ok, Document(ledger, bytes), {}};
}

bool IsAbsent(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    auto candidate = Filesystem::AbsolutePath(path, error);
    // This failed-open diagnostic walk is bounded by the verified reader's path
    // limit. It never authorizes a read. Missing suffixes are normal only when
    // they end at an existing directory, not a dangling link or a wrong-type parent.
    if (error || candidate.native().size() > 32767) {
        return false;
    }
    bool missing_suffix = false;
    while (!candidate.empty()) {
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            return false;
        }
        if (status.type() != std::filesystem::file_type::not_found) {
            return missing_suffix && status.type() == std::filesystem::file_type::directory;
        }
        missing_suffix = true;
        const auto parent = candidate.parent_path();
        if (parent == candidate) {
            return false;
        }
        candidate = parent;
    }
    return false;
}
} // namespace

ActiveRegionDumpResult DumpActiveRegions(const std::filesystem::path& path) {
    try {
        return Read(path);
    } catch (const std::filesystem::filesystem_error& error) {
        if (error.code() == std::errc::no_such_file_or_directory && IsAbsent(path)) {
            return {ActiveRegionDumpStatus::Absent, {}, "Active-region ledger is absent."};
        }
        return {ActiveRegionDumpStatus::IoError, {}, error.what()};
    } catch (const std::exception& error) {
        return {ActiveRegionDumpStatus::IoError, {}, error.what()};
    }
}

int ActiveRegionsDumpCommand(const std::filesystem::path& path,
                             std::ostream& output,
                             std::ostream& errors) {
    const auto result = DumpActiveRegions(path);
    if (result.status != ActiveRegionDumpStatus::Ok) {
        errors << result.error << '\n';
        return static_cast<int>(result.status);
    }
    output << result.document;
    output.flush();
    if (!output) {
        errors << "Cannot write active-region ledger dump.\n";
        return static_cast<int>(ActiveRegionDumpStatus::IoError);
    }
    return 0;
}
} // namespace Luminumbra::Tools

#ifdef LUMINUMBRA_ACTIVE_REGIONS_DUMP_ENTRY
// Usage: luminumbra_active_regions_dump <save>/chunks/region/active-regions.arl
// Native argv preserves UTF-16 paths on Windows, including drive and UNC paths.
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc != 2) {
        std::cerr << "usage: luminumbra_active_regions_dump <ledger-file>\n";
        return 2;
    }
    try {
        return Luminumbra::Tools::ActiveRegionsDumpCommand(
            std::filesystem::path(argv[1]), std::cout, std::cerr);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return static_cast<int>(Luminumbra::Tools::ActiveRegionDumpStatus::IoError);
    }
}
#endif
