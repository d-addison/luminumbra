#pragma once

#include "SavedInputFile.h"
#include "SavedInputLimits.h"
#include <array>
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

// Iterative traversal bounds stack use before the JSON library recursively
// serializes or copies a value. The fixed buffer holds at most 64 iterator ranges.
inline bool AdmitJsonValueDepth(const nlohmann::json& value,
                                std::size_t maximum = SavedInputLimits::kJsonNestingDepth,
                                std::size_t maximum_nodes = SavedInputLimits::kPlantFileBytes) {
    if (maximum_nodes == 0 || maximum > SavedInputLimits::kJsonNestingDepth || value.is_discarded())
        return false;
    // Binary JSON encodes an object containing a bytes array and a subtype.
    // Count those two container levels and all encoded values without copying.
    if (value.is_binary())
        return maximum >= 2 && maximum_nodes >= 3 && value.get_binary().size() <= maximum_nodes - 3;
    if (!value.is_structured())
        return true;
    if (maximum == 0)
        return false;
    using Range = std::pair<nlohmann::json::const_iterator, nlohmann::json::const_iterator>;
    std::array<Range, SavedInputLimits::kJsonNestingDepth> stack;
    std::size_t depth = 1;
    std::size_t nodes = 1;
    stack[0] = {value.begin(), value.end()};
    while (depth != 0) {
        auto& [current, end] = stack[depth - 1];
        if (current == end) {
            --depth;
            continue;
        }
        // Every JSON value occupies at least one encoded byte, so this also
        // bounds pre-serialization work by the caller's output byte budget.
        if (nodes == maximum_nodes)
            return false;
        ++nodes;
        const auto& child = *current++;
        if (child.is_discarded())
            return false;
        if (child.is_binary()) {
            if (maximum - depth < 2 || maximum_nodes - nodes < 2 ||
                child.get_binary().size() > maximum_nodes - nodes - 2)
                return false;
            nodes += child.get_binary().size() + 2;
            continue;
        }
        if (!child.is_structured())
            continue;
        if (depth == maximum)
            return false;
        stack[depth++] = {child.begin(), child.end()};
    }
    return true;
}

inline bool BoundedJsonDump(const nlohmann::json& value,
                            std::size_t maximum,
                            std::string& bytes,
                            int indent = -1,
                            bool trailing_newline = true) {
    if (!AdmitJsonValueDepth(value, SavedInputLimits::kJsonNestingDepth, maximum))
        return false;
    LimitedJsonBuffer buffer(maximum);
    std::ostream stream(&buffer);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        if (indent >= 0)
            stream << std::setw(indent);
        stream << value;
        if (indent >= 0 && trailing_newline)
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
    if (!SavedInputLimits::JsonDepthAllowed(bytes))
        return false;
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

enum class SavedJsonFailure {
    None,
    Unreadable,
    Limit,
    Malformed,
    Cancelled
};

inline bool ReadSavedJson(const std::filesystem::path& path,
                          std::size_t maximum,
                          nlohmann::json& value,
                          bool& missing,
                          bool metadata_input = true,
                          SavedJsonFailure* failure = nullptr,
                          const std::stop_token& stop = {}) {
    if (failure)
        *failure = SavedJsonFailure::None;
    missing = false;
    const auto cancelled = [&] {
        if (!stop.stop_requested())
            return false;
        if (failure)
            *failure = SavedJsonFailure::Cancelled;
        return true;
    };
    if (cancelled())
        return false;
    const SavedInputFile input(path);
    if (cancelled())
        return false;
    missing = input.missing();
    if (missing)
        return true;
    std::string bytes;
    std::uint64_t length = 0;
    if (!input.length(length)) {
        if (failure)
            *failure = SavedJsonFailure::Unreadable;
        return false;
    }
    if (length > maximum) {
        if (failure)
            *failure = SavedJsonFailure::Limit;
        return false;
    }
    if (!input.read(bytes, maximum, stop)) {
        if (cancelled())
            return false;
        if (failure)
            *failure = SavedJsonFailure::Unreadable;
        return false;
    }
    if (!SavedInputLimits::JsonDepthAllowed(bytes) ||
        (metadata_input && !AdmitMetadataText(bytes))) {
        if (failure)
            *failure = SavedJsonFailure::Limit;
        return false;
    }
    auto candidate = nlohmann::json::parse(bytes, nullptr, false);
    if (candidate.is_discarded()) {
        if (failure)
            *failure = SavedJsonFailure::Malformed;
        return false;
    }
    if (cancelled())
        return false;
    value = std::move(candidate);
    return true;
}

// Lexical preflight before DOM allocation. Only component data values at the
// snapshot's component depth are counted. Whitespace outside strings is free,
// matching the compact writer encoding; escaped quotes never terminate strings.
// Each byte is visited at most three times, bounded by kPlantFileBytes. JSON syntax
// and the plant numeric domains are still checked by the existing parsers.
inline bool AdmitPlantComponentBytes(std::string_view bytes) {
    if (bytes.size() > SavedInputLimits::kPlantFileBytes ||
        !SavedInputLimits::JsonDepthAllowed(bytes))
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
