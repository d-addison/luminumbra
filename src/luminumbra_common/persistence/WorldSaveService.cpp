#include "WorldSaveService.h"

#include "../ecs/EntitySnapshot.h" //  plant entity snapshot persistence
#include "../fields/EnergyFieldState.h"
#include "SaveTransaction.h"
#include "WorldPersistenceRoundtrip.h"
#include "world/FarLodStore.h"

#include "nlohmann/json.hpp"

#include <lz4.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Luminumbra::Persistence {
namespace {

constexpr const char* kChunksDirectoryName = "chunks";
constexpr const char* kWorldStateFileName = "world-state.json";
constexpr const char* kWorldStateBackupFileName = "world-state.json.bak";
constexpr const char* kRegionDirectoryName = "region";
constexpr const char* kWorldManifestFileName = "world-manifest.json";
constexpr const char* kWorldManifestSchema = "luminumbra.persistence.world_manifest.v1";
constexpr const char* kRegionFileExtension = ".lmr";

// Container constants (the deterministic runtime contract section 3).
constexpr char kRegionMagic[4] = {'L', 'M', 'R', '1'};
constexpr std::uint16_t kRegionVersion = WorldSaveService::kContainerVersion;
// Header: magic u32 | version u16 | record_count u16.
constexpr std::size_t kRegionFileHeaderSize = 8;
// Record header: u64 id | u8 lod_level | u8 flags | u32 uncompressed_size |
// u32 compressed_size.
constexpr std::size_t kRecordHeaderSize = 18;

constexpr std::uint8_t kRecordFlagEdited = 0x01;
constexpr std::uint8_t kRecordFlagWaterPresent = 0x02;

std::atomic<std::uint64_t>& RegionTempSequence() {
    static std::atomic<std::uint64_t> sequence{0};
    return sequence;
}

std::atomic<bool>& InterruptBeforeRegionReplaceForTesting() {
    static std::atomic<bool> interrupt{false};
    return interrupt;
}

// Number of POSIX directory flushes to let succeed before the next one fails;
// negative disables the seam.
std::atomic<int>& DirectoryFlushFailureCountdownForTesting() {
    static std::atomic<int> countdown{-1};
    return countdown;
}

#if !defined(_WIN32)
// True when the injected flush failure fires on this flush.
bool ConsumeDirectoryFlushFailureForTesting() {
    std::atomic<int>& countdown = DirectoryFlushFailureCountdownForTesting();
    int current = countdown.load(std::memory_order_acquire);
    while (current >= 0) {
        const int next = current == 0 ? -1 : current - 1;
        if (countdown.compare_exchange_weak(
                current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return current == 0;
        }
    }
    return false;
}
#endif

using ActiveRegionsReplaceHook = bool (*)();
ActiveRegionsReplaceHook& BeforeActiveRegionsReplaceForTesting() {
    static ActiveRegionsReplaceHook hook = nullptr;
    return hook;
}

#ifndef NDEBUG
// Production pair saves are synchronous on the process's host thread. Detect
// concurrent or reentrant callers instead of silently serializing a violated
// ownership contract. This guard does not wait and is absent in release builds.
class PairedSaveGuard {
public:
    PairedSaveGuard() {
        const bool already_saving = saving().test_and_set(std::memory_order_acquire);
        assert(!already_saving && "Clock/ledger saves must not overlap");
    }
    ~PairedSaveGuard() {
        saving().clear(std::memory_order_release);
    }

private:
    static std::atomic_flag& saving() {
        static std::atomic_flag flag = ATOMIC_FLAG_INIT;
        return flag;
    }
};
#endif

// One record of an LMR1 region file kept in its on-disk (compressed) form so
// untouched records survive a merge byte-for-byte without a decode pass.
struct RegionRecord {
    std::uint64_t id = 0;
    std::uint8_t lod_level = 0;
    std::uint8_t flags = 0;
    std::uint32_t uncompressed_size = 0;
    std::string compressed_payload;
};

int FloorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    const int remainder = value % divisor;
    return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

void AppendU16(std::string& buffer, std::uint16_t value) {
    buffer.push_back(static_cast<char>(value & 0xffu));
    buffer.push_back(static_cast<char>((value >> 8) & 0xffu));
}

void AppendU32(std::string& buffer, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        buffer.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

void AppendU64(std::string& buffer, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        buffer.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

std::uint16_t ReadU16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::uint32_t ReadU32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t ReadU64(const unsigned char* bytes) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | static_cast<std::uint64_t>(bytes[i]);
    }
    return value;
}

void AddError(std::vector<std::string>* errors, std::string message) {
    if (errors) {
        errors->push_back(std::move(message));
    }
}

bool ReadAmbientAnchor(const nlohmann::json& metadata,
                       std::optional<Vec3>& anchor,
                       std::string& error) {
    anchor.reset();
    if (!metadata.contains("simulationTick") && !metadata.contains("calendar"))
        return true;
    // Older clock snapshots used spawnPoint itself as the ambient anchor.
    const char* key = metadata.contains("ambientFieldAnchor") ? "ambientFieldAnchor" : "spawnPoint";
    if (!metadata.contains(key))
        return true;
    const auto& value = metadata.at(key);
    Vec3 point{};
    for (int i = 0; i < 3; ++i) {
        const char* axis = i == 0 ? "x" : (i == 1 ? "y" : "z");
        if (!value.is_object() || value.size() != 3 || !value.contains(axis) ||
            !value.at(axis).is_number()) {
            error = "Corrupt world metadata: invalid ambient field anchor.";
            return false;
        }
        point[i] = value.at(axis).get<float>();
        if (!std::isfinite(point[i])) {
            error = "Corrupt world metadata: ambient field anchor must be finite.";
            return false;
        }
    }
    anchor = point;
    return true;
}

std::uint64_t CurrentProcessIdValue() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::filesystem::path NextRegionTempPath(const std::filesystem::path& destination) {
    const std::uint64_t sequence =
        RegionTempSequence().fetch_add(1u, std::memory_order_relaxed) + 1u;
    std::filesystem::path temp = destination;
    temp += ".tmp." + std::to_string(CurrentProcessIdValue()) + "." + std::to_string(sequence);
    return temp;
}

void RemoveTemporaryFile(const std::filesystem::path& path) {
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
}

// Reclaim staging files abandoned by a terminated save. Saves are serialized per
// destination by construction, so any staging file that already exists for this
// destination was left by a process that is no longer writing it. Without this,
// every interruption leaks a full ledger copy and repeated crashes exhaust the
// volume. Best effort: a file that cannot be removed is left alone and never
// blocks the save, because staging lives outside every scanned directory.
std::size_t ReclaimAbandonedStaging(const std::filesystem::path& destination) {
    std::error_code list_error;
    const auto directory = destination.parent_path();
    const auto prefix = destination.filename().string() + ".tmp.";
    std::size_t reclaimed = 0;
    std::filesystem::directory_iterator it(directory, list_error);
    if (list_error)
        return 0;
    for (const auto& entry : it) {
        std::error_code kind_error;
        if (!entry.is_regular_file(kind_error) || kind_error)
            continue;
        if (entry.path().filename().string().rfind(prefix, 0) != 0)
            continue;
        std::error_code remove_error;
        if (std::filesystem::remove(entry.path(), remove_error) && !remove_error)
            ++reclaimed;
    }
    return reclaimed;
}

#if defined(_WIN32)

std::string WindowsErrorMessage(DWORD error) {
    return std::error_code(static_cast<int>(error), std::system_category()).message();
}

bool WriteDurableRegionTemp(const std::filesystem::path& destination,
                            const std::string& bytes,
                            std::filesystem::path& out_temp,
                            std::vector<std::string>* errors) {
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 128; ++attempt) {
        out_temp = NextRegionTempPath(destination);
        file = ::CreateFileW(out_temp.c_str(),
                             GENERIC_WRITE,
                             0,
                             nullptr,
                             CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                             nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            AddError(errors,
                     "failed to create temporary region file: " + out_temp.string() + ": " +
                         WindowsErrorMessage(error));
            return false;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        AddError(errors,
                 "failed to allocate a unique temporary region file for: " + destination.string());
        return false;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(0xffffffffu)));
        DWORD written = 0;
        if (!::WriteFile(file, bytes.data() + offset, requested, &written, nullptr) ||
            written == 0) {
            DWORD error = ::GetLastError();
            if (error == ERROR_SUCCESS) {
                error = ERROR_WRITE_FAULT;
            }
            ::CloseHandle(file);
            RemoveTemporaryFile(out_temp);
            AddError(errors,
                     "failed to write temporary region file: " + out_temp.string() + ": " +
                         WindowsErrorMessage(error));
            return false;
        }
        offset += written;
    }

    if (!::FlushFileBuffers(file)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(file);
        RemoveTemporaryFile(out_temp);
        AddError(errors,
                 "failed to durably flush temporary region file: " + out_temp.string() + ": " +
                     WindowsErrorMessage(error));
        return false;
    }
    if (!::CloseHandle(file)) {
        const DWORD error = ::GetLastError();
        RemoveTemporaryFile(out_temp);
        AddError(errors,
                 "failed to close temporary region file: " + out_temp.string() + ": " +
                     WindowsErrorMessage(error));
        return false;
    }
    return true;
}

bool ReplaceRegionFileAtomically(const std::filesystem::path& temp,
                                 const std::filesystem::path& destination,
                                 std::vector<std::string>* errors) {
    // Temp bytes were flushed before publication. Indexed saves additionally retain a flushed
    // undo copy until commit; recovery handles an interrupted replacement. ReplaceFileW's
    // REPLACEFILE_WRITE_THROUGH flag is unsupported, so use the supported same-volume move.
    if (::MoveFileExW(temp.c_str(),
                      destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    const DWORD error = ::GetLastError();
    AddError(errors,
             "failed to atomically install region file: " + destination.string() + ": " +
                 WindowsErrorMessage(error));
    return false;
}

#else

std::string PosixErrorMessage(int error) {
    return std::error_code(error, std::generic_category()).message();
}

bool WriteDurableRegionTemp(const std::filesystem::path& destination,
                            const std::string& bytes,
                            std::filesystem::path& out_temp,
                            std::vector<std::string>* errors) {
    int file = -1;
    for (int attempt = 0; attempt < 128; ++attempt) {
        out_temp = NextRegionTempPath(destination);
        file = ::open(out_temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (file >= 0) {
            break;
        }
        if (errno != EEXIST) {
            const int error = errno;
            AddError(errors,
                     "failed to create temporary region file: " + out_temp.string() + ": " +
                         PosixErrorMessage(error));
            return false;
        }
    }
    if (file < 0) {
        AddError(errors,
                 "failed to allocate a unique temporary region file for: " + destination.string());
        return false;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const std::size_t requested = std::min<std::size_t>(
            remaining, static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)()));
        const ssize_t written = ::write(file, bytes.data() + offset, requested);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            const int error = written < 0 ? errno : EIO;
            ::close(file);
            RemoveTemporaryFile(out_temp);
            AddError(errors,
                     "failed to write temporary region file: " + out_temp.string() + ": " +
                         PosixErrorMessage(error));
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }

    while (::fsync(file) != 0) {
        if (errno == EINTR) {
            continue;
        }
        const int error = errno;
        ::close(file);
        RemoveTemporaryFile(out_temp);
        AddError(errors,
                 "failed to durably flush temporary region file: " + out_temp.string() + ": " +
                     PosixErrorMessage(error));
        return false;
    }
    if (::close(file) != 0) {
        const int error = errno;
        RemoveTemporaryFile(out_temp);
        AddError(errors,
                 "failed to close temporary region file: " + out_temp.string() + ": " +
                     PosixErrorMessage(error));
        return false;
    }
    return true;
}

bool ReplaceRegionFileAtomically(const std::filesystem::path& temp,
                                 const std::filesystem::path& destination,
                                 std::vector<std::string>* errors) {
    // POSIX rename replaces an existing same-filesystem destination atomically.
    // LMR1 staging is beside the live file; ledger staging is in the save root,
    // outside the chunks scan. A cross-filesystem rename fails without replacing.
    if (std::rename(temp.c_str(), destination.c_str()) != 0) {
        const int error = errno;
        AddError(errors,
                 "failed to atomically replace region file: " + destination.string() + ": " +
                     PosixErrorMessage(error));
        return false;
    }

    // Persist the directory entry update as well as the replacement bytes.
    // Without this fsync, rename is atomically visible to live processes but a
    // power loss may resurrect the prior directory entry on some filesystems.
    const std::filesystem::path parent =
        destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
    if (ConsumeDirectoryFlushFailureForTesting()) {
        AddError(errors,
                 "region file was replaced but its directory flush failed (test seam): " +
                     parent.string());
        return false;
    }
    int directory_flags = O_RDONLY;
#if defined(O_DIRECTORY)
    directory_flags |= O_DIRECTORY;
#endif
    const int directory = ::open(parent.c_str(), directory_flags);
    if (directory < 0) {
        const int error = errno;
        AddError(
            errors,
            "region file was replaced but its directory could not be opened for durable flush: " +
                parent.string() + ": " + PosixErrorMessage(error));
        return false;
    }
    while (::fsync(directory) != 0) {
        if (errno == EINTR)
            continue;
        const int error = errno;
        ::close(directory);
        AddError(errors,
                 "region file was replaced but its directory flush failed: " + parent.string() +
                     ": " + PosixErrorMessage(error));
        return false;
    }
    if (::close(directory) != 0) {
        const int error = errno;
        AddError(errors,
                 "region directory close failed after durable replacement: " + parent.string() +
                     ": " + PosixErrorMessage(error));
        return false;
    }
    return true;
}

// Flushes the directory that holds a newly created entry so the entry survives
// power loss.
bool FlushDirectoryEntry(const std::filesystem::path& directory_path,
                         std::vector<std::string>* errors) {
    if (ConsumeDirectoryFlushFailureForTesting()) {
        AddError(errors, "directory flush failed (test seam): " + directory_path.string());
        return false;
    }
    int directory_flags = O_RDONLY;
#if defined(O_DIRECTORY)
    directory_flags |= O_DIRECTORY;
#endif
    const int directory = ::open(directory_path.c_str(), directory_flags);
    if (directory < 0) {
        const int error = errno;
        AddError(errors,
                 "cannot open directory for durable flush: " + directory_path.string() + ": " +
                     PosixErrorMessage(error));
        return false;
    }
    while (::fsync(directory) != 0) {
        if (errno == EINTR) {
            continue;
        }
        const int error = errno;
        ::close(directory);
        AddError(errors,
                 "directory flush failed: " + directory_path.string() + ": " +
                     PosixErrorMessage(error));
        return false;
    }
    if (::close(directory) != 0) {
        const int error = errno;
        AddError(errors,
                 "directory close failed after flush: " + directory_path.string() + ": " +
                     PosixErrorMessage(error));
        return false;
    }
    return true;
}

#endif

thread_local WorldSaveService::SaveFactsScope* g_active_facts = nullptr;

// Every member replacement funnels through here so tests can observe real publication sites.
bool ReplaceMember(const std::filesystem::path& temp,
                   const std::filesystem::path& destination,
                   std::vector<std::string>* errors) {
    report_snapshot_step("before-replace", destination);
    if (g_active_facts != nullptr)
        g_active_facts->invalidate_presence_facts();
    if (!ReplaceRegionFileAtomically(temp, destination, errors))
        return false;
    report_snapshot_step("after-replace", destination);
    return true;
}

bool CompressPayload(const std::string& payload,
                     RegionRecord& record,
                     std::vector<std::string>* errors) {
    if (payload.size() > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) {
        AddError(errors, "region record payload exceeds the LZ4 input limit");
        return false;
    }
    const int bound = LZ4_compressBound(static_cast<int>(payload.size()));
    std::string compressed(static_cast<std::size_t>(bound), '\0');
    const int written = LZ4_compress_default(
        payload.data(), compressed.data(), static_cast<int>(payload.size()), bound);
    if (written <= 0) {
        AddError(errors, "LZ4 compression of a region record failed");
        return false;
    }
    compressed.resize(static_cast<std::size_t>(written));
    record.uncompressed_size = static_cast<std::uint32_t>(payload.size());
    record.compressed_payload = std::move(compressed);
    return true;
}

bool DecompressPayload(const RegionRecord& record,
                       std::string& out_payload,
                       std::vector<std::string>* errors) {
    // Bound allocation before trusting an on-disk length or narrowing to LZ4's int API.
    constexpr std::uint32_t kMaxRecordBytes = 64u * 1024u * 1024u;
    if (record.uncompressed_size == 0 || record.uncompressed_size > kMaxRecordBytes ||
        record.compressed_payload.empty() || record.compressed_payload.size() > kMaxRecordBytes) {
        AddError(errors, "corrupt region record payload length");
        return false;
    }
    out_payload.assign(record.uncompressed_size, '\0');
    const int produced = LZ4_decompress_safe(record.compressed_payload.data(),
                                             out_payload.data(),
                                             static_cast<int>(record.compressed_payload.size()),
                                             static_cast<int>(record.uncompressed_size));
    if (produced < 0 || static_cast<std::uint32_t>(produced) != record.uncompressed_size) {
        AddError(errors, "LZ4 decompression of a region record failed");
        return false;
    }
    return true;
}

bool ReadRegionFile(const std::filesystem::path& path,
                    std::vector<RegionRecord>& out_records,
                    std::vector<std::string>* errors,
                    const std::stop_token& stop = {}) {
    out_records.clear();

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        AddError(errors, "failed to open region file for reading: " + path.string());
        return false;
    }
    std::ostringstream buffer;
    char read_buffer[64 * 1024];
    while (input) {
        if (stop.stop_requested()) {
            AddError(errors, "Saved-world validation cancelled.");
            return false;
        }
        input.read(read_buffer, sizeof(read_buffer));
        buffer.write(read_buffer, input.gcount());
    }
    if (input.bad()) {
        AddError(errors, "failed to read region file: " + path.string());
        return false;
    }
    const std::string bytes = buffer.str();

    if (bytes.size() < kRegionFileHeaderSize ||
        std::memcmp(bytes.data(), kRegionMagic, sizeof(kRegionMagic)) != 0) {
        AddError(errors, "region file is missing the LMR1 magic: " + path.string());
        return false;
    }
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::uint16_t version = ReadU16(data + 4);
    if (version != kRegionVersion) {
        AddError(errors,
                 version < kRegionVersion ? WorldSaveService::kObsoleteWorldMessage
                                          : "unsupported future LMR1 container version " +
                                                std::to_string(version) + ": " + path.string());
        return false;
    }
    const std::uint16_t record_count = ReadU16(data + 6);

    const std::size_t manifest_bytes = static_cast<std::size_t>(record_count) * kRecordHeaderSize;
    if (bytes.size() < kRegionFileHeaderSize + manifest_bytes) {
        AddError(errors, "region file record manifest is truncated: " + path.string());
        return false;
    }

    std::set<std::pair<u8, u64>> keys;
    out_records.resize(record_count);
    std::size_t payload_offset = kRegionFileHeaderSize + manifest_bytes;
    for (std::uint16_t i = 0; i < record_count; ++i) {
        if (stop.stop_requested()) {
            AddError(errors, "Saved-world validation cancelled.");
            out_records.clear();
            return false;
        }
        const unsigned char* header =
            data + kRegionFileHeaderSize + static_cast<std::size_t>(i) * kRecordHeaderSize;
        RegionRecord& record = out_records[i];
        record.id = ReadU64(header);
        record.lod_level = header[8];
        record.flags = header[9];
        if (record.lod_level > 2 || (record.flags & ~3u) != 0u ||
            !keys.emplace(record.lod_level, record.id).second) {
            AddError(errors, "corrupt or unknown region record header: " + path.string());
            out_records.clear();
            return false;
        }
        record.uncompressed_size = ReadU32(header + 10);
        const std::uint32_t compressed_size = ReadU32(header + 14);
        if (payload_offset + compressed_size > bytes.size()) {
            AddError(errors, "region file record payload is truncated: " + path.string());
            out_records.clear();
            return false;
        }
        record.compressed_payload.assign(bytes.data() + payload_offset, compressed_size);
        payload_offset += compressed_size;
    }
    if (payload_offset != bytes.size()) {
        AddError(errors, "region file has trailing bytes after the last record: " + path.string());
        out_records.clear();
        return false;
    }
    return true;
}

bool WriteRegionFile(const std::filesystem::path& path,
                     std::vector<RegionRecord>& records,
                     std::vector<std::string>* errors) {
    if (records.size() > 0xffffu) {
        AddError(errors, "region file exceeds the 65535-record limit: " + path.string());
        return false;
    }

    // Deterministic record order: lod_level ascending, then id ascending.
    std::sort(records.begin(), records.end(), [](const RegionRecord& lhs, const RegionRecord& rhs) {
        if (lhs.lod_level != rhs.lod_level) {
            return lhs.lod_level < rhs.lod_level;
        }
        return lhs.id < rhs.id;
    });

    std::string bytes;
    std::size_t payload_bytes = 0;
    for (const RegionRecord& record : records) {
        payload_bytes += record.compressed_payload.size();
    }
    bytes.reserve(kRegionFileHeaderSize + records.size() * kRecordHeaderSize + payload_bytes);

    bytes.append(kRegionMagic, sizeof(kRegionMagic));
    AppendU16(bytes, kRegionVersion);
    AppendU16(bytes, static_cast<std::uint16_t>(records.size()));
    for (const RegionRecord& record : records) {
        AppendU64(bytes, record.id);
        bytes.push_back(static_cast<char>(record.lod_level));
        bytes.push_back(static_cast<char>(record.flags));
        AppendU32(bytes, record.uncompressed_size);
        AppendU32(bytes, static_cast<std::uint32_t>(record.compressed_payload.size()));
    }
    for (const RegionRecord& record : records) {
        bytes.append(record.compressed_payload);
    }

    std::filesystem::path temp_path;
    if (!WriteDurableRegionTemp(path, bytes, temp_path, errors)) {
        return false;
    }

    // Test-only crash seam: at this point the replacement bytes are complete
    // and durable, while the prior live LMR1 file has not been touched.
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel)) {
        RemoveTemporaryFile(temp_path);
        AddError(errors,
                 "region replacement interrupted by test seam before replacing: " + path.string());
        return false;
    }

    if (!ReplaceMember(temp_path, path, errors)) {
        RemoveTemporaryFile(temp_path);
        return false;
    }
    return true;
}

// Current lod-0 records retain canonical in-memory chunk serialization.
std::string SerializeSingleChunkSnapshot(const std::shared_ptr<Chunk>& chunk) {
    WorldStreamingState single;
    single.insert_chunk(chunk);
    return SerializeWorldStreamingStateSnapshotJson(single);
}

std::shared_ptr<Chunk> DecodeChunkRecord(const RegionRecord& record,
                                         const std::filesystem::path& path,
                                         std::vector<std::string>* errors) {
    std::string payload;
    if (!DecompressPayload(record, payload, errors)) {
        return nullptr;
    }

    WorldStreamingState single;
    std::vector<std::string> load_errors;
    if (!LoadWorldStreamingStateSnapshotJson(payload, single, load_errors)) {
        for (std::string& error : load_errors) {
            AddError(errors, std::move(error));
        }
        AddError(errors, "failed to decode chunk record payload: " + path.string());
        return nullptr;
    }
    const auto chunks = single.snapshot_chunks();
    if (chunks.size() != 1 || !chunks.front()) {
        AddError(errors,
                 "chunk record payload does not contain exactly one chunk: " + path.string());
        return nullptr;
    }
    if (chunks.front()->get_id() != record.id) {
        AddError(errors, "chunk record id does not match its payload: " + path.string());
        return nullptr;
    }
    // Save-dirty is transient, but the container's sticky edited flag is the
    // durable authority bit. Pristine cached chunks stay regenerable.
    if ((record.flags & kRecordFlagEdited) != 0u) {
        chunks.front()->mark_sdf_loaded_or_edited();
    } else {
        chunks.front()->mark_sdf_generated_current_params();
    }
    return chunks.front();
}

std::uint8_t ChunkRecordFlags(const Chunk& chunk, std::uint8_t previous_flags) {
    std::uint8_t flags = 0;
    // Edited/authoritative is sticky: once a chunk record was persisted with
    // post-generation edits it stays authoritative even after the in-memory
    // dirty flag is cleared by the save.
    if (chunk.sdf_provenance() == ChunkSdfProvenance::LoadedOrEdited ||
        (previous_flags & kRecordFlagEdited) != 0) {
        flags |= kRecordFlagEdited;
    }
    if (chunk.has_water_sim.load(std::memory_order_acquire) || !chunk.water_mesh_vertices.empty()) {
        flags |= kRecordFlagWaterPresent;
    }
    return flags;
}

bool ParseRegionFileName(const std::filesystem::path& path, int& rx, int& rz) {
    if (path.extension() != kRegionFileExtension)
        return false;
    const std::string stem = path.stem().string();
    if (stem.rfind("r.", 0) != 0)
        return false;
    const auto split = stem.find('.', 2);
    if (split == std::string::npos)
        return false;
    const char* begin = stem.data();
    const auto x = std::from_chars(begin + 2, begin + split, rx);
    const auto z = std::from_chars(begin + split + 1, begin + stem.size(), rz);
    return x.ec == std::errc{} && x.ptr == begin + split && z.ec == std::errc{} &&
           z.ptr == begin + stem.size() &&
           stem == "r." + std::to_string(rx) + "." + std::to_string(rz);
}

bool WriteWorldManifest(const std::filesystem::path& manifest_path,
                        std::vector<std::string>* errors) {
    const nlohmann::json manifest = {{"schema", kWorldManifestSchema},
                                     {"container", "LMR1"},
                                     {"container_version", kRegionVersion}};

    const std::string bytes = manifest.dump(4) + "\n";
    std::filesystem::path temp;
    if (!WriteDurableRegionTemp(manifest_path, bytes, temp, errors))
        return false;
    if (!ReplaceMember(temp, manifest_path, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
    return true;
}

// Metadata gate shared by load_world and member_digest. Returns an empty string when the file is
// absent or valid, else the rejection message. Malformed JSON throws like before.
std::string CheckMetadataFile(const std::filesystem::path& save_dir,
                              std::int64_t* snapshot_format = nullptr) {
    const auto metadata_path = save_dir / "world_info.json";
    if (!std::filesystem::exists(metadata_path))
        return {};
    std::ifstream input(metadata_path, std::ios::binary);
    const auto metadata = nlohmann::json::parse(input);
    if (!metadata.is_object())
        return "corrupt world metadata: expected an object";
    // Old worlds can contain only metadata: no chunk container exists yet
    // to reveal their version. Refuse them before the clean-miss path.
    if (!metadata.contains("container_version"))
        return WorldSaveService::kObsoleteWorldMessage;
    const auto& version = metadata.at("container_version");
    if (!version.is_number_integer())
        return "corrupt world metadata container version";
    if (version != WorldSaveService::kContainerVersion) {
        if (version >= 0 && version < WorldSaveService::kContainerVersion)
            return WorldSaveService::kObsoleteWorldMessage;
        if (version > WorldSaveService::kContainerVersion)
            return "unsupported future world metadata container version " + version.dump();
        return "corrupt world metadata container version";
    }
    if (metadata.contains("simulationTick") || metadata.contains("calendar")) {
        world::WorldClock clock;
        std::string error;
        std::optional<Vec3> anchor;
        if (!world::WorldClock::from_metadata(metadata, clock, error) ||
            !ReadAmbientAnchor(metadata, anchor, error))
            return error;
    }
    if (snapshot_format != nullptr) {
        *snapshot_format = 0;
        if (metadata.contains("snapshotFormat") &&
            metadata.at("snapshotFormat").is_number_integer())
            *snapshot_format = metadata.at("snapshotFormat").get<std::int64_t>();
    }
    return {};
}

// Manifest gate shared by load_world and member_digest (same contract as CheckMetadataFile).
std::string CheckManifestFile(const std::filesystem::path& manifest_path) {
    if (!std::filesystem::exists(manifest_path))
        return {};
    std::ifstream input(manifest_path, std::ios::binary);
    const auto manifest = nlohmann::json::parse(input);
    if (manifest.is_object() && manifest.contains("schema") && manifest.at("schema").is_string()) {
        const auto schema = manifest.at("schema").get<std::string>();
        const std::string prefix = "luminumbra.persistence.world_manifest.v";
        if (schema.rfind(prefix, 0) == 0 && schema != kWorldManifestSchema) {
            unsigned version = 0;
            const auto parsed = std::from_chars(
                schema.data() + prefix.size(), schema.data() + schema.size(), version);
            if (parsed.ec == std::errc{} && parsed.ptr == schema.data() + schema.size()) {
                if (version > 1)
                    return "unsupported future world manifest schema: " + schema;
                if (version == 0)
                    return WorldSaveService::kObsoleteWorldMessage;
            }
        }
    }
    if (!manifest.is_object() || !manifest.contains("schema") ||
        manifest.at("schema") != kWorldManifestSchema || !manifest.contains("container") ||
        manifest.at("container") != "LMR1" || !manifest.contains("container_version") ||
        !manifest.at("container_version").is_number_integer())
        return "corrupt or unknown world manifest identity/version";
    const auto& version = manifest.at("container_version");
    if (version != kRegionVersion) {
        if (version >= 0 && version < kRegionVersion)
            return WorldSaveService::kObsoleteWorldMessage;
        if (version > kRegionVersion)
            return "unsupported future world manifest container version " + version.dump();
        return "corrupt world manifest container version";
    }
    return {};
}

// Fingerprint of a region file's lod-0 record stream: records by ascending id, each as
// u64 id | u8 flags | u32 raw_size | u32 compressed_size | compressed payload. Far-LOD records and
// file order never change it. A file without lod-0 records is not a snapshot member
// (present=false).
MemberDigest Lod0Fingerprint(const std::vector<RegionRecord>& records) {
    std::vector<const RegionRecord*> lod0;
    for (const RegionRecord& record : records) {
        if (record.lod_level == 0)
            lod0.push_back(&record);
    }
    MemberDigest digest;
    if (lod0.empty())
        return digest;
    std::sort(lod0.begin(), lod0.end(), [](const RegionRecord* lhs, const RegionRecord* rhs) {
        return lhs->id < rhs->id;
    });
    std::string stream;
    for (const RegionRecord* record : lod0) {
        AppendU64(stream, record->id);
        stream.push_back(static_cast<char>(record->flags));
        AppendU32(stream, record->uncompressed_size);
        AppendU32(stream, static_cast<std::uint32_t>(record->compressed_payload.size()));
        stream.append(record->compressed_payload);
    }
    digest.present = true;
    digest.bytes = stream.size();
    digest.fnv64 = fnv1a64_bytes(stream.data(), stream.size());
    return digest;
}

// Joins the active transaction for save_dir, else (indexed directory or pending journal) runs an
// implicit one-writer transaction, else is inert and the caller keeps its legacy code path.
// Owner thread only. Construct BEFORE the first member write; call commit() after the last one.
class TransactionScope {
public:
    TransactionScope(const std::filesystem::path& save_dir,
                     const std::vector<std::string>& relative_paths,
                     std::vector<std::string>* errors,
                     bool require_index = false) {
        if (SaveTransaction* active = SaveTransaction::active_for(save_dir)) {
            m_engaged = true;
            m_ok = active->protect(relative_paths, errors);
            return;
        }
        if (!require_index && !snapshot_index_exists(save_dir) && !journal_pending(save_dir))
            return;
        // Refuse a known invalid baseline before begin() creates journal/region directories.
        // Pending recovery must still run first: installed metadata may be a partial new member.
        if (!journal_pending(save_dir)) {
            const auto metadata_error = CheckMetadataFile(save_dir);
            if (!metadata_error.empty()) {
                m_engaged = true;
                m_ok = false;
                AddError(errors, metadata_error);
                return;
            }
        }
        SnapshotIndex previous;
        read_snapshot_index(save_dir, previous, nullptr);
        m_facts = std::make_unique<WorldSaveService::SaveFactsScope>(save_dir);
        m_transaction = std::make_unique<SaveTransaction>(
            save_dir, previous.tick, previous.anchors, &WorldSaveService::member_digest);
        m_engaged = true;
        // Recover first (begin), then refuse to bless a corrupt baseline: an implicit writer must
        // not overwrite a checksum-mismatched member and publish an index that conceals it.
        m_ok = m_transaction->begin(errors) && m_facts->validate(errors) &&
               m_transaction->protect(relative_paths, errors);
    }
    TransactionScope(const TransactionScope&) = delete;
    TransactionScope& operator=(const TransactionScope&) = delete;
    ~TransactionScope() {
        m_transaction.reset(); // rolls back an uncommitted owned transaction first
        if (m_facts)
            m_facts->discard_deferred();
        m_facts.reset();
    }

    // False when protecting the members failed; the writer must return false.
    [[nodiscard]] bool ok() const {
        return m_ok;
    }
    // True when the writer participates in a transaction (joined or owned).
    [[nodiscard]] bool engaged() const {
        return m_engaged;
    }
    [[nodiscard]] bool owns_transaction() const {
        return static_cast<bool>(m_transaction);
    }
    // Owned scopes commit here; joined and inert scopes succeed trivially.
    bool commit(std::vector<std::string>* errors) {
        if (!m_transaction)
            return true;
        const bool committed = m_transaction->commit(errors);
        if (committed)
            m_facts->run_deferred();
        else
            m_facts->discard_deferred();
        return committed;
    }

private:
    bool m_ok = true;
    bool m_engaged = false;
    std::unique_ptr<WorldSaveService::SaveFactsScope> m_facts;
    std::unique_ptr<SaveTransaction> m_transaction;
};

std::string RelativeRegionMember(int rx, int rz) {
    return "chunks/region/r." + std::to_string(rx) + "." + std::to_string(rz) +
           kRegionFileExtension;
}

// Compares every index member with the directory. Returns the offending relative path, or an
// empty string when the directory matches the index exactly. Region digests come from the records
// load_world already read; singleton members are hashed as raw bytes.
std::string VerifySnapshotIndex(const std::filesystem::path& save_dir,
                                const SnapshotIndex& index,
                                const std::map<std::string, MemberDigest>& region_digests) {
    static const char* const kSingletons[] = {"world_info.json",
                                              "aether_state.efs",
                                              "chunks/region/world-manifest.json",
                                              "chunks/region/plant-entities.json",
                                              "chunks/region/active-regions.arl"};
    std::set<std::string> listed;
    for (const SnapshotMember& member : index.members) {
        listed.insert(member.path);
        MemberDigest actual;
        const auto region = region_digests.find(member.path);
        const bool is_region = member.path.rfind("chunks/region/r.", 0) == 0;
        if (is_region) {
            if (region != region_digests.end())
                actual = region->second;
        } else {
            std::uint64_t bytes = 0;
            std::uint64_t fnv64 = 0;
            bool exists = false;
            if (!hash_file(save_dir / member.path, bytes, fnv64, exists, nullptr))
                return member.path;
            actual.present = exists;
            actual.bytes = bytes;
            actual.fnv64 = fnv64;
            if (member.path == "aether_state.efs" && actual.present) {
                std::string error;
                if (!WorldSaveService::member_digest(save_dir, member.path, actual, error))
                    return member.path + ": " + error;
            }
        }
        if (member.present != actual.present)
            return member.path;
        if (member.present && (member.bytes != actual.bytes || member.fnv64 != actual.fnv64))
            return member.path;
    }
    for (const auto& [path, digest] : region_digests) {
        if (digest.present && listed.count(path) == 0)
            return path;
    }
    for (const char* single : kSingletons) {
        if (listed.count(single) != 0)
            continue;
        std::error_code ec;
        if (std::filesystem::exists(save_dir / single, ec))
            return single;
    }
    return {};
}

// Maps <save>/chunks/region/r.<x>.<z>.lmr to (save_dir, relative member path).
bool SnapshotMemberForRegionFile(const std::filesystem::path& region_file,
                                 std::filesystem::path& save_dir,
                                 std::string& relative) {
    int rx = 0;
    int rz = 0;
    if (!ParseRegionFileName(region_file.filename(), rx, rz))
        return false;
    const std::filesystem::path region_dir = region_file.parent_path();
    const std::filesystem::path chunks_dir = region_dir.parent_path();
    if (region_dir.filename() != kRegionDirectoryName ||
        chunks_dir.filename() != kChunksDirectoryName)
        return false;
    save_dir = chunks_dir.parent_path();
    relative = RelativeRegionMember(rx, rz);
    return true;
}

bool VerifyRegionMembership(const std::filesystem::path& region_file,
                            const std::vector<RegionRecord>& records,
                            std::vector<std::string>* errors) {
    std::filesystem::path save_dir;
    std::string relative;
    if (!SnapshotMemberForRegionFile(region_file, save_dir, relative))
        return true;
    if (!snapshot_index_exists(save_dir)) {
        std::int64_t format = 0;
        const auto error = CheckMetadataFile(save_dir, &format);
        if (!error.empty() || format >= 1) {
            AddError(errors, error.empty() ? "inconsistent snapshot: index missing" : error);
            return false;
        }
        world::WorldClock clock;
        bool required = false;
        if (!WorldSaveService::read_clock_metadata(save_dir, clock, required, errors) || required) {
            if (required)
                AddError(errors, "inconsistent snapshot: enabled records without index");
            return false;
        }
        return true;
    }
    SnapshotIndex index;
    if (!read_snapshot_index(save_dir, index, errors))
        return false;
    const auto member =
        std::find_if(index.members.begin(),
                     index.members.end(),
                     [&relative](const SnapshotMember& value) { return value.path == relative; });
    const auto digest = Lod0Fingerprint(records);
    if ((member == index.members.end() && digest.present) ||
        (member != index.members.end() &&
         (member->present != digest.present ||
          (digest.present && (member->bytes != digest.bytes || member->fnv64 != digest.fnv64))))) {
        AddError(errors, "inconsistent snapshot: " + relative);
        return false;
    }
    return true;
}

} // namespace

bool WorldSaveService::save_metadata(const std::string& bytes,
                                     const std::filesystem::path& save_dir,
                                     std::vector<std::string>* errors) {
    const auto metadata = nlohmann::json::parse(bytes, nullptr, false);
    const bool has_clock = metadata.contains("simulationTick") || metadata.contains("calendar");
    TransactionScope scope(save_dir, {"world_info.json"}, errors, has_clock);
    if (!scope.ok())
        return false;
    if (!validate_save(save_dir, errors))
        return false;
    world::WorldClock clock;
    std::string clock_error;
    // Legacy metadata is opaque input to this helper. Only clock-bearing input
    // opts into the new validation; the existing destination is still validated.
    std::optional<Vec3> ambient_anchor;
    if (has_clock && (!world::WorldClock::from_metadata(metadata, clock, clock_error) ||
                      !ReadAmbientAnchor(metadata, ambient_anchor, clock_error))) {
        AddError(errors, clock_error);
        return false;
    }
    world::WorldClock previous_clock;
    bool requires_active_regions = false;
    if (!read_clock_metadata(save_dir, previous_clock, requires_active_regions, errors))
        return false;
    if (requires_active_regions && !has_clock) {
        AddError(errors, "Incompatible configuration: this world requires sim.active_regions.");
        return false;
    }
    const auto destination = save_dir / "world_info.json";
    std::filesystem::path temp;
    if (!WriteDurableRegionTemp(destination, bytes, temp, errors))
        return false;
    if (InterruptBeforeRegionReplaceForTesting().exchange(false)) {
        RemoveTemporaryFile(temp);
        AddError(errors, "interrupted before metadata replacement");
        return false;
    }
    if (!ReplaceMember(temp, destination, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
    return scope.commit(errors);
}

bool WorldSaveService::save_metadata_and_active_regions(const std::string& bytes,
                                                        world::ActiveRegionLedger& ledger,
                                                        const std::filesystem::path& save_dir,
                                                        std::vector<std::string>* errors) {
#ifndef NDEBUG
    const PairedSaveGuard guard;
#endif
    const auto metadata = nlohmann::json::parse(bytes, nullptr, false);
    world::WorldClock clock;
    std::optional<Vec3> ambient_anchor;
    std::string error;
    if (!world::WorldClock::from_metadata(metadata, clock, error) ||
        !ReadAmbientAnchor(metadata, ambient_anchor, error)) {
        AddError(errors, error);
        return false;
    }
    auto saved = ledger;
    saved.mark_saved(clock);
    world::ActiveRegionLedger checked;
    if (!world::ActiveRegionLedger::decode(saved.encode(), checked, error) ||
        ledger.header_tick() > clock.tick()) {
        AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
    TransactionScope scope(
        save_dir, {"world_info.json", "chunks/region/active-regions.arl"}, errors, true);
    if (!scope.ok())
        return false;
    world::WorldClock previous_clock;
    bool requires_active_regions = false;
    if (!read_clock_metadata(save_dir, previous_clock, requires_active_regions, errors))
        return false;
    // Each writer validates the installed pair. On rewind, replace the ledger
    // first so it cannot be newer than the clock, even if metadata writing fails.
    const bool written = clock.tick() < previous_clock.tick()
                             ? (save_active_regions(ledger, clock, save_dir, errors) &&
                                save_metadata(bytes, save_dir, errors))
                             : (save_metadata(bytes, save_dir, errors) &&
                                save_active_regions(ledger, clock, save_dir, errors));
    const bool joined = scope.engaged() && !scope.owns_transaction();
    if (!written || !scope.commit(errors))
        return false;
    if (!joined)
        ledger = std::move(saved);
    return true;
}

bool WorldSaveService::read_clock_metadata(const std::filesystem::path& save_dir,
                                           world::WorldClock& clock,
                                           bool& requires_active_regions,
                                           std::vector<std::string>* errors,
                                           std::optional<Vec3>* ambient_anchor) {
    clock = world::WorldClock{};
    requires_active_regions = false;
    if (ambient_anchor)
        ambient_anchor->reset();
    try {
        requires_active_regions = std::filesystem::exists(active_regions_path(save_dir));
        const auto path = save_dir / "world_info.json";
        if (!std::filesystem::exists(path))
            return true;
        std::ifstream input(path, std::ios::binary);
        const auto metadata = nlohmann::json::parse(input);
        std::string error;
        std::optional<Vec3> anchor;
        if (!world::WorldClock::from_metadata(metadata, clock, error) ||
            !ReadAmbientAnchor(metadata, anchor, error)) {
            AddError(errors, error);
            return false;
        }
        requires_active_regions = requires_active_regions || metadata.contains("simulationTick") ||
                                  metadata.contains("calendar");
        if (ambient_anchor)
            *ambient_anchor = anchor;
        return true;
    } catch (const std::exception& e) {
        AddError(errors, std::string("Corrupt world metadata: ") + e.what());
        return false;
    }
}

std::filesystem::path WorldSaveService::world_state_path(const std::filesystem::path& save_dir) {
    return save_dir / kChunksDirectoryName / kWorldStateFileName;
}

std::filesystem::path WorldSaveService::region_directory(const std::filesystem::path& save_dir) {
    return save_dir / kChunksDirectoryName / kRegionDirectoryName;
}

std::filesystem::path
WorldSaveService::region_file_path(const std::filesystem::path& save_dir, int rx, int rz) {
    return region_directory(save_dir) /
           ("r." + std::to_string(rx) + "." + std::to_string(rz) + kRegionFileExtension);
}

std::filesystem::path WorldSaveService::world_manifest_path(const std::filesystem::path& save_dir) {
    return region_directory(save_dir) / kWorldManifestFileName;
}

std::filesystem::path WorldSaveService::plant_entities_path(const std::filesystem::path& save_dir) {
    return region_directory(save_dir) / "plant-entities.json";
}

bool WorldSaveService::save_plant_entities(const Luminumbra::Ecs::EntityRegistrySnapshot& snapshot,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors) {
    TransactionScope scope(save_dir, {"chunks/region/plant-entities.json"}, errors);
    if (!scope.ok())
        return false;
    if (!validate_save(save_dir, errors))
        return false;
    const std::filesystem::path path = plant_entities_path(save_dir);
    std::error_code ec;
    if (snapshot.entities.empty()) {
        // No plants -> no file (a no-plant save is byte-identical). Retire any stale file.
        if (scope.engaged())
            return durable_remove(path, errors) && scope.commit(errors);
        std::filesystem::remove(path, ec);
        return true;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    if (scope.engaged()) {
        // Transactional lane: durable binary replace instead of a truncating stream.
        return durable_write_file(
                   path, Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(snapshot), errors) &&
               scope.commit(errors);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        AddError(errors, "failed to open plant entities file for writing: " + path.string());
        return false;
    }
    output << Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(snapshot);
    output.flush();
    if (!output.good()) {
        AddError(errors, "failed to write plant entities file: " + path.string());
        return false;
    }
    return true;
}

bool WorldSaveService::load_plant_entities(Luminumbra::Ecs::EntityRegistrySnapshot& out,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors) {
    out = Luminumbra::Ecs::EntityRegistrySnapshot{};
    const std::filesystem::path path = plant_entities_path(save_dir);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true; // clean miss: a fresh / no-plant world
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        AddError(errors, "failed to open plant entities file: " + path.string());
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::vector<std::string> load_errors;
    if (!Luminumbra::Ecs::LoadEntityRegistrySnapshotJson(ss.str(), out, load_errors)) {
        for (const auto& e : load_errors)
            AddError(errors, e);
        return false;
    }
    return true;
}

std::filesystem::path WorldSaveService::active_regions_path(const std::filesystem::path& save_dir) {
    return region_directory(save_dir) / "active-regions.arl";
}

bool WorldSaveService::load_active_regions(world::ActiveRegionLedger& ledger,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors,
                                           const world::WorldClock& absent_clock) {
    try {
        const auto path = active_regions_path(save_dir);
        if (!std::filesystem::exists(path)) {
            ledger = world::ActiveRegionLedger({}, absent_clock);
            return true;
        }
        const auto length = std::filesystem::file_size(path);
        if (length > world::ActiveRegionLedger::kMaxFileBytes) {
            AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(length), '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(length));
        std::string error;
        if (!input || !world::ActiveRegionLedger::decode(bytes, ledger, error)) {
            AddError(errors, error.empty() ? world::ActiveRegionLedger::kCorruptMessage : error);
            return false;
        }
        return true;
    } catch (const std::exception&) {
        AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
}

bool WorldSaveService::save_active_regions(world::ActiveRegionLedger& ledger,
                                           const world::WorldClock& clock,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors) {
    const bool has_ledger = !ledger.records().empty() || ledger.local_anchor() ||
                            std::filesystem::exists(active_regions_path(save_dir));
    TransactionScope scope(save_dir, {"chunks/region/active-regions.arl"}, errors, has_ledger);
    if (!scope.ok())
        return false;
    if (!validate_save(save_dir, errors))
        return false;
    // Keep an anchor-only ledger: it restores last walking feet before activation.
    if (ledger.records().empty() && !ledger.local_anchor() &&
        !std::filesystem::exists(active_regions_path(save_dir)))
        return true;
    auto saved = ledger;
    saved.mark_saved(clock);
    const auto bytes = saved.encode();
    world::ActiveRegionLedger checked;
    std::string error;
    if (!world::ActiveRegionLedger::decode(bytes, checked, error) ||
        ledger.header_tick() > clock.tick()) {
        AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
    try {
        const auto path = active_regions_path(save_dir);
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::path temp;
        // Keep abandoned staging files outside the strict region integrity scan.
        // The save root is on the save filesystem; replacement refuses EXDEV
        // if a separately mounted chunks/region/ directory violates that layout.
        const auto staging = save_dir / path.filename();
        ReclaimAbandonedStaging(staging);
        if (!WriteDurableRegionTemp(staging, bytes, temp, errors))
            return false;
        if (const auto hook = BeforeActiveRegionsReplaceForTesting(); hook && !hook()) {
            RemoveTemporaryFile(temp);
            AddError(errors, "interrupted before active-region ledger replacement");
            return false;
        }
        if (!ReplaceMember(temp, path, errors)) {
            RemoveTemporaryFile(temp);
            return false;
        }
        // An outer transaction cannot retain a caller-local ledger reference. The owner can
        // acknowledge through its own lifetime-safe callback at outer commit.
        const bool joined = scope.engaged() && !scope.owns_transaction();
        if (!scope.commit(errors))
            return false;
        if (!joined)
            ledger = std::move(saved);
        return true;
    } catch (const std::exception& e) {
        AddError(errors, std::string("Cannot save active-region ledger: ") + e.what());
        return false;
    }
}

std::uint64_t WorldSaveService::region_simulation_digest(const std::filesystem::path& save_dir,
                                                         world::RegionKey key,
                                                         const RegionDigestInputs& inputs) {
    WorldStreamingState region;
    // Filtering precedes insertion: a live pristine or Loading chunk must not shadow a durable
    // edited record with the same id. insert_chunk preserves the first record per id.
    const auto qualifies = [&key](const std::shared_ptr<Chunk>& chunk) {
        if (!chunk || chunk->sdf_provenance() != ChunkSdfProvenance::LoadedOrEdited ||
            chunk->get_state() == ChunkState::Loading)
            return false;
        int rx = 0, rz = 0;
        region_coords_for_chunk(chunk->get_coords(), rx, rz);
        return rx == key.x && rz == key.z;
    };
    for (const auto& chunk : inputs.live) {
        if (qualifies(chunk))
            region.insert_chunk(chunk);
    }
    for (const auto& chunk : inputs.parked) {
        if (qualifies(chunk))
            region.insert_chunk(chunk);
    }
    if (!save_dir.empty()) {
        if (!inputs.durable.empty() && !snapshot_index_exists(save_dir))
            throw std::runtime_error("inconsistent snapshot: index missing");
        std::vector<std::shared_ptr<Chunk>> durable_chunks;
        std::vector<std::string> errors;
        if (!read_authoritative_region_chunks(region_file_path(save_dir, key.x, key.z),
                                              key.x * kRegionChunkSpan,
                                              key.x * kRegionChunkSpan + kRegionChunkSpan - 1,
                                              key.z * kRegionChunkSpan,
                                              key.z * kRegionChunkSpan + kRegionChunkSpan - 1,
                                              durable_chunks,
                                              &errors))
            throw std::runtime_error(errors.empty() ? "Cannot read durable region records"
                                                    : errors.front());
        for (const auto& chunk : durable_chunks) {
            if (qualifies(chunk))
                region.insert_chunk(chunk);
        }
    }
    return std::stoull(ComputeRegionContentHash(region), nullptr, 16);
}

void WorldSaveService::region_coords_for_chunk(const IVec3& chunk_coords,
                                               int& out_rx,
                                               int& out_rz) {
    out_rx = FloorDiv(chunk_coords.x, kRegionChunkSpan);
    out_rz = FloorDiv(chunk_coords.z, kRegionChunkSpan);
}

namespace {
bool SameSaveDirectory(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    std::error_code ec;
    std::filesystem::path a = std::filesystem::weakly_canonical(lhs, ec);
    if (ec)
        a = lhs.lexically_normal();
    ec.clear();
    std::filesystem::path b = std::filesystem::weakly_canonical(rhs, ec);
    if (ec)
        b = rhs.lexically_normal();
    return a == b;
}
} // namespace

WorldSaveService::SaveFactsScope::SaveFactsScope(const std::filesystem::path& save_dir)
    : m_save_dir(save_dir)
    , m_previous(g_active_facts) {
    g_active_facts = this;
}

WorldSaveService::SaveFactsScope::~SaveFactsScope() {
    g_active_facts = m_previous;
}

WorldSaveService::SaveFactsScope*
WorldSaveService::SaveFactsScope::active_for(const std::filesystem::path& save_dir) {
    for (SaveFactsScope* scope = g_active_facts; scope != nullptr; scope = scope->m_previous) {
        if (SameSaveDirectory(scope->m_save_dir, save_dir))
            return scope;
    }
    return nullptr;
}

bool WorldSaveService::SaveFactsScope::validate(std::vector<std::string>* errors) {
    if (!m_validated) {
        WorldStreamingState state;
        m_diagnostics.clear();
        m_chunk_snapshot_present = false;
        m_snapshot_present = WorldSaveService{}.load_world(
            state, m_save_dir, m_diagnostics, {}, [this](const IVec3&) {
                m_chunk_snapshot_present = true;
                return false;
            });
        m_chunk_snapshot_present = m_snapshot_present && m_chunk_snapshot_present;
        m_validated_ok = m_diagnostics.empty();
        m_validated = true;
        m_presence_known = true;
    }
    for (const auto& message : m_diagnostics)
        AddError(errors, message);
    return m_validated_ok;
}

void WorldSaveService::SaveFactsScope::defer_until_commit(std::function<void()> action) {
    if (auto* transaction = SaveTransaction::active_for(m_save_dir)) {
        transaction->defer_until_commit(std::move(action));
        return;
    }
    m_deferred.push_back(std::move(action));
}

void WorldSaveService::SaveFactsScope::run_deferred() {
    auto actions = std::move(m_deferred);
    m_deferred.clear();
    for (auto& action : actions)
        action();
}

void WorldSaveService::SaveFactsScope::discard_deferred() {
    m_deferred.clear();
}

void WorldSaveService::SaveFactsScope::invalidate_presence_facts() {
    m_presence_known = false;
}

bool WorldSaveService::has_world_save(const std::filesystem::path& save_dir) {
    if (SaveFactsScope* facts = SaveFactsScope::active_for(save_dir)) {
        facts->validate(nullptr);
        if (facts->m_presence_known)
            return facts->m_snapshot_present;
    }
    WorldStreamingState state;
    std::vector<std::string> errors;
    return WorldSaveService{}.load_world(
        state, save_dir, errors, {}, [](const IVec3&) { return false; });
}

bool WorldSaveService::has_chunk_snapshot(const std::filesystem::path& save_dir) {
    if (SaveFactsScope* facts = SaveFactsScope::active_for(save_dir)) {
        facts->validate(nullptr);
        if (facts->m_presence_known)
            return facts->m_chunk_snapshot_present;
    }
    WorldStreamingState state;
    std::vector<std::string> errors;
    bool has_chunks = false;
    return WorldSaveService{}.load_world(state, save_dir, errors, {}, [&has_chunks](const IVec3&) {
        has_chunks = true;
        return false;
    }) && has_chunks;
}

bool WorldSaveService::validate_save(const std::filesystem::path& save_dir,
                                     std::vector<std::string>* errors,
                                     const std::stop_token& stop) {
    if (SaveFactsScope* facts = SaveFactsScope::active_for(save_dir))
        return facts->validate(errors);
    WorldStreamingState state;
    std::vector<std::string> diagnostics;
    WorldSaveService{}.load_world(
        state, save_dir, diagnostics, stop, [](const IVec3&) { return false; });
    for (const auto& message : diagnostics)
        AddError(errors, message);
    return diagnostics.empty();
}

bool WorldSaveService::read_container_records(const std::filesystem::path& region_file,
                                              std::vector<ContainerRecord>& out_records,
                                              std::vector<std::string>* errors) {
    out_records.clear();
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(region_file, exists_error);
    if (exists_error) {
        AddError(errors, "failed to inspect region file: " + exists_error.message());
        return false;
    }
    std::vector<RegionRecord> raw_records;
    if (exists && !ReadRegionFile(region_file, raw_records, errors)) {
        return false;
    }
    if (!VerifyRegionMembership(region_file, raw_records, errors))
        return false;
    out_records.reserve(raw_records.size());
    for (const RegionRecord& raw : raw_records) {
        ContainerRecord record;
        record.id = raw.id;
        record.lod_level = raw.lod_level;
        record.flags = raw.flags;
        if (!DecompressPayload(raw, record.payload, errors)) {
            out_records.clear();
            return false;
        }
        out_records.push_back(std::move(record));
    }
    return true;
}

bool WorldSaveService::read_region_chunks(const std::filesystem::path& region_file,
                                          std::vector<std::shared_ptr<Chunk>>& out_chunks,
                                          std::vector<std::string>* errors) {
    out_chunks.clear();
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(region_file, exists_error);
    if (exists_error) {
        AddError(errors, "failed to inspect region file: " + exists_error.message());
        return false;
    }
    std::vector<RegionRecord> raw_records;
    if ((exists && !ReadRegionFile(region_file, raw_records, errors)) ||
        !VerifyRegionMembership(region_file, raw_records, errors)) {
        return false;
    }
    for (const RegionRecord& record : raw_records) {
        if (record.lod_level != 0u)
            continue;
        std::shared_ptr<Chunk> chunk = DecodeChunkRecord(record, region_file, errors);
        if (!chunk) {
            out_chunks.clear();
            return false;
        }
        out_chunks.push_back(std::move(chunk));
    }
    std::sort(out_chunks.begin(),
              out_chunks.end(),
              [](const std::shared_ptr<Chunk>& lhs, const std::shared_ptr<Chunk>& rhs) {
                  return lhs->get_id() < rhs->get_id();
              });
    return true;
}

bool WorldSaveService::read_authoritative_region_chunks(
    const std::filesystem::path& region_file,
    int min_chunk_x,
    int max_chunk_x,
    int min_chunk_z,
    int max_chunk_z,
    std::vector<std::shared_ptr<Chunk>>& out_chunks,
    std::vector<std::string>* errors) {
    out_chunks.clear();
    if (min_chunk_x > max_chunk_x || min_chunk_z > max_chunk_z) {
        AddError(errors, "authoritative chunk read bounds are invalid");
        return false;
    }
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(region_file, exists_error);
    if (exists_error) {
        AddError(errors, "failed to inspect region file: " + exists_error.message());
        return false;
    }
    std::vector<RegionRecord> raw_records;
    if ((exists && !ReadRegionFile(region_file, raw_records, errors)) ||
        !VerifyRegionMembership(region_file, raw_records, errors)) {
        return false;
    }
    for (const RegionRecord& record : raw_records) {
        if (record.lod_level != 0u || (record.flags & kRecordFlagEdited) == 0u)
            continue;
        const IVec3 indexed_coords = Chunk::decode_id(record.id);
        if (indexed_coords.x < min_chunk_x || indexed_coords.x > max_chunk_x ||
            indexed_coords.z < min_chunk_z || indexed_coords.z > max_chunk_z) {
            continue;
        }
        std::shared_ptr<Chunk> chunk = DecodeChunkRecord(record, region_file, errors);
        if (!chunk) {
            out_chunks.clear();
            return false;
        }
        out_chunks.push_back(std::move(chunk));
    }
    std::sort(out_chunks.begin(),
              out_chunks.end(),
              [](const std::shared_ptr<Chunk>& lhs, const std::shared_ptr<Chunk>& rhs) {
                  return lhs->get_id() < rhs->get_id();
              });
    return true;
}

bool WorldSaveService::upsert_container_records(const std::filesystem::path& region_file,
                                                const std::vector<ContainerRecord>& records,
                                                std::vector<std::string>* errors) {
    try {
        // Only lod-0 content is snapshot-indexed; far-only upserts never open a transaction.
        std::unique_ptr<TransactionScope> scope;
        const bool has_lod0 = std::any_of(records.begin(), records.end(), [](const auto& record) {
            return record.lod_level == 0;
        });
        std::filesystem::path scoped_save_dir;
        std::string scoped_member;
        if (has_lod0 && SnapshotMemberForRegionFile(region_file, scoped_save_dir, scoped_member)) {
            scope = std::make_unique<TransactionScope>(
                scoped_save_dir, std::vector<std::string>{scoped_member}, errors);
            if (!scope->ok())
                return false;
        }
        const std::filesystem::path parent = region_file.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        std::vector<RegionRecord> raw_records;
        std::error_code exists_error;
        if (std::filesystem::exists(region_file, exists_error) && !exists_error) {
            if (!ReadRegionFile(region_file, raw_records, errors)) {
                return false;
            }
        }

        std::map<std::pair<std::uint8_t, std::uint64_t>, std::size_t> record_index;
        for (std::size_t i = 0; i < raw_records.size(); ++i) {
            record_index[{raw_records[i].lod_level, raw_records[i].id}] = i;
        }

        for (const ContainerRecord& record : records) {
            RegionRecord raw;
            raw.id = record.id;
            raw.lod_level = record.lod_level;
            raw.flags = record.flags;
            if (!CompressPayload(record.payload, raw, errors)) {
                return false;
            }
            const auto existing = record_index.find({raw.lod_level, raw.id});
            if (existing != record_index.end()) {
                raw_records[existing->second] = std::move(raw);
            } else {
                record_index[{raw.lod_level, raw.id}] = raw_records.size();
                raw_records.push_back(std::move(raw));
            }
        }

        if (!WriteRegionFile(region_file, raw_records, errors))
            return false;
        return !scope || scope->commit(errors);
    } catch (const std::exception& e) {
        AddError(errors, std::string("failed to upsert region records: ") + e.what());
        return false;
    }
}

namespace {
std::atomic<std::uint64_t>& DecodeCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}
} // namespace

std::uint64_t WorldSaveService::decode_count_for_testing() {
    return DecodeCounter().load(std::memory_order_acquire);
}

bool WorldSaveService::durable_flush_directory(const std::filesystem::path& directory,
                                               std::vector<std::string>* errors) {
#if defined(_WIN32)
    // Windows has no supported FlushFileBuffers operation on an ordinary directory
    // handle. Namespace publication/removal uses same-volume, write-through moves;
    // undo bytes are flushed first. Native power-loss qualification remains a
    // separate requirement for each supported filesystem/storage stack.
    (void)directory;
    (void)errors;
    return true;
#else
    int flags = O_RDONLY;
#if defined(O_DIRECTORY)
    flags |= O_DIRECTORY;
#endif
    const int fd = ::open(directory.c_str(), flags);
    if (fd < 0) {
        const int error = errno;
        AddError(errors,
                 "failed to open directory for durable flush: " + directory.string() + ": " +
                     PosixErrorMessage(error));
        return false;
    }
    while (::fsync(fd) != 0) {
        if (errno == EINTR)
            continue;
        const int error = errno;
        ::close(fd);
        AddError(errors,
                 "directory flush failed: " + directory.string() + ": " + PosixErrorMessage(error));
        return false;
    }
    if (::close(fd) != 0) {
        const int error = errno;
        AddError(errors,
                 "directory close failed: " + directory.string() + ": " + PosixErrorMessage(error));
        return false;
    }
    return true;
#endif
}

bool WorldSaveService::durable_write_file(const std::filesystem::path& destination,
                                          const std::string& bytes,
                                          std::vector<std::string>* errors) {
    std::filesystem::path temp;
    if (!WriteDurableRegionTemp(destination, bytes, temp, errors))
        return false;
    if (!ReplaceMember(temp, destination, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
    return true;
}

bool WorldSaveService::durable_remove(const std::filesystem::path& path,
                                      std::vector<std::string>* errors) {
    std::error_code ec;
    const bool existed = std::filesystem::exists(path, ec);
    if (ec) {
        AddError(errors, "failed to stat " + path.string() + ": " + ec.message());
        return false;
    }
    if (!existed)
        return true;
    report_snapshot_step("before-delete", path);
    if (g_active_facts != nullptr)
        g_active_facts->invalidate_presence_facts();
#if defined(_WIN32)
    // Publish absence through a write-through move before deleting the expendable
    // tombstone. If interrupted here, recovery restores the protected original;
    // the normal staging sweep reclaims the tombstone. Never cross a volume.
    ReclaimAbandonedStaging(path);
    const auto tombstone = NextRegionTempPath(path);
    if (!::MoveFileExW(path.c_str(), tombstone.c_str(), MOVEFILE_WRITE_THROUGH)) {
        AddError(errors,
                 "failed to publish removal of " + path.string() + ": " +
                     WindowsErrorMessage(::GetLastError()));
        return false;
    }
    RemoveTemporaryFile(tombstone);
#else
    std::filesystem::remove(path, ec);
    if (ec) {
        AddError(errors, "failed to remove " + path.string() + ": " + ec.message());
        return false;
    }
#endif
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    if (!durable_flush_directory(parent, errors))
        return false;
    report_snapshot_step("after-delete", path);
    return true;
}

bool WorldSaveService::member_digest(const std::filesystem::path& save_dir,
                                     std::string_view relative_path,
                                     MemberDigest& out,
                                     std::string& error) {
    out = MemberDigest{};
    try {
        if (!member_path_allowed(relative_path)) {
            error = "member path is not allowed";
            return false;
        }
        const std::string relative(relative_path);
        const std::filesystem::path path = save_dir / relative;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return !ec; // absent (not an error)

        std::vector<std::string> diagnostics;
        const auto first_error = [&diagnostics](const char* fallback) {
            return diagnostics.empty() ? std::string(fallback) : diagnostics.front();
        };

        if (relative.rfind("chunks/region/r.", 0) == 0) {
            int rx = 0;
            int rz = 0;
            if (!ParseRegionFileName(path, rx, rz)) {
                error = "unrecognised region file name";
                return false;
            }
            std::vector<RegionRecord> records;
            if (!ReadRegionFile(path, records, &diagnostics)) {
                error = first_error("unreadable region file");
                return false;
            }
            for (const RegionRecord& record : records) {
                if (record.lod_level == 0) {
                    const auto chunk = DecodeChunkRecord(record, path, &diagnostics);
                    if (!chunk) {
                        error = first_error("corrupt chunk record");
                        return false;
                    }
                    constexpr std::size_t kLattice = static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                                     (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
                    if (!chunk->sdf_data.empty() && chunk->sdf_data.size() != kLattice) {
                        error = "corrupt world chunk SDF lattice";
                        return false;
                    }
                    int chunk_rx = 0;
                    int chunk_rz = 0;
                    region_coords_for_chunk(chunk->get_coords(), chunk_rx, chunk_rz);
                    if (chunk_rx != rx || chunk_rz != rz) {
                        error = "corrupt chunk region identity";
                        return false;
                    }
                } else {
                    const auto tier = static_cast<World::FarLodTier>(record.lod_level);
                    if (record.id != World::FarLodStore::tile_record_id(tier, rx, rz)) {
                        error = "corrupt far-LOD region identity";
                        return false;
                    }
                    std::string payload;
                    World::FarLodTile tile;
                    if (!DecompressPayload(record, payload, &diagnostics) ||
                        !World::FarLodStore::decode_payload(
                            payload, tier, rx, rz, record.flags, tile, &diagnostics)) {
                        error = first_error("corrupt far-LOD record");
                        return false;
                    }
                }
            }
            out = Lod0Fingerprint(records);
            return true;
        }

        if (relative == "world_info.json") {
            const std::string message = CheckMetadataFile(save_dir);
            if (!message.empty()) {
                error = message;
                return false;
            }
            // The installed ledger may never be ahead of the metadata clock (load_world refuses
            // that pair), so a standalone metadata rewrite must not publish it.
            if (std::filesystem::exists(active_regions_path(save_dir))) {
                world::ActiveRegionLedger ledger;
                world::WorldClock clock;
                bool required = false;
                if (load_active_regions(ledger, save_dir, &diagnostics) &&
                    read_clock_metadata(save_dir, clock, required, &diagnostics) &&
                    ledger.header_tick() > clock.tick()) {
                    error = world::ActiveRegionLedger::kCorruptMessage;
                    return false;
                }
            }
        } else if (relative == "chunks/region/world-manifest.json") {
            const std::string message = CheckManifestFile(path);
            if (!message.empty()) {
                error = message;
                return false;
            }
        } else if (relative == "chunks/region/plant-entities.json") {
            Ecs::EntityRegistrySnapshot plants;
            if (!load_plant_entities(plants, save_dir, &diagnostics)) {
                error = first_error("corrupt plant entities");
                return false;
            }
        } else if (relative == "aether_state.efs") {
            constexpr std::uintmax_t kMaxEnergyBytes = 16u * 1024u * 1024u;
            if (std::filesystem::file_size(path) > kMaxEnergyBytes) {
                error = "corrupt energy field record: size limit";
                return false;
            }
            std::ifstream input(path, std::ios::binary);
            const std::string bytes(std::istreambuf_iterator<char>{input}, {});
            std::istringstream header(bytes);
            std::string magic;
            int channels = 0;
            // Bound channel allocation before invoking the legacy decoder. Indexed writers
            // emit canonical EFS1; re-encoding also refuses duplicate pages/cells and junk.
            if (!input || !(header >> magic >> channels) || magic != "EFS1" || channels < 1 ||
                channels > 64) {
                error = "corrupt energy field record";
                return false;
            }
            std::string line;
            std::size_t pages = 0;
            std::getline(header, line);
            while (std::getline(header, line))
                if (!line.empty() &&
                    ++pages > (64u * 1024u * 1024u) / (256u * sizeof(std::uint16_t) *
                                                       static_cast<std::size_t>(channels))) {
                    error = "corrupt energy field record: decoded size limit";
                    return false;
                }
            luminumbra::fields::EnergyFieldState energy;
            if (!energy.DeserializeRecord(bytes, 0) || energy.SerializeRecord(0) != bytes) {
                error = "corrupt energy field record";
                return false;
            }
        } else if (relative == "chunks/region/active-regions.arl") {
            world::ActiveRegionLedger ledger;
            if (!load_active_regions(ledger, save_dir, &diagnostics)) {
                error = first_error(world::ActiveRegionLedger::kCorruptMessage);
                return false;
            }
            world::WorldClock clock;
            bool required = false;
            if (!read_clock_metadata(save_dir, clock, required, &diagnostics)) {
                error = first_error("corrupt world metadata");
                return false;
            }
            if (ledger.header_tick() > clock.tick()) {
                error = world::ActiveRegionLedger::kCorruptMessage;
                return false;
            }
        }

        std::vector<std::string> hash_errors;
        bool exists = false;
        if (!hash_file(path, out.bytes, out.fnv64, exists, &hash_errors)) {
            error = hash_errors.empty() ? std::string("unreadable member") : hash_errors.front();
            return false;
        }
        out.present = exists;
        return true;
    } catch (const std::exception& e) {
        out = MemberDigest{};
        error = e.what();
        return false;
    }
}

bool WorldSaveService::write_member_bytes(const std::filesystem::path& save_dir,
                                          std::string_view relative_path,
                                          const std::string& bytes,
                                          std::vector<std::string>* errors) {
    if (!member_path_allowed(relative_path)) {
        AddError(errors, "member path is not allowed: " + std::string(relative_path));
        return false;
    }
    const std::string relative(relative_path);
    TransactionScope scope(save_dir, {relative}, errors);
    if (!scope.ok())
        return false;
    const std::filesystem::path destination = save_dir / relative;
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    return durable_write_file(destination, bytes, errors) && scope.commit(errors);
}

bool WorldSaveService::remove_member(const std::filesystem::path& save_dir,
                                     std::string_view relative_path,
                                     std::vector<std::string>* errors) {
    if (!member_path_allowed(relative_path)) {
        AddError(errors, "member path is not allowed: " + std::string(relative_path));
        return false;
    }
    const std::string relative(relative_path);
    TransactionScope scope(save_dir, {relative}, errors);
    if (!scope.ok())
        return false;
    return durable_remove(save_dir / relative, errors) && scope.commit(errors);
}

bool WorldSaveService::forget_snapshot_index_for_testing(const std::filesystem::path& save_dir) {
    std::error_code ec;
    std::filesystem::remove(snapshot_index_path(save_dir), ec);
    std::filesystem::remove_all(journal_directory(save_dir), ec);
    const auto metadata_path = save_dir / "world_info.json";
    if (!std::filesystem::exists(metadata_path, ec))
        return true;
    try {
        nlohmann::json metadata;
        {
            std::ifstream input(metadata_path, std::ios::binary);
            metadata = nlohmann::json::parse(input);
        }
        if (!metadata.is_object() || !metadata.contains("snapshotFormat"))
            return true;
        metadata.erase("snapshotFormat");
        std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
        output << metadata.dump(2) << "\n";
        return output.good();
    } catch (const std::exception&) {
        return false;
    }
}

void WorldSaveService::set_interrupt_before_region_replace_for_testing(bool enabled) {
    InterruptBeforeRegionReplaceForTesting().store(enabled, std::memory_order_release);
}

bool WorldSaveService::write_file_durably(const std::filesystem::path& destination,
                                          const std::string& bytes,
                                          std::vector<std::string>* errors) {
    std::error_code directory_error;
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), directory_error);
    }
    if (directory_error) {
        AddError(errors,
                 "cannot create directory " + destination.parent_path().string() + ": " +
                     directory_error.message());
        return false;
    }
#if !defined(_WIN32)
    // Flush the entry of every ancestor directory, deepest first. A retry after a
    // failed flush finds the directories already present, so the chain is flushed
    // on every call rather than only for directories this call created. The
    // replacement helper flushes the destination directory itself.
    for (std::filesystem::path ancestor = destination.parent_path();
         !ancestor.empty() && ancestor != ancestor.root_path();
         ancestor = ancestor.parent_path()) {
        const std::filesystem::path holder =
            ancestor.has_parent_path() ? ancestor.parent_path() : std::filesystem::path(".");
        if (!FlushDirectoryEntry(holder, errors)) {
            return false;
        }
    }
#endif
    std::filesystem::path temp_path;
    if (!WriteDurableRegionTemp(destination, bytes, temp_path, errors)) {
        return false;
    }
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel)) {
        RemoveTemporaryFile(temp_path);
        AddError(errors,
                 "file replacement interrupted by test seam before replacing: " +
                     destination.string());
        return false;
    }
    if (!ReplaceRegionFileAtomically(temp_path, destination, errors)) {
        RemoveTemporaryFile(temp_path);
        return false;
    }
    return true;
}

void WorldSaveService::set_directory_flush_failure_for_testing(int flushes_to_skip) {
    DirectoryFlushFailureCountdownForTesting().store(flushes_to_skip, std::memory_order_release);
}

void WorldSaveService::set_before_active_regions_replace_for_testing(bool (*hook)()) {
    BeforeActiveRegionsReplaceForTesting() = hook;
}

bool WorldSaveService::save_world(const WorldStreamingState& state,
                                  const std::filesystem::path& save_dir,
                                  std::vector<std::string>* errors) const {
    return write_snapshot(state.snapshot_chunks(), save_dir, errors);
}

WorldSaveService::ChunkReadResult WorldSaveService::read_chunk(
    const std::filesystem::path& save_dir, const IVec3& coords, bool require_index) {
    ChunkReadResult result;
    const auto fail = [&result](std::string error) {
        result.status = ChunkReadStatus::Error;
        result.chunk.reset();
        result.error = std::move(error);
        return result;
    };
    try {
        int rx = 0, rz = 0;
        region_coords_for_chunk(coords, rx, rz);
        const auto path = region_file_path(save_dir, rx, rz);
        std::vector<std::string> errors;
        SnapshotIndex index;
        const bool indexed = snapshot_index_exists(save_dir);
        if (require_index && !indexed)
            return fail("inconsistent snapshot: index missing");
        if (indexed && !read_snapshot_index(save_dir, index, &errors))
            return fail(errors.empty() ? "corrupt snapshot index" : errors.front());
        std::vector<RegionRecord> records;
        if (std::filesystem::exists(path) && !ReadRegionFile(path, records, &errors))
            return fail(errors.empty() ? "unreadable chunk region" : errors.front());
        if (!VerifyRegionMembership(path, records, &errors))
            return fail(errors.empty() ? "inconsistent snapshot" : errors.front());
        if (indexed) {
            const auto relative = RelativeRegionMember(rx, rz);
            const auto member = std::find_if(
                index.members.begin(),
                index.members.end(),
                [&relative](const SnapshotMember& value) { return value.path == relative; });
            const auto digest = Lod0Fingerprint(records);
            if ((member == index.members.end() && digest.present) ||
                (member != index.members.end() &&
                 (member->present != digest.present ||
                  (digest.present &&
                   (member->bytes != digest.bytes || member->fnv64 != digest.fnv64)))))
                return fail("inconsistent snapshot: " + relative);
        }
        const auto id = Chunk::calculate_id(coords);
        for (const auto& record : records) {
            if (record.lod_level != 0 || record.id != id)
                continue;
            result.chunk = DecodeChunkRecord(record, path, &errors);
            constexpr std::size_t kLattice = static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                             (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
            if (!result.chunk || result.chunk->get_coords() != coords ||
                (!result.chunk->sdf_data.empty() && result.chunk->sdf_data.size() != kLattice))
                return fail(errors.empty() ? "corrupt exact chunk identity or SDF lattice"
                                           : errors.front());
            result.chunk->normalize_for_park();
            result.status = ChunkReadStatus::Found;
            return result;
        }
        return result;
    } catch (const std::exception& error) {
        return fail(std::string("unreadable durable chunk: ") + error.what());
    }
}

std::function<void()>
WorldSaveService::chunk_save_acknowledgement(const std::vector<std::shared_ptr<Chunk>>& chunks) {
    struct Revision {
        std::shared_ptr<Chunk> chunk;
        u32 voxel;
        u32 water;
    };
    std::vector<Revision> revisions;
    for (const auto& chunk : chunks)
        if (chunk && chunk->is_voxel_data_dirty())
            revisions.push_back({chunk, chunk->voxel_revision(), chunk->water_revision()});
    return [revisions = std::move(revisions)]() {
        for (const auto& revision : revisions)
            if (revision.voxel != std::numeric_limits<u32>::max() &&
                revision.water != std::numeric_limits<u32>::max() &&
                revision.chunk->voxel_revision() == revision.voxel &&
                revision.chunk->water_revision() == revision.water) {
                revision.chunk->clear_voxel_data_dirty();
                revision.chunk->clear_water_state_dirty();
            }
    };
}

bool WorldSaveService::load_world(WorldStreamingState& state,
                                  const std::filesystem::path& save_dir,
                                  std::vector<std::string>& errors,
                                  const std::stop_token& stop,
                                  const std::function<bool(const IVec3&)>& retain_chunk,
                                  std::vector<IVec3>* authoritative_inventory) const {
    DecodeCounter().fetch_add(1, std::memory_order_acq_rel);
    state.clear();
    if (authoritative_inventory)
        authoritative_inventory->clear();
    const auto reject = [&state, &errors, authoritative_inventory](const std::string& message) {
        state.clear();
        if (authoritative_inventory)
            authoritative_inventory->clear();
        if (!message.empty())
            errors.push_back(message);
        return false;
    };
    try {
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
        // An indexed directory is verified against its snapshot index (S4). Legacy directories
        // (no index, no marker) load exactly as before.
        const bool indexed = snapshot_index_exists(save_dir);
        SnapshotIndex snapshot_index;
        if (indexed) {
            std::vector<std::string> index_errors;
            if (!read_snapshot_index(save_dir, snapshot_index, &index_errors))
                return reject("inconsistent snapshot: " +
                              (index_errors.empty() ? std::string("corrupt snapshot index")
                                                    : index_errors.front()));
        }
        std::int64_t snapshot_format = 0;
        if (const std::string metadata_error = CheckMetadataFile(save_dir, &snapshot_format);
            !metadata_error.empty())
            return reject(metadata_error);
        if (!indexed && snapshot_format >= 1)
            return reject("inconsistent snapshot: index missing");
        if (!indexed) {
            world::WorldClock clock;
            bool requires_active_regions = false;
            if (!read_clock_metadata(save_dir, clock, requires_active_regions, &errors))
                return reject("");
            if (requires_active_regions)
                return reject("inconsistent snapshot: enabled records without index");
        }
        const auto chunks_dir = save_dir / kChunksDirectoryName;
        const auto region_dir = region_directory(save_dir);
        if (std::filesystem::exists(world_state_path(save_dir)) ||
            std::filesystem::exists(chunks_dir / kWorldStateBackupFileName)) {
            return reject(kObsoleteWorldMessage);
        }
        if (!std::filesystem::exists(chunks_dir))
            return false;
        for (const auto& entry : std::filesystem::directory_iterator(chunks_dir)) {
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            if (entry.path() != region_dir || !entry.is_directory())
                return reject("corrupt or unknown world persistence artifact: " +
                              entry.path().string());
        }
        if (!std::filesystem::exists(region_dir))
            return false;
        std::vector<std::filesystem::path> region_files;
        bool present = false;
        for (const auto& entry : std::filesystem::directory_iterator(region_dir)) {
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            int rx = 0, rz = 0;
            if (!entry.is_regular_file())
                return reject("corrupt world persistence artifact: " + entry.path().string());
            if (entry.path() == snapshot_index_path(save_dir)) {
                continue; // the index is verified below; it is never a member
            }
            if (entry.path() == world_manifest_path(save_dir) ||
                entry.path() == plant_entities_path(save_dir) ||
                entry.path() == active_regions_path(save_dir)) {
                present = true;
            } else if (ParseRegionFileName(entry.path(), rx, rz)) {
                region_files.push_back(entry.path());
                present = true;
            } else {
                return reject("corrupt or unknown region file: " + entry.path().string());
            }
        }
        if (!present) {
            // An index-only region directory is still an indexed snapshot: verify it before
            // reporting a clean miss, or tampered metadata/energy would go unnoticed.
            if (indexed) {
                const std::string mismatch = VerifySnapshotIndex(save_dir, snapshot_index, {});
                if (!mismatch.empty())
                    return reject("inconsistent snapshot: " + mismatch);
            }
            return false;
        }
        if (const std::string manifest_error = CheckManifestFile(world_manifest_path(save_dir));
            !manifest_error.empty())
            return reject(manifest_error);
        std::map<std::string, MemberDigest> region_digests;
        std::set<ChunkID> decoded_ids;
        std::sort(region_files.begin(), region_files.end());
        for (const auto& path : region_files) {
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            int rx = 0, rz = 0;
            ParseRegionFileName(path, rx, rz);
            std::vector<RegionRecord> records;
            if (!ReadRegionFile(path, records, &errors, stop))
                return reject("");
            if (indexed) {
                const MemberDigest digest = Lod0Fingerprint(records);
                if (digest.present)
                    region_digests[RelativeRegionMember(rx, rz)] = digest;
            }
            for (const auto& record : records) {
                if (stop.stop_requested())
                    return reject("Saved-world validation cancelled.");
                if (record.lod_level == 0) {
                    const auto chunk = DecodeChunkRecord(record, path, &errors);
                    if (!chunk)
                        return reject("");
                    constexpr std::size_t kLattice = static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                                     (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
                    if ((indexed || authoritative_inventory) && !chunk->sdf_data.empty() &&
                        chunk->sdf_data.size() != kLattice)
                        return reject("corrupt world chunk SDF lattice");
                    int chunk_rx = 0, chunk_rz = 0;
                    region_coords_for_chunk(chunk->get_coords(), chunk_rx, chunk_rz);
                    if (chunk_rx != rx || chunk_rz != rz ||
                        !decoded_ids.insert(chunk->get_id()).second)
                        return reject("corrupt chunk region identity or duplicate id: " +
                                      path.string());
                    if (authoritative_inventory &&
                        chunk->sdf_provenance() == ChunkSdfProvenance::LoadedOrEdited)
                        authoritative_inventory->push_back(chunk->get_coords());
                    if (!retain_chunk || retain_chunk(chunk->get_coords()))
                        state.insert_chunk(chunk);
                } else {
                    const auto tier = static_cast<World::FarLodTier>(record.lod_level);
                    if (record.id != World::FarLodStore::tile_record_id(tier, rx, rz))
                        return reject("corrupt far-LOD region identity: " + path.string());
                    std::string payload;
                    World::FarLodTile tile;
                    if (!DecompressPayload(record, payload, &errors) ||
                        !World::FarLodStore::decode_payload(
                            payload, tier, rx, rz, record.flags, tile, &errors))
                        return reject("");
                }
            }
        }
        world::ActiveRegionLedger ledger;
        if (!load_active_regions(ledger, save_dir, &errors))
            return reject("");
        if (std::filesystem::exists(active_regions_path(save_dir))) {
            world::WorldClock clock;
            bool required = false;
            if (!read_clock_metadata(save_dir, clock, required, &errors))
                return reject("");
            if (ledger.header_tick() > clock.tick())
                return reject(world::ActiveRegionLedger::kCorruptMessage);
        }
        Ecs::EntityRegistrySnapshot plants;
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
        if (!load_plant_entities(plants, save_dir, &errors))
            return reject("");
        if (indexed) {
            const std::string mismatch =
                VerifySnapshotIndex(save_dir, snapshot_index, region_digests);
            if (!mismatch.empty())
                return reject("inconsistent snapshot: " + mismatch);
        }
        return true;
    } catch (const std::exception& e) {
        return reject(std::string("corrupt or unreadable world persistence: ") + e.what());
    }
}

std::string WorldSaveService::world_hash(const WorldStreamingState& state) const {
    return ComputeWorldStreamingStateHash(state);
}

WorldSaveDirtyReport WorldSaveService::save_dirty_chunks(WorldStreamingState& state,
                                                         const std::filesystem::path& save_dir,
                                                         std::vector<std::string>* errors) const {
    WorldSaveDirtyReport report;
    report.chunks_total = state.size();

    const std::vector<ChunkID> dirty_ids = state.dirty_chunk_ids();
    report.chunks_dirty = dirty_ids.size();
    if (dirty_ids.empty()) {
        return report;
    }

    // O(edited regions): rewrite only region files containing dirty chunks,
    // refreshing the records of every in-memory chunk in those regions.
    std::map<std::pair<int, int>, bool> dirty_regions;
    for (const ChunkID id : dirty_ids) {
        if (const auto chunk = state.find_chunk(id)) {
            int rx = 0;
            int rz = 0;
            region_coords_for_chunk(chunk->get_coords(), rx, rz);
            dirty_regions[{rx, rz}] = true;
        }
    }

    std::vector<std::shared_ptr<Chunk>> chunks_to_write;
    for (const auto& chunk : state.snapshot_chunks()) {
        if (!chunk) {
            continue;
        }
        int rx = 0;
        int rz = 0;
        region_coords_for_chunk(chunk->get_coords(), rx, rz);
        if (dirty_regions.count({rx, rz}) != 0) {
            chunks_to_write.push_back(chunk);
        }
    }

    const auto clear_dirty = chunk_save_acknowledgement(chunks_to_write);
    if (!write_snapshot(chunks_to_write, save_dir, errors, &report.regions_written)) {
        return report;
    }

    // Inside an outer transaction the dirty bits clear only once the whole save commits.
    if (auto* transaction = SaveTransaction::active_for(save_dir))
        transaction->defer_until_commit(clear_dirty);
    else
        clear_dirty();
    report.saved = true;
    return report;
}

bool WorldSaveService::write_snapshot(const std::vector<std::shared_ptr<Chunk>>& chunks,
                                      const std::filesystem::path& save_dir,
                                      std::vector<std::string>* errors,
                                      std::size_t* regions_written) const {
    try {
        // Group the chunks to persist by region (deterministic order).
        std::map<std::pair<int, int>, std::vector<std::shared_ptr<Chunk>>> regions;
        for (const auto& chunk : chunks) {
            if (!chunk) {
                continue;
            }
            int rx = 0;
            int rz = 0;
            region_coords_for_chunk(chunk->get_coords(), rx, rz);
            regions[{rx, rz}].push_back(chunk);
        }

        std::vector<std::string> members = {"chunks/region/world-manifest.json"};
        for (const auto& [region_coords, region_chunks] : regions) {
            members.push_back(RelativeRegionMember(region_coords.first, region_coords.second));
        }
        TransactionScope scope(save_dir, members, errors);
        if (!scope.ok())
            return false;
        if (!validate_save(save_dir, errors))
            return false;
        const std::filesystem::path region_dir = region_directory(save_dir);
        std::filesystem::create_directories(region_dir);

        std::size_t written = 0;
        for (auto& [region_coords, region_chunks] : regions) {
            const std::filesystem::path path =
                region_file_path(save_dir, region_coords.first, region_coords.second);

            // Merge with existing on-disk records: chunks absent from memory
            // and far-LOD tile records (lod_level > 0) are preserved
            // verbatim; records for the chunks being written are replaced.
            std::vector<RegionRecord> records;
            std::error_code exists_error;
            if (std::filesystem::exists(path, exists_error) && !exists_error) {
                if (!ReadRegionFile(path, records, errors)) {
                    return false;
                }
            }
            std::map<std::pair<std::uint8_t, std::uint64_t>, std::size_t> record_index;
            for (std::size_t i = 0; i < records.size(); ++i) {
                record_index[{records[i].lod_level, records[i].id}] = i;
            }

            for (const auto& chunk : region_chunks) {
                RegionRecord record;
                record.id = chunk->get_id();
                record.lod_level = 0;

                std::uint8_t previous_flags = 0;
                const auto existing = record_index.find({record.lod_level, record.id});
                if (existing != record_index.end()) {
                    previous_flags = records[existing->second].flags;
                }
                record.flags = ChunkRecordFlags(*chunk, previous_flags);

                if (!CompressPayload(SerializeSingleChunkSnapshot(chunk), record, errors)) {
                    return false;
                }

                if (existing != record_index.end()) {
                    records[existing->second] = std::move(record);
                } else {
                    record_index[{record.lod_level, record.id}] = records.size();
                    records.push_back(std::move(record));
                }
            }

            if (!WriteRegionFile(path, records, errors)) {
                return false;
            }
            ++written;
        }

        if (!WriteWorldManifest(world_manifest_path(save_dir), errors)) {
            return false;
        }

        if (!scope.commit(errors))
            return false;
        if (regions_written) {
            *regions_written = written;
        }
        return true;
    } catch (const std::exception& e) {
        AddError(errors, std::string("failed to write world region files: ") + e.what());
        return false;
    }
}

} // namespace Luminumbra::Persistence
