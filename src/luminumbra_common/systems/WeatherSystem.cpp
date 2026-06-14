#include "WeatherSystem.h"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

#include "WindFieldSystem.h"
#include "../core/DeterministicMath.h"

// NOTE: call the wrappers via the fully-qualified DeterministicMath:: name (not
// a short alias) so the SimDeterminismLint trig allow-check (which looks for the
// literal "DeterministicMath::" on the line) recognises these as the sanctioned
// deterministic transcendentals rather than banned libm calls.
namespace DeterministicMath = Luminumbra::DeterministicMath;

namespace Luminumbra::Systems {

namespace {

// --- Tunables (PINNED) -----------------------------------------------------
// The base weather pressure is a LOW-FREQUENCY function of tick-time, drifting
// over minutes (1800 ticks/in-game-minute at 30 Hz) so the sky state evolves
// slowly, not every second. Same crawl-through-noise scheme the wind base uses.
constexpr double kPressureDriftPerTick = 0.04; // noise-space units per tick
constexpr float kPressureNoiseFrequency = 0.012f; // low-frequency large-scale swing
// The per-cell spatial weather sample (the category map) is sampled over the
// cell world coordinates, scrolled slowly by tick-time.
constexpr float kClimateNoiseFrequency = 0.0025f;
constexpr double kClimateScrollPerTick = 0.015; // world-units scroll per tick

// Storm-cell schedule. A new storm-cell SPAWN OPPORTUNITY occurs on a fixed
// tick epoch; whether it actually spawns is a deterministic seeded coin keyed on
// (seed+12, epoch). Lifetime + intensity envelope are bounded so the cell set is
// always <= kMaxStormCells and the precip field memory is flat.
constexpr std::uint64_t kStormSpawnEpochTicks = 45; // a spawn opportunity every 45 ticks
constexpr std::uint32_t kStormLifetimeTicks = 240;  // 8 s at 30 Hz
constexpr std::uint32_t kStormRampTicks = 45;       // ramp-in / decay-out window
constexpr float kStormRadiusM = 220.0f;             // influence radius (world metres)
constexpr float kStormPeakIntensity = 1.0f;

// Category classification thresholds over the normalized pressure/climate. The
// weather pressure dominates (clear at high pressure, precip at low); the
// temperature decides rain-vs-snow; humidity nudges overcast/fog.
constexpr float kClearPressure = 0.18f;     // pressure above this => clear/overcast
constexpr float kOvercastPressure = -0.02f; // between this and kClear => overcast/fog
constexpr float kSnowTemperature = -0.30f;  // temperature below this => snow not rain

// fnv1a-64 over raw bytes (matches the wind/persistence Checksum machinery so the
// weather sub-hash uses the identical stable algorithm).
std::uint64_t Fnv1a64(const unsigned char* bytes, std::size_t count, std::uint64_t hash) {
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

void MixFloat(std::uint64_t& hash, float value) {
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

void MixU8(std::uint64_t& hash, std::uint8_t value) {
    hash = Fnv1a64(&value, 1, hash);
}

// Deterministic integer hash (splitmix64-style finalizer) over a 64-bit key.
// Pure integer arithmetic (no FP, no libm, no RNG) -> a seeded "coin" / float in
// [0, 1) for the storm schedule that is bit-stable on every machine.
std::uint64_t Mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Deterministic float in [0, 1) from a 64-bit key (top 24 bits / 2^24).
float UnitFloat(std::uint64_t key) {
    const std::uint32_t mantissa = static_cast<std::uint32_t>(Mix64(key) >> 40); // 24 bits
    return static_cast<float>(mantissa) * (1.0f / 16777216.0f);
}

} // namespace

const char* WeatherCategoryName(WeatherCategory category) noexcept {
    switch (category) {
        case WeatherCategory::Clear: return "clear";
        case WeatherCategory::Overcast: return "overcast";
        case WeatherCategory::Rain: return "rain";
        case WeatherCategory::Snow: return "snow";
        case WeatherCategory::Fog: return "fog";
    }
    return "clear";
}

WeatherSystem::WeatherSystem(int world_seed)
    : m_world_seed(world_seed),
      m_weather_seed(world_seed + 12), // seed-offset registry: +12 weather (FIRST)
      m_precip(kWeatherExtentCells, kWeatherCellSizeM),
      m_category(kWeatherExtentCells, kWeatherCellSizeM),
      m_grid_extent(kWeatherExtentCells),
      m_grid_cell_size(kWeatherCellSizeM) {
    // Low-frequency FBm Simplex (same family the wind base + worldgen climate
    // noises use) so the FastNoise batch path matches the single-sample path.
    auto pressure = FastNoise::New<FastNoise::FractalFBm>();
    pressure->SetSource(FastNoise::New<FastNoise::Simplex>());
    pressure->SetOctaveCount(2);
    m_pressure_noise = pressure;

    auto temperature = FastNoise::New<FastNoise::FractalFBm>();
    temperature->SetSource(FastNoise::New<FastNoise::Simplex>());
    temperature->SetOctaveCount(2);
    m_temperature_noise = temperature;

    auto humidity = FastNoise::New<FastNoise::FractalFBm>();
    humidity->SetSource(FastNoise::New<FastNoise::Simplex>());
    humidity->SetOctaveCount(2);
    m_humidity_noise = humidity;

    const std::size_t count = m_precip.cell_count();
    m_scratch_px.assign(count, 0.0f);
    m_scratch_pz.assign(count, 0.0f);
    m_scratch_pressure.assign(count, 0.0f);
    m_scratch_temperature.assign(count, 0.0f);
    m_scratch_humidity.assign(count, 0.0f);

    // Seed the fields at tick 0 so a pre-Update sample is coherent.
    Update(0, Vec3(0.0f), nullptr);
}

void WeatherSystem::LocalCell(const Vec3& world_pos, int& out_lx, int& out_lz, bool& in_region) const {
    const float cell = m_precip.cell_size_m();
    const std::int64_t gx = static_cast<std::int64_t>(std::floor(world_pos.x / cell));
    const std::int64_t gz = static_cast<std::int64_t>(std::floor(world_pos.z / cell));
    const std::int64_t lx = gx - m_precip.origin_cell_x();
    const std::int64_t lz = gz - m_precip.origin_cell_z();
    in_region = lx >= 0 && lz >= 0 &&
                lx < static_cast<std::int64_t>(m_precip.extent_cells()) &&
                lz < static_cast<std::int64_t>(m_precip.extent_cells());
    out_lx = static_cast<int>(lx);
    out_lz = static_cast<int>(lz);
}

WeatherCategory WeatherSystem::Classify(float pressure, float temperature, float humidity) noexcept {
    // High pressure: clear, unless very humid (-> overcast haze).
    if (pressure >= kClearPressure) {
        return humidity > 0.55f ? WeatherCategory::Overcast : WeatherCategory::Clear;
    }
    // Mid pressure: overcast, or fog when cold+humid+calm.
    if (pressure >= kOvercastPressure) {
        if (humidity > 0.35f && temperature < -0.10f) {
            return WeatherCategory::Fog;
        }
        return WeatherCategory::Overcast;
    }
    // Low pressure: precipitation. Cold -> snow, else rain.
    return temperature < kSnowTemperature ? WeatherCategory::Snow : WeatherCategory::Rain;
}

void WeatherSystem::RebuildFields(std::uint64_t tick, const Vec3& region_anchor) {
    // Anchor the grids on the streamed region (pure integer; same as wind).
    const float cell = m_precip.cell_size_m();
    const std::int64_t anchor_cx = static_cast<std::int64_t>(std::floor(region_anchor.x / cell));
    const std::int64_t anchor_cz = static_cast<std::int64_t>(std::floor(region_anchor.z / cell));
    const std::int64_t half = static_cast<std::int64_t>(m_precip.extent_cells()) / 2;
    m_precip.set_origin_cells(anchor_cx - half, anchor_cz - half);
    m_category.set_origin_cells(anchor_cx - half, anchor_cz - half);

    const int extent = m_precip.extent_cells();
    const std::size_t count = m_precip.cell_count();
    if (count == 0) {
        return;
    }

    // Sample coordinates: a slow tick-time scroll plus the cell world position,
    // through the FastNoise BATCH path (parity with GenSingle2D). Pressure uses
    // its own tick-drift coordinate so the whole region breathes together; the
    // climate channels add per-cell spatial structure.
    const double drift = static_cast<double>(tick) * kPressureDriftPerTick;
    const double scroll = static_cast<double>(tick) * kClimateScrollPerTick;
    const float base_world_x =
        (static_cast<float>(m_precip.origin_cell_x()) + 0.5f) * cell + static_cast<float>(scroll);
    const float base_world_z =
        (static_cast<float>(m_precip.origin_cell_z()) + 0.5f) * cell + static_cast<float>(scroll);

    float* px = m_scratch_px.data();
    float* pz = m_scratch_pz.data();
    for (int lz = 0; lz < extent; ++lz) {
        for (int lx = 0; lx < extent; ++lx) {
            const std::size_t i = m_precip.index(lx, lz);
            // Fold the slow pressure drift into the X coordinate so the whole
            // region's pressure evolves over time while keeping spatial variety.
            px[i] = (base_world_x + static_cast<float>(lx) * cell) * kClimateNoiseFrequency
                    + static_cast<float>(drift) * kPressureNoiseFrequency;
            pz[i] = (base_world_z + static_cast<float>(lz) * cell) * kClimateNoiseFrequency;
        }
    }

    m_pressure_noise->GenPositionArray2D(
        m_scratch_pressure.data(), static_cast<int>(count), px, pz, 0.0f, 0.0f, m_weather_seed);
    // Distinct seed salts for the climate channels so temperature/humidity are
    // decorrelated from pressure but still come from the +12 stream.
    m_temperature_noise->GenPositionArray2D(
        m_scratch_temperature.data(), static_cast<int>(count), px, pz, 0.0f, 0.0f, m_weather_seed + 101);
    m_humidity_noise->GenPositionArray2D(
        m_scratch_humidity.data(), static_cast<int>(count), px, pz, 0.0f, 0.0f, m_weather_seed + 211);

    for (int lz = 0; lz < extent; ++lz) {
        for (int lx = 0; lx < extent; ++lx) {
            const std::size_t i = m_precip.index(lx, lz);
            const float pressure = m_scratch_pressure[i];
            const float temperature = m_scratch_temperature[i];
            // humidity noise is in ~[-1, 1]; remap to [0, 1] for the thresholds.
            const float humidity = 0.5f * (m_scratch_humidity[i] + 1.0f);
            const WeatherCategory cat = Classify(pressure, temperature, humidity);
            m_category.cells()[i] = static_cast<std::uint8_t>(cat);

            // Category base precipitation: only rain/snow precipitate; overcast/
            // fog are dry-but-grey. Scaled by how far below the precip threshold
            // the pressure sits (deeper low => heavier base precip).
            float base_precip = 0.0f;
            if (cat == WeatherCategory::Rain || cat == WeatherCategory::Snow) {
                const float depth = kOvercastPressure - pressure; // > 0 in this branch
                base_precip = depth > 0.0f ? (depth > 0.6f ? 0.6f : depth) : 0.0f;
            }
            m_precip.cells()[i] = base_precip;
        }
    }

    // Anchor diagnostics: the category + wind under the stream anchor.
    int alx = 0;
    int alz = 0;
    bool in_region = false;
    LocalCell(region_anchor, alx, alz, in_region);
    if (in_region) {
        m_anchor_category = static_cast<WeatherCategory>(m_category.at(alx, alz));
    } else {
        m_anchor_category = WeatherCategory::Clear;
    }
}

void WeatherSystem::StepStormCells(std::uint64_t tick, const Vec3& region_anchor, const WindFieldSystem* wind) {
    // 1) Age + advect existing cells; drop expired ones. Canonical spawn order is
    // preserved (erase-by-index keeps relative order).
    std::vector<StormCell> survivors;
    survivors.reserve(m_storm_cells.size());
    for (StormCell cell : m_storm_cells) {
        const std::uint64_t age = tick - cell.spawn_tick;
        if (age >= cell.lifetime_ticks) {
            continue; // expired
        }
        // Advect by the local wind (ground layer) if available; else drift on the
        // stored velocity. fixed_dt at 30 Hz; we advance one tick.
        if (wind) {
            const Vec2 w = wind->SampleWind(Vec3(cell.center_world.x, 5.0f, cell.center_world.y),
                                            WindLayer::Ground);
            cell.velocity = w;
        }
        // One tick of advection (1/30 s). Position is world XZ stored in Vec2.
        const float dt = 1.0f / 30.0f;
        cell.center_world.x += cell.velocity.x * dt;
        cell.center_world.y += cell.velocity.y * dt;

        // Intensity envelope: ramp in over kStormRampTicks, hold, decay out.
        const std::uint32_t a = static_cast<std::uint32_t>(age);
        float env;
        if (a < kStormRampTicks) {
            env = static_cast<float>(a) / static_cast<float>(kStormRampTicks);
        } else if (a > cell.lifetime_ticks - kStormRampTicks) {
            const std::uint32_t remain = cell.lifetime_ticks - a;
            env = static_cast<float>(remain) / static_cast<float>(kStormRampTicks);
        } else {
            env = 1.0f;
        }
        cell.intensity = kStormPeakIntensity * env;
        survivors.push_back(cell);
    }
    m_storm_cells.swap(survivors);

    // 2) Spawn opportunity on the fixed epoch. Deterministic coin keyed on the
    // epoch index + the seed + the anchored region cell (so distinct regions get
    // distinct schedules). NO wall-clock, NO std::random.
    if (tick > 0 && (tick % kStormSpawnEpochTicks) == 0) {
        const std::uint64_t epoch = tick / kStormSpawnEpochTicks;
        const std::int64_t region_cx = static_cast<std::int64_t>(
            std::floor(region_anchor.x / m_precip.cell_size_m()));
        const std::int64_t region_cz = static_cast<std::int64_t>(
            std::floor(region_anchor.z / m_precip.cell_size_m()));
        std::uint64_t key = static_cast<std::uint64_t>(static_cast<std::uint32_t>(m_weather_seed));
        key = Mix64(key ^ (epoch * 0x100000001b3ull));
        key ^= static_cast<std::uint64_t>(region_cx) * 0x9e3779b1u;
        key ^= static_cast<std::uint64_t>(region_cz) * 0x85ebca77u;

        const float coin = UnitFloat(key);
        // ~55% of epochs spawn a cell (until the cap), giving a steady but bounded
        // population the Endurance300Storm gate can exercise.
        if (coin < 0.55f && static_cast<int>(m_storm_cells.size()) < kMaxStormCells) {
            StormCell cell;
            // Spawn position: offset from the anchor by a seeded vector within the
            // streamed extent so cells sweep across the region.
            const float half_extent_m =
                0.5f * static_cast<float>(m_precip.extent_cells()) * m_precip.cell_size_m();
            const float ox = (UnitFloat(key ^ 0xAAAA5555ull) * 2.0f - 1.0f) * half_extent_m * 0.8f;
            const float oz = (UnitFloat(key ^ 0x5555AAAAull) * 2.0f - 1.0f) * half_extent_m * 0.8f;
            cell.center_world = Vec2(region_anchor.x + ox, region_anchor.z + oz);
            // Initial velocity from the wind at the spawn point (ground layer).
            if (wind) {
                cell.velocity = wind->SampleWind(Vec3(cell.center_world.x, 5.0f, cell.center_world.y),
                                                 WindLayer::Ground);
            }
            cell.intensity = 0.0f;
            cell.spawn_tick = tick;
            cell.lifetime_ticks = kStormLifetimeTicks;
            // Per-cell salt derived from the schedule key (deterministic, unique
            // per epoch). T-I5a-5 seeds its strike stream from this.
            cell.seed_salt = static_cast<std::uint32_t>(Mix64(key ^ 0xC0FFEEull) & 0xffffffffull);
            m_storm_cells.push_back(cell);
        }
    }
}

float WeatherSystem::StormPrecipAt(const Vec3& world_pos) const {
    float precip = 0.0f;
    for (const StormCell& cell : m_storm_cells) {
        const float dx = world_pos.x - cell.center_world.x;
        const float dz = world_pos.z - cell.center_world.y;
        const float dist = DeterministicMath::Sqrt(dx * dx + dz * dz);
        if (dist >= kStormRadiusM) {
            continue;
        }
        // Smooth falloff (1 - (d/r))^2, scaled by the cell intensity envelope.
        const float t = 1.0f - (dist / kStormRadiusM);
        const float falloff = t * t;
        precip += cell.intensity * falloff;
    }
    return precip > 1.0f ? 1.0f : precip;
}

float WeatherSystem::StormIntensityAt(const Vec3& world_pos) const {
    float best = 0.0f;
    for (const StormCell& cell : m_storm_cells) {
        const float dx = world_pos.x - cell.center_world.x;
        const float dz = world_pos.z - cell.center_world.y;
        const float dist = DeterministicMath::Sqrt(dx * dx + dz * dz);
        if (dist >= kStormRadiusM) {
            continue;
        }
        const float t = 1.0f - (dist / kStormRadiusM);
        const float v = cell.intensity * t;
        if (v > best) {
            best = v;
        }
    }
    return best;
}

void WeatherSystem::Update(std::uint64_t tick, const Vec3& region_anchor, const WindFieldSystem* wind) {
    m_last_tick = tick;
    // 1) Category map + base precipitation field for this tick.
    RebuildFields(tick, region_anchor);
    // 2) Storm cells: age/advect existing, spawn on the deterministic schedule.
    StepStormCells(tick, region_anchor, wind);
    // 3) Anchor wind diagnostic (for the overlay's wind-direction uniform).
    if (wind) {
        m_anchor_wind = wind->SampleWind(Vec3(region_anchor.x, 5.0f, region_anchor.z),
                                         WindLayer::Ground);
    } else {
        m_anchor_wind = Vec2(0.0f);
    }
}

WeatherCategory WeatherSystem::CategoryAt(const Vec3& world_pos) const {
    int lx = 0;
    int lz = 0;
    bool in_region = false;
    LocalCell(world_pos, lx, lz, in_region);
    if (!in_region) {
        return WeatherCategory::Clear; // out-of-region clamps to the control-phase default
    }
    return static_cast<WeatherCategory>(m_category.at(lx, lz));
}

float WeatherSystem::PrecipitationAt(const Vec3& world_pos) const {
    int lx = 0;
    int lz = 0;
    bool in_region = false;
    LocalCell(world_pos, lx, lz, in_region);
    float base = 0.0f;
    if (in_region) {
        base = m_precip.at(lx, lz);
    }
    const float total = base + StormPrecipAt(world_pos);
    return total > 1.0f ? 1.0f : total;
}

WeatherSample WeatherSystem::SampleAt(const Vec3& world_pos) const {
    WeatherSample sample;
    sample.category = CategoryAt(world_pos);
    sample.precip_intensity = PrecipitationAt(world_pos);
    sample.storm_intensity = StormIntensityAt(world_pos);
    sample.wind = m_anchor_wind;
    return sample;
}

std::string WeatherSystem::ComputeWeatherSubHash() const {
    // fnv1a-64 over: seed, tick, grid geometry, origin, the per-cell category +
    // precip in the FieldGrid canonical order, then the bounded storm-cell set
    // (count + each cell's pos/velocity/intensity/spawn/lifetime/salt), then a
    // RESERVED strike-schedule epoch (0 in 5a; T-I5a-5 folds the real schedule
    // here WITHOUT reordering the preceding bytes). Bit-exact float hashing so a
    // one-ULP drift fails the gate loudly.
    std::uint64_t hash = 14695981039346656037ull; // fnv offset basis
    MixU64(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(m_weather_seed)));
    MixU64(hash, m_last_tick);
    MixU64(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(m_precip.extent_cells())));
    MixFloat(hash, m_precip.cell_size_m());
    MixU64(hash, static_cast<std::uint64_t>(m_precip.origin_cell_x()));
    MixU64(hash, static_cast<std::uint64_t>(m_precip.origin_cell_z()));

    const int extent = m_precip.extent_cells();
    for (int lz = 0; lz < extent; ++lz) {
        for (int lx = 0; lx < extent; ++lx) {
            MixU8(hash, m_category.at(lx, lz));
            MixFloat(hash, m_precip.at(lx, lz));
        }
    }

    // Storm-cell set (bounded; canonical spawn order).
    MixU64(hash, static_cast<std::uint64_t>(m_storm_cells.size()));
    for (const StormCell& cell : m_storm_cells) {
        MixFloat(hash, cell.center_world.x);
        MixFloat(hash, cell.center_world.y);
        MixFloat(hash, cell.velocity.x);
        MixFloat(hash, cell.velocity.y);
        MixFloat(hash, cell.intensity);
        MixU64(hash, cell.spawn_tick);
        MixU64(hash, static_cast<std::uint64_t>(cell.lifetime_ticks));
        MixU64(hash, static_cast<std::uint64_t>(cell.seed_salt));
    }

    // Reserved lightning strike-schedule slot (T-I5a-5). Hashed as a single 0 so
    // the byte layout is fixed now; B3 replaces this with the real schedule.
    MixU64(hash, 0ull);

    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

} // namespace Luminumbra::Systems
