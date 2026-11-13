#include "SimulationTerrain.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace Luminumbra::world {
namespace {
constexpr std::int64_t kChunkMm = SimulationTerrain::kChunkCells * 1000;

std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor) {
    return value / divisor - (value % divisor < 0 ? 1 : 0);
}

bool ValidCoordinate(std::int64_t mm) {
    const auto chunk = FloorDiv(mm, kChunkMm);
    return chunk >= std::numeric_limits<std::int32_t>::min() &&
           chunk <= std::numeric_limits<std::int32_t>::max();
}

std::size_t Index(std::int64_t x, std::int64_t y, std::int64_t z) {
    return static_cast<std::size_t>(x + 17 * y + 17 * 17 * z);
}

double Interpolate(const std::array<float, SimulationTerrain::kLatticeSamples>& samples,
                   std::int64_t x,
                   std::int64_t y,
                   std::int64_t z,
                   double tx,
                   double tz) {
    const double a = samples[Index(x, y, z)];
    const double b = samples[Index(x + 1, y, z)];
    const double c = samples[Index(x, y, z + 1)];
    const double d = samples[Index(x + 1, y, z + 1)];
    const double along_x0 = std::fma(b - a, tx, a);
    const double along_x1 = std::fma(d - c, tx, c);
    return std::fma(along_x1 - along_x0, tz, along_x0);
}
} // namespace

SimulationTerrain::SimulationTerrain()
    : SimulationTerrain(Limits{}) {}
SimulationTerrain::SimulationTerrain(const Limits& limits)
    : m_limits(limits) {}
SimulationTerrain::~SimulationTerrain() = default;

std::size_t SimulationTerrain::lower_bound(Key key) const {
    std::size_t begin = 0;
    std::size_t end = m_size;
    while (begin < end) {
        const auto middle = begin + (end - begin) / 2;
        if (m_index[middle]->key < key) {
            begin = middle + 1;
        } else {
            end = middle;
        }
    }
    return begin;
}

const SimulationTerrain::Record* SimulationTerrain::find(Key key) const {
    const auto position = lower_bound(key);
    if (position == m_size || m_index[position]->key != key) {
        return nullptr;
    }
    return m_index[position].get();
}

bool SimulationTerrain::agrees(Key key,
                               std::span<const float> samples,
                               std::span<const Lattice> replacements) const {
    // Only 26 neighbours can share nodes: at most 6*289 + 12*17 + 8 comparisons.
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                const auto nx = static_cast<std::int64_t>(key.x) + dx;
                const auto ny = static_cast<std::int64_t>(key.y) + dy;
                const auto nz = static_cast<std::int64_t>(key.z) + dz;
                constexpr auto low = std::numeric_limits<std::int32_t>::min();
                constexpr auto high = std::numeric_limits<std::int32_t>::max();
                if (nx < low || nx > high || ny < low || ny > high || nz < low || nz > high) {
                    continue;
                }
                const Key neighbour_key{static_cast<std::int32_t>(nx),
                                        static_cast<std::int32_t>(ny),
                                        static_cast<std::int32_t>(nz)};
                const auto replacement = std::find_if(replacements.begin(),
                                                      replacements.end(),
                                                      [neighbour_key](const Lattice& lattice) {
                                                          return lattice.key == neighbour_key;
                                                      });
                const auto* neighbour = find(neighbour_key);
                const std::span<const float> adjacent =
                    replacement != replacements.end()
                        ? replacement->samples
                        : (neighbour ? std::span<const float>(neighbour->samples)
                                     : std::span<const float>{});
                if (adjacent.empty()) {
                    continue;
                }
                const int x0 = dx > 0 ? 16 : 0;
                const int y0 = dy > 0 ? 16 : 0;
                const int z0 = dz > 0 ? 16 : 0;
                const int x1 = dx < 0 ? 0 : 16;
                const int y1 = dy < 0 ? 0 : 16;
                const int z1 = dz < 0 ? 0 : 16;
                for (int z = z0; z <= z1; ++z) {
                    for (int y = y0; y <= y1; ++y) {
                        for (int x = x0; x <= x1; ++x) {
                            if (samples[Index(x, y, z)] !=
                                adjacent[Index(x - dx * 16, y - dy * 16, z - dz * 16)]) {
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}

SimulationTerrain::Admission SimulationTerrain::admit(Key key, std::span<const float> samples) {
    const Lattice lattice{key, samples};
    return admit(std::span(&lattice, 1));
}

bool SimulationTerrain::contains(Key key) const {
    return find(key) != nullptr;
}

bool SimulationTerrain::matches(Key key, std::span<const float> samples) const {
    const auto* record = find(key);
    return record && samples.size() == kLatticeSamples &&
           std::equal(samples.begin(), samples.end(), record->samples.begin());
}

SimulationTerrain::Admission SimulationTerrain::admit(std::span<const Lattice> lattices) {
    if (lattices.empty())
        return Admission::Unavailable;
    if (lattices.size() > kMaxBatch || m_limits.lattices == 0 || m_limits.lattices > kMaxLattices ||
        m_limits.owned_bytes > kMaxOwnedBytes ||
        m_limits.lattices > m_limits.owned_bytes / sizeof(Entry))
        return Admission::WorkLimit;
    const auto index_bytes = m_limits.lattices * sizeof(Entry);
    if (lattices.size() + m_size > (m_limits.owned_bytes - index_bytes) / sizeof(Record))
        return Admission::WorkLimit;
    std::size_t additions = 0;
    for (std::size_t i = 0; i < lattices.size(); ++i) {
        const auto& lattice = lattices[i];
        if (lattice.samples.size() != kLatticeSamples ||
            !std::all_of(lattice.samples.begin(), lattice.samples.end(), [](float value) {
                return std::isfinite(value);
            }))
            return Admission::Unavailable;
        for (std::size_t j = 0; j < i; ++j)
            if (lattices[j].key == lattice.key)
                return Admission::Unavailable;
        additions += contains(lattice.key) ? 0u : 1u;
    }
    if (additions > m_limits.lattices - m_size)
        return Admission::WorkLimit;
    for (const auto& lattice : lattices)
        if (!agrees(lattice.key, lattice.samples, lattices))
            return Admission::Unavailable;
    // Fixed stack staging has no growth slack; only participating records are copied.
    std::array<Entry, kMaxBatch> staged;
    std::unique_ptr<Entry[]> index;
    try {
        for (std::size_t i = 0; i < lattices.size(); ++i) {
            staged[i] = std::make_unique<Record>();
            staged[i]->key = lattices[i].key;
            std::copy(
                lattices[i].samples.begin(), lattices[i].samples.end(), staged[i]->samples.begin());
        }
        if (!m_index)
            index = std::make_unique<Entry[]>(m_limits.lattices);
    } catch (const std::bad_alloc&) {
        return Admission::WorkLimit;
    }
    if (index)
        m_index = std::move(index);
    // All fallible work precedes publication. At most 128*4096 pointer moves.
    for (std::size_t i = 0; i < lattices.size(); ++i) {
        const auto position = lower_bound(lattices[i].key);
        if (position == m_size || m_index[position]->key != lattices[i].key) {
            for (auto j = m_size; j > position; --j)
                m_index[j] = std::move(m_index[j - 1]);
            ++m_size;
        }
        m_index[position] = std::move(staged[i]);
    }
    return Admission::Accepted;
}

SimulationTerrain::Result SimulationTerrain::first_surface(std::int64_t x_mm,
                                                           std::int64_t z_mm,
                                                           std::int64_t start_y_mm,
                                                           std::int64_t min_y_mm,
                                                           std::size_t max_cells) const {
    Result result;
    if (min_y_mm > start_y_mm || !ValidCoordinate(x_mm) || !ValidCoordinate(z_mm) ||
        !ValidCoordinate(start_y_mm) || !ValidCoordinate(min_y_mm)) {
        return result;
    }
    const auto first = FloorDiv(start_y_mm, 1000) - (start_y_mm % 1000 == 0 ? 1 : 0);
    const auto last = std::min(first, FloorDiv(min_y_mm, 1000));
    const auto cells = static_cast<std::uint64_t>(first - last + 1);
    if (cells > std::min(max_cells, kMaxVerticalCells)) {
        result.status = Status::WorkLimit;
        return result;
    }
    const auto cx = FloorDiv(x_mm, kChunkMm);
    const auto cz = FloorDiv(z_mm, kChunkMm);
    const auto lx_mm = x_mm - cx * kChunkMm;
    const auto lz_mm = z_mm - cz * kChunkMm;
    const double tx = static_cast<double>(lx_mm % 1000) / 1000.0;
    const double tz = static_cast<double>(lz_mm % 1000) / 1000.0;
    const Record* record = nullptr;
    std::int64_t previous_y = std::numeric_limits<std::int64_t>::min();
    for (auto cell = first; cell >= last; --cell) {
        const auto cy = FloorDiv(cell, kChunkCells);
        if (cy < std::numeric_limits<std::int32_t>::min()) {
            return result;
        }
        if (cy != previous_y) {
            ++result.visits.lattice_lookups;
            record = find({static_cast<std::int32_t>(cx),
                           static_cast<std::int32_t>(cy),
                           static_cast<std::int32_t>(cz)});
            previous_y = cy;
        }
        if (record == nullptr) {
            return result;
        }
        ++result.visits.cells;
        const auto ly = cell - cy * kChunkCells;
        const double lower = Interpolate(record->samples, lx_mm / 1000, ly, lz_mm / 1000, tx, tz);
        const double upper =
            Interpolate(record->samples, lx_mm / 1000, ly + 1, lz_mm / 1000, tx, tz);
        result.visits.samples += 8;
        if (upper >= 0.0 && lower <= 0.0 && upper > lower) {
            const double height_mm =
                std::fma(-lower / (upper - lower), 1000.0, static_cast<double>(cell * 1000));
            if (height_mm <= static_cast<double>(start_y_mm) &&
                height_mm >= static_cast<double>(min_y_mm)) {
                result.status = Status::Surface;
                result.y_mm = static_cast<std::int64_t>(std::round(height_mm));
                return result;
            }
        }
    }
    result.status = Status::NoSurface;
    return result;
}

std::size_t SimulationTerrain::lattice_count() const {
    return m_size;
}

std::size_t SimulationTerrain::owned_bytes() const {
    return m_index ? m_limits.lattices * sizeof(Entry) + m_size * sizeof(Record) : 0;
}
} // namespace Luminumbra::world
