#include "WorldSaveService.h"
#include "core/Profiler.h"

#include "../ecs/EntitySnapshot.h" //  plant entity snapshot persistence
#include "WorldPersistenceRoundtrip.h"
#include "core/FilesystemPath.h"
#include "luminumbra/core/Types.h"
#include "world/ActiveRegionLedger.h"
#include "world/Chunk.h"
#include "world/FarLodStore.h"
#include "world/WorldClock.h"
#include "world/WorldStreamingState.h"

#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"

#include <lz4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
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
// Existing LMR1 decompression bound, shared by streaming readers and writers.
constexpr std::uint32_t kMaxRecordBytes = 64u * 1024u * 1024u;

constexpr std::uint8_t kRecordFlagEdited = 0x01;
constexpr std::uint8_t kRecordFlagWaterPresent = 0x02;

struct FileIdentity {
#if defined(_WIN32)
    DWORD volume = 0;
    DWORD index_high = 0;
    DWORD index_low = 0;
#else
    dev_t device{};
    ino_t inode{};
#endif
    bool valid = false;
};

bool ReadFileIdentity(const std::filesystem::path& path, FileIdentity& identity) {
    // Inspect the leaf entry itself; parent symlinks still resolve normally.
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileW(path.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool read = ::GetFileInformationByHandle(handle, &information) != 0;
    const bool closed = ::CloseHandle(handle) != 0;
    if (!read || !closed)
        return false;
    identity.volume = information.dwVolumeSerialNumber;
    identity.index_high = information.nFileIndexHigh;
    identity.index_low = information.nFileIndexLow;
#else
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0)
        return false;
    identity.device = information.st_dev;
    identity.inode = information.st_ino;
#endif
    identity.valid = true;
    return true;
}

bool SameFileIdentity(const FileIdentity& current, const FileIdentity& expected) {
    if (!current.valid || !expected.valid)
        return false;
#if defined(_WIN32)
    return current.volume == expected.volume && current.index_high == expected.index_high &&
           current.index_low == expected.index_low;
#else
    return current.device == expected.device && current.inode == expected.inode;
#endif
}

bool MatchesFileIdentity(const std::filesystem::path& path, const FileIdentity& expected) {
    FileIdentity current;
    return ReadFileIdentity(path, current) && SameFileIdentity(current, expected);
}

bool WriteFileDurably(const std::filesystem::path& destination,
                      const std::string& bytes,
                      const std::filesystem::path& staging,
                      std::vector<std::string>* errors,
                      FileIdentity* published_identity = nullptr);

bool FlushAncestorEntries(const std::filesystem::path& destination,
                          std::vector<std::string>* errors);
bool FlushRemovalDirectory(const std::filesystem::path& destination,
                           std::vector<std::string>* errors);

void ValidateRegionDigestKey(world::RegionKey key) {
    // Packed chunk coordinates are [-2^20,2^20); reject unsupported public
    // region keys before multiplying bounds or reading any durable member.
    constexpr int minimum_key = -(1 << 20) / WorldSaveService::kRegionChunkSpan;
    constexpr int maximum_key = (1 << 20) / WorldSaveService::kRegionChunkSpan;
    if (key.x < minimum_key || key.x >= maximum_key || key.z < minimum_key ||
        key.z >= maximum_key) {
        throw std::invalid_argument("Invalid region digest key");
    }
}

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

// A process-wide lock also covers aliases of the same destination, nested pair
// saves, and region read/merge/write transactions. No path-keyed lock cache grows
// with the number of saves. Inputs owned by callers must remain stable for a call.
std::recursive_mutex& SaveMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

std::atomic<std::size_t>& WaitingSaveWriters() {
    static std::atomic<std::size_t> waiting{0};
    return waiting;
}

class WaitingSaveWriter {
public:
    WaitingSaveWriter() {
        WaitingSaveWriters().fetch_add(1, std::memory_order_release);
    }
    ~WaitingSaveWriter() {
        WaitingSaveWriters().fetch_sub(1, std::memory_order_release);
    }
};

using SaveGuard = WorldSaveService::SaveOwnership;

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

bool ReadWaterCursor(const nlohmann::json& metadata,
                     std::size_t* water_cursor,
                     std::string& error) {
    if (water_cursor)
        *water_cursor = 0;
    if (!metadata.contains("waterSimCursor"))
        return true;
    const auto& cursor = metadata.at("waterSimCursor");
    if (!cursor.is_number_unsigned() ||
        cursor.get<std::uint64_t>() > std::numeric_limits<std::size_t>::max()) {
        error = "Corrupt world metadata: invalid water simulation cursor.";
        return false;
    }
    if (water_cursor)
        *water_cursor = cursor.get<std::size_t>();
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

// SaveGuard spans enumeration through replacement/removal for every writer.
// Existing staging belongs to a completed or terminated save, never another
// active in-process writer. Independently launched processes sharing a save
// directory are outside the persistence ownership contract. Best effort: one
// pass over the staging directory, outside strict save scans.
std::size_t ReclaimAbandonedStaging(const std::filesystem::path& destination) {
    std::error_code list_error;
    const auto directory =
        destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
    // Compare native filename code units, including Unicode on Windows.
    auto prefix = destination.filename();
    prefix += ".tmp.";
    std::size_t reclaimed = 0;
    std::filesystem::directory_iterator it(directory, list_error);
    if (list_error)
        return 0;
    for (const std::filesystem::directory_iterator end; !list_error && it != end;
         it.increment(list_error)) {
        const auto& entry = *it;
        std::error_code kind_error;
        if (!entry.is_regular_file(kind_error) || kind_error)
            continue;
        if (entry.path().filename().native().rfind(prefix.native(), 0) != 0)
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

bool WriteRegionBytes(HANDLE file, std::string_view bytes, std::vector<std::string>* errors) {
    LUMIN_PROFILE_ZONE_N("File save write");
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
            AddError(errors,
                     "failed to write temporary region file: " + WindowsErrorMessage(error));
            return false;
        }
        offset += written;
    }

    return true;
}

bool RewindRegionFile(HANDLE file) {
    LARGE_INTEGER beginning{};
    return ::SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) != 0;
}

using RegionFileHandle = HANDLE;

bool OpenDurableRegionTemp(const std::filesystem::path& destination,
                           HANDLE& file,
                           std::filesystem::path& out_temp,
                           std::vector<std::string>* errors) {
    file = INVALID_HANDLE_VALUE;
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
                     "failed to create temporary region file: " +
                         Filesystem::GenericUtf8PathString(out_temp) + ": " +
                         WindowsErrorMessage(error));
            return false;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        AddError(errors,
                 "failed to allocate a unique temporary region file for: " +
                     Filesystem::GenericUtf8PathString(destination));
        return false;
    }

    return true;
}

void CloseRegionFile(HANDLE file) {
    ::CloseHandle(file);
}

bool FinishDurableRegionTemp(HANDLE file,
                             const std::filesystem::path& out_temp,
                             std::vector<std::string>* errors) {
    if (!::FlushFileBuffers(file)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(file);
        RemoveTemporaryFile(out_temp);
        AddError(errors,
                 "failed to durably flush temporary region file: " +
                     Filesystem::GenericUtf8PathString(out_temp) + ": " +
                     WindowsErrorMessage(error));
        return false;
    }
    if (!::CloseHandle(file)) {
        const DWORD error = ::GetLastError();
        RemoveTemporaryFile(out_temp);
        AddError(errors,
                 "failed to close temporary region file: " +
                     Filesystem::GenericUtf8PathString(out_temp) + ": " +
                     WindowsErrorMessage(error));
        return false;
    }
    return true;
}

bool ReplaceRegionFileAtomically(const std::filesystem::path& temp,
                                 const std::filesystem::path& destination,
                                 std::vector<std::string>* errors) {
    LUMIN_PROFILE_ZONE_N("File save replace");
    if (!FlushAncestorEntries(destination, errors))
        return false;
    const DWORD attributes = ::GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (::ReplaceFileW(destination.c_str(),
                           temp.c_str(),
                           nullptr,
                           REPLACEFILE_WRITE_THROUGH | REPLACEFILE_IGNORE_MERGE_ERRORS,
                           nullptr,
                           nullptr)) {
            return true;
        }
        const DWORD error = ::GetLastError();
        AddError(errors,
                 "failed to atomically replace region file: " +
                     Filesystem::GenericUtf8PathString(destination) + ": " +
                     WindowsErrorMessage(error));
        return false;
    }

    const DWORD attributes_error = ::GetLastError();
    if (attributes_error != ERROR_FILE_NOT_FOUND && attributes_error != ERROR_PATH_NOT_FOUND) {
        AddError(errors,
                 "failed to inspect region file before replacement: " +
                     Filesystem::GenericUtf8PathString(destination) + ": " +
                     WindowsErrorMessage(attributes_error));
        return false;
    }
    if (::MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    const DWORD error = ::GetLastError();
    AddError(errors,
             "failed to atomically install region file: " +
                 Filesystem::GenericUtf8PathString(destination) + ": " +
                 WindowsErrorMessage(error));
    return false;
}

#else

std::string PosixErrorMessage(int error) {
    return std::error_code(error, std::generic_category()).message();
}

bool WriteRegionBytes(int file, std::string_view bytes, std::vector<std::string>* errors) {
    LUMIN_PROFILE_ZONE_N("File save write");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const std::size_t requested = std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(
                (std::numeric_limits<decltype(::write(file, bytes.data(), 0))>::max)()));
        const auto written = ::write(file, bytes.data() + offset, requested);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            const int error = written < 0 ? errno : EIO;
            AddError(errors, "failed to write temporary region file: " + PosixErrorMessage(error));
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }

    return true;
}

bool RewindRegionFile(int file) {
    return ::lseek(file, 0, SEEK_SET) == 0;
}

using RegionFileHandle = int;

bool OpenDurableRegionTemp(const std::filesystem::path& destination,
                           int& file,
                           std::filesystem::path& out_temp,
                           std::vector<std::string>* errors) {
    file = -1;
    for (int attempt = 0; attempt < 128; ++attempt) {
        out_temp = NextRegionTempPath(destination);
        file = ::open(out_temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (file >= 0) {
            break;
        }
        if (errno != EEXIST) {
            const int error = errno;
            AddError(errors,
                     "failed to create temporary region file: " +
                         Filesystem::GenericUtf8PathString(out_temp) + ": " +
                         PosixErrorMessage(error));
            return false;
        }
    }
    if (file < 0) {
        AddError(errors,
                 "failed to allocate a unique temporary region file for: " +
                     Filesystem::GenericUtf8PathString(destination));
        return false;
    }

    return true;
}

void CloseRegionFile(int file) {
    ::close(file);
}

bool FinishDurableRegionTemp(int file,
                             const std::filesystem::path& out_temp,
                             std::vector<std::string>* errors) {
    while (::fsync(file) != 0) {
        if (errno == EINTR) {
            continue;
        }
        const int error = errno;
        ::close(file);
        RemoveTemporaryFile(out_temp);
        AddError(errors,
                 "failed to durably flush temporary region file: " +
                     Filesystem::GenericUtf8PathString(out_temp) + ": " + PosixErrorMessage(error));
        return false;
    }
    if (::close(file) != 0) {
        const int error = errno;
        RemoveTemporaryFile(out_temp);
        AddError(errors,
                 "failed to close temporary region file: " +
                     Filesystem::GenericUtf8PathString(out_temp) + ": " + PosixErrorMessage(error));
        return false;
    }
    return true;
}

bool ReplaceRegionFileAtomically(const std::filesystem::path& temp,
                                 const std::filesystem::path& destination,
                                 std::vector<std::string>* errors) {
    LUMIN_PROFILE_ZONE_N("File save replace");
    if (!FlushAncestorEntries(destination, errors))
        return false;
    // POSIX rename replaces an existing same-filesystem destination atomically.
    // World members stage in the save root outside the chunks scan; standalone
    // containers stage beside the file. Cross-filesystem rename fails safely.
    if (std::rename(temp.c_str(), destination.c_str()) != 0) {
        const int error = errno;
        AddError(errors,
                 "failed to atomically replace region file: " +
                     Filesystem::GenericUtf8PathString(destination) + ": " +
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
                     Filesystem::GenericUtf8PathString(parent));
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
                Filesystem::GenericUtf8PathString(parent) + ": " + PosixErrorMessage(error));
        return false;
    }
    while (::fsync(directory) != 0) {
        if (errno == EINTR)
            continue;
        const int error = errno;
        ::close(directory);
        AddError(errors,
                 "region file was replaced but its directory flush failed: " +
                     Filesystem::GenericUtf8PathString(parent) + ": " + PosixErrorMessage(error));
        return false;
    }
    if (::close(directory) != 0) {
        const int error = errno;
        AddError(errors,
                 "region directory close failed after durable replacement: " +
                     Filesystem::GenericUtf8PathString(parent) + ": " + PosixErrorMessage(error));
        return false;
    }
    return true;
}

// Flushes the directory that holds a newly created entry so the entry survives
// power loss.
bool FlushDirectoryEntry(const std::filesystem::path& directory_path,
                         std::vector<std::string>* errors) {
    if (ConsumeDirectoryFlushFailureForTesting()) {
        AddError(errors,
                 "directory flush failed (test seam): " +
                     Filesystem::GenericUtf8PathString(directory_path));
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
                 "cannot open directory for durable flush: " +
                     Filesystem::GenericUtf8PathString(directory_path) + ": " +
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
                 "directory flush failed: " + Filesystem::GenericUtf8PathString(directory_path) +
                     ": " + PosixErrorMessage(error));
        return false;
    }
    if (::close(directory) != 0) {
        const int error = errno;
        AddError(errors,
                 "directory close failed after flush: " +
                     Filesystem::GenericUtf8PathString(directory_path) + ": " +
                     PosixErrorMessage(error));
        return false;
    }
    return true;
}

#endif

bool RegionStagingDestination(const std::filesystem::path& destination,
                              std::filesystem::path& staging,
                              std::vector<std::string>* errors) {
    // Walk at most the path's component count. Stage on the same filesystem in
    // an existing ancestor; refusal before publication must not create directories.
    auto parent = destination.parent_path();
    while (!parent.empty()) {
        std::error_code error;
        const bool exists = std::filesystem::exists(parent, error);
        if (error) {
            AddError(errors, "cannot inspect staging directory: " + error.message());
            return false;
        }
        if (exists)
            break;
        const auto next = parent.parent_path();
        if (next == parent)
            return false;
        parent = next;
    }
    staging = parent / destination.filename();
    return true;
}

bool CreatePayloadDirectory(const std::filesystem::path& destination,
                            std::vector<std::string>* errors) {
    std::error_code error;
    if (destination.has_parent_path())
        std::filesystem::create_directories(destination.parent_path(), error);
    if (!error)
        return true;
    AddError(errors,
             "cannot create directory " +
                 Filesystem::GenericUtf8PathString(destination.parent_path()) + ": " +
                 error.message());
    return false;
}

template<typename Producer>
bool WriteDurableRegionTempWithProducer(const std::filesystem::path& destination,
                                        Producer&& produce,
                                        std::filesystem::path& out_temp,
                                        std::vector<std::string>* errors) {
    std::filesystem::path staging;
    if (!RegionStagingDestination(destination, staging, errors))
        return false;
    RegionFileHandle file{};
    if (!OpenDurableRegionTemp(staging, file, out_temp, errors))
        return false;
    bool produced = false;
    try {
        produced = produce(file);
    } catch (...) {
        CloseRegionFile(file);
        RemoveTemporaryFile(out_temp);
        throw;
    }
    if (!produced) {
        CloseRegionFile(file);
        RemoveTemporaryFile(out_temp);
        AddError(errors, "failed to produce temporary region contents");
        return false;
    }
    return FinishDurableRegionTemp(file, out_temp, errors);
}

bool WriteDurableRegionTemp(const std::filesystem::path& destination,
                            const std::string& bytes,
                            std::filesystem::path& out_temp,
                            std::vector<std::string>* errors) {
    return WriteDurableRegionTempWithProducer(
        destination,
        [&](auto file) { return WriteRegionBytes(file, bytes, errors); },
        out_temp,
        errors);
}
// Directory traversal is bounded by the supplied path's component count. Repeat
// it on retries even when a prior attempt already created the directories.
bool FlushAncestorEntries(const std::filesystem::path& destination,
                          std::vector<std::string>* errors) {
#if !defined(_WIN32)
    for (std::filesystem::path ancestor = destination.parent_path();
         !ancestor.empty() && ancestor != ancestor.root_path();
         ancestor = ancestor.parent_path()) {
        const auto holder =
            ancestor.has_parent_path() ? ancestor.parent_path() : std::filesystem::path(".");
        if (!FlushDirectoryEntry(holder, errors))
            return false;
    }
#else
    (void)destination;
    (void)errors;
#endif
    return true;
}

bool FlushRemovalDirectory(const std::filesystem::path& destination,
                           std::vector<std::string>* errors) {
#if !defined(_WIN32)
    const auto parent =
        destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
    std::error_code error;
    const bool exists = std::filesystem::exists(parent, error);
    if (error) {
        AddError(errors, "cannot inspect removal directory: " + error.message());
        return false;
    }
    // A wholly absent hierarchy is a clean miss. An existing parent must be
    // flushed even when unlink already succeeded on a previous attempt.
    return !exists || FlushDirectoryEntry(parent, errors);
#else
    (void)destination;
    (void)errors;
    return true;
#endif
}

// Holds a reference to the newly published file until source retirement. A
// failed source unlink rolls back only this publication. Process save ownership
// spans identity checking and removal, as it does all other persistence writers.
class NewFilePublication {
public:
    NewFilePublication(const std::filesystem::path& temp, const std::filesystem::path& destination)
        : m_temp(temp)
        , m_destination(destination) {}
    NewFilePublication(const NewFilePublication&) = delete;
    NewFilePublication& operator=(const NewFilePublication&) = delete;
    ~NewFilePublication() {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) {
            if (m_published && !m_committed) {
                FILE_DISPOSITION_INFO disposition{TRUE};
                ::SetFileInformationByHandle(m_handle,
                                             FileDispositionInfo,
                                             &disposition,
                                             static_cast<DWORD>(sizeof(disposition)));
            }
            ::CloseHandle(m_handle);
        }
#else
        if (m_published && !m_committed) {
            struct stat published {};
            struct stat named {};
            // Compare against our still-open file, immediately before unlink,
            // while process save ownership excludes other persistence writers.
            if (::fstat(m_descriptor, &published) == 0 &&
                ::lstat(m_destination.c_str(), &named) == 0 && published.st_dev == named.st_dev &&
                published.st_ino == named.st_ino) {
                ::unlink(m_destination.c_str());
                FlushRemovalDirectory(m_destination, nullptr);
            }
        }
        if (m_descriptor >= 0)
            ::close(m_descriptor);
#endif
        RemoveTemporaryFile(m_temp);
    }
    bool Publish(std::vector<std::string>* errors) {
        if (!FlushAncestorEntries(m_destination, errors))
            return false;
#if defined(_WIN32)
        // Keep identity through the move; rollback targets this handle, never a
        // replacement file found by looking up the destination a second time.
        m_handle = ::CreateFileW(m_temp.c_str(),
                                 DELETE | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
        if (m_handle == INVALID_HANDLE_VALUE ||
            !::MoveFileExW(m_temp.c_str(), m_destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            AddError(errors,
                     "cannot publish an unoccupied destination: " +
                         WindowsErrorMessage(::GetLastError()));
            return false;
        }
        m_published = true;
        return true;
#else
        m_descriptor = ::open(m_temp.c_str(), O_RDONLY);
        if (m_descriptor < 0) {
            AddError(errors, "cannot retain publication identity: " + PosixErrorMessage(errno));
            return false;
        }
        // Native exclusive rename also supports filesystems without hard links.
        // Other POSIX hosts use link, whose publication atomically refuses every
        // occupied entry, including dangling symlinks. No overwriting fallback.
#if defined(__linux__)
        int installed = static_cast<int>(::syscall(SYS_renameat2,
                                                   AT_FDCWD,
                                                   m_temp.c_str(),
                                                   AT_FDCWD,
                                                   m_destination.c_str(),
                                                   RENAME_NOREPLACE));
        if (installed != 0 && (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP))
            installed = ::link(m_temp.c_str(), m_destination.c_str());
#elif defined(__APPLE__)
        const int installed = ::renamex_np(m_temp.c_str(), m_destination.c_str(), RENAME_EXCL);
#else
        const int installed = ::link(m_temp.c_str(), m_destination.c_str());
#endif
        if (installed != 0) {
            AddError(errors,
                     "cannot publish an unoccupied destination: " + PosixErrorMessage(errno));
            return false;
        }
        m_published = true;
        return FlushRemovalDirectory(m_destination, errors);
#endif
    }
    void Commit() {
        m_committed = true;
    }

private:
    const std::filesystem::path& m_temp;
    const std::filesystem::path& m_destination;
    bool m_published = false;
    bool m_committed = false;
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_descriptor = -1;
#endif
};

bool CompressPayload(const std::string& payload,
                     RegionRecord& record,
                     std::vector<std::string>* errors) {
    if (payload.empty() || payload.size() > kMaxRecordBytes) {
        AddError(errors, "region record payload exceeds the 64 MiB container bound");
        return false;
    }
    const int bound = LZ4_compressBound(static_cast<int>(payload.size()));
    std::string compressed(static_cast<std::size_t>(bound), '\0');
    const int written = LZ4_compress_default(
        payload.data(), compressed.data(), static_cast<int>(payload.size()), bound);
    if (written <= 0 || static_cast<std::size_t>(written) > kMaxRecordBytes) {
        AddError(errors, "LZ4 compression of a region record failed or exceeded its bound");
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

struct RegionIndex {
    struct Record {
        u64 id = 0;
        u8 lod_level = 0;
        u8 flags = 0;
        u32 raw_size = 0;
        u32 compressed_size = 0;
        std::streamoff offset = 0;
    };
    std::filesystem::path path;
    Filesystem::ReplacementSafeInputFile input;
    // LMR1's u16 count bounds the manifest to 65,535 entries.
    std::vector<Record> records;
    std::size_t next = 0;
};

bool ReadRegionIndex(const std::filesystem::path& path,
                     RegionIndex& index,
                     std::vector<std::string>* errors,
                     const std::stop_token& stop = {}) {
    LUMIN_PROFILE_ZONE_N("File save read");
    index = {};
    index.path = path;
    index.input.open(path);
    const auto reject = [&](const char* message) {
        AddError(errors, std::string(message) + ": " + Filesystem::GenericUtf8PathString(path));
        return false;
    };
    if (!index.input.is_open())
        return reject("failed to open region file");
    std::error_code size_error;
    const auto file_bytes = index.input.size(size_error);
    if (size_error)
        return reject("failed to read region file extent");
    if (file_bytes < kRegionFileHeaderSize ||
        file_bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max()))
        return reject("corrupt region file length");
    std::array<unsigned char, kRegionFileHeaderSize> header{};
    if (!index.input.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        std::memcmp(header.data(), kRegionMagic, sizeof(kRegionMagic)) != 0)
        return reject("region file is missing the LMR1 magic");
    const auto version = ReadU16(header.data() + 4);
    if (version != kRegionVersion) {
        AddError(errors,
                 version < kRegionVersion
                     ? WorldSaveService::kObsoleteWorldMessage
                     : "unsupported future LMR1 container version " + std::to_string(version) +
                           ": " + Filesystem::GenericUtf8PathString(path));
        return false;
    }
    const auto count = ReadU16(header.data() + 6);
    // The u16 record count bounds this manifest to 65,535 entries. Payload
    // lengths and the file extent are checked before allocating any payload.
    std::uintmax_t offset =
        kRegionFileHeaderSize + static_cast<std::size_t>(count) * kRecordHeaderSize;
    if (offset > file_bytes)
        return reject("region file record manifest is truncated");
    index.records.resize(count);
    std::set<std::pair<u8, u64>> keys;
    for (auto& record : index.records) {
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
        std::array<unsigned char, kRecordHeaderSize> record_header{};
        if (!index.input.read(reinterpret_cast<char*>(record_header.data()), record_header.size()))
            return reject("region file record manifest is truncated");
        record.id = ReadU64(record_header.data());
        record.lod_level = record_header[8];
        record.flags = record_header[9];
        record.raw_size = ReadU32(record_header.data() + 10);
        record.compressed_size = ReadU32(record_header.data() + 14);
        if (record.lod_level > 2 || (record.flags & ~3u) != 0 ||
            !keys.emplace(record.lod_level, record.id).second)
            return reject("corrupt or unknown region record header");
        if (record.compressed_size > file_bytes - offset)
            return reject("region file record payload is truncated");
        if (record.raw_size == 0 || record.raw_size > kMaxRecordBytes ||
            record.compressed_size == 0 || record.compressed_size > kMaxRecordBytes)
            return reject("corrupt region record payload length");
        record.offset = static_cast<std::streamoff>(offset);
        offset += record.compressed_size;
    }
    return offset == file_bytes || reject("region file has trailing bytes after the last record");
}

bool ReadIndexedRecord(RegionIndex& index,
                       const RegionIndex::Record& entry,
                       RegionRecord& record,
                       std::vector<std::string>* errors) {
    if (entry.compressed_size == 0 || entry.compressed_size > kMaxRecordBytes ||
        entry.raw_size == 0 || entry.raw_size > kMaxRecordBytes) {
        AddError(errors, "corrupt region record payload length");
        return false;
    }
    record = {entry.id, entry.lod_level, entry.flags, entry.raw_size, {}};
    record.compressed_payload.resize(entry.compressed_size);
    if (!index.input.seek(static_cast<std::uint64_t>(entry.offset)) ||
        !index.input.read(record.compressed_payload.data(), entry.compressed_size)) {
        AddError(errors,
                 "failed to read region record: " + Filesystem::GenericUtf8PathString(index.path));
        return false;
    }
    return true;
}

// Current lod-0 records retain canonical in-memory chunk serialization.
std::string SerializeSingleChunkSnapshot(const std::shared_ptr<Chunk>& chunk,
                                         bool preserve_authority) {
    WorldStreamingState single;
    single.insert_chunk(chunk);
    return SerializeWorldStreamingStateSnapshotJson(single, preserve_authority);
}

std::shared_ptr<Chunk> DecodeChunkRecord(const RegionRecord& record,
                                         const std::filesystem::path& path,
                                         std::vector<std::string>* errors,
                                         bool migrate_legacy_water = false) {
    std::string payload;
    if (!DecompressPayload(record, payload, errors)) {
        return nullptr;
    }

    WorldStreamingState single;
    std::vector<std::string> load_errors;
    if (!LoadWorldStreamingStateSnapshotJson(
            payload,
            single,
            load_errors,
            {.terrain_edited = (record.flags & kRecordFlagEdited) != 0u,
             .migrate_legacy_water = migrate_legacy_water,
             .validate_record_authority = true})) {
        for (std::string& error : load_errors) {
            AddError(errors, std::move(error));
        }
        AddError(errors,
                 "failed to decode chunk record payload: " +
                     Filesystem::GenericUtf8PathString(path));
        return nullptr;
    }
    const auto chunks = single.snapshot_chunks();
    if (chunks.size() != 1 || !chunks.front()) {
        AddError(errors,
                 "chunk record payload does not contain exactly one chunk: " +
                     Filesystem::GenericUtf8PathString(path));
        return nullptr;
    }
    if (chunks.front()->get_id() != record.id) {
        AddError(errors,
                 "chunk record id does not match its payload: " +
                     Filesystem::GenericUtf8PathString(path));
        return nullptr;
    }
    return chunks.front();
}

std::uint8_t ChunkRecordFlags(const Chunk& chunk, std::uint8_t previous_flags) {
    std::uint8_t flags = 0;
    // Edited/authoritative is sticky: once a chunk record was persisted with
    // post-generation edits it stays authoritative even after the in-memory
    // dirty flag is cleared by the save.
    if (chunk.has_simulation_authority() || (previous_flags & kRecordFlagEdited) != 0) {
        flags |= kRecordFlagEdited;
    }
    if (chunk.has_water_sim.load(std::memory_order_acquire) || !chunk.water_mesh_vertices.empty()) {
        flags |= kRecordFlagWaterPresent;
    }
    return flags;
}

struct RegionReplacement {
    u64 id = 0;
    u8 lod_level = 0;
    bool preserve_authority = false;
    std::shared_ptr<Chunk> chunk;
    const WorldSaveService::ContainerRecord* container = nullptr;
    RegionIndex* source = nullptr;
    std::size_t source_record = 0;
    bool omit_water = false;
};

bool RewriteRegionFile(const std::filesystem::path& path,
                       const std::vector<RegionReplacement>& replacements,
                       std::vector<std::string>* errors) {
    RegionIndex previous;
    if (std::filesystem::exists(path) && !ReadRegionIndex(path, previous, errors))
        return false;
    struct Entry {
        RegionIndex::Record record;
        std::optional<std::size_t> replacement;
    };
    // LMR1's u16 manifest bounds both maps; no historical payload is retained.
    std::map<std::pair<u8, u64>, Entry> entries;
    for (const auto& record : previous.records)
        entries.emplace(std::pair{record.lod_level, record.id}, Entry{record, std::nullopt});
    for (std::size_t i = 0; i < replacements.size(); ++i) {
        const auto& replacement = replacements[i];
        const auto key = std::pair{replacement.lod_level, replacement.id};
        auto found = entries.find(key);
        if (found != entries.end() && (found->second.record.flags & kRecordFlagEdited) != 0 &&
            replacement.preserve_authority && replacement.chunk &&
            !replacement.chunk->has_simulation_authority()) {
            AddError(errors, "regenerable chunk cannot replace durable simulation authority");
            return false;
        }
        if (found == entries.end()) {
            if (entries.size() == std::numeric_limits<u16>::max()) {
                AddError(errors, "region file exceeds the 65535-record limit");
                return false;
            }
            found = entries
                        .emplace(key,
                                 Entry{RegionIndex::Record{.id = replacement.id,
                                                           .lod_level = replacement.lod_level},
                                       std::nullopt})
                        .first;
        }
        found->second.replacement = i;
    }
    std::filesystem::path temp;
    const auto staging = WorldSaveService::region_staging_path(path);
    ReclaimAbandonedStaging(staging);
    const bool written = WriteDurableRegionTempWithProducer(
        staging,
        [&](auto file) {
            // At most 8 + 65,535 * 18 bytes. Reserve the manifest, stream each
            // bounded payload, then backpatch its exact compressed lengths.
            std::string manifest(kRegionFileHeaderSize + entries.size() * kRecordHeaderSize, '\0');
            if (!WriteRegionBytes(file, manifest, errors))
                return false;
            std::uintmax_t total_bytes = manifest.size();
            manifest.clear();
            manifest.append(kRegionMagic, sizeof(kRegionMagic));
            AppendU16(manifest, kRegionVersion);
            AppendU16(manifest, static_cast<u16>(entries.size()));
            for (const auto& [key, entry] : entries) {
                RegionRecord record;
                if (!entry.replacement) {
                    if (!ReadIndexedRecord(previous, entry.record, record, errors))
                        return false;
                } else {
                    const auto& replacement = replacements[*entry.replacement];
                    if (replacement.source) {
                        if (!ReadIndexedRecord(
                                *replacement.source,
                                replacement.source->records[replacement.source_record],
                                record,
                                errors))
                            return false;
                    } else {
                        record.id = key.second;
                        record.lod_level = key.first;
                        if (replacement.chunk) {
                            record.flags =
                                replacement.omit_water
                                    ? (replacement.chunk->sdf_provenance() ==
                                               ChunkSdfProvenance::LoadedOrEdited
                                           ? kRecordFlagEdited
                                           : 0)
                                    : ChunkRecordFlags(*replacement.chunk, entry.record.flags);
                            const auto payload =
                                replacement.omit_water
                                    ? SerializeChunkAuthoritySnapshotJson(*replacement.chunk, true)
                                    : SerializeSingleChunkSnapshot(replacement.chunk,
                                                                   replacement.preserve_authority);
                            if (!CompressPayload(payload, record, errors))
                                return false;
                        } else {
                            record.flags = replacement.container->flags;
                            if (!CompressPayload(replacement.container->payload, record, errors))
                                return false;
                        }
                    }
                }
                const auto size = record.compressed_payload.size();
                if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max()) -
                               total_bytes) {
                    AddError(errors, "region file exceeds the supported stream extent");
                    return false;
                }
                total_bytes += size;
                if (!WriteRegionBytes(file, record.compressed_payload, errors))
                    return false;
                AppendU64(manifest, record.id);
                manifest.push_back(static_cast<char>(record.lod_level));
                manifest.push_back(static_cast<char>(record.flags));
                AppendU32(manifest, record.uncompressed_size);
                AppendU32(manifest, static_cast<u32>(record.compressed_payload.size()));
            }
            return RewindRegionFile(file) && WriteRegionBytes(file, manifest, errors);
        },
        temp,
        errors);
    // Release this writer's source descriptors. Independent readers finish their
    // immutable old image; all LMR1 readers share deletion on Windows.
    previous.input.close();
    for (const auto& replacement : replacements) {
        if (replacement.source && replacement.source->input.is_open())
            replacement.source->input.close();
    }
    if (!written)
        return false;
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel)) {
        RemoveTemporaryFile(temp);
        AddError(errors, "region replacement interrupted by test seam before replacing");
        return false;
    }
    if (!CreatePayloadDirectory(path, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
    if (!ReplaceRegionFileAtomically(temp, path, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
    return true;
}

bool ParseRegionFileName(const std::filesystem::path& path, int& rx, int& rz) {
    if (path.extension() != kRegionFileExtension)
        return false;
    const std::string stem = Filesystem::GenericUtf8PathString(path.stem());
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
    const auto staging = WorldSaveService::region_staging_path(manifest_path);
    ReclaimAbandonedStaging(staging);
    if (!WriteDurableRegionTemp(staging, bytes, temp, errors))
        return false;
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel)) {
        RemoveTemporaryFile(temp);
        AddError(errors, "interrupted before world manifest replacement");
        return false;
    }
    if (!CreatePayloadDirectory(manifest_path, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
    if (!ReplaceRegionFileAtomically(temp, manifest_path, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
    return true;
}

bool OpenAuthorityIndex(const std::filesystem::path& region_file, RegionIndex& reader) {
    reader = {};
    reader.path = region_file;
    std::error_code error;
    const bool exists = std::filesystem::exists(region_file, error);
    if (error)
        return false;
    if (!exists)
        return true;
    if (!ReadRegionIndex(region_file, reader, nullptr))
        return false;
    int expected_x = 0, expected_z = 0;
    if (!ParseRegionFileName(region_file, expected_x, expected_z))
        return false;
    // Old saves have no water-authority marker. Their initialized simulation
    // grids are durable input too; a water mesh alone is only a render cache.
    // This cold index pass is bounded by LMR1's 65,535 records per region.
    bool valid = true;
    std::erase_if(reader.records, [&](const auto& entry) {
        if (entry.lod_level != 0)
            return true;
        if ((entry.flags & kRecordFlagEdited) != 0)
            return false;
        if ((entry.flags & kRecordFlagWaterPresent) == 0)
            return true;
        RegionRecord raw;
        if (!ReadIndexedRecord(reader, entry, raw, nullptr)) {
            valid = false;
            return true;
        }
        const auto chunk = DecodeChunkRecord(raw, reader.path, nullptr, true);
        if (!chunk) {
            valid = false;
            return true;
        }
        return !chunk->has_simulation_authority();
    });
    if (!valid)
        return false;
    for (const auto& record : reader.records) {
        int actual_x = 0, actual_z = 0;
        WorldSaveService::region_coords_for_chunk(Chunk::decode_id(record.id), actual_x, actual_z);
        if (actual_x != expected_x || actual_z != expected_z)
            return false;
    }
    std::sort(reader.records.begin(), reader.records.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.id < rhs.id;
    });
    return true;
}

} // namespace

WorldSaveService::SaveOwnership::SaveOwnership()
    : m_lock(SaveMutex(), std::defer_lock) {
    if (!m_lock.try_lock()) {
        const WaitingSaveWriter waiting;
        m_lock.lock();
    }
}

std::filesystem::path
WorldSaveService::region_staging_path(const std::filesystem::path& destination) {
    // Every region member lives at save/chunks/region/<filename>. Stage on the
    // save filesystem outside both strictly scanned directories, like the ledger.
    const auto directory = destination.parent_path();
    if (directory.filename() == kRegionDirectoryName &&
        directory.parent_path().filename() == kChunksDirectoryName)
        // Remove layout components without traversing a region symlink's parents
        // or collapsing symlink/.. components in the caller's save-root prefix.
        return directory.parent_path().parent_path() / destination.filename();
    // Raw container access also supports standalone files outside a world save.
    return destination;
}

bool WorldSaveService::save_metadata(const std::string& bytes,
                                     const std::filesystem::path& save_dir,
                                     std::vector<std::string>* errors) {
    const SaveGuard guard;
    return validate_save(save_dir, errors) && save_metadata_validated(bytes, save_dir, errors);
}

bool WorldSaveService::save_metadata_validated(const std::string& bytes,
                                               const std::filesystem::path& save_dir,
                                               std::vector<std::string>* errors) {
    // Metadata-only writes require an existing root. Ancestor staging must not
    // turn a missing legacy destination into a newly created save directory.
    const auto directory = save_dir.empty() ? std::filesystem::path(".") : save_dir;
    std::error_code directory_error;
    if (!std::filesystem::is_directory(directory, directory_error) || directory_error) {
        AddError(errors, "metadata destination is not an accessible directory");
        return false;
    }
    world::WorldClock clock;
    std::string clock_error;
    const auto metadata = nlohmann::json::parse(bytes, nullptr, false);
    // Clock and water cursor fields opt into structured validation. Other legacy
    // input remains opaque; the existing destination is still validated.
    const bool has_clock = metadata.contains("simulationTick") || metadata.contains("calendar");
    std::optional<Vec3> ambient_anchor;
    if ((has_clock && (!world::WorldClock::from_metadata(metadata, clock, clock_error) ||
                       !ReadAmbientAnchor(metadata, ambient_anchor, clock_error))) ||
        !ReadWaterCursor(metadata, nullptr, clock_error)) {
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
    ReclaimAbandonedStaging(destination);
    std::filesystem::path temp;
    if (!WriteDurableRegionTemp(destination, bytes, temp, errors))
        return false;
    if (InterruptBeforeRegionReplaceForTesting().exchange(false)) {
        RemoveTemporaryFile(temp);
        AddError(errors, "interrupted before metadata replacement");
        return false;
    }
    if (!ReplaceRegionFileAtomically(temp, destination, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
    return true;
}

bool WorldSaveService::save_metadata_and_active_regions(const std::string& bytes,
                                                        world::ActiveRegionLedger& ledger,
                                                        const std::filesystem::path& save_dir,
                                                        std::vector<std::string>* errors) {
    const SaveGuard guard;
    return validate_save(save_dir, errors) && save_pair_validated(bytes, ledger, save_dir, errors);
}

bool WorldSaveService::save_pair_validated(const std::string& bytes,
                                           world::ActiveRegionLedger& ledger,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors) {
    const SaveGuard guard;
    const auto metadata = nlohmann::json::parse(bytes, nullptr, false);
    world::WorldClock clock;
    std::optional<Vec3> ambient_anchor;
    std::string error;
    if (!world::WorldClock::from_metadata(metadata, clock, error) ||
        !ReadAmbientAnchor(metadata, ambient_anchor, error) ||
        !ReadWaterCursor(metadata, nullptr, error)) {
        AddError(errors, error);
        return false;
    }
    // Decode validates record stamps; empty ledgers need the same clock check explicitly.
    if (ledger.header_tick() != clock.tick()) {
        AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
    auto saved = ledger;
    saved.mark_saved(clock);
    world::ActiveRegionLedger checked;
    if (!world::ActiveRegionLedger::decode(saved.encode(), checked, error)) {
        AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
    world::WorldClock previous_clock;
    bool requires_active_regions = false;
    if (!read_clock_metadata(save_dir, previous_clock, requires_active_regions, errors))
        return false;
    // Each writer validates the installed pair. On rewind, replace the ledger
    // first so it cannot be newer than the clock, even if metadata writing fails.
    if (clock.tick() < previous_clock.tick())
        return save_regions_validated(ledger, clock, save_dir, errors) &&
               save_metadata_validated(bytes, save_dir, errors);
    return save_metadata_validated(bytes, save_dir, errors) &&
           save_regions_validated(ledger, clock, save_dir, errors);
}

bool WorldSaveService::read_clock_metadata(const std::filesystem::path& save_dir,
                                           world::WorldClock& clock,
                                           bool& requires_active_regions,
                                           std::vector<std::string>* errors,
                                           std::optional<Vec3>* ambient_anchor,
                                           std::size_t* water_cursor) {
    LUMIN_PROFILE_ZONE_N("File clock metadata load");
    clock = world::WorldClock{};
    requires_active_regions = false;
    if (ambient_anchor)
        ambient_anchor->reset();
    if (water_cursor)
        *water_cursor = 0;
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
            !ReadAmbientAnchor(metadata, anchor, error) ||
            !ReadWaterCursor(metadata, water_cursor, error)) {
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
    const SaveGuard guard;
    return validate_save(save_dir, errors) && save_plants_validated(snapshot, save_dir, errors);
}

bool WorldSaveService::save_plants_validated(
    const Luminumbra::Ecs::EntityRegistrySnapshot& snapshot,
    const std::filesystem::path& save_dir,
    std::vector<std::string>* errors) {
    LUMIN_PROFILE_ZONE_N("File plant entities save");
    const std::filesystem::path path = plant_entities_path(save_dir);
    // Like ledger staging, abandoned plant copies must live outside the strict
    // region scan. Reclaim them even when the new roster is empty.
    const auto staging = save_dir / path.filename();
    ReclaimAbandonedStaging(staging);
    std::error_code ec;
    if (snapshot.entities.empty()) {
        // No plants -> no file (a no-plant save is byte-identical). Retire any stale file.
        std::filesystem::remove(path, ec);
        if (ec) {
            AddError(errors,
                     "failed to remove plant entities file: " +
                         Filesystem::GenericUtf8PathString(path) + ": " + ec.message());
            return false;
        }
        return FlushRemovalDirectory(path, errors);
    }
    return WriteFileDurably(
        path, Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(snapshot), staging, errors);
}

bool WorldSaveService::load_plant_entities(Luminumbra::Ecs::EntityRegistrySnapshot& out,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors) {
    LUMIN_PROFILE_ZONE_N("File plant entities load");
    out = Luminumbra::Ecs::EntityRegistrySnapshot{};
    const std::filesystem::path path = plant_entities_path(save_dir);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true; // clean miss: a fresh / no-plant world
    }
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        AddError(errors,
                 "failed to open plant entities file: " + Filesystem::GenericUtf8PathString(path));
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
    LUMIN_PROFILE_ZONE_N("File active regions load");
    try {
        const auto path = active_regions_path(save_dir);
        if (!std::filesystem::exists(path)) {
            ledger = world::ActiveRegionLedger(ledger.config(), absent_clock);
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
    const SaveGuard guard;
    return validate_save(save_dir, errors) &&
           save_regions_validated(ledger, clock, save_dir, errors);
}

bool WorldSaveService::save_regions_validated(world::ActiveRegionLedger& ledger,
                                              const world::WorldClock& clock,
                                              const std::filesystem::path& save_dir,
                                              std::vector<std::string>* errors) {
    // Empty ledgers have no last_save stamps to validate against the ceiling.
    // Require the acknowledged clock before either writing or taking the no-op path.
    if (ledger.header_tick() != clock.tick()) {
        AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
    const auto staging = save_dir / active_regions_path(save_dir).filename();
    ReclaimAbandonedStaging(staging);
    // Keep an anchor-only ledger: it restores last walking feet before activation.
    if (ledger.records().empty() && !ledger.local_anchor() &&
        !std::filesystem::exists(active_regions_path(save_dir)))
        return true;
    auto saved = ledger;
    saved.mark_saved(clock);
    const auto bytes = saved.encode();
    world::ActiveRegionLedger checked;
    std::string error;
    if (!world::ActiveRegionLedger::decode(bytes, checked, error)) {
        AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
    try {
        const auto path = active_regions_path(save_dir);
        std::filesystem::path temp;
        // Keep abandoned staging files outside the strict region integrity scan.
        // The save root is on the save filesystem; replacement refuses EXDEV
        // if a separately mounted chunks/region/ directory violates that layout.
        if (!WriteDurableRegionTemp(staging, bytes, temp, errors))
            return false;
        if (const auto hook = BeforeActiveRegionsReplaceForTesting(); hook && !hook()) {
            RemoveTemporaryFile(temp);
            AddError(errors, "interrupted before active-region ledger replacement");
            return false;
        }
        if (!CreatePayloadDirectory(path, errors)) {
            RemoveTemporaryFile(temp);
            return false;
        }
        if (!ReplaceRegionFileAtomically(temp, path, errors)) {
            RemoveTemporaryFile(temp);
            return false;
        }
        ledger = std::move(saved);
        return true;
    } catch (const std::exception& e) {
        AddError(errors, std::string("Cannot save active-region ledger: ") + e.what());
        return false;
    }
}

RegionContentHasher WorldSaveService::region_content_hasher(const std::filesystem::path& save_dir,
                                                            world::RegionKey key,
                                                            const RegionDigestInputs& inputs) {
    ValidateRegionDigestKey(key);
    WorldStreamingState region;
    // Filtering precedes insertion: incomplete streaming inputs must not shadow
    // durable edits. Complete retained authority is independent of render lifecycle.
    // insert_chunk preserves the first record per id.
    const auto qualifies = [&key](const std::shared_ptr<Chunk>& chunk) {
        if (!chunk || !chunk->has_simulation_authority())
            return false;
        int rx = 0, rz = 0;
        region_coords_for_chunk(chunk->get_coords(), rx, rz);
        return rx == key.x && rz == key.z;
    };
    for (const auto& chunk : inputs.live) {
        if (qualifies(chunk) &&
            (inputs.complete_authority || chunk->get_state() != ChunkState::Loading))
            region.insert_chunk(chunk);
    }
    for (const auto& chunk : inputs.parked) {
        if (qualifies(chunk) &&
            (inputs.complete_authority || chunk->get_state() != ChunkState::Loading))
            region.insert_chunk(chunk);
    }
    auto live = region.snapshot_chunks();
    std::sort(live.begin(), live.end(), [](const auto& lhs, const auto& rhs) {
        return lhs->get_id() < rhs->get_id();
    });
    AuthorityReader reader;
    if (!save_dir.empty() &&
        !open_region_authority(region_file_path(save_dir, key.x, key.z), reader))
        throw std::runtime_error("Cannot read durable region records");
    RegionContentHasher hasher;
    std::size_t live_index = 0;
    while (auto durable = read_next_authority(reader)) {
        while (live_index < live.size() && live[live_index]->get_id() < durable->get_id())
            hasher.append(*live[live_index++]);
        if (live_index < live.size() && live[live_index]->get_id() == durable->get_id())
            hasher.append(*live[live_index++]);
        else
            hasher.append(*durable);
    }
    while (live_index < live.size())
        hasher.append(*live[live_index++]);
    return hasher;
}

std::uint64_t WorldSaveService::region_simulation_digest(const std::filesystem::path& save_dir,
                                                         world::RegionKey key,
                                                         const RegionDigestInputs& inputs) {
    return region_content_hasher(save_dir, key, inputs).value();
}

std::uint64_t
WorldSaveService::region_simulation_digest(const std::filesystem::path& save_dir,
                                           world::RegionKey key,
                                           const std::vector<std::shared_ptr<Chunk>>& live_chunks) {
    ValidateRegionDigestKey(key);
    return region_simulation_digest(save_dir, key, RegionDigestInputs{live_chunks, {}});
}

std::uint64_t WorldSaveService::region_simulation_digest(
    const std::filesystem::path& save_dir,
    world::RegionKey key,
    std::initializer_list<std::shared_ptr<Chunk>> live_chunks) {
    ValidateRegionDigestKey(key);
    return region_simulation_digest(
        save_dir, key, RegionDigestInputs{std::vector<std::shared_ptr<Chunk>>(live_chunks), {}});
}

void WorldSaveService::region_coords_for_chunk(const IVec3& chunk_coords,
                                               int& out_rx,
                                               int& out_rz) {
    out_rx = FloorDiv(chunk_coords.x, kRegionChunkSpan);
    out_rz = FloorDiv(chunk_coords.z, kRegionChunkSpan);
}

bool WorldSaveService::has_world_save(const std::filesystem::path& save_dir) {
    WorldStreamingState state;
    std::vector<std::string> errors;
    return WorldSaveService{}.load_world(state, save_dir, errors, {}, 0);
}

bool WorldSaveService::has_chunk_snapshot(const std::filesystem::path& save_dir) {
    WorldStreamingState state;
    std::vector<std::string> errors;
    return WorldSaveService{}.load_world(state, save_dir, errors, {}, 1) && !state.empty();
}

bool WorldSaveService::validate_save(const std::filesystem::path& save_dir,
                                     std::vector<std::string>* errors,
                                     const std::stop_token& stop) {
    WorldStreamingState state;
    std::vector<std::string> diagnostics;
    WorldSaveService{}.load_world(state, save_dir, diagnostics, stop, 0);
    for (const auto& message : diagnostics)
        AddError(errors, message);
    return diagnostics.empty();
}

std::optional<WorldSaveService::SaveBatch>
WorldSaveService::SaveBatch::begin(const std::filesystem::path& directory,
                                   std::vector<std::string>* errors) {
    if (directory.empty())
        return std::nullopt;
    SaveOwnership ownership;
    WorldStreamingState probe;
    std::vector<std::string> diagnostics;
    const bool present = WorldSaveService{}.load_world(probe, directory, diagnostics, {}, 1);
    if (!diagnostics.empty()) {
        for (const auto& message : diagnostics)
            AddError(errors, message);
        return std::nullopt;
    }
    return SaveBatch(directory, present, !probe.empty(), std::move(ownership));
}

bool WorldSaveService::SaveBatch::save_world(const WorldStreamingState& state,
                                             std::vector<std::string>* errors,
                                             bool preserve_authority) {
    m_valid =
        m_valid && WorldSaveService{}.write_snapshot(
                       state.snapshot_chunks(), m_directory, errors, nullptr, preserve_authority);
    return m_valid;
}

WorldSaveDirtyReport WorldSaveService::SaveBatch::save_dirty_chunks(
    WorldStreamingState& state, std::vector<std::string>* errors, bool preserve_authority) {
    if (!m_valid)
        return {};
    auto report = WorldSaveService{}.save_dirty_validated(
        state, m_directory, errors, preserve_authority, false);
    m_valid = report.saved || report.chunks_dirty == 0;
    return report;
}

bool WorldSaveService::SaveBatch::save_plant_entities(const Ecs::EntityRegistrySnapshot& snapshot,
                                                      std::vector<std::string>* errors) {
    m_valid = m_valid && save_plants_validated(snapshot, m_directory, errors);
    return m_valid;
}

bool WorldSaveService::SaveBatch::save_metadata(const std::string& bytes,
                                                std::vector<std::string>* errors) {
    m_valid = m_valid && save_metadata_validated(bytes, m_directory, errors);
    return m_valid;
}

bool WorldSaveService::SaveBatch::save_water_cursor(std::size_t cursor,
                                                    std::vector<std::string>* errors) {
    if (!m_valid)
        return false;
    try {
        const auto path = m_directory / "world_info.json";
        nlohmann::json metadata = {{"container_version", kContainerVersion}};
        if (std::filesystem::exists(path)) {
            std::ifstream input(path, std::ios::binary);
            metadata = nlohmann::json::parse(input);
            std::size_t previous = 0;
            std::string error;
            if (!metadata.is_object() || !ReadWaterCursor(metadata, &previous, error)) {
                AddError(errors, "corrupt water scheduler metadata: " + error);
                m_valid = false;
                return false;
            }
            if (previous == cursor)
                return true;
        } else if (cursor == 0) {
            return true;
        }
        metadata["waterSimCursor"] = cursor;
        // The batch validated existing metadata before any payload. All fields
        // except this size_t cursor remain intact, including clocks and unknown
        // extensions. The durable writer also supports a new cursor-only root.
        m_valid = write_file_durably(path, metadata.dump(4) + "\n", errors);
        return m_valid;
    } catch (const std::exception& error) {
        AddError(errors, std::string("cannot persist water scheduler: ") + error.what());
        m_valid = false;
        return false;
    }
}

bool WorldSaveService::SaveBatch::save_metadata_and_active_regions(
    const std::string& bytes, world::ActiveRegionLedger& ledger, std::vector<std::string>* errors) {
    m_valid = m_valid && save_pair_validated(bytes, ledger, m_directory, errors);
    return m_valid;
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
    if (!exists)
        return true; // clean miss

    RegionIndex index;
    if (!ReadRegionIndex(region_file, index, errors)) {
        return false;
    }
    out_records.reserve(index.records.size());
    for (const auto& entry : index.records) {
        RegionRecord raw;
        if (!ReadIndexedRecord(index, entry, raw, errors)) {
            out_records.clear();
            return false;
        }
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
    if (!exists)
        return true; // clean miss

    RegionIndex index;
    if (!ReadRegionIndex(region_file, index, errors)) {
        return false;
    }
    for (const auto& entry : index.records) {
        if (entry.lod_level != 0u)
            continue;
        RegionRecord record;
        if (!ReadIndexedRecord(index, entry, record, errors)) {
            out_chunks.clear();
            return false;
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
    if (!exists)
        return true; // clean miss

    RegionIndex index;
    if (!ReadRegionIndex(region_file, index, errors)) {
        return false;
    }
    for (const auto& entry : index.records) {
        if (entry.lod_level != 0u || (entry.flags & kRecordFlagEdited) == 0u)
            continue;
        const IVec3 indexed_coords = Chunk::decode_id(entry.id);
        if (indexed_coords.x < min_chunk_x || indexed_coords.x > max_chunk_x ||
            indexed_coords.z < min_chunk_z || indexed_coords.z > max_chunk_z) {
            continue;
        }
        RegionRecord record;
        if (!ReadIndexedRecord(index, entry, record, errors)) {
            out_chunks.clear();
            return false;
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

WorldAuthorityHashes
WorldSaveService::authority_hashes(const std::filesystem::path& save_dir,
                                   const std::vector<std::shared_ptr<Chunk>>& live_chunks) {
    auto cache = load_authority_hash_cache(save_dir);
    for (const auto& chunk : live_chunks)
        if (chunk)
            cache.update(chunk);
    return cache.hashes();
}

AuthorityHashCache
WorldSaveService::load_authority_hash_cache(const std::filesystem::path& save_dir) {
    AuthorityHashCache cache;
    const auto directory = region_directory(save_dir);
    if (save_dir.empty() || !std::filesystem::exists(directory))
        return cache;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        int rx = 0, rz = 0;
        if (!entry.is_regular_file() || !ParseRegionFileName(entry.path(), rx, rz))
            continue;
        AuthorityReader reader;
        if (!open_region_authority(entry.path(), reader))
            throw std::runtime_error("Cannot read durable authority for hashing");
        while (auto chunk = read_next_authority(reader))
            cache.update(chunk);
    }
    return cache;
}

struct WorldSaveService::AuthorityReader::Impl {
    RegionIndex index;
};

WorldSaveService::AuthorityReader::AuthorityReader() = default;
WorldSaveService::AuthorityReader::~AuthorityReader() = default;
WorldSaveService::AuthorityReader::AuthorityReader(AuthorityReader&&) noexcept = default;
WorldSaveService::AuthorityReader&
WorldSaveService::AuthorityReader::operator=(AuthorityReader&&) noexcept = default;

bool WorldSaveService::AuthorityReader::open(const std::filesystem::path& region_file) {
    m_impl = std::make_unique<Impl>();
    if (OpenAuthorityIndex(region_file, m_impl->index))
        return true;
    m_impl.reset();
    return false;
}

std::optional<u64> WorldSaveService::AuthorityReader::next_id() const {
    if (!m_impl || m_impl->index.next >= m_impl->index.records.size())
        return std::nullopt;
    return m_impl->index.records[m_impl->index.next].id;
}

void WorldSaveService::AuthorityReader::skip() {
    if (next_id())
        ++m_impl->index.next;
}

std::shared_ptr<Chunk> WorldSaveService::AuthorityReader::read(u64 id) {
    if (!m_impl)
        return nullptr;
    auto& index = m_impl->index;
    const auto entry = std::lower_bound(
        index.records.begin(), index.records.end(), id, [](const auto& record, u64 value) {
            return record.id < value;
        });
    if (entry == index.records.end() || entry->id != id)
        return nullptr;
    RegionRecord raw;
    if (!ReadIndexedRecord(index, *entry, raw, nullptr))
        throw std::runtime_error("Cannot read durable chunk authority");
    auto chunk = DecodeChunkRecord(raw, index.path, nullptr, true);
    if (!chunk)
        throw std::runtime_error("Cannot decode durable chunk authority");
    // Runtime jobs and collider handles belong to the previous process.
    chunk->reset_pending_jobs();
    chunk->has_collision.store(false, std::memory_order_release);
    chunk->set_state(ChunkState::Idle);
    return chunk;
}

std::shared_ptr<Chunk> WorldSaveService::AuthorityReader::read_next() {
    const auto id = next_id();
    if (!id)
        return nullptr;
    auto chunk = read(*id);
    skip();
    return chunk;
}

bool WorldSaveService::open_region_authority(const std::filesystem::path& region_file,
                                             AuthorityReader& reader) {
    return reader.open(region_file);
}

std::shared_ptr<Chunk> WorldSaveService::read_next_authority(AuthorityReader& reader) {
    return reader.read_next();
}

bool WorldSaveService::persist_chunk_authority(const std::filesystem::path& save_dir,
                                               const std::shared_ptr<Chunk>& chunk) {
    if (save_dir.empty() || !chunk || !chunk->has_simulation_authority())
        return false;
    int rx = 0, rz = 0;
    region_coords_for_chunk(chunk->get_coords(), rx, rz);
    const ContainerRecord record{chunk->get_id(),
                                 0,
                                 ChunkRecordFlags(*chunk, 0),
                                 SerializeChunkAuthoritySnapshotJson(*chunk)};
    if (!upsert_container_records(region_file_path(save_dir, rx, rz), {record}))
        return false;
    chunk->clear_voxel_data_dirty();
    return true;
}

bool WorldSaveService::persist_water_column_migration(
    const std::filesystem::path& save_dir,
    const std::shared_ptr<Chunk>& destination,
    const std::vector<std::shared_ptr<Chunk>>& sources) {
    const SaveGuard guard;
    if (save_dir.empty() || !destination || destination->get_coords().y != 0 ||
        !destination->has_water_authority() || !destination->has_complete_water_state() ||
        sources.empty() || sources.size() >= std::numeric_limits<u16>::max())
        return false;
    // LMR1 bounds this list, including the destination, to 65,535 records.
    std::vector<RegionReplacement> replacements;
    replacements.reserve(sources.size() + 1u);
    replacements.push_back(
        {.id = destination->get_id(), .preserve_authority = true, .chunk = destination});
    std::set<ChunkID> seen;
    for (const auto& source : sources) {
        if (!source || source->get_coords().x != destination->get_coords().x ||
            source->get_coords().z != destination->get_coords().z || source->get_coords().y == 0 ||
            !source->has_water_authority() || !source->has_complete_water_state() ||
            !source->same_water_state(*destination) || !seen.insert(source->get_id()).second)
            return false;
        replacements.push_back({.id = source->get_id(),
                                .preserve_authority = true,
                                .chunk = source,
                                .omit_water = true});
    }
    int rx = 0, rz = 0;
    region_coords_for_chunk(destination->get_coords(), rx, rz);
    return RewriteRegionFile(region_file_path(save_dir, rx, rz), replacements, nullptr);
}

bool WorldSaveService::copy_chunk_authority(const std::filesystem::path& source,
                                            const std::filesystem::path& destination,
                                            std::span<const std::shared_ptr<Chunk>> live_chunks) {
    const SaveGuard guard;
    if (source == destination)
        return true;
    try {
        if (!source.empty() && std::filesystem::exists(source) &&
            std::filesystem::exists(destination) &&
            std::filesystem::equivalent(source, destination))
            return true;
        // Preflight every destination region before writing anything. Region
        // replacement preserves siblings, so adopting destination-only edited
        // records would import another snapshot's authority into the live world.
        // Both manifests are bounded by LMR1's 65,535-record limit per region.
        const auto destination_regions = region_directory(destination);
        if (std::filesystem::exists(destination_regions)) {
            // Runtime callers bound live owners by the shared resident budget.
            std::set<ChunkID> live_ids;
            for (const auto& chunk : live_chunks) {
                if (chunk && chunk->has_simulation_authority())
                    live_ids.insert(chunk->get_id());
            }
            for (const auto& entry : std::filesystem::directory_iterator(destination_regions)) {
                int rx = 0, rz = 0;
                if (!entry.is_regular_file() || !ParseRegionFileName(entry.path(), rx, rz))
                    continue;
                RegionIndex previous, incoming;
                if (!OpenAuthorityIndex(entry.path(), previous) ||
                    (!source.empty() &&
                     !OpenAuthorityIndex(region_file_path(source, rx, rz), incoming)))
                    return false;
                for (const auto& record : previous.records) {
                    const auto source_record = std::lower_bound(
                        incoming.records.begin(),
                        incoming.records.end(),
                        record.id,
                        [](const auto& candidate, ChunkID id) { return candidate.id < id; });
                    if ((source_record == incoming.records.end() ||
                         source_record->id != record.id) &&
                        !live_ids.contains(record.id))
                        return false;
                }
            }
        }
        if (source.empty())
            return true;
        const auto directory = region_directory(source);
        if (!std::filesystem::exists(directory))
            return true;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            int rx = 0, rz = 0;
            if (!entry.is_regular_file() || !ParseRegionFileName(entry.path(), rx, rz))
                continue;
            RegionIndex index;
            if (!OpenAuthorityIndex(entry.path(), index))
                return false;
            std::vector<RegionReplacement> replacements;
            replacements.reserve(index.records.size());
            for (std::size_t i = 0; i < index.records.size(); ++i)
                replacements.push_back({.id = index.records[i].id,
                                        .lod_level = 0,
                                        .chunk = {},
                                        .source = &index,
                                        .source_record = i});
            if (!replacements.empty() &&
                !RewriteRegionFile(region_file_path(destination, rx, rz), replacements, nullptr))
                return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool WorldSaveService::upsert_container_records(const std::filesystem::path& region_file,
                                                const std::vector<ContainerRecord>& records,
                                                std::vector<std::string>* errors) {
    const SaveGuard guard;
    try {
        if (records.size() > std::numeric_limits<u16>::max()) {
            AddError(errors, "region file exceeds the 65535-record limit");
            return false;
        }
        std::vector<RegionReplacement> replacements;
        replacements.reserve(records.size());
        for (const auto& record : records) {
            if (record.lod_level > 2 || (record.flags & ~3u) != 0) {
                AddError(errors, "corrupt or unknown region record header");
                return false;
            }
            replacements.push_back({.id = record.id,
                                    .lod_level = record.lod_level,
                                    .chunk = {},
                                    .container = &record});
        }
        return RewriteRegionFile(region_file, replacements, errors);
    } catch (const std::exception& e) {
        AddError(errors, std::string("failed to upsert region records: ") + e.what());
        return false;
    }
}

void WorldSaveService::set_interrupt_before_region_replace_for_testing(bool enabled) {
    InterruptBeforeRegionReplaceForTesting().store(enabled, std::memory_order_release);
}

namespace {
bool WriteFileDurably(const std::filesystem::path& destination,
                      const std::string& bytes,
                      const std::filesystem::path& staging,
                      std::vector<std::string>* errors,
                      FileIdentity* published_identity) {
    // Callers hold SaveGuard. Stage in an existing ancestor before creating
    // payload directories, so a refused payload leaves the hierarchy unchanged.
    std::filesystem::path temp_path;
    if (!WriteDurableRegionTemp(staging, bytes, temp_path, errors))
        return false;
    // Capture the exclusive staging file's identity before publication. Alias
    // rollback later checks this identity at replacement under SaveGuard.
    if (published_identity && !ReadFileIdentity(temp_path, *published_identity)) {
        RemoveTemporaryFile(temp_path);
        AddError(errors, "cannot retain replacement identity");
        return false;
    }
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel)) {
        RemoveTemporaryFile(temp_path);
        AddError(errors,
                 "file replacement interrupted by test seam before replacing: " +
                     Filesystem::GenericUtf8PathString(destination));
        return false;
    }
    if (!CreatePayloadDirectory(destination, errors)) {
        RemoveTemporaryFile(temp_path);
        return false;
    }
    if (!ReplaceRegionFileAtomically(temp_path, destination, errors)) {
        RemoveTemporaryFile(temp_path);
        return false;
    }
    return true;
}
} // namespace

bool WorldSaveService::write_file_durably(const std::filesystem::path& destination,
                                          const std::string& bytes,
                                          std::vector<std::string>* errors) {
    const SaveGuard guard;
    ReclaimAbandonedStaging(destination);
    return WriteFileDurably(destination, bytes, destination, errors);
}

bool WorldSaveService::create_file_durably(const std::filesystem::path& destination,
                                           const std::string& bytes,
                                           std::vector<std::string>* errors) {
    const SaveGuard guard;
    ReclaimAbandonedStaging(destination);
    std::error_code error;
    if (destination.has_parent_path())
        std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        AddError(errors, "cannot create publication directory: " + error.message());
        return false;
    }
    std::filesystem::path temp;
    if (!WriteDurableRegionTemp(destination, bytes, temp, errors))
        return false;
    NewFilePublication publication(temp, destination);
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel))
        return false;
    if (!publication.Publish(errors))
        return false;
    publication.Commit();
    return true;
}

bool WorldSaveService::rename_file_durably(const std::filesystem::path& source,
                                           const std::filesystem::path& destination,
                                           const std::string& expected_bytes,
                                           const std::string& bytes,
                                           std::vector<std::string>* errors) {
    const SaveGuard guard;
    // Revalidate the parsed source under ownership so another persistence writer
    // cannot change the source between the caller's parse and this transaction.
    {
        const std::ifstream input(source, std::ios::binary);
        std::ostringstream contents;
        contents << input.rdbuf();
        if (!input || input.bad() || contents.bad() || contents.str() != expected_bytes) {
            AddError(errors, "rename source changed or cannot be read");
            return false;
        }
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(destination, error);
    const bool missing = status.type() == std::filesystem::file_type::not_found &&
                         (!error || error == std::errc::no_such_file_or_directory);
    bool alias = false;
    if (!missing) {
        // Follow targets only to admit an existing alias. Retirement below
        // compares leaf entries so a separate symlink is still removed.
        if (!error)
            alias = std::filesystem::equivalent(source, destination, error);
        if (error || !alias) {
            AddError(errors, "rename destination is occupied or cannot be inspected");
            return false;
        }
    }
    error.clear();
    ReclaimAbandonedStaging(destination);
    if (alias) {
        std::filesystem::path backup;
        class BackupCleanup {
        public:
            explicit BackupCleanup(const std::filesystem::path& path)
                : m_path(path) {}
            ~BackupCleanup() {
                if (!m_path.empty())
                    RemoveTemporaryFile(m_path);
            }

        private:
            const std::filesystem::path& m_path;
        };
        const BackupCleanup cleanup{backup};
        const auto links = std::filesystem::hard_link_count(destination, error);
        if (error) {
            AddError(errors, "cannot inspect rename alias: " + error.message());
            return false;
        }
        if (links > 1 || status.type() == std::filesystem::file_type::symlink) {
            // A separate hard-link destination must be restorable if source
            // retirement fails. Keep the original entry (including a symlink's
            // own spelling) beside the destination until the transaction ends.
            // Like payload staging, exclusive allocation is bounded to 128 tries.
            for (int attempt = 0; attempt < 128; ++attempt) {
                const auto candidate = NextRegionTempPath(destination);
                std::filesystem::create_hard_link(destination, candidate, error);
                if (!error) {
                    backup = candidate;
                    break;
                }
                if (error != std::errc::file_exists) {
                    AddError(errors, "cannot retain rename alias: " + error.message());
                    return false;
                }
            }
            if (backup.empty()) {
                AddError(errors, "cannot allocate rename alias staging");
                return false;
            }
        }
        FileIdentity published_identity;
        if (!WriteFileDurably(destination, bytes, destination, errors, &published_identity)) {
            // A final directory-flush failure may follow replacement. Restore a
            // separate hard-link entry so retry can still recognize the alias.
            if (!backup.empty() && MatchesFileIdentity(destination, published_identity))
                ReplaceRegionFileAtomically(backup, destination, errors);
            return false;
        }
        // Case and directory aliases still name the replacement entry. Separate
        // hard links and leaf symlinks must be retired, even when following a
        // symlink would reach the replacement at destination.
        FileIdentity remaining_source;
        if (!ReadFileIdentity(source, remaining_source)) {
            AddError(errors, "cannot inspect rename source after publication");
            return false;
        }
        if (SameFileIdentity(remaining_source, published_identity))
            return true;
        if (!std::filesystem::remove(source, error) || error) {
            if (!backup.empty() && MatchesFileIdentity(destination, published_identity))
                ReplaceRegionFileAtomically(backup, destination, errors);
            AddError(errors, "cannot remove rename source: " + error.message());
            return false;
        }
        return FlushRemovalDirectory(source, errors);
    }
    std::filesystem::path temp;
    // Create the destination hierarchy using the same lexical spelling as the
    // existing writer; parent aliases and UNC roots must retain their meaning.
    if (destination.has_parent_path())
        std::filesystem::create_directories(destination.parent_path(), error);
    if (error || !WriteDurableRegionTemp(destination, bytes, temp, errors))
        return false;
    NewFilePublication publication(temp, destination);
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel))
        return false;
    if (!publication.Publish(errors))
        return false;
    if (!std::filesystem::remove(source, error) || error) {
        AddError(errors, "cannot remove rename source: " + error.message());
        return false;
    }
    // The destination now owns the content; a post-unlink flush failure must
    // retain it. Like replacement-directory failures this reports incomplete
    // durability even though the complete new file is visible.
    publication.Commit();
    return FlushRemovalDirectory(source, errors);
}

bool WorldSaveService::remove_file_and_staging(const std::filesystem::path& destination,
                                               std::vector<std::string>* errors) {
    const SaveGuard guard;
    ReclaimAbandonedStaging(destination);
    std::error_code error;
    std::filesystem::remove(destination, error);
    if (error) {
        AddError(errors,
                 "cannot remove file: " + Filesystem::GenericUtf8PathString(destination) + ": " +
                     error.message());
        return false;
    }
    return FlushRemovalDirectory(destination, errors);
}

std::size_t WorldSaveService::waiting_save_writers() {
    return WaitingSaveWriters().load(std::memory_order_acquire);
}

void WorldSaveService::set_directory_flush_failure_for_testing(int flushes_to_skip) {
    DirectoryFlushFailureCountdownForTesting().store(flushes_to_skip, std::memory_order_release);
}

void WorldSaveService::set_before_active_regions_replace_for_testing(bool (*hook)()) {
    BeforeActiveRegionsReplaceForTesting() = hook;
}

bool WorldSaveService::save_world(const WorldStreamingState& state,
                                  const std::filesystem::path& save_dir,
                                  std::vector<std::string>* errors,
                                  bool preserve_authority) const {
    const SaveGuard guard;
    return validate_save(save_dir, errors) &&
           write_snapshot(state.snapshot_chunks(), save_dir, errors, nullptr, preserve_authority);
}

bool WorldSaveService::load_world(WorldStreamingState& state,
                                  const std::filesystem::path& save_dir,
                                  std::vector<std::string>& errors,
                                  const std::stop_token& stop,
                                  std::size_t max_resident_chunks,
                                  bool require_full_sdf_lattice,
                                  bool migrate_legacy_water) const {
    LUMIN_PROFILE_ZONE_N("File world load");
    state.clear();
    const auto reject = [&state, &errors](const std::string& message) {
        state.clear();
        if (!message.empty())
            errors.push_back(message);
        return false;
    };
    try {
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
        const auto metadata_path = save_dir / "world_info.json";
        if (std::filesystem::exists(metadata_path)) {
            std::ifstream input(metadata_path, std::ios::binary);
            const auto metadata = nlohmann::json::parse(input);
            if (!metadata.is_object())
                return reject("corrupt world metadata: expected an object");
            // Old worlds can contain only metadata: no chunk container exists yet
            // to reveal their version. Refuse them before the clean-miss path.
            if (!metadata.contains("container_version"))
                return reject(kObsoleteWorldMessage);
            const auto& version = metadata.at("container_version");
            if (!version.is_number_integer())
                return reject("corrupt world metadata container version");
            if (version != kContainerVersion) {
                if (version >= 0 && version < kContainerVersion)
                    return reject(kObsoleteWorldMessage);
                if (version > kContainerVersion)
                    return reject("unsupported future world metadata container version " +
                                  version.dump());
                return reject("corrupt world metadata container version");
            }
            if (metadata.contains("simulationTick") || metadata.contains("calendar")) {
                world::WorldClock clock;
                std::string error;
                std::optional<Vec3> anchor;
                if (!world::WorldClock::from_metadata(metadata, clock, error) ||
                    !ReadAmbientAnchor(metadata, anchor, error))
                    return reject(error);
            }
            std::string cursor_error;
            if (!ReadWaterCursor(metadata, nullptr, cursor_error))
                return reject(cursor_error);
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
                              Filesystem::GenericUtf8PathString(entry.path()));
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
                return reject("corrupt world persistence artifact: " +
                              Filesystem::GenericUtf8PathString(entry.path()));
            if (entry.path() == world_manifest_path(save_dir) ||
                entry.path() == plant_entities_path(save_dir) ||
                entry.path() == active_regions_path(save_dir)) {
                present = true;
            } else if (ParseRegionFileName(entry.path(), rx, rz)) {
                region_files.push_back(entry.path());
                present = true;
            } else {
                return reject("corrupt or unknown region file: " +
                              Filesystem::GenericUtf8PathString(entry.path()));
            }
        }
        if (!present)
            return false;
        if (std::filesystem::exists(world_manifest_path(save_dir))) {
            std::ifstream input(world_manifest_path(save_dir), std::ios::binary);
            const auto manifest = nlohmann::json::parse(input);
            if (manifest.is_object() && manifest.contains("schema") &&
                manifest.at("schema").is_string()) {
                const auto schema = manifest.at("schema").get<std::string>();
                const std::string prefix = "luminumbra.persistence.world_manifest.v";
                if (schema.rfind(prefix, 0) == 0 && schema != kWorldManifestSchema) {
                    unsigned version = 0;
                    const auto parsed = std::from_chars(
                        schema.data() + prefix.size(), schema.data() + schema.size(), version);
                    if (parsed.ec == std::errc{} && parsed.ptr == schema.data() + schema.size()) {
                        if (version > 1)
                            return reject("unsupported future world manifest schema: " + schema);
                        if (version == 0)
                            return reject(kObsoleteWorldMessage);
                    }
                }
            }
            if (!manifest.is_object() || !manifest.contains("schema") ||
                manifest.at("schema") != kWorldManifestSchema || !manifest.contains("container") ||
                manifest.at("container") != "LMR1" || !manifest.contains("container_version") ||
                !manifest.at("container_version").is_number_integer())
                return reject("corrupt or unknown world manifest identity/version");
            const auto& version = manifest.at("container_version");
            if (version != kRegionVersion) {
                if (version >= 0 && version < kRegionVersion)
                    return reject(kObsoleteWorldMessage);
                if (version > kRegionVersion)
                    return reject("unsupported future world manifest container version " +
                                  version.dump());
                return reject("corrupt world manifest container version");
            }
        }
        std::sort(region_files.begin(), region_files.end());
        std::set<std::pair<int, int>> seen_regions;
        for (const auto& path : region_files) {
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            int rx = 0, rz = 0;
            ParseRegionFileName(path, rx, rz);
            if (!seen_regions.emplace(rx, rz).second)
                return reject("corrupt duplicate region identity: " +
                              Filesystem::GenericUtf8PathString(path));
            RegionIndex index;
            if (!ReadRegionIndex(path, index, &errors, stop))
                return reject("");
            for (const auto& entry : index.records) {
                RegionRecord record;
                if (!ReadIndexedRecord(index, entry, record, &errors))
                    return reject("");
                if (stop.stop_requested())
                    return reject("Saved-world validation cancelled.");
                if (record.lod_level == 0) {
                    const auto chunk =
                        DecodeChunkRecord(record, path, &errors, migrate_legacy_water);
                    if (!chunk)
                        return reject("");
                    int chunk_rx = 0, chunk_rz = 0;
                    region_coords_for_chunk(chunk->get_coords(), chunk_rx, chunk_rz);
                    if (chunk_rx != rx || chunk_rz != rz)
                        return reject("corrupt chunk region identity or duplicate id: " +
                                      Filesystem::GenericUtf8PathString(path));
                    // Preserve the runtime loader's full-lattice validation,
                    // including records beyond its bounded resident prefix.
                    constexpr std::size_t lattice = static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                                    (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
                    if (require_full_sdf_lattice && !chunk->sdf_data.empty() &&
                        chunk->sdf_data.size() != lattice)
                        return reject("corrupt world chunk SDF lattice");
                    // Manifest keys are unique within a region; region identity
                    // validation above also excludes duplicates across files.
                    if (state.size() < max_resident_chunks && !state.insert_chunk(chunk))
                        return reject("corrupt duplicate chunk id: " +
                                      Filesystem::GenericUtf8PathString(path));
                } else {
                    const auto tier = static_cast<World::FarLodTier>(record.lod_level);
                    if (record.id != World::FarLodStore::tile_record_id(tier, rx, rz))
                        return reject("corrupt far-LOD region identity: " +
                                      Filesystem::GenericUtf8PathString(path));
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
                                                         std::vector<std::string>* errors,
                                                         bool acknowledge_dirty,
                                                         bool preserve_authority) const {
    const SaveGuard guard;
    if (state.dirty_chunk_ids().empty() || !validate_save(save_dir, errors))
        return {.chunks_total = state.size(), .chunks_dirty = state.dirty_chunk_ids().size()};
    return save_dirty_validated(state, save_dir, errors, preserve_authority, acknowledge_dirty);
}

WorldSaveDirtyReport WorldSaveService::save_dirty_validated(WorldStreamingState& state,
                                                            const std::filesystem::path& save_dir,
                                                            std::vector<std::string>* errors,
                                                            bool preserve_authority,
                                                            bool acknowledge_dirty) const {
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

    if (!write_snapshot(
            chunks_to_write, save_dir, errors, &report.regions_written, preserve_authority)) {
        return report;
    }

    if (acknowledge_dirty) {
        for (const ChunkID id : dirty_ids) {
            if (const auto chunk = state.find_chunk(id)) {
                chunk->clear_voxel_data_dirty();
            }
        }
    }
    report.saved = true;
    return report;
}

bool WorldSaveService::write_snapshot(const std::vector<std::shared_ptr<Chunk>>& chunks,
                                      const std::filesystem::path& save_dir,
                                      std::vector<std::string>* errors,
                                      std::size_t* regions_written,
                                      bool preserve_authority) const {
    const SaveGuard guard;
    LUMIN_PROFILE_ZONE_N("File world save");
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

        std::size_t written = 0;
        for (auto& [region_coords, region_chunks] : regions) {
            const std::filesystem::path path =
                region_file_path(save_dir, region_coords.first, region_coords.second);

            if (region_chunks.size() > std::numeric_limits<u16>::max()) {
                AddError(errors, "region file exceeds the 65535-record limit");
                return false;
            }
            std::vector<RegionReplacement> replacements;
            replacements.reserve(region_chunks.size());
            for (const auto& chunk : region_chunks)
                replacements.push_back({.id = chunk->get_id(),
                                        .lod_level = 0,
                                        .preserve_authority = preserve_authority,
                                        .chunk = chunk});
            if (!RewriteRegionFile(path, replacements, errors))
                return false;
            ++written;
        }

        if (!WriteWorldManifest(world_manifest_path(save_dir), errors)) {
            return false;
        }

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
