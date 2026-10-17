#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"

#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/SimulationPendingState.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/AuthorityHashState.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace P = Luminumbra::Persistence;
namespace W = Luminumbra::world;
namespace S = luminumbra::simulation;
namespace fs = std::filesystem;
using State = P::SimulationPendingState;
using TreeMap = std::map<std::string, std::string>;

std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read pending fixture");
    return {std::istreambuf_iterator<char>(in), {}};
}
void Write(const fs::path& file, std::string_view bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out.good())
        throw std::runtime_error("Cannot write pending fixture");
}
TreeMap Tree(const fs::path& root) {
    TreeMap result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file())
            result.emplace(
                Luminumbra::Filesystem::GenericPathString(entry.path().lexically_relative(root)),
                Read(entry.path()));
    return result;
}
void Copy(const fs::path& from, const fs::path& to) {
    fs::remove_all(to);
    fs::create_directories(to.parent_path());
    fs::copy(from, to, fs::copy_options::recursive);
}
class PendingWorld {
public:
    fs::path root = Luminumbra::TestSupport::CreateUniqueTempDirectory("pending_state_");
    Luminumbra::JobSystem jobs;
    W::GameSession session;
    explicit PendingWorld(bool enabled = true) {
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        Luminumbra::TestSupport::WriteSimulationContentFixture(root);
        jobs.startup(1);
        Configure(session, enabled);
        if (!session.CreateWorld("Region golden", "1337", "default"))
            throw std::runtime_error(session.GetWorldOpenError());
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3{0, 0, 0});
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->heightmap_data = {8.0f, 8.5f, 9.0f};
        chunk->mark_voxel_data_dirty();
        if (!session.GetWorldSystem()->adopt_streamed_chunk(chunk))
            throw std::runtime_error("Cannot adopt pending fixture chunk");
    }
    ~PendingWorld() {
        session.GetWorldSystem()->quiesce_streaming_jobs_for_save();
        std::error_code error;
        fs::remove_all(root, error);
    }
    void Configure(W::GameSession& target, bool enabled = true) {
        target.SetRootPath(Luminumbra::Filesystem::GenericPathString(root) + "/");
        target.SetJobSystem(&jobs);
        target.SetActiveRegionsEnabled(enabled);
        target.SetAetherStateEnabled(enabled);
    }
};
State Snapshot(W::GameSession& session) {
    State state;
    state.events = session.GetSimulationEventBus().snapshot();
    if (const auto* energy = session.GetEnergyFieldState()) {
        state.energy_channels = energy->channels();
        state.deposits = energy->PendingDeposits();
    }
    return state;
}
bool RawDigest(const fs::path& root, std::string_view member, P::MemberDigest& out, std::string&) {
    return P::hash_file(root / member,
                        out.bytes,
                        out.fnv64,
                        out.present,
                        nullptr,
                        P::save_member_byte_limit(member));
}
void ReindexMember(const fs::path& root, std::string_view path = State::kMember) {
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(root, index, nullptr));
    const auto member = std::find_if(index.members.begin(),
                                     index.members.end(),
                                     [path](const auto& item) { return item.path == path; });
    ASSERT_NE(member, index.members.end());
    P::MemberDigest digest;
    std::string error;
    ASSERT_TRUE(RawDigest(root, path, digest, error));
    member->present = digest.present;
    member->bytes = digest.bytes;
    member->fnv64 = digest.fnv64;
    Write(P::snapshot_index_path(root), P::encode_snapshot_index(index));
}
void SetInteger(std::string& bytes, std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.at(offset + i) = static_cast<char>((value >> (8 * i)) & 255);
}
void Reseal(std::string& bytes) {
    const auto hash = P::fnv1a64_bytes(bytes.data(), bytes.size() - 8);
    SetInteger(bytes, bytes.size() - 8, hash, 8);
}
struct HookState {
    fs::path source, copy;
    bool capture = false, fail = false, committed = false, seen = false;
    const char* corrupt_member = nullptr;
    const char* publication_member = "simulation-pending.spn";
};
HookState& Hook() {
    static HookState state;
    return state;
}
void SnapshotHook(const char* step, const fs::path& path) {
    auto& state = Hook();
    const bool selected = state.corrupt_member ? std::string_view(step) == "after-replace" &&
                                                     path.filename() == state.corrupt_member
                          : state.committed ? std::string_view(step) == "commit-point"
                                            : std::string_view(step) == "after-replace" &&
                                                  path.filename() == state.publication_member;
    if (!selected)
        return;
    state.seen = true;
    if (state.corrupt_member) {
        P::set_snapshot_step_hook_for_testing(nullptr);
        Write(path, "malformed");
    }
    if (state.capture)
        Copy(state.source, state.copy);
    if (state.fail) {
        P::set_snapshot_step_hook_for_testing(nullptr);
        throw std::runtime_error("Injected pending member replacement failure");
    }
}
class HookGuard {
public:
    explicit HookGuard(HookState state) {
        Hook() = std::move(state);
        P::set_snapshot_step_hook_for_testing(&SnapshotHook);
    }
    ~HookGuard() {
        P::set_snapshot_step_hook_for_testing(nullptr);
    }
};

void SaveWithDecodeBudget(W::GameSession& session,
                          const fs::path& destination,
                          std::uint64_t expected_pending_decodes,
                          std::uint64_t expected_ledger_loads) {
    const auto worlds = P::WorldSaveService::decode_count_for_testing();
    const auto pending = State::decode_count_for_testing();
    const auto ledgers = P::WorldSaveService::ledger_load_count_for_testing();
    ASSERT_TRUE(session.SaveWorldStateTo(destination));
    EXPECT_LE(P::WorldSaveService::decode_count_for_testing() - worlds, 1u);
    EXPECT_EQ(State::decode_count_for_testing() - pending, expected_pending_decodes);
    EXPECT_EQ(P::WorldSaveService::ledger_load_count_for_testing() - ledgers,
              expected_ledger_loads);
}

TEST(SimulationPendingState, FutureEventsAndSequenceRoundtrip) {
    EXPECT_EQ(P::save_member_byte_limit(State::kMember), State::kMaxFileBytes);
    PendingWorld world;
    auto& bus = world.session.GetSimulationEventBus();
    const auto empty = world.session.CaptureAuthorityState();
    ASSERT_TRUE(empty.state) << empty.error;
    EXPECT_EQ(bus.publish(9, std::string("topic\0x", 7), std::string("data\0z", 6), 2), 0u);
    EXPECT_EQ(bus.publish(9, "first", "negative lane", -7), 1u);
    EXPECT_EQ(bus.publish(3, "earlier", "retired"), 2u);
    ASSERT_EQ(bus.drain_enabled(3, [] { return true; }, {}).size(), 1u);
    const auto expected = Snapshot(world.session).encode();
    const auto capture = world.session.CaptureAuthorityState();
    ASSERT_TRUE(capture.state) << capture.error;
    EXPECT_NE(capture.state->authority_bytes(), empty.state->authority_bytes());
    EXPECT_EQ(capture.state->pending_event_bytes(), Snapshot(world.session).event_bytes());
    const auto dir = world.session.GetWorldSaveDir();
    // A first index uses writer fingerprints; established saves decode only the
    // destination's old pending member and load its ledger once.
    ASSERT_NO_FATAL_FAILURE(SaveWithDecodeBudget(world.session, world.root / "fresh", 0, 0));
    ASSERT_NO_FATAL_FAILURE(SaveWithDecodeBudget(world.session, dir, 1, 1));
    ASSERT_NO_FATAL_FAILURE(SaveWithDecodeBudget(world.session, dir, 1, 1));
    W::GameSession disabled;
    world.Configure(disabled, false);
    const auto readers = P::WorldSaveService::pending_reader_count_for_testing();
    const auto probes = P::WorldSaveService::pending_probe_count_for_testing();
    EXPECT_FALSE(disabled.LoadWorld(world.session.GetMetadata().worldId));
    EXPECT_EQ(P::WorldSaveService::pending_reader_count_for_testing(), readers);
    EXPECT_EQ(P::WorldSaveService::pending_probe_count_for_testing(), probes);
    EXPECT_EQ(Snapshot(world.session).encode(), expected);
    const auto tree = Tree(dir);
    const auto repeated = world.session.CaptureAuthorityState();
    ASSERT_TRUE(repeated.state) << repeated.error;
    EXPECT_EQ(repeated.state->authority_bytes(), capture.state->authority_bytes());
    EXPECT_EQ(Tree(dir), tree);
    W::GameSession loaded;
    world.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(world.session.GetMetadata().worldId))
        << loaded.GetWorldOpenError();
    EXPECT_EQ(Snapshot(loaded).encode(), expected);
    const auto restored = loaded.CaptureAuthorityState();
    ASSERT_TRUE(restored.state) << restored.error;
    EXPECT_EQ(restored.state->authority_bytes(), capture.state->authority_bytes());
    EXPECT_EQ(loaded.GetSimulationEventBus().publish(10, "probe", ""), 3u);
    const auto delivered = loaded.GetSimulationEventBus().drain_enabled(9, [] { return true; }, {});
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(delivered[0].sequence, 1u);
    EXPECT_EQ(delivered[0].lane, -7);
    EXPECT_EQ(delivered[1].topic, std::string("topic\0x", 7));
    EXPECT_EQ(delivered[1].payload, std::string("data\0z", 6));
    EXPECT_EQ(loaded.GetSimulationEventBus().pending_count(), 1u);
    // Sequence remains authority after every queued event has been delivered.
    loaded.GetSimulationEventBus().drain_enabled(10, [] { return true; }, {});
    auto sequence_only = loaded.GetSimulationEventBus().snapshot();
    const auto sequenced = loaded.CaptureAuthorityState();
    ASSERT_TRUE(sequenced.state) << sequenced.error;
    loaded.GetSimulationEventBus().commit_batch(S::OrderedEventBus::prepare_restore({}));
    const auto reset_sequence = loaded.CaptureAuthorityState();
    ASSERT_TRUE(reset_sequence.state) << reset_sequence.error;
    EXPECT_NE(reset_sequence.state->authority_bytes(), sequenced.state->authority_bytes());
    loaded.GetSimulationEventBus().commit_batch(
        S::OrderedEventBus::prepare_restore(std::move(sequence_only)));
    ASSERT_TRUE(loaded.SaveWorldState());
    W::GameSession again;
    world.Configure(again);
    ASSERT_TRUE(again.LoadWorld(loaded.GetMetadata().worldId));
    EXPECT_EQ(again.GetSimulationEventBus().pending_count(), 0u);
    EXPECT_EQ(again.GetSimulationEventBus().publish(11, "next", ""), 4u);
    S::OrderedEventBus exhausted;
    exhausted.set_durable(true);
    S::OrderedEventBus::Snapshot terminal;
    terminal.next_sequence = std::numeric_limits<std::uint64_t>::max();
    exhausted.commit_batch(S::OrderedEventBus::prepare_restore(std::move(terminal)));
    EXPECT_THROW(exhausted.publish(12, "overflow", ""), std::overflow_error);
    EXPECT_EQ(exhausted.snapshot().next_sequence, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(exhausted.pending_count(), 0u);
    const auto protected_tree = Tree(dir);
    // A generic raw writer must invalidate the cached pending facts.
    EXPECT_FALSE(world.session.RunInSnapshotTransaction(
        [&] { return P::WorldSaveService::write_member_bytes(dir, State::kMember, "malformed"); }));
    EXPECT_EQ(Tree(dir), protected_tree);
}

void CheckLimitRecovery(PendingWorld& world) {
    const auto expected = Snapshot(world.session).encode();
    const auto capture = world.session.CaptureAuthorityState();
    ASSERT_TRUE(capture.state) << capture.error;
    ASSERT_NO_FATAL_FAILURE(SaveWithDecodeBudget(world.session, world.root / "at-limit", 0, 0));
    ASSERT_NO_FATAL_FAILURE(
        SaveWithDecodeBudget(world.session, world.session.GetWorldSaveDir(), 1, 1));
    EXPECT_EQ(Snapshot(world.session).encode(), expected);
    // A full queue defers activation deliveries in the outbox. An independent
    // preparation failure still exercises the scheduler fault/recovery-save path.
    world.session.PinActiveRegion({0, 0}, true);
    world.session.SetActivationEnqueuePreparationHook(
        [] { throw std::runtime_error("Injected preparation failure at queue limit"); });
    EXPECT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(world.session.GetRegionSchedulerFault());
    EXPECT_EQ(Snapshot(world.session).encode(), expected);
    const auto fault_capture = world.session.CaptureAuthorityState();
    ASSERT_TRUE(fault_capture.state) << fault_capture.error;
    EXPECT_EQ(world.session.TickSimulation(1.0), 0u);
    ASSERT_TRUE(world.session.SaveWorldState());
    ASSERT_TRUE(world.session.GetRegionSchedulerFault());
    W::GameSession loaded;
    world.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(world.session.GetMetadata().worldId))
        << loaded.GetWorldOpenError();
    EXPECT_EQ(Snapshot(loaded).encode(), expected);
}

TEST(SimulationPendingState, DurableAdmissionCountLimitPreservesRecoverySaveability) {
    PendingWorld world;
    auto& bus = world.session.GetSimulationEventBus();
    for (std::size_t i = 0; i < State::kMaxEvents; ++i)
        ASSERT_EQ(bus.publish(99, "", ""), static_cast<std::uint64_t>(i));
    const auto before = bus.snapshot();
    EXPECT_THROW(bus.publish(99, "refused", ""), std::length_error);
    EXPECT_EQ(bus.snapshot().events, before.events);
    EXPECT_EQ(bus.snapshot().next_sequence, before.next_sequence);
    const std::vector<S::SimulationEvent> extra{{99, 0, 0, "batch", ""}};
    bool prepared = false;
    EXPECT_THROW((void)bus.prepare_batch(extra, [&] { prepared = true; }), std::length_error);
    EXPECT_FALSE(prepared);
    EXPECT_EQ(bus.snapshot().events, before.events);
    EXPECT_EQ(bus.snapshot().next_sequence, before.next_sequence);
    ASSERT_NO_FATAL_FAILURE(CheckLimitRecovery(world));
}

TEST(SimulationPendingState, DurableAdmissionTextTickAndActivationBounds) {
    {
        PendingWorld world;
        auto& bus = world.session.GetSimulationEventBus();
        const auto before = bus.snapshot();
        EXPECT_THROW(bus.publish(W::WorldClock::kTickLimit, "", ""), std::invalid_argument);
        EXPECT_THROW(bus.publish(99, std::string(State::kMaxTextBytes + 1, 'x'), ""),
                     std::length_error);
        EXPECT_THROW(bus.publish(99, "", std::string(State::kMaxTextBytes + 1, 'x')),
                     std::length_error);
        {
            const std::vector<S::SimulationEvent> oversized_text{
                {99, 0, 0, std::string(State::kMaxTextBytes / 2, 'a'), ""},
                {99, 0, 0, "", std::string(State::kMaxTextBytes / 2 + 1, 'b')}};
            EXPECT_THROW((void)bus.prepare_batch(oversized_text), std::length_error);
        }
        EXPECT_EQ(bus.snapshot().events, before.events);
        EXPECT_EQ(bus.snapshot().next_sequence, before.next_sequence);
        for (const auto& event : std::vector<S::SimulationEvent>{
                 {W::WorldClock::kTickLimit, 0, 0, "", ""},
                 {99, 0, 0, "", "", S::ActivationDeliveryKey{98, 0, 0}},
                 {99, 0, 0, "", "", S::ActivationDeliveryKey{99, -32769, 0}},
                 {99, 0, 0, "", "", S::ActivationDeliveryKey{99, 0, 32768}}}) {
            const std::vector<S::SimulationEvent> batch{{99, 0, 0, "prefix", ""}, event};
            EXPECT_THROW((void)bus.prepare_batch(batch), std::invalid_argument);
            EXPECT_EQ(bus.snapshot().events, before.events);
            EXPECT_EQ(bus.snapshot().next_sequence, before.next_sequence);
        }
        ASSERT_EQ(bus.publish(W::WorldClock::kTickLimit - 1, "", ""), 0u);
        bus.drain_enabled(W::WorldClock::kTickLimit - 1, [] { return true; }, {});
        ASSERT_EQ(bus.publish(99, "x", std::string(State::kMaxTextBytes - 1, 'y')), 1u);
        EXPECT_THROW(bus.publish(99, "", "z"), std::length_error);
        const std::vector<S::SimulationEvent> extra{{99, 0, 0, "z", ""}};
        EXPECT_THROW((void)bus.prepare_batch(extra), std::length_error);
        EXPECT_EQ(bus.snapshot().next_sequence, 2u);
        ASSERT_EQ(bus.pending_count(), 1u);
        EXPECT_EQ(bus.snapshot().events.front().payload.size(), State::kMaxTextBytes - 1);
        ASSERT_NO_FATAL_FAILURE(CheckLimitRecovery(world));
    }
    // Text budget is released by delivery, ordinary drain, commit and clear.
    S::OrderedEventBus bus;
    bus.set_durable(true);
    bus.publish(1, "", std::string(State::kMaxTextBytes, 'x'));
    bus.drain_enabled(1, [] { return true; }, {});
    bus.publish(2, std::string(State::kMaxTextBytes, 'y'), "");
    bus.drain(2);
    const std::vector<S::SimulationEvent> batch{
        {3, 0, 0, "", std::string(State::kMaxTextBytes, 'z')}};
    bus.commit_batch(bus.prepare_batch(batch));
    EXPECT_THROW(bus.publish(4, "x", ""), std::length_error);
    bus.clear();
    EXPECT_EQ(bus.publish(4, "x", ""), 0u);
}

TEST(SimulationPendingState, ActivationReconciliationDefersAtCountAndTextLimits) {
    const std::vector<S::SimulationEvent> activation{{1,
                                                      std::numeric_limits<std::int32_t>::min(),
                                                      0,
                                                      "region.activated",
                                                      "0,0,pin",
                                                      S::ActivationDeliveryKey{1, 0, 0}}};
    for (const bool full_text : {false, true}) {
        S::OrderedEventBus bus;
        bus.set_durable(true);
        if (full_text)
            bus.publish(99, "", std::string(State::kMaxTextBytes, 'x'));
        else
            for (std::size_t i = 0; i < State::kMaxEvents; ++i)
                bus.publish(99, "", "");
        const auto before = bus.snapshot();
        auto deferred = bus.prepare_batch(activation, {}, true, true);
        bus.commit_batch(std::move(deferred));
        EXPECT_EQ(bus.snapshot().events, before.events);
        EXPECT_EQ(bus.snapshot().next_sequence, before.next_sequence);
        bus.drain_enabled(99, [] { return true; }, {});
        bus.commit_batch(bus.prepare_batch(activation, {}, true, true));
        const auto admitted = bus.snapshot();
        ASSERT_EQ(admitted.events.size(), 1u);
        EXPECT_EQ(admitted.events.front().activation, activation.front().activation);
        EXPECT_EQ(admitted.events.front().sequence, before.next_sequence);
        EXPECT_EQ(admitted.next_sequence, before.next_sequence + 1);
        // Repeated reconciliation preserves a delivery's allocated sequence.
        bus.commit_batch(bus.prepare_batch(activation, {}, true, true));
        EXPECT_EQ(bus.snapshot().events, admitted.events);
        EXPECT_EQ(bus.snapshot().next_sequence, admitted.next_sequence);
    }
}

void CheckQueueAccounting(const S::OrderedEventBus& bus) {
    const auto state = bus.snapshot();
    std::size_t text = 0;
    for (const auto& event : state.events)
        text += event.topic.size() + event.payload.size();
    EXPECT_EQ(state.text_bytes, text);
    EXPECT_EQ(bus.pending_count(), state.events.size());
}

TEST(SimulationPendingState, PreparedQueueAccountingVisitsOnlyBatchEvents) {
    S::OrderedEventBus bus;
    bus.set_durable(true);
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    for (std::size_t i = 0; i < 128; ++i) {
        bus.publish(100 + i, "future", std::string(i, 'x'));
        ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    }
    const auto future = bus.snapshot();
    for (std::uint64_t tick = 1; tick <= 8; ++tick) {
        bus.commit_batch(bus.prepare_batch({}, {}, true, true));
        EXPECT_EQ(bus.commit_events_scanned_for_testing(), 0u);
        ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
        EXPECT_TRUE(bus.drain_enabled(tick, [] { return true; }, {}).empty());
        ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
        EXPECT_EQ(bus.snapshot().events, future.events);
        EXPECT_EQ(bus.snapshot().next_sequence, future.next_sequence);
    }
    std::vector<S::SimulationEvent> batch{
        {20, 0, 0, "activation", "old", S::ActivationDeliveryKey{20, 1, 0}}};
    bus.commit_batch(bus.prepare_batch(batch));
    EXPECT_EQ(bus.commit_events_scanned_for_testing(), batch.size());
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    const auto admitted = bus.snapshot();
    bus.commit_batch(bus.prepare_batch(batch)); // duplicate key consumes no sequence or budget
    EXPECT_LE(bus.commit_events_scanned_for_testing(), batch.size());
    EXPECT_EQ(bus.snapshot().events, admitted.events);
    EXPECT_EQ(bus.snapshot().next_sequence, admitted.next_sequence);
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    bus.commit_batch(bus.prepare_batch(batch, {}, true, true));
    EXPECT_LE(bus.commit_events_scanned_for_testing(), 3 * batch.size());
    EXPECT_EQ(bus.snapshot().events, admitted.events);
    EXPECT_EQ(bus.snapshot().next_sequence, admitted.next_sequence);
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    batch.front().payload = "larger replacement";
    bus.commit_batch(bus.prepare_batch(batch, {}, true, true));
    EXPECT_LE(bus.commit_events_scanned_for_testing(), 3 * batch.size());
    EXPECT_EQ(bus.snapshot().next_sequence, admitted.next_sequence + 1);
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    const auto replaced = bus.snapshot();
    EXPECT_THROW((void)bus.prepare_batch(
                     batch, [] { throw std::runtime_error("Refused batch"); }, true),
                 std::runtime_error);
    EXPECT_EQ(bus.snapshot().events, replaced.events);
    EXPECT_EQ(bus.snapshot().next_sequence, replaced.next_sequence);
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    bus.commit_batch(bus.prepare_batch({}, {}, true, true)); // remove the activation only
    EXPECT_LE(bus.commit_events_scanned_for_testing(), 2u);
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    EXPECT_EQ(bus.snapshot().events, future.events);
    bus.publish(1, "due", "text");
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    bool fail = true;
    bus.subscribe([&](const auto&) {
        if (fail)
            throw std::runtime_error("Retain due event");
        bus.publish(300, "handler", "insert");
    });
    const auto retained = bus.snapshot();
    EXPECT_THROW(bus.drain_enabled(1, [] { return true; }, {}), std::runtime_error);
    EXPECT_EQ(bus.snapshot().events, retained.events);
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    fail = false;
    ASSERT_EQ(bus.drain_enabled(1, [] { return true; }, {}).size(), 1u);
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    bus.drain(100, [] { return false; });
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    auto restored = S::OrderedEventBus::prepare_restore(bus.snapshot());
    bus.clear();
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    bus.commit_batch(std::move(restored));
    EXPECT_EQ(bus.commit_events_scanned_for_testing(), bus.pending_count());
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    S::OrderedEventBus candidate;
    candidate.set_durable(true);
    auto transfer = bus.release_pending();
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    candidate.commit_batch(std::move(transfer));
    ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(candidate));
    EXPECT_GT(candidate.pending_count(), 0u);
}

TEST(SimulationPendingState, FutureQueueAccountingDoesNotScanOnSessionTicks) {
    PendingWorld world;
    world.session.SetReplicatedSimulationAnchors({});
    ASSERT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(world.session.GetRegionSchedulerFault());
    auto& bus = world.session.GetSimulationEventBus();
    for (std::size_t i = 0; i < 128; ++i) {
        bus.publish(1000 + i, "future", std::string(i, 'x'));
        ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    }
    const auto before = bus.snapshot();
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(world.session.GetRegionSchedulerFault());
        EXPECT_EQ(bus.commit_events_scanned_for_testing(), 0u);
        EXPECT_EQ(bus.snapshot().events, before.events);
        EXPECT_EQ(bus.snapshot().next_sequence, before.next_sequence);
        ASSERT_NO_FATAL_FAILURE(CheckQueueAccounting(bus));
    }
}

TEST(SimulationPendingState, FailedDeliveryRetainsFailedEventAndSuffix) {
    PendingWorld world;
    auto& bus = world.session.GetSimulationEventBus();
    bus.publish(1, "first", "a");
    bus.publish(1, "middle", "b");
    bus.publish(1, "last", "c");
    bus.publish(99, "future", "d");
    std::vector<std::string> observed;
    bus.subscribe([&](const auto& event) {
        if (event.topic == "first" || event.topic == "middle" || event.topic == "last") {
            observed.push_back(event.topic);
            if (event.topic == "middle")
                throw std::runtime_error("Injected delivery failure");
        }
    });
    EXPECT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(world.session.GetRegionSchedulerFault());
    EXPECT_EQ(observed, (std::vector<std::string>{"first", "middle"}));
    const auto pending = Snapshot(world.session);
    ASSERT_EQ(pending.events.events.size(), 3u);
    EXPECT_EQ(pending.events.events[0].topic, "middle");
    EXPECT_EQ(pending.events.events[1].topic, "last");
    EXPECT_EQ(pending.events.events[2].topic, "future");
    const auto tick = world.session.GetWorldClock().tick();
    EXPECT_EQ(world.session.TickSimulation(1.0), 0u);
    EXPECT_EQ(world.session.GetWorldClock().tick(), tick);
    ASSERT_TRUE(world.session.SaveWorldState());
    W::GameSession loaded;
    world.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(world.session.GetMetadata().worldId));
    EXPECT_EQ(Snapshot(loaded).encode(), pending.encode());
    std::vector<std::string> resumed;
    loaded.GetSimulationEventBus().subscribe(
        [&](const auto& event) { resumed.push_back(event.topic); });
    const auto delivered = loaded.GetSimulationEventBus().drain_enabled(1, [] { return true; }, {});
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(resumed, (std::vector<std::string>{"middle", "last"}));
    EXPECT_EQ(loaded.GetSimulationEventBus().pending_count(), 1u);
    // A callback that stops delivery also leaves its event and suffix unacknowledged.
    S::OrderedEventBus interrupted;
    interrupted.publish(1, "stop", "");
    interrupted.publish(1, "suffix", "");
    bool proceed = true;
    interrupted.subscribe([&](const auto&) { proceed = false; });
    EXPECT_TRUE(interrupted.drain_enabled(1, [&] { return proceed; }, {}).empty());
    EXPECT_EQ(interrupted.pending_count(), 2u);
}

TEST(SimulationPendingState, RetainedEnergyDepositsSurviveFaultSaveLoad) {
    PendingWorld world;
    world.session.PinActiveRegion({0, 0}, true);
    auto* energy = world.session.GetEnergyFieldState();
    ASSERT_NE(energy, nullptr);
    for (unsigned i = 0; i < 3; ++i)
        ASSERT_TRUE(energy->QueueRegionalDeposit(42, 0, 0, 1, 200));
    const auto deposits = energy->PendingDeposits();
    luminumbra::fields::EnergyFieldState::RegionalResult refused;
    const std::vector<W::RegionKey> invalid_due{{0, 0}, {0, 0}};
    EXPECT_FALSE(energy->TickRegional(1,
                                      world.session.GetActiveRegionLedger().records(),
                                      invalid_due,
                                      world.session.GetActiveRegionLedger().energy_ages(),
                                      refused));
    EXPECT_EQ(energy->PendingDeposits(), deposits);
    world.session.SetActivationEnqueuePreparationHook(
        [] { throw std::runtime_error("Injected enqueue failure"); });
    EXPECT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(world.session.GetRegionSchedulerFault());
    EXPECT_EQ(energy->PendingDeposits(), deposits);
    const auto capture = world.session.CaptureAuthorityState();
    ASSERT_TRUE(capture.state) << capture.error;
    ASSERT_TRUE(world.session.SaveWorldState());
    const auto expected = Snapshot(world.session).encode();
    EXPECT_EQ(energy->PendingDeposits(), deposits);
    W::GameSession loaded;
    world.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(world.session.GetMetadata().worldId))
        << loaded.GetWorldOpenError();
    EXPECT_EQ(Snapshot(loaded).encode(), expected);
    ASSERT_NE(loaded.GetEnergyFieldState(), nullptr);
    EXPECT_EQ(loaded.GetEnergyFieldState()->PendingDeposits(), deposits);
    const auto restored = loaded.CaptureAuthorityState();
    ASSERT_TRUE(restored.state) << restored.error;
    EXPECT_EQ(restored.state->authority_bytes(), capture.state->authority_bytes());
    // Multiplicity contributes to pages even when the stored energy layer is empty.
    auto positive = Snapshot(loaded);
    positive.deposits.pop_back();
    EXPECT_NE(positive.deposit_bytes(), restored.state->pending_deposit_bytes());
    ASSERT_TRUE(loaded.GetEnergyFieldState()->RestorePendingDeposits(positive.deposits));
    const auto changed = loaded.CaptureAuthorityState();
    ASSERT_TRUE(changed.state) << changed.error;
    EXPECT_NE(changed.state->authority_bytes(), restored.state->authority_bytes());
    EXPECT_NE(changed.state->energy_bytes(), restored.state->energy_bytes());
    ASSERT_TRUE(loaded.GetEnergyFieldState()->RestorePendingDeposits(deposits));
    EXPECT_EQ(loaded.GetEnergyFieldState()->at_cell(0, 0, 1), 0u);
    EXPECT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(loaded.GetRegionSchedulerFault());
    EXPECT_TRUE(loaded.GetEnergyFieldState()->PendingDeposits().empty());
    EXPECT_EQ(loaded.GetEnergyFieldState()->at_cell(0, 0, 1), 600u);
    // Canonical deposit order includes negative packed keys, amount tie-breaking,
    // and duplicate entries; input order cannot change saved authority.
    luminumbra::fields::EnergyFieldState gathered(2);
    ASSERT_TRUE(gathered.QueueRegionalDeposit(9, 1, 0, 0, 8));
    ASSERT_TRUE(gathered.QueueRegionalDeposit(9, -1, 0, 1, 5));
    ASSERT_TRUE(gathered.QueueRegionalDeposit(9, -1, 0, 1, 3));
    ASSERT_TRUE(gathered.QueueRegionalDeposit(9, -1, 0, 1, 3));
    State unordered;
    unordered.energy_channels = 2;
    unordered.deposits = gathered.PendingDeposits();
    State canonical;
    std::string error;
    const auto encoded = unordered.encode();
    ASSERT_TRUE(State::decode(encoded, canonical, error)) << error;
    ASSERT_EQ(canonical.deposits.size(), 4u);
    EXPECT_LT(canonical.deposits[0].cell_key, 0);
    EXPECT_EQ(canonical.deposits[0].amount, 3u);
    EXPECT_EQ(canonical.deposits[0], canonical.deposits[1]);
    EXPECT_EQ(canonical.deposits[2].amount, 5u);
    EXPECT_EQ(canonical.deposits[3].amount, 8u);
    std::reverse(unordered.deposits.begin(), unordered.deposits.end());
    EXPECT_EQ(unordered.encode(), encoded);
    auto bad_channel = encoded;
    const auto deposit_offset = 8 + std::string_view("pending_events:v1:").size() + 12 +
                                std::string_view("pending_deposits:v1:").size() + 8;
    SetInteger(bad_channel, deposit_offset + 8, 2, 4);
    Reseal(bad_channel);
    EXPECT_FALSE(State::decode(bad_channel, canonical, error));
    EXPECT_EQ(canonical.encode(), encoded);
    auto bad_count = encoded;
    SetInteger(bad_count,
               deposit_offset - 4,
               luminumbra::fields::EnergyFieldState::kMaxRegionalDeposits + 1,
               4);
    Reseal(bad_count);
    EXPECT_FALSE(State::decode(bad_count, canonical, error));
    EXPECT_EQ(canonical.encode(), encoded);
}

TEST(SimulationPendingState, PendingDigestDescribesTheValidatedRead) {
    PendingWorld world;
    ASSERT_TRUE(world.session.SaveWorldState());
    const auto dir = world.session.GetWorldSaveDir();
    const auto member = dir / State::kMember;
    world.session.GetSimulationEventBus().publish(99, "future", "replacement authority");
    const auto bytes = Snapshot(world.session).encode();
    static fs::path selected;
    static std::string replacement;
    selected = member;
    replacement = bytes;
    const HookGuard guard({});
    P::set_snapshot_step_hook_for_testing(+[](const char* step, const fs::path& path) {
        if (std::string_view(step) == "hash-opened" && path == selected) {
            P::set_snapshot_step_hook_for_testing(nullptr);
            EXPECT_TRUE(P::WorldSaveService::write_file_durably(path, replacement));
            selected.clear();
            replacement.clear();
        }
    });
    P::MemberDigest digest;
    std::string error;
    ASSERT_TRUE(P::WorldSaveService::member_digest(dir, State::kMember, digest, error)) << error;
    EXPECT_TRUE(selected.empty());
    EXPECT_TRUE(digest.present);
    EXPECT_EQ(digest.bytes, bytes.size());
    EXPECT_EQ(digest.fnv64, P::fnv1a64_bytes(bytes.data(), bytes.size()));
}

TEST(SimulationPendingState, PendingReaderRefusesUnsafeFilesBeforeDecode) {
    PendingWorld world;
    ASSERT_TRUE(world.session.SaveWorldState());
    const auto dir = world.session.GetWorldSaveDir();
    const auto member = dir / State::kMember;
    const auto valid = Read(member);
    const auto before = Snapshot(world.session).encode();
    const auto refuse_without_decode = [&] {
        const auto decodes = State::decode_count_for_testing();
        EXPECT_FALSE(world.session.LoadWorld(world.session.GetMetadata().worldId));
        EXPECT_EQ(State::decode_count_for_testing(), decodes);
        EXPECT_EQ(Snapshot(world.session).encode(), before);
    };
    ASSERT_TRUE(fs::remove(member));
    ASSERT_TRUE(fs::create_directory(member));
    refuse_without_decode();
    ASSERT_TRUE(fs::remove(member));
    Write(member, valid);
    fs::resize_file(member, State::kMaxFileBytes + 1);
    refuse_without_decode();
    ASSERT_TRUE(fs::remove(member));
#ifndef _WIN32
    // POSIX links need no elevation. Windows denial of link creation is not a reader test.
    const auto target = world.root / "pending-target";
    Write(target, valid);
    fs::create_symlink(target, member);
    refuse_without_decode();
    ASSERT_TRUE(fs::remove(member));
#endif
    Write(member, valid.substr(0, valid.size() - 1));
    EXPECT_FALSE(world.session.LoadWorld(world.session.GetMetadata().worldId));
    EXPECT_EQ(Snapshot(world.session).encode(), before);
    Write(member, valid);
    EXPECT_TRUE(world.session.LoadWorld(world.session.GetMetadata().worldId));
}

TEST(SimulationPendingState, PendingStateMissingAdoptsPresentMalformedRefuses) {
    PendingWorld world;
    // A historical member absence derives activation deliveries from the ledger outbox.
    world.session.PinActiveRegion({10, 0}, true);
    world.session.SetReplicatedSimulationAnchors({});
    world.session.SetActivationEnqueuePreparationHook(
        [] { throw std::runtime_error("Historical outbox fault"); });
    ASSERT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(world.session.GetRegionSchedulerFault());
    ASSERT_EQ(world.session.GetActiveRegionLedger().pending_activations().size(), 1u);
    world.session.GetSimulationEventBus().publish(9, "historical", "not recoverable when absent");
    ASSERT_TRUE(world.session.SaveWorldState());
    const auto dir = world.session.GetWorldSaveDir();
    ASSERT_TRUE(fs::remove(dir / State::kMember));
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(dir, index, nullptr));
    std::erase_if(index.members, [](const auto& member) { return member.path == State::kMember; });
    Write(P::snapshot_index_path(dir), P::encode_snapshot_index(index));
    const auto historical = Tree(dir);
    W::GameSession adopted;
    world.Configure(adopted);
    ASSERT_TRUE(adopted.LoadWorld(world.session.GetMetadata().worldId))
        << adopted.GetWorldOpenError();
    EXPECT_EQ(Tree(dir), historical);
    EXPECT_EQ(adopted.GetWorldClock().tick(), world.session.GetWorldClock().tick());
    EXPECT_EQ(adopted.GetActiveRegionLedger().canonical_bytes(),
              world.session.GetActiveRegionLedger().canonical_bytes());
    auto adopted_state = Snapshot(adopted);
    ASSERT_EQ(adopted_state.events.events.size(), 1u);
    EXPECT_EQ(adopted_state.events.events[0].topic, "region.activated");
    EXPECT_TRUE(adopted_state.events.events[0].activation.has_value());
    EXPECT_TRUE(adopted_state.deposits.empty());
    ASSERT_TRUE(adopted.SaveWorldState());
    EXPECT_TRUE(fs::exists(dir / State::kMember));
    ASSERT_TRUE(P::read_snapshot_index(dir, index, nullptr));
    EXPECT_TRUE(std::any_of(index.members.begin(), index.members.end(), [](const auto& member) {
        return member.path == State::kMember && member.present &&
               member.cls == P::MemberClass::Authoritative;
    }));
    const auto valid_tree = Tree(dir);
    const auto valid = Read(dir / State::kMember);
    std::vector<std::string> invalid{"", "malformed", valid.substr(0, valid.size() - 1)};
    auto version = valid;
    SetInteger(version, 4, 99, 4);
    Reseal(version);
    invalid.push_back(version);
    auto count = valid;
    SetInteger(
        count, 8 + std::string_view("pending_events:v1:").size() + 8, State::kMaxEvents + 1, 4);
    Reseal(count);
    invalid.push_back(count);
    auto text = valid;
    SetInteger(text,
               8 + std::string_view("pending_events:v1:").size() + 12 + 20,
               State::kMaxTextBytes + 1,
               4);
    Reseal(text);
    invalid.push_back(text);
    auto trailing = valid;
    trailing.insert(trailing.size() - 8, "x");
    Reseal(trailing);
    invalid.push_back(trailing);
    for (const auto& bytes : invalid) {
        Write(dir / State::kMember, bytes);
        ReindexMember(dir); // Valid fingerprints must not conceal malformed contents.
        const auto bad_tree = Tree(dir);
        const auto live = Snapshot(adopted).encode();
        const auto ledger = adopted.GetActiveRegionLedger().canonical_bytes();
        EXPECT_FALSE(adopted.LoadWorld(world.session.GetMetadata().worldId));
        EXPECT_EQ(Snapshot(adopted).encode(), live);
        EXPECT_EQ(adopted.GetActiveRegionLedger().canonical_bytes(), ledger);
        EXPECT_EQ(Tree(dir), bad_tree);
        State output = adopted_state;
        std::string error;
        EXPECT_FALSE(State::decode(bytes, output, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(output.encode(), adopted_state.encode());
    }
    W::GameSession direct;
    world.Configure(direct);
    ASSERT_TRUE(direct.CreateWorld("Direct pending refusal", "1337", "default"));
    const auto direct_pending = Snapshot(direct).encode();
    const auto direct_ledger = direct.GetActiveRegionLedger().canonical_bytes();
    const auto direct_clock = direct.GetWorldClock().tick();
    const auto direct_chunks = direct.GetWorldSystem()->snapshot_streamed_chunks();
    EXPECT_FALSE(direct.LoadWorldStateFrom(dir));
    EXPECT_FALSE(direct.GetWorldOpenError().empty());
    EXPECT_EQ(Snapshot(direct).encode(), direct_pending);
    EXPECT_EQ(direct.GetActiveRegionLedger().canonical_bytes(), direct_ledger);
    EXPECT_EQ(direct.GetWorldClock().tick(), direct_clock);
    EXPECT_EQ(direct.GetWorldSystem()->snapshot_streamed_chunks(), direct_chunks);
    EXPECT_FALSE(direct.SaveWorldState());
    auto wrong_activation = valid;
    const auto topic_offset = wrong_activation.find("region.activated");
    ASSERT_NE(topic_offset, std::string::npos);
    wrong_activation[topic_offset] = 'x';
    Reseal(wrong_activation);
    Write(dir / State::kMember, wrong_activation);
    ReindexMember(dir);
    const auto live_pending = Snapshot(adopted).encode();
    EXPECT_FALSE(adopted.LoadWorld(world.session.GetMetadata().worldId));
    EXPECT_EQ(Snapshot(adopted).encode(), live_pending);
    // Listed-present missing and unlisted-present files retain strict membership refusal.
    Write(dir / State::kMember, valid);
    Write(P::snapshot_index_path(dir), valid_tree.at("chunks/region/snapshot.json"));
    fs::remove(dir / State::kMember);
    EXPECT_FALSE(adopted.LoadWorld(world.session.GetMetadata().worldId));
    Write(dir / State::kMember, valid);
    ASSERT_TRUE(P::read_snapshot_index(dir, index, nullptr));
    std::erase_if(index.members, [](const auto& member) { return member.path == State::kMember; });
    Write(P::snapshot_index_path(dir), P::encode_snapshot_index(index));
    EXPECT_FALSE(adopted.LoadWorld(world.session.GetMetadata().worldId));
    // Oversized files refuse before reading/allocating their payload.
    Write(P::snapshot_index_path(dir), valid_tree.at("chunks/region/snapshot.json"));
    fs::resize_file(dir / State::kMember, State::kMaxFileBytes + 1);
    P::MemberDigest digest;
    std::string error;
    EXPECT_FALSE(P::WorldSaveService::member_digest(dir, State::kMember, digest, error));
    EXPECT_FALSE(error.empty());
    // Rejected public replacement leaves the current world saveable elsewhere.
    Write(dir / State::kMember, valid);
    const auto alternate = world.root / "alternate";
    ASSERT_TRUE(adopted.SaveWorldStateTo(alternate));
    EXPECT_EQ(Snapshot(adopted).encode(), adopted_state.encode());
}

TEST(SimulationPendingState, AboveCapacityHistoricalOutboxAdoptsAndDeliversExactlyOnce) {
    PendingWorld world;
    world.session.SetReplicatedSimulationAnchors({});
    world.session.PinActiveRegion({10, 0}, true);
    ASSERT_FALSE(world.session.GetRegionSchedulerFault());
    world.session.SetActivationEnqueuePreparationHook(
        [] { throw std::runtime_error("Retain historical activation"); });
    ASSERT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(world.session.GetRegionSchedulerFault());
    ASSERT_EQ(world.session.GetActiveRegionLedger().pending_activations().size(), 1u);
    ASSERT_TRUE(world.session.SaveWorldState());
    const auto dir = world.session.GetWorldSaveDir();
    constexpr auto count = State::kMaxEvents + 3;
    // Build a valid historical ledger with many retained activations for one
    // owner, avoiding thousands of unrelated regions and simulated consumers.
    auto bytes = world.session.GetActiveRegionLedger().encode();
    const auto extensions = W::ActiveRegionLedger::kHeaderBytes +
                            W::ActiveRegionLedger::kRecordBytes *
                                world.session.GetActiveRegionLedger().records().size();
    const auto activation_header = bytes.size() - 8 - 6 - (8 + 17);
    ASSERT_EQ(static_cast<unsigned char>(bytes.at(activation_header)), 7u);
    bytes.resize(activation_header + 6 + 8 + 17 * count + 8, '\0');
    SetInteger(bytes, 8, count, 8);  // Canonical scheduler tick.
    SetInteger(bytes, 16, count, 8); // Validation ceiling.
    SetInteger(bytes, extensions, bytes.size() - extensions - 4 - 8, 4);
    SetInteger(bytes, activation_header + 2, 8 + 17 * count, 4);
    SetInteger(bytes, activation_header + 6, 0, 4); // No activation intents.
    SetInteger(bytes, activation_header + 10, count, 4);
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = activation_header + 14 + 17 * i;
        SetInteger(bytes, offset, i + 1, 8);
        SetInteger(bytes, offset + 8, 10, 4);
        SetInteger(bytes, offset + 12, 0, 4);
        SetInteger(bytes, offset + 16, static_cast<std::uint8_t>(W::ActivationCause::Pin), 1);
    }
    Reseal(bytes);
    W::ActiveRegionLedger historical_ledger;
    std::string error;
    ASSERT_TRUE(W::ActiveRegionLedger::decode(bytes, historical_ledger, error)) << error;
    ASSERT_EQ(historical_ledger.pending_activations().size(), count);
    State tail;
    tail.events.events.push_back(
        {static_cast<std::uint64_t>(count),
         std::numeric_limits<std::int32_t>::min(),
         0,
         "region.activated",
         "10,0,pin",
         S::ActivationDeliveryKey{static_cast<std::uint64_t>(count), 10, 0}});
    tail.events.next_sequence = 1;
    EXPECT_TRUE(tail.validate_activations(historical_ledger));
    Write(P::WorldSaveService::active_regions_path(dir), bytes);
    auto metadata = nlohmann::json::parse(Read(dir / "world_info.json"));
    W::WorldClock(static_cast<std::uint64_t>(count)).write_metadata(metadata);
    Write(dir / "world_info.json", metadata.dump(2));
    ASSERT_NO_FATAL_FAILURE(ReindexMember(dir, "chunks/region/active-regions.arl"));
    ASSERT_NO_FATAL_FAILURE(ReindexMember(dir, "world_info.json"));
    ASSERT_TRUE(fs::remove(dir / State::kMember));
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(dir, index, nullptr));
    index.tick = static_cast<std::uint64_t>(count);
    std::erase_if(index.members, [](const auto& member) { return member.path == State::kMember; });
    Write(P::snapshot_index_path(dir), P::encode_snapshot_index(index));
    const auto historical = Tree(dir);
    W::GameSession adopted;
    world.Configure(adopted);
    ASSERT_TRUE(adopted.LoadWorld(world.session.GetMetadata().worldId))
        << adopted.GetWorldOpenError();
    EXPECT_EQ(Tree(dir), historical);
    EXPECT_EQ(adopted.GetWorldClock().tick(), count);
    EXPECT_EQ(adopted.GetActiveRegionLedger().canonical_bytes(),
              historical_ledger.canonical_bytes());
    EXPECT_EQ(adopted.GetActiveRegionLedger().pending_activations().size(), count);
    EXPECT_EQ(adopted.GetSimulationEventBus().pending_count(), State::kMaxEvents);
    EXPECT_EQ(adopted.GetSimulationEventBus().snapshot().next_sequence, State::kMaxEvents);
    const auto capture = adopted.CaptureAuthorityState();
    ASSERT_TRUE(capture.state) << capture.error;
    ASSERT_NO_FATAL_FAILURE(SaveWithDecodeBudget(adopted, dir, 0, 1));
    W::GameSession resumed;
    world.Configure(resumed);
    ASSERT_TRUE(resumed.LoadWorld(world.session.GetMetadata().worldId))
        << resumed.GetWorldOpenError();
    EXPECT_EQ(Snapshot(resumed).encode(), Snapshot(adopted).encode());
    resumed.SetReplicatedSimulationAnchors({});
    std::vector<S::SimulationEvent> delivered;
    resumed.GetSimulationEventBus().subscribe(
        [&](const auto& event) { delivered.push_back(event); });
    ASSERT_EQ(resumed.TickSimulation(resumed.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(resumed.GetRegionSchedulerFault());
    EXPECT_EQ(delivered.size(), State::kMaxEvents);
    EXPECT_EQ(resumed.GetActiveRegionLedger().pending_activations().size(), 3u);
    ASSERT_EQ(resumed.TickSimulation(resumed.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(resumed.GetRegionSchedulerFault());
    ASSERT_EQ(delivered.size(), count);
    for (std::size_t i = 0; i < delivered.size(); ++i) {
        EXPECT_EQ(delivered[i].activation, (S::ActivationDeliveryKey{i + 1, 10, 0}));
        EXPECT_EQ(delivered[i].sequence, i);
        EXPECT_EQ(delivered[i].topic, "region.activated");
        EXPECT_EQ(delivered[i].payload, "10,0,pin");
    }
    EXPECT_TRUE(resumed.GetActiveRegionLedger().pending_activations().empty());
    EXPECT_EQ(resumed.GetSimulationEventBus().pending_count(), 0u);
    ASSERT_EQ(resumed.TickSimulation(resumed.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(resumed.GetRegionSchedulerFault());
    EXPECT_EQ(delivered.size(), count);
    ASSERT_NO_FATAL_FAILURE(SaveWithDecodeBudget(resumed, dir, 1, 1));
    W::GameSession complete;
    world.Configure(complete);
    ASSERT_TRUE(complete.LoadWorld(world.session.GetMetadata().worldId));
    EXPECT_TRUE(complete.GetActiveRegionLedger().pending_activations().empty());
    EXPECT_EQ(complete.GetSimulationEventBus().pending_count(), 0u);
    EXPECT_EQ(complete.GetSimulationEventBus().snapshot().next_sequence, count);
}

TEST(SimulationPendingState, StandaloneFirstPendingPublicationIsIndexed) {
    PendingWorld world(false);
    State pending;
    pending.events.next_sequence = 8;
    pending.events.events.push_back({99, -1, 7, "first", "pending"});
    for (const bool raw : {false, true}) {
        SCOPED_TRACE(raw ? "raw" : "typed");
        const auto dir = world.root / (raw ? "raw-legacy" : "typed-legacy");
        ASSERT_TRUE(world.session.SaveWorldStateTo(dir));
        ASSERT_FALSE(P::snapshot_index_exists(dir));
        const auto baseline = Tree(dir);
        std::vector<std::string> errors;
        const auto publish = [&](const fs::path& destination) {
            return raw ? P::WorldSaveService::write_member_bytes(
                             destination, State::kMember, pending.encode(), &errors)
                       : P::WorldSaveService::save_pending_state(pending, destination, &errors);
        };
        if (raw) {
            EXPECT_FALSE(
                P::WorldSaveService::write_member_bytes(dir, State::kMember, "malformed", &errors));
            ASSERT_FALSE(errors.empty());
            EXPECT_NE(errors.front().find("simulation-pending.spn"), std::string::npos);
            EXPECT_EQ(Tree(dir), baseline);
            EXPECT_FALSE(P::snapshot_index_exists(dir));
            EXPECT_FALSE(P::journal_pending(dir));
            errors.clear();
        }
        const auto interrupted = world.root / (raw ? "raw-interrupted" : "typed-interrupted");
        {
            HookGuard guard({dir, interrupted, true, true, false, false});
            if (raw) {
                EXPECT_THROW(publish(dir), std::runtime_error);
            } else
                EXPECT_FALSE(publish(dir));
            EXPECT_TRUE(Hook().seen);
        }
        if (!raw) {
            EXPECT_FALSE(errors.empty());
        }
        errors.clear();
        EXPECT_EQ(Tree(dir), baseline);
        EXPECT_FALSE(P::journal_pending(dir));
        ASSERT_TRUE(P::journal_pending(interrupted));
        EXPECT_EQ(Read(interrupted / State::kMember), pending.encode());
        EXPECT_FALSE(P::snapshot_index_exists(interrupted));
        EXPECT_EQ(P::recover_pending_journal(interrupted, nullptr), P::RecoveryOutcome::RolledBack);
        EXPECT_EQ(Tree(interrupted), baseline);
        EXPECT_EQ(P::recover_pending_journal(interrupted, nullptr),
                  P::RecoveryOutcome::NothingToDo);
        Luminumbra::WorldStreamingState loaded;
        State restored;
        bool present = true;
        ASSERT_TRUE(P::WorldSaveService{}.load_world(
            loaded, interrupted, errors, {}, {}, nullptr, &restored, &present, true));
        EXPECT_TRUE(errors.empty());
        EXPECT_FALSE(present); // Recovered legacy absence still adopts successfully.
        const auto committed = world.root / (raw ? "raw-committed" : "typed-committed");
        const auto decodes = State::decode_count_for_testing();
        {
            HookGuard guard({dir, committed, true, false, true, false});
            ASSERT_TRUE(publish(dir));
            EXPECT_TRUE(Hook().seen);
        }
        EXPECT_EQ(State::decode_count_for_testing() - decodes, raw ? 1u : 0u);
        EXPECT_TRUE(errors.empty());
        ASSERT_TRUE(P::snapshot_index_exists(dir));
        EXPECT_FALSE(P::journal_pending(dir));
        P::SnapshotIndex index;
        ASSERT_TRUE(P::read_snapshot_index(dir, index, nullptr));
        const auto member =
            std::find_if(index.members.begin(), index.members.end(), [](const auto& row) {
                return row.path == State::kMember;
            });
        ASSERT_NE(member, index.members.end());
        EXPECT_TRUE(member->present);
        EXPECT_EQ(member->cls, P::MemberClass::Authoritative);
        EXPECT_EQ(member->bytes, pending.encode().size());
        EXPECT_EQ(member->fnv64,
                  P::fnv1a64_bytes(pending.encode().data(), pending.encode().size()));
        ASSERT_TRUE(P::journal_pending(committed));
        EXPECT_EQ(P::recover_pending_journal(committed, nullptr),
                  P::RecoveryOutcome::DiscardedStale);
        EXPECT_EQ(Tree(committed), Tree(dir));
        EXPECT_EQ(P::recover_pending_journal(committed, nullptr), P::RecoveryOutcome::NothingToDo);
        // Both a new save and its recovered committed copy support enabled loading.
        for (const auto& destination : {dir, committed}) {
            ASSERT_TRUE(P::WorldSaveService{}.load_world(
                loaded, destination, errors, {}, {}, nullptr, &restored, &present, true));
            EXPECT_TRUE(errors.empty());
            EXPECT_TRUE(present);
            EXPECT_EQ(restored.encode(), pending.encode());
        }
    }
}

TEST(SimulationPendingState, FirstLedgerWritersRequireIndexedTransactions) {
    PendingWorld world(false);
    for (const bool raw : {false, true}) {
        SCOPED_TRACE(raw ? "raw" : "allocator-only");
        const auto dir = world.root / (raw ? "raw-ledger" : "allocator-ledger");
        ASSERT_TRUE(world.session.SaveWorldStateTo(dir));
        ASSERT_FALSE(P::snapshot_index_exists(dir));
        const auto baseline = Tree(dir);
        W::ActiveRegionLedger ledger;
        ASSERT_TRUE(P::WorldSaveService::save_active_regions(ledger, W::WorldClock{}, dir));
        EXPECT_EQ(Tree(dir), baseline);
        EXPECT_FALSE(P::snapshot_index_exists(dir));
        if (raw)
            ledger.set_local_anchor({0.0, 0.0, 0.0});
        else {
            // Allocation alone writes a ledger even with no records or anchor.
            std::uint64_t id = 0;
            ASSERT_TRUE(ledger.allocate_persistent_id(id));
            EXPECT_EQ(id, 1u);
        }
        const auto caller_ledger = ledger.encode();
        std::vector<std::string> errors;
        const auto publish = [&] {
            return raw ? P::WorldSaveService::write_member_bytes(
                             dir, "chunks/region/active-regions.arl", ledger.encode(), &errors)
                       : P::WorldSaveService::save_active_regions(
                             ledger, W::WorldClock{}, dir, &errors);
        };
        const auto interrupted =
            world.root / (raw ? "raw-ledger-interrupted" : "allocator-interrupted");
        {
            HookState state{dir, interrupted, true, true, false, false};
            state.publication_member = "active-regions.arl";
            HookGuard guard(std::move(state));
            if (raw) {
                EXPECT_THROW(publish(), std::runtime_error);
            } else
                EXPECT_FALSE(publish());
            EXPECT_TRUE(Hook().seen);
        }
        if (!raw) {
            EXPECT_FALSE(errors.empty());
        }
        errors.clear();
        EXPECT_EQ(Tree(dir), baseline);
        EXPECT_EQ(ledger.encode(), caller_ledger);
        EXPECT_FALSE(P::journal_pending(dir));
        ASSERT_TRUE(P::journal_pending(interrupted));
        EXPECT_TRUE(fs::exists(P::WorldSaveService::active_regions_path(interrupted)));
        EXPECT_EQ(P::recover_pending_journal(interrupted, nullptr), P::RecoveryOutcome::RolledBack);
        EXPECT_EQ(Tree(interrupted), baseline);
        EXPECT_EQ(P::recover_pending_journal(interrupted, nullptr),
                  P::RecoveryOutcome::NothingToDo);
        ASSERT_TRUE(publish());
        EXPECT_TRUE(errors.empty());
        ASSERT_TRUE(P::snapshot_index_exists(dir));
        EXPECT_FALSE(P::journal_pending(dir));
        P::SnapshotIndex index;
        ASSERT_TRUE(P::read_snapshot_index(dir, index, nullptr));
        const auto member =
            std::find_if(index.members.begin(), index.members.end(), [](const auto& row) {
                return row.path == "chunks/region/active-regions.arl";
            });
        ASSERT_NE(member, index.members.end());
        EXPECT_TRUE(member->present);
        W::ActiveRegionLedger restored;
        ASSERT_TRUE(P::WorldSaveService::load_active_regions(restored, dir, &errors));
        EXPECT_EQ(restored.canonical_bytes(), ledger.canonical_bytes());
        Luminumbra::WorldStreamingState loaded;
        bool present = true;
        ASSERT_TRUE(P::WorldSaveService{}.load_world(
            loaded, dir, errors, {}, {}, nullptr, nullptr, &present, true));
        EXPECT_TRUE(errors.empty());
        EXPECT_FALSE(present);
    }
}

TEST(SimulationPendingState, RawAuthorityRemovalFromLegacyDestinationIsIndexed) {
    PendingWorld world(false);
    for (const auto member :
         {State::kMember, std::string_view("chunks/region/active-regions.arl")}) {
        SCOPED_TRACE(std::string(member));
        const auto dir =
            world.root / (member == State::kMember ? "remove-pending" : "remove-ledger");
        ASSERT_TRUE(world.session.SaveWorldStateTo(dir));
        // A disabled historical pending member can be present without an index.
        // A missing ledger is a no-op removal, but still requires a transaction.
        if (member == State::kMember)
            Write(dir / member, State{}.encode());
        ASSERT_FALSE(P::snapshot_index_exists(dir));
        std::vector<std::string> errors;
        ASSERT_TRUE(P::WorldSaveService::remove_member(dir, member, &errors));
        EXPECT_TRUE(errors.empty());
        EXPECT_FALSE(fs::exists(dir / member));
        ASSERT_TRUE(P::snapshot_index_exists(dir));
        EXPECT_FALSE(P::journal_pending(dir));
        P::SnapshotIndex index;
        ASSERT_TRUE(P::read_snapshot_index(dir, index, nullptr));
        const auto row = std::find_if(index.members.begin(),
                                      index.members.end(),
                                      [member](const auto& entry) { return entry.path == member; });
        if (member == State::kMember) {
            ASSERT_NE(row, index.members.end());
            EXPECT_FALSE(row->present);
        } else
            EXPECT_EQ(row, index.members.end()); // Never-present members need no index row.
        Luminumbra::WorldStreamingState loaded;
        bool present = true;
        ASSERT_TRUE(P::WorldSaveService{}.load_world(
            loaded, dir, errors, {}, {}, nullptr, nullptr, &present, true));
        EXPECT_TRUE(errors.empty());
        EXPECT_FALSE(present);
    }
}

TEST(SimulationPendingState, FirstLedgerValidatesPreviouslyDisabledPendingMember) {
    PendingWorld world(false);
    const auto dir = world.root / "legacy";
    ASSERT_TRUE(world.session.SaveWorldStateTo(dir));
    ASSERT_FALSE(P::snapshot_index_exists(dir));
    ASSERT_FALSE(fs::exists(P::WorldSaveService::active_regions_path(dir)));
    Write(dir / State::kMember, "malformed");
    const auto baseline = Tree(dir);
    W::ActiveRegionLedger ledger;
    ledger.set_local_anchor({0.0, 0.0, 0.0});
    const auto caller_ledger = ledger.encode();
    std::vector<std::string> raw_errors;
    const auto raw_decodes = State::decode_count_for_testing();
    EXPECT_FALSE(P::WorldSaveService::write_member_bytes(
        dir, "chunks/region/active-regions.arl", ledger.encode(), &raw_errors));
    EXPECT_FALSE(raw_errors.empty());
    EXPECT_EQ(State::decode_count_for_testing() - raw_decodes, 1u);
    EXPECT_EQ(Tree(dir), baseline);
    EXPECT_FALSE(P::snapshot_index_exists(dir));
    EXPECT_FALSE(P::journal_pending(dir));
    const auto decodes = State::decode_count_for_testing();
    std::vector<std::string> errors;
    EXPECT_FALSE(P::WorldSaveService::save_active_regions(ledger, W::WorldClock{}, dir, &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(State::decode_count_for_testing() - decodes, 1u);
    EXPECT_EQ(ledger.encode(), caller_ledger);
    EXPECT_EQ(Tree(dir), baseline);
    EXPECT_FALSE(P::snapshot_index_exists(dir));
    EXPECT_FALSE(P::journal_pending(dir));
    Luminumbra::WorldStreamingState loaded;
    errors.clear();
    ASSERT_TRUE(P::WorldSaveService{}.load_world(
        loaded, dir, errors, {}, {}, nullptr, nullptr, nullptr, false));
    EXPECT_TRUE(errors.empty());
    // A valid previously ignored member is decoded once, then reused when the
    // first index enumerates it and the ledger checks its final dependency.
    State pending;
    pending.events.next_sequence = 1;
    pending.events.events.push_back({99, 0, 0, "existing", "valid"});
    Write(dir / State::kMember, pending.encode());
    const auto valid_decodes = State::decode_count_for_testing();
    const auto ledger_loads = P::WorldSaveService::ledger_load_count_for_testing();
    ASSERT_TRUE(P::WorldSaveService::save_active_regions(ledger, W::WorldClock{}, dir));
    EXPECT_EQ(State::decode_count_for_testing() - valid_decodes, 1u);
    EXPECT_EQ(P::WorldSaveService::ledger_load_count_for_testing(), ledger_loads);
    State restored;
    bool present = false;
    ASSERT_TRUE(P::WorldSaveService{}.load_world(
        loaded, dir, errors, {}, {}, nullptr, &restored, &present, true));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(present);
    EXPECT_EQ(restored.encode(), pending.encode());
}

TEST(SimulationPendingState, IndexedFirstLedgerValidatesPendingIndexFacts) {
    PendingWorld world(false);
    const auto original = world.root / "indexed-legacy";
    ASSERT_TRUE(world.session.SaveWorldStateTo(original));
    State pending;
    pending.events.next_sequence = 1;
    pending.events.events.push_back({99, 0, 0, "existing", "valid"});
    ASSERT_TRUE(P::WorldSaveService::save_pending_state(pending, original));
    ASSERT_TRUE(P::snapshot_index_exists(original));
    ASSERT_FALSE(fs::exists(P::WorldSaveService::active_regions_path(original)));
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(original, index, nullptr));
    const auto member = std::find_if(index.members.begin(),
                                     index.members.end(),
                                     [](const auto& row) { return row.path == State::kMember; });
    ASSERT_NE(member, index.members.end());
    ASSERT_TRUE(member->present);
    ASSERT_EQ(member->bytes, pending.encode().size());

    for (const std::string mutation : {"deleted", "altered", "unlisted", "absent", "size"}) {
        SCOPED_TRACE(mutation);
        const auto dir = world.root / mutation;
        Copy(original, dir);
        auto changed_index = index;
        auto changed_member =
            std::find_if(changed_index.members.begin(),
                         changed_index.members.end(),
                         [](const auto& row) { return row.path == State::kMember; });
        ASSERT_NE(changed_member, changed_index.members.end());
        if (mutation == "deleted") {
            ASSERT_TRUE(fs::remove(dir / State::kMember));
        } else if (mutation == "altered") {
            auto altered = pending;
            altered.events.events.front().payload = "other";
            ASSERT_EQ(altered.encode().size(), pending.encode().size());
            Write(dir / State::kMember, altered.encode()); // Valid codec, mismatched fingerprint.
        } else {
            if (mutation == "unlisted")
                changed_index.members.erase(changed_member);
            else if (mutation == "absent") {
                changed_member->present = false;
                changed_member->bytes = 0;
                changed_member->fnv64 = 0;
            } else
                ++changed_member->bytes;
            Write(P::snapshot_index_path(dir), P::encode_snapshot_index(changed_index));
        }
        const auto baseline = Tree(dir);
        Luminumbra::WorldStreamingState loaded;
        std::vector<std::string> errors;
        ASSERT_TRUE(P::WorldSaveService{}.load_world(
            loaded, dir, errors, {}, {}, nullptr, nullptr, nullptr, false));
        ASSERT_TRUE(errors.empty());
        W::ActiveRegionLedger ledger;
        ledger.set_local_anchor({0.0, 0.0, 0.0});
        const auto caller_ledger = ledger.encode();
        const auto decodes = State::decode_count_for_testing();
        EXPECT_FALSE(
            P::WorldSaveService::save_active_regions(ledger, W::WorldClock{}, dir, &errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_LE(State::decode_count_for_testing() - decodes, 1u);
        EXPECT_EQ(ledger.encode(), caller_ledger);
        EXPECT_EQ(Tree(dir), baseline);
        EXPECT_FALSE(P::journal_pending(dir));
        EXPECT_FALSE(fs::exists(P::WorldSaveService::active_regions_path(dir)));
        errors.clear();
        ASSERT_TRUE(P::WorldSaveService{}.load_world(
            loaded, dir, errors, {}, {}, nullptr, nullptr, nullptr, false));
        EXPECT_TRUE(errors.empty());
        EXPECT_EQ(Tree(dir), baseline);
    }

    // Verified absence remains adoptable, with or without a historical index row.
    for (const bool listed : {false, true}) {
        const auto dir = world.root / (listed ? "indexed-absent" : "unlisted-absent");
        Copy(original, dir);
        ASSERT_TRUE(fs::remove(dir / State::kMember));
        ASSERT_NO_FATAL_FAILURE(ReindexMember(dir));
        if (!listed) {
            auto absent_index = index;
            std::erase_if(absent_index.members,
                          [](const auto& row) { return row.path == State::kMember; });
            Write(P::snapshot_index_path(dir), P::encode_snapshot_index(absent_index));
        }
        W::ActiveRegionLedger absent_ledger;
        absent_ledger.set_local_anchor({0.0, 0.0, 0.0});
        const auto absence_decodes = State::decode_count_for_testing();
        ASSERT_TRUE(P::WorldSaveService::save_active_regions(absent_ledger, W::WorldClock{}, dir));
        EXPECT_EQ(State::decode_count_for_testing(), absence_decodes);
        Luminumbra::WorldStreamingState loaded;
        bool present = true;
        std::vector<std::string> errors;
        ASSERT_TRUE(P::WorldSaveService{}.load_world(
            loaded, dir, errors, {}, {}, nullptr, nullptr, &present, true));
        EXPECT_TRUE(errors.empty());
        EXPECT_FALSE(present);
    }

    W::ActiveRegionLedger ledger;
    ledger.set_local_anchor({0.0, 0.0, 0.0});
    const auto decodes = State::decode_count_for_testing();
    const auto ledger_loads = P::WorldSaveService::ledger_load_count_for_testing();
    ASSERT_TRUE(P::WorldSaveService::save_active_regions(ledger, W::WorldClock{}, original));
    EXPECT_EQ(State::decode_count_for_testing() - decodes, 1u);
    EXPECT_EQ(P::WorldSaveService::ledger_load_count_for_testing(), ledger_loads);
    EXPECT_EQ(Read(original / State::kMember), pending.encode());
    Luminumbra::WorldStreamingState loaded;
    State restored;
    bool present = false;
    std::vector<std::string> errors;
    ASSERT_TRUE(P::WorldSaveService{}.load_world(
        loaded, original, errors, {}, {}, nullptr, &restored, &present, true));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(present);
    EXPECT_EQ(restored.encode(), pending.encode());
}

TEST(SimulationPendingState, IndexedPendingTransactionsValidateOriginalIndexFacts) {
    PendingWorld world(false);
    const auto original = world.root / "indexed-legacy";
    ASSERT_TRUE(world.session.SaveWorldStateTo(original));
    State pending;
    pending.events.next_sequence = 1;
    pending.events.events.push_back({99, 0, 0, "existing", "valid"});
    ASSERT_TRUE(P::WorldSaveService::save_pending_state(pending, original));
    ASSERT_TRUE(P::snapshot_index_exists(original));
    for (const std::string shape :
         {"pending", "raw-pending", "both", "remove-pending", "remove-ledger"}) {
        for (const std::string mutation : {"deleted", "altered", "valid"}) {
            SCOPED_TRACE(shape + "/" + mutation);
            const auto dir = world.root / (shape + "-" + mutation);
            Copy(original, dir);
            W::ActiveRegionLedger ledger;
            ledger.set_local_anchor({0.0, 0.0, 0.0});
            if (shape == "remove-ledger") {
                ASSERT_TRUE(P::WorldSaveService::save_active_regions(ledger, W::WorldClock{}, dir));
            }
            if (mutation == "deleted") {
                ASSERT_TRUE(fs::remove(dir / State::kMember));
            } else if (mutation == "altered") {
                auto altered = pending;
                altered.events.events.front().payload = "other";
                Write(dir / State::kMember, altered.encode());
            }
            const auto baseline = Tree(dir);
            Luminumbra::WorldStreamingState loaded;
            std::vector<std::string> errors;
            ASSERT_TRUE(P::WorldSaveService{}.load_world(
                loaded, dir, errors, {}, {}, nullptr, nullptr, nullptr, false));
            ASSERT_TRUE(errors.empty());
            auto replacement = pending;
            replacement.events.events.front().payload = "replacement";
            const auto decodes = State::decode_count_for_testing();
            bool committed = false;
            if (shape == "both") {
                P::WorldSaveService::SaveFactsScope facts(dir);
                P::SaveTransaction transaction(dir, 0, {}, &P::WorldSaveService::member_digest);
                ASSERT_TRUE(transaction.begin(&errors));
                ASSERT_TRUE(facts.validate(&errors));
                ASSERT_TRUE(P::WorldSaveService::save_pending_state(replacement, dir, &errors));
                ASSERT_TRUE(P::WorldSaveService::save_active_regions(
                    ledger, W::WorldClock{}, dir, &errors));
                committed = transaction.commit(&errors);
            } else if (shape == "pending")
                committed = P::WorldSaveService::save_pending_state(replacement, dir, &errors);
            else if (shape == "raw-pending")
                committed = P::WorldSaveService::write_member_bytes(
                    dir, State::kMember, replacement.encode(), &errors);
            else
                committed = P::WorldSaveService::remove_member(
                    dir,
                    shape == "remove-pending" ? State::kMember : "chunks/region/active-regions.arl",
                    &errors);
            EXPECT_EQ(committed, mutation == "valid");
            EXPECT_LE(State::decode_count_for_testing() - decodes, 1u);
            EXPECT_FALSE(P::journal_pending(dir));
            if (mutation != "valid") {
                EXPECT_FALSE(errors.empty());
                EXPECT_EQ(Tree(dir), baseline);
                errors.clear();
                ASSERT_TRUE(P::WorldSaveService{}.load_world(
                    loaded, dir, errors, {}, {}, nullptr, nullptr, nullptr, false));
                EXPECT_TRUE(errors.empty());
            } else {
                EXPECT_TRUE(errors.empty());
                State restored;
                bool present = false;
                ASSERT_TRUE(P::WorldSaveService{}.load_world(
                    loaded, dir, errors, {}, {}, nullptr, &restored, &present, true));
                EXPECT_TRUE(errors.empty());
                EXPECT_EQ(present, shape != "remove-pending");
                if (present) {
                    EXPECT_EQ(restored.encode(),
                              shape == "remove-ledger" ? pending.encode() : replacement.encode());
                }
            }
        }
    }
}

TEST(SimulationPendingState, LedgerOnlyAcknowledgementRefusesAtomically) {
    PendingWorld world;
    world.session.SetReplicatedSimulationAnchors({});
    world.session.PinActiveRegion({10, 0}, true);
    ASSERT_FALSE(world.session.GetRegionSchedulerFault());
    world.session.GetSimulationEventBus().subscribe([](const auto& event) {
        if (event.activation)
            throw std::runtime_error("Retain queued activation");
    });
    ASSERT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(world.session.GetRegionSchedulerFault());
    ASSERT_EQ(world.session.GetActiveRegionLedger().pending_activations().size(), 1u);
    ASSERT_EQ(world.session.GetSimulationEventBus().pending_count(), 1u);
    ASSERT_TRUE(world.session.SaveWorldState());
    const auto dir = world.session.GetWorldSaveDir();
    const auto baseline = Tree(dir);
    auto acknowledged = world.session.GetActiveRegionLedger();
    acknowledged.acknowledge_activation(1, {10, 0});
    ASSERT_TRUE(acknowledged.pending_activations().empty());
    const auto caller_ledger = acknowledged.encode();
    const auto pending_decodes = State::decode_count_for_testing();
    const auto ledger_loads = P::WorldSaveService::ledger_load_count_for_testing();
    std::vector<std::string> errors;
    EXPECT_FALSE(P::WorldSaveService::save_active_regions(
        acknowledged, world.session.GetWorldClock(), dir, &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(Tree(dir), baseline);
    EXPECT_EQ(acknowledged.encode(), caller_ledger);
    EXPECT_EQ(State::decode_count_for_testing() - pending_decodes, 1u);
    EXPECT_EQ(P::WorldSaveService::ledger_load_count_for_testing() - ledger_loads, 1u);
    // The raw writer and removal path must enforce the same dependency.
    for (const bool remove : {false, true}) {
        EXPECT_FALSE(world.session.RunInSnapshotTransaction([&] {
            return remove
                       ? P::WorldSaveService::remove_member(dir, "chunks/region/active-regions.arl")
                       : P::WorldSaveService::write_member_bytes(
                             dir, "chunks/region/active-regions.arl", acknowledged.encode());
        }));
        EXPECT_EQ(Tree(dir), baseline);
    }
    // A compatible ledger-only save still succeeds without extra member decodes.
    auto unchanged = world.session.GetActiveRegionLedger();
    const auto compatible_pending = State::decode_count_for_testing();
    const auto compatible_ledger = P::WorldSaveService::ledger_load_count_for_testing();
    ASSERT_TRUE(
        P::WorldSaveService::save_active_regions(unchanged, world.session.GetWorldClock(), dir));
    EXPECT_EQ(Read(dir / State::kMember), baseline.at(std::string(State::kMember)));
    EXPECT_EQ(State::decode_count_for_testing() - compatible_pending, 1u);
    EXPECT_EQ(P::WorldSaveService::ledger_load_count_for_testing() - compatible_ledger, 1u);
    W::GameSession loaded;
    world.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(world.session.GetMetadata().worldId))
        << loaded.GetWorldOpenError();
    EXPECT_EQ(Snapshot(loaded).encode(), Snapshot(world.session).encode());
    EXPECT_EQ(loaded.GetActiveRegionLedger().pending_activations().size(), 1u);
    loaded.SetReplicatedSimulationAnchors({});
    std::size_t delivered = 0;
    loaded.GetSimulationEventBus().subscribe([&](const auto& event) {
        if (event.activation)
            ++delivered;
    });
    ASSERT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(loaded.GetRegionSchedulerFault());
    EXPECT_EQ(delivered, 1u);
    EXPECT_TRUE(loaded.GetActiveRegionLedger().pending_activations().empty());
    ASSERT_NO_FATAL_FAILURE(SaveWithDecodeBudget(loaded, dir, 1, 1));
    ASSERT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(delivered, 1u);
}

TEST(SimulationPendingState, PendingMemberCommitRollbackAndRecovery) {
    PendingWorld world;
    const auto dir = world.session.GetWorldSaveDir();
    ASSERT_TRUE(world.session.SaveWorldState());
    const auto baseline = Tree(dir);
    world.session.GetSimulationEventBus().publish(99, "future", "new authority");
    const auto pending = Snapshot(world.session).encode();
    const auto interrupted = world.root / "interrupted";
    {
        HookGuard guard({dir, interrupted, true, true, false, false});
        EXPECT_FALSE(world.session.SaveWorldState());
        EXPECT_TRUE(Hook().seen);
    }
    EXPECT_EQ(Tree(dir), baseline);
    EXPECT_EQ(Snapshot(world.session).encode(), pending);
    ASSERT_TRUE(P::journal_pending(interrupted));
    EXPECT_EQ(Read(interrupted / State::kMember), pending);
    EXPECT_EQ(P::recover_pending_journal(interrupted, nullptr), P::RecoveryOutcome::RolledBack);
    EXPECT_EQ(Tree(interrupted), baseline);
    EXPECT_EQ(P::recover_pending_journal(interrupted, nullptr), P::RecoveryOutcome::NothingToDo);
    // Writer fingerprints must match the installed bytes before cached validation
    // can be reused, including bytes changed by a publication hook.
    for (const auto* member : {"simulation-pending.spn", "active-regions.arl"}) {
        HookState state;
        state.corrupt_member = member;
        HookGuard guard(std::move(state));
        EXPECT_FALSE(world.session.SaveWorldState());
        EXPECT_TRUE(Hook().seen);
        EXPECT_EQ(Tree(dir), baseline);
        EXPECT_EQ(Snapshot(world.session).encode(), pending);
    }
    const auto committed = world.root / "committed";
    {
        HookGuard guard({dir, committed, true, false, true, false});
        ASSERT_TRUE(world.session.SaveWorldState());
        EXPECT_TRUE(Hook().seen);
    }
    const auto final_tree = Tree(dir);
    ASSERT_TRUE(P::journal_pending(committed));
    EXPECT_EQ(P::recover_pending_journal(committed, nullptr), P::RecoveryOutcome::DiscardedStale);
    EXPECT_EQ(Tree(committed), final_tree);
    EXPECT_EQ(P::recover_pending_journal(committed, nullptr), P::RecoveryOutcome::NothingToDo);
    EXPECT_EQ(Read(dir / State::kMember), pending);
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(dir, index, nullptr));
    const auto member = std::find_if(index.members.begin(),
                                     index.members.end(),
                                     [](const auto& item) { return item.path == State::kMember; });
    ASSERT_NE(member, index.members.end());
    EXPECT_TRUE(member->present);
    EXPECT_EQ(member->cls, P::MemberClass::Authoritative);
    EXPECT_EQ(member->bytes, pending.size());
    EXPECT_EQ(member->fnv64, P::fnv1a64_bytes(pending.data(), pending.size()));
    W::GameSession loaded;
    world.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(world.session.GetMetadata().worldId));
    EXPECT_EQ(Snapshot(loaded).encode(), pending);
}

TEST(SimulationPendingState, FlagOffEventBehaviorAndSaveBytesUnchanged) {
    const auto pending_readers = P::WorldSaveService::pending_reader_count_for_testing();
    const auto pending_probes = P::WorldSaveService::pending_probe_count_for_testing();
    const auto pending_decodes = State::decode_count_for_testing();
    PendingWorld world(false);
    for (int i = 0; i < 7; ++i)
        ASSERT_EQ(world.session.TickSimulation(world.session.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(world.session.ComputeChunkStateHash(), "3efa127826e435c3");
    EXPECT_EQ(world.session.FoldClockIntoEcologyHash("legacy"), "legacy");
    const auto before = world.root / "before";
    ASSERT_TRUE(world.session.SaveWorldStateTo(before));
    const auto baseline = Tree(before);
    EXPECT_EQ(baseline.size(), 2u);
    const auto golden = fs::path(LUMINUMBRA_SOURCE_ROOT) / "test/fixtures/active-regions/devel-off";
    for (const auto& [path, bytes] : baseline)
        EXPECT_EQ(bytes, Read(golden / fs::path(path).filename()));
    auto& bus = world.session.GetSimulationEventBus();
    bus.publish(99, "future", "legacy remains ephemeral");
    bus.publish(1, "first", "");
    bus.publish(1, "suffix", "");
    bus.subscribe([](const auto&) { throw std::runtime_error("Legacy delivery failure"); });
    EXPECT_THROW(bus.drain(1), std::runtime_error);
    EXPECT_EQ(bus.pending_count(), 1u); // Legacy removes the entire due prefix before callbacks.
    const auto after = world.root / "after";
    ASSERT_TRUE(world.session.SaveWorldStateTo(after));
    EXPECT_EQ(Tree(after), baseline);
    EXPECT_FALSE(fs::exists(after / State::kMember));
    EXPECT_FALSE(P::snapshot_index_exists(after));
    EXPECT_EQ(world.session.ComputeChunkStateHash(), "3efa127826e435c3");
    // Disabled loading/validation ignores even a malformed pending member.
    Write(after / State::kMember, "malformed");
    W::WorldClock clock;
    bool required = true;
    ASSERT_TRUE(P::WorldSaveService::read_clock_metadata(after, clock, required));
    EXPECT_FALSE(required);
    ASSERT_TRUE(P::WorldSaveService::validate_save(after));
    P::MemberDigest absent_ledger;
    std::string digest_error;
    ASSERT_TRUE(P::WorldSaveService::member_digest(
        after, "chunks/region/active-regions.arl", absent_ledger, digest_error));
    EXPECT_FALSE(absent_ledger.present);
    W::GameSession loaded;
    world.Configure(loaded, false);
    ASSERT_TRUE(loaded.CreateWorld("Legacy pending ignored", "1337", "default"));
    ASSERT_TRUE(loaded.LoadWorldStateFrom(after));
    EXPECT_EQ(P::WorldSaveService::pending_reader_count_for_testing(), pending_readers);
    EXPECT_EQ(P::WorldSaveService::pending_probe_count_for_testing(), pending_probes);
    EXPECT_EQ(State::decode_count_for_testing(), pending_decodes);
    // The legacy bus still admits counts, text and ticks outside durable bounds.
    S::OrderedEventBus legacy;
    for (std::size_t i = 0; i <= State::kMaxEvents; ++i)
        legacy.publish(std::numeric_limits<std::uint64_t>::max(), "", "");
    EXPECT_EQ(legacy.pending_count(), State::kMaxEvents + 1);
    EXPECT_EQ(
        legacy.publish(W::WorldClock::kTickLimit, std::string(State::kMaxTextBytes + 1, 'x'), ""),
        static_cast<std::uint64_t>(State::kMaxEvents + 1));
    const auto replay = S::replay_eventbus_order_fixture();
    EXPECT_TRUE(replay.passed);
    EXPECT_EQ(S::describe_event_order(replay.delivered_events),
              (std::vector<std::string>{"1|-1|3|physics.impulse|crate:push",
                                        "1|0|1|input.command|player:move",
                                        "1|0|2|script.trigger|door:open",
                                        "2|-1|6|ai.intent|npc-2:wait",
                                        "2|0|0|ai.intent|npc-1:turn",
                                        "2|0|5|script.trigger|torch:light",
                                        "3|0|4|audio.event|stone:slide"}));
}
} // namespace
