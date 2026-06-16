#include "HydraulicErosion.h"

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace Luminumbra::World {

// Deterministic thermal + hydraulic erosion. Each sweep propagates information
// at most ONE cell (von-Neumann neighbours), so the interior is halo-independent
// when halo >= iterations (see header). Pure IEEE +,-,*,/ ; no libm, no RNG.
void BakeHydraulicErosion(const std::vector<float>& heights,
                          int n,
                          int halo,
                          const HydroErosionParams& p,
                          std::vector<float>& out_offset) {
    assert(halo >= p.iterations && "halo must be >= iterations for halo-independence");
    const int m = n + 2 * halo;
    assert(static_cast<std::size_t>(m) * static_cast<std::size_t>(m) == heights.size());

    const std::size_t cells = static_cast<std::size_t>(m) * static_cast<std::size_t>(m);
    std::vector<float> h = heights;                 // working (eroded) height, padded
    std::vector<float> water(cells, 0.0f);
    std::vector<float> sediment(cells, 0.0f);
    std::vector<float> surf(cells, 0.0f);
    std::vector<float> water_next(cells, 0.0f);
    std::vector<float> sed_next(cells, 0.0f);
    std::vector<float> dh(cells, 0.0f);

    auto idx = [m](int x, int z) -> std::size_t {
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(m) + static_cast<std::size_t>(x);
    };
    const int dx[4] = {-1, 1, 0, 0};
    const int dz[4] = {0, 0, -1, 1};

    for (int it = 0; it < p.iterations; ++it) {
        // --- THERMAL (talus) relaxation: mass-conserving downhill transport ---
        std::fill(dh.begin(), dh.end(), 0.0f);
        for (int z = 1; z < m - 1; ++z) {
            for (int x = 1; x < m - 1; ++x) {
                const std::size_t c = idx(x, z);
                for (int k = 0; k < 4; ++k) {
                    const std::size_t nb = idx(x + dx[k], z + dz[k]);
                    const float d = h[c] - h[nb];
                    if (d > p.talus_height) {
                        const float move = p.thermal_rate * (d - p.talus_height) * 0.25f;
                        dh[c] -= move;
                        dh[nb] += move;
                    }
                }
            }
        }
        for (std::size_t i = 0; i < cells; ++i) {
            h[i] += dh[i];
        }

        // --- HYDRAULIC: rain -> single-step downhill flow w/ erode + deposit ---
        for (std::size_t i = 0; i < cells; ++i) {
            water[i] += p.rain_per_sweep;
            surf[i] = h[i] + water[i];
        }
        std::fill(water_next.begin(), water_next.end(), 0.0f);
        std::fill(sed_next.begin(), sed_next.end(), 0.0f);
        std::fill(dh.begin(), dh.end(), 0.0f);
        for (int z = 1; z < m - 1; ++z) {
            for (int x = 1; x < m - 1; ++x) {
                const std::size_t c = idx(x, z);
                const float w = water[c];
                // Lowest neighbour by surface (fixed tie-break = neighbour order).
                int best = -1;
                float best_surf = surf[c];
                for (int k = 0; k < 4; ++k) {
                    const std::size_t nb = idx(x + dx[k], z + dz[k]);
                    if (surf[nb] < best_surf) {
                        best_surf = surf[nb];
                        best = k;
                    }
                }
                if (best < 0 || w <= 0.0f) {
                    // Pit or dry cell: drop all sediment here, keep the water.
                    dh[c] += sediment[c];
                    water_next[c] += w;
                    continue;
                }
                const std::size_t nb = idx(x + dx[best], z + dz[best]);
                const float diff = surf[c] - surf[nb]; // > 0 by construction
                const float flow = (diff * 0.5f < w) ? (diff * 0.5f) : w; // stable, <= w
                const float capacity = p.sediment_capacity * diff * flow;
                float carried = sediment[c];
                if (carried < capacity) {
                    const float erode = p.solubility * (capacity - carried);
                    dh[c] -= erode;   // incise the source
                    carried += erode;
                } else {
                    const float drop = p.deposition * (carried - capacity);
                    dh[c] += drop;    // deposit at the source
                    carried -= drop;
                }
                const float frac = flow / w; // (0, 1]
                water_next[nb] += flow;
                water_next[c] += (w - flow);
                sed_next[nb] += carried * frac;
                sed_next[c] += carried * (1.0f - frac);
            }
        }
        for (std::size_t i = 0; i < cells; ++i) {
            h[i] += dh[i];
            water[i] = water_next[i] * (1.0f - p.evaporation); // linear evaporation
            sediment[i] = sed_next[i];
        }
    }

    // Crop the interior offset = eroded - input, clamped to +/- max_offset.
    out_offset.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n), 0.0f);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const std::size_t src = idx(x + halo, z + halo);
            float off = h[src] - heights[src];
            if (off > p.max_offset) off = p.max_offset;
            if (off < -p.max_offset) off = -p.max_offset;
            out_offset[static_cast<std::size_t>(z) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x)] = off;
        }
    }
}

} // namespace Luminumbra::World
