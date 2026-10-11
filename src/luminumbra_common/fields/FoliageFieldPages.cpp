#include "FoliageFieldPages.h"
#include "FieldPageState.h"
#include <bit>
#include <limits>

namespace Luminumbra::world {
namespace {
std::int32_t FloorDiv16(std::int32_t v) {
    return v >= 0 ? v / 16 : (v - 15) / 16;
}
std::pair<FieldPageKey, unsigned> Slot(ScentCellKey cell) {
    const FieldPageKey key{FloorDiv16(cell.x), FloorDiv16(cell.z)};
    return {key, static_cast<unsigned>((cell.z - key.z * 16) * 16 + cell.x - key.x * 16)};
}
void Append(std::string& out, std::uint64_t v, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 255));
}
std::uint64_t Read(std::string_view bytes, std::size_t& at, unsigned width) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < width; ++i)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[at++])) << (8 * i);
    return v;
}
} // namespace
bool FoliageFieldPages::valid_cell(ScentCellKey cell) {
    return cell.x >= -16777216 && cell.x < 16777216 && cell.z >= -16777216 && cell.z < 16777216;
}
std::int32_t FoliageFieldPages::sample(ScentCellKey cell, std::int32_t baseline) const {
    if (!valid_cell(cell))
        return baseline;
    const auto [key, i] = Slot(cell);
    const auto it = m_pages.find(key);
    return it != m_pages.end() && ((it->second.mask[i / 64] >> (i % 64)) & 1) ? it->second.cells[i]
                                                                              : baseline;
}
bool FoliageFieldPages::set(ScentCellKey cell, std::int32_t value, std::int32_t limit) {
    if (!valid_cell(cell) || value < 0 || value > limit)
        return false;
    const auto [key, i] = Slot(cell);
    if (!m_pages.contains(key) && m_pages.size() >= kMaxPages)
        return false;
    auto& page = m_pages[key];
    page.mask[i / 64] |= std::uint64_t{1} << (i % 64);
    page.cells[i] = value;
    return true;
}
bool FoliageFieldPages::owns_region(RegionKey owner) const {
    for (const auto& [key, page] : m_pages)
        for (unsigned i = 0; i < 256; ++i)
            if (((page.mask[i / 64] >> (i % 64)) & 1) &&
                FieldPageState::region_of_scent_cell({key.x * 16 + static_cast<int>(i % 16),
                                                      key.z * 16 + static_cast<int>(i / 16)}) ==
                    owner)
                return true;
    return false;
}
std::string FoliageFieldPages::records() const {
    std::string out;
    for (const auto& [key, page] : m_pages) {
        Append(out, static_cast<std::uint32_t>(key.x), 4);
        Append(out, static_cast<std::uint32_t>(key.z), 4);
        for (const auto word : page.mask)
            Append(out, word, 8);
        for (const auto value : page.cells)
            Append(out, static_cast<std::uint32_t>(value), 4);
    }
    return out;
}
std::string FoliageFieldPages::region_bytes(RegionKey owner) const {
    std::string out;
    for (const auto& [key, page] : m_pages)
        for (unsigned i = 0; i < 256; ++i) {
            const ScentCellKey cell{key.x * 16 + static_cast<int>(i % 16),
                                    key.z * 16 + static_cast<int>(i / 16)};
            if (((page.mask[i / 64] >> (i % 64)) & 1) &&
                FieldPageState::region_of_scent_cell(cell) == owner) {
                Append(out, static_cast<std::uint32_t>(cell.x), 4);
                Append(out, static_cast<std::uint32_t>(cell.z), 4);
                Append(out, static_cast<std::uint32_t>(page.cells[i]), 4);
            }
        }
    return out;
}
bool FoliageFieldPages::parse_records(std::string_view bytes,
                                      std::size_t count,
                                      std::int32_t limit,
                                      FoliageFieldPages& out) {
    if (count == 0 || count > kMaxPages || bytes.size() != count * kRecordBytes || limit < 0)
        return false;
    FoliageFieldPages next;
    std::optional<FieldPageKey> prior;
    std::size_t at = 0;
    for (std::size_t record = 0; record < count; ++record) {
        const auto x = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(Read(bytes, at, 4)));
        const auto z = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(Read(bytes, at, 4)));
        const FieldPageKey key{x, z};
        if ((prior && key <= *prior) || x < -1048576 || x >= 1048576 || z < -1048576 ||
            z >= 1048576)
            return false;
        Page page;
        bool any = false;
        for (auto& word : page.mask) {
            word = Read(bytes, at, 8);
            any |= word != 0;
        }
        if (!any)
            return false;
        for (unsigned i = 0; i < 256; ++i) {
            const auto value =
                std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(Read(bytes, at, 4)));
            const bool initialized = ((page.mask[i / 64] >> (i % 64)) & 1) != 0;
            if (value < 0 || value > limit || (!initialized && value != 0))
                return false;
            page.cells[i] = value;
        }
        next.m_pages.emplace(key, page);
        prior = key;
    }
    out = std::move(next);
    return true;
}
} // namespace Luminumbra::world
