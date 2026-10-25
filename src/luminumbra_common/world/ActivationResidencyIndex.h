#pragma once

#include "Chunk.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Luminumbra::World {

// Owner-thread derived state. Only publication, never worker completion, may
// change content versions or residency. Neither the index nor its counters is hashed.
class ActivationResidencyIndex {
public:
    struct SurfaceSpan {
        int min_y = 0;
        int max_y = 0;
        int center_y = 0;
        u8 biome_id = 255u;
    };
    struct Candidate {
        IVec3 coords{0};
        bool surface = false;
        int ring_distance = 0;
        int horizontal_distance_sq = 0;
        int vertical_rank = 0;
        bool interior_focus = false;
    };
    struct Priority {
        bool operator()(const Candidate& a, const Candidate& b) const {
            if (a.interior_focus != b.interior_focus)
                return a.interior_focus;
            if (a.surface != b.surface)
                return a.surface;
            if (a.ring_distance != b.ring_distance)
                return a.ring_distance < b.ring_distance;
            if (a.horizontal_distance_sq != b.horizontal_distance_sq)
                return a.horizontal_distance_sq < b.horizontal_distance_sq;
            if (a.vertical_rank != b.vertical_rank)
                return a.vertical_rank < b.vertical_rank;
            if (a.coords.y != b.coords.y)
                return a.coords.y < b.coords.y;
            if (a.coords.x != b.coords.x)
                return a.coords.x < b.coords.x;
            return a.coords.z < b.coords.z;
        }
    };
    struct Counters {
        std::uint64_t span_cache_hits = 0;
        std::uint64_t span_cache_misses = 0;
        std::uint64_t columns_scanned =
            0;                           // wanted/eviction enumeration, including order restoration
        std::uint64_t index_updates = 0; // residency, content, and wanted-priority changes
    };
    struct Column {
        int x = 0;
        int z = 0;
        std::uint64_t content_version = 0;
        std::uint64_t span_version = 0;
        std::optional<SurfaceSpan> span;
        std::unordered_map<ChunkID, std::shared_ptr<Chunk>> residents;
    };

    static u64 column_key(int x, int z) {
        return (static_cast<u64>(static_cast<u32>(x)) << 32u) | static_cast<u32>(z);
    }

    template<typename Sampler>
    SurfaceSpan surface_span(int x, int z, const Sampler& sample) {
        auto& column = ensure_column(x, z);
        if (column.span && column.span_version == column.content_version) {
            ++m_counters.span_cache_hits;
            return *column.span;
        }
        ++m_counters.span_cache_misses;
        column.span = sample(x, z);
        column.span_version = column.content_version;
        return *column.span;
    }

    void content_changed(const IVec3& coords) {
        auto& column = ensure_column(coords.x, coords.z);
        // Reset the cached value as well as its version, including at saturation.
        if (column.content_version != std::numeric_limits<std::uint64_t>::max())
            ++column.content_version;
        column.span.reset();
        m_dirty_columns.insert(column_key(coords.x, coords.z));
        ++m_counters.index_updates;
    }

    void insert(const std::shared_ptr<Chunk>& chunk) {
        const auto coords = chunk->get_coords();
        auto& column = ensure_column(coords.x, coords.z);
        column.residents.insert_or_assign(chunk->get_id(), chunk);
        m_resident_columns.insert(column_key(coords.x, coords.z));
        if (const auto it = m_wanted.find(chunk->get_id()); it != m_wanted.end())
            remove_pending(it->second);
        m_dirty_columns.insert(column_key(coords.x, coords.z));
        ++m_counters.index_updates;
    }

    void erase(const Chunk& chunk) {
        const auto coords = chunk.get_coords();
        const auto key = column_key(coords.x, coords.z);
        if (const auto it = m_columns.find(key); it != m_columns.end()) {
            it->second.residents.erase(chunk.get_id());
            if (it->second.residents.empty())
                m_resident_columns.erase(key);
        }
        if (const auto it = m_wanted.find(chunk.get_id()); it != m_wanted.end())
            add_pending(it->second);
        m_dirty_columns.insert(key);
        ++m_counters.index_updates;
    }

    // Accept unique candidates with priorities already merged across anchors.
    // Rebuild priorities when the anchor chunks or policy radius change.
    // On unchanged inputs, insert/erase maintain the missing ordered set directly.
    void replace_wanted(const std::vector<Candidate>& candidates) {
        m_wanted.clear();
        m_pending.clear();
        m_surface_pending = 0;
        for (const auto& candidate : candidates) {
            const auto id = Chunk::calculate_id(candidate.coords);
            m_wanted.emplace(id, candidate);
            const auto it = m_columns.find(column_key(candidate.coords.x, candidate.coords.z));
            if (it == m_columns.end() || !it->second.residents.contains(id))
                add_pending(candidate);
        }
        ++m_counters.index_updates;
    }

    const auto& pending() const {
        return m_pending;
    }
    std::size_t surface_pending() const {
        return m_surface_pending;
    }
    const auto& columns() const {
        return m_columns;
    }
    const auto& resident_columns() const {
        return m_resident_columns;
    }
    const auto& dirty_columns() const {
        return m_dirty_columns;
    }
    void clear_dirty_columns() {
        m_dirty_columns.clear();
    }
    void note_column_scan(std::size_t count = 1) {
        m_counters.columns_scanned += count;
    }
    const Counters& counters() const {
        return m_counters;
    }

    void invalidate_spans() {
        for (auto& [key, column] : m_columns) {
            (void)key;
            content_changed(IVec3(column.x, 0, column.z));
        }
    }

    void clear() {
        m_columns.clear();
        m_resident_columns.clear();
        m_dirty_columns.clear();
        m_wanted.clear();
        m_pending.clear();
        m_surface_pending = 0;
        ++m_counters.index_updates;
    }

private:
    Column& ensure_column(int x, int z) {
        const auto [it, inserted] = m_columns.try_emplace(column_key(x, z));
        if (inserted) {
            it->second.x = x;
            it->second.z = z;
        }
        return it->second;
    }
    void add_pending(const Candidate& candidate) {
        if (m_pending.insert(candidate).second && candidate.surface)
            ++m_surface_pending;
    }
    void remove_pending(const Candidate& candidate) {
        if (m_pending.erase(candidate) != 0 && candidate.surface)
            --m_surface_pending;
    }

    std::unordered_map<u64, Column> m_columns;
    std::unordered_set<u64> m_resident_columns;
    std::unordered_set<u64> m_dirty_columns;
    std::unordered_map<ChunkID, Candidate> m_wanted;
    std::set<Candidate, Priority> m_pending;
    std::size_t m_surface_pending = 0;
    Counters m_counters;
};

} // namespace Luminumbra::World
