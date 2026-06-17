#include "PlayerAvatar.h"

#include <cmath>

namespace Luminumbra::World {

namespace {
// Quantize a world-unit length to integer millimetres (deterministic, formatting-
// independent hash input). std::lround is bit-stable for the finite values here.
std::int64_t to_mm(float v) {
    return static_cast<std::int64_t>(std::lround(static_cast<double>(v) * 1000.0));
}
} // namespace

Vec3 DeterministicAvatarSpawnOffset(std::uint32_t player_id) {
    if (player_id == 0) {
        return Vec3(0.0f);
    }
    // Phyllotaxis ("sunflower") layout: angle = id * golden angle, radius grows
    // as sqrt(id) * spacing so the points fan out evenly with no overlap. PURE.
    constexpr float kGoldenAngle = 2.39996323f; // radians (137.5 degrees)
    constexpr float kSpacingM = 4.0f;           // ~4 m between adjacent avatars
    const float angle = static_cast<float>(player_id) * kGoldenAngle;
    const float radius = kSpacingM * std::sqrt(static_cast<float>(player_id));
    return Vec3(radius * std::cos(angle), 0.0f, radius * std::sin(angle));
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
            {"facing_mrad", static_cast<std::int64_t>(std::lround(static_cast<double>(a.facing) * 1000.0))},
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

} // namespace Luminumbra::World
