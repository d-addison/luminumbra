#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Luminumbra::Rendering {

inline constexpr double kImpostorOpaqueCoverage = 0.98;
inline constexpr double kImpostorEmptyCoverage = 0.002;
inline constexpr int kImpostorMaxAtlasSide = 8192;
inline constexpr int kImpostorMaxGrid = 16;
inline constexpr const char* kTreeImpostorAtlasName = "tree-small-02/1.0.0";

enum class ImpostorCoverageState {
    Unchecked,
    Invalid,
    Empty,
    Opaque,
    Accepted
};

struct ImpostorCoverage {
    ImpostorCoverageState state = ImpostorCoverageState::Unchecked;
    double coverage = -1.0; // Unmeasured; never confuse this with an empty atlas.
    double min_cell = -1.0;
    double max_cell = -1.0;
    // At most 16x16 view cells; the helper never allocates.
    std::array<double, kImpostorMaxGrid * kImpostorMaxGrid> cells{};
    std::size_t cell_count = 0;

    bool usable() const {
        return state == ImpostorCoverageState::Accepted;
    }
    bool refused() const {
        return state != ImpostorCoverageState::Unchecked && !usable();
    }
    const char* status() const {
        switch (state) {
            case ImpostorCoverageState::Unchecked:
                return "unchecked";
            case ImpostorCoverageState::Invalid:
                return "invalid";
            case ImpostorCoverageState::Empty:
                return "empty";
            case ImpostorCoverageState::Opaque:
                return "opaque";
            case ImpostorCoverageState::Accepted:
                return "accepted";
        }
        return "invalid";
    }
};

// Exact, tightly packed RGBA8 only. Bounds cap the scan at 8192^2 texels and
// storage at 256 cells. Check dimensions before multiplication or buffer access.
inline std::size_t ImpostorRgbaBytes(int width, int height, int grid) {
    if (width <= 0 || height <= 0 || width > kImpostorMaxAtlasSide ||
        height > kImpostorMaxAtlasSide || grid <= 0 || grid > kImpostorMaxGrid ||
        width % grid != 0 || height % grid != 0) {
        return 0;
    }
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
}

inline ImpostorCoverage
MeasureImpostorCoverage(std::span<const std::uint8_t> rgba, int width, int height, int grid = 1) {
    ImpostorCoverage result;
    result.state = ImpostorCoverageState::Invalid;
    const auto bytes = ImpostorRgbaBytes(width, height, grid);
    if (bytes == 0 || rgba.size() != bytes) {
        return result;
    }
    const auto columns = static_cast<std::size_t>(width);
    const auto cell_width = columns / static_cast<std::size_t>(grid);
    const auto cell_height = static_cast<std::size_t>(height / grid);
    const auto cell_pixels = cell_width * cell_height;
    const auto grid_size = static_cast<std::size_t>(grid);
    std::size_t total = 0;
    result.cell_count = grid_size * grid_size;
    result.min_cell = 1.0;
    result.max_cell = 0.0;
    for (std::size_t cell = 0; cell < result.cell_count; ++cell) {
        std::size_t covered = 0;
        for (std::size_t y = 0; y < cell_height; ++y) {
            const auto row = (cell / grid_size * cell_height + y) * columns;
            for (std::size_t x = 0; x < cell_width; ++x) {
                const auto pixel = row + cell % grid_size * cell_width + x;
                covered += rgba[pixel * 4u + 3u] >= 128 ? 1u : 0u;
            }
        }
        const double fraction = static_cast<double>(covered) / static_cast<double>(cell_pixels);
        result.cells[cell] = fraction;
        result.min_cell = std::min(result.min_cell, fraction);
        result.max_cell = std::max(result.max_cell, fraction);
        total += covered;
    }
    result.coverage = static_cast<double>(total) / static_cast<double>(bytes / 4u);
    result.state = result.coverage <= kImpostorEmptyCoverage    ? ImpostorCoverageState::Empty
                   : result.coverage >= kImpostorOpaqueCoverage ? ImpostorCoverageState::Opaque
                                                                : ImpostorCoverageState::Accepted;
    return result;
}

} // namespace Luminumbra::Rendering
