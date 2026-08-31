// Adversarial determinism-hardening for the replication stack (net/). Hunts the bugs the
// happy-path siblings (ReplicationProtocol_test / ReplicationDelta_test /
// ReplicationDeltaLoop_test / ReplicationEndpoint_test / NetworkSim_test) miss on the E2E
// determinism path: serialize/deserialize symmetry on adversarial states, delta-vs-snapshot
// equivalence, id-ordering independence from insertion order, idempotency/convergence under
// dropped/duplicate/out-of-order acks, AOI radius boundary, and despawn-mid-stream ghosts.
//
// Include-path style + namespace usage match the closest sibling (ReplicationDeltaLoop_test.cpp):
// "net/..." headers, `using namespace Luminumbra::Net;`. Everything net here lives under the
// single namespace Luminumbra::Net (capital-L). No ::Luminumbra::Components types are touched.
//
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "net/NetworkSim.h"
#include "net/ReplicationDelta.h"
#include "net/ReplicationEndpoint.h"
#include "net/ReplicationProtocol.h"

namespace {

using namespace Luminumbra::Net;

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

ReplEntityState Ent(std::uint32_t id, std::int32_t x, std::int32_t y, std::int32_t z) {
    ReplEntityState e;
    e.entity_id = id;
    e.px_mm = x;
    e.py_mm = y;
    e.pz_mm = z;
    return e;
}

// Sort a snapshot's entities by id so a direct vector compare is order-independent.
std::vector<ReplEntityState> SortedEnts(std::vector<ReplEntityState> v) {
    std::sort(v.begin(), v.end(), [](const ReplEntityState& a, const ReplEntityState& b) {
        return a.entity_id < b.entity_id;
    });
    return v;
}

std::vector<std::uint32_t> IdsOf(const std::vector<ReplEntityState>& v) {
    std::vector<std::uint32_t> ids;
    ids.reserve(v.size());
    for (const auto& e : v)
        ids.push_back(e.entity_id);
    return ids;
}

// A rich, adversarial entity exercising every field at boundary/sign/extreme values.
ReplEntityState RichEntity(std::uint32_t id, int salt) {
    ReplEntityState e;
    e.entity_id = id;
    e.px_mm = (salt % 2 == 0) ? std::numeric_limits<std::int32_t>::min()
                              : std::numeric_limits<std::int32_t>::max();
    e.py_mm = (salt % 3 == 0) ? 0 : -((salt + 1) * 123457);
    e.pz_mm = (salt % 4 == 0) ? std::numeric_limits<std::int32_t>::min() + 1 : (salt * 999983);
    e.yaw_mrad =
        static_cast<std::int16_t>((salt % 2 == 0) ? std::numeric_limits<std::int16_t>::min()
                                                  : std::numeric_limits<std::int16_t>::max());
    e.flags = static_cast<std::uint8_t>(0xA5 ^ salt);
    e.type_id = static_cast<std::uint16_t>(0xFFFF - (salt * 7));
    e.anim_state = static_cast<std::uint8_t>(salt * 13);
    e.anim_phase = static_cast<std::uint8_t>(255 - salt);
    return e;
}

constexpr std::uint32_t kClientId = 1;

// ===========================================================================
// 1) SERIALIZE / DESERIALIZE SYMMETRY (encode -> decode == original, bit-exact)
// ===========================================================================

TEST(ReplHardening, SnapshotRichAdversarialRosterRoundTripsBitExact) {
    SnapshotMsg in;
    in.server_tick = std::numeric_limits<std::uint64_t>::max();
    in.snapshot_seq = std::numeric_limits<std::uint32_t>::max();
    in.delta_from_seq = 0xDEADBEEFu;
    in.acked_usercmd_tick = std::numeric_limits<std::uint64_t>::max() - 17;
    for (int i = 0; i < 64; ++i)
        in.entities.push_back(RichEntity(static_cast<std::uint32_t>(i), i));
    in.removed_ids = {0u, 1u, std::numeric_limits<std::uint32_t>::max(), 123456u};

    SnapshotMsg out;
    ASSERT_TRUE(DecodeSnapshot(EncodeSnapshot(in), out));
    EXPECT_EQ(out.server_tick, in.server_tick);
    EXPECT_EQ(out.snapshot_seq, in.snapshot_seq);
    EXPECT_EQ(out.delta_from_seq, in.delta_from_seq);
    EXPECT_EQ(out.acked_usercmd_tick, in.acked_usercmd_tick);
    ASSERT_EQ(out.entities.size(), in.entities.size());
    for (std::size_t i = 0; i < in.entities.size(); ++i)
        EXPECT_TRUE(out.entities[i] == in.entities[i]) << "entity " << i;
    EXPECT_EQ(out.removed_ids, in.removed_ids);
}

// delta_from_seq is a real wire field. A non-zero value MUST survive the round-trip,
// else the client cannot find the baseline to apply the delta against.
TEST(ReplHardening, DeltaFromSeqFieldRoundTrips) {
    SnapshotMsg in;
    in.snapshot_seq = 9;
    in.delta_from_seq = 7; // "deltaFrom" the seq-7 baseline
    in.entities.push_back(Ent(1, 5, 6, 7));
    SnapshotMsg out;
    ASSERT_TRUE(DecodeSnapshot(EncodeSnapshot(in), out));
    EXPECT_EQ(out.delta_from_seq, 7u) << "delta_from_seq written but not read back";
}

// Encoding the SAME message twice is byte-identical (stable wire form).
TEST(ReplHardening, EncodeIsByteStable) {
    SnapshotMsg in;
    in.server_tick = 4242;
    in.snapshot_seq = 5;
    for (int i = 0; i < 10; ++i)
        in.entities.push_back(RichEntity(static_cast<std::uint32_t>(i), i));
    in.removed_ids = {99u, 7u, 3u};
    EXPECT_EQ(EncodeSnapshot(in), EncodeSnapshot(in));
}

TEST(ReplHardening, UsercmdBoundaryFieldsRoundTrip) {
    UsercmdMsg in;
    in.tick = std::numeric_limits<std::uint64_t>::max();
    in.player_id = std::numeric_limits<std::uint32_t>::max();
    in.move_x = std::numeric_limits<std::int16_t>::min();
    in.move_z = std::numeric_limits<std::int16_t>::max();
    in.yaw_mrad = -1;
    in.action_bits = 0xFF;
    UsercmdMsg out;
    ASSERT_TRUE(DecodeUsercmd(EncodeUsercmd(in), out));
    EXPECT_EQ(in, out);
}

TEST(ReplHardening, AckBoundaryRoundTrips) {
    AckMsg in{std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint64_t>::max()};
    AckMsg out;
    ASSERT_TRUE(DecodeAck(EncodeAck(in), out));
    EXPECT_EQ(out.snapshot_seq, in.snapshot_seq);
    EXPECT_EQ(out.usercmd_tick, in.usercmd_tick);
}

// Negative-coordinate entity (px/py/pz < 0, negative yaw) survives the int->uint->int wire trip.
TEST(ReplHardening, NegativeCoordEntityRoundTrips) {
    SnapshotMsg in;
    in.entities.push_back(Ent(42, -2000000000, -1, -2000000000));
    in.entities.back().yaw_mrad = -32000;
    SnapshotMsg out;
    ASSERT_TRUE(DecodeSnapshot(EncodeSnapshot(in), out));
    ASSERT_EQ(out.entities.size(), 1u);
    EXPECT_TRUE(out.entities[0] == in.entities[0]);
}

// A truncated frame (declared length longer than payload) is rejected, not partially decoded.
TEST(ReplHardening, TruncatedSnapshotRejected) {
    SnapshotMsg in;
    in.entities.push_back(Ent(1, 1, 2, 3));
    in.entities.push_back(Ent(2, 4, 5, 6));
    std::vector<std::uint8_t> frame = EncodeSnapshot(in);
    frame.pop_back(); // payload now shorter than the declared length
    SnapshotMsg out;
    EXPECT_FALSE(DecodeSnapshot(frame, out));
}

// An empty frame must be rejected (no crash, no spurious accept).
TEST(ReplHardening, EmptyFrameRejected) {
    std::vector<std::uint8_t> empty;
    SnapshotMsg s;
    UsercmdMsg u;
    AckMsg a;
    EXPECT_FALSE(DecodeSnapshot(empty, s));
    EXPECT_FALSE(DecodeUsercmd(empty, u));
    EXPECT_FALSE(DecodeAck(empty, a));
}

// ===========================================================================
// 2) DELTA vs SNAPSHOT consistency (base + delta == full snapshot of new state)
// ===========================================================================

// Reconstruct(base, delta(base, cur)) == cur, for a rich adversarial cur (added / changed /
// removed all at once), compared order-independently.
TEST(ReplHardening, DeltaThenApplyReproducesRichSnapshot) {
    SnapshotMsg base;
    base.snapshot_seq = 1;
    for (int i = 0; i < 20; ++i)
        base.entities.push_back(RichEntity(static_cast<std::uint32_t>(i), i));
    SnapshotMsg cur;
    cur.snapshot_seq = 2;
    for (int i = 5; i < 30; ++i) // 0..4 removed, 5..19 partly changed, 20..29 new
        cur.entities.push_back(RichEntity(static_cast<std::uint32_t>(i), i + (i % 2)));

    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);
    const SnapshotMsg recon = ApplySnapshotDelta(base, delta);
    EXPECT_EQ(SortedEnts(recon.entities), SortedEnts(cur.entities));
}

// Header fields (server_tick / snapshot_seq / acked_usercmd_tick) survive base+delta.
TEST(ReplHardening, ApplyDeltaPreservesHeader) {
    SnapshotMsg base;
    base.server_tick = 10;
    base.entities.push_back(Ent(1, 0, 0, 0));
    SnapshotMsg cur;
    cur.server_tick = 11;
    cur.snapshot_seq = 99;
    cur.acked_usercmd_tick = 88;
    cur.entities.push_back(Ent(1, 100, 0, 0));
    const SnapshotMsg recon = ApplySnapshotDelta(base, MakeSnapshotDelta(base, cur));
    EXPECT_EQ(recon.server_tick, 11u);
    EXPECT_EQ(recon.snapshot_seq, 99u);
    EXPECT_EQ(recon.acked_usercmd_tick, 88u);
}

// Empty base (a fresh client) + full current: delta carries every entity, applies to full set.
TEST(ReplHardening, DeltaAgainstEmptyBaseIsFullSet) {
    SnapshotMsg base; // empty
    SnapshotMsg cur;
    for (int i = 0; i < 8; ++i)
        cur.entities.push_back(Ent(static_cast<std::uint32_t>(i), i, 0, 0));
    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);
    EXPECT_EQ(delta.entities.size(), cur.entities.size());
    EXPECT_TRUE(delta.removed_ids.empty());
    EXPECT_EQ(SortedEnts(ApplySnapshotDelta(base, delta).entities), SortedEnts(cur.entities));
}

// Current empty (everything despawned): delta removes ALL baseline ids; reconstruct is empty.
TEST(ReplHardening, DeltaToEmptyRemovesAll) {
    SnapshotMsg base;
    for (int i = 0; i < 5; ++i)
        base.entities.push_back(Ent(static_cast<std::uint32_t>(i), i, 0, 0));
    SnapshotMsg cur; // empty
    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);
    EXPECT_EQ(delta.removed_ids.size(), base.entities.size());
    EXPECT_TRUE(ApplySnapshotDelta(base, delta).entities.empty());
}

// Identical base==cur: a zero-change delta (no entities, no removals) that round-trips.
TEST(ReplHardening, NoChangeDeltaIsEmptyAndReconstructs) {
    SnapshotMsg base;
    for (int i = 0; i < 6; ++i)
        base.entities.push_back(RichEntity(static_cast<std::uint32_t>(i), i));
    SnapshotMsg cur = base;
    cur.snapshot_seq = base.snapshot_seq + 1;
    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);
    EXPECT_TRUE(delta.entities.empty());
    EXPECT_TRUE(delta.removed_ids.empty());
    EXPECT_EQ(SortedEnts(ApplySnapshotDelta(base, delta).entities), SortedEnts(cur.entities));
}

// ===========================================================================
// 3) ORDER & ITERATION (id-ordered, independent of insertion order)
// ===========================================================================

// Same logical set built in two DIFFERENT insertion orders must encode to IDENTICAL wire bytes
// after the codec's canonical (id) ordering -- a result that depends on insertion order is a bug.
// MakeSnapshotDelta sorts by id, so deltaing each ordering against the same base must match.
TEST(ReplHardening, DeltaWireBytesIndependentOfInsertionOrder) {
    SnapshotMsg base; // empty -> the whole set is "new"
    std::vector<ReplEntityState> es;
    for (int i = 0; i < 12; ++i)
        es.push_back(RichEntity(static_cast<std::uint32_t>(100 - i), i));

    SnapshotMsg a;
    a.snapshot_seq = 3;
    a.entities = es;
    SnapshotMsg b;
    b.snapshot_seq = 3;
    b.entities = es;
    std::reverse(b.entities.begin(), b.entities.end()); // different insertion order, same set

    const SnapshotMsg da = MakeSnapshotDelta(base, a);
    const SnapshotMsg db = MakeSnapshotDelta(base, b);
    EXPECT_EQ(EncodeSnapshot(da), EncodeSnapshot(db))
        << "delta wire form depends on insertion order (must be id-ordered)";
}

// ApplySnapshotDelta output is id-ascending regardless of baseline/delta input ordering.
TEST(ReplHardening, ApplyDeltaOutputIsIdAscending) {
    SnapshotMsg base;
    base.entities = {Ent(9, 0, 0, 0), Ent(2, 0, 0, 0), Ent(5, 0, 0, 0)};
    SnapshotMsg delta;
    delta.entities = {Ent(7, 1, 0, 0), Ent(1, 1, 0, 0)};
    delta.removed_ids = {5};
    const SnapshotMsg recon = ApplySnapshotDelta(base, delta);
    const std::vector<std::uint32_t> ids = IdsOf(recon.entities);
    EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end())) << "reconstructed set not id-ordered";
    EXPECT_EQ(ids, (std::vector<std::uint32_t>{1, 2, 7, 9}));
}

// removed_ids in the delta are sorted ascending and deterministic regardless of base order.
TEST(ReplHardening, DeltaRemovedIdsSortedDeterministic) {
    SnapshotMsg base;
    base.entities = {Ent(50, 0, 0, 0), Ent(3, 0, 0, 0), Ent(20, 0, 0, 0), Ent(7, 0, 0, 0)};
    SnapshotMsg cur; // all gone
    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);
    EXPECT_EQ(delta.removed_ids, (std::vector<std::uint32_t>{3, 7, 20, 50}));
}

// ===========================================================================
// 4) IDEMPOTENCY / CONVERGENCE
// ===========================================================================

// Re-applying the SAME delta to the SAME base is a no-op (idempotent reconstruction).
TEST(ReplHardening, ReapplyingSameDeltaIsIdempotent) {
    SnapshotMsg base;
    for (int i = 0; i < 10; ++i)
        base.entities.push_back(Ent(static_cast<std::uint32_t>(i), i, 0, 0));
    SnapshotMsg cur;
    for (int i = 3; i < 13; ++i)
        cur.entities.push_back(Ent(static_cast<std::uint32_t>(i), i * 2, 0, 0));
    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);
    const SnapshotMsg once = ApplySnapshotDelta(base, delta);
    const SnapshotMsg twice = ApplySnapshotDelta(base, delta);
    EXPECT_EQ(SortedEnts(once.entities), SortedEnts(twice.entities));
    EXPECT_EQ(SortedEnts(once.entities), SortedEnts(cur.entities));
}

// SnapshotReceiver: a duplicate of the current seq is rejected and never regresses state.
TEST(ReplHardening, ReceiverRejectsDuplicateSeq) {
    SnapshotReceiver rx;
    SnapshotMsg s;
    s.snapshot_seq = 4;
    s.server_tick = 400;
    s.entities.push_back(Ent(1, 7, 0, 0));
    EXPECT_TRUE(rx.Receive(s));
    SnapshotMsg dup = s;
    dup.entities[0].px_mm = 999; // a corrupted duplicate must NOT overwrite
    EXPECT_FALSE(rx.Receive(dup));
    ASSERT_TRUE(rx.has_snapshot());
    EXPECT_EQ(rx.current().entities[0].px_mm, 7);
}

// UsercmdReceiver convergence under out-of-order + duplicate inbound: newest tick wins, exactly.
TEST(ReplHardening, UsercmdReceiverOutOfOrderConverges) {
    UsercmdReceiver rx;
    UsercmdMsg t10;
    t10.tick = 10;
    t10.move_x = 10;
    UsercmdMsg t12;
    t12.tick = 12;
    t12.move_x = 12;
    UsercmdMsg t11;
    t11.tick = 11;
    t11.move_x = 11;
    EXPECT_TRUE(rx.Receive(t10));
    EXPECT_TRUE(rx.Receive(t12));
    EXPECT_FALSE(rx.Receive(t11)); // older than 12 -> rejected
    EXPECT_FALSE(rx.Receive(t12)); // duplicate -> rejected
    EXPECT_EQ(rx.latest().tick, 12u);
    EXPECT_EQ(rx.latest().move_x, 12);
}

// Ack tracking is monotonic: a stale (lower-seq) ack, even arriving last, never lowers state.
TEST(ReplHardening, AckMonotonicUnderReorder) {
    UsercmdReceiver rx;
    rx.ApplyAck(AckMsg{8, 0});
    rx.ApplyAck(AckMsg{2, 0}); // stale, arrives later
    rx.ApplyAck(AckMsg{5, 0}); // also stale vs 8
    EXPECT_EQ(rx.acked_snapshot_seq(), 8u);
    rx.ApplyAck(AckMsg{8, 0}); // duplicate of current -> no change
    EXPECT_EQ(rx.acked_snapshot_seq(), 8u);
}

// ===========================================================================
// 5) AOI / interest boundary + per-client scoping
// ===========================================================================

// An entity at EXACTLY the AOI radius: the contract (code) is INCLUSIVE (<= r). Pin it so a
// later off-by-one (flipping to <) is caught, and assert both sides of the boundary.
TEST(ReplHardening, AoiRadiusBoundaryIsDeterministicInclusive) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(kClientId, pair.first.get());
    ReplicationClient client(kClientId, pair.second.get());
    server.SetAoiRadiusMm(5000);
    std::vector<ReplEntityState> entities = {
        Ent(1, 0, 0, 0),     // own avatar (always in)
        Ent(10, 5000, 0, 0), // exactly at the radius
        Ent(11, 5001, 0, 0), // one mm beyond
    };
    server.BroadcastSnapshot(1, entities);
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    const std::vector<std::uint32_t> ids = IdsOf(client.snapshot().entities);
    EXPECT_NE(std::find(ids.begin(), ids.end(), 10u), ids.end())
        << "at-radius entity must be in (inclusive)";
    EXPECT_EQ(std::find(ids.begin(), ids.end(), 11u), ids.end())
        << "beyond-radius entity must be out";
}

// The client's OWN avatar is always included even when far outside any radius of itself's center
// (degenerate: a 1 mm radius). Self must never be culled.
TEST(ReplHardening, AoiAlwaysIncludesOwnAvatar) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(kClientId, pair.first.get());
    ReplicationClient client(kClientId, pair.second.get());
    server.SetAoiRadiusMm(1); // tiny radius
    std::vector<ReplEntityState> entities = {Ent(1, 7000000, 0, 0), Ent(2, 7100000, 0, 0)};
    server.BroadcastSnapshot(1, entities);
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    const std::vector<std::uint32_t> ids = IdsOf(client.snapshot().entities);
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1u), ids.end());
    EXPECT_EQ(std::find(ids.begin(), ids.end(), 2u), ids.end());
}

// AOI distance math must not overflow at extreme world coordinates (int32 px near limits). The
// box-cull + int64 squared-distance path should still correctly include/exclude.
TEST(ReplHardening, AoiNoOverflowAtExtremeCoordinates) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(kClientId, pair.first.get());
    ReplicationClient client(kClientId, pair.second.get());
    server.SetAoiRadiusMm(1000);
    const std::int32_t big = std::numeric_limits<std::int32_t>::max() - 10;
    std::vector<ReplEntityState> entities = {
        Ent(1, big, 0, 0),                                      // own avatar at the world edge
        Ent(2, std::numeric_limits<std::int32_t>::min(), 0, 0), // opposite edge -> far, out
    };
    server.BroadcastSnapshot(1, entities);
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    const std::vector<std::uint32_t> ids = IdsOf(client.snapshot().entities);
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1u), ids.end());
    EXPECT_EQ(std::find(ids.begin(), ids.end(), 2u), ids.end())
        << "overflow misclassified a far entity";
}

// Chunk-AOI at a chunk SEAM: an entity whose coordinate is the exact chunk-boundary belongs to
// the higher chunk (floor-divide), so at radius 0 a centre one mm below the seam must NOT see it.
TEST(ReplHardening, ChunkAoiSeamRadiusZero) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(kClientId, pair.first.get());
    ReplicationClient client(kClientId, pair.second.get());
    server.SetAoiChunkRadius(0, 16000);
    std::vector<ReplEntityState> entities = {
        Ent(1, 15999, 0, 0),  // chunk (0,0): centre just below the seam
        Ent(10, 16000, 0, 0), // chunk (1,0): exactly on the seam -> different chunk -> out at r0
    };
    server.BroadcastSnapshot(1, entities);
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    const std::vector<std::uint32_t> ids = IdsOf(client.snapshot().entities);
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1u), ids.end());
    EXPECT_EQ(std::find(ids.begin(), ids.end(), 10u), ids.end())
        << "seam entity leaked across chunk boundary";
}

// Chunk-AOI with a negative-coordinate centre: floor-divide must keep boundaries stable across
// the origin (the documented reason floor-divide is used). Centre at -1mm is chunk -1.
TEST(ReplHardening, ChunkAoiNegativeOriginStable) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(kClientId, pair.first.get());
    ReplicationClient client(kClientId, pair.second.get());
    server.SetAoiChunkRadius(0, 16000);
    std::vector<ReplEntityState> entities = {
        Ent(1, -1, 0, -1), // chunk (-1,-1)
        Ent(10,
            -16000,
            0,
            -1), // chunk (-1,-1)? -16000 -> chunk -1 boundary: floor(-16000/16000) = -1
        Ent(11, -16001, 0, -1), // chunk (-2,-1) -> out at radius 0
    };
    server.BroadcastSnapshot(1, entities);
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    const std::vector<std::uint32_t> ids = IdsOf(client.snapshot().entities);
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1u), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), 10u), ids.end())
        << "negative-origin chunk math drifted";
    EXPECT_EQ(std::find(ids.begin(), ids.end(), 11u), ids.end());
}

// AOI disabled (radius 0, no chunk) sends the FULL set even with far-flung entities.
TEST(ReplHardening, AoiDisabledSendsFullSet) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(kClientId, pair.first.get());
    ReplicationClient client(kClientId, pair.second.get());
    std::vector<ReplEntityState> entities = {Ent(1, 0, 0, 0), Ent(2, 9000000, 0, 0)};
    server.BroadcastSnapshot(1, entities);
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    EXPECT_EQ(client.snapshot().entities.size(), 2u);
}

// ===========================================================================
// 6) DESPAWN MID-STREAM / explicit-removal delivery (full vs delta parity)
// ===========================================================================

// In FULL-snapshot mode, a caller-supplied despawn for a TRANSIENT id (never a replicated
// entity, e.g. a spent arrow) reaches the client. This is the baseline the delta path must match.
TEST(ReplHardening, FullModeDeliversTransientDespawn) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server; // delta OFF
    server.AddClient(kClientId, pair.first.get());
    ReplicationClient client(kClientId, pair.second.get());
    server.BroadcastSnapshot(1, {Ent(1, 0, 0, 0)}, {/*removed*/ 2000u});
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    const auto& rem = client.snapshot().removed_ids;
    EXPECT_NE(std::find(rem.begin(), rem.end(), 2000u), rem.end());
}

// EXPECTED-FAIL (real bug): under DELTA compression a caller-supplied despawn for a transient id
// that is NOT in the acked baseline is SILENTLY DROPPED -- MakeSnapshotDelta derives removed_ids
// only from (baseline - current) and ignores snap.removed_ids. The client never despawns the
// arrow (a stale ghost). Asserts the CORRECT contract: the despawn must still reach the client.
TEST(ReplHardening, DeltaModeDeliversTransientDespawn) {
    NetworkConditions cond;
    cond.latency_ticks = 1;
    cond.seed = 7;
    NetworkSim sim(cond);
    ReplicationServer server;
    server.SetDeltaCompression(true);
    server.AddClient(kClientId, &sim.A());
    ReplicationClient client(kClientId, &sim.B());

    auto pump = [&] {
        for (int k = 0; k < 6; ++k) {
            sim.Tick();
            client.PumpInbound();
            sim.Tick();
            server.PumpInbound();
        }
    };

    const std::vector<ReplEntityState> world = {Ent(1, 0, 0, 0)};
    // First two broadcasts establish + ack a baseline so the next send is a true delta.
    server.BroadcastSnapshot(1, world);
    pump();
    server.BroadcastSnapshot(2, world);
    pump();
    ASSERT_GT(server.AckedSnapshotSeq(kClientId), 0u);

    // Now despawn a transient id (2000) that was never in the baseline entity set.
    server.BroadcastSnapshot(3, world, {/*removed*/ 2000u});
    pump();

    ASSERT_TRUE(client.has_snapshot());
    const auto& rem = client.snapshot().removed_ids;
    EXPECT_NE(std::find(rem.begin(), rem.end(), 2000u), rem.end())
        << "delta mode dropped a transient despawn -> stale ghost on the client";
}

// EXPECTED-FAIL (same root cause): a disconnected client's avatar despawn (PruneDisconnected ->
// pending removed_ids) must reach survivors even under delta compression. The pending id is the
// leaver's avatar, which after the leave is no longer in the entity set the server broadcasts, so
// MakeSnapshotDelta might catch it via baseline-diff ONLY if it was in the acked baseline -- but
// a transient/just-joined leaver whose avatar never made an acked baseline is dropped. Here the
// leaver's avatar (id 2) WAS broadcast but we drive a fresh acked baseline AFTER removal, so the
// removal must travel via removed_ids, not baseline diff.
TEST(ReplHardening, DeltaModePruneDespawnReachesSurvivor) {
    auto pa = MakeLoopbackPair();
    auto pb = MakeLoopbackPair();
    ReplicationServer server;
    server.SetDeltaCompression(true);
    server.AddClient(1, pa.first.get());
    server.AddClient(2, pb.first.get());
    ReplicationClient client_a(1, pa.second.get());
    // Note: only entity id 1 (survivor's avatar) is ever in the broadcast set, so the leaver's
    // despawn can only travel via removed_ids (not via baseline-diff).
    const std::vector<ReplEntityState> world = {Ent(1, 0, 0, 0)};

    auto sync = [&] {
        client_a.PumpInbound();
        server.PumpInbound();
    };

    server.BroadcastSnapshot(1, world);
    sync();
    server.BroadcastSnapshot(2, world); // survivor acks a real baseline
    sync();
    ASSERT_GT(server.AckedSnapshotSeq(1), 0u);

    pb.second->Close();
    server.PumpInbound();
    auto removed = server.PruneDisconnectedClients();
    ASSERT_EQ(removed.size(), 1u);
    ASSERT_EQ(removed[0], 2u);

    // The pending despawn for id 2 must reach the survivor across the repeated broadcasts.
    bool saw_despawn = false;
    for (int b = 0; b < 4 && !saw_despawn; ++b) {
        server.BroadcastSnapshot(static_cast<std::uint64_t>(10 + b), world);
        client_a.PumpInbound();
        server.PumpInbound();
        if (client_a.has_snapshot()) {
            const auto& rem = client_a.snapshot().removed_ids;
            if (std::find(rem.begin(), rem.end(), 2u) != rem.end())
                saw_despawn = true;
        }
    }
    EXPECT_TRUE(saw_despawn) << "delta mode never told the survivor to despawn the leaver's avatar";
}

// A mid-stream despawn that IS in the baseline (a replicated entity that leaves the set) is the
// case delta-diff handles: it should appear in removed_ids and the client drops it. (Sanity:
// proves the codec path itself works, isolating the bug above to the snap.removed_ids drop.)
TEST(ReplHardening, DeltaDropsEntityThatLeavesSet) {
    SnapshotMsg base;
    base.snapshot_seq = 1;
    base.entities = {Ent(1, 0, 0, 0), Ent(2, 0, 0, 0), Ent(3, 0, 0, 0)};
    SnapshotMsg cur;
    cur.snapshot_seq = 2;
    cur.entities = {Ent(1, 0, 0, 0), Ent(3, 0, 0, 0)}; // entity 2 left
    const SnapshotMsg delta = MakeSnapshotDelta(base, cur);
    EXPECT_NE(std::find(delta.removed_ids.begin(), delta.removed_ids.end(), 2u),
              delta.removed_ids.end());
    const SnapshotMsg recon = ApplySnapshotDelta(base, delta);
    EXPECT_EQ(IdsOf(SortedEnts(recon.entities)), (std::vector<std::uint32_t>{1, 3}));
}

// ===========================================================================
// 7) END-TO-END convergence & determinism over the lossy NetworkSim
// ===========================================================================

// run == replay: the same conditions + sequence reconstruct a BYTE-identical client set, and the
// reconstructed wire bytes match too (stronger than the per-field compare in the sibling).
TEST(ReplHardening, DeltaLoopRunEqualsReplayWireExact) {
    auto authoritative = [](int tick) {
        std::vector<ReplEntityState> set;
        for (int i = 0; i < 6; ++i) {
            ReplEntityState e =
                Ent(static_cast<std::uint32_t>(i + 1), (i == 2) ? tick * 50 : i * 1000, 0, i * 250);
            e.yaw_mrad = static_cast<std::int16_t>(i * 10);
            set.push_back(e);
        }
        return set;
    };
    auto run = [&] {
        NetworkConditions cond;
        cond.latency_ticks = 2;
        cond.jitter_ticks = 1;
        cond.drop_one_in = 3;
        cond.seed = 0xABCDEF;
        NetworkSim sim(cond);
        ReplicationServer server;
        server.SetDeltaCompression(true);
        server.AddClient(kClientId, &sim.A());
        ReplicationClient client(kClientId, &sim.B());
        for (int t = 0; t < 40; ++t) {
            server.BroadcastSnapshot(static_cast<std::uint64_t>(t), authoritative(t));
            for (int k = 0; k < 6; ++k) {
                sim.Tick();
                client.PumpInbound();
                sim.Tick();
                server.PumpInbound();
            }
        }
        return client.has_snapshot() ? EncodeSnapshot(client.snapshot())
                                     : std::vector<std::uint8_t>{};
    };
    EXPECT_EQ(run(), run());
}

// Convergence: after a quiet drain the delta-mode client reconstructs EXACTLY the server's set
// even under heavy loss (the ack-driven baseline self-heals dropped deltas).
TEST(ReplHardening, DeltaLoopConvergesUnderLoss) {
    auto authoritative = [](int tick) {
        std::vector<ReplEntityState> set;
        for (int i = 0; i < 5; ++i)
            set.push_back(
                Ent(static_cast<std::uint32_t>(i + 1), (i == 1) ? tick * 33 : i * 500, 0, i * 100));
        return set;
    };
    NetworkConditions cond;
    cond.latency_ticks = 1;
    cond.jitter_ticks = 1;
    cond.drop_one_in = 2;
    cond.seed = 314159;
    NetworkSim sim(cond);
    ReplicationServer server;
    server.SetDeltaCompression(true);
    server.AddClient(kClientId, &sim.A());
    ReplicationClient client(kClientId, &sim.B());

    auto pump = [&] {
        for (int k = 0; k < 6; ++k) {
            sim.Tick();
            client.PumpInbound();
            sim.Tick();
            server.PumpInbound();
        }
    };
    int last = 0;
    for (int t = 0; t < 60; ++t) {
        server.BroadcastSnapshot(static_cast<std::uint64_t>(t), authoritative(t));
        pump();
        last = t;
    }
    for (int q = 0; q < 16; ++q) {
        server.BroadcastSnapshot(static_cast<std::uint64_t>(last), authoritative(last));
        pump();
    }
    ASSERT_TRUE(client.has_snapshot());
    EXPECT_EQ(SortedEnts(client.snapshot().entities), SortedEnts(authoritative(last)));
}

// ===========================================================================
// 8) GATING / EMPTY NO-OP
// ===========================================================================

// Broadcasting with NO clients is a clean no-op (telemetry zeroed, no crash).
TEST(ReplHardening, BroadcastWithNoClientsIsNoOp) {
    ReplicationServer server;
    server.BroadcastSnapshot(1, {Ent(1, 0, 0, 0)});
    EXPECT_EQ(server.client_count(), 0u);
    EXPECT_EQ(server.last_broadcast_total_bytes(), 0u);
    EXPECT_EQ(server.last_broadcast_max_client_bytes(), 0u);
}

// An empty roster broadcast still produces a valid, decodable (entity-less) snapshot.
TEST(ReplHardening, EmptyRosterBroadcastDecodes) {
    auto pair = MakeLoopbackPair();
    ReplicationServer server;
    server.AddClient(kClientId, pair.first.get());
    ReplicationClient client(kClientId, pair.second.get());
    server.BroadcastSnapshot(1, {}); // empty authoritative set
    client.PumpInbound();
    ASSERT_TRUE(client.has_snapshot());
    EXPECT_TRUE(client.snapshot().entities.empty());
}

// SnapshotInterpolator on an empty buffer returns nothing (no crash / no extrapolation).
TEST(ReplHardening, InterpolatorEmptyIsNoOp) {
    SnapshotInterpolator interp;
    EXPECT_TRUE(interp.empty());
    EXPECT_EQ(interp.newest_tick(), 0u);
    EXPECT_TRUE(interp.Sample(123.0).empty());
}

// A single-snapshot interpolator clamps to that snapshot at any sample time (degenerate span).
TEST(ReplHardening, InterpolatorSingleSnapshotClamps) {
    SnapshotInterpolator interp;
    SnapshotMsg s;
    s.server_tick = 10;
    s.entities = {Ent(1, 500, 0, 0)};
    interp.Push(s);
    EXPECT_EQ(interp.Sample(0.0)[0].px_mm, 500);
    EXPECT_EQ(interp.Sample(10.0)[0].px_mm, 500);
    EXPECT_EQ(interp.Sample(1000.0)[0].px_mm, 500);
}

} // namespace
