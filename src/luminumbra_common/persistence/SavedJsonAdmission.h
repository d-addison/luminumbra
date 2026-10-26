#pragma once

#include "SavedInputFile.h"
#include "SavedInputLimits.h"
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <ios>
#include <nlohmann/json.hpp>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>

namespace Luminumbra::Persistence {

// Streaming serialization refuses before growing output beyond its compatibility
// limit. Used for writer preflight; no on-disk state changes here.
class LimitedJsonBuffer final : public std::streambuf {
public:
    explicit LimitedJsonBuffer(std::size_t maximum)
        : m_maximum(maximum) {}
    [[nodiscard]] std::string take_bytes() {
        return std::move(m_bytes);
    }

protected:
    std::streamsize xsputn(const char* text, std::streamsize count) override {
        if (count < 0 || static_cast<std::uintmax_t>(count) > m_maximum - m_bytes.size())
            return 0;
        m_bytes.append(text, static_cast<std::size_t>(count));
        return count;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        if (m_bytes.size() == m_maximum)
            return traits_type::eof();
        m_bytes.push_back(traits_type::to_char_type(value));
        return value;
    }

private:
    std::size_t m_maximum;
    std::string m_bytes;
};

inline bool BoundedJsonDump(const nlohmann::json& value,
                            std::size_t maximum,
                            std::string& bytes,
                            int indent = -1) {
    LimitedJsonBuffer buffer(maximum);
    std::ostream stream(&buffer);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        if (indent >= 0)
            stream << std::setw(indent);
        stream << value;
        if (indent >= 0)
            stream << '\n';
    } catch (const std::exception&) {
        return false;
    }
    bytes = buffer.take_bytes();
    return true;
}

inline bool AdmitMetadata(const nlohmann::json& metadata) {
    for (const auto& [key, maximum] : {std::pair{"name", SavedInputLimits::kWorldNameBytes},
                                       std::pair{"seed", SavedInputLimits::kWorldSeedBytes}}) {
        if (metadata.contains(key) &&
            (!metadata.at(key).is_string() ||
             metadata.at(key).get_ref<const std::string&>().size() > maximum))
            return false;
    }
    return true;
}

// Root name/seed admission precedes DOM construction. A JSON escape uses at
// most six source bytes per decoded byte; only that bounded token is decoded.
inline bool AdmitMetadataText(std::string_view bytes) {
    std::size_t depth = 0;
    for (std::size_t pos = 0; pos < bytes.size(); ++pos) {
        const char ch = bytes[pos];
        if (ch == '{' || ch == '[') {
            ++depth;
        } else if (ch == '}' || ch == ']') {
            if (depth > 0)
                --depth;
        } else if (ch == '"') {
            const auto start = pos++;
            for (; pos < bytes.size() && bytes[pos] != '"'; ++pos) {
                if (bytes[pos] == '\\')
                    ++pos;
            }
            if (pos >= bytes.size())
                return false;
            if (depth != 1 || pos - start > 26)
                continue;
            const auto key =
                nlohmann::json::parse(bytes.substr(start, pos - start + 1), nullptr, false);
            if (key != "name" && key != "seed")
                continue;
            auto cursor = pos + 1;
            while (cursor < bytes.size() &&
                   std::isspace(static_cast<unsigned char>(bytes[cursor])) != 0)
                ++cursor;
            if (cursor == bytes.size() || bytes[cursor++] != ':')
                continue;
            while (cursor < bytes.size() &&
                   std::isspace(static_cast<unsigned char>(bytes[cursor])) != 0)
                ++cursor;
            if (cursor == bytes.size() || bytes[cursor] != '"')
                return false;
            const auto value_start = cursor++;
            const auto maximum = key == "name" ? SavedInputLimits::kWorldNameBytes
                                               : SavedInputLimits::kWorldSeedBytes;
            for (; cursor < bytes.size() && bytes[cursor] != '"'; ++cursor) {
                if (cursor - value_start > 6 * maximum)
                    return false;
                if (bytes[cursor] == '\\')
                    ++cursor;
            }
            if (cursor >= bytes.size() || cursor - value_start > 6 * maximum + 1)
                return false;
            const auto value = nlohmann::json::parse(
                bytes.substr(value_start, cursor - value_start + 1), nullptr, false);
            if (!value.is_string() || value.get_ref<const std::string&>().size() > maximum)
                return false;
            pos = cursor;
        }
    }
    return true;
}

inline bool ReadSavedJson(const std::filesystem::path& path,
                          std::size_t maximum,
                          nlohmann::json& value,
                          bool& missing,
                          bool metadata_input = true,
                          bool* malformed_json = nullptr) {
    if (malformed_json)
        *malformed_json = false;
    const SavedInputFile input(path);
    missing = input.missing();
    if (missing)
        return true;
    std::string bytes;
    if (!input.read(bytes, maximum) || (metadata_input && !AdmitMetadataText(bytes)))
        return false;
    auto candidate = nlohmann::json::parse(bytes, nullptr, false);
    if (candidate.is_discarded()) {
        if (malformed_json)
            *malformed_json = true;
        return false;
    }
    value = std::move(candidate);
    return true;
}

// Lexical preflight before DOM allocation. Only component data values at the
// snapshot's component depth are counted. Whitespace outside strings is free,
// matching the compact writer encoding; escaped quotes never terminate strings.
// Each byte is visited at most twice, bounded by kPlantFileBytes. JSON syntax
// and the plant numeric domains are still checked by the existing parsers.
inline bool AdmitPlantComponentBytes(std::string_view bytes) {
    if (bytes.size() > SavedInputLimits::kPlantFileBytes)
        return false;
    std::size_t depth = 0;
    std::size_t aggregate = 0;
    for (std::size_t pos = 0; pos < bytes.size(); ++pos) {
        const char token = bytes[pos];
        if (token == '{' || token == '[') {
            ++depth;
        } else if (token == '}' || token == ']') {
            if (depth == 0)
                return false;
            --depth;
        } else if (token == '"') {
            const auto start = pos++;
            for (; pos < bytes.size() && bytes[pos] != '"'; ++pos) {
                if (bytes[pos] == '\\')
                    ++pos;
            }
            if (pos >= bytes.size())
                return false;
            if (depth != 5 || pos - start > 26)
                continue;
            const auto key =
                nlohmann::json::parse(bytes.substr(start, pos - start + 1), nullptr, false);
            if (key != "data")
                continue;
            auto cursor = pos + 1;
            const auto skip_space = [&bytes](std::size_t& offset) {
                while (offset < bytes.size() &&
                       std::isspace(static_cast<unsigned char>(bytes[offset])) != 0)
                    ++offset;
            };
            skip_space(cursor);
            if (cursor == bytes.size() || bytes[cursor++] != ':')
                continue;
            skip_space(cursor);
            std::size_t value_depth = 0;
            std::size_t count = 0;
            bool quoted = false;
            bool escaped = false;
            for (; cursor < bytes.size(); ++cursor) {
                const char ch = bytes[cursor];
                if (!quoted && value_depth == 0 && (ch == ',' || ch == '}' || ch == ']'))
                    break;
                if (quoted || std::isspace(static_cast<unsigned char>(ch)) == 0) {
                    if (++count > SavedInputLimits::kPlantComponentBytes)
                        return false;
                }
                if (quoted) {
                    if (escaped)
                        escaped = false;
                    else if (ch == '\\')
                        escaped = true;
                    else if (ch == '"')
                        quoted = false;
                } else if (ch == '"') {
                    quoted = true;
                } else if (ch == '{' || ch == '[') {
                    ++value_depth;
                } else if (ch == '}' || ch == ']') {
                    if (value_depth == 0)
                        return false;
                    --value_depth;
                }
            }
            if (count > SavedInputLimits::kPlantComponentAggregateBytes - aggregate)
                return false;
            aggregate += count;
        }
    }
    return true;
}
} // namespace Luminumbra::Persistence
