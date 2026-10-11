#pragma once

#include "SaveTransaction.h"
#include "world/ActiveRegionLedger.h"
#include "world/GroundObjectStore.h"
#include "world/WorldClock.h"
#include "world/WorldStreamingState.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
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

// Inputs of the region simulation digest. Priority when ids collide: live over
// parked over durable edited records. Parked is the dirty-chunk parking area
// introduced by a later slice; it is empty until then.
struct RegionDigestInputs {
    std::vector<std::shared_ptr<Chunk>> live;
    std::vector<std::shared_ptr<Chunk>> parked;
    // Opaque ground object projection of the region (GroundObjectStore::region_bytes); empty when
    // the region holds no ground objects, which keeps the digest of such regions unchanged.
    std::string ground_object_bytes;
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
    // Also recorded in world_info.json, including before the first chunk save.
    static constexpr std::uint16_t kContainerVersion = 2;
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

    // Ground object store persisted as region/ground-objects.json (SIBLING to the chunk container).
    // An EMPTY store writes NO file (and removes a stale one). Absent file: out is cleared, true.
    static std::filesystem::path ground_objects_path(const std::filesystem::path& save_dir);
    static bool save_ground_objects(const world::GroundObjectStore& store,
                                    const std::filesystem::path& save_dir,
                                    std::vector<std::string>* errors = nullptr);
    static bool load_ground_objects(world::GroundObjectStore& out,
                                    const std::filesystem::path& save_dir,
                                    std::uint64_t next_id,
                                    std::vector<std::string>* errors = nullptr);
    // load_world refusal rules for ground objects: every object lies in a region the ledger
    // records and no object was spawned after the metadata clock. Absent file: true.
    static bool validate_ground_objects(const std::filesystem::path& save_dir,
                                        const world::ActiveRegionLedger& ledger,
                                        std::vector<std::string>* errors);

    static std::filesystem::path active_regions_path(const std::filesystem::path& save_dir);
    static bool save_active_regions(world::ActiveRegionLedger& ledger,
                                    const world::WorldClock& clock,
                                    const std::filesystem::path& save_dir,
                                    std::vector<std::string>* errors = nullptr);
    static bool load_active_regions(world::ActiveRegionLedger& ledger,
                                    const std::filesystem::path& save_dir,
                                    std::vector<std::string>* errors = nullptr,
                                    const world::WorldClock& absent_clock = world::WorldClock{});
    // Content-only projection region_content_v2: live over parked over durable records flagged
    // edited; pristine and Loading chunks excluded.
    static std::uint64_t region_simulation_digest(const std::filesystem::path& save_dir,
                                                  world::RegionKey key,
                                                  const RegionDigestInputs& inputs);

    // Region addressing: rx = floor(chunk_x / 32), rz = floor(chunk_z / 32).
    static constexpr int kRegionChunkSpan = 32;
    static void region_coords_for_chunk(const IVec3& chunk_coords, int& out_rx, int& out_rz);

    // True only for a valid supported save. Obsolete artifacts are still refused by load_world.
    static bool has_world_save(const std::filesystem::path& save_dir);
    // A ledger or other sibling record alone is not an initial chunk snapshot.
    static bool has_chunk_snapshot(const std::filesystem::path& save_dir);

    // Validates all existing persistence artifacts before a writer may modify them.
    // A missing save is valid. Diagnostics distinguish obsolete, future and corrupt files.
    static bool validate_save(const std::filesystem::path& save_dir,
                              std::vector<std::string>* errors = nullptr,
                              const std::stop_token& stop = {});

    // Uses the same durable temporary-file/atomic replacement path as chunk containers.
    // Refuses an unsupported existing save before touching its metadata.
    static bool save_metadata(const std::string& bytes,
                              const std::filesystem::path& save_dir,
                              std::vector<std::string>* errors = nullptr);
    // Prevalidate the incoming pair and keep the installed ledger tick <= clock tick
    // between replacements, including when an explicit snapshot rewinds the clock.
    // Host-thread-only, synchronous and non-reentrant across sessions. Production
    // callers own one save destination; debug builds assert if pair saves overlap.
    static bool save_metadata_and_active_regions(const std::string& bytes,
                                                 world::ActiveRegionLedger& ledger,
                                                 const std::filesystem::path& save_dir,
                                                 std::vector<std::string>* errors = nullptr);

    // Shared read-only clock validation for catalog, explicit snapshots and writers.
    // Missing metadata is a legacy snapshot with a default clock.
    static bool read_clock_metadata(const std::filesystem::path& save_dir,
                                    world::WorldClock& clock,
                                    bool& requires_active_regions,
                                    std::vector<std::string>* errors = nullptr,
                                    std::optional<Vec3>* ambient_anchor = nullptr);

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

    // Called after ledger staging is flushed/closed, before replacement. Returning
    // false fails the write and cleans up; a test subprocess may terminate here
    // to leave real crash debris. Single-threaded persistence tests only; nullptr
    // disables the hook.
    static void set_before_active_regions_replace_for_testing(bool (*hook)());

    // --- S4 durable primitives (SaveTransaction and flag-on writers) ---
    // Writes bytes beside `destination` (staged temp, file fsync), atomically replaces it and
    // flushes the directory. Fires the snapshot step hook "before-replace"/"after-replace". Binary,
    // no newline translation.
    static bool durable_write_file(const std::filesystem::path& destination,
                                   const std::string& bytes,
                                   std::vector<std::string>* errors = nullptr);
    // Unlinks `path` when it exists (a missing file is success) and flushes the parent directory.
    // Fires "before-delete"/"after-delete".
    static bool durable_remove(const std::filesystem::path& path,
                               std::vector<std::string>* errors = nullptr);
    // fsync of a directory (no-op success where the platform cannot open directories).
    static bool durable_flush_directory(const std::filesystem::path& directory,
                                        std::vector<std::string>* errors = nullptr);
    // Number of full load_world decodes since process start (single-decode contract tests).
    static std::uint64_t decode_count_for_testing();

    // Validates one snapshot member and computes its index fingerprint (MemberDigestFn-compatible).
    // Region files hash their lod-0 record stream and are reported absent when they hold no lod-0
    // record; every other member hashes its raw bytes after a format check.
    static bool member_digest(const std::filesystem::path& save_dir,
                              std::string_view relative_path,
                              MemberDigest& out,
                              std::string& error);
    // Durable single-member writers for flag-on GameSession members (plants, energy). They join the
    // active transaction for save_dir, else run an implicit one when the directory is indexed, else
    // just replace the file durably. relative_path must satisfy member_path_allowed.
    static bool write_member_bytes(const std::filesystem::path& save_dir,
                                   std::string_view relative_path,
                                   const std::string& bytes,
                                   std::vector<std::string>* errors = nullptr);
    static bool remove_member(const std::filesystem::path& save_dir,
                              std::string_view relative_path,
                              std::vector<std::string>* errors = nullptr);
    // Test-only: deletes snapshot.json and strips "snapshotFormat" from world_info.json so a test
    // can express legacy (pre-index) semantics for a directory a flag-on session created.
    static bool forget_snapshot_index_for_testing(const std::filesystem::path& save_dir);

    // Thread-local per-save facts. While live for a directory, validate_save / has_world_save /
    // has_chunk_snapshot reuse the first load_world result instead of decoding the world again, and
    // post-commit actions (dirty-flag clearing) are deferred until the owning transaction commits.
    class SaveFactsScope {
    public:
        explicit SaveFactsScope(const std::filesystem::path& save_dir);
        SaveFactsScope(const SaveFactsScope&) = delete;
        SaveFactsScope& operator=(const SaveFactsScope&) = delete;
        ~SaveFactsScope();
        static SaveFactsScope* active_for(const std::filesystem::path& save_dir);
        // First call decodes the world once; later calls return the cached verdict.
        bool validate(std::vector<std::string>* errors);
        void defer_until_commit(std::function<void()> action);
        // Runs and clears the deferred actions (after a successful commit).
        void run_deferred();
        // Drops the deferred actions (rollback).
        void discard_deferred();
        void invalidate_presence_facts();

    private:
        friend class WorldSaveService;
        std::filesystem::path m_save_dir;
        SaveFactsScope* m_previous = nullptr;
        bool m_validated = false;
        bool m_validated_ok = false;
        std::vector<std::string> m_diagnostics;
        bool m_presence_known = false;
        bool m_snapshot_present = false;
        bool m_chunk_snapshot_present = false;
        std::vector<std::function<void()>> m_deferred;
    };

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
                    std::vector<std::string>& errors,
                    const std::stop_token& stop = {}) const;

    // Deterministic hash of the streaming state, reusing the persistence hash
    // machinery (fnv1a_64 over the canonical snapshot bytes). Format
    // independent: computed over the in-memory snapshot, never file bytes.
    std::string world_hash(const WorldStreamingState& state) const;

    // Incremental writes: only region files containing
    // at least one dirty chunk are rewritten (all in-memory chunks of those
    // regions are refreshed; records of chunks absent from memory and far-LOD
    // tile records are preserved verbatim). Dirty flags of the saved chunks
    // are cleared after a successful write. With no dirty chunks nothing is
    // written and saved stays false. Before writing, validation reads the whole
    // existing save to refuse obsolete, mixed-format, or corrupt durable data.
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
