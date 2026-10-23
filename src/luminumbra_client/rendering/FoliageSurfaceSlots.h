#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>

namespace Luminumbra::Rendering {

// Render-thread ownership of at most 512 surface slots (64 MiB at 128 KiB
// per column). Call retain for the new horizon before release_unretained, then
// acquire in scatter order. Ascending release and a LIFO free list make reuse
// deterministic without depending on unordered-map iteration order.
class FoliageSurfaceSlots {
public:
    static constexpr std::size_t kCapacity = 512;
    // Usually one footprint per column; distinct scatter entries for the same
    // column must retain distinct exact candidate samples.
    struct Key {
        std::uint64_t column;
        std::uint64_t placement;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            return std::hash<std::uint64_t>{}(key.column ^ (key.placement + 0x9E3779B97F4A7C15ull +
                                                            (key.column << 6) + (key.column >> 2)));
        }
    };
    void begin_frame() {
        m_retained.fill(false);
    }
    void retain(std::uint64_t column, std::uint64_t placement = 0) {
        if (const auto slot = find(column, placement))
            m_retained[*slot] = true;
    }
    void release_unretained() {
        for (std::size_t slot = 0; slot < m_capacity; ++slot) {
            if (m_live[slot] && !m_retained[slot]) {
                m_indices.erase(m_keys[slot]);
                m_live[slot] = false;
                m_free[m_free_count++] = slot;
            }
        }
    }
    std::optional<std::size_t> acquire(std::uint64_t column, std::uint64_t placement = 0) {
        if (const auto slot = find(column, placement))
            return slot;
        if (m_free_count == 0) {
            if (m_capacity == kCapacity)
                return std::nullopt;
            const auto next = m_capacity == 0 ? 1 : m_capacity * 2;
            for (std::size_t slot = next; slot > m_capacity; --slot)
                m_free[m_free_count++] = slot - 1;
            m_capacity = next;
        }
        const auto slot = m_free[m_free_count - 1];
        const Key key{column, placement};
        m_indices.emplace(key, slot);
        --m_free_count;
        m_keys[slot] = key;
        m_live[slot] = true;
        return slot;
    }
    std::optional<std::size_t> find(std::uint64_t column, std::uint64_t placement = 0) const {
        const auto it = m_indices.find(Key{column, placement});
        return it == m_indices.end() ? std::nullopt : std::optional<std::size_t>(it->second);
    }
    std::size_t capacity() const {
        return m_capacity;
    }
    void clear() {
        m_indices.clear();
        m_live.fill(false);
        m_free_count = 0;
        for (std::size_t slot = m_capacity; slot > 0; --slot)
            m_free[m_free_count++] = slot - 1;
    }

private:
    std::unordered_map<Key, std::size_t, KeyHash> m_indices;
    std::array<Key, kCapacity> m_keys{};
    std::array<std::size_t, kCapacity> m_free{};
    std::array<bool, kCapacity> m_live{};
    std::array<bool, kCapacity> m_retained{};
    std::size_t m_capacity = 0;
    std::size_t m_free_count = 0;
};

} // namespace Luminumbra::Rendering
