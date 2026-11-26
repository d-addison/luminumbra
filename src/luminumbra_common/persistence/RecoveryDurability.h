#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace Luminumbra::Persistence::detail {

// Concrete operations bind and validate the recorded object, flush that same writable
// handle on Windows, and report close errors. Content equality never completes recovery.
// POSIX repeats every supplied parent barrier, even after an earlier rename succeeded.
// Callers supply at most four parents (destination, temporary and two private stages).
template<bool FlushFile, typename Operations>
bool CompleteRestoration(Operations& operations,
                         const std::array<std::uint64_t, 3>& expected,
                         const std::array<std::filesystem::path, 4>& parents) {
    const bool owned = operations.bind(expected);
    bool flushed = owned;
    if constexpr (FlushFile)
        flushed = owned && operations.flush_file();
    // Release even on ownership/flush refusal; a close failure also blocks progression.
    const bool closed = operations.close_file();
    if (!owned || !flushed || !closed)
        return false;
    if constexpr (!FlushFile) {
        // The destination parent is mandatory; empty optional staging slots are allowed.
        if (parents.front().empty())
            return false;
        for (const auto& parent : parents)
            if (!parent.empty() && !operations.flush_parent(parent))
                return false;
    }
    return true;
}

} // namespace Luminumbra::Persistence::detail
