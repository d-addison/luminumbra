#include "PlayerAvatar.h"
#include "../systems/PhysicsSystem.h"

#include "../components/CoreComponents.h"
#include "../core/DeterministicMath.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace Luminumbra::World {

namespace {
// Quantize a world-unit length to integer millimetres (deterministic, formatting-
// independent hash input). std::lround is bit-stable for the finite values here.
std::int64_t to_mm(float v) {
    return static_cast<std::int64_t>(std::lround(static_cast<double>(v) * 1000.0));
}
} // namespace

namespace {
bool ValidAvatar(const PlayerAvatar& avatar) {
    return std::isfinite(avatar.position.x) && std::isfinite(avatar.position.y) &&
           std::isfinite(avatar.position.z) && std::isfinite(avatar.facing) &&
           std::isfinite(avatar.velocity.x) && std::isfinite(avatar.velocity.y) &&
           std::isfinite(avatar.velocity.z);
}

void WriteOwnerWord(std::span<std::uint8_t> output, std::size_t& cursor, std::uint32_t word) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output[cursor++] = static_cast<std::uint8_t>(word >> shift);
    }
}

std::uint32_t ReadOwnerWord(std::span<const std::uint8_t> input, std::size_t& cursor) {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        result |= static_cast<std::uint32_t>(input[cursor++]) << shift;
    }
    return result;
}
} // namespace

bool ValidateAvatarOwner(const PlayerAvatarOwnerState& state) {
    return ValidAvatar(state.avatar) &&
           Systems::PhysicsSystem::validate_avatar_owner_state(state.physics, state.avatar);
}

bool EncodeAvatarOwner(const PlayerAvatarOwnerState& state,
                       std::span<std::uint8_t> output,
                       std::size_t& written) {
    // Header: version, player ID, position xyz, yaw, velocity xyz, physics size.
    // Validate first; neither output bytes nor written change on refusal.
    if (!ValidateAvatarOwner(state) || output.size() < 40 ||
        state.physics.size > output.size() - 40) {
        return false;
    }
    std::array<std::uint8_t, kMaxAvatarOwnerBytes> staged{};
    const std::span<std::uint8_t> destination(staged);
    std::size_t cursor = 0;
    WriteOwnerWord(destination, cursor, 1);
    WriteOwnerWord(destination, cursor, state.avatar.player_id);
    const std::array<float, 7> scalars{state.avatar.position.x,
                                       state.avatar.position.y,
                                       state.avatar.position.z,
                                       state.avatar.facing,
                                       state.avatar.velocity.x,
                                       state.avatar.velocity.y,
                                       state.avatar.velocity.z};
    for (const auto scalar : scalars) {
        WriteOwnerWord(destination, cursor, std::bit_cast<std::uint32_t>(scalar));
    }
    WriteOwnerWord(destination, cursor, state.physics.size);
    std::copy_n(state.physics.bytes.begin(), state.physics.size, destination.begin() + 40);
    const std::size_t total = 40 + state.physics.size;
    std::copy_n(staged.begin(), total, output.begin());
    written = total;
    return true;
}

bool DecodeAvatarOwner(std::span<const std::uint8_t> input, PlayerAvatarOwnerState& output) {
    if (input.size() < 40 || input.size() > kMaxAvatarOwnerBytes) {
        return false;
    }
    std::size_t cursor = 0;
    if (ReadOwnerWord(input, cursor) != 1) {
        return false;
    }
    PlayerAvatarOwnerState staged;
    staged.avatar.player_id = ReadOwnerWord(input, cursor);
    std::array<float, 7> scalars{};
    for (auto& scalar : scalars) {
        scalar = std::bit_cast<float>(ReadOwnerWord(input, cursor));
    }
    staged.avatar.position = Vec3(scalars[0], scalars[1], scalars[2]);
    staged.avatar.facing = scalars[3];
    staged.avatar.velocity = Vec3(scalars[4], scalars[5], scalars[6]);
    staged.physics.size = ReadOwnerWord(input, cursor);
    if (!ValidAvatar(staged.avatar) || staged.physics.size != input.size() - 40) {
        return false;
    }
    std::copy(input.begin() + 40, input.end(), staged.physics.bytes.begin());
    if (!ValidateAvatarOwner(staged)) {
        return false;
    }
    output = staged;
    return true;
}

Vec3 DeterministicAvatarSpawnOffset(std::uint32_t player_id) {
    if (player_id == 0) {
        return Vec3(0.0f);
    }
    // Phyllotaxis ("sunflower") layout: angle = id * golden angle, radius grows
    // as sqrt(id) * spacing so the points fan out evenly with no overlap. PURE.
    constexpr float kGoldenAngle = 2.39996323f; // radians (137.5 degrees)
    constexpr float kSpacingM = 4.0f;           // ~4 m between adjacent avatars
    const float angle = static_cast<float>(player_id) * kGoldenAngle;
    // DeterministicMath:: (not libm) so the spawn layout is cross-platform
    // bit-stable: this offset feeds avatar position -> the `entities` world_hash.
    const float radius = kSpacingM * DeterministicMath::Sqrt(static_cast<float>(player_id));
    return Vec3(
        radius * DeterministicMath::Cos(angle), 0.0f, radius * DeterministicMath::Sin(angle));
}

Ecs::EntityRegistrySnapshot BuildAvatarEntitySnapshot(const std::vector<PlayerAvatar>& avatars) {
    Ecs::EntityRegistrySnapshot snapshot;
    snapshot.entities.reserve(avatars.size());
    for (const PlayerAvatar& a : avatars) {
        Ecs::EntitySnapshotRecord record;
        record.entity_id = static_cast<std::uint64_t>(a.player_id);
        record.name = "player_avatar";

        Ecs::EntityComponentSnapshot component;
        component.type = "PlayerAvatar";
        component.data = nlohmann::json{
            {"player_id", a.player_id},
            // Fixed-point millimetres / milli-radians: deterministic + float-
            // formatting independent (the bit-reproducible `entities` sub-hash).
            {"px_mm", to_mm(a.position.x)},
            {"py_mm", to_mm(a.position.y)},
            {"pz_mm", to_mm(a.position.z)},
            {"facing_mrad",
             static_cast<std::int64_t>(std::lround(static_cast<double>(a.facing) * 1000.0))},
            {"vx_mm_s", to_mm(a.velocity.x)},
            {"vy_mm_s", to_mm(a.velocity.y)},
            {"vz_mm_s", to_mm(a.velocity.z)},
        };
        record.components.push_back(std::move(component));
        snapshot.entities.push_back(std::move(record));
    }
    // SerializeEntityRegistrySnapshotJson sorts by the contract, but sort here too
    // so any direct consumer sees the canonical order.
    Ecs::SortEntityRegistrySnapshot(snapshot);
    return snapshot;
}

std::vector<Net::ReplEntityState> BuildEntityReplStates(const entt::registry& registry) {
    std::vector<Net::ReplEntityState> states;
    auto view =
        registry
            .view<const Components::TransformComponent, const Components::ReplicatedComponent>();
    for (auto entity : view) {
        const auto& tf = view.get<const Components::TransformComponent>(entity);
        const auto& rep = view.get<const Components::ReplicatedComponent>(entity);
        Net::ReplEntityState s;
        s.entity_id = rep.network_id;
        s.px_mm = Net::ReplQuantPos(tf.position.x);
        s.py_mm = Net::ReplQuantPos(tf.position.y);
        s.pz_mm = Net::ReplQuantPos(tf.position.z);
        // Yaw from the transform's facing (rotate +Z forward, atan2 in the XZ plane).
        const Vec3 fwd = tf.rotation * Vec3(0.0f, 0.0f, 1.0f);
        // DeterministicMath::Atan2 (not libm) -- this yaw feeds yaw_mrad onto the
        // replication wire, so it must be cross-platform bit-stable.
        s.yaw_mrad = Net::ReplQuantAngle(DeterministicMath::Atan2(fwd.x, fwd.z));
        s.flags = rep.flags;
        s.type_id = rep.type_id;
        s.anim_state = rep.anim_state;
        s.anim_phase = rep.anim_phase;
        states.push_back(s);
    }
    // Stable order by network_id (snapshot determinism / readable diffs).
    std::sort(states.begin(),
              states.end(),
              [](const Net::ReplEntityState& a, const Net::ReplEntityState& b) {
                  return a.entity_id < b.entity_id;
              });
    return states;
}

std::vector<Net::ReplEntityState> BuildAvatarReplStates(const std::vector<PlayerAvatar>& avatars) {
    std::vector<Net::ReplEntityState> states;
    states.reserve(avatars.size());
    for (const PlayerAvatar& a : avatars) {
        Net::ReplEntityState s;
        s.entity_id = a.player_id;
        s.px_mm = Net::ReplQuantPos(a.position.x);
        s.py_mm = Net::ReplQuantPos(a.position.y);
        s.pz_mm = Net::ReplQuantPos(a.position.z);
        s.yaw_mrad = Net::ReplQuantAngle(a.facing);
        s.flags = 0; // bit0 grounded reserved (PlayerAvatar has no grounded field yet)
        states.push_back(s);
    }
    return states;
}

} // namespace Luminumbra::World
