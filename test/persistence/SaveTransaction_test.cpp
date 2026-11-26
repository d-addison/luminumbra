#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/persistence/CreatedWorldArtifacts.h"
#include "luminumbra_common/persistence/RecoveryDurability.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/WorldSaveService.h"

#include "../support/PersistenceFileOps.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;

namespace Luminumbra::Persistence::detail {
std::size_t file_publication_stripe_for_testing(const fs::path& path);
}

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
                                   &diagnostics)) {
                        error = diagnostics.empty() ? "read failed" : diagnostics.front();
                        return false;
                    }
                    out.present = exists;
                    return true;
                });
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(transaction.protect({"aether_state.efs"}, nullptr));
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
#if defined(_WIN32)
        name += std::to_string(::GetCurrentProcessId()) + "_";
#else
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

std::string_view& ReceiptInterruption() {
    static std::string_view selected;
    return selected;
}

fs::path FindReceiptCarrier(const fs::path& save) {
    for (const auto& child : fs::directory_iterator(save))
        if (child.path().filename().string().starts_with("journal.receipts."))
            return child.path();
    return {};
}

std::vector<nlohmann::json> JournalRecords(const fs::path& save) {
    std::istringstream input(Get(journal_directory(save) / "txn.json"));
    std::vector<nlohmann::json> records;
    std::string line;
    while (std::getline(input, line))
        records.push_back(nlohmann::json::parse(line));
    return records;
}

const nlohmann::json& LastEntryRecord(const std::vector<nlohmann::json>& records) {
    const auto found = std::find_if(records.rbegin(), records.rend(), [](const auto& record) {
        return record.at("record") == "entry" && !record.at("entries").empty();
    });
    if (found == records.rend())
        throw std::runtime_error("missing journal entry record");
    return *found;
}

std::string EncodeJournalRecords(const std::vector<nlohmann::json>& records) {
    std::string bytes;
    for (const auto& record : records)
        bytes += record.dump() + "\n";
    return bytes;
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

// Unpublished root staging is deliberately outside the strict inventory. Check
// its exact preservation separately from the authoritative committed tree.
TreeMap UnregisteredStaging(const fs::path& save) {
    std::set<std::string> registered;
    auto bytes = Get(journal_directory(save) / "txn.json");
    if (bytes.empty())
        bytes = Get(journal_directory(save) / "undo/cleanup.txn.json");
    if (bytes.empty()) {
        const auto carrier = FindReceiptCarrier(save);
        if (!carrier.empty())
            bytes = Get(carrier);
    }
    std::string_view remaining(bytes);
    while (true) {
        const auto end = remaining.find('\n');
        if (end == std::string_view::npos)
            break;
        const auto record = nlohmann::json::parse(remaining.substr(0, end));
        for (const auto& entry : record.at("entries"))
            for (const auto* key : {"undo_temporary", "incoming_temporary", "restored_temporary"})
                registered.insert(entry.at(key).get<std::string>());
        remaining.remove_prefix(end + 1);
    }
    TreeMap neutral;
    for (const auto& [name, payload] : Tree(save))
        if (name.find('/') == std::string::npos && name.find(".tmp.") != std::string::npos &&
            !registered.contains(name))
            neutral.emplace(name, payload);
    return neutral;
}

TreeMap RecoveredCommittedTree(const fs::path& save, const TreeMap& neutral) {
    auto tree = Tree(save);
    for (const auto& [name, payload] : neutral) {
        const auto found = tree.find(name);
        EXPECT_NE(found, tree.end()) << name;
        if (found != tree.end()) {
            EXPECT_EQ(found->second, payload) << name;
            tree.erase(found);
        }
    }
    return tree;
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

#if defined(__linux__)
std::size_t OpenDescriptorCount() {
    std::error_code error;
    std::size_t count = 0;
    for (fs::directory_iterator it("/proc/self/fd", error), end; !error && it != end;
         it.increment(error))
        ++count;
    return error ? std::numeric_limits<std::size_t>::max() : count;
}

std::size_t& DescriptorPeak() {
    static std::size_t peak = 0;
    return peak;
}

void ObserveTransactionDescriptors(const char* step, const fs::path&) {
    const std::string_view phase(step);
    if (phase == "journal-published" || phase == "after-replace" || phase == "rollback-entry")
        DescriptorPeak() = std::max(DescriptorPeak(), OpenDescriptorCount());
}

void RollbackWithExhaustedDescriptors(const fs::path& save_dir) {
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0)
        std::_Exit(1);
    limit.rlim_cur = std::min<rlim_t>(limit.rlim_cur, 128);
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0)
        std::_Exit(2);
    SaveTransaction transaction(save_dir, 2, {}, &RawDigest);
    std::vector<std::string> errors;
    if (!transaction.begin(&errors) ||
        !transaction.protect({"world_info.json", "aether_state.efs"}, &errors) ||
        !WorldSaveService::write_member_bytes(
            save_dir, "world_info.json", "replacement", &errors) ||
        !WorldSaveService::write_member_bytes(save_dir, "aether_state.efs", "created", &errors))
        std::_Exit(3);
    std::vector<int> descriptors;
    int descriptor = -1;
    while ((descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC)) >= 0)
        descriptors.push_back(descriptor);
    if (errno != EMFILE)
        std::_Exit(4);
    transaction.abort(&errors);
    for (const auto held : descriptors)
        ::close(held);
    std::_Exit(errors.empty() && !journal_pending(save_dir) ? 0 : 5);
}
#endif

struct RollbackMutationState {
    fs::path target;
    bool changed = false;
};

RollbackMutationState& RollbackMutation() {
    static RollbackMutationState state;
    return state;
}

void ChangeLaterRollbackOccupant(const char* step, const fs::path& path) {
    if (std::string_view(step) == "rollback-entry" && path.filename() == "aether_state.efs") {
        Put(RollbackMutation().target, "changed after rollback preflight");
        RollbackMutation().changed = true;
    }
}

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
        fs::rename(JournalCapture().source, JournalCapture().target);
    }
}

bool RunInventoryScenario(const fs::path& dir) {
    SaveTransaction transaction(dir, 5, {}, &RawDigest);
    return transaction.begin(nullptr) &&
           transaction.protect({"world_info.json",
                                "aether_state.efs",
                                "chunks/region/world-manifest.json",
                                "chunks/region/plant-entities.json"},
                               nullptr) &&
           WorldSaveService::write_member_bytes(dir, "world_info.json", "new metadata") &&
           WorldSaveService::write_member_bytes(
               dir, "chunks/region/world-manifest.json", "new manifest") &&
           WorldSaveService::write_member_bytes(
               dir, "chunks/region/plant-entities.json", "created") &&
           WorldSaveService::remove_member(dir, "aether_state.efs") && transaction.commit(nullptr);
}

#if !defined(_WIN32)
// Death-test state. Death tests run in forked children (gtest "fast" style), so the parent-side
// paths are inherited unchanged by the child.
struct DeathTestState {
    int kill_at = 0;
    int calls = 0;
    int published_at = 0;
    int last_exit_code = -1;
    bool include_disk_events = false;
};

DeathTestState& DeathState() {
    static DeathTestState state;
    return state;
}

void KillHook(const char* step, const fs::path& /*path*/) {
    if (!DeathState().include_disk_events && std::string_view(step).starts_with("crash-"))
        return;
    if (++DeathState().calls == DeathState().kill_at) {
        std::_Exit(7);
    }
}

void RecordHook(const char* step, const fs::path& /*path*/) {
    if (!DeathState().include_disk_events && std::string_view(step).starts_with("crash-"))
        return;
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

void ChildInventoryScenario(const fs::path& dir, int cut, bool before_index = false) {
    DeathState().calls = 0;
    DeathState().kill_at = cut;
    DeathState().include_disk_events = true;
    if (before_index) {
        set_snapshot_step_hook_for_testing(+[](const char* step, const fs::path& path) {
            if (std::string_view(step) == "before-replace" && path.filename() == "snapshot.json")
                std::_Exit(7);
        });
    } else {
        set_snapshot_step_hook_for_testing(&KillHook);
    }
    std::_Exit(RunInventoryScenario(dir) ? 0 : 9);
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

namespace {
using Receipt = detail::NamespaceReceipt;
using ReceiptPhase = detail::NamespaceReceiptPhase;
using ReceiptOutcome = detail::NamespaceRetirementOutcome;
using NamespaceState = detail::NamespaceEntryState;
using FlushObservation = detail::NamespaceFlushObservation;

// One artifact, one durable receipt, and at most one injected failure per attempt.
// Reconstructing the receipt from durable models restarting between any two phases.
struct ReceiptOperations {
    Receipt durable{.identity = {7, 11, 13}, .phase = ReceiptPhase::Prepared};
    std::array<std::uint64_t, 3> occupant = durable.identity;
    FlushObservation observation{
        .succeeded = true, .volume_known = true, .ntfs = true, .volume = 7};
    bool present = true;
    bool retained = true;
    bool unknown_namespace = false;
    std::string failure;
    std::vector<std::string> trace;

    NamespaceState inspect_namespace(const std::array<std::uint64_t, 3>& expected) {
        trace.emplace_back("inspect");
        if (unknown_namespace)
            return static_cast<NamespaceState>(255);
        if (failure == "inspect" || (present && occupant != expected))
            return NamespaceState::Refused;
        return present ? NamespaceState::Owned : NamespaceState::Absent;
    }
    bool record_prepared(const std::array<std::uint64_t, 3>& expected) {
        trace.emplace_back("prepare");
        if (failure == "prepare")
            return false;
        durable = {.identity = expected, .phase = ReceiptPhase::Prepared};
        return true;
    }
    bool remove_owned(const std::array<std::uint64_t, 3>& expected) {
        trace.emplace_back("remove");
        if (failure == "remove" || !present || occupant != expected)
            return false;
        present = false;
        return failure != "remove-after";
    }
    bool record_namespace_done(const std::array<std::uint64_t, 3>& expected) {
        trace.emplace_back("namespace-record");
        if (failure == "namespace-record")
            return false;
        durable = {.identity = expected, .phase = ReceiptPhase::NamespaceDone};
        return true;
    }
    FlushObservation flush_later_commit() {
        trace.emplace_back("later-flush");
        return failure == "later-flush" ? FlushObservation{} : observation;
    }
    bool record_flush_observed(const std::array<std::uint64_t, 3>& expected,
                               const FlushObservation& observed) {
        trace.emplace_back("flush-record");
        if (failure == "flush-record" || !observed.succeeded || !observed.volume_known ||
            !observed.ntfs || observed.volume != expected.front())
            return false;
        durable = {.identity = expected, .phase = ReceiptPhase::FlushObserved};
        return true;
    }
    bool retire_receipt(const std::array<std::uint64_t, 3>& expected) {
        trace.emplace_back("retire");
        if (failure == "retire" || durable.identity != expected ||
            durable.phase != ReceiptPhase::FlushObserved)
            return false;
        retained = false;
        return true;
    }
};
} // namespace

TEST(SaveTransaction, NamespaceReceiptRetainedWithoutLaterFlush) {
    ReceiptOperations operations;
    operations.observation.succeeded = false;
    auto receipt = operations.durable;
    EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Failed);
    EXPECT_FALSE(operations.present);
    EXPECT_TRUE(operations.retained);
    EXPECT_EQ(operations.durable.phase, ReceiptPhase::NamespaceDone);
    EXPECT_EQ(operations.trace,
              (std::vector<std::string>{"inspect", "remove", "namespace-record", "later-flush"}));
}

TEST(SaveTransaction, NamespaceReceiptRetainedOnDifferentVolume) {
    ReceiptOperations operations;
    operations.observation.volume = 19;
    auto receipt = operations.durable;
    EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Retained);
    EXPECT_TRUE(operations.retained);
    EXPECT_EQ(receipt.phase, ReceiptPhase::NamespaceDone);
    EXPECT_EQ(operations.trace.back(), "later-flush");
    // The very same observation can qualify another receipt on its own volume.
    ReceiptOperations same_volume;
    same_volume.observation = operations.observation;
    same_volume.durable.identity.front() = operations.observation.volume;
    same_volume.occupant = same_volume.durable.identity;
    auto qualified = same_volume.durable;
    EXPECT_EQ(detail::CompleteNamespaceRetirement(qualified, same_volume), ReceiptOutcome::Retired);
    EXPECT_TRUE(operations.retained);
    EXPECT_FALSE(same_volume.retained);
}

TEST(SaveTransaction, NamespaceReceiptRetainedOnNonNtfsOrUnknownVolume) {
    for (const bool known : {false, true}) {
        SCOPED_TRACE(known);
        ReceiptOperations operations;
        operations.observation.volume_known = known;
        operations.observation.ntfs = !known;
        auto receipt = operations.durable;
        EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations),
                  ReceiptOutcome::Retained);
        EXPECT_TRUE(operations.retained);
        EXPECT_EQ(receipt.phase, ReceiptPhase::NamespaceDone);
        EXPECT_EQ(operations.trace.back(), "later-flush");
    }
}

TEST(SaveTransaction, NamespaceReceiptRetiresAfterLaterSameVolumeNtfsFlush) {
    ReceiptOperations operations;
    auto receipt = operations.durable;
    EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Retired);
    EXPECT_FALSE(operations.retained);
    EXPECT_EQ(operations.durable.phase, ReceiptPhase::FlushObserved);
    EXPECT_EQ(
        operations.trace,
        (std::vector<std::string>{
            "inspect", "remove", "namespace-record", "later-flush", "flush-record", "retire"}));
}

TEST(SaveTransaction, NamespaceReceiptSurvivesInterruptionBeforeLaterFlush) {
    ReceiptOperations operations;
    operations.failure = "later-flush";
    auto receipt = operations.durable;
    EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Failed);
    ASSERT_TRUE(operations.retained);
    ASSERT_FALSE(operations.present);
    // Discard volatile phase state, and model an identity-matching resurrected entry.
    receipt = operations.durable;
    operations.present = true;
    operations.occupant = receipt.identity;
    operations.failure.clear();
    operations.trace.clear();
    EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Retired);
    EXPECT_FALSE(operations.present);
    EXPECT_FALSE(operations.retained);
    EXPECT_EQ(operations.trace,
              (std::vector<std::string>{"inspect",
                                        "prepare",
                                        "remove",
                                        "namespace-record",
                                        "later-flush",
                                        "flush-record",
                                        "retire"}));
}

TEST(SaveTransaction, NamespaceReceiptFailuresPreserveEvidenceAndRetry) {
    for (const auto* failure : {"inspect",
                                "prepare",
                                "remove",
                                "namespace-record",
                                "later-flush",
                                "flush-record",
                                "retire"}) {
        SCOPED_TRACE(failure);
        ReceiptOperations operations;
        operations.failure = failure;
        if (operations.failure == "prepare")
            operations.durable.phase = ReceiptPhase::FlushObserved;
        auto receipt = operations.durable;
        EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Failed);
        EXPECT_TRUE(operations.retained);
        EXPECT_EQ(operations.trace.back(), failure);
        EXPECT_LE(operations.trace.size(), 7u);
        // Retry uses only the durable record, including when the entry is already absent.
        receipt = operations.durable;
        operations.failure.clear();
        operations.trace.clear();
        EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations),
                  ReceiptOutcome::Retired);
        EXPECT_FALSE(operations.present);
        EXPECT_FALSE(operations.retained);
    }
}

TEST(SaveTransaction, NamespaceReceiptRetirementRetryReusesCompletedPhases) {
    ReceiptOperations operations;
    operations.failure = "retire";
    auto receipt = operations.durable;
    ASSERT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Failed);
    ASSERT_EQ(operations.durable.phase, ReceiptPhase::FlushObserved);
    ASSERT_FALSE(operations.present);
    // Retries append no records and need no further flush once completion is durable.
    for (int attempt = 0; attempt < 3; ++attempt) {
        SCOPED_TRACE(attempt);
        receipt = operations.durable;
        operations.trace.clear();
        EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Failed);
        EXPECT_TRUE(operations.retained);
        EXPECT_EQ(operations.trace, (std::vector<std::string>{"inspect", "retire"}));
    }
    operations.failure.clear();
    EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Retired);
}

TEST(SaveTransaction, NamespaceReceiptInvalidatesOldCompletionBeforeRepeatedRemoval) {
    ReceiptOperations operations;
    operations.durable.phase = ReceiptPhase::FlushObserved;
    operations.failure = "remove-after";
    auto receipt = operations.durable;
    EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Failed);
    EXPECT_FALSE(operations.present);
    EXPECT_TRUE(operations.retained);
    EXPECT_EQ(operations.durable.phase, ReceiptPhase::Prepared);
    EXPECT_EQ(operations.trace, (std::vector<std::string>{"inspect", "prepare", "remove"}));
    // A crash after removal but before its completion record cannot reuse the old flush.
    operations.failure = "later-flush";
    operations.trace.clear();
    receipt = operations.durable;
    EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Failed);
    EXPECT_TRUE(operations.retained);
    EXPECT_EQ(operations.durable.phase, ReceiptPhase::NamespaceDone);
    EXPECT_EQ(operations.trace,
              (std::vector<std::string>{"inspect", "namespace-record", "later-flush"}));
}

TEST(SaveTransaction, NamespaceReceiptRefusesForeignIdentityAndInvalidState) {
    for (const auto invalid : {0, 1, 2, 3}) {
        SCOPED_TRACE(invalid);
        ReceiptOperations operations;
        auto receipt = operations.durable;
        if (invalid == 0)
            operations.occupant[1] = 23;
        else if (invalid == 1)
            receipt.identity = {};
        else if (invalid == 2)
            receipt.phase = static_cast<ReceiptPhase>(255);
        else
            operations.unknown_namespace = true;
        EXPECT_EQ(detail::CompleteNamespaceRetirement(receipt, operations), ReceiptOutcome::Failed);
        EXPECT_TRUE(operations.present);
        EXPECT_TRUE(operations.retained);
        EXPECT_EQ(operations.durable.phase, ReceiptPhase::Prepared);
        EXPECT_EQ(operations.trace.size(), invalid == 0 || invalid == 3 ? 1u : 0u);
    }
}

TEST(SaveTransaction, HistoricalV4JournalStillDecodesAndRecovers) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    const auto baseline = Tree(save.path());
    {
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
        HookGuard hook(+[](const char* step, const fs::path&) {
            if (std::string_view(step) == "before-rollback")
                throw std::runtime_error("interrupted before rollback");
        });
        transaction.abort(nullptr);
    }
    auto records = JournalRecords(save.path());
    ASSERT_FALSE(records.empty());
    // Historical writers emitted entries only, without v5 receipt phases.
    std::erase_if(records, [](const auto& record) { return record.at("record") != "entry"; });
    // Pin the historical schema independently of the current writer constant.
    for (auto& record : records) {
        record["schema"] = "luminumbra.journal.v4";
        record.erase("record");
    }
    Put(journal_directory(save.path()) / "txn.json", EncodeJournalRecords(records));
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(Tree(save.path()), baseline);
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
}

TEST(SaveTransaction, TemporaryRootsAreProcessScopedAndDistinct) {
    const TempSave first, second;
#if defined(_WIN32)
    const auto process = std::to_string(::GetCurrentProcessId());
#else
    const auto process = std::to_string(static_cast<long long>(::getpid()));
#endif
    const auto prefix = fs::path("lum_snaptxn_" + process + "_").native();
    EXPECT_TRUE(first.path().filename().native().starts_with(prefix));
    EXPECT_TRUE(second.path().filename().native().starts_with(prefix));
    EXPECT_NE(first.path(), second.path());
    EXPECT_TRUE(fs::is_directory(first.path() / "chunks/region"));
    EXPECT_TRUE(fs::is_directory(second.path() / "chunks/region"));
}

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

TEST(SaveTransaction, AbsoluteTransactionProtectsEquivalentRelativeWriterPaths) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    const auto before = Tree(save.path());
    const auto relative = fs::relative(save.path(), fs::current_path());
    ASSERT_TRUE(relative.is_relative());
    SaveTransaction transaction(fs::absolute(save.path()), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json", "aether_state.efs"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(relative, "world_info.json", "first"));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(relative, "world_info.json", "second"));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(relative, "aether_state.efs", "created"));
    transaction.abort(nullptr);
    EXPECT_EQ(Tree(save.path()), before);
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, FinishedTransactionReleasesTheActiveSlotForTheNextWriter) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    for (const bool commit : {true, false}) {
        SCOPED_TRACE(commit);
        SaveTransaction first(save.path(), 2, {}, &RawDigest);
        // Constructed while the first transaction owns the thread's slot.
        SaveTransaction early(save.path(), 3, {}, &RawDigest);
        ASSERT_TRUE(first.begin(nullptr));
        std::vector<std::string> errors;
        EXPECT_FALSE(early.begin(&errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(SaveTransaction::active_for(save.path()), &first);
        ASSERT_TRUE(first.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "first"));
        if (commit) {
            ASSERT_TRUE(first.commit(nullptr));
        } else {
            first.abort(nullptr);
        }
        EXPECT_EQ(SaveTransaction::active_for(save.path()), nullptr);
        // The finished transaction is still alive; a new writer must own the slot itself.
        SaveTransaction next(save.path(), 3, {}, &RawDigest);
        ASSERT_TRUE(next.begin(nullptr));
        EXPECT_EQ(SaveTransaction::active_for(save.path()), &next);
        ASSERT_TRUE(next.protect({}, nullptr));
        const auto before = Tree(save.path());
        EXPECT_FALSE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
        EXPECT_EQ(Tree(save.path()), before);
        next.abort(nullptr);
        EXPECT_EQ(SaveTransaction::active_for(save.path()), nullptr);
        // A transaction refused earlier may begin once the slot is free.
        ASSERT_TRUE(early.begin(nullptr));
        EXPECT_EQ(SaveTransaction::active_for(save.path()), &early);
        early.abort(nullptr);
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, UnprotectedMemberAndEscapingPublicationRefuseWithoutMutation) {
    TempSave save, outside;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({}, nullptr));
    const auto before = Tree(save.path());
    EXPECT_FALSE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
    std::vector<std::string> errors;
    EXPECT_FALSE(
        SaveTransaction::prepare_publication(outside.path() / "world_info.json", {}, &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(Tree(save.path()), before);
}

TEST(SaveTransaction, CollidingJournalStripeAllowsMemberUpdateAndRemoval) {
    for (const bool commit : {false, true}) {
        SCOPED_TRACE(commit);
        TempSave save;
        const std::string member = "chunks/region/r.25.8.lmr";
        const auto path = save.path() / member;
        const auto journal = journal_directory(save.path()) / "txn.json";
        ASSERT_EQ(detail::file_publication_stripe_for_testing(path), 236u);
        ASSERT_EQ(detail::file_publication_stripe_for_testing(path),
                  detail::file_publication_stripe_for_testing(journal));
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{member, "previous"}}));
        const auto baseline = Tree(save.path());
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        std::vector<std::string> errors;
        ASSERT_TRUE(transaction.begin(&errors));
        ASSERT_TRUE(transaction.protect({member}, &errors));
        // Both operations read txn.json while holding the member's exclusive
        // stripe. The reader must borrow ownership without unlocking that gate.
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), member, "updated", &errors));
        EXPECT_EQ(Get(path), "updated");
        ASSERT_TRUE(WorldSaveService::remove_member(save.path(), member, &errors));
        EXPECT_FALSE(fs::exists(path));
        if (commit) {
            ASSERT_TRUE(transaction.commit(&errors));
            SnapshotIndex index;
            ASSERT_TRUE(read_snapshot_index(save.path(), index, &errors));
            const auto* removed = FindMember(index, member);
            ASSERT_NE(removed, nullptr);
            EXPECT_FALSE(removed->present);
        } else {
            transaction.abort(&errors);
            EXPECT_EQ(Tree(save.path()), baseline);
        }
        EXPECT_TRUE(errors.empty());
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, ForwardJournalWorkScalesWithAppendedRecords) {
    constexpr std::size_t kMembers = 24;
    for (const bool existing : {false, true}) {
        for (const bool commit : {false, true}) {
            SCOPED_TRACE(existing);
            SCOPED_TRACE(commit);
            TempSave save;
            std::vector<std::string> members;
            for (std::size_t index = 0; index < kMembers; ++index) {
                members.push_back("chunks/region/r." + std::to_string(index) + ".0.lmr");
                if (existing)
                    Put(save.path() / members.back(), "previous");
            }
            const auto baseline = Tree(save.path());
            SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
            std::vector<std::string> errors;
            ASSERT_TRUE(transaction.begin(&errors));
            ASSERT_TRUE(transaction.protect(members, &errors));
            // One header read, then one parse per appended member/index record.
            EXPECT_EQ(transaction.journal_work(), (std::array<std::size_t, 2>{kMembers + 2, 0}));
            for (const auto& member : members) {
                ASSERT_TRUE(
                    WorldSaveService::write_member_bytes(save.path(), member, "incoming", &errors));
            }
            EXPECT_EQ(transaction.journal_work(),
                      (std::array<std::size_t, 2>{2 * kMembers + 2, existing ? kMembers : 0}));
            if (commit) {
                ASSERT_TRUE(transaction.commit(&errors));
            } else {
                transaction.abort(&errors);
                EXPECT_EQ(Tree(save.path()), baseline);
            }
            EXPECT_TRUE(errors.empty());
            EXPECT_FALSE(journal_pending(save.path()));
        }
    }
}

TEST(SaveTransaction, LiveJournalReloadRefusesChangedControlWithoutPublishing) {
    for (const std::string_view fault : {"empty",
                                         "partial header",
                                         "malformed",
                                         "unknown metadata",
                                         "transaction",
                                         "base generation",
                                         "new generation",
                                         "journal identity",
                                         "undo identity",
                                         "cleanup",
                                         "oversized"}) {
        SCOPED_TRACE(fault);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        const auto control = journal_directory(save.path()) / "txn.json";
        const auto original = Get(control);
        auto records = JournalRecords(save.path());
        if (fault == "empty") {
            Put(control, "");
        } else if (fault == "partial header") {
            Put(control, original.substr(0, original.find('\n')));
        } else if (fault == "malformed") {
            Put(control, original + "{broken}\n");
        } else if (fault == "oversized") {
            Put(control, original + std::string(kMaxSnapshotIndexBytes, ' '));
        } else {
            if (fault == "cleanup") {
                auto cleanup = records.front();
                cleanup["record"] = "cleanup";
                cleanup["root_identity"] = std::array<std::uint64_t, 3>{1, 2, 3};
                cleanup["control_identity"] = std::array<std::uint64_t, 3>{1, 2, 3};
                records.push_back(cleanup);
            } else {
                for (auto& record : records) {
                    if (fault == "unknown metadata") {
                        record["unknown"] = true;
                    } else if (fault == "transaction") {
                        record["txn_id"] = 9u; // Same encoded size still invalidates the cache.
                    } else if (fault == "base generation") {
                        record["base_generation"] = 5u;
                        record["new_generation"] = 6u;
                    } else if (fault == "new generation") {
                        record["new_generation"] = 999u;
                    } else if (fault == "journal identity") {
                        record["journal_identity"] = std::array<std::uint64_t, 3>{1, 2, 3};
                    } else if (fault == "undo identity") {
                        record["undo_directory_identity"] = std::array<std::uint64_t, 3>{1, 2, 3};
                    }
                }
            }
            Put(control, EncodeJournalRecords(records));
        }
        const auto evidence = Tree(save.path());
        std::vector<std::string> errors;
        EXPECT_FALSE(
            SaveTransaction::prepare_publication(save.path() / "world_info.json", {}, &errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(Tree(save.path()), evidence);
        Put(control, original);
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "next"));
        transaction.abort(nullptr);
        EXPECT_EQ(Get(save.path() / "world_info.json"), "old");
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, LiveJournalReloadAcceptsCompletePrefixAndDiscardsUnpublishedTail) {
    for (const bool partial : {false, true}) {
        SCOPED_TRACE(partial);
        TempSave save;
        SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({}, nullptr));
        EXPECT_EQ(transaction.journal_work(), (std::array<std::size_t, 2>{2, 0}));
        const auto control = journal_directory(save.path()) / "txn.json";
        const auto original = Get(control);
        const auto records = JournalRecords(save.path());
        Put(control, original + (partial ? "{unpublished" : records.back().dump() + "\n"));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        EXPECT_GT(transaction.journal_work()[0], 3u);
        EXPECT_EQ(transaction.journal_work()[1], 1u);
        EXPECT_EQ(Get(control).find("{unpublished"), std::string::npos);
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "next"));
        transaction.abort(nullptr);
        EXPECT_FALSE(fs::exists(save.path() / "world_info.json"));
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, LiveJournalCacheRechecksControlAndDirectoryIdentities) {
    for (const bool directory : {false, true}) {
        SCOPED_TRACE(directory);
        TempSave save, held;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        const auto original = journal_directory(save.path()) / (directory ? "undo" : "txn.json");
        const auto retained = held.path() / "retained";
        fs::rename(original, retained);
        for (const bool occupied : {false, true}) {
            SCOPED_TRACE(occupied);
            if (occupied) {
                if (directory) {
                    ASSERT_TRUE(fs::create_directory(original));
                } else {
                    Put(original, Get(retained));
                }
            }
            const auto evidence = Tree(save.path());
            const auto completed =
                SaveTransaction::complete_owned_removal(save.path() / "world_info.json", nullptr);
            ASSERT_TRUE(completed.has_value());
            if (completed.has_value()) {
                EXPECT_FALSE(*completed);
            }
            EXPECT_EQ(Tree(save.path()), evidence);
            EXPECT_EQ(Get(save.path() / "world_info.json"), "old");
            if (occupied) {
                ASSERT_TRUE(fs::remove(original));
            }
        }
        fs::rename(retained, original);
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "next"));
        transaction.abort(nullptr);
        EXPECT_EQ(Get(save.path() / "world_info.json"), "old");
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, LiveJournalCacheDoesNotAdoptChangesAfterAppendFlush) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    {
        const HookGuard hook(+[](const char* step, const fs::path& path) {
            if (std::string_view(step) == "crash-record-published") {
                Put(path, Get(path) + "{invalid}\n");
            }
        });
        EXPECT_FALSE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "next"));
    }
    EXPECT_EQ(Get(save.path() / "world_info.json"), "old");
    const auto control = journal_directory(save.path()) / "txn.json";
    auto bytes = Get(control);
    const auto invalid = bytes.find("{invalid}\n");
    ASSERT_NE(invalid, std::string::npos);
    bytes.resize(invalid);
    Put(control, bytes);
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "next"));
    transaction.abort(nullptr);
    EXPECT_EQ(Get(save.path() / "world_info.json"), "old");
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, JournalAppendRequiresUnchangedVersionBeforeMutatingPrefix) {
    for (const std::string_view fault :
         {"same length", "matching stamp", "partial tail", "empty", "published hook"}) {
        SCOPED_TRACE(fault);
        TempSave save;
        const auto control = save.path() / "control";
        Put(control, "first\n");
        std::string cached = "first\n";
        WorldSaveService::ArtifactIdentity identity{};
        std::array<std::uint64_t, 8> version{};
        bool missing = false;
        ASSERT_TRUE(WorldSaveService::artifact_identity(control, identity, missing));
        ASSERT_FALSE(missing);
        ASSERT_TRUE(WorldSaveService::artifact_version(control, version, missing));
        ASSERT_FALSE(missing);
        if (fault == "published hook") {
            const HookGuard hook(+[](const char* step, const fs::path& path) {
                if (std::string_view(step) == "crash-record-published") {
                    Put(path, Get(path) + "foreign\n");
                }
            });
            ASSERT_TRUE(WorldSaveService::append_journal_record(
                control, "next\n", identity, nullptr, 0, nullptr, &version, &cached));
            cached += "next\n";
        } else {
            Put(control,
                fault == "empty"          ? ""
                : fault == "partial tail" ? "first\n{tail"
                                          : "other\n");
            if (fault == "matching stamp") {
                // Force metadata equality without changing timestamps: bytes
                // must independently refuse the same-size immediate rewrite.
                ASSERT_TRUE(WorldSaveService::artifact_version(control, version, missing));
                ASSERT_FALSE(missing);
            }
        }
        const auto evidence = Tree(save.path());
        EXPECT_FALSE(WorldSaveService::append_journal_record(
            control, "next\n", identity, nullptr, 0, nullptr, &version, &cached));
        EXPECT_EQ(version, (std::array<std::uint64_t, 8>{}));
        EXPECT_EQ(Tree(save.path()), evidence);
        Put(control, "first\n");
        cached = "first\n";
        // Unknown stamps use the existing append path and return a new stamp.
        ASSERT_TRUE(WorldSaveService::append_journal_record(
            control, "next\n", identity, nullptr, 0, nullptr, &version, &cached));
        cached += "next\n";
        EXPECT_NE(version, (std::array<std::uint64_t, 8>{}));
        EXPECT_EQ(Get(control), "first\nnext\n");
        ASSERT_TRUE(WorldSaveService::artifact_version(control, version, missing));
        ASSERT_FALSE(missing);
        ASSERT_TRUE(WorldSaveService::append_journal_record(
            control, "last\n", identity, nullptr, 0, nullptr, &version, &cached));
        cached += "last\n";
        EXPECT_EQ(Get(control), "first\nnext\nlast\n");
        const auto completed = Tree(save.path());
        EXPECT_FALSE(WorldSaveService::append_journal_record(
            control, "partial", identity, nullptr, 0, nullptr, &version, &cached));
        EXPECT_EQ(version, (std::array<std::uint64_t, 8>{}));
        EXPECT_EQ(Tree(save.path()), completed);
    }
}

TEST(SaveTransaction, JournalAppendChecksEveryCachedByteBeforeTruncationOrWrite) {
    for (const std::string_view fault : {"first block",
                                         "middle block",
                                         "last block",
                                         "short read",
                                         "empty cache",
                                         "oversized cache",
                                         "missing cache",
                                         "unknown stamp",
                                         "partial tail"}) {
        SCOPED_TRACE(fault);
        TempSave save;
        const auto control = save.path() / "control";
        std::string cached(2u * 65536u + 17u, 'a');
        cached.back() = '\n';
        Put(control, cached);
        WorldSaveService::ArtifactIdentity identity{};
        bool missing = false;
        ASSERT_TRUE(WorldSaveService::artifact_identity(control, identity, missing));
        ASSERT_FALSE(missing);
        auto changed = cached;
        if (fault == "empty cache") {
            cached.clear();
        } else if (fault == "oversized cache") {
            cached.resize(kMaxSnapshotIndexBytes + 1u, 'a');
        } else if (fault == "short read") {
            changed.resize(65536u);
        } else if (fault == "partial tail") {
            changed += "{unpublished";
        } else {
            changed[fault == "middle block" ? 65536u
                    : fault == "last block" ? changed.size() - 2u
                                            : 0u] = 'b';
        }
        Put(control, changed);
        std::array<std::uint64_t, 8> version{};
        ASSERT_TRUE(WorldSaveService::artifact_version(control, version, missing));
        ASSERT_FALSE(missing);
        if (fault == "unknown stamp") {
            version = {};
        }
        const auto evidence = Tree(save.path());
        EXPECT_FALSE(
            WorldSaveService::append_journal_record(control,
                                                    "next\n",
                                                    identity,
                                                    nullptr,
                                                    0,
                                                    nullptr,
                                                    &version,
                                                    fault == "missing cache" ? nullptr : &cached));
        EXPECT_EQ(version, (std::array<std::uint64_t, 8>{}));
        EXPECT_EQ(Tree(save.path()), evidence);
    }
}

TEST(SaveTransaction, JournalAppendCachedBytesHandleBoundariesAndUnavailableControl) {
    for (const std::string_view state : {"matching",
                                         "unknown stamp",
                                         "no stamp",
                                         "partial tail",
                                         "empty",
                                         "unpublished",
                                         "missing",
                                         "directory"}) {
        SCOPED_TRACE(state);
        TempSave save, held;
        const auto control = save.path() / "control";
        // Exercise more than two 64 KiB reads, including the final short block.
        std::string cached(2u * 65536u + 17u, 'a');
        cached.back() = '\n';
        if (state == "empty") {
            cached.clear();
        } else if (state == "unpublished") {
            cached.back() = 'a';
        } else if (state == "partial tail") {
            cached += "{unpublished";
        }
        Put(control, cached);
        WorldSaveService::ArtifactIdentity identity{};
        std::array<std::uint64_t, 8> version{};
        bool missing = false;
        ASSERT_TRUE(WorldSaveService::artifact_identity(control, identity, missing));
        ASSERT_FALSE(missing);
        ASSERT_TRUE(WorldSaveService::artifact_version(control, version, missing));
        ASSERT_FALSE(missing);
        if (state == "missing" || state == "directory") {
            fs::rename(control, held.path() / "control");
            if (state == "directory") {
                ASSERT_TRUE(fs::create_directory(control));
            }
        } else if (state == "unknown stamp") {
            version = {};
        }
        const auto evidence = Tree(save.path());
        const bool accepted = state == "matching" || state == "unknown stamp" ||
                              state == "no stamp" || state == "partial tail";
        EXPECT_EQ(WorldSaveService::append_journal_record(control,
                                                          "next\n",
                                                          identity,
                                                          nullptr,
                                                          0,
                                                          nullptr,
                                                          state == "no stamp" ? nullptr : &version,
                                                          &cached),
                  accepted);
        if (accepted) {
            cached.resize(cached.rfind('\n') + 1u);
            EXPECT_EQ(Get(control), cached + "next\n");
            if (state != "no stamp") {
                EXPECT_NE(version, (std::array<std::uint64_t, 8>{}));
            }
        } else {
            EXPECT_EQ(version, (std::array<std::uint64_t, 8>{}));
            EXPECT_EQ(Tree(save.path()), evidence);
        }
    }
}

TEST(SaveTransaction, ForwardRemovalRefusesForeignMemberSlotsBeforeMutation) {
    for (const std::string_view slot :
         {"undo", "incoming", "retired", "restored", "rollback-retired"}) {
        SCOPED_TRACE(slot);
        TempSave save, held;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        const auto member = save.path() / "world_info.json";
        ASSERT_TRUE(SaveTransaction::prepare_publication(member, {}, nullptr));
        const auto record = JournalRecords(save.path()).back().at("entries").at(0);
        ASSERT_EQ(record.at("path"), "world_info.json");
        const auto target =
            slot == "undo" ? journal_directory(save.path()) / record.at("undo").get<std::string>()
                           : journal_directory(save.path()) / "undo" /
                                 (std::string(slot) + ".world_info.json");
        if (slot == "undo") {
            fs::rename(target, held.path() / "undo");
        }
        Put(target, "foreign");
        const auto evidence = Tree(save.path());
        const auto completed = SaveTransaction::complete_owned_removal(member, nullptr);
        ASSERT_TRUE(completed.has_value());
        if (completed.has_value()) {
            EXPECT_FALSE(*completed);
        }
        EXPECT_EQ(Tree(save.path()), evidence);
        EXPECT_EQ(Get(member), "old");
        ASSERT_TRUE(fs::remove(target));
        if (slot == "undo") {
            fs::rename(held.path() / "undo", target);
        }
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "next"));
        transaction.abort(nullptr);
        EXPECT_EQ(Get(member), "old");
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, LiveJournalCacheRefusesMissingProtectedEntry) {
    TempSave save;
    SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    const auto control = journal_directory(save.path()) / "txn.json";
    const auto original = Get(control);
    auto records = JournalRecords(save.path());
    std::erase_if(records, [](const auto& record) {
        return !record.at("entries").empty() &&
               record.at("entries").at(0).at("path") == "world_info.json";
    });
    Put(control, EncodeJournalRecords(records));
    const auto evidence = Tree(save.path());
    const auto completed =
        SaveTransaction::complete_owned_removal(save.path() / "world_info.json", nullptr);
    ASSERT_TRUE(completed.has_value());
    if (completed.has_value()) {
        EXPECT_FALSE(*completed);
    }
    EXPECT_EQ(Tree(save.path()), evidence);
    Put(control, original);
    transaction.abort(nullptr);
    EXPECT_FALSE(journal_pending(save.path()));
}

#if defined(__linux__)
TEST(SaveTransaction, ManyMembersKeepDescriptorsBoundedThroughPublicationAndRollback) {
    constexpr std::size_t kMembers = 1200;
    for (const bool existing : {false, true}) {
        for (const bool commit : {false, true}) {
            SCOPED_TRACE(existing);
            SCOPED_TRACE(commit);
            TempSave save;
            std::vector<std::string> members;
            for (std::size_t index = 0; index < kMembers; ++index) {
                members.push_back("chunks/region/r." + std::to_string(index) + ".0.lmr");
                if (existing)
                    Put(save.path() / members.back(), "previous");
            }
            const auto baseline = Tree(save.path());
            const auto descriptors = OpenDescriptorCount();
            ASSERT_NE(descriptors, std::numeric_limits<std::size_t>::max());
            DescriptorPeak() = descriptors;
            const HookGuard hook(&ObserveTransactionDescriptors);
            {
                SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
                std::vector<std::string> errors;
                ASSERT_TRUE(transaction.begin(&errors));
                ASSERT_TRUE(transaction.protect(members, &errors));
                EXPECT_LE(OpenDescriptorCount(), descriptors + 24);
                for (const auto& member : members) {
                    ASSERT_TRUE(WorldSaveService::write_member_bytes(
                        save.path(), member, "incoming", &errors));
                }
                EXPECT_LE(OpenDescriptorCount(), descriptors + 24);
                if (commit) {
                    ASSERT_TRUE(transaction.commit(&errors));
                } else {
                    transaction.abort(&errors);
                    EXPECT_EQ(Tree(save.path()), baseline);
                }
                EXPECT_TRUE(errors.empty());
                EXPECT_FALSE(journal_pending(save.path()));
                EXPECT_LE(DescriptorPeak(), descriptors + 24);
            }
            EXPECT_EQ(OpenDescriptorCount(), descriptors);
        }
    }
}

TEST(SaveTransactionDeathTest, RollbackReleasesDescriptorReserveUnderExhaustion) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "previous"}}));
    const auto baseline = Tree(save.path());
    EXPECT_EXIT(RollbackWithExhaustedDescriptors(save.path()), ::testing::ExitedWithCode(0), "");
    EXPECT_EQ(Tree(save.path()), baseline);
    EXPECT_FALSE(journal_pending(save.path()));
}
#endif

TEST(SaveTransaction, RetryAfterFailedReplacementRetainsInstalledVersion) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "previous"}}));
    const auto baseline = Tree(save.path());
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "first"));
    detail::set_file_publication_hook_for_testing(
        +[](const fs::path& path, bool) { return path.filename() != "world_info.json"; });
    EXPECT_FALSE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "second"));
    detail::set_file_publication_hook_for_testing(nullptr);
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "third"));
    EXPECT_EQ(Get(save.path() / "world_info.json"), "third");
    std::vector<std::string> errors;
    transaction.abort(&errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(Tree(save.path()), baseline);
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, RollbackRechecksVersionsAfterPreflight) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "previous"}}));
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json", "aether_state.efs"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "incoming"));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "aether_state.efs", "created"));
    RollbackMutation() = {save.path() / "world_info.json", false};
    const HookGuard hook(&ChangeLaterRollbackOccupant);
    std::vector<std::string> errors;
    transaction.abort(&errors);
    EXPECT_TRUE(RollbackMutation().changed);
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(Get(RollbackMutation().target), "changed after rollback preflight");
    EXPECT_TRUE(journal_pending(save.path()));
}

TEST(SaveTransaction, AbortRefusesForeignCreatedAndReplacedOccupants) {
    for (const bool replacing : {false, true}) {
        SCOPED_TRACE(replacing);
        TempSave save, displaced;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        const std::string relative = replacing ? "world_info.json" : "aether_state.efs";
        const auto member = save.path() / relative;
        {
            SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(transaction.protect({relative}, nullptr));
            ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), relative, "ours"));
            fs::rename(member, displaced.path() / "owned");
            Put(member, "foreign occupant");
            std::vector<std::string> errors;
            transaction.abort(&errors);
            EXPECT_FALSE(errors.empty());
            EXPECT_EQ(Get(member), "foreign occupant");
            EXPECT_TRUE(journal_pending(save.path()));
        }
        fs::rename(member, displaced.path() / "foreign");
        fs::rename(displaced.path() / "owned", member);
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
        EXPECT_EQ(Tree(save.path()), baseline);
        EXPECT_EQ(Get(displaced.path() / "foreign"), "foreign occupant");
    }
}

TEST(SaveTransaction, AbortRefusesChangedPreviousOccupantBytes) {
    for (const auto* changed : {"bad", "changed previous occupant"}) {
        SCOPED_TRACE(changed);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        const auto member = save.path() / "world_info.json";
        {
            SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
            // Truncate the same object: both its native identity and its name remain unchanged.
            Put(member, changed);
            std::vector<std::string> errors;
            transaction.abort(&errors);
            EXPECT_FALSE(errors.empty());
            EXPECT_EQ(Get(member), changed);
            EXPECT_TRUE(journal_pending(save.path()));
        }
        Put(member, "old");
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
        EXPECT_EQ(Tree(save.path()), baseline);
    }
}

TEST(SaveTransaction, AbortRefusesSubstitutedJournalAndRecoveryRefusesUnknownDebris) {
    TempSave save, displaced;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    const auto baseline = Tree(save.path());
    {
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "ours"));
        fs::rename(journal_directory(save.path()), displaced.path() / "owned-journal");
        Put(journal_directory(save.path()) / "foreign", "retain");
        transaction.abort(nullptr);
        EXPECT_EQ(Get(journal_directory(save.path()) / "foreign"), "retain");
        EXPECT_EQ(Get(save.path() / "world_info.json"), "ours");
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_EQ(Get(journal_directory(save.path()) / "foreign"), "retain");
    }
    fs::rename(journal_directory(save.path()), displaced.path() / "foreign-journal");
    fs::rename(displaced.path() / "owned-journal", journal_directory(save.path()));
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(Tree(save.path()), baseline);
    EXPECT_EQ(Get(displaced.path() / "foreign-journal/foreign"), "retain");
}

TEST(SaveTransaction, PreviousCommitUsesJournalRollbackEvenWithArtifactRegistry) {
    for (const bool replacing : {false, true}) {
        for (const bool foreign : {false, true}) {
            SCOPED_TRACE(replacing);
            SCOPED_TRACE(foreign);
            TempSave save, displaced;
            ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
            const auto baseline = Tree(save.path());
            const std::string relative = replacing ? "world_info.json" : "aether_state.efs";
            const auto member = save.path() / relative;
            CreatedWorldArtifacts artifacts;
            {
                SaveTransaction transaction(save.path(), 2, {}, &RawDigest, &artifacts);
                ASSERT_TRUE(transaction.begin(nullptr));
                ASSERT_TRUE(transaction.protect({relative}, nullptr));
                ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), relative, "ours"));
                if (foreign) {
                    fs::rename(member, displaced.path() / "owned");
                    Put(member, "foreign occupant");
                }
                std::vector<std::string> errors;
                transaction.abort(&errors);
                EXPECT_EQ(errors.empty(), !foreign);
                EXPECT_EQ(journal_pending(save.path()), foreign);
                if (foreign) {
                    EXPECT_EQ(Get(member), "foreign occupant");
                }
            }
            if (foreign) {
                {
                    SaveTransaction admission(save.path(), 3, {}, &RawDigest);
                    std::vector<std::string> errors;
                    EXPECT_FALSE(admission.begin(&errors));
                    EXPECT_FALSE(errors.empty());
                    EXPECT_TRUE(journal_pending(save.path()));
                    EXPECT_EQ(Get(member), "foreign occupant");
                }
                fs::rename(member, displaced.path() / "foreign");
                fs::rename(displaced.path() / "owned", member);
                EXPECT_EQ(recover_pending_journal(save.path(), nullptr),
                          RecoveryOutcome::RolledBack);
            }
            EXPECT_EQ(Tree(save.path()), baseline);
            EXPECT_FALSE(journal_pending(save.path()));
        }
    }
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

TEST(SaveTransaction, RecoveryRestoresRetiredIndexBeforeExclusivePublication) {
    for (const bool empty : {false, true}) {
        SCOPED_TRACE(empty);
        TempSave save;
        const std::map<std::string, std::string> members =
            empty ? std::map<std::string, std::string>{}
                  : std::map<std::string, std::string>{{"world_info.json", "old"}};
        ASSERT_TRUE(CommitOnce(save.path(), 1, members));
        const auto baseline = Tree(save.path());
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({}, nullptr));
        // Represent interruption between retiring the bound index and publishing its successor.
        ASSERT_TRUE(fs::remove(snapshot_index_path(save.path())));
        ASSERT_TRUE(journal_pending(save.path()));
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
        EXPECT_EQ(Tree(save.path()), baseline);
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
    }
}

TEST(SaveTransaction, RetiredIndexRecoveryPreservesMissingOrCorruptUndoEvidence) {
    for (const int invalid : {0, 1, 2, 3}) {
        SCOPED_TRACE(invalid);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({}, nullptr));
        ASSERT_TRUE(fs::remove(snapshot_index_path(save.path())));
        const auto undo = journal_directory(save.path()) / "undo/0";
        if (invalid == 0) {
            ASSERT_TRUE(fs::remove(undo));
        } else if (invalid == 1) {
            // Preserve the inode so decoding, rather than ownership, rejects the partial data.
            Put(undo, "partial undo");
        } else if (invalid == 2) {
            ASSERT_TRUE(fs::remove(undo));
            ASSERT_TRUE(fs::create_directory(undo));
            Put(undo / "occupant", "preserve");
        } else {
            fs::resize_file(undo, kMaxSnapshotIndexBytes + 1);
        }
        const auto evidence = Tree(save.path());
        std::vector<std::string> errors;
        EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
        EXPECT_FALSE(errors.empty());
        EXPECT_TRUE(journal_pending(save.path()));
        EXPECT_EQ(Tree(save.path()), evidence);
    }
}

TEST(SaveTransaction, RecoveryRefusesMismatchedBaseGenerationOrTransaction) {
    for (const bool retired_index : {false, true}) {
        for (const bool generation : {false, true}) {
            SCOPED_TRACE(retired_index);
            SCOPED_TRACE(generation);
            TempSave save;
            ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
            SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(transaction.protect({}, nullptr));
            if (retired_index) {
                ASSERT_TRUE(fs::remove(snapshot_index_path(save.path())));
            }
            auto records = JournalRecords(save.path());
            ASSERT_FALSE(records.empty());
            for (auto& record : records) {
                if (generation) {
                    record["base_generation"] = 2;
                    record["new_generation"] = 3;
                } else {
                    record["txn_id"] = 3;
                }
            }
            Put(journal_directory(save.path()) / "txn.json", EncodeJournalRecords(records));
            const auto evidence = Tree(save.path());
            std::vector<std::string> errors;
            EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
            EXPECT_FALSE(errors.empty());
            EXPECT_TRUE(journal_pending(save.path()));
            EXPECT_EQ(Tree(save.path()), evidence);
        }
    }
}

TEST(SaveTransaction, RecoveryRefusesNoninitialTransactionWithoutCommittedIndex) {
    TempSave save;
    SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    auto records = JournalRecords(save.path());
    ASSERT_FALSE(records.empty());
    for (auto& record : records)
        record["txn_id"] = 2;
    Put(journal_directory(save.path()) / "txn.json", EncodeJournalRecords(records));
    const auto evidence = Tree(save.path());
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_FALSE(errors.empty());
    EXPECT_TRUE(journal_pending(save.path()));
    EXPECT_EQ(Tree(save.path()), evidence);
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

TEST(SaveTransaction, ProtectClosesSourceBeforePublishingCollidingUndoFile) {
    TempSave save;
    const std::map<std::string, std::string> baseline{
        {"world_info.json", "metadata"},
        {"aether_state.efs", "energy"},
        {"chunks/region/world-manifest.json", "manifest"},
        {"chunks/region/plant-entities.json", "plants"},
        {"chunks/region/r.-2.-2.lmr", "region"}};
    ASSERT_TRUE(CommitOnce(save.path(), 1, baseline));
    const auto before = Tree(save.path());
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    // The existing index takes undo 0. These four members take 1 through 4,
    // so r.-2.-2.lmr and its undo file 5 both use publication stripe 53.
    ASSERT_TRUE(transaction.protect({"world_info.json",
                                     "aether_state.efs",
                                     "chunks/region/world-manifest.json",
                                     "chunks/region/plant-entities.json",
                                     "chunks/region/r.-2.-2.lmr"},
                                    nullptr));
    EXPECT_EQ(Get(journal_directory(save.path()) / "undo/5"), "region");
    ASSERT_TRUE(
        WorldSaveService::write_member_bytes(save.path(), "chunks/region/r.-2.-2.lmr", "changed"));
    transaction.abort(nullptr);
    EXPECT_EQ(Tree(save.path()), before);
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, InterruptedPublicationsKeepTemporaryFilesInsideTheJournal) {
    for (const std::string_view member :
         {"world_info.json", "chunks/region/r.-2.-2.lmr", "chunks/region/snapshot.json"}) {
        SCOPED_TRACE(member);
        TempSave save;
        ASSERT_TRUE(
            CommitOnce(save.path(),
                       1,
                       {{"world_info.json", "metadata"}, {"chunks/region/r.-2.-2.lmr", "region"}}));
        const auto before = Tree(save.path());
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        if (member != kSnapshotIndexRelativePath) {
            ASSERT_TRUE(transaction.protect({std::string(member)}, nullptr));
        } else {
            ASSERT_TRUE(transaction.protect({}, nullptr));
        }
        set_snapshot_step_hook_for_testing(+[](const char* step, const fs::path&) {
            if (std::string_view(step) == "before-replace")
                throw std::runtime_error("publication interrupted after durable staging");
        });
        if (member == kSnapshotIndexRelativePath) {
            EXPECT_FALSE(transaction.commit(nullptr));
        } else {
            EXPECT_THROW(WorldSaveService::write_member_bytes(save.path(), member, "changed"),
                         std::runtime_error);
        }
        set_snapshot_step_hook_for_testing(nullptr);
        transaction.abort(nullptr);
        EXPECT_EQ(Tree(save.path()), before);
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
    }
}

namespace {
struct CreationRollbackSchedule {
    unsigned cut = 0;
    unsigned observed = 0;
};
CreationRollbackSchedule& CreationRollbackCuts() {
    static CreationRollbackSchedule schedule;
    return schedule;
}
void InterruptCreationRollback(const char* step, const fs::path& path) {
    if (std::string_view(step) == "journal-unlinked") {
        EXPECT_FALSE(fs::exists(path.parent_path().parent_path() / "world_info.json"));
    }
    auto& schedule = CreationRollbackCuts();
    if (++schedule.observed == schedule.cut)
        throw std::runtime_error("interrupted creation rollback");
}
} // namespace

TEST(SaveTransaction, CreationRollbackCompletesFilesBeforeJournalAndRetriesEveryInterruption) {
    bool finished = false;
    // One tiny snapshot member; the existing disk schedule remains capped at 256.
    for (unsigned cut = 1; cut <= 256 && !finished; ++cut) {
        SCOPED_TRACE(cut);
        TempSave parent;
        const auto saves = parent.path() / "worlds/saves";
        fs::create_directories(saves);
        const auto root = saves / "created";
        CreatedWorldArtifacts artifacts;
        artifacts.create_root(root);
        artifacts.create_directory(root / "chunks");
        artifacts.create_directory(root / "chunks/region");
        artifacts.write_file(root / "world_info.json",
                             R"({"container_version":3,"snapshotFormat":1})");
        SaveTransaction transaction(root, 0, {}, &RawDigest, &artifacts);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        CreationRollbackCuts() = {.cut = cut, .observed = 0};
        std::vector<std::string> rollback_errors;
        {
            const HookGuard hook(&InterruptCreationRollback);
            transaction.abort(&rollback_errors);
        }
        finished = CreationRollbackCuts().observed < cut;
        if (finished) {
            EXPECT_TRUE(rollback_errors.empty());
            EXPECT_FALSE(fs::exists(root));
        }
        // Catalog inspection may accept an empty directory as a clean miss.
        // The catalog must expose pending evidence without accepting payloads;
        // neither that listing nor a clean miss authorizes opening a world.
        const auto listed = InspectSavedWorld(parent.path(), "created");
        EXPECT_TRUE(listed.pending_recovery || !listed.error.empty());
        EXPECT_FALSE(listed.payloads_verified);
        EXPECT_FALSE(snapshot_index_exists(root));
        EXPECT_FALSE(WorldSaveService::has_world_save(root));
        WorldStreamingState state;
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService{}.load_world(state, root, errors));
        // Retained registry identities and recorded phases complete on retry;
        // empty creation directories never gain receipt inventory locations.
        transaction.abort(nullptr);
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_FALSE(fs::exists(root));
    }
    EXPECT_TRUE(finished);
}

TEST(SaveTransaction, CreationRollbackCompletesRecordedTemporaryOutsideRegistry) {
    TempSave parent;
    const auto root = parent.path() / "created";
    CreatedWorldArtifacts artifacts;
    artifacts.create_root(root);
    artifacts.write_file(root / "world_info.json", "uncommitted");
    SaveTransaction transaction(root, 0, {}, &RawDigest, &artifacts);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    const auto entry = LastEntryRecord(JournalRecords(root)).at("entries").at(0);
    // Interrupted undo publication leaves its identity-bound temporary in the
    // neutral root, outside the creation registry but inside the v5 inventory.
    const auto temporary = root / entry.at("undo_temporary").get<std::string>();
    fs::rename(journal_directory(root) / entry.at("undo").get<std::string>(), temporary);
    transaction.abort(nullptr);
    EXPECT_FALSE(journal_pending(root));
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(fs::exists(root));
}

TEST(SaveTransaction, CreationIndexPublicationRetainsIdentitiesForRollback) {
    for (const std::string_view failure :
         {"none", "before-replace", "after-replace", "occupied", "substituted"}) {
        SCOPED_TRACE(failure);
        TempSave parent;
        const auto root = parent.path() / "created";
        CreatedWorldArtifacts artifacts;
        artifacts.create_root(root);
        SaveTransaction transaction(root, 0, {}, &RawDigest, &artifacts);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({}, nullptr));
        // Set the existing publication hook only for the index's publication.
        if (failure == "before-replace") {
            set_snapshot_step_hook_for_testing(+[](const char* step, const fs::path& path) {
                if (path.filename() == "snapshot.json" &&
                    std::string_view(step) == "before-replace")
                    throw std::runtime_error("interrupted index publication");
            });
        } else if (failure == "after-replace") {
            set_snapshot_step_hook_for_testing(+[](const char* step, const fs::path& path) {
                if (path.filename() == "snapshot.json" && std::string_view(step) == "after-replace")
                    throw std::runtime_error("interrupted index publication");
            });
        } else if (failure == "occupied") {
            artifacts.create_directory(root / "chunks");
            artifacts.create_directory(root / "chunks/region");
            Put(snapshot_index_path(root), "unowned");
        } else if (failure == "substituted") {
            set_snapshot_step_hook_for_testing(+[](const char* step, const fs::path& path) {
                if (path.filename() == "snapshot.json" &&
                    std::string_view(step) == "after-replace") {
                    if (!fs::remove(path))
                        throw std::runtime_error("could not retire index publication");
                    Put(path, "unowned");
                    throw std::runtime_error("index publication substituted");
                }
            });
        }
        std::vector<std::string> errors;
        const bool committed = transaction.commit(&errors);
        set_snapshot_step_hook_for_testing(nullptr);
        EXPECT_EQ(committed, failure == "none");
        if (failure == "occupied" || failure == "substituted") {
            EXPECT_EQ(Get(snapshot_index_path(root)), "unowned");
            EXPECT_FALSE(artifacts.rollback().empty());
            EXPECT_TRUE(journal_pending(root));
            ASSERT_TRUE(fs::remove(snapshot_index_path(root)));
            transaction.abort(nullptr);
        } else if (committed) {
            EXPECT_TRUE(snapshot_index_exists(root));
        } else {
            EXPECT_FALSE(errors.empty());
        }
        EXPECT_FALSE(journal_pending(root));
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_FALSE(fs::exists(root));
    }
}

TEST(SaveTransaction, RecoveryRefusesUnknownAndSubstitutedPublicationSlots) {
    for (const bool unknown : {false, true}) {
        SCOPED_TRACE(unknown);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        const auto slot = journal_directory(save.path()) / "undo" /
                          (unknown ? "unrecognized" : "incoming.world_info.json");
        Put(slot, "foreign");
        const auto evidence = Tree(save.path());
        std::vector<std::string> errors;
        EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(Tree(save.path()), evidence);
        ASSERT_TRUE(fs::remove(slot));
        transaction.abort(nullptr);
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, RecoveryDiscardsStaleJournalWhenIndexAlreadyCommitted) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "v0"}}));

    TempSave side;
    JournalCapture().source = journal_directory(save.path());
    JournalCapture().target = side.path() / "stale_journal";
    fs::remove_all(JournalCapture().target);
    {
        HookGuard guard(&CaptureJournalHook);
        ASSERT_TRUE(CommitOnce(save.path(), 2, {{"world_info.json", "v1"}}));
    }
    ASSERT_TRUE(fs::exists(JournalCapture().target / "txn.json"))
        << "stale journal was not captured";
    ASSERT_FALSE(journal_pending(save.path()));
    const TreeMap post_commit = Tree(save.path());

    fs::rename(JournalCapture().target, journal_directory(save.path()));
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

TEST(SaveTransaction, RecoveryAcceptsWriterTemporaryNamesAtEveryStagingLocation) {
    for (const auto* member : {"world_info.json", "chunks/region/active-regions.arl"}) {
        for (const auto* location : {"", "journal/", "chunks/region/"}) {
            if (std::string_view(member) == "world_info.json" &&
                std::string_view(location) == "chunks/region/")
                continue; // Metadata has no region-directory staging location.
            for (const auto* suffix : {".tmp.1.2", ".tmp.1.2.0123456789abcdef0123456789abcdef"}) {
                const bool ledger = std::string_view(member) == "chunks/region/active-regions.arl";
                if (!ledger && std::string_view(suffix) != ".tmp.1.2")
                    continue;
                SCOPED_TRACE(std::string(member) + ":" + location + suffix);
                TempSave save;
                SaveTransaction writer(save.path(), 1, {}, &RawDigest);
                ASSERT_TRUE(writer.begin(nullptr));
                ASSERT_TRUE(writer.protect({member}, nullptr));
                auto records = JournalRecords(save.path());
                records.back()["entries"][0]["incoming_temporary"] =
                    std::string(location) + fs::path(member).filename().generic_string() + suffix;
                // An absent incoming file is a legitimate interruption before publication.
                Put(journal_directory(save.path()) / "txn.json", EncodeJournalRecords(records));
                std::vector<std::string> errors;
                EXPECT_EQ(recover_pending_journal(save.path(), &errors),
                          RecoveryOutcome::RolledBack);
                EXPECT_TRUE(errors.empty());
                EXPECT_FALSE(journal_pending(save.path()));
                EXPECT_FALSE(fs::exists(save.path() / member));
            }
        }
    }
}

TEST(SaveTransaction, RecoveryRejectsMalformedTemporaryNamesWithoutMutation) {
    const std::vector<std::string> invalid{
        "active-regions.arl.tmp.",
        "active-regions.arl.tmp.1",
        "active-regions.arl.tmp..2",
        "active-regions.arl.tmp.1.",
        "active-regions.arl.tmp.1.x",
        "active-regions.arl.tmp.1.2.",
        "active-regions.arl.tmp.1.2.0123456789abcdef0123456789abcde",
        "active-regions.arl.tmp.1.2.0123456789abcdef0123456789abcdef0",
        "active-regions.arl.tmp.1.2.0123456789abcdef0123456789abcdeg",
        "active-regions.arl.tmp.1.2.0123456789abcdef0123456789abcdeF",
        "active-regions.arl.tmp.1.2.0123456789abcdef0123456789abcdef.owner",
        "active-regions.arl.tmp.1.2.0123456789abcdef0123456789abcdef.claim",
        "../active-regions.arl.tmp.1.2",
        "journal/../active-regions.arl.tmp.1.2",
        "chunks/region/active-regions.arl.tmp.1.2/child",
        "chunks\\region\\active-regions.arl.tmp.1.2",
        "/active-regions.arl.tmp.1.2"};
    for (const auto& name : invalid) {
        SCOPED_TRACE(name);
        TempSave save;
        SaveTransaction writer(save.path(), 1, {}, &RawDigest);
        ASSERT_TRUE(writer.begin(nullptr));
        ASSERT_TRUE(writer.protect({"chunks/region/active-regions.arl"}, nullptr));
        auto records = JournalRecords(save.path());
        records.back()["entries"][0]["incoming_temporary"] = name;
        Put(journal_directory(save.path()) / "txn.json", EncodeJournalRecords(records));
        const auto evidence = Tree(save.path());
        std::vector<std::string> errors;
        EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
        ASSERT_FALSE(errors.empty());
        EXPECT_EQ(errors.front(), "corrupt snapshot journal: journal temporary name invalid");
        EXPECT_EQ(Tree(save.path()), evidence);
    }
    for (const auto* field : {"undo_temporary", "restored_temporary", "incoming_temporary"}) {
        TempSave save;
        Put(save.path() / "world_info.json", "preserve");
        SaveTransaction writer(save.path(), 1, {}, &RawDigest);
        ASSERT_TRUE(writer.begin(nullptr));
        ASSERT_TRUE(writer.protect({"world_info.json"}, nullptr));
        auto records = JournalRecords(save.path());
        const auto& entry = records.back()["entries"][0];
        const auto prefix = std::string_view(field) == "undo_temporary"
                                ? "journal/" + entry.at("undo").get<std::string>()
                                : "journal/world_info.json";
        records.back()["entries"][0][field] = prefix + ".tmp.1.2.0123456789abcdef0123456789abcdef";
        Put(journal_directory(save.path()) / "txn.json", EncodeJournalRecords(records));
        const auto evidence = Tree(save.path());
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_EQ(Tree(save.path()), evidence);
    }
}

TEST(SaveTransaction, ProtectedPublicationClosesNamedSourceAndPreservesForeignOccupants) {
    for (const auto* occupant : {"absent", "owned", "foreign"}) {
        SCOPED_TRACE(occupant);
        TempSave save;
        const auto destination = save.path() / "world_info.json";
        WorldSaveService::ArtifactIdentity expected{}, incoming{};
        bool missing = false;
        if (std::string_view(occupant) != "absent") {
            Put(destination, "previous");
            ASSERT_TRUE(WorldSaveService::artifact_identity(destination, expected, missing));
            ASSERT_FALSE(missing);
        }
        fs::path temporary;
        ASSERT_TRUE(WorldSaveService::prepare_restoration(
            save.path() / "restore", "incoming", temporary, incoming, nullptr));
        if (std::string_view(occupant) == "foreign") {
            // Retain the previous inode under another name so the replacement cannot reuse it.
            fs::rename(destination, save.path() / "previous");
            Put(destination, "foreign");
            EXPECT_FALSE(WorldSaveService::publish_restoration(
                temporary, destination, expected, incoming, nullptr));
            EXPECT_EQ(Get(destination), "foreign");
            EXPECT_EQ(Get(temporary), "incoming");
        } else {
            ASSERT_TRUE(WorldSaveService::publish_restoration(
                temporary, destination, expected, incoming, nullptr));
            EXPECT_EQ(Get(destination), "incoming");
            EXPECT_FALSE(fs::exists(temporary));
        }
    }
}

TEST(SaveTransaction, TornJournalTailsRestoreCompletedOwnershipAndAllowTheNextSave) {
    for (const std::size_t cut : {1u, 17u, 127u, 511u, 70000u}) {
        SCOPED_TRACE(cut);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        {
            SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(transaction.protect({"world_info.json", "aether_state.efs"}, nullptr));
            ASSERT_TRUE(
                WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
            ASSERT_TRUE(
                WorldSaveService::write_member_bytes(save.path(), "aether_state.efs", "created"));
            const auto control = journal_directory(save.path()) / "txn.json";
            const auto complete = Get(control);
            const auto record = LastEntryRecord(JournalRecords(save.path())).dump();
            const auto tail = cut > record.size() ? std::string(cut, '{') : record.substr(0, cut);
            Put(control, complete + tail);
            std::vector<std::string> errors;
            ASSERT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::RolledBack)
                << (errors.empty() ? "" : errors.front());
            EXPECT_EQ(Tree(save.path()), baseline);
            EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
            transaction.abort(nullptr);
        }
        ASSERT_TRUE(CommitOnce(save.path(), 3, {{"world_info.json", "next"}}));
        EXPECT_EQ(Get(save.path() / "world_info.json"), "next");
    }
}

TEST(SaveTransaction, RecoveryRetiresTheRecordedPublicationAliasAndRefusesForeignLinks) {
    for (const bool foreign : {false, true}) {
        SCOPED_TRACE(foreign);
        const TempSave save;
        const TempSave elsewhere;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        const auto member = save.path() / "world_info.json";
        {
            SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
            ASSERT_TRUE(
                WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
            // The publication record is the last one appended before the replacement.
            const auto records = JournalRecords(save.path());
            ASSERT_FALSE(records.empty());
            std::string incoming;
            for (const auto& entry : LastEntryRecord(records).at("entries")) {
                if (entry.at("path") == "world_info.json")
                    incoming = entry.at("incoming_temporary").get<std::string>();
            }
            ASSERT_FALSE(incoming.empty());
            const auto alias = save.path() / incoming;
            ASSERT_FALSE(fs::exists(alias));
            // An interruption between linking the destination and retiring the source name
            // leaves the recorded temporary as a second name of the installed member.
            fs::create_hard_link(member, alias);
            if (foreign) {
                const auto unknown = elsewhere.path() / "foreign-link";
                fs::create_hard_link(member, unknown);
                const auto evidence = Tree(save.path());
                std::vector<std::string> errors;
                EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
                EXPECT_FALSE(errors.empty());
                EXPECT_EQ(Tree(save.path()), evidence);
                EXPECT_EQ(fs::hard_link_count(member), 3u);
                ASSERT_TRUE(fs::remove(unknown));
            }
            std::vector<std::string> errors;
            ASSERT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::RolledBack)
                << (errors.empty() ? "" : errors.front());
            EXPECT_EQ(Tree(save.path()), baseline);
            EXPECT_FALSE(fs::exists(alias));
            EXPECT_EQ(fs::hard_link_count(member), 1u);
            EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
            transaction.abort(nullptr);
        }
        ASSERT_TRUE(CommitOnce(save.path(), 3, {{"world_info.json", "next"}}));
        EXPECT_EQ(Get(member), "next");
    }
}

TEST(SaveTransaction, RestorationIdentityBelongsToTheCreatedObjectBeforeClosing) {
    TempSave save;
    static fs::path original;
    static bool substituted = false;
    original = save.path() / "original";
    substituted = false;
    fs::path temporary;
    WorldSaveService::ArtifactIdentity prepared{};
    {
        HookGuard hook(+[](const char* step, const fs::path& path) {
            if (std::string_view(step) != "crash-staging-written")
                return;
            std::error_code error;
            fs::rename(path, original, error);
            if (!error) {
                Put(path, "foreign");
                substituted = true;
            }
        });
        ASSERT_TRUE(WorldSaveService::prepare_restoration(
            save.path() / "restore", "ours", temporary, prepared, nullptr));
    }
    WorldSaveService::ArtifactIdentity actual{};
    bool missing = false;
#if defined(_WIN32)
    EXPECT_FALSE(substituted); // The still-open creation handle denies delete sharing.
    ASSERT_TRUE(WorldSaveService::artifact_identity(temporary, actual, missing));
    EXPECT_FALSE(missing);
    EXPECT_EQ(prepared, actual);
    ASSERT_TRUE(WorldSaveService::publish_restoration(
        temporary, save.path() / "destination", {}, prepared, nullptr));
    EXPECT_EQ(Get(save.path() / "destination"), "ours");
#else
    ASSERT_TRUE(substituted);
    ASSERT_TRUE(WorldSaveService::artifact_identity(original, actual, missing));
    EXPECT_FALSE(missing);
    EXPECT_EQ(prepared, actual);
    EXPECT_FALSE(WorldSaveService::publish_restoration(
        temporary, save.path() / "destination", {}, prepared, nullptr));
    EXPECT_EQ(Get(temporary), "foreign");
    EXPECT_EQ(Get(original), "ours");
    EXPECT_FALSE(fs::exists(save.path() / "destination"));
#endif
}

TEST(SaveTransaction, CompleteMalformedJournalRecordsAndForeignArtifactsRefuseBeforeRollback) {
    for (const int fault : {0, 1, 2}) {
        SCOPED_TRACE(fault);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
        const auto control = journal_directory(save.path()) / "txn.json";
        const auto complete = Get(control);
        const auto foreign = journal_directory(save.path()) / "undo" /
                             (fault == 1 ? "foreign" : "incoming.world_info.json");
        if (fault == 0)
            Put(control, complete + "{partial}\n");
        else
            Put(foreign, "retain foreign bytes");
        const auto evidence = Tree(save.path());
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_EQ(Tree(save.path()), evidence);
        if (fault == 0) {
            Put(control, complete);
        } else {
            ASSERT_TRUE(fs::remove(foreign));
        }
        transaction.abort(nullptr);
        EXPECT_FALSE(journal_pending(save.path()));
        EXPECT_EQ(Get(save.path() / "world_info.json"), "old");
    }
}

TEST(SaveTransaction, RecoveryFailureAlwaysAppendsDiagnostic) {
    for (const int fault : {0, 1, 2}) {
        SCOPED_TRACE(fault);
        TempSave save;
        const auto journal = journal_directory(save.path());
        std::optional<SaveTransaction> transaction;
        if (fault == 2) {
            ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
            transaction.emplace(save.path(), 2, std::vector<std::array<float, 3>>{}, &RawDigest);
            ASSERT_TRUE(transaction->begin(nullptr));
            ASSERT_TRUE(transaction->protect({"world_info.json"}, nullptr));
            ASSERT_TRUE(
                WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
            auto records = JournalRecords(save.path());
            auto prior = LastEntryRecord(records);
            auto& entry = prior["entries"].at(0);
            const auto identity = entry.at("previous_identity");
            entry["rollback_retired_identity"] = identity;
            records.push_back(prior);
            // A prepared historical receipt is admitted, but cannot be reused
            // for another identity. That refusal has no lower-level diagnostic.
            auto receipt = records.front();
            receipt["record"] = "receipt";
            receipt["phase"] = 0u;
            receipt["receipts"] =
                nlohmann::json::array({{{"path", "journal/undo/rollback-retired.world_info.json"},
                                        {"generation", 0u},
                                        {"identity", identity},
                                        {"directory", false},
                                        {"absent_only", false}}});
            records.push_back(receipt);
            Put(journal / "txn.json", EncodeJournalRecords(records));
        } else {
            fs::create_directory(journal);
            if (fault == 1)
                Put(journal / "undo", "foreign file instead of directory");
            else
                Put(journal / "txn.json", "{partial}\n");
        }
        const auto evidence = Tree(save.path());
        for (const bool prior_error : {false, true}) {
            SCOPED_TRACE(prior_error);
            std::vector<std::string> errors;
            if (prior_error)
                errors.emplace_back("prior diagnostic");
            const auto count = errors.size();
            EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
            ASSERT_GT(errors.size(), count);
            EXPECT_FALSE(errors.back().empty());
            if (fault != 2) {
                EXPECT_EQ(Tree(save.path()), evidence);
            }
            EXPECT_TRUE(journal_pending(save.path()));
        }
    }
}

TEST(SaveTransaction, RecoveryPreflightsLiveVersionsBeforeChangingAnyMember) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
    // Keep the admitted native identity but change its version. Recovery must
    // refuse before appending restoration ownership or changing the index.
    Put(save.path() / "world_info.json", "foreign changed bytes");
    const auto evidence = Tree(save.path());
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(Tree(save.path()), evidence);
    EXPECT_EQ(Get(save.path() / "world_info.json"), "foreign changed bytes");
}

TEST(SaveTransaction, RecoveryVersionsRequireMatchingWriterAndProtectedPath) {
    TempSave save, elsewhere;
    Put(save.path() / "world_info.json", "old");
    SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    WorldSaveService::ArtifactIdentity control{};
    bool missing = false;
    ASSERT_TRUE(WorldSaveService::artifact_identity(
        journal_directory(save.path()) / "txn.json", control, missing));
    ASSERT_FALSE(missing);
    std::array<std::array<std::uint64_t, 8>, 3> versions{};
    ASSERT_TRUE(SaveTransaction::recovery_versions(
        save.path() / "world_info.json", control, versions, nullptr));
    EXPECT_EQ(versions[1], (std::array<std::uint64_t, 8>{}));
    EXPECT_EQ(versions[2], (std::array<std::uint64_t, 8>{}));
    EXPECT_EQ(versions[0][0], control[0]);
    EXPECT_NE(versions[0], (std::array<std::uint64_t, 8>{}));
    const auto retained = versions;
    const auto evidence = Tree(save.path());
    for (const auto& invalid :
         {fs::path{}, elsewhere.path() / "world_info.json", save.path() / "aether_state.efs"}) {
        SCOPED_TRACE(invalid);
        std::vector<std::string> errors;
        EXPECT_FALSE(SaveTransaction::recovery_versions(invalid, control, versions, &errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(versions, retained);
        EXPECT_EQ(Tree(save.path()), evidence);
    }
    auto other_control = control;
    other_control[0] ^= 1u;
    EXPECT_TRUE(SaveTransaction::recovery_versions({}, other_control, versions, nullptr));
    EXPECT_EQ(versions, retained);
    transaction.abort(nullptr);
    // Without the matching live writer, replay keeps its recorded version state.
    EXPECT_TRUE(SaveTransaction::recovery_versions({}, control, versions, nullptr));
    EXPECT_EQ(versions, retained);
}

TEST(SaveTransaction, TornStaleJournalTailKeepsTheCommittedGeneration) {
    TempSave save, side;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    JournalCapture().source = journal_directory(save.path());
    JournalCapture().target = side.path() / "retained";
    {
        HookGuard hook(&CaptureJournalHook);
        ASSERT_TRUE(CommitOnce(save.path(), 2, {{"world_info.json", "committed"}}));
    }
    const auto committed = Tree(save.path());
    fs::rename(JournalCapture().target, journal_directory(save.path()));
    const auto control = journal_directory(save.path()) / "txn.json";
    Put(control, Get(control) + "{unpublished");
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::DiscardedStale);
    EXPECT_EQ(Tree(save.path()), committed);
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
}

TEST(SaveTransaction, JournalAppendRefusesInvalidRecordsAndForeignControlWithoutMutation) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    const auto control = journal_directory(save.path()) / "txn.json";
    WorldSaveService::ArtifactIdentity identity{};
    bool missing = false;
    ASSERT_TRUE(WorldSaveService::artifact_identity(control, identity, missing));
    ASSERT_FALSE(missing);
    for (const auto& invalid : {std::string{},
                                std::string("unterminated"),
                                std::string(kMaxSnapshotIndexBytes, 'x') + "\n"}) {
        const auto before = Tree(save.path());
        EXPECT_FALSE(WorldSaveService::append_journal_record(control, invalid, identity, nullptr));
        EXPECT_EQ(Tree(save.path()), before);
    }
    auto foreign = identity;
    ++foreign[0];
    const auto before = Tree(save.path());
    EXPECT_FALSE(WorldSaveService::append_journal_record(control, "{}\n", foreign, nullptr));
    EXPECT_EQ(Tree(save.path()), before);
    for (const auto reserve : {kMaxSnapshotIndexBytes, kMaxSnapshotIndexBytes + 1}) {
        EXPECT_FALSE(
            WorldSaveService::append_journal_record(control, "{}\n", identity, nullptr, reserve));
        EXPECT_EQ(Tree(save.path()), before);
    }
    for (const std::string_view state : {"empty", "partial", "directory", "missing"}) {
        SCOPED_TRACE(state);
        const auto retained = save.path() / "retained-control";
        fs::rename(control, retained);
        auto candidate = identity;
        if (state == "empty" || state == "partial") {
            Put(control, state == "empty" ? "" : "{unpublished");
            ASSERT_TRUE(WorldSaveService::artifact_identity(control, candidate, missing));
            ASSERT_FALSE(missing);
        } else if (state == "directory") {
            ASSERT_TRUE(fs::create_directory(control));
        }
        const auto evidence = Tree(save.path());
        EXPECT_FALSE(WorldSaveService::append_journal_record(control, "{}\n", candidate, nullptr));
        EXPECT_EQ(Tree(save.path()), evidence);
        EXPECT_EQ(fs::exists(control), state != "missing");
        EXPECT_EQ(fs::is_directory(control), state == "directory");
        if (state != "missing") {
            ASSERT_TRUE(fs::remove(control));
        }
        fs::rename(retained, control);
    }
}

TEST(SaveTransaction, FullForwardJournalRetainsRollbackCapacity) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    const auto baseline = Tree(save.path());
    {
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
        const auto control = journal_directory(save.path()) / "txn.json";
        WorldSaveService::ArtifactIdentity identity{};
        bool missing = false;
        ASSERT_TRUE(WorldSaveService::artifact_identity(control, identity, missing));
        ASSERT_FALSE(missing);
        // A valid whitespace-padded duplicate reaches the byte boundary without
        // thousands of synchronous writes. Subsequent ordinary writes consume it.
        const auto encoded = LastEntryRecord(JournalRecords(save.path())).dump() + "\n";
        // v5 reserves completion records as well as restoration records.
        const auto headroom = transaction.rollback_reserve_bytes();
        const auto padding = kMaxSnapshotIndexBytes - headroom - Get(control).size();
        ASSERT_GT(padding, encoded.size());
        const auto record = std::string(padding - encoded.size(), ' ') + encoded;
        ASSERT_TRUE(
            WorldSaveService::append_journal_record(control, record, identity, nullptr, headroom));
        bool refused = false;
        for (int attempt = 0; attempt < 32 && !refused; ++attempt)
            refused = !WorldSaveService::write_member_bytes(
                save.path(), "world_info.json", "payload " + std::to_string(attempt));
        ASSERT_TRUE(refused);
        EXPECT_LE(Get(control).size(), kMaxSnapshotIndexBytes);
        const auto neutral = UnregisteredStaging(save.path());
        std::vector<std::string> errors;
        ASSERT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::RolledBack)
            << (errors.empty() ? "" : errors.front());
        EXPECT_EQ(RecoveredCommittedTree(save.path(), neutral), baseline);
        transaction.abort(nullptr);
    }
    ASSERT_TRUE(CommitOnce(save.path(), 3, {{"world_info.json", "next"}}));
}

TEST(SaveTransaction, RecoveryTracksReusedInstalledIdentityAcrossJournalRecords) {
    for (const std::string_view occupant : {"installed", "foreign", "historical"}) {
        SCOPED_TRACE(occupant);
        TempSave save, displaced;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        const auto member = save.path() / "world_info.json";
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "first"));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "second"));
        // Preparing a removal records the installed occupant and clears incoming. Repeat
        // that record to exercise replay after the incoming identity has been superseded.
        ASSERT_TRUE(SaveTransaction::prepare_publication(member, {}, nullptr));
        ASSERT_TRUE(SaveTransaction::prepare_publication(member, {}, nullptr));
        const auto installed = LastEntryRecord(JournalRecords(save.path())).at("entries").at(0);
        const auto original_identity = installed.at("previous_identity");
        const auto reused_identity = installed.at("installed_identity");
        if (occupant != "installed") {
            fs::rename(member, displaced.path() / "retained");
            if (occupant == "historical") {
                ASSERT_TRUE(
                    WorldSaveService::write_member_bytes(save.path(), "world_info.json", "third"));
                fs::rename(member, displaced.path() / "latest");
                fs::rename(displaced.path() / "retained", member);
            }
            Put(member, "foreign occupant");
        }
        // Model original-identity reuse by the second publication independently of the
        // filesystem's allocator. All incoming identities still name actual writer output.
        auto records = JournalRecords(save.path());
        for (auto& record : records) {
            for (auto& entry : record["entries"]) {
                if (entry.at("path") == "world_info.json") {
                    entry["previous_identity"] = reused_identity;
                    if (entry.at("installed_identity") == original_identity)
                        entry["installed_identity"] = reused_identity;
                }
            }
        }
        // The synthetic identity substitution covers every bound use, including
        // historical immutable receipts emitted by Windows forward retirement.
        for (auto& record : records) {
            if (record.at("record") == "receipt" || record.at("record") == "receipt-reset") {
                for (auto& receipt : record["receipts"]) {
                    if (receipt.at("identity") == original_identity)
                        receipt["identity"] = reused_identity;
                }
            }
        }
        Put(journal_directory(save.path()) / "txn.json", EncodeJournalRecords(records));
        const auto evidence = Tree(save.path());
        std::vector<std::string> errors;
        if (occupant == "installed") {
            ASSERT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::RolledBack)
                << (errors.empty() ? "" : errors.front());
            EXPECT_TRUE(errors.empty());
            EXPECT_EQ(Tree(save.path()), baseline);
            EXPECT_FALSE(journal_pending(save.path()));
        } else {
            EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
            EXPECT_FALSE(errors.empty());
            EXPECT_EQ(Tree(save.path()), evidence);
            EXPECT_EQ(Get(member), "foreign occupant");
            EXPECT_TRUE(journal_pending(save.path()));
        }
        transaction.abort(nullptr);
    }
}

TEST(SaveTransaction, RepeatedRecoveryReusesPublishedRetirementAndRestoration) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    const auto baseline = Tree(save.path());
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
    std::string published;
    for (int interruption = 0; interruption < 8; ++interruption) {
        SCOPED_TRACE(interruption);
        {
            HookGuard hook(+[](const char* step, const fs::path&) {
                if (std::string_view(step) == "before-replace")
                    throw std::runtime_error("restoration interrupted");
            });
            EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        }
        const auto bytes = Get(journal_directory(save.path()) / "txn.json");
        if (interruption == 0) {
            published = bytes;
        } else {
            EXPECT_EQ(bytes, published);
        }
    }
    ASSERT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(Tree(save.path()), baseline);
}

TEST(SaveTransaction, RecoveryCanResumeAfterTailTruncationBodyFlushAndDelimiterPublication) {
    for (const std::string_view event :
         {"crash-record-tail-truncated", "crash-record-body-flushed", "crash-record-published"}) {
        SCOPED_TRACE(event);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
        const auto control = journal_directory(save.path()) / "txn.json";
        Put(control, Get(control) + "{unpublished");
        {
            static std::string_view selected;
            selected = event;
            HookGuard hook(+[](const char* step, const fs::path&) {
                if (step == selected)
                    throw std::runtime_error("journal recovery interrupted");
            });
            EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        }
        ASSERT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
        EXPECT_EQ(Tree(save.path()), baseline);
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
    }
}

TEST(SaveTransaction, AbsentCreateRetryCompletesBeforePayloadRollback) {
    TempSave save;
    SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "payload"));
    ASSERT_TRUE(
        WorldSaveService::durable_write_file(snapshot_index_path(save.path()), "new index"));
    std::string retained;
    for (int attempt = 0; attempt < 3; ++attempt) {
        HookGuard hook(+[](const char* step, const fs::path& path) {
            if (std::string_view(step) == "crash-before-namespace-completion" &&
                path.filename() == "snapshot.json")
                throw std::runtime_error("index absence completion interrupted");
        });
        if (attempt == 0) {
            transaction.abort(nullptr);
        } else {
            EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        }
        EXPECT_FALSE(fs::exists(snapshot_index_path(save.path())));
        EXPECT_EQ(Get(save.path() / "world_info.json"), "payload");
        EXPECT_TRUE(fs::exists(journal_directory(save.path()) / "txn.json"));
        const auto control = Get(journal_directory(save.path()) / "txn.json");
        if (attempt == 0) {
            retained = control;
        } else {
            EXPECT_EQ(control, retained);
        }
    }
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
    EXPECT_FALSE(fs::exists(save.path() / "world_info.json"));
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
}

TEST(SaveTransaction, AbortFlushFailureRetainsRecoveryEvidence) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    const auto baseline = Tree(save.path());
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
    {
        HookGuard hook(+[](const char* step, const fs::path&) {
            if (std::string_view(step) == "crash-before-namespace-completion")
                throw std::runtime_error("completion flush interrupted");
        });
        transaction.abort(nullptr);
    }
    EXPECT_TRUE(fs::exists(journal_directory(save.path()) / "txn.json"));
    EXPECT_FALSE(fs::is_empty(journal_directory(save.path()) / "undo"));
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(Tree(save.path()), baseline);
}

TEST(SaveTransaction, MatchingRestoreFailureBlocksCleanup) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    const auto baseline = Tree(save.path());
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
    const auto evidence = Tree(save.path());
    {
        HookGuard hook(+[](const char* step, const fs::path& path) {
            if (std::string_view(step) == "crash-before-restoration-completion" &&
                path.filename() == "snapshot.json")
                throw std::runtime_error("matching index completion interrupted");
        });
        transaction.abort(nullptr);
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
    }
    EXPECT_EQ(Tree(save.path()), evidence);
    EXPECT_EQ(Get(save.path() / "world_info.json"), "new");
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(Tree(save.path()), baseline);
}

TEST(SaveTransaction, ReceiptDiscoverySurvivesControlAndDirectoryRetirement) {
    for (const std::string_view interruption : {"crash-receipt-carrier-published",
                                                "journal-unlinked",
                                                "crash-before-receipt-retirement"}) {
        SCOPED_TRACE(interruption);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
        {
            ReceiptInterruption() = interruption;
            HookGuard hook(+[](const char* step, const fs::path&) {
                if (step == ReceiptInterruption())
                    throw std::runtime_error("receipt retirement interrupted");
            });
            transaction.abort(nullptr);
        }
        EXPECT_FALSE(fs::exists(journal_directory(save.path()) / "txn.json"));
        EXPECT_TRUE(!FindReceiptCarrier(save.path()).empty());
        EXPECT_TRUE(journal_pending(save.path()));
        EXPECT_NE(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_EQ(Tree(save.path()), baseline);
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
    }
}

TEST(SaveTransaction, AbortRetriesRecordedCleanupWithoutRepeatingRollback) {
    for (const bool protect_member : {false, true}) {
        for (const std::string_view interruption : {"crash-receipt-carrier-published",
                                                    "journal-unlinked",
                                                    "crash-before-receipt-retirement"}) {
            SCOPED_TRACE(protect_member);
            SCOPED_TRACE(interruption);
            TempSave save;
            ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
            const auto baseline = Tree(save.path());
            SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(transaction.protect(protect_member
                                                ? std::vector<std::string>{"world_info.json"}
                                                : std::vector<std::string>{},
                                            nullptr));
            if (protect_member) {
                ASSERT_TRUE(
                    WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
            }
            {
                ReceiptInterruption() = interruption;
                HookGuard hook(+[](const char* step, const fs::path&) {
                    if (step == ReceiptInterruption())
                        throw std::runtime_error("cleanup interrupted");
                });
                transaction.abort(nullptr);
            }
            EXPECT_TRUE(journal_pending(save.path()));
            std::vector<std::string> errors;
            transaction.abort(&errors);
            EXPECT_TRUE(errors.empty());
            EXPECT_EQ(Tree(save.path()), baseline);
            EXPECT_FALSE(journal_pending(save.path()));
            transaction.abort(nullptr);
            EXPECT_EQ(Tree(save.path()), baseline);
        }
    }
}

TEST(SaveTransaction, AbortCleanupRetryRefusesPartialAndSubstitutedEvidence) {
    for (const bool substitute : {false, true}) {
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
        {
            HookGuard hook(+[](const char* step, const fs::path&) {
                if (std::string_view(step) == "crash-receipt-carrier-published")
                    throw std::runtime_error("cleanup interrupted");
            });
            transaction.abort(nullptr);
        }
        const auto carrier = FindReceiptCarrier(save.path());
        ASSERT_FALSE(carrier.empty());
        const auto recorded = Get(carrier);
        const auto retained = save.path() / "retained-receipts";
        if (substitute)
            fs::rename(carrier, retained);
        Put(carrier, "{partial");
        const auto evidence = Tree(save.path());
        std::vector<std::string> errors;
        transaction.abort(&errors);
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(Tree(save.path()), evidence);
        if (substitute) {
            fs::remove(carrier);
            fs::rename(retained, carrier);
        } else {
            Put(carrier, recorded);
        }
        transaction.abort(nullptr);
        EXPECT_EQ(Tree(save.path()), baseline);
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, RecordedFileReceiptsRemainAuthoritativeAcrossCompletionPhases) {
    for (unsigned phase = 0; phase <= 3; ++phase) {
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
        const auto header = JournalRecords(save.path()).front();
        {
            HookGuard hook(+[](const char* step, const fs::path&) {
                if (std::string_view(step) == "crash-receipt-carrier-published")
                    throw std::runtime_error("cleanup interrupted");
            });
            transaction.abort(nullptr);
        }
        const auto carrier = FindReceiptCarrier(save.path());
        ASSERT_FALSE(carrier.empty());
        WorldSaveService::ArtifactIdentity control{}, undo_identity{};
        bool missing = false;
        const auto undo = journal_directory(save.path()) / "undo/0";
        ASSERT_TRUE(WorldSaveService::artifact_identity(carrier, control, missing));
        ASSERT_FALSE(missing);
        ASSERT_TRUE(WorldSaveService::artifact_identity(undo, undo_identity, missing));
        ASSERT_FALSE(missing);
        auto receipt = header;
        receipt["record"] = "receipt";
        receipt["receipts"] = nlohmann::json::array({{{"path", "journal/undo/0"},
                                                      {"identity", undo_identity},
                                                      {"directory", false},
                                                      {"absent_only", false}}});
        // POSIX must honour persisted file receipts too; Windows preparation
        // already recorded the same phase before carrier publication.
        for (unsigned recorded = 0; recorded <= phase; ++recorded) {
            receipt["phase"] = recorded;
            ASSERT_TRUE(WorldSaveService::append_journal_record(
                carrier, receipt.dump() + "\n", control, nullptr));
        }
        const auto retained = save.path() / "retained-undo";
        fs::rename(undo, retained);
        Put(undo, "foreign undo");
        const auto evidence = Tree(save.path());
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_EQ(Tree(save.path()), evidence);
        fs::remove(undo);
        fs::rename(retained, undo);
        EXPECT_NE(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_EQ(Tree(save.path()), baseline);
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, CompletedReceiptsRefuseSubstitutedDirectoriesAndUnknownContents) {
    for (const bool substitute_directory : {false, true}) {
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
        {
            HookGuard hook(+[](const char* step, const fs::path&) {
                if (std::string_view(step) == "crash-receipt-carrier-published")
                    throw std::runtime_error("cleanup interrupted");
            });
            transaction.abort(nullptr);
        }
        const auto journal = journal_directory(save.path());
        const auto retained = save.path() / "retained-journal";
        if (substitute_directory) {
            fs::rename(journal, retained);
            fs::create_directory(journal);
        } else {
            Put(journal / "undo/unknown", "foreign");
        }
        const auto evidence = Tree(save.path());
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_EQ(Tree(save.path()), evidence);
        if (substitute_directory) {
            fs::remove(journal);
            fs::rename(retained, journal);
        } else {
            fs::remove(journal / "undo/unknown");
        }
        EXPECT_NE(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
    }
}

TEST(SaveTransaction, ReceiptDiscoveryCompletesOnlyRecordedControlAliases) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    const auto baseline = Tree(save.path());
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
    {
        HookGuard hook(+[](const char* step, const fs::path&) {
            if (std::string_view(step) == "crash-receipt-carrier-published")
                throw std::runtime_error("carrier publication interrupted");
        });
        transaction.abort(nullptr);
    }
    const auto carrier = FindReceiptCarrier(save.path());
    ASSERT_FALSE(carrier.empty());
    // Model both link-publication interruption and namespace resurrection on
    // every platform; no symlink privilege or native crash harness is required.
    fs::create_hard_link(carrier, journal_directory(save.path()) / "txn.json");
    const auto foreign = save.path() / "foreign-control-alias";
    fs::create_hard_link(carrier, foreign);
    const auto evidence = Tree(save.path());
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
    EXPECT_EQ(Tree(save.path()), evidence);
    fs::remove(foreign);
    fs::create_hard_link(carrier, journal_directory(save.path()) / "undo/cleanup.txn.json");
    EXPECT_NE(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
    EXPECT_EQ(Tree(save.path()), baseline);
    EXPECT_FALSE(journal_pending(save.path()));
}

namespace {
struct ReceiptBatchCapture {
    unsigned stop_phase = 4;
    std::size_t event = 0;
    std::size_t removal = 0;
    std::size_t flush = 0;
    struct Published {
        nlohmann::json record;
        std::size_t removal;
        std::size_t flush;
    };
    std::vector<Published> published;
};

ReceiptBatchCapture& BatchCapture() {
    static ReceiptBatchCapture capture;
    return capture;
}

void CaptureReceiptBatch(const char* step, const fs::path& path) {
    auto& capture = BatchCapture();
    ++capture.event;
    const std::string_view event(step);
    if (event == "crash-artifact-removed")
        capture.removal = capture.event;
    if (event == "crash-record-body-flushed")
        capture.flush = capture.event;
    if (event != "crash-record-published" ||
        !path.filename().string().starts_with("journal.receipts."))
        return;
    std::istringstream input(Get(path));
    nlohmann::json record;
    std::string line;
    while (std::getline(input, line))
        record = nlohmann::json::parse(line);
    if (record.at("record") != "receipt")
        return;
    capture.published.push_back({record, capture.removal, capture.flush});
    const auto& receipts = record.at("receipts");
    if (record.at("phase") == capture.stop_phase && receipts.size() == 2 &&
        receipts.at(0).at("directory") == true && receipts.at(1).at("directory") == true)
        throw std::runtime_error("directory batch interrupted");
}
} // namespace

TEST(SaveTransaction, TornReceiptBatchAdvancesNoReceipt) {
    for (const bool complete_body : {false, true}) {
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        BatchCapture() = {.stop_phase = 2, .published = {}};
        {
            HookGuard hook(&CaptureReceiptBatch);
            transaction.abort(nullptr);
        }
        const auto carrier = FindReceiptCarrier(save.path());
        ASSERT_FALSE(carrier.empty());
        ASSERT_FALSE(BatchCapture().published.empty());
        auto batch = BatchCapture().published.back().record;
        ASSERT_EQ(batch.at("phase"), 2u);
        ASSERT_EQ(batch.at("receipts").size(), 2u);
        batch["phase"] = 3u;
        const auto body = batch.dump();
        // A full JSON body without its delimiter is unpublished too. The partial
        // body includes the first receipt, which must not advance independently.
        const auto cut = complete_body ? body.size() : body.find("},{") + 1;
        ASSERT_GT(cut, 0u);
        ASSERT_LE(cut, body.size());
        Put(carrier, Get(carrier) + body.substr(0, cut));
        BatchCapture() = {.stop_phase = 3, .published = {}};
        {
            HookGuard hook(&CaptureReceiptBatch);
            EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        }
        ASSERT_EQ(BatchCapture().published.size(), 1u);
        EXPECT_EQ(BatchCapture().published.front().record.at("phase"), 3u);
        EXPECT_EQ(BatchCapture().published.front().record.at("receipts"), batch.at("receipts"));
        EXPECT_TRUE(journal_pending(save.path()));
        EXPECT_NE(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_EQ(Tree(save.path()), baseline);
    }
}

TEST(SaveTransaction, ReceiptPhaseBatchesFollowLaterCarrierFlush) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(),
                           1,
                           {{"world_info.json", "old metadata"},
                            {"aether_state.efs", "old energy"},
                            {"chunks/region/world-manifest.json", "old manifest"}}));
    BatchCapture() = {};
    {
        HookGuard hook(&CaptureReceiptBatch);
        ASSERT_TRUE(RunInventoryScenario(save.path()));
    }
    std::array<unsigned, 4> counts{};
    for (const auto& published : BatchCapture().published) {
        const auto phase = published.record.at("phase").get<unsigned>();
        ASSERT_LT(phase, counts.size());
        ++counts[phase];
        EXPECT_GT(published.flush, published.removal);
        const auto& receipts = published.record.at("receipts");
        EXPECT_GE(receipts.size(), 2u);
        if (phase == 3) {
            EXPECT_GT(published.removal, 0u);
        }
    }
    // Preparation is appended before carrier relocation; the capture starts at
    // that relocation. Windows completes files then directories; POSIX only dirs.
#if defined(_WIN32)
    EXPECT_EQ(counts, (std::array<unsigned, 4>{0, 2, 2, 2}));
#else
    EXPECT_EQ(counts, (std::array<unsigned, 4>{0, 1, 1, 1}));
#endif
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, ReceiptDiscoveryRefusesMalformedOrForeignEvidence) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
    {
        HookGuard hook(+[](const char* step, const fs::path&) {
            if (std::string_view(step) == "crash-receipt-carrier-published")
                throw std::runtime_error("carrier publication interrupted");
        });
        transaction.abort(nullptr);
    }
    const auto carrier = FindReceiptCarrier(save.path());
    ASSERT_FALSE(carrier.empty());
    const auto valid = Get(carrier);
    std::istringstream lines(valid);
    std::vector<nlohmann::json> original;
    std::string line;
    while (std::getline(lines, line))
        original.push_back(nlohmann::json::parse(line));
    ASSERT_EQ(original.back().at("record"), "receipt");
    // One tiny retained transaction exercises all decode refusals without repeating
    // successful synchronous save/cleanup work for each malformed input.
    for (const std::string_view mutation : {"unknown phase",
                                            "phase order",
                                            "phase regression",
                                            "mixed phase regression",
                                            "duplicate path",
                                            "empty batch",
                                            "nonarray batch",
                                            "scalar receipt",
                                            "invalid receipt flags",
                                            "unknown receipt field",
                                            "invalid reset",
                                            "negative phase",
                                            "escaped name",
                                            "foreign identity",
                                            "zero identity",
                                            "unknown metadata",
                                            "unknown version",
                                            "mixed versions",
                                            "unknown kind",
                                            "unknown inventory",
                                            "unknown entry field",
                                            "foreign control",
                                            "partial"}) {
        SCOPED_TRACE(mutation);
        auto records = original;
        if (mutation == "unknown phase")
            records.back()["phase"] = 4u;
        else if (mutation == "phase order")
            records.back()["phase"] = 2u;
        else if (mutation == "phase regression") {
            auto advanced = records.back();
            advanced["phase"] = 1u;
            records.push_back(advanced);
            advanced["phase"] = 0u;
            records.push_back(advanced);
        } else if (mutation == "mixed phase regression") {
            auto advanced = records.back();
            advanced["receipts"] = nlohmann::json::array({advanced.at("receipts").back()});
            advanced["phase"] = 1u;
            records.push_back(advanced);
            advanced["phase"] = 2u;
            records.push_back(advanced);
            advanced = original.back();
            advanced["phase"] = 1u;
            records.push_back(advanced);
        } else if (mutation == "duplicate path") {
            records.back()["receipts"].push_back(records.back()["receipts"].at(0));
        } else if (mutation == "empty batch") {
            records.back()["receipts"] = nlohmann::json::array();
        } else if (mutation == "nonarray batch") {
            records.back()["receipts"] = true;
        } else if (mutation == "scalar receipt") {
            records.back()["receipts"].at(0) = 7u;
        } else if (mutation == "invalid receipt flags") {
            records.back()["receipts"].at(0)["directory"] = true;
            records.back()["receipts"].at(0)["absent_only"] = true;
        } else if (mutation == "unknown receipt field") {
            records.back()["receipts"].at(0)["unknown"] = true;
        } else if (mutation == "invalid reset") {
            records.back()["record"] = "receipt-reset";
        } else if (mutation == "negative phase")
            records.back()["phase"] = -1;
        else if (mutation == "escaped name")
            records.back()["receipts"].at(0)["path"] = "../world_info.json";
        else if (mutation == "foreign identity")
            records.back()["receipts"].at(0)["identity"] = std::array<std::uint64_t, 3>{1, 2, 3};
        else if (mutation == "zero identity")
            records.back()["receipts"].at(0)["identity"] = std::array<std::uint64_t, 3>{};
        else if (mutation == "unknown metadata")
            records.back()["unknown"] = true;
        else if (mutation == "unknown version")
            records.front()["schema"] = "luminumbra.journal.v6";
        else if (mutation == "mixed versions")
            records.back()["schema"] = "luminumbra.journal.v4";
        else if (mutation == "unknown kind")
            records.back()["record"] = "unknown";
        else if (mutation == "unknown inventory")
            records.back()["receipts"].at(0)["path"] = "journal/undo/unrecorded";
        else if (mutation == "unknown entry field") {
            ASSERT_FALSE(records.at(1).at("entries").empty());
            records.at(1).at("entries").at(0)["unknown"] = true;
        } else if (mutation == "foreign control") {
            for (auto& record : records) {
                if (record.at("record") == "cleanup") {
                    record["control_identity"] = std::array<std::uint64_t, 3>{1, 2, 3};
                }
            }
        }
        Put(carrier,
            mutation == "partial" ? valid.substr(0, valid.find('\n'))
                                  : EncodeJournalRecords(records));
        const auto evidence = Tree(save.path());
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_EQ(Tree(save.path()), evidence);
    }
    Put(carrier, valid);
    EXPECT_NE(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
}

TEST(SaveTransaction, ReceiptSlotReuseExplicitlyResetsOnlyCompletedOwnedAbsence) {
    for (const std::string_view phase : {"prepared",
                                         "namespace",
                                         "flush",
                                         "retired",
                                         "foreign",
                                         "namespace matching",
                                         "flush matching",
                                         "retired matching",
                                         "prepared matching"}) {
        SCOPED_TRACE(phase);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        WorldSaveService::ArtifactIdentity original{}, installed{};
        bool missing = false;
        ASSERT_TRUE(WorldSaveService::artifact_identity(
            save.path() / "world_info.json", original, missing));
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "first"));
        ASSERT_TRUE(WorldSaveService::artifact_identity(
            save.path() / "world_info.json", installed, missing));
        auto records = JournalRecords(save.path());
        // Model retained completion evidence on every CI platform. Remove native
        // forward receipt records before installing this bounded synthetic prefix.
        std::erase_if(records, [](const auto& record) {
            return record.at("record") == "receipt" || record.at("record") == "receipt-reset";
        });
        auto receipt = records.front();
        receipt["record"] = "receipt";
        receipt["phase"] = 0u;
        receipt["receipts"] = nlohmann::json::array(
            {{{"path", "journal/undo/retired.world_info.json"},
              {"generation", 0u},
              {"identity",
               phase == "foreign"             ? WorldSaveService::ArtifactIdentity{1, 2, 3}
               : phase.ends_with(" matching") ? installed
                                              : original},
              {"directory", false},
              {"absent_only", false}}});
        records.push_back(receipt);
        const unsigned last_phase = (phase == "prepared" || phase == "prepared matching") ? 0u
                                    : phase.starts_with("retired")                        ? 3u
                                    : phase.starts_with("flush")                          ? 2u
                                                                                          : 1u;
        for (unsigned recorded = 1; recorded <= last_phase; ++recorded) {
            receipt["phase"] = recorded;
            records.push_back(receipt);
        }
        Put(journal_directory(save.path()) / "txn.json", EncodeJournalRecords(records));
        const bool reused =
            WorldSaveService::write_member_bytes(save.path(), "world_info.json", "second");
        EXPECT_EQ(reused,
                  phase == "namespace" || phase == "flush" || phase == "retired" ||
                      phase.ends_with(" matching"));
        EXPECT_EQ(Get(save.path() / "world_info.json"), reused ? "second" : "first");
        if (reused) {
            bool reset = false, next = false;
            for (const auto& record : JournalRecords(save.path())) {
                if (record.at("record") == "receipt-reset") {
                    reset = true;
                    EXPECT_EQ(record.at("receipts").at(0).at("identity"),
                              nlohmann::json(phase.ends_with(" matching") ? installed : original));
                } else if (record.at("record") == "receipt" &&
                           record.at("receipts").at(0).at("generation") == 1u) {
                    next = true;
                    EXPECT_EQ(record.at("receipts").at(0).at("identity"),
                              nlohmann::json(installed));
                }
            }
            EXPECT_EQ(reset,
                      phase == "namespace" || phase == "flush" || phase == "namespace matching" ||
                          phase == "flush matching");
            EXPECT_EQ(next, phase != "prepared matching");
        } else {
            // Restore the admitted history before the transaction's ordinary abort.
            records.pop_back();
            if (phase == "foreign") {
                records.pop_back();
            }
            Put(journal_directory(save.path()) / "txn.json", EncodeJournalRecords(records));
        }
        transaction.abort(nullptr);
        EXPECT_EQ(Tree(save.path()), baseline);
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, PendingReceiptCapacityIsReservedBeforeForwardCompletion) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    const auto baseline = Tree(save.path());
    const auto member = save.path() / "world_info.json";
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "first"));
    ASSERT_TRUE(SaveTransaction::prepare_publication(member, {}, nullptr));
    WorldSaveService::ArtifactIdentity installed{}, control_identity{};
    bool missing = false;
    ASSERT_TRUE(WorldSaveService::artifact_identity(member, installed, missing));
    ASSERT_FALSE(missing);
    auto records = JournalRecords(save.path());
    std::erase_if(records, [](const auto& record) {
        return record.at("record") == "receipt" || record.at("record") == "receipt-reset";
    });
    auto receipt = records.front();
    receipt["record"] = "receipt";
    receipt["receipts"] = nlohmann::json::array({{{"path", "journal/undo/retired.world_info.json"},
                                                  {"generation", 0u},
                                                  {"identity", installed},
                                                  {"directory", false},
                                                  {"absent_only", false}}});
    for (unsigned phase = 0; phase <= 1; ++phase) {
        receipt["phase"] = phase;
        records.push_back(receipt);
    }
    ASSERT_TRUE(fs::remove(member));
    const auto control = journal_directory(save.path()) / "txn.json";
    const auto history = EncodeJournalRecords(records);
    Put(control, history);
    ASSERT_TRUE(WorldSaveService::artifact_identity(control, control_identity, missing));
    ASSERT_FALSE(missing);
    // Leave room for the immediate phase records, but not the independent
    // pending-receipt reserve. A duplicate completed record is valid replay.
    constexpr std::size_t kForwardRoom = 2048;
    const auto encoded = receipt.dump() + "\n";
    const auto padding = kMaxSnapshotIndexBytes - transaction.rollback_reserve_bytes() -
                         kForwardRoom - history.size();
    ASSERT_GT(padding, encoded.size());
    ASSERT_TRUE(WorldSaveService::append_journal_record(
        control,
        std::string(padding - encoded.size(), ' ') + encoded,
        control_identity,
        nullptr,
        transaction.rollback_reserve_bytes() + kForwardRoom));
    const auto evidence = Get(control);
    // Entry updates must preserve the same pending capacity as phase appends.
    EXPECT_FALSE(SaveTransaction::prepare_publication(member, {}, nullptr));
    EXPECT_EQ(Get(control), evidence);
    const auto completed = SaveTransaction::complete_owned_removal(member, nullptr);
    ASSERT_TRUE(completed.has_value());
    if (completed.has_value()) {
        EXPECT_FALSE(*completed);
    }
    EXPECT_EQ(Get(control), evidence);
    EXPECT_TRUE(journal_pending(save.path()));
    Put(control, history);
    transaction.abort(nullptr);
    EXPECT_EQ(Tree(save.path()), baseline);
    EXPECT_FALSE(journal_pending(save.path()));
}

TEST(SaveTransaction, ReceiptGenerationsRetainEachRecordedPublicationIdentity) {
    for (const bool explicit_reset : {false, true}) {
        SCOPED_TRACE(explicit_reset);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        WorldSaveService::ArtifactIdentity original{}, first{};
        bool missing = false;
        ASSERT_TRUE(WorldSaveService::artifact_identity(
            save.path() / "world_info.json", original, missing));
        ASSERT_FALSE(missing);
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "first"));
        ASSERT_TRUE(
            WorldSaveService::artifact_identity(save.path() / "world_info.json", first, missing));
        ASSERT_FALSE(missing);
        ASSERT_NE(original, first);
        ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "second"));
        {
            const HookGuard hook(+[](const char* step, const fs::path&) {
                if (std::string_view(step) == "crash-receipt-carrier-published")
                    throw std::runtime_error("carrier publication interrupted");
            });
            transaction.abort(nullptr);
        }
        const auto carrier = FindReceiptCarrier(save.path());
        ASSERT_FALSE(carrier.empty());
        const auto retained = Get(carrier);
        std::istringstream lines(retained);
        std::vector<nlohmann::json> records;
        std::string line;
        while (std::getline(lines, line))
            records.push_back(nlohmann::json::parse(line));
        for (auto record = records.begin(); record != records.end();) {
            if (record->at("record") == "receipt" || record->at("record") == "receipt-reset") {
                auto& items = (*record)["receipts"];
                for (auto item = items.begin(); item != items.end();) {
                    if (item->at("path") == "journal/undo/retired.world_info.json")
                        item = items.erase(item);
                    else
                        ++item;
                }
                if (items.empty()) {
                    record = records.erase(record);
                    continue;
                }
            }
            ++record;
        }
        auto receipt = records.front();
        receipt["record"] = "receipt";
        receipt["receipts"] =
            nlohmann::json::array({{{"path", "journal/undo/retired.world_info.json"},
                                    {"generation", 0u},
                                    {"identity", original},
                                    {"directory", false},
                                    {"absent_only", false}}});
        // Four phase records per generation. The second identity was bound by a
        // preceding entry record, even though the slot's final identity is different.
        for (unsigned generation = 0; generation < 2; ++generation) {
            receipt["receipts"][0]["generation"] = generation;
            receipt["receipts"][0]["identity"] = generation == 0 ? original : first;
            for (unsigned phase = 0; phase < 4; ++phase) {
                const bool reset = explicit_reset && generation == 0 && phase == 3;
                receipt["record"] = reset ? "receipt-reset" : "receipt";
                receipt["phase"] = reset ? 0u : phase;
                records.push_back(receipt);
            }
        }
        for (const std::string_view mutation : {"gap",
                                                "regression",
                                                "second live",
                                                "old generation",
                                                "duplicate",
                                                "negative",
                                                "string",
                                                "foreign",
                                                "changed retired"}) {
            SCOPED_TRACE(mutation);
            auto invalid = records;
            const auto start = invalid.size() - 8;
            if (mutation == "gap")
                invalid[start + 4]["receipts"][0]["generation"] = 2u;
            else if (mutation == "regression")
                invalid[start + 4]["receipts"][0]["generation"] = 0u;
            else if (mutation == "second live")
                invalid.erase(invalid.begin() + static_cast<std::ptrdiff_t>(start + 3));
            else if (mutation == "old generation")
                invalid.push_back(invalid[start + 3]);
            else if (mutation == "duplicate")
                invalid[start]["receipts"].push_back(invalid[start]["receipts"][0]);
            else if (mutation == "negative")
                invalid[start]["receipts"][0]["generation"] = -1;
            else if (mutation == "string")
                invalid[start]["receipts"][0]["generation"] = "0";
            else if (mutation == "foreign") {
                for (std::size_t index = start; index < start + 4; ++index)
                    invalid[index]["receipts"][0]["identity"] =
                        std::array<std::uint64_t, 3>{1, 2, 3};
            } else if (mutation == "changed retired")
                invalid[start + 3]["receipts"][0]["identity"] = first;
            Put(carrier, EncodeJournalRecords(invalid));
            const auto evidence = Tree(save.path());
            EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
            EXPECT_EQ(Tree(save.path()), evidence);
        }
        // Absence of generation continues to mean zero for earlier v5 writers.
        for (std::size_t index = records.size() - 8; index < records.size() - 4; ++index)
            records[index]["receipts"][0].erase("generation");
        Put(carrier, EncodeJournalRecords(records));
        EXPECT_NE(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
        EXPECT_FALSE(journal_pending(save.path()));
        EXPECT_EQ(Tree(save.path()), baseline);
    }
}

TEST(SaveTransaction, RecoveryRejectsUnparsableJournal) {
    TempSave save;
    Put(journal_directory(save.path()) / "txn.json", "garbage");
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_FALSE(errors.empty());
}

TEST(SaveTransaction, RecoveryRestoresRetiringControlAndRefusesInvalidRetiringEvidence) {
    for (const auto* input : {"valid", "empty", "partial", "unknown", "directory"}) {
        SCOPED_TRACE(input);
        TempSave save, retained;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto baseline = Tree(save.path());
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        const auto control = journal_directory(save.path()) / "txn.json";
        const auto slot = journal_directory(save.path()) / "undo/cleanup.txn.json";
        const auto saved_control = retained.path() / "control";
        if (std::string_view(input) == "valid") {
            fs::rename(control, slot);
            EXPECT_EQ(
                recover_pending_journal(fs::relative(save.path(), fs::current_path()), nullptr),
                RecoveryOutcome::RolledBack);
            EXPECT_EQ(Tree(save.path()), baseline);
            EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
        } else {
            fs::rename(control, saved_control);
            if (std::string_view(input) == "directory") {
                ASSERT_TRUE(fs::create_directory(slot));
            } else {
                Put(slot,
                    std::string_view(input) == "empty"     ? ""
                    : std::string_view(input) == "partial" ? "{"
                                                           : R"({"schema":"unknown"})");
            }
            const auto evidence = Tree(save.path());
            EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
            EXPECT_EQ(Tree(save.path()), evidence);
            ASSERT_TRUE(fs::remove(slot));
            fs::rename(saved_control, control);
            transaction.abort(nullptr);
            EXPECT_EQ(Tree(save.path()), baseline);
        }
    }
}

TEST(SaveTransaction, RetiringControlRefusesForeignInventoryWithoutRepublishing) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
    const auto baseline = Tree(save.path());
    SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new"));
    const auto journal = journal_directory(save.path());
    fs::rename(journal / "txn.json", journal / "undo/cleanup.txn.json");
    Put(journal / "foreign", "retain foreign bytes");
    const auto evidence = Tree(save.path());
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::Failed);
    EXPECT_EQ(Tree(save.path()), evidence);
    ASSERT_TRUE(fs::remove(journal / "foreign"));
    ASSERT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(Tree(save.path()), baseline);
    EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
}

TEST(SaveTransaction, RecoveryDiscardsJournalWithoutTxnFile) {
    TempSave save;
    fs::create_directories(journal_directory(save.path()));
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::DiscardedStale);
    EXPECT_FALSE(fs::exists(journal_directory(save.path())));
}

TEST(SaveTransaction, RecoveryPreservesUnknownTemporariesOfAnIndexedDirectoryWithoutJournal) {
    TempSave dir;
    ASSERT_TRUE(CommitOnce(dir.path(), 1, {{"world_info.json", "w"}}));
    Put(dir.path() / "chunks/region/r.0.0.lmr.tmp.123.4", "partial");
    Put(dir.path() / "world_info.json.tmp.5.6", "partial");
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(dir.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_TRUE(fs::exists(dir.path() / "chunks/region/r.0.0.lmr.tmp.123.4"));
    EXPECT_TRUE(fs::exists(dir.path() / "world_info.json.tmp.5.6"));
    EXPECT_EQ(Get(dir.path() / "world_info.json"), "w");
}

TEST(SaveTransaction, RecoveryPreservesUnknownTemporaries) {
    TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "w"}}));
    Put(save.path() / "chunks/region/r.0.0.lmr.tmp.123.4", "partial");
    Put(save.path() / "world_info.json.tmp.5.6", "partial");
    fs::create_directories(journal_directory(save.path()));

    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_TRUE(fs::exists(save.path() / "chunks/region/r.0.0.lmr.tmp.123.4"));
    EXPECT_TRUE(fs::exists(save.path() / "world_info.json.tmp.5.6"));
    EXPECT_TRUE(fs::exists(save.path() / "world_info.json"));
}

TEST(SaveTransaction, RecoveryPreservesNeutralStagingOutsideStrictInventory) {
    const TempSave save;
    ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "w"}}));
    const auto temporary = save.path() / "world_info.json.tmp.5.6";
    Put(temporary, "other occupant");
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::NothingToDo);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(Get(temporary), "other occupant");
}

TEST(SaveTransaction, RecoveryRefusesNonFileTransactionWithoutDeletingJournal) {
    const TempSave save;
    const auto transaction = journal_directory(save.path()) / "txn.json";
    fs::create_directories(transaction);
    Put(transaction / "occupant", "preserve");
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(Get(transaction / "occupant"), "preserve");
}

TEST(SaveTransaction, InitialIndexRefusesNonFileMemberWithoutPublishingPartialInventory) {
    const TempSave save;
    const auto invalid = save.path() / "chunks/region/r.0.0.lmr";
    fs::create_directory(invalid);
    Put(invalid / "occupant", "preserve");
    SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
    std::vector<std::string> errors;
    ASSERT_TRUE(transaction.begin(&errors));
    ASSERT_TRUE(transaction.protect({"world_info.json"}, &errors));
    EXPECT_FALSE(transaction.commit(&errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_FALSE(snapshot_index_exists(save.path()));
    EXPECT_FALSE(journal_pending(save.path()));
    EXPECT_EQ(Get(invalid / "occupant"), "preserve");
}

#if !defined(_WIN32)

TEST(SaveTransactionDeathTest, EveryJournaledPublicationMoveRecoversThePreviousCommit) {
    ::testing::GTEST_FLAG(death_test_style) = "fast";
    for (const std::string_view step : {"before-replace",
                                        "retirement-quarantined",
                                        "after-retire",
                                        "publication-quarantined",
                                        "after-replace"}) {
        SCOPED_TRACE(step);
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "old"}}));
        const auto before = Tree(save.path());
        EXPECT_EXIT(
            {
                SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
                if (!transaction.begin(nullptr) ||
                    !transaction.protect({"world_info.json"}, nullptr))
                    std::_Exit(9);
                // The selected event is local to this member, after its journal append.
                static std::string_view selected = step;
                set_snapshot_step_hook_for_testing(+[](const char* event, const fs::path& path) {
                    if (path.filename() == "world_info.json" && event == selected)
                        std::_Exit(7);
                });
                (void)WorldSaveService::write_member_bytes(save.path(), "world_info.json", "new");
                std::_Exit(9);
            },
            ::testing::ExitedWithCode(7),
            "");
        std::vector<std::string> errors;
        ASSERT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::RolledBack)
            << (errors.empty() ? "" : errors.front());
        EXPECT_EQ(Tree(save.path()), before);
        EXPECT_EQ(recover_pending_journal(save.path(), nullptr), RecoveryOutcome::NothingToDo);
    }
}

TEST(SaveTransactionDeathTest, EveryDiskTransitionRecoversAndAllowsTheNextSave) {
    ::testing::GTEST_FLAG(death_test_style) = "fast";
    for (const bool initial : {false, true}) {
        SCOPED_TRACE(initial);
        TempSave base, final;
        if (!initial) {
            ASSERT_TRUE(CommitOnce(base.path(),
                                   1,
                                   {{"world_info.json", "old metadata"},
                                    {"aether_state.efs", "old energy"},
                                    {"chunks/region/world-manifest.json", "old manifest"}}));
        }
        const auto baseline = Tree(base.path());
        CopyTree(base.path(), final.path());
        ASSERT_TRUE(RunInventoryScenario(final.path()));
        const auto finished_tree = Tree(final.path());
        bool complete = false;
        for (int cut = 1; cut <= 256 && !complete; ++cut) {
            SCOPED_TRACE(cut);
            TempSave work;
            CopyTree(base.path(), work.path());
            EXPECT_EXIT(ChildInventoryScenario(work.path(), cut), KilledOrFinished, "");
            ASSERT_TRUE(DeathState().last_exit_code == 0 || DeathState().last_exit_code == 7);
            const auto neutral = UnregisteredStaging(work.path());
            std::vector<std::string> errors;
            ASSERT_NE(recover_pending_journal(work.path(), &errors), RecoveryOutcome::Failed)
                << (errors.empty() ? "" : errors.front());
            const auto recovered = RecoveredCommittedTree(work.path(), neutral);
            EXPECT_TRUE(recovered == baseline || recovered == finished_tree);
            EXPECT_EQ(recover_pending_journal(work.path(), nullptr), RecoveryOutcome::NothingToDo);
            ASSERT_TRUE(CommitOnce(work.path(), 6, {{"world_info.json", "next"}}));
            complete = DeathState().last_exit_code == 0;
        }
        EXPECT_TRUE(complete);
    }
}

TEST(SaveTransactionDeathTest, EveryRecoveryDiskTransitionRestoresThePreviousCommit) {
    ::testing::GTEST_FLAG(death_test_style) = "fast";
    TempSave base;
    ASSERT_TRUE(CommitOnce(base.path(),
                           1,
                           {{"world_info.json", "old metadata"},
                            {"aether_state.efs", "old energy"},
                            {"chunks/region/world-manifest.json", "old manifest"}}));
    const auto baseline = Tree(base.path());
    bool complete = false;
    for (int cut = 1; cut <= 256 && !complete; ++cut) {
        SCOPED_TRACE(cut);
        TempSave work;
        CopyTree(base.path(), work.path());
        EXPECT_EXIT(ChildInventoryScenario(work.path(), 0, true), ::testing::ExitedWithCode(7), "");
        EXPECT_EXIT(
            {
                DeathState().include_disk_events = true;
                ChildKilledRecovery(work.path(), cut);
            },
            KilledOrFinished,
            "");
        ASSERT_TRUE(DeathState().last_exit_code == 0 || DeathState().last_exit_code == 7);
        const auto neutral = UnregisteredStaging(work.path());
        std::vector<std::string> errors;
        ASSERT_NE(recover_pending_journal(work.path(), &errors), RecoveryOutcome::Failed)
            << (errors.empty() ? "" : errors.front());
        EXPECT_EQ(RecoveredCommittedTree(work.path(), neutral), baseline);
        EXPECT_EQ(recover_pending_journal(work.path(), nullptr), RecoveryOutcome::NothingToDo);
        ASSERT_TRUE(CommitOnce(work.path(), 6, {{"world_info.json", "next"}}));
        complete = DeathState().last_exit_code == 0;
    }
    EXPECT_TRUE(complete);
}

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
    DeathState().calls = 0;
    DeathState().published_at = 0;
    {
        HookGuard guard(&RecordHook);
        ASSERT_TRUE(RunScenario(probe.path()));
    }
    ASSERT_GT(DeathState().published_at, 0);
    ASSERT_GT(DeathState().calls, DeathState().published_at);
    // Midway between journal publication and the end of the transaction: the journal still exists.
    const int kill_at = (DeathState().published_at + DeathState().calls) / 2;

    TempSave dead;
    CopyTree(base.path(), dead.path());
    EXPECT_EXIT(ChildKillScenario(dead.path(), kill_at), KilledOrFinished, "");
    ASSERT_EQ(DeathState().last_exit_code, 7) << "transaction was not killed at hook " << kill_at;
    ASSERT_TRUE(journal_pending(dead.path())) << "kill_at=" << kill_at;

    bool saw_finish = false;
    int recovery_kills = 0;
    for (int m = 1; m <= 20 && !saw_finish; ++m) {
        TempSave rec;
        CopyTree(base.path(), rec.path());
        EXPECT_EXIT(ChildKillScenario(rec.path(), kill_at), KilledOrFinished, "");
        ASSERT_EQ(DeathState().last_exit_code, 7);
        ASSERT_TRUE(journal_pending(rec.path()));
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
struct WindowsInterruptionState {
    int cut = 0;
    int calls = 0;
    int rollback_cut = 0;
    bool include_disk_events = false;
};
WindowsInterruptionState& WindowsInterruption() {
    static WindowsInterruptionState state;
    return state;
}
void WindowsKillHook(const char* step, const fs::path&) {
    auto& state = WindowsInterruption();
    if (!state.include_disk_events && std::string_view(step).starts_with("crash-"))
        return;
    if (++state.calls == state.cut)
        std::_Exit(7);
}

void WindowsRecordHook(const char* step, const fs::path& path) {
    auto& state = WindowsInterruption();
    if (!state.include_disk_events && std::string_view(step).starts_with("crash-"))
        return;
    ++state.calls;
    const auto member = state.include_disk_events ? "world_info.json" : "aether_state.efs";
    // Retain the original earliest visible replacement, before source retirement
    // in the disk matrix, so recovery still exercises the linked staging entry.
    const auto visible = state.include_disk_events ? "crash-publication-linked" : "after-replace";
    if (state.rollback_cut == 0 && std::string_view(step) == visible && path.filename() == member)
        state.rollback_cut = state.calls;
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

std::optional<DWORD>
WindowsTxnChild(const fs::path& dir, int cut, bool recovery, bool disk_events = false) {
    std::wstring executable(32768, L'\0');
    const DWORD count =
        ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (count == 0 || count == executable.size())
        return std::nullopt;
    executable.resize(count);
    const auto previous_dir = WindowsTestEnv(L"LUM_TXN_TEST_CHILD_DIR");
    const auto previous_cut = WindowsTestEnv(L"LUM_TXN_TEST_CHILD_CUT");
    const auto previous_mode = WindowsTestEnv(L"LUM_TXN_TEST_CHILD_RECOVER");
    const auto previous_disk = WindowsTestEnv(L"LUM_TXN_TEST_CHILD_DISK");
    const auto cut_text = std::to_wstring(cut);
    const bool configured =
        ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_DIR", dir.c_str()) &&
        ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_CUT", cut_text.c_str()) &&
        ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_RECOVER", recovery ? L"1" : L"0") &&
        ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_DISK", disk_events ? L"1" : L"0");
    std::wstring command = L"\"" + executable +
                           L"\" --gtest_filter=SaveTransaction.WindowsInterruptionChild "
                           L"--gtest_color=no";
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
    ::SetEnvironmentVariableW(L"LUM_TXN_TEST_CHILD_DISK",
                              previous_disk.empty() ? nullptr : previous_disk.c_str());
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

// Direct runs verify the same publication scenario. The interruption matrix supplies
// the child directory and cut point when exercising process termination.
TEST(SaveTransaction, WindowsInterruptionChild) {
    const auto dir = WindowsTestEnv(L"LUM_TXN_TEST_CHILD_DIR");
    if (dir.empty()) {
        TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(),
                               1,
                               {{"world_info.json", "w0"},
                                {"aether_state.efs", "e0"},
                                {"chunks/region/active-regions.arl", "a0"}}));
        ASSERT_TRUE(WindowsScenario(save.path()));
        EXPECT_EQ(Get(save.path() / "world_info.json"), "w1");
        EXPECT_EQ(Get(save.path() / "aether_state.efs"), "e1");
        EXPECT_EQ(Get(save.path() / "chunks/region/plant-entities.json"), "p1");
        EXPECT_FALSE(fs::exists(save.path() / "chunks/region/active-regions.arl"));
        EXPECT_FALSE(journal_pending(save.path()));
        return;
    }
    auto& state = WindowsInterruption();
    state.cut = std::stoi(WindowsTestEnv(L"LUM_TXN_TEST_CHILD_CUT"));
    state.calls = 0;
    state.include_disk_events = WindowsTestEnv(L"LUM_TXN_TEST_CHILD_DISK") == L"1";
    set_snapshot_step_hook_for_testing(&WindowsKillHook);
    if (WindowsTestEnv(L"LUM_TXN_TEST_CHILD_RECOVER") == L"1")
        std::_Exit(recover_pending_journal(fs::path(dir), nullptr) == RecoveryOutcome::Failed ? 9
                                                                                              : 0);
    std::_Exit((state.include_disk_events ? RunInventoryScenario(fs::path(dir))
                                          : WindowsScenario(fs::path(dir)))
                   ? 0
                   : 9);
}

namespace {
// Each CTest entry executes at most two cuts. Together the entries retain all
// 256 disk cuts and all 200 publication cuts, including a successful completion.
class WindowsTransactionTransitions : public ::testing::TestWithParam<int> {};

void CheckWindowsTransitions(int batch, bool disk_events, bool recovery) {
    TempSave base, final, work;
    if (disk_events) {
        ASSERT_TRUE(CommitOnce(base.path(),
                               1,
                               {{"world_info.json", "old metadata"},
                                {"aether_state.efs", "old energy"},
                                {"chunks/region/world-manifest.json", "old manifest"}}));
    } else {
        ASSERT_TRUE(CommitOnce(base.path(),
                               1,
                               {{"world_info.json", "w0"},
                                {"aether_state.efs", "e0"},
                                {"chunks/region/active-regions.arl", "a0"}}));
    }
    const auto baseline = Tree(base.path());
    CopyTree(base.path(), final.path());
    auto& state = WindowsInterruption();
    state = {};
    state.include_disk_events = disk_events;
    {
        const HookGuard hook(&WindowsRecordHook);
        ASSERT_TRUE(disk_events ? RunInventoryScenario(final.path())
                                : WindowsScenario(final.path()));
    }
    const auto finished = Tree(final.path());
    const int bound = disk_events ? 256 : 200;
    ASSERT_LT(state.calls, bound);
    const int rollback_cut = state.rollback_cut;
    ASSERT_GT(rollback_cut, 0);
    bool complete = false;
    constexpr int cuts_per_batch = 2;
    const int first = batch * cuts_per_batch + 1;
    const int last = std::min(first + cuts_per_batch - 1, bound);
    for (int cut = first; cut <= last && !complete; ++cut) {
        SCOPED_TRACE(cut);
        CopyTree(base.path(), work.path());
        if (recovery) {
            const auto interrupted = WindowsTxnChild(work.path(), rollback_cut, false, disk_events);
            ASSERT_TRUE(interrupted.has_value());
            ASSERT_EQ(interrupted.value_or(9), 7u);
            ASSERT_TRUE(journal_pending(work.path()));
            ASSERT_EQ(Get(snapshot_index_path(work.path())),
                      baseline.at(kSnapshotIndexRelativePath));
            ASSERT_EQ(Get(work.path() / (disk_events ? "world_info.json" : "aether_state.efs")),
                      disk_events ? "new metadata" : "e1");
        }
        const auto exit_code = WindowsTxnChild(work.path(), cut, recovery, disk_events);
        ASSERT_TRUE(exit_code.has_value());
        const DWORD code = exit_code.value_or(9);
        ASSERT_TRUE(code == 0 || code == 7) << code;
        const auto neutral = UnregisteredStaging(work.path());
        std::vector<std::string> errors;
        ASSERT_NE(recover_pending_journal(work.path(), &errors), RecoveryOutcome::Failed)
            << (errors.empty() ? "" : errors.front());
        const auto recovered =
            disk_events ? RecoveredCommittedTree(work.path(), neutral) : Tree(work.path());
        if (disk_events && recovery) {
            EXPECT_EQ(recovered, baseline);
        } else {
            EXPECT_TRUE(recovered == baseline || recovered == finished);
        }
        EXPECT_EQ(recover_pending_journal(work.path(), nullptr), RecoveryOutcome::NothingToDo);
        EXPECT_EQ(disk_events ? RecoveredCommittedTree(work.path(), neutral) : Tree(work.path()),
                  recovered);
        if (disk_events && !recovery) {
            ASSERT_TRUE(CommitOnce(work.path(), 6, {{"world_info.json", "next"}}));
        }
        complete = code == 0;
    }
    // Earlier batches need not reach completion; the final batch must prove
    // that no unvisited transition lies beyond the unchanged enumeration bound.
    if (last == bound) {
        EXPECT_TRUE(complete);
    }
}
} // namespace

TEST_P(WindowsTransactionTransitions, EveryDiskTransitionPreservesCommitAndNeutralStaging) {
    CheckWindowsTransitions(GetParam(), true, false);
}
TEST_P(WindowsTransactionTransitions, EveryRecoveryDiskTransitionRestoresThePreviousCommit) {
    CheckWindowsTransitions(GetParam(), true, true);
}
INSTANTIATE_TEST_SUITE_P(DiskCuts, WindowsTransactionTransitions, ::testing::Range(0, 128));

// Publication-only cuts have a separate fixture so their original 200-cut bound
// remains independent of the disk-event matrix.
namespace {
class WindowsPublicationTransitions : public ::testing::TestWithParam<int> {};
} // namespace
TEST_P(WindowsPublicationTransitions, KillAtEveryPublicationStepIsIdempotent) {
    CheckWindowsTransitions(GetParam(), false, false);
}
TEST_P(WindowsPublicationTransitions, KillAtEveryRecoveryStepIsIdempotent) {
    CheckWindowsTransitions(GetParam(), false, true);
}
INSTANTIATE_TEST_SUITE_P(PublicationCuts, WindowsPublicationTransitions, ::testing::Range(0, 100));

#endif

} // namespace Luminumbra::Persistence

namespace Luminumbra::Persistence {
TEST(SaveTransaction, SnapshotInputsDistinguishMissingInvalidAndOversized) {
    const TempSave save;
    const auto path = snapshot_index_path(save.path());
    fs::create_directories(path.parent_path());
    SnapshotIndex index;
    std::vector<std::string> errors;
    ASSERT_TRUE(read_snapshot_index(save.path(), index, &errors));
    EXPECT_FALSE(snapshot_index_exists(save.path()));
    fs::create_directory(path);
    EXPECT_TRUE(snapshot_index_exists(save.path()));
    EXPECT_FALSE(read_snapshot_index(save.path(), index, &errors));
    std::uint64_t bytes = 0;
    std::uint64_t hash = 0;
    bool present = false;
    EXPECT_FALSE(hash_file(path, bytes, hash, present, &errors));
    ASSERT_TRUE(fs::remove(path));
    Put(path, "{");
    EXPECT_FALSE(read_snapshot_index(save.path(), index, &errors));
    fs::resize_file(path, kMaxSnapshotIndexBytes + 1);
    EXPECT_FALSE(read_snapshot_index(save.path(), index, &errors));
    ASSERT_TRUE(fs::remove(path));
    EXPECT_TRUE(hash_file(path, bytes, hash, present, &errors));
    EXPECT_FALSE(present);
    EXPECT_EQ(bytes, 0u);
    EXPECT_EQ(hash, 0u);
}
} // namespace Luminumbra::Persistence

namespace Luminumbra::Persistence {
TEST(SaveTransaction, ControlJsonRefusesExcessiveNestingBeforeParsing) {
    const std::string nested = std::string(65, '[') + "0" + std::string(65, ']');
    auto encoded = encode_snapshot_index(MakeIndex());
    encoded.insert(encoded.rfind('}'), ",\"deep\":" + nested);
    SnapshotIndex index;
    std::string error;
    EXPECT_FALSE(decode_snapshot_index(encoded, index, error));
    EXPECT_FALSE(error.empty());
    const TempSave save;
    fs::create_directories(journal_directory(save.path()));
    const std::string journal =
        R"({"schema":"luminumbra.journal.v1","txn_id":1,"base_generation":0,"new_generation":1,"entries":[],"deep":)" +
        nested + "}";
    Put(journal_directory(save.path()) / "txn.json", journal);
    std::vector<std::string> errors;
    EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
    EXPECT_FALSE(errors.empty());
    EXPECT_TRUE(fs::exists(journal_directory(save.path()) / "txn.json"));
}

TEST(SaveTransaction, ExhaustedGenerationAndTransactionRefuseBeforeJournalCreation) {
    for (const bool exhaust_generation : {false, true}) {
        const TempSave save;
        auto index = MakeIndex();
        if (exhaust_generation)
            index.generation = std::numeric_limits<std::uint64_t>::max();
        else
            index.txn_id = std::numeric_limits<std::uint64_t>::max();
        fs::create_directories(snapshot_index_path(save.path()).parent_path());
        Put(snapshot_index_path(save.path()), encode_snapshot_index(index));
        SaveTransaction transaction(save.path(), 1, {}, &RawDigest);
        std::vector<std::string> errors;
        EXPECT_FALSE(transaction.begin(&errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_FALSE(journal_pending(save.path()));
    }
}

TEST(SaveTransaction, SnapshotAnchorsRefuseOutOfFloatRangeBeforeConversion) {
    auto encoded = nlohmann::json::parse(encode_snapshot_index(MakeIndex()));
    encoded["anchors"] = nlohmann::json::array({nlohmann::json::array({1e300, 0, 0})});
    SnapshotIndex index;
    std::string error;
    EXPECT_FALSE(decode_snapshot_index(encoded.dump(), index, error));
    EXPECT_FALSE(error.empty());
}

TEST(SaveTransaction, RecoveryRefusesUnknownGenerationAndDuplicateMembersWithoutMutation) {
    for (const bool duplicate : {false, true}) {
        const TempSave save;
        ASSERT_TRUE(CommitOnce(save.path(), 1, {{"world_info.json", "other owner's bytes"}}));
        SaveTransaction transaction(save.path(), 2, {}, &RawDigest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(transaction.protect({"world_info.json"}, nullptr));
        auto records = JournalRecords(save.path());
        ASSERT_FALSE(records.empty());
        ASSERT_EQ(records.back().at("entries").size(), 1u);
        if (duplicate) {
            const auto entry = records.back().at("entries").front();
            records.back()["entries"].push_back(entry);
        } else {
            for (auto& record : records) {
                record["base_generation"] = 3;
                record["new_generation"] = 4;
            }
        }
        Put(journal_directory(save.path()) / "txn.json", EncodeJournalRecords(records));
        const auto evidence = Tree(save.path());
        std::vector<std::string> errors;
        EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed);
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(Get(save.path() / "world_info.json"), "other owner's bytes");
        EXPECT_TRUE(journal_pending(save.path()));
        EXPECT_EQ(Tree(save.path()), evidence);
    }
}

TEST(SaveTransaction, RecoveryRefusesInvalidJournalIdentityBeforeMutation) {
    for (int invalid_case = 0; invalid_case != 8; ++invalid_case) {
        const TempSave save;
        Put(save.path() / "world_info.json", "preserve");
        SaveTransaction writer(save.path(), 1, {}, &RawDigest);
        ASSERT_TRUE(writer.begin(nullptr));
        ASSERT_TRUE(writer.protect({"aether_state.efs"}, nullptr));
        auto records = JournalRecords(save.path());
        ASSERT_FALSE(records.empty());
        ASSERT_EQ(records.back().at("entries").size(), 1u);
        ASSERT_EQ(records.back().at("entries").front().at("action"), "create");
        auto& journal = records.back();
        switch (invalid_case) {
            case 0:
                journal["txn_id"] = 0;
                break;
            case 1:
                journal["base_generation"] = std::numeric_limits<std::uint64_t>::max();
                break;
            case 2:
                journal["new_generation"] = 2;
                break;
            case 3:
                journal["entries"][0]["undo"] = "undo/0";
                break;
            case 4:
                journal["entries"][0]["prev_bytes"] = 1;
                break;
            case 5:
                journal["entries"][0]["prev_fnv64"] = "0000000000000001";
                break;
            case 6:
                journal["entries"][0]["action"] = "replace";
                break;
            case 7:
                journal["entries"][0]["action"] = "replace";
                journal["entries"][0]["undo"] = "undo/0";
                journal["entries"][0]["prev_bytes"] = std::numeric_limits<std::uint64_t>::max();
                break;
        }
        const auto transaction = journal_directory(save.path()) / "txn.json";
        const auto encoded = EncodeJournalRecords(records);
        Put(transaction, encoded);
        std::vector<std::string> errors;
        EXPECT_EQ(recover_pending_journal(save.path(), &errors), RecoveryOutcome::Failed)
            << invalid_case;
        EXPECT_FALSE(errors.empty()) << invalid_case;
        EXPECT_EQ(Get(save.path() / "world_info.json"), "preserve") << invalid_case;
        EXPECT_EQ(Get(transaction), encoded) << invalid_case;
    }
}
} // namespace Luminumbra::Persistence
