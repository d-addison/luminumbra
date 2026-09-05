#pragma once

#include "world/WorldStreamingState.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::Ecs {
struct EntityRegistrySnapshot;
}

namespace Luminumbra::Persistence {

// Counters describing one incremental save pass over the streaming state.
struct WorldSaveDirtyReport {
    std::size_t chunks_total = 0;
    std::size_t chunks_dirty = 0;
    std::size_t regions_written = 0;
    bool saved = false;
};

// Persists WorldStreamingState snapshots beneath a world save directory.
//
// Current layout: chunks/region/r.<rx>.<rz>.lmr and world-manifest.json.
// LMR1: magic | u16 version=2 | u16 record_count | record headers | LZ4 payloads.
// Record headers: u64 id | u8 lod | u8 flags | u32 raw_size | u32 compressed_size.
// Lod-0 payloads retain canonical in-memory world_state_snapshot.v1 serialization.
// Obsolete disk formats are refused; migration is not supported.
class WorldSaveService {
public:
    static constexpr const char* kObsoleteWorldMessage =
        "This world predates the v0.3.0 format and cannot be opened. Create a new world; migration "
        "is not supported.";

    // Location used only to recognize and refuse obsolete snapshots.
    static std::filesystem::path world_state_path(const std::filesystem::path& save_dir);

    // v2 container locations.
    static std::filesystem::path region_directory(const std::filesystem::path& save_dir);
    static std::filesystem::path
    region_file_path(const std::filesystem::path& save_dir, int rx, int rz);
    static std::filesystem::path world_manifest_path(const std::filesystem::path& save_dir);

    //  plant entity snapshot persisted as a SIBLING to the chunk container
    // (region/plant-entities.json). Generic over the engine's EntityRegistrySnapshot; the plant
    // projection lives in foliage/PlantPersistence.h. An EMPTY snapshot writes NO file (and removes
    // a stale one) so a no-plant save is byte-identical. The chunk LMR1 path is UNTOUCHED.
    static std::filesystem::path plant_entities_path(const std::filesystem::path& save_dir);
    static bool save_plant_entities(const Luminumbra::Ecs::EntityRegistrySnapshot& snapshot,
                                    const std::filesystem::path& save_dir,
                                    std::vector<std::string>* errors = nullptr);
    // A missing file is a clean miss: out is emptied, returns true.
    static bool load_plant_entities(Luminumbra::Ecs::EntityRegistrySnapshot& out,
                                    const std::filesystem::path& save_dir,
                                    std::vector<std::string>* errors = nullptr);

    // Region addressing: rx = floor(chunk_x / 32), rz = floor(chunk_z / 32).
    static constexpr int kRegionChunkSpan = 32;
    static void region_coords_for_chunk(const IVec3& chunk_coords, int& out_rx, int& out_rz);

    // True only for a valid supported save. Obsolete artifacts are still refused by load_world.
    static bool has_world_save(const std::filesystem::path& save_dir);

    // Validates all existing persistence artifacts before a writer may modify them.
    // A missing save is valid. Diagnostics distinguish obsolete, future and corrupt files.
    static bool validate_save(const std::filesystem::path& save_dir,
                              std::vector<std::string>* errors = nullptr);

    // --- Raw LMR1 record access (far-LOD tiles, ) ---
    // Non-chunk payloads (lod_level 1/2 far tier records) share the chunk
    // region files; these helpers expose the container at record granularity
    // without duplicating the LZ4/layout code. Chunk writes preserve records
    // they do not own and vice versa (merge keyed on (lod_level, id)).
    struct ContainerRecord {
        u64 id = 0;
        u8 lod_level = 0;
        u8 flags = 0;
        std::string payload; // uncompressed payload bytes
    };
    // Reads and decompresses every record of a region file. A missing file is
    // a clean miss: returns true with out_records empty.
    static bool read_container_records(const std::filesystem::path& region_file,
                                       std::vector<ContainerRecord>& out_records,
                                       std::vector<std::string>* errors = nullptr);

    // Decodes the durable lod-0 chunk records from one LMR1 file. A missing
    // file is a clean empty result. Callers such as far-LOD cache recovery use
    // this to overlay simulation truth over older derived records without
    // loading every region in the world.
    static bool read_region_chunks(const std::filesystem::path& region_file,
                                   std::vector<std::shared_ptr<Chunk>>& out_chunks,
                                   std::vector<std::string>* errors = nullptr);

    // Bounded far-cache recovery variant: decodes only lod-0 records carrying
    // the sticky edited/authoritative flag, avoiding decompression of pristine
    // streamed chunks that cannot supersede FSD2 authority.
    static bool read_authoritative_region_chunks(const std::filesystem::path& region_file,
                                                 int min_chunk_x,
                                                 int max_chunk_x,
                                                 int min_chunk_z,
                                                 int max_chunk_z,
                                                 std::vector<std::shared_ptr<Chunk>>& out_chunks,
                                                 std::vector<std::string>* errors = nullptr);
    // Inserts/replaces the given records keyed (lod_level, id), preserving
    // all other records of the file verbatim.
    static bool upsert_container_records(const std::filesystem::path& region_file,
                                         const std::vector<ContainerRecord>& records,
                                         std::vector<std::string>* errors = nullptr);

    // Deterministic test seam for failure-atomic LMR1 rewrites. When armed,
    // the next region write stops after its unique temporary file has been
    // durably flushed, immediately before the atomic replacement of the live
    // path. The write reports failure and removes the temporary file. This is
    // process-global and must only be used by single-threaded persistence
    // tests.
    static void set_interrupt_before_region_replace_for_testing(bool enabled);

    // Serializes the full streaming state into the save directory, creating
    // intermediate directories as needed. Returns false (with diagnostics in
    // errors when provided) if the snapshot could not be written.
    bool save_world(const WorldStreamingState& state,
                    const std::filesystem::path& save_dir,
                    std::vector<std::string>* errors = nullptr) const;

    // Loads a previously saved snapshot into state. A missing snapshot file is
    // a clean miss (fresh world): returns false WITHOUT appending an error.
    // Malformed or unreadable snapshots return false with diagnostics.
    bool load_world(WorldStreamingState& state,
                    const std::filesystem::path& save_dir,
                    std::vector<std::string>& errors) const;

    // Deterministic hash of the streaming state, reusing the persistence hash
    // machinery (fnv1a_64 over the canonical snapshot bytes). Format
    // independent: computed over the in-memory snapshot, never file bytes.
    std::string world_hash(const WorldStreamingState& state) const;

    // Incremental save pass: O(edited regions). Only region files containing
    // at least one dirty chunk are rewritten (all in-memory chunks of those
    // regions are refreshed; records of chunks absent from memory and far-LOD
    // tile records are preserved verbatim). Dirty flags of the saved chunks
    // are cleared after a successful write. With no dirty chunks nothing is
    // written and saved stays false.
    WorldSaveDirtyReport save_dirty_chunks(WorldStreamingState& state,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors = nullptr) const;

private:
    // The single v2 write seam: serializes the given chunks into LMR1 region
    // files, merging with on-disk records, and refreshes the world manifest.
    // regions_written receives the
    // number of region files rewritten when provided.
    bool write_snapshot(const std::vector<std::shared_ptr<Chunk>>& chunks,
                        const std::filesystem::path& save_dir,
                        std::vector<std::string>* errors,
                        std::size_t* regions_written = nullptr) const;
};

} // namespace Luminumbra::Persistence
