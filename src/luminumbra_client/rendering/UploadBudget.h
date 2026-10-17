#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Luminumbra::Rendering {

struct UploadLimits {
    std::size_t bytes = 4 * 1024 * 1024;
    std::size_t count = 8;
    std::size_t progress = 1;
    double milliseconds = 3.0;

    constexpr bool valid() const {
        return bytes >= 1 && bytes <= 64 * 1024 * 1024 && count >= 1 && count <= 64 &&
               progress >= 1 && progress <= count && milliseconds >= 0.1 && milliseconds <= 16.0;
    }
};
inline constexpr UploadLimits kTerrainUploadLimits{};
inline constexpr UploadLimits kWaterUploadLimits{1024 * 1024, 8, 1, 1.0};
static_assert(kTerrainUploadLimits.valid() && kWaterUploadLimits.valid());

// Check exact sizes before copying payloads or allocating graphics storage.
inline bool mesh_upload_bytes(std::size_t vertices,
                              std::size_t vertex_size,
                              std::size_t indices,
                              std::size_t& bytes) {
    constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (vertex_size == 0 || vertices > std::numeric_limits<std::uint32_t>::max() ||
        indices > std::numeric_limits<std::uint32_t>::max() || vertices > maximum / vertex_size)
        return false;
    const auto vertex_bytes = vertices * vertex_size;
    if (indices > (maximum - vertex_bytes) / sizeof(std::uint32_t))
        return false;
    bytes = vertex_bytes + indices * sizeof(std::uint32_t);
    return true;
}

// Allocate first: failed replacement must not release resident geometry.
template<typename Handle, typename Allocate, typename Release>
Handle replace_upload(Handle resident, Handle invalid, Allocate allocate, Release release) {
    const auto replacement = allocate();
    if (replacement != invalid)
        release(resident);
    return replacement;
}

struct UploadWorkStats {
    std::size_t attempts = 0;
    std::size_t adopted = 0;
    std::size_t bytes = 0;
    std::size_t byte_overshoot = 0;
    double elapsed_ms = 0;
    double overshoot_ms = 0;
};

// Clock-free policy. Callers inject inclusive elapsed time, starting before
// preparation. A single indivisible upload may exceed bytes/time for progress.
class UploadBudget {
public:
    explicit UploadBudget(UploadLimits limits)
        : m_limits(limits.valid() ? limits : kTerrainUploadLimits) {}
    bool admits(std::size_t bytes, double elapsed_ms) const {
        if (m_stats.attempts >= m_limits.count)
            return false;
        if (m_stats.attempts < m_limits.progress)
            return true;
        return elapsed_ms < m_limits.milliseconds && m_stats.bytes <= m_limits.bytes &&
               bytes <= m_limits.bytes - m_stats.bytes;
    }
    void charge(std::size_t bytes, bool adopted) {
        ++m_stats.attempts;
        m_stats.adopted += adopted ? 1u : 0u;
        // Saturation keeps diagnostic arithmetic defined even for injected costs.
        m_stats.bytes += std::min(bytes, std::numeric_limits<std::size_t>::max() - m_stats.bytes);
    }
    UploadWorkStats finish(double elapsed_ms) const {
        auto result = m_stats;
        result.elapsed_ms = elapsed_ms;
        result.overshoot_ms = std::max(0.0, elapsed_ms - m_limits.milliseconds);
        result.byte_overshoot = result.bytes > m_limits.bytes ? result.bytes - m_limits.bytes : 0;
        return result;
    }

private:
    UploadLimits m_limits;
    UploadWorkStats m_stats{};
};

// Stable linear ordering of unsigned keys. Fixed 256-bin scratch and one
// metadata vector bounded by the input size; no mesh payloads are copied here.
template<typename Item, typename Key>
void order_uploads(std::vector<Item>& items, Key key) {
    std::vector<Item> scratch(items.size());
    for (unsigned shift = 0; shift < 64; shift += 8) {
        std::array<std::size_t, 256> offsets{};
        for (const auto& item : items)
            ++offsets[(key(item) >> shift) & 255u];
        std::size_t offset = 0;
        for (auto& bin : offsets) {
            const auto count = bin;
            bin = offset;
            offset += count;
        }
        for (const auto& item : items)
            scratch[offsets[(key(item) >> shift) & 255u]++] = item;
        items.swap(scratch);
    }
}

// One metadata entry per currently pending renderable chunk. Refreshing a
// replacement never resets its FIFO position. New arrivals cannot starve it.
// Lookups are expected O(1); prune visits each entry once. No payload ownership.
// During refresh, metadata is bounded by the union of two successive snapshots;
// end_refresh removes every entry absent from the current snapshot.
template<typename Item>
class UploadQueue {
public:
    UploadQueue() = default;
    UploadQueue(const UploadQueue&) = delete;
    UploadQueue& operator=(const UploadQueue&) = delete;
    void begin_refresh() {
        for (auto& entry : m_items)
            entry.seen = false;
    }
    void refresh(std::uint64_t id, const Item& item) {
        const auto found = m_index.find(id);
        if (found != m_index.end()) {
            found->second->item = item;
            found->second->seen = true;
        } else {
            m_items.push_back({id, item, true});
            m_index.emplace(id, std::prev(m_items.end()));
        }
    }
    void end_refresh() {
        for (auto it = m_items.begin(); it != m_items.end();) {
            if (it->seen) {
                ++it;
            } else {
                m_index.erase(it->id);
                it = m_items.erase(it);
            }
        }
    }
    std::size_t size() const {
        return m_items.size();
    }
    bool contains(std::uint64_t id) const {
        return m_index.contains(id);
    }
    const Item& front() const {
        return m_items.front().item;
    }
    void complete(bool adopted) {
        if (adopted) {
            m_index.erase(m_items.front().id);
            m_items.pop_front();
        } else {
            m_items.splice(m_items.end(), m_items, m_items.begin());
        }
    }
    void clear() {
        m_index.clear();
        m_items.clear();
    }

private:
    struct Entry {
        std::uint64_t id;
        Item item;
        bool seen;
    };
    std::list<Entry> m_items;
    std::unordered_map<std::uint64_t, typename std::list<Entry>::iterator> m_index;
};

// Refresh existing FIFO entries without sorting them again. Only arrivals need
// priority ordering; sorting retained work cannot change its queue position.
// Two scratch pointer arrays are each bounded by the candidate count, never
// copying mesh payloads. Each radix sort has eight passes: O(1) per candidate.
template<typename Item, typename Id, typename Priority>
void refresh_uploads(UploadQueue<Item>& queue,
                     const std::vector<Item>& candidates,
                     Id id,
                     Priority priority) {
    queue.begin_refresh();
    std::vector<const Item*> arrivals;
    arrivals.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (queue.contains(id(candidate)))
            queue.refresh(id(candidate), candidate);
        else
            arrivals.push_back(&candidate);
    }
    if (arrivals.size() > 1) {
        order_uploads(arrivals, [&](const Item* item) { return id(*item); });
        order_uploads(arrivals, [&](const Item* item) { return priority(*item); });
    }
    for (const auto* arrival : arrivals)
        queue.refresh(id(*arrival), *arrival);
    queue.end_refresh();
}

// The callback performs copying, ground construction and GPU adoption only
// after admission. Failure rotates to the tail; each item is tried at most once
// per frame. Resident resources are owned by the callback, never by the queue.
template<typename Item, typename Cost, typename Adopt, typename Elapsed>
UploadWorkStats drain_uploads(
    UploadQueue<Item>& queue, UploadLimits limits, Cost cost, Adopt adopt, Elapsed elapsed) {
    UploadBudget budget(limits);
    const auto pending = queue.size();
    for (std::size_t visited = 0; visited < pending; ++visited) {
        const auto& item = queue.front();
        const auto bytes = cost(item);
        if (!budget.admits(bytes, elapsed()))
            break;
        const bool adopted = adopt(item);
        budget.charge(bytes, adopted);
        queue.complete(adopted);
    }
    return budget.finish(elapsed());
}

// Publication may add work during a drain. Each observed arrival buys one more
// pass, up to 8192 arrivals per wait. Failed attempts consume the initial backlog
// allowance; neither persistent failure nor an endless producer can extend it.
// This is a presentation failure policy, never a simulation scheduling input.
class UploadSettlingWait {
public:
    bool waiting(std::size_t pending, std::size_t adopted = 0) {
        if (pending == 0)
            return false;
        if (!m_started) {
            m_remaining = pending;
            m_started = true;
        } else {
            const auto survivors = m_previous - std::min(m_previous, adopted);
            const auto arrivals = pending - std::min(pending, survivors);
            if (arrivals > kArrivalAllowance - m_arrivals) {
                m_failed = true;
                return false;
            }
            m_arrivals += arrivals;
            m_remaining +=
                std::min(arrivals, std::numeric_limits<std::size_t>::max() - m_remaining);
        }
        m_previous = pending;
        if (m_remaining == 0) {
            m_failed = true;
            return false;
        }
        --m_remaining;
        ++m_frames;
        return true;
    }
    bool failed() const {
        return m_failed;
    }
    std::size_t frames() const {
        return m_frames;
    }

private:
    static constexpr std::size_t kArrivalAllowance = 8192;
    std::size_t m_previous = 0;
    std::size_t m_remaining = 0;
    std::size_t m_arrivals = 0;
    std::size_t m_frames = 0;
    bool m_started = false;
    bool m_failed = false;
};

// Fixed-scene qualification drains presentation without advancing simulation.
// Pending returns {remaining, adopted by the last render}, in both upload lanes.
template<typename Pending, typename Render>
bool settle_uploads(Pending pending, Render render) {
    UploadSettlingWait wait;
    auto work = pending();
    while (wait.waiting(work.first, work.second)) {
        render();
        work = pending();
    }
    return !wait.failed();
}
} // namespace Luminumbra::Rendering
