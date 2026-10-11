#pragma once

#include "../world/ActiveRegionLedger.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::world {

// World-anchored sparse field pages (S6). A cell is 24 m square; a page is 16x16 cells. A cell is
// OWNED by the region (512 m square) that contains its centre, and only the owner's due ticks may
// change it. Nothing here reads a wall clock, a player position or a floating point value.
struct FieldPageKey {
    std::int32_t x = 0;
    std::int32_t z = 0;
    auto operator<=>(const FieldPageKey&) const = default;
};

struct FieldPage {
    static constexpr std::size_t kCells = 256; // 16 x 16, index = local_z * 16 + local_x
    // Bit i of word i / 64 set = cell i is initialized. Uninitialized cells hold zero.
    std::array<std::uint64_t, 4> mask{};
    // Channel-major: cells[channel][cell].
    std::array<std::array<std::uint16_t, kCells>, 5> cells{};
    bool operator==(const FieldPage&) const = default;
};

class FieldPageState {
public:
    static constexpr std::int64_t kCellMm = 24'000;
    static constexpr std::int32_t kPageCells = 16;
    static constexpr int kChannels = 5; // wind_x, wind_z, moisture, front, charge (all u16)
    // Seed epoch = tick / kCadenceTicks (the relaxation target changes once per 8 world ticks).
    static constexpr std::uint64_t kCadenceTicks = 8;
    static constexpr std::uint64_t kSeedOffset = 39;
    static constexpr unsigned kRelaxShift = 4;
    static constexpr std::size_t kMaxPages = 16384;
    // header 12 + pages * 2600 + footer 8
    static constexpr std::size_t kMaxFileBytes = 12 + kMaxPages * 2600 + 8;
    // Valid cell indices (interval intersection, never per-endpoint clamping): the centre must
    // satisfy -16'777'216'000 <= centre_mm < 16'777'216'000.
    static constexpr std::int32_t kMinCell = -699'051;
    static constexpr std::int32_t kMaxCell = 699'050;
    static constexpr const char* kRelativePath = "chunks/region/field-pages.fpg";

    // Region (512 m square, floor division) that owns the cell with this index.
    [[nodiscard]] static RegionKey region_of_cell(std::int32_t cell_x, std::int32_t cell_z);
    // Every valid cell owned by `key`, ascending by (cell_z, cell_x). Empty for a key whose cells
    // all fall outside [kMinCell, kMaxCell].
    [[nodiscard]] static std::vector<std::pair<std::int32_t, std::int32_t>>
    cells_of_region(RegionKey key);
    // Pure seed function: splitmix64 chain, see s6-impl.md.
    [[nodiscard]] static std::uint16_t seed_value(std::uint64_t world_seed,
                                                  int channel,
                                                  std::int32_t cell_x,
                                                  std::int32_t cell_z,
                                                  std::uint64_t epoch);

    [[nodiscard]] bool empty() const {
        return m_pages.empty();
    }
    [[nodiscard]] std::size_t page_count() const {
        return m_pages.size();
    }
    [[nodiscard]] const std::map<FieldPageKey, FieldPage>& pages() const {
        return m_pages;
    }
    void clear() {
        m_pages.clear();
    }

    // True when the cell exists and is initialized.
    [[nodiscard]] bool initialized(std::int32_t cell_x, std::int32_t cell_z) const;
    // Reads one channel of an initialized cell. False (out untouched) otherwise.
    [[nodiscard]] bool
    sample(int channel, std::int32_t cell_x, std::int32_t cell_z, std::uint16_t& out) const;
    // Number of initialized cells owned by `key`.
    [[nodiscard]] std::size_t owned_initialized_cells(RegionKey key) const;
    [[nodiscard]] bool owns_region(RegionKey key) const {
        return owned_initialized_cells(key) != 0;
    }

    // Seeds every uninitialized cell owned by `key` with seed_value(.., tick / kCadenceTicks).
    // Initialized cells are never touched. Returns false and changes nothing (existing cells are
    // preserved) when creating the missing pages would exceed kMaxPages.
    bool activate_region(RegionKey key, std::uint64_t world_seed, std::uint64_t tick);
    // One step per CALL (the caller calls it once per due tick of the region, so Reduced regions
    // step less often): for every initialized cell owned by `key` and every channel,
    // v += (int32(target) - int32(v)) >> kRelaxShift (arithmetic shift) with target =
    // seed_value(.., tick / kCadenceTicks). Returns the number of cells stepped.
    std::uint64_t step_region(RegionKey key, std::uint64_t world_seed, std::uint64_t tick);

    // FPG1 file bytes. Empty string for an empty store (an empty store writes no file).
    [[nodiscard]] std::string serialize() const;
    // Strict transactional parse; `out` is untouched and `error` set on any failure. Every
    // initialized cell must lie in [kMinCell, kMaxCell] in both axes.
    static bool parse(const std::string& bytes, FieldPageState& out, std::string& error);
    // "" when empty, otherwise "field_pages:v1:" + serialize().
    [[nodiscard]] std::string canonical_bytes() const;
    // "" when `key` owns no initialized cell, otherwise for every owned initialized cell in
    // cells_of_region order: i32 cell_x, i32 cell_z, then kChannels u16 (all little endian).
    [[nodiscard]] std::string region_bytes(RegionKey key) const;

private:
    std::map<FieldPageKey, FieldPage> m_pages;
};

} // namespace Luminumbra::world
