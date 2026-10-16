#pragma once

#include "luminumbra_common/core/CaptureContentPaths.h"
#include "luminumbra_common/core/SystemConfig.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Luminumbra::Client::Measurement {

// Stream bytes without allocating in proportion to asset size. Binary reads
// preserve CRLF and every other byte identically on Windows and POSIX.
inline std::uintmax_t append_content_digest(std::istream& input, std::uint64_t& hash) {
    std::array<char, 64 * 1024> buffer{}; // At most 64 KiB of asset bytes in memory.
    std::uintmax_t bytes = 0;
    do {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::uintmax_t>(input.gcount());
        if (count > std::numeric_limits<std::uintmax_t>::max() - bytes)
            throw std::runtime_error("workload content size overflow");
        bytes += count;
        for (std::streamsize index = 0; index < input.gcount(); ++index)
            hash = (hash ^ static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)])) *
                   1099511628211ull;
    } while (input);
    if (!input.eof() || input.bad())
        throw std::runtime_error("cannot read workload content");
    return bytes;
}

inline std::string content_digest_hex(std::uint64_t hash) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "fnv1a64:";
    for (int shift = 60; shift >= 0; shift -= 4)
        result += digits[(hash >> shift) & 15u];
    return result;
}

inline std::string content_digest(std::istream& input) {
    std::uint64_t hash = 14695981039346656037ull;
    append_content_digest(input, hash);
    return content_digest_hex(hash);
}

inline std::string content_digest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open workload content");
    return content_digest(input);
}

// Bind every saved file's sorted UTF-8 relative path and exact binary contents.
// Length prefixes distinguish path/file boundaries without buffering save bytes.
inline std::string saved_world_digest(const std::filesystem::path& directory) {
    if (std::filesystem::is_symlink(directory) || !std::filesystem::is_directory(directory))
        throw std::runtime_error("unsupported workload save directory");
    const auto canonical_directory = std::filesystem::canonical(directory);
    std::vector<std::string> paths;
    std::size_t entries = 0;
    // At most 65536 entries, 4096 bytes per relative path and 64 directory levels.
    for (auto iterator = std::filesystem::recursive_directory_iterator(directory);
         iterator != std::filesystem::recursive_directory_iterator();
         ++iterator) {
        if (entries == 65536 || iterator.depth() > 64)
            throw std::runtime_error("workload save inventory exceeds bounds");
        ++entries;
        const auto& entry = *iterator;
        const auto relative = entry.path().lexically_relative(directory).generic_u8string();
        if (relative.size() > 4096 || entry.is_symlink() ||
            std::filesystem::canonical(entry.path()) !=
                canonical_directory / std::filesystem::path(relative))
            throw std::runtime_error("unsupported workload save path");
        if (entry.is_regular_file())
            paths.emplace_back(reinterpret_cast<const char*>(relative.data()), relative.size());
        else if (!entry.is_directory())
            throw std::runtime_error("unsupported workload save file");
    }
    std::sort(paths.begin(), paths.end());
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto& relative : paths) {
        const auto path = directory / Filesystem::PathFromUtf8(relative);
        const auto size = std::filesystem::file_size(path);
        std::istringstream header(std::to_string(relative.size()) + ":" + relative +
                                  std::to_string(size) + ":");
        append_content_digest(header, hash);
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open workload save");
        if (append_content_digest(input, hash) != size || std::filesystem::file_size(path) != size)
            throw std::runtime_error("workload save changed while reading");
    }
    return content_digest_hex(hash);
}

inline void require_saved_world_digest(const std::filesystem::path& directory,
                                       std::string_view expected) {
    if (expected.empty() || saved_world_digest(directory) != expected)
        throw std::runtime_error("workload saved authority changed before reload");
}

// Conservatively include the data, resource, config and installed asset trees:
// scatter archetypes can reference models/textures (including asset packs),
// presets can reference tuning, and UI documents can reference styles/fonts.
// Sorting by portable relative path
// makes enumeration order irrelevant. Only used outside measured frame intervals.
inline nlohmann::json workload_content(const std::filesystem::path& root) {
    Filesystem::CaptureContentPaths containment;
    containment.enable(root);
    nlohmann::json result = nlohmann::json::object();
    // Bound manifest storage to 65536 entries with 4096-byte relative paths;
    // also bound directory depth to 64, independently of filesystem traversal order.
    std::size_t entries = 0;
    for (const char* tree : Filesystem::kCaptureContentTrees) {
        if (std::filesystem::is_symlink(root / tree) || !std::filesystem::is_directory(root / tree))
            throw std::runtime_error("missing workload content tree");
        for (auto iterator = std::filesystem::recursive_directory_iterator(root / tree);
             iterator != std::filesystem::recursive_directory_iterator();
             ++iterator) {
            if (entries == 65536 || iterator.depth() > 64)
                throw std::runtime_error("workload content manifest exceeds bounds");
            ++entries;
            const auto& entry = *iterator;
            const auto relative = entry.path().lexically_relative(root).generic_u8string();
            if (relative.size() > 4096 || entry.is_symlink() || !containment.allows(entry.path()))
                throw std::runtime_error("unsupported workload content path");
            const std::string path(reinterpret_cast<const char*>(relative.data()), relative.size());
            if (path.find_first_of("\\:") != std::string::npos)
                throw std::runtime_error("unsupported workload content path");
            if (entry.is_regular_file())
                result[path] = content_digest(entry.path());
            else if (!entry.is_directory())
                throw std::runtime_error("unsupported workload content file");
        }
    }
    return result;
}

// Enumerate the same generated registry that defines the runtime queries.
// Parameters retain their resolved configured/default values even when their
// owning flag is disabled, so captures conservatively retain all config inputs.
inline nlohmann::json render_systems(const luminumbra::core::SystemConfig& config) {
    using luminumbra::core::SysKey;
    using luminumbra::core::SysParam;
    nlohmann::json result = nlohmann::json::object();
    std::array<const char*, static_cast<std::size_t>(SysKey::Count)> names{};
#define LUMIN_RENDER_KEY(ENUM, SECTION, JSON_SECTION, JSON_NAME, RESIDENCY)                        \
    if (std::string_view(JSON_SECTION) == "render") {                                              \
        names[static_cast<std::size_t>(SysKey::ENUM)] = JSON_NAME;                                 \
        result[JSON_NAME] = {{"enabled", config.enabled(SysKey::ENUM)},                            \
                             {"params", nlohmann::json::object()}};                                \
    }
    LUMIN_CONFIG_KEY_TABLE(LUMIN_RENDER_KEY)
#undef LUMIN_RENDER_KEY
#define LUMIN_RENDER_PARAM(ENUM, OWNER, JSON_NAME, IS_VEC3, SCALAR, VX, VY, VZ)                    \
    if (const auto* owner = names[static_cast<std::size_t>(SysKey::OWNER)]) {                      \
        if constexpr (IS_VEC3) {                                                                   \
            const auto value = config.param3(SysParam::ENUM, glm::vec3(VX, VY, VZ));               \
            result[owner]["params"][JSON_NAME] = {value.x, value.y, value.z};                      \
        } else {                                                                                   \
            result[owner]["params"][JSON_NAME] = config.param(SysParam::ENUM, SCALAR);             \
        }                                                                                          \
    }
    LUMIN_CONFIG_PARAM_TABLE(LUMIN_RENDER_PARAM)
#undef LUMIN_RENDER_PARAM
    return result;
}

// FNV-1a over a typed canonical stream, independent of JSON whitespace, locale,
// floating-point formatting, object insertion order and host endianness.
// Mirrors render_contract.render_identity. Called only at report boundaries.
inline std::string render_identity(const nlohmann::json& settings) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    std::uint64_t hash = 14695981039346656037ull;
    const auto byte = [&](unsigned char value) {
        hash = (hash ^ value) * 1099511628211ull;
    };
    const auto text = [&](std::string_view value) {
        for (const unsigned char item : value)
            byte(item);
    };
    const auto visit = [&](const auto& self, const nlohmann::json& value) -> void {
        if (value.is_object()) {
            byte('{');
            for (const auto& [key, item] : value.items()) {
                self(self, nlohmann::json(key));
                self(self, item);
            }
            byte('}');
        } else if (value.is_array()) {
            byte('[');
            for (const auto& item : value)
                self(self, item);
            byte(']');
        } else if (value.is_string()) {
            const auto& string = value.get_ref<const std::string&>();
            byte('s');
            text(std::to_string(string.size()));
            byte(':');
            text(string);
        } else if (value.is_boolean()) {
            byte(value.get<bool>() ? 't' : 'f');
        } else if (value.is_number_float()) {
            const double number = value.get<double>();
            if (!std::isfinite(number))
                throw std::invalid_argument("non-finite render configuration");
            byte('d');
            const auto bits = std::bit_cast<std::uint64_t>(number);
            for (unsigned shift = 0; shift < 64; shift += 8)
                byte(static_cast<unsigned char>(bits >> shift));
        } else if (value.is_number_integer()) {
            byte('i');
            text(value.dump());
            byte(';');
        } else if (value.is_null()) {
            byte('n');
        } else {
            throw std::invalid_argument("unsupported render configuration value");
        }
    };
    visit(visit, settings);
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "fnv1a64:";
    for (int shift = 60; shift >= 0; shift -= 4)
        result += digits[(hash >> shift) & 15u];
    return result;
}

} // namespace Luminumbra::Client::Measurement
