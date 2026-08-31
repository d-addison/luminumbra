#pragma once

//  far-field tree impostors, : the ATLAS BAKE.
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

class RenderPipeline; // for the loaded static-model texture array + per-part layers

struct ImpostorBakeResult {
    bool ok = false;
    int atlas_size = 0;      // atlas side length in pixels (gridResolution * tileResolution)
    float mean_coverage = 0; // mean fraction of non-background (tree) pixels across all tiles
    float min_coverage = 0;  // the emptiest tile's coverage (a sanity floor — 0 => a blank tile)
    std::string error;       // populated when ok == false
};

// Bakes the tree leaf + branch parts into an octahedral impostor atlas and writes:
//   <outBasePath>            -> the atlas as a binary PPM (RGB; tree on a keyed background)
//   <outBasePath>.json       -> { atlas_size, grid, mean_coverage, min_coverage, per-tile coverage
//   }
// rootDir is the asset root (the tree parts load from rootDir/data/models/trees/...). rp supplies
// the loaded static-model texture array + per-part albedo layer / luma-cutout flag so the bake
// renders the REAL bark/leaf textures (leaves luma-keyed, matching g_buffer.frag). Requires a
// current GL context.
ImpostorBakeResult BakeTreeImpostorAtlas(const std::string& outPpmPath,
                                         const std::string& rootDir,
                                         const RenderPipeline& rp,
                                         const OctaImpostorGrid& grid);

// Runtime variant: bakes the impostor atlas into two KEPT GL_TEXTURE_2D handles (albedo +
// object-space normal) for the runtime LOD3 impostor draw to sample. The caller owns/deletes the
// textures. Also reports the tree's local-space bounding sphere (center + radius) so the runtime
// billboard can be sized to match the geometry it replaces. No disk output. Requires a current GL
// context.
struct ImpostorAtlasTextures {
    bool ok = false;
    unsigned int albedoTex =
        0; // GL_TEXTURE_2D, atlas_size^2, sRGB-encoded albedo (magenta = empty)
    unsigned int normalTex = 0; // GL_TEXTURE_2D, atlas_size^2, encoded object-space normal
    int grid = 0;               // tiles per axis (== OctaImpostorGrid.gridResolution)
    float sphereY = 0; // tree local bounding-sphere center Y (height the billboard centers on)
    float radius = 0;  // tree local bounding-sphere radius (billboard half-size)
    std::string error;
};
ImpostorAtlasTextures BakeTreeImpostorAtlasToTextures(const std::string& rootDir,
                                                      const RenderPipeline& rp,
                                                      const OctaImpostorGrid& grid);

} // namespace Luminumbra::Rendering
