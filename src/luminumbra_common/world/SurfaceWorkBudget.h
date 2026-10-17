#ifndef LUMINUMBRA_WORLD_SURFACE_WORK_BUDGET_H
#define LUMINUMBRA_WORLD_SURFACE_WORK_BUDGET_H

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace Luminumbra::World {

// Synchronous surface preparation supports the interactive radius-12 horizon
// (625 columns). Far terrain uses the separate asynchronous LOD path. The chunk
// budget matches streaming's active-chunk budget and includes each column's
// entire vertical surface span and both margins.
inline constexpr int kMaxSurfaceRadius = 12;
inline constexpr std::size_t kMaxSurfaceColumns = 625;
inline constexpr std::size_t kMaxSurfaceChunks = 8192;

[[nodiscard]] constexpr bool SurfaceRadiiSupported(std::int64_t surface, std::int64_t collision) {
    return surface >= 0 && surface <= kMaxSurfaceRadius && collision >= 0 &&
           collision <= kMaxSurfaceRadius;
}

// Check before multiplying, including on 32-bit hosts. Refusal carries no size.
[[nodiscard]] constexpr std::optional<std::size_t> SurfaceColumnCount(std::uint64_t radius) {
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    if (radius > (max_size - 1) / 2)
        return std::nullopt;
    const auto diameter = static_cast<std::size_t>(radius) * 2 + 1;
    if (diameter > max_size / diameter)
        return std::nullopt;
    const auto columns = diameter * diameter;
    if (radius > static_cast<std::uint64_t>(kMaxSurfaceRadius) || columns > kMaxSurfaceColumns)
        return std::nullopt;
    return columns;
}

// Includes min_y-1 through max_y+1. Keep subsequent int loop endpoints and
// additions representable; a failed addition leaves the running total intact.
[[nodiscard]] constexpr bool AddSurfaceColumnChunks(int min_y, int max_y, std::size_t& total) {
    if (min_y > max_y || min_y == std::numeric_limits<int>::min() ||
        max_y >= std::numeric_limits<int>::max() - 1 || total > kMaxSurfaceChunks)
        return false;
    const auto count = static_cast<std::uint64_t>(static_cast<std::int64_t>(max_y) - min_y) + 3;
    if (count > kMaxSurfaceChunks - total)
        return false;
    total += static_cast<std::size_t>(count);
    return true;
}

// Strict decimal CLI input: no partial conversion, overflow or silent fallback.
inline bool TryParseSurfaceRadius(std::string_view value, int& radius) {
    if (value.empty())
        return false;
    int parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !SurfaceRadiiSupported(parsed, 0))
        return false;
    radius = parsed;
    return true;
}

} // namespace Luminumbra::World

#endif
