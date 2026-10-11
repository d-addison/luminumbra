#pragma once

#include "world/WorldStreamingState.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::Persistence {

// Deterministic synthetic saved-world writer. No worldgen is involved: every
// value comes from a splitmix64 hash of (seed, chunk index, lattice index) or
// from a fixed constant. The output is intended to be byte-identical across
// repeated runs on one host. Cross-host identity is observed and reported by
// the tooling, not promised.
//
// Coordinates: chunk i maps to column j = i / 2 and cy = i % 2. Column j maps
// to (cx, cz) on a square spiral from (0, 0) with directions +x, +z, -x, -z and
// run lengths 1, 1, 2, 2, 3, 3, ... The first columns are
// (0,0) (1,0) (1,1) (0,1) (-1,1) (-1,0) (-1,-1) (0,-1) (1,-1) (2,-1) (2,0) (2,1) (2,2).
enum class FixturePayload {
    Noise,
    Flat
};

struct SyntheticSaveSpec {
    std::uint64_t seed = 1337;
    std::uint32_t chunk_count = 96;
    std::uint32_t edit_count = 0; // first K chunks get a deterministic mound added
    FixturePayload payload = FixturePayload::Noise;
    std::string world_id = "synthetic"; // directory name under worlds/saves
    std::string world_name = "Synthetic Fixture";
    std::filesystem::path preset_source; // if non-empty: copied byte-for-byte to preset.json
};

struct SyntheticSaveReport {
    bool ok = false;
    std::string error;
    std::filesystem::path save_dir; // <root>/worlds/saves/<world_id>
    std::uint32_t chunks_written = 0;
    std::uint32_t region_files = 0;
    std::uint64_t region_bytes = 0; // sum of r.*.lmr sizes
};

std::vector<IVec3> SyntheticChunkCoords(std::uint32_t count);
SyntheticSaveReport WriteSyntheticSave(const SyntheticSaveSpec& spec,
                                       const std::filesystem::path& root);

} // namespace Luminumbra::Persistence
