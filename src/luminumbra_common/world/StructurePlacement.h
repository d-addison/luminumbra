#pragma once

// Structure placement + jigsaw assembly (, the deterministic runtime contract section 5).
//
// Placement is a spacing/separation/salt GRID per structure type: the world is
// tiled into spacing x spacing metre cells; a per-cell salted hash decides
// whether a site spawns in that cell (separation/density) and jitters its
// position inside the cell. This is O(1) per query, fully deterministic
// (pure function of seed + type salt + cell), and supports locate(type, near):
// scan the cells in a small radius around `near` and return the nearest site.
//
// Assembly is a JIGSAW over template POOLS that are GAME DATA under
// data/common/structures/<type>/ (JSON voxel-box pieces with material ids +
// socket joints). The engine knows only how to: pick a deterministic root
// piece, then attach pieces at matching sockets to assemble a voxel set. The
// assembled voxels are written through the NORMAL edit path (caller stamps them
// as chunk edits), so structures persist as edited chunks with no new format.
//
// Determinism contract: every choice (site presence, jitter, root piece, socket
// attachment) is driven by a SplitMix64 stream seeded from
// (world_seed, type_salt, cell or site coordinates). ComputeContentHash over the
// loaded template pool is fnv1a64 of the canonicalized pieces; the assembled
// voxel set has a stable hash (gate-snapshotted).

#include "../../../include/luminumbra/core/Types.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Luminumbra::World {

// A single voxel box piece: an axis-aligned solid box of one material, in
// piece-local coordinates (metres). A piece is a small set of boxes plus named
// socket joints other pieces can attach to.
struct StructureSocket {
    std::string name;
    IVec3 position{0, 0, 0}; // piece-local attach point
};

struct StructureBox {
    IVec3 min{0, 0, 0};  // inclusive, piece-local
    IVec3 size{1, 1, 1}; // extent in voxels (>= 1 on each axis)
    u8 material = 0;     // MaterialType id
};

struct StructurePiece {
    std::string name;
    std::vector<StructureBox> boxes;
    std::vector<StructureSocket> sockets;
};

// A loaded template pool for one structure type (one data/common/structures/<type>/).
struct StructureTemplatePool {
    std::string type;
    // Placement grid parameters for this type.
    int spacing = 64;    // grid cell size in metres (site spacing)
    int separation = 16; // min jitter margin kept from the cell edge
    u32 salt = 0;        // per-type seed salt (registry; collisions = defect)
    // 0..1 probability a given grid cell hosts a site (density). Quantized
    // deterministically against the cell hash.
    float density = 1.0f;
    std::vector<StructurePiece> pieces;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    u64 content_hash = 0;
    // conservative half-extent (metres) of any assembled structure on the
    // X/Z plane relative to the site origin. Computed once in
    // LoadStructureTemplatePool as the max over every piece's boxes (and socket
    // attach points, which can translate jigsaw pieces away from the origin) of
    // max(|min|, |min + size|) on X and Z. The chunk-stamp enumeration pads the
    // chunk AABB by this radius so a structure whose voxels straddle a chunk
    // border (cairn boxes have negative mins) is enumerated by every chunk it
    // touches; each chunk then writes only the in-bounds voxels (disjoint subset
    // => boundary-complete, no double-stamp, order-independent). Folded into
    // content_hash so a template change self-invalidates far tiles.
    int footprint_radius = 0;

    bool ok() const {
        return errors.empty() && !pieces.empty();
    }
};

// One placed structure site: world position of the assembly origin.
struct StructureSite {
    std::string type;
    IVec3 origin{0, 0, 0}; // world voxel coordinates of the assembly root
    u64 site_seed = 0;     // SplitMix64 stream seed for this site's assembly
};

// One assembled voxel (world coordinates, material id). The assembly is the
// piece-local boxes expanded to world voxels at the site origin.
struct StructureVoxel {
    IVec3 position{0, 0, 0};
    u8 material = 0;
};

// Loads every piece JSON under data/common/structures/<type>/ plus the
// type's placement.json (spacing/separation/salt/density). Unknown keys are
// warnings, never errors (loader discipline matches TerrainPresetLoader).
StructureTemplatePool LoadStructureTemplatePool(const std::filesystem::path& type_dir,
                                                const std::string& type);

// --- Deterministic placement grid ---
//
// Returns the site (if any) hosted by the grid cell that contains world (wx,wz)
// for this pool, as a pure function of (world_seed, pool.salt, cell). The
// y origin is left to the caller (placement drops onto the terrain surface).
std::optional<StructureSite>
SiteInCell(const StructureTemplatePool& pool, int world_seed, int cell_x, int cell_z);

// O(1) nearest-site query: scans the cells within `search_radius_cells` of the
// cell containing (near_x, near_z) and returns the nearest hosted site, or
// nullopt if none. Deterministic.
std::optional<StructureSite> LocateNearestSite(const StructureTemplatePool& pool,
                                               int world_seed,
                                               int near_x,
                                               int near_z,
                                               int search_radius_cells = 2);

// All sites whose grid cell centre falls inside [min, max) on X/Z (half-open),
// in deterministic cell-scan order. Used to enumerate sites overlapping a
// region for placement.
std::vector<StructureSite> SitesInArea(
    const StructureTemplatePool& pool, int world_seed, int min_x, int min_z, int max_x, int max_z);

// --- Jigsaw assembly ---
//
// Assembles the site's voxel set: a deterministic root piece, then attaches
// pieces at matching sockets (a socket on the placed set joins a socket on a
// candidate piece). Returns voxels in world coordinates relative to site.origin.
// Pure function of (pool, site.site_seed).
std::vector<StructureVoxel> AssembleStructure(const StructureTemplatePool& pool,
                                              const StructureSite& site);

// fnv1a64 over a canonicalized assembled voxel set (sorted by position then
// material). Gate snapshot of the assembled fixture.
u64 ComputeAssembledVoxelHash(const std::vector<StructureVoxel>& voxels);

} // namespace Luminumbra::World
