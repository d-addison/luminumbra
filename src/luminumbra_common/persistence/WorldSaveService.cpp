#include "WorldSaveService.h"
#include "CreatedWorldArtifacts.h"
#include "RegionFileWriteGuard.h"
#include "core/Profiler.h"

#include "../ecs/EntitySnapshot.h" //  plant entity snapshot persistence
#include "PlantPersistence.h"
#include "SavedInputFile.h"
#include "SavedInputLimits.h"
#include "SavedJsonAdmission.h"
#include "WorldPersistenceRoundtrip.h"
#include "core/CaptureContentPaths.h"
#include "core/FilesystemPath.h"
#include "core/Log.h"
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
#include <random>
#include <set>
#include <shared_mutex>
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
namespace detail {

struct RegionFileSync {
    std::shared_mutex publication;
    std::atomic<std::size_t> waiting_publishers{0};
};

namespace {
RegionFileSync& RegionFileSyncFor(const std::filesystem::path& path) {
    // Fixed storage bounds synchronization overhead regardless of world size.
    // Hash the generated filename so relative/absolute world paths and Windows
    // case differences select the same stripe. Collisions may delay publication;
    // readers never acquire the writer mutex that covers compression or fsync.
    static std::array<RegionFileSync, 256> stripes;
    std::size_t hash = 0;
    const auto filename = path.filename().native();
    for (const auto character : filename) {
        const auto folded =
            character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
        hash = hash * 31u + static_cast<std::size_t>(folded);
    }
    return stripes[hash % stripes.size()];
}
} // namespace

FilePublicationReadGuard::FilePublicationReadGuard(const std::filesystem::path& path)
    : m_sync(&RegionFileSyncFor(path)) {
    m_sync->publication.lock_shared();
}

FilePublicationReadGuard::~FilePublicationReadGuard() {
    m_sync->publication.unlock_shared();
}

RegionFileWriteGuard::RegionFileWriteGuard(const std::filesystem::path&) {}

RegionFileWriteGuard::~RegionFileWriteGuard() = default;

std::size_t RegionFileWriteGuard::waiting_writers() const {
    return WorldSaveService::waiting_save_writers();
}

} // namespace detail

// File-operation injection is defined here rather than in the production API.
// Persistence regressions observe real open streams and emulate Windows sharing
// violations on every platform. Hooks are installed before starting any workers.
namespace detail {
using ReadObserver = void (*)(const std::filesystem::path&, bool);
using PublicationHook = bool (*)(const std::filesystem::path&, bool);
namespace {
std::atomic<ReadObserver>& ReadObserverForTesting() {
    static std::atomic<ReadObserver> observer{nullptr};
    return observer;
}
std::atomic<PublicationHook>& PublicationHookForTesting() {
    static std::atomic<PublicationHook> hook{nullptr};
    return hook;
}
} // namespace
void set_file_read_observer_for_testing(ReadObserver observer) {
    ReadObserverForTesting().store(observer, std::memory_order_release);
}
void set_file_publication_hook_for_testing(PublicationHook hook) {
    PublicationHookForTesting().store(hook, std::memory_order_release);
}
std::size_t waiting_file_publishers_for_testing(const std::filesystem::path& path) {
    return RegionFileSyncFor(path).waiting_publishers.load(std::memory_order_acquire);
}
} // namespace detail
namespace {
bool MissingSavedInputAncestorsAreValid(const std::filesystem::path& path);
}

SavedInputFile::SavedInputFile(const std::filesystem::path& path, LeafSymlinks leaf_symlinks)
    : m_publication(path)
    , m_path(path) {
#if defined(_WIN32)
    // Win32 can report PATH_NOT_FOUND for an overlong component beneath a
    // missing directory. Such an invalid path must never become a clean miss.
    // Win32 file-name components are bounded to 255 UTF-16 code units.
    for (const auto& component : path) {
        if (component.native().size() > 255)
            return;
    }
    const DWORD flags = leaf_symlinks == LeafSymlinks::Refuse
                            ? FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS
                            : FILE_FLAG_BACKUP_SEMANTICS;
    m_handle = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        m_missing = error == ERROR_FILE_NOT_FOUND ||
                    (error == ERROR_PATH_NOT_FOUND && MissingAncestorsAreDirectories(path));
    }
#else
    // NONBLOCK prevents a FIFO open from waiting for a writer; NOFOLLOW
    // refuses even dangling symlinks rather than mistaking them for absence.
    const int flags = leaf_symlinks == LeafSymlinks::Refuse
                          ? O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC
                          : O_RDONLY | O_NONBLOCK | O_CLOEXEC;
    m_descriptor = ::open(path.c_str(), flags);
    m_missing = m_descriptor < 0 && errno == ENOENT;
#endif

#if defined(_WIN32)
    const bool opened = m_handle != INVALID_HANDLE_VALUE;
#else
    const bool opened = m_descriptor >= 0;
#endif
    if (!opened && m_missing && leaf_symlinks == LeafSymlinks::Refuse)
        m_missing = MissingSavedInputAncestorsAreValid(m_path);
    if (opened) {
        if (const auto observer = detail::ReadObserverForTesting().load(std::memory_order_acquire))
            observer(m_path, true);
        if (leaf_symlinks == LeafSymlinks::Refuse)
            m_location_admitted = is_within_save_root();
    }
}

SavedInputFile::~SavedInputFile() {
#if defined(_WIN32)
    const bool opened = m_handle != INVALID_HANDLE_VALUE;
#else
    const bool opened = m_descriptor >= 0;
#endif

#if defined(_WIN32)
    if (m_handle != INVALID_HANDLE_VALUE)
        ::CloseHandle(m_handle);
#else
    if (m_descriptor >= 0)
        ::close(m_descriptor);
#endif

    if (opened) {
        if (const auto observer = detail::ReadObserverForTesting().load(std::memory_order_acquire))
            observer(m_path, false);
    }
}

namespace {

std::shared_mutex& ClockPairPublicationMutex() {
    static std::shared_mutex mutex;
    return mutex;
}

void AcquirePublicationLock(std::unique_lock<std::shared_mutex>& lock,
                            detail::RegionFileSync& sync) {
    if (!lock.try_lock()) {
        // Count only confirmed contention, so diagnostics establish that an
        // open reader really prevented publication, independent of scheduling.
        sync.waiting_publishers.fetch_add(1, std::memory_order_release);
        lock.lock();
        sync.waiting_publishers.fetch_sub(1, std::memory_order_release);
    }
}

struct PublicationLocks {
    std::unique_lock<std::shared_mutex> clock_pair;
    std::unique_lock<std::shared_mutex> file;
};

PublicationLocks LockPublication(const std::filesystem::path& path) {
    auto& sync = detail::RegionFileSyncFor(path);
    PublicationLocks locks{.clock_pair =
                               std::unique_lock(ClockPairPublicationMutex(), std::defer_lock),
                           .file = std::unique_lock(sync.publication, std::defer_lock)};
    // Readers comparing ledger and clock need one consistent pair, including
    // across a rewind. Writers keep the installed ledger tick <= clock tick.
    // Only replacement holds this gate; staging and fsync never acquire it.
    if (path.filename() == "world_info.json" || path.filename() == "active-regions.arl")
        AcquirePublicationLock(locks.clock_pair, sync);
    AcquirePublicationLock(locks.file, sync);
    return locks;
}

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

// Discovery may follow a directory link only within the resolved save root.
// Payload leaves still use no-follow, same-handle type/length/read admission.
// Missing directories are quiet; a present dangling link or wrong type is not.
enum class PersistenceDirectory {
    Missing,
    Present,
    Invalid
};

bool IsSaveDescendant(const std::filesystem::path& target, const std::filesystem::path& root) {
    auto component = target.begin();
    // Both resolved paths are bounded to 32767 native characters, bounding this walk.
    for (const auto& expected : root) {
        if (component == target.end() || !Filesystem::PathSpellingEqual(*component, expected))
            return false;
        ++component;
    }
    return component != target.end();
}

PersistenceDirectory InspectPersistenceDirectory(const std::filesystem::path& path,
                                                 const std::filesystem::path& save_root) {
    if (path.native().size() > 32767 || save_root.native().size() > 32767)
        return PersistenceDirectory::Invalid;
    std::error_code error;
    const auto leaf = std::filesystem::symlink_status(path, error);
    if (leaf.type() == std::filesystem::file_type::not_found &&
        (!error || error == std::errc::no_such_file_or_directory))
        return PersistenceDirectory::Missing;
    if (error)
        return PersistenceDirectory::Invalid;
    if (std::filesystem::is_directory(leaf))
        return PersistenceDirectory::Present;
    if (!std::filesystem::is_symlink(leaf) || !std::filesystem::is_directory(path, error) || error)
        return PersistenceDirectory::Invalid;
    const auto root = Filesystem::WeaklyCanonicalPath(
        save_root.empty() ? std::filesystem::path(".") : save_root, error);
    if (error)
        return PersistenceDirectory::Invalid;
    const auto target = Filesystem::WeaklyCanonicalPath(path, error);
    if (error || root.native().size() > 32767 || target.native().size() > 32767)
        return PersistenceDirectory::Invalid;
    return IsSaveDescendant(target, root) ? PersistenceDirectory::Present
                                          : PersistenceDirectory::Invalid;
}

bool MissingSavedInputAncestorsAreValid(const std::filesystem::path& path) {
    const auto directory = path.parent_path();
    if (directory.filename() != kRegionDirectoryName ||
        directory.parent_path().filename() != kChunksDirectoryName)
        return true;
    const auto save_root = directory.parent_path().parent_path();
    // A missing payload beneath an escaping/dangling directory link is a
    // refusal, not an optional miss. Ordinary missing directories stay quiet.
    return InspectPersistenceDirectory(directory.parent_path(), save_root) !=
               PersistenceDirectory::Invalid &&
           InspectPersistenceDirectory(directory, save_root) != PersistenceDirectory::Invalid;
}

#if !defined(_WIN32)
int OpenResolvedSavedInput(const std::filesystem::path& resolved) {
#if defined(O_PATH)
    constexpr int directory_access = O_PATH;
#elif defined(O_SEARCH)
    constexpr int directory_access = O_SEARCH;
#else
    constexpr int directory_access = O_RDONLY;
#endif
    constexpr int directory_flags = directory_access | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int descriptor = ::open(resolved.root_path().c_str(), directory_flags);
    if (descriptor < 0)
        return -1;
    try {
        const auto relative = resolved.relative_path();
        // The caller bounds resolved to 32767 native characters before this walk.
        for (auto component = relative.begin(); component != relative.end(); ++component) {
            auto next = component;
            ++next;
            const int flags = next == relative.end()
                                  ? O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC
                                  : directory_flags;
            const int child = ::openat(descriptor, component->c_str(), flags);
            ::close(descriptor);
            descriptor = child;
            if (descriptor < 0)
                return -1;
        }
    } catch (...) {
        ::close(descriptor);
        throw;
    }
    return descriptor;
}
#endif

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
                      FileIdentity* published_identity = nullptr,
                      CreatedWorldArtifacts* artifacts = nullptr);

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
    } else {
        LUMINUMBRA_CORE_ERROR("{}", message);
    }
}

bool AllowPublication(const std::filesystem::path& path,
                      bool removing,
                      std::vector<std::string>* errors) {
    if (const auto hook = detail::PublicationHookForTesting().load(std::memory_order_acquire);
        hook && !hook(path, removing)) {
        AddError(errors, "file publication refused: " + Filesystem::GenericUtf8PathString(path));
        return false;
    }
    return true;
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

void RemoveTemporaryFile(const std::filesystem::path& path,
                         CreatedWorldArtifacts* artifacts = nullptr) {
    // Creation rollback owns these files and checks identity after writers drain.
    if (artifacts)
        return;
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
}

// A sidecar is separate from every save format. Its instance identity is the
// exclusively created record's file identity; the payload binds that instance
// to one filename, one staging identity/change stamp and one save directory.
using StagingIdentity = std::array<std::uint64_t, 6>;

class StagingFile {
public:
    enum class RetainedAccess {
        Identity,
        Read
    };
    enum class Mode {
        Inspect,
        Create,
        Deferred,
        Directory
    };

    StagingFile(const std::filesystem::path& path, Mode mode)
        : m_path(path) {
        if (mode != Mode::Deferred)
            open(mode);
    }

    void open(Mode mode) {
#if defined(_WIN32)
        const DWORD access = mode == Mode::Directory ? FILE_READ_ATTRIBUTES
                             : mode == Mode::Create  ? GENERIC_READ | GENERIC_WRITE
                                                     : GENERIC_READ | DELETE;
        const DWORD sharing = mode == Mode::Create || mode == Mode::Directory
                                  ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
                                  : FILE_SHARE_READ | FILE_SHARE_DELETE;
        m_handle = ::CreateFileW(m_path.c_str(),
                                 access,
                                 sharing,
                                 nullptr,
                                 mode == Mode::Create ? CREATE_NEW : OPEN_EXISTING,
                                 (mode == Mode::Directory ? 0 : FILE_FLAG_OPEN_REPARSE_POINT) |
                                     (mode == Mode::Create ? FILE_FLAG_WRITE_THROUGH : 0) |
                                     FILE_FLAG_BACKUP_SEMANTICS,
                                 nullptr);
#else
        const int flags = mode == Mode::Create ? O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC
                          : mode == Mode::Directory
                              ? O_RDONLY | O_DIRECTORY | O_CLOEXEC
                              : O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
        m_descriptor = ::open(m_path.c_str(), flags, 0666);
#endif
    }
    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;
    ~StagingFile() {
        close();
    }

    bool close() {
#if defined(_WIN32)
        if (m_handle == INVALID_HANDLE_VALUE)
            return true;
        const bool closed = ::CloseHandle(m_handle) != 0;
        m_handle = INVALID_HANDLE_VALUE;
#else
        if (m_descriptor < 0)
            return true;
        const bool closed = ::close(m_descriptor) == 0;
        m_descriptor = -1;
#endif
        return closed;
    }

    bool identity(StagingIdentity& result, bool directory = false, unsigned links = 1) const {
#if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION info{};
        if (m_handle == INVALID_HANDLE_VALUE || ::GetFileType(m_handle) != FILE_TYPE_DISK ||
            !::GetFileInformationByHandle(m_handle, &info) ||
            (info.nFileIndexHigh == 0 && info.nFileIndexLow == 0) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory ||
            (!directory && (info.nNumberOfLinks == 0 || info.nNumberOfLinks > links)))
            return false;
        result = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow, 0, 0, 0};
        // Directory contents may change between recording and orphan cleanup.
        // Bind only volume/index; retain timestamps and length for ordinary files.
        if (directory)
            return true;
        result[3] = (static_cast<std::uint64_t>(info.ftCreationTime.dwHighDateTime) << 32) |
                    info.ftCreationTime.dwLowDateTime;
        result[4] = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                    info.ftLastWriteTime.dwLowDateTime;
        result[5] = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
        struct stat info {};
        if (m_descriptor < 0 || ::fstat(m_descriptor, &info) != 0 || info.st_ino == 0 ||
            (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) ||
            (!directory && (info.st_nlink == 0 || info.st_nlink > links || info.st_size < 0)))
            return false;
        result = {static_cast<std::uint64_t>(info.st_dev),
                  static_cast<std::uint64_t>(info.st_ino),
                  0,
                  0,
                  0,
                  0};
        // ctime changes on entry creation, rename and removal within a directory.
        if (directory)
            return true;
#if defined(__APPLE__)
        const auto changed = info.st_ctimespec;
#else
        const auto changed = info.st_ctim;
#endif
        result[3] = static_cast<std::uint64_t>(changed.tv_sec);
        result[4] = static_cast<std::uint64_t>(changed.tv_nsec);
        result[5] = static_cast<std::uint64_t>(info.st_size);
#endif
        return true;
    }

    bool write(const std::string& bytes) const {
        std::size_t offset = 0;
        // Bounded by the admitted ledger size (or the 512-byte sidecar bound).
        while (offset < bytes.size()) {
#if defined(_WIN32)
            DWORD written = 0;
            const auto count =
                static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 65536));
            if (!::WriteFile(m_handle, bytes.data() + offset, count, &written, nullptr) ||
                written == 0)
                return false;
#else
            const auto count = std::min<std::size_t>(bytes.size() - offset, 65536);
            const auto written = ::write(m_descriptor, bytes.data() + offset, count);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return false;
#endif
            offset += static_cast<std::size_t>(written);
        }
#if defined(_WIN32)
        return ::FlushFileBuffers(m_handle) != 0;
#else
        int flushed = 0;
        do {
            flushed = ::fsync(m_descriptor);
        } while (flushed != 0 && errno == EINTR);
        return flushed == 0;
#endif
    }

    bool promote_seed(const std::filesystem::path& destination,
                      bool& occupied,
                      std::vector<std::string>* errors) {
        occupied = false;
        if (!matches_name())
            return refuse("staging reservation name no longer matches retained identity", errors);
#if defined(_WIN32)
        if (!::MoveFileExW(m_path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = ::GetLastError();
            occupied = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS;
            return refuse("cannot exclusively promote staging reservation", errors);
        }
#else
            // Prefer native no-replace rename, including on filesystems without
            // hard links. The portable fallback also refuses every occupied leaf.
#if defined(__linux__)
        const int renamed = static_cast<int>(::syscall(SYS_renameat2,
                                                       AT_FDCWD,
                                                       m_path.c_str(),
                                                       AT_FDCWD,
                                                       destination.c_str(),
                                                       RENAME_NOREPLACE));
        if (renamed != 0 && errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) {
            occupied = errno == EEXIST;
            return refuse("cannot exclusively promote staging reservation", errors);
        }
        const bool needs_link = renamed != 0;
#elif defined(__APPLE__)
        if (::renamex_np(m_path.c_str(), destination.c_str(), RENAME_EXCL) != 0) {
            occupied = errno == EEXIST;
            return refuse("cannot exclusively promote staging reservation", errors);
        }
        constexpr bool needs_link = false;
#else
        constexpr bool needs_link = true;
#endif
        if (needs_link) {
            // An interruption between link and unlink leaves two names, both
            // of which recovery verifies against the durable reservation.
            if (::link(m_path.c_str(), destination.c_str()) != 0) {
                occupied = errno == EEXIST;
                return refuse("cannot exclusively promote staging reservation", errors);
            }
            if (!matches_name(2) || ::unlink(m_path.c_str()) != 0)
                return refuse("cannot retire staging reservation name", errors);
        }
#endif
        m_path = destination;
        return matches_name();
    }

    bool truncate_payload() const {
#if defined(_WIN32)
        LARGE_INTEGER start{};
        return ::SetFilePointerEx(m_handle, start, nullptr, FILE_BEGIN) && ::SetEndOfFile(m_handle);
#else
        return ::lseek(m_descriptor, 0, SEEK_SET) == 0 && ::ftruncate(m_descriptor, 0) == 0;
#endif
    }

    bool finish_write(RetainedAccess access, std::vector<std::string>* errors) {
        // Check close errors before use, then retain only the same file through
        // publication. A replacement observed during reopening is never adopted.
        StagingIdentity before{};
        if (!identity(before))
            return refuse("cannot identify staging file before closing writer", errors);
#if defined(_WIN32)
        const bool closed = ::CloseHandle(m_handle) != 0;
        m_handle = INVALID_HANDLE_VALUE;
        if (!closed)
            return refuse("cannot close staging writer", errors);
        // ReplaceFileW opens the replacement without sharing read/write/delete
        // access. Retain attributes only for a payload: a GENERIC_READ handle
        // conflicts even when that handle itself grants FILE_SHARE_DELETE.
        // Sidecars need read access but are never replacement payloads.
        m_handle =
            ::CreateFileW(m_path.c_str(),
                          access == RetainedAccess::Read ? GENERIC_READ : FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                          nullptr);
#else
        const bool closed = ::close(m_descriptor) == 0;
        m_descriptor = -1;
        if (!closed)
            return refuse("cannot close staging writer", errors);
        (void)access;
        m_descriptor = ::open(m_path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
#endif
        StagingIdentity after{};
#if defined(_WIN32)
        // Windows finalizes last-write time when the writing handle closes.
        // Creation time, identity and length must survive the reopen unchanged.
        if (identity(after) && std::equal(before.begin(), before.begin() + 4, after.begin()) &&
            before[5] == after[5])
            return true;
#else
        if (identity(after) && after == before)
            return true;
#endif
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
#else
        if (m_descriptor >= 0)
            ::close(m_descriptor);
        m_descriptor = -1;
#endif
        return refuse("cannot retain unchanged staging identity after closing writer", errors);
    }

    bool read_record(std::string& bytes) const {
        StagingIdentity current{};
        // Sidecars have a fixed 512-byte admission ceiling, before allocation.
        if (!identity(current) || current[5] > 512)
            return false;
        bytes.assign(static_cast<std::size_t>(current[5]), '\0');
#if defined(_WIN32)
        LARGE_INTEGER start{};
        if (!::SetFilePointerEx(m_handle, start, nullptr, FILE_BEGIN))
            return false;
#else
        if (::lseek(m_descriptor, 0, SEEK_SET) < 0)
            return false;
#endif
        std::size_t offset = 0;
        while (offset < bytes.size()) {
#if defined(_WIN32)
            DWORD received = 0;
            if (!::ReadFile(m_handle,
                            bytes.data() + offset,
                            static_cast<DWORD>(bytes.size() - offset),
                            &received,
                            nullptr) ||
                received == 0)
                return false;
#else
            const auto received =
                ::read(m_descriptor, bytes.data() + offset, bytes.size() - offset);
            if (received < 0 && errno == EINTR)
                continue;
            if (received <= 0)
                return false;
#endif
            offset += static_cast<std::size_t>(received);
        }
        StagingIdentity after{};
        return identity(after) && after == current;
    }

    bool matches_path(const std::filesystem::path& path, unsigned links = 1) const {
        StagingIdentity held{};
        FileIdentity named;
        if (!identity(held, false, links) || !ReadFileIdentity(path, named))
            return false;
#if defined(_WIN32)
        return named.volume == held[0] && named.index_high == held[1] && named.index_low == held[2];
#else
        return static_cast<std::uint64_t>(named.device) == held[0] &&
               static_cast<std::uint64_t>(named.inode) == held[1];
#endif
    }

    bool matches_name(unsigned links = 1) const {
        return matches_path(m_path, links);
    }

    void retain_publication_identity() {
#if defined(_WIN32)
        // Both first installation and journal creation publish through hard
        // links. A named payload handle keeps the source link delete-pending
        // even with delete sharing; retain only its identity before publication.
        const auto retained = CreatedWorldArtifacts::retain_file_identity(m_handle);
        const bool retired = close();
        m_handle = retained;
        if (!retired)
            throw std::runtime_error("Cannot close named staging payload handle.");
#endif
    }

    void record_artifact(CreatedWorldArtifacts& artifacts) {
        retain_publication_identity();
#if defined(_WIN32)
        artifacts.record_staged_file(m_path, m_handle);
#else
        artifacts.record_staged_file(m_path, m_descriptor);
#endif
        m_artifacts = &artifacts;
    }

    bool remove(unsigned links = 1) {
        StagingIdentity held{};
        if (!identity(held, false, links) || !matches_name(links))
            return false;
#if defined(_WIN32)
        // Reopen only for delete access, verify against the retained identity,
        // then disposition targets that handle even if the name is substituted.
        const HANDLE retiring =
            ::CreateFileW(m_path.c_str(),
                          DELETE | FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                          nullptr);
        if (retiring == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION info{};
        FILE_DISPOSITION_INFO disposition{TRUE};
        const bool removed = ::GetFileInformationByHandle(retiring, &info) &&
                             (info.dwFileAttributes &
                              (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
                             info.nNumberOfLinks > 0 && info.nNumberOfLinks <= links &&
                             info.dwVolumeSerialNumber == held[0] &&
                             info.nFileIndexHigh == held[1] && info.nFileIndexLow == held[2] &&
                             ::SetFileInformationByHandle(retiring,
                                                          FileDispositionInfo,
                                                          &disposition,
                                                          static_cast<DWORD>(sizeof(disposition)));
        // Disposition alone leaves the name enumerable until ALL retained
        // handles close, including the creation journal's open-by-ID handle.
        // Keep retiring open while releasing those references, so identity
        // cannot be recycled between the removal and journal retirement.
        const bool finished = removed && finish_removal();
        const bool closed = ::CloseHandle(retiring) != 0;
        return finished && closed;
#else
        struct stat named {};
        // SaveGuard excludes cooperating writers. Recheck at unlink itself;
        // POSIX has no identity-conditioned unlink for hostile concurrent writers.
        return ::lstat(m_path.c_str(), &named) == 0 && S_ISREG(named.st_mode) &&
               named.st_nlink > 0 && named.st_nlink <= links &&
               static_cast<std::uint64_t>(named.st_dev) == held[0] &&
               static_cast<std::uint64_t>(named.st_ino) == held[1] &&
               ::unlink(m_path.c_str()) == 0 && finish_removal();
#endif
    }

    bool refuse(const char* reason, std::vector<std::string>* errors) const {
        AddError(errors, std::string(reason) + ": " + Filesystem::GenericUtf8PathString(m_path));
        return false;
    }

private:
    bool finish_removal() {
        if (m_artifacts) {
            try {
#if defined(_WIN32)
                m_artifacts->forget_removed_staged_file(m_path, m_handle);
#else
                m_artifacts->forget_removed_staged_file(m_path, m_descriptor);
#endif
                m_artifacts = nullptr;
            } catch (const std::exception&) {
                return false;
            }
        }
        return close();
    }

    std::filesystem::path m_path;
    CreatedWorldArtifacts* m_artifacts = nullptr;
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_descriptor = -1;
#endif
};

std::filesystem::path OwnershipPath(const std::filesystem::path& staging) {
    auto record = staging;
    record += ".owner";
    return record;
}

std::filesystem::path ClaimPath(const std::filesystem::path& staging) {
    auto record = staging;
    record += ".claim";
    return record;
}

StagingIdentity StablePayloadIdentity(const StagingIdentity& identity) {
    // An incomplete claim binds the reserved inode/file index, before its
    // contents and change stamps exist. Completed .owner records keep all six words.
    auto stable = identity;
    std::fill(stable.begin() + 3, stable.end(), 0);
    return stable;
}

std::string StagingOwnershipBytes(const std::filesystem::path& staging,
                                  const StagingIdentity& world_identity,
                                  const StagingIdentity& staging_identity,
                                  const StagingIdentity& instance_identity) {
    const auto filename = staging.filename();
    const auto& native_name = filename.native();
    // Native staging names are short ASCII; reject foreign names before UTF-8
    // conversion, including ill-formed native Unicode. The record is <= 512 bytes.
    if (native_name.size() > 256 ||
        std::any_of(native_name.begin(), native_name.end(), [](auto unit) {
            return static_cast<std::uint64_t>(unit) > 127;
        }))
        return {};
    const auto name = Filesystem::GenericUtf8PathString(filename);
    // Version 1 already stores only the directory's three stable identity words.
    // At most 408 bytes: 8 magic + 12 identity words + name length + 256 name
    // bytes + 32 instance bytes + checksum. This stays within the 512-byte reader bound.
    std::string bytes = "AROWN001";
    for (std::size_t index = 0; index < 3; ++index)
        AppendU64(bytes, world_identity[index]);
    for (const auto value : staging_identity)
        AppendU64(bytes, value);
    for (std::size_t index = 0; index < 3; ++index)
        AppendU64(bytes, instance_identity[index]);
    AppendU64(bytes, name.size());
    bytes += name;
    const auto instance_start = name.rfind('.');
    if (instance_start == std::string::npos || name.size() - instance_start - 1 != 32)
        return {};
    const auto instance = name.substr(instance_start + 1);
    if (instance.find_first_not_of("0123456789abcdef") != std::string::npos)
        return {};
    bytes += instance;
    std::uint64_t checksum = 14695981039346656037ull;
    for (const unsigned char byte : bytes) {
        checksum ^= byte;
        checksum *= 1099511628211ull;
    }
    AppendU64(bytes, checksum);
    return bytes;
}

bool ValidStagingOwnership(const std::filesystem::path& staging,
                           const StagingFile& payload,
                           const StagingFile& record,
                           std::string* reason = nullptr) {
    // This detects invalid/altered records, not deliberate valid fabrication by
    // an actor already able to write the save directory; that is outside scope.
    const auto parent =
        staging.has_parent_path() ? staging.parent_path() : std::filesystem::path(".");
    const StagingFile directory(parent, StagingFile::Mode::Directory);
    StagingIdentity world_identity{}, staging_identity{}, instance_identity{};
    std::string recorded;
    const auto refuse = [reason](const char* message) {
        if (reason)
            *reason = message;
        return false;
    };
    if (!payload.matches_name())
        return refuse("staging payload name no longer matches retained identity");
    if (!record.matches_name())
        return refuse("staging ownership name no longer matches retained identity");
    if (!directory.identity(world_identity, true))
        return refuse("cannot identify staging save directory during ownership validation");
    if (!payload.identity(staging_identity))
        return refuse("cannot identify staging payload during ownership validation");
    if (!record.identity(instance_identity))
        return refuse("cannot identify staging ownership instance during validation");
    if (!record.read_record(recorded))
        return refuse("cannot read complete unchanged staging ownership within 512-byte limit");
    const auto expected =
        StagingOwnershipBytes(staging, world_identity, staging_identity, instance_identity);
    if (expected.empty())
        return refuse("cannot encode staging ownership filename or instance during validation");
    if (recorded != expected)
        return refuse(
            "staging ownership bytes do not match directory, payload, instance or filename");
    return true;
}

bool ReclaimOwnedLedgerStaging(const std::filesystem::path& staging) {
    StagingFile payload(staging, StagingFile::Mode::Inspect);
    StagingFile record(OwnershipPath(staging), StagingFile::Mode::Inspect);
    if (!ValidStagingOwnership(staging, payload, record))
        return false;
    if (!payload.remove())
        return payload.refuse("cannot reclaim verified abandoned staging payload", nullptr);
    if (!record.remove())
        record.refuse("cannot reclaim staging ownership after payload removal", nullptr);
    FlushRemovalDirectory(staging, nullptr);
    return true;
}

bool ValidOrphanedStagingOwnership(const std::filesystem::path& staging,
                                   const StagingFile& record,
                                   StagingIdentity* payload_identity = nullptr) {
    const auto parent =
        staging.has_parent_path() ? staging.parent_path() : std::filesystem::path(".");
    const StagingFile directory(parent, StagingFile::Mode::Directory);
    StagingIdentity world_identity{}, staging_identity{}, instance_identity{};
    std::string recorded;
    if (!record.matches_name() || !directory.identity(world_identity, true) ||
        !record.identity(instance_identity) || !record.read_record(recorded))
        return false;
    // Only the historical payload identity is unavailable after publication.
    // Read its six fixed-width fields within the admitted 512-byte record, then
    // require the entire canonical encoding, including all remaining bindings.
    constexpr std::size_t kPayloadIdentityOffset = 8 + 3 * 8;
    if (recorded.size() < kPayloadIdentityOffset + staging_identity.size() * 8)
        return false;
    for (std::size_t index = 0; index < staging_identity.size(); ++index)
        staging_identity[index] = ReadU64(reinterpret_cast<const unsigned char*>(
            recorded.data() + kPayloadIdentityOffset + index * 8));
    const auto expected =
        StagingOwnershipBytes(staging, world_identity, staging_identity, instance_identity);
    if (expected.empty() || recorded != expected)
        return false;
    if (payload_identity)
        *payload_identity = staging_identity;
    return true;
}

bool StagingPathMissing(const std::filesystem::path& staging) {
    // A failed open is not absence: preserve inaccessible and nonregular leaves,
    // including dangling symlinks and reparse points.
    const auto refuse = [&staging](const char* reason) {
        AddError(nullptr, std::string(reason) + ": " + Filesystem::GenericUtf8PathString(staging));
        return false;
    };
#if defined(_WIN32)
    const HANDLE leaf = ::CreateFileW(staging.c_str(),
                                      FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr);
    if (leaf != INVALID_HANDLE_VALUE) {
        if (!::CloseHandle(leaf))
            return refuse("cannot close staging leaf inspection handle during orphan cleanup");
        return refuse("preserving staging ownership: payload leaf is still present");
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        return true;
#else
    struct stat named {};
    if (::lstat(staging.c_str(), &named) == 0)
        return refuse("preserving staging ownership: payload leaf is still present");
    if (errno == ENOENT)
        return true;
#endif
    return refuse("cannot inspect staging payload leaf during orphan cleanup");
}

bool ReclaimOrphanedLedgerStaging(const std::filesystem::path& staging) {
    StagingFile record(OwnershipPath(staging), StagingFile::Mode::Inspect);
    if (!ValidOrphanedStagingOwnership(staging, record) || !StagingPathMissing(staging))
        return false;
    if (!record.remove())
        return record.refuse("cannot reclaim verified orphaned staging ownership", nullptr);
    FlushRemovalDirectory(staging, nullptr);
    return true;
}

bool ReclaimClaimedLedgerStaging(const std::filesystem::path& staging) {
    StagingFile claim(ClaimPath(staging), StagingFile::Mode::Inspect);
    StagingIdentity reserved{};
    if (!ValidOrphanedStagingOwnership(staging, claim, &reserved) ||
        reserved != StablePayloadIdentity(reserved) || (reserved[1] == 0 && reserved[2] == 0))
        return false;
    StagingFile payload(staging, StagingFile::Mode::Inspect);
    StagingIdentity present{};
    if (payload.identity(present, false, 2)) {
        if (StablePayloadIdentity(present) != reserved || !payload.matches_name(2))
            return false;
        // A POSIX interruption between link and unlink leaves both names of
        // the reservation. Permit two links only when BOTH names prove that
        // exact inode; an unrelated hard link is never sufficient.
        StagingFile seed(OwnershipPath(staging), StagingFile::Mode::Inspect);
        StagingIdentity seed_identity{};
        if (seed.identity(seed_identity, false, 2) &&
            StablePayloadIdentity(seed_identity) == reserved) {
            if (!seed.remove(2) || !seed.close())
                return false;
        }
        if (!payload.remove() || !payload.close())
            return false;
        // A completed record is removable by the ordinary orphan validator.
        // A partial or substituted completion remains unprovable and survives.
        ReclaimOrphanedLedgerStaging(staging);
    } else if (!StagingPathMissing(staging)) {
        return false;
    } else {
        ReclaimOrphanedLedgerStaging(staging);
    }
    if (!claim.remove())
        return claim.refuse("cannot reclaim verified staging claim", nullptr);
    FlushRemovalDirectory(staging, nullptr);
    return true;
}

std::filesystem::path NextLedgerTempPath(const std::filesystem::path& destination) {
    auto path = NextRegionTempPath(destination);
    std::random_device entropy;
    // A 128-bit suffix avoids predictable collisions after sequence/PID reuse.
    constexpr char digits[] = "0123456789abcdef";
    std::string instance;
    instance.reserve(32);
    for (int word = 0; word < 4; ++word) {
        const auto value = static_cast<std::uint32_t>(entropy());
        for (int shift = 28; shift >= 0; shift -= 4)
            instance.push_back(digits[(value >> shift) & 15u]);
    }
    path += "." + instance;
    return path;
}

class LedgerStaging {
public:
    explicit LedgerStaging(const std::filesystem::path& destination)
        : m_path(NextLedgerTempPath(destination))
        , m_payload(OwnershipPath(m_path), StagingFile::Mode::Create)
        , m_claim(ClaimPath(m_path), StagingFile::Mode::Create)
        , m_record(OwnershipPath(m_path), StagingFile::Mode::Deferred) {}
    ~LedgerStaging() {
        // Only remove objects created by this call. A substituted payload or
        // record has lost proof and both occupants must survive untouched.
        bool payload_remains = false;
        if (m_ready) {
            if (!ValidStagingOwnership(m_path, m_payload, m_record)) {
                payload_remains = true;
                m_record.refuse("preserving staging pair: ownership changed before rollback",
                                nullptr);
            } else if (!m_payload.remove()) {
                payload_remains = true;
                m_payload.refuse("cannot remove verified staging payload during rollback", nullptr);
            } else if (!m_record.remove()) {
                m_record.refuse("cannot remove staging ownership after payload rollback", nullptr);
            }
        } else {
            if (m_payload_created && !m_payload.remove()) {
                payload_remains = true;
                m_payload.refuse("cannot remove newly created staging payload", nullptr);
            }
            if (m_record_created && !m_record.remove())
                m_record.refuse("cannot remove newly created staging ownership", nullptr);
        }
        if (!payload_remains && m_claim_created && !m_claim.remove())
            m_claim.refuse("cannot remove newly created staging claim", nullptr);
    }
    LedgerStaging(const LedgerStaging&) = delete;
    LedgerStaging& operator=(const LedgerStaging&) = delete;

    bool available(std::vector<std::string>* errors) {
        StagingIdentity payload_identity{}, claim_identity{};
        m_payload_created = m_payload.identity(payload_identity);
        m_claim_created = m_claim.identity(claim_identity);
        if (!m_payload_created)
            return m_payload.refuse("cannot exclusively create and identify staging payload",
                                    errors);
        if (!m_claim_created)
            return m_claim.refuse("cannot exclusively create and identify staging ownership",
                                  errors);
        return true;
    }
    bool prepare(const std::string& bytes, std::vector<std::string>* errors) {
        const auto parent =
            m_path.has_parent_path() ? m_path.parent_path() : std::filesystem::path(".");
        const StagingFile directory(parent, StagingFile::Mode::Directory);
        StagingIdentity world_identity{}, staging_identity{}, instance_identity{}, claim_identity{};
        if (!directory.identity(world_identity, true))
            return directory.refuse("cannot identify staging save directory", errors);
        if (!m_payload.identity(instance_identity) || !m_claim.identity(claim_identity))
            return m_record.refuse("cannot identify staging ownership instance", errors);
        // The future payload starts as a small, self-identifying ownership
        // record. No ledger bytes exist until its identity is durably claimed.
        const auto seed = StagingOwnershipBytes(m_path, world_identity, {}, instance_identity);
        const auto claim = StagingOwnershipBytes(
            m_path, world_identity, StablePayloadIdentity(instance_identity), claim_identity);
        if (seed.empty() || claim.empty())
            return m_claim.refuse("cannot encode staging ownership filename or instance", errors);
        if (!m_payload.write(seed) || !m_claim.write(claim))
            return m_claim.refuse("cannot write and flush staging claim", errors);
        if (!m_claim.finish_write(StagingFile::RetainedAccess::Read, errors) ||
            !FlushAncestorEntries(m_path, errors) || !FlushRemovalDirectory(m_path, errors))
            return false;
        if (!m_payload.promote_seed(m_path, m_occupied, errors) ||
            !FlushRemovalDirectory(m_path, errors))
            return false;
        if (!m_payload.truncate_payload() || !m_payload.write(bytes))
            return m_payload.refuse("cannot write and flush staging payload", errors);
        if (!m_payload.finish_write(StagingFile::RetainedAccess::Identity, errors))
            return false;
        if (!m_payload.identity(staging_identity))
            return m_payload.refuse("cannot identify completed staging payload", errors);
        m_record.open(StagingFile::Mode::Create);
        m_record_created = m_record.identity(instance_identity);
        if (!m_record_created)
            return m_record.refuse("cannot exclusively create and identify staging completion",
                                   errors);
        // The record is still empty here. Its size and change stamps move as it is
        // written, so bind only the device and file index, which are what the
        // encoder and every validator compare.
        instance_identity = StablePayloadIdentity(instance_identity);
        const auto ownership =
            StagingOwnershipBytes(m_path, world_identity, staging_identity, instance_identity);
        if (ownership.empty())
            return m_record.refuse("cannot encode staging ownership filename or instance", errors);
        if (!m_record.write(ownership))
            return m_record.refuse("cannot write and flush staging ownership", errors);
        if (!m_record.finish_write(StagingFile::RetainedAccess::Read, errors) ||
            !FlushRemovalDirectory(m_path, errors))
            return false;
        m_ready = true;
        // Keep the durable stable-identity claim until completion is durable;
        // a torn completion write can never strand a ledger-sized payload.
        if (!m_claim.remove())
            return m_claim.refuse("cannot retire completed staging claim", errors);
        m_claim_created = false;
        // Windows does not retire a delete-pending name until the last handle
        // closes. Hooks and publication must not observe that name as a payload.
        if (!m_claim.close())
            return m_claim.refuse("cannot close retired staging claim", errors);
        if (!FlushRemovalDirectory(m_path, errors))
            return false;
        return true;
    }
    bool valid(std::vector<std::string>* errors) const {
        if (!m_ready)
            return m_record.refuse("staging ownership has not been prepared", errors);
        std::string reason;
        if (!ValidStagingOwnership(m_path, m_payload, m_record, &reason))
            return m_record.refuse(reason.c_str(), errors);
        return true;
    }
    bool occupied() const {
        return m_occupied;
    }
    const std::filesystem::path& path() const {
        return m_path;
    }
    StagingFile& payload() {
        return m_payload;
    }
    void record_payload(CreatedWorldArtifacts& artifacts) {
        // Register only the validated attempt immediately before publication.
        // Staging still owns identity-checked payload/sidecar cleanup on failure;
        // the journal tolerates the absent temporary and owns the published file.
        m_payload.record_artifact(artifacts);
    }
    bool installed_at(const std::filesystem::path& destination, unsigned links = 1) const {
        return m_payload.matches_path(destination, links);
    }
    void published(std::vector<std::string>* errors) {
        // The payload was renamed (including a post-rename flush failure).
        // The caller's live ledger is never retired through this handle.
        m_ready = false;
        m_payload_created = false;
        if (m_record.remove())
            m_record_created = false;
        else
            m_record.refuse("cannot remove staging ownership after publication", errors);
        FlushRemovalDirectory(m_path, errors);
    }

private:
    std::filesystem::path m_path;
    StagingFile m_payload;
    StagingFile m_claim;
    StagingFile m_record;
    bool m_payload_created = false;
    bool m_record_created = false;
    bool m_claim_created = false;
    bool m_ready = false;
    bool m_occupied = false;
};

bool StagingNameHasPrefix(const std::filesystem::path& name, const std::filesystem::path& prefix) {
    const auto& native_name = name.native();
    const auto& native_prefix = prefix.native();
    if (native_name.size() < native_prefix.size())
        return false;
    return std::equal(
        native_prefix.begin(), native_prefix.end(), native_name.begin(), [](auto left, auto right) {
#if defined(_WIN32)
            // Generated names are ASCII. Recognize their case aliases
            // before choosing ownership-aware cleanup on Windows.
            const auto fold = [](auto unit) {
                return unit >= 'A' && unit <= 'Z' ? unit + ('a' - 'A') : unit;
            };
            return fold(left) == fold(right);
#else
                          return left == right;
#endif
        });
}

bool StagingNameEquals(const std::filesystem::path& name, const std::filesystem::path& expected) {
    return name.native().size() == expected.native().size() && StagingNameHasPrefix(name, expected);
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
    const auto prefix = destination.filename().native() + std::filesystem::path(".tmp.").native();
    std::size_t reclaimed = 0;
    std::filesystem::directory_iterator it(directory, list_error);
    if (list_error)
        return 0;
    for (const std::filesystem::directory_iterator end; !list_error && it != end;
         it.increment(list_error)) {
        const auto& entry = *it;
        if (!StagingNameHasPrefix(entry.path().filename(), prefix))
            continue;
        if (StagingNameEquals(destination.filename(), "active-regions.arl")) {
            if (StagingNameEquals(entry.path().extension(), ".claim")) {
                auto staging = entry.path();
                staging.replace_extension();
                if (ReclaimClaimedLedgerStaging(staging))
                    ++reclaimed;
            } else if (StagingNameEquals(entry.path().extension(), ".owner")) {
                auto staging = entry.path();
                staging.replace_extension();
                if (ReclaimOrphanedLedgerStaging(staging))
                    ++reclaimed;
            } else if (ReclaimOwnedLedgerStaging(entry.path()))
                ++reclaimed;
            continue;
        }
        std::error_code kind_error;
        if (!entry.is_regular_file(kind_error) || kind_error)
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
                           std::vector<std::string>* errors,
                           CreatedWorldArtifacts* artifacts = nullptr) {
    file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 128; ++attempt) {
        out_temp = NextRegionTempPath(destination);
        if (artifacts)
            artifacts->reserve_file(out_temp);
        file = ::CreateFileW(out_temp.c_str(),
                             GENERIC_WRITE,
                             artifacts ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : 0,
                             nullptr,
                             CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                             nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            try {
                if (artifacts)
                    artifacts->file_created(out_temp, file);
            } catch (...) {
                ::CloseHandle(file);
                throw;
            }
            break;
        }
        const DWORD error = ::GetLastError();
        if (artifacts)
            artifacts->abandon_file_reservation();
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
                             std::vector<std::string>* errors,
                             CreatedWorldArtifacts* artifacts = nullptr) {
    if (!::FlushFileBuffers(file)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(file);
        RemoveTemporaryFile(out_temp, artifacts);
        AddError(errors,
                 "failed to durably flush temporary region file: " +
                     Filesystem::GenericUtf8PathString(out_temp) + ": " +
                     WindowsErrorMessage(error));
        return false;
    }
    if (!::CloseHandle(file)) {
        const DWORD error = ::GetLastError();
        RemoveTemporaryFile(out_temp, artifacts);
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
                                 std::vector<std::string>* errors,
                                 CreatedWorldArtifacts* artifacts = nullptr,
                                 StagingFile* retained_payload = nullptr) {
    LUMIN_PROFILE_ZONE_N("File save replace");
    if (!FlushAncestorEntries(destination, errors))
        return false;
    const auto publish = LockPublication(destination);
    if (!AllowPublication(destination, false, errors))
        return false;
    if (artifacts) {
        artifacts->publish_file(temp, destination);
        return true;
    }
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
    // ReplaceFileW above permits the retained attributes handle. First install
    // retires a hard-link source, so release its named handle while keeping ID.
    if (retained_payload)
        retained_payload->retain_publication_identity();
    auto file = ::CreateFileW(temp.c_str(),
                              GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        AddError(errors, "cannot bind file installation: " + WindowsErrorMessage(::GetLastError()));
        return false;
    }
    try {
        file =
            Detail::WindowsPublishWithoutReplacement(std::exchange(file, INVALID_HANDLE_VALUE),
                                                     temp,
                                                     destination,
                                                     GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES);
        // Flush the published object's data and metadata through its bound link.
        if (!::FlushFileBuffers(file))
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Cannot flush installed file");
    } catch (const std::exception& error) {
        if (file != INVALID_HANDLE_VALUE)
            ::CloseHandle(file);
        AddError(errors, "failed to atomically install region file: " + std::string(error.what()));
        return false;
    }
    ::CloseHandle(file);
    return true;
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
                           std::vector<std::string>* errors,
                           CreatedWorldArtifacts* artifacts = nullptr) {
    file = -1;
    for (int attempt = 0; attempt < 128; ++attempt) {
        out_temp = NextRegionTempPath(destination);
        if (artifacts)
            artifacts->reserve_file(out_temp);
        file = ::open(out_temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (file >= 0) {
            try {
                if (artifacts)
                    artifacts->file_created(out_temp, file);
            } catch (...) {
                ::close(file);
                throw;
            }
            break;
        }
        const int error = errno;
        if (artifacts)
            artifacts->abandon_file_reservation();
        if (error != EEXIST) {
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
                             std::vector<std::string>* errors,
                             CreatedWorldArtifacts* artifacts = nullptr) {
    while (::fsync(file) != 0) {
        if (errno == EINTR) {
            continue;
        }
        const int error = errno;
        ::close(file);
        RemoveTemporaryFile(out_temp, artifacts);
        AddError(errors,
                 "failed to durably flush temporary region file: " +
                     Filesystem::GenericUtf8PathString(out_temp) + ": " + PosixErrorMessage(error));
        return false;
    }
    if (::close(file) != 0) {
        const int error = errno;
        RemoveTemporaryFile(out_temp, artifacts);
        AddError(errors,
                 "failed to close temporary region file: " +
                     Filesystem::GenericUtf8PathString(out_temp) + ": " + PosixErrorMessage(error));
        return false;
    }
    return true;
}

bool FlushDirectoryEntry(const std::filesystem::path& directory_path,
                         std::vector<std::string>* errors);

bool ReplaceRegionFileAtomically(const std::filesystem::path& temp,
                                 const std::filesystem::path& destination,
                                 std::vector<std::string>* errors,
                                 CreatedWorldArtifacts* artifacts = nullptr,
                                 StagingFile* retained_payload = nullptr) {
    (void)retained_payload; // POSIX open descriptors do not delay source unlink.
    LUMIN_PROFILE_ZONE_N("File save replace");
    if (!FlushAncestorEntries(destination, errors))
        return false;
    // Only publication excludes open readers, matching Windows semantics. Flush
    // both directory entries after releasing that lock when staging is elsewhere.
    {
        const auto publish = LockPublication(destination);
        if (!AllowPublication(destination, false, errors))
            return false;
        if (artifacts) {
            artifacts->publish_file(temp, destination);
        } else if (std::rename(temp.c_str(), destination.c_str()) != 0) {
            const int error = errno;
            AddError(errors,
                     "failed to atomically replace region file: " +
                         Filesystem::GenericUtf8PathString(destination) + ": " +
                         PosixErrorMessage(error));
            return false;
        }
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
    return temp.parent_path() == destination.parent_path() ||
           FlushDirectoryEntry(
               temp.has_parent_path() ? temp.parent_path() : std::filesystem::path("."), errors);
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
                                        std::vector<std::string>* errors,
                                        CreatedWorldArtifacts* artifacts = nullptr) {
    std::filesystem::path staging;
    if (!RegionStagingDestination(destination, staging, errors))
        return false;
    RegionFileHandle file{};
    if (!OpenDurableRegionTemp(staging, file, out_temp, errors, artifacts))
        return false;
    bool produced = false;
    try {
        produced = produce(file);
    } catch (...) {
        CloseRegionFile(file);
        RemoveTemporaryFile(out_temp, artifacts);
        throw;
    }
    if (!produced) {
        CloseRegionFile(file);
        RemoveTemporaryFile(out_temp, artifacts);
        AddError(errors, "failed to produce temporary region contents");
        return false;
    }
    return FinishDurableRegionTemp(file, out_temp, errors, artifacts);
}

bool WriteDurableRegionTemp(const std::filesystem::path& destination,
                            const std::string& bytes,
                            std::filesystem::path& out_temp,
                            std::vector<std::string>* errors,
                            CreatedWorldArtifacts* artifacts = nullptr) {
    return WriteDurableRegionTempWithProducer(
        destination,
        [&](auto file) { return WriteRegionBytes(file, bytes, errors); },
        out_temp,
        errors,
        artifacts);
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
        const auto publish = LockPublication(m_destination);
        if (!AllowPublication(m_destination, false, errors))
            return false;
#if defined(_WIN32)
        // Keep the created destination link bound; rollback targets its handle, never a
        // replacement file found by looking up the destination a second time.
        m_handle = ::CreateFileW(m_temp.c_str(),
                                 GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT,
                                 nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            AddError(errors,
                     "cannot publish an unoccupied destination: " +
                         WindowsErrorMessage(::GetLastError()));
            return false;
        }
        try {
            m_handle = Detail::WindowsPublishWithoutReplacement(
                std::exchange(m_handle, INVALID_HANDLE_VALUE),
                m_temp,
                m_destination,
                GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES);
        } catch (const std::exception& error) {
            AddError(errors,
                     "cannot publish an unoccupied destination: " + std::string(error.what()));
            return false;
        }
        m_published = true;
        if (!::FlushFileBuffers(m_handle)) {
            AddError(errors,
                     "cannot flush published file: " + WindowsErrorMessage(::GetLastError()));
            return false;
        }
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
    if (written <= 0 ||
        static_cast<std::size_t>(written) > SavedInputLimits::kRegionCompressedBytes) {
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
    std::unique_ptr<SavedInputFile> input;
    std::stop_token stop;
    // LMR1's u16 count bounds the manifest to 65,535 entries.
    std::vector<Record> records;
    std::size_t next = 0;
};

bool ReadRegionIndex(const std::filesystem::path& path,
                     RegionIndex& index,
                     std::vector<std::string>* errors,
                     const std::stop_token& stop = {},
                     bool missing_allowed = false) {
    LUMIN_PROFILE_ZONE_N("File save read");
    index = {};
    index.path = path;
    index.input = std::make_unique<SavedInputFile>(path);
    index.stop = stop;
    const auto reject = [&](const char* message) {
        AddError(errors, std::string(message) + ": " + Filesystem::GenericUtf8PathString(path));
        return false;
    };
    if (index.input->missing())
        return missing_allowed || reject("failed to open region file");
    std::uint64_t file_bytes = 0;
    if (!index.input->length(file_bytes))
        return reject("failed to read region file extent");
    if (!SavedInputLimits::RegionFileLengthAllowed(file_bytes))
        return reject(
            "Saved-input admission refused: region file exceeds byte compatibility limit");
    if (file_bytes < kRegionFileHeaderSize ||
        file_bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max()))
        return reject("corrupt region file length");
    std::array<unsigned char, kRegionFileHeaderSize> header{};
    if (!index.input->read_exact(reinterpret_cast<char*>(header.data()), header.size(), stop) ||
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
    std::uint64_t work_bytes = 0, payload_bytes = 0;
    std::set<std::pair<u8, u64>> keys;
    for (auto& record : index.records) {
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
        std::array<unsigned char, kRecordHeaderSize> record_header{};
        if (!index.input->read_exact(
                reinterpret_cast<char*>(record_header.data()), record_header.size(), stop))
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
        if (record.raw_size == 0 || record.raw_size > SavedInputLimits::kRegionRecordBytes ||
            record.compressed_size == 0 ||
            record.compressed_size > SavedInputLimits::kRegionCompressedBytes ||
            record.raw_size > SavedInputLimits::kRegionReadWorkBytes - work_bytes ||
            record.compressed_size >
                SavedInputLimits::kRegionCompressedAggregateBytes - payload_bytes)
            return reject("Saved-input admission refused: region record or aggregate exceeds "
                          "compatibility limit");
        work_bytes += record.raw_size;
        payload_bytes += record.compressed_size;
        record.offset = static_cast<std::streamoff>(offset);
        offset += record.compressed_size;
    }
    return offset == file_bytes || reject("region file has trailing bytes after the last record");
}

bool ReadIndexedRecord(RegionIndex& index,
                       const RegionIndex::Record& entry,
                       RegionRecord& record,
                       std::vector<std::string>* errors) {
    if (entry.compressed_size == 0 ||
        entry.compressed_size > SavedInputLimits::kRegionCompressedBytes || entry.raw_size == 0 ||
        entry.raw_size > kMaxRecordBytes) {
        AddError(errors, "corrupt region record payload length");
        return false;
    }
    record = {entry.id, entry.lod_level, entry.flags, entry.raw_size, {}};
    record.compressed_payload.resize(entry.compressed_size);
    if (!index.input->seek(static_cast<std::uint64_t>(entry.offset)) ||
        !index.input->read_exact(
            record.compressed_payload.data(), entry.compressed_size, index.stop)) {
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
                       std::vector<std::string>* errors,
                       std::filesystem::path* prepared_temp = nullptr) {
    RegionIndex previous;
    if (!ReadRegionIndex(path, previous, errors, {}, true))
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
            std::uint64_t work_bytes = 0;
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
                if (record.uncompressed_size >
                        SavedInputLimits::kRegionWriteWorkBytes - work_bytes ||
                    record.compressed_payload.size() >
                        record.uncompressed_size + record.uncompressed_size / 255u + 16u) {
                    AddError(errors,
                             "region record or aggregate exceeds writer compatibility limit");
                    return false;
                }
                work_bytes += record.uncompressed_size;
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
    previous.input.reset();
    for (const auto& replacement : replacements) {
        if (replacement.source && replacement.source->input)
            replacement.source->input.reset();
    }
    if (!written)
        return false;
    if (prepared_temp) {
        *prepared_temp = std::move(temp);
        return true;
    }
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
    const detail::RegionFileWriteGuard transaction(manifest_path);
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
    if (!ReadRegionIndex(region_file, reader, nullptr, {}, true))
        return false;
    if (reader.records.empty())
        return true;
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

bool SavedInputFile::is_within_save_root() const {
    const auto directory = m_path.parent_path();
    if (directory.filename() != kRegionDirectoryName ||
        directory.parent_path().filename() != kChunksDirectoryName)
        return true;
    if (m_path.native().size() > 32767)
        return false;
    std::error_code error;
    const auto save_root = directory.parent_path().parent_path();
    const auto root = Filesystem::WeaklyCanonicalPath(
        save_root.empty() ? std::filesystem::path(".") : save_root, error);
    if (error || root.native().size() > 32767)
        return false;
#if defined(_WIN32)
    // Final names come from this payload's owning handle, including junctions
    // and short-name aliases. The fixed buffer covers the extended-path bound.
    std::array<wchar_t, 32768> buffer{};
    const DWORD resolved_length = ::GetFinalPathNameByHandleW(
        m_handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    if (resolved_length == 0 || resolved_length >= buffer.size())
        return false;
    std::wstring spelling(buffer.data(), resolved_length);
    if (spelling.starts_with(LR"(\\?\UNC\)"))
        spelling = LR"(\\)" + spelling.substr(8);
    else if (spelling.starts_with(LR"(\\?\)"))
        spelling.erase(0, 4);
    const std::filesystem::path resolved(std::move(spelling));
#else
    struct stat actual {};
    if (::fstat(m_descriptor, &actual) != 0 || !S_ISREG(actual.st_mode))
        return false;
    const auto resolved = std::filesystem::canonical(m_path, error);
    if (error || resolved.native().size() > 32767 || !IsSaveDescendant(resolved, root))
        return false;
    // The canonical spelling alone cannot authorize a read. Open its components
    // without links and compare identities to the original payload descriptor.
    const int checked = OpenResolvedSavedInput(resolved);
    if (checked < 0)
        return false;
    struct stat expected {};
    const bool matches = ::fstat(checked, &expected) == 0 && S_ISREG(expected.st_mode) &&
                         actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino;
    ::close(checked);
    if (!matches)
        return false;
#endif
    return IsSaveDescendant(resolved, root);
}

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
                                     std::vector<std::string>* errors,
                                     CreatedWorldArtifacts* artifacts) {
    const SaveGuard guard;
    return validate_save(save_dir, errors) &&
           save_metadata_validated(bytes, save_dir, errors, artifacts);
}

bool WorldSaveService::save_metadata_validated(const std::string& bytes,
                                               const std::filesystem::path& save_dir,
                                               std::vector<std::string>* errors,
                                               CreatedWorldArtifacts* artifacts) {
    if (!admit_metadata_bytes(bytes, errors))
        return false;
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
    auto metadata = nlohmann::json::parse(bytes, nullptr, false);
    // Legacy metadata is opaque input to this helper. Clock or marker input
    // opts into calendar validation; the existing destination is still validated.
    const bool has_clock = metadata.contains("simulationTick") || metadata.contains("calendar");
    std::optional<Vec3> ambient_anchor;
    const bool has_render_calendar = metadata.contains("renderCalendar");
    if (((has_clock || has_render_calendar) &&
         (!world::WorldClock::from_metadata(metadata, clock, clock_error) ||
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
    const auto destination = save_dir / "world_info.json";
    if (artifacts)
        artifacts->reserve_file(destination);
    else
        ReclaimAbandonedStaging(destination);
    std::filesystem::path temp;
    if (!WriteDurableRegionTemp(destination, output_bytes, temp, errors, artifacts))
        return false;
    if (InterruptBeforeRegionReplaceForTesting().exchange(false)) {
        RemoveTemporaryFile(temp, artifacts);
        AddError(errors, "interrupted before metadata replacement");
        return false;
    }
    if (!ReplaceRegionFileAtomically(temp, destination, errors, artifacts)) {
        RemoveTemporaryFile(temp, artifacts);
        return false;
    }
    return true;
}

bool WorldSaveService::save_metadata_and_active_regions(const std::string& bytes,
                                                        world::ActiveRegionLedger& ledger,
                                                        const std::filesystem::path& save_dir,
                                                        std::vector<std::string>* errors,
                                                        CreatedWorldArtifacts* artifacts) {
    const SaveGuard guard;
    return validate_save(save_dir, errors) &&
           save_pair_validated(bytes, ledger, save_dir, errors, artifacts);
}

bool WorldSaveService::save_pair_validated(const std::string& bytes,
                                           world::ActiveRegionLedger& ledger,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors,
                                           CreatedWorldArtifacts* artifacts) {
    if (!admit_metadata_bytes(bytes, errors))
        return false;
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
    if (!metadata.contains("simulationTick") && !metadata.contains("calendar")) {
        AddError(errors, "Corrupt world metadata: active regions require clock metadata.");
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
        return save_regions_validated(ledger, clock, save_dir, errors, artifacts) &&
               save_metadata_validated(bytes, save_dir, errors, artifacts);
    return save_metadata_validated(bytes, save_dir, errors, artifacts) &&
           save_regions_validated(ledger, clock, save_dir, errors, artifacts);
}

bool WorldSaveService::read_clock_metadata(const std::filesystem::path& save_dir,
                                           world::WorldClock& clock,
                                           bool& requires_active_regions,
                                           std::vector<std::string>* errors,
                                           std::optional<Vec3>* ambient_anchor,
                                           std::optional<world::WorldCalendar>* render_calendar,
                                           std::size_t* water_sim_cursor,
                                           const std::stop_token& stop) {
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
        if (!ReadSavedJson(path,
                           SavedInputLimits::kMetadataBytes,
                           metadata,
                           missing,
                           true,
                           &json_failure,
                           stop) ||
            (!missing && !AdmitMetadata(metadata))) {
            AddError(
                errors,
                json_failure == SavedJsonFailure::Cancelled ? "Saved-world validation cancelled."
                : json_failure == SavedJsonFailure::Malformed ||
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
            !ReadAmbientAnchor(metadata, anchor, error) ||
            !ReadWaterCursor(metadata, water_sim_cursor, error)) {
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
    if (!luminumbra::foliage::ValidatePlantEntitySnapshot(snapshot, false)) {
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
    PreparedPlantWrite prepared;
    return prepared.prepare(snapshot, errors) && prepared.publish(save_dir, errors);
}

bool WorldSaveService::save_plants_validated(const Ecs::EntityRegistrySnapshot& snapshot,
                                             const std::filesystem::path& save_dir,
                                             std::vector<std::string>* errors) {
    PreparedPlantWrite prepared;
    return prepared.prepare(snapshot, errors) && prepared.publish(save_dir, errors, false);
}

bool WorldSaveService::PreparedPlantWrite::prepare(const Ecs::EntityRegistrySnapshot& snapshot,
                                                   std::vector<std::string>* errors) {
    m_prepared = false;
    m_bytes.clear();
    if (!prepare_plant_entities(snapshot, m_bytes, errors))
        return false;
    m_empty = snapshot.entities.empty();
    m_prepared = true;
    return true;
}

bool WorldSaveService::PreparedPlantWrite::publish(const std::filesystem::path& save_dir,
                                                   std::vector<std::string>* errors,
                                                   bool validate_existing) const {
    const SaveGuard guard;
    LUMIN_PROFILE_ZONE_N("File plant entities save");
    if (!m_prepared) {
        AddError(errors, "plant write has not been prepared");
        return false;
    }
    if (validate_existing && !validate_save(save_dir, errors))
        return false;
    const std::filesystem::path path = plant_entities_path(save_dir);
    const detail::RegionFileWriteGuard transaction(path);
    // Like ledger staging, abandoned plant copies must live outside the strict
    // region scan. Reclaim them even when the new roster is empty.
    const auto staging = save_dir / path.filename();
    ReclaimAbandonedStaging(staging);
    std::error_code ec;
    if (m_empty) {
        // No plants -> no file (a no-plant save is byte-identical). Retire any stale file.
        {
            const auto publish = LockPublication(path);
            if (!AllowPublication(path, true, errors)) {
                AddError(errors,
                         "failed to remove plant entities file: " +
                             Filesystem::GenericUtf8PathString(path));
                return false;
            }
            std::filesystem::remove(path, ec);
        }
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
    return WriteFileDurably(path, m_bytes, staging, errors);
}

bool WorldSaveService::load_plant_entities(Luminumbra::Ecs::EntityRegistrySnapshot& out,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors,
                                           const std::stop_token& stop) {
    LUMIN_PROFILE_ZONE_N("File plant entities load");
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    const std::filesystem::path path = plant_entities_path(save_dir);
    const SavedInputFile input(path);
    if (input.missing()) {
        out = Luminumbra::Ecs::EntityRegistrySnapshot{};
        return true; // clean miss: a fresh / no-plant world
    }
    std::string bytes;
    if (!input.read(bytes, SavedInputLimits::kPlantFileBytes, stop)) {
        AddError(
            errors,
            stop.stop_requested()
                ? "Saved-world validation cancelled."
                : "Saved-input admission refused: unreadable, non-regular or over 256 MiB plant "
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
    if (!luminumbra::foliage::ValidatePlantEntitySnapshot(candidate, false)) {
        AddError(errors, "Saved-input admission refused: invalid plant component value.");
        return false;
    }
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
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
                                           const world::WorldClock& absent_clock,
                                           const std::stop_token& stop) {
    LUMIN_PROFILE_ZONE_N("File active regions load");
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    try {
        const auto path = active_regions_path(save_dir);
        const SavedInputFile input(path);
        if (input.missing()) {
            ledger = world::ActiveRegionLedger(ledger.config(), absent_clock);
            return true;
        }
        std::string bytes;
        if (!input.read(bytes, world::ActiveRegionLedger::kMaxFileBytes, stop)) {
            AddError(errors,
                     stop.stop_requested() ? "Saved-world validation cancelled."
                                           : world::ActiveRegionLedger::kCorruptMessage);
            return false;
        }
        std::string error;
        world::ActiveRegionLedger candidate(ledger.config(), absent_clock);
        if (!world::ActiveRegionLedger::decode(bytes, candidate, error)) {
            AddError(errors, error.empty() ? world::ActiveRegionLedger::kCorruptMessage : error);
            return false;
        }
        if (stop.stop_requested()) {
            AddError(errors, "Saved-world validation cancelled.");
            return false;
        }
        ledger = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        AddError(errors,
                 stop.stop_requested() ? "Saved-world validation cancelled."
                                       : world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
}

bool WorldSaveService::save_active_regions(world::ActiveRegionLedger& ledger,
                                           const world::WorldClock& clock,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors,
                                           CreatedWorldArtifacts* artifacts) {
    const SaveGuard guard;
    return validate_save(save_dir, errors) &&
           save_regions_validated(ledger, clock, save_dir, errors, artifacts);
}

bool WorldSaveService::save_regions_validated(world::ActiveRegionLedger& ledger,
                                              const world::WorldClock& clock,
                                              const std::filesystem::path& save_dir,
                                              std::vector<std::string>* errors,
                                              CreatedWorldArtifacts* artifacts) {
    // Empty ledgers have no last_save stamps to validate against the ceiling.
    // Require the acknowledged clock before either writing or taking the no-op path.
    if (ledger.header_tick() != clock.tick()) {
        AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
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
    if (!artifacts)
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
        if (artifacts) {
            artifacts->create_directory(path.parent_path().parent_path());
            artifacts->create_directory(path.parent_path());
            artifacts->reserve_file(path);
        }
        // Exclusive creation skips unknown payload or sidecar occupants. The
        // bounded retry also handles a substituted file observed before use.
        bool hook_called = false;
        bool replaced = false;
        for (int attempt = 0; attempt < 128; ++attempt) {
            LedgerStaging temporary(staging);
            if (!temporary.available(errors))
                continue;
            if (!temporary.prepare(bytes, errors)) {
                if (temporary.occupied())
                    continue;
                AddError(errors, "Cannot durably record active-region staging ownership");
                return false;
            }
            if (!hook_called) {
                hook_called = true;
                if (const auto hook = BeforeActiveRegionsReplaceForTesting(); hook && !hook()) {
                    AddError(errors, "interrupted before active-region ledger replacement");
                    return false;
                }
            }
            if (!temporary.valid(errors))
                continue;
            if (!artifacts && !CreatePayloadDirectory(path, errors))
                return false;
            if (artifacts)
                temporary.record_payload(*artifacts);
            try {
                replaced = ReplaceRegionFileAtomically(
                    temporary.path(), path, errors, artifacts, &temporary.payload());
            } catch (...) {
                // Windows recovery can retain both verified publication links.
                // The journal owns those links; retire the scoped sidecar too.
                if (artifacts && temporary.installed_at(path, 2))
                    temporary.published(errors);
                throw;
            }
            // A failed final directory flush can leave the complete new ledger
            // installed. Retiring the sidecar never removes that live file.
            if (temporary.installed_at(path))
                temporary.published(errors);
            if (!replaced)
                return false;
            break;
        }
        if (!replaced) {
            AddError(errors, "Cannot allocate unique active-region staging");
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
        bool missing = false;
        nlohmann::json existing;
        if (!ReadSavedJson(path, SavedInputLimits::kMetadataBytes, existing, missing, true) ||
            (!missing && !AdmitMetadata(existing))) {
            AddError(errors, "Saved-input admission refused: invalid water scheduler metadata.");
            m_valid = false;
            return false;
        }
        if (!missing) {
            metadata = std::move(existing);
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
    RegionIndex index;
    if (!ReadRegionIndex(region_file, index, errors, {}, true)) {
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
    RegionIndex index;
    if (!ReadRegionIndex(region_file, index, errors, {}, true)) {
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
    RegionIndex index;
    if (!ReadRegionIndex(region_file, index, errors, {}, true)) {
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
    return upsert_container_records(guard, region_file, records, errors);
}

bool WorldSaveService::upsert_container_records(const SaveOwnership&,
                                                const std::filesystem::path& region_file,
                                                const std::vector<ContainerRecord>& records,
                                                std::vector<std::string>* errors) {
    const auto parent = region_file.parent_path();
    if (parent.filename() == kRegionDirectoryName &&
        parent.parent_path().filename() == kChunksDirectoryName) {
        world::WorldClock clock;
        bool required = false;
        if (!read_clock_metadata(parent.parent_path().parent_path(), clock, required, errors))
            return false;
    }
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
                      FileIdentity* published_identity,
                      CreatedWorldArtifacts* artifacts) {
    // Callers hold SaveGuard. Stage in an existing ancestor before creating
    // payload directories, so a refused payload leaves the hierarchy unchanged.
    if (artifacts)
        artifacts->reserve_file(destination);
    std::filesystem::path temp_path;
    if (!WriteDurableRegionTemp(staging, bytes, temp_path, errors, artifacts))
        return false;
    // Capture the exclusive staging file's identity before publication. Alias
    // rollback later checks this identity at replacement under SaveGuard.
    if (published_identity && !ReadFileIdentity(temp_path, *published_identity)) {
        RemoveTemporaryFile(temp_path, artifacts);
        AddError(errors, "cannot retain replacement identity");
        return false;
    }
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel)) {
        RemoveTemporaryFile(temp_path, artifacts);
        AddError(errors,
                 "file replacement interrupted by test seam before replacing: " +
                     Filesystem::GenericUtf8PathString(destination));
        return false;
    }
    if (!CreatePayloadDirectory(destination, errors)) {
        RemoveTemporaryFile(temp_path, artifacts);
        return false;
    }
    if (!ReplaceRegionFileAtomically(temp_path, destination, errors, artifacts)) {
        RemoveTemporaryFile(temp_path, artifacts);
        return false;
    }
    return true;
}
} // namespace

bool WorldSaveService::write_file_durably(const std::filesystem::path& destination,
                                          const std::string& bytes,
                                          std::vector<std::string>* errors,
                                          CreatedWorldArtifacts* artifacts) {
    const SaveGuard guard;
    if (!artifacts)
        ReclaimAbandonedStaging(destination);
    return WriteFileDurably(destination, bytes, destination, errors, nullptr, artifacts);
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
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel)) {
        AddError(errors, "interrupted before exclusive file publication");
        return false;
    }
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
        const SavedInputFile input(source, SavedInputFile::LeafSymlinks::Follow);
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
    if (error) {
        AddError(errors, "cannot create rename publication directory: " + error.message());
        return false;
    }
    if (!WriteDurableRegionTemp(destination, bytes, temp, errors))
        return false;
    NewFilePublication publication(temp, destination);
    if (InterruptBeforeRegionReplaceForTesting().exchange(false, std::memory_order_acq_rel)) {
        AddError(errors, "interrupted before rename publication");
        return false;
    }
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
    PreparedWorldWrite prepared;
    return prepared.prepare(state, save_dir, false, errors, preserve_authority) &&
           prepared.publish(errors);
}

bool WorldSaveService::load_world(WorldStreamingState& state,
                                  const std::filesystem::path& save_dir,
                                  std::vector<std::string>& errors,
                                  const std::stop_token& stop,
                                  std::size_t max_resident_chunks,
                                  bool require_full_sdf_lattice,
                                  bool migrate_legacy_water) const {
    LUMIN_PROFILE_ZONE_N("File world load");
    // Staging lives outside the strict region inventory. Each published file
    // is read independently; validation never holds a world-wide transaction.
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
                           &json_failure,
                           stop) ||
            (!metadata_missing && !AdmitMetadata(metadata)))
            return reject(
                json_failure == SavedJsonFailure::Cancelled ? "Saved-world validation cancelled."
                : json_failure == SavedJsonFailure::Malformed ||
                        json_failure == SavedJsonFailure::Unreadable
                    ? (json_failure == SavedJsonFailure::Malformed
                           ? "corrupt world metadata: invalid JSON"
                           : "corrupt world metadata: unreadable or non-regular input")
                    : "Saved-input admission refused: unreadable or oversized world metadata.");
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
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
        const auto chunks_status = InspectPersistenceDirectory(chunks_dir, save_dir);
        if (chunks_status == PersistenceDirectory::Missing)
            return false;
        if (chunks_status == PersistenceDirectory::Invalid)
            return reject("corrupt world persistence directory: " +
                          Filesystem::GenericUtf8PathString(chunks_dir));
        for (const auto& entry : std::filesystem::directory_iterator(chunks_dir)) {
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            if (entry.path() != region_dir || InspectPersistenceDirectory(entry.path(), save_dir) !=
                                                  PersistenceDirectory::Present)
                return reject("corrupt or unknown world persistence artifact: " +
                              Filesystem::GenericUtf8PathString(entry.path()));
        }
        const auto region_status = InspectPersistenceDirectory(region_dir, save_dir);
        if (region_status == PersistenceDirectory::Missing)
            return false;
        if (region_status == PersistenceDirectory::Invalid)
            return reject("corrupt world persistence directory: " +
                          Filesystem::GenericUtf8PathString(region_dir));
        std::vector<std::filesystem::path> region_files;
        bool present = false;
        for (const auto& entry : std::filesystem::directory_iterator(region_dir)) {
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            int rx = 0, rz = 0;
            const std::shared_lock read(detail::RegionFileSyncFor(entry.path()).publication);
            // A plant file can be retired after iterator discovery. Inspect it
            // under its publication lock and accept that clean disappearance.
            // Inspect the leaf itself: a dangling link is present and invalid,
            // never the clean disappearance of a retired plant file. Payload
            // readers independently open once and verify that same handle.
            const auto status = std::filesystem::symlink_status(entry.path());
            if (!std::filesystem::exists(status) && entry.path() == plant_entities_path(save_dir))
                continue;
            if (!std::filesystem::is_regular_file(status))
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
                           &json_failure,
                           stop))
            return reject(
                json_failure == SavedJsonFailure::Cancelled ? "Saved-world validation cancelled."
                : json_failure == SavedJsonFailure::Malformed ||
                        json_failure == SavedJsonFailure::Unreadable
                    ? (json_failure == SavedJsonFailure::Malformed
                           ? "corrupt world manifest: invalid JSON"
                           : "corrupt world manifest: unreadable or non-regular input")
                    : "Saved-input admission refused: unreadable or oversized world manifest.");
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
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
        {
            const std::shared_lock pair(ClockPairPublicationMutex());
            world::ActiveRegionLedger ledger;
            if (!load_active_regions(ledger,
                                     save_dir,
                                     &errors,
                                     world::WorldClock{0, world::WorldCalendar::Legacy()},
                                     stop))
                return reject("");
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            if (std::filesystem::exists(active_regions_path(save_dir))) {
                world::WorldClock clock;
                bool required = false;
                if (!read_clock_metadata(
                        save_dir, clock, required, &errors, nullptr, nullptr, nullptr, stop))
                    return reject("");
                if (stop.stop_requested())
                    return reject("Saved-world validation cancelled.");
                if (ledger.header_tick() > clock.tick())
                    return reject(world::ActiveRegionLedger::kCorruptMessage);
            }
        }
        Ecs::EntityRegistrySnapshot plants;
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
        if (!load_plant_entities(plants, save_dir, &errors, stop))
            return reject("");
        // The last payload reader has closed. Cancellation at that boundary
        // must discard the staged chunks just like cancellation during a read.
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
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

    PreparedWorldWrite prepared;
    if (!prepared.prepare(state, save_dir, true, errors, preserve_authority, false) ||
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

bool WorldSaveService::write_snapshot(const std::vector<std::shared_ptr<Chunk>>& chunks,
                                      const std::filesystem::path& save_dir,
                                      std::vector<std::string>* errors,
                                      std::size_t* regions_written,
                                      bool preserve_authority) const {
    const SaveGuard guard;
    WorldStreamingState state;
    for (const auto& chunk : chunks)
        state.insert_chunk(chunk);
    PreparedWorldWrite prepared;
    return prepared.prepare(state, save_dir, false, errors, preserve_authority, false) &&
           prepared.publish(errors, regions_written);
}

struct WorldSaveService::PreparedWorldWrite::Data {
    SaveOwnership ownership;
    std::filesystem::path save_dir;
    std::map<std::filesystem::path, std::filesystem::path> regions;
    ~Data() {
        for (const auto& [path, temp] : regions)
            RemoveTemporaryFile(temp);
    }
    bool write_manifest = false;
};

WorldSaveService::PreparedWorldWrite::PreparedWorldWrite() = default;
WorldSaveService::PreparedWorldWrite::~PreparedWorldWrite() = default;

bool WorldSaveService::PreparedWorldWrite::prepare(const WorldStreamingState& state,
                                                   const std::filesystem::path& save_dir,
                                                   bool dirty_only,
                                                   std::vector<std::string>* errors,
                                                   bool preserve_authority,
                                                   bool validate_existing) {
    m_data.reset();
    auto candidate = std::make_unique<Data>();
    candidate->save_dir = save_dir;
    try {
        if (validate_existing && !validate_save(save_dir, errors))
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
            const detail::RegionFileWriteGuard transaction(path);

            std::vector<RegionReplacement> replacements;
            replacements.reserve(region_chunks.size());
            for (const auto& chunk : region_chunks)
                replacements.push_back({.id = chunk->get_id(),
                                        .lod_level = 0,
                                        .preserve_authority = preserve_authority,
                                        .chunk = chunk});
            auto& temp = candidate->regions[path];
            if (!RewriteRegionFile(path, replacements, errors, &temp))
                return false;
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
    if (!prepared) {
        AddError(errors, "world write has not been prepared or was already consumed");
        return false;
    }
    if (regions_written)
        *regions_written = 0;
    if (!prepared->write_manifest)
        return true;
    try {
        for (auto& [path, temp] : prepared->regions) {
            if (InterruptBeforeRegionReplaceForTesting().exchange(false,
                                                                  std::memory_order_acq_rel)) {
                AddError(errors, "region replacement interrupted by test seam before replacing");
                return false;
            }
            if (!CreatePayloadDirectory(path, errors) ||
                !ReplaceRegionFileAtomically(temp, path, errors))
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
