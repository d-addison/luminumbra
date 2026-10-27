#include "WorldSaveService.h"
#include "core/Profiler.h"

#include "../ecs/EntitySnapshot.h" //  plant entity snapshot persistence
#include "PlantPersistence.h"
#include "SavedInputFile.h"
#include "SavedInputLimits.h"
#include "SavedJsonAdmission.h"
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

bool WriteDurableRegionTemp(const std::filesystem::path& destination,
                            const std::string& bytes,
                            std::filesystem::path& out_temp,
                            std::vector<std::string>* errors) {
    LUMIN_PROFILE_ZONE_N("File save write");
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
                     "failed to write temporary region file: " +
                         Filesystem::GenericUtf8PathString(out_temp) + ": " +
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

bool WriteDurableRegionTemp(const std::filesystem::path& destination,
                            const std::string& bytes,
                            std::filesystem::path& out_temp,
                            std::vector<std::string>* errors) {
    LUMIN_PROFILE_ZONE_N("File save write");
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
                     "failed to write temporary region file: " +
                         Filesystem::GenericUtf8PathString(out_temp) + ": " +
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
    if (payload.empty() || payload.size() > SavedInputLimits::kRegionRecordBytes) {
        AddError(errors, "region record payload exceeds the 64 MiB compatibility limit");
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
    if (record.uncompressed_size == 0 ||
        record.uncompressed_size > SavedInputLimits::kRegionRecordBytes ||
        record.compressed_payload.empty() ||
        record.compressed_payload.size() > SavedInputLimits::kRegionCompressedBytes) {
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
                    const std::stop_token& stop = {},
                    bool missing_allowed = false) {
    LUMIN_PROFILE_ZONE_N("File save read");
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    const SavedInputFile input(path);
    if (missing_allowed && input.missing()) {
        out_records.clear();
        return true;
    }
    const auto refuse = [errors, &path](const std::string& reason) {
        AddError(errors, reason + ": " + Filesystem::GenericUtf8PathString(path));
        return false;
    };
    std::uint64_t file_size = 0;
    std::array<char, kRegionFileHeaderSize> header{};
    if (!input.length(file_size))
        return refuse("Saved-input admission refused: unreadable or non-regular region file");
    if (!SavedInputLimits::RegionFileLengthAllowed(file_size))
        return refuse(
            "Saved-input admission refused: region file exceeds byte compatibility limit");
    if (!input.read_exact(header.data(), header.size()))
        return refuse("truncated region file header");
    if (std::memcmp(header.data(), kRegionMagic, sizeof(kRegionMagic)) != 0)
        return refuse("region file is missing the LMR1 magic");
    const auto* control = reinterpret_cast<const unsigned char*>(header.data());
    const auto version = ReadU16(control + 4);
    if (version < kRegionVersion) {
        AddError(errors, WorldSaveService::kObsoleteWorldMessage);
        return false;
    }
    if (version > kRegionVersion)
        return refuse("unsupported future LMR1 container version " + std::to_string(version));
    const auto count = ReadU16(control + 6);
    const std::size_t manifest_bytes = static_cast<std::size_t>(count) * kRecordHeaderSize;
    if (file_size < kRegionFileHeaderSize + manifest_bytes)
        return refuse("region file record manifest is truncated");
    // The 8-byte control is validated before buffering anything proportional.
    // u16 count bounds this manifest to 65535 * 18 bytes.
    std::string manifest(manifest_bytes, '\0');
    if (!input.read_exact(manifest.data(), manifest.size()))
        return refuse("region file record manifest is truncated");
    std::vector<RegionRecord> candidate(count);
    std::set<std::pair<u8, u64>> keys;
    std::uint64_t payload_bytes = 0;
    std::uint64_t work_bytes = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (stop.stop_requested())
            return refuse("Saved-world validation cancelled.");
        const auto* entry =
            reinterpret_cast<const unsigned char*>(manifest.data()) + index * kRecordHeaderSize;
        auto& record = candidate[index];
        record.id = ReadU64(entry);
        record.lod_level = entry[8];
        record.flags = entry[9];
        record.uncompressed_size = ReadU32(entry + 10);
        const auto compressed_size = ReadU32(entry + 14);
        if (record.lod_level > 2 || (record.flags & ~3u) != 0u ||
            !keys.emplace(record.lod_level, record.id).second)
            return refuse("corrupt or unknown region record header");
        if (record.uncompressed_size == 0 ||
            record.uncompressed_size > SavedInputLimits::kRegionRecordBytes ||
            compressed_size == 0 || compressed_size > SavedInputLimits::kRegionCompressedBytes ||
            record.uncompressed_size > SavedInputLimits::kRegionReadWorkBytes - work_bytes)
            return refuse("Saved-input admission refused: region record or aggregate exceeds "
                          "compatibility limit");
        work_bytes += record.uncompressed_size;
        if (compressed_size > SavedInputLimits::kRegionCompressedAggregateBytes - payload_bytes)
            return refuse("Saved-input admission refused: region compressed aggregate exceeds "
                          "compatibility limit");
        payload_bytes += compressed_size; // u16 * bounded u32 fits u64.
    }
    const auto available_payload_bytes = file_size - kRegionFileHeaderSize - manifest_bytes;
    if (payload_bytes > available_payload_bytes)
        return refuse("region record payload is truncated: payload lengths do not match file size");
    if (payload_bytes != available_payload_bytes)
        return refuse("region file payload lengths do not match file size");
    // All declared sizes and aggregate decompression work pass before any payload allocation.
    for (std::size_t index = 0; index < count; ++index) {
        if (stop.stop_requested())
            return refuse("Saved-world validation cancelled.");
        const auto* entry =
            reinterpret_cast<const unsigned char*>(manifest.data()) + index * kRecordHeaderSize;
        auto& payload = candidate[index].compressed_payload;
        payload.resize(ReadU32(entry + 14));
        if (!input.read_exact(payload.data(), payload.size()))
            return refuse("truncated region record payload");
    }
    if (!input.at_end())
        return refuse("region file has trailing bytes after the last record");
    out_records = std::move(candidate);
    return true;
}

bool AdmitRegionWrite(const std::vector<RegionRecord>& records, std::vector<std::string>* errors) {
    if (records.size() > SavedInputLimits::kRegionRecords) {
        AddError(errors, "region file exceeds the 65535-record compatibility limit");
        return false;
    }
    std::uint64_t work_bytes = 0;
    std::set<std::pair<u8, u64>> keys;
    for (const auto& record : records) {
        if (record.lod_level > 2 || (record.flags & ~3u) != 0u ||
            !keys.emplace(record.lod_level, record.id).second || record.uncompressed_size == 0 ||
            record.uncompressed_size > SavedInputLimits::kRegionRecordBytes ||
            record.compressed_payload.empty() ||
            record.compressed_payload.size() > SavedInputLimits::kRegionCompressedBytes ||
            // Preserved records must obey the same LZ4 writer bound as newly
            // compressed records, keeping aggregate output within its derivation.
            record.compressed_payload.size() >
                record.uncompressed_size + record.uncompressed_size / 255u + 16u ||
            record.uncompressed_size > SavedInputLimits::kRegionWriteWorkBytes - work_bytes) {
            AddError(errors, "region record or aggregate exceeds writer compatibility limit");
            return false;
        }
        work_bytes += record.uncompressed_size;
    }
    return true;
}

bool WriteRegionFile(const std::filesystem::path& path,
                     std::vector<RegionRecord>& records,
                     std::vector<std::string>* errors) {
    if (!AdmitRegionWrite(records, errors))
        return false;

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
    const auto staging = WorldSaveService::region_staging_path(path);
    ReclaimAbandonedStaging(staging);
    if (!WriteDurableRegionTemp(staging, bytes, temp_path, errors)) {
        return false;
    }

    // Test-only crash seam: at this point the replacement bytes are complete
    // and durable, while the prior live LMR1 file has not been touched.
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel)) {
        RemoveTemporaryFile(temp_path);
        AddError(errors,
                 "region replacement interrupted by test seam before replacing: " +
                     Filesystem::GenericUtf8PathString(path));
        return false;
    }

    if (!ReplaceRegionFileAtomically(temp_path, path, errors)) {
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

    std::string bytes;
    if (!BoundedJsonDump(manifest, SavedInputLimits::kManifestBytes, bytes, 4)) {
        AddError(errors, "world manifest exceeds 2 MiB compatibility limit");
        return false;
    }
    std::filesystem::path temp;
    const auto staging = WorldSaveService::region_staging_path(manifest_path);
    ReclaimAbandonedStaging(staging);
    if (!WriteDurableRegionTemp(staging, bytes, temp, errors))
        return false;
    if (!ReplaceRegionFileAtomically(temp, manifest_path, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
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

bool WorldSaveService::admit_metadata_bytes(const std::string& bytes,
                                            std::vector<std::string>* errors) {
    if (bytes.size() > SavedInputLimits::kMetadataBytes) {
        AddError(errors,
                 "Saved-input admission refused: metadata exceeds 2 MiB compatibility limit.");
        return false;
    }
    if (!SavedInputLimits::JsonDepthAllowed(bytes)) {
        AddError(errors,
                 "Saved-input admission refused: JSON nesting depth exceeds compatibility limit.");
        return false;
    }
    if (!AdmitMetadataText(bytes)) {
        AddError(
            errors,
            "Saved-input admission refused: world name or seed exceeds 4 KiB compatibility limit.");
        return false;
    }
    auto metadata = nlohmann::json::parse(bytes, nullptr, false);
    if (!AdmitMetadata(metadata)) {
        AddError(
            errors,
            "Saved-input admission refused: world name or seed exceeds 4 KiB compatibility limit.");
        return false;
    }
    const bool has_clock = metadata.contains("simulationTick") || metadata.contains("calendar");
    const bool has_render_calendar = metadata.contains("renderCalendar");
    if (has_clock || has_render_calendar) {
        // Match save_metadata's normalization before a paired writer can publish
        // its ledger. Even adding an absent simulationTick can cross the byte cap.
        world::WorldClock clock;
        std::string error;
        std::optional<Vec3> anchor;
        std::optional<world::WorldCalendar> calendar;
        if (!world::WorldClock::from_metadata(metadata, clock, error) ||
            !ReadAmbientAnchor(metadata, anchor, error) ||
            !world::WorldClock::read_render_calendar(metadata, calendar, error)) {
            AddError(errors, error);
            return false;
        }
        if (has_clock)
            clock.write_metadata(metadata);
        if (calendar)
            metadata["renderCalendar"] = world::WorldClock(0, *calendar).calendar_metadata();
        std::string normalized;
        if (!BoundedJsonDump(metadata, SavedInputLimits::kMetadataBytes, normalized, 4)) {
            AddError(errors,
                     "Saved-input admission refused: normalized metadata exceeds 2 MiB "
                     "compatibility limit.");
            return false;
        }
    }
    return true;
}

bool WorldSaveService::save_metadata(const std::string& bytes,
                                     const std::filesystem::path& save_dir,
                                     std::vector<std::string>* errors) {
    const SaveGuard guard;
    if (!admit_metadata_bytes(bytes, errors) || !validate_save(save_dir, errors))
        return false;
    world::WorldClock clock;
    std::string clock_error;
    auto metadata = nlohmann::json::parse(bytes, nullptr, false);
    // Legacy metadata is opaque input to this helper. Clock or marker input
    // opts into calendar validation; the existing destination is still validated.
    const bool has_clock = metadata.contains("simulationTick") || metadata.contains("calendar");
    std::optional<Vec3> ambient_anchor;
    const bool has_render_calendar = metadata.contains("renderCalendar");
    if ((has_clock || has_render_calendar) &&
        (!world::WorldClock::from_metadata(metadata, clock, clock_error) ||
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
    // Explicit writes normalize supported calendar encodings; reads never rewrite.
    if (has_clock)
        clock.write_metadata(metadata);
    if (has_render_calendar) {
        std::optional<world::WorldCalendar> calendar;
        if (!world::WorldClock::read_render_calendar(metadata, calendar, clock_error)) {
            AddError(errors, clock_error);
            return false;
        }
        if (!calendar.has_value()) {
            AddError(errors, "Corrupt world metadata: missing renderCalendar.");
            return false;
        }
        metadata["renderCalendar"] = world::WorldClock(0, *calendar).calendar_metadata();
    }
    const std::string output_bytes =
        (has_clock || has_render_calendar) ? metadata.dump(4) + "\n" : bytes;
    if (!admit_metadata_bytes(output_bytes, errors))
        return false;
    std::error_code directory_error;
    std::filesystem::create_directories(save_dir, directory_error);
    if (directory_error) {
        AddError(errors, "failed to create metadata directory: " + directory_error.message());
        return false;
    }
    const auto destination = save_dir / "world_info.json";
    ReclaimAbandonedStaging(destination);
    std::filesystem::path temp;
    if (!WriteDurableRegionTemp(destination, output_bytes, temp, errors))
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
    if (!admit_metadata_bytes(bytes, errors))
        return false;
    const auto metadata = nlohmann::json::parse(bytes, nullptr, false);
    world::WorldClock clock;
    std::optional<Vec3> ambient_anchor;
    std::string error;
    if (!world::WorldClock::from_metadata(metadata, clock, error) ||
        !ReadAmbientAnchor(metadata, ambient_anchor, error)) {
        AddError(errors, error);
        return false;
    }
    if (!metadata.contains("simulationTick") && !metadata.contains("calendar")) {
        AddError(errors, "Corrupt world metadata: active regions require clock metadata.");
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
    world::WorldClock previous_clock;
    bool requires_active_regions = false;
    if (!read_clock_metadata(save_dir, previous_clock, requires_active_regions, errors))
        return false;
    // Each writer validates the installed pair. On rewind, replace the ledger
    // first so it cannot be newer than the clock, even if metadata writing fails.
    if (clock.tick() < previous_clock.tick())
        return save_active_regions(ledger, clock, save_dir, errors) &&
               save_metadata(bytes, save_dir, errors);
    return save_metadata(bytes, save_dir, errors) &&
           save_active_regions(ledger, clock, save_dir, errors);
}

bool WorldSaveService::read_clock_metadata(const std::filesystem::path& save_dir,
                                           world::WorldClock& clock,
                                           bool& requires_active_regions,
                                           std::vector<std::string>* errors,
                                           std::optional<Vec3>* ambient_anchor,
                                           std::optional<world::WorldCalendar>* render_calendar,
                                           std::size_t* water_sim_cursor) {
    LUMIN_PROFILE_ZONE_N("File clock metadata load");
    clock = world::WorldClock{0, world::WorldCalendar::Legacy()};
    if (water_sim_cursor)
        *water_sim_cursor = 0;
    if (render_calendar)
        render_calendar->reset();
    requires_active_regions = false;
    if (ambient_anchor)
        ambient_anchor->reset();
    try {
        // The installed ledger is simulation authority even without clock
        // metadata, including empty and anchor-only ledgers of either version.
        // Root-level .tmp.* siblings are uncommitted staging, never authority.
        requires_active_regions = std::filesystem::exists(active_regions_path(save_dir));
        const auto path = save_dir / "world_info.json";
        nlohmann::json metadata;
        bool missing = false;
        SavedJsonFailure json_failure = SavedJsonFailure::None;
        if (!ReadSavedJson(
                path, SavedInputLimits::kMetadataBytes, metadata, missing, true, &json_failure) ||
            (!missing && !AdmitMetadata(metadata))) {
            AddError(
                errors,
                json_failure == SavedJsonFailure::Malformed ||
                        json_failure == SavedJsonFailure::Unreadable
                    ? (json_failure == SavedJsonFailure::Malformed
                           ? "Corrupt world metadata: invalid JSON."
                           : "Corrupt world metadata: unreadable or non-regular input.")
                    : "Saved-input admission refused: unreadable or oversized world metadata.");
            return false;
        }
        if (missing)
            return true;
        std::string error;
        std::optional<Vec3> anchor;
        if (!world::WorldClock::from_metadata(metadata, clock, error) ||
            !ReadAmbientAnchor(metadata, anchor, error)) {
            AddError(errors, error);
            return false;
        }
        std::optional<world::WorldCalendar> rendering;
        if (!world::WorldClock::read_render_calendar(metadata, rendering, error)) {
            AddError(errors, error);
            return false;
        }
        if (rendering && requires_active_regions) {
            AddError(errors,
                     "Corrupt world metadata: renderCalendar conflicts with active regions.");
            return false;
        }
        std::size_t cursor = 0;
        if (metadata.contains("waterSimCursor")) {
            const auto& value = metadata.at("waterSimCursor");
            if (!value.is_number_unsigned() ||
                value.get<std::uint64_t>() > std::numeric_limits<std::size_t>::max()) {
                AddError(errors, "Corrupt world metadata: invalid water simulation cursor.");
                return false;
            }
            cursor = value.get<std::size_t>();
        }
        if (water_sim_cursor)
            *water_sim_cursor = cursor;
        if (render_calendar)
            *render_calendar = rendering;
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

bool WorldSaveService::prepare_plant_entities(const Ecs::EntityRegistrySnapshot& snapshot,
                                              std::string& bytes,
                                              std::vector<std::string>* errors) {
    if (!luminumbra::foliage::ValidatePlantEntitySnapshot(snapshot)) {
        AddError(errors, "Saved-input admission refused: invalid plant component value.");
        return false;
    }
    std::size_t aggregate = 0;
    for (const auto& entity : snapshot.entities) {
        for (const auto& component : entity.components) {
            std::string encoded;
            // The envelope adds five object/array levels before component data.
            if (!AdmitJsonValueDepth(component.data,
                                     SavedInputLimits::kJsonNestingDepth - 5,
                                     SavedInputLimits::kPlantComponentBytes) ||
                !BoundedJsonDump(component.data, SavedInputLimits::kPlantComponentBytes, encoded) ||
                encoded.size() > SavedInputLimits::kPlantComponentAggregateBytes - aggregate) {
                AddError(errors,
                         "Saved-input admission refused: plant component JSON compatibility limit "
                         "(1 MiB each, 128 MiB aggregate).");
                return false;
            }
            aggregate += encoded.size();
        }
    }
    // Preserve the existing serializer's ordering and exact pretty-file bytes,
    // with bounded output rather than an unbounded dump followed by a size check.
    nlohmann::json entities = nlohmann::json::array();
    std::size_t components = 0;
    for (const auto& entity : snapshot.entities) {
        nlohmann::json values = nlohmann::json::array();
        for (const auto& component : entity.components) {
            values.push_back({{"type", component.type}, {"data", component.data}});
            ++components;
        }
        std::sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.at("type") < rhs.at("type");
        });
        entities.push_back({{"entity_id", entity.entity_id},
                            {"name", entity.name},
                            {"components", std::move(values)}});
    }
    std::sort(entities.begin(), entities.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.at("entity_id") < rhs.at("entity_id");
    });
    const nlohmann::json envelope = {{"schema", Ecs::EntitySnapshotSchema()},
                                     {"order_contract", Ecs::EntitySnapshotOrderContract()},
                                     {"entity_count", snapshot.entities.size()},
                                     {"component_count", components},
                                     {"entities", std::move(entities)}};
    std::string checked;
    if (!BoundedJsonDump(envelope, SavedInputLimits::kPlantFileBytes, checked, 4)) {
        AddError(errors,
                 "Saved-input admission refused: plant file exceeds 256 MiB compatibility limit.");
        return false;
    }
    bytes = std::move(checked);
    return true;
}

bool WorldSaveService::save_plant_entities(const Luminumbra::Ecs::EntityRegistrySnapshot& snapshot,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors) {
    const SaveGuard guard;
    std::string bytes;
    if (!prepare_plant_entities(snapshot, bytes, errors))
        return false;
    LUMIN_PROFILE_ZONE_N("File plant entities save");
    if (!luminumbra::foliage::ValidatePlantEntitySnapshot(snapshot)) {
        AddError(errors, "Saved-input admission refused: invalid plant component value.");
        return false;
    }
    if (!validate_save(save_dir, errors))
        return false;
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
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        AddError(errors, "failed to create plant directory: " + ec.message());
        return false;
    }
    return WriteFileDurably(path, bytes, staging, errors);
}

bool WorldSaveService::load_plant_entities(Luminumbra::Ecs::EntityRegistrySnapshot& out,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors) {
    LUMIN_PROFILE_ZONE_N("File plant entities load");
    const std::filesystem::path path = plant_entities_path(save_dir);
    const SavedInputFile input(path);
    if (input.missing()) {
        out = Luminumbra::Ecs::EntityRegistrySnapshot{};
        return true; // clean miss: a fresh / no-plant world
    }
    std::string bytes;
    if (!input.read(bytes, SavedInputLimits::kPlantFileBytes)) {
        AddError(errors,
                 "Saved-input admission refused: unreadable, non-regular or over 256 MiB plant "
                 "entities file: " +
                     Filesystem::GenericUtf8PathString(path));
        return false;
    }
    std::vector<std::string> load_errors;
    Luminumbra::Ecs::EntityRegistrySnapshot candidate;
    if (!AdmitPlantComponentBytes(bytes)) {
        AddError(errors,
                 "Saved-input admission refused: plant component JSON compatibility limit (1 MiB "
                 "each, 128 MiB aggregate).");
        return false;
    }
    if (!Luminumbra::Ecs::LoadEntityRegistrySnapshotJson(bytes, candidate, load_errors)) {
        for (const auto& e : load_errors)
            AddError(errors, e);
        return false;
    }
    if (!luminumbra::foliage::ValidatePlantEntitySnapshot(candidate)) {
        AddError(errors, "Saved-input admission refused: invalid plant component value.");
        return false;
    }
    out = std::move(candidate);
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
        const SavedInputFile input(path);
        if (input.missing()) {
            ledger = world::ActiveRegionLedger(ledger.config(), absent_clock);
            return true;
        }
        std::string bytes;
        if (!input.read(bytes, world::ActiveRegionLedger::kMaxFileBytes)) {
            AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
            return false;
        }
        std::string error;
        if (!world::ActiveRegionLedger::decode(bytes, ledger, error)) {
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
    if (!validate_save(save_dir, errors))
        return false;
    world::WorldClock previous_clock;
    bool required = false;
    std::optional<world::WorldCalendar> render_calendar;
    if (!read_clock_metadata(save_dir, previous_clock, required, errors, nullptr, &render_calendar))
        return false;
    if (render_calendar) {
        AddError(errors, "Corrupt world metadata: renderCalendar conflicts with active regions.");
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
        if (!WriteDurableRegionTemp(staging, bytes, temp, errors))
            return false;
        if (const auto hook = BeforeActiveRegionsReplaceForTesting(); hook && !hook()) {
            RemoveTemporaryFile(temp);
            AddError(errors, "interrupted before active-region ledger replacement");
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

std::uint64_t WorldSaveService::region_simulation_digest(const std::filesystem::path& save_dir,
                                                         world::RegionKey key,
                                                         const RegionDigestInputs& inputs) {
    ValidateRegionDigestKey(key);
    WorldStreamingState region;
    // Filtering precedes insertion: a live pristine or Loading chunk must not shadow a durable
    // edited record with the same id. insert_chunk preserves the first record per id.
    const auto qualifies = [&key](const std::shared_ptr<Chunk>& chunk) {
        if (!chunk || chunk->sdf_provenance() != ChunkSdfProvenance::LoadedOrEdited)
            return false;
        int rx = 0, rz = 0;
        region_coords_for_chunk(chunk->get_coords(), rx, rz);
        return rx == key.x && rz == key.z;
    };
    for (const auto& chunk : inputs.live) {
        if (qualifies(chunk) && chunk->get_state() != ChunkState::Loading)
            region.insert_chunk(chunk);
    }
    for (const auto& chunk : inputs.parked) {
        if (qualifies(chunk) && chunk->get_state() != ChunkState::Loading)
            region.insert_chunk(chunk);
    }
    if (!save_dir.empty()) {
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
            // Durable edited content is complete even if its saved lifecycle was Loading.
            if (qualifies(chunk))
                region.insert_chunk(chunk);
        }
    }
    return std::stoull(ComputeRegionContentHash(region), nullptr, 16);
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
    return WorldSaveService{}.load_world(state, save_dir, errors);
}

bool WorldSaveService::has_chunk_snapshot(const std::filesystem::path& save_dir) {
    WorldStreamingState state;
    std::vector<std::string> errors;
    return WorldSaveService{}.load_world(state, save_dir, errors) && !state.empty();
}

bool WorldSaveService::validate_save(const std::filesystem::path& save_dir,
                                     std::vector<std::string>* errors,
                                     const std::stop_token& stop) {
    WorldStreamingState state;
    std::vector<std::string> diagnostics;
    WorldSaveService{}.load_world(state, save_dir, diagnostics, stop);
    for (const auto& message : diagnostics)
        AddError(errors, message);
    return diagnostics.empty();
}

bool WorldSaveService::read_container_records(const std::filesystem::path& region_file,
                                              std::vector<ContainerRecord>& out_records,
                                              std::vector<std::string>* errors) {
    out_records.clear();
    std::vector<RegionRecord> raw_records;
    if (!ReadRegionFile(region_file, raw_records, errors, {}, true)) {
        return false;
    }
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
    std::vector<RegionRecord> raw_records;
    if (!ReadRegionFile(region_file, raw_records, errors, {}, true)) {
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
    std::vector<RegionRecord> raw_records;
    if (!ReadRegionFile(region_file, raw_records, errors, {}, true)) {
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
    const SaveGuard guard;
    // A conventional region destination belongs to a save; validate its calendar
    // before creating directories or replacing raw far-LOD records.
    const auto parent = region_file.parent_path();
    if (parent.filename() == kRegionDirectoryName &&
        parent.parent_path().filename() == kChunksDirectoryName) {
        world::WorldClock clock;
        bool required = false;
        if (!read_clock_metadata(parent.parent_path().parent_path(), clock, required, errors))
            return false;
    }
    try {
        std::vector<RegionRecord> raw_records;
        if (!ReadRegionFile(region_file, raw_records, errors, {}, true))
            return false;

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

        if (!AdmitRegionWrite(raw_records, errors))
            return false;
        if (!parent.empty())
            std::filesystem::create_directories(parent);
        return WriteRegionFile(region_file, raw_records, errors);
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
    // Callers hold SaveGuard and reclaim staging before reaching this helper.
    std::error_code directory_error;
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), directory_error);
    }
    if (directory_error) {
        AddError(errors,
                 "cannot create directory " +
                     Filesystem::GenericUtf8PathString(destination.parent_path()) + ": " +
                     directory_error.message());
        return false;
    }

    std::filesystem::path temp_path;
    if (!WriteDurableRegionTemp(staging, bytes, temp_path, errors)) {
        return false;
    }
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
        const SavedInputFile input(source);
        std::string contents;
        if (!input.read(contents, expected_bytes.size()) || contents != expected_bytes) {
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
                                  std::vector<std::string>* errors) const {
    PreparedWorldWrite prepared;
    return prepared.prepare(state, save_dir, false, errors) && prepared.publish(errors);
}

bool WorldSaveService::load_world(WorldStreamingState& state,
                                  const std::filesystem::path& save_dir,
                                  std::vector<std::string>& errors,
                                  const std::stop_token& stop) const {
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
        nlohmann::json metadata;
        bool metadata_missing = false;
        SavedJsonFailure json_failure = SavedJsonFailure::None;
        if (!ReadSavedJson(metadata_path,
                           SavedInputLimits::kMetadataBytes,
                           metadata,
                           metadata_missing,
                           true,
                           &json_failure) ||
            (!metadata_missing && !AdmitMetadata(metadata)))
            return reject(
                json_failure == SavedJsonFailure::Malformed ||
                        json_failure == SavedJsonFailure::Unreadable
                    ? (json_failure == SavedJsonFailure::Malformed
                           ? "corrupt world metadata: invalid JSON"
                           : "corrupt world metadata: unreadable or non-regular input")
                    : "Saved-input admission refused: unreadable or oversized world metadata.");
        if (!metadata_missing) {
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
            if (metadata.contains("simulationTick") || metadata.contains("calendar") ||
                metadata.contains("renderCalendar")) {
                world::WorldClock clock;
                std::string error;
                std::optional<Vec3> anchor;
                if (!world::WorldClock::from_metadata(metadata, clock, error) ||
                    !ReadAmbientAnchor(metadata, anchor, error))
                    return reject(error);
            }
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
        nlohmann::json manifest;
        bool manifest_missing = false;
        if (!ReadSavedJson(world_manifest_path(save_dir),
                           SavedInputLimits::kManifestBytes,
                           manifest,
                           manifest_missing,
                           false,
                           &json_failure))
            return reject(
                json_failure == SavedJsonFailure::Malformed ||
                        json_failure == SavedJsonFailure::Unreadable
                    ? (json_failure == SavedJsonFailure::Malformed
                           ? "corrupt world manifest: invalid JSON"
                           : "corrupt world manifest: unreadable or non-regular input")
                    : "Saved-input admission refused: unreadable or oversized world manifest.");
        if (!manifest_missing) {
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
        for (const auto& path : region_files) {
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            int rx = 0, rz = 0;
            ParseRegionFileName(path, rx, rz);
            std::vector<RegionRecord> records;
            if (!ReadRegionFile(path, records, &errors, stop))
                return reject("");
            for (const auto& record : records) {
                if (stop.stop_requested())
                    return reject("Saved-world validation cancelled.");
                if (record.lod_level == 0) {
                    const auto chunk = DecodeChunkRecord(record, path, &errors);
                    if (!chunk)
                        return reject("");
                    int chunk_rx = 0, chunk_rz = 0;
                    region_coords_for_chunk(chunk->get_coords(), chunk_rx, chunk_rz);
                    if (chunk_rx != rx || chunk_rz != rz || !state.insert_chunk(chunk))
                        return reject("corrupt chunk region identity or duplicate id: " +
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

std::string WorldSaveService::world_hash(const WorldStreamingState& state,
                                         std::uint64_t water_sim_cursor) const {
    return ComputeWorldStreamingStateHash(state, water_sim_cursor);
}

WorldSaveDirtyReport WorldSaveService::save_dirty_chunks(WorldStreamingState& state,
                                                         const std::filesystem::path& save_dir,
                                                         std::vector<std::string>* errors,
                                                         bool acknowledge_dirty) const {
    const SaveGuard guard;
    WorldSaveDirtyReport report;
    report.chunks_total = state.size();

    const std::vector<ChunkID> dirty_ids = state.dirty_chunk_ids();
    report.chunks_dirty = dirty_ids.size();
    if (dirty_ids.empty()) {
        return report;
    }

    PreparedWorldWrite prepared;
    if (!prepared.prepare(state, save_dir, true, errors) ||
        !prepared.publish(errors, &report.regions_written))
        return report;

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

struct WorldSaveService::PreparedWorldWrite::Data {
    SaveOwnership ownership;
    std::filesystem::path save_dir;
    std::map<std::filesystem::path, std::vector<RegionRecord>> regions;
    bool write_manifest = false;
};

WorldSaveService::PreparedWorldWrite::PreparedWorldWrite() = default;
WorldSaveService::PreparedWorldWrite::~PreparedWorldWrite() = default;

bool WorldSaveService::PreparedWorldWrite::prepare(const WorldStreamingState& state,
                                                   const std::filesystem::path& save_dir,
                                                   bool dirty_only,
                                                   std::vector<std::string>* errors) {
    m_data.reset();
    auto candidate = std::make_unique<Data>();
    candidate->save_dir = save_dir;
    try {
        if (!validate_save(save_dir, errors))
            return false;
        std::set<std::pair<int, int>> dirty_regions;
        if (dirty_only) {
            for (const auto id : state.dirty_chunk_ids()) {
                if (const auto chunk = state.find_chunk(id)) {
                    int rx = 0, rz = 0;
                    region_coords_for_chunk(chunk->get_coords(), rx, rz);
                    dirty_regions.emplace(rx, rz);
                }
            }
        }
        if (dirty_only && dirty_regions.empty()) {
            m_data = std::move(candidate);
            return true;
        }
        // Group the chunks to persist by region (deterministic order).
        std::map<std::pair<int, int>, std::vector<std::shared_ptr<Chunk>>> regions;
        for (const auto& chunk : state.snapshot_chunks()) {
            if (!chunk) {
                continue;
            }
            int rx = 0;
            int rz = 0;
            region_coords_for_chunk(chunk->get_coords(), rx, rz);
            if (!dirty_only || dirty_regions.contains({rx, rz}))
                regions[{rx, rz}].push_back(chunk);
        }

        for (auto& [region_coords, region_chunks] : regions) {
            const std::filesystem::path path =
                region_file_path(save_dir, region_coords.first, region_coords.second);

            // Merge with existing on-disk records: chunks absent from memory
            // and far-LOD tile records (lod_level > 0) are preserved
            // verbatim; records for the chunks being written are replaced.
            std::vector<RegionRecord> records;
            if (!ReadRegionFile(path, records, errors, {}, true))
                return false;
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

            if (!AdmitRegionWrite(records, errors))
                return false;
            candidate->regions.emplace(path, std::move(records));
        }
        candidate->write_manifest = !dirty_only || !regions.empty();
        m_data = std::move(candidate);
        return true;
    } catch (const std::exception& error) {
        AddError(errors, std::string("failed to prepare world region files: ") + error.what());
        return false;
    }
}

bool WorldSaveService::PreparedWorldWrite::publish(std::vector<std::string>* errors,
                                                   std::size_t* regions_written) {
    auto prepared = std::move(m_data);
    if (!prepared)
        return false;
    if (regions_written)
        *regions_written = 0;
    if (!prepared->write_manifest)
        return true;
    try {
        std::filesystem::create_directories(region_directory(prepared->save_dir));
        for (auto& [path, records] : prepared->regions) {
            if (!WriteRegionFile(path, records, errors))
                return false;
        }
        if (!WriteWorldManifest(world_manifest_path(prepared->save_dir), errors))
            return false;
        if (regions_written)
            *regions_written = prepared->regions.size();
        return true;
    } catch (const std::exception& error) {
        AddError(errors, std::string("failed to write world region files: ") + error.what());
        return false;
    }
}

} // namespace Luminumbra::Persistence
