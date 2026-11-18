//  the server-authoritative player avatar primitives.
// Proves the deterministic spawn layout + the entity-snapshot projection are
// pure/reproducible and that an empty avatar set yields the empty snapshot
// (byte-identical to the zero-avatar headless `entities` lane). The avatar->streaming
// ->entities-sub-hash lane end-to-end is validated by `--smoke --avatars N`
// (the existing HeadlessServerTick double-run asserts the entities sub-hash
// matches run-to-run).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/PlayerAvatar.h"

#include <entt/entt.hpp>

namespace {

using Luminumbra::Vec3;
using Luminumbra::World::BuildAvatarEntitySnapshot;
using Luminumbra::World::DeterministicAvatarSpawnOffset;
using Luminumbra::World::PlayerAvatar;

float horiz_len(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.z * v.z);
}

TEST(PlayerAvatar, SpawnOffsetIsDeterministicAndPure) {
    // Same id -> identical offset across calls (no RNG, no wall-clock).
    for (std::uint32_t id = 0; id < 32; ++id) {
        const Vec3 a = DeterministicAvatarSpawnOffset(id);
        const Vec3 b = DeterministicAvatarSpawnOffset(id);
        EXPECT_FLOAT_EQ(a.x, b.x) << "id=" << id;
        EXPECT_FLOAT_EQ(a.y, b.y) << "id=" << id;
        EXPECT_FLOAT_EQ(a.z, b.z) << "id=" << id;
    }
}

TEST(PlayerAvatar, SpawnOffsetZeroIsSpawnPoint) {
    const Vec3 o = DeterministicAvatarSpawnOffset(0);
    EXPECT_FLOAT_EQ(o.x, 0.0f);
    EXPECT_FLOAT_EQ(o.y, 0.0f);
    EXPECT_FLOAT_EQ(o.z, 0.0f);
}

TEST(PlayerAvatar, SpawnOffsetsAreDistinctAndFanOut) {
    // Phyllotaxis ring: radius grows ~sqrt(id), so later players sit further out
    // and no two players share an anchor (distinct streaming anchors).
    std::vector<Vec3> offs;
    for (std::uint32_t id = 0; id < 20; ++id)
        offs.push_back(DeterministicAvatarSpawnOffset(id));
    for (std::size_t i = 1; i < offs.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            const float dx = offs[i].x - offs[j].x;
            const float dz = offs[i].z - offs[j].z;
            EXPECT_GT(std::sqrt(dx * dx + dz * dz), 0.01f)
                << "avatars " << i << " and " << j << " share a position";
        }
    }
    // Monotone-ish fan-out: the outermost is clearly beyond the innermost ring.
    EXPECT_GT(horiz_len(offs[19]), horiz_len(offs[1]));
}

TEST(PlayerAvatar, BuildEntityReplStatesProjectsTypedRegistryEntities) {
    using Luminumbra::World::BuildEntityReplStates;
    namespace C = Luminumbra::Components;
    entt::registry reg;

    // A player (type 0), a deer (type 7), an arrow (type 42) at distinct positions.
    auto mk = [&](std::uint32_t nid, std::uint16_t type, Luminumbra::Vec3 pos, std::uint8_t anim) {
        auto e = reg.create();
        auto& tf = reg.emplace<C::TransformComponent>(e);
        tf.position = pos;
        auto& rep = reg.emplace<C::ReplicatedComponent>(e);
        rep.network_id = nid;
        rep.type_id = type;
        rep.anim_state = anim;
    };
    mk(2, 7, Luminumbra::Vec3(10.0f, 34.0f, -3.0f), 1); // deer
    mk(1, 0, Luminumbra::Vec3(0.0f, 35.0f, 0.0f), 0);   // player
    mk(3, 42, Luminumbra::Vec3(-5.0f, 36.0f, 8.0f), 0); // arrow
    // An entity WITHOUT ReplicatedComponent must be ignored.
    {
        auto e = reg.create();
        reg.emplace<C::TransformComponent>(e);
    }

    const auto states = BuildEntityReplStates(reg);
    ASSERT_EQ(states.size(), 3u);
    // Sorted by network_id.
    EXPECT_EQ(states[0].entity_id, 1u);
    EXPECT_EQ(states[0].type_id, 0u);
    EXPECT_EQ(states[1].entity_id, 2u);
    EXPECT_EQ(states[1].type_id, 7u);
    EXPECT_EQ(states[1].anim_state, 1u);
    EXPECT_NEAR(Luminumbra::Net::ReplDequantPos(states[1].px_mm), 10.0f, 0.001f);
    EXPECT_EQ(states[2].entity_id, 3u);
    EXPECT_EQ(states[2].type_id, 42u);
}

TEST(PlayerAvatar, EmptyAvatarsYieldEmptySnapshot) {
    const std::string empty_builtin = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        Luminumbra::Ecs::EntityRegistrySnapshot{});
    const std::string empty_from_avatars =
        Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(BuildAvatarEntitySnapshot({}));
    // Byte-identical to the zero-avatar empty lane -> default entities sub-hash unchanged.
    EXPECT_EQ(empty_builtin, empty_from_avatars);
}

TEST(PlayerAvatar, SnapshotIsDeterministicOrderedAndNonEmpty) {
    std::vector<PlayerAvatar> avatars;
    // Insert OUT OF ORDER to prove the snapshot sorts by player_id (the contract).
    PlayerAvatar a2;
    a2.player_id = 2;
    a2.position = Vec3(10.0f, 5.0f, -3.0f);
    a2.facing = 1.5f;
    PlayerAvatar a0;
    a0.player_id = 0;
    a0.position = Vec3(0.0f, 4.0f, 0.0f);
    PlayerAvatar a1;
    a1.player_id = 1;
    a1.position = Vec3(-2.5f, 4.2f, 7.1f);
    a1.velocity = Vec3(1.0f, 0.0f, 0.0f);
    avatars = {a2, a0, a1};

    const std::string s1 =
        Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(BuildAvatarEntitySnapshot(avatars));
    // Reorder the input -> identical serialization (order-independent / sorted).
    std::vector<PlayerAvatar> reordered = {a0, a1, a2};
    const std::string s2 =
        Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(BuildAvatarEntitySnapshot(reordered));
    EXPECT_EQ(s1, s2);

    // Non-empty: differs from the empty snapshot (so the entities sub-hash moves
    // off the empty value once players are present).
    const std::string empty = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        Luminumbra::Ecs::EntityRegistrySnapshot{});
    EXPECT_NE(s1, empty);

    // The snapshot reports 3 entities, one PlayerAvatar component each.
    EXPECT_NE(s1.find("\"entity_count\": 3"), std::string::npos);
    EXPECT_NE(s1.find("PlayerAvatar"), std::string::npos);
}

} // namespace

namespace {
using Luminumbra::Systems::PhysicsSystem;
using BodyOwnerState = PhysicsSystem::BodyOwnerState;
using BodySetState = PhysicsSystem::BodySetState;

void ExpectBodyBits(const BodyOwnerState& expected, const BodyOwnerState& actual) {
    EXPECT_EQ(expected.suspended, actual.suspended);
    EXPECT_EQ(expected.retained_velocity, actual.retained_velocity);
    EXPECT_EQ(expected.retained_angular_velocity, actual.retained_angular_velocity);
    EXPECT_EQ(expected.was_active, actual.was_active);
    ASSERT_EQ(expected.size, actual.size);
    ASSERT_LE(expected.size, expected.bytes.size());
    EXPECT_TRUE(std::equal(
        expected.bytes.begin(), expected.bytes.begin() + expected.size, actual.bytes.begin()));
}

void ExpectBodySet(const BodySetState& expected, const BodySetState& actual) {
    EXPECT_EQ(expected.slot_count, actual.slot_count);
    EXPECT_EQ(expected.free_slots, actual.free_slots);
    ASSERT_EQ(expected.owners.size(), actual.owners.size());
    for (std::size_t i = 0; i < expected.owners.size(); ++i) {
        ExpectBodyBits(expected.owners[i], actual.owners[i]);
    }
}

void AddBodyFloor(PhysicsSystem& physics) {
    Luminumbra::Chunk floor(Luminumbra::IVec3(0));
    constexpr auto side = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1);
    floor.heightmap_data.assign(side * side, 8.0f);
    ASSERT_TRUE(physics.add_chunk_collision(floor));
}

void SetBodyWord(BodyOwnerState& state, std::size_t offset, std::uint32_t word) {
    ASSERT_LE(offset, state.size);
    ASSERT_LE(4u, state.size - offset);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        state.bytes[offset++] = static_cast<std::uint8_t>(word >> shift);
    }
}

std::uint32_t BodyWord(const BodyOwnerState& state, std::size_t offset) {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        result |= static_cast<std::uint32_t>(state.bytes[offset++]) << shift;
    }
    return result;
}

TEST(BodyContinuation, FreezeUnfreezeEmptyCapturePreservesNextCapsuleIdentity) {
    PhysicsSystem source;
    source.startup();
    const Vec3 feet(0.0f, 40.0f, 0.0f);
    const auto avatar = source.create_avatar_character(feet);
    source.set_avatar_frozen(avatar, true, feet);
    source.set_avatar_frozen(avatar, false, feet);
    BodySetState captured;
    ASSERT_TRUE(source.capture_body_owners(captured));
    ASSERT_TRUE(captured.owners.empty());
    ASSERT_EQ(captured.slot_count, 1u);
    ASSERT_EQ(captured.free_slots.size(), 1u);
    EXPECT_EQ(captured.free_slots[0], JPH::BodyID(0, 1).GetIndexAndSequenceNumber());

    PhysicsSystem restored;
    restored.startup();
    const auto restored_avatar = restored.create_avatar_character(feet);
    ASSERT_TRUE(restored.restore_body_owners(captured, {}));
    source.set_avatar_frozen(avatar, true, feet);
    restored.set_avatar_frozen(restored_avatar, true, feet);
    BodySetState expected;
    BodySetState actual;
    ASSERT_TRUE(source.capture_body_owners(expected));
    ASSERT_TRUE(restored.capture_body_owners(actual));
    ASSERT_EQ(expected.owners.size(), 1u);
    EXPECT_EQ(BodyWord(expected.owners[0], 8), JPH::BodyID(0, 2).GetIndexAndSequenceNumber());
    ExpectBodySet(expected, actual);
    BodyOwnerState ignored;
    EXPECT_FALSE(restored.capture_body_owner(JPH::BodyID(0, 1), ignored));
}

TEST(BodyContinuation, EmptyHistoryPreservesGenerationsAndFreeStackOrder) {
    PhysicsSystem source;
    source.startup();
    std::array<JPH::BodyID, 4> ids{};
    for (auto& id : ids) {
        id = source.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
        ASSERT_FALSE(id.IsInvalid());
    }
    source.destroy_body(ids[1]);
    ids[1] = source.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
    ASSERT_EQ(ids[1], JPH::BodyID(1, 2));
    for (const auto index : {2u, 0u, 3u, 1u}) {
        source.destroy_body(ids[index]);
    }
    BodySetState captured;
    ASSERT_TRUE(source.capture_body_owners(captured));
    ASSERT_TRUE(captured.owners.empty());
    ASSERT_EQ(captured.free_slots.size(), 4u);
    PhysicsSystem target;
    target.startup();
    // The destination can have older, different generations in the same extent.
    for (unsigned i = 0; i < 5; ++i) {
        const auto old = target.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
        target.destroy_body(old);
    }
    ASSERT_TRUE(target.restore_body_owners(captured, {}));
    for (const auto index : {1u, 3u, 0u, 2u}) {
        const auto expected = source.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
        const auto actual = target.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
        EXPECT_EQ(expected,
                  JPH::BodyID(index, static_cast<JPH::uint8>(ids[index].GetSequenceNumber() + 1)));
        EXPECT_EQ(actual, expected);
    }
    EXPECT_EQ(target.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f), JPH::BodyID(4, 1));
}

TEST(BodyContinuation, ExplicitSequencesAdvanceForSpheresAndCapsulesIncludingWrap) {
    for (const bool capsule : {false, true}) {
        PhysicsSystem source;
        source.startup();
        if (capsule) {
            const auto avatar = source.create_avatar_character(Vec3(0.0f));
            source.set_avatar_frozen(avatar, true, Vec3(0.0f));
        } else {
            ASSERT_FALSE(source.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f).IsInvalid());
        }
        BodySetState state;
        ASSERT_TRUE(source.capture_body_owners(state));
        ASSERT_EQ(state.owners.size(), 1u);
        for (const unsigned sequence : {0u, 1u, 2u, 127u, 255u}) {
            const JPH::BodyID restored_id(0, static_cast<JPH::uint8>(sequence));
            SetBodyWord(state.owners[0], 8, restored_id.GetIndexAndSequenceNumber());
            PhysicsSystem target;
            target.startup();
            std::array<JPH::BodyID, 1> ids{};
            ASSERT_TRUE(target.restore_body_owners(state, ids));
            ASSERT_EQ(ids[0], restored_id);
            target.destroy_body(ids[0]);
            const auto next = target.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
            EXPECT_EQ(next, JPH::BodyID(0, static_cast<JPH::uint8>(sequence + 1)));
            BodyOwnerState ignored;
            EXPECT_FALSE(target.capture_body_owner(restored_id, ignored));
            EXPECT_TRUE(target.capture_body_owner(next, ignored));
        }
    }
}

TEST(BodyContinuation, TerrainAndRestartShareAllocatorBookkeeping) {
    PhysicsSystem source;
    source.startup();
    AddBodyFloor(source);
    source.remove_chunk_collision(Luminumbra::Chunk(Luminumbra::IVec3(0)).get_id());
    const auto sphere = source.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
    ASSERT_EQ(sphere, JPH::BodyID(0, 2));
    BodySetState state;
    ASSERT_TRUE(source.capture_body_owners(state));
    PhysicsSystem target;
    target.startup();
    std::array<JPH::BodyID, 1> ids{};
    ASSERT_TRUE(target.restore_body_owners(state, ids));
    target.destroy_body(ids[0]);
    EXPECT_EQ(target.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f), JPH::BodyID(0, 3));
    target.shutdown();
    target.startup();
    EXPECT_EQ(target.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f), JPH::BodyID(0, 1));
}

TEST(BodyContinuation, SpinContactsSleepAndSavedBoundsContinueExactly) {
    constexpr float dt = 1.0f / 30.0f;
    for (const int capture_tick : {6, 12, 240}) {
        PhysicsSystem source;
        source.startup();
        AddBodyFloor(source);
        const auto velocity = capture_tick == 12 ? Vec3(0.1234567f, 0.0f, 0.0765432f) : Vec3(0.0f);
        const auto body = source.create_dynamic_sphere(
            Vec3(5.0f, capture_tick == 6 ? 8.3f : 8.4f, 5.0f), velocity, 0.3f);
        for (int tick = 0; tick < capture_tick; ++tick) {
            source.update(dt);
        }
        BodySetState captured;
        ASSERT_TRUE(source.capture_body_owners(captured));
        ASSERT_EQ(captured.owners.size(), 1u);
        if (capture_tick == 6) {
            EXPECT_GT(std::bit_cast<float>(BodyWord(captured.owners[0], 180)), 0.0f);
        }
        if (capture_tick == 12) {
            EXPECT_TRUE(std::bit_cast<float>(BodyWord(captured.owners[0], 72)) != 0.0f ||
                        std::bit_cast<float>(BodyWord(captured.owners[0], 80)) != 0.0f);
        }
        if (capture_tick == 240) {
            EXPECT_FALSE(source.body_is_active(body));
        }
        PhysicsSystem target;
        target.startup();
        AddBodyFloor(target);
        std::array<JPH::BodyID, 1> ids{};
        ASSERT_TRUE(target.restore_body_owners(captured, ids));
        EXPECT_EQ(ids[0], body);
        // A sleeping body's broadphase bounds must already be at the saved position.
        const auto position = source.get_body_position(body);
        EXPECT_TRUE(target.audio_raycast(position + Vec3(0.0f, 1.0f, 0.0f), position).hit);
        for (int tick = 0; tick < 20; ++tick) {
            BodySetState expected;
            BodySetState actual;
            ASSERT_TRUE(source.capture_body_owners(expected));
            ASSERT_TRUE(target.capture_body_owners(actual));
            ExpectBodySet(expected, actual);
            source.update(dt);
            target.update(dt);
        }
    }
}

TEST(BodyContinuation, MixedSetRestoresSharedContactsInAnyOrder) {
    PhysicsSystem source;
    source.startup();
    AddBodyFloor(source);
    const auto avatar = source.create_avatar_character(Vec3(9.0f, 8.0f, 8.0f));
    source.set_avatar_frozen(avatar, true, source.get_avatar_position(avatar));
    ASSERT_FALSE(
        source.create_dynamic_sphere(Vec3(7.8f, 8.31f, 8.0f), Vec3(0.1234567f, 0.0f, 0.0f), 0.3f)
            .IsInvalid());
    ASSERT_FALSE(
        source.create_dynamic_sphere(Vec3(8.4f, 8.31f, 8.0f), Vec3(-0.0765432f, 0.0f, 0.0f), 0.3f)
            .IsInvalid());
    ASSERT_FALSE(
        source.create_dynamic_sphere(Vec3(12.0f, 40.0f, 12.0f), Vec3(0.0f), 0.3f).IsInvalid());
    for (unsigned tick = 0; tick < 12; ++tick) {
        source.update(1.0f / 30.0f);
    }
    BodySetState state;
    ASSERT_TRUE(source.capture_body_owners(state));
    ASSERT_EQ(state.owners.size(), 4u);
    ASSERT_GT(BodyWord(state.owners[1], 208), 0u);
    ASSERT_EQ(BodyWord(state.owners[3], 208), 0u);
    std::reverse(state.owners.begin(), state.owners.end());
    PhysicsSystem target;
    target.startup();
    AddBodyFloor(target);
    std::array<JPH::BodyID, 4> ids{};
    for (const auto offset : {192u, 196u, 200u, 204u}) {
        auto mismatched = state;
        SetBodyWord(
            mismatched.owners[0], offset, BodyWord(mismatched.owners[0], offset) ^ 0x80000000u);
        EXPECT_FALSE(target.restore_body_owners(mismatched, ids));
        EXPECT_TRUE(
            std::all_of(ids.begin(), ids.end(), [](JPH::BodyID id) { return id.IsInvalid(); }));
    }
    ASSERT_TRUE(target.restore_body_owners(state, ids));
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(ids[i].GetIndexAndSequenceNumber(), BodyWord(state.owners[i], 8));
    }
    for (unsigned tick = 0; tick < 20; ++tick) {
        BodySetState expected;
        BodySetState actual;
        ASSERT_TRUE(source.capture_body_owners(expected));
        ASSERT_TRUE(target.capture_body_owners(actual));
        ExpectBodySet(expected, actual);
        source.update(1.0f / 30.0f);
        target.update(1.0f / 30.0f);
    }
}

TEST(BodyContinuation, MalformedPayloadsAndPartialSetsRefuseWithoutPublication) {
    PhysicsSystem source;
    source.startup();
    AddBodyFloor(source);
    BodyOwnerState terrain_output;
    terrain_output.size = 7;
    EXPECT_FALSE(source.capture_body_owner(JPH::BodyID(0, 1), terrain_output));
    EXPECT_EQ(terrain_output.size, 7u);
    ASSERT_FALSE(
        source.create_dynamic_sphere(Vec3(5.0f, 8.31f, 5.0f), Vec3(0.1234567f, 0.0f, 0.0f), 0.3f)
            .IsInvalid());
    for (unsigned tick = 0; tick < 12; ++tick) {
        source.update(1.0f / 30.0f);
    }
    BodySetState state;
    ASSERT_TRUE(source.capture_body_owners(state));
    ASSERT_GT(BodyWord(state.owners[0], 208), 0u);
    PhysicsSystem target;
    target.startup();
    AddBodyFloor(target);
    BodySetState before;
    ASSERT_TRUE(target.capture_body_owners(before));
    const JPH::BodyID sentinel(777, 123);
    std::array<JPH::BodyID, 1> ids{sentinel};
    const auto refuse = [&](const BodySetState& invalid) {
        EXPECT_FALSE(target.restore_body_owners(invalid, ids));
        EXPECT_EQ(ids[0], sentinel);
        BodySetState after;
        ASSERT_TRUE(target.capture_body_owners(after));
        ExpectBodySet(before, after);
    };
    for (std::uint32_t size = 0; size < state.owners[0].size; ++size) {
        auto invalid = state;
        invalid.owners[0].size = size;
        refuse(invalid);
    }
    for (const auto& [offset, word] : std::array<std::pair<std::size_t, std::uint32_t>, 18>{
             {{0, 0},
              {4, 99},
              {8, JPH::BodyID::cInvalidBodyID},
              {8, JPH::BodyID::cBroadPhaseBit},
              {8, 10240},
              {12, 0},
              {12, 0x7fc00000u},
              {12, 1},
              {12, std::bit_cast<std::uint32_t>(1.0e-10f)},
              {12, 0x7f7fffffu},
              {16, 2},
              {44, 0x7fc00000u},
              {188, 0},
              {208, 65},
              {244, 257},
              {264, 0},
              {268, 0x7fc00000u},
              {232, std::bit_cast<std::uint32_t>(1.01f)}}}) {
        auto invalid = state;
        SetBodyWord(invalid.owners[0], offset, word);
        refuse(invalid);
    }
    auto invalid = state;
    invalid.owners[0].size = static_cast<std::uint32_t>(BodyOwnerState::kMaxBytes + 1);
    refuse(invalid);
    invalid = state;
    ++invalid.owners[0].size;
    refuse(invalid);
    invalid = state;
    invalid.owners.clear();
    EXPECT_FALSE(target.restore_body_owners(invalid, {}));
    EXPECT_FALSE(target.restore_body_owners(state, {}));
    // No external terrain: the cached ground-body reference cannot be dropped.
    PhysicsSystem missing;
    missing.startup();
    auto dangling = state;
    dangling.free_slots.push_back(JPH::BodyID(0, 1).GetIndexAndSequenceNumber());
    EXPECT_FALSE(missing.restore_body_owners(dangling, ids));
    EXPECT_EQ(ids[0], sentinel);
    ASSERT_TRUE(target.restore_body_owners(state, ids));
    BodySetState actual;
    ASSERT_TRUE(target.capture_body_owners(actual));
    ExpectBodySet(state, actual);
}

TEST(BodyContinuation, AllocatorRefusalsPreserveDestinationAndNextAllocation) {
    PhysicsSystem source;
    source.startup();
    std::array<JPH::BodyID, 3> freed{};
    for (auto& id : freed) {
        id = source.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
    }
    for (const auto id : freed) {
        source.destroy_body(id);
    }
    BodySetState state;
    ASSERT_TRUE(source.capture_body_owners(state));
    PhysicsSystem target;
    target.startup();
    const auto avatar = target.create_avatar_character(Vec3(0.0f));
    target.set_avatar_frozen(avatar, true, Vec3(0.0f));
    BodySetState before;
    ASSERT_TRUE(target.capture_body_owners(before));
    // The source's allocator marks slot 0 free, but the destination owns it.
    EXPECT_FALSE(target.restore_body_owners(state, {}));
    target.set_avatar_frozen(avatar, false, Vec3(0.0f));
    ASSERT_TRUE(target.capture_body_owners(before));
    for (unsigned variant = 0; variant < 7; ++variant) {
        auto invalid = state;
        if (variant == 0) {
            invalid.slot_count = 10241;
        } else if (variant == 1) {
            invalid.free_slots.push_back(invalid.free_slots[0]);
        } else if (variant == 2) {
            invalid.free_slots[1] = invalid.free_slots[0];
        } else if (variant == 3) {
            invalid.free_slots[0] = JPH::BodyID::cInvalidBodyID;
        } else if (variant == 4) {
            invalid.free_slots[0] = JPH::BodyID(3, 1).GetIndexAndSequenceNumber();
        } else if (variant == 5) {
            invalid.free_slots[0] = JPH::BodyID::cBroadPhaseBit;
        } else {
            invalid.free_slots.pop_back();
        }
        EXPECT_FALSE(target.restore_body_owners(invalid, {}));
        BodySetState after;
        ASSERT_TRUE(target.capture_body_owners(after));
        ExpectBodySet(before, after);
    }
    EXPECT_EQ(target.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f), JPH::BodyID(0, 2));
    // Refuse occupied slots even when an incoming body's generation differs.
    BodySetState occupied;
    ASSERT_TRUE(target.capture_body_owners(occupied));
    SetBodyWord(occupied.owners[0], 8, JPH::BodyID(0, 3).GetIndexAndSequenceNumber());
    std::array<JPH::BodyID, 1> ids{};
    EXPECT_FALSE(target.restore_body_owners(occupied, ids));
    EXPECT_TRUE(ids[0].IsInvalid());
    // An empty destination cannot be shrunk after allocating a larger extent.
    PhysicsSystem larger;
    larger.startup();
    std::array<JPH::BodyID, 4> old{};
    for (auto& id : old) {
        id = larger.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
    }
    for (const auto id : old) {
        larger.destroy_body(id);
    }
    EXPECT_FALSE(larger.restore_body_owners(state, {}));
    EXPECT_EQ(larger.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f), JPH::BodyID(3, 2));
}

TEST(BodyContinuation, AdmissionBoundsCaptureOutputsAndMaximumPoolExtent) {
    PhysicsSystem source;
    source.startup();
    BodySetState empty;
    ASSERT_TRUE(source.capture_body_owners(empty));
    EXPECT_TRUE(source.restore_body_owners(empty, {}));
    for (std::size_t i = 0; i < PhysicsSystem::kMaxRestoredBodies; ++i) {
        ASSERT_FALSE(source.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f).IsInvalid());
    }
    BodySetState maximum;
    ASSERT_TRUE(source.capture_body_owners(maximum));
    ASSERT_EQ(maximum.owners.size(), PhysicsSystem::kMaxRestoredBodies);
    PhysicsSystem target;
    target.startup();
    std::vector<JPH::BodyID> ids(maximum.owners.size());
    auto duplicate = maximum;
    duplicate.owners[1] = duplicate.owners[0];
    EXPECT_FALSE(target.restore_body_owners(duplicate, ids));
    ASSERT_TRUE(target.restore_body_owners(maximum, ids));
    const auto extra = source.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
    ASSERT_FALSE(extra.IsInvalid());
    BodySetState output = empty;
    EXPECT_FALSE(source.capture_body_owners(output));
    ExpectBodySet(empty, output);
    BodyOwnerState extra_owner;
    ASSERT_TRUE(source.capture_body_owner(extra, extra_owner));
    maximum.owners.push_back(extra_owner);
    ++maximum.slot_count;
    ids.emplace_back();
    std::fill(ids.begin(), ids.end(), JPH::BodyID());
    PhysicsSystem extent;
    extent.startup();
    EXPECT_FALSE(extent.restore_body_owners(maximum, ids));
    EXPECT_TRUE(std::all_of(ids.begin(), ids.end(), [](JPH::BodyID id) { return id.IsInvalid(); }));
    auto high = empty;
    high.slot_count = 10240;
    for (std::uint32_t slot = 0; slot < high.slot_count; ++slot) {
        high.free_slots.push_back(JPH::BodyID(slot, 255).GetIndexAndSequenceNumber());
    }
    ASSERT_TRUE(extent.restore_body_owners(high, {}));
    EXPECT_EQ(extent.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f), JPH::BodyID(10239, 0));
}

// Construct individually valid parts whose aggregate headers, alignment and
// allocator-block tails exceed Jolt's cache, despite fitting the count limits.
void AddBodyCache(BodyOwnerState& state,
                  std::span<const BodyOwnerState> capsules,
                  std::uint32_t manifold_count,
                  std::uint32_t points) {
    ASSERT_LE(capsules.size(), manifold_count);
    ASSERT_LE(manifold_count, 2 * capsules.size());
    state.size = 208; // sphere prefix ends before the body-pair count
    const auto append = [&state](std::uint32_t word) {
        ASSERT_LE(state.size, state.bytes.size() - 4);
        state.size += 4;
        SetBodyWord(state, state.size - 4, word);
    };
    append(static_cast<std::uint32_t>(capsules.size()));
    for (std::size_t pair = 0; pair < capsules.size(); ++pair) {
        const auto first = BodyWord(capsules[pair], 8);
        const auto second = BodyWord(state, 8);
        append(first);
        append(second);
        for (unsigned scalar = 0; scalar < 6; ++scalar) {
            append(0);
        }
        const auto count = pair < manifold_count - capsules.size() ? 2u : 1u;
        append(count);
        for (std::uint32_t manifold = 0; manifold < count; ++manifold) {
            append(first);
            append(manifold);
            append(second);
            append(0);
            append(points);
            append(0);
            append(std::bit_cast<std::uint32_t>(1.0f));
            append(0);
            for (std::uint32_t scalar = 0; scalar < 9 * points; ++scalar) {
                append(0);
            }
        }
    }
    append(0); // CCD count
}

TEST(BodyContinuation, ContactCacheChecksRealCapacityBeforePublicationAndAllowsRetry) {
    PhysicsSystem source;
    source.startup();
    PhysicsSystem target;
    target.startup();
    for (std::size_t i = 0; i < 64; ++i) {
        const Vec3 feet(static_cast<float>(i) * 4.0f, 40.0f, 0.0f);
        const auto original = source.create_avatar_character(feet);
        source.set_avatar_frozen(original, true, feet);
        const auto restored = target.create_avatar_character(feet);
        target.set_avatar_frozen(restored, true, feet);
    }
    BodySetState before;
    ASSERT_TRUE(target.capture_body_owners(before));
    const auto body = source.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
    BodyOwnerState prototype;
    ASSERT_TRUE(source.capture_body_owner(body, prototype));
    BodySetState state;
    state.slot_count = 192;
    state.owners.assign(PhysicsSystem::kMaxRestoredBodies, prototype);
    std::vector<JPH::BodyID> ids(state.owners.size());
    for (std::size_t i = 0; i < state.owners.size(); ++i) {
        SetBodyWord(state.owners[i],
                    8,
                    JPH::BodyID(static_cast<JPH::uint32>(64 + i), 1).GetIndexAndSequenceNumber());
        AddBodyCache(state.owners[i], before.owners, 80, 4);
        SetBodyWord(state.owners[i], 192, std::bit_cast<std::uint32_t>(0.125f));
        SetBodyWord(state.owners[i], 200, std::bit_cast<std::uint32_t>(-3.0f));
    }
    EXPECT_FALSE(target.restore_body_owners(state, ids));
    EXPECT_TRUE(std::all_of(ids.begin(), ids.end(), [](JPH::BodyID id) { return id.IsInvalid(); }));
    BodySetState after;
    ASSERT_TRUE(target.capture_body_owners(after));
    ExpectBodySet(before, after);
    const auto probe = target.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f);
    EXPECT_EQ(probe, body);
    BodyOwnerState probe_state;
    ASSERT_TRUE(target.capture_body_owner(probe, probe_state));
    ExpectBodyBits(prototype, probe_state);
    target.destroy_body(probe);
    for (auto& owner : state.owners) {
        AddBodyCache(owner, before.owners, 64, 1);
    }
    ASSERT_TRUE(target.restore_body_owners(state, ids));
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(ids[i].GetIndexAndSequenceNumber(), BodyWord(state.owners[i], 8));
        ASSERT_TRUE(target.capture_body_owner(ids[i], probe_state));
        ExpectBodyBits(state.owners[i], probe_state);
    }
}
} // namespace

namespace {
TEST(BodyContinuation, FrozenPayloadRefusalsAndOccupiedStaticGenerationsAreAtomic) {
    PhysicsSystem source;
    source.startup();
    const auto avatar = source.create_avatar_character(Vec3(0.0f, 40.0f, 0.0f));
    source.set_avatar_frozen(avatar, true, source.get_avatar_position(avatar));
    BodySetState state;
    ASSERT_TRUE(source.capture_body_owners(state));
    ASSERT_EQ(state.owners.size(), 1u);
    PhysicsSystem target;
    target.startup();
    BodySetState empty;
    ASSERT_TRUE(target.capture_body_owners(empty));
    const JPH::BodyID sentinel(777, 123);
    std::array<JPH::BodyID, 1> ids{sentinel};
    for (std::uint32_t size = 0; size < state.owners[0].size; ++size) {
        auto invalid = state;
        invalid.owners[0].size = size;
        EXPECT_FALSE(target.restore_body_owners(invalid, ids));
        EXPECT_EQ(ids[0], sentinel);
    }
    for (const auto& [offset, word] : std::array<std::pair<std::size_t, std::uint32_t>, 4>{
             {{0, BodyWord(state.owners[0], 0) ^ 0x00010000u},
              {4, 2},
              {8, JPH::BodyID::cInvalidBodyID},
              {16, 0x7ff00000u}}}) {
        auto invalid = state;
        SetBodyWord(invalid.owners[0], offset, word);
        EXPECT_FALSE(target.restore_body_owners(invalid, ids));
        EXPECT_EQ(ids[0], sentinel);
    }
    BodySetState after;
    ASSERT_TRUE(target.capture_body_owners(after));
    ExpectBodySet(empty, after);
    const auto resident = target.create_avatar_character(Vec3(12.0f, 40.0f, 0.0f));
    target.set_avatar_frozen(resident, true, target.get_avatar_position(resident));
    ASSERT_TRUE(target.capture_body_owners(state));
    const auto before = state;
    SetBodyWord(state.owners[0], 8, JPH::BodyID(0, 2).GetIndexAndSequenceNumber());
    EXPECT_FALSE(target.restore_body_owners(state, ids));
    ASSERT_TRUE(target.capture_body_owners(after));
    ExpectBodySet(before, after);
    EXPECT_EQ(ids[0], sentinel);
    EXPECT_EQ(target.create_dynamic_sphere(Vec3(0.0f), Vec3(0.0f), 0.3f), JPH::BodyID(1, 1));
}

TEST(BodyContinuation, AvatarSlotCapAndStoppedSystemsPreserveOutputs) {
    PhysicsSystem source;
    BodySetState state;
    state.slot_count = 17;
    EXPECT_FALSE(source.capture_body_owners(state));
    EXPECT_EQ(state.slot_count, 17u);
    BodyOwnerState owner;
    owner.size = 7;
    EXPECT_FALSE(source.capture_body_owner(JPH::BodyID(), owner));
    EXPECT_EQ(owner.size, 7u);
    EXPECT_FALSE(source.restore_body_owners({}, {}));
    source.startup();
    for (std::size_t i = 0; i < PhysicsSystem::kMaxRestoredAvatars; ++i) {
        const auto index = source.create_avatar_character(Vec3(0.0f));
        source.remove_avatar_character(index);
    }
    ASSERT_TRUE(source.capture_body_owners(state));
    EXPECT_TRUE(source.restore_body_owners(state, {}));
    const auto before = state;
    const auto extra = source.create_avatar_character(Vec3(0.0f));
    source.remove_avatar_character(extra);
    EXPECT_FALSE(source.capture_body_owners(state));
    ExpectBodySet(before, state);
    EXPECT_FALSE(source.restore_body_owners(before, {}));
}
} // namespace

namespace {
TEST(BodyContinuation, ReplacesRemovedBodyCachesForEmptyAndNonemptySets) {
    constexpr float dt = 1.0f / 30.0f;
    for (const bool empty : {false, true}) {
        PhysicsSystem source;
        source.startup();
        AddBodyFloor(source);
        const auto body = source.create_dynamic_sphere(Vec3(5.0f, 8.31f, 5.0f), Vec3(0.0f), 0.3f);
        PhysicsSystem target;
        target.startup();
        AddBodyFloor(target);
        const auto removed =
            target.create_dynamic_sphere(Vec3(12.0f, 8.31f, 12.0f), Vec3(0.0f), 0.3f);
        for (unsigned tick = 0; tick < 12; ++tick) {
            source.update(dt);
            target.update(dt);
        }
        target.destroy_body(removed);
        if (empty) {
            source.destroy_body(body);
        }
        BodySetState state;
        ASSERT_TRUE(source.capture_body_owners(state));
        std::vector<JPH::BodyID> ids(state.owners.size());
        ASSERT_TRUE(target.restore_body_owners(state, ids));
        if (empty) {
            const auto expected =
                source.create_dynamic_sphere(Vec3(5.0f, 8.31f, 5.0f), Vec3(0.0f), 0.3f);
            const auto actual =
                target.create_dynamic_sphere(Vec3(5.0f, 8.31f, 5.0f), Vec3(0.0f), 0.3f);
            EXPECT_EQ(expected, actual);
        }
        for (unsigned tick = 0; tick < 20; ++tick) {
            source.update(dt);
            target.update(dt);
            BodySetState expected;
            BodySetState actual;
            ASSERT_TRUE(source.capture_body_owners(expected));
            ASSERT_TRUE(target.capture_body_owners(actual));
            ExpectBodySet(expected, actual);
        }
    }
}
} // namespace

TEST(BodyContinuation, InvalidSuspensionMetadataRefusesBeforeCommit) {
    PhysicsSystem source, target;
    source.startup();
    target.startup();
    const auto sphere = source.create_dynamic_sphere({0, 100, 0}, {}, 0.1f);
    ASSERT_FALSE(sphere.IsInvalid());
    BodySetState state;
    ASSERT_TRUE(source.capture_body_owners(state));
    ASSERT_EQ(state.owners.size(), 1u);
    state.owners[0].suspended = true; // An active payload cannot represent suspension.
    std::array<JPH::BodyID, 1> ids{};
    EXPECT_FALSE(target.restore_body_owners(state, ids));
    EXPECT_TRUE(ids[0].IsInvalid());
    BodySetState unchanged;
    ASSERT_TRUE(target.capture_body_owners(unchanged));
    EXPECT_TRUE(unchanged.owners.empty());
    EXPECT_EQ(unchanged.slot_count, 0u);

    // Suspended continuation also owns its registration before terrain is bound.
    auto suspended = state;
    SetBodyWord(suspended.owners[0], 16, 0); // inactive Jolt payload
    suspended.owners[0].retained_velocity = {2, 0, 0};
    suspended.owners[0].retained_angular_velocity = {1, 2, 3};
    suspended.owners[0].was_active = true;
    PhysicsSystem suspended_target;
    suspended_target.startup();
    std::array<JPH::BodyID, 1> suspended_ids{};
    ASSERT_TRUE(suspended_target.restore_body_owners(suspended, suspended_ids));
    BodyOwnerState captured;
    ASSERT_TRUE(suspended_target.capture_body_owner(suspended_ids[0], captured));
    ExpectBodyBits(suspended.owners[0], captured);
    suspended_target.clear_simulation_collisions();
    suspended_target.update(1.0f / 30);
    EXPECT_GT(suspended_target.get_body_position(suspended_ids[0]).x, 0);

    source.destroy_body(sphere);
    const auto avatar = source.create_avatar_character({0, 100, 0});
    ASSERT_NE(avatar, PhysicsSystem::kInvalidAvatar);
    source.set_avatar_frozen(avatar, true, {0, 100, 0});
    ASSERT_TRUE(source.capture_body_owners(state));
    ASSERT_EQ(state.owners.size(), 1u);
    state.owners[0].suspended = true; // Only spheres can carry suspended motion.
    EXPECT_FALSE(target.restore_body_owners(state, ids));
    EXPECT_TRUE(ids[0].IsInvalid());
    ASSERT_TRUE(target.capture_body_owners(unchanged));
    EXPECT_TRUE(unchanged.owners.empty());
    EXPECT_EQ(unchanged.slot_count, 0u);
}
