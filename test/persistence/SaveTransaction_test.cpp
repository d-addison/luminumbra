#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "luminumbra_common/ai/CreaturePersistence.h"
#include "luminumbra_common/core/BoundedFileReader.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/fields/FieldPageState.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/GroundObjectStore.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "luminumbra_common/world/WorldgenFileReader.h"

#include <entt/entity/fwd.hpp>
#include <entt/entt.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json_fwd.hpp>

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/DiseaseComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/components/SoilComponents.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <optional>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace Luminumbra::Persistence {

TEST(SaveTransaction, IndexedEnergyValidationRejectsCanonicalFingerprintOfMalformedContents) {
    const auto remaining = std::to_string(luminumbra::fields::kEnergyCadenceTicks);
    const std::string header = "EFS1 1 " + remaining + "\n";
    std::vector<std::string> invalid{
        "not an EFS record",
        "EFS1 2147483647 " + remaining + "\n",
        header + "P 0 0 0:1 0:2\n",
        header + "P 0 0 0:1\nP 0 0 0:2\n",
        header + "P 0 0 256:1\n",
        header + "P 0 0 0:1junk\n",
    };
    std::string oversized = "EFS1 64 " + remaining + "\n";
    for (int page = 0; page <= 2048; ++page)
        oversized += "P " + std::to_string(page) + " 0\n";
    invalid.push_back(std::move(oversized));
    for (const auto& bytes : invalid) {
        SCOPED_TRACE(bytes.substr(0, 128));
        const auto dir =
            fs::temp_directory_path() /
            ("lum_bad_energy_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        {
            // Seal using raw fingerprints to model a correctly fingerprinted malformed
            // external member. The reader must validate structure as well as its hash.
            SaveTransaction transaction(
                dir,
                1,
                {},
                +[](const fs::path& root,
                    std::string_view relative,
                    MemberDigest& out,
                    std::string& error) {
                    bool exists = false;
                    std::vector<std::string> diagnostics;
                    if (!hash_file(root / std::string(relative),
                                   out.bytes,
                                   out.fnv64,
                                   exists,
                                   &diagnostics,
                                   save_member_byte_limit(relative))) {
                        error = diagnostics.empty() ? "read failed" : diagnostics.front();
                        return false;
                    }
                    out.present = exists;
                    return true;
                });
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(WorldSaveService::write_member_bytes(dir, "aether_state.efs", bytes));
            ASSERT_TRUE(transaction.commit(nullptr));
        }
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService::validate_save(dir, &errors));
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find("energy field"), std::string::npos);
        std::error_code remove_error;
        fs::remove_all(dir, remove_error);
    }
}

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
    if (!hash_file(save_dir / std::string(relative_path),
                   out.bytes,
                   out.fnv64,
                   exists,
                   nullptr,
                   save_member_byte_limit(relative_path))) {
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
    keys.reserve(writes.size());
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

struct JournalCaptureState {
    fs::path source;
    fs::path target;
};

JournalCaptureState& JournalCapture() {
    static JournalCaptureState state;
    return state;
}

void CaptureJournalHook(const char* step, const fs::path& /*path*/) {
    if (std::string_view(step) == "commit-point" && fs::exists(JournalCapture().source)) {
        CopyTree(JournalCapture().source, JournalCapture().target);
    }
}

#if !defined(_WIN32)
// Death-test state. Death tests run in forked children (gtest "fast" style), so the parent-side
// paths are inherited unchanged by the child.
struct DeathTestState {
    int kill_at = 0;
    int calls = 0;
    int published_at = 0;
    int last_exit_code = -1;
};

DeathTestState& DeathState() {
    static DeathTestState state;
    return state;
}

void KillHook(const char* /*step*/, const fs::path& /*path*/) {
    if (++DeathState().calls == DeathState().kill_at) {
        std::_Exit(7);
    }
}

void RecordHook(const char* step, const fs::path& /*path*/) {
    ++DeathState().calls;
    if (DeathState().published_at == 0 && std::string_view(step) == "journal-published") {
        DeathState().published_at = DeathState().calls;
    }
}

// Runs in the parent after waitpid. Records the child's exit code for the caller.
bool KilledOrFinished(int status) {
    if (!WIFEXITED(status)) {
        return false;
    }
    DeathState().last_exit_code = WEXITSTATUS(status);
    return DeathState().last_exit_code == 7 || DeathState().last_exit_code == 0;
}

bool RunScenario(const fs::path& dir) {
    return CommitOnce(dir,
                      5,
                      {{"world_info.json", "w1"},
                       {"aether_state.efs", "e1"},
                       {"chunks/region/plant-entities.json", "p1"}});
}

void ChildKillScenario(const fs::path& dir, int kill_at) {
    DeathState().calls = 0;
    DeathState().kill_at = kill_at;
    set_snapshot_step_hook_for_testing(&KillHook);
    const bool ok = RunScenario(dir);
    std::_Exit(ok ? 0 : 9);
}

void ChildKilledRecovery(const fs::path& dir, int kill_at) {
    DeathState().calls = 0;
    DeathState().kill_at = kill_at;
    set_snapshot_step_hook_for_testing(&KillHook);
    (void)recover_pending_journal(dir, nullptr);
    std::_Exit(0);
}

// Computes the expected post-commit tree by running the scenario in-process on a copy.
void ComputeFinalTree(const fs::path& baseline_dir, TreeMap& out) {
    const TempSave scratch;
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
        "chunks/region/creature-entities.json",
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
    const TempSave save;
    SnapshotIndex index;
    index.generation = 99;
    ASSERT_TRUE(read_snapshot_index(save.path(), index, nullptr));
    EXPECT_EQ(index.generation, 0u);
    EXPECT_FALSE(snapshot_index_exists(save.path()));
}

TEST(SaveTransaction, CommitPublishesIndexAndDeletesJournal) {
    const TempSave save;
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
    const TempSave save;
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
    const TempSave save;
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
    const TempSave save;
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
    const TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "v0"}}));

    const TempSave side;
    JournalCapture().source = journal_directory(save.path());
    JournalCapture().target = side.path() / "stale_journal";
    fs::remove_all(JournalCapture().target);
    {
        const HookGuard guard(&CaptureJournalHook);
        ASSERT_TRUE(CommitOnce(save.path(), 2, {{"world_info.json", "v1"}}));
    }
    ASSERT_TRUE(fs::exists(JournalCapture().target / "txn.json"))
        << "stale journal was not captured";
    ASSERT_FALSE(journal_pending(save.path()));
    const TreeMap post_commit = Tree(save.path());

    CopyTree(JournalCapture().target, journal_directory(save.path()));
    ASSERT_TRUE(journal_pending(save.path()));

    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::DiscardedStale);
    EXPECT_EQ(Tree(save.path()), post_commit);
    EXPECT_FALSE(journal_pending(save.path()));
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::NothingToDo);
}

TEST(SaveTransaction, RecoveryOnCleanDirectoryIsNothingToDo) {
    const TempSave save;
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::NothingToDo);
    EXPECT_TRUE(errors.empty());
}

TEST(SaveTransaction, RecoveryRejectsUnparsableJournal) {
    const TempSave save;
    Put(journal_directory(save.path()) / "txn.json", "garbage");
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_FALSE(errors.empty());
}

namespace {

std::string RecoveryJournal(std::string_view entries) {
    return R"({"schema":"luminumbra.journal.v1","txn_id":1,"base_generation":0,"new_generation":1,"entries":[)" +
           std::string(entries) + "]}";
}

std::string CreateRecoveryEntry() {
    return R"({"path":"world_info.json","action":"create","prev_bytes":0,"prev_fnv64":"0000000000000000","undo":""})";
}

} // namespace

TEST(SaveTransaction, RecoveryRefusesOversizedJournalBeforeReadingAnyBytes) {
    const TempSave save;
    const auto path = journal_directory(save.path()) / "txn.json";
    Put(path, "{}");
    // Extend an imported save without constructing its contents in memory.
    fs::resize_file(path, kMaxJournalBytes + 1);
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_EQ(recovery_journal_bytes_read(), 0u);
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("size cap"), std::string::npos);
    EXPECT_TRUE(fs::exists(path));
}

TEST(SaveTransaction, RecoveryRefusesExcessiveEntriesBeforeReadingTheTail) {
    const TempSave save;
    std::string entries;
    const auto item = CreateRecoveryEntry();
    for (std::size_t i = 0; i <= kMaxJournalEntries; ++i) {
        if (i != 0)
            entries += ',';
        entries += item;
    }
    const auto path = journal_directory(save.path()) / "txn.json";
    const auto bytes = RecoveryJournal(entries) + std::string(std::size_t{1024} * 1024u, ' ');
    ASSERT_LT(bytes.size(), kMaxJournalBytes);
    Put(path, bytes);
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_LT(recovery_journal_bytes_read(), bytes.size() - std::size_t{512} * 1024u);
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("entry count"), std::string::npos);
    EXPECT_TRUE(fs::exists(path));
}

TEST(SaveTransaction, RecoveryBoundsStringsNestingAndUnknownFields) {
    const TempSave save;
    const auto path = journal_directory(save.path()) / "txn.json";
    const std::vector<std::pair<std::string, std::string>> invalid = {
        {R"({"schema":")" + std::string(kMaxJournalStringBytes + 1, 'x') + R"("})", "string"},
        {RecoveryJournal("[]"), "nesting"},
        {R"({"unknown":0})", "unknown"},
        {R"({"schema":"luminumbra.journal.v1","schema":"luminumbra.journal.v1"})", "duplicated"},
        {R"({"schema":null})", "type"},
        {R"({"schema":true})", "type"},
        {R"({"txn_id":-1})", "unsigned"},
        {R"({"txn_id":1.5})", "unsigned"},
        {RecoveryJournal("0"), "unsigned"},
        {RecoveryJournal("{}"), "fields"},
        {RecoveryJournal(R"({"unknown":0})"), "unknown"},
        {R"({"schema":"unsupported"})", "schema"},
        {RecoveryJournal(
             R"({"path":"../escape","action":"create","prev_bytes":0,"prev_fnv64":"0000000000000000","undo":""})"),
         "path"},
        {RecoveryJournal(
             R"({"path":"world_info.json","action":"remove","prev_bytes":0,"prev_fnv64":"0000000000000000","undo":""})"),
         "action"},
        {RecoveryJournal(
             R"({"path":"world_info.json","action":"create","prev_bytes":0,"prev_fnv64":"000000000000000g","undo":""})"),
         "digest"},
        {RecoveryJournal(
             R"({"path":"world_info.json","action":"create","prev_bytes":0,"prev_fnv64":"0000000000000000","undo":"undo/bad"})"),
         "undo name"},
        {"{}", "header"},
        {"[]", "nesting"},
        {RecoveryJournal(
             R"({"path":"world_info.json","action":"replace","prev_bytes":1048577,"prev_fnv64":"0000000000000000","undo":"undo/0"})"),
         "undo size"},
        {RecoveryJournal("{"), "malformed"},
    };
    for (const auto& [bytes, diagnostic] : invalid) {
        SCOPED_TRACE(diagnostic);
        Put(path, bytes);
        std::vector<std::string> errors;
        EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find(diagnostic), std::string::npos);
        EXPECT_TRUE(fs::exists(path));
    }
}

TEST(SaveTransaction, BoundedJournalStillRecoversValidAndEmptyTransactions) {
    const TempSave save;
    Put(save.path() / "world_info.json", "new content");
    Put(journal_directory(save.path()) / "txn.json", RecoveryJournal(CreateRecoveryEntry()));
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
    EXPECT_FALSE(fs::exists(save.path() / "world_info.json"));
    EXPECT_FALSE(journal_pending(save.path()));
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
    Put(journal_directory(save.path()) / "txn.json", RecoveryJournal(""));
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
}

TEST(SaveTransaction, RecoveryRefusesUnreadableJournalAndMissingOrOversizedUndo) {
    const TempSave save;
    const auto path = journal_directory(save.path()) / "txn.json";
    fs::create_directories(path);
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
    fs::remove(path);
    Put(path,
        RecoveryJournal(
            R"({"path":"world_info.json","action":"replace","prev_bytes":2,"prev_fnv64":"0000000000000000","undo":"undo/0"})"));
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
    const auto undo = journal_directory(save.path()) / "undo/0";
    Put(undo, "{}");
    fs::resize_file(undo, kMaxSaveMetadataBytes + 1);
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
    EXPECT_TRUE(fs::exists(path));
    EXPECT_FALSE(fs::exists(save.path() / "world_info.json"));
}

TEST(SaveTransaction, HashingBoundsRegularFilesAndReportsMissingSeparately) {
    const TempSave save;
    const auto path = save.path() / "member";
    std::uint64_t bytes = 99, hash = 99;
    bool exists = true;
    EXPECT_TRUE(hash_file(path, bytes, hash, exists, nullptr, 4));
    EXPECT_FALSE(exists);
    Put(path, "");
    ASSERT_TRUE(hash_file(path, bytes, hash, exists, nullptr, 0));
    EXPECT_TRUE(exists);
    EXPECT_EQ(bytes, 0u);
    EXPECT_EQ(hash, fnv1a64_bytes("", 0));
    Put(path, "abcd");
    ASSERT_TRUE(hash_file(path, bytes, hash, exists, nullptr, 4));
    EXPECT_EQ(bytes, 4u);
    EXPECT_EQ(hash, fnv1a64_bytes("abcd", 4));
    EXPECT_FALSE(hash_file(path, bytes, hash, exists, nullptr, 3));
    EXPECT_EQ(bytes, 0u); // size refusal happens before hashing any content
    EXPECT_FALSE(hash_file(save.path(), bytes, hash, exists, nullptr, 4));
    EXPECT_EQ(bytes, 0u);
}

TEST(SaveTransaction, CreatureMemberBoundAppliesBeforeHashingAndUndoAllocation) {
    const TempSave save;
    constexpr std::string_view member = "chunks/region/creature-entities.json";
    constexpr auto limit = luminumbra::ai::kMaxStampedCreatureBytes;
    EXPECT_EQ(save_member_byte_limit(member), limit);
    const auto path = save.path() / member;
    Put(path, "{}");
    fs::resize_file(path, limit + 1);
    std::uint64_t bytes = 99, hash = 99;
    bool exists = false;
    EXPECT_FALSE(hash_file(path, bytes, hash, exists, nullptr, save_member_byte_limit(member)));
    EXPECT_TRUE(exists);
    EXPECT_EQ(bytes, 0u);
    SaveTransaction transaction(save.path(), 0, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    EXPECT_FALSE(transaction.protect({std::string(member)}, nullptr));
    EXPECT_EQ(fs::file_size(path), limit + 1);
}

TEST(SaveTransaction, RecoveryRefusesGrowingUndoThroughTheOpenedHashHandle) {
    const TempSave save;
    const auto undo = journal_directory(save.path()) / "undo/0";
    Put(undo, "old");
    Put(journal_directory(save.path()) / "txn.json",
        RecoveryJournal(
            R"({"path":"world_info.json","action":"replace","prev_bytes":3,"prev_fnv64":"0000000000000000","undo":"undo/0"})"));
    // Deterministically grow the undo after its handle's initial size check.
    const HookGuard hook(+[](const char* step, const fs::path& path) {
        if (std::string_view(step) == "hash-opened" && path.filename() == "0")
            fs::resize_file(path, kMaxSaveMetadataBytes + 1);
    });
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("grew"), std::string::npos);
    EXPECT_FALSE(fs::exists(save.path() / "world_info.json"));
    EXPECT_TRUE(journal_pending(save.path()));
    // The standalone hash exposes the exact bound even when the file grows.
    Put(undo, "old");
    std::uint64_t bytes = 0, hash = 0;
    bool exists = false;
    EXPECT_FALSE(hash_file(undo, bytes, hash, exists, nullptr, kMaxSaveMetadataBytes));
    EXPECT_EQ(bytes, 3u);
}

TEST(SaveTransaction, HashingRefusesPartialUndoReadsBeforeRestoration) {
    const TempSave save;
    const auto undo = journal_directory(save.path()) / "undo/0";
    Put(undo, "old");
    const HookGuard hook(+[](const char* step, const fs::path& path) {
        if (std::string_view(step) == "hash-opened")
            fs::resize_file(path, 1);
    });
    std::uint64_t bytes = 0, hash = 0;
    bool exists = false;
    EXPECT_FALSE(hash_file(undo, bytes, hash, exists, nullptr, 3));
    EXPECT_EQ(bytes, 0u);
}

TEST(SaveTransaction, RecoveryAndHashingRefuseDeviceSymlinksAndNonRegularFiles) {
    const TempSave save;
    const auto undo = journal_directory(save.path()) / "undo/0";
    fs::create_directories(undo);
    std::uint64_t bytes = 0, hash = 0;
    bool exists = false;
    std::string content;
    EXPECT_FALSE(hash_file(undo, bytes, hash, exists, nullptr, 3));
    EXPECT_EQ(bytes, 0u);
    EXPECT_FALSE(read_bounded_save_file(undo, content, 3, nullptr));
    fs::remove(undo);
#ifndef _WIN32
    fs::create_symlink("/dev/zero", undo);
    Put(journal_directory(save.path()) / "txn.json",
        RecoveryJournal(
            R"({"path":"world_info.json","action":"replace","prev_bytes":3,"prev_fnv64":"0000000000000000","undo":"undo/0"})"));
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
    EXPECT_TRUE(journal_pending(save.path()));
    EXPECT_FALSE(hash_file(undo, bytes, hash, exists, nullptr, 3));
    EXPECT_EQ(bytes, 0u);
    EXPECT_FALSE(read_bounded_save_file(undo, content, 3, nullptr));
    EXPECT_FALSE(hash_file("/dev/zero", bytes, hash, exists, nullptr, 3));
    fs::remove(undo);
    ASSERT_EQ(::mkfifo(undo.c_str(), 0600), 0);
    EXPECT_FALSE(hash_file(undo, bytes, hash, exists, nullptr, 3));
    EXPECT_FALSE(read_bounded_save_file(undo, content, 3, nullptr));
    fs::remove(undo);
    fs::create_symlink(save.path() / "absent", undo);
    EXPECT_FALSE(hash_file(undo, bytes, hash, exists, nullptr, 3));
#else
    EXPECT_FALSE(read_bounded_save_file(L"\\\\.\\NUL", content, 3, nullptr));
#endif
}

TEST(SaveTransaction, LegacyEnergyDecoderBoundsChannelsAndAllPagesBeforeAllocation) {
    luminumbra::fields::EnergyFieldState state;
    ASSERT_TRUE(state.DeserializeRecord("EFS1 1 1\nP 0 0 0:7\n", 0));
    const auto before = state.CanonicalBytes();
    std::string too_many = "EFS1 64 1\n";
    for (int page = 0; page <= 2048; ++page)
        too_many += "P " + std::to_string(page) + " 0\n";
    for (const auto& record : std::vector<std::string>{"EFS1 2147483647 1\nP 0 0\n",
                                                       "EFS1 65 1\nP 0 0\n",
                                                       "EFS1 0 1\n",
                                                       "EFS1 1 0\n",
                                                       "EFS1 1 1 extra\n",
                                                       "EFS1 1 1\nP 0\n",
                                                       "EFS1 1 1\nP 0 0 256:1\n",
                                                       "EFS1 1 1\nP 0 0 0:1junk\n",
                                                       "EFS1 1 1\nP 0 0 0:1 0:2\n",
                                                       "EFS1 1 1\nP 0 0\nP 0 0\n",
                                                       "EFS1 1 1\nP 2147483647 0\n",
                                                       too_many}) {
        SCOPED_TRACE(record.substr(0, 80));
        EXPECT_NO_THROW(EXPECT_FALSE(state.DeserializeRecord(record, 0)));
        EXPECT_EQ(state.CanonicalBytes(), before);
    }
    EXPECT_FALSE(state.DeserializeRecord("EFS1 1 1\n", std::numeric_limits<std::uint64_t>::max()));
    EXPECT_TRUE(state.DeserializeRecord("EFS1 64 1\nP 0 0 16383:9\n", 0));
    EXPECT_EQ(state.channels(), 64);
    luminumbra::fields::EnergyFieldState unsupported(65);
    EXPECT_THROW(static_cast<void>(unsupported.SerializeRecord(0)), std::length_error);
}

TEST(SaveTransaction, LegacyEnergyAboveSixteenMiBRoundTripsAndRefusalsAreTyped) {
    using State = luminumbra::fields::EnergyFieldState;
    using Status = WorldSaveService::LegacyEnergyReadStatus;
    const TempSave save;
    const auto path = save.path() / "aether_state.efs";
    State original;
    std::string record = "EFS1 1 8\n";
    for (int page = 0; page < 8192; ++page) {
        record += "P " + std::to_string(page) + " 0";
        for (int index = 0; index < 256; ++index)
            record += " " + std::to_string(index) + ":65535";
        record += '\n';
    }
    ASSERT_GT(record.size(), 16u * 1024u * 1024u);
    ASSERT_TRUE(original.DeserializeRecord(record, 0));
    EXPECT_EQ(original.SerializeRecord(0), record); // legitimate legacy writer output
    Put(path, record);
    State loaded;
    ASSERT_EQ(WorldSaveService::load_legacy_energy(loaded, save.path(), 0), Status::Loaded);
    EXPECT_EQ(loaded.page_count(), 8192u);
    EXPECT_EQ(loaded.total_raw(), original.total_raw());
    const auto before = loaded.CanonicalBytes();
    fs::resize_file(path, kMaxSaveEnergyBytes + 1);
    EXPECT_EQ(WorldSaveService::load_legacy_energy(loaded, save.path(), 0), Status::Unreadable);
    EXPECT_EQ(loaded.CanonicalBytes(), before);
    Put(path, "EFS1 2147483647 1\nP 0 0\n");
    EXPECT_EQ(WorldSaveService::load_legacy_energy(loaded, save.path(), 0), Status::Invalid);
    EXPECT_EQ(loaded.CanonicalBytes(), before);
    fs::remove(path);
    EXPECT_EQ(WorldSaveService::load_legacy_energy(loaded, save.path(), 0), Status::Missing);
    EXPECT_EQ(loaded.page_count(), 0u);
    fs::create_directory(path);
    EXPECT_EQ(WorldSaveService::load_legacy_energy(loaded, save.path(), 0), Status::Unreadable);
}

TEST(SaveTransaction, FiftyThousandPlantSnapshotAboveSeventyTwoMiBRoundTrips) {
    namespace C = Components;
    entt::registry registry;
    const auto plant = registry.create();
    registry.emplace<C::PlantTag>(plant);
    registry.emplace<C::TransformComponent>(plant);
    registry.emplace<C::PlantGrowthComponent>(plant);
    registry.emplace<C::PlantGenomeComponent>(plant).genes.fill(.125f);
    registry.emplace<C::SoilFeederComponent>(plant);
    registry.emplace<C::PlantHealthComponent>(plant);
    auto snapshot = luminumbra::foliage::BuildPlantEntitySnapshot(registry);
    const auto prototype = snapshot.entities.front();
    snapshot.entities.resize(50'000, prototype);
    for (std::size_t index = 0; index < snapshot.entities.size(); ++index)
        snapshot.entities[index].entity_id = index + 1;
    const TempSave save;
    ASSERT_TRUE(WorldSaveService::save_plant_entities(snapshot, save.path()));
    ASSERT_GE(fs::file_size(WorldSaveService::plant_entities_path(save.path())),
              72u * 1024u * 1024u);
    snapshot.entities.clear();
    snapshot.entities.shrink_to_fit();
    Ecs::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(WorldSaveService::load_plant_entities(loaded, save.path()));
    ASSERT_EQ(loaded.entities.size(), 50'000u);
    for (std::size_t index = 0; index < loaded.entities.size(); ++index) {
        const auto& entity = loaded.entities[index];
        ASSERT_EQ(entity.entity_id, index + 1);
        ASSERT_EQ(entity.name, prototype.name);
        ASSERT_EQ(entity.components.size(), prototype.components.size());
        ASSERT_TRUE(std::equal(
            entity.components.begin(),
            entity.components.end(),
            prototype.components.begin(),
            [](const auto& a, const auto& b) { return a.type == b.type && a.data == b.data; }));
    }
}

TEST(SaveTransaction, MemberWriterUsesReaderBoundBeforePublication) {
    const TempSave save;
    const std::string relative = "chunks/region/r.0.0.lmr";
    Put(save.path() / relative, "original");
    std::vector<std::string> errors;
    {
        // The fixed 256 MiB container bound also applies before generic publication.
        const std::string oversized(kMaxSaveMemberBytes + 1, 'x');
        EXPECT_FALSE(
            WorldSaveService::write_member_bytes(save.path(), relative, oversized, &errors));
    }
    EXPECT_EQ(Get(save.path() / relative), "original");
    EXPECT_FALSE(journal_pending(save.path()));
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("size cap"), std::string::npos);

    // Exercise the container writer itself through the raw-record API. Nine
    // incompressible 32 MiB records each fit the reader's 64 MiB record bound,
    // while their combined container exceeds 256 MiB before writer allocation.
    std::string payload(std::size_t{32} * 1024u * 1024u, '\0');
    std::uint32_t random = 0x9e3779b9u;
    for (char& byte : payload) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<char>(random & 0xffu);
    }
    std::vector<WorldSaveService::ContainerRecord> records;
    records.reserve(9);
    for (std::uint64_t id = 1; id <= 9; ++id)
        records.push_back({.id = id, .lod_level = 1, .flags = 0, .payload = payload});
    payload.clear();
    payload.shrink_to_fit();
    const auto batch_path = WorldSaveService::region_file_path(save.path(), 1, 0);
    errors.clear();
    EXPECT_FALSE(WorldSaveService::upsert_container_records(batch_path, records, &errors));
    EXPECT_FALSE(fs::exists(batch_path));
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("size cap"), std::string::npos);
}

TEST(SaveTransaction, PlantWriterRefusesPopulationAndRecordOverflowBeforeReplacing) {
    const TempSave save;
    const auto path = WorldSaveService::plant_entities_path(save.path());
    Put(path, "original");
    Ecs::EntityRegistrySnapshot snapshot;
    snapshot.entities.resize(kMaxSavePlants + 1);
    EXPECT_FALSE(WorldSaveService::save_plant_entities(snapshot, save.path()));
    EXPECT_EQ(Get(path), "original");
    snapshot.entities.resize(1);
    snapshot.entities.front().name.assign(kMaxSavePlantRecordBytes + 1, 'x');
    EXPECT_FALSE(WorldSaveService::save_plant_entities(snapshot, save.path()));
    EXPECT_EQ(Get(path), "original");
    snapshot.entities.front().name = "plant";
    nlohmann::json nested = 0;
    for (int depth = 0; depth < 20; ++depth)
        nested = nlohmann::json::array({std::move(nested)});
    snapshot.entities.front().components.push_back({"unknown", std::move(nested)});
    EXPECT_FALSE(WorldSaveService::save_plant_entities(snapshot, save.path()));
    EXPECT_EQ(Get(path), "original");
    snapshot.entities.front().components.front().data = nlohmann::json::binary({1, 2});
    EXPECT_FALSE(WorldSaveService::save_plant_entities(snapshot, save.path()));
    EXPECT_EQ(Get(path), "original");
}

TEST(SaveTransaction, BoundedFileReadAcceptsExactLimitAndRejectsOversizeUnreadableAndCancelled) {
    const TempSave save;
    const auto path = save.path() / "member";
    std::string bytes = "old";
    Put(path, "");
    EXPECT_TRUE(read_bounded_save_file(path, bytes, 0, nullptr));
    EXPECT_TRUE(bytes.empty());
    Put(path, "abcd");
    EXPECT_TRUE(read_bounded_save_file(path, bytes, 4, nullptr));
    EXPECT_EQ(bytes, "abcd");
    EXPECT_FALSE(read_bounded_save_file(path, bytes, 3, nullptr));
    EXPECT_TRUE(bytes.empty());
    EXPECT_FALSE(read_bounded_save_file(save.path() / "missing", bytes, 4, nullptr));
    EXPECT_FALSE(read_bounded_save_file(save.path(), bytes, 4, nullptr));
    const std::stop_source cancelled;
    cancelled.request_stop();
    EXPECT_FALSE(read_bounded_save_file(path, bytes, 4, nullptr, cancelled.get_token()));
    EXPECT_TRUE(bytes.empty());
}

TEST(SaveTransaction, BoundedOpenedFileRefusesGrowthAndPartialReads) {
    const TempSave save;
    const auto path = save.path() / "member";
    Put(path, "abcd");
    {
        Filesystem::BoundedFileInput input(path, 4);
        EXPECT_EQ(input.bytes_read(), 0u);
        fs::resize_file(path, 2);
        std::array<char, 4> bytes{};
        EXPECT_THROW(input.stream().read(bytes.data(), bytes.size()), std::runtime_error);
        EXPECT_LE(input.bytes_read(), 2u);
    }
    Put(path, "abcd");
    {
        Filesystem::BoundedFileInput input(path, 4);
        fs::resize_file(path, 5);
        std::array<char, 4> bytes{};
        input.stream().read(bytes.data(), bytes.size());
        EXPECT_EQ(input.bytes_read(), 4u);
        EXPECT_THROW(input.stream().peek(), std::runtime_error);
    }
    Put(path, "abcd");
    {
        const std::stop_source cancellation;
        Filesystem::BoundedFileInput input(path, 4, cancellation.get_token());
        cancellation.request_stop();
        EXPECT_THROW(input.stream().peek(), std::runtime_error);
        EXPECT_EQ(input.bytes_read(), 0u);
    }
}

TEST(SaveTransaction, BoundedRecoveryRestoresValidUndoAndRefusesTruncatedUndo) {
    const TempSave save;
    const auto undo = journal_directory(save.path()) / "undo/0";
    std::ostringstream digest;
    digest << std::hex << std::setfill('0') << std::setw(16) << fnv1a64_bytes("old", 3);
    const auto journal = RecoveryJournal(
        R"({"path":"world_info.json","action":"replace","prev_bytes":3,"prev_fnv64":")" +
        digest.str() + R"(","undo":"undo/0"})");
    Put(save.path() / "world_info.json", "new");
    Put(undo, "ol");
    Put(journal_directory(save.path()) / "txn.json", journal);
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
    EXPECT_EQ(Get(save.path() / "world_info.json"), "new");
    Put(undo, "old");
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(Get(save.path() / "world_info.json"), "old");
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, PresetReadersRefuseOversizeUnreadableAndPartialFiles) {
    const TempSave save;
    const auto path = save.path() / "preset.json";
    std::string bytes;
    Put(path, "{}");
    EXPECT_TRUE(World::ReadWorldgenFile(path, bytes));
    EXPECT_EQ(bytes, "{}");
    fs::resize_file(path, World::kMaxWorldgenFileBytes + 1);
    EXPECT_FALSE(World::ReadWorldgenFile(path, bytes));
    EXPECT_TRUE(bytes.empty());
    EXPECT_FALSE(world::LoadTerrainPreset(path).ok);
    std::string output = "unchanged";
    std::string error;
    EXPECT_FALSE(world::LoadTerrainPresetOverride(path, save.path(), output, error));
    EXPECT_EQ(output, "unchanged");
    EXPECT_FALSE(error.empty());
    Put(path, "{");
    EXPECT_FALSE(world::LoadTerrainPreset(path).ok);
    EXPECT_FALSE(world::LoadTerrainPresetOverride(path, save.path(), output, error));
    EXPECT_EQ(output, "unchanged");
    EXPECT_FALSE(World::ReadWorldgenFile(save.path(), bytes));
    EXPECT_FALSE(World::ReadWorldgenFile(save.path() / "missing", bytes));
    EXPECT_FALSE(
        World::ReadWorldgenFileWithStatus(path, bytes, [](const fs::path&, std::error_code& ec) {
            ec = std::make_error_code(std::errc::permission_denied);
            return fs::file_status{};
        }));
}

TEST(SaveTransaction, ProtectionRefusesExcessiveEntriesBeforeWritingUndoOrJournal) {
    const TempSave save;
    SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    std::vector<std::string> paths;
    paths.reserve(kMaxJournalEntries);
    for (std::size_t i = 0; i < kMaxJournalEntries; ++i)
        paths.push_back("chunks/region/r." + std::to_string(i) + ".0.lmr");
    // The index is an additional entry. Refuse the batch before filesystem work on members.
    EXPECT_FALSE(transaction.protect(paths, nullptr));
    EXPECT_FALSE(fs::exists(journal_directory(save.path()) / "txn.json"));
    EXPECT_TRUE(fs::is_empty(journal_directory(save.path()) / "undo"));
    EXPECT_TRUE(transaction.protect({"world_info.json", "world_info.json"}, nullptr));
}

TEST(SaveTransaction, ProtectionRefusesOversizedMembersAndSnapshotIndex) {
    for (const std::string relative : {"world_info.json", "chunks/region/snapshot.json"}) {
        const TempSave save;
        const auto path = save.path() / relative;
        Put(path, "{}");
        fs::resize_file(path,
                        relative == "world_info.json" ? kMaxSaveMetadataBytes + 1
                                                      : kMaxSnapshotIndexBytes + 1);
        SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
        if (relative == kSnapshotIndexRelativePath) {
            SnapshotIndex index;
            EXPECT_FALSE(read_snapshot_index(save.path(), index, nullptr));
            EXPECT_FALSE(transaction.begin(nullptr));
        } else {
            ASSERT_TRUE(transaction.begin(nullptr));
            EXPECT_FALSE(transaction.protect({relative}, nullptr));
            EXPECT_FALSE(fs::exists(journal_directory(save.path()) / "undo/0"));
        }
    }
}

TEST(SaveTransaction, OpenReadersRefuseOversizedPersistenceFiles) {
    const TempSave save;
    for (const auto& [relative, limit] : std::vector<std::pair<std::string, std::size_t>>{
             {"world_info.json", kMaxSaveMetadataBytes},
             {"chunks/region/world-manifest.json", kMaxSaveManifestBytes},
             {"chunks/region/r.0.0.lmr", kMaxSaveMemberBytes},
             {"chunks/region/plant-entities.json", kMaxSaveEntityBytes},
             {"aether_state.efs", kMaxSaveEnergyBytes},
             {"chunks/region/active-regions.arl", world::ActiveRegionLedger::kMaxFileBytes},
             {"chunks/region/ground-objects.json", world::GroundObjectStore::kMaxFileBytes},
             {"chunks/region/field-pages.fpg", world::FieldPageState::kMaxFileBytes}}) {
        SCOPED_TRACE(relative);
        const auto path = save.path() / relative;
        Put(path, "{}");
        fs::resize_file(path, limit + 1);
        MemberDigest digest;
        std::string error;
        EXPECT_FALSE(WorldSaveService::member_digest(save.path(), relative, digest, error));
        EXPECT_FALSE(error.empty());
        fs::remove(path);
    }
    const auto catalog_save = save.path() / "worlds/saves/imported";
    Put(catalog_save / "world_info.json", "{}");
    fs::resize_file(catalog_save / "world_info.json", kMaxSaveMetadataBytes + 1);
    EXPECT_FALSE(InspectSavedWorld(save.path(), "imported").error.empty());
    Put(journal_directory(catalog_save) / "txn.json", "{}");
    const auto pending = InspectSavedWorld(save.path(), "imported");
    EXPECT_TRUE(pending.pending_recovery);
    EXPECT_TRUE(pending.error.empty());
    EXPECT_EQ(pending.metadata.name, "imported");
}

TEST(SaveTransaction, RecoveryDiscardsJournalWithoutTxnFile) {
    const TempSave save;
    fs::create_directories(journal_directory(save.path()));
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::DiscardedStale);
    EXPECT_FALSE(fs::exists(journal_directory(save.path())));
}

TEST(SaveTransaction, RecoverySweepsStagingTemporariesOfAnIndexedDirectoryWithoutJournal) {
    const TempSave dir;
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
    const TempSave save;
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

    const TempSave base;
    ASSERT_TRUE(
        CommitOnce(base.path(), 1, {{"world_info.json", "w0"}, {"aether_state.efs", "e0"}}));
    const TreeMap baseline = Tree(base.path());

    TreeMap final_tree;
    ComputeFinalTree(base.path(), final_tree);
    ASSERT_FALSE(final_tree.empty());
    ASSERT_NE(final_tree, baseline);

    const TempSave work;
    int killed = 0;
    bool saw_finish = false;
    for (int n = 1; n <= 200 && !saw_finish; ++n) {
        CopyTree(base.path(), work.path());
        EXPECT_EXIT(ChildKillScenario(work.path(), n), KilledOrFinished, "");
        ASSERT_TRUE(DeathState().last_exit_code == 0 || DeathState().last_exit_code == 7)
            << "N=" << n << " child exit=" << DeathState().last_exit_code;

        const bool finished = DeathState().last_exit_code == 0;
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

    const TempSave base;
    ASSERT_TRUE(
        CommitOnce(base.path(), 1, {{"world_info.json", "w0"}, {"aether_state.efs", "e0"}}));
    const TreeMap baseline = Tree(base.path());

    TreeMap final_tree;
    ComputeFinalTree(base.path(), final_tree);
    ASSERT_FALSE(final_tree.empty());

    // Probe the total hook count of the scenario, and the first journal-published hook.
    const TempSave probe;
    CopyTree(base.path(), probe.path());
    DeathState().calls = 0;
    DeathState().published_at = 0;
    {
        const HookGuard guard(&RecordHook);
        ASSERT_TRUE(RunScenario(probe.path()));
    }
    ASSERT_GT(DeathState().published_at, 0);
    ASSERT_GT(DeathState().calls, DeathState().published_at);
    // Midway between journal publication and the end of the transaction: the journal still exists.
    const int kill_at = (DeathState().published_at + DeathState().calls) / 2;

    const TempSave dead;
    CopyTree(base.path(), dead.path());
    EXPECT_EXIT(ChildKillScenario(dead.path(), kill_at), KilledOrFinished, "");
    ASSERT_EQ(DeathState().last_exit_code, 7) << "transaction was not killed at hook " << kill_at;
    ASSERT_TRUE(journal_pending(dead.path())) << "kill_at=" << kill_at;

    bool saw_finish = false;
    int recovery_kills = 0;
    for (int m = 1; m <= 20 && !saw_finish; ++m) {
        const TempSave rec;
        CopyTree(dead.path(), rec.path());
        EXPECT_EXIT(ChildKilledRecovery(rec.path(), m), KilledOrFinished, "");
        ASSERT_TRUE(DeathState().last_exit_code == 0 || DeathState().last_exit_code == 7)
            << "M=" << m << " child exit=" << DeathState().last_exit_code;
        const bool finished = DeathState().last_exit_code == 0;
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

#if defined(_WIN32)
namespace {
int g_windows_cut = 0;
int g_windows_calls = 0;
void WindowsKillHook(const char*, const fs::path&) {
    if (++g_windows_calls == g_windows_cut)
        std::_Exit(7);
}

std::wstring WindowsTestEnv(const wchar_t* name) {
    const DWORD count = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (count == 0)
        return {};
    std::wstring value(count, L'\0');
    const DWORD copied = ::GetEnvironmentVariableW(name, value.data(), count);
    value.resize(copied);
    return value;
}

bool WindowsScenario(const fs::path& dir) {
    SaveTransaction txn(dir, 5, {}, &RawDigest);
    if (!txn.begin(nullptr) || !txn.protect({"world_info.json",
                                             "aether_state.efs",
                                             "chunks/region/plant-entities.json",
                                             "chunks/region/active-regions.arl"},
                                            nullptr))
        return false;
    return WorldSaveService::write_member_bytes(dir, "world_info.json", "w1") &&
           WorldSaveService::write_member_bytes(dir, "aether_state.efs", "e1") &&
           WorldSaveService::write_member_bytes(dir, "chunks/region/plant-entities.json", "p1") &&
           WorldSaveService::remove_member(dir, "chunks/region/active-regions.arl") &&
           txn.commit(nullptr);
}

std::optional<DWORD> WindowsTxnChild(const fs::path& dir, int cut, bool recovery) {
    std::wstring executable(32768, L'\0');
    const DWORD count =
        ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (count == 0 || count == executable.size())
        return std::nullopt;
    executable.resize(count);
    const auto previous_dir = WindowsTestEnv(L"LUM_TXN_TEST_CHILD_DIR");
    const auto previous_cut = WindowsTestEnv(L"LUM_TXN_TEST_CHILD_CUT");
    const auto previous_mode = WindowsTestEnv(L"LUM_TXN_TEST_CHILD_RECOVER");
    const auto cut_text = std::to_wstring(cut);
    const bool configured =
        ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_DIR", dir.c_str()) &&
        ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_CUT", cut_text.c_str()) &&
        ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_RECOVER", recovery ? L"1" : L"0");
    std::wstring command = L"\"" + executable +
                           L"\" --gtest_filter=SaveTransaction.DISABLED_WindowsInterruptionChild "
                           L"--gtest_also_run_disabled_tests --gtest_color=no";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool started = configured && ::CreateProcessW(executable.c_str(),
                                                        command.data(),
                                                        nullptr,
                                                        nullptr,
                                                        FALSE,
                                                        CREATE_NO_WINDOW,
                                                        nullptr,
                                                        nullptr,
                                                        &startup,
                                                        &process) != FALSE;
    ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_DIR",
                              previous_dir.empty() ? nullptr : previous_dir.c_str());
    ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_CUT",
                              previous_cut.empty() ? nullptr : previous_cut.c_str());
    ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_RECOVER",
                              previous_mode.empty() ? nullptr : previous_mode.c_str());
    if (!started)
        return std::nullopt;
    const DWORD waited = ::WaitForSingleObject(process.hProcess, 30000);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(process.hProcess, 9);
        ::WaitForSingleObject(process.hProcess, 30000);
    }
    DWORD code = 9;
    const bool got_exit = ::GetExitCodeProcess(process.hProcess, &code) != FALSE;
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return got_exit && waited == WAIT_OBJECT_0 ? std::optional<DWORD>(code) : std::nullopt;
}
} // namespace

// Direct GoogleTest runs must leave this child to the interruption matrix below.
// CTest also labels it manual via SaveTransactionCTest.cmake after discovery.
TEST(SaveTransaction, DISABLED_WindowsInterruptionChild) {
    const auto dir = WindowsTestEnv(L"LUM_TXN_TEST_CHILD_DIR");
    if (dir.empty())
        GTEST_SKIP() << "invoked only by the Windows interruption matrix";
    g_windows_cut = std::stoi(WindowsTestEnv(L"LUM_TXN_TEST_CHILD_CUT"));
    g_windows_calls = 0;
    set_snapshot_step_hook_for_testing(&WindowsKillHook);
    if (WindowsTestEnv(L"LUM_TXN_TEST_CHILD_RECOVER") == L"1")
        std::_Exit(recover_pending_journal(fs::path(dir), nullptr) == RecoveryOutcome::Failed ? 9
                                                                                              : 0);
    std::_Exit(WindowsScenario(fs::path(dir)) ? 0 : 9);
}

TEST(SaveTransaction, WindowsKillAtEveryPublicationAndRecoveryStepIsIdempotent) {
    const TempSave base, final, work, interrupted;
    ASSERT_TRUE(CommitOnce(base.path(),
                           1,
                           {{"world_info.json", "w0"},
                            {"aether_state.efs", "e0"},
                            {"chunks/region/active-regions.arl", "a0"}}));
    const auto baseline = Tree(base.path());
    CopyTree(base.path(), final.path());
    ASSERT_TRUE(WindowsScenario(final.path()));
    const auto finished_tree = Tree(final.path());
    bool complete = false;
    bool have_interrupted = false;
    for (int cut = 1; cut <= 200 && !complete; ++cut) {
        SCOPED_TRACE(cut);
        CopyTree(base.path(), work.path());
        const auto exit_code = WindowsTxnChild(work.path(), cut, false);
        ASSERT_TRUE(exit_code.has_value());
        if (!exit_code.has_value())
            return;
        const DWORD code = exit_code.value();
        ASSERT_TRUE(code == 0 || code == 7) << code;
        if (!have_interrupted && journal_pending(work.path()) &&
            Get(work.path() / "aether_state.efs") != "e0") {
            CopyTree(work.path(), interrupted.path());
            have_interrupted = true;
        }
        std::vector<std::string> errors;
        ASSERT_NE(recover_pending_journal(work.path(), &errors), RecoveryOutcome::Failed);
        EXPECT_EQ(recover_pending_journal(work.path(), nullptr), RecoveryOutcome::NothingToDo);
        const auto recovered = Tree(work.path());
        EXPECT_TRUE(recovered == baseline || recovered == finished_tree);
        complete = code == 0;
    }
    ASSERT_TRUE(complete);
    ASSERT_TRUE(have_interrupted);
    complete = false;
    for (int cut = 1; cut <= 200 && !complete; ++cut) {
        SCOPED_TRACE(cut);
        CopyTree(interrupted.path(), work.path());
        const auto exit_code = WindowsTxnChild(work.path(), cut, true);
        ASSERT_TRUE(exit_code.has_value());
        if (!exit_code.has_value())
            return;
        const DWORD code = exit_code.value();
        ASSERT_TRUE(code == 0 || code == 7) << code;
        ASSERT_NE(recover_pending_journal(work.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_EQ(recover_pending_journal(work.path(), nullptr), RecoveryOutcome::NothingToDo);
        const auto recovered = Tree(work.path());
        EXPECT_TRUE(recovered == baseline || recovered == finished_tree);
        complete = code == 0;
    }
    EXPECT_TRUE(complete);
}
#endif

namespace {
void FailAfterCreatureReplacement(const char* step, const fs::path& path) {
    if (std::string_view(step) == "after-replace" && path.filename() == "creature-entities.json") {
        set_snapshot_step_hook_for_testing(nullptr);
        throw std::runtime_error("injected failure after creature replacement");
    }
}
#ifndef _WIN32
void KillAfterCreatureReplacement(const char* step, const fs::path& path) {
    if (std::string_view(step) == "after-replace" && path.filename() == "creature-entities.json")
        std::_Exit(7);
}
#endif
} // namespace

TEST(SaveTransaction,
     RollbackAfterCreatureMemberReplacementRestoresWholeSnapshotAndDropsAcknowledgement) {
    const TempSave save;
    ASSERT_TRUE(
        CommitOnce(save.path(),
                   1,
                   {{"world_info.json", "w0"}, {"chunks/region/creature-entities.json", "c0"}}));
    const auto before = Tree(save.path());
    bool acknowledged = false;
    {
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        transaction.defer_until_commit([&acknowledged] { acknowledged = true; });
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "w1"));
        set_snapshot_step_hook_for_testing(&FailAfterCreatureReplacement);
        EXPECT_THROW(WorldSaveService::write_member_bytes(
                         save.path(), "chunks/region/creature-entities.json", "c1"),
                     std::runtime_error);
        set_snapshot_step_hook_for_testing(nullptr);
        EXPECT_EQ(Get(save.path() / "chunks/region/creature-entities.json"), "c1");
    }
    EXPECT_EQ(Tree(save.path()), before);
    EXPECT_FALSE(acknowledged);
    EXPECT_FALSE(journal_pending(save.path()));
}

#ifndef _WIN32
TEST(SaveTransactionDeathTest, RecoveryRollsBackCrashAfterCreatureReplacement) {
    const TempSave save;
    ASSERT_TRUE(
        CommitOnce(save.path(),
                   1,
                   {{"world_info.json", "w0"}, {"chunks/region/creature-entities.json", "c0"}}));
    const auto before = Tree(save.path());
    EXPECT_EXIT(
        {
            SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
            if (!transaction.begin(nullptr) ||
                !WorldSaveService::write_member_bytes(save.path(), "world_info.json", "w1"))
                std::_Exit(9);
            set_snapshot_step_hook_for_testing(&KillAfterCreatureReplacement);
            WorldSaveService::write_member_bytes(
                save.path(), "chunks/region/creature-entities.json", "c1");
            std::_Exit(9);
        },
        ::testing::ExitedWithCode(7),
        "");
    ASSERT_TRUE(journal_pending(save.path()));
    EXPECT_EQ(Get(save.path() / "chunks/region/creature-entities.json"), "c1");
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::RolledBack);
    EXPECT_EQ(Tree(save.path()), before);
    EXPECT_FALSE(journal_pending(save.path()));
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::NothingToDo);
}
#endif

} // namespace Luminumbra::Persistence
