#pragma once

// ONE-WAY sim->render snapshot of the ScentField's deposited trail channels
// (2 = food-trail, 3 = home-trail), for the pheromone ground decal. Written by
// main_client each frame AFTER the tick completes (a const Sample read), read only
// by GroundDecalPass. The sim NEVER reads this mirror back, so it is determinism-
// neutral by construction: there is no path from render state into the canonical tick
// or world_hash.
#include <cstddef>
#include <vector>

namespace Luminumbra::Rendering {

struct ScentFieldRenderMirror {
    int   cells = 0;          // grid is cells x cells (128)
    float cell_size = 1.0f;   // world units per cell
    float origin_x = 0.0f;    // world X of cell (0,0) corner
    float origin_z = 0.0f;    // world Z of cell (0,0) corner
    bool  valid = false;      // false => the decal pass is a no-op this frame
    bool  any_scent = false;  // false => all-zero field (no trails to draw yet)
    // Interleaved RG, row-major [(z*cells + x)*2 + {0=food,1=home}] — ready for an
    // RG16F glTexSubImage2D with no further repacking.
    std::vector<float> rg;

    void resize(int n) {
        cells = n;
        rg.assign(static_cast<std::size_t>(n) * n * 2, 0.0f);
    }
};

} // namespace Luminumbra::Rendering
