#pragma once

// Wave-3 far-field tree impostors, slice 2: the ATLAS BAKE.
//
// Renders the static tree parts from each hemi-octahedral view direction
// (OctaImpostor.h) into a single texture atlas — one tile per direction — and
// writes it out (PPM + a per-tile coverage JSON) for review. A distant tree later
// draws ONE camera-facing quad sampling the tile(s) nearest the view direction, so
// a vast forest's far field collapses to a few quads + one shared atlas.
//
// This is a render-only offline-ish bake: it needs a current GL context (run from
// the client's render loop via --bake-tree-impostor). Nothing here is hashed.

#include "OctaImpostor.h"

#include <string>

namespace Luminumbra::Rendering {

struct ImpostorBakeResult {
    bool ok = false;
    int atlas_size = 0;       // atlas side length in pixels (gridResolution * tileResolution)
    float mean_coverage = 0;  // mean fraction of non-background (tree) pixels across all tiles
    float min_coverage = 0;   // the emptiest tile's coverage (a sanity floor — 0 => a blank tile)
    std::string error;        // populated when ok == false
};

// Bakes the tree leaf + branch parts into an octahedral impostor atlas and writes:
//   <outBasePath>            -> the atlas as a binary PPM (RGB; tree on a keyed background)
//   <outBasePath>.json       -> { atlas_size, grid, mean_coverage, min_coverage, per-tile coverage }
// rootDir is the asset root (the tree parts load from rootDir/data/models/trees/...).
// Requires a current GL context. Returns coverage stats / an error.
ImpostorBakeResult BakeTreeImpostorAtlas(const std::string& outPpmPath,
                                         const std::string& rootDir,
                                         const OctaImpostorGrid& grid);

} // namespace Luminumbra::Rendering
