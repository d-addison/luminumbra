#pragma once

// =============================================================================
// FoliageSurface — the world surface-query callback behind FoliagePass scatter.
// =============================================================================
// Resolves, at a world (x,z), the terrain surface height + a slope estimate +
// a moisture estimate for ground-cover placement. All values are PURE
// functions of the world generator (seed, params) — no RNG, no sim writes.
// The slope is derived from finite-difference height samples; the moisture
// from the biome humidity proxy (the biome density already encodes it, so a
// mild render-side modulation suffices). The query skips underwater columns
// and roofed-cave columns. Shared by the shipping client's in-game scatter
// (main_client), the worldgen preview diorama, and the QA scenario harness.

#include "rendering/passes/FoliagePass.h"

namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
}

namespace Luminumbra::Rendering {

// Context handed (type-erased) to FoliagePass::SurfaceQuery callbacks.
struct FoliageScatterContext {
    Luminumbra::Systems::SHIELD_WorldSystem* world_system = nullptr;
};

FoliagePass::SurfaceSample FoliageSurfaceQuery(void* ctx, float world_x, float world_z);

} // namespace Luminumbra::Rendering
