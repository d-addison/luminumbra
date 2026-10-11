#include "FieldPageState.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Luminumbra::world {
namespace {

using CellList = std::vector<std::pair<std::int32_t, std::int32_t>>;
using PageMap = std::map<FieldPageKey, FieldPage>;

constexpr std::int64_t kCellMm = FieldPageState::kCellMm;
constexpr std::int64_t kHalfCellMm = kCellMm / 2;
constexpr std::int64_t kRegionMm = 512'000;
constexpr std::int64_t kPageSpan = FieldPageState::kPageCells;
constexpr std::int64_t kMinCellIndex = FieldPageState::kMinCell;
constexpr std::int64_t kMaxCellIndex = FieldPageState::kMaxCell;

// FPG1 v2: magic/version/channel mask/section count, ascending typed sections, FNV footer.
constexpr const char* kMagic = "FPG1";
constexpr std::uint64_t kFormatVersion = 2;
constexpr std::uint64_t kChannelCount = 5;
constexpr std::uint64_t kChannelMask = 31;
constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kFooterBytes = 8;
// key (i32 x, i32 z) + 4 x u64 mask + 5 x 256 x u16 cells.
constexpr std::size_t kPageKeyBytes = 8;
constexpr std::size_t kMaskBytes = 32;
constexpr std::size_t kPageBytes = kPageKeyBytes + kMaskBytes + 2 * 5 * FieldPage::kCells;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

static_assert(FieldPageState::kChannels == static_cast<int>(kChannelCount));
static_assert(kPageBytes == 2600);
// A cell's page and its bit / slot position inside that page.
struct CellSlot {
    FieldPageKey key;
    std::size_t index = 0;
};

struct CellRange {
    std::int64_t lo = 0;
    std::int64_t hi = -1;
};

std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor) {
    std::int64_t quotient = value / divisor;
    if (value % divisor != 0 && value < 0) {
        --quotient;
    }
    return quotient;
}

// Rounds toward +infinity for a positive divisor.
std::int64_t CeilDiv(std::int64_t value, std::int64_t divisor) {
    return -FloorDiv(-value, divisor);
}

std::int32_t RegionAxisOfCell(std::int32_t cell) {
    const std::int64_t centre = static_cast<std::int64_t>(cell) * kCellMm + kHalfCellMm;
    return static_cast<std::int32_t>(FloorDiv(centre, kRegionMm));
}

// Cells on one axis whose centre lies in [region * 512 m, (region + 1) * 512 m), intersected with
// the valid cell range. Endpoints are intersected, never clamped independently.
CellRange AxisCellRange(std::int32_t region) {
    const std::int64_t start = static_cast<std::int64_t>(region) * kRegionMm;
    const std::int64_t raw_lo = CeilDiv(start - kHalfCellMm, kCellMm);
    const std::int64_t raw_hi = CeilDiv(start + kRegionMm - kHalfCellMm, kCellMm) - 1;
    return CellRange{std::max(raw_lo, kMinCellIndex), std::min(raw_hi, kMaxCellIndex)};
}

CellSlot CellSlotOf(std::int32_t cell_x, std::int32_t cell_z) {
    const std::int64_t x = cell_x;
    const std::int64_t z = cell_z;
    const std::int64_t page_x = FloorDiv(x, kPageSpan);
    const std::int64_t page_z = FloorDiv(z, kPageSpan);
    const std::int64_t local_x = x - page_x * kPageSpan;
    const std::int64_t local_z = z - page_z * kPageSpan;
    CellSlot slot;
    slot.key = FieldPageKey{static_cast<std::int32_t>(page_x), static_cast<std::int32_t>(page_z)};
    slot.index = static_cast<std::size_t>(local_z * kPageSpan + local_x);
    return slot;
}

bool CellBit(const FieldPage& page, std::size_t index) {
    return ((page.mask[index / 64] >> (index % 64)) & 1U) != 0;
}

void SetCellBit(FieldPage& page, std::size_t index) {
    page.mask[index / 64] |= std::uint64_t{1} << (index % 64);
}

const FieldPage* InitializedPage(const PageMap& pages, const CellSlot& slot) {
    const auto found = pages.find(slot.key);
    if (found == pages.end() || !CellBit(found->second, slot.index)) {
        return nullptr;
    }
    return &found->second;
}

FieldPage* InitializedPage(PageMap& pages, const CellSlot& slot) {
    const auto found = pages.find(slot.key);
    if (found == pages.end() || !CellBit(found->second, slot.index)) {
        return nullptr;
    }
    return &found->second;
}

std::size_t CountMissingPages(const PageMap& pages, const CellList& cells) {
    std::set<FieldPageKey> missing;
    for (const auto& [cell_x, cell_z] : cells) {
        const CellSlot slot = CellSlotOf(cell_x, cell_z);
        if (pages.find(slot.key) == pages.end()) {
            missing.insert(slot.key);
        }
    }
    return missing.size();
}

std::uint64_t Fin(std::uint64_t value) {
    std::uint64_t z = value + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void SeedCell(FieldPage& page,
              std::size_t index,
              std::int32_t cell_x,
              std::int32_t cell_z,
              std::uint64_t world_seed,
              std::uint64_t epoch) {
    SetCellBit(page, index);
    for (int channel = 0; channel < FieldPageState::kChannels; ++channel) {
        page.cells[static_cast<std::size_t>(channel)][index] =
            FieldPageState::seed_value(world_seed, channel, cell_x, cell_z, epoch);
    }
}

std::int32_t ChannelValue(int channel, std::uint16_t word) {
    return channel < 2 ? static_cast<std::int32_t>(std::bit_cast<std::int16_t>(word))
                       : static_cast<std::int32_t>(word);
}
std::uint16_t RelaxToward(int channel, std::uint16_t value, std::uint16_t target) {
    const auto current = ChannelValue(channel, value);
    const auto goal = ChannelValue(channel, target);
    return static_cast<std::uint16_t>(current + ((goal - current) >> FieldPageState::kRelaxShift));
}

void StepCell(FieldPage& page,
              std::size_t index,
              std::int32_t cell_x,
              std::int32_t cell_z,
              std::uint64_t world_seed,
              std::uint64_t epoch) {
    for (int channel = 0; channel < FieldPageState::kChannels; ++channel) {
        std::uint16_t& value = page.cells[static_cast<std::size_t>(channel)][index];
        const std::uint16_t target =
            FieldPageState::seed_value(world_seed, channel, cell_x, cell_z, epoch);
        value = RelaxToward(channel, value, target);
    }
}

void AppendLe(std::string& out, std::uint64_t value, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i) {
        out += static_cast<char>(static_cast<unsigned char>((value >> (8 * i)) & 0xFFU));
    }
}

std::uint64_t ReadLe(const std::string& bytes, std::size_t offset, std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(bytes[offset + i]);
        value |= static_cast<std::uint64_t>(byte) << (8 * i);
    }
    return value;
}

std::uint64_t Fnv1a64(std::string_view bytes) {
    std::uint64_t hash = kFnvOffset;
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        hash *= kFnvPrime;
    }
    return hash;
}

void AppendCells(std::string& out, const FieldPage& page) {
    for (const auto& channel : page.cells) {
        for (const std::uint16_t value : channel) {
            AppendLe(out, value, 2);
        }
    }
}

void AppendPage(std::string& out, const FieldPageKey& key, const FieldPage& page) {
    AppendLe(out, static_cast<std::uint32_t>(key.x), 4);
    AppendLe(out, static_cast<std::uint32_t>(key.z), 4);
    for (const std::uint64_t word : page.mask) {
        AppendLe(out, word, 8);
    }
    AppendCells(out, page);
}

// Pure decode of one page record at `offset`. Validation happens separately.
void DecodePage(const std::string& bytes, std::size_t offset, FieldPageKey& key, FieldPage& page) {
    key.x = static_cast<std::int32_t>(static_cast<std::uint32_t>(ReadLe(bytes, offset, 4)));
    key.z = static_cast<std::int32_t>(static_cast<std::uint32_t>(ReadLe(bytes, offset + 4, 4)));
    std::size_t position = offset + kPageKeyBytes;
    for (std::uint64_t& word : page.mask) {
        word = ReadLe(bytes, position, 8);
        position += 8;
    }
    for (auto& channel : page.cells) {
        for (std::uint16_t& value : channel) {
            value = static_cast<std::uint16_t>(ReadLe(bytes, position, 2));
            position += 2;
        }
    }
}

bool MaskIsEmpty(const FieldPage& page) {
    return std::all_of(
        page.mask.begin(), page.mask.end(), [](std::uint64_t word) { return word == 0; });
}

bool ChannelsZeroAt(const FieldPage& page, std::size_t index) {
    return std::all_of(page.cells.begin(), page.cells.end(), [index](const auto& channel) {
        return channel[index] == 0;
    });
}

// Global cell index of local index `local` on the page at `page_coord`, in int64.
std::int64_t GlobalCell(std::int32_t page_coord, std::size_t local) {
    return static_cast<std::int64_t>(page_coord) * kPageSpan + static_cast<std::int64_t>(local);
}

bool CellWithinRange(const FieldPageKey& key, std::size_t index) {
    constexpr std::size_t kSide = 16;
    const std::size_t local_x = index % kSide;
    const std::size_t local_z = index / kSide;
    const std::int64_t cell_x = GlobalCell(key.x, local_x);
    const std::int64_t cell_z = GlobalCell(key.z, local_z);
    return cell_x >= kMinCellIndex && cell_x <= kMaxCellIndex && cell_z >= kMinCellIndex &&
           cell_z <= kMaxCellIndex;
}

// One cell: an initialized cell must lie in range; an uninitialized cell must hold zeros.
bool CheckCell(const FieldPageKey& key,
               const FieldPage& page,
               std::size_t index,
               std::string& error) {
    if (!CellBit(page, index)) {
        if (!ChannelsZeroAt(page, index)) {
            error = "field pages: value set on an uninitialized cell";
            return false;
        }
        return true;
    }
    if (!CellWithinRange(key, index)) {
        error = "field pages: initialized cell outside the valid range";
        return false;
    }
    return true;
}

bool CheckPage(const FieldPageKey& key, const FieldPage& page, std::string& error) {
    if (MaskIsEmpty(page)) {
        error = "field pages: page with an empty mask";
        return false;
    }
    for (std::size_t index = 0; index < FieldPage::kCells; ++index) {
        if (!CheckCell(key, page, index, error)) {
            return false;
        }
    }
    return true;
}

// Decodes and validates every page record in key order into `pages`.
bool DecodePages(const std::string& bytes,
                 std::size_t page_count,
                 std::size_t records_at,
                 PageMap& pages,
                 std::string& error) {
    FieldPageKey previous;
    for (std::size_t index = 0; index < page_count; ++index) {
        FieldPageKey key;
        FieldPage page;
        DecodePage(bytes, records_at + index * kPageBytes, key, page);
        if (index > 0 && key <= previous) {
            error = "field pages: keys not strictly ascending";
            return false;
        }
        if (!CheckPage(key, page, error)) {
            return false;
        }
        pages.emplace_hint(pages.end(), key, std::move(page));
        previous = key;
    }
    return true;
}

bool ValidOwner(RegionKey key) {
    return key.x >= -32768 && key.x < 32768 && key.z >= -32768 && key.z < 32768;
}
bool ValidPosition(std::uint32_t bits) {
    const float value = std::bit_cast<float>(bits);
    return std::isfinite(value) && value >= -16777216.0f && value < 16777216.0f;
}
bool ValidUnit(std::uint32_t bits) {
    const float value = std::bit_cast<float>(bits);
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}
RegionKey PositionOwner(std::uint32_t x, std::uint32_t z) {
    return {
        static_cast<std::int32_t>(std::floor(static_cast<double>(std::bit_cast<float>(x)) / 512.0)),
        static_cast<std::int32_t>(
            std::floor(static_cast<double>(std::bit_cast<float>(z)) / 512.0))};
}
bool ValidStorm(const RegionalStorm& storm) {
    return ValidOwner(storm.owner) && ValidPosition(storm.center_x) &&
           ValidPosition(storm.center_z) && std::isfinite(std::bit_cast<float>(storm.velocity_x)) &&
           std::isfinite(std::bit_cast<float>(storm.velocity_z)) && ValidUnit(storm.intensity) &&
           std::abs(std::bit_cast<float>(storm.velocity_x)) <= 32.0f &&
           std::abs(std::bit_cast<float>(storm.velocity_z)) <= 32.0f && storm.lifetime_ticks != 0 &&
           storm.active_age < storm.lifetime_ticks && storm.spawn_tick < WorldClock::kTickLimit &&
           PositionOwner(storm.center_x, storm.center_z) == storm.owner;
}
bool ValidStrike(const RegionalStrike& strike) {
    return ValidOwner(strike.owner) && ValidPosition(strike.world_x) &&
           ValidPosition(strike.world_z) && ValidUnit(strike.magnitude) &&
           strike.strike_tick < WorldClock::kTickLimit && strike.active_age <= 1 &&
           PositionOwner(strike.world_x, strike.world_z) == strike.owner;
}
auto StormOrder(const RegionalStorm& storm) {
    return std::tuple(storm.spawn_tick, storm.seed_salt);
}
auto StrikeOrder(const RegionalStrike& strike) {
    return std::tuple(strike.strike_tick, strike.storm_salt, strike.world_x, strike.world_z);
}
bool ValidScent(ScentCellKey key, const ScentCell& cell) {
    if (key.x < -16777216 || key.x >= 16777216 || key.z < -16777216 || key.z >= 16777216)
        return false;
    for (const auto bits : cell) {
        const double value = std::bit_cast<double>(bits);
        // Negative zero is an alternate empty encoding, so refuse it explicitly.
        if (!std::isfinite(value) || value < 0 || value > 1.0e6 || (value == 0 && bits != 0))
            return false;
    }
    return true;
}
void AppendOwner(std::string& out, RegionKey key) {
    AppendLe(out, static_cast<std::uint32_t>(key.x), 4);
    AppendLe(out, static_cast<std::uint32_t>(key.z), 4);
}
void AppendStorm(std::string& out, const RegionalStorm& storm) {
    AppendOwner(out, storm.owner);
    for (const auto bits :
         {storm.center_x, storm.center_z, storm.velocity_x, storm.velocity_z, storm.intensity})
        AppendLe(out, bits, 4);
    AppendLe(out, storm.spawn_tick, 8);
    for (const auto value : {storm.lifetime_ticks, storm.seed_salt, storm.active_age})
        AppendLe(out, value, 4);
}
void AppendStrike(std::string& out, const RegionalStrike& strike) {
    AppendOwner(out, strike.owner);
    AppendLe(out, strike.strike_tick, 8);
    for (const auto value :
         {strike.world_x, strike.world_z, strike.magnitude, strike.storm_salt, strike.active_age})
        AppendLe(out, value, 4);
}
void AppendScent(std::string& out, ScentCellKey key, const ScentCell& cell) {
    AppendLe(out, static_cast<std::uint32_t>(key.x), 4);
    AppendLe(out, static_cast<std::uint32_t>(key.z), 4);
    for (const auto bits : cell)
        AppendLe(out, bits, 8);
}
void AppendSection(std::string& out,
                   std::uint16_t type,
                   std::size_t count,
                   const std::string& records) {
    if (count == 0)
        return;
    AppendLe(out, type, 2);
    AppendLe(out, records.size() + 4, 4);
    AppendLe(out, count, 4);
    out += records;
}
class SectionReader {
public:
    SectionReader(const std::string& bytes, std::size_t at)
        : m_bytes(bytes)
        , m_at(at) {}
    std::uint64_t read(std::size_t width) {
        const auto result = ReadLe(m_bytes, m_at, width);
        m_at += width;
        return result;
    }
    std::uint32_t word() {
        return static_cast<std::uint32_t>(read(4));
    }
    RegionKey owner() {
        return {std::bit_cast<std::int32_t>(word()), std::bit_cast<std::int32_t>(word())};
    }

private:
    const std::string& m_bytes;
    std::size_t m_at;
};

} // namespace

RegionKey FieldPageState::region_of_cell(std::int32_t cell_x, std::int32_t cell_z) {
    return RegionKey{RegionAxisOfCell(cell_x), RegionAxisOfCell(cell_z)};
}

CellList FieldPageState::cells_of_region(RegionKey key) {
    CellList out;
    const CellRange x_range = AxisCellRange(key.x);
    const CellRange z_range = AxisCellRange(key.z);
    if (x_range.lo > x_range.hi || z_range.lo > z_range.hi) {
        return out;
    }
    for (std::int64_t cell_z = z_range.lo; cell_z <= z_range.hi; ++cell_z) {
        for (std::int64_t cell_x = x_range.lo; cell_x <= x_range.hi; ++cell_x) {
            out.emplace_back(static_cast<std::int32_t>(cell_x), static_cast<std::int32_t>(cell_z));
        }
    }
    return out;
}

std::uint16_t FieldPageState::seed_value(std::uint64_t world_seed,
                                         int channel,
                                         std::int32_t cell_x,
                                         std::int32_t cell_z,
                                         std::uint64_t epoch) {
    const auto x_bits = static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell_x));
    const auto z_bits = static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell_z));
    std::uint64_t h = Fin(world_seed + kSeedOffset);
    h = Fin(h ^ static_cast<std::uint64_t>(channel));
    h = Fin(h ^ x_bits);
    h = Fin(h ^ z_bits);
    h = Fin(h ^ epoch);
    return static_cast<std::uint16_t>(h >> 48);
}

bool FieldPageState::initialized(std::int32_t cell_x, std::int32_t cell_z) const {
    return InitializedPage(m_pages, CellSlotOf(cell_x, cell_z)) != nullptr;
}

bool FieldPageState::sample(int channel,
                            std::int32_t cell_x,
                            std::int32_t cell_z,
                            std::uint16_t& out) const {
    if (channel < 0 || channel >= kChannels) {
        return false;
    }
    const CellSlot slot = CellSlotOf(cell_x, cell_z);
    const FieldPage* page = InitializedPage(m_pages, slot);
    if (page == nullptr) {
        return false;
    }
    out = page->cells[static_cast<std::size_t>(channel)][slot.index];
    return true;
}

std::size_t FieldPageState::owned_initialized_cells(RegionKey key) const {
    std::size_t count = 0;
    for (const auto& [cell_x, cell_z] : cells_of_region(key)) {
        if (initialized(cell_x, cell_z)) {
            ++count;
        }
    }
    return count;
}

bool FieldPageState::activate_region(RegionKey key, std::uint64_t world_seed, std::uint64_t tick) {
    const CellList cells = cells_of_region(key);
    const std::size_t missing = CountMissingPages(m_pages, cells);
    if (m_pages.size() + missing > kMaxPages) {
        return false;
    }
    const std::uint64_t epoch = tick / kCadenceTicks;
    for (const auto& [cell_x, cell_z] : cells) {
        const CellSlot slot = CellSlotOf(cell_x, cell_z);
        if (InitializedPage(m_pages, slot) != nullptr) {
            continue;
        }
        SeedCell(m_pages[slot.key], slot.index, cell_x, cell_z, world_seed, epoch);
    }
    return true;
}

std::uint64_t
FieldPageState::step_region(RegionKey key, std::uint64_t world_seed, std::uint64_t tick) {
    const std::uint64_t epoch = tick / kCadenceTicks;
    std::uint64_t stepped = 0;
    for (const auto& [cell_x, cell_z] : cells_of_region(key)) {
        const CellSlot slot = CellSlotOf(cell_x, cell_z);
        FieldPage* page = InitializedPage(m_pages, slot);
        if (page == nullptr) {
            continue;
        }
        StepCell(*page, slot.index, cell_x, cell_z, world_seed, epoch);
        ++stepped;
    }
    return stepped;
}

std::map<RegionKey, std::uint64_t>
FieldPageState::step_scheduled(const std::map<RegionKey, ActiveRegionRecord>& records,
                               std::span<const RegionKey> due,
                               std::uint64_t world_seed,
                               std::uint64_t tick,
                               const FieldPageState* previous_tick) {
    const PageMap previous = previous_tick ? previous_tick->m_pages : m_pages;
    const std::set<RegionKey> due_keys(due.begin(), due.end());
    std::map<RegionKey, std::uint64_t> work;
    const auto eligible = [&records](RegionKey owner) {
        const auto found = records.find(owner);
        return found != records.end() && found->second.state != RegionState::Frozen;
    };
    // Seed relaxation uses only the sealed old values, even when an adjacent region's
    // interior phase differs. No consumer observes a partially updated neighbour.
    for (const auto& [page_key, old_page] : previous) {
        for (std::size_t i = 0; i < FieldPage::kCells; ++i) {
            if (!CellBit(old_page, i))
                continue;
            const auto x = page_key.x * kPageCells + static_cast<std::int32_t>(i % kPageCells);
            const auto z = page_key.z * kPageCells + static_cast<std::int32_t>(i / kPageCells);
            const RegionKey owner = region_of_cell(x, z);
            if (!eligible(owner) || !due_keys.contains(owner))
                continue;
            FieldPage& page = m_pages.at(page_key);
            for (int channel = 0; channel < kChannels; ++channel)
                page.cells[channel][i] = static_cast<std::uint16_t>(
                    ChannelValue(channel, old_page.cells[channel][i]) +
                    ((ChannelValue(channel,
                                   seed_value(world_seed, channel, x, z, tick / kCadenceTicks)) -
                      ChannelValue(channel, old_page.cells[channel][i])) >>
                     kRelaxShift));
            ++work[owner];
        }
    }
    // Visit every undirected edge once, in page/cell order, and transfer equal/opposite
    // integer fluxes. The 1/32 edge coefficient plus 1/16 relaxation is a bounded convex
    // update, so all intermediate and final concentrations remain representable as u16.
    for (const auto& [page_key, old_page] : previous) {
        for (std::size_t i = 0; i < FieldPage::kCells; ++i) {
            if (!CellBit(old_page, i))
                continue;
            const auto x = page_key.x * kPageCells + static_cast<std::int32_t>(i % kPageCells);
            const auto z = page_key.z * kPageCells + static_cast<std::int32_t>(i / kPageCells);
            const RegionKey owner = region_of_cell(x, z);
            if (!eligible(owner))
                continue;
            for (const auto [dx, dz] : {std::pair{1, 0}, std::pair{0, 1}}) {
                const CellSlot neighbour = CellSlotOf(x + dx, z + dz);
                const auto* old_neighbour = InitializedPage(previous, neighbour);
                const RegionKey other = region_of_cell(x + dx, z + dz);
                if (!old_neighbour || !eligible(other))
                    continue;
                if (owner == other) {
                    if (!due_keys.contains(owner))
                        continue;
                } else {
                    const auto shift =
                        std::max(records.at(owner).cadence_shift, records.at(other).cadence_shift);
                    if (shift >= 63 || (tick & ((std::uint64_t{1} << shift) - 1)) != 0)
                        continue;
                }
                auto& here = m_pages.at(page_key);
                auto& there = m_pages.at(neighbour.key);
                for (int channel = 0; channel < kChannels; ++channel) {
                    const auto difference =
                        ChannelValue(channel, old_neighbour->cells[channel][neighbour.index]) -
                        ChannelValue(channel, old_page.cells[channel][i]);
                    const auto flux = difference / 32;
                    here.cells[channel][i] =
                        static_cast<std::uint16_t>(here.cells[channel][i] + flux);
                    there.cells[channel][neighbour.index] =
                        static_cast<std::uint16_t>(there.cells[channel][neighbour.index] - flux);
                }
                ++work[owner];
                ++work[other];
            }
        }
    }
    return work;
}

bool FieldPageState::replace_weather(std::vector<RegionalStorm> storms,
                                     std::vector<RegionalStrike> strikes) {
    if (storms.size() > kMaxStorms || strikes.size() > kMaxStrikes)
        return false;
    std::map<RegionKey, std::size_t> owner_storms, owner_strikes;
    for (const auto& storm : storms)
        if (++owner_storms[storm.owner] > kMaxOwnerStorms)
            return false;
    for (const auto& strike : strikes)
        if (++owner_strikes[strike.owner] > kMaxOwnerStrikes)
            return false;
    if (!std::all_of(storms.begin(), storms.end(), ValidStorm) ||
        !std::all_of(strikes.begin(), strikes.end(), ValidStrike))
        return false;
    std::sort(storms.begin(), storms.end(), [](const auto& a, const auto& b) {
        return StormOrder(a) < StormOrder(b);
    });
    std::sort(strikes.begin(), strikes.end(), [](const auto& a, const auto& b) {
        return StrikeOrder(a) < StrikeOrder(b);
    });
    for (std::size_t i = 1; i < storms.size(); ++i)
        if (StormOrder(storms[i - 1]) == StormOrder(storms[i]))
            return false;
    for (std::size_t i = 1; i < strikes.size(); ++i)
        if (StrikeOrder(strikes[i - 1]) == StrikeOrder(strikes[i]))
            return false;
    m_storms = std::move(storms);
    m_strikes = std::move(strikes);
    return true;
}
bool FieldPageState::set_scent(ScentCellKey key, const ScentCell& values) {
    if (!ValidScent(key, values))
        return false;
    const bool zero =
        std::all_of(values.begin(), values.end(), [](auto bits) { return bits == 0; });
    if (zero) {
        m_scent.erase(key);
        return true;
    }
    if (!m_scent.contains(key) && m_scent.size() == kMaxScentCells)
        return false;
    m_scent[key] = values;
    return true;
}
RegionKey FieldPageState::region_of_scent_cell(ScentCellKey key) {
    return {static_cast<std::int32_t>(
                FloorDiv(static_cast<std::int64_t>(key.x) * 1000 + 500, kRegionMm)),
            static_cast<std::int32_t>(
                FloorDiv(static_cast<std::int64_t>(key.z) * 1000 + 500, kRegionMm))};
}
bool FieldPageState::owns_evolution_state(RegionKey key) const {
    if (m_weatherClocks.contains(key))
        return true;
    for (const auto& storm : m_storms)
        if (storm.owner == key)
            return true;
    for (const auto& strike : m_strikes)
        if (strike.owner == key)
            return true;
    for (const auto& [cell, values] : m_scent) {
        (void)values;
        if (region_of_scent_cell(cell) == key)
            return true;
    }
    return false;
}
bool FieldPageState::set_weather_age(RegionKey owner, std::uint64_t active_age) {
    if (!ValidOwner(owner) || active_age >= WorldClock::kTickLimit ||
        (!m_weatherClocks.contains(owner) && m_weatherClocks.size() == kMaxWeatherClocks))
        return false;
    m_weatherClocks[owner] = active_age;
    return true;
}
std::string FieldPageState::serialize() const {
    if (empty())
        return {};
    std::string out = kMagic;
    AppendLe(out, kFormatVersion, 2);
    AppendLe(out, kChannelMask, 2);
    AppendLe(out,
             static_cast<unsigned>(!m_pages.empty()) + static_cast<unsigned>(!m_storms.empty()) +
                 static_cast<unsigned>(!m_strikes.empty()) +
                 static_cast<unsigned>(!m_scent.empty()) +
                 static_cast<unsigned>(!m_weatherClocks.empty()),
             4);
    std::string records;
    for (const auto& [key, page] : m_pages)
        AppendPage(records, key, page);
    AppendSection(out, 1, m_pages.size(), records);
    records.clear();
    for (const auto& storm : m_storms)
        AppendStorm(records, storm);
    AppendSection(out, 2, m_storms.size(), records);
    records.clear();
    for (const auto& strike : m_strikes)
        AppendStrike(records, strike);
    AppendSection(out, 3, m_strikes.size(), records);
    records.clear();
    for (const auto& [key, cell] : m_scent)
        AppendScent(records, key, cell);
    AppendSection(out, 4, m_scent.size(), records);
    records.clear();
    for (const auto& [owner, active_age] : m_weatherClocks) {
        AppendLe(records, static_cast<std::uint32_t>(owner.x), 4);
        AppendLe(records, static_cast<std::uint32_t>(owner.z), 4);
        AppendLe(records, active_age, 8);
    }
    AppendSection(out, 7, m_weatherClocks.size(), records);
    AppendLe(out, Fnv1a64(out), 8);
    return out;
}
bool FieldPageState::parse(const std::string& bytes, FieldPageState& out, std::string& error) {
    error = "field pages: corrupt typed record";
    try {
        if (bytes.size() < kHeaderBytes + kFooterBytes || bytes.size() > kMaxFileBytes ||
            bytes.compare(0, 4, kMagic) != 0 || ReadLe(bytes, 4, 2) != kFormatVersion ||
            ReadLe(bytes, 6, 2) != kChannelMask)
            return false;
        const auto footer = bytes.size() - kFooterBytes;
        if (ReadLe(bytes, footer, 8) != Fnv1a64(std::string_view(bytes).substr(0, footer)))
            return false;
        const auto sections = ReadLe(bytes, 8, 4);
        if (sections == 0 || sections > 5)
            return false;
        FieldPageState temporary;
        std::size_t at = kHeaderBytes;
        std::uint64_t previous = 0;
        for (std::uint64_t section = 0; section < sections; ++section) {
            if (footer - at < 10)
                return false;
            const auto type = ReadLe(bytes, at, 2), length = ReadLe(bytes, at + 2, 4),
                       count = ReadLe(bytes, at + 6, 4);
            if (type <= previous || (type > 4 && type != 7) || length < 4 ||
                length > footer - at - 6 || count == 0)
                return false;
            previous = type;
            const std::size_t width = type == 1   ? kPageBytes
                                      : type == 2 ? 48
                                      : type == 3 ? 36
                                      : type == 4 ? 40
                                                  : 16;
            const std::size_t cap = type == 1   ? kMaxPages
                                    : type == 2 ? kMaxStorms
                                    : type == 3 ? kMaxStrikes
                                    : type == 4 ? kMaxScentCells
                                                : kMaxWeatherClocks;
            if (count > cap || length != 4 + count * width)
                return false;
            const auto records_at = at + 10;
            if (type == 1) {
                if (!DecodePages(bytes,
                                 static_cast<std::size_t>(count),
                                 records_at,
                                 temporary.m_pages,
                                 error))
                    return false;
            } else {
                SectionReader reader(bytes, records_at);
                for (std::uint64_t entry = 0; entry < count; ++entry) {
                    if (type == 2) {
                        RegionalStorm storm;
                        storm.owner = reader.owner();
                        storm.center_x = reader.word();
                        storm.center_z = reader.word();
                        storm.velocity_x = reader.word();
                        storm.velocity_z = reader.word();
                        storm.intensity = reader.word();
                        storm.spawn_tick = reader.read(8);
                        storm.lifetime_ticks = reader.word();
                        storm.seed_salt = reader.word();
                        storm.active_age = reader.word();
                        temporary.m_storms.push_back(storm);
                    } else if (type == 3) {
                        RegionalStrike strike;
                        strike.owner = reader.owner();
                        strike.strike_tick = reader.read(8);
                        strike.world_x = reader.word();
                        strike.world_z = reader.word();
                        strike.magnitude = reader.word();
                        strike.storm_salt = reader.word();
                        strike.active_age = reader.word();
                        temporary.m_strikes.push_back(strike);
                    } else if (type == 7) {
                        const RegionKey owner = reader.owner();
                        const auto age = reader.read(8);
                        if (!temporary.set_weather_age(owner, age))
                            return false;
                    } else {
                        const ScentCellKey key{std::bit_cast<std::int32_t>(reader.word()),
                                               std::bit_cast<std::int32_t>(reader.word())};
                        ScentCell values{};
                        for (auto& value : values)
                            value = reader.read(8);
                        if (!temporary.set_scent(key, values))
                            return false;
                    }
                }
            }
            at += 6 + static_cast<std::size_t>(length);
        }
        if (at != footer || !temporary.replace_weather(temporary.m_storms, temporary.m_strikes) ||
            temporary.serialize() != bytes)
            return false;
        out = std::move(temporary);
        error.clear();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string FieldPageState::canonical_bytes() const {
    if (empty())
        return {};
    return "field_pages:v2:" + serialize();
}

std::string FieldPageState::ambient_bytes(std::uint8_t channels, bool weather) const {
    std::string out;
    for (const auto& [key, page] : m_pages) {
        AppendLe(out, static_cast<std::uint32_t>(key.x), 4);
        AppendLe(out, static_cast<std::uint32_t>(key.z), 4);
        for (const auto mask : page.mask)
            AppendLe(out, mask, 8);
        for (int channel = 0; channel < kChannels; ++channel)
            if ((channels & (1u << channel)) != 0)
                for (const auto word : page.cells[channel])
                    AppendLe(out, word, 2);
    }
    if (weather) {
        std::string records;
        for (const auto& storm : m_storms)
            AppendStorm(records, storm);
        AppendSection(out, 2, m_storms.size(), records);
        records.clear();
        for (const auto& strike : m_strikes)
            AppendStrike(records, strike);
        AppendSection(out, 3, m_strikes.size(), records);
        records.clear();
        for (const auto& [owner, age] : m_weatherClocks) {
            AppendLe(records, static_cast<std::uint32_t>(owner.x), 4);
            AppendLe(records, static_cast<std::uint32_t>(owner.z), 4);
            AppendLe(records, age, 8);
        }
        AppendSection(out, 7, m_weatherClocks.size(), records);
    }
    return out;
}

std::string FieldPageState::region_bytes(RegionKey key) const {
    std::string out;
    for (const auto& [cell_x, cell_z] : cells_of_region(key)) {
        const CellSlot slot = CellSlotOf(cell_x, cell_z);
        const FieldPage* page = InitializedPage(m_pages, slot);
        if (page == nullptr) {
            continue;
        }
        AppendLe(out, static_cast<std::uint32_t>(cell_x), 4);
        AppendLe(out, static_cast<std::uint32_t>(cell_z), 4);
        for (const auto& channel : page->cells) {
            AppendLe(out, channel[slot.index], 2);
        }
    }
    std::string storms, strikes, scent;
    std::size_t storm_count = 0, strike_count = 0, scent_count = 0;
    for (const auto& storm : m_storms)
        if (storm.owner == key) {
            AppendStorm(storms, storm);
            ++storm_count;
        }
    for (const auto& strike : m_strikes)
        if (strike.owner == key) {
            AppendStrike(strikes, strike);
            ++strike_count;
        }
    for (const auto& [cell, values] : m_scent)
        if (region_of_scent_cell(cell) == key) {
            AppendScent(scent, cell, values);
            ++scent_count;
        }
    const auto clock = m_weatherClocks.find(key);
    if (storm_count + strike_count + scent_count != 0 || clock != m_weatherClocks.end()) {
        out += "|evolution:v2:";
        AppendSection(out, 2, storm_count, storms);
        AppendSection(out, 3, strike_count, strikes);
        AppendSection(out, 4, scent_count, scent);
        if (clock != m_weatherClocks.end()) {
            std::string age;
            AppendLe(age, static_cast<std::uint32_t>(key.x), 4);
            AppendLe(age, static_cast<std::uint32_t>(key.z), 4);
            AppendLe(age, clock->second, 8);
            AppendSection(out, 7, 1, age);
        }
    }
    return out;
}

} // namespace Luminumbra::world
