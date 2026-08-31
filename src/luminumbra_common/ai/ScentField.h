#pragma once

// scent / pheromone STIGMERGY field — the substrate for emergent
// tracking, hunting, and trail behaviors (ants laying food/home trails; a
// predator following prey scent up-gradient; prey fleeing down a predator-scent
// gradient). Indirect coordination through a shared, decaying environment field
// is "stigmergy" (Grasse 1959; Dorigo's Ant Colony Optimization; Deneubourg's
// double-bridge ant-trail experiments).
//
// Built ON TOP of the engine's existing conservative diffusion solver
// (fields::ScalarFieldDiffusion) — one channel per scent species — and adds the
// two things a *scent* field needs that pure diffusion does not:
//   * EVAPORATION: scent is non-conservative; each step every cell decays toward
//     zero so stale trails fade (this is what makes ant trails self-optimize and
//     what bounds a predator's tracking window).
//   * GRADIENT SENSING: agents follow the up-gradient (toward the source) or the
//     down-gradient (away), the read side that turns a deposited trail into a
//     steering signal (compose with InstinctLocomotion's wish_xz).
//
// DETERMINISM (sim contract): the diffusion solver is deterministic (double,
// fixed traversal order); evaporation is a uniform multiply; the gradient is a
// central difference. No RNG / wall-clock / libm transcendentals. The field is a
// standalone subsystem — until a deposit/sense system wires it into the canonical
// tick it does not feed world_hash (and that wiring is data-flagged, like the
// flocking/flee/path behaviors).

#include <cmath>
#include <cstddef>
#include <vector>

#include "../fields/ScalarFieldDiffusion.h"

namespace luminumbra::ai {

class ScentField {
public:
    // `channels` independent scent species (e.g. 0 = prey, 1 = predator, 2 =
    // food-trail, 3 = home-trail). All share the grid dimensions.
    ScentField(int width, int height, int channels = 1)
        : m_w(width > 0 ? width : 0)
        , m_h(height > 0 ? height : 0) {
        const int n = channels > 0 ? channels : 1;
        m_ch.reserve(static_cast<std::size_t>(n));
        for (int c = 0; c < n; ++c) {
            m_ch.emplace_back(static_cast<std::size_t>(m_w), static_cast<std::size_t>(m_h), 0.0);
        }
    }

    [[nodiscard]] int width() const {
        return m_w;
    }
    [[nodiscard]] int height() const {
        return m_h;
    }
    [[nodiscard]] int channels() const {
        return static_cast<int>(m_ch.size());
    }

    [[nodiscard]] bool in_bounds(int x, int z) const {
        return x >= 0 && z >= 0 && x < m_w && z < m_h;
    }

    // Lay scent: an agent deposits `amount` of species `ch` at its cell (trail-
    // laying / scent-marking). Out-of-bounds or unknown channel is a no-op.
    void Deposit(int ch, int x, int z, double amount) {
        if (!valid(ch, x, z) || amount == 0.0)
            return;
        m_ch[static_cast<std::size_t>(ch)].add_impulse(
            static_cast<std::size_t>(x), static_cast<std::size_t>(z), amount);
    }

    // Advance every channel one step: (optionally) ADVECT downwind, then diffuse
    // (spread), then evaporate (decay toward zero by `evaporation` in [0,1]).
    // `diffusion_iters`/`diffusion_rate` pass through to the conservative solver.
    // Evaporation is applied AFTER diffusion so a fresh deposit both spreads and
    // begins to fade.
    //
    // wind-advection: `wind_cx`/`wind_cz` are the wind vector in
    // CELLS PER STEP (the caller converts world-units/sec via dt and cell size).
    // The pre-pass is a semi-Lagrangian backtrace — each cell samples UPWIND
    // (x - wind, z - wind) with bilinear interpolation, so the whole field drifts
    // in the +wind direction (downwind), letting a predator track prey UP-wind.
    // Default (0,0) wind SKIPS advection entirely, so the step is byte-identical to
    // the diffuse+evaporate-only result (canonical roster unchanged). Deterministic:
    // double math, fixed traversal order, edge-clamped sampling — no RNG/wall-clock.
    void Step(double diffusion_rate,
              std::size_t diffusion_iters,
              double evaporation,
              double wind_cx = 0.0,
              double wind_cz = 0.0) {
        const double keep =
            1.0 - (evaporation < 0.0 ? 0.0 : (evaporation > 1.0 ? 1.0 : evaporation));
        const bool did_diffuse = diffusion_iters > 0 && diffusion_rate > 0.0;
        const bool advect = (wind_cx != 0.0 || wind_cz != 0.0) && m_w > 0 && m_h > 0;
        const std::size_t W = static_cast<std::size_t>(m_w);
        const std::size_t H = static_cast<std::size_t>(m_h);
        for (auto& field : m_ch) {
            if (advect) {
                // Snapshot the channel, then rewrite each cell from its upwind source.
                std::vector<double> src(W * H);
                for (std::size_t y = 0; y < H; ++y)
                    for (std::size_t x = 0; x < W; ++x)
                        src[y * W + x] = field.at(x, y);
                for (std::size_t y = 0; y < H; ++y) {
                    for (std::size_t x = 0; x < W; ++x) {
                        field.set(x,
                                  y,
                                  BilinearClamped(src,
                                                  static_cast<double>(x) - wind_cx,
                                                  static_cast<double>(y) - wind_cz));
                    }
                }
            }
            if (did_diffuse) {
                (void)field.diffuse(diffusion_iters, diffusion_rate);
                // The conservative 4-neighbour solver leaves a parity (checkerboard)
                // artifact — after even iterations, odd-Manhattan-distance cells are
                // exactly 0 — which makes a central-difference gradient read 0 at
                // those cells. One deterministic 5-point smoothing pass (center 0.5,
                // each in-bounds neighbour 0.125, edge-normalized) fills the parity
                // gaps so the scent gradient is usable from any cell. Evaporation is
                // folded into the same write. Only runs when diffusing, so a pure
                // evaporation step (iters 0) stays an exact per-cell decay.
                std::vector<double> tmp(W * H);
                for (std::size_t y = 0; y < H; ++y)
                    for (std::size_t x = 0; x < W; ++x)
                        tmp[y * W + x] = field.at(x, y);
                for (std::size_t y = 0; y < H; ++y) {
                    for (std::size_t x = 0; x < W; ++x) {
                        double acc = 0.5 * tmp[y * W + x];
                        double wsum = 0.5;
                        if (x > 0) {
                            acc += 0.125 * tmp[y * W + (x - 1)];
                            wsum += 0.125;
                        }
                        if (x + 1 < W) {
                            acc += 0.125 * tmp[y * W + (x + 1)];
                            wsum += 0.125;
                        }
                        if (y > 0) {
                            acc += 0.125 * tmp[(y - 1) * W + x];
                            wsum += 0.125;
                        }
                        if (y + 1 < H) {
                            acc += 0.125 * tmp[(y + 1) * W + x];
                            wsum += 0.125;
                        }
                        field.set(x, y, (acc / wsum) * keep);
                    }
                }
            } else if (keep < 1.0) {
                for (std::size_t y = 0; y < H; ++y)
                    for (std::size_t x = 0; x < W; ++x)
                        field.set(x, y, field.at(x, y) * keep);
            }
        }
    }

    // MAX-MIN Ant System anti-stagnation: clamp every cell of every channel to
    // [tau_min, tau_max] (scent-field contract). A positive tau_min keeps every cell's
    // follow-probability strictly above zero so agents never lock permanently onto
    // one trail (prevents premature convergence); tau_max caps unbounded deposit
    // buildup. Apply AFTER Step. Deterministic. Default args = no-op.
    void Clamp(double tau_min = 0.0, double tau_max = 1.0e300) {
        for (auto& field : m_ch) {
            for (std::size_t y = 0; y < static_cast<std::size_t>(m_h); ++y) {
                for (std::size_t x = 0; x < static_cast<std::size_t>(m_w); ++x) {
                    double v = field.at(x, y);
                    if (v < tau_min)
                        v = tau_min;
                    if (v > tau_max)
                        v = tau_max;
                    field.set(x, y, v);
                }
            }
        }
    }

    // Scent concentration of species `ch` at a cell (0 outside the grid).
    [[nodiscard]] double Sample(int ch, int x, int z) const {
        if (!valid(ch, x, z))
            return 0.0;
        return m_ch[static_cast<std::size_t>(ch)].at(static_cast<std::size_t>(x),
                                                     static_cast<std::size_t>(z));
    }

    // Central-difference gradient of species `ch` at (x,z). Writes (gx,gz) that
    // point UP-gradient — toward stronger scent (the source). A hunter steers
    // along +(gx,gz) to track prey; a prey flees along -(gx,gz). Returns the
    // gradient magnitude (0 when flat / out of bounds), so callers can gate on a
    // minimum scent strength before committing to a heading.
    double Gradient(int ch, int x, int z, float& gx, float& gz) const {
        gx = 0.0f;
        gz = 0.0f;
        if (!valid(ch, x, z))
            return 0.0;
        const auto& f = m_ch[static_cast<std::size_t>(ch)];
        auto sample = [&](int sx, int sz) -> double {
            const int cx = sx < 0 ? 0 : (sx >= m_w ? m_w - 1 : sx); // clamp at edges
            const int cz = sz < 0 ? 0 : (sz >= m_h ? m_h - 1 : sz);
            return f.at(static_cast<std::size_t>(cx), static_cast<std::size_t>(cz));
        };
        const double dgx = sample(x + 1, z) - sample(x - 1, z);
        const double dgz = sample(x, z + 1) - sample(x, z - 1);
        gx = static_cast<float>(dgx);
        gz = static_cast<float>(dgz);
        return std::sqrt(static_cast<double>(gx) * gx + static_cast<double>(gz) * gz);
    }

    // chemotaxis STEERING off the scent gradient — the hunting/tracking
    // read side (research: Weber-law normalized gradient following). Writes a UNIT
    // direction (out_dx,out_dz) to steer TOWARD (sign=+1, a predator tracking prey)
    // or AWAY FROM (sign=-1, prey fleeing) the scent source at (x,z), and returns a
    // Weber-normalized CONFIDENCE m/(m+k) in [0,1) (k = the half-confidence
    // gradient magnitude — robust to absolute concentration). Returns 0 with a zero
    // direction when the gradient magnitude is below `floor` — the tracking-window
    // cutoff (a cold/evaporated trail yields no commitment). Deterministic.
    float GradientSteer(int ch,
                        int x,
                        int z,
                        float sign,
                        float floor,
                        float k,
                        float& out_dx,
                        float& out_dz) const {
        out_dx = 0.0f;
        out_dz = 0.0f;
        float gx = 0.0f;
        float gz = 0.0f;
        const double mag = Gradient(ch, x, z, gx, gz);
        if (mag <= 0.0 || static_cast<float>(mag) < floor)
            return 0.0f;
        const float inv = 1.0f / static_cast<float>(mag); // |(gx,gz)| == mag
        out_dx = sign * gx * inv;
        out_dz = sign * gz * inv;
        const float m = static_cast<float>(mag);
        const float kk = k > 0.0f ? k : 1.0f;
        return m / (m + kk); // Weber-normalized confidence in [0,1)
    }

private:
    [[nodiscard]] bool valid(int ch, int x, int z) const {
        return ch >= 0 && ch < static_cast<int>(m_ch.size()) && in_bounds(x, z);
    }

    // Bilinear sample of a row-major W*H buffer at (fx,fz), edge-clamped. Used by the
    // semi-Lagrangian advection backtrace. After clamping, fx/fz are >= 0 so the int
    // cast == floor (no libm floor needed). Deterministic double arithmetic.
    [[nodiscard]] double BilinearClamped(const std::vector<double>& s, double fx, double fz) const {
        const double maxx = static_cast<double>(m_w - 1);
        const double maxz = static_cast<double>(m_h - 1);
        if (fx < 0.0)
            fx = 0.0;
        else if (fx > maxx)
            fx = maxx;
        if (fz < 0.0)
            fz = 0.0;
        else if (fz > maxz)
            fz = maxz;
        const int x0 = static_cast<int>(fx);
        const int z0 = static_cast<int>(fz);
        const int x1 = x0 + 1 < m_w ? x0 + 1 : x0;
        const int z1 = z0 + 1 < m_h ? z0 + 1 : z0;
        const double tx = fx - static_cast<double>(x0);
        const double tz = fz - static_cast<double>(z0);
        const std::size_t W = static_cast<std::size_t>(m_w);
        const double v00 = s[static_cast<std::size_t>(z0) * W + static_cast<std::size_t>(x0)];
        const double v10 = s[static_cast<std::size_t>(z0) * W + static_cast<std::size_t>(x1)];
        const double v01 = s[static_cast<std::size_t>(z1) * W + static_cast<std::size_t>(x0)];
        const double v11 = s[static_cast<std::size_t>(z1) * W + static_cast<std::size_t>(x1)];
        const double a = v00 + (v10 - v00) * tx;
        const double b = v01 + (v11 - v01) * tx;
        return a + (b - a) * tz;
    }
    int m_w;
    int m_h;
    std::vector<luminumbra::fields::ScalarFieldDiffusion> m_ch;
};

} // namespace luminumbra::ai
