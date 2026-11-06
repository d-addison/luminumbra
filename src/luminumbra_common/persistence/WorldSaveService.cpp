#include "WorldSaveService.h"
#include "CreatedWorldArtifacts.h"
#include "RegionFileWriteGuard.h"
#include "core/Profiler.h"

#include "../ecs/EntitySnapshot.h" //  plant entity snapshot persistence
#include "../fields/EnergyFieldState.h"
#include "PlantPersistence.h"
#include "SaveTransaction.h"
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
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
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
#include <unordered_map>
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
        if (const auto observer =
                detail::ReadObserverForTesting().load(std::memory_order_acquire)) {
            invalidate_snapshot_observations();
            observer(m_path, true);
        }
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
        if (const auto observer =
                detail::ReadObserverForTesting().load(std::memory_order_acquire)) {
            invalidate_snapshot_observations();
            observer(m_path, false);
        }
    }
}

std::shared_ptr<const void> SavedInputFile::retain_identity() const {
    std::array<std::uint64_t, 3> checked{};
    if (!identity(checked))
        return {};
#if defined(_WIN32)
    const auto retained = CreatedWorldArtifacts::retain_file_identity(m_handle);
    return std::shared_ptr<const void>(new HANDLE(retained), [](const void* pointer) {
        const auto* handle = static_cast<const HANDLE*>(pointer);
        ::CloseHandle(*handle);
        delete handle;
    });
#else
    const auto retained = ::fcntl(m_descriptor, F_DUPFD_CLOEXEC, 0);
    if (retained < 0)
        return {};
    return std::shared_ptr<const void>(new int(retained), [](const void* pointer) {
        const auto* descriptor = static_cast<const int*>(pointer);
        ::close(*descriptor);
        delete descriptor;
    });
#endif
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
constexpr const char* kIndexedWorldManifestSchema = "luminumbra.persistence.world_manifest.v2";
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

// A ledger that grew past the file cap is a save refusal, not an exception from the writer.
bool EncodeActiveRegionsForSave(const world::ActiveRegionLedger& ledger,
                                std::string& bytes,
                                std::vector<std::string>* errors) {
    try {
        bytes = ledger.encode();
    } catch (const std::length_error& capacity) {
        AddError(errors, capacity.what());
        return false;
    }
    return true;
}

bool AllowPublication(const std::filesystem::path& path,
                      bool removing,
                      std::vector<std::string>* errors) {
    if (const auto hook = detail::PublicationHookForTesting().load(std::memory_order_acquire)) {
        invalidate_snapshot_observations();
        if (!hook(path, removing)) {
            AddError(errors,
                     "file publication refused: " + Filesystem::GenericUtf8PathString(path));
            return false;
        }
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
                                 FILE_FLAG_OPEN_REPARSE_POINT |
                                     (mode == Mode::Create ? FILE_FLAG_WRITE_THROUGH : 0) |
                                     FILE_FLAG_BACKUP_SEMANTICS,
                                 nullptr);
#else
        const int flags = mode == Mode::Create ? O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC
                          : mode == Mode::Directory
                              ? O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
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

    bool version(SavedInputFile::Version& result) const {
#if defined(_WIN32)
        return SavedInputFile::native_version(m_handle, result);
#else
        return SavedInputFile::native_version(m_descriptor, result);
#endif
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

    bool remove(unsigned links = 1, const BoundPublicationStage* stage = nullptr) {
        StagingIdentity held{};
        if (!identity(held, false, links) || !matches_name(links))
            return false;
#if defined(_WIN32)
        (void)stage;
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
        const bool removed =
            ::GetFileInformationByHandle(retiring, &info) &&
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ==
                0 &&
            info.nNumberOfLinks > 0 && info.nNumberOfLinks <= links &&
            info.dwVolumeSerialNumber == held[0] && info.nFileIndexHigh == held[1] &&
            info.nFileIndexLow == held[2] && Detail::WindowsDisposeLink(retiring);
        // Keep retiring open while releasing the staged references, so identity
        // cannot be recycled between the removal and journal retirement.
        const bool finished = removed && finish_removal();
        const bool closed = ::CloseHandle(retiring) != 0;
        return finished && closed;
#else
        try {
            CreatedWorldArtifacts::remove_bound_entry(m_path, m_descriptor, false, stage);
            return finish_removal();
        } catch (const std::exception&) {
            return false;
        }
#endif
    }

    bool publish_bound(const std::filesystem::path& destination,
                       const BoundPublicationStage* stage = nullptr) {
        try {
#if defined(_WIN32)
            // Exclusive publication reopens the named source without delete sharing.
            // Keep only an attributes-only, open-by-ID handle across that reopen.
            retain_publication_identity();
            CreatedWorldArtifacts::publish_bound_file(m_path, destination, m_handle, stage);
#else
            CreatedWorldArtifacts::publish_bound_file(m_path, destination, m_descriptor, stage);
#endif
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool remove_bound(bool directory = false, const BoundPublicationStage* stage = nullptr) {
        try {
#if defined(_WIN32)
            CreatedWorldArtifacts::remove_bound_entry(m_path, m_handle, directory, stage);
#else
            CreatedWorldArtifacts::remove_bound_entry(m_path, m_descriptor, directory, stage);
#endif
            return close();
        } catch (const std::exception&) {
            return false;
        }
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
    for (const char byte : bytes) {
        checksum ^= static_cast<unsigned char>(byte);
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
        StagingFile abandoned(entry.path(), StagingFile::Mode::Inspect);
        StagingIdentity identity{};
        if (abandoned.identity(identity) && abandoned.remove_bound())
            ++reclaimed;
    }
    return reclaimed;
}

bool PublishProtectedMember(const std::filesystem::path& temp,
                            const std::filesystem::path& destination,
                            const std::array<std::uint64_t, 3>& expected,
                            const std::array<std::uint64_t, 3>& incoming,
                            std::vector<std::string>* errors,
                            const BoundPublicationStage* source_stage = nullptr,
                            const BoundPublicationStage* retired_stage = nullptr) {
    BoundPublicationStage source_slot, retired_slot;
    if (!source_stage &&
        SaveTransaction::publication_stages(destination, source_slot, retired_slot)) {
        source_stage = &source_slot;
        retired_stage = &retired_slot;
    }
    StagingFile source(temp, StagingFile::Mode::Inspect);
    StagingIdentity source_identity{};
    if (!source.identity(source_identity) ||
        !std::equal(incoming.begin(), incoming.end(), source_identity.begin())) {
        AddError(errors, "snapshot incoming publication ownership changed");
        return false;
    }
    if (expected != std::array<std::uint64_t, 3>{}) {
        StagingFile current(destination, StagingFile::Mode::Inspect);
        StagingIdentity actual{};
        if (!current.identity(actual) ||
            !std::equal(expected.begin(), expected.end(), actual.begin()) ||
            !current.remove_bound(false, retired_stage)) {
            AddError(errors, "snapshot publication occupant changed");
            return false;
        }
        report_snapshot_step("after-retire", destination);
    }
    if (!source.publish_bound(destination, source_stage)) {
        AddError(errors, "cannot exclusively publish protected snapshot member");
        return false;
    }
    return !source_stage ||
           WorldSaveService::durable_flush_directory(source_stage->directory, errors);
}

#if defined(_WIN32)

std::string WindowsErrorMessage(DWORD error) {
    return std::error_code(static_cast<int>(error), std::system_category()).message();
}

bool WriteDurableRegionTemp(const std::filesystem::path& destination,
                            const std::string& bytes,
                            std::filesystem::path& out_temp,
                            std::vector<std::string>* errors,
                            CreatedWorldArtifacts* artifacts = nullptr,
                            WorldSaveService::ArtifactIdentity* identity = nullptr,
                            SavedInputFile::Version* written_version = nullptr) {
    if (written_version)
        *written_version = {};
    LUMIN_PROFILE_ZONE_N("File save write");
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 128; ++attempt) {
        out_temp = NextRegionTempPath(destination);
        if (artifacts)
            artifacts->reserve_file(out_temp);
        file = ::CreateFileW(out_temp.c_str(),
                             GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                             artifacts ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : 0,
                             nullptr,
                             CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                             nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            try {
                if (artifacts)
                    artifacts->file_created(out_temp, file);
                report_snapshot_step("crash-staging-created", out_temp);
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
            RemoveTemporaryFile(out_temp, artifacts);
            AddError(errors,
                     "failed to write temporary region file: " +
                         Filesystem::GenericUtf8PathString(out_temp) + ": " +
                         WindowsErrorMessage(error));
            return false;
        }
        offset += written;
        try {
            report_snapshot_step("crash-staging-written", out_temp);
        } catch (...) {
            ::CloseHandle(file);
            throw;
        }
    }

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
    if (identity) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(file, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) !=
                0 ||
            (info.nFileIndexHigh == 0 && info.nFileIndexLow == 0)) {
            ::CloseHandle(file);
            AddError(errors, "cannot identify created snapshot staging file");
            return false;
        }
        *identity = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
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
    if (!AllowPublication(destination, false, errors) ||
        !SaveTransaction::check_publication_target(destination, errors))
        return false;
    if (artifacts) {
        artifacts->publish_file(temp, destination);
        return true;
    }
    std::array<std::uint64_t, 3> expected{}, incoming{};
    if (SaveTransaction::publication_identity(destination, expected, incoming)) {
        if (retained_payload)
            retained_payload->retain_publication_identity();
        return PublishProtectedMember(temp, destination, expected, incoming, errors);
    }
    const DWORD attributes = ::GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (retained_payload)
            retained_payload->retain_publication_identity();
        // Payload and transaction undo bytes are flushed before publication.
        // ReplaceFileW does not support write-through; use the supported same-volume move.
        if (::MoveFileExW(temp.c_str(),
                          destination.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
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
    // First install retires a hard-link source, so release its named handle
    // while keeping the retained publication identity.
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

bool WriteDurableRegionTemp(const std::filesystem::path& destination,
                            const std::string& bytes,
                            std::filesystem::path& out_temp,
                            std::vector<std::string>* errors,
                            CreatedWorldArtifacts* artifacts = nullptr,
                            WorldSaveService::ArtifactIdentity* identity = nullptr,
                            SavedInputFile::Version* written_version = nullptr) {
    if (written_version)
        *written_version = {};
    LUMIN_PROFILE_ZONE_N("File save write");
    int file = -1;
    for (int attempt = 0; attempt < 128; ++attempt) {
        out_temp = NextRegionTempPath(destination);
        if (artifacts)
            artifacts->reserve_file(out_temp);
        file = ::open(out_temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (file >= 0) {
            try {
                if (artifacts)
                    artifacts->file_created(out_temp, file);
                report_snapshot_step("crash-staging-created", out_temp);
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
            RemoveTemporaryFile(out_temp, artifacts);
            AddError(errors,
                     "failed to write temporary region file: " +
                         Filesystem::GenericUtf8PathString(out_temp) + ": " +
                         PosixErrorMessage(error));
            return false;
        }
        offset += static_cast<std::size_t>(written);
        if (written_version && offset == bytes.size() &&
            !SavedInputFile::native_version(file, *written_version))
            *written_version = {};
        try {
            report_snapshot_step("crash-staging-written", out_temp);
        } catch (...) {
            ::close(file);
            throw;
        }
    }

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
    if (identity) {
        struct stat info {};
        if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_ino == 0) {
            ::close(file);
            AddError(errors, "cannot identify created snapshot staging file");
            return false;
        }
        *identity = {
            static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino), 0};
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
        if (!AllowPublication(destination, false, errors) ||
            !SaveTransaction::check_publication_target(destination, errors))
            return false;
        if (artifacts) {
            artifacts->publish_file(temp, destination);
        } else {
            std::array<std::uint64_t, 3> expected{}, incoming{};
            const bool protected_member =
                SaveTransaction::publication_identity(destination, expected, incoming);
            if (protected_member &&
                !PublishProtectedMember(temp, destination, expected, incoming, errors))
                return false;
            if (!protected_member && std::rename(temp.c_str(), destination.c_str()) != 0) {
                const int error = errno;
                AddError(errors,
                         "failed to atomically replace region file: " +
                             Filesystem::GenericUtf8PathString(destination) + ": " +
                             PosixErrorMessage(error));
                return false;
            }
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

std::atomic<std::uint64_t>& PublicationRevision() {
    static std::atomic<std::uint64_t> revision{0};
    return revision;
}

WorldSaveService::SaveFactsScope*& ActiveFacts() {
    static thread_local WorldSaveService::SaveFactsScope* active = nullptr;
    return active;
}

struct WrittenRegionDigest {
    MemberDigest digest;
    SavedInputFile::Version version{};
    std::uint64_t observation_revision = 0;
};

// Every member replacement funnels through here so tests can observe real publication sites.
bool ReplaceMember(const std::filesystem::path& temp,
                   const std::filesystem::path& destination,
                   std::vector<std::string>* errors,
                   CreatedWorldArtifacts* artifacts = nullptr,
                   StagingFile* retained_payload = nullptr,
                   const WrittenRegionDigest* written = nullptr) {
    if (!SaveTransaction::prepare_publication(destination, temp, errors))
        return false;
    report_snapshot_step("before-replace", destination);
    bool unchanged_output = false;
    if (written) {
        const SavedInputFile input(temp);
        SavedInputFile::Version version{};
        unchanged_output = input.version(version) && version == written->version;
    }
    PublicationRevision().fetch_add(1, std::memory_order_relaxed);
    if (ActiveFacts() != nullptr)
        ActiveFacts()->invalidate_presence_facts();
    if (!ReplaceRegionFileAtomically(temp, destination, errors, artifacts, retained_payload))
        return false;
    if (unchanged_output) {
        const SavedInputFile input(destination);
        SavedInputFile::Version version{};
        // Publication changes ctime. Identity, length and nanosecond mtime must
        // still match the write handle; future reuse compares the complete stamp.
        if (input.version(version) &&
            snapshot_observation_revision() == written->observation_revision &&
            std::equal(version.begin(), version.begin() + 6, written->version.begin())) {
            const auto root = destination.parent_path().parent_path().parent_path();
            if (auto* transaction = SaveTransaction::active_for(root))
                transaction->remember_written_digest(destination, written->digest, version);
        }
    }
    report_snapshot_step("after-replace", destination);
    return true;
}
class AncestorFlushBatch {
public:
    explicit AncestorFlushBatch(std::filesystem::path root)
        : m_root(std::move(root))
        , m_previous(active()) {
        active() = this;
    }
    ~AncestorFlushBatch() {
        active() = m_previous;
    }
    static AncestorFlushBatch*& active() {
        static thread_local AncestorFlushBatch* batch = nullptr;
        return batch;
    }
    bool contains(const std::filesystem::path& directory) const {
        return m_flushed.contains(directory);
    }
    void remember(const std::filesystem::path& directory) {
        // Save-root staging and journal entries change throughout publication.
        // Only their already-established ancestors can share a completed flush.
        if (directory != m_root && IsSaveDescendant(m_root, directory))
            m_flushed.insert(directory);
    }

private:
    std::filesystem::path m_root;
    AncestorFlushBatch* m_previous;
    std::set<std::filesystem::path> m_flushed;
};

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
        auto* batch = AncestorFlushBatch::active();
        if (batch && batch->contains(holder))
            continue;
        if (!FlushDirectoryEntry(holder, errors))
            return false;
        if (batch)
            batch->remember(holder);
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
        if (m_published && !m_committed)
            PublicationRevision().fetch_add(1, std::memory_order_relaxed);
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
            try {
                CreatedWorldArtifacts::remove_bound_entry(m_destination, m_descriptor, false);
                FlushRemovalDirectory(m_destination, nullptr);
            } catch (const std::exception&) {}
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
        PublicationRevision().fetch_add(1, std::memory_order_relaxed);
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
        try {
            CreatedWorldArtifacts::publish_bound_file(m_temp, m_destination, m_descriptor);
        } catch (const std::exception& error) {
            AddError(errors,
                     "cannot publish an unoccupied destination: " + std::string(error.what()));
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

std::atomic<std::uint64_t>& RecordCompressionCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

bool CompressPayload(const std::string& payload,
                     RegionRecord& record,
                     std::vector<std::string>* errors) {
    if (payload.empty() || payload.size() > SavedInputLimits::kRegionRecordBytes) {
        AddError(errors, "region record payload exceeds the 64 MiB compatibility limit");
        return false;
    }
    RecordCompressionCounter().fetch_add(1, std::memory_order_relaxed);
    const int bound = LZ4_compressBound(static_cast<int>(payload.size()));
    std::string compressed(static_cast<std::size_t>(bound), '\0');
    const int written = LZ4_compress_default(
        payload.data(), compressed.data(), static_cast<int>(payload.size()), bound);
    if (written <= 0) {
        AddError(errors, "LZ4 compression of a region record failed");
        return false;
    }
    record.uncompressed_size = static_cast<std::uint32_t>(payload.size());
    // Region batches retain many records. Keep only the compressed bytes, rather than retaining
    // LZ4's allocation for the entire uncompressed JSON payload for every chunk in the batch.
    record.compressed_payload.assign(compressed.data(), static_cast<std::size_t>(written));
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

std::atomic<std::uint64_t>& RegionReadCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

bool ReadRegionFile(const std::filesystem::path& path,
                    std::vector<RegionRecord>& out_records,
                    std::vector<std::string>* errors,
                    const std::stop_token& stop = {},
                    bool missing_allowed = false) {
    LUMIN_PROFILE_ZONE_N("File save read");
    RegionReadCounter().fetch_add(1, std::memory_order_relaxed);
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    const SavedInputFile input(path);
    if (missing_allowed && input.missing()) {
        out_records.clear();
        return true;
    }
    const auto refuse = [errors, &path, &stop](const std::string& reason) {
        AddError(errors,
                 stop.stop_requested() ? "Saved-world validation cancelled."
                                       : reason + ": " + Filesystem::GenericUtf8PathString(path));
        return false;
    };
    std::uint64_t file_size = 0;
    std::array<char, kRegionFileHeaderSize> header{};
    if (!input.length(file_size))
        return refuse("Saved-input admission refused: unreadable or non-regular region file");
    if (!SavedInputLimits::RegionFileLengthAllowed(file_size))
        return refuse(
            "Saved-input admission refused: region file exceeds byte compatibility limit");
    if (!input.read_exact(header.data(), header.size(), stop))
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
    if (!input.read_exact(manifest.data(), manifest.size(), stop))
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
        if (!input.read_exact(payload.data(), payload.size(), stop))
            return refuse("truncated region record payload");
    }
    if (!input.at_end(stop))
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

bool DigestRegionRecords(const std::filesystem::path& path,
                         const std::vector<RegionRecord>& records,
                         MemberDigest& out,
                         std::string& error);

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
    std::size_t total_bytes = kRegionFileHeaderSize + records.size() * kRecordHeaderSize;
    for (const RegionRecord& record : records) {
        if (record.compressed_payload.size() > std::numeric_limits<std::uint32_t>::max() ||
            record.compressed_payload.size() > bytes.max_size() - total_bytes) {
            AddError(errors,
                     "region file exceeds the serializable size limit: " +
                         Filesystem::GenericUtf8PathString(path));
            return false;
        }
        total_bytes += record.compressed_payload.size();
    }
    bytes.reserve(total_bytes);

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

    WrittenRegionDigest written;
    std::string admission_error;
    const auto root = path.parent_path().parent_path().parent_path();
    const bool admitted = SaveTransaction::active_for(root) &&
                          DigestRegionRecords(path, records, written.digest, admission_error);
    std::filesystem::path temp_path;
    const auto staging = WorldSaveService::region_staging_path(path);
    ReclaimAbandonedStaging(staging);
    if (admitted)
        written.observation_revision = snapshot_observation_revision();
    if (!WriteDurableRegionTemp(staging,
                                bytes,
                                temp_path,
                                errors,
                                nullptr,
                                nullptr,
                                admitted ? &written.version : nullptr)) {
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

    if (!ReplaceMember(temp_path, path, errors, nullptr, nullptr, admitted ? &written : nullptr)) {
        RemoveTemporaryFile(temp_path);
        return false;
    }
    return true;
}

std::atomic<std::uint64_t>& ChunkRecordDecodeCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

std::shared_ptr<Chunk> DecodeChunkRecord(const RegionRecord& record,
                                         const std::filesystem::path& path,
                                         std::vector<std::string>* errors) {
    ChunkRecordDecodeCounter().fetch_add(1, std::memory_order_acq_rel);
    std::string payload;
    if (!DecompressPayload(record, payload, errors)) {
        return nullptr;
    }

    WorldStreamingState single;
    std::vector<std::string> load_errors;
    // The header's edited flag is the durable terrain authority bit: a payload may not claim
    // terrain ownership the header lacks. Owned water under an unflagged header is the one
    // accepted disagreement; the flag repair below keeps it.
    const ChunkAuthorityLoadPolicy record_policy{.terrain_edited =
                                                     (record.flags & kRecordFlagEdited) != 0u,
                                                 .validate_record_authority = true,
                                                 .allow_unflagged_water = true};
    if (!LoadWorldStreamingStateSnapshotJson(payload, single, load_errors, record_policy)) {
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
    if ((record.flags & kRecordFlagEdited) != 0u && !chunks.front()->has_water_authority()) {
        chunks.front()->mark_sdf_loaded_or_edited();
    } else if ((record.flags & kRecordFlagEdited) == 0u) {
        chunks.front()->mark_sdf_generated_current_params();
    }
    return chunks.front();
}

// What admission of one lod-0 record established. DecodeChunkRecord is a pure
// function of the record header and payload bytes, so these facts hold for any
// later record with the same header and exact payload bytes.
struct ChunkRecordFacts {
    IVec3 coords{0};
    bool lattice_ok = false;
    bool loaded_or_edited = false;
};

struct ChunkRecordKey {
    std::uint64_t id = 0;
    std::uint8_t lod_level = 0;
    std::uint8_t flags = 0;
    std::uint32_t uncompressed_size = 0;
    std::string_view compressed_payload;
    friend bool operator==(const ChunkRecordKey&, const ChunkRecordKey&) = default;
};

ChunkRecordKey ChunkRecordKeyOf(const RegionRecord& record) {
    return ChunkRecordKey{record.id,
                          record.lod_level,
                          record.flags,
                          record.uncompressed_size,
                          record.compressed_payload};
}

// Validation still reads the current file and checks region/index identity.
// Only the pure decompression and tile checks are reused, after comparing every
// header and compressed payload byte with a successfully admitted record.
class AdmittedFarLodRecords {
public:
    bool contains(const RegionRecord& record, int rx, int rz) {
        const std::lock_guard lock(m_mutex);
        const auto found = m_records.find(record.id);
        return found != m_records.end() && found->second.rx == rx && found->second.rz == rz &&
               ChunkRecordKeyOf(found->second.record) == ChunkRecordKeyOf(record);
    }

    void remember(const RegionRecord& record, int rx, int rz) {
        const auto bytes = record.compressed_payload.size();
        if (bytes > kMaxBytes)
            return;
        const std::lock_guard lock(m_mutex);
        if (const auto previous = m_records.find(record.id); previous != m_records.end()) {
            m_bytes -= previous->second.record.compressed_payload.size();
            m_records.erase(previous);
        }
        while (!m_records.empty() &&
               (m_records.size() >= kMaxRecords || bytes > kMaxBytes - m_bytes)) {
            m_bytes -= m_records.begin()->second.record.compressed_payload.size();
            m_records.erase(m_records.begin());
        }
        m_records.emplace(record.id, Entry{record, rx, rz});
        m_bytes += bytes;
    }

private:
    struct Entry {
        RegionRecord record;
        int rx;
        int rz;
    };
    static constexpr std::size_t kMaxRecords = 1024;
    static constexpr std::size_t kMaxBytes = std::size_t{64} * 1024 * 1024;
    std::mutex m_mutex;
    std::unordered_map<std::uint64_t, Entry> m_records;
    std::size_t m_bytes = 0;
};

bool AdmitFarLodRecord(const RegionRecord& record,
                       int rx,
                       int rz,
                       std::vector<std::string>* errors) {
    static AdmittedFarLodRecords admitted;
    if (admitted.contains(record, rx, rz))
        return true;
    const auto tier = static_cast<World::FarLodTier>(record.lod_level);
    std::string payload;
    World::FarLodTile tile;
    if (!DecompressPayload(record, payload, errors) ||
        !World::FarLodStore::decode_payload(payload, tier, rx, rz, record.flags, tile, errors))
        return false;
    admitted.remember(record, rx, rz);
    return true;
}

std::atomic<std::size_t>& PendingChunkAdmissionByteLimit() {
    static std::atomic<std::size_t> limit{std::size_t{256} * 1024 * 1024};
    return limit;
}

// Validation decodes held for the retaining read of the same world-read scope. Bounded in
// records and bytes, so a validation that ends in a refusal never holds every decoded chunk
// of a large save; a record past the bound is decoded again if a later read retains it.
class PendingChunkAdmissionMap {
public:
    struct Admission {
        RegionRecord record;
        ChunkRecordFacts facts;
        std::shared_ptr<Chunk> chunk;
        std::size_t bytes = 0;
    };
    const Admission* find(const ChunkRecordKey& key) const {
        const auto found = m_admissions.find(key.id);
        return found != m_admissions.end() && ChunkRecordKeyOf(found->second.record) == key
                   ? &found->second
                   : nullptr;
    }
    std::shared_ptr<Chunk> take(ChunkID id) {
        const auto found = m_admissions.find(id);
        if (found == m_admissions.end())
            return {};
        m_bytes -= found->second.bytes;
        auto chunk = std::move(found->second.chunk);
        m_admissions.erase(found);
        return chunk;
    }
    // Moves the chunk in only when it fits; otherwise the caller keeps it.
    bool retain(const RegionRecord& record,
                const ChunkRecordFacts& facts,
                std::shared_ptr<Chunk>& chunk) {
        take(record.id);
        const std::size_t limit = PendingChunkAdmissionByteLimit().load(std::memory_order_relaxed);
        const std::size_t bytes =
            record.compressed_payload.size() + sizeof(Chunk) + chunk->estimated_heap_bytes();
        if (m_admissions.size() >= kMaxRecords || bytes > limit || m_bytes > limit - bytes)
            return false;
        m_bytes += bytes;
        m_admissions.emplace(record.id, Admission{record, facts, std::move(chunk), bytes});
        return true;
    }
    [[nodiscard]] std::size_t size() const {
        return m_admissions.size();
    }
    [[nodiscard]] std::size_t bytes() const {
        return m_bytes;
    }
    [[nodiscard]] const std::unordered_map<ChunkID, Admission>& admissions() const {
        return m_admissions;
    }

private:
    static constexpr std::size_t kMaxRecords = 65536;
    std::unordered_map<ChunkID, Admission> m_admissions;
    std::size_t m_bytes = 0;
};

std::optional<PendingChunkAdmissionMap>& PendingChunkAdmissions() {
    static thread_local std::optional<PendingChunkAdmissionMap> pending;
    return pending;
}

class AdmittedChunkRecords {
public:
    std::optional<ChunkRecordFacts> find(const ChunkRecordKey& key) {
        const std::lock_guard lock(m_mutex);
        const auto found = m_facts.find(key.id);
        if (found == m_facts.end() || ChunkRecordKeyOf(found->second.record) != key)
            return std::nullopt;
        return found->second.facts;
    }
    bool matches_serialized(const ChunkRecordKey& key, const std::string& inputs) {
        const std::lock_guard lock(m_mutex);
        const auto found = m_facts.find(key.id);
        return found != m_facts.end() && ChunkRecordKeyOf(found->second.record) == key &&
               found->second.serialization_inputs == inputs;
    }
    std::shared_ptr<Chunk> take_decoded(const ChunkRecordKey& key) {
        const std::lock_guard lock(m_mutex);
        const auto found = m_facts.find(key.id);
        if (found == m_facts.end() || ChunkRecordKeyOf(found->second.record) != key)
            return {};
        m_decoded_bytes -= found->second.decoded_bytes;
        found->second.decoded_bytes = 0;
        return std::move(found->second.decoded);
    }
    void remember_decoded(const ChunkRecordKey& key, const std::shared_ptr<Chunk>& chunk) {
        const std::lock_guard lock(m_mutex);
        const auto found = m_facts.find(key.id);
        const auto bytes = sizeof(Chunk) + chunk->estimated_heap_bytes();
        if (found == m_facts.end() || ChunkRecordKeyOf(found->second.record) != key ||
            found->second.decoded || bytes > kMaxDecodedBytes)
            return;
        if (bytes > kMaxDecodedBytes - m_decoded_bytes) {
            for (auto& [id, entry] : m_facts) {
                (void)id;
                entry.decoded.reset();
                entry.decoded_bytes = 0;
            }
            m_decoded_bytes = 0;
        }
        found->second.decoded = chunk;
        found->second.decoded_bytes = bytes;
        m_decoded_bytes += bytes;
    }
    void remember(const ChunkRecordKey& key,
                  const ChunkRecordFacts& facts,
                  const std::string& inputs = {}) {
        const std::lock_guard lock(m_mutex);
        const auto bytes = key.compressed_payload.size();
        if (bytes > kMaxBytes)
            return;
        if (const auto previous = m_facts.find(key.id); previous != m_facts.end()) {
            if (ChunkRecordKeyOf(previous->second.record) == key &&
                (inputs.empty() || previous->second.serialization_inputs == inputs)) {
                previous->second.facts = facts;
                return;
            }
            m_bytes -= previous->second.record.compressed_payload.size();
            m_input_bytes -= previous->second.serialization_inputs.size();
            m_decoded_bytes -= previous->second.decoded_bytes;
            m_facts.erase(previous);
        }
        if (m_facts.size() >= kMaxRecords || m_bytes + bytes > kMaxBytes) {
            m_facts.clear();
            m_bytes = 0;
            m_input_bytes = 0;
            m_decoded_bytes = 0;
        }
        m_bytes += bytes;
        RegionRecord record;
        record.id = key.id;
        record.lod_level = key.lod_level;
        record.flags = key.flags;
        record.uncompressed_size = key.uncompressed_size;
        record.compressed_payload = key.compressed_payload;
        // Optional serialization reuse must never evict facts needed by commit admission.
        const bool retain_inputs = inputs.size() <= kMaxInputBytes - m_input_bytes;
        if (retain_inputs)
            m_input_bytes += inputs.size();
        m_facts.emplace(
            key.id, Entry{std::move(record), facts, retain_inputs ? inputs : std::string{}, {}, 0});
    }

private:
    struct Entry {
        RegionRecord record;
        ChunkRecordFacts facts;
        std::string serialization_inputs;
        std::shared_ptr<Chunk> decoded;
        std::size_t decoded_bytes = 0;
    };
    static constexpr std::size_t kMaxRecords = 65536;
    static constexpr std::size_t kMaxBytes = std::size_t{256} * 1024 * 1024;
    static constexpr std::size_t kMaxInputBytes = std::size_t{64} * 1024 * 1024;
    static constexpr std::size_t kMaxDecodedBytes = std::size_t{256} * 1024 * 1024;
    std::mutex m_mutex;
    std::size_t m_bytes = 0;
    std::size_t m_input_bytes = 0;
    std::size_t m_decoded_bytes = 0;
    std::unordered_map<ChunkID, Entry> m_facts;
};

AdmittedChunkRecords& AdmittedChunkRecordCache() {
    static AdmittedChunkRecords cache;
    return cache;
}

bool SdfLatticeAdmissible(const Chunk& chunk) {
    constexpr std::size_t kLattice =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    return chunk.sdf_data.empty() || chunk.sdf_data.size() == kLattice;
}

ChunkRecordFacts DecodedChunkRecordFacts(const Chunk& chunk) {
    return ChunkRecordFacts{
        chunk.get_coords(), SdfLatticeAdmissible(chunk), chunk.has_simulation_authority()};
}

// Exact record bytes gate both facts and the transfer of a cold validation decode.
// Transferred chunks leave the cache before the caller can mutate them.
bool AdmitChunkRecord(const RegionRecord& record,
                      const std::filesystem::path& path,
                      std::vector<std::string>* errors,
                      ChunkRecordFacts& facts,
                      std::shared_ptr<Chunk>* decoded,
                      bool validation_only = false,
                      bool cache_unretained = false) {
    const ChunkRecordKey key = ChunkRecordKeyOf(record);
    auto& pending = PendingChunkAdmissions();
    if (pending) {
        if (const auto* found = pending->find(key)) {
            facts = found->facts;
            if (decoded)
                *decoded = pending->take(record.id);
            return true;
        }
    }
    if (decoded) {
        if (auto admitted = AdmittedChunkRecordCache().take_decoded(key)) {
            facts = DecodedChunkRecordFacts(*admitted);
            *decoded = std::move(admitted);
            return true;
        }
    } else {
        if (const auto admitted = AdmittedChunkRecordCache().find(key)) {
            facts = *admitted;
            return true;
        }
    }
    auto chunk = DecodeChunkRecord(record, path, errors);
    if (!chunk)
        return false;
    facts = DecodedChunkRecordFacts(*chunk);
    // The decode is a pure function of these bytes, so a chunk whose canonical inputs still
    // equal the decoded ones is already stored by this record and needs no new encoding.
    AdmittedChunkRecordCache().remember(key, facts, ChunkSnapshotInputBytes(*chunk));
    if (decoded)
        *decoded = std::move(chunk);
    else if (pending && validation_only && pending->retain(record, facts, chunk))
        return true;
    else if (cache_unretained)
        // Cold admission also visits chunks outside the initial retain set. Transfer those
        // decodes to later activation reads, including reads on the adopting owner thread.
        AdmittedChunkRecordCache().remember_decoded(key, chunk);
    return true;
}

bool RepairOwnedWaterRecordFlag(Chunk& chunk, const RegionRecord& record) {
    if ((record.flags & kRecordFlagEdited) == 0u && chunk.has_water_authority()) {
        chunk.mark_dirty_for_save();
        return true;
    }
    return false;
}

// Legacy containers predate water authority, so evolved water on a generated
// chunk was saved without the edited flag. Keep that water instead of
// regenerating it; the chunk stays save-dirty until an indexed save records it.
bool PromoteLegacyWaterRecord(Chunk& chunk, const RegionRecord& record) {
    if (RepairOwnedWaterRecordFlag(chunk, record))
        return true;
    if ((record.flags & kRecordFlagEdited) != 0u)
        return false;
    if ((record.flags & kRecordFlagWaterPresent) == 0u &&
        !chunk.has_water_sim.load(std::memory_order_acquire))
        return false;
    chunk.set_water_authority();
    chunk.mark_sdf_generated_current_params();
    return true;
}

std::uint8_t ChunkRecordFlags(const Chunk& chunk, std::uint8_t previous_flags) {
    std::uint8_t flags = 0;
    // Edited/authoritative is sticky: once a chunk record was persisted with
    // post-generation edits it stays authoritative even after the in-memory
    // dirty flag is cleared by the save.
    if (chunk.has_simulation_authority() || (previous_flags & kRecordFlagEdited) != 0u) {
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

bool FileBytesEqual(const std::filesystem::path& path, const std::string& bytes);

// Publishes the manifest inside the active transaction. An unprotected manifest
// with identical bytes stays in place; a changed one is protected before the write.
bool PublishTransactionManifest(const std::filesystem::path& save_dir,
                                const std::string& bytes,
                                std::vector<std::string>* errors) {
    constexpr const char* kManifest = "chunks/region/world-manifest.json";
    auto* transaction = SaveTransaction::active_for(save_dir);
    if (transaction && !transaction->member_is_protected(kManifest)) {
        if (FileBytesEqual(save_dir / kManifest, bytes))
            return true;
        if (!transaction->protect({kManifest}, errors))
            return false;
    }
    return WorldSaveService::write_member_bytes(save_dir, kManifest, bytes, errors);
}

bool WriteWorldManifest(const std::filesystem::path& manifest_path,
                        std::vector<std::string>* errors) {
    const detail::RegionFileWriteGuard transaction(manifest_path);
    const auto save_dir = manifest_path.parent_path().parent_path().parent_path();
    const auto* active = SaveTransaction::active_for(save_dir);
    const nlohmann::json manifest = {
        {"schema", active ? kIndexedWorldManifestSchema : kWorldManifestSchema},
        {"container", "LMR1"},
        {"container_version", kRegionVersion}};

    std::string bytes;
    if (!BoundedJsonDump(manifest, SavedInputLimits::kManifestBytes, bytes, 4)) {
        AddError(errors, "world manifest exceeds 2 MiB compatibility limit");
        return false;
    }
    if (active)
        return PublishTransactionManifest(save_dir, bytes, errors);
    std::filesystem::path temp;
    const auto staging = WorldSaveService::region_staging_path(manifest_path);
    ReclaimAbandonedStaging(staging);
    // An identical manifest is already durable, so it stays in place.
    const bool unchanged = FileBytesEqual(manifest_path, bytes);
    if (!unchanged && !WriteDurableRegionTemp(staging, bytes, temp, errors))
        return false;
    if (!unchanged && !ReplaceMember(temp, manifest_path, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
    return true;
}

// Pure value parser shared by admission and clock restoration. Semantic diagnostics precede
// fingerprint comparisons; valid edits still have to match the committed index.
bool ReadWaterCursor(const nlohmann::json& metadata, std::size_t& cursor, std::string& error) {
    cursor = 0;
    if (!metadata.contains("waterSimCursor"))
        return true;
    const auto& value = metadata.at("waterSimCursor");
    if (!value.is_number_unsigned() ||
        value.get<std::uint64_t>() > std::numeric_limits<std::size_t>::max()) {
        error = "Corrupt world metadata: invalid water simulation cursor.";
        return false;
    }
    cursor = value.get<std::size_t>();
    return true;
}

// Metadata gate shared by load_world and member_digest. Returns an empty string when the file is
// absent or valid, else the rejection message. Malformed JSON throws like before.
std::string CheckMetadataValue(const nlohmann::json& metadata,
                               std::int64_t* snapshot_format = nullptr) {
    if (!metadata.is_object())
        return "corrupt world metadata: expected an object";
    // Old worlds can contain only metadata: no chunk container exists yet
    // to reveal their version. Refuse them before the clean-miss path.
    if (!metadata.contains("container_version"))
        return WorldSaveService::kObsoleteWorldMessage;
    const auto& version = metadata.at("container_version");
    if (!version.is_number_integer())
        return "corrupt world metadata container version";
    if (version != WorldSaveService::kContainerVersion &&
        version != WorldSaveService::kIndexedMetadataVersion) {
        if (version >= 0 && version < WorldSaveService::kContainerVersion)
            return WorldSaveService::kObsoleteWorldMessage;
        if (version > WorldSaveService::kIndexedMetadataVersion)
            return "unsupported future world metadata container version " + version.dump();
        return "corrupt world metadata container version";
    }
    if (metadata.contains("simulationTick") || metadata.contains("calendar") ||
        metadata.contains("renderCalendar")) {
        world::WorldClock clock;
        std::string error;
        std::optional<Vec3> anchor;
        std::optional<world::WorldCalendar> render_calendar;
        if (!world::WorldClock::from_metadata(metadata, clock, error) ||
            !ReadAmbientAnchor(metadata, anchor, error) ||
            !world::WorldClock::read_render_calendar(metadata, render_calendar, error))
            return error;
    }
    if (metadata.contains("snapshotFormat") &&
        (!metadata.at("snapshotFormat").is_number_integer() || metadata.at("snapshotFormat") != 1))
        return "inconsistent snapshot: unsupported snapshot format";
    std::size_t cursor = 0;
    std::string cursor_error;
    if (!ReadWaterCursor(metadata, cursor, cursor_error))
        return cursor_error;
    if (snapshot_format != nullptr)
        *snapshot_format = metadata.contains("snapshotFormat") ||
                                   version == WorldSaveService::kIndexedMetadataVersion
                               ? 1
                               : 0;
    return {};
}

std::string CheckMetadataFile(const std::filesystem::path& save_dir,
                              std::int64_t* snapshot_format = nullptr) {
    const auto metadata_path = save_dir / "world_info.json";
    nlohmann::json metadata;
    bool missing = false;
    if (!ReadSavedJson(metadata_path, SavedInputLimits::kMetadataBytes, metadata, missing, true) ||
        (!missing && !AdmitMetadata(metadata)))
        return "Saved-input admission refused: unreadable or oversized world metadata.";
    if (missing)
        return {};
    return CheckMetadataValue(metadata, snapshot_format);
}

// Manifest gate shared by load_world and member_digest (same contract as CheckMetadataFile).
std::string CheckManifestValue(const nlohmann::json& manifest) {
    if (manifest.is_object() && manifest.contains("schema") && manifest.at("schema").is_string()) {
        const auto schema = manifest.at("schema").get<std::string>();
        const std::string prefix = "luminumbra.persistence.world_manifest.v";
        if (schema.rfind(prefix, 0) == 0 && schema != kWorldManifestSchema &&
            schema != kIndexedWorldManifestSchema) {
            unsigned version = 0;
            const auto parsed = std::from_chars(
                schema.data() + prefix.size(), schema.data() + schema.size(), version);
            if (parsed.ec == std::errc{} && parsed.ptr == schema.data() + schema.size()) {
                if (version > 2)
                    return "unsupported future world manifest schema: " + schema;
                if (version == 0)
                    return WorldSaveService::kObsoleteWorldMessage;
            }
        }
    }
    if (!manifest.is_object() || !manifest.contains("schema") ||
        (manifest.at("schema") != kWorldManifestSchema &&
         manifest.at("schema") != kIndexedWorldManifestSchema) ||
        !manifest.contains("container") || manifest.at("container") != "LMR1" ||
        !manifest.contains("container_version") ||
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
    std::uint64_t hash = 14695981039346656037ull;
    // Pointer loop over a local: every lod-0 payload byte passes here on each validation.
    const auto fold = [&hash](const std::string& bytes) {
        std::uint64_t value = hash;
        const auto* byte = reinterpret_cast<const unsigned char*>(bytes.data());
        for (const auto* const end = byte + bytes.size(); byte != end; ++byte) {
            value ^= *byte;
            value *= 1099511628211ull;
        }
        hash = value;
    };
    for (const RegionRecord* record : lod0) {
        std::string header;
        AppendU64(header, record->id);
        header.push_back(static_cast<char>(record->flags));
        AppendU32(header, record->uncompressed_size);
        AppendU32(header, static_cast<std::uint32_t>(record->compressed_payload.size()));
        fold(header);
        fold(record->compressed_payload);
        digest.bytes += header.size() + record->compressed_payload.size();
    }
    digest.present = true;
    digest.fnv64 = hash;
    return digest;
}

std::atomic<std::uint64_t>& RegionFingerprintCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

// Lod0Fingerprint is a pure function of the lod-0 records. A region whose lod-0 records equal,
// byte for byte, the ones last hashed at the same path reuses that digest; any changed header or
// payload byte misses and is hashed again, so tamper evidence is compared on every validation.
class RegionFingerprints {
public:
    MemberDigest fingerprint(const std::filesystem::path& path,
                             const std::vector<RegionRecord>& records) {
        std::vector<const RegionRecord*> lod0;
        std::size_t lod0_bytes = 0;
        for (const RegionRecord& record : records) {
            if (record.lod_level != 0)
                continue;
            lod0.push_back(&record);
            lod0_bytes += record.compressed_payload.size();
        }
        if (const auto known = find(path); known && Matches(*known, lod0))
            return known->digest;
        RegionFingerprintCounter().fetch_add(1, std::memory_order_acq_rel);
        const MemberDigest digest = Lod0Fingerprint(records);
        if (digest.present && lod0_bytes <= kMaxBytes)
            remember(path, lod0, lod0_bytes, digest);
        return digest;
    }

private:
    struct Entry {
        std::vector<RegionRecord> lod0;
        std::size_t bytes = 0;
        MemberDigest digest;
    };
    static constexpr std::size_t kMaxRegions = 32;
    static constexpr std::size_t kMaxBytes = std::size_t{256} * 1024 * 1024;

    static bool Matches(const Entry& entry, const std::vector<const RegionRecord*>& lod0) {
        if (entry.lod0.size() != lod0.size())
            return false;
        for (std::size_t index = 0; index < lod0.size(); ++index) {
            const RegionRecord& known = entry.lod0[index];
            const RegionRecord& current = *lod0[index];
            if (known.id != current.id || known.flags != current.flags ||
                known.uncompressed_size != current.uncompressed_size ||
                known.compressed_payload != current.compressed_payload)
                return false;
        }
        return true;
    }

    std::shared_ptr<const Entry> find(const std::filesystem::path& path) {
        const std::lock_guard lock(m_mutex);
        const auto found = m_entries.find(path);
        return found == m_entries.end() ? nullptr : found->second;
    }

    void remember(const std::filesystem::path& path,
                  const std::vector<const RegionRecord*>& lod0,
                  std::size_t lod0_bytes,
                  const MemberDigest& digest) {
        auto entry = std::make_shared<Entry>();
        entry->lod0.reserve(lod0.size());
        for (const RegionRecord* record : lod0)
            entry->lod0.push_back(*record);
        entry->bytes = lod0_bytes;
        entry->digest = digest;
        const std::lock_guard lock(m_mutex);
        if (const auto previous = m_entries.find(path); previous != m_entries.end()) {
            m_bytes -= previous->second->bytes;
            m_entries.erase(previous);
        }
        while (!m_entries.empty() &&
               (m_entries.size() >= kMaxRegions || m_bytes + lod0_bytes > kMaxBytes)) {
            m_bytes -= m_entries.begin()->second->bytes;
            m_entries.erase(m_entries.begin());
        }
        m_bytes += lod0_bytes;
        m_entries.emplace(path, std::move(entry));
    }

    std::mutex m_mutex;
    std::size_t m_bytes = 0;
    std::map<std::filesystem::path, std::shared_ptr<const Entry>> m_entries;
};

MemberDigest RegionLod0Fingerprint(const std::filesystem::path& path,
                                   const std::vector<RegionRecord>& records) {
    static RegionFingerprints fingerprints;
    return fingerprints.fingerprint(path, records);
}

bool DigestRegionRecords(const std::filesystem::path& path,
                         const std::vector<RegionRecord>& records,
                         MemberDigest& out,
                         std::string& error) {
    int rx = 0, rz = 0;
    if (!ParseRegionFileName(path, rx, rz)) {
        error = "unrecognised region file name";
        return false;
    }
    std::vector<std::string> diagnostics;
    const auto first_error = [&diagnostics](const char* fallback) {
        return diagnostics.empty() ? std::string(fallback) : diagnostics.front();
    };
    for (const RegionRecord& record : records) {
        if (record.lod_level == 0) {
            ChunkRecordFacts facts;
            if (!AdmitChunkRecord(record, path, &diagnostics, facts, nullptr)) {
                error = first_error("corrupt chunk record");
                return false;
            }
            if (!facts.lattice_ok) {
                error = "corrupt world chunk SDF lattice";
                return false;
            }
            int chunk_rx = 0;
            int chunk_rz = 0;
            WorldSaveService::region_coords_for_chunk(facts.coords, chunk_rx, chunk_rz);
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
            if (!AdmitFarLodRecord(record, rx, rz, &diagnostics)) {
                error = first_error("corrupt far-LOD record");
                return false;
            }
        }
    }
    out = RegionLod0Fingerprint(path, records);
    return true;
}

bool FileBytesEqual(const std::filesystem::path& path, const std::string& bytes) {
    const SavedInputFile input(path);
    SavedInputFile::Version before{}, after{};
    std::uint64_t length = 0;
    if (!input.version(before) || !input.length(length) || length != bytes.size())
        return false;
    std::array<char, 65536> buffer{};
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min(buffer.size(), bytes.size() - offset);
        if (!input.read_exact(buffer.data(), count) ||
            std::memcmp(buffer.data(), bytes.data() + offset, count) != 0)
            return false;
        offset += count;
    }
    return input.at_end() && input.version(after) && before == after;
}

// Joins the active transaction for save_dir, else (indexed directory or pending journal) runs an
// implicit one-writer transaction, else is inert and the caller keeps its legacy code path.
// Owner thread only. Construct BEFORE the first member write; call commit() after the last one.
class TransactionScope {
public:
    TransactionScope(const std::filesystem::path& save_dir,
                     const std::vector<std::string>& relative_paths,
                     std::vector<std::string>* errors,
                     bool require_index = false,
                     CreatedWorldArtifacts* artifacts = nullptr)
        : m_artifacts(artifacts) {
        if (SaveTransaction* active = SaveTransaction::active_for(save_dir)) {
            m_engaged = true;
            m_ok = active->protect(relative_paths, errors);
            return;
        }
        WorldSaveService::SaveFormat format;
        if (!journal_pending(save_dir) &&
            !WorldSaveService::read_save_format(save_dir, format, errors)) {
            m_engaged = true;
            m_ok = false;
            return;
        }
        if (!require_index && !format.indexed && !journal_pending(save_dir))
            return;
        // Refuse a known invalid baseline before begin() creates journal/region directories.
        // Pending recovery must still run first: installed metadata may be a partial new member.
        if (!journal_pending(save_dir)) {
            std::string metadata_error;
            try {
                metadata_error = CheckMetadataFile(save_dir);
            } catch (const std::exception& error) {
                metadata_error =
                    std::string("corrupt or unreadable world metadata: ") + error.what();
            }
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
            save_dir, previous.tick, previous.anchors, &WorldSaveService::member_digest, artifacts);
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
        const bool committed = WorldSaveService::finalize_indexed_envelope(
                                   m_transaction->save_dir(), errors, m_artifacts) &&
                               m_transaction->commit(errors);
        if (committed)
            m_facts->run_deferred();
        else
            m_facts->discard_deferred();
        return committed;
    }

private:
    bool m_ok = true;
    bool m_engaged = false;
    CreatedWorldArtifacts* m_artifacts = nullptr;
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
                                const std::map<std::string, MemberDigest>& region_digests,
                                const std::stop_token& stop) {
    static const char* const kSingletons[] = {"world_info.json",
                                              "aether_state.efs",
                                              "chunks/region/world-manifest.json",
                                              "chunks/region/plant-entities.json",
                                              "chunks/region/ground-objects.json",
                                              "chunks/region/field-pages.fpg",
                                              "chunks/region/active-regions.arl"};
    std::set<std::string> listed;
    for (const SnapshotMember& member : index.members) {
        if (stop.stop_requested())
            return "Saved-world validation cancelled.";
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
            if (!hash_file(save_dir / member.path, bytes, fnv64, exists, nullptr, stop))
                return member.path;
            actual.present = exists;
            actual.bytes = bytes;
            actual.fnv64 = fnv64;
            if (member.path == "aether_state.efs" && actual.present) {
                if (stop.stop_requested())
                    return "Saved-world validation cancelled.";
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
        if (stop.stop_requested())
            return "Saved-world validation cancelled.";
        if (listed.count(single) != 0)
            continue;
        const SavedInputFile input(save_dir / single);
        if (!input.missing())
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
                            std::vector<std::string>* errors,
                            const SnapshotIndex* admitted_index = nullptr) {
    std::filesystem::path save_dir;
    std::string relative;
    if (!SnapshotMemberForRegionFile(region_file, save_dir, relative))
        return true;
    SnapshotIndex loaded;
    if (!admitted_index) {
        WorldSaveService::SaveFormat format;
        if (!WorldSaveService::read_save_format(save_dir, format, errors, {}, &loaded))
            return false;
        admitted_index = &loaded;
    }
    if (admitted_index->generation == 0)
        return true;
    const auto& index = *admitted_index;
    const auto member = std::lower_bound(
        index.members.begin(),
        index.members.end(),
        relative,
        [](const SnapshotMember& value, const std::string& path) { return value.path < path; });
    const bool listed = member != index.members.end() && member->path == relative;
    const auto digest = RegionLod0Fingerprint(region_file, records);
    if ((!listed && digest.present) ||
        (listed &&
         (member->present != digest.present ||
          (digest.present && (member->bytes != digest.bytes || member->fnv64 != digest.fnv64))))) {
        AddError(errors, "inconsistent snapshot: " + relative);
        return false;
    }
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

namespace {
std::shared_mutex& SnapshotPublicationMutex() {
    static std::shared_mutex mutex;
    return mutex;
}
std::size_t& SnapshotReadDepth() {
    static thread_local std::size_t depth = 0;
    return depth;
}
std::size_t& SnapshotWriteDepth() {
    static thread_local std::size_t depth = 0;
    return depth;
}
} // namespace

detail::SnapshotReadOwnership::SnapshotReadOwnership() {
    if (SnapshotReadDepth() == 0 && SnapshotWriteDepth() == 0) {
        if (!SnapshotPublicationMutex().try_lock_shared()) {
            const WaitingSaveWriter waiting;
            SnapshotPublicationMutex().lock_shared();
        }
        m_locked = true;
    }
    ++SnapshotReadDepth();
}
detail::SnapshotReadOwnership::~SnapshotReadOwnership() {
    --SnapshotReadDepth();
    if (m_locked)
        SnapshotPublicationMutex().unlock_shared();
}
detail::SnapshotWriteOwnership::SnapshotWriteOwnership()
    : m_lock(SnapshotPublicationMutex(), std::defer_lock) {
    if (SnapshotWriteDepth() == 0 && !m_lock.try_lock()) {
        const WaitingSaveWriter waiting;
        m_lock.lock();
    }
    ++SnapshotWriteDepth();
}
detail::SnapshotWriteOwnership::~SnapshotWriteOwnership() {
    --SnapshotWriteDepth();
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
        if (!BoundedJsonDump(
                metadata, SavedInputLimits::kMetadataBytes - 1, normalized, 4, false)) {
            AddError(errors,
                     "Saved-input admission refused: normalized metadata exceeds 2 MiB "
                     "compatibility limit.");
            return false;
        }
    }
    return true;
}

bool WorldSaveService::read_save_format(const std::filesystem::path& save_dir,
                                        SaveFormat& format,
                                        std::vector<std::string>* errors,
                                        const std::stop_token& stop,
                                        SnapshotIndex* admitted_index) {
    const detail::SnapshotReadOwnership ownership;
    SavedJsonFailure json_failure = SavedJsonFailure::None;
    format = {};
    nlohmann::json metadata;
    nlohmann::json manifest;
    bool metadata_missing = false;
    bool manifest_missing = false;
    if (!ReadSavedJson(save_dir / "world_info.json",
                       SavedInputLimits::kMetadataBytes,
                       metadata,
                       metadata_missing,
                       true,
                       &json_failure,
                       stop) ||
        (!metadata_missing && !AdmitMetadata(metadata))) {
        AddError(errors,
                 stop.stop_requested() ? "Saved-world validation cancelled."
                 : json_failure == SavedJsonFailure::Limit
                     ? "Saved-input admission refused: world metadata compatibility limit."
                     : "corrupt or unreadable world metadata");
        return false;
    }
    std::int64_t marker = 0;
    if (!metadata_missing) {
        const auto error = CheckMetadataValue(metadata, &marker);
        if (!error.empty()) {
            AddError(errors, error);
            return false;
        }
    }
    if (!ReadSavedJson(world_manifest_path(save_dir),
                       SavedInputLimits::kManifestBytes,
                       manifest,
                       manifest_missing,
                       false,
                       &json_failure,
                       stop)) {
        AddError(errors,
                 stop.stop_requested() ? "Saved-world validation cancelled."
                 : json_failure == SavedJsonFailure::Limit
                     ? "Saved-input admission refused: world manifest compatibility limit."
                     : "corrupt or unreadable world manifest");
        return false;
    }
    if (!manifest_missing) {
        const auto error = CheckManifestValue(manifest);
        if (!error.empty()) {
            AddError(errors, error);
            return false;
        }
    }
    SnapshotIndex index;
    if (!read_snapshot_index(save_dir, index, errors, stop))
        return false;
    format.indexed = index.generation != 0;
    format.requires_index =
        marker != 0 || (!manifest_missing && manifest.at("schema") == kIndexedWorldManifestSchema);
    if (!format.indexed && format.requires_index) {
        AddError(errors, "inconsistent snapshot: index missing");
        return false;
    }
    if (admitted_index)
        *admitted_index = std::move(index);
    return true;
}

bool WorldSaveService::finalize_indexed_envelope(const std::filesystem::path& save_dir,
                                                 std::vector<std::string>* errors,
                                                 CreatedWorldArtifacts* artifacts) {
    const SaveOwnership ownership;
    auto* transaction = SaveTransaction::active_for(save_dir);
    if (!transaction) {
        AddError(errors, "indexed envelope requires an open save transaction");
        return false;
    }
    nlohmann::json metadata;
    bool missing = false;
    if (!ReadSavedJson(save_dir / "world_info.json",
                       SavedInputLimits::kMetadataBytes,
                       metadata,
                       missing,
                       true) ||
        (!missing && !AdmitMetadata(metadata))) {
        AddError(errors, "corrupt or unreadable world metadata");
        return false;
    }
    if (!missing) {
        const auto error = CheckMetadataValue(metadata);
        if (!error.empty()) {
            AddError(errors, error);
            return false;
        }
        if (metadata.at("container_version") != kIndexedMetadataVersion ||
            !metadata.contains("snapshotFormat")) {
            metadata["container_version"] = kIndexedMetadataVersion;
            metadata["snapshotFormat"] = 1;
            std::string bytes;
            if (!BoundedJsonDump(metadata, SavedInputLimits::kMetadataBytes - 1, bytes, 4, false) ||
                !transaction->protect({"world_info.json"}, errors) ||
                !write_file_durably(save_dir / "world_info.json", bytes + "\n", errors, artifacts))
                return false;
        }
    }
    const nlohmann::json manifest = {{"schema", kIndexedWorldManifestSchema},
                                     {"container", "LMR1"},
                                     {"container_version", kRegionVersion}};
    std::string bytes;
    if (!BoundedJsonDump(manifest, SavedInputLimits::kManifestBytes, bytes, 4))
        return false;
    const auto destination = world_manifest_path(save_dir);
    if (artifacts) {
        for (const auto& directory : {save_dir / "chunks", destination.parent_path()})
            if (!std::filesystem::exists(directory))
                artifacts->create_directory(directory);
        // Initial creation reserves each destination once. A payload writer may already have
        // published this identical manifest, in which case no second replacement is needed.
        std::string existing;
        {
            const SavedInputFile input(destination);
            if (!input.missing() && input.read(existing, SavedInputLimits::kManifestBytes) &&
                existing == bytes)
                return true;
        }
        return transaction->protect({"chunks/region/world-manifest.json"}, errors) &&
               write_file_durably(destination, bytes, errors, artifacts);
    }
    std::filesystem::create_directories(destination.parent_path());
    return PublishTransactionManifest(save_dir, bytes, errors);
}

bool WorldSaveService::save_metadata(const std::string& bytes,
                                     const std::filesystem::path& save_dir,
                                     std::vector<std::string>* errors,
                                     CreatedWorldArtifacts* artifacts) {
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
    TransactionScope scope(save_dir,
                           {"world_info.json"},
                           errors,
                           metadata.contains("snapshotFormat") ||
                               (metadata.contains("container_version") &&
                                metadata.at("container_version") == kIndexedMetadataVersion),
                           artifacts);
    if (!scope.ok())
        return false;
    if (!validate_save(save_dir, errors))
        return false;
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
    if (scope.engaged()) {
        metadata["container_version"] = kIndexedMetadataVersion;
        metadata["snapshotFormat"] = 1;
    }
    const std::string output_bytes =
        (scope.engaged() || has_clock || has_render_calendar) ? metadata.dump(4) + "\n" : bytes;
    if (!admit_metadata_bytes(output_bytes, errors))
        return false;
    const auto destination = save_dir / "world_info.json";
    if (artifacts)
        artifacts->reserve_file(destination);
    else
        ReclaimAbandonedStaging(destination);
    if (!artifacts && FileBytesEqual(destination, output_bytes))
        return SaveTransaction::check_publication_target(destination, errors) &&
               scope.commit(errors);
    std::filesystem::path temp;
    if (!WriteDurableRegionTemp(destination, output_bytes, temp, errors, artifacts))
        return false;
    if (InterruptBeforeRegionReplaceForTesting().exchange(false)) {
        RemoveTemporaryFile(temp, artifacts);
        AddError(errors, "interrupted before metadata replacement");
        return false;
    }
    if (!ReplaceMember(temp, destination, errors, artifacts)) {
        RemoveTemporaryFile(temp, artifacts);
        return false;
    }
    return scope.commit(errors);
}

namespace {
// Every object must lie in a region the ledger records and must not be spawned after the clock.
bool GroundObjectsMatchLedger(const world::GroundObjectStore& store,
                              const world::ActiveRegionLedger& ledger,
                              std::uint64_t tick,
                              std::vector<std::string>* errors) {
    for (const auto& entry : store.objects()) {
        const world::GroundObject& object = entry.second;
        if (ledger.records().count(world::GroundObjectStore::region_of(object)) == 0) {
            AddError(errors, "corrupt ground objects: orphaned object");
            return false;
        }
        if (object.spawn_tick > tick) {
            AddError(errors, "corrupt ground objects: spawn tick is ahead of the clock");
            return false;
        }
    }
    return true;
}
} // namespace

bool WorldSaveService::save_metadata_active_regions_and_ground_objects(
    const std::string& bytes,
    const world::ActiveRegionLedger& ledger,
    const world::GroundObjectStore& store,
    const world::GroundNotifications& notifications,
    const std::filesystem::path& save_dir,
    std::vector<std::string>* errors,
    CreatedWorldArtifacts* artifacts) {
    const SaveGuard guard;
    // Refuse what the next load would reject: orphaned objects or spawns ahead of the saved clock.
    if (!admit_metadata_bytes(bytes, errors))
        return false;
    {
        world::WorldClock clock;
        std::string clock_error;
        if (!world::WorldClock::from_metadata(
                nlohmann::json::parse(bytes, nullptr, false), clock, clock_error)) {
            AddError(errors, clock_error);
            return false;
        }
        if (!GroundObjectsMatchLedger(store, ledger, clock.tick(), errors))
            return false;
    }
    TransactionScope scope(save_dir,
                           {world::GroundObjectStore::kRelativePath,
                            "world_info.json",
                            "chunks/region/active-regions.arl"},
                           errors,
                           true,
                           artifacts);
    if (!scope.ok() || !save_ground_objects(store, save_dir, errors, notifications))
        return false;
    auto saved_ledger = ledger;
    return save_metadata_and_active_regions(bytes, saved_ledger, save_dir, errors, artifacts) &&
           scope.commit(errors);
}

bool WorldSaveService::save_metadata_and_active_regions(const std::string& bytes,
                                                        world::ActiveRegionLedger& ledger,
                                                        const std::filesystem::path& save_dir,
                                                        std::vector<std::string>* errors,
                                                        CreatedWorldArtifacts* artifacts) {
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
    std::string encoded;
    if (!EncodeActiveRegionsForSave(saved, encoded, errors))
        return false;
    world::ActiveRegionLedger checked;
    if (!world::ActiveRegionLedger::decode(encoded, checked, error) ||
        ledger.header_tick() > clock.tick()) {
        AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
    TransactionScope scope(save_dir,
                           {"world_info.json",
                            "chunks/region/active-regions.arl",
                            world::GroundObjectStore::kRelativePath},
                           errors,
                           metadata.contains("snapshotFormat") ||
                               (metadata.contains("container_version") &&
                                metadata.at("container_version") == kIndexedMetadataVersion),
                           artifacts);
    if (!scope.ok())
        return false;
    world::WorldClock previous_clock;
    bool requires_active_regions = false;
    if (!read_clock_metadata(save_dir, previous_clock, requires_active_regions, errors))
        return false;
    // Each writer validates the installed pair. On rewind, replace the ledger
    // first so it cannot be newer than the clock, even if metadata writing fails.
    const bool written = clock.tick() < previous_clock.tick()
                             ? (save_active_regions(ledger, clock, save_dir, errors, artifacts) &&
                                save_metadata(bytes, save_dir, errors, artifacts))
                             : (save_metadata(bytes, save_dir, errors, artifacts) &&
                                save_active_regions(ledger, clock, save_dir, errors, artifacts));
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
        if (!ReadWaterCursor(metadata, cursor, error)) {
            AddError(errors, error);
            return false;
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
                                                   std::vector<std::string>* errors) const {
    const SaveGuard guard;
    LUMIN_PROFILE_ZONE_N("File plant entities save");
    if (!m_prepared) {
        AddError(errors, "plant write has not been prepared");
        return false;
    }
    if (!validate_save(save_dir, errors))
        return false;
    TransactionScope scope(save_dir, {}, errors);
    if (!scope.ok())
        return false;
    const std::filesystem::path path = plant_entities_path(save_dir);
    const detail::RegionFileWriteGuard transaction(path);
    // Like ledger staging, abandoned plant copies must live outside the strict
    // region scan. Reclaim them even when the new roster is empty.
    const auto staging = save_dir / path.filename();
    ReclaimAbandonedStaging(staging);
    if (!m_empty && FileBytesEqual(path, m_bytes)) {
        if (auto* active = SaveTransaction::active_for(save_dir);
            active && active->member_is_protected("chunks/region/plant-entities.json") &&
            !SaveTransaction::check_publication_target(path, errors))
            return false;
        return scope.commit(errors);
    }
    if (auto* active = SaveTransaction::active_for(save_dir);
        active && !active->protect({"chunks/region/plant-entities.json"}, errors))
        return false;
    std::error_code ec;
    if (m_empty) {
        // No plants -> no file (a no-plant save is byte-identical). Retire any stale file.
        if (scope.engaged())
            return durable_remove(path, errors) && scope.commit(errors);
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
        return FlushRemovalDirectory(path, errors) && scope.commit(errors);
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        AddError(errors, "failed to create plant directory: " + ec.message());
        return false;
    }
    return WriteFileDurably(path, m_bytes, staging, errors) && scope.commit(errors);
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
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
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

std::filesystem::path WorldSaveService::ground_objects_path(const std::filesystem::path& save_dir) {
    return region_directory(save_dir) / "ground-objects.json";
}

bool WorldSaveService::save_ground_objects(const world::GroundObjectStore& store,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors,
                                           const world::GroundNotifications& notifications) {
    const SaveGuard guard;
    std::string bytes;
    try {
        std::size_t rejected_payloads = 0;
        bytes = store.serialize(notifications, &rejected_payloads);
        if (rejected_payloads != 0) {
            // Dropping an accepted event would lose it on reload, so the save is refused.
            AddError(errors,
                     "Ground objects save refused: " + std::to_string(rejected_payloads) +
                         " pending notification(s) have non-UTF-8 payloads");
            return false;
        }
        if (!bytes.empty()) {
            world::GroundObjectStore parsed;
            std::string error;
            if (!world::GroundObjectStore::parse(
                    bytes, std::numeric_limits<std::uint64_t>::max(), parsed, error)) {
                AddError(errors, "Ground objects save failed: " + error);
                return false;
            }
        }
    } catch (const std::exception& error) {
        AddError(errors,
                 std::string("Ground objects save failed: serialization exception: ") +
                     error.what());
        return false;
    } catch (...) {
        AddError(errors, "Ground objects save failed: serialization exception");
        return false;
    }
    TransactionScope scope(save_dir, {world::GroundObjectStore::kRelativePath}, errors, true);
    if (!scope.ok() || !validate_save(save_dir, errors))
        return false;
    if (bytes.empty()) {
        bool missing = false;
        {
            const SavedInputFile input(ground_objects_path(save_dir));
            missing = input.missing();
        }
        return (missing ||
                remove_member(save_dir, world::GroundObjectStore::kRelativePath, errors)) &&
               scope.commit(errors);
    }
    return write_member_bytes(save_dir, world::GroundObjectStore::kRelativePath, bytes, errors) &&
           scope.commit(errors);
}

bool WorldSaveService::load_ground_objects(world::GroundObjectStore& out,
                                           const std::filesystem::path& save_dir,
                                           std::uint64_t next_id,
                                           std::vector<std::string>* errors,
                                           const std::stop_token& stop,
                                           world::GroundNotifications* notifications) {
    const detail::SnapshotReadOwnership ownership;
    const std::filesystem::path path = ground_objects_path(save_dir);
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    bool present = false;
    std::string bytes;
    {
        const SavedInputFile input(path);
        present = !input.missing();
        if (present && !input.read(bytes, world::GroundObjectStore::kMaxFileBytes, stop)) {
            AddError(errors,
                     stop.stop_requested()
                         ? "Saved-world validation cancelled."
                         : "Corrupt ground objects: unreadable or oversized file.");
            return false;
        }
    }
    if (!SaveTransaction::active_for(save_dir)) {
        if (snapshot_index_exists(save_dir)) {
            SnapshotIndex index;
            if (!read_snapshot_index(save_dir, index, errors, stop))
                return false;
            const auto member = std::find_if(
                index.members.begin(), index.members.end(), [](const SnapshotMember& value) {
                    return value.path == "chunks/region/ground-objects.json";
                });
            const auto fingerprint = fnv1a64_bytes(bytes.data(), bytes.size());
            if ((member == index.members.end() && present) ||
                (member != index.members.end() &&
                 (member->present != present ||
                  (present && (member->bytes != bytes.size() || member->fnv64 != fingerprint))))) {
                AddError(errors, "inconsistent snapshot: chunks/region/ground-objects.json");
                return false;
            }
        } else if (present) {
            AddError(errors, "inconsistent snapshot: ground authority without index");
            return false;
        }
    }
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    if (!present) {
        out.clear();
        if (notifications)
            *notifications = {};
        return true; // A verified absent member is a clean miss.
    }
    world::GroundObjectStore parsed;
    world::GroundNotifications pending;
    std::string error;
    if (!world::GroundObjectStore::parse(bytes, next_id, parsed, error, &pending)) {
        AddError(errors, "Ground objects load failed: " + error);
        return false;
    }
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    out = std::move(parsed);
    if (notifications)
        *notifications = std::move(pending);
    return true;
}

bool WorldSaveService::validate_ground_objects(const std::filesystem::path& save_dir,
                                               const world::ActiveRegionLedger& ledger,
                                               std::vector<std::string>* errors,
                                               const std::stop_token& stop) {
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    bool has_objects = false;
    {
        const SavedInputFile input(ground_objects_path(save_dir));
        has_objects = !input.missing();
    }
    if (!has_objects)
        return true;
    bool has_ledger = false;
    {
        const SavedInputFile input(active_regions_path(save_dir));
        has_ledger = !input.missing();
    }
    if (!has_ledger) {
        // A ledger-less save has no records, so every ground object is orphaned.
        AddError(errors, "corrupt ground objects: orphaned object");
        return false;
    }
    world::GroundObjectStore store;
    if (!load_ground_objects(store, save_dir, ledger.next_persistent_id(), errors, stop))
        return false;
    world::WorldClock clock;
    bool required = false;
    if (!read_clock_metadata(save_dir, clock, required, errors, nullptr, nullptr, nullptr, stop))
        return false;
    return GroundObjectsMatchLedger(store, ledger, clock.tick(), errors);
}

std::filesystem::path WorldSaveService::field_pages_path(const std::filesystem::path& save_dir) {
    return save_dir / world::FieldPageState::kRelativePath;
}

bool WorldSaveService::PreparedFieldWrite::prepare(const world::FieldPageState& state,
                                                   std::vector<std::string>* errors) {
    m_prepared = false;
    m_bytes.clear();
    try {
        std::string bytes = state.serialize();
        world::FieldPageState parsed;
        std::string error;
        if (!bytes.empty() && !world::FieldPageState::parse(bytes, parsed, error)) {
            AddError(errors, "Field pages save failed: " + error);
            return false;
        }
        m_bytes = std::move(bytes);
        m_prepared = true;
        return true;
    } catch (const std::exception& error) {
        AddError(errors, std::string("Field pages save failed: ") + error.what());
        return false;
    }
}

bool WorldSaveService::PreparedFieldWrite::publish(const std::filesystem::path& save_dir,
                                                   std::vector<std::string>* errors) const {
    const SaveGuard guard;
    if (!m_prepared) {
        AddError(errors, "field write has not been prepared");
        return false;
    }
    TransactionScope scope(save_dir, {world::FieldPageState::kRelativePath}, errors);
    if (!scope.ok())
        return false;
    if (!m_bytes.empty() && !scope.engaged()) {
        AddError(errors,
                 "Field pages save refused: nonempty state requires an indexed transaction");
        return false;
    }
    if (!validate_save(save_dir, errors))
        return false;
    if (m_bytes.empty()) {
        bool missing = false;
        {
            const SavedInputFile input(field_pages_path(save_dir));
            missing = input.missing();
        }
        return (missing || remove_member(save_dir, world::FieldPageState::kRelativePath, errors)) &&
               scope.commit(errors);
    }
    return write_member_bytes(save_dir, world::FieldPageState::kRelativePath, m_bytes, errors) &&
           scope.commit(errors);
}

bool WorldSaveService::save_field_pages(const world::FieldPageState& state,
                                        const std::filesystem::path& save_dir,
                                        std::vector<std::string>* errors) {
    PreparedFieldWrite prepared;
    return prepared.prepare(state, errors) && prepared.publish(save_dir, errors);
}

bool WorldSaveService::load_field_pages(world::FieldPageState& out,
                                        const std::filesystem::path& save_dir,
                                        std::vector<std::string>* errors,
                                        const std::stop_token& stop) {
    const detail::SnapshotReadOwnership ownership;
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    bool present = false;
    std::string bytes;
    {
        const SavedInputFile input(field_pages_path(save_dir));
        present = !input.missing();
        if (present && !input.read(bytes, world::FieldPageState::kMaxFileBytes, stop)) {
            AddError(errors,
                     stop.stop_requested()
                         ? "Saved-world validation cancelled."
                         : "Field pages load failed: unreadable or oversized file (64 MiB cap)");
            return false;
        }
    }
    if (!SaveTransaction::active_for(save_dir)) {
        if (snapshot_index_exists(save_dir)) {
            SnapshotIndex index;
            if (!read_snapshot_index(save_dir, index, errors, stop))
                return false;
            const auto member = std::find_if(
                index.members.begin(), index.members.end(), [](const SnapshotMember& value) {
                    return value.path == world::FieldPageState::kRelativePath;
                });
            const auto fingerprint = fnv1a64_bytes(bytes.data(), bytes.size());
            if ((member == index.members.end() && present) ||
                (member != index.members.end() &&
                 (member->present != present ||
                  (present && (member->bytes != bytes.size() || member->fnv64 != fingerprint))))) {
                AddError(errors, "inconsistent snapshot: chunks/region/field-pages.fpg");
                return false;
            }
        } else if (present) {
            AddError(errors, "inconsistent snapshot: field authority without index");
            return false;
        }
    }
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    if (!present) {
        out.clear();
        return true;
    }
    world::FieldPageState parsed;
    std::string error;
    if (!world::FieldPageState::parse(bytes, parsed, error)) {
        AddError(errors, "Field pages load failed: " + error);
        return false;
    }
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    out = std::move(parsed);
    return true;
}

namespace {
// Called only on parsed state: initialized coordinates are bounded before narrowing.
world::ScentCellKey FieldCellAt(const world::FieldPageKey& key, std::size_t index) {
    return {static_cast<std::int32_t>(static_cast<std::int64_t>(key.x) * 16 +
                                      static_cast<std::int64_t>(index % 16)),
            static_cast<std::int32_t>(static_cast<std::int64_t>(key.z) * 16 +
                                      static_cast<std::int64_t>(index / 16))};
}

template<typename Pages, typename Owner>
bool FieldPageOwnersRecorded(const Pages& pages,
                             const world::ActiveRegionLedger& ledger,
                             const Owner& owner) {
    for (const auto& [key, page] : pages) {
        for (std::size_t index = 0; index < world::FieldPage::kCells; ++index) {
            if (((page.mask[index / 64] >> (index % 64)) & std::uint64_t{1}) != 0 &&
                !ledger.records().contains(owner(FieldCellAt(key, index))))
                return false;
        }
    }
    return true;
}

bool FieldPagesMatchLedger(const world::FieldPageState& state,
                           const world::ActiveRegionLedger& ledger) {
    const auto has_owner = [&ledger](world::RegionKey key) {
        return ledger.records().contains(key);
    };
    const auto ambient_owner = [](world::ScentCellKey cell) {
        return world::FieldPageState::region_of_cell(cell.x, cell.z);
    };
    const auto metre_owner = world::FieldPageState::region_of_scent_cell;
    return FieldPageOwnersRecorded(state.pages(), ledger, ambient_owner) &&
           FieldPageOwnersRecorded(state.soil().pages(), ledger, metre_owner) &&
           FieldPageOwnersRecorded(state.irrigation().pages(), ledger, metre_owner) &&
           std::all_of(state.storms().begin(),
                       state.storms().end(),
                       [&has_owner](const auto& storm) { return has_owner(storm.owner); }) &&
           std::all_of(state.strikes().begin(),
                       state.strikes().end(),
                       [&has_owner](const auto& strike) { return has_owner(strike.owner); }) &&
           std::all_of(state.weather_clocks().begin(),
                       state.weather_clocks().end(),
                       [&has_owner](const auto& clock) { return has_owner(clock.first); }) &&
           std::all_of(state.scent_cells().begin(),
                       state.scent_cells().end(),
                       [&has_owner, metre_owner](const auto& cell) {
                           return has_owner(metre_owner(cell.first));
                       }) &&
           std::all_of(state.scent_floors().begin(),
                       state.scent_floors().end(),
                       [&has_owner, metre_owner](const auto& block) {
                           // Aligned 8 m blocks never cross a 512 m owner boundary.
                           return has_owner(metre_owner(block.first));
                       });
}
} // namespace

bool WorldSaveService::validate_field_pages(const std::filesystem::path& save_dir,
                                            const world::ActiveRegionLedger& ledger,
                                            std::vector<std::string>* errors,
                                            const std::stop_token& stop) {
    world::FieldPageState state;
    if (!load_field_pages(state, save_dir, errors, stop))
        return false;
    if (state.empty())
        return true;
    const SavedInputFile input(active_regions_path(save_dir));
    if (input.missing() || !FieldPagesMatchLedger(state, ledger)) {
        AddError(errors, "corrupt field pages: orphaned owner");
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
        if (stop.stop_requested()) {
            AddError(errors, "Saved-world validation cancelled.");
            return false;
        }
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
    bool has_ledger =
        !ledger.records().empty() || ledger.local_anchor() || ledger.next_persistent_id() != 1;
    {
        // Close the admitted reader before transaction publication takes its lock.
        const SavedInputFile input(active_regions_path(save_dir));
        has_ledger = has_ledger || !input.missing();
    }
    TransactionScope scope(
        save_dir, {"chunks/region/active-regions.arl"}, errors, false, artifacts);
    if (!scope.ok() || !validate_save(save_dir, errors))
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
    if (!artifacts)
        ReclaimAbandonedStaging(staging);
    // Keep an anchor-only ledger: it restores last walking feet before activation.
    if (!has_ledger)
        return true;
    auto saved = ledger;
    saved.mark_saved(clock);
    std::string bytes;
    if (!EncodeActiveRegionsForSave(saved, bytes, errors))
        return false;
    world::ActiveRegionLedger checked;
    std::string error;
    if (!world::ActiveRegionLedger::decode(bytes, checked, error) ||
        ledger.header_tick() > clock.tick()) {
        AddError(errors, world::ActiveRegionLedger::kCorruptMessage);
        return false;
    }
    try {
        const auto path = active_regions_path(save_dir);
        if (artifacts) {
            artifacts->create_directory(path.parent_path().parent_path());
            artifacts->create_directory(path.parent_path());
            artifacts->reserve_file(path);
        } else {
            std::filesystem::create_directories(path.parent_path());
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
            if (artifacts)
                temporary.record_payload(*artifacts);
            try {
                replaced =
                    ReplaceMember(temporary.path(), path, errors, artifacts, &temporary.payload());
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

RegionDigestOperation::RegionDigestOperation(const std::filesystem::path& save_dir,
                                             bool require_index)
    : m_save_dir(save_dir)
    , m_publication_revision(PublicationRevision().load(std::memory_order_relaxed))
    , m_require_index(require_index) {
    const detail::SnapshotReadOwnership ownership;
    if (save_dir.empty())
        return;
    WorldSaveService::SaveFormat format;
    std::vector<std::string> errors;
    ++m_index_reads;
    if (!WorldSaveService::read_save_format(save_dir, format, &errors, {}, &m_index) ||
        (require_index && !format.indexed))
        throw std::runtime_error(errors.empty() ? "inconsistent snapshot: index missing"
                                                : errors.front());
}

std::uint64_t RegionDigestOperation::digest(world::RegionKey key,
                                            const RegionDigestInputs& inputs) {
    const detail::SnapshotReadOwnership ownership;
    const auto revision = PublicationRevision().load(std::memory_order_relaxed);
    if (!m_save_dir.empty() && revision != m_publication_revision) {
        WorldSaveService::SaveFormat format;
        std::vector<std::string> errors;
        ++m_index_reads;
        if (!WorldSaveService::read_save_format(m_save_dir, format, &errors, {}, &m_index) ||
            (m_require_index && !format.indexed))
            throw std::runtime_error(errors.empty() ? "inconsistent snapshot: index missing"
                                                    : errors.front());
        m_publication_revision = revision;
    }
    m_input_items += inputs.live.size() + inputs.parked.size() + inputs.durable.size();
    return WorldSaveService::region_simulation_digest(m_save_dir, key, inputs, &m_index);
}

namespace {
using DigestVersions = std::array<SavedInputFile::Version, 4>;

bool ReadDigestVersions(const std::filesystem::path& save_dir,
                        const std::filesystem::path& region_path,
                        DigestVersions& versions) {
    const std::array paths{region_path,
                           save_dir / "world_info.json",
                           WorldSaveService::world_manifest_path(save_dir),
                           snapshot_index_path(save_dir)};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const SavedInputFile input(paths[i]);
        if (input.missing())
            versions[i] = {};
        else if (!input.version(versions[i]))
            return false;
    }
    return true;
}

std::array<std::uint64_t, 5> DigestMembership(const SnapshotIndex* index,
                                              const world::RegionKey& key) {
    if (!index)
        return {};
    const auto relative = RelativeRegionMember(key.x, key.z);
    const auto found = std::lower_bound(
        index->members.begin(),
        index->members.end(),
        relative,
        [](const SnapshotMember& member, const std::string& path) { return member.path < path; });
    if (found == index->members.end() || found->path != relative)
        return {1, index->generation, 0, 0, 0};
    return {1, index->generation, found->present ? 2u : 1u, found->bytes, found->fnv64};
}

struct DigestAuthorityCache {
    struct Entry {
        DigestVersions versions;
        std::array<std::uint64_t, 5> membership;
        std::vector<std::shared_ptr<Chunk>> chunks;
        std::size_t bytes = 0;
    };
    std::uint64_t revision = 0;
    std::size_t bytes = 0;
    std::map<std::filesystem::path, Entry> regions;
};

bool ReadDigestAuthority(const std::filesystem::path& save_dir,
                         const world::RegionKey& key,
                         std::vector<std::shared_ptr<Chunk>>& chunks,
                         std::vector<std::string>* errors,
                         const SnapshotIndex* admitted_index) {
    static thread_local DigestAuthorityCache cache;
    constexpr std::size_t kMaxRegions = 16;
    constexpr std::size_t kMaxBytes = std::size_t{256} * 1024 * 1024;
    const auto revision = PublicationRevision().load(std::memory_order_relaxed);
    if (cache.revision != revision) {
        cache = {};
        cache.revision = revision;
    }
    const auto path = WorldSaveService::region_file_path(save_dir, key.x, key.z);
    DigestVersions versions{};
    const bool versioned = ReadDigestVersions(save_dir, path, versions);
    const auto membership = DigestMembership(admitted_index, key);
    if (const auto found = cache.regions.find(path); found != cache.regions.end()) {
        if (versioned && found->second.versions == versions &&
            found->second.membership == membership) {
            chunks = found->second.chunks;
            return true;
        }
        cache.bytes -= found->second.bytes;
        cache.regions.erase(found);
    }
    constexpr auto span = WorldSaveService::kRegionChunkSpan;
    if (!WorldSaveService::read_authoritative_region_chunks(path,
                                                            key.x * span,
                                                            key.x * span + span - 1,
                                                            key.z * span,
                                                            key.z * span + span - 1,
                                                            chunks,
                                                            errors,
                                                            admitted_index))
        return false;
    DigestVersions after{};
    if (!versioned || !ReadDigestVersions(save_dir, path, after) || versions != after)
        return true;
    std::size_t bytes = 0;
    for (const auto& chunk : chunks)
        bytes += sizeof(Chunk) + chunk->estimated_heap_bytes();
    if (bytes > kMaxBytes)
        return true;
    while (!cache.regions.empty() &&
           (cache.regions.size() >= kMaxRegions || cache.bytes + bytes > kMaxBytes)) {
        cache.bytes -= cache.regions.begin()->second.bytes;
        cache.regions.erase(cache.regions.begin());
    }
    cache.bytes += bytes;
    cache.regions.emplace(path, DigestAuthorityCache::Entry{versions, membership, chunks, bytes});
    return true;
}
} // namespace

std::uint64_t WorldSaveService::region_simulation_digest(const std::filesystem::path& save_dir,
                                                         world::RegionKey key,
                                                         const RegionDigestInputs& inputs,
                                                         const SnapshotIndex* admitted_index) {
    const detail::SnapshotReadOwnership ownership;
    ValidateRegionDigestKey(key);
    WorldStreamingState region;
    // Filtering precedes insertion: a live pristine or Loading chunk must not shadow a durable
    // edited record with the same id. insert_chunk preserves the first record per id.
    const auto qualifies = [&key](const std::shared_ptr<Chunk>& chunk) {
        if (!chunk || !chunk->has_simulation_authority())
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
        if (inputs.require_index && !inputs.durable.empty() &&
            (admitted_index ? admitted_index->generation == 0 : !snapshot_index_exists(save_dir)))
            throw std::runtime_error("inconsistent snapshot: index missing");
        std::vector<std::shared_ptr<Chunk>> durable_chunks;
        std::vector<std::string> errors;
        if (!ReadDigestAuthority(save_dir, key, durable_chunks, &errors, admitted_index))
            throw std::runtime_error(errors.empty() ? "Cannot read durable region records"
                                                    : errors.front());
        for (const auto& chunk : durable_chunks) {
            // Durable edited content is complete even if its saved lifecycle was Loading.
            if (qualifies(chunk))
                region.insert_chunk(chunk);
        }
    }
    std::string composed = ComputeRegionContentHash(region);
    if (!inputs.ground_object_bytes.empty())
        composed = StableChecksum(composed + "|ground:" + inputs.ground_object_bytes);
    if (!inputs.field_page_bytes.empty())
        composed = StableChecksum(composed + "|pages:" + inputs.field_page_bytes);
    return std::stoull(composed, nullptr, 16);
}

std::uint64_t
WorldSaveService::region_simulation_digest(const std::filesystem::path& save_dir,
                                           world::RegionKey key,
                                           const std::vector<std::shared_ptr<Chunk>>& live_chunks) {
    ValidateRegionDigestKey(key);
    // This compatibility overload projects chunks only. Durable chunks are read from save_dir;
    // callers with live ground objects or field pages supply those projections explicitly.
    return region_simulation_digest(
        save_dir,
        key,
        RegionDigestInputs{
            .live = live_chunks, .parked = {}, .durable = {}, .ground_object_bytes = {}});
}

std::uint64_t WorldSaveService::region_simulation_digest(
    const std::filesystem::path& save_dir,
    world::RegionKey key,
    std::initializer_list<std::shared_ptr<Chunk>> live_chunks) {
    ValidateRegionDigestKey(key);
    return region_simulation_digest(
        save_dir, key, std::vector<std::shared_ptr<Chunk>>(live_chunks));
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
        return false;
    ec.clear();
    std::filesystem::path b = std::filesystem::weakly_canonical(rhs, ec);
    if (ec)
        return false;
    return a == b;
}
} // namespace

WorldSaveService::WorldReadScope::WorldReadScope() {
    if (!PendingChunkAdmissions()) {
        PendingChunkAdmissions().emplace();
        m_owner = true;
    }
}

WorldSaveService::WorldReadScope::~WorldReadScope() {
    if (m_owner) {
        auto& pending = PendingChunkAdmissions();
        if (pending) {
            for (const auto& [id, admission] : pending->admissions()) {
                (void)id;
                AdmittedChunkRecordCache().remember_decoded(ChunkRecordKeyOf(admission.record),
                                                            admission.chunk);
            }
        }
        pending.reset();
    }
}

WorldSaveService::SaveFactsScope::SaveFactsScope(const std::filesystem::path& save_dir)
    : m_save_dir(save_dir)
    , m_previous(ActiveFacts()) {
    if (auto* previous = active_for(save_dir)) {
        m_validated = previous->m_validated;
        m_validated_ok = previous->m_validated_ok;
        m_diagnostics = previous->m_diagnostics;
        m_presence_known = previous->m_presence_known;
        m_snapshot_present = previous->m_snapshot_present;
        m_chunk_snapshot_present = previous->m_chunk_snapshot_present;
    }
    ActiveFacts() = this;
}

WorldSaveService::SaveFactsScope::~SaveFactsScope() {
    ActiveFacts() = m_previous;
}

WorldSaveService::SaveFactsScope*
WorldSaveService::SaveFactsScope::active_for(const std::filesystem::path& save_dir) {
    for (SaveFactsScope* scope = ActiveFacts(); scope != nullptr; scope = scope->m_previous) {
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
        const std::unordered_set<ChunkID> retain_none;
        std::size_t decoded_chunks = 0;
        m_snapshot_present = WorldSaveService{}.load_world(
            state, m_save_dir, m_diagnostics, {}, &retain_none, nullptr, &decoded_chunks);
        m_chunk_snapshot_present = m_snapshot_present && decoded_chunks != 0;
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
        if (facts->presence_known())
            return facts->snapshot_present();
    }
    WorldStreamingState state;
    std::vector<std::string> errors;
    const std::unordered_set<ChunkID> retain_none;
    return WorldSaveService{}.load_world(state, save_dir, errors, {}, &retain_none);
}

bool WorldSaveService::has_chunk_snapshot(const std::filesystem::path& save_dir) {
    if (SaveFactsScope* facts = SaveFactsScope::active_for(save_dir)) {
        facts->validate(nullptr);
        if (facts->presence_known())
            return facts->chunk_snapshot_present();
    }
    WorldStreamingState state;
    std::vector<std::string> errors;
    const std::unordered_set<ChunkID> retain_none;
    std::size_t decoded_chunks = 0;
    return WorldSaveService{}.load_world(
               state, save_dir, errors, {}, &retain_none, nullptr, &decoded_chunks) &&
           decoded_chunks != 0;
}

bool WorldSaveService::validate_save(const std::filesystem::path& save_dir,
                                     std::vector<std::string>* errors,
                                     const std::stop_token& stop) {
    if (SaveFactsScope* facts = SaveFactsScope::active_for(save_dir))
        return facts->validate(errors);
    WorldStreamingState state;
    std::vector<std::string> diagnostics;
    const std::unordered_set<ChunkID> retain_none;
    WorldSaveService{}.load_world(state, save_dir, diagnostics, stop, &retain_none);
    for (const auto& message : diagnostics)
        AddError(errors, message);
    return diagnostics.empty();
}

bool WorldSaveService::read_container_records(const std::filesystem::path& region_file,
                                              std::vector<ContainerRecord>& out_records,
                                              std::vector<std::string>* errors) {
    const detail::SnapshotReadOwnership ownership;
    out_records.clear();
    std::vector<RegionRecord> raw_records;
    if (!ReadRegionFile(region_file, raw_records, errors, {}, true)) {
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
    const detail::SnapshotReadOwnership ownership;
    out_chunks.clear();
    std::vector<RegionRecord> raw_records;
    if (!ReadRegionFile(region_file, raw_records, errors, {}, true) ||
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
    std::vector<std::string>* errors,
    const SnapshotIndex* admitted_index) {
    const detail::SnapshotReadOwnership ownership;
    out_chunks.clear();
    if (min_chunk_x > max_chunk_x || min_chunk_z > max_chunk_z) {
        AddError(errors, "authoritative chunk read bounds are invalid");
        return false;
    }
    std::vector<RegionRecord> raw_records;
    if (!ReadRegionFile(region_file, raw_records, errors, {}, true) ||
        !VerifyRegionMembership(region_file, raw_records, errors, admitted_index)) {
        return false;
    }
    for (const RegionRecord& record : raw_records) {
        if (record.lod_level != 0u)
            continue;
        const IVec3 indexed_coords = Chunk::decode_id(record.id);
        if (indexed_coords.x < min_chunk_x || indexed_coords.x > max_chunk_x ||
            indexed_coords.z < min_chunk_z || indexed_coords.z > max_chunk_z) {
            continue;
        }
        std::shared_ptr<Chunk> chunk;
        if ((record.flags & kRecordFlagEdited) != 0u) {
            chunk = DecodeChunkRecord(record, region_file, errors);
        } else {
            ChunkRecordFacts facts;
            if (!AdmitChunkRecord(record, region_file, errors, facts, nullptr, false, true)) {
                out_chunks.clear();
                return false;
            }
            if (!facts.loaded_or_edited)
                continue;
            if (!AdmitChunkRecord(record, region_file, errors, facts, &chunk) || !chunk) {
                out_chunks.clear();
                return false;
            }
            if (!chunk->has_water_authority())
                continue;
        }
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
    return upsert_container_records(guard, region_file, records, errors);
}

bool WorldSaveService::upsert_container_records(const SaveOwnership&,
                                                const std::filesystem::path& region_file,
                                                const std::vector<ContainerRecord>& records,
                                                std::vector<std::string>* errors) {
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
        std::unique_ptr<TransactionScope> scope;
        std::filesystem::path scoped_save_dir;
        std::string scoped_member;
        const bool has_lod0 = std::any_of(records.begin(), records.end(), [](const auto& record) {
            return record.lod_level == 0;
        });
        if (has_lod0 && SnapshotMemberForRegionFile(region_file, scoped_save_dir, scoped_member)) {
            scope = std::make_unique<TransactionScope>(
                scoped_save_dir, std::vector<std::string>{scoped_member}, errors);
            if (!scope->ok())
                return false;
        }
        std::vector<RegionRecord> raw_records;
        if (!ReadRegionFile(region_file, raw_records, errors, {}, true))
            return false;

        std::map<std::pair<std::uint8_t, std::uint64_t>, std::size_t> record_index;
        for (std::size_t i = 0; i < raw_records.size(); ++i) {
            record_index[{raw_records[i].lod_level, raw_records[i].id}] = i;
        }

        bool changed = raw_records.empty() ||
                       !std::is_sorted(raw_records.begin(),
                                       raw_records.end(),
                                       [](const RegionRecord& lhs, const RegionRecord& rhs) {
                                           return std::pair{lhs.lod_level, lhs.id} <
                                                  std::pair{rhs.lod_level, rhs.id};
                                       });
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
                changed = changed ||
                          ChunkRecordKeyOf(raw_records[existing->second]) != ChunkRecordKeyOf(raw);
                raw_records[existing->second] = std::move(raw);
            } else {
                changed = true;
                record_index[{raw.lod_level, raw.id}] = raw_records.size();
                raw_records.push_back(std::move(raw));
            }
        }

        if (!AdmitRegionWrite(raw_records, errors))
            return false;
        if (!changed) {
            ReclaimAbandonedStaging(region_staging_path(region_file));
            return !scope || scope->commit(errors);
        }
        if (!parent.empty())
            std::filesystem::create_directories(parent);
        return WriteRegionFile(region_file, raw_records, errors) &&
               (!scope || scope->commit(errors));
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

std::uint64_t WorldSaveService::record_compression_count_for_testing() {
    return RecordCompressionCounter().load(std::memory_order_relaxed);
}

std::uint64_t WorldSaveService::chunk_record_decode_count_for_testing() {
    return ChunkRecordDecodeCounter().load(std::memory_order_acquire);
}

std::uint64_t WorldSaveService::region_fingerprint_count_for_testing() {
    return RegionFingerprintCounter().load(std::memory_order_acquire);
}

std::uint64_t WorldSaveService::region_read_count_for_testing() {
    return RegionReadCounter().load(std::memory_order_relaxed);
}

std::size_t WorldSaveService::pending_chunk_admission_count_for_testing() {
    const auto& pending = PendingChunkAdmissions();
    return pending ? pending->size() : 0;
}

std::size_t WorldSaveService::pending_chunk_admission_bytes_for_testing() {
    const auto& pending = PendingChunkAdmissions();
    return pending ? pending->bytes() : 0;
}

void WorldSaveService::set_pending_chunk_admission_byte_limit_for_testing(std::size_t bytes) {
    PendingChunkAdmissionByteLimit().store(bytes, std::memory_order_relaxed);
}

bool WorldSaveService::audit_admitted_chunk_records_for_testing(
    const std::filesystem::path& region_file, AdmittedChunkRecordAudit& audit) {
    audit = {};
    std::vector<RegionRecord> records;
    std::vector<std::string> errors;
    if (!ReadRegionFile(region_file, records, &errors))
        return false;
    for (const RegionRecord& record : records) {
        if (record.lod_level != 0)
            continue;
        ++audit.records;
        const auto admitted = AdmittedChunkRecordCache().find(ChunkRecordKeyOf(record));
        const auto chunk = DecodeChunkRecord(record, region_file, &errors);
        if (!admitted)
            continue;
        ++audit.admitted;
        if (!chunk) {
            ++audit.mismatched;
            continue;
        }
        const ChunkRecordFacts decoded = DecodedChunkRecordFacts(*chunk);
        if (admitted->coords != decoded.coords || admitted->lattice_ok != decoded.lattice_ok ||
            admitted->loaded_or_edited != decoded.loaded_or_edited)
            ++audit.mismatched;
    }
    return true;
}

bool WorldSaveService::durable_flush_directory(const std::filesystem::path& directory,
                                               std::vector<std::string>* errors) {
#if defined(_WIN32)
    // Windows has no supported FlushFileBuffers operation on an ordinary directory
    // handle. Protected publications flush the bound published file after linking;
    // other replacements use write-through moves. Undo bytes are flushed first. Native power-loss
    // qualification remains a separate requirement for each supported filesystem/storage stack.
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
                 "failed to open directory for durable flush: " +
                     Filesystem::GenericUtf8PathString(directory) + ": " +
                     PosixErrorMessage(error));
        return false;
    }
    while (::fsync(fd) != 0) {
        if (errno == EINTR)
            continue;
        const int error = errno;
        ::close(fd);
        AddError(errors,
                 "directory flush failed: " + Filesystem::GenericUtf8PathString(directory) + ": " +
                     PosixErrorMessage(error));
        return false;
    }
    if (::close(fd) != 0) {
        const int error = errno;
        AddError(errors,
                 "directory close failed: " + Filesystem::GenericUtf8PathString(directory) + ": " +
                     PosixErrorMessage(error));
        return false;
    }
    return true;
#endif
}

bool WorldSaveService::durable_write_file(const std::filesystem::path& destination,
                                          const std::string& bytes,
                                          std::vector<std::string>* errors,
                                          const std::filesystem::path& staging,
                                          ArtifactIdentity* published_identity) {
    std::filesystem::path temp;
    if (!WriteDurableRegionTemp(staging.empty() ? destination : staging,
                                bytes,
                                temp,
                                errors,
                                nullptr,
                                published_identity))
        return false;
    if (!ReplaceMember(temp, destination, errors)) {
        RemoveTemporaryFile(temp);
        return false;
    }
    return true;
}

bool WorldSaveService::append_journal_record(const std::filesystem::path& path,
                                             const std::string& record,
                                             ArtifactIdentity& expected,
                                             std::vector<std::string>* errors,
                                             std::size_t rollback_reserve) {
    const SaveOwnership ownership;
    if (record.empty() || record.back() != '\n' || rollback_reserve > kMaxSnapshotIndexBytes ||
        record.size() > kMaxSnapshotIndexBytes - rollback_reserve) {
        AddError(errors, "invalid snapshot journal record");
        return false;
    }
    if (expected == ArtifactIdentity{}) {
        // The initial header has no published prefix to fall back to. Stage it
        // in neutral root staging and publish only the complete, flushed file.
        std::filesystem::path temporary;
        ArtifactIdentity identity{};
        if (!prepare_restoration(path.parent_path().parent_path() / path.filename(),
                                 record,
                                 temporary,
                                 identity,
                                 errors))
            return false;
        report_snapshot_step("crash-control-staged", temporary);
        ArtifactIdentity staging_directory{};
        bool missing = false;
        if (!artifact_identity(temporary.parent_path(), staging_directory, missing, true) ||
            missing)
            return false;
        const BoundPublicationStage stage{
            .directory = temporary.parent_path(),
            .name = temporary.filename(),
            .directory_identity = staging_directory,
            .report = +[](const char* step, const std::filesystem::path& target) {
                report_snapshot_step(std::string_view(step) == "publication-quarantined"
                                         ? "crash-control-bound"
                                         : step,
                                     target);
            }};
        expected = identity;
        bool installed = false;
        {
            const auto publication = LockPublication(path);
            StagingFile source(temporary, StagingFile::Mode::Inspect);
            StagingIdentity actual{};
            installed = source.identity(actual) &&
                        std::equal(identity.begin(), identity.end(), actual.begin()) &&
                        source.publish_bound(path, &stage);
        }
        if (!installed) {
            AddError(errors, "cannot publish initial snapshot journal");
            return false;
        }
        const bool flushed = durable_flush_directory(path.parent_path(), errors) &&
                             durable_flush_directory(temporary.parent_path(), errors);
        if (flushed)
            report_snapshot_step("crash-control-published", path);
        return flushed;
    }
    const auto publish = LockPublication(path);
    // Fixed 64 KiB tail buffer; both existing bytes and appends share the 16 MiB bound.
    std::array<char, 65536> tail{};
#if defined(_WIN32)
    const HANDLE file = ::CreateFileW(path.c_str(),
                                      GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
                                      nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        AddError(errors, "cannot open owned snapshot journal");
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER length{};
    bool ok =
        ::GetFileInformationByHandle(file, &info) &&
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
        ::GetFileSizeEx(file, &length) && length.QuadPart >= 0;
    const ArtifactIdentity actual{
        info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
    ok = ok && (actual[1] != 0 || actual[2] != 0) && actual == expected;
    const auto size =
        ok ? static_cast<std::uint64_t>(length.QuadPart) : kMaxSnapshotIndexBytes + 1ull;
    const auto seek = [&](std::uint64_t offset) {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        return ::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != 0;
    };
    const auto read_tail = [&](std::uint64_t offset, std::size_t count) {
        DWORD received = 0;
        return seek(offset) &&
               ::ReadFile(file, tail.data(), static_cast<DWORD>(count), &received, nullptr) &&
               received == count;
    };
    const auto truncate_tail = [&](std::uint64_t offset) {
        return seek(offset) && ::SetEndOfFile(file);
    };
    const auto write_bytes = [&](const char* bytes, std::size_t count) {
        std::size_t offset = 0;
        while (offset < count) {
            DWORD written = 0;
            const auto requested = static_cast<DWORD>(std::min<std::size_t>(65536, count - offset));
            if (!::WriteFile(file, bytes + offset, requested, &written, nullptr) || written == 0)
                return false;
            offset += written;
        }
        return true;
    };
    const auto flush = [&]() {
        return ::FlushFileBuffers(file) != 0;
    };
    const auto close = [&]() {
        return ::CloseHandle(file) != 0;
    };
#else
    const int file = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (file < 0) {
        AddError(errors, "cannot open owned snapshot journal");
        return false;
    }
    struct stat info {};
    bool ok =
        ::fstat(file, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 && info.st_ino != 0;
    const ArtifactIdentity actual{
        static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino), 0};
    ok = ok && actual == expected;
    const auto size = ok ? static_cast<std::uint64_t>(info.st_size) : kMaxSnapshotIndexBytes + 1ull;
    const auto read_tail = [&](std::uint64_t offset, std::size_t count) {
        std::size_t received = 0;
        while (received < count) {
            const auto result = ::pread(file,
                                        tail.data() + received,
                                        count - received,
                                        static_cast<off_t>(offset + received));
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0)
                return false;
            received += static_cast<std::size_t>(result);
        }
        return true;
    };
    const auto truncate_tail = [&](std::uint64_t offset) {
        return ::ftruncate(file, static_cast<off_t>(offset)) == 0;
    };
    const auto seek = [&](std::uint64_t offset) {
        return ::lseek(file, static_cast<off_t>(offset), SEEK_SET) >= 0;
    };
    const auto write_bytes = [&](const char* bytes, std::size_t count) {
        std::size_t offset = 0;
        while (offset < count) {
            const auto written =
                ::write(file, bytes + offset, std::min<std::size_t>(65536, count - offset));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return false;
            offset += static_cast<std::size_t>(written);
        }
        return true;
    };
    const auto flush = [&]() {
        int result = -1;
        do {
            result = ::fsync(file);
        } while (result != 0 && errno == EINTR);
        return result == 0;
    };
    const auto close = [&]() {
        return ::close(file) == 0;
    };
#endif
    const auto report = [&](const char* step) {
        try {
            report_snapshot_step(step, path);
        } catch (...) {
            close();
            throw;
        }
    };
    ok = ok && size <= kMaxSnapshotIndexBytes;
    std::uint64_t published = 0;
    if (ok && size != 0) {
        ok = read_tail(size - 1, 1);
        if (ok && tail.front() == '\n')
            published = size;
    }
    for (std::uint64_t end = size; ok && end != 0 && published == 0;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(end, tail.size()));
        const auto begin = end - count;
        ok = read_tail(begin, count);
        if (ok) {
            for (auto position = count; position != 0; --position) {
                if (tail[position - 1] == '\n') {
                    published = begin + position;
                    break;
                }
            }
        }
        end = begin;
    }
    // Never append past an unpublished tail, including during interrupted rollback.
    ok = ok && published != 0 && rollback_reserve <= kMaxSnapshotIndexBytes - published &&
         record.size() <= kMaxSnapshotIndexBytes - published - rollback_reserve;
    if (ok && published != size) {
        ok = truncate_tail(published);
        if (ok)
            report("crash-record-tail-truncated");
    }
    ok = ok && seek(published);
    // Publish the delimiter only after the body is durable. A torn write can
    // therefore discard only this record, whose mutation has not started.
    ok = ok && write_bytes(record.data(), record.size() - 1) && flush();
    if (ok)
        report("crash-record-body-flushed");
    ok = ok && write_bytes("\n", 1) && flush();
    const bool closed = close();
    if (!ok || !closed) {
        AddError(errors, "snapshot journal append failed or exceeded control byte limit");
        return false;
    }
    report_snapshot_step("crash-record-published", path);
    return true;
}

bool WorldSaveService::artifact_identity(const std::filesystem::path& path,
                                         ArtifactIdentity& result,
                                         bool& missing,
                                         bool directory,
                                         unsigned max_links) {
    result = {};
    missing = false;
    StagingFile bound(path, directory ? StagingFile::Mode::Directory : StagingFile::Mode::Inspect);
    StagingIdentity identity{};
    if (bound.identity(identity, directory, max_links)) {
        std::copy_n(identity.begin(), result.size(), result.begin());
        return true;
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    missing = error == std::errc::no_such_file_or_directory ||
              (!error && !std::filesystem::exists(status));
    return missing;
}

bool WorldSaveService::artifact_version(const std::filesystem::path& path,
                                        std::array<std::uint64_t, 8>& version,
                                        bool& missing) {
    version = {};
    missing = false;
    const StagingFile bound(path, StagingFile::Mode::Inspect);
    if (bound.version(version))
        return true;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    missing = error == std::errc::no_such_file_or_directory ||
              (!error && !std::filesystem::exists(status));
    return missing;
}

bool WorldSaveService::remove_owned_entry(const std::filesystem::path& path,
                                          const ArtifactIdentity& expected,
                                          std::vector<std::string>* errors,
                                          bool directory,
                                          const BoundPublicationStage* stage) {
    const SaveOwnership ownership;
    const auto publish = LockPublication(path);
    PublicationRevision().fetch_add(1, std::memory_order_relaxed);
    StagingFile bound(path, directory ? StagingFile::Mode::Directory : StagingFile::Mode::Inspect);
    StagingIdentity identity{};
    if (!bound.identity(identity, directory, std::numeric_limits<unsigned>::max()) ||
        !std::equal(expected.begin(), expected.end(), identity.begin()) ||
        !bound.remove_bound(directory, stage)) {
        AddError(errors,
                 "snapshot artifact ownership changed: " + Filesystem::GenericUtf8PathString(path));
        return false;
    }
    const bool flushed = durable_flush_directory(path.parent_path(), errors) &&
                         (!stage || stage->directory == path.parent_path() ||
                          durable_flush_directory(stage->directory, errors));
    if (flushed)
        report_snapshot_step("crash-artifact-removed", path);
    return flushed;
}

bool WorldSaveService::prepare_restoration(const std::filesystem::path& staging,
                                           const std::string& bytes,
                                           std::filesystem::path& temp,
                                           ArtifactIdentity& identity,
                                           std::vector<std::string>* errors) {
    return WriteDurableRegionTemp(staging, bytes, temp, errors, nullptr, &identity);
}

bool WorldSaveService::publish_restoration(const std::filesystem::path& temp,
                                           const std::filesystem::path& destination,
                                           const ArtifactIdentity& expected,
                                           const ArtifactIdentity& incoming,
                                           std::vector<std::string>* errors,
                                           const BoundPublicationStage* source_stage,
                                           const BoundPublicationStage* retired_stage) {
    const SaveOwnership ownership;
    report_snapshot_step("before-replace", destination);
    PublicationRevision().fetch_add(1, std::memory_order_relaxed);
    {
        const auto publication = LockPublication(destination);
        if (!PublishProtectedMember(
                temp, destination, expected, incoming, errors, source_stage, retired_stage))
            return false;
    }
    if (!durable_flush_directory(destination.parent_path(), errors) ||
        (temp.parent_path() != destination.parent_path() &&
         !durable_flush_directory(temp.parent_path(), errors)))
        return false;
    report_snapshot_step("after-replace", destination);
    return true;
}

bool WorldSaveService::durable_remove(const std::filesystem::path& path,
                                      std::vector<std::string>* errors) {
    const SaveGuard guard;
    if (!SaveTransaction::prepare_publication(path, {}, errors))
        return false;
    const auto publish = LockPublication(path);
    if (!AllowPublication(path, true, errors))
        return false;
    StagingFile retained(path, StagingFile::Mode::Inspect);
    StagingIdentity expected{};
    if (!retained.identity(expected)) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error && !std::filesystem::exists(status)))
            return true;
        AddError(errors,
                 "cannot bind removal identity: " + Filesystem::GenericUtf8PathString(path));
        return false;
    }
    report_snapshot_step("before-delete", path);
    PublicationRevision().fetch_add(1, std::memory_order_relaxed);
    if (!SaveTransaction::check_publication_target(path, errors))
        return false;
    if (ActiveFacts() != nullptr)
        ActiveFacts()->invalidate_presence_facts();
    // Windows dispositions target the verified handle; POSIX uses a retained-parent
    // quarantine and restores a substituted entry instead of unlinking it.
    BoundPublicationStage unused_stage, retired_stage;
    const auto* stage = SaveTransaction::publication_stages(path, unused_stage, retired_stage)
                            ? &retired_stage
                            : nullptr;
    if (!retained.remove(1, stage)) {
        AddError(errors, "cannot retire removed payload identity");
        return false;
    }
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    if (!durable_flush_directory(parent, errors) ||
        (stage && !durable_flush_directory(stage->directory, errors)))
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
            if (!ReadRegionFile(path, records, &diagnostics, {}, true)) {
                error = first_error("unreadable region file");
                return false;
            }
            return DigestRegionRecords(path, records, out, error);
        }

        std::uint64_t maximum = SavedInputLimits::kManifestBytes;
        if (relative == "chunks/region/plant-entities.json")
            maximum = SavedInputLimits::kPlantFileBytes;
        else if (relative == "chunks/region/ground-objects.json")
            maximum = world::GroundObjectStore::kMaxFileBytes;
        else if (relative == world::FieldPageState::kRelativePath)
            maximum = world::FieldPageState::kMaxFileBytes;
        else if (relative == "chunks/region/active-regions.arl")
            maximum = world::ActiveRegionLedger::kMaxFileBytes;
        else if (relative == "aether_state.efs")
            maximum = luminumbra::fields::kEnergyReadRecordBytes;
        std::string member_bytes;
        {
            const SavedInputFile input(path);
            if (input.missing())
                return true;
            if (!input.read(member_bytes, maximum)) {
                error = "unreadable or oversized snapshot member";
                return false;
            }
        }
        nlohmann::json document;
        if (relative == "world_info.json" || relative == "chunks/region/world-manifest.json") {
            if (!SavedInputLimits::JsonDepthAllowed(member_bytes) ||
                !AdmitMetadataText(member_bytes)) {
                error = "snapshot JSON compatibility limit";
                return false;
            }
            document = nlohmann::json::parse(member_bytes, nullptr, false);
            if (document.is_discarded()) {
                error = "corrupt snapshot JSON";
                return false;
            }
        }
        if (relative == "world_info.json") {
            const std::string message = CheckMetadataValue(document);
            if (!message.empty()) {
                error = message;
                return false;
            }
            // The installed ledger may never be ahead of the metadata clock (load_world refuses
            // that pair), so a standalone metadata rewrite must not publish it.
            if (std::filesystem::exists(active_regions_path(save_dir))) {
                world::ActiveRegionLedger ledger;
                world::WorldClock clock;
                if (load_active_regions(ledger, save_dir, &diagnostics) &&
                    world::WorldClock::from_metadata(document, clock, error) &&
                    ledger.header_tick() > clock.tick()) {
                    error = world::ActiveRegionLedger::kCorruptMessage;
                    return false;
                }
            }
        } else if (relative == "chunks/region/world-manifest.json") {
            const std::string message = CheckManifestValue(document);
            if (!message.empty()) {
                error = message;
                return false;
            }
        } else if (relative == "chunks/region/plant-entities.json") {
            Ecs::EntityRegistrySnapshot plants;
            if (!AdmitPlantComponentBytes(member_bytes) ||
                !Ecs::LoadEntityRegistrySnapshotJson(member_bytes, plants, diagnostics) ||
                !luminumbra::foliage::ValidatePlantEntitySnapshot(plants, false)) {
                error = first_error("corrupt plant entities");
                return false;
            }
        } else if (relative == "chunks/region/ground-objects.json") {
            world::GroundObjectStore objects;
            const std::uint64_t no_id_limit = std::numeric_limits<std::uint64_t>::max();
            if (!world::GroundObjectStore::parse(member_bytes, no_id_limit, objects, error)) {
                return false;
            }
        } else if (relative == world::FieldPageState::kRelativePath) {
            world::FieldPageState pages;
            if (!world::FieldPageState::parse(member_bytes, pages, error)) {
                error = "Field pages load failed: " + error;
                return false;
            }
        } else if (relative == "aether_state.efs") {
            const auto& bytes = member_bytes;
            std::istringstream header(bytes);
            std::string magic;
            int channels = 0;
            // Bound channel allocation before invoking the legacy decoder. Indexed writers
            // emit canonical EFS1; re-encoding also refuses duplicate pages/cells and junk.
            if (!(header >> magic >> channels) || magic != "EFS1" || channels < 1 ||
                channels > luminumbra::fields::kEnergyRecordChannels) {
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
            if (!world::ActiveRegionLedger::decode(member_bytes, ledger, error)) {
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

        out.bytes = member_bytes.size();
        out.fnv64 = fnv1a64_bytes(member_bytes.data(), member_bytes.size());
        out.present = true;
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
    const SaveGuard guard;
    if (!member_path_allowed(relative_path)) {
        AddError(errors, "member path is not allowed: " + std::string(relative_path));
        return false;
    }
    const std::string relative(relative_path);
    if (SaveTransaction::active_for(save_dir) &&
        !SaveTransaction::check_publication_target(save_dir / relative, errors))
        return false;
    TransactionScope scope(save_dir, {}, errors);
    if (!scope.ok())
        return false;
    const std::filesystem::path destination = save_dir / relative;
    ReclaimAbandonedStaging(scope.engaged() ? save_dir / destination.filename() : destination);
    if (FileBytesEqual(destination, bytes))
        return scope.commit(errors);
    if (auto* transaction = SaveTransaction::active_for(save_dir);
        transaction && !transaction->protect({relative}, errors))
        return false;
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    // Before registration, staging must be outside every strict save inventory.
    const auto staging = scope.engaged() ? save_dir / destination.filename() : destination;
    return durable_write_file(destination, bytes, errors, staging) && scope.commit(errors);
}

bool WorldSaveService::remove_member(const std::filesystem::path& save_dir,
                                     std::string_view relative_path,
                                     std::vector<std::string>* errors) {
    const SaveGuard guard;
    if (!member_path_allowed(relative_path)) {
        AddError(errors, "member path is not allowed: " + std::string(relative_path));
        return false;
    }
    const std::string relative(relative_path);
    if (SaveTransaction::active_for(save_dir) &&
        !SaveTransaction::check_publication_target(save_dir / relative, errors))
        return false;
    TransactionScope scope(save_dir, {relative}, errors);
    if (!scope.ok())
        return false;
    return durable_remove(save_dir / relative, errors) && scope.commit(errors);
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
    // Callers hold SaveGuard and reclaim staging before reaching this helper.
    std::error_code directory_error;
    if (!artifacts && destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), directory_error);
    }
    if (directory_error) {
        AddError(errors,
                 "cannot create directory " +
                     Filesystem::GenericUtf8PathString(destination.parent_path()) + ": " +
                     directory_error.message());
        return false;
    }
    if (artifacts)
        artifacts->reserve_file(destination);
    std::filesystem::path temp_path;
    if (!WriteDurableRegionTemp(staging, bytes, temp_path, errors, artifacts)) {
        return false;
    }
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
    if (!ReplaceMember(temp_path, destination, errors, artifacts)) {
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
    const auto staging = artifacts ? region_staging_path(destination) : destination;
    return WriteFileDurably(destination, bytes, staging, errors, nullptr, artifacts);
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
            if (!backup.empty() && MatchesFileIdentity(destination, published_identity)) {
                PublicationRevision().fetch_add(1, std::memory_order_relaxed);
                ReplaceRegionFileAtomically(backup, destination, errors);
            }
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
            if (!backup.empty() && MatchesFileIdentity(destination, published_identity)) {
                PublicationRevision().fetch_add(1, std::memory_order_relaxed);
                ReplaceRegionFileAtomically(backup, destination, errors);
            }
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

void WorldSaveService::reclaim_file_staging(const std::filesystem::path& destination) {
    const SaveGuard guard;
    ReclaimAbandonedStaging(destination);
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

namespace {
struct ChunkReadCache {
    struct Admission {
        WorldSaveService::SaveFormat format;
        SnapshotIndex index;
        std::array<std::optional<std::string>, 3> envelope;
    };
    struct Region {
        std::vector<RegionRecord> records;
        std::string header;
        std::map<ChunkID, std::pair<std::size_t, std::uint64_t>> positions;
        std::uint64_t file_bytes = 0;
        std::size_t payload_bytes = 0;
    };
    static constexpr std::size_t kMaxRegions = 16;
    static constexpr std::size_t kMaxBytes = std::size_t{256} * 1024 * 1024;

    std::uint64_t revision = PublicationRevision().load(std::memory_order_relaxed);
    std::map<std::filesystem::path, Admission> admissions;
    std::map<std::filesystem::path, Region> regions;
    std::size_t payload_bytes = 0;

    void clear() {
        admissions.clear();
        regions.clear();
        payload_bytes = 0;
    }
};

// Any publication may change an index or region file; drop everything verified before it.
void RefreshChunkReadCache(ChunkReadCache& cache) {
    const auto current = PublicationRevision().load(std::memory_order_relaxed);
    if (current == cache.revision)
        return;
    cache.clear();
    cache.revision = current;
}

bool ReadChunkEnvelope(const std::filesystem::path& save_dir,
                       std::array<std::optional<std::string>, 3>& envelope) {
    const std::array paths{save_dir / "world_info.json",
                           WorldSaveService::world_manifest_path(save_dir),
                           snapshot_index_path(save_dir)};
    const std::array limits{
        SavedInputLimits::kMetadataBytes, SavedInputLimits::kManifestBytes, kMaxSnapshotIndexBytes};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const SavedInputFile input(paths[i]);
        if (input.missing()) {
            envelope[i].reset();
        } else {
            std::string bytes;
            if (!input.read(bytes, limits[i]))
                return false;
            envelope[i] = std::move(bytes);
        }
    }
    return true;
}

ChunkReadCache::Region IndexChunkReadRegion(std::vector<RegionRecord> records) {
    ChunkReadCache::Region region;
    region.header.append(kRegionMagic, sizeof(kRegionMagic));
    AppendU16(region.header, kRegionVersion);
    AppendU16(region.header, static_cast<std::uint16_t>(records.size()));
    std::uint64_t offset = kRegionFileHeaderSize + records.size() * kRecordHeaderSize;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        AppendU64(region.header, record.id);
        region.header.push_back(static_cast<char>(record.lod_level));
        region.header.push_back(static_cast<char>(record.flags));
        AppendU32(region.header, record.uncompressed_size);
        AppendU32(region.header, static_cast<std::uint32_t>(record.compressed_payload.size()));
        if (record.lod_level == 0)
            region.positions.emplace(record.id, std::pair{i, offset});
        offset += record.compressed_payload.size();
        region.payload_bytes += record.compressed_payload.size();
    }
    region.file_bytes = offset;
    region.records = std::move(records);
    return region;
}

// Admission covers the container and its membership. A subsequent read checks the
// complete manifest and the exact requested payload on one fresh, admitted handle.
// Unrequested payloads are checked when used; no cached chunk bytes escape unchecked.
bool CurrentRecordMatches(const std::filesystem::path& path,
                          const ChunkReadCache::Region& region,
                          ChunkID id) {
    const SavedInputFile input(path);
    if (input.missing())
        return region.file_bytes == 0;
    std::uint64_t length = 0;
    if (region.file_bytes == 0 || !input.length(length) || length != region.file_bytes)
        return false;
    std::string header(region.header.size(), '\0');
    if (!input.read_exact(header.data(), header.size()) || header != region.header)
        return false;
    const auto found = region.positions.find(id);
    if (found == region.positions.end())
        return input.length(length) && length == region.file_bytes;
    const auto& [index, offset] = found->second;
    const auto& payload = region.records[index].compressed_payload;
    std::string current(payload.size(), '\0');
    return input.seek(offset) && input.read_exact(current.data(), current.size()) &&
           current == payload && input.length(length) && length == region.file_bytes;
}

std::optional<ChunkReadCache>& ActiveChunkReadCache() {
    static thread_local std::optional<ChunkReadCache> active;
    return active;
}
} // namespace

struct WorldSaveService::ChunkReader::Impl {
    ChunkReadCache cache;
};

WorldSaveService::ChunkReader::ChunkReader()
    : m_impl(std::make_unique<Impl>()) {}
WorldSaveService::ChunkReader::~ChunkReader() = default;

WorldSaveService::ChunkReadBatch::ChunkReadBatch() {
    if (ActiveChunkReadCache())
        return;
    ActiveChunkReadCache().emplace();
    m_owner = true;
}

WorldSaveService::ChunkReadBatch::~ChunkReadBatch() {
    if (m_owner)
        ActiveChunkReadCache().reset();
}

WorldSaveService::ChunkReadResult
WorldSaveService::read_chunk(const std::filesystem::path& save_dir,
                             const IVec3& coords,
                             bool require_index,
                             ChunkReader* reader) {
    const detail::SnapshotReadOwnership ownership;
    auto& active = ActiveChunkReadCache();
    auto* batch = reader ? &reader->m_impl->cache : (active ? &active.value() : nullptr);
    if (batch)
        RefreshChunkReadCache(*batch);
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
        ChunkReadCache::Admission read_admission;
        if (!ReadChunkEnvelope(save_dir, read_admission.envelope))
            return fail("unreadable snapshot envelope");
        const ChunkReadCache::Admission* admission = nullptr;
        if (batch) {
            if (const auto cached = batch->admissions.find(save_dir);
                cached != batch->admissions.end() &&
                cached->second.envelope == read_admission.envelope)
                admission = &cached->second;
            else
                batch->clear();
        }
        if (!admission) {
            if (!read_save_format(
                    save_dir, read_admission.format, &errors, {}, &read_admission.index))
                return fail(errors.empty() ? "corrupt snapshot index" : errors.front());
            std::array<std::optional<std::string>, 3> current;
            if (!ReadChunkEnvelope(save_dir, current) || current != read_admission.envelope)
                return fail("snapshot envelope changed during admission");
            admission =
                batch
                    ? &batch->admissions.emplace(save_dir, std::move(read_admission)).first->second
                    : &read_admission;
        }
        if (require_index && !admission->format.indexed)
            return fail("inconsistent snapshot: index missing");
        ChunkReadCache::Region read_region;
        const ChunkReadCache::Region* region = nullptr;
        if (batch) {
            if (const auto cached = batch->regions.find(path); cached != batch->regions.end())
                region = &cached->second;
        }
        const bool cached_region = region != nullptr;
        if (!region) {
            std::vector<RegionRecord> read_records;
            if (!ReadRegionFile(path, read_records, &errors, {}, true))
                return fail(errors.empty() ? "unreadable chunk region" : errors.front());
            if (!VerifyRegionMembership(path, read_records, &errors, &admission->index))
                return fail(errors.empty() ? "inconsistent snapshot" : errors.front());
            read_region = IndexChunkReadRegion(std::move(read_records));
            if (read_region.records.empty() && SavedInputFile(path).missing())
                read_region = {};
            if (batch && read_region.payload_bytes <= ChunkReadCache::kMaxBytes) {
                while (!batch->regions.empty() &&
                       (batch->regions.size() >= ChunkReadCache::kMaxRegions ||
                        batch->payload_bytes + read_region.payload_bytes >
                            ChunkReadCache::kMaxBytes)) {
                    batch->payload_bytes -= batch->regions.begin()->second.payload_bytes;
                    batch->regions.erase(batch->regions.begin());
                }
                batch->payload_bytes += read_region.payload_bytes;
                region = &batch->regions.emplace(path, std::move(read_region)).first->second;
            } else {
                region = &read_region;
            }
        }
        const auto id = Chunk::calculate_id(coords);
        if (cached_region && !CurrentRecordMatches(path, *region, id))
            return fail("durable chunk region changed since admission");
        if (const auto found = region->positions.find(id); found != region->positions.end()) {
            const auto& record = region->records[found->second.first];
            ChunkRecordFacts facts;
            const bool admitted = AdmitChunkRecord(record, path, &errors, facts, &result.chunk);
            constexpr std::size_t kLattice = static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                             (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
            if (!admitted || !result.chunk || result.chunk->get_coords() != coords ||
                (!result.chunk->sdf_data.empty() && result.chunk->sdf_data.size() != kLattice))
                return fail(errors.empty() ? "corrupt exact chunk identity or SDF lattice"
                                           : errors.front());
            if (!admission->format.indexed)
                PromoteLegacyWaterRecord(*result.chunk, record);
            else
                RepairOwnedWaterRecordFlag(*result.chunk, record);
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
        u64 authority;
    };
    std::vector<Revision> revisions;
    for (const auto& chunk : chunks)
        if (chunk && chunk->is_voxel_data_dirty())
            revisions.push_back({chunk,
                                 chunk->voxel_revision(),
                                 chunk->water_revision(),
                                 chunk->authority_hash_revision()});
    return [revisions = std::move(revisions)]() {
        for (const auto& revision : revisions)
            if (revision.voxel != std::numeric_limits<u32>::max() &&
                revision.water != std::numeric_limits<u32>::max() &&
                revision.chunk->voxel_revision() == revision.voxel &&
                revision.chunk->water_revision() == revision.water &&
                revision.chunk->authority_hash_revision() == revision.authority) {
                revision.chunk->clear_voxel_data_dirty();
                revision.chunk->clear_water_state_dirty();
            }
    };
}

bool WorldSaveService::load_world(WorldStreamingState& state,
                                  const std::filesystem::path& save_dir,
                                  std::vector<std::string>& errors,
                                  const std::stop_token& stop,
                                  const std::unordered_set<ChunkID>* retain_chunks,
                                  std::vector<IVec3>* authoritative_inventory,
                                  std::size_t* decoded_chunk_count) const {
    const detail::SnapshotReadOwnership ownership;
    const SavedJsonReadScope json_reads;
    DecodeCounter().fetch_add(1, std::memory_order_acq_rel);
    LUMIN_PROFILE_ZONE_N("File world load");
    // Staging lives outside the strict region inventory. Each published file
    // uses its file lock; the snapshot gate excludes partial indexed publication only.
    state.clear();
    if (decoded_chunk_count)
        *decoded_chunk_count = 0;
    if (authoritative_inventory)
        authoritative_inventory->clear();
    const auto reject = [&state, &errors, authoritative_inventory, decoded_chunk_count](
                            const std::string& message) {
        state.clear();
        if (decoded_chunk_count)
            *decoded_chunk_count = 0;
        if (authoritative_inventory)
            authoritative_inventory->clear();
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
            const auto metadata_error = CheckMetadataValue(metadata);
            if (!metadata_error.empty())
                return reject(metadata_error);
        }
        nlohmann::json manifest;
        bool manifest_missing = false;
        if (!ReadSavedJson(world_manifest_path(save_dir),
                           SavedInputLimits::kManifestBytes,
                           manifest,
                           manifest_missing,
                           false,
                           &json_failure,
                           stop))
            return reject(json_failure == SavedJsonFailure::Cancelled
                              ? "Saved-world validation cancelled."
                          : json_failure == SavedJsonFailure::Limit
                              ? "Saved-input admission refused: world manifest compatibility limit."
                              : "corrupt or unreadable world manifest");
        if (!manifest_missing) {
            const auto manifest_error = CheckManifestValue(manifest);
            if (!manifest_error.empty())
                return reject(manifest_error);
        }
        SnapshotIndex snapshot_index;
        if (!read_snapshot_index(save_dir, snapshot_index, &errors, stop))
            return reject("inconsistent snapshot: corrupt snapshot index");
        const bool indexed = snapshot_index.generation != 0;
        const bool requires_index =
            (!metadata_missing && (metadata.at("container_version") == kIndexedMetadataVersion ||
                                   metadata.contains("snapshotFormat"))) ||
            (!manifest_missing && manifest.at("schema") == kIndexedWorldManifestSchema);
        if (!indexed && requires_index)
            return reject("inconsistent snapshot: index missing");
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
            if (entry.path() == snapshot_index_path(save_dir))
                continue;
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
                entry.path() == ground_objects_path(save_dir) ||
                entry.path() == field_pages_path(save_dir) ||
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
        if (!present) {
            // An index-only region directory is still an indexed snapshot: verify it before
            // reporting a clean miss, or tampered metadata/energy would go unnoticed.
            if (indexed) {
                const std::string mismatch =
                    VerifySnapshotIndex(save_dir, snapshot_index, {}, stop);
                if (stop.stop_requested())
                    return reject("Saved-world validation cancelled.");
                if (!mismatch.empty())
                    return reject("inconsistent snapshot: " + mismatch);
            }
            return false;
        }
        std::map<std::string, MemberDigest> region_digests;
        std::set<ChunkID> decoded_ids;
        constexpr std::size_t kMaxStagedRepairBytes = std::size_t{256} * 1024 * 1024;
        std::size_t staged_repair_bytes = 0;
        const bool promote_legacy_water = !indexed && authoritative_inventory != nullptr;
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
                const MemberDigest digest = RegionLod0Fingerprint(path, records);
                if (digest.present)
                    region_digests[RelativeRegionMember(rx, rz)] = digest;
            }
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            for (const auto& record : records) {
                if (stop.stop_requested())
                    return reject("Saved-world validation cancelled.");
                if (record.lod_level == 0) {
                    // A decoded record carries its own id (DecodeChunkRecord checks it).
                    const bool retain = !retain_chunks || retain_chunks->contains(record.id);
                    std::shared_ptr<Chunk> chunk;
                    ChunkRecordFacts facts;
                    if (!AdmitChunkRecord(record,
                                          path,
                                          &errors,
                                          facts,
                                          retain || promote_legacy_water ? &chunk : nullptr,
                                          retain_chunks && retain_chunks->empty() &&
                                              !authoritative_inventory,
                                          true))
                        return reject("");
                    if (!chunk && (record.flags & kRecordFlagEdited) == 0u &&
                        facts.loaded_or_edited &&
                        !AdmitChunkRecord(record, path, &errors, facts, &chunk))
                        return reject("");
                    // Retain promotions and flag repairs until the save that records them commits.
                    // A pure validation (no inventory) re-derives repairs on every load, so it
                    // never retains them, and loads bound the staged repair chunks by bytes.
                    const bool promoted =
                        chunk &&
                        (promote_legacy_water ? PromoteLegacyWaterRecord(*chunk, record)
                                              : RepairOwnedWaterRecordFlag(*chunk, record));
                    if (promoted)
                        facts.loaded_or_edited = true;
                    if ((indexed || authoritative_inventory) && !facts.lattice_ok)
                        return reject("corrupt world chunk SDF lattice");
                    int chunk_rx = 0, chunk_rz = 0;
                    region_coords_for_chunk(facts.coords, chunk_rx, chunk_rz);
                    if (chunk_rx != rx || chunk_rz != rz || !decoded_ids.insert(record.id).second)
                        return reject("corrupt chunk region identity or duplicate id: " +
                                      Filesystem::GenericUtf8PathString(path));
                    if (authoritative_inventory && facts.loaded_or_edited)
                        authoritative_inventory->push_back(facts.coords);
                    if (decoded_chunk_count)
                        ++*decoded_chunk_count;
                    const bool repaired_only = promoted && !promote_legacy_water;
                    if (repaired_only && authoritative_inventory) {
                        staged_repair_bytes += record.uncompressed_size;
                        if (staged_repair_bytes > kMaxStagedRepairBytes)
                            return reject("Saved-input admission refused: world exceeds the flag "
                                          "repair staging budget");
                    }
                    if (retain || (promoted && (promote_legacy_water || authoritative_inventory)))
                        state.insert_chunk(chunk);
                    else if (chunk)
                        AdmittedChunkRecordCache().remember_decoded(ChunkRecordKeyOf(record),
                                                                    chunk);
                } else {
                    const auto tier = static_cast<World::FarLodTier>(record.lod_level);
                    if (record.id != World::FarLodStore::tile_record_id(tier, rx, rz))
                        return reject("corrupt far-LOD region identity: " +
                                      Filesystem::GenericUtf8PathString(path));
                    if (!AdmitFarLodRecord(record, rx, rz, &errors))
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
            if (!validate_ground_objects(save_dir, ledger, &errors, stop) ||
                !validate_field_pages(save_dir, ledger, &errors, stop))
                return reject("");
        }
        Ecs::EntityRegistrySnapshot plants;
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
        if (!load_plant_entities(plants, save_dir, &errors, stop))
            return reject("");
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
        if (indexed) {
            const std::string mismatch =
                VerifySnapshotIndex(save_dir, snapshot_index, region_digests, stop);
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            if (!mismatch.empty())
                return reject("inconsistent snapshot: " + mismatch);
        }
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
                                                         bool acknowledge_dirty) const {
    const SaveGuard guard;
    WorldSaveDirtyReport report;
    report.chunks_total = state.size();

    const std::vector<ChunkID> dirty_ids = state.dirty_chunk_ids();
    report.chunks_dirty = dirty_ids.size();
    if (dirty_ids.empty()) {
        return report;
    }

    std::vector<std::shared_ptr<Chunk>> covered;
    covered.reserve(dirty_ids.size());
    for (const auto id : dirty_ids)
        if (auto chunk = state.find_chunk(id))
            covered.push_back(std::move(chunk));
    auto acknowledge = chunk_save_acknowledgement(covered);
    PreparedWorldWrite prepared;
    if (!prepared.prepare(state, save_dir, true, errors) ||
        !prepared.publish(errors, &report.regions_written))
        return report;

    if (acknowledge_dirty) {
        if (auto* transaction = SaveTransaction::active_for(save_dir))
            transaction->defer_until_commit(std::move(acknowledge));
        else
            acknowledge();
    }
    report.saved = true;
    return report;
}

struct WorldSaveService::PreparedWorldWrite::Data {
    SaveOwnership ownership;
    std::filesystem::path save_dir;
    std::map<std::filesystem::path, std::vector<RegionRecord>> regions;
    bool write_manifest = false;
    std::vector<std::filesystem::path> unchanged_regions;
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
            const detail::RegionFileWriteGuard transaction(path);

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

            // What DecodeChunkRecord will find in each record encoded here. Admitted only
            // after the region passes the writer limits that decoding also enforces.
            struct EncodedChunk {
                std::size_t offset;
                ChunkRecordFacts facts;
                std::string inputs;
            };
            std::vector<EncodedChunk> encoded;
            bool changed = !std::is_sorted(records.begin(),
                                           records.end(),
                                           [](const RegionRecord& lhs, const RegionRecord& rhs) {
                                               return std::pair{lhs.lod_level, lhs.id} <
                                                      std::pair{rhs.lod_level, rhs.id};
                                           });
            encoded.reserve(region_chunks.size());
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

                const auto inputs = ChunkSnapshotInputBytes(*chunk);
                if (existing != record_index.end() && record.flags == previous_flags &&
                    AdmittedChunkRecordCache().matches_serialized(
                        ChunkRecordKeyOf(records[existing->second]), inputs))
                    continue;
                const bool water_authority = chunk->has_water_authority();
                const std::string payload = SerializeChunkRecordPayloadJson(*chunk);
                if (!CompressPayload(payload, record, errors)) {
                    return false;
                }
                // Non-finite values serialize as null, which decoding rejects; a concurrent
                // authority change leaves the encoded field unknown. Both stay with a real decode.
                // A JSON null follows ':', '[' or ','; none of those occur in base64 data.
                const bool has_null = payload.find(":null") != std::string::npos ||
                                      payload.find("[null") != std::string::npos ||
                                      payload.find(",null") != std::string::npos;
                if (water_authority == chunk->has_water_authority() && !has_null) {
                    encoded.push_back(
                        {existing != record_index.end() ? existing->second : records.size(),
                         ChunkRecordFacts{chunk->get_coords(),
                                          SdfLatticeAdmissible(*chunk),
                                          water_authority ||
                                              (record.flags & kRecordFlagEdited) != 0u},
                         inputs == ChunkSnapshotInputBytes(*chunk) ? inputs : std::string{}});
                }

                if (existing != record_index.end()) {
                    changed = changed || ChunkRecordKeyOf(records[existing->second]) !=
                                             ChunkRecordKeyOf(record);
                    records[existing->second] = std::move(record);
                } else {
                    changed = true;
                    record_index[{record.lod_level, record.id}] = records.size();
                    records.push_back(std::move(record));
                }
            }

            if (!AdmitRegionWrite(records, errors))
                return false;
            for (const auto& entry : encoded)
                AdmittedChunkRecordCache().remember(
                    ChunkRecordKeyOf(records[entry.offset]), entry.facts, entry.inputs);
            if (changed)
                candidate->regions.emplace(path, std::move(records));
            else
                candidate->unchanged_regions.push_back(path);
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
        std::vector<std::string> members;
        for (const auto& [path, records] : prepared->regions) {
            (void)records;
            members.push_back(
                Filesystem::GenericUtf8PathString(path.lexically_relative(prepared->save_dir)));
        }
        TransactionScope scope(prepared->save_dir, members, errors);
        if (!scope.ok())
            return false;
        std::filesystem::create_directories(region_directory(prepared->save_dir));
        const AncestorFlushBatch directory_flushes(prepared->save_dir);
        for (const auto& path : prepared->unchanged_regions)
            ReclaimAbandonedStaging(region_staging_path(path));
        for (auto& [path, records] : prepared->regions) {
            if (!WriteRegionFile(path, records, errors))
                return false;
        }
        if (!WriteWorldManifest(world_manifest_path(prepared->save_dir), errors))
            return false;
        if (!scope.commit(errors))
            return false;
        if (regions_written)
            *regions_written = prepared->regions.size() + prepared->unchanged_regions.size();
        return true;
    } catch (const std::exception& error) {
        AddError(errors, std::string("failed to write world region files: ") + error.what());
        return false;
    }
}

} // namespace Luminumbra::Persistence
