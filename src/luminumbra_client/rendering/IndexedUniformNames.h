#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Luminumbra::Rendering {

// One cache per shader instance and uniform family. Count and Fields bound all
// storage to the shader's array capacity. Reset when replacing the shader object;
// successful relinks invalidate accessed entries through ProgramGeneration().
template<std::size_t Count, std::size_t Fields = 1>
class IndexedUniformNames {
public:
    using Names = std::array<std::string, Fields>;

    template<typename Build>
    const Names& Get(std::uint64_t generation, std::size_t index, const Build& build) {
        auto& entry = m_entries.at(index);
        if (!entry.ready || entry.generation != generation) {
            // Build before publishing the generation, so a failed allocation
            // leaves this entry eligible for another attempt.
            entry.names = build(index);
            entry.generation = generation;
            entry.ready = true;
        }
        return entry.names;
    }

    void Reset() {
        for (auto& entry : m_entries)
            entry.ready = false;
    }

private:
    struct Entry {
        Names names{};
        std::uint64_t generation = 0;
        bool ready = false;
    };
    std::array<Entry, Count> m_entries{};
};

} // namespace Luminumbra::Rendering
