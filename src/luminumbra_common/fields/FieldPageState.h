#pragma once

#include "../world/ActiveRegionLedger.h"
#include "FieldCellTypes.h"
#include "FoliageFieldPages.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::world {

// World-anchored sparse field pages (S6). A cell is 24 m square; a page is 16x16 cells. A cell is
// OWNED by the region (512 m square) that contains its centre, and only the owner's due ticks may
// change it. Nothing here reads a wall clock, a player position or a floating point value.
struct FieldPage {
    static constexpr std::size_t kCells = 256; // 16 x 16, index = local_z * 16 + local_x
    // Bit i of word i / 64 set = cell i is initialized. Uninitialized cells hold zero.
    std::array<std::uint64_t, 4> mask{};
    // Channel-major: cells[channel][cell].
    std::array<std::array<std::uint16_t, kCells>, 5> cells{};
    bool operator==(const FieldPage&) const = default;
};

// Exact evolution state. Binary32/binary64 bits are encoded explicitly; no compiler struct layout
// appears in the file. Owners and active ages keep frozen state independent of absolute time.
struct RegionalStorm {
    RegionKey owner;
    std::uint32_t center_x = 0, center_z = 0, velocity_x = 0, velocity_z = 0, intensity = 0;
    std::uint64_t spawn_tick = 0;
    std::uint32_t lifetime_ticks = 0, seed_salt = 0, active_age = 0;
    bool operator==(const RegionalStorm&) const = default;
};
struct RegionalStrike {
    RegionKey owner;
    std::uint64_t strike_tick = 0;
    std::uint32_t world_x = 0, world_z = 0, magnitude = 0, storm_salt = 0, active_age = 0;
    bool operator==(const RegionalStrike&) const = default;
};
using ScentCell = std::array<std::uint64_t, 4>; // exact finite, nonnegative binary64 concentrations

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
    static constexpr std::size_t kMaxStorms = 65536, kMaxStrikes = 65536, kMaxScentCells = 65536;
    static constexpr std::size_t kMaxOwnerStorms = 16, kMaxOwnerStrikes = 32;
    static constexpr std::size_t kMaxWeatherClocks = 65536;
    static constexpr std::size_t kMaxFileBytes = 64u * 1024u * 1024u;
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
        return m_pages.empty() && m_storms.empty() && m_strikes.empty() && m_scent.empty() &&
               m_weatherClocks.empty() && m_soil.empty() && m_irrigation.empty();
    }
    [[nodiscard]] std::size_t page_count() const {
        return m_pages.size();
    }
    [[nodiscard]] const std::map<FieldPageKey, FieldPage>& pages() const {
        return m_pages;
    }
    void clear() {
        m_pages.clear();
        m_storms.clear();
        m_strikes.clear();
        m_ownerStorms.clear();
        m_ownerStrikes.clear();
        m_scent.clear();
        m_weatherClocks.clear();
        m_soil = {};
        m_irrigation = {};
    }

    [[nodiscard]] const std::vector<RegionalStorm>& storms() const {
        return m_storms;
    }
    [[nodiscard]] const std::vector<RegionalStrike>& strikes() const {
        return m_strikes;
    }
    [[nodiscard]] const std::map<ScentCellKey, ScentCell>& scent_cells() const {
        return m_scent;
    }
    [[nodiscard]] const std::map<RegionKey, std::uint64_t>& weather_clocks() const {
        return m_weatherClocks;
    }
    // Indexed authority probes avoid traversing frozen owners on idle ticks.
    [[nodiscard]] bool owns_weather(RegionKey owner) const {
        return m_weatherClocks.contains(owner) || m_ownerStorms.contains(owner) ||
               m_ownerStrikes.contains(owner);
    }
    [[nodiscard]] std::span<const std::size_t> storm_indices(RegionKey owner) const {
        const auto found = m_ownerStorms.find(owner);
        return found == m_ownerStorms.end() ? std::span<const std::size_t>{} : found->second;
    }
    // Transfer the already staged map into advancement; do not copy it again.
    [[nodiscard]] std::map<RegionKey, std::uint64_t> take_weather_clocks() noexcept {
        return std::move(m_weatherClocks);
    }
    static constexpr std::int32_t kSoilBaseline = 1000000, kIrrigationLimit = 4000000;
    [[nodiscard]] const FoliageFieldPages& soil() const {
        return m_soil;
    }
    [[nodiscard]] const FoliageFieldPages& irrigation() const {
        return m_irrigation;
    }
    // Foliage evolution and admission write only soil/irrigation. Retain these
    // channels for both border reads and rollback, without copying ambient state.
    [[nodiscard]] FieldPageState foliage_snapshot() const;
    void restore_foliage(FieldPageState&& snapshot) noexcept;
    bool set_soil(const ScentCellKey& cell, std::int32_t value) {
        return m_soil.set(cell, value, kSoilBaseline);
    }
    bool set_irrigation(const ScentCellKey& cell, std::int32_t value) {
        return m_irrigation.set(cell, value, kIrrigationLimit);
    }
    bool set_weather_age(RegionKey owner, std::uint64_t active_age);
    // Transactional admission, canonical ordering and bounded exact-state validation.
    bool replace_weather(std::vector<RegionalStorm> storms, std::vector<RegionalStrike> strikes);
    bool replace_weather(std::vector<RegionalStorm> storms,
                         std::vector<RegionalStrike> strikes,
                         std::map<RegionKey, std::uint64_t> clocks);
    bool set_scent(ScentCellKey key, const ScentCell& values);
    // Same admissions as set_scent, committed together. Keys must be strictly ordered by
    // (z, x), as in the near-domain capture. Only newly admitted cells allocate map nodes.
    bool update_scent(std::span<const std::pair<ScentCellKey, ScentCell>> updates);
    // Column-major near window, at most 128 x 128 cells. Updates only changed cells,
    // preserves outside authority, and reports the same per-owner work as sparse admissions.
    // The window must intersect the scent domain; cells beyond its edge must be zero.
    // Invalid input/capacity refusal leaves authority and work unchanged.
    bool capture_scent_window(ScentCellKey origin,
                              int side,
                              std::span<const ScentCell> cells,
                              std::map<RegionKey, std::uint64_t>& work);
    [[nodiscard]] static RegionKey region_of_scent_cell(ScentCellKey key);
    [[nodiscard]] bool owns_evolution_state(RegionKey key) const;

    // True when the cell exists and is initialized.
    [[nodiscard]] bool initialized(std::int32_t cell_x, std::int32_t cell_z) const;
    // Reads one channel of an initialized cell. False (out untouched) otherwise.
    [[nodiscard]] bool
    sample(int channel, std::int32_t cell_x, std::int32_t cell_z, std::uint16_t& out) const;
    // Number of initialized cells owned by `key`.
    [[nodiscard]] std::size_t owned_initialized_cells(RegionKey key) const;
    [[nodiscard]] bool owns_region(RegionKey key) const {
        return owned_initialized_cells(key) != 0 || owns_evolution_state(key);
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

    // Sealed page-only input for step_scheduled, captured BEFORE activation. Retains only
    // pages that can participate this tick; excludes scent, weather and frozen-only pages.
    [[nodiscard]] FieldPageState
    scheduled_snapshot(const std::map<RegionKey, ActiveRegionRecord>& records,
                       std::span<const RegionKey> due,
                       std::uint64_t tick) const;

    // One sealed previous-state pass. Interiors follow the due list; cross-owner edges follow
    // global common phase independently of each owner's interior phase. Frozen or absent
    // neighbours never exchange. Counts include each actual interior/edge update per owner.
    [[nodiscard]] std::map<RegionKey, std::uint64_t>
    step_scheduled(const std::map<RegionKey, ActiveRegionRecord>& records,
                   std::span<const RegionKey> due,
                   std::uint64_t world_seed,
                   std::uint64_t tick,
                   const FieldPageState* previous_tick = nullptr);

    // Commits the stepped pages of a scheduled-step candidate built from scheduled_snapshot.
    // Overwrites exactly the pages held by `staged` and leaves every other page and all
    // evolution state untouched. Allocation-free and non-throwing: it only assigns values into
    // pages that already exist.
    void adopt_scheduled_pages(FieldPageState&& staged) noexcept;
    // Stage only weather authority and its owner indices; never copy unrelated pages or fields.
    void copy_weather_from(const FieldPageState& source);
    // Commit validated candidate weather together with its indices, without allocating.
    void adopt_weather_state(FieldPageState& staged) noexcept;

    // FPG1 file bytes. Empty string for an empty store (an empty store writes no file).
    [[nodiscard]] std::string serialize() const;
    // Strict transactional parse; `out` is untouched and `error` set on any failure. Every
    // initialized cell must lie in [kMinCell, kMaxCell] in both axes.
    static bool parse(const std::string& bytes, FieldPageState& out, std::string& error);
    // "" when empty, otherwise "field_pages:v2:" + serialize().
    [[nodiscard]] std::string canonical_bytes() const;
    [[nodiscard]] std::string ambient_bytes(std::uint8_t channels,
                                            bool weather = false,
                                            const FieldPageState* overlay = nullptr) const;
    // For every owned initialized cell in cells_of_region order: i32 cell_x, i32 cell_z, then
    // kChannels u16 (all little endian). When the region owns storms, strikes, scent cells or a
    // weather clock, an "|evolution:v2:" suffix follows with those sections. "" only when the
    // region owns neither initialized cells nor evolution state. Soil and irrigation
    // append a "|foliage:v1:" suffix containing their owned exact integer cells.
    [[nodiscard]] std::string region_bytes(RegionKey key) const;

private:
    std::map<FieldPageKey, FieldPage> m_pages;
    std::vector<RegionalStorm> m_storms;
    std::vector<RegionalStrike> m_strikes;
    // Indices retain canonical vector order, bounded by kMaxStorms/kMaxStrikes in total and
    // kMaxOwnerStorms/kMaxOwnerStrikes per owner. Rebuilt atomically with weather admission.
    std::map<RegionKey, std::vector<std::size_t>> m_ownerStorms, m_ownerStrikes;
    std::map<ScentCellKey, ScentCell> m_scent;
    std::map<RegionKey, std::uint64_t> m_weatherClocks;
    FoliageFieldPages m_soil, m_irrigation;
};

// Borrowed tick view: candidate pages override live pages, and candidate weather is authoritative.
// Neither store is copied or modified. The candidate must outlive the view.
class FieldPageView {
public:
    FieldPageView(const FieldPageState& live, const FieldPageState* candidate)
        : m_live(live)
        , m_candidate(candidate) {}
    [[nodiscard]] bool
    sample(int channel, std::int32_t x, std::int32_t z, std::uint16_t& out) const {
        return (m_candidate && m_candidate->sample(channel, x, z, out)) ||
               m_live.sample(channel, x, z, out);
    }
    [[nodiscard]] const std::vector<RegionalStorm>& storms() const {
        return weather().storms();
    }
    [[nodiscard]] const std::vector<RegionalStrike>& strikes() const {
        return weather().strikes();
    }
    [[nodiscard]] std::string ambient_bytes(std::uint8_t channels, bool include_weather) const {
        return m_live.ambient_bytes(channels, include_weather, m_candidate);
    }

private:
    [[nodiscard]] const FieldPageState& weather() const {
        return m_candidate ? *m_candidate : m_live;
    }
    const FieldPageState& m_live;
    const FieldPageState* m_candidate;
};

} // namespace Luminumbra::world
