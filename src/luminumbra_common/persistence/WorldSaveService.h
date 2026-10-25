#pragma once

#include "world/ActiveRegionLedger.h"
#include "world/WorldClock.h"
#include "world/WorldStreamingState.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace Luminumbra::Ecs {
struct EntityRegistrySnapshot;
}

namespace Luminumbra::Persistence {

class CreatedWorldArtifacts;

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
    // Compose multiple persistence operations under the same process ownership.
    // Acquire only after quiescing jobs: a job may itself need a persistence writer.
    // Recursive acquisition lets the ordinary writers retain their own guards.
    // Caller-owned session inputs must remain stable until the sequence returns.
    class SaveOwnership {
    public:
        SaveOwnership();

    private:
        std::unique_lock<std::recursive_mutex> m_lock;
    };

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

    static std::filesystem::path active_regions_path(const std::filesystem::path& save_dir);
    static bool save_active_regions(world::ActiveRegionLedger& ledger,
                                    const world::WorldClock& clock,
                                    const std::filesystem::path& save_dir,
                                    std::vector<std::string>* errors = nullptr,
                                    CreatedWorldArtifacts* artifacts = nullptr);
    // Missing file: clear region state at absent_clock and preserve ledger.config().
    // Present file: decode its persisted scheduler settings along with the region state.
    static bool load_active_regions(world::ActiveRegionLedger& ledger,
                                    const std::filesystem::path& save_dir,
                                    std::vector<std::string>* errors = nullptr,
                                    const world::WorldClock& absent_clock = world::WorldClock{
                                        0, world::WorldCalendar::Legacy()});
    // Content-only projection region_content_v2: live over parked over durable records flagged
    // edited; pristine chunks excluded. Loading is excluded only for live/parked inputs.
    static std::uint64_t region_simulation_digest(const std::filesystem::path& save_dir,
                                                  world::RegionKey key,
                                                  const RegionDigestInputs& inputs);
    // Compatibility call shape: live chunks with no parked inputs, using the same v2 projection.
    static std::uint64_t
    region_simulation_digest(const std::filesystem::path& save_dir,
                             world::RegionKey key,
                             const std::vector<std::shared_ptr<Chunk>>& live_chunks);
    // Preserve braced chunk lists, including {}, without ambiguity with RegionDigestInputs.
    static std::uint64_t
    region_simulation_digest(const std::filesystem::path& save_dir,
                             world::RegionKey key,
                             std::initializer_list<std::shared_ptr<Chunk>> live_chunks);

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
                              std::vector<std::string>* errors = nullptr,
                              CreatedWorldArtifacts* artifacts = nullptr);
    // Prevalidate the incoming pair and keep the installed ledger tick <= clock tick
    // between replacements, including when an explicit snapshot rewinds the clock.
    // Writes are serialized within the process, including nested pair saves and
    // region merges. Callers keep their input objects stable until return.
    static bool save_metadata_and_active_regions(const std::string& bytes,
                                                 world::ActiveRegionLedger& ledger,
                                                 const std::filesystem::path& save_dir,
                                                 std::vector<std::string>* errors = nullptr,
                                                 CreatedWorldArtifacts* artifacts = nullptr);

    // Shared read-only clock validation for catalog, explicit snapshots and writers.
    // Missing metadata is a legacy snapshot with a pinned legacy clock and zero water cursor.
    // A present cursor must be an unsigned integer representable by size_t.
    static bool read_clock_metadata(const std::filesystem::path& save_dir,
                                    world::WorldClock& clock,
                                    bool& requires_active_regions,
                                    std::vector<std::string>* errors = nullptr,
                                    std::optional<Vec3>* ambient_anchor = nullptr,
                                    std::optional<world::WorldCalendar>* render_calendar = nullptr,
                                    std::size_t* water_sim_cursor = nullptr);

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

    // Read-only diagnostic of the staging filename base used by region/manifest
    // writers, before the unique temporary suffix. Region members stage in the
    // save root outside strict scans when the original suffix is chunks/region.
    // Other spellings retain standalone behavior. Recognition performs no filesystem
    // I/O; relocation preserves symlinks followed by parent components in that prefix.
    static std::filesystem::path region_staging_path(const std::filesystem::path& destination);

    // Deterministic test seam for failure-atomic LMR1 rewrites. When armed,
    // the next region write stops after its unique temporary file has been
    // durably flushed, immediately before the atomic replacement of the live
    // path. The write reports failure and removes the temporary file. This is
    // process-global and must only be used by single-threaded persistence
    // tests.
    static void set_interrupt_before_region_replace_for_testing(bool enabled);

    // Test seam for POSIX directory flushes: the next flush after `flushes_to_skip`
    // successful ones fails once, then the seam disarms. A negative value disarms it.
    // Flushes covered include ancestor entries for every replacement, publication
    // and removal directories (including already-absent files on retries); Windows
    // builds never consult it. Process-global; single-threaded persistence tests only.
    static void set_directory_flush_failure_for_testing(int flushes_to_skip);

    // Writes `bytes` to `destination` with the same durability contract as an LMR1
    // region rewrite: the parent directory is created, a unique temporary file in
    // the destination directory is flushed to stable storage, and it atomically
    // replaces the destination. On POSIX the entry of every ancestor directory is
    // flushed on every call, so a retry after a failed flush repeats the missing
    // flushes. A failed or interrupted write (including the interrupt-before-replace
    // test seam above) removes the temporary file; the destination is then the
    // previous complete file, or on POSIX the new complete file when only the final
    // directory flush failed, never a partial file. Used for derived files that live
    // beside the save, such as the far authority overlay. Before every write,
    // reclaim abandoned staging. All process writers serialize through cleanup
    // and replacement, independent of destination spelling. Independently launched
    // processes sharing a destination are outside this ownership contract.
    // With a creation journal, the parent must already be owned. Temporary and
    // published files remain journal-owned on failure until writers drain and rollback runs.
    static bool write_file_durably(const std::filesystem::path& destination,
                                   const std::string& bytes,
                                   std::vector<std::string>* errors = nullptr,
                                   CreatedWorldArtifacts* artifacts = nullptr);

    // Create a new file with checked, atomic no-replace publication. An occupied
    // entry (including a dangling symlink) is refused without changing its bytes.
    // All in-process writers share the same ownership and staging cleanup.
    static bool create_file_durably(const std::filesystem::path& destination,
                                    const std::string& bytes,
                                    std::vector<std::string>* errors = nullptr);

    // Rewrite and rename a parsed file under process save ownership. Refuse an
    // occupied distinct destination; publication to a missing destination uses
    // an atomic no-replace operation. Recheck expected source bytes before any
    // publication. Same-file/case/path aliases and hard links are supported.
    // Failed source removal rolls back a newly created destination when possible;
    // alias replacement and post-unlink flush failures may leave the complete new
    // file visible, as with write_file_durably's final-directory-flush failures.
    static bool rename_file_durably(const std::filesystem::path& source,
                                    const std::filesystem::path& destination,
                                    const std::string& expected_bytes,
                                    const std::string& bytes,
                                    std::vector<std::string>* errors = nullptr);

    // Retires a neutral optional record and reclaims abandoned staging without
    // creating directories. POSIX flushes the parent even on an already-absent
    // retry. Uses the same synchronization as durable writes.
    static bool remove_file_and_staging(const std::filesystem::path& destination,
                                        std::vector<std::string>* errors = nullptr);

    // Const diagnostic: number of writers currently waiting for process save
    // ownership. Includes all destinations; does not inspect or mutate the disk.
    static std::size_t waiting_save_writers();

    // Called after ledger staging is flushed/closed, before replacement. Returning
    // false fails the write and cleans up; a test subprocess may terminate here
    // to leave real crash debris. Install/clear while no writers are running.
    // The hook runs while save ownership is held; nullptr disables it.
    static void set_before_active_regions_replace_for_testing(bool (*hook)());

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
    // Supply the live cursor or the validated cursor restored from world metadata.
    // Chunk-only legacy fixtures use the zero default.
    std::string world_hash(const WorldStreamingState& state,
                           std::uint64_t water_sim_cursor = 0) const;

    // Incremental writes: only region files containing
    // at least one dirty chunk are rewritten (all in-memory chunks of those
    // regions are refreshed; records of chunks absent from memory and far-LOD
    // tile records are preserved verbatim). Dirty flags of the saved chunks
    // are cleared after a successful write unless acknowledge_dirty is false;
    // composite saves then acknowledge only after every sibling writer succeeds.
    // With no dirty chunks nothing is written and saved stays false.
    // Before writing, validation reads the whole
    // existing save to refuse obsolete, mixed-format, or corrupt durable data.
    WorldSaveDirtyReport save_dirty_chunks(WorldStreamingState& state,
                                           const std::filesystem::path& save_dir,
                                           std::vector<std::string>* errors = nullptr,
                                           bool acknowledge_dirty = true) const;

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
