#include "WorldSaveService.h"

#include "../ecs/EntitySnapshot.h" //  plant entity snapshot persistence
#include "WorldPersistenceRoundtrip.h"

#include "nlohmann/json.hpp"

#include <lz4.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
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
constexpr const char* kRegionDirectoryName = "region";
constexpr const char* kWorldManifestFileName = "world-manifest.json";
constexpr const char* kWorldManifestSchema = "luminumbra.persistence.world_manifest.v1";
constexpr const char* kRegionFileExtension = ".lmr";

// Container constants (the deterministic runtime contract section 3).
constexpr char kRegionMagic[4] = {'L', 'M', 'R', '1'};
constexpr std::uint16_t kRegionVersion = 2;
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
                 "failed to atomically replace region file: " + destination.string() + ": " +
                     WindowsErrorMessage(error));
        return false;
    }

    const DWORD attributes_error = ::GetLastError();
    if (attributes_error != ERROR_FILE_NOT_FOUND && attributes_error != ERROR_PATH_NOT_FOUND) {
        AddError(errors,
                 "failed to inspect region file before replacement: " + destination.string() +
                     ": " + WindowsErrorMessage(attributes_error));
        return false;
    }
    if (::MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
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
    // The temporary file is always created beside the live LMR1 file.
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

#endif

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
    out_payload.assign(record.uncompressed_size, '\0');
    if (record.uncompressed_size == 0) {
        return record.compressed_payload.empty();
    }
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
                    std::vector<std::string>* errors) {
    out_records.clear();

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        AddError(errors, "failed to open region file for reading: " + path.string());
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
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
        AddError(
            errors,
            "region file uses retired pre-v0.3.0 LMR1 container version " +
                std::to_string(version) +
                " and cannot be opened; create a new world with v0.3.0 or later: " + path.string());
        return false;
    }
    const std::uint16_t record_count = ReadU16(data + 6);

    const std::size_t manifest_bytes = static_cast<std::size_t>(record_count) * kRecordHeaderSize;
    if (bytes.size() < kRegionFileHeaderSize + manifest_bytes) {
        AddError(errors, "region file record manifest is truncated: " + path.string());
        return false;
    }

    out_records.resize(record_count);
    std::size_t payload_offset = kRegionFileHeaderSize + manifest_bytes;
    for (std::uint16_t i = 0; i < record_count; ++i) {
        const unsigned char* header =
            data + kRegionFileHeaderSize + static_cast<std::size_t>(i) * kRecordHeaderSize;
        RegionRecord& record = out_records[i];
        record.id = ReadU64(header);
        record.lod_level = header[8];
        record.flags = header[9];
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

    if (!ReplaceRegionFileAtomically(temp_path, path, errors)) {
        RemoveTemporaryFile(temp_path);
        return false;
    }
    return true;
}

// A lod_level 0 record payload is the v1 chunk snapshot record: the canonical
// single-chunk world_state_snapshot.v1 JSON. Reusing the v1 serializer keeps
// the migration gate trivially true (identical field set and float formatting
// in both formats) and handles empty/band SDF chunks as plain empty
// arrays.
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

bool IsRegionFileName(const std::filesystem::path& path) {
    if (path.extension() != kRegionFileExtension) {
        return false;
    }
    const std::string stem = path.stem().string();
    return stem.rfind("r.", 0) == 0;
}

bool WriteWorldManifest(const std::filesystem::path& manifest_path,
                        std::vector<std::string>* errors) {
    const nlohmann::json manifest = {{"schema", kWorldManifestSchema},
                                     {"container", "LMR1"},
                                     {"container_version", kRegionVersion}};

    std::ofstream output(manifest_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        AddError(errors, "failed to open world manifest for writing: " + manifest_path.string());
        return false;
    }
    output << std::setw(4) << manifest << '\n';
    output.flush();
    if (!output.good()) {
        AddError(errors, "failed to write world manifest: " + manifest_path.string());
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path WorldSaveService::world_state_path(const std::filesystem::path& save_dir) {
    return save_dir / kChunksDirectoryName / "world-state.json";
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
    const std::filesystem::path path = plant_entities_path(save_dir);
    std::error_code ec;
    if (snapshot.entities.empty()) {
        // No plants -> no file (a no-plant save is byte-identical). Retire any stale file.
        std::filesystem::remove(path, ec);
        return true;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
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

void WorldSaveService::region_coords_for_chunk(const IVec3& chunk_coords,
                                               int& out_rx,
                                               int& out_rz) {
    out_rx = FloorDiv(chunk_coords.x, kRegionChunkSpan);
    out_rz = FloorDiv(chunk_coords.z, kRegionChunkSpan);
}

bool WorldSaveService::has_world_save(const std::filesystem::path& save_dir) {
    std::error_code ec;
    if (std::filesystem::exists(world_manifest_path(save_dir), ec) && !ec) {
        return true;
    }
    const std::filesystem::path region_dir = region_directory(save_dir);
    if (!std::filesystem::exists(region_dir, ec) || ec) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(region_dir, ec)) {
        if (!ec && entry.is_regular_file() && IsRegionFileName(entry.path())) {
            return true;
        }
    }
    return false;
}

bool WorldSaveService::read_container_records(const std::filesystem::path& region_file,
                                              std::vector<ContainerRecord>& out_records,
                                              std::vector<std::string>* errors) {
    out_records.clear();
    std::error_code exists_error;
    if (!std::filesystem::exists(region_file, exists_error) || exists_error) {
        return true; // clean miss
    }

    std::vector<RegionRecord> raw_records;
    if (!ReadRegionFile(region_file, raw_records, errors)) {
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
    std::error_code exists_error;
    if (!std::filesystem::exists(region_file, exists_error) || exists_error) {
        return true; // clean miss
    }

    std::vector<RegionRecord> raw_records;
    if (!ReadRegionFile(region_file, raw_records, errors)) {
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
    if (!std::filesystem::exists(region_file, exists_error) || exists_error) {
        return true; // clean miss
    }

    std::vector<RegionRecord> raw_records;
    if (!ReadRegionFile(region_file, raw_records, errors)) {
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

        return WriteRegionFile(region_file, raw_records, errors);
    } catch (const std::exception& e) {
        AddError(errors, std::string("failed to upsert region records: ") + e.what());
        return false;
    }
}

void WorldSaveService::set_interrupt_before_region_replace_for_testing(bool enabled) {
    InterruptBeforeRegionReplaceForTesting().store(enabled, std::memory_order_release);
}

bool WorldSaveService::save_world(const WorldStreamingState& state,
                                  const std::filesystem::path& save_dir,
                                  std::vector<std::string>* errors) const {
    return write_snapshot(state.snapshot_chunks(), save_dir, errors);
}

bool WorldSaveService::load_world(WorldStreamingState& state,
                                  const std::filesystem::path& save_dir,
                                  std::vector<std::string>& errors) const {
    const std::filesystem::path region_dir = region_directory(save_dir);
    std::error_code ec;
    std::vector<std::filesystem::path> region_files;
    const bool manifest_present = std::filesystem::exists(world_manifest_path(save_dir), ec) && !ec;
    if (std::filesystem::exists(region_dir, ec) && !ec) {
        for (const auto& entry : std::filesystem::directory_iterator(region_dir, ec)) {
            if (!ec && entry.is_regular_file() && IsRegionFileName(entry.path())) {
                region_files.push_back(entry.path());
            }
        }
    }
    if (!manifest_present && region_files.empty()) {
        return false;
    }

    std::sort(region_files.begin(), region_files.end());
    state.clear();
    // A torn/corrupt region that fails partway (truncated payload, bit-flipped
    // LZ4, a bad record after good ones) must NOT leave the caller with a
    // partially-populated world it would mistake for a real one. Every hard-error
    // exit clears the state so a rejected load is always observably empty (the
    // load_world contract: false => no usable world). The clean-miss / success
    // paths are unaffected.
    const auto reject = [&state]() {
        state.clear();
        return false;
    };
    for (const std::filesystem::path& path : region_files) {
        std::vector<RegionRecord> records;
        if (!ReadRegionFile(path, records, &errors)) {
            return reject();
        }
        for (const RegionRecord& record : records) {
            if (record.lod_level != 0) {
                continue; // far-LOD tile records are read via FarLodStore
            }
            const std::shared_ptr<Chunk> chunk = DecodeChunkRecord(record, path, &errors);
            if (!chunk) {
                return reject();
            }
            if (!state.insert_chunk(chunk)) {
                errors.push_back("duplicate chunk id across region files: " + path.string());
                return reject();
            }
        }
    }
    return true;
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

    if (!write_snapshot(chunks_to_write, save_dir, errors, &report.regions_written)) {
        return report;
    }

    for (const ChunkID id : dirty_ids) {
        if (const auto chunk = state.find_chunk(id)) {
            chunk->clear_voxel_data_dirty();
        }
    }
    report.saved = true;
    return report;
}

bool WorldSaveService::write_snapshot(const std::vector<std::shared_ptr<Chunk>>& chunks,
                                      const std::filesystem::path& save_dir,
                                      std::vector<std::string>* errors,
                                      std::size_t* regions_written) const {
    try {
        const std::filesystem::path region_dir = region_directory(save_dir);
        std::filesystem::create_directories(region_dir);

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
