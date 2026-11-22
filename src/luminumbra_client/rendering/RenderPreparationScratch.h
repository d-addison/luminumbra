#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>

namespace Luminumbra::Rendering {

// Counts actual backing allocations, including unordered-map nodes and buckets.
// The owner keeps the counter alive longer than every container using it.
template<typename T>
struct RenderScratchAllocator {
    using value_type = T;
    std::uint64_t* allocations;

    explicit RenderScratchAllocator(std::uint64_t& counter) noexcept
        : allocations(&counter) {}
    template<typename U>
    RenderScratchAllocator(const RenderScratchAllocator<U>& other) noexcept
        : allocations(other.allocations) {}

    T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        T* storage = std::allocator<T>{}.allocate(count);
        ++*allocations;
        return storage;
    }
    void deallocate(T* storage, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(storage, count);
    }
    template<typename U>
    bool operator==(const RenderScratchAllocator<U>& other) const noexcept {
        return allocations == other.allocations;
    }
};

// Zero is reserved for unvisited records. The wrap sweep occurs only once per
// 2^64 visits and is bounded by the retained record count.
template<typename Records>
std::uint64_t NextRenderFrameStamp(std::uint64_t previous, Records& records) {
    const auto next = previous + 1;
    if (next != 0) {
        return next;
    }
    for (auto& [id, record] : records) {
        (void)id;
        record.last_seen_frame = 0;
    }
    return 1;
}

// Exact terrain-bearing membership, independent of input order and mesh version.
// Storage is bounded by the union of successive snapshots during refresh, then
// by the current snapshot. Settled refreshes only visit the supplied IDs.
class TerrainResidencyRevision {
public:
    explicit TerrainResidencyRevision(std::uint64_t& allocations)
        : m_records(RenderScratchAllocator<std::pair<const std::uint64_t, Record>>(allocations)) {}

    void begin_frame() {
        m_frame = NextRenderFrameStamp(m_frame, m_records);
        m_seen = 0;
    }
    void mark(std::uint64_t id) {
        auto [entry, inserted] = m_records.try_emplace(id);
        if (inserted) {
            ++m_revision;
        }
        if (entry->second.last_seen_frame != m_frame) {
            entry->second.last_seen_frame = m_frame;
            ++m_seen;
        }
    }
    void end_frame() {
        if (m_seen == m_records.size()) {
            return;
        }
        for (auto entry = m_records.begin(); entry != m_records.end();) {
            if (entry->second.last_seen_frame != m_frame) {
                entry = m_records.erase(entry);
                ++m_revision;
            } else {
                ++entry;
            }
        }
    }
    std::uint64_t revision() const {
        return m_revision;
    }
    std::size_t size() const {
        return m_records.size();
    }

private:
    struct Record {
        std::uint64_t last_seen_frame = 0;
    };
    std::unordered_map<std::uint64_t,
                       Record,
                       std::hash<std::uint64_t>,
                       std::equal_to<std::uint64_t>,
                       RenderScratchAllocator<std::pair<const std::uint64_t, Record>>>
        m_records;
    std::uint64_t m_frame = 0;
    std::uint64_t m_revision = 0;
    std::size_t m_seen = 0;
};

} // namespace Luminumbra::Rendering
