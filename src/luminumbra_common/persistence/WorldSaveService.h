#pragma once

#include "world/ActiveRegionLedger.h"
#include "world/WorldClock.h"
#include "world/WorldStreamingState.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::Ecs {
struct EntityRegistrySnapshot;
}

namespace Luminumbra::Persistence {

class RegionContentHasher;
class AuthorityHashCache;
struct WorldAuthorityHashes;

// Counters describing one incremental save pass over the streaming state.
struct WorldSaveDirtyReport {
    std::size_t chunks_total = 0;
    std::size_t chunks_dirty = 0;
    std::size_t regions_written = 0;
    bool saved = false;
};

// Inputs of the region simulation digest. Priority when ids collide: live over
// parked over durable edited records. The scheduler supplies the complete
// resident region authority in live; durable-only records are merged separately.
struct RegionDigestInputs {
    std::vector<std::shared_ptr<Chunk>> live;
    std::vector<std::shared_ptr<Chunk>> parked;
    // Complete authoritative content retained independently of streaming. Its
    // lifecycle can be Loading during a render rebuild without losing authority.
    bool complete_authority = false;
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
                                    std::vector<std::string>* errors = nullptr);
    // Missing file: clear region state at absent_clock and preserve ledger.config().
    // Present file: decode its persisted scheduler settings along with the region state.
    static bool load_active_regions(world::ActiveRegionLedger& ledger,
                                    const std::filesystem::path& save_dir,
                                    std::vector<std::string>* errors = nullptr,
                                    const world::WorldClock& absent_clock = world::WorldClock{});
    // Content-only projection region_content_v2: live over parked over durable records flagged
    // edited; pristine chunks excluded. Loading is excluded for incomplete live/parked inputs.
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

    // Same complete live-over-durable merge as freeze digests. Streams one
    // decoded durable chunk at a time; no second resident voxel collection.
    static WorldAuthorityHashes
    authority_hashes(const std::filesystem::path& save_dir,
                     const std::vector<std::shared_ptr<Chunk>>& live_chunks);

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
    // Writes are serialized within the process, including nested pair saves and
    // region merges. Callers keep their input objects stable until return.
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
                                    std::optional<Vec3>* ambient_anchor = nullptr,
                                    std::size_t* water_cursor = nullptr);

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
    // Owner-thread cursor: keeps the bounded manifest for one region, decoding at
    // most one complete chunk at a time. Render caches are never restored.
    class AuthorityReader {
    public:
        AuthorityReader();
        ~AuthorityReader();
        AuthorityReader(AuthorityReader&&) noexcept;
        AuthorityReader& operator=(AuthorityReader&&) noexcept;
        bool open(const std::filesystem::path& region_file);
        std::shared_ptr<Chunk> read_next();
        // Indexed lookup leaves the sequential cursor unchanged. Missing IDs
        // return null; malformed payloads throw without publishing a chunk.
        std::shared_ptr<Chunk> read(u64 id);
        std::optional<u64> next_id() const;
        void skip();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
    // Cold scan only, at load or after replacement of the durable authority root.
    static AuthorityHashCache load_authority_hash_cache(const std::filesystem::path& save_dir);
    static bool open_region_authority(const std::filesystem::path& region_file,
                                      AuthorityReader& reader);
    static std::shared_ptr<Chunk> read_next_authority(AuthorityReader& reader);
    // Atomic replacement of one complete simulation owner, preserving sibling records.
    // Regenerable chunks are refused before writing.
    // Unlike incremental saves this writes even when dirty was acknowledged.
    static bool persist_chunk_authority(const std::filesystem::path& save_dir,
                                        const std::shared_ptr<Chunk>& chunk);
    // Atomically install a Y=0 water owner and retire its validated equal-state
    // nonzero-Y sources. Preserves each source's terrain and every sibling record.
    // Inputs remain unchanged, including dirty flags, on failure.
    static bool persist_water_column_migration(const std::filesystem::path& save_dir,
                                               const std::shared_ptr<Chunk>& destination,
                                               const std::vector<std::shared_ptr<Chunk>>& sources);
    // Transfer durable authority for an explicit snapshot destination. Refuses
    // destination-only authority absent from both source and live owners before
    // writing; processes one region at a time. Live owners must be saved afterwards.
    static bool copy_chunk_authority(const std::filesystem::path& source,
                                     const std::filesystem::path& destination,
                                     std::span<const std::shared_ptr<Chunk>> live_chunks = {});
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
    static bool write_file_durably(const std::filesystem::path& destination,
                                   const std::string& bytes,
                                   std::vector<std::string>* errors = nullptr);

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
                    std::vector<std::string>* errors = nullptr,
                    bool preserve_authority = false) const;

    // Loads a previously saved snapshot into state. A missing snapshot file is
    // a clean miss (fresh world): returns false WITHOUT appending an error.
    // Malformed or unreadable snapshots return false with diagnostics. A bounded
    // runtime load retains at most max_resident_chunks complete chunks; all
    // remaining records are still decoded/validated one at a time and stay on
    // disk for region rehydration. Zero selects validation without residency.
    bool load_world(WorldStreamingState& state,
                    const std::filesystem::path& save_dir,
                    std::vector<std::string>& errors,
                    const std::stop_token& stop = {},
                    std::size_t max_resident_chunks = std::numeric_limits<std::size_t>::max(),
                    bool require_full_sdf_lattice = false,
                    bool migrate_legacy_water = false) const;

    // Deterministic hash of the streaming state, reusing the persistence hash
    // machinery (fnv1a_64 over the canonical snapshot bytes). Format
    // independent: computed over the in-memory snapshot, never file bytes.
    std::string world_hash(const WorldStreamingState& state) const;

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
                                           bool acknowledge_dirty = true,
                                           bool preserve_authority = false) const;

    // One synchronous owner-thread save operation. Begin validates every durable
    // record once and retains at most one chunk to determine snapshot presence.
    // No other writer may change the destination until this operation finishes.
    // A failed payload poisons the operation: metadata cannot acknowledge it.
    class SaveBatch {
    public:
        static std::optional<SaveBatch> begin(const std::filesystem::path& directory,
                                              std::vector<std::string>* errors = nullptr);
        SaveBatch(SaveBatch&& other) noexcept
            : m_ownership(std::move(other.m_ownership))
            , m_directory(std::move(other.m_directory))
            , m_hasWorldSave(other.m_hasWorldSave)
            , m_hasChunkSnapshot(other.m_hasChunkSnapshot)
            , m_valid(std::exchange(other.m_valid, false)) {}
        SaveBatch& operator=(SaveBatch&&) noexcept = delete;
        SaveBatch(const SaveBatch&) = delete;
        SaveBatch& operator=(const SaveBatch&) = delete;
        const std::filesystem::path& directory() const {
            return m_directory;
        }
        bool has_world_save() const {
            return m_hasWorldSave;
        }
        bool has_chunk_snapshot() const {
            return m_hasChunkSnapshot;
        }
        bool save_world(const WorldStreamingState& state,
                        std::vector<std::string>* errors,
                        bool preserve_authority);
        WorldSaveDirtyReport save_dirty_chunks(WorldStreamingState& state,
                                               std::vector<std::string>* errors,
                                               bool preserve_authority);
        bool save_plant_entities(const Ecs::EntityRegistrySnapshot& snapshot,
                                 std::vector<std::string>* errors = nullptr);
        bool save_metadata(const std::string& bytes, std::vector<std::string>* errors = nullptr);
        // Update only scheduler state after payloads; preserve legacy bytes when
        // the cursor is unchanged or both absent and zero.
        bool save_water_cursor(std::size_t cursor, std::vector<std::string>* errors = nullptr);
        bool save_metadata_and_active_regions(const std::string& bytes,
                                              world::ActiveRegionLedger& ledger,
                                              std::vector<std::string>* errors = nullptr);

    private:
        SaveBatch(std::filesystem::path directory,
                  bool has_world,
                  bool has_chunks,
                  SaveOwnership ownership)
            : m_ownership(std::move(ownership))
            , m_directory(std::move(directory))
            , m_hasWorldSave(has_world)
            , m_hasChunkSnapshot(has_chunks) {}
        SaveOwnership m_ownership;
        std::filesystem::path m_directory;
        bool m_hasWorldSave = false;
        bool m_hasChunkSnapshot = false;
        bool m_valid = true;
    };

private:
    static bool save_metadata_validated(const std::string& bytes,
                                        const std::filesystem::path& directory,
                                        std::vector<std::string>* errors);
    static bool save_pair_validated(const std::string& bytes,
                                    world::ActiveRegionLedger& ledger,
                                    const std::filesystem::path& directory,
                                    std::vector<std::string>* errors);
    static bool save_regions_validated(world::ActiveRegionLedger& ledger,
                                       const world::WorldClock& clock,
                                       const std::filesystem::path& directory,
                                       std::vector<std::string>* errors);
    static bool save_plants_validated(const Ecs::EntityRegistrySnapshot& snapshot,
                                      const std::filesystem::path& directory,
                                      std::vector<std::string>* errors);
    WorldSaveDirtyReport save_dirty_validated(WorldStreamingState& state,
                                              const std::filesystem::path& directory,
                                              std::vector<std::string>* errors,
                                              bool preserve_authority,
                                              bool acknowledge_dirty) const;
    static RegionContentHasher region_content_hasher(const std::filesystem::path& save_dir,
                                                     world::RegionKey key,
                                                     const RegionDigestInputs& inputs);

    // The single v2 write seam: serializes the given chunks into LMR1 region
    // files, merging with on-disk records, and refreshes the world manifest.
    // regions_written receives the
    // number of region files rewritten when provided.
    bool write_snapshot(const std::vector<std::shared_ptr<Chunk>>& chunks,
                        const std::filesystem::path& save_dir,
                        std::vector<std::string>* errors,
                        std::size_t* regions_written = nullptr,
                        bool preserve_authority = false) const;
};

} // namespace Luminumbra::Persistence
