//  the server-authoritative player avatar primitives.
// Proves the deterministic spawn layout + the entity-snapshot projection are
// pure/reproducible and that an empty avatar set yields the empty snapshot
// (byte-identical to the zero-avatar headless `entities` lane). The avatar->streaming
// ->entities-sub-hash lane end-to-end is validated by `--smoke --avatars N`
// (the existing HeadlessServerTick double-run asserts the entities sub-hash
// matches run-to-run).
#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
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
