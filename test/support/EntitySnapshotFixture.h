#pragma once

// the game-flavored entity snapshot fixture (grovestrider_alpha /
// lantern_wisp / water_marker), relocated out of the engine header
// src/luminumbra_common/ecs/EntitySnapshot.h. The data is byte-identical to
// the pre-relocation fixture so every committed checksum (entity snapshot
// stability, network state hash) is unchanged. Test-support code only —
// never include from src/.

#include "luminumbra_common/ecs/EntitySnapshot.h"

namespace Luminumbra::TestSupport {

inline Luminumbra::Ecs::EntityRegistrySnapshot BuildEntitySnapshotFixture() {
    using Luminumbra::Ecs::EntityComponentSnapshot;
    using Luminumbra::Ecs::EntityRegistrySnapshot;
    using Luminumbra::Ecs::EntitySnapshotRecord;

    EntityRegistrySnapshot snapshot;
    snapshot.entities = {
        EntitySnapshotRecord{
            1001u,
            "grovestrider_alpha",
            {EntityComponentSnapshot{"Instinct",
                                     nlohmann::json{{"archetype", "grovestrider"},
                                                    {"dominant_need", "hunger"},
                                                    {"hunger", 0.94},
                                                    {"safety", 0.28}}},
             EntityComponentSnapshot{"PersistenceAnchor",
                                     nlohmann::json{{"save_priority", 9}, {"world_chunk_id", "0"}}},
             EntityComponentSnapshot{
                 "Transform",
                 nlohmann::json{{"position", {{"x", 2.0}, {"y", 5.0}, {"z", -1.0}}},
                                {"rotation_y", 0.25},
                                {"scale", 1.0}}}}},
        EntitySnapshotRecord{
            1002u,
            "lantern_wisp",
            {EntityComponentSnapshot{"AethericField",
                                     nlohmann::json{{"charge", 0.73}, {"radius", 4.5}}},
             EntityComponentSnapshot{
                 "PersistenceAnchor",
                 nlohmann::json{{"save_priority", 6}, {"world_chunk_id", "4194302"}}},
             EntityComponentSnapshot{
                 "Transform",
                 nlohmann::json{{"position", {{"x", -3.0}, {"y", 2.0}, {"z", 6.0}}},
                                {"rotation_y", 1.5},
                                {"scale", 0.6}}}}},
        EntitySnapshotRecord{
            1003u,
            "water_marker",
            {EntityComponentSnapshot{
                 "PersistenceAnchor",
                 nlohmann::json{{"save_priority", 4}, {"world_chunk_id", "18446739675667234817"}}},
             EntityComponentSnapshot{
                 "Transform",
                 nlohmann::json{{"position", {{"x", 8.0}, {"y", 1.25}, {"z", 11.0}}},
                                {"rotation_y", 0.0},
                                {"scale", 1.0}}},
             EntityComponentSnapshot{"WaterAffinity",
                                     nlohmann::json{{"flow_bias", {{"x", 0.1}, {"y", -0.1}}},
                                                    {"surface_lock", true}}}}}};
    Luminumbra::Ecs::SortEntityRegistrySnapshot(snapshot);
    return snapshot;
}

} // namespace Luminumbra::TestSupport
