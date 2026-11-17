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
#include <limits>
#include <memory>
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

using Luminumbra::Systems::PhysicsSystem;
using Luminumbra::World::PhysicsOwnerState;
using Luminumbra::World::PlayerAvatarOwnerState;

void ExpectOwnerBits(const PhysicsOwnerState& expected, const PhysicsOwnerState& actual) {
    ASSERT_EQ(expected.size, actual.size);
    EXPECT_TRUE(std::equal(
        expected.bytes.begin(), expected.bytes.begin() + expected.size, actual.bytes.begin()));
}

void ExpectAvatarBits(const PlayerAvatarOwnerState& expected,
                      const PlayerAvatarOwnerState& actual) {
    EXPECT_EQ(expected.avatar.player_id, actual.avatar.player_id);
    const std::array<float, 7> expected_scalars{expected.avatar.position.x,
                                                expected.avatar.position.y,
                                                expected.avatar.position.z,
                                                expected.avatar.facing,
                                                expected.avatar.velocity.x,
                                                expected.avatar.velocity.y,
                                                expected.avatar.velocity.z};
    const std::array<float, 7> actual_scalars{actual.avatar.position.x,
                                              actual.avatar.position.y,
                                              actual.avatar.position.z,
                                              actual.avatar.facing,
                                              actual.avatar.velocity.x,
                                              actual.avatar.velocity.y,
                                              actual.avatar.velocity.z};
    for (std::size_t i = 0; i < expected_scalars.size(); ++i) {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(expected_scalars[i]),
                  std::bit_cast<std::uint32_t>(actual_scalars[i]));
    }
    EXPECT_EQ(expected.physics.size, actual.physics.size);
    EXPECT_EQ(expected.physics.bytes, actual.physics.bytes);
}

std::shared_ptr<Luminumbra::Chunk> OwnerFloor(PhysicsSystem& physics) {
    auto floor = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0));
    constexpr auto side = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1);
    floor->heightmap_data.assign(side * side, 8.0f);
    EXPECT_TRUE(physics.add_chunk_collision(*floor));
    return floor;
}

void SetOwnerWord(PhysicsOwnerState& state, std::size_t offset, std::uint32_t word) {
    ASSERT_LE(offset + 4, state.size);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        state.bytes[offset++] = static_cast<std::uint8_t>(word >> shift);
    }
}

std::uint32_t OwnerWord(const PhysicsOwnerState& state, std::size_t offset) {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        result |= static_cast<std::uint32_t>(state.bytes[offset++]) << shift;
    }
    return result;
}

TEST(PlayerAvatar, OwnerEncodingIsExactBoundedAndTransactional) {
    PhysicsSystem physics;
    physics.startup();
    const auto index = physics.create_avatar_character(Vec3(1.234567f, 40.125f, -0.0f));
    physics.set_avatar_wish_velocity(index, glm::vec2(0.1234567f, -0.7654321f));
    physics.update_avatars(1.0f / 30.0f);
    PlayerAvatarOwnerState original;
    original.avatar = {.player_id = 37,
                       .position = physics.get_avatar_position(index),
                       .facing = -0.0f,
                       .velocity = physics.get_avatar_velocity(index)};
    ASSERT_TRUE(physics.capture_avatar_owner(index, original.physics));
    std::array<std::uint8_t, Luminumbra::World::kMaxAvatarOwnerBytes> bytes{};
    std::size_t written = 0;
    ASSERT_TRUE(Luminumbra::World::EncodeAvatarOwner(original, bytes, written));
    EXPECT_EQ(bytes[0], 1);
    EXPECT_EQ(bytes[4], 37);
    // The sign of zero is part of the recipe, including little-endian float bits.
    EXPECT_EQ(bytes[23], 0x80);
    PlayerAvatarOwnerState decoded;
    ASSERT_TRUE(Luminumbra::World::DecodeAvatarOwner(std::span(bytes).first(written), decoded));
    std::array<std::uint8_t, Luminumbra::World::kMaxAvatarOwnerBytes> encoded{};
    std::size_t encoded_size = 0;
    ASSERT_TRUE(Luminumbra::World::EncodeAvatarOwner(decoded, encoded, encoded_size));
    ASSERT_EQ(written, encoded_size);
    EXPECT_EQ(bytes, encoded);
    const auto retained = decoded;
    for (std::size_t size = 0; size < written; ++size) {
        EXPECT_FALSE(Luminumbra::World::DecodeAvatarOwner(std::span(bytes).first(size), decoded));
        ExpectAvatarBits(retained, decoded);
    }
    auto malformed = bytes;
    malformed[0] = 2;
    EXPECT_FALSE(
        Luminumbra::World::DecodeAvatarOwner(std::span(malformed).first(written), decoded));
    malformed = bytes;
    malformed[10] = 0x80;
    malformed[11] = 0x7f; // non-finite avatar position
    EXPECT_FALSE(
        Luminumbra::World::DecodeAvatarOwner(std::span(malformed).first(written), decoded));
    malformed = bytes;
    malformed[40] = 2; // nested physics version
    EXPECT_FALSE(
        Luminumbra::World::DecodeAvatarOwner(std::span(malformed).first(written), decoded));
    EXPECT_FALSE(
        Luminumbra::World::DecodeAvatarOwner(std::span(bytes).first(written + 1), decoded));
    std::vector<std::uint8_t> oversized(bytes.size() + 1);
    EXPECT_FALSE(Luminumbra::World::DecodeAvatarOwner(oversized, decoded));
    ExpectAvatarBits(retained, decoded);
    bytes.fill(0xa5);
    const auto untouched = bytes;
    written = 123;
    EXPECT_FALSE(
        Luminumbra::World::EncodeAvatarOwner(original, std::span(bytes).first(39), written));
    EXPECT_EQ(written, 123u);
    EXPECT_EQ(bytes, untouched);
    original.avatar.velocity.x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(Luminumbra::World::EncodeAvatarOwner(original, bytes, written));
    EXPECT_EQ(written, 123u);
    EXPECT_EQ(bytes, untouched);
}

TEST(PlayerAvatar, OwnerEncodingPreservesSubnormalBits) {
    for (const auto bits : {1u, 0x00400000u, 0x007fffffu, 0x00800000u}) {
        PhysicsSystem source;
        source.startup();
        const Vec3 position(
            std::bit_cast<float>(bits), 30.0f, std::bit_cast<float>(bits | 0x80000000u));
        const auto index = source.create_avatar_character(position);
        PlayerAvatarOwnerState capture;
        capture.avatar = {
            .player_id = 0, .position = position, .facing = -0.0f, .velocity = Vec3(0.0f)};
        ASSERT_TRUE(source.capture_avatar_owner(index, capture.physics));
        std::array<std::uint8_t, Luminumbra::World::kMaxAvatarOwnerBytes> bytes{};
        std::size_t written = 0;
        ASSERT_TRUE(Luminumbra::World::EncodeAvatarOwner(capture, bytes, written));
        PlayerAvatarOwnerState decoded;
        ASSERT_TRUE(Luminumbra::World::DecodeAvatarOwner(std::span(bytes).first(written), decoded));
        ExpectAvatarBits(capture, decoded);
        PhysicsSystem restored;
        restored.startup();
        std::size_t restored_index = 999;
        ASSERT_TRUE(restored.restore_avatar_owner(decoded.physics, restored_index));
        PhysicsOwnerState actual;
        ASSERT_TRUE(restored.capture_avatar_owner(restored_index, actual));
        ExpectOwnerBits(capture.physics, actual);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(restored.get_avatar_position(restored_index).x),
                  bits);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(restored.get_avatar_position(restored_index).z),
                  bits | 0x80000000u);
    }
}

TEST(PlayerAvatar, OwnerContinuationPreservesFallingGroundAndSubMillimetreVelocity) {
    constexpr float dt = 1.0f / 30.0f;
    for (const int capture_tick : {3, 45}) {
        PhysicsSystem original;
        original.startup();
        const auto floor = OwnerFloor(original);
        const auto index = original.create_avatar_character(Vec3(4.123456f, 10.0f, 5.765432f));
        original.set_avatar_wish_velocity(index, glm::vec2(0.1234567f, 0.0765432f));
        for (int tick = 0; tick < capture_tick; ++tick) {
            original.update_avatars(dt);
        }
        ASSERT_EQ(original.is_avatar_grounded(index), capture_tick == 45);
        PhysicsOwnerState captured;
        ASSERT_TRUE(original.capture_avatar_owner(index, captured));
        PhysicsSystem restored;
        restored.startup();
        const auto restored_floor = OwnerFloor(restored);
        std::size_t restored_index = 999;
        ASSERT_TRUE(restored.restore_avatar_owner(captured, restored_index));
        const auto source_velocity = original.get_avatar_velocity(index);
        const auto restored_velocity = restored.get_avatar_velocity(restored_index);
        for (const auto axis : {0, 1, 2}) {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(source_velocity[axis]),
                      std::bit_cast<std::uint32_t>(restored_velocity[axis]));
        }
        PhysicsOwnerState recaptured;
        ASSERT_TRUE(restored.capture_avatar_owner(restored_index, recaptured));
        ExpectOwnerBits(captured, recaptured);
        // Exercise mutations through the real recipe reader: a rounded wish
        // velocity and a missing ground state must both break continuation.
        PhysicsSystem quantized;
        quantized.startup();
        const auto quantized_floor = OwnerFloor(quantized);
        auto rounded = captured;
        SetOwnerWord(rounded, 12, std::bit_cast<std::uint32_t>(0.123f));
        SetOwnerWord(rounded, 124, std::bit_cast<std::uint32_t>(0.123f));
        std::size_t rounded_index = 999;
        ASSERT_TRUE(quantized.restore_avatar_owner(rounded, rounded_index));
        PhysicsSystem ungrounded;
        ungrounded.startup();
        const auto ungrounded_floor = OwnerFloor(ungrounded);
        auto missing_ground = captured;
        SetOwnerWord(missing_ground, 24, capture_tick == 45 ? 3u : 0u);
        std::size_t ungrounded_index = 999;
        ASSERT_TRUE(ungrounded.restore_avatar_owner(missing_ground, ungrounded_index));
        bool velocity_mutation_observed = false;
        bool ground_mutation_observed = false;
        for (int tick = 0; tick < 20; ++tick) {
            original.update_avatars(dt);
            restored.update_avatars(dt);
            quantized.update_avatars(dt);
            ungrounded.update_avatars(dt);
            ASSERT_TRUE(original.capture_avatar_owner(index, captured));
            ASSERT_TRUE(restored.capture_avatar_owner(restored_index, recaptured));
            ExpectOwnerBits(captured, recaptured);
            ASSERT_TRUE(quantized.capture_avatar_owner(rounded_index, rounded));
            ASSERT_TRUE(ungrounded.capture_avatar_owner(ungrounded_index, missing_ground));
            velocity_mutation_observed |=
                original.get_avatar_position(index) !=
                    quantized.get_avatar_position(rounded_index) ||
                original.get_avatar_velocity(index) != quantized.get_avatar_velocity(rounded_index);
            ground_mutation_observed |= original.get_avatar_position(index) !=
                                            ungrounded.get_avatar_position(ungrounded_index) ||
                                        original.get_avatar_velocity(index) !=
                                            ungrounded.get_avatar_velocity(ungrounded_index);
        }
        EXPECT_TRUE(velocity_mutation_observed);
        EXPECT_TRUE(ground_mutation_observed);
    }
}

TEST(PlayerAvatar, LocalJumpAndCrouchContinueExactly) {
    constexpr float dt = 1.0f / 30.0f;
    PhysicsSystem original;
    original.startup();
    const auto floor = OwnerFloor(original);
    original.create_player_controller(Vec3(8.0f, 8.25f, 8.0f));
    for (int tick = 0; tick < 30; ++tick) {
        original.update_player(Vec3(0.0f), false, 6.0f, dt);
    }
    ASSERT_TRUE(original.is_player_grounded());
    ASSERT_TRUE(original.set_player_crouched(true));
    for (int tick = 0; tick < 3; ++tick) {
        original.update_player(Vec3(0.1234567f, 0.0f, 0.0765432f), tick == 0, 6.0f, dt);
    }
    ASSERT_FALSE(original.is_player_grounded());
    PhysicsOwnerState capture;
    ASSERT_TRUE(original.capture_player_owner(capture));
    PhysicsSystem restored;
    restored.startup();
    const auto restored_floor = OwnerFloor(restored);
    ASSERT_TRUE(restored.restore_player_owner(capture));
    EXPECT_FALSE(restored.restore_player_owner(capture)); // no replacement of an existing owner
    for (int tick = 0; tick < 20; ++tick) {
        original.update_player(Vec3(0.1234567f, 0.0f, 0.0765432f), false, 6.0f, dt);
        restored.update_player(Vec3(0.1234567f, 0.0f, 0.0765432f), false, 6.0f, dt);
        PhysicsOwnerState actual;
        ASSERT_TRUE(original.capture_player_owner(capture));
        ASSERT_TRUE(restored.capture_player_owner(actual));
        ExpectOwnerBits(capture, actual);
    }
}

TEST(PlayerAvatar, BodyContinuationPreservesSpinContactsAndSleep) {
    constexpr float dt = 1.0f / 30.0f;
    for (const int capture_tick : {6, 12, 90, 240}) {
        PhysicsSystem original;
        original.startup();
        const auto floor = OwnerFloor(original);
        const Vec3 initial_velocity = capture_tick == 6 || capture_tick == 240
                                          ? Vec3(0.0f)
                                          : Vec3(0.1234567f, 0.0f, 0.0765432f);
        const auto body = original.create_dynamic_sphere(
            Vec3(5.0f, capture_tick == 6 ? 8.3f : 8.4f, 5.0f), initial_velocity, 0.3f);
        ASSERT_FALSE(body.IsInvalid());
        for (int tick = 0; tick < capture_tick; ++tick) {
            original.update(dt);
        }
        PhysicsOwnerState capture;
        ASSERT_TRUE(original.capture_body_owner(body, capture));
        if (capture_tick == 6) {
            EXPECT_TRUE(original.body_is_active(body));
            EXPECT_GT(std::bit_cast<float>(OwnerWord(capture, 180)), 0.0f); // sleep timer
        }
        if (capture_tick == 12) {
            // v1: header(16), active(4), position(24), rotation(16), velocity(12), spin xyz.
            EXPECT_TRUE(std::bit_cast<float>(OwnerWord(capture, 72)) != 0.0f ||
                        std::bit_cast<float>(OwnerWord(capture, 80)) != 0.0f);
        }
        if (capture_tick == 240) {
            EXPECT_FALSE(original.body_is_active(body));
        }
        PhysicsSystem restored;
        restored.startup();
        const auto restored_floor = OwnerFloor(restored);
        std::array<JPH::BodyID, 1> ids{};
        ASSERT_TRUE(restored.restore_body_owners(std::span(&capture, 1), ids));
        EXPECT_EQ(ids[0], body);
        PhysicsOwnerState actual;
        ASSERT_TRUE(restored.capture_body_owner(ids[0], actual));
        ExpectOwnerBits(capture, actual);
        for (int tick = 0; tick < 20; ++tick) {
            original.update(dt);
            restored.update(dt);
            ASSERT_TRUE(original.capture_body_owner(body, capture));
            ASSERT_TRUE(restored.capture_body_owner(ids[0], actual));
            ExpectOwnerBits(capture, actual);
        }
    }
}

TEST(PlayerAvatar, MalformedPhysicsCannotPublishAnOwner) {
    PhysicsSystem source;
    source.startup();
    const auto index = source.create_avatar_character(Vec3(1.0f, 30.0f, 2.0f));
    PhysicsOwnerState capture;
    ASSERT_TRUE(source.capture_avatar_owner(index, capture));
    PhysicsSystem target;
    target.startup();
    for (std::uint32_t size = 0; size < capture.size; ++size) {
        auto partial = capture;
        partial.size = size;
        std::size_t destination = 123;
        EXPECT_FALSE(target.restore_avatar_owner(partial, destination));
        EXPECT_EQ(destination, 123u);
        EXPECT_EQ(target.avatar_character_slot_count(), 0u);
    }
    for (const auto& [offset, word] :
         std::array<std::pair<std::size_t, std::uint32_t>, 12>{{{0, 2},
                                                                {4, 99},
                                                                {8, 9},
                                                                {12, 0x7f800000},
                                                                {20, 0x80000000},
                                                                {24, 4},
                                                                {144, UINT32_MAX},
                                                                {140, 2},
                                                                {88, 0x7fefffff},
                                                                {136, 0xbf800000},
                                                                {108, 0x3b800000},
                                                                {60, 0x40000000}}}) {
        auto invalid = capture;
        SetOwnerWord(invalid, offset, word);
        std::size_t destination = 123;
        EXPECT_FALSE(target.restore_avatar_owner(invalid, destination)) << offset;
        EXPECT_EQ(destination, 123u);
        EXPECT_EQ(target.avatar_character_slot_count(), 0u);
    }
    capture.size = static_cast<std::uint32_t>(capture.bytes.size() + 1);
    std::size_t destination = 123;
    EXPECT_FALSE(target.restore_avatar_owner(capture, destination));
    EXPECT_EQ(destination, 123u);
    EXPECT_EQ(target.avatar_character_slot_count(), 0u);
}

TEST(PlayerAvatar, FrozenCapsuleTopologyCanPrecedeControllerAdoption) {
    PhysicsSystem original;
    original.startup();
    const auto floor = OwnerFloor(original);
    const auto index = original.create_avatar_character(Vec3(8.0f, 8.0f, 8.0f));
    original.update_avatars(1.0f / 30.0f);
    original.set_avatar_frozen(index, true, original.get_avatar_position(index));
    PhysicsOwnerState capture;
    ASSERT_TRUE(original.capture_avatar_owner(index, capture));
    PhysicsSystem restored;
    restored.startup();
    const auto restored_floor = OwnerFloor(restored);
    std::array<JPH::BodyID, 1> ids{};
    ASSERT_TRUE(restored.restore_body_owners(std::span(&capture, 1), ids));
    std::size_t restored_index = 999;
    ASSERT_TRUE(restored.restore_avatar_owner(capture, restored_index));
    std::size_t duplicate = 999;
    EXPECT_FALSE(restored.restore_avatar_owner(capture, duplicate));
    EXPECT_EQ(duplicate, 999u);
    ASSERT_EQ(restored.avatar_character_slot_count(), 1u);
    PhysicsOwnerState actual;
    ASSERT_TRUE(restored.capture_avatar_owner(restored_index, actual));
    ExpectOwnerBits(capture, actual);
    for (const bool frozen : {true, false}) {
        original.set_avatar_frozen(index, frozen, original.get_avatar_position(index));
        restored.set_avatar_frozen(
            restored_index, frozen, restored.get_avatar_position(restored_index));
        original.set_avatar_wish_velocity(index, glm::vec2(0.1234567f, 0.0f));
        restored.set_avatar_wish_velocity(restored_index, glm::vec2(0.1234567f, 0.0f));
        original.update_avatars(1.0f / 30.0f);
        restored.update_avatars(1.0f / 30.0f);
        ASSERT_TRUE(original.capture_avatar_owner(index, capture));
        ASSERT_TRUE(restored.capture_avatar_owner(restored_index, actual));
        ExpectOwnerBits(capture, actual);
    }
}

TEST(PlayerAvatar, BodySetRestorationRefusesBeforePublishingAnyBody) {
    PhysicsSystem source;
    source.startup();
    const auto floor = OwnerFloor(source);
    const auto body =
        source.create_dynamic_sphere(Vec3(8.0f, 8.4f, 8.0f), Vec3(0.1234567f, 0.0f, 0.0f), 0.3f);
    for (int tick = 0; tick < 12; ++tick) {
        source.update(1.0f / 30.0f);
    }
    PhysicsOwnerState capture;
    ASSERT_TRUE(source.capture_body_owner(body, capture));
    std::array<JPH::BodyID, 1> ids{JPH::BodyID()};
    PhysicsSystem unopened;
    EXPECT_FALSE(unopened.restore_body_owners(std::span(&capture, 1), ids));
    PhysicsSystem target;
    target.startup();
    EXPECT_FALSE(target.restore_body_owners(std::span(&capture, 1), ids)); // missing terrain
    EXPECT_TRUE(ids[0].IsInvalid());
    const auto restored_floor = OwnerFloor(target);
    EXPECT_TRUE(target.restore_body_owners({}, {}));
    const std::vector<PhysicsOwnerState> oversized(PhysicsSystem::kMaxRestoredBodies + 1);
    std::vector<JPH::BodyID> oversized_ids(oversized.size());
    EXPECT_FALSE(target.restore_body_owners(oversized, oversized_ids));
    EXPECT_FALSE(target.restore_body_owners(std::span(&capture, 1), {}));
    std::array<PhysicsOwnerState, 2> duplicates{capture, capture};
    std::array<JPH::BodyID, 2> duplicate_ids{};
    EXPECT_FALSE(target.restore_body_owners(duplicates, duplicate_ids));
    for (std::uint32_t size = 0; size < capture.size; ++size) {
        auto truncated = capture;
        truncated.size = size;
        EXPECT_FALSE(target.restore_body_owners(std::span(&truncated, 1), ids));
        EXPECT_TRUE(ids[0].IsInvalid());
    }
    // v1 sphere fields: version, kind, body ID, radius, active, position, rotation.
    ASSERT_GE(capture.size, 280u);
    ASSERT_GT(OwnerWord(capture, 208), 0u); // body pairs
    ASSERT_GT(OwnerWord(capture, 244), 0u); // first pair manifolds
    for (const auto& [offset, word] :
         std::array<std::pair<std::size_t, std::uint32_t>, 19>{{{0, 2},
                                                                {4, 99},
                                                                {8, 0x80000000},
                                                                {12, 0},
                                                                {16, 2},
                                                                {44, 0x7f800000},
                                                                {24, 0x7fefffff},
                                                                {180, 0xbf800000},
                                                                {184, 2},
                                                                {188, 15},
                                                                {192, 0xbf800000},
                                                                {196, 0x7f800000},
                                                                {208, UINT32_MAX},
                                                                {44, 0x3b800000},
                                                                {268, 0x40000000},
                                                                {12, 1},
                                                                {12, 0x7f7fffff},
                                                                {112, 0x7fefffff},
                                                                {144, 0xbf800000}}}) {
        auto invalid = capture;
        SetOwnerWord(invalid, offset, word);
        EXPECT_FALSE(target.restore_body_owners(std::span(&invalid, 1), ids));
        EXPECT_TRUE(ids[0].IsInvalid());
    }
    for (const auto offset : {212u, 248u}) {
        auto reversed = capture;
        const auto second = offset + (offset == 212u ? 4u : 8u);
        SetOwnerWord(reversed, offset, OwnerWord(capture, second));
        SetOwnerWord(reversed, second, OwnerWord(capture, offset));
        EXPECT_FALSE(target.restore_body_owners(std::span(&reversed, 1), ids));
        EXPECT_TRUE(ids[0].IsInvalid());
    }
    ASSERT_TRUE(target.restore_body_owners(std::span(&capture, 1), ids));
    const auto retained_id = ids[0];
    EXPECT_FALSE(target.restore_body_owners(std::span(&capture, 1), ids)); // occupied body identity
    EXPECT_EQ(ids[0], retained_id);
    PhysicsOwnerState actual;
    ASSERT_TRUE(target.capture_body_owner(ids[0], actual));
    ExpectOwnerBits(capture, actual);
    source.destroy_body(body);
    EXPECT_FALSE(source.capture_body_owner(body, actual)); // stale handle
    EXPECT_FALSE(source.capture_body_owner(JPH::BodyID(), actual));
    ExpectOwnerBits(capture, actual);
}

TEST(PlayerAvatar, BodySetPreservesSharedContactsAndFrozenDependencies) {
    PhysicsSystem source;
    source.startup();
    const auto floor = OwnerFloor(source);
    const auto avatar = source.create_avatar_character(Vec3(9.0f, 8.0f, 8.0f));
    source.set_avatar_frozen(avatar, true, source.get_avatar_position(avatar));
    const std::array<JPH::BodyID, 2> original_ids{
        source.create_dynamic_sphere(Vec3(7.8f, 8.31f, 8.0f), Vec3(0.1234567f, 0.0f, 0.0f), 0.3f),
        source.create_dynamic_sphere(Vec3(8.4f, 8.31f, 8.0f), Vec3(-0.0765432f, 0.0f, 0.0f), 0.3f)};
    for (int tick = 0; tick < 12; ++tick) {
        source.update(1.0f / 30.0f);
    }
    // Reverse body order to exercise stable body identities and disjoint pair ownership.
    std::array<PhysicsOwnerState, 3> states{};
    ASSERT_TRUE(source.capture_body_owner(original_ids[1], states[0]));
    ASSERT_TRUE(source.capture_avatar_owner(avatar, states[1]));
    ASSERT_TRUE(source.capture_body_owner(original_ids[0], states[2]));
    PhysicsSystem restored;
    restored.startup();
    const auto restored_floor = OwnerFloor(restored);
    std::array<JPH::BodyID, 3> ids{};
    auto mismatched = states;
    SetOwnerWord(mismatched[2], 200, std::bit_cast<std::uint32_t>(-5.0f));
    EXPECT_FALSE(restored.restore_body_owners(mismatched, ids));
    EXPECT_TRUE(std::all_of(ids.begin(), ids.end(), [](JPH::BodyID id) { return id.IsInvalid(); }));
    ASSERT_TRUE(restored.restore_body_owners(states, ids));
    std::size_t restored_avatar = 999;
    ASSERT_TRUE(restored.restore_avatar_owner(states[1], restored_avatar));
    for (int tick = 0; tick < 20; ++tick) {
        source.update(1.0f / 30.0f);
        restored.update(1.0f / 30.0f);
        PhysicsOwnerState expected;
        PhysicsOwnerState actual;
        for (const std::size_t index : {0u, 1u}) {
            ASSERT_TRUE(source.capture_body_owner(original_ids[index], expected));
            ASSERT_TRUE(restored.capture_body_owner(index == 0 ? ids[2] : ids[0], actual));
            ExpectOwnerBits(expected, actual);
        }
        ASSERT_TRUE(source.capture_avatar_owner(avatar, expected));
        ASSERT_TRUE(restored.capture_avatar_owner(restored_avatar, actual));
        ExpectOwnerBits(expected, actual);
    }
}

} // namespace
