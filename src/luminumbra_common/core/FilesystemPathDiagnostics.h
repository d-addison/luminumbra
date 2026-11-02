#pragma once

#include <cstddef>

namespace Luminumbra::Filesystem {

// Read-only count of GetLongPathNameW calls on this thread (zero on POSIX).
// Diagnostic only: the count never controls path resolution or caches its results.
std::size_t LongNameExpansionCount() noexcept;

} // namespace Luminumbra::Filesystem
