#pragma once

// Snapshot commit index, journal and recovery (ds slice S4).
//
// An "indexed" save directory contains chunks/region/snapshot.json. An indexed directory is only
// ever changed through a SaveTransaction: members are journaled (undo copies) before they are
// replaced, the new index is renamed last (the durable commit point), and the journal is deleted
// afterwards. Recovery at world open rolls an interrupted transaction back, or discards a stale
// journal when the committed index already carries the transaction's new generation.
//
// All functions here are owner-thread only. They never run unless a save directory is indexed or a
// flag-on GameSession save opens a transaction, so flag-off saves are untouched.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::Persistence {

enum class MemberClass : std::uint8_t {
    Authoritative,
    Derived
};

struct SnapshotMember {
    std::string path; // relative to the save root, '/' separators
    MemberClass cls = MemberClass::Authoritative;
    bool present = true;
    std::uint64_t bytes = 0;
    std::uint64_t fnv64 = 0;
    std::uint64_t generation = 0;
};

struct SnapshotIndex {
    std::uint64_t generation = 0; // 0 = no index
    std::uint64_t tick = 0;
    std::uint64_t txn_id = 0;
    std::vector<std::array<float, 3>> anchors;
    std::vector<SnapshotMember> members; // sorted by path, unique
};

inline constexpr const char* kSnapshotIndexSchema = "luminumbra.snapshot.v1";
inline constexpr const char* kJournalSchema = "luminumbra.journal.v1";
inline constexpr std::size_t kMaxSnapshotMembers = 1'048'576;
inline constexpr std::size_t kMaxSnapshotIndexBytes = 16u * 1024u * 1024u;
// One undo entry per bounded snapshot member, plus the index itself. The journal's extra undo
// metadata gets twice the existing 16 MiB index budget; both readers check the handle size before
// allocation.
inline constexpr std::size_t kMaxJournalEntries = kMaxSnapshotMembers + 1;
inline constexpr std::size_t kMaxJournalBytes = 2 * kMaxSnapshotIndexBytes;
// A member/undo allocation and whole-file hash never exceeds 256 MiB.
inline constexpr std::uint64_t kMaxSnapshotPayloadBytes = 256ull * 1024 * 1024;
// Reads a single checked regular-file handle. Missing is a clean result with exists=false;
// symlinks, devices, FIFOs, size changes and inputs above limit refuse before publication.
bool read_snapshot_bytes(const std::filesystem::path& path,
                         std::uint64_t limit,
                         std::string& out,
                         bool& exists,
                         std::vector<std::string>* errors);
inline constexpr const char* kSnapshotIndexRelativePath = "chunks/region/snapshot.json";

// <save>/chunks/region/snapshot.json
std::filesystem::path snapshot_index_path(const std::filesystem::path& save_dir);
// <save>/journal
std::filesystem::path journal_directory(const std::filesystem::path& save_dir);

// Allow-list for relative member paths: world_info.json, aether_state.efs and
// chunks/region/{world-manifest.json, plant-entities.json, ground-objects.json, field-pages.fpg,
// active-regions.arl, r.<int>.<int>.lmr}.
// Rejects empty, absolute, drive/UNC, backslash and any ".." component. snapshot.json itself is not
// a member.
bool member_path_allowed(std::string_view relative_path);

// Canonical deterministic JSON encoding / strict decoding. decode enforces the schema, sorted
// unique member paths, member_path_allowed, the member cap and the index size cap.
std::string encode_snapshot_index(const SnapshotIndex& index);
bool decode_snapshot_index(std::string_view bytes, SnapshotIndex& out, std::string& error);

// An absent index file is a clean miss: returns true with out = {} (generation 0).
bool read_snapshot_index(const std::filesystem::path& save_dir,
                         SnapshotIndex& out,
                         std::vector<std::string>* errors);
bool snapshot_index_exists(const std::filesystem::path& save_dir);

// FNV-1a 64 over raw bytes (shared by the index and the journal).
std::uint64_t fnv1a64_bytes(const void* data, std::size_t size);
// Whole-file size + FNV-1a 64. A missing file returns false with exists=false.
bool hash_file(const std::filesystem::path& path,
               std::uint64_t& bytes,
               std::uint64_t& fnv64,
               bool& exists,
               std::vector<std::string>* errors);

// True when <save>/journal exists. Read-only; used by the catalog.
bool journal_pending(const std::filesystem::path& save_dir);

enum class RecoveryOutcome : std::uint8_t {
    NothingToDo,
    DiscardedStale,
    RolledBack,
    Failed
};

// Idempotent and crash-safe (see the protocol in specs/ds/s4-impl.md). Failed appends a diagnostic.
RecoveryOutcome recover_pending_journal(const std::filesystem::path& save_dir,
                                        std::vector<std::string>* errors);

// Callback supplied by the integration layer so the transaction can compute the fingerprint of a
// touched member when committing (region files hash their lod-0 record stream, everything else
// hashes raw bytes) and validate it before publication. Returns false and fills error on a corrupt
// member.
struct MemberDigest {
    bool present = false;
    std::uint64_t bytes = 0;
    std::uint64_t fnv64 = 0;
};
using MemberDigestFn = bool (*)(const std::filesystem::path& save_dir,
                                std::string_view relative_path,
                                MemberDigest& out,
                                std::string& error);

class SaveTransaction {
public:
    // Opens the thread_local "active transaction" slot for save_dir. Only one may be active per
    // thread.
    SaveTransaction(std::filesystem::path save_dir,
                    std::uint64_t tick,
                    std::vector<std::array<float, 3>> anchors,
                    MemberDigestFn digest_fn);
    SaveTransaction(const SaveTransaction&) = delete;
    SaveTransaction& operator=(const SaveTransaction&) = delete;
    ~SaveTransaction(); // aborts (rolls back) when still Open; no-op once Committed

    // The active transaction of this thread for the same save_dir (canonical comparison), else
    // nullptr.
    static SaveTransaction* active_for(const std::filesystem::path& save_dir);

    // Runs recovery, loads the previous index (may be absent), and creates the journal directory.
    bool begin(std::vector<std::string>* errors);
    // Journals the given relative member paths (undo copies + durable txn.json) BEFORE the caller
    // replaces them. Already protected paths are skipped. The index file is protected on the first
    // call.
    bool protect(const std::vector<std::string>& relative_paths, std::vector<std::string>* errors);
    // Commits: validates and re-hashes every protected member via digest_fn, publishes the new
    // index (durable commit point), then deletes the journal. Failure before the commit point rolls
    // back.
    bool commit(std::vector<std::string>* errors);
    // Joined writers acknowledge memory only at this transaction's durable commit point.
    // Aborted transactions discard their callbacks.
    void defer_until_commit(std::function<void()> action);
    // Rolls back (Open -> RolledBack). No-op after commit.
    void abort(std::vector<std::string>* errors);

    [[nodiscard]] bool committed() const {
        return m_state == State::Committed;
    }
    [[nodiscard]] const std::filesystem::path& save_dir() const {
        return m_save_dir;
    }
    [[nodiscard]] std::uint64_t new_generation() const {
        return m_new_generation;
    }
    [[nodiscard]] bool was_indexed() const {
        return m_previous.generation != 0;
    }

private:
    enum class State : std::uint8_t {
        Idle,
        Open,
        Committing,
        Committed,
        RolledBack
    };
    struct Entry {
        std::string path;
        std::string action; // "replace" | "create"
        std::uint64_t prev_bytes = 0;
        std::uint64_t prev_fnv64 = 0;
        std::string undo; // relative to the journal directory, empty for create
    };
    bool write_journal(std::vector<std::string>* errors);

    std::filesystem::path m_save_dir;
    std::uint64_t m_tick = 0;
    std::vector<std::array<float, 3>> m_anchors;
    MemberDigestFn m_digest_fn = nullptr;
    State m_state = State::Idle;
    SnapshotIndex m_previous;
    std::uint64_t m_txn_id = 0;
    std::uint64_t m_new_generation = 0;
    std::vector<Entry> m_entries;
    // Membership lookup never scans the journal prefix; at most kMaxJournalEntries paths.
    std::set<std::string> m_protected_paths;
    std::vector<std::function<void()>> m_committed_actions;
    std::uint64_t m_next_undo = 0;
    bool m_registered = false;
};

// Test hook: invoked at real publication sites ("journal-published", "before-replace",
// "after-replace", "before-delete", "after-delete", "commit-point", "journal-unlinked",
// "rollback-entry").
using SnapshotStepHook = void (*)(const char* step, const std::filesystem::path& path);
void set_snapshot_step_hook_for_testing(SnapshotStepHook hook);
void report_snapshot_step(const char* step, const std::filesystem::path& path);

} // namespace Luminumbra::Persistence
