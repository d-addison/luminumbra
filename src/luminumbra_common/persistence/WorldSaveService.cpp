#include "WorldSaveService.h"
#include "../components/CoreComponents.h"
#include "../components/IrrigationComponents.h"
#include "../components/PlantComponents.h"
#include "../components/SoilComponents.h"
#include "../fields/FieldCellTypes.h"
#include "../fields/FieldPageState.h"
#include "../world/ActiveRegionLedger.h"
#include "../world/Chunk.h"
#include "../world/GroundObjectStore.h"
#include "../world/WorldClock.h"
#include "../world/WorldStreamingState.h"
#include "ai/CreaturePersistence.h"
#include "core/FilesystemPath.h"
#include <entt/entity/fwd.hpp>
#include <entt/entt.hpp>
#include <luminumbra/core/Types.h>
#include <nlohmann/json_fwd.hpp>

#include "ChunkValidationCache.h"

#include "../ecs/EntitySnapshot.h" //  plant entity snapshot persistence
#include "../fields/EnergyFieldState.h"
#include "../systems/RegionalFoliage.h"
#include "PlantPersistence.h"
#include "SaveTransaction.h"
#include "WorldPersistenceRoundtrip.h"
#include "systems/WaterSystem.h"
#include "world/FarAuthorityOverlay.h"
#include "world/FarEditedCache.h"
#include "world/FarEditedCatalog.h"
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
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <ios>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
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
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Luminumbra::Persistence {
namespace {

std::atomic<std::uint64_t>& EditedRegionDecodeCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
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

constexpr std::uint8_t kRecordFlagEdited = 0x01;
constexpr std::uint8_t kRecordFlagWaterPresent = 0x02;

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

std::atomic<std::uint64_t>& ChunkDecodeCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

std::atomic<std::uint64_t>& GroundObjectDecodeCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

std::atomic<std::uint64_t>& FieldPageDecodeCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
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
    const std::filesystem::directory_iterator it(directory, list_error);
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
        if (ConsumeDirectoryFlushFailureForTesting()) {
            AddError(errors, "member was replaced but its durable publication failed (test seam)");
            return false;
        }
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

WorldSaveService::SaveFactsScope*& ActiveFacts() {
    static thread_local WorldSaveService::SaveFactsScope* active = nullptr;
    return active;
}

// Every member replacement funnels through here so tests can observe real publication sites.
bool ReplaceMember(const std::filesystem::path& temp,
                   const std::filesystem::path& destination,
                   std::vector<std::string>* errors) {
    report_snapshot_step("before-replace", destination);
    if (ActiveFacts() != nullptr)
        ActiveFacts()->invalidate_presence_facts();
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
                    const std::stop_token& stop = {},
                    const WorldSaveService::EditedRegionReadLimits* capture_limits = nullptr) {
    out_records.clear();

    // Apply both the save-member cap and the capture budget before allocation.
    const auto byte_limit = capture_limits ? static_cast<std::size_t>(std::min<std::uint64_t>(
                                                 kMaxSaveMemberBytes, capture_limits->file_bytes))
                                           : kMaxSaveMemberBytes;
    std::string bytes;
    if (!read_bounded_save_file(path, bytes, byte_limit, errors, stop))
        return false;

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
    if (capture_limits && record_count > (capture_limits->scratch_bytes - bytes.size() * 4) / 512) {
        AddError(errors, "edited source manifest scratch limit");
        return false;
    }

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
        if (payload_offset > bytes.size() || compressed_size > bytes.size() - payload_offset) {
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
    // The record count bounds the manifest; check the exact payload sum before reserving.
    const std::size_t header_bytes = kRegionFileHeaderSize + records.size() * kRecordHeaderSize;
    if (header_bytes > bytes.max_size()) {
        AddError(errors, "region file header size overflow: " + path.string());
        return false;
    }
    std::size_t payload_bytes = 0;
    for (const RegionRecord& record : records) {
        if (record.compressed_payload.size() > std::numeric_limits<std::uint32_t>::max() ||
            record.compressed_payload.size() > bytes.max_size() - header_bytes - payload_bytes) {
            AddError(errors, "region file payload size overflow: " + path.string());
            return false;
        }
        payload_bytes += record.compressed_payload.size();
    }
    if (header_bytes > kMaxSaveMemberBytes || payload_bytes > kMaxSaveMemberBytes - header_bytes) {
        AddError(errors, "region file exceeds size cap: " + path.string());
        return false;
    }
    bytes.reserve(header_bytes + payload_bytes);

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
void SeedChunkRecord(const RegionRecord& record,
                     const std::string& payload,
                     const ChunkValidationModes& modes,
                     ChunkValidationCache& cache) try {
    // Facts describe the canonical serialization of the quiesced source chunk.
    // Prove compressed output decodes to exactly those bytes before seeding.
    std::string decoded;
    if (!DecompressPayload(record, decoded, nullptr) || decoded != payload)
        return;
    for (const bool strict : {false, true}) {
        const auto& mode = modes[strict];
        if (!mode.has_value())
            continue;
        auto facts = *mode;
        facts.authoritative = (record.flags & kRecordFlagEdited) != 0u;
        cache.remember(
            {record.id, record.uncompressed_size, record.lod_level, record.flags, strict},
            record.compressed_payload,
            facts,
            true);
    }
} catch (const std::exception&) {
    // Seeding is optional; a speculative strict refusal cannot reject a save.
    return;
}

bool CompressChunkSnapshot(const std::shared_ptr<Chunk>& chunk,
                           RegionRecord& record,
                           std::vector<std::string>* errors) {
    WorldStreamingState single;
    single.insert_chunk(chunk);
    auto* cache = ChunkValidationCache::active();
    ChunkValidationModes modes{};
    const auto payload = StreamWorldStreamingStateSnapshotJson(single, cache ? &modes : nullptr);
    if (!CompressPayload(payload, record, errors))
        return false;
    if (cache)
        SeedChunkRecord(record, payload, modes, *cache);
    return true;
}

std::shared_ptr<Chunk> DecodeChunkRecord(const RegionRecord& record,
                                         const std::filesystem::path& path,
                                         std::vector<std::string>* errors,
                                         bool strict_water = false) {
    ChunkDecodeCounter().fetch_add(1, std::memory_order_relaxed);
    if (auto* cache = ChunkValidationCache::active())
        cache->record_decode(strict_water);
    std::string payload;
    if (!DecompressPayload(record, payload, errors)) {
        return nullptr;
    }

    WorldStreamingState single;
    std::vector<std::string> load_errors;
    if (!LoadWorldStreamingStateSnapshotJson(payload, single, load_errors, strict_water)) {
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

struct ValidatedChunkRecord {
    ChunkValidationCache::Facts facts{};
    std::shared_ptr<Chunk> chunk{};
};

// Failed decodes are never cached: diagnostics retain their original path and order.
std::optional<ValidatedChunkRecord> ValidateChunkRecord(const RegionRecord& record,
                                                        const std::filesystem::path& path,
                                                        std::vector<std::string>* errors,
                                                        bool strict_water) {
    auto* cache = ChunkValidationCache::active();
    const ChunkValidationCache::Key key{
        record.id, record.uncompressed_size, record.lod_level, record.flags, strict_water};
    if (cache) {
        if (const auto facts = cache->find(key, record.compressed_payload))
            return ValidatedChunkRecord{*facts, {}};
    }
    auto chunk = DecodeChunkRecord(record, path, errors, strict_water);
    if (!chunk)
        return std::nullopt;
    const ChunkValidationCache::Facts facts{chunk->get_coords(),
                                            chunk->sdf_data.size(),
                                            chunk->sdf_provenance() ==
                                                ChunkSdfProvenance::LoadedOrEdited};
    if (cache) {
        try {
            cache->remember(key, record.compressed_payload, facts);
        } catch (const std::bad_alloc&) {
            // Reuse is optional. Cache allocation failure must not turn a
            // successfully validated record into a persistence refusal.
            return ValidatedChunkRecord{facts, std::move(chunk)};
        }
    }
    return ValidatedChunkRecord{facts, std::move(chunk)};
}

bool LoadValidatedChunkRecord(const RegionRecord& record,
                              const std::filesystem::path& path,
                              int rx,
                              int rz,
                              bool indexed,
                              WorldStreamingState& state,
                              std::vector<std::string>& errors,
                              const std::function<bool(const IVec3&)>& retain_chunk,
                              std::set<ChunkID>& decoded_ids,
                              std::vector<IVec3>* authoritative_inventory) {
    auto validated = ValidateChunkRecord(record, path, &errors, indexed);
    if (!validated)
        return false;
    constexpr std::size_t kLattice =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    if ((indexed || authoritative_inventory) && validated->facts.sdf_size != 0 &&
        validated->facts.sdf_size != kLattice) {
        AddError(&errors, "corrupt world chunk SDF lattice");
        return false;
    }
    int chunk_rx = 0, chunk_rz = 0;
    WorldSaveService::region_coords_for_chunk(validated->facts.coords, chunk_rx, chunk_rz);
    if (chunk_rx != rx || chunk_rz != rz || !decoded_ids.insert(record.id).second) {
        AddError(&errors, "corrupt chunk region identity or duplicate id: " + path.string());
        return false;
    }
    if (authoritative_inventory && validated->facts.authoritative)
        authoritative_inventory->push_back(validated->facts.coords);
    if (!retain_chunk || retain_chunk(validated->facts.coords)) {
        // Runtime loads still construct fresh mutable chunks. Validation-only
        // callers reuse facts without retaining runtime arrays or objects.
        if (!validated->chunk)
            validated->chunk = DecodeChunkRecord(record, path, &errors, indexed);
        if (!validated->chunk)
            return false;
        state.insert_chunk(validated->chunk);
    }
    return true;
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
    std::string bytes;
    if (!read_bounded_save_file(metadata_path, bytes, kMaxSaveMetadataBytes, nullptr))
        return "corrupt world metadata: unreadable or exceeds size cap";
    const auto metadata = nlohmann::json::parse(bytes);
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
    std::string bytes;
    if (!read_bounded_save_file(manifest_path, bytes, kMaxSaveManifestBytes, nullptr))
        return "corrupt world manifest: unreadable or exceeds size cap";
    const auto manifest = nlohmann::json::parse(bytes);
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
    std::uint64_t hash = 14695981039346656037ull;
    const auto fold = [&hash](const std::string& bytes) {
        for (const char byte : bytes) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= 1099511628211ull;
        }
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

// Joins the active transaction for save_dir, else (indexed directory or pending journal) runs an
// implicit one-writer transaction, else is inert and the caller keeps its legacy code path.
// Owner thread only. Construct BEFORE the first member write; call commit() after the last one.
class TransactionScope {
public:
    TransactionScope(const std::filesystem::path& save_dir,
                     const std::vector<std::string>& relative_paths,
                     std::vector<std::string>* errors,
                     bool require_index = false) {
        const auto far_policy = World::FarEditedCacheSaveScope::PolicyFor(save_dir);
        if (SaveTransaction* active = SaveTransaction::active_for(save_dir)) {
            m_engaged = true;
            m_ok = active->protect(relative_paths, errors) &&
                   World::RegisterFarEditedCacheFinalizer(*active, far_policy);
            return;
        }
        if (!require_index && !far_policy && !snapshot_index_exists(save_dir) &&
            !journal_pending(save_dir))
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
            save_dir, previous.tick, previous.anchors, &WorldSaveService::member_digest);
        m_engaged = true;
        // Recover first (begin), then refuse to bless a corrupt baseline: an implicit writer must
        // not overwrite a checksum-mismatched member and publish an index that conceals it.
        m_ok = m_transaction->begin(errors) && m_facts->validate(errors) &&
               m_transaction->protect(relative_paths, errors) &&
               World::RegisterFarEditedCacheFinalizer(*m_transaction, far_policy);
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
                                              "chunks/region/creature-entities.json",
                                              "chunks/region/ground-objects.json",
                                              "chunks/region/field-pages.fpg",
                                              "chunks/region/active-regions.arl"};
    std::set<std::string> listed;
    for (const SnapshotMember& member : index.members) {
        if (member.cls == MemberClass::Derived)
            continue; // optional cache presence never participates in authoritative validation
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
            if (!hash_file(save_dir / member.path,
                           bytes,
                           fnv64,
                           exists,
                           nullptr,
                           save_member_byte_limit(member.path)))
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

bool StrictWaterRegion(const std::filesystem::path& region_file) {
    std::filesystem::path save_dir;
    std::string relative;
    return SnapshotMemberForRegionFile(region_file, save_dir, relative) &&
           snapshot_index_exists(save_dir);
}

bool VerifyIndexedRegionMembership(const std::string& relative,
                                   const std::vector<RegionRecord>& records,
                                   const SnapshotIndex& index,
                                   std::vector<std::string>* errors) {
    auto member = std::lower_bound(
        index.members.begin(),
        index.members.end(),
        relative,
        [](const SnapshotMember& value, const std::string& path) { return value.path < path; });
    if (member != index.members.end() && member->path != relative)
        member = index.members.end();
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
    return VerifyIndexedRegionMembership(relative, records, index, errors);
}

} // namespace

bool WorldSaveService::save_metadata(const std::string& bytes,
                                     const std::filesystem::path& save_dir,
                                     std::vector<std::string>* errors) {
    const auto metadata = nlohmann::json::parse(bytes, nullptr, false);
    const bool has_clock = metadata.contains("simulationTick") || metadata.contains("calendar");
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
    TransactionScope scope(save_dir, {"world_info.json"}, errors, has_clock);
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
        std::string bytes;
        if (!read_bounded_save_file(path, bytes, kMaxSaveMetadataBytes, errors))
            return false;
        const auto metadata = nlohmann::json::parse(bytes);
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

WorldSaveService::LegacyEnergyReadStatus
WorldSaveService::load_legacy_energy(luminumbra::fields::EnergyFieldState& state,
                                     const std::filesystem::path& save_dir,
                                     std::uint64_t tick,
                                     std::vector<std::string>* errors) {
    const auto path = save_dir / "aether_state.efs";
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found &&
        (!ec || ec == std::errc::no_such_file_or_directory)) {
        state.ResetEmptyAtTick(tick);
        return LegacyEnergyReadStatus::Missing;
    }
    std::string bytes;
    if (ec || !read_bounded_save_file(path, bytes, kMaxSaveEnergyBytes, errors)) {
        AddError(errors, kLegacyEnergyUnreadable);
        return LegacyEnergyReadStatus::Unreadable;
    }
    if (!state.DeserializeRecord(bytes, tick)) {
        AddError(errors, kLegacyEnergyInvalid);
        return LegacyEnergyReadStatus::Invalid;
    }
    return LegacyEnergyReadStatus::Loaded;
}

std::filesystem::path WorldSaveService::plant_entities_path(const std::filesystem::path& save_dir) {
    return region_directory(save_dir) / "plant-entities.json";
}

bool WorldSaveService::save_plant_entities(const Luminumbra::Ecs::EntityRegistrySnapshot& snapshot,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors) {
    if (snapshot.entities.size() > kMaxSavePlants) {
        AddError(errors, "plant population exceeds save limit");
        return false;
    }
    for (const auto& entity : snapshot.entities) {
        if (!luminumbra::foliage::PlantRecordFits(entity)) {
            AddError(errors, "plant record exceeds save limit");
            return false;
        }
    }
    const auto serialized = Ecs::SerializeEntityRegistrySnapshotJson(snapshot);
    if (serialized.size() > kMaxSaveEntityBytes) {
        AddError(errors, "plant snapshot exceeds save limit");
        return false;
    }
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
        return durable_write_file(path, serialized, errors) && scope.commit(errors);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        AddError(errors, "failed to open plant entities file for writing: " + path.string());
        return false;
    }
    output << serialized;
    output.flush();
    if (!output.good()) {
        AddError(errors, "failed to write plant entities file: " + path.string());
        return false;
    }
    return true;
}

bool WorldSaveService::load_plant_entities(Luminumbra::Ecs::EntityRegistrySnapshot& out,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors,
                                           bool admitted_capture) {
    out = Luminumbra::Ecs::EntityRegistrySnapshot{};
    const std::filesystem::path path = plant_entities_path(save_dir);
    std::error_code ec;
    const bool present = std::filesystem::exists(path, ec);
    if (ec) {
        AddError(errors, "failed to inspect plant authority: " + ec.message());
        return false;
    }
    if (!admitted_capture && !SaveTransaction::active_for(save_dir) &&
        snapshot_index_exists(save_dir)) {
        SnapshotIndex index;
        if (!read_snapshot_index(save_dir, index, errors))
            return false;
        const auto member =
            std::find_if(index.members.begin(), index.members.end(), [](const SnapshotMember& m) {
                return m.path == "chunks/region/plant-entities.json";
            });
        std::uint64_t size = 0, fnv = 0;
        bool exists = false;
        if (!hash_file(path, size, fnv, exists, errors, kMaxSaveEntityBytes))
            return false;
        if ((member == index.members.end() && exists) ||
            (member != index.members.end() &&
             (member->present != exists ||
              (exists && (member->bytes != size || member->fnv64 != fnv))))) {
            AddError(errors, "inconsistent snapshot: chunks/region/plant-entities.json");
            return false;
        }
    }
    if (!present) {
        return true; // clean miss: a fresh / no-plant world
    }
    std::string bytes;
    if (!read_bounded_save_file(path, bytes, kMaxSaveEntityBytes, errors))
        return false;
    std::vector<std::string> load_errors;
    if (!Luminumbra::Ecs::LoadEntityRegistrySnapshotJson(bytes, out, load_errors)) {
        for (const auto& e : load_errors)
            AddError(errors, e);
        return false;
    }
    if (out.entities.size() > kMaxSavePlants ||
        !std::all_of(
            out.entities.begin(), out.entities.end(), luminumbra::foliage::PlantRecordFits)) {
        AddError(errors, "plant population or record exceeds save limit");
        return false;
    }
    if (out.persistent_identity && ((!admitted_capture && !SaveTransaction::active_for(save_dir) &&
                                     !snapshot_index_exists(save_dir)) ||
                                    Ecs::SerializeEntityRegistrySnapshotJson(out) != bytes)) {
        AddError(errors, "corrupt or unindexed persistent plant snapshot");
        return false;
    }
    return true;
}

std::filesystem::path
WorldSaveService::creature_entities_path(const std::filesystem::path& save_dir) {
    return region_directory(save_dir) / "creature-entities.json";
}

bool WorldSaveService::save_creature_entities(const luminumbra::ai::CreatureSnapshot& snapshot,
                                              const std::filesystem::path& save_dir,
                                              std::vector<std::string>* errors) {
    // Validate before replacing even a single member. The outer writer validates the shared
    // allocator, owners and plant targets against its complete candidate snapshot.
    std::string bytes, error;
    try {
        bytes = luminumbra::ai::SerializeCreatureSnapshot(snapshot);
    } catch (const std::exception& e) {
        AddError(errors, e.what());
        return false;
    }
    luminumbra::ai::CreatureSnapshot decoded;
    if (!luminumbra::ai::LoadCreatureSnapshot(bytes, decoded, error)) {
        AddError(errors, error);
        return false;
    }
    TransactionScope scope(save_dir, {"chunks/region/creature-entities.json"}, errors, true);
    if (!scope.ok() || !validate_save(save_dir, errors))
        return false;
    const auto path = creature_entities_path(save_dir);
    const bool written = snapshot.entities.empty() ? durable_remove(path, errors)
                                                   : durable_write_file(path, bytes, errors);
    return written && scope.commit(errors);
}

bool WorldSaveService::load_creature_entities(luminumbra::ai::CreatureSnapshot& out,
                                              const std::filesystem::path& save_dir,
                                              std::vector<std::string>* errors,
                                              bool admitted_capture) {
    const auto path = creature_entities_path(save_dir);
    std::error_code ec;
    const bool present = std::filesystem::exists(path, ec);
    if (ec) {
        AddError(errors, "failed to inspect creature authority: " + ec.message());
        return false;
    }
    if (!admitted_capture && !SaveTransaction::active_for(save_dir)) {
        if (snapshot_index_exists(save_dir)) {
            SnapshotIndex index;
            if (!read_snapshot_index(save_dir, index, errors))
                return false;
            const auto member = std::find_if(
                index.members.begin(), index.members.end(), [](const SnapshotMember& m) {
                    return m.path == "chunks/region/creature-entities.json";
                });
            std::uint64_t bytes = 0, fnv = 0;
            bool exists = false;
            if (!hash_file(
                    path, bytes, fnv, exists, errors, luminumbra::ai::kMaxStampedCreatureBytes))
                return false;
            if ((member == index.members.end() && exists) ||
                (member != index.members.end() &&
                 (member->present != exists ||
                  (exists && (member->bytes != bytes || member->fnv64 != fnv))))) {
                AddError(errors, "inconsistent snapshot: chunks/region/creature-entities.json");
                return false;
            }
        } else if (present) {
            AddError(errors, "inconsistent snapshot: creature authority without index");
            return false;
        }
    }
    if (!present) {
        out = {};
        return true;
    }
    // Allocation is bounded by 64 MiB plus one 63-byte controller stamp per allowed record.
    std::string bytes;
    if (!read_bounded_save_file(path, bytes, luminumbra::ai::kMaxStampedCreatureBytes, errors)) {
        return false;
    }
    luminumbra::ai::CreatureSnapshot candidate;
    std::string error;
    if (!luminumbra::ai::LoadCreatureSnapshot(bytes, candidate, error) ||
        candidate.entities.empty()) {
        AddError(errors, error.empty() ? "empty creature authority must be absent" : error);
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
                                           std::vector<std::string>* errors) {
    TransactionScope scope(save_dir, {"chunks/region/ground-objects.json"}, errors, true);
    if (!scope.ok())
        return false;
    if (!validate_save(save_dir, errors))
        return false;
    const std::filesystem::path path = ground_objects_path(save_dir);
    std::error_code ec;
    if (store.empty()) {
        // No ground objects -> no file. Retire any stale file.
        if (scope.engaged())
            return durable_remove(path, errors) && scope.commit(errors);
        std::filesystem::remove(path, ec);
        return true;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::string bytes = store.serialize();
    if (scope.engaged()) {
        // Transactional lane: durable binary replace instead of a truncating stream.
        return durable_write_file(path, bytes, errors) && scope.commit(errors);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        AddError(errors, "failed to open ground objects file for writing: " + path.string());
        return false;
    }
    output << bytes;
    output.flush();
    if (!output.good()) {
        AddError(errors, "failed to write ground objects file: " + path.string());
        return false;
    }
    return true;
}

bool WorldSaveService::load_ground_objects(world::GroundObjectStore& out,
                                           const std::filesystem::path& save_dir,
                                           std::uint64_t next_id,
                                           std::vector<std::string>* errors,
                                           bool admitted_capture) {
    const std::filesystem::path path = ground_objects_path(save_dir);
    std::error_code ec;
    const bool present = std::filesystem::exists(path, ec);
    if (ec) {
        AddError(errors, "failed to inspect ground objects file: " + ec.message());
        return false;
    }
    if (!admitted_capture && !SaveTransaction::active_for(save_dir)) {
        if (snapshot_index_exists(save_dir)) {
            SnapshotIndex index;
            if (!read_snapshot_index(save_dir, index, errors))
                return false;
            const auto member = std::find_if(
                index.members.begin(), index.members.end(), [](const SnapshotMember& value) {
                    return value.path == "chunks/region/ground-objects.json";
                });
            std::uint64_t bytes = 0, fnv = 0;
            bool exists = false;
            if (!hash_file(
                    path, bytes, fnv, exists, errors, world::GroundObjectStore::kMaxFileBytes))
                return false;
            if ((member == index.members.end() && exists) ||
                (member != index.members.end() &&
                 (member->present != exists ||
                  (exists && (member->bytes != bytes || member->fnv64 != fnv))))) {
                AddError(errors, "inconsistent snapshot: chunks/region/ground-objects.json");
                return false;
            }
        } else {
            world::WorldClock clock;
            bool required = false;
            std::int64_t format = 0;
            const auto metadata_error = CheckMetadataFile(save_dir, &format);
            if (!metadata_error.empty() ||
                !read_clock_metadata(save_dir, clock, required, errors)) {
                AddError(errors,
                         metadata_error.empty() ? "corrupt world metadata" : metadata_error);
                return false;
            }
            if (present || required || format >= 1) {
                AddError(errors, "inconsistent snapshot: ground authority without index");
                return false;
            }
        }
    }
    if (!present) {
        out.clear();
        return true; // A verified absent member is a clean miss.
    }
    std::string bytes;
    if (!read_bounded_save_file(path, bytes, world::GroundObjectStore::kMaxFileBytes, errors))
        return false;
    world::GroundObjectStore parsed;
    std::string error;
    GroundObjectDecodeCounter().fetch_add(1, std::memory_order_relaxed);
    if (!world::GroundObjectStore::parse(bytes, next_id, parsed, error)) {
        AddError(errors, "Ground objects load failed: " + error);
        return false;
    }
    out = std::move(parsed);
    return true;
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

bool WorldSaveService::validate_ground_objects(const std::filesystem::path& save_dir,
                                               const world::ActiveRegionLedger& ledger,
                                               std::vector<std::string>* errors,
                                               world::GroundObjectStore* validated,
                                               bool admitted_capture) {
    std::error_code ec;
    const bool has_objects = std::filesystem::exists(ground_objects_path(save_dir), ec);
    if (ec) {
        AddError(errors, "failed to inspect ground objects file: " + ec.message());
        return false;
    }
    if (!has_objects) {
        if (validated)
            validated->clear();
        return true;
    }
    const bool has_ledger = std::filesystem::exists(active_regions_path(save_dir), ec);
    if (ec) {
        AddError(errors, "failed to inspect active regions file: " + ec.message());
        return false;
    }
    if (!has_ledger) {
        // A ledger-less save has no records, so every ground object is orphaned.
        AddError(errors, "corrupt ground objects: orphaned object");
        return false;
    }
    world::GroundObjectStore store;
    if (!load_ground_objects(
            store, save_dir, ledger.next_persistent_id(), errors, admitted_capture))
        return false;
    world::WorldClock clock;
    bool required = false;
    if (!read_clock_metadata(save_dir, clock, required, errors))
        return false;
    if (!GroundObjectsMatchLedger(store, ledger, clock.tick(), errors))
        return false;
    if (validated)
        *validated = std::move(store);
    return true;
}

std::filesystem::path WorldSaveService::field_pages_path(const std::filesystem::path& save_dir) {
    return region_directory(save_dir) / "field-pages.fpg";
}

bool WorldSaveService::save_field_pages(const world::FieldPageState& state,
                                        const std::filesystem::path& save_dir,
                                        std::vector<std::string>* errors) {
    TransactionScope scope(save_dir, {"chunks/region/field-pages.fpg"}, errors);
    if (!scope.ok())
        return false;
    if (!validate_save(save_dir, errors))
        return false;
    const std::filesystem::path path = field_pages_path(save_dir);
    std::error_code ec;
    if (state.empty()) {
        // No field pages -> no file. Retire any stale file.
        if (scope.engaged())
            return durable_remove(path, errors) && scope.commit(errors);
        std::filesystem::remove(path, ec);
        return true;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::string bytes = state.serialize();
    if (scope.engaged()) {
        // Transactional lane: durable binary replace instead of a truncating stream.
        return durable_write_file(path, bytes, errors) && scope.commit(errors);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        AddError(errors, "failed to open field pages file for writing: " + path.string());
        return false;
    }
    output << bytes;
    output.flush();
    if (!output.good()) {
        AddError(errors, "failed to write field pages file: " + path.string());
        return false;
    }
    return true;
}

bool WorldSaveService::load_field_pages(world::FieldPageState& out,
                                        const std::filesystem::path& save_dir,
                                        std::vector<std::string>* errors) {
    const std::filesystem::path path = field_pages_path(save_dir);
    std::error_code ec;
    const bool present = std::filesystem::exists(path, ec);
    if (ec) {
        AddError(errors, "failed to inspect field pages file: " + ec.message());
        return false;
    }
    if (!present) {
        out.clear();
        return true; // clean miss: a world without field pages
    }
    std::string bytes;
    if (!read_bounded_save_file(path, bytes, world::FieldPageState::kMaxFileBytes, errors))
        return false;
    world::FieldPageState parsed;
    std::string error;
    FieldPageDecodeCounter().fetch_add(1, std::memory_order_relaxed);
    if (!world::FieldPageState::parse(bytes, parsed, error)) {
        AddError(errors, "Field pages load failed: " + error);
        return false;
    }
    out = std::move(parsed);
    return true;
}

namespace {
constexpr std::size_t kFieldWordBits = 64;
constexpr auto kFieldPageSide = static_cast<std::size_t>(world::FieldPageState::kPageCells);

bool FieldCellInitialized(const world::FieldPage& page, std::size_t index) {
    const std::uint64_t word = page.mask[index / kFieldWordBits];
    return ((word >> (index % kFieldWordBits)) & std::uint64_t{1}) != 0;
}

// Owner region of the cell at `index` (local_z * 16 + local_x) inside the page keyed by `key`.
world::RegionKey FieldCellOwner(const world::FieldPageKey& key, std::size_t index) {
    const auto cell_x = static_cast<std::int64_t>(key.x) * world::FieldPageState::kPageCells +
                        static_cast<std::int64_t>(index % kFieldPageSide);
    const auto cell_z = static_cast<std::int64_t>(key.z) * world::FieldPageState::kPageCells +
                        static_cast<std::int64_t>(index / kFieldPageSide);
    // parse() bounds every initialized cell to [kMinCell, kMaxCell], so narrowing is exact.
    return world::FieldPageState::region_of_cell(static_cast<std::int32_t>(cell_x),
                                                 static_cast<std::int32_t>(cell_z));
}

// Every initialized cell's owner region must be recorded by the ledger.
bool FieldPagesMatchLedger(const world::FieldPageState& state,
                           const world::ActiveRegionLedger& ledger,
                           std::vector<std::string>* errors) {
    for (const auto& [key, page] : state.pages()) {
        for (std::size_t index = 0; index < world::FieldPage::kCells; ++index) {
            if (!FieldCellInitialized(page, index))
                continue;
            if (ledger.records().count(FieldCellOwner(key, index)) == 0) {
                AddError(errors, "corrupt field pages: orphaned page");
                return false;
            }
        }
    }
    for (const auto& storm : state.storms()) {
        if (ledger.records().count(storm.owner) == 0) {
            AddError(errors, "corrupt field pages: orphaned storm");
            return false;
        }
    }
    for (const auto& strike : state.strikes()) {
        if (ledger.records().count(strike.owner) == 0) {
            AddError(errors, "corrupt field pages: orphaned strike");
            return false;
        }
    }
    for (const auto& [owner, age] : state.weather_clocks()) {
        (void)age;
        if (ledger.records().count(owner) == 0) {
            AddError(errors, "corrupt field pages: orphaned weather clock");
            return false;
        }
    }
    for (const auto& [key, scent] : state.scent_cells()) {
        (void)scent;
        if (ledger.records().count(world::FieldPageState::region_of_scent_cell(key)) == 0) {
            AddError(errors, "corrupt field pages: orphaned scent");
            return false;
        }
    }
    return true;
}
} // namespace

bool WorldSaveService::validate_field_pages(const std::filesystem::path& save_dir,
                                            const world::ActiveRegionLedger& ledger,
                                            std::vector<std::string>* errors,
                                            world::FieldPageState* validated) {
    std::error_code ec;
    const bool has_pages = std::filesystem::exists(field_pages_path(save_dir), ec);
    if (ec) {
        AddError(errors, "failed to inspect field pages file: " + ec.message());
        return false;
    }
    if (!has_pages) {
        if (validated)
            validated->clear();
        return true;
    }
    const bool has_ledger = std::filesystem::exists(active_regions_path(save_dir), ec);
    if (ec) {
        AddError(errors, "failed to inspect active regions file: " + ec.message());
        return false;
    }
    if (!has_ledger) {
        // A ledger-less save has no records, so every field page is orphaned.
        AddError(errors, "corrupt field pages: orphaned page");
        return false;
    }
    world::FieldPageState state;
    if (!load_field_pages(state, save_dir, errors))
        return false;
    world::WorldClock clock;
    bool required = false;
    if (!read_clock_metadata(save_dir, clock, required, errors))
        return false;
    for (const auto& [owner, age] : state.weather_clocks()) {
        (void)owner;
        if (age > clock.tick()) {
            AddError(errors, "corrupt field pages: future weather age");
            return false;
        }
    }
    for (const auto& storm : state.storms()) {
        if (storm.spawn_tick > clock.tick() || storm.active_age > clock.tick() - storm.spawn_tick) {
            AddError(errors, "corrupt field pages: future storm age");
            return false;
        }
    }
    for (const auto& strike : state.strikes()) {
        if (strike.strike_tick > clock.tick()) {
            AddError(errors, "corrupt field pages: future strike");
            return false;
        }
    }
    if (!FieldPagesMatchLedger(state, ledger, errors))
        return false;
    if (validated)
        *validated = std::move(state);
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
            ledger = world::ActiveRegionLedger(ledger.config(), absent_clock);
            return true;
        }
        std::string bytes;
        if (!read_bounded_save_file(path, bytes, world::ActiveRegionLedger::kMaxFileBytes, errors))
            return false;
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
    const bool has_ledger = !ledger.records().empty() || ledger.local_anchor() ||
                            std::filesystem::exists(active_regions_path(save_dir));
    TransactionScope scope(save_dir, {"chunks/region/active-regions.arl"}, errors, has_ledger);
    if (!scope.ok())
        return false;
    if (!validate_save(save_dir, errors))
        return false;
    // Keep an anchor-only ledger: it restores last walking feet before activation.
    if (ledger.records().empty() && !ledger.local_anchor() && ledger.next_persistent_id() == 1 &&
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
            // Durable edited content is complete even if its saved lifecycle was Loading.
            if (qualifies(chunk))
                region.insert_chunk(chunk);
        }
    }
    // Optional extensions compose in a fixed order: ground, pages, then creatures. Empty
    // inputs preserve the preceding projection, including the content-only hash.
    std::string composed = ComputeRegionContentHash(region);
    if (!inputs.ground_object_bytes.empty())
        composed = StableChecksum(composed + "|ground:" + inputs.ground_object_bytes);
    if (!inputs.field_page_bytes.empty())
        composed = StableChecksum(composed + "|pages:" + inputs.field_page_bytes);
    if (!inputs.creature_bytes.empty())
        composed = StableChecksum(composed + "|creatures:" + inputs.creature_bytes);
    return std::stoull(composed, nullptr, 16);
}

std::uint64_t
WorldSaveService::region_simulation_digest(const std::filesystem::path& save_dir,
                                           world::RegionKey key,
                                           const std::vector<std::shared_ptr<Chunk>>& live_chunks) {
    ValidateRegionDigestKey(key);
    // This compatibility overload projects chunks only. Durable chunks are read from save_dir;
    // callers with live ground objects or field pages supply those projections explicitly.
    return region_simulation_digest(save_dir,
                                    key,
                                    RegionDigestInputs{.live = live_chunks,
                                                       .parked = {},
                                                       .durable = {},
                                                       .ground_object_bytes = {},
                                                       .field_page_bytes = {}});
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
    , m_previous(ActiveFacts()) {
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
                                     const std::stop_token& stop,
                                     MemberDigestFn catalog_digest) {
    if (SaveFactsScope* facts = SaveFactsScope::active_for(save_dir); facts && !catalog_digest)
        return facts->validate(errors);
    WorldStreamingState state;
    std::vector<std::string> diagnostics;
    WorldSaveService{}.load_world(
        state,
        save_dir,
        diagnostics,
        stop,
        [](const IVec3&) { return false; },
        nullptr,
        nullptr,
        catalog_digest);
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
        std::shared_ptr<Chunk> chunk =
            DecodeChunkRecord(record, region_file, errors, StrictWaterRegion(region_file));
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
        std::shared_ptr<Chunk> chunk =
            DecodeChunkRecord(record, region_file, errors, StrictWaterRegion(region_file));
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

bool WorldSaveService::read_edited_source_region(
    const std::filesystem::path& region_file,
    const EditedRegionReadLimits& limits,
    bool strict_water,
    const std::function<bool(const Chunk&, std::uint64_t)>& visitor,
    MemberDigest& digest,
    std::uint64_t& decoded_bytes,
    std::size_t& records_read,
    std::vector<std::string>* errors) {
    EditedRegionDecodeCounter().fetch_add(1, std::memory_order_acq_rel);
    std::error_code ec;
    const auto size = std::filesystem::file_size(region_file, ec);
    // Four copies cover stream growth, extracted bytes and compressed record storage.
    // Per-record control storage includes manifest, key validation and sort scratch.
    if (ec || size > limits.file_bytes || size > limits.scratch_bytes / 4) {
        AddError(errors, "edited source region unreadable or over byte limit");
        return false;
    }
    std::vector<RegionRecord> records;
    auto read_limits = limits;
    read_limits.file_bytes = size;
    if (!ReadRegionFile(region_file, records, errors, {}, &read_limits))
        return false;
    std::uint64_t raw_bytes = 0;
    std::uint64_t largest_record = 0;
    std::uint64_t source_samples = 0;
    std::size_t selected_records = 0;
    constexpr std::uint64_t lattice_samples =
        static_cast<std::uint64_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    for (const auto& record : records) {
        if (record.lod_level != 0 || (record.flags & kRecordFlagEdited) == 0)
            continue;
        if (lattice_samples > limits.authoritative_samples - source_samples) {
            AddError(errors, "edited source sample work limit");
            return false;
        }
        source_samples += lattice_samples;
        if (!limits.selected || limits.selected(Chunk::decode_id(record.id))) {
            if (selected_records == limits.records) {
                AddError(errors, "edited source record limit");
                return false;
            }
            ++selected_records;
        }
        if (record.uncompressed_size > limits.decode_bytes - raw_bytes) {
            AddError(errors, "edited source decode limit");
            return false;
        }
        raw_bytes += record.uncompressed_size;
        largest_record =
            std::max(largest_record, static_cast<std::uint64_t>(record.uncompressed_size));
    }
    // JSON nodes and the decoded vectors coexist with input; admit a conservative envelope.
    const auto control_bytes = static_cast<std::uint64_t>(records.size()) * 512;
    if (largest_record > (limits.scratch_bytes - size * 4 - control_bytes) / 128) {
        AddError(errors, "edited source decode scratch limit");
        return false;
    }
    int rx = 0;
    int rz = 0;
    if (!ParseRegionFileName(region_file, rx, rz)) {
        AddError(errors, "edited source region identity invalid");
        return false;
    }
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        return std::pair(a.lod_level, a.id) < std::pair(b.lod_level, b.id);
    });
    for (const auto& record : records) {
        if (record.lod_level != 0 || (record.flags & kRecordFlagEdited) == 0)
            continue;
        const auto chunk = DecodeChunkRecord(record, region_file, errors, strict_water);
        if (!chunk)
            return false;
        if (!strict_water)
            Systems::WaterSystem::ValidateRegionalGrid(*chunk, true);
        int chunk_rx = 0;
        int chunk_rz = 0;
        region_coords_for_chunk(chunk->get_coords(), chunk_rx, chunk_rz);
        if (chunk_rx != rx || chunk_rz != rz) {
            AddError(errors, "edited source chunk region mismatch");
            return false;
        }
        std::string header;
        AppendU64(header, record.id);
        header.push_back(static_cast<char>(record.flags));
        AppendU32(header, record.uncompressed_size);
        AppendU32(header, static_cast<std::uint32_t>(record.compressed_payload.size()));
        auto record_digest = fnv1a64_bytes(header.data(), header.size());
        for (const auto byte : record.compressed_payload) {
            record_digest ^= static_cast<unsigned char>(byte);
            record_digest *= 1099511628211ull;
        }
        if ((record.flags & kRecordFlagEdited) != 0 &&
            (!visitor || !visitor(*chunk, record_digest))) {
            AddError(errors, "edited source chunk refused");
            return false;
        }
    }
    digest = Lod0Fingerprint(records);
    decoded_bytes = raw_bytes;
    records_read = selected_records;
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

std::uint64_t WorldSaveService::edited_source_region_decode_count_for_testing() {
    return EditedRegionDecodeCounter().load(std::memory_order_acquire);
}

std::uint64_t WorldSaveService::decode_count_for_testing() {
    return DecodeCounter().load(std::memory_order_acquire);
}

std::uint64_t WorldSaveService::field_page_decode_count_for_testing() {
    return FieldPageDecodeCounter().load(std::memory_order_relaxed);
}

std::uint64_t WorldSaveService::chunk_decode_count_for_testing() {
    return ChunkDecodeCounter().load(std::memory_order_relaxed);
}

std::uint64_t WorldSaveService::ground_object_decode_count_for_testing() {
    return GroundObjectDecodeCounter().load(std::memory_order_relaxed);
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
    if (ActiveFacts() != nullptr)
        ActiveFacts()->invalidate_presence_facts();
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
    // First-index assembly validates the authoritative baseline after derived groups have
    // reached their final outcome. Restored legacy generations cannot enter an adopted save.
    const auto* transaction = SaveTransaction::active_for(save_dir);
    if (transaction && !transaction->DerivedGroupsAllowCommit()) {
        out = {};
        error = "required derived publication was rolled back";
        return false;
    }
    return member_digest_impl(save_dir, relative_path, out, error, false);
}

bool WorldSaveService::edited_source_member_digest(const std::filesystem::path& save_dir,
                                                   std::string_view relative_path,
                                                   MemberDigest& out,
                                                   std::string& error) {
    return member_digest_impl(save_dir, relative_path, out, error, true);
}

bool WorldSaveService::member_digest_impl(const std::filesystem::path& save_dir,
                                          std::string_view relative_path,
                                          MemberDigest& out,
                                          std::string& error,
                                          bool admitted_capture) {
    if (auto* cache = ChunkValidationCache::active())
        cache->exclude_save_directory(save_dir);
    out = MemberDigest{};
    try {
        if (!member_path_allowed(relative_path)) {
            error = "member path is not allowed";
            return false;
        }
        const auto cls = ClassifySnapshotMember(relative_path);
        if (cls.has_value() && *cls == MemberClass::Derived) {
            SnapshotIndex index;
            if (!read_snapshot_index(save_dir, index, nullptr)) {
                error = "invalid snapshot index";
                return false;
            }
            const auto* transaction = SaveTransaction::active_for(save_dir);
            const auto generation = transaction ? transaction->new_generation() : index.generation;
            return derived_member_digest(save_dir,
                                         relative_path,
                                         generation == 0 ? std::numeric_limits<std::uint64_t>::max()
                                                         : generation,
                                         out,
                                         error);
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
                    const auto validated = ValidateChunkRecord(record, path, &diagnostics, false);
                    if (!validated) {
                        error = first_error("corrupt chunk record");
                        return false;
                    }
                    constexpr std::size_t kLattice = static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                                     (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
                    if (validated->facts.sdf_size != 0 && validated->facts.sdf_size != kLattice) {
                        error = "corrupt world chunk SDF lattice";
                        return false;
                    }
                    int chunk_rx = 0;
                    int chunk_rz = 0;
                    region_coords_for_chunk(validated->facts.coords, chunk_rx, chunk_rz);
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
                if (!load_active_regions(ledger, save_dir, &diagnostics) ||
                    !read_clock_metadata(save_dir, clock, required, &diagnostics)) {
                    error = first_error(world::ActiveRegionLedger::kCorruptMessage);
                    return false;
                }
                if (ledger.header_tick() > clock.tick()) {
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
            if (!load_plant_entities(plants, save_dir, &diagnostics, admitted_capture)) {
                error = first_error("corrupt plant entities");
                return false;
            }
        } else if (relative == "chunks/region/creature-entities.json") {
            luminumbra::ai::CreatureSnapshot creatures;
            if (!load_creature_entities(creatures, save_dir, &diagnostics, admitted_capture)) {
                error = first_error("corrupt creature entities");
                return false;
            }
        } else if (relative == "chunks/region/ground-objects.json") {
            world::GroundObjectStore objects;
            const std::uint64_t no_id_limit = std::numeric_limits<std::uint64_t>::max();
            if (!load_ground_objects(
                    objects, save_dir, no_id_limit, &diagnostics, admitted_capture)) {
                error = first_error("corrupt ground objects");
                return false;
            }
        } else if (relative == "chunks/region/field-pages.fpg") {
            world::FieldPageState pages;
            if (!load_field_pages(pages, save_dir, &diagnostics)) {
                error = first_error("corrupt field pages");
                return false;
            }
        } else if (relative == "aether_state.efs") {
            luminumbra::fields::EnergyFieldState energy;
            world::WorldClock clock;
            bool regional = false;
            if (!read_clock_metadata(save_dir, clock, regional, &diagnostics)) {
                error = first_error("corrupt world metadata");
                return false;
            }
            std::string bytes;
            const auto limit = regional
                                   ? luminumbra::fields::EnergyFieldState::kMaxRegionalRecordBytes
                                   : kMaxSaveEnergyBytes;
            if (!read_bounded_save_file(path, bytes, limit, &diagnostics)) {
                error = "corrupt energy field record: unreadable or size limit";
                return false;
            }
            if (regional) {
                world::ActiveRegionLedger ledger;
                if (!energy.DeserializeRegionalRecord(bytes) ||
                    !load_active_regions(ledger, save_dir, &diagnostics) ||
                    !energy.ValidateRegionalOwners(ledger.records(), ledger.energy_ages())) {
                    error = "corrupt regional energy field record or ownership";
                    return false;
                }
                for (const auto& [owner, age] : ledger.energy_ages()) {
                    (void)owner;
                    if (age > clock.tick()) {
                        error = "corrupt regional energy field age";
                        return false;
                    }
                }
            } else if (!energy.DeserializeRecord(bytes, 0) || energy.SerializeRecord(0) != bytes) {
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
            for (const auto& [owner, age] : ledger.energy_ages()) {
                (void)owner;
                if (age > clock.tick()) {
                    error = "corrupt regional energy field age";
                    return false;
                }
            }
        }

        std::vector<std::string> hash_errors;
        bool exists = false;
        if (!hash_file(path,
                       out.bytes,
                       out.fnv64,
                       exists,
                       &hash_errors,
                       save_member_byte_limit(relative))) {
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

bool WorldSaveService::derived_member_digest(const std::filesystem::path& save_dir,
                                             std::string_view relative_path,
                                             std::uint64_t source_generation_limit,
                                             MemberDigest& out,
                                             std::string& error,
                                             MemberDigestFn catalog_digest,
                                             World::FarOverlayTile* overlay) {
    out = {};
    const auto cls = ClassifySnapshotMember(relative_path);
    if (!cls.has_value() || *cls != MemberClass::Derived) {
        error = "not a canonical derived member";
        return false;
    }
    try {
        const auto path = save_dir / std::string(relative_path);
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec) {
            error = "cannot inspect derived member";
            return false;
        }
        if (!exists)
            return true;
        if (relative_path == "far/catalog.json") {
            if (std::filesystem::file_size(path) > kMaxSnapshotIndexBytes || !catalog_digest) {
                error = "present catalog requires bounded catalog validation";
                return false;
            }
            if (!catalog_digest(save_dir, relative_path, out, error))
                return false;
            if (!out.present) {
                error = "present catalog validated as absent";
                return false;
            }
            return true;
        }
        const auto key = World::ParseFarOverlayFileName(relative_path.substr(4));
        if (!key.has_value()) {
            error = "invalid overlay path";
            return false;
        }
        // Validate and fingerprint the same bounded bytes from one regular-file handle.
        // Reopening the path after decoding could receipt an unrelated replacement.
        std::string bytes;
        if (!read_bounded_save_file(path, bytes, save_member_byte_limit(relative_path), nullptr)) {
            error = "cannot read derived overlay";
            return false;
        }
        auto decoded = World::DecodeFarOverlay(bytes, *key);
        if (decoded.error != World::FarOverlayError::None) {
            error = std::string("invalid derived overlay: ") +
                    World::FarOverlayErrorName(decoded.error);
            return false;
        }
        if (decoded.tile.generation > source_generation_limit) {
            error = "future derived source generation";
            return false;
        }
        out.bytes = bytes.size();
        out.fnv64 = fnv1a64_bytes(bytes.data(), bytes.size());
        out.present = true;
        if (overlay)
            *overlay = std::move(decoded.tile);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool WorldSaveService::inspect_derived_member(const std::filesystem::path& save_dir,
                                              std::string_view relative_path,
                                              const SnapshotIndex& index,
                                              DerivedMemberState& state,
                                              std::string& error,
                                              MemberDigestFn catalog_digest,
                                              DerivedMemberValidation* verified) {
    if (verified)
        *verified = {};
    const auto record_state = [&](DerivedMemberState value) {
        state = value;
        if (verified)
            verified->state = value;
    };
    if (journal_pending(save_dir) && !SaveTransaction::active_for(save_dir)) {
        record_state(DerivedMemberState::RecoveryPending);
        error = "pending snapshot recovery";
        return false;
    }
    MemberDigest actual;
    const auto generation =
        index.generation == 0 ? std::numeric_limits<std::uint64_t>::max() : index.generation;
    if (!derived_member_digest(save_dir,
                               relative_path,
                               generation,
                               actual,
                               error,
                               catalog_digest,
                               verified ? &verified->overlay : nullptr))
        return false;
    if (verified)
        verified->digest = actual;
    if (!actual.present) {
        record_state(DerivedMemberState::Missing);
        return true;
    }
    const auto found =
        std::find_if(index.members.begin(),
                     index.members.end(),
                     [relative_path](const auto& member) { return member.path == relative_path; });
    if (found == index.members.end() || !found->present) {
        record_state(DerivedMemberState::Unreceipted);
        return true;
    }
    if (found->cls != MemberClass::Derived || found->generation > index.generation) {
        error = "invalid derived membership";
        return false;
    }
    record_state(found->bytes == actual.bytes && found->fnv64 == actual.fnv64
                     ? DerivedMemberState::Receipted
                     : DerivedMemberState::Stale);
    return true;
}

bool WorldSaveService::verify_derived_members(
    const std::filesystem::path& save_dir,
    const SnapshotIndex& index,
    std::vector<std::string>* errors,
    MemberDigestFn catalog_digest,
    std::size_t max_entries,
    std::uint64_t max_overlay_bytes,
    const std::function<bool(std::string_view, DerivedMemberValidation&&)>& visitor) {
    if (journal_pending(save_dir) && !SaveTransaction::active_for(save_dir)) {
        AddError(errors, "pending snapshot recovery");
        return false;
    }
    try {
        std::set<std::string> paths;
        for (const auto& member : index.members)
            if (member.cls == MemberClass::Derived)
                paths.insert(member.path);
        const auto directory = save_dir / "far";
        std::error_code ec;
        const bool exists = std::filesystem::exists(directory, ec);
        if (ec)
            throw std::runtime_error("cannot inspect far directory");
        std::size_t visits = 0;
        std::uint64_t overlay_bytes = 0;
        if (exists) {
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                if (++visits > std::min(max_entries, kMaxSnapshotMembers))
                    throw std::runtime_error("derived directory entry limit");
                const auto name = Filesystem::GenericPathString(entry.path().filename());
                const auto relative = "far/" + name;
                const auto key = World::ParseFarOverlayFileName(name);
                // Admit the entire recognized overlay set before the first payload read.
                if (key) {
                    if (!entry.is_regular_file())
                        throw std::runtime_error("derived overlay is not a regular file");
                    const auto bytes = entry.file_size();
                    if (bytes > World::kFsv1MaxFileBytes ||
                        bytes > max_overlay_bytes - overlay_bytes)
                        throw std::runtime_error("derived overlay byte limit");
                    overlay_bytes += bytes;
                }
                // Recognition is broader than publication eligibility: a parsed key outside
                // codec bounds is present malformed data, not an absent optional cache.
                if (relative == "far/catalog.json" || key.has_value())
                    paths.insert(relative);
                if (paths.size() > kMaxSnapshotMembers)
                    throw std::runtime_error("derived member limit");
            }
        }
        for (const auto& path : paths) {
            DerivedMemberValidation verified;
            std::string error;
            if (!inspect_derived_member(save_dir,
                                        path,
                                        index,
                                        verified.state,
                                        error,
                                        catalog_digest,
                                        visitor ? &verified : nullptr)) {
                std::string message = path;
                message += ": ";
                message += error;
                AddError(errors, message);
                return false;
            }
            if (visitor && !visitor(path, std::move(verified))) {
                AddError(errors, path + ": derived validation consumer refused");
                return false;
            }
        }
        return true;
    } catch (const std::exception& exception) {
        AddError(errors, std::string("derived validation failed: ") + exception.what());
        return false;
    }
}

bool WorldSaveService::write_member_bytes(const std::filesystem::path& save_dir,
                                          std::string_view relative_path,
                                          const std::string& bytes,
                                          std::vector<std::string>* errors) {
    const auto cls = ClassifySnapshotMember(relative_path);
    if (!cls.has_value() || *cls != MemberClass::Authoritative) {
        AddError(errors, "member path is not allowed: " + std::string(relative_path));
        return false;
    }
    if (bytes.size() > save_member_byte_limit(relative_path)) {
        AddError(errors, "member exceeds save size cap: " + std::string(relative_path));
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
    const auto cls = ClassifySnapshotMember(relative_path);
    if (!cls.has_value() || *cls != MemberClass::Authoritative) {
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
            std::string bytes;
            if (!read_bounded_save_file(metadata_path, bytes, kMaxSaveMetadataBytes, nullptr))
                return false;
            metadata = nlohmann::json::parse(bytes);
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
        const bool indexed = snapshot_index_exists(save_dir);
        if (require_index && !indexed)
            return fail("inconsistent snapshot: index missing");
        std::vector<RegionRecord> records;
        if (std::filesystem::exists(path) && !ReadRegionFile(path, records, &errors))
            return fail(errors.empty() ? "unreadable chunk region" : errors.front());
        if (!VerifyRegionMembership(path, records, &errors))
            return fail(errors.empty() ? "inconsistent snapshot" : errors.front());
        const auto id = Chunk::calculate_id(coords);
        for (const auto& record : records) {
            if (record.lod_level != 0 || record.id != id)
                continue;
            result.chunk = DecodeChunkRecord(record, path, &errors, require_index || indexed);
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

bool WorldSaveService::copy_authoritative_chunks(const std::filesystem::path& source,
                                                 const std::filesystem::path& destination,
                                                 const std::vector<IVec3>& coords,
                                                 std::vector<std::string>* errors,
                                                 bool require_index) try {
    if (coords.empty())
        return true;
    if (!SaveTransaction::active_for(destination)) {
        AddError(errors, "durable region copy requires a destination snapshot transaction");
        return false;
    }
    if (auto* cache = ChunkValidationCache::active())
        cache->exclude_save_directory(destination);
    SnapshotIndex index;
    const bool indexed = snapshot_index_exists(source);
    if ((require_index && !indexed) || (indexed && !read_snapshot_index(source, index, errors))) {
        AddError(errors, "inconsistent snapshot: source index unavailable");
        return false;
    }
    if (!indexed && !validate_save(source, errors))
        return false;
    std::map<std::pair<int, int>, std::map<ChunkID, IVec3>> regions;
    for (const auto& coord : coords) {
        int rx = 0, rz = 0;
        region_coords_for_chunk(coord, rx, rz);
        regions[{rx, rz}].emplace(Chunk::calculate_id(coord), coord);
    }
    std::vector<std::string> members = {"chunks/region/world-manifest.json"};
    for (const auto& [region, expected] : regions)
        members.push_back(RelativeRegionMember(region.first, region.second));
    TransactionScope scope(destination, members, errors);
    if (!scope.ok() || !validate_save(destination, errors))
        return false;
    for (const auto& [region, expected] : regions) {
        const auto relative = RelativeRegionMember(region.first, region.second);
        const auto source_path = source / relative;
        // LMR1 bounds each region to 65535 compressed records. Keep one source/destination
        // region pair in memory and validate one decoded chunk at a time.
        std::vector<RegionRecord> records;
        if (!ReadRegionFile(source_path, records, errors) ||
            (indexed && !VerifyIndexedRegionMembership(relative, records, index, errors)))
            return false;
        const auto target_path = destination / relative;
        std::vector<RegionRecord> merged;
        if (std::filesystem::exists(target_path) && !ReadRegionFile(target_path, merged, errors))
            return false;
        std::map<std::pair<std::uint8_t, std::uint64_t>, std::size_t> positions;
        for (std::size_t i = 0; i < merged.size(); ++i)
            positions.emplace(std::pair{merged[i].lod_level, merged[i].id}, i);
        std::size_t copied = 0;
        for (auto& record : records) {
            const auto wanted = expected.find(record.id);
            if (record.lod_level != 0 || wanted == expected.end())
                continue;
            const auto validated = ValidateChunkRecord(record, source_path, errors, indexed);
            constexpr std::size_t kLattice = static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                             (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
            if (!validated || !validated->facts.authoritative ||
                validated->facts.coords != wanted->second ||
                (validated->facts.sdf_size != 0 && validated->facts.sdf_size != kLattice)) {
                AddError(errors, "corrupt exact chunk identity or SDF lattice during save-as");
                return false;
            }
            const auto previous = positions.find({record.lod_level, record.id});
            if (previous != positions.end())
                merged[previous->second] = std::move(record);
            else
                merged.push_back(std::move(record));
            ++copied;
        }
        if (copied != expected.size()) {
            AddError(errors, "expected durable chunk missing during save-as");
            return false;
        }
        std::filesystem::create_directories(target_path.parent_path());
        if (!WriteRegionFile(target_path, merged, errors))
            return false;
    }
    return WriteWorldManifest(world_manifest_path(destination), errors) && scope.commit(errors);
} catch (const std::exception& error) {
    AddError(errors, std::string("failed to copy durable regions: ") + error.what());
    return false;
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

bool WorldSaveService::validate_authoritative_relationships(const std::filesystem::path& save_dir,
                                                            std::vector<std::string>& errors,
                                                            const std::stop_token& stop,
                                                            bool admitted_capture) {
    world::ActiveRegionLedger ledger;
    if (!load_active_regions(ledger, save_dir, &errors))
        return false;
    if (std::filesystem::exists(active_regions_path(save_dir))) {
        world::WorldClock clock;
        bool required = false;
        if (!read_clock_metadata(save_dir, clock, required, &errors))
            return false;
        if (ledger.header_tick() > clock.tick()) {
            AddError(&errors, world::ActiveRegionLedger::kCorruptMessage);
            return false;
        }
        for (const auto& [owner, age] : ledger.energy_ages()) {
            (void)owner;
            if (age > clock.tick()) {
                errors.push_back("corrupt regional energy field age");
                return false;
            }
        }
    }
    world::GroundObjectStore ground;
    if (!validate_ground_objects(save_dir, ledger, &errors, &ground, admitted_capture))
        return false;
    world::FieldPageState foliage_pages;
    if (!validate_field_pages(save_dir, ledger, &errors, &foliage_pages))
        return false;
    Ecs::EntityRegistrySnapshot plants;
    if (stop.stop_requested()) {
        errors.push_back("Saved-world validation cancelled.");
        return false;
    }
    if (!load_plant_entities(plants, save_dir, &errors, admitted_capture))
        return false;
    luminumbra::ai::CreatureSnapshot creatures;
    if (!load_creature_entities(creatures, save_dir, &errors, admitted_capture))
        return false;
    if (snapshot_index_exists(save_dir) || plants.persistent_identity ||
        !creatures.entities.empty()) {
        std::set<std::uint64_t> creature_ids;
        for (const auto& record : creatures.entities)
            creature_ids.insert(record.id);
        std::string plant_error;
        if (!luminumbra::foliage::ValidatePlantEntitySnapshot(
                plants, ledger, ground, plant_error, creature_ids)) {
            errors.push_back(plant_error);
            return false;
        }
        std::string creature_error;
        if (!luminumbra::ai::ValidateCreatureSnapshot(
                creatures, ledger, ground, plants, creature_error)) {
            errors.push_back(creature_error);
            return false;
        }
        entt::registry decoded;
        luminumbra::foliage::ApplyPlantEntitySnapshot(decoded, plants);
        for (auto e : decoded.view<Components::PlantTag, Components::TransformComponent>()) {
            const auto cell = luminumbra::foliage::FoliageCell(
                decoded.get<Components::TransformComponent>(e).position);
            if ((decoded.all_of<Components::SoilFeederComponent>(e) &&
                 !foliage_pages.soil().initialized(cell)) ||
                (decoded.all_of<Components::WaterSourceComponent>(e) &&
                 !foliage_pages.irrigation().initialized(cell))) {
                errors.push_back("Corrupt plant authority: missing owned foliage cell");
                return false;
            }
        }
    }
    return true;
}

bool WorldSaveService::load_world(WorldStreamingState& state,
                                  const std::filesystem::path& save_dir,
                                  std::vector<std::string>& errors,
                                  const std::stop_token& stop,
                                  const std::function<bool(const IVec3&)>& retain_chunk,
                                  std::vector<IVec3>* authoritative_inventory,
                                  WorldSaveRecords* validated_records,
                                  MemberDigestFn catalog_digest) const {
    if (!catalog_digest)
        catalog_digest = &World::InspectFarEditedCatalogDigest;
    if (auto* cache = ChunkValidationCache::active())
        cache->exclude_save_directory(save_dir);
    const ChunkValidationCache::InventoryScope inventory(ChunkValidationCache::active());
    DecodeCounter().fetch_add(1, std::memory_order_acq_rel);
    state.clear();
    if (validated_records)
        *validated_records = {};
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
        if (journal_pending(save_dir) && !SaveTransaction::active_for(save_dir))
            return reject("pending snapshot recovery");
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
        if (!std::filesystem::exists(chunks_dir)) {
            verify_derived_members(save_dir, snapshot_index, &errors, catalog_digest);
            return false;
        }
        for (const auto& entry : std::filesystem::directory_iterator(chunks_dir)) {
            if (stop.stop_requested())
                return reject("Saved-world validation cancelled.");
            if (entry.path() != region_dir || !entry.is_directory())
                return reject("corrupt or unknown world persistence artifact: " +
                              entry.path().string());
        }
        if (!std::filesystem::exists(region_dir)) {
            verify_derived_members(save_dir, snapshot_index, &errors, catalog_digest);
            return false;
        }
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
                entry.path() == creature_entities_path(save_dir) ||
                entry.path() == ground_objects_path(save_dir) ||
                entry.path() == field_pages_path(save_dir) ||
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
            verify_derived_members(save_dir, snapshot_index, &errors, catalog_digest);
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
                    if (!LoadValidatedChunkRecord(record,
                                                  path,
                                                  rx,
                                                  rz,
                                                  indexed,
                                                  state,
                                                  errors,
                                                  retain_chunk,
                                                  decoded_ids,
                                                  authoritative_inventory))
                        return reject("");
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
        const bool has_ledger = std::filesystem::exists(active_regions_path(save_dir));
        if (has_ledger) {
            world::WorldClock clock;
            bool required = false;
            if (!read_clock_metadata(save_dir, clock, required, &errors))
                return reject("");
            if (ledger.header_tick() > clock.tick())
                return reject(world::ActiveRegionLedger::kCorruptMessage);
            for (const auto& [owner, age] : ledger.energy_ages()) {
                (void)owner;
                if (age > clock.tick())
                    return reject("corrupt regional energy field age");
            }
        }
        world::GroundObjectStore ground;
        if (!validate_ground_objects(save_dir, ledger, &errors, &ground))
            return reject("");
        world::FieldPageState foliage_pages;
        if (!validate_field_pages(save_dir, ledger, &errors, &foliage_pages))
            return reject("");
        Ecs::EntityRegistrySnapshot plants;
        if (stop.stop_requested())
            return reject("Saved-world validation cancelled.");
        if (!load_plant_entities(plants, save_dir, &errors))
            return reject("");
        luminumbra::ai::CreatureSnapshot creatures;
        if (!load_creature_entities(creatures, save_dir, &errors))
            return reject("");
        if (indexed || plants.persistent_identity || !creatures.entities.empty()) {
            std::set<std::uint64_t> creature_ids;
            for (const auto& record : creatures.entities)
                creature_ids.insert(record.id);
            std::string plant_error;
            if (!luminumbra::foliage::ValidatePlantEntitySnapshot(
                    plants, ledger, ground, plant_error, creature_ids))
                return reject(plant_error);
            std::string creature_error;
            if (!luminumbra::ai::ValidateCreatureSnapshot(
                    creatures, ledger, ground, plants, creature_error))
                return reject(creature_error);
            entt::registry decoded;
            luminumbra::foliage::ApplyPlantEntitySnapshot(decoded, plants);
            for (auto e : decoded.view<Components::PlantTag, Components::TransformComponent>()) {
                const auto cell = luminumbra::foliage::FoliageCell(
                    decoded.get<Components::TransformComponent>(e).position);
                if ((decoded.all_of<Components::SoilFeederComponent>(e) &&
                     !foliage_pages.soil().initialized(cell)) ||
                    (decoded.all_of<Components::WaterSourceComponent>(e) &&
                     !foliage_pages.irrigation().initialized(cell)))
                    return reject("Corrupt plant authority: missing owned foliage cell");
            }
        }
        if (indexed) {
            const std::string mismatch =
                VerifySnapshotIndex(save_dir, snapshot_index, region_digests);
            if (!mismatch.empty())
                return reject("inconsistent snapshot: " + mismatch);
        }
        if (!verify_derived_members(save_dir, snapshot_index, &errors, catalog_digest))
            return reject("");
        if (validated_records) {
            validated_records->ledger = std::move(ledger);
            validated_records->ground = std::move(ground);
            validated_records->pages = std::move(foliage_pages);
            validated_records->plants = std::move(plants);
            validated_records->creatures = std::move(creatures);
            validated_records->has_ledger = has_ledger;
        }
        return true;
    } catch (const std::exception& e) {
        if (validated_records)
            *validated_records = {};
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
        if (World::FarEditedCacheSaveScope::PolicyFor(save_dir)) {
            TransactionScope scope(save_dir, {}, errors);
            report.saved = scope.ok() && scope.commit(errors);
        }
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
    if (auto* cache = ChunkValidationCache::active())
        cache->exclude_save_directory(save_dir);
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

                if (!CompressChunkSnapshot(chunk, record, errors)) {
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
