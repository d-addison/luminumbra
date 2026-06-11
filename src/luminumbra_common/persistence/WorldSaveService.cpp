#include "WorldSaveService.h"

#include "WorldPersistenceRoundtrip.h"

#include "nlohmann/json.hpp"

#include <lz4.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

namespace Luminumbra::Persistence {
namespace {

constexpr const char* kChunksDirectoryName = "chunks";
constexpr const char* kWorldStateFileName = "world-state.json";
constexpr const char* kWorldStateBackupFileName = "world-state.json.bak";
constexpr const char* kRegionDirectoryName = "region";
constexpr const char* kWorldManifestFileName = "world-manifest.json";
constexpr const char* kWorldManifestSchema = "luminumbra.persistence.world_manifest.v1";
constexpr const char* kRegionFileExtension = ".lmr";

// Container constants (design-decisions.md section 3).
constexpr char kRegionMagic[4] = {'L', 'M', 'R', '1'};
constexpr std::uint16_t kRegionVersion = 1;
// Header: magic u32 | version u16 | record_count u16.
constexpr std::size_t kRegionFileHeaderSize = 8;
// Record header: u64 id | u8 lod_level | u8 flags | u32 uncompressed_size |
// u32 compressed_size.
constexpr std::size_t kRecordHeaderSize = 18;

constexpr std::uint8_t kRecordFlagEdited = 0x01;
constexpr std::uint8_t kRecordFlagWaterPresent = 0x02;

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
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
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

bool CompressPayload(const std::string& payload, RegionRecord& record, std::vector<std::string>* errors) {
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

bool DecompressPayload(const RegionRecord& record, std::string& out_payload, std::vector<std::string>* errors) {
    out_payload.assign(record.uncompressed_size, '\0');
    if (record.uncompressed_size == 0) {
        return record.compressed_payload.empty();
    }
    const int produced = LZ4_decompress_safe(
        record.compressed_payload.data(),
        out_payload.data(),
        static_cast<int>(record.compressed_payload.size()),
        static_cast<int>(record.uncompressed_size));
    if (produced < 0 || static_cast<std::uint32_t>(produced) != record.uncompressed_size) {
        AddError(errors, "LZ4 decompression of a region record failed");
        return false;
    }
    return true;
}

bool ReadRegionFile(
    const std::filesystem::path& path,
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
        AddError(errors, "unsupported LMR1 container version " + std::to_string(version) + ": " + path.string());
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
        const unsigned char* header = data + kRegionFileHeaderSize + static_cast<std::size_t>(i) * kRecordHeaderSize;
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

bool WriteRegionFile(
    const std::filesystem::path& path,
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

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        AddError(errors, "failed to open region file for writing: " + path.string());
        return false;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output.good()) {
        AddError(errors, "failed to write region file: " + path.string());
        return false;
    }
    return true;
}

// A lod_level 0 record payload is the v1 chunk snapshot record: the canonical
// single-chunk world_state_snapshot.v1 JSON. Reusing the v1 serializer keeps
// the migration gate trivially true (identical field set and float formatting
// in both formats) and handles empty/band SDF chunks (T-I3-1) as plain empty
// arrays.
std::string SerializeSingleChunkSnapshot(const std::shared_ptr<Chunk>& chunk) {
    WorldStreamingState single;
    single.insert_chunk(chunk);
    return SerializeWorldStreamingStateSnapshotJson(single);
}

std::shared_ptr<Chunk> DecodeChunkRecord(
    const RegionRecord& record,
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
        AddError(errors, "chunk record payload does not contain exactly one chunk: " + path.string());
        return nullptr;
    }
    if (chunks.front()->get_id() != record.id) {
        AddError(errors, "chunk record id does not match its payload: " + path.string());
        return nullptr;
    }
    return chunks.front();
}

std::uint8_t ChunkRecordFlags(const Chunk& chunk, std::uint8_t previous_flags) {
    std::uint8_t flags = 0;
    // Edited/authoritative is sticky: once a chunk record was persisted with
    // post-generation edits it stays authoritative even after the in-memory
    // dirty flag is cleared by the save.
    if (chunk.is_voxel_data_dirty() || (previous_flags & kRecordFlagEdited) != 0) {
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

bool WriteWorldManifest(
    const std::filesystem::path& manifest_path,
    std::vector<std::string>* errors) {
    // Preserve the durable entity id allocator across rewrites; the manifest
    // is its source of truth from T-I3-7 onward (just a persisted counter for
    // now - entity systems start allocating from it in a later task).
    std::uint64_t next_durable_entity_id = 1;
    {
        std::ifstream input(manifest_path);
        if (input.is_open()) {
            try {
                const nlohmann::json existing = nlohmann::json::parse(input);
                if (existing.contains("next_durable_entity_id")) {
                    next_durable_entity_id = existing.at("next_durable_entity_id").get<std::uint64_t>();
                }
            } catch (const std::exception&) {
                // A corrupt manifest is rebuilt with the default allocator.
            }
        }
    }

    const nlohmann::json manifest = {
        {"schema", kWorldManifestSchema},
        {"container", "LMR1"},
        {"container_version", kRegionVersion},
        {"next_durable_entity_id", next_durable_entity_id}
    };

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
    return save_dir / kChunksDirectoryName / kWorldStateFileName;
}

std::filesystem::path WorldSaveService::region_directory(const std::filesystem::path& save_dir) {
    return save_dir / kChunksDirectoryName / kRegionDirectoryName;
}

std::filesystem::path WorldSaveService::region_file_path(const std::filesystem::path& save_dir, int rx, int rz) {
    return region_directory(save_dir) /
        ("r." + std::to_string(rx) + "." + std::to_string(rz) + kRegionFileExtension);
}

std::filesystem::path WorldSaveService::world_manifest_path(const std::filesystem::path& save_dir) {
    return region_directory(save_dir) / kWorldManifestFileName;
}

void WorldSaveService::region_coords_for_chunk(const IVec3& chunk_coords, int& out_rx, int& out_rz) {
    out_rx = FloorDiv(chunk_coords.x, kRegionChunkSpan);
    out_rz = FloorDiv(chunk_coords.z, kRegionChunkSpan);
}

bool WorldSaveService::has_world_save(const std::filesystem::path& save_dir) {
    std::error_code ec;
    if (std::filesystem::exists(world_manifest_path(save_dir), ec) && !ec) {
        return true;
    }
    if (std::filesystem::exists(world_state_path(save_dir), ec) && !ec) {
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

bool WorldSaveService::read_container_records(
    const std::filesystem::path& region_file,
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

bool WorldSaveService::upsert_container_records(
    const std::filesystem::path& region_file,
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

bool WorldSaveService::save_world(
    const WorldStreamingState& state,
    const std::filesystem::path& save_dir,
    std::vector<std::string>* errors) const {
    return write_snapshot(state.snapshot_chunks(), save_dir, errors);
}

bool WorldSaveService::load_world(
    WorldStreamingState& state,
    const std::filesystem::path& save_dir,
    std::vector<std::string>& errors) const {
    // v2 sniff: a world manifest or any LMR1 region file selects the region
    // container; otherwise fall back to the legacy v1 single snapshot.
    const std::filesystem::path region_dir = region_directory(save_dir);
    std::error_code ec;
    std::vector<std::filesystem::path> region_files;
    bool v2_present = std::filesystem::exists(world_manifest_path(save_dir), ec) && !ec;
    if (std::filesystem::exists(region_dir, ec) && !ec) {
        for (const auto& entry : std::filesystem::directory_iterator(region_dir, ec)) {
            if (!ec && entry.is_regular_file() && IsRegionFileName(entry.path())) {
                region_files.push_back(entry.path());
            }
        }
    }
    if (!region_files.empty()) {
        v2_present = true;
    }

    if (v2_present) {
        std::sort(region_files.begin(), region_files.end());
        state.clear();
        for (const std::filesystem::path& path : region_files) {
            std::vector<RegionRecord> records;
            if (!ReadRegionFile(path, records, &errors)) {
                return false;
            }
            for (const RegionRecord& record : records) {
                if (record.lod_level != 0) {
                    continue; // far-LOD tile records are read via FarLodStore
                }
                const std::shared_ptr<Chunk> chunk = DecodeChunkRecord(record, path, &errors);
                if (!chunk) {
                    return false;
                }
                if (!state.insert_chunk(chunk)) {
                    errors.push_back("duplicate chunk id across region files: " + path.string());
                    return false;
                }
            }
        }
        return true;
    }

    // Legacy v1 single-snapshot load (migration path; still fully supported).
    const std::filesystem::path snapshot_path = world_state_path(save_dir);
    std::error_code exists_error;
    if (!std::filesystem::exists(snapshot_path, exists_error) || exists_error) {
        // Fresh world: nothing has been persisted yet. Not an error.
        return false;
    }

    std::ifstream input(snapshot_path, std::ios::binary);
    if (!input.is_open()) {
        errors.push_back("failed to open world state snapshot for reading: " + snapshot_path.string());
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        errors.push_back("failed to read world state snapshot: " + snapshot_path.string());
        return false;
    }

    return LoadWorldStreamingStateSnapshotJson(buffer.str(), state, errors);
}

std::string WorldSaveService::world_hash(const WorldStreamingState& state) const {
    return ComputeWorldStreamingStateHash(state);
}

WorldSaveDirtyReport WorldSaveService::save_dirty_chunks(
    WorldStreamingState& state,
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

bool WorldSaveService::write_snapshot(
    const std::vector<std::shared_ptr<Chunk>>& chunks,
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

        // First v2 save over a v1 world: retire the legacy snapshot to .bak
        // so the migration is reversible and the v2 files are authoritative.
        const std::filesystem::path v1_path = world_state_path(save_dir);
        std::error_code v1_exists_error;
        if (std::filesystem::exists(v1_path, v1_exists_error) && !v1_exists_error) {
            const std::filesystem::path backup_path =
                v1_path.parent_path() / kWorldStateBackupFileName;
            std::error_code rename_error;
            std::filesystem::remove(backup_path, rename_error);
            rename_error.clear();
            std::filesystem::rename(v1_path, backup_path, rename_error);
            if (rename_error) {
                AddError(errors, "failed to retire v1 snapshot to .bak: " + rename_error.message());
                return false;
            }
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
