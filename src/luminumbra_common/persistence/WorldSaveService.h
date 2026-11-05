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
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Luminumbra::Ecs {
struct EntityRegistrySnapshot;
}

namespace Luminumbra::Persistence {

class CreatedWorldArtifacts;
struct BoundPublicationStage;

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
    std::vector<IVec3> durable;
    // Legacy admitted saves need no index until their first complete indexed save.
    bool require_index = true;
    // Empty when the region holds no ground objects, preserving its existing digest.
    std::string ground_object_bytes{};
};

// One owner-thread digest operation admits the index once. Durable authority is cached by
// publication revision and native file versions; live and parked inputs are always projected.
class RegionDigestOperation {
public:
    RegionDigestOperation(const std::filesystem::path& save_dir, bool require_index);
    std::uint64_t digest(world::RegionKey key, const RegionDigestInputs& inputs);
    [[nodiscard]] std::size_t index_reads() const {
        return m_index_reads;
    }
    [[nodiscard]] std::size_t input_items() const {
        return m_input_items;
    }

private:
    std::filesystem::path m_save_dir;
    SnapshotIndex m_index;
    std::uint64_t m_publication_revision = 0;
    bool m_require_index = false;
    std::size_t m_index_reads = 0;
    std::size_t m_input_items = 0;
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

    // LMR payload wire version; metadata envelopes have their own version boundary.
    static constexpr std::uint16_t kContainerVersion = 2;
    static constexpr std::uint16_t kIndexedMetadataVersion = 3;
    struct SaveFormat {
        bool indexed = false;
        bool requires_index = false;
    };
    // Present invalid control files refuse; only v2 metadata/v1 manifests permit index absence.
    static bool read_save_format(const std::filesystem::path& save_dir,
                                 SaveFormat& format,
                                 std::vector<std::string>* errors = nullptr,
                                 const std::stop_token& stop = {},
                                 SnapshotIndex* admitted_index = nullptr);
    // Persistence integration calls this after payload writers and before the final index.
    // A deliberately absent metadata member stays absent; the manifest is always published.
    static bool finalize_indexed_envelope(const std::filesystem::path& save_dir,
                                          std::vector<std::string>* errors = nullptr,
                                          CreatedWorldArtifacts* artifacts = nullptr);
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
    // Own the admitted encoding so a compound save serializes plants once,
    // before any payload writes. Empty snapshots retain stale-file retirement.
    class PreparedPlantWrite {
    public:
        PreparedPlantWrite() = default;
        PreparedPlantWrite(const PreparedPlantWrite&) = delete;
        PreparedPlantWrite& operator=(const PreparedPlantWrite&) = delete;
        bool prepare(const Ecs::EntityRegistrySnapshot& snapshot,
                     std::vector<std::string>* errors = nullptr);
        bool publish(const std::filesystem::path& save_dir,
                     std::vector<std::string>* errors = nullptr) const;

    private:
        // Bounded by SavedInputLimits::kPlantFileBytes during preparation.
        std::string m_bytes;
        bool m_prepared = false;
        bool m_empty = true;
    };
    // A missing file is a clean miss: out is emptied, returns true.
    static bool load_plant_entities(Luminumbra::Ecs::EntityRegistrySnapshot& out,
                                    const std::filesystem::path& save_dir,
                                    std::vector<std::string>* errors = nullptr,
                                    const std::stop_token& stop = {});

    // Ground object store persisted as region/ground-objects.json (SIBLING to the chunk container).
    // Empty objects and delivery state write no file. Absent file clears both outputs.
    static std::filesystem::path ground_objects_path(const std::filesystem::path& save_dir);
    static bool save_ground_objects(const world::GroundObjectStore& store,
                                    const std::filesystem::path& save_dir,
                                    std::vector<std::string>* errors = nullptr,
                                    const world::GroundNotifications& notifications = {});
    static bool load_ground_objects(world::GroundObjectStore& out,
                                    const std::filesystem::path& save_dir,
                                    std::uint64_t next_id,
                                    std::vector<std::string>* errors = nullptr,
                                    const std::stop_token& stop = {},
                                    world::GroundNotifications* notifications = nullptr);
    // load_world refusal rules for ground objects: every object lies in a region the ledger
    // records and no object was spawned after the metadata clock. Absent file: true.
    static bool validate_ground_objects(const std::filesystem::path& save_dir,
                                        const world::ActiveRegionLedger& ledger,
                                        std::vector<std::string>* errors,
                                        const std::stop_token& stop = {});

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
                                    const world::WorldClock& absent_clock =
                                        world::WorldClock{0, world::WorldCalendar::Legacy()},
                                    const std::stop_token& stop = {});
    // Content-only projection region_content_v2: live over parked over durable records flagged
    // edited; pristine chunks excluded. Loading is excluded only for live/parked inputs.
    static std::uint64_t region_simulation_digest(const std::filesystem::path& save_dir,
                                                  world::RegionKey key,
                                                  const RegionDigestInputs& inputs,
                                                  const SnapshotIndex* admitted_index = nullptr);
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
    // Publish the object store, metadata and ledger at one snapshot commit point.
    static bool
    save_metadata_active_regions_and_ground_objects(const std::string& bytes,
                                                    const world::ActiveRegionLedger& ledger,
                                                    const world::GroundObjectStore& store,
                                                    const world::GroundNotifications& notifications,
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
                                    std::size_t* water_sim_cursor = nullptr,
                                    const std::stop_token& stop = {});

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
                                                 std::vector<std::string>* errors = nullptr,
                                                 const SnapshotIndex* admitted_index = nullptr);
    enum class ChunkReadStatus {
        Found,
        Absent,
        Error
    };
    struct ChunkReadResult {
        ChunkReadStatus status = ChunkReadStatus::Absent;
        std::shared_ptr<Chunk> chunk;
        std::string error;
    };
    // Owner-thread state retained by an open world's loader, released with that loader.
    // At most 16 regions / 256 MiB of compressed payload and one admitted envelope.
    class ChunkReader {
    public:
        ChunkReader();
        ~ChunkReader();
        ChunkReader(const ChunkReader&) = delete;
        ChunkReader& operator=(const ChunkReader&) = delete;

    private:
        friend class WorldSaveService;
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
    // Reads the exact lod-0 identity only. Indexed membership is checked before decoding;
    // missing records are distinct from corrupt/unreadable expected authority.
    static ChunkReadResult read_chunk(const std::filesystem::path& save_dir,
                                      const IVec3& coords,
                                      bool require_index = false,
                                      ChunkReader* reader = nullptr);
    // While alive, read_chunk calls on this thread admit each index and verify each region
    // file once, until any publication changes the snapshot. Every use checks fresh bytes.
    // Nested batches share the outer; a supplied ChunkReader persists across batches.
    class ChunkReadBatch {
    public:
        ChunkReadBatch();
        ~ChunkReadBatch();
        ChunkReadBatch(const ChunkReadBatch&) = delete;
        ChunkReadBatch& operator=(const ChunkReadBatch&) = delete;
        ChunkReadBatch(ChunkReadBatch&&) = delete;
        ChunkReadBatch& operator=(ChunkReadBatch&&) = delete;

    private:
        bool m_owner = false;
    };
    // Capture BEFORE serialization; call only after the complete snapshot commits. A changed
    // or saturated revision retains its dirty obligation.
    static std::function<void()>
    chunk_save_acknowledgement(const std::vector<std::shared_ptr<Chunk>>& chunks);
    // Inserts/replaces the given records keyed (lod_level, id), preserving
    // all other records of the file verbatim.
    static bool upsert_container_records(const std::filesystem::path& region_file,
                                         const std::vector<ContainerRecord>& records,
                                         std::vector<std::string>* errors = nullptr);

    // Compose a record update with an authority check under existing ownership.
    // The token must remain on its acquiring thread through publication.
    static bool upsert_container_records(const SaveOwnership& ownership,
                                         const std::filesystem::path& region_file,
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

    // Best-effort cleanup of abandoned temporary siblings only. Session saves use
    // this before validating optional authority that can itself be invalid.
    static void reclaim_file_staging(const std::filesystem::path& destination);

    // Retires a neutral optional record and reclaims abandoned staging without
    // creating directories. POSIX flushes the parent even on an already-absent
    // retry. Uses the same synchronization as durable writes.
    static bool remove_file_and_staging(const std::filesystem::path& destination,
                                        std::vector<std::string>* errors = nullptr);

    // Const diagnostic: number of operations waiting for process save ownership
    // or indexed snapshot publication. Includes all destinations; does not inspect or mutate the
    // disk.
    static std::size_t waiting_save_writers();

    // Called after ledger staging and ownership are flushed, before replacement. Returning
    // false fails the write and cleans up; a test subprocess may terminate here
    // to leave real crash debris. Install/clear while no writers are running.
    // The hook runs while save ownership is held; nullptr disables it.
    static void set_before_active_regions_replace_for_testing(bool (*hook)());

    // --- S4 durable primitives (SaveTransaction and flag-on writers) ---
    // Writes bytes beside `staging` (or destination when empty), then atomically replaces
    // destination and flushes its directory. A supplied staging parent must exist on the same
    // filesystem. Fires the snapshot step hook "before-replace"/"after-replace". Binary, no newline
    // translation.
    static bool durable_write_file(const std::filesystem::path& destination,
                                   const std::string& bytes,
                                   std::vector<std::string>* errors = nullptr,
                                   const std::filesystem::path& staging = {},
                                   std::array<std::uint64_t, 3>* published_identity = nullptr);
    using ArtifactIdentity = std::array<std::uint64_t, 3>;
    // Versioned journal records append through one verified handle; total control bytes stay
    // within kMaxSnapshotIndexBytes, retaining rollback_reserve bytes for recovery.
    // Empty expected identity requests exclusive creation.
    static bool append_journal_record(const std::filesystem::path& path,
                                      const std::string& record,
                                      ArtifactIdentity& identity,
                                      std::vector<std::string>* errors,
                                      std::size_t rollback_reserve = 0);
    // Recovery operations require the recorded object, including at the destructive operation.
    // A file with more than max_links names is reported as neither identified nor missing.
    static bool artifact_identity(const std::filesystem::path& path,
                                  ArtifactIdentity& identity,
                                  bool& missing,
                                  bool directory = false,
                                  unsigned max_links = std::numeric_limits<unsigned>::max());
    // Caller owns publication; this inspection does not reacquire the file's read lock.
    static bool artifact_version(const std::filesystem::path& path,
                                 std::array<std::uint64_t, 8>& version,
                                 bool& missing);
    static bool remove_owned_entry(const std::filesystem::path& path,
                                   const ArtifactIdentity& identity,
                                   std::vector<std::string>* errors,
                                   bool directory = false,
                                   const BoundPublicationStage* stage = nullptr);
    static bool prepare_restoration(const std::filesystem::path& staging,
                                    const std::string& bytes,
                                    std::filesystem::path& temp,
                                    ArtifactIdentity& identity,
                                    std::vector<std::string>* errors);
    static bool publish_restoration(const std::filesystem::path& temp,
                                    const std::filesystem::path& destination,
                                    const ArtifactIdentity& expected,
                                    const ArtifactIdentity& incoming,
                                    std::vector<std::string>* errors,
                                    const BoundPublicationStage* source_stage = nullptr,
                                    const BoundPublicationStage* retired_stage = nullptr);
    // Unlinks `path` when it exists (a missing file is success) and flushes the parent directory.
    // Fires "before-delete"/"after-delete".
    static bool durable_remove(const std::filesystem::path& path,
                               std::vector<std::string>* errors = nullptr);
    // fsync of a directory (no-op success where the platform cannot open directories).
    static bool durable_flush_directory(const std::filesystem::path& directory,
                                        std::vector<std::string>* errors = nullptr);
    // Number of full load_world decodes since process start (single-decode contract tests).
    static std::uint64_t decode_count_for_testing();
    // Number of region record payload compressions since process start.
    static std::uint64_t record_compression_count_for_testing();
    // Number of lod-0 chunk record payload decodes since process start.
    static std::uint64_t chunk_record_decode_count_for_testing();
    // Number of region lod-0 fingerprints hashed (not reused from identical bytes) since
    // process start.
    static std::uint64_t region_fingerprint_count_for_testing();
    // Full-container reads, excluding fresh manifest/requested-record checks.
    static std::uint64_t region_read_count_for_testing();
    // Validation decodes the calling thread's world-read scope currently holds for a later
    // retaining read, and their accounted bytes. The byte bound applies process-wide.
    static std::size_t pending_chunk_admission_count_for_testing();
    static std::size_t pending_chunk_admission_bytes_for_testing();
    static void set_pending_chunk_admission_byte_limit_for_testing(std::size_t bytes);
    // Compares the admitted-check entry of every lod-0 record in a region file with a fresh
    // decode of that record (which advances the decode counter). False when the file is
    // unreadable.
    struct AdmittedChunkRecordAudit {
        std::size_t records = 0;
        std::size_t admitted = 0;
        std::size_t mismatched = 0;
    };
    static bool audit_admitted_chunk_records_for_testing(const std::filesystem::path& region_file,
                                                         AdmittedChunkRecordAudit& audit);

    // Validates one snapshot member and computes its index fingerprint (MemberDigestFn-compatible).
    // Region files hash their lod-0 record stream and are reported absent when they hold no lod-0
    // record; every other member hashes its raw bytes after a format check.
    static bool member_digest(const std::filesystem::path& save_dir,
                              std::string_view relative_path,
                              MemberDigest& out,
                              std::string& error);
    // Durable single-member writers for flag-on GameSession members (plants, energy). They join the
    // active transaction for save_dir, else run an implicit one when the directory is indexed, else
    // just replace the file durably. An outer transaction must already protect the member.
    // relative_path must satisfy member_path_allowed.
    static bool write_member_bytes(const std::filesystem::path& save_dir,
                                   std::string_view relative_path,
                                   const std::string& bytes,
                                   std::vector<std::string>* errors = nullptr);
    static bool remove_member(const std::filesystem::path& save_dir,
                              std::string_view relative_path,
                              std::vector<std::string>* errors = nullptr);
    // Shares cold validation decodes with the read that adopts them during one world open.
    // Nested scopes join the outer scope; no mutable adopted chunks remain cached.
    class WorldReadScope {
    public:
        WorldReadScope();
        ~WorldReadScope();
        WorldReadScope(const WorldReadScope&) = delete;
        WorldReadScope& operator=(const WorldReadScope&) = delete;

    private:
        bool m_owner = false;
    };

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
        // Cached presence is diagnostic only; callers validate before consulting it.
        [[nodiscard]] bool presence_known() const {
            return m_presence_known;
        }
        [[nodiscard]] bool snapshot_present() const {
            return m_snapshot_present;
        }
        [[nodiscard]] bool chunk_snapshot_present() const {
            return m_chunk_snapshot_present;
        }

    private:
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
    // Preparation retains process save ownership through publication or destruction.
    // Inputs must remain stable while preparing; publication reuses compressed records.
    // Publication consumes the preparation, including on failure.
    // dirty_only selects every resident chunk in a dirty region, and no others.
    class PreparedWorldWrite {
    public:
        PreparedWorldWrite();
        ~PreparedWorldWrite();
        PreparedWorldWrite(const PreparedWorldWrite&) = delete;
        PreparedWorldWrite& operator=(const PreparedWorldWrite&) = delete;
        bool prepare(const WorldStreamingState& state,
                     const std::filesystem::path& save_dir,
                     bool dirty_only,
                     std::vector<std::string>* errors = nullptr);
        bool publish(std::vector<std::string>* errors = nullptr,
                     std::size_t* regions_written = nullptr);

    private:
        struct Data;
        std::unique_ptr<Data> m_data;
    };
    static bool admit_metadata_bytes(const std::string& bytes,
                                     std::vector<std::string>* errors = nullptr);
    static bool prepare_plant_entities(const Ecs::EntityRegistrySnapshot& snapshot,
                                       std::string& bytes,
                                       std::vector<std::string>* errors = nullptr);

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
                    const std::stop_token& stop = {},
                    const std::unordered_set<ChunkID>* retain_chunks = nullptr,
                    std::vector<IVec3>* authoritative_inventory = nullptr,
                    std::size_t* decoded_chunk_count = nullptr) const;

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
};

} // namespace Luminumbra::Persistence
