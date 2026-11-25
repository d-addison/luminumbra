#include "DeterministicUniformGrid.h"

#include "core/IntegerPrimitives.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace luminumbra::simulation {

std::size_t DeterministicUniformGrid::Bucket(Point cell) const {
    // Explicit unsigned mixing, independent of the standard library's hash.
    auto x = static_cast<std::uint64_t>(cell.x);
    auto z = static_cast<std::uint64_t>(cell.z);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    const auto mixed = x ^ z;
    return static_cast<std::size_t>(mixed ^ (mixed >> 31)) & (m_cursor.size() - 1);
}

void DeterministicUniformGrid::Build(std::span<const Point> points, std::int64_t cell_width) {
    if (cell_width <= 0) {
        throw std::invalid_argument("Grid cell width must be positive");
    }
    const auto limit = std::min({m_points.max_size(),
                                 m_offsets.max_size() - 1,
                                 m_cursor.max_size(),
                                 m_indices.max_size(),
                                 m_visited.max_size(),
                                 m_buckets.max_size(),
                                 m_heap.max_size(),
                                 m_result.max_size()});
    // Power-of-two buckets are bounded by eight times the source count (or one
    // for an empty index). Check multiplication and rounding before allocation.
    if (points.size() > limit / 8) {
        throw std::length_error("Grid source count exceeds storage bounds");
    }
    std::size_t bucket_count = 1;
    while (bucket_count < points.size() * 4) {
        bucket_count *= 2;
    }

    // Reserve everything before replacing the previous queryable contents.
    m_points.reserve(points.size());
    m_offsets.reserve(bucket_count + 1);
    m_cursor.reserve(bucket_count);
    m_indices.reserve(points.size());
    m_visited.reserve(bucket_count);
    m_buckets.reserve(bucket_count);
    m_heap.reserve(bucket_count);
    m_result.reserve(points.size());
    m_cell_width = cell_width;
    m_candidate_tests = 0;
    m_points.assign(points.begin(), points.end());
    m_offsets.assign(bucket_count + 1, 0);
    m_cursor.resize(bucket_count);
    m_indices.resize(points.size());
    m_visited.assign(bucket_count, 0);
    m_buckets.clear();
    m_heap.clear();
    m_result.clear();

    const auto cell_of = [cell_width](Point point) {
        return Point{::Luminumbra::Core::FloorDiv(point.x, cell_width),
                     ::Luminumbra::Core::FloorDiv(point.z, cell_width)};
    };
    for (const auto& point : m_points) {
        ++m_offsets[Bucket(cell_of(point)) + 1];
    }
    for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
        m_offsets[bucket + 1] += m_offsets[bucket];
        m_cursor[bucket] = m_offsets[bucket];
    }
    // Stable counting sort into flat CSR buckets; no per-cell allocations.
    for (std::size_t index = 0; index < m_points.size(); ++index) {
        m_indices[m_cursor[Bucket(cell_of(m_points[index]))]++] = index;
    }
}

void DeterministicUniformGrid::TestPoint(std::size_t index, Point minimum, Point maximum) {
    if (m_candidate_tests != std::numeric_limits<std::uint64_t>::max()) {
        ++m_candidate_tests;
    }
    const auto& point = m_points[index];
    if (point.x >= minimum.x && point.x <= maximum.x && point.z >= minimum.z &&
        point.z <= maximum.z) {
        m_result.push_back(index);
    }
}

std::span<const std::size_t> DeterministicUniformGrid::Query(Point minimum, Point maximum) {
    m_result.clear();
    m_buckets.clear();
    m_heap.clear();
    if (minimum.x > maximum.x || minimum.z > maximum.z || m_points.empty()) {
        return m_result;
    }
    const Point first{::Luminumbra::Core::FloorDiv(minimum.x, m_cell_width),
                      ::Luminumbra::Core::FloorDiv(minimum.z, m_cell_width)};
    const Point last{::Luminumbra::Core::FloorDiv(maximum.x, m_cell_width),
                     ::Luminumbra::Core::FloorDiv(maximum.z, m_cell_width)};
    // Unsigned differences cover the entire signed coordinate domain. Avoid +1
    // and product overflow; huge rectangles scan at most the admitted sources.
    const auto dx = static_cast<std::uint64_t>(last.x) - static_cast<std::uint64_t>(first.x);
    const auto dz = static_cast<std::uint64_t>(last.z) - static_cast<std::uint64_t>(first.z);
    const auto budget = m_cursor.size();
    if (dx >= budget || dz >= budget || dx + 1 > budget / (dz + 1)) {
        for (std::size_t index = 0; index < m_points.size(); ++index) {
            TestPoint(index, minimum, maximum);
        }
        return m_result;
    }

    // At most bucket_count cell visits, each bucket scanned once even on a hash
    // collision. Equality exits before incrementing a maximal signed coordinate.
    for (auto x = first.x;; ++x) {
        for (auto z = first.z;; ++z) {
            const auto bucket = Bucket({x, z});
            if (m_visited[bucket] == 0) {
                m_visited[bucket] = 1;
                m_buckets.push_back(bucket);
            }
            if (z == last.z) {
                break;
            }
        }
        if (x == last.x) {
            break;
        }
    }
    for (const auto bucket : m_buckets) {
        if (m_offsets[bucket] != m_offsets[bucket + 1]) {
            m_heap.push_back({m_offsets[bucket], m_offsets[bucket + 1]});
        }
        m_visited[bucket] = 0;
    }
    // Merge the already ascending CSR buckets. A fixed-size neighbourhood keeps
    // even dense queries linear in their candidate count, rather than sorting
    // all results again. Each source is tested at most once per query.
    const auto later = [this](const Cursor& lhs, const Cursor& rhs) {
        return m_indices[lhs.offset] > m_indices[rhs.offset];
    };
    std::make_heap(m_heap.begin(), m_heap.end(), later);
    while (!m_heap.empty()) {
        std::pop_heap(m_heap.begin(), m_heap.end(), later);
        auto cursor = m_heap.back();
        m_heap.pop_back();
        TestPoint(m_indices[cursor.offset], minimum, maximum);
        ++cursor.offset;
        if (cursor.offset < cursor.end) {
            m_heap.push_back(cursor);
            std::push_heap(m_heap.begin(), m_heap.end(), later);
        }
    }
    return m_result;
}

} // namespace luminumbra::simulation
