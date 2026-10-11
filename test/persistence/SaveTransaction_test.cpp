#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/WorldSaveService.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace Luminumbra::Persistence {

namespace {

using TreeMap = std::map<std::string, std::string>;

class TempSave {
public:
    TempSave() {
        static std::atomic<std::uint64_t> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string name = "lum_snaptxn_";
#ifndef _WIN32
        name += std::to_string(static_cast<long long>(::getpid())) + "_";
#endif
        name += std::to_string(counter.fetch_add(1)) + "_" + std::to_string(stamp);
        m_path = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::create_directories(m_path / "chunks" / "region", ec);
    }
    ~TempSave() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    TempSave(const TempSave&) = delete;
    TempSave& operator=(const TempSave&) = delete;

    [[nodiscard]] const fs::path& path() const {
        return m_path;
    }

private:
    fs::path m_path;
};

void Put(const fs::path& file, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << file.string();
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string Get(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TreeMap Tree(const fs::path& root) {
    TreeMap out;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, ec);
    if (ec) {
        return out;
    }
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code status_ec;
        if (!it->is_regular_file(status_ec) || status_ec) {
            continue;
        }
        const std::string rel = fs::relative(it->path(), root, status_ec).generic_string();
        out[rel] = Get(it->path());
    }
    return out;
}

void CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << from.string() << " -> " << to.string() << ": " << ec.message();
}

bool RawDigest(const fs::path& save_dir,
               std::string_view relative_path,
               MemberDigest& out,
               std::string& error) {
    bool exists = false;
    if (!hash_file(save_dir / std::string(relative_path), out.bytes, out.fnv64, exists, nullptr)) {
        error = "cannot hash " + std::string(relative_path);
        return false;
    }
    out.present = exists;
    return true;
}

bool RejectingDigest(const fs::path& save_dir,
                     std::string_view relative_path,
                     MemberDigest& out,
                     std::string& error) {
    if (relative_path == "aether_state.efs") {
        error = "boom";
        return false;
    }
    return RawDigest(save_dir, relative_path, out, error);
}

bool CommitOnce(const fs::path& dir,
                std::uint64_t tick,
                const std::map<std::string, std::string>& writes) {
    SaveTransaction txn(dir, tick, {}, &RawDigest);
    std::vector<std::string> errors;
    std::vector<std::string> keys;
    for (const auto& entry : writes) {
        keys.push_back(entry.first);
    }
    if (!txn.begin(&errors) || !txn.protect(keys, &errors)) {
        return false;
    }
    for (const auto& entry : writes) {
        if (!WorldSaveService::write_member_bytes(dir, entry.first, entry.second, &errors)) {
            return false;
        }
    }
    return txn.commit(&errors);
}

SnapshotMember MakeMember(const std::string& path,
                          MemberClass cls,
                          bool present,
                          std::uint64_t bytes,
                          std::uint64_t fnv64,
                          std::uint64_t generation) {
    SnapshotMember member;
    member.path = path;
    member.cls = cls;
    member.present = present;
    member.bytes = bytes;
    member.fnv64 = fnv64;
    member.generation = generation;
    return member;
}

SnapshotIndex MakeIndex() {
    SnapshotIndex index;
    index.generation = 3;
    index.tick = 9;
    index.txn_id = 4;
    index.anchors.push_back({1.0f, 2.0f, 3.0f});
    index.members.push_back(
        MakeMember("aether_state.efs", MemberClass::Authoritative, true, 10, 0x1234abcdULL, 3));
    index.members.push_back(MakeMember(
        "chunks/region/plant-entities.json", MemberClass::Authoritative, false, 0, 0, 3));
    index.members.push_back(
        MakeMember("world_info.json", MemberClass::Derived, true, 20, 0xfeedULL, 2));
    return index;
}

const SnapshotMember* FindMember(const SnapshotIndex& index, const std::string& path) {
    for (const SnapshotMember& member : index.members) {
        if (member.path == path) {
            return &member;
        }
    }
    return nullptr;
}

std::string ReplaceFirst(std::string text, const std::string& from, const std::string& to) {
    const std::size_t pos = text.find(from);
    if (pos != std::string::npos) {
        text.replace(pos, from.size(), to);
    }
    return text;
}

// Installs a hook for the lifetime of the guard and clears it on exit.
class HookGuard {
public:
    explicit HookGuard(SnapshotStepHook hook) {
        set_snapshot_step_hook_for_testing(hook);
    }
    ~HookGuard() {
        set_snapshot_step_hook_for_testing(nullptr);
    }
    HookGuard(const HookGuard&) = delete;
    HookGuard& operator=(const HookGuard&) = delete;
};

fs::path g_capture_source;
fs::path g_capture_target;

void CaptureJournalHook(const char* step, const fs::path& /*path*/) {
    if (std::string_view(step) == "commit-point" && fs::exists(g_capture_source)) {
        CopyTree(g_capture_source, g_capture_target);
    }
}

#if !defined(_WIN32)
// Death-test state. Death tests run in forked children (gtest "fast" style), so the parent-side
// paths are inherited unchanged by the child.
int g_kill_at = 0;
int g_calls = 0;
int g_published_at = 0;
int g_last_exit_code = -1;

void KillHook(const char* /*step*/, const fs::path& /*path*/) {
    if (++g_calls == g_kill_at) {
        std::_Exit(7);
    }
}

void RecordHook(const char* step, const fs::path& /*path*/) {
    ++g_calls;
    if (g_published_at == 0 && std::string_view(step) == "journal-published") {
        g_published_at = g_calls;
    }
}

// Runs in the parent after waitpid. Records the child's exit code for the caller.
bool KilledOrFinished(int status) {
    if (!WIFEXITED(status)) {
        return false;
    }
    g_last_exit_code = WEXITSTATUS(status);
    return g_last_exit_code == 7 || g_last_exit_code == 0;
}

bool RunScenario(const fs::path& dir) {
    return CommitOnce(dir,
                      5,
                      {{"world_info.json", "w1"},
                       {"aether_state.efs", "e1"},
                       {"chunks/region/plant-entities.json", "p1"}});
}

void ChildKillScenario(const fs::path& dir, int kill_at) {
    g_calls = 0;
    g_kill_at = kill_at;
    set_snapshot_step_hook_for_testing(&KillHook);
    const bool ok = RunScenario(dir);
    std::_Exit(ok ? 0 : 9);
}

void ChildKilledRecovery(const fs::path& dir, int kill_at) {
    g_calls = 0;
    g_kill_at = kill_at;
    set_snapshot_step_hook_for_testing(&KillHook);
    (void)recover_pending_journal(dir, nullptr);
    std::_Exit(0);
}

// Computes the expected post-commit tree by running the scenario in-process on a copy.
void ComputeFinalTree(const fs::path& baseline_dir, TreeMap& out) {
    TempSave scratch;
    CopyTree(baseline_dir, scratch.path());
    ASSERT_TRUE(RunScenario(scratch.path()));
    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(scratch.path(), index, nullptr));
    ASSERT_EQ(index.generation, 2u);
    out = Tree(scratch.path());
}
#endif

} // namespace

TEST(SaveTransaction, MemberPathAllowListAcceptsOnlyKnownShapes) {
    const std::vector<std::string> accepted = {
        "world_info.json",
        "aether_state.efs",
        "chunks/region/world-manifest.json",
        "chunks/region/plant-entities.json",
        "chunks/region/ground-objects.json",
        "chunks/region/field-pages.fpg",
        "chunks/region/active-regions.arl",
        "chunks/region/r.0.0.lmr",
        "chunks/region/r.-3.12.lmr",
    };
    for (const std::string& path : accepted) {
        EXPECT_TRUE(member_path_allowed(path)) << "path=" << path;
    }
    const std::vector<std::string> rejected = {
        "",
        "/world_info.json",
        "../world_info.json",
        "chunks/region/../world_info.json",
        "chunks\\region\\r.0.0.lmr",
        "C:/x/world_info.json",
        "chunks/region/snapshot.json",
        "chunks/region/r.a.b.lmr",
        "other.txt",
        "chunks/region/r.0.0.lmr.tmp.1.2",
    };
    for (const std::string& path : rejected) {
        EXPECT_FALSE(member_path_allowed(path)) << "path=" << path;
    }
}

TEST(SaveTransaction, IndexCodecRoundTripIsDeterministic) {
    const SnapshotIndex original = MakeIndex();
    const std::string encoded = encode_snapshot_index(original);

    SnapshotIndex decoded;
    std::string error;
    ASSERT_TRUE(decode_snapshot_index(encoded, decoded, error)) << error;
    EXPECT_EQ(decoded.generation, 3u);
    EXPECT_EQ(decoded.tick, 9u);
    EXPECT_EQ(decoded.txn_id, 4u);
    ASSERT_EQ(decoded.anchors.size(), 1u);
    EXPECT_EQ(decoded.anchors[0][0], 1.0f);
    EXPECT_EQ(decoded.anchors[0][1], 2.0f);
    EXPECT_EQ(decoded.anchors[0][2], 3.0f);
    ASSERT_EQ(decoded.members.size(), 3u);
    for (std::size_t i = 0; i < original.members.size(); ++i) {
        const SnapshotMember& want = original.members[i];
        const SnapshotMember& got = decoded.members[i];
        EXPECT_EQ(got.path, want.path) << "i=" << i;
        EXPECT_EQ(got.cls, want.cls) << "i=" << i;
        EXPECT_EQ(got.present, want.present) << "i=" << i;
        EXPECT_EQ(got.bytes, want.bytes) << "i=" << i;
        EXPECT_EQ(got.fnv64, want.fnv64) << "i=" << i;
        EXPECT_EQ(got.generation, want.generation) << "i=" << i;
    }
    EXPECT_EQ(encode_snapshot_index(decoded), encoded);
}

TEST(SaveTransaction, IndexDecodeRejectsMalformedInput) {
    const std::string good = encode_snapshot_index(MakeIndex());

    SnapshotIndex unsorted = MakeIndex();
    std::swap(unsorted.members.front(), unsorted.members.back());

    SnapshotIndex duplicated = MakeIndex();
    duplicated.members[1].path = duplicated.members[0].path;

    SnapshotIndex traversal = MakeIndex();
    traversal.members = {MakeMember("../x", MemberClass::Authoritative, true, 1, 1, 3)};

    SnapshotIndex not_allowed = MakeIndex();
    not_allowed.members = {MakeMember("other.txt", MemberClass::Authoritative, true, 1, 1, 3)};

    const std::vector<std::pair<std::string, std::string>> cases = {
        {"empty", ""},
        {"not json", "not json"},
        {"wrong schema", ReplaceFirst(good, kSnapshotIndexSchema, "luminumbra.snapshot.v9")},
        {"unsorted members", encode_snapshot_index(unsorted)},
        {"duplicate member", encode_snapshot_index(duplicated)},
        {"traversal member", encode_snapshot_index(traversal)},
        {"not allow-listed member", encode_snapshot_index(not_allowed)},
    };
    for (const auto& [name, bytes] : cases) {
        SnapshotIndex out;
        std::string error;
        EXPECT_FALSE(decode_snapshot_index(bytes, out, error)) << "case=" << name;
        EXPECT_FALSE(error.empty()) << "case=" << name;
    }
}

TEST(SaveTransaction, ReadSnapshotIndexAbsentIsCleanMiss) {
    TempSave save;
    SnapshotIndex index;
    index.generation = 99;
    ASSERT_TRUE(read_snapshot_index(save.path(), index, nullptr));
    EXPECT_EQ(index.generation, 0u);
    EXPECT_FALSE(snapshot_index_exists(save.path()));
}

TEST(SaveTransaction, CommitPublishesIndexAndDeletesJournal) {
    TempSave save;
    Put(save.path() / "world_info.json", "v1");

    ASSERT_TRUE(
        CommitOnce(save.path(), 7, {{"world_info.json", "v2"}, {"aether_state.efs", "e1"}}));

    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(save.path(), index, nullptr));
    EXPECT_EQ(index.generation, 1u);
    EXPECT_EQ(index.tick, 7u);
    EXPECT_FALSE(journal_pending(save.path()));
    EXPECT_FALSE(fs::exists(journal_directory(save.path())));

    const std::map<std::string, std::string> expected = {{"world_info.json", "v2"},
                                                         {"aether_state.efs", "e1"}};
    for (const auto& [path, bytes] : expected) {
        const SnapshotMember* member = FindMember(index, path);
        ASSERT_NE(member, nullptr) << "path=" << path;
        EXPECT_TRUE(member->present) << "path=" << path;
        EXPECT_EQ(member->bytes, bytes.size()) << "path=" << path;
        EXPECT_EQ(member->fnv64, fnv1a64_bytes(bytes.data(), bytes.size())) << "path=" << path;
        EXPECT_EQ(Get(save.path() / path), bytes) << "path=" << path;
    }

    const SnapshotMember* world_before = FindMember(index, "world_info.json");
    ASSERT_NE(world_before, nullptr);
    const SnapshotMember world_copy = *world_before;

    ASSERT_TRUE(CommitOnce(save.path(), 8, {{"aether_state.efs", "e2"}}));
    SnapshotIndex second;
    ASSERT_TRUE(read_snapshot_index(save.path(), second, nullptr));
    EXPECT_EQ(second.generation, 2u);
    const SnapshotMember* world_after = FindMember(second, "world_info.json");
    ASSERT_NE(world_after, nullptr);
    EXPECT_EQ(world_after->bytes, world_copy.bytes);
    EXPECT_EQ(world_after->fnv64, world_copy.fnv64);
    EXPECT_EQ(world_after->generation, world_copy.generation);
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, AbortRestoresPreviousBytesAndRemovesCreatedMembers) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "a"}}));
    const TreeMap before = Tree(save.path());

    {
        SaveTransaction txn(save.path(), 2, {}, &RawDigest);
        std::vector<std::string> errors;
        ASSERT_TRUE(txn.begin(&errors));
        ASSERT_TRUE(txn.protect({"world_info.json", "chunks/region/plant-entities.json"}, &errors));
        ASSERT_TRUE(
            WorldSaveService::write_member_bytes(save.path(), "world_info.json", "b", &errors));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(
            save.path(), "chunks/region/plant-entities.json", "p", &errors));
        txn.abort(&errors);
        EXPECT_FALSE(txn.committed());
    }

    EXPECT_EQ(Tree(save.path()), before);
    EXPECT_EQ(Get(save.path() / "world_info.json"), "a");
    EXPECT_FALSE(fs::exists(save.path() / "chunks/region/plant-entities.json"));
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, DestructorRollsBackAnOpenTransaction) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "a"}}));
    const TreeMap before = Tree(save.path());

    {
        SaveTransaction txn(save.path(), 2, {}, &RawDigest);
        std::vector<std::string> errors;
        ASSERT_TRUE(txn.begin(&errors));
        ASSERT_TRUE(txn.protect({"world_info.json", "chunks/region/plant-entities.json"}, &errors));
        ASSERT_TRUE(
            WorldSaveService::write_member_bytes(save.path(), "world_info.json", "b", &errors));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(
            save.path(), "chunks/region/plant-entities.json", "p", &errors));
    }

    EXPECT_EQ(Tree(save.path()), before);
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, FailedCommitValidationRollsBack) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "a"}}));
    const TreeMap before = Tree(save.path());

    SaveTransaction txn(save.path(), 2, {}, &RejectingDigest);
    std::vector<std::string> errors;
    ASSERT_TRUE(txn.begin(&errors));
    ASSERT_TRUE(txn.protect({"world_info.json", "aether_state.efs"}, &errors));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "b", &errors));
    ASSERT_TRUE(
        WorldSaveService::write_member_bytes(save.path(), "aether_state.efs", "e", &errors));

    EXPECT_FALSE(txn.commit(&errors));
    EXPECT_FALSE(txn.committed());
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(Tree(save.path()), before);
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, RecoveryDiscardsStaleJournalWhenIndexAlreadyCommitted) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "v0"}}));

    TempSave side;
    g_capture_source = journal_directory(save.path());
    g_capture_target = side.path() / "stale_journal";
    fs::remove_all(g_capture_target);
    {
        HookGuard guard(&CaptureJournalHook);
        ASSERT_TRUE(CommitOnce(save.path(), 2, {{"world_info.json", "v1"}}));
    }
    ASSERT_TRUE(fs::exists(g_capture_target / "txn.json")) << "stale journal was not captured";
    ASSERT_FALSE(journal_pending(save.path()));
    const TreeMap post_commit = Tree(save.path());

    CopyTree(g_capture_target, journal_directory(save.path()));
    ASSERT_TRUE(journal_pending(save.path()));

    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::DiscardedStale);
    EXPECT_EQ(Tree(save.path()), post_commit);
    EXPECT_FALSE(journal_pending(save.path()));
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::NothingToDo);
}

TEST(SaveTransaction, RecoveryOnCleanDirectoryIsNothingToDo) {
    TempSave save;
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::NothingToDo);
    EXPECT_TRUE(errors.empty());
}

TEST(SaveTransaction, RecoveryRejectsUnparsableJournal) {
    TempSave save;
    Put(journal_directory(save.path()) / "txn.json", "garbage");
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_FALSE(errors.empty());
}

TEST(SaveTransaction, RecoveryDiscardsJournalWithoutTxnFile) {
    TempSave save;
    fs::create_directories(journal_directory(save.path()));
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::DiscardedStale);
    EXPECT_FALSE(fs::exists(journal_directory(save.path())));
}

TEST(SaveTransaction, RecoverySweepsStagingTemporariesOfAnIndexedDirectoryWithoutJournal) {
    TempSave dir;
    ASSERT_TRUE(CommitOnce(dir.path(), 1, {{"world_info.json", "w"}}));
    Put(dir.path() / "chunks/region/r.0.0.lmr.tmp.123.4", "partial");
    Put(dir.path() / "world_info.json.tmp.5.6", "partial");
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(dir.path(), &errors), RecoveryOutcome::NothingToDo);
    EXPECT_FALSE(fs::exists(dir.path() / "chunks/region/r.0.0.lmr.tmp.123.4"));
    EXPECT_FALSE(fs::exists(dir.path() / "world_info.json.tmp.5.6"));
    EXPECT_EQ(Get(dir.path() / "world_info.json"), "w");
}

TEST(SaveTransaction, RecoverySweepsStagingTemporaries) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "w"}}));
    Put(save.path() / "chunks/region/r.0.0.lmr.tmp.123.4", "partial");
    Put(save.path() / "world_info.json.tmp.5.6", "partial");
    fs::create_directories(journal_directory(save.path()));

    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::DiscardedStale);
    EXPECT_FALSE(fs::exists(save.path() / "chunks/region/r.0.0.lmr.tmp.123.4"));
    EXPECT_FALSE(fs::exists(save.path() / "world_info.json.tmp.5.6"));
    EXPECT_TRUE(fs::exists(save.path() / "world_info.json"));
}

#if !defined(_WIN32)

TEST(SaveTransactionDeathTest, KillAtEveryStepRecoversToBaselineOrFinal) {
    // Fork-style death tests: the child inherits the parent's directories, so the paths computed
    // below (which embed the pid) are valid inside the child.
    ::testing::GTEST_FLAG(death_test_style) = "fast";

    TempSave base;
    ASSERT_TRUE(
        CommitOnce(base.path(), 1, {{"world_info.json", "w0"}, {"aether_state.efs", "e0"}}));
    const TreeMap baseline = Tree(base.path());

    TreeMap final_tree;
    ComputeFinalTree(base.path(), final_tree);
    ASSERT_FALSE(final_tree.empty());
    ASSERT_NE(final_tree, baseline);

    TempSave work;
    int killed = 0;
    bool saw_finish = false;
    for (int n = 1; n <= 200 && !saw_finish; ++n) {
        CopyTree(base.path(), work.path());
        EXPECT_EXIT(ChildKillScenario(work.path(), n), KilledOrFinished, "");
        ASSERT_TRUE(g_last_exit_code == 0 || g_last_exit_code == 7)
            << "N=" << n << " child exit=" << g_last_exit_code;

        const bool finished = g_last_exit_code == 0;
        if (!finished) {
            ++killed;
        }
        std::vector<std::string> errors;
        const RecoveryOutcome first = recover_pending_journal(work.path(), &errors);
        ASSERT_NE(first, RecoveryOutcome::Failed) << "N=" << n << " errors=" << errors.size();
        EXPECT_EQ(recover_pending_journal(work.path(), nullptr), RecoveryOutcome::NothingToDo)
            << "N=" << n;
        const TreeMap tree = Tree(work.path());
        ASSERT_TRUE(tree == baseline || tree == final_tree) << "N=" << n;
        ASSERT_FALSE(journal_pending(work.path())) << "N=" << n;
        if (finished) {
            saw_finish = true;
            EXPECT_EQ(tree, final_tree) << "N=" << n;
        }
    }
    EXPECT_TRUE(saw_finish) << "scenario did not finish within 200 hook points";
    EXPECT_GE(killed, 5);
}

TEST(SaveTransactionDeathTest, RecoveryItselfSurvivesKillAtEachRollbackEntry) {
    ::testing::GTEST_FLAG(death_test_style) = "fast";

    TempSave base;
    ASSERT_TRUE(
        CommitOnce(base.path(), 1, {{"world_info.json", "w0"}, {"aether_state.efs", "e0"}}));
    const TreeMap baseline = Tree(base.path());

    TreeMap final_tree;
    ComputeFinalTree(base.path(), final_tree);
    ASSERT_FALSE(final_tree.empty());

    // Probe the total hook count of the scenario, and the first journal-published hook.
    TempSave probe;
    CopyTree(base.path(), probe.path());
    g_calls = 0;
    g_published_at = 0;
    {
        HookGuard guard(&RecordHook);
        ASSERT_TRUE(RunScenario(probe.path()));
    }
    ASSERT_GT(g_published_at, 0);
    ASSERT_GT(g_calls, g_published_at);
    // Midway between journal publication and the end of the transaction: the journal still exists.
    const int kill_at = (g_published_at + g_calls) / 2;

    TempSave dead;
    CopyTree(base.path(), dead.path());
    EXPECT_EXIT(ChildKillScenario(dead.path(), kill_at), KilledOrFinished, "");
    ASSERT_EQ(g_last_exit_code, 7) << "transaction was not killed at hook " << kill_at;
    ASSERT_TRUE(journal_pending(dead.path())) << "kill_at=" << kill_at;

    bool saw_finish = false;
    int recovery_kills = 0;
    for (int m = 1; m <= 20 && !saw_finish; ++m) {
        TempSave rec;
        CopyTree(dead.path(), rec.path());
        EXPECT_EXIT(ChildKilledRecovery(rec.path(), m), KilledOrFinished, "");
        ASSERT_TRUE(g_last_exit_code == 0 || g_last_exit_code == 7)
            << "M=" << m << " child exit=" << g_last_exit_code;
        const bool finished = g_last_exit_code == 0;
        if (!finished) {
            ++recovery_kills;
        }

        std::vector<std::string> errors;
        ASSERT_NE(recover_pending_journal(rec.path(), &errors), RecoveryOutcome::Failed)
            << "M=" << m << " errors=" << errors.size();
        const TreeMap tree = Tree(rec.path());
        ASSERT_TRUE(tree == baseline || tree == final_tree) << "M=" << m;
        ASSERT_FALSE(journal_pending(rec.path())) << "M=" << m;
        EXPECT_EQ(recover_pending_journal(rec.path(), nullptr), RecoveryOutcome::NothingToDo)
            << "M=" << m;
        saw_finish = finished;
    }
    EXPECT_TRUE(saw_finish) << "recovery did not finish within 20 hook points";
    EXPECT_GE(recovery_kills, 1);
}

#endif

} // namespace Luminumbra::Persistence
