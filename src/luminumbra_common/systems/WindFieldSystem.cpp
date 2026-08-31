#include "WindFieldSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <vector>

#include "../core/DeterministicMath.h"

// NOTE: call the wrappers via the fully-qualified DeterministicMath:: name (not
// a short alias) so the SimDeterminismLint trig allow-check (which looks for the
// literal "DeterministicMath::" on the line) recognises these as the sanctioned
// deterministic transcendentals rather than banned libm calls.
namespace DeterministicMath = Luminumbra::DeterministicMath;

namespace Luminumbra::Systems {

namespace {

// Tick -> base-direction noise coordinate. The base large-scale direction is a
// LOW-FREQUENCY function of tick-time: at 30 Hz one in-game minute is 1800 ticks,
// and the wind should swing over minutes, not seconds. We feed the noise a
// coordinate that advances kBaseDriftPerTick metres per tick (a smooth slow
// crawl through the noise field); the noise frequency below is the low-frequency
// pin. tick is exact in double up to 2^53 so the coordinate is bit-stable.
constexpr double kBaseDriftPerTick = 0.05; // noise-space units per tick
constexpr float kBaseNoiseFrequency = 0.015f; // low-frequency large-scale swing

// Per-cell spatial variation: a second low-frequency sample over the cell's
// world coordinates, scrolled by tick-time, rotates the base angle by up to
// +/- kCellAngleJitter radians and scales magnitude. Kept gentle so the field is
// dominated by the coherent base direction used for large-scale advection.
constexpr float kCellNoiseFrequency = 0.0035f;
constexpr float kCellAngleJitter = 0.45f;   // radians
constexpr double kCellScrollPerTick = 0.02; // world-units scroll per tick

// Per-layer base wind speed (m/s-ish, arbitrary sim units). Higher layers blow
// harder (free-atmosphere); ground is sheared down by surface friction.
constexpr float kLayerSpeed[kWindLayerCount] = {6.0f, 9.0f, 13.0f};

// fnv1a-64 over raw bytes (matches the persistence Checksum machinery so the
// wind sub-hash uses the identical stable algorithm as terrain/water).
std::uint64_t Fnv1a64(const unsigned char* bytes, std::size_t count, std::uint64_t hash) {
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

void MixFloat(std::uint64_t& hash, float value) {
    // Hash the raw IEEE bits so the sub-hash is bit-exact (not formatting-lossy).
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

WindFieldSystem::WindFieldSystem(int world_seed)
    : m_wind_seed(world_seed + 11)
    , m_grid(kWindExtentCells, kWindCellSizeM) {
    // Low-frequency FBm simplex, the same family the worldgen climate noises use,
    // so the FastNoise batch path (GenPositionArray2D / GenUniformGrid2D) and the
    // single-sample path (GenSingle2D) produce identical float bits.
    auto fractal = FastNoise::New<FastNoise::FractalFBm>();
    fractal->SetSource(FastNoise::New<FastNoise::Simplex>());
    fractal->SetOctaveCount(2);
    m_direction_noise = fractal;

    // Hoisted per-tick scratch (no allocation on the tick path).
    const std::size_t count = m_grid.cell_count();
    m_scratch_px.assign(count, 0.0f);
    m_scratch_pz.assign(count, 0.0f);
    m_scratch_cell_noise.assign(count, 0.0f);

    // Seed the grid with the base direction at tick 0 so a pre-Update sample is
    // already coherent (out-of-region clamp target).
    Update(0, Vec3(0.0f));
}

WindLayer WindFieldSystem::LayerForHeight(float world_y) noexcept {
    if (world_y < kWindGroundTopM) {
        return WindLayer::Ground;
    }
    if (world_y < kWindMidTopM) {
        return WindLayer::Mid;
    }
    return WindLayer::High;
}

void WindFieldSystem::LocalCell(const Vec3& world_pos, int& out_lx, int& out_lz, bool& in_region) const {
    // World metres -> global cell index (floor division on a positive cell size).
    const float cell = m_grid.cell_size_m();
    const std::int64_t gx = static_cast<std::int64_t>(std::floor(world_pos.x / cell));
    const std::int64_t gz = static_cast<std::int64_t>(std::floor(world_pos.z / cell));
    const std::int64_t lx = gx - m_grid.origin_cell_x();
    const std::int64_t lz = gz - m_grid.origin_cell_z();
    in_region = lx >= 0 && lz >= 0 &&
                lx < static_cast<std::int64_t>(m_grid.extent_cells()) &&
                lz < static_cast<std::int64_t>(m_grid.extent_cells());
    out_lx = static_cast<int>(lx);
    out_lz = static_cast<int>(lz);
}

void WindFieldSystem::Update(std::uint64_t tick, const Vec3& region_anchor) {
    m_last_tick = tick;

    // 1) Anchor the grid origin on the streamed region. The grid is centred on
    // the anchor's cell so it follows the player/stream centre. Pure integer.
    const float cell = m_grid.cell_size_m();
    const std::int64_t anchor_cx = static_cast<std::int64_t>(std::floor(region_anchor.x / cell));
    const std::int64_t anchor_cz = static_cast<std::int64_t>(std::floor(region_anchor.z / cell));
    const std::int64_t half = static_cast<std::int64_t>(m_grid.extent_cells()) / 2;
    m_grid.set_origin_cells(anchor_cx - half, anchor_cz - half);

    // 2) Base large-scale direction: two seed+11 low-frequency samples at a
    // slowly-advancing tick-time coordinate, via the FastNoise BATCH path so the
    // batch result is bit-identical to a GenSingle2D sample (parity, like the
    // worldgen noises). The two samples form an angle; the magnitude swings
    // gently around 1. tick is exact-in-double for the coordinate.
    const double drift = static_cast<double>(tick) * kBaseDriftPerTick;
    // Two distinct probe points in noise space (separated so x/y decorrelate).
    float base_in_x[2] = {
        static_cast<float>(drift) * kBaseNoiseFrequency,
        static_cast<float>(drift + 137.0) * kBaseNoiseFrequency};
    float base_in_y[2] = {
        static_cast<float>(0.0) * kBaseNoiseFrequency,
        static_cast<float>(53.0) * kBaseNoiseFrequency};
    float base_out[2] = {0.0f, 0.0f};
    m_direction_noise->GenPositionArray2D(base_out, 2, base_in_x, base_in_y, 0.0f, 0.0f, m_wind_seed);

    // angle in [-pi, pi] from the first sample; second sample modulates speed.
    const float base_angle = base_out[0] * DeterministicMath::kPi;
    const float speed_mod = 1.0f + 0.25f * base_out[1]; // ~[0.75, 1.25]
    m_base_direction = Vec2(DeterministicMath::Cos(base_angle), DeterministicMath::Sin(base_angle));

    // 3) Per-cell spatial blend. Sample the same noise field over the cell world
    // coordinates (scrolled by tick-time) through the BATCH grid path, then
    // rotate the base angle by a gentle per-cell jitter and scale per layer.
    const int extent = m_grid.extent_cells();
    const std::size_t count = m_grid.cell_count();
    if (count == 0) {
        return;
    }

    // Cell-centre world coordinates of the lower-left cell (origin), in metres.
    const double scroll = static_cast<double>(tick) * kCellScrollPerTick;
    const float base_world_x =
        (static_cast<float>(m_grid.origin_cell_x()) + 0.5f) * cell + static_cast<float>(scroll);
    const float base_world_z =
        (static_cast<float>(m_grid.origin_cell_z()) + 0.5f) * cell + static_cast<float>(scroll);

    // GenUniformGrid2D samples on an integer lattice; fold the cell pitch into
    // the frequency and pass integer start cells. We instead sample over the
    // cell-pitch world grid via the position-array path for an explicit,
    // parity-stable coordinate set.
    float* px = m_scratch_px.data();
    float* pz = m_scratch_pz.data();
    for (int lz = 0; lz < extent; ++lz) {
        for (int lx = 0; lx < extent; ++lx) {
            const std::size_t i = m_grid.index(lx, lz);
            px[i] = (base_world_x + static_cast<float>(lx) * cell) * kCellNoiseFrequency;
            pz[i] = (base_world_z + static_cast<float>(lz) * cell) * kCellNoiseFrequency;
        }
    }
    float* cell_noise = m_scratch_cell_noise.data();
    m_direction_noise->GenPositionArray2D(
        cell_noise, static_cast<int>(count), px, pz, 0.0f, 0.0f, m_wind_seed);

    for (int lz = 0; lz < extent; ++lz) {
        for (int lx = 0; lx < extent; ++lx) {
            const std::size_t i = m_grid.index(lx, lz);
            const float jitter = cell_noise[i] * kCellAngleJitter;
            const float angle = base_angle + jitter;
            const float cs = DeterministicMath::Cos(angle);
            const float sn = DeterministicMath::Sin(angle);
            WindCell& wcell = m_grid.cells()[i];
            for (int layer = 0; layer < kWindLayerCount; ++layer) {
                const float speed = kLayerSpeed[layer] * speed_mod;
                wcell.layer[layer] = Vec2(cs * speed, sn * speed);
            }
        }
    }

    // 4) Localized storm response. Perturbations are sorted when queued, so
    // their additive result is independent of caller iteration order. The
    // ground layer receives the strongest gust; upper layers retain more of
    // the coherent large-scale flow.
    for (const StormPerturbation& storm : m_storm_perturbations) {
        if (storm.radius_m <= 0.0f || storm.intensity <= 0.0f)
            continue;
        const float radius_sq = storm.radius_m * storm.radius_m;
        for (int lz = 0; lz < extent; ++lz) {
            for (int lx = 0; lx < extent; ++lx) {
                const std::size_t i = m_grid.index(lx, lz);
                const float world_x =
                    (static_cast<float>(m_grid.origin_cell_x() + lx) + 0.5f) * cell;
                const float world_z =
                    (static_cast<float>(m_grid.origin_cell_z() + lz) + 0.5f) * cell;
                const float dx = world_x - storm.center_world.x;
                const float dz = world_z - storm.center_world.y;
                const float dist_sq = dx * dx + dz * dz;
                if (dist_sq >= radius_sq)
                    continue;
                const float dist = DeterministicMath::Sqrt(dist_sq);
                const float falloff = 1.0f - dist / storm.radius_m;
                Vec2 swirl(0.0f);
                if (dist > 1.0e-5f) {
                    const float inv_dist = 1.0f / dist;
                    swirl = Vec2(-dz * inv_dist, dx * inv_dist);
                }
                const Vec2 local = (storm.velocity + swirl * 2.0f) * (storm.intensity * falloff);
                constexpr float kLayerResponse[kWindLayerCount] = {1.0f, 0.7f, 0.4f};
                WindCell& wcell = m_grid.cells()[i];
                for (int layer = 0; layer < kWindLayerCount; ++layer)
                    wcell.layer[layer] += local * kLayerResponse[layer];
            }
        }
    }
}

void WindFieldSystem::InjectStormPerturbation(const StormPerturbation& perturbation) {
    if (perturbation.radius_m <= 0.0f || perturbation.intensity <= 0.0f)
        return;
    m_storm_perturbations.push_back(perturbation);
    std::sort(m_storm_perturbations.begin(),
              m_storm_perturbations.end(),
              [](const StormPerturbation& a, const StormPerturbation& b) {
                  return std::tie(a.center_world.x,
                                  a.center_world.y,
                                  a.radius_m,
                                  a.velocity.x,
                                  a.velocity.y,
                                  a.intensity) < std::tie(b.center_world.x,
                                                          b.center_world.y,
                                                          b.radius_m,
                                                          b.velocity.x,
                                                          b.velocity.y,
                                                          b.intensity);
              });
}

void WindFieldSystem::ClearStormPerturbations() {
    m_storm_perturbations.clear();
}

Vec2 WindFieldSystem::SampleWind(const Vec3& world_pos, WindLayer layer) const {
    int lx = 0;
    int lz = 0;
    bool in_region = false;
    LocalCell(world_pos, lx, lz, in_region);
    if (!in_region) {
        // Out-of-region: clamp to the base large-scale direction at the layer
        // speed (the documented clamp behaviour). No magnitude modulation
        // because we have no cell to read; the coherent base is the best answer.
        const int li = static_cast<int>(layer);
        const float speed = kLayerSpeed[li];
        return Vec2(m_base_direction.x * speed, m_base_direction.y * speed);
    }
    return m_grid.at(lx, lz).layer[static_cast<int>(layer)];
}

Vec2 WindFieldSystem::SampleWind(const Vec3& world_pos) const {
    return SampleWind(world_pos, LayerForHeight(world_pos.y));
}

std::string WindFieldSystem::ComputeWindSubHash() const {
    // fnv1a-64 over: seed, tick, grid geometry, origin, then every cell's per-
    // layer vector bits in the FieldGrid canonical order. Bit-exact float
    // hashing (raw IEEE bits) so a one-ULP drift fails the gate loudly.
    std::uint64_t hash = 14695981039346656037ull; // fnv offset basis
    MixU64(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(m_wind_seed)));
    MixU64(hash, m_last_tick);
    MixU64(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(m_grid.extent_cells())));
    MixFloat(hash, m_grid.cell_size_m());
    MixU64(hash, static_cast<std::uint64_t>(m_grid.origin_cell_x()));
    MixU64(hash, static_cast<std::uint64_t>(m_grid.origin_cell_z()));

    const int extent = m_grid.extent_cells();
    for (int lz = 0; lz < extent; ++lz) {
        for (int lx = 0; lx < extent; ++lx) {
            const WindCell& wcell = m_grid.at(lx, lz);
            for (int layer = 0; layer < kWindLayerCount; ++layer) {
                MixFloat(hash, wcell.layer[layer].x);
                MixFloat(hash, wcell.layer[layer].y);
            }
        }
    }

    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

} // namespace Luminumbra::Systems
