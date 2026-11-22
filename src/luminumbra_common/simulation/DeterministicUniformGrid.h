#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace luminumbra::simulation {

// Integer XZ broad phase. Each source occupies one cell; hash collisions only
// increase work. Inclusive queries return original source indices, once each,
// in ascending order. Rebuilding and querying retain their scratch capacities.
class DeterministicUniformGrid {
public:
    struct Point {
        std::int64_t x = 0;
        std::int64_t z = 0;
    };

    // A nonpositive cell width is refused before changing the index.
    void Build(std::span<const Point> points, std::int64_t cell_width);
    // The returned span remains valid until the next Build or Query.
    [[nodiscard]] std::span<const std::size_t> Query(Point minimum, Point maximum);
    // Counts source point tests, including hash collisions, since the last Build.
    [[nodiscard]] std::uint64_t CandidateTests() const {
        return m_candidate_tests;
    }

private:
    struct Cursor {
        std::size_t offset = 0;
        std::size_t end = 0;
    };
    [[nodiscard]] std::size_t Bucket(Point cell) const;
    void TestPoint(std::size_t index, Point minimum, Point maximum);

    std::int64_t m_cell_width = 1;
    std::uint64_t m_candidate_tests = 0;
    std::vector<Point> m_points;
    std::vector<std::size_t> m_offsets;
    std::vector<std::size_t> m_cursor;
    std::vector<std::size_t> m_indices;
    std::vector<unsigned char> m_visited;
    std::vector<std::size_t> m_buckets;
    std::vector<Cursor> m_heap;
    std::vector<std::size_t> m_result;
};

} // namespace luminumbra::simulation
