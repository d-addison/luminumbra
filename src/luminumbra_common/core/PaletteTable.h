#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra {

using PaletteId = std::uint16_t;

struct PaletteEntry {
    std::string key;
    bool tombstone = false;
    bool operator==(const PaletteEntry&) const = default;
};

// Save-local identity only; this primitive performs no save I/O or mod admission.
// Registration/loading are bounded setup operations, never tick-path operations.
class PaletteTable {
public:
    // At most the complete u16 ID space; keys use at most 255 ASCII bytes.
    static constexpr std::size_t kMaxEntries = 65536;
    static constexpr std::size_t kMaxKeyBytes = 255;
    // Each canonical record needs at most key bytes + 11 punctuation/bool bytes.
    static constexpr std::size_t kMaxJsonBytes = 28 + kMaxEntries * (kMaxKeyBytes + 11);
    static constexpr std::size_t kBaseEntries = 8;

    PaletteTable();
    PaletteTable(const PaletteTable&) = default;
    PaletteTable& operator=(const PaletteTable&) = default;

    // Exactly one colon separates nonempty namespace and name. Each component
    // uses lowercase ASCII letters, digits, '_', '.', or '-'. No normalization.
    [[nodiscard]] static bool valid_key(std::string_view key);

    // New keys append in ASCII order. Existing IDs (including tombstones) stay
    // pinned. Duplicate inputs are idempotent. Refusals leave the table unchanged.
    // Each input batch is limited to kMaxEntries before scanning/copying it.
    [[nodiscard]] bool register_ids(std::span<const std::string> keys, std::string& error);

    // Strict canonical JSON: {"entries":[["namespace:name",false],...],"version":1}
    // Array position is the dense ID. No whitespace, escapes, or extra fields.
    // Base entries are always live. Other missing keys become permanent
    // tombstones; newly available keys append in ASCII order. The caller must
    // enforce missing-mod admission before opting into this recovery operation.
    // Validation completes before replacing this table; errors leave it unchanged.
    [[nodiscard]] bool load_json(std::string_view bytes,
                                 std::span<const std::string> available_keys,
                                 std::string& error);
    [[nodiscard]] std::string to_json() const;

    // Numeric lookup is O(1); string lookup is O(log(size)), for setup/resolution.
    // A tombstone still resolves to its original ID. Invalid/absent keys and
    // out-of-range numeric IDs return empty/null; inspect the entry for liveness.
    [[nodiscard]] std::optional<PaletteId> find(std::string_view key) const;
    [[nodiscard]] const PaletteEntry* entry(std::size_t id) const;
    [[nodiscard]] std::size_t size() const;

private:
    std::vector<PaletteEntry> m_entries;
    std::map<std::string, PaletteId, std::less<>> m_ids;

    [[nodiscard]] bool append_ids(std::span<const std::string> keys, std::string& error);
    [[nodiscard]] bool parse_json(std::string_view bytes, std::string& error);
};

} // namespace Luminumbra
