#pragma once

#include "world/WorldStreamingState.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::Persistence {

// Persists WorldStreamingState snapshots beneath a world save directory.
//
// Current on-disk layout is a single versioned snapshot file at
// <save_dir>/chunks/world-state.json that reuses the schema header emitted by
// SerializeWorldStreamingStateSnapshotJson. All writes funnel through the
// private write_snapshot() seam so the layout can later split into per-chunk
// files without changing the public API.
class WorldSaveService {
public:
    // Canonical location of the world state snapshot inside a save directory.
    static std::filesystem::path world_state_path(const std::filesystem::path& save_dir);

    // Serializes the full streaming state into the save directory, creating
    // intermediate directories as needed. Returns false (with diagnostics in
    // errors when provided) if the snapshot could not be written.
    bool save_world(
        const WorldStreamingState& state,
        const std::filesystem::path& save_dir,
        std::vector<std::string>* errors = nullptr) const;

    // Loads a previously saved snapshot into state. A missing snapshot file is
    // a clean miss (fresh world): returns false WITHOUT appending an error.
    // Malformed or unreadable snapshots return false with diagnostics.
    bool load_world(
        WorldStreamingState& state,
        const std::filesystem::path& save_dir,
        std::vector<std::string>& errors) const;

    // Deterministic hash of the streaming state, reusing the persistence hash
    // machinery (fnv1a_64 over the canonical snapshot bytes).
    std::string world_hash(const WorldStreamingState& state) const;

private:
    bool write_snapshot(
        const std::string& snapshot_json,
        const std::filesystem::path& save_dir,
        std::vector<std::string>* errors) const;
};

} // namespace Luminumbra::Persistence
