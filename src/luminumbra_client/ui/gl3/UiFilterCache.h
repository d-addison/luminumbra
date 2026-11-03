#pragma once

#include <array>
#include <cstddef>

namespace Luminumbra::Client {

// Exact keys: no probabilistic image hashes. Storage and lookup are bounded by
// eight entries regardless of document size or the number of filter calls.
template<typename Key, typename Value>
class UiFilterCache {
public:
    struct Entry {
        Key key{};
        Value value{};
        bool valid = false;
    };
    const Value* find(const Key& key) const {
        for (const auto& entry : m_entries)
            if (entry.valid && entry.key == key)
                return &entry.value;
        return nullptr;
    }
    // Returns the replaced value so callers can release owned GL resources.
    Value store(const Key& key, const Value& value) {
        auto& entry = m_entries[m_next];
        const Value previous = entry.value;
        entry = {.key = key, .value = value, .valid = true};
        m_next = (m_next + 1) % m_entries.size();
        return previous;
    }
    const auto& entries() const {
        return m_entries;
    }

private:
    std::array<Entry, 8> m_entries{};
    std::size_t m_next = 0;
};

struct BlurKernelKey {
    float sigma = 0;
};
inline bool operator==(const BlurKernelKey& left, const BlurKernelKey& right) {
    return left.sigma == right.sigma;
}
struct FilterQuadKey {
    float offset_x = 0, offset_y = 0;
    float scale_x = 1, scale_y = 1;
};
inline bool operator==(const FilterQuadKey& left, const FilterQuadKey& right) {
    return left.offset_x == right.offset_x && left.offset_y == right.offset_y &&
           left.scale_x == right.scale_x && left.scale_y == right.scale_y;
}
} // namespace Luminumbra::Client
