#pragma once

// Minimal shared field-storage surface for wind and scalar energy fields.
//
// The wind field and scalar energy field
// both need the same coarse, region-following 2.5D cell-grid plumbing: a flat
// row-major cell store with a fixed cell size + stride, an origin that follows
// the streamed region, and a canonical iteration order for snapshot/sub-hash.
// This header factors only the common storage surface; it is not a solver.
// WindFieldSystem and the energy-field stack supply their own update rules.
//
// DETERMINISM. FieldGrid is pure storage: flat std::vector<T>, integer indexing,
// no floating-point ops, no hashed-container iteration, no wall-clock, no RNG.
// The canonical cell order (z-major, then x ascending) is the snapshot/sub-hash
// order; it is stable across machines because it is integer-derived only.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace luminumbra::fields {

// Coarse 2.5D cell grid shared by the wind field (vector cells) and the
// scalar energy field (scalar cells). T is the per-cell payload
// (e.g. a per-layer wind vector struct, or a float). The grid covers a square
// region of `extent_cells` x `extent_cells` cells of `cell_size_m` metres each,
// anchored at an integer cell origin that follows the streamed region.
template<typename T>
class FieldGrid {
public:
    FieldGrid() = default;

    FieldGrid(int extent_cells, float cell_size_m)
        : extent_cells_(extent_cells > 0 ? extent_cells : 0)
        , cell_size_m_(cell_size_m) {
        cells_.assign(cell_count(), T{});
    }

    [[nodiscard]] int extent_cells() const noexcept {
        return extent_cells_;
    }
    [[nodiscard]] float cell_size_m() const noexcept {
        return cell_size_m_;
    }
    [[nodiscard]] std::size_t stride() const noexcept {
        return static_cast<std::size_t>(extent_cells_);
    }
    [[nodiscard]] std::size_t cell_count() const noexcept {
        return static_cast<std::size_t>(extent_cells_) * static_cast<std::size_t>(extent_cells_);
    }

    // Integer cell origin (in cell units) the grid is currently anchored at.
    // The grid follows the streamed region by moving this origin; cell math is
    // pure integer so the anchor never introduces FP drift.
    [[nodiscard]] std::int64_t origin_cell_x() const noexcept {
        return origin_cell_x_;
    }
    [[nodiscard]] std::int64_t origin_cell_z() const noexcept {
        return origin_cell_z_;
    }
    void set_origin_cells(std::int64_t cx, std::int64_t cz) noexcept {
        origin_cell_x_ = cx;
        origin_cell_z_ = cz;
    }

    // Canonical row-major index (z-major, x ascending) -- the ONE iteration
    // order used for both updates and the snapshot/sub-hash, so the hash is
    // reproducible. Caller guarantees 0 <= lx,lz < extent_cells.
    [[nodiscard]] std::size_t index(int lx, int lz) const noexcept {
        return static_cast<std::size_t>(lz) * stride() + static_cast<std::size_t>(lx);
    }

    [[nodiscard]] bool in_bounds(int lx, int lz) const noexcept {
        return lx >= 0 && lz >= 0 && lx < extent_cells_ && lz < extent_cells_;
    }

    [[nodiscard]] T& at(int lx, int lz) {
        return cells_[index(lx, lz)];
    }
    [[nodiscard]] const T& at(int lx, int lz) const {
        return cells_[index(lx, lz)];
    }

    [[nodiscard]] std::vector<T>& cells() noexcept {
        return cells_;
    }
    [[nodiscard]] const std::vector<T>& cells() const noexcept {
        return cells_;
    }

    void reset(const T& value) {
        for (T& cell : cells_) {
            cell = value;
        }
    }

private:
    int extent_cells_ = 0;
    float cell_size_m_ = 0.0f;
    std::int64_t origin_cell_x_ = 0;
    std::int64_t origin_cell_z_ = 0;
    std::vector<T> cells_;
};

} // namespace luminumbra::fields
