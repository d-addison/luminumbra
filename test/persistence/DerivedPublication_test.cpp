#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/FarAuthorityOverlay.h"
#include "luminumbra_common/world/WorldStreamingState.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Luminumbra::Persistence {
namespace {
namespace fs = std::filesystem;
constexpr const char* kOverlay = "far/v1.-1.0.lmr";
constexpr const char* kCatalog = "far/catalog.json";
using Tree = std::map<std::string, std::string>;

class SaveRoot {
public:
    SaveRoot() {
        static std::atomic<unsigned> sequence{0};
        // Atomic directory creation also separates concurrent test processes without a clock.
        do {
            path = fs::temp_directory_path() /
                   ("lum_derived_publication_" + std::to_string(sequence.fetch_add(1)));
        } while (!fs::create_directory(path));
    }
    ~SaveRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    SaveRoot(const SaveRoot&) = delete;
    SaveRoot& operator=(const SaveRoot&) = delete;
    fs::path path;
};

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Put(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good());
}
Tree ReadTree(const fs::path& path) {
    Tree result;
    for (const auto& entry : fs::recursive_directory_iterator(path))
        if (entry.is_regular_file())
            result.emplace(fs::relative(entry.path(), path).generic_string(), Read(entry.path()));
    return result;
}
void Copy(const fs::path& source, const fs::path& destination) {
    fs::remove_all(destination);
    fs::copy(source, destination, fs::copy_options::recursive);
}
bool RawDigest(const fs::path& root, std::string_view path, MemberDigest& out, std::string& error) {
    bool present = false;
    if (!hash_file(root / std::string(path), out.bytes, out.fnv64, present, nullptr)) {
        error = "read failed";
        return false;
    }
    out.present = present;
    return true;
}
// Test-only consumer validator: deliberately no catalog format implementation here.
bool CatalogDigest(const fs::path& root,
                   std::string_view path,
                   MemberDigest& out,
                   std::string& error) {
    if (!RawDigest(root, path, out, error))
        return false;
    if (out.present && Read(root / std::string(path)) != "catalog receipt") {
        error = "invalid catalog receipt";
        return false;
    }
    return true;
}
std::string Overlay(std::uint64_t generation) {
    World::FarOverlayTile tile;
    tile.key = {1, -1, 0};
    tile.generation = generation;
    std::string bytes;
    EXPECT_EQ(World::EncodeFarOverlay(tile, bytes), World::FarOverlayError::None);
    return bytes;
}
bool Publish(const fs::path& root, bool derived = true) {
    SaveTransaction transaction(root, 0, {}, &RawDigest);
    if (!transaction.begin(nullptr) ||
        !WorldSaveService::write_member_bytes(root, "world_info.json", "authority"))
        return false;
    if (derived) {
        const auto group =
            transaction.BeginDerivedGroup({kOverlay, kCatalog}, nullptr, &CatalogDigest);
        if (!group.has_value())
            return false;
        if (!transaction.WriteDerivedMember(
                *group, kOverlay, Overlay(transaction.new_generation())) ||
            !transaction.WriteDerivedMember(*group, kCatalog, "catalog receipt"))
            return false;
    }
    return transaction.commit(nullptr);
}
bool PublishInspectableSave(const fs::path& root, bool indexed = true) {
    Put(root / "world_info.json", R"({"container_version":2,"name":"Example"})");
    Put(root / "preset.json",
        Read(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json"));
    WorldStreamingState state;
    if (!WorldSaveService{}.save_world(state, root))
        return false;
    if (!indexed)
        return true;
    SaveTransaction transaction(root, 0, {}, &WorldSaveService::member_digest);
    return transaction.begin(nullptr) && transaction.commit(nullptr);
}
const SnapshotMember* Find(const SnapshotIndex& index, std::string_view path) {
    for (const auto& member : index.members)
        if (member.path == path)
            return &member;
    return nullptr;
}

struct HookPlan {
    std::string step;
    fs::path path, source, copy;
    bool hit = false;
    bool fail = false;
};
HookPlan*& ActivePlan() {
    static HookPlan* plan = nullptr;
    return plan;
}
void PublicationHook(const char* step, const fs::path& path) {
    auto* plan = ActivePlan();
    if (!plan || plan->hit || plan->step != step || plan->path != path)
        return;
    plan->hit = true;
    if (!plan->copy.empty())
        Copy(plan->source, plan->copy);
    if (plan->fail)
        throw std::runtime_error("injected publication interruption");
}
class HookScope {
public:
    explicit HookScope(HookPlan& plan) {
        ActivePlan() = &plan;
        set_snapshot_step_hook_for_testing(&PublicationHook);
    }
    ~HookScope() {
        set_snapshot_step_hook_for_testing(nullptr);
        ActivePlan() = nullptr;
    }
    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;
};

TEST(DerivedPublication, ClassificationRejectsTraversalWrongClassAndNoncanonicalPaths) {
    for (const auto* path : {kOverlay, kCatalog, "far/v5.0.-33554432.lmr"}) {
        const auto cls = ClassifySnapshotMember(path);
        ASSERT_TRUE(cls.has_value());
        if (!cls.has_value())
            return;
        EXPECT_EQ(*cls, MemberClass::Derived);
    }
    for (const auto* path : {"far/../world_info.json",
                             "far/v0.0.0.lmr",
                             "far/v6.0.0.lmr",
                             "far/v1.00.0.lmr",
                             "far/v1.-0.0.lmr",
                             "far/v1.+1.0.lmr",
                             "far/v1.33554433.0.lmr",
                             "far/other.json",
                             "far/x/catalog.json",
                             "/far/catalog.json",
                             "far\\catalog.json",
                             "far/v1.0.0.lmr/extra"})
        EXPECT_FALSE(ClassifySnapshotMember(path).has_value()) << path;
    SnapshotIndex index;
    index.generation = 1;
    SnapshotMember member;
    member.path = "world_info.json";
    member.cls = MemberClass::Derived;
    index.members.push_back(member);
    SnapshotIndex decoded;
    std::string error;
    EXPECT_THROW(encode_snapshot_index(index), std::invalid_argument);
    index.members[0].cls = MemberClass::Authoritative;
    auto wrong_class = encode_snapshot_index(index);
    wrong_class.replace(wrong_class.find("authoritative"), 13, "derived");
    EXPECT_FALSE(decode_snapshot_index(wrong_class, decoded, error));
    index.members[0].path = kOverlay;
    EXPECT_THROW(encode_snapshot_index(index), std::invalid_argument);
    index.members[0].cls = MemberClass::Derived;
    wrong_class = encode_snapshot_index(index);
    wrong_class.replace(wrong_class.find("derived"), 7, "authoritative");
    EXPECT_FALSE(decode_snapshot_index(wrong_class, decoded, error));
    SaveRoot root;
    SaveTransaction transaction(root.path, 0, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    EXPECT_FALSE(transaction.BeginDerivedGroup({"world_info.json"}).has_value());
    EXPECT_FALSE(transaction.protect({kOverlay}, nullptr));
    EXPECT_FALSE(WorldSaveService::write_member_bytes(root.path, kOverlay, Overlay(1)));
    EXPECT_FALSE(WorldSaveService::remove_member(root.path, kOverlay));
}

TEST(DerivedPublication, DerivedRoundTripsAndUnchangedChangedRemovedMembersKeepCorrectGenerations) {
    SaveRoot root;
    ASSERT_TRUE(Publish(root.path));
    {
        SaveTransaction transaction(root.path, 0, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        const auto group =
            transaction.BeginDerivedGroup({kOverlay, kCatalog}, nullptr, &CatalogDigest);
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        ASSERT_TRUE(transaction.WriteDerivedMember(*group, kOverlay, Overlay(1)));
        ASSERT_TRUE(transaction.WriteDerivedMember(*group, kCatalog, "catalog receipt"));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(root.path, index, nullptr));
    const auto* overlay = Find(index, kOverlay);
    ASSERT_NE(overlay, nullptr);
    EXPECT_EQ(overlay->cls, MemberClass::Derived);
    EXPECT_EQ(overlay->generation, 1u);
    EXPECT_EQ(index.generation, 2u);
    SnapshotIndex decoded;
    std::string error;
    ASSERT_TRUE(decode_snapshot_index(encode_snapshot_index(index), decoded, error));
    EXPECT_EQ(encode_snapshot_index(index), encode_snapshot_index(decoded));
    {
        SaveTransaction transaction(root.path, 0, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        const auto group = transaction.BeginDerivedGroup({kOverlay});
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        ASSERT_TRUE(transaction.WriteDerivedMember(*group, kOverlay, Overlay(3)));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    ASSERT_TRUE(read_snapshot_index(root.path, index, nullptr));
    overlay = Find(index, kOverlay);
    ASSERT_NE(overlay, nullptr);
    EXPECT_EQ(overlay->generation, 3u);
    {
        SaveTransaction transaction(root.path, 0, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        const auto group = transaction.BeginDerivedGroup({kOverlay});
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        ASSERT_TRUE(transaction.RemoveDerivedMember(*group, kOverlay));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    ASSERT_TRUE(read_snapshot_index(root.path, index, nullptr));
    overlay = Find(index, kOverlay);
    ASSERT_NE(overlay, nullptr);
    EXPECT_FALSE(overlay->present);
    EXPECT_EQ(overlay->generation, 4u);
    EXPECT_EQ(overlay->bytes, 0u);
    EXPECT_EQ(overlay->fnv64, 0u);
}

TEST(DerivedPublication, FailureBeforeAndAfterOverlayRenameRestoresGroupAndCommitsAuthority) {
    for (const auto* step : {"before-replace", "after-replace"}) {
        SCOPED_TRACE(step);
        SaveRoot root;
        ASSERT_TRUE(Publish(root.path));
        const auto old_overlay = Read(root.path / kOverlay);
        SaveTransaction transaction(root.path, 0, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(
            WorldSaveService::write_member_bytes(root.path, "world_info.json", "new authority"));
        const auto group =
            transaction.BeginDerivedGroup({kOverlay, kCatalog}, nullptr, &CatalogDigest);
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        ASSERT_TRUE(transaction.RemoveDerivedMember(*group, kCatalog));
        HookPlan plan;
        plan.step = step;
        plan.path = root.path / kOverlay;
        plan.fail = true;
        {
            HookScope hook(plan);
            EXPECT_FALSE(transaction.WriteDerivedMember(*group, kOverlay, Overlay(2)));
        }
        ASSERT_TRUE(plan.hit);
        EXPECT_EQ(Read(root.path / kOverlay), old_overlay);
        EXPECT_EQ(Read(root.path / kCatalog), "catalog receipt");
        EXPECT_TRUE(journal_pending(root.path));
        EXPECT_TRUE(transaction.RollbackDerivedGroup(*group));
        bool acknowledged = false;
        transaction.defer_until_commit([&acknowledged] { acknowledged = true; });
        EXPECT_FALSE(acknowledged);
        ASSERT_TRUE(transaction.commit(nullptr));
        EXPECT_TRUE(acknowledged);
        EXPECT_EQ(Read(root.path / "world_info.json"), "new authority");
        SnapshotIndex index;
        ASSERT_TRUE(read_snapshot_index(root.path, index, nullptr));
        const auto* member = Find(index, kOverlay);
        ASSERT_NE(member, nullptr);
        EXPECT_EQ(member->generation, 1u);
    }
}

TEST(DerivedPublication, FailedRestorationPoisonsOuterCommitAndRetainsRecoveryJournal) {
    SaveRoot root;
    ASSERT_TRUE(Publish(root.path));
    const auto baseline = ReadTree(root.path);
    fs::path lost_undo;
    {
        SaveTransaction transaction(root.path, 0, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(
            WorldSaveService::write_member_bytes(root.path, "world_info.json", "new authority"));
        const auto group = transaction.BeginDerivedGroup({kOverlay});
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        ASSERT_TRUE(transaction.WriteDerivedMember(*group, kOverlay, Overlay(2)));
        // Losing the only old overlay copy makes a durable rollback impossible.
        for (const auto& entry : fs::directory_iterator(journal_directory(root.path) / "undo")) {
            if (Read(entry.path()) == Overlay(1)) {
                lost_undo = entry.path();
                fs::remove(entry.path());
            }
        }
        ASSERT_FALSE(lost_undo.empty());
        bool acknowledged = false;
        transaction.defer_until_commit([&acknowledged] { acknowledged = true; });
        EXPECT_FALSE(transaction.RollbackDerivedGroup(*group));
        EXPECT_FALSE(transaction.commit(nullptr));
        EXPECT_FALSE(acknowledged);
        EXPECT_FALSE(transaction.receipt());
        EXPECT_TRUE(journal_pending(root.path));
    }
    // Repair only the missing undo payload, then exercise the retained outer recovery.
    Put(lost_undo, Overlay(1));
    EXPECT_EQ(recover_pending_journal(root.path, nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(ReadTree(root.path), baseline);
}

TEST(DerivedPublication, InvalidDerivedCandidateRollsBackBeforeAuthoritativeCommit) {
    SaveRoot root;
    ASSERT_TRUE(Publish(root.path, false));
    SaveTransaction transaction(root.path, 0, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    const auto group = transaction.BeginDerivedGroup({kOverlay});
    ASSERT_TRUE(group.has_value());
    if (!group.has_value())
        return;
    ASSERT_TRUE(transaction.WriteDerivedMember(*group, kOverlay, "malformed"));
    ASSERT_TRUE(transaction.commit(nullptr));
    EXPECT_FALSE(fs::exists(root.path / kOverlay));
    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(root.path, index, nullptr));
    EXPECT_EQ(Find(index, kOverlay), nullptr);
    EXPECT_NE(Find(index, "world_info.json"), nullptr);
}

TEST(DerivedPublication, CrashesAtOverlayCatalogAndIndexPublicationRecoverBaselineOrFinal) {
    SaveRoot baseline;
    ASSERT_TRUE(Publish(baseline.path));
    const auto old_tree = ReadTree(baseline.path);
    SaveRoot complete;
    Copy(baseline.path, complete.path);
    ASSERT_TRUE(Publish(complete.path));
    const auto final_tree = ReadTree(complete.path);
    for (const auto* member : {kOverlay, kCatalog, kSnapshotIndexRelativePath}) {
        for (const auto* step : {"before-replace", "after-replace"}) {
            SCOPED_TRACE(std::string(step) + " " + member);
            SaveRoot live, interrupted;
            Copy(baseline.path, live.path);
            HookPlan plan;
            plan.step = step;
            plan.path = live.path / member;
            plan.source = live.path;
            plan.copy = interrupted.path;
            {
                HookScope hook(plan);
                ASSERT_TRUE(Publish(live.path));
            }
            ASSERT_TRUE(plan.hit);
            const bool published = std::string_view(member) == kSnapshotIndexRelativePath &&
                                   std::string_view(step) == "after-replace";
            EXPECT_EQ(recover_pending_journal(interrupted.path, nullptr),
                      published ? RecoveryOutcome::DiscardedStale : RecoveryOutcome::RolledBack);
            EXPECT_EQ(ReadTree(interrupted.path), published ? final_tree : old_tree);
            EXPECT_EQ(recover_pending_journal(interrupted.path, nullptr),
                      RecoveryOutcome::NothingToDo);
        }
    }
}

TEST(DerivedPublication, RecoveryInterruptedAtEveryRollbackMemberIsIdempotent) {
    SaveRoot root, interrupted;
    ASSERT_TRUE(Publish(root.path));
    const auto baseline = ReadTree(root.path);
    HookPlan capture;
    capture.step = "before-replace";
    capture.path = snapshot_index_path(root.path);
    capture.source = root.path;
    capture.copy = interrupted.path;
    {
        HookScope hook(capture);
        ASSERT_TRUE(Publish(root.path));
    }
    ASSERT_TRUE(capture.hit);
    for (const auto* member : {kSnapshotIndexRelativePath, kOverlay, kCatalog, "world_info.json"}) {
        SaveRoot replay;
        Copy(interrupted.path, replay.path);
        HookPlan interruption;
        interruption.step = "rollback-entry";
        interruption.path = replay.path / member;
        interruption.fail = true;
        {
            HookScope hook(interruption);
            EXPECT_EQ(recover_pending_journal(replay.path, nullptr), RecoveryOutcome::Failed);
        }
        ASSERT_TRUE(interruption.hit) << member;
        EXPECT_TRUE(journal_pending(replay.path));
        EXPECT_EQ(recover_pending_journal(replay.path, nullptr), RecoveryOutcome::RolledBack);
        EXPECT_EQ(ReadTree(replay.path), baseline);
        EXPECT_EQ(recover_pending_journal(replay.path, nullptr), RecoveryOutcome::NothingToDo);
    }
}

TEST(DerivedPublication, FinalizerRunsOnceBeforeIndexAndAcknowledgementAtDurableReceipt) {
    SaveRoot root;
    std::shared_ptr<const SnapshotCommitReceipt> receipt;
    int finalized = 0, acknowledged = 0;
    {
        SaveTransaction transaction(root.path, 0, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(root.path, "world_info.json", "staged"));
        ASSERT_TRUE(transaction.AddFinalizerOnce("far", [&](SaveTransaction& outer, auto*) {
            ++finalized;
            EXPECT_EQ(&outer, SaveTransaction::active_for(root.path));
            EXPECT_EQ(Read(root.path / "world_info.json"), "staged");
            EXPECT_FALSE(outer.receipt());
            EXPECT_EQ(acknowledged, 0);
            const auto group = outer.BeginDerivedGroup({kOverlay});
            if (!group.has_value())
                return false;
            return outer.WriteDerivedMember(*group, kOverlay, Overlay(1));
        }));
        ASSERT_TRUE(transaction.AddFinalizerOnce("far", [](auto&, auto*) { return false; }));
        transaction.defer_until_commit([&] {
            ++acknowledged;
            receipt = transaction.receipt();
            EXPECT_TRUE(transaction.committed());
            EXPECT_TRUE(snapshot_index_exists(root.path));
        });
        ASSERT_TRUE(transaction.commit(nullptr));
        EXPECT_EQ(finalized, 1);
        EXPECT_EQ(acknowledged, 1);
    }
    ASSERT_TRUE(receipt);
    const auto bytes = Read(snapshot_index_path(root.path));
    EXPECT_EQ(receipt->generation, 1u);
    EXPECT_EQ(receipt->txn_id, 1u);
    EXPECT_EQ(receipt->index_fnv64, fnv1a64_bytes(bytes.data(), bytes.size()));
}

TEST(DerivedPublication, AbortDropsDerivedOutputFinalizerAndCommitReceipt) {
    SaveRoot root;
    SaveTransaction transaction(root.path, 0, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    const auto group = transaction.BeginDerivedGroup({kOverlay});
    ASSERT_TRUE(group.has_value());
    if (!group.has_value())
        return;
    ASSERT_TRUE(transaction.WriteDerivedMember(*group, kOverlay, Overlay(1)));
    int calls = 0;
    ASSERT_TRUE(transaction.AddFinalizerOnce("far", [&](auto&, auto*) {
        ++calls;
        return true;
    }));
    transaction.defer_until_commit([&] { ++calls; });
    transaction.abort(nullptr);
    EXPECT_EQ(calls, 0);
    EXPECT_FALSE(transaction.receipt());
    EXPECT_FALSE(fs::exists(root.path / kOverlay));
    EXPECT_FALSE(snapshot_index_exists(root.path));
}

TEST(DerivedPublication, FirstLegacyAdoptionPreservesAuthorityAndAbsentOptionalRecords) {
    SaveRoot root;
    const std::string legacy = R"({"container_version":2})";
    Put(root.path / "world_info.json", legacy);
    ASSERT_TRUE(WorldSaveService::validate_save(root.path));
    EXPECT_FALSE(snapshot_index_exists(root.path));
    {
        SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    EXPECT_EQ(Read(root.path / "world_info.json"), legacy);
    EXPECT_TRUE(WorldSaveService::validate_save(root.path));
    EXPECT_FALSE(fs::exists(root.path / "far"));
    EXPECT_FALSE(fs::exists(root.path / "chunks/region/active-regions.arl"));
    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(root.path, index, nullptr));
    ASSERT_EQ(index.members.size(), 1u);
    EXPECT_EQ(index.members[0].path, "world_info.json");
    EXPECT_EQ(index.members[0].generation, 1u);
}

TEST(DerivedPublication, MissingStaleUnreceiptedAndMalformedOrFutureAreDistinct) {
    SaveRoot root;
    ASSERT_TRUE(Publish(root.path));
    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(root.path, index, nullptr));
    DerivedMemberState state = DerivedMemberState::Missing;
    std::string error;
    ASSERT_TRUE(WorldSaveService::inspect_derived_member(root.path, kOverlay, index, state, error));
    EXPECT_EQ(state, DerivedMemberState::Receipted);
    fs::remove(root.path / kOverlay);
    ASSERT_TRUE(WorldSaveService::inspect_derived_member(root.path, kOverlay, index, state, error));
    EXPECT_EQ(state, DerivedMemberState::Missing);
    Put(root.path / kOverlay, Overlay(0));
    ASSERT_TRUE(WorldSaveService::inspect_derived_member(root.path, kOverlay, index, state, error));
    EXPECT_EQ(state, DerivedMemberState::Stale);
    SnapshotIndex legacy;
    ASSERT_TRUE(
        WorldSaveService::inspect_derived_member(root.path, kOverlay, legacy, state, error));
    EXPECT_EQ(state, DerivedMemberState::Unreceipted);
    for (const auto& bytes : {std::string("malformed"), Overlay(2)}) {
        Put(root.path / kOverlay, bytes);
        EXPECT_FALSE(
            WorldSaveService::inspect_derived_member(root.path, kOverlay, index, state, error));
    }
    auto future = Overlay(1);
    future[4] = 2; // FSV1 future format, regardless of the old membership fingerprint
    Put(root.path / kOverlay, future);
    EXPECT_FALSE(
        WorldSaveService::inspect_derived_member(root.path, kOverlay, index, state, error));
    Put(root.path / kCatalog, "malformed catalog");
    EXPECT_FALSE(WorldSaveService::inspect_derived_member(
        root.path, kCatalog, index, state, error, &CatalogDigest));
    EXPECT_FALSE(
        WorldSaveService::inspect_derived_member(root.path, kCatalog, index, state, error));
}

TEST(DerivedPublication, ForeignThreadCannotMutateFinalizeOrPublishTransaction) {
    SaveRoot root;
    SaveTransaction transaction(root.path, 0, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    const auto group = transaction.BeginDerivedGroup({kOverlay});
    ASSERT_TRUE(group.has_value());
    if (!group.has_value())
        return;
    bool wrote = true, removed = true, rolled_back = true, finalized = true, committed = true;
    std::thread worker([&] {
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        wrote = transaction.WriteDerivedMember(*group, kOverlay, Overlay(1));
        removed = transaction.RemoveDerivedMember(*group, kOverlay);
        rolled_back = transaction.RollbackDerivedGroup(*group);
        finalized = transaction.AddFinalizerOnce("far", [](auto&, auto*) { return true; });
        committed = transaction.commit(nullptr);
        transaction.abort(nullptr);
    });
    worker.join();
    EXPECT_FALSE(wrote);
    EXPECT_FALSE(removed);
    EXPECT_FALSE(rolled_back);
    EXPECT_FALSE(finalized);
    EXPECT_FALSE(committed);
    EXPECT_FALSE(transaction.receipt());
    ASSERT_TRUE(transaction.WriteDerivedMember(*group, kOverlay, Overlay(1)));
    ASSERT_TRUE(transaction.commit(nullptr));
}

TEST(DerivedPublication, FailedFinalizerAbortsAuthorityAndDropsAcknowledgements) {
    SaveRoot root;
    ASSERT_TRUE(Publish(root.path, false));
    const auto baseline = ReadTree(root.path);
    bool acknowledged = false;
    SaveTransaction transaction(root.path, 0, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(root.path, "world_info.json", "replacement"));
    ASSERT_TRUE(transaction.AddFinalizerOnce("far", [](auto&, auto*) { return false; }));
    transaction.defer_until_commit([&] { acknowledged = true; });
    EXPECT_FALSE(transaction.commit(nullptr));
    EXPECT_FALSE(acknowledged);
    EXPECT_FALSE(transaction.receipt());
    EXPECT_EQ(ReadTree(root.path), baseline);
}

TEST(DerivedPublication, ReadOnlyInspectionReportsPendingRecoveryWithoutChangingFiles) {
    SaveRoot live, interrupted;
    ASSERT_TRUE(Publish(live.path));
    HookPlan capture;
    capture.step = "before-replace";
    capture.path = snapshot_index_path(live.path);
    capture.source = live.path;
    capture.copy = interrupted.path;
    {
        HookScope hook(capture);
        ASSERT_TRUE(Publish(live.path));
    }
    ASSERT_TRUE(capture.hit);
    const auto before = ReadTree(interrupted.path);
    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(interrupted.path, index, nullptr));
    DerivedMemberState state = DerivedMemberState::Missing;
    std::string error;
    EXPECT_FALSE(WorldSaveService::inspect_derived_member(
        interrupted.path, kCatalog, index, state, error, &CatalogDigest));
    EXPECT_EQ(state, DerivedMemberState::RecoveryPending);
    EXPECT_EQ(ReadTree(interrupted.path), before);
    EXPECT_EQ(recover_pending_journal(interrupted.path, nullptr), RecoveryOutcome::RolledBack);
}

TEST(DerivedPublication, CatalogAndValidatePreserveEveryFileWithPendingJournal) {
    SaveRoot root;
    const auto save = root.path / "worlds/saves/example";
    ASSERT_TRUE(PublishInspectableSave(save));
    const auto baseline = ReadTree(save);
    SaveRoot interrupted;
    {
        SaveTransaction transaction(save, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save, "world_info.json", "partial"));
        Copy(root.path, interrupted.path);
    }
    const auto pending = interrupted.path / "worlds/saves/example";
    const auto before = ReadTree(interrupted.path);
    const auto catalog = EnumerateSavedWorlds(interrupted.path);
    ASSERT_TRUE(catalog.error.empty());
    ASSERT_EQ(catalog.worlds.size(), 1u);
    EXPECT_TRUE(catalog.worlds[0].pending_recovery);
    EXPECT_TRUE(catalog.worlds[0].error.empty());
    EXPECT_EQ(ReadTree(interrupted.path), before);
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::validate_save(pending, &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors.front(), "pending snapshot recovery");
    EXPECT_EQ(ReadTree(interrupted.path), before);
    EXPECT_EQ(recover_pending_journal(pending, nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(ReadTree(pending), baseline);
    EXPECT_TRUE(WorldSaveService::validate_save(pending));
}

TEST(DerivedPublication, CatalogAndValidatePreserveEveryFileWithIndexedStrayStaging) {
    SaveRoot root;
    const auto save = root.path / "worlds/saves/example";
    ASSERT_TRUE(PublishInspectableSave(save));
    ASSERT_TRUE(InspectSavedWorld(root.path, "example").error.empty());
    for (const auto* path :
         {"world_info.json.tmp.1.2", "chunks/region/r.0.0.lmr.tmp.1.2", "far/v1.-1.0.lmr.tmp.1.2"})
        Put(save / path, "stray staging bytes");
    ASSERT_FALSE(journal_pending(save));
    const auto before = ReadTree(root.path);
    const auto catalog = EnumerateSavedWorlds(root.path);
    ASSERT_TRUE(catalog.error.empty());
    ASSERT_EQ(catalog.worlds.size(), 1u);
    EXPECT_FALSE(catalog.worlds[0].pending_recovery);
    EXPECT_FALSE(catalog.worlds[0].error.empty());
    EXPECT_EQ(ReadTree(root.path), before);
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::validate_save(save, &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(ReadTree(root.path), before);
}

TEST(DerivedPublication, InspectionAndRecoveryCannotAlterAnotherThreadsLiveTransaction) {
    SaveRoot root;
    const auto save = root.path / "worlds/saves/example";
    ASSERT_TRUE(PublishInspectableSave(save));
    SaveTransaction transaction(save, 0, {}, &WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(nullptr));
    for (const bool staged : {false, true}) {
        SCOPED_TRACE(staged);
        if (staged)
            ASSERT_TRUE(WorldSaveService::write_member_bytes(
                save, "world_info.json", R"({"container_version":2,"name":"Updated"})"));
        EXPECT_EQ(fs::exists(journal_directory(save) / "txn.json"), staged);
        const auto before = ReadTree(root.path);
        // The owner holds the transaction open until inspection has finished. Neither thread
        // mutates members while the inspector compares bytes, so no timing controls are needed.
        std::thread worker([&] {
            const auto catalog = EnumerateSavedWorlds(root.path);
            EXPECT_TRUE(catalog.error.empty());
            ASSERT_EQ(catalog.worlds.size(), 1u);
            EXPECT_TRUE(catalog.worlds[0].pending_recovery);
            EXPECT_TRUE(catalog.worlds[0].error.empty());
            EXPECT_EQ(ReadTree(root.path), before);
            std::vector<std::string> errors;
            EXPECT_FALSE(WorldSaveService::validate_save(save, &errors));
            EXPECT_EQ(errors, std::vector<std::string>{"pending snapshot recovery"});
            EXPECT_EQ(ReadTree(root.path), before);
            WorldStreamingState loaded;
            errors.clear();
            EXPECT_FALSE(WorldSaveService{}.load_world(loaded, save, errors));
            EXPECT_EQ(errors, std::vector<std::string>{"pending snapshot recovery"});
            EXPECT_EQ(ReadTree(root.path), before);
            EXPECT_EQ(recover_pending_journal(save / ".", nullptr), RecoveryOutcome::Failed);
            EXPECT_EQ(ReadTree(root.path), before);
            SaveTransaction competing(save / ".", 0, {}, &WorldSaveService::member_digest);
            EXPECT_FALSE(competing.begin(nullptr));
            EXPECT_EQ(ReadTree(root.path), before);
        });
        worker.join();
        EXPECT_EQ(ReadTree(root.path), before);
    }
    ASSERT_TRUE(transaction.commit(nullptr));
    EXPECT_TRUE(transaction.receipt());
    EXPECT_FALSE(journal_pending(save));
    EXPECT_TRUE(InspectSavedWorld(root.path, "example").error.empty());
}

TEST(DerivedPublication, CatalogConsumerValidationIsRequiredForPresentRecords) {
    SaveRoot root;
    ASSERT_TRUE(Publish(root.path));
    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(root.path, index, nullptr));
    EXPECT_TRUE(
        WorldSaveService::verify_derived_members(root.path, index, nullptr, &CatalogDigest));
    EXPECT_FALSE(WorldSaveService::verify_derived_members(root.path, index, nullptr));
    Put(root.path / kCatalog, "future catalog");
    EXPECT_FALSE(
        WorldSaveService::verify_derived_members(root.path, index, nullptr, &CatalogDigest));
}

TEST(DerivedPublication, SafeOptionalRollbackRetainsUndoForLaterCrashRecovery) {
    SaveRoot root, interrupted;
    ASSERT_TRUE(Publish(root.path));
    const auto baseline = ReadTree(root.path);
    {
        SaveTransaction transaction(root.path, 0, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(
            WorldSaveService::write_member_bytes(root.path, "world_info.json", "replacement"));
        const auto group =
            transaction.BeginDerivedGroup({kOverlay, kCatalog}, nullptr, &CatalogDigest);
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        ASSERT_TRUE(transaction.WriteDerivedMember(*group, kOverlay, Overlay(2)));
        ASSERT_TRUE(transaction.RemoveDerivedMember(*group, kCatalog));
        ASSERT_TRUE(transaction.RollbackDerivedGroup(*group));
        Copy(root.path, interrupted.path);
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    ASSERT_TRUE(journal_pending(interrupted.path));
    EXPECT_EQ(recover_pending_journal(interrupted.path, nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(ReadTree(interrupted.path), baseline);
    EXPECT_EQ(recover_pending_journal(interrupted.path, nullptr), RecoveryOutcome::NothingToDo);
}

TEST(DerivedPublication, OversizedExistingDerivedUndoRefusesWithoutChangingRecoveryProtection) {
    for (const auto* path : {kOverlay, kCatalog}) {
        SCOPED_TRACE(path);
        SaveRoot root, interrupted;
        ASSERT_TRUE(Publish(root.path));
        const auto baseline_index = Read(snapshot_index_path(root.path));
        const auto cap =
            path == std::string_view(kCatalog) ? kMaxSnapshotIndexBytes : World::kFsv1MaxFileBytes;
        fs::resize_file(root.path / path, cap + 1);
        MemberDigest oversized;
        std::string error;
        ASSERT_TRUE(RawDigest(root.path, path, oversized, error));
        ASSERT_EQ(oversized.bytes, cap + 1);
        {
            SaveTransaction transaction(root.path, 0, {}, &RawDigest);
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(WorldSaveService::write_member_bytes(
                root.path, "world_info.json", "staged authority"));
            const auto protection = ReadTree(journal_directory(root.path));
            std::vector<std::string> errors;
            EXPECT_FALSE(
                transaction.BeginDerivedGroup({kOverlay, kCatalog}, &errors, &CatalogDigest)
                    .has_value());
            EXPECT_FALSE(errors.empty());
            EXPECT_EQ(ReadTree(journal_directory(root.path)), protection);
            EXPECT_EQ(Read(root.path / "world_info.json"), "staged authority");
            EXPECT_EQ(Read(snapshot_index_path(root.path)), baseline_index);
            EXPECT_FALSE(transaction.receipt());
            MemberDigest after;
            ASSERT_TRUE(RawDigest(root.path, path, after, error));
            EXPECT_EQ(after.bytes, oversized.bytes);
            EXPECT_EQ(after.fnv64, oversized.fnv64);
            Copy(root.path, interrupted.path);
            transaction.abort(nullptr);
        }
        EXPECT_EQ(recover_pending_journal(interrupted.path, nullptr), RecoveryOutcome::RolledBack);
        for (const auto& restored : {root.path, interrupted.path}) {
            EXPECT_EQ(Read(restored / "world_info.json"), "authority");
            EXPECT_EQ(Read(snapshot_index_path(restored)), baseline_index);
            MemberDigest after;
            ASSERT_TRUE(RawDigest(restored, path, after, error));
            EXPECT_EQ(after.bytes, oversized.bytes);
            EXPECT_EQ(after.fnv64, oversized.fnv64);
            EXPECT_FALSE(journal_pending(restored));
        }
    }
}

TEST(DerivedPublication, OversizedDerivedCandidateNeverInstallsAndRetainsOuterUndo) {
    for (const auto* path : {kOverlay, kCatalog}) {
        SCOPED_TRACE(path);
        SaveRoot root, interrupted;
        ASSERT_TRUE(Publish(root.path));
        const auto baseline = ReadTree(root.path);
        SaveTransaction transaction(root.path, 0, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(
            WorldSaveService::write_member_bytes(root.path, "world_info.json", "staged authority"));
        const auto group =
            transaction.BeginDerivedGroup({kOverlay, kCatalog}, nullptr, &CatalogDigest);
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        const auto protection = ReadTree(journal_directory(root.path));
        const auto* peer = path == std::string_view(kCatalog) ? kOverlay : kCatalog;
        ASSERT_TRUE(transaction.RemoveDerivedMember(*group, peer));
        const auto cap =
            path == std::string_view(kCatalog) ? kMaxSnapshotIndexBytes : World::kFsv1MaxFileBytes;
        const std::string candidate(static_cast<std::size_t>(cap + 1), 'x');
        HookPlan installation;
        installation.step = "before-replace";
        installation.path = root.path / path;
        std::vector<std::string> errors;
        {
            HookScope hook(installation);
            EXPECT_FALSE(transaction.WriteDerivedMember(*group, path, candidate, &errors));
        }
        EXPECT_FALSE(errors.empty());
        EXPECT_FALSE(installation.hit);
        EXPECT_EQ(ReadTree(journal_directory(root.path)), protection);
        EXPECT_EQ(Read(root.path / kOverlay), baseline.at(kOverlay));
        EXPECT_EQ(Read(root.path / kCatalog), baseline.at(kCatalog));
        EXPECT_EQ(Read(root.path / "world_info.json"), "staged authority");
        EXPECT_FALSE(transaction.receipt());
        Copy(root.path, interrupted.path);
        ASSERT_TRUE(transaction.commit(nullptr));
        EXPECT_EQ(Read(root.path / "world_info.json"), "staged authority");
        EXPECT_EQ(recover_pending_journal(interrupted.path, nullptr), RecoveryOutcome::RolledBack);
        EXPECT_EQ(ReadTree(interrupted.path), baseline);
        EXPECT_EQ(recover_pending_journal(interrupted.path, nullptr), RecoveryOutcome::NothingToDo);
    }
}

TEST(DerivedPublication, MissingDerivedMemberReconstructsWithoutChangingAuthoritativeFingerprint) {
    SaveRoot root;
    ASSERT_TRUE(Publish(root.path));
    SnapshotIndex before;
    ASSERT_TRUE(read_snapshot_index(root.path, before, nullptr));
    const auto* authority_before = Find(before, "world_info.json");
    ASSERT_NE(authority_before, nullptr);
    fs::remove(root.path / kOverlay);
    DerivedMemberState state = DerivedMemberState::Receipted;
    std::string error;
    ASSERT_TRUE(
        WorldSaveService::inspect_derived_member(root.path, kOverlay, before, state, error));
    ASSERT_EQ(state, DerivedMemberState::Missing);
    {
        SaveTransaction transaction(root.path, 0, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        const auto group = transaction.BeginDerivedGroup({kOverlay});
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        ASSERT_TRUE(transaction.WriteDerivedMember(*group, kOverlay, Overlay(1)));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    SnapshotIndex after;
    ASSERT_TRUE(read_snapshot_index(root.path, after, nullptr));
    const auto* authority_after = Find(after, "world_info.json");
    ASSERT_NE(authority_after, nullptr);
    EXPECT_EQ(authority_before->generation, authority_after->generation);
    EXPECT_EQ(authority_before->bytes, authority_after->bytes);
    EXPECT_EQ(authority_before->fnv64, authority_after->fnv64);
    EXPECT_EQ(authority_before->present, authority_after->present);
    ASSERT_TRUE(WorldSaveService::inspect_derived_member(root.path, kOverlay, after, state, error));
    EXPECT_EQ(state, DerivedMemberState::Receipted);
}

TEST(DerivedPublication, OwnerOpenRecoveryPrecedesReadOnlyDerivedValidation) {
    SaveRoot root, interrupted;
    Put(root.path / "world_info.json", R"({"container_version":2})");
    {
        SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        const auto group = transaction.BeginDerivedGroup({kOverlay});
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        ASSERT_TRUE(transaction.WriteDerivedMember(*group, kOverlay, Overlay(1)));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    const auto baseline = ReadTree(root.path);
    {
        SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        const auto group = transaction.BeginDerivedGroup({kOverlay});
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        ASSERT_TRUE(transaction.WriteDerivedMember(*group, kOverlay, "malformed"));
        Copy(root.path, interrupted.path);
    }
    const auto pending = ReadTree(interrupted.path);
    EXPECT_FALSE(WorldSaveService::validate_save(interrupted.path));
    EXPECT_EQ(ReadTree(interrupted.path), pending);
    // Session opening recovers on the owner thread before invoking the read-only loader.
    EXPECT_EQ(recover_pending_journal(interrupted.path, nullptr), RecoveryOutcome::RolledBack);
    EXPECT_TRUE(WorldSaveService::validate_save(interrupted.path));
    EXPECT_EQ(ReadTree(interrupted.path), baseline);
    Put(interrupted.path / kOverlay, "malformed");
    EXPECT_FALSE(WorldSaveService::validate_save(interrupted.path));
    Put(interrupted.path / kOverlay, Overlay(2));
    EXPECT_FALSE(WorldSaveService::validate_save(interrupted.path));
    fs::remove(interrupted.path / kOverlay);
    EXPECT_TRUE(WorldSaveService::validate_save(interrupted.path));
}

void ExpectRecognizedMalformedDerivedLoadRefusal(bool indexed) {
    SaveRoot root;
    ASSERT_TRUE(PublishInspectableSave(root.path, indexed));
    WorldSaveService service;
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(service.load_world(loaded, root.path, errors));
    ASSERT_TRUE(errors.empty());
    ASSERT_EQ(snapshot_index_exists(root.path), indexed);
    for (const auto* path : {"far/v1.33554433.0.lmr", "far/v1.0.-33554433.lmr", kCatalog}) {
        SCOPED_TRACE(path);
        Put(root.path / path, "malformed derived record");
        const auto before = ReadTree(root.path);
        errors.clear();
        EXPECT_FALSE(service.load_world(loaded, root.path, errors));
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find(path), std::string::npos);
        EXPECT_TRUE(loaded.empty());
        EXPECT_EQ(ReadTree(root.path), before);
        fs::remove(root.path / path);
        errors.clear();
        EXPECT_TRUE(service.load_world(loaded, root.path, errors));
        EXPECT_TRUE(errors.empty());
    }
}

TEST(DerivedPublication, IndexedLoadRefusesRecognizedMalformedDerivedFiles) {
    ExpectRecognizedMalformedDerivedLoadRefusal(true);
}

TEST(DerivedPublication, LegacyLoadRefusesRecognizedMalformedDerivedFiles) {
    ExpectRecognizedMalformedDerivedLoadRefusal(false);
}

TEST(DerivedPublication, FinalizersRunForFirstMetadataAndZeroDirtyRetryTransactions) {
    SaveRoot root;
    int finalized = 0;
    for (std::uint64_t pass = 0; pass < 3; ++pass) {
        SaveTransaction transaction(root.path, 0, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.AddFinalizerOnce("far", [&](auto& outer, auto*) {
            EXPECT_EQ(outer.new_generation(), pass + 1);
            ++finalized;
            return true;
        }));
        if (pass == 1)
            ASSERT_TRUE(
                WorldSaveService::write_member_bytes(root.path, "world_info.json", "metadata"));
        ASSERT_TRUE(transaction.commit(nullptr));
        EXPECT_EQ(finalized, pass + 1);
    }
}
} // namespace
} // namespace Luminumbra::Persistence
