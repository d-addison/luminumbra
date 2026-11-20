//  the server-authoritative player avatar primitives.
// Proves the deterministic spawn layout + the entity-snapshot projection are
// pure/reproducible and that an empty avatar set yields the empty snapshot
// (byte-identical to the zero-avatar headless `entities` lane). The avatar->streaming
// ->entities-sub-hash lane end-to-end is validated by `--smoke --avatars N`
// (the existing HeadlessServerTick double-run asserts the entities sub-hash
// matches run-to-run).
#include <gtest/gtest.h>

#include "../support/GameSessionTestWorld.h"
#include "luminumbra_common/core/JobSystem.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../fixtures/AvatarContinuationWorld.h"
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
using Luminumbra::World::DecodeAvatarOwner;
using Luminumbra::World::EncodeAvatarOwner;
using Luminumbra::World::PhysicsOwnerState;
using Luminumbra::World::PlayerAvatarOwnerState;
using Luminumbra::World::ValidateAvatarOwner;
using Luminumbra::World::ValidatePhysicsOwner;

#ifdef JPH_DOUBLE_PRECISION
constexpr std::uint32_t kFixtureVersion = 0x00010001u;
#else
constexpr std::uint32_t kFixtureVersion = 1;
#endif
constexpr std::uint32_t kFixtureBody = 0x01000000u;

// Fixtures use scalar words, independently of the production reader's layout.
void SetPhysicsWord(PhysicsOwnerState& state, std::size_t offset, std::uint32_t word) {
    ASSERT_LE(offset + 4, state.bytes.size());
    for (unsigned byte = 0; byte < 4; ++byte) {
        state.bytes[offset + byte] = static_cast<std::uint8_t>(word >> (8 * byte));
    }
}

struct OwnerFixture {
    std::vector<std::uint32_t> words;
    void append(std::initializer_list<std::uint32_t> values) {
        words.insert(words.end(), values);
    }
    void zeros(std::size_t count) {
        words.insert(words.end(), count, 0);
    }
    PhysicsOwnerState state() const {
        PhysicsOwnerState result;
        for (std::size_t i = 0; i < words.size(); ++i) {
            SetPhysicsWord(result, 4 * i, words[i]);
        }
        result.size = static_cast<std::uint32_t>(4 * words.size());
        return result;
    }
};

OwnerFixture CharacterFixture(bool contact = false) {
    OwnerFixture fixture;
    fixture.append({kFixtureVersion, 1, 0, 0, 0, UINT32_MAX, 3, UINT32_MAX, UINT32_MAX});
    fixture.zeros(18);                      // ground position, normal, velocity, and feet position
    fixture.append({0, 0, 0, 0x3f800000u}); // identity quaternion
    fixture.zeros(5);                       // velocity, previous delta, hit limit
    fixture.append({contact ? 1u : 0u});
    if (contact) {
        fixture.append({kFixtureBody + 1, UINT32_MAX, 0});
        fixture.zeros(9);                                       // position and velocity
        fixture.append({0, 0x3f800000u, 0, 0, 0x3f800000u, 0}); // normals
        fixture.append({0, 0, 2, 0, 1, 0, 1}); // distance, fraction, motion and flags
    }
    return fixture;
}

OwnerFixture SphereFixture(std::uint32_t pairs = 1,
                           std::uint32_t manifolds = 1,
                           std::uint32_t points = 1,
                           std::uint32_t ccd = 1,
                           std::uint32_t free_slots = 0) {
    OwnerFixture fixture;
    fixture.append({kFixtureVersion, 3, kFixtureBody, 0x3f000000u, 1});
    fixture.zeros(6); // position
    fixture.append({0, 0, 0, 0x3f800000u});
    fixture.zeros(32); // velocity, spin, force, torque, sleep offsets/spheres/timer/flag
    fixture.append({5, 0, 0, 0xc1200000u, 0, pairs}); // scene flags, delta, gravity
    for (std::uint32_t pair = 0; pair < pairs; ++pair) {
        fixture.append({kFixtureBody, kFixtureBody + pair + 1});
        fixture.zeros(6); // relative position and compressed rotation
        fixture.append({manifolds});
        for (std::uint32_t manifold = 0; manifold < manifolds; ++manifold) {
            fixture.append(
                {kFixtureBody, manifold, kFixtureBody + pair + 1, 0, points, 0, 0x3f800000u, 0});
            fixture.zeros(9 * points);
        }
    }
    fixture.append({ccd});
    for (std::uint32_t i = 0; i < ccd; ++i) {
        fixture.append({kFixtureBody, 1000 + i, kFixtureBody + 1, 0});
    }
    fixture.append({10240, free_slots});
    for (std::uint32_t i = 0; i < free_slots; ++i) {
        fixture.append({kFixtureBody + pairs + 1 + i});
    }
    return fixture;
}

void ExpectAvatarUnchanged(const PlayerAvatarOwnerState& expected,
                           const PlayerAvatarOwnerState& actual) {
    EXPECT_EQ(expected.avatar.player_id, actual.avatar.player_id);
    for (int axis = 0; axis < 3; ++axis) {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.avatar.position[axis]),
                  std::bit_cast<std::uint32_t>(actual.avatar.position[axis]));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.avatar.velocity[axis]),
                  std::bit_cast<std::uint32_t>(actual.avatar.velocity[axis]));
    }
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.avatar.facing),
              std::bit_cast<std::uint32_t>(actual.avatar.facing));
    EXPECT_EQ(expected.physics.size, actual.physics.size);
    EXPECT_EQ(expected.physics.bytes, actual.physics.bytes);
}

std::vector<std::uint8_t> RawAvatarBytes(const PlayerAvatarOwnerState& state) {
    // Deliberately bypass Encode: malformed bytes must reach Decode unchanged.
    OwnerFixture header;
    header.append({1, state.avatar.player_id});
    for (const auto scalar : {state.avatar.position.x,
                              state.avatar.position.y,
                              state.avatar.position.z,
                              state.avatar.facing,
                              state.avatar.velocity.x,
                              state.avatar.velocity.y,
                              state.avatar.velocity.z}) {
        header.append({std::bit_cast<std::uint32_t>(scalar)});
    }
    header.append({state.physics.size});
    const auto prefix = header.state();
    std::vector<std::uint8_t> bytes(prefix.bytes.begin(), prefix.bytes.begin() + prefix.size);
    const auto count = std::min<std::size_t>(state.physics.size, state.physics.bytes.size());
    bytes.insert(bytes.end(), state.physics.bytes.begin(), state.physics.bytes.begin() + count);
    return bytes;
}

void ExpectAvatarRefused(const PlayerAvatarOwnerState& malformed) {
    EXPECT_FALSE(ValidateAvatarOwner(malformed));
    std::array<std::uint8_t, Luminumbra::World::kMaxAvatarOwnerBytes> bytes;
    bytes.fill(0xa5);
    const auto retained_bytes = bytes;
    std::size_t written = 123;
    EXPECT_FALSE(EncodeAvatarOwner(malformed, bytes, written));
    EXPECT_EQ(written, 123u);
    EXPECT_EQ(bytes, retained_bytes);
    PlayerAvatarOwnerState decoded;
    decoded.avatar.player_id = 77;
    decoded.physics.bytes.fill(0x5a);
    decoded.physics.size = 7;
    const auto retained = decoded;
    EXPECT_FALSE(DecodeAvatarOwner(RawAvatarBytes(malformed), decoded));
    ExpectAvatarUnchanged(retained, decoded);
}

TEST(PlayerAvatarOwnerCodec, VersionOneLittleEndianGoldenAndRoundTrip) {
    PlayerAvatarOwnerState original;
    original.avatar.player_id = 0x12345678u;
    original.avatar.facing = -0.0f;
    original.physics = CharacterFixture().state();
    ASSERT_EQ(original.physics.size, 148u);
    std::array<std::uint8_t, 188> golden{};
    golden[0] = 1;
    golden[4] = 0x78;
    golden[5] = 0x56;
    golden[6] = 0x34;
    golden[7] = 0x12;
    golden[23] = 0x80; // yaw sign bit
    golden[36] = 148;
    golden[40] = 1;
#ifdef JPH_DOUBLE_PRECISION
    golden[42] = 1;
#endif
    golden[44] = 1; // avatar kind
    for (const auto start : {60u, 68u, 72u}) {
        std::fill_n(golden.begin() + start, 4, 0xff);
    }
    golden[64] = 3; // unsupported ground
    golden[162] = 0x80;
    golden[163] = 0x3f; // quaternion w = 1
    std::array<std::uint8_t, 188> bytes{};
    std::size_t written = 0;
    ASSERT_TRUE(EncodeAvatarOwner(original, bytes, written));
    EXPECT_EQ(written, golden.size());
    EXPECT_EQ(bytes, golden);
    PlayerAvatarOwnerState decoded;
    ASSERT_TRUE(DecodeAvatarOwner(golden, decoded));
    ExpectAvatarUnchanged(original, decoded);
    ASSERT_TRUE(EncodeAvatarOwner(decoded, bytes, written));
    EXPECT_EQ(bytes, golden);
}

TEST(PlayerAvatarOwnerCodec, ExactPositionsVelocitiesAndSignedSubnormalsRoundTrip) {
    for (const auto bits :
         {0u, 0x80000000u, 1u, 0x80000001u, 0x007fffffu, 0x00800000u, 0x3f9e0651u, 0x7f7fffffu}) {
        PlayerAvatarOwnerState original;
        original.physics = CharacterFixture(true).state();
        const auto value = std::bit_cast<float>(bits);
        original.avatar.position.x = value;
        original.avatar.velocity.z = value;
        const auto wide = std::bit_cast<std::uint64_t>(static_cast<double>(value));
        SetPhysicsWord(original.physics, 84, static_cast<std::uint32_t>(wide));
        SetPhysicsWord(original.physics, 88, static_cast<std::uint32_t>(wide >> 32));
        SetPhysicsWord(original.physics, 132, bits);
        std::array<std::uint8_t, Luminumbra::World::kMaxAvatarOwnerBytes> bytes{};
        std::size_t written = 0;
        ASSERT_TRUE(EncodeAvatarOwner(original, bytes, written)) << bits;
        PlayerAvatarOwnerState decoded;
        ASSERT_TRUE(DecodeAvatarOwner(std::span(bytes).first(written), decoded));
        ExpectAvatarUnchanged(original, decoded);
    }
}

TEST(PlayerAvatarOwnerCodec, RefusesEveryTruncationOversizeAndHeaderWithoutPublishing) {
    PlayerAvatarOwnerState original;
    original.physics = CharacterFixture(true).state();
    for (std::uint32_t size = 0; size < original.physics.size; ++size) {
        auto malformed = original;
        malformed.physics.size = size;
        ExpectAvatarRefused(malformed);
    }
    original.physics.size = PhysicsOwnerState::kMaxBytes + 1;
    ExpectAvatarRefused(original);
    original.physics = CharacterFixture().state();
    auto bytes = RawAvatarBytes(original);
    PlayerAvatarOwnerState decoded;
    decoded.avatar.player_id = 99;
    const auto retained = decoded;
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_FALSE(DecodeAvatarOwner(std::span(bytes).first(size), decoded));
        ExpectAvatarUnchanged(retained, decoded);
    }
    for (const auto version : {0u, 2u, UINT32_MAX}) {
        bytes[0] = static_cast<std::uint8_t>(version);
        EXPECT_FALSE(DecodeAvatarOwner(bytes, decoded));
        ExpectAvatarUnchanged(retained, decoded);
    }
    bytes = RawAvatarBytes(original);
    bytes.push_back(0);
    EXPECT_FALSE(DecodeAvatarOwner(bytes, decoded));
    bytes.resize(Luminumbra::World::kMaxAvatarOwnerBytes + 1);
    EXPECT_FALSE(DecodeAvatarOwner(bytes, decoded));
    ExpectAvatarUnchanged(retained, decoded);
    std::array<std::uint8_t, 188> output{};
    output.fill(0xa5);
    const auto retained_output = output;
    for (const auto size : {0u, 39u, 40u, 187u}) {
        std::size_t written = 123;
        EXPECT_FALSE(EncodeAvatarOwner(original, std::span(output).first(size), written));
        EXPECT_EQ(written, 123u);
        EXPECT_EQ(output, retained_output);
    }
}

TEST(PlayerAvatarOwnerCodec, RefusesEveryAvatarScalarAndProjectionMismatch) {
    for (std::size_t scalar = 0; scalar < 7; ++scalar) {
        for (const auto invalid : {0x7f800000u, 0xff800000u, 0x7fc00000u}) {
            PlayerAvatarOwnerState malformed;
            malformed.physics = CharacterFixture().state();
            std::array<float*, 7> fields{&malformed.avatar.position.x,
                                         &malformed.avatar.position.y,
                                         &malformed.avatar.position.z,
                                         &malformed.avatar.facing,
                                         &malformed.avatar.velocity.x,
                                         &malformed.avatar.velocity.y,
                                         &malformed.avatar.velocity.z};
            *fields[scalar] = std::bit_cast<float>(invalid);
            ExpectAvatarRefused(malformed);
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        PlayerAvatarOwnerState malformed;
        malformed.physics = CharacterFixture().state();
        malformed.avatar.position[axis] = 1.0f;
        ExpectAvatarRefused(malformed);
        malformed.avatar.position[axis] = 0.0f;
        malformed.avatar.velocity[axis] = -0.0f; // mismatch includes the sign of zero
        ExpectAvatarRefused(malformed);
    }
}

TEST(PlayerAvatarOwnerCodec, RefusesMalformedCharacterFieldsAndCrossPrecision) {
    const auto original = CharacterFixture(true).state();
    const std::vector<std::pair<std::size_t, std::uint32_t>> invalid_words{
        {0, 0},
        {0, 2},
        {0, kFixtureVersion ^ 0x00010000u},
        {4, 0},
        {4, 4},
        {8, 2},
        {12, 0x7f800000u},
        {16, 0x7fc00000u},
        {20, 0x80000000u},
        {20, kFixtureBody},
        {24, 4},
        {28, 0x80000000u},
        {28, 10240},
        {40, 0x7ff00000u},
        {60, 0x40000000u},
        {72, 0x7f800000u},
        {88, 0x7ff00000u},
        {108, 0x3b800000u},
        {120, 0},
        {124, 0x7fc00000u},
        {136, 0xbf800000u},
        {140, 2},
        {144, 257},
        {148, UINT32_MAX},
        {148, 0x80000000u},
        {152, 0},
        {164, 0x7ff00000u},
        {184, 0x7f800000u},
        {196, 0x40000000u},
        {208, 0x40000000u},
        {220, 0x7f800000u},
        {224, 0x7fc00000u},
        {228, 3},
        {232, 2},
        {236, 2},
        {240, 2},
        {244, 2}};
    ASSERT_TRUE(ValidatePhysicsOwner(original));
    for (const auto& [offset, word] : invalid_words) {
        SCOPED_TRACE(offset);
        PlayerAvatarOwnerState malformed;
        malformed.physics = original;
        SetPhysicsWord(malformed.physics, offset, word);
        EXPECT_FALSE(ValidatePhysicsOwner(malformed.physics));
        ExpectAvatarRefused(malformed);
    }
    auto trailing = original;
    trailing.size += 4;
    EXPECT_FALSE(ValidatePhysicsOwner(trailing));
    for (const auto offset : {36u, 84u, 160u}) {
        auto malformed = original;
        SetPhysicsWord(malformed, offset + 4, 0x7fefffffu); // exceeds float range
        EXPECT_FALSE(ValidatePhysicsOwner(malformed));
#ifndef JPH_DOUBLE_PRECISION
        malformed = original;
        SetPhysicsWord(malformed, offset, 1); // finite, not float32-exact
        SetPhysicsWord(malformed, offset + 4, 0x3ff00000u);
        EXPECT_FALSE(ValidatePhysicsOwner(malformed));
#endif
    }
}

TEST(PlayerAvatarOwnerCodec, ChecksBothControllerKindsAndFrozenFlags) {
    auto moving = CharacterFixture().state();
    SetPhysicsWord(moving, 12, 0x3f800000u);
    EXPECT_TRUE(ValidatePhysicsOwner(moving));
    auto frozen = CharacterFixture().state();
    SetPhysicsWord(frozen, 8, 1);
    EXPECT_TRUE(ValidatePhysicsOwner(frozen)); // frozen, no capsule
    frozen.size += 8;
    SetPhysicsWord(frozen, 20, kFixtureBody);
    SetPhysicsWord(frozen, 148, 1);
    SetPhysicsWord(frozen, 152, 0);
    EXPECT_TRUE(ValidatePhysicsOwner(frozen));
    SetPhysicsWord(frozen, 12, 0x3f800000u);
    EXPECT_FALSE(ValidatePhysicsOwner(frozen));
    for (const auto flag : {0u, 2u}) {
        auto local = CharacterFixture().state();
        SetPhysicsWord(local, 4, 2);
        SetPhysicsWord(local, 8, flag);
        ASSERT_TRUE(ValidatePhysicsOwner(local));
        PlayerAvatarOwnerState wrong_kind;
        wrong_kind.physics = local;
        ExpectAvatarRefused(wrong_kind);
        SetPhysicsWord(local, 12, 0x3f800000u);
        EXPECT_FALSE(ValidatePhysicsOwner(local));
        local = CharacterFixture().state();
        SetPhysicsWord(local, 4, 2);
        SetPhysicsWord(local, 8, flag);
        SetPhysicsWord(local, 20, kFixtureBody);
        EXPECT_FALSE(ValidatePhysicsOwner(local));
    }
    auto local = CharacterFixture().state();
    SetPhysicsWord(local, 4, 2);
    SetPhysicsWord(local, 8, 1);
    EXPECT_FALSE(ValidatePhysicsOwner(local));
}

TEST(PlayerAvatarOwnerCodec, RefusesMalformedSphereSceneAndAllocatorFields) {
    const auto original = SphereFixture().state();
    ASSERT_EQ(original.size, 344u);
    ASSERT_TRUE(ValidatePhysicsOwner(original));
    for (std::uint32_t size = 0; size < original.size; ++size) {
        auto truncated = original;
        truncated.size = size;
        EXPECT_FALSE(ValidatePhysicsOwner(truncated));
    }
    const std::vector<std::pair<std::size_t, std::uint32_t>> invalid_words{{8, UINT32_MAX},
                                                                           {8, 0x80000000u},
                                                                           {8, 10240},
                                                                           {12, 0},
                                                                           {12, 1},
                                                                           {12, 0x7f7fffffu},
                                                                           {12, 0x7f800000u},
                                                                           {12, 0xbf800000u},
                                                                           {16, 2},
                                                                           {24, 0x7ff00000u},
                                                                           {44, 0x3b800000u},
                                                                           {56, 0},
                                                                           {60, 0x7f800000u},
                                                                           {108, 0x7ff00000u},
                                                                           {144, 0xbf800000u},
                                                                           {160, 0xbf800000u},
                                                                           {176, 0xbf800000u},
                                                                           {180, 0xbf800000u},
                                                                           {184, 2},
                                                                           {188, 15},
                                                                           {192, 0xbf800000u},
                                                                           {196, 0x7f800000u},
                                                                           {208, 65},
                                                                           {212, kFixtureBody + 2},
                                                                           {216, kFixtureBody},
                                                                           {220, 0x7f800000u},
                                                                           {244, 257},
                                                                           {248, kFixtureBody + 1},
                                                                           {256, kFixtureBody + 2},
                                                                           {264, 0},
                                                                           {264, 5},
                                                                           {268, 0x40000000u},
                                                                           {280, 0x7fc00000u},
                                                                           {316, 257},
                                                                           {320, kFixtureBody + 2},
                                                                           {328, UINT32_MAX},
                                                                           {336, 0},
                                                                           {336, 10241},
                                                                           {340, 10240}};
    for (const auto& [offset, word] : invalid_words) {
        SCOPED_TRACE(offset);
        auto malformed = original;
        SetPhysicsWord(malformed, offset, word);
        EXPECT_FALSE(ValidatePhysicsOwner(malformed));
    }
    // 1e-13 underflows inertia to zero and 1e-9 leaves a subnormal inertia whose
    // reciprocal overflows, both while mass stays invertible.
    for (const float radius : {1.0e-14f, 1.0e-13f, 1.0e-9f, 1.0e8f}) {
        auto invalid_radius = original;
        SetPhysicsWord(invalid_radius, 12, std::bit_cast<std::uint32_t>(radius));
        EXPECT_FALSE(ValidatePhysicsOwner(invalid_radius));
    }
    auto empty = SphereFixture(0, 0, 0, 0).state();
    EXPECT_TRUE(ValidatePhysicsOwner(empty));
    empty.size += 4;
    EXPECT_FALSE(ValidatePhysicsOwner(empty));
}

TEST(PlayerAvatarOwnerCodec, RejectsEveryNonUnitQuaternionAndCachedRotation) {
    for (const auto component : {0u, 4u, 8u, 12u}) {
        for (const auto word : {0x40000000u, 0x3b800000u, 0x7f7fffffu, 0x7f800000u, 0x7fc00000u}) {
            auto character = CharacterFixture().state();
            SetPhysicsWord(character, 108 + component, word);
            EXPECT_FALSE(ValidatePhysicsOwner(character));
            PlayerAvatarOwnerState malformed;
            malformed.physics = character;
            ExpectAvatarRefused(malformed);
            auto sphere = SphereFixture().state();
            SetPhysicsWord(sphere, 44 + component, word);
            EXPECT_FALSE(ValidatePhysicsOwner(sphere));
        }
    }
    for (const auto component : {232u, 236u, 240u}) {
        for (const float value : {2.0f,
                                  1.0001f,
                                  (std::numeric_limits<float>::max)(),
                                  std::numeric_limits<float>::infinity(),
                                  std::numeric_limits<float>::quiet_NaN()}) {
            auto malformed = SphereFixture().state();
            SetPhysicsWord(malformed, component, std::bit_cast<std::uint32_t>(value));
            EXPECT_FALSE(ValidatePhysicsOwner(malformed));
        }
    }
    for (const float x : {0.0f, 1.0f, 1.00002f}) {
        auto valid = SphereFixture().state();
        SetPhysicsWord(valid, 232, std::bit_cast<std::uint32_t>(x));
        EXPECT_TRUE(ValidatePhysicsOwner(valid));
    }
    auto half_turn = CharacterFixture().state();
    SetPhysicsWord(half_turn, 108, 0x3f800000u);
    SetPhysicsWord(half_turn, 120, 0);
    EXPECT_TRUE(ValidatePhysicsOwner(half_turn));
}

TEST(PlayerAvatarOwnerCodec, RejectsDuplicateUnorderedAndMismatchedTopology) {
    auto duplicate_ccd = SphereFixture(1, 1, 1, 2).state();
    ASSERT_TRUE(ValidatePhysicsOwner(duplicate_ccd));
    SetPhysicsWord(duplicate_ccd, 340, 1000); // second CCD key duplicates the first
    EXPECT_FALSE(ValidatePhysicsOwner(duplicate_ccd));
    auto duplicate_manifold = SphereFixture(1, 2).state();
    ASSERT_TRUE(ValidatePhysicsOwner(duplicate_manifold));
    SetPhysicsWord(duplicate_manifold, 320, 0); // second manifold's first subshape
    EXPECT_FALSE(ValidatePhysicsOwner(duplicate_manifold));
    auto cross_cache_duplicate = SphereFixture().state();
    SetPhysicsWord(cross_cache_duplicate, 324, 0);
    EXPECT_FALSE(ValidatePhysicsOwner(cross_cache_duplicate));
    auto unordered = SphereFixture(2, 0, 0, 0).state();
    ASSERT_TRUE(ValidatePhysicsOwner(unordered));
    SetPhysicsWord(unordered, 216, kFixtureBody + 2);
    SetPhysicsWord(unordered, 252, kFixtureBody + 1);
    EXPECT_FALSE(ValidatePhysicsOwner(unordered));
    auto unrelated = SphereFixture().state();
    SetPhysicsWord(unrelated, 212, kFixtureBody + 1);
    SetPhysicsWord(unrelated, 216, kFixtureBody + 2);
    EXPECT_FALSE(ValidatePhysicsOwner(unrelated));
    for (const auto word : {kFixtureBody, kFixtureBody + 10240, UINT32_MAX}) {
        auto invalid_slot = SphereFixture(0, 0, 0, 0, 1).state();
        SetPhysicsWord(invalid_slot, 224, word);
        EXPECT_FALSE(ValidatePhysicsOwner(invalid_slot));
    }
    auto duplicate_slot = SphereFixture(0, 0, 0, 0, 2).state();
    ASSERT_TRUE(ValidatePhysicsOwner(duplicate_slot));
    SetPhysicsWord(duplicate_slot, 228, 0x02000001u); // same slot, different sequence
    EXPECT_FALSE(ValidatePhysicsOwner(duplicate_slot));
}

TEST(PlayerAvatarOwnerCodec, AcceptsExact64KiBBoundAndRefusesExcessContactCounts) {
    auto maximum = SphereFixture(1, 139, 4, 0, 10203).state();
    ASSERT_EQ(maximum.size, PhysicsOwnerState::kMaxBytes);
    EXPECT_TRUE(ValidatePhysicsOwner(maximum));
    maximum.size += 1;
    EXPECT_FALSE(ValidatePhysicsOwner(maximum));
    auto contacts = SphereFixture(2, 128, 1, 0).state();
    ASSERT_TRUE(ValidatePhysicsOwner(contacts));
    // Include the complete excess records: without the count limit, parsing
    // must succeed instead of refusing later because the payload is truncated.
    const auto excess_manifolds = SphereFixture(2, 129, 1, 0).state();
    EXPECT_FALSE(ValidatePhysicsOwner(excess_manifolds));
    const auto full_cache = SphereFixture(64, 4, 1, 256).state();
    EXPECT_TRUE(ValidatePhysicsOwner(full_cache));
    const auto excess_pairs = SphereFixture(65, 0, 0, 0).state();
    EXPECT_FALSE(ValidatePhysicsOwner(excess_pairs));
    const auto ccd = SphereFixture(0, 0, 0, 256).state();
    ASSERT_TRUE(ValidatePhysicsOwner(ccd));
    const auto excess_ccd = SphereFixture(0, 0, 0, 257).state();
    EXPECT_FALSE(ValidatePhysicsOwner(excess_ccd));
}
TEST(PlayerAvatarOwnerCodec, EveryPhysicsScalarRefusesNonFiniteValues) {
    const auto character = CharacterFixture(true).state();
    const auto sphere = SphereFixture().state();
    // All float32 fields, including each vector component and contact impulse.
    const std::vector<std::pair<std::size_t, std::size_t>> character_ranges{
        {12, 20}, {60, 84}, {108, 140}, {184, 228}};
    const std::vector<std::pair<std::size_t, std::size_t>> sphere_ranges{
        {12, 16}, {44, 108}, {132, 184}, {192, 208}, {220, 244}, {268, 316}};
    for (const auto& [begin, end] : character_ranges) {
        for (auto offset = begin; offset < end; offset += 4) {
            for (const auto bits : {0x7f800000u, 0xff800000u, 0x7fc00000u}) {
                SCOPED_TRACE(offset);
                PlayerAvatarOwnerState malformed;
                malformed.physics = character;
                SetPhysicsWord(malformed.physics, offset, bits);
                ExpectAvatarRefused(malformed);
            }
        }
    }
    for (const auto& [begin, end] : sphere_ranges) {
        for (auto offset = begin; offset < end; offset += 4) {
            for (const auto bits : {0x7f800000u, 0xff800000u, 0x7fc00000u}) {
                SCOPED_TRACE(offset);
                auto malformed = sphere;
                SetPhysicsWord(malformed, offset, bits);
                EXPECT_FALSE(ValidatePhysicsOwner(malformed));
            }
        }
    }
    for (const auto offset : {36u, 44u, 52u, 84u, 92u, 100u, 160u, 168u, 176u}) {
        for (const auto high : {0x7ff00000u, 0xfff00000u, 0x7ff80000u}) {
            PlayerAvatarOwnerState malformed;
            malformed.physics = character;
            SetPhysicsWord(malformed.physics, offset + 4, high);
            ExpectAvatarRefused(malformed);
        }
    }
    for (const auto offset : {20u, 28u, 36u, 108u, 116u, 124u}) {
        for (const auto high : {0x7ff00000u, 0xfff00000u, 0x7ff80000u}) {
            auto malformed = sphere;
            SetPhysicsWord(malformed, offset + 4, high);
            EXPECT_FALSE(ValidatePhysicsOwner(malformed));
        }
    }
}

TEST(PlayerAvatarOwnerCodec, LocalAllocatorReferencesMustNameAllocatedSlots) {
    auto sphere = SphereFixture().state();
    SetPhysicsWord(sphere, 336, 1); // pair and CCD reference slot 1 outside the extent
    EXPECT_FALSE(ValidatePhysicsOwner(sphere));
    sphere = SphereFixture(1, 1, 1, 1, 1).state();
    SetPhysicsWord(sphere, 344, kFixtureBody + 1); // live contact slot cannot be free
    EXPECT_FALSE(ValidatePhysicsOwner(sphere));
    auto frozen = CharacterFixture(true).state();
    SetPhysicsWord(frozen, 8, 1);
    SetPhysicsWord(frozen, 20, kFixtureBody);
    frozen.size += 8;
    SetPhysicsWord(frozen, 248, 1);
    SetPhysicsWord(frozen, 252, 0);
    EXPECT_FALSE(ValidatePhysicsOwner(frozen)); // character contact outside extent
    SetPhysicsWord(frozen, 248, 2);
    EXPECT_TRUE(ValidatePhysicsOwner(frozen));
    SetPhysicsWord(frozen, 28, 0x02000000u); // owner's slot, mismatched sequence
    EXPECT_FALSE(ValidatePhysicsOwner(frozen));
    auto conflicting_pairs = SphereFixture(2, 0, 0, 0).state();
    SetPhysicsWord(conflicting_pairs, 252, 0x02000001u);
    EXPECT_FALSE(ValidatePhysicsOwner(conflicting_pairs)); // slot 1, two sequences
    PlayerAvatarOwnerState conflicting_ground;
    conflicting_ground.physics = CharacterFixture(true).state();
    SetPhysicsWord(conflicting_ground.physics, 24, 0);
    SetPhysicsWord(conflicting_ground.physics, 28, 0x02000001u);
    SetPhysicsWord(conflicting_ground.physics, 64, 0x3f800000u);
    EXPECT_FALSE(ValidatePhysicsOwner(conflicting_ground.physics));
    ExpectAvatarRefused(conflicting_ground);
}

TEST(PlayerAvatarOwnerCodec, OrderedCacheListsAndAllAvatarModesRoundTrip) {
    for (const bool contact : {false, true}) {
        for (const bool capsule : {false, true}) {
            PlayerAvatarOwnerState original;
            original.physics = CharacterFixture(contact).state();
            if (capsule) {
                SetPhysicsWord(original.physics, 8, 1);
                SetPhysicsWord(original.physics, 20, kFixtureBody);
                const auto tail = original.physics.size;
                original.physics.size += 8;
                SetPhysicsWord(original.physics, tail, 2);
                SetPhysicsWord(original.physics, tail + 4, 0);
            }
            std::array<std::uint8_t, Luminumbra::World::kMaxAvatarOwnerBytes> bytes{};
            std::size_t written = 0;
            ASSERT_TRUE(EncodeAvatarOwner(original, bytes, written));
            PlayerAvatarOwnerState decoded;
            ASSERT_TRUE(DecodeAvatarOwner(std::span(bytes).first(written), decoded));
            ExpectAvatarUnchanged(original, decoded);
        }
    }
    auto manifolds = SphereFixture(1, 2).state();
    SetPhysicsWord(manifolds, 252, 2);
    SetPhysicsWord(manifolds, 320, 1);
    EXPECT_FALSE(ValidatePhysicsOwner(manifolds));
    auto ccd = SphereFixture(1, 0, 0, 2).state();
    SetPhysicsWord(ccd, 256, 1001);
    SetPhysicsWord(ccd, 272, 1000);
    EXPECT_FALSE(ValidatePhysicsOwner(ccd));
    auto duplicate_pairs = SphereFixture(2, 0, 0, 0).state();
    SetPhysicsWord(duplicate_pairs, 252, kFixtureBody + 1);
    EXPECT_FALSE(ValidatePhysicsOwner(duplicate_pairs));
}

TEST(PlayerAvatarOwnerCodec, NormalsPrecisionBranchesAndBoundaryIds) {
    for (const auto begin : {60u, 196u, 208u}) {
        for (const auto axis : {0u, 4u, 8u}) {
            auto invalid = CharacterFixture(true).state();
            SetPhysicsWord(invalid, begin + axis, 0x40000000u);
            EXPECT_FALSE(ValidatePhysicsOwner(invalid));
        }
    }
    for (const auto begin : {196u, 208u}) {
        auto invalid = CharacterFixture(true).state();
        SetPhysicsWord(invalid, begin + 4, 0);
        EXPECT_FALSE(ValidatePhysicsOwner(invalid));
    }
    for (const auto bits : {std::uint64_t{1},
                            std::uint64_t{0x3690000000000001},
                            std::uint64_t{0x3680000000000000},
                            std::uint64_t{0x3ff0000000000001}}) {
        auto position = CharacterFixture().state();
        SetPhysicsWord(position, 84, static_cast<std::uint32_t>(bits));
        SetPhysicsWord(position, 88, static_cast<std::uint32_t>(bits >> 32));
#ifdef JPH_DOUBLE_PRECISION
        EXPECT_TRUE(ValidatePhysicsOwner(position));
#else
        EXPECT_FALSE(ValidatePhysicsOwner(position));
#endif
    }
    auto sleep_offset = SphereFixture().state();
    SetPhysicsWord(sleep_offset, 112, 0x3ff00000u);
#ifdef JPH_DOUBLE_PRECISION
    EXPECT_TRUE(ValidatePhysicsOwner(sleep_offset));
#else
    EXPECT_FALSE(ValidatePhysicsOwner(sleep_offset));
#endif
    SetPhysicsWord(sleep_offset, 112, 0x7fefffffu);
    EXPECT_FALSE(ValidatePhysicsOwner(sleep_offset));
    auto largest_id = SphereFixture(0, 0, 0, 0).state();
    // Jolt stores the sequence at bit 23; bit 31 is the broadphase bit.
    SetPhysicsWord(largest_id, 8, 0x7f8027ffu); // index 10239, sequence 255
    EXPECT_TRUE(ValidatePhysicsOwner(largest_id));
    auto contacts = CharacterFixture(true);
    const std::vector<std::uint32_t> contact(contacts.words.begin() + 37, contacts.words.end());
    for (int i = 1; i < 256; ++i) {
        contacts.words.insert(contacts.words.end(), contact.begin(), contact.end());
    }
    contacts.words[36] = 256;
    EXPECT_TRUE(ValidatePhysicsOwner(contacts.state()));
    contacts.words.insert(contacts.words.end(), contact.begin(), contact.end());
    contacts.words[36] = 257;
    EXPECT_FALSE(ValidatePhysicsOwner(contacts.state()));
    for (const auto id : {0u, UINT32_MAX}) {
        PlayerAvatarOwnerState owner;
        owner.avatar.player_id = id;
        owner.physics = CharacterFixture().state();
        EXPECT_TRUE(ValidateAvatarOwner(owner));
    }
    PlayerAvatarOwnerState owner;
    owner.physics = CharacterFixture().state();
    auto bytes = RawAvatarBytes(owner);
    PlayerAvatarOwnerState output;
    output.avatar.player_id = 99;
    const auto retained = output;
    bytes[2] = 1; // all outer version bits participate in validation
    EXPECT_FALSE(DecodeAvatarOwner(bytes, output));
    ExpectAvatarUnchanged(retained, output);
    bytes = RawAvatarBytes(owner);
    for (const auto size : {0u, 147u, 149u, UINT32_MAX}) {
        for (unsigned i = 0; i < 4; ++i) {
            bytes[36 + i] = static_cast<std::uint8_t>(size >> (8 * i));
        }
        EXPECT_FALSE(DecodeAvatarOwner(bytes, output));
        ExpectAvatarUnchanged(retained, output);
    }
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
        // Copies of one live sphere would share its registration; give each its own.
        state.owners[i].registration_slot = static_cast<std::uint32_t>(i + 1);
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
        ASSERT_EQ(source.create_avatar_character(Vec3(0.0f)), i);
    }
    // Retire only after reaching the cap: immediate retirement reuses slot zero.
    for (std::size_t i = 0; i < PhysicsSystem::kMaxRestoredAvatars; ++i) {
        source.remove_avatar_character(i);
    }
    ASSERT_TRUE(source.capture_body_owners(state));
    EXPECT_TRUE(source.restore_body_owners(state, {}));
    const auto before = state;
    for (std::size_t i = 0; i < PhysicsSystem::kMaxRestoredAvatars; ++i) {
        ASSERT_EQ(source.create_avatar_character(Vec3(0.0f)), i);
    }
    const auto extra = source.create_avatar_character(Vec3(0.0f));
    ASSERT_EQ(extra, PhysicsSystem::kMaxRestoredAvatars);
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

namespace {

// Only the session constructs the internal physics adapter.
static_assert(!std::is_constructible_v<PhysicsSystem::OwnerContinuation, PhysicsSystem&>);

template<class Physics>
concept PublicAvatarCapture = requires(Physics& physics, PhysicsOwnerState& state) {
    physics.capture_avatar_owner(0, state);
};
template<class Physics>
concept PublicAvatarRestore =
    requires(Physics& physics, const PhysicsOwnerState& state, std::size_t& index) {
        physics.restore_avatar_owner(state, index);
    };
static_assert(!PublicAvatarCapture<PhysicsSystem>);
static_assert(!PublicAvatarRestore<PhysicsSystem>);

template<class Physics>
concept PublicPlayerCapture =
    requires(Physics& physics, PhysicsOwnerState& state) { physics.capture_player_owner(state); };
template<class Physics>
concept PublicPlayerRestore = requires(Physics& physics, const PhysicsOwnerState& state) {
    physics.restore_player_owner(state);
};
static_assert(!PublicPlayerCapture<PhysicsSystem>);
static_assert(!PublicPlayerRestore<PhysicsSystem>);

void ExpectControllerBits(const PhysicsOwnerState& expected, const PhysicsOwnerState& actual) {
    ASSERT_EQ(expected.size, actual.size);
    EXPECT_TRUE(std::equal(
        expected.bytes.begin(), expected.bytes.begin() + expected.size, actual.bytes.begin()));
}

TEST(ControllerContinuation, PlayerFallingGroundedAndCrouchedResumeBitExactly) {
    constexpr float dt = 1.0f / 30.0f;
    for (const int capture_tick : {3, 45}) {
        for (const bool crouched : {false, true}) {
            Luminumbra::TestSupport::AvatarContinuationWorld source_world;
            auto& source = source_world.physics();
            AddBodyFloor(source);
            ASSERT_TRUE(source.create_player_controller(Vec3(8.123456f, 10, 8.765432f)));
            ASSERT_TRUE(source.set_player_crouched(crouched));
            const Vec3 wish(0.1234567f, 0, 0.0765432f);
            for (int tick = 0; tick < capture_tick; ++tick) {
                source.update_player(wish, false, 5, dt);
            }
            ASSERT_EQ(source.is_player_grounded(), capture_tick == 45);
            PhysicsOwnerState expected;
            ASSERT_TRUE(source_world.capture_player_owner(expected));
            ASSERT_TRUE(ValidatePhysicsOwner(expected));
            Luminumbra::TestSupport::AvatarContinuationWorld target_world;
            auto& target = target_world.physics();
            AddBodyFloor(target);
            ASSERT_TRUE(target_world.restore_player_owner(expected));
            for (int tick = 0; tick < 20; ++tick) {
                PhysicsOwnerState actual;
                ASSERT_TRUE(target_world.capture_player_owner(actual));
                ExpectControllerBits(expected, actual);
                // Continue through stance changes and a jump as well as saved contacts.
                if (tick == 5) {
                    ASSERT_TRUE(source.set_player_crouched(!crouched));
                    ASSERT_TRUE(target.set_player_crouched(!crouched));
                }
                source.update_player(wish, tick == 10, 5, dt);
                target.update_player(wish, tick == 10, 5, dt);
                ASSERT_TRUE(source_world.capture_player_owner(expected));
                ASSERT_TRUE(target_world.capture_player_owner(actual));
                ExpectControllerBits(expected, actual);
            }
        }
    }
}

TEST(ControllerContinuation, PlayerRefusalsPreserveOwnerAndCaptureOutput) {
    Luminumbra::TestSupport::AvatarContinuationWorld source_world;
    auto& source = source_world.physics();
    AddBodyFloor(source);
    ASSERT_TRUE(source.create_player_controller(Vec3(8, 10, 8)));
    for (int tick = 0; tick < 45; ++tick) {
        source.update_player(Vec3(0), false, 5, 1.0f / 30.0f);
    }
    PhysicsOwnerState captured;
    ASSERT_TRUE(source_world.capture_player_owner(captured));
    Luminumbra::TestSupport::AvatarContinuationWorld target_world;
    Luminumbra::TestSupport::AvatarContinuationWorld stopped_world(false);
    auto retained = captured;
    EXPECT_FALSE(stopped_world.capture_player_owner(retained));
    ExpectControllerBits(captured, retained);
    EXPECT_FALSE(stopped_world.restore_player_owner(captured));
    EXPECT_FALSE(target_world.capture_player_owner(retained));
    ExpectControllerBits(captured, retained);
    EXPECT_FALSE(target_world.restore_player_owner(captured)); // Missing contact terrain.
    AddBodyFloor(target_world.physics());
    auto invalid = captured;
    --invalid.size;
    EXPECT_FALSE(target_world.restore_player_owner(invalid));
    ASSERT_EQ(source.create_avatar_character(Vec3(8, 10, 8)), 0u);
    ASSERT_TRUE(source_world.capture_avatar_owner(0, invalid));
    EXPECT_FALSE(target_world.restore_player_owner(invalid)); // Wrong owner kind.
    EXPECT_FALSE(target_world.capture_player_owner(retained));
    ASSERT_TRUE(target_world.restore_player_owner(captured));
    EXPECT_FALSE(target_world.restore_player_owner(captured)); // Singleton already occupied.
    ASSERT_TRUE(target_world.capture_player_owner(retained));
    ExpectControllerBits(captured, retained);
}

TEST(ControllerContinuation, AvatarFallingGroundedAndFrozenResumeBitExactly) {
    constexpr float dt = 1.0f / 30.0f;
    for (const int capture_tick : {3, 45}) {
        for (const bool frozen : {false, true}) {
            Luminumbra::TestSupport::AvatarContinuationWorld source_world;
            auto& source = source_world.physics();
            AddBodyFloor(source);
            const auto index = source.create_avatar_character(Vec3(8.123456f, 10.0f, 8.765432f));
            source.set_avatar_wish_velocity(index, glm::vec2(0.1234567f, 0.0765432f));
            for (int tick = 0; tick < capture_tick; ++tick) {
                source.update_avatars(dt);
            }
            ASSERT_EQ(source.is_avatar_grounded(index), capture_tick == 45);
            source.set_avatar_frozen(index, frozen, source.get_avatar_position(index));
            PhysicsOwnerState expected;
            ASSERT_TRUE(source_world.capture_avatar_owner(index, expected));
            ASSERT_TRUE(ValidatePhysicsOwner(expected));
            Luminumbra::TestSupport::AvatarContinuationWorld target_world;
            auto& target = target_world.physics();
            AddBodyFloor(target);
            std::size_t restored_index = 999;
            ASSERT_TRUE(target_world.restore_avatar_owner(expected, restored_index));
            ASSERT_EQ(restored_index, 0u);
            for (int tick = 0; tick < 20; ++tick) {
                PhysicsOwnerState actual;
                ASSERT_TRUE(target_world.capture_avatar_owner(restored_index, actual));
                ExpectControllerBits(expected, actual);
                // Both the frozen capsule and subsequent resumed movement must
                // preserve controller contacts, full precision and allocator IDs.
                if (tick == 5 && frozen) {
                    source.set_avatar_frozen(index, false, source.get_avatar_position(index));
                    target.set_avatar_frozen(
                        restored_index, false, target.get_avatar_position(restored_index));
                    source.set_avatar_wish_velocity(index, glm::vec2(0.1234567f, 0.0f));
                    target.set_avatar_wish_velocity(restored_index, glm::vec2(0.1234567f, 0.0f));
                }
                source.update_avatars(dt);
                target.update_avatars(dt);
                ASSERT_TRUE(source_world.capture_avatar_owner(index, expected));
            }
        }
    }
}

TEST(ControllerContinuation, RefusalsPreserveOwnersIndicesAndOutputs) {
    Luminumbra::TestSupport::AvatarContinuationWorld source_world;
    auto& source = source_world.physics();
    AddBodyFloor(source);
    const auto index = source.create_avatar_character(Vec3(8, 10, 8));
    for (int tick = 0; tick < 45; ++tick) {
        source.update_avatars(1.0f / 30.0f);
    }
    PhysicsOwnerState captured;
    ASSERT_TRUE(source_world.capture_avatar_owner(index, captured));
    Luminumbra::TestSupport::AvatarContinuationWorld target_world;
    auto& target = target_world.physics();
    std::size_t destination = 999;
    EXPECT_FALSE(target_world.restore_avatar_owner(captured, destination)); // Missing terrain.
    AddBodyFloor(target);
    Luminumbra::TestSupport::AvatarContinuationWorld stopped_world(false);
    EXPECT_FALSE(stopped_world.restore_avatar_owner(captured, destination));
    for (std::uint32_t size = 0; size < captured.size; ++size) {
        auto truncated = captured;
        truncated.size = size;
        EXPECT_FALSE(target_world.restore_avatar_owner(truncated, destination));
        EXPECT_EQ(destination, 999u);
        EXPECT_EQ(target.avatar_character_slot_count(), 0u);
    }
    auto malformed = captured;
    SetPhysicsWord(malformed, 8, 9); // Unknown flags.
    EXPECT_FALSE(target_world.restore_avatar_owner(malformed, destination));
    EXPECT_EQ(destination, 999u);
    PhysicsOwnerState output = captured;
    EXPECT_FALSE(stopped_world.capture_avatar_owner(0, output));
    EXPECT_FALSE(target_world.capture_avatar_owner(0, output));
    EXPECT_FALSE(source_world.capture_avatar_owner(index + 1, output));
    ExpectControllerBits(captured, output);
    ASSERT_TRUE(target_world.restore_avatar_owner(captured, destination));
    EXPECT_EQ(destination, 0u);
    for (std::size_t slot = 1; slot < PhysicsSystem::kMaxRestoredAvatars - 1; ++slot) {
        target.create_avatar_character(Vec3(0, 30, 0));
    }
    destination = 999;
    EXPECT_FALSE(target_world.restore_avatar_owner(captured, destination, std::span(&captured, 1)));
    EXPECT_EQ(destination, 999u);
    EXPECT_EQ(target.avatar_character_slot_count(), PhysicsSystem::kMaxRestoredAvatars - 1);
    target.create_avatar_character(Vec3(0, 30, 0));
    EXPECT_FALSE(target_world.restore_avatar_owner(captured, destination));
    EXPECT_EQ(destination, 999u);
    EXPECT_EQ(target.avatar_character_slot_count(), PhysicsSystem::kMaxRestoredAvatars);
    source.remove_avatar_character(index);
    EXPECT_FALSE(source_world.capture_avatar_owner(index, output));
    ExpectControllerBits(captured, output);
}
} // namespace

namespace {
using Luminumbra::JobSystem;
using Luminumbra::TestSupport::HeadlessRoot;
using Luminumbra::TestSupport::ReadClockTestFile;
using Luminumbra::world::GameSession;
TEST(GameSessionHeadlessWorldTest, InternalAvatarOwnerRestoresWithoutSpawnOrPersistence) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    {
        GameSession source;
        source.SetJobSystem(&jobs);
        source.SetRootPath(root.root_string());
        source.SetActiveRegionsEnabled(false);
        ASSERT_TRUE(source.CreateWorld("OwnerSource", "12345", "default"));
        auto* original = source.GetPhysicsSystem();
        ASSERT_NE(original, nullptr);
        const auto index =
            original->create_avatar_character(Luminumbra::Vec3(1.234567f, 40.125f, 2.765432f));
        original->set_avatar_wish_velocity(index, glm::vec2(0.1234567f, 0.0765432f));
        for (int tick = 0; tick < 7; ++tick) {
            original->update_avatars(1.0f / 30.0f);
        }
        Luminumbra::World::PlayerAvatar avatar{.player_id = 0,
                                               .position = original->get_avatar_position(index),
                                               .facing = -0.1256789f,
                                               .velocity = original->get_avatar_velocity(index)};
        Luminumbra::World::PlayerAvatarOwnerState captured;
        ASSERT_TRUE(source.CaptureAvatarOwner(avatar, index, captured));
        auto invalid_avatar = avatar;
        invalid_avatar.facing = std::numeric_limits<float>::infinity();
        auto retained_capture = captured;
        EXPECT_FALSE(source.CaptureAvatarOwner(invalid_avatar, index, retained_capture));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(retained_capture.avatar.facing),
                  std::bit_cast<std::uint32_t>(captured.avatar.facing));
        ExpectControllerBits(captured.physics, retained_capture.physics);
        GameSession restored;
        restored.SetJobSystem(&jobs);
        restored.SetRootPath(root.root_string());
        restored.SetActiveRegionsEnabled(false);
        ASSERT_TRUE(restored.CreateWorld("OwnerRestored", "12345", "default"));
        const auto metadata_before =
            ReadClockTestFile(restored.GetWorldSaveDir() / "world_info.json");
        Luminumbra::World::PlayerAvatar actual;
        std::size_t restored_index = 123;
        auto invalid = captured;
        invalid.avatar.velocity.x = std::numeric_limits<float>::infinity();
        EXPECT_FALSE(restored.RestoreAvatarOwner(invalid, actual, restored_index));
        EXPECT_EQ(restored_index, 123u);
        EXPECT_EQ(actual.player_id, 0u);
        ASSERT_EQ(restored.GetPhysicsSystem()->avatar_character_slot_count(), 0u);
        ASSERT_TRUE(restored.RestoreAvatarOwner(captured, actual, restored_index));
        EXPECT_EQ(actual.player_id, avatar.player_id);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.facing),
                  std::bit_cast<std::uint32_t>(avatar.facing));
        EXPECT_EQ(ReadClockTestFile(restored.GetWorldSaveDir() / "world_info.json"),
                  metadata_before);
        for (int tick = 0; tick < 20; ++tick) {
            original->update_avatars(1.0f / 30.0f);
            restored.GetPhysicsSystem()->update_avatars(1.0f / 30.0f);
            avatar.position = original->get_avatar_position(index);
            avatar.velocity = original->get_avatar_velocity(index);
            actual.position = restored.GetPhysicsSystem()->get_avatar_position(restored_index);
            actual.velocity = restored.GetPhysicsSystem()->get_avatar_velocity(restored_index);
            Luminumbra::World::PlayerAvatarOwnerState expected_state;
            Luminumbra::World::PlayerAvatarOwnerState actual_state;
            ASSERT_TRUE(source.CaptureAvatarOwner(avatar, index, expected_state));
            ASSERT_TRUE(restored.CaptureAvatarOwner(actual, restored_index, actual_state));
            std::array<std::uint8_t, Luminumbra::World::kMaxAvatarOwnerBytes> expected_bytes{};
            std::array<std::uint8_t, Luminumbra::World::kMaxAvatarOwnerBytes> actual_bytes{};
            std::size_t expected_size = 0;
            std::size_t actual_size = 0;
            ASSERT_TRUE(Luminumbra::World::EncodeAvatarOwner(
                expected_state, expected_bytes, expected_size));
            ASSERT_TRUE(
                Luminumbra::World::EncodeAvatarOwner(actual_state, actual_bytes, actual_size));
            ASSERT_EQ(expected_size, actual_size);
            EXPECT_EQ(expected_bytes, actual_bytes);
        }
        Luminumbra::World::PlayerAvatarOwnerState retained = captured;
        EXPECT_FALSE(source.CaptureAvatarOwner(avatar, 999, retained));
        EXPECT_EQ(retained.physics.bytes, captured.physics.bytes);
        GameSession unopened;
        EXPECT_FALSE(unopened.CaptureAvatarOwner(avatar, index, retained));
        EXPECT_FALSE(unopened.RestoreAvatarOwner(captured, actual, restored_index));
    }
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, AvatarRestoreRefusesOutOfOrderAndMisalignedRosterSlots) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetJobSystem(&jobs);
    session.SetRootPath(root.root_string());
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.CreateTransientWorld("Roster refusal", "12345", "default"));
    auto* target = session.GetPhysicsSystem();
    ASSERT_NE(target, nullptr);
    Luminumbra::TestSupport::AvatarContinuationWorld source_world;
    auto& source = source_world.physics();
    ASSERT_EQ(source.create_avatar_character(Vec3(8, 40, 8)), 0u);
    Luminumbra::World::PlayerAvatarOwnerState captured;
    captured.avatar.position = source.get_avatar_position(0);
    ASSERT_TRUE(source_world.capture_avatar_owner(0, captured.physics));
    for (const int failure : {0, 1, 2, 3}) {
        if (failure == 1) {
            PlayerAvatar restored;
            std::size_t restored_index = 999;
            ASSERT_TRUE(session.RestoreAvatarOwner(captured, restored, restored_index));
            EXPECT_EQ(restored_index, 0u);
            EXPECT_EQ(restored.player_id, 0u);
        } else if (failure == 3) {
            ASSERT_EQ(target->create_avatar_character(Vec3(16, 40, 8)), 1u);
        }
        auto invalid = captured;
        // Future player at an empty roster, repeated player, skipped player,
        // then a player whose slot was already occupied outside the session.
        invalid.avatar.player_id = failure == 0 || failure == 3 ? 1u : failure == 1 ? 0u : 2u;
        ASSERT_TRUE(Luminumbra::World::ValidateAvatarOwner(invalid));
        BodySetState before;
        ASSERT_TRUE(target->capture_body_owners(before));
        const auto slots = target->avatar_character_slot_count();
        PhysicsOwnerState previous;
        if (slots != 0) {
            Luminumbra::World::PlayerAvatarOwnerState existing;
            ASSERT_TRUE(session.CaptureAvatarOwner(captured.avatar, 0, existing));
            previous = existing.physics;
        }
        PlayerAvatar output{.player_id = 77, .position = Vec3(1), .facing = 2, .velocity = Vec3(3)};
        std::size_t index = 999;
        EXPECT_FALSE(session.RestoreAvatarOwner(invalid, output, index));
        EXPECT_EQ(index, 999u);
        EXPECT_EQ(output.player_id, 77u);
        EXPECT_EQ(output.position, Vec3(1));
        EXPECT_EQ(output.facing, 2);
        EXPECT_EQ(output.velocity, Vec3(3));
        EXPECT_EQ(target->avatar_character_slot_count(), slots);
        BodySetState after;
        ASSERT_TRUE(target->capture_body_owners(after));
        ExpectBodySet(before, after);
        if (slots != 0) {
            Luminumbra::World::PlayerAvatarOwnerState retained;
            ASSERT_TRUE(session.CaptureAvatarOwner(captured.avatar, 0, retained));
            ExpectControllerBits(previous, retained.physics);
        }
    }
}

TEST(GameSessionHeadlessWorldTest, OutOfBoundsAvatarRestorePublishesNothingAndTicksContinue) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    Luminumbra::TestSupport::AvatarContinuationWorld source_world;
    auto& source = source_world.physics();
    ASSERT_EQ(source.create_avatar_character(Vec3(8, 40, 8)), 0u);
    Luminumbra::World::PlayerAvatarOwnerState captured;
    captured.avatar.position = source.get_avatar_position(0);
    ASSERT_TRUE(source_world.capture_avatar_owner(0, captured.physics));
    ASSERT_TRUE(Luminumbra::World::ValidateAvatarOwner(captured));
    for (const std::size_t axis : {0u, 2u}) {
        for (const float coordinate :
             {static_cast<float>(Luminumbra::Chunk::kPackedMinXz * Luminumbra::CHUNK_SIZE_X),
              static_cast<float>(Luminumbra::Chunk::kPackedMaxXz * Luminumbra::CHUNK_SIZE_X),
              -1.0e30f,
              1.0e30f}) {
            GameSession session;
            session.SetJobSystem(&jobs);
            session.SetRootPath(root.root_string());
            session.SetActiveRegionsEnabled(true);
            ASSERT_TRUE(session.CreateTransientWorld("Bounds refusal", "12345", "default"));
            session.GetWorldSystem()->set_durable_residency(true);
            session.SetReplicatedSimulationAnchors({});
            auto* target = session.GetPhysicsSystem();
            ASSERT_NE(target, nullptr);
            ASSERT_TRUE(session.ActiveRegionsEnabled());
            BodySetState before;
            ASSERT_TRUE(target->capture_body_owners(before));
            auto invalid = captured;
            invalid.avatar.position[static_cast<int>(axis)] = coordinate;
            const auto wide = std::bit_cast<std::uint64_t>(static_cast<double>(coordinate));
            SetPhysicsWord(invalid.physics, 84 + 8 * axis, static_cast<std::uint32_t>(wide));
            SetPhysicsWord(invalid.physics, 88 + 8 * axis, static_cast<std::uint32_t>(wide >> 32));
            // Finite, codec-valid and consistent with the avatar: only simulation
            // coordinate admission can refuse this payload.
            ASSERT_TRUE(Luminumbra::World::ValidateAvatarOwner(invalid));
            PlayerAvatar output{
                .player_id = 77, .position = Vec3(1), .facing = 2, .velocity = Vec3(3)};
            std::size_t index = 999;
            EXPECT_FALSE(session.RestoreAvatarOwner(invalid, output, index));
            EXPECT_EQ(index, 999u);
            EXPECT_EQ(output.player_id, 77u);
            EXPECT_EQ(output.position, Vec3(1));
            EXPECT_EQ(output.facing, 2);
            EXPECT_EQ(output.velocity, Vec3(3));
            EXPECT_EQ(target->avatar_character_slot_count(), 0u);
            BodySetState after;
            ASSERT_TRUE(target->capture_body_owners(after));
            ExpectBodySet(before, after);
            ASSERT_TRUE(session.RestoreAvatarOwner(captured, output, index));
            EXPECT_EQ(index, 0u);
            EXPECT_EQ(target->avatar_character_slot_count(), 1u);
            const auto tick = session.GetSimulationTickCount();
            EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
            EXPECT_EQ(session.GetSimulationTickCount(), tick + 1);
        }
    }
}

TEST(GameSessionHeadlessWorldTest, OutOfBoundsPlayerRestorePublishesNothingAndTicksContinue) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetJobSystem(&jobs);
    session.SetRootPath(root.root_string());
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Bounds refusal", "12345", "default"));
    session.GetWorldSystem()->set_durable_residency(true);
    session.SetReplicatedSimulationAnchors({});
    auto* target = session.GetPhysicsSystem();
    ASSERT_NE(target, nullptr);
    Luminumbra::TestSupport::AvatarContinuationWorld source_world;
    auto& source = source_world.physics();
    ASSERT_TRUE(source.create_player_controller(Vec3(8, 40, 8)));
    PhysicsOwnerState captured;
    ASSERT_TRUE(source_world.capture_player_owner(captured));
    ASSERT_TRUE(ValidatePhysicsOwner(captured));
    ASSERT_TRUE(session.ActiveRegionsEnabled());
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    BodySetState before;
    ASSERT_TRUE(target->capture_body_owners(before));
    for (const std::size_t axis : {0u, 2u}) {
        for (const float coordinate :
             {static_cast<float>(Luminumbra::Chunk::kPackedMinXz * Luminumbra::CHUNK_SIZE_X),
              static_cast<float>(Luminumbra::Chunk::kPackedMaxXz * Luminumbra::CHUNK_SIZE_X),
              -1.0e30f,
              1.0e30f}) {
            auto invalid = captured;
            const auto wide = std::bit_cast<std::uint64_t>(static_cast<double>(coordinate));
            SetPhysicsWord(invalid, 84 + 8 * axis, static_cast<std::uint32_t>(wide));
            SetPhysicsWord(invalid, 88 + 8 * axis, static_cast<std::uint32_t>(wide >> 32));
            // Finite and codec-valid: only simulation
            // coordinate admission can refuse this payload.
            ASSERT_TRUE(ValidatePhysicsOwner(invalid));
            EXPECT_FALSE(session.RestorePlayerOwner(invalid));
            PhysicsOwnerState absent = captured;
            EXPECT_FALSE(session.CapturePlayerOwner(absent));
            ExpectControllerBits(captured, absent);
            EXPECT_EQ(target->avatar_character_slot_count(), 0u);
            BodySetState after;
            ASSERT_TRUE(target->capture_body_owners(after));
            ExpectBodySet(before, after);
            const auto tick = session.GetSimulationTickCount();
            EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
            EXPECT_EQ(session.GetSimulationTickCount(), tick + 1);
        }
    }
    ASSERT_TRUE(session.RestorePlayerOwner(captured));
    PhysicsOwnerState actual;
    ASSERT_TRUE(session.CapturePlayerOwner(actual));
    ExpectControllerBits(captured, actual);
    EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
}

TEST(ControllerContinuation, FrozenRosterRestoresSequentiallyAndContinuesBitExactly) {
    constexpr float dt = 1.0f / 30.0f;
    for (const std::size_t count : {2u, 3u}) {
        for (const bool free_slot : {false, true}) {
            Luminumbra::TestSupport::AvatarContinuationWorld source_world;
            auto& source = source_world.physics();
            AddBodyFloor(source);
            const auto hole = source.create_dynamic_sphere(Vec3(20), Vec3(0), 0.3f);
            ASSERT_FALSE(hole.IsInvalid());
            for (std::size_t slot = 0; slot < count; ++slot) {
                const Vec3 feet(4.0f + 4.0f * static_cast<float>(slot), 10.0f, 8.0f);
                ASSERT_EQ(source.create_avatar_character(feet), slot);
                source.set_avatar_frozen(slot, true, feet);
            }
            if (free_slot) {
                source.destroy_body(hole);
            }
            // A live unrelated body is restored separately, just like terrain.
            Luminumbra::TestSupport::AvatarContinuationWorld target_world;
            auto& target = target_world.physics();
            AddBodyFloor(target);
            if (!free_slot) {
                ASSERT_EQ(target.create_dynamic_sphere(Vec3(20), Vec3(0), 0.3f), hole);
            }
            std::vector<PhysicsOwnerState> captured(count);
            for (std::size_t slot = 0; slot < count; ++slot) {
                ASSERT_TRUE(source_world.capture_avatar_owner(slot, captured[slot]));
            }
            std::size_t restored_index = 999;
            // Unexplained occupied slots are still an inconsistent allocator.
            EXPECT_FALSE(target_world.restore_avatar_owner(captured[0], restored_index));
            EXPECT_EQ(restored_index, 999u);
            EXPECT_EQ(target.avatar_character_slot_count(), 0u);
            for (std::size_t slot = 0; slot < count; ++slot) {
                ASSERT_TRUE(target_world.restore_avatar_owner(
                    captured[slot], restored_index, std::span(captured).subspan(slot + 1)));
                EXPECT_EQ(restored_index, slot);
            }
            for (int tick = 0; tick < 12; ++tick) {
                for (std::size_t slot = 0; slot < count; ++slot) {
                    PhysicsOwnerState expected;
                    PhysicsOwnerState actual;
                    ASSERT_TRUE(source_world.capture_avatar_owner(slot, expected));
                    ASSERT_TRUE(target_world.capture_avatar_owner(slot, actual));
                    ExpectControllerBits(expected, actual);
                    if (tick == 3) {
                        source.set_avatar_frozen(slot, false, source.get_avatar_position(slot));
                        target.set_avatar_frozen(slot, false, target.get_avatar_position(slot));
                        source.set_avatar_wish_velocity(slot, glm::vec2(0.125f, 0.25f));
                        target.set_avatar_wish_velocity(slot, glm::vec2(0.125f, 0.25f));
                    }
                }
                source.update_avatars(dt);
                target.update_avatars(dt);
            }
            BodySetState expected;
            BodySetState actual;
            ASSERT_TRUE(source.capture_body_owners(expected));
            ASSERT_TRUE(target.capture_body_owners(actual));
            ExpectBodySet(expected, actual);
            EXPECT_EQ(source.create_dynamic_sphere(Vec3(20), Vec3(0), 0.3f),
                      target.create_dynamic_sphere(Vec3(20), Vec3(0), 0.3f));
        }
    }
}

TEST(ControllerContinuation, FrozenContactsResolveLaterCapsulesAndRefuseUnreservedGenerations) {
    constexpr float dt = 1.0f / 30.0f;
    Luminumbra::TestSupport::AvatarContinuationWorld source_world;
    auto& source = source_world.physics();
    AddBodyFloor(source);
    ASSERT_EQ(source.create_avatar_character(Vec3(8, 16, 8)), 0u);
    ASSERT_EQ(source.create_avatar_character(Vec3(8, 10, 8)), 1u);
    source.set_avatar_frozen(1, true, source.get_avatar_position(1));
    for (int tick = 0; tick < 45; ++tick) {
        source.update_avatars(dt);
    }
    ASSERT_TRUE(source.is_avatar_grounded(0));
    source.set_avatar_frozen(0, true, source.get_avatar_position(0));
    std::array<PhysicsOwnerState, 2> captured{};
    for (std::size_t slot = 0; slot < captured.size(); ++slot) {
        ASSERT_TRUE(source_world.capture_avatar_owner(slot, captured[slot]));
    }
    const auto word = [](const PhysicsOwnerState& state, std::size_t offset) {
        std::uint32_t result = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            result |= static_cast<std::uint32_t>(state.bytes[offset++]) << shift;
        }
        return result;
    };
    // Pin both the ground body and an active contact to player 1's capsule.
    ASSERT_GE(captured[0].size, 152u);
    const auto capsule = word(captured[1], 20);
    ASSERT_NE(capsule, JPH::BodyID::cInvalidBodyID);
    ASSERT_EQ(word(captured[0], 28), capsule);
    ASSERT_GT(word(captured[0], 144), 0u);
    ASSERT_EQ(word(captured[0], 148), capsule);
    Luminumbra::TestSupport::AvatarContinuationWorld target_world;
    auto& target = target_world.physics();
    AddBodyFloor(target);
    BodySetState before;
    ASSERT_TRUE(target.capture_body_owners(before));
    auto stale = captured[1];
    const JPH::BodyID original(capsule);
    SetPhysicsWord(
        stale,
        20,
        JPH::BodyID(original.GetIndex(), static_cast<JPH::uint8>(original.GetSequenceNumber() + 1))
            .GetIndexAndSequenceNumber());
    ASSERT_TRUE(ValidatePhysicsOwner(stale));
    std::size_t index = 999;
    // Reserving the same slot with another generation cannot satisfy this contact.
    EXPECT_FALSE(target_world.restore_avatar_owner(captured[0], index, std::span(&stale, 1)));
    EXPECT_EQ(index, 999u);
    EXPECT_EQ(target.avatar_character_slot_count(), 0u);
    BodySetState after;
    ASSERT_TRUE(target.capture_body_owners(after));
    ExpectBodySet(before, after);
    // The server requires player-ID order, so player 1 is still only reserved
    // when player 0's contacts are restored.
    for (std::size_t slot = 0; slot < captured.size(); ++slot) {
        ASSERT_TRUE(target_world.restore_avatar_owner(
            captured[slot], index, std::span(captured).subspan(slot + 1)));
        EXPECT_EQ(index, slot);
    }
    PhysicsOwnerState restored;
    ASSERT_TRUE(target_world.capture_avatar_owner(0, restored));
    ExpectControllerBits(captured[0], restored);
    BodyOwnerState restored_capsule;
    ASSERT_TRUE(target.capture_body_owner(JPH::BodyID(word(restored, 28)), restored_capsule));
    EXPECT_EQ(BodyWord(restored_capsule, 8), capsule);
    source.set_avatar_frozen(0, false, source.get_avatar_position(0));
    target.set_avatar_frozen(0, false, target.get_avatar_position(0));
    for (int tick = 0; tick < 12; ++tick) {
        source.update_avatars(dt);
        target.update_avatars(dt);
        PhysicsOwnerState expected;
        ASSERT_TRUE(source_world.capture_avatar_owner(0, expected));
        ASSERT_TRUE(target_world.capture_avatar_owner(0, restored));
        ExpectControllerBits(expected, restored);
        EXPECT_EQ(word(restored, 28), capsule);
        EXPECT_TRUE(target.is_avatar_grounded(0));
    }
}

TEST(ControllerContinuation, MixedRosterContactsRestoreInPlayerOrderWithExactReservations) {
    constexpr float dt = 1.0f / 30.0f;
    for (const auto& [free_slot, frozen_without_capsule] : std::array{std::pair{false, false},
                                                                      std::pair{false, true},
                                                                      std::pair{true, false},
                                                                      std::pair{true, true}}) {
        Luminumbra::TestSupport::AvatarContinuationWorld source_world;
        auto& source = source_world.physics();
        AddBodyFloor(source);
        ASSERT_EQ(source.create_avatar_character(Vec3(8, 16, 8)), 0u);
        ASSERT_EQ(source.create_avatar_character(Vec3(8, 10, 8)), 1u);
        const auto hole =
            free_slot ? source.create_dynamic_sphere(Vec3(20), Vec3(0), 0.3f) : JPH::BodyID();
        source.set_avatar_frozen(1, true, source.get_avatar_position(1));
        ASSERT_EQ(source.create_avatar_character(Vec3(16, 10, 8)), 2u);
        source.set_avatar_frozen(2, true, source.get_avatar_position(2));
        if (free_slot) {
            ASSERT_FALSE(hole.IsInvalid());
            source.destroy_body(hole);
        }
        for (int tick = 0; tick < 45; ++tick) {
            source.update_avatars(dt);
        }
        ASSERT_TRUE(source.is_avatar_grounded(0));
        std::array<PhysicsOwnerState, 3> captured{};
        for (std::size_t slot = 0; slot < captured.size(); ++slot) {
            ASSERT_TRUE(source_world.capture_avatar_owner(slot, captured[slot]));
        }
        if (frozen_without_capsule) {
            // A headroom refusal can freeze a controller without a body.
            SetPhysicsWord(captured[0], 8, 1);
        }
        ASSERT_TRUE(ValidatePhysicsOwner(captured[0]));
        const auto word = [](const PhysicsOwnerState& state, std::size_t offset) {
            std::uint32_t result = 0;
            for (unsigned shift = 0; shift < 32; shift += 8) {
                result |= static_cast<std::uint32_t>(state.bytes[offset++]) << shift;
            }
            return result;
        };
        ASSERT_GE(captured[0].size, 152u);
        const JPH::BodyID capsule(word(captured[1], 20));
        ASSERT_FALSE(capsule.IsInvalid());
        ASSERT_EQ(word(captured[0], 20), JPH::BodyID::cInvalidBodyID);
        ASSERT_EQ(word(captured[0], 28), capsule.GetIndexAndSequenceNumber());
        ASSERT_GT(word(captured[0], 144), 0u);
        ASSERT_EQ(word(captured[0], 148), capsule.GetIndexAndSequenceNumber());
        Luminumbra::TestSupport::AvatarContinuationWorld target_world;
        auto& target = target_world.physics();
        AddBodyFloor(target);
        BodySetState before;
        ASSERT_TRUE(target.capture_body_owners(before));
        for (const int failure : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
            SCOPED_TRACE(failure);
            auto current = captured[0];
            auto pending = std::array{captured[1], captured[2]};
            std::size_t pending_count = 2;
            const auto unreserved =
                JPH::BodyID(capsule.GetIndex(),
                            static_cast<JPH::uint8>(capsule.GetSequenceNumber() + 1))
                    .GetIndexAndSequenceNumber();
            if (failure == 0) {
                pending_count = 0; // Missing reservation.
            } else if (failure == 1) {
                SetPhysicsWord(pending[0], 20, unreserved); // Same slot, wrong generation.
            } else if (failure == 2 || failure == 8) {
                // Use the other capsule's slot so the unchanged references to
                // the first capsule retain a consistent generation. The owner
                // stays codec-valid, but this exact body ID is not reserved.
                const JPH::BodyID other_capsule(word(pending[1], 20));
                const auto unreserved_reference =
                    JPH::BodyID(other_capsule.GetIndex(),
                                static_cast<JPH::uint8>(other_capsule.GetSequenceNumber() + 1))
                        .GetIndexAndSequenceNumber();
                SetPhysicsWord(current, failure == 2 ? 28 : 148, unreserved_reference);
            } else if (failure == 3) {
                pending[0].size = 0;
            } else if (failure == 4) {
                --pending[0].size;
            } else if (failure == 5) {
                pending[0] = captured[0]; // No capsule to reserve.
            } else if (failure == 6) {
                pending[1] = pending[0]; // Duplicate capsule slot.
            } else {
                const auto trailer = pending[1].size - (free_slot ? 12u : 8u);
                SetPhysicsWord(pending[1], trailer, word(pending[1], trailer) + 1);
                if (failure == 9) {
                    // Matching trailers must still explain every pool slot.
                    SetPhysicsWord(pending[0], trailer, word(pending[0], trailer) + 1);
                }
            }
            ASSERT_TRUE(ValidatePhysicsOwner(current));
            std::size_t index = 999;
            EXPECT_FALSE(target_world.restore_avatar_owner(
                current, index, std::span(pending).first(pending_count)));
            EXPECT_EQ(index, 999u);
            EXPECT_EQ(target.avatar_character_slot_count(), 0u);
            BodySetState after;
            ASSERT_TRUE(target.capture_body_owners(after));
            ExpectBodySet(before, after);
        }
        std::size_t index = 999;
        for (std::size_t slot = 0; slot < captured.size(); ++slot) {
            ASSERT_TRUE(target_world.restore_avatar_owner(
                captured[slot], index, std::span(captured).subspan(slot + 1)));
            EXPECT_EQ(index, slot);
        }
        for (std::size_t slot = 0; slot < captured.size(); ++slot) {
            PhysicsOwnerState restored;
            ASSERT_TRUE(target_world.capture_avatar_owner(slot, restored));
            ExpectControllerBits(captured[slot], restored);
        }
        BodySetState expected;
        BodySetState actual;
        ASSERT_TRUE(source.capture_body_owners(expected));
        ASSERT_TRUE(target.capture_body_owners(actual));
        ExpectBodySet(expected, actual);
        EXPECT_EQ(source.create_dynamic_sphere(Vec3(20), Vec3(0), 0.3f),
                  target.create_dynamic_sphere(Vec3(20), Vec3(0), 0.3f));
    }
}

TEST(ControllerContinuation, FrozenReservationsRefuseMalformedAndConflictingCaptures) {
    Luminumbra::TestSupport::AvatarContinuationWorld source_world;
    auto& source = source_world.physics();
    AddBodyFloor(source);
    std::array<PhysicsOwnerState, 3> captured{};
    for (std::size_t slot = 0; slot < captured.size(); ++slot) {
        const Vec3 feet(4.0f + 4.0f * static_cast<float>(slot), 10.0f, 8.0f);
        ASSERT_EQ(source.create_avatar_character(feet), slot);
        source.set_avatar_frozen(slot, true, feet);
    }
    for (std::size_t slot = 0; slot < captured.size(); ++slot) {
        ASSERT_TRUE(source_world.capture_avatar_owner(slot, captured[slot]));
    }
    Luminumbra::TestSupport::AvatarContinuationWorld target_world;
    auto& target = target_world.physics();
    AddBodyFloor(target);
    BodySetState before;
    ASSERT_TRUE(target.capture_body_owners(before));
    for (const int failure : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}) {
        auto pending = std::array{captured[1], captured[2]};
        if (failure == 0) {
            pending[0].size = 0;
        } else if (failure == 1) {
            --pending[0].size;
        } else if (failure == 2) {
            SetPhysicsWord(pending[0], 4, 99); // Unknown owner kind.
        } else if (failure == 3) {
            SetPhysicsWord(pending[0], 8, 0); // Moving controller cannot reserve a body.
            SetPhysicsWord(pending[0], 20, JPH::BodyID::cInvalidBodyID);
            pending[0].size -= 8; // Remove the absent body's allocator trailer.
        } else if (failure == 4) {
            SetPhysicsWord(pending[0], 20, JPH::BodyID::cInvalidBodyID);
            pending[0].size -= 8; // Frozen without a capsule is codec-valid.
        } else if (failure == 5) {
            SetPhysicsWord(pending[0], pending[0].size - 8, 5); // Different pool extent.
        } else if (failure == 6) {
            SetPhysicsWord(pending[0],
                           20,
                           JPH::BodyID(0, 1).GetIndexAndSequenceNumber()); // Occupied terrain slot.
        } else if (failure == 7) {
            pending[1] = pending[0]; // Duplicate reservation.
        } else if (failure == 8) {
            pending[0] = captured[0]; // Current owner's slot cannot also be reserved.
        } else if (failure == 9) {
            const auto wide = std::bit_cast<std::uint64_t>(static_cast<double>(1.0e30f));
            SetPhysicsWord(pending[0], 84, static_cast<std::uint32_t>(wide));
            SetPhysicsWord(pending[0], 88, static_cast<std::uint32_t>(wide >> 32));
        } else if (failure == 10) {
            const auto end = pending[0].size;
            pending[0].size += 4;
            SetPhysicsWord(pending[0], end - 4, 1); // Different free stack.
            SetPhysicsWord(pending[0], end, JPH::BodyID(1, 1).GetIndexAndSequenceNumber());
        } else {
            SetPhysicsWord(pending[0], 4, 2); // Other controller kind cannot reserve an avatar.
            SetPhysicsWord(pending[0], 8, 0);
            SetPhysicsWord(pending[0], 20, JPH::BodyID::cInvalidBodyID);
            pending[0].size -= 8;
        }
        if (failure == 3 || failure == 4 || failure == 5 || failure >= 9) {
            ASSERT_TRUE(ValidatePhysicsOwner(pending[0]));
        }
        std::size_t index = 999;
        EXPECT_FALSE(target_world.restore_avatar_owner(captured[0], index, pending));
        EXPECT_EQ(index, 999u);
        EXPECT_EQ(target.avatar_character_slot_count(), 0u);
        BodySetState after;
        ASSERT_TRUE(target.capture_body_owners(after));
        ExpectBodySet(before, after);
    }
    std::size_t index = 999;
    auto moving = captured[0];
    SetPhysicsWord(moving, 8, 0);
    SetPhysicsWord(moving, 20, JPH::BodyID::cInvalidBodyID);
    moving.size -= 8;
    ASSERT_TRUE(ValidatePhysicsOwner(moving));
    EXPECT_FALSE(target_world.restore_avatar_owner(moving, index, std::span(captured).subspan(1)));
    EXPECT_EQ(index, 999u);
    EXPECT_FALSE(target_world.restore_avatar_owner(
        captured[0], index, std::span(captured).subspan(1, 1))); // Missing reservation.
    EXPECT_EQ(index, 999u);
    ASSERT_TRUE(
        target_world.restore_avatar_owner(captured[0], index, std::span(captured).subspan(1)));
    EXPECT_EQ(index, 0u);
}

TEST(ControllerContinuation, FrozenCapsuleAdmissionRefusesConflictsWithoutAdopting) {
    Luminumbra::TestSupport::AvatarContinuationWorld source_world;
    auto& source = source_world.physics();
    AddBodyFloor(source);
    const auto slot = source.create_avatar_character(Vec3(8.0f, 10.0f, 8.0f));
    source.set_avatar_frozen(slot, true, source.get_avatar_position(slot));
    PhysicsOwnerState captured;
    ASSERT_TRUE(source_world.capture_avatar_owner(slot, captured));
    BodySetState capsules;
    ASSERT_TRUE(source.capture_body_owners(capsules));
    Luminumbra::TestSupport::AvatarContinuationWorld target_world;
    auto& target = target_world.physics();
    AddBodyFloor(target);
    std::size_t index = 999;
    auto incomplete_pool = captured;
    SetPhysicsWord(incomplete_pool, incomplete_pool.size - 8, capsules.slot_count + 1);
    EXPECT_FALSE(target_world.restore_avatar_owner(incomplete_pool, index));
    std::array<JPH::BodyID, 1> ids{};
    ASSERT_TRUE(target.restore_body_owners(capsules, ids));
    EXPECT_FALSE(target_world.restore_avatar_owner(incomplete_pool, index));
    EXPECT_FALSE(target_world.restore_avatar_owner(captured, index, std::span(&captured, 1)));
    auto displaced = captured;
    SetPhysicsWord(displaced, 84, 0);
    SetPhysicsWord(displaced, 88, 0x3ff00000u); // Valid float64 feet.x = 1.0.
    EXPECT_FALSE(target_world.restore_avatar_owner(displaced, index));
    auto stale = captured;
    SetPhysicsWord(stale, 20, JPH::BodyID(1, 2).GetIndexAndSequenceNumber());
    EXPECT_FALSE(target_world.restore_avatar_owner(stale, index));
    EXPECT_EQ(index, 999u);
    EXPECT_EQ(target.avatar_character_slot_count(), 0u);
    BodySetState after;
    ASSERT_TRUE(target.capture_body_owners(after));
    ExpectBodySet(capsules, after);
    ASSERT_TRUE(target_world.restore_avatar_owner(captured, index));
    EXPECT_EQ(index, 0u);
    EXPECT_FALSE(target_world.restore_avatar_owner(captured, index));
    EXPECT_EQ(target.avatar_character_slot_count(), 1u);

    Luminumbra::TestSupport::AvatarContinuationWorld conflicting_world;
    auto& conflicting = conflicting_world.physics();
    AddBodyFloor(conflicting);
    const auto sphere = conflicting.create_dynamic_sphere(Vec3(20), Vec3(0), 0.3f);
    ASSERT_EQ(sphere, ids[0]);
    index = 999;
    EXPECT_FALSE(conflicting_world.restore_avatar_owner(captured, index));
    EXPECT_EQ(index, 999u);
    EXPECT_EQ(conflicting.avatar_character_slot_count(), 0u);

    // A frozen owner can lack a capsule after ordinary headroom refusal.
    auto without_capsule = captured;
    SetPhysicsWord(without_capsule, 20, JPH::BodyID::cInvalidBodyID);
    without_capsule.size -= 8; // This fixture has an empty free stack.
    ASSERT_TRUE(ValidatePhysicsOwner(without_capsule));
    Luminumbra::TestSupport::AvatarContinuationWorld no_capsule_world;
    auto& no_capsule = no_capsule_world.physics();
    AddBodyFloor(no_capsule);
    ASSERT_TRUE(no_capsule_world.restore_avatar_owner(without_capsule, index));
    PhysicsOwnerState actual;
    ASSERT_TRUE(no_capsule_world.capture_avatar_owner(index, actual));
    ExpectControllerBits(without_capsule, actual);
}

} // namespace

namespace {
TEST(ControllerContinuation, FrozenAdoptionRefusalsAndMissingCapsulePreserveState) {
    Luminumbra::TestSupport::AvatarContinuationWorld source_world;
    auto& source = source_world.physics();
    AddBodyFloor(source);
    const auto index = source.create_avatar_character(Vec3(8, 10, 8));
    for (int tick = 0; tick < 45; ++tick) {
        source.update_avatars(1.0f / 30.0f);
    }
    source.set_avatar_frozen(index, true, source.get_avatar_position(index));
    PhysicsOwnerState captured;
    ASSERT_TRUE(source_world.capture_avatar_owner(index, captured));
    BodySetState capsules;
    ASSERT_TRUE(source.capture_body_owners(capsules));
    ASSERT_EQ(capsules.owners.size(), 1u);
    ASSERT_TRUE(capsules.free_slots.empty());
    for (const int conflict : {0, 1, 2, 3}) {
        Luminumbra::TestSupport::AvatarContinuationWorld target_world;
        auto& target = target_world.physics();
        AddBodyFloor(target);
        if (conflict < 2) {
            auto body = target.create_dynamic_sphere(Vec3(20), Vec3(0), 0.3f);
            ASSERT_FALSE(body.IsInvalid());
            if (conflict == 1) {
                target.destroy_body(body);
                body = target.create_dynamic_sphere(Vec3(20), Vec3(0), 0.3f);
                ASSERT_FALSE(body.IsInvalid());
            }
        } else {
            std::array<JPH::BodyID, 1> ids{};
            ASSERT_TRUE(target.restore_body_owners(capsules, ids));
        }
        auto invalid = captured;
        if (conflict == 2) {
            // A different exact feet position cannot adopt this static capsule.
            SetPhysicsWord(invalid, 84, 0);
            SetPhysicsWord(invalid, 88, 0x40220000u); // float64 9.0
        } else if (conflict == 3) {
            SetPhysicsWord(invalid, invalid.size - 8, capsules.slot_count + 1);
        }
        BodySetState before;
        ASSERT_TRUE(target.capture_body_owners(before));
        std::size_t restored_index = 999;
        EXPECT_FALSE(target_world.restore_avatar_owner(invalid, restored_index));
        EXPECT_EQ(restored_index, 999u);
        EXPECT_EQ(target.avatar_character_slot_count(), 0u);
        BodySetState after;
        ASSERT_TRUE(target.capture_body_owners(after));
        ExpectBodySet(before, after);
        if (conflict >= 2) {
            ASSERT_TRUE(target_world.restore_avatar_owner(captured, restored_index));
            EXPECT_FALSE(target_world.restore_avatar_owner(captured, restored_index));
            EXPECT_EQ(target.avatar_character_slot_count(), 1u);
        }
    }
    // Frozen controllers are also valid when capsule admission previously failed.
    // Omit the allocator footer along with its invalid owned-body ID.
    auto without_capsule = captured;
    SetPhysicsWord(without_capsule, 20, JPH::BodyID::cInvalidBodyID);
    without_capsule.size -= 8;
    ASSERT_TRUE(ValidatePhysicsOwner(without_capsule));
    Luminumbra::TestSupport::AvatarContinuationWorld target_world;
    auto& target = target_world.physics();
    AddBodyFloor(target);
    std::size_t restored_index = 999;
    ASSERT_TRUE(target_world.restore_avatar_owner(without_capsule, restored_index));
    PhysicsOwnerState actual;
    ASSERT_TRUE(target_world.capture_avatar_owner(restored_index, actual));
    ExpectControllerBits(without_capsule, actual);
    const auto before = target.get_avatar_position(restored_index);
    target.update_avatars(1.0f / 30.0f);
    EXPECT_EQ(target.get_avatar_position(restored_index), before);
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

TEST(BodyContinuation, RestoreKeepsLiveRegistrationOrderAndRegistersWhileTerrainIsOff) {
    PhysicsSystem source;
    source.startup();
    for (int i = 0; i < 3; ++i) {
        ASSERT_FALSE(source.create_dynamic_sphere({float(i), 100, 0}, {}, 0.1f).IsInvalid());
    }
    // Live creation registers spheres with terrain collisions off, in creation
    // order, exactly as restore does.
    BodySetState live;
    ASSERT_TRUE(source.capture_body_owners(live));
    ASSERT_EQ(live.owners.size(), 3u);
    for (std::uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(live.owners[i].registration_slot, i + 1);
    }
    auto unregistered = live;
    for (auto& owner : unregistered.owners) {
        owner.registration_slot = 0;
    }

    // Restore registers unregistered payloads even with terrain collisions off,
    // in payload order, matching the live original.
    PhysicsSystem first;
    first.startup();
    std::array<JPH::BodyID, 3> first_ids{};
    ASSERT_TRUE(first.restore_body_owners(unregistered, first_ids));
    for (std::uint32_t i = 0; i < 3; ++i) {
        BodyOwnerState owner;
        ASSERT_TRUE(first.capture_body_owner(first_ids[i], owner));
        EXPECT_EQ(owner.registration_slot, i + 1);
    }

    // Swap-removal moves the last registration into the freed slot.
    first.destroy_body(first_ids[0]);
    BodyOwnerState second_owner, third_owner;
    ASSERT_TRUE(first.capture_body_owner(first_ids[1], second_owner));
    ASSERT_TRUE(first.capture_body_owner(first_ids[2], third_owner));
    ASSERT_EQ(second_owner.registration_slot, 2u);
    ASSERT_EQ(third_owner.registration_slot, 1u);
    BodySetState reordered;
    ASSERT_TRUE(first.capture_body_owners(reordered));
    ASSERT_EQ(reordered.owners.size(), 2u);

    // The live order survives restore, independently of payload order.
    PhysicsSystem second;
    second.startup();
    std::array<JPH::BodyID, 2> second_ids{};
    ASSERT_TRUE(second.restore_body_owners(reordered, second_ids));
    for (std::size_t i = 0; i < 2; ++i) {
        BodyOwnerState owner;
        ASSERT_TRUE(second.capture_body_owner(second_ids[i], owner));
        EXPECT_EQ(owner.registration_slot, reordered.owners[i].registration_slot);
    }

    // Duplicate registration slots refuse before any body is created.
    auto duplicate = reordered;
    duplicate.owners[1].registration_slot = duplicate.owners[0].registration_slot;
    PhysicsSystem refused;
    refused.startup();
    std::array<JPH::BodyID, 2> refused_ids{};
    EXPECT_FALSE(refused.restore_body_owners(duplicate, refused_ids));
    EXPECT_TRUE(refused_ids[0].IsInvalid());
    BodySetState unchanged;
    ASSERT_TRUE(refused.capture_body_owners(unchanged));
    EXPECT_TRUE(unchanged.owners.empty());

    // Slots beyond the registration table refuse, including the sentinel's sort key.
    for (const std::uint32_t slot : {10241u, std::numeric_limits<std::uint32_t>::max()}) {
        auto out_of_range = reordered;
        out_of_range.owners[1].registration_slot = slot;
        PhysicsSystem bounded;
        bounded.startup();
        std::array<JPH::BodyID, 2> bounded_ids{};
        EXPECT_FALSE(bounded.restore_body_owners(out_of_range, bounded_ids));
        EXPECT_TRUE(bounded_ids[0].IsInvalid());
        BodySetState empty;
        ASSERT_TRUE(bounded.capture_body_owners(empty));
        EXPECT_TRUE(empty.owners.empty());
    }
}
