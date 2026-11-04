#pragma once

#include "FieldCellTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace Luminumbra::world {
struct RegionKey;

// Exact existing one-metre soil/irrigation integer units, independent of the
// 24-metre ambient grid. Initialized cells are grouped into 16x16 world pages.
// Soil baseline is 1,000,000; irrigation baseline zero. Sparse initialized masks
// preserve cells whose values have returned to baseline across save/reopen.
class FoliageFieldPages {
public:
    struct Page {
        std::array<std::uint64_t, 4> mask{};
        std::array<std::int32_t, 256> cells{};
        bool operator==(const Page&) const = default;
    };
    static constexpr std::size_t kMaxPages = 4096;
    static constexpr std::size_t kRecordBytes = 1064;
    [[nodiscard]] const std::map<FieldPageKey, Page>& pages() const {
        return m_pages;
    }
    [[nodiscard]] bool empty() const {
        return m_pages.empty();
    }
    [[nodiscard]] static bool valid_cell(const ScentCellKey& cell);
    [[nodiscard]] std::int32_t sample(const ScentCellKey& cell, std::int32_t baseline) const;
    [[nodiscard]] bool initialized(const ScentCellKey& cell) const;
    bool set(const ScentCellKey& cell, std::int32_t value, std::int32_t limit);
    [[nodiscard]] bool owns_region(const RegionKey& owner) const;
    [[nodiscard]] std::string records() const;
    [[nodiscard]] std::string region_bytes(const RegionKey& owner) const;
    // Strict sorted unique pages, nonempty masks, zero uninitialized slots,
    // valid centres, exact i32 units and bounded allocation; transactional.
    static bool parse_records(const std::string_view& bytes,
                              std::size_t count,
                              std::int32_t limit,
                              FoliageFieldPages& out);

private:
    std::map<FieldPageKey, Page> m_pages;
};
} // namespace Luminumbra::world
