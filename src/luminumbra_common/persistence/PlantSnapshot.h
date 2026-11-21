#pragma once

#include "ecs/EntitySnapshot.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::world {
class ActiveRegionLedger;
class GroundObjectStore;
} // namespace Luminumbra::world

namespace Luminumbra::Persistence {

// Candidate plant-specific format. Shipping sessions retain their existing codec
// until the authority transition. IDs never contain runtime entity handles.
struct PlantSnapshot {
    std::uint64_t next_id = 1;
    std::vector<Ecs::EntitySnapshotRecord> entities;
};

inline constexpr std::size_t kMaxPlantSnapshotRecords = 1u << 20;
inline constexpr std::size_t kMaxPlantSnapshotComponents = 17;
inline constexpr std::string_view kPlantSnapshotSchema = "luminumbra.plants.v2";
inline constexpr std::string_view kPlantSnapshotOrder =
    "simulation_id_ascending_component_type_ascending";
inline constexpr std::string_view kUnsupportedPlantSnapshot =
    "Unsupported pre-v2 simulation save; this build requires plant identity format v2.";

// Snapshot and byte outputs remain unchanged on refusal. Components have version 1;
// missing components stay absent. Wire integers are unsigned except owner coordinates.
// Exact binary32 values (including signed zero) are preserved as JSON floats.
bool ValidatePlantSnapshot(const PlantSnapshot& snapshot, std::string& error);
bool ValidatePlantSnapshot(const PlantSnapshot& snapshot,
                           const world::ActiveRegionLedger& ledger,
                           const world::GroundObjectStore& ground,
                           std::string& error);
bool EncodePlantSnapshot(const PlantSnapshot& snapshot, std::string& bytes, std::string& error);
bool DecodePlantSnapshot(std::string_view bytes, PlantSnapshot& out, std::string& error);

} // namespace Luminumbra::Persistence
