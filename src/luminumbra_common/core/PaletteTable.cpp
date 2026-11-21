#include "PaletteTable.h"

#include <array>
#include <limits>
#include <set>
#include <utility>

namespace Luminumbra {
namespace {

constexpr std::array<std::string_view, PaletteTable::kBaseEntries> kBaseKeys = {
    "luminumbra:air",
    "luminumbra:stone",
    "luminumbra:soil",
    "luminumbra:grass",
    "luminumbra:sand",
    "luminumbra:deepslate",
    "luminumbra:lumin_crystal",
    "luminumbra:water"};
constexpr std::string_view kJsonPrefix = R"({"entries":[)";
constexpr std::string_view kJsonSuffix = R"(],"version":1})";
static_assert(PaletteTable::kMaxEntries - 1 == std::numeric_limits<PaletteId>::max());
static_assert(kJsonPrefix.size() + kJsonSuffix.size() <= 28);
static_assert(PaletteTable::kMaxEntries <=
              (std::numeric_limits<std::size_t>::max() - 28) / (PaletteTable::kMaxKeyBytes + 11));

bool Refuse(std::string& error, std::string_view reason) {
    error = reason;
    return false;
}

bool Consume(std::string_view& input, std::string_view token) {
    if (!input.starts_with(token)) {
        return false;
    }
    input.remove_prefix(token.size());
    return true;
}

} // namespace

PaletteTable::PaletteTable() {
    for (const auto key : kBaseKeys) {
        const auto id = static_cast<PaletteId>(m_entries.size());
        m_entries.push_back({std::string(key), false});
        m_ids.emplace(key, id);
    }
}

bool PaletteTable::valid_key(std::string_view key) {
    if (key.size() > kMaxKeyBytes) {
        return false;
    }
    const auto colon = key.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == key.size()) {
        return false;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        const char ch = key[i];
        if (i != colon && !(ch >= 'a' && ch <= 'z') && !(ch >= '0' && ch <= '9') && ch != '_' &&
            ch != '.' && ch != '-') {
            return false;
        }
    }
    return true;
}

bool PaletteTable::append_ids(std::span<const std::string> keys, std::string& error) {
    if (keys.size() > kMaxEntries) {
        return Refuse(error, "palette input count exceeds limit");
    }
    // At most kMaxEntries views of already bounded keys, sorted without copying keys.
    std::set<std::string_view> additions;
    for (const auto& key : keys) {
        if (!valid_key(key)) {
            return Refuse(error, "invalid palette key");
        }
        if (!m_ids.contains(key)) {
            additions.insert(key);
        }
    }
    if (additions.size() > kMaxEntries - m_entries.size()) {
        return Refuse(error, "palette ID space exhausted");
    }
    for (const auto key : additions) {
        const auto id = static_cast<PaletteId>(m_entries.size());
        m_entries.push_back({std::string(key), false});
        m_ids.emplace(key, id);
    }
    return true;
}

bool PaletteTable::register_ids(std::span<const std::string> keys, std::string& error) {
    // Stage mutations so input errors and allocation failures preserve the table.
    PaletteTable candidate = *this;
    if (!candidate.append_ids(keys, error)) {
        return false;
    }
    m_entries.swap(candidate.m_entries);
    m_ids.swap(candidate.m_ids);
    error.clear();
    return true;
}

bool PaletteTable::parse_json(std::string_view bytes, std::string& error) {
    if (bytes.size() > kMaxJsonBytes || !Consume(bytes, kJsonPrefix) ||
        !bytes.ends_with(kJsonSuffix)) {
        return Refuse(error, "invalid canonical palette envelope");
    }
    bytes.remove_suffix(kJsonSuffix.size());
    m_entries.clear();
    m_ids.clear();
    while (!bytes.empty()) {
        if (m_entries.size() == kMaxEntries || !Consume(bytes, "[\"")) {
            return Refuse(error, "invalid palette record or entry limit exceeded");
        }
        // Scan at most one bounded key plus its closing quote, even on bad input.
        const auto quote = bytes.substr(0, kMaxKeyBytes + 1).find('"');
        if (quote == std::string_view::npos || !valid_key(bytes.substr(0, quote))) {
            return Refuse(error, "invalid palette record key");
        }
        const std::string key(bytes.substr(0, quote));
        bytes.remove_prefix(quote + 1);
        bool tombstone = false;
        if (Consume(bytes, ",true]")) {
            tombstone = true;
        } else if (!Consume(bytes, ",false]")) {
            return Refuse(error, "invalid palette tombstone flag");
        }
        const auto id = m_entries.size();
        if (id < kBaseEntries && (key != kBaseKeys[id] || tombstone)) {
            return Refuse(error, "reserved palette entries must match built-in IDs");
        }
        if (!m_ids.emplace(key, static_cast<PaletteId>(id)).second) {
            return Refuse(error, "duplicate palette key");
        }
        m_entries.push_back({key, tombstone});
        if (!bytes.empty() && (!Consume(bytes, ",") || bytes.empty())) {
            return Refuse(error, "invalid palette record separator");
        }
    }
    if (m_entries.size() < kBaseEntries) {
        return Refuse(error, "reserved palette entries are missing");
    }
    return true;
}

bool PaletteTable::load_json(std::string_view bytes,
                             std::span<const std::string> available_keys,
                             std::string& error) {
    PaletteTable candidate;
    if (!candidate.parse_json(bytes, error) || !candidate.append_ids(available_keys, error)) {
        return false;
    }
    // Availability is independent of registration order. Base keys are implicit.
    // At most kMaxEntries views; append_ids has validated the complete input.
    const std::set<std::string_view> available(available_keys.begin(), available_keys.end());
    for (std::size_t id = kBaseEntries; id < candidate.m_entries.size(); ++id) {
        auto& record = candidate.m_entries[id];
        record.tombstone = record.tombstone || !available.contains(record.key);
    }
    m_entries.swap(candidate.m_entries);
    m_ids.swap(candidate.m_ids);
    error.clear();
    return true;
}

std::string PaletteTable::to_json() const {
    // Checked table/key bounds make this exact output size <= kMaxJsonBytes.
    std::size_t bytes = kJsonPrefix.size() + kJsonSuffix.size();
    for (const auto& record : m_entries) {
        bytes += record.key.size() + (record.tombstone ? 9 : 10);
    }
    bytes += m_entries.size() - 1;
    std::string result;
    result.reserve(bytes);
    result += kJsonPrefix;
    for (std::size_t id = 0; id < m_entries.size(); ++id) {
        if (id != 0) {
            result += ',';
        }
        const auto& record = m_entries[id];
        result += "[\"";
        result += record.key;
        result += record.tombstone ? "\",true]" : "\",false]";
    }
    result += kJsonSuffix;
    return result;
}

std::optional<PaletteId> PaletteTable::find(std::string_view key) const {
    if (!valid_key(key)) {
        return std::nullopt;
    }
    const auto found = m_ids.find(key);
    if (found == m_ids.end()) {
        return std::nullopt;
    }
    return found->second;
}

const PaletteEntry* PaletteTable::entry(std::size_t id) const {
    return id < m_entries.size() ? &m_entries[id] : nullptr;
}

std::size_t PaletteTable::size() const {
    return m_entries.size();
}

} // namespace Luminumbra
