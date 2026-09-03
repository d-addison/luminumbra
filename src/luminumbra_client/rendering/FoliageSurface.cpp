#include "rendering/FoliageSurface.h"

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"

#include <algorithm>
#include <cmath>

namespace Luminumbra::Rendering {

FoliagePass::SurfaceSample FoliageSurfaceQuery(void* ctx, float world_x, float world_z) {
    FoliagePass::SurfaceSample s;
    auto* fctx = static_cast<FoliageScatterContext*>(ctx);
    if (fctx == nullptr || fctx->world_system == nullptr) {
        s.valid = false;
        return s;
    }
    Luminumbra::Systems::SHIELD_WorldSystem* ws = fctx->world_system;
    const float h = ws->GetTerrainHeightAt(world_x, world_z);
    s.height = h;
    // Underwater columns carry no ground-cover foliage.
    if (h <= Luminumbra::SEA_LEVEL) {
        s.valid = false;
        return s;
    }
    // ROOFED-CAVE REJECT (cave bug A): GetTerrainHeightAt is the ANALYTIC heightmap
    // surface and ignores the cave SDF. Where a cavern roof rises above that surface,
    // the heightmap point sits INSIDE a roofed air pocket, so grass cards were being
    // planted on ledges deep in cave chambers (green albedo patches underground). Only
    // OPEN-SKY columns should grow ground cover: probe straight up from the surface and
    // reject if SOLID terrain lies within a short overhead span. DENSITY CONVENTION
    // ( root cause — this probe shipped INVERTED and rejected every open-sky
    // column, defoliating the world): worldgen density = (y - height) + cave carve, so
    // SOLID = density < 0 and air = >= 0 (the mesher's solid corner is val < iso 0).
    // Pure read of the deterministic SDF — render-only, never hashed.
    {
        constexpr float kRoofProbeM = 6.0f; // solid this far overhead == roofed
        constexpr float kRoofStep = 1.0f;
        for (float up = kRoofStep; up <= kRoofProbeM; up += kRoofStep) {
            if (ws->get_density_at(Luminumbra::Vec3(world_x, h + up, world_z)) < 0.0f) {
                s.valid = false; // a roof overhead -> underground, no sky-lit grass
                return s;
            }
        }
    }
    // Slope from a 1 m central finite difference of the shaped height (pure).
    const float hx = ws->GetTerrainHeightAt(world_x + 1.0f, world_z);
    const float hz = ws->GetTerrainHeightAt(world_x, world_z + 1.0f);
    const float grad = std::sqrt((hx - h) * (hx - h) + (hz - h) * (hz - h));
    s.slope = std::clamp(grad, 0.0f, 1.0f); // 1 m rise over 1 m == slope 1
    // Moisture proxy: the biome density already encodes it; modulate mildly by a
    // height-band proxy (lower/flatter ground reads wetter). Render-only.
    s.moisture = std::clamp(1.0f - s.slope, 0.0f, 1.0f);
    s.valid = true;
    return s;
}

} // namespace Luminumbra::Rendering
