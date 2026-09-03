#include "AetherFieldSystem.h"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

#include "../core/DeterministicMath.h"
#include "WindFieldSystem.h"

// Call DeterministicMath via the fully-qualified name so SimDeterminismLint's
// trig allow-check recognizes the sanctioned wrappers. (This TU uses no trig,
// but BitsOf comes from the same header.)
namespace DeterministicMath = Luminumbra::DeterministicMath;

namespace Luminumbra::Systems {

namespace {

// fnv1a-64 over raw bytes (identical algorithm to WindFieldSystem / persistence).
std::uint64_t Fnv1a64(const unsigned char* bytes, std::size_t count, std::uint64_t hash) {
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

void MixFloat(std::uint64_t& hash, float value) {
    // Hash raw IEEE bits so a one-ULP drift fails the gate loudly.
    std::uint32_t bits = DeterministicMath::BitsOf(value);
    unsigned char b[4];
    b[0] = static_cast<unsigned char>(bits & 0xffu);
    b[1] = static_cast<unsigned char>((bits >> 8) & 0xffu);
    b[2] = static_cast<unsigned char>((bits >> 16) & 0xffu);
    b[3] = static_cast<unsigned char>((bits >> 24) & 0xffu);
    hash = Fnv1a64(b, 4, hash);
}

void MixU64(std::uint64_t& hash, std::uint64_t value) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<unsigned char>((value >> (i * 8)) & 0xffu);
    }
    hash = Fnv1a64(b, 8, hash);
}

} // namespace

AetherFieldSystem::AetherFieldSystem(int world_seed)
    : m_aether_seed(world_seed + 14)
    , m_grid(kAetherExtentCells, kAetherCellSizeM) {
    // Low-frequency FBm simplex (same family as the worldgen/wind noises) so the
    // FastNoise batch path produces bit-stable floats.
    auto fractal = FastNoise::New<FastNoise::FractalFBm>();
    fractal->SetSource(FastNoise::New<FastNoise::Simplex>());
    fractal->SetOctaveCount(2);
    m_emission_noise = fractal;

    const std::size_t count = m_grid.cell_count();
    m_scratch_px.assign(count, 0.0f);
    m_scratch_pz.assign(count, 0.0f);
    m_scratch_emission.assign(count, 0.0f);
    m_scratch_advected.assign(count, 0.0f);

    // Seed the grid at tick 0 so a pre-Update sample is already coherent.
    Update(0, Vec3(0.0f), nullptr);
}

void AetherFieldSystem::LocalCell(const Vec3& world_pos,
                                  int& out_lx,
                                  int& out_lz,
                                  bool& in_region) const {
    const float cell = m_grid.cell_size_m();
    const std::int64_t gx = static_cast<std::int64_t>(std::floor(world_pos.x / cell));
    const std::int64_t gz = static_cast<std::int64_t>(std::floor(world_pos.z / cell));
    const std::int64_t lx = gx - m_grid.origin_cell_x();
    const std::int64_t lz = gz - m_grid.origin_cell_z();
    in_region = lx >= 0 && lz >= 0 && lx < static_cast<std::int64_t>(m_grid.extent_cells()) &&
                lz < static_cast<std::int64_t>(m_grid.extent_cells());
    out_lx = static_cast<int>(lx);
    out_lz = static_cast<int>(lz);
}

void AetherFieldSystem::Update(std::uint64_t tick,
                               const Vec3& region_anchor,
                               const WindFieldSystem* wind) {
    m_last_tick = tick;

    // 1) Anchor the grid origin on the streamed region (pure integer).
    const float cell = m_grid.cell_size_m();
    const std::int64_t anchor_cx = static_cast<std::int64_t>(std::floor(region_anchor.x / cell));
    const std::int64_t anchor_cz = static_cast<std::int64_t>(std::floor(region_anchor.z / cell));
    const std::int64_t half = static_cast<std::int64_t>(m_grid.extent_cells()) / 2;
    m_grid.set_origin_cells(anchor_cx - half, anchor_cz - half);

    const int extent = m_grid.extent_cells();
    const std::size_t count = m_grid.cell_count();
    if (count == 0) {
        return;
    }

    // 2) Emission source: a seed+14 low-frequency noise over the cell world
    //    coords, scrolled slowly by tick-time, mapped to a non-negative scalar.
    const double scroll = static_cast<double>(tick) * kAetherEmissionScrollPerTick;
    const float base_world_x =
        (static_cast<float>(m_grid.origin_cell_x()) + 0.5f) * cell + static_cast<float>(scroll);
    const float base_world_z =
        (static_cast<float>(m_grid.origin_cell_z()) + 0.5f) * cell + static_cast<float>(scroll);
    float* px = m_scratch_px.data();
    float* pz = m_scratch_pz.data();
    for (int lz = 0; lz < extent; ++lz) {
        for (int lx = 0; lx < extent; ++lx) {
            const std::size_t i = m_grid.index(lx, lz);
            px[i] = (base_world_x + static_cast<float>(lx) * cell) * kAetherEmissionFrequency;
            pz[i] = (base_world_z + static_cast<float>(lz) * cell) * kAetherEmissionFrequency;
        }
    }
    float* emission = m_scratch_emission.data();
    m_emission_noise->GenPositionArray2D(
        emission, static_cast<int>(count), px, pz, 0.0f, 0.0f, m_aether_seed);
    // Map noise [-1,1] -> [0,1] (non-negative emission). Only +,*; no libm.
    for (std::size_t i = 0; i < count; ++i) {
        emission[i] = 0.5f * (emission[i] + 1.0f);
    }

    // 3) Semi-Lagrangian advection of the emission source by the wind field.
    //    Backtrace each cell centre by wind*step, bilinear-sample the emission
    //    grid (edge-clamped). No wind -> identity copy. Deterministic (+,-,*,/).
    float* advected = m_scratch_advected.data();
    if (wind == nullptr) {
        for (std::size_t i = 0; i < count; ++i) {
            advected[i] = emission[i];
        }
    } else {
        for (int lz = 0; lz < extent; ++lz) {
            for (int lx = 0; lx < extent; ++lx) {
                const std::size_t i = m_grid.index(lx, lz);
                const float cx = (static_cast<float>(m_grid.origin_cell_x() + lx) + 0.5f) * cell;
                const float cz = (static_cast<float>(m_grid.origin_cell_z() + lz) + 0.5f) * cell;
                const Vec2 vel = wind->SampleWind(Vec3(cx, 5.0f, cz)); // ground layer
                const float sx = cx - vel.x * kAetherAdvectionMetersPerTick;
                const float sz = cz - vel.y * kAetherAdvectionMetersPerTick;
                // World -> fractional local cell (cell centres at integer+0.5).
                const float flx = sx / cell - static_cast<float>(m_grid.origin_cell_x()) - 0.5f;
                const float flz = sz / cell - static_cast<float>(m_grid.origin_cell_z()) - 0.5f;
                int l0 = static_cast<int>(std::floor(flx));
                int m0 = static_cast<int>(std::floor(flz));
                const float tx = flx - static_cast<float>(l0);
                const float tz = flz - static_cast<float>(m0);
                int l1 = l0 + 1;
                int m1 = m0 + 1;
                // Edge-clamp the four taps into [0, extent-1].
                l0 = (l0 < 0) ? 0 : ((l0 > extent - 1) ? extent - 1 : l0);
                l1 = (l1 < 0) ? 0 : ((l1 > extent - 1) ? extent - 1 : l1);
                m0 = (m0 < 0) ? 0 : ((m0 > extent - 1) ? extent - 1 : m0);
                m1 = (m1 < 0) ? 0 : ((m1 > extent - 1) ? extent - 1 : m1);
                const float e00 = emission[m_grid.index(l0, m0)];
                const float e10 = emission[m_grid.index(l1, m0)];
                const float e01 = emission[m_grid.index(l0, m1)];
                const float e11 = emission[m_grid.index(l1, m1)];
                const float e0 = e00 + (e10 - e00) * tx;
                const float e1 = e01 + (e11 - e01) * tx;
                advected[i] = e0 + (e1 - e0) * tz;
            }
        }
    }

    // 4) Diffuse: PINNED Gauss-Seidel sweeps (implicit, Neumann boundary). In
    //    place on the grid; non-negative in -> non-negative out. Deterministic.
    std::vector<float>& field = m_grid.cells();
    for (std::size_t i = 0; i < count; ++i) {
        field[i] = advected[i];
    }
    const float k = kAetherDiffuseRate;
    for (int iter = 0; iter < kAetherDiffuseIterations; ++iter) {
        for (int lz = 0; lz < extent; ++lz) {
            for (int lx = 0; lx < extent; ++lx) {
                const std::size_t i = m_grid.index(lx, lz);
                float nsum = 0.0f;
                int ncount = 0;
                if (lx > 0) {
                    nsum += field[m_grid.index(lx - 1, lz)];
                    ++ncount;
                }
                if (lx < extent - 1) {
                    nsum += field[m_grid.index(lx + 1, lz)];
                    ++ncount;
                }
                if (lz > 0) {
                    nsum += field[m_grid.index(lx, lz - 1)];
                    ++ncount;
                }
                if (lz < extent - 1) {
                    nsum += field[m_grid.index(lx, lz + 1)];
                    ++ncount;
                }
                field[i] = (advected[i] + k * nsum) / (1.0f + k * static_cast<float>(ncount));
            }
        }
    }
}

float AetherFieldSystem::SampleAether(const Vec3& world_pos) const {
    int lx = 0;
    int lz = 0;
    bool in_region = false;
    LocalCell(world_pos, lx, lz, in_region);
    if (!in_region) {
        return 0.0f; // no aether beyond the streamed region
    }
    return m_grid.at(lx, lz);
}

std::string AetherFieldSystem::ComputeAetherSubHash() const {
    std::uint64_t hash = 14695981039346656037ull; // fnv offset basis
    MixU64(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(m_aether_seed)));
    MixU64(hash, m_last_tick);
    MixU64(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(m_grid.extent_cells())));
    MixFloat(hash, m_grid.cell_size_m());
    MixU64(hash, static_cast<std::uint64_t>(m_grid.origin_cell_x()));
    MixU64(hash, static_cast<std::uint64_t>(m_grid.origin_cell_z()));

    const int extent = m_grid.extent_cells();
    for (int lz = 0; lz < extent; ++lz) {
        for (int lx = 0; lx < extent; ++lx) {
            MixFloat(hash, m_grid.at(lx, lz));
        }
    }

    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

} // namespace Luminumbra::Systems
