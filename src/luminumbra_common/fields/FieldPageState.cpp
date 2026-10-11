#include "FieldPageState.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <string_view>
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

// FPG1 layout: header (magic, version, channel count, page count), pages, FNV-1a footer.
constexpr const char* kMagic = "FPG1";
constexpr std::uint64_t kFormatVersion = 1;
constexpr std::uint64_t kChannelCount = 5;
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
static_assert(FieldPageState::kMaxFileBytes ==
              kHeaderBytes + FieldPageState::kMaxPages * kPageBytes + kFooterBytes);

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

std::uint16_t RelaxToward(std::uint16_t value, std::uint16_t target) {
    const auto current = static_cast<std::int32_t>(value);
    const auto goal = static_cast<std::int32_t>(target);
    const std::int32_t next = current + ((goal - current) >> FieldPageState::kRelaxShift);
    return static_cast<std::uint16_t>(next);
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
        value = RelaxToward(value, target);
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

// Header checks: size cap, magic, version, channel count and page count cap.
bool CheckHeader(const std::string& bytes, std::uint64_t& page_count, std::string& error) {
    if (bytes.empty()) {
        error = "field pages: empty input";
        return false;
    }
    if (bytes.size() > FieldPageState::kMaxFileBytes) {
        error = "field pages: file exceeds size cap";
        return false;
    }
    if (bytes.size() < kHeaderBytes + kFooterBytes) {
        error = "field pages: truncated";
        return false;
    }
    if (bytes.compare(0, 4, kMagic) != 0) {
        error = "field pages: wrong magic";
        return false;
    }
    if (ReadLe(bytes, 4, 2) != kFormatVersion || ReadLe(bytes, 6, 2) != kChannelCount) {
        error = "field pages: wrong version or channel count";
        return false;
    }
    page_count = ReadLe(bytes, 8, 4);
    if (page_count > FieldPageState::kMaxPages) {
        error = "field pages: page count exceeds cap";
        return false;
    }
    return true;
}

// Exact length for the declared page count, then the FNV-1a footer over everything before it.
bool CheckLengthAndFooter(const std::string& bytes, std::uint64_t page_count, std::string& error) {
    const std::size_t expected =
        kHeaderBytes + static_cast<std::size_t>(page_count) * kPageBytes + kFooterBytes;
    if (bytes.size() != expected) {
        error = "field pages: length does not match page count";
        return false;
    }
    const std::size_t footer_at = bytes.size() - kFooterBytes;
    const std::string_view body = std::string_view(bytes).substr(0, footer_at);
    if (ReadLe(bytes, footer_at, 8) != Fnv1a64(body)) {
        error = "field pages: footer mismatch";
        return false;
    }
    return true;
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
                 PageMap& pages,
                 std::string& error) {
    FieldPageKey previous;
    for (std::size_t index = 0; index < page_count; ++index) {
        FieldPageKey key;
        FieldPage page;
        DecodePage(bytes, kHeaderBytes + index * kPageBytes, key, page);
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

bool DecodeFile(const std::string& bytes, PageMap& pages, std::string& error) {
    std::uint64_t page_count = 0;
    if (!CheckHeader(bytes, page_count, error)) {
        return false;
    }
    if (!CheckLengthAndFooter(bytes, page_count, error)) {
        return false;
    }
    return DecodePages(bytes, static_cast<std::size_t>(page_count), pages, error);
}

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

std::string FieldPageState::serialize() const {
    if (m_pages.empty()) {
        return {};
    }
    std::string out = kMagic;
    AppendLe(out, kFormatVersion, 2);
    AppendLe(out, kChannelCount, 2);
    AppendLe(out, static_cast<std::uint64_t>(m_pages.size()), 4);
    for (const auto& [key, page] : m_pages) {
        AppendPage(out, key, page);
    }
    const std::uint64_t footer = Fnv1a64(out);
    AppendLe(out, footer, 8);
    return out;
}

bool FieldPageState::parse(const std::string& bytes, FieldPageState& out, std::string& error) {
    try {
        FieldPageState temporary;
        if (!DecodeFile(bytes, temporary.m_pages, error)) {
            return false;
        }
        if (temporary.serialize() != bytes) {
            error = "field pages: bytes are not canonical";
            return false;
        }
        out = std::move(temporary);
        return true;
    } catch (const std::exception&) {
        error = "field pages: parse exception";
        return false;
    }
}

std::string FieldPageState::canonical_bytes() const {
    if (m_pages.empty()) {
        return {};
    }
    return "field_pages:v1:" + serialize();
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
    return out;
}

} // namespace Luminumbra::world
