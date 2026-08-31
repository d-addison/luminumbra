#pragma once

// deterministic thermal + hydraulic erosion over a square height tile.
//
// ENGINE-GENERIC worldgen primitive (no game concepts). Given an analytic
// pre-erosion height field sampled over an INTERIOR n x n region PLUS a `halo`
// ring of padding on every side (so the input is (n + 2*halo) x (n + 2*halo)
// row-major), it runs a fixed number of erosion iterations and returns the
// per-cell additive OFFSET (eroded_height - input_height) for the INTERIOR only.
// Callers add the offset to the analytic height so the player walks the eroded
// surface (hydraulic erosion integration wires this into the shared height path; hydraulic erosion
// kernel is the pure kernel).
//
// DETERMINISM (critical -- this feeds a world_hash bump): pure function of
// (input heights, params). Fixed canonical iteration order (row-major), fixed
// iteration count, only IEEE +,-,*,/ and DeterministicMath::Sqrt -- NO libm
// transcendentals, NO Exp (rates are linear), NO RNG, NO wall-clock. Bit-
// identical across runs/machines; SimDeterminismLint-clean.
//
// HALO-INDEPENDENCE (the brief's #1 correctness risk): each iteration propagates
// information at most ONE cell (von-Neumann 4-neighbour stencil), so after K
// iterations an interior cell depends only on cells within Chebyshev distance K.
// Therefore, as long as halo >= iterations, the cropped INTERIOR offset is
// byte-identical regardless of how much terrain lies beyond the halo. The bake
// REQUIRES halo >= iterations and asserts it; the acceptance test bakes the same
// interior with halo == iterations and halo == iterations + margin and requires
// byte-identical interior offsets.

#include <cstdint>
#include <vector>

#include "../../include/luminumbra/core/Types.h"

namespace Luminumbra::World {

// Cache salt for shape-affecting kernel changes that are not represented by
// TerrainGenParams. Incrementing this value deliberately invalidates generated
// terrain caches while leaving worlds with erosion disabled byte-stable.
inline constexpr u32 kHydraulicErosionWorldgenVersion = 2u;

// PINNED erosion parameters. Changing ANY value changes the baked offset bits
// and is therefore a deliberate world_hash bump; these values are frozen.
struct HydroErosionParams {
    int iterations = 24; // fixed sweeps; also the influence radius (cells)
    // Thermal (talus) transport: material above the stable slope slides downhill.
    float talus_height = 1.2f; // max stable per-cell height delta (m)
    float thermal_rate = 0.5f; // fraction of excess moved per sweep [0..1]
    // Hydraulic transport: a fixed rain column erodes by stream power and
    // deposits when over capacity; linear evaporation (no Exp).
    float rain_per_sweep = 0.02f;    // water added per cell per sweep
    float solubility = 0.10f;        // height eroded per unit (slope * water)
    float deposition = 0.10f;        // fraction of over-capacity sediment dropped
    float evaporation = 0.20f;       // linear water loss per sweep [0..1]
    float sediment_capacity = 0.40f; // capacity per unit (slope * water)
    float max_offset = 24.0f;        // clamp |offset| (m) so a bake can't explode
};

// Bake the interior n x n erosion offset. `heights` is the padded
// (n + 2*halo)^2 row-major analytic height field; `out_offset` is filled with
// n*n interior offsets in row-major order. halo MUST be >= params.iterations
// (asserted). Pure + deterministic.
void BakeHydraulicErosion(const std::vector<float>& heights,
                          int n,
                          int halo,
                          const HydroErosionParams& params,
                          std::vector<float>& out_offset);

} // namespace Luminumbra::World
